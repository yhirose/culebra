#pragma once

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "interpreter.h"
#include "module_loader.h"
#include "shared.h"

namespace culebra {

// Single test case as registered by `test("name", fn() { ... })`.
// `fn` keeps the closure +1 alive in the registry until the runner
// invokes it and pops the entry.
//
// `param_names` is the runtime-resolvable parameter list — at run time
// the runner looks each name up in `env` and invokes it as a fixture,
// passing the result as the corresponding positional argument. Empty
// for tests with no params (the common case).
//
// `param_cases` is populated by `@parametrize(cases)`: each entry is
// a Tuple/Array of positional args to invoke `fn` with as a separate
// reported test. When non-empty, fixture DI on `param_names` is
// skipped (parametrize values fill those slots).
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

// Extract the user-facing parameter names from a Function value. The
// dispatcher's synthetic `__KWARGS__` and kw-only params are skipped
// — only regular positional params participate in fixture DI.
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

// Recursively resolve a fixture by name. The fixture's own
// parameters are looked up in the same env and resolved before
// invoking. Plain (non-Function) bindings are returned as-is —
// they represent already-materialized values. Each call creates a
// fresh instance; per-test caching is future work.
//
// `visited` tracks the in-progress resolution chain; a name already
// on the chain indicates a fixture cycle (e.g. `db` depends on
// `user` and `user` depends on `db`) and is reported as a
// CycleError instead of recursing to stack overflow.
inline Value resolve_fixture_impl(std::shared_ptr<Environment> env,
                                   const std::string& name,
                                   std::set<std::string>& visited) {
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
    throw CulebraError(
        "CycleError",
        "fixture cycle detected: " + chain);
  }
  auto val = env->get(name);
  if (val.type != Value::Function) return val;
  visited.insert(name);
  auto params = extract_param_names(val);
  std::vector<Value> args;
  for (const auto& p : params) {
    args.push_back(resolve_fixture_impl(env, p, visited));
  }
  visited.erase(name);
  return call(env, name, std::move(args));
}

inline Value resolve_fixture(std::shared_ptr<Environment> env,
                              const std::string& name) {
  std::set<std::string> visited;
  return resolve_fixture_impl(env, name, visited);
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

// Inject the test runtime into `env` so user scripts can call
// `test("...", fn)` (call form) or `@test fn name() {}` (decorator
// form) without importing anything. Called only when the CLI was
// invoked as `culebra test ...`. Normal `culebra script.cul` runs
// do NOT see this binding (it would pollute production code).
//
// Three names get bound in addition to `test`:
//   - `fixture` — no-op decorator (marker); leaves fn under its own
//     name in env, the runner looks the param name up there directly
//   - `parametrize(cases)` — decorator factory; each entry of `cases`
//     becomes a separate registered test with values unpacked into
//     the fn's positional params (cases entry can be a Tuple/Array
//     or a scalar for single-arg tests)
//
// `test` is polymorphic over arity to make both forms ergonomic:
//   - `test("name", fn)` — 2 args, explicit name (call form)
//   - `test(fn)`         — 1 arg, name from fn.name (decorator form)
inline void install_test_ambient(Environment& env) {
  using namespace std::literals;
  // `fixture` is a marker decorator. The fn keeps its source-level
  // env binding (e.g. `@fixture fn db() { ... }` puts `db` in env);
  // the runner resolves a test's param by name against env at
  // dispatch time. No separate fixture table required.
  env.initialize(
      "fixture",
      Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> e) {
                             return e->get("f");
                           })),
      false);

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

