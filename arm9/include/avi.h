#ifndef AVI_H
#define AVI_H

// Minimal AVI writer: uncompressed 16-bit video plus optional 16-bit mono PCM.
// Plain C with no libnds dependencies so it can be tested on a PC.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVI_HEADER_SIZE 1024    // movi data starts here, keeps writes sector aligned
#define AVI_FRAME_PREFIX 512    // bytes the caller reserves before each frame
#define AVI_MAX_REPEATS 62      // repeated frames that fit in one prefix
#define AVI_MAX_AUDIO 0x10000   // max bytes per aviWriteAudio call
#define AVI_SIZE_LIMIT 0x40000000u // 1 GiB, the safe limit for idx1-only AVIs

typedef struct {
	uint32_t id, flags, offset, size;
} AviIndexEntry;

typedef struct {
	int fd;
	uint16_t width, height;
	uint32_t frameSize;
	uint32_t fps;         // timeline rate, including repeated frames
	uint32_t sampleRate;  // audio sample rate in Hz, 0 = no audio stream
	uint32_t pos;         // bytes written so far
	uint32_t pending;     // pos plus prepared but not yet written frames
	uint32_t videoChunks; // timeline frames, including repeats
	uint32_t realFrames;
	uint32_t audioBytes;
	AviIndexEntry *index;
	uint32_t indexCount, indexCap;
	uint8_t *scratch;
} AviFile;

// Creates the file and writes a placeholder header
bool aviOpen(AviFile *avi, const char *path, uint16_t width, uint16_t height, uint32_t fps, uint32_t sampleRate);

// Converts a top-down DS RGB15 frame (R in the low bits) in place to the
// bottom-up BGR555 layout AVI expects
void aviConvertFrame(uint16_t *frame, uint16_t width, uint16_t height);

// Fills the AVI_FRAME_PREFIX bytes in front of a frame with `repeats` empty
// chunks (play the previous frame again), padding and the frame's chunk header.
// Prepared frames must be written with aviWriteFrames before anything else.
bool aviPrepareFrame(AviFile *avi, uint8_t *prefix, uint32_t repeats);

// Writes `count` prepared frames laid out back to back as [prefix][frame]
bool aviWriteFrames(AviFile *avi, const void *data, uint32_t count);

// Writes a chunk of 16-bit PCM samples (padded to keep sector alignment)
bool aviWriteAudio(AviFile *avi, const void *samples, uint32_t bytes);

// Bytes the file would have after closing it now
uint32_t aviFinalSize(const AviFile *avi);

// Rewrites the header with the current lengths and syncs, so a file cut off
// by power loss is still readable
bool aviSync(AviFile *avi);

// Writes the index and final header, then closes the file. The sample rate
// can be corrected here if the measured one differs from the nominal one.
bool aviClose(AviFile *avi, uint32_t sampleRate);

#ifdef __cplusplus
}
#endif

#endif // AVI_H
