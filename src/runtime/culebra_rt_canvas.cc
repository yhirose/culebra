// Native desktop window backend for the `Canvas` 2D framebuffer namespace,
// built only with -DCULEBRA_ENABLE_CANVAS_WINDOW=ON. The value-neutral
// framebuffer/sprite core lives in include/stdlib/canvas.h and stays raylib-free; this
// file provides just the backend-specific pieces its native branch declares:
// present (upload the frame to a texture, scale it up, block to vsync), polled
// keyboard/mouse input, and the window's close state. It mirrors the Scene
// facade's script-owned-loop model (culebra_rt_scene.cc) — the culebra `run`
// loop drives frames and `present` blocks to the display — so interp/JIT stay
// symmetric with no semantic change from the headless build.
//
// raylib + SDL3 are the same vendored statics Scene links; CMake shares one
// build between the two knobs.

#include "stdlib/canvas.h"
#include "stdlib/keynames.h"

#include <algorithm>
#include <chrono>
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

#include <unicodelib_encodings.h>  // unicode::utf8::encode_codepoint

#include "raylib.h"
#include "rlgl.h"  // viewport + projection, to correct raylib's on a HighDPI window
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
// The screen layer's texture (canvas.h): the framebuffer at the drawable's own
// pixel size, blitted 1:1 over the upscaled frame so its antialiased text keeps
// the edges the nearest-neighbour upscale of g_tex would turn into blocks.
Texture2D g_screen_tex;
int g_screen_tex_w = 0;
int g_screen_tex_h = 0;
bool g_screen_tex_dirty = false;  // last frame drew into it; clear it once
// The drawable's size in real device pixels, which is g_scale * the display's
// DPI scale times the framebuffer — 2x that on a Retina panel. See
// sync_render_size() for why this is tracked here rather than read off raylib.
int g_render_w = 0;
int g_render_h = 0;
bool g_window_ready = false;
bool g_window_failed = false;  // creation tried and failed; don't try again
// Set by quit() (below); closing() ORs it into WindowShouldClose() so a
// script-requested quit stops Canvas.run's loop the same way the window's
// own close box does, with no separate signal for `run` to know about.
bool g_quit_requested = false;
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

// Record the drawable's real size in device pixels, and correct raylib's idea
// of it.
//
// FLAG_WINDOW_HIGHDPI (set before InitWindow, below) makes SDL give the window
// a high-density backing store — 1280x760 device pixels for a 640x380-point
// window on a Retina panel — which is what lets the screen layer hold text at
// the resolution the display can actually show. raylib's SDL backend creates
// that window correctly but then records `render` as the POINT size and never
// asks SDL for the pixel size, so its viewport and projection cover a quarter
// of the drawable. Rather than patch the vendored raylib, ask SDL directly and
// set the viewport/projection ourselves in present(): BeginDrawing() touches
// only the modelview (and CORE.Window.screenScale, which stays identity here),
// so nothing overwrites it per frame.
//
// On a non-Retina display the pixel size equals the point size and all of this
// reduces to exactly what the code did before.
void sync_render_size() {
  // Point size is the floor, and the answer wherever the drawable is 1x.
  g_render_w = GetScreenWidth();
  g_render_h = GetScreenHeight();
  // The SDL window has to come from SDL's own window list, the way
  // stop_text_input() gets it: raylib's GetWindowHandle() hands back the
  // PLATFORM window (an NSWindow* on macOS), not the SDL_Window*, so passing
  // it to an SDL call quietly does nothing.
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) return;
  if (count == 1) {  // the process opens exactly one window
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(windows[0], &pw, &ph) && pw > 0 && ph > 0) {
      g_render_w = pw;
      g_render_h = ph;
    }
  }
  SDL_free(windows);
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
    // Ask for a high-density backing store; harmless where there isn't one.
    // Must precede InitWindow — raylib only maps this to SDL's window flag at
    // creation (SetWindowState warns it cannot change it afterwards).
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);
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
    // raylib's own default: Esc alone force-closes the window with no
    // chance for a script to intercept it (closing() would report it exactly
    // like the window's close box, one frame too late to ask "are you
    // sure?"). A game that wants Esc back for something else — or wants it
    // to quit outright — reads Canvas.key("escape") and/or calls quit()
    // itself instead.
    SetExitKey(KEY_NULL);
    g_window_ready = true;
    arm_exit_teardown();
  } else {
    UnloadTexture(g_tex);
    SetWindowSize(w * g_scale, h * g_scale);
  }
  sync_render_size();  // both arms changed the drawable
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
// The name table itself lives in stdlib/keynames.h, shared with the Scene
// backend so the two namespaces cannot drift apart on what a key is called.
using keynames::key_code_of;
using keynames::key_name_of;

