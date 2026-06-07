#pragma once

// Static analysis: does the program reference the `Tensor` stdlib
// namespace? `culebra build` uses this to pick between
// `libculebra_rt.a` (full) and `libculebra_rt_no_tensor.a`
// (stripped of tensor entry points + cblas refs); the latter binary
// also drops the Accelerate / BLAS link. False positives only
// inflate the binary; false negatives leave runtime calls
// unresolved, so a textual identifier match is the conservative
// choice — `Tensor.zeros(...)` style references plus any bare
// `Tensor` identifier all flip the flag.

#include <parser.h>

namespace culebra {

inline bool aot_uses_tensor(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Tensor") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_tensor(*child)) return true;
  }
  return false;
}

// Does the program reference the `Http` namespace? Reserved for a future
// no-http runtime archive (mirroring no-tensor): OpenSSL currently can't be
// gated by reachability because the single runtime archive references it
// unconditionally (its http helpers are __attribute__((used)), pinned past
// dead-strip), so `culebra build` links OpenSSL whenever Http is compiled in
// regardless of this. Once a stubbed no-http archive exists, the build can
// pick it for programs where this returns false and drop the OpenSSL link.
// Same conservative bare-identifier match as aot_uses_tensor.
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

}  // namespace culebra
