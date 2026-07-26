#!/usr/bin/env python3
"""Generate retro-run's placeholder art.

The upstream javascript-racer ships sprites lifted from OutRun and music that
may not be redistributed, so this draws stand-ins from scratch. What is NOT a
stand-in is the geometry: every sprite lands at the exact atlas rectangle
common.js names, because SPRITES.w feeds both collision (Util.overlap) and
scale (SPRITES.SCALE = 0.3 / PLAYER_STRAIGHT.w). Swap the pictures, keep the
rectangles, and the game plays identically.

Palette is base colours, a saturation boost, and a narrowing of each light/dark
pair. The whole recipe lives here, so this file is the single thing the assets
depend on.

The four day-cycle backgrounds must be geometry-identical — the cross-fade
blends them pixel for pixel, so a ridge line one pixel out would ghost. That is
enforced structurally: the art is drawn once into a buffer of palette KEYS, and
each scene is a different lookup of the same buffer. There is no way to draw a
scene-specific shape, because the drawing code never sees a colour.

Usage:  python3 gen_assets.py [outdir]      (default ../assets)
Only the Python standard library is used; PNGs are written by hand via zlib.
"""

import colorsys
import os
import struct
import sys
import zlib

# --- palette -----------------------------------------------------------------
SAT_K = 0.85
NARROW = 0.30
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

# Colours that do not change with the time of day: bodywork, trunks, ink.
COMMON = dict(
    INK=(120, 98, 106), INK2=(154, 132, 138),
    CORAL=(244, 172, 162), CORAL_D=(216, 140, 134), CORAL_L=(252, 208, 198),
    GLASS=(214, 230, 238), GLASS_D=(182, 204, 218),
    LAMP=(240, 166, 156), LAMP_L=(250, 210, 200),
    TYRE=(144, 134, 140), TYRE_D=(120, 110, 118),
    CREAM=(250, 242, 228), PLATE=(238, 232, 220), SHADOW=(198, 186, 190),
    LANE=(252, 248, 242),
    WOOD_D=(164, 134, 114), WOOD_M=(192, 164, 138), WOOD_L=(216, 194, 168),
)

HIRU = dict(
    SKY_TOP=(172, 206, 232), SKY_MID=(206, 224, 236), SKY_LOW=(250, 230, 212),
    CLOUD=(253, 248, 244), SUN=(253, 232, 196),
    HILL_FAR=(188, 206, 190), HILL_NEAR=(160, 188, 164), TREELINE=(140, 170, 144),
    LEAF_D=(142, 172, 136), LEAF_M=(172, 198, 152), LEAF_L=(206, 222, 174),
    ROAD_L=(198, 194, 202), ROAD_D=(186, 182, 192),
    GRASS_L=(190, 216, 168), GRASS_D=(176, 204, 156),
    RUMBLE_L=(248, 202, 192), RUMBLE_D=(242, 234, 224),
    FOG=(218, 228, 212),
)
ASAYAKE = dict(
    SKY_TOP=(182, 192, 222), SKY_MID=(242, 190, 150), SKY_LOW=(255, 178, 108),
    CLOUD=(255, 240, 220), SUN=(255, 210, 120),
    HILL_FAR=(202, 160, 128), HILL_NEAR=(172, 120, 88), TREELINE=(132, 82, 60),
    LEAF_D=(150, 148, 96), LEAF_M=(182, 178, 118), LEAF_L=(214, 204, 148),
    GRASS_D=(182, 170, 88), GRASS_L=(202, 190, 118),
    ROAD_D=(190, 164, 148), ROAD_L=(206, 184, 168),
    RUMBLE_L=(252, 188, 156), RUMBLE_D=(238, 208, 186),
    FOG=(250, 198, 148),
)
YUUGURE = dict(
    SKY_TOP=(158, 156, 208), SKY_MID=(222, 172, 182), SKY_LOW=(255, 202, 158),
    CLOUD=(253, 248, 244), SUN=(255, 222, 172),
    HILL_FAR=(184, 156, 176), HILL_NEAR=(154, 124, 146), TREELINE=(124, 96, 118),
    LEAF_D=(142, 150, 120), LEAF_M=(172, 176, 138), LEAF_L=(206, 202, 162),
    GRASS_D=(174, 178, 122), GRASS_L=(192, 196, 146),
    ROAD_D=(172, 162, 182), ROAD_L=(188, 178, 198),
    RUMBLE_L=(248, 200, 190), RUMBLE_D=(236, 214, 210),
    FOG=(232, 194, 176),
)
TWILIGHT = dict(
    SKY_TOP=(96, 84, 148), SKY_MID=(174, 112, 162), SKY_LOW=(240, 152, 160),
    CLOUD=(244, 218, 226), SUN=(255, 190, 150),
    HILL_FAR=(142, 112, 152), HILL_NEAR=(102, 82, 122), TREELINE=(72, 56, 92),
    LEAF_D=(112, 108, 118), LEAF_M=(142, 134, 144), LEAF_L=(176, 162, 168),
    GRASS_D=(140, 128, 108), GRASS_L=(162, 150, 130),
    ROAD_D=(142, 120, 152), ROAD_L=(160, 140, 172),
    RUMBLE_L=(226, 160, 176), RUMBLE_D=(210, 178, 196),
    FOG=(202, 150, 172),
)

