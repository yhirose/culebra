#!/usr/bin/env python3
"""Four follow-up candidates, all starting from C's saturation (+85%). The
complaint was "the pale end is too pale" -- two independent fixes, crossed:

  narrow: pull each colour's light variant toward its dark sibling (in S and
          V), so the light end reads less washed-out without touching the
          already-good dark end.
  fog:    cap how far distance fade can wash a colour toward FOG, instead of
          letting it approach pure FOG colour at the horizon.

E = narrow only   F = fog-cap only   G = both (moderate)   H = both (strong)
"""

import colorsys
import math
import struct
import zlib

from PIL import Image, ImageDraw

BASE = dict(
    INK=(120, 98, 106), INK2=(154, 132, 138),
    SKY_TOP=(172, 206, 232), SKY_MID=(206, 224, 236), SKY_LOW=(250, 230, 212),
    CLOUD=(253, 248, 244), SUN=(253, 232, 196),
    HILL_FAR=(188, 206, 190), HILL_NEAR=(160, 188, 164), TREELINE=(140, 170, 144),
    LEAF_D=(142, 172, 136), LEAF_M=(172, 198, 152), LEAF_L=(206, 222, 174),
    WOOD_D=(164, 134, 114), WOOD_M=(192, 164, 138), WOOD_L=(216, 194, 168),
    ROAD_L=(198, 194, 202), ROAD_D=(186, 182, 192),
    GRASS_L=(190, 216, 168), GRASS_D=(176, 204, 156),
    RUMBLE_L=(248, 202, 192), RUMBLE_D=(242, 234, 224),
    LANE=(252, 248, 242), FOG=(218, 228, 212),
    CORAL=(244, 172, 162), CORAL_D=(216, 140, 134), CORAL_L=(252, 208, 198),
    GLASS=(214, 230, 238), GLASS_D=(182, 204, 218),
    LAMP=(240, 166, 156), LAMP_L=(250, 210, 200),
    TYRE=(144, 134, 140), TYRE_D=(120, 110, 118),
    CREAM=(250, 242, 228), PLATE=(238, 232, 220), SHADOW=(198, 186, 190),
)
NO_BOOST = {"INK", "INK2", "CLOUD", "LANE", "CREAM", "PLATE"}
SAT_K = 0.85  # candidate C's level, fixed for all 4 here

# (dark_key, light_key) pairs -- pull light toward dark by `narrow` fraction
PAIRS = [
    ("SKY_TOP", "SKY_MID"), ("SKY_MID", "SKY_LOW"),
    ("HILL_NEAR", "HILL_FAR"),
    ("LEAF_D", "LEAF_M"), ("LEAF_M", "LEAF_L"),
    ("WOOD_M", "WOOD_L"),
    ("GRASS_D", "GRASS_L"),
    ("ROAD_D", "ROAD_L"),
    ("RUMBLE_D", "RUMBLE_L"),
    ("CORAL_D", "CORAL"), ("CORAL", "CORAL_L"),
    ("GLASS_D", "GLASS"),
    ("LAMP", "LAMP_L"),
]

CANDIDATES = [
    ("E  narrow 35%", dict(narrow=0.35, fog_floor=0.0)),
    ("F  fog-cap only", dict(narrow=0.0, fog_floor=0.45)),
    ("G  narrow 30% + fog-cap", dict(narrow=0.30, fog_floor=0.35)),
    ("H  narrow 50% + fog-cap+", dict(narrow=0.50, fog_floor=0.55)),
]


def hboost(c, k):
    r, g, b = c
    h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    s = min(1.0, s * (1 + k) + 0.02)
    r2, g2, b2 = colorsys.hsv_to_rgb(h, s, v)
    return (r2 * 255, g2 * 255, b2 * 255)


def narrow_toward(light_c, dark_c, amount):
    """Pull light_c's S and V toward dark_c's, keep light_c's hue."""
    lh, ls, lv = colorsys.rgb_to_hsv(*(x / 255 for x in light_c))
    _, ds, dv = colorsys.rgb_to_hsv(*(x / 255 for x in dark_c))
    ns = ls + (ds - ls) * amount
    nv = lv + (dv - lv) * amount * 0.6  # value less aggressively (avoid muddying)
    r, g, b = colorsys.hsv_to_rgb(lh, min(1.0, ns), nv)
    return (round(r * 255), round(g * 255), round(b * 255))


def palette_for(narrow):
    pal = {name: (tuple(round(x) for x in c) if name in NO_BOOST else tuple(round(x) for x in hboost(c, SAT_K)))
           for name, c in BASE.items()}
    if narrow > 0:
        for dark_key, light_key in PAIRS:
            pal[light_key] = narrow_toward(pal[light_key], pal[dark_key], narrow)
    return pal


