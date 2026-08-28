#pragma once

// The formatter for messages that reach an AOT binary. `rt_` names the
// boundary the way rt.h and rt_macros.h do — this is not formatter.h, which
// re-prints source for `culebra fmt`.
//
// `std::format` links all-or-nothing: one argument visitor names the integer,
// float and string formatters together, so a single reachable call pulls the
// whole machinery (with the unicode width tables the string formatter reads)
// into every program `culebra build` produces — 15% of a `print("hello")`,
// measured in docs/deployment.md §4. The runtime's uses are messages, `{}`
// with a few width and hex specs, so they are served here and libstdc++'s
// formatter stays out. tools/check_aot_feature_axes.sh holds the line by
// reading built binaries, because nothing in the source shows it.
//
// A header the runtime archive compiles uses this. The driver, the parser and
// the transforms keep `std::format` — none of them is linked into a program —
// and so does the interpolation spec (`"{x:.2f}"`, shared.h's
// format_value_as), whose mini-language IS std::format's: a program that
// writes one asks for the formatter and pays for it.
//
// The format string is checked at compile time, as std::format's is: the field
// count has to match the arguments, and a spec outside the subset below is
// rejected rather than silently rendered differently. Every argument type is
// named by an overload — implicit conversions do not slip through, so nothing
// can pick up a rendering `std::format` would not have given it.
//
//   {}        the argument, as `std::format`'s `{}` renders it
//   {:N}      in a field of N, text left, numbers right (std::format's rule)
//   {:<N}     left-aligned      {:>N}  right-aligned
//   {:0N}     zero-filled
//   {:0Nx}    …in lowercase hex ({:X} for uppercase)

#include <charconv>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>

namespace culebra {
namespace format_detail {

struct Spec {
  int width = 0;
  char fill = ' ';
  char align = 0;  // '<', '>', or 0 for the argument type's own default
  char type = 0;   // 'x' or 'X'; a written 'd' means the same as none
};

// Reads one field's spec, leaving `i` just past its '}'. False for a spec
// this subset does not serve — the consteval check reports it, so the runtime
// walk never sees one.
constexpr bool parse_spec(std::string_view f, size_t& i, Spec& out) {
  Spec s;
  if (i < f.size() && f[i] == ':') {
    ++i;
    if (i < f.size() && (f[i] == '<' || f[i] == '>')) s.align = f[i++];
    if (i < f.size() && f[i] == '0') {
      s.fill = '0';
      ++i;
    }
    while (i < f.size() && f[i] >= '0' && f[i] <= '9')
      s.width = s.width * 10 + (f[i++] - '0');
    if (i < f.size() && f[i] != '}') {
      char t = f[i++];
      if (t != 'd' && t != 'x' && t != 'X') return false;
      if (t != 'd') s.type = t;
    }
  }
  if (i >= f.size() || f[i] != '}') return false;
  ++i;
  out = s;
  return true;
}

// Fields in `f`, or -1 if it is malformed. `{{` and `}}` are literal braces.
constexpr int field_count(std::string_view f) {
  int n = 0;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] == '}') {
      if (i + 1 >= f.size() || f[i + 1] != '}') return -1;
      ++i;
      continue;
    }
    if (f[i] != '{') continue;
    if (i + 1 < f.size() && f[i + 1] == '{') {
      ++i;
      continue;
    }
    ++i;
    Spec s;
    if (!parse_spec(f, i, s)) return -1;
    --i;  // the loop's ++i steps past the '}'
    ++n;
  }
  return n;
}

// A format string checked against its arguments. Same shape as
// std::basic_format_string: the check runs in the constructor, in constant
// evaluation, so a mismatch is a compile error at the call site.
template <typename... Ts>
struct String {
  std::string_view s;
  template <typename T>
    requires std::convertible_to<const T&, std::string_view>
  consteval String(const T& v) : s(v) {
    int n = field_count(s);
    if (n < 0)
      throw "culebra::format serves {}, {:N}, {:<N}, {:0N}, {:0Nx} and "
            "{:0NX}. Render the value yourself, or widen include/"
            "rt_format.h — reaching for std::format in a header the runtime "
            "archive compiles adds its whole formatter to every program";
    if (n != static_cast<int>(sizeof...(Ts)))
      throw "culebra::format: the format string's field count does not match "
            "the arguments";
  }
};

