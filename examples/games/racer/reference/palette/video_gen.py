#!/usr/bin/env python3
"""Compare two background-crossfade techniques across the 4-scene day cycle
(asagake/I2 -> hiru/G -> yuugure/I1 -> twilight/I3 -> loop), as two 14s GIFs:

  dither.gif  -- ordered (Bayer) dither crossfade, 2-valued alpha
  blend.gif   -- true per-pixel alpha blend, 0..255

Frame budget: render each of the 4 pure scenes once (expensive), then combine
already-rendered raster buffers per output frame (cheap) -- both techniques
reuse the same 4 cached renders so the comparison isolates the transition
method, not scene content.
"""

import colorsys
import math

from PIL import Image, ImageDraw

# ---- shared G treatment (finalized) ----------------------------------------
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

DAY = dict(
    SKY_TOP=(172, 206, 232), SKY_MID=(206, 224, 236), SKY_LOW=(250, 230, 212),
    CLOUD=(253, 248, 244), SUN=(253, 232, 196),
    HILL_FAR=(188, 206, 190), HILL_NEAR=(160, 188, 164), TREELINE=(140, 170, 144),
    LEAF_D=(142, 172, 136), LEAF_M=(172, 198, 152), LEAF_L=(206, 222, 174),
    ROAD_L=(198, 194, 202), ROAD_D=(186, 182, 192),
    GRASS_L=(190, 216, 168), GRASS_D=(176, 204, 156),
    RUMBLE_L=(248, 202, 192), RUMBLE_D=(242, 234, 224),
    FOG=(218, 228, 212),
)
ASAGAKE = dict(  # I2 golden hour
    SKY_TOP=(182, 192, 222), SKY_MID=(242, 190, 150), SKY_LOW=(255, 178, 108),
    CLOUD=(255, 240, 220), SUN=(255, 210, 120),
    HILL_FAR=(202, 160, 128), HILL_NEAR=(172, 120, 88), TREELINE=(132, 82, 60),
    LEAF_D=(150, 148, 96), LEAF_M=(182, 178, 118), LEAF_L=(214, 204, 148),
    GRASS_D=(182, 170, 88), GRASS_L=(202, 190, 118),
    ROAD_D=(190, 164, 148), ROAD_L=(206, 184, 168),
    RUMBLE_L=(252, 188, 156), RUMBLE_D=(238, 208, 186),
    FOG=(250, 198, 148),
)
YUUGURE = dict(  # I1 dusk pastel
    SKY_TOP=(158, 156, 208), SKY_MID=(222, 172, 182), SKY_LOW=(255, 202, 158),
    CLOUD=(253, 248, 244), SUN=(255, 222, 172),
    HILL_FAR=(184, 156, 176), HILL_NEAR=(154, 124, 146), TREELINE=(124, 96, 118),
    LEAF_D=(142, 150, 120), LEAF_M=(172, 176, 138), LEAF_L=(206, 202, 162),
    GRASS_D=(174, 178, 122), GRASS_L=(192, 196, 146),
    ROAD_D=(172, 162, 182), ROAD_L=(188, 178, 198),
    RUMBLE_L=(248, 200, 190), RUMBLE_D=(236, 214, 210),
    FOG=(232, 194, 176),
)
TWILIGHT = dict(  # I3 twilight violet
    SKY_TOP=(96, 84, 148), SKY_MID=(174, 112, 162), SKY_LOW=(240, 152, 160),
    CLOUD=(244, 218, 226), SUN=(255, 190, 150),
    HILL_FAR=(142, 112, 152), HILL_NEAR=(102, 82, 122), TREELINE=(72, 56, 92),
    LEAF_D=(112, 108, 118), LEAF_M=(142, 134, 144), LEAF_L=(176, 162, 168),
    GRASS_D=(140, 128, 108), GRASS_L=(162, 150, 130),
    ROAD_D=(142, 120, 152), ROAD_L=(160, 140, 172),
    RUMBLE_L=(226, 160, 176), RUMBLE_D=(210, 178, 196),
    FOG=(202, 150, 172),
)

# cycle order per user: asagake -> hiru -> yuugure -> twilight -> (loop)
SCENE_ORDER = [("asagake", ASAGAKE), ("hiru", DAY), ("yuugure", YUUGURE), ("twilight", TWILIGHT)]


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


