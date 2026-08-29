#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "shared.h"
#include "stdout_capture.h"  // StdoutCapture — shared with IO.capture

namespace culebra {

// The engine a test run happens on (test_engine.h implements one per engine).
//
// `test` and `parametrize` are culebra source — src/preambles/test_ambient.cul
// — so the registry is a culebra Array both engines build the same way, and
// everything the runner needs from a value except calling it has already been
// reduced to a String or an Array there. What is left is this: run a program,
// read a global, walk an Array / Object, and call a function. The two value
// models never meet.
//
// A value is named by a `ValueRef` into the host's own store; the host keeps
// it alive until `release_to` rolls the store back, which is how a fixture's
// `drop` fires when the test that used it ends rather than at the end of the
// run.
using ValueRef = int32_t;
inline constexpr ValueRef kNoValue = -1;

// Why a file never got as far as registering anything. A parse or load failure
// carries the position it was raised at, which is what the JSON reporter's
// `file_error` event reports.
struct TestFileError {
  std::string kind;
  std::string message;
  int64_t line = 0;
  int64_t col = 0;
};

class TestHost {
 public:
  virtual ~TestHost() = default;

  // Process-wide setup, once, before any file.
  virtual bool begin_run(std::vector<std::string>& msgs) = 0;
  // Open a scope of its own for this file and run it there, with `test` /
  // `parametrize` and a registry of its own bound first. A test file is a
  // program: what it writes at the top level is the file's, so a `mut range
  // = 5` in one file leaves `range` a function in every other.
  virtual bool run_file(const std::string& path, const std::string& source,
                        TestFileError& err) = 0;
  // Drop that scope. Called once per run_file, after the file's own tests
  // have run — they are what the scope was being kept alive for.
  virtual void end_file() = 0;

  // A top-level binding of the run: the registry, or a fixture by name.
  virtual ValueRef global(std::string_view name) = 0;
  virtual bool is_function(ValueRef v) = 0;
  virtual bool is_nil(ValueRef v) = 0;
  virtual int64_t array_size(ValueRef v) = 0;
  virtual ValueRef array_at(ValueRef v, int64_t i) = 0;
  virtual ValueRef member(ValueRef v, std::string_view name) = 0;
  virtual std::string as_string(ValueRef v) = 0;
  // Call, propagating whatever the program threw. The result is owned by the
  // host's store, not by the caller.
  virtual ValueRef call(ValueRef fn, const std::vector<ValueRef>& args) = 0;

  // Called from inside a `catch (...)`: if the in-flight exception is this
  // engine's "a program threw a value", fill kind / message and return true.
  // The engines disagree on the type — the interpreter throws the value, the
  // compiled lanes wrap it — and only they can rethrow to inspect it.
  virtual bool describe_current_throw(std::string& kind,
                                      std::string& message) = 0;

