#pragma once

// The Audio namespace's backend choke: the one owner of the sound device and
// of everything that plays through it (tone, Sound, Music, PCM, and the
// scores Audio.Kauai plays) or listens on the microphone (Capture). The
// script-facing surface is src/preambles/audio.cul and the natives are in
// stdlib/bindings.h; this header picks the backend.
//
//   native (CULEBRA_AUDIO_NATIVE)   raylib's audio module built standalone
//                                   (src/runtime/culebra_raudio.c), with the
//                                   bodies in src/runtime/culebra_rt_audio.cc
//   base AOT archive of a native    weak silent bodies, overridden by the Audio
//   build (CULEBRA_RT_AUDIO_WEAK)   feature archive when a program names Audio
//   browser (__EMSCRIPTEN__)        WebAudio on the page (playground/app.js);
//                                   PCM is silent there
//   anything else                   silent, as a machine with no device is
//
// Silent means what a native build answers on a machine with no audio device:
// nothing plays, a PCM's block counts pushes all the same, and a Capture
// hears nothing.

#include <stdlib/pcm_block.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include <variant>
#include <vector>

// The table above, decided once.
#if defined(__EMSCRIPTEN__)
#define CULEBRA_AUDIO_BROWSER 1
#include <emscripten.h>
#elif defined(CULEBRA_AUDIO_NATIVE) && !defined(CULEBRA_RT_AUDIO_WEAK)
#define CULEBRA_AUDIO_DEVICE 1
#else
#define CULEBRA_AUDIO_SILENT 1
#endif

