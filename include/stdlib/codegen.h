#pragma once
// CodeGen.Module -- a script-visible builder over cpp-vmlib's Core-IR and its
// register-bytecode compiler/executor (vendor/cpp-vmlib), plus Program and
// Runtime (below), the compile-once/run-many-times split over the same
// library. Wrapped for scripts via wrap.h in codegen_binding.h.
//
// IR nodes are plain int64_t (coreir::NodeId::v), not handles: a Module
// method taking one as an argument only needs a scalar, not wrap.h's
// (newer) handle-argument support, and every node a script holds onto has
// to be a scalar regardless -- there is no reader that could hand one back
// any other way. Variadic shapes (a Block's statements, a Call's arguments,
// a capture map's entries) go through a small generic list-staging
// mechanism for the same reason -- see list_new/list_push and
// capture_map_new/capture_map_push below.

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vmlib.h"

#include "base/shared.h"  // culebra::CulebraError
#include <interop/value_args.h>  // JitObjectArg, JitListArg
// The natives bridge below marshals culebra values and calls a culebra
// closure, so this header needs the runtime layer -- the same reason
// interop/wrap.h includes it rather than waiting for culebra.h.
#include <rt/rt.h>

namespace culebra::codegen {

class Program;
class Resolver;

// The compile-time-checked heap CodeGen.Program.run() actually runs on.
// Exposes only what a host may safely watch from outside: live_objects()/
// heap_bytes() (Runtime::collect() runs every condemned object's drop hook
// first, same as the 'collect' intrinsic; calling it here between runs does
// too), never the GC/host-embedding hooks cpp-vmlib's Runtime also carries
// (set_drop_fn, first_object, owned_*, Runtime::Scope) -- a script cannot be
// a host, and exposing them would put a memory-safety violation within
// script reach.
class Runtime {
 public:
  Runtime() = default;

  int64_t live_objects() const { return rt_.live_objects(); }
  int64_t heap_bytes() const { return rt_.heap_bytes(); }
  int64_t collect() { return rt_.collect(); }

 private:
  friend class Program;
  coreir::Runtime rt_;
  // The id of the Program this heap's objects belong to (0 = none yet).
  // Program::run's own guard reads this; nothing else does.
  uint64_t bound_ = 0;
};

// --- Host functions: what a module's declared natives actually do ----------
//
// A module carries native NAMES (Module::declare_native); vm::RunOptions
// carries the implementations, as C++ function pointers. A script cannot
// produce one of those, so Program.run bridges: it takes an Object mapping
// each declared name to an ordinary culebra Function, and registers one
// NativeFn per name that marshals the arguments across, calls the closure,
// and marshals the answer back.
//
// The two heaps stay apart: coreir::Runtime's and culebra's are collected
// independently, with no coordination, so only values with no heap identity
// to share cross -- nil, bools, ints, doubles and strings, as copies.
// Anything else is a named TypeError rather than a silent coercion.
namespace natives {

// The five kinds, guest -> culebra. `what` names the side and the slot for
// the TypeError a value of any other kind gets -- a callable, not a string,
// because every crossing that succeeds would otherwise pay to build and
// throw away a message. This runs once per argument of every native call.
template <class What>
inline JitValue marshal_in(const coreir::Value& v, What&& what) {
  if (v.is_nil()) return {TAG_NIL, 0};
  if (v.is_bool()) return {TAG_BOOL, v.as_bool() ? 1 : 0};
  if (v.is_int()) return {TAG_LONG, v.as_int()};
  if (v.is_double()) {
    int64_t bits = 0;
    const double d = v.as_double();
    std::memcpy(&bits, &d, sizeof d);
    return {TAG_FLOAT, bits};
  }
  if (v.is_str()) {
    return {TAG_STRING,
            reinterpret_cast<int64_t>(_culebra_heap_str(v.as_str()))};
  }
  throw culebra::CulebraError("TypeError", what(), 0, 0);
}

// The same five, culebra -> guest, `what` lazy for the same reason.
// Allocates in the guest's own heap, which is the one current on this thread
// for the length of the run.
template <class What>
inline coreir::Value marshal_out(JitValue v, What&& what) {
  switch (v.tag) {
    case TAG_NIL: return coreir::Value();
    case TAG_BOOL: return coreir::Value::make_bool(v.data != 0);
    case TAG_LONG: return coreir::Value::make_int(v.data);
    case TAG_FLOAT: {
      double d = 0;
      std::memcpy(&d, &v.data, sizeof d);
      return coreir::Value::make_double(d);
    }
    case TAG_STRING:
    case TAG_STRINGVIEW:
      return coreir::Value::make_str(
          std::string(_culebra_str_view(v.tag, v.data)));
    default:
      throw culebra::CulebraError("TypeError", what(), 0, 0);
  }
}

// The {kind, message, line, col} object a native's failure carries: the
// executor's own trap shape (NativeCall::trap) plus the kind, so a guest
// TryCatch reads a bridged error the same way it reads a native one.
inline coreir::Value make_error(const std::string& kind,
                                const std::string& message,
                                coreir::SrcPos pos) {
  coreir::Value e = coreir::Value::make_object();
  e.as_object()->set("kind", coreir::Value::make_str(kind));
  e.as_object()->set("message", coreir::Value::make_str(message));
  e.as_object()->set("line", coreir::Value::make_int(pos.line));
  e.as_object()->set("col", coreir::Value::make_int(pos.col));
  return e;
}

// A culebra throw crossing into the guest: a scalar as itself, so the
// program's catch sees exactly what was thrown, anything else as the
// {kind, message, line, col} object a culebra error would have shown.
inline coreir::Value cross_thrown(JitValue thrown, coreir::SrcPos pos) {
  switch (thrown.tag) {
    case TAG_NIL:
    case TAG_BOOL:
    case TAG_LONG:
    case TAG_FLOAT:
    case TAG_STRING:
    case TAG_STRINGVIEW:
      return marshal_out(thrown, [] { return std::string(); });
    default: {
      std::string kind, message;
      describe_thrown_value(thrown, kind, message);
      return make_error(kind.empty() ? "RuntimeError" : kind, message, pos);
    }
  }
}

// The closure runs over an empty thrown-value carrier, and whatever it left
// there is released on the way out: this is the pair a guarded call uses
// when it answers its own exception (mem.inc.h's drop hook is the same
// shape). Both halves matter -- the closure must not see a throw already in
// flight further out, and a throw it makes is answered here rather than by
// culebra, so leaving it in the carrier would hand the NEXT run this
// program's error. RAII because Interrupted crosses this frame too, and it
// must not skip the restore on its way to ending the run.
struct ThrownCarrierGuard {
  int8_t flag = 0, tag = 0;
  int64_t data = 0;
  ThrownCarrierGuard() { culebra_runtime_save_thrown(&flag, &tag, &data); }
  ~ThrownCarrierGuard() { culebra_runtime_restore_thrown(flag, tag, data); }
  ThrownCarrierGuard(const ThrownCarrierGuard&) = delete;
  ThrownCarrierGuard& operator=(const ThrownCarrierGuard&) = delete;
};

// One bound native. The guard holds this table's own reference to the
// closure; `cls` is the borrowed pointer the shim reaches it through, and
// the address of the BoundNative itself is what rides in NativeDef::ctx --
// hence unique_ptr, so a growing vector never moves it.
struct BoundNative {
  std::string name;
  JitClosure* cls = nullptr;
  JitOwnedVal guard;
};

// The NativeFn the guest calls. Every exit the guest can observe goes
// through `result` or `error`; an Interrupted still ends the whole run, as
// it does everywhere else.
inline bool shim(coreir::NativeCall& call) {
  auto* bn = static_cast<BoundNative*>(call.ctx);
  ThrownCarrierGuard carrier;
  try {
    if (call.argc != static_cast<int32_t>(bn->cls->arity)) {
      call.error = make_error(
          "ArityError",
          culebra::format("{} takes {} argument(s), given {}", bn->name,
                          static_cast<int64_t>(bn->cls->arity),
                          static_cast<int64_t>(call.argc)),
          call.pos);
      return false;
    }
    std::vector<JitValue> argv;
    argv.reserve(static_cast<size_t>(call.argc));
    for (int32_t i = 0; i < call.argc; i++) {
      argv.push_back(marshal_in(call.arg(i), [&] {
        return culebra::format(
            "native '{}': argument {} is a {}, which cannot cross into a "
            "culebra function (nil, Bool, Long, Float and String do)",
            bn->name, static_cast<int64_t>(i),
            coreir::type_name(call.arg(i).tag()));
      }));
    }
    JitOwnedVal ret(_jit_invoke(bn->cls, JitValue{TAG_NO_SELF, 0}, call.argc,
                                argv.empty() ? nullptr : argv.data()));
    call.result = marshal_out(ret.borrow(), [&] {
      return culebra::format(
          "native '{}' returned a value that cannot cross back into the "
          "program (nil, Bool, Long, Float and String do)",
          bn->name);
    });
    return true;
  } catch (const CulebraException& e) {
    call.error = cross_thrown({e.tag, e.data}, call.pos);
    return false;
  } catch (const culebra::CulebraError& e) {
    call.error = make_error(e.kind, e.what(), call.pos);
    return false;
  }
}

}  // namespace natives

// A verified, compiled program: Module::compile() is the only way to get
// one, and it verifies first, so vm::run's trust in verify()'s invariants
// (funcs is non-empty, every index in range) holds by construction here --
// unlike Module::run(), nothing between compiling and running could have
// skipped it. entry_frame_drops is a property of the compiled program
// (Module's flag at the moment compile() ran), not a per-run argument: a
// script that wants both call set_entry_frame_drops() before compile().
class Program {
 public:
  Program(vm::Program p, bool entry_frame_drops)
      : p_(std::move(p)), entry_frame_drops_(entry_frame_drops) {}

