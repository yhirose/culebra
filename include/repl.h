#pragma once

#include <linenoise.hpp>
#include "interpreter.h"

#ifdef CULEBRA_JIT_ENABLED
#include "jit.h"
#endif

namespace culebra {

inline int repl(std::shared_ptr<Environment> env, bool print_ast,
                bool jit_mode = false) {
  using namespace std;

  if (jit_mode) {
    cout << "JIT REPL (state not preserved between inputs; use puts() for "
            "output)"
         << endl;
  }

  for (;;) {
    auto prompt = jit_mode ? "jit> " : "cul> ";
    auto line = linenoise::Readline(prompt);

    if (line == "exit" || line == "quit") {
      break;
    }

    if (!line.empty()) {
      vector<string> msgs;
      auto ast = parse("(repl)", line.data(), line.size(), msgs);
      if (ast) {
        if (print_ast) {
          cout << peg::ast_to_s(ast);
        }

#ifdef CULEBRA_JIT_ENABLED
        if (jit_mode) {
          try {
            JIT::run(ast, false, false);
            linenoise::AddHistory(line.c_str());
            continue;
          } catch (exception& e) {
            cout << e.what() << endl;
            continue;
          }
        }
#endif

        Value val;
        if (interpret(ast, env, val, msgs)) {
          cout << val << endl;
          linenoise::AddHistory(line.c_str());
          continue;
        }
      }

      for (const auto& msg : msgs) {
        cout << msg << endl;
      }
    }
  }

  return 0;
}

}  // namespace culebra
