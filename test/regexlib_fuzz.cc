//
//  regexlib_fuzz.cc
//
//  Deterministic regression fuzzer for regexlib.h. Two jobs:
//
//   1. GATE (portable, fails the test): oracle-free robustness — random
//      patterns/subjects must never crash, hang, or throw anything but
//      RegexError at construction, and every reported match must be
//      self-consistent (span/byte-slice agreement, find_all vs search
//      agreement, capture bounds). Run it under ASan/UBSan in CI for memory
//      safety. A fixed seed makes it reproducible.
//
//   2. INFO (not a gate): a differential pass against std::regex (ECMAScript,
//      which is leftmost-first like us) over the common ASCII subset. The diff
//      count is printed but does NOT fail the test, because std::regex
//      semantics vary across standard libraries (libc++ vs libstdc++ differ on
//      \b at string end and on match-complexity limits). It is a human signal,
//      not a portable assertion.
//
//  Build (standalone):
//    c++ -std=c++17 -Iinclude -Ivendor/cpp-unicodelib \
//        test/regexlib_fuzz.cc -o regexlib_fuzz && ./regexlib_fuzz
//

#include <cstdio>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "regexlib.h"

namespace {

std::mt19937 rng(0xC0FFEE);  // fixed seed -> reproducible
int U(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

const char *LIT = "abc";

std::string gen_atom(int depth);

std::string gen_class() {
  std::string s = "[";
  if (U(0, 3) == 0) s += "^";
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    if (U(0, 2) == 0) {
      char a = "ac"[U(0, 1)];
      s += a;
      s += "-";
      s += (a == 'a' ? 'c' : 'z');
    } else {
      s += LIT[U(0, 2)];
    }
  }
  return s + "]";
}

std::string gen_quant() {
  switch (U(0, 5)) {
    case 0: return "*";
    case 1: return "+";
    case 2: return "?";
    case 3: return "{" + std::to_string(U(0, 3)) + "}";
    case 4: return "{" + std::to_string(U(0, 2)) + "," + std::to_string(U(2, 4)) + "}";
    default: return "{" + std::to_string(U(1, 2)) + ",}";
  }
}

std::string gen_concat(int depth) {
  std::string s;
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    std::string a = gen_atom(depth);
    if (U(0, 2) != 0) {
      a += gen_quant();
      if (U(0, 3) == 0) a += "?";
    }
    s += a;
  }
  return s;
}

std::string gen_alt(int depth) {
  std::string s = gen_concat(depth);
  int k = U(0, 2);
  for (int i = 0; i < k; i++) s += "|" + gen_concat(depth);
  return s;
}

std::string gen_atom(int depth) {
  int choice = (depth <= 0) ? U(0, 4) : U(0, 7);
  switch (choice) {
    case 0: return std::string(1, LIT[U(0, 2)]);
    case 1: return ".";
    case 2: return gen_class();
    case 3: { const char *e = "\\d\\w\\s"; return std::string(e + U(0, 2) * 2, 2); }
    case 4: return (U(0, 1) ? "^" : "$");
    case 5: return "\\b";
    case 6: return "(" + gen_alt(depth - 1) + ")";
    default: return "(?:" + gen_alt(depth - 1) + ")";
  }
}

std::string gen_subject() {
  static const char *A = "abcABC 123_\n";
  int n = U(0, 12);
  std::string s;
  for (int i = 0; i < n; i++) s += A[U(0, 11)];
  return s;
}

int g_fail = 0;

void fail(const std::string &why, const std::string &pat,
          const std::string &subj) {
  if (g_fail < 25)
    std::printf("[INVARIANT] %s : /%s/ on \"%s\"\n", why.c_str(), pat.c_str(),
                subj.c_str());
  g_fail++;
}