  virtual size_t mark() = 0;
  virtual void release_to(size_t mark) = 0;
};

// The two names the ambient binds that the runner reads back: the registry,
// and the helper that says which parameters a function takes fixtures for. The
// second is the ambient's own — the registry already carries the answer for a
// test, and asking the same code for a fixture is what keeps the two from
// disagreeing about what a `**kwargs` rest or a keyword-only parameter means.
inline constexpr std::string_view kTestRegistryName = "__TestRegistry";
inline constexpr std::string_view kTestParamsName = "__test_params";

// Convert a runtime error line (1-based) to a user-file line, returning
// 0 for unknown locations. Preamble offset removed in Phase 2, when the
// interp test path stopped prepending stdlib source.
inline int to_user_line(int64_t raw) {
  return raw > 0 ? static_cast<int>(raw) : 0;
}

// Snippet of `source` around `line` (1-based, in user-file coordinates)
// with `context` lines on either side. Returns "" if line is out of range.
inline std::string source_snippet(const std::string& source, int line,
                                   int context = 2) {
  if (line <= 0 || source.empty()) return {};
  int start_line = std::max(1, line - context);
  int end_line = line + context;
  std::string out;
  int cur = 1;
  size_t pos = 0;
  while (pos < source.size() && cur < start_line) {
    auto nl = source.find('\n', pos);
    if (nl == std::string::npos) return out;
    pos = nl + 1;
    cur++;
  }
  while (pos < source.size() && cur <= end_line) {
    auto nl = source.find('\n', pos);
    auto line_end = (nl == std::string::npos) ? source.size() : nl;
    out += std::format("{:>3}{} {}\n", cur, (cur == line ? '>' : ' '),
                       source.substr(pos, line_end - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
    cur++;
  }
  return out;
}

// Whether a program being loaded is one `culebra test` runs. Binding the two
// values is only half the job — a bare name is resolved before anything runs
// — so the runner turns this on, and `culebra lint` turns it on per file for
// the ones it would have run.
inline void set_test_ambients(bool on) {
  static constexpr std::string_view kNames[] = {"test", "parametrize"};
  ambient_globals() = on ? std::span<const std::string_view>(kNames)
                         : std::span<const std::string_view>{};
}

// Default file matcher: `test_*.cul` (the convention for unit tests).
inline bool default_test_cul_matcher(const std::filesystem::path& p) {
  auto name = p.filename().string();
  return name.starts_with("test_") && p.extension() == ".cul";
}

// Discover files under `roots`. A path that is a directory is walked
// recursively for files satisfying `match`; a path that is a regular
// file is included as-is (the matcher is not applied to explicit file
// arguments, so `culebra test foo.cul` / `--doc foo.md` always run).
// Returns absolute paths, deduplicated and sorted. The default matcher
// selects `test_*.cul`; the doctest runner passes a `.md` matcher.
using FileMatcher = std::function<bool(const std::filesystem::path&)>;
inline std::vector<std::filesystem::path> discover_test_files(
    const std::vector<std::string>& roots,
    const FileMatcher& match = default_test_cul_matcher) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  auto add = [&](const fs::path& p) {
    // weakly_canonical's throwing overload would abort the entire
    // discovery on a single unstatable symlink or permission-denied
    // segment. Use the ec form and fall back to the un-canonicalized
    // path so the file at least gets a chance to run (the file path
    // is informative even when its parents can't be resolved).
    std::error_code canon_ec;
    auto canon = fs::weakly_canonical(p, canon_ec);
    if (canon_ec) canon = p;
    for (const auto& existing : out) {
      if (existing == canon) return;
    }
    out.push_back(canon);
  };
  std::vector<std::string> effective = roots;
  if (effective.empty()) effective.push_back(".");
  for (const auto& root : effective) {
    fs::path p(root);
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
      add(p);
      continue;
    }
    if (!fs::is_directory(p, ec)) {
      std::cerr << "culebra test: skipping non-existent path '" << root
                << "'\n";
      continue;
    }
    std::error_code walk_ec;
    auto it = fs::recursive_directory_iterator(p, walk_ec);
    for (; it != fs::end(it); it.increment(walk_ec)) {
      if (walk_ec) {
        std::cerr << "culebra test: warning: descent into '"
                  << it->path().string() << "' failed: "
                  << walk_ec.message() << "\n";
        walk_ec.clear();
        continue;
      }
      std::error_code stat_ec;
      if (!it->is_regular_file(stat_ec)) continue;
      if (match(it->path())) {
        add(it->path());
      }
    }
    if (walk_ec) {
      std::cerr << "culebra test: warning: discovery in '" << root
                << "' aborted early: " << walk_ec.message() << "\n";
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Output format for the runner. `Default` emits human-readable lines;
// `Json` emits one JSON object per line (NDJSON) for machine consumption.
enum class Reporter { Default, Json };

// Result of running all collected tests. `errored_files` is kept
// separate from `failed` so a syntax error in one file doesn't
// inflate the per-test failure count.
struct TestRunSummary {
  int passed = 0;
  int failed = 0;
  int errored_files = 0;
  // Files the runner actually ran. A suite of plain scripts registers no
  // tests, so `passed` says nothing about coverage there and this does.
  int files = 0;
  std::vector<std::string> failure_messages;
};

// `--bail [n]`: whether the run has seen the failures it was told to stop at.
// One predicate, because three sites ask — the test loop, the file loop around
// it, and the `bailed` the report closes with.
inline bool bailed_out(const TestRunSummary& summary, int bail_after) {
  return bail_after > 0 && summary.failed >= bail_after;
}

// One entry of the registry the ambient built, resolved to the pieces the
// runner walks: the function to call, the parameters to inject fixtures for
// (empty when `args` supplies them), and the fixed arguments a `@parametrize`
// case carries. `source` is not the ambient's — the runner knows which file
// was running when the entry appeared.
struct TestCase {
  std::string name;
  std::string source_path;
  ValueRef fn = kNoValue;
  std::vector<std::string> params;
  ValueRef args = kNoValue;  // kNoValue / nil: inject fixtures instead
};

inline std::vector<std::string> strings_at(TestHost& host, ValueRef arr) {
  std::vector<std::string> out;
  for (int64_t i = 0; i < host.array_size(arr); i++)
    out.push_back(host.as_string(host.array_at(arr, i)));
  return out;
}

// The parameters `fn` takes fixtures for, through the ambient's own helper.
inline std::vector<std::string> fixture_params(TestHost& host, ValueRef fn) {
  auto helper = host.global(kTestParamsName);
  if (helper == kNoValue) return {};
  return strings_at(host, host.call(helper, {fn}));
}

// Per-test memoized fixture resolution. `cache` keeps the same instance
// across multiple mentions of one name; `visited` catches cycles.
inline ValueRef resolve_fixture(TestHost& host, const std::string& name,
                                std::set<std::string>& visited,
                                std::map<std::string, ValueRef>& cache) {
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  auto val = host.global(name);
  if (val == kNoValue) {
    throw CulebraError("NameError", "fixture '" + name + "' not found in env");
  }
  if (visited.contains(name)) {
    std::string chain;
    for (const auto& v : visited) {
      if (!chain.empty()) chain += " → ";
      chain += v;
    }
    chain += " → " + name;
    throw CulebraError("CycleError", "fixture cycle detected: " + chain);
  }
  if (!host.is_function(val)) {
    cache.emplace(name, val);
    return val;
  }
  visited.insert(name);
  std::vector<ValueRef> args;
  for (const auto& p : fixture_params(host, val))
    args.push_back(resolve_fixture(host, p, visited, cache));
  visited.erase(name);
  auto result = host.call(val, args);
  cache.emplace(name, result);
  return result;
}

// Read the registry the ambient built into `out`, tagging entries that
// appeared since `from` with the file that was running.
inline void collect_new_cases(TestHost& host, size_t from,
                              const std::string& source_path,
                              std::vector<TestCase>& out) {
  auto reg = host.global(kTestRegistryName);
  if (reg == kNoValue) return;
  auto n = static_cast<size_t>(host.array_size(reg));
  for (size_t i = from; i < n; i++) {
    auto e = host.array_at(reg, static_cast<int64_t>(i));
    TestCase c;
    c.name = host.as_string(host.member(e, "name"));
    c.source_path = source_path;
    c.fn = host.member(e, "fn");
    c.params = strings_at(host, host.member(e, "params"));
    auto args = host.member(e, "args");
    if (!host.is_nil(args)) c.args = args;
    out.push_back(std::move(c));
  }
}

// Walk one file's registry calling each fn, while that file's scope is still
// open. Sequential; a failure doesn't stop the run unless `bail_after > 0`.
//
// A test body may itself call `test(...)` (the table-driven pattern), so the
// registry can grow while the loop runs; re-reading it after each test picks
// those up. Every case is a copy, so growth never invalidates what is in hand.
inline void run_cases(TestHost& host, std::vector<TestCase>& cases,
                      const std::string& filter, Reporter reporter,
                      int bail_after, const std::string& user_source,
                      TestRunSummary& summary) {
  for (size_t i = 0; i < cases.size(); i++) {
    TestCase c = cases[i];
    if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
    // RAII redirect so the rdbuf is restored on every exit path (normal
    // return, any catch, or unwind through an unexpected exception type).
    // Default mode passes false → no-op guard.
    StdoutCapture capture(reporter == Reporter::Json);
    // The try block fills these on failure; emitted AFTER the per-test values
    // are released, so a `drop()` fired by that release is captured into the
    // same `stdout` field.
    bool failed = false;
    std::string err_kind, err_what;
    int64_t err_line = 0, err_col = 0;
    size_t scratch = host.mark();
    // The registry is this file's and only grows, and every entry in it has
    // been copied into `cases` — so the length of one is the length of the
    // other, and what a test body appends starts here.
    size_t registry_before = cases.size();
    try {
      std::vector<ValueRef> args;
      if (c.args != kNoValue) {
        for (int64_t a = 0; a < host.array_size(c.args); a++)
          args.push_back(host.array_at(c.args, a));
      } else if (!c.params.empty()) {
        std::set<std::string> visited;
        std::map<std::string, ValueRef> fixtures;
        for (const auto& pname : c.params)
          args.push_back(resolve_fixture(host, pname, visited, fixtures));
      }
      host.call(c.fn, args);
    } catch (const CulebraError& e) {
      // Ctrl+C stops the run, it does not fail this one case.
      if (is_interrupt(e)) throw;
      failed = true;
      err_kind = e.kind;
      err_what = e.what();
      err_line = e.line;
      err_col = e.col;
    } catch (...) {
      failed = true;
      // A program's own `throw <value>`, in whichever shape this engine
      // raises it; anything else is ours.
      if (host.describe_current_throw(err_kind, err_what)) {
        if (err_kind.empty()) err_kind = "UserThrow";
      } else {
        try {
          throw;
        } catch (const std::exception& e) {
          err_kind = "InternalError";
          err_what = e.what();
        } catch (...) {
          err_kind = "InternalError";
          err_what = "unknown exception";
        }
      }
    }
    // Fixtures die here, before the capture is taken, so a class `drop()`
    // writes into this test's stdout rather than the next one's.
    host.release_to(scratch);
    collect_new_cases(host, registry_before, c.source_path, cases);

    auto captured = capture.take();

    if (!failed) {
      summary.passed++;
      if (reporter == Reporter::Json) {
        std::cout << R"({"event":"test_pass","name":)"
                  << json_escape(c.name)
                  << R"(,"source":)" << json_escape(c.source_path)
                  << R"(,"stdout":)" << json_escape(captured)
                  << "}\n";
      } else {
        std::cout << "  ok  " << c.name << "\n";
      }
    } else {
      summary.failed++;
      int user_line = to_user_line(err_line);
      if (reporter == Reporter::Json) {
        // A line number only maps to the file's own source; for a position
        // raised inside an imported module, leave the snippet empty rather
        // than render the wrong file's lines.
        std::string snippet;
        if (err_line > 0) snippet = source_snippet(user_source, user_line);
        std::cout << R"({"event":"test_fail","name":)"
                  << json_escape(c.name)
                  << R"(,"kind":)" << json_escape(err_kind)
                  << R"(,"message":)" << json_escape(err_what)
                  << R"(,"line":)" << user_line
                  << R"(,"col":)" << err_col
                  << R"(,"source":)" << json_escape(c.source_path)
                  << R"(,"snippet":)" << json_escape(snippet)
                  << R"(,"stdout":)" << json_escape(captured)
                  << "}\n";
      } else {
        std::string loc = user_line > 0
            ? " at " + std::to_string(user_line) +
              ":" + std::to_string(err_col)
            : "";
        std::string msg = std::string("  FAIL ") + c.name + " — "
                          + err_kind + ": " + err_what + loc
                          + " (in " + c.source_path + ")";
        summary.failure_messages.push_back(msg);
        std::cout << msg << "\n";
      }
    }
    if (bailed_out(summary, bail_after)) break;
  }
}

// `--list`: the discovered names, run none. Returns how many it printed.
inline size_t list_cases(const std::vector<TestCase>& cases,
                         const std::string& filter, Reporter reporter) {
  size_t listed = 0;
  for (const auto& e : cases) {
    if (!filter.empty() && e.name.find(filter) == std::string::npos) continue;
    listed++;
    if (reporter == Reporter::Json) {
      std::cout << R"({"event":"test_list","name":)" << json_escape(e.name)
                << R"(,"source":)" << json_escape(e.source_path) << "}\n";
    } else {
      std::cout << e.source_path << ": " << e.name << "\n";
    }
  }
  return listed;
}

// Run each test file: load it (so its `test(...)` calls register entries), run
// what it registered, then drop its scope and move on. Interleaved rather than
// load-everything-then-run because a file's scope is a file's — it is open
// exactly while that file's own tests need it. When `list_only` is true, emit a
// list of discovered tests without executing them.
inline TestRunSummary run_tests(
    TestHost& host,
    const std::vector<std::filesystem::path>& files,
    const std::string& filter,
    Reporter reporter = Reporter::Default,
    int bail_after = 0,
    bool list_only = false) {
  TestRunSummary summary;
  size_t listed = 0;

  auto emit_file_error = [&](const std::string& path_str,
                              const TestFileError& e) {
    const std::string& kind = e.kind;
    const std::string& message = e.message;
    int64_t line = to_user_line(e.line), col = e.col;
    if (reporter == Reporter::Json) {
      std::cout << R"({"event":"file_error","source":)"
                << json_escape(path_str)
                << R"(,"kind":)" << json_escape(kind)
                << R"(,"message":)" << json_escape(message)
                << R"(,"line":)" << line
                << R"(,"col":)" << col
                << "}\n";
    } else {
      std::cerr << "culebra test: " << kind << " for " << path_str
                << ": " << message << "\n";
    }
  };

  {
    // Turned on before any file is loaded: a bare `test` is resolved before
    // anything runs, so both the load-stage lint and the compiler need it.
    set_test_ambients(true);
    std::vector<std::string> msgs;
    if (!host.begin_run(msgs)) {
      emit_file_error("<test ambient>",
                      {"internal_error", join_messages(msgs), 0, 0});
      summary.errored_files++;
      return summary;
    }
  }

  for (const auto& path : files) {
    auto path_str = path.string();
    std::ifstream ifs(path_str, std::ios::binary);
    if (!ifs) {
      emit_file_error(path_str, {"open_failed", "can't open file", 0, 0});
      summary.errored_files++;
      continue;
    }
    std::string buff((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());

    // The file's scope closes on every way out of this iteration.
    struct FileScope {
      TestHost& host;
      ~FileScope() { host.end_file(); }
    } file_scope{host};

    TestFileError err;
    if (!host.run_file(path_str, buff, err)) {
      emit_file_error(path_str, err);
      summary.errored_files++;
      continue;
    }
    summary.files++;
    // The registry is the file's own, so every entry in it is new.
    std::vector<TestCase> cases;
    collect_new_cases(host, 0, path_str, cases);

    if (list_only) {
      listed += list_cases(cases, filter, reporter);
    } else {
      run_cases(host, cases, filter, reporter, bail_after, buff, summary);
      if (bailed_out(summary, bail_after)) break;
    }
  }

  if (reporter == Reporter::Json) {
    // list_end first, then run_end either way, so a consumer waiting on it as
    // the stream terminator doesn't hang on a --list run.
    if (list_only)
      std::cout << R"({"event":"list_end","count":)" << listed << "}\n";
    std::cout << R"({"event":"run_end","passed":)" << summary.passed
              << R"(,"failed":)" << summary.failed
              << R"(,"files":)" << summary.files
              << R"(,"errored_files":)" << summary.errored_files
              << R"(,"bailed":)"
              << (bailed_out(summary, bail_after) ? "true" : "false")
              << "}\n";
  }
  return summary;
}

}  // namespace culebra