// Discover test files under `roots`. A path that is a directory is
// walked recursively for files matching `test_*.cul`. A path that is
// a file is included as-is. Returns absolute paths, deduplicated.
inline std::vector<std::filesystem::path> discover_test_files(
    const std::vector<std::string>& roots) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  auto add = [&](const fs::path& p) {
    auto canon = fs::weakly_canonical(p);
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
      auto name = it->path().filename().string();
      if (name.starts_with("test_") && it->path().extension() == ".cul") {
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

// Result of running all collected tests. The driver prints a summary
// from this struct; main.cc returns 1 if `failed > 0`.
struct TestRunSummary {
  int passed = 0;
  int failed = 0;
  std::vector<std::string> failure_messages;
};

// Load each test file (so its `test(...)` calls register entries),
// then walk the registry calling each fn. Filtering happens on
// substring of the test name. Sequential execution; failures don't
// stop the run.
inline TestRunSummary run_tests(
    const std::vector<std::filesystem::path>& files,
    const std::string& filter,
    std::shared_ptr<Environment> env) {
  TestRunSummary summary;

  // Keep every loaded module alive for the full duration of the run.
  // LoadedModule owns a shared_ptr<std::string> backing every AST
  // token's std::string_view; registered FunctionValues reference
  // those views after the per-file loop ends, so dropping the
  // modules vector before the execution loop would leave the
  // registry pointing into freed memory.
  std::vector<LoadedModule> all_modules;

  for (const auto& path : files) {
    auto path_str = path.string();
    std::ifstream ifs(path_str, std::ios::binary);
    if (!ifs) {
      std::cerr << "culebra test: can't open '" << path_str << "'\n";
      summary.failed++;
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
    auto entry_src = prepend_stdlib_preamble(buff);
    try {
      modules = loader.load_program(path_str, entry_src, msgs);
    } catch (const CulebraError& e) {
      std::cerr << "culebra test: load failed for " << path_str << ": "
                << e.kind << ": " << e.what() << "\n";
      summary.failed++;
      continue;
    }

    Value val;
    Debugger dbg;
    if (!interpret_modules(modules, env, val, msgs, dbg)) {
      for (auto& m : msgs) std::cerr << m << "\n";
      summary.failed++;
      continue;
    }

    // Pin the file's modules (and thus their source buffers and AST
    // tokens) for the rest of run_tests' lifetime.
    for (auto& m : modules) all_modules.push_back(std::move(m));

    // Tag new entries with source path for nicer failure reporting.
    for (size_t i = pre; i < reg.entries.size(); i++) {
      if (reg.entries[i].source_path.empty()) {
        reg.entries[i].source_path = path_str;
      }
    }
  }

  // Now execute every registered test. We bind each entry under a
  // private slot name (`__test_<i>`) so the public `culebra::call`
  // helper can invoke it without exposing the Interpreter's private
  // call internals. The slot is overwritten on each pass — minimal
  // env pollution given the runner exits immediately after.
  auto& reg = test_registry();
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
    try {
      env->initialize(slot, entry_fn, false);
      // Args resolution priority:
      //   1. @parametrize-supplied case wins (skip fixture DI)
      //   2. fixture DI: each param name resolved as a 0-arg fn in env
      //   3. neither: invoke with no args
      std::vector<Value> args;
      if (!entry_param_cases.empty()) {
        const auto& c = entry_param_cases[0];
        if (c.type == Value::Tuple) {
          for (const auto& v : *c.get<TupleValue>().elements)
            args.push_back(v);
        } else if (c.type == Value::Array) {
          for (const auto& v : *c.to_array().values) args.push_back(v);
        } else {
          args.push_back(c);  // scalar single-arg
        }
      } else if (!entry_param_names.empty()) {
        for (const auto& pname : entry_param_names) {
          args.push_back(resolve_fixture(env, pname));
        }
      }
      call(env, slot, std::move(args));
      summary.passed++;
      std::cout << "  ok  " << entry_name << "\n";
    } catch (const CulebraError& e) {
      summary.failed++;
      std::string loc = e.line > 0
          ? " at " + std::to_string(e.line) + ":" + std::to_string(e.col)
          : "";
      std::string msg = std::string("  FAIL ") + entry_name + " — "
                        + e.kind + ": " + e.what() + loc
                        + " (in " + entry_source + ")";
      summary.failure_messages.push_back(msg);
      std::cout << msg << "\n";
    } catch (const std::exception& e) {
      summary.failed++;
      summary.failure_messages.push_back(
          std::string("  FAIL ") + entry_name + " — " + e.what());
      std::cout << "  FAIL " << entry_name << " — " << e.what() << "\n";
    }
  }

  return summary;
}

}  // namespace culebra
