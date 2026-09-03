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

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

// Every name the splice can register: the namespace modules and the
// bare-function groups. What CMake bakes must be exactly this list.
std::vector<std::string> bakeable_names() {
  std::vector<std::string> out;
  for (const auto& m : culebra::lazy_ns_modules()) out.emplace_back(m.name);
  for (const auto& g : culebra::lazy_fn_groups()) out.emplace_back(g.name);
  return out;
}

// The registration source for `name` alone — the same string the splice would
// have contributed for it (stdlib_module_source), so the baked object is the
// module the lane would otherwise have lowered.
std::string source_for(std::string_view name) {
  for (const auto& m : culebra::lazy_ns_modules())
    if (name == m.name) return culebra::stdlib_module_source(m);
  for (const auto& g : culebra::lazy_fn_groups())
    if (name == g.name) return culebra::stdlib_module_source(g);
  return {};
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
  auto prog = culebra::vm::Compiler::compile_stdlib_prologue(*ast);
  return culebra::vm::Lowering::build_preamble_object(
      prog, out, /*opt_level=*/2, culebra::baked_preamble_symbol(name));
}
