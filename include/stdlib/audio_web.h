#pragma once

// The Audio namespace's backend in a browser (audio.h's table), as
// culebra_rt_audio.cc is a native build's. The program runs in a worker and
// what is heard is the page's. Sounds and Music the page plays with WebAudio
// (playground/app.js). Tones, schedules and graphs an Engine (audio_engine.h)
// plays in the page's AudioWorklet at the mixer's rate, as a native device's
// stream does: each call goes to it as numbers (audio_wire.h) over the
// MessagePort the page gives the worker (self.__audioPort, worker.js), dated
// as a native main thread dates it, by where the worklet last said the
// stream was and the time since. The samples a schedule plays are decoded in
// the worker by the decoders a native build has (playground's
// audio-engine.wasm), so a song mixes the same samples as natively. Without a
// page (no port) nothing plays, as on a machine with no device.
//
// Included by playground/wasm_main.cc alone, which is the whole program.

#include <stdlib/audio_wire.h>

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace culebra {
namespace _audio_detail {

// The page's side of Sounds and Music: commands posted to it, a setter's or
// seek's number as `value`. The playing flags a script reads back are kept
// here, optimistically and in step with the call, and corrected by the
// page's "soundState" / "musicState" messages when something ends on its own
// (worker.js). A Sound loaded is also decoded for the mixer (__soundPcm),
// which a schedule's slots are filled from.
EM_JS(void, _wasm_audio_sound, (int cmd, int id, double value), {
  const names = ["play", "stop", "free", "volume", "pitch", "pan"];
  const flags = self.__soundsPlaying;
  if (cmd === 0) flags[id] = true;
  else if (cmd === 1) flags[id] = false;
  else if (cmd === 2) { delete flags[id]; delete self.__soundPcm[id]; }
  postMessage({ type: "sound", cmd: names[cmd], id: id, value: value });
});
EM_JS(void, _wasm_audio_sound_load, (int id, const uint8_t* buf, int len, const char* fmt), {
  const bytes = HEAPU8.slice(buf, buf + len);
  const pcm = self.__decodeForMixer ? self.__decodeForMixer(bytes, UTF8ToString(fmt)) : null;
  if (pcm) self.__soundPcm[id] = pcm;
  postMessage({ type: "sound", cmd: "load", id: id, buf: bytes });
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

// The worklet's side. A message's numbers, and when `sound` is not -1 that
// Sound's samples beside them (none decoded: silence, as natively).
EM_JS(void, _wasm_audio_send, (const double* m, int n, int sound), {
  const port = self.__audioPort;
  if (!port) return;
  const numbers = HEAPF64.slice(m >> 3, (m >> 3) + n);
  const pcm = sound < 0 ? null : (self.__soundPcm[sound] || new Float32Array(0));
  port.postMessage({ numbers: numbers, pcm: pcm });
});
// The stream sample the worklet is at by its last word and the time since,
// at `rate` samples a second; -1 when it has said nothing since `since_ms`.
// Both sides date on Date.now(), the one clock a worklet has, so the time a
// word took to come does not count.
EM_JS(double, _wasm_audio_stream_at, (double rate, double since_ms), {
  const s = self.__audioStatus;
  return s && s.at >= since_ms ? s.pos + (Date.now() - s.at) * rate / 1000 : -1;
});
EM_JS(double, _wasm_audio_now_ms, (), { return Date.now(); });
// The samples schedule `id` has held since it last started, by the worklet's last word.
EM_JS(double, _wasm_audio_held, (double id), {
  const s = self.__audioStatus;
  return (s && s.held[id]) || 0;
});
EM_JS(int, _wasm_audio_has_pcm, (int id), {
  return self.__soundPcm[id] ? 1 : 0;
});

inline void send(const std::vector<double>& m, int64_t sound = -1) {
  _wasm_audio_send(m.data(), static_cast<int>(m.size()), static_cast<int>(sound));
}
// The stream's clock, as the worker reckons it: from the place it set at the
// last reset (reset_playing), which the worklet took up, and the wall clock
// since, corrected by the worklet's word once it comes. So it runs with no
// worklet too (a page not yet clicked, a test), as a native stream's clock
// does with no device.
struct WebClock {
  int64_t at = 0;  // the stream sample at the last reset
  double at_ms = std::nan("");  // and when that was, on the wall clock (none before the first)
  int64_t latest = 0;  // the latest stream_now(), which never goes back
};
inline WebClock& web_clock() {
  static WebClock clock;
  return clock;
}
// Where a call made now starts on the stream: never behind what it said
// before, as Engine::stream_now.
inline int64_t stream_now() {
  auto& c = web_clock();
  double at = _wasm_audio_stream_at(kSampleRate, c.at_ms);
  if (at < 0)
    at = static_cast<double>(c.at) +
         (std::isnan(c.at_ms) ? 0.0 : (_wasm_audio_now_ms() - c.at_ms) * kSampleRate / 1000.0);
  c.latest = std::max(c.latest, static_cast<int64_t>(at));
  return c.latest;
}
// The stream sample each graph playing started on.
inline std::unordered_map<int64_t, int64_t>& graph_origins() {
  static std::unordered_map<int64_t, int64_t> origins;
  return origins;
}

// Everything the mixer plays let go, as a native process's exit does: before
// and after each run (playground/wasm_main.cc).
// The stream's place goes with it, for the worklet to take up.
inline void reset_playing() {
  graph_origins().clear();
  auto& c = web_clock();
  const int64_t at = stream_now();
  c.at = at;
  c.at_ms = _wasm_audio_now_ms();
  send(wire::reset(at, c.at_ms));
}

bool available() { return true; }
// The page's AudioContext chooses its own latency.
bool latency_ask(double) { return true; }
double latency() { return std::nan(""); }

void tone(int64_t start_freq, int64_t end_freq, int64_t attack, int64_t decay, int64_t sustain, int64_t release,
          int64_t vol, int64_t peak, int64_t channel, int64_t duty) {
  if (channel < 0 || channel > 4) return;
  send(wire::tone(static_cast<int>(channel),
                  tone_note(start_freq, end_freq, attack, decay, sustain, release, vol, peak, duty), stream_now()));
}

void sound_load(int64_t id, const uint8_t* data, int64_t len, const char* fmt) {
  _wasm_audio_sound_load(static_cast<int>(id), data, static_cast<int>(len), fmt);
}
void sound_play(int64_t id) { _wasm_audio_sound(0, static_cast<int>(id), 0); }
void sound_stop(int64_t id) { _wasm_audio_sound(1, static_cast<int>(id), 0); }
void sound_free(int64_t id) { _wasm_audio_sound(2, static_cast<int>(id), 0); }
bool sound_playing(int64_t id) { return _wasm_audio_sound_playing(static_cast<int>(id)) != 0; }
void sound_volume(int64_t id, double x) { _wasm_audio_sound(3, static_cast<int>(id), x); }
void sound_pitch(int64_t id, double x) { _wasm_audio_sound(4, static_cast<int>(id), x); }
void sound_pan(int64_t id, double x) { _wasm_audio_sound(5, static_cast<int>(id), x); }

void music_load(int64_t id, const uint8_t* data, int64_t len, const char* /*fmt*/, bool loop) {
  _wasm_audio_music_load(static_cast<int>(id), data, static_cast<int>(len), loop ? 1 : 0);
}
void music_play(int64_t id) { _wasm_audio_music(0, static_cast<int>(id), 0); }
void music_stop(int64_t id) { _wasm_audio_music(1, static_cast<int>(id), 0); }
void music_pause(int64_t id) { _wasm_audio_music(2, static_cast<int>(id), 0); }
void music_resume(int64_t id) { _wasm_audio_music(3, static_cast<int>(id), 0); }
void music_free(int64_t id) { _wasm_audio_music(4, static_cast<int>(id), 0); }
bool music_playing(int64_t id) { return _wasm_audio_music_playing(static_cast<int>(id)) != 0; }
void music_seek(int64_t id, double seconds) { _wasm_audio_music(5, static_cast<int>(id), seconds); }
void music_volume(int64_t id, double x) { _wasm_audio_music(6, static_cast<int>(id), x); }
void music_pitch(int64_t id, double x) { _wasm_audio_music(7, static_cast<int>(id), x); }
void music_pan(int64_t id, double x) { _wasm_audio_music(8, static_cast<int>(id), x); }

// A schedule from Kauai's list goes as a schedule opened, its slots filled
// with the Sounds decoded here (culebra_rt_audio.cc's build_schedule).
void schedule_new(int64_t id, const double* events, int64_t count, double length, double loop_at) {
  auto slotted = slotted_rows(events, count,
                              [](int64_t handle) { return _wasm_audio_has_pcm(static_cast<int>(handle)) != 0; });
  send(wire::schedule_open(id, slotted.rows.data(), count, static_cast<int64_t>(slotted.handles.size()), length,
                           loop_at));
  for (size_t k = 0; k < slotted.handles.size(); k++)
    send(wire::schedule_fill(id, static_cast<int64_t>(k)), slotted.handles[k]);
}
bool schedule_open(int64_t id, const double* events, int64_t count, int64_t slots, double length,
                   double loop_at) {
  send(wire::schedule_open(id, events, count, slots, length, loop_at));
  return true;
}
void schedule_fill(int64_t id, int64_t slot, int64_t sound) { send(wire::schedule_fill(id, slot), sound); }
void schedule_play(int64_t id) { send(wire::schedule_play(id, stream_now())); }
void schedule_stop(int64_t id) { send(wire::schedule(WireOp::ScheduleStop, id)); }
void schedule_free(int64_t id) { send(wire::schedule(WireOp::ScheduleFree, id)); }
void schedule_volume(int64_t id, double x) { send(wire::schedule(WireOp::ScheduleVolume, id, x)); }
void schedule_room(int64_t id, double x) { send(wire::schedule(WireOp::ScheduleRoom, id, x)); }
double schedule_held(int64_t id) { return _wasm_audio_held(static_cast<double>(id)) / kSampleRate; }
// Rendering and decoding answer the program, which the worklet's mixer
// cannot: not available here yet.
bool schedule_render(const double*, int64_t, double, double, double, double, const std::vector<SoundBytes>&,
                     std::vector<float>&, std::vector<float>*) {
  return false;
}
DecodeResult decode(const uint8_t*, int64_t, const char*, Recording&) { return DecodeResult::Unavailable; }

void graph_play(int64_t id, int64_t into, const std::vector<int64_t>& order) {
  const int64_t start = stream_now();
  graph_origins()[id] = start;
  send(wire::graph_play(id, into, order, start));
}
void graph_stop(int64_t id) {
  if (graph_origins().erase(id) == 0) return;
  send(wire::graph_stop(id));
}
void graph_post(int64_t id, int part, int value, ParamEvent e) {
  if (graph_origins().contains(id)) send(wire::graph_post(id, part, value, e));
}
double graph_now(int64_t id) {
  auto it = graph_origins().find(id);
  return it == graph_origins().end() ? std::nan("") : static_cast<double>(stream_now() - it->second) / kSampleRate;
}

}  // namespace _audio_detail
}  // namespace culebra
