#pragma once

// Unicode-aware String operations, shared by the interpreter and the JIT
// runtime so both backends agree scalar for scalar. Byte-level helpers that
// need no character data stay in shared.h; everything here reaches into
// cpp-unicodelib's tables, so only the headers that already include them
// (jit.h and the runtime headers) pull this in.

#include <shared.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>

#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// One step of a byte walk: either a decodable UTF-8 scalar or a single
// undecodable byte. culebra Strings may hold arbitrary bytes (§18.1:
// `String.from_bytes` does not validate, `iter()` yields bad bytes as they
// are), so every transform below carries them through rather than dropping
// or replacing them.
struct Utf8Step {
  std::string_view bytes;
  char32_t cp = 0;  // meaningful only when `valid`
  bool valid = false;
};

inline Utf8Step utf8_step(std::string_view s, size_t i) {
  char32_t cp = 0;
  size_t n = unicode::utf8::decode_codepoint(s.data() + i, s.size() - i, cp);
  if (n == 0) return {s.substr(i, 1), 0, false};
  return {s.substr(i, n), cp, true};
}

// Apply `f` to each maximal run of decodable scalars, splicing the
// undecodable bytes back in unchanged. Case mapping and normalization are
// both context-sensitive (final sigma, canonical ordering), so they run over
// whole runs rather than one scalar at a time.
template <typename F>
inline std::string utf8_map_runs(std::string_view s, F&& f) {
  std::string out;
  out.reserve(s.size());
  std::u32string run;
  auto flush = [&] {
    if (run.empty()) return;
    unicode::utf8::encode(f(run), out);
    run.clear();
  };
  for (size_t i = 0; i < s.size();) {
    auto st = utf8_step(s, i);
    if (st.valid) {
      run += st.cp;
    } else {
      flush();
      out += st.bytes;
    }
    i += st.bytes.size();
  }
  flush();
  return out;
}

// `s.upper()` / `s.lower()` — full Unicode case mapping (UAX #21), so a
// mapping may change the string's length (`'ß'.upper() == 'SS'`).
inline std::string str_upper(std::string_view s) {
  return utf8_map_runs(
      s, [](const std::u32string& r) { return unicode::to_uppercase(r); });
}

inline std::string str_lower(std::string_view s) {
  return utf8_map_runs(
      s, [](const std::u32string& r) { return unicode::to_lowercase(r); });
}

// `s.title()` — every word's first letter titlecased, the rest lowercased.
// Word boundaries are UAX #29, so `"o'neil"` titlecases as one word.
inline std::string str_title(std::string_view s) {
  return utf8_map_runs(
      s, [](const std::u32string& r) { return unicode::to_titlecase(r); });
}

// `s.capitalize()` — the first letter titlecased, everything after it
// lowercased (Python/Ruby `capitalize`, not `title`).
inline std::string str_capitalize(std::string_view s) {
  bool first = true;
  return utf8_map_runs(s, [&](const std::u32string& r) {
    auto out = unicode::to_lowercase(r);
    if (first && !out.empty()) {
      out[0] = unicode::simple_titlecase_mapping(out[0]);
      first = false;
    }
    return out;
  });
}

// `s.eq_ignore_case(other)` — default caseless matching: full case folding
// on both sides, so `'ß'` and `'SS'` compare equal. Canonical equivalence is
// deliberately *not* folded in; `normalize()` is the explicit way to ask for
// that.
inline std::string str_case_fold(std::string_view s) {
  return utf8_map_runs(
      s, [](const std::u32string& r) { return unicode::to_case_fold(r); });
}

inline bool str_eq_ignore_case(std::string_view a, std::string_view b) {
  if (a == b) return true;  // the common hit, without decoding either side
  return str_case_fold(a) == str_case_fold(b);
}

// `s.normalize(form)` — the four Unicode normalization forms. An unknown
// form is a ValueError rather than a silent pass-through.
inline std::string str_normalize(std::string_view s, std::string_view form,
                                 long line = 0, long col = 0) {
  if (form == "NFC") {
    return utf8_map_runs(
        s, [](const std::u32string& r) { return unicode::to_nfc(r); });
  }
  if (form == "NFD") {
    return utf8_map_runs(
        s, [](const std::u32string& r) { return unicode::to_nfd(r); });
  }
  if (form == "NFKC") {
    return utf8_map_runs(
        s, [](const std::u32string& r) { return unicode::to_nfkc(r); });
  }
  if (form == "NFKD") {
    return utf8_map_runs(
        s, [](const std::u32string& r) { return unicode::to_nfkd(r); });
  }
  throw CulebraError(
      "ValueError",
      culebra::format("normalize() form must be 'NFC', 'NFD', 'NFKC', or "
                      "'NFKD', got '{}'",
                      form),
      line, col);
}

