#pragma once

// Emission gate for `culebra_runtime_*` helpers.
//
// Header-only / JIT path (default): each TU that includes a runtime
// header gets `inline __attribute__((used))` definitions. The
// `used` attribute keeps the symbols through `-O` dead-code passes
// so ORC JIT's `DynamicLibrarySearchGenerator` can resolve them
// from the host process via -rdynamic.
//
// Static-archive path: `src/runtime/culebra_rt.cc` is compiled with
// `CULEBRA_RT_DEFINE_RUNTIME` (CMake's culebra_rt target), so the
// helpers are emitted as plain extern-C definitions. Dropping
// `__attribute__((used))` lifts the implicit `.no_dead_strip`
// section flag that would otherwise pin every helper into AOT
// binaries — `cc -Wl,-dead_strip` can now drop runtime helpers a
// program never references.

// Feature-archive path: a `culebra_rt_*` archive, force-loaded beside the core
// archive into an AOT binary (CULEBRA_RT_FEATURE_ARCHIVE, set by CMake on those
// targets). Reaching wrap.h pulls these headers in, and `used` then forced out
// a copy of every helper next to the core archive's real ones: 348 in the
// Webview archive. Dropping the attribute leaves ordinary `inline`, so nothing
// is emitted for a use the compiler inlines — which today is all of them. That
// is an optimizer-dependent outcome, not a contract: at -O0 four copies come
// back, and each one is a multiple definition to PE's ld, which has no weak
// external to fold it with (ELF and Mach-O fold silently, so only Windows would
// report it). tools/check_rt_archive_tls.sh checks the result instead of
// trusting it.
//
// Only the `culebra_runtime_*` ABI helpers — the ones codegen names — take
// the pair. Internal `_jit_*` / `_culebra_*` helpers are plain `inline`
// everywhere: a strong archive definition against a feature TU's COMDAT copy
// is the one shape PE's ld refuses (`_jit_handle_bind_method`, mingw GCC 16),
// and COMDAT against COMDAT folds. tools/check_rt_keep_scope.sh ratchets it.
//
// Scoped to the archive targets, and never to a source file: the binding TUs
// (culebra_rt_webview.cc and its siblings) compile into BOTH the archive and
// the driver, and a source-file property reaches every target that names the
// file. Suppressing `used` in the driver strips the JIT's own helpers out of
// the image it resolves them from — tools/check_jit_host_symbols.sh is the
// ratchet, and its header records what that cost. That is why this is a
// separate flag from CULEBRA_RT_FEATURE_BUILD (rt_shared_tls.h), which does
// belong on the sources: borrowing a thread_local is right in both places,
// borrowing a helper is not.

#ifdef CULEBRA_RT_DEFINE_RUNTIME
#define CULEBRA_RT_KEEP
#define CULEBRA_RT_INLINE
#elif defined(CULEBRA_RT_FEATURE_ARCHIVE)
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