  // rt is nil (the default) -- a program's own throwaway heap, same as
  // Module::run() -- or a Runtime to run on, so a script can compile once
  // and run many times, and inspect the heap in between runs. A rt that
  // still holds another program's objects is refused: a ClosureObj's own
  // func field is an index into ITS Program's chunks (push_closure indexes
  // p.chunks[c->func] unchecked), so reusing a heap across two different
  // compiled programs without collecting first would read the wrong
  // program's chunk table the moment a leftover closure from the first one
  // is called by the GC's own drop-hook dispatch during the second's run.
  // `natives` maps each name the module declared to the culebra Function
  // that implements it -- nil or an empty Object where it declared none.
  // The table lives exactly as long as this call: vm::run drains the job
  // queue (enqueued closures and scheduled coroutines alike) before it
  // returns, so nothing can call a native afterwards.
  void run(Runtime* rt = nullptr,
           int64_t max_call_depth = vm::RunOptions{}.max_call_depth,
           JitValue natives_obj = {TAG_NIL, 0}) {
    std::vector<std::unique_ptr<natives::BoundNative>> bound;
    vm::RunOptions opts;
    opts.entry_frame_drops = entry_frame_drops_;
    opts.max_call_depth = static_cast<int>(max_call_depth);
    opts.natives = bind_natives(natives_obj, bound);
    if (!rt) {
      coreir::Runtime scratch;  // the program's own heap, gone at the return
      vm::run(p_, scratch, opts);
      return;
    }
    if (rt->bound_ != 0 && rt->bound_ != id_ && rt->rt_.live_objects() != 0) {
      throw culebra::CulebraError(
          "IrError", "runtime still holds objects from another program", 0, 0);
    }
    rt->bound_ = id_;
    vm::run(p_, rt->rt_, opts);
  }

  std::string dump_bc() const { return vm::to_string(p_); }

 private:
  // Resolves every name the module declared against the supplied table,
  // failing here rather than inside vm::run so the diagnostic can name the
  // one that is missing. A name in the table the module never declared is
  // not an error: one table can serve several programs.
  std::vector<vm::NativeDef> bind_natives(
      JitValue natives_obj,
      std::vector<std::unique_ptr<natives::BoundNative>>& bound) const {
    std::vector<vm::NativeDef> defs;
    if (p_.natives.empty()) return defs;
    JitObject* table = natives_obj.tag == TAG_OBJECT
                           ? reinterpret_cast<JitObject*>(natives_obj.data)
                           : nullptr;
    for (const std::string& name : p_.natives) {
      const size_t slot =
          table ? table->find_slot(name) : static_cast<size_t>(-1);
      if (slot == static_cast<size_t>(-1)) {
        throw culebra::CulebraError(
            "IrError",
            culebra::format("native '{}' is declared but not supplied", name),
            0, 0);
      }
      const JitValue fn = table->slots[slot].value;
      if (fn.tag != TAG_FUNC) {
        throw culebra::CulebraError(
            "TypeError",
            culebra::format("native '{}' must be a Function, got {}", name,
                            _culebra_tag_name(fn.tag)),
            0, 0);
      }
      auto* cls = reinterpret_cast<JitClosure*>(fn.data);
      if (cls->arity == JIT_VARIADIC_ARITY) {
        throw culebra::CulebraError(
            "TypeError",
            culebra::format("native '{}' must take a fixed number of "
                            "arguments",
                            name),
            0, 0);
      }
      bound.push_back(std::make_unique<natives::BoundNative>(
          natives::BoundNative{name, cls,
                               JitOwnedVal::from_borrowed(fn)}));
      // arity -1: no check in the executor. Its own would raise a
      // kind-less trap, where the shim's carries kind 'ArityError' like
      // every other answer this bridge gives.
      defs.push_back({name, -1, &natives::shim, bound.back().get()});
    }
    return defs;
  }

  static uint64_t next_id() {
    static std::atomic<uint64_t> n{1};
    return n++;
  }

  vm::Program p_;
  bool entry_frame_drops_;
  uint64_t id_ = next_id();
};

class Module {
 public:
  int64_t literal(int64_t v, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.literal(v, pos(line, col, at)));
  }

