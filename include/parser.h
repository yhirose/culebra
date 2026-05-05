#pragma once

#include <peglib.h>

#include <format>
#include <string>
#include <vector>

namespace culebra {

const auto grammar_ = R"(
  PROGRAM                  <-  _ STATEMENTS _
  STATEMENTS               <-  (STATEMENT (_sp_ (';' / _nl_) (_ STATEMENT)?)*)?
  STATEMENT                <-  DEBUGGER / RETURN / LEXICAL_SCOPE / EXPRESSION

  DEBUGGER                 <-  debugger
  RETURN                   <-  return (_sp_ !_nl_ EXPRESSION)?
  LEXICAL_SCOPE            <-  BLOCK

  EXPRESSION               <-  ASSIGNMENT / LOGICAL_OR

  ASSIGNMENT               <-  LET _ MUTABLE _ PRIMARY (_ (ARGUMENTS / INDEX / DOT))* (_ TYPE_ANNOTATION)? _ '=' _ EXPRESSION

  LOGICAL_OR               <-  LOGICAL_AND (_ '||' _ LOGICAL_AND)*
  LOGICAL_AND              <-  CONDITION (_ '&&' _  CONDITION)*
  CONDITION                <-  ADDITIVE (_ CONDITION_OPERATOR _ ADDITIVE)*
  ADDITIVE                 <-  UNARY_PLUS (_ ADDITIVE_OPERATOR _ UNARY_PLUS)*
  UNARY_PLUS               <-  UNARY_PLUS_OPERATOR? UNARY_MINUS
  UNARY_MINUS              <-  UNARY_MINUS_OPERATOR? UNARY_NOT
  UNARY_NOT                <-  UNARY_NOT_OPERATOR? MULTIPLICATIVE
  MULTIPLICATIVE           <-  CALL (_ MULTIPLICATIVE_OPERATOR _ CALL)*

  CALL                     <-  PRIMARY (_ (ARGUMENTS / INDEX / DOT))*
  ARGUMENTS                <-  '(' _ SEQUENCE _ ')'
  INDEX                    <-  '[' _ EXPRESSION _ ']'
  DOT                      <-  '.' _ IDENTIFIER

  SEQUENCE                 <-  (EXPRESSION (_ ',' _ EXPRESSION)*)?

  WHILE                    <-  while _ EXPRESSION _ BLOCK
  IF                       <-  if _ EXPRESSION _ BLOCK (_ else _ if _ EXPRESSION _ BLOCK)* (_ else _ BLOCK)?

  PRIMARY                  <-  WHILE / IF / FUNCTION / OBJECT / ARRAY / NIL / BOOLEAN / NUMBER / IDENTIFIER /
                               STRING / INTERPOLATED_STRING / '(' _ EXPRESSION _ ')'

  FUNCTION                 <-  fn _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK
  PARAMETERS               <-  '(' _ (PARAMETER (_ ',' _ PARAMETER)*)? _ ')'
  PARAMETER                <-  MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)?

  TYPE_ANNOTATION          <-  ':' _ < [A-Z] [a-zA-Z_0-9]* >
  RETURN_TYPE              <-  '->' _ < [A-Z] [a-zA-Z_0-9]* >

  BLOCK                    <-  '{' _ STATEMENTS _ '}'

  CONDITION_OPERATOR       <-  '==' / '!=' / '<=' / '<' / '>=' / '>'
  ADDITIVE_OPERATOR        <-  [-+]
  UNARY_PLUS_OPERATOR      <-  '+'
  UNARY_MINUS_OPERATOR     <-  '-'
  UNARY_NOT_OPERATOR       <-  '!'
  MULTIPLICATIVE_OPERATOR  <-  [*/%]

  LET                      <-  K('let')?
  MUTABLE                  <-  K('mut')?

  IDENTIFIER               <-  < IdentInitChar IdentChar* >

  OBJECT                   <-  '{' _ (OBJECT_PROPERTY (_ ',' _ OBJECT_PROPERTY)*)? _ '}'
  OBJECT_PROPERTY          <-  MUTABLE _ IDENTIFIER _ ':' _ EXPRESSION

  ARRAY                    <-  '[' _ SEQUENCE _ ']' (_ '(' _ EXPRESSION (_ ',' _ EXPRESSION)? _ ')')?

  NIL                      <-  K('nil')
  BOOLEAN                  <-  K('true' / 'false')

  NUMBER                   <-  < [0-9]+ >
  STRING                   <-  ['] < (!['] .)* > [']

  INTERPOLATED_STRING      <-  '"' ('{' _ EXPRESSION _ '}' / INTERPOLATED_CONTENT)* '"'
  INTERPOLATED_CONTENT     <-  (!["{] .) (!["{] .)*

  ~debugger                <-  K('debugger')
  ~while                   <-  K('while')
  ~if                      <-  K('if')
  ~else                    <-  K('else')
  ~fn                      <-  K('fn')
  ~return                  <-  K('return')

  ~_                       <-  (WhiteSpace / EndOfLine)*
  ~_sp_                    <-  SpaceChar*
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
  static peg::parser parser;
  static bool initialized = false;

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
    auto opt = peg::AstOptimizer(true, {"PARAMETERS", "SEQUENCE", "OBJECT",
                                        "ARRAY", "RETURN", "LEXICAL_SCOPE",
                                        "TYPE_ANNOTATION", "RETURN_TYPE"});

    return opt.optimize(ast);
  }

  return nullptr;
}

}  // namespace culebra
