#pragma once

// The static analysis of one source text: what `culebra lint` prints and what
// a language server publishes, from one function so the two cannot come to
// disagree about a file.

#include <cli/test_runner.h>  // set_test_ambients, default_test_cul_matcher
#include <frontend/effects_transform.h>
#include <frontend/lint.h>
#include <frontend/parser.h>

#include <memory>
#include <string>
#include <vector>

namespace culebra {

struct SourceLint {
  // False when the source did not parse: `diagnostics` is then empty and
  // `messages` says why.
  bool parsed = false;
  std::vector<lint::Diagnostic> diagnostics;
  // Where the parse of the source as written stopped, for a caller that
  // places it in the text. `messages` carries the same failures formatted the
  // way the CLI prints them, plus any the lowering's own re-parse reported.
  std::vector<ParseFailure> syntax_errors;
  std::vector<std::string> messages;
  // The source as written, parsed. It views `src`, so it lives no longer.
  std::shared_ptr<peg::Ast> authored;
};

inline SourceLint lint_source(const std::string& path, std::string& src) {
  SourceLint out;
  // Per file, because `culebra lint .` sees both kinds in one pass — and keyed
  // on the runner's own matcher, so the two cannot come to disagree about what
  // a test file is.
  set_test_ambients(default_test_cul_matcher(path));

  // `collect_module` wants both views of the program: the lowered AST the
  // backends run (sound error checks) and the source as written (advisory
  // warnings). See its comment for why neither alone is enough.
  out.authored = parse(path, src, out.syntax_errors);
  for (const auto& f : out.syntax_errors)
    out.messages.push_back(format_parse_failure(path, f));
  if (!out.authored) return out;

  // The lowered AST views fragments this ledger owns, and neither leaves this
  // function, so the ledger is declared first and dropped last.
  FragmentLedger fragments;
  std::shared_ptr<peg::Ast> lowered;
  // The lowering itself rejects malformed effects (two `return` clauses, a
  // duplicate handler clause, …) by throwing. Report those as ordinary error
  // diagnostics — a linter must never abort on the input it was asked to
  // inspect.
  try {
    lowered = parse_with_transforms(path, src, out.messages, fragments);
  } catch (const CulebraError& e) {
    out.parsed = true;
    out.diagnostics.push_back(
        {e.kind, e.what(), e.line, e.col, lint::Severity::Error});
    return out;
  }
  if (!lowered) return out;
  out.parsed = true;
  out.diagnostics = lint::collect_module(*lowered, *out.authored);
  return out;
}

}  // namespace culebra
