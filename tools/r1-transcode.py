#!/usr/bin/env python3
"""Turn any video into one the HiBy R1 can play, and line a music video up
with its track.

    tools/r1-transcode.py canvas  CLIP  [-o OUT]        an animated cover
    tools/r1-transcode.py video   CLIP  --track SONG    a music video
    tools/r1-transcode.py clip    CLIP  [-o OUT]        for a screensaver

The player decodes with FFmpeg (see tools/rbvideo), so it can play most
files as they are. Converting is still worth it: a 1 GHz MIPS core with no
video hardware decodes a small picture comfortably and a large one badly,
and the file is on an SD card. The defaults here are what plays smoothly:
H.264 baseline, no B-frames, 480 px wide at most, 24 fps, a keyframe every
second, and no audio (a canvas and a music video are silent - the sound is
the track's).

For a music video it also finds the offset between the video's own sound
and the track's, by cross-correlating their loudness envelopes, and writes
it into <track>.video.cfg, which is what the player reads:

    offset_ms = 1240      # the video starts 1.24 s after the track
    rate      = 1.0       # video speed against the track's
    start_ms  = 0         # trim at the start of the video
    end_ms    = 0         # trim at its end, 0 = none
    outside   = cover     # cover (show the cover) or hold (freeze)

--check-drift measures the offset near the start and near the end and
writes `rate` as well, for a video cut from a different master.

Needs ffmpeg and ffprobe on the PATH. numpy is used when it is installed;
without it a small built-in FFT does the same job a little slower.
"""

import argparse
import cmath
import json
import math
import os
import shutil
import subprocess
import sys

PANEL_W, PANEL_H = 480, 800

# ------------------------------------------------------------- ffmpeg

def tool(name):
    """ffmpeg, or ffmpeg.exe when the PATH only has that (WSL, MSYS)."""
    return shutil.which(name) or shutil.which(name + ".exe")


FFMPEG = FFPROBE = None


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def need_tools():
    global FFMPEG, FFPROBE
    FFMPEG = tool("ffmpeg")
    FFPROBE = tool("ffprobe")
    for name, path in (("ffmpeg", FFMPEG), ("ffprobe", FFPROBE)):
        if not path:
            sys.exit("%s is not on the PATH" % name)


def probe(path):
    out = run([FFPROBE, "-v", "error", "-print_format", "json",
               "-show_format", "-show_streams", path]).stdout
    return json.loads(out)


def stream(info, kind):
    for s in info.get("streams", []):
        if s.get("codec_type") == kind:
            return s
    return None


def duration(info):
    try:
        return float(info["format"]["duration"])
    except (KeyError, ValueError):
        return 0.0


# ------------------------------------------- loudness envelope and offset

