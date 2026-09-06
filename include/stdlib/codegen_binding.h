#pragma once
// The CodeGen.Module wrap declaration -- the thin TU shape wrap.h expects
// (see foreign_binding.h for the pattern this follows).

#include <stdlib/codegen.h>
#include <interop/wrap.h>

namespace culebra {

// Reads one declared argument of a hand-written thunk: the caller's value, or
// nil where it was omitted, checked against the annotation `A` would carry on
// a deduced thunk. The type check is the part a raw thunk must not skip --
// jit_handle_self reinterpret_casts what it is handed, so an unchecked Long
// in a handle's slot is a wild pointer rather than a TypeError.
template <class A>
inline JitValue codegen_raw_arg(int64_t n, JitValue* args, int64_t i,
                                const char* name) {
  // An omitted slot is the declared default's, not a value to check: every
  // parameter here is optional, so nil-because-absent must not be read as a
  // nil the caller passed.
  if (n <= i || args[i].tag == TAG_UNFILLED) return {TAG_NIL, 0};
  const JitValue v = args[i];
  if (!wrap_detail::jit_arg_matches<A>(v)) {
    const auto pos = _jit_arg_pos(static_cast<int>(i));
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("type error: parameter '{}' expects {}", name,
                        wrap_detail::param_type_name<A>()),
        pos.line, pos.col);
  }
  return v;
}

// Program.run's own thunk. `natives` is an Object of culebra Functions,
// which .method<>()'s deduced marshalling has no branch for (jit_arg_get
// reads handles and scalars, never a raw value), so this reads the three
// arguments itself -- the door wrap.h's raw_method opens. A raw thunk owes
// the whole ABI contract the deduced ones get for free: the ownership
// guards, the argument type checks, and the error surfacing.
inline void codegen_program_run_thunk(JitValue* __ret, JitClosure*,
                                      int8_t self_tag, int64_t self_data,
                                      int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  // The method ABI is callee-consumes: self and every argument arrive at +1
  // and are ours to release on every exit, including an unwinding one.
  JitMethodArgs _a{n, args};
  JitMethodSelf _s{self};
  auto* prog = wrap_detail::jit_handle_self<codegen::Program>(self);
  const JitValue rt_v =
      codegen_raw_arg<codegen::Runtime*>(n, args, 0, "rt");
  codegen::Runtime* rt =
      rt_v.tag == TAG_NIL
          ? nullptr
          : wrap_detail::jit_handle_self<codegen::Runtime>(rt_v);
  const JitValue depth_v = codegen_raw_arg<long>(n, args, 1, "max_call_depth");
  const int64_t depth = depth_v.tag == TAG_NIL
                            ? vm::RunOptions{}.max_call_depth
                            : depth_v.data;
  // No annotation names "an Object", so this one is checked by hand.
  const JitValue natives =
      (n > 2 && args[2].tag != TAG_UNFILLED) ? args[2] : JitValue{TAG_NIL, 0};
  if (natives.tag != TAG_NIL && natives.tag != TAG_OBJECT) {
    const auto pos = _jit_arg_pos(2);
    throw culebra::CulebraError(
        "TypeError", "type error: parameter 'natives' expects Object",
        pos.line, pos.col);
  }
  // The same guard the deduced thunks end with: a native body's own C++
  // exception becomes a catchable RuntimeError instead of escaping the
  // process, and a positionless CulebraError gets the call site.
  *__ret = wrap_detail::surface_native_error_at_call_site([&] {
    prog->run(rt, depth, natives);
    return JitValue{TAG_NIL, 0};
  });
}

