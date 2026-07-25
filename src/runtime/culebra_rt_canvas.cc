// Native desktop window backend for the `Canvas` 2D framebuffer namespace,
// built only with -DCULEBRA_ENABLE_CANVAS_WINDOW=ON. The value-neutral
// framebuffer/sprite core lives in include/canvas.h and stays raylib-free; this
// file provides just the backend-specific pieces its native branch declares:
// present (upload the frame to a texture, scale it up, block to vsync), polled
// keyboard/mouse input, and the window's close state. It mirrors the Scene
// facade's script-owned-loop model (culebra_rt_scene.cc) — the culebra `run`
// loop drives frames and `present` blocks to the display — so interp/JIT stay
// symmetric with no semantic change from the headless build.
//
// raylib + SDL3 are the same vendored statics Scene links; CMake shares one
// build between the two knobs.

#include "canvas.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "raylib.h"

namespace culebra {
namespace _canvas_detail {
namespace {

// The window is created lazily on the first backend call (present or input),
// once init() has sized the framebuffer. State is process-lifetime, torn down
// at exit; a little game opens exactly one window.
Texture2D g_tex;
int g_tex_w = 0;
int g_tex_h = 0;
int g_scale = 1;
bool g_window_ready = false;

// Integer upscale so the small framebuffer fills a comfortable window while
// pixels stay crisp (nearest-neighbour), matching the Playground's
// image-rendering:pixelated presentation.
int pick_scale(int w, int h) {
  int longest = std::max(w, h);
  if (longest <= 0) return 1;
  return std::max(1, 640 / longest);
}

// Forced-headless mode for the windowed build: with CULEBRA_CANVAS_HEADLESS set
// to anything but ""/"0"/"off", no window is ever opened and present/input
// become no-ops, exactly like the default headless backend. This keeps the
// PPM-md5 diff tests (and displayless CI / SSH runs) working against a
// window-enabled binary — the "headless path and window mode coexist"
// requirement — without depending on the OS failing window creation cleanly
// (some backends crash instead). Reading the value rather than just its presence
// (the CULEBRA_JIT_CACHE convention) lets the justfile export it for every
// recipe while a single run opts back into the window with =0.
bool forced_headless() {
  static const bool h = [] {
    const char* v = std::getenv("CULEBRA_CANVAS_HEADLESS");
    if (!v) return false;
    std::string_view s(v);
    return !(s.empty() || s == "0" || s == "off");
  }();
  return h;
}

// (Re)create the window + texture to match the current framebuffer size. Called
// on first use and again if the framebuffer is re-init()'d at a new size.
void ensure_window() {
  if (forced_headless()) return;
  int w = width();
  int h = height();
  if (w <= 0 || h <= 0) return;  // nothing sized yet
  if (g_window_ready && w == g_tex_w && h == g_tex_h) return;

  g_scale = pick_scale(w, h);
  if (!g_window_ready) {
    SetTraceLogLevel(LOG_WARNING);  // quiet raylib's INFO chatter
    InitWindow(w * g_scale, h * g_scale, "culebra Canvas");
    // No display (headless server / CI / SSH): raylib fails to open a window
    // and IsWindowReady() stays false. Degrade to the headless backend rather
    // than driving GL on a dead context — present/input become no-ops.
    if (!IsWindowReady()) return;
    SetTargetFPS(60);  // pin the game clock to 60 fps like the browser loop
    g_window_ready = true;
  } else {
    UnloadTexture(g_tex);
    SetWindowSize(w * g_scale, h * g_scale);
  }
  Image img = GenImageColor(w, h, BLANK);
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  g_tex = LoadTextureFromImage(img);
  UnloadImage(img);
  SetTextureFilter(g_tex, TEXTURE_FILTER_POINT);  // crisp upscale
  g_tex_w = w;
  g_tex_h = h;
}

// Close the window at process exit so the run terminates cleanly rather than
// leaving the OS to reclaim it.
struct WindowCloser {
  ~WindowCloser() {
    if (g_window_ready && IsWindowReady()) {
      UnloadTexture(g_tex);
      CloseWindow();
    }
  }
};
WindowCloser g_closer;

// One held-key -> Canvas button bit (bits match src/preambles/canvas.cul and
// the Playground's KEY_BITS: LEFT=1, RIGHT=2, UP=4, DOWN=8, A=16, B=32).
int64_t held_button(int key, int64_t bit) { return IsKeyDown(key) ? bit : 0; }

}  // namespace

void present() {
  ensure_window();
  if (!g_window_ready) return;
  const std::vector<uint32_t>& fb = _fb();
  if (!fb.empty()) UpdateTexture(g_tex, fb.data());
  BeginDrawing();
  ClearBackground(BLACK);
  DrawTexturePro(g_tex, Rectangle{0, 0, (float)g_tex_w, (float)g_tex_h},
                 Rectangle{0, 0, (float)(g_tex_w * g_scale),
                           (float)(g_tex_h * g_scale)},
                 Vector2{0, 0}, 0.0f, WHITE);
  EndDrawing();  // blocks to vsync / the 60 fps target
}

int64_t buttons() {
  ensure_window();
  if (!g_window_ready) return 0;
  int64_t m = 0;
  // WASD doubles the d-pad, so a game that wants a hand on each side of the
  // keyboard (steering left, action right) works without remapping.
  m |= held_button(KEY_LEFT, 1) | held_button(KEY_A, 1);
  m |= held_button(KEY_RIGHT, 2) | held_button(KEY_D, 2);
  m |= held_button(KEY_UP, 4) | held_button(KEY_W, 4);
  m |= held_button(KEY_DOWN, 8) | held_button(KEY_S, 8);
  m |= (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_Z)) ? 16 : 0;   // A
  m |= IsKeyDown(KEY_X) ? 32 : 0;                             // B
  return m;
}

// Mouse position in framebuffer pixels (window pixels / the upscale), clamped
// to the buffer.
int64_t mouse_x() {
  ensure_window();
  if (!g_window_ready) return 0;
  int x = GetMouseX() / g_scale;
  return std::clamp(x, 0, g_tex_w > 0 ? g_tex_w - 1 : 0);
}
int64_t mouse_y() {
  ensure_window();
  if (!g_window_ready) return 0;
  int y = GetMouseY() / g_scale;
  return std::clamp(y, 0, g_tex_h > 0 ? g_tex_h - 1 : 0);
}

// Held mouse buttons, DOM MouseEvent.buttons convention (1=left, 2=right,
// 4=middle) so it matches the browser frontend and the games that poll it.
int64_t mouse_buttons() {
  ensure_window();
  if (!g_window_ready) return 0;
  int64_t m = 0;
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) m |= 1;
  if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) m |= 2;
  if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) m |= 4;
  return m;
}

// True once the window's close box (or Esc) has been used, so the run loop
// stops. Before the window exists there is nothing to close.
bool closing() {
  ensure_window();
  if (!g_window_ready) return false;
  return WindowShouldClose();
}

// Native audio for Canvas tones is a later milestone; silent for now (the
// framebuffer game still runs, just without sound).
void tone(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
          int64_t, int64_t, int64_t) {}

}  // namespace _canvas_detail
}  // namespace culebra
