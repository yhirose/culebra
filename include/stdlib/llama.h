// Value-neutral Llama core for culebra: a language model run in this process,
// over cpp-llamalib (llama.h only) and a statically linked llama.cpp. Mirrors
// sqlite.h: this header carries no interpreter/JIT/Value types, only plain C++
// structs and an id-based API that the backends adapt into culebra values.
//
// A model is a handle id into a registry (id_registry.h), never a pointer, so a
// closed or stale id degrades to an error instead of being dereferenced. The
// registry is thread_local: a model handle is __nonsendable__ (it never crosses
// an isolate), so per-thread tables are correct and lock-free.
//
// Everything that reaches llama.cpp is a "choke" that three linkages share
// (the same partitioning as sqlite.h): the core runtime archive gets weak stubs
// that reference no llama symbol, the feature archive gets strong bodies that
// `culebra build` force-loads only for a program that names Llama, and the
// driver and in-process JIT use the inline bodies. Failure is a `false` with
// `*err` set; cpp-llamalib's exceptions stop here.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(CULEBRA_RT_LLAMA_STRONG)
#define CULEBRA_RT_LLAMA_LINKAGE
#elif defined(CULEBRA_RT_LLAMA_WEAK)
#define CULEBRA_RT_LLAMA_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_LLAMA_LINKAGE inline
#endif

#if !defined(CULEBRA_RT_LLAMA_WEAK)
#include <base/id_registry.h>  // IdRegistry<T> (slot+generation handle table)
#include <stdlib/llama_gpu.h>

#include <memory>

#include "cpp-llamalib.h"
#endif

