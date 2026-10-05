// Generates include/frontend/grammar_blob.gen.h from culebra::grammar_ (grammar_def.h).
//
// Run via `just gen-blob` after changing the grammar or bumping
// vendor/cpp-peglib — the blob layout is peglib-version-specific. Skipping it
// is safe but silent: get_parser() guards the blob with GRAMMAR_BLOB_HASH and
// falls back to load_grammar(), which costs ~10 ms on every startup. `--check`
// (via `just check-blob`) fails when the checked-in header is stale.
//
// The header also carries what the grammar says of a syntax error's expected
// items (frontend/syntax_error.h): which of them may follow any expression,
// which may start any operand, and which are whitespace. They are asked of
// the grammar here, by parsing a few sources that stop short, so the
// formatter keeps no list of its own to fall behind the grammar.
#include "frontend/grammar_blob_key.h"  // culebra::grammar_blob_key() (+ grammar_, CPPPEGLIB_VERSION)
#include "frontend/syntax_error.h"      // culebra::syntax_expected_key()
#include "peglib.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

void appendf(std::string& out, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  out += buf;
}

std::string read_file(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::string out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

using Keys = std::vector<std::string>;

bool has(const Keys& keys, const std::string& k) {
  return std::find(keys.begin(), keys.end(), k) != keys.end();
}

// What the parse of `src` expected where it failed. Empty when it did not
// fail, which no probe below is meant to do.
Keys expected_at(peg::parser& parser, const std::string& src) {
  Keys out;
  parser.set_error_reporter(
      [&](const peg::ErrorReport& r) { out = culebra::syntax_expected_keys(r); });
  bool ok = parser.parse(src);
  parser.set_error_reporter(nullptr);
  if (ok) out.clear();
  return out;
}

// A position, read both ways peglib reports one: at the end of the input,
// where every alternative is tried, and ahead of a byte nothing starts with,
// where an alternative that cannot start is skipped and listed by its first
// literal instead.
Keys expected_after(peg::parser& parser, const std::string& src) {
  Keys out = expected_at(parser, src);
  for (auto& k : expected_at(parser, src + "\x01"))
    if (!has(out, k)) out.push_back(std::move(k));
  return out;
}

// What every one of `srcs` expects.
Keys expected_by_all(peg::parser& parser,
                     std::initializer_list<const char*> srcs) {
  Keys out;
  bool first = true;
  for (const char* src : srcs) {
    Keys here = expected_after(parser, src);
    if (first) out = std::move(here);
    else std::erase_if(out, [&](const std::string& k) { return !has(here, k); });
    first = false;
  }
  return out;
}

// The literals of the rules a leading underscore keeps out of an error
// message (_SpaceChar, _LineComment, ...). peglib leaves such a rule out when
// it tried it; one it skipped it lists by literal, with no rule to go by.
Keys underscore_literals(const std::string& grammar) {
  Keys out;
  static const std::regex rule(R"(^\s*~?_[A-Z]\w*\s*<-(.*)$)");
  static const std::regex literal(R"('((?:[^'\\]|\\.)*)')");
  size_t at = 0;
  while (at < grammar.size()) {
    size_t eol = grammar.find('\n', at);
    if (eol == std::string::npos) eol = grammar.size();
    std::string line = grammar.substr(at, eol - at);
    at = eol + 1;
    std::smatch m;
    if (!std::regex_match(line, m, rule)) continue;
    std::string rhs = m[1];
    for (std::sregex_iterator it(rhs.begin(), rhs.end(), literal), end;
         it != end; ++it) {
      // The text peglib makes of a grammar literal, by the function it uses.
      std::string raw = (*it)[1];
      auto key = culebra::syntax_expected_key(
          peg::resolve_escape_sequence(raw.data(), raw.size()), false);
      if (!has(out, key)) out.push_back(std::move(key));
    }
  }
  return out;
}

void append_keys(std::string& text, const char* name, const Keys& keys) {
  appendf(text, "inline constexpr const char* %s[] = {\n", name);
  for (const auto& k : keys) {
    text += "    \"";
    for (unsigned char ch : k) {
      if (ch == '"' || ch == '\\') appendf(text, "\\%c", ch);
      else if (ch >= 0x20 && ch < 0x7f) text += static_cast<char>(ch);
      else appendf(text, "\\%03o", ch);
    }
    text += "\",\n";
  }
  text += "};\n\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool check = false;
  const char* out = "include/frontend/grammar_blob.gen.h";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--check") == 0) {
      check = true;
    } else {
      out = argv[i];
    }
  }

  peg::parser parser;
  parser.set_logger([](size_t, size_t, const std::string&) {});
  if (!parser.load_grammar(culebra::grammar_)) {
    std::fprintf(stderr, "gen_grammar_blob: load_grammar failed\n");
    return 1;
  }
  parser.enable_ast();
  parser.enable_packrat_parsing();

  std::vector<uint8_t> blob;
  try {
    blob = parser.serialize_grammar();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "gen_grammar_blob: serialize failed: %s\n", e.what());
    return 1;
  }

  const uint64_t hash = culebra::grammar_blob_key();

  // A complete expression in four places that close differently; and where
  // an expression is due, as an operator's operand or as a whole.
  Keys silent = underscore_literals(culebra::grammar_);
  Keys after_expr =
      expected_by_all(parser, {"x = (f()", "x = [f()", "if f()", "g(f()"});
  Keys operand = expected_after(parser, "x = 1 + ");
  for (auto& k : expected_after(parser, "x = "))
    if (!has(operand, k)) operand.push_back(std::move(k));
  std::erase_if(after_expr, [&](const auto& k) { return has(silent, k); });
  std::erase_if(operand, [&](const auto& k) { return has(silent, k); });
  if (silent.empty() || after_expr.empty() || operand.empty()) {
    std::fprintf(stderr,
                 "gen_grammar_blob: a syntax-error probe expected nothing "
                 "(silent %zu, after an expression %zu, operand %zu)\n",
                 silent.size(), after_expr.size(), operand.size());
    return 1;
  }

  std::string text;
  text += "#pragma once\n";
  text += "// GENERATED by tools/gen_grammar_blob.cc (`just gen-blob`). Do not edit.\n";
  text += "// Serialized cpp-peglib grammar for culebra::grammar_ — lets get_parser()\n";
  text += "// skip the meta-parse. GRAMMAR_BLOB_HASH keys the blob to the grammar text\n";
  appendf(text, "// AND the cpp-peglib version (%s here); get_parser() falls back to\n", CPPPEGLIB_VERSION);
  text += "// load_grammar() whenever either changes (stale blob).\n";
  text += "#include <cstddef>\n#include <cstdint>\n\nnamespace culebra {\n\n";
  appendf(text, "inline constexpr uint64_t GRAMMAR_BLOB_HASH = 0x%016llxULL;\n\n",
          static_cast<unsigned long long>(hash));
  text += "inline constexpr unsigned char GRAMMAR_BLOB[] = {\n";
  for (size_t i = 0; i < blob.size(); ++i) {
    appendf(text, "0x%02x,", blob[i]);
    if ((i % 20) == 19) text += "\n";
  }
  text += "\n};\n\ninline constexpr size_t GRAMMAR_BLOB_SIZE = sizeof(GRAMMAR_BLOB);\n\n";
  text += "// What a syntax error's expected items are, by the grammar's own account\n";
  text += "// (frontend/syntax_error.h): whitespace and a comment's opener, what may\n";
  text += "// follow any expression, what may start one.\n";
  append_keys(text, "GRAMMAR_EXPECT_SILENT", silent);
  append_keys(text, "GRAMMAR_EXPECT_AFTER_EXPR", after_expr);
  append_keys(text, "GRAMMAR_EXPECT_OPERAND", operand);
  text += "}  // namespace culebra\n";

  if (check) {
    if (read_file(out) == text) return 0;
    std::fprintf(stderr,
                 "gen_grammar_blob: %s is stale (grammar or cpp-peglib changed).\n"
                 "  Run `just gen-blob` and commit the result — a stale blob silently\n"
                 "  costs ~10 ms of startup by falling back to load_grammar().\n",
                 out);
    return 1;
  }

  FILE* f = std::fopen(out, "w");
  if (!f) {
    std::fprintf(stderr, "gen_grammar_blob: cannot open %s\n", out);
    return 1;
  }
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);

  std::fprintf(stderr, "gen_grammar_blob: wrote %s (%zu bytes, hash %016llx)\n", out,
               blob.size(), static_cast<unsigned long long>(hash));
  return 0;
}