// Oracle-free self-consistency checks on a single match.
void check_match(const regexlib::MatchResult &m, const std::string &s,
                 const std::string &pat, const std::string &subj) {
  if (!m.matched) return;
  if (!(m.begin <= m.end && m.end <= s.size())) fail("match span out of range", pat, subj);
  else if (m.str != s.substr(m.begin, m.end - m.begin)) fail("match str != slice", pat, subj);
  for (auto &g : m.groups) {
    if (!g.matched) continue;
    if (!(g.begin <= g.end && g.end <= s.size())) fail("group span out of range", pat, subj);
    else if (g.str != s.substr(g.begin, g.end - g.begin)) fail("group str != slice", pat, subj);
  }
}

// --- Unicode / grapheme-cluster fuzzing (the engine's differentiator) ---
// A mix of: ASCII, a precomposed accent, a base+combining grapheme, CJK, a
// single emoji, a ZWJ family cluster (many code points, one grapheme), and a
// regional-indicator flag (two code points, one grapheme).
const std::vector<std::string> UCHARS = {
    "a", "1", " ",
    "\xC3\xA9",                                  // é  (U+00E9)
    "e\xCC\x81",                                 // e + combining acute (1 cluster)
    "\xE6\xBC\xA2",                              // 漢 (U+6F22)
    "\xF0\x9F\x98\x80",                          // 😀 (U+1F600)
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",  // 👨‍👩‍👧
    "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",          // 🇯🇵 flag
};

std::string gen_uchar() { return UCHARS[U(0, (int)UCHARS.size() - 1)]; }

std::string gen_upattern_inner(int depth);  // mutually recursive with gen_uatom

std::string gen_uatom(int depth) {
  switch ((depth <= 0) ? U(0, 3) : U(0, 5)) {
    case 0: return gen_uchar();                          // literal grapheme
    case 1: return ".";
    case 2: { const char *e = "\\d\\w\\s"; return std::string(e + U(0, 2) * 2, 2); }
    case 3: return "[" + gen_uchar() + gen_uchar() + "]";  // class of clusters
    case 4: return U(0, 1) ? "\\p{L}" : "\\p{N}";
    default: return "(" + gen_upattern_inner(depth - 1) + ")";
  }
}

std::string gen_uconcat(int depth) {
  std::string s;
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    std::string a = gen_uatom(depth);
    if (U(0, 2) != 0) a += gen_quant();
    s += a;
  }
  return s;
}

std::string gen_upattern_inner(int depth) {
  std::string s = gen_uconcat(depth);
  int k = U(0, 1);
  for (int i = 0; i < k; i++) s += "|" + gen_uconcat(depth);
  return s;
}

std::string gen_usubject() {
  int n = U(0, 8);
  std::string s;
  for (int i = 0; i < n; i++) s += gen_uchar();
  return s;
}

// --- Lookaround fuzzing: exercises the reverse-matching lookbehind path,
// which the std::regex-compatible generator never produces. Oracle-free. ---
std::string gen_la_pattern(int depth);  // mutually recursive with gen_la_atom

std::string gen_la_atom(int depth) {
  switch ((depth <= 0) ? U(0, 3) : U(0, 6)) {
    case 0: return std::string(1, LIT[U(0, 2)]);
    case 1: return ".";
    case 2: return gen_class();
    case 3: { const char *e = "\\d\\w\\s"; return std::string(e + U(0, 2) * 2, 2); }
    case 4: {  // lookbehind — the reverse-compiled / reverse-matched path
      const char *p[] = {"(?<=", "(?<!"};
      return std::string(p[U(0, 1)]) + gen_la_pattern(depth - 1) + ")";
    }
    case 5: {  // lookahead
      const char *p[] = {"(?=", "(?!"};
      return std::string(p[U(0, 1)]) + gen_la_pattern(depth - 1) + ")";
    }
    default: return "(" + gen_la_pattern(depth - 1) + ")";
  }
}

std::string gen_la_concat(int depth) {
  std::string s;
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    std::string a = gen_la_atom(depth);
    if (U(0, 2) != 0) a += gen_quant();
    s += a;
  }
  return s;
}

