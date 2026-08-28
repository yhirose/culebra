// Unit test for the parse funnel's newline normalization (include/parser.h).
//
// Source newlines are LF. A CRLF file is normalized in place before the
// grammar sees it, because a `\r` left in the buffer lands inside string
// literals and changes their value — the reason four tests failed on Windows
// and nowhere else. A bare `\r` is rejected instead of guessed at.
//
// The end-to-end half of this lives on Windows CI, whose checkout has CRLF
// endings: tools/check_suite_on_windows.sh runs the whole suite there, so the
// four files are covered by their own assertions on a real CRLF file. What is
// left for here is the byte-level behaviour (positions, idempotence, the
// rejections) and the proof that both parse entries route through it.
//
// Built and run by CTest (see CMakeLists.txt).

#include <parser.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what.c_str());
  failures++;
}

void eq(const std::string& got, const std::string& want,
        const std::string& what) {
  if (got == want) return;
  std::printf("FAIL: %s\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(),
              want.c_str());
  failures++;
}

std::string normalized(std::string src) {
  culebra::normalize_source_newlines(src);
  return src;
}

// The rejection, as (line, col, message) — or a marker when none came.
struct Rejection {
  int64_t line = -1, col = -1;
  std::string msg;
};

Rejection rejection_for(std::string src) {
  try {
    culebra::normalize_source_newlines(src);
  } catch (const culebra::CulebraError& e) {
    return {e.line, e.col, e.what()};
  }
  return {};
}

void test_crlf_becomes_lf() {
  eq(normalized("a\r\nb"), "a\nb", "one CRLF");
  eq(normalized("a\r\nb\r\n"), "a\nb\n", "trailing CRLF");
  eq(normalized("a\r\n\r\nb"), "a\n\nb", "blank CRLF line");
  eq(normalized("\r\n"), "\n", "a file that is one CRLF");
}

void test_lf_and_cr_free_sources_are_untouched() {
  eq(normalized("a\nb\n"), "a\nb\n", "LF source");
  eq(normalized(""), "", "empty source");
  eq(normalized("no newline at all"), "no newline at all", "no newline");
}

void test_normalizing_is_idempotent() {
  // `culebra lint` parses one buffer twice (authored AST, then lowered), so
  // the second pass has to see the same bytes as the first.
  std::string src = "a\r\nb\r\n";
  culebra::normalize_source_newlines(src);
  std::string once = src;
  culebra::normalize_source_newlines(src);
  eq(src, once, "second normalization changes nothing");
}

void test_bare_cr_is_rejected_with_a_position() {
  auto r = rejection_for("let x = 1\rlet y = 2\n");
  check(r.line == 1 && r.col == 10,
        std::format("bare CR position: got {}:{}, want 1:10", r.line, r.col));
  check(r.msg.find("bare carriage return") != std::string::npos,
        "bare CR message names what it found");

  // A later line: the count has to survive the LF-only prefix.
  auto r2 = rejection_for("x = 1\ny = 2\rz = 3\n");
  check(r2.line == 2 && r2.col == 6,
        std::format("bare CR on line 2: got {}:{}, want 2:6", r2.line, r2.col));

  // A CR at end of file has no LF after it, and is a bare CR like any other.
  auto r3 = rejection_for("x = 1\r");
  check(r3.line == 1 && r3.col == 6,
        std::format("trailing bare CR: got {}:{}, want 1:6", r3.line, r3.col));

  // A CRLF earlier in the file must not excuse a bare CR later.
  auto r4 = rejection_for("a\r\nb\rc\n");
  check(r4.line == 2 && r4.col == 2,
        std::format("bare CR after a CRLF: got {}:{}, want 2:2", r4.line,
                    r4.col));
}

// The point of doing this in the funnel: a CRLF file has to parse to the same
// tree as the LF file, literals included, on every entry point.
void test_both_parse_entries_route_through_it() {
  const std::string lf =
      "let s = \"\"\"line one\nline two\"\"\"\nIO.inspect(s.len())\n";
  std::string crlf;
  for (char c : lf) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }

  for (bool for_format : {false, true}) {
    const char* which = for_format ? "parse_for_format" : "parse";
    std::string a = lf, b = crlf;
    std::vector<std::string> ma, mb;
    auto lf_ast = for_format ? culebra::parse_for_format("(lf)", a, ma)
                             : culebra::parse("(lf)", a, ma);
    auto crlf_ast = for_format ? culebra::parse_for_format("(crlf)", b, mb)
                               : culebra::parse("(crlf)", b, mb);
    check(lf_ast != nullptr, std::format("{}: LF source parses", which));
    check(crlf_ast != nullptr, std::format("{}: CRLF source parses", which));
    if (lf_ast && crlf_ast) {
      eq(peg::ast_to_s(crlf_ast), peg::ast_to_s(lf_ast),
         std::format("{}: CRLF parses to the same tree as LF", which));
    }

    // The rejection reaches the caller as msgs + nullptr, the one failure
    // shape both entries promise, rather than escaping as an exception.
    std::string bare = "let x = 1\rlet y = 2\n";
    std::vector<std::string> mc;
    auto bad = for_format ? culebra::parse_for_format("(bare)", bare, mc)
                          : culebra::parse("(bare)", bare, mc);
    check(bad == nullptr, std::format("{}: bare CR fails the parse", which));
    check(mc.size() == 1 &&
              mc[0].find("bare carriage return") != std::string::npos,
          std::format("{}: bare CR is reported through msgs", which));
  }
}

}  // namespace

int main() {
  test_crlf_becomes_lf();
  test_lf_and_cr_free_sources_are_untouched();
  test_normalizing_is_idempotent();
  test_bare_cr_is_rejected_with_a_position();
  test_both_parse_entries_route_through_it();

  if (failures) {
    std::printf("source_newlines_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("source_newlines_test OK\n");
  return 0;
}
