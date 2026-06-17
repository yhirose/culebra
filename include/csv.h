#pragma once

// CSV parse / stringify for the `CSV` stdlib namespace.
//
// Value-neutral core (no culebra types), shared by the interp
// (stdlib_interp.h) and the JIT/AOT slow-path adapters (stdlib_jit.h) so the
// three backends agree byte-for-byte. RFC 4180-ish: comma-separated fields,
// records ended by LF / CRLF / CR, fields quoted with double quotes when they
// contain a delimiter / quote / newline, and an embedded quote escaped by
// doubling (`""`). Parsing is lenient (no errors): every field comes back as a
// String, and `stringify(parse(text))` round-trips well-formed input. Binary
// safe (operates on bytes; embedded NUL is an ordinary field byte).

#include <string>
#include <string_view>
#include <vector>

namespace culebra::csv {

// Parse CSV text into rows of string fields. An empty input yields no rows; a
// trailing newline does not add an empty final row. A field is quoted with `"`
// and an embedded `"` is written `""`; a quote that opens a field switches to
// quoted mode where commas and newlines are literal.
inline std::vector<std::vector<std::string>> parse(std::string_view s,
                                                  char delim = ',') {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool in_quotes = false;
  bool pending = false;  // a field/record has begun since the last record end
  size_t i = 0, n = s.size();
  while (i < n) {
    char c = s[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < n && s[i + 1] == '"') {  // "" -> literal "
          field += '"';
          i += 2;
          continue;
        }
        in_quotes = false;
        i++;
        continue;
      }
      field += c;
      i++;
      continue;
    }
    if (c == '"') {
      in_quotes = true;
      pending = true;
      i++;
    } else if (c == delim) {
      row.push_back(std::move(field));
      field.clear();
      pending = true;
      i++;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < n && s[i + 1] == '\n') i++;  // CRLF is one break
      i++;
      row.push_back(std::move(field));
      field.clear();
      rows.push_back(std::move(row));
      row.clear();
      pending = false;
    } else {
      field += c;
      pending = true;
      i++;
    }
  }
  // Flush a final record that had no trailing newline.
  if (pending || !field.empty() || !row.empty()) {
    row.push_back(std::move(field));
    rows.push_back(std::move(row));
  }
  return rows;
}

// Quote one field if it contains the delimiter, a double quote, CR or LF
// (doubling any embedded quote); otherwise return it unchanged.
inline std::string quote_field(std::string_view f, char delim = ',') {
  bool needs = false;
  for (char c : f) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') {
      needs = true;
      break;
    }
  }
  if (!needs) return std::string(f);
  std::string out;
  out.reserve(f.size() + 2);
  out += '"';
  for (char c : f) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// Serialize rows into CSV text: each field quoted as needed, joined by the
// delimiter, records separated by `\n` (no trailing newline, so parse
// round-trips).
inline std::string stringify(const std::vector<std::vector<std::string>>& rows,
                             char delim = ',') {
  std::string out;
  for (size_t r = 0; r < rows.size(); r++) {
    if (r) out += '\n';
    const auto& row = rows[r];
    for (size_t c = 0; c < row.size(); c++) {
      if (c) out += delim;
      out += quote_field(row[c], delim);
    }
  }
  return out;
}

}  // namespace culebra::csv
