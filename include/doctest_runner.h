#pragma once

// Doctest runner: extract ` ```culebra ` blocks from Markdown docs,
// run each on the engine the caller supplies (a BlockRunner — the CLI has
// one per backend), and check output against the convention markers
// (`# =>`, `# => |`, `# !!`, `# doctest:`).
// See docs/handbook.md §16.1 for the convention. The backend-filter
// directives (jit-only/aot-only) and `compile-only` are not yet honored —
// see the per-marker notes below.

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "effects_transform.h"
#include <script_teardown.h>  // ScriptTeardownGuard (B7-b)
#include "test_runner.h"  // StdoutCapture, Reporter, TestRunSummary, json_escape

namespace culebra {

// A single extracted documentation example.
struct DocBlock {
  int start_line = 0;                 // line of the ```culebra fence (1-based)
  std::string code;                   // block body, verbatim (markers are
                                      // comments, harmless to the interpreter)
  std::vector<std::string> expected;  // expected stdout lines, in source order
  bool expect_throw = false;          // a `# !!` marker was present
  std::string throw_pattern;          // substring the thrown message must contain
  enum class Directive { None, Skip } directive = Directive::None;
  bool has_expectations = false;      // any `# =>` / `# !!` present
};

namespace doctest_detail {

// Split into lines, dropping a single trailing '\r' per line (LF docs,
// tolerate CRLF). The final line is included even without a trailing LF.
inline std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(std::move(cur));
  return out;
}

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// Result of splitting a code line at its line-comment `#`.
struct LineParts {
  bool has_code = false;     // non-whitespace before the comment
  bool has_comment = false;  // an unquoted `#` was found
  std::string comment;       // text after the `#` (leading space kept)
};

// Find the line comment, skipping `#` inside string literals. Tracks
// single/double quotes with backslash escapes — enough for the doc
// examples (no string interpolation marker collides with `#`). Block
// comments (`/* */`) are not modeled; no doc marker hides inside one.
inline LineParts split_comment(const std::string& line) {
  LineParts r;
  bool in_s = false, in_d = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (in_s) {
      if (c == '\\') { i++; continue; }
      if (c == '\'') in_s = false;
    } else if (in_d) {
      if (c == '\\') { i++; continue; }
      if (c == '"') in_d = false;
    } else if (c == '#') {
      r.has_comment = true;
      r.comment = line.substr(i + 1);
      break;
    } else {
      if (c == '\'') in_s = true;
      else if (c == '"') in_d = true;
      if (c != ' ' && c != '\t') r.has_code = true;
    }
  }
  return r;
}

// Strip exactly one leading space (the `# ` convention) to preserve
// intentional indentation in multi-line expected output.
inline std::string strip_one_space(const std::string& s) {
  if (!s.empty() && s.front() == ' ') return s.substr(1);
  return s;
}

}  // namespace doctest_detail

