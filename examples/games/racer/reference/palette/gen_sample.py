#!/usr/bin/env python3
"""Sample asset generator: proves the racer sprites/background can be drawn
procedurally (no third-party art). Stdlib only -- PNG written via zlib.

Output: sample.png -- a preview sheet with three sprites at their true atlas
sizes (PALM_TREE 215x540, BILLBOARD01 300x170, CAR01 80x56) plus a 640x240
crop of the three background layers.
"""

import math
import struct
import zlib

# ---------------------------------------------------------------- canvas ----


class Img:
    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.px = bytearray(w * h * 4)  # RGBA, transparent

    def set(self, x, y, c):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        i = (y * self.w + x) * 4
        r, g, b, a = c
        if a == 255:
            self.px[i : i + 4] = bytes((r, g, b, 255))
        elif a:
            dr, dg, db, da = self.px[i : i + 4]
            k = a / 255.0
            self.px[i : i + 4] = bytes(
                (
                    int(r * k + dr * (1 - k)),
                    int(g * k + dg * (1 - k)),
                    int(b * k + db * (1 - k)),
                    max(da, a),
                )
            )

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, c)

    def ellipse(self, cx, cy, rx, ry, c):
        for yy in range(int(cy - ry), int(cy + ry) + 1):
            for xx in range(int(cx - rx), int(cx + rx) + 1):
                dx = (xx - cx) / rx
                dy = (yy - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.set(xx, yy, c)

    def poly(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            n = len(pts)
            for i in range(n):
                x1, y1 = pts[i]
                x2, y2 = pts[(i + 1) % n]
                if (y1 <= y < y2) or (y2 <= y < y1):
                    xs.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.floor(xs[i])), int(math.ceil(xs[i + 1]))):
                    self.set(x, y, c)

    def blit(self, src, dx, dy):
        for y in range(src.h):
            for x in range(src.w):
                i = (y * src.w + x) * 4
                a = src.px[i + 3]
                if a:
                    self.set(dx + x, dy + y, tuple(src.px[i : i + 4]))

    def write(self, path):
        raw = bytearray()
        stride = self.w * 4
        for y in range(self.h):
            raw.append(0)  # filter: none
            raw += self.px[y * stride : (y + 1) * stride]

        def chunk(tag, data):
            return (
                struct.pack(">I", len(data))
                + tag
                + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
            )

        hdr = struct.pack(">2I5B", self.w, self.h, 8, 6, 0, 0, 0)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", hdr))
            f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
            f.write(chunk(b"IEND", b""))


def shade(c, k):
    return (
        min(255, int(c[0] * k)),
        min(255, int(c[1] * k)),
        min(255, int(c[2] * k)),
        c[3] if len(c) > 3 else 255,
    )


# ---------------------------------------------------------------- sprites ----

TRUNK = (150, 110, 70, 255)
TRUNK_D = (110, 78, 48, 255)
LEAF = (26, 138, 54, 255)
LEAF_D = (16, 96, 40, 255)


def palm_tree(w=215, h=540):
    im = Img(w, h)
    cx = w // 2
    # trunk: a slight S curve, tapering upward
    for y in range(int(h * 0.28), h):
        t = (y - h * 0.28) / (h * 0.72)
        bend = math.sin(t * 2.1) * 16
        tw = 6 + t * 12
        x0 = cx + bend - tw / 2
        for x in range(int(x0), int(x0 + tw) + 1):
            im.set(x, y, TRUNK if x < x0 + tw * 0.6 else TRUNK_D)
    top_x = cx + math.sin(0.0) * 16
    top_y = int(h * 0.28)
    # fronds: tapered quads with a darker underside, drawn back-to-front
    fronds = [
        (-1.55, 96, 1),
        (1.55, 96, 1),
        (-1.15, 88, 0),
        (1.15, 88, 0),
        (-0.62, 74, 1),
        (0.62, 74, 1),
        (-0.10, 60, 0),
        (0.20, 62, 0),
    ]
    for ang, ln, dark in fronds:
        col = LEAF_D if dark else LEAF
        a = ang
        pts = []
        # midrib sags: sample the arc, offset perpendicular for thickness
        seg = []
        for i in range(9):
            t = i / 8.0
            r = ln * t
            x = top_x + math.cos(a - math.pi / 2) * r * 1.5
            y = top_y + math.sin(a - math.pi / 2) * r + (t * t) * ln * 0.55
            seg.append((x, y))
        thick = [1 + (1 - abs(t / 8.0 - 0.35)) * 15 for t in range(9)]
        for i, (x, y) in enumerate(seg):
            pts.append((x, y - thick[i] * 0.55))
        for i in range(8, -1, -1):
            x, y = seg[i]
            pts.append((x, y + thick[i] * 0.55))
        im.poly(pts, col)
    # coconuts
    for dx, dy in ((-11, 10), (7, 14), (-2, 20)):
        im.ellipse(top_x + dx, top_y + dy, 7, 7, (120, 92, 52, 255))
    return im


