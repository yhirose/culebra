// Llama without a GPU backend: what a binary links in place of
// llama_vulkan_dynload.cc when it carries no GPU code (a driver built with
// CULEBRA_LLAMA_VULKAN off, and the default `culebra build` output).
//
// ggml's backend registry is the one llama.cpp object whose code changes with
// GGML_VULKAN: built with it, libggml.a calls ggml_backend_vk_reg() and needs
// nothing else from libggml-vulkan.a. A null registry is what register_backend()
// already skips (it is also what ggml_backend_vk_reg() itself returns when the
// loader is missing), so answering null here is the same "no Vulkan backend"
// and the same statics serve both builds. Unreferenced in a build without
// GGML_VULKAN.

#include <stdlib/llama_gpu.h>

#include <ggml-backend.h>

// Never asked: with no GPU backend, no GPU device is found.
const char* culebra::llama::gpu_backend() { return "cpu"; }

const char* culebra::llama::gpu_unavailable_reason() { return "built without GPU support"; }

extern "C" ggml_backend_reg_t ggml_backend_vk_reg(void) { return nullptr; }
