#pragma once

// `Dir.embedded(name)` names the directory `name` next to the file that calls
// it, as `import` names a module. The runtime (stdlib/vfs.h's
// open_embed_dir) and `culebra build`'s asset baking (main.cc) both resolve a
// name against the entry script's directory, so the loader rebases the calls
// of every other module as it reads it: a literal name becomes the same
// directory's path from the entry script's (relative wherever it can be, so a
// built binary carries no paths of the machine that built it), and any other
// is joined to the module's directory when it runs, through
// `Dir._embedded_in(module_dir, name)`. The entry script's own calls are left
// as they are written.

#include <frontend/parser.h>

#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace culebra {

// Extract a compile-time-constant string from a literal AST node: a raw STRING
// (single-quote / backtick) or a non-interpolated double-quoted string. Returns
// false for anything computed (an interpolation expression, a call, …) — those
// can't be baked at build time.
inline bool _ast_literal_string(const peg::Ast& n, std::string& out) {
  using namespace peg::udl;
  if (n.tag == "STRING"_) {
    out = std::string(n.token);
    return true;
  }
  if (n.tag == "INTERPOLATED_CONTENT"_) {
    out = decode_interpolated_content(n.token);
    return true;
  }
  if (n.tag == "INTERPOLATED_STRING"_) {
    std::string s;
    for (const auto& c : n.nodes) {
      if (c->tag != "INTERPOLATED_CONTENT"_) return false;  // has interpolation
      s += decode_interpolated_content(c->token);
    }
    out = std::move(s);
    return true;
  }
  return false;
}

// The slot holding the literal that names a `Dir.embedded(...)` call's
// directory — the call's one argument, by position or as `name:` — or nullptr
// when the name is worked out as the program runs (`"d{x}"`, `'a' + b`, a
// call, a splat). The one judgment both the AOT scan (what to bake) and the
// loader's rebasing (what to rewrite) make.
template <typename Node>
inline auto embedded_literal_slot(Node& args) {
  using namespace peg::udl;
  using Slot = decltype(&args.nodes[0]);
  if (args.nodes.size() != 1) return Slot{};
  Slot slot = &args.nodes[0];
  if ((*slot)->tag == "KWARG"_) {
    const auto& kw = (*slot)->nodes;
    if (kw.size() != 2 || kw[0]->token != "name") return Slot{};
    slot = &(*slot)->nodes[1];
  }
  std::string s;
  return _ast_literal_string(**slot, s) ? slot : Slot{};
}

// Each `Dir.embedded(...)` in `node`: `fn(call, i)` for the CALL whose children
// i - 1, i and i + 1 are the `Dir` receiver, the `.embedded` member and the
// arguments. The one matcher both the loader's rebasing and the AOT scan use.
template <typename Node, typename Fn>
inline void for_each_embedded_call(Node& node, Fn&& fn) {
  using namespace peg::udl;
  if (node.tag == "CALL"_) {
    for (size_t i = 1; i + 1 < node.nodes.size(); i++) {
      const auto& dot = *node.nodes[i];
      const auto& recv = *node.nodes[i - 1];
      if (dot.original_tag == "DOT"_ && dot.token == "embedded" &&
          recv.tag == "IDENTIFIER"_ && recv.original_tag != "DOT"_ &&
          recv.token == "Dir" &&
          node.nodes[i + 1]->original_tag == "ARGUMENTS"_)
        fn(node, i);
    }
  }
  for (const auto& child : node.nodes) for_each_embedded_call(*child, fn);
}

// A module's directory as the entry script's sees it: relative where one is
// to be had (the empty string for the entry's own directory), absolute where
// none is (another drive).
inline std::string embed_base(const std::filesystem::path& module_dir,
                              const std::filesystem::path& entry_dir) {
  auto rel = module_dir.lexically_normal().lexically_relative(
      entry_dir.lexically_normal());
  if (rel.empty()) return module_dir.lexically_normal().generic_string();
  auto s = rel.generic_string();
  return s == "." ? std::string() : s;
}

// The directory `name` next to a module at `base` (embed_base's), as a name
// the entry script's directory resolves: `.` for that directory itself, and
// no trailing `/`.
inline std::string embed_key(const std::string& base, const std::string& name) {
  if (base.empty()) return name;
  auto key = (std::filesystem::path(base) / name).lexically_normal().generic_string();
  while (key.size() > 1 && key.back() == '/') key.pop_back();
  return key.empty() ? std::string(".") : key;
}

// The text of a token the rebasing writes: AST tokens are views, so the bytes
// are kept for the process (a module's few names; modules are read on more
// than one thread).
inline std::string_view embed_token(std::string s) {
  static std::mutex mu;
  static std::deque<std::string> kept;
  std::lock_guard lock(mu);
  return kept.emplace_back(std::move(s));
}

// Rebase the `Dir.embedded(...)` calls of a module at `base` (embed_base's;
// nothing to do for the entry script's directory).
inline void rebase_embedded_calls(peg::Ast& ast, const std::string& base) {
  if (base.empty()) return;
  for_each_embedded_call(ast, [&](peg::Ast& call, size_t i) {
    // The arguments are an ARG_LIST (ARGUMENTS) of ARG_ITEMs, none or more.
    auto& args = *call.nodes[i + 1];
    SynthNode nf{call.path.c_str(), call.line, call.column};
    if (auto* slot = embedded_literal_slot(args)) {
      // A literal: the same literal, from the entry script's directory.
      std::string name;
      _ast_literal_string(**slot, name);
      *slot = nf.tok("STRING", (*slot)->original_name.c_str(),
                     embed_token(embed_key(base, name)), (*slot)->line,
                     (*slot)->column);
      return;
    }
    // Any other: `Dir._embedded_in(base, <the arguments as written>)`, which
    // binds them as Dir.embedded does.
    call.nodes[i] = nf.tok("IDENTIFIER", "DOT", "_embedded_in",
                           call.nodes[i]->line, call.nodes[i]->column);
    args.nodes.insert(args.nodes.begin(),
                      nf.tok("STRING", "ARG_ITEM", embed_token(base),
                             args.line, args.column));
  });
}

}  // namespace culebra