namespace culebra {
namespace _audio_detail {

// The audio container `data` opens with, named as the extension string
// raylib's decoder dispatch keys on: an ID3v2 tag or an MPEG frame sync
// (0xFF then the top three bits set — MPEG2/mono encodes don't start 0xFF
// 0xFB) is MP3, an "OggS" capture pattern is Ogg, anything else (or a byte
// count past raylib's int size) is nullptr. This sniff runs before any
// backend: the browser's own decodeAudioData is asynchronous, so leaving
// detection to it would give wasm a different error site than the others.
inline const char* music_format(const uint8_t* p, size_t n) {
  if (p == nullptr || n < 4 || n > static_cast<size_t>(INT32_MAX))
    return nullptr;
  if (p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') return ".ogg";
  if (p[0] == 'I' && p[1] == 'D' && p[2] == '3') return ".mp3";
  if (p[0] == 0xff && (p[1] & 0xe0) == 0xe0) return ".mp3";
  return nullptr;
}
inline constexpr auto kMusicFormatError = "not a valid MP3 or Ogg audio stream";

// A Sound accepts WAV on top of Music's MP3/Ogg: the natural container for a
// one-shot sample.
inline const char* sound_format(const uint8_t* p, size_t n) {
  if (p != nullptr && n >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' &&
      p[3] == 'F' && p[8] == 'W' && p[9] == 'A' && p[10] == 'V' && p[11] == 'E')
    return ".wav";
  return music_format(p, n);
}
inline constexpr auto kSoundFormatError =
    "not a valid WAV, MP3 or Ogg audio stream";

// Samples (-1..1, mono, `rate` a second) as the 16-bit WAV file a program
// would load, so a Sound made from samples decodes as one from bytes does.
inline std::vector<uint8_t> wav16(const double* x, size_t n, uint32_t rate) {
  std::vector<uint8_t> out;
  out.reserve(44 + 2 * n);
  auto put = [&](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto data = static_cast<uint32_t>(2 * n);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put(36 + data, 4);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(16, 4);
  put(1, 2);  // PCM
  put(1, 2);  // mono
  put(rate, 4);
  put(rate * 2, 4);
  put(2, 2);
  put(16, 2);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put(data, 4);
  for (size_t i = 0; i < n; i++) {
    double v = std::isnan(x[i]) ? 0.0 : std::clamp(x[i], -1.0, 1.0);
    put(static_cast<uint16_t>(static_cast<int16_t>(std::nearbyint(v * 32767))), 2);
  }
  return out;
}

inline constexpr auto kSamplesError =
    "type error: parameter 'samples' expects an Array of Long|Float";
inline constexpr auto kScoreEventsError =
    "type error: parameter 'events' expects an Array of Long|Float";

// Sound, Music and PCM handles: one counter for every backend, so a
// handle's lifecycle reads the same whether or not a host can play it.
inline int64_t alloc_id() {
  static int64_t n = 0;
  return ++n;
}

// The rate the mixer runs at, which a graph played must have.
inline constexpr double kMixerRate = 44100;

// Numbers an event takes in a score's flat list (score_new).
inline constexpr int64_t kScoreStride = 11;

// --- Parts an instrument builds its sound from, the same on every backend ---
//
// Each part is a processor: its state, and a step that takes it one sample
// on. The functions that work out a whole array at once (resonate, pluck,
// filter, envelope, partials) run them in a loop, and so does a Graph as it
// plays, so a part sounds the same either way. What a part reads that moves
// over time is a Param.

// The sample nearest `seconds` in, a tie going to the even one as Math.round
// has it, so a part and the script that times it agree on the sample.
inline int64_t sample_at(double seconds, double rate) {
  return static_cast<int64_t>(std::nearbyint(seconds * rate));
}

// A part's state too small to hear (-600 dB) is silence. Kept, a state left
// with nothing going in decays into subnormal numbers, which x86 works out
// many times slower, and a part that plays on never leaves them.
inline double settled(double x) { return std::abs(x) < 1e-30 ? 0.0 : x; }

// A change scheduled for a value, `at` seconds in: from then on it is
// `value`, or (`ramp`) it arrives there in a straight line from the change
// before. Web Audio's setValueAtTime and linearRampToValueAtTime. A change
// takes effect at the first sample whose time, its index over the rate, is
// `at` or later.
struct ParamEvent {
  double at = 0;
  bool ramp = false;
  double value = 0;
};

// A value a part reads, `rest` until its first change and then moved by its
// changes; asked for at times that only go forward.
class Param {
 public:
  explicit Param(double rest = 0) : rest_(rest), value_(rest), from_value_(rest) {}
  // A value through `n` numbers of (at, value) pairs, `at` in seconds: it
  // holds its first value before them and its last after, moving in straight
  // lines between; two at the same time are a step. `rest` when there are none.
  static Param points(const double* pairs, size_t n, double rest) {
    Param p(n >= 2 ? pairs[1] : rest);
    for (size_t k = 0; k + 1 < n; k += 2) p.schedule({pairs[k], k > 0, pairs[k + 1]});
    return p;
  }

  // After the changes already scheduled for the same time.
  void schedule(ParamEvent e) {
    auto it = std::upper_bound(events_.begin() + static_cast<std::ptrdiff_t>(next_), events_.end(),
                               e.at, [](double at, const ParamEvent& x) { return at < x.at; });
    events_.insert(it, e);
  }
  // Room for `extra` changes more without allocating.
  void make_room(size_t extra) { events_.reserve(events_.size() + extra); }
  // schedule() that never allocates: the changes already passed make room
  // when there is none, and false when there is still none.
  bool schedule_in_place(ParamEvent e) {
    if (events_.size() == events_.capacity()) {
      if (next_ == 0) return false;
      events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(next_));
      next_ = 0;
    }
    schedule(e);
    return true;
  }
  double at(double t) {
    while (next_ < events_.size()) {
      const ParamEvent& e = events_[next_];
      if (e.at > t) {
        if (!e.ramp) break;
        return from_value_ + (e.value - from_value_) * (t - from_at_) / (e.at - from_at_);
      }
      value_ = from_value_ = e.value;
      from_at_ = e.at;
      next_++;
    }
    return value_;
  }

 private:
  std::vector<ParamEvent> events_;
  size_t next_ = 0;
  double rest_, value_, from_value_;
  double from_at_ = 0;
};

inline constexpr auto kResonateModesError =
    "Audio.resonate: a mode is {freq, width, gain}, each a number";

// A two-pole resonator, its poles `width` Hz wide about `freq` Hz, scaled so
// its peak is about `gain`.
struct Resonator {
  double a1, a2, b0, y1 = 0, y2 = 0;

  Resonator(double freq, double width, double gain, double rate) {
    double r = std::exp(-std::numbers::pi * width / rate);
    double w = 2.0 * std::numbers::pi * freq / rate;
    a1 = 2.0 * r * std::cos(w);
    a2 = -r * r;
    b0 = (1.0 - r) * 2.0 * std::sin(w) * gain;
  }
  double step(double x) {
    double y = settled(b0 * x + a1 * y1 + a2 * y2);
    y2 = y1;
    y1 = y;
    return y;
  }
};

// A bank of resonances, `modes` three numbers each (freq and width in Hz,
// gain), plus `dry` of what goes in.
struct Resonance {
  std::vector<Resonator> modes;
  double dry = 0;

  Resonance(const double* m, int64_t count, double dry, double rate) : dry(dry) {
    modes.reserve(static_cast<size_t>(count));
    for (int64_t k = 0; k < count; k++) modes.emplace_back(m[3 * k], m[3 * k + 1], m[3 * k + 2], rate);
  }
  double step(double x) {
    double v = dry * x;
    for (auto& r : modes) v += r.step(x);
    return v;
  }
};

// `x` through a Resonance, into `out` (as long as `x`).
inline void resonate(const double* x, int64_t n, const double* modes,
                     int64_t m, double dry, double rate, double* out) {
  Resonance bank(modes, m, dry, rate);
  for (int64_t i = 0; i < n; i++) out[i] = bank.step(x[i]);
}

inline constexpr auto kPluckTuningsError =
    "Audio.pluck: a tuning is {at, freq, ring}, each a number";
inline constexpr auto kPluckStrikesError =
    "Audio.pluck: a strike is {at, samples}, its samples numbers";

// A plucked string (Karplus-Strong), tuned to `freq` Hz and ringing down 60 dB
// in `ring` seconds: a whole-sample delay, an allpass for the fraction (Jaffe
// and Smith), and the loss each pass, its loop no longer than `size` - 1
// samples. What goes in is added into the loop as it passes, and the string
// moves on from what it holds whenever its tuning changes. What it answers
// passes no DC.
class PluckString {
 public:
  PluckString(int64_t size, double rate)
      : buf_(static_cast<size_t>(size), 0.0), rate_(rate), pole_(std::pow(0.995, 44100.0 / rate)) {}
  // The loop a string at `freq` Hz needs.
  static int64_t loop_length(double freq, double rate) {
    return static_cast<int64_t>(std::floor(rate / freq - 0.6));
  }
  void tune(double freq, double ring) {
    if (freq == freq_ && ring == ring_) return;
    freq_ = freq;
    ring_ = ring;
    double period = rate_ / freq;
    double whole = std::floor(period - 0.6);
    double frac = period - 0.5 - whole;
    len_ = static_cast<int64_t>(whole);
    c_ = (1.0 - frac) / (1.0 + frac);
    loss_ = std::pow(10.0, -3.0 / (ring * freq));
  }
  double step(double kick) {
    auto size = static_cast<int64_t>(buf_.size());
    double read = buf_[static_cast<size_t>((w_ - len_ + size) % size)];
    double v = 0.5 * (read + prev_) * loss_;
    prev_ = read;
    double ap = settled(c_ * v + ap_in_ - c_ * ap_out_);
    ap_in_ = v;
    ap_out_ = ap;
    buf_[static_cast<size_t>(w_)] = ap + kick;
    w_ = (w_ + 1) % size;
    dc_out_ = settled(read - dc_in_ + pole_ * dc_out_);
    dc_in_ = read;
    return dc_out_;
  }
  // Still, as made.
  void reset() {
    std::ranges::fill(buf_, 0.0);
    prev_ = ap_in_ = ap_out_ = dc_in_ = dc_out_ = 0;
    w_ = 0;
    freq_ = ring_ = -1;
  }

 private:
  std::vector<double> buf_;
  double rate_, pole_;
  double freq_ = -1, ring_ = -1, c_ = 0, loss_ = 0;
  int64_t len_ = 1, w_ = 0;
  double prev_ = 0, ap_in_ = 0, ap_out_ = 0, dc_in_ = 0, dc_out_ = 0;
};

// `n` samples of a PluckString: `tunings` three numbers each (at in seconds,
// freq in Hz, ring in seconds), in the order they come, each from its sample
// on and the first from the start; `kick` (n long) goes in.
inline void pluck(int64_t n, const double* tunings, int64_t nt,
                  const double* kick, double rate, double* out) {
  int64_t longest = 1;
  Param freq(tunings[1]), ring(tunings[2]);
  for (int64_t k = 0; k < nt; k++) {
    longest = std::max(longest, PluckString::loop_length(tunings[3 * k + 1], rate));
    // The tuning's sample, as a time.
    double from = static_cast<double>(sample_at(tunings[3 * k], rate)) / rate;
    freq.schedule({from, false, tunings[3 * k + 1]});
    ring.schedule({from, false, tunings[3 * k + 2]});
  }
  PluckString string(longest + 1, rate);
  for (int64_t i = 0; i < n; i++) {
    double t = static_cast<double>(i) / rate;
    string.tune(freq.at(t), ring.at(t));
    out[i] = string.step(kick[i]);
  }
}

inline constexpr auto kPartialsEnvelopeError =
    "Audio.partials: an envelope is a list of (at, value), each a number";
inline constexpr auto kFilterEnvelopeError =
    "Audio.filter: a cutoff is a number, or a list of (at, value) each a number";
inline constexpr auto kEnvelopeError =
    "Audio.envelope: an envelope is a list of (at, value), each a number";
inline constexpr auto kMixPartsError =
    "Audio.mix: a part is samples, or {samples, at, gain}, its samples numbers";
inline constexpr auto kPartialsSpectrumError =
    "Audio.partials: a spectrum is a list of (at, amps), amps a list of numbers";

// What a filter lets through: below its cutoff, above it, or about it.
enum class FilterKind { Low, High, Band };

// A two-pole state-variable filter, trapezoidal as Zavalishin and Simper
// write it: the low, high or band output (a band's peak at 0 dB) of one state
// that stays sound however fast its cutoff moves, `q` its resonance (0.707
// the flattest, 0.5 two one-pole stages). Each answers as the analog filter
// does through the bilinear transform, as the RBJ cookbook's biquads do.
struct Svf {
  FilterKind kind = FilterKind::Low;
  double k = 1.0 / 0.707, a1 = 0, a2 = 0, a3 = 0, s1 = 0, s2 = 0, at = -1;

  Svf() = default;
  Svf(FilterKind kind, double q) : kind(kind), k(1.0 / q) {}
  void tune(double cutoff, double rate) {
    if (cutoff == at) return;
    at = cutoff;
    double g = std::tan(std::numbers::pi * cutoff / rate);
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  double step(double x) {
    double v3 = x - s2;
    double v1 = a1 * s1 + a2 * v3;
    double v2 = s2 + a2 * s1 + a3 * v3;
    s1 = settled(2.0 * v1 - s1);
    s2 = settled(2.0 * v2 - s2);
    switch (kind) {
      case FilterKind::Low: return v2;
      case FilterKind::High: return x - k * v1 - v2;
      case FilterKind::Band: return k * v1;
    }
    return 0.0;
  }
  // Silent, as made.
  void reset() {
    s1 = s2 = 0;
    at = -1;
  }
};

// `x` (`n` long) through an Svf whose cutoff follows `cutoff`, into `out`
// (which may be `x`).
inline void filter(const double* x, int64_t n, FilterKind kind, Param cutoff,
                   double q, double rate, double* out) {
  Svf svf(kind, q);
  for (int64_t i = 0; i < n; i++) {
    svf.tune(cutoff.at(static_cast<double>(i) / rate), rate);
    out[i] = svf.step(x[i]);
  }
}

// `x` (`n` long) times `gain`, into `out` (which may be `x`).
inline void envelope(const double* x, int64_t n, Param gain, double rate, double* out) {
  for (int64_t i = 0; i < n; i++) out[i] = x[i] * gain.at(static_cast<double>(i) / rate);
}

// The same source of white noise, -1..1, from the same seed (a linear
// congruential generator, as the examples' wave.noise is).
struct Noise {
  int64_t x;
  double next() {
    x = (x * 1103515245 + 12345) % 2147483648;
    return static_cast<double>(x) / 1073741824.0 - 1.0;
  }
};

// The pieces of a harmonic tone for partials(): its spectra, `count` amps
// each starting at `from` in `amps`, and the noise heard with it.
struct PartialsSpectrum {
  std::vector<double> at;
  std::vector<int64_t> from, count;
  const double* amps;
};
struct PartialsNoise {
  bool on;
  double fast, slow;  // one-pole coefficients at the band's top and bottom
  int64_t seed;
  Param steady, pulsed;
};

// `n` samples of a harmonic tone: its frequency and gain, its spectrum moving
// in straight lines between the amps given at their times (the k-th of them
// at k times the frequency, those at or above 0.45 of the rate left out), and
// band noise heard steadily and pulsing with the tone's wave (0.5 + 0.5 sin
// of its phase), as air through a reed is.
inline void partials(int64_t n, Param freq, Param gain, const PartialsSpectrum& sp,
                     PartialsNoise noise, double rate, double* out) {
  int64_t widest = 0;
  for (int64_t c : sp.count) widest = std::max(widest, c);
  std::vector<double> amps(static_cast<size_t>(widest), 0.0);
  Noise white{noise.seed};
  double fast = 0, slow = 0, phase = 0;
  double limit = rate * 0.45;
  size_t j = 0;
  for (int64_t i = 0; i < n; i++) {
    double t = static_cast<double>(i) / rate;
    double f = freq.at(t);
    phase += 2.0 * std::numbers::pi * f / rate;
    // The spectrum between the points about `t`.
    while (j + 1 < sp.at.size() && sp.at[j + 1] <= t) j++;
    size_t k1 = j + 1 < sp.at.size() && t > sp.at[j] ? j + 1 : j;
    double x = k1 == j || sp.at[k1] <= sp.at[j] ? 0.0 : (t - sp.at[j]) / (sp.at[k1] - sp.at[j]);
    for (int64_t k = 0; k < widest; k++) {
      double a0 = k < sp.count[j] ? sp.amps[sp.from[j] + k] : 0.0;
      double a1 = k < sp.count[k1] ? sp.amps[sp.from[k1] + k] : 0.0;
      amps[static_cast<size_t>(k)] = a0 + (a1 - a0) * x;
    }
    double s1 = std::sin(phase), c2 = 2.0 * std::cos(phase);
    double below = std::floor(limit / f);
    int64_t top = below >= static_cast<double>(widest) ? widest : static_cast<int64_t>(below);
    double prev = 0, cur = s1, sum = top > 0 ? amps[0] * s1 : 0.0;
    for (int64_t k = 1; k < top; k++) {
      double next = c2 * cur - prev;
      prev = cur;
      cur = next;
      sum += amps[static_cast<size_t>(k)] * cur;
    }
    double v = gain.at(t) * sum;
    if (noise.on) {
      double w = white.next();
      fast += noise.fast * (w - fast);
      slow += noise.slow * (w - slow);
      v += (fast - slow) * (noise.steady.at(t) + noise.pulsed.at(t) * (0.5 + 0.5 * s1));
    }
    out[i] = v;
  }
}

// --- Graph: parts wired together, worked out as they play or ahead ---------

enum class WaveKind { Sine, Saw, Square, Triangle };

// The step a band-limited saw or square smooths at its jump (polyBLEP), `t`
// the phase 0..1 and `dt` its step a sample.
inline double poly_blep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return t * t + t + t + 1.0;
  }
  return 0.0;
}

// A wave from -1 to 1: a sine, a saw or a square whose jumps are smoothed so
// little of it folds back from above half the rate (polyBLEP), or a triangle.
struct Oscillator {
  WaveKind kind;
  double phase = 0;

