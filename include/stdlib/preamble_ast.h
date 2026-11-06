#pragma once

// A stdlib module's parsed AST as data. culebra_preamble_cc writes each
// module's at build time (beside its native object), and the splice builds
// the `<stdlib>` module from it instead of parsing the source at every start
// of the executor (docs/internals/vm.md §2). A module the binary did not bake
// is parsed once per process and goes through the same records, so the two
// paths differ only in where the records come from.

#include <peglib.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace culebra {

// One node, in preorder: its `children` subtrees follow it. The names, the
// path and a token the source does not hold (a transform synthesized it) are
// offsets into the strings; a token the source holds is its offset there.
struct AstRecord {
  uint32_t position, length, token;
  uint16_t name, original_name, path, line, column, token_size, children;
  uint8_t choice_count, choice, original_choice_count, original_choice, flags;
  friend bool operator==(const AstRecord&, const AstRecord&) = default;
};
inline constexpr uint8_t kAstIsToken = 1, kAstTokenInSource = 2,
                         kAstPreservePosition = 4;

struct AstData {
  std::span<const AstRecord> records;
  std::string_view strings;
  friend bool operator==(const AstData& a, const AstData& b) {
    return a.strings == b.strings && std::ranges::equal(a.records, b.records);
  }
};

// What flatten_ast fills: the storage an AstData views.
struct FlatAst {
  std::vector<AstRecord> records;
  std::string strings;
  AstData data() const { return {records, strings}; }
};

// Where `token` starts in `source`, or nullopt when `source` does not hold it.
inline std::optional<size_t> offset_in(std::string_view token,
                                       std::string_view source) {
  const char* t = token.data();
  if (!t || t < source.data() || t + token.size() > source.data() + source.size())
    return std::nullopt;
  return static_cast<size_t>(t - source.data());
}

// `root`, whose source tokens point into `source`, as records; nullopt when a
// value does not fit its field (a module of more than 65535 lines, say).
inline std::optional<FlatAst> flatten_ast(const peg::Ast& root,
                                          std::string_view source) {
  FlatAst out;
  bool fits = true;
  auto narrow = [&]<class T>(T& field, size_t v) {
    field = static_cast<T>(v);
    fits = fits && field == v;
  };
  std::unordered_map<std::string, size_t> interned;
  auto intern = [&](std::string_view s) {
    auto [it, fresh] = interned.try_emplace(std::string(s), out.strings.size());
    if (fresh) {
      out.strings.append(s);
      out.strings.push_back('\0');
    }
    return it->second;
  };
  auto walk = [&](auto& self, const peg::Ast& n) -> void {
    AstRecord r{};
    narrow(r.name, intern(n.name));
    narrow(r.original_name, intern(n.original_name));
    narrow(r.path, intern(n.path));
    narrow(r.line, n.line);
    narrow(r.column, n.column);
    narrow(r.position, n.position);
    narrow(r.length, n.length);
    narrow(r.choice_count, n.choice_count);
    narrow(r.choice, n.choice);
    narrow(r.original_choice_count, n.original_choice_count);
    narrow(r.original_choice, n.original_choice);
    r.flags = (n.is_token ? kAstIsToken : 0) |
              (n.preserve_position ? kAstPreservePosition : 0);
    if (n.is_token) {
      narrow(r.token_size, n.token.size());
      if (auto at = offset_in(n.token, source)) {
        r.flags |= kAstTokenInSource;
        narrow(r.token, *at);
      } else {
        narrow(r.token, intern(n.token));
      }
    }
    narrow(r.children, n.nodes.size());
    out.records.push_back(r);
    for (const auto& c : n.nodes) self(self, *c);
  };
  walk(walk, root);
  if (!fits) return std::nullopt;
  return out;
}

// The AST `data` records, as if parsed from `source` at `offset`: positions
// move by `offset`, and a source token points into `source` there. `source`
// and `data.strings` have to outlive the result.
inline std::shared_ptr<peg::Ast> instantiate_ast(const AstData& data,
                                                 std::string_view source,
                                                 size_t offset) {
  size_t i = 0;
  auto build = [&](auto& self) -> std::shared_ptr<peg::Ast> {
    const auto& r = data.records[i++];
    const char* s = data.strings.data();
    const bool preserve = r.flags & kAstPreservePosition;
    std::shared_ptr<peg::Ast> n;
    if (r.flags & kAstIsToken) {
      std::string_view token =
          r.flags & kAstTokenInSource
              ? source.substr(offset + r.token, r.token_size)
              : std::string_view(s + r.token, r.token_size);
      n = std::make_shared<peg::Ast>(s + r.path, r.line, r.column, s + r.name,
                                     token, r.position + offset, r.length,
                                     r.choice_count, r.choice, preserve);
    } else {
      // The constructor copies the vector it is handed: fill the node's own.
      n = std::make_shared<peg::Ast>(
          s + r.path, r.line, r.column, s + r.name,
          std::vector<std::shared_ptr<peg::Ast>>{}, r.position + offset,
          r.length, r.choice_count, r.choice, preserve);
      n->nodes.reserve(r.children);
      for (uint32_t k = 0; k < r.children; k++) {
        n->nodes.push_back(self(self));
        n->nodes.back()->parent = n;
      }
    }
    n->original_name = s + r.original_name;
    n->original_tag = peg::str2tag(n->original_name);
    n->original_choice_count = r.original_choice_count;
    n->original_choice = r.original_choice;
    return n;
  };
  return build(build);
}

// The modules this binary baked, by registered name (`Time`, `__Matchers`,
// …): generated beside baked_preambles() from the same build step.
struct BakedAst {
  const char* name;
  AstData data;
};
#ifdef CULEBRA_HAS_BAKED_PREAMBLES
std::span<const BakedAst> baked_preamble_asts();
#else
inline std::span<const BakedAst> baked_preamble_asts() { return {}; }
#endif

inline const BakedAst* baked_preamble_ast(std::string_view name) {
  for (const auto& b : baked_preamble_asts())
    if (name == b.name) return &b;
  return nullptr;
}

}  // namespace culebra
