#!/usr/bin/env python3
"""Car sprite, take 2. Proportions matched to the atlas cell (the car fills the
frame edge-to-edge), shading in flat bands, and the shadow faked with a
checkerboard dither -- because Canvas alpha is 2-valued (0 or 255), which is
also how the original art does it.

Renders a 6x preview next to the original for a proportion check.
"""

import math
import struct
import zlib

from PIL import Image


class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h * 4)

    def set(self, x, y, c):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        i = (int(y) * self.w + int(x)) * 4
        self.px[i : i + 4] = bytes((c[0], c[1], c[2], 255))

    def rect(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, c)

    # Horizontal-edged trapezoid: the shape everything in this art is made of.
    def trap(self, y0, xl0, xr0, y1, xl1, xr1, c):
        y0, y1 = int(y0), int(y1)
        if y1 <= y0:
            return
        for y in range(y0, y1):
            t = (y - y0) / (y1 - y0)
            xl = xl0 + (xl1 - xl0) * t
            xr = xr0 + (xr1 - xr0) * t
            for x in range(int(round(xl)), int(round(xr))):
                self.set(x, y, c)

    def dither(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                if (xx + yy) % 2 == 0:
                    self.set(xx, yy, c)

    def write(self, path):
        raw = bytearray()
        st = self.w * 4
        for y in range(self.h):
            raw.append(0)
            raw += self.px[y * st : (y + 1) * st]

        def ch(tag, data):
            return (
                struct.pack(">I", len(data))
                + tag
                + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
            )

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(ch(b"IHDR", struct.pack(">2I5B", self.w, self.h, 8, 6, 0, 0, 0)))
            f.write(ch(b"IDAT", zlib.compress(bytes(raw), 9)))
            f.write(ch(b"IEND", b""))


def sh(c, k):
    return tuple(min(255, int(v * k)) for v in c[:3])


def car(w, h, body, roof_cut=0.06):
    """Rear three-quarter-off view of a car, filling the cell.

    Bands, top to bottom: cabin + rear window / rear deck / rear panel with
    lights and plate / bumper / tyres + dithered ground shadow.
    """
    im = Img(w, h)
    dark = sh(body, 0.55)
    darker = sh(body, 0.38)
    light = sh(body, 1.22)
    glass = (150, 186, 214)
    glass_d = (96, 130, 164)
    black = (26, 24, 28)
    lamp = (196, 32, 34)
    lamp_h = (240, 86, 74)
    plate = (208, 206, 198)

    top = int(h * roof_cut)  # roof is cropped by the cell, like the original
    shadow_h = max(3, int(h * 0.10))
    tyre_h = max(4, int(h * 0.13))
    ground = h - shadow_h
    bumper = ground - tyre_h
    panel = int(h * 0.52)
    deck = int(h * 0.40)

    # tyres poke out at the lower corners
    im.rect(1, bumper - 2, int(w * 0.16), tyre_h + 2, black)
    im.rect(w - 1 - int(w * 0.16), bumper - 2, int(w * 0.16), tyre_h + 2, black)

    # cabin: narrow at the roof, flaring to the shoulders
    im.trap(top, w * 0.20, w * 0.80, deck, w * 0.06, w * 0.94, light)
    # rear window, inset in the cabin
    im.trap(top + 2, w * 0.24, w * 0.76, deck - 3, w * 0.13, w * 0.87, glass_d)
    im.trap(top + 2, w * 0.255, w * 0.62, deck - 4, w * 0.145, w * 0.60, glass)
    # rear deck (the lid): a thin bright band under the window
    im.trap(deck, w * 0.055, w * 0.945, panel, w * 0.035, w * 0.965, body)
    # rear panel: the dark face of the car
    im.trap(panel, w * 0.035, w * 0.965, bumper, w * 0.02, w * 0.98, dark)
    im.trap(panel + 1, w * 0.09, w * 0.91, bumper - 2, w * 0.075, w * 0.925, black)

    # tail lights: two blocks each side, like the original's layout
    ly = panel + max(2, int(h * 0.06))
    lh = max(3, int(h * 0.13))
    lw = max(3, int(w * 0.085))
    for lx in (w * 0.10, w * 0.10 + lw + 2):
        im.rect(lx, ly, lw, lh, lamp)
        im.rect(lx, ly, lw, 2, lamp_h)
    for lx in (w * 0.90 - lw, w * 0.90 - 2 * lw - 2):
        im.rect(lx, ly, lw, lh, lamp)
        im.rect(lx, ly, lw, 2, lamp_h)
    # plate, centred low on the panel
    pw, ph = int(w * 0.26), max(3, int(h * 0.11))
    im.rect((w - pw) / 2, bumper - ph - 2, pw, ph, plate)

    # bumper
    im.trap(bumper, w * 0.03, w * 0.97, ground, w * 0.045, w * 0.955, darker)
    # ground shadow: checkerboard dither, the original's transparency trick
    im.dither(w * 0.06, ground, w * 0.88, shadow_h - 1, black)
    return im


def truck(w=100, h=78):
    im = Img(w, h)
    body = (206, 202, 196)
    box = (176, 60, 56)
    return_box = sh(box, 0.6)
    black = (26, 24, 28)
    ground = h - 6
    im.rect(2, ground - 12, int(w * 0.18), 12, black)
    im.rect(w - 2 - int(w * 0.18), ground - 12, int(w * 0.18), 12, black)
    im.trap(int(h * 0.05), w * 0.10, w * 0.90, ground - 10, w * 0.05, w * 0.95, box)
    im.trap(int(h * 0.09), w * 0.16, w * 0.84, int(h * 0.62), w * 0.12, w * 0.88, return_box)
    im.trap(ground - 12, w * 0.05, w * 0.95, ground, w * 0.07, w * 0.93, sh(body, 0.5))
    im.rect(w * 0.10, ground - 20, w * 0.10, 6, (196, 32, 34))
    im.rect(w * 0.80, ground - 20, w * 0.10, 6, (196, 32, 34))
    im.dither(w * 0.08, ground, w * 0.84, 5, black)
    return im


def preview():
    cars = [
        car(80, 56, (44, 78, 190)),      # CAR01
        car(80, 59, (196, 52, 44)),      # CAR02
        car(88, 55, (232, 176, 40)),     # CAR03
        car(80, 57, (40, 152, 96)),      # CAR04
        car(80, 41, (216, 216, 220), roof_cut=0.02),  # PLAYER
        truck(),
    ]
    for i, c in enumerate(cars):
        c.write(f"car{i}.png")

    S = 6
    pad = 12
    tiles = []
    orig = Image.open(
        "/Users/yuji/Projects/javascript-racer/images/sprites/car01.png"
    ).convert("RGBA")
    tiles.append(orig.resize((orig.width * S, orig.height * S), Image.NEAREST))
    for i in range(len(cars)):
        im = Image.open(f"car{i}.png").convert("RGBA")
        tiles.append(im.resize((im.width * S, im.height * S), Image.NEAREST))

    W = sum(t.width for t in tiles) + pad * (len(tiles) + 1)
    H = max(t.height for t in tiles) + pad * 2
    sheet = Image.new("RGBA", (W, H), (28, 30, 34, 255))
    x = pad
    for t in tiles:
        sheet.alpha_composite(t, (x, pad + (H - pad * 2 - t.height)))
        x += t.width + pad
    sheet.save("cars_preview.png")
    print("wrote cars_preview.png", sheet.size)


preview()
