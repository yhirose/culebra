#pragma once

// Value-neutral full-text search core for the Search namespace (the `Search`
// native rows in stdlib_rt.h). Mirrors regex.h / sqlite.h: no Value / JitValue
// here — the binding layer adapts the handle ids and Hit records below into
// its own objects.
//
// Linkage partitioning, the regex.h choke applied to cpp-searchlib: the engine
// is one header, compiled once into the Search feature archive, so a binary
// that never names Search carries none of it.
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
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <base/shared.h>  // CulebraError
#include <stdlib/search_splitter.h>  // ISplitter / SplitEmit, the third-party contract

#if !defined(CULEBRA_RT_SEARCH_WEAK)
#include <searchlib.h>
#include <unicodelib.h>

#include <memory>
#include <optional>
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

// ---- Analyzer --------------------------------------------------------------
//
// How text is cut into terms and how each term is normalized. The index side
// and the query side have to agree or a search quietly finds nothing, so an
// index holds one analyzer and applies it to both.
//
// Value-neutral like the rest of this header: plain callables over UTF-8
// bytes, with the binding layer supplying whatever trampolines into a culebra
// closure. Every hook left empty keeps the built-in behaviour, which is also
// the fast one -- a closure here is a VM round trip per token.
struct Analyzer {
  // Called once per term, before the filters. Empty keeps the native
  // lowercase default: a normalizer runs on every token of every document,
  // so making the default anything else would charge every program for it.
  std::function<std::string(std::string_view)> normalizer;

  // Cuts the whole text into terms, each with the byte range it came from —
  // ISplitter's contract in its whole-text role (offset 0, everything
  // consumed), so a native splitter can stand where a closure goes once the
  // binding that lifts one exists. Empty keeps the built-in splitting: UAX #29
  // word segments that contain a letter or a number (searchlib's default).
  using Emit = SplitEmit;
  std::function<void(std::string_view text, const Emit &emit)> splitter;

  // Applied in order after the normalizer. A filter rewrites its term in
  // place and returns true to keep it, or returns false to drop it (a stop
  // word). Turning one term into several is deliberately not expressible:
  // alternatives belong on the query side, and a *sequence* of pieces is the
  // splitter's job.
  std::vector<std::function<bool(std::string &)>> filters;
};

// ---- Index handles ---------------------------------------------------------
//
// Every function throws CulebraError("SearchError") on failure, with no
// position (the binding layer stamps one). Keys are caller-owned strings:
// adding under an existing key replaces that document.

// Views throughout, as regex.h and fst.h do: the engine tokenizes out of the
// caller's bytes, so a document need not be copied to be indexed.
CULEBRA_RT_SEARCH_LINKAGE int64_t index_new(Analyzer analyzer = {});
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(std::string_view path,
                                             Analyzer analyzer = {});
CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t id, std::string_view key,
                                         std::string_view text);
CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id, std::string_view key);
CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        std::string_view query,
                                                        size_t limit);
CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t id, std::string_view path);
// Frees the handle. Idempotent; a stale/forged id is ignored.
CULEBRA_RT_SEARCH_LINKAGE void index_drop(int64_t id);

#if defined(CULEBRA_RT_SEARCH_WEAK)

