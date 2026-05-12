#pragma once

#include <peglib.h>

#include <format>
#include <print>
#include <string>
#include <vector>

namespace culebra {

const auto grammar_ = R"(
  PROGRAM                  <-  _ STATEMENTS _
  STATEMENTS               <-  (STATEMENT (_sp_ (';' / _nl_) (_ STATEMENT)?)*)?
  STATEMENT                <-  DEBUGGER / RETURN / THROW / BREAK / CONTINUE / DEFER / MULTIFN_DECL / CLASS_DECL / LEXICAL_SCOPE / EXPRESSION

  # Top-level named function declaration. Multiple declarations with
  # the same name and different parameter type signatures form a
  # multimethod (free fn only). Anonymous `fn(...) {...}` keeps its
  # existing role inside expressions.
  MULTIFN_DECL             <-  fn _ IDENTIFIER _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK

  CLASS_DECL               <-  class _ IDENTIFIER _ '{' _ (METHOD (_ METHOD)*)? _ '}'
  METHOD                   <-  IDENTIFIER _ PARAMETERS _ BLOCK

  DEBUGGER                 <-  debugger
  RETURN                   <-  return (_sp_ !_nl_ EXPRESSION)?
  THROW                    <-  throw _sp_ !_nl_ EXPRESSION
  BREAK                    <-  break
  CONTINUE                 <-  continue
  DEFER                    <-  defer _ BLOCK
  LEXICAL_SCOPE            <-  BLOCK

  EXPRESSION               <-  DESTRUCTURE_ASSIGN / ASSIGNMENT / TRY / NIL_COALESCE
  TRY                      <-  try _ BLOCK _ catch _ IDENTIFIER _ BLOCK

