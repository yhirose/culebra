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
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <numbers>
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

// vol/peak arrive as 0..100; keep the mix well under clipping when several
// channels stack, matching the browser's "keep it gentle" 0.2 headroom.
double gain_of(int64_t v) { return std::clamp(v, int64_t{0}, int64_t{100}) / 100.0 * 0.2; }

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
    g_audio_failed = true;  // no device (headless server/CI): stay silent
    return;
  }
  g_stream = LoadAudioStream(kSampleRate, 32, 1);  // 32-bit float, mono
  SetAudioStreamCallback(g_stream, audio_callback);
  PlayAudioStream(g_stream);
  g_audio_ready = true;
}

struct AudioCloser {
  ~AudioCloser() {
    if (g_audio_ready) {
      UnloadAudioStream(g_stream);
      CloseAudioDevice();
    }
  }
};
AudioCloser g_audio_closer;

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
// Defined after g_audio_closer on purpose: statics destroy in reverse order,
// so the slot's UnloadMusicStream runs before CloseAudioDevice.
MusicSlot g_music;

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
  n.vol = gain_of(vol);
  n.peak = gain_of(peak);
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

}  // namespace _canvas_detail
}  // namespace culebra
