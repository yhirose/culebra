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
#include <stdlib_rt.h>
// The __Foreign wrap fixture is NOT here: its static initializer would keep
// the wrap metadata behind it in every binary. It has its own archive
// (culebra_rt_foreign.cc), force-loaded when the program names __Foreign.

// The AOT entry itself, which only a build that can emit an object needs.
#ifdef CULEBRA_JIT_ENABLED
#include <runtime/runtime_aot.h>
#endif