// A unicode code point as UTF-8, for the typed-characters queue. A negative
// or non-scalar value (raylib hands over whatever the platform reported)
// yields the empty string rather than ill-formed bytes.
std::string utf8_of(int cp) {
  std::string out;
  if (cp >= 0) unicode::utf8::encode_codepoint(static_cast<char32_t>(cp), out);
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
  double start_freq = 0, end_freq = 0;
  int64_t attack = 0, decay = 0, sustain = 0, release = 0;  // samples
  int64_t total = 0;                                        // samples
  double vol = 0, peak = 0;   // 0..1 gain, already scaled and headroom-capped
  double duty = 0.5;          // pulse only
  int64_t elapsed = 0;        // samples into the note; audio thread owns this
  double phase = 0;           // 0..1, oscillator channels
  double lp_state = 0;        // noise channel's one-pole lowpass
};

// A note carries where it starts, on the stream's own sample clock. tone()
// runs on the main thread while the mixer renders whole buffers at once, so
// writing straight to the sounding voice keeps only the last note a buffer
// spans and pins every onset to a buffer edge — invisible at the 10 ms device
// default, a third of the tune gone once a period outlasts the gap between
// notes. This is what WebAudio's own src.start(now) gives the browser side.
struct Queued {
  int64_t start = 0;   // absolute position in the stream, in samples
  int channel = 0;
  Note note;
};

// Bounded on both sides of the handover: a device that stalls must not grow
// these without limit, and the oldest note is the one to lose, being the one a
// monophonic channel would have cut anyway.
constexpr int kQueueCap = 64;

std::mutex g_audio_mutex;
Queued g_inbox[kQueueCap];   // handed over by tone(), drained once per buffer
int g_inbox_count = 0;
// Where the stream stands once the buffer being rendered is done, and the
// instant that was. tone() dates itself against this pair, so notes keep their
// spacing however long a buffer is.
int64_t g_stream_next = 0;
std::chrono::steady_clock::time_point g_stream_at{};
int64_t g_last_start = 0;    // notes are queued in order, never behind

// The mixer's own state: the voice sounding on each channel, and the notes
// still waiting for their sample. Read and written only on the audio thread.
Note g_voice[5];
Queued g_pending[kQueueCap];
int g_pending_count = 0;
int64_t g_pos = 0;           // samples rendered so far

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

