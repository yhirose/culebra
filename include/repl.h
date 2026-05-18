#pragma once

#include <cstdlib>
#include <filesystem>
#include <linenoise.hpp>
#include "interpreter.h"

#ifdef CULEBRA_JIT_ENABLED
#include "jit.h"
#endif

namespace culebra {

// Persistent REPL history path. Honors `CULEBRA_HISTFILE` if set, else
// `XDG_STATE_HOME/culebra/history`, else `~/.culebra_history`. Returns
// an empty string when `$HOME` is unset (e.g. some sandboxed runs) —
// callers must treat that as "skip persistence".
inline std::string repl_history_path() {
  if (const char* override_ = std::getenv("CULEBRA_HISTFILE");
      override_ && *override_) {
    return override_;
  }
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/culebra/history";
  }
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::string(home) + "/.culebra_history";
}

inline int repl(std::shared_ptr<Environment> env, bool print_ast,
                bool jit_mode = false) {
  using namespace std;

#ifdef CULEBRA_JIT_ENABLED
  // Per-session JIT instance + globals dict. addIRModule of each input
  // keeps the prior input's compiled functions live, and run_repl
  // reads/writes `jit_globals` so `let x = 1` followed by `puts(x)`
  // works across inputs (mirrors other JIT-backed REPLs).
  std::unique_ptr<llvm::orc::LLJIT> jit_handle;
  JitReplGlobals jit_globals;
  if (jit_mode) {
    JIT::ensure_native_target_init();
    jit_handle = JIT::create_jit_instance();
  }
#endif

  // Default linenoise cap is 100 entries, which is small by REPL
  // convention (bash ≈ 500, python / node ≈ 1000). Bump before
  // LoadHistory so newly-loaded entries aren't truncated.
  linenoise::SetHistoryMaxLen(1000);

  auto histfile = repl_history_path();
  if (!histfile.empty()) {
    // Ensure the parent directory exists (matters for the XDG_STATE_HOME
    // path, which can land at a fresh-box location like
    // ~/.local/state/culebra/history). Errors are ignored — SaveHistory
    // will fail silently if the path is genuinely unwritable, matching
    // python / readline behavior.
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(histfile).parent_path(), ec);
    linenoise::LoadHistory(histfile.c_str());
  }

  // Save after each accepted line so a crash mid-session doesn't lose
  // history (matches the python / node REPL convention).
  auto add_history = [&](const std::string& line) {
    linenoise::AddHistory(line.c_str());
    if (!histfile.empty()) linenoise::SaveHistory(histfile.c_str());
  };

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
            auto v = JIT::run_repl(ast, jit_globals, *jit_handle);
            if (v.tag != TAG_NIL) {
              cout << _culebra_value_to_str_impl(v.tag, v.data) << endl;
            }
            _culebra_value_release_impl(v.tag, v.data);
            add_history(line);
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
          add_history(line);
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