// Extract every ` ```culebra ` block from Markdown `md`, parsing the
// convention markers within each. Fences are matched after trimming
// leading whitespace; doc fences are top-level in practice.
inline std::vector<DocBlock> extract_doc_blocks(const std::string& md) {
  using namespace doctest_detail;
  std::vector<DocBlock> blocks;
  auto lines = split_lines(md);

  for (size_t i = 0; i < lines.size(); i++) {
    if (trim(lines[i]) != "```culebra") continue;
    DocBlock blk;
    blk.start_line = static_cast<int>(i) + 1;
    size_t body = i + 1;
    std::vector<std::string> code_lines;
    while (body < lines.size() && trim(lines[body]) != "```") {
      code_lines.push_back(lines[body]);
      body++;
    }
    i = body;  // resume after the closing fence

    // Join the body verbatim — markers are `#` comments, so the
    // interpreter ignores them. A trailing newline is required: the
    // grammar's LineComment rule ends with `&EndOfLine`, so a final
    // comment line with no terminator would otherwise fail to parse.
    for (const auto& cl : code_lines) {
      blk.code += cl;
      blk.code += '\n';
    }

    bool seen_first = false;
    bool multiline = false;
    for (const auto& raw : code_lines) {
      auto parts = split_comment(raw);

      // A run of pure-comment lines after `# => |` is the multi-line
      // expected block; it ends at the first code/blank/marker line.
      if (multiline) {
        if (parts.has_comment && !parts.has_code) {
          auto t = trim(parts.comment);
          if (!t.starts_with("=>") && !t.starts_with("!!") &&
              !t.starts_with("doctest:")) {
            blk.expected.push_back(strip_one_space(parts.comment));
            continue;
          }
        }
        multiline = false;  // fall through and reprocess this line
      }

      if (!parts.has_comment) {
        if (!trim(raw).empty()) seen_first = true;
        continue;
      }
      auto t = trim(parts.comment);

      // `# doctest: <directive>` — only as the block-leading line.
      if (!seen_first && t.starts_with("doctest:")) {
        auto d = trim(t.substr(8));
        if (d == "skip") blk.directive = DocBlock::Directive::Skip;
        // Other directives (compile-only, *-only backend filters) are
        // not yet honored; the block runs normally. No doc uses them.
        seen_first = true;
        continue;
      }
      seen_first = true;

      if (t.starts_with("=>")) {
        auto val = trim(t.substr(2));
        blk.has_expectations = true;
        if (val == "|") {
          multiline = true;
        } else {
          blk.expected.push_back(val);
        }
      } else if (t.starts_with("!!")) {
        blk.expect_throw = true;
        blk.throw_pattern = trim(t.substr(2));
        blk.has_expectations = true;
      }
    }
    blocks.push_back(std::move(blk));
  }
  return blocks;
}


// What running one block produced. `message` carries the diagnostic in the
// text the CLI prints for that failure — which is what a `# !!` pattern is
// matched against, so every engine has to spell it the same way.
struct DocRunOutcome {
  bool ok = false;
  std::string kind;  // "SyntaxError" when it never ran, else "RuntimeError"
  std::string message;
};

// How one block is parsed and run. The runner owns everything around it —
// finding the blocks, capturing stdout, comparing the markers, reporting —
// so an engine is exactly this function: the interpreter's is `interpret`
// against a fresh env, and each compiled lane's is its own compile + run.
// A block is independent of every other, which for the compiled lanes means
// a Runtime of its own (their namespace caches, class and overload
// registries live there, as the interpreter's live in its env).
using BlockRunner =
    std::function<DocRunOutcome(const std::string& name,
                                const std::string& code)>;

