// Compiled and run by check_search_splitter_standalone.sh with `-I include`
// and nothing else on the include path.
//
// Two claims, and the reason each is checked by compiling rather than by
// reading. First, stdlib/search_splitter.h is the whole contract a splitter
// implements: a library outside this repo has to be able to include it and
// nothing else, which is a property no other gate would notice losing the day
// search.h starts pulling searchlib or the runtime in behind it. Second, the
// contract carries what a real morphological analyzer needs, which is not
// obvious from its four lines -- so what is implemented below is the shape
// measured against Kiwi (Korean), the case that is furthest from the built-in
// splitting:
//
//   - the analyzer speaks UTF-16 code units, the contract speaks UTF-8 bytes
//   - it answers with base forms, so a term is routinely not the bytes its
//     range points at (22.3% of tokens on a 4400-sentence corpus)
//   - its grammatical morphemes overlap the one before them (6.80% of all
//     tokens) and are also the ones an index does not want, so the
//     part-of-speech filter makes the ranges disjoint
//
// No Kiwi here: the analyses replayed below were produced by it, and the point
// is only that the interface has room for the adapter, not that the adapter
// exists yet.

#include <stdlib/search_splitter.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// One morpheme as a UTF-16-based analyzer reports it.
struct Token {
  size_t position;  // in UTF-16 code units
  size_t length;
  std::string form;  // may be a base form, unlike the text it points at
  bool content;      // what a part-of-speech filter would keep
};

// UTF-16 index -> UTF-8 byte offset, built in one pass over the text. Both
// units of a surrogate pair map to the start of their 4-byte sequence, so a
// range whose ends fall on character boundaries converts exactly.
class OffsetMap {
public:
  explicit OffsetMap(std::string_view text) {
    size_t at = 0;
    while (at < text.size()) {
      auto lead = static_cast<unsigned char>(text[at]);
      size_t width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      if (at + width > text.size()) {
        width = 1;  // truncated: take the byte, stay in step
      }
      bytes_.push_back(at);
      if (width == 4) {
        bytes_.push_back(at);  // the pair's low surrogate
      }
      at += width;
    }
    bytes_.push_back(text.size());
  }

  size_t byte_at(size_t u16_index) const {
    return bytes_[u16_index < bytes_.size() ? u16_index : bytes_.size() - 1];
  }

private:
  std::vector<size_t> bytes_;
};

// The adapter shape: convert offsets, drop what the contract forbids, emit.
class MorphologicalSplitter : public culebra::search::ISplitter {
public:
  explicit MorphologicalSplitter(std::vector<Token> analysis)
      : analysis_(std::move(analysis)) {}

  // A whole-text splitter: analyzes from `offset` to the end and says so.
  size_t split(std::string_view text, size_t offset,
               const culebra::search::SplitEmit &emit) const override {
    auto rest = text.substr(offset);
    OffsetMap map(rest);
    size_t previous_end = 0;
    for (const auto &token : analysis_) {
      if (!token.content || token.length == 0) {
        continue;
      }
      auto begin = map.byte_at(token.position);
      auto end = map.byte_at(token.position + token.length);
      if (end <= begin || begin < previous_end) {
        continue;
      }
      emit(token.form, offset + begin, end - begin);
      previous_end = end;
    }
    return rest.size();
  }

private:
  std::vector<Token> analysis_;
};

struct Emitted {
  std::string term;
  size_t position;
  size_t length;
};

std::vector<Emitted> run(const culebra::search::ISplitter &splitter,
                         std::string_view text, size_t offset = 0) {
  std::vector<Emitted> out;
  auto consumed =
      splitter.split(text, offset, [&](std::string_view term, size_t position,
                                       size_t length) {
        out.push_back({std::string(term), position, length});
      });
  assert(consumed == text.size() - offset);
  (void)consumed;
  return out;
}

}  // namespace

int main() {
  // 도와요 -> 돕/VV + 어요/EF. The base form 돕 does not occur in the text, so
  // the term is not the bytes its range points at -- the case the built-in
  // splitting cannot express and the reason the contract only requires the
  // range to be true.
  {
    std::string text = "도와요";
    MorphologicalSplitter splitter({{0, 1, "돕", true}, {1, 2, "어요", true}});
    auto terms = run(splitter, text);
    assert(terms.size() == 2);
    assert(terms[0].term == "돕");
    assert(text.substr(terms[0].position, terms[0].length) == "도");
    assert(terms[0].term != text.substr(terms[0].position, terms[0].length));
    assert(terms[1].term == "어요");
    assert(text.substr(terms[1].position, terms[1].length) == "와요");
  }

  // 학교에서 공부한다, with the grammatical morphemes filtered out. 하/XSV and
  // ᆫ다/EF share a syllable; keeping them would overlap, and dropping them is
  // what an index wants anyway.
  {
    std::string text = "학교에서 공부한다";
    MorphologicalSplitter splitter({{0, 2, "학교", true},
                                    {2, 2, "에서", false},
                                    {5, 2, "공부", true},
                                    {7, 1, "하", false},
                                    {7, 2, "ᆫ다", false}});
    auto terms = run(splitter, text);
    assert(terms.size() == 2);
    assert(text.substr(terms[0].position, terms[0].length) == "학교");
    assert(text.substr(terms[1].position, terms[1].length) == "공부");
    // Disjoint and increasing, which is the rule the index cannot check.
    assert(terms[0].position + terms[0].length <= terms[1].position);
  }

  // An overlapping content token is dropped rather than trusted: a splitter
  // that breaks the rule corrupts highlighting silently.
  {
    std::string text = "갔다";
    MorphologicalSplitter splitter({{0, 1, "가", true}, {0, 1, "었", true}});
    auto terms = run(splitter, text);
    assert(terms.size() == 1);
    assert(terms[0].term == "가");
  }

  // Text outside the Basic Multilingual Plane: a surrogate pair is two UTF-16
  // units and four UTF-8 bytes, and the map has to agree.
  {
    std::string text = "🍣寿司";  // 2 units + 1 + 1
    MorphologicalSplitter splitter({{2, 2, "寿司", true}});
    auto terms = run(splitter, text);
    assert(terms.size() == 1);
    assert(text.substr(terms[0].position, terms[0].length) == "寿司");
  }

  // Called mid-text, as a segmenter is: the offsets come back in the whole
  // text's terms and the consumed length is what was left.
  {
    std::string text = "Tokyo 도와요";
    MorphologicalSplitter splitter({{0, 1, "돕", true}, {1, 2, "어요", true}});
    auto terms = run(splitter, text, 6);
    assert(terms.size() == 2);
    assert(text.substr(terms[0].position, terms[0].length) == "도");
    assert(text.substr(terms[1].position, terms[1].length) == "와요");
  }

  std::printf("search-splitter OK (standalone header, UTF-16 adapter shape)\n");
  return 0;
}
