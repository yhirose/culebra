// Embed::run(modules) must expose the entry module's top-level bindings
// through the session (the write side: a host reads a global or calls a
// function after the run returns), while a dependency module still cannot
// see names the entry declares after the import (the read side — the
// session's cells are the entry's alone; deps compile module-scoped, see
// vm::Compiler::compile_session_modules). Embedding callers hold the
// session, not any internal scope, so this contract has no .cul-level
// test. (The interp-era interpret_modules/env contract this exercised
// retired with the tree-walker — Phase 4 B7-d.)

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <culebra.h>
#include <vm/embed.h>

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
    culebra::Runtime rt;
    culebra::RuntimeScope scope(rt);
    culebra::vm::Embed embed;
    culebra::ModuleLoader loader;
    std::vector<std::string> msgs;
    // A single-element module list (no imports): modules.back() is the
    // only element, so this exercises exactly the entry path with no
    // dependency loop involved.
    auto modules = loader.load_program(
        "tests/embedding/<module-scope-smoke>", "x = 42\n", msgs);
    ok &= check(!modules.empty(), "entry module loaded");

    culebra::vm::Value val;
    bool ran = embed.run(modules, val, msgs);
    if (!ran) {
      for (auto& m : msgs) std::cerr << m << "\n";
    }
    ok &= check(ran, "run succeeded");
    ok &= check(!embed.global("x").is_nil(),
                "entry's top-level binding visible through the session");
    ok &= check(embed.global("x").to_long() == 42,
                "entry's top-level binding has the right value");
  }

  {
    // Read-side variant of the module-scope leak: a dependency reads a
    // name the entry only defines AFTER the import. `entry.ast` resolves
    // its own `./scope_dep_reader.cul` relative to the entry path's
    // directory, so the entry path must actually sit next to it on disk
    // even though the entry source itself is supplied inline.
    culebra::Runtime rt;
    culebra::RuntimeScope scope(rt);
    culebra::vm::Embed embed;
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

    culebra::vm::Value val;
    bool ran = embed.run(modules, val, msgs);
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
