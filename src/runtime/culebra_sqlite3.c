// The SQLite amalgamation as culebra compiles it. The options live here, not
// in CMake, so an embedding host that compiles this file gets the same SQLite
// the `culebra` driver runs (docs/deployment.md, Building your host program).

#define SQLITE_THREADSAFE 1
#define SQLITE_DQS 0
#define SQLITE_OMIT_LOAD_EXTENSION
#define SQLITE_DEFAULT_FOREIGN_KEYS 1
#define SQLITE_OMIT_DEPRECATED
#define SQLITE_OMIT_SHARED_CACHE
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_ENABLE_FTS5
#define SQLITE_ENABLE_RTREE

#include "../../vendor/sqlite/sqlite3.c"
