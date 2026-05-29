//
//  regexlib_test.cc
//
//  Self-contained test suite for include/regexlib.h. Depends only on
//  regexlib.h and cpp-unicodelib, so it travels with the header when it is
//  extracted to the standalone yhirose/cpp-regexlib repository.
//
//  Build (standalone):
//    c++ -std=c++17 -Iinclude -Ivendor/cpp-unicodelib \
//        test/regexlib_test.cc -o regexlib_test && ./regexlib_test
//  Or via CMake/ctest: `ctest -R regexlib_test`.
//

#include <chrono>
#include <cstdio>
#include <string>

#include "regexlib.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const char *expr, int line) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    std::printf("  FAIL (line %d): %s\n", line, expr);
  }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

using regexlib::Regex;
using regexlib::RegexError;

// Returns true if constructing the pattern throws RegexError.
bool rejects(const std::string &pat) {
  try {
    Regex r(pat);
    (void)r;
  } catch (const RegexError &) {
    return true;
  }
  return false;
}

void test_literals_and_anchors() {
  CHECK(Regex("abc").test("xxabcxx"));
  CHECK(!Regex("abc").test("ab"));
  CHECK(Regex("^abc$").test("abc"));
  CHECK(!Regex("^abc$").test("abcd"));
  CHECK(!Regex("^abc$").test("zabc"));
  CHECK(Regex("").test(""));        // empty pattern matches empty
  CHECK(Regex("").test("x"));       // and the empty prefix of anything
}

void test_any_and_classes() {
  CHECK(Regex("a.c").test("axc"));
  CHECK(!Regex("a.c").test("a\nc"));  // . excludes newline
  CHECK(Regex("[a-z]+").test("hello"));
  CHECK(!Regex("^[a-z]+$").test("Hello"));
  CHECK(Regex("[^0-9]+").test("abc"));
  CHECK(Regex("[A-Za-z_][A-Za-z0-9_]*").search("_id42").str == "_id42");
  CHECK(Regex("[-a-z]").test("-"));          // leading '-' is literal
  CHECK(Regex("[a\\]b]").test("]"));         // escaped ']' in class
  CHECK(Regex("\\d+").search("year 2026").str == "2026");
  CHECK(Regex("\\w+").search("snake_case99").str == "snake_case99");
  CHECK(Regex("a\\sb").test("a b"));
  CHECK(Regex("\\D+").search("abc123").str == "abc");
  CHECK(Regex("\\S+").search("  hi  ").str == "hi");
}

void test_quantifiers() {
  CHECK(Regex("ab*c").test("ac"));
  CHECK(Regex("ab*c").test("abbbbc"));
  CHECK(Regex("ab+c").test("abc"));
  CHECK(!Regex("ab+c").test("ac"));
  CHECK(Regex("ab?c").test("ac"));
  CHECK(Regex("ab?c").test("abc"));
  CHECK(Regex("^a{2,3}$").test("aa"));
  CHECK(Regex("^a{2,3}$").test("aaa"));
  CHECK(!Regex("^a{2,3}$").test("a"));
  CHECK(!Regex("^a{2,3}$").test("aaaa"));
  CHECK(Regex("^a{2}$").test("aa"));
  CHECK(Regex("^a{2,}$").test("aaaaa"));
  CHECK(Regex("a{0}b").test("b"));   // zero repetitions
  // greedy vs lazy
  CHECK(Regex("a.*b").search("a_b_b").str == "a_b_b");
  CHECK(Regex("a.*?b").search("a_b_b").str == "a_b");
  CHECK(Regex("<.+?>").search("<a><b>").str == "<a>");
  // a literal '{' that is not a valid counted quantifier
  CHECK(Regex("a{b").test("a{b"));
}

void test_alternation_and_groups() {
  CHECK(Regex("cat|dog|bird").test("I have a dog"));
  CHECK(!Regex("cat|dog").test("fish"));
  CHECK(Regex("(ab)+").search("ababab").str == "ababab");
  CHECK(Regex("(?:ab)+c").test("ababc"));  // non-capturing
  auto m = Regex("(\\d{4})-(\\d{2})-(\\d{2})").search("date: 2026-05-29!");
  CHECK(m);
  CHECK(m.str == "2026-05-29");
  CHECK(m.group(1).str == "2026");
  CHECK(m.group(2).str == "05");
  CHECK(m.group(3).str == "29");
  CHECK(m.group(4).matched == false);  // no such group
  // unmatched optional group
  auto m2 = Regex("(a)(b)?").search("a");
  CHECK(m2 && m2.group(1).str == "a" && !m2.group(2).matched);
}