// raylib's audio thread callback: fill `frames` mono float samples. The lock
// is held only to take the handover, never across the render, so a long buffer
// cannot delay tone() on the main thread. Everything the render touches after
// that belongs to this thread alone.
void audio_callback(void* buffer_data, unsigned int frames) {
  float* out = static_cast<float*>(buffer_data);
  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    // Losing the oldest keeps the newest, which is what a channel would be
    // sounding by the time the queue drained anyway.
    int drop = g_pending_count + g_inbox_count - kQueueCap;
    if (drop > 0) {
      drop = std::min(drop, g_pending_count);
      std::move(g_pending + drop, g_pending + g_pending_count, g_pending);
      g_pending_count -= drop;
    }
    for (int i = 0; i < g_inbox_count && g_pending_count < kQueueCap; i++) {
      g_pending[g_pending_count++] = g_inbox[i];
    }
    g_inbox_count = 0;
    g_stream_next = g_pos + frames;
    g_stream_at = std::chrono::steady_clock::now();
  }

  int head = 0;   // g_pending is ordered by start, so only the head can be due
  for (unsigned int i = 0; i < frames; i++) {
    int64_t p = g_pos + static_cast<int64_t>(i);
    // A note whose sample has come round takes its channel, cutting whatever
    // was sounding there. One already behind (the device fell back) starts
    // here rather than being dropped.
    while (head < g_pending_count && g_pending[head].start <= p) {
      g_voice[g_pending[head].channel] = g_pending[head].note;
      head++;
    }
    double mixed = 0.0;
    for (int c = 0; c < 5; c++) {
      Note& n = g_voice[c];
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

  std::move(g_pending + head, g_pending + g_pending_count, g_pending);
  g_pending_count -= head;
  g_pos += frames;
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
  // Date the stream clock before the device can call back: a tone() issued
  // ahead of the first buffer would otherwise be measured from the epoch and
  // scheduled past any sample this process will ever render.
  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_stream_at = std::chrono::steady_clock::now();
  }
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
    if (g_screen_tex_w > 0) UnloadTexture(g_screen_tex);
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

// (Re)create the screen layer's texture when its size changes, the same shape
// ensure_window() uses for g_tex. No filter is set: this one is blitted 1:1,
// so there is nothing to filter.
void ensure_screen_texture() {
  // Never drawn to, so never shown: a program that does not use the screen
  // layer should not pay for a drawable-sized texture on the GPU.
  if (g_screen_tex_w == 0 && !_screen_dirty()) return;
  int w = _screen_w();
  int h = _screen_h();
  if (w == g_screen_tex_w && h == g_screen_tex_h) return;
  if (g_screen_tex_w > 0) UnloadTexture(g_screen_tex);
  g_screen_tex_w = 0;
  g_screen_tex_h = 0;
  if (w <= 0 || h <= 0) return;
  Image img = GenImageColor(w, h, BLANK);
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  g_screen_tex = LoadTextureFromImage(img);
  UnloadImage(img);
  g_screen_tex_w = w;
  g_screen_tex_h = h;
}

}  // namespace

void present() {
  // Pump the music stream before anything window-related: a display-less (but
  // audible) run degrades to no window, and must still keep playing.
  music_pump();
  ensure_window();
  if (!g_window_ready) {
    screen_buffer_reset();
    return;
  }
  // ensure_window() only resyncs the render size against a framebuffer
  // resize (a script's own Canvas.init() at a new w/h); the window's actual
  // drawable can also change on its own -- toggle_fullscreen() swaps it to
  // the display's full size, and a resizable() window can be dragged by the
  // user -- neither of which touches the framebuffer at all. Left stale,
  // present() below draws into a viewport sized for the OLD drawable, which
  // reads as the frame pinned in a corner of the new one. Every frame is the
  // simplest fix that is still correct every time, and the two SDL/GL calls
  // it costs are cheap next to a whole present().
  sync_render_size();
  const std::vector<uint32_t>& fb = _fb();
  if (!fb.empty()) UpdateTexture(g_tex, fb.data());
  // The screen layer is uploaded only on frames that drew into it, plus the one
  // frame after — a program that never draws screen text pays nothing per
  // frame, and one that stops drawing it gets the layer cleared once rather
  // than leaving its last text stuck on screen.
  ensure_screen_buffer();
  ensure_screen_texture();
  bool blit_screen = g_screen_tex_w > 0 && (_screen_dirty() || g_screen_tex_dirty);
  if (blit_screen) {
    const std::vector<uint32_t>& sfb = _screen_fb();
    if (!sfb.empty()) UpdateTexture(g_screen_tex, sfb.data());
    g_screen_tex_dirty = _screen_dirty();
  }
  BeginDrawing();
  // Draw in device pixels: raylib sized its viewport and projection from the
  // window's POINT size, which is half the drawable on a Retina panel (see
  // sync_render_size). BeginDrawing leaves the projection alone, so setting it
  // here holds for the frame.
  rlViewport(0, 0, g_render_w, g_render_h);
  rlMatrixMode(RL_PROJECTION);
  rlLoadIdentity();
  rlOrtho(0, g_render_w, g_render_h, 0, 0.0f, 1.0f);
  rlMatrixMode(RL_MODELVIEW);
  rlLoadIdentity();
  ClearBackground(BLACK);
  DrawTexturePro(g_tex, Rectangle{0, 0, (float)g_tex_w, (float)g_tex_h},
                 Rectangle{0, 0, (float)g_render_w, (float)g_render_h},
                 Vector2{0, 0}, 0.0f, WHITE);
  // 1:1 over the upscaled frame: g_screen_tex is sized fb * screen_scale(),
  // and screen_scale() is defined as the drawable over the framebuffer, so it
  // is exactly g_render_w x g_render_h — one texel per device pixel, which is
  // the whole point of the layer.
  if (blit_screen)
    DrawTexturePro(g_screen_tex,
                   Rectangle{0, 0, (float)g_screen_tex_w, (float)g_screen_tex_h},
                   Rectangle{0, 0, (float)g_render_w, (float)g_render_h},
                   Vector2{0, 0}, 0.0f, WHITE);
  EndDrawing();  // blocks to vsync / the 60 fps target
  drain_key_events();
  screen_buffer_reset();
}

