// Invalid-UTF-8 handling smoke test. culebra Strings are Go-style byte
// strings, so a String can hold bytes that aren't valid UTF-8. The
// character-level methods (iter / code_points / graphemes) must map an
// invalid byte to U+FFFD (the replacement character) and advance one
// byte — never silently drop it. This pins that behavior on the interp
// (the JIT shares the same _decode_one_utf8 / decode_codepoint policy).
//
// We can't write a lone 0xFF from culebra source (no \x escape), so the
// invalid String is supplied from C++ via a host function.

#include <cstring>
#include <iostream>
#include <string>

#include <culebra.h>
#include <stdlib_interp.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace utf8_invalid_smoke_ns {

namespace {

std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<utf8>", code, std::strlen(code), msgs);
  if (!ast) {
    for (auto& m : msgs) std::cerr << m;
    std::exit(1);
  }
  return ast;
}

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

}  // namespace

int run() {
  auto env = culebra::environment({});
  bool ok = true;

  // Supply "a\xffb" — 'a', a lone 0xFF (invalid UTF-8), 'b'.
  culebra::define(env, "bad_utf8",
                  []() -> std::string { return std::string("a\xff" "b"); }, {});

  auto run_long = [&](const char* code, long expected, const char* what) {
    auto ast = parse_or_die(code);
    culebra::Value v;
    std::vector<std::string> msgs;
    if (!culebra::interpret(ast, env, v, msgs, culebra::Debugger())) {
      for (auto& m : msgs) std::cerr << m;
      ok &= check(false, what);
      return;
    }
    ok &= check(v.to_long() == expected, what);
  };

  // The String is 3 bytes regardless of validity.
  run_long("bad_utf8().size()", 3, "size counts bytes");

  // code_points: 'a'(97), invalid 0xFF -> U+FFFD (65533), 'b'(98).
  run_long("bad_utf8().code_points().collect()[0]", 97, "cp[0] = a");
  run_long("bad_utf8().code_points().collect()[1]", 65533,
           "cp[1] = U+FFFD (invalid byte, not dropped)");
  run_long("bad_utf8().code_points().collect()[2]", 98, "cp[2] = b");
  run_long("bad_utf8().code_points().collect().size()", 3,
           "3 code points (invalid byte not skipped)");

  // graphemes: same — three clusters, the middle one the replacement.
  run_long("bad_utf8().graphemes().collect().size()", 3,
           "3 graphemes (invalid byte not skipped)");

  // The original bytes are never lost: slicing / size still see all 3.
  // (Round-trip via the byte string is preserved.)

  std::cout << (ok ? "utf8_invalid OK\n" : "utf8_invalid FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace utf8_invalid_smoke_ns
