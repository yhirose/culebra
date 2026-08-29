#pragma once

// A session on the bytecode VM's executor: a scope whose top-level bindings
// outlive the program that made them.
//
// Two consumers need that and need it for the same reason. The REPL calls back
// into an earlier line; `culebra test` calls back into a file after it has run.
// Both compile each input as a session unit (vm::Compiler::compile_repl_line),
// so its top-level bindings land in vm::ReplSession's cells rather than in the
// frame that ran it — and both must then keep the program alive, because a
// closure reaches its bytecode through a descriptor pointing into it.
//
// The stdlib delta is the other shared half. A script's loader sees the whole
// token set before it splices; a session sees one input at a time, so the set
// grows with the session and each input registers only what is new —
// registering a builder twice would mint a second instance of the namespace,
// and values an earlier input built would stop matching it.

#include <stdlib_preamble.h>  // stdlib_preamble_for / stdlib_preamble_triggers
#include <vm.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace culebra::vm {

class Session {
 public:
  // The built-in traits (Eq's `neq`, Comparable's four comparisons, ...) are a
  // session-wide registration, not a per-input one: run them once, where the
  // script lane runs them once as chunk 0's prologue.
  bool run_builtin_traits(std::vector<std::string>& msgs) {
    auto traits = parse_builtin_traits_preamble();
    return !traits || run_unit(traits, nullptr, /*session=*/false, msgs);
  }

  // The stdlib modules this input names that no earlier one pulled in.
  bool run_stdlib_delta(const peg::Ast& ast, std::vector<std::string>& msgs) {
    std::unordered_set<std::string_view> tokens;
    collect_ast_tokens(ast, tokens);
    return run_stdlib_delta(tokens, msgs);
  }

 private:
  bool run_stdlib_delta(const std::unordered_set<std::string_view>& tokens,
                        std::vector<std::string>& msgs) {
    std::unordered_set<std::string_view> fresh;
    for (auto t : tokens)
      if (!registered_.contains(t)) fresh.insert(t);
    auto triggers = stdlib_preamble_triggers(fresh);
    if (triggers.empty()) return true;
    for (auto t : triggers) registered_.emplace(t);
    std::vector<std::string> parse_msgs;
    auto pre = parse_preamble(stdlib_preamble_for(fresh), parse_msgs);
    if (!pre.ast) {  // the stdlib is trusted; a parse failure is a build bug
      msgs.insert(msgs.end(), parse_msgs.begin(), parse_msgs.end());
      return false;
    }
    return run_unit(pre.ast, pre.source, /*session=*/false, msgs);
  }

 public:

  // Compile and run one program, retaining it for the session. Errors are
  // reported the way interpret() reports them, so an input that throws prints
  // the same text on either engine and the session carries on. `session` picks
  // whether the input's top-level bindings are the session's (an input) or the
  // program's own (a prologue, which binds nothing that has to outlive it).
  bool run_unit(const std::shared_ptr<peg::Ast>& ast,
                std::shared_ptr<std::string> source, bool session,
                std::vector<std::string>& msgs,
                const std::function<void(const VmProgram&)>& before_run = {}) {
    return run_reported(msgs, [&] {
      auto prog = std::make_unique<VmProgram>(
          session ? Compiler::compile_repl_line(*ast)
                  : Compiler::compile_stdlib_prologue(*ast));
      if (before_run) before_run(*prog);
      auto& kept = retained_.keep(std::move(source), ast, std::move(prog));
      Exec::run(*kept.prog);
    });
  }