  int64_t bool_literal(bool v, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.bool_literal(v, pos(line, col, at)));
  }

  int64_t double_literal(double v, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.double_literal(v, pos(line, col, at)));
  }

  int64_t nil_literal(JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.nil_literal(pos(line, col, at)));
  }

  int64_t str_literal(std::string_view s, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.str_literal(std::string(s), pos(line, col, at)));
  }

  int64_t var_ref(std::string_view kind, int64_t index, JitObjectArg at, int64_t line,
                  int64_t col) {
    coreir::Builder b(m_);
    return id(b.varref(to_kind(kind), idx32(index), pos(line, col, at)));
  }

  int64_t unary(std::string_view op, int64_t operand, JitObjectArg at, int64_t line,
               int64_t col) {
    coreir::Builder b(m_);
    return id(b.unary(to_unop(op), node(operand), pos(line, col, at)));
  }

  int64_t binary(std::string_view op, int64_t lhs, int64_t rhs, JitObjectArg at, int64_t line,
                int64_t col) {
    coreir::Builder b(m_);
    return id(b.binary(to_binop(op), node(lhs), node(rhs), pos(line, col, at)));
  }

  int64_t assign(std::string_view kind, int64_t index, int64_t value,
                JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.assign(to_kind(kind), idx32(index), node(value),
                       pos(line, col, at)));
  }

  int64_t make_if(int64_t cond, int64_t then_branch, JitObjectArg at, int64_t line,
                  int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_if(node(cond), node(then_branch), coreir::NodeId{},
                        pos(line, col, at)));
  }

  int64_t make_if_else(int64_t cond, int64_t then_branch, int64_t else_branch,
                       JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_if(node(cond), node(then_branch), node(else_branch),
                        pos(line, col, at)));
  }

  // arms holds key, body, key, body, ... -- the flat shape object_lit's
  // kv already uses, re-paired the same way. Each key must be a
  // literal node (int or str, all one ConstKind, pairwise distinct --
  // verify() enforces both); Break passes through a switch the same way it
  // passes through an if, so a switch-scoped break is a front end's own
  // lowering, not something this builds in.
  int64_t make_switch(int64_t subject, JitListArg arms, JitObjectArg at, int64_t line,
                      int64_t col) {
    return make_switch_impl(subject, arms, coreir::NodeId{}, at, line, col);
  }
  int64_t make_switch_default(int64_t subject, JitListArg arms,
                              int64_t default_body, JitObjectArg at, int64_t line,
                              int64_t col) {
    return make_switch_impl(subject, arms, node(default_body), at, line,
                            col);
  }

  int64_t make_while(int64_t cond, int64_t body, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_while(node(cond), node(body), pos(line, col, at)));
  }

  int64_t block(JitListArg stmts, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.block(take_list(stmts), pos(line, col, at)));
  }

  // Builder-side sugar over MakeClosure+CallValue (the one calling mechanism
  // Core-IR has -- see vmlib.h's Tag::MakeClosure comment for why a
  // separate "call this function by index" tag was removed rather than kept
  // alongside it). PL/0 procedures take no arguments, so the immediately-built
  // closure is called with an empty argument list; a front end wanting real
  // first-class functions calls make_closure/call_value below instead.
  int64_t call(int64_t func, int64_t cmap, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    const coreir::SrcPos p = pos(line, col, at);
    const coreir::NodeId closure = b.make_closure(idx32(func), idx32(cmap), p);
    return id(b.call_value(closure, {}, p));
  }

  // The two primitives themselves, for a front end whose functions are
  // values: a closure built here can be stored in a variable, passed as an
  // argument, or returned, and called later wherever it ends up.
  int64_t make_closure(int64_t func, int64_t cmap, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_closure(idx32(func), idx32(cmap), pos(line, col, at)));
  }
  int64_t call_value(int64_t callee, JitListArg args, JitObjectArg at, int64_t line,
                     int64_t col) {
    coreir::Builder b(m_);
    return id(b.call_value(node(callee), take_list(args), pos(line, col, at)));
  }

  int64_t array_lit(JitListArg items, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.array_lit(take_list(items), pos(line, col, at)));
  }

  // kv holds key, value, key, value, ... -- the flat shape ObjectLit
  // itself stores; the builder wants pairs, so this re-pairs them.
  int64_t object_lit(JitListArg kv, JitObjectArg at, int64_t line, int64_t col) {
    const std::vector<coreir::NodeId> flat = take_list(kv);
    std::vector<std::pair<coreir::NodeId, coreir::NodeId>> kvs;
    kvs.reserve(flat.size() / 2);
    for (size_t i = 0; i + 1 < flat.size(); i += 2) {
      kvs.push_back({flat[i], flat[i + 1]});
    }
    coreir::Builder b(m_);
    return id(b.object_lit(kvs, pos(line, col, at)));
  }

  int64_t index(int64_t recv, int64_t key, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.index(node(recv), node(key), pos(line, col, at)));
  }

  int64_t set_index(int64_t recv, int64_t key, int64_t value, JitObjectArg at, int64_t line,
                    int64_t col) {
    coreir::Builder b(m_);
    return id(b.set_index(node(recv), node(key), node(value),
                          pos(line, col, at)));
  }

  // A struct field at a slot the front end assigns, read/written in O(1)
  // rather than index/set_index's key comparison -- `name` is carried only
  // for a trap message ("field 'next' of ..."), never read to execute
  // anything. `recv` must already be built through object_lit with every
  // field present (props in key order); set_index's own key comparison
  // never has that requirement, which is the whole difference in cost.
  int64_t field_get(int64_t recv, int64_t slot, std::string_view name,
                    JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.field_get(node(recv), idx32(slot), std::string(name),
                          pos(line, col, at)));
  }
  int64_t field_set(int64_t recv, int64_t slot, std::string_view name,
                    int64_t value, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.field_set(node(recv), idx32(slot), std::string(name),
                          node(value), pos(line, col, at)));
  }

  // Scopes, non-local exits, exceptions, defers -- the Core-IR surface the
  // exception phase added; each is a thin forward to the builder.
  int64_t scope(int64_t first_local, int64_t end_local, int64_t body,
                JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.scope(idx32(first_local), idx32(end_local), node(body),
                      pos(line, col, at)));
  }
  // The same scope with its release order spelled out: `release` holds
  // var_ref nodes (local or cell), released in that order at every exit --
  // a front end lists reverse declaration order, captured slots included.
  int64_t scope_release(int64_t first_local, int64_t end_local, int64_t body,
                        JitListArg release, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.scope(idx32(first_local), idx32(end_local), node(body),
                      take_list(release), pos(line, col, at)));
  }

  // A bare `return` is spelled with an explicit nil_literal argument; the
  // wrap layer has no optional parameters, and the front end lowering a
  // return statement holds a position for the nil anyway.
  int64_t make_return(int64_t value, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_return(node(value), pos(line, col, at)));
  }

  // `depth` is how many enclosing loops to skip -- 0, the default, leaves
  // the innermost. A front end that resolves its own labels answers with a
  // depth, so the IR needs no label table of its own.
  int64_t make_break(JitObjectArg at, int64_t line, int64_t col, int64_t depth) {
    coreir::Builder b(m_);
    return id(b.make_break(pos(line, col, at), idx32(depth)));
  }

  int64_t make_continue(JitObjectArg at, int64_t line, int64_t col, int64_t depth) {
    coreir::Builder b(m_);
    return id(b.make_continue(pos(line, col, at), idx32(depth)));
  }

  int64_t make_throw(int64_t value, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_throw(node(value), pos(line, col, at)));
  }

  int64_t make_try(int64_t caught_local, int64_t body, int64_t handler,
                   JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_try(idx32(caught_local), node(body), node(handler),
                         pos(line, col, at)));
  }

  int64_t make_defer(int64_t value, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_defer(node(value), pos(line, col, at)));
  }

  int64_t make_yield(int64_t value, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_yield(node(value), pos(line, col, at)));
  }
  int64_t cell_fresh(int64_t cell, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.cell_fresh(idx32(cell), pos(line, col, at)));
  }

  int64_t intrinsic(std::string_view name, JitListArg args, JitObjectArg at, int64_t line,
                    int64_t col) {
    coreir::Builder b(m_);
    return id(
        b.intrinsic(to_intrinsic(name), take_list(args), pos(line, col, at)));
  }

  // A host function the module calls but does not carry: declare_native
  // interns the name and answers its index, native_ref builds the value that
  // names it, and an ordinary call_value calls it. What the name resolves to
  // is supplied per run -- Program.run's `natives` -- never by the module,
  // which holds names alone.
  int64_t declare_native(std::string_view name) {
    coreir::Builder b(m_);
    return b.declare_native(std::string(name));
  }
  int64_t native_ref(int64_t index, JitObjectArg at, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.native_ref(idx32(index), pos(line, col, at)));
  }

  // Generic staging for a node's variadic children -- consumed and gone once
  // handed to block()/intrinsic() (or, packed as CaptureSrc, add_capture_map()
  // below). Reusing a list id afterward is undefined.
  int64_t list_new() {
    lists_.emplace_back();
    return static_cast<int64_t>(lists_.size() - 1);
  }
  void list_push(int64_t list, int64_t value) {
    lists_.at(static_cast<size_t>(list)).push_back(value);
  }

  int64_t capture_map_new() {
    cmaps_.emplace_back();
    return static_cast<int64_t>(cmaps_.size() - 1);
  }
  void capture_map_push(int64_t cmap, std::string_view kind, int64_t index) {
    cmaps_.at(static_cast<size_t>(cmap))
        .push_back({to_kind(kind), idx32(index)});
  }
  int64_t add_capture_map(int64_t cmap) {
    m_.capture_maps.push_back(std::move(cmaps_.at(static_cast<size_t>(cmap))));
    return static_cast<int64_t>(m_.capture_maps.size() - 1);
  }

  // Params are the first `num_params` locals (coreir::Func's own convention),
  // so num_params <= num_locals; a param a nested closure captures still
  // arrives in its local slot, and the front end copies it into a cell in the
  // body's prologue.
  int64_t add_func(std::string_view name, int64_t num_locals,
                   int64_t num_captures, int64_t num_cells, int64_t num_params,
                   int64_t body) {
    coreir::Func f;
    f.name = std::string(name);
    f.num_locals = idx32(num_locals);
    f.num_captures = idx32(num_captures);
    f.num_cells = idx32(num_cells);
    f.num_params = idx32(num_params);
    f.body = node(body);
    f.local_names.resize(static_cast<size_t>(f.num_locals));
    f.capture_names.resize(static_cast<size_t>(f.num_captures));
    m_.funcs.push_back(std::move(f));
    return static_cast<int64_t>(m_.funcs.size() - 1);
  }
  void set_local_name(int64_t func, int64_t index, std::string_view name) {
    m_.funcs.at(static_cast<size_t>(func))
        .local_names.at(static_cast<size_t>(index)) = name;
  }
  void set_capture_name(int64_t func, int64_t index, std::string_view name) {
    m_.funcs.at(static_cast<size_t>(func))
        .capture_names.at(static_cast<size_t>(index)) = name;
  }
  // Calling the function then packages a suspended activation instead of
  // running the body; drive it with the genresume/genreturn/genthrow
  // intrinsics. A setter rather than an add_func parameter so every existing
  // caller keeps its arity.
  void set_generator(int64_t func) {
    m_.funcs.at(static_cast<size_t>(func)).is_generator = true;
  }
  // Calls of this function tolerate any argument count (extras dropped,
  // missing params nil); the body reads the supplied count with the
  // argcount intrinsic.
  void set_lenient_arity(int64_t func) {
    m_.funcs.at(static_cast<size_t>(func)).lenient_arity = true;
  }
  // A call in tail position -- a Return's operand, or a body's final value
  // through Block/If/Switch/Scope -- replaces this function's frame instead
  // of stacking on it, so a loop written as a call chain runs in one frame
  // however long it goes. The frame exits BEFORE the callee runs, which is
  // the one thing this changes: a local's drop hook precedes the callee's
  // own output, where an ordinary call has it follow. A call inside a try
  // body, or crossing a scope that declares defers or its own release
  // order, stays an ordinary call -- each of those needs the frame.
  void set_tail_calls(int64_t func) {
    m_.funcs.at(static_cast<size_t>(func)).tail_calls = true;
  }
  // Every make_closure of this function yields the same closure object,
  // built at the first one to run and kept for the rest of the run. Opt-in
  // because the sharing is observable through `same`; a front end sets it
  // on a helper whose closure has no identity of its own. verify() refuses
  // it on a function with captures.
  void set_singleton(int64_t func) {
    m_.funcs.at(static_cast<size_t>(func)).singleton = true;
  }
  // Whether the entry frame's own bindings run their drop hooks when the
  // program ends (on by default). A front end whose top-level scope is
  // released without destructors, as culebra's is, turns it off.
  void set_entry_frame_drops(bool on) { entry_frame_drops_ = on; }

  // Throws CulebraError("IrError") on failure -- structural, so it carries
  // no useful source position (unlike a run() failure, which does).
  void verify() { verify_or_throw(); }

  // Throws CulebraError("IrError") with the failing operation's own position
  // -- see src/runtime/codegen_rt.cc, which implements cpp-vmlib's
  // coreir_rt_fail this way for every Module in this process.
  //
  // Verifies first, every time: vm::compile trusts verify()'s invariants
  // (funcs is non-empty, every index is in range) and does not re-check
  // them, so a script that calls run() on a Module it never verified would
  // otherwise reach undefined behavior in native code -- not a catchable
  // script-level error -- on something as simple as an empty Module. A
  // script that already called verify() itself just pays a second, cheap
  // structural walk. compile() carries the same guarantee for a script
  // that wants to run the same program more than once, or inspect the heap
  // between runs -- see Program above.
  Program compile() {
    verify_or_throw();
    return Program(vm::compile(m_), entry_frame_drops_);
  }

  // Sugar over compile(): a throwaway Runtime, run once. The single call
  // site every example front end and tests/test_codegen.cul still use.
  void run() { compile().run(); }

  std::string dump_ir() { return coreir::to_string(m_); }
  std::string dump_bc() { return compile().dump_bc(); }

  // --- Reading the IR back ---------------------------------------------
  //
  // Everything below reads what the builders above wrote, structurally --
  // a script's own constant folder, or a test asserting a node's shape
  // rather than substring-matching dump_ir(). Every accessor routes through
  // vmlib.h's own Views (view_scope, view_try, ...) rather than a node's raw
  // a/b fields, so a tag's layout still lives in exactly one place; a bad
  // node/func/cmap id or a tag/kind mismatch is CulebraError("IrError"), the
  // same failure class verify() itself reports.
  int64_t num_nodes() const { return static_cast<int64_t>(m_.nodes.size()); }
  std::string node_tag(int64_t node) const {
    return coreir::name_of(m_.at(checked_node(node)).tag);
  }
  int64_t node_line(int64_t node) const {
    return m_.pos_of(checked_node(node)).line;
  }
  int64_t node_col(int64_t node) const {
    return m_.pos_of(checked_node(node)).col;
  }
  int64_t num_children(int64_t node) const {
    return static_cast<int64_t>(m_.num_children(checked_node(node)));
  }
  int64_t child(int64_t node, int64_t index) const {
    const coreir::NodeId n = checked_node(node);
    checked_sub_index("node", node, m_.num_children(n), "children", index);
    return id(m_.child(n, static_cast<uint32_t>(index)));
  }

  // Literal: const_kind says which of the five kinds a node holds, and each
  // of the four decoders checks it first -- int_const on a str literal is a
  // caller mistake, not a silent reinterpretation of its bits. (Nil has no
  // payload to read, hence five kinds but four decoders.)
  std::string const_kind(int64_t node) const {
    const coreir::NodeId n = require_tag(node, coreir::Tag::Literal, "const_kind");
    return coreir::name_of(m_.const_kind(n));
  }
  int64_t int_const(int64_t node) const {
    const coreir::NodeId n = require_tag(node, coreir::Tag::Literal, "int_const");
    require_const_kind(n, coreir::ConstKind::Int, "int_const");
    return m_.int_const(n);
  }
  bool bool_const(int64_t node) const {
    const coreir::NodeId n = require_tag(node, coreir::Tag::Literal, "bool_const");
    require_const_kind(n, coreir::ConstKind::Bool, "bool_const");
    return m_.bool_const(n);
  }
  double double_const(int64_t node) const {
    const coreir::NodeId n =
        require_tag(node, coreir::Tag::Literal, "double_const");
    require_const_kind(n, coreir::ConstKind::Double, "double_const");
    return m_.double_const(n);
  }
  std::string str_const(int64_t node) const {
    const coreir::NodeId n = require_tag(node, coreir::Tag::Literal, "str_const");
    require_const_kind(n, coreir::ConstKind::Str, "str_const");
    return m_.str_const(n);
  }

  // node_op and var_kind read the same byte (Node::op), decoded per the
  // tag that owns it -- an operator for Unary/Binary/Intrinsic, a var kind
  // for VarRef/Assign. Two accessors, not one, because "add" and "local"
  // answer different questions; a varref calling node_op is redirected
  // rather than silently answering as if it were something else.
  std::string node_op(int64_t node) const {
    const coreir::NodeId n = checked_node(node);
    const coreir::Tag tag = m_.at(n).tag;
    switch (tag) {
      case coreir::Tag::Unary:
        return coreir::name_of(coreir::view_unary(m_, n).op);
      case coreir::Tag::Binary:
        return coreir::name_of(coreir::view_binary(m_, n).op);
      case coreir::Tag::Intrinsic:
        return coreir::name_of(coreir::view_intrinsic(m_, n).id);
      case coreir::Tag::VarRef:
      case coreir::Tag::Assign:
        throw culebra::CulebraError(
            "IrError",
            culebra::format("node #{} is {}; use var_kind() for its var kind",
                            node, describe(coreir::name_of(tag))),
            0, 0);
      default:
        throw culebra::CulebraError(
            "IrError",
            culebra::format("node #{} is {}, which has no operator", node,
                            describe(coreir::name_of(tag))),
            0, 0);
    }
  }
  std::string var_kind(int64_t node) const {
    return coreir::name_of(require_varref_or_assign(node).kind);
  }
  int64_t var_index(int64_t node) const {
    return require_varref_or_assign(node).index;
  }

  // Switch's shape is decided by arm count and parity, not a fixed child
  // position (the same reason cpp-vmlib's own compiler and verifier read it
  // through one View rather than each rederiving it) -- these give a script
  // the same one place, instead of it recomputing "rest odd means there is
  // a default" by hand.
  int64_t switch_subject(int64_t node) const {
    return id(coreir::view_switch(
                   m_, require_tag(node, coreir::Tag::Switch, "switch_subject"))
                  .subject);
  }
  int64_t switch_arm_count(int64_t node) const {
    return coreir::view_switch(
               m_, require_tag(node, coreir::Tag::Switch, "switch_arm_count"))
        .arm_count;
  }
  int64_t switch_key(int64_t node, int64_t index) const {
    const coreir::NodeId n =
        require_tag(node, coreir::Tag::Switch, "switch_key");
    const auto arm_count = coreir::view_switch(m_, n).arm_count;
    checked_sub_index("node", node, arm_count, "switch arms", index);
    return id(coreir::switch_key(m_, n, static_cast<uint32_t>(index)));
  }
  int64_t switch_body(int64_t node, int64_t index) const {
    const coreir::NodeId n =
        require_tag(node, coreir::Tag::Switch, "switch_body");
    const auto arm_count = coreir::view_switch(m_, n).arm_count;
    checked_sub_index("node", node, arm_count, "switch arms", index);
    return id(coreir::switch_body(m_, n, static_cast<uint32_t>(index)));
  }
  bool switch_has_default(int64_t node) const {
    return coreir::view_switch(
               m_, require_tag(node, coreir::Tag::Switch, "switch_has_default"))
        .default_body.valid();
  }
  int64_t switch_default_body(int64_t node) const {
    const auto v = coreir::view_switch(
        m_, require_tag(node, coreir::Tag::Switch, "switch_default_body"));
    if (!v.default_body.valid()) {
      throw culebra::CulebraError(
          "IrError",
          culebra::format("node #{} has no default arm", node), 0, 0);
    }
    return id(v.default_body);
  }

  // FieldGet/FieldSet share slot and name_const (FieldSet adds the value
  // child) -- one check for both, the same way require_varref_or_assign
  // covers VarRef/Assign.
  int64_t field_slot(int64_t node) const {
    return require_field(node, "field_slot").slot;
  }
  std::string field_name(int64_t node) const {
    return m_.str_const_at(require_field(node, "field_name").name_const);
  }
  int64_t field_receiver(int64_t node) const {
    return id(require_field(node, "field_receiver").receiver);
  }
  int64_t field_set_value(int64_t node) const {
    return id(coreir::view_field_set(
                   m_, require_tag(node, coreir::Tag::FieldSet,
                                   "field_set_value"))
                  .value);
  }

  // One tag each, via the same Views the Compiler and Dumper read.
  int64_t scope_first_local(int64_t node) const {
    return coreir::view_scope(
               m_, require_tag(node, coreir::Tag::Scope, "scope_first_local"))
        .first_local;
  }
  int64_t scope_end_local(int64_t node) const {
    return coreir::view_scope(
               m_, require_tag(node, coreir::Tag::Scope, "scope_end_local"))
        .end_local;
  }
  int64_t try_caught_local(int64_t node) const {
    return coreir::view_try(
               m_, require_tag(node, coreir::Tag::TryCatch, "try_caught_local"))
        .caught_local;
  }
  int64_t break_depth(int64_t node) const {
    return coreir::view_break(
               m_, require_tag(node, coreir::Tag::Break, "break_depth"))
        .depth;
  }
  int64_t continue_depth(int64_t node) const {
    return coreir::view_continue(
               m_, require_tag(node, coreir::Tag::Continue, "continue_depth"))
        .depth;
  }
  int64_t native_index(int64_t node) const {
    return coreir::view_native_ref(
               m_, require_tag(node, coreir::Tag::NativeRef, "native_index"))
        .index;
  }
  int64_t closure_func(int64_t node) const {
    return coreir::view_make_closure(
               m_, require_tag(node, coreir::Tag::MakeClosure, "closure_func"))
        .func;
  }
  int64_t closure_cmap(int64_t node) const {
    return coreir::view_make_closure(
               m_, require_tag(node, coreir::Tag::MakeClosure, "closure_cmap"))
        .capture_map;
  }
  int64_t cell_index(int64_t node) const {
    return coreir::view_cellfresh(
               m_, require_tag(node, coreir::Tag::CellFresh, "cell_index"))
        .cell;
  }

  // The function table: add_func's own arguments, read back one at a time.
  int64_t num_funcs() const { return static_cast<int64_t>(m_.funcs.size()); }
  std::string func_name(int64_t func) const {
    return m_.funcs[checked_func(func)].name;
  }
  int64_t func_num_locals(int64_t func) const {
    return m_.funcs[checked_func(func)].num_locals;
  }
  int64_t func_num_captures(int64_t func) const {
    return m_.funcs[checked_func(func)].num_captures;
  }
  int64_t func_num_cells(int64_t func) const {
    return m_.funcs[checked_func(func)].num_cells;
  }
  int64_t func_num_params(int64_t func) const {
    return m_.funcs[checked_func(func)].num_params;
  }
  int64_t func_body(int64_t func) const {
    return id(m_.funcs[checked_func(func)].body);
  }
  bool func_is_generator(int64_t func) const {
    return m_.funcs[checked_func(func)].is_generator;
  }
  bool func_lenient_arity(int64_t func) const {
    return m_.funcs[checked_func(func)].lenient_arity;
  }
  bool func_tail_calls(int64_t func) const {
    return m_.funcs[checked_func(func)].tail_calls;
  }
  bool func_singleton(int64_t func) const {
    return m_.funcs[checked_func(func)].singleton;
  }
  std::string func_local_name(int64_t func, int64_t index) const {
    const auto& f = m_.funcs[checked_func(func)];
    return checked_name(f.local_names, func, index, "locals");
  }
  std::string func_capture_name(int64_t func, int64_t index) const {
    const auto& f = m_.funcs[checked_func(func)];
    return checked_name(f.capture_names, func, index, "captures");
  }

  // Natives: declare_native's own argument, read back one at a time.
  int64_t num_natives() const {
    return static_cast<int64_t>(m_.natives.size());
  }
  std::string native_name(int64_t index) const {
    return m_.natives[checked_index(index, m_.natives.size(), "native")];
  }

  // Capture maps: capture_map_push's own arguments, read back one at a time.
  int64_t num_capture_maps() const {
    return static_cast<int64_t>(m_.capture_maps.size());
  }
  int64_t num_capture_entries(int64_t cmap) const {
    return static_cast<int64_t>(m_.capture_maps[checked_cmap(cmap)].size());
  }
  std::string capture_kind(int64_t cmap, int64_t index) const {
    return coreir::name_of(checked_capture_entry(cmap, index).from);
  }
  int64_t capture_index(int64_t cmap, int64_t index) const {
    return checked_capture_entry(cmap, index).index;
  }

 private:
  void verify_or_throw() {
    if (auto err = coreir::verify(m_)) {
      throw culebra::CulebraError("IrError", *err, 0, 0);
    }
  }

  static coreir::NodeId node(int64_t v) {
    return coreir::NodeId{static_cast<uint32_t>(v)};
  }
  static int64_t id(coreir::NodeId n) { return static_cast<int64_t>(n.v); }
  static int32_t idx32(int64_t v) { return static_cast<int32_t>(v); }
  static coreir::SrcPos pos(int64_t line, int64_t col) {
    return {static_cast<uint32_t>(line), static_cast<uint32_t>(col)};
  }

  // Where a node came from, given either way of saying it: `at`, any Object
  // with `line` and `column` (a parse node, or one the front end made up),
  // or the two numbers. `at` wins when both are there.
  //
  // It is a parameter rather than a builder that remembers a position,
  // because a position-bound builder would have to be an object per node and
  // allocating one costs about twice what the call it saves does.
  static coreir::SrcPos pos(int64_t line, int64_t col, JitObjectArg at) {
    if (at.v.tag != TAG_OBJECT) return pos(line, col);
    auto* o = reinterpret_cast<JitObject*>(at.v.data);
    return {static_cast<uint32_t>(field(o, "line")),
            static_cast<uint32_t>(field(o, "column"))};
  }

  static int64_t field(JitObject* o, const char* name) {
    const size_t slot = o->find_slot(name);
    if (slot == static_cast<size_t>(-1)) {
      throw culebra::CulebraError(
          "TypeError",
          culebra::format("'at' needs a '{}' -- an Object with 'line' and "
                          "'column', such as a parse node",
                          name),
          0, 0);
    }
    const JitValue v = o->slots[slot].value;
    if (v.tag != TAG_LONG) {
      throw culebra::CulebraError(
          "TypeError",
          culebra::format("'at.{}' must be a Long, got {}", name,
                          _culebra_tag_name(v.tag)),
          0, 0);
    }
    return v.data;
  }

  // The nodes of a variadic shape, from an Array of them or from the id of a
  // staging list built by list_new/list_push. The Array is the ordinary way;
  // the id is the older two-call form, kept because a module already built
  // that way still compiles.
  std::vector<coreir::NodeId> take_list(JitListArg list) const {
    std::vector<coreir::NodeId> out;
    if (list.v.tag == TAG_ARRAY) {
      auto* a = reinterpret_cast<JitArray*>(list.v.data);
      out.reserve(a->size);
      for (size_t i = 0; i < a->size; i++) {
        const JitValue e = a->items[i];
        if (e.tag != TAG_LONG) {
          throw culebra::CulebraError(
              "TypeError",
              culebra::format("element {} is {}, not a node id", i,
                              _culebra_tag_name(e.tag)),
              0, 0);
        }
        out.push_back(node(e.data));
      }
      return out;
    }
    const auto& raw = lists_.at(static_cast<size_t>(list.v.data));
    out.reserve(raw.size());
    for (int64_t v : raw) out.push_back(node(v));
    return out;
  }

  // Shared by make_switch/make_switch_default: re-pairs the arms the same
  // way object_lit re-pairs its keys and values, then builds with whichever default
  // (possibly invalid, meaning none) the caller already resolved.
  int64_t make_switch_impl(int64_t subject, JitListArg arms,
                           coreir::NodeId default_body, JitObjectArg at, int64_t line,
                           int64_t col) {
    const std::vector<coreir::NodeId> flat = take_list(arms);
    std::vector<std::pair<coreir::NodeId, coreir::NodeId>> pairs;
    pairs.reserve(flat.size() / 2);
    for (size_t i = 0; i + 1 < flat.size(); i += 2) {
      pairs.push_back({flat[i], flat[i + 1]});
    }
    coreir::Builder b(m_);
    return id(
        b.make_switch(node(subject), pairs, default_body, pos(line, col, at)));
  }

  // vmlib.h owns each enum's vocabulary (name_of/from_name); a second, hand-
  // typed copy here could drift from it the moment cpp-vmlib gains a member.
  // The four named wrappers stay because their call sites read better than
  // an explicit template argument would.
  template <class E>
  static E to_enum(std::string_view s, const char* what) {
    if (auto v = coreir::from_name<E>(s)) return *v;
    throw std::invalid_argument(
        culebra::format("CodeGen: unknown {} '{}'", what, s));
  }
  static coreir::VarKind to_kind(std::string_view s) {
    return to_enum<coreir::VarKind>(s, "var kind");
  }
  static coreir::UnOp to_unop(std::string_view s) {
    return to_enum<coreir::UnOp>(s, "unary op");
  }
  static coreir::BinOp to_binop(std::string_view s) {
    return to_enum<coreir::BinOp>(s, "binary op");
  }
  static coreir::IntrinsicId to_intrinsic(std::string_view s) {
    return to_enum<coreir::IntrinsicId>(s, "intrinsic");
  }

  // "a varref" / "an assign": several tag and kind names (Assign, If,
  // ArrayLit, ObjectLit, Index; ConstKind::Int) start with a vowel, so the
  // messages below pick their article rather than always saying "a".
  static std::string describe(const char* name) {
    switch (name[0]) {
      case 'a': case 'e': case 'i': case 'o': case 'u':
        return std::string("an ") + name;
      default:
        return std::string("a ") + name;
    }
  }

  // Bounds/kind checks for the read-out API: unlike node() above (always
  // called on an id this same Module just handed out), a reader's id is
  // whatever the caller passed in, so it is checked against the table's own
  // size before ever reaching coreir::Module::at, which does not check
  // itself.
  static size_t checked_index(int64_t v, size_t size, const char* what) {
    if (v < 0 || static_cast<size_t>(v) >= size) {
      throw culebra::CulebraError(
          "IrError", culebra::format("no {} #{}", what, v), 0, 0);
    }
    return static_cast<size_t>(v);
  }
  // The same for an index INTO one of those: how many the owner has is worth
  // saying, since a caller reading a node's children is usually walking them.
  static void checked_sub_index(const char* owner, int64_t owner_id,
                                size_t count, const char* what,
                                int64_t index) {
    if (index < 0 || static_cast<size_t>(index) >= count) {
      throw culebra::CulebraError(
          "IrError",
          culebra::format("{} #{} has {} {}, no #{}", owner, owner_id, count,
                          what, index),
          0, 0);
    }
  }
  coreir::NodeId checked_node(int64_t v) const {
    return coreir::NodeId{
        static_cast<uint32_t>(checked_index(v, m_.nodes.size(), "node"))};
  }
  size_t checked_func(int64_t v) const {
    return checked_index(v, m_.funcs.size(), "func");
  }
  size_t checked_cmap(int64_t v) const {
    return checked_index(v, m_.capture_maps.size(), "capture map");
  }

  // "node #3 is a binary, not a literal (int_const)" and its ConstKind twin:
  // one shape, since name_of is overloaded on both enums.
  template <class E>
  static void require_same(const char* what, int64_t id, E got, E want,
                           const char* accessor) {
    if (got != want) {
      throw culebra::CulebraError(
          "IrError",
          culebra::format("{} #{} is {}, not {} ({})", what, id,
                          describe(coreir::name_of(got)),
                          describe(coreir::name_of(want)), accessor),
          0, 0);
    }
  }
  coreir::NodeId require_tag(int64_t v, coreir::Tag want,
                             const char* accessor) const {
    const coreir::NodeId n = checked_node(v);
    require_same("node", v, m_.at(n).tag, want, accessor);
    return n;
  }
  void require_const_kind(coreir::NodeId n, coreir::ConstKind want,
                          const char* accessor) const {
    require_same("literal", n.v, m_.const_kind(n), want, accessor);
  }
  coreir::VarRefView require_varref_or_assign(int64_t v) const {
    const coreir::NodeId n = checked_node(v);
    const coreir::Tag tag = m_.at(n).tag;
    if (tag == coreir::Tag::VarRef) return coreir::view_varref(m_, n);
    if (tag == coreir::Tag::Assign) {
      const auto a = coreir::view_assign(m_, n);
      return {a.kind, a.index};
    }
    throw culebra::CulebraError(
        "IrError",
        culebra::format("node #{} is {}, not a varref or assign", v,
                        describe(coreir::name_of(tag))),
        0, 0);
  }
  coreir::FieldView require_field(int64_t v, const char* accessor) const {
    const coreir::NodeId n = checked_node(v);
    const coreir::Tag tag = m_.at(n).tag;
    if (tag == coreir::Tag::FieldGet) return coreir::view_field_get(m_, n);
    if (tag == coreir::Tag::FieldSet) {
      const auto s = coreir::view_field_set(m_, n);
      return {s.slot, s.name_const, s.receiver};
    }
    throw culebra::CulebraError(
        "IrError",
        culebra::format("node #{} is {}, not a fieldget or fieldset ({})", v,
                        describe(coreir::name_of(tag)), accessor),
        0, 0);
  }
  const std::string& checked_name(const std::vector<std::string>& names,
                                  int64_t func, int64_t index,
                                  const char* what) const {
    checked_sub_index("func", func, names.size(), what, index);
    return names[static_cast<size_t>(index)];
  }
  const coreir::CaptureSrc& checked_capture_entry(int64_t cmap,
                                                   int64_t index) const {
    const auto& entries = m_.capture_maps[checked_cmap(cmap)];
    checked_sub_index("capture map", cmap, entries.size(), "entries", index);
    return entries[static_cast<size_t>(index)];
  }

  friend class Resolver;

  coreir::Module m_;
  std::vector<std::vector<int64_t>> lists_;
  std::vector<std::vector<coreir::CaptureSrc>> cmaps_;
  bool entry_frame_drops_ = true;
};

