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
// The segmenter's model loader is partitioned once more, out of the search
// archive into its own (search_segmenter.h), so a program that searches
// without loading a model carries no cpp-segmentlib either.
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
#include <interop/search_splitter.h>  // ISplitter / SplitEmit, the third-party contract

#if !defined(CULEBRA_RT_SEARCH_WEAK)
#include <searchlib.h>
#include <unicodelib.h>

#include <stdlib/search_segmenter.h>

#include <cstring>
#include <filesystem>
#include <fstream>
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
  // consumed). Empty keeps the built-in splitting: UAX #29 word segments that
  // contain a letter or a number (searchlib's default).
  using Emit = SplitEmit;
  std::function<void(std::string_view text, const Emit &emit)> splitter;

  // Or a native one: a handle from segmenter_load, whose splitter is copied
  // into the index when it is built (so closing the handle later does not
  // affect the index). Negative is none -- ids start at 0. The binding layer
  // sets one of the three.
  int64_t segmenter = -1;

  // Or ISplitter::split itself, for a splitter implemented outside this repo
  // (interop/search_splitter.h; the binding decides how the instance behind
  // it is reached). Runs in the whole-text role on searchlib's delegation
  // path, which checks what it emits (searchlib's Segmenter contract) -- a
  // closure in `splitter` is not held to that.
  using NativeSplitter = std::function<size_t(std::string_view text,
                                              size_t offset, const Emit &emit)>;
  NativeSplitter native_splitter;

  // Applied in order after the normalizer. A filter rewrites its term in
  // place and returns true to keep it, or returns false to drop it (a stop
  // word). Turning one term into several is deliberately not expressible:
  // alternatives belong on the query side, and a *sequence* of pieces is the
  // splitter's job.
  std::vector<std::function<bool(std::string &)>> filters;

  // How this analyzer is shaped, as the caller describes it: written into a
  // saved index and compared when one is loaded, so that opening an index
  // with an analyzer of a different shape fails instead of quietly answering
  // from term boundaries nobody agrees on. The hooks themselves are
  // std::functions with no identity to record, so this is what the layer
  // above can say about them -- see the binding's analyzer_shape.
  std::string shape;
};

// ---- Index handles ---------------------------------------------------------
//
// Every function throws CulebraError("SearchError") on failure, with no
// position (the binding layer stamps one). Keys are caller-owned strings:
// adding under an existing key replaces that document.

// Views throughout, as regex.h and fst.h do: the engine tokenizes out of the
// caller's bytes, so a document need not be copied to be indexed.
CULEBRA_RT_SEARCH_LINKAGE int64_t index_new(Analyzer analyzer = {});
// `readonly` keeps the saved file's Elias-Fano postings and text ranges
// compressed in memory instead of expanding them into the mutable
// representation: opening is much cheaper and the index is much smaller,
// searching is somewhat slower, and add/remove/save are refused (see the
// numbers in docs/stdlib.md).
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(std::string_view path,
                                             Analyzer analyzer = {},
                                             bool readonly = false);
CULEBRA_RT_SEARCH_LINKAGE void index_add(int64_t id, std::string_view key,
                                         std::string_view text);
CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id, std::string_view key);
CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        std::string_view query,
                                                        size_t limit);
CULEBRA_RT_SEARCH_LINKAGE void index_save(int64_t id, std::string_view path);
// Frees the handle. Idempotent; a stale/forged id is ignored.
CULEBRA_RT_SEARCH_LINKAGE void index_drop(int64_t id);

// A word-segmentation model (cpp-segmentlib, any format it loads) as a
// splitter handle for Analyzer::segmenter: runs of Japanese script go to the
// model, everything else is cut by the built-in rules. Throws SearchError when
// the file cannot be loaded.
CULEBRA_RT_SEARCH_LINKAGE int64_t segmenter_load(std::string_view model_path);
// How a saved index names this segmenter's model (Analyzer::shape). Empty
// once the handle is closed.
CULEBRA_RT_SEARCH_LINKAGE std::string segmenter_model_tag(int64_t id);
// Frees the handle. Idempotent; a stale/forged id is ignored.
CULEBRA_RT_SEARCH_LINKAGE void segmenter_drop(int64_t id);

