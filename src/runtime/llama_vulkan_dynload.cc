// Load the Vulkan loader when ggml first asks for it, not when the process starts.
//
// ggml-vulkan links against the loader (libvulkan.so.1, vulkan-1.dll), which
// would put it in DT_NEEDED and stop the binary from starting where no GPU
// driver stack is installed: a server, a container, a CI runner. A language
// runtime should start there and run Llama on the CPU.
//
// ggml-vulkan reaches the loader through vulkan-hpp's dynamic dispatcher, which
// it fills from vkGetInstanceProcAddr; only four loader functions are named
// directly (`nm -u libggml-vulkan.a`). This TU defines those four as forwarders
// over pointers resolved at run time, all or nothing. When the loader cannot be
// opened, vkGetInstanceProcAddr answers nullptr for everything:
// ggml_vk_instance_init() then throws "Vulkan 1.2 required", which
// ggml_backend_vk_reg() catches and turns into "no Vulkan backend", so llama.cpp
// sees only the CPU. The other three are reached only through an instance, which
// cannot exist unless the table was filled. tools/checks/check_llama_vulkan_dynload.sh
// keeps the list equal to what the archive names and the loader out of DT_NEEDED.
//
// The forwarders must stay out of the dynamic symbol table, for the reason
// webview_gtk_dynload.cc gives: -fvisibility=hidden (CMakeLists) and the
// `culebra_*` --dynamic-list both keep them there.

#include <vulkan/vulkan.h>

#include <stdlib/llama_gpu.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// One entry per symbol `nm -u` reports for libggml-vulkan.a.
#define CULEBRA_LLAMA_VULKAN_SYMBOLS(X) \
  X(vkCmdCopyBuffer)                    \
  X(vkGetDeviceProcAddr)                \
  X(vkGetInstanceProcAddr)              \
  X(vkGetPhysicalDeviceFeatures2)

namespace {

// Slot types come from the header's own prototypes, so a signature that drifts
// is a compile error here rather than a corrupt call at run time.
struct Symbols {
#define CULEBRA_LLAMA_VULKAN_SLOT(name) decltype(&::name) name = nullptr;
  CULEBRA_LLAMA_VULKAN_SYMBOLS(CULEBRA_LLAMA_VULKAN_SLOT)
#undef CULEBRA_LLAMA_VULKAN_SLOT
};

Symbols sym;

bool resolve() {
#if defined(_WIN32)
  HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
  if (!lib) return false;
  auto find = [&](const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
  };
#else
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  auto find = [&](const char* name) { return dlsym(lib, name); };
#endif
  bool complete = true;
  Symbols resolved;
#define CULEBRA_LLAMA_VULKAN_RESOLVE(name)                               \
  resolved.name = reinterpret_cast<decltype(&::name)>(find(#name)); \
  if (!resolved.name) complete = false;
  CULEBRA_LLAMA_VULKAN_SYMBOLS(CULEBRA_LLAMA_VULKAN_RESOLVE)
#undef CULEBRA_LLAMA_VULKAN_RESOLVE
  // A loader without one of the four is treated as no loader. It stays open:
  // the loader keeps process-wide state once it has been asked anything.
  if (!complete) return false;
  sym = resolved;
  return true;
}

// Once per process; every later forwarder reads the cached result.
bool loader_loaded() {
  static const bool ok = resolve();
  return ok;
}

}  // namespace

const char* culebra::llama::gpu_backend() { return "vulkan"; }

const char* culebra::llama::gpu_unavailable_reason() {
  return loader_loaded() ? "no Vulkan device" : "no Vulkan driver";
}

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char* pName) {
  return loader_loaded() ? sym.vkGetInstanceProcAddr(instance, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char* pName) {
  return sym.vkGetDeviceProcAddr(device, pName);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
  sym.vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer,
                                           VkBuffer srcBuffer, VkBuffer dstBuffer,
                                           uint32_t regionCount,
                                           const VkBufferCopy* pRegions) {
  sym.vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

}  // extern "C"