// --- Closure conversion, script-visible ------------------------------------
//
// CodeGen.Resolver and CodeGen.FrameLayout are coreir::Resolver and
// coreir::FrameLayout, the two things a front end needs beside Module and
// which every front end that skipped them wrote itself. A variable is an id
// here, not a (function, slot) pair: `declare` hands one out, the front end
// keeps whatever else its language records beside it, and `use`/`resolve`
// record a read. What that buys is the propagation -- a name read two levels
// in is recorded free in *every* function between the reader and the owner,
// because a closure's capture map is written in the frame that builds it and
// a frame two levels down cannot name a cell it does not own.
//
// Nothing here decides policy. What a second declaration of a name in one
// block means (an error, a fresh binding, the same binding) is the
// language's, so `declared_here` answers whether there is one and the front
// end chooses. Where a closure gets built is the language's too, and the two
// answers need different work: nesting closes in one walk, which is what
// `use` does, while a language that builds a closure at every *reference* to
// a function -- PL/0's procedures, a static `fn` -- has to close its own free
// sets over the call graph. `free_at`/`add_free` are there for that, and
// `capture_map` refuses a frame that cannot supply what it is asked for
// rather than emitting a capture map of nothing.
//
// A lookup that finds nothing answers -1, not nil: every id here is a
// non-negative index, and the alternative is an Any-typed return on the
// half-dozen methods a bind pass calls most.
class Resolver {
 public:
  Resolver() = default;

