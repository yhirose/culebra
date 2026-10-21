// Native backend for the `Audio` namespace: the one owner of the sound device,
// on raylib's audio module built standalone (culebra_raudio.c, raudio.h), so
// sound links neither raylib's core nor SDL. Canvas and Scene carry no audio
// of their own. The backend-neutral pieces (format sniff, handle ids, the
// silent and browser backends) are in include/stdlib/audio.h.
//
// Everything here runs on the main thread except three helpers raylib,
// miniaudio or this file starts: raylib's mixer thread (the tone synth's
// callback below), the music feeder, which takes g_music_mutex around every
// Music call, and a capture device's thread, which only fills that capture's
// ring (culebra_raudio.c).

#include "stdlib/audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "culebra_capture.h"
#include "raudio.h"

namespace culebra {
namespace _audio_detail {
namespace {

// --- the device ------------------------------------------------------------

// CULEBRA_AUDIO=off (or 0) keeps the device closed: a test suite, or a program
// run on a machine whose device should stay free, plays silently as if there
// were none. Read by value, like CULEBRA_CANVAS_HEADLESS.
bool audio_off() {
  static const bool off = [] {
    const char* v = std::getenv("CULEBRA_AUDIO");
    if (!v) return false;
    std::string_view s(v);
    return s == "0" || s == "off";
  }();
  return off;
}

bool g_ready = false;
bool g_failed = false;  // latch: don't retry InitAudioDevice every call

void arm_exit_teardown();

void ensure_device() {
  if (g_ready || g_failed || audio_off()) return;
  InitAudioDevice();
  if (!IsAudioDeviceReady()) {
    g_failed = true;
    // Sound is decorative, so no error; but say it once, or a silent latch
    // reads as "my calls are broken" on a machine without a device.
    std::fputs("WARNING: Audio: no audio device -- sound stays off\n", stderr);
    return;
  }
  g_ready = true;
  arm_exit_teardown();
}

// The capture context, apart from the device above so a program that only
// listens never opens an output. Latched and armed the same way.
bool g_capture_ready = false;
bool g_capture_failed = false;

bool ensure_capture() {
  if (g_capture_ready || g_capture_failed || audio_off()) return g_capture_ready;
  if (!culebra_capture_init()) {
    g_capture_failed = true;
    return false;
  }
  g_capture_ready = true;
  arm_exit_teardown();
  return true;
}

// A stream takes its sub-buffer size from a raylib global, which then sizes
// every stream loaded after it; put it back once this one is loaded.
AudioStream load_stream(int rate, int channels, int buffer_frames) {
  SetAudioStreamBufferSizeDefault(buffer_frames);
  AudioStream s = LoadAudioStream(static_cast<unsigned>(rate), 32,
                                  static_cast<unsigned>(channels));
  SetAudioStreamBufferSizeDefault(0);
  return s;
}

// --- tone: a small WASM-4-style APU, mixed in software ----------------------
//
// The browser side (playground/app.js's playTone) hands each note to WebAudio,
// which owns its own envelope/oscillator machinery; native has none of that,
// so the callback below IS the synth. Five channels (two pulse, a triangle,
// noise, and the culebra-only sawtooth), one note at a time each: a new tone()
// on a channel cuts whatever was still playing there, like the real APU. Times
// arrive in frames at ~60 fps and become sample counts once, at note-start.
constexpr int kSampleRate = 44100;
static_assert(kSampleRate == kMixerRate);
// Channel numbers match src/preambles/audio.cul's PULSE/PULSE2/TRIANGLE/
// NOISE/SAWTOOTH. 0 and 1 (the two pulse channels) fall to oscillate()'s
// default case: only their duty cycle differs, not their shape.
constexpr int kTriangle = 2, kNoise = 3, kSawtooth = 4;

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
  int64_t owner = 0;          // the score that started it; 0 for tone()
  Room* room = nullptr;       // the room its score plays in
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
constexpr int kQueueCap = 64;

std::mutex g_tone_mutex;
Queued g_inbox[kQueueCap];   // handed over by tone(), drained once per buffer
int g_inbox_count = 0;
// Where the stream stands once the buffer being rendered is done, and the
// instant that was. tone() dates itself against this pair, so notes keep their
// spacing however long a buffer is.
int64_t g_stream_next = 0;
std::chrono::steady_clock::time_point g_stream_at{};
int64_t g_last_start = 0;    // notes are queued in order, never behind

// The notes still waiting for their sample (the voices they start on are
// g_mix's, below). Read and written only on the audio thread.
Queued g_pending[kQueueCap];
int g_pending_count = 0;
int64_t g_pos = 0;           // samples rendered so far

// --- Score: a song's notes, on the stream's own clock ---------------------
//
// Audio.Kauai hands a whole song over at once, every note already timed, and
// the mixer below starts each one on its sample: the music keeps time however
// long a frame runs, and plays in a program with no frame loop. A note is a
// tone on one of the channels, or a sample (an Audio.Sound's decoded PCM)
// mixed in alongside them, as many at once as the song asks for.
// The room a score plays in (Freeverb: eight damped combs in parallel, then
// four allpasses in series, at Jezar's delays and gain), heard `wet` strong
// alongside the score's own sound. The score's tones and samples add into
// `send` as they render, and step() runs the room one sample on. It is a
// score's own, so a song's echoes are its own, and ring on after its last
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
  // Silent again, for a score played from the top.
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

struct ScoreEvent {
  int64_t at = 0;    // samples from the score's start
  int channel = -1;  // a tone channel, or -1 for a sample
  Note note;         // a tone
  std::shared_ptr<const std::vector<float>> pcm;  // a sample, mono at kSampleRate
  double gain = 0;   // a sample's
};

struct Score {
  std::vector<ScoreEvent> events;  // ordered by `at`
  int64_t length = 0;              // samples in one time through
  int64_t loop_at = -1;            // where a loop goes back to; -1 plays once
  std::atomic<double> volume{1.0};
  std::unique_ptr<Room> room = std::make_unique<Room>();
};

// Asked for on the main thread, carried out at the next buffer.
struct ScoreCommand {
  int64_t id = 0;
  std::shared_ptr<Score> score;  // play; null stops
  int64_t start = 0;             // play: the stream sample the score starts on
};

// A score being played, and a sample sounding. Audio thread only. The audio
// thread never drops a score's last reference (see g_retired), so it frees
// nothing: a sample plays its score's PCM, and the score plays on until its
// samples end.
struct PlayingScore {
  int64_t id = 0;
  std::shared_ptr<Score> score;
  int64_t origin = 0;  // the stream sample this time through starts on
  size_t next = 0;     // the next event to start
};
struct Sampler {
  int64_t owner = 0;
  std::span<const float> pcm;  // the owner's
  size_t pos = 0;
  double gain = 0;
  Room* room = nullptr;        // the owner's
};
constexpr size_t kMaxSamplers = 64;  // the oldest goes, as a channel's note would

// The mixing: the voice sounding on each channel, the scores playing and the
// samples they started, and the noise channel's generator. The stream has
// one, g_mix, which the audio thread alone touches; a score rendered to
// samples (score_render) gets one of its own, so it renders as it plays, and
// the same every time.
struct Mixer {
  Note voice[5];
  std::vector<PlayingScore> playing;
  std::vector<Sampler> samplers;
  uint32_t rng = 0x9e3779b9u;

