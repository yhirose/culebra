// Native backend for the `Audio` namespace: the one owner of the sound device,
// on raylib's audio module built standalone (culebra_raudio.c, raudio.h), so
// sound links neither raylib's core nor SDL. Canvas and Scene carry no audio
// of their own. The backend-neutral pieces (format sniff, handle ids, the
// silent and browser backends) are in include/stdlib/audio.h, and the mixer
// tones, schedules and graphs play through in include/stdlib/audio_engine.h.
//
// Everything here runs on the main thread except three helpers raylib,
// miniaudio or this file starts: raylib's mixer thread (the engine's audio
// thread, through the tone stream's callback below), the music feeder, which
// takes g_music_mutex around every Music call, and a capture device's thread,
// which only fills that capture's ring (culebra_raudio.c).

#include "stdlib/audio.h"
#include "stdlib/audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/shared.h"
#include "culebra_capture.h"
#include "culebra_period.h"
#include "midilib.h"
#include "culebra_decode.h"
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

// The MIDI inputs and outputs open, by handle (see midi_open below).
struct MidiIn {
  MidiIn(const std::string& port, bool sysex) : input(port, 4096, sysex) {}
  midilib::Input input;
  midilib::Controls controls;
};
std::unordered_map<int64_t, MidiIn> g_midi;                // main thread
std::unordered_map<int64_t, midilib::Output> g_midi_out;  // main thread

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

// --- tone, schedules and graphs: the engine ------------------------------
//
// stdlib/audio_engine.h mixes them; the tone stream's callback is its audio
// thread, raylib's mixer thread rendering into the stream's buffer what the
// engine plays.
Engine g_engine;
AudioStream g_tone_stream;
bool g_tone_ready = false;

void tone_callback(void* buffer_data, unsigned int frames) {
  g_engine.render(static_cast<float*>(buffer_data), frames);
}

void ensure_tone_stream() {
  ensure_device();
  if (!g_ready || g_tone_ready) return;
  g_tone_stream = load_stream(kSampleRate, 2, 0);  // 32-bit float, left and right in turn
  g_engine.open();
  SetAudioStreamCallback(g_tone_stream, tone_callback);
  PlayAudioStream(g_tone_stream);
  g_tone_ready = true;
}

// The Sounds a schedule can play, by handle, as the mixer's samples. Main
// thread only.
PcmByHandle g_sound_pcm;

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
  g_midi.clear();
  g_midi_out.clear();
  if (g_capture_ready) {
    culebra_capture_shutdown();
    g_capture_ready = false;
  }
  if (g_tone_ready) {
    UnloadAudioStream(g_tone_stream);
    g_tone_ready = false;
  }
  g_engine.clear();
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

// Asked for once the device is open, a period cannot be had without opening
// it again, which every sound playing would not survive.
bool latency_ask(double seconds) {
  if (g_ready) return false;
  culebra_period_ask(seconds);
  return true;
}
double latency() { return g_ready ? culebra_period() : std::nan(""); }

// --- MIDI -------------------------------------------------------------------
//
// cpp-midilib takes each input's messages on the system's MIDI thread into a
// queue of its own; the script reads them here, on its thread, which keeps
// the controls they set. An output sends from the script's thread.
// CULEBRA_AUDIO=off closes MIDI too: no inputs and no outputs.

// A backend that cannot start (no CoreMIDI client, in a sandbox) has no
// ports, as CULEBRA_AUDIO=off has none.
std::vector<std::string> midi_ports(bool output) {
  std::vector<std::string> out;
  if (audio_off()) return out;
  try {
    for (auto& p : output ? midilib::output_ports() : midilib::input_ports()) {
      out.push_back(p.id);
      out.push_back(p.name);
    }
  } catch (const midilib::Error&) {
    out.clear();
  }
  return out;
}

bool midi_open(int64_t id, const std::string& port, bool sysex) {
  if (audio_off()) return false;
  try {
    g_midi.try_emplace(id, port, sysex);
    return true;
  } catch (const midilib::Error&) {
    return false;
  }
}

bool midi_out_open(int64_t id, const std::string& port) {
  if (audio_off()) return false;
  try {
    g_midi_out.try_emplace(id, port);
    return true;
  } catch (const midilib::Error&) {
    return false;
  }
}

void midi_free(int64_t id) {
  g_midi.erase(id);
  g_midi_out.erase(id);
}

std::string midi_send(int64_t id, const std::vector<uint8_t>& bytes) {
  auto* out = find_handle(g_midi_out, id);
  if (!out) return "the output is closed";
  try {
    out->send(bytes);
    return "";
  } catch (const midilib::Error& e) {
    return std::string(str_strip_prefix(e.what(), "midilib: "));
  }
}

void midi_read(int64_t id, std::vector<double>& out) {
  auto* in = find_handle(g_midi, id);
  if (!in) return;
  auto now = midilib::now_ns();
  midilib::Message m;
  while (in->input.poll(m)) {
    double ago = static_cast<double>(now - m.time_ns) / 1e9;
    if (m.kind() == midilib::Kind::sysex) {
      out.insert(out.end(), {ago, 240.0, static_cast<double>(m.sysex.size()), 0.0, 0.0, 0.0});
      out.insert(out.end(), m.sysex.begin(), m.sysex.end());
      continue;
    }
    in->controls.apply(m);
    bool on = m.kind() == midilib::Kind::note_on;
    out.insert(out.end(), {ago, static_cast<double>(m.bytes[0]), static_cast<double>(m.bytes[1]),
                           static_cast<double>(m.bytes[2]), static_cast<double>(m.size),
                           on ? static_cast<double>(in->controls.velocity14(m.channel())) : 0.0});
  }
}

