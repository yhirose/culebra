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

inline void clear(uint32_t rgba) { std::fill(_fb().begin(), _fb().end(), rgba); }

inline void set_pixel(int x, int y, uint32_t rgba) {
  if (x < 0 || y < 0 || x >= _fb_w() || y >= _fb_h()) return;
  _fb()[static_cast<size_t>(y) * _fb_w() + x] = rgba;
}

// The framebuffer pixel at (x, y), or 0 (transparent) off-buffer. Exposed as
// `Canvas.get` for pixel-readback collision (reading back what was drawn).
inline uint32_t get_pixel(int x, int y) {
  if (x < 0 || y < 0 || x >= _fb_w() || y >= _fb_h()) return 0;
  return _fb()[static_cast<size_t>(y) * _fb_w() + x];
}

// Filled rectangle, clipped to the framebuffer.
inline void rect(int x, int y, int w, int h, uint32_t rgba) {
  int x0 = std::max(0, x);
  int y0 = std::max(0, y);
  int x1 = std::min(_fb_w(), x + w);
  int y1 = std::min(_fb_h(), y + h);
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++)
      _fb()[static_cast<size_t>(yy) * _fb_w() + xx] = rgba;
}

// Division rounding toward -inf. Edge interpolation uses it so an edge that
// crosses x = 0 keeps its slope instead of kinking the way C's truncation
// toward zero would.
inline int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
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
inline void polygon(const int64_t* pts, int64_t n, uint32_t rgba) {
  if (pts == nullptr || n < 3) return;
  auto vx = [&](int64_t i) { return guard(pts[2 * i]); };
  auto vy = [&](int64_t i) { return guard(pts[2 * i + 1]); };
  int64_t ymin = vy(0), ymax = vy(0);
  for (int64_t i = 1; i < n; i++) {
    ymin = std::min(ymin, vy(i));
    ymax = std::max(ymax, vy(i));
  }
  int64_t y0 = std::max<int64_t>(ymin, 0);
  int64_t y1 = std::min<int64_t>(ymax, _fb_h());
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
    uint32_t* row = _fb().data() + static_cast<size_t>(y) * _fb_w();
    for (size_t k = 0; k + 1 < xs.size(); k += 2) {
      int64_t x0 = std::max<int64_t>(xs[k], 0);
      int64_t x1 = std::min<int64_t>(xs[k + 1], _fb_w());
      for (int64_t x = x0; x < x1; x++) row[x] = rgba;
    }
  }
}