#if defined(CULEBRA_RT_SEARCH_WEAK)

[[noreturn]] inline void _not_linked() {
  throw CulebraError("InternalError",
                     "Search runtime entered in a no-Search binary", 0, 0);
}
CULEBRA_RT_SEARCH_LINKAGE int64_t index_new(Analyzer) { _not_linked(); }
CULEBRA_RT_SEARCH_LINKAGE int64_t index_load(std::string_view, Analyzer, bool) {
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
CULEBRA_RT_SEARCH_LINKAGE int64_t segmenter_load(std::string_view) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE std::string segmenter_model_tag(int64_t) {
  _not_linked();
}
CULEBRA_RT_SEARCH_LINKAGE void segmenter_drop(int64_t) {}

#else  // the real bodies

namespace detail {

using Invidx = searchlib::InMemoryInvertedIndex<searchlib::TextRange, std::string>;
using Indexer = searchlib::InMemoryIndexer<searchlib::TextRange, std::string>;

inline std::u32string lowercase(const std::u32string& s) {
  return unicode::to_lowercase(s);
}

// Loaded segmenters, by handle. Per-thread like g_indexes below, and for the
// same reason (see there).
// The splitter, plus how the model it holds is named in an index's shape: the
// file's own name and byte count. Not a digest -- that would mean reading the
// model again on every save -- so it tells two models apart by what a
// directory listing shows, which is the altitude a shape check works at.
struct Segmenter {
  searchlib::TextSplitter splitter;
  std::string model;
};

inline thread_local IdRegistry<Segmenter> g_segmenters;

inline const Segmenter& segmenter_entry(int64_t id) {
  auto* p = g_segmenters.get(id);
  if (!p) throw CulebraError("SearchError", "Search: segmenter was closed", 0, 0);
  return *p;
}

inline const searchlib::TextSplitter& segmenter_get(int64_t id) {
  return segmenter_entry(id).splitter;
}

// `name(size)` of a model file. A model that cannot be stat'd still loads, so
// an unknown size is a mark in the tag rather than an error.
inline std::string model_tag(std::string_view path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
  return culebra::format("{}({})",
                         std::filesystem::path(path).filename().string(),
                         ec ? std::string("?") : std::to_string(size));
}

// A splitter's emit in searchlib's terms. One conversion for every splitter
// shape, so the index side and the query side cannot disagree on it.
inline SplitEmit to_searchlib(const searchlib::SplitEmit &emit) {
  return [&emit](std::string_view term, size_t position, size_t length) {
    emit(searchlib::u32(term), searchlib::TextRange{position, length});
  };
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

    if (analyzer.segmenter >= 0) {
      splitter = segmenter_get(analyzer.segmenter);  // a copy: shares the model
    } else if (analyzer.native_splitter) {
      // Claiming every scalar hands the whole text over at its first one.
      splitter = searchlib::utf8_plain_text_splitter(
          [fn = analyzer.native_splitter](std::string_view text, size_t offset,
                                          const searchlib::SplitEmit &emit) {
            return fn(text, offset, to_searchlib(emit));
          },
          [](char32_t) { return true; });
    } else if (analyzer.splitter) {
      splitter = [fn = analyzer.splitter](std::string_view text,
                                          const searchlib::SplitEmit &emit) {
        fn(text, to_searchlib(emit));
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

// What every read goes through. Both backends implement it, so searching is
// one code path: the mutable InMemoryInvertedIndex, and the read-only one
// load_compressed_index returns.
using Reader = searchlib::IInvertedIndexWithTextRange<searchlib::TextRange,
                                                      std::string>;

// An index is either writable (it owns an Invidx, and an Indexer over it) or
// read-only (it holds the compressed backend, and no Indexer). `reader` points
// at whichever one it has, so index_search never asks which.
struct Index {
  std::unique_ptr<Invidx> invidx;      // null when read-only
  std::shared_ptr<Reader> compressed;  // null when writable
  const Reader *reader = nullptr;
  Pipeline pipeline;
  std::optional<Indexer> indexer;      // empty when read-only
  std::string shape;                   // what a save records, see write_envelope

  explicit Index(const Analyzer &analyzer)
      : invidx(std::make_unique<Invidx>()), pipeline(analyzer),
        shape(analyzer.shape) {
    reader = invidx.get();
    indexer.emplace(*invidx, pipeline.normalizer);
  }

  // The read-only shape: the backend is loaded before the handle exists, so a
  // failed load never leaves a half-built index behind.
  Index(const Analyzer &analyzer, std::shared_ptr<Reader> backend)
      : compressed(std::move(backend)), reader(compressed.get()),
        pipeline(analyzer), shape(analyzer.shape) {}

  bool writable() const { return invidx != nullptr; }
};

// The one refusal a read-only index gives, worded once so add, remove and
// save cannot drift apart.
[[noreturn]] inline void throw_readonly(const char *what) {
  throw CulebraError(
      "SearchError",
      culebra::format("Search: {} needs a writable index, and this one was "
                      "loaded with readonly: true",
                      what),
      0, 0);
}

// The registry belongs to the implementation, so the weak-stub branch leaves it
// out (see sqlite.h on why the same `inline thread_local` must not land in both
// the core and the feature archive).
inline thread_local IdRegistry<Index> g_indexes;

inline Index& index_get(int64_t id) {
  auto* p = g_indexes.get(id);
  if (!p) throw CulebraError("SearchError", "Search: index was dropped", 0, 0);
  return *p;
}

// A saved index is culebra's envelope around the engine's bytes: the analyzer
// is culebra's idea, not searchlib's, and the file has to carry it or loading
// with the wrong one stays undetectable. The engine's own save/load take a
// stream, so the envelope is read off the front and the rest handed over
// untouched.
inline constexpr char kEnvelopeMagic[8] = {'c', 'u', 'l', 's', 'e', 'a', 'r', '\0'};
// The envelope's own version, apart from the magic so that a file this build
// cannot read is told apart from a file that is not an index at all -- the
// two need different answers, and only one of them is the user's mistake.
inline constexpr uint32_t kEnvelopeVersion = 1;

inline void write_envelope(std::ostream &os, const std::string &shape) {
  os.write(kEnvelopeMagic, sizeof(kEnvelopeMagic));
  auto version = kEnvelopeVersion;
  os.write(reinterpret_cast<const char *>(&version), sizeof(version));
  auto n = static_cast<uint32_t>(shape.size());
  os.write(reinterpret_cast<const char *>(&n), sizeof(n));
  os.write(shape.data(), std::streamsize(shape.size()));
}

// The shape the file was written with. Throws if this is not an index file
// this build reads.
inline std::string read_envelope(std::istream &is, std::string_view path) {
  char magic[sizeof(kEnvelopeMagic)];
  is.read(magic, sizeof(magic));
  if (!is || std::memcmp(magic, kEnvelopeMagic, sizeof(magic)) != 0) {
    throw CulebraError(
        "SearchError",
        culebra::format("Search: {} is not a culebra index file", path), 0, 0);
  }
  uint32_t version = 0;
  is.read(reinterpret_cast<char *>(&version), sizeof(version));
  if (!is || version != kEnvelopeVersion) {
    throw CulebraError(
        "SearchError",
        culebra::format("Search: {} is a version {} index and this culebra "
                        "writes version {}; rebuild it",
                        path, version, kEnvelopeVersion),
        0, 0);
  }
  uint32_t n = 0;
  is.read(reinterpret_cast<char *>(&n), sizeof(n));
  std::string shape(n, '\0');
  if (n) is.read(shape.data(), std::streamsize(n));
  if (!is) {
    throw CulebraError(
        "SearchError",
        culebra::format("Search: {} ends mid-header", path), 0, 0);
  }
  return shape;
}

// What a splitter cut into terms is what a query has to be cut by, so an
// index opened with an analyzer shaped differently from the one that built it
// answers from boundaries the two sides do not share. The shapes are coarse
// (a Culebra closure has no identity to record, so every closure reads the
// same) -- this catches the mismatches that have one, not every one.
inline void check_shape(std::string_view want, std::string_view got,
                        std::string_view path) {
  if (want == got) return;
  throw CulebraError(
      "SearchError",
      culebra::format("Search: {} was built with a different analyzer "
                      "(saved {}, opened with {}); an index has to be searched "
                      "with the analyzer that built it",
                      path, want.empty() ? "none" : want,
                      got.empty() ? "none" : got),
      0, 0);
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
                                             Analyzer analyzer, bool readonly) {
  // The saved form records nothing about the analyzer, so loading with one
  // that differs from the index's own gives quietly wrong results. Nothing
  // here can check that; the docs say so instead.
  std::unique_ptr<detail::Index> index;
  try {
    std::ifstream is(std::string(path), std::ios::binary);
    if (!is) {
      throw CulebraError("SearchError",
                         culebra::format("Search: cannot read {}", path), 0, 0);
    }
    detail::check_shape(detail::read_envelope(is, path), analyzer.shape, path);
    if (readonly) {
      index = std::make_unique<detail::Index>(
          analyzer, searchlib::load_compressed_index<std::string>(is));
    } else {
      index = std::make_unique<detail::Index>(analyzer);
      index->invidx->load(is);
    }
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
  if (!index.writable()) detail::throw_readonly("add");
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
    index.indexer->index_document(
        std::string(key), searchlib::Analyzer<searchlib::TextRange>(
                              std::move(tokenizer), pipeline.filter));
  } catch (const CulebraError&) {
    throw;  // an analyzer hook's own error, already shaped
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
}

CULEBRA_RT_SEARCH_LINKAGE void index_remove(int64_t id, std::string_view key) {
  auto& index = detail::index_get(id);
  if (!index.writable()) detail::throw_readonly("remove");
  index.invidx->remove_document(std::string(key));
}

CULEBRA_RT_SEARCH_LINKAGE std::vector<Hit> index_search(int64_t id,
                                                        std::string_view query,
                                                        size_t limit) {
  auto& index = detail::index_get(id);
  const auto& invidx = *index.reader;
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
  auto& index = detail::index_get(id);
  if (!index.writable()) detail::throw_readonly("save");
  try {
    std::ofstream os(std::string(path), std::ios::binary);
    if (!os) {
      throw CulebraError(
          "SearchError",
          culebra::format("Search: cannot write {}", path), 0, 0);
    }
    detail::write_envelope(os, index.shape);
    index.invidx->save(os, {}, searchlib::IndexFormat::Compressed);
    os.flush();
    if (!os) {
      throw CulebraError(
          "SearchError",
          culebra::format("Search: could not finish writing {}", path), 0, 0);
    }
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

CULEBRA_RT_SEARCH_LINKAGE int64_t segmenter_load(std::string_view model_path) {
  try {
    return detail::g_segmenters.add(new detail::Segmenter{
        segmenter_open(model_path), detail::model_tag(model_path)});
  } catch (const std::exception& e) {
    detail::rethrow(e);
  }
}

// How an index built with this segmenter names it in its shape (see
// Analyzer::shape). Empty for a handle that is already closed: the analyzer
// it would have described cannot be built either.
CULEBRA_RT_SEARCH_LINKAGE std::string segmenter_model_tag(int64_t id) {
  auto* p = detail::g_segmenters.get(id);
  return p ? p->model : std::string();
}

CULEBRA_RT_SEARCH_LINKAGE void segmenter_drop(int64_t id) {
  auto* entry = detail::g_segmenters.get(id);
  if (!entry) return;
  detail::g_segmenters.invalidate(id);
  delete entry;
}

#endif  // CULEBRA_RT_SEARCH_WEAK

}  // namespace culebra::search
