// Value-neutral SQLite core for culebra. Mirrors http.h: this header carries
// no interpreter/JIT/Value types — only plain C++ structs and a cursor-style
// API that the three backends (interp / JIT / AOT) each adapt into culebra
// values. The interp builds Object rows directly; the JIT runtime builds
// JitObjects; both drive the same prepare → bind → step → column → finalize
// cursor exposed here.
//
// Why not cpp-sqlitelib (yhirose's own wrapper): its API encodes column types
// as C++ compile-time template parameters (execute<int,std::string>(...)) and
// hides sqlite3_stmt*. A dynamically-typed language only knows column types at
// runtime (sqlite3_column_type), so we wrap the raw sqlite3 C API at the right
// abstraction here instead — exactly as http.h wraps httplib for runtime-typed
// request/response.
//
// Linkage partitioning (identical to http.h's http_request choke): every
// function that calls sqlite3_* is the AOT "heavy dependency" anchor. The core
// runtime archive compiles them as weak stubs that reference no sqlite3 symbol;
// the culebra_rt_sqlite feature archive (which also bundles the amalgamation
// object) compiles strong real bodies, force-loaded only when the program uses
// SQLite. The in-process driver/JIT use the normal inline bodies and link the
// amalgamation directly.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite3.h"

#if defined(CULEBRA_RT_SQLITE_STRONG)
#define CULEBRA_RT_SQLITE_LINKAGE
#elif defined(CULEBRA_RT_SQLITE_WEAK)
#define CULEBRA_RT_SQLITE_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_SQLITE_LINKAGE inline
#endif