inline bool register_codegen_binding() {
  wrap<codegen::Module>("CodeGen", "Module")
      .ctor<>()
      .method<&codegen::Module::literal>("literal", {"v", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::bool_literal>("bool_literal",
                                              {"v", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::double_literal>("double_literal",
                                                {"v", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::nil_literal>("nil_literal", {{"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::str_literal>("str_literal",
                                             {"s", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::var_ref>("var_ref",
                                         {"kind", "index", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::unary>("unary", {"op", "operand", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::binary>("binary",
                                        {"op", "lhs", "rhs", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::assign>(
          "assign", {"kind", "index", "value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_if>(
          "make_if", {"cond", "then_branch", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_if_else>(
          "make_if_else",
          {"cond", "then_branch", "else_branch", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_switch>(
          "make_switch", {"subject", "arms", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_switch_default>(
          "make_switch_default",
          {"subject", "arms", "default_body", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_while>(
          "make_while", {"cond", "body", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::block>("block", {"stmts", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::call>("call", {"func", "cmap", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_closure>(
          "make_closure", {"func", "cmap", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::call_value>(
          "call_value", {"callee", "args", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::intrinsic>(
          "intrinsic", {"name", "args", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::declare_native>("declare_native", {"name"})
      .method<&codegen::Module::native_ref>("native_ref",
                                            {"index", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::array_lit>("array_lit",
                                           {"items", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::object_lit>("object_lit",
                                            {"kv", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::index>("index",
                                       {"recv", "key", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::set_index>(
          "set_index", {"recv", "key", "value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::field_get>(
          "field_get", {"recv", "slot", "name", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::field_set>(
          "field_set", {"recv", "slot", "name", "value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::scope>(
          "scope", {"first_local", "end_local", "body", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::scope_release>(
          "scope_release",
          {"first_local", "end_local", "body", "release", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_return>("make_return",
                                             {"value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_break>("make_break",
                                            {{"at", nullptr}, {"line", 1L}, {"col", 1L}, {"depth", 0L}})
      .method<&codegen::Module::make_continue>(
          "make_continue", {{"at", nullptr}, {"line", 1L}, {"col", 1L}, {"depth", 0L}})
      .method<&codegen::Module::make_throw>("make_throw",
                                            {"value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_try>(
          "make_try", {"caught_local", "body", "handler", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_defer>("make_defer",
                                            {"value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::cell_fresh>("cell_fresh",
                                            {"cell", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::make_yield>("make_yield",
                                            {"value", {"at", nullptr}, {"line", 1L}, {"col", 1L}})
      .method<&codegen::Module::list_new>("list_new")
      .method<&codegen::Module::list_push>("list_push", {"list", "value"})
      .method<&codegen::Module::capture_map_new>("capture_map_new")
      .method<&codegen::Module::capture_map_push>(
          "capture_map_push", {"cmap", "kind", "index"})
      .method<&codegen::Module::add_capture_map>("add_capture_map", {"cmap"})
      .method<&codegen::Module::add_func>(
          "add_func", {"name", "num_locals", "num_captures", "num_cells",
                       "num_params", "body"})
      .method<&codegen::Module::set_local_name>("set_local_name",
                                                {"func", "index", "name"})
      .method<&codegen::Module::set_capture_name>("set_capture_name",
                                                  {"func", "index", "name"})
      .method<&codegen::Module::set_generator>("set_generator", {"func"})
      .method<&codegen::Module::set_lenient_arity>("set_lenient_arity",
                                                   {"func"})
      .method<&codegen::Module::set_tail_calls>("set_tail_calls", {"func"})
      .method<&codegen::Module::set_singleton>("set_singleton", {"func"})
      .method<&codegen::Module::set_entry_frame_drops>("set_entry_frame_drops",
                                                       {"on"})
      .method<&codegen::Module::verify>("verify")
      .method<&codegen::Module::run>("run")
      .method<&codegen::Module::compile>("compile")
      .method<&codegen::Module::dump_ir>("dump_ir")
      .method<&codegen::Module::dump_bc>("dump_bc")
      .method<&codegen::Module::num_nodes>("num_nodes")
      .method<&codegen::Module::node_tag>("node_tag", {"node"})
      .method<&codegen::Module::node_line>("node_line", {"node"})
      .method<&codegen::Module::node_col>("node_col", {"node"})
      .method<&codegen::Module::num_children>("num_children", {"node"})
      .method<&codegen::Module::child>("child", {"node", "index"})
      .method<&codegen::Module::const_kind>("const_kind", {"node"})
      .method<&codegen::Module::int_const>("int_const", {"node"})
      .method<&codegen::Module::bool_const>("bool_const", {"node"})
      .method<&codegen::Module::double_const>("double_const", {"node"})
      .method<&codegen::Module::str_const>("str_const", {"node"})
      .method<&codegen::Module::node_op>("node_op", {"node"})
      .method<&codegen::Module::var_kind>("var_kind", {"node"})
      .method<&codegen::Module::var_index>("var_index", {"node"})
      .method<&codegen::Module::switch_subject>("switch_subject", {"node"})
      .method<&codegen::Module::switch_arm_count>("switch_arm_count",
                                                   {"node"})
      .method<&codegen::Module::switch_key>("switch_key", {"node", "index"})
      .method<&codegen::Module::switch_body>("switch_body", {"node", "index"})
      .method<&codegen::Module::switch_has_default>("switch_has_default",
                                                     {"node"})
      .method<&codegen::Module::switch_default_body>("switch_default_body",
                                                      {"node"})
      .method<&codegen::Module::field_slot>("field_slot", {"node"})
      .method<&codegen::Module::field_name>("field_name", {"node"})
      .method<&codegen::Module::field_receiver>("field_receiver", {"node"})
      .method<&codegen::Module::field_set_value>("field_set_value", {"node"})
      .method<&codegen::Module::scope_first_local>("scope_first_local",
                                                    {"node"})
      .method<&codegen::Module::scope_end_local>("scope_end_local", {"node"})
      .method<&codegen::Module::try_caught_local>("try_caught_local", {"node"})
      .method<&codegen::Module::break_depth>("break_depth", {"node"})
      .method<&codegen::Module::continue_depth>("continue_depth", {"node"})
      .method<&codegen::Module::native_index>("native_index", {"node"})
      .method<&codegen::Module::closure_func>("closure_func", {"node"})
      .method<&codegen::Module::closure_cmap>("closure_cmap", {"node"})
      .method<&codegen::Module::cell_index>("cell_index", {"node"})
      .method<&codegen::Module::num_funcs>("num_funcs")
      .method<&codegen::Module::func_name>("func_name", {"func"})
      .method<&codegen::Module::func_num_locals>("func_num_locals", {"func"})
      .method<&codegen::Module::func_num_captures>("func_num_captures",
                                                    {"func"})
      .method<&codegen::Module::func_num_cells>("func_num_cells", {"func"})
      .method<&codegen::Module::func_num_params>("func_num_params", {"func"})
      .method<&codegen::Module::func_body>("func_body", {"func"})
      .method<&codegen::Module::func_is_generator>("func_is_generator",
                                                    {"func"})
      .method<&codegen::Module::func_lenient_arity>("func_lenient_arity",
                                                     {"func"})
      .method<&codegen::Module::func_tail_calls>("func_tail_calls", {"func"})
      .method<&codegen::Module::func_singleton>("func_singleton", {"func"})
      .method<&codegen::Module::func_local_name>("func_local_name",
                                                  {"func", "index"})
      .method<&codegen::Module::func_capture_name>("func_capture_name",
                                                    {"func", "index"})
      .method<&codegen::Module::num_natives>("num_natives")
      .method<&codegen::Module::native_name>("native_name", {"index"})
      .method<&codegen::Module::num_capture_maps>("num_capture_maps")
      .method<&codegen::Module::num_capture_entries>("num_capture_entries",
                                                      {"cmap"})
      .method<&codegen::Module::capture_kind>("capture_kind",
                                              {"cmap", "index"})
      .method<&codegen::Module::capture_index>("capture_index",
                                                {"cmap", "index"});

  // Program has no .ctor: Module::compile() is the only way to get one, so
  // that a Program's existence itself means its module passed verify().
  wrap<codegen::Program>("CodeGen", "Program")
      // The depth bound's default is cpp-vmlib's own, read from it rather
      // than copied: a submodule bump that changes it must not leave this
      // behind naming the old one.
      .raw_method("run", &codegen_program_run_thunk,
                  {{"rt", nullptr},
                   {"max_call_depth", vm::RunOptions{}.max_call_depth},
                   {"natives", nullptr}})
      .method<&codegen::Program::dump_bc>("dump_bc");
  wrap<codegen::Runtime>("CodeGen", "Runtime")
      .ctor<>()
      .method<&codegen::Runtime::live_objects>("live_objects")
      .method<&codegen::Runtime::heap_bytes>("heap_bytes")
      .method<&codegen::Runtime::collect>("collect");
  wrap<codegen::Resolver>("CodeGen", "Resolver")
      .ctor<>()
      .method<&codegen::Resolver::new_fn>("new_fn", {{"parent", -1L}})
      .method<&codegen::Resolver::num_fns>("num_fns")
      .method<&codegen::Resolver::parent_of>("parent_of", {"fn"})
      .method<&codegen::Resolver::set_parent>("set_parent",
                                              {"fn", "parent"})
      .method<&codegen::Resolver::set_func_index>("set_func_index",
                                                  {"fn", "index"})
      .method<&codegen::Resolver::func_index>("func_index", {"fn"})
      .method<&codegen::Resolver::push_scope>("push_scope")
      .method<&codegen::Resolver::pop_scope>("pop_scope")
      .method<&codegen::Resolver::depth>("depth")
      .method<&codegen::Resolver::declare_var>("declare_var",
                                               {"name", "owner"})
      .method<&codegen::Resolver::declare>("declare", {"name", "owner"})
      .method<&codegen::Resolver::declare_in>("declare_in",
                                              {"scope", "name", "owner"})
      .method<&codegen::Resolver::alias>("alias", {"name", "v"})
      .method<&codegen::Resolver::declared_here>("declared_here", {"name"})
      .method<&codegen::Resolver::declared_at>("declared_at",
                                               {"scope", "name"})
      .method<&codegen::Resolver::declared_count>("declared_count", {"scope"})
      .method<&codegen::Resolver::declared_index>("declared_index",
                                                  {"scope", "i"})
      .method<&codegen::Resolver::lookup>("lookup", {"name"})
      .method<&codegen::Resolver::lookup_from>("lookup_from",
                                               {"name", "from_scope"})
      .method<&codegen::Resolver::num_vars>("num_vars")
      .method<&codegen::Resolver::var_name>("var_name", {"v"})
      .method<&codegen::Resolver::var_owner>("var_owner", {"v"})
      .method<&codegen::Resolver::var_slot>("var_slot", {"v"})
      .method<&codegen::Resolver::set_var_slot>("set_var_slot", {"v", "slot"})
      .method<&codegen::Resolver::use>("use", {"v", "fn"})
      .method<&codegen::Resolver::resolve>("resolve", {"name", "fn"})
      .method<&codegen::Resolver::force_cell>("force_cell", {"v"})
      .method<&codegen::Resolver::mark>("mark")
      .method<&codegen::Resolver::rollback>("rollback", {"mark"})
      .method<&codegen::Resolver::reset_fn>("reset_fn",
                                            {"fn", {"parent", -1L}})
      .method<&codegen::Resolver::free_count>("free_count", {"fn"})
      .method<&codegen::Resolver::free_at>("free_at", {"fn", "i"})
      .method<&codegen::Resolver::add_free>("add_free", {"fn", "v"})
      .method<&codegen::Resolver::number_captures>("number_captures")
      .method<&codegen::Resolver::capture_name>("capture_name",
                                                {"fn", "i"})
      .method<&codegen::Resolver::capture_map>("capture_map",
                                               {"m", "builder", "target"})
      .method<&codegen::Resolver::reaches>("reaches", {"fn", "v"})
      .method<&codegen::Resolver::read>(
          "read", {"m", "fn", "v", {"at", nullptr}, {"line", 1L},
                   {"col", 1L}})
      .method<&codegen::Resolver::write>(
          "write", {"m", "fn", "v", "value", {"at", nullptr},
                    {"line", 1L}, {"col", 1L}})
      .method<&codegen::Resolver::name_captures>("name_captures",
                                                 {"m", "func", "fn"})
      .method<&codegen::Resolver::note_call>("note_call", {"f", "g"})
      .method<&codegen::Resolver::close_over_calls>("close_over_calls")
      .method<&codegen::Resolver::access_kind>("access_kind", {"fn", "v"})
      .method<&codegen::Resolver::access_index>("access_index", {"fn", "v"})
      .method<&codegen::Resolver::num_captures>("num_captures", {"fn"})
      .method<&codegen::Resolver::num_cells>("num_cells", {"fn"})
      .method<&codegen::Resolver::cell_of>("cell_of", {"fn", "v"});
  wrap<codegen::FrameLayout>("CodeGen", "FrameLayout")
      .ctor<>()
      .method<&codegen::FrameLayout::alloc_local>("alloc_local", {"name"})
      .method<&codegen::FrameLayout::mark>("mark")
      .method<&codegen::FrameLayout::release>("release", {"mark"})
      .method<&codegen::FrameLayout::num_locals>("num_locals")
      .method<&codegen::FrameLayout::local_name>("local_name", {"slot"});
  return true;
}

}  // namespace culebra
