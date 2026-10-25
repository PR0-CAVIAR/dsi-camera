# DSi camera test

This is an example app for using the DSi's cameras. It shows both cameras, saves a picture to `sd:/DCIM/100DSI00/IMG_####.png`, and records video with sound to `sd:/DCIM/VIDEO/REC_####.AVI`.

This fork by [PR0-CAVIAR](https://github.com/PR0-CAVIAR) adds the video recording with sound to [Epicpkmn11/dsi-camera](https://github.com/Epicpkmn11/dsi-camera). Download the `.nds` from the [releases](https://github.com/PR0-CAVIAR/dsi-camera/releases).

![Example photo](resources/example.png)

## Controls

| Button | Action |
| --- | --- |
| A | Swap inner/outer camera (disabled while recording) |
| L / R | Take a 640x480 photo (disabled while recording) |
| SELECT | Start / stop video recording |
| START | Exit (stops and saves a recording first) |

While recording, the top screen shows the live picture with a red dot, and the bottom screen shows:

- `Time`: elapsed time
- `Frames` / `Dropped`: frames saved, and frames the camera delivered that didn't fit in the buffer because the SD card fell behind
- `FPS`: saved frames per second, and frames per second coming from the camera, averaged over the whole recording
- `Live`: camera frames per second over the last half second, plus the shortest and longest gap between frames
- `Audio`: the mic's sample rate and a level meter. It shows a warning if the mic failed to start or sends no data.
- `SD`: write speed while writing, and the share of time spent writing (near 100% means the SD card is the bottleneck)
- `Buffer`: frames waiting to be written (a full buffer means frames are being dropped)
- `File` / `Free`: current file size, and free space with an estimate of the minutes left

If the camera stops delivering frames for 2 seconds, "No frames from camera!" is shown. After you stop, a summary (frames, length, fps, dropped, audio rate, file name) and any error stay on screen.

## Video recording

Recordings are standard **AVI files: 256x192 uncompressed 16-bit video plus 16-bit mono PCM audio** from the built-in mic. They play as they are in VLC and other ffmpeg-based players. For an MP4 to put on a phone or share, use the converter below.

The DS can't encode MP4 (H.264) itself: it's far too slow for that. So it records uncompressed video, about 1.5 MB/s or roughly 5 GB per hour.

Video comes from the camera's **preview stream**: 256x192, with the camera interface converting YUV to RGB in hardware. The 640x480 capture mode is 600 KiB per frame, more than 6x the data, which would cap recording at a few fps.

### Frame rate

On real hardware this records about 13-15 fps in daylight and about 9-11 fps indoors. The SD card isn't the limit: a test card wrote about 4.8 MB/s, enough for roughly 50 fps of these frames, while it was only busy 26% of the time. The limit is the camera sensor, whose auto exposure stretches frames in low light.

An experiment that lowered the sensor's max exposure (`AE_MAX_INDEX`, MCU 0xA20C) at runtime didn't clearly raise the frame rate. Raising it locked up the DSi, most likely because the ARM7 waited forever for the sensor's refresh command to finish. That code was removed.

### Audio sync

The AVI runs at a fixed 30 fps timeline. Each camera frame is timestamped when it arrives and placed at its real time. In between, zero-length "repeat the previous frame" chunks fill the gaps. That way audio stays in sync even when the camera's frame rate changes, for example going from indoors to daylight.

The mic is recorded with calico's DMA mode (`MicRate_Div2`), which is 16364 Hz with the DSi sound clock at 32 kHz. If a launcher left the clock at 47 kHz, the rate becomes 23803 Hz instead. The app measures which one it's getting and writes the right rate into the file.

### How it works

- The camera DMAs each frame into a ring buffer of up to 32 frames (about 3 MiB) in main RAM. Each slot has 512 bytes in front of the frame for its AVI chunk headers, so a run of frames is written with a single `write()` and stays sector aligned.
- When a frame finishes, an NDMA interrupt timestamps it, queues it, and starts the next transfer right away. Capture keeps running while the main loop is blocked writing to the SD card.
- Just before writing, each frame is flipped and its red and blue swapped in place, into the bottom-up BGR555 layout AVI uses.
- Mic buffers arrive on calico's mic thread and are queued in a 256 KiB ring. They're written as audio chunks every ~8 KiB.
- If the frame buffer fills up, new frames are dropped and counted. The timeline repeats the previous frame instead, so the timing stays right.
- Every 5 seconds the AVI header and FAT directory entry are synced. If the battery dies, the file is missing its index but ffmpeg and VLC can still read it, and the converter handles it.
- AVI files without OpenDML get unreliable past 1 GiB (about 12 minutes). Long recordings continue in the next `REC_####.AVI`.
- Recording stops cleanly, with a message, if the SD card is full or a write fails.

The AVI writer (`arm9/source/avi.c`) is plain C, so it can be tested on a PC. See `tools/avi_selftest.c` and `tools/test_convert.py`.

## Converting to MP4

You need Python 3 and [ffmpeg](https://ffmpeg.org) 5.1 or newer on your PATH. Copy the files off the SD card, then run:

```sh
python tools/convert.py REC_0000.AVI                       # -> REC_0000.mp4 (H.264 + AAC)
python tools/convert.py REC_0000.AVI --scale 4 -o car.mp4  # 1024x768, crisp nearest-neighbor pixels
python tools/convert.py REC_0003.AVI REC_0004.AVI          # join the parts of a long recording
python tools/convert.py REC_0000.AVI --info                # print stream info only
```

Every frame keeps its recorded timestamp in the MP4.

### Old .BIN recordings

Earlier test builds wrote `REC_####.BIN`: a 512-byte little-endian header followed by raw frames. The converter still reads them, with `--fps` to override the frame rate:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | char[4] | magic `DSIV` |
| 4 | u16 | version (1) |
| 6 | u16 | header size (512) |
| 8 | u16 | width (256) |
| 10 | u16 | height (192) |
| 12 | u16 | pixel format: 1 = RGB15 (ffmpeg `bgr555le`) |
| 14 | u16 | camera: 1 = inner, 2 = outer |
| 16 | u32 | bytes per frame (98304) |
| 20 | u32 | frame count |
| 24 | u32 | average fps × 1000 |
| 28 | u32 | duration in ms |
| 32 | u32 | dropped frames |
| 36 | u32 | flags: bit 0 = closed cleanly, bit 1 = continues previous file, bit 2 = stopped by an error |
| 40 | u32 | part number (0 for the first file) |

## Building

The build uses devkitARM with calico and libnds, which is the same setup as CI.

With Docker, from the repo root:

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitarm sh -c "git config --global --add safe.directory /src && make"
```

With a native [devkitPro](https://devkitpro.org/wiki/Getting_Started) install that has the `nds-dev` group:

```sh
make
```

Either way, the output is `dsi-camera.nds`. Copy it to the SD card and launch it from TWiLight Menu++ or another homebrew launcher running in DSi mode. The cameras only work on DSi/3DS hardware in DSi mode.

To test the AVI writer and converter on a PC (the devkitarm image includes gcc):

```sh
gcc -O2 -Wall -Iarm9/include tools/avi_selftest.c arm9/source/avi.c -lm -o avi_selftest && ./avi_selftest selftest.avi
DSI_AVI_SELFTEST=selftest.avi python tools/test_convert.py
```

Credits to:
- [Arisotura](http://kuribo64.net) for finding why it wasn't working
- [devkitPro](https://github.com/devkitPro) for devkitARM and libnds
- [nocash](https://problemkaputt.de) for GBATek