  // Start what score `ps` has due at sample `p`, and go round again at its
  // end if it loops; false once it has played through and its sounds ended.
  bool advance(PlayingScore& ps, int64_t p);
  // Silence what score `id` started: its tones on their channels, its samples.
  void cut(int64_t id);
  // Sample `p`: the scores' notes due then started, the samples and the
  // channels sounding, `extra`, and the rooms' echoes. Not yet clamped.
  double sample(int64_t p, double extra);
  // The noise channel's next sample, from the mixer's own generator.
  double noise(Note& n, double cutoff);
};

std::vector<ScoreCommand> g_score_inbox;  // guarded by g_tone_mutex
Mixer g_mix;                              // audio thread

// The scores and the samples they can play, by handle. Main thread only; a
// score holds its samples' PCM itself, so a Sound freed while a song plays
// keeps sounding in it.
std::unordered_map<int64_t, std::shared_ptr<Score>> g_scores;
// Freed scores the mixer may still hold, kept until this thread holds the last
// reference, so that the free happens here.
std::vector<std::shared_ptr<Score>> g_retired;

// --- Graph: parts the mixer works out as they play -------------------------
//
// Playing a graph hands the mixer a copy of it, which the audio thread alone
// then works out; changes scheduled after that to the values of the parts
// it was played with reach the copy through g_graph_posts. Parts added, and
// a new output, wait for the next play(). The main thread keeps a reference to each copy (in
// g_live_graphs, then g_retired_graphs) so that, as with scores, the free
// happens here. Nothing on the audio thread allocates or frees: its list is
// reserved for kGraphsPlaying, a copy's values have room for the changes
// made while it plays (graph_post sends a longer list before they could run
// out), and the posts, and the lists they bring back, are freed here.
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
// The samples the mixer asks for at once, at most; a longer ask is split.
constexpr unsigned kMixChunk = 4096;

std::vector<GraphCommand> g_graph_inbox;               // guarded by g_tone_mutex
std::vector<GraphPost> g_graph_posts;                  // guarded by g_tone_mutex
size_t g_graph_posts_taken = 0;  // guarded by g_tone_mutex: the posts the mixer is done with
std::vector<PlayingGraph> g_playing_graphs;            // audio thread
double g_graph_mix[kMixChunk], g_graph_samples[kMixChunk];  // audio thread
std::unordered_map<int64_t, LiveGraph> g_live_graphs;  // main thread
std::vector<std::shared_ptr<Graph>> g_retired_graphs;  // main thread

// Called from the main thread's audio calls, so a freed song goes soon after
// the mixer lets go of it.
void release_retired() {
  {
    std::lock_guard<std::mutex> lock(g_tone_mutex);
    g_graph_posts.erase(g_graph_posts.begin(),
                        g_graph_posts.begin() + static_cast<std::ptrdiff_t>(g_graph_posts_taken));
    g_graph_posts_taken = 0;
  }
  std::erase_if(g_retired, [](const auto& s) { return s.use_count() == 1; });
  std::erase_if(g_retired_graphs, [](const auto& g) { return g.use_count() == 1; });
}
std::unordered_map<int64_t, std::shared_ptr<const std::vector<float>>> g_sound_pcm;

bool Mixer::advance(PlayingScore& ps, int64_t p) {
  const Score& s = *ps.score;
  for (;;) {
    while (ps.next < s.events.size() && ps.origin + s.events[ps.next].at <= p) {
      const ScoreEvent& e = s.events[ps.next++];
      double v = s.volume.load(std::memory_order_relaxed);
      if (e.channel >= 0) {
        Note n = e.note;
        n.vol *= v;
        n.peak *= v;
        n.owner = ps.id;
        n.room = s.room.get();
        voice[e.channel] = n;
      } else {
        if (samplers.size() == kMaxSamplers) samplers.erase(samplers.begin());
        samplers.push_back(Sampler{ps.id, *e.pcm, 0, e.gain * v, s.room.get()});
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
                               [](const ScoreEvent& e, int64_t at) { return e.at < at; }) -
              s.events.begin();
  }
}

void Mixer::cut(int64_t id) {
  for (int c = 0; c < 5; c++)
    if (voice[c].owner == id) voice[c].active = false;
  std::erase_if(samplers, [id](const Sampler& s) { return s.owner == id; });
}

AudioStream g_tone_stream;
bool g_tone_ready = false;

// tone's vol/peak arrive as WASM-4's 0..100. A synthesised waveform is raw and
// up to four channels stack, so tone keeps a headroom the file paths don't
// need: kToneLevel, the browser's "keep it gentle" 0.2.
double tone_gain_of(double v) {
  return std::clamp(v, 0.0, 100.0) / 100.0 * kToneLevel;
}

// The ADSR envelope's gain at `elapsed` samples into a note of the given phase
// lengths (all in samples): attack ramps 0->peak, decay peak->sustain, sustain
// holds, release ramps sustain->0.
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
// advancing the phase by one sample's worth of its (linearly swept) frequency.
// Naive (non-band-limited) waves: fine at chiptune-bleep durations and
// volumes, and it keeps the mixer simple.
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
// start->end (times 8, matching the browser's filter sweep), so it reads as a
// pitched hiss rather than flat static. The generator is the mixer's, so a
// rendered score's noise is the same every time.
double Mixer::noise(Note& n, double cutoff) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  double white = (static_cast<double>(rng) / static_cast<double>(0xffffffffu)) * 2.0 - 1.0;
  double nyq = kSampleRate / 2.0;
  double alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * std::min(cutoff, nyq * 0.99) / kSampleRate);
  n.lp_state += alpha * (white - n.lp_state);
  return n.lp_state;
}

double Mixer::sample(int64_t p, double extra) {
  std::erase_if(playing, [&](PlayingScore& ps) { return !advance(ps, p); });
  double mixed = 0.0;
  for (auto& s : samplers) {
    if (s.pos >= s.pcm.size()) continue;
    double v = s.pcm[s.pos++] * s.gain;
    mixed += v;
    if (s.room) s.room->send += v;
  }
  std::erase_if(samplers, [](const Sampler& s) { return s.pos >= s.pcm.size(); });
  for (int c = 0; c < 5; c++) {
    Note& n = voice[c];
    if (!n.active || n.elapsed >= n.total) {
      n.active = false;
      continue;
    }
    double t = n.total > 0 ? static_cast<double>(n.elapsed) / n.total : 1.0;
    double freq = n.start_freq + (n.end_freq - n.start_freq) * t;
    double g = envelope(n.elapsed, n.attack, n.decay, n.sustain, n.release, n.peak, n.vol);
    double raw = c == kNoise ? noise(n, std::max(1.0, freq * 8.0))
                              : oscillate(n, c, std::max(1.0, freq));
    mixed += raw * g;
    if (n.room) n.room->send += raw * g;
    n.elapsed++;
  }
  mixed += extra;
  for (auto& ps : playing) mixed += ps.score->room->step();
  return mixed;
}

// raylib's audio thread callback: fill `frames` mono float samples. The lock
// is held only to take the handover, never across the render, so a long buffer
// cannot delay tone() on the main thread. Everything the render touches after
// that belongs to this thread alone.
void tone_callback(void* buffer_data, unsigned int frames) {
  float* out = static_cast<float*>(buffer_data);
  if (frames > kMixChunk) {
    tone_callback(out, kMixChunk);
    tone_callback(out + kMixChunk, frames - kMixChunk);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_tone_mutex);
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
    // A score played again starts over; one stopped goes silent at once.
    for (auto& cmd : g_score_inbox) {
      std::erase_if(g_mix.playing, [&](const PlayingScore& ps) { return ps.id == cmd.id; });
      g_mix.cut(cmd.id);
      if (cmd.score) {
        cmd.score->room->reset();
        g_mix.playing.push_back(PlayingScore{cmd.id, std::move(cmd.score), cmd.start, 0});
      }
    }
    g_score_inbox.clear();
    // A graph played again starts over; one stopped goes silent at once.
    for (auto& cmd : g_graph_inbox) {
      std::erase_if(g_playing_graphs, [&](const PlayingGraph& pg) { return pg.id == cmd.id; });
      if (cmd.graph) g_playing_graphs.push_back(PlayingGraph{cmd.id, std::move(cmd.graph), cmd.start});
    }
    g_graph_inbox.clear();
    // Each change has room in its copy (graph_post sees to it).
    for (size_t k = g_graph_posts_taken; k < g_graph_posts.size(); k++) {
      auto& post = g_graph_posts[k];
      post.graph->schedule_in_place(post.part, post.value, post.e, post.longer);
    }
    g_graph_posts_taken = g_graph_posts.size();
    g_stream_next = g_pos + frames;
    g_stream_at = std::chrono::steady_clock::now();
  }

