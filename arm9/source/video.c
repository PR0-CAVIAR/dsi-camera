#include "video.h"

#include "avi.h"

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

// Recording uses the preview stream: 256x192 RGB15 (the camera interface does
// the YUV->RGB conversion), 96 KiB per frame. The 640x480 YUV422 capture mode is
// 600 KiB per frame, so the SD card would cap it at a few fps.
#define FRAME_WIDTH 256
#define FRAME_HEIGHT 192
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_SIZE (FRAME_PIXELS * sizeof(u16))
#define SLOT_SIZE (AVI_FRAME_PREFIX + FRAME_SIZE) // the prefix holds the frame's AVI chunk headers

#define RING_FRAMES_MAX 32 // ~3 MiB, rides out SD card stalls
#define RING_FRAMES_MIN 4
#define WRITE_FRAMES_MAX 4 // frames per write() call, ~386 KiB

// The AVI runs at a fixed rate and camera frames are placed by timestamp,
// repeating the previous frame in between, so audio stays in sync even when
// the camera's frame rate changes with the light
#define TIMELINE_FPS 30

#define MIC_RATE_32K 16364      // MicRate_Div2 with the DSi sound clock at 32.73 kHz
#define MIC_RATE_47K 23803      // ...and at 47.61 kHz, if the launcher left it there
#define MIC_BUFFER_SIZE 4096    // bytes per half of the double buffer, ~125 ms
#define AUDIO_RING_SIZE 0x40000 // 256 KiB, must be a power of two
#define AUDIO_WRITE_MIN 0x2000
#define MIC_SILENT_MS 2000

// Calico's ARM7 re-arms the mic DMA from an IRQ after every buffer. If that's
// late the mic's FIFO (~2 ms) overruns and the mic stops for good, so restart
// it when buffers stop coming and fill the gap with silence to stay in sync.
#define MIC_STALL_MS 400

#define FREE_SPACE_MARGIN (16u * 1024 * 1024)
#define SYNC_INTERVAL_MS 5000 // so a dead battery doesn't lose the whole clip
#define STATUS_INTERVAL_MS 500
#define STALL_MS 2000

typedef struct {
	Camera camera;
	AviFile avi;
	bool open;
	bool audio;       // mic is running
	u64 startTick;    // when recording (and the mic) started
	u64 partTick;     // timeline zero of the current file
	u32 nextSlot;     // timeline frames in the current file
	bool firstFrame;  // next frame is the first of the current file
	u32 part;
	u64 bytesWritten; // all parts
	u64 writeTicks;   // time spent inside write()
	u32 audioPeak;    // loudest sample since the last status update
	u32 micRestarts;
	u32 micBuffersSeen; // micBuffers at micBufferTick
	u64 micBufferTick;  // when a mic buffer last arrived (or the mic was started)
	char path[32];
	const char *notice;
	const char *error;
	u64 statusTick;   // when the live fps window started
	u32 statusFrames; // camera frames at statusTick
} Recording;

static u8 *ring;
static u32 ringFrames;
static vu32 framesCaptured; // advanced by the NDMA IRQ
static vu32 framesDropped;  // advanced by the NDMA IRQ when the ring is full
static vu32 framesWritten;  // advanced by the main loop
static volatile bool capturing;
static volatile u64 frameTicks[RING_FRAMES_MAX]; // when each slot's frame finished
static u64 lastFrameIrqTick;
static vu32 intervalMin, intervalMax; // camera frame interval in ticks, reset with each status update

static u8 micBuffer[MIC_BUFFER_SIZE * 2] __attribute__((aligned(32)));
static u8 audioRing[AUDIO_RING_SIZE] __attribute__((aligned(32)));
static vu32 audioHead; // bytes received, advanced by the mic thread
static vu32 audioTail; // bytes written, advanced by the main loop
static vu32 audioDropped;
static volatile bool micAccepting;
static bool micInited;
static vu32 micBuffers;          // buffers received, for stall detection
static u64 micLastTick;          // mic thread only
static vu32 micIntervalSum;      // ticks between consecutive buffers...
static vu32 micIntervalCount;    // ...and how many, to measure the real rate

