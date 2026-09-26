#pragma once

// The Audio namespace's backend choke: the one owner of the sound device and
// of everything that plays through it (tone, Sound, Music, Stream). The
// script-facing surface is src/preambles/audio.cul and the natives are in
// stdlib/bindings.h; this header picks the backend.
//
//   native (CULEBRA_AUDIO_NATIVE)   raylib's audio module built standalone
//                                   (src/runtime/culebra_raudio.c), with the
//                                   bodies in src/runtime/culebra_rt_audio.cc
//   base AOT archive of a native    weak silent bodies, overridden by the Audio
//   build (CULEBRA_RT_AUDIO_WEAK)   feature archive when a program names Audio
//   browser (__EMSCRIPTEN__)        WebAudio on the page (playground/app.js);
//                                   Stream is silent there
//   anything else                   silent, as a machine with no device is
//
// Silent means what a native build answers on a machine with no audio device:
// nothing plays, and a Stream's block counts pushes all the same.

#include <stdlib/pcm_block.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
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

inline constexpr auto kStreamSamplesError =
    "type error: parameter 'samples' expects an Array of Long|Float";

// Sound, Music and Stream handles: one counter for every backend, so a
// handle's lifecycle reads the same whether or not a host can play it.
inline int64_t alloc_id() {
  static int64_t n = 0;
  return ++n;
}

#if defined(__EMSCRIPTEN__)

// The page owns WebAudio; these post commands to it (playground/app.js). The
// playing flags a script reads back are kept here, optimistically and in step
// with the call, and corrected by the page's "soundState" / "musicState"
// messages when something ends on its own (playground/worker.js).

// A WASM-4-style tone: times in frames at ~60 fps, vol/peak 0..100.
EM_JS(void, _wasm_audio_tone,
      (int start_freq, int end_freq, int attack, int decay, int sustain,
       int release, int vol, int peak, int channel, int duty), {
  postMessage({ type: "tone", startFreq: start_freq, endFreq: end_freq,
                attack: attack, decay: decay, sustain: sustain,
                release: release, vol: vol, peak: peak, channel: channel,
                duty: duty });
});
EM_JS(void, _wasm_audio_sound, (int cmd, int id, double a, double b, double c), {
  const names = ["play", "stop", "free", "set"];
  const flags = (self.__soundsPlaying = self.__soundsPlaying || {});
  if (cmd === 0) flags[id] = true;
  else if (cmd === 1) flags[id] = false;
  else if (cmd === 2) delete flags[id];
  postMessage({ type: "sound", cmd: names[cmd], id: id, vol: a, pitch: b, pan: c });
});
EM_JS(void, _wasm_audio_sound_load, (int id, const uint8_t* buf, int len), {
  postMessage({ type: "sound", cmd: "load", id: id,
                buf: HEAPU8.slice(buf, buf + len) });
});
EM_JS(int, _wasm_audio_sound_playing, (int id), {
  return self.__soundsPlaying && self.__soundsPlaying[id] ? 1 : 0;
});
EM_JS(void, _wasm_audio_music, (int cmd, int id, double a, double b, double c), {
  const names = ["play", "stop", "pause", "resume", "free", "seek", "set"];
  const flags = (self.__musicPlaying = self.__musicPlaying || {});
  if (cmd === 0 || cmd === 3) flags[id] = true;
  else if (cmd === 1 || cmd === 2) flags[id] = false;
  else if (cmd === 4) delete flags[id];
  postMessage({ type: "music", cmd: names[cmd], id: id, a: a, b: b, c: c });
});
EM_JS(void, _wasm_audio_music_load,
      (int id, const uint8_t* buf, int len, int looping), {
  postMessage({ type: "music", cmd: "load", id: id,
                buf: HEAPU8.slice(buf, buf + len), loop: looping !== 0 });
});
EM_JS(int, _wasm_audio_music_playing, (int id), {
  return self.__musicPlaying && self.__musicPlaying[id] ? 1 : 0;
});

// What the page needs to (re)build a voice: the handle's settings, kept on
// this side so play() carries them and set() can change a live voice.
struct WasmVoice {
  double volume = 1.0, pitch = 1.0, pan = 0.0;
};
inline std::unordered_map<int64_t, WasmVoice>& wasm_voices() {
  static std::unordered_map<int64_t, WasmVoice> voices;
  return voices;
}

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
  wasm_voices()[id];
  _wasm_audio_sound_load(static_cast<int>(id), data, static_cast<int>(len));
}
inline void sound_free(int64_t id) {
  wasm_voices().erase(id);
  _wasm_audio_sound(2, static_cast<int>(id), 0, 0, 0);
}
inline void sound_play(int64_t id) {
  auto& v = wasm_voices()[id];
  _wasm_audio_sound(0, static_cast<int>(id), v.volume, v.pitch, v.pan);
}
inline void sound_stop(int64_t id) {
  _wasm_audio_sound(1, static_cast<int>(id), 0, 0, 0);
}
inline bool sound_playing(int64_t id) {
  return _wasm_audio_sound_playing(static_cast<int>(id)) != 0;
}
inline void sound_set(int64_t id) {
  auto& v = wasm_voices()[id];
  _wasm_audio_sound(3, static_cast<int>(id), v.volume, v.pitch, v.pan);
}
inline void sound_volume(int64_t id, double x) { wasm_voices()[id].volume = x; sound_set(id); }
inline void sound_pitch(int64_t id, double x) { wasm_voices()[id].pitch = x; sound_set(id); }
inline void sound_pan(int64_t id, double x) { wasm_voices()[id].pan = x; sound_set(id); }

