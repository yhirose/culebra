#pragma once

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <linenoise.hpp>
#include "interpreter.h"
#include "stdlib_interp.h"  // culebra::environment()

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

// One accepted input, handed to whichever engine the session runs on. The
// engine prints its own echo — the two hold a result differently, an interp
// `Value` against the VM's tagged pair — and returns false when it put a
// report in `msgs` for the loop to print.
using ReplEval = std::function<bool(const std::shared_ptr<peg::Ast>&,
                                    std::vector<std::string>& msgs)>;

// The REPL always runs on a tier-0 engine: the interpreter, or the bytecode
// VM's executor under `--vm` (see vm_repl.h). A REPL line is never a hot loop,
// so compiling each input only adds latency for no gain — the same reason V8 /
// the JVM / LuaJIT start interpreted and only JIT hot code. The compiled lane
// (`--jit`) is for scripts; combined with the REPL it is a no-op the CLI notes
// and ignores (see main.cc).
//
// This is the line editor and session bookkeeping both engines share; `eval`
// is the engine.
inline int repl_loop(bool print_ast, const ReplEval& eval) {
  using namespace std;

  // Ctrl+C interrupts the running eval and returns to the prompt (rather than
  // killing the REPL): the handler sets the global SIGINT flag, the interp /
  // JIT safepoints throw a catchable Interrupted, and the per-input try/catch
  // below prints it and loops. During line editing linenoise holds the
  // terminal in raw mode with ISIG disabled, so Ctrl+C there is the literal
  // 0x03 it handles itself (cancel the line) — no signal — and only an
  // in-flight eval (cooked mode) sees SIGINT. The flag is cleared before each
  // eval so a stray press at the prompt doesn't carry into the next one and
  // the force-kill second press only applies within a single wedged eval.
  install_sigint_handler();

  // Reap whatever is still outstanding once the REPL session itself ends
  // (Ctrl-D, `exit`/`quit`, or an unexpected early return) —
  // ScriptTeardownGuard (interpreter.h) is function-scoped here instead of
  // per-input: an Isolate.spawn'd background task (or a watch bound to a
  // variable) is expected to keep running across multiple REPL lines, so
  // reaping after every `interpret()` call below would cancel legitimate
  // in-flight work instead of just clearing stragglers at session exit.
  ScriptTeardownGuard script_teardown_guard;

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
  // Must be a `std::deque`, not a `std::vector`: a vector's `push_back`
  // can reallocate and relocate every existing element, and a small
  // input (e.g. `fn g(a) { a + 1 }`) lives inline in the `std::string`
  // via SSO, so relocation moves its bytes to a new address and leaves
  // a previously-defined function's `string_view` tokens dangling.
  // `std::deque` never relocates existing elements on `push_back`, so
  // every retained source — and the SSO bytes inside it — keeps a
  // stable address for the whole session.
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
  // accumulate noticeably; an LRU-style eviction is the natural
  // future cleanup.
  std::deque<std::string> retained_sources_;

  for (;;) {
    bool continuing = !accum.empty();
    auto prompt = continuing ? "...> " : "cul> ";
    bool eof_signal = false;
    // `print(...)` (unlike `println`/`inspect`) writes no trailing
    // newline, so it can sit unflushed in cout's buffer until some
    // later `std::endl` call flushes it — visibly delaying it past
    // the next prompt. Flush here so anything buffered lands before
    // the prompt that follows it.
    cout.flush();
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
      // accepted input).
      retained_sources_.push_back(full_line);
      const auto& src = retained_sources_.back();
      auto ast = parse_with_transforms("(repl)", src, msgs);
      if (ast) {
        if (print_ast) {
          cout << peg::ast_to_s(ast);
        }
        // Fresh interrupt state per eval: a Ctrl+C only cancels the eval it
        // lands in, and the second (force-killing) press is scoped to one
        // wedged eval.
        culebra_g_sigint.store(false, std::memory_order_relaxed);

        if (eval(ast, msgs)) {
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
