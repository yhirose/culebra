#pragma once

#include <vm.h>

#include <deque>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace culebra {

// Reading a parked program on the bytecode executor: the call stack, the
// names a frame can see, and expressions evaluated against one — the queries
// a source-level debugger asks, answered over the chunk debug tables rather
// than an environment chain.
//
// Everything here runs on the thread that runs the program, while it sits
// inside the statement hook. That is not a convenience: a frame's registers
// are that thread's machine stack, its Runtime (namespace cache, class
// registrations) and GC heap are thread-local, and an evaluated expression
// runs real bytecode against all three.
class VmDebugSession {
 public:
  struct Frame {
    std::string name;
    std::string path;
    int64_t line;
  };
  struct Var {
    std::string name;
    std::string value;
    std::string type;
  };

  ~VmDebugSession() {
    // The evaluated expressions' descriptor cells, which run_retained left
    // pinned so a closure one of them built could still be called.
    for (auto& r : retained_)
      if (r.prog) vm::Exec::release_descs(*r.prog);
  }

  // Innermost first, the order a call stack is shown in.
  std::vector<Frame> frames() const {
    std::vector<Frame> out;
    for (const auto* f : user_frames())
      out.push_back({frame_name(*f), f->prog->source_path, f->line});
    return out;
  }

  // The names in scope at a frame, with their current values — the frame's
  // own live bindings plus the module top level's, which are in scope
  // everywhere the frame does not shadow them. Functions are hidden (a
  // variables pane shows data), as are the compiler's own slots.
  std::vector<Var> variables(size_t frame_ix) const {
    std::vector<Var> out;
    for (const auto& [name, v] : visible(frame_ix)) {
      if (!user_name(name)) continue;
      JitValue val = read(v);
      if (val.tag == TAG_NO_SELF || val.tag == TAG_UNFILLED) continue;
      if (val.tag == TAG_FUNC) continue;
      out.push_back({name, display(val), _culebra_tag_name(val.tag)});
    }
    return out;
  }

  bool has_name(size_t frame_ix, const std::string& name) const {
    return visible(frame_ix).contains(name);
  }

  // Evaluate `expr` against a frame. The frame's live bindings become a REPL
  // session — the machinery a line's top-level names already use — so name
  // resolution, mutability and closure capture behave the way they do at the
  // prompt, and whatever the expression writes lands back in the frame.
  bool evaluate(size_t frame_ix, const std::string& expr, Var& out,
                std::string& err) {
    auto vis = visible(frame_ix);
    vm::ReplSessionSwap swap;
    for (const auto& [name, v] : vis) {
      if (!bindable_name(name)) continue;
      JitValue val = read(v);
      if (val.tag == TAG_NO_SELF || val.tag == TAG_UNFILLED) continue;
      auto* cell = swap.session.cell(name);
      culebra_runtime_value_retain(val.tag, val.data);
      cell->value = val;  // minted holding the sentinel, so nothing to free
      swap.session.set_mut(name, v.sd->is_mut);
    }
    std::vector<std::string> msgs;
    auto source = std::make_shared<std::string>(expr);
    auto ast = parse_with_transforms("(dap)", *source, msgs);
    if (!ast) {
      err = msgs.empty() ? "parse error" : msgs.front();
      return false;
    }
    bool ok = run(ast, source, err);
    // Even a throwing expression may have written before it threw, so the
    // frame takes what the session holds either way.
    for (const auto& [name, v] : vis) {
      if (!bindable_name(name)) continue;
      JitValue nv = swap.session.value(name);
      JitValue cur = read(v);
      if (nv.tag == cur.tag && nv.data == cur.data) continue;
      if (nv.tag == TAG_NO_SELF) continue;
      culebra_runtime_value_retain(nv.tag, nv.data);
      write(v, nv);
    }
    if (!ok) return false;
    auto* cell = vm::repl_session().cell(vm::kReplResultName);
    JitValue v = cell->value;
    cell->value = JitValue{TAG_NIL, 0};
    if (v.tag == TAG_NO_SELF) v = JitValue{TAG_NIL, 0};
    out = {"", display(v), _culebra_tag_name(v.tag)};
    _culebra_value_release_impl(v.tag, v.data);
    return true;
  }

  // A DAP `setVariable`: the assignment the client asked for, run as an
  // expression in the frame. Routing it through the session is what makes
  // immutability answer — a `let` name raises ImmutableError here exactly as
  // it does in the program.
  bool set_variable(size_t frame_ix, const std::string& name,
                    const std::string& expr, Var& out, std::string& err) {
    if (!evaluate(frame_ix, name + " = (" + expr + ")", out, err)) return false;
    out.name = name;
    return true;
  }

 private:
  // A program and everything its compilation read, kept for the session:
  // a closure the expression built reaches its bytecode through a descriptor
  // pointing into that program (vm_repl.h's Retained, same reason).
  struct Retained {
    std::shared_ptr<std::string> source;
    std::shared_ptr<peg::Ast> ast;
    std::unique_ptr<vm::VmProgram> prog;
  };

