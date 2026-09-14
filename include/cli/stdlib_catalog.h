#pragma once

// The stdlib as an editor's type inference sees it (infer::Catalog): the native
// namespaces and their signatures, the wrapped C++ classes, the global
// functions, and the culebra-source modules (Regex, PEG, Time, ...). A module's
// members are read from its embedded source with the same resolution and
// inference a document gets, so what its functions return is known too:
// `Regex.compile(p).` completes to the Regex class's methods.

#include <cli/infer.h>
#include <frontend/resolve.h>
#include <interop/wrap_registry.h>
#include <stdlib/bindings.h>
#include <stdlib/preamble.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace culebra::lsp {

class StdlibCatalog final : public infer::Catalog {
 public:
  StdlibCatalog() { build(); }
  StdlibCatalog(const StdlibCatalog&) = delete;
  StdlibCatalog& operator=(const StdlibCatalog&) = delete;

  const std::vector<infer::Member>* namespace_members(
      std::string_view path) const override {
    auto it = namespaces_.find(path);
    return it == namespaces_.end() ? nullptr : &it->second;
  }
  const std::vector<infer::Member>& globals() const override { return globals_; }

 private:
  // A culebra-source module, kept parsed: the member types read from it point
  // into its AST and are read through its inference.
  struct Module {
    std::string source;
    std::shared_ptr<peg::Ast> ast;
    resolve::Resolution res;
    std::unique_ptr<infer::Inference> inference;
  };

  static void add(std::vector<infer::Member>& ms, infer::Member m) {
    for (const auto& x : ms)
      if (x.name == m.name) return;
    ms.push_back(std::move(m));
  }

  static infer::Type namespace_type(const std::string& path) {
    infer::Alt a{infer::Kind::Namespace};
    a.name = path;
    return infer::Type::single(std::move(a));
  }

  void add_row(std::string_view ns, std::string_view sub, std::string_view name,
               std::string detail, std::string_view return_type) {
    if (ns.empty() || ns.starts_with("_") || name.starts_with("_")) return;
    std::string path(ns);
    if (!sub.empty()) {
      std::string sub_path = path + "." + std::string(sub);
      add(namespaces_[path], {std::string(sub), infer::MemberKind::Namespace,
                              sub_path, namespace_type(sub_path)});
      path = sub_path;
    }
    add(namespaces_[path], {std::string(name), infer::MemberKind::Function,
                            std::move(detail), infer::parse_type(return_type)});
  }

  void build() {
    for (const auto& g : ns_groups())
      if (g.group)
        for (const auto& s : g.group->sigs)
          add_row(s.ns, s.sub, s.name, infer::canon_signature(s), s.return_type);
    for (const auto& row : wrapped_ns_rows()) {
      std::string detail = row.name + "(";
      for (size_t i = 0; i < row.param_names.size(); i++) {
        if (i) detail += ", ";
        detail += row.param_names[i];
        if (i < row.param_types.size() && !row.param_types[i].empty())
          detail += ": " + std::string(row.param_types[i]);
      }
      detail += ")";
      if (!row.return_type.empty()) detail += " -> " + std::string(row.return_type);
      add_row(row.ns, row.sub, row.name, std::move(detail), row.return_type);
    }
    for (const auto& m : lazy_ns_modules())
      if (!std::string_view(m.name).starts_with("_"))
        read_module(m.name, m.source, m.builder);

    for (const auto& s : kCanonSigs_Bare)
      if (!s.name.starts_with("_"))
        add(globals_, {std::string(s.name), infer::MemberKind::Function,
                       infer::canon_signature(s), infer::parse_type(s.return_type)});
    for (const auto& g : lazy_fn_groups())
      read_functions(lazy_fn_group_source(g.name));
    for (const auto& [path, members] : namespaces_)
      if (path.find('.') == std::string::npos)
        add(globals_, {path, infer::MemberKind::Namespace, path, namespace_type(path)});
  }

