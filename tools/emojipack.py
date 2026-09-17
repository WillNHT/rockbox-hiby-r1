#!/usr/bin/env python3
"""Build the colour emoji pack Rockbox reads from /.rockbox/emoji/emoji.rbe.

    emojipack.py [--size 36] [--keep-tones] PNG_DIR OUT.rbe

PNG_DIR holds one PNG per emoji, named after its codepoints the way the
common sets name them:

    Noto Emoji   emoji_u1f600.png, emoji_u1f468_200d_1f469.png
    Twemoji      1f600.png, 1f468-200d-1f469.png

Variation selector 16 (fe0f) is dropped from every name: text may or may
not carry it and the firmware looks emoji up without it. Skin tone
variants are left out unless --keep-tones is given; the firmware then
draws the plain emoji for a toned one, which keeps the pack about half
the size.

The format is described in apps/emoji.c. Needs Pillow.
"""
import argparse
import os
import re
import struct
import sys

from PIL import Image

MAGIC = b"RBEMOJI1"
SEQ = 10
ENTRY = 1 + 3 * SEQ + 1
HEADER = 24

NAME = re.compile(r"^(?:emoji_u)?([0-9a-fA-F]+(?:[_-][0-9a-fA-F]+)*)\.png$")


def parse(name):
    m = NAME.match(name)
    if not m:
        return None
    cps = [int(x, 16) for x in re.split(r"[_-]", m.group(1))]
    cps = [c for c in cps if c != 0xFE0F]
    if not cps or len(cps) > SEQ or any(c > 0x10FFFF for c in cps):
        return None
    return tuple(cps)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def picture(path, size):
    img = Image.open(path).convert("RGBA")
    # Fit inside the square, keeping the aspect (flags are wider than tall).
    w, h = img.size
    scale = size / max(w, h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(img, ((size - nw) // 2, (size - nh) // 2))
    raw = canvas.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        if a == 0:
            r = g = b = 0
        out += struct.pack(">HB", rgb565(r, g, b), a)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=36,
                    help="picture size in pixels (default 36, at most 128)")
    ap.add_argument("--keep-tones", action="store_true",
                    help="keep skin tone variants")
    ap.add_argument("src")
    ap.add_argument("out")
    args = ap.parse_args()
    if not 8 <= args.size <= 128:
        sys.exit("size must be 8..128")

    found = {}
    for name in os.listdir(args.src):
        key = parse(name)
        if key is None:
            continue
        if not args.keep_tones and any(0x1F3FB <= c <= 0x1F3FF for c in key):
            continue
        # With fe0f dropped two files can share a key; either picture will do.
        found.setdefault(key, os.path.join(args.src, name))

    keys = sorted(found)
    if not keys:
        sys.exit("no emoji PNGs in " + args.src)

    index = bytearray()
    for key in keys:
        entry = bytearray([len(key)])
        for c in key:
            entry += bytes([c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF])
        entry += bytes(ENTRY - len(entry))
        index += entry

    index_off = HEADER
    data_off = index_off + len(index)
    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHIII", args.size, 0, len(keys), index_off, data_off))
        f.write(index)
        for key in keys:
            f.write(picture(found[key], args.size))

    print("%s: %d emoji at %d px, %d KiB" %
          (args.out, len(keys), args.size, os.path.getsize(args.out) // 1024))


if __name__ == "__main__":
    main()