  double step(double freq, double rate) {
    double t = phase, dt = freq / rate, y;
    switch (kind) {
      case WaveKind::Sine: y = std::sin(2.0 * std::numbers::pi * t); break;
      case WaveKind::Saw: y = 2.0 * t - 1.0 - poly_blep(t, dt); break;
      case WaveKind::Square: {
        double half = t + 0.5 - std::floor(t + 0.5);
        y = (t < 0.5 ? 1.0 : -1.0) + poly_blep(t, dt) - poly_blep(half, dt);
        break;
      }
      default: y = 4.0 * std::abs(t - std::floor(t + 0.5)) - 1.0; break;
    }
    phase = t + dt - std::floor(t + dt);
    return y;
  }
};

enum class PartKind { Wave, Noise, Filter, Gain, Resonate, Pluck };

inline constexpr int kGraphBlock = 128;
// The lowest a graph's string is tuned, which sizes its loop.
inline constexpr double kGraphPluckLowest = 20.0;
// How many graphs play at once: the mixer's list of them never grows.
inline constexpr size_t kGraphsPlaying = 64;
// Changes a playing graph's value takes on top of those it was played with
// before the audio thread drops one rather than allocate.
inline constexpr size_t kGraphLiveRoom = 256;

struct GraphPart {
  PartKind kind;
  std::vector<int> inputs;  // parts made before it, added together
  Param values[2];          // a wave's freq, a filter's cutoff, a gain's gain, a string's freq and ring
  std::variant<std::monostate, Oscillator, Noise, Svf, Resonance, PluckString> processor;
  double out[kGraphBlock] = {};
};

// Parts wired together, each reading the sum of the parts it takes in (made
// before it); what the graph answers is one part's. Every part and value is
// worked out a sample at a time, in blocks that fall at the same samples
// however the samples are asked for, so a graph played and a graph rendered
// answer the same. The graph a program builds is never worked out itself:
// rendering or playing one works out a copy, from the start.
class Graph {
 public:
  explicit Graph(double rate) : rate_(rate) {}
  double rate() const { return rate_; }
  size_t size() const { return parts_.size(); }
  PartKind kind(int part) const { return parts_[static_cast<size_t>(part)].kind; }