  // A loader's whole module list as one session unit (the embedding lane;
  // Compiler::compile_session_modules holds the module-isolation story). The
  // modules are retained for the session — a closure the host later calls
  // reaches its bytecode through the program, and its tokens through the
  // module sources. The spliced stdlib preamble's registrations are noted so
  // a later per-input delta cannot register a module twice (a second
  // registration mints a second namespace instance).
  bool run_modules(const std::vector<LoadedModule>& modules,
                   std::vector<std::string>& msgs) {
    // The stdlib the list names, unless the list already carries it: the CLI
    // splices a `<stdlib>` module in front, and registering a builder twice
    // mints a second instance of the namespace. A caller that hands over a
    // bare loader result — the embedding lane, and `culebra test` on a file
    // that imports — gets the delta here, over the union of every module's
    // tokens, which is the one preamble the splice would have produced.
    bool spliced = !modules.empty() &&
                   modules.front().abs_path == kStdlibPreamblePath;
    if (!spliced) {
      std::unordered_set<std::string_view> tokens;
      for (const auto& m : modules)
        if (m.ast) collect_ast_tokens(*m.ast, tokens);
      if (!run_stdlib_delta(tokens, msgs)) return false;
    }
    return run_reported(msgs, [&] {
      auto prog =
          std::make_unique<VmProgram>(Compiler::compile_session_modules(modules));
      for (const auto& m : modules) {
        std::unordered_set<std::string_view> tokens;
        if (m.ast) collect_ast_tokens(*m.ast, tokens);
        for (auto t : stdlib_preamble_triggers(tokens)) registered_.emplace(t);
      }
      retained_modules_.push_back(modules);  // shared_ptrs: sources + ASTs
      auto& kept = retained_.keep(nullptr, modules.back().ast, std::move(prog));
      Exec::run(*kept.prog);
    });
  }

  // Take the value a session unit left in the result cell, clearing it. The
  // REPL echoes it; a caller that has no prompt still has to take it, or the
  // input's last value stays pinned for the rest of the session.
  JitValue take_result() {
    auto* cell = repl_session().cell(kReplResultName);
    JitValue v = cell->value;
    cell->value = JitValue{TAG_NIL, 0};
    return v;
  }

  void drop_result() {
    JitValue v = take_result();
    _culebra_value_release_impl(v.tag, v.data);
  }

 private:
  // A preamble text and the AST whose tokens point into it.
  struct ParsedPreamble {
    std::shared_ptr<std::string> source;
    std::shared_ptr<peg::Ast> ast;
  };

  // Parsing is the expensive half and depends on nothing but the text, so it
  // is cached by that text and the AST run into each session that asks —
  // parse_builtin_traits_preamble's bargain, for a set that isn't fixed. The
  // sessions `culebra test` opens per file mostly name the same few modules,
  // and without this each re-parses the whole preamble it splices.
  //
  // Per thread, because an embedding may run sessions on several and the cache
  // grows as it is read.
  static ParsedPreamble parse_preamble(std::string text,
                                       std::vector<std::string>& parse_msgs) {
    static thread_local std::map<std::string, ParsedPreamble, std::less<>> cache;
    if (auto it = cache.find(text); it != cache.end()) return it->second;
    auto src = std::make_shared<std::string>(std::move(text));
    auto ast = parse_with_transforms(kStdlibPreamblePath, *src, parse_msgs);
    if (!ast) return {};
    return cache.emplace(*src, ParsedPreamble{src, ast}).first->second;
  }

  // Run `body`, reporting what it throws the way interpret() reports it. An
  // interrupt passes through: the session hosts no loop, so whoever does (the
  // REPL, an embedder) answers for it, and a runner stops on it.
  template <class Body>
  static bool run_reported(std::vector<std::string>& msgs, Body&& body) {
    try {
      body();
      return true;
    } catch (const CulebraError& e) {
      if (is_interrupt(e)) throw;
      // interpret()'s formatter, which is main.cc's.
      msgs.push_back(format_error_message(e));
    } catch (const std::exception& e) {
      // Exec::run has already turned an uncaught user throw into the
      // "uncaught: ..." line both other backends print for one.
      msgs.push_back(e.what());
    }
    return false;
  }

  RetainedRuns retained_;
  // Module lists run_modules kept alive (shared_ptr'd sources + ASTs): a
  // retained program's chunks hold string_views into them.
  std::vector<std::vector<LoadedModule>> retained_modules_;
  std::set<std::string, std::less<>> registered_;
};

}  // namespace culebra::vm
