#pragma once

// Emission gate for `culebra_runtime_*` helpers.
//
// Header-only / JIT path (default): each TU that includes a runtime
// header gets `inline __attribute__((used))` definitions. The
// `used` attribute keeps the symbols through `-O` dead-code passes
// so ORC JIT's `DynamicLibrarySearchGenerator` can resolve them
// from the host process via -rdynamic.
//
// Static-archive path: `src/runtime/culebra_rt.cc` defines
// `CULEBRA_RT_DEFINE_RUNTIME` before including the headers, so the
// helpers are emitted as plain extern-C definitions. Dropping
// `__attribute__((used))` lifts the implicit `.no_dead_strip`
// section flag that would otherwise pin every helper into AOT
// binaries — `cc -Wl,-dead_strip` can now drop runtime helpers a
// program never references.

#ifdef CULEBRA_RT_DEFINE_RUNTIME
#define CULEBRA_RT_KEEP
#define CULEBRA_RT_INLINE
#else
#define CULEBRA_RT_KEEP __attribute__((used))
#define CULEBRA_RT_INLINE inline
#endif

// `CULEBRA_RT_NO_TENSOR` flips tensor entry points into nullptr / no-op
// stubs so the static reachability chain from `culebra_runtime_num_add`
// → `_try_tensor_binop` → `culebra::tensor_binop` → cblas is broken.
// `src/runtime/culebra_rt.cc` builds the regular `libculebra_rt.a`;
// the same source compiled with this macro becomes
// `libculebra_rt_no_tensor.a`, which AOT-built CLIs that never
// reference Tensor link against (skipping Accelerate / BLAS entirely).
