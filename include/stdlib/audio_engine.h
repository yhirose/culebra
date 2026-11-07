#pragma once

// The mixer the Audio namespace plays its tones, its schedules (Kauai's songs)
// and its graphs through, apart from any device: what a backend drives. A
// native build drives it from raylib's stream callback (culebra_rt_audio.cc);
// a schedule rendered to samples (render_schedule) runs a Mixer of its own
// from sample 0; test/audio_engine_test.cc drives it as the device would, in
// buffers of any size, and holds it to the offline render sample for sample.
//
// Two threads share an Engine: the main thread, which asks for what plays,
// and the audio thread, which renders it a buffer at a time (render()). What
// the main thread asks for waits in a handover under the Engine's lock, which
// the audio thread takes at the start of each buffer and never holds across
// the render. Nothing on the audio thread allocates or frees: what it would
// free comes back to the main thread to be freed there.

#include <stdlib/audio.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace culebra {
namespace _audio_detail {

// --- tone: a small WASM-4-style APU, mixed in software ----------------------
//
// The browser side (playground/app.js's playTone) hands each note to WebAudio,
// which owns its own envelope/oscillator machinery; native has none of that,
// so the Mixer below IS the synth. Five channels (two pulse, a triangle,
// noise, and the culebra-only sawtooth), one note at a time each: a new tone()
// on a channel cuts whatever was still playing there, like the real APU. Times
// arrive in frames at ~60 fps and become sample counts once, at note-start.
inline constexpr int kSampleRate = 44100;
static_assert(kSampleRate == kMixerRate);
// Channel numbers match src/preambles/audio.cul's PULSE/PULSE2/TRIANGLE/
// NOISE/SAWTOOTH. 0 and 1 (the two pulse channels) fall to oscillate()'s
// default case: only their duty cycle differs, not their shape.
inline constexpr int kTriangle = 2, kNoise = 3, kSawtooth = 4;

struct Room;

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
  int64_t owner = 0;          // the schedule that started it; 0 for tone()
  Room* room = nullptr;       // the room its schedule plays in
};

// A note carries where it starts, on the stream's own sample clock. tone()
// runs on the main thread while the mixer renders whole buffers at once, so
// writing straight to the sounding voice keeps only the last note a buffer
// spans and pins every onset to a buffer edge. This is what WebAudio's own
// src.start(now) gives the browser side.
struct Queued {
  int64_t start = 0;   // absolute position in the stream, in samples
  int channel = 0;
  Note note;
};

// Bounded on both sides of the handover: a device that stalls must not grow
// these without limit, and the oldest note is the one to lose, being the one a
// monophonic channel would have cut anyway.
inline constexpr int kQueueCap = 64;

// tone's vol/peak arrive as WASM-4's 0..100. A synthesised waveform is raw and
// up to four channels stack, so tone keeps a headroom the file paths don't
// need: kToneLevel, the browser's "keep it gentle" 0.2.
inline double tone_gain_of(double v) {
  return std::clamp(v, 0.0, 100.0) / 100.0 * kToneLevel;
}

