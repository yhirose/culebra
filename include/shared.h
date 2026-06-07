#pragma once

#include <array>
#include <atomic>
#include <csignal>
#include <cctype>
#include <mutex>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <random>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <climits>   // PATH_MAX (Sys.executable)
#include <cstring>   // strlen (Sys.executable)
#include <cstdio>    // Windows stdin fallback (read_stdin_*_interruptible)
#include <unistd.h>  // readlink (Sys.executable), read (interruptible stdin)
#if !defined(_WIN32)
#include <poll.h>    // interruptible stdin (poll fd 0 between interrupt checks)
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath (Sys.executable)
#endif

namespace culebra {

// Names of value-type built-in methods (Array / String / Set / Tuple / Object-
// dict / Iterator / Tensor). These are dispatched inline by the JIT (no stored
// closure) and table-wrapped by the interp, so a BARE reference to one
// (`let m = [1,2].map`) is not a first-class value on either backend — both
// reject it (call it, or wrap it in a lambda). A user-defined property/method
// of the same name on an Object/class is found first and stays first-class, so
// this set is only consulted after the stored-property lookup misses. Single
// source for both backends' bare-method-reference check AND the JIT's
// known_builtin_methods() (which delegates here so the list never drifts).
inline const std::unordered_set<std::string_view>& builtin_method_names() {
  static const std::unordered_set<std::string_view> kNames = {
      "size",       "push",        "pop",        "reverse",    "slice",
      "join",       "index_of",    "contains",   "upper",      "lower",
      "trim",       "tr",          "trim_start", "trim_end",   "split",
      "starts_with","ends_with",   "keys",       "has",        "remove",
      "map",        "filter",      "reduce",     "for_each",   "find",
      "any",        "all",         "flat_map",   "sort_by",    "sorted_by",
      "sum",        "product",     "min",        "max",        "collect",
      "count",      "take",        "skip",       "take_while", "chain",
      "zip",        "enumerate",   "code_points","graphemes",  "iter",
      "view",       "split_iter",  "shape",      "pow",        "transpose",
      "reshape",    "mean",        "argmax",     "to_array",   "dot",
      "linear_sigmoid", "clone",   "relu",       "sigmoid",    "softmax",
      "union",      "intersect",   "diff",       "sym_diff",   "subset",
      "superset"};
  return kNames;
}
inline bool is_builtin_method_name(std::string_view name) {
  return builtin_method_names().count(name) > 0;
}

// Transparent hash/eq for std::string-keyed unordered_map that allows
// std::string_view lookups without constructing a temporary std::string
// on every find (C++20 heterogeneous lookup needs is_transparent on
// both the hash and the equality functor).
struct sv_hash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};
struct sv_equal {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

// --- Structured runtime error ---

// Both backends throw this; try/catch machinery translates it into a
// culebra Object with `kind`/`message`/`line`/`col` fields. Inherits
// from std::runtime_error so unconverted call sites still work via
// the standard exception interface.
class CulebraError : public std::runtime_error {
 public:
  std::string kind;
  long line = 0;
  long col = 0;

