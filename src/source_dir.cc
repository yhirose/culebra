#include "source_dir.h"

#include <cstdlib>

namespace culebra {

const char* source_dir() {
#ifdef CULEBRA_SOURCE_DIR
  return CULEBRA_SOURCE_DIR;
#else
  return "";
#endif
}

std::string resolved_source_dir() {
  const char* home = std::getenv("CULEBRA_HOME");
  return (home && *home) ? home : source_dir();
}

}  // namespace culebra
