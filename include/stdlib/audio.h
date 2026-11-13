#pragma once

// The Audio namespace's backend choke: the one owner of the sound device and
// of everything that plays through it (tone, Sound, Music, PCM, and the
// schedules Kauai plays) or listens on the microphone (Capture). The
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
// hears nothing. What needs raudio's code but no device (schedule_render,
// decode) answers there, and in the browser, that it is not available.

#include <stdlib/pcm_block.h>

#include <algorithm>
#include <cassert>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
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

// Samples (-1..1, `rate` a second, `frames` of each channel, the channels
// interleaved) as the 16-bit WAV file a program would load, so a Sound made
// from samples decodes as one from bytes does.
inline std::vector<uint8_t> wav16(const std::vector<const double*>& channels, size_t frames, uint32_t rate) {
  const auto count = static_cast<uint32_t>(channels.size());
  std::vector<uint8_t> out;
  out.reserve(44 + 2 * count * frames);
  auto put = [&](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto data = static_cast<uint32_t>(2 * count * frames);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put(36 + data, 4);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(16, 4);
  put(1, 2);  // PCM
  put(count, 2);
  put(rate, 4);
  put(rate * 2 * count, 4);
  put(2 * count, 2);
  put(16, 2);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put(data, 4);
  for (size_t i = 0; i < frames; i++) {
    for (const double* x : channels) {
      double v = std::isnan(x[i]) ? 0.0 : std::clamp(x[i], -1.0, 1.0);
      put(static_cast<uint16_t>(static_cast<int16_t>(std::nearbyint(v * 32767))), 2);
    }
  }
  return out;
}

// --- How loud a sound is, the same on every backend ---
//
// ITU-R BS.1770's momentary loudness, its largest: the samples K-weighted (a
// shelf of about +4 dB above 1.7 kHz for the head, a high-pass at 38 Hz),
// their mean square over 400 ms stepped by 100 ms (a sound shorter than that
// heard over 400 ms, silence after it), in LUFS. NaN under BS.1770's absolute
// gate of -70 LUFS, which is silence. A Kauai song weighs an instrument it is
// handed against its own tones with it. The filters are BS.1770's pair made
// for the sound's own rate as libebur128 makes them (at 48 kHz they are the
// standard's own coefficients).

struct LoudnessBiquad {
  double b0, b1, b2, a1, a2;
  double z1 = 0, z2 = 0;
  double step(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

inline LoudnessBiquad k_shelf(double rate) {
  double q = 0.7071752369554196;
  double k = std::tan(std::numbers::pi * 1681.974450955533 / rate);
  double vh = std::pow(10.0, 3.999843853973347 / 20.0);
  double vb = std::pow(vh, 0.4996667741545416);
  double a0 = 1.0 + k / q + k * k;
  return {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
          2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

inline LoudnessBiquad k_highpass(double rate) {
  double q = 0.5003270373238773;
  double k = std::tan(std::numbers::pi * 38.13547087602444 / rate);
  double a0 = 1.0 + k / q + k * k;
  return {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

// The running sum of the K-weighted signal's squares, for any window's power.
inline std::vector<double> k_weighted_sums(const double* x, size_t n, double rate) {
  auto shelf = k_shelf(rate);
  auto high = k_highpass(rate);
  std::vector<double> sum(n + 1, 0.0);
  for (size_t i = 0; i < n; i++) {
    double v = std::isnan(x[i]) ? 0.0 : std::clamp(x[i], -1.0, 1.0);
    double y = high.step(shelf.step(v));
    sum[i + 1] = sum[i] + y * y;
  }
  return sum;
}

inline double lufs_of(double power) { return power > 0 ? -0.691 + 10.0 * std::log10(power) : -INFINITY; }

inline double loudness(const double* x, size_t n, double rate) {
  auto sum = k_weighted_sums(x, n, rate);
  auto window = static_cast<size_t>(std::llround(0.4 * rate));
  auto hop = std::max<size_t>(1, static_cast<size_t>(std::llround(0.1 * rate)));
  double most = 0.0;
  if (n <= window) {
    most = sum[n] / static_cast<double>(window);
  } else {
    for (size_t s = 0;; s += hop) {
      size_t e = std::min(n, s + window);
      most = std::max(most, (sum[e] - sum[e - window]) / static_cast<double>(window));
      if (e == n) break;
    }
  }
  double lufs = lufs_of(most);
  return lufs < -70.0 ? std::nan("") : lufs;
}

// BS.1770's integrated loudness: the mean power of the 400 ms blocks stepped
// by 100 ms that lie wholly in the samples (one over 400 ms, silence after,
// for a sound shorter than that), leaving out the blocks under the absolute
// gate of -70 LUFS and then those 10 LU under the mean of the rest. NaN when
// no block is left, which is silence.
inline double integrated_loudness(const double* x, size_t n, double rate) {
  auto sum = k_weighted_sums(x, n, rate);
  auto window = static_cast<size_t>(std::llround(0.4 * rate));
  auto hop = std::max<size_t>(1, static_cast<size_t>(std::llround(0.1 * rate)));
  std::vector<double> blocks;
  if (n <= window) {
    blocks.push_back(sum[n] / static_cast<double>(window));
  } else {
    for (size_t s = 0; s + window <= n; s += hop) {
      blocks.push_back((sum[s + window] - sum[s]) / static_cast<double>(window));
    }
  }
  auto mean_above = [&](double gate) {
    double total = 0.0;
    size_t count = 0;
    for (double z : blocks) {
      if (lufs_of(z) > gate) {
        total += z;
        count++;
      }
    }
    return count == 0 ? 0.0 : total / static_cast<double>(count);
  };
  double ungated = mean_above(-70.0);
  if (ungated == 0.0) return std::nan("");
  return lufs_of(mean_above(std::max(-70.0, lufs_of(ungated) - 10.0)));
}

// The samples of a WAV file as mono -1..1 and its rate: PCM of 8, 16, 24 or
// 32 bits or floats of 32 or 64, its channels averaged. False for one it
// does not read, whose loudness is then not known.
inline bool wav_samples(const uint8_t* p, size_t n, std::vector<double>& out, double& rate) {
  auto u16 = [&](size_t at) { return static_cast<uint32_t>(p[at] | p[at + 1] << 8); };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  if (n < 12 || std::string_view(reinterpret_cast<const char*>(p), 4) != "RIFF" ||
      std::string_view(reinterpret_cast<const char*>(p + 8), 4) != "WAVE")
    return false;
  uint32_t format = 0, channels = 0, bits = 0;
  for (size_t at = 12; at + 8 <= n;) {
    std::string_view id(reinterpret_cast<const char*>(p + at), 4);
    size_t len = u32(at + 4), body = at + 8;
    if (len > n - body) return false;
    if (id == "fmt " && len >= 16) {
      format = u16(body);
      channels = u16(body + 2);
      rate = u32(body + 4);
      bits = u16(body + 14);
      if (format == 0xfffe && len >= 26) format = u16(body + 24);  // extensible
    } else if (id == "data") {
      size_t width = bits / 8;
      bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
      bool floats = format == 3 && (bits == 32 || bits == 64);
      if ((!pcm && !floats) || channels == 0 || rate <= 0) return false;
      size_t frames = len / (width * channels);
      out.assign(frames, 0.0);
      for (size_t f = 0; f < frames; f++) {
        double v = 0;
        for (size_t c = 0; c < channels; c++) {
          const uint8_t* s = p + body + (f * channels + c) * width;
          if (floats) {
            if (bits == 32) {
              float x;
              std::memcpy(&x, s, 4);
              v += x;
            } else {
              double x;
              std::memcpy(&x, s, 8);
              v += x;
            }
          } else if (bits == 8) {
            v += (s[0] - 128) / 128.0;
          } else {
            uint32_t raw = 0;
            for (size_t b = 0; b < width; b++) raw |= static_cast<uint32_t>(s[b]) << (8 * b);
            int shift = 32 - static_cast<int>(bits);
            v += static_cast<int32_t>(raw << shift) / 2147483648.0;
          }
        }
        out[f] = v / channels;
      }
      return true;
    }
    at = body + len + (len & 1);
  }
  return false;
}

// What each Sound weighs, by handle: kept as it is made, as the browser
// decodes a file's bytes only later, and dropped as it is freed.
inline std::unordered_map<int64_t, double>& sound_loudness_table() {
  static std::unordered_map<int64_t, double> table;
  return table;
}

inline constexpr auto kSamplesError =
    "type error: parameter 'samples' expects an Array of Long|Float";
inline constexpr auto kF0Error =
    "type error: parameter 'f0' expects an Array of Long|Float|Nil";
inline constexpr auto kScheduleEventsError =
    "type error: parameter 'events' expects an Array of Long|Float";

// Sound, Music and PCM handles: one counter for every backend, so a
// handle's lifecycle reads the same whether or not a host can play it.
inline int64_t alloc_id() {
  static int64_t n = 0;
  return ++n;
}

// The rate the mixer runs at, which a graph played must have.
inline constexpr double kMixerRate = 44100;

// A tone at vol 100 is a raw wave at a fifth of full scale, as up to four
// channels stack (the browser's app.js keeps the same 0.2).
inline constexpr double kToneLevel = 0.2;

// How loud a tone of `freq` Hz is at vol 100 on a pulse at a half's duty, in
// LUFS: a second of the mixer's own square. A Kauai song plays an external
// sound at vol N as loud as this tone at vol N.
inline double tone_loudness(double freq) {
  auto n = static_cast<size_t>(kMixerRate);
  std::vector<double> x(n);
  double phase = 0;
  for (size_t i = 0; i < n; i++) {
    x[i] = phase < 0.5 ? kToneLevel : -kToneLevel;
    phase += freq / kMixerRate;
    if (phase >= 1.0) phase -= std::floor(phase);
  }
  return loudness(x.data(), n, kMixerRate);
}

// The schedule: all the engine knows of a song Kauai plays, as a flat
// list of numbers in rows of kScheduleStride, one row for each sound that
// starts. A row is its time in seconds, then a tone's channel, start and end
// frequency, attack, decay, sustain and release in seconds, vol, peak and
// duty; or -1, a Sound's handle and gain, and zeros to fill the row. The rows
// come in time order, since the mixers read them from the head; the binding
// refuses a list out of it or not a whole number of rows.
//
// The width is defined here alone. Kauai asks the binding for it
// (_Audio.schedule_stride) and checks each row it builds against it, and the
// page is sent it with the list. The entry points, all on _Audio:
// schedule_new(list, length, loop_at) -> id, schedule_play/stop/free(id),
// schedule_volume/room(id, x), schedule_render(...) -> samples, schedule_stride().
//
// A song can also start before its Sounds are all made: schedule_open(list,
// slots, length, loop_at) takes a list whose Sound rows name a slot, 0 to
// slots - 1, in place of a handle; schedule_fill(id, slot, sound) gives a
// slot its Sound as it comes; the mixer holds the song where it reaches a
// slot still empty, and schedule_held(id) answers for how long, in seconds,
// since it last started. Only a native mixer holds a song (schedule_open
// answers false elsewhere, and the song is made whole with schedule_new).
inline constexpr int64_t kScheduleStride = 11;

// A Sound a schedule plays, by the bytes it was made from, for schedule_render: the
// handle the schedule's list names it by, and its format (sound_format's).
struct SoundBytes {
  int64_t handle = 0;
  const uint8_t* data = nullptr;
  int64_t len = 0;
  const char* fmt = nullptr;
};

// A recording as decode() reads it: its rate, its channels, and its frames
// with their channels interleaved, -1..1.
struct Recording {
  int rate = 0;
  int channels = 0;
  std::vector<float> frames;
};
enum class DecodeResult { Decoded, Undecodable, Unavailable };

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
// A value that is not a number, or infinite, is let go too: in a state that
// feeds back it would stay, and the part would never sound again.
inline double settled(double x) {
  const double a = std::abs(x);
  return a >= 1e-30 && a <= std::numeric_limits<double>::max() ? x : 0.0;
}

// The slot after `i` in a ring of `size`, without a division.
template <class I>
constexpr I ring_next(I i, I size) { return i + 1 == size ? 0 : i + 1; }

// A change scheduled for a value, `at` seconds in: from then on it is
// `value`, or (`ramp`) it arrives there in a straight line from the change
// before. Web Audio's setValueAtTime and linearRampToValueAtTime. A change
// takes effect at the first sample whose time, its index over the rate, is
// `at` or later. One that `cancel`s first drops the changes after `at` still
// to come (Param::held makes it).
struct ParamEvent {
  double at = 0;
  bool ramp = false;
  double value = 0;
  bool cancel = false;
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

  // After the changes already scheduled for the same time; one that cancels
  // takes the place of those after it still to come.
  void schedule(ParamEvent e) {
    auto it = std::upper_bound(events_.begin() + static_cast<std::ptrdiff_t>(next_), events_.end(),
                               e.at, [](double at, const ParamEvent& x) { return at < x.at; });
    if (e.cancel) {
      it = events_.erase(it, events_.end());
      e.cancel = false;
    }
    events_.insert(it, e);
  }
  // The change that cancels those after `at`, holding the value there: a
  // ramp to it when one was under way at `at`, as Web Audio's
  // cancelAndHoldAtTime keeps the line, and a set otherwise. Read on a value
  // that has every change it was given, a graph's definition.
  ParamEvent held(double at) const {
    auto next = std::upper_bound(events_.begin(), events_.end(), at,
                                 [](double t, const ParamEvent& x) { return t < x.at; });
    double from_at = 0, from = rest_;
    if (next != events_.begin()) {
      from_at = std::prev(next)->at;
      from = std::prev(next)->value;
    }
    if (next != events_.end() && next->ramp)
      return {at, true, from + (next->value - from) * (at - from_at) / (next->at - from_at), true};
    return {at, false, from, true};
  }
  size_t size() const { return events_.size(); }
  // Room for `extra` changes more without allocating; answers how many it
  // can hold.
  size_t make_room(size_t extra) {
    events_.reserve(events_.size() + extra);
    return events_.capacity();
  }
  // The changes still to come moved into `longer` (empty, with room for them
  // and more), which then stands in for this one's list; `longer` is left
  // holding the old list, for its owner to free. Never allocates.
  void take_list(std::vector<ParamEvent>& longer) {
    for (size_t k = next_; k < events_.size(); k++) longer.push_back(events_[k]);
    events_.swap(longer);
    longer.clear();
    next_ = 0;
  }
  // schedule() that never allocates: false when there is no room for it.
  bool schedule_in_place(ParamEvent e) {
    bool frees = e.cancel && next_ < events_.size() && events_.back().at > e.at;
    if (events_.size() == events_.capacity() && !frees) return false;
    schedule(e);
    return true;
  }
  // at() for each of `n` times that only go forward, into `out`. When the next
  // change comes after the last of them, the value holds through them (a set)
  // or moves on one line (a ramp), with nothing to check between.
  void fill(const double* times, int n, double* out) {
    if (next_ == events_.size() || (!events_[next_].ramp && events_[next_].at > times[n - 1])) {
      std::fill(out, out + n, value_);
      return;
    }
    const ParamEvent& e = events_[next_];
    if (e.at > times[n - 1]) {
      for (int i = 0; i < n; i++)
        out[i] = from_value_ + (e.value - from_value_) * (times[i] - from_at_) / (e.at - from_at_);
      return;
    }
    for (int i = 0; i < n; i++) out[i] = at(times[i]);
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
    int64_t back = w_ - len_;
    double read = buf_[static_cast<size_t>(back < 0 ? back + size : back)];
    double v = 0.5 * (read + prev_) * loss_;
    prev_ = read;
    double ap = settled(c_ * v + ap_in_ - c_ * ap_out_);
    ap_in_ = v;
    ap_out_ = ap;
    buf_[static_cast<size_t>(w_)] = ap + kick;
    w_ = ring_next(w_, size);
    dc_out_ = settled(read - dc_in_ + pole_ * dc_out_);
    dc_in_ = read;
    return dc_out_;
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

// The streams of the graph parts that draw numbers, so that parts of two
// kinds given one seed draw different numbers (a noise draws from its seed
// itself).
inline constexpr uint64_t kDustStream = 1;
inline constexpr uint64_t kLfNoiseStream = 2;

// A Noise for `stream` from `seed`: the seed and the stream mixed
// (SplitMix64's finalizer), so nearby seeds start far apart.
inline Noise noise_stream(int64_t seed, uint64_t stream) {
  uint64_t z = static_cast<uint64_t>(seed) + stream * 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return Noise{static_cast<int64_t>((z ^ (z >> 31)) % 2147483648ull)};
}

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

// How many harmonics of `f` are at most 0.45 of the rate: those a harmonic
// tone sounds and those its pitch is read and measured from.
inline double harmonics_under(double f, double rate) { return std::floor(0.45 * rate / f); }

// `n` samples of a harmonic tone: its frequency and gain, its spectrum moving
// in straight lines between the amps given at their times (the k-th of them
// at k times the frequency, those above 0.45 of the rate left out), and
// band noise heard steadily and pulsing with the tone's wave (0.5 + 0.5 sin
// of its phase), as air through a reed is.
inline void partials(int64_t n, Param freq, Param gain, const PartialsSpectrum& sp,
                     PartialsNoise noise, double rate, double* out) {
  int64_t widest = 0;
  for (int64_t c : sp.count) widest = std::max(widest, c);
  std::vector<double> amps(static_cast<size_t>(widest), 0.0);
  Noise white{noise.seed};
  double fast = 0, slow = 0, phase = 0;
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
    double below = harmonics_under(f, rate);
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

// --- Measuring: a sound's spectrum, its pitch, its samples at another rate --

// An iterative radix-2 FFT of `size` (a power of 2, 2 or more) points, its
// input order and twiddles worked out once for every frame: the caller puts
// point i at a[order(i)], and transform() leaves X_k at a[k].
class Fft {
 public:
  explicit Fft(size_t size) : size_(size), order_(size), twiddle_(size / 2) {
    const double tau = 2.0 * std::numbers::pi, n = static_cast<double>(size);
    const int bits = std::countr_zero(size);
    for (size_t i = 0; i < size; i++)
      order_[i] = (order_[i >> 1] >> 1) | ((i & 1) << (bits - 1));  // i's bits reversed
    for (size_t k = 0; k < size / 2; k++)
      twiddle_[k] = std::polar(1.0, -tau * static_cast<double>(k) / n);
  }
  size_t size() const { return size_; }
  size_t order(size_t i) const { return order_[i]; }
  void transform(std::complex<double>* a) const {
    for (size_t len = 2; len <= size_; len *= 2) {
      const size_t half = len / 2, step = size_ / len;
      for (size_t s = 0; s < size_; s += len)
        for (size_t j = 0; j < half; j++) {
          auto t = twiddle_[j * step] * a[s + j + half];
          a[s + j + half] = a[s + j] - t;
          a[s + j] += t;
        }
    }
  }

 private:
  size_t size_;
  std::vector<size_t> order_;
  std::vector<std::complex<double>> twiddle_;
};

// The spectrum of a frame of `size` (a power of 2) samples: through a
// periodic Hann window, then the magnitude |X_k| of bins 0 to size/2, as
// numpy's rfft gives it (bin k is k * rate / size Hz).
class Spectrum {
 public:
  explicit Spectrum(size_t size) : fft_(size), window_(size), a_(size) {
    const double tau = 2.0 * std::numbers::pi, n = static_cast<double>(size);
    for (size_t i = 0; i < size; i++) window_[i] = 0.5 - 0.5 * std::cos(tau * static_cast<double>(i) / n);
  }
  // Frame t starts at sample t * hop, so `n` samples are ceil(n / hop) frames.
  static size_t frames(size_t n, size_t hop) { return n == 0 ? 0 : (n - 1) / hop + 1; }
  size_t bins() const { return fft_.size() / 2 + 1; }
  // The frame of `x` (`n` long) from `start`, those past the end taken as 0,
  // into `out` (bins() long).
  void frame(const double* x, size_t n, size_t start, double* out) {
    transform(x, n, start);
    for (size_t k = 0; k < bins(); k++) out[k] = std::sqrt(std::norm(a_[k]));
  }
  // The same frame's bins as they are, real and imaginary parts.
  void complex_frame(const double* x, size_t n, size_t start, double* re, double* im) {
    transform(x, n, start);
    for (size_t k = 0; k < bins(); k++) re[k] = a_[k].real(), im[k] = a_[k].imag();
  }
  // The samples a frame's bins (`re`, `im`, bins() each) stand for, through
  // the window again, added into `out` from `start`, and the window's square
  // into `weight` (numpy's irfft: the 0th and the last bin's imaginary parts
  // are not read).
  void add_inverse(const double* re, const double* im, size_t start, double* out, double* weight) {
    const size_t n = fft_.size(), last = bins() - 1;
    for (size_t k = 0; k < n; k++) {
      std::complex<double> v;
      if (k == 0 || k == last) v = re[k];
      else if (k < last) v = {re[k], im[k]};
      else v = {re[n - k], -im[n - k]};
      a_[fft_.order(k)] = std::conj(v);  // the inverse as the conjugate of the transform of the conjugate
    }
    fft_.transform(a_.data());
    for (size_t i = 0; i < n; i++) {
      out[start + i] += a_[i].real() / static_cast<double>(n) * window_[i];
      weight[start + i] += window_[i] * window_[i];
    }
  }

 private:
  void transform(const double* x, size_t n, size_t start) {
    for (size_t i = 0; i < fft_.size(); i++)
      a_[fft_.order(i)] = start + i < n ? x[start + i] * window_[i] : 0.0;
    fft_.transform(a_.data());
  }

  Fft fft_;
  std::vector<double> window_;
  std::vector<std::complex<double>> a_;
};

// The samples `frames` frames of bins stand for (`re` and `im` a frame's
// bins after another, as Spectrum::complex_frame gives them, frame t from
// t * hop): each frame's inverse through the window, added together and
// divided by the window's squares there, so that the frames of a sound give
// that sound back. (frames - 1) * hop + size samples; 0 where no window
// reaches.
inline std::vector<double> inverse_spectrum(const std::vector<double>& re, const std::vector<double>& im,
                                            size_t frames, size_t size, size_t hop) {
  Spectrum spectrum(size);
  const size_t bins = spectrum.bins();
  const size_t n = frames == 0 ? 0 : (frames - 1) * hop + size;
  std::vector<double> out(n, 0.0), weight(n, 0.0);
  for (size_t t = 0; t < frames; t++)
    spectrum.add_inverse(re.data() + t * bins, im.data() + t * bins, t * hop, out.data(), weight.data());
  for (size_t i = 0; i < n; i++) out[i] = weight[i] > 1e-30 ? out[i] / weight[i] : 0.0;
  return out;
}

// I0, the Kaiser window's Bessel function, by its series (std::cyl_bessel_i
// is not in every standard library).
inline double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; term > sum * 1e-17; k++) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
  }
  return sum;
}

// One side of the band-limited interpolation filter resample() reads: a
// sinc rolled off below the band's edge under a Kaiser window, 64 zero
// crossings long at 512 points each, the shape of resampy's kaiser_best.
struct ResampleFilter {
  static constexpr size_t kZeros = 64;
  static constexpr size_t kPerZero = 512;
  std::vector<double> win;  // its kZeros * kPerZero + 1 points, then a 0
  ResampleFilter() {
    constexpr double rolloff = 0.9475937167399596, beta = 14.769656459379492;
    const size_t n = kZeros * kPerZero;
    const double pi = std::numbers::pi, edge = bessel_i0(beta);
    win.assign(n + 2, 0.0);
    for (size_t i = 0; i <= n; i++) {
      double x = rolloff * static_cast<double>(i) / kPerZero;
      double sinc = i == 0 ? 1.0 : std::sin(pi * x) / (pi * x);
      double r = static_cast<double>(i) / static_cast<double>(n);
      win[i] = rolloff * sinc * bessel_i0(beta * std::sqrt(1.0 - r * r)) / edge;
    }
  }
  // The filter `pos` points from its middle (0 to its last point), between
  // the two points about it.
  double at(double pos) const {
    auto w = static_cast<int64_t>(pos);
    return win[w] + (pos - static_cast<double>(w)) * (win[w + 1] - win[w]);
  }
};

// `x` (`n` long), sampled `from` times a second, as it is sampled `to` times
// a second into `out`: ceil(n * to / from) samples, each the filter's sum over
// the samples it reaches about its time (Smith's band-limited interpolation),
// the band cut below the lower rate's half. The filter is read at each
// sample's exact distance: rounding the step to its points, as resampy does,
// leaves an error of -70 dB from 48000 to 44100. The same samples when the
// rates are equal.
inline void resample(const double* x, size_t n, double from, double to, std::vector<double>& out) {
  if (from == to) {
    out.assign(x, x + n);
    return;
  }
  static const ResampleFilter filter;
  const double ratio = to / from, scale = std::min(1.0, ratio);
  const double stride = scale * ResampleFilter::kPerZero;  // filter points a sample apart
  const double reach = ResampleFilter::kZeros / scale;     // samples it reaches either side
  const size_t m = n == 0 ? 0 : static_cast<size_t>(std::ceil(static_cast<double>(n) * ratio - 1e-9));
  out.assign(m, 0.0);
  for (size_t t = 0; t < m; t++) {
    const double time = static_cast<double>(t) / ratio;
    const auto first = static_cast<int64_t>(std::max(0.0, std::ceil(time - reach)));
    const auto last = static_cast<int64_t>(std::min(static_cast<double>(n - 1), std::floor(time + reach)));
    double sum = 0.0;
    for (int64_t j = first; j <= last; j++)
      sum += filter.at(std::abs(time - static_cast<double>(j)) * stride) * x[j];
    out[t] = scale * sum;
  }
}

// The sample a measure every 10 ms has frame t about (t may run past either
// end), so that Audio.pitch's frames and Audio.harmonics' line up.
inline int64_t frame_centre(int64_t t, double rate) { return std::llround(static_cast<double>(t) * rate / 100.0); }

// Sample j of `x` (`n` long), 0 outside it or where it is not a number.
inline double sample_at(const double* x, size_t n, int64_t j) {
  if (j < 0 || j >= static_cast<int64_t>(n)) return 0.0;
  return std::isfinite(x[j]) ? x[j] : 0.0;
}

// The pitch every 10 ms, read as pYIN reads it but with the candidates scored
// by their harmonics as SWIPE' scores a pitch. YIN alone takes two, three or
// four periods for one where a tone is weak or its attack rough, and half a
// period where its even harmonics are strong; so each frame lists candidates
// (YIN's own pick, that pick times and divided by 2, 3 and 4, and each other
// dip of YIN's normalized difference under 0.5) and scores each against the
// frame's spectrum. Along each run of frames with candidates, a gap under 6
// frames not ending it, one is chosen for each frame (Viterbi) against the
// cost of each jump between frames, so a note held through a weak frame keeps
// its pitch. The one chosen is then read finely from its harmonics' peaks,
// through a window as long as YIN's: a longer one reads a slur's
// neighbouring note into the frames about it.
struct PitchTrack {
  std::vector<double> hz;       // NaN where a frame has no pitch
  std::vector<double> clarity;  // 0 to 1
};

class PitchReader {
 public:
  // A frame's candidates, each with its cost and its pitch read finely (none
  // where the frame has no pitch), and how periodic the frame is: 1 less
  // YIN's normalized difference at the period it picks, 0 where silent.
  struct Frame {
    std::vector<double> cands, costs, fine;
    double clarity = 0.0;
  };

  // YIN reads n_ samples, the smallest power of 2 holding 3 periods of `low`.
  // The scores read the harmonics through a Hann window twice that, the fine
  // reading through one as long; both padded to 4 n_.
  PitchReader(double rate, double low, double high)
      : rate_(rate),
        low_(low),
        high_(high),
        lo_(static_cast<size_t>(std::floor(rate / high))),
        hi_(static_cast<size_t>(std::ceil(rate / low))),
        n_(std::bit_ceil(static_cast<size_t>(std::ceil(3.0 * rate / low)))),
        yin_(2 * n_),
        spec_(4 * n_),
        frame_(n_),
        energy_(n_ + 1),
        d_(hi_ + 2),
        cmnd_(hi_ + 2),
        power_(2 * n_),
        a_(2 * n_),
        long_(hann(2 * n_)),
        short_(hann(n_)),
        b_(4 * n_),
        mag_(2 * n_ + 1),
        bin_hz_(rate / static_cast<double>(4 * n_)) {}

  // The frame of `x` (`n` long) about sample `c`, those outside it taken as 0.
  Frame read(const double* x, size_t n, int64_t c) {
    Frame f;
    if (!yin(x, n, c)) return f;
    // YIN's pick: the first dip under 0.12, else the least, if under 0.3.
    size_t best = lo_;
    bool dipped = false;
    for (size_t t = lo_; t <= hi_ && !dipped; t++) {
      if (cmnd_[t] < 0.12) {
        best = t;
        while (best < hi_ && cmnd_[best + 1] < cmnd_[best]) best++;
        dipped = true;
      }
    }
    if (!dipped)
      for (size_t t = lo_; t <= hi_; t++)
        if (cmnd_[t] < cmnd_[best]) best = t;
    f.clarity = std::clamp(1.0 - cmnd_[best], 0.0, 1.0);
    if (cmnd_[best] > 0.3) return f;
    const double pick = dip_hz(best);
    std::vector<double> wanted{pick};
    for (double m : {2.0, 3.0, 4.0}) {
      wanted.push_back(pick * m);
      wanted.push_back(pick / m);
    }
    for (size_t t = lo_; t <= hi_; t++)
      if (t != best && cmnd_[t] < 0.5 && cmnd_[t] < cmnd_[t - 1] && cmnd_[t] <= cmnd_[t + 1])
        wanted.push_back(dip_hz(t));
    for (double hz : wanted) {
      if (hz < low_ || hz > high_) continue;
      if (std::any_of(f.cands.begin(), f.cands.end(), [&](double g) { return cents_between(hz, g) <= 30.0; }))
        continue;
      f.cands.push_back(hz);
    }
    if (f.cands.empty()) return f;
    spectrum(x, n, c, long_);
    for (double hz : f.cands) f.costs.push_back(1.0 - score(hz));
    spectrum(x, n, c, short_);
    for (double hz : f.cands) f.fine.push_back(fine(hz));
    return f;
  }

  static double cents_between(double f, double g) { return std::abs(1200.0 * std::log2(f / g)); }

 private:
  // The highest frequency a candidate is scored to.
  static constexpr double kScoreTop = 5000.0;

  // YIN's cumulative-mean-normalized difference of the n_ samples about `c`
  // into cmnd_, and the raw difference into d_ (the frame's overlap with
  // itself shrinking as the lag grows); false where they are near silent, a
  // mean square under 1e-6 (-60 dBFS).
  bool yin(const double* x, size_t n, int64_t c) {
    const int64_t start = c - static_cast<int64_t>(n_ / 2);
    energy_[0] = 0.0;
    for (size_t i = 0; i < n_; i++) {
      frame_[i] = sample_at(x, n, start + static_cast<int64_t>(i));
      energy_[i + 1] = energy_[i] + frame_[i] * frame_[i];
    }
    if (energy_[n_] / static_cast<double>(n_) < 1e-6) return false;
    // The autocorrelation, the transform of the padded frame's power spectrum.
    const size_t m = 2 * n_;
    for (size_t i = 0; i < m; i++) a_[yin_.order(i)] = i < n_ ? frame_[i] : 0.0;
    yin_.transform(a_.data());
    for (size_t k = 0; k < m; k++) power_[k] = std::norm(a_[k]);
    for (size_t k = 0; k < m; k++) a_[yin_.order(k)] = power_[k];
    yin_.transform(a_.data());
    double running = 0.0;
    cmnd_[0] = 1.0;
    for (size_t t = 0; t <= hi_ + 1; t++) {
      d_[t] = energy_[n_ - t] + (energy_[n_] - energy_[t]) - 2.0 * a_[t].real() / static_cast<double>(m);
      if (t == 0) continue;
      running += d_[t];
      cmnd_[t] = running > 0 ? d_[t] * static_cast<double>(t) / running : 1.0;
    }
    return true;
  }

  // The frequency of the dip at lag `t`, between lags by a parabola.
  double dip_hz(size_t t) const {
    const double a = d_[t - 1], b = d_[t], c = d_[t + 1], den = a - 2.0 * b + c;
    const double shift = den != 0.0 ? 0.5 * (a - c) / den : 0.0;
    return rate_ / (static_cast<double>(t) + std::clamp(shift, -1.0, 1.0));
  }

  static std::vector<double> hann(size_t size) {
    std::vector<double> w(size);
    const double tau = 2.0 * std::numbers::pi, n = static_cast<double>(size);
    for (size_t i = 0; i < size; i++) w[i] = 0.5 - 0.5 * std::cos(tau * static_cast<double>(i) / n);
    return w;
  }

  // The magnitudes of the frame about `c` through `window` into mag_.
  void spectrum(const double* x, size_t n, int64_t c, const std::vector<double>& window) {
    const size_t w = window.size();
    const int64_t start = c - static_cast<int64_t>(w / 2);
    for (size_t i = 0; i < 4 * n_; i++)
      b_[spec_.order(i)] = i < w ? sample_at(x, n, start + static_cast<int64_t>(i)) * window[i] : 0.0;
    spec_.transform(b_.data());
    for (size_t k = 0; k < mag_.size(); k++) mag_[k] = std::sqrt(std::norm(b_[k]));
  }

  // How well the frame's magnitudes match a kernel of the candidate's 1st and
  // prime harmonics, as SWIPE' (Camacho and Harris, 2008) scores a pitch:
  // each a cosine's lobe, positive at the harmonic and negative half a
  // harmonic either side, weighed by 1 over the root of its frequency, from a
  // quarter of `low` to 5 kHz; their normalized inner product, -1 to 1. A
  // reading at 1/m of the pitch finds the pitch's harmonics at its own 2nd,
  // 4th, 6th, which the primes past the 2nd leave out; one at 2/5 of it finds
  // the pitch half a harmonic off its own 2nd and 3rd; one at twice it finds
  // the pitch half a harmonic under its own 1st. (SWIPE' scores the
  // magnitudes' square roots; the magnitudes themselves misread fewer of a
  // soprano saxophone's notes.)
  double score(double hz) {
    const auto lo = std::max<size_t>(1, static_cast<size_t>(std::ceil(0.25 * low_ / bin_hz_)));
    const auto hi = std::min(mag_.size() - 1, static_cast<size_t>(std::floor(kScoreTop / bin_hz_)));
    if (lo > hi) return 0.0;
    kernel_.assign(hi - lo + 1, 0.0);
    for (int p : kPrimes) {
      if ((p - 0.75) * hz >= kScoreTop) break;
      const auto a = std::max(lo, static_cast<size_t>(std::ceil((p - 0.75) * hz / bin_hz_)));
      const auto b = std::min(hi, static_cast<size_t>(std::floor((p + 0.75) * hz / bin_hz_)));
      for (size_t u = a; u <= b; u++) {
        const double freq = static_cast<double>(u) * bin_hz_;
        kernel_[u - lo] += std::cos(2.0 * std::numbers::pi * (freq / hz - p)) / std::sqrt(freq);
      }
    }
    double dot = 0.0, kk = 0.0, ss = 0.0;
    for (size_t u = lo; u <= hi; u++) {
      const double k = kernel_[u - lo];
      dot += k * mag_[u];
      kk += k * k;
      ss += mag_[u] * mag_[u];
    }
    return kk > 0.0 && ss > 0.0 ? dot / std::sqrt(kk * ss) : 0.0;
  }

  // 1 and the primes, enough for a 5 kHz kernel over 20 Hz.
  static constexpr int kPrimes[] = {1,   2,   3,   5,   7,   11,  13,  17,  19,  23,  29,  31,  37,  41,
                                    43,  47,  53,  59,  61,  67,  71,  73,  79,  83,  89,  97,  101, 103,
                                    107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179,
                                    181, 191, 193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251};

  // A candidate read finely: first where its first eight harmonics are
  // strongest within 60 cents (in steps of 5), then from where its
  // harmonics' peaks are (each found within a quarter of the spacing and
  // placed between bins by a parabola through the log), each one's pitch
  // weighed by its amplitude and its number, a higher harmonic placing the
  // pitch more finely: with the first eight, then with all below 0.45 of the
  // rate.
  double fine(double guess) const {
    const double per_hz = 1.0 / bin_hz_;
    const size_t last = mag_.size() - 1;
    auto mag_at = [&](double hz) {
      const double pos = hz * per_hz;
      if (pos <= 0.0) return mag_[0];
      if (pos >= static_cast<double>(last)) return mag_[last];
      const auto i = static_cast<size_t>(pos);
      return mag_[i] + (pos - static_cast<double>(i)) * (mag_[i + 1] - mag_[i]);
    };
    double f0 = guess, strongest = -1.0;
    for (int cents = -60; cents <= 60; cents += 5) {
      const double f = guess * std::exp2(cents / 1200.0);
      double e = 0.0;
      for (int k = 1; k <= 8; k++) e += mag_at(k * f);
      if (e > strongest) {
        f0 = f;
        strongest = e;
      }
    }
    const int tops[] = {8, static_cast<int>(harmonics_under(f0, rate_))};
    for (int top : tops) {
      double num = 0.0, den = 0.0;
      for (int k = 1; k <= std::max(2, top); k++) {
        const auto lo = static_cast<int64_t>((k - 0.25) * f0 * per_hz);
        const auto hi = static_cast<int64_t>((k + 0.25) * f0 * per_hz) + 1;
        if (hi >= static_cast<int64_t>(last) || lo < 1) break;
        auto i = static_cast<size_t>(lo);
        for (auto u = static_cast<size_t>(lo); u < static_cast<size_t>(hi); u++)
          if (mag_[u] > mag_[i]) i = u;
        // Strongest at the edge of where it is looked for, rising past it, is
        // not this harmonic's peak; at a peak the parabola stays within half
        // a bin of it.
        if (i == 0 || i >= last || mag_[i] < mag_[i - 1] || mag_[i] < mag_[i + 1]) continue;
        const double a = std::log(std::max(mag_[i - 1], 1e-12)), b = std::log(std::max(mag_[i], 1e-12));
        const double c = std::log(std::max(mag_[i + 1], 1e-12)), curve = a - 2.0 * b + c;
        const double shift = curve < 0.0 ? 0.5 * (a - c) / curve : 0.0;
        assert(std::abs(shift) <= 0.5 + 1e-9 && "a peak's parabola stays within half a bin of it");
        const double w = mag_[i] * k;
        num += w * (static_cast<double>(i) + shift) * bin_hz_ / k;
        den += w;
      }
      if (den > 0.0) f0 = num / den;
    }
    return f0;
  }

  double rate_, low_, high_;
  size_t lo_, hi_, n_;  // the lags YIN searches, and its frame
  Fft yin_, spec_;
  std::vector<double> frame_, energy_, d_, cmnd_, power_;
  std::vector<std::complex<double>> a_;
  std::vector<double> long_, short_;  // the Hann windows the harmonics are read through
  std::vector<std::complex<double>> b_;
  std::vector<double> mag_;
  double bin_hz_;
  std::vector<double> kernel_;  // score()'s, a bin each
};

// The cost of a jump between frames: |cents| / 200, and 6 more within 50
// cents of a whole ratio (2, 3, 4; an octave costs 12). Those are the jumps
// a reading makes in error, and the ones an attack invites: its first
// frames can cost a whole fraction of the note least (a reed's first cycles
// alternate), while the note heard is the one held. A real leap by an octave
// pays it once against every frame of the new note.
inline double pitch_jump_cost(double f, double g) {
  const double c = PitchReader::cents_between(f, g);
  bool whole = false;
  for (double m : {2.0, 3.0, 4.0}) whole = whole || std::abs(c - 1200.0 * std::log2(m)) < 50.0;
  return c / 200.0 + (whole ? 6.0 : 0.0);
}

// Along a run of frames with candidates, the index of the one chosen in each.
inline std::vector<size_t> choose_pitches(const std::vector<const PitchReader::Frame*>& run) {
  const size_t n = run.size();
  std::vector<double> cost = run[0]->costs;
  std::vector<std::vector<size_t>> back(n);
  for (size_t i = 1; i < n; i++) {
    const auto& prev = run[i - 1]->cands;
    const auto& here = *run[i];
    std::vector<double> next(here.cands.size());
    back[i].resize(here.cands.size());
    for (size_t c = 0; c < here.cands.size(); c++) {
      double least = INFINITY;
      for (size_t p = 0; p < prev.size(); p++) {
        const double v = cost[p] + pitch_jump_cost(prev[p], here.cands[c]);
        if (v < least) {
          least = v;
          back[i][c] = p;
        }
      }
      next[c] = least + here.costs[c];
    }
    cost = std::move(next);
  }
  std::vector<size_t> path(n);
  path[n - 1] = static_cast<size_t>(std::min_element(cost.begin(), cost.end()) - cost.begin());
  for (size_t i = n - 1; i > 0; i--) path[i - 1] = back[i][path[i]];
  return path;
}

// The pitch of `x` (`n` long, `rate` a second, 20 <= low < high < rate / 2)
// every 10 ms: frame t about sample round(t * rate / 100), ceil(n * 100 /
// rate) frames.
inline PitchTrack pitch_track(const double* x, size_t n, double rate, double low, double high) {
  const size_t frames =
      n == 0 ? 0 : static_cast<size_t>(std::ceil(static_cast<double>(n) * 100.0 / rate - 1e-9));
  PitchTrack out{std::vector<double>(frames, std::nan("")), std::vector<double>(frames, 0.0)};
  PitchReader reader(rate, low, high);
  std::vector<PitchReader::Frame> listed(frames);
  for (size_t t = 0; t < frames; t++) {
    listed[t] = reader.read(x, n, frame_centre(static_cast<int64_t>(t), rate));
    out.clarity[t] = listed[t].clarity;
  }
  for (size_t i = 0; i < frames;) {
    if (listed[i].cands.empty()) {
      i++;
      continue;
    }
    std::vector<size_t> pitched;
    for (size_t gap = 0; i < frames && gap < 6; i++) {
      if (listed[i].cands.empty()) {
        gap++;
      } else {
        pitched.push_back(i);
        gap = 0;
      }
    }
    std::vector<const PitchReader::Frame*> run;
    for (size_t u : pitched) run.push_back(&listed[u]);
    auto path = choose_pitches(run);
    for (size_t j = 0; j < pitched.size(); j++) out.hz[pitched[j]] = listed[pitched[j]].fine[path[j]];
  }
  return out;
}

// The harmonics of a sound along its pitch every 10 ms, and what is left of
// it without them, as a harmonic-plus-residual analysis (Serra's SMS) splits
// a sound. Frame t, about frame_centre(t), reaches to its neighbours' middles
// through a raised cosine, 0 there, and across it the pitch moves in a
// straight line to each pitched neighbour's (held, a vibrato leaves its
// harmonics in the rest about as loud as a saxophone's breath). Harmonic k is
// the frame's projection on a sine and a cosine at k times that pitch's
// phase, twice it over the window's sum: for a steady tone, the peak
// Audio.partials sounds it at. Put back together, each frame's harmonics
// faded into its neighbours' through the same raised cosine (two pitched
// frames' sum to 1 between their middles, and the last frame's hold past its
// middle), they are taken from the sound to leave the rest.
struct HarmonicsTrack {
  std::vector<double> amps;  // `count` a frame, frame after frame; 0 where unpitched
  std::vector<double> rest;
};

// `f0` the pitch of each frame, NaN where there is none.
inline HarmonicsTrack harmonics_track(const double* x, size_t n, const std::vector<double>& f0, size_t count,
                                      double rate) {
  const size_t frames = f0.size();
  // The harmonics are put back together in `rest`, then taken from x there.
  HarmonicsTrack out{std::vector<double>(frames * count, 0.0), std::vector<double>(n, 0.0)};
  std::vector<double> weight;
  std::vector<std::complex<double>> turn, coef(count);
  for (size_t t = 0; t < frames; t++) {
    const double f = f0[t];
    if (!std::isfinite(f)) continue;
    const auto top = static_cast<size_t>(std::min(static_cast<double>(count), harmonics_under(f, rate)));
    if (top == 0) continue;
    const auto ti = static_cast<int64_t>(t);
    const int64_t c = frame_centre(ti, rate), back = c - frame_centre(ti - 1, rate),
                  ahead = frame_centre(ti + 1, rate) - c;
    const double before = t > 0 ? f0[t - 1] : NAN, after = t + 1 < frames ? f0[t + 1] : NAN;
    weight.resize(static_cast<size_t>(back + ahead - 1));
    turn.resize(weight.size());
    std::fill(coef.begin(), coef.end(), std::complex<double>{});
    double sum = 0.0;
    for (int64_t d = 1 - back; d < ahead; d++) {
      const auto i = static_cast<size_t>(d + back - 1);
      const double reach = static_cast<double>(d < 0 ? back : ahead), dd = static_cast<double>(d);
      weight[i] = 0.5 + 0.5 * std::cos(std::numbers::pi * dd / reach);
      sum += weight[i];
      // The fundamental's cycles from the middle, its pitch moving to g.
      double cycles = f * dd;
      const double g = d < 0 ? before : after;
      if (std::isfinite(g)) cycles += (g - f) * dd * std::abs(dd) / (2.0 * reach);
      turn[i] = std::polar(1.0, 2.0 * std::numbers::pi * cycles / rate);
      const double wy = weight[i] * sample_at(x, n, c + d);
      const auto step = std::conj(turn[i]);
      auto z = step;
      for (size_t k = 0; k < top; k++) {
        coef[k] += wy * z;
        z *= step;
      }
    }
    for (size_t k = 0; k < top; k++) {
      coef[k] *= 2.0 / sum;
      out.amps[t * count + k] = std::abs(coef[k]);
    }
    // Past the last frame's middle no frame takes over: it holds.
    const bool last = t + 1 == frames;
    for (int64_t d = 1 - back; d < ahead; d++) {
      const int64_t j = c + d;
      if (j < 0 || j >= static_cast<int64_t>(n)) continue;
      const auto i = static_cast<size_t>(d + back - 1);
      auto z = turn[i];
      double v = 0.0;
      for (size_t k = 0; k < top; k++) {
        v += (coef[k] * z).real();
        z *= turn[i];
      }
      out.rest[static_cast<size_t>(j)] += (last && d > 0 ? 1.0 : weight[i]) * v;
    }
  }
  for (size_t j = 0; j < n; j++) out.rest[j] = x[j] - out.rest[j];
  return out;
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
// `mod` radians move its phase for a sample (its phase value, the parts
// connected to it among them), and so do `feedback` times the mean of its
// last two outputs (as the DX7's operator feeds itself); the phase it keeps,
// in turns, runs on at `freq` regardless.
struct Oscillator {
  WaveKind kind;
  double phase = 0;
  double last[2] = {0, 0};

  // The next `n` samples into `out`, at `freq` Hz, moved by `mod` and, unless
  // it is null, by `feedback` (each `n` long). With no feedback no sample's
  // phase waits on the output before it, so the samples are worked out side
  // by side. The state stays in locals for the block.
  void run(int n, const double* freq, const double* mod, const double* feedback, double rate, double* out) {
    double ph = phase, y0 = last[0], y1 = last[1];
    auto next = [&](double t, double dt) {
      t -= std::floor(t);
      double y = shape(t, dt);
      ph = ph + dt - std::floor(ph + dt);
      y1 = y0;
      y0 = y;
      return y;
    };
    if (feedback)
      for (int i = 0; i < n; i++)
        out[i] = next(ph + (mod[i] + feedback[i] * 0.5 * (y0 + y1)) / (2.0 * std::numbers::pi), freq[i] / rate);
    else
      for (int i = 0; i < n; i++) out[i] = next(ph + mod[i] / (2.0 * std::numbers::pi), freq[i] / rate);
    phase = ph;
    last[0] = y0;
    last[1] = y1;
  }

 private:
  // The wave at phase `t` (0..1), stepping `dt` a sample.
  double shape(double t, double dt) const {
    switch (kind) {
      case WaveKind::Sine: return std::sin(2.0 * std::numbers::pi * t);
      case WaveKind::Saw: return 2.0 * t - 1.0 - poly_blep(t, dt);
      case WaveKind::Square: {
        double half = t + 0.5 - std::floor(t + 0.5);
        return (t < 0.5 ? 1.0 : -1.0) + poly_blep(t, dt) - poly_blep(half, dt);
      }
      default: return 4.0 * std::abs(t - std::floor(t + 0.5)) - 1.0;
    }
  }
};

// What went in `samples` samples ago, read between two samples in a straight
// line so the time may move smoothly; held from 0 to the longest the line
// keeps. step() takes a sample in and answers (0 samples is that sample);
// in a loop, read() answers from what went in before and write() takes the
// sample in after, one sample or more apart.
class DelayLine {
 public:
  explicit DelayLine(size_t longest) : buf_(longest + 2, 0.0) {}
  double step(double x, double samples) {
    buf_[w_] = x;
    const double y = read(samples);
    write_on();
    return y;
  }
  double read(double samples) const {
    const size_t size = buf_.size();
    samples = std::clamp(samples, 0.0, static_cast<double>(size - 2));
    double r = static_cast<double>(w_) - samples;
    if (r < 0) r += static_cast<double>(size);
    const auto i = static_cast<size_t>(r);
    const double frac = r - static_cast<double>(i);
    return buf_[i] + (buf_[ring_next(i, size)] - buf_[i]) * frac;
  }
  void write(double x) {
    buf_[w_] = x;
    write_on();
  }

 private:
  void write_on() { w_ = ring_next(w_, buf_.size()); }
  std::vector<double> buf_;
  size_t w_ = 0;
};

// What comes in kept at or under `ceiling`, without delaying it: the gain
// drops at the sample that would go over, by as much as it takes, and
// returns to 1 exponentially, with 1/e of the way left after each `release`
// seconds.
class Limiter {
 public:
  Limiter(double ceiling, double release, double rate)
      : ceiling_(ceiling), back_(1.0 - std::exp(-1.0 / (release * rate))) {}
  double step(double x) {
    gain_ += (1.0 - gain_) * back_;
    const double a = std::abs(x);
    if (a * gain_ <= ceiling_) return x * gain_;
    gain_ = ceiling_ / a;
    return std::copysign(ceiling_, x);  // not x * gain_, which can round past it
  }

 private:
  double ceiling_;
  double back_;
  double gain_ = 1.0;
};

// What comes in times `gain` (a value), rounded by tanh: near that while it is
// small, never past ±1, as an overdrive bends a loud signal. Each sample is
// tanh averaged over the straight line from the sample before, the step of its
// antiderivative (log cosh) over the step of the input: first-order
// antiderivative anti-aliasing, which folds back far less of what tanh adds
// above half the rate, at the cost of half a sample's delay.
class Drive {
 public:
  double step(double x, double gain) {
    const double u = x * gain;
    const double f = log_cosh(u);
    const double d = u - u_;
    const double y = std::abs(d) > 1e-6 ? (f - f_) / d : std::tanh(0.5 * (u + u_));
    u_ = u;
    f_ = f;
    return std::clamp(y, -1.0, 1.0);  // a step's rounding can pass ±1 a little
  }

 private:
  // log(cosh(u)), which does not overflow however large u is.
  static double log_cosh(double u) {
    const double a = std::abs(u);
    return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
  }
  double u_ = 0;
  double f_ = 0;
};

// 1 at the first sample of each period of its freq, 0 at the others:
// `start`, from 0 to 1, is how much of a period has gone by at the
// first sample, 0 making it fire there. A freq of 0 fires once, at the start
// when `start` is 0.
class Impulse {
 public:
  explicit Impulse(double start) : phase_(start == 0 ? 1.0 : start) {}
  double step(double freq, double rate) {
    double y = 0;
    if (phase_ >= 1) {
      phase_ -= std::floor(phase_);
      y = 1;
    }
    phase_ += freq / rate;
    return y;
  }

 private:
  double phase_;
};

// Impulses at random samples, `density` a second on average, each 0 to 1:
// a sample's number from 0 to 1, when under its chance, is
// the impulse, scaled by the chance to 0..1.
struct Dust {
  Noise noise;
  double step(double density, double rate) {
    const double chance = density / rate;
    const double z = 0.5 * (noise.next() + 1.0);
    return z < chance ? z / chance : 0.0;
  }
};

// How LfNoise moves between its numbers: it holds each, goes to the next in a
// straight line, or through the four about it (cubic).
enum class LfNoiseKind { Step, Linear, Cubic };

// A new number from -1 to 1 at freq Hz: its
// phase runs down from 1 at freq a second, and each time it passes 0 the
// numbers move on by one. A cubic reads the four about it by 4-point Hermite,
// which goes at most 1.25 times past them, so its numbers are 0.8 of the draw
// and it stays within ±1.
class LfNoise {
 public:
  LfNoise(LfNoiseKind kind, Noise noise) : kind_(kind), noise_(noise) {
    if (kind == LfNoiseKind::Linear) d_ = draw();
    if (kind == LfNoiseKind::Cubic)
      for (double* v : {&a_, &b_, &c_, &d_}) *v = draw();
  }
  double step(double freq, double rate) {
    phase_ -= freq / rate;
    if (phase_ < 0) {
      phase_ -= std::floor(phase_);
      a_ = b_;
      b_ = c_;
      c_ = d_;
      d_ = draw();
    }
    switch (kind_) {
      case LfNoiseKind::Step: return d_;
      case LfNoiseKind::Linear: return d_ + phase_ * (c_ - d_);
      case LfNoiseKind::Cubic: {
        const double x = 1.0 - phase_;
        const double c1 = 0.5 * (c_ - a_);
        const double c2 = a_ - 2.5 * b_ + 2.0 * c_ - 0.5 * d_;
        const double c3 = 0.5 * (d_ - a_) + 1.5 * (b_ - c_);
        return ((c3 * x + c2) * x + c1) * x + b_;
      }
    }
    return 0.0;
  }

 private:
  double draw() { return kind_ == LfNoiseKind::Cubic ? 0.8 * noise_.next() : noise_.next(); }
  LfNoiseKind kind_;
  Noise noise_;
  double phase_ = 0, a_ = 0, b_ = 0, c_ = 0, d_ = 0;
};

// The pole of a one-pole filter that falls 60 dB in `seconds`, 0 for 0
// seconds.
inline double pole_60db(double seconds, double rate) {
  return seconds == 0 ? 0.0 : std::exp(std::log(0.001) / (seconds * rate));
}

// What comes in, followed by a one-pole filter that closes 60 dB of a step in
// its time, from the first sample in rather than from 0.
class Lag {
 public:
  Lag(double seconds, double rate) : b_(pole_60db(seconds, rate)) {}
  double step(double x) {
    if (!started_) {
      y_ = x;
      started_ = true;
    }
    y_ = settled(x + b_ * (y_ - x));
    return y_;
  }

 private:
  double b_, y_ = 0;
  bool started_ = false;
};

// What comes in plus what it answered a sample before, times the pole that
// falls 60 dB in its time: an impulse in rings down.
class Decay {
 public:
  Decay(double seconds, double rate) : b_(pole_60db(seconds, rate)) {}
  double step(double x) {
    y_ = settled(x + b_ * y_);
    return y_;
  }

 private:
  double b_, y_ = 0;
};

// A room's feedback and damp at `size` and `damp`, each 0 to 1 (Freeverb's
// scaling: size 4/7 and damp 0.625 are Kauai's room).
inline double reverb_feedback(double size) { return 0.7 + 0.28 * size; }
inline double reverb_damp(double damp) { return 0.4 * damp; }

// One side of a room (Freeverb, Jezar's): eight damped combs in parallel,
// then four allpasses in series, what comes in taken in at kInput. The right
// side (`side` 1) has every delay kSpread samples longer, so the two sides
// ring apart; the delays are Jezar's at 44100 and in proportion at another
// rate. Kauai's Room plays a song in two of these, and a graph's room part is
// one. `feedback` sets how long it rings and `damp` how soon its highs go.
class ReverbTank {
 public:
  static constexpr int kCombs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  static constexpr int kPasses[4] = {556, 441, 341, 225};
  static constexpr int kSpread = 23;
  static constexpr double kInput = 0.015;

  ReverbTank(double rate, int side) {
    uint32_t start = 0;
    for (int k = 0; k < kLines; k++) {
      const int d = k < 8 ? kCombs[k] : kPasses[k - 8];
      const auto length = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround((d + side * kSpread) * rate / 44100.0)));
      line_.insert(line_.end(), {start, length, 0});
      start += length;
    }
    state_.assign(start + 8, 0.0f);
  }
  // Silent again.
  void reset() {
    std::ranges::fill(state_, 0.0f);
    for (int k = 0; k < kLines; k++) line_[3 * k + 2] = 0;
  }
  // The echo this sample, of `x` and what came in before it.
  double step(double x, double feedback, double damp) {
    float* damped = state_.data() + state_.size() - 8;
    // Line k's sample now, which `put` then replaces before it moves on.
    auto line = [&](int k) -> float& { return state_[line_[3 * k] + line_[3 * k + 2]]; };
    auto on = [&](int k) { line_[3 * k + 2] = ring_next(line_[3 * k + 2], line_[3 * k + 1]); };
    double into = x * kInput;
    double echo = 0;
    for (int k = 0; k < 8; k++) {
      float& at = line(k);
      double y = at;
      damped[k] = static_cast<float>(settled(y * (1.0 - damp) + damped[k] * damp));
      at = static_cast<float>(settled(into + damped[k] * feedback));
      on(k);
      echo += y;
    }
    for (int k = 8; k < kLines; k++) {
      float& at = line(k);
      double held = at;
      at = static_cast<float>(settled(echo + held * 0.5));
      on(k);
      echo = held - echo;
    }
    return echo;
  }

 private:
  static constexpr int kLines = 12;  // the combs, then the allpasses
  // The lines end to end, then the combs' damped samples; and each line's
  // start, length and place, three numbers a line. Kept out of the part
  // itself, so a graph's parts stay small whatever kind they are.
  std::vector<float> state_;
  std::vector<uint32_t> line_;
};

// What a Math part answers for what comes in: its size, it held within lo..hi,
// e to its power, or it rounded to the nearest multiple of `unit` (a half
// going up).
enum class MathKind { Abs, Clip, Exp, Round };

struct MathOp {
  MathKind kind;
  double lo = -1, hi = 1;
  double unit = 1;
  double step(double x) const {
    switch (kind) {
      case MathKind::Abs: return std::abs(x);
      case MathKind::Clip: return std::clamp(x, lo, hi);
      case MathKind::Exp: return std::exp(x);
      case MathKind::Round: return std::floor(x / unit + 0.5) * unit;
    }
    return 0.0;
  }
};

// What came in when its trigger last went from 0 or below to above 0, held
// until it next does; 0 until it first does. A trigger above 0 at the first
// sample takes that sample in.
class Latch {
 public:
  double step(double x, double trigger) {
    const bool up = trigger > 0;
    if (up && !was_up_) held_ = x;
    was_up_ = up;
    return held_;
  }

 private:
  double held_ = 0;
  bool was_up_ = false;
};

enum class PartKind { Wave, Noise, Filter, Gain, Resonate, Pluck, Delay, Limit, Drive, Impulse, Dust, LfNoise, Lag, Decay, Math, Room,
                      Input, Latch };

// How many values a part of `kind` reads: a wave's freq, feedback and phase,
// a filter's cutoff, a gain's gain, a string's freq and ring, a delay's time,
// a drive's gain, an impulse's and an lfnoise's freq, a dust's density, a
// room's size and damp, a latch's trigger.
// (Every kind is named here and below, so a kind added is a -Wswitch error in
// check-bench-audio-graph, which builds this header with -Werror, until each
// says what it is.)
inline int value_count(PartKind kind) {
  switch (kind) {
    case PartKind::Noise:
    case PartKind::Resonate:
    case PartKind::Limit:
    case PartKind::Lag:
    case PartKind::Decay:
    case PartKind::Math:
    case PartKind::Input: return 0;
    case PartKind::Filter:
    case PartKind::Gain:
    case PartKind::Delay:
    case PartKind::Drive:
    case PartKind::Impulse:
    case PartKind::Dust:
    case PartKind::LfNoise:
    case PartKind::Latch: return 1;
    case PartKind::Pluck:
    case PartKind::Room: return 2;
    case PartKind::Wave: return 3;
  }
  return 0;
}
inline constexpr int kGraphValues = 3;  // the most value_count answers

// Whether a part of `kind` takes in what is connected to it; a source (a
// wave, a noise, an impulse, a dust, an lfnoise, an input) makes its own.
inline bool takes_in(PartKind kind) {
  switch (kind) {
    case PartKind::Wave:
    case PartKind::Noise:
    case PartKind::Impulse:
    case PartKind::Dust:
    case PartKind::LfNoise:
    case PartKind::Input: return false;
    case PartKind::Filter:
    case PartKind::Gain:
    case PartKind::Resonate:
    case PartKind::Pluck:
    case PartKind::Delay:
    case PartKind::Limit:
    case PartKind::Drive:
    case PartKind::Lag:
    case PartKind::Decay:
    case PartKind::Math:
    case PartKind::Room:
    case PartKind::Latch: return true;
  }
  return false;
}

inline constexpr int kGraphBlock = 128;
// The lowest a graph's string is tuned, which sizes its loop.
inline constexpr double kGraphPluckLowest = 20.0;
// The longest a graph's delay holds what went in, in seconds (Web Audio's
// DelayNode's default).
inline constexpr double kGraphDelayLongest = 1.0;

// The parts' processors as a graph makes them for `rate`: a delay as long as
// a graph's delay can be, a string as low as a graph's string can be tuned.
inline DelayLine graph_delay_line(double rate) {
  return DelayLine(static_cast<size_t>(std::ceil(kGraphDelayLongest * rate)));
}
inline PluckString graph_string(double rate) {
  return PluckString(PluckString::loop_length(kGraphPluckLowest, rate) + 1, rate);
}

// Where value `value` of a part of `kind` is held once the parts connected
// to it have moved it, so the part stays sound: a wave's, an impulse's and an
// lfnoise's freq, a cutoff and a string's freq from 0 (a string's from its
// lowest) to half the rate, a dust's density from 0 to the rate, a delay's
// time from 0 to its longest, a room's size and damp from 0 to 1, and a
// wave's feedback, a string's ring and a drive's gain at 0 or more. Found
// as the part is made, so the samples are only clamped.
struct ValueRange {
  double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
};
inline ValueRange held_range(PartKind kind, int value, double rate) {
  constexpr double kUp = std::numeric_limits<double>::infinity();
  switch (kind) {
    case PartKind::Wave: return value == 0 ? ValueRange{0.0, 0.5 * rate} : value == 1 ? ValueRange{0.0, kUp} : ValueRange{};
    case PartKind::Filter:
    case PartKind::Impulse:
    case PartKind::LfNoise: return {0.0, 0.5 * rate};
    case PartKind::Dust: return {0.0, rate};
    case PartKind::Pluck: return value == 0 ? ValueRange{kGraphPluckLowest, 0.5 * rate} : ValueRange{0.0, kUp};
    case PartKind::Delay: return {0.0, kGraphDelayLongest};
    case PartKind::Drive: return {0.0, kUp};
    case PartKind::Room: return {0.0, 1.0};
    case PartKind::Gain:
    case PartKind::Noise:
    case PartKind::Resonate:
    case PartKind::Limit:
    case PartKind::Lag:
    case PartKind::Decay:
    case PartKind::Math:
    case PartKind::Input:
    case PartKind::Latch: break;  // a gain's and a latch's trigger are any number; the rest have no values
  }
  return {};
}
// How many graphs play at once: the mixer's list of them never grows.
inline constexpr size_t kGraphsPlaying = 64;
// Room for changes a copy being played makes in each value at the start, on
// top of those it was played with; more comes in a longer list (graph_post).
inline constexpr size_t kGraphLiveRoom = 256;

// The samples the mixer asks for at once, at most (Engine's kMixChunk): a
// bus holds one such stretch a channel.
inline constexpr int64_t kBusFrames = 4096;

// A bus: what graphs played into it add up to over the stretch the mixer is
// rendering, a channel or two, which an input part of another graph reads.
// Made and let go on the main thread (a graph holds the buses it reads and
// the one it plays into); the audio thread clears it at the first touch of
// each stretch, so a bus no graph plays into reads as silence.
struct Bus {
  explicit Bus(int channels) : channels(channels) {}
  // Silent for the stretch of `frames` at stream sample `pos`, unless it
  // already holds that stretch.
  void begin(int64_t pos, int64_t frames) {
    if (cleared == pos) return;
    for (int c = 0; c < channels; c++) std::fill(samples[c], samples[c] + frames, 0.0);
    cleared = pos;
  }
  const int channels;
  int64_t cleared = -1;  // audio thread: the stream sample of the stretch it holds
  double samples[2][kBusFrames] = {};
};
// The buses a program has made, by handle. Main thread only.
inline std::unordered_map<int64_t, std::shared_ptr<Bus>>& bus_defs() {
  static std::unordered_map<int64_t, std::shared_ptr<Bus>> defs;
  return defs;
}

// An input part's: the bus and channel it reads, and where in the stretch
// being rendered it is.
struct BusRead {
  std::shared_ptr<Bus> bus;
  int64_t id = 0;
  int channel = 0;
  int64_t at = 0;
};

struct GraphPart {
  PartKind kind;
  std::vector<int> inputs;  // parts added together as what it takes in
  // A wave's freq, feedback and phase, a filter's cutoff, a gain's gain, a
  // string's freq and ring, a delay's time, a drive's gain, an impulse's and
  // an lfnoise's freq, a dust's density, a room's size and damp, a latch's
  // trigger; each is its schedule plus the sum of the parts connected to it.
  Param values[kGraphValues];
  std::vector<int> connected[kGraphValues];
  ValueRange held[kGraphValues];  // where each value is held (held_range's)
  std::variant<std::monostate, Oscillator, Noise, Svf, Resonance, PluckString, DelayLine, Limiter, Drive, Impulse, Dust,
               LfNoise, Lag, Decay, MathOp, ReverbTank, BusRead, Latch>
      processor;
  double out[kGraphBlock] = {};
};

// How a part is worked out: for the samples of a block at once (Block), or in
// a loop a sample at a time (Sample), a delay then in two halves: Read, what
// it answers from what went in before, and Write, the sample it takes in.
enum class GraphPhase : uint8_t { Block, Sample, Read, Write };

// One step of a block: a part worked out for the whole block, or a loop, its
// parts worked out one sample after another in the order given.
struct GraphStep {
  int part = -1;  // the part, when this is not a loop
  std::vector<std::pair<int, GraphPhase>> loop;
};

// The strongly connected sets of a graph, `out[v]` the nodes v leads to
// (Tarjan's, without recursion): `set[v]` is the set of v, and the number of
// sets is answered.
inline int strongly_connected(const std::vector<std::vector<int>>& out, std::vector<int>& set) {
  const auto n = static_cast<int>(out.size());
  std::vector<int> index(n, -1), low(n, 0), stack;
  std::vector<char> open(n, 0);
  set.assign(n, -1);
  struct Frame {
    int node;
    size_t next;
  };
  std::vector<Frame> calls;
  int counter = 0, sets = 0;
  for (int root = 0; root < n; root++) {
    if (index[root] >= 0) continue;
    index[root] = low[root] = counter++;
    stack.push_back(root);
    open[root] = 1;
    calls.push_back({root, 0});
    while (!calls.empty()) {
      const int v = calls.back().node;
      if (calls.back().next < out[v].size()) {
        const int w = out[v][calls.back().next++];
        if (index[w] < 0) {
          index[w] = low[w] = counter++;
          stack.push_back(w);
          open[w] = 1;
          calls.push_back({w, 0});
        } else if (open[w]) {
          low[v] = std::min(low[v], index[w]);
        }
        continue;
      }
      if (low[v] == index[v]) {
        for (int w = -1; w != v;) {
          w = stack.back();
          stack.pop_back();
          open[w] = 0;
          set[w] = sets;
        }
        sets++;
      }
      calls.pop_back();
      if (!calls.empty()) low[calls.back().node] = std::min(low[calls.back().node], low[v]);
    }
  }
  return sets;
}

// The nodes 0 to key.size() - 1 in an order each comes after those with a line
// (from, to) to it, the one with the smallest `key` first among those with
// nothing left before them (the keys are different); none when the lines make
// a cycle.
inline std::optional<std::vector<int>> smallest_first_order(const std::vector<int>& key,
                                                            const std::vector<std::pair<int, int>>& lines) {
  const auto count = static_cast<int>(key.size());
  std::vector<std::vector<int>> next(count);
  std::vector<int> waiting(count, 0);
  for (const auto& [from, to] : lines) {
    next[from].push_back(to);
    waiting[to]++;
  }
  std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> ready;
  for (int v = 0; v < count; v++)
    if (waiting[v] == 0) ready.push({key[v], v});
  std::vector<int> order;
  while (!ready.empty()) {
    const int v = ready.top().second;
    ready.pop();
    order.push_back(v);
    for (int w : next[v])
      if (--waiting[w] == 0) ready.push({key[w], w});
  }
  if (static_cast<int>(order.size()) != count) return std::nullopt;
  return order;
}

// Parts wired together, each reading the sum of the parts it takes in, and
// each of its values the sum of the parts connected to it on top of its
// schedule, as Web Audio's AudioParam does; what the graph answers is one
// part's. A part may be connected from any part, but a loop has to pass
// through a delay, which answers from what went in at least a sample before.
// Every part and value is worked out a sample at a time, in blocks that fall
// at the same samples however the samples are asked for, so a graph played
// and a graph rendered answer the same. The graph a program builds is never
// worked out itself: rendering or playing one works out a copy, from the
// start.
class Graph {
 public:
  explicit Graph(double rate) : rate_(rate) {}
  double rate() const { return rate_; }
  size_t size() const { return parts_.size(); }
  PartKind kind(int part) const { return parts_[static_cast<size_t>(part)].kind; }

  // A part taking in `inputs`, parts made before it.
  int add(PartKind kind, std::vector<int> inputs, decltype(GraphPart::processor) processor, double rest0 = 0,
          double rest1 = 0, double rest2 = 0) {
    parts_.push_back(GraphPart{kind, std::move(inputs), {Param(rest0), Param(rest1), Param(rest2)}, {},
                               {held_range(kind, 0, rate_), held_range(kind, 1, rate_), held_range(kind, 2, rate_)},
                               std::move(processor)});
    steps_.push_back(GraphStep{static_cast<int>(parts_.size()) - 1, {}});
    return static_cast<int>(parts_.size()) - 1;
  }
  // `from`'s output added into `to`: what it takes in when `value` is -1, its
  // value `value` otherwise. False, and nothing connected, when that closes a
  // loop that does not pass through a delay.
  bool connect(int from, int to, int value) {
    assert(value < 0 ? takes_in(kind(to)) : value < value_count(kind(to)));
    auto& list = value < 0 ? parts_[static_cast<size_t>(to)].inputs : parts_[static_cast<size_t>(to)].connected[value];
    list.push_back(from);
    // A line to a later part cannot close a loop, and while every line is
    // one, the parts run in the order they were made.
    if (forward_ && from < to) return true;
    std::vector<GraphStep> steps;
    if (!order(steps)) {
      list.pop_back();
      return false;
    }
    forward_ = false;
    steps_ = std::move(steps);
    return true;
  }
  // The output: one part, or a left and a right (`right` -1 for one).
  void output(int left, int right = -1) {
    out_ = left;
    out_right_ = right;
  }
  bool stereo() const { return out_right_ >= 0; }
  // The buses its input parts read, by handle.
  std::vector<int64_t> buses_read() const {
    std::vector<int64_t> ids;
    for (const auto& p : parts_)
      if (p.kind == PartKind::Input) ids.push_back(std::get<BusRead>(p.processor).id);
    return ids;
  }
  bool reads_bus() const {
    return std::ranges::any_of(parts_, [](const GraphPart& p) { return p.kind == PartKind::Input; });
  }
  // A copy being played: the bus it plays into (null for the device).
  void play_into(std::shared_ptr<Bus> bus) { into_ = std::move(bus); }
  Bus* into() const { return into_.get(); }
  // Before a copy being played renders the samples of a stretch from `skip`
  // on: its inputs read their buses from there, a bus no graph has played
  // into this stretch (`frames` at stream sample `pos`) read as silence.
  void begin_reads(int64_t skip, int64_t pos, int64_t frames) {
    for (auto& p : parts_) {
      if (p.kind != PartKind::Input) continue;
      auto& r = std::get<BusRead>(p.processor);
      r.bus->begin(pos, frames);
      r.at = skip;
    }
  }
  void schedule(int part, int value, ParamEvent e) {
    parts_[static_cast<size_t>(part)].values[value].schedule(e);
  }
  ParamEvent held(int part, int value, double at) const {
    return parts_[static_cast<size_t>(part)].values[value].held(at);
  }
  // How many changes value `value` of `part` has been given.
  size_t changes(int part, int value) const { return parts_[static_cast<size_t>(part)].values[value].size(); }
  // Room in every value for `kGraphLiveRoom` changes more, so a copy being
  // played takes changes without allocating; how many each can hold goes
  // into `room`, kGraphValues a part.
  void make_room(std::vector<size_t>& room) {
    room.clear();
    for (auto& p : parts_)
      for (int k = 0; k < kGraphValues; k++)
        room.push_back(k < value_count(p.kind) ? p.values[k].make_room(kGraphLiveRoom) : 0);
  }
  // A change to a copy being played, on the audio thread, into `longer` when
  // it comes with one (Param::take_list): false when there is no room.
  bool schedule_in_place(int part, int value, ParamEvent e, std::vector<ParamEvent>& longer) {
    auto& v = parts_[static_cast<size_t>(part)].values[value];
    if (longer.capacity() > 0) v.take_list(longer);
    return v.schedule_in_place(e);
  }

  // The next `n` samples into `out`: the left, or the one output.
  void render(double* out, int64_t n) { render(out, nullptr, n); }
  // The next `n` samples of each channel; `right` may be null. A graph with
  // one output answers it on both.
  void render(double* left, double* right, int64_t n) {
    const int r = stereo() ? out_right_ : out_;
    auto copy = [&](int part, double* into, int m) {
      for (int i = 0; i < m; i++) into[i] = part >= 0 ? parts_[static_cast<size_t>(part)].out[i] : 0.0;
    };
    for (int64_t done = 0; done < n;) {
      int m = static_cast<int>(std::min<int64_t>(kGraphBlock - pos_ % kGraphBlock, n - done));
      block(m);
      copy(out_, left + done, m);
      if (right) copy(r, right + done, m);
      done += m;
    }
  }

 private:
  void block(int n) {
    double time[kGraphBlock];
    for (int i = 0; i < n; i++) time[i] = static_cast<double>(pos_ + i) / rate_;
    for (const auto& step : steps_) {
      if (step.part >= 0) {
        work<GraphPhase::Block>(parts_[static_cast<size_t>(step.part)], 0, n, time);
        continue;
      }
      for (int i = 0; i < n; i++) {
        for (const auto& [part, phase] : step.loop) {
          auto& p = parts_[static_cast<size_t>(part)];
          switch (phase) {
            case GraphPhase::Block:  // a loop has none: it is worked out a sample at a time
            case GraphPhase::Sample: work<GraphPhase::Sample>(p, i, 1, time); break;
            case GraphPhase::Read: work<GraphPhase::Read>(p, i, 1, time); break;
            case GraphPhase::Write: work<GraphPhase::Write>(p, i, 1, time); break;
          }
        }
      }
    }
    pos_ += n;
  }

  // Part `p` for `count` samples from `offset` samples into the block (a
  // Block is the whole block, from 0), `time` the block's seconds. A delay's
  // Read takes no input and its Write no values: they come at different points
  // of a loop. `phase` is a template argument, and a Block's offset a
  // constant, so that the block-at-a-time path, all but loops, is worked out
  // as it was before loops; a loop's phases are one sample each, a constant
  // too, so their work is compiled for one sample.
  template <GraphPhase phase>
  void work(GraphPart& p, int offset, int count, const double* time) {
    assert(phase == GraphPhase::Block || count == 1);
    const int a = phase == GraphPhase::Block ? 0 : offset;
    constexpr int kSamples = phase == GraphPhase::Block ? kGraphBlock : 1;
    const int n = phase == GraphPhase::Block ? count : 1;
    double in[kSamples], signal[kSamples], v[kGraphValues][kSamples];
    double *v0 = v[0], *v1 = v[1], *v2 = v[2];
    auto sum = [&](const std::vector<int>& from, double* into) {
      std::fill(into, into + n, 0.0);
      for (int k : from) {
        const double* out = parts_[static_cast<size_t>(k)].out + a;
        for (int i = 0; i < n; i++) into[i] += out[i];
      }
    };
    if (phase != GraphPhase::Read) sum(p.inputs, in);
    if (phase != GraphPhase::Write) {
      for (int k = 0; k < value_count(p.kind); k++) {
        p.values[k].fill(time + a, n, v[k]);
        if (p.connected[k].empty()) continue;
        sum(p.connected[k], signal);
        const auto [lo, hi] = p.held[k];
        if (std::isinf(lo) && std::isinf(hi)) {
          for (int i = 0; i < n; i++) v[k][i] += signal[i];
        } else {
          for (int i = 0; i < n; i++) v[k][i] = std::clamp(v[k][i] + signal[i], lo, hi);
        }
      }
    }
    double* out = p.out + a;
    // A processor stepped by what comes in, and a source stepped by its value.
    auto each_in = [&](auto& q) {
      for (int i = 0; i < n; i++) out[i] = q.step(in[i]);
    };
    auto each_value = [&](auto& q) {
      for (int i = 0; i < n; i++) out[i] = q.step(v0[i], rate_);
    };
    switch (p.kind) {
      case PartKind::Wave: {
        bool feeds = std::any_of(v1, v1 + n, [](double feedback) { return feedback != 0.0; });
        std::get<Oscillator>(p.processor).run(n, v0, v2, feeds ? v1 : nullptr, rate_, out);
        break;
      }
      case PartKind::Noise: {
        auto& z = std::get<Noise>(p.processor);
        for (int i = 0; i < n; i++) out[i] = z.next();
        break;
      }
      case PartKind::Filter: {
        auto& f = std::get<Svf>(p.processor);
        for (int i = 0; i < n; i++) {
          f.tune(v0[i], rate_);
          out[i] = f.step(in[i]);
        }
        break;
      }
      case PartKind::Gain:
        for (int i = 0; i < n; i++) out[i] = in[i] * v0[i];
        break;
      case PartKind::Resonate: each_in(std::get<Resonance>(p.processor)); break;
      case PartKind::Pluck: {
        auto& s = std::get<PluckString>(p.processor);
        for (int i = 0; i < n; i++) {
          s.tune(v0[i], v1[i]);
          out[i] = s.step(in[i]);
        }
        break;
      }
      case PartKind::Delay: {
        auto& d = std::get<DelayLine>(p.processor);
        // In a loop it answers from what went in a sample before at the
        // soonest, and takes in only what is a number, a bit more than silence.
        if (phase == GraphPhase::Read)
          for (int i = 0; i < n; i++) out[i] = d.read(std::max(v0[i] * rate_, 1.0));
        else if (phase == GraphPhase::Write)
          for (int i = 0; i < n; i++) d.write(settled(in[i]));
        else
          for (int i = 0; i < n; i++) out[i] = d.step(in[i], v0[i] * rate_);
        break;
      }
      case PartKind::Limit: each_in(std::get<Limiter>(p.processor)); break;
      case PartKind::Drive: {
        auto& d = std::get<Drive>(p.processor);
        for (int i = 0; i < n; i++) out[i] = d.step(in[i], v0[i]);
        break;
      }
      case PartKind::Impulse: each_value(std::get<Impulse>(p.processor)); break;
      case PartKind::Dust: each_value(std::get<Dust>(p.processor)); break;
      case PartKind::LfNoise: each_value(std::get<LfNoise>(p.processor)); break;
      case PartKind::Lag: each_in(std::get<Lag>(p.processor)); break;
      case PartKind::Decay: each_in(std::get<Decay>(p.processor)); break;
      case PartKind::Math: each_in(std::get<MathOp>(p.processor)); break;
      case PartKind::Latch: {
        auto& l = std::get<Latch>(p.processor);
        for (int i = 0; i < n; i++) out[i] = l.step(in[i], v0[i]);
        break;
      }
      case PartKind::Room: {
        auto& t = std::get<ReverbTank>(p.processor);
        for (int i = 0; i < n; i++) out[i] = t.step(in[i], reverb_feedback(v0[i]), reverb_damp(v1[i]));
        break;
      }
      case PartKind::Input: {
        // Bound by begin_reads: never past the stretch (render() refuses a
        // graph that reads a bus, so only the mixer works one out).
        auto& r = std::get<BusRead>(p.processor);
        assert(r.at + n <= kBusFrames);
        const double* from = r.bus->samples[r.channel] + r.at;
        std::copy(from, from + n, out);
        r.at += n;
        break;
      }
    }
  }

  // A delay answers from what went in before, so it cuts a loop: in a loop it
  // is two nodes, its Read (what it answers, with its time), which is also the
  // node of any other part, and its Write (what it takes in).
  static constexpr int read_node(int part) { return 2 * part; }
  static constexpr int write_node(int part) { return 2 * part + 1; }
  static constexpr int part_of(int node) { return node / 2; }

  // The steps of a block in the order they run in, from the lines between the
  // parts, into `steps`; false when a loop does not pass through a delay.
  //
  // Loops are the strongly connected sets of parts. A loop is in order when its
  // nodes are: a line into a delay's input goes to its Write, a line into its
  // time or a line out of it to its Read, so a loop with no delay (or with only
  // a delay's own answer into its own time) has no order. The sets are then
  // put in order, each a part that is not in a loop or a loop, ties going to
  // the part made first, so a graph with only lines to later parts runs as
  // made.
  bool order(std::vector<GraphStep>& steps) const {
    const auto n = static_cast<int>(parts_.size());
    struct Line {
      int from, to;
      bool input;  // into what the part takes in, not into one of its values
    };
    std::vector<Line> lines;
    for (int t = 0; t < n; t++) {
      const auto& p = parts_[t];
      for (int s : p.inputs) lines.push_back({s, t, true});
      for (const auto& list : p.connected)
        for (int s : list) lines.push_back({s, t, false});
    }
    // The node a line goes to.
    auto goes_to = [&](const Line& l) {
      return l.input && kind(l.to) == PartKind::Delay ? write_node(l.to) : read_node(l.to);
    };

    std::vector<std::vector<int>> out(n);
    for (const auto& l : lines) out[l.from].push_back(l.to);
    std::vector<int> set;
    const int sets = strongly_connected(out, set);
    std::vector<std::vector<int>> members(sets);  // the first made first
    for (int p = 0; p < n; p++) members[set[p]].push_back(p);
    std::vector<char> loops(sets, 0);
    for (int s = 0; s < sets; s++) loops[s] = members[s].size() > 1;
    for (const auto& l : lines)
      if (l.from == l.to) loops[set[l.from]] = 1;

    std::vector<std::vector<std::pair<int, GraphPhase>>> ordered(sets);
    for (int s = 0; s < sets; s++) {
      if (!loops[s]) continue;
      std::vector<int> nodes;  // the loop's, smallest first
      for (int p : members[s]) {
        nodes.push_back(read_node(p));
        if (kind(p) == PartKind::Delay) nodes.push_back(write_node(p));
      }
      auto slot = [&](int node) { return static_cast<int>(std::lower_bound(nodes.begin(), nodes.end(), node) - nodes.begin()); };
      std::vector<std::pair<int, int>> inside;
      for (const auto& l : lines)
        if (set[l.from] == s && set[l.to] == s) inside.emplace_back(slot(read_node(l.from)), slot(goes_to(l)));
      std::vector<int> key(nodes.size());
      for (size_t i = 0; i < key.size(); i++) key[i] = static_cast<int>(i);
      const auto sorted = smallest_first_order(key, inside);
      if (!sorted) return false;
      for (int i : *sorted) {
        const int node = nodes[i], p = part_of(node);
        ordered[s].emplace_back(p, kind(p) != PartKind::Delay ? GraphPhase::Sample
                                   : node == read_node(p)     ? GraphPhase::Read
                                                              : GraphPhase::Write);
      }
    }

    std::vector<std::pair<int, int>> between;
    for (const auto& l : lines)
      if (set[l.from] != set[l.to]) between.emplace_back(set[l.from], set[l.to]);
    std::vector<int> first(sets);
    for (int s = 0; s < sets; s++) first[s] = members[s].front();
    const auto sets_in_order = smallest_first_order(first, between);
    assert(sets_in_order);  // the sets of a graph have no cycle between them
    steps.clear();
    for (int s : *sets_in_order)
      steps.push_back(loops[s] ? GraphStep{-1, std::move(ordered[s])} : GraphStep{members[s].front(), {}});
    return true;
  }

  double rate_;
  std::vector<GraphPart> parts_;
  std::vector<GraphStep> steps_;  // the order a block runs in
  bool forward_ = true;           // every line goes to a later part, so steps_ is the order the parts were made
  int out_ = -1, out_right_ = -1;
  int64_t pos_ = 0;
  std::shared_ptr<Bus> into_;
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
// A graph playing: when it started on the monotonic clock (what graph_now
// answers with no device to keep time), the bus it plays into (0 for the
// device), the buses it reads, and the how-manyth play it was.
struct GraphPlay {
  std::chrono::steady_clock::time_point started;
  int64_t into = 0;
  std::vector<int64_t> reads;
  uint64_t seq = 0;
};
// The graphs playing, by handle. Main thread only, and the same with or
// without a device, so a program is refused the same way on either.
inline std::unordered_map<int64_t, GraphPlay>& graphs_playing() {
  static std::unordered_map<int64_t, GraphPlay> playing;
  return playing;
}
// The graphs in the order the mixer works them out: each one that plays into
// a bus before those that read it, the rest in the order they were played;
// none when they play into each other's buses in a loop.
inline std::optional<std::vector<int64_t>> graph_order(const std::unordered_map<int64_t, GraphPlay>& playing) {
  std::vector<int64_t> ids;
  for (const auto& [id, play] : playing) ids.push_back(id);
  std::ranges::sort(ids, {}, [&](int64_t id) { return playing.at(id).seq; });
  std::vector<int> key(ids.size());
  std::vector<std::pair<int, int>> lines;
  for (size_t a = 0; a < ids.size(); a++) {
    key[a] = static_cast<int>(a);
    const int64_t into = playing.at(ids[a]).into;
    if (into == 0) continue;
    for (size_t b = 0; b < ids.size(); b++)
      if (std::ranges::count(playing.at(ids[b]).reads, into) > 0) lines.emplace_back(static_cast<int>(a), static_cast<int>(b));
  }
  auto order = smallest_first_order(key, lines);
  if (!order) return std::nullopt;
  std::vector<int64_t> out;
  for (int k : *order) out.push_back(ids[static_cast<size_t>(k)]);
  return out;
}
// Graph `id` played into `into` (0 for the device), reading `reads`: the
// graphs playing then, in graph_order's order. None, and nothing kept, when
// they would play into each other's buses in a loop.
inline std::optional<std::vector<int64_t>> graph_played(int64_t id, int64_t into, std::vector<int64_t> reads) {
  static uint64_t plays = 0;
  auto playing = graphs_playing();
  playing[id] = GraphPlay{std::chrono::steady_clock::now(), into, std::move(reads), ++plays};
  auto order = graph_order(playing);
  if (order) graphs_playing() = std::move(playing);
  return order;
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
// A schedule's events go to the page whole, with the width of a row; it
// schedules them on its own clock.
EM_JS(void, _wasm_audio_schedule_new,
      (int id, const double* events, int count, int stride, double length, double loop_at), {
  postMessage({ type: "schedule", cmd: "new", id: id,
                events: HEAPF64.slice(events >> 3, (events >> 3) + count),
                stride: stride, length: length, loopAt: loop_at });
});
EM_JS(void, _wasm_audio_schedule, (int cmd, int id, double value), {
  const names = ["play", "stop", "free", "volume", "room"];
  postMessage({ type: "schedule", cmd: names[cmd], id: id, value: value });
});

inline bool available() { return true; }
// The page's AudioContext chooses its own latency.
inline bool latency_ask(double) { return true; }
inline double latency() { return std::nan(""); }
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

inline void schedule_new(int64_t id, const double* events, int64_t count,
                      double length, double loop_at) {
  _wasm_audio_schedule_new(static_cast<int>(id), events, static_cast<int>(count),
                           static_cast<int>(kScheduleStride), length, loop_at);
}
inline void schedule_play(int64_t id) { _wasm_audio_schedule(0, static_cast<int>(id), 0); }
inline void schedule_stop(int64_t id) { _wasm_audio_schedule(1, static_cast<int>(id), 0); }
inline void schedule_free(int64_t id) { _wasm_audio_schedule(2, static_cast<int>(id), 0); }
inline void schedule_volume(int64_t id, double x) { _wasm_audio_schedule(3, static_cast<int>(id), x); }
inline void schedule_room(int64_t id, double x) { _wasm_audio_schedule(4, static_cast<int>(id), x); }
// The page plays a song whole.
inline bool schedule_open(int64_t, const double*, int64_t, int64_t, double, double) { return false; }
inline void schedule_fill(int64_t, int64_t, int64_t) {}
inline double schedule_held(int64_t) { return 0.0; }
// The page mixes a schedule in WebAudio, which renders nothing back to the program.
inline bool schedule_render(const double*, int64_t, double, double, double, double,
                         const std::vector<SoundBytes>&, std::vector<float>&, std::vector<float>*) {
  return false;
}
// The page's decoder answers later, and to the page.
inline DecodeResult decode(const uint8_t*, int64_t, const char*, Recording&) {
  return DecodeResult::Unavailable;
}

#elif defined(CULEBRA_AUDIO_DEVICE)

// Strong bodies in src/runtime/culebra_rt_audio.cc.
bool available();
// The output's period: asked for before the device opens (false once it has),
// and the one it runs at in seconds, NaN when it is not open.
bool latency_ask(double seconds);
double latency();
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
// MIDI (vendor/cpp-midilib), on the device's backend: the inputs (or
// outputs) there are, each its id and then its name; an input opened under
// handle `id`, getting sysexes when `sysex` (false when there is no `port`);
// an output the same way; either closed; what came since the last read, six
// numbers a message (seconds ago, the status and two data bytes, the size,
// and a note on's 14-bit velocity, 0 for the rest), and for a sysex
// (status 240) its size in place of the data bytes, then its bytes; a wait
// for one; a controller's 14-bit value as the messages read have set it; how
// many were dropped; and bytes sent to an output, answering what is wrong
// with them ("" when they were sent).
std::vector<std::string> midi_ports(bool output);
bool midi_open(int64_t id, const std::string& port, bool sysex);
bool midi_out_open(int64_t id, const std::string& port);
void midi_free(int64_t id);
std::string midi_send(int64_t id, const std::vector<uint8_t>& bytes);
void midi_read(int64_t id, std::vector<double>& out);
bool midi_wait(int64_t id, double seconds);
int64_t midi_control(int64_t id, int64_t channel, int64_t cc);
int64_t midi_dropped(int64_t id);
void schedule_new(int64_t id, const double* events, int64_t count, double length,
               double loop_at);
void schedule_free(int64_t id);
void schedule_play(int64_t id);
void schedule_stop(int64_t id);
void schedule_volume(int64_t id, double v);
// How much of the room the schedule plays in is heard with it, 0 for none.
void schedule_room(int64_t id, double wet);
// A schedule with its Sounds to come (see kScheduleStride above), a slot
// filled, and the seconds it has held since it last started.
bool schedule_open(int64_t id, const double* events, int64_t count, int64_t slots, double length,
                   double loop_at);
void schedule_fill(int64_t id, int64_t slot, int64_t sound);
double schedule_held(int64_t id);
// A schedule (schedule_new's list and length) rendered into `out`, which arrives
// empty, as the stream would play it once through at `volume` in a room heard
// `wet` strong: the left at kMixerRate into `out` (and the right into
// `right` unless it is null), from `from` seconds on until its last
// sound and its room's echoes end. It is mixed from the top all the same, so
// what sounds from `from` on is what the whole schedule sounds there. Its Sounds
// are decoded from `sounds` as sound_load decodes them, so it needs no
// device. False where there is no mixer to render with.
bool schedule_render(const double* events, int64_t count, double length, double wet,
                  double volume, double from, const std::vector<SoundBytes>& sounds,
                  std::vector<float>& out, std::vector<float>* right);
// A recording's bytes (`fmt` sound_format's) read with the decoders a Sound
// plays through, as floats at its own rate and channels. Needs no device.
DecodeResult decode(const uint8_t* data, int64_t len, const char* fmt, Recording& out);
// A graph played: a copy the mixer works out from the next sample on, into
// `into` (null for the device), the graphs playing then worked out in
// `order` (graph_order's); the changes scheduled after that to the copy's
// parts are sent on to it (a part added since is the next play()'s).
// graph_now answers where on the graph's clock a change made now lands, NaN
// when it is not playing.
void graph_play(int64_t id, std::shared_ptr<Bus> into, const std::vector<int64_t>& order);
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
CULEBRA_RT_AUDIO_LINKAGE bool latency_ask(double) { return true; }
CULEBRA_RT_AUDIO_LINKAGE double latency() { return std::nan(""); }
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
CULEBRA_RT_AUDIO_LINKAGE void schedule_new(int64_t, const double*, int64_t, double,
                                        double) {}
CULEBRA_RT_AUDIO_LINKAGE void schedule_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void schedule_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void schedule_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void schedule_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void schedule_room(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE bool schedule_open(int64_t, const double*, int64_t, int64_t, double,
                                            double) {
  return false;
}
CULEBRA_RT_AUDIO_LINKAGE void schedule_fill(int64_t, int64_t, int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE double schedule_held(int64_t) { return 0.0; }
CULEBRA_RT_AUDIO_LINKAGE bool schedule_render(const double*, int64_t, double, double,
                                           double, double, const std::vector<SoundBytes>&,
                                           std::vector<float>&, std::vector<float>*) {
  return false;
}
CULEBRA_RT_AUDIO_LINKAGE DecodeResult decode(const uint8_t*, int64_t, const char*,
                                             Recording&) {
  return DecodeResult::Unavailable;
}
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

// MIDI input, the same: no inputs, and none opens.
#if !defined(CULEBRA_AUDIO_DEVICE)
CULEBRA_RT_AUDIO_LINKAGE std::vector<std::string> midi_ports(bool) { return {}; }
CULEBRA_RT_AUDIO_LINKAGE bool midi_open(int64_t, const std::string&, bool) { return false; }
CULEBRA_RT_AUDIO_LINKAGE bool midi_out_open(int64_t, const std::string&) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void midi_free(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE std::string midi_send(int64_t, const std::vector<uint8_t>&) { return "the output is closed"; }
CULEBRA_RT_AUDIO_LINKAGE void midi_read(int64_t, std::vector<double>&) {}
CULEBRA_RT_AUDIO_LINKAGE bool midi_wait(int64_t, double) { return false; }
CULEBRA_RT_AUDIO_LINKAGE int64_t midi_control(int64_t, int64_t, int64_t) { return 0; }
CULEBRA_RT_AUDIO_LINKAGE int64_t midi_dropped(int64_t) { return 0; }
#endif

// A graph renders everywhere, and plays on a device alone: the browser has no
// backend for one yet.
#if !defined(CULEBRA_AUDIO_DEVICE)
CULEBRA_RT_AUDIO_LINKAGE void graph_play(int64_t, std::shared_ptr<Bus>, const std::vector<int64_t>&) {}
CULEBRA_RT_AUDIO_LINKAGE void graph_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void graph_post(int64_t, int, int, ParamEvent) {}
CULEBRA_RT_AUDIO_LINKAGE double graph_now(int64_t) { return std::nan(""); }
#endif

#undef CULEBRA_RT_AUDIO_LINKAGE

}  // namespace _audio_detail
}  // namespace culebra