  // The graphs, each from the sample it starts on.
  std::fill(g_graph_mix, g_graph_mix + frames, 0.0);
  for (auto& pg : g_playing_graphs) {
    int64_t skip = std::clamp<int64_t>(pg.origin - g_pos, 0, frames);
    pg.graph->render(g_graph_samples, frames - skip);
    for (int64_t i = skip; i < frames; i++) g_graph_mix[i] += g_graph_samples[i - skip];
  }

  int head = 0;   // g_pending is ordered by start, so only the head can be due
  for (unsigned int i = 0; i < frames; i++) {
    int64_t p = g_pos + static_cast<int64_t>(i);
    // A note whose sample has come round takes its channel, cutting whatever
    // was sounding there. One already behind (the device fell back) starts
    // here rather than being dropped.
    while (head < g_pending_count && g_pending[head].start <= p) {
      g_mix.voice[g_pending[head].channel] = g_pending[head].note;
      head++;
    }
    out[i] = static_cast<float>(std::clamp(g_mix.sample(p, g_graph_mix[i]), -1.0, 1.0));
  }

  std::move(g_pending + head, g_pending + g_pending_count, g_pending);
  g_pending_count -= head;
  g_pos += frames;
}

void ensure_tone_stream() {
  ensure_device();
  if (!g_ready || g_tone_ready) return;
  g_tone_stream = load_stream(kSampleRate, 1, 0);  // 32-bit float, mono
  // The mixer adds to these as notes start, and must not allocate to do it.
  g_mix.playing.reserve(16);
  g_mix.samplers.reserve(kMaxSamplers);
  g_playing_graphs.reserve(kGraphsPlaying);
  SetAudioStreamCallback(g_tone_stream, tone_callback);
  // Date the stream clock before the device can call back: a tone() issued
  // ahead of the first buffer would otherwise be measured from the epoch and
  // scheduled past any sample this process will ever render.
  {
    std::lock_guard<std::mutex> lock(g_tone_mutex);
    g_stream_at = std::chrono::steady_clock::now();
  }
  PlayAudioStream(g_tone_stream);
  g_tone_ready = true;
}

// Frames (at ~60 fps) to samples, matching the browser's own `frames / 60`
// seconds (playground/app.js's F()).
int64_t frames_to_samples(int64_t frames) {
  return static_cast<int64_t>(std::max<int64_t>(0, frames) *
                              (static_cast<double>(kSampleRate) / 60.0));
}

// The handle behind `id` in one of the tables below; nullptr once freed.
template <class Map>
typename Map::mapped_type* find_handle(Map& handles, int64_t id) {
  auto it = handles.find(id);
  return it == handles.end() ? nullptr : &it->second;
}

// A note sliding `start_freq` -> `end_freq` through its phases (in samples),
// at tone's vol and peak (0..100).
Note make_note(double start_freq, double end_freq, int64_t attack, int64_t decay,
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

// The stream sample a note asked for now starts on: the next buffer's, and as
// far into it as the clock has moved since that buffer was dated, so notes
// keep their spacing however long a buffer is. Called under g_tone_mutex.
int64_t stream_now() {
  double ahead = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                               g_stream_at).count();
  return g_stream_next + static_cast<int64_t>(ahead * kSampleRate);
}

// --- Sound: decoded once, played per call -----------------------------------

std::unordered_map<int64_t, Sound> g_sounds;

// --- Music: streamed, fed by a thread of its own ------------------------------
//
// raylib's memory decoders (drmp3 / stb_vorbis) keep POINTERS into the byte
// buffer they were opened on rather than copying it, so a track owns its bytes
// and its Music together. UpdateMusicStream decodes outside raylib's own lock,
// and StopMusicStream rewinds the decoder without taking it, so every Music
// call here and the feeder's update share g_music_mutex.
struct Track {
  Music music{};
  std::vector<uint8_t> bytes;
};

std::mutex g_music_mutex;
std::unordered_map<int64_t, Track> g_tracks;  // guarded by g_music_mutex

std::thread g_feeder;
std::condition_variable g_feeder_wake;
bool g_feeder_stop = false;  // guarded by g_music_mutex

// Every playing track's buffers topped up every few milliseconds: a frame that
// runs long, or a program with no frame loop at all, keeps its music. With
// nothing playing it sleeps until play or resume wakes it.
void feed_music() {
  std::unique_lock<std::mutex> lock(g_music_mutex);
  while (!g_feeder_stop) {
    bool any = false;
    for (auto& [id, t] : g_tracks) {
      if (!IsMusicStreamPlaying(t.music)) continue;
      UpdateMusicStream(t.music);
      any = true;
    }
    if (any) {
      g_feeder_wake.wait_for(lock, std::chrono::milliseconds(5));
    } else {
      g_feeder_wake.wait(lock);
    }
  }
}

void ensure_feeder() {
  if (g_feeder.joinable()) return;
  g_feeder = std::thread(feed_music);
}

void unload_track(Track& t) {
  if (t.music.ctxData != nullptr) UnloadMusicStream(t.music);
  t.music = Music{};
  t.bytes.clear();
}

// --- Pcm: the samples the script synthesises --------------------------------
//
// A block at a time: the script produces samples on the main thread and hands
// them over (push / submit); the audio thread only copies a finished block. A
// script callback on that thread was never an option: the runtime is not
// reentrant from a foreign thread, and a collector's pause inside a ~10 ms
// audio period is an audible dropout. raylib's double-buffered stream is the
// shape for this: needed() says when a block has drained.
class PcmStream : public rt::PcmBlock {
 public:
  PcmStream(int64_t rate, int64_t channels, int64_t buffer)
      : PcmBlock(rate, channels, buffer) {
    if (!g_ready) return;
    // The device period (~10 ms) is the floor raylib clamps a sub-buffer up
    // to; below it, needed() would understate what a block holds and the
    // remainder would play as silence, hence the documented ~512 minimum.
    stream_ = load_stream(rate_, channels_, buffer_);
    ready_ = IsAudioStreamValid(stream_);
  }
  ~PcmStream() { if (ready_ && IsAudioDeviceReady()) UnloadAudioStream(stream_); }
  PcmStream(const PcmStream&) = delete;
  PcmStream& operator=(const PcmStream&) = delete;