// Framebuffer pixels -> device pixels: the window's integer upscale times the
// display's DPI scale, taken straight from the drawable's real size so the
// screen layer comes out exactly one texel per device pixel.
//
// ensure_window() first: a script can draw screen text inside its very first
// tick(), before any present() has opened the window, and the layer has to be
// sized for the window that tick's frame will land in — every other backend
// entry point resolves the window the same way.
double screen_scale() {
  ensure_window();
  if (!g_window_ready || g_tex_w <= 0 || g_render_w <= 0) return 1.0;
  return static_cast<double>(g_render_w) / static_cast<double>(g_tex_w);
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

// Gamepads, by slot (0-3, raylib's MAX_GAMEPADS): axis/button numbers are
// raylib's own GamepadAxis/GamepadButton enum values, which canvas.cul
// mirrors as named constants so a script never has to know the numbers.
// Scene.View exposes the same thing (pad_available/pad_axis/pad_button/
// pad_pressed/pad_name/gamepad_mappings/rumble, gamepad 0 only) — this is
// that, generalized to a slot argument, so Canvas games get it too.
bool pad_available(int64_t index) {
  ensure_window();
  if (!g_window_ready) return false;
  return IsGamepadAvailable(static_cast<int>(index));
}
double pad_axis(int64_t index, int64_t axis) {
  ensure_window();
  if (!g_window_ready) return 0.0;
  return static_cast<double>(
      GetGamepadAxisMovement(static_cast<int>(index), static_cast<int>(axis)));
}
bool pad_button(int64_t index, int64_t button) {
  ensure_window();
  if (!g_window_ready) return false;
  return IsGamepadButtonDown(static_cast<int>(index), static_cast<int>(button));
}
bool pad_pressed(int64_t index, int64_t button) {
  ensure_window();
  if (!g_window_ready) return false;
  return IsGamepadButtonPressed(static_cast<int>(index),
                                static_cast<int>(button));
}
std::string pad_name(int64_t index) {
  ensure_window();
  if (!g_window_ready) return "";
  const char* n = GetGamepadName(static_cast<int>(index));
  return n ? n : "";
}
// Rumble strength 0..1 each motor, `sec` seconds. Silently does nothing on a
// backend/pad without haptics (Xbox pads on macOS: no API drives them).
void pad_rumble(int64_t index, double left, double right, double sec) {
  ensure_window();
  if (!g_window_ready) return;
  SetGamepadVibration(static_cast<int>(index), static_cast<float>(left),
                      static_cast<float>(right), static_cast<float>(sec));
}
// Load extra SDL_GameControllerDB mapping lines for pads the bundled DB
// lacks. Returns 1 on success, matching Scene.View.gamepad_mappings.
int64_t pad_mappings(const char* db) {
  ensure_window();
  if (!g_window_ready) return 0;
  return SetGamepadMappings(db);
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

// Vertical wheel delta this frame (positive = away from the user, matching
// the browser's -deltaY convention for scroll-to-zoom-in).
double mouse_wheel() {
  ensure_window();
  if (!g_window_ready) return 0.0;
  return static_cast<double>(GetMouseWheelMove());
}

// Seconds since the previous present() — Scene.View has the same dt(); a
// Canvas game that wants to step by real elapsed time (rather than the fixed
// 1/60 a vsynced tick() otherwise assumes) reads this instead.
double dt() {
  ensure_window();
  if (!g_window_ready) return 0.0;
  return static_cast<double>(GetFrameTime());
}

// SetTargetFPS(0) uncaps it; the default from ensure_window() is 60, matching
// the browser loop, so this only matters to a game that wants something else.
void set_target_fps(int64_t fps) {
  ensure_window();
  if (!g_window_ready) return;
  SetTargetFPS(static_cast<int>(fps));
}
int64_t fps() {
  ensure_window();
  if (!g_window_ready) return 0;
  return GetFPS();
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

// True once the window's close box has been used or a script called quit(),
// so the run loop stops. Before the window exists there is nothing to close.
// Esc no longer contributes on its own -- see the SetExitKey(KEY_NULL) note
// in ensure_window().
bool closing() {
  ensure_window();
  if (!g_window_ready) return false;
  return WindowShouldClose() || g_quit_requested;
}

// A script's own "close the window" -- the same event WindowShouldClose()
// reports for the close box, so closing() can't tell them apart and nothing
// downstream (the run loop, a program polling closing() itself) needs to.
void quit() {
  ensure_window();
  if (g_window_ready) g_quit_requested = true;
}
// Whether quit() (and the fullscreen/cursor/clipboard primitives beside it)
// does anything here: true only with a real window to close, so a script can
// decide whether an in-game "quit?" prompt makes sense before offering one
// (there is nothing to close on the wasm or headless backends).
bool can_quit() {
  ensure_window();
  return g_window_ready;
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

// SDL (the platform backend here) applies fullscreen and the resizable flag
// live, unlike FLAG_WINDOW_HIGHDPI above, so none of these need to happen
// before ensure_window() the way that one does.
void toggle_fullscreen() {
  ensure_window();
  if (!g_window_ready) return;
  ToggleFullscreen();
}
bool is_fullscreen() {
  ensure_window();
  if (!g_window_ready) return false;
  return IsWindowFullscreen();
}
void show_cursor() {
  ensure_window();
  if (g_window_ready) ShowCursor();
}
void hide_cursor() {
  ensure_window();
  if (g_window_ready) HideCursor();
}
bool cursor_hidden() {
  ensure_window();
  if (!g_window_ready) return false;
  return IsCursorHidden();
}
std::string clipboard_get() {
  ensure_window();
  if (!g_window_ready) return "";
  const char* s = GetClipboardText();
  return s ? s : "";
}
void clipboard_set(const char* text) {
  ensure_window();
  if (g_window_ready) SetClipboardText(text);
}
void set_resizable(bool enabled) {
  ensure_window();
  if (!g_window_ready) return;
  if (enabled) {
    SetWindowState(FLAG_WINDOW_RESIZABLE);
  } else {
    ClearWindowState(FLAG_WINDOW_RESIZABLE);
  }
}
// One-frame edge, like WindowShouldClose: true only on the frame the OS
// window's pixel size last changed (the user dragging an edge, maximizing,
// entering fullscreen). The framebuffer itself does not follow — present()
// still scales the same logical w x h to fit whatever the window now is.
bool window_resized() {
  ensure_window();
  if (!g_window_ready) return false;
  return IsWindowResized();
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
  if (g_inbox_count == kQueueCap) {   // see kQueueCap: the oldest is the loss
    std::move(g_inbox + 1, g_inbox + kQueueCap, g_inbox);
    g_inbox_count--;
  }
  double ahead = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                               g_stream_at).count();
  int64_t start = g_stream_next + static_cast<int64_t>(ahead * kSampleRate);
  // The queue is walked head-first, and a sequencer never means to place a
  // note behind one it has already asked for, so clock jitter across a buffer
  // edge settles in favour of the order the calls came in.
  start = std::max(start, g_last_start);
  g_last_start = start;
  g_inbox[g_inbox_count++] = Queued{start, static_cast<int>(channel), n};
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