// Run the doctest blocks in each file. Mirrors run_tests' reporter
// output (Default human lines / Json NDJSON) and summary/exit
// semantics. Each block runs in a fresh env (blocks are independent).
inline TestRunSummary run_doctests(
    const std::vector<std::filesystem::path>& files,
    const std::string& filter, const BlockRunner& run_block,
    Reporter reporter = Reporter::Default, int bail_after = 0,
    bool list_only = false) {
  using namespace doctest_detail;
  TestRunSummary summary;

  auto emit_fail = [&](const std::string& name, const std::string& source,
                       const std::string& kind, const std::string& message,
                       const std::string& captured) {
    summary.failed++;
    if (reporter == Reporter::Json) {
      std::cout << R"({"event":"doc_fail","name":)" << json_escape(name)
                << R"(,"source":)" << json_escape(source)
                << R"(,"kind":)" << json_escape(kind)
                << R"(,"message":)" << json_escape(message)
                << R"(,"stdout":)" << json_escape(captured) << "}\n";
    } else {
      std::string msg = "  FAIL " + name + " — " + kind + ": " + message +
                        " (in " + source + ")";
      summary.failure_messages.push_back(msg);
      std::cout << msg << "\n";
    }
  };

  auto emit_pass = [&](const std::string& name) {
    summary.passed++;
    if (reporter == Reporter::Json) {
      std::cout << R"({"event":"doc_pass","name":)" << json_escape(name)
                << "}\n";
    } else {
      std::cout << "  ok  " << name << "\n";
    }
  };

  for (const auto& path : files) {
    auto source = path.string();
    std::ifstream ifs(source, std::ios::binary);
    if (!ifs) {
      summary.errored_files++;
      if (reporter == Reporter::Json) {
        std::cout << R"({"event":"file_error","source":)" << json_escape(source)
                  << R"(,"kind":"IOError","message":"cannot open file")"
                  << "}\n";
      } else {
        std::cerr << "culebra test: cannot open " << source << "\n";
      }
      continue;
    }
    std::string md((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());

    auto blocks = extract_doc_blocks(md);
    for (const auto& blk : blocks) {
      std::string name = source + "#" + std::to_string(blk.start_line);
      if (!filter.empty() && name.find(filter) == std::string::npos) continue;

      if (list_only) {
        if (reporter == Reporter::Json) {
          std::cout << R"({"event":"doc_list","name":)" << json_escape(name)
                    << "}\n";
        } else {
          std::cout << name << "\n";
        }
        continue;
      }

      if (blk.directive == DocBlock::Directive::Skip) {
        if (reporter == Reporter::Json) {
          std::cout << R"({"event":"doc_skip","name":)" << json_escape(name)
                    << "}\n";
        }
        continue;  // skipped blocks are not counted as passed or failed
      }

      std::string captured;
      DocRunOutcome outcome;
      {
        StdoutCapture cap(true);
        outcome = run_block(name, blk.code);
        captured = cap.take();
      }
      // Reap whatever this block left outstanding (e.g. a doc example that
      // throws past an unreached join()) — ScriptTeardownGuard
      // (script_teardown.h), scoped to the rest of this loop iteration, so it
      // fires once per doc block instead of once per script run
      // (interpret_modules) or once per REPL session (repl.h): each doc
      // block is its own fresh env / run.
      ScriptTeardownGuard script_teardown_guard;
      if (!outcome.ok && outcome.kind == "SyntaxError") {
        emit_fail(name, source, outcome.kind, outcome.message, captured);
        if (bail_after > 0 && summary.failed >= bail_after) break;
        continue;
      }
      bool ok = outcome.ok;
      const std::string& err = outcome.message;

      if (blk.expect_throw) {
        if (ok) {
          emit_fail(name, source, "ExpectError",
                    "expected throw matching \"" + blk.throw_pattern +
                        "\" but block completed normally",
                    captured);
        } else if (err.find(blk.throw_pattern) == std::string::npos) {
          emit_fail(name, source, "ErrorMismatch",
                    "expected throw matching \"" + blk.throw_pattern +
                        "\" but got: " + err,
                    captured);
        } else {
          emit_pass(name);
        }
        if (bail_after > 0 && summary.failed >= bail_after) break;
        continue;
      }

      if (!ok) {
        emit_fail(name, source, "RuntimeError", err, captured);
        if (bail_after > 0 && summary.failed >= bail_after) break;
        continue;
      }

      if (blk.has_expectations) {
        // Compare captured stdout, line by line, against the markers.
        auto got = split_lines(captured);
        if (!got.empty() && got.back().empty()) got.pop_back();  // trailing LF
        bool match = got.size() == blk.expected.size();
        for (size_t k = 0; match && k < got.size(); k++) {
          if (got[k] != blk.expected[k]) match = false;
        }
        if (!match) {
          std::string want, have;
          for (const auto& l : blk.expected) want += l + "\\n";
          for (const auto& l : got) have += l + "\\n";
          emit_fail(name, source, "OutputMismatch",
                    "expected [" + want + "] but got [" + have + "]", captured);
          if (bail_after > 0 && summary.failed >= bail_after) break;
          continue;
        }
      }

      emit_pass(name);
    }
    if (bail_after > 0 && summary.failed >= bail_after) break;
  }

  bool bailed = bail_after > 0 && summary.failed >= bail_after;
  if (reporter == Reporter::Json) {
    std::cout << R"({"event":"run_end","passed":)" << summary.passed
              << R"(,"failed":)" << summary.failed
              << R"(,"errored_files":)" << summary.errored_files
              << R"(,"bailed":)" << (bailed ? "true" : "false") << "}\n";
  }
  return summary;
}

}  // namespace culebra