// The ADSR envelope's gain at `elapsed` samples into a note of the given phase
// lengths (all in samples): attack ramps 0->peak, decay peak->sustain, sustain
// holds, release ramps sustain->0.
inline double adsr(int64_t elapsed, int64_t attack, int64_t decay, int64_t sustain,
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
// advancing the phase by one sample's worth of its (linearly swept) frequency.
// Naive (non-band-limited) waves: fine at chiptune-bleep durations and
// volumes, and it keeps the mixer simple.
inline double oscillate(Note& n, int channel, double freq) {
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

// A note sliding `start_freq` -> `end_freq` through its phases (in samples),
// at tone's vol and peak (0..100).
inline Note make_note(double start_freq, double end_freq, int64_t attack, int64_t decay,
                      int64_t sustain, int64_t release, double vol, double peak,
                      int64_t duty) {
  Note n;
  n.active = true;
  n.start_freq = std::max(1.0, start_freq);
  n.end_freq = std::max(1.0, end_freq);
  n.attack = attack;
  n.decay = decay;
  n.sustain = sustain;
  n.release = release;
  n.total = n.attack + n.decay + n.sustain + n.release;
  if (n.total <= 0) n.total = 1;  // guarantee an audible blip, like the browser
  n.vol = tone_gain_of(vol);
  n.peak = tone_gain_of(peak);
  static constexpr double kDutyCycles[4] = {0.125, 0.25, 0.5, 0.75};
  n.duty = kDutyCycles[std::clamp<int64_t>(duty, 0, 3)];
  return n;
}

// --- Schedule: a song's notes, on the stream's own clock ---------------------
//
// Kauai hands a whole song over at once, every note already timed, and
// the mixer below starts each one on its sample: the music keeps time however
// long a frame runs, and plays in a program with no frame loop. A note is a
// tone on one of the channels, or a sample (an Audio.Sound's decoded PCM)
// mixed in alongside them, as many at once as the song asks for. A song can
// start before its samples are all made (open_schedule): it waits for one
// that has not come, as a video waits for its next frames.
// The room a schedule plays in (Freeverb: eight damped combs in parallel, then
// four allpasses in series, at Jezar's delays and gain), heard `wet` strong
// alongside the schedule's own sound. The schedule's tones and samples add into
// `send` as they render, and step() runs the room one sample on. It is a
// schedule's own, so a song's echoes are its own, and ring on after its last
// sound. Audio thread only, but for `wet`.
struct Room {
  static constexpr int kCombs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  static constexpr int kPasses[4] = {556, 441, 341, 225};
  static constexpr double kFeedback = 0.86, kDamp = 0.25, kInput = 0.015;
  // Samples it rings on after the last of its sound: some 90 dB down.
  static constexpr int64_t kTail = 2 * kSampleRate;

  std::atomic<double> wet{0.0};
  std::vector<float> combs[8];
  std::vector<float> passes[4];
  float damped[8] = {};
  size_t comb_at[8] = {};
  size_t pass_at[4] = {};
  double send = 0;
  int64_t quiet = kTail;  // samples since `send` last had sound in it

  Room() {
    for (int k = 0; k < 8; k++) combs[k].assign(kCombs[k], 0.0f);
    for (int k = 0; k < 4; k++) passes[k].assign(kPasses[k], 0.0f);
  }
  // Silent again, for a schedule played from the top.
  void reset() {
    for (int k = 0; k < 8; k++) {
      std::ranges::fill(combs[k], 0.0f);
      damped[k] = 0;
      comb_at[k] = 0;
    }
    for (int k = 0; k < 4; k++) {
      std::ranges::fill(passes[k], 0.0f);
      pass_at[k] = 0;
    }
    send = 0;
    quiet = kTail;
  }
  bool ringing() const { return quiet < kTail; }
  // The echoes this sample, of `send` and what came before it.
  double step() {
    double x = send;
    send = 0;
    double w = wet.load(std::memory_order_relaxed);
    quiet = w <= 0 ? kTail : x != 0 ? 0 : std::min(quiet + 1, kTail);
    if (!ringing()) return 0;
    double into = x * kInput;
    double echo = 0;
    for (int k = 0; k < 8; k++) {
      auto& buf = combs[k];
      double y = buf[comb_at[k]];
      damped[k] = static_cast<float>(y * (1.0 - kDamp) + damped[k] * kDamp);
      buf[comb_at[k]] = static_cast<float>(into + damped[k] * kFeedback);
      comb_at[k] = (comb_at[k] + 1) % buf.size();
      echo += y;
    }
    for (int k = 0; k < 4; k++) {
      auto& buf = passes[k];
      double held = buf[pass_at[k]];
      buf[pass_at[k]] = static_cast<float>(echo + held * 0.5);
      pass_at[k] = (pass_at[k] + 1) % buf.size();
      echo = held - echo;
    }
    return echo * w;
  }
};

struct ScheduleEvent {
  int64_t at = 0;    // samples from the schedule's start
  int channel = -1;  // a tone channel, or -1 for a sample
  Note note;         // a tone
  size_t slot = 0;   // a sample's: the schedule's slot it plays
  double gain = 0;   // a sample's
};

using Pcm = std::shared_ptr<const std::vector<float>>;  // a sample, mono at kSampleRate

// A sound a schedule plays, by the slot its samples name. A schedule made
// with a slot empty (open_schedule) has it filled later, on the main thread,
// while the mixer may be playing it: the main thread keeps the samples, and
// publishes them to the mixer by `pcm`.
struct Slot {
  Pcm kept;                                         // main thread
  std::atomic<const std::vector<float>*> pcm{nullptr};  // kept's, once it is filled
};

struct Schedule {
  std::vector<ScheduleEvent> events;  // ordered by `at`
  std::vector<Slot> slots;         // sized when it is made, never after
  int64_t length = 0;              // samples in one time through
  int64_t loop_at = -1;            // where a loop goes back to; -1 plays once
  std::atomic<double> volume{1.0};
  std::unique_ptr<Room> room = std::make_unique<Room>();
  // Samples it has held since it last started, waiting for a slot (Mixer::advance).
  std::atomic<int64_t> held{0};

  // Main thread: slot `k` filled, from the mixer's next sample on; a slot is
  // filled once.
  void fill(size_t k, Pcm samples) {
    assert(samples);
    if (k >= slots.size() || slots[k].kept) return;
    slots[k].kept = std::move(samples);
    slots[k].pcm.store(slots[k].kept.get(), std::memory_order_release);
  }
};

// A schedule from Kauai's list (kScheduleStride), each sample's row naming
// the slot `slot_of(the row's third number)` answers, the row left out when
// it answers none. The caller gives it its slots.
template <class SlotOf>
std::shared_ptr<Schedule> schedule_of(const double* events, int64_t count, double length,
                                      double loop_at, SlotOf slot_of) {
  auto samples = [](double seconds) {
    return static_cast<int64_t>(std::max(0.0, seconds) * kSampleRate);
  };
  auto s = std::make_shared<Schedule>();
  for (int64_t i = 0; i + kScheduleStride <= count; i += kScheduleStride) {
    const double* e = events + i;
    ScheduleEvent ev;
    ev.at = samples(e[0]);
    ev.channel = static_cast<int>(e[1]);
    if (ev.channel < 0) {
      auto slot = slot_of(e[2]);
      if (!slot) continue;
      ev.slot = *slot;
      ev.gain = std::max(0.0, e[3]);
    } else {
      if (ev.channel > 4) continue;
      ev.note = make_note(e[2], e[3], samples(e[4]), samples(e[5]), samples(e[6]),
                          samples(e[7]), e[8], e[9], static_cast<int64_t>(e[10]));
    }
    s->events.push_back(std::move(ev));
  }
  s->length = samples(length);
  s->loop_at = loop_at < 0 ? -1 : samples(loop_at);
  return s;
}

// A schedule from Kauai's list, a Sound's samples found by its handle in
// `pcm` (one it does not hold, which the device could not decode, is left
// out): a slot for each, filled.
using PcmByHandle = std::unordered_map<int64_t, Pcm>;
inline std::shared_ptr<Schedule> build_schedule(const double* events, int64_t count, double length,
                                                double loop_at, const PcmByHandle& pcm) {
  std::unordered_map<int64_t, size_t> slot_by_handle;
  std::vector<Pcm> filled;
  auto s = schedule_of(events, count, length, loop_at,
                       [&](double handle) -> std::optional<size_t> {
                         auto it = pcm.find(static_cast<int64_t>(handle));
                         if (it == pcm.end()) return std::nullopt;
                         auto [entry, added] = slot_by_handle.try_emplace(it->first, filled.size());
                         if (added) filled.push_back(it->second);
                         return entry->second;
                       });
  s->slots = std::vector<Slot>(filled.size());
  for (size_t k = 0; k < filled.size(); k++) s->fill(k, filled[k]);
  return s;
}

// A schedule from Kauai's list whose samples' rows name a slot, 0 to `slots`
// - 1 (a row naming none is left out), every slot empty: it plays as its
// sounds come, holding where one has not (Mixer::advance).
inline std::shared_ptr<Schedule> open_schedule(const double* events, int64_t count, int64_t slots,
                                               double length, double loop_at) {
  size_t n = static_cast<size_t>(std::max<int64_t>(0, slots));
  auto s = schedule_of(events, count, length, loop_at, [n](double k) -> std::optional<size_t> {
    if (!(k >= 0 && k < static_cast<double>(n))) return std::nullopt;
    return static_cast<size_t>(k);
  });
  s->slots = std::vector<Slot>(n);
  return s;
}

// A schedule being played, and a sample sounding. Audio thread only. The audio
// thread never drops a schedule's last reference (see Engine::retired_), so it
// frees nothing: a sample plays its schedule's PCM, and the schedule plays on
// until its samples end.
struct PlayingSchedule {
  int64_t id = 0;
  std::shared_ptr<Schedule> schedule;
  int64_t origin = 0;  // the stream sample this time through starts on
  size_t next = 0;     // the next event to start
};
struct Sampler {
  int64_t owner = 0;
  std::span<const float> pcm;  // the owner's
  size_t pos = 0;
  double gain = 0;
  Room* room = nullptr;        // the owner's
  int fade = -1;               // samples left of the fade it goes with once stolen; -1 before
};
// The samples sounding at once; past them the oldest goes, as a channel's note
// would, fading out over kStealFade samples (some 3 ms), as a cut would click.
inline constexpr size_t kMaxSamplers = 64;
inline constexpr int kStealFade = 128;
// The mixer's room for samplers, those fading out too: past it, one already
// fading goes at once, which takes as many steals inside one fade.
inline constexpr size_t kSamplerSlots = 2 * kMaxSamplers;

// The mixing: the voice sounding on each channel, the schedules playing and the
// samples they started, and the noise channel's generator. A stream has one,
// which its audio thread alone touches; a schedule rendered to samples
// (render_schedule) gets one of its own, so it renders as it plays, and the same
// every time.
struct Mixer {
  Note voice[5];
  std::vector<PlayingSchedule> playing;
  std::vector<Sampler> samplers;
  uint32_t rng = 0x9e3779b9u;

  Mixer() { samplers.reserve(kSamplerSlots); }  // start() never grows it

  // Start what schedule `ps` has due at sample `p`, and go round again at its
  // end if it loops; false once it has played through and its sounds ended.
  bool advance(PlayingSchedule& ps, int64_t p);
  // Silence what schedule `id` started: its tones on their channels, its samples.
  void cut(int64_t id);
  // A sample starting, stealing from the oldest past kMaxSamplers.
  void start(const Sampler& s);
  // Sample `p`: the schedules' notes due then started, the samples and the
  // channels sounding, `extra`, and the rooms' echoes. Not yet clamped.
  double sample(int64_t p, double extra);
  // The noise channel's next sample, from the mixer's own generator.
  double noise(Note& n, double cutoff);
};

inline bool Mixer::advance(PlayingSchedule& ps, int64_t p) {
  Schedule& s = *ps.schedule;
  for (;;) {
    // A sample due whose slot is still empty holds the schedule: its clock
    // stands still, nothing due starts until the slot fills (so what starts
    // together still does), and what sounds already sounds on.
    for (size_t k = ps.next; k < s.events.size() && ps.origin + s.events[k].at <= p; k++) {
      const ScheduleEvent& e = s.events[k];
      if (e.channel < 0 && !s.slots[e.slot].pcm.load(std::memory_order_acquire)) {
        ps.origin++;
        s.held.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
    while (ps.next < s.events.size() && ps.origin + s.events[ps.next].at <= p) {
      const ScheduleEvent& e = s.events[ps.next++];
      double v = s.volume.load(std::memory_order_relaxed);
      if (e.channel >= 0) {
        Note n = e.note;
        n.vol *= v;
        n.peak *= v;
        n.owner = ps.id;
        n.room = s.room.get();
        voice[e.channel] = n;
      } else {
        start(Sampler{ps.id, *s.slots[e.slot].pcm.load(std::memory_order_acquire), 0, e.gain * v,
                      s.room.get()});
      }
    }
    if (p < ps.origin + s.length) return true;
    if (s.loop_at < 0 || s.length <= s.loop_at) {
      // Played through, it goes once nothing it started sounds, its room too.
      return s.room->ringing() ||
             std::ranges::any_of(samplers, [&](const Sampler& x) { return x.owner == ps.id; }) ||
             std::any_of(voice, voice + 5, [&](const Note& n) { return n.active && n.owner == ps.id; });
    }
    ps.origin += s.length - s.loop_at;
    ps.next = std::lower_bound(s.events.begin(), s.events.end(), s.loop_at,
                               [](const ScheduleEvent& e, int64_t at) { return e.at < at; }) -
              s.events.begin();
  }
}

inline void Mixer::start(const Sampler& s) {
  auto sounding = [](const Sampler& x) { return x.fade < 0; };
  if (static_cast<size_t>(std::ranges::count_if(samplers, sounding)) == kMaxSamplers)
    std::ranges::find_if(samplers, sounding)->fade = kStealFade;
  // Full, the samplers are kMaxSamplers sounding at most and the rest fading.
  if (samplers.size() == kSamplerSlots)
    samplers.erase(std::ranges::find_if(samplers, [&](const Sampler& x) { return !sounding(x); }));
  assert(samplers.size() < samplers.capacity());
  samplers.push_back(s);
}

inline void Mixer::cut(int64_t id) {
  for (int c = 0; c < 5; c++)
    if (voice[c].owner == id) voice[c].active = false;
  std::erase_if(samplers, [id](const Sampler& s) { return s.owner == id; });
}

// Noise: a cheap xorshift PRNG through a one-pole lowpass that sweeps
// start->end (times 8, matching the browser's filter sweep), so it reads as a
// pitched hiss rather than flat static. The generator is the mixer's, so a
// rendered schedule's noise is the same every time.
inline double Mixer::noise(Note& n, double cutoff) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  double white = (static_cast<double>(rng) / static_cast<double>(0xffffffffu)) * 2.0 - 1.0;
  double nyq = kSampleRate / 2.0;
  double alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * std::min(cutoff, nyq * 0.99) / kSampleRate);
  n.lp_state += alpha * (white - n.lp_state);
  return n.lp_state;
}

inline double Mixer::sample(int64_t p, double extra) {
  std::erase_if(playing, [&](PlayingSchedule& ps) { return !advance(ps, p); });
  double mixed = 0.0;
  for (auto& s : samplers) {
    if (s.pos >= s.pcm.size()) continue;
    double v = s.pcm[s.pos++] * s.gain;
    if (s.fade >= 0) v *= static_cast<double>(s.fade--) / kStealFade;
    mixed += v;
    if (s.room) s.room->send += v;
  }
  std::erase_if(samplers, [](const Sampler& s) { return s.pos >= s.pcm.size() || s.fade == 0; });
  for (int c = 0; c < 5; c++) {
    Note& n = voice[c];
    if (!n.active || n.elapsed >= n.total) {
      n.active = false;
      continue;
    }
    double t = n.total > 0 ? static_cast<double>(n.elapsed) / n.total : 1.0;
    double freq = n.start_freq + (n.end_freq - n.start_freq) * t;
    double g = adsr(n.elapsed, n.attack, n.decay, n.sustain, n.release, n.peak, n.vol);
    double raw = c == kNoise ? noise(n, std::max(1.0, freq * 8.0))
                              : oscillate(n, c, std::max(1.0, freq));
    mixed += raw * g;
    if (n.room) n.room->send += raw * g;
    n.elapsed++;
  }
  mixed += extra;
  for (auto& ps : playing) mixed += ps.schedule->room->step();
  return mixed;
}

// A schedule (schedule_new's list and length) played once through by a mixer
// of its own, at `volume` in a room heard `wet` strong: from sample 0 until
// it lets go of it (its last sound and its room's echoes over), as the stream
// would, keeping the samples from `from` seconds on.
inline void render_schedule(const double* events, int64_t count, double length, double wet,
                            double volume, double from, const PcmByHandle& pcm,
                            std::vector<float>& out) {
  auto schedule = build_schedule(events, count, length, -1, pcm);
  // Every slot filled, so it never holds: held, it would never end.
  assert(std::ranges::all_of(schedule->slots,
                             [](const Slot& slot) { return slot.kept != nullptr; }));
  schedule->volume.store(std::max(0.0, volume));
  schedule->room->wet.store(std::max(0.0, wet));
  Mixer m;
  m.playing.push_back(PlayingSchedule{1, std::move(schedule), 0, 0});
  const int64_t first = std::llround(std::max(0.0, from) * kSampleRate);
  for (int64_t p = 0; !m.playing.empty(); p++) {
    double v = m.sample(p, 0.0);
    if (p >= first) out.push_back(static_cast<float>(std::clamp(v, -1.0, 1.0)));
  }
}

// --- Graph: parts the mixer works out as they play -------------------------
//
// Playing a graph hands the mixer a copy of it, which the audio thread alone
// then works out; changes scheduled after that to the values of the parts
// it was played with reach the copy through the posts. Parts added, and a new
// output, wait for the next play(). The main thread keeps a reference to each
// copy (in live_graphs_, then retired_graphs_) so that, as with schedules, the
// free happens there. The audio thread's list of them grows by a longer list
// the main thread sends (LongerList), as its list of the schedules playing
// does; a copy's values have room for the changes made while it plays
// (graph_post sends a longer list before they could run out); and the posts,
// and the lists they bring back, are freed on the main thread.
struct GraphCommand {
  int64_t id = 0;
  std::shared_ptr<Graph> graph;  // play; null stops
  int64_t start = 0;             // play: the stream sample it starts on
};
struct GraphPost {
  std::shared_ptr<Graph> graph;
  int part = 0, value = 0;
  ParamEvent e;
  // A longer list for the value, when its own might not hold the change; it
  // comes back holding the old one.
  std::vector<ParamEvent> longer;
};
struct PlayingGraph {
  int64_t id = 0;
  std::shared_ptr<Graph> graph;
  int64_t origin = 0;  // the stream sample it starts on
};
struct LiveGraph {
  std::shared_ptr<Graph> graph;
  int64_t origin = 0;
  size_t parts = 0;  // the copy's, which a change's part must be one of
  std::vector<size_t> room;  // how many changes each value of the copy holds, two a part
};

// Asked for on the main thread, carried out at the next buffer.
struct ScheduleCommand {
  int64_t id = 0;
  std::shared_ptr<Schedule> schedule;  // play; null stops
  int64_t start = 0;             // play: the stream sample the schedule starts on
};

// The samples the mixer asks for at once, at most; a longer ask is split.
inline constexpr unsigned kMixChunk = 4096;

// A list the audio thread adds to (the schedules or the graphs playing),
// kept from ever growing there by the handover graph_post's longer lists use:
// the main thread, which knows how long the list can get, sends a longer one
// before it could fill; the audio thread moves into it and sends its old one
// back, which the main thread frees.
template <class T>
class LongerList {
 public:
  // Main thread: a list with room for `n` when the audio thread's might not
  // have it, to send(); empty when it has.
  std::vector<T> longer_for(size_t n) {
    std::vector<T> longer;
    if (n <= room_) return longer;
    longer.reserve(2 * n);
    room_ = longer.capacity();
    return longer;
  }
  // Main thread, under the lock: `longer` sent, coming back holding what it
  // displaces (one sent and not yet taken, or one come back), for the caller
  // to free once the lock is let go.
  void send(std::vector<T>& longer) {
    if (longer.capacity() == 0) return;
    sent_.swap(longer);
    back_ = false;
  }
  // Audio thread, under the lock: `items` moved into the list sent, if one
  // was, which stands in for it from now on; the old one goes back.
  void take(std::vector<T>& items) {
    if (back_ || sent_.capacity() == 0) return;
    for (auto& x : items) sent_.push_back(std::move(x));
    items.swap(sent_);
    sent_.clear();
    back_ = true;
  }
  // Main thread, under the lock: the old list into `old`, once it has come
  // back, for the caller to free once the lock is let go.
  void reclaim(std::vector<T>& old) {
    if (!back_) return;
    old.swap(sent_);
    back_ = false;
  }

 private:
  std::vector<T> sent_;  // guarded by the lock: a longer list sent, or the old one back
  bool back_ = false;    // guarded by the lock: `sent_` is the old one
  size_t room_ = 0;      // main thread: what the audio thread's list holds once it takes sent_
};

// --- Engine: the live mixer and what the main thread hands it ---------------
//
// One per stream. The main thread's half (tone, the schedule_ and graph_
// calls, release_retired) asks; the audio thread's half (render) plays. The
// clock dates what the main thread asks for against the buffers rendered: a
// device's driver passes the monotonic clock, a test one it moves by hand.
class Engine {
 public:
  using Clock = std::chrono::steady_clock;
  explicit Engine(Clock::time_point (*now)() = [] { return Clock::now(); }) : now_(now) {}
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // --- main thread ---

  // Before the device can call back: the stream clock dated, so a tone()
  // issued ahead of the first buffer is not measured from the epoch and
  // scheduled past any sample this process will ever render.
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_at_ = now_();
  }

  // A note on `channel` from the next sample the stream can start it on.
  void tone(int channel, const Note& n) {
    if (channel < 0 || channel > 4) return;
    release_retired();
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbox_count_ == kQueueCap) {   // see kQueueCap: the oldest is the loss
      std::move(inbox_ + 1, inbox_ + kQueueCap, inbox_);
      inbox_count_--;
    }
    // The queue is walked head-first, and a sequencer never means to place a
    // note behind one it has already asked for, so clock jitter across a buffer
    // edge settles in favour of the order the calls came in.
    int64_t start = std::max(stream_now(), last_start_);
    last_start_ = start;
    inbox_[inbox_count_++] = Queued{start, channel, n};
  }

  void schedule_new(int64_t id, std::shared_ptr<Schedule> s) {
    release_retired();
    schedules_[id] = std::move(s);
  }
  void schedule_free(int64_t id) {
    auto it = schedules_.find(id);
    if (it == schedules_.end()) return;
    schedule_stop(id);
    retired_.push_back(std::move(it->second));
    schedules_.erase(it);
    release_retired();
  }
  void schedule_play(int64_t id) {
    release_retired();
    auto it = schedules_.find(id);
    if (it == schedules_.end()) return;
    // A schedule plays only while it is not freed (schedule_free stops it
    // first), so no more play at once than there are schedules.
    auto longer = playing_.longer_for(schedules_.size());
    std::lock_guard<std::mutex> lock(mutex_);
    playing_.send(longer);
    schedule_inbox_.push_back(ScheduleCommand{id, it->second, stream_now()});
  }
  void schedule_stop(int64_t id) {
    if (!schedules_.contains(id)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    schedule_inbox_.push_back(ScheduleCommand{id, nullptr, 0});
  }
  void schedule_volume(int64_t id, double v) {
    auto it = schedules_.find(id);
    if (it != schedules_.end()) it->second->volume.store(std::max(0.0, v));
  }
  void schedule_room(int64_t id, double wet) {
    auto it = schedules_.find(id);
    if (it != schedules_.end()) it->second->room->wet.store(std::max(0.0, wet));
  }
  // Slot `slot` of schedule `id` (open_schedule's) filled with `samples`.
  void schedule_fill(int64_t id, int64_t slot, Pcm samples) {
    auto it = schedules_.find(id);
    if (it != schedules_.end() && slot >= 0)
      it->second->fill(static_cast<size_t>(slot), std::move(samples));
  }
  // Samples schedule `id` has held since it last started; 0 for none.
  int64_t schedule_held(int64_t id) {
    auto it = schedules_.find(id);
    return it == schedules_.end() ? 0 : it->second->held.load(std::memory_order_relaxed);
  }

  // The copy of graph `id` (graph_def's) plays from the next sample the
  // stream can start it on.
  void graph_play(int64_t id) {
    const Graph* def = graph_def(id);
    if (!def) return;
    graph_stop(id);
    auto live = std::make_shared<Graph>(*def);
    std::vector<size_t> room;
    live->make_room(room);
    // No more play at once than are live (graph_stop stops one as it lets go).
    auto longer = graphs_playing_.longer_for(live_graphs_.size() + 1);
    std::lock_guard<std::mutex> lock(mutex_);
    graphs_playing_.send(longer);
    int64_t start = stream_now();
    live_graphs_[id] = LiveGraph{live, start, def->size(), std::move(room)};
    graph_inbox_.push_back(GraphCommand{id, std::move(live), start});
  }
  void graph_stop(int64_t id) {
    release_retired();
    auto it = live_graphs_.find(id);
    if (it == live_graphs_.end()) return;
    retired_graphs_.push_back(std::move(it->second.graph));
    live_graphs_.erase(it);
    std::lock_guard<std::mutex> lock(mutex_);
    graph_inbox_.push_back(GraphCommand{id, nullptr, 0});
  }
  // The definition holds every change the copy does and this one too, so its
  // count bounds what the copy's list must hold; past what that list can, the
  // change brings a longer one, twice as long.
  void graph_post(int64_t id, int part, int value, ParamEvent e) {
    release_retired();
    auto it = live_graphs_.find(id);
    if (it == live_graphs_.end() || static_cast<size_t>(part) >= it->second.parts) return;
    size_t need = graph_def(id)->changes(part, value);
    size_t& room = it->second.room[2 * static_cast<size_t>(part) + static_cast<size_t>(value)];
    std::vector<ParamEvent> longer;
    if (need > room) {
      room = 2 * need + kGraphLiveRoom;
      longer.reserve(room);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    graph_posts_.push_back(GraphPost{it->second.graph, part, value, e, std::move(longer)});
  }
  // Where on graph `id`'s clock a change made now lands, in seconds; NaN when
  // it is not playing.
  double graph_now(int64_t id) {
    auto it = live_graphs_.find(id);
    if (it == live_graphs_.end()) return std::nan("");
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<double>(stream_now() - it->second.origin) / kSampleRate;
  }

  // Called from the main thread's audio calls, so a freed song goes soon after
  // the mixer lets go of it, and the lists the mixer has outgrown with it.
  void release_retired() {
    std::vector<PlayingSchedule> old_playing;
    std::vector<PlayingGraph> old_graphs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      graph_posts_.erase(graph_posts_.begin(),
                         graph_posts_.begin() + static_cast<std::ptrdiff_t>(graph_posts_taken_));
      graph_posts_taken_ = 0;
      playing_.reclaim(old_playing);
      graphs_playing_.reclaim(old_graphs);
    }
    std::erase_if(retired_, [](const auto& s) { return s.use_count() == 1; });
    std::erase_if(retired_graphs_, [](const auto& g) { return g.use_count() == 1; });
  }

  // Everything let go, once the device can no longer call back.
  void clear() {
    mix_.playing.clear();
    mix_.samplers.clear();
    schedule_inbox_.clear();
    graphs_.clear();
    graph_inbox_.clear();
    graph_posts_.clear();
    graph_posts_taken_ = 0;
    live_graphs_.clear();
    retired_graphs_.clear();
    schedules_.clear();
    retired_.clear();
  }

  // --- audio thread ---

  // Fill `frames` mono float samples. The lock is held only to take the
  // handover, never across the render, so a long buffer cannot delay tone()
  // on the main thread. Everything the render touches after that belongs to
  // this thread alone.
  void render(float* out, unsigned int frames) {
    if (frames > kMixChunk) {
      render(out, kMixChunk);
      render(out + kMixChunk, frames - kMixChunk);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Longer lists first, so the commands below have room.
      playing_.take(mix_.playing);
      graphs_playing_.take(graphs_);
      // Losing the oldest keeps the newest, which is what a channel would be
      // sounding by the time the queue drained anyway.
      int drop = pending_count_ + inbox_count_ - kQueueCap;
      if (drop > 0) {
        drop = std::min(drop, pending_count_);
        std::move(pending_ + drop, pending_ + pending_count_, pending_);
        pending_count_ -= drop;
      }
      for (int i = 0; i < inbox_count_ && pending_count_ < kQueueCap; i++) {
        pending_[pending_count_++] = inbox_[i];
      }
      inbox_count_ = 0;
      // A schedule played again starts over; one stopped goes silent at once.
      for (auto& cmd : schedule_inbox_) {
        std::erase_if(mix_.playing, [&](const PlayingSchedule& ps) { return ps.id == cmd.id; });
        mix_.cut(cmd.id);
        if (cmd.schedule) {
          cmd.schedule->room->reset();
          cmd.schedule->held.store(0, std::memory_order_relaxed);
          assert(mix_.playing.size() < mix_.playing.capacity());  // LongerList's room
          mix_.playing.push_back(PlayingSchedule{cmd.id, std::move(cmd.schedule), cmd.start, 0});
        }
      }
      schedule_inbox_.clear();
      // A graph played again starts over; one stopped goes silent at once.
      for (auto& cmd : graph_inbox_) {
        std::erase_if(graphs_, [&](const PlayingGraph& pg) { return pg.id == cmd.id; });
        if (!cmd.graph) continue;
        assert(graphs_.size() < graphs_.capacity());  // LongerList's room
        graphs_.push_back(PlayingGraph{cmd.id, std::move(cmd.graph), cmd.start});
      }
      graph_inbox_.clear();
      // Each change has room in its copy (graph_post sees to it).
      for (size_t k = graph_posts_taken_; k < graph_posts_.size(); k++) {
        auto& post = graph_posts_[k];
        post.graph->schedule_in_place(post.part, post.value, post.e, post.longer);
      }
      graph_posts_taken_ = graph_posts_.size();
      stream_next_ = pos_ + frames;
      stream_at_ = now_();
    }

    // The graphs, each from the sample it starts on.
    std::fill(graph_mix_, graph_mix_ + frames, 0.0);
    for (auto& pg : graphs_) {
      int64_t skip = std::clamp<int64_t>(pg.origin - pos_, 0, frames);
      pg.graph->render(graph_samples_, frames - skip);
      for (int64_t i = skip; i < frames; i++) graph_mix_[i] += graph_samples_[i - skip];
    }

    int head = 0;   // pending_ is ordered by start, so only the head can be due
    for (unsigned int i = 0; i < frames; i++) {
      int64_t p = pos_ + static_cast<int64_t>(i);
      // A note whose sample has come round takes its channel, cutting whatever
      // was sounding there. One already behind (the device fell back) starts
      // here rather than being dropped.
      while (head < pending_count_ && pending_[head].start <= p) {
        mix_.voice[pending_[head].channel] = pending_[head].note;
        head++;
      }
      out[i] = static_cast<float>(std::clamp(mix_.sample(p, graph_mix_[i]), -1.0, 1.0));
    }

    std::move(pending_ + head, pending_ + pending_count_, pending_);
    pending_count_ -= head;
    pos_ += frames;
  }

 private:
  // The stream sample a note asked for now starts on: the next buffer's, and as
  // far into it as the clock has moved since that buffer was dated, so notes
  // keep their spacing however long a buffer is. Called under mutex_.
  int64_t stream_now() {
    double ahead = std::chrono::duration<double>(now_() - stream_at_).count();
    return stream_next_ + static_cast<int64_t>(ahead * kSampleRate);
  }

  Clock::time_point (*now_)();
  std::mutex mutex_;

  // The handover, guarded by mutex_.
  Queued inbox_[kQueueCap];   // handed over by tone(), drained once per buffer
  int inbox_count_ = 0;
  // Where the stream stands once the buffer being rendered is done, and the
  // instant that was. tone() dates itself against this pair, so notes keep their
  // spacing however long a buffer is.
  int64_t stream_next_ = 0;
  Clock::time_point stream_at_{};
  int64_t last_start_ = 0;    // notes are queued in order, never behind
  std::vector<ScheduleCommand> schedule_inbox_;
  std::vector<GraphCommand> graph_inbox_;
  std::vector<GraphPost> graph_posts_;
  size_t graph_posts_taken_ = 0;  // the posts the mixer is done with
  LongerList<PlayingSchedule> playing_;      // for mix_.playing
  LongerList<PlayingGraph> graphs_playing_;  // for graphs_

  // The audio thread's. The notes still waiting for their sample (the voices
  // they start on are mix_'s).
  Queued pending_[kQueueCap];
  int pending_count_ = 0;
  int64_t pos_ = 0;           // samples rendered so far
  Mixer mix_;
  std::vector<PlayingGraph> graphs_;
  double graph_mix_[kMixChunk], graph_samples_[kMixChunk];

  // The main thread's: the schedules by handle, and the graphs' live copies.
  // A schedule holds its samples' PCM itself, so a Sound freed while a song
  // plays keeps sounding in it.
  std::unordered_map<int64_t, std::shared_ptr<Schedule>> schedules_;
  // Freed schedules the mixer may still hold, kept until this thread holds the
  // last reference, so that the free happens here.
  std::vector<std::shared_ptr<Schedule>> retired_;
  std::unordered_map<int64_t, LiveGraph> live_graphs_;
  std::vector<std::shared_ptr<Graph>> retired_graphs_;
};

}  // namespace _audio_detail
}  // namespace culebra
