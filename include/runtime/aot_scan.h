#pragma once

// Static analysis: does the program reference the `Tensor` stdlib
// namespace? `culebra build` uses this to force-load
// `libculebra_rt_tensor.a` (the strong tensor_eval_node choke that
// overrides the base archive's weak stub) and append the Accelerate /
// BLAS link — only when true. The base archive's weak tensor stub
// breaks cblas reachability, so a non-Tensor program references no BLAS
// symbol and drops that link. False positives only inflate the binary;
// false negatives leave runtime calls unresolved, so a textual
// identifier match is the conservative choice — `Tensor.zeros(...)`
// style references plus any bare `Tensor` identifier all flip the flag.

#include <parser.h>

#include <algorithm>
#include <string>
#include <vector>

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

// The constant string literal of `Embed.dir("...")`'s argument, descending only
// through structural wrappers — never into a nested call. So `Embed.dir("ui")`
// reads "ui", but `Embed.dir(make_path("ui"))` finds no literal (returns false)
// rather than baking the wrong directory from a buried argument. Returns false
// when the argument isn't a plain string literal (the runtime then falls back
// to live disk, and nothing is embedded).
inline bool _first_literal_string(const peg::Ast& node, std::string& out) {
  using namespace peg::udl;
  if (_ast_literal_string(node, out)) return true;
  if (node.tag == "CALL"_) return false;  // don't cross into a nested call
  for (const auto& c : node.nodes)
    if (_first_literal_string(*c, out)) return true;
  return false;
}

// Collect the literal directory names passed to `Embed.dir("...")` across the
// program. `culebra build` walks each at build time (relative to the entry
// script) and bakes its files into the binary; dirs whose argument isn't a
// string literal are skipped (they fall back to live disk at runtime). The
// returned names are deduplicated.
inline void aot_collect_embed_dirs(const peg::Ast& node,
                                   std::vector<std::string>& out) {
  using namespace peg::udl;
  if (node.tag == "CALL"_) {
    for (size_t i = 1; i + 1 < node.nodes.size(); i++) {
      const auto& dot = *node.nodes[i];
      const auto& recv = *node.nodes[i - 1];
      if ((dot.original_tag == "DOT"_) && dot.token == "dir" &&
          recv.tag == "IDENTIFIER"_ && recv.original_tag != "DOT"_ &&
          recv.token == "Embed" &&
          node.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
        std::string name;
        if (_first_literal_string(*node.nodes[i + 1], name) &&
            std::find(out.begin(), out.end(), name) == out.end()) {
          out.push_back(name);
        }
      }
    }
  }
  for (const auto& child : node.nodes) aot_collect_embed_dirs(*child, out);
}

inline bool aot_uses_tensor(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Tensor") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_tensor(*child)) return true;
  }
  return false;
}

// Does the program reference the `Http` namespace? Drives the force-load of
// libculebra_rt_http.a (the strong http_request choke that overrides the base
// archive's weak stub) and appends the OpenSSL + zlib link — only when true,
// the same usage-gating as tensor/compress/wrap. The base archive's weak http
// stub breaks reachability, so a non-Http program references no httplib/TLS/zlib
// symbol and links no OpenSSL at all (it pays nothing for it; ~4 MB lighter than
// an Http binary). Same conservative bare-identifier match as aot_uses_tensor.
inline bool aot_uses_http(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Http") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_http(*child)) return true;
  }
  return false;
}

// Does the program reference the `Compress` namespace? Drives the libz link
// and the force-load of libculebra_rt_compress.a (the lone zlib choke), exactly
// like aot_uses_tensor/http. Same conservative bare-identifier match.
inline bool aot_uses_compress(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Compress") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_compress(*child)) return true;
  }
  return false;
}

// Does the program reference the `SQLite` namespace? Drives the force-load of
// libculebra_rt_sqlite.a (the strong sqlite3 wrappers + the bundled amalgamation
// object that override the base archive's weak stubs) and appends the SQLite
// link deps — only when true, the same usage-gating as tensor/http/compress. The
// base archive's weak SQLite stubs reference no sqlite3 symbol, so a non-SQLite
// program links no sqlite3 at all. Same conservative bare-identifier match.
inline bool aot_uses_sqlite(const peg::Ast& node) {
#if defined(CULEBRA_SQLITE_ENABLED)
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "SQLite") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_sqlite(*child)) return true;
  }
#else
  (void)node;
#endif
  return false;
}

// Does the program reference the `Graphics` namespace? Drives the force-load of
// libculebra_rt_graphics.a (the wrap registration whose static registrar pulls
// in raylib) and appends the raylib/SDL link deps — only when true, the same
// usage-gating as tensor/http/compress/sqlite. Graphics has no weak choke: it
// simply isn't in the base archive, so a non-Graphics binary references no
// raylib symbol. Same conservative bare-identifier match.
inline bool aot_uses_graphics(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Graphics") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_graphics(*child)) return true;
  }
  return false;
}

// Does the program reference the `Webview` namespace (or the `Desktop` facade
// that drives it)? Force-loads libculebra_rt_webview.a and appends the OS
// WebView framework link deps — only when true, the same usage-gating as
// Graphics (no weak choke; the symbols simply aren't in the base archive).
// Matches `Desktop` too so the facade triggers the force-load even when the
// program never names `Webview` directly. Same conservative bare-identifier
// match.
inline bool aot_uses_webview(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ &&
      (node.token == "Webview" || node.token == "Desktop"))
    return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_webview(*child)) return true;
  }
  return false;
}

// Does the program reference any of `names`? Gates the `culebra wrap`
// archive + its wrapped-library link flags, mirroring the tensor/http/
// compress axes — but the namespace set is dynamic (whatever the embedded
// `culebra wrap` declarations registered), so the names come from the
// runtime registry rather than a hardcoded literal. Same conservative
// bare-identifier match: a false positive only links a wrapped library a
// program never calls; a false negative would leave the namespace
// unregistered. `names` is small (one per wrapped namespace), so the
// linear scan per identifier is fine.
inline bool aot_uses_any_name(const peg::Ast& node,
                              const std::vector<std::string>& names) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_) {
    for (const auto& n : names)
      if (node.token == n) return true;
  }
  for (const auto& child : node.nodes) {
    if (aot_uses_any_name(*child, names)) return true;
  }
  return false;
}

}  // namespace culebra