static s64 freeSpace = -2; // -2 = not checked yet, -1 = unknown

static u32 ticksToMs(u64 ticks) { return ticks * 1000 / TICK_FREQ; }

static u32 min3(u32 a, u32 b, u32 c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }

static u8 *slotPtr(u32 slot) { return ring + slot * SLOT_SIZE; }

static u16 *slotFrame(u32 slot) { return (u16 *)(slotPtr(slot) + AVI_FRAME_PREFIX); }

// Runs when NDMA1 finishes a frame: queue it and start the next one right away
static void ndmaHandler(void) {
	if(!capturing)
		return;

	// Uneven intervals (some ~2x the others) would mean sensor frames get skipped
	u64 now = tickGetCount();
	if(lastFrameIrqTick) {
		u32 interval = now - lastFrameIrqTick;
		if(interval < intervalMin)
			intervalMin = interval;
		if(interval > intervalMax)
			intervalMax = interval;
	}
	lastFrameIrqTick = now;

	if(framesCaptured - framesWritten < ringFrames - 1) {
		frameTicks[framesCaptured % ringFrames] = now;
		framesCaptured++;
	} else {
		framesDropped++; // writer is behind, overwrite this slot
	}

	cameraTransferStart(slotFrame(framesCaptured % ringFrames), CAPTURE_MODE_PREVIEW);
}

// Runs on calico's mic thread whenever half of the double buffer is full
static void micHandler(void *user, void *buf, size_t size) {
	if(!micAccepting)
		return;

	// Time back-to-back buffers to find the real sample rate. Gaps from mic
	// restarts are much longer and get skipped.
	u64 now = tickGetCount();
	if(micLastTick && now - micLastTick < TICK_FREQ / 4 && micIntervalCount < 1000) {
		micIntervalSum += now - micLastTick;
		micIntervalCount++;
	}
	micLastTick = now;
	micBuffers++;

	if(audioHead - audioTail + size > AUDIO_RING_SIZE) {
		audioDropped += size;
		return;
	}

	DC_InvalidateRange(buf, size);
	u32 offset = audioHead & (AUDIO_RING_SIZE - 1);
	u32 first  = size < AUDIO_RING_SIZE - offset ? size : AUDIO_RING_SIZE - offset;
	memcpy(audioRing + offset, buf, first);
	memcpy(audioRing, (u8 *)buf + first, size - first);
	audioHead += size;
}

static bool startMic(void) {
	if(!micInited) {
		micInit();
		micInited = true;
	}

	if(!micSetDmaRate(MicRate_Div2))
		return false;
	micSetCallback(micHandler, NULL);

	audioHead = audioTail = audioDropped = 0;
	micBuffers = micIntervalSum = micIntervalCount = 0;
	micLastTick = 0;
	DC_FlushRange(micBuffer, sizeof(micBuffer));
	pmMicSetAmp(true, PmMicGain_40);
	micAccepting = true;
	if(!micStart(micBuffer, MIC_BUFFER_SIZE, MicFmt_Pcm16, MicMode_DoubleBuffer)) {
		micAccepting = false;
		pmMicSetAmp(false, 0);
		return false;
	}
	return true;
}

static void stopMic(void) {
	micAccepting = false;
	micStop();
	pmMicSetAmp(false, 0);
	// Let the mic thread drain buffers that were already on their way
	swiWaitForVBlank();
}

// The mic's real rate depends on the DSi sound clock, so time the buffers
static u32 micRate(void) {
	u32 count = micIntervalCount, sum = micIntervalSum;
	if(count < 4 || sum == 0)
		return MIC_RATE_32K;

	u32 measured = (u64)(MIC_BUFFER_SIZE / 2) * count * TICK_FREQ / sum;
	if(measured > MIC_RATE_32K * 85 / 100 && measured < MIC_RATE_32K * 115 / 100)
		return MIC_RATE_32K;
	if(measured > MIC_RATE_47K * 85 / 100 && measured < MIC_RATE_47K * 115 / 100)
		return MIC_RATE_47K;
	return measured;
}

