// Deterministic, offline unit test for the value-neutral SQLite core
// (include/stdlib/sqlite.h). Exercises the cursor API the three backends drive:
// open -> prepare -> bind -> step -> column, runtime type mapping, named and
// positional parameters, error reporting, and changes()/last_insert_rowid().
//
// Built and run by CTest (see CMakeLists.txt). No interpreter/JIT linkage.

#include <stdlib/sqlite.h>

#include <cassert>
#include <cstdio>
#include <string>

namespace sq = culebra::sqlite;

static int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                   __LINE__, #cond);                             \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

// Bind helpers for the neutral BindVal.
static sq::BindVal vint(int64_t v) {
  return sq::BindVal{sq::ColType::Integer, v, 0.0, {}};
}
static sq::BindVal vtext(std::string_view v) {
  return sq::BindVal{sq::ColType::Text, 0, 0.0, v};
}

// Run a parameterless DML/DDL statement; assert it completes.
static void exec(int64_t db, const std::string& sql) {
  std::string err;
  int64_t st = sq::prepare(db, sql, &err);
  CHECK(st >= 0);
  if (st < 0) {
    std::fprintf(stderr, "  prepare error: %s\n", err.c_str());
    return;
  }
  int rc = sq::step(st, &err);
  CHECK(rc == 0);  // DONE, no rows
  sq::finalize(st);
}

int main() {
  std::string err;

  // ---- open in-memory ----
  int64_t db = sq::open_db(":memory:", &err);
  CHECK(db >= 0);

  // ---- DDL + runtime type mapping across all 5 column classes ----
  exec(db,
       "CREATE TABLE t(id INTEGER, name TEXT, score REAL, data BLOB, note TEXT)");
  exec(db,
       "INSERT INTO t VALUES(1, 'Alice', 9.5, x'00ff01', NULL)");

  {
    int64_t st = sq::prepare(db, "SELECT id, name, score, data, note FROM t", &err);
    CHECK(st >= 0);
    CHECK(sq::column_count(st) == 5);
    CHECK(sq::column_name(st, 0) == "id");
    CHECK(sq::column_name(st, 1) == "name");
    CHECK(sq::step(st, &err) == 1);  // a row

    sq::Cell c0 = sq::column(st, 0);
    CHECK(c0.type == sq::ColType::Integer);
    CHECK(c0.i == 1);

    sq::Cell c1 = sq::column(st, 1);
    CHECK(c1.type == sq::ColType::Text);
    CHECK(c1.text == "Alice");

    sq::Cell c2 = sq::column(st, 2);
    CHECK(c2.type == sq::ColType::Float);
    CHECK(c2.d == 9.5);

    sq::Cell c3 = sq::column(st, 3);
    CHECK(c3.type == sq::ColType::Blob);
    CHECK(c3.text.size() == 3);
    CHECK(static_cast<unsigned char>(c3.text[0]) == 0x00);
    CHECK(static_cast<unsigned char>(c3.text[1]) == 0xff);
    CHECK(static_cast<unsigned char>(c3.text[2]) == 0x01);

    sq::Cell c4 = sq::column(st, 4);
    CHECK(c4.type == sq::ColType::Null);

    CHECK(sq::step(st, &err) == 0);  // done
    sq::finalize(st);
  }

  // ---- positional binding (?) + changes() + last_insert_rowid() ----
  {
    int64_t st = sq::prepare(db, "INSERT INTO t(id, name) VALUES(?, ?)", &err);
    CHECK(st >= 0);
    CHECK(sq::bind_count(st) == 2);
    CHECK(sq::bind(st, 1, vint(2), &err));
    CHECK(sq::bind(st, 2, vtext("Bob"), &err));
    CHECK(sq::step(st, &err) == 0);
    CHECK(sq::changes(db) == 1);
    CHECK(sq::last_insert_rowid(db) == 2);
    sq::finalize(st);
  }

  // ---- named binding (:name) + prepared reuse via reset() ----
  {
    int64_t st = sq::prepare(db, "SELECT name FROM t WHERE id = :id", &err);
    CHECK(st >= 0);
    int idx = sq::bind_index(st, ":id");
    CHECK(idx == 1);
    CHECK(sq::bind_index(st, ":nope") == 0);

    CHECK(sq::bind(st, idx, vint(2), &err));
    CHECK(sq::step(st, &err) == 1);
    CHECK(sq::column(st, 0).text == "Bob");

    sq::reset(st);
    CHECK(sq::bind(st, idx, vint(1), &err));
    CHECK(sq::step(st, &err) == 1);
    CHECK(sq::column(st, 0).text == "Alice");
    sq::finalize(st);
  }

  // ---- error path: prepare of invalid SQL reports sqlite's message ----
  {
    err.clear();
    int64_t st = sq::prepare(db, "SELECT * FROM no_such_table", &err);
    CHECK(st < 0);
    CHECK(err.find("no_such_table") != std::string::npos);
  }

  // ---- error path: constraint violation surfaces on step ----
  {
    exec(db, "CREATE TABLE u(id INTEGER PRIMARY KEY)");
    exec(db, "INSERT INTO u VALUES(1)");
    int64_t st = sq::prepare(db, "INSERT INTO u VALUES(1)", &err);
    CHECK(st >= 0);
    err.clear();
    int rc = sq::step(st, &err);
    CHECK(rc == -1);
    CHECK(!err.empty());
    sq::finalize(st);
  }

  sq::close_db(db);

  // ---- stale handle is graceful, not a crash ----
  CHECK(sq::prepare(db, "SELECT 1", &err) == -1);
  sq::close_db(db);  // idempotent / no-op on a forged id
  CHECK(sq::prepare(999999, "SELECT 1", &err) == -1);

  // ---- no ABA on slot reuse: g_dbs/g_stmts are IdRegistry, so a slot freed by
  // close_db()/finalize() gets a bumped generation on reuse. A captured id from
  // the closed db/stmt must not resolve to whatever new one lands on the same
  // slot. ----
  {
    int64_t db1 = sq::open_db(":memory:", &err);
    CHECK(db1 >= 0);
    sq::close_db(db1);
    int64_t db2 = sq::open_db(":memory:", &err);
    CHECK(db2 >= 0);
    CHECK(db2 != db1);  // likely reuses db1's slot, but the id itself differs
    CHECK(sq::prepare(db1, "SELECT 1", &err) == -1);  // old id: dead
    int64_t st2 = sq::prepare(db2, "SELECT 1", &err);
    CHECK(st2 >= 0);  // new id on the same slot: live

    int64_t stmt1 = st2;
    sq::finalize(stmt1);
    int64_t stmt2 = sq::prepare(db2, "SELECT 2", &err);
    CHECK(stmt2 >= 0);
    CHECK(stmt2 != stmt1);  // likely reuses stmt1's slot, but the id differs
    err.clear();
    CHECK(sq::step(stmt1, &err) == -1);  // old id: dead
    CHECK(err == "statement is finalized");
    CHECK(sq::step(stmt2, &err) == 1);  // new id on the same slot: live
    CHECK(sq::column(stmt2, 0).i == 2);
    sq::finalize(stmt2);

    sq::close_db(db2);
  }

  if (g_failures == 0) {
    std::printf("sqlite_test: all checks passed (sqlite %s)\n",
                sq::libversion());
    return 0;
  }
  std::fprintf(stderr, "sqlite_test: %d failure(s)\n", g_failures);
  return 1;
}
