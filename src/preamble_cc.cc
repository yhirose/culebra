// culebra_preamble_cc — build-time tool: compiles one stdlib preamble module
// (src/preambles/*.cul, in the shape the splice registers it) into a native
// object whose entry `culebra_preamble_<Name>` registers the module's builder.
// The driver and libculebra_rt.a carry those objects, so a JIT or AOT run
// calls the entry instead of lowering the module at every start-up
// (docs/internals/vm.md §2). Same compiler, same lowering, same source as
// the splice — only when it runs differs.
//
//   culebra_preamble_cc <Name> -o <object>
//   culebra_preamble_cc --check-list <Name>…   exit 1 unless that is the list
#include <culebra.h>
#include <stdlib/bindings.h>
#include <stdlib/preamble.h>
#include <jit/lowering.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// Every name a compiled lane can call an entry for: the core built-in traits,
// which every program registers, plus the splice's trait units, namespace
// modules and bare-function groups. What CMake bakes must be exactly this
// list.
std::vector<std::string> bakeable_names() {
  std::vector<std::string> out{culebra::kBuiltinTraitsBakedName};
  for (const auto& u : culebra::builtin_trait_units()) out.emplace_back(u.name);
  for (const auto& m : culebra::lazy_ns_modules()) out.emplace_back(m.name);
  for (const auto& g : culebra::lazy_fn_groups()) out.emplace_back(g.name);
  return out;
}

const culebra::BuiltinTraitUnit* trait_unit(std::string_view name) {
  for (const auto& u : culebra::builtin_trait_units())
    if (name == u.name) return &u;
  return nullptr;
}

// The registration source for `name` alone — the same string the lane would
// otherwise have lowered itself: the splice's contribution for a stdlib
// module or trait unit (stdlib_module_source), the traits prologue for the
// core traits.
std::string source_for(std::string_view name) {
  if (name == culebra::kBuiltinTraitsBakedName)
    return std::string(culebra::builtin_traits_preamble());
  if (const auto* u = trait_unit(name)) return culebra::stdlib_module_source(*u);
  for (const auto& m : culebra::lazy_ns_modules())
    if (name == m.name) return culebra::stdlib_module_source(m);
  for (const auto& g : culebra::lazy_fn_groups())
    if (name == g.name) return culebra::stdlib_module_source(g);
  return {};
}

// A trait unit's declaration, held to its row in builtin_trait_units: the
// one trait it declares, and `required` as exactly its methods without a
// default — the selection reads the row, so a method added to the trait and
// not to the row would let a class conform where the trait is never declared.
bool unit_matches_declaration(const culebra::BuiltinTraitUnit& u,
                              const peg::Ast& ast) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> decls;
  for (const auto& n : ast.nodes)
    if (n->tag == "TRAIT_DECL"_) decls.push_back(n.get());
  if (ast.tag == "TRAIT_DECL"_) decls.push_back(&ast);
  std::set<std::string_view> declared, listed(u.required.begin(),
                                               u.required.end());
  std::string_view head;
  for (const auto* d : decls)
    for (const auto& part : d->nodes) {
      if (part->tag == "TRAIT_HEAD"_) head = part->token;
      if (part->tag != "TRAIT_METHOD"_) continue;
      bool has_default = std::ranges::any_of(
          part->nodes, [](const auto& p) { return p->tag == "TRAIT_BODY"_; });
      if (!has_default) declared.insert(part->nodes[0]->token);
    }
  if (decls.size() == 1 && head == u.trait && declared == listed) return true;
  std::fprintf(stderr,
               "culebra_preamble_cc: trait unit '%.*s' must declare the one "
               "trait `%.*s` whose methods without a default are its "
               "`required` row (shared.h builtin_trait_units)\n",
               static_cast<int>(u.name.size()), u.name.data(),
               static_cast<int>(u.trait.size()), u.trait.data());
  return false;
}

