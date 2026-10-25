#!/usr/bin/env python3
"""Convert dsi-camera recordings (REC_####.AVI, or older REC_####.BIN) to MP4 with ffmpeg.

Usage:
    convert.py REC_0001.AVI                      -> REC_0001.mp4
    convert.py REC_0001.AVI --scale 4 -o car.mp4  (1024x768, nearest-neighbor)
    convert.py REC_0003.AVI REC_0004.AVI          (join the parts of a long recording)
    convert.py REC_0001.AVI --info                (print stream info, don't convert)

The AVIs play as-is in VLC and most PC players; MP4 is for phones and sharing.

Old .BIN format (little-endian), see README.md:
    512-byte header, then frameCount frames of width*height RGB15 pixels.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"DSIV"
HEADER_FORMAT = "<4sHHHHHHIIIIIII"  # 44 bytes, padded to headerSize
HEADER_FIELDS = (
    "magic", "version", "header_size", "width", "height", "pixel_format", "camera",
    "frame_size", "frame_count", "fps_x1000", "duration_ms", "dropped_frames", "flags", "part",
)
FLAG_FINALIZED = 1 << 0
FLAG_CONTINUED = 1 << 1
FLAG_ERROR = 1 << 2

# header pixel_format -> (ffmpeg pix_fmt, bytes per pixel)
PIXEL_FORMATS = {
    1: ("bgr555le", 2),  # DS RGB15: R bits 0-4, G 5-9, B 10-14, bit 15 ignored
    2: ("yuyv422", 2),   # reserved for raw YUV422 from capture mode
}
DEFAULT_FPS = 10.0
CHUNK_FRAMES = 64


class FormatError(Exception):
    pass


def read_header(path):
    with open(path, "rb") as f:
        raw = f.read(struct.calcsize(HEADER_FORMAT))
    if len(raw) < struct.calcsize(HEADER_FORMAT):
        raise FormatError(f"{path}: too small to be a recording")

    header = dict(zip(HEADER_FIELDS, struct.unpack(HEADER_FORMAT, raw)))
    if header["magic"] != MAGIC:
        raise FormatError(f"{path}: bad magic {header['magic']!r}, expected {MAGIC!r}")
    if header["version"] != 1:
        print(f"warning: {path}: unknown version {header['version']}, trying anyway", file=sys.stderr)
    if header["pixel_format"] not in PIXEL_FORMATS:
        raise FormatError(f"{path}: unsupported pixel format {header['pixel_format']}")

    bpp = PIXEL_FORMATS[header["pixel_format"]][1]
    expected = header["width"] * header["height"] * bpp
    if header["frame_size"] != expected:
        raise FormatError(f"{path}: frame size {header['frame_size']} != {header['width']}x{header['height']}x{bpp}")

    # A recording interrupted by power loss may have a stale frame count, trust the file size
    data_size = os.path.getsize(path) - header["header_size"]
    on_disk = max(data_size, 0) // header["frame_size"]
    header["frames_on_disk"] = on_disk
    if not header["flags"] & FLAG_FINALIZED:
        print(f"warning: {path}: not closed cleanly, using {on_disk} frames found on disk", file=sys.stderr)
        header["frame_count"] = on_disk
    elif header["frame_count"] > on_disk:
        print(f"warning: {path}: header says {header['frame_count']} frames, file only has {on_disk}", file=sys.stderr)
        header["frame_count"] = on_disk
    return header


def print_info(path, header):
    flags = [name for bit, name in ((FLAG_FINALIZED, "finalized"), (FLAG_CONTINUED, "continued"), (FLAG_ERROR, "error"))
             if header["flags"] & bit]
    print(f"{path}:")
    print(f"  size        {header['width']}x{header['height']}, {PIXEL_FORMATS[header['pixel_format']][0]}")
    print(f"  camera      {'inner' if header['camera'] == 1 else 'outer'}")
    print(f"  frames      {header['frame_count']} ({header['frames_on_disk']} on disk)")
    print(f"  fps         {header['fps_x1000'] / 1000:.3f}")
    print(f"  duration    {header['duration_ms'] / 1000:.1f} s")
    print(f"  dropped     {header['dropped_frames']}")
    print(f"  part        {header['part'] + 1}")
    print(f"  flags       {', '.join(flags) or 'none'}")


def find_ffmpeg(explicit):
    ffmpeg = explicit or shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("error: ffmpeg not found, install it or pass --ffmpeg PATH")
    return ffmpeg


def is_avi(path):
    with open(path, "rb") as f:
        head = f.read(12)
    return head[:4] == b"RIFF" and head[8:12] == b"AVI "


def output_path(args):
    output = args.output or os.path.splitext(args.inputs[0])[0] + ".mp4"
    if os.path.exists(output) and not args.overwrite:
        sys.exit(f"error: {output} exists, pass -y to overwrite")
    return output


def convert_avi(args):
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if args.info:
        ffprobe = os.path.join(os.path.dirname(ffmpeg), "ffprobe" + os.path.splitext(ffmpeg)[1])
        for path in args.inputs:
            print(f"{path}:")
            subprocess.run([ffprobe if os.path.exists(ffprobe) else "ffprobe", "-v", "error", "-show_entries",
                            "stream=codec_type,codec_name,width,height,sample_rate:format=duration",
                            "-of", "default=noprint_wrappers=1", path])
        return
    if args.fps:
        print("warning: --fps is ignored for AVI files, they carry exact timestamps", file=sys.stderr)

    output = output_path(args)
    with tempfile.TemporaryDirectory() as tmp:
        if len(args.inputs) == 1:
            source = ["-i", args.inputs[0]]
        else:
            # Join the parts of a long recording with ffmpeg's concat demuxer
            listing = os.path.join(tmp, "parts.txt")
            with open(listing, "w", encoding="utf-8") as f:
                for path in args.inputs:
                    escaped = os.path.abspath(path).replace("'", "'\\''")
                    f.write(f"file '{escaped}'\n")
            source = ["-f", "concat", "-safe", "0", "-i", listing]

        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *source]
        if args.scale > 1:
            cmd += ["-vf", f"scale=iw*{args.scale}:ih*{args.scale}:flags=neighbor"]
        # passthrough keeps each frame at its recorded time instead of re-timing to a fixed rate
        cmd += ["-fps_mode", "passthrough", "-c:v", "libx264", "-crf", str(args.crf), "-pix_fmt", "yuv420p",
                "-c:a", "aac", "-b:a", "96k", "-movflags", "+faststart", output]

        print(f"{', '.join(args.inputs)} -> {output}")
        if subprocess.run(cmd).returncode != 0:
            sys.exit("error: ffmpeg failed")


def main():
    parser = argparse.ArgumentParser(description="Convert dsi-camera recordings (.AVI or old .BIN) to MP4.")
    parser.add_argument("inputs", nargs="+", help="recording file(s); several are joined in order")
    parser.add_argument("-o", "--output", help="output file (default: first input with .mp4)")
    parser.add_argument("--fps", type=float, help="override the frame rate stored in a .BIN header")
    parser.add_argument("--scale", type=int, default=1, help="integer nearest-neighbor upscale (e.g. 4 -> 1024x768)")
    parser.add_argument("--crf", type=int, default=18, help="x264 quality, lower is better (default 18)")
    parser.add_argument("--ffmpeg", help="path to ffmpeg")
    parser.add_argument("--info", action="store_true", help="print header info and exit")
    parser.add_argument("-y", "--overwrite", action="store_true", help="overwrite the output file")
    args = parser.parse_args()

    if args.scale < 1:
        sys.exit("error: --scale must be 1 or more")

    try:
        kinds = {is_avi(path) for path in args.inputs}
    except OSError as e:
        sys.exit(f"error: {e}")
    if kinds == {True}:
        convert_avi(args)
        return
    if len(kinds) > 1:
        sys.exit("error: can't mix .AVI and .BIN inputs")

    try:
        headers = [read_header(path) for path in args.inputs]
    except (OSError, FormatError) as e:
        sys.exit(f"error: {e}")

    if args.info:
        for path, header in zip(args.inputs, headers):
            print_info(path, header)
        return

    first = headers[0]
    for path, header in zip(args.inputs[1:], headers[1:]):
        if (header["width"], header["height"], header["pixel_format"]) != (first["width"], first["height"], first["pixel_format"]):
            sys.exit(f"error: {path} has a different size or pixel format than {args.inputs[0]}")

    fps = args.fps or first["fps_x1000"] / 1000
    if fps <= 0:
        print(f"warning: no frame rate in header, using {DEFAULT_FPS} (use --fps to set one)", file=sys.stderr)
        fps = DEFAULT_FPS

    total = sum(h["frame_count"] for h in headers)
    if total == 0:
        sys.exit("error: no frames to convert")

    output = output_path(args)

    pix_fmt = PIXEL_FORMATS[first["pixel_format"]][0]
    width, height = first["width"], first["height"]
    cmd = [
        find_ffmpeg(args.ffmpeg), "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", pix_fmt, "-s", f"{width}x{height}", "-framerate", f"{fps:.6g}", "-i", "-",
    ]
    if args.scale > 1:
        cmd += ["-vf", f"scale=iw*{args.scale}:ih*{args.scale}:flags=neighbor"]
    # yuv420p for compatibility; at --scale 2+ the chroma subsampling stays inside the blocks
    cmd += ["-c:v", "libx264", "-crf", str(args.crf), "-pix_fmt", "yuv420p", "-movflags", "+faststart", output]

    print(f"{total} frames, {width}x{height} -> {width * args.scale}x{height * args.scale} @ {fps:.3f} fps "
          f"({total / fps:.1f} s) -> {output}")

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    try:
        for path, header in zip(args.inputs, headers):
            with open(path, "rb") as f:
                f.seek(header["header_size"])
                remaining = header["frame_count"]
                while remaining:
                    count = min(remaining, CHUNK_FRAMES)
                    proc.stdin.write(f.read(count * header["frame_size"]))
                    remaining -= count
        proc.stdin.close()
    except BrokenPipeError:
        pass
    if proc.wait() != 0:
        sys.exit(f"error: ffmpeg failed with exit code {proc.returncode}")


if __name__ == "__main__":
    main()