namespace culebra::sqlite {

// Column / bind value classes. Numeric codes intentionally match the SQLITE_*
// constants so the strong bodies can pass them straight through, but callers
// (the backends) treat this as a neutral enum.
enum class ColType : int {
  Integer = 1,  // SQLITE_INTEGER -> Long
  Float = 2,    // SQLITE_FLOAT   -> Float
  Text = 3,     // SQLITE_TEXT    -> String
  Blob = 4,     // SQLITE_BLOB    -> String (bytes)
  Null = 5,     // SQLITE_NULL    -> nil
};

// One result cell read from the current row. `text` (for Text/Blob) points into
// sqlite's own buffer and is valid only until the next step()/finalize() on the
// same statement — the backend copies it into a culebra String immediately.
struct Cell {
  ColType type = ColType::Null;
  int64_t i = 0;
  double d = 0.0;
  std::string_view text;
};

// One bound parameter value, produced by the backend from a culebra value.
struct BindVal {
  ColType type = ColType::Null;
  int64_t i = 0;
  double d = 0.0;
  std::string_view text;  // Text/Blob payload (copied by sqlite via _TRANSIENT)
};

// ---- Process-local handle tables -------------------------------------------
//
// Script-visible handles store an int64 index into these tables, never a raw
// sqlite3*/sqlite3_stmt* — a forged index is bounds-checked here and degrades
// to a graceful error, it can never be dereferenced (same soundness posture as
// the wrap.h foreign table and File's fd table). Tables are thread_local: a
// Database/Statement handle is __nonsendable__ (never crosses an isolate, i.e.
// never crosses a thread), so per-thread tables are correct and lock-free.
//
// The tables belong to the implementation, so the weak-stub branch leaves them
// out: it never interns a handle. Carrying them there would put the same
// `inline thread_local` in both the core archive and the force-loaded feature
// archive, and mingw's ld rejects the duplicate `.text$__tls_init` COMDAT
// instead of folding it (`multiple definition of 'TLS init function for …'`).

#if !defined(CULEBRA_RT_SQLITE_WEAK)

namespace detail {
inline thread_local std::vector<sqlite3*> g_dbs;
inline thread_local std::vector<int64_t> g_db_free;
inline thread_local std::vector<sqlite3_stmt*> g_stmts;
inline thread_local std::vector<int64_t> g_stmt_free;

inline int64_t db_intern(sqlite3* db) {
  if (!g_db_free.empty()) {
    int64_t id = g_db_free.back();
    g_db_free.pop_back();
    g_dbs[id] = db;
    return id;
  }
  g_dbs.push_back(db);
  return static_cast<int64_t>(g_dbs.size()) - 1;
}
inline sqlite3* db_get(int64_t id) {
  if (id < 0 || id >= static_cast<int64_t>(g_dbs.size())) return nullptr;
  return g_dbs[id];
}
inline int64_t stmt_intern(sqlite3_stmt* st) {
  if (!g_stmt_free.empty()) {
    int64_t id = g_stmt_free.back();
    g_stmt_free.pop_back();
    g_stmts[id] = st;
    return id;
  }
  g_stmts.push_back(st);
  return static_cast<int64_t>(g_stmts.size()) - 1;
}
inline sqlite3_stmt* stmt_get(int64_t id) {
  if (id < 0 || id >= static_cast<int64_t>(g_stmts.size())) return nullptr;
  return g_stmts[id];
}
}  // namespace detail

#endif  // !CULEBRA_RT_SQLITE_WEAK

// ---- Library info ----------------------------------------------------------

CULEBRA_RT_SQLITE_LINKAGE const char* libversion();

// ---- Connection ------------------------------------------------------------

// Opens (or creates) the database at `path` (":memory:" for an in-memory db).
// Returns a db handle id, or -1 with *err set on failure.
CULEBRA_RT_SQLITE_LINKAGE int64_t open_db(const std::string& path,
                                          std::string* err);

// Closes the connection and frees its table slot. Uses sqlite3_close_v2, which
// is safe even if statements are still live (the connection is reclaimed when
// the last one finalizes). Idempotent; a stale/forged id is ignored.
CULEBRA_RT_SQLITE_LINKAGE void close_db(int64_t db_id);

CULEBRA_RT_SQLITE_LINKAGE int64_t changes(int64_t db_id);
CULEBRA_RT_SQLITE_LINKAGE int64_t last_insert_rowid(int64_t db_id);

// ---- Statement -------------------------------------------------------------

// Compiles `sql` into a statement; returns a stmt handle id or -1 with *err.
CULEBRA_RT_SQLITE_LINKAGE int64_t prepare(int64_t db_id, const std::string& sql,
                                          std::string* err);

// Finalizes the statement and frees its slot. Idempotent.
CULEBRA_RT_SQLITE_LINKAGE void finalize(int64_t stmt_id);

// Resets a statement for re-execution (prepared-statement reuse) and clears
// bindings.
CULEBRA_RT_SQLITE_LINKAGE void reset(int64_t stmt_id);

// 1-based parameter index for a named parameter (":name"/"@name"/"$name"), or
// 0 if the statement has no such parameter.
CULEBRA_RT_SQLITE_LINKAGE int bind_index(int64_t stmt_id, const std::string& name);

// Number of parameters (?, ?NNN, :name, ...) in the statement.
CULEBRA_RT_SQLITE_LINKAGE int bind_count(int64_t stmt_id);

// Binds one value at 1-based `idx`. Returns false with *err on failure.
CULEBRA_RT_SQLITE_LINKAGE bool bind(int64_t stmt_id, int idx, const BindVal& v,
                                    std::string* err);

// Advances the cursor. Returns 1 if a row is ready, 0 when done, -1 on error
// (with *err set).
CULEBRA_RT_SQLITE_LINKAGE int step(int64_t stmt_id, std::string* err);

CULEBRA_RT_SQLITE_LINKAGE int column_count(int64_t stmt_id);
CULEBRA_RT_SQLITE_LINKAGE std::string column_name(int64_t stmt_id, int i);

// Reads column `i` of the current row (call only after step() returned 1).
CULEBRA_RT_SQLITE_LINKAGE Cell column(int64_t stmt_id, int i);

// ============================================================================
// Implementation. The weak-stub branch (core archive) must not reference any
// sqlite3_* symbol; it returns graceful error sentinels and is never actually
// reached at runtime (the SQLite namespace is unused in a no-SQLite binary).
// ============================================================================

#if defined(CULEBRA_RT_SQLITE_WEAK)

#define CULEBRA_SQLITE_STUB \
  "SQLite runtime not linked (no SQLite use detected at build)"

CULEBRA_RT_SQLITE_LINKAGE const char* libversion() { return "0.0.0"; }
CULEBRA_RT_SQLITE_LINKAGE int64_t open_db(const std::string&, std::string* err) {
  if (err) *err = CULEBRA_SQLITE_STUB;
  return -1;
}
CULEBRA_RT_SQLITE_LINKAGE void close_db(int64_t) {}
CULEBRA_RT_SQLITE_LINKAGE int64_t changes(int64_t) { return 0; }
CULEBRA_RT_SQLITE_LINKAGE int64_t last_insert_rowid(int64_t) { return 0; }
CULEBRA_RT_SQLITE_LINKAGE int64_t prepare(int64_t, const std::string&,
                                          std::string* err) {
  if (err) *err = CULEBRA_SQLITE_STUB;
  return -1;
}
CULEBRA_RT_SQLITE_LINKAGE void finalize(int64_t) {}
CULEBRA_RT_SQLITE_LINKAGE void reset(int64_t) {}
CULEBRA_RT_SQLITE_LINKAGE int bind_index(int64_t, const std::string&) { return 0; }
CULEBRA_RT_SQLITE_LINKAGE int bind_count(int64_t) { return 0; }
CULEBRA_RT_SQLITE_LINKAGE bool bind(int64_t, int, const BindVal&,
                                    std::string* err) {
  if (err) *err = CULEBRA_SQLITE_STUB;
  return false;
}
CULEBRA_RT_SQLITE_LINKAGE int step(int64_t, std::string* err) {
  if (err) *err = CULEBRA_SQLITE_STUB;
  return -1;
}
CULEBRA_RT_SQLITE_LINKAGE int column_count(int64_t) { return 0; }
CULEBRA_RT_SQLITE_LINKAGE std::string column_name(int64_t, int) { return {}; }
CULEBRA_RT_SQLITE_LINKAGE Cell column(int64_t, int) { return {}; }

#undef CULEBRA_SQLITE_STUB

#else  // strong (feature archive) or inline (driver / in-process JIT)

CULEBRA_RT_SQLITE_LINKAGE const char* libversion() {
  return sqlite3_libversion();
}

CULEBRA_RT_SQLITE_LINKAGE int64_t open_db(const std::string& path,
                                          std::string* err) {
  sqlite3* db = nullptr;
  int rc = sqlite3_open_v2(
      path.c_str(), &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, nullptr);
  if (rc != SQLITE_OK) {
    if (err) *err = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close_v2(db);  // free the (possibly partial) handle
    return -1;
  }
  // Wait up to 5s on a locked database rather than failing immediately.
  sqlite3_busy_timeout(db, 5000);
  return detail::db_intern(db);
}

CULEBRA_RT_SQLITE_LINKAGE void close_db(int64_t db_id) {
  sqlite3* db = detail::db_get(db_id);
  if (!db) return;
  sqlite3_close_v2(db);
  detail::g_dbs[db_id] = nullptr;
  detail::g_db_free.push_back(db_id);
}

CULEBRA_RT_SQLITE_LINKAGE int64_t changes(int64_t db_id) {
  sqlite3* db = detail::db_get(db_id);
  return db ? sqlite3_changes64(db) : 0;
}

CULEBRA_RT_SQLITE_LINKAGE int64_t last_insert_rowid(int64_t db_id) {
  sqlite3* db = detail::db_get(db_id);
  return db ? sqlite3_last_insert_rowid(db) : 0;
}

CULEBRA_RT_SQLITE_LINKAGE int64_t prepare(int64_t db_id, const std::string& sql,
                                          std::string* err) {
  sqlite3* db = detail::db_get(db_id);
  if (!db) {
    if (err) *err = "database is closed";
    return -1;
  }
  sqlite3_stmt* st = nullptr;
  int rc = sqlite3_prepare_v2(db, sql.data(),
                              static_cast<int>(sql.size()), &st, nullptr);
  if (rc != SQLITE_OK) {
    if (err) *err = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return -1;
  }
  return detail::stmt_intern(st);
}

CULEBRA_RT_SQLITE_LINKAGE void finalize(int64_t stmt_id) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) return;
  sqlite3_finalize(st);
  detail::g_stmts[stmt_id] = nullptr;
  detail::g_stmt_free.push_back(stmt_id);
}

