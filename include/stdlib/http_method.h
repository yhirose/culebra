#pragma once

// An HTTP request method a request line can carry: an RFC 9110 token. A
// method goes onto the wire verbatim, so CR/LF in one ends the line early and
// smuggles a second request onto the connection; cpp-httplib now refuses a
// non-token, but only as a failed write, which a program reads as a network
// fault ("Failed to write connection", or "Could not establish connection"
// when the host is also unreachable). The constructor is private, so a
// method the program supplied reaches a request only through `checked`,
// which names the mistake before anything is sent — and an entry that
// forgets to check fails to compile, as with Port.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <base/shared.h>  // CulebraError / culebra::format / json_escape

namespace culebra {

// RFC 9110 §5.6.2: tchar is ALPHA, DIGIT, or one of !#$%&'*+-.^_`|~. `token`
// is this method's grammar, and also a header field-name's (§5.1) — shared
// here rather than duplicated in the header-name check http.h adds.
inline bool is_http_token(std::string_view s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9');
    if (!alnum && std::string_view("!#$%&'*+-.^_`|~").find(c) ==
                      std::string_view::npos)
      return false;
  }
  return true;
}

class HttpMethod {
 public:
  // `ctx` names the entry (`Http.request`), as its other errors do. Case is
  // kept: a method is case-sensitive, and `get` is a token of its own.
  static HttpMethod checked(std::string_view m, std::string_view ctx,
                            int64_t line, int64_t col) {
    if (!is_http_token(m))
      throw CulebraError(
          "ValueError",
          culebra::format("{}: method must be an HTTP token, got {}", ctx,
                          json_escape(m)),
          line, col);
    return HttpMethod(std::string(m));
  }
  static HttpMethod get() { return HttpMethod("GET"); }
  static HttpMethod post() { return HttpMethod("POST"); }
  static HttpMethod put() { return HttpMethod("PUT"); }
  static HttpMethod del() { return HttpMethod("DELETE"); }
  static HttpMethod head() { return HttpMethod("HEAD"); }

  const std::string& str() const { return m_; }

 private:
  explicit HttpMethod(std::string m) : m_(std::move(m)) {}

  std::string m_;
};

}  // namespace culebra
