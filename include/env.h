#pragma once

// dotenv-style `.env` parsing for the `Env` stdlib namespace.
//
// Value-neutral core (no culebra types), shared by the interp
// (stdlib_interp.h) and the JIT/AOT slow-path adapters (stdlib_jit.h) so the
// three backends agree byte-for-byte. Parsing is lenient (no errors):
// malformed lines are skipped, every value comes back as a String.
//
// Grammar (one entry per line; multi-line values are not supported):
//   * blank lines and lines whose first non-space char is `#` are ignored
//   * an optional `export ` prefix is stripped (common in shell `.env` files)
//   * `KEY=VALUE`; the key is trimmed, a line without `=` or with an empty key
//     is skipped
//   * a double-quoted value (`"..."`) honours `\n \t \r \\ \"` escapes
//   * a single-quoted value (`'...'`) is raw (no escapes)
//   * an unquoted value is trimmed and an inline ` # comment` (a `#` preceded
//     by whitespace) is stripped; `#` not preceded by whitespace stays literal
// Duplicate keys keep their first position with the last value winning.

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace culebra::env {

inline std::string _trim(std::string_view v) {
  size_t b = 0, e = v.size();
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (b < e && ws(v[b])) b++;
  while (e > b && ws(v[e - 1])) e--;
  return std::string(v.substr(b, e - b));
}

// Parse dotenv text into ordered (key, value) pairs (first-position order,
// last value wins on a duplicate key).
inline std::vector<std::pair<std::string, std::string>> parse(
    std::string_view s) {
  std::vector<std::pair<std::string, std::string>> out;
  std::unordered_map<std::string, size_t> idx;
  size_t i = 0, n = s.size();
  while (i < n) {
    // Slice one line, then advance past its LF / CRLF / CR terminator.
    size_t le = i;
    while (le < n && s[le] != '\n' && s[le] != '\r') le++;
    std::string_view line = s.substr(i, le - i);
    if (le < n && s[le] == '\r' && le + 1 < n && s[le + 1] == '\n')
      i = le + 2;
    else if (le < n)
      i = le + 1;
    else
      i = le;

    // Skip leading whitespace; ignore blanks and full-line comments.
    size_t p = 0;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
    std::string_view t = line.substr(p);
    if (t.empty() || t[0] == '#') continue;

    // Optional `export ` prefix.
    if (t.size() > 7 && t.substr(0, 7) == "export " ) {
      size_t q = 7;
      while (q < t.size() && (t[q] == ' ' || t[q] == '\t')) q++;
      t = t.substr(q);
    }

    size_t eq = t.find('=');
    if (eq == std::string_view::npos) continue;  // lenient: skip malformed
    std::string key = _trim(t.substr(0, eq));
    if (key.empty()) continue;

    std::string_view rest = t.substr(eq + 1);
    size_t vp = 0;
    while (vp < rest.size() && (rest[vp] == ' ' || rest[vp] == '\t')) vp++;
    rest = rest.substr(vp);

    std::string value;
    if (!rest.empty() && (rest[0] == '"' || rest[0] == '\'')) {
      char quote = rest[0];
      for (size_t k = 1; k < rest.size(); k++) {
        char c = rest[k];
        if (quote == '"' && c == '\\' && k + 1 < rest.size()) {
          char nx = rest[k + 1];
          switch (nx) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            default: value += '\\'; value += nx; break;
          }
          k++;  // consumed the escape char too
          continue;
        }
        if (c == quote) break;  // closing quote; ignore any trailing text
        value += c;
      }
    } else {
      // Unquoted: strip an inline ` #...` comment, then trim.
      size_t hash = std::string_view::npos;
      for (size_t k = 0; k < rest.size(); k++) {
        if (rest[k] == '#' && (k == 0 || rest[k - 1] == ' ' || rest[k - 1] == '\t')) {
          hash = k;
          break;
        }
      }
      value = _trim(hash == std::string_view::npos ? rest : rest.substr(0, hash));
    }

    auto it = idx.find(key);
    if (it != idx.end()) {
      out[it->second].second = std::move(value);
    } else {
      idx.emplace(key, out.size());
      out.emplace_back(std::move(key), std::move(value));
    }
  }
  return out;
}

}  // namespace culebra::env