# Cycled one per lap, in this order.
SCENES = [("asayake", ASAYAKE), ("hiru", HIRU), ("yuugure", YUUGURE),
          ("twilight", TWILIGHT)]


def _boost(c, k):
    h, s, v = colorsys.rgb_to_hsv(*(x / 255 for x in c))
    r, g, b = colorsys.hsv_to_rgb(h, min(1.0, s * (1 + k) + 0.02), v)
    return (r * 255, g * 255, b * 255)


def _narrow_toward(light_c, dark_c, amount):
    lh, ls, lv = colorsys.rgb_to_hsv(*(x / 255 for x in light_c))
    _, ds, dv = colorsys.rgb_to_hsv(*(x / 255 for x in dark_c))
    r, g, b = colorsys.hsv_to_rgb(lh, min(1.0, ls + (ds - ls) * amount),
                                  lv + (dv - lv) * amount * 0.6)
    return (round(r * 255), round(g * 255), round(b * 255))


def palette_for(scene):
    """Base colours -> the final RGB the art is baked with."""
    base = dict(COMMON)
    base.update(scene)
    pal = {n: (tuple(round(x) for x in c) if n in NO_BOOST
               else tuple(round(x) for x in _boost(c, SAT_K)))
           for n, c in base.items()}
    for dark, light in PAIRS:
        if dark in pal and light in pal:
            pal[light] = _narrow_toward(pal[light], pal[dark], NARROW)
    return pal


# Key 0 is transparent; the rest index this list. Drawing code names keys, never
# colours, which is what keeps the four scenes geometry-identical.
KEYS = ["", ] + sorted(set(COMMON) | set(HIRU))
KEY_ID = {name: i for i, name in enumerate(KEYS)}


# --- canvas ------------------------------------------------------------------
class Cells:
    """A grid of palette keys. Everything is drawn here, in chunky cells, and
    expanded to pixels at the end — that is where the coarse pixel-art grid
    comes from."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.k = bytearray(w * h)

    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.k[y * self.w + x]
        return 0

    def set(self, x, y, key):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            self.k[y * self.w + x] = KEY_ID[key] if isinstance(key, str) else key

    def rect(self, x, y, w, h, key):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, key)

    def dither(self, x, y, w, h, key_a, key_b, size=1):
        """Checkerboard between two keys — how shading is done, since the
        palette has no in-between tones."""
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, key_a if ((xx // size + yy // size) % 2 == 0)
                         else key_b)

    def ellipse(self, cx, cy, rx, ry, key):
        if rx <= 0 or ry <= 0:
            return
        for yy in range(int(cy - ry), int(cy + ry) + 1):
            dy = (yy - cy) / ry
            if abs(dy) > 1:
                continue
            half = rx * (1 - dy * dy) ** 0.5
            for xx in range(int(cx - half), int(cx + half) + 1):
                self.set(xx, yy, key)

    def poly(self, pts, key):
        """Even-odd scanline fill, the same rule Canvas.polygon uses."""
        if len(pts) < 3:
            return
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            for i in range(len(pts)):
                ax, ay = pts[i]
                bx, by = pts[(i + 1) % len(pts)]
                if ay == by:
                    continue
                if y < min(ay, by) or y >= max(ay, by):
                    continue
                xs.append(ax + (bx - ax) * (y - ay) / (by - ay))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(xs[i]), int(xs[i + 1]) + 1):
                    self.set(x, y, key)

    def outline(self, key="INK"):
        """One-cell ink border around everything drawn so far. Runs on a copy so
        the border doesn't grow into itself."""
        src = bytes(self.k)
        kid = KEY_ID[key]
        for y in range(self.h):
            for x in range(self.w):
                if src[y * self.w + x]:
                    continue
                touching = False
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < self.w and 0 <= ny < self.h \
                            and src[ny * self.w + nx] not in (0, kid):
                        touching = True
                        break
                if touching:
                    self.k[y * self.w + x] = kid

    def shade_bottom(self, key_dark, frac=0.35):
        """Dither the lower part of every filled column toward `key_dark` — a
        cheap, consistent ambient occlusion for the plants and boulders."""
        for x in range(self.w):
            col = [y for y in range(self.h) if self.get(x, y) not in
                   (0, KEY_ID["INK"])]
            if not col:
                continue
            start = int(min(col) + (max(col) - min(col)) * (1 - frac))
            for y in col:
                if y >= start and (x + y) % 2 == 0:
                    self.set(x, y, key_dark)


