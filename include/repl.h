#pragma once

#include <cstdlib>
#include <filesystem>
#include <linenoise.hpp>
#include "interpreter.h"
#include "stdlib_interp.h"  // STDLIB_PREAMBLE_SOURCE

#ifdef CULEBRA_JIT_ENABLED
#include "jit.h"
#endif

namespace culebra {

// Best-effort "is this input syntactically complete?" check. Returns
// false when an opener (`{`/`(`/`[`) hasn't been closed yet — that's
// the signal to keep accumulating lines before parsing. Tracks single
// quotes, double-quoted strings with `{expr}` interpolation (where
// `{` opens an expression that nests with `}`), `\\` escapes inside
// strings, and `#` line comments. Conservative: any ambiguity (e.g.
// malformed string) returns true so the parser surfaces a real error
// rather than silently swallowing the input.
//
// Coupled to the lexical shape of the grammar (see parser.h). Update
// this if the language adds raw strings (`r"..."`), triple-quoted
// strings, block comments, or other bracket-shaped tokens — without
// the update, those would either falsely accept (input parses early)
// or stick the REPL in continuation mode.
inline bool repl_input_is_complete(std::string_view src) {
  int braces = 0, parens = 0, brackets = 0;
  bool in_squote = false;          // '...' (no interp, no escapes)
  bool in_dquote = false;          // "..." (escapes + {expr} interp)
  int dquote_interp_depth = 0;     // nested `{` count inside "..."
  bool escape = false;
  for (size_t i = 0; i < src.size(); i++) {
    char c = src[i];
    if (escape) { escape = false; continue; }
    if (in_squote) {
      if (c == '\'') in_squote = false;
      continue;
    }
    if (in_dquote) {
      if (c == '\\') { escape = true; continue; }
      if (dquote_interp_depth > 0) {
        if (c == '{') dquote_interp_depth++;
        else if (c == '}') dquote_interp_depth--;
        continue;
      }
      if (c == '{') { dquote_interp_depth = 1; continue; }
      if (c == '"') in_dquote = false;
      continue;
    }
    if (c == '#') {  // line comment
      while (i < src.size() && src[i] != '\n') i++;
      continue;
    }
    switch (c) {
      case '\'': in_squote = true; break;
      case '"':  in_dquote = true; break;
      case '{':  braces++; break;
      case '}':  if (braces > 0) braces--; break;
      case '(':  parens++; break;
      case ')':  if (parens > 0) parens--; break;
      case '[':  brackets++; break;
      case ']':  if (brackets > 0) brackets--; break;
    }
  }
  return braces == 0 && parens == 0 && brackets == 0 &&
         !in_squote && !in_dquote;
}

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
  // works across inputs (mirrors other JIT-backed REPLs). Preamble
  // (stdlib `Time` module) is compiled into `jit_globals` once at
  // session start so subsequent inputs resolve `Time.*` normally.
  std::unique_ptr<llvm::orc::LLJIT> jit_handle;
  JitReplGlobals jit_globals;
  if (jit_mode) {
    JIT::ensure_native_target_init();
    jit_handle = JIT::create_jit_instance();
    std::vector<std::string> pre_msgs;
    auto pre_src = STDLIB_PREAMBLE_SOURCE;
    auto pre_ast = parse_with_transforms("<stdlib>", pre_src,
                                         std::strlen(pre_src), pre_msgs);
    if (!pre_ast) {
      std::fprintf(stderr, "culebra: stdlib preamble failed to parse\n");
      for (auto& m : pre_msgs) std::fprintf(stderr, "  %s", m.c_str());
      std::abort();
    }
    try {
      auto v = JIT::run_repl(pre_ast, jit_globals, *jit_handle);
      _culebra_value_release_impl(v.tag, v.data);
    } catch (std::exception& e) {
      std::fprintf(stderr, "culebra: stdlib preamble failed in JIT REPL: %s\n",
                   e.what());
      std::abort();
    }
  }
#endif
  // interp REPL relies on the env's lazy Time / Args bindings registered
  // by `environment()`; no explicit preamble load needed. JIT REPL still
  // pre-runs the preamble above (Phase 3 of [[project-startup-overhead]]).

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

  // Accumulator for multi-line input: when a line leaves an opener
  // unclosed, the next prompt switches to a continuation marker and
  // appends instead of starting fresh. Matches the Python / Julia /
  // Node REPL convention.
  std::string accum;

  // Per-session retention of every accepted input's source buffer.
  // AST token `string_view`s point into this buffer, and the
  // FunctionValues created during eval need those tokens (parameter
  // names, return types) to outlive the input that produced them.
  //
  // The AST nodes themselves do NOT need a separate retention list:
  // `FunctionValue`'s `eval` lambda captures the body subtree's
  // `shared_ptr<peg::Ast>` by value, which keeps the body and all
  // its descendants alive even after the input's root AST is
  // destroyed. Only the source-buffer string needs explicit retention.
  //
  // Memory is O(session length). Typical interactive sessions
  // (hundreds of inputs) sit in the tens-of-KB range, which we
  // accept. A very long-running REPL (>>10K accepted inputs) would
  // accumulate noticeably; an LRU-style eviction parallel to the
  // JIT's per-input ResourceTracker (see `JIT::run_repl`) is the
  // natural future cleanup.
  std::vector<std::string> retained_sources_;

  for (;;) {
    bool continuing = !accum.empty();
    auto prompt = continuing ? "...> "
                             : (jit_mode ? "jit> " : "cul> ");
    bool eof_signal = false;
    auto line = linenoise::Readline(prompt, eof_signal);

    // Exit on Ctrl-D / EOF (linenoise sets `eof_signal` in raw mode)
    // or on piped-stdin closure (the unsupported-terminal path
    // returns an empty line silently when std::getline hits EOF).
    if (eof_signal || (line.empty() && std::cin.eof())) break;

    if (!continuing && (line == "exit" || line == "quit")) {
      break;
    }

    if (line.empty() && !continuing) continue;

    // Empty line in continuation mode acts as "force-submit": let
    // the parser surface whatever error it would for the accumulated
    // text, so an accidentally-stuck session has an escape hatch.
    if (!line.empty()) {
      if (!accum.empty()) accum.push_back('\n');
      accum += line;
    }
    if (!repl_input_is_complete(accum)) continue;

    auto full_line = std::move(accum);
    accum.clear();
    {
      vector<string> msgs;
      // Retain the source buffer + parsed AST for the whole session.
      // `peg::Ast` nodes hold `string_view` tokens into the source
      // buffer, and FunctionValues capture body-subtree shared_ptr's
      // whose lifetimes alone don't keep the source alive. Without
      // retention, every REPL-defined function dangles its parameter
      // names + body tokens as soon as the next input drops the
      // previous line. Bounded by session length (one entry per
      // accepted input). The JIT path is unaffected — JIT REPL
      // copies the names it cares about into LLVM module globals.
      retained_sources_.push_back(full_line);
      const auto& src = retained_sources_.back();
      auto ast = parse_with_transforms("(repl)", src.data(), src.size(), msgs);
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
            add_history(full_line);
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
          add_history(full_line);
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