CULEBRA_RT_SQLITE_LINKAGE void reset(int64_t stmt_id) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) return;
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
}

CULEBRA_RT_SQLITE_LINKAGE int bind_index(int64_t stmt_id,
                                         const std::string& name) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) return 0;
  return sqlite3_bind_parameter_index(st, name.c_str());
}

CULEBRA_RT_SQLITE_LINKAGE int bind_count(int64_t stmt_id) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  return st ? sqlite3_bind_parameter_count(st) : 0;
}

CULEBRA_RT_SQLITE_LINKAGE bool bind(int64_t stmt_id, int idx, const BindVal& v,
                                    std::string* err) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) {
    if (err) *err = "statement is finalized";
    return false;
  }
  int rc = SQLITE_OK;
  switch (v.type) {
    case ColType::Integer:
      rc = sqlite3_bind_int64(st, idx, v.i);
      break;
    case ColType::Float:
      rc = sqlite3_bind_double(st, idx, v.d);
      break;
    case ColType::Text:
      rc = sqlite3_bind_text64(st, idx, v.text.data(), v.text.size(),
                               SQLITE_TRANSIENT, SQLITE_UTF8);
      break;
    case ColType::Blob:
      rc = sqlite3_bind_blob64(st, idx, v.text.data(), v.text.size(),
                               SQLITE_TRANSIENT);
      break;
    case ColType::Null:
      rc = sqlite3_bind_null(st, idx);
      break;
  }
  if (rc != SQLITE_OK) {
    sqlite3* db = sqlite3_db_handle(st);
    if (err) *err = db ? sqlite3_errmsg(db) : "bind failed";
    return false;
  }
  return true;
}