  // --- functions -----------------------------------------------------------

  // Registers a function whose closure is built in `parent`'s frame -- the
  // function it is written in. -1 when closures are only ever built at
  // reference sites, in which case the front end closes its own free sets.
  int64_t new_fn(int64_t parent) {
    return rs_.new_fn(static_cast<int32_t>(parent));
  }
  int64_t num_fns() const { return static_cast<int64_t>(rs_.fns.size()); }
  int64_t parent_of(int64_t fn) const { return checked_fn(fn).parent; }

  // Where in Module::funcs this function's body will land. The front end
  // assigns it; capture_map reads it.
  void set_func_index(int64_t fn, int64_t index) {
    checked_fn(fn).index = static_cast<int32_t>(index);
  }
  int64_t func_index(int64_t fn) const { return checked_fn(fn).index; }

  // --- scopes --------------------------------------------------------------

  void push_scope() { rs_.push_scope(); }
  void pop_scope() {
    if (rs_.depth() == 0) {
      throw culebra::CulebraError("IrError", "pop_scope with no open scope", 0,
                                  0);
    }
    rs_.pop_scope();
  }
  int64_t depth() const { return static_cast<int64_t>(rs_.depth()); }

  // A new binding in the innermost scope, owned by `owner`. Overwrites a name
  // the scope already had, so a language that shadows within a block gets
  // that by default and one that refuses it checks declared_here first.
  int64_t declare(const std::string& name, int64_t owner) {
    checked_fn(owner);
    checked_depth();
    return rs_.declare(name, static_cast<int32_t>(owner));
  }
  // The same, into a scope that is not the innermost -- what Python's
  // `global` needs, and a hoist into a function's own outermost scope.
  int64_t declare_in(int64_t scope, const std::string& name, int64_t owner) {
    checked_fn(owner);
    return rs_.declare_in(checked_scope(scope), name,
                          static_cast<int32_t>(owner));
  }
  // A second name for a binding that already exists. Not a declaration:
  // Ruby's `&blk` and a constructor re-entering its own parameters are both
  // naming something already declared, so neither joins the order table.
  void alias(const std::string& name, int64_t v) {
    checked_var(v);
    checked_depth();
    rs_.alias(name, static_cast<int32_t>(v));
  }

