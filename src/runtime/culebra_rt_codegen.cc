// The `CodeGen.Module` wrap binding (codegen_binding.h) as its own archive --
// the `CodeGen` AOT axis.
//
// Like `__Foreign` (see culebra_rt_foreign.cc), this cannot ride
// namespace-group dead-stripping: `wrap<T>(...)` registers through a static
// initializer, so this TU's static-init entry holds a reference the link can
// never drop, and every binary would carry the Module class/method metadata
// and the wrap template instantiations behind it whether or not the program
// names `CodeGen`.

#include <codegen_binding.h>

// The registrar variable belongs to the TU, not the header -- see wrap.h.
namespace {
const bool registered = culebra::register_codegen_binding();
}
