#!/usr/bin/env python3
"""Four sunset-leaning colourways, all using G's treatment (saturation +85%,
gradient narrowed 30%, fog floor 0.35) -- only the base hues for sky / hills /
fog / grass / road shift toward dusk. The car and palm stay close to the
daytime palette so they read as the same asset set under different light.

I1 dusk pastel     -- closest to daytime G, warmed horizon
I2 golden hour      -- strong warm orange, still bright
I3 twilight violet  -- cooler, magenta/violet leaning, deeper
I4 apricot deep     -- richest orange-to-indigo gradient
"""

import colorsys
import math
import struct
import zlib

from PIL import Image, ImageDraw

# ---- shared G treatment -----------------------------------------------------
SAT_K = 0.85
NARROW = 0.30
FOG_FLOOR = 0.35
NO_BOOST = {"INK", "INK2", "CLOUD", "LANE", "CREAM", "PLATE"}
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

CAR_COMMON = dict(
    INK=(120, 98, 106), INK2=(154, 132, 138),
    CORAL=(244, 172, 162), CORAL_D=(216, 140, 134), CORAL_L=(252, 208, 198),
    GLASS=(214, 230, 238), GLASS_D=(182, 204, 218),
    LAMP=(240, 166, 156), LAMP_L=(250, 210, 200),
    TYRE=(144, 134, 140), TYRE_D=(120, 110, 118),
    CREAM=(250, 242, 228), PLATE=(238, 232, 220), SHADOW=(198, 186, 190),
    LANE=(252, 248, 242),
    WOOD_D=(164, 134, 114), WOOD_M=(192, 164, 138), WOOD_L=(216, 194, 168),
)

CANDIDATES = {
    "I1 dusk pastel": dict(
        SKY_TOP=(158, 156, 208), SKY_MID=(222, 172, 182), SKY_LOW=(255, 202, 158),
        CLOUD=(253, 248, 244), SUN=(255, 222, 172),
        HILL_FAR=(184, 156, 176), HILL_NEAR=(154, 124, 146), TREELINE=(124, 96, 118),
        LEAF_D=(142, 150, 120), LEAF_M=(172, 176, 138), LEAF_L=(206, 202, 162),
        GRASS_D=(174, 178, 122), GRASS_L=(192, 196, 146),
        ROAD_D=(172, 162, 182), ROAD_L=(188, 178, 198),
        RUMBLE_L=(248, 200, 190), RUMBLE_D=(236, 214, 210),
        FOG=(232, 194, 176),
    ),
    "I2 golden hour": dict(
        SKY_TOP=(182, 192, 222), SKY_MID=(242, 190, 150), SKY_LOW=(255, 178, 108),
        CLOUD=(255, 240, 220), SUN=(255, 210, 120),
        HILL_FAR=(202, 160, 128), HILL_NEAR=(172, 120, 88), TREELINE=(132, 82, 60),
        LEAF_D=(150, 148, 96), LEAF_M=(182, 178, 118), LEAF_L=(214, 204, 148),
        GRASS_D=(182, 170, 88), GRASS_L=(202, 190, 118),
        ROAD_D=(190, 164, 148), ROAD_L=(206, 184, 168),
        RUMBLE_L=(252, 188, 156), RUMBLE_D=(238, 208, 186),
        FOG=(250, 198, 148),
    ),
    "I3 twilight violet": dict(
        SKY_TOP=(96, 84, 148), SKY_MID=(174, 112, 162), SKY_LOW=(240, 152, 160),
        CLOUD=(244, 218, 226), SUN=(255, 190, 150),
        HILL_FAR=(142, 112, 152), HILL_NEAR=(102, 82, 122), TREELINE=(72, 56, 92),
        LEAF_D=(112, 108, 118), LEAF_M=(142, 134, 144), LEAF_L=(176, 162, 168),
        GRASS_D=(140, 128, 108), GRASS_L=(162, 150, 130),
        ROAD_D=(142, 120, 152), ROAD_L=(160, 140, 172),
        RUMBLE_L=(226, 160, 176), RUMBLE_D=(210, 178, 196),
        FOG=(202, 150, 172),
    ),
    "I4 apricot deep": dict(
        SKY_TOP=(70, 68, 122), SKY_MID=(202, 110, 112), SKY_LOW=(255, 150, 90),
        CLOUD=(250, 208, 190), SUN=(255, 200, 110),
        HILL_FAR=(152, 100, 90), HILL_NEAR=(112, 70, 60), TREELINE=(78, 48, 44),
        LEAF_D=(126, 100, 78), LEAF_M=(158, 128, 96), LEAF_L=(196, 160, 118),
        GRASS_D=(162, 140, 90), GRASS_L=(182, 160, 110),
        ROAD_D=(162, 128, 130), ROAD_L=(182, 150, 152),
        RUMBLE_L=(246, 158, 128), RUMBLE_D=(226, 178, 158),
        FOG=(242, 160, 120),
    ),
}


def hboost(c, k):
    r, g, b = c
    h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    s = min(1.0, s * (1 + k) + 0.02)
    r2, g2, b2 = colorsys.hsv_to_rgb(h, s, v)
    return (r2 * 255, g2 * 255, b2 * 255)


def narrow_toward(light_c, dark_c, amount):
    lh, ls, lv = colorsys.rgb_to_hsv(*(x / 255 for x in light_c))
    _, ds, dv = colorsys.rgb_to_hsv(*(x / 255 for x in dark_c))
    ns = ls + (ds - ls) * amount
    nv = lv + (dv - lv) * amount * 0.6
    r, g, b = colorsys.hsv_to_rgb(lh, min(1.0, ns), nv)
    return (round(r * 255), round(g * 255), round(b * 255))


