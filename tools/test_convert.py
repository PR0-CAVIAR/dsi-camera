#!/usr/bin/env python3
"""Tests convert.py against synthetic recordings. Needs ffmpeg and ffprobe on PATH.

    python tools/test_convert.py

The AVI tests check a file written by the DS's own AVI writer (arm9/source/avi.c),
built for the PC with tools/avi_selftest.c. Point DSI_AVI_SELFTEST at it:

    gcc -O2 -Iarm9/include tools/avi_selftest.c arm9/source/avi.c -lm -o avi_selftest
    ./avi_selftest selftest.avi
    DSI_AVI_SELFTEST=selftest.avi python tools/test_convert.py
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CONVERT = os.path.join(HERE, "convert.py")

WIDTH, HEIGHT = 256, 192
FRAME_SIZE = WIDTH * HEIGHT * 2
HEADER_SIZE = 512


def rgb15(r, g, b):
    """DS color: 5 bits each, R in the low bits, bit 15 set like the camera outputs."""
    return r | (g << 5) | (b << 10) | 0x8000


# Quadrants: red, green / blue, white. A wrong channel order swaps red and blue.
QUADRANTS = [rgb15(31, 0, 0), rgb15(0, 31, 0), rgb15(0, 0, 31), rgb15(31, 31, 31)]
QUADRANT_RGB = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]


def make_frame(index):
    pixels = []
    bar = (index * 8) % WIDTH
    for y in range(HEIGHT):
        top = 0 if y < HEIGHT // 2 else 2
        row = [QUADRANTS[top + (0 if x < WIDTH // 2 else 1)] for x in range(WIDTH)]
        # Moving black bar in the top 8 rows so frames differ
        if y < 8:
            for x in range(bar, min(bar + 8, WIDTH)):
                row[x] = rgb15(0, 0, 0)
        pixels.extend(row)
    return struct.pack(f"<{WIDTH * HEIGHT}H", *pixels)


def make_header(frame_count, fps, flags, part=0, dropped=0):
    header = struct.pack(
        "<4sHHHHHHIIIIIII", b"DSIV", 1, HEADER_SIZE, WIDTH, HEIGHT, 1, 2, FRAME_SIZE, frame_count,
        int(fps * 1000), int(frame_count * 1000 / fps) if fps else 0, dropped, flags, part,
    )
    return header.ljust(HEADER_SIZE, b"\0")


def write_recording(path, frames, fps, flags=1, header_frames=None, part=0, trailing=b""):
    with open(path, "wb") as f:
        f.write(make_header(frames if header_frames is None else header_frames, fps, flags, part))
        for i in range(frames):
            f.write(make_frame(i))
        f.write(trailing)


def run_convert(*args):
    return subprocess.run([sys.executable, CONVERT, *args], capture_output=True, text=True)


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
         "stream=width,height,r_frame_rate,nb_read_frames,codec_name,pix_fmt", "-of", "json", path],
        capture_output=True, text=True, check=True,
    )
    stream = json.loads(out.stdout)["streams"][0]
    num, den = stream["r_frame_rate"].split("/")
    stream["fps"] = int(num) / int(den)
    stream["nb_read_frames"] = int(stream["nb_read_frames"])
    return stream


def first_frame_rgb(path, width, height):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True,
    ).stdout
    assert len(out) == width * height * 3

    def pixel(x, y):
        i = (y * width + x) * 3
        return tuple(out[i:i + 3])

    return pixel


class ConvertTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    def assertColorNear(self, actual, expected, tolerance=24):
        for a, e in zip(actual, expected):
            self.assertLessEqual(abs(a - e), tolerance, f"{actual} != {expected}")

    def test_basic_conversion(self):
        src = self.path("REC_0000.BIN")
        write_recording(src, 30, 12.5)
        res = run_convert(src)
        self.assertEqual(res.returncode, 0, res.stderr)

        info = probe(self.path("REC_0000.mp4"))
        self.assertEqual((info["width"], info["height"]), (WIDTH, HEIGHT))
        self.assertEqual(info["nb_read_frames"], 30)
        self.assertAlmostEqual(info["fps"], 12.5, places=2)
        self.assertEqual(info["codec_name"], "h264")

        pixel = first_frame_rgb(self.path("REC_0000.mp4"), WIDTH, HEIGHT)
        for (x, y), rgb in zip([(64, 48), (192, 48), (64, 144), (192, 144)], QUADRANT_RGB):
            self.assertColorNear(pixel(x, y), rgb)

    def test_scale_is_nearest_neighbor(self):
        src = self.path("REC_0001.BIN")
        write_recording(src, 5, 10)
        out = self.path("big.mp4")
        res = run_convert(src, "--scale", "4", "-o", out)
        self.assertEqual(res.returncode, 0, res.stderr)

        info = probe(out)
        self.assertEqual((info["width"], info["height"]), (WIDTH * 4, HEIGHT * 4))
        pixel = first_frame_rgb(out, WIDTH * 4, HEIGHT * 4)
        # Right at the quadrant edge, nearest-neighbor keeps the colors pure
        self.assertColorNear(pixel(WIDTH * 2 - 4, HEIGHT), (255, 0, 0))
        self.assertColorNear(pixel(WIDTH * 2 + 3, HEIGHT), (0, 255, 0))

    def test_fps_override(self):
        src = self.path("REC_0002.BIN")
        write_recording(src, 10, 10)
        res = run_convert(src, "--fps", "24", "-o", self.path("o.mp4"))
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertAlmostEqual(probe(self.path("o.mp4"))["fps"], 24, places=2)

    def test_unfinalized_recording_uses_file_size(self):
        # Power loss: header still says 0 frames, last frame only half written
        src = self.path("REC_0003.BIN")
        write_recording(src, 7, 0, flags=0, header_frames=0, trailing=b"\x00" * (FRAME_SIZE // 2))
        res = run_convert(src, "-o", self.path("o.mp4"))
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("not closed cleanly", res.stderr)
        info = probe(self.path("o.mp4"))
        self.assertEqual(info["nb_read_frames"], 7)
        self.assertAlmostEqual(info["fps"], 10, places=2)  # default when the header has none

    def test_join_parts(self):
        a, b = self.path("REC_0004.BIN"), self.path("REC_0005.BIN")
        write_recording(a, 6, 15)
        write_recording(b, 4, 15, flags=1 | 2, part=1)
        res = run_convert(a, b, "-o", self.path("joined.mp4"))
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(probe(self.path("joined.mp4"))["nb_read_frames"], 10)

    def test_info(self):
        src = self.path("REC_0006.BIN")
        write_recording(src, 3, 11.25)
        res = run_convert(src, "--info")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("frames      3", res.stdout)
        self.assertIn("fps         11.250", res.stdout)
        self.assertFalse(os.path.exists(self.path("REC_0006.mp4")))

    def test_bad_magic(self):
        src = self.path("junk.bin")
        with open(src, "wb") as f:
            f.write(b"NOPE" + b"\0" * 1000)
        res = run_convert(src)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("bad magic", res.stderr)

    def test_refuses_to_overwrite(self):
        src = self.path("REC_0007.BIN")
        write_recording(src, 2, 10)
        self.assertEqual(run_convert(src).returncode, 0)
        res = run_convert(src)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("-y", res.stderr)
        self.assertEqual(run_convert(src, "-y").returncode, 0)


SELFTEST_AVI = os.environ.get("DSI_AVI_SELFTEST")
# Must match tools/avi_selftest.c
SELFTEST_REPEATS = [0, 1, 1, 0, 2, 1, 5, 1, 0, 1, 1, 1]
SELFTEST_SAMPLES = 14182


def selftest_frame_pts():
    pts, slot = [], 0
    for repeats in SELFTEST_REPEATS:
        slot += repeats
        pts.append(slot)
        slot += 1
    return pts


@unittest.skipUnless(SELFTEST_AVI and os.path.exists(SELFTEST_AVI), "set DSI_AVI_SELFTEST to an avi_selftest output")
class AviWriterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def streams(self, path):
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries",
             "stream=codec_type,codec_name,width,height,r_frame_rate,sample_rate,channels", "-of", "json", path],
            capture_output=True, text=True, check=True,
        )
        return {s["codec_type"]: s for s in json.loads(out.stdout)["streams"]}

    def frame_times(self, path):
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "frame=pts_time", "-of", "csv=p=0", path],
            capture_output=True, text=True, check=True,
        )
        return [float(line.split(",")[0]) for line in out.stdout.split()]

    def test_streams(self):
        streams = self.streams(SELFTEST_AVI)
        video, audio = streams["video"], streams["audio"]
        self.assertEqual((video["codec_name"], video["width"], video["height"]), ("rawvideo", WIDTH, HEIGHT))
        self.assertEqual(video["r_frame_rate"], "30/1")
        self.assertEqual((audio["codec_name"], audio["sample_rate"], audio["channels"]), ("pcm_s16le", "16364", 1))

    def test_repeated_frames_keep_timestamps(self):
        expected = [p / 30 for p in selftest_frame_pts()]
        actual = self.frame_times(SELFTEST_AVI)
        self.assertEqual(len(actual), len(expected))
        for a, e in zip(actual, expected):
            self.assertAlmostEqual(a, e, places=3)

    def test_colors_and_orientation(self):
        pixel = first_frame_rgb(SELFTEST_AVI, WIDTH, HEIGHT)
        for (x, y), rgb in zip([(64, 48), (192, 48), (64, 144), (192, 144)], QUADRANT_RGB):
            self.assertColorNear(pixel(x, y), rgb)
        self.assertColorNear(pixel(4, 2), (0, 0, 0))  # frame 0's marker is in the top-left corner

    def assertColorNear(self, actual, expected, tolerance=24):
        for a, e in zip(actual, expected):
            self.assertLessEqual(abs(a - e), tolerance, f"{actual} != {expected}")

    def test_audio_length(self):
        out = subprocess.run(["ffmpeg", "-v", "error", "-i", SELFTEST_AVI, "-map", "0:a", "-f", "s16le", "-"],
                             capture_output=True, check=True).stdout
        self.assertEqual(len(out) // 2, SELFTEST_SAMPLES)

    def test_convert_to_mp4(self):
        out = os.path.join(self.tmp.name, "out.mp4")
        res = run_convert(SELFTEST_AVI, "--scale", "2", "-o", out)
        self.assertEqual(res.returncode, 0, res.stderr)

        streams = self.streams(out)
        self.assertEqual((streams["video"]["codec_name"], streams["video"]["width"]), ("h264", WIDTH * 2))
        self.assertEqual(streams["audio"]["codec_name"], "aac")
        times = self.frame_times(out)
        expected = [p / 30 for p in selftest_frame_pts()]
        self.assertEqual(len(times), len(expected))
        for a, e in zip(times, expected):
            self.assertAlmostEqual(a - times[0], e, delta=0.002)

    def test_join_parts(self):
        out = os.path.join(self.tmp.name, "joined.mp4")
        res = run_convert(SELFTEST_AVI, SELFTEST_AVI, "-o", out)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(len(self.frame_times(out)), 2 * len(SELFTEST_REPEATS))


if __name__ == "__main__":
    unittest.main(verbosity=2)