// Fills the audio queue with silence up to where it should be at `tick`.
// Only call this while the mic is stopped.
static void padAudio(const Recording *rec, u64 tick) {
	u32 expected = ((tick - rec->startTick) * micRate() / TICK_FREQ) * 2;
	if(expected <= audioHead)
		return;

	u32 pad   = expected - audioHead;
	u32 space = AUDIO_RING_SIZE - (audioHead - audioTail);
	if(pad > space) {
		audioDropped += pad - space;
		pad = space;
	}

	u32 offset = audioHead & (AUDIO_RING_SIZE - 1);
	u32 first  = pad < AUDIO_RING_SIZE - offset ? pad : AUDIO_RING_SIZE - offset;
	memset(audioRing + offset, 0, first);
	memset(audioRing, 0, pad - first);
	audioHead += pad;
}

static void restartMic(Recording *rec) {
	stopMic();
	padAudio(rec, tickGetCount());

	micLastTick = 0;
	DC_FlushRange(micBuffer, sizeof(micBuffer));
	pmMicSetAmp(true, PmMicGain_40);
	micAccepting = true;
	if(!micStart(micBuffer, MIC_BUFFER_SIZE, MicFmt_Pcm16, MicMode_DoubleBuffer))
		micAccepting = false;
	rec->micRestarts++;
	rec->micBuffersSeen = micBuffers;
	rec->micBufferTick  = tickGetCount();
}

static int getVideoNumber() {
	int highest = -1;

	DIR *pdir = opendir(VIDEO_DIR);
	if(pdir == NULL)
		return -1;

	while(true) {
		struct dirent *pent = readdir(pdir);
		if(pent == NULL)
			break;

		if(strncmp(pent->d_name, "REC_", 4) == 0) {
			int val = atoi(pent->d_name + 4);
			if(val > highest)
				highest = val;
		}
	}
	closedir(pdir);

	return highest + 1;
}

static s64 getFreeSpace() {
	struct statvfs st;
	if(statvfs(VIDEO_DIR, &st) != 0)
		return -1;
	return (s64)st.f_bavail * (st.f_frsize ? st.f_frsize : st.f_bsize);
}

static bool openPart(Recording *rec) {
	int number = getVideoNumber();
	if(number < 0) {
		rec->error = "Can't open " VIDEO_DIR;
		return false;
	}

	sprintf(rec->path, VIDEO_DIR "/REC_%04d.AVI", number);
	if(!aviOpen(&rec->avi, rec->path, FRAME_WIDTH, FRAME_HEIGHT, TIMELINE_FPS, rec->audio ? micRate() : 0)) {
		rec->error = "Can't create video file";
		return false;
	}

	// The file's timeline starts with the first audio sample that goes into it
	if(rec->audio)
		rec->partTick = rec->startTick + (u64)(audioTail / 2) * TICK_FREQ / micRate();
	else
		rec->partTick = tickGetCount();
	rec->nextSlot   = 0;
	rec->firstFrame = true;
	rec->open       = true;
	return true;
}

static bool closePart(Recording *rec) {
	if(!rec->open)
		return true;
	rec->open = false;
	return aviClose(&rec->avi, rec->audio ? micRate() : 0);
}

// Starts a new file before the AVI size limit and checks for free space
static bool ensureSpace(Recording *rec, u32 bytes) {
	if(aviFinalSize(&rec->avi) + bytes + 0x10000 > AVI_SIZE_LIMIT) {
		if(!closePart(rec)) {
			rec->error = "Can't close video file";
			return false;
		}
		rec->part++;
		if(!openPart(rec))
			return false;
	}

	if(freeSpace >= 0 && (s64)(rec->bytesWritten + bytes + FREE_SPACE_MARGIN) > freeSpace) {
		rec->error = "SD card is full";
		return false;
	}
	return true;
}