  bool ready() const { return ready_; }
  // Frames the stream can take now: a whole block once one has drained.
  int64_t needed() const { return ready_ && IsAudioStreamProcessed(stream_) ? buffer_ : 0; }
  // Hand the pending block over. Nothing to hand, or nowhere to put it yet
  // (both blocks still full): 0, and the samples wait for the next call.
  int64_t submit() {
    int frames = (int)pending();
    if (frames == 0) return 0;
    if (!ready_) { discard(); return 0; }
    if (!IsAudioStreamProcessed(stream_)) return 0;
    UpdateAudioStream(stream_, pend_.data(), frames);
    pend_.clear();
    return frames;
  }
  void play() { if (ready_) PlayAudioStream(stream_); }
  void stop() { if (ready_) StopAudioStream(stream_); }
  void pause() { if (ready_) PauseAudioStream(stream_); }
  void resume() { if (ready_) ResumeAudioStream(stream_); }
  bool playing() const { return ready_ && IsAudioStreamPlaying(stream_); }
  void volume(double v) { if (ready_) SetAudioStreamVolume(stream_, (float)v); }
  void pitch(double p) { if (ready_) SetAudioStreamPitch(stream_, (float)p); }
  void pan(double p) { if (ready_) SetAudioStreamPan(stream_, (float)p); }