inline void music_load(int64_t id, const uint8_t* data, int64_t len,
                       const char* /*fmt*/, bool loop) {
  wasm_voices()[id];
  _wasm_audio_music_load(static_cast<int>(id), data, static_cast<int>(len),
                         loop ? 1 : 0);
}
inline void music_set(int64_t id) {
  auto& v = wasm_voices()[id];
  _wasm_audio_music(6, static_cast<int>(id), v.volume, v.pitch, v.pan);
}
inline void music_free(int64_t id) {
  wasm_voices().erase(id);
  _wasm_audio_music(4, static_cast<int>(id), 0, 0, 0);
}
inline void music_play(int64_t id) {
  music_set(id);
  _wasm_audio_music(0, static_cast<int>(id), 0, 0, 0);
}
inline void music_stop(int64_t id) { _wasm_audio_music(1, static_cast<int>(id), 0, 0, 0); }
inline void music_pause(int64_t id) { _wasm_audio_music(2, static_cast<int>(id), 0, 0, 0); }
inline void music_resume(int64_t id) { _wasm_audio_music(3, static_cast<int>(id), 0, 0, 0); }
inline bool music_playing(int64_t id) {
  return _wasm_audio_music_playing(static_cast<int>(id)) != 0;
}
inline void music_seek(int64_t id, double seconds) {
  _wasm_audio_music(5, static_cast<int>(id), seconds, 0, 0);
}
inline void music_volume(int64_t id, double x) { wasm_voices()[id].volume = x; music_set(id); }
inline void music_pitch(int64_t id, double x) { wasm_voices()[id].pitch = x; music_set(id); }
inline void music_pan(int64_t id, double x) { wasm_voices()[id].pan = x; music_set(id); }

#elif defined(CULEBRA_AUDIO_NATIVE) && !defined(CULEBRA_RT_AUDIO_WEAK)

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
void stream_new(int64_t id, int64_t rate, int64_t channels, int64_t buffer);
void stream_free(int64_t id);
bool stream_ready(int64_t id);
int64_t stream_needed(int64_t id);
int64_t stream_push(int64_t id, const double* values, int64_t count);
int64_t stream_submit(int64_t id);
double stream_latency(int64_t id);
void stream_play(int64_t id);
void stream_stop(int64_t id);
void stream_pause(int64_t id);
void stream_resume(int64_t id);
bool stream_playing(int64_t id);
void stream_volume(int64_t id, double v);
void stream_pitch(int64_t id, double p);
void stream_pan(int64_t id, double p);

#endif

// Silent bodies: inline where nothing else defines them, weak in the base AOT
// archive of a native build so the feature archive's strong ones win.
#if defined(CULEBRA_RT_AUDIO_WEAK)
#define CULEBRA_RT_AUDIO_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_AUDIO_LINKAGE inline
#endif

#if !defined(__EMSCRIPTEN__) && \
    (!defined(CULEBRA_AUDIO_NATIVE) || defined(CULEBRA_RT_AUDIO_WEAK))
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
#endif

#if !defined(CULEBRA_AUDIO_NATIVE) || defined(CULEBRA_RT_AUDIO_WEAK) || \
    defined(__EMSCRIPTEN__)
// A Stream with nowhere to play: its block counts pushes and a submit empties
// it, as a native stream with no device does.
inline std::unordered_map<int64_t, rt::PcmBlock>& silent_streams() {
  static std::unordered_map<int64_t, rt::PcmBlock> streams;
  return streams;
}
inline rt::PcmBlock* silent_stream(int64_t id) {
  auto it = silent_streams().find(id);
  return it == silent_streams().end() ? nullptr : &it->second;
}
CULEBRA_RT_AUDIO_LINKAGE void stream_new(int64_t id, int64_t rate,
                                         int64_t channels, int64_t buffer) {
  silent_streams().try_emplace(id, rate, channels, buffer);
}
CULEBRA_RT_AUDIO_LINKAGE void stream_free(int64_t id) {
  silent_streams().erase(id);
}
CULEBRA_RT_AUDIO_LINKAGE bool stream_ready(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE int64_t stream_needed(int64_t) { return 0; }
CULEBRA_RT_AUDIO_LINKAGE int64_t stream_push(int64_t id, const double* values,
                                             int64_t count) {
  auto* b = silent_stream(id);
  return b ? b->push_block(values, count) : 0;
}
CULEBRA_RT_AUDIO_LINKAGE int64_t stream_submit(int64_t id) {
  if (auto* b = silent_stream(id)) b->discard();
  return 0;
}
CULEBRA_RT_AUDIO_LINKAGE double stream_latency(int64_t id) {
  auto* b = silent_stream(id);
  return b ? b->latency() : 0.0;
}
CULEBRA_RT_AUDIO_LINKAGE void stream_play(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void stream_stop(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void stream_pause(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE void stream_resume(int64_t) {}
CULEBRA_RT_AUDIO_LINKAGE bool stream_playing(int64_t) { return false; }
CULEBRA_RT_AUDIO_LINKAGE void stream_volume(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void stream_pitch(int64_t, double) {}
CULEBRA_RT_AUDIO_LINKAGE void stream_pan(int64_t, double) {}
#endif

#undef CULEBRA_RT_AUDIO_LINKAGE

}  // namespace _audio_detail
}  // namespace culebra
