#pragma once

#include <repl_core.h>
#include <vm_session.h>

#include <memory>
#include <string>
#include <vector>

namespace culebra {

// The REPL on the bytecode VM's executor (`culebra --vm` with no script) —
// the tier-0 story repl.h tells about the interpreter, told about the engine
// that is going to replace it. What a session is, and why a line's program
// has to outlive it, is vm_session.h; this is the prompt on top of one.
class VmRepl {
 public:
  explicit VmRepl(bool dump) : dump_(dump) {}

  int run(bool print_ast) {
    std::vector<std::string> msgs;
    if (!session_.run_builtin_traits(msgs))
      for (const auto& m : msgs) std::cout << m << std::endl;
    return repl_loop(print_ast, [this](const std::shared_ptr<peg::Ast>& ast,
                                       std::vector<std::string>& line_msgs) {
      return eval_line(ast, line_msgs);
    });
  }

 private:
  bool eval_line(const std::shared_ptr<peg::Ast>& ast,
                 std::vector<std::string>& msgs) {
    if (!session_.run_stdlib_delta(*ast, msgs)) return false;
    auto dump = [this](const vm::VmProgram& p) {
      if (dump_) std::cout << vm::dump(p);
    };
    if (!session_.run_unit(ast, nullptr, /*session=*/true, msgs, dump))
      return false;
    echo_result();
    return true;
  }

  // What the prompt shows: the value of the line's last statement, which the
  // program left in the session's result cell. Released right after, because
  // that is where the interp's own `val` dies — a resource echoed and bound
  // to nothing drops before the next prompt, not one line later (probed).
  void echo_result() {
    JitValue v = session_.take_result();
    // A nil result isn't echoed (repl_loop's convention). The sentinel is
    // what the cell holds when the line stored nothing at all — a statement
    // that jumped away, or a program that never reached the store.
    if (v.tag != TAG_NIL && v.tag != TAG_NO_SELF)
      _culebra_inspect_to(std::cout, v.tag, v.data);
    _culebra_value_release_impl(v.tag, v.data);
  }

  bool dump_;
  vm::Session session_;
};

inline int vm_repl(bool print_ast, bool dump) {
  return VmRepl(dump).run(print_ast);
}

}  // namespace culebra
