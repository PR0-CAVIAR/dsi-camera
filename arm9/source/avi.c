#include "avi.h"

#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ID_RIFF FOURCC('R', 'I', 'F', 'F')
#define ID_LIST FOURCC('L', 'I', 'S', 'T')
#define ID_JUNK FOURCC('J', 'U', 'N', 'K')
#define ID_VIDEO FOURCC('0', '0', 'd', 'c')
#define ID_AUDIO FOURCC('0', '1', 'w', 'b')

#define MOVI_LIST_POS (AVI_HEADER_SIZE - 12)
#define MOVI_FOURCC_POS (AVI_HEADER_SIZE - 4) // idx1 offsets are relative to this
#define SECTOR 512
#define SCRATCH_SIZE (AVI_MAX_AUDIO + 2 * SECTOR)
#define INDEX_GROW 4096

#define AVIF_HASINDEX 0x10
#define AVIF_ISINTERLEAVED 0x100
#define AVIIF_KEYFRAME 0x10

typedef struct {
	uint8_t *buf;
	uint32_t pos;
} Writer;

static void put32(uint8_t *p, uint32_t v) {
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static void w16(Writer *w, uint16_t v) {
	w->buf[w->pos++] = v;
	w->buf[w->pos++] = v >> 8;
}

static void w32(Writer *w, uint32_t v) {
	put32(w->buf + w->pos, v);
	w->pos += 4;
}

// Returns where the chunk's data starts, for endChunk
static uint32_t beginChunk(Writer *w, uint32_t id) {
	w32(w, id);
	w32(w, 0);
	return w->pos;
}

static void endChunk(Writer *w, uint32_t start) { put32(w->buf + start - 4, w->pos - start); }

static uint32_t beginList(Writer *w, uint32_t type) {
	uint32_t start = beginChunk(w, ID_LIST);
	w32(w, type);
	return start;
}

static void buildHeader(const AviFile *avi, uint8_t *buf, uint32_t riffSize, uint32_t moviEnd) {
	memset(buf, 0, AVI_HEADER_SIZE);
	Writer w = {buf, 0};

	w32(&w, ID_RIFF);
	w32(&w, riffSize);
	w32(&w, FOURCC('A', 'V', 'I', ' '));

	uint32_t hdrl = beginList(&w, FOURCC('h', 'd', 'r', 'l'));
	uint32_t avih = beginChunk(&w, FOURCC('a', 'v', 'i', 'h'));
	w32(&w, 1000000 / avi->fps);                   // dwMicroSecPerFrame
	w32(&w, avi->frameSize * avi->fps);            // dwMaxBytesPerSec
	w32(&w, 0);                                    // dwPaddingGranularity
	w32(&w, AVIF_HASINDEX | AVIF_ISINTERLEAVED);   // dwFlags
	w32(&w, avi->videoChunks);                     // dwTotalFrames
	w32(&w, 0);                                    // dwInitialFrames
	w32(&w, avi->sampleRate ? 2 : 1);              // dwStreams
	w32(&w, avi->frameSize + 8);                   // dwSuggestedBufferSize
	w32(&w, avi->width);                           // dwWidth
	w32(&w, avi->height);                          // dwHeight
	w.pos += 16;                                   // dwReserved
	endChunk(&w, avih);

	uint32_t strl = beginList(&w, FOURCC('s', 't', 'r', 'l'));
	uint32_t strh = beginChunk(&w, FOURCC('s', 't', 'r', 'h'));
	w32(&w, FOURCC('v', 'i', 'd', 's')); // fccType
	w32(&w, 0);                          // fccHandler
	w32(&w, 0);                          // dwFlags
	w16(&w, 0);                          // wPriority
	w16(&w, 0);                          // wLanguage
	w32(&w, 0);                          // dwInitialFrames
	w32(&w, 1);                          // dwScale
	w32(&w, avi->fps);                   // dwRate
	w32(&w, 0);                          // dwStart
	w32(&w, avi->videoChunks);           // dwLength
	w32(&w, avi->frameSize);             // dwSuggestedBufferSize
	w32(&w, 0xFFFFFFFF);                 // dwQuality
	w32(&w, 0);                          // dwSampleSize
	w16(&w, 0);                          // rcFrame
	w16(&w, 0);
	w16(&w, avi->width);
	w16(&w, avi->height);
	endChunk(&w, strh);
	uint32_t strf = beginChunk(&w, FOURCC('s', 't', 'r', 'f'));
	w32(&w, 40);             // biSize
	w32(&w, avi->width);     // biWidth
	w32(&w, avi->height);    // biHeight (positive = bottom-up)
	w16(&w, 1);              // biPlanes
	w16(&w, 16);             // biBitCount
	w32(&w, 0);              // biCompression (BI_RGB, 5-5-5 with blue in the low bits)
	w32(&w, avi->frameSize); // biSizeImage
	w.pos += 16;             // resolution, palette
	endChunk(&w, strf);
	endChunk(&w, strl);

	if(avi->sampleRate) {
		strl = beginList(&w, FOURCC('s', 't', 'r', 'l'));
		strh = beginChunk(&w, FOURCC('s', 't', 'r', 'h'));
		w32(&w, FOURCC('a', 'u', 'd', 's')); // fccType
		w32(&w, 0);                          // fccHandler
		w32(&w, 0);                          // dwFlags
		w16(&w, 0);                          // wPriority
		w16(&w, 0);                          // wLanguage
		w32(&w, 0);                          // dwInitialFrames
		w32(&w, 2);                          // dwScale (block align)
		w32(&w, avi->sampleRate * 2);        // dwRate (bytes per second)
		w32(&w, 0);                          // dwStart
		w32(&w, avi->audioBytes / 2);        // dwLength (samples)
		w32(&w, AVI_MAX_AUDIO);              // dwSuggestedBufferSize
		w32(&w, 0xFFFFFFFF);                 // dwQuality
		w32(&w, 2);                          // dwSampleSize
		w.pos += 8;                          // rcFrame
		endChunk(&w, strh);
		strf = beginChunk(&w, FOURCC('s', 't', 'r', 'f'));
		w16(&w, 1);                   // wFormatTag (PCM)
		w16(&w, 1);                   // nChannels
		w32(&w, avi->sampleRate);     // nSamplesPerSec
		w32(&w, avi->sampleRate * 2); // nAvgBytesPerSec
		w16(&w, 2);                   // nBlockAlign
		w16(&w, 16);                  // wBitsPerSample
		w16(&w, 0);                   // cbSize
		endChunk(&w, strf);
		endChunk(&w, strl);
	}
	endChunk(&w, hdrl);

	// Pad up to the movi list so frame data starts sector aligned
	uint32_t junk = beginChunk(&w, ID_JUNK);
	w.pos = MOVI_LIST_POS;
	endChunk(&w, junk);

	w32(&w, ID_LIST);
	w32(&w, moviEnd - MOVI_FOURCC_POS);
	w32(&w, FOURCC('m', 'o', 'v', 'i'));
}

static bool writeHeader(AviFile *avi, uint32_t riffSize, uint32_t moviEnd) {
	buildHeader(avi, avi->scratch, riffSize, moviEnd);
	if(lseek(avi->fd, 0, SEEK_SET) != 0 || write(avi->fd, avi->scratch, AVI_HEADER_SIZE) != AVI_HEADER_SIZE)
		return false;
	return lseek(avi->fd, avi->pos, SEEK_SET) == (off_t)avi->pos;
}

static bool addIndex(AviFile *avi, uint32_t id, uint32_t flags, uint32_t pos, uint32_t size) {
	if(avi->indexCount == avi->indexCap) {
		AviIndexEntry *index = realloc(avi->index, (avi->indexCap + INDEX_GROW) * sizeof(AviIndexEntry));
		if(!index)
			return false;
		avi->index = index;
		avi->indexCap += INDEX_GROW;
	}

	AviIndexEntry *entry = &avi->index[avi->indexCount++];
	entry->id            = id;
	entry->flags         = flags;
	entry->offset        = pos - MOVI_FOURCC_POS;
	entry->size          = size;
	return true;
}

static void freeAvi(AviFile *avi) {
	if(avi->fd >= 0)
		close(avi->fd);
	free(avi->index);
	free(avi->scratch);
	avi->fd      = -1;
	avi->index   = NULL;
	avi->scratch = NULL;
}

bool aviOpen(AviFile *avi, const char *path, uint16_t width, uint16_t height, uint32_t fps, uint32_t sampleRate) {
	memset(avi, 0, sizeof(*avi));
	avi->fd         = -1;
	avi->width      = width;
	avi->height     = height;
	avi->frameSize  = width * height * 2;
	avi->fps        = fps;
	avi->sampleRate = sampleRate;
	avi->pos = avi->pending = AVI_HEADER_SIZE;

	avi->scratch = memalign(32, SCRATCH_SIZE);
	if(!avi->scratch)
		return false;

	avi->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if(avi->fd < 0 || !writeHeader(avi, avi->pos - 8, avi->pos)) {
		freeAvi(avi);
		return false;
	}
	return true;
}

static uint32_t swapRedBlue(uint32_t v) {
	return (v & 0x83E083E0) | ((v & 0x001F001F) << 10) | ((v >> 10) & 0x001F001F);
}

void aviConvertFrame(uint16_t *frame, uint16_t width, uint16_t height) {
	uint32_t words = width / 2;
	for(uint32_t y = 0; y < (height + 1u) / 2; y++) {
		uint32_t *top    = (uint32_t *)(frame + y * width);
		uint32_t *bottom = (uint32_t *)(frame + (height - 1 - y) * width);
		for(uint32_t i = 0; i < words; i++) {
			uint32_t t = top[i];
			top[i]     = swapRedBlue(bottom[i]);
			if(top != bottom)
				bottom[i] = swapRedBlue(t);
		}
	}
}

bool aviPrepareFrame(AviFile *avi, uint8_t *prefix, uint32_t repeats) {
	if(repeats > AVI_MAX_REPEATS)
		repeats = AVI_MAX_REPEATS;

	uint32_t pos = avi->pending;
	uint8_t *p   = prefix;
	for(uint32_t i = 0; i < repeats; i++) {
		put32(p, ID_VIDEO);
		put32(p + 4, 0);
		if(!addIndex(avi, ID_VIDEO, 0, pos, 0))
			return false;
		p += 8;
		pos += 8;
	}

	uint32_t junk = AVI_FRAME_PREFIX - 8 * repeats - 16;
	put32(p, ID_JUNK);
	put32(p + 4, junk);
	memset(p + 8, 0, junk);
	p += 8 + junk;
	pos += 8 + junk;

	put32(p, ID_VIDEO);
	put32(p + 4, avi->frameSize);
	if(!addIndex(avi, ID_VIDEO, AVIIF_KEYFRAME, pos, avi->frameSize))
		return false;

	avi->pending = pos + 8 + avi->frameSize;
	avi->videoChunks += repeats + 1;
	avi->realFrames++;
	return true;
}

bool aviWriteFrames(AviFile *avi, const void *data, uint32_t count) {
	uint32_t size = count * (AVI_FRAME_PREFIX + avi->frameSize);
	if(avi->pos + size != avi->pending)
		return false;

	ssize_t res = write(avi->fd, data, size);
	if(res > 0)
		avi->pos += res;
	return res == (ssize_t)size;
}

bool aviWriteAudio(AviFile *avi, const void *samples, uint32_t bytes) {
	if(avi->pos != avi->pending || bytes > AVI_MAX_AUDIO || (bytes & 1))
		return false;
	if(!addIndex(avi, ID_AUDIO, AVIIF_KEYFRAME, avi->pos, bytes))
		return false;

	uint8_t *p = avi->scratch;
	put32(p, ID_AUDIO);
	put32(p + 4, bytes);
	memcpy(p + 8, samples, bytes);
	uint32_t size = 8 + bytes;

	// Pad with a JUNK chunk to keep the file sector aligned
	uint32_t pad = (SECTOR - size % SECTOR) % SECTOR;
	if(pad && pad < 8)
		pad += SECTOR;
	if(pad) {
		put32(p + size, ID_JUNK);
		put32(p + size + 4, pad - 8);
		memset(p + size + 8, 0, pad - 8);
		size += pad;
	}

	ssize_t res = write(avi->fd, p, size);
	if(res > 0)
		avi->pos += res;
	avi->pending = avi->pos;
	if(res != (ssize_t)size)
		return false;

	avi->audioBytes += bytes;
	return true;
}

uint32_t aviFinalSize(const AviFile *avi) { return avi->pending + 8 + avi->indexCount * sizeof(AviIndexEntry); }

bool aviSync(AviFile *avi) {
	if(avi->pos != avi->pending)
		return false;
	return writeHeader(avi, avi->pos - 8, avi->pos) && fsync(avi->fd) == 0;
}

bool aviClose(AviFile *avi, uint32_t sampleRate) {
	if(avi->sampleRate)
		avi->sampleRate = sampleRate;

	bool ok          = true;
	uint32_t moviEnd = avi->pos;
	uint32_t count   = avi->indexCount;
	if(avi->pos != avi->pending) {
		// A write failed: cut back to the last complete chunk and recount
		ok      = false;
		moviEnd = AVI_HEADER_SIZE;
		for(; count > 0; count--) {
			const AviIndexEntry *entry = &avi->index[count - 1];
			uint32_t end               = entry->offset + MOVI_FOURCC_POS + 8 + entry->size;
			if(end <= avi->pos) {
				moviEnd = end;
				break;
			}
		}

		avi->videoChunks = avi->audioBytes = 0;
		for(uint32_t i = 0; i < count; i++) {
			if(avi->index[i].id == ID_VIDEO)
				avi->videoChunks++;
			else
				avi->audioBytes += avi->index[i].size;
		}
		avi->pos = moviEnd;
	}

	uint32_t indexSize = count * sizeof(AviIndexEntry);
	uint8_t header[8];
	put32(header, FOURCC('i', 'd', 'x', '1'));
	put32(header + 4, indexSize);

	if(lseek(avi->fd, moviEnd, SEEK_SET) == (off_t)moviEnd && write(avi->fd, header, 8) == 8 &&
	   write(avi->fd, avi->index, indexSize) == (ssize_t)indexSize) {
		avi->pos = moviEnd + 8 + indexSize;
		if(!ok)
			ftruncate(avi->fd, avi->pos);
	} else {
		ok = false;
	}

	ok = writeHeader(avi, avi->pos - 8, moviEnd) && ok;
	ok = close(avi->fd) == 0 && ok;
	avi->fd = -1;
	freeAvi(avi);
	return ok;
}
