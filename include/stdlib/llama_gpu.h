// Which GPU code a binary carries for Llama. Defined by exactly one TU:
// src/runtime/llama_vulkan_dynload.cc (the Vulkan backend, its loader opened at
// run time), src/runtime/llama_gpu_metal.cc (Metal, on macOS) or
// src/runtime/llama_gpu_none.cc (no GPU backend). stdlib/llama.h asks it when a
// model asked for a GPU got none.

#pragma once

namespace culebra::llama {

// The backend a GPU device found belongs to ("vulkan" or "metal"): the only
// GPU backend this binary carries.
const char* gpu_backend();

// Why no GPU device was found: "no Vulkan driver", "no Vulkan device",
// "no Metal device" or "built without GPU support".
const char* gpu_unavailable_reason();

}  // namespace culebra::llama
