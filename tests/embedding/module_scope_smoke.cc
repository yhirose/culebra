// interpret_modules must still expose the entry module's top-level
// bindings through the very `env` shared_ptr the caller passed in — the
// entry now runs in its own child scope (so a dependency module's
// functions can't see it, see interpreter.h's interpret_modules), and
// its bindings are moved into `env` only after it finishes. REPL /
// embedding callers hold their own reference to `env`, not to the
// internal child scope, so this contract has no .cul-level test.

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <culebra.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace module_scope_smoke_ns {

namespace {

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

}  // namespace

int run() {
  bool ok = true;

  {
    auto env = culebra::environment();
    culebra::ModuleLoader loader;
    std::vector<std::string> msgs;
    // A single-element module list (no imports): modules.back() is the
    // only element, so this exercises exactly the entry-flatten path with
    // no dependency loop involved.
    auto modules = loader.load_program(
        "tests/embedding/<module-scope-smoke>", "x = 42\n", msgs);
    ok &= check(!modules.empty(), "entry module loaded");

    culebra::Value val;
    bool ran = culebra::interpret_modules(modules, env, val, msgs);
    if (!ran) {
      for (auto& m : msgs) std::cerr << m << "\n";
    }
    ok &= check(ran, "run succeeded");
    ok &= check(env->has("x"), "entry's top-level binding visible via the "
                               "original env pointer");
    ok &= check(ran && env->has("x") && env->get("x").to_long() == 42,
               "entry's top-level binding has the right value");
  }

  {
    // Read-side variant of the module-scope leak: a dependency reads a
    // name the entry only defines AFTER the import. `entry.ast` resolves
    // its own `./scope_dep_reader.cul` relative to the entry path's
    // directory, so the entry path must actually sit next to it on disk
    // even though the entry source itself is supplied inline.
    auto env = culebra::environment();
    culebra::ModuleLoader loader;
    std::vector<std::string> msgs;
    auto modules = loader.load_program(
        std::filesystem::path(__FILE__).parent_path() /
            "../test_import_helpers/<module-scope-smoke-entry>.cul",
        "import dep from './scope_dep_reader.cul'\n"
        "shared = \"entry-value\"\n"
        "dep.read_shared()\n",
        msgs);
    ok &= check(modules.size() == 2, "dependency + entry both loaded");

    culebra::Value val;
    bool ran = culebra::interpret_modules(modules, env, val, msgs);
    ok &= check(!ran, "dependency must NOT resolve the entry's later "
                      "top-level binding");
    bool saw_name_error =
        !msgs.empty() && msgs.back().find("undefined variable 'shared'") !=
                             std::string::npos;
    ok &= check(saw_name_error,
               "failure is the dependency's own NameError, not a leaked "
               "read of \"entry-value\"");
  }

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace module_scope_smoke_ns