  int64_t declared_here(const std::string& name) const {
    checked_depth();
    return opt(rs_.declared_here(name));
  }
  int64_t declared_at(int64_t scope, const std::string& name) const {
    return opt(rs_.declared_at(checked_scope(scope), name));
  }
  // Everything the scope declared, in order, one entry per declare() --
  // including a name declared twice, which the name map cannot answer.
  int64_t declared_count(int64_t scope) const {
    return static_cast<int64_t>(rs_.declared_order(checked_scope(scope)).size());
  }
  int64_t declared_index(int64_t scope, int64_t i) const {
    const auto& order = rs_.declared_order(checked_scope(scope));
    if (i < 0 || static_cast<size_t>(i) >= order.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("scope {} declared {} bindings, no #{}", scope,
                          order.size(), i),
          0, 0);
    }
    return order[static_cast<size_t>(i)];
  }

  // The innermost scope that binds `name`, searching outward from the
  // innermost, or from `from_scope`.
  int64_t lookup(const std::string& name) const {
    return opt(rs_.lookup(name));
  }
  int64_t lookup_from(const std::string& name, int64_t from_scope) const {
    return opt(rs_.lookup(name, checked_scope(from_scope)));
  }

  // --- variables -----------------------------------------------------------

  int64_t num_vars() const { return static_cast<int64_t>(rs_.vars.size()); }
  std::string var_name(int64_t v) const { return checked_var(v).name; }
  int64_t var_owner(int64_t v) const { return checked_var(v).owner; }
  // The owner's local index. The front end assigns it (see FrameLayout);
  // access() reports it for a variable that stayed a plain local.
  int64_t var_slot(int64_t v) const { return checked_var(v).slot; }
  void set_var_slot(int64_t v, int64_t slot) {
    checked_var(v).slot = static_cast<int32_t>(slot);
  }

  // Records that `fn` reads `v`, marking it free in every function between
  // the two. The whole propagation, in one call per read.
  void use(int64_t v, int64_t fn) {
    checked_var(v);
    checked_fn(fn);
    rs_.use(static_cast<int32_t>(v), static_cast<int32_t>(fn));
  }
  // lookup + use in one: the shape a bind pass reads a name with.
  int64_t resolve(const std::string& name, int64_t fn) {
    checked_fn(fn);
    return opt(rs_.resolve(name, static_cast<int32_t>(fn)));
  }
  // A binding that must be a cell whether or not anything was seen capturing
  // it -- one a closure built by hand reaches. Applied by number_captures.
  void force_cell(int64_t v) {
    checked_var(v);
    rs_.force_cell(static_cast<int32_t>(v));
  }

  // --- free sets, for a front end that closes its own ----------------------

  // What `fn` needs from outside itself, after every `use`. A language whose
  // closures are built at reference sites reads these, lifts them along its
  // own call graph with add_free, and iterates to a fixpoint before calling
  // number_captures -- which is what PL/0 does and what nesting makes
  // unnecessary everywhere else.
  int64_t free_count(int64_t fn) const {
    return static_cast<int64_t>(checked_fn(fn).free.size());
  }
  int64_t free_at(int64_t fn, int64_t i) const {
    const auto& free = checked_fn(fn).free;
    if (i < 0 || static_cast<size_t>(i) >= free.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("func {} has {} free variables, no #{}", fn,
                          free.size(), i),
          0, 0);
    }
    auto it = free.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(i));
    return *it;
  }
  // Adds `v` to fn's free set, answering whether it was not already there.
  // Refuses a variable fn owns: that is a local, not a capture.
  bool add_free(int64_t fn, int64_t v) {
    const auto& var = checked_var(v);
    checked_fn(fn);
    if (var.owner == static_cast<int32_t>(fn)) return false;
    return rs_.fns[static_cast<size_t>(fn)]
        .free.insert(static_cast<int32_t>(v))
        .second;
  }

  // --- numbering and capture maps ------------------------------------------

  // Assigns every function's capture indices and every owner's cell indices.
  // Call once, after the free sets are closed and before any capture_map.
  //
  // Deliberately not coreir's Module overload: that one writes the counts
  // and names straight into Module::funcs, which needs the table already
  // sized, and CodeGen.Module only ever appends a finished func. So the
  // numbering stays here and the front end passes num_captures / num_cells
  // to add_func and the names to set_capture_name, which is the order it
  // builds in anyway -- a body cannot be emitted until the capture indices
  // it reads through exist.
  void number_captures() { rs_.number_captures(); }

  // The name at capture index `i` of `fn`, for set_capture_name.
  std::string capture_name(int64_t fn, int64_t i) const {
    const auto& free = checked_fn(fn).free;
    if (i < 0 || static_cast<size_t>(i) >= free.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("func {} has {} captures, no #{}", fn, free.size(),
                          i),
          0, 0);
    }
    return rs_.capture_name(static_cast<int32_t>(fn), static_cast<int32_t>(i));
  }

  // The forwarding table for a closure of `target` built in `builder`'s
  // frame. Throws when `builder` cannot supply what `target` captures --
  // build the closure in the frame its function is written in, or record the
  // captures there first.
  int64_t capture_map(Module& m, int64_t builder, int64_t target) {
    checked_fn(builder);
    checked_fn(target);
    return rs_.capture_map(m.m_, static_cast<int32_t>(builder),
                           static_cast<int32_t>(target));
  }
  bool reaches(int64_t fn, int64_t v) const {
    checked_fn(fn);
    checked_var(v);
    return rs_.reaches(static_cast<int32_t>(fn), static_cast<int32_t>(v));
  }

  // How `fn` reaches `v`: 'local' (its own slot), 'cell' (its own slot, made
  // a cell because something nested captures it) or 'capture' (an index into
  // its own capture list). The pair is what CodeGen.Module.var_ref wants.
  std::string access_kind(int64_t fn, int64_t v) const {
    return kind_name(access(fn, v).first);
  }
  int64_t access_index(int64_t fn, int64_t v) const {
    return access(fn, v).second;
  }

  int64_t num_captures(int64_t fn) const {
    return static_cast<int64_t>(checked_fn(fn).free.size());
  }
  int64_t num_cells(int64_t fn) const {
    checked_fn(fn);
    return rs_.num_cells(static_cast<int32_t>(fn));
  }
  // Which of `fn`'s cells holds `v`, or -1 when `v` did not become one.
  int64_t cell_of(int64_t fn, int64_t v) const {
    checked_fn(fn);
    checked_var(v);
    const auto& idx = rs_.fns[static_cast<size_t>(fn)].cell_index;
    const auto it = idx.find(static_cast<int32_t>(v));
    return it == idx.end() ? -1 : it->second;
  }

 private:
  std::pair<coreir::VarKind, int32_t> access(int64_t fn, int64_t v) const {
    checked_fn(fn);
    checked_var(v);
    if (!rs_.reaches(static_cast<int32_t>(fn), static_cast<int32_t>(v))) {
      throw culebra::CulebraError(
          "IrError",
          culebra::format(
              "func {} cannot name '{}' -- it neither owns it nor captures it",
              fn, rs_.vars[static_cast<size_t>(v)].name),
          0, 0);
    }
    return rs_.access(static_cast<int32_t>(fn), static_cast<int32_t>(v));
  }

  static std::string kind_name(coreir::VarKind k) {
    switch (k) {
      case coreir::VarKind::Local:
        return "local";
      case coreir::VarKind::Cell:
        return "cell";
      case coreir::VarKind::Capture:
        return "capture";
    }
    return "local";
  }

  static int64_t opt(const std::optional<int32_t>& v) {
    return v ? *v : -1;
  }

  void checked_depth() const {
    if (rs_.depth() == 0) {
      throw culebra::CulebraError("IrError", "no scope is open", 0, 0);
    }
  }
  size_t checked_scope(int64_t scope) const {
    if (scope < 0 || static_cast<size_t>(scope) >= rs_.depth()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("{} scopes are open, no #{}", rs_.depth(), scope), 0,
          0);
    }
    return static_cast<size_t>(scope);
  }
  const coreir::Resolver::Fn& checked_fn(int64_t fn) const {
    if (fn < 0 || static_cast<size_t>(fn) >= rs_.fns.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("no func #{} ({} registered)", fn, rs_.fns.size()), 0,
          0);
    }
    return rs_.fns[static_cast<size_t>(fn)];
  }
  coreir::Resolver::Fn& checked_fn(int64_t fn) {
    return const_cast<coreir::Resolver::Fn&>(
        static_cast<const Resolver*>(this)->checked_fn(fn));
  }
  const coreir::Resolver::Var& checked_var(int64_t v) const {
    if (v < 0 || static_cast<size_t>(v) >= rs_.vars.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("no variable #{} ({} declared)", v, rs_.vars.size()),
          0, 0);
    }
    return rs_.vars[static_cast<size_t>(v)];
  }
  coreir::Resolver::Var& checked_var(int64_t v) {
    return const_cast<coreir::Resolver::Var&>(
        static_cast<const Resolver*>(this)->checked_var(v));
  }

  coreir::Resolver rs_;
};

