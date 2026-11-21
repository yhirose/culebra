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
// page (no port) nothing plays, as on a machine with no device. MIDI is the
// page's Web MIDI, through midilib as natively (below).
//
// Included by playground/wasm_main.cc alone, which is the whole program.

#include <stdlib/audio_wire.h>
#include <stdlib/midi_handles.h>

#include <emscripten.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
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

// Everything the mixer plays let go (reset_run's part), the stream's place
// going with it for the worklet to take up.
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

// --- MIDI: the page's Web MIDI, through midilib's host backend --------------
//
// The page asks the browser for MIDI when a program first asks for a port
// (worker.js's __midiAccess, which this waits for with JSPI: the first time,
// the browser asks the user), then tells the worker the ports there are and
// each message of an input this program holds open. Those wait in worker.js
// until taken here (midi_pump) into midilib, as the system's MIDI thread
// fills its queues natively, and from there everything is a native build's
// (midi_handles.h). Waiting for one gives the worker back to its event loop
// until one comes. Without JSPI (an older browser) there is no MIDI: no ports.

#if defined(CULEBRA_WASM_JSPI)
// What is granted, MIDI asked for (with sysexes when `sysex`): 1 for MIDI,
// and 2 for sysexes, the page asking the browser when it must.
EM_ASYNC_JS(int, _wasm_midi_access, (int sysex), {
  const granted = await self.__midiAccess(sysex !== 0);
  return (granted.ok ? 1 : 0) | (granted.sysex ? 2 : 0);
});
// Until something comes from the page (a MIDI message of an input held open,
// a message for Page.socket; playground/worker-wake.js), or `ms` pass.
EM_ASYNC_JS(void, _wasm_input_wait, (double ms), {
  await self.__inputWait(ms);
});
#endif
// Which list of ports the page last told of (each a new number).
EM_JS(int, _wasm_midi_ports_version, (), { return self.__midi.version; });
// That list: each input as id, unit separator, name, record separator; then a
// group separator; then each output the same way. Freed by the caller.
EM_JS(char*, _wasm_midi_ports, (), {
  const list = (ports) => ports.map((p) => p.id + "\x1f" + p.name + "\x1e").join("");
  const text = list(self.__midi.inputs) + "\x1d" + list(self.__midi.outputs);
  const n = lengthBytesUTF8(text) + 1;
  const at = _malloc(n);
  stringToUTF8(text, at, n);
  return at;
});
// The messages waiting, taken: their count, then each as how long ago it came
// in ms (a double), its port's id's length and its bytes' (32 bits each), and
// those bytes, little-endian; null when none wait. Freed by the caller.
EM_JS(uint8_t*, _wasm_midi_take, (), {
  const q = self.__midi.queue;
  if (q.length === 0) return 0;
  const now = performance.timeOrigin + performance.now();
  const enc = new TextEncoder();
  const byPort = new Map();  // a take's messages come from a port or two
  const ids = q.map((m) => byPort.get(m.id) || byPort.set(m.id, enc.encode(m.id)).get(m.id));
  let n = 4;
  q.forEach((m, i) => { n += 16 + ids[i].length + m.data.length; });
  const at = _malloc(n);
  const view = new DataView(HEAPU8.buffer, at, n);
  view.setUint32(0, q.length, true);
  let k = 4;
  q.forEach((m, i) => {
    view.setFloat64(k, now - m.at, true);
    view.setUint32(k + 8, ids[i].length, true);
    view.setUint32(k + 12, m.data.length, true);
    HEAPU8.set(ids[i], at + k + 16);
    HEAPU8.set(m.data, at + k + 16 + ids[i].length);
    k += 16 + ids[i].length + m.data.length;
  });
  q.length = 0;
  return at;
});
// An input of port `id` held open (`by` 1) or let go (-1): worker.js keeps the
// messages of the ports held, and passes over the rest.
EM_JS(void, _wasm_midi_hold, (const char* id, int by), {
  const held = self.__midi.held;
  const port = UTF8ToString(id);
  held[port] = (held[port] || 0) + by;
});
EM_JS(void, _wasm_midi_send, (const char* id, const uint8_t* bytes, int n), {
  postMessage({ type: "midiSend", id: UTF8ToString(id), bytes: HEAPU8.slice(bytes, bytes + n) });
});