def envelope(path, rate=100, start=0.0, length=None):
    """The loudness of `path`, `rate` values a second."""
    sr = 8000
    step = sr // rate
    cmd = [FFMPEG, "-v", "error"]
    if start:
        cmd += ["-ss", "%.3f" % start]
    if length:
        cmd += ["-t", "%.3f" % length]
    cmd += ["-i", path, "-vn", "-ac", "1", "-ar", str(sr),
            "-f", "s16le", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    n = len(raw) // 2
    try:
        import numpy as np
        a = np.frombuffer(raw[:n * 2], dtype="<i2").astype("float32")
        m = (len(a) // step) * step
        env = np.abs(a[:m]).reshape(-1, step).mean(axis=1)
        return env.tolist()
    except ImportError:
        import array
        a = array.array("h")
        a.frombytes(raw[:n * 2])
        env = []
        for i in range(0, len(a) - step, step):
            s = 0
            for j in range(i, i + step):
                s += a[j] if a[j] >= 0 else -a[j]
            env.append(s / step)
        return env


def _fft(x, inverse=False):
    n = len(x)
    if n == 1:
        return x
    even = _fft(x[0::2], inverse)
    odd = _fft(x[1::2], inverse)
    sign = 1 if inverse else -1
    out = [0] * n
    for k in range(n // 2):
        t = cmath.exp(sign * 2j * math.pi * k / n) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def correlate(a, b):
    """The lag (in samples of the envelopes) where b best matches a, and
    how strong the match is (0..1)."""
    try:
        import numpy as np
        x = np.asarray(a, dtype="float64")
        y = np.asarray(b, dtype="float64")
        x -= x.mean()
        y -= y.mean()
        n = 1
        while n < len(x) + len(y):
            n *= 2
        c = np.fft.irfft(np.fft.rfft(x, n) * np.conj(np.fft.rfft(y, n)), n)
        c = np.concatenate((c[-(len(y) - 1):], c[:len(x)]))
        lag = int(c.argmax()) - (len(y) - 1)
        peak = float(c.max())
        norm = float(np.sqrt((x ** 2).sum() * (y ** 2).sum())) or 1.0
        return lag, max(0.0, min(1.0, peak / norm))
    except ImportError:
        pass

    ma = sum(a) / len(a)
    mb = sum(b) / len(b)
    x = [v - ma for v in a]
    y = [v - mb for v in b]
    n = 1
    while n < len(x) + len(y):
        n *= 2
    fx = _fft([complex(v) for v in x] + [0j] * (n - len(x)))
    fy = _fft([complex(v) for v in y] + [0j] * (n - len(y)))
    prod = [fx[i] * fy[i].conjugate() for i in range(n)]
    c = [v.real / n for v in _fft(prod, inverse=True)]
    c = c[-(len(y) - 1):] + c[:len(x)]
    peak = max(c)
    lag = c.index(peak) - (len(y) - 1)
    ea = math.sqrt(sum(v * v for v in x))
    eb = math.sqrt(sum(v * v for v in y))
    return lag, max(0.0, min(1.0, peak / ((ea * eb) or 1.0)))


def find_offset(track, video, start=0.0, length=None):
    """How long after the track's start the video's sound begins, in ms."""
    rate = 100
    a = envelope(track, rate, start, length)
    b = envelope(video, rate, start, length)
    if len(a) < rate or len(b) < rate:
        return None, 0.0
    lag, conf = correlate(a, b)
    return int(round(-lag * 1000.0 / rate)), conf


# ------------------------------------------------------------- encoding

def encode(src, dst, w, h, fps, crf, bitrate, mode, gop, keep_audio):
    """Scale to fit w x h (mode: fit, fill, stretch) and encode."""
    if mode == "fill":
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase,"
              "crop=%d:%d" % (w, h, w, h))
    elif mode == "stretch":
        vf = "scale=%d:%d" % (w, h)
    else:
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease,"
              "scale=trunc(iw/2)*2:trunc(ih/2)*2" % (w, h))
    vf += ",fps=%g,format=yuv420p" % fps

    cmd = [FFMPEG, "-v", "error", "-y", "-i", src,
           "-vf", vf,
           "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0",
           "-preset", "slow", "-tune", "fastdecode",
           "-bf", "0", "-refs", "1",
           "-g", str(int(round(fps * gop))), "-keyint_min", "1",
           "-sc_threshold", "0",
           "-crf", str(crf), "-maxrate", bitrate, "-bufsize", bitrate,
           "-movflags", "+faststart"]
    if keep_audio:
        cmd += ["-c:a", "aac", "-b:a", "128k"]
    else:
        cmd += ["-an"]
    cmd += [dst]
    run(cmd)


def write_cfg(path, offset_ms, rate, start_ms, end_ms, outside, conf):
    with open(path, "w", newline="\n") as f:
        f.write("# Written by tools/r1-transcode.py\n")
        if conf is not None:
            f.write("# match confidence %.2f%s\n"
                    % (conf, "" if conf >= 0.3 else "  (low - check by ear)"))
        f.write("offset_ms = %d\n" % offset_ms)
        f.write("rate      = %.6g\n" % rate)
        f.write("start_ms  = %d\n" % start_ms)
        f.write("end_ms    = %d\n" % end_ms)
        f.write("outside   = %s\n" % outside)


# ----------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("kind", choices=["canvas", "video", "clip"])
    p.add_argument("input")
    p.add_argument("-o", "--out", help="output file (default: next to the "
                                       "track, or beside the input)")
    p.add_argument("--track", help="the song a music video belongs to")
    p.add_argument("--size", help="WxH for the picture "
                                  "(default: 480x480 canvas, 480x800 clip, "
                                  "480x270 video)")
    p.add_argument("--mode", choices=["fit", "fill", "stretch"],
                   default=None, help="how to fit the size")
    p.add_argument("--fps", type=float, default=24.0)
    p.add_argument("--crf", type=int, default=26)
    p.add_argument("--bitrate", default="900k")
    p.add_argument("--gop", type=float, default=1.0,
                   help="seconds between keyframes (seeking and looping)")
    p.add_argument("--offset-ms", type=int, default=None,
                   help="skip the search and use this offset")
    p.add_argument("--check-drift", action="store_true",
                   help="also measure `rate` (a different master)")
    p.add_argument("--outside", choices=["cover", "hold"], default="cover")
    p.add_argument("--keep-audio", action="store_true",
                   help="a standalone film keeps its sound")
    p.add_argument("-n", "--dry-run", action="store_true")
    args = p.parse_args()

    need_tools()
    info = probe(args.input)
    vs = stream(info, "video")
    if not vs:
        sys.exit("%s has no picture" % args.input)

    if args.size:
        w, h = (int(v) for v in args.size.lower().split("x"))
    elif args.kind == "canvas":
        w, h = 480, 480
    elif args.kind == "clip":
        w, h = PANEL_W, PANEL_H
    else:
        sw, sh = int(vs.get("width", 16)), int(vs.get("height", 9))
        w = PANEL_W
        h = max(2, int(round(PANEL_W * sh / max(1, sw))) // 2 * 2)
        if h > PANEL_H:
            w, h = max(2, int(round(PANEL_H * sw / max(1, sh))) // 2 * 2), PANEL_H
    mode = args.mode or ("fill" if args.kind == "canvas" else "fit")

    if args.kind == "video":
        if not args.track:
            sys.exit("a music video needs --track SONG")
        base = os.path.splitext(args.track)[0]
        out = args.out or base + ".video.mp4"
    elif args.kind == "canvas":
        base = os.path.splitext(args.track or args.input)[0]
        out = args.out or (base + ".canvas.mp4" if args.track
                           else os.path.splitext(args.input)[0] + ".canvas.mp4")
    else:
        out = args.out or os.path.splitext(args.input)[0] + ".r1.mp4"

    print("%s -> %s  %dx%d %g fps %s" % (args.input, out, w, h, args.fps, mode))
    if not args.dry_run:
        encode(args.input, out, w, h, args.fps, args.crf, args.bitrate,
               mode, args.gop, args.keep_audio and args.kind == "clip")

    if args.kind != "video":
        return

    offset, conf, rate = args.offset_ms, None, 1.0
    if offset is None:
        if not stream(info, "audio"):
            print("the video has no sound to line up; offset 0")
            offset = 0
        else:
            offset, conf = find_offset(args.track, args.input)
            if offset is None:
                print("could not compare the two; offset 0")
                offset = 0
            else:
                print("offset %d ms (confidence %.2f)" % (offset, conf))
                if conf < 0.3:
                    print("  low confidence: check it by ear, or pass "
                          "--offset-ms")
    if args.check_drift and stream(info, "audio"):
        dur = min(duration(probe(args.track)), duration(info))
        span = max(10.0, dur - 60.0)
        near, c1 = find_offset(args.track, args.input, 5.0, 25.0)
        far, c2 = find_offset(args.track, args.input, 5.0 + span, 25.0)
        if near is not None and far is not None and min(c1, c2) >= 0.2:
            rate = 1.0 + (far - near) / (span * 1000.0)
            print("drift: %+d ms over %.0f s -> rate %.6g"
                  % (far - near, span, rate))
        else:
            print("drift: the ends do not match well enough; rate 1.0")

    cfg = os.path.splitext(out)[0] + ".cfg"
    print("%s: offset %d ms, rate %.6g" % (cfg, offset, rate))
    if not args.dry_run:
        write_cfg(cfg, offset, rate, 0, 0, args.outside, conf)


if __name__ == "__main__":
    main()
