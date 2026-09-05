#pragma once
// The contract a text splitter implements — on its own, so that a library
// outside this repo can implement one while including nothing else. searchlib,
// the runtime, the value representation and the rest of the standard library
// all stay out; <functional> and <string_view> are the whole dependency, and
// tools/checks/check_search_splitter_standalone.sh holds them there.
//
// A splitter cuts text into the terms an index holds, each with the byte range
// it came from. The same one runs when a document is added and when a query is
// parsed — an index built with one set of term boundaries and searched with
// another finds nothing — so it has to be deterministic, and it has to be
// callable from several threads at once: an index copies it freely, and a
// writer may be indexing while readers search. Whatever state it needs is
// built when it is loaded, not while it runs.
//
// Two rules the index depends on and cannot check:
//
//   - Ranges must not overlap and must increase. One term position carries one
//     text range, so two terms over the same bytes make highlighting answer
//     with a neighbour's range.
//
//   - The emitted term need not be the bytes its range points at, and only the
//     range has to be true. A morphological analyzer indexes a base form while
//     the range still points at the surface it was recovered from, so a search
//     for the base form highlights the text as written.
//
// Offsets are UTF-8 byte offsets into the `text` handed in. An analyzer that
// works in UTF-16 converts on the way out; that is its adapter's job, not this
// interface's, and the two rules above are usually what the adapter has to
// work for — a morphological analyzer's grammatical morphemes routinely share
// a syllable with the one before them, and are also the ones an index does not
// want, so the part-of-speech filter such an index needs anyway is what makes
// the ranges disjoint.

#include <cstddef>
#include <functional>
#include <string_view>

namespace culebra::search {

// One term, and the byte range in the text it was cut from.
using SplitEmit =
    std::function<void(std::string_view term, size_t position, size_t length)>;

class ISplitter {
public:
  virtual ~ISplitter() = default;

  // Calls emit once per term, in increasing range order.
  virtual void split(std::string_view text, const SplitEmit &emit) const = 0;
};

}  // namespace culebra::search
