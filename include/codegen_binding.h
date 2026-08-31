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
      .method<&codegen::Module::intrinsic>(
          "intrinsic", {"name", "args_list", "line", "col"})
      .method<&codegen::Module::list_new>("list_new")
      .method<&codegen::Module::list_push>("list_push", {"list", "value"})
      .method<&codegen::Module::capture_map_new>("capture_map_new")
      .method<&codegen::Module::capture_map_push>(
          "capture_map_push", {"cmap", "kind", "index"})
      .method<&codegen::Module::add_capture_map>("add_capture_map", {"cmap"})
      .method<&codegen::Module::add_func>(
          "add_func", {"name", "num_locals", "num_captures", "body"})
      .method<&codegen::Module::set_local_name>("set_local_name",
                                                {"func", "index", "name"})
      .method<&codegen::Module::set_capture_name>("set_capture_name",
                                                  {"func", "index", "name"})
      .method<&codegen::Module::verify>("verify")
      .method<&codegen::Module::run>("run")
      .method<&codegen::Module::dump_ir>("dump_ir")
      .method<&codegen::Module::dump_bc>("dump_bc");
  return true;
}

}  // namespace culebra
