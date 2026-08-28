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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <id_registry.h>  // IdRegistry<T> (slot+generation handle table)
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace culebra {
namespace _canvas_detail {

// A registered sprite: RGBA pixels plus its dimensions. Registration is
// upload-once and blit re-references the handle (no per-frame pixel
// marshalling across the FFI boundary).
//
// A handle is an IdRegistry id (id_registry.h): freeing a sprite bumps its
// slot's generation, so a stale handle resolves to nothing rather than to
// whichever sprite the slot holds next. 0 is never a sprite handle — it names
// the framebuffer as the draw target (see _sprites()).
struct Sprite {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> px;
};

// This state is process-global and unsynchronized: drawing is a single-thread
// activity. The framebuffer reallocates and sprites are deleted on free, so a
// SECOND isolate drawing would free the buffer the first is drawing through —
// an init() or a sprite free against a live blit. One isolate owns the state
// instead: the first thread to reach a seam that claims it (init,
// resolve_target, sprite_of, sprite_adopt, target — `git grep own_canvas`).
// Every other thread finds an empty canvas, so its draws clip themselves away;
// the seams that can report say why (see refusal()), which is how a drawing
// isolate hears about it at its first call rather than through a window that
// stays blank.
//
// Deliberately outside the claim: font_load, which every isolate reaches as it
// resolves the Canvas module (that is why the font registry carries a lock
// instead), and the window backend's read of _fb(), which belongs to whichever
// thread drives the window.
inline constexpr auto kBusyError =
    "another isolate is already drawing to the canvas";
inline bool own_canvas() {
  static std::atomic<bool> claimed{false};
  thread_local int mine = 0;   // 0 = never asked, 1 = owner, 2 = not
  if (mine == 0) {
    // Both answers are final — the claim is never released — so a plain
    // thread_local caches it, which an atomic load could not: the seams that
    // ask twice per call (resolve_target, blit, glyph) then check once.
    bool unclaimed = false;
    mine = claimed.compare_exchange_strong(unclaimed, true,
                                           std::memory_order_relaxed) ? 1 : 2;
  }
  return mine == 1;
}

inline int& _fb_w() { static int w = 0; return w; }
inline int& _fb_h() { static int h = 0; return h; }
inline std::vector<uint32_t>& _fb() { static std::vector<uint32_t> b; return b; }
// Slot 0 is burned on construction so no sprite can ever be handle 0, the id
// that names the framebuffer: it resolves to the null slot through the
// registry's ordinary bounds/generation path, so sprite_of needs no case for
// it.
inline IdRegistry<Sprite>& _sprites() {
  static IdRegistry<Sprite> r = [] {
    IdRegistry<Sprite> t;
    t.add(nullptr);
    return t;
  }();
  return r;
}

// The live sprite a handle names, or nullptr for anything else (freed, stale
// generation, never valid). Each sprite is its own allocation, so the pointer
// survives other registrations — but not that sprite's own free.
inline Sprite* sprite_of(int64_t id) {
  if (!own_canvas()) return nullptr;
  return _sprites().get(id);
}

// (Re)allocate the framebuffer to w*h transparent pixels. The sprite registry
// is deliberately NOT cleared: sprites are typically registered once at top
// level before the loop first calls init(), so clearing here would invalidate
// their handles. It is process-lifetime state, reclaimed at process exit (or,
// in the Playground, when Stop respawns the worker).
// False when another isolate owns the canvas (the caller raises kBusyError).
inline bool init(int w, int h) {
  if (!own_canvas()) return false;
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  _fb_w() = w;
  _fb_h() = h;
  _fb().assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
  return true;
}

// Declared ahead: width/height report the CURRENT draw target (the
// framebuffer, or the sprite draw_to switched to), so centring code keeps
// working offscreen.
inline int width();
inline int height();

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
// (id 0) or a sprite draw_to switched to. Held as an id and resolved per call
// — never cached as a pointer, because init() reallocates the framebuffer and
// a sprite's pixels go away with the sprite when it is freed.
struct Target {
  uint32_t* px;
  int w;
  int h;
};
inline int64_t& _target_id() { static int64_t id = 0; return id; }
inline Target resolve_target() {
  if (!own_canvas()) return {nullptr, 0, 0};
  if (_target_id() != 0) {
    // Freeing the current target is refused at the seam, so this resolves as
    // long as the id is set.
    if (Sprite* s = sprite_of(_target_id())) return {s->px.data(), s->w, s->h};
    _target_id() = 0;
  }
  return {_fb().data(), _fb_w(), _fb_h()};
}

inline int width() { return resolve_target().w; }
inline int height() { return resolve_target().h; }

// --- the screen layer ------------------------------------------------------
// A SECOND buffer, at the size the framebuffer is actually presented at, for
// the one thing the framebuffer's nearest-neighbour upscale cannot carry:
// antialiased text. A glyph rasterized into the framebuffer has its soft edge
// magnified into blocks by present(); rasterized at the presented size and
// blitted 1:1 instead, it stays crisp. Everything else — sprites, shapes, the
// 8x8 bitmap font — wants the blocky upscale and stays where it is.
//
// This is the standard split a 2D engine makes between a low-resolution game
// world and a resolution-independent UI layer, with culebra's own constraint
// on top: the glyphs are rasterized by the same stb_truetype code (font_ttf.h)
// and composited through the same put()/blend_over() seam as everything else,
// never by the host's text API — raylib's DrawText and the browser's fillText
// would each round differently and break native/browser symmetry.
//
// The backend supplies exactly one fact, screen_scale(): framebuffer pixels ->
// presented pixels, the window's upscale times the display's DPI scale. It is
// 1.0 wherever there is no display (headless, a window that failed to open,
// the browser before the page has measured itself), which makes headless a
// real test of this path rather than a special case — at scale 1.0 a
// screen-layer glyph must land pixel-identical to a framebuffer one, which is
// what tests/test_canvas.cul asserts across all three backends.
//
// Declared here, defined per backend below. NOT inline: the window build's
// definition is in culebra_rt_canvas.cc, and an inline one would be invisible
// to every other translation unit.
double screen_scale();

inline int& _screen_w() { static int w = 0; return w; }
inline int& _screen_h() { static int h = 0; return h; }
inline std::vector<uint32_t>& _screen_fb() {
  static std::vector<uint32_t> b;
  return b;
}
// Whether anything drew here since the last present. Frames that drew no
// screen text then skip the clear and the upload entirely, which is most
// frames of most programs.
inline bool& _screen_dirty() { static bool d = false; return d; }

// Size the screen layer to the presented size, reallocating only when that
// actually changes. Resolved per call rather than cached: a native window's
// scale is fixed once it opens, but the browser's follows the pane and can
// change between frames.
inline void ensure_screen_buffer() {
  if (!own_canvas()) return;
  double s = screen_scale();
  int w = static_cast<int>(std::llround(_fb_w() * s));
  int h = static_cast<int>(std::llround(_fb_h() * s));
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  if (w == _screen_w() && h == _screen_h()) return;
  _screen_w() = w;
  _screen_h() = h;
  _screen_fb().assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
}

// The screen layer as a draw target. Deliberately ignores _target_id(): unlike
// every framebuffer draw, a screen-layer draw inside Canvas.draw_to still goes
// to the screen, because the layer exists to sit on top of the presented frame
// and a sprite is not presented.
inline Target resolve_screen_target() {
  if (!own_canvas()) return {nullptr, 0, 0};
  ensure_screen_buffer();
  return {_screen_fb().data(), _screen_w(), _screen_h()};
}

// Erase the screen layer — every backend's present() ends here, so unlike the
// framebuffer it is redrawn every frame by contract. It has to be: antialiased
// glyphs composited over themselves frame after frame darken every edge.
// Owner-gated because present() itself is not, and skipped when nothing drew
// (the only writer sets the flag, so a clean frame left it already zero).
inline void screen_buffer_reset() {
  if (!own_canvas() || !_screen_dirty()) return;
  _screen_dirty() = false;
  std::fill(_screen_fb().begin(), _screen_fb().end(), 0u);
}

// The pixels an id names for readback (PNG encoding): 0 follows the CURRENT
// draw target, the way width/height/get_pixel do, so `Canvas.to_png()` inside
// `draw_to` encodes the sprite being drawn into; any other id is that sprite.
// `px` is null for a stale handle — the caller raises with refusal().
inline Target readback_target(int64_t id) {
  if (id == 0) return resolve_target();
  if (Sprite* s = sprite_of(id)) return {s->px.data(), s->w, s->h};
  return {nullptr, 0, 0};
}

// Switch the draw target: 0 for the framebuffer, a sprite handle for its
// pixels. Returns the previous target id, or -1 when `id` names no live sprite
// (the caller raises with refusal(); the target is left unchanged). A
// non-owner is refused before the store: `id` 0 resolves no sprite, so nothing
// else here would stop it from redirecting the owner's drawing.
inline int64_t target(int64_t id) {
  if (!own_canvas()) return -1;
  if (id != 0 && sprite_of(id) == nullptr) return -1;
  int64_t prev = _target_id();
  _target_id() = id;
  return prev;
}

// Why a seam that resolves a handle refused. A non-owner is not holding a
// stale handle — it has no handles at all — and hearing about sprite lifetimes
// is the least useful thing it could be told.
struct Refusal { const char* kind; const char* message; };
inline constexpr auto kStaleSpriteError = "not a live sprite handle";
inline Refusal refusal() {
  return own_canvas() ? Refusal{"ValueError", kStaleSpriteError}
                      : Refusal{"RuntimeError", kBusyError};
}

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

// The screen layer's size and pixels. No `Canvas`-level surface wraps these:
// a script draws to the screen layer in logical coordinates and never needs to
// know what it was scaled to. They exist so the test suite can assert, in
// headless (where screen_scale() is 1.0 by construction), that a screen-layer
// glyph lands pixel-identical to a framebuffer one on all three backends.
inline int screen_width() { return resolve_screen_target().w; }
inline int screen_height() { return resolve_screen_target().h; }
inline uint32_t get_screen_pixel(int x, int y) {
  Target t = resolve_screen_target();
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
// boundary once at module load instead of per character drawn. Registered off
// the drawing thread (see the note above `_fb_w`), so the table is bundled
// with the lock that guards it — and it is a deque, whose entries keep their
// address, so `glyph` can read a font's bytes with the lock released.
struct FontTable {
  std::mutex m;
  std::deque<std::vector<uint8_t>> v;
};
inline FontTable& _fonts() {
  static FontTable t;
  return t;
}

// The bytes handle `id` names, or nullptr for a handle that names no font.
// Entries are never mutated after registration, so the pointer outlives the
// lock.
inline const std::vector<uint8_t>* font_of(int64_t id) {
  FontTable& t = _fonts();
  std::lock_guard<std::mutex> lk(t.m);
  if (id < 0 || static_cast<size_t>(id) >= t.v.size()) return nullptr;
  return &t.v[static_cast<size_t>(id)];
}

inline int64_t font_load(const uint8_t* rows, int64_t n) {
  if (n < 0) n = 0;
  FontTable& t = _fonts();
  std::lock_guard<std::mutex> lk(t.m);
  // Identical bytes reuse the handle: fonts never unregister, so repeated
  // isolate spawns would otherwise grow the table forever.
  for (size_t i = 0; i < t.v.size(); i++) {
    if (std::equal(t.v[i].begin(), t.v[i].end(), rows, rows + n))
      return static_cast<int64_t>(i);
  }
  t.v.emplace_back(rows, rows + n);
  return static_cast<int64_t>(t.v.size() - 1);
}

// Draw glyph `index` of font `id` at (x, y), clipped, each font pixel as a
// scale x scale block. A ZERO bit is a lit pixel — the WASM-4 runtime font's
// convention, which is the font the Canvas preamble ships. An unknown handle
// or an index past the table draws nothing, so a bad glyph can't read out of
// bounds; a non-positive scale draws nothing (the draw_scaled convention).
inline void glyph(int64_t id, int64_t index, int x, int y, uint32_t rgba,
                  int64_t scale) {
  if (scale <= 0) return;
  const std::vector<uint8_t>* fp = font_of(id);
  if (fp == nullptr) return;
  const std::vector<uint8_t>& f = *fp;
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

// Register a sprite from already-decoded pixels, taking ownership of the
// buffer.
inline int64_t sprite_adopt(std::vector<uint32_t>&& px, int w, int h) {
  if (!own_canvas()) return 0;
  auto s = std::make_unique<Sprite>();
  s->w = w < 0 ? 0 : w;
  s->h = h < 0 ? 0 : h;
  s->px = std::move(px);
  s->px.resize(static_cast<size_t>(s->w) * static_cast<size_t>(s->h), 0u);
  // s stays owning until add() has placed the pointer, so a growth-triggered
  // bad_alloc inside add() doesn't leak the pixels (same order as net.h).
  int64_t id = _sprites().add(s.get());
  s.release();
  return id;
}

// Register a sprite from a flat array of packed-RGBA pixels (row-major, w*h).
// Returns its handle. The pixel count is normalised to w*h (padded with
// transparent / truncated) so a short or long array can't read out of bounds
// at blit time.
inline int64_t sprite_load(const uint32_t* px, int64_t n, int w, int h) {
  if (n < 0) n = 0;
  return sprite_adopt(std::vector<uint32_t>(px, px + n), w, h);
}

// A blank sprite filled with one colour — the raw material of an offscreen
// draw target (Canvas.Sprite.blank + Canvas.draw_to).
inline int64_t sprite_blank(int w, int h, uint32_t rgba) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  return sprite_adopt(
      std::vector<uint32_t>(static_cast<size_t>(w) * static_cast<size_t>(h),
                            rgba),
      w, h);
}

// Free a sprite's pixels and retire its handle. Returns false when `id` is
// the current draw target (the caller raises — freeing what is being drawn
// to would leave the target dangling); an unknown or already-freed handle is
// a no-op, like blitting one.
inline constexpr auto kFreeTargetError =
    "cannot free the sprite currently drawn to";
inline bool sprite_free(int64_t id) {
  if (id != 0 && id == _target_id()) return false;
  Sprite* s = sprite_of(id);
  if (s == nullptr) return true;
  delete s;
  _sprites().invalidate(id);
  return true;
}

// A registered sprite's dimensions, or 0 for an unknown handle. `from_png`
// learns its size from the decode, so the preamble reads it back through these
// rather than being told it at construction.
inline int64_t sprite_width(int64_t id) {
  Sprite* s = sprite_of(id);
  return s == nullptr ? 0 : s->w;
}
inline int64_t sprite_height(int64_t id) {
  Sprite* s = sprite_of(id);
  return s == nullptr ? 0 : s->h;
}

// The one refusal shared by both blits: reading and writing the same pixels
// in one pass would see its own writes.
inline constexpr auto kSelfBlitError = "cannot draw a sprite onto itself";

// Blit the sub-rectangle (sx, sy, sw, sh) of sprite `id` to (dx, dy).
// flags: 1 = flip X, 2 = flip Y, 4 = transpose (swap X/Y — a diagonal
// reflection; combine with a flip for a 90° rotation). Source alpha
// composites through put(): 0 is skipped, 255 written as-is, and a partial
// alpha (a PNG's anti-aliased edge) blends over what is there. Returns false
// when `id` is the current draw target (the caller raises).
inline bool blit(int64_t id, int dx, int dy, int sx, int sy, int sw, int sh,
                 int64_t flags) {
  if (id != 0 && id == _target_id()) return false;
  const Sprite* sp = sprite_of(id);
  if (sp == nullptr) return true;
  const Sprite& s = *sp;
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
  return true;
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
inline bool blit_scaled(int64_t id, int64_t dx, int64_t dy, int64_t dw,
                        int64_t dh, int64_t sx, int64_t sy, int64_t sw,
                        int64_t sh, int64_t flags, int64_t alpha) {
  if (id != 0 && id == _target_id()) return false;
  const Sprite* sp = sprite_of(id);
  if (sp == nullptr) return true;
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || alpha <= 0) return true;
  if (alpha > 255) alpha = 255;
  const Sprite& s = *sp;
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
  return true;
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

// Sound effects (Canvas.Sound) accept WAV on top of music's MP3/Ogg — the
// natural container for a one-shot sample. Same sniff-before-backend shape.
inline const char* sound_format(const uint8_t* p, size_t n) {
  if (p != nullptr && n >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' &&
      p[3] == 'F' && p[8] == 'W' && p[9] == 'A' && p[10] == 'V' && p[11] == 'E')
    return ".wav";
  return music_format(p, n);
}
inline constexpr auto kSoundFormatError =
    "not a valid WAV, MP3 or Ogg audio stream";

// Sound handles are allocated here, one counter for every backend, so the
// script-visible lifecycle (load -> play/stop -> free) reads identically
// whether or not a host can actually decode and play the bytes.
inline int64_t sound_alloc_id() {
  static int64_t n = 0;
  return ++n;
}

#if defined(__EMSCRIPTEN__)

// Browser backend for the Playground. present posts the framebuffer to the
// page (main thread turns it into putImageData) and, with JSPI, suspends the
// wasm call until the next animation-frame tick — the frame-driven analogue of
// the TUI backend's read_key wait. Input and tone read/notify JS-side state
// the frontend maintains; see playground/worker.js and app.js.

// The screen layer rides along in the same message as the frame: one copy, one
// suspend point. `slen` is 0 on frames that drew no screen-layer text, which is
// most frames of most programs — the page then just clears its overlay.
#if defined(CULEBRA_WASM_JSPI)
// Post the frame, then suspend (via JSPI) until worker.js's self.__nextFrame
// resolves — the main thread's requestAnimationFrame loop forwards a "tick"
// that resolves it (with a setTimeout fallback so a backgrounded tab, where
// rAF stalls, doesn't wedge the run).
EM_ASYNC_JS(void, _wasm_present,
            (int w, int h, const uint8_t* buf, int len, int sw, int sh,
             const uint8_t* sbuf, int slen), {
  postMessage({ type: "frame", w: w, h: h, buf: HEAPU8.slice(buf, buf + len),
                screenW: sw, screenH: sh,
                screenBuf: slen ? HEAPU8.slice(sbuf, sbuf + slen) : null });
  await self.__nextFrame();
});
#else
// No JSPI in this build: post the frame but don't wait — the loop runs as fast
// as the interpreter allows and the page shows whatever it last received.
EM_JS(void, _wasm_present,
      (int w, int h, const uint8_t* buf, int len, int sw, int sh,
       const uint8_t* sbuf, int slen), {
  postMessage({ type: "frame", w: w, h: h, buf: HEAPU8.slice(buf, buf + len),
                screenW: sw, screenH: sh,
                screenBuf: slen ? HEAPU8.slice(sbuf, sbuf + slen) : null });
});
#endif  // CULEBRA_WASM_JSPI

// self.__canvasButtons / __canvasMouse* are kept current by worker.js from the
// page's keydown/keyup/pointer events (app.js). Plain EM_JS — reading JS state
// is synchronous, like the TUI backend's cols()/rows().
EM_JS(int, _wasm_canvas_buttons, (), { return self.__canvasButtons || 0; });
EM_JS(int, _wasm_canvas_mouse_x, (), { return self.__canvasMouseX || 0; });
EM_JS(int, _wasm_canvas_mouse_y, (), { return self.__canvasMouseY || 0; });
EM_JS(int, _wasm_canvas_mouse_buttons, (), { return self.__canvasMouseButtons || 0; });
// How far the page is stretching the framebuffer, in device pixels rather than
// CSS ones, kept current by app.js's ResizeObserver on the canvas pane. Missing
// until the page has measured itself (and in a hidden tab, which has no box to
// measure), so it defaults to 1.0 — the screen layer then matches the
// framebuffer instead of vanishing.
EM_JS(double, _wasm_canvas_screen_scale, (), {
  return self.__canvasScreenScale || 1.0;
});
// Arbitrary keyboard state in Term's key vocabulary ("a", " ", "left", "f1",
// …). worker.js keeps the held set and the pressed/typed queues current from
// the page's normalized key events; the pops hand back "" when empty.
EM_JS(int, _wasm_canvas_key, (const char* name), {
  const held = self.__canvasKeysHeld;
  return held && held.has(UTF8ToString(name)) ? 1 : 0;
});
EM_JS(char*, _wasm_canvas_key_pop, (), {
  const q = self.__canvasKeyQueue;
  const s = q && q.length ? q.shift() : "";
  const len = lengthBytesUTF8(s) + 1;
  const ptr = _malloc(len);
  stringToUTF8(s, ptr, len);
  return ptr;
});
EM_JS(char*, _wasm_canvas_char_pop, (), {
  const q = self.__canvasCharQueue;
  const s = q && q.length ? q.shift() : "";
  const len = lengthBytesUTF8(s) + 1;
  const ptr = _malloc(len);
  stringToUTF8(s, ptr, len);
  return ptr;
});
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
// Sound effects: decoded and played on the main thread like music, but many
// slots keyed by the wasm-side handle. __soundsPlaying is optimistic on play
// (the state a script reads right back) and corrected by "soundState"
// messages when a one-shot ends.
EM_JS(void, _wasm_canvas_sound_load, (int id, const uint8_t* buf, int len), {
  postMessage({ type: "sound", cmd: "load", id: id,
                buf: HEAPU8.slice(buf, buf + len) });
});
EM_JS(void, _wasm_canvas_sound_play, (int id, int vol), {
  (self.__soundsPlaying = self.__soundsPlaying || {})[id] = true;
  postMessage({ type: "sound", cmd: "play", id: id, vol: vol });
});
EM_JS(void, _wasm_canvas_sound_stop, (int id), {
  if (self.__soundsPlaying) self.__soundsPlaying[id] = false;
  postMessage({ type: "sound", cmd: "stop", id: id });
});
EM_JS(int, _wasm_canvas_sound_playing, (int id), {
  return self.__soundsPlaying && self.__soundsPlaying[id] ? 1 : 0;
});
EM_JS(void, _wasm_canvas_sound_free, (int id), {
  if (self.__soundsPlaying) delete self.__soundsPlaying[id];
  postMessage({ type: "sound", cmd: "free", id: id });
});

inline double screen_scale() { return _wasm_canvas_screen_scale(); }
inline void present() {
  auto& fb = _fb();
  ensure_screen_buffer();
  auto& sfb = _screen_fb();
  bool dirty = _screen_dirty() && !sfb.empty();
  _wasm_present(_fb_w(), _fb_h(), reinterpret_cast<const uint8_t*>(fb.data()),
                static_cast<int>(fb.size() * 4), _screen_w(), _screen_h(),
                reinterpret_cast<const uint8_t*>(sfb.data()),
                dirty ? static_cast<int>(sfb.size() * 4) : 0);
  screen_buffer_reset();
}
inline int64_t buttons() { return _wasm_canvas_buttons(); }
inline int64_t mouse_x() { return _wasm_canvas_mouse_x(); }
inline int64_t mouse_y() { return _wasm_canvas_mouse_y(); }
inline int64_t mouse_buttons() { return _wasm_canvas_mouse_buttons(); }
inline bool key(const char* name) { return _wasm_canvas_key(name) != 0; }
inline std::string key_pop() {
  char* p = _wasm_canvas_key_pop();
  std::string out(p);
  std::free(p);
  return out;
}
inline std::string char_pop() {
  char* p = _wasm_canvas_char_pop();
  std::string out(p);
  std::free(p);
  return out;
}
// The browser loop ends via tick()/frames, not a window-close event.
inline bool closing() { return false; }
// The browser canvas always shows the frames it is handed.
inline bool windowed() { return true; }
// The page hosting the canvas IS the display; there is no window to fail.
inline const char* window_error() { return nullptr; }
// Deliberate: the tab's title belongs to the page hosting the canvas, not to
// the program drawing on it.
inline void set_title(const char*) {}
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
// `fmt` is for raylib's decoder dispatch; the browser sniffs the bytes itself.
inline void sound_load(int64_t id, const uint8_t* data, int64_t len,
                       const char* /*fmt*/) {
  _wasm_canvas_sound_load(static_cast<int>(id), data, static_cast<int>(len));
}
inline void sound_play(int64_t id, int64_t vol) {
  _wasm_canvas_sound_play(static_cast<int>(id), static_cast<int>(vol));
}
inline void sound_stop(int64_t id) {
  _wasm_canvas_sound_stop(static_cast<int>(id));
}
inline bool sound_playing(int64_t id) {
  return _wasm_canvas_sound_playing(static_cast<int>(id)) != 0;
}
inline void sound_free(int64_t id) {
  _wasm_canvas_sound_free(static_cast<int>(id));
}

// Fullscreen/cursor/clipboard/resize/dt/fps/wheel/gamepad have no browser
// wiring yet: the docs/index.html-style host page already owns fullscreen,
// hjkl and gamepad for the embedded Playground itself, relaying DOM/Gamepad
// API events onto its canvas pane, so a wasm program reading these gets safe
// no-ops rather than a build that fails to link.
inline void toggle_fullscreen() {}
inline bool is_fullscreen() { return false; }
inline void show_cursor() {}
inline void hide_cursor() {}
inline bool cursor_hidden() { return false; }
inline std::string clipboard_get() { return ""; }
inline void clipboard_set(const char*) {}
inline void set_resizable(bool) {}
inline bool window_resized() { return false; }
inline double dt() { return 0.0; }
inline void set_target_fps(int64_t) {}
inline int64_t fps() { return 0; }
inline double mouse_wheel() { return 0.0; }
inline bool pad_available(int64_t) { return false; }
inline double pad_axis(int64_t, int64_t) { return 0.0; }
inline bool pad_button(int64_t, int64_t) { return false; }
inline bool pad_pressed(int64_t, int64_t) { return false; }
inline std::string pad_name(int64_t) { return ""; }
inline void pad_rumble(int64_t, double, double, double) {}
inline int64_t pad_mappings(const char*) { return 0; }
// Quitting is the tab's call, not the program's -- see closing() above.
inline void quit() {}
inline bool can_quit() { return false; }

#elif defined(CULEBRA_CANVAS_WINDOW)  // native raylib desktop window

#if defined(CULEBRA_RT_CANVAS_WEAK)

// Weak headless stubs for the base AOT runtime archive of a window build: a
// program that never uses Canvas links these and pulls in no raylib. The strong
// raylib bodies in the canvas feature archive (culebra_rt_canvas.cc,
// force-loaded only when the AST scan reports Canvas use) override them — the
// same weak choke as compress / http / sqlite.
// Clears the screen layer for the same reason the headless present() does:
// these stubs ARE the headless behaviour for a window build that linked no
// raylib, and "a presented frame starts the screen layer over" has to hold
// in every one of them or a program's output depends on which it linked.
__attribute__((weak)) void present() { screen_buffer_reset(); }
__attribute__((weak)) int64_t buttons() { return 0; }
__attribute__((weak)) int64_t mouse_x() { return 0; }
__attribute__((weak)) int64_t mouse_y() { return 0; }
__attribute__((weak)) int64_t mouse_buttons() { return 0; }
__attribute__((weak)) bool key(const char*) { return false; }
__attribute__((weak)) std::string key_pop() { return ""; }
__attribute__((weak)) std::string char_pop() { return ""; }
__attribute__((weak)) bool closing() { return false; }
__attribute__((weak)) bool windowed() { return false; }
__attribute__((weak)) double screen_scale() { return 1.0; }
__attribute__((weak)) const char* window_error() { return nullptr; }
__attribute__((weak)) void set_title(const char*) {}
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
__attribute__((weak)) void sound_load(int64_t, const uint8_t*, int64_t,
                                      const char*) {}
__attribute__((weak)) void sound_play(int64_t, int64_t) {}
__attribute__((weak)) void sound_stop(int64_t) {}
__attribute__((weak)) bool sound_playing(int64_t) { return false; }
__attribute__((weak)) void sound_free(int64_t) {}
__attribute__((weak)) void toggle_fullscreen() {}
__attribute__((weak)) bool is_fullscreen() { return false; }
__attribute__((weak)) void show_cursor() {}
__attribute__((weak)) void hide_cursor() {}
__attribute__((weak)) bool cursor_hidden() { return false; }
__attribute__((weak)) std::string clipboard_get() { return ""; }
__attribute__((weak)) void clipboard_set(const char*) {}
__attribute__((weak)) void set_resizable(bool) {}
__attribute__((weak)) bool window_resized() { return false; }
__attribute__((weak)) double dt() { return 0.0; }
__attribute__((weak)) void set_target_fps(int64_t) {}
__attribute__((weak)) int64_t fps() { return 0; }
__attribute__((weak)) double mouse_wheel() { return 0.0; }
__attribute__((weak)) bool pad_available(int64_t) { return false; }
__attribute__((weak)) double pad_axis(int64_t, int64_t) { return 0.0; }
__attribute__((weak)) bool pad_button(int64_t, int64_t) { return false; }
__attribute__((weak)) bool pad_pressed(int64_t, int64_t) { return false; }
__attribute__((weak)) std::string pad_name(int64_t) { return ""; }
__attribute__((weak)) void pad_rumble(int64_t, double, double, double) {}
__attribute__((weak)) int64_t pad_mappings(const char*) { return 0; }
__attribute__((weak)) void quit() {}
__attribute__((weak)) bool can_quit() { return false; }

#else

// Strong raylib bodies live in src/runtime/culebra_rt_canvas.cc (the driver's
// in-process backend + the force-loaded feature archive). Declarations only
// here so the framebuffer core header stays raylib-free everywhere else.
void present();
int64_t buttons();
int64_t mouse_x();
int64_t mouse_y();
int64_t mouse_buttons();
bool key(const char* name);
std::string key_pop();
std::string char_pop();
bool closing();
bool windowed();
// screen_scale() is declared once for every backend, above.
// Non-null after the window could not be opened with no headless mode
// declared; the message for the error present() raises. Null while the window
// is up, before anything asked for one, and in every declared-headless mode.
const char* window_error();
void set_title(const char* title);
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
void sound_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt);
void sound_play(int64_t id, int64_t vol);
void sound_stop(int64_t id);
bool sound_playing(int64_t id);
void sound_free(int64_t id);
void toggle_fullscreen();
bool is_fullscreen();
void show_cursor();
void hide_cursor();
bool cursor_hidden();
std::string clipboard_get();
void clipboard_set(const char* text);
void set_resizable(bool enabled);
bool window_resized();
double dt();
void set_target_fps(int64_t fps);
int64_t fps();
double mouse_wheel();
bool pad_available(int64_t index);
double pad_axis(int64_t index, int64_t axis);
bool pad_button(int64_t index, int64_t button);
bool pad_pressed(int64_t index, int64_t button);
std::string pad_name(int64_t index);
void pad_rumble(int64_t index, double left, double right, double sec);
int64_t pad_mappings(const char* db);
void quit();
bool can_quit();

#endif  // CULEBRA_RT_CANVAS_WEAK

#else  // native headless (default): frame held, nothing shown, no input

// Headless: the framebuffer is held, nothing shown. The screen layer is still
// cleared, so "a frame was presented, redraw the screen layer" is one contract
// across every backend rather than something only the displaying ones honour.
inline void present() { screen_buffer_reset(); }
inline int64_t buttons() { return 0; }
inline int64_t mouse_x() { return 0; }
inline int64_t mouse_y() { return 0; }
inline int64_t mouse_buttons() { return 0; }
inline bool key(const char*) { return false; }
inline std::string key_pop() { return ""; }
inline std::string char_pop() { return ""; }
inline bool closing() { return false; }
inline bool windowed() { return false; }  // no window to show frames in
inline double screen_scale() { return 1.0; }  // ...and none to stretch into
inline const char* window_error() { return nullptr; }  // ...by declaration
inline void set_title(const char*) {}  // ...and none to title
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
inline void sound_load(int64_t, const uint8_t*, int64_t, const char*) {}
inline void sound_play(int64_t, int64_t) {}
inline void sound_stop(int64_t) {}
inline bool sound_playing(int64_t) { return false; }
inline void sound_free(int64_t) {}
inline void toggle_fullscreen() {}
inline bool is_fullscreen() { return false; }
inline void show_cursor() {}
inline void hide_cursor() {}
inline bool cursor_hidden() { return false; }
inline std::string clipboard_get() { return ""; }
inline void clipboard_set(const char*) {}
inline void set_resizable(bool) {}
inline bool window_resized() { return false; }
inline double dt() { return 0.0; }
inline void set_target_fps(int64_t) {}
inline int64_t fps() { return 0; }
inline double mouse_wheel() { return 0.0; }
inline bool pad_available(int64_t) { return false; }
inline double pad_axis(int64_t, int64_t) { return 0.0; }
inline bool pad_button(int64_t, int64_t) { return false; }
inline bool pad_pressed(int64_t, int64_t) { return false; }
inline std::string pad_name(int64_t) { return ""; }
inline void pad_rumble(int64_t, double, double, double) {}
inline int64_t pad_mappings(const char*) { return 0; }
inline void quit() {}
inline bool can_quit() { return false; }

#endif  // __EMSCRIPTEN__

}  // namespace _canvas_detail
}  // namespace culebra