  int add(PartKind kind, std::vector<int> inputs, decltype(GraphPart::processor) processor,
          double rest0 = 0, double rest1 = 0) {
    parts_.push_back(GraphPart{kind, std::move(inputs), {Param(rest0), Param(rest1)}, std::move(processor)});
    return static_cast<int>(parts_.size()) - 1;
  }
  void output(int part) { out_ = part; }
  void schedule(int part, int value, ParamEvent e) {
    parts_[static_cast<size_t>(part)].values[value].schedule(e);
  }
  // Room in every value for `kGraphLiveRoom` changes more, so a copy being
  // played takes changes without allocating.
  void make_room() {
    for (auto& p : parts_)
      for (auto& v : p.values) v.make_room(kGraphLiveRoom);
  }
  // A change to a copy being played, on the audio thread: false when there
  // is no room left for it.
  bool schedule_in_place(int part, int value, ParamEvent e) {
    return parts_[static_cast<size_t>(part)].values[value].schedule_in_place(e);
  }

  // The next `n` samples into `out`.
  void render(double* out, int64_t n) {
    for (int64_t done = 0; done < n;) {
      int m = static_cast<int>(std::min<int64_t>(kGraphBlock - pos_ % kGraphBlock, n - done));
      block(m);
      for (int i = 0; i < m; i++) out[done + i] = out_ >= 0 ? parts_[static_cast<size_t>(out_)].out[i] : 0.0;
      done += m;
    }
  }

