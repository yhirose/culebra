#pragma once

// Value-neutral FST core for the `FST` namespace (the `_FST` native rows in
// bindings.h). Mirrors peg.h / regex.h: no Value / JitValue here — the binding
// layer turns the plain structs below into culebra Arrays and Objects.
//
// The namespace is `fstdict`, not `fst`: a `culebra::fst` would shadow
// cpp-fstlib's own `::fst` at every `fst::` spelled inside namespace culebra
// (the same reason peg.h's core is `culebra::pegparser`).
//
// Like peg.h and unlike regex.h, this header carries no linkage split: it is
// always compiled into the core archive. cpp-fstlib links nothing external and
// runs no start-up constructor — the two things that bought Regex its own AOT
// axis — and every entry point below is a function template or calls one, so a
// binary that never names FST instantiates none of them.
//
// A matcher is rebuilt from the byte code on every call rather than cached or
// held open: construction measured 31.5 ns against a 1 MB / 100k-key
// dictionary, a fifth of a single `contains` and 7% over a whole
// construct-once loop. That buys the culebra-visible classes the right to hold
// nothing but the byte code String — copyable, sendable across isolates, and
// with no drop contract or handle registry to leak.

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <base/format.h>
#include <base/shared.h>  // CulebraError

#include <fstlib.h>

namespace culebra::fstdict {

// The output type `FST.IndexMap` is compiled for. uint32_t, not uint64_t,
// because cpp-fstlib's key-only "auto index" builder hard-codes uint32_t and a
// matcher rejects byte code whose header output type differs — one numeric
// class keeps `compile_auto_index` and `compile_index_map` loadable by the
// same `FST.IndexMap`.
using index_t = uint32_t;
inline constexpr int64_t kIndexMin = 0;
inline constexpr int64_t kIndexMax = 4294967295;  // index_t's range

// Query results, flat and value-neutral. `length` is a byte length into the
// subject, matching the offsets Regex hands back.
template <typename O> struct PrefixHit {
  size_t length;
  O value;
};
template <typename O> struct Entry {
  std::string key;
  O value;
};
template <typename O> struct Ranked {
  double ratio;
  std::string key;
  O value;
};
struct RankedKey {
  double ratio;
  std::string key;
};

// --- Shared guards -------------------------------------------------------

// fst::Result -> FSTError, naming the offending input position.
inline void _check_compile(std::pair<::fst::Result, size_t> r) {
  using R = ::fst::Result;
  if (r.first == R::Success) return;
  const char* what = r.first == R::EmptyKey     ? "empty key"
                     : r.first == R::UnsortedKey ? "key out of sorted order"
                                                 : "duplicate key";
  throw CulebraError("FSTError",
                     culebra::format("FST: {} at index {}", what, r.second), 0,
                     0);
}

// A matcher reports invalid both for corrupt bytes and for byte code built for
// a different output type, so this one guard is also the "Set byte code handed
// to FST.Map" check.
inline void _require_valid(bool ok, const char* what) {
  if (!ok) {
    throw CulebraError(
        "FSTError",
        culebra::format("FST: byte code is not a valid {}", what), 0, 0);
  }
}

template <typename O> inline const char* _map_kind() {
  if constexpr (std::is_same_v<O, std::string>) {
    return "Map";
  } else {
    return "IndexMap";
  }
}

// --- Building ------------------------------------------------------------

// Keys only, no value per key: loads as `FST.Set`.
inline std::string compile_set(const std::vector<std::string>& keys,
                               bool sorted) {
  std::ostringstream os;
  _check_compile(::fst::compile(keys, os, /*need_output=*/false, sorted));
  return os.str();
}

// Keys only, each key's value its 0-based rank in sorted order: loads as
// `FST.IndexMap`. This is cpp-fstlib's own "auto index" mode, which is why
// index_t is uint32_t.
inline std::string compile_auto_index(const std::vector<std::string>& keys,
                                      bool sorted) {
  std::ostringstream os;
  _check_compile(::fst::compile(keys, os, /*need_output=*/true, sorted));
  return os.str();
}

// Explicit value per key: `FST.Map` for O = std::string, `FST.IndexMap` for
// O = index_t.
template <typename O>
inline std::string compile_map(
    const std::vector<std::pair<std::string, O>>& entries, bool sorted) {
  std::ostringstream os;
  _check_compile(::fst::compile<O>(entries, os, sorted));
  return os.str();
}

// --- Querying: Set -------------------------------------------------------

inline void set_check(std::string_view bc) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
}

