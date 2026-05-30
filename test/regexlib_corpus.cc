//
//  regexlib_corpus.cc
//
//  A portable semantic-regression gate: a corpus of (pattern, subject,
//  expected) cases, each adjudicated once against Python's `re` module (a
//  reliable leftmost-first oracle) and baked into regexlib_corpus_data.h.
//  regexlib must reproduce every expected result. Unlike the fuzzer's live
//  std::regex differential, this needs no runtime oracle, so it is portable
//  across platforms and standard libraries.
//
//  To regenerate after an intentional semantics change:
//    c++ -std=c++17 -Iinclude -Ivendor/cpp-unicodelib \
//        test/regexlib_corpus_gen.cc -o gen
//    ./gen | python3 test/regexlib_corpus_gen.py > test/regexlib_corpus_data.h
//

#include <cstdio>
#include <string>

#include "regexlib.h"
#include "regexlib_corpus_data.h"

int main() {
  int pass = 0, fail = 0;
  for (int i = 0; i < regexlib_corpus::kCaseCount; i++) {
    const auto &c = regexlib_corpus::kCases[i];
    std::string pat = c.pattern, subj = c.subject, expected = c.expected;
    bool ok = true;
    try {
      regexlib::Regex re(pat);
      auto m = re.search(subj);
      if (m.matched != c.matched) ok = false;
      else if (c.matched && m.str != expected) ok = false;
    } catch (const regexlib::RegexError &) {
      ok = false;
    }
    if (ok) {
      pass++;
    } else {
      fail++;
      if (fail <= 20)
        std::printf("  FAIL: /%s/ on \"%s\" (expected matched=%d \"%s\")\n",
                    pat.c_str(), subj.c_str(), c.matched, expected.c_str());
    }
  }
  std::printf("regexlib_corpus: %d passed, %d failed (of %d)\n", pass, fail,
              regexlib_corpus::kCaseCount);
  return fail == 0 ? 0 : 1;
}
