#pragma once
// The CodeGen.Module wrap declaration -- the thin TU shape wrap.h expects
// (see foreign_binding.h for the pattern this follows).

#include <codegen.h>
#include <wrap.h>

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
      .method<&codegen::Module::array_lit>("array_lit",
                                           {"items_list", "line", "col"})
      .method<&codegen::Module::object_lit>("object_lit",
                                            {"kv_list", "line", "col"})
      .method<&codegen::Module::index>("index",
                                       {"recv", "key", "line", "col"})
      .method<&codegen::Module::set_index>(
          "set_index", {"recv", "key", "value", "line", "col"})
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
      .method<&codegen::Module::dump_ir>("dump_ir")
      .method<&codegen::Module::dump_bc>("dump_bc");
  return true;
}

}  // namespace culebra
