#!/usr/bin/env python3
"""Generate outline templates a designer can draw pixel art into.

Draws a 1px black rectangle around every sprite / background-layer atlas
rectangle, on an otherwise transparent canvas the same size as the real
sheet. SPRITES / BACKGROUND in gen_assets.py are the single source of truth
for those rectangles; this script only borders them, never redraws them.

Usage:  python3 gen_templates.py [outdir]      (default ../assets)
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_assets import SPRITES, SPRITE_SIZE, BACKGROUND, BG_SIZE, write_png

BLACK = bytes((0, 0, 0, 255))


def outline_rects(w, h, rects):
    out = bytearray(w * h * 4)
    stride = w * 4

    def put(x, y):
        if 0 <= x < w and 0 <= y < h:
            i = y * stride + x * 4
            out[i:i + 4] = BLACK

    for x, y, rw, rh in rects:
        for xx in range(x, x + rw):
            put(xx, y)
            put(xx, y + rh - 1)
        for yy in range(y, y + rh):
            put(x, yy)
            put(x + rw - 1, yy)
    return bytes(out)


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    written = []

    w, h = SPRITE_SIZE
    p = os.path.join(outdir, "sprites-template.png")
    written.append((p, write_png(p, w, h, outline_rects(w, h, SPRITES.values()))))

    w, h = BG_SIZE
    p = os.path.join(outdir, "background-template.png")
    written.append((p, write_png(p, w, h, outline_rects(w, h, BACKGROUND.values()))))

    for path, n in written:
        print("%-44s %8d bytes" % (os.path.relpath(path), n))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets"))