static void setWriteError(Recording *rec) { rec->error = errno == ENOSPC ? "SD card is full" : "SD write failed"; }

// Writes one chunk of queued audio, or all of it when flushing
static bool writeAudio(Recording *rec, bool flush) {
	while(true) {
		u32 pending = audioHead - audioTail;
		if(pending == 0 || (!flush && pending < AUDIO_WRITE_MIN))
			return true;

		u32 offset = audioTail & (AUDIO_RING_SIZE - 1);
		u32 size   = min3(pending, AUDIO_RING_SIZE - offset, AVI_MAX_AUDIO) & ~1u;
		if(!ensureSpace(rec, size + 1024))
			return false;

		const s16 *samples = (const s16 *)(audioRing + offset);
		for(u32 i = 0; i < size / 2; i++) {
			u32 level = samples[i] < 0 ? -samples[i] : samples[i];
			if(level > rec->audioPeak)
				rec->audioPeak = level;
		}

		u64 start = tickGetCount();
		bool ok   = aviWriteAudio(&rec->avi, samples, size);
		rec->writeTicks += tickGetCount() - start;
		if(!ok) {
			setWriteError(rec);
			return false;
		}

		rec->bytesWritten += size;
		audioTail += size;
		if(!flush)
			return true;
	}
}

// Timeline position for a frame, as the number of repeats to put before it
static u32 frameRepeats(Recording *rec, u64 tick) {
	// The first frame of a file covers the start of the timeline
	if(rec->firstFrame) {
		rec->firstFrame = false;
		rec->nextSlot   = 1;
		return 0;
	}

	u64 delta    = tick > rec->partTick ? tick - rec->partTick : 0;
	u32 slot     = (delta * TIMELINE_FPS + TICK_FREQ / 2) / TICK_FREQ;
	u32 repeats  = slot > rec->nextSlot ? slot - rec->nextSlot : 0;
	if(repeats > AVI_MAX_REPEATS)
		repeats = AVI_MAX_REPEATS;
	rec->nextSlot += repeats + 1;
	return repeats;
}

// Converts and writes up to WRITE_FRAMES_MAX queued frames with a single write()
static bool writeFrames(Recording *rec, u32 pending) {
	u32 slot  = framesWritten % ringFrames;
	u32 count = min3(pending, ringFrames - slot, WRITE_FRAMES_MAX);
	u32 size  = count * SLOT_SIZE;
	if(!ensureSpace(rec, size))
		return false;

	for(u32 i = 0; i < count; i++) {
		// The camera DMA wrote these behind the cache's back
		u16 *frame = slotFrame(slot + i);
		DC_InvalidateRange(frame, FRAME_SIZE);
		aviConvertFrame(frame, FRAME_WIDTH, FRAME_HEIGHT);
		if(!aviPrepareFrame(&rec->avi, slotPtr(slot + i), frameRepeats(rec, frameTicks[slot + i]))) {
			rec->error = "Out of memory";
			return false;
		}
	}
	DC_FlushRange(slotPtr(slot), size);

	u64 start = tickGetCount();
	bool ok   = aviWriteFrames(&rec->avi, slotPtr(slot), count);
	rec->writeTicks += tickGetCount() - start;
	if(!ok) {
		setWriteError(rec);
		return false;
	}

	rec->bytesWritten += size;
	framesWritten += count;
	return true;
}

static void drawRecDot(u16 *display) {
	for(int y = -5; y <= 5; y++) {
		for(int x = -5; x <= 5; x++) {
			if(x * x + y * y <= 25)
				display[(12 + y) * FRAME_WIDTH + 12 + x] = RGB15(31, 0, 0) | BIT(15);
		}
	}
}

