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
// The native backend is headless in this milestone: the pixel buffer and
// sprite ops run identically to the browser (so interp/JIT symmetry is
// verifiable off-screen via get_pixel), but `present` shows nothing and input
// reads as "no button". A real SDL2 window is a later milestone.

#include <algorithm>
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
EM_JS(void, _wasm_canvas_tone, (int freq, int dur, int vol, int wave), {
  postMessage({ type: "tone", freq: freq, dur: dur, vol: vol, wave: wave });
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
inline void tone(int64_t freq, int64_t dur, int64_t vol, int64_t wave) {
  _wasm_canvas_tone(static_cast<int>(freq), static_cast<int>(dur),
                    static_cast<int>(vol), static_cast<int>(wave));
}

#else  // native (headless in this milestone)

inline void present() {}  // headless: the framebuffer is held, nothing shown
inline int64_t buttons() { return 0; }
inline int64_t mouse_x() { return 0; }
inline int64_t mouse_y() { return 0; }
inline int64_t mouse_buttons() { return 0; }
inline void tone(int64_t, int64_t, int64_t, int64_t) {}

#endif  // __EMSCRIPTEN__

}  // namespace _canvas_detail
}  // namespace culebra
