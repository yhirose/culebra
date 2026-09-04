#pragma once
// The CodeGen.Module wrap declaration -- the thin TU shape wrap.h expects
// (see foreign_binding.h for the pattern this follows).

#include <stdlib/codegen.h>
#include <interop/wrap.h>

namespace culebra {

inline bool register_codegen_binding() {
  wrap<codegen::Module>("CodeGen", "Module")
      .ctor<>()
      .method<&codegen::Module::literal>("literal", {"v", "line", "col"})
      .method<&codegen::Module::bool_literal>("bool_literal",
                                              {"v", "line", "col"})
      .method<&codegen::Module::double_literal>("double_literal",
                                                {"v", "line", "col"})
      .method<&codegen::Module::nil_literal>("nil_literal", {"line", "col"})
      .method<&codegen::Module::str_literal>("str_literal",
                                             {"s", "line", "col"})
      .method<&codegen::Module::var_ref>("var_ref",
                                         {"kind", "index", "line", "col"})
      .method<&codegen::Module::unary>("unary", {"op", "operand", "line", "col"})
      .method<&codegen::Module::binary>("binary",
                                        {"op", "lhs", "rhs", "line", "col"})
      .method<&codegen::Module::assign>(
          "assign", {"kind", "index", "value", "line", "col"})
      .method<&codegen::Module::make_if>(
          "make_if", {"cond", "then_branch", "line", "col"})
      .method<&codegen::Module::make_if_else>(
          "make_if_else",
          {"cond", "then_branch", "else_branch", "line", "col"})
      .method<&codegen::Module::make_switch>(
          "make_switch", {"subject", "arms_list", "line", "col"})
      .method<&codegen::Module::make_switch_default>(
          "make_switch_default",
          {"subject", "arms_list", "default_body", "line", "col"})
      .method<&codegen::Module::make_while>(
          "make_while", {"cond", "body", "line", "col"})
      .method<&codegen::Module::block>("block", {"stmts_list", "line", "col"})
      .method<&codegen::Module::call>("call", {"func", "cmap", "line", "col"})
      .method<&codegen::Module::make_closure>(
          "make_closure", {"func", "cmap", "line", "col"})
      .method<&codegen::Module::call_value>(
          "call_value", {"callee", "args_list", "line", "col"})
      .method<&codegen::Module::intrinsic>(
          "intrinsic", {"name", "args_list", "line", "col"})
      .method<&codegen::Module::declare_native>("declare_native", {"name"})
      .method<&codegen::Module::native_ref>("native_ref",
                                            {"index", "line", "col"})
      .method<&codegen::Module::array_lit>("array_lit",
                                           {"items_list", "line", "col"})
      .method<&codegen::Module::object_lit>("object_lit",
                                            {"kv_list", "line", "col"})
      .method<&codegen::Module::index>("index",
                                       {"recv", "key", "line", "col"})
      .method<&codegen::Module::set_index>(
          "set_index", {"recv", "key", "value", "line", "col"})
      .method<&codegen::Module::field_get>(
          "field_get", {"recv", "slot", "name", "line", "col"})
      .method<&codegen::Module::field_set>(
          "field_set", {"recv", "slot", "name", "value", "line", "col"})
      .method<&codegen::Module::scope>(
          "scope", {"first_local", "end_local", "body", "line", "col"})
      .method<&codegen::Module::scope_release>(
          "scope_release",
          {"first_local", "end_local", "body", "release_list", "line", "col"})
      .method<&codegen::Module::make_return>("make_return",
                                             {"value", "line", "col"})
      .method<&codegen::Module::make_break>("make_break", {"line", "col"})
      .method<&codegen::Module::make_continue>("make_continue",
                                               {"line", "col"})
      .method<&codegen::Module::make_throw>("make_throw",
                                            {"value", "line", "col"})
      .method<&codegen::Module::make_try>(
          "make_try", {"caught_local", "body", "handler", "line", "col"})
      .method<&codegen::Module::make_defer>("make_defer",
                                            {"value", "line", "col"})
      .method<&codegen::Module::cell_fresh>("cell_fresh",
                                            {"cell", "line", "col"})
      .method<&codegen::Module::make_yield>("make_yield",
                                            {"value", "line", "col"})
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
      .method<&codegen::Program::run>(
          "run", {{"rt", nullptr},
                  {"max_call_depth", vm::RunOptions{}.max_call_depth}})
      .method<&codegen::Program::dump_bc>("dump_bc");
  wrap<codegen::Runtime>("CodeGen", "Runtime")
      .ctor<>()
      .method<&codegen::Runtime::live_objects>("live_objects")
      .method<&codegen::Runtime::heap_bytes>("heap_bytes")
      .method<&codegen::Runtime::collect>("collect");
  return true;
}

}  // namespace culebra
