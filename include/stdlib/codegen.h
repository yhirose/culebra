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

#include "vmlib.h"

#include "base/shared.h"  // culebra::CulebraError

namespace culebra::codegen {

class Module {
 public:
  int64_t literal(int64_t v, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.literal(v, pos(line, col)));
  }

  int64_t bool_literal(bool v, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.bool_literal(v, pos(line, col)));
  }

  int64_t double_literal(double v, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.double_literal(v, pos(line, col)));
  }

  int64_t nil_literal(int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.nil_literal(pos(line, col)));
  }

  int64_t str_literal(std::string_view s, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.str_literal(std::string(s), pos(line, col)));
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
  // Core-IR has -- see vmlib.h's Tag::MakeClosure comment for why a
  // separate "call this function by index" tag was removed rather than kept
  // alongside it). PL/0 procedures take no arguments, so the immediately-built
  // closure is called with an empty argument list; a front end wanting real
  // first-class functions calls make_closure/call_value below instead.
  int64_t call(int64_t func, int64_t cmap, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    const coreir::SrcPos p = pos(line, col);
    const coreir::NodeId closure = b.make_closure(idx32(func), idx32(cmap), p);
    return id(b.call_value(closure, {}, p));
  }

  // The two primitives themselves, for a front end whose functions are
  // values: a closure built here can be stored in a variable, passed as an
  // argument, or returned, and called later wherever it ends up.
  int64_t make_closure(int64_t func, int64_t cmap, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_closure(idx32(func), idx32(cmap), pos(line, col)));
  }
  int64_t call_value(int64_t callee, int64_t args_list, int64_t line,
                     int64_t col) {
    coreir::Builder b(m_);
    return id(b.call_value(node(callee), take_list(args_list), pos(line, col)));
  }

  int64_t array_lit(int64_t items_list, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.array_lit(take_list(items_list), pos(line, col)));
  }

  // kv_list holds key, value, key, value, ... -- the flat shape ObjectLit
  // itself stores; the builder wants pairs, so this re-pairs them.
  int64_t object_lit(int64_t kv_list, int64_t line, int64_t col) {
    const std::vector<coreir::NodeId> flat = take_list(kv_list);
    std::vector<std::pair<coreir::NodeId, coreir::NodeId>> kvs;
    kvs.reserve(flat.size() / 2);
    for (size_t i = 0; i + 1 < flat.size(); i += 2) {
      kvs.push_back({flat[i], flat[i + 1]});
    }
    coreir::Builder b(m_);
    return id(b.object_lit(kvs, pos(line, col)));
  }

  int64_t index(int64_t recv, int64_t key, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.index(node(recv), node(key), pos(line, col)));
  }

  int64_t set_index(int64_t recv, int64_t key, int64_t value, int64_t line,
                    int64_t col) {
    coreir::Builder b(m_);
    return id(b.set_index(node(recv), node(key), node(value),
                          pos(line, col)));
  }

  // Scopes, non-local exits, exceptions, defers -- the Core-IR surface the
  // exception phase added; each is a thin forward to the builder.
  int64_t scope(int64_t first_local, int64_t end_local, int64_t body,
                int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.scope(idx32(first_local), idx32(end_local), node(body),
                      pos(line, col)));
  }
  // The same scope with its release order spelled out: `release_list` holds
  // var_ref nodes (local or cell), released in that order at every exit --
  // a front end lists reverse declaration order, captured slots included.
  int64_t scope_release(int64_t first_local, int64_t end_local, int64_t body,
                        int64_t release_list, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.scope(idx32(first_local), idx32(end_local), node(body),
                      take_list(release_list), pos(line, col)));
  }

  // A bare `return` is spelled with an explicit nil_literal argument; the
  // wrap layer has no optional parameters, and the front end lowering a
  // return statement holds a position for the nil anyway.
  int64_t make_return(int64_t value, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_return(node(value), pos(line, col)));
  }

  int64_t make_break(int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_break(pos(line, col)));
  }

  int64_t make_continue(int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_continue(pos(line, col)));
  }

  int64_t make_throw(int64_t value, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_throw(node(value), pos(line, col)));
  }

  int64_t make_try(int64_t caught_local, int64_t body, int64_t handler,
                   int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_try(idx32(caught_local), node(body), node(handler),
                         pos(line, col)));
  }

  int64_t make_defer(int64_t value, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_defer(node(value), pos(line, col)));
  }

  int64_t make_yield(int64_t value, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.make_yield(node(value), pos(line, col)));
  }
  int64_t cell_fresh(int64_t cell, int64_t line, int64_t col) {
    coreir::Builder b(m_);
    return id(b.cell_fresh(idx32(cell), pos(line, col)));
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
  // structural walk.
  void run() {
    verify_or_throw();
    coreir::Runtime rt;
    vm::RunOptions opts;
    opts.entry_frame_drops = entry_frame_drops_;
    vm::run(vm::compile(m_), rt, opts);
  }

  std::string dump_ir() { return coreir::to_string(m_); }
  std::string dump_bc() {
    verify_or_throw();
    return vm::to_string(vm::compile(m_));
  }

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
    const int64_t n_children = static_cast<int64_t>(m_.num_children(n));
    if (index < 0 || index >= n_children) {
      throw culebra::CulebraError(
          "IrError", "node #" + std::to_string(node) + " has " +
                        std::to_string(n_children) + " children, no #" +
                        std::to_string(index),
          0, 0);
    }
    return id(m_.child(n, static_cast<uint32_t>(index)));
  }

  // Literal: const_kind says which of the four payloads a node holds, and
  // each of the other four checks it before decoding -- int_const on a str
  // literal is a caller mistake, not a silent reinterpretation of its bits.
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
            "IrError", "node #" + std::to_string(node) + " is " +
                          describe(coreir::name_of(tag)) +
                          "; its op is a var kind -- use var_kind()",
            0, 0);
      default:
        throw culebra::CulebraError(
            "IrError", "node #" + std::to_string(node) + " is " +
                          describe(coreir::name_of(tag)) +
                          ", which has no operator",
            0, 0);
    }
  }
  std::string var_kind(int64_t node) const {
    return coreir::name_of(require_varref_or_assign(node).kind);
  }
  int64_t var_index(int64_t node) const {
    return require_varref_or_assign(node).index;
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
  std::string func_local_name(int64_t func, int64_t index) const {
    const auto& f = m_.funcs[checked_func(func)];
    return checked_name(f.local_names, func, index, "local");
  }
  std::string func_capture_name(int64_t func, int64_t index) const {
    const auto& f = m_.funcs[checked_func(func)];
    return checked_name(f.capture_names, func, index, "capture");
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

  std::vector<coreir::NodeId> take_list(int64_t list) const {
    const auto& raw = lists_.at(static_cast<size_t>(list));
    std::vector<coreir::NodeId> out;
    out.reserve(raw.size());
    for (int64_t v : raw) out.push_back(node(v));
    return out;
  }

  // vmlib.h owns each enum's vocabulary (name_of/from_name); a second, hand-
  // typed copy here could drift from it the moment cpp-vmlib gains a member.
  static coreir::VarKind to_kind(std::string_view s) {
    if (auto k = coreir::from_name<coreir::VarKind>(s)) return *k;
    throw std::invalid_argument("CodeGen: unknown var kind '" +
                                std::string(s) + "'");
  }
  static coreir::UnOp to_unop(std::string_view s) {
    if (auto op = coreir::from_name<coreir::UnOp>(s)) return *op;
    throw std::invalid_argument("CodeGen: unknown unary op '" +
                                std::string(s) + "'");
  }
  static coreir::BinOp to_binop(std::string_view s) {
    if (auto op = coreir::from_name<coreir::BinOp>(s)) return *op;
    throw std::invalid_argument("CodeGen: unknown binary op '" +
                                std::string(s) + "'");
  }
  static coreir::IntrinsicId to_intrinsic(std::string_view s) {
    if (auto id = coreir::from_name<coreir::IntrinsicId>(s)) return *id;
    throw std::invalid_argument("CodeGen: unknown intrinsic '" +
                                std::string(s) + "'");
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
  // whatever the caller passed in, so it is checked against m_.nodes.size()
  // before ever reaching coreir::Module::at, which does not check itself.
  coreir::NodeId checked_node(int64_t v) const {
    if (v < 0 || static_cast<size_t>(v) >= m_.nodes.size()) {
      throw culebra::CulebraError("IrError", "no node #" + std::to_string(v),
                                  0, 0);
    }
    return coreir::NodeId{static_cast<uint32_t>(v)};
  }
  coreir::NodeId require_tag(int64_t v, coreir::Tag want,
                             const char* accessor) const {
    const coreir::NodeId n = checked_node(v);
    const coreir::Tag got = m_.at(n).tag;
    if (got != want) {
      throw culebra::CulebraError(
          "IrError", "node #" + std::to_string(v) + " is " +
                        describe(coreir::name_of(got)) + ", not " +
                        describe(coreir::name_of(want)) + " (" + accessor +
                        ")",
          0, 0);
    }
    return n;
  }
  void require_const_kind(coreir::NodeId n, coreir::ConstKind want,
                          const char* accessor) const {
    const coreir::ConstKind got = m_.const_kind(n);
    if (got != want) {
      throw culebra::CulebraError(
          "IrError", "literal #" + std::to_string(n.v) + " is " +
                        describe(coreir::name_of(got)) + ", not " +
                        describe(coreir::name_of(want)) + " (" + accessor +
                        ")",
          0, 0);
    }
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
        "IrError", "node #" + std::to_string(v) + " is " +
                      describe(coreir::name_of(tag)) +
                      ", not a varref or assign",
        0, 0);
  }
  size_t checked_func(int64_t v) const {
    if (v < 0 || static_cast<size_t>(v) >= m_.funcs.size()) {
      throw culebra::CulebraError("IrError", "no func #" + std::to_string(v),
                                  0, 0);
    }
    return static_cast<size_t>(v);
  }
  size_t checked_cmap(int64_t v) const {
    if (v < 0 || static_cast<size_t>(v) >= m_.capture_maps.size()) {
      throw culebra::CulebraError(
          "IrError", "no capture map #" + std::to_string(v), 0, 0);
    }
    return static_cast<size_t>(v);
  }
  const std::string& checked_name(const std::vector<std::string>& names,
                                  int64_t func, int64_t index,
                                  const char* what) const {
    if (index < 0 || static_cast<size_t>(index) >= names.size()) {
      throw culebra::CulebraError(
          "IrError", "func #" + std::to_string(func) + " has " +
                        std::to_string(names.size()) + " " + what +
                        "(s), no #" + std::to_string(index),
          0, 0);
    }
    return names[static_cast<size_t>(index)];
  }
  const coreir::CaptureSrc& checked_capture_entry(int64_t cmap,
                                                   int64_t index) const {
    const auto& entries = m_.capture_maps[checked_cmap(cmap)];
    if (index < 0 || static_cast<size_t>(index) >= entries.size()) {
      throw culebra::CulebraError(
          "IrError", "capture map #" + std::to_string(cmap) + " has " +
                        std::to_string(entries.size()) + " entrie(s), no #" +
                        std::to_string(index),
          0, 0);
    }
    return entries[static_cast<size_t>(index)];
  }

  coreir::Module m_;
  std::vector<std::vector<int64_t>> lists_;
  std::vector<std::vector<coreir::CaptureSrc>> cmaps_;
  bool entry_frame_drops_ = true;
};

}  // namespace culebra::codegen