// Waits in slices, so one Ctrl+C stops a program waiting for a note, as it
// stops one waiting on a socket or a child process.
bool midi_wait(int64_t id, double seconds) {
  using namespace std::chrono;
  auto* in = find_handle(g_midi, id);
  if (!in) return false;
  auto deadline = steady_clock::now() + duration_cast<nanoseconds>(duration<double>(seconds));
  for (;;) {
    throw_if_interrupted();
    auto left = std::max<nanoseconds>(deadline - steady_clock::now(), nanoseconds::zero());
    if (in->input.wait(std::min<nanoseconds>(left, milliseconds(100)))) return true;
    if (left == nanoseconds::zero()) return false;
  }
}

int64_t midi_control(int64_t id, int64_t channel, int64_t cc) {
  auto* in = find_handle(g_midi, id);
  return in ? in->controls.value14(static_cast<int>(channel), static_cast<int>(cc)) : 0;
}

int64_t midi_dropped(int64_t id) {
  auto* in = find_handle(g_midi, id);
  return in ? static_cast<int64_t>(in->input.dropped()) : 0;
}

void tone(int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay,
          int64_t sustain, int64_t release, int64_t vol, int64_t peak,
          int64_t channel, int64_t duty) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  g_engine.tone(static_cast<int>(channel),
                make_note(start_freq, end_freq, frames_to_samples(attack),
                          frames_to_samples(decay), frames_to_samples(sustain),
                          frames_to_samples(release), vol, peak, duty));
}

// A decoded sample as a schedule mixes it, mono floats at the mixer's rate
// (converting `w` so); nullptr when it has none. Playing a Sound and
// rendering a schedule both decode through it.
std::shared_ptr<const std::vector<float>> mixer_samples(Wave& w) {
  WaveFormat(&w, kSampleRate, 32, 1);
  float* f = LoadWaveSamples(w);
  if (f == nullptr) return nullptr;
  auto pcm = std::make_shared<const std::vector<float>>(f, f + w.frameCount);
  UnloadWaveSamples(f);
  return pcm;
}

// A schedule from Kauai's list, its Sounds' samples the ones sound_load kept.
void schedule_new(int64_t id, const double* events, int64_t count, double length,
               double loop_at) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  g_engine.schedule_new(id, build_schedule(events, count, length, loop_at, g_sound_pcm));
}

// The schedule rendered by a mixer of its own, its Sounds decoded here as
// sound_load decodes them, so no device is needed.
bool schedule_render(const double* events, int64_t count, double length, double wet,
                  double volume, double from, const std::vector<SoundBytes>& sounds,
                  std::vector<float>& out) {
  PcmByHandle pcm;
  for (const SoundBytes& b : sounds) {
    Wave w = LoadWaveFromMemory(b.fmt, b.data, static_cast<int>(b.len));
    if (w.data == nullptr) continue;
    if (auto p = mixer_samples(w)) pcm[b.handle] = std::move(p);
    UnloadWave(w);
  }
  render_schedule(events, count, length, wet, volume, from, pcm, out);
  return true;
}
DecodeResult decode(const uint8_t* data, int64_t len, const char* fmt, Recording& out) {
  auto sink = [](void* ctx, unsigned rate, unsigned channels, const float* frames,
                 unsigned long long count) {
    auto& r = *static_cast<Recording*>(ctx);
    r.rate = static_cast<int>(rate);
    r.channels = static_cast<int>(channels);
    r.frames.assign(frames, frames + count * channels);
  };
  return culebra_decode(fmt, data, static_cast<size_t>(len), sink, &out) ? DecodeResult::Decoded
                                                                          : DecodeResult::Undecodable;
}
void schedule_free(int64_t id) { g_engine.schedule_free(id); }
void schedule_play(int64_t id) { g_engine.schedule_play(id); }
void schedule_stop(int64_t id) { g_engine.schedule_stop(id); }
void schedule_volume(int64_t id, double v) { g_engine.schedule_volume(id, v); }
void schedule_room(int64_t id, double wet) { g_engine.schedule_room(id, wet); }

// A song with its Sounds to come. With no device it plays nowhere, as
// schedule_new's, and its slots fill nothing.
bool schedule_open(int64_t id, const double* events, int64_t count, int64_t slots, double length,
                   double loop_at) {
  ensure_tone_stream();
  if (g_tone_ready) g_engine.schedule_new(id, open_schedule(events, count, slots, length, loop_at));
  return true;
}
// A Sound the device could not decode fills its slot with silence, so the
// song does not wait for it.
void schedule_fill(int64_t id, int64_t slot, int64_t sound) {
  auto it = g_sound_pcm.find(sound);
  g_engine.schedule_fill(id, slot,
                         it != g_sound_pcm.end() ? it->second : std::make_shared<Pcm::element_type>());
}
double schedule_held(int64_t id) {
  return static_cast<double>(g_engine.schedule_held(id)) / kSampleRate;
}

void graph_play(int64_t id) {
  ensure_tone_stream();
  if (!g_tone_ready) return;
  g_engine.graph_play(id);
}
void graph_stop(int64_t id) { g_engine.graph_stop(id); }
void graph_post(int64_t id, int part, int value, ParamEvent e) {
  g_engine.graph_post(id, part, value, e);
}
double graph_now(int64_t id) { return g_engine.graph_now(id); }

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
  // A schedule mixes the sample itself, so it keeps it as the mixer's samples.
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