// The tokens of what a default runs: its body and its parameters' default
// values. The trait's own name (`trait Dir`) only names it.
void default_body_tokens(const peg::Ast& ast,
                         std::unordered_set<std::string_view>& out) {
  if (ast.name == "TRAIT_BODY" || ast.name == "DEFAULT_VALUE") {
    culebra::collect_ast_tokens(ast, out);
    return;
  }
  for (const auto& n : ast.nodes) default_body_tokens(*n, out);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--check-list") {
    auto names = bakeable_names();
    std::set<std::string> want(names.begin(), names.end());
    std::set<std::string> got(argv + 2, argv + argc);
    if (want == got) return 0;
    std::fprintf(stderr,
                 "culebra_preamble_cc: the baked-module list in CMakeLists.txt "
                 "does not match lazy_ns_modules() + lazy_fn_groups():\n");
    for (const auto& n : want)
      if (!got.count(n)) std::fprintf(stderr, "  missing from CMake: %s\n", n.c_str());
    for (const auto& n : got)
      if (!want.count(n)) std::fprintf(stderr, "  unknown to the stdlib: %s\n", n.c_str());
    return 1;
  }
  if (argc != 4 || std::string(argv[2]) != "-o") {
    std::fprintf(stderr, "usage: culebra_preamble_cc <Name> -o <object>\n");
    return 2;
  }
  std::string name = argv[1], out = argv[3];
  auto src = std::make_shared<std::string>(source_for(name));
  if (src->empty()) {
    std::fprintf(stderr, "culebra_preamble_cc: no stdlib module '%s'\n",
                 name.c_str());
    return 1;
  }

  culebra::install_jit_stdlib();
  std::vector<std::string> msgs;
  auto ast = culebra::parse_with_transforms(culebra::kStdlibPreamblePath, *src,
                                            msgs);
  if (!ast) {
    for (const auto& s : msgs) std::fprintf(stderr, "%s\n", s.c_str());
    return 1;
  }
  // A built-in trait runs where the program never named a stdlib module: a
  // default that names one (a `replace`, a `Regex.`) works only when the
  // program names it too, and is a NameError otherwise (as on a Windows-only
  // branch).
  const auto* unit = trait_unit(name);
  if (unit && !unit_matches_declaration(*unit, *ast)) return 1;
  if (unit || name == culebra::kBuiltinTraitsBakedName) {
    std::unordered_set<std::string_view> names;
    default_body_tokens(*ast, names);
    auto sel = culebra::select_stdlib_modules(names);
    if (!sel.modules.empty() || !sel.groups.empty()) {
      std::fprintf(stderr,
                   "culebra_preamble_cc: the built-in traits name stdlib "
                   "modules or functions a program may not have:");
      for (const auto* m : sel.modules) std::fprintf(stderr, " %s", m->name);
      for (const auto* g : sel.groups)
        for (auto n : g->members)
          if (names.contains(n))
            std::fprintf(stderr, " %.*s", static_cast<int>(n.size()), n.data());
      std::fprintf(stderr, "\n");
      return 1;
    }
  }
  // Every type the module declares has to be on the list that answers for its
  // name without the declaration: a lane that calls this entry never reads
  // the module's other types, and lint checks a `@value` field before any
  // lane parses the module's `@value` classes.
  culebra::DeclaredTypeNames types;
  culebra::collect_declared_type_names(*ast, types);
  bool listed = true;
  auto require = [&](const culebra::TypeNames& names, auto is_listed,
                     const char* list) {
    for (const auto& n : names)
      if (!is_listed(n)) {
        std::fprintf(stderr,
                     "culebra_preamble_cc: stdlib module '%s' declares the "
                     "type `%s`, which %s (parser.h) lacks\n",
                     name.c_str(), n.c_str(), list);
        listed = false;
      }
  };
  require(types.classes, culebra::is_stdlib_type_name, "is_stdlib_type_name");
  require(types.others, culebra::is_stdlib_type_name, "is_stdlib_type_name");
  require(types.values, culebra::is_stdlib_value_class_name,
          "is_stdlib_value_class_name");
  if (!listed) return 1;
  auto prog = culebra::vm::Compiler::compile_stdlib_prologue(*ast);
  return culebra::vm::Lowering::build_preamble_object(
      prog, out, /*opt_level=*/2, culebra::baked_preamble_symbol(name));
}