def palette_for(scene):
    base = dict(CAR_COMMON)
    base.update(scene)
    pal = {name: (tuple(round(x) for x in c) if name in NO_BOOST else tuple(round(x) for x in hboost(c, SAT_K)))
           for name, c in base.items()}
    for dark_key, light_key in PAIRS:
        if dark_key in pal and light_key in pal:
            pal[light_key] = narrow_toward(pal[light_key], pal[dark_key], NARROW)
    return pal


ROW_KEYS = [
    ["SKY_TOP", "SKY_MID", "SKY_LOW", "SUN", "CLOUD"],
    ["HILL_FAR", "HILL_NEAR", "TREELINE", "LEAF_D", "LEAF_M", "LEAF_L"],
    ["GRASS_D", "GRASS_L", "ROAD_D", "ROAD_L", "RUMBLE_L", "RUMBLE_D", "FOG"],
    ["CORAL_D", "CORAL", "CORAL_L", "GLASS_D", "GLASS", "LAMP"],
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


def palm_tree(pal, w=170, h=420, p=4):
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


def mock_frame(pal, W=340, H=250):
    im = Img(W, H)
    horizon = int(H * 0.40)
    for y in range(horizon + 6):
        u = (y / (horizon + 6)) ** 1.2
        if u < 0.55:
            c = lerp(pal["SKY_TOP"], pal["SKY_MID"], u / 0.55)
        else:
            c = lerp(pal["SKY_MID"], pal["SKY_LOW"], (u - 0.55) / 0.45)
        im.rect(0, y, W, 1, c)
    # a soft low sun disc
    sx, sy, sr = W * 0.62, horizon - 6, 22
    for yy in range(int(sy - sr), int(sy + sr)):
        for xx in range(int(sx - sr), int(sx + sr)):
            dx, dy = (xx - sx) / sr, (yy - sy) / sr
            if dx * dx + dy * dy <= 1.0:
                im.set(xx, yy, pal["SUN"])
    for x in range(W):
        u = x / W
        yy = horizon - 14 + math.sin(2 * math.pi * u) * 8 + math.sin(2 * math.pi * 3 * u + 0.7) * 4
        for y in range(int(yy), horizon + 2):
            im.set(x, y, pal["HILL_FAR"])
    for x in range(W):
        u = x / W
        yy = horizon - 7 + math.sin(2 * math.pi * 2 * u + 2.1) * 5 + math.sin(2 * math.pi * 5 * u) * 3
        for y in range(int(yy), horizon + 2):
            im.set(x, y, pal["HILL_NEAR"])
    for x in range(W):
        u = x / W
        yy = horizon - 3 + math.sin(2 * math.pi * 7 * u) * 2
        for y in range(int(yy), horizon + 1):
            im.set(x, y, pal["TREELINE"])

    lanes = 3
    curve = 0.4
    for y in range(horizon, H):
        t = (y - horizon + 1) / (H - horizon)
        raw_fog = math.exp(-((1 - t) ** 2) * 2.0)
        fog = max(raw_fog, FOG_FLOOR)
        cx = W / 2 + curve * (1 - t) ** 2 * W * 0.55
        hw = W * 0.56 * t
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

    palm = palm_tree(pal)
    for t, side in ((0.30, -1), (0.55, 1), (1.0, -1)):
        cx = W / 2 + curve * (1 - t) ** 2 * W * 0.55
        hw = W * 0.56 * t
        dw = max(2, int(palm.w * t * 0.62))
        dh = max(2, int(palm.h * t * 0.62))
        x = int(cx + side * (hw * 1.7)) - dw // 2
        y = int(horizon + (H - horizon) * t) - dh
        for yy in range(dh):
            j = yy * palm.h // dh
            for xx in range(dw):
                i = xx * palm.w // dw
                idx = (j * palm.w + i) * 4
                a = palm.px[idx + 3]
                if a:
                    im.set(x + xx, y + yy, tuple(palm.px[idx : idx + 4]))

    car = car_ferrari(pal)
    dw, dh = car.w * 2, car.h * 2
    ox, oy = (W - dw) // 2, H - dh - 4
    for yy in range(car.h):
        for xx in range(car.w):
            i = (yy * car.w + xx) * 4
            a = car.px[i + 3]
            if a:
                p = tuple(car.px[i : i + 4])
                for dy2 in range(2):
                    for dx2 in range(2):
                        im.set(ox + xx * 2 + dx2, oy + yy * 2 + dy2, p)
    return im


def main():
    SW, GAP = 26, 4
    row_h = SW + GAP
    frame_w, frame_h = 340, 250

    card_w = max(9 * (SW + GAP), frame_w) + GAP * 2
    card_h = 26 + len(ROW_KEYS) * row_h + GAP * 2 + frame_h + GAP * 2

    sheet = Image.new("RGBA", (card_w * 2 + GAP * 3, card_h * 2 + GAP * 3), (244, 240, 236, 255))

    for idx, (label, scene) in enumerate(CANDIDATES.items()):
        pal = palette_for(scene)
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
        frame_im = mock_frame(pal, frame_w, frame_h)
        frame_png = Image.frombytes("RGBA", (frame_im.w, frame_im.h), bytes(frame_im.px))
        card.alpha_composite(frame_png, (GAP, y))

        cx = (idx % 2) * (card_w + GAP) + GAP
        cy = (idx // 2) * (card_h + GAP) + GAP
        sheet.alpha_composite(card, (cx, cy))
        d = ImageDraw.Draw(sheet)
        d.rectangle([cx, cy, cx + card_w, cy + 22], fill=(90, 78, 96, 255))
        d.text((cx + 8, cy + 4), label, fill=(250, 247, 244, 255))

    sheet.save("swatch6.png")
    print("wrote swatch6.png", sheet.size)


main()
