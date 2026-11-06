// Llama with the Metal backend (macOS). Metal is part of the system, so unlike
// Vulkan there is no loader to open: ggml-metal links the frameworks, and a Mac
// without a GPU Metal can use (a virtual machine) registers no device.

#include <stdlib/llama_gpu.h>

const char* culebra::llama::gpu_backend() { return "metal"; }

const char* culebra::llama::gpu_unavailable_reason() { return "no Metal device"; }
