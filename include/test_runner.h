#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "interpreter.h"
#include "module_loader.h"
#include "shared.h"

namespace culebra {

// A registered test. `param_names` drives DI; `param_cases` (from
// `@parametrize`) takes precedence and skips DI.
struct TestEntry {
  std::string name;
  Value fn;
  std::vector<std::string> param_names;
  std::vector<Value> param_cases;
  std::string source_path;
};

struct TestRegistry {
  std::vector<TestEntry> entries;
};

inline TestRegistry& test_registry() {
  return runtime_substate<TestRegistry>(kSlotTestRegistry);
}

// RAII redirect of `std::cout` into an internal stringstream. The dtor
// restores the original rdbuf even if the consumer never calls `take()`
// (e.g. an unexpected exception type escaped the catch). `take()` is
// idempotent — repeated calls return "" after the first.
class StdoutCapture {
  std::stringstream buf_;
  std::streambuf* old_ = nullptr;

 public:
  explicit StdoutCapture(bool active) {
    if (active) old_ = std::cout.rdbuf(buf_.rdbuf());
  }
  ~StdoutCapture() {
    if (old_) std::cout.rdbuf(old_);
  }
  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  std::string take() {
    if (!old_) return {};
    std::cout.rdbuf(old_);
    old_ = nullptr;
    return buf_.str();
  }
};

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

// Regular positional param names of `fn` (kw-only / kwargs-rest skipped).
inline std::vector<std::string> extract_param_names(const Value& fn) {
  std::vector<std::string> out;
  if (fn.type != Value::Function) return out;
  const auto& fv = fn.get<FunctionValue>();
  const auto& source = fv.introspection_target ? *fv.introspection_target : fv;
  for (const auto& p : *source.params) {
    if (p.kw_only || p.kwargs_rest) continue;
    out.emplace_back(p.name);
  }
  return out;
}

// Per-test memoized fixture resolution. `cache` keeps the same
// instance across multiple mentions; `visited` catches cycles.
inline Value resolve_fixture_impl(
    std::shared_ptr<Environment> env,
    const std::string& name,
    std::set<std::string>& visited,
    std::unordered_map<std::string, Value>& cache) {
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  if (!env->has(name)) {
    throw CulebraError(
        "NameError",
        "fixture '" + name + "' not found in env");
  }
  if (visited.count(name)) {
    std::string chain;
    for (auto& v : visited) {
      if (!chain.empty()) chain += " → ";
      chain += v;
    }
    chain += " → " + name;
    throw CulebraError("CycleError", "fixture cycle detected: " + chain);
  }
  auto val = env->get(name);
  if (val.type != Value::Function) {
    cache.emplace(name, val);
    return val;
  }
  visited.insert(name);
  auto params = extract_param_names(val);
  std::vector<Value> args;
  for (const auto& p : params) {
    args.push_back(resolve_fixture_impl(env, p, visited, cache));
  }
  visited.erase(name);
  auto result = call(env, name, std::move(args));
  cache.emplace(name, result);
  return result;
}

// `test(name, fn)` runtime helper used by the built-in `test` global
// (ambient-injected under `culebra test` mode).
inline Value register_test(std::string name, Value fn,
                            std::vector<Value> param_cases = {},
                            std::string source_path = {}) {
  TestEntry entry;
  entry.name = std::move(name);
  entry.param_names = extract_param_names(fn);
  entry.fn = std::move(fn);
  entry.param_cases = std::move(param_cases);
  entry.source_path = std::move(source_path);
  test_registry().entries.push_back(std::move(entry));
  return Value();
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

// Inject ambient bindings used only under `culebra test`: `test` /
// `parametrize` for registration. The matcher family (assert_true /
// assert_eq / etc.) is a 3-backend global via stdlib MATCHERS_MODULE_SOURCE
// and isn't installed here.
// Fixtures are plain fns in env, resolved by param name at dispatch
// (per-test memoized). Cleanup uses class `drop`. See guide §16.
//
// `test` is polymorphic: `test("name", fn)` or `test(fn)` (decorator).
inline void install_test_ambient(Environment& env) {
  using namespace std::literals;

  // Before any module is loaded: without it `test("name", fn)` is rejected by
  // the load-stage lint. (`@test` was never rejected — the lint skips
  // decorator subtrees outright, deliberately, for soundness.)
  set_test_ambients(true);

  // `parametrize(cases)` returns a decorator that registers one
  // entry per case under `<fn.name>[i]`. Single-arg fns can pass
  // scalars directly; multi-arg fns should pass Tuples/Arrays whose
  // length matches the param count.
  env.initialize(
      "parametrize",
      Value(FunctionValue(
          {{"cases", false, "Array"sv}},
          [](std::shared_ptr<Environment> callEnv) -> Value {
            auto cases_val = callEnv->get("cases");
            return Value(FunctionValue(
                {{"f", false, "Function"sv}},
                [cases_val](std::shared_ptr<Environment> deco_env) -> Value {
                  auto f = deco_env->get("f");
                  auto fname = std::string(f.get<FunctionValue>().name);
                  if (fname.empty()) {
                    throw CulebraError(
                        "ValueError",
                        "@parametrize requires a named function");
                  }
                  const auto& cases = *cases_val.to_array().values;
                  for (size_t i = 0; i < cases.size(); i++) {
                    std::string case_name =
                        fname + "[" + std::to_string(i) + "]";
                    register_test(case_name, f, {cases[i]});
                  }
                  return f;
                }));
          })),
      false);

  env.initialize(
      "test",
      Value(FunctionValue(
          {},  // no formals — everything lands in __ARGS__
          [](std::shared_ptr<Environment> callEnv) -> Value {
            if (!callEnv->has("__ARGS__")) {
              throw CulebraError(
                  "ArityError",
                  "test() expects 1 or 2 arguments (got 0)");
            }
            const auto& args = *callEnv->get("__ARGS__").to_array().values;
            if (args.size() == 2) {
              if (args[0].type != Value::String) {
                throw CulebraError(
                    "TypeError",
                    "test(name, fn): name must be a String");
              }
              if (args[1].type != Value::Function) {
                throw CulebraError(
                    "TypeError",
                    "test(name, fn): fn must be a Function");
              }
              return register_test(std::string(args[0].to_string()),
                                    args[1]);
            }
            if (args.size() == 1) {
              if (args[0].type != Value::Function) {
                throw CulebraError(
                    "TypeError",
                    "test(fn): argument must be a Function");
              }
              const auto& fn_val = args[0];
              auto fname = std::string(fn_val.get<FunctionValue>().name);
              if (fname.empty()) {
                throw CulebraError(
                    "ValueError",
                    "@test requires a named function (got anonymous); "
                    "use test(\"name\", fn) for anonymous bodies");
              }
              // Skip when `@parametrize` already registered `<fname>[i]`
              // entries for this fn; stacking `@test` over `@parametrize`
              // would otherwise add a spurious bare entry that fails
              // fixture DI at run time.
              const auto& reg = test_registry().entries;
              std::string prefix = fname + "[";
              for (const auto& e : reg) {
                if (e.name.starts_with(prefix)) {
                  return fn_val;
                }
              }
              register_test(fname, fn_val);
              return fn_val;  // decorator: return the original fn
            }
            throw CulebraError(
                "ArityError",
                "test() expects 1 or 2 arguments");
          })),
      false);

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
  std::vector<std::string> failure_messages;
};

// Load each test file (so its `test(...)` calls register entries),
// then walk the registry calling each fn. Sequential execution;
// failures don't stop the run unless `bail_after > 0`. When
// `list_only` is true, emit a list of discovered tests without
// executing them.
inline TestRunSummary run_tests(
    const std::vector<std::filesystem::path>& files,
    const std::string& filter,
    std::shared_ptr<Environment> env,
    Reporter reporter = Reporter::Default,
    int bail_after = 0,
    bool list_only = false) {
  TestRunSummary summary;

  // Embedders may call run_tests multiple times in the same process
  // (watch mode, host applications). Clear the registry on entry so
  // stale entries from a previous run don't re-execute against the
  // new env. (The CLI's one-shot path is unaffected — the registry
  // starts empty either way.)
  test_registry().entries.clear();

  // Keep every loaded module alive for the full duration of the run.
  // LoadedModule owns a shared_ptr<std::string> backing every AST
  // token's std::string_view; registered FunctionValues reference
  // those views after the per-file loop ends, so dropping the
  // modules vector before the execution loop would leave the
  // registry pointing into freed memory.
  std::vector<LoadedModule> all_modules;
  // Original (unprepended) user source per file path — used to render
  // a failure-site snippet in JSON events.
  std::unordered_map<std::string, std::string> user_sources;

  auto emit_file_error = [&](const std::string& path_str,
                              const std::string& kind,
                              const std::string& message,
                              int64_t raw_line = 0, int64_t col = 0) {
    // raw_line is in entry-buffer coordinates (preamble-included).
    // Reduce to user-file coordinates so the JSON line value points
    // into the actual file the user wrote.
    int64_t line = to_user_line(raw_line);
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

  for (const auto& path : files) {
    auto path_str = path.string();
    std::ifstream ifs(path_str, std::ios::binary);
    if (!ifs) {
      emit_file_error(path_str, "open_failed", "can't open file");
      summary.errored_files++;
      continue;
    }
    std::string buff((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());

    // Remember how many tests existed before this file so we can tag
    // newly added ones with the source path.
    auto& reg = test_registry();
    size_t pre = reg.entries.size();

    std::vector<std::string> msgs;
    ModuleLoader loader;
    std::vector<LoadedModule> modules;
    // Time / Args come from the env's lazy bindings — no preamble
    // prepend needed (Phase 2).
    try {
      modules = loader.load_program(path_str, buff, msgs);
    } catch (const CulebraError& e) {
      emit_file_error(path_str, e.kind, e.what(), e.line, e.col);
      summary.errored_files++;
      continue;
    }

    Value val;
    Debugger dbg;
    if (!interpret_modules(modules, env, val, msgs, dbg)) {
      emit_file_error(path_str, "interpret_failed", join_messages(msgs));
      summary.errored_files++;
      continue;
    }

    // Pin the file's modules (and thus their source buffers and AST
    // tokens) for the rest of run_tests' lifetime.
    for (auto& m : modules) all_modules.push_back(std::move(m));
    user_sources.emplace(path_str, std::move(buff));

    // Tag new entries with source path for nicer failure reporting.
    for (size_t i = pre; i < reg.entries.size(); i++) {
      if (reg.entries[i].source_path.empty()) {
        reg.entries[i].source_path = path_str;
      }
    }
  }

  auto& reg = test_registry();

  if (list_only) {
    size_t listed = 0;
    for (const auto& e : reg.entries) {
      if (!filter.empty() && e.name.find(filter) == std::string::npos) continue;
      listed++;
      if (reporter == Reporter::Json) {
        std::cout << R"({"event":"test_list","name":)"
                  << json_escape(e.name)
                  << R"(,"source":)" << json_escape(e.source_path)
                  << "}\n";
      } else {
        std::cout << e.source_path << ": " << e.name << "\n";
      }
    }
    if (reporter == Reporter::Json) {
      std::cout << R"({"event":"list_end","count":)" << listed << "}\n";
      // Emit run_end too so consumers waiting on it as the stream
      // terminator don't hang on --list runs.
      std::cout << R"({"event":"run_end","passed":0,"failed":0,)"
                << R"("errored_files":)" << summary.errored_files
                << R"(,"bailed":false})" << "\n";
    }
    return summary;
  }

  // Now execute every registered test. We bind each entry under a
  // private slot name (`__test_<i>`) so the public `culebra::call`
  // helper can invoke it without exposing the Interpreter's private
  // call internals. The slot is overwritten on each pass — minimal
  // env pollution given the runner exits immediately after.
  // Snapshot each entry's fields by value before running. A test
  // body may dynamically call `test(...)` (legitimate table-driven
  // pattern) which push_back's onto reg.entries — capacity growth
  // would invalidate any reference into the vector mid-loop. Loop
  // bound uses .size() at each iteration so dynamic additions still
  // run, but the per-test fields are stable.
  for (size_t i = 0; i < reg.entries.size(); i++) {
    std::string entry_name;
    std::string entry_source;
    Value entry_fn;
    std::vector<std::string> entry_param_names;
    std::vector<Value> entry_param_cases;
    {
      const auto& e = reg.entries[i];
      entry_name = e.name;
      entry_source = e.source_path;
      entry_fn = e.fn;
      entry_param_names = e.param_names;
      entry_param_cases = e.param_cases;
    }
    if (!filter.empty() && entry_name.find(filter) == std::string::npos) {
      continue;
    }
    std::string slot = "__test_" + std::to_string(i);
    // RAII capture so the rdbuf is restored on every exit path (normal
    // return, any catch, or unwind through an uncaught exception type).
    // Default mode passes false → no-op guard.
    StdoutCapture capture(reporter == Reporter::Json);
    // The try block fills these on failure; emitted AFTER the inner
    // scope's fixture_cache destructs so `drop()`-time puts get
    // captured into the same `stdout` field.
    bool failed = false;
    std::string err_kind, err_what;
    int64_t err_line = 0, err_col = 0;
    try {
      // Inner scope so the per-test fixture_cache destructs before
      // we take() captured stdout — class `drop()` then writes into
      // the capture rather than into the restored stream.
      std::unordered_map<std::string, Value> fixture_cache;
      env->initialize(slot, entry_fn, false);
      std::vector<Value> args;
      if (!entry_param_cases.empty()) {
        const auto& c = entry_param_cases[0];
        if (c.type == Value::Tuple) {
          for (const auto& v : *c.get<TupleValue>().elements)
            args.push_back(v);
        } else if (c.type == Value::Array) {
          for (const auto& v : *c.to_array().values) args.push_back(v);
        } else {
          args.push_back(c);
        }
      } else if (!entry_param_names.empty()) {
        std::set<std::string> visited;
        for (const auto& pname : entry_param_names) {
          args.push_back(
              resolve_fixture_impl(env, pname, visited, fixture_cache));
        }
      }
      call(env, slot, std::move(args));
    } catch (const CulebraError& e) {
      failed = true;
      err_kind = e.kind;
      err_what = e.what();
      err_line = e.line;
      err_col = e.col;
    } catch (const Value& v) {
      // User `throw <value>` lands here. Preserve kind/message when the
      // payload is an Object with the conventional shape.
      failed = true;
      if (v.type == Value::Object) {
        const auto& obj = v.to_object();
        if (obj.has("kind")) {
          auto kv = obj.get("kind");
          if (kv.type == Value::String) err_kind = std::string(kv.to_string());
        }
        if (obj.has("message")) {
          auto mv = obj.get("message");
          if (mv.type == Value::String) err_what = std::string(mv.to_string());
        }
      }
      if (err_kind.empty()) err_kind = "UserThrow";
      if (err_what.empty()) err_what = v.str_display();
    } catch (const std::exception& e) {
      failed = true;
      err_kind = "InternalError";
      err_what = e.what();
    }

    auto captured = capture.take();

    if (!failed) {
      summary.passed++;
      if (reporter == Reporter::Json) {
        std::cout << R"({"event":"test_pass","name":)"
                  << json_escape(entry_name)
                  << R"(,"source":)" << json_escape(entry_source)
                  << R"(,"stdout":)" << json_escape(captured)
                  << "}\n";
      } else {
        std::cout << "  ok  " << entry_name << "\n";
      }
    } else {
      summary.failed++;
      int user_line = to_user_line(err_line);
      if (reporter == Reporter::Json) {
        // Snippet only when the line plausibly maps to the entry
        // file. For imported modules we don't have the source mapped
        // here, so leave snippet empty rather than render the wrong
        // file's lines.
        std::string snippet;
        if (err_line > 0) {
          if (auto it = user_sources.find(entry_source);
              it != user_sources.end()) {
            snippet = source_snippet(it->second, user_line);
          }
        }
        std::cout << R"({"event":"test_fail","name":)"
                  << json_escape(entry_name)
                  << R"(,"kind":)" << json_escape(err_kind)
                  << R"(,"message":)" << json_escape(err_what)
                  << R"(,"line":)" << user_line
                  << R"(,"col":)" << err_col
                  << R"(,"source":)" << json_escape(entry_source)
                  << R"(,"snippet":)" << json_escape(snippet)
                  << R"(,"stdout":)" << json_escape(captured)
                  << "}\n";
      } else {
        std::string loc = user_line > 0
            ? " at " + std::to_string(user_line) +
              ":" + std::to_string(err_col)
            : "";
        std::string msg = std::string("  FAIL ") + entry_name + " — "
                          + err_kind + ": " + err_what + loc
                          + " (in " + entry_source + ")";
        summary.failure_messages.push_back(msg);
        std::cout << msg << "\n";
      }
    }
    if (bail_after > 0 && summary.failed >= bail_after) break;
  }

  bool bailed = bail_after > 0 && summary.failed >= bail_after;
  if (reporter == Reporter::Json) {
    std::cout << R"({"event":"run_end","passed":)" << summary.passed
              << R"(,"failed":)" << summary.failed
              << R"(,"errored_files":)" << summary.errored_files
              << R"(,"bailed":)" << (bailed ? "true" : "false")
              << "}\n";
  }
  return summary;
}

}  // namespace culebra