// `s.reverse()` — reversed by Extended Grapheme Cluster (UAX #29), so an
// emoji ZWJ sequence or a base+combining pair survives the flip intact.
// Undecodable bytes reverse as units of their own.
inline std::string str_reverse(std::string_view s) {
  std::vector<std::string_view> units;
  size_t i = 0;
  while (i < s.size()) {
    if (!utf8_step(s, i).valid) {
      units.push_back(s.substr(i, 1));
      i++;
      continue;
    }
    // Decode the whole decodable run once, remembering where each scalar
    // started, then cut the run into clusters.
    std::u32string run;
    std::vector<size_t> offs;
    size_t j = i;
    while (j < s.size()) {
      auto st = utf8_step(s, j);
      if (!st.valid) break;
      offs.push_back(j);
      run += st.cp;
      j += st.bytes.size();
    }
    offs.push_back(j);
    for (size_t k = 0; k < run.size();) {
      size_t len = unicode::grapheme_length(run.data() + k, run.size() - k);
      if (len == 0) len = 1;
      if (k + len > run.size()) len = run.size() - k;
      units.push_back(s.substr(offs[k], offs[k + len] - offs[k]));
      k += len;
    }
    i = j;
  }
  std::string out;
  out.reserve(s.size());
  for (auto it = units.rbegin(); it != units.rend(); ++it) out += *it;
  return out;
}

// The `is_*` family: true when the receiver is non-empty and every scalar
// satisfies the property. An empty receiver is false throughout — one rule
// for the whole family instead of a per-method exception. An undecodable
// byte fails every property.
template <typename P>
inline bool utf8_all_scalars(std::string_view s, P&& p) {
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size();) {
    auto st = utf8_step(s, i);
    if (!st.valid || !p(st.cp)) return false;
    i += st.bytes.size();
  }
  return true;
}

inline bool str_is_digit(std::string_view s) {
  return utf8_all_scalars(s, [](char32_t c) {
    return unicode::general_category(c) == unicode::GeneralCategory::Nd;
  });
}

inline bool str_is_alpha(std::string_view s) {
  return utf8_all_scalars(s,
                          [](char32_t c) { return unicode::is_alphabetic(c); });
}

inline bool str_is_alnum(std::string_view s) {
  return utf8_all_scalars(s, [](char32_t c) {
    return unicode::is_alphabetic(c) ||
           unicode::is_number_category(unicode::general_category(c));
  });
}

inline bool str_is_space(std::string_view s) {
  return utf8_all_scalars(
      s, [](char32_t c) { return unicode::is_white_space(c); });
}

inline bool str_is_ascii(std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x80;
  });
}

// `trim` / `trim_start` / `trim_end`. An empty `chars` trims Unicode
// White_Space; a non-empty one lists the scalars to trim and needs no
// character data, so it stays on shared.h's trim_chars.
inline std::string_view str_trim(std::string_view s, std::string_view chars,
                                 bool left, bool right) {
  if (!chars.empty()) return trim_chars(s, chars, left, right);
  size_t b = 0, e = s.size();
  if (left) {
    while (b < e) {
      auto st = utf8_step(s, b);
      if (!st.valid || !unicode::is_white_space(st.cp)) break;
      b += st.bytes.size();
    }
  }
  if (right) {
    while (e > b) {
      size_t start = e - 1;  // walk back over UTF-8 continuation bytes
      while (start > b &&
             (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
        start--;
      }
      auto st = utf8_step(s, start);
      if (!st.valid || st.bytes.size() != e - start ||
          !unicode::is_white_space(st.cp)) {
        break;
      }
      e = start;
    }
  }
  return s.substr(b, e - b);
}

// `s.split_whitespace()` — split on runs of Unicode White_Space, with no
// empty piece at either end. `'  a  b '.split_whitespace() == ['a', 'b']`,
// where `split(' ')` would yield the empty pieces too.
inline std::vector<std::string_view> str_split_whitespace(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    auto st = utf8_step(s, i);
    if (st.valid && unicode::is_white_space(st.cp)) {
      i += st.bytes.size();
      continue;
    }
    size_t start = i;
    while (i < s.size()) {
      auto w = utf8_step(s, i);
      if (w.valid && unicode::is_white_space(w.cp)) break;
      i += w.bytes.size();
    }
    out.push_back(s.substr(start, i - start));
  }
  return out;
}

}  // namespace culebra
