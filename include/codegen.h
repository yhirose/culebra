#pragma once
// CodeGen.Module -- a script-visible builder over cpp-vmlib's Core-IR and its
// register-bytecode compiler/executor (vendor/cpp-vmlib). Wrapped for scripts
// via wrap.h in codegen_binding.h.
//
// IR nodes are plain int64_t (coreir::NodeId::v), not handles: wrap.h cannot
// marshal a handle or an array as a method argument (see wrap.h's own
// jit_arg_get), so every node a script holds onto has to be a scalar, and
// this Module is the only object wrap.h needs to wrap at all. Variadic
// shapes (a Block's statements, a Call's arguments, a capture map's entries)
// go through a small generic list-staging mechanism for the same reason --
// see list_new/list_push and capture_map_new/capture_map_push below.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coreir/ir.h"
#include "coreir/semantics.h"
#include "vm/bytecode.h"
#include "vm/compiler.h"
#include "vm/exec.h"

#include "shared.h"  // culebra::CulebraError

namespace culebra::codegen {

class Module {
 public:
  int64_t literal(int64_t v, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.literal(v, pos(line, col)));
  }

  int64_t var_ref(std::string_view kind, int64_t index, int64_t line,
                  int64_t col) {
    coreir::Builder b(m_);
    return id(b.varref(to_kind(kind), idx32(index), pos(line, col)));
  }

  int64_t unary(std::string_view op, int64_t operand, int64_t line,
               int64_t col) {
    coreir::Builder b(m_);
    return id(b.unary(to_unop(op), node(operand), pos(line, col)));
  }

  int64_t binary(std::string_view op, int64_t lhs, int64_t rhs, int64_t line,
                int64_t col) {
    coreir::Builder b(m_);
    return id(b.binary(to_binop(op), node(lhs), node(rhs), pos(line, col)));
  }

  int64_t assign(std::string_view kind, int64_t index, int64_t value,
                int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.assign(to_kind(kind), idx32(index), node(value),
                       pos(line, col)));
  }

  int64_t make_if(int64_t cond, int64_t then_branch, int64_t line,
                  int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_if(node(cond), node(then_branch), coreir::NodeId{},
                        pos(line, col)));
  }

  int64_t make_if_else(int64_t cond, int64_t then_branch, int64_t else_branch,
                       int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_if(node(cond), node(then_branch), node(else_branch),
                        pos(line, col)));
  }

  int64_t make_while(int64_t cond, int64_t body, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_while(node(cond), node(body), pos(line, col)));
  }

  int64_t block(int64_t stmts_list, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.block(take_list(stmts_list), pos(line, col)));
  }

  // Builder-side sugar over MakeClosure+CallValue (the one calling mechanism
  // Core-IR has -- see coreir/ir.h's Tag::MakeClosure comment for why a
  // separate "call this function by index" tag was removed rather than kept
  // alongside it). PL/0 procedures take no arguments, so the immediately-built
  // closure is called with an empty argument list; a front end wanting real
  // first-class functions would call make_closure/call_value directly instead
  // of this method.
  int64_t call(int64_t func, int64_t cmap, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    const coreir::SrcPos p = pos(line, col);
    const coreir::NodeId closure = b.make_closure(idx32(func), idx32(cmap), p);
    return id(b.call_value(closure, {}, p));
  }

  int64_t intrinsic(std::string_view name, int64_t args_list, int64_t line,
                    int64_t col) {
    coreir::Builder b(m_);
    return id(
        b.intrinsic(to_intrinsic(name), take_list(args_list), pos(line, col)));
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

  int64_t add_func(std::string_view name, int64_t num_locals,
                   int64_t num_captures, int64_t num_cells, int64_t body) {
    coreir::Func f;
    f.name = std::string(name);
    f.num_locals = idx32(num_locals);
    f.num_captures = idx32(num_captures);
    f.num_cells = idx32(num_cells);
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
  // structural walk.
  void run() {
    verify_or_throw();
    vm::run(vm::compile(m_));
  }

  std::string dump_ir() { return coreir::to_string(m_); }
  std::string dump_bc() {
    verify_or_throw();
    return vm::to_string(vm::compile(m_));
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

  std::vector<coreir::NodeId> take_list(int64_t list) const {
    const auto& raw = lists_.at(static_cast<size_t>(list));
    std::vector<coreir::NodeId> out;
    out.reserve(raw.size());
    for (int64_t v : raw) out.push_back(node(v));
    return out;
  }

  static coreir::VarKind to_kind(std::string_view s) {
    if (s == "local") return coreir::VarKind::Local;
    if (s == "capture") return coreir::VarKind::Capture;
    if (s == "cell") return coreir::VarKind::Cell;
    throw std::invalid_argument("CodeGen: unknown var kind '" +
                                std::string(s) + "'");
  }
  static coreir::UnOp to_unop(std::string_view s) {
    if (s == "neg") return coreir::UnOp::Neg;
    throw std::invalid_argument("CodeGen: unknown unary op '" +
                                std::string(s) + "'");
  }
  static coreir::BinOp to_binop(std::string_view s) {
    static constexpr std::pair<std::string_view, coreir::BinOp> kOps[] = {
        {"add", coreir::BinOp::Add}, {"sub", coreir::BinOp::Sub},
        {"mul", coreir::BinOp::Mul}, {"div", coreir::BinOp::Div},
        {"mod", coreir::BinOp::Mod}, {"eq", coreir::BinOp::Eq},
        {"ne", coreir::BinOp::Ne},   {"lt", coreir::BinOp::Lt},
        {"le", coreir::BinOp::Le},   {"gt", coreir::BinOp::Gt},
        {"ge", coreir::BinOp::Ge},
    };
    for (const auto& [name, op] : kOps) {
      if (s == name) return op;
    }
    throw std::invalid_argument("CodeGen: unknown binary op '" +
                                std::string(s) + "'");
  }
  static coreir::IntrinsicId to_intrinsic(std::string_view s) {
    if (s == "print") return coreir::IntrinsicId::Print;
    if (s == "readint") return coreir::IntrinsicId::ReadInt;
    throw std::invalid_argument("CodeGen: unknown intrinsic '" +
                                std::string(s) + "'");
  }

  coreir::Module m_;
  std::vector<std::vector<int64_t>> lists_;
  std::vector<std::vector<coreir::CaptureSrc>> cmaps_;
};

}  // namespace culebra::codegen