// The ports and messages the page has sent since, into midilib.
inline void midi_pump() {
  static int version = -1;
  if (int v = _wasm_midi_ports_version(); v != version) {
    version = v;
    char* text = _wasm_midi_ports();
    std::string s(text);
    std::free(text);
    std::vector<midilib::Port> lists[2];
    size_t side = 0;
    for (size_t i = 0; i < s.size();) {
      if (s[i] == '\x1d') {
        side = 1;
        i++;
        continue;
      }
      size_t unit = s.find('\x1f', i), end = s.find('\x1e', i);
      if (unit == std::string::npos || end == std::string::npos) break;
      lists[side].push_back({s.substr(i, unit - i), s.substr(unit + 1, end - unit - 1)});
      i = end + 1;
    }
    midilib::host::set_ports(std::move(lists[0]), std::move(lists[1]));
  }
  uint8_t* taken = _wasm_midi_take();
  if (!taken) return;
  auto u32 = [](const uint8_t* p) {
    uint32_t x;
    std::memcpy(&x, p, 4);
    return x;
  };
  const int64_t now = midilib::now_ns();
  const uint8_t* p = taken + 4;
  std::string id;
  for (uint32_t k = u32(taken); k > 0; k--) {
    double ago_ms;
    std::memcpy(&ago_ms, p, 8);
    const uint32_t id_len = u32(p + 8), n = u32(p + 12);
    id.assign(reinterpret_cast<const char*>(p + 16), id_len);
    midilib::host::receive(id, p + 16 + id_len, n, now - static_cast<int64_t>(ago_ms * 1e6));
    p += 16 + id_len + n;
  }
  std::free(taken);
}

// What the browser granted this run, and what it was asked for: a later run
// asks again, as a later process does natively.
struct MidiGrant {
  bool asked = false, asked_sysex = false;
  bool midi = false, sysex = false;
};
inline MidiGrant& midi_grant() {
  static MidiGrant grant;
  return grant;
}

// What is granted, MIDI asked for once a run, and sysexes too when `sysex`;
// a refused sysex leaves what was granted.
inline const MidiGrant& midi_granted(bool sysex) {
  auto& g = midi_grant();
#if defined(CULEBRA_WASM_JSPI)
  static const bool sending = [] {
    midilib::host::on_send([](const std::string& id, const std::vector<uint8_t>& bytes) {
      _wasm_midi_send(id.c_str(), bytes.data(), static_cast<int>(bytes.size()));
    });
    return true;
  }();
  (void)sending;
  if (sysex ? !g.sysex && !g.asked_sysex : !g.asked) {
    const int answer = _wasm_midi_access(sysex ? 1 : 0);
    g.asked = true;
    g.asked_sysex = g.asked_sysex || sysex;
    g.midi = (answer & 1) != 0;
    g.sysex = (answer & 2) != 0;
  }
#else
  (void)sysex;
#endif
  return g;
}

// Whether MIDI is there, the ports and messages the page has sent taken in.
inline bool midi_ready(bool sysex) {
  if (!midi_granted(sysex).midi) return false;
  midi_pump();
  return true;
}

std::vector<std::string> midi_ports(bool output) { return midi_ready(false) ? midi::ports(output) : std::vector<std::string>{}; }
// An input's messages are held from when it opens, as natively.
bool midi_open(int64_t id, const std::string& port, bool sysex) {
  if (!midi_ready(sysex) || !midi::open(id, port, sysex)) return false;
  _wasm_midi_hold(port.c_str(), 1);
  return true;
}
bool midi_out_open(int64_t id, const std::string& port) { return midi_ready(false) && midi::open_output(id, port); }
void midi_free(int64_t id) {
  if (auto* in = midi::input(id)) _wasm_midi_hold(in->port.c_str(), -1);
  midi::close(id);
}
// A sysex goes out only with the browser's leave, asked for when one is sent
// (0xF0 is never a data byte).
std::string midi_send(int64_t id, const std::vector<uint8_t>& bytes) {
  if (std::find(bytes.begin(), bytes.end(), 0xF0) != bytes.end() && !midi_granted(true).sysex)
    return "the browser did not allow sysexes";
  return midi::send(id, bytes);
}
void midi_read(int64_t id, std::vector<double>& out) {
  midi_pump();
  midi::read(id, out);
}
bool midi_wait(int64_t id, double seconds) {
  auto* in = midi::input(id);
  if (!in) return false;
  const double until = _wasm_audio_now_ms() + seconds * 1000.0;
  for (;;) {
    midi_pump();
    if (in->input.wait(std::chrono::nanoseconds(0))) return true;
    const double left = until - _wasm_audio_now_ms();
    if (left <= 0) return false;
#if defined(CULEBRA_WASM_JSPI)
    _wasm_input_wait(left);
#else
    return false;
#endif
  }
}
int64_t midi_control(int64_t id, int64_t channel, int64_t cc) { return midi::control(id, channel, cc); }
int64_t midi_dropped(int64_t id) { return midi::dropped(id); }

// What a run holds of the audio let go, as a native process's exit does: what
// the mixer plays, the MIDI ports open and what was granted. Before and after each run
// (playground/wasm_main.cc).
inline void reset_run() {
  reset_playing();
  for (auto& [id, in] : midi::ins()) _wasm_midi_hold(in.port.c_str(), -1);
  midi::close_all();
  midi_grant() = {};
}

}  // namespace _audio_detail
}  // namespace culebra