// Filled triangle — the conventional general shape (raylib, SDL and every GPU
// rasterizer take three vertices), and the same even-odd fill as polygon.
inline void triangle(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t x3,
                     int64_t y3, uint32_t rgba) {
  const int64_t pts[6] = {x1, y1, x2, y2, x3, y3};
  polygon(pts, 3, rgba);
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

// Draw glyph `index` of font `id` at (x, y), clipped. A ZERO bit is a lit
// pixel — the WASM-4 runtime font's convention, which is the font the Canvas
// preamble ships. An unknown handle or an index past the table draws nothing,
// so a bad glyph can't read out of bounds.
inline void glyph(int64_t id, int64_t index, int x, int y, uint32_t rgba) {
  if (id < 0 || static_cast<size_t>(id) >= _fonts().size()) return;
  const std::vector<uint8_t>& f = _fonts()[id];
  // Bound the index against the glyph COUNT, never against `index * 8`: that
  // product wraps for a large index, which would let the check pass and read
  // off the front of the table.
  if (index < 0 || static_cast<size_t>(index) >= f.size() / 8) return;
  size_t base = static_cast<size_t>(index) * 8;
  for (int ry = 0; ry < 8; ry++) {
    uint8_t bits = f[base + static_cast<size_t>(ry)];
    for (int rx = 0; rx < 8; rx++)
      if (((bits >> (7 - rx)) & 1) == 0) set_pixel(x + rx, y + ry, rgba);
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
// reflection; combine with a flip for a 90° rotation). Transparent source
// pixels (alpha 0) are skipped, so sprites composite.
inline void blit(int64_t id, int dx, int dy, int sx, int sy, int sw, int sh,
                 int64_t flags) {
  if (id < 0 || static_cast<size_t>(id) >= _sprites().size()) return;
  const Sprite& s = _sprites()[id];
  bool flip_x = (flags & 1) != 0;
  bool flip_y = (flags & 2) != 0;
  bool transpose = (flags & 4) != 0;
  for (int j = 0; j < sh; j++) {
    for (int i = 0; i < sw; i++) {
      int srcx = sx + i;
      int srcy = sy + j;
      if (srcx < 0 || srcy < 0 || srcx >= s.w || srcy >= s.h) continue;
      uint32_t p = s.px[static_cast<size_t>(srcy) * s.w + srcx];
      if ((p >> 24) == 0) continue;  // alpha 0 = transparent
      int lx = flip_x ? sw - 1 - i : i;
      int ly = flip_y ? sh - 1 - j : j;
      if (transpose)
        set_pixel(dx + ly, dy + lx, p);
      else
        set_pixel(dx + lx, dy + ly, p);
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

// Composite `src` over `dst` at `alpha`/255, channel by channel and in
// integers only, so the three backends round identically.
inline uint32_t blend_over(uint32_t src, uint32_t dst, uint32_t alpha) {
  uint32_t out = 0;
  for (int c = 0; c < 4; c++) {
    uint32_t sc = (src >> (c * 8)) & 0xff;
    uint32_t dc = (dst >> (c * 8)) & 0xff;
    uint32_t m = (sc * alpha + dc * (255 - alpha) + 127) / 255;
    out |= m << (c * 8);
  }
  return out;
}

// Blit the sub-rectangle (sx, sy, sw, sh) of sprite `id` into the destination
// rectangle (dx, dy, dw, dh), resampling to fit.
//
// flags: 1 = flip X, 2 = flip Y, 8 = box-average the source when shrinking
// (ignored when neither axis shrinks; the default is nearest-neighbour, so
// pixel art scaled up stays crisp and existing 1:1 callers are unaffected).
// Transpose has no scaled form — swapping axes here would need two source
// extents per destination axis.
//
// `alpha` (0..255) composites the whole blit: 255 writes the source pixel
// unchanged (the fast path, bit-identical to blit), 0 draws nothing, and in
// between each channel becomes (src*a + dst*(255-a) + 127) / 255 — integer
// only, so the three backends round identically. Source alpha stays the shape
// mask it is for sprites: a fully transparent source pixel is skipped and never
// contributes, whether it is sampled directly or averaged over.
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
  bool flip_x = (flags & 1) != 0;
  bool flip_y = (flags & 2) != 0;
  bool smooth = (flags & 8) != 0 && (dw < sw || dh < sh);
  int64_t x0 = std::max<int64_t>(dx, 0);
  int64_t x1 = std::min<int64_t>(dx + dw, _fb_w());
  int64_t y0 = std::max<int64_t>(dy, 0);
  int64_t y1 = std::min<int64_t>(dy + dh, _fb_h());
  // u and v are the destination offsets read back through the flips; both stay
  // within [0, dw) and [0, dh) after the clip, so the divisions floor.
  int64_t u0 = flip_x ? dx + dw - 1 - x0 : x0 - dx;
  uint32_t a = static_cast<uint32_t>(alpha);
  for (int64_t y = y0; y < y1; y++) {
    int64_t v = flip_y ? dy + dh - 1 - y : y - dy;
    int64_t j0 = v * sh / dh;
    int64_t j1 = smooth ? std::max((v + 1) * sh / dh, j0 + 1) : j0 + 1;
    uint32_t* row = _fb().data() + static_cast<size_t>(y) * _fb_w();
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
        if ((src >> 24) == 0) continue;  // alpha 0 = transparent
        row[x] = a == 255 ? src : blend_over(src, row[x], a);
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
      row[x] = a == 255 ? src : blend_over(src, row[x], a);
    }
  }
}

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

#endif  // __EMSCRIPTEN__

}  // namespace _canvas_detail
}  // namespace culebra
