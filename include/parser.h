#pragma once

#include <peglib.h>

#include <format>
#include <optional>
#include <print>
#include <string>

#include "shared.h"
#include <vector>

namespace culebra {

const auto grammar_ = R"(
  PROGRAM                  <-  _ STATEMENTS _
  STATEMENTS               <-  (STATEMENT (_sp_ (';' / _nl_) (_ STATEMENT)?)*)?
  STATEMENT                <-  DEBUGGER / RETURN / THROW / YIELD_FROM / YIELD / BREAK / CONTINUE / DEFER / IMPORT_STMT / EXPORT_STMT / MULTIFN_DECL / ENUM_DECL / CLASS_DECL / TRAIT_DECL / LEXICAL_SCOPE / EXPRESSION

  # Module system (§25). `import name from "./path"` binds the file's
  # `export { ... }` value to `name`. String-literal paths only.
  IMPORT_STMT              <-  import _ IDENTIFIER _ from _ STRING
  EXPORT_STMT              <-  export _ '{' _ (IDENTIFIER (_ ',' _ IDENTIFIER)*)? _ '}'

  # Top-level named function declaration. Multiple declarations with
  # the same name and different parameter type signatures form a
  # multimethod (free fn only). Anonymous `fn(...) {...}` keeps its
  # existing role inside expressions. Optional leading decorators
  # transform the declared fn value before binding it to the name —
  # see DECORATOR below.
  MULTIFN_DECL             <-  (DECORATOR (_ DECORATOR)* _)? fn _ CLASS_HEAD _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK

  CLASS_DECL               <-  (DECORATOR (_ DECORATOR)* _)? class _ CLASS_HEAD _ '{' _ (METHOD (_ METHOD)*)? _ '}'

  # `trait` declaration (§15). Methods are either signature-only (must
  # be supplied by the conforming class) or default-impl. Structural
  # conformance: any class whose own methods cover the required ones
  # (arity match) is treated as conforming automatically — no `impl`
  # block needed in this MVP.
  TRAIT_DECL               <-  (DECORATOR (_ DECORATOR)* _)? trait _ TRAIT_HEAD _ '{' _ (TRAIT_METHOD (_ TRAIT_METHOD)*)? _ '}'
  TRAIT_METHOD             <-  IDENTIFIER _ PARAMETERS (_ RETURN_TYPE)? (_ TRAIT_BODY)?
  # TRAIT_BODY wraps BLOCK so AstOptimizer keeps a distinct node
  # indicating "this method has a default impl" — BLOCK itself is
  # folded away by the optimizer.
  TRAIT_BODY               <-  BLOCK

  # `enum Name<T> { Ok(T), Err(String), None }` — a sum type. Each
  # VARIANT lowers to a variant-as-class: constructing `Name.Ok(v)`
  # makes an instance tagged with the variant name and the parent enum
  # name, with positional payload fields `_0.._n`. Nullary variants
  # (`None`) are singleton instances exposed as `Name.None`.
  # VARIANT_FIELD types are documentation in the MVP (payload arity is
  # what matters; runtime values are positional and untyped).
  ENUM_DECL                <-  (DECORATOR (_ DECORATOR)* _)? enum _ CLASS_HEAD _ '{' _ (VARIANT (_ ',' _ VARIANT)* _ ','?)? _ '}'
  VARIANT                  <-  IDENTIFIER (_ '(' _ (VARIANT_FIELD (_ ',' _ VARIANT_FIELD)*)? _ ')')?
  VARIANT_FIELD            <-  < TYPE_REF >

