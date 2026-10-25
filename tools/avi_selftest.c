// Writes a test AVI with arm9/source/avi.c on a PC, using it the same way the
// recorder does. tools/test_convert.py checks the result with ffmpeg.
//
//   gcc -O2 -Wall -Iarm9/include tools/avi_selftest.c arm9/source/avi.c -lm -o avi_selftest
//   ./avi_selftest selftest.avi

#include "avi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 256
#define HEIGHT 192
#define FPS 30
#define RATE 16364
#define SLOT_SIZE (AVI_FRAME_PREFIX + WIDTH * HEIGHT * 2)
#define BATCH 3

// Repeated frames before each real frame, like a camera running below 30 fps
static const uint8_t repeats[] = {0, 1, 1, 0, 2, 1, 5, 1, 0, 1, 1, 1};
#define FRAMES (sizeof(repeats) / sizeof(repeats[0]))

static uint16_t rgb15(int r, int g, int b) { return r | (g << 5) | (b << 10) | 0x8000; }

// Quadrants red, green / blue, white, with a black marker in the top rows at x = 16 * n
static void drawFrame(uint16_t *px, int n) {
	for(int y = 0; y < HEIGHT; y++) {
		for(int x = 0; x < WIDTH; x++) {
			int top    = y < HEIGHT / 2;
			int left   = x < WIDTH / 2;
			uint16_t c = top ? (left ? rgb15(31, 0, 0) : rgb15(0, 31, 0)) : (left ? rgb15(0, 0, 31) : rgb15(31, 31, 31));
			if(y < 8 && x >= n * 16 && x < n * 16 + 8)
				c = rgb15(0, 0, 0);
			px[y * WIDTH + x] = c;
		}
	}
}

static int writeAudio(AviFile *avi, uint32_t *written, uint32_t target) {
	static int16_t buf[AVI_MAX_AUDIO / 2];
	while(*written < target) {
		uint32_t count = target - *written;
		if(count > AVI_MAX_AUDIO / 2)
			count = AVI_MAX_AUDIO / 2;
		for(uint32_t i = 0; i < count; i++)
			buf[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * (*written + i) / RATE));
		if(!aviWriteAudio(avi, buf, count * 2))
			return 0;
		*written += count;
	}
	return 1;
}

int main(int argc, char **argv) {
	if(argc != 2) {
		fprintf(stderr, "usage: %s out.avi\n", argv[0]);
		return 2;
	}

	AviFile avi;
	if(!aviOpen(&avi, argv[1], WIDTH, HEIGHT, FPS, RATE)) {
		perror("aviOpen");
		return 1;
	}

	uint8_t *slots    = malloc(SLOT_SIZE * BATCH);
	uint32_t timeline = 0, samples = 0;
	for(uint32_t i = 0; i < FRAMES;) {
		uint32_t batch = FRAMES - i < BATCH ? FRAMES - i : BATCH;
		uint32_t end   = timeline;
		for(uint32_t j = 0; j < batch; j++)
			end += repeats[i + j] + 1;

		// Audio first, like the recorder does
		if(!writeAudio(&avi, &samples, (uint64_t)end * RATE / FPS)) {
			perror("aviWriteAudio");
			return 1;
		}

		for(uint32_t j = 0; j < batch; j++) {
			uint8_t *slot   = slots + j * SLOT_SIZE;
			uint16_t *frame = (uint16_t *)(slot + AVI_FRAME_PREFIX);
			drawFrame(frame, i + j);
			aviConvertFrame(frame, WIDTH, HEIGHT);
			if(!aviPrepareFrame(&avi, slot, repeats[i + j])) {
				fprintf(stderr, "aviPrepareFrame failed\n");
				return 1;
			}
		}
		if(!aviWriteFrames(&avi, slots, batch)) {
			perror("aviWriteFrames");
			return 1;
		}

		timeline = end;
		i += batch;
		if(i == 6 && !aviSync(&avi)) {
			perror("aviSync");
			return 1;
		}
	}

	uint32_t expectedSize = aviFinalSize(&avi);
	if(!aviClose(&avi, RATE)) {
		perror("aviClose");
		return 1;
	}

	printf("frames=%u timeline=%u samples=%u size=%u\n", (unsigned)FRAMES, timeline, samples, expectedSize);
	free(slots);
	return 0;
}
