#pragma once

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <linenoise.hpp>
#include "interpreter.h"
#include "repl_core.h"  // the engine-neutral loop (B7-b)
#include "stdlib_interp.h"  // culebra::environment()

namespace culebra {


// The interpreter engine: each input is parsed on its own and evaluated
// against the one `Environment` that persists for the session, which is where
// a line's declarations live on for the next line to see.
inline int repl(std::shared_ptr<Environment> env, bool print_ast) {
  // The interp REPL relies on the env's lazy Time / Args / Regex bindings
  // registered by `environment()`; no explicit preamble load is needed.
  return repl_loop(print_ast, [&](const std::shared_ptr<peg::Ast>& ast,
                                  std::vector<std::string>& msgs) {
    Value val;
    if (!interpret(ast, env, val, msgs)) return false;
    // A nil result isn't echoed: `println(...)`, a `fn` declaration and an
    // `if` without else all evaluate to nil, so echoing would double every
    // line of output (matches the python REPL convention).
    if (val.type != Value::Nil) std::cout << val << std::endl;
    return true;
  });
}

}  // namespace culebra