CULEBRA_RT_SQLITE_LINKAGE int step(int64_t stmt_id, std::string* err) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) {
    if (err) *err = "statement is finalized";
    return -1;
  }
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) return 1;
  if (rc == SQLITE_DONE) return 0;
  sqlite3* db = sqlite3_db_handle(st);
  if (err) *err = db ? sqlite3_errmsg(db) : "step failed";
  return -1;
}

CULEBRA_RT_SQLITE_LINKAGE int column_count(int64_t stmt_id) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  return st ? sqlite3_column_count(st) : 0;
}

CULEBRA_RT_SQLITE_LINKAGE std::string column_name(int64_t stmt_id, int i) {
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) return {};
  const char* n = sqlite3_column_name(st, i);
  return n ? std::string(n) : std::string();
}

CULEBRA_RT_SQLITE_LINKAGE Cell column(int64_t stmt_id, int i) {
  Cell cell;
  sqlite3_stmt* st = detail::stmt_get(stmt_id);
  if (!st) return cell;
  int t = sqlite3_column_type(st, i);
  switch (t) {
    case SQLITE_INTEGER:
      cell.type = ColType::Integer;
      cell.i = sqlite3_column_int64(st, i);
      break;
    case SQLITE_FLOAT:
      cell.type = ColType::Float;
      cell.d = sqlite3_column_double(st, i);
      break;
    case SQLITE_TEXT: {
      cell.type = ColType::Text;
      const auto* p = static_cast<const char*>(
          static_cast<const void*>(sqlite3_column_text(st, i)));
      int n = sqlite3_column_bytes(st, i);
      cell.text = std::string_view(p ? p : "", p ? n : 0);
      break;
    }
    case SQLITE_BLOB: {
      cell.type = ColType::Blob;
      const auto* p = static_cast<const char*>(sqlite3_column_blob(st, i));
      int n = sqlite3_column_bytes(st, i);
      cell.text = std::string_view(p ? p : "", p ? n : 0);
      break;
    }
    default:  // SQLITE_NULL
      cell.type = ColType::Null;
      break;
  }
  return cell;
}

#endif  // CULEBRA_RT_SQLITE_WEAK

}  // namespace culebra::sqlite
