#pragma once

// CSV parse / stringify for the `CSV` stdlib namespace.
//
// Value-neutral core (no culebra types), shared by the interp
// and the JIT/AOT slow-path adapters (stdlib_jit.h) so the
// three backends agree byte-for-byte. RFC 4180-ish: comma-separated fields,
// records ended by LF / CRLF / CR, fields quoted with double quotes when they
// contain a delimiter / quote / newline, and an embedded quote escaped by
// doubling (`""`). Parsing is lenient (no errors): every field comes back as a
// String, and `stringify(parse(text))` round-trips well-formed input. Binary
// safe (operates on bytes; embedded NUL is an ordinary field byte).

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace culebra::csv {

// --- Optional typed-column coercion (the `types:` kwarg) ------------------
// A column type a cell can be coerced to. `types:` maps a header name to one of
// these; unlisted columns stay String. Coercion is explicit (never inferred),
// so values like ZIP codes / IDs declared String keep their exact text — no
// leading-zero or precision loss. Coercion is strict: no implicit trimming, and
// Bool is exactly "true" / "false".
enum class ColType { String, Long, Float, Bool };

// Map a type name to a ColType; false for an unknown name (the caller reports
// "unknown type '<name>'"). Keep in sync with the valid-types list in the
// callers' messages.
inline bool parse_col_type(std::string_view name, ColType& out) {
  if (name == "String") { out = ColType::String; return true; }
  if (name == "Long")   { out = ColType::Long;   return true; }
  if (name == "Float")  { out = ColType::Float;  return true; }
  if (name == "Bool")   { out = ColType::Bool;   return true; }
  return false;
}

// Name of a ColType, for error messages ("expected Long, got '...'").
inline const char* col_type_name(ColType t) {
  switch (t) {
    case ColType::String: return "String";
    case ColType::Long:   return "Long";
    case ColType::Float:  return "Float";
    case ColType::Bool:   return "Bool";
  }
  return "String";
}

// The scalar a cell coerced to; `kind` selects which field is meaningful. The
// backend maps this neutral result to its own Value (single-sourcing the
// coercion logic keeps interp/JIT/AOT byte-identical).
struct CoercedCell {
  ColType kind = ColType::String;
  int64_t long_val = 0;
  double float_val = 0;
  bool bool_val = false;
  std::string_view str_val;  // for String: points into the source cell.
};

// Coerce `cell` to `type`, filling `out` and returning true on success. On
// failure returns false (the cell isn't a valid value of that type, including an
// empty cell for a non-String column) — the caller raises a row/column error.
inline bool coerce_cell(std::string_view cell, ColType type, CoercedCell& out) {
  out.kind = type;
  switch (type) {
    case ColType::String:
      out.str_val = cell;
      return true;
    case ColType::Long: {
      const char* b = cell.data();
      const char* e = b + cell.size();
      int64_t v = 0;
      auto r = std::from_chars(b, e, v);
      if (r.ec != std::errc() || r.ptr != e) return false;  // junk/overflow/empty
      out.long_val = v;
      return true;
    }
    case ColType::Float: {
      if (cell.empty()) return false;
      // libc++ lacks from_chars<double>; strtod needs NUL termination.
      std::string buf(cell);
      const char* b = buf.c_str();
      char* e = nullptr;
      double d = std::strtod(b, &e);
      if (e != b + buf.size()) return false;  // trailing junk (covers empty too)
      out.float_val = d;
      return true;
    }
    case ColType::Bool:
      if (cell == "true") { out.bool_val = true; return true; }
      if (cell == "false") { out.bool_val = false; return true; }
      return false;
  }
  return false;
}

// Resolve per-column types from a header row + a (column-name → type-name)
// `types` spec. Validates: header names are unique, every `types` key names an
// existing column, and every type name is known. On success fills `col_types`
// (one entry per header column, String where unlisted) and returns true; else
// sets `err` (message without the "CSV.parse: " prefix) and returns false.
// Single-sources the header/types validation + messages across backends.
inline bool resolve_col_types(
    const std::vector<std::string>& header,
    const std::vector<std::pair<std::string, std::string>>& types,
    std::vector<ColType>& col_types, std::string& err) {
  std::unordered_map<std::string_view, size_t> idx;
  for (size_t c = 0; c < header.size(); c++) {
    if (!idx.emplace(header[c], c).second) {
      err = "duplicate header column '" + header[c] + "'";
      return false;
    }
  }
  col_types.assign(header.size(), ColType::String);
  for (const auto& [name, tname] : types) {
    auto it = idx.find(name);
    if (it == idx.end()) {
      err = "types references unknown column '" + name + "'";
      return false;
    }
    ColType ct;
    if (!parse_col_type(tname, ct)) {
      err = "unknown type '" + tname + "' for column '" + name +
            "' (valid: String, Long, Float, Bool)";
      return false;
    }
    col_types[it->second] = ct;
  }
  return true;
}

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
