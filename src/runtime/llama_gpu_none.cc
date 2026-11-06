// Llama without a GPU backend: what a binary links in place of the GPU half
// (llama_vulkan_dynload.cc, llama_gpu_metal.cc) when it carries no GPU code (a
// driver built without one, and `culebra build --llama-cpu`).
//
// ggml's backend registry is the one llama.cpp object whose code changes with
// GGML_VULKAN or GGML_METAL: built with one, libggml.a calls that backend's
// registry function and needs nothing else from its archive. A null registry is
// what register_backend() already skips (it is also what ggml_backend_vk_reg()
// itself returns when the loader is missing), so answering null here is the
// same "no GPU backend" and the same statics serve both links. The one that
// libggml.a does not reference is never pulled in.

#include <stdlib/llama_gpu.h>

#include <ggml-backend.h>

// Never asked: with no GPU backend, no GPU device is found.
const char* culebra::llama::gpu_backend() { return "cpu"; }

const char* culebra::llama::gpu_unavailable_reason() { return "built without GPU support"; }

extern "C" ggml_backend_reg_t ggml_backend_vk_reg(void) { return nullptr; }
extern "C" ggml_backend_reg_t ggml_backend_metal_reg(void) { return nullptr; }
