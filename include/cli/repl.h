#pragma once

#include <cli/repl_core.h>
#include <vm/session.h>

#include <memory>
#include <string>
#include <vector>

namespace culebra {

// The REPL on the bytecode VM's executor (`culebra` with no script). What a
// session is, and why a line's program has to outlive it, is session.h; this
// is the prompt on top of one.
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
    return echo_result(msgs);
  }

  // What the prompt shows: the value of the line's last statement, which the
  // program left in the session's result cell. Released right after, because
  // that is where a line's value dies — a resource echoed and bound to
  // nothing drops before the next prompt, not one line later (probed).
  bool echo_result(std::vector<std::string>& msgs) {
    JitOwnedVal v(session_.take_result());
    const JitValue r = v.borrow();
    // A nil result isn't echoed (repl_loop's convention). The sentinel is
    // what the cell holds when the line stored nothing at all — a statement
    // that jumped away, or a program that never reached the store.
    if (r.tag == TAG_NIL || r.tag == TAG_NO_SELF) return true;
    // The echo runs the value's `__str__`: what that raises is the line's
    // failure, and the prompt goes on. The echo is no op of the line's, so
    // an error without a position takes none from the last op that ran.
    return session_.run_guarded(msgs, [&] {
      culebra_runtime_set_op_pos(0, 0);
      _culebra_inspect_to(std::cout, r.tag, r.data);
    });
  }

  bool dump_;
  vm::Session session_;
};

inline int vm_repl(bool print_ast, bool dump) {
  return VmRepl(dump).run(print_ast);
}

}  // namespace culebra