  # Class / trait / fn head with optional Generic type parameters:
  # `Box`, `Box<T>`, `Pair<K, V>`, `sort<T: Comparable>`. Captured into
  # a single token; eval_class_decl / eval_trait_decl / multifn_decl
  # peel off the outer name via parse_generic_head, then split args
  # into per-param `name: bound` pairs via parse_type_param. A bound
  # may be composite (`T: Hashable + Stringer`, all-of). Unbounded
  # params lower to Any; bounded ones lower to the bound trait(s) and
  # are enforced at dispatch / type_check time.
  CLASS_HEAD               <-  < IdentInitChar IdentChar* ( _sp_ '<' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ ':' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ '+' _sp_ [A-Z] [a-zA-Z_0-9]* )* )? ( _sp_ ',' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ ':' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ '+' _sp_ [A-Z] [a-zA-Z_0-9]* )* )? )* _sp_ '>' )? >
  # Trait declaration head: CLASS_HEAD plus an optional supertrait list
  # (`trait Ord: Eq`, `trait Cmp<T>: Eq, Show`). Captured as one token;
  # parse_trait_head splits the name (with generics) from the comma-
  # separated supertraits, whose methods are flattened into the trait.
  TRAIT_HEAD               <-  < IdentInitChar IdentChar* ( _sp_ '<' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ ':' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ '+' _sp_ [A-Z] [a-zA-Z_0-9]* )* )? ( _sp_ ',' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ ':' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ '+' _sp_ [A-Z] [a-zA-Z_0-9]* )* )? )* _sp_ '>' )? ( _sp_ ':' _sp_ [A-Z] [a-zA-Z_0-9]* ( _sp_ ',' _sp_ [A-Z] [a-zA-Z_0-9]* )* )? >

  # `@expr` before a `fn` / `class` declaration. The expression is any
  # CALL chain (`@deco`, `@deco(arg)`, `@module.deco(arg)`); it must
  # evaluate to a callable that takes the declared value and returns
  # the value to bind under the original name. Stacked decorators
  # apply bottom-up (innermost first), mirroring Python.
  # `@` as a binary matmul operator only matches inside expressions,
  # never at statement-prefix position, so the two uses are
  # unambiguous.
  DECORATOR                <-  '@' _ CALL
  METHOD                   <-  STATIC_MOD _ IDENTIFIER _ ('=' _ EXPRESSION / PARAMETERS _ BLOCK)
  STATIC_MOD               <-  K('static')?

  DEBUGGER                 <-  debugger
  RETURN                   <-  return (_sp_ !_nl_ EXPRESSION)?
  THROW                    <-  throw _sp_ !_nl_ EXPRESSION
  YIELD_FROM               <-  yield _sp_ from _sp_ !_nl_ EXPRESSION                  { no_ast_opt }
  YIELD                    <-  yield _sp_ !_nl_ EXPRESSION                            { no_ast_opt }
  BREAK                    <-  break
  CONTINUE                 <-  continue
  DEFER                    <-  defer _ BLOCK
  LEXICAL_SCOPE            <-  BLOCK

  EXPRESSION               <-  DESTRUCTURE_ASSIGN / ASSIGNMENT / TRY / NIL_COALESCE
  TRY                      <-  try _ BLOCK _ catch _ IDENTIFIER _ BLOCK

  ASSIGNMENT               <-  LET _ MUTABLE _ PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)* (_ TYPE_ANNOTATION)? _ ASSIGN_OP _ EXPRESSION
  # `let` is optional: `let (a, b) = …` declares; bare `(a, b) = (b, a)`
  # (parallel / swap assignment) reassigns existing variables.
  DESTRUCTURE_ASSIGN       <-  LET _ MUTABLE _ (OBJECT_PATTERN / ARRAY_PATTERN / TUPLE_PATTERN) _ '=' _ EXPRESSION
  # ASSIGN_OP captures the literal so eval_assignment can dispatch on
  # `=` (plain) vs the compound forms. `**=` precedes `*=` so the
  # alternation chooses the longer match. `??=` is the nil-coalescing
  # assign (`a ??= b` assigns only when `a` is nil; short-circuits b).
  ASSIGN_OP                <-  < '**=' / '??=' / '+=' / '-=' / '*=' / '/=' / '%=' / '@=' / '=' >

  NIL_COALESCE             <-  LOGICAL_OR (_ '??' _ LOGICAL_OR)*
  LOGICAL_OR               <-  LOGICAL_AND (_ '||' _ LOGICAL_AND)*
  LOGICAL_AND              <-  CONDITION (_ '&&' _  CONDITION)*
  CONDITION                <-  BIT_XOR (_ CONDITION_OPERATOR _ BIT_XOR)*
  # Bitwise / shift levels (Python precedence: comparison < ^ < & < shift
  # < additive). Bit-OR `|` is intentionally absent — a single `|` infix
  # collides with the `|...|` lambda close delimiter under a stateless PEG
  # (same reason Set ops use methods, see the note above); combine
  # disjoint flag bits with `+` until a `|` disambiguation lands.
  BIT_XOR                  <-  BIT_AND (_h_ BIT_XOR_OPERATOR _ BIT_AND)*
  BIT_AND                  <-  SHIFT (_h_ BIT_AND_OPERATOR _ SHIFT)*
  SHIFT                    <-  RANGE (_h_ SHIFT_OPERATOR _ RANGE)*
  RANGE                    <-  ADDITIVE (_ RANGE_OPERATOR _ ADDITIVE)?
  # Container operations use method form: Set has `.union(b)` /
  # `.intersect(b)` / `.diff(b)` / `.sym_diff(b)`; Tuple has no
  # concat by design (multi-value returns + fixed records were the
  # primary use case). Numeric / Tensor types keep operators
  # (`+ - * / % ** @`). Set `|` used to be a binary operator here
  # but conflicted with the `|` lambda-close delimiter — routing
  # all four set ops through methods keeps the grammar stateless
  # and unambiguous.
  # Newline before `+`/`-` does NOT continue the current expression:
  # `let v = X` followed by a line starting with `-y` is two statements.
  # Continuation across newlines requires the operator at line end
  # (or wrapping in parentheses).
  ADDITIVE                 <-  UNARY_PLUS (_h_ ADDITIVE_OPERATOR _ UNARY_PLUS)*
  UNARY_PLUS               <-  UNARY_PLUS_OPERATOR? UNARY_MINUS
  UNARY_MINUS              <-  UNARY_MINUS_OPERATOR? UNARY_NOT
  UNARY_NOT                <-  UNARY_NOT_OPERATOR? UNARY_BNOT
  UNARY_BNOT               <-  UNARY_BNOT_OPERATOR? MULTIPLICATIVE
  # `_h_` (no newline) before the operator matches ADDITIVE's rule
  # so `}\n@deco fn ...` is two statements (decorator on a fresh fn),
  # not a matmul continuation of the preceding expression.
  MULTIPLICATIVE           <-  POWER (_h_ MULTIPLICATIVE_OPERATOR _ POWER)*
  # '**' RHS recurses through UNARY_PLUS so `2 ** -1` and `2 ** 3 ** 4`
  # both parse as in Python (unary prefix allowed, right-associative).
  POWER                    <-  CALL (_ POWER_OPERATOR _ UNARY_PLUS)?

  # Newline before `(` or `[` does NOT extend the previous expression
  # into a call/index — that line is a parenthesized expression or
  # array literal in its own right. `.` is unambiguous (cannot start
  # a statement) so cross-line method chaining still works:
  #   xs.map(...)
  #     .filter(...)   # OK
  # Postfix chain. SAFE_DOT (`?.`) and SAFE_INDEX (`?[...]`) are the
  # optional-chaining operators: a nil receiver short-circuits the whole
  # remaining chain to nil. NONNULL (`!!`) is the non-null assertion
  # (throws NilError on nil, else passes the value through). `?.` / `?[`
  # are matched before plain DOT / INDEX; none collide with `??` (that
  # needs `?` then `?`) or `!=` (that needs `!` then `=`).
  CALL                     <-  PRIMARY (_h_ (ARGUMENTS / INDEX) / _ SAFE_DOT / _ SAFE_INDEX / _ DOT / NONNULL)*
  ARGUMENTS                <-  '(' _ ARG_LIST _ ')'
  ARG_LIST                 <-  (ARG_ITEM (_ ',' _ ARG_ITEM)*)?
  ARG_ITEM                 <-  KWARG_SPLAT / KWARG / EXPRESSION
  KWARG_SPLAT              <-  '**' _ EXPRESSION
  KWARG                    <-  IDENTIFIER _ ':' _ EXPRESSION
  INDEX                    <-  '[' _ EXPRESSION _ ']'
  SAFE_INDEX               <-  '?[' _ EXPRESSION _ ']'
  DOT                      <-  '.' _ IDENTIFIER
  SAFE_DOT                 <-  '?.' _ IDENTIFIER
  NONNULL                  <-  '!!'

  SEQUENCE                 <-  (EXPRESSION (_ ',' _ EXPRESSION)*)?

  WHILE                    <-  while _ EXPRESSION _ BLOCK
  FOR                      <-  for _ IDENTIFIER _ in _ EXPRESSION _ BLOCK         { no_ast_opt }
  IF                       <-  if _ EXPRESSION _ BLOCK (_ else _ if _ EXPRESSION _ BLOCK)* (_ else _ BLOCK)?

  MATCH                    <-  match _ EXPRESSION _ '{' _ MATCH_ARMS _ '}'
  MATCH_ARMS               <-  (MATCH_ARM (_ ',' _ MATCH_ARM)* _ ','?)?
  MATCH_ARM                <-  PATTERN (_ GUARD)? _ '=>' _ EXPRESSION
  GUARD                    <-  if _ EXPRESSION

  PATTERN                  <-  PRIMARY_PATTERN (_ '|' _ PRIMARY_PATTERN)*
  PRIMARY_PATTERN          <-  WILDCARD / CTOR_PATTERN / TYPED_IDENT / NIL / BOOLEAN / FLOAT / NUMBER / STRING /
                               ARRAY_PATTERN / OBJECT_PATTERN / TUPLE_PATTERN / IDENTIFIER
  WILDCARD                 <-  '_' !IdentChar
  # Enum constructor pattern: `Ok(x)`, `Result.Ok(x)`, `Pair(a, b)`.
  # The ctor path (variant name, optionally `Enum.Variant`) is the
  # captured token; the children are the positional payload sub-patterns
  # matched against the instance's `_0.._n` fields. Nullary variants are
  # matched with the plain type pattern (`_: None`), so no parens form.
  CTOR_PATTERN             <-  CTOR_PATH _ '(' _ (PATTERN (_ ',' _ PATTERN)*)? _ ')'
  CTOR_PATH                <-  < IdentInitChar IdentChar* ( '.' IdentInitChar IdentChar* )? >
  TYPED_IDENT              <-  IDENTIFIER _ TYPE_ANNOTATION

  ARRAY_PATTERN            <-  '[' _ (ARRAY_PAT_ELEM (_ ',' _ ARRAY_PAT_ELEM)*)? _ ']'
  ARRAY_PAT_ELEM           <-  REST_PATTERN / PATTERN
  REST_PATTERN             <-  '...' _ IDENTIFIER

  # OBJECT_PAT_ENTRY: either `key: PATTERN` (value match / nested) or
  # a bare identifier (shorthand for `name: name`).
  OBJECT_PATTERN           <-  '{' _ (OBJECT_PAT_ENTRY (_ ',' _ OBJECT_PAT_ENTRY)*)? _ '}'
  OBJECT_PAT_ENTRY         <-  IDENTIFIER _ ':' _ PATTERN
                            /  IDENTIFIER

  # Tuple pattern: at least one comma, optional trailing comma. No
  # rest pattern (Tuple is fixed-arity). Mirrors the TUPLE literal.
  TUPLE_PATTERN            <-  '(' _ PATTERN _ ',' _ PATTERN (_ ',' _ PATTERN)* _ ','? _ ')'
                            /  '(' _ PATTERN _ ',' _ ')'

  PRIMARY                  <-  WHILE / FOR / IF / MATCH / FUNCTION / LAMBDA / OBJECT / SET / ARRAY / NIL / BOOLEAN / FLOAT / NUMBER / IDENTIFIER /
                               STRING / INTERPOLATED_STRING / TUPLE / '(' _ EXPRESSION _ ')'
  TUPLE                    <-  '(' _ EXPRESSION _ ',' _ EXPRESSION (_ ',' _ EXPRESSION)* _ ','? _ ')'
                            /  '(' _ EXPRESSION _ ',' _ ')'
  SET                      <-  '{' _ EXPRESSION _ ',' _ EXPRESSION (_ ',' _ EXPRESSION)* _ ','? _ '}'
                            /  '{' _ EXPRESSION _ ',' _ '}'

  FUNCTION                 <-  fn _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK
  # Lambda sugar: `|x, y| expr` desugars to `fn (x, y) expr`. Body is
  # restricted to a single EXPRESSION (not BLOCK) — for multiple
  # statements / intermediate `let` / side effects, use `fn (...) { ... }`.
  # `if` / `while` / `for` / `match` / `try` are themselves expressions,
  # so they work as the body: `|x| if x < 0 { -x } else { x }`.
  # Object literals as the body are EXPRESSION-position OBJECTs:
  # `|x| {a: x}` returns `{a: x}` unambiguously.
  LAMBDA                   <-  LAMBDA_PARAMS _ EXPRESSION
  LAMBDA_PARAMS            <-  '|' _ (PARAMETER (_ ',' _ PARAMETER)*)? _ '|'
  PARAMETERS               <-  '(' _ (PARAMETER (_ ',' _ PARAMETER)*)? _ ')'
  PARAMETER                <-  KWARGS_REST / KW_ONLY_SEP / MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)? (_ '=' _ DEFAULT_VALUE)?
  KW_ONLY_SEP              <-  '*' !'*'
  KWARGS_REST              <-  '**' _ < IdentInitChar IdentChar* >
  DEFAULT_VALUE            <-  EXPRESSION

  # TYPE_NAME = single type, optionally Generic, with an optional
  # trailing `?` (the `T?` = `T | Nil` Optional sugar). TYPE_REF wraps
  # Union alternation around it so `Array<Long | Float>` and the outer
  # `Long | Float` use one shared definition. Captured verbatim into
  # the surrounding TYPE_ANNOTATION / RETURN_TYPE token — whitespace and
  # the `?` sugar are canonicalized later by canonicalize_type_annotation.
  TYPE_NAME                <-  [A-Z] [a-zA-Z_0-9]* ( _sp_ '<' _sp_ TYPE_REF ( _sp_ ',' _sp_ TYPE_REF )* _sp_ '>' )? '?'?
  TYPE_REF                 <-  TYPE_NAME ( _sp_ '|' _sp_ TYPE_NAME )*
  TYPE_ANNOTATION          <-  ':' _ < TYPE_REF >
  RETURN_TYPE              <-  '->' _ < TYPE_REF >

  BLOCK                    <-  '{' _ STATEMENTS _ '}'

  CONDITION_OPERATOR       <-  '==' / '!=' / '<=' / '<' / '>=' / '>'
  RANGE_OPERATOR           <-  < '..=' / '..' >
  ADDITIVE_OPERATOR        <-  [-+]
  UNARY_PLUS_OPERATOR      <-  '+'
  UNARY_MINUS_OPERATOR     <-  '-'
  UNARY_NOT_OPERATOR       <-  '!'
  UNARY_BNOT_OPERATOR      <-  '~'
  BIT_XOR_OPERATOR         <-  < '^' >
  BIT_AND_OPERATOR         <-  < '&' >
  SHIFT_OPERATOR           <-  < '<<' / '>>' >
  # '*' negative-lookahead avoids chewing the first '*' of '**' when a
  # left-over arithmetic chain hands back to MULTIPLICATIVE.
  # '@' is matrix-multiply (PEP 465) and shares multiplicative precedence.
  MULTIPLICATIVE_OPERATOR  <-  '*' !'*' / [/%@]
  POWER_OPERATOR           <-  '**'

  LET                      <-  K('let')?
  MUTABLE                  <-  K('mut')?

  IDENTIFIER               <-  < IdentInitChar IdentChar* >

  OBJECT                   <-  '{' _ (OBJECT_PROPERTY (_ ',' _ OBJECT_PROPERTY)*)? _ '}'
  OBJECT_PROPERTY          <-  MUTABLE _ (FLOAT / NUMBER / NIL / BOOLEAN / TUPLE) _ ':' _ EXPRESSION
                            /  MUTABLE _ IDENTIFIER (_ ':' _ EXPRESSION)?

  ARRAY                    <-  '[' _ SEQUENCE _ ']' (_ '(' _ EXPRESSION (_ ',' _ EXPRESSION)? _ ')')?

  NIL                      <-  K('nil')
  BOOLEAN                  <-  K('true' / 'false')

  # Float literals. Either (a) integer part followed by a dot and
  # fractional digits, with an optional exponent, or (b) integer part
  # followed directly by an exponent. A trailing-dot form (e.g. `1.`)
  # is intentionally *not* accepted so that `1.foo` stays unambiguous.
  FLOAT                    <-  < [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)?
                              / [0-9]+ [eE] [-+]? [0-9]+ >
  # Integer literal: hex `0x` / octal `0o` / binary `0b` radix prefixes,
  # or plain decimal. Radix forms precede the decimal alt so `0x1f`
  # isn't split into `0` + `x1f`. FLOAT is tried before NUMBER, so a
  # leading-`0` decimal that is really a float (`0.5`) still wins.
  NUMBER                   <-  < '0' [xX] [0-9a-fA-F]+ / '0' [oO] [0-7]+ / '0' [bB] [01]+ / [0-9]+ >
  STRING                   <-  ['] < (!['] .)* > [']

  # `{expr}` embeds an expression; `{expr:spec}` adds a std::format-style
  # format spec (`{x:.2f}`, `{n:05}`, `{n:x}`, `{s:>10}`). INTERP_EXPR
  # collapses to the bare EXPRESSION when no spec is present.
  INTERPOLATED_STRING      <-  '"' (INTERP_EXPR / INTERPOLATED_CONTENT)* '"'
  INTERP_EXPR              <-  '{' _ EXPRESSION (_ ':' FORMAT_SPEC)? _ '}'
  FORMAT_SPEC              <-  < (![}] .)* >
  # Inside interpolated strings, '\X' is one logical character: the parser
  # keeps both bytes in the captured token and the runtime decoder turns
  # recognized escapes (\n \r \t \\ \" \{) into their byte values. This
  # also lets '\"' and '\{' appear inside the content without prematurely
  # closing the string or starting an interpolation.
  INTERPOLATED_CONTENT     <-  ('\\' . / !["{\\] .)+

  ~class                   <-  K('class')
  ~debugger                <-  K('debugger')
  ~while                   <-  K('while')
  ~for                     <-  K('for')
  ~in                      <-  K('in')
  ~if                      <-  K('if')
  ~else                    <-  K('else')
  ~fn                      <-  K('fn')
  ~return                  <-  K('return')
  ~yield                   <-  K('yield')
  ~match                   <-  K('match')
  ~throw                   <-  K('throw')
  ~try                     <-  K('try')
  ~catch                   <-  K('catch')
  ~break                   <-  K('break')
  ~continue                <-  K('continue')
  ~defer                   <-  K('defer')
  ~import                  <-  K('import')
  ~export                  <-  K('export')
  ~from                    <-  K('from')
  ~trait                   <-  K('trait')
  ~enum                    <-  K('enum')

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

// View of an ASSIGNMENT AST node — see grammar:
//   ASSIGNMENT <- LET _ MUTABLE _ PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)*
//                 (_ TYPE_ANNOTATION)? _ ASSIGN_OP _ EXPRESSION
// Layout: [LET, MUTABLE, lval-chain..., (TYPE_ANNOTATION)?, ASSIGN_OP, EXPRESSION].
// `lvaloff` is the index of the first lvalue child (always 2 today;
// exposed so callers iterate as `ast.nodes[av.lvaloff + i]`). All four
// walkers (interp shadow / interp eval / JIT shadow / JIT compile) and
// both passes (collect_fn_locals, visit_for_frees) read through this
// view so a future grammar tweak only updates view_assignment.
struct AssignmentView {
  bool is_let;                  // node[0].token == "let"
  bool is_mut;                  // node[1].token == "mut"
  bool compound;                // op_token != "="
  std::string_view op_token;    // e.g. "+=", "*=", "="
  std::string_view op_base;     // op_token with trailing '=' stripped (compound), else ""
  std::string_view type_annotation;  // "" when absent
  int lvalcnt;                  // count of lvalue chain items
  size_t lvaloff;               // index of first lvalue child (= 2)
  const peg::Ast* rhs;          // node.back() — EXPRESSION
};

inline AssignmentView view_assignment(const peg::Ast& a) {
  std::string_view op_token = a.nodes[a.nodes.size() - 2]->token;
  bool compound = op_token != "=";
  auto type_annotation = extract_type_annotation(a, a.nodes.size() - 3);
  int lvalcnt = static_cast<int>(a.nodes.size()) - 4;
  if (!type_annotation.empty()) lvalcnt--;
  return AssignmentView{
      a.nodes[0]->token == "let",
      a.nodes[1]->token == "mut",
      compound,
      op_token,
      compound ? op_token.substr(0, op_token.size() - 1) : std::string_view{},
      type_annotation,
      lvalcnt,
      /*lvaloff=*/2,
      a.nodes.back().get(),
  };
}

// View of an OBJECT_PROPERTY AST node — see grammar:
//   OBJECT_PROPERTY <- MUTABLE _ (FLOAT/NUMBER/NIL/BOOLEAN/TUPLE/IDENTIFIER) _ ':' _ EXPRESSION
//   (shorthand) OBJECT_PROPERTY <- MUTABLE _ IDENTIFIER   (no ':' or value)
// Layouts: long form size 3 `[MUTABLE, KEY, EXPRESSION]`,
//          shorthand size 2 `[MUTABLE, IDENTIFIER]` (IDENTIFIER doubles
//          as the key and the read-from-scope value).
// Normalizing `value` (shorthand collapses to the identifier node) lets
// closure-capture walkers visit a single child without branching.
struct ObjectPropertyView {
  bool is_shorthand;            // size() == 2
  bool is_mut;                  // node[0].token == "mut"
  const peg::Ast* key;          // node[1]
  const peg::Ast* value;        // shorthand: same as key; long: node[2]
};

inline ObjectPropertyView view_object_property(const peg::Ast& p) {
  bool is_shorthand = p.nodes.size() == 2;
  return ObjectPropertyView{
      is_shorthand,
      p.nodes[0]->token == "mut",
      p.nodes[1].get(),
      is_shorthand ? p.nodes[1].get() : p.nodes[2].get(),
  };
}

// View of a TRAIT_METHOD AST node — see grammar:
//   TRAIT_METHOD <- IDENTIFIER _ PARAMETERS (_ RETURN_TYPE)? (_ TRAIT_BODY)?
// Both RETURN_TYPE and TRAIT_BODY are optional, so the body slot
// floats — the loop below tag-tests instead of indexing. `body` is an
// empty shared_ptr for signature-only methods (required-method form);
// when present it points at the inner BLOCK (TRAIT_BODY wraps BLOCK so
// AstOptimizer doesn't fold it).
struct TraitMethodView {
  std::string_view name;
  size_t name_line;
  size_t name_col;
  const peg::Ast* params;             // node[1]
  std::string_view return_type;       // "" when absent
  std::shared_ptr<peg::Ast> body;     // empty for sig-only methods
};

inline TraitMethodView view_trait_method(const peg::Ast& m) {
  using namespace peg::udl;
  const auto& ident = *m.nodes[0];
  std::shared_ptr<peg::Ast> body;
  std::string_view return_type;
  for (size_t j = 2; j < m.nodes.size(); j++) {
    if (m.nodes[j]->tag == "RETURN_TYPE"_) {
      return_type = m.nodes[j]->token;
    } else if (m.nodes[j]->tag == "TRAIT_BODY"_) {
      body = m.nodes[j]->nodes.empty() ? m.nodes[j] : m.nodes[j]->nodes[0];
    }
  }
  return TraitMethodView{
      ident.token, ident.line, ident.column,
      m.nodes[1].get(), return_type, body,
  };
}

// View of a METHOD AST node — see grammar:
//   METHOD <- STATIC_MOD _ IDENTIFIER _ ('=' _ EXPRESSION / PARAMETERS _ BLOCK)
// `is_field` distinguishes the two tails (size 3 vs 4). One central
// accessor avoids walker drift when the rule changes again.
struct MethodView {
  bool is_static;
  std::string_view name;
  size_t name_line;
  size_t name_col;
  bool is_field;
  const peg::Ast* params;                       // nullptr if field
  const std::shared_ptr<peg::Ast>* body;        // nullptr if field
  const peg::Ast* value;                        // nullptr if method
};

inline MethodView view_method(const peg::Ast& m) {
  const auto& ident = *m.nodes[1];
  bool is_field = m.nodes.size() == 3;
  return MethodView{
      m.nodes[0]->token == "static",
      ident.token,
      ident.line,
      ident.column,
      is_field,
      is_field ? nullptr : m.nodes[2].get(),
      is_field ? nullptr : &m.nodes[3],
      is_field ? m.nodes[2].get() : nullptr,
  };
}

// View of a VARIANT AST node — see grammar:
//   VARIANT <- IDENTIFIER (_ '(' (VARIANT_FIELD ...)? ')')?
// `arity` is the positional payload count (0 = nullary). VARIANT is
// kept un-collapsed by the AstOptimizer so nodes[0] is always the name.
struct VariantView {
  std::string_view name;
  size_t name_line;
  size_t name_col;
  size_t arity;
};
inline VariantView view_variant(const peg::Ast& v) {
  const auto& ident = *v.nodes[0];
  return VariantView{ident.token, ident.line, ident.column,
                     v.nodes.size() - 1};
}

// Stable storage for synthetic positional payload field names
// (`_0`, `_1`, ...) so a FunctionValue::Parameter's string_view name
// and the instance field key outlive the call. Capped well above any
// realistic variant arity.
inline std::string_view positional_field_name(size_t i) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    v.reserve(64);
    for (size_t k = 0; k < 64; k++) v.push_back("_" + std::to_string(k));
    return v;
  }();
  return names.at(i);
}