inline bool set_contains(std::string_view bc, std::string_view key) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  return m.contains(key);
}

inline std::vector<size_t> set_common_prefix_search(std::string_view bc,
                                                    std::string_view text) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  return m.common_prefix_search(text);
}

// 0 when nothing matches: a key is never empty (EmptyKey is a compile error),
// so a 0-length prefix hit cannot otherwise occur.
inline size_t set_longest_common_prefix_search(std::string_view bc,
                                               std::string_view text) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  return m.longest_common_prefix_search(text);
}

inline std::vector<std::string> set_predictive_search(std::string_view bc,
                                                      std::string_view prefix) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  return m.predictive_search(prefix);
}

inline std::vector<std::string> set_edit_distance_search(
    std::string_view bc, std::string_view word, size_t max_edits,
    size_t insert_cost, size_t delete_cost, size_t replace_cost) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  return m.edit_distance_search(word, max_edits, insert_cost, delete_cost,
                                replace_cost);
}

inline std::vector<RankedKey> set_suggest(std::string_view bc,
                                          std::string_view word) {
  ::fst::set m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), "Set");
  std::vector<RankedKey> out;
  for (auto& [ratio, key] : m.suggest(word)) {
    out.push_back({ratio, std::move(key)});
  }
  return out;
}

// --- Querying: Map / IndexMap --------------------------------------------
//
// One body per operation, instantiated for std::string (`FST.Map`) and
// index_t (`FST.IndexMap`); the binding layer picks the instantiation.

template <typename O> inline void map_check(std::string_view bc) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
}

// false (leaving `out` untouched) on a miss — `at` throws, which the culebra
// side spells as nil instead.
template <typename O>
inline bool map_get(std::string_view bc, std::string_view key, O& out) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  return m.exact_match_search(key, out);
}

template <typename O>
inline std::vector<PrefixHit<O>> map_common_prefix_search(
    std::string_view bc, std::string_view text) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  std::vector<PrefixHit<O>> out;
  for (auto& [length, value] : m.common_prefix_search(text)) {
    out.push_back({length, std::move(value)});
  }
  return out;
}

// 0 when nothing matches (see set_longest_common_prefix_search).
template <typename O>
inline size_t map_longest_common_prefix_search(std::string_view bc,
                                               std::string_view text, O& out) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  return m.longest_common_prefix_search(text, out);
}

template <typename O>
inline std::vector<Entry<O>> map_predictive_search(std::string_view bc,
                                                   std::string_view prefix) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  std::vector<Entry<O>> out;
  for (auto& [key, value] : m.predictive_search(prefix)) {
    out.push_back({std::move(key), std::move(value)});
  }
  return out;
}

template <typename O>
inline std::vector<Entry<O>> map_edit_distance_search(
    std::string_view bc, std::string_view word, size_t max_edits,
    size_t insert_cost, size_t delete_cost, size_t replace_cost) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  std::vector<Entry<O>> out;
  for (auto& [key, value] : m.edit_distance_search(
           word, max_edits, insert_cost, delete_cost, replace_cost)) {
    out.push_back({std::move(key), std::move(value)});
  }
  return out;
}

template <typename O>
inline std::vector<Ranked<O>> map_suggest(std::string_view bc,
                                          std::string_view word) {
  ::fst::map<O> m(bc.data(), bc.size());
  _require_valid(static_cast<bool>(m), _map_kind<O>());
  std::vector<Ranked<O>> out;
  for (auto& [ratio, key, value] : m.suggest(word)) {
    out.push_back({ratio, std::move(key), std::move(value)});
  }
  return out;
}

}  // namespace culebra::fstdict
