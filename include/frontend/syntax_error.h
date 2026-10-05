#pragma once
// A syntax error's message, built from peglib's structured report instead of
// taken from its formatted string.
//
// peglib lists what every failure at the furthest position expected. For a
// grammar the size of culebra's that is often dozens of items, most of them
// true of nearly any position: after a complete expression any operator may
// follow, and where an operand is due any expression may start. The message
// names those two crowds ("an operator", "an expression"), the way rustc's
// does, and lists the rest: the closing bracket, the keyword, the separator
// that was missing. The crowds are not written down here: the blob generator
// asks the grammar for them (tools/gen_grammar_blob.cc).
//
// An item is known by its text alone, which is all peglib's report says of
// a literal. So a closer spelled like an operator goes with the operators
// when they are named: `|` ending a lambda's parameters, `:` in `a ? b : c`.
#include "peglib.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// One expected item as the classes key it: a literal quoted, a rule in angle
// brackets (peglib's own display). The generator and the formatter share it.
inline std::string syntax_expected_key(std::string_view text, bool is_rule) {
  std::string key;
  key += is_rule ? '<' : '\'';
  key += text;
  key += is_rule ? '>' : '\'';
  return key;
}

// Every item a report expected, as its key, in the report's order.
inline std::vector<std::string> syntax_expected_keys(
    const peg::ErrorReport& r) {
  std::vector<std::string> keys;
  for (const auto& l : r.expected_literals)
    keys.push_back(syntax_expected_key(l, false));
  for (const auto& n : r.expected_rules)
    keys.push_back(syntax_expected_key(n, true));
  return keys;
}

// What the grammar says of its own expected items (grammar_blob.gen.h).
struct SyntaxExpectClasses {
  std::span<const char* const> silent;      // whitespace, a comment's opener
  std::span<const char* const> after_expr;  // may follow any expression
  std::span<const char* const> operand;     // may start an expression
};

// `cls` null: a grammar the program supplied (PEG.parse), which gets its
// list whole, escaped.
inline std::string format_syntax_error(const peg::ErrorReport& r,
                                       const SyntaxExpectClasses* cls) {
  if (!r.message.empty()) return r.message;  // a custom message, or a limit

  auto in = [](std::span<const char* const> set, const std::string& key) {
    return std::ranges::any_of(set,
                               [&](const char* k) { return key == k; });
  };
  struct Item {
    std::string key;
    bool after = false, operand = false;
  };
  std::vector<Item> items;
  for (auto& key : syntax_expected_keys(r)) {
    Item it{std::move(key)};
    if (cls) {
      if (in(cls->silent, it.key)) continue;
      it.after = in(cls->after_expr, it.key);
      it.operand = in(cls->operand, it.key);
    }
    items.push_back(std::move(it));
  }

  // A crowd is named when most of it is there. A few of its members are
  // listed like anything else: a parameter may be followed by `:` or `=`, a
  // pattern may open with some of what opens an expression.
  auto most_of = [&](bool Item::* member, std::span<const char* const> all) {
    auto n = static_cast<size_t>(std::ranges::count_if(
        items, [&](const Item& it) { return it.*member; }));
    return n * 2 > all.size();
  };
  const bool operators = cls && most_of(&Item::after, cls->after_expr);
  const bool expression = cls && most_of(&Item::operand, cls->operand);

  std::vector<std::string> parts;
  for (const auto& it : items) {
    if ((operators && it.after) || (expression && it.operand)) continue;
    // Escaped as peglib escapes the unexpected token: a newline the grammar
    // expects does not break the message in two.
    parts.push_back(peg::escape_characters(it.key));
  }
  if (expression) parts.push_back("an expression");
  if (operators) parts.push_back("an operator");

  std::string msg = "syntax error";
  if (!r.unexpected_token.empty())
    msg += ", unexpected '" + r.unexpected_token + "'";
  for (size_t i = 0; i < parts.size(); ++i) {
    msg += i == 0 ? ", expecting " : i + 1 == parts.size() ? " or " : ", ";
    msg += parts[i];
  }
  msg += '.';
  return msg;
}

}  // namespace culebra