std::string gen_la_pattern(int depth) {
  std::string s = gen_la_concat(depth);
  int k = U(0, 1);
  for (int i = 0; i < k; i++) s += "|" + gen_la_concat(depth);
  return s;
}

void invariant_fuzz_lookaround(int iters) {
  for (int it = 0; it < iters; it++) {
    std::string pat = gen_la_pattern(3);
    regexlib::Regex *re = nullptr;
    try {
      re = new regexlib::Regex(pat);
    } catch (const regexlib::RegexError &) {
      continue;
    } catch (...) {
      fail("non-RegexError at construction (lookaround)", pat, "");
      continue;
    }
    for (int s = 0; s < 5; s++) {
      std::string subj = gen_subject();
      regexlib::MatchResult m;
      std::vector<regexlib::MatchResult> all;
      try {
        m = re->search(subj);
        all = re->find_all(subj);
        if (re->test(subj) != m.matched) fail("test() != search (lookaround)", pat, subj);
      } catch (const regexlib::RegexError &) {
        continue;  // step budget on a pathological lookaround is acceptable
      } catch (...) {
        fail("matcher threw (lookaround)", pat, subj);
        continue;
      }
      check_match(m, subj, pat, subj);
      if (m.matched) {
        if (all.empty()) fail("matched but find_all empty (lookaround)", pat, subj);
        else if (all[0].begin != m.begin || all[0].str != m.str)
          fail("find_all[0] != search (lookaround)", pat, subj);
      } else if (!all.empty()) {
        fail("no-match but find_all non-empty (lookaround)", pat, subj);
      }
      for (size_t i = 1; i < all.size(); i++) {
        check_match(all[i], subj, pat, subj);
        if (all[i].begin < all[i - 1].end) fail("find_all overlaps (lookaround)", pat, subj);
      }
    }
    delete re;
  }
}

// Oracle-free invariant fuzzing over Unicode/grapheme inputs.
void invariant_fuzz_unicode(int iters) {
  for (int it = 0; it < iters; it++) {
    std::string pat = gen_upattern_inner(3);
    regexlib::Regex *re = nullptr;
    try {
      re = new regexlib::Regex(pat);
    } catch (const regexlib::RegexError &) {
      continue;
    } catch (...) {
      fail("non-RegexError at construction (unicode)", pat, "");
      continue;
    }
    for (int s = 0; s < 5; s++) {
      std::string subj = gen_usubject();
      regexlib::MatchResult m;
      std::vector<regexlib::MatchResult> all;
      try {
        m = re->search(subj);
        all = re->find_all(subj);
        if (re->test(subj) != m.matched) fail("test() != search (unicode)", pat, subj);
      } catch (...) {
        fail("matcher threw (unicode)", pat, subj);
        continue;
      }
      check_match(m, subj, pat, subj);
      if (m.matched) {
        if (all.empty()) fail("matched but find_all empty (unicode)", pat, subj);
        else if (all[0].begin != m.begin || all[0].str != m.str)
          fail("find_all[0] != search (unicode)", pat, subj);
      } else if (!all.empty()) {
        fail("no-match but find_all non-empty (unicode)", pat, subj);
      }
      for (size_t i = 1; i < all.size(); i++) {
        check_match(all[i], subj, pat, subj);
        if (all[i].begin < all[i - 1].end) fail("find_all overlaps (unicode)", pat, subj);
      }
    }
    delete re;
  }
}