namespace culebra::llama {

struct LoadOptions {
  int n_ctx = 2048;
  int n_batch = 0;  // 0 = llama.cpp's default
  int n_slots = 1;
  bool gpu = true;  // use a GPU when there is one; false keeps the model on the CPU
};

struct Message {
  std::string role;
  std::string content;
};

struct GenerateOptions {
  int max_tokens = -1;  // -1 = until the end or the context is full
  std::vector<std::string> stop;
  std::optional<float> temperature;
  std::optional<uint32_t> seed;
  std::string grammar;  // GBNF; empty = none
  bool cache = false;   // reuse the cache of the previous call (see cpp-llamalib reuse_cache)
};

enum class Finish { End, Length, Stop, Aborted };

struct Result {
  std::string text;
  Finish finish = Finish::End;
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

// Where a model runs, decided when it is loaded.
struct Device {
  std::string backend;  // "vulkan" or "cpu"
  std::string name;     // the GPU's own description, or "CPU"
  std::string reason;   // empty when a GPU is used; otherwise why not
};

// Called with each piece of text as it is generated; false stops the generation.
using OnPiece = std::function<bool(std::string_view)>;

// ---- Process-local handle table --------------------------------------------
// Left out of the weak-stub branch for the reason sqlite.h gives: it never
// interns a handle, and a second `inline thread_local` in the core archive would
// collide with the feature archive's.

#if !defined(CULEBRA_RT_LLAMA_WEAK)

namespace detail {
struct Model {
  std::unique_ptr<llamalib::Llama> lib;
  Device device;
};

inline thread_local IdRegistry<Model> g_models;

inline int64_t model_intern(Model* m) { return g_models.add(m); }
inline Model* model_get(int64_t id) { return g_models.get(id); }
}  // namespace detail

#endif  // !CULEBRA_RT_LLAMA_WEAK

// ---- The model -------------------------------------------------------------

// Loads the model at `path`. Returns a handle id, or -1 with *err set.
CULEBRA_RT_LLAMA_LINKAGE int64_t load(const std::string& path,
                                      const LoadOptions& opts, std::string* err);

// Frees the model and its table slot. Idempotent; a stale id is ignored.
CULEBRA_RT_LLAMA_LINKAGE void close(int64_t id);

// The context size the model got, which llama.cpp may round up from n_ctx.
// 0 for a closed handle.
CULEBRA_RT_LLAMA_LINKAGE int64_t n_ctx(int64_t id);

CULEBRA_RT_LLAMA_LINKAGE bool device(int64_t id, Device* out, std::string* err);

CULEBRA_RT_LLAMA_LINKAGE bool generate(int64_t id, const std::string& prompt,
                                       const GenerateOptions& opts,
                                       const OnPiece& on_piece, Result* out,
                                       std::string* err);

// `messages` go through the model's own chat template.
CULEBRA_RT_LLAMA_LINKAGE bool chat(int64_t id, const std::vector<Message>& messages,
                                   const GenerateOptions& opts,
                                   const OnPiece& on_piece, Result* out,
                                   std::string* err);

// The prompt the chat template makes of `messages`.
CULEBRA_RT_LLAMA_LINKAGE bool format_chat(int64_t id,
                                          const std::vector<Message>& messages,
                                          std::string* out, std::string* err);

CULEBRA_RT_LLAMA_LINKAGE bool count_tokens(int64_t id, const std::string& text,
                                           int64_t* out, std::string* err);

// ============================================================================
// Implementation. The weak-stub branch (core archive) references no llama
// symbol; it answers that Llama is not linked and is never reached at run time
// (a program that does not name Llama does not call it).
// ============================================================================

#if defined(CULEBRA_RT_LLAMA_WEAK)

#define CULEBRA_LLAMA_STUB "Llama runtime not linked (no Llama use detected at build)"

CULEBRA_RT_LLAMA_LINKAGE int64_t load(const std::string&, const LoadOptions&,
                                      std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return -1;
}
CULEBRA_RT_LLAMA_LINKAGE void close(int64_t) {}
CULEBRA_RT_LLAMA_LINKAGE int64_t n_ctx(int64_t) { return 0; }
CULEBRA_RT_LLAMA_LINKAGE bool device(int64_t, Device*, std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return false;
}
CULEBRA_RT_LLAMA_LINKAGE bool generate(int64_t, const std::string&,
                                       const GenerateOptions&, const OnPiece&,
                                       Result*, std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return false;
}
CULEBRA_RT_LLAMA_LINKAGE bool chat(int64_t, const std::vector<Message>&,
                                   const GenerateOptions&, const OnPiece&,
                                   Result*, std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return false;
}
CULEBRA_RT_LLAMA_LINKAGE bool format_chat(int64_t, const std::vector<Message>&,
                                          std::string*, std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return false;
}
CULEBRA_RT_LLAMA_LINKAGE bool count_tokens(int64_t, const std::string&, int64_t*,
                                           std::string* err) {
  if (err) *err = CULEBRA_LLAMA_STUB;
  return false;
}

#undef CULEBRA_LLAMA_STUB

#else  // strong (feature archive) or inline (driver / in-process JIT)

namespace detail {

// llama.cpp narrates every load on stderr, which a script has no use for. Its
// first error is the one thing worth keeping: it says why a model did not load
// ("failed to open GGUF file ..."), where cpp-llamalib can only say that it did
// not. So the log goes to this, and nowhere else.
inline thread_local std::string g_load_error;

inline void log_sink(ggml_log_level level, const char* text, void*) {
  if (level == GGML_LOG_LEVEL_ERROR && g_load_error.empty()) g_load_error = text;
}

inline void silence_llama_log() {
  static const bool once = (llama_log_set(log_sink, nullptr), true);
  (void)once;
}

// The CPU features the build of ggml compiles in unconditionally (cmake/llama.cmake:
// GGML_NATIVE=OFF still turns on AVX, AVX2, BMI2, FMA, F16C and SSE4.2 on x86).
// A CPU without them would stop with an illegal instruction in the first kernel,
// so say so before loading anything. Empty when nothing is missing.
inline std::string missing_cpu_features() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_cpu_init();
  std::string missing;
  auto need = [&](bool have, const char* name) {
    if (have) return;
    if (!missing.empty()) missing += ", ";
    missing += name;
  };
  need(__builtin_cpu_supports("avx2"), "AVX2");
  need(__builtin_cpu_supports("bmi2"), "BMI2");
  need(__builtin_cpu_supports("fma"), "FMA");
  need(__builtin_cpu_supports("f16c"), "F16C");
  return missing;
#else
  return {};
#endif
}

inline llamalib::GenerateOptions to_lib(const GenerateOptions& o) {
  llamalib::GenerateOptions g;
  g.max_tokens = o.max_tokens;
  g.grammar = o.grammar;
  g.stop = o.stop;
  g.temperature = o.temperature;
  g.seed = o.seed;
  g.reuse_cache = o.cache;
  return g;
}

inline std::vector<llamalib::Message> to_lib(const std::vector<Message>& ms) {
  std::vector<llamalib::Message> out;
  out.reserve(ms.size());
  for (const auto& m : ms) out.push_back({m.role, m.content});
  return out;
}

inline Finish from_lib(llamalib::FinishReason r) {
  switch (r) {
    case llamalib::FinishReason::end: return Finish::End;
    case llamalib::FinishReason::length: return Finish::Length;
    case llamalib::FinishReason::stop: return Finish::Stop;
    case llamalib::FinishReason::aborted: return Finish::Aborted;
  }
  return Finish::End;
}

inline Result from_lib(llamalib::Result&& r) {
  Result out;
  out.text = std::move(r.text);
  out.finish = from_lib(r.finish);
  out.prompt_tokens = r.prompt_tokens;
  out.completion_tokens = r.completion_tokens;
  return out;
}

// The device llama.cpp put the model on. With no device list, its loader offloads
// to every discrete GPU, or to the integrated ones when there is none
// (llama_model_load_from_file_impl); the first of those is the one reported.
inline Device chosen_device(bool gpu) {
  if (!gpu) return {"cpu", "CPU", "gpu: false"};
  ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
  if (!dev) dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_IGPU);
  if (!dev) return {"cpu", "CPU", gpu_unavailable_reason()};
  return {gpu_backend(), ggml_backend_dev_description(dev), ""};
}

// A handle that is not open: the one message for every method that needs a model.
inline Model* open_model(int64_t id, std::string* err) {
  auto* m = model_get(id);
  if (!m && err) *err = "the model is closed";
  return m;
}

// Runs `body`, which may throw cpp-llamalib's errors; they end here as text.
template <class F>
inline bool guarded(std::string* err, F&& body) {
  try {
    body();
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

}  // namespace detail

CULEBRA_RT_LLAMA_LINKAGE int64_t load(const std::string& path,
                                      const LoadOptions& opts, std::string* err) {
  if (auto missing = detail::missing_cpu_features(); !missing.empty()) {
    if (err) {
      *err = "this CPU lacks " + missing +
             ", which the bundled llama.cpp needs";
    }
    return -1;
  }
  detail::silence_llama_log();
  detail::g_load_error.clear();
  std::unique_ptr<llamalib::Llama> model;
  bool ok = detail::guarded(err, [&] {
    llamalib::Options o;
    o.n_ctx = opts.n_ctx;
    o.n_batch = opts.n_batch;
    o.n_slots = opts.n_slots;
    o.cpu_only = !opts.gpu;
    // A model without a template this library can apply is an error when it is
    // asked to chat, not a silent join of the messages.
    o.strict_chat_template = true;
    model = std::make_unique<llamalib::Llama>(path, o);
  });
  if (!ok) {
    if (err && !detail::g_load_error.empty()) {
      std::string why = detail::g_load_error;
      while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
      *err += ": " + why;
    }
    return -1;
  }
  return detail::model_intern(
      new detail::Model{std::move(model), detail::chosen_device(opts.gpu)});
}

CULEBRA_RT_LLAMA_LINKAGE void close(int64_t id) {
  auto* m = detail::model_get(id);
  if (!m) return;
  detail::g_models.invalidate(id);
  delete m;
}

CULEBRA_RT_LLAMA_LINKAGE int64_t n_ctx(int64_t id) {
  auto* m = detail::model_get(id);
  return m ? m->lib->n_ctx() : 0;
}

CULEBRA_RT_LLAMA_LINKAGE bool device(int64_t id, Device* out, std::string* err) {
  auto* m = detail::open_model(id, err);
  if (!m) return false;
  *out = m->device;
  return true;
}

CULEBRA_RT_LLAMA_LINKAGE bool generate(int64_t id, const std::string& prompt,
                                       const GenerateOptions& opts,
                                       const OnPiece& on_piece, Result* out,
                                       std::string* err) {
  auto* m = detail::open_model(id, err);
  if (!m) return false;
  return detail::guarded(err, [&] {
    *out = detail::from_lib(m->lib->complete(prompt, detail::to_lib(opts), on_piece));
  });
}

CULEBRA_RT_LLAMA_LINKAGE bool chat(int64_t id, const std::vector<Message>& messages,
                                   const GenerateOptions& opts,
                                   const OnPiece& on_piece, Result* out,
                                   std::string* err) {
  auto* m = detail::open_model(id, err);
  if (!m) return false;
  return detail::guarded(err, [&] {
    *out = detail::from_lib(
        m->lib->chat_complete(detail::to_lib(messages), detail::to_lib(opts), on_piece));
  });
}

CULEBRA_RT_LLAMA_LINKAGE bool format_chat(int64_t id,
                                          const std::vector<Message>& messages,
                                          std::string* out, std::string* err) {
  auto* m = detail::open_model(id, err);
  if (!m) return false;
  return detail::guarded(err, [&] { *out = m->lib->format_chat(detail::to_lib(messages)); });
}

CULEBRA_RT_LLAMA_LINKAGE bool count_tokens(int64_t id, const std::string& text,
                                           int64_t* out, std::string* err) {
  auto* m = detail::open_model(id, err);
  if (!m) return false;
  return detail::guarded(err, [&] { *out = static_cast<int64_t>(m->lib->token_count(text)); });
}

#endif  // CULEBRA_RT_LLAMA_WEAK

}  // namespace culebra::llama