 private:
  AudioStream stream_{};
  bool ready_ = false;
};

std::unordered_map<int64_t, PcmStream> g_streams;

// --- Capture: the microphone, read a block at a time ----------------------
//
// The device thread fills a one-second ring and the script drains it, the
// mirror of PCM: the runtime is not reentrant from a foreign thread, so a
// script callback on the device thread was never an option. Opening needs no
// output device, and a capture that cannot be had (no microphone, permission
// refused, CULEBRA_AUDIO=off) is a handle whose reads find nothing, as a PCM
// with no device plays nothing.
class Capture {
 public:
  // `rate` and `channels` arrive checked (rt::pcm_count / pcm_channels).
  Capture(int rate, int channels) : channels_(static_cast<unsigned>(channels)) {
    if (audio_off()) return;
    if (ensure_capture())
      c_ = culebra_capture_open(static_cast<unsigned>(rate), channels_,
                                static_cast<unsigned>(rate));
    static bool warned = false;  // said once, as the device's own warning is
    if (c_ == nullptr && !warned) {
      warned = true;
      std::fputs("WARNING: Audio: no capture device -- Capture stays silent\n",
                 stderr);
    }
  }
  ~Capture() { culebra_capture_close(c_); }
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

  bool ready() const { return c_ != nullptr; }
  void start() { if (c_) culebra_capture_start(c_); }
  void stop() { if (c_) culebra_capture_stop(c_); }
  bool running() const { return c_ && culebra_capture_running(c_); }
  int64_t waiting() const {
    return c_ ? static_cast<int64_t>(culebra_capture_waiting(c_)) : 0;
  }
  // A negative `frames` takes all that are waiting. The ring has one reader,
  // so what waiting() counts is all there when read.
  void read(int64_t frames, std::vector<float>& out) {
    if (!c_) return;
    int64_t have = waiting();
    frames = frames < 0 ? have : std::min(frames, have);
    out.resize(static_cast<size_t>(frames) * channels_);
    culebra_capture_read(c_, out.data(), static_cast<size_t>(frames));
  }