class KeyImage:
    """A full-resolution buffer of palette keys — the atlas under construction."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.k = bytearray(w * h)

    def blit_cells(self, cells, x, y, w, h, scale):
        """Expand `cells` by `scale` and drop it at (x, y), cropped to w x h."""
        for py in range(h):
            cy = py // scale
            if cy >= cells.h:
                break
            row = cy * cells.w
            base = (y + py) * self.w + x
            for px in range(w):
                cx = px // scale
                if cx >= cells.w:
                    break
                v = cells.k[row + cx]
                if v:
                    self.k[base + px] = v

    def bake(self, palette):
        """Key buffer -> RGBA bytes. The only place colour enters."""
        lut = bytearray(len(KEYS) * 4)
        for i, name in enumerate(KEYS):
            if i == 0:
                continue
            r, g, b = palette[name]
            lut[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
        out = bytearray(self.w * self.h * 4)
        for i, v in enumerate(self.k):
            if v:
                out[i * 4:i * 4 + 4] = lut[v * 4:v * 4 + 4]
        return bytes(out)


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)  # filter: none, so the file stays trivially reproducible
        raw += rgba[y * stride:(y + 1) * stride]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# --- deterministic noise -----------------------------------------------------
# A hash, not an RNG: the same input always gives the same value, so nothing in
# the output depends on evaluation order or on how many shapes came before.
def noise(*args):
    h = 2166136261
    for a in args:
        h = ((h ^ (int(a) & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    return h / 0xFFFFFFFF


# --- background --------------------------------------------------------------
# Three 1280x480 layers at the atlas rectangles common.js names. Each scrolls
# horizontally at its own rate and wraps, so every shape's horizontal period
# must divide the layer width exactly — otherwise the wrap shows a seam.
BACKGROUND = {
    "HILLS": (5, 5, 1280, 480),
    "SKY":   (5, 495, 1280, 480),
    "TREES": (5, 985, 1280, 480),
}
BG_SIZE = (1290, 1470)
BG_CELL = 4          # one drawn cell = 4x4 pixels
LW, LH = 1280 // BG_CELL, 480 // BG_CELL   # 320 x 120 cells


# Every horizontal period must divide the layer width exactly or the scroll
# shows a seam where the layer meets itself. These are the divisors of 320.
PERIODS = (160, 80, 64, 40, 32)


def wave(x, period, amp, phase=0.0):
    """A sine whose period divides the layer width, so it wraps exactly."""
    import math
    assert LW % period == 0, "period %r does not divide the layer width" % period
    return amp * math.sin(2 * math.pi * (x / period + phase))


def draw_sky(c):
    # Three bands, dithered where they meet so the gradient reads as pixel art
    # rather than a smooth ramp.
    top, mid = int(LH * 0.40), int(LH * 0.72)
    c.rect(0, 0, LW, top, "SKY_TOP")
    c.rect(0, top, LW, mid - top, "SKY_MID")
    c.rect(0, mid, LW, LH - mid, "SKY_LOW")
    for y in range(top - 4, top + 4):
        for x in range(LW):
            if (x + y) % 2 == 0:
                c.set(x, y, "SKY_TOP" if y < top else "SKY_MID")
    for y in range(mid - 4, mid + 4):
        for x in range(LW):
            if (x + y) % 2 == 0:
                c.set(x, y, "SKY_MID" if y < mid else "SKY_LOW")

    # The sun sits well inside the layer, so it never straddles the wrap.
    c.ellipse(LW * 0.72, LH * 0.30, 13, 13, "SUN")

    # Clouds: horizontal periods chosen to divide LW, drawn wrapped so a cloud
    # that runs off the right edge continues on the left.
    for i in range(7):
        cx = (i * LW // 7) + int(noise(i, 11) * 20)
        cy = int(LH * (0.10 + 0.22 * noise(i, 3)))
        for j in range(3):
            rx = 9 + int(noise(i, j, 5) * 11)
            ry = max(2, rx // 3)
            ox = cx + (j - 1) * rx
            for yy in range(cy - ry, cy + ry + 1):
                dy = (yy - cy) / ry
                if abs(dy) > 1:
                    continue
                half = rx * (1 - dy * dy) ** 0.5
                for xx in range(int(ox - half), int(ox + half) + 1):
                    c.set(xx % LW, yy, "CLOUD")


def draw_hills(c):
    # Two ridges. Each is a sum of sines whose periods divide LW, so the
    # silhouette meets itself at the wrap.
    for x in range(LW):
        far = LH * 0.62 + wave(x, 160, 9) + wave(x, 64, 4, 0.3)
        near = LH * 0.74 + wave(x, 80, 11, 0.15) + wave(x, 40, 5, 0.6)
        c.rect(x, int(far), 1, LH - int(far), "HILL_FAR")
        c.rect(x, int(near), 1, LH - int(near), "HILL_NEAR")
        # A dithered band where the near ridge starts, so the two read apart.
        for y in range(int(near), min(LH, int(near) + 6)):
            if (x + y) % 2 == 0:
                c.set(x, y, "HILL_FAR")


def _tree_row(c, base, step, seed, key, scale):
    """One row of alternating conifer / round crowns, wrapped at the layer edge.
    `step` must divide LW so the row meets itself."""
    assert LW % step == 0, "tree spacing %r does not divide the layer width" % step
    for i in range(LW // step):
        x = i * step + int(noise(i, seed, 7) * step // 2)
        hgt = int((12 + noise(i, seed, 2) * 16) * scale)
        top = base - hgt
        if noise(i, seed, 9) < 0.5:                      # conifer
            for y in range(top, base):
                half = max(1, int((y - top) / max(1, hgt) * 6 * scale) + 1)
                for xx in range(x - half, x + half + 1):
                    c.set(xx % LW, y, key)
        else:                                            # round crown, on a trunk
            r = max(2, int((4 + noise(i, seed, 4) * 4) * scale))
            c.ellipse(x, top + r, r, r, key)
            for y in range(top + r, base):
                c.set(x % LW, y, key)
                c.set((x + 1) % LW, y, key)
    # wrap-around crowns land partly off the right edge; mirror them back
    for y in range(0, LH):
        pass


def draw_trees(c):
    # Two rows: a far row in the hill colour, then the treeline proper. The far
    # row fills the gaps between near crowns, so the layer reads as depth
    # instead of as slits of sky.
    base = int(LH * 0.84)
    c.rect(0, base, LW, LH - base, "TREELINE")
    _tree_row(c, base - 2, 10, 31, "HILL_NEAR", 0.8)
    _tree_row(c, base, 8, 17, "TREELINE", 1.0)


def _check_wraps(name, cells):
    """A scrolling layer meets its own left edge. Column 0 and column LW-1 are
    neighbours once it wraps, so a shape that changes abruptly across that join
    is a seam. Allow a little change — real detail differs column to column —
    but not more than two neighbouring interior columns do."""
    def diff(a, b):
        return sum(1 for y in range(cells.h)
                   if cells.get(a, y) != cells.get(b, y))
    seam = diff(LW - 1, 0)
    interior = max(diff(x, x + 1) for x in range(1, LW - 2))
    assert seam <= interior, \
        "%s does not wrap: %d cells change across the seam, at most %d change " \
        "between interior columns" % (name, seam, interior)


def build_background():
    """Draw all three layers once, into keys. Returns the atlas."""
    img = KeyImage(*BG_SIZE)
    for name, draw in (("SKY", draw_sky), ("HILLS", draw_hills),
                       ("TREES", draw_trees)):
        cells = Cells(LW, LH)
        draw(cells)
        _check_wraps(name, cells)
        x, y, w, h = BACKGROUND[name]
        img.blit_cells(cells, x, y, w, h, BG_CELL)
    return img


# --- sprites -----------------------------------------------------------------
# Verbatim from common.js. These rectangles are the contract: SPRITES.w drives
# collision (Util.overlap) and the world scale (SPRITES.SCALE), so the art may
# be redrawn but the numbers may not move.
SPRITES = {
    "PALM_TREE":              (5, 5, 215, 540),
    "BILLBOARD08":            (230, 5, 385, 265),
    "TREE1":                  (625, 5, 360, 360),
    "DEAD_TREE1":             (5, 555, 135, 332),
    "BILLBOARD09":            (150, 555, 328, 282),
    "BOULDER3":               (230, 280, 320, 220),
    "COLUMN":                 (995, 5, 200, 315),
    "BILLBOARD01":            (625, 375, 300, 170),
    "BILLBOARD06":            (488, 555, 298, 190),
    "BILLBOARD05":            (5, 897, 298, 190),
    "BILLBOARD07":            (313, 897, 298, 190),
    "BOULDER2":               (621, 897, 298, 140),
    "TREE2":                  (1205, 5, 282, 295),
    "BILLBOARD04":            (1205, 310, 268, 170),
    "DEAD_TREE2":             (1205, 490, 150, 260),
    "BOULDER1":               (1205, 760, 168, 248),
    "BUSH1":                  (5, 1097, 240, 155),
    "CACTUS":                 (929, 897, 235, 118),
    "BUSH2":                  (255, 1097, 232, 152),
    "BILLBOARD03":            (5, 1262, 230, 220),
    "BILLBOARD02":            (245, 1262, 215, 220),
    "STUMP":                  (995, 330, 195, 140),
    "SEMI":                   (1365, 490, 122, 144),
    "TRUCK":                  (1365, 644, 100, 78),
    "CAR03":                  (1383, 760, 88, 55),
    "CAR02":                  (1383, 825, 80, 59),
    "CAR04":                  (1383, 894, 80, 57),
    "CAR01":                  (1205, 1018, 80, 56),
    "PLAYER_UPHILL_LEFT":     (1383, 961, 80, 45),
    "PLAYER_UPHILL_STRAIGHT": (1295, 1018, 80, 45),
    "PLAYER_UPHILL_RIGHT":    (1385, 1018, 80, 45),
    "PLAYER_LEFT":            (995, 480, 80, 41),
    "PLAYER_STRAIGHT":        (1085, 480, 80, 41),
    "PLAYER_RIGHT":           (995, 531, 80, 41),
}
SPRITE_SIZE = (1492, 1487)


def cell_scale(w, h):
    """Pixels per drawn cell — the coarse grid the whole sheet shares. Sprites
    already at pixel-art size (the cars) draw one cell per pixel; the large
    scenery is drawn coarse so it reads at the same chunkiness."""
    if max(w, h) <= 130:
        return 1
    return max(2, round(max(w, h) / 60))


def draw_vehicle(c, w, h, kind, seed, body=("CORAL", "CORAL_D", "CORAL_L")):
    """Rear three-quarter view — what the player sees of traffic ahead.
    `kind` picks the silhouette family; the parts are the same everywhere."""
    mid, lo, hi = body
    roof = {"wedge": 0.30, "beetle": 0.40, "suv": 0.52, "van": 0.58}[kind]
    waist = {"wedge": 0.94, "beetle": 0.82, "suv": 0.88, "van": 0.90}[kind]
    top = int(h * (1 - roof - 0.28))
    body_y = int(h * 0.42)
    floor = int(h * 0.86)
    bw = int(w * waist)
    bx = (w - bw) // 2

    c.ellipse(w / 2, floor + 1, bw * 0.55, max(1, h * 0.06), "SHADOW")   # contact
    tyre_w = max(2, int(w * 0.13))
    for tx in (bx - 1, bx + bw - tyre_w + 1):                            # tyres
        c.rect(tx, floor - int(h * 0.16), tyre_w, int(h * 0.18), "TYRE")
        c.rect(tx, floor - int(h * 0.06), tyre_w, int(h * 0.08), "TYRE_D")

    cabin_w = int(bw * (0.62 if kind == "wedge" else 0.78))
    cabin_x = (w - cabin_w) // 2
    c.rect(cabin_x, top, cabin_w, body_y - top, mid)                     # cabin
    c.rect(cabin_x + 2, top + 1, cabin_w - 4, max(1, (body_y - top) - 3), "GLASS")
    c.dither(cabin_x + 2, top + 1, cabin_w - 4, max(1, (body_y - top) // 3),
             "GLASS", "GLASS_D")
    c.rect(bx, body_y, bw, floor - body_y, mid)                          # body
    c.dither(bx, floor - int(h * 0.10), bw, int(h * 0.10), mid, lo)      # skirt
    c.rect(bx + 1, body_y, bw - 2, max(1, int(h * 0.05)), hi)            # top edge
    c.rect(cabin_x, top, cabin_w, max(1, int(h * 0.04)), hi)             # roof

    lamp_w = max(2, int(bw * 0.16))
    ly = int(h * 0.60)
    c.rect(bx + 2, ly, lamp_w, max(2, int(h * 0.10)), "LAMP")
    c.rect(bx + bw - lamp_w - 2, ly, lamp_w, max(2, int(h * 0.10)), "LAMP")
    pw = max(3, int(bw * 0.26))
    c.rect((w - pw) // 2, floor - int(h * 0.14), pw, max(2, int(h * 0.08)), "PLATE")
    c.outline()


def draw_semi(c, w, h, seed):
    """A trailer: tall box on a cab, seen from behind."""
    box_h = int(h * 0.62)
    c.rect(int(w * 0.06), int(h * 0.04), int(w * 0.88), box_h, "CREAM")
    c.rect(int(w * 0.10), int(h * 0.10), int(w * 0.80), int(box_h * 0.5), "PLATE")
    c.dither(int(w * 0.06), int(h * 0.04) + box_h - int(h * 0.08),
             int(w * 0.88), int(h * 0.08), "CREAM", "SHADOW")
    c.rect(int(w * 0.12), int(h * 0.04) + box_h, int(w * 0.76),
           int(h * 0.16), "CORAL_D")
    floor = int(h * 0.92)
    c.ellipse(w / 2, floor, w * 0.46, max(1, h * 0.04), "SHADOW")
    for tx in (int(w * 0.10), int(w * 0.74)):
        c.rect(tx, floor - int(h * 0.12), int(w * 0.16), int(h * 0.13), "TYRE")
        c.rect(tx, floor - int(h * 0.04), int(w * 0.16), int(h * 0.05), "TYRE_D")
    c.rect(int(w * 0.20), floor - int(h * 0.10), int(w * 0.12), int(h * 0.05), "LAMP")
    c.rect(int(w * 0.68), floor - int(h * 0.10), int(w * 0.12), int(h * 0.05), "LAMP")
    c.outline()


def draw_round_tree(c, w, h, seed, leaf=("LEAF_D", "LEAF_M", "LEAF_L")):
    """Overlapping crown blobs on a trunk — TREE1 / TREE2."""
    d, m, l = leaf
    trunk_w = max(2, int(w * 0.12))
    c.rect((w - trunk_w) // 2, int(h * 0.60), trunk_w, int(h * 0.42), "WOOD_M")
    c.dither((w - trunk_w) // 2, int(h * 0.60), max(1, trunk_w // 2),
             int(h * 0.42), "WOOD_D", "WOOD_M")
    cy, cr = h * 0.36, min(w, h) * 0.40
    for i in range(6):
        a = i / 6 * 6.28318
        import math
        ox = w / 2 + math.cos(a) * cr * 0.45
        oy = cy + math.sin(a) * cr * 0.34
        c.ellipse(ox, oy, cr * (0.42 + noise(seed, i) * 0.18),
                  cr * (0.36 + noise(seed, i, 2) * 0.16), d)
    c.ellipse(w / 2, cy, cr * 0.86, cr * 0.72, m)
    c.ellipse(w / 2 - cr * 0.22, cy - cr * 0.22, cr * 0.44, cr * 0.34, l)
    c.shade_bottom("LEAF_D", 0.45)
    c.outline()


def draw_dead_tree(c, w, h, seed):
    """Bare branching trunk."""
    trunk_w = max(2, int(w * 0.16))
    c.rect((w - trunk_w) // 2, int(h * 0.20), trunk_w, int(h * 0.82), "WOOD_D")
    for i in range(5):
        y = int(h * (0.22 + 0.14 * i))
        side = 1 if i % 2 == 0 else -1
        length = int(w * (0.42 - 0.05 * i))
        for j in range(length):
            c.set(w / 2 + side * j, y - j * 0.6, "WOOD_D")
            c.set(w / 2 + side * j, y - j * 0.6 + 1, "WOOD_M")
    c.outline()


def draw_palm(c, w, h, seed):
    """Curved tapering trunk under a crown of drooping fronds."""
    import math
    top = h * 0.30
    for y in range(int(top), h):
        t = (y - top) / (h - top)
        x = w / 2 + math.sin(t * 1.1) * w * 0.14
        half = max(1, (w * 0.06) * (0.6 + 0.6 * t))
        c.rect(x - half, y, half * 2, 1, "WOOD_M")
        c.rect(x - half, y, max(1, half * 0.7), 1, "WOOD_D")
    cx, cy = w / 2, top
    for i in range(8):
        a = math.pi + (i + 0.5) / 8 * math.pi        # fan out sideways and up
        length = w * (0.40 + 0.14 * noise(seed, i))
        for j in range(int(length)):
            t = j / length
            fx = cx + math.cos(a) * j
            fy = cy + math.sin(a) * j * 0.5 + t * t * length * 0.55  # droop
            thick = max(1, int((1 - t) * w * 0.05))
            c.rect(fx - thick / 2, fy, thick, 1, "LEAF_M")
            c.rect(fx - thick / 2, fy + 1, thick, 1, "LEAF_D")
    c.ellipse(cx, cy + 1, max(2, w * 0.07), max(2, w * 0.05), "LEAF_D")
    c.outline()


def draw_bush(c, w, h, seed):
    for i in range(5):
        c.ellipse(w * (0.18 + 0.16 * i), h * (0.62 - 0.10 * noise(seed, i)),
                  w * 0.18, h * 0.30, "LEAF_D")
    c.ellipse(w * 0.46, h * 0.58, w * 0.30, h * 0.28, "LEAF_M")
    c.ellipse(w * 0.36, h * 0.46, w * 0.16, h * 0.14, "LEAF_L")
    c.shade_bottom("LEAF_D", 0.4)
    c.outline()


def draw_cactus(c, w, h, seed):
    """Saguaro: a stem with two arms that run out level, then turn up."""
    stem_w = max(3, int(w * 0.13))
    stem_x = (w - stem_w) // 2
    c.rect(stem_x, int(h * 0.10), stem_w, h - int(h * 0.10), "LEAF_M")
    for side, elbow, rise in ((-1, 0.52, 0.30), (1, 0.64, 0.22)):
        reach = int(w * 0.22)
        arm_y = int(h * elbow)
        x0 = stem_x if side < 0 else stem_x + stem_w - 1
        # the level run, from the stem out to the elbow
        c.rect(min(x0, x0 + side * reach), arm_y, reach, stem_w, "LEAF_M")
        # the upturn, overlapping the run so the joint is solid
        c.rect(x0 + side * reach - (stem_w if side > 0 else 0),
               int(h * (elbow - rise)), stem_w, int(h * rise) + stem_w, "LEAF_M")
    c.dither(stem_x, int(h * 0.10), max(1, stem_w // 3), h - int(h * 0.10),
             "LEAF_D", "LEAF_M")
    c.outline()


def draw_stump(c, w, h, seed):
    c.rect(int(w * 0.24), int(h * 0.34), int(w * 0.52), int(h * 0.62), "WOOD_D")
    c.ellipse(w / 2, h * 0.36, w * 0.28, h * 0.16, "WOOD_M")
    c.ellipse(w / 2, h * 0.36, w * 0.16, h * 0.09, "WOOD_L")
    c.outline()


def draw_boulder(c, w, h, seed):
    """A rounded rock: lit cap, dithered flank, darker base."""
    c.ellipse(w * 0.50, h * 0.64, w * 0.46, h * 0.38, "SHADOW")
    c.ellipse(w * 0.44, h * 0.52, w * 0.30, h * 0.22, "SHADOW")
    c.ellipse(w * 0.40, h * 0.46, w * 0.18, h * 0.12, "PLATE")   # small highlight
    for y in range(int(h * 0.62), h):
        for x in range(w):
            if c.get(x, y) and (x + y) % 2 == 0:
                c.set(x, y, "INK2")
    c.outline()


def draw_billboard(c, w, h, seed):
    """A sign panel on two posts; the face carries flat bands so each board
    reads differently without needing type."""
    post_w = max(2, int(w * 0.06))
    for px in (int(w * 0.22), int(w * 0.72)):
        c.rect(px, int(h * 0.52), post_w, int(h * 0.50), "WOOD_D")
    c.rect(int(w * 0.04), int(h * 0.06), int(w * 0.92), int(h * 0.52), "CREAM")
    face_x, face_y = int(w * 0.08), int(h * 0.10)
    face_w, face_h = int(w * 0.84), int(h * 0.44)
    bands = 2 + int(noise(seed, 1) * 3)
    for i in range(bands):
        key = ("CORAL", "GLASS", "LAMP", "LEAF_M", "CORAL_D")[
            int(noise(seed, i, 5) * 5)]
        by = face_y + face_h * i // bands
        c.rect(face_x, by, face_w, max(1, face_h // bands), key)
    c.dither(face_x, face_y + face_h - max(1, face_h // 5), face_w,
             max(1, face_h // 5), "SHADOW", "PLATE")
    c.outline()


def draw_column(c, w, h, seed):
    shaft = int(w * 0.44)
    c.rect((w - shaft) // 2, int(h * 0.10), shaft, int(h * 0.80), "PLATE")
    c.dither((w - shaft) // 2, int(h * 0.10), max(1, shaft // 3), int(h * 0.80),
             "SHADOW", "PLATE")
    for y, ww in ((0.04, 0.66), (0.86, 0.72)):
        c.rect(int(w * (1 - ww) / 2), int(h * y), int(w * ww), int(h * 0.10),
               "CREAM")
    c.outline()


# Which routine draws what. One table, so adding a sprite is one line.
DRAW = {
    "CAR01": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 1),
    "CAR02": lambda c, w, h: draw_vehicle(c, w, h, "beetle", 2,
                                          ("GLASS", "GLASS_D", "CREAM")),
    "CAR03": lambda c, w, h: draw_vehicle(c, w, h, "suv", 3,
                                          ("LEAF_M", "LEAF_D", "LEAF_L")),
    "CAR04": lambda c, w, h: draw_vehicle(c, w, h, "van", 4,
                                          ("LAMP", "CORAL_D", "LAMP_L")),
    "TRUCK": lambda c, w, h: draw_vehicle(c, w, h, "van", 5,
                                          ("CREAM", "SHADOW", "PLATE")),
    "SEMI": lambda c, w, h: draw_semi(c, w, h, 6),
    "PLAYER_STRAIGHT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 10),
    "PLAYER_LEFT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 11),
    "PLAYER_RIGHT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 12),
    "PLAYER_UPHILL_STRAIGHT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 13),
    "PLAYER_UPHILL_LEFT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 14),
    "PLAYER_UPHILL_RIGHT": lambda c, w, h: draw_vehicle(c, w, h, "wedge", 15),
    "TREE1": lambda c, w, h: draw_round_tree(c, w, h, 20),
    "TREE2": lambda c, w, h: draw_round_tree(c, w, h, 21),
    "DEAD_TREE1": lambda c, w, h: draw_dead_tree(c, w, h, 22),
    "DEAD_TREE2": lambda c, w, h: draw_dead_tree(c, w, h, 23),
    "PALM_TREE": lambda c, w, h: draw_palm(c, w, h, 24),
    "BUSH1": lambda c, w, h: draw_bush(c, w, h, 25),
    "BUSH2": lambda c, w, h: draw_bush(c, w, h, 26),
    "CACTUS": lambda c, w, h: draw_cactus(c, w, h, 27),
    "STUMP": lambda c, w, h: draw_stump(c, w, h, 28),
    "BOULDER1": lambda c, w, h: draw_boulder(c, w, h, 29),
    "BOULDER2": lambda c, w, h: draw_boulder(c, w, h, 30),
    "BOULDER3": lambda c, w, h: draw_boulder(c, w, h, 31),
    "COLUMN": lambda c, w, h: draw_column(c, w, h, 32),
}
for _i in range(1, 10):
    DRAW["BILLBOARD%02d" % _i] = (
        lambda c, w, h, s=40 + _i: draw_billboard(c, w, h, s))


def build_sprites():
    """One sheet, baked with the day palette — sprites are not recoloured per
    scene (only the backgrounds are)."""
    img = KeyImage(*SPRITE_SIZE)
    missing = set(SPRITES) - set(DRAW)
    assert not missing, "no draw routine for %s" % sorted(missing)
    for name, (x, y, w, h) in SPRITES.items():
        s = cell_scale(w, h)
        cw, ch = -(-w // s), -(-h // s)      # ceil, then crop back to w x h
        cells = Cells(cw, ch)
        DRAW[name](cells, cw, ch)
        img.blit_cells(cells, x, y, w, h, s)
    return img


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    written = []

    bg = build_background()
    baked = {}
    for name, scene in SCENES:
        rgba = bg.bake(palette_for(scene))
        baked[name] = rgba
        p = os.path.join(outdir, "background_%s.png" % name)
        written.append((p, write_png(p, bg.w, bg.h, rgba)))

    # The cross-fade blends the scenes pixel for pixel, so their opaque regions
    # must coincide exactly. Assert it rather than trust it.
    ref = baked[SCENES[0][0]]
    for name, _ in SCENES[1:]:
        other = baked[name]
        assert all(ref[i] == other[i] for i in range(3, len(ref), 4)), \
            "background_%s has a different alpha mask than background_%s" % (
                name, SCENES[0][0])

    sp = build_sprites()
    p = os.path.join(outdir, "sprites.png")
    written.append((p, write_png(p, sp.w, sp.h, sp.bake(palette_for(HIRU)))))

    for path, n in written:
        print("%-44s %8d bytes" % (os.path.relpath(path), n))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets"))