// `static`-modifier check for the field form (size 3). Throws a single
// canonical SyntaxError so interp and JIT diagnostics stay identical.
inline void require_static_field(const MethodView& mv,
                                 std::string_view class_name) {
  if (mv.is_static) return;
  throw CulebraError(
      "SyntaxError",
      std::format("field `{}` in class `{}` must be declared with `static`",
                  mv.name, class_name),
      static_cast<long>(mv.name_line),
      static_cast<long>(mv.name_col));
}

// View of a `@derive(...)` decorator. The grammar is
//   DECORATOR <- '@' _ CALL
// and `@derive(Eq, Hash, Show, Comparable)` parses (post-AstOptimizer,
// DECORATOR kept un-collapsed) to:
//   DECORATOR
//     CALL
//       IDENTIFIER "derive"      (callee — PRIMARY collapsed)
//       ARG_LIST [ IDENTIFIER "Eq", IDENTIFIER "Hash", ... ]
// Returns the requested trait identifiers when the decorator is a bare
// `derive(...)` call, or an empty vector otherwise (a regular decorator
// the caller applies as a function). The eval/compile side validates the
// names against the supported set. Used by both interp (`eval_class_decl`)
// and JIT (`compile_class_decl`) so the directive is recognized
// identically — see project_type_system.md §D.
inline std::vector<std::string_view> view_derive(const peg::Ast& decorator) {
  std::vector<std::string_view> traits;
  if (decorator.nodes.empty()) return traits;
  const auto& call = *decorator.nodes[0];
  // A derive directive is `derive(...)`: a 2-child CALL whose callee is
  // the bare identifier `derive` and whose arg list follows.
  if (call.nodes.size() < 2) return traits;
  if (call.nodes[0]->token != "derive") return traits;
  for (const auto& arg : call.nodes[1]->nodes) {
    traits.push_back(arg->token);
  }
  return traits;
}