 private:
  CulebraCapture* c_ = nullptr;
  unsigned channels_;
};

std::unordered_map<int64_t, Capture> g_captures;

// --- exit ------------------------------------------------------------------

// Hand the device back at process exit, after everything that plays through
// it: the feeder stops and joins first, then tracks, sounds and streams go,
// then the captures and their context, then the tone stream and the device.
// Each step clears what guards it, so the owning statics find nothing left
// when their own destructors run.
void exit_teardown() {
  if (g_feeder.joinable()) {
    {
      std::lock_guard<std::mutex> lock(g_music_mutex);
      g_feeder_stop = true;
    }
    g_feeder_wake.notify_all();
    g_feeder.join();
  }
  {
    std::lock_guard<std::mutex> lock(g_music_mutex);
    for (auto& [id, t] : g_tracks) unload_track(t);
    g_tracks.clear();
  }
  for (auto& [id, s] : g_sounds) UnloadSound(s);
  g_sounds.clear();
  g_streams.clear();
  g_captures.clear();
  if (g_capture_ready) {
    culebra_capture_shutdown();
    g_capture_ready = false;
  }
  if (g_tone_ready) {
    UnloadAudioStream(g_tone_stream);
    g_tone_ready = false;
  }
  g_mix.playing.clear();
  g_mix.samplers.clear();
  g_score_inbox.clear();
  g_playing_graphs.clear();
  g_graph_inbox.clear();
  g_graph_posts.clear();
  g_graph_posts_taken = 0;
  g_live_graphs.clear();
  g_retired_graphs.clear();
  g_scores.clear();
  g_retired.clear();
  g_sound_pcm.clear();
  if (g_ready) {
    CloseAudioDevice();
    g_ready = false;
  }
}

// Registered once the device exists rather than from a file-scope object's
// destructor: a registration made after the audio driver was dlopen'd runs
// before that driver tears itself down (culebra_rt_canvas.cc's
// arm_exit_teardown has the long form). The device and the capture context
// each arm it; the first one registers.
void arm_exit_teardown() {
  static bool armed = false;
  if (armed) return;
  armed = true;
  std::atexit(exit_teardown);
}

}  // namespace

bool available() {
  ensure_device();
  return g_ready;
}

void tone(int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay,
          int64_t sustain, int64_t release, int64_t vol, int64_t peak,
          int64_t channel, int64_t duty) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  if (channel < 0 || channel > 4) return;
  release_retired();

  Note n = make_note(start_freq, end_freq, frames_to_samples(attack),
                     frames_to_samples(decay), frames_to_samples(sustain),
                     frames_to_samples(release), vol, peak, duty);
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  if (g_inbox_count == kQueueCap) {   // see kQueueCap: the oldest is the loss
    std::move(g_inbox + 1, g_inbox + kQueueCap, g_inbox);
    g_inbox_count--;
  }
  // The queue is walked head-first, and a sequencer never means to place a
  // note behind one it has already asked for, so clock jitter across a buffer
  // edge settles in favour of the order the calls came in.
  int64_t start = std::max(stream_now(), g_last_start);
  g_last_start = start;
  g_inbox[g_inbox_count++] = Queued{start, static_cast<int>(channel), n};
}

// A decoded sample as a score mixes it, mono floats at the mixer's rate
// (converting `w` so); nullptr when it has none. Playing a Sound and
// rendering a score both decode through it.
std::shared_ptr<const std::vector<float>> mixer_samples(Wave& w) {
  WaveFormat(&w, kSampleRate, 32, 1);
  float* f = LoadWaveSamples(w);
  if (f == nullptr) return nullptr;
  auto pcm = std::make_shared<const std::vector<float>>(f, f + w.frameCount);
  UnloadWaveSamples(f);
  return pcm;
}

