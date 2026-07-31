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

// Link anchor for the Shared.new view readers — the AOT counterpart of the one
// in main.cc, which explains the shape. A TU that includes wrap.h (a `culebra
// wrap` extension, or the Webview binding's feature archive) sees only
// interpreter.h's forward declarations, so its out-of-line eval_property copy
// leaves an undefined reference. main.cc anchors the driver and wrap links;
// this archive is what an AOT link has instead. Internal linkage, so the two
// anchors cannot collide if a future link ever holds both.
namespace {
[[gnu::used]] void* const _shared_val_reader_anchor_rt[] = {
    reinterpret_cast<void*>(&culebra::shared_val_get_prop),
    reinterpret_cast<void*>(&culebra::shared_val_get_index),
    reinterpret_cast<void*>(&culebra::shared_val_make_iter),
};
}  // namespace