  CulebraError(std::string k, std::string msg, long l = 0, long c = 0)
      : std::runtime_error(std::move(msg)),
        kind(std::move(k)),
        line(l),
        col(c) {}
};

// Absolute path to the running culebra executable, for re-spawning a worker
// copy of the interpreter (Sys.executable). macOS: _NSGetExecutablePath;
// Linux: /proc/self/exe. Empty string if it can't be resolved. Backend-neutral
// so interp and JIT both surface the same value.
inline std::string current_executable_path() {
#if defined(__APPLE__)
  uint32_t sz = 0;
  _NSGetExecutablePath(nullptr, &sz);  // first call reports the needed size
  std::string buf(sz, '\0');
  if (_NSGetExecutablePath(buf.data(), &sz) != 0) return "";
  buf.resize(std::strlen(buf.c_str()));
  char real[PATH_MAX];
  if (::realpath(buf.c_str(), real)) return real;
  return buf;
#elif defined(__linux__)
  char buf[PATH_MAX];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  return std::string(buf, static_cast<size_t>(n));
#else
  return "";
#endif
}

// Throw TypeError "takes N positional argument(s) but M given" when M
// exceeds the cap. `cap < 0` means no `*` separator (no cap); `cap ==
// 0` means a leading `*` (variadic via __ARGS__, no overflow error).
// Shared by interp's bind_call_args, the JIT static kwargs resolver,
// and the JIT dynamic-callee runtime guard — all three throw the same
// shape.
inline void throw_if_too_many_positionals(long cap, long n_pos,
                                           long line, long col) {
  if (cap <= 0 || n_pos <= cap) return;
  throw CulebraError("TypeError", std::format(
      "takes {} positional argument{} but {} given",
      cap, cap == 1 ? "" : "s", n_pos), line, col);
}

// Count-based ArityError message for a wrong-arity built-in method call,
// shared by both backends so interp/JIT/AOT emit byte-identical text. A
// fixed arity renders `'push' takes 1 argument but 3 given`; an optional
// range renders `'slice' takes 1 to 2 arguments but 3 given`. The `given`
// count drives nothing; `min`/`max` drive the singular/plural of the
// expected noun (no period — the printer appends ` at L:C.`).
inline std::string builtin_arity_error_message(std::string_view method,
                                               long min, long max, long got) {
  if (min == max) {
    return std::format("'{}' takes {} argument{} but {} given", method, min,
                       min == 1 ? "" : "s", got);
  }
  return std::format("'{}' takes {} to {} arguments but {} given", method, min,
                     max, got);
}

// Count-based arity error for a built-in *function* (namespace method or
// bare global) invoked as a value with the wrong number of positional args:
// "expected N positional argument(s), got M". Deliberately nameless: the
// interpreter does not carry the qualified name on these FunctionValues, so a
// nameless message lets both backends render byte-identical text for
// `Math.abs(1, 2)` and `let f = Math.abs; f(1, 2)` alike. Distinct from
// builtin_arity_error_message, which names value-type *methods*.
inline std::string ns_fn_arity_error_message(long expected, long got) {
  return std::format("expected {} positional argument{}, got {}", expected,
                     expected == 1 ? "" : "s", got);
}

// --- Numeric formatting / parsing ---

// Shortest round-trip decimal for a double, with a forced decimal point
// or exponent so Float display is visually distinguishable from Long
// (`1.0`, `0.5`, `-2.5`, `1e-05`, `nan`, `inf`, `-inf`). Shared by the
// interpreter's `Value::str_float` and the JIT's `_culebra_value_to_str_impl`.
inline std::string format_float_shortest(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buf[32];
  auto* end =
      std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general).ptr;
  std::string s(buf, end);
  if (s.find('.') == std::string::npos &&
      s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

// JSON.stringify quote escaping. Shared by both backends' stringify
// implementations. Control bytes (<0x20) emit as `\u00xx`.
inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

// Append code point `cp` to `out` as UTF-8. Out-of-range / surrogate code
// points fall back to U+FFFD, matching how the rest of the runtime handles
// invalid scalar values.
inline void append_utf8(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// HTML escape: the five characters that are unsafe in HTML text/attributes.
// `&` is replaced first so the others' replacements aren't re-escaped.
inline std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// Named character references covered by html_unescape. Not the full HTML5
// table (~2200 entries) — a practical set of the common typographic, Latin-1,
// Greek, math, and currency entities. Unknown names are left verbatim.
inline const std::unordered_map<std::string_view, std::string_view, sv_hash,
                                std::equal_to<>>&
html_named_entities() {
  static const std::unordered_map<std::string_view, std::string_view, sv_hash,
                                  std::equal_to<>> table = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
    {"nbsp", " "}, {"shy", "­"},
    {"copy", "©"}, {"reg", "®"}, {"trade", "™"},
    {"hellip", "…"}, {"mdash", "—"}, {"ndash", "–"},
    {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"},
    {"rdquo", "”"}, {"sbquo", "‚"}, {"bdquo", "„"},
    {"laquo", "«"}, {"raquo", "»"}, {"lsaquo", "‹"},
    {"rsaquo", "›"}, {"bull", "•"}, {"middot", "·"},
    {"dagger", "†"}, {"Dagger", "‡"}, {"prime", "′"},
    {"Prime", "″"}, {"permil", "‰"},
    {"emsp", " "}, {"ensp", " "}, {"thinsp", " "},
    {"deg", "°"}, {"plusmn", "±"}, {"times", "×"},
    {"divide", "÷"}, {"frac12", "½"}, {"frac14", "¼"},
    {"frac34", "¾"}, {"sup1", "¹"}, {"sup2", "²"},
    {"sup3", "³"}, {"micro", "µ"}, {"not", "¬"},
    {"sect", "§"}, {"para", "¶"}, {"iexcl", "¡"},
    {"iquest", "¿"}, {"ordf", "ª"}, {"ordm", "º"},
    {"euro", "€"}, {"pound", "£"}, {"yen", "¥"},
    {"cent", "¢"}, {"curren", "¤"},
    {"larr", "←"}, {"uarr", "↑"}, {"rarr", "→"},
    {"darr", "↓"}, {"harr", "↔"},
    {"infin", "∞"}, {"ne", "≠"}, {"le", "≤"}, {"ge", "≥"},
    {"asymp", "≈"}, {"equiv", "≡"}, {"sum", "∑"},
    {"prod", "∏"}, {"radic", "√"}, {"part", "∂"},
    {"nabla", "∇"}, {"int", "∫"}, {"minus", "−"},
    {"agrave", "à"}, {"aacute", "á"}, {"acirc", "â"},
    {"atilde", "ã"}, {"auml", "ä"}, {"aring", "å"},
    {"aelig", "æ"}, {"ccedil", "ç"}, {"egrave", "è"},
    {"eacute", "é"}, {"ecirc", "ê"}, {"euml", "ë"},
    {"igrave", "ì"}, {"iacute", "í"}, {"icirc", "î"},
    {"iuml", "ï"}, {"eth", "ð"}, {"ntilde", "ñ"},
    {"ograve", "ò"}, {"oacute", "ó"}, {"ocirc", "ô"},
    {"otilde", "õ"}, {"ouml", "ö"}, {"oslash", "ø"},
    {"ugrave", "ù"}, {"uacute", "ú"}, {"ucirc", "û"},
    {"uuml", "ü"}, {"yacute", "ý"}, {"thorn", "þ"},
    {"yuml", "ÿ"}, {"szlig", "ß"},
    {"Agrave", "À"}, {"Aacute", "Á"}, {"Acirc", "Â"},
    {"Atilde", "Ã"}, {"Auml", "Ä"}, {"Aring", "Å"},
    {"AElig", "Æ"}, {"Ccedil", "Ç"}, {"Egrave", "È"},
    {"Eacute", "É"}, {"Ecirc", "Ê"}, {"Euml", "Ë"},
    {"Igrave", "Ì"}, {"Iacute", "Í"}, {"Icirc", "Î"},
    {"Iuml", "Ï"}, {"ETH", "Ð"}, {"Ntilde", "Ñ"},
    {"Ograve", "Ò"}, {"Oacute", "Ó"}, {"Ocirc", "Ô"},
    {"Otilde", "Õ"}, {"Ouml", "Ö"}, {"Oslash", "Ø"},
    {"Ugrave", "Ù"}, {"Uacute", "Ú"}, {"Ucirc", "Û"},
    {"Uuml", "Ü"}, {"Yacute", "Ý"}, {"THORN", "Þ"},
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"},
    {"delta", "δ"}, {"epsilon", "ε"}, {"theta", "θ"},
    {"lambda", "λ"}, {"mu", "μ"}, {"pi", "π"},
    {"sigma", "σ"}, {"phi", "φ"}, {"omega", "ω"},
    {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"},
    {"Lambda", "Λ"}, {"Pi", "Π"}, {"Sigma", "Σ"},
    {"Phi", "Φ"}, {"Omega", "Ω"},
  };
  return table;
}

// HTML unescape: turn entity references back into their characters. Handles
// numeric refs `&#DDD;` / `&#xHHH;` (any case) plus the named set in
// html_named_entities(). A reference must end in `;`; anything that isn't a
// recognized, well-formed reference is left exactly as written (browser-style
// leniency), so `&` on its own and unknown entities pass through unchanged.
inline std::string html_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') { out += s[i++]; continue; }
    size_t semi = s.find(';', i + 1);
    // Bound the search so a stray '&' doesn't scan the whole string.
    if (semi == std::string_view::npos || semi - i > 32) {
      out += s[i++];
      continue;
    }
    std::string_view body = s.substr(i + 1, semi - i - 1);
    bool replaced = false;
    if (!body.empty() && body[0] == '#') {  // numeric reference
      uint32_t cp = 0;
      bool ok = false;
      std::string_view digits = body.substr(1);
      if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
        digits = digits.substr(1);
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(),
                                 cp, 16);
        ok = !digits.empty() && r.ec == std::errc{} &&
             r.ptr == digits.data() + digits.size();
      } else {
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(),
                                 cp, 10);
        ok = !digits.empty() && r.ec == std::errc{} &&
             r.ptr == digits.data() + digits.size();
      }
      if (ok) {
        append_utf8(out, cp);
        replaced = true;
      }
    } else {  // named reference
      const auto& table = html_named_entities();
      if (auto it = table.find(body); it != table.end()) {
        out += it->second;
        replaced = true;
      }
    }
    if (replaced) {
      i = semi + 1;
    } else {
      out += s[i++];  // leave the '&' as-is and keep scanning
    }
  }
  return out;
}

// Base64 (RFC 4648, standard alphabet, `=` padding). Shared by the interp and
// JIT `Encoding.base64.*` adapters.
inline std::string base64_encode(std::string_view in) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  auto u = [&](size_t k) { return static_cast<unsigned char>(in[k]); };
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned n = (u(i) << 16) | (u(i + 1) << 8) | u(i + 2);
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += tbl[n & 63];
  }
  if (size_t rem = in.size() - i; rem == 1) {
    unsigned n = u(i) << 16;
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned n = (u(i) << 16) | (u(i + 1) << 8);
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

// Decode standard base64. Returns nullopt on an invalid character; ASCII
// whitespace (so wrapped base64 decodes) and `=` padding are tolerated.
inline std::optional<std::string> base64_decode(std::string_view in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(in.size() / 4 * 3);
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=') break;  // padding → end of data
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    int v = val(c);
    if (v < 0) return std::nullopt;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buf >> bits) & 0xFF);
    }
  }
  return out;
}

// One hex digit → its value, or -1 if not [0-9a-fA-F]. Shared by hex_decode
// and url_decode (the percent-escape body is two hex digits).
inline int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Lower-case hex encode: each byte becomes two hex digits. Shared by the
// interp and JIT `Encoding.hex.*` adapters.
inline std::string hex_encode(std::string_view in) {
  static const char tbl[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out += tbl[c >> 4];
    out += tbl[c & 0xF];
  }
  return out;
}

