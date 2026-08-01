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
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>

#include "raylib.h"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

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
bool g_window_failed = false;  // creation tried and failed; don't try again
// Why the window could not open — the message present() raises. Headless is
// declared (CULEBRA_CANVAS_HEADLESS=1), never inferred from a failed window.
std::string g_window_error;
std::string g_title = "culebra Canvas";  // applied when the window opens

// Integer upscale so the small framebuffer fills a comfortable window while
// pixels stay crisp (nearest-neighbour), matching the Playground's
// image-rendering:pixelated presentation.
int pick_scale(int w, int h) {
  int longest = std::max(w, h);
  if (longest <= 0) return 1;
  return std::max(1, 640 / longest);
}

// raylib's SDL backend turns text input on for every window it opens, which
// makes the window an active text-input client. Canvas polls key state and
// never consumes text, and being a text client is what routes a held key
// through the platform's compose/IME machinery: macOS pops up its
// press-and-hold accent picker, and a CJK input method would open its
// candidate window over the game. Turn it back off — raylib exposes no API
// for this, so it goes through SDL directly (raylib is built on the same
// vendored static SDL3 this links). The process has exactly one window, and
// asking SDL for its window list keeps this independent of focus, which the
// window may not have yet at creation.
void stop_text_input() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) return;
  for (int i = 0; i < count; i++) SDL_StopTextInput(windows[i]);
  SDL_free(windows);
}

// char_pop() (Canvas.typed) needs those same text-input events, so the first
// call turns text input back on — a program that reads typed characters opts
// into the platform's text machinery, and one that only polls keys never sees
// an IME popup. Latched: the flip happens once.
bool g_text_input_on = false;
void start_text_input() {
  if (g_text_input_on) return;
  g_text_input_on = true;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) return;
  for (int i = 0; i < count; i++) SDL_StartTextInput(windows[i]);
  SDL_free(windows);
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

// Registers the exit teardown; defined with the audio state it tears down, at
// the end of the audio section.
void arm_exit_teardown();

// raylib reports a graphics device it could not create with LOG_FATAL, and its
// default log handler exits the process — so on exactly the machines the
// headless fallback below exists for, the run died instead of degrading. A
// trace-log callback is raylib's own way out: TraceLog hands the message over
// and returns, which lets InitPlatform's `return -1` reach InitWindow, which
// reports the failure through IsWindowReady() like any other.
void trace_log(int level, const char* text, va_list args) {
  if (level < LOG_WARNING) return;  // raylib's INFO chatter stays quiet
  std::fprintf(stderr, "%s: ", level >= LOG_FATAL     ? "FATAL"
                               : level >= LOG_ERROR   ? "ERROR"
                                                      : "WARNING");
  std::vfprintf(stderr, text, args);
  std::fputc('\n', stderr);
}