  Module* parse_module(std::string_view label, std::string_view source) {
    auto mod = std::make_unique<Module>();
    mod->source = std::string(source);
    std::vector<ParseFailure> failures;
    mod->ast = parse("<" + std::string(label) + ">", mod->source, failures);
    if (!mod->ast) return nullptr;
    mod->res = resolve::resolve_module(*mod->ast, mod->source);
    mod->inference =
        std::make_unique<infer::Inference>(*mod->ast, mod->source, mod->res, this);
    modules_.push_back(std::move(mod));
    return modules_.back().get();
  }

  // The object literal a module's builder returns: its public members.
  static const peg::Ast* module_object(const peg::Ast& root,
                                       std::string_view builder) {
    using namespace peg::udl;
    const peg::Ast* body = nullptr;
    auto consider = [&](const peg::Ast& s) {
      if (s.tag == "ASSIGNMENT"_) {
        auto av = view_assignment(s);
        const auto* t = assign_name_target(s, av);
        if (t && t->token == builder && av.rhs->tag == "FUNCTION"_)
          body = view_function(*av.rhs).body.get();
      } else if (s.tag == "MULTIFN_DECL"_) {
        size_t i = first_non_decorator_index(s);
        if (i < s.nodes.size() && s.nodes[i]->token == builder)
          body = s.nodes.back().get();
      }
    };
    if (root.tag == "STATEMENTS"_)
      for (const auto& c : root.nodes) consider(*c);
    if (!body) return nullptr;
    const peg::Ast* tail = body->tag == "STATEMENTS"_ && !body->nodes.empty()
                               ? body->nodes.back().get()
                               : body;
    return tail->tag == "OBJECT"_ ? tail : nullptr;
  }

  infer::Member describe_value(const Module& mod, std::string key,
                               const peg::Ast& value) {
    infer::Type t = mod.inference->expr_type(value);
    if (!t.unknown() && t.alts[0].kind == infer::Kind::Function &&
        t.alts[0].function && t.alts[0].owner) {
      const auto& a = t.alts[0];
      return {key, infer::MemberKind::Function, a.owner->signature(key, *a.function),
              a.owner->return_type(*a.function)};
    }
    if (!t.unknown() && t.alts[0].kind == infer::Kind::Class)
      return {key, infer::MemberKind::Class, "class " + key, t};
    return {key, infer::MemberKind::Constant,
            t.unknown() ? key : key + ": " + t.to_string(), t};
  }

  void read_module(std::string_view name, std::string_view source,
                   std::string_view builder) {
    using namespace peg::udl;
    Module* mod = parse_module(name, source);
    if (!mod) return;
    const peg::Ast* object = module_object(*mod->ast, builder);
    if (!object) return;
    auto& ms = namespaces_[std::string(name)];
    for (const auto& p : object->nodes) {
      if (p->tag != "OBJECT_PROPERTY"_) continue;
      auto pv = view_object_property(*p);
      if (pv.key->tag != "IDENTIFIER"_) continue;
      std::string key(pv.key->token);
      if (key.starts_with("_")) continue;
      add(ms, describe_value(*mod, key, *pv.value));
    }
  }

  void read_functions(std::string_view source) {
    using namespace peg::udl;
    if (source.empty()) return;
    Module* mod = parse_module("stdlib", source);
    if (!mod) return;
    const peg::Ast& root = *mod->ast;
    auto consider = [&](const peg::Ast& s) {
      if (s.tag != "MULTIFN_DECL"_) return;
      size_t i = first_non_decorator_index(s);
      if (i >= s.nodes.size()) return;
      std::string name(s.nodes[i]->token);
      if (name.starts_with("_")) return;
      add(globals_, {name, infer::MemberKind::Function,
                     mod->inference->signature(name, s),
                     mod->inference->return_type(s)});
    };
    if (root.tag == "STATEMENTS"_)
      for (const auto& c : root.nodes) consider(*c);
    else
      consider(root);
  }

  std::map<std::string, std::vector<infer::Member>, std::less<>> namespaces_;
  std::vector<infer::Member> globals_;
  std::deque<std::unique_ptr<Module>> modules_;
};

// Built on first use: reading the culebra-source modules parses each of them.
inline const StdlibCatalog& stdlib_catalog() {
  static const StdlibCatalog catalog;
  return catalog;
}

}  // namespace culebra::lsp