// Decode hex. Returns nullopt on an odd length or any non-hex character;
// both upper- and lower-case digits are accepted.
inline std::optional<std::string> hex_decode(std::string_view in) {
  if (in.size() % 2 != 0) return std::nullopt;
  std::string out;
  out.reserve(in.size() / 2);
  for (size_t i = 0; i < in.size(); i += 2) {
    int hi = hex_digit(in[i]), lo = hex_digit(in[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out += static_cast<char>((hi << 4) | lo);
  }
  return out;
}

// Percent-encode per RFC 3986: the unreserved set `A-Za-z0-9-_.~` is kept
// verbatim, every other byte becomes `%XX` with upper-case hex (so a space
// is `%20`, not `+`). Shared by the interp and JIT `Encoding.url.*` adapters.
inline std::string url_encode(std::string_view in) {
  static const char tbl[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += tbl[c >> 4];
      out += tbl[c & 0xF];
    }
  }
  return out;
}

// Decode percent-encoding: `%XX` (either hex case) becomes its byte. A `%`
// not followed by two hex digits is left verbatim (lenient, like Python's
// urllib unquote), and `+` stays literal so url_encode/url_decode round-trip
// exactly. Never fails.
inline std::string url_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hex_digit(in[i + 1]), lo = hex_digit(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 3;
        continue;
      }
    }
    out += in[i++];
  }
  return out;
}

// Location lives in the CulebraError's line/col fields; the top-level
// printer (src/main.cc) appends ` at L:C.` once, so messages must not
// embed it themselves (else it prints twice).
[[noreturn]] inline void throw_type_error_at(long line, long col) {
  throw CulebraError("TypeError", "type error", line, col);
}

// Shared by interp's `eval_destructure_assign` and JIT's
// `compile_destructure_assign` so both backends report the same
// structured error (`ValueError` + descriptive message + location)
// when an Object / Array / Tuple pattern fails to match its rval.
[[noreturn]] inline void throw_destructure_mismatch_at(long line, long col) {
  throw CulebraError("ValueError",
                     "destructure pattern did not match value", line, col);
}

// `o.x += rhs` against a missing property `x`. Both backends used to
// diverge here — interp checked existence and threw AttributeError,
// JIT read the missing slot as nil and then threw TypeError on
// `nil + rhs`. This helper unifies the kind + message + location.
[[noreturn]] inline void throw_compound_missing_property_at(
    long line, long col) {
  throw CulebraError("AttributeError",
                     "compound assignment on missing property.",
                     line, col);
}

// Reassigning a `let` (non-mut) binding. Interp tracks per-binding
// mut via the `Symbol`'s `mut` flag; the JIT carries `mut` on
// `VarSlot` and routes here when a write hits a non-mut slot.
[[noreturn]] inline void throw_immutable_assign_at(
    const std::string& name, long line, long col) {
  throw CulebraError("ImmutableError",
                     std::format("cannot reassign '{}' (declared without 'mut')",
                                 name),
                     line, col);
}

// Raise the uniform shadow-prohibition error. Used by interp and JIT at
// every binding site that would shadow a closure-captured outer variable
// (let/mut declarations, function parameters, match pattern bindings).
[[noreturn]] inline void throw_shadow_error(std::string_view name,
                                            size_t line, size_t column) {
  throw CulebraError(
      "ShadowError",
      std::format("cannot shadow outer variable '{}' (declared in an enclosing "
                  "function)",
                  name),
      static_cast<long>(line), static_cast<long>(column));
}

// Call site passed a keyword the callee doesn't accept. Both backends
// used to diverge: interp threw at runtime (catchable by try/catch),
// JIT raised at IR-emit time (uncatchable). This helper unifies them
// to runtime throws.
[[noreturn]] inline void throw_unknown_kwarg_at(
    const std::string& name, long line, long col) {
  throw CulebraError("TypeError",
                     std::format("unknown keyword argument '{}'", name),
                     line, col);
}

// Call site failed to bind a required parameter (no positional, no
// kwarg, no default). Same backend-asymmetry rationale as the unknown-
// kwarg helper above.
[[noreturn]] inline void throw_missing_required_arg_at(
    const std::string& name, long line, long col) {
  throw CulebraError("ArityError",
                     std::format("missing required argument '{}'", name),
                     line, col);
}

// Generic runtime throw for cases where the JIT used to detect the
// error at IR-emit time (uncatchable) while the interp threw at
// eval time (catchable). Both backends now route through this helper
// to keep `try { ... } catch e { e.kind }` semantics symmetric.
[[noreturn]] inline void throw_runtime_error_at(
    const std::string& kind, const std::string& msg,
    long line, long col) {
  throw CulebraError(kind, msg, line, col);
}

// Canonical arithmetic type error: `1 + "a"`, `nil + nil`, `2 ** "a"`,
// `1 @ 2`, etc. Single message source for both backends — interp's
// `arith_dispatch` / `compute_power` and the JIT's typed fast-path and
// `_arith_*` helpers all route here so the wording stays in lockstep.
[[noreturn]] inline void throw_arith_type_error(std::string_view op,
                                                std::string_view lhs,
                                                std::string_view rhs,
                                                long line = 0, long col = 0) {
  throw CulebraError("TypeError",
                     "type error: cannot apply '" + std::string(op) +
                         "' to " + std::string(lhs) + " and " +
                         std::string(rhs),
                     line, col);
}

// Canonical comparison type error for `<`, `<=`, `>`, `>=`: cross-type
// (`1 < "a"`) and same-type-unorderable (`[1] < [2]`, `{} < {}`). Both
// backends single-source from here.
[[noreturn]] inline void throw_compare_type_error(std::string_view lhs,
                                                  std::string_view rhs,
                                                  long line = 0,
                                                  long col = 0) {
  throw CulebraError("TypeError",
                     "type error: cannot compare " + std::string(lhs) +
                         " and " + std::string(rhs),
                     line, col);
}

// Canonical "expected X, got Y" type error. Used for unary negation
// (`-"a"` → expected "Long or Float") and any other single-operand type
// guard shared between backends.
[[noreturn]] inline void throw_type_mismatch(std::string_view expected,
                                             std::string_view got,
                                             long line = 0, long col = 0) {
  throw CulebraError("TypeError",
                     "type error: expected " + std::string(expected) +
                         ", got " + std::string(got),
                     line, col);
}

// Integer power by squaring. `exp` must be non-negative; result wraps
// on overflow (matches the rest of Long arithmetic — no bignum).
inline long ipow_nonneg(long base, long exp) {
  long r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    exp >>= 1;
    if (exp > 0) base *= base;
  }
  return r;
}

// Trim ASCII whitespace from both ends of a string view, returning the
// substring. Shared by the numeric string parsers below.
inline std::string_view trim_ascii(std::string_view s) {
  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) j--;
  return s.substr(i, j - i);
}

// ASCII-only case fold. Range-checked so the result is locale-
// independent — std::toupper/tolower consult the C locale and would
// fold Latin-1 bytes too under e.g. en_US.UTF-8. Non-ASCII bytes
// (>= 0x80) pass through unchanged, matching the "ASCII uppercase /
// lowercase" contract in docs/language.md §17.1.
inline std::string ascii_upper(std::string s) {
  for (auto& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}
inline std::string ascii_lower(std::string s) {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// Byte length of the UTF-8 scalar that starts at leading byte `c`. A
// continuation or invalid byte reports 1, so callers always advance.
inline size_t utf8_scalar_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

// Trim scalars in `chars` from one or both ends. An empty `chars` trims
// ASCII whitespace (the no-arg `trim`/`trim_start`/`trim_end` default).
// Returns a view into `s`. Shared by interp + JIT so both agree.
inline std::string_view trim_chars(std::string_view s, std::string_view chars,
                                   bool left, bool right) {
  auto in_set = [&](std::string_view ch) {
    if (chars.empty()) {
      return ch.size() == 1 && std::isspace(static_cast<unsigned char>(ch[0]));
    }
    for (size_t i = 0; i < chars.size();) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(chars[i]));
      if (i + n > chars.size()) n = 1;
      if (chars.substr(i, n) == ch) return true;
      i += n;
    }
    return false;
  };
  size_t b = 0, e = s.size();
  if (left) {
    while (b < e) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(s[b]));
      if (b + n > e) n = 1;
      if (!in_set(s.substr(b, n))) break;
      b += n;
    }
  }
  if (right) {
    while (e > b) {
      size_t st = e - 1;  // walk back over UTF-8 continuation bytes
      while (st > b && (static_cast<unsigned char>(s[st]) & 0xC0) == 0x80) st--;
      if (!in_set(s.substr(st, e - st))) break;
      e = st;
    }
  }
  return s.substr(b, e - b);
}