[[noreturn]] inline void _not_linked() {
  throw CulebraError("InternalError",
                     "Search runtime entered in a no-Search binary", 0, 0);
}
CULEBRA_RT_SEARCH_LINKAGE int64_t index_new(Analyzer) { _not_linked(); }
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(std::string_view, Analyzer) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t, std::string_view,
                                         std::string_view) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t, std::string_view) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t,
                                                        std::string_view,
                                                        size_t) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t, std::string_view) {
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

// The analyzer as searchlib's own three shapes. Built once per index, because
// index-time and query-time have to be the same objects for the two sides to
// agree on term boundaries.
struct Pipeline {
  searchlib::Normalizer normalizer;
  searchlib::TextSplitter splitter;  // null means the built-in splitting
  searchlib::TermFilter filter;      // null means no filtering
  searchlib::TermFilter query_filter;

  explicit Pipeline(const Analyzer &analyzer) {
    normalizer = analyzer.normalizer
                     ? searchlib::Normalizer(
                           [fn = analyzer.normalizer](const std::u32string &s) {
                             return searchlib::u32(fn(searchlib::u8(s)));
                           })
                     : searchlib::Normalizer(lowercase);

    if (analyzer.splitter) {
      splitter = [fn = analyzer.splitter](
                     std::string_view text,
                     const std::function<void(const std::u32string &,
                                              searchlib::TextRange)> &emit) {
        fn(text, [&](std::string_view term, size_t position, size_t length) {
          emit(searchlib::u32(term), searchlib::TextRange{position, length});
        });
      };
    }

    if (!analyzer.filters.empty()) {
      filter = [fns = analyzer.filters](
                   const std::u32string &s,
                   std::function<void(std::u32string)> emit) {
        auto term = searchlib::u8(s);
        for (const auto &fn : fns) {
          if (!fn(term)) {
            return;  // a stop word, and the position gap closes behind it
          }
        }
        emit(searchlib::u32(term));
      };
    }

    // The index side gets its normalizer through the indexer (searchlib
    // applies it as stage 0) and its filters through Analyzer<T>. parse_query
    // has no normalizer parameter, so the query side gets the same two as one
    // chain, in the same order.
    query_filter = filter
                       ? searchlib::compose({
                             searchlib::to_term_filter(normalizer),
                             filter,
                         })
                       : searchlib::to_term_filter(normalizer);
  }
};

struct Index {
  Invidx invidx;
  Pipeline pipeline;
  Indexer indexer;

  explicit Index(const Analyzer &analyzer)
      : pipeline(analyzer), indexer(invidx, pipeline.normalizer) {}
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

// searchlib prefixes its own messages with "searchlib: "; drop it so a
// surfaced error reads with one prefix, the namespace's own.
[[noreturn]] inline void rethrow(const std::exception& e) {
  std::string_view what = e.what();
  constexpr std::string_view kPrefix = "searchlib: ";
  if (what.substr(0, kPrefix.size()) == kPrefix) what.remove_prefix(kPrefix.size());
  throw CulebraError("SearchError", culebra::format("Search: {}", what), 0, 0);
}

}  // namespace detail

CULEBRA_RT_SEARCH_LINKAGE int64_t index_new(Analyzer analyzer) {
  return detail::g_indexes.add(new detail::Index(analyzer));
}

CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(std::string_view path,
                                             Analyzer analyzer) {
  // The saved form records nothing about the analyzer, so loading with one
  // that differs from the index's own gives quietly wrong results. Nothing
  // here can check that; the docs say so instead.
  auto index = std::make_unique<detail::Index>(analyzer);
  try {
    index->invidx.load(std::string(path));
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
  return detail::g_indexes.add(index.release());
}

CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t id, std::string_view key,
                                         std::string_view text) {
  auto& index = detail::index_get(id);
  try {
    // The key is copied because the index owns it from here on; the text is
    // not, since the tokenizer only reads it.
    const auto &pipeline = index.pipeline;
    searchlib::Tokenizer<searchlib::TextRange> tokenizer =
        pipeline.splitter
            ? searchlib::Tokenizer<searchlib::TextRange>(
                  searchlib::SplitterTokenizer(pipeline.splitter, text))
            : searchlib::Tokenizer<searchlib::TextRange>(
                  searchlib::UTF8PlainTextTokenizer(text));
    index.indexer.index_document(
        std::string(key), searchlib::Analyzer<searchlib::TextRange>(
                              std::move(tokenizer), pipeline.filter));
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
}

CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id, std::string_view key) {
  detail::index_get(id).invidx.remove_document(std::string(key));
}

CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        std::string_view query,
                                                        size_t limit) {
  auto& index = detail::index_get(id);
  auto& invidx = index.invidx;
  // Parsed on its own so that the malformed-query error below is raised
  // outside the catch that wraps engine failures — it is already a
  // CulebraError and must not be re-wrapped.
  std::optional<searchlib::Expression> expr;
  try {
    expr = searchlib::parse_query(index.pipeline.splitter,
                                 index.pipeline.query_filter, query);
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
  if (!expr) {
    throw CulebraError("SearchError",
                       culebra::format("Search: invalid query: {}", query), 0,
                       0);
  }
  std::vector<Hit> hits;
  try {
    auto result = searchlib::perform_search(invidx, *expr);
    searchlib::BM25Scorer scorer(invidx, *expr);
    // top_k scores each entry once and keeps only `limit` of them, rather than
    // sorting a whole postings list to throw most of it away.
    auto ranked = searchlib::top_k(*result, limit,
                                   [&](size_t i) { return scorer(*result, i); });
    hits.reserve(ranked.size());
    for (const auto& scored : ranked) {
      Hit& hit = hits.emplace_back();
      hit.key = invidx.document_key(result->document_ordinal(scored.index));
      hit.score = scored.score;
      size_t range_count = result->search_hit_count(scored.index);
      hit.ranges.reserve(range_count);
      for (size_t h = 0; h < range_count; h++) {
        auto r = invidx.text_range(*result, scored.index, h);
        hit.ranges.push_back({r.position, r.length});
      }
    }
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
  return hits;
}

CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t id, std::string_view path) {
  auto& invidx = detail::index_get(id).invidx;
  try {
    invidx.save(std::string(path), {}, searchlib::IndexFormat::Compressed);
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
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
