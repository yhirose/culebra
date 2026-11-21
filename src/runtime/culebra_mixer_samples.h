#pragma once

// A Sound's samples as a schedule mixes them: mono floats at the mixer's
// rate, decoded and converted by raudio (culebra_raudio.c). A native build's
// Sounds and rendered schedules, and a browser's worker (the engine wasm,
// playground/audio_engine_wasm.cc), decode through it, so a song mixes the
// same samples everywhere.

#include <cstdint>
#include <memory>
#include <vector>

#include "raudio.h"
#include "stdlib/audio_engine.h"

namespace culebra {
namespace _audio_detail {

// `w` converted to the mixer's format and its samples kept; nullptr when it
// has none.
inline Pcm mixer_samples(Wave& w) {
  WaveFormat(&w, kSampleRate, 32, 1);
  float* f = LoadWaveSamples(w);
  if (f == nullptr) return nullptr;
  auto pcm = std::make_shared<const std::vector<float>>(f, f + w.frameCount);
  UnloadWaveSamples(f);
  return pcm;
}
// The samples of a file's `len` bytes, its format `fmt` (sound_format's:
// ".wav", ".ogg", ".mp3"); nullptr when they do not decode.
inline Pcm mixer_samples_of(const char* fmt, const uint8_t* data, int64_t len) {
  Wave w = LoadWaveFromMemory(fmt, data, static_cast<int>(len));
  if (w.data == nullptr) return nullptr;
  auto pcm = mixer_samples(w);
  UnloadWave(w);
  return pcm;
}

}  // namespace _audio_detail
}  // namespace culebra