# ------------------------------------------------------------------ drawing --


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


def mock_frame(pal, W, H):
    im = Img(W, H)
    horizon = int(H * 0.40)
    for y in range(horizon + 6):
        u = (y / (horizon + 6)) ** 1.2
        if u < 0.55:
            c = lerp(pal["SKY_TOP"], pal["SKY_MID"], u / 0.55)
        else:
            c = lerp(pal["SKY_MID"], pal["SKY_LOW"], (u - 0.55) / 0.45)
        im.rect(0, y, W, 1, c)
    sx, sy, sr = W * 0.62, horizon - 6, W * 0.065
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
        dw = max(2, int(palm.w * t * 0.62 * (W / 340)))
        dh = max(2, int(palm.h * t * 0.62 * (W / 340)))
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
    dw, dh = int(car.w * 2 * (W / 340)), int(car.h * 2 * (W / 340))
    ox, oy = (W - dw) // 2, H - dh - 4
    for yy in range(car.h):
        for xx in range(car.w):
            i = (yy * car.w + xx) * 4
            a = car.px[i + 3]
            if a:
                p = tuple(car.px[i : i + 4])
                for j in range(dh // car.h + 1):
                    for k in range(dw // car.w + 1):
                        im.set(ox + xx * (dw // car.w) + k, oy + yy * (dh // car.h) + j, p)
    return im


# --------------------------------------------------------------- combining --

BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def combine_dither(a, b, p):
    out = Img(a.w, a.h)
    for y in range(a.h):
        row_thresh = BAYER8[y % 8]
        for x in range(a.w):
            thresh = (row_thresh[x % 8] + 0.5) / 64.0
            i = (y * a.w + x) * 4
            out.px[i : i + 4] = b.px[i : i + 4] if thresh < p else a.px[i : i + 4]
    return out


def combine_blend(a, b, p):
    out = Img(a.w, a.h)
    n = a.w * a.h
    for i in range(0, n * 4, 4):
        for k in range(3):
            out.px[i + k] = round(a.px[i + k] + (b.px[i + k] - a.px[i + k]) * p)
        out.px[i + 3] = 255
    return out


def to_pil(im, scale=2):
    pil = Image.frombytes("RGBA", (im.w, im.h), bytes(im.px)).convert("RGB")
    if scale != 1:
        pil = pil.resize((im.w * scale, im.h * scale), Image.NEAREST)
    return pil


# -------------------------------------------------------------------- main --


def main():
    W, H = 220, 150
    print("rendering 4 base scenes...")
    frames_by_scene = {}
    for name, scene in SCENE_ORDER:
        pal = palette_for(scene)
        frames_by_scene[name] = mock_frame(pal, W, H)
        print(" ", name, "done")

    ordered = [frames_by_scene[name] for name, _ in SCENE_ORDER]
    names = [name for name, _ in SCENE_ORDER]

    FPS = 12
    HOLD_S, TRANS_S = 1.5, 2.0
    LEG_S = HOLD_S + TRANS_S
    TOTAL_S = LEG_S * 4
    n_frames = int(TOTAL_S * FPS)

    for mode, combine in (("dither", combine_dither), ("blend", combine_blend)):
        print(f"rendering {mode} ({n_frames} frames)...")
        pil_frames = []
        for f in range(n_frames):
            t = f / FPS
            leg = int(t / LEG_S) % 4
            local = t % LEG_S
            a_idx, b_idx = leg, (leg + 1) % 4
            if local < HOLD_S:
                frame = ordered[a_idx]
                label = names[a_idx]
            else:
                p = (local - HOLD_S) / TRANS_S
                frame = combine(ordered[a_idx], ordered[b_idx], p)
                label = f"{names[a_idx]} -> {names[b_idx]}  {int(p*100)}%"
            pil = to_pil(frame, scale=2)
            d = ImageDraw.Draw(pil)
            d.rectangle([0, 0, pil.width, 14], fill=(40, 34, 38))
            d.text((3, 2), f"{mode}: {label}", fill=(250, 247, 244))
            pil_frames.append(pil)
        out_path = f"{mode}.gif"
        pil_frames[0].save(
            out_path, save_all=True, append_images=pil_frames[1:],
            duration=int(1000 / FPS), loop=0, optimize=False,
        )
        print("wrote", out_path)


main()