// A machine can have a video driver and still not have the GL raylib needs: a
// Windows session with only the generic Microsoft renderer hands out an OpenGL
// 1.1 context, where the shader entry points rlglInit calls are null — and it
// calls them, so the process dies inside raylib with nothing we can check
// afterwards (a CI runner did exactly this: rlLoadShader -> 0x0). raylib does
// the window and the GL context in one call, so the only place to ask is
// before it: take a hidden context of our own, see whether the entry point
// rlglInit will reach for is there, and hand it back.
bool gl_usable() {
  if (!SDL_Init(SDL_INIT_VIDEO)) return false;
  SDL_Window* w = SDL_CreateWindow("culebra Canvas probe", 1, 1,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (w == nullptr) return false;
  bool ok = false;
  if (SDL_GLContext ctx = SDL_GL_CreateContext(w)) {
    ok = SDL_GL_GetProcAddress("glCreateShader") != nullptr;
    SDL_GL_DestroyContext(ctx);
  }
  SDL_DestroyWindow(w);
  return ok;
}

// (Re)create the window + texture to match the current framebuffer size. Called
// on first use and again if the framebuffer is re-init()'d at a new size.
void ensure_window() {
  if (forced_headless() || g_window_failed) return;
  // The window mirrors the FRAMEBUFFER — width()/height() report the current
  // draw target, which inside Canvas.draw_to is an offscreen sprite.
  int w = _fb_w();
  int h = _fb_h();
  if (w <= 0 || h <= 0) return;  // nothing sized yet
  if (g_window_ready && w == g_tex_w && h == g_tex_h) return;

  g_scale = pick_scale(w, h);
  if (!g_window_ready) {
    SetTraceLogLevel(LOG_WARNING);  // quiet raylib's INFO chatter
    SetTraceLogCallback(trace_log);  // and keep a fatal one from exiting
    // Both failure arms latch (every present and every input poll comes back
    // through here — a retry per call would re-run SDL's whole video-driver
    // probe thousands of times over a 600-frame run) and record the message
    // present() raises.
    if (!gl_usable()) {
      g_window_failed = true;
      g_window_error =
          "cannot open a Canvas window: no usable OpenGL; set "
          "CULEBRA_CANVAS_HEADLESS=1 to run without one";
      return;
    }
    InitWindow(w * g_scale, h * g_scale, g_title.c_str());
    // No display (headless server / SSH): raylib fails to open a window and
    // IsWindowReady() stays false.
    if (!IsWindowReady()) {
      g_window_failed = true;
      g_window_error =
          "cannot open a Canvas window: no display; set "
          "CULEBRA_CANVAS_HEADLESS=1 to run without one";
      return;
    }
    stop_text_input();
    SetTargetFPS(60);  // pin the game clock to 60 fps like the browser loop
    g_window_ready = true;
    arm_exit_teardown();
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

// One held-key -> Canvas button bit (bits match src/preambles/canvas.cul and
// the Playground's KEY_BITS: LEFT=1, RIGHT=2, UP=4, DOWN=8, A=16, B=32).
int64_t held_button(int key, int64_t bit) { return IsKeyDown(key) ? bit : 0; }

// --- arbitrary keys: Term's key vocabulary over raylib key codes ------------
//
// A name is a printable character ("a", " ", "-") or a special-key name
// ("left", "enter", "f1", …) — the same vocabulary Term.read_key reports, so
// key handling code moves between the two namespaces unchanged. raylib's
// printable key codes are upper-case ASCII, so single characters map by
// arithmetic and only the specials need a table.
const std::unordered_map<std::string_view, int>& special_keys() {
  static const std::unordered_map<std::string_view, int> m = {
      {"up", KEY_UP},           {"down", KEY_DOWN},
      {"left", KEY_LEFT},       {"right", KEY_RIGHT},
      {"enter", KEY_ENTER},     {"escape", KEY_ESCAPE},
      {"tab", KEY_TAB},         {"backspace", KEY_BACKSPACE},
      {"insert", KEY_INSERT},   {"delete", KEY_DELETE},
      {"home", KEY_HOME},       {"end", KEY_END},
      {"pageup", KEY_PAGE_UP},  {"pagedown", KEY_PAGE_DOWN},
      {"f1", KEY_F1},  {"f2", KEY_F2},   {"f3", KEY_F3},  {"f4", KEY_F4},
      {"f5", KEY_F5},  {"f6", KEY_F6},   {"f7", KEY_F7},  {"f8", KEY_F8},
      {"f9", KEY_F9},  {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
  };
  return m;
}

int key_code_of(std::string_view name) {
  if (name.size() == 1) {
    unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= ' ' && c <= '~') return c;
    return 0;
  }
  auto it = special_keys().find(name);
  return it == special_keys().end() ? 0 : it->second;
}

// The queue direction: a pressed key code back to its name. Unmapped codes
// (modifiers, keypad) produce "" and are dropped from the queue, the same
// keys Term's parser has no name for.
std::string key_name_of(int code) {
  if (code >= 'A' && code <= 'Z') return std::string(1, static_cast<char>(code + 32));
  if (code >= ' ' && code <= '~') return std::string(1, static_cast<char>(code));
  for (const auto& [name, c] : special_keys())
    if (c == code) return std::string(name);
  return "";
}

// A unicode code point as UTF-8, for the typed-characters queue.
std::string utf8_of(int cp) {
  std::string out;
  if (cp < 0) return out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
  return out;
}

// Pressed-key and typed-character queues, drained by the script through
// key_pop/char_pop. Capped so a program that never reads them stays bounded;
// the oldest events fall off first (matching worker.js's cap).
constexpr size_t kKeyQueueCap = 256;
std::deque<std::string> g_key_queue;
std::deque<std::string> g_char_queue;

// Pull raylib's per-frame pressed/typed events into the queues. Called from
// present(), right after EndDrawing has pumped the platform events.
void drain_key_events() {
  for (int c = GetKeyPressed(); c != 0; c = GetKeyPressed()) {
    std::string n = key_name_of(c);
    if (n.empty()) continue;
    if (g_key_queue.size() >= kKeyQueueCap) g_key_queue.pop_front();
    g_key_queue.push_back(std::move(n));
  }
  for (int c = GetCharPressed(); c != 0; c = GetCharPressed()) {
    if (g_char_queue.size() >= kKeyQueueCap) g_char_queue.pop_front();
    g_char_queue.push_back(utf8_of(c));
  }
}

// --- audio: a small WASM-4-style APU, mixed in software ---------------------
//
// The browser side (playground/app.js's playTone) hands each note to
// WebAudio, which owns its own envelope/oscillator machinery; native has none
// of that; the callback below IS the synth. Five channels (two pulse, a
// triangle, noise, and the culebra-only sawtooth), one note at a time each —
// a new tone() call on a channel cuts whatever was still playing there, like
// the real APU. Times arrive in frames at ~60fps; this converts them to
// sample counts once, at note-start, using the stream's own sample rate.
constexpr int kSampleRate = 44100;
// Channel numbers match src/preambles/canvas.cul's PULSE/PULSE2/TRIANGLE/
// NOISE/SAWTOOTH constants. 0 and 1 (the two pulse channels) fall to
// oscillate()'s default case — only their duty cycle differs, not their shape.
constexpr int kTriangle = 2, kNoise = 3, kSawtooth = 4;

struct Note {
  bool active = false;
  uint64_t id = 0;         // bumped on every tone() so a stale write can't
                            // clobber a note that started mid-buffer (below)
  double start_freq = 0, end_freq = 0;
  int64_t attack = 0, decay = 0, sustain = 0, release = 0;  // samples
  int64_t total = 0;                                        // samples
  double vol = 0, peak = 0;   // 0..1 gain, already scaled and headroom-capped
  double duty = 0.5;          // pulse only
  int64_t elapsed = 0;        // samples into the note; audio thread owns this
  double phase = 0;           // 0..1, oscillator channels
  double lp_state = 0;        // noise channel's one-pole lowpass
};

std::mutex g_audio_mutex;
Note g_notes[5];

AudioStream g_stream;
bool g_audio_ready = false;
bool g_audio_failed = false;  // latch: don't retry InitAudioDevice every call

// vol/peak arrive as 0..100. File-backed audio (music, Sound) is already
// mixed, so 100 is the file's own level.
double gain_of(int64_t v) { return std::clamp(v, int64_t{0}, int64_t{100}) / 100.0; }

// A synthesized waveform is raw and up to four channels stack, so tone keeps a
// headroom the file paths don't need — the browser's "keep it gentle" 0.2.
double tone_gain_of(int64_t v) { return gain_of(v) * 0.2; }

// The ADSR envelope's gain at `elapsed` samples into a note of the given
// phase lengths (all in samples). Attack ramps 0->peak, decay ramps
// peak->sustain, sustain holds, release ramps sustain->0.
double envelope(int64_t elapsed, int64_t attack, int64_t decay, int64_t sustain,
                int64_t release, double peak, double sustain_gain) {
  if (elapsed < attack) {
    return attack > 0 ? peak * (static_cast<double>(elapsed) / attack) : peak;
  }
  elapsed -= attack;
  if (elapsed < decay) {
    double t = decay > 0 ? static_cast<double>(elapsed) / decay : 1.0;
    return peak + (sustain_gain - peak) * t;
  }
  elapsed -= decay;
  if (elapsed < sustain) return sustain_gain;
  elapsed -= sustain;
  if (elapsed < release) {
    double t = release > 0 ? static_cast<double>(elapsed) / release : 1.0;
    return sustain_gain * (1.0 - t);
  }
  return 0.0;
}

// One sample of a channel's raw waveform at its current phase (-1..1),
// advancing the phase by one sample's worth of its (linearly swept)
// frequency. Naive (non-band-limited) waves — fine at chiptune-bleep
// durations and volumes, and it keeps the mixer simple.
double oscillate(Note& n, int channel, double freq) {
  double out;
  switch (channel) {
    case kTriangle:
      out = 4.0 * std::abs(n.phase - std::floor(n.phase + 0.5)) - 1.0;
      break;
    case kSawtooth:
      out = 2.0 * n.phase - 1.0;
      break;
    default:  // pulse / pulse2
      out = n.phase < n.duty ? 1.0 : -1.0;
      break;
  }
  n.phase += freq / kSampleRate;
  if (n.phase >= 1.0) n.phase -= std::floor(n.phase);
  return out;
}

// Noise: a cheap xorshift PRNG through a one-pole lowpass that sweeps
// start->end (times 8, matching the browser's filter sweep), so it reads as
// a pitched hiss rather than flat static.
double noise_sample(Note& n, double cutoff) {
  static thread_local uint32_t rng = 0x9e3779b9u;
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  double white = (static_cast<double>(rng) / static_cast<double>(0xffffffffu)) * 2.0 - 1.0;
  double nyq = kSampleRate / 2.0;
  double alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * std::min(cutoff, nyq * 0.99) / kSampleRate);
  n.lp_state += alpha * (white - n.lp_state);
  return n.lp_state;
}

// raylib's audio thread callback: fill `frames` mono float samples. Runs
// off the main thread, so every shared Note is read through the mutex — the
// snapshot/writeback split (rather than holding the lock the whole buffer)
// keeps a slow buffer from delaying tone() on the main thread.
void audio_callback(void* buffer_data, unsigned int frames) {
  float* out = static_cast<float*>(buffer_data);
  Note local[5];
  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    for (int c = 0; c < 5; c++) local[c] = g_notes[c];
  }

  for (unsigned int i = 0; i < frames; i++) {
    double mixed = 0.0;
    for (int c = 0; c < 5; c++) {
      Note& n = local[c];
      if (!n.active || n.elapsed >= n.total) {
        n.active = false;
        continue;
      }
      double t = n.total > 0 ? static_cast<double>(n.elapsed) / n.total : 1.0;
      double freq = n.start_freq + (n.end_freq - n.start_freq) * t;
      double g = envelope(n.elapsed, n.attack, n.decay, n.sustain, n.release, n.peak, n.vol);
      double raw = c == kNoise ? noise_sample(n, std::max(1.0, freq * 8.0))
                                : oscillate(n, c, std::max(1.0, freq));
      mixed += raw * g;
      n.elapsed++;
    }
    out[i] = static_cast<float>(std::clamp(mixed, -1.0, 1.0));
  }

  std::lock_guard<std::mutex> lock(g_audio_mutex);
  for (int c = 0; c < 5; c++) {
    // Only write back a channel whose note is still the one we started the
    // buffer with — tone() may have started a fresher note on the main
    // thread mid-buffer, and that note's own elapsed=0 must not be clobbered
    // by this buffer's (now-stale) count.
    if (local[c].id == g_notes[c].id) g_notes[c] = local[c];
  }
}

void ensure_audio() {
  if (forced_headless() || g_audio_failed) return;
  if (g_audio_ready) return;
  SetTraceLogLevel(LOG_WARNING);  // audio may come up before any window does
  InitAudioDevice();
  if (!IsAudioDeviceReady()) {
    g_audio_failed = true;  // no device (headless server/CI)
    // Sound is decorative, so no error — but say it once. A silent latch
    // reads as "my tone() calls are broken" on a machine without a device.
    TraceLog(LOG_WARNING, "Canvas: no audio device -- sound stays off");
    return;
  }
  g_stream = LoadAudioStream(kSampleRate, 32, 1);  // 32-bit float, mono
  SetAudioStreamCallback(g_stream, audio_callback);
  PlayAudioStream(g_stream);
  g_audio_ready = true;
  arm_exit_teardown();
}

// --- music: one streamed-file slot ------------------------------------------
//
// A single Music at a time, pygame-mixer style: music_play replaces whatever
// was playing, music_stop unloads it, process exit reclaims the slot. No
// handle ever reaches the script, so there is no lifetime to manage there.
// raylib's memory decoders (drmp3 / stb_vorbis) keep POINTERS into the byte
// buffer they were opened on rather than copying it, so the slot owns the
// bytes and the Music together and releases them together — separating them
// is a use-after-free that only surfaces once playback reads the freed pages.
// Everything here runs on the main thread (decoding happens in the
// UpdateMusicStream pump, not the audio callback), so no lock of ours needed.
struct MusicSlot {
  Music music{};
  std::vector<uint8_t> bytes;
  bool loaded = false;

  void unload() {
    if (!loaded) return;
    UnloadMusicStream(music);
    music = Music{};
    bytes.clear();
    bytes.shrink_to_fit();
    loaded = false;
  }
  ~MusicSlot() { unload(); }
};
MusicSlot g_music;

// Loaded sound effects, keyed by the canvas.h-allocated handle. One live
// instance per handle (raylib PlaySound restarts the sound), matching the
// browser side.
struct SoundRegistry {
  std::unordered_map<int64_t, Sound> sounds;

  void unload_all() {
    for (auto& [id, s] : sounds) UnloadSound(s);
    sounds.clear();
  }
  ~SoundRegistry() { unload_all(); }
};
SoundRegistry g_sounds;

// Hand the window and the audio device back at process exit, in the order
// raylib wants: every sound and the music stream go back before the device they
// were decoded for, and the device before the window. Each step clears what
// guards it, so the owning statics above find nothing left to do when their own
// destructors run afterwards.
void exit_teardown() {
  g_sounds.unload_all();
  g_music.unload();
  if (g_audio_ready) {
    UnloadAudioStream(g_stream);
    CloseAudioDevice();
    g_audio_ready = false;
  }
  if (g_window_ready && IsWindowReady()) {
    UnloadTexture(g_tex);
    CloseWindow();
    g_window_ready = false;
  }
}

// Armed once the window or the audio device exists, rather than run from a
// file-scope object's destructor. A static object registers its destructor with
// __cxa_atexit when it is CONSTRUCTED — before main for a file-scope one — and
// exit runs those registrations in reverse, so a file-scope closer runs last:
// after the GL and audio drivers dlopen'd along the way have torn themselves
// down. Calling into Mesa there segfaults, which is what every Canvas program on
// Linux did on the way out. Registering once the resource exists nests our
// teardown inside the lifetime of the libraries it calls into.
//
// Re-registered per resource rather than latched to the first one: only a
// registration made after a driver was dlopen'd runs before that driver tears
// itself down. A tone() on the first frame brings audio up before the window,
// which latched the registration ahead of Mesa and put CloseWindow() back after
// Mesa was gone. exit_teardown clears what guards each step, so the second run
// finds nothing to do and the duplicate registration costs only its slot.
void arm_exit_teardown() { std::atexit(exit_teardown); }

// Refill the stream's buffers — called from present(), the one place every
// frame loop passes through, so no pump API needs exposing.
void music_pump() {
  if (g_music.loaded) UpdateMusicStream(g_music.music);
}

}  // namespace

