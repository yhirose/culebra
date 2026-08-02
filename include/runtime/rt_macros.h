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

// Feature-archive path: a binding TU that reaches wrap.h (Webview, Scene) gets
// the same runtime headers, and `used` made it emit its own copy of every
// helper — 348 of them in the Webview archive, next to the core archive that
// already defines all of them. It resolves them from there at link time
// instead, so the attribute comes off here and the compiler emits only what it
// cannot inline away. ELF and Mach-O were folding the duplicates; PE has no
// weak external and ld calls each one a multiple definition, which the Webview
// link fragment used to wave through with --allow-multiple-definition.

#ifdef CULEBRA_RT_DEFINE_RUNTIME
#define CULEBRA_RT_KEEP
#define CULEBRA_RT_INLINE
#elif defined(CULEBRA_RT_FEATURE_BUILD)
#define CULEBRA_RT_KEEP
#define CULEBRA_RT_INLINE inline
#else
#define CULEBRA_RT_KEEP __attribute__((used))
#define CULEBRA_RT_INLINE inline
#endif

// The cblas / OpenSSL reachability chains are broken per-feature by a
// single weak choke stub in the core archive (culebra::tensor_eval_node,
// culebra::http::http_request), with the strong body force-loaded from a
// feature object only when the program uses it. See the
// CULEBRA_RT_TENSOR_EVAL_* / CULEBRA_RT_HTTP_REQUEST_* gates and the
// force-load wiring in `culebra build` (src/main.cc).
