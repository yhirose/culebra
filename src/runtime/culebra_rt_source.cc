// Isolate.spawn_source in a built binary — the `spawn_source` AOT axis.
//
// The source runs on the parser, the bytecode compiler and the executor
// (vm/source_isolate.h), which a binary carries only when the program names
// spawn_source: they are the front end, about as large as the rest of the
// runtime. Installing it is a static initializer, as the wrap fixtures'
// registrars are, so this archive is force-loaded rather than reached by name.
#include <vm/source_isolate.h>

namespace {
const bool installed = (culebra::vm::install_source_isolate(), true);
}