void invariant_fuzz(int iters) {
  for (int it = 0; it < iters; it++) {
    std::string pat = gen_alt(3);
    regexlib::Regex *re = nullptr;
    try {
      re = new regexlib::Regex(pat);
    } catch (const regexlib::RegexError &) {
      continue;  // rejecting a pattern is fine
    } catch (...) {
      fail("non-RegexError thrown at construction", pat, "");
      continue;
    }
    for (int s = 0; s < 5; s++) {
      std::string subj = gen_subject();
      regexlib::MatchResult m;
      std::vector<regexlib::MatchResult> all;
      try {
        m = re->search(subj);
        all = re->find_all(subj);
        if (re->test(subj) != m.matched) fail("test() != search().matched", pat, subj);
      } catch (...) {
        fail("matcher threw", pat, subj);
        continue;
      }
      check_match(m, subj, pat, subj);
      // match() takes the forward-DFA path for tier-1 patterns; search() stays
      // on the Pike VM. They must agree whenever the leftmost match is anchored
      // at 0 — a genuine DFA-vs-Pike differential (oracle-free).
      regexlib::MatchResult am;
      try {
        am = re->match(subj);
      } catch (...) {
        fail("match() threw", pat, subj);
        continue;
      }
      bool anchored = m.matched && m.begin == 0;
      if (am.matched != anchored) fail("match() != (search anchored at 0)", pat, subj);
      else if (am.matched && (am.end != m.end || am.str != m.str))
        fail("match() span != search span at 0", pat, subj);
      // find_all must agree with search on the leftmost match.
      if (m.matched) {
        if (all.empty()) fail("search matched but find_all empty", pat, subj);
        else if (all[0].begin != m.begin || all[0].str != m.str)
          fail("find_all[0] != search", pat, subj);
      } else if (!all.empty()) {
        fail("search no-match but find_all non-empty", pat, subj);
      }
      // find_all results must advance (non-regressing positions).
      for (size_t i = 1; i < all.size(); i++) {
        check_match(all[i], subj, pat, subj);
        if (all[i].begin < all[i - 1].end) fail("find_all overlaps/regresses", pat, subj);
      }
    }
    delete re;
  }
}

// Random byte patterns (including metacharacters): construction must only ever
// throw RegexError, and matching must not crash.
void parser_fuzz(int iters) {
  static const std::string META = "abc().[]{}|*+?^$\\<>=!:-,0123 dwsbpiPm";
  for (int it = 0; it < iters; it++) {
    int n = U(0, 14);
    std::string pat;
    for (int i = 0; i < n; i++) pat += META[U(0, (int)META.size() - 1)];
    try {
      regexlib::Regex re(pat);
      for (int s = 0; s < 3; s++) (void)re.search(gen_subject());
    } catch (const regexlib::RegexError &) {
    } catch (...) {
      fail("non-RegexError thrown", pat, "");
    }
  }
}

// Informational only: differential vs std::regex. Not a gate.
void differential_info(int iters) {
  int cmp = 0, diff = 0, skip = 0;
  for (int it = 0; it < iters; it++) {
    std::string pat = gen_alt(3);
    regexlib::Regex *re = nullptr;
    try {
      re = new regexlib::Regex(pat);
    } catch (...) {
      continue;
    }
    std::regex sre;
    try {
      sre = std::regex(pat, std::regex::ECMAScript);
    } catch (...) {
      delete re;
      skip++;
      continue;
    }
    for (int s = 0; s < 4; s++) {
      std::string subj = gen_subject();
      auto m = re->search(subj);
      std::smatch sm;
      bool std_m;
      try {
        std_m = std::regex_search(subj, sm, sre);
      } catch (...) {
        skip++;
        continue;
      }
      cmp++;
      if (m.matched != std_m || (m.matched && m.str != sm.str())) diff++;
    }
    delete re;
  }
  std::printf("[info] differential vs std::regex: %d compared, %d diffs, "
              "%d skipped (platform-dependent; not a gate)\n", cmp, diff, skip);
}

}  // namespace

int main() {
  invariant_fuzz(25000);
  invariant_fuzz_unicode(25000);
  invariant_fuzz_lookaround(25000);
  parser_fuzz(30000);
  differential_info(12000);

  if (g_fail == 0) {
    std::printf("regexlib_fuzz: all invariants held, no crashes\n");
    return 0;
  }
  std::printf("regexlib_fuzz: %d invariant violations\n", g_fail);
  return 1;
}