void test_named_groups() {
  auto m = Regex("(?<year>\\d{4})-(?<mon>\\d{2})").search("2026-05");
  CHECK(m);
  CHECK(m.group("year").str == "2026");
  CHECK(m.group("mon").str == "05");
  CHECK(m.group("nope").matched == false);
  CHECK(Regex("(?'q'\\d+)").search("x42").group("q").str == "42");  // (?'name')
}

void test_boundaries_and_flags() {
  CHECK(Regex("\\bcat\\b").test("a cat sat"));
  CHECK(!Regex("\\bcat\\b").test("category"));
  CHECK(Regex("\\Bcat\\B").test("locate"));
  CHECK(!Regex("\\Bcat\\B").test("a cat"));
  CHECK(Regex("(?i)hello").test("HELLO World"));
  CHECK(Regex("(?i)[a-z]+").test("ABC"));
  CHECK(Regex("(?i)STRASSE").test("strasse"));
  CHECK(Regex("(?m)^bar$").test("foo\nbar\nbaz"));
  CHECK(!Regex("^bar$").test("foo\nbar\nbaz"));  // without (?m)
}

void test_find_all_and_replace() {
  auto ms = Regex("\\d+").find_all("a1 bb22 ccc333");
  CHECK(ms.size() == 3);
  if (ms.size() == 3) {
    CHECK(ms[0].str == "1");
    CHECK(ms[1].str == "22");
    CHECK(ms[2].str == "333");
  }
  // captures preserved across many matches
  auto ms2 = Regex("(\\w)(\\d)").find_all("a1 b2 c3 d4");
  CHECK(ms2.size() == 4);
  if (ms2.size() == 4) {
    CHECK(ms2[0].group(1).str == "a" && ms2[0].group(2).str == "1");
    CHECK(ms2[3].group(1).str == "d" && ms2[3].group(2).str == "4");
  }
  // empty-match handling must still advance
  auto ms3 = Regex("a*").find_all("aXaa");
  CHECK(!ms3.empty());
  CHECK(Regex("\\d+").replace_all("a1 b22 c333", "#") == "a# b# c#");
  CHECK(Regex("(\\w+)@(\\w+)").replace_all("x@y", "$2.$1") == "y.x");
  CHECK(Regex("(?<g>\\d+)").replace_all("a12b", "[$<g>]") == "a[12]b");
  CHECK(Regex("o").replace_all("foo", "$$") == "f$$");  // $$ -> literal $
}

void test_byte_offsets() {
  // é is two bytes in UTF-8, so the match starts at byte offset 2.
  auto m = Regex("café").search("a café here");
  CHECK(m);
  CHECK(m.begin == 2);
  CHECK(m.str == "café");
  CHECK(m.end == m.begin + std::string("café").size());
}

void test_grapheme_aware() {
  // A family emoji is one extended grapheme cluster (several code points).
  std::string family = "👨‍👩‍👧‍👦";
  CHECK(Regex(".").search(family).str == family);    // one '.' consumes it
  CHECK(Regex("^.$").search(family).str == family);

  // base 'e' + combining acute is a single grapheme.
  std::string combined = "e\xCC\x81";
  CHECK(Regex("^.$").search(combined).str == combined);

  // ".." consumes exactly two clusters.
  std::string two = family + family;
  CHECK(Regex("^..$").search(two).str == two);
  CHECK(Regex(".").search(two).str == family);  // first cluster only

  // A character class only matches a single-code-point grapheme.
  CHECK(!Regex("[a-z]").test(family));
}

void test_lookahead() {
  CHECK(Regex("\\d+(?= USD)").search("price 42 USD").str == "42");
  CHECK(Regex("foo(?=bar)").test("foobar"));
  CHECK(!Regex("foo(?=bar)").test("foobaz"));
  CHECK(Regex("foo(?!bar)").test("foobaz"));
  CHECK(!Regex("foo(?!bar)").test("foobar"));
  // Perl leftmost-first: \d+ backtracks to a shorter match the lookahead allows
  CHECK(Regex("\\d+(?! USD)").search("42 USD").str == "4");
  // variable-length lookahead body
  CHECK(Regex("\\w+(?=\\s*;)").search("name   ;").str == "name");
}

