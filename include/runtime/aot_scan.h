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

// Does the program reference the `Http` namespace? `culebra build` appends the
// OpenSSL static archives (CULEBRA_SSL_LINK) to the link line only when true —
// programs that never touch Http avoid the OpenSSL dependency, mirroring the
// Tensor/BLAS gating. Same conservative bare-identifier match.
inline bool aot_uses_http(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "IDENTIFIER"_ && node.token == "Http") return true;
  for (const auto& child : node.nodes) {
    if (aot_uses_http(*child)) return true;
  }
  return false;
}

}  // namespace culebra
