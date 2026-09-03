#pragma once
// Single source for the grammar-blob guard key, shared by parser.h and the
// generator (tools/gen_grammar_blob.cc) so the two can't drift. The key folds
// the grammar text AND the cpp-peglib version: the blob layout is
// version-specific and its "PEG1" magic doesn't bump between versions, so a
// grammar edit or peglib upgrade without `just gen-blob` invalidates a stale
// blob and get_parser() falls back to load_grammar() instead of mis-loading it.
#include <cstdint>
#include <string_view>

#include <peglib.h>       // CPPPEGLIB_VERSION

#include "frontend/grammar_def.h"  // culebra::grammar_

namespace culebra {

inline uint64_t fnv1a_fold(uint64_t h, std::string_view s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

inline uint64_t grammar_blob_key() {
  uint64_t h = 1469598103934665603ULL;
  h = fnv1a_fold(h, grammar_);
  h = fnv1a_fold(h, "\x1f");  // separator between grammar and version
  h = fnv1a_fold(h, CPPPEGLIB_VERSION);
  return h;
}

}  // namespace culebra