// The local slots of one function: hand them out in order, and give a block's
// back at its end so a sibling block reuses them. `release` answers the end
// of the range the block claimed, which is what CodeGen.Module.scope wants.
class FrameLayout {
 public:
  FrameLayout() = default;

  int64_t alloc_local(const std::string& name) {
    return fl_.alloc_local(name);
  }
  int64_t mark() const { return fl_.mark(); }
  int64_t release(int64_t mark) {
    if (mark < 0 || mark > fl_.next_local) {
      throw culebra::CulebraError(
          "IrError",
          culebra::format("release({}) outside the {} slots in hand", mark,
                          fl_.next_local),
          0, 0);
    }
    return fl_.release(static_cast<int32_t>(mark));
  }
  // The most slots ever in hand at once -- what Func::num_locals wants.
  int64_t num_locals() const { return fl_.high_local; }
  // The name table, exactly num_locals long: what set_local_name wants, in
  // slot order, empty where a slot was never named.
  std::string local_name(int64_t slot) const {
    const auto names = fl_.names();
    if (slot < 0 || static_cast<size_t>(slot) >= names.size()) {
      throw culebra::CulebraError(
          "IndexError",
          culebra::format("{} locals, no slot #{}", names.size(), slot), 0, 0);
    }
    return names[static_cast<size_t>(slot)];
  }

 private:
  coreir::FrameLayout fl_;
};

}  // namespace culebra::codegen