void present() {
  // Pump the music stream before anything window-related: a display-less (but
  // audible) run degrades to no window, and must still keep playing.
  music_pump();
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
  drain_key_events();
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

// Held state of one named key; unknown names are simply not held.
bool key(const char* name) {
  ensure_window();
  if (!g_window_ready || name == nullptr) return false;
  int code = key_code_of(name);
  return code != 0 && IsKeyDown(code);
}

std::string key_pop() {
  if (g_key_queue.empty()) return "";
  std::string s = std::move(g_key_queue.front());
  g_key_queue.pop_front();
  return s;
}

std::string char_pop() {
  ensure_window();
  if (g_window_ready) start_text_input();  // opt into text events on first use
  if (g_char_queue.empty()) return "";
  std::string s = std::move(g_char_queue.front());
  g_char_queue.pop_front();
  return s;
}

// True once the window's close box (or Esc) has been used, so the run loop
// stops. Before the window exists there is nothing to close.
bool closing() {
  ensure_window();
  if (!g_window_ready) return false;
  return WindowShouldClose();
}

// True when frames actually reach a display. False under CULEBRA_CANVAS_HEADLESS
// and when window creation failed (no display), where this build behaves exactly
// like the headless one: nothing shown, no input, no close box.
bool windowed() {
  ensure_window();
  return g_window_ready;
}

const char* window_error() {
  return g_window_error.empty() ? nullptr : g_window_error.c_str();
}

// Deliberately does not ensure_window(): naming a window must not open one.
// Before there is one the name is only remembered; ensure_window() applies it.
void set_title(const char* title) {
  if (g_title == title) return;  // a per-frame rename that isn't one
  g_title = title;
  if (g_window_ready) SetWindowTitle(g_title.c_str());
}

// Frames (at ~60fps) to samples (at kSampleRate), matching the browser's own
// `frames / 60` -> seconds conversion (playground/app.js's F()).
int64_t frames_to_samples(int64_t frames) {
  return static_cast<int64_t>(std::max<int64_t>(0, frames) *
                              (static_cast<double>(kSampleRate) / 60.0));
}

void tone(int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay,
          int64_t sustain, int64_t release, int64_t vol, int64_t peak,
          int64_t channel, int64_t duty) {
  ensure_audio();
  if (!g_audio_ready) return;
  if (channel < 0 || channel > 4) return;

  Note n;
  n.active = true;
  n.start_freq = std::max<int64_t>(1, start_freq);
  n.end_freq = std::max<int64_t>(1, end_freq);
  n.attack = frames_to_samples(attack);
  n.decay = frames_to_samples(decay);
  n.sustain = frames_to_samples(sustain);
  n.release = frames_to_samples(release);
  n.total = n.attack + n.decay + n.sustain + n.release;
  if (n.total <= 0) n.total = 1;  // guarantee an audible blip, like the browser
  n.vol = tone_gain_of(vol);
  n.peak = tone_gain_of(peak);
  static constexpr double kDutyCycles[4] = {0.125, 0.25, 0.5, 0.75};
  n.duty = kDutyCycles[std::clamp<int64_t>(duty, 0, 3)];
  n.elapsed = 0;
  n.phase = 0.0;
  n.lp_state = 0.0;

  std::lock_guard<std::mutex> lock(g_audio_mutex);
  n.id = g_notes[channel].id + 1;
  g_notes[channel] = n;
}

// The format sniff (and its ValueError) has already run in the caller's
// backend-neutral shim; `fmt` is its verdict. A stream the sniff accepted but
// the decoder rejects stays silent, like the browser's failed decodeAudioData.
void music_play(const uint8_t* data, int64_t len, const char* fmt,
                int64_t looping, int64_t vol, double start) {
  ensure_audio();
  if (!g_audio_ready) return;
  g_music.unload();
  g_music.bytes.assign(data, data + len);
  Music m = LoadMusicStreamFromMemory(fmt, g_music.bytes.data(),
                                      static_cast<int>(g_music.bytes.size()));
  if (!IsMusicValid(m)) {
    // A null ctx means raylib already freed its partial state; a non-null one
    // (a decodable but empty stream) still owns a decoder to unload.
    if (m.ctxData != nullptr) UnloadMusicStream(m);
    g_music.bytes.clear();
    g_music.bytes.shrink_to_fit();
    return;
  }
  m.looping = looping != 0;
  SetMusicVolume(m, static_cast<float>(gain_of(vol)));
  PlayMusicStream(m);
  if (start > 0) SeekMusicStream(m, static_cast<float>(start));
  g_music.music = m;
  g_music.loaded = true;
}

void music_stop() { g_music.unload(); }

void music_pause() {
  if (g_music.loaded) PauseMusicStream(g_music.music);
}

void music_resume() {
  if (g_music.loaded) ResumeMusicStream(g_music.music);
}

void music_volume(int64_t vol) {
  if (g_music.loaded)
    SetMusicVolume(g_music.music, static_cast<float>(gain_of(vol)));
}

void music_seek(double seconds) {
  if (!g_music.loaded) return;
  if (!(seconds > 0)) seconds = 0;  // NaN and negatives land at the start
  SeekMusicStream(g_music.music, static_cast<float>(seconds));
}

bool music_playing() {
  return g_music.loaded && IsMusicStreamPlaying(g_music.music);
}

// --- sound effects: decoded once, played per call ---------------------------

void sound_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt) {
  ensure_audio();
  if (!g_audio_ready) return;
  // Past the sniff but undecodable: the handle stays silent, the music-slot
  // convention for a stream that fails to decode.
  Wave w = LoadWaveFromMemory(fmt, data, static_cast<int>(len));
  if (w.data == nullptr) return;
  Sound s = LoadSoundFromWave(w);
  UnloadWave(w);
  g_sounds.sounds[id] = s;
}

void sound_play(int64_t id, int64_t vol) {
  auto it = g_sounds.sounds.find(id);
  if (it == g_sounds.sounds.end()) return;
  SetSoundVolume(it->second, static_cast<float>(gain_of(vol)));
  PlaySound(it->second);  // restarts if already playing
}

void sound_stop(int64_t id) {
  auto it = g_sounds.sounds.find(id);
  if (it != g_sounds.sounds.end()) StopSound(it->second);
}

bool sound_playing(int64_t id) {
  auto it = g_sounds.sounds.find(id);
  return it != g_sounds.sounds.end() && IsSoundPlaying(it->second);
}

void sound_free(int64_t id) {
  auto it = g_sounds.sounds.find(id);
  if (it == g_sounds.sounds.end()) return;
  StopSound(it->second);
  UnloadSound(it->second);
  g_sounds.sounds.erase(it);
}

}  // namespace _canvas_detail
}  // namespace culebra
