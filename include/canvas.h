#pragma once

// Value-neutral 2D framebuffer primitives (an immediate-mode "Canvas") shared
// by the interpreter and the JIT/AOT backends. These are the thin host/OS
// pieces a little game needs that cannot be expressed in culebra itself: the
// pixel buffer, a sprite blit with flip/transpose, `present` (hand a finished
// frame to the host and, in the browser, suspend until the next animation
// frame), and polled input. Everything above this — the `Canvas` namespace,
// the `Sprite` type, the bitmap font, colours and the `run` loop — lives in
// the culebra stdlib preamble (`CANVAS_MODULE_SOURCE`, src/preambles/-
// canvas.cul), so it stays automatically symmetric across backends.
//
// Underscore-prefixed `_Canvas` marks these as the wrapper's ABI, not a stable
// surface for direct use.
//
// A colour is a packed RGBA Long: r | g<<8 | b<<16 | a<<24. In memory that is
// the byte order [r, g, b, a] — exactly what the browser's putImageData
// consumes — so `present` hands the framebuffer to JS with no repacking.
//
// Geometry arguments (positions and sizes) are Long or Float; `coord()` below
// is the one rounding rule all three backends share.
//
// The native backend is headless when built without CULEBRA_ENABLE_CANVAS_WINDOW
// (the default off macOS): the pixel buffer and sprite ops run identically to
// the browser (so interp/JIT symmetry is verifiable off-screen via get_pixel),
// but `present` shows nothing and input reads as "no button". With the window
// enabled a real raylib desktop window is linked instead (present shows the
// frame and blocks to vsync, input polls the keyboard/mouse) — the
// backend-specific present/input/tone/closing are then declarations here,
// defined in src/runtime/culebra_rt_canvas.cc, so this widely-included
// framebuffer header still pulls in no raylib either way.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace culebra {
namespace _canvas_detail {

// A registered sprite: RGBA pixels plus its dimensions. Handles are indices
// into the registry, so registration is upload-once and blit re-references it
// (no per-frame pixel marshalling across the FFI boundary).
struct Sprite {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> px;
};

inline int& _fb_w() { static int w = 0; return w; }
inline int& _fb_h() { static int h = 0; return h; }
inline std::vector<uint32_t>& _fb() { static std::vector<uint32_t> b; return b; }
inline std::vector<Sprite>& _sprites() { static std::vector<Sprite> s; return s; }

// (Re)allocate the framebuffer to w*h transparent pixels. The sprite registry
// is deliberately NOT cleared: sprites are typically registered once at top
// level before the loop first calls init(), so clearing here would invalidate
// their handles. It is process-lifetime state, reclaimed at process exit (or,
// in the Playground, when Stop respawns the worker).
inline void init(int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  _fb_w() = w;
  _fb_h() = h;
  _fb().assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
}

inline int width() { return _fb_w(); }
inline int height() { return _fb_h(); }

// Geometry saturates into a guard band. 2^30 pixels is far outside any
// framebuffer, and clamping there keeps the edge interpolation's products
// (a coordinate difference times a row offset) inside int64 — so the shape
// rasterizers can stay integer without a single overflow case to reason about.
inline constexpr int64_t kGuard = int64_t{1} << 30;
inline int64_t guard(int64_t v) {
  return v < -kGuard ? -kGuard : (v > kGuard ? kGuard : v);
}

// The pixel index a Float geometry argument names. Rounding is toward -inf,
// the rasterization convention (pixel n covers [n, n+1)): adjacent spans then
// tile with no gap or overlap, and a negative coordinate stays off-buffer
// instead of snapping onto column 0 the way truncation would. Non-finite and
// out-of-range values saturate into the guard band rather than trap, so Long
// and Float arguments land on one rule. Long arguments never reach here — they
// are already pixel indices — so this is the whole Float->pixel contract,
// shared by the interpreter, the JIT and AOT.
inline int64_t coord(double d) {
  if (std::isnan(d)) return 0;
  double f = std::floor(d);
  if (f < -static_cast<double>(kGuard)) return -kGuard;
  if (f > static_cast<double>(kGuard)) return kGuard;
  return static_cast<int64_t>(f);
}

// The drawing target every draw call resolves before writing: the framebuffer
// today, a sprite once render targets exist. Held as an id and resolved per
// call — never cached as a pointer, because the sprite registry vector can
// reallocate between calls.
struct Target {
  uint32_t* px;
  int w;
  int h;
};
inline int64_t& _target_id() { static int64_t id = 0; return id; }
inline Target resolve_target() { return {_fb().data(), _fb_w(), _fb_h()}; }

// Composite `src` over `dst` at `alpha`/255 — straight-alpha source-over,
// integer only, so the three backends round identically. Colour channels mix
// at alpha; the alpha channel follows Porter-Duff (a + da*(1-a)), so drawing
// something translucent on an opaque buffer leaves it opaque instead of
// eroding it toward the page background at present() time. For opaque src
// and dst every channel reduces to a plain weighted mix.
inline uint32_t blend_over(uint32_t src, uint32_t dst, uint32_t alpha) {
  uint32_t out = 0;
  for (int c = 0; c < 3; c++) {
    uint32_t sc = (src >> (c * 8)) & 0xff;
    uint32_t dc = (dst >> (c * 8)) & 0xff;
    uint32_t m = (sc * alpha + dc * (255 - alpha) + 127) / 255;
    out |= m << (c * 8);
  }
  uint32_t da = dst >> 24;
  return out | ((alpha + (da * (255 - alpha) + 127) / 255) << 24);
}

// The two write seams every draw call goes through: one pixel (clipped) and
// one row span [x0, x1). The colour's own alpha composites: 255 stores it
// as-is (the fast path), 0 draws nothing, and in between it blends over what
// is there — so rgba(r, g, b, 128) means a translucent shape everywhere, not
// just on sprite blits. Erasing to transparent is what clear() is for.
inline void put(const Target& t, int64_t x, int64_t y, uint32_t rgba) {
  if (x < 0 || y < 0 || x >= t.w || y >= t.h) return;
  uint32_t a = rgba >> 24;
  if (a == 0) return;
  uint32_t* p = t.px + static_cast<size_t>(y) * t.w + x;
  *p = a == 255 ? rgba : blend_over(rgba, *p, a);
}
inline void span(const Target& t, int64_t y, int64_t x0, int64_t x1,
                 uint32_t rgba) {
  if (y < 0 || y >= t.h) return;
  x0 = std::max<int64_t>(x0, 0);
  x1 = std::min<int64_t>(x1, t.w);
  uint32_t a = rgba >> 24;
  if (a == 0) return;
  uint32_t* row = t.px + static_cast<size_t>(y) * t.w;
  if (a == 255) {
    for (int64_t x = x0; x < x1; x++) row[x] = rgba;
  } else {
    for (int64_t x = x0; x < x1; x++) row[x] = blend_over(rgba, row[x], a);
  }
}

inline void clear(uint32_t rgba) {
  Target t = resolve_target();
  std::fill(t.px, t.px + static_cast<size_t>(t.w) * t.h, rgba);
}

// Store one pixel exactly as given — the raw poke that pairs with get_pixel.
// Unlike the drawing calls it does not composite, so a program can write a
// transparent or half-transparent value and read the same value back.
inline void set_pixel(int x, int y, uint32_t rgba) {
  Target t = resolve_target();
  if (x < 0 || y < 0 || x >= t.w || y >= t.h) return;
  t.px[static_cast<size_t>(y) * t.w + x] = rgba;
}

// The target pixel at (x, y), or 0 (transparent) off-buffer. Exposed as
// `Canvas.get` for pixel-readback collision (reading back what was drawn).
inline uint32_t get_pixel(int x, int y) {
  Target t = resolve_target();
  if (x < 0 || y < 0 || x >= t.w || y >= t.h) return 0;
  return t.px[static_cast<size_t>(y) * t.w + x];
}

// Rectangle, clipped to the target: filled, or just the one-pixel outline
// ring (the outermost pixels of the same fill, so outline + fill tile).
inline void rect(int x, int y, int w, int h, uint32_t rgba, bool fill) {
  if (w <= 0 || h <= 0) return;
  Target t = resolve_target();
  int64_t x1 = static_cast<int64_t>(x) + w;
  if (fill || h <= 2) {
    for (int yy = std::max(0, y); yy < std::min(t.h, y + h); yy++)
      span(t, yy, x, x1, rgba);
    return;
  }
  span(t, y, x, x1, rgba);
  span(t, static_cast<int64_t>(y) + h - 1, x, x1, rgba);
  for (int yy = std::max(0, y + 1); yy < std::min(t.h, y + h - 1); yy++) {
    put(t, x, yy, rgba);
    if (w > 1) put(t, x1 - 1, yy, rgba);
  }
}

// Division rounding toward -inf. Edge interpolation uses it so an edge that
// crosses x = 0 keeps its slope instead of kinking the way C's truncation
// toward zero would.
inline int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Exact integer square root (floor of the real root). The double sqrt is only
// a seed; the fixup loops make the result exact, so every platform and
// optimization level lands on the same pixel.
inline int64_t isqrt(int64_t v) {
  if (v <= 0) return 0;
  int64_t r = static_cast<int64_t>(std::sqrt(static_cast<double>(v)));
  while (r > 0 && r * r > v) r--;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

// Line between two points, both endpoints included. The major axis is walked
// one pixel at a time and the minor coordinate is the interpolated value
// rounded to nearest (ties toward -inf); endpoints are sorted on the major
// axis first, so line(A, B) and line(B, A) draw the same pixels. The walk
// covers only the clipped major range, so a line crossing a small viewport
// costs the pixels visible, and with coordinates in the guard band every
// product below stays inside int64.
inline void line(int64_t x1, int64_t y1, int64_t x2, int64_t y2,
                 uint32_t rgba) {
  Target t = resolve_target();
  x1 = guard(x1);
  y1 = guard(y1);
  x2 = guard(x2);
  y2 = guard(y2);
  bool ymajor = std::llabs(y2 - y1) > std::llabs(x2 - x1);
  if (ymajor) {
    std::swap(x1, y1);
    std::swap(x2, y2);
  }
  if (x1 > x2) {
    std::swap(x1, x2);
    std::swap(y1, y2);
  }
  int64_t dx = x2 - x1, dy = y2 - y1;  // dx >= |dy|
  int64_t m0 = std::max<int64_t>(x1, 0);
  int64_t m1 = std::min<int64_t>(x2, (ymajor ? t.h : t.w) - 1);
  for (int64_t m = m0; m <= m1; m++) {
    int64_t n = dx == 0 ? y1 : y1 + floor_div((m - x1) * dy + dx / 2, dx);
    if (ymajor)
      put(t, n, m, rgba);
    else
      put(t, m, n, rgba);
  }
}

// The one message both backends raise for a `points` element that is neither
// Long nor Float, so the polygon contract reads identically everywhere.
inline constexpr auto kPolygonPointsError =
    "type error: parameter 'points' expects an Array of Long|Float";

// Filled polygon, even-odd rule. `pts` is a flat x0,y0,x1,y1,... of `n`
// vertices; the outline closes automatically. Rows are half-open and each
// filled span is [xl, xr), the same convention as rect, so polygons that share
// an edge tile with no seam and no double-drawn pixel: a row belongs to the
// edge whose y-span contains it, so a shared vertex is counted exactly once.
// The interpolation is integer throughout, so every backend rasterizes the
// identical shape. Fewer than 3 vertices draws nothing.
inline void polygon(const int64_t* pts, int64_t n, uint32_t rgba, bool fill) {
  if (pts == nullptr || n < 3) return;
  auto vx = [&](int64_t i) { return guard(pts[2 * i]); };
  auto vy = [&](int64_t i) { return guard(pts[2 * i + 1]); };
  if (!fill) {
    // The outline is the closed chain of edges as line() draws them.
    for (int64_t i = 0; i < n; i++) {
      int64_t j = (i + 1 == n) ? 0 : i + 1;
      line(vx(i), vy(i), vx(j), vy(j), rgba);
    }
    return;
  }
  Target t = resolve_target();
  int64_t ymin = vy(0), ymax = vy(0);
  for (int64_t i = 1; i < n; i++) {
    ymin = std::min(ymin, vy(i));
    ymax = std::max(ymax, vy(i));
  }
  int64_t y0 = std::max<int64_t>(ymin, 0);
  int64_t y1 = std::min<int64_t>(ymax, t.h);
  if (y0 >= y1) return;
  // Reused across calls: the framebuffer is already process-wide state, so a
  // per-call crossings buffer would be the only allocation in the draw path.
  static std::vector<int64_t> xs;
  for (int64_t y = y0; y < y1; y++) {
    xs.clear();
    for (int64_t i = 0; i < n; i++) {
      int64_t j = (i + 1 == n) ? 0 : i + 1;
      int64_t ay = vy(i), by = vy(j);
      if (ay == by) continue;  // horizontal edge crosses no row
      if (y < std::min(ay, by) || y >= std::max(ay, by)) continue;
      int64_t ax = vx(i), bx = vx(j);
      xs.push_back(ax + floor_div((bx - ax) * (y - ay), by - ay));
    }
    if (xs.size() < 2) continue;
    std::sort(xs.begin(), xs.end());
    for (size_t k = 0; k + 1 < xs.size(); k += 2)
      span(t, y, xs[k], xs[k + 1], rgba);
  }
}

// Triangle — the conventional general shape (raylib, SDL and every GPU
// rasterizer take three vertices), and the same even-odd fill as polygon.
inline void triangle(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t x3,
                     int64_t y3, uint32_t rgba, bool fill) {
  const int64_t pts[6] = {x1, y1, x2, y2, x3, y3};
  polygon(pts, 3, rgba, fill);
}

// Ellipse centred on (cx, cy) with radii (rx, ry) — a circle when they are
// equal. The half-width of the row at offset dy from the centre is
// w = floor(rx * floor(sqrt(ry^2 - dy^2)) / ry), integer throughout (isqrt is
// exact), and the row then covers [cx-w, cx+w] — 2r+1 pixels across the
// middle, matching the midpoint-circle convention that (cx±r, cy) lies on the
// circle. The outline is the fill minus the next row outward's fill, plus the
// edge pixel each side, so it is one pixel thick and connected everywhere.
// A negative radius draws nothing; a zero radius degenerates to a segment.
inline void ellipse(int64_t cx, int64_t cy, int64_t rx, int64_t ry,
                    uint32_t rgba, bool fill) {
  if (rx < 0 || ry < 0) return;
  Target t = resolve_target();
  cx = guard(cx);
  cy = guard(cy);
  rx = guard(rx);
  ry = guard(ry);
  // Row half-width at |dy| <= ry. rx * isqrt <= 2^60, inside int64.
  auto half_w = [&](int64_t ady) {
    return ry == 0 ? rx : rx * isqrt(ry * ry - ady * ady) / ry;
  };
  int64_t y0 = std::max<int64_t>(cy - ry, 0);
  int64_t y1 = std::min<int64_t>(cy + ry, t.h - 1);
  for (int64_t y = y0; y <= y1; y++) {
    int64_t ady = std::llabs(y - cy);
    int64_t w = half_w(ady);
    if (fill) {
      span(t, y, cx - w, cx + w + 1, rgba);
      continue;
    }
    // wn = the next row outward's half-width (monotonically narrower toward
    // the poles; -1 on the pole rows, which then fill entirely). The left
    // span covers this row's fill that the next row lacks — at least the edge
    // pixel — and the right span starts no earlier than the left one ended,
    // so no pixel is ever written twice (compositing would double up).
    int64_t wn = ady == ry ? -1 : half_w(ady + 1);
    int64_t le = std::min(std::max(cx - wn, cx - w + 1), cx + w + 1);
    span(t, y, cx - w, le, rgba);
    span(t, y, std::max(std::min(cx + wn + 1, cx + w), le), cx + w + 1, rgba);
  }
}

// Registered 8x8 bitmap fonts: a flat table of 8 row-bytes per glyph, MSB the
// leftmost pixel. Handles index the registry, so a font crosses the FFI
// boundary once at module load instead of per character drawn.
inline std::vector<std::vector<uint8_t>>& _fonts() {
  static std::vector<std::vector<uint8_t>> f;
  return f;
}

inline int64_t font_load(const uint8_t* rows, int64_t n) {
  if (n < 0) n = 0;
  _fonts().emplace_back(rows, rows + n);
  return static_cast<int64_t>(_fonts().size() - 1);
}

// Draw glyph `index` of font `id` at (x, y), clipped, each font pixel as a
// scale x scale block. A ZERO bit is a lit pixel — the WASM-4 runtime font's
// convention, which is the font the Canvas preamble ships. An unknown handle
// or an index past the table draws nothing, so a bad glyph can't read out of
// bounds; a non-positive scale draws nothing (the draw_scaled convention).
inline void glyph(int64_t id, int64_t index, int x, int y, uint32_t rgba,
                  int64_t scale) {
  if (scale <= 0) return;
  if (id < 0 || static_cast<size_t>(id) >= _fonts().size()) return;
  const std::vector<uint8_t>& f = _fonts()[id];
  // Bound the index against the glyph COUNT, never against `index * 8`: that
  // product wraps for a large index, which would let the check pass and read
  // off the front of the table.
  if (index < 0 || static_cast<size_t>(index) >= f.size() / 8) return;
  size_t base = static_cast<size_t>(index) * 8;
  Target t = resolve_target();
  // Walk the clipped destination rows and read the bit row back, so a huge
  // scale costs the rows visible, not 8 * scale iterations.
  scale = std::min(scale, kGuard);
  int64_t y0 = std::max<int64_t>(y, 0);
  int64_t y1 = std::min<int64_t>(y + 8 * scale, t.h);
  for (int64_t yy = y0; yy < y1; yy++) {
    uint8_t bits = f[base + static_cast<size_t>((yy - y) / scale)];
    for (int rx = 0; rx < 8; rx++)
      if (((bits >> (7 - rx)) & 1) == 0)
        span(t, yy, x + rx * scale, x + (rx + 1) * scale, rgba);
  }
}

// Register a sprite from a flat array of packed-RGBA pixels (row-major, w*h).
// Returns its handle. The pixel count is normalised to w*h (padded with
// transparent / truncated) so a short or long array can't read out of bounds
// at blit time.
inline int64_t sprite_load(const uint32_t* px, int64_t n, int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  Sprite s;
  s.w = w;
  s.h = h;
  if (n < 0) n = 0;
  s.px.assign(px, px + n);
  s.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
  _sprites().push_back(std::move(s));
  return static_cast<int64_t>(_sprites().size() - 1);
}

// Register a sprite from already-decoded pixels, taking ownership of the
// buffer — the PNG path, where the pixels never pass through a culebra Array.
inline int64_t sprite_adopt(std::vector<uint32_t>&& px, int w, int h) {
  Sprite s;
  s.w = w < 0 ? 0 : w;
  s.h = h < 0 ? 0 : h;
  s.px = std::move(px);
  s.px.resize(static_cast<size_t>(s.w) * static_cast<size_t>(s.h), 0u);
  _sprites().push_back(std::move(s));
  return static_cast<int64_t>(_sprites().size() - 1);
}

// A registered sprite's dimensions, or 0 for an unknown handle. `from_png`
// learns its size from the decode, so the preamble reads it back through these
// rather than being told it at construction.
inline int64_t sprite_width(int64_t id) {
  if (id < 0 || static_cast<size_t>(id) >= _sprites().size()) return 0;
  return _sprites()[id].w;
}
inline int64_t sprite_height(int64_t id) {
  if (id < 0 || static_cast<size_t>(id) >= _sprites().size()) return 0;
  return _sprites()[id].h;
}

// Blit the sub-rectangle (sx, sy, sw, sh) of sprite `id` to (dx, dy).
// flags: 1 = flip X, 2 = flip Y, 4 = transpose (swap X/Y — a diagonal
// reflection; combine with a flip for a 90° rotation). Source alpha
// composites through put(): 0 is skipped, 255 written as-is, and a partial
// alpha (a PNG's anti-aliased edge) blends over what is there.
inline void blit(int64_t id, int dx, int dy, int sx, int sy, int sw, int sh,
                 int64_t flags) {
  if (id < 0 || static_cast<size_t>(id) >= _sprites().size()) return;
  const Sprite& s = _sprites()[id];
  Target t = resolve_target();
  bool flip_x = (flags & 1) != 0;
  bool flip_y = (flags & 2) != 0;
  bool transpose = (flags & 4) != 0;
  for (int j = 0; j < sh; j++) {
    for (int i = 0; i < sw; i++) {
      int srcx = sx + i;
      int srcy = sy + j;
      if (srcx < 0 || srcy < 0 || srcx >= s.w || srcy >= s.h) continue;
      uint32_t p = s.px[static_cast<size_t>(srcy) * s.w + srcx];
      int lx = flip_x ? sw - 1 - i : i;
      int ly = flip_y ? sh - 1 - j : j;
      if (transpose)
        put(t, dx + ly, dy + lx, p);
      else
        put(t, dx + lx, dy + ly, p);
    }
  }
}

// Walks a source axis in lockstep with a destination axis: `pos` is exactly
// (u * num) / den for the current u, carried forward by an add and a compare.
// A scaling blit maps every destination pixel back to the source, and doing
// that with a division costs more than fetching the pixel it names.
struct SourceWalk {
  int64_t pos;    // (u * num) / den
  int64_t err;    // (u * num) % den — what the floor dropped
  int64_t step;   // num / den
  int64_t rem;    // num % den
  int64_t den;

  SourceWalk(int64_t u, int64_t num, int64_t d)
      : pos(u * num / d), err(u * num % d), step(num / d), rem(num % d),
        den(d) {}

  void next() {  // u + 1
    pos += step;
    err += rem;
    if (err >= den) {
      err -= den;
      pos++;
    }
  }
  void prev() {  // u - 1
    pos -= step;
    err -= rem;
    if (err < 0) {
      err += den;
      pos--;
    }
  }
};

// Blit the sub-rectangle (sx, sy, sw, sh) of sprite `id` into the destination
// rectangle (dx, dy, dw, dh), resampling to fit.
//
// flags: 1 = flip X, 2 = flip Y, 8 = box-average the source when shrinking
// (ignored when neither axis shrinks; the default is nearest-neighbour, so
// pixel art scaled up stays crisp and existing 1:1 callers are unaffected).
// Transpose has no scaled form — swapping axes here would need two source
// extents per destination axis.
//
// `alpha` (0..255) scales the whole blit: each pixel composites at its own
// source alpha times `alpha` (integer, (sa*a + 127)/255), so an opaque pixel
// under alpha 255 is written unchanged (the fast path, bit-identical to blit)
// and a PNG's anti-aliased edge blends the same way it does un-scaled. A
// fully transparent source pixel is skipped and never contributes, whether it
// is sampled directly or averaged over.
//
// The loop walks the (clipped) destination and maps back to the source, so
// cost is the pixels actually written, and nothing is drawn for a degenerate
// destination or source rectangle.
inline void blit_scaled(int64_t id, int64_t dx, int64_t dy, int64_t dw,
                        int64_t dh, int64_t sx, int64_t sy, int64_t sw,
                        int64_t sh, int64_t flags, int64_t alpha) {
  if (id < 0 || static_cast<size_t>(id) >= _sprites().size()) return;
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || alpha <= 0) return;
  if (alpha > 255) alpha = 255;
  const Sprite& s = _sprites()[id];
  Target t = resolve_target();
  bool flip_x = (flags & 1) != 0;
  bool flip_y = (flags & 2) != 0;
  bool smooth = (flags & 8) != 0 && (dw < sw || dh < sh);
  int64_t x0 = std::max<int64_t>(dx, 0);
  int64_t x1 = std::min<int64_t>(dx + dw, t.w);
  int64_t y0 = std::max<int64_t>(dy, 0);
  int64_t y1 = std::min<int64_t>(dy + dh, t.h);
  // u and v are the destination offsets read back through the flips; both stay
  // within [0, dw) and [0, dh) after the clip, so the divisions floor.
  int64_t u0 = flip_x ? dx + dw - 1 - x0 : x0 - dx;
  uint32_t a = static_cast<uint32_t>(alpha);
  for (int64_t y = y0; y < y1; y++) {
    int64_t v = flip_y ? dy + dh - 1 - y : y - dy;
    int64_t j0 = v * sh / dh;
    int64_t j1 = smooth ? std::max((v + 1) * sh / dh, j0 + 1) : j0 + 1;
    uint32_t* row = t.px + static_cast<size_t>(y) * t.w;
    SourceWalk u(u0, sw, dw);

    if (!smooth) {
      // Nearest neighbour, which is every caller that isn't shrinking with
      // averaging asked for: one source pixel per destination pixel, and one
      // source row for the whole destination row.
      int64_t srcy = sy + j0;
      if (srcy < 0 || srcy >= s.h) continue;
      const uint32_t* src_row =
          s.px.data() + static_cast<size_t>(srcy) * s.w;
      for (int64_t x = x0; x < x1; x++) {
        int64_t srcx = sx + u.pos;
        if (flip_x)
          u.prev();
        else
          u.next();
        if (srcx < 0 || srcx >= s.w) continue;
        uint32_t src = src_row[srcx];
        uint32_t ea = ((src >> 24) * a + 127) / 255;
        if (ea == 0) continue;
        row[x] = ea == 255 ? src : blend_over(src, row[x], ea);
      }
      continue;
    }

    // Box average: the source span each destination pixel covers, which the
    // walk one step ahead names.
    SourceWalk un(u0 + 1, sw, dw);
    for (int64_t x = x0; x < x1; x++) {
      int64_t i0 = u.pos;
      int64_t i1 = std::max(un.pos, i0 + 1);
      if (flip_x) {
        u.prev();
        un.prev();
      } else {
        u.next();
        un.next();
      }
      // Count only the opaque samples, so a sprite's transparent margin
      // doesn't bleed into its edge.
      uint32_t acc[4] = {0, 0, 0, 0};
      uint32_t n = 0;
      for (int64_t j = j0; j < j1; j++) {
        int64_t srcy = sy + j;
        if (srcy < 0 || srcy >= s.h) continue;
        for (int64_t i = i0; i < i1; i++) {
          int64_t srcx = sx + i;
          if (srcx < 0 || srcx >= s.w) continue;
          uint32_t p = s.px[static_cast<size_t>(srcy) * s.w + srcx];
          if ((p >> 24) == 0) continue;
          acc[0] += p & 0xff;
          acc[1] += (p >> 8) & 0xff;
          acc[2] += (p >> 16) & 0xff;
          acc[3] += p >> 24;
          n++;
        }
      }
      if (n == 0) continue;
      uint32_t src = (acc[0] / n) | ((acc[1] / n) << 8) |
                     ((acc[2] / n) << 16) | ((acc[3] / n) << 24);
      uint32_t ea = ((acc[3] / n) * a + 127) / 255;
      if (ea == 0) continue;
      row[x] = ea == 255 ? src : blend_over(src, row[x], ea);
    }
  }
}

// The audio container `data` opens with, named as the extension string
// raylib's decoder dispatch keys on: an ID3v2 tag or an MPEG frame sync
// (0xFF then the top three bits set — MPEG2/mono encodes don't start 0xFF
// 0xFB) is MP3, an "OggS" capture pattern is Ogg, anything else (or a byte
// count past raylib's int size) is nullptr. This sniff runs before any
// backend branch: the browser's own decodeAudioData is asynchronous, so
// leaving detection to it would give wasm a different error site than the
// other backends.
inline const char* music_format(const uint8_t* p, size_t n) {
  if (p == nullptr || n < 4 || n > static_cast<size_t>(INT32_MAX))
    return nullptr;
  if (p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') return ".ogg";
  if (p[0] == 'I' && p[1] == 'D' && p[2] == '3') return ".mp3";
  if (p[0] == 0xff && (p[1] & 0xe0) == 0xe0) return ".mp3";
  return nullptr;
}

// The one message both backends raise for bytes that are neither MP3 nor Ogg,
// the music analogue of Sprite.from_png's ValueError.
inline constexpr auto kMusicFormatError = "not a valid MP3 or Ogg audio stream";

#if defined(__EMSCRIPTEN__)

// Browser backend for the Playground. present posts the framebuffer to the
// page (main thread turns it into putImageData) and, with JSPI, suspends the
// wasm call until the next animation-frame tick — the frame-driven analogue of
// the TUI backend's read_key wait. Input and tone read/notify JS-side state
// the frontend maintains; see playground/worker.js and app.js.

#if defined(CULEBRA_WASM_JSPI)
// Post the frame, then suspend (via JSPI) until worker.js's self.__nextFrame
// resolves — the main thread's requestAnimationFrame loop forwards a "tick"
// that resolves it (with a setTimeout fallback so a backgrounded tab, where
// rAF stalls, doesn't wedge the run).
EM_ASYNC_JS(void, _wasm_present, (int w, int h, const uint8_t* buf, int len), {
  postMessage({ type: "frame", w: w, h: h, buf: HEAPU8.slice(buf, buf + len) });
  await self.__nextFrame();
});
#else
// No JSPI in this build: post the frame but don't wait — the loop runs as fast
// as the interpreter allows and the page shows whatever it last received.
EM_JS(void, _wasm_present, (int w, int h, const uint8_t* buf, int len), {
  postMessage({ type: "frame", w: w, h: h, buf: HEAPU8.slice(buf, buf + len) });
});
#endif  // CULEBRA_WASM_JSPI

// self.__canvasButtons / __canvasMouse* are kept current by worker.js from the
// page's keydown/keyup/pointer events (app.js). Plain EM_JS — reading JS state
// is synchronous, like the TUI backend's cols()/rows().
EM_JS(int, _wasm_canvas_buttons, (), { return self.__canvasButtons || 0; });
EM_JS(int, _wasm_canvas_mouse_x, (), { return self.__canvasMouseX || 0; });
EM_JS(int, _wasm_canvas_mouse_y, (), { return self.__canvasMouseY || 0; });
EM_JS(int, _wasm_canvas_mouse_buttons, (), { return self.__canvasMouseButtons || 0; });
// A WASM-4-style tone: a note that slides start->end frequency over its life
// under an ADSR envelope, on one of four channels (two pulse waves with a duty
// cycle, a triangle, and noise). Times are in frames at ~60fps; volume/peak are
// 0..100. The frontend (app.js playTone) turns this into WebAudio nodes.
EM_JS(void, _wasm_canvas_tone,
      (int start_freq, int end_freq, int attack, int decay, int sustain,
       int release, int vol, int peak, int channel, int duty), {
  postMessage({ type: "tone", startFreq: start_freq, endFreq: end_freq,
                attack: attack, decay: decay, sustain: sustain,
                release: release, vol: vol, peak: peak, channel: channel,
                duty: duty });
});
// Music playback lives on the main thread (app.js decodes and drives WebAudio);
// these post the commands over. __musicLoaded/__musicPlaying are updated
// optimistically here, synchronously with the call — the state a script reads
// right back — and the main thread pushes corrections (a failed decode, a
// non-looping file ending) as "musicState" messages worker.js applies.
EM_JS(void, _wasm_canvas_music_play,
      (const uint8_t* buf, int len, int looping, int vol, double start), {
  self.__musicLoaded = true;
  self.__musicPlaying = true;
  postMessage({ type: "music", cmd: "play", buf: HEAPU8.slice(buf, buf + len),
                loop: looping !== 0, vol: vol, start: start });
});
EM_JS(void, _wasm_canvas_music_stop, (), {
  self.__musicLoaded = false;
  self.__musicPlaying = false;
  postMessage({ type: "music", cmd: "stop" });
});
EM_JS(void, _wasm_canvas_music_pause, (), {
  self.__musicPlaying = false;
  postMessage({ type: "music", cmd: "pause" });
});
EM_JS(void, _wasm_canvas_music_resume, (), {
  if (self.__musicLoaded) self.__musicPlaying = true;
  postMessage({ type: "music", cmd: "resume" });
});
EM_JS(void, _wasm_canvas_music_volume, (int vol), {
  postMessage({ type: "music", cmd: "volume", vol: vol });
});
EM_JS(void, _wasm_canvas_music_seek, (double seconds), {
  postMessage({ type: "music", cmd: "seek", seconds: seconds });
});
EM_JS(int, _wasm_canvas_music_playing, (), {
  return self.__musicPlaying ? 1 : 0;
});

inline void present() {
  auto& fb = _fb();
  _wasm_present(_fb_w(), _fb_h(), reinterpret_cast<const uint8_t*>(fb.data()),
                static_cast<int>(fb.size() * 4));
}
inline int64_t buttons() { return _wasm_canvas_buttons(); }
inline int64_t mouse_x() { return _wasm_canvas_mouse_x(); }
inline int64_t mouse_y() { return _wasm_canvas_mouse_y(); }
inline int64_t mouse_buttons() { return _wasm_canvas_mouse_buttons(); }
// The browser loop ends via tick()/frames, not a window-close event.
inline bool closing() { return false; }
inline void tone(int64_t start_freq, int64_t end_freq, int64_t attack,
                 int64_t decay, int64_t sustain, int64_t release, int64_t vol,
                 int64_t peak, int64_t channel, int64_t duty) {
  _wasm_canvas_tone(static_cast<int>(start_freq), static_cast<int>(end_freq),
                    static_cast<int>(attack), static_cast<int>(decay),
                    static_cast<int>(sustain), static_cast<int>(release),
                    static_cast<int>(vol), static_cast<int>(peak),
                    static_cast<int>(channel), static_cast<int>(duty));
}
// `fmt` is for raylib's decoder dispatch; the browser sniffs the bytes itself.
inline void music_play(const uint8_t* data, int64_t len, const char* /*fmt*/,
                       int64_t looping, int64_t vol, double start) {
  _wasm_canvas_music_play(data, static_cast<int>(len),
                          static_cast<int>(looping), static_cast<int>(vol),
                          start);
}
inline void music_stop() { _wasm_canvas_music_stop(); }
inline void music_pause() { _wasm_canvas_music_pause(); }
inline void music_resume() { _wasm_canvas_music_resume(); }
inline void music_volume(int64_t vol) {
  _wasm_canvas_music_volume(static_cast<int>(vol));
}
inline void music_seek(double seconds) { _wasm_canvas_music_seek(seconds); }
inline bool music_playing() { return _wasm_canvas_music_playing() != 0; }

#elif defined(CULEBRA_CANVAS_WINDOW)  // native raylib desktop window

#if defined(CULEBRA_RT_CANVAS_WEAK)

// Weak headless stubs for the base AOT runtime archive of a window build: a
// program that never uses Canvas links these and pulls in no raylib. The strong
// raylib bodies in the canvas feature archive (culebra_rt_canvas.cc,
// force-loaded only when the AST scan reports Canvas use) override them — the
// same weak choke as compress / http / sqlite.
__attribute__((weak)) void present() {}
__attribute__((weak)) int64_t buttons() { return 0; }
__attribute__((weak)) int64_t mouse_x() { return 0; }
__attribute__((weak)) int64_t mouse_y() { return 0; }
__attribute__((weak)) int64_t mouse_buttons() { return 0; }
__attribute__((weak)) bool closing() { return false; }
__attribute__((weak)) void tone(int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t, int64_t, int64_t) {}
__attribute__((weak)) void music_play(const uint8_t*, int64_t, const char*,
                                      int64_t, int64_t, double) {}
__attribute__((weak)) void music_stop() {}
__attribute__((weak)) void music_pause() {}
__attribute__((weak)) void music_resume() {}
__attribute__((weak)) void music_volume(int64_t) {}
__attribute__((weak)) void music_seek(double) {}
__attribute__((weak)) bool music_playing() { return false; }

#else

// Strong raylib bodies live in src/runtime/culebra_rt_canvas.cc (the driver's
// in-process backend + the force-loaded feature archive). Declarations only
// here so the framebuffer core header stays raylib-free everywhere else.
void present();
int64_t buttons();
int64_t mouse_x();
int64_t mouse_y();
int64_t mouse_buttons();
bool closing();
void tone(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
          int64_t, int64_t, int64_t);
void music_play(const uint8_t* data, int64_t len, const char* fmt,
                int64_t looping, int64_t vol, double start);
void music_stop();
void music_pause();
void music_resume();
void music_volume(int64_t vol);
void music_seek(double seconds);
bool music_playing();

#endif  // CULEBRA_RT_CANVAS_WEAK

#else  // native headless (default): frame held, nothing shown, no input

inline void present() {}  // headless: the framebuffer is held, nothing shown
inline int64_t buttons() { return 0; }
inline int64_t mouse_x() { return 0; }
inline int64_t mouse_y() { return 0; }
inline int64_t mouse_buttons() { return 0; }
inline bool closing() { return false; }
inline void tone(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                 int64_t, int64_t, int64_t) {}
inline void music_play(const uint8_t*, int64_t, const char*, int64_t, int64_t,
                       double) {}
inline void music_stop() {}
inline void music_pause() {}
inline void music_resume() {}
inline void music_volume(int64_t) {}
inline void music_seek(double) {}
inline bool music_playing() { return false; }

#endif  // __EMSCRIPTEN__

}  // namespace _canvas_detail
}  // namespace culebra
