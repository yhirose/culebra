#!/usr/bin/env python3
"""Pastel palette swatch + a mock game frame, for signing off on the colour
direction before drawing all 34 sprites.

Everything here is stdlib-only and written so the palette and the Pen/sprite
code lift straight into examples/games/racer/tools/gen_assets.py. PIL is used
only to assemble the human-facing preview.
"""

import math
import struct
import zlib

from PIL import Image

# ----------------------------------------------------------------- palette ---
# Soft, warm, bright pastel. Saturation stays low and value high; greens lean
# sage/olive, blues lean periwinkle, the horizon goes cream->peach. There is no
# black anywhere -- outlines use INK (a warm plum) so edges read soft.

INK = (120, 98, 106)
INK2 = (154, 132, 138)

SKY_TOP = (144, 194, 232)
SKY_LOW = (250, 219, 191)
CLOUD = (253, 242, 233)
SUN = (253, 222, 168)

HILL_FAR = (176, 206, 179)
HILL_NEAR = (145, 188, 151)
TREELINE = (124, 170, 130)

LEAF_D = (127, 172, 118)
LEAF_M = (160, 198, 130)
LEAF_L = (198, 222, 151)
WOOD_D = (164, 121, 92)
WOOD_M = (192, 151, 113)
WOOD_L = (216, 183, 145)

# road constants (these live in racer.cul, not in the PNG)
ROAD_L = (194, 185, 202)
ROAD_D = (180, 173, 192)
GRASS_L = (177, 216, 145)
GRASS_D = (163, 204, 133)
RUMBLE_L = (248, 180, 165)
RUMBLE_D = (242, 228, 210)
LANE = (252, 244, 231)
FOG = (210, 228, 200)

CORAL = (244, 140, 126)
CORAL_D = (216, 107, 99)
CORAL_L = (252, 186, 172)
GLASS = (198, 225, 238)
GLASS_D = (163, 197, 218)
LAMP = (240, 134, 119)
LAMP_L = (250, 190, 175)
TYRE = (144, 126, 137)
TYRE_D = (120, 103, 117)
CREAM = (250, 236, 213)
PLATE = (238, 228, 207)
SHADOW = (198, 176, 183)

PALETTE_ROWS = [
    ("sky / horizon", [SKY_TOP, (206, 224, 236), SKY_LOW, CLOUD, SUN]),
    ("hills / trees", [HILL_FAR, HILL_NEAR, TREELINE, LEAF_D, LEAF_M, LEAF_L]),
    ("wood / ink", [WOOD_D, WOOD_M, WOOD_L, INK, INK2]),
    ("road (racer.cul)", [GRASS_D, GRASS_L, ROAD_D, ROAD_L, RUMBLE_L, RUMBLE_D, LANE, FOG]),
    ("vehicle", [CORAL_D, CORAL, CORAL_L, GLASS_D, GLASS, LAMP, CREAM, TYRE, SHADOW]),
]


# -------------------------------------------------------------------- image ---


class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h * 4)

    def set(self, x, y, c):
        x, y = int(x), int(y)
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        i = (y * self.w + x) * 4
        self.px[i : i + 4] = bytes((c[0], c[1], c[2], 255))

    def get(self, x, y):
        i = (int(y) * self.w + int(x)) * 4
        return tuple(self.px[i : i + 4])

    def rect(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, c)

    def span(self, y, x0, x1, c):
        for x in range(int(round(x0)), int(round(x1))):
            self.set(x, y, c)

    def blit_scaled(self, src, dx, dy, dw, dh):
        """The integer nearest-neighbour mapping requirement A' specifies."""
        if dw <= 0 or dh <= 0:
            return
        for y in range(max(0, dy), min(self.h, dy + dh)):
            j = (y - dy) * src.h // dh
            for x in range(max(0, dx), min(self.w, dx + dw)):
                i = (x - dx) * src.w // dw
                p = src.get(i, j)
                if p[3]:
                    self.set(x, y, p)

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