// A score from Audio.Kauai's list (kScoreStride), a Sound's samples found by
// its handle in `pcm` (one it does not hold, which the device could not
// decode, is left out).
using PcmByHandle = std::unordered_map<int64_t, std::shared_ptr<const std::vector<float>>>;
std::shared_ptr<Score> build_score(const double* events, int64_t count, double length,
                                   double loop_at, const PcmByHandle& pcm) {
  auto samples = [](double seconds) {
    return static_cast<int64_t>(std::max(0.0, seconds) * kSampleRate);
  };
  auto s = std::make_shared<Score>();
  for (int64_t i = 0; i + kScoreStride <= count; i += kScoreStride) {
    const double* e = events + i;
    ScoreEvent ev;
    ev.at = samples(e[0]);
    ev.channel = static_cast<int>(e[1]);
    if (ev.channel < 0) {
      auto it = pcm.find(static_cast<int64_t>(e[2]));
      if (it == pcm.end()) continue;
      ev.pcm = it->second;
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

void score_new(int64_t id, const double* events, int64_t count, double length,
               double loop_at) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  release_retired();
  g_scores[id] = build_score(events, count, length, loop_at, g_sound_pcm);
}

// A mixer of its own plays the score once through, from sample 0 until it
// lets go of it (its last sound and its room's echoes over), as the stream
// would; its Sounds decoded here as sound_load decodes them, so no device is
// needed.
bool score_render(const double* events, int64_t count, double length, double wet,
                  double volume, const std::vector<SoundBytes>& sounds,
                  std::vector<float>& out) {
  PcmByHandle pcm;
  for (const SoundBytes& b : sounds) {
    Wave w = LoadWaveFromMemory(b.fmt, b.data, static_cast<int>(b.len));
    if (w.data == nullptr) continue;
    if (auto p = mixer_samples(w)) pcm[b.handle] = std::move(p);
    UnloadWave(w);
  }
  auto score = build_score(events, count, length, -1, pcm);
  score->volume.store(std::max(0.0, volume));
  score->room->wet.store(std::max(0.0, wet));
  Mixer m;
  m.playing.push_back(PlayingScore{1, std::move(score), 0, 0});
  for (int64_t p = 0; !m.playing.empty(); p++)
    out.push_back(static_cast<float>(std::clamp(m.sample(p, 0.0), -1.0, 1.0)));
  return true;
}
void score_free(int64_t id) {
  auto it = g_scores.find(id);
  if (it == g_scores.end()) return;
  score_stop(id);
  g_retired.push_back(std::move(it->second));
  g_scores.erase(it);
  release_retired();
}
void score_play(int64_t id) {
  release_retired();
  auto it = g_scores.find(id);
  if (it == g_scores.end()) return;
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  g_score_inbox.push_back(ScoreCommand{id, it->second, stream_now()});
}
void score_stop(int64_t id) {
  if (!g_scores.contains(id)) return;
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  g_score_inbox.push_back(ScoreCommand{id, nullptr, 0});
}
void score_volume(int64_t id, double v) {
  auto it = g_scores.find(id);
  if (it != g_scores.end()) it->second->volume.store(std::max(0.0, v));
}
void score_room(int64_t id, double wet) {
  auto it = g_scores.find(id);
  if (it != g_scores.end()) it->second->room->wet.store(std::max(0.0, wet));
}

// The copy plays from the next sample the stream can start it on.
void graph_play(int64_t id) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  const Graph* def = graph_def(id);
  if (!def) return;
  graph_stop(id);
  auto live = std::make_shared<Graph>(*def);
  std::vector<size_t> room;
  live->make_room(room);
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  int64_t start = stream_now();
  g_live_graphs[id] = LiveGraph{live, start, def->size(), std::move(room)};
  g_graph_inbox.push_back(GraphCommand{id, std::move(live), start});
}
void graph_stop(int64_t id) {
  release_retired();
  auto it = g_live_graphs.find(id);
  if (it == g_live_graphs.end()) return;
  g_retired_graphs.push_back(std::move(it->second.graph));
  g_live_graphs.erase(it);
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  g_graph_inbox.push_back(GraphCommand{id, nullptr, 0});
}
// The definition holds every change the copy does and this one too, so its
// count bounds what the copy's list must hold; past what that list can, the
// change brings a longer one, twice as long.
void graph_post(int64_t id, int part, int value, ParamEvent e) {
  release_retired();
  auto it = g_live_graphs.find(id);
  if (it == g_live_graphs.end() || static_cast<size_t>(part) >= it->second.parts) return;
  size_t need = graph_def(id)->changes(part, value);
  size_t& room = it->second.room[2 * static_cast<size_t>(part) + static_cast<size_t>(value)];
  std::vector<ParamEvent> longer;
  if (need > room) {
    room = 2 * need + kGraphLiveRoom;
    longer.reserve(room);
  }
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  g_graph_posts.push_back(GraphPost{it->second.graph, part, value, e, std::move(longer)});
}
double graph_now(int64_t id) {
  auto it = g_live_graphs.find(id);
  if (it == g_live_graphs.end()) return std::nan("");
  std::lock_guard<std::mutex> lock(g_tone_mutex);
  return static_cast<double>(stream_now() - it->second.origin) / kSampleRate;
}

// The format sniff (and its ValueError) has already run in the backend-neutral
// binding; `fmt` is its verdict. Bytes the sniff accepted but the decoder
// rejects give a silent handle, like the browser's failed decodeAudioData.
void sound_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt) {
  ensure_device();
  if (!g_ready) return;
  Wave w = LoadWaveFromMemory(fmt, data, static_cast<int>(len));
  if (w.data == nullptr) return;
  Sound s = LoadSoundFromWave(w);
  g_sounds[id] = s;
  // A score mixes the sample itself, so it keeps it as the mixer's samples.
  if (auto pcm = mixer_samples(w)) g_sound_pcm[id] = std::move(pcm);
  UnloadWave(w);
}
void sound_free(int64_t id) {
  auto it = g_sounds.find(id);
  if (it == g_sounds.end()) return;
  StopSound(it->second);
  UnloadSound(it->second);
  g_sounds.erase(it);
  g_sound_pcm.erase(id);
}
void sound_play(int64_t id) {
  if (auto* s = find_handle(g_sounds, id)) PlaySound(*s);  // restarts if already playing
}
void sound_stop(int64_t id) {
  if (auto* s = find_handle(g_sounds, id)) StopSound(*s);
}
bool sound_playing(int64_t id) {
  auto* s = find_handle(g_sounds, id);
  return s && IsSoundPlaying(*s);
}
void sound_volume(int64_t id, double v) {
  if (auto* s = find_handle(g_sounds, id)) SetSoundVolume(*s, static_cast<float>(v));
}
void sound_pitch(int64_t id, double p) {
  if (auto* s = find_handle(g_sounds, id)) SetSoundPitch(*s, static_cast<float>(p));
}
void sound_pan(int64_t id, double p) {
  if (auto* s = find_handle(g_sounds, id)) SetSoundPan(*s, static_cast<float>(p));
}