 private:
  void block(int n) {
    double in[kGraphBlock], v0[kGraphBlock], v1[kGraphBlock];
    for (auto& p : parts_) {
      std::fill(in, in + n, 0.0);
      for (int k : p.inputs)
        for (int i = 0; i < n; i++) in[i] += parts_[static_cast<size_t>(k)].out[i];
      for (int i = 0; i < n; i++) v0[i] = p.values[0].at(static_cast<double>(pos_ + i) / rate_);
      switch (p.kind) {
        case PartKind::Wave: {
          auto& w = std::get<Oscillator>(p.processor);
          for (int i = 0; i < n; i++) p.out[i] = w.step(v0[i], rate_);
          break;
        }
        case PartKind::Noise: {
          auto& z = std::get<Noise>(p.processor);
          for (int i = 0; i < n; i++) p.out[i] = z.next();
          break;
        }
        case PartKind::Filter: {
          auto& f = std::get<Svf>(p.processor);
          for (int i = 0; i < n; i++) {
            f.tune(v0[i], rate_);
            p.out[i] = f.step(in[i]);
          }
          break;
        }
        case PartKind::Gain:
          for (int i = 0; i < n; i++) p.out[i] = in[i] * v0[i];
          break;
        case PartKind::Resonate: {
          auto& r = std::get<Resonance>(p.processor);
          for (int i = 0; i < n; i++) p.out[i] = r.step(in[i]);
          break;
        }
        case PartKind::Pluck: {
          auto& s = std::get<PluckString>(p.processor);
          for (int i = 0; i < n; i++) v1[i] = p.values[1].at(static_cast<double>(pos_ + i) / rate_);
          for (int i = 0; i < n; i++) {
            s.tune(v0[i], v1[i]);
            p.out[i] = s.step(in[i]);
          }
          break;
        }
      }
    }
    pos_ += n;
  }