class Pen:
    """Draws on a coarse virtual-pixel grid, so output reads as dot art even in
    a 215x540 cell. Alpha is 2-valued: a cell is either a colour or empty."""

    def __init__(self, w, h, p):
        self.w, self.h, self.p = w, h, p
        self.gw = -(-w // p)
        self.gh = -(-h // p)
        self.g = [None] * (self.gw * self.gh)

    def s(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < self.gw and 0 <= y < self.gh:
            self.g[y * self.gw + x] = c

    def at(self, x, y):
        if 0 <= x < self.gw and 0 <= y < self.gh:
            return self.g[int(y) * self.gw + int(x)]
        return None

    def rect(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.s(xx, yy, c)

    def ell(self, cx, cy, rx, ry, c):
        for yy in range(int(cy - ry), int(cy + ry) + 1):
            for xx in range(int(cx - rx), int(cx + rx) + 1):
                dx, dy = (xx - cx) / max(rx, 0.01), (yy - cy) / max(ry, 0.01)
                if dx * dx + dy * dy <= 1.0:
                    self.s(xx, yy, c)

    def trap(self, y0, xl0, xr0, y1, xl1, xr1, c):
        y0, y1 = int(y0), int(y1)
        if y1 <= y0:
            return
        for y in range(y0, y1):
            t = (y - y0) / (y1 - y0)
            for x in range(int(round(xl0 + (xl1 - xl0) * t)), int(round(xr0 + (xr1 - xr0) * t))):
                self.s(x, y, c)

    def poly(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            for i in range(len(pts)):
                x1, y1 = pts[i]
                x2, y2 = pts[(i + 1) % len(pts)]
                if (y1 <= y < y2) or (y2 <= y < y1):
                    xs.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(xs[i]), int(xs[i + 1]) + 1):
                    self.s(x, y, c)

    def dither(self, x, y, w, h, c, phase=0):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                if (xx + yy + phase) % 2 == 0:
                    self.s(xx, yy, c)

    def outline_in(self, c=INK):
        """Recolour filled cells that touch empty space -- an inline outline, so
        the silhouette (and therefore the sprite's effective size) is unchanged."""
        edge = []
        for y in range(self.gh):
            for x in range(self.gw):
                if self.at(x, y) is None:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    inside = 0 <= nx < self.gw and 0 <= ny < self.gh
                    if inside and self.at(nx, ny) is None:
                        edge.append((x, y))
                        break
        for x, y in edge:
            self.s(x, y, c)

    def img(self):
        im = Img(self.w, self.h)
        for gy in range(self.gh):
            for gx in range(self.gw):
                c = self.g[gy * self.gw + gx]
                if c is None:
                    continue
                im.rect(gx * self.p, gy * self.p, self.p, self.p, c)
        return im


# ------------------------------------------------------------------ sprites ---


def palm_tree(w=215, h=540, p=5):
    pen = Pen(w, h, p)
    gw, gh = pen.gw, pen.gh
    cx = gw / 2
    crown_y = gh * 0.36
    # trunk: gentle S, thicker at the base, two tones
    for gy in range(int(crown_y), gh):
        t = (gy - crown_y) / (gh - crown_y)
        bend = math.sin(t * 2.0) * (gw * 0.09)
        tw = 2.0 + t * 3.4
        x0 = cx + bend - tw / 2
        for gx in range(int(x0), int(x0 + tw) + 1):
            pen.s(gx, gy, WOOD_M if gx < x0 + tw * 0.55 else WOOD_D)
    top_x, top_y = cx, crown_y
    fronds = [
        (-1.52, 0.60, LEAF_D), (1.52, 0.60, LEAF_D),
        (-1.12, 0.56, LEAF_M), (1.12, 0.56, LEAF_M),
        (-0.66, 0.48, LEAF_L), (0.68, 0.48, LEAF_L),
        (-0.14, 0.40, LEAF_M), (0.24, 0.40, LEAF_M),
    ]
    for ang, ln, col in fronds:
        n = 8
        ribs = []
        for i in range(n + 1):
            t = i / n
            r = ln * gh * t
            x = top_x + math.cos(ang - math.pi / 2) * r * 1.45
            y = top_y + math.sin(ang - math.pi / 2) * r + t * t * ln * gh * 0.6
            ribs.append((x, y))
        thick = [0.7 + (1 - abs(i / n - 0.35)) * 3.2 for i in range(n + 1)]
        pts = [(x, y - thick[i]) for i, (x, y) in enumerate(ribs)]
        pts += [(x, y + thick[i]) for i, (x, y) in reversed(list(enumerate(ribs)))]
        pen.poly(pts, col)
    for dx, dy in ((-2, 2), (1, 3), (0, 4)):
        pen.ell(top_x + dx, top_y + dy, 1.2, 1.2, WOOD_L)
    pen.outline_in()
    return pen.img()


def car_ferrari(w=80, h=56, p=2, body=CORAL, body_d=CORAL_D, body_l=CORAL_L):
    """Low, wide, wedge-nosed -- a modern mid-engine silhouette from behind."""
    pen = Pen(w, h, p)
    gw, gh = pen.gw, pen.gh
    shadow_h = 2
    ground = gh - shadow_h
    tyre_top = ground - 4
    panel = int(gh * 0.50)
    deck = int(gh * 0.34)
    wing = int(gh * 0.16)

    pen.rect(gw * 0.06, tyre_top, gw * 0.13, ground - tyre_top, TYRE)
    pen.rect(gw * 0.81, tyre_top, gw * 0.13, ground - tyre_top, TYRE)
    # rear wing on two stalks
    pen.rect(gw * 0.16, wing, gw * 0.68, 1.6, body_l)
    pen.rect(gw * 0.30, wing + 1, 1.4, deck - wing, body_d)
    pen.rect(gw * 0.68, wing + 1, 1.4, deck - wing, body_d)
    # cabin glass under the wing
    pen.trap(wing + 2, gw * 0.30, gw * 0.70, deck, gw * 0.24, gw * 0.76, GLASS_D)
    pen.trap(wing + 2, gw * 0.31, gw * 0.55, deck - 1, gw * 0.25, gw * 0.53, GLASS)
    # rear deck, then the wide haunches
    pen.trap(deck, gw * 0.14, gw * 0.86, panel, gw * 0.05, gw * 0.95, body)
    pen.trap(deck, gw * 0.14, gw * 0.30, panel, gw * 0.05, gw * 0.22, body_l)
    # tail panel
    pen.trap(panel, gw * 0.05, gw * 0.95, tyre_top, gw * 0.03, gw * 0.97, body_d)
    ly = panel + 1
    for lx in (gw * 0.10, gw * 0.22, gw * 0.66, gw * 0.78):
        pen.rect(lx, ly, gw * 0.10, 2, LAMP)
        pen.rect(lx, ly, gw * 0.10, 1, LAMP_L)
    # diffuser + plate
    pen.trap(tyre_top, gw * 0.10, gw * 0.90, ground, gw * 0.12, gw * 0.88, INK2)
    pen.rect(gw * 0.40, tyre_top - 2, gw * 0.20, 2, PLATE)
    pen.dither(gw * 0.06, ground, gw * 0.88, shadow_h, SHADOW)
    pen.outline_in()
    return pen.img()


# ----------------------------------------------------------------- mock frame ---


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def mock_frame(W=480, H=270):
    im = Img(W, H)
    horizon = int(H * 0.40)

    # --- background: sky gradient, then two hill bands and a treeline -------
    for y in range(horizon + 10):
        im.span(y, 0, W, lerp(SKY_TOP, SKY_LOW, (y / (horizon + 10)) ** 1.3))
    for x in range(W):
        u = x / W
        yy = horizon - 16 + math.sin(2 * math.pi * u) * 9 + math.sin(2 * math.pi * 3 * u + 0.7) * 4
        for y in range(int(yy), horizon + 2):
            im.set(x, y, HILL_FAR)
    for x in range(W):
        u = x / W
        yy = horizon - 8 + math.sin(2 * math.pi * 2 * u + 2.1) * 6 + math.sin(2 * math.pi * 5 * u) * 3
        for y in range(int(yy), horizon + 2):
            im.set(x, y, HILL_NEAR)
    for x in range(W):
        u = x / W
        yy = horizon - 4 + math.sin(2 * math.pi * 7 * u) * 2
        for y in range(int(yy), horizon + 1):
            im.set(x, y, TREELINE)

    # --- road: one span pair per screen row, fogged with distance ----------
    # t is proportional to 1/z, so widths are linear in t and the stripe phase
    # (which uses 1/t) bunches up near the horizon, as in the real projection.
    lanes = 3
    curve = 0.55
    for y in range(horizon, H):
        t = (y - horizon + 1) / (H - horizon)
        z = 1.0 / t
        fog = math.exp(-((1 - t) ** 2) * 2.4)
        cx = W / 2 + curve * (1 - t) ** 2 * W * 0.55
        hw = W * 0.56 * t
        rw = hw / max(6, 2 * lanes)
        lw = hw / max(32, 8 * lanes)
        dark = int(z * 2.2) % 2 == 0
        road = ROAD_D if dark else ROAD_L
        grass = GRASS_D if dark else GRASS_L
        rumble = RUMBLE_L if dark else RUMBLE_D
        im.span(y, 0, W, lerp(grass, FOG, 1 - fog))
        im.span(y, cx - hw - rw, cx + hw + rw, lerp(rumble, FOG, 1 - fog))
        im.span(y, cx - hw, cx + hw, lerp(road, FOG, 1 - fog))
        if not dark:
            for i in range(1, lanes):
                lx = cx - hw + (2 * hw) * i / lanes
                im.span(y, lx - lw / 2, lx + lw / 2, lerp(LANE, FOG, 1 - fog))

    # --- scenery: same integer nearest-neighbour scaling A' will use -------
    palm = palm_tree()
    for t, side in ((0.22, -1), (0.30, -1), (0.46, 1), (0.72, -1), (1.0, 1)):
        cx = W / 2 + curve * (1 - t) ** 2 * W * 0.55
        hw = W * 0.56 * t
        dw = max(2, int(palm.w * t * 0.72))
        dh = max(2, int(palm.h * t * 0.72))
        x = int(cx + side * (hw * 1.9)) - dw // 2
        y = int(horizon + (H - horizon) * t) - dh
        im.blit_scaled(palm, x, y, dw, dh)

    car = car_ferrari()
    dw, dh = car.w * 2, car.h * 2
    im.blit_scaled(car, (W - dw) // 2, H - dh - 6, dw, dh)
    return im


# -------------------------------------------------------------------- sheet ---


def main():
    mock = mock_frame()
    mock.write("mock.png")

    palm = palm_tree()
    palm.write("palm.png")
    car = car_ferrari()
    car.write("car.png")

    # assemble a preview: palette bands on top, mock frame + sprites below
    SW, GAP = 44, 6
    rows = len(PALETTE_ROWS)
    pal_h = rows * (SW + GAP) + GAP
    mock_im = Image.open("mock.png").resize((mock.w * 2, mock.h * 2), Image.NEAREST)
    palm_im = Image.open("palm.png")
    car_im = Image.open("car.png").resize((car.w * 3, car.h * 3), Image.NEAREST)

    W = max(mock_im.width + GAP * 2, 9 * (SW + GAP) + GAP)
    H = pal_h + mock_im.height + GAP * 3 + palm_im.height + GAP
    sheet = Image.new("RGBA", (W, H), (244, 240, 236, 255))

    y = GAP
    for _, colors in PALETTE_ROWS:
        x = GAP
        for c in colors:
            sheet.paste(c + (255,), (x, y, x + SW, y + SW))
            x += SW + GAP
        y += SW + GAP

    sheet.alpha_composite(mock_im, (GAP, y))
    y2 = y + mock_im.height + GAP
    sheet.alpha_composite(palm_im, (GAP, y2))
    sheet.alpha_composite(car_im, (GAP + palm_im.width + GAP * 3, y2 + 40))
    sheet.save("swatch.png")
    print("wrote swatch.png", sheet.size)


main()
