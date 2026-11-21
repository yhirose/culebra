// The browser's audio engine, audio-engine.wasm: audio_wire.h's WireEngine,
// which the page's AudioWorklet drives (audio-worklet.js), and the decoders
// a native build has, which the worker decodes a Sound's samples with for
// the mixer (worker.js). A standalone module, no JS runtime: an
// AudioWorkletGlobalScope has no fetch and no performance clock, so the
// worklet instantiates it from the module the page compiled, and it asks
// the host for nothing it would need them for (audio-engine.js).
//
// The host writes a message into buffers this keeps (engine_numbers,
// engine_pcm) and reads what comes back from buffers this keeps too, so
// nothing is allocated per block, and a message's numbers per call only as
// their buffer grows.

#include <stdlib/audio_wire.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "culebra_mixer_samples.h"

namespace ad = culebra::_audio_detail;

namespace {

ad::WireEngine& engine() {
  static ad::WireEngine e;
  return e;
}
std::vector<double> g_numbers;
std::vector<float> g_pcm;
std::vector<float> g_out(2 * ad::kMixChunk);
std::vector<double> g_status;
ad::Pcm g_decoded;

}  // namespace

extern "C" {

// Room for a message's `n` numbers, and for the `n` samples a fill brings.
__attribute__((export_name("engine_numbers"))) double* engine_numbers(int n) {
  g_numbers.resize(static_cast<size_t>(n));
  return g_numbers.data();
}
__attribute__((export_name("engine_pcm"))) float* engine_pcm(int n) {
  g_pcm.resize(static_cast<size_t>(n));
  return g_pcm.data();
}
// The message in engine_numbers, with the samples in engine_pcm when
// `has_pcm` (which it takes), at `now_ms` on the wall clock; 1 when it was one.
__attribute__((export_name("engine_apply"))) int engine_apply(int has_pcm, double now_ms) {
  ad::Pcm samples = has_pcm ? std::make_shared<const std::vector<float>>(std::move(g_pcm)) : nullptr;
  g_pcm = {};
  return engine().apply(g_numbers.data(), g_numbers.size(), std::move(samples), now_ms) ? 1 : 0;
}
// Everything let go (WireOp::Reset), the stream playing on.
__attribute__((export_name("engine_reset"))) void engine_reset() {
  const auto m = ad::wire::reset();
  engine().apply(m.data(), m.size());
}
// `frames` (at most kMixChunk) frames, the left and the right in turn.
__attribute__((export_name("engine_render"))) const float* engine_render(int frames) {
  engine().render(g_out.data(), static_cast<unsigned>(frames));
  return g_out.data();
}
// WireEngine::status() into engine_status_at, answering its count.
__attribute__((export_name("engine_status"))) int engine_status() {
  engine().status(g_status);
  return static_cast<int>(g_status.size());
}
__attribute__((export_name("engine_status_at"))) const double* engine_status_at() { return g_status.data(); }
// A file's `len` bytes (format `fmt`) as the mixer's samples, their count
// into `*n`; null when they do not decode. The samples stay until the next call.
__attribute__((export_name("decode_for_mixer"))) const float* decode_for_mixer(const uint8_t* data, int len,
                                                                               const char* fmt, int* n) {
  g_decoded = ad::mixer_samples_of(fmt, data, len);
  if (!g_decoded) return nullptr;
  *n = static_cast<int>(g_decoded->size());
  return g_decoded->data();
}
__attribute__((export_name("engine_alloc"))) void* engine_alloc(int bytes) { return std::malloc(static_cast<size_t>(bytes)); }
__attribute__((export_name("engine_free"))) void engine_free(void* p) { std::free(p); }

}  // extern "C"