static void drawStatus(Recording *rec, bool stalled) {
	u64 now      = tickGetCount();
	u32 ms       = ticksToMs(now - rec->startTick);
	u32 captured = framesCaptured, dropped = framesDropped, written = framesWritten;
	u32 saved10  = ms ? (u64)captured * 10000 / ms : 0;             // fps * 10
	u32 camera10 = ms ? (u64)(captured + dropped) * 10000 / ms : 0; // fps * 10
	u32 writeMs  = ticksToMs(rec->writeTicks);
	u32 kbps     = writeMs ? rec->bytesWritten * 1000 / 1024 / writeMs : 0;
	u32 busy     = ms ? (u64)writeMs * 100 / ms : 0;

	// Camera fps over the last status interval, and the frame interval range in it
	u32 windowMs      = ticksToMs(now - rec->statusTick);
	u32 live10        = windowMs ? (u64)(captured + dropped - rec->statusFrames) * 10000 / windowMs : 0;
	u32 min10         = intervalMin == UINT32_MAX ? 0 : (u64)intervalMin * 10000 / TICK_FREQ; // ms * 10
	u32 max10         = (u64)intervalMax * 10000 / TICK_FREQ;
	intervalMin       = UINT32_MAX;
	intervalMax       = 0;
	rec->statusTick   = now;
	rec->statusFrames = captured + dropped;

	printf("\x1b[0;0H\x1b[31;1m* REC *\x1b[39m  %s\x1b[K", rec->path + sizeof(VIDEO_DIR));
	printf("\x1b[2;0HTime    %02lu:%02lu.%lu\x1b[K", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
	printf("\x1b[3;0HFrames  %lu saved\x1b[K", written);
	printf("\x1b[4;0HDropped %s%lu\x1b[39m\x1b[K", dropped ? "\x1b[33;1m" : "", dropped);
	printf("\x1b[5;0HFPS     %lu.%lu saved, %lu.%lu cam\x1b[K", saved10 / 10, saved10 % 10, camera10 / 10,
	       camera10 % 10);
	printf("\x1b[6;0HLive    %lu.%lu fps, %lu.%lu-%lu.%lu ms\x1b[K", live10 / 10, live10 % 10, min10 / 10, min10 % 10,
	       max10 / 10, max10 % 10);

	if(!rec->audio) {
		printf("\x1b[7;0HAudio   \x1b[33;1moff (mic failed)\x1b[39m\x1b[K");
	} else if(micBuffers == 0 && ms >= MIC_SILENT_MS) {
		printf("\x1b[7;0HAudio   \x1b[33;1mno data from mic!\x1b[39m\x1b[K");
	} else {
		// Rough level meter: one bar per doubling of the peak
		char meter[9] = "        ";
		for(int i = 0; i < 8; i++) {
			if(rec->audioPeak >= (256u << i))
				meter[i] = '#';
		}
		u32 rate = micRate();
		printf("\x1b[7;0HAudio   %lu.%lu kHz [%s]", rate / 1000, rate / 100 % 10, meter);
		if(rec->micRestarts)
			printf(" \x1b[33;1mr%lu\x1b[39m", rec->micRestarts);
		printf("\x1b[K");
	}
	rec->audioPeak = 0;

	printf("\x1b[8;0HSD      %lu KB/s, busy %lu%%\x1b[K", kbps, busy);
	printf("\x1b[9;0HBuffer  %lu/%lu frames\x1b[K", captured - written, ringFrames - 1);
	printf("\x1b[10;0HFile    %lu MB, part %lu\x1b[K", aviFinalSize(&rec->avi) >> 20, rec->part + 1);
	if(freeSpace >= 0) {
		u32 freeMB  = (u32)((freeSpace - (s64)rec->bytesWritten) >> 20);
		u32 bytesPS = (u64)saved10 * SLOT_SIZE / 10;
		printf("\x1b[11;0HFree    %lu MB", freeMB);
		if(bytesPS)
			printf(" (~%lu min)", (u32)((freeSpace - (s64)rec->bytesWritten) / bytesPS / 60));
		printf("\x1b[K");
	} else {
		printf("\x1b[11;0HFree    unknown\x1b[K");
	}

	printf("\x1b[13;0H\x1b[33;1m%s\x1b[39m\x1b[K", stalled ? "No frames from camera!" : "");
	printf("\x1b[14;0H\x1b[33;1m%s\x1b[39m\x1b[K", rec->notice ? rec->notice : "");
	printf("\x1b[16;0HSELECT: stop  START: stop+exit\x1b[K");
}

VideoResult videoRecord(Camera camera, u16 *display) {
	Recording rec      = {.camera = camera};
	VideoResult result = VIDEO_STOPPED;

	consoleClear();
	printf("Starting recording...\n");

	if(freeSpace == -2) {
		printf("Checking free space... ");
		freeSpace = getFreeSpace();
		if(freeSpace >= 0)
			printf("%lu MB\n", (u32)(freeSpace >> 20));
		else
			printf("unknown\n");
	}

	if(freeSpace >= 0 && freeSpace < FREE_SPACE_MARGIN + WRITE_FRAMES_MAX * SLOT_SIZE) {
		printf("\x1b[31;1mSD card is full\x1b[39m\n\n");
		return VIDEO_STOPPED;
	}

	for(ringFrames = RING_FRAMES_MAX; ringFrames >= RING_FRAMES_MIN; ringFrames /= 2) {
		ring = (u8 *)memalign(32, ringFrames * SLOT_SIZE);
		if(ring)
			break;
	}
	if(!ring) {
		printf("\x1b[31;1mOut of memory\x1b[39m\n\n");
		return VIDEO_STOPPED;
	}

	// Wait for the preview transfer to VRAM to finish
	while(cameraTransferActive())
		swiWaitForVBlank();
	cameraTransferStop();

	// Make sure no dirty cache lines get written back over DMA'd frames
	DC_FlushRange(ring, ringFrames * SLOT_SIZE);

	// Calico only starts the tick timer lazily (from threadSleepTicks), without
	// this tickGetCount() reads a stopped timer. It's a no-op if already running.
	tickInit();

	framesCaptured = framesDropped = framesWritten = 0;
	rec.audio      = startMic();
	rec.startTick  = tickGetCount();
	if(!openPart(&rec)) {
		if(rec.audio)
			stopMic();
		free(ring);
		ring = NULL;
		printf("\x1b[31;1m%s\x1b[39m\n\n", rec.error);
		return VIDEO_STOPPED;
	}

	// Give the ARM7 main RAM priority while recording. Otherwise converting and
	// flushing frames here can hold off its mic DMA long enough to overrun the
	// mic's FIFO.
	u16 exmemPrio = REG_EXMEMCNT & EXMEMCNT_MAIN_RAM_PRIO_ARM7;
	REG_EXMEMCNT |= EXMEMCNT_MAIN_RAM_PRIO_ARM7;
	rec.micBufferTick = rec.startTick;

	consoleClear();
	rec.statusTick   = rec.startTick;
	lastFrameIrqTick = 0;
	intervalMin      = UINT32_MAX;
	intervalMax      = 0;
	capturing        = true;
	irqSet(IRQ_NDMA1, ndmaHandler);
	irqEnable(IRQ_NDMA1);
	cameraTransferSetIrq(true);
	cameraTransferStart(slotFrame(0), CAPTURE_MODE_PREVIEW);

	u64 lastStatus = 0, lastSync = rec.startTick, lastFrameTick = rec.startTick;
	u32 lastCameraFrames = 0;
	while(true) {
		scanKeys();
		u16 pressed = keysDown();
		if(pressed & KEY_SELECT) {
			break;
		} else if(pressed & KEY_START) {
			result = VIDEO_EXIT;
			break;
		} else if(pressed & KEY_A) {
			rec.notice = "Can't swap camera while recording";
		} else if(pressed & (KEY_L | KEY_R)) {
			rec.notice = "Can't take photo while recording";
		}

		if(rec.audio && !writeAudio(&rec, false))
			break;

		u32 captured = framesCaptured;
		u32 pending  = captured - framesWritten;
		if(pending > 0) {
			// Show the newest frame, it can't be overwritten until it's written
			u16 *newest = slotFrame((captured - 1) % ringFrames);
			DC_InvalidateRange(newest, FRAME_SIZE);
			memcpy(display, newest, FRAME_SIZE);
			drawRecDot(display);

			if(!writeFrames(&rec, pending))
				break;
		} else {
			swiWaitForVBlank();
		}

		u64 now = tickGetCount();
		if(captured + framesDropped != lastCameraFrames) {
			lastCameraFrames = captured + framesDropped;
			lastFrameTick    = now;
		}

		if(rec.audio) {
			if(micBuffers != rec.micBuffersSeen) {
				rec.micBuffersSeen = micBuffers;
				rec.micBufferTick  = now;
			} else if(ticksToMs(now - rec.micBufferTick) >= MIC_STALL_MS) {
				restartMic(&rec);
			}
		}

		if(ticksToMs(now - lastSync) >= SYNC_INTERVAL_MS) {
			// Keep the header and directory entry current in case of power loss
			if(rec.audio)
				rec.avi.sampleRate = micRate();
			if(!aviSync(&rec.avi)) {
				rec.error = "SD sync failed";
				break;
			}
			lastSync = now;
		}

		if(ticksToMs(now - lastStatus) >= STATUS_INTERVAL_MS) {
			drawStatus(&rec, ticksToMs(now - lastFrameTick) >= STALL_MS);
			lastStatus = now;
		}
	}

	// Stop capturing, give the in-flight frame a moment to finish
	capturing    = false;
	u64 stopTick = tickGetCount();
	while(cameraTransferActive() && ticksToMs(tickGetCount() - stopTick) < 500)
		swiWaitForVBlank();
	cameraTransferStop();
	REG_NDMA1CNT = 0;
	cameraTransferSetIrq(false);
	irqDisable(IRQ_NDMA1);
	irqSet(IRQ_NDMA1, NULL);
	if(rec.audio) {
		stopMic();
		padAudio(&rec, stopTick); // in case the mic died near the end
	}
	REG_EXMEMCNT = (REG_EXMEMCNT & ~EXMEMCNT_MAIN_RAM_PRIO_ARM7) | exmemPrio;

	// Flush what's still queued
	if(rec.open && !rec.error) {
		printf("\x1b[13;0HSaving buffered frames...\x1b[K");
		while(framesWritten != framesCaptured) {
			if(!writeFrames(&rec, framesCaptured - framesWritten))
				break;
		}
		if(rec.audio && !rec.error)
			writeAudio(&rec, true);
	}

	bool closed = rec.open && closePart(&rec);
	free(ring);
	ring = NULL;
	if(freeSpace >= 0)
		freeSpace -= rec.bytesWritten + (rec.part + 1) * AVI_HEADER_SIZE;

	u32 ms    = ticksToMs(stopTick - rec.startTick);
	u32 fps10 = ms ? (u64)framesWritten * 10000 / ms : 0;
	consoleClear();
	if(rec.error)
		printf("\x1b[31;1mRecording stopped: %s\x1b[39m\n", rec.error);
	if(!closed)
		printf("\x1b[31;1mVideo file not closed cleanly\x1b[39m\n");
	printf("Saved %lu frames, %lu.%lus\n", framesWritten, ms / 1000, ms / 100 % 10);
	printf("%lu.%lu fps, %lu dropped\n", fps10 / 10, fps10 % 10, framesDropped);
	if(rec.audio)
		printf("Audio %lu Hz, %lu mic restarts\n", micRate(), rec.micRestarts);
	else
		printf("\x1b[33;1mNo audio (mic failed)\x1b[39m\n");
	printf("Last file:\n%s\n", rec.path);
	if(rec.part > 0)
		printf("(%lu parts)\n", rec.part + 1);
	printf("\n");

	return result;
}