  ASSIGNMENT               <-  LET _ MUTABLE _ PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)* (_ TYPE_ANNOTATION)? _ ASSIGN_OP _ EXPRESSION
  DESTRUCTURE_ASSIGN       <-  let _ MUTABLE _ (OBJECT_PATTERN / ARRAY_PATTERN) _ '=' _ EXPRESSION
  # ASSIGN_OP captures the literal so eval_assignment can dispatch on
  # `=` (plain) vs the compound forms. `**=` precedes `*=` so the
  # alternation chooses the longer match.
  ASSIGN_OP                <-  < '**=' / '+=' / '-=' / '*=' / '/=' / '%=' / '@=' / '=' >

  NIL_COALESCE             <-  LOGICAL_OR (_ '??' _ LOGICAL_OR)*
  LOGICAL_OR               <-  LOGICAL_AND (_ '||' _ LOGICAL_AND)*
  LOGICAL_AND              <-  CONDITION (_ '&&' _  CONDITION)*
  CONDITION                <-  RANGE (_ CONDITION_OPERATOR _ RANGE)*
  RANGE                    <-  ADDITIVE (_ RANGE_OPERATOR _ ADDITIVE)?
  # Newline before `+`/`-` does NOT continue the current expression:
  # `let v = X` followed by a line starting with `-y` is two statements.
  # Continuation across newlines requires the operator at line end
  # (or wrapping in parentheses).
  ADDITIVE                 <-  UNARY_PLUS (_h_ ADDITIVE_OPERATOR _ UNARY_PLUS)*
  UNARY_PLUS               <-  UNARY_PLUS_OPERATOR? UNARY_MINUS
  UNARY_MINUS              <-  UNARY_MINUS_OPERATOR? UNARY_NOT
  UNARY_NOT                <-  UNARY_NOT_OPERATOR? MULTIPLICATIVE
  MULTIPLICATIVE           <-  POWER (_ MULTIPLICATIVE_OPERATOR _ POWER)*
  # '**' RHS recurses through UNARY_PLUS so `2 ** -1` and `2 ** 3 ** 4`
  # both parse as in Python (unary prefix allowed, right-associative).
  POWER                    <-  CALL (_ POWER_OPERATOR _ UNARY_PLUS)?

  # Newline before `(` or `[` does NOT extend the previous expression
  # into a call/index — that line is a parenthesized expression or
  # array literal in its own right. `.` is unambiguous (cannot start
  # a statement) so cross-line method chaining still works:
  #   xs.map(...)
  #     .filter(...)   # OK
  CALL                     <-  PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)*
  ARGUMENTS                <-  '(' _ SEQUENCE _ ')'
  INDEX                    <-  '[' _ EXPRESSION _ ']'
  DOT                      <-  '.' _ IDENTIFIER

  SEQUENCE                 <-  (EXPRESSION (_ ',' _ EXPRESSION)*)?

  WHILE                    <-  while _ EXPRESSION _ BLOCK
  FOR                      <-  for _ IDENTIFIER _ in _ EXPRESSION _ BLOCK
  IF                       <-  if _ EXPRESSION _ BLOCK (_ else _ if _ EXPRESSION _ BLOCK)* (_ else _ BLOCK)?

  MATCH                    <-  match _ EXPRESSION _ '{' _ MATCH_ARMS _ '}'
  MATCH_ARMS               <-  (MATCH_ARM (_ ',' _ MATCH_ARM)* _ ','?)?
  MATCH_ARM                <-  PATTERN (_ GUARD)? _ '=>' _ EXPRESSION
  GUARD                    <-  if _ EXPRESSION

  PATTERN                  <-  PRIMARY_PATTERN (_ '|' _ PRIMARY_PATTERN)*
  PRIMARY_PATTERN          <-  WILDCARD / TYPED_IDENT / NIL / BOOLEAN / FLOAT / NUMBER / STRING /
                               ARRAY_PATTERN / OBJECT_PATTERN / IDENTIFIER
  WILDCARD                 <-  '_' !IdentChar
  TYPED_IDENT              <-  IDENTIFIER _ TYPE_ANNOTATION

  ARRAY_PATTERN            <-  '[' _ (ARRAY_PAT_ELEM (_ ',' _ ARRAY_PAT_ELEM)*)? _ ']'
  ARRAY_PAT_ELEM           <-  REST_PATTERN / PATTERN
  REST_PATTERN             <-  '...' _ IDENTIFIER

  OBJECT_PATTERN           <-  '{' _ (IDENTIFIER (_ ',' _ IDENTIFIER)*)? _ '}'

  PRIMARY                  <-  WHILE / FOR / IF / MATCH / FUNCTION / LAMBDA / OBJECT / ARRAY / NIL / BOOLEAN / FLOAT / NUMBER / IDENTIFIER /
                               STRING / INTERPOLATED_STRING / '(' _ EXPRESSION _ ')'

  FUNCTION                 <-  fn _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK
  # Lambda sugar: `|x, y| expr` / `|x, y| { ... }` desugars to
  # `fn(x, y) { ... }`. To return an object literal from an expression
  # body, wrap in parens: `|x| ({a: x})` — otherwise the `{...}` is
  # parsed as a block.
  LAMBDA                   <-  LAMBDA_PARAMS _ (BLOCK / EXPRESSION)
  LAMBDA_PARAMS            <-  '|' _ (PARAMETER (_ ',' _ PARAMETER)*)? _ '|'
  PARAMETERS               <-  '(' _ (PARAMETER (_ ',' _ PARAMETER)*)? _ ')'
  PARAMETER                <-  MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)? (_ '=' _ DEFAULT_VALUE)?
  DEFAULT_VALUE            <-  EXPRESSION

  TYPE_ANNOTATION          <-  ':' _ < [A-Z] [a-zA-Z_0-9]* >
  RETURN_TYPE              <-  '->' _ < [A-Z] [a-zA-Z_0-9]* >

  BLOCK                    <-  '{' _ STATEMENTS _ '}'

  CONDITION_OPERATOR       <-  '==' / '!=' / '<=' / '<' / '>=' / '>'
  RANGE_OPERATOR           <-  < '..=' / '..' >
  ADDITIVE_OPERATOR        <-  [-+]
  UNARY_PLUS_OPERATOR      <-  '+'
  UNARY_MINUS_OPERATOR     <-  '-'
  UNARY_NOT_OPERATOR       <-  '!'
  # '*' negative-lookahead avoids chewing the first '*' of '**' when a
  # left-over arithmetic chain hands back to MULTIPLICATIVE.
  # '@' is matrix-multiply (PEP 465) and shares multiplicative precedence.
  MULTIPLICATIVE_OPERATOR  <-  '*' !'*' / [/%@]
  POWER_OPERATOR           <-  '**'

  LET                      <-  K('let')?
  MUTABLE                  <-  K('mut')?

  IDENTIFIER               <-  < IdentInitChar IdentChar* >

  OBJECT                   <-  '{' _ (OBJECT_PROPERTY (_ ',' _ OBJECT_PROPERTY)*)? _ '}'
  OBJECT_PROPERTY          <-  MUTABLE _ IDENTIFIER (_ ':' _ EXPRESSION)?

  ARRAY                    <-  '[' _ SEQUENCE _ ']' (_ '(' _ EXPRESSION (_ ',' _ EXPRESSION)? _ ')')?

  NIL                      <-  K('nil')
  BOOLEAN                  <-  K('true' / 'false')

  # Float literals. Either (a) integer part followed by a dot and
  # fractional digits, with an optional exponent, or (b) integer part
  # followed directly by an exponent. A trailing-dot form (e.g. `1.`)
  # is intentionally *not* accepted so that `1.foo` stays unambiguous.
  FLOAT                    <-  < [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)?
                              / [0-9]+ [eE] [-+]? [0-9]+ >
  NUMBER                   <-  < [0-9]+ >
  STRING                   <-  ['] < (!['] .)* > [']

  INTERPOLATED_STRING      <-  '"' ('{' _ EXPRESSION _ '}' / INTERPOLATED_CONTENT)* '"'
  # Inside interpolated strings, '\X' is one logical character: the parser
  # keeps both bytes in the captured token and the runtime decoder turns
  # recognized escapes (\n \r \t \\ \" \{) into their byte values. This
  # also lets '\"' and '\{' appear inside the content without prematurely
  # closing the string or starting an interpolation.
  INTERPOLATED_CONTENT     <-  ('\\' . / !["{\\] .)+

  ~let                     <-  K('let')
  ~class                   <-  K('class')
  ~debugger                <-  K('debugger')
  ~while                   <-  K('while')
  ~for                     <-  K('for')
  ~in                      <-  K('in')
  ~if                      <-  K('if')
  ~else                    <-  K('else')
  ~fn                      <-  K('fn')
  ~return                  <-  K('return')
  ~match                   <-  K('match')
  ~throw                   <-  K('throw')
  ~try                     <-  K('try')
  ~catch                   <-  K('catch')
  ~break                   <-  K('break')
  ~continue                <-  K('continue')
  ~defer                   <-  K('defer')

  ~_                       <-  (WhiteSpace / EndOfLine)*
  ~_sp_                    <-  SpaceChar*
  ~_h_                     <-  (SpaceChar / BlockComment)*
  ~_nl_                    <-  LineComment? EndOfLine

  WhiteSpace               <-  SpaceChar / Comment
  Comment                  <-  BlockComment / LineComment

  SpaceChar                <-  ' ' / '\t'
  EndOfLine                <-  '\r\n' / '\n' / '\r'
  IdentInitChar            <-  [a-zA-Z_]
  IdentChar                <-  [a-zA-Z0-9_]
  BlockComment             <-  '/*' (!'*/' .)* '*/'
  LineComment              <-  ('#' / '//') (!EndOfLine .)* &EndOfLine

  K(S)                     <-  < S > !IdentInitChar # Keyward Macro
)";

inline peg::parser& get_parser() {
  // thread_local — peg::parser's logger callback and VM state aren't
  // safe to share across host threads.
  static thread_local peg::parser parser;
  static thread_local bool initialized = false;

  if (!initialized) {
    initialized = true;

    parser.set_logger([&](size_t ln, size_t col, const std::string& msg) {
      std::println(stderr, "{}:{}: {}", ln, col, msg);
    });

    if (!parser.load_grammar(grammar_)) {
      throw std::logic_error("invalid peg grammar");
    }

    parser.enable_ast();
  }

  return parser;
}

// Decode escape sequences in an INTERPOLATED_CONTENT token. Recognized:
//   \n \r \t \\ \" \{
// Unknown '\X' is preserved literally as backslash + char (Python's
// permissive default — keeps the string round-trippable rather than
// silently dropping the escape introducer).
inline std::string decode_interpolated_content(std::string_view raw) {
  // Common case: most plain-text segments contain no backslash. Skip the
  // per-byte loop and emit the source bytes verbatim.
  if (raw.find('\\') == std::string_view::npos) {
    return std::string(raw);
  }
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    char c = raw[i];
    if (c == '\\' && i + 1 < raw.size()) {
      char n = raw[i + 1];
      switch (n) {
        case 'n':  out += '\n'; i++; continue;
        case 'r':  out += '\r'; i++; continue;
        case 't':  out += '\t'; i++; continue;
        case '\\': out += '\\'; i++; continue;
        case '"':  out += '"';  i++; continue;
        case '{':  out += '{';  i++; continue;
        default:
          out += '\\';
          out += n;
          i++;
          continue;
      }
    }
    out += c;
  }
  return out;
}

// Extract an optional TYPE_ANNOTATION sibling (the captured type name) from
// an AST node, returning {} if none is present. The expected position is
// `node.nodes[index]`; pass the tentative slot to probe.
inline std::string_view extract_type_annotation(const peg::Ast& node,
                                                size_t index) {
  using namespace peg::udl;
  if (index < node.nodes.size() &&
      node.nodes[index]->tag == "TYPE_ANNOTATION"_) {
    return node.nodes[index]->token;
  }
  return {};
}

// For a PARAMETER AST node, returns the DEFAULT_VALUE's inner expression
// (or nullptr if the parameter has no default). PARAMETER layout is
// `[MUTABLE, IDENTIFIER, (TYPE_ANNOTATION)?, (DEFAULT_VALUE)?]` and the
// optimizer unwraps DEFAULT_VALUE's single EXPRESSION child.
inline const peg::Ast* extract_default_expr(const peg::Ast& param_node) {
  using namespace peg::udl;
  for (size_t i = 2; i < param_node.nodes.size(); i++) {
    if (param_node.nodes[i]->tag == "DEFAULT_VALUE"_) {
      return param_node.nodes[i]->nodes[0].get();
    }
  }
  return nullptr;
}

// For a FUNCTION AST node, returns the declared return type (or {}) and
// writes the body's node index to *body_idx.
inline std::string_view extract_return_type(const peg::Ast& fn_ast,
                                            size_t& body_idx) {
  using namespace peg::udl;
  if (fn_ast.nodes.size() == 3 &&
      fn_ast.nodes[1]->tag == "RETURN_TYPE"_) {
    body_idx = 2;
    return fn_ast.nodes[1]->token;
  }
  body_idx = 1;
  return {};
}

inline std::shared_ptr<peg::Ast> parse(const std::string& path,
                                       const char* expr, size_t len,
                                       std::vector<std::string>& msgs) {
  auto& parser = get_parser();

  parser.set_logger([&](size_t ln, size_t col, const std::string& err_msg) {
    msgs.push_back(std::format("{}:{}:{}: {}\n", path, ln, col, err_msg));
  });

  std::shared_ptr<peg::Ast> ast;
  if (parser.parse_n(expr, len, ast, path.c_str())) {
    auto opt = peg::AstOptimizer(
        true, {"PARAMETERS", "LAMBDA_PARAMS", "SEQUENCE", "OBJECT",
               "OBJECT_PROPERTY",
               "ARRAY", "RETURN",
               "THROW", "TRY", "DEFER",
               "LEXICAL_SCOPE", "TYPE_ANNOTATION", "RETURN_TYPE",
               "DEFAULT_VALUE",
               "CLASS_DECL", "METHOD",
               "MATCH_ARMS", "GUARD", "ARRAY_PATTERN", "OBJECT_PATTERN",
               "REST_PATTERN"});

    return opt.optimize(ast);
  }

  return nullptr;
}

}  // namespace culebra