// Resolve a `@derive` trait name to the method it generates and the
// runtime selector the JIT uses (`make_derived_method`). Throws the
// canonical SyntaxError on an unknown name so interp / JIT diagnostics
// stay identical — same rationale as require_static_field. Shared by
// both backends; see project_type_system.md §D.
struct DerivedMethod {
  std::string_view name;  // generated method name (static-lifetime literal)
  int kind;               // 0=eq, 1=hash, 2=show, 3=cmp
};
inline DerivedMethod derive_method_for(std::string_view trait) {
  if (trait == "Eq") return {"eq", 0};
  if (trait == "Hash") return {"hash", 1};
  if (trait == "Show") return {"to_s", 2};
  if (trait == "Comparable") return {"cmp", 3};
  throw CulebraError(
      "SyntaxError",
      std::format("@derive: unknown trait `{}` (expected Eq, Hash, "
                  "Show, or Comparable)",
                  trait));
}

inline bool is_kw_only_sep(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "KW_ONLY_SEP"_;
}

inline bool is_kwargs_rest(const peg::Ast& node) {
  using namespace peg::udl;
  return node.tag == "KWARGS_REST"_;
}

// Returns (name, line, col) for a PARAMETER-shaped AST node. The
// KWARGS_REST shape stores the name as the node's own token; normal
// parameters keep it on the IDENTIFIER child at index 1.
struct ParamNameLoc {
  std::string_view name;
  size_t line;
  size_t column;
};
inline ParamNameLoc extract_param_name_loc(const peg::Ast& p) {
  if (is_kwargs_rest(p)) return {p.token, p.line, p.column};
  const auto& id = *p.nodes[1];
  return {id.token, id.line, id.column};
}

