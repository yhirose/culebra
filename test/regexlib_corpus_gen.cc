// Scratch: emit candidate (pattern, subject) cases + regexlib's result, as
// hex-encoded lines, for Python to adjudicate. Deterministic. Delete after.
#include <cstdio>
#include <random>
#include <string>
#include "regexlib.h"

std::mt19937 rng(0x5EED);
int U(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
const char *LIT = "abc";

std::string atom(int d);
std::string concat(int d) {
  std::string s;
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    std::string a = atom(d);
    if (U(0, 2) != 0) {
      switch (U(0, 5)) {
        case 0: a += "*"; break;
        case 1: a += "+"; break;
        case 2: a += "?"; break;
        case 3: a += "{" + std::to_string(U(0, 3)) + "}"; break;
        case 4: a += "{" + std::to_string(U(0, 2)) + "," + std::to_string(U(2, 4)) + "}"; break;
        default: a += "{" + std::to_string(U(1, 2)) + ",}"; break;
      }
      if (U(0, 3) == 0) a += "?";
    }
    s += a;
  }
  return s;
}
std::string alt(int d) {
  std::string s = concat(d);
  int k = U(0, 2);
  for (int i = 0; i < k; i++) s += "|" + concat(d);
  return s;
}
std::string cls() {
  std::string s = "[";
  if (U(0, 3) == 0) s += "^";
  int k = U(1, 3);
  for (int i = 0; i < k; i++) {
    if (U(0, 2) == 0) { char a = "ac"[U(0, 1)]; s += a; s += "-"; s += (a == 'a' ? 'c' : 'z'); }
    else s += LIT[U(0, 2)];
  }
  return s + "]";
}
std::string atom(int d) {
  switch ((d <= 0) ? U(0, 4) : U(0, 7)) {
    case 0: return std::string(1, LIT[U(0, 2)]);
    case 1: return ".";
    case 2: return cls();
    case 3: { const char *e = "\\d\\w\\s"; return std::string(e + U(0, 2) * 2, 2); }
    case 4: return (U(0, 1) ? "^" : "$");
    case 5: return "\\b";
    case 6: return "(" + alt(d - 1) + ")";
    default: { std::string p = U(0,1) ? "(?=" : "(?!"; return p + alt(d - 1) + ")"; }  // lookahead
  }
}
std::string subj() {
  static const char *A = "abABC 12_";
  int n = U(0, 9);
  std::string s;
  for (int i = 0; i < n; i++) s += A[U(0, 8)];
  return s;
}
void hex(const std::string &s) { for (unsigned char c : s) printf("%02x", c); }

int main() {
  for (int it = 0; it < 6000; it++) {
    std::string p = alt(3);
    try {
      regexlib::Regex re(p);
      std::string sj = subj();
      auto m = re.search(sj);
      hex(p); printf(" "); hex(sj); printf(" %d ", m.matched ? 1 : 0); hex(m.str); printf("\n");
    } catch (...) { continue; }
  }
  return 0;
}