// `default_left` is what the argument's type aligns to when the spec says
// nothing — std::format left-aligns text and right-aligns numbers.
inline void pad(std::string& out, std::string_view body, const Spec& s,
                bool default_left) {
  int fill = s.width - static_cast<int>(body.size());
  if (fill <= 0) {
    out += body;
    return;
  }
  if (s.align ? s.align == '<' : default_left) {
    out += body;
    out.append(static_cast<size_t>(fill), s.fill);
  } else {
    out.append(static_cast<size_t>(fill), s.fill);
    out += body;
  }
}

// The rendering `{}` has in std::format, one overload per argument type the
// runtime's messages use. `char` and `bool` need their own (they satisfy
// std::integral and would come out as numbers), `const char*` carries the
// null guard, and `std::string` needs one because the deleted template below
// would otherwise claim it.
inline void one(std::string& out, const Spec& s, std::string_view v) {
  pad(out, v, s, true);
}
inline void one(std::string& out, const Spec& s, const std::string& v) {
  pad(out, v, s, true);
}
inline void one(std::string& out, const Spec& s, const char* v) {
  pad(out, v ? std::string_view(v) : std::string_view(), s, true);
}
inline void one(std::string& out, const Spec& s, char v) {
  pad(out, std::string_view(&v, 1), s, true);
}
inline void one(std::string& out, const Spec& s, bool v) {
  pad(out, v ? "true" : "false", s, true);
}
template <std::integral T>
void one(std::string& out, const Spec& s, T v) {
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof(buf), v, s.type ? 16 : 10);
  if (s.type == 'X')
    for (char* p = buf; p < r.ptr; ++p)
      if (*p >= 'a' && *p <= 'f') *p = static_cast<char>(*p - ('a' - 'A'));
  pad(out, std::string_view(buf, static_cast<size_t>(r.ptr - buf)), s, false);
}
template <std::floating_point T>
void one(std::string& out, const Spec& s, T v) {
  // std::format's `{}` for a float is the shortest round-trip form, which is
  // to_chars' default too.
  char buf[40];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  pad(out, std::string_view(buf, static_cast<size_t>(r.ptr - buf)), s, false);
}
// Anything else, including a type that would arrive through a conversion:
// std::format has no formatter for it either, and a silent conversion is how
// a rendering drifts from the one the message used to have.
template <typename T>
void one(std::string&, const Spec&, T) = delete;

// Copies literal text (folding `{{` and `}}`) and stops just past the '{'
// that opens the next field, or at the end when none is left.
inline size_t literal(std::string& out, std::string_view f, size_t i) {
  for (; i < f.size(); ++i) {
    if (f[i] == '{' && (i + 1 >= f.size() || f[i + 1] != '{')) return i + 1;
    if ((f[i] == '{' || f[i] == '}') && i + 1 < f.size() && f[i + 1] == f[i])
      ++i;
    out += f[i];
  }
  return i;
}

// The text before one field, then the field. Shared by every call site with
// the same argument type, which a per-pack recursion would not be.
template <typename T>
void field(std::string& out, std::string_view f, size_t& i, const T& v) {
  i = literal(out, f, i);
  Spec s;
  parse_spec(f, i, s);  // checked at compile time; cannot fail here
  one(out, s, v);
}

}  // namespace format_detail

// std::format for the subset the runtime's messages need. See the file header.
template <typename... Ts>
std::string format(format_detail::String<std::type_identity_t<Ts>...> f,
                   const Ts&... args) {
  std::string out;
  out.reserve(f.s.size() + 16);
  size_t i = 0;
  (format_detail::field(out, f.s, i, args), ...);
  format_detail::literal(out, f.s, i);
  return out;
}

}  // namespace culebra
