#pragma once

#include <filesystem>
#include <iostream>
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

// Invoke `receiver.<name>(rhs?)` if the method exists, mirroring how
// eval_condition dispatches `__eq__`/`__lt__`/`__le__`. Used by the
// six comparison matchers below so their semantics match `==`/`<` etc.
inline std::optional<Value> dispatch_special_method(
    const Value& receiver, std::string_view name, const Value* rhs) {
  if (receiver.type != Value::Object) return std::nullopt;
  const auto& obj = receiver.to_object();
  auto it = obj.properties->find(name);
  if (it == obj.properties->end()) return std::nullopt;
  const auto& m = it->second.val;
  if (m.type != Value::Function) return std::nullopt;
  const auto& mf = m.get<FunctionValue>();
  auto inner = std::make_shared<Environment>();
  inner->is_function_frame = true;
  inner->initialize("self", m, false);
  inner->initialize("this", receiver, false);
  inner->initialize("__LINE__", Value(0L), false);
  inner->initialize("__COLUMN__", Value(0L), false);
  if (rhs && !mf.params->empty()) {
    inner->initialize(mf.params->at(0).name, *rhs, false);
  }
  try {
    return mf.eval(inner);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

inline bool matcher_equal(const Value& a, const Value& b) {
  if (auto r = dispatch_special_method(a, "__eq__", &b)) return r->to_bool();
  if (auto r = dispatch_special_method(b, "__eq__", &a)) return r->to_bool();
  return a == b;
}

inline bool matcher_less(const Value& a, const Value& b) {
  if (auto r = dispatch_special_method(a, "__lt__", &b)) return r->to_bool();
  return a < b;
}

inline bool matcher_leq(const Value& a, const Value& b) {
  if (auto r = dispatch_special_method(a, "__le__", &b)) return r->to_bool();
  auto lt = dispatch_special_method(a, "__lt__", &b);
  auto eq = dispatch_special_method(a, "__eq__", &b);
  if (lt || eq) {
    return (lt && lt->to_bool()) || (eq && eq->to_bool());
  }
  return a <= b;
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

// Inject ambient bindings used only under `culebra test`: `test` /
// `parametrize` for registration, the matcher family for assertions.
// Fixtures are plain fns in env, resolved by param name at dispatch
// (per-test memoized). Cleanup uses class `drop`. See guide §16.
//
// `test` is polymorphic: `test("name", fn)` or `test(fn)` (decorator).
inline void install_test_ambient(Environment& env) {
  using namespace std::literals;

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

  // `assert_throws(kind, fn)` — invoke 0-arg `fn` and assert it throws
  // an error whose `kind` matches. `got_kind` distinguishes an empty
  // String kind from a fallthrough to v.str_display().
  env.initialize(
      "assert_throws",
      Value(FunctionValue(
          {{"kind", false, "String"sv}, {"fn", false, "Function"sv}},
          [](std::shared_ptr<Environment> callEnv) -> Value {
            auto expected = std::string(callEnv->get("kind").to_string());
            auto fn_val = callEnv->get("fn");
            const auto& f = fn_val.to_function();
            if (!f.params->empty()) {
              throw CulebraError("ArityError",
                  std::format("assert_throws: fn must take 0 parameters "
                              "(got {})", f.params->size()));
            }
            auto inner = std::make_shared<Environment>();
            inner->is_function_frame = true;
            inner->initialize("self", fn_val, false);
            inner->initialize("__LINE__", Value(0L), false);
            inner->initialize("__COLUMN__", Value(0L), false);
            bool threw = false;
            bool got_kind = false;
            std::string actual;
            try {
              f.eval(inner);
            } catch (const ReturnValue&) {
            } catch (const CulebraError& e) {
              threw = true;
              actual = e.kind;
              got_kind = true;
            } catch (const Value& v) {
              threw = true;
              if (v.type == Value::Object) {
                const auto& obj = v.to_object();
                if (obj.has("kind")) {
                  auto kv = obj.get("kind");
                  if (kv.type == Value::String) {
                    actual = std::string(kv.to_string());
                    got_kind = true;
                  }
                }
              }
              if (!got_kind) actual = v.str_display();
            }
            if (!threw) {
              throw CulebraError("AssertionError",
                  std::format("assert_throws('{}', fn): expected throw "
                              "but fn returned normally", expected));
            }
            if (actual != expected) {
              throw CulebraError("AssertionError",
                  std::format("assert_throws: expected kind '{}' "
                              "but got '{}'", expected, actual));
            }
            return Value();
          })),
      false);

  // `assert_close(a, b, tol)` — `|a - b| <= tol`, with NaN treated as
  // failure (a naive `diff > tol` check would silently pass NaN).
  env.initialize(
      "assert_close",
      Value(FunctionValue(
          {{"a", false}, {"b", false}, {"tol", false}},
          [](std::shared_ptr<Environment> callEnv) -> Value {
            auto a = callEnv->get("a").to_double_coerce();
            auto b = callEnv->get("b").to_double_coerce();
            auto tol = callEnv->get("tol").to_double_coerce();
            auto diff = std::abs(a - b);
            if (std::isnan(diff) || diff > tol) {
              throw CulebraError("AssertionError",
                  std::format("assert_close failed:\n"
                              "  a:    {}\n"
                              "  b:    {}\n"
                              "  diff: {} (> tol {})",
                              a, b, diff, tol));
            }
            return Value();
          })),
      false);

  // Comparison matchers — `assert_eq(a, b)` etc. Each dispatches via
  // the same `__eq__`/`__lt__`/`__le__` rules as the `==`/`<`/`<=`
  // operators so `assert_eq(p1, p2)` matches `assert(p1 == p2)` for
  // class instances. Failure messages name both operands.
  auto make_cmp_matcher = [](std::string_view name,
                              std::string_view label,
                              auto pred) {
    return Value(FunctionValue(
        {{"a", false}, {"b", false}},
        [n = std::string(name), lbl = std::string(label), pred](
            std::shared_ptr<Environment> callEnv) -> Value {
          auto a = callEnv->get("a");
          auto b = callEnv->get("b");
          if (pred(a, b)) return Value();
          throw CulebraError("AssertionError",
              std::format("{} failed:\n  left:  {}\n  right: {}",
                          n,
                          str_quoted_with_special(a),
                          str_quoted_with_special(b)));
        }));
  };

  env.initialize("assert_eq",
      make_cmp_matcher("assert_eq", "==",
          [](const Value& a, const Value& b) { return matcher_equal(a, b); }),
      false);
  env.initialize("assert_ne",
      make_cmp_matcher("assert_ne", "!=",
          [](const Value& a, const Value& b) { return !matcher_equal(a, b); }),
      false);
  env.initialize("assert_lt",
      make_cmp_matcher("assert_lt", "<",
          [](const Value& a, const Value& b) { return matcher_less(a, b); }),
      false);
  env.initialize("assert_le",
      make_cmp_matcher("assert_le", "<=",
          [](const Value& a, const Value& b) { return matcher_leq(a, b); }),
      false);
  env.initialize("assert_gt",
      make_cmp_matcher("assert_gt", ">",
          [](const Value& a, const Value& b) { return matcher_less(b, a); }),
      false);
  env.initialize("assert_ge",
      make_cmp_matcher("assert_ge", ">=",
          [](const Value& a, const Value& b) { return matcher_leq(b, a); }),
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
// from this struct; main.cc returns non-zero if any test failed or
// any file failed to load/parse/interpret. `errored_files` is kept
// separate from `failed` so the per-test failure count is faithful
// (a syntax error in one file shouldn't inflate `failed` by 1 while
// hiding however many tests that file would have registered).
struct TestRunSummary {
  int passed = 0;
  int failed = 0;
  int errored_files = 0;
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

  for (const auto& path : files) {
    auto path_str = path.string();
    std::ifstream ifs(path_str, std::ios::binary);
    if (!ifs) {
      std::cerr << "culebra test: can't open '" << path_str << "'\n";
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
    auto entry_src = prepend_stdlib_preamble(buff);
    try {
      modules = loader.load_program(path_str, entry_src, msgs);
    } catch (const CulebraError& e) {
      std::cerr << "culebra test: load failed for " << path_str << ": "
                << e.kind << ": " << e.what() << "\n";
      summary.errored_files++;
      continue;
    }

    Value val;
    Debugger dbg;
    if (!interpret_modules(modules, env, val, msgs, dbg)) {
      for (auto& m : msgs) std::cerr << m << "\n";
      summary.errored_files++;
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
    // Per-test fixture cache; cleared on scope exit so class `drop` fires.
    std::unordered_map<std::string, Value> fixture_cache;
    try {
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