void test_unicode_properties() {
  CHECK(Regex("\\p{L}+").test("héllo"));
  CHECK(Regex("^\\p{L}+$").test("café"));
  CHECK(!Regex("^\\p{L}+$").test("ab12"));
  CHECK(Regex("\\p{N}+").search("count 2026").str == "2026");
  CHECK(Regex("^\\P{L}+$").test("123 !!"));
  CHECK(Regex("[\\p{L}\\p{N}]+").search("abc123 ").str == "abc123");
  CHECK(Regex("\\p{L}+").search("λόγος42").str == "λόγος");  // Greek
  CHECK(Regex("\\p{Lu}").search("aBc").str == "B");          // uppercase
}

void test_lookbehind_fixed() {
  CHECK(Regex("(?<=\\$)\\d+").search("costs $50 or £60").str == "50");
  CHECK(Regex("(?<=foo)bar").test("foobar"));
  CHECK(!Regex("(?<=foo)bar").test("xxxbar"));
  CHECK(Regex("(?<=foo|baz)X").search("bazX").str == "X");
  CHECK(Regex("(?<!foo)bar").search("foobar zbar").begin == 8);
}

void test_lookbehind_variable() {
  CHECK(Regex("(?<=a+)b").search("aaab").begin == 3);
  CHECK(Regex("(?<=\\d+)px").search("width 1280px").begin == 10);
  // alternation with branches of different lengths
  CHECK(Regex("(?<=foo|barbar)!").search("barbar!").begin == 6);
  CHECK(Regex("(?<=foo|barbar)!").search("foo!").begin == 3);
  // bounded-but-variable
  CHECK(Regex("(?<=x{2,4})y").test("xxxxy"));
  CHECK(!Regex("(?<=x{2,4})y").test("xy"));
  // anchor inside a variable-length lookbehind
  CHECK(Regex("(?<=^\\w+ )\\w+").search("hello world again").str == "world");
  // negative variable-length lookbehind
  CHECK(Regex("(?<!v\\d*)\\d").search("v12 7").str == "7");
}

void test_gpt2_tokenizer() {
  // The real GPT-2 pre-tokenizer pattern (needs \p{...} and lookahead).
  Regex re(
      "'s|'t|'re|'ve|'m|'ll|'d| ?\\p{L}+| ?\\p{N}+| ?[^\\s\\p{L}\\p{N}]+|"
      "\\s+(?!\\S)|\\s+");
  auto toks = re.find_all("I've got 3 cats!");
  CHECK(toks.size() == 6);
  if (toks.size() == 6) {
    CHECK(toks[0].str == "I");
    CHECK(toks[1].str == "'ve");
    CHECK(toks[2].str == " got");
    CHECK(toks[3].str == " 3");
    CHECK(toks[4].str == " cats");
    CHECK(toks[5].str == "!");
  }
}

void test_linear_time() {
  // `(a+)+$` against all-'a' + a trailing mismatch is the classic ReDoS bomb
  // that hangs backtracking engines. The Pike VM stays linear, so this must
  // complete near-instantly and report no match. We assert correctness (not a
  // hard time bound, to stay portable in CI) but also print the timing.
  Regex re("(a+)+$");
  std::string bomb(50000, 'a');
  bomb += 'X';
  auto t0 = std::chrono::steady_clock::now();
  bool matched = re.test(bomb);
  double dt = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
  CHECK(!matched);
  std::printf("  [info] ReDoS bomb n=50000 completed in %.1f ms\n", dt);
}

void test_invalid_patterns() {
  CHECK(rejects("(abc"));        // unmatched '('
  CHECK(rejects("abc)"));        // unmatched ')'
  CHECK(rejects("*abc"));        // nothing to repeat
  CHECK(rejects("[abc"));        // unterminated class
  CHECK(rejects("\\"));          // trailing backslash
  CHECK(rejects("(?P<x>a)"));    // unsupported group construct
  // valid patterns must NOT throw
  CHECK(!rejects("(?<=a+)b"));   // variable lookbehind is supported now
  CHECK(!rejects("a{2,3}"));
  CHECK(!rejects("(?<name>x)"));
}

}  // namespace

int main() {
  test_literals_and_anchors();
  test_any_and_classes();
  test_quantifiers();
  test_alternation_and_groups();
  test_named_groups();
  test_boundaries_and_flags();
  test_find_all_and_replace();
  test_byte_offsets();
  test_grapheme_aware();
  test_lookahead();
  test_unicode_properties();
  test_lookbehind_fixed();
  test_lookbehind_variable();
  test_gpt2_tokenizer();
  test_linear_time();
  test_invalid_patterns();

  std::printf("\nregexlib: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