def billboard(w=300, h=170):
    im = Img(w, h)
    post = (96, 84, 74, 255)
    panel_h = int(h * 0.74)
    # two legs
    im.rect(int(w * 0.22), panel_h - 4, 14, h - panel_h + 4, post)
    im.rect(int(w * 0.70), panel_h - 4, 14, h - panel_h + 4, post)
    # panel + frame
    im.rect(0, 0, w, panel_h, (238, 232, 214, 255))
    im.rect(0, 0, w, 7, (58, 62, 70, 255))
    im.rect(0, panel_h - 7, w, 7, (58, 62, 70, 255))
    im.rect(0, 0, 7, panel_h, (58, 62, 70, 255))
    im.rect(w - 7, 0, 7, panel_h, (58, 62, 70, 255))
    # diagonal stripe band
    for y in range(10, panel_h - 10):
        for x in range(10, w - 10):
            if ((x + y) // 22) % 2 == 0:
                im.set(x, y, (246, 200, 60, 255))
    # a coiled snake silhouette (culebra) over the stripes
    for i in range(120):
        t = i / 119.0
        x = 40 + t * (w - 110)
        y = panel_h / 2 + math.sin(t * math.pi * 2.2) * (panel_h * 0.24)
        r = 11 - t * 5
        im.ellipse(x, y, r, r, (22, 108, 46, 255))
    im.ellipse(w - 66, panel_h / 2 + math.sin(2.2 * math.pi) * panel_h * 0.24, 9, 9, (14, 84, 34, 255))
    return im


def car(w=80, h=56, body=(196, 52, 44, 255)):
    im = Img(w, h)
    dark = shade(body, 0.62)
    glass = (58, 74, 92, 255)
    tyre = (34, 32, 34, 255)
    # rear view: tyres, lower body, cabin, window, lights, shadow
    im.rect(2, h - 16, 13, 14, tyre)
    im.rect(w - 15, h - 16, 13, 14, tyre)
    im.poly([(6, h - 10), (w - 6, h - 10), (w - 3, h - 22), (3, h - 22)], dark)
    im.poly([(4, h - 22), (w - 4, h - 22), (w - 8, h - 38), (8, h - 38)], body)
    im.poly([(12, h - 38), (w - 12, h - 38), (w - 17, h - 50), (17, h - 50)], shade(body, 0.86))
    im.poly([(17, h - 40), (w - 17, h - 40), (w - 21, h - 48), (21, h - 48)], glass)
    im.rect(6, h - 30, 12, 7, (240, 96, 70, 255))
    im.rect(w - 18, h - 30, 12, 7, (240, 96, 70, 255))
    im.rect(int(w / 2) - 7, h - 27, 14, 4, (226, 220, 206, 255))
    im.rect(4, h - 4, w - 8, 3, (0, 0, 0, 90))
    return im


# ------------------------------------------------------------- background ----


def bg_sky(w=1280, h=480):
    im = Img(w, h)
    for y in range(h):
        t = y / (h - 1)
        c = (
            int(96 + t * 60),
            int(198 + t * 42),
            int(238 + t * 14),
            255,
        )
        im.rect(0, y, w, 1, c)
    # a couple of seamless cloud bands (integer periods -> tiles horizontally)
    for cy, amp, per, col in ((90, 12, 2, (255, 255, 255, 70)), (150, 9, 3, (255, 255, 255, 50))):
        for x in range(w):
            a = math.sin(2 * math.pi * per * x / w)
            b = math.sin(2 * math.pi * (per * 2) * x / w + 1.3)
            th = max(0.0, a * 0.7 + b * 0.4) * amp
            for y in range(int(cy - th), int(cy + th) + 1):
                im.set(x, y, col)
    return im


def bg_hills(w=1280, h=480):
    im = Img(w, h)
    far = (86, 150, 96, 255)
    near = (52, 122, 72, 255)
    for x in range(w):
        u = x / w
        y_far = h * 0.60 + math.sin(2 * math.pi * u) * 42 + math.sin(2 * math.pi * 3 * u + 0.7) * 18
        for y in range(int(y_far), h):
            im.set(x, y, far)
    for x in range(w):
        u = x / w
        y_near = h * 0.74 + math.sin(2 * math.pi * 2 * u + 2.1) * 34 + math.sin(2 * math.pi * 5 * u) * 12
        for y in range(int(y_near), h):
            im.set(x, y, near)
    return im


def bg_trees(w=1280, h=480):
    im = Img(w, h)
    dark = (14, 74, 34, 255)
    base = int(h * 0.78)
    # treeline: densely overlapping crowns so the silhouette reads as a mass of
    # foliage, not a row of poles. Seamless: every period is an integer.
    n = 110
    for i in range(n):
        u = i / n
        x = u * w
        r = 30 + 20 * ((math.sin(2 * math.pi * 7 * u) + 1) / 2)
        top = base - 26 - 46 * ((math.sin(2 * math.pi * 5 * u + 1.1) + 1) / 2)
        im.ellipse(x, top, r, r * 0.85, dark)
        im.ellipse(x + r * 0.5, top + r * 0.5, r * 0.8, r * 0.7, dark)
    im.rect(0, base, w, h - base, dark)
    return im


# ------------------------------------------------------------------ sheet ----


def main():
    pal = palm_tree()
    bb = billboard()
    c1 = car()
    c2 = car(body=(56, 96, 200, 255))
    sky = bg_sky()
    hills = bg_hills()
    trees = bg_trees()

    W, H = 980, 800
    out = Img(W, H)
    out.rect(0, 0, W, H, (24, 26, 30, 255))

    # background preview: the three layers composited, 640x240 crop
    prev = Img(640, 240)
    for src in (sky, hills, trees):
        half = Img(640, 240)
        for y in range(240):
            for x in range(640):
                i = ((y * 2) * src.w + x * 2) * 4
                a = src.px[i + 3]
                if a:
                    half.set(x, y, tuple(src.px[i : i + 4]))
        prev.blit(half, 0, 0)
    out.blit(prev, 20, 20)

    out.blit(pal, 700, 20)
    out.blit(bb, 20, 290)
    out.blit(c1, 340, 300)
    out.blit(c2, 440, 300)

    out.write("sample.png")
    print("wrote sample.png", W, "x", H)


main()
