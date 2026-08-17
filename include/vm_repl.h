#pragma once

#include <repl.h>
#include <stdlib_interp.h>  // stdlib_preamble_for / stdlib_preamble_triggers
#include <vm.h>

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace culebra {

// The REPL on the bytecode VM's executor (`culebra --vm` with no script) —
// the tier-0 story repl.h tells about the interpreter, told about the engine
// that is going to replace it.
//
// Each input compiles to its own VmProgram whose top-level bindings live in
// vm::ReplSession's cells, which is the interp REPL's single persistent
// Environment. Cells are not all that has to outlive a line, though: a
// closure the line built reaches its bytecode through a descriptor pointing
// into that line's program, so the programs — and the sources and ASTs they
// were compiled from — are retained for the whole session too. Memory is
// O(session length), the same bound repl_loop accepts for its sources.
class VmRepl {
 public:
  explicit VmRepl(bool dump) : dump_(dump) {}

  ~VmRepl() {
    // The session is what owned every program's descriptor cells (see
    // Exec::run_retained); hand them back now that no line can call into
    // one again.
    for (auto& r : retained_)
      if (r.prog) vm::Exec::release_descs(*r.prog);
  }

  int run(bool print_ast) {
    // The built-in traits (Eq's `neq`, Comparable's four comparisons, ...)
    // are a session-wide registration, not a per-line one: run them once,
    // where the script lane runs them once as chunk 0's prologue.
    if (auto traits = parse_builtin_traits_preamble()) {
      std::vector<std::string> msgs;
      if (!run_unit(traits, nullptr, /*repl_line=*/false, msgs))
        for (const auto& m : msgs) std::cout << m << std::endl;
    }
    return repl_loop(print_ast, [this](const std::shared_ptr<peg::Ast>& ast,
                                       std::vector<std::string>& msgs) {
      return eval_line(ast, msgs);
    });
  }

 private:
  // A program and everything its compilation read. Chunks intern their own
  // string constants, but retaining the source and the AST beside the program
  // is what makes "a line's closure still works three lines later" true
  // without auditing every field for a borrowed token.
  struct Retained {
    std::shared_ptr<std::string> source;  // null for the traits preamble
    std::shared_ptr<peg::Ast> ast;
    std::unique_ptr<vm::VmProgram> prog;
  };

  bool eval_line(const std::shared_ptr<peg::Ast>& ast,
                 std::vector<std::string>& msgs) {
    if (!run_stdlib_delta(*ast, msgs)) return false;
    if (!run_unit(ast, nullptr, /*repl_line=*/true, msgs)) return false;
    echo_result();
    return true;
  }

  // The stdlib modules this line names that no earlier one pulled in. A
  // script's loader sees the whole token set before it splices; a REPL sees
  // one line at a time, so the set grows with the session and each line
  // registers only what is new — registering a builder twice would mint a
  // second instance of the namespace, and values an earlier line built would
  // stop matching it.
  bool run_stdlib_delta(const peg::Ast& ast, std::vector<std::string>& msgs) {
    std::unordered_set<std::string_view> tokens;
    collect_ast_tokens(ast, tokens);
    std::unordered_set<std::string_view> fresh;
    for (auto t : tokens)
      if (!registered_.contains(t)) fresh.insert(t);
    auto triggers = stdlib_preamble_triggers(fresh);
    if (triggers.empty()) return true;
    auto src = std::make_shared<std::string>(stdlib_preamble_for(fresh));
    for (auto t : triggers) registered_.emplace(t);
    std::vector<std::string> parse_msgs;
    auto pre = parse_with_transforms(kStdlibPreamblePath, *src, parse_msgs);
    if (!pre) {  // the stdlib is trusted; a parse failure is a build bug
      msgs.insert(msgs.end(), parse_msgs.begin(), parse_msgs.end());
      return false;
    }
    return run_unit(pre, src, /*repl_line=*/false, msgs);
  }

  // Compile and run one program, retaining it for the session. Errors are
  // reported the way interpret() reports them, so a line that throws prints
  // the same text on either engine and the session carries on.
  bool run_unit(const std::shared_ptr<peg::Ast>& ast,
                std::shared_ptr<std::string> source, bool repl_line,
                std::vector<std::string>& msgs) {
    try {
      auto prog = std::make_unique<vm::VmProgram>(
          repl_line
              ? vm::Compiler::compile_repl_line(*ast)
              : vm::Compiler::compile_repl_prologue(*ast,
                                                    /*builtin_traits=*/false));
      if (dump_ && repl_line) std::cout << vm::dump(*prog);
      auto& kept = retained_.emplace_back(
          Retained{std::move(source), ast, std::move(prog)});
      vm::Exec::run_retained(*kept.prog);
      return true;
    } catch (const CulebraError& e) {
      // interpret()'s formatter, which is main.cc's: the structured position
      // when the error carries one.
      if (e.line > 0 || e.col > 0)
        msgs.push_back(
            std::format("{}: {} at {}:{}.", e.kind, e.what(), e.line, e.col));
      else
        msgs.push_back(std::format("{}: {}", e.kind, e.what()));
    } catch (const std::exception& e) {
      // Exec::run has already turned an uncaught user throw into the
      // "uncaught: ..." line both other backends print for one.
      msgs.push_back(e.what());
    }
    return false;
  }

  // What the prompt shows: the value of the line's last statement, which the
  // program left in the session's result cell. Released right after, because
  // that is where the interp's own `val` dies — a resource echoed and bound
  // to nothing drops before the next prompt, not one line later (probed).
  void echo_result() {
    auto* cell = vm::repl_session().cell(vm::kReplResultName);
    JitValue v = cell->value;
    cell->value = JitValue{TAG_NIL, 0};
    // A nil result isn't echoed (repl_loop's convention). The sentinel is
    // what the cell holds when the line stored nothing at all — a statement
    // that jumped away, or a program that never reached the store.
    if (v.tag != TAG_NIL && v.tag != TAG_NO_SELF)
      _culebra_inspect_to(std::cout, v.tag, v.data);
    _culebra_value_release_impl(v.tag, v.data);
  }

  bool dump_;
  std::deque<Retained> retained_;
  std::set<std::string, std::less<>> registered_;
};

inline int vm_repl(bool print_ast, bool dump) {
  return VmRepl(dump).run(print_ast);
}

}  // namespace culebra
