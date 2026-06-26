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

#include <string>
#include <vector>

namespace culebra {

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
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "SQLite") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_sqlite(*child)) return true;
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