void music_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt,
                bool loop) {
  ensure_device();
  if (!g_ready) return;
  // Decoded outside the lock (an MP3 is walked whole to count its frames), so
  // the tracks already playing keep being fed; only the insert takes it.
  Track t;
  t.bytes.assign(data, data + len);
  t.music = LoadMusicStreamFromMemory(fmt, t.bytes.data(),
                                      static_cast<int>(t.bytes.size()));
  if (!IsMusicValid(t.music)) {
    // A null ctx means raylib already freed its partial state; a non-null one
    // (a decodable but empty stream) still owns a decoder to unload.
    unload_track(t);
    return;
  }
  t.music.looping = loop;
  std::lock_guard<std::mutex> lock(g_music_mutex);
  g_tracks.insert_or_assign(id, std::move(t));  // the bytes keep their address
  ensure_feeder();
}
void music_free(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  auto it = g_tracks.find(id);
  if (it == g_tracks.end()) return;
  unload_track(it->second);
  g_tracks.erase(it);
}
void music_play(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) PlayMusicStream(t->music);
  g_feeder_wake.notify_one();
}
void music_stop(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) StopMusicStream(t->music);
}
void music_pause(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) PauseMusicStream(t->music);
}
void music_resume(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) ResumeMusicStream(t->music);
  g_feeder_wake.notify_one();
}
bool music_playing(int64_t id) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  auto* t = find_handle(g_tracks, id);
  return t && IsMusicStreamPlaying(t->music);
}
void music_seek(int64_t id, double seconds) {
  if (!(seconds > 0)) seconds = 0;  // NaN and negatives land at the start
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) SeekMusicStream(t->music, static_cast<float>(seconds));
}
void music_volume(int64_t id, double v) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) SetMusicVolume(t->music, static_cast<float>(v));
}
void music_pitch(int64_t id, double p) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) SetMusicPitch(t->music, static_cast<float>(p));
}
void music_pan(int64_t id, double p) {
  std::lock_guard<std::mutex> lock(g_music_mutex);
  if (auto* t = find_handle(g_tracks, id)) SetMusicPan(t->music, static_cast<float>(p));
}

void pcm_new(int64_t id, int64_t rate, int64_t channels, int64_t buffer) {
  ensure_device();
  g_streams.try_emplace(id, rate, channels, buffer);
}
void pcm_free(int64_t id) { g_streams.erase(id); }
bool pcm_ready(int64_t id) {
  auto* s = find_handle(g_streams, id);
  return s && s->ready();
}
int64_t pcm_needed(int64_t id) {
  auto* s = find_handle(g_streams, id);
  return s ? s->needed() : 0;
}
int64_t pcm_push(int64_t id, const double* values, int64_t count) {
  auto* s = find_handle(g_streams, id);
  return s ? s->push_block(values, count) : 0;
}
int64_t pcm_submit(int64_t id) {
  auto* s = find_handle(g_streams, id);
  return s ? s->submit() : 0;
}
double pcm_latency(int64_t id) {
  auto* s = find_handle(g_streams, id);
  return s ? s->latency() : 0.0;
}
void pcm_play(int64_t id) {
  if (auto* s = find_handle(g_streams, id)) s->play();
}
void pcm_stop(int64_t id) {
  if (auto* s = find_handle(g_streams, id)) s->stop();
}
void pcm_pause(int64_t id) {
  if (auto* s = find_handle(g_streams, id)) s->pause();
}
void pcm_resume(int64_t id) {
  if (auto* s = find_handle(g_streams, id)) s->resume();
}
bool pcm_playing(int64_t id) {
  auto* s = find_handle(g_streams, id);
  return s && s->playing();
}
void pcm_volume(int64_t id, double v) {
  if (auto* s = find_handle(g_streams, id)) s->volume(v);
}
void pcm_pitch(int64_t id, double p) {
  if (auto* s = find_handle(g_streams, id)) s->pitch(p);
}
void pcm_pan(int64_t id, double p) {
  if (auto* s = find_handle(g_streams, id)) s->pan(p);
}

bool capture_present() {
  return ensure_capture() && culebra_capture_present();
}
void capture_new(int64_t id, int rate, int channels) {
  g_captures.try_emplace(id, rate, channels);
}
void capture_free(int64_t id) { g_captures.erase(id); }
bool capture_ready(int64_t id) {
  auto* c = find_handle(g_captures, id);
  return c && c->ready();
}
void capture_start(int64_t id) {
  if (auto* c = find_handle(g_captures, id)) c->start();
}
void capture_stop(int64_t id) {
  if (auto* c = find_handle(g_captures, id)) c->stop();
}
bool capture_running(int64_t id) {
  auto* c = find_handle(g_captures, id);
  return c && c->running();
}
int64_t capture_waiting(int64_t id) {
  auto* c = find_handle(g_captures, id);
  return c ? c->waiting() : 0;
}
void capture_read(int64_t id, int64_t frames, std::vector<float>& out) {
  if (auto* c = find_handle(g_captures, id)) c->read(frames, out);
}

}  // namespace _audio_detail
}  // namespace culebra