// Returns the index of the first kw-only parameter (the cap on
// allowed positional arguments), or nullopt if there is no `*`
// separator. Templated so it works for both FunctionValue::Parameter
// (interp) and JIT-local ParamInfo, which share a `kw_only` field.
template <class P>
inline std::optional<size_t> first_kw_only_index(
    const std::vector<P>& params) {
  for (size_t i = 0; i < params.size(); i++) {
    if (params[i].kw_only) return i;
  }
  return std::nullopt;
}

// Count of regular (positional-bindable) params — those with neither
// `kw_only` nor `kwargs_rest` set. This is where overflow positionals
// stop being consumed by formal slots and start flowing to __ARGS__.
template <class P>
inline size_t regular_param_count(const std::vector<P>& params) {
  size_t n = 0;
  for (auto& p : params) {
    if (!p.kw_only && !p.kwargs_rest) n++;
  }
  return n;
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

// View of a PARAMETER AST node — see grammar:
//   PARAMETER <- KWARGS_REST / KW_ONLY_SEP
//              / MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)? (_ '=' _ DEFAULT_VALUE)?
// Three forms collapse to one struct: KW_ONLY_SEP (`*`) carries only the
// separator flag, KWARGS_REST (`**name`) carries name on the node token
// itself, normal parameters carry `[MUTABLE, IDENTIFIER, ...]`. Callers
// that consume multiple fields read through the view; pure boolean
// predicates (`is_kw_only_sep` / `is_kwargs_rest`) remain available.
struct ParameterView {
  bool is_kwargs_rest;          // tag == KWARGS_REST
  bool is_kw_only_sep;          // tag == KW_ONLY_SEP
  bool is_mut;                  // normal form: node[0].token == "mut"
  std::string_view name;        // KWARGS_REST: p.token; normal: IDENTIFIER child
  size_t name_line;
  size_t name_col;
  std::string_view type_annotation;   // normal form, "" when absent
  const peg::Ast* default_value;      // normal form, nullptr when absent
};

inline ParameterView view_parameter(const peg::Ast& p) {
  if (is_kw_only_sep(p)) {
    return ParameterView{false, true, false, {}, p.line, p.column, {}, nullptr};
  }
  auto loc = extract_param_name_loc(p);
  if (is_kwargs_rest(p)) {
    return ParameterView{true, false, false, loc.name, loc.line, loc.column,
                         {}, nullptr};
  }
  return ParameterView{
      false, false,
      p.nodes[0]->token == "mut",
      loc.name, loc.line, loc.column,
      extract_type_annotation(p, 2),
      extract_default_expr(p),
  };
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

// View of a FUNCTION or LAMBDA AST node — see grammar:
//   FUNCTION <- fn _ PARAMETERS (_ RETURN_TYPE)? _ BLOCK
//   LAMBDA   <- LAMBDA_PARAMS _ (BLOCK / EXPRESSION)
// Layouts: FUNCTION size 2 `[PARAMETERS, BLOCK]` or size 3 with
//          `[PARAMETERS, RETURN_TYPE, BLOCK]`; LAMBDA always size 2
//          `[LAMBDA_PARAMS, BODY]` (no return type slot).
// One struct covers both: `return_type` is "" for LAMBDA. `body` is
// `std::shared_ptr<peg::Ast>` so callers can hand it directly to
// `make_function_value` (which retains ownership).
// Phase 3 (`yield` / generator) will grow this view with an
// `is_generator` flag once YIELD scanning lands; centralizing here
// keeps the lowering site count to one.
struct FunctionView {
  const peg::Ast* params;             // node[0]
  std::string_view return_type;       // "" for LAMBDA / no annotation
  std::shared_ptr<peg::Ast> body;     // node[body_idx]
};

inline FunctionView view_function(const peg::Ast& fn) {
  size_t body_idx = 1;
  auto rt = extract_return_type(fn, body_idx);
  return FunctionView{fn.nodes[0].get(), rt, fn.nodes[body_idx]};
}

inline FunctionView view_lambda(const peg::Ast& lam) {
  return FunctionView{lam.nodes[0].get(), {}, lam.nodes[1]};
}

// Parse the built-in trait preamble (`trait Stringer`, `trait Eq`,
// `trait Comparable` + defaults). Lazily cached so subsequent calls
// reuse the same AST; the AST is process-lifetime so dependent
// modules' string_views aliasing the source stay valid.
inline std::shared_ptr<peg::Ast> parse_builtin_traits_preamble();

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
               "OBJECT_PROPERTY", "TUPLE", "SET",
               "ARRAY", "RETURN",
               "THROW", "YIELD", "YIELD_FROM", "TRY", "DEFER", "FOR",
               "LEXICAL_SCOPE", "TYPE_ANNOTATION", "RETURN_TYPE",
               "DEFAULT_VALUE",
               "ARG_LIST", "KWARG", "KWARG_SPLAT",
               "CLASS_DECL", "METHOD", "DECORATOR",
               "TRAIT_DECL", "TRAIT_METHOD", "TRAIT_BODY",
               "ENUM_DECL", "VARIANT",
               "MATCH_ARMS", "GUARD", "ARRAY_PATTERN", "OBJECT_PATTERN",
               "TUPLE_PATTERN", "CTOR_PATTERN",
               "REST_PATTERN", "INTERP_EXPR", "INTERPOLATED_STRING",
               "IMPORT_STMT", "EXPORT_STMT"});

    return opt.optimize(ast);
  }

  return nullptr;
}

inline std::shared_ptr<peg::Ast> parse_builtin_traits_preamble() {
  static auto cached = [] {
    auto src = builtin_traits_preamble();
    std::vector<std::string> ignore;
    return parse("<builtin>", src.data(), src.size(), ignore);
  }();
  return cached;
}

}  // namespace culebra
