#pragma once

#include <linenoise.hpp>
#include "interpreter.h"

namespace culebra {

inline int repl(std::shared_ptr<Environment> env, bool print_ast) {
  using namespace std;

  for (;;) {
    auto line = linenoise::Readline("cul> ");

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