// Per-scalar translation (Ruby `String#tr`, character-list form — no `a-z`
// ranges or `^` negation). Each Unicode scalar of `s` that appears in `from`
// is replaced by the scalar at the same position in `to`; if `to` is shorter
// its last scalar repeats, and an empty `to` deletes the matched scalars.
// Shared by the interp String method and the JIT runtime so both agree.
inline std::string str_tr(std::string_view s, std::string_view from,
                          std::string_view to) {
  auto scalars = [](std::string_view x) {
    std::vector<std::string_view> v;
    for (size_t i = 0; i < x.size();) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(x[i]));
      if (i + n > x.size()) n = 1;
      v.push_back(x.substr(i, n));
      i += n;
    }
    return v;
  };
  auto from_s = scalars(from);
  auto to_s = scalars(to);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = utf8_scalar_len(static_cast<unsigned char>(s[i]));
    if (i + n > s.size()) n = 1;
    std::string_view ch = s.substr(i, n);
    auto it = std::find(from_s.begin(), from_s.end(), ch);
    if (it == from_s.end()) {
      out += ch;
    } else if (!to_s.empty()) {
      size_t idx = static_cast<size_t>(it - from_s.begin());
      out += to_s[idx < to_s.size() ? idx : to_s.size() - 1];
    }  // else: `to` empty → delete (append nothing)
    i += n;
  }
  return out;
}

// True iff `name` has a `|` at the outermost bracket depth (i.e. a
// top-level Union alt separator). `Array<Long | Float>` returns
// false — the `|` lives inside `<...>`. Callers gate the Union
// branch on this so a single Generic-with-inner-Union doesn't
// recurse forever via the otherwise-safe `find('|') != npos` check.
inline bool has_toplevel_pipe(std::string_view name) {
  int depth = 0;
  for (char c : name) {
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '|' && depth == 0) return true;
  }
  return false;
}