  // One visible name: where its value lives in the frame that owns it.
  struct Visible {
    const vm::Chunk::SlotDebug* sd;
    JitValue* regs;
  };

  bool run(const std::shared_ptr<peg::Ast>& ast,
           std::shared_ptr<std::string> source, std::string& err) {
    try {
      auto prog =
          std::make_unique<vm::VmProgram>(vm::Compiler::compile_repl_line(*ast));
      auto& kept =
          retained_.emplace_back(Retained{std::move(source), ast, std::move(prog)});
      // The expression is not a statement of the program being debugged: its
      // own boundaries must not re-enter the hook that is holding this thread.
      auto& st = vm::dbg_state();
      auto saved_hook = std::move(st.hook);
      st.hook = nullptr;
      struct Restore {
        vm::DbgState& st;
        vm::DbgHook& saved;
        ~Restore() { st.hook = std::move(saved); }
      } restore{st, saved_hook};
      vm::Exec::run_retained(*kept.prog);
      return true;
    } catch (const CulebraError& e) {
      err = std::string(e.kind) + ": " + e.what();
    } catch (const std::exception& e) {
      err = e.what();
    }
    return false;
  }

  // The frames a user can see: one per executor frame that carries statement
  // boundaries, innermost first. A frame whose chunk has none is not user
  // code (a constructor thunk, a stdlib module body) — the interpreter folds
  // its own internal delegations away for the same reason.
  std::vector<const vm::DbgFrame*> user_frames() const {
    std::vector<const vm::DbgFrame*> out;
    const auto& fs = vm::dbg_state().frames;
    for (auto it = fs.rbegin(); it != fs.rend(); ++it)
      if (it->chunk->has_dbg) out.push_back(&*it);
    return out;
  }

  static bool is_module_frame(const vm::DbgFrame& f) {
    return f.chunk == &f.prog->chunks[0];
  }

  static std::string frame_name(const vm::DbgFrame& f) {
    if (is_module_frame(f)) return "main";
    // The interpreter's name for a frame whose function was never named.
    return f.chunk->multifn_name.empty() ? "<anonymous>"
                                         : f.chunk->multifn_name;
  }

  // The compiler's own slots (`(for.val)`, a field-init thunk's `\x1f…`) are
  // bindings too; they are not names anyone typed.
  static bool bindable_name(std::string_view n) {
    return !n.empty() && n[0] != '(' && n[0] != '\x1f';
  }
  static bool user_name(std::string_view n) {
    return bindable_name(n) && n[0] != '_';
  }

  // Live bindings at `frame_ix`, name-ordered: the module top level first so
  // the frame's own names win a clash, which is the order a lexical chain
  // resolves them in.
  std::map<std::string, Visible, std::less<>> visible(size_t frame_ix) const {
    std::map<std::string, Visible, std::less<>> out;
    auto fs = user_frames();
    if (frame_ix >= fs.size()) return out;
    if (!fs.empty() && is_module_frame(*fs.back())) add_frame(*fs.back(), out);
    add_frame(*fs[frame_ix], out);
    return out;
  }

  static void add_frame(const vm::DbgFrame& f,
                        std::map<std::string, Visible, std::less<>>& out) {
    // A name declared twice in one frame (an inner scope shadowing an outer
    // one) has several ranges covering this pc; the innermost declaration is
    // the latest one, which is the one that started last.
    std::map<std::string, const vm::Chunk::SlotDebug*, std::less<>> best;
    for (const auto& sd : f.chunk->slot_debug) {
      if (f.pc < sd.start || f.pc >= sd.end) continue;
      auto it = best.find(sd.name);
      if (it == best.end() || it->second->start <= sd.start) best[sd.name] = &sd;
    }
    for (const auto& [name, sd] : best) out[name] = Visible{sd, f.regs};
  }

  static JitValue read(const Visible& v) {
    JitValue slot = v.regs[v.sd->slot];
    if (!v.sd->is_cell) return slot;
    if (slot.tag != TAG_LONG || slot.data == 0) return {TAG_NO_SELF, 0};
    return reinterpret_cast<JitCell*>(slot.data)->value;
  }

  // Stores an already-retained value, releasing what was there.
  static void write(const Visible& v, JitValue nv) {
    if (v.sd->is_cell) {
      JitValue slot = v.regs[v.sd->slot];
      if (slot.tag != TAG_LONG || slot.data == 0) return;
      auto* cell = reinterpret_cast<JitCell*>(slot.data);
      JitValue old = cell->value;
      cell->value = nv;
      _culebra_value_release_impl(old.tag, old.data);
      return;
    }
    JitValue old = v.regs[v.sd->slot];
    v.regs[v.sd->slot] = nv;
    _culebra_value_release_impl(old.tag, old.data);
  }

  // What a debugger shows a value as — the interpreter's `Value::str()`,
  // whose twin here is `inspect`'s text without the line `inspect` adds.
  static std::string display(JitValue v) {
    return _culebra_inspect_repr(v.tag, v.data);
  }

  std::deque<Retained> retained_;
};

}  // namespace culebra
