// SQLite feature object for the linear-scaling AOT runtime. Emits the strong
// definitions of every culebra::sqlite::* function that touches sqlite3_* — the
// lone SQLite dependency choke — which the AOT link force-loads only when the
// program uses SQLite, overriding the weak stubs in the core archive. This
// archive also bundles the amalgamation object (vendor/sqlite/sqlite3.c, added
// in CMake), so a force-loaded culebra_rt_sqlite is fully self-contained and a
// program that never opens a database links neither this object nor sqlite3.
//
// CULEBRA_RT_SQLITE_STRONG (set by CMake on this target) makes the wrappers
// strong, non-inline definitions; sqlite.h is value-neutral (no
// interpreter/JIT types), so this TU stays small.

#include <stdlib/sqlite.h>
