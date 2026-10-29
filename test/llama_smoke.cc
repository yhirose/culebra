// The Llama build, with no model. The static llama.cpp links, its backends come
// up (the system info names the CPU features it was built with), and a model
// that cannot be loaded is reported as the wrapper's ModelLoadError.
#include "cpp-llamalib.h"

#include <cstdio>
#include <exception>

int main() {
  const char *info = llama_print_system_info();
  if (!info || !*info) {
    std::fprintf(stderr, "llama_print_system_info answered nothing\n");
    return 1;
  }
  std::printf("%s\n", info);

  try {
    llamalib::Llama missing("/nonexistent/model.gguf");
    std::fprintf(stderr, "a missing model was loaded\n");
    return 1;
  } catch (const llamalib::ModelLoadError &) {
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "the wrong error for a missing model: %s\n", e.what());
    return 1;
  }
}