  double rate_;
  std::vector<GraphPart> parts_;
  int out_ = -1;
  int64_t pos_ = 0;
};

// The graphs a program has made, by handle: the same on every backend, so one
// renders anywhere. Main thread only.
inline std::unordered_map<int64_t, Graph>& graph_defs() {
  static std::unordered_map<int64_t, Graph> defs;
  return defs;
}
inline Graph* graph_def(int64_t id) {
  auto it = graph_defs().find(id);
  return it == graph_defs().end() ? nullptr : &it->second;
}
// The graphs playing, by handle, and when each started on the monotonic
// clock: what graph_now answers with no device to keep time.
inline std::unordered_map<int64_t, std::chrono::steady_clock::time_point>& graphs_playing() {
  static std::unordered_map<int64_t, std::chrono::steady_clock::time_point> playing;
  return playing;
}

#if defined(CULEBRA_AUDIO_BROWSER)

// The page owns WebAudio and each handle's settings; these post commands to it
// (playground/app.js), a setter's or seek's number as `value`. The playing
// flags a script reads back are kept here, optimistically and in step with the
// call, and corrected by the page's "soundState" / "musicState" messages when
// something ends on its own (playground/worker.js).

// A WASM-4-style tone: times in frames at ~60 fps, vol/peak 0..100.
EM_JS(void, _wasm_audio_tone,
      (int start_freq, int end_freq, int attack, int decay, int sustain,
       int release, int vol, int peak, int channel, int duty), {
  postMessage({ type: "tone", startFreq: start_freq, endFreq: end_freq,
                attack: attack, decay: decay, sustain: sustain,
                release: release, vol: vol, peak: peak, channel: channel,
                duty: duty });
});
EM_JS(void, _wasm_audio_sound, (int cmd, int id, double value), {
  const names = ["play", "stop", "free", "volume", "pitch", "pan"];
  const flags = self.__soundsPlaying;
  if (cmd === 0) flags[id] = true;
  else if (cmd === 1) flags[id] = false;
  else if (cmd === 2) delete flags[id];
  postMessage({ type: "sound", cmd: names[cmd], id: id, value: value });
});
EM_JS(void, _wasm_audio_sound_load, (int id, const uint8_t* buf, int len), {
  postMessage({ type: "sound", cmd: "load", id: id,
                buf: HEAPU8.slice(buf, buf + len) });
});
EM_JS(int, _wasm_audio_sound_playing, (int id), {
  return self.__soundsPlaying[id] ? 1 : 0;
});
EM_JS(void, _wasm_audio_music, (int cmd, int id, double value), {
  const names = ["play", "stop", "pause", "resume", "free", "seek",
                 "volume", "pitch", "pan"];
  const flags = self.__musicPlaying;
  if (cmd === 0 || cmd === 3) flags[id] = true;
  else if (cmd === 1 || cmd === 2) flags[id] = false;
  else if (cmd === 4) delete flags[id];
  postMessage({ type: "music", cmd: names[cmd], id: id, value: value });
});
EM_JS(void, _wasm_audio_music_load,
      (int id, const uint8_t* buf, int len, int looping), {
  postMessage({ type: "music", cmd: "load", id: id,
                buf: HEAPU8.slice(buf, buf + len), loop: looping !== 0 });
});
EM_JS(int, _wasm_audio_music_playing, (int id), {
  return self.__musicPlaying[id] ? 1 : 0;
});
// A score's events go to the page whole; it schedules them on its own clock.
EM_JS(void, _wasm_audio_score_new,
      (int id, const double* events, int count, double length, double loop_at), {
  postMessage({ type: "score", cmd: "new", id: id,
                events: HEAPF64.slice(events >> 3, (events >> 3) + count),
                length: length, loopAt: loop_at });
});
EM_JS(void, _wasm_audio_score, (int cmd, int id, double value), {
  const names = ["play", "stop", "free", "volume", "room"];
  postMessage({ type: "score", cmd: names[cmd], id: id, value: value });
});

inline bool available() { return true; }
inline void tone(int64_t start_freq, int64_t end_freq, int64_t attack,
                 int64_t decay, int64_t sustain, int64_t release, int64_t vol,
                 int64_t peak, int64_t channel, int64_t duty) {
  _wasm_audio_tone(static_cast<int>(start_freq), static_cast<int>(end_freq),
                   static_cast<int>(attack), static_cast<int>(decay),
                   static_cast<int>(sustain), static_cast<int>(release),
                   static_cast<int>(vol), static_cast<int>(peak),
                   static_cast<int>(channel), static_cast<int>(duty));
}
// `fmt` is for raylib's decoder dispatch; the page decodes the bytes itself.
inline void sound_load(int64_t id, const uint8_t* data, int64_t len,
                       const char* /*fmt*/) {
  _wasm_audio_sound_load(static_cast<int>(id), data, static_cast<int>(len));
}
inline void sound_play(int64_t id) { _wasm_audio_sound(0, static_cast<int>(id), 0); }
inline void sound_stop(int64_t id) { _wasm_audio_sound(1, static_cast<int>(id), 0); }
inline void sound_free(int64_t id) { _wasm_audio_sound(2, static_cast<int>(id), 0); }
inline bool sound_playing(int64_t id) {
  return _wasm_audio_sound_playing(static_cast<int>(id)) != 0;
}
inline void sound_volume(int64_t id, double x) { _wasm_audio_sound(3, static_cast<int>(id), x); }
inline void sound_pitch(int64_t id, double x) { _wasm_audio_sound(4, static_cast<int>(id), x); }
inline void sound_pan(int64_t id, double x) { _wasm_audio_sound(5, static_cast<int>(id), x); }

inline void music_load(int64_t id, const uint8_t* data, int64_t len,
                       const char* /*fmt*/, bool loop) {
  _wasm_audio_music_load(static_cast<int>(id), data, static_cast<int>(len),
                         loop ? 1 : 0);
}
inline void music_play(int64_t id) { _wasm_audio_music(0, static_cast<int>(id), 0); }
inline void music_stop(int64_t id) { _wasm_audio_music(1, static_cast<int>(id), 0); }
inline void music_pause(int64_t id) { _wasm_audio_music(2, static_cast<int>(id), 0); }
inline void music_resume(int64_t id) { _wasm_audio_music(3, static_cast<int>(id), 0); }
inline void music_free(int64_t id) { _wasm_audio_music(4, static_cast<int>(id), 0); }
inline bool music_playing(int64_t id) {
  return _wasm_audio_music_playing(static_cast<int>(id)) != 0;
}
inline void music_seek(int64_t id, double seconds) {
  _wasm_audio_music(5, static_cast<int>(id), seconds);
}
inline void music_volume(int64_t id, double x) { _wasm_audio_music(6, static_cast<int>(id), x); }
inline void music_pitch(int64_t id, double x) { _wasm_audio_music(7, static_cast<int>(id), x); }
inline void music_pan(int64_t id, double x) { _wasm_audio_music(8, static_cast<int>(id), x); }

inline void score_new(int64_t id, const double* events, int64_t count,
                      double length, double loop_at) {
  _wasm_audio_score_new(static_cast<int>(id), events, static_cast<int>(count),
                        length, loop_at);
}
inline void score_play(int64_t id) { _wasm_audio_score(0, static_cast<int>(id), 0); }
inline void score_stop(int64_t id) { _wasm_audio_score(1, static_cast<int>(id), 0); }
inline void score_free(int64_t id) { _wasm_audio_score(2, static_cast<int>(id), 0); }
inline void score_volume(int64_t id, double x) { _wasm_audio_score(3, static_cast<int>(id), x); }
inline void score_room(int64_t id, double x) { _wasm_audio_score(4, static_cast<int>(id), x); }

#elif defined(CULEBRA_AUDIO_DEVICE)

// Strong bodies in src/runtime/culebra_rt_audio.cc.
bool available();
void tone(int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay,
          int64_t sustain, int64_t release, int64_t vol, int64_t peak,
          int64_t channel, int64_t duty);
void sound_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt);
void sound_free(int64_t id);
void sound_play(int64_t id);
void sound_stop(int64_t id);
bool sound_playing(int64_t id);
void sound_volume(int64_t id, double v);
void sound_pitch(int64_t id, double p);
void sound_pan(int64_t id, double p);
void music_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt,
                bool loop);