ROW_KEYS = [
    ["SKY_TOP", "SKY_MID", "SKY_LOW", "SUN"],
    ["HILL_FAR", "HILL_NEAR", "TREELINE", "LEAF_D", "LEAF_M", "LEAF_L"],
    ["WOOD_D", "WOOD_M", "WOOD_L"],
    ["GRASS_D", "GRASS_L", "ROAD_D", "ROAD_L", "RUMBLE_L", "RUMBLE_D", "FOG"],
    ["CORAL_D", "CORAL", "CORAL_L", "GLASS_D", "GLASS", "LAMP", "TYRE", "SHADOW"],
]


class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h * 4)

    def set(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.px[i : i + 4] = bytes((c[0], c[1], c[2], 255))

    def rect(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, c)


class Pen:
    def __init__(self, w, h, p):
        self.w, self.h, self.p = w, h, p
        self.gw, self.gh = -(-w // p), -(-h // p)
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

    def outline_in(self, c):
        edge = []
        for y in range(self.gh):
            for x in range(self.gw):
                if self.at(x, y) is None:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if not (0 <= nx < self.gw and 0 <= ny < self.gh) or self.at(nx, ny) is None:
                        edge.append((x, y))
                        break
        for x, y in edge:
            self.s(x, y, c)

    def img(self):
        im = Img(self.w, self.h)
        for gy in range(self.gh):
            for gx in range(self.gw):
                c = self.g[gy * self.gw + gx]
                if c is not None:
                    im.rect(gx * self.p, gy * self.p, self.p, self.p, c)
        return im


def car_ferrari(pal, w=80, h=56, p=2):
    body, body_d, body_l = pal["CORAL"], pal["CORAL_D"], pal["CORAL_L"]
    pen = Pen(w, h, p)
    gw, gh = pen.gw, pen.gh
    ground = gh - 2
    tyre_top = ground - 4
    panel = int(gh * 0.50)
    deck = int(gh * 0.34)
    wing = int(gh * 0.16)
    pen.rect(gw * 0.06, tyre_top, gw * 0.13, ground - tyre_top, pal["TYRE"])
    pen.rect(gw * 0.81, tyre_top, gw * 0.13, ground - tyre_top, pal["TYRE"])
    pen.rect(gw * 0.16, wing, gw * 0.68, 1.6, body_l)
    pen.rect(gw * 0.30, wing + 1, 1.4, deck - wing, body_d)
    pen.rect(gw * 0.68, wing + 1, 1.4, deck - wing, body_d)
    pen.trap(wing + 2, gw * 0.30, gw * 0.70, deck, gw * 0.24, gw * 0.76, pal["GLASS_D"])
    pen.trap(wing + 2, gw * 0.31, gw * 0.55, deck - 1, gw * 0.25, gw * 0.53, pal["GLASS"])
    pen.trap(deck, gw * 0.14, gw * 0.86, panel, gw * 0.05, gw * 0.95, body)
    pen.trap(deck, gw * 0.14, gw * 0.30, panel, gw * 0.05, gw * 0.22, body_l)
    pen.trap(panel, gw * 0.05, gw * 0.95, tyre_top, gw * 0.03, gw * 0.97, body_d)
    ly = panel + 1
    for lx in (gw * 0.10, gw * 0.22, gw * 0.66, gw * 0.78):
        pen.rect(lx, ly, gw * 0.10, 2, pal["LAMP"])
        pen.rect(lx, ly, gw * 0.10, 1, pal["LAMP_L"])
    pen.trap(tyre_top, gw * 0.10, gw * 0.90, ground, gw * 0.12, gw * 0.88, pal["INK2"])
    pen.rect(gw * 0.40, tyre_top - 2, gw * 0.20, 2, pal["PLATE"])
    pen.dither(gw * 0.06, ground, gw * 0.06 + 2, 2, pal["SHADOW"])
    pen.outline_in(pal["INK"])
    return pen.img()


def palm_tree(pal, w=215, h=540, p=5):
    pen = Pen(w, h, p)
    gw, gh = pen.gw, pen.gh
    cx = gw / 2
    crown_y = gh * 0.36
    for gy in range(int(crown_y), gh):
        t = (gy - crown_y) / (gh - crown_y)
        bend = math.sin(t * 2.0) * (gw * 0.09)
        tw = 2.0 + t * 3.4
        x0 = cx + bend - tw / 2
        for gx in range(int(x0), int(x0 + tw) + 1):
            pen.s(gx, gy, pal["WOOD_M"] if gx < x0 + tw * 0.55 else pal["WOOD_D"])
    top_x, top_y = cx, crown_y
    fronds = [
        (-1.52, 0.60, pal["LEAF_D"]), (1.52, 0.60, pal["LEAF_D"]),
        (-1.12, 0.56, pal["LEAF_M"]), (1.12, 0.56, pal["LEAF_M"]),
        (-0.66, 0.48, pal["LEAF_L"]), (0.68, 0.48, pal["LEAF_L"]),
        (-0.14, 0.40, pal["LEAF_M"]), (0.24, 0.40, pal["LEAF_M"]),
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
    pen.outline_in(pal["INK"])
    return pen.img()


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def mock_strip(pal, fog_floor, W=320, H=180):
    im = Img(W, H)
    lanes = 3
    curve = 0.4
    for y in range(H):
        t = (y + 1) / H
        raw_fog = math.exp(-((1 - t) ** 2) * 2.0)
        fog = max(raw_fog, fog_floor)
        cx = W / 2 + curve * (1 - t) ** 2 * W * 0.5
        hw = W * 0.6 * t
        rw = hw / max(6, 2 * lanes)
        lw = hw / max(32, 8 * lanes)
        dark = int((1 / t) * 2.2) % 2 == 0
        road = pal["ROAD_D"] if dark else pal["ROAD_L"]
        grass = pal["GRASS_D"] if dark else pal["GRASS_L"]
        rumble = pal["RUMBLE_L"] if dark else pal["RUMBLE_D"]
        for x in range(W):
            c = grass
            if cx - hw - rw <= x < cx + hw + rw:
                c = rumble
            if cx - hw <= x < cx + hw:
                c = road
            if not dark:
                for i in range(1, lanes):
                    lx = cx - hw + (2 * hw) * i / lanes
                    if lx - lw / 2 <= x < lx + lw / 2:
                        c = pal["LANE"]
            im.set(x, y, lerp(c, pal["FOG"], 1 - fog))
    # skyline strip above road, faded with the same floor so the join reads
    for y in range(0, 0):
        pass
    car = car_ferrari(pal)
    dw, dh = car.w * 2, car.h * 2
    ox, oy = (W - dw) // 2, H - dh
    for y in range(car.h):
        for x in range(car.w):
            i = (y * car.w + x) * 4
            a = car.px[i + 3]
            if a:
                p = tuple(car.px[i : i + 4])
                for yy in range(2):
                    for xx in range(2):
                        im.set(ox + x * 2 + xx, oy + y * 2 + yy, p)
    return im


def main():
    SW, GAP = 30, 5
    row_h = SW + GAP
    strip_w, strip_h = 320, 180
    palm_h = 200

    card_w = max(9 * (SW + GAP), strip_w) + GAP * 2
    card_h = 26 + len(ROW_KEYS) * row_h + GAP * 2 + strip_h + GAP + palm_h + GAP * 2

    sheet = Image.new("RGBA", (card_w * 2 + GAP * 3, card_h * 2 + GAP * 3), (244, 240, 236, 255))

    for idx, (label, opts) in enumerate(CANDIDATES):
        pal = palette_for(opts["narrow"])
        card = Image.new("RGBA", (card_w, card_h), (250, 247, 244, 255))
        y = 26
        for keys in ROW_KEYS:
            x = GAP
            for key in keys:
                c = pal[key]
                for yy in range(y, y + SW):
                    for xx in range(x, x + SW):
                        card.putpixel((xx, yy), c + (255,))
                x += SW + GAP
            y += row_h
        y += GAP
        strip_im = mock_strip(pal, opts["fog_floor"], strip_w, strip_h)
        strip_png = Image.frombytes("RGBA", (strip_im.w, strip_im.h), bytes(strip_im.px))
        card.alpha_composite(strip_png, (GAP, y))
        y += strip_h + GAP
        palm = palm_tree(pal)
        palm_png = Image.frombytes("RGBA", (palm.w, palm.h), bytes(palm.px)).resize(
            (int(palm.w * palm_h / palm.h), palm_h)
        )
        card.alpha_composite(palm_png, (GAP, y))

        cx = (idx % 2) * (card_w + GAP) + GAP
        cy = (idx // 2) * (card_h + GAP) + GAP
        sheet.alpha_composite(card, (cx, cy))
        d = ImageDraw.Draw(sheet)
        d.rectangle([cx, cy, cx + card_w, cy + 22], fill=(120, 98, 106, 255))
        d.text((cx + 8, cy + 4), label, fill=(250, 247, 244, 255))

    sheet.save("swatch5.png")
    print("wrote swatch5.png", sheet.size)


main()
