// The `__Foreign.Counter` / `__Foreign.Box` wrap fixture (foreign_binding.h)
// as its own archive — the `__Foreign` AOT axis.
//
// The fixture is what tests/test_foreign.cul and tests/test_borrow.cul are
// written against, and it is deliberately shaped like the TU a user writes
// for their own class, so the AOT lane exercises the same wrap machinery a
// `culebra wrap` build ships. It cannot ride namespace-group dead-stripping
// like Proc or the Canvas decoders do: `wrap<T>(...)` registers through a
// static initializer, so this TU's static-init entry holds a reference the
// link can never drop, and every binary carried the fixture's class/method
// metadata and the wrap template instantiations behind it (~64 KB) whether or
// not the program names `__Foreign`.
#include <foreign_binding.h>

// The registrar variable belongs to the TU, not the header — see wrap.h.
namespace {
const bool registered = culebra::register_foreign_fixture();
}