void music_free(int64_t id);
void music_play(int64_t id);
void music_stop(int64_t id);
void music_pause(int64_t id);
void music_resume(int64_t id);
bool music_playing(int64_t id);
void music_seek(int64_t id, double seconds);
void music_volume(int64_t id, double v);
void music_pitch(int64_t id, double p);
void music_pan(int64_t id, double p);
void pcm_new(int64_t id, int64_t rate, int64_t channels, int64_t buffer);
void pcm_free(int64_t id);
bool pcm_ready(int64_t id);
int64_t pcm_needed(int64_t id);
int64_t pcm_push(int64_t id, const double* values, int64_t count);
int64_t pcm_submit(int64_t id);
double pcm_latency(int64_t id);
void pcm_play(int64_t id);
void pcm_stop(int64_t id);
void pcm_pause(int64_t id);
void pcm_resume(int64_t id);
bool pcm_playing(int64_t id);
void pcm_volume(int64_t id, double v);
void pcm_pitch(int64_t id, double p);
void pcm_pan(int64_t id, double p);
bool capture_present();
// `rate` and `channels` arrive checked (rt::pcm_count / pcm_channels).
void capture_new(int64_t id, int rate, int channels);
void capture_free(int64_t id);
bool capture_ready(int64_t id);
void capture_start(int64_t id);
void capture_stop(int64_t id);
bool capture_running(int64_t id);
int64_t capture_waiting(int64_t id);
// Up to `frames` waiting frames (all of them when negative), interleaved into
// `out`, which arrives empty.
void capture_read(int64_t id, int64_t frames, std::vector<float>& out);
void score_new(int64_t id, const double* events, int64_t count, double length,
               double loop_at);
