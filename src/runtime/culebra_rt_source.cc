// The core runtime with Isolate.spawn_source in it — the `spawn_source` AOT
// axis.
//
// The source runs on the parser, the bytecode compiler and the executor
// (vm/source_isolate.h), which a binary carries only when the program names
// spawn_source: they are the front end, about as large as the rest of the
// runtime. They reach the whole runtime, so they cannot ride an archive of
// their own beside the core's object without defining its symbols a second
// time. This is that object again with them added, and a program that names
// spawn_source links it in place of the core's culebra_rt.cc.o. Installing is
// a static initializer, as the wrap fixtures' registrars are.
#include "culebra_rt.cc"

#include <vm/source_isolate.h>

namespace {
const bool installed = (culebra::vm::install_source_isolate(), true);
}
