// Unit test for the value-neutral file-watching core (include/fswatcher.h).
// Runs entirely inside a temporary directory: create / modify / delete are
// driven directly and pulled back through fs_watch_next, with no interpreter
// or JIT linkage.
//
// Assertions are written the way the backends' own tests have to be. The OS
// decides how many events one change produces — FSEvents coalesces a burst
// into a single event carrying the union of the flags, inotify reports each
// change on its own — so a check waits for a *path* to show up and then looks
// at the kind, and never fixes the event count or the interleaving of two
// paths. Anything stricter would be testing the platform's batching.
//
// Built and run by CTest (see CMakeLists.txt).

#include <fswatcher.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fw = culebra::fswatch;
namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_OK(cond, err)                                          \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s (err: %s)\n", __FILE__,   \
                   __LINE__, #cond, (err).c_str());                  \
      ++g_failures;                                                  \
    }                                                                \
  } while (0)

// --- Harness ----------------------------------------------------------------

// A scratch directory that removes itself. Canonicalized because that is what
// the watcher reports: on macOS a /tmp root comes back as /private/tmp.
struct TempDir {
  fs::path path;
  explicit TempDir(const char* tag) {
    auto base = fs::temp_directory_path() /
                std::format("culebra_fswatch_{}_{}", tag, ::getpid());
    fs::remove_all(base);
    fs::create_directories(base);
    path = fs::canonical(base);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string operator/(std::string_view name) const {
    return (path / name).string();
  }
};

static void write_file(const std::string& p, std::string_view text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// Pull events until `path` shows up, or give up after `budget`. Returns the
// kind it carried. Every wait in this file goes through here rather than
// sleeping for a fixed time: the OS decides when an event lands, so polling
// until it does is both faster and immune to a slow machine.
static std::optional<fw::Kind> await_path(
    int64_t id, const std::string& path,
    std::chrono::milliseconds budget = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return std::nullopt;
    fw::FsEvent e;
    if (fw::fs_watch_next(id, e, left.count()) != fw::Next::Event)
      return std::nullopt;
    if (e.path == path) return e.kind;
  }
}

// --- Tests ------------------------------------------------------------------

static void test_create_modify_delete() {
  TempDir dir("cmd");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  auto file = dir / "a.txt";
  write_file(file, "one");
  auto created = await_path(id, file);
  CHECK(created.has_value());
  // A create followed by a write inside one FSEvents window arrives as a
  // single Created; inotify splits it. Either is a correct report of "this
  // path came into existence", so accept both rather than pin the platform.
  CHECK(created == fw::Kind::Created || created == fw::Kind::Modified);

  write_file(file, "one two");
  auto modified = await_path(id, file);
  CHECK(modified.has_value());

  fs::remove(file);
  auto deleted = await_path(id, file);
  CHECK(deleted == fw::Kind::Deleted);

  fw::fs_watch_close(id);
}

static void test_recursive() {
  TempDir dir("rec");
  fs::create_directories(dir.path / "sub");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  auto nested = dir / "sub/deep.txt";
  write_file(nested, "x");
  CHECK(await_path(id, nested).has_value());
  fw::fs_watch_close(id);
}

// A directory created after the watch starts must be watched too, including
// files that appear in it before the new watch is in place — the classic
// inotify race the walk-after-add closes.
static void test_new_subdirectory_is_watched() {
  TempDir dir("newsub");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  auto sub = dir / "late";
  fs::create_directories(sub);
  write_file(sub + "/inside.txt", "x");
  CHECK(await_path(id, sub + "/inside.txt").has_value());
  fw::fs_watch_close(id);
}

static void test_non_recursive_ignores_subtree() {
  TempDir dir("nonrec");
  fs::create_directories(dir.path / "sub");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), false, {}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  write_file(dir / "sub/hidden.txt", "x");
  auto top = dir / "top.txt";
  write_file(top, "x");
  // Drained rather than awaited: await_path discards what it isn't looking
  // for, so it could not tell a filtered nested write from a delivered one.
  // top.txt is written second, so its arrival bounds how long the nested one
  // had to show up.
  fw::FsEvent e;
  bool saw_top = false, saw_nested = false;
  while (!saw_top && fw::fs_watch_next(id, e, 5000) == fw::Next::Event) {
    if (e.path == top) saw_top = true;
    if (e.path.find("/sub/") != std::string::npos) saw_nested = true;
  }
  CHECK(saw_top);
  CHECK(!saw_nested);
  fw::fs_watch_close(id);
}

