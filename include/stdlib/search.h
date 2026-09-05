#pragma once

// Value-neutral full-text search core for the Search namespace (the `_Search`
// native rows in stdlib_rt.h). Mirrors regex.h / sqlite.h: no Value / JitValue
// here — the binding layer adapts the handle ids and Hit records below into
// its own objects.
//
// Linkage partitioning, the regex.h choke applied to cpp-searchlib: the six
// engine sources are compiled once (CMake's culebra_searchlib objects) and
// bundled into the Search feature archive, so a binary that never names
// Search carries none of them.
//   - core archive   (CULEBRA_RT_SEARCH_WEAK):   weak throwing stubs; searchlib.h
//     is never even included, so the archive references no searchlib:: symbol.
//   - search archive (CULEBRA_RT_SEARCH_STRONG): strong real bodies, force-loaded
//     only when the AST scan reports Search use.
//   - header-only / in-process JIT (neither): the normal inline body.
//
// An index is a handle in a per-thread IdRegistry, like sqlite.h's tables, so
// a dropped or forged id fails safely; the binding layer keeps the handle
// non-sendable so it never crosses to an isolate's registry.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/shared.h>  // CulebraError

#if !defined(CULEBRA_RT_SEARCH_WEAK)
#include <searchlib.h>
#include <unicodelib.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

#include <base/id_registry.h>
#endif

#if defined(CULEBRA_RT_SEARCH_STRONG)
#define CULEBRA_RT_SEARCH_LINKAGE
#elif defined(CULEBRA_RT_SEARCH_WEAK)
#define CULEBRA_RT_SEARCH_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_SEARCH_LINKAGE inline
#endif

namespace culebra::search {

// One matched term's byte span in the indexed text.
struct Range {
  size_t position = 0;
  size_t length = 0;
};

// One document of a search result, best score first.
struct Hit {
  std::string key;
  double score = 0;
  std::vector<Range> ranges;
};

// ---- Index handles ---------------------------------------------------------
//
// Every function throws CulebraError("SearchError") on failure, with no
// position (the binding layer stamps one). Keys are caller-owned strings:
// adding under an existing key replaces that document.

CULEBRA_RT_SEARCH_LINKAGE int64_t index_new();
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(const std::string& path);
CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t id, const std::string& key,
                                         const std::string& text);
CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id, const std::string& key);
CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        const std::string& query,
                                                        size_t limit);
CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t id, const std::string& path);
// Frees the handle. Idempotent; a stale/forged id is ignored.
CULEBRA_RT_SEARCH_LINKAGE void index_drop(int64_t id);

#if defined(CULEBRA_RT_SEARCH_WEAK)

[[noreturn]] inline void _not_linked() {
  throw CulebraError("InternalError",
                     "Search runtime entered in a no-Search binary", 0, 0);
}
CULEBRA_RT_SEARCH_LINKAGE int64_t index_new() { _not_linked(); }
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(const std::string&) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t, const std::string&,
                                         const std::string&) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t, const std::string&) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t,
                                                        const std::string&,
                                                        size_t) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t, const std::string&) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_drop(int64_t) {}

#else  // the real bodies

namespace detail {

using Invidx = searchlib::InMemoryInvertedIndex<searchlib::TextRange, std::string>;
using Indexer = searchlib::InMemoryIndexer<searchlib::TextRange, std::string>;

inline std::u32string lowercase(const std::u32string& s) {
  return unicode::to_lowercase(s);
}

struct Index {
  Invidx invidx;
  Indexer indexer{invidx, lowercase};
};

// The registry belongs to the implementation, so the weak-stub branch leaves it
// out (see sqlite.h on why the same `inline thread_local` must not land in both
// the core and the feature archive).
inline thread_local IdRegistry<Index> g_indexes;

inline Index& index_get(int64_t id) {
  auto* p = g_indexes.get(id);
  if (!p) throw CulebraError("SearchError", "Search: index was dropped", 0, 0);
  return *p;
}

[[noreturn]] inline void rethrow(const std::exception& e) {
  throw CulebraError("SearchError", culebra::format("Search: {}", e.what()), 0,
                     0);
}

}  // namespace detail

CULEBRA_RT_SEARCH_LINKAGE int64_t index_new() {
  return detail::g_indexes.add(new detail::Index());
}

CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(const std::string& path) {
  auto index = std::make_unique<detail::Index>();
  try {
    index->invidx.load(path);
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
  return detail::g_indexes.add(index.release());
}

CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t id, const std::string& key,
                                         const std::string& text) {
  auto& index = detail::index_get(id);
  try {
    index.indexer.index_document(key,
                                 searchlib::UTF8PlainTextTokenizer(text));
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
}

CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id,
                                            const std::string& key) {
  detail::index_get(id).invidx.remove_document(key);
}

CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        const std::string& query,
                                                        size_t limit) {
  auto& invidx = detail::index_get(id).invidx;
  std::vector<Hit> hits;
  try {
    auto expr = searchlib::parse_query(detail::lowercase, query);
    if (!expr) {
      throw CulebraError("SearchError",
                         culebra::format("Search: invalid query: {}", query), 0,
                         0);
    }
    auto result = searchlib::perform_search(invidx, *expr);
    searchlib::BM25Scorer scorer(invidx, *expr);

    // Rank by score; a tie keeps index order, so results are deterministic.
    std::vector<std::pair<double, size_t>> order;
    order.reserve(result->size());
    for (size_t i = 0; i < result->size(); i++) {
      order.emplace_back(scorer(*result, i), i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) {
                       return a.first > b.first;
                     });
    if (order.size() > limit) order.resize(limit);

    hits.reserve(order.size());
    for (const auto& [score, i] : order) {
      Hit hit;
      hit.key = invidx.document_key(result->document_ordinal(i));
      hit.score = score;
      for (size_t h = 0; h < result->search_hit_count(i); h++) {
        auto r = invidx.text_range(*result, i, h);
        hit.ranges.push_back({r.position, r.length});
      }
      hits.push_back(std::move(hit));
    }
  } catch (const CulebraError&) {
    throw;
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
  return hits;
}

CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t id, const std::string& path) {
  auto& invidx = detail::index_get(id).invidx;
  try {
    invidx.save(path, {}, searchlib::IndexFormat::Compressed);
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
}

CULEBRA_RT_SEARCH_LINKAGE void index_drop(int64_t id) {
  auto* index = detail::g_indexes.get(id);
  if (!index) return;
  detail::g_indexes.invalidate(id);
  delete index;
}

#endif  // CULEBRA_RT_SEARCH_WEAK

}  // namespace culebra::search
