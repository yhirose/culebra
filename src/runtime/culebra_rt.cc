// Translation unit that forces emission of all `culebra_runtime_*`
// helpers (declared `inline __attribute__((used))` in the headers)
// into the `libculebra_rt.a` static archive.
//
// The headers are designed for header-only embedding, where each
// embedder's TU gets weak/COMDAT copies of every runtime symbol. For
// AOT-built CLI binaries we also need a real archive to link against;
// this TU is the single canonical emission point referenced by the
// `culebra_rt` CMake target.

#include <culebra.h>
#include <stdlib_interp.h>

#ifdef CULEBRA_JIT_ENABLED
#include <runtime/runtime_aot.h>
#include <stdlib_jit.h>
#endif