void score_free(int64_t id);
void score_play(int64_t id);
void score_stop(int64_t id);
void score_volume(int64_t id, double v);
// How much of the room the score plays in is heard with it, 0 for none.
void score_room(int64_t id, double wet);
// A graph played: a copy the mixer works out from the next sample on, the
// changes scheduled after that to the copy's parts sent on to it (a part
// added since is the next play()'s). graph_now answers where on the
// graph's clock a change made now lands, NaN when it is not playing.
void graph_play(int64_t id);
void graph_stop(int64_t id);
void graph_post(int64_t id, int part, int value, ParamEvent e);
double graph_now(int64_t id);

#endif

// Silent bodies: inline where nothing else defines them, weak in the base AOT
// archive of a native build so the feature archive's strong ones win.
#if defined(CULEBRA_RT_AUDIO_WEAK)
#define CULEBRA_RT_AUDIO_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_AUDIO_LINKAGE inline
#endif

#if defined(CULEBRA_AUDIO_SILENT)
CULEBRA_RT_AUDIO_LINKAGE bool available() { return false; }
CULEBRA_RT_AUDIO_LINKAGE void tone(int64_t, int64_t, int64_t, int64_t, int64_t,
                                   int64_t, int64_t, int64_t, int64_t, int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_load(int64_t, const uint8_t*, int64_t,
                                         const char*) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool sound_playing(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void sound_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_pitch(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void sound_pan(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void music_load(int64_t, const uint8_t*, int64_t,
                                         const char*, bool) {}
CULEBRA_RT_AUDIO_LINKAGE void music_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void music_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void music_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void music_pause(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void music_resume(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool music_playing(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void music_seek(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void music_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void music_pitch(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void music_pan(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void score_new(int64_t, const double*, int64_t, double,
                                        double) {}
CULEBRA_RT_AUDIO_LINKAGE void score_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void score_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void score_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void score_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void score_room(int64_t, double) {}
#endif

#if !defined(CULEBRA_AUDIO_DEVICE)
// A PCM with nowhere to play: its block counts pushes and a submit empties
// it, as a native stream with no device does.
inline std::unordered_map<int64_t, rt::PcmBlock>& silent_streams() {
  static std::unordered_map<int64_t, rt::PcmBlock> streams;
  return streams;
}
inline rt::PcmBlock* silent_stream(int64_t id) {
  auto it = silent_streams().find(id);
  return it == silent_streams().end() ? nullptr : &it->second;
}
CULEBRA_RT_AUDIO_LINKAGE void pcm_new(int64_t id, int64_t rate,
                                         int64_t channels, int64_t buffer) {
  silent_streams().try_emplace(id, rate, channels, buffer);
}
CULEBRA_RT_AUDIO_LINKAGE void pcm_free(int64_t id) {
  silent_streams().erase(id);
}
CULEBRA_RT_AUDIO_LINKAGE bool pcm_ready(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE int64_t pcm_needed(int64_t) { return 0; }
CULEBRA_RT_AUDIO_LINKAGE int64_t pcm_push(int64_t id, const double* values,
                                             int64_t count) {
  auto* b = silent_stream(id);
  return b ? b->push_block(values, count) : 0;
}
CULEBRA_RT_AUDIO_LINKAGE int64_t pcm_submit(int64_t id) {
  if (auto* b = silent_stream(id)) b->discard();
  return 0;
}
CULEBRA_RT_AUDIO_LINKAGE double pcm_latency(int64_t id) {
  auto* b = silent_stream(id);
  return b ? b->latency() : 0.0;
}
CULEBRA_RT_AUDIO_LINKAGE void pcm_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void pcm_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void pcm_pause(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void pcm_resume(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool pcm_playing(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void pcm_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void pcm_pitch(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void pcm_pan(int64_t, double) {}
#endif

// Capture has no browser backend yet, so the browser is silent here too: no
// device, and a read finds nothing.
#if !defined(CULEBRA_AUDIO_DEVICE)
CULEBRA_RT_AUDIO_LINKAGE bool capture_present() { return false; }
CULEBRA_RT_AUDIO_LINKAGE void capture_new(int64_t, int, int) {}
CULEBRA_RT_AUDIO_LINKAGE void capture_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool capture_ready(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void capture_start(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void capture_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool capture_running(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE int64_t capture_waiting(int64_t) { return 0; }
CULEBRA_RT_AUDIO_LINKAGE void capture_read(int64_t, int64_t,
                                           std::vector<float>&) {}
#endif

// A graph renders everywhere, and plays on a device alone: the browser has no
// backend for one yet.
#if !defined(CULEBRA_AUDIO_DEVICE)
CULEBRA_RT_AUDIO_LINKAGE void graph_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void graph_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void graph_post(int64_t, int, int, ParamEvent) {}
CULEBRA_RT_AUDIO_LINKAGE double graph_now(int64_t) { return std::nan(""); }
#endif

#undef CULEBRA_RT_AUDIO_LINKAGE

}  // namespace _audio_detail
}  // namespace culebra