// Split a pipe-separated Union type annotation into its component
// type names. `Long | Float` → ["Long", "Float"]. Whitespace around
// each candidate is trimmed. A single type name (no pipe) returns
// the input unchanged as the only element. Empty input yields an
// empty vector. Shared by both backends' type checks.
//
// Generic-aware: `|` inside `<...>` does not split, so
// `Array<Long | Float>` stays one candidate. Depth is tracked across
// the whole string; unbalanced `>` are clamped to 0.
//
// DEFENSIVE: empty alternatives (e.g. `Long ||  | Float`, leading
// `|`, trailing `|`) are silently skipped. Grammar prevents these
// from reaching here in normal source, but the helper is also
// called on canonicalized strings during multifn dedup so we don't
// rely on grammar alone.
//
// LIFETIME: each returned string_view aliases bytes inside `name`;
// the caller must keep `name`'s underlying storage alive while
// using the returned views. Do not pass a temporary.
inline std::vector<std::string_view> split_union_types(
    std::string_view name) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '|' && depth == 0) {
      auto cand = trim_ascii(name.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(name.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// True iff `name` has a `+` at the outermost bracket depth — the
// intersection separator used in composite Generic bounds
// (`T: Hashable + Stringer`, lowered to `"Hashable + Stringer"`).
// Depth-aware to mirror has_toplevel_pipe, though bound atoms are
// bare trait names today and never carry `<...>`.
inline bool has_toplevel_plus(std::string_view name) {
  int depth = 0;
  for (char c : name) {
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '+' && depth == 0) return true;
  }
  return false;
}

// Split a `+`-separated intersection bound into its component trait
// names. `Hashable + Stringer` → ["Hashable", "Stringer"]. A value
// satisfies the bound when it conforms to *all* parts (all-of), the
// dual of split_union_types' any-of. Whitespace is trimmed; empty
// parts are skipped. Mirrors split_union_types (incl. LIFETIME: each
// view aliases `name`, so don't pass a temporary).
inline std::vector<std::string_view> split_intersection_types(
    std::string_view name) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == '+' && depth == 0) {
      auto cand = trim_ascii(name.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(name.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a comma-separated Generic argument list into its components,
// respecting nested `<...>`. `Long, Map<String, Long>` →
// ["Long", "Map<String, Long>"]. Empty args yield empty vector.
inline std::vector<std::string_view> split_generic_args(
    std::string_view args) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < args.size(); i++) {
    char c = args[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == ',' && depth == 0) {
      auto cand = trim_ascii(args.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(args.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a single (non-Union) type name into outer + generic args.
// `Array<Long>` → {"Array", "Long"}; `Map<String, Long>` →
// {"Map", "String, Long"}; `Long` → {"Long", ""}.
// The args view is the raw inside-the-brackets text (callers split
// it further with `split_generic_args`).
//
// DEFENSIVE: malformed inputs (`<T>` with empty outer, `Array<Long>>`
// or `Array<<Long>` with unbalanced brackets) fall back to the whole
// trimmed string as the outer with no args, rather than yielding a
// degenerate split. Grammar normally prevents these from reaching
// here, but the helper is exported and may be called on
// programmatically-constructed type strings.
//
// LIFETIME: outer and args alias bytes inside `name`; caller must
// keep `name`'s underlying storage alive while using them. Do not
// pass a temporary std::string.
struct GenericHead {
  std::string_view outer;
  std::string_view args;  // empty if no <...>
};
inline GenericHead parse_generic_head(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return {trimmed, {}};
  auto pos = trimmed.find('<');
  if (pos == std::string_view::npos || trimmed.back() != '>') {
    return {trimmed, {}};
  }
  // Outer name must be non-empty.
  if (pos == 0) return {trimmed, {}};
  // Brackets must balance — reject `Array<Long>>` and `Array<<Long>`.
  int depth = 0;
  for (char c : trimmed) {
    if (c == '<') depth++;
    else if (c == '>') {
      depth--;
      if (depth < 0) return {trimmed, {}};
    }
  }
  if (depth != 0) return {trimmed, {}};
  return {
    trim_ascii(trimmed.substr(0, pos)),
    trimmed.substr(pos + 1, trimmed.size() - pos - 2),
  };
}

// Parse a type-parameter declaration like `T` or `T: Comparable` into
// its name and (possibly empty) bound. Used by class / trait / fn
// declarations to extract Generic params from CLASS_HEAD tokens.
struct TypeParam {
  std::string_view name;
  std::string_view bound;   // empty = no bound
};
inline TypeParam parse_type_param(std::string_view raw) {
  auto trimmed = trim_ascii(raw);
  auto pos = trimmed.find(':');
  if (pos == std::string_view::npos) return {trimmed, {}};
  return {trim_ascii(trimmed.substr(0, pos)),
          trim_ascii(trimmed.substr(pos + 1))};
}

// Split a TRAIT_HEAD token into the trait name (with any Generic
// params still attached) and its supertrait list. `Ord: Eq` →
// {"Ord", ["Eq"]}; `Cmp<T>: Eq, Show` → {"Cmp<T>", ["Eq", "Show"]};
// `Foo` → {"Foo", {}}. The name/supertrait split is on the first `:`
// at bracket depth 0 so a type-param bound (`Box<T: Bound>`) inside
// `<...>` is not mistaken for the supertrait separator. Each view
// aliases `token` — keep it alive (do not pass a temporary).
struct TraitHead {
  std::string_view name;                     // may include `<...>`
  std::vector<std::string_view> supertraits;
};
inline TraitHead parse_trait_head(std::string_view token) {
  auto trimmed = trim_ascii(token);
  int depth = 0;
  size_t colon = std::string_view::npos;
  for (size_t i = 0; i < trimmed.size(); i++) {
    char c = trimmed[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == ':' && depth == 0) { colon = i; break; }
  }
  if (colon == std::string_view::npos) return {trimmed, {}};
  TraitHead out;
  out.name = trim_ascii(trimmed.substr(0, colon));
  out.supertraits = split_generic_args(trimmed.substr(colon + 1));
  return out;
}

// Lower every occurrence of a type-param name (`T`, `K`, ...) inside
// `tn` to its runtime check target, including nested forms like
// `Array<T>` and `T | Long`. An *unbounded* param (`T`) lowers to
// "Any" (the annotation was documentation, so the check is a no-op).
// A *bounded* param (`T: Comparable`) lowers to its bound trait name
// (`Comparable`) so the existing trait-conformance machinery enforces
// the bound at dispatch / type_check time — no separate generic
// dispatch path is needed. Used by both class-method and free-fn
// signature neutralization. Returns the result in canonical form
// (single-space `|`, comma-space-separated generic args).
inline std::string lower_type_params(
    std::string_view tn,
    const std::vector<std::string_view>& type_params) {
  auto trimmed = trim_ascii(tn);
  if (trimmed.empty()) return "";
  // Top-level Union (depth-aware): rewrite each alt, join with " | ".
  if (has_toplevel_pipe(trimmed)) {
    std::string out;
    bool first = true;
    for (auto cand : split_union_types(trimmed)) {
      if (!first) out += " | ";
      out += lower_type_params(cand, type_params);
      first = false;
    }
    return out;
  }
  auto head = parse_generic_head(trimmed);
  // Outer alone matches a type-param: the whole annotation collapses to
  // the param's lowered form (so `T`, `T<Long>`, etc. all become "Any"
  // for an unbounded param, or the bound trait name for a bounded one —
  // the generic args were documentation anyway).
  for (auto tp_raw : type_params) {
    auto tp = parse_type_param(tp_raw);
    if (head.outer != tp.name) continue;
    if (tp.bound.empty()) return "Any";
    // Composite bound (`A + B`): normalize spacing to a canonical
    // " + "-joined form so dispatch-key dedup compares equal regardless
    // of source whitespace. Matching itself is spacing-insensitive
    // (split_intersection_types trims), but the registered key isn't.
    if (!has_toplevel_plus(tp.bound)) return std::string(tp.bound);
    std::string out;
    bool first = true;
    for (auto part : split_intersection_types(tp.bound)) {
      if (!first) out += " + ";
      out += part;
      first = false;
    }
    return out;
  }
  if (head.args.empty()) return std::string(head.outer);
  // Generic: keep outer, recurse into each arg.
  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += lower_type_params(a, type_params);
    first = false;
  }
  out += '>';
  return out;
}

// Canonical form for a (possibly Union, possibly Generic) type
// annotation, used in fn.params introspection and multifn
// redeclaration matching so whitespace variants of the same
// type compare equal.
//
// `Long|Float`               → `Long | Float`
// `Array<Long|Float>`        → `Array<Long | Float>`
// `Array < Long , Float >`   → `Array<Long, Float>`
// `Long | Long`              → `Long`         (dedup at top level)
// Empty input                → ""
//
// Recursive: nested `<...>` are canonicalized as well. Owned
// std::string so storage outlives `name`.
inline std::string canonicalize_type_annotation(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return "";

  auto cands = split_union_types(trimmed);
  if (cands.size() > 1) {
    // Top-level Union — canonicalize each candidate and dedup. Linear
    // dedup against canonical forms covers `Array<Long> | Array<Long>`
    // (different whitespace inside the args still collapses).
    std::string out;
    std::vector<std::string> seen;
    for (auto cand : cands) {
      auto canon = canonicalize_type_annotation(cand);
      bool dup = false;
      for (auto& s : seen) if (s == canon) { dup = true; break; }
      if (dup) continue;
      seen.push_back(canon);
      if (!out.empty()) out += " | ";
      out += canon;
    }
    return out;
  }

  // `T?` Optional sugar -> `T | Nil`. The `?` binds to a single type
  // name; top-level unions were already split above, so a `?` here is on
  // one alternative (`Foo?`, `Array<Long>?`). Dedup keeps `Nil?` == `Nil`.
  if (trimmed.back() == '?') {
    auto base = canonicalize_type_annotation(trimmed.substr(0, trimmed.size() - 1));
    if (base.empty() || base == "Nil") return "Nil";
    return base + " | Nil";
  }

  // Single type — check for Generic.
  auto head = parse_generic_head(trimmed);
  if (head.args.empty()) return std::string(head.outer);

  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += canonicalize_type_annotation(a);
    first = false;
  }
  out += '>';
  return out;
}

// Parse a full string as a base-10 signed long; whitespace-trim allowed,
// any other trailing content or invalid form throws `type error at L:C`.
inline long parse_long_strict(std::string_view s, long line, long col) {
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    long v = std::stol(owned, &used, 10);
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0;  // unreachable
}

// Parse an integer literal token to a signed long, recognizing the
// `0x` / `0o` / `0b` radix prefixes (hex / octal / binary) in addition
// to plain decimal. The grammar (NUMBER rule) guarantees the digit set
// per prefix, so this is a clean base dispatch with no validation churn.
// Shared by interp (eval_number) and JIT (compile NUMBER) so the two
// backends decode literals identically.
inline long parse_integer_literal(std::string_view tok) {
  std::string owned(tok);
  int base = 10;
  size_t off = 0;
  if (tok.size() > 2 && tok[0] == '0') {
    char p = tok[1];
    if (p == 'x' || p == 'X') { base = 16; off = 2; }
    else if (p == 'o' || p == 'O') { base = 8; off = 2; }
    else if (p == 'b' || p == 'B') { base = 2; off = 2; }
  }
  // strtol over the body after any prefix; the grammar restricted the
  // digits, so no partial-parse guard is needed. errno is checked so an
  // out-of-range literal throws instead of silently clamping to
  // LONG_MAX/MIN (matches parse_long_strict's strict overflow handling).
  errno = 0;
  long v = std::strtol(owned.c_str() + off, nullptr, base);
  if (errno == ERANGE) {
    throw CulebraError("ValueError",
                       std::format("integer literal out of range: {}", tok));
  }
  return v;
}

// --- Interpolation format spec (`"{x:.2f}"`) ----------------------------
//
// The mini-language is delegated to std::format, whose spec syntax is
// Python-derived (`[[fill]align][sign][#][0][width][grp][.prec][type]`).
// We wrap `{:<spec>}` around the captured spec and vformat the value.
// Shared by interp and JIT so both render identically. A bad spec throws
// a ValueError (mapped from std::format_error).

// Trailing type char of a spec (`f`, `x`, …), or 0 if none.
inline char format_spec_type(std::string_view spec) {
  if (spec.empty()) return 0;
  char c = spec.back();
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '%') return c;
  return 0;
}
inline bool format_spec_wants_float(std::string_view spec) {
  char c = format_spec_type(spec);
  return c == 'e' || c == 'E' || c == 'f' || c == 'F' || c == 'g' ||
         c == 'G' || c == '%' || c == 'a' || c == 'A';
}
inline bool format_spec_wants_int(std::string_view spec) {
  char c = format_spec_type(spec);
  return c == 'b' || c == 'c' || c == 'd' || c == 'o' || c == 'x' || c == 'X';
}

// Common core: wrap `{:<spec>}`, vformat, map format_error to a
// ValueError tagged with the value's culebra type name.
template <typename T>
inline std::string format_value_as(T& v, std::string_view type_name,
                                   std::string_view spec, long line, long col) {
  std::string fmt = "{:";
  fmt += spec;
  fmt += "}";
  try {
    return std::vformat(fmt, std::make_format_args(v));
  } catch (const std::format_error&) {
    throw CulebraError("ValueError",
        std::format("invalid format spec '{}' for {}", spec, type_name), line,
        col);
  }
}
inline std::string format_value_long(long v, std::string_view spec, long line,
                                     long col) {
  return format_value_as(v, "Long", spec, line, col);
}
inline std::string format_value_double(double v, std::string_view spec,
                                       long line, long col) {
  return format_value_as(v, "Float", spec, line, col);
}
inline std::string format_value_string(std::string v, std::string_view spec,
                                       long line, long col) {
  return format_value_as(v, "String", spec, line, col);
}

// Parse a full string as a double; same trim / full-consumption rules.
inline double parse_double_strict(std::string_view s, long line, long col) {
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    double v = std::stod(owned, &used);
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0.0;  // unreachable
}

// --- Runtime context ---

// Owns all per-instance VM state. Multiple Runtimes can coexist on the
// same thread; RuntimeScope swaps the active one. Single-Runtime
// embedders never construct one — a lazy thread-local default fills in.
//
// Heavy types (InterpGC, ShapeRegistry, ...) live in type-erased slots
// because they're defined in headers that include shared.h, so they
// can't be direct members.
enum RuntimeSlot : size_t {
  kSlotInterpGc = 0,
  kSlotJitGc,
  kSlotShapeRegistry,  // reserved/unused: the Shape intern table is now a
                       // process-global singleton (see jit.h ShapeRegistry) —
                       // Shapes are shared immutable metadata, not isolated heap
  kSlotDeferStack,
  kSlotJitHooks,
  kSlotJitModuleTable,
  kSlotJitNamespaceTable,
  kSlotTestRegistry,
  kSlotFileTable,
  kRuntimeSlotCount
};

struct Runtime {
  std::mt19937_64 random_engine{std::random_device{}()};
  std::vector<std::string> sys_argv;

  // JIT exception carriers. Set by `culebra_runtime_throw`, read by
  // the try/catch landing pad. See jit.h for the protocol.
  int8_t thrown_tag = 0;
  int64_t thrown_data = 0;
  int8_t is_throw = 0;

  // Cooperative cancellation. When non-null and set, the interpreter's
  // statement dispatch throws `Interrupted` to unwind this thread (an isolate
  // being cancelled / dropped). The flag itself lives on the owning IsolateCore
  // (isolate.h); the Runtime only borrows a pointer. Null on the main thread.
  std::atomic<bool>* interrupt_flag = nullptr;

  std::array<void*, kRuntimeSlotCount> substate{};
  std::array<void (*)(void*), kRuntimeSlotCount> substate_deleter{};

  Runtime() = default;
  ~Runtime() {
    // Destroy substates in reverse slot order. The GC substates (kSlotInterpGc,
    // kSlotJitGc) are the lowest slots and MUST outlive the JitValue-holding
    // substates (module/namespace tables, test registry, defer stack), whose
    // destructors release values back into the GC. Forward order frees the GC
    // first, leaving those later releases to call into a destroyed heap. Null
    // each slot after deleting so any release that still resolves a substate
    // mid-teardown lazily revives an empty one instead of touching freed memory.
    for (size_t i = substate.size(); i-- > 0;) {
      if (substate[i] && substate_deleter[i]) {
        substate_deleter[i](substate[i]);
        substate[i] = nullptr;
      }
    }
  }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
};

inline thread_local Runtime* _culebra_current_runtime = nullptr;

inline Runtime& default_runtime() {
  static thread_local Runtime rt;
  return rt;
}

inline Runtime& current_runtime() {
  return _culebra_current_runtime ? *_culebra_current_runtime
                                  : default_runtime();
}

// True when the current isolate has been asked to cancel (see Runtime::
// interrupt_flag). Polled by the statement dispatch and by blocking channel
// waits. Always false on the main thread (no flag installed).
inline bool interrupt_requested() {
  auto* f = current_runtime().interrupt_flag;
  return f && f->load(std::memory_order_relaxed);
}

// --- Ctrl+C (SIGINT) cooperative interruption ---------------------------
//
// A process-wide one-shot flag set by the installed SIGINT handler. The main
// thread's Runtime::interrupt_flag and the main interpreter's poll both point
// here, and the JIT/AOT loop safepoint reads the same symbol inline. It is
// distinct from an isolate's cancel flag (sticky + per-isolate): a Ctrl+C is
// consumed when the cooperative `Interrupted` is thrown, so a `catch` can
// resume (Python's KeyboardInterrupt model). `extern "C"` + `used` keeps the
// symbol resolvable by the in-process JIT and linkable by AOT output.
extern "C" {
__attribute__((used)) inline std::atomic<bool> culebra_g_sigint{false};
// The JIT/AOT loop safepoint inlines a load of THIS flag — not the per-thread
// interrupt flag, which inlined codegen can't cheaply read. Set whenever ANY
// interrupt becomes pending (a real Ctrl+C OR a per-isolate cancel), so a tight
// JIT loop branches to the cold slow path; the slow path then consults the
// per-thread `interrupt_flag` for the truth (Ctrl+C vs which isolate). Cleared
// once nothing remains pending, so loops return to the one-load fast path.
__attribute__((used)) inline std::atomic<bool> culebra_g_wake{false};
}

// Count of isolates cancelled but not yet reaped. Keeps `culebra_g_wake` set
// while a cancelled JIT isolate may still be spinning toward its next safepoint;
// the last reap (IsolateCore's dtor) clears the wake. Host-side only — no JIT IR
// references it.
inline std::atomic<int> culebra_g_cancel_pending{0};

// Consume a pending Ctrl+C (so a `catch` can resume) and release the JIT wake
// flag when nothing else is pending. Shared by the interp poll, the JIT
// safepoint, and the stdin poll so the wake is dropped uniformly.
inline void _consume_sigint() {
  culebra_g_sigint.store(false, std::memory_order_relaxed);
  if (culebra_g_cancel_pending.load(std::memory_order_relaxed) == 0) {
    culebra_g_wake.store(false, std::memory_order_relaxed);
  }
}

// Signal.notify (Go's signal.Notify model). When a program registers a channel
// to receive Ctrl+C, `culebra_g_signal_notify` is set: the handler then relays
// the signal to a delivery thread (via `culebra_g_signal_pending`) instead of
// latching the throw flag, so the running code keeps going and the program
// drives its own shutdown by reading the channel. Both are plain host-side
// atomics (no JIT IR references them). The registry + delivery thread live in
// isolate.h; here is only the handler's branch. See [[project_concurrency]].
inline std::atomic<bool> culebra_g_signal_notify{false};
inline std::atomic<bool> culebra_g_signal_pending{false};

// True iff `f` is the process SIGINT flag (a Ctrl+C) rather than a per-isolate
// cancel — the poll uses this to pick the message and the one-shot consume.
inline bool is_sigint_flag(const std::atomic<bool>* f) {
  return f == &culebra_g_sigint;
}

// Async-signal-safe SIGINT handler: the first Ctrl+C latches the flag; a second
// one (while the first is still pending / unhandled) restores the default
// disposition and re-raises, so a wedged program that never reaches a safepoint
// can still be force-killed. Only atomic ops + signal()/raise() run here.
inline void _culebra_sigint_handler(int sig) {
  // Notify mode (Signal.notify): relay to the registered channel via the
  // delivery thread instead of throwing. No force-kill escalation — the program
  // opted to handle signals itself (Signal.reset() restores default behavior).
  if (culebra_g_signal_notify.load(std::memory_order_relaxed)) {
    culebra_g_signal_pending.store(true, std::memory_order_relaxed);
    return;
  }
  if (culebra_g_sigint.exchange(true, std::memory_order_relaxed)) {
    std::signal(sig, SIG_DFL);
    std::raise(sig);
  } else {
    // First Ctrl+C: wake the JIT/AOT loop safepoints (atomic store is
    // async-signal-safe). The slow path consumes the one-shot flag.
    culebra_g_wake.store(true, std::memory_order_relaxed);
  }
}

// Request a cooperative interrupt (a "Ctrl+C") from an embedder or test, without
// a real signal: set the throw flag AND the JIT/AOT wake flag, so a running loop
// — interpreter or JIT — stops at its next safepoint. The signal handler sets the
// same pair (plus the double-press force-kill escalation). Set both: the interp
// poll reads the throw flag directly; the inlined JIT safepoint reads only the
// wake flag (it can't cheaply read the per-thread flag).
inline void request_interrupt() {
  culebra_g_sigint.store(true, std::memory_order_relaxed);
  culebra_g_wake.store(true, std::memory_order_relaxed);
}

// Install the SIGINT handler and point the current (main) thread's Runtime at
// the global flag, so blocking channel waits and the statement poll observe
// Ctrl+C. CLI-only: embedders opt in by calling this.
inline void install_sigint_handler() {
  current_runtime().interrupt_flag = &culebra_g_sigint;
  std::signal(SIGINT, _culebra_sigint_handler);
}

// Throw the cooperative Interrupted if an interrupt is pending — for blocking
// syscalls that poll between waits (interruptible stdin below). Checks the
// process SIGINT flag directly (like the JIT safepoint), so it works regardless
// of which Runtime is active — the JIT runs under a fresh RuntimeScope whose
// borrowed flag is null. A Ctrl+C is one-shot (consumed so a `catch` can
// resume); an isolate's cancel flag (this thread's borrowed flag, if any) stays
// sticky and keeps its own message.
inline void throw_if_interrupted() {
  auto* f = current_runtime().interrupt_flag;
  // Per-thread isolate cancel: sticky, its own message. Checked first so an
  // isolate thread reports its own cancel instead of consuming a Ctrl+C wake.
  if (f && f != &culebra_g_sigint && f->load(std::memory_order_relaxed)) {
    throw CulebraError("Interrupted", "isolate cancelled");
  }
  // Ctrl+C: one-shot. Only the main/CLI context — whose interrupt_flag IS the
  // sigint flag, or is null under a borrowed JIT RuntimeScope — consumes it. An
  // isolate thread that merely saw the wake leaves the flag for the main thread.
  if (culebra_g_sigint.load(std::memory_order_relaxed) &&
      (!f || f == &culebra_g_sigint)) {
    _consume_sigint();
    throw CulebraError("Interrupted", "interrupted");
  }
}

// --- Interruptible stdin --------------------------------------------------
//
// A plain `std::cin` read is not a cooperative safepoint: a program blocked
// waiting for stdin couldn't be stopped by a single Ctrl+C (only the second,
// force-killing press). These read the raw fd in a `poll` loop that wakes every
// ~200ms to honor the interrupt flag, throwing Interrupted if it fires — so a
// single Ctrl+C breaks a stdin wait too. `IO.read_all` / `IO.input` (both
// backends) route through here. A thread-local buffer holds bytes read past a
// line so the next read sees them. POSIX only; Windows keeps the blocking
// `std::cin`/stdio path (not a current build target).

#if !defined(_WIN32)
inline std::string& _stdin_leftover() {
  static thread_local std::string buf;
  return buf;
}

// Wait for more bytes on fd 0, appending one chunk to the leftover buffer.
// Returns false at EOF. Polls the interrupt flag between (and on) waits.
inline bool _stdin_fill() {
  char chunk[65536];
  for (;;) {
    throw_if_interrupted();
    struct pollfd pfd{/*fd=*/0, /*events=*/POLLIN, /*revents=*/0};
    int r = ::poll(&pfd, 1, 200);
    if (r < 0) {
      if (errno == EINTR) continue;  // signal arrived → re-check the flag
      return false;                  // real poll error → treat as EOF
    }
    if (r == 0) continue;            // timeout → re-check the flag
    ssize_t n = ::read(0, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;        // EOF
    _stdin_leftover().append(chunk, static_cast<size_t>(n));
    return true;
  }
}
#endif

// Read all of stdin to EOF, interruptibly.
inline std::string read_stdin_all_interruptible() {
#if defined(_WIN32)
  std::string out;
  char chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), stdin)) > 0)
    out.append(chunk, n);
  return out;
#else
  std::string out;
  out.swap(_stdin_leftover());       // take anything already buffered
  while (_stdin_fill()) {
    out += _stdin_leftover();
    _stdin_leftover().clear();
  }
  return out;
#endif
}

// Read one line (without the trailing newline) interruptibly. Returns false at
// EOF with nothing read; a final unterminated line is returned once.
inline bool read_stdin_line_interruptible(std::string& out) {
  out.clear();
#if defined(_WIN32)
  int c;
  bool any = false;
  while ((c = std::fgetc(stdin)) != EOF) {
    any = true;
    if (c == '\n') break;
    out.push_back(static_cast<char>(c));
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return any;
#else
  auto& buf = _stdin_leftover();
  for (;;) {
    auto nl = buf.find('\n');
    if (nl != std::string::npos) {
      out.assign(buf, 0, nl);
      buf.erase(0, nl + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (!_stdin_fill()) {            // EOF
      if (buf.empty()) return false;
      out.swap(buf);                 // trailing line with no newline
      buf.clear();
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
  }
#endif
}

// Serializes stdout/stderr writes so concurrent isolates don't interleave
// mid-line (`puts` once = one atomic line). Process-wide.
inline std::mutex& stdio_mutex() {
  static std::mutex m;
  return m;
}

// RAII to switch the active Runtime for the current thread.
struct RuntimeScope {
  Runtime* prev_;
  explicit RuntimeScope(Runtime& rt) : prev_(_culebra_current_runtime) {
    _culebra_current_runtime = &rt;
  }
  ~RuntimeScope() { _culebra_current_runtime = prev_; }
  RuntimeScope(const RuntimeScope&) = delete;
  RuntimeScope& operator=(const RuntimeScope&) = delete;
};

// Lazy-init a default-constructible T into a Runtime slot.
template <class T>
inline T& runtime_substate(RuntimeSlot slot) {
  auto& rt = current_runtime();
  if (!rt.substate[slot]) {
    rt.substate[slot] = new T();
    rt.substate_deleter[slot] = [](void* p) { delete static_cast<T*>(p); };
  }
  return *static_cast<T*>(rt.substate[slot]);
}

// --- Shared PRNG (interpreter and JIT) ---

inline std::mt19937_64& random_engine() {
  return current_runtime().random_engine;
}

// --- Well-known property contract ---

// Property names the runtime invokes behind the scenes:
//   drop  - RAII destructor (called on last release / cycle break)
//   iter  - iterator constructor (returns an object with `next`)
//   next  - iterator advance (returns `{done, value}`)
// Each must be a 0-arg Function. The name set and error wording live
// here so the two backends can't drift; per-backend type checks stay
// in each backend (Value vs JitClosure aren't interchangeable).
inline bool is_well_known_prop(std::string_view name) {
  return name == "drop" || name == "iter" || name == "next";
}

[[noreturn]] inline void throw_well_known_prop_contract_error(
    std::string_view name) {
  throw CulebraError(
      "DropContractError",
      std::format("type error: '{}' must be a Function taking no arguments.",
                  name));
}

// --- trait / protocol registry (§15) ---
//
// A trait declares a set of required methods. A class conforms to a
// trait when it has every required method (matching arity) — Go /
// Python `__str__` style structural conformance, no `impl` block.
// Default methods (`has_default = true`) are supplied by the trait
// itself and do NOT need to be on the class.

struct TraitMethod {
  std::string name;
  size_t arity;          // declared positional arity (excluding `this`)
  bool has_default;
};

struct TraitDef {
  std::string name;
  std::vector<TraitMethod> methods;
  // Supertrait names (`trait Ord: Eq` → {"Eq"}); their methods are
  // flattened into `methods` at register_trait time so conformance and
  // dispatch stay single-set look-ups. Empty on the JIT/AOT path, which
  // flattens at runtime via culebra_runtime_register_trait_super.
  std::vector<std::string> supertraits;
};

// Process-wide registry. Trait declarations register here; type_matches
// / multifn_specificity consult it on each lookup. Keyed by trait name.
// Guarded by trait_mutex(): host threads can declare traits and look
// them up concurrently (mt_smoke exercises this).
inline std::shared_mutex& trait_mutex() {
  static std::shared_mutex m;
  return m;
}

inline std::unordered_map<std::string, TraitDef>& trait_registry() {
  static std::unordered_map<std::string, TraitDef> reg;
  return reg;
}

// Cache: class name → trait name → conforms? Populated lazily by
// type_matches when it encounters a value of a known class against a
// known trait. Cleared on new trait registration (later traits may flip
// earlier "no" answers for already-cached classes). Shares trait_mutex().
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, bool>>&
trait_conformance_cache() {
  static std::unordered_map<std::string,
                            std::unordered_map<std::string, bool>>
      cache;
  return cache;
}

// Merge a supertrait's methods into `def` (dedup by name, keeping
// `def`'s own entries). The supertrait must already be registered —
// declaration order guarantees this for `trait Ord: Eq`. Unknown
// supers are silently skipped (best-effort). Caller holds trait_mutex().
inline void merge_supertrait_into(TraitDef& def, const std::string& super) {
  auto& reg = trait_registry();
  auto it = reg.find(super);
  if (it == reg.end()) return;
  for (const auto& m : it->second.methods) {
    bool present = false;
    for (const auto& e : def.methods)
      if (e.name == m.name) { present = true; break; }
    if (!present) def.methods.push_back(m);
  }
}

inline void register_trait(TraitDef def) {
  std::unique_lock lock(trait_mutex());
  // Flatten supertrait methods (trait inheritance) so conformance is a
  // single-set check. JIT/AOT carries empty supertraits and instead
  // merges via culebra_runtime_register_trait_super after registration.
  for (const auto& super : def.supertraits) merge_supertrait_into(def, super);
  trait_registry()[def.name] = std::move(def);
  trait_conformance_cache().clear();
}

inline const TraitDef* lookup_trait(std::string_view name) {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  auto it = reg.find(std::string(name));
  return it == reg.end() ? nullptr : &it->second;
}

// Snapshot every registered trait name under a read lock so the caller
// can iterate the names without holding trait_mutex. Used by interp /
// JIT multifn warm-up where the inner `type_matches` call re-acquires
// the mutex for cache writes — holding it across the loop would
// deadlock. Returns an empty vector with no allocation if the registry
// is empty (= the common case for trait-free programs).
inline std::vector<std::string> snapshot_trait_names() {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  if (reg.empty()) return {};
  std::vector<std::string> names;
  names.reserve(reg.size());
  for (const auto& [n, _] : reg) names.push_back(n);
  return names;
}

// Built-in conformance: hard-coded table of which primitive type
// labels conform to which built-in traits. Lets `fn show(x: Stringer)`
// accept Long / String / Array / ... without each builtin needing a
// class-instance wrapper. Only the three built-in traits are
// supported here — user-declared traits still operate solely on
// class instances. Returns false for unknown traits, leaving
// type_matches to fall back to the registered-trait path.
inline bool builtin_conforms_to_trait(std::string_view type_label,
                                       std::string_view trait_name) {
  if (trait_name == "Stringer") {
    // Every value culebra can produce has a `to_string` interpretation
    // via the runtime display path; expose that uniformly.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor" ||
           type_label == "Function";
  }
  if (trait_name == "Eq") {
    // Equality is defined on every primitive (value-based) and
    // reference types compare by identity — covers all builtins.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor";
  }
  if (trait_name == "Comparable") {
    // Ordering is well-defined on the value primitives. Container
    // types (Array / Tuple / Set / Tensor) do compare lexicographically
    // in the runtime, but we keep this conservative for the MVP.
    return type_label == "Bool" || type_label == "Long" ||
           type_label == "Float" || type_label == "String" ||
           type_label == "StringView";
  }
  if (trait_name == "StringLike") {
    // Byte-readable string flavors: owning `String` and the borrowed
    // `StringView`. User classes that want to be string-substitutable
    // also satisfy this via structural conformance on `to_string_view`.
    return type_label == "String" || type_label == "StringView";
  }
  if (trait_name == "Hashable") {
    // Mirrors what ValueHash / JitValueHash actually hash: every value
    // primitive plus Tuple (hash combines element hashes). Mutable
    // containers (Array / Set / Object / Function / Tensor) stay out
    // — they throw at hash time today and shouldn't pretend otherwise.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Tuple";
  }
  if (trait_name == "Iterable") {
    // Anything `for x in y` accepts. Primitive collections expose
    // `iter()` — String/StringView via string_builtins, Array via the
    // array iterator wrapper, Object via ObjectValue::builtins() (a
    // bare `{...}` literal still has the default key iterator), Set
    // and Tuple via their runtime-built iterator wrappers.
    return type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Object";
  }
  return false;
}

// Structural conformance check: `class_methods` maps method name to
// arity for the class under test. The class conforms when every
// non-default method on `trait` is matched by name and the class
// method accepts at least `trait_arity` positional args. Default
// methods don't need a class-side definition — the trait provides
// them and the dispatcher falls through.
//
// The arity check is one-sided (`class_arity >= trait_arity`) to
// accept class methods that carry extra parameters with defaults
// or a `**kwargs` rest. Strict equality would reject `class C {
// foo(a, **rest) }` against `trait T { foo(a) }`, since the
// class-side walk on the JIT path counts the `**rest` toward
// JitClosure::arity. The runtime call site still verifies actual
// argument counts, so an over-strict class signature surfaces as
// ArityError at call time rather than DispatchError.
inline bool class_conforms_to_trait(
    const std::unordered_map<std::string, size_t>& class_methods,
    const TraitDef& trait) {
  for (const auto& m : trait.methods) {
    if (m.has_default) continue;
    auto it = class_methods.find(m.name);
    if (it == class_methods.end() || it->second < m.arity) return false;
  }
  return true;
}

// Built-in trait declarations evaluated at the top of every program
// (interp + JIT). Provides the standard `Stringer`, `Eq`, and
// `Comparable` abstractions — small enough to inline as a const
// string, structurally usable by any class that ships the right
// methods.
inline std::string_view builtin_traits_preamble() {
  static constexpr std::string_view src = R"culebra(
trait Stringer {
  to_s() -> String
}
trait Eq {
  eq(other) -> Bool
  neq(other) -> Bool { !this.eq(other) }
}
trait Comparable {
  cmp(other) -> Long
  lt(other) -> Bool { this.cmp(other) < 0 }
  le(other) -> Bool { this.cmp(other) <= 0 }
  gt(other) -> Bool { this.cmp(other) > 0 }
  ge(other) -> Bool { this.cmp(other) >= 0 }
}
trait StringLike {
  to_string_view() -> StringView
}
trait Hashable {
  hash() -> Long
}
trait Iterator {
  has_next() -> Bool
  next() -> Any
  dispose() {}
}
trait Iterable {
  iter() -> Iterator
}
)culebra";
  return src;
}

}  // namespace culebra