static void test_extension_filter() {
  TempDir dir("ext");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), true,
                                 {fw::normalize_ext("cul")}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  write_file(dir / "skipped.txt", "x");
  auto kept = dir / "kept.cul";
  write_file(kept, "x");
  CHECK(await_path(id, kept).has_value());

  // Everything queued up to the .cul file has been drained by now, so a
  // filtered path could only surface as a later event; drain what is left and
  // confirm none of it is the .txt.
  fw::FsEvent e;
  bool leaked_txt = false;
  while (fw::fs_watch_next(id, e, 200) == fw::Next::Event)
    if (e.path.ends_with(".txt")) leaked_txt = true;
  CHECK(!leaked_txt);
  fw::fs_watch_close(id);
}

static void test_normalize_ext() {
  CHECK(fw::normalize_ext("cul") == ".cul");
  CHECK(fw::normalize_ext(".cul") == ".cul");
  CHECK(fw::normalize_ext("") == "");
}

static void test_open_errors() {
  std::string err;
  TempDir dir("err");

  CHECK(fw::fs_watch_open((dir.path / "nope").string(), true, {}, err) == -1);
  CHECK(err.starts_with("FS.watch("));

  auto file = dir / "plain.txt";
  write_file(file, "x");
  err.clear();
  CHECK(fw::fs_watch_open(file, true, {}, err) == -1);
  CHECK(err.find("not a directory") != std::string::npos);
}

// A forged, stale or already-closed id must fail safely instead of reaching
// into freed memory (the IdRegistry generation check).
static void test_id_safety() {
  TempDir dir("ids");
  std::string err;
  int64_t id = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(id >= 0, err);
  if (id < 0) return;

  fw::fs_watch_close(id);
  fw::FsEvent e;
  CHECK(fw::fs_watch_next(id, e, 0) == fw::Next::Closed);  // stale
  fw::fs_watch_close(id);                               // idempotent
  fw::fs_watch_close(id + (int64_t{1} << 32));          // forged generation
  CHECK(fw::fs_watch_next(1234567, e, 0) == fw::Next::Closed);
}

// Closing a watch whose slot is then reused must not let the old id address
// the new watch.
static void test_slot_reuse_has_no_aba() {
  TempDir dir("aba");
  std::string err;
  int64_t first = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(first >= 0, err);
  fw::fs_watch_close(first);

  int64_t second = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK_OK(second >= 0, err);
  CHECK(second != first);
  fw::FsEvent e;
  CHECK(fw::fs_watch_next(first, e, 0) == fw::Next::Closed);
  fw::fs_watch_close(second);
}

// close_all is what a script run's teardown uses: a watch left open must be
// stopped and joined there, not left running into process teardown.
static void test_close_all() {
  TempDir dir("all");
  std::string err;
  int64_t a = fw::fs_watch_open(dir.path.string(), true, {}, err);
  int64_t b = fw::fs_watch_open(dir.path.string(), true, {}, err);
  CHECK(a >= 0 && b >= 0);

  fw::fs_watch_close_all();
  fw::FsEvent e;
  CHECK(fw::fs_watch_next(a, e, 0) == fw::Next::Closed);
  CHECK(fw::fs_watch_next(b, e, 0) == fw::Next::Closed);
  fw::fs_watch_close_all();  // idempotent
}

int main() {
#if defined(__APPLE__) || defined(__linux__)
  test_normalize_ext();
  test_open_errors();
  test_create_modify_delete();
  test_recursive();
  test_new_subdirectory_is_watched();
  test_non_recursive_ignores_subtree();
  test_extension_filter();
  test_id_safety();
  test_slot_reuse_has_no_aba();
  test_close_all();
#else
  // No backend on this platform: fs_watch_open must say so rather than
  // pretend to watch.
  std::string err;
  CHECK(fw::fs_watch_open(".", true, {}, err) == -1);
  CHECK(err.find("not supported") != std::string::npos);
#endif

  if (g_failures == 0) {
    std::puts("fswatcher_test: all checks passed");
    return 0;
  }
  std::fprintf(stderr, "fswatcher_test: %d check(s) failed\n", g_failures);
  return 1;
}
