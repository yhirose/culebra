#include "source_dir.h"

namespace culebra {

const char* source_dir() {
#ifdef CULEBRA_SOURCE_DIR
  return CULEBRA_SOURCE_DIR;
#else
  return "";
#endif
}

}  // namespace culebra
