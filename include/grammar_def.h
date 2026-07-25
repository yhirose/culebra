#pragma once
// culebra PEG grammar — single source of truth.
// parser.h loads it (or the prebuilt grammar_blob.h); tools/gen_grammar_blob.cc
// serializes it into grammar_blob.h. Regenerate with `just gen-blob`.
namespace culebra {

const auto grammar_ = R"(
  PROGRAM                  <-  _ STATEMENTS _
  STATEMENTS               <-  (STATEMENT (_sp_ (';' / _nl_) (_ STATEMENT)?)*)?
  STATEMENT                <-  DEBUGGER / RETURN / THROW / YIELD_FROM / YIELD / BREAK / CONTINUE / DEFER / IMPORT_STMT / EXPORT_STMT / EFFECT_FN_DECL / MULTIFN_DECL / ENUM_DECL / CLASS_DECL / TRAIT_DECL / LEXICAL_SCOPE / EXPRESSION

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

  # Algebraic effects (thin slice). `effect fn op(...)` with no body
  # declares an effect operation that `perform op(...)` may invoke; with a
  # body it is an effectful function whose calls to other effect fns /
  # `perform` are suspension points. Both shapes are lowered, at parse
  # time, onto the generator CPS engine (see effects_transform.h). See
  # [[project-algebraic-effects]].
  EFFECT_FN_DECL           <-  effect _ fn _ CLASS_HEAD _ PARAMETERS (_ RETURN_TYPE)? (_ BLOCK)?

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
  # A class member is one of: a typed instance field (`x: Float32`,
  # `x: Float32 = 0.0` — the type annotation distinguishes it), a static
  # field (`static x = expr`), a getter method (`get name() { ... }`,
  # invoked without parens as `obj.name`), or a plain method (`f(a) { ... }`).
  # The typed field alternative must come first so its `: Type` is matched
  # before the bare-`=` static field. See MethodView / view_method.
  METHOD                   <-  MEMBER_MOD _ IDENTIFIER _ (TYPE_ANNOTATION (_ '=' _ EXPRESSION)? / '=' _ EXPRESSION / PARAMETERS _ BLOCK)
  # A single leading modifier: `static` (field/method) or `get` (getter).
  # `static` and `get` never combine, so one slot keeps METHOD's node
  # layout unchanged; the `< >` capture puts the matched keyword in the
  # MEMBER_MOD node's token (view_method reads it). `get` is contextual —
  # the `&(_ IdentInitChar)` lookahead (outside the capture, so the token
  # stays exactly "get") means it is a modifier only when an identifier
  # follows (the getter's name), so a member literally named `get`
  # (`get()`, `get: Int`) still parses as an ordinary identifier.
  MEMBER_MOD               <-  (< 'static' !IdentInitChar > / < 'get' !IdentInitChar > &(_ IdentInitChar))?

  DEBUGGER                 <-  debugger
  RETURN                   <-  return (_sp_ !_nl_ EXPRESSION)?
  THROW                    <-  throw _sp_ !_nl_ EXPRESSION
  YIELD_FROM               <-  yield _sp_ from _sp_ !_nl_ EXPRESSION                  { no_ast_opt }
  YIELD                    <-  yield _sp_ !_nl_ EXPRESSION                            { no_ast_opt }
  BREAK                    <-  break
  CONTINUE                 <-  continue
  DEFER                    <-  defer _ BLOCK
  LEXICAL_SCOPE            <-  BLOCK

  EXPRESSION               <-  DESTRUCTURE_ASSIGN / PLACE_ASSIGN / ASSIGNMENT / TRY / CONDITIONAL
  # Bit-or-free EXPRESSION used by parameter defaults (`|x = e|`), where a
  # top-level bit-or `|` would be ambiguous with the lambda close delimiter.
  ND_EXPRESSION            <-  DESTRUCTURE_ASSIGN / PLACE_ASSIGN / ASSIGNMENT / TRY / ND_CONDITIONAL
  TRY                      <-  try _ BLOCK _ catch _ IDENTIFIER _ BLOCK

  ASSIGNMENT               <-  LET _ MUTABLE _ PRIMARY (_h_ (ARGUMENTS / INDEX) / _ DOT)* (_ TYPE_ANNOTATION)? _ ASSIGN_OP _ EXPRESSION
  # `let` is optional: `let (a, b) = …` declares; bare `(a, b) = (b, a)`
  # (parallel / swap assignment) reassigns existing variables.
  DESTRUCTURE_ASSIGN       <-  LET _ MUTABLE _ (OBJECT_PATTERN / ARRAY_PATTERN / TUPLE_PATTERN) _ '=' _ EXPRESSION
  # Parallel assignment whose targets may be index / property chains:
  # `(p[i], p[j]) = (p[j], p[i])`, `(self.front, self.back) = …`. Sits after
  # DESTRUCTURE_ASSIGN, so an all-plain-name tuple still takes that (pattern)
  # path — a PATTERN cannot match a postfix chain, so only a target list with
  # at least one chain reaches here. Each target is assigned exactly as the
  # single-target form would (see interpreter.h eval_place_assign), so this
  # form declares nothing new beyond what `x = v` does.
  PLACE_ASSIGN             <-  '(' _ PLACE _ ',' _ PLACE (_ ',' _ PLACE)* _ ','? _ ')' _ '=' _ EXPRESSION
                            /  '(' _ PLACE _ ',' _ ')' _ '=' _ EXPRESSION
  # An assignable location. Same postfix set as ASSIGNMENT's lvalue chain, but
  # anchored on IDENTIFIER rather than PRIMARY: PRIMARY would make every
  # pattern-position identifier speculatively try WHILE / IF / MATCH / … first.
  # With no postfix the node collapses to its lone IDENTIFIER child, which is
  # how both backends tell a plain name from a chain.
  PLACE                    <-  IDENTIFIER (_h_ (ARGUMENTS / INDEX) / _ DOT)*
  # ASSIGN_OP captures the literal so eval_assignment can dispatch on
  # `=` (plain) vs the compound forms. `**=` precedes `*=` so the
  # alternation chooses the longer match. `??=` is the nil-coalescing
  # assign (`a ??= b` assigns only when `a` is nil; short-circuits b).
  ASSIGN_OP                <-  < '**=' / '??=' / '+=' / '-=' / '*=' / '/=' / '%=' / '@=' / '=' >

  # C-style ternary: `c ? a : b`. Right-associative; sits just below
  # assignment and just above `??` (so `a ?? b ? c : d` is `(a ?? b) ? c : d`).
  # `??`/`?.`/`?[` are already consumed lower down, so a `?` reaching here is
  # the ternary; its `:` is consumed within this rule, never colliding with a
  # kwarg / dict `:`. With no `? :`, the single-child node optimizes to its
  # NIL_COALESCE operand, so non-ternary code is unaffected.
  CONDITIONAL              <-  NIL_COALESCE (_ '?' _ EXPRESSION _ ':' _ CONDITIONAL)?
  NIL_COALESCE             <-  LOGICAL_OR (_ '??' _ LOGICAL_OR)*
  LOGICAL_OR               <-  LOGICAL_AND (_ '||' _ LOGICAL_AND)*
  LOGICAL_AND              <-  CONDITION (_ '&&' _  CONDITION)*
  CONDITION                <-  BIT_OR (_ CONDITION_OPERATOR _ BIT_OR)*
  # Bitwise / shift levels (precedence: comparison < | < ^ < & < shift
  # < additive). A single bit-OR `|` infix collides with the `|...|`
  # lambda delimiter under a stateless PEG, so parameter defaults parse
  # through the parallel bit-or-free ladder below (ND_*); a top-level `|`
  # inside a default therefore needs parentheses.
  BIT_OR                   <-  BIT_XOR (_h_ BIT_OR_OPERATOR _ BIT_XOR)*
  # No-bit-or ladder: identical to NIL_COALESCE..CONDITION but bottoming
  # out at BIT_XOR so a bare `|` closes the enclosing `|...|` instead of
  # parsing as bit-or. `{ ast_name }` makes each rung emit its normal tag
  # so the evaluator / JIT need no extra cases.
  ND_CONDITIONAL           <-  ND_NIL_COALESCE (_ '?' _ ND_EXPRESSION _ ':' _ ND_CONDITIONAL)?  { ast_name: CONDITIONAL }
  ND_NIL_COALESCE          <-  ND_LOGICAL_OR (_ '??' _ ND_LOGICAL_OR)*     { ast_name: NIL_COALESCE }
  ND_LOGICAL_OR            <-  ND_LOGICAL_AND (_ '||' _ ND_LOGICAL_AND)*   { ast_name: LOGICAL_OR }
  ND_LOGICAL_AND           <-  ND_CONDITION (_ '&&' _ ND_CONDITION)*       { ast_name: LOGICAL_AND }
  ND_CONDITION             <-  BIT_XOR (_ CONDITION_OPERATOR _ BIT_XOR)*   { ast_name: CONDITION }
  BIT_XOR                  <-  BIT_AND (_h_ BIT_XOR_OPERATOR _ BIT_AND)*
  BIT_AND                  <-  SHIFT (_h_ BIT_AND_OPERATOR _ SHIFT)*
  SHIFT                    <-  RANGE_EXPR (_h_ SHIFT_OPERATOR _ RANGE_EXPR)*
  RANGE_EXPR               <-  RANGE / ADDITIVE
  # A range value (`a..b`, `a..=b`, `a..`, `..b`, `..`), optionally stepped
  # (`0..10 by 2`). Either endpoint may be omitted (open-ended). `_h_` keeps
  # the whole range on one line so an open end (`2..`) can't swallow the
  # next statement. The bare `..` form collapses to a lone RANGE_OPERATOR
  # node (single child); eval and codegen treat that tag as a full range. A
  # plain expression with no operator falls through to ADDITIVE.
  RANGE                    <-  ADDITIVE _h_ RANGE_OPERATOR (_h_ ADDITIVE)? (_h_ BY_STEP)? / RANGE_OPERATOR (_h_ ADDITIVE)? (_h_ BY_STEP)?
  # `by` is contextual (not a reserved word): only recognized right after a
  # range's bounds, so it stays free to use as an ordinary identifier
  # elsewhere. Kept un-collapsed by the AstOptimizer (parser.h keep-list) so
  # RANGE's decoder can tell a step clause apart from an end bound
  # positionally instead of collapsing onto its lone ADDITIVE child.
  BY_STEP                  <-  'by' !IdentInitChar _h_ ADDITIVE
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
  ARG_LIST                 <-  (ARG_ITEM (_ ',' _ ARG_ITEM)* _ ','?)?
  ARG_ITEM                 <-  KWARG_SPLAT / KWARG / EXPRESSION
  KWARG_SPLAT              <-  '**' _ EXPRESSION
  KWARG                    <-  IDENTIFIER _ ':' _ EXPRESSION
  INDEX                    <-  '[' _ EXPRESSION _ ']'
  SAFE_INDEX               <-  '?[' _ EXPRESSION _ ']'
  DOT                      <-  '.' _ IDENTIFIER
  SAFE_DOT                 <-  '?.' _ IDENTIFIER
  NONNULL                  <-  '!!'

  # Array element list. SPREAD_ELEM (`...expr`) splices an iterable's
  # elements into the literal (`[...a, x]`). SPREAD_ELEM is kept by the
  # AstOptimizer so it doesn't collapse to its inner EXPRESSION.
  SEQUENCE                 <-  (SEQ_ELEM (_ ',' _ SEQ_ELEM)* _ ','?)?
  SEQ_ELEM                 <-  SPREAD_ELEM / EXPRESSION
  SPREAD_ELEM              <-  '...' _ EXPRESSION

  # Optional C++17-style init clause (`while mut i = 0; i < n { … }`): a
  # comma-separated list of declaration bindings scoped to the loop, split
  # from the condition by `;`. INIT_CLAUSE is kept by the AstOptimizer so its
  # presence is detectable as WHILE's first child (see view_while). Each
  # binding parses as ASSIGNMENT / DESTRUCTURE_ASSIGN; a shared static pass
  # rejects a bare (non let/mut) binding. No ambiguity: ASSIGNMENT requires an
  # assign op, so a plain `while cond { }` fails the init match and falls
  # through — the `;` is what selects the init form.
  # Optional trailing `nobreak { … }` clause (Python/Zig loop-else, no-break
  # semantics): the block runs when the loop finishes normally (condition
  # false / iterator exhausted), NOT when it exits via break / return / throw.
  # NOBREAK_CLAUSE wraps its BLOCK and is kept by the AstOptimizer so its
  # presence is a last-child tag test (see view_while / view_for). `nobreak` is
  # contextual: it is recognized only in this trailing position, so it stays a
  # valid identifier elsewhere (IDENTIFIER does not exclude keywords).
  WHILE                    <-  while _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ BLOCK (_ NOBREAK_CLAUSE)?
  INIT_CLAUSE              <-  INIT_BINDING (_ ',' _ INIT_BINDING)*
  INIT_BINDING             <-  DESTRUCTURE_ASSIGN / ASSIGNMENT
  NOBREAK_CLAUSE           <-  nobreak _ BLOCK
  # The loop variable may be a destructuring pattern. Bare comma-separated
  # targets `for k, v in obj` are sugar for a parenthesized tuple pattern
  # `for (k, v) in obj`. FOR_BINDING keeps its own node name (not
  # TUPLE_PATTERN's — a single-element tuple pattern `(a,)` is a real
  # TUPLE_PATTERN that must stay un-collapsed, so the two names can't be
  # shared): the AST optimizer collapses FOR_BINDING to its lone child when
  # there is only one target (so `for x in xs` binds a single name), and
  # keeps it as a FOR_BINDING node for two-or-more. The pattern matcher in
  # both backends treats FOR_BINDING identically to TUPLE_PATTERN.
  FOR                      <-  for _ FOR_BINDING _ in _ EXPRESSION _ BLOCK (_ NOBREAK_CLAUSE)?   { no_ast_opt }
  FOR_BINDING              <-  FOR_PAT (_ ',' _ FOR_PAT)*
  FOR_PAT                  <-  TUPLE_PATTERN / ARRAY_PATTERN / OBJECT_PATTERN / IDENTIFIER
  # An optional init clause (`if mut x = f(); x > 0 { … }`, C++17-style) scopes
  # its bindings to the whole if / else-if / else chain. Same INIT_CLAUSE as
  # WHILE; kept by the AstOptimizer so it is detectable as IF's first child,
  # which shifts the cond/block pairing by one (see view_if).
  IF                       <-  if _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ BLOCK (_ else _ if _ EXPRESSION _ BLOCK)* (_ else _ BLOCK)?

  # An optional init clause (`match mut x = f(); x { … }`, C++17-style) scopes
  # its bindings to the subject and every arm. Same INIT_CLAUSE as WHILE / IF;
  # kept by the AstOptimizer so it is detectable as MATCH's first child, which
  # shifts the subject/arms pairing by one (see view_match).
  MATCH                    <-  match _ (INIT_CLAUSE _ ';' _)? EXPRESSION _ '{' _ MATCH_ARMS _ '}'
  MATCH_ARMS               <-  (MATCH_ARM (_ ',' _ MATCH_ARM)* _ ','?)?
  # Arm body: a single EXPRESSION, or a brace BLOCK for multi-statement arms
  # (`=> { stmt; expr }`, Rust-style; the arm yields the block's last value).
  # EXPRESSION is tried first so object/set literals keep their meaning.
  MATCH_ARM                <-  PATTERN (_ GUARD)? _ '=>' _ (EXPRESSION / BLOCK)
  GUARD                    <-  if _ EXPRESSION

  # Subjectless multi-way conditional (Elixir `cond` / Kotlin argless `when`):
  # `cond { test => expr, ..., _ => default }`. The first arm whose test is
  # truthy yields its body; `_` (WILDCARD) is the always-match default. No
  # subject, so the grammar never collides with `match EXPRESSION { … }`.
  COND                     <-  cond _ '{' _ (COND_ARM (_ ',' _ COND_ARM)* _ ','?)? _ '}'
  COND_ARM                 <-  (WILDCARD / EXPRESSION) _ '=>' _ (EXPRESSION / BLOCK)

  PATTERN                  <-  PRIMARY_PATTERN (_ '|' _ PRIMARY_PATTERN)*
  PRIMARY_PATTERN          <-  WILDCARD / CTOR_PATTERN / TYPED_IDENT / NIL / BOOLEAN / FLOAT / NUMBER / STRING / RAW_STRING / INTERPOLATED_STRING /
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

  ARRAY_PATTERN            <-  '[' _ (ARRAY_PAT_ELEM (_ ',' _ ARRAY_PAT_ELEM)* _ ','?)? _ ']'
  ARRAY_PAT_ELEM           <-  REST_PATTERN / PATTERN
  REST_PATTERN             <-  '...' _ IDENTIFIER

  # OBJECT_PAT_ENTRY: either `key: PATTERN` (value match / nested) or
  # a bare identifier (shorthand for `name: name`).
  OBJECT_PATTERN           <-  '{' _ (OBJECT_PAT_ENTRY (_ ',' _ OBJECT_PAT_ENTRY)* _ ','?)? _ '}'
  OBJECT_PAT_ENTRY         <-  IDENTIFIER _ ':' _ PATTERN
                            /  IDENTIFIER

  # Tuple pattern: at least one comma, optional trailing comma. No
  # rest pattern (Tuple is fixed-arity). Mirrors the TUPLE literal.
  TUPLE_PATTERN            <-  '(' _ PATTERN _ ',' _ PATTERN (_ ',' _ PATTERN)* _ ','? _ ')'
                            /  '(' _ PATTERN _ ',' _ ')'

  PRIMARY                  <-  WHILE / FOR / IF / MATCH / COND / HANDLE / PERFORM / FUNCTION / LAMBDA / OBJECT / SET / ARRAY / NIL / BOOLEAN / FLOAT / NUMBER / REGEX_LIT / IDENTIFIER /
                               TRIPLE_STRING / STRING / RAW_STRING / INTERPOLATED_STRING / TUPLE / '(' _ EXPRESSION _ ')'

  # `perform op(args)` invokes an effect operation; it is an expression
  # whose value is what the handler resumes with. `handle { body } with
  # op(resume) { … }` runs `body` under a dynamically-scoped handler for
  # `op`; `resume` is the one-shot continuation. One handle may carry several
  # clauses, each introduced by its own `with` (`handle { … } with get(r) { … }
  # with put(v, r) { … }`) — the leading `with` keyword disambiguates a clause
  # from a following statement. Both are lowered at parse time
  # (effects_transform.h) — no backend-specific runtime.
  # An optional `with return(v) { … }` clause maps the handled computation's
  # normal-completion value; a clause that aborts (never resumes) is not
  # wrapped by it.
  PERFORM                  <-  perform _sp_ IDENTIFIER _ ARGUMENTS
  HANDLE                   <-  handle _ BLOCK (_ with _ (RETURN_CLAUSE / HANDLE_CLAUSE))+
  RETURN_CLAUSE            <-  return _ PARAMETERS _ BLOCK
  HANDLE_CLAUSE            <-  IDENTIFIER _ PARAMETERS _ BLOCK
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
  LAMBDA_PARAMS            <-  '|' _ (PARAMETER (_ ',' _ PARAMETER)* _ ','?)? _ '|'
  PARAMETERS               <-  '(' _ (PARAMETER (_ ',' _ PARAMETER)* _ ','?)? _ ')'
  # A parameter may be a destructuring pattern — `fn ({a, b})`,
  # `fn ([x, y])`, `|(k, v)| …` — which binds the pattern's names from
  # the matching argument (desugared to a synthetic param + a destructure
  # at the function body's entry).
  PARAMETER                <-  KWARGS_REST / ARGS_REST / KW_ONLY_SEP / OBJECT_PATTERN / ARRAY_PATTERN / TUPLE_PATTERN / MUTABLE _ IDENTIFIER (_ TYPE_ANNOTATION)? (_ '=' _ DEFAULT_VALUE)?
  KW_ONLY_SEP              <-  '*' !'*'
  ARGS_REST                <-  '*' _ < IdentInitChar IdentChar* >
  KWARGS_REST              <-  '**' _ < IdentInitChar IdentChar* >
  DEFAULT_VALUE            <-  ND_EXPRESSION

  # TYPE_NAME = single type, optionally Generic, with an optional
  # trailing `?` (the `T?` = `T | Nil` Optional sugar). TYPE_REF wraps
  # Union alternation around a TYPE_ATOM so `Array<Long | Float>` and the
  # outer `Long | Float` use one shared definition. A TYPE_ATOM is either a
  # plain TYPE_NAME or a function type `fn(P, ...) -> R` (FN_TYPE); the
  # return is a single TYPE_ATOM (a top-level `|` after `->` belongs to the
  # surrounding Union, so `fn(A) -> B | C` parses as `(fn(A) -> B) | C`).
  # Captured verbatim into the surrounding TYPE_ANNOTATION / RETURN_TYPE
  # token — whitespace and the `?` sugar are canonicalized later by
  # canonicalize_type_annotation.
  FN_TYPE                  <-  fn _sp_ '(' _sp_ ( TYPE_REF ( _sp_ ',' _sp_ TYPE_REF )* )? _sp_ ')' _sp_ '->' _sp_ TYPE_ATOM
  TYPE_ATOM                <-  FN_TYPE / TYPE_NAME
  TYPE_NAME                <-  [A-Z] [a-zA-Z_0-9]* ( _sp_ '<' _sp_ TYPE_ARG ( _sp_ ',' _sp_ TYPE_ARG )* _sp_ '>' )? '?'?
  TYPE_ARG                 <-  TYPE_REF / [0-9]+
  TYPE_REF                 <-  TYPE_ATOM ( _sp_ '|' _sp_ TYPE_ATOM )*
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
  # `!'|'` so the first `|` of `||` (logical-or) isn't taken as bit-or.
  BIT_OR_OPERATOR          <-  < '|' !'|' >
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

  # A member is either a `...spread` or a key/value property. SPREAD_ELEM
  # is a direct OBJECT child (kept by the optimizer) so eval sees it
  # uncollapsed — same shape as array SEQ_ELEM.
  OBJECT                   <-  '{' _ ((SPREAD_ELEM / OBJECT_PROPERTY) (_ ',' _ (SPREAD_ELEM / OBJECT_PROPERTY))* _ ','?)? _ '}'
  OBJECT_PROPERTY          <-  MUTABLE _ (STRING / RAW_STRING / INTERPOLATED_STRING / FLOAT / NUMBER / NIL / BOOLEAN / TUPLE) _ ':' _ EXPRESSION
                            /  MUTABLE _ IDENTIFIER (_ ':' _ EXPRESSION)?

  # The trailing `(n[, default])` / `(rows, cols)` shape suffix binds with
  # horizontal space only (`_h_`, like the postfix chain in ASSIGNMENT /
  # POSTFIX): with `_` it crossed newlines, so a statement ending in an array
  # literal swallowed the next line's parenthesized statement as its shape
  # (`mut q = [1, 2]` followed by `(a, b) = (b, a)` silently assigned to the
  # shaped array instead of swapping).
  ARRAY                    <-  '[' _ SEQUENCE _ ']' (_h_ '(' _ EXPRESSION (_ ',' _ EXPRESSION)? _ ')')?

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
  # Backtick raw string (Go-style): every byte verbatim between backticks
  # — no escapes, no interpolation, multi-line. Unlike `'...'` it may hold
  # `'`, `"`, and `{`; the only inexpressible byte is a backtick itself.
  # Emits the same AST tag as STRING so the backends treat it identically
  # (raw token, no decode).
  RAW_STRING               <-  '`' < (!'`' .)* > '`'                     { ast_name: STRING }

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
  # `\u{...}` is matched as one unit *before* the bare-`{` interpolation
  # rule so its brace isn't read as an INTERP_EXPR opener. The decoder
  # validates the hex/range; the grammar just delimits the token.
  INTERPOLATED_CONTENT     <-  ('\\u{' [0-9a-fA-F]* '}' / '\\' . / !["{\\] .)+

  # Triple-quoted `"""..."""`: multi-line, interpolated like `"..."` but
  # single/double quotes need no escaping (only `"""` closes). Ideal for
  # embedded LLM prompts. Reuses INTERP_EXPR; TRIPLE_CONTENT stops at
  # `"""` or `{` and still decodes `\X` escapes (use `\{` for a literal
  # brace). Tried before `"` in PRIMARY so `"""` isn't read as `""` + `"`.
  # Swift-style block form (opening `"""` followed by a newline) strips the
  # leading/trailing newline and dedents by the closing delimiter's indent —
  # see normalize_triple_pieces (shared by interp + JIT, so it can't diverge).
  TRIPLE_STRING            <-  '"""' (INTERP_EXPR / TRIPLE_CONTENT)* '"""'
  TRIPLE_CONTENT           <-  ('\\u{' [0-9a-fA-F]* '}' / '\\' . / !('"""' / '{' / '\\') .)+

  # Regex literal: `re'...'` / `re"..."` / `` re`...` ``. The body is RAW — no
  # escape decoding — regardless of which quote delimits it, so regex
  # metacharacters, `\d`/`\w`, and `{n}` quantifiers all pass through verbatim
  # (the `re` prefix overrides the usual escape handling of `"..."`). The one
  # exception is `${expr}` interpolation: it uses `$`, not the bare `{` of
  # `"..."`, precisely because `{n}` is a quantifier here — `${...}` cannot
  # collide with one (a `$` anchor is never quantified in practice). A literal
  # `$` before a `{` is written `\$` (the regex escape for a literal dollar),
  # which also suppresses interpolation. After parsing, a REGEX_LIT is
  # desugared to `Regex.compile(<pattern>, <flags>)`, where `<pattern>` is a
  # raw STRING when there is no interpolation, or a `+`-concatenation of raw
  # STRINGs and `Regex.interp(expr)` calls when there is — so both backends
  # reuse the existing Regex machinery (see desugar in parser.h). `re` not
  # followed by a quote falls through to IDENTIFIER, so `re` stays a name.
  REGEX_LIT                <-  're' REGEX_BODY REGEX_FLAGS
  REGEX_BODY               <-  ['] (REGEX_INTERP / REGEX_CONTENT_SQ)* [']
                            /  '"' (REGEX_INTERP / REGEX_CONTENT_DQ)* '"'
                            /  '`' (REGEX_INTERP / REGEX_CONTENT_BT)* '`'
  REGEX_INTERP             <-  '${' _ EXPRESSION _ '}'
  REGEX_CONTENT_SQ         <-  < ('\\$' / !['] !'${' .)+ >                { ast_name: REGEX_CONTENT }
  REGEX_CONTENT_DQ         <-  < ('\\$' / !'"' !'${' .)+ >                { ast_name: REGEX_CONTENT }
  REGEX_CONTENT_BT         <-  < ('\\$' / !'`' !'${' .)+ >                { ast_name: REGEX_CONTENT }
  REGEX_FLAGS              <-  < [ims]* >

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
  ~cond                    <-  K('cond')
  ~throw                   <-  K('throw')
  ~try                     <-  K('try')
  ~catch                   <-  K('catch')
  ~break                   <-  K('break')
  ~continue                <-  K('continue')
  ~nobreak                 <-  K('nobreak')
  ~defer                   <-  K('defer')
  ~import                  <-  K('import')
  ~export                  <-  K('export')
  ~from                    <-  K('from')
  ~trait                   <-  K('trait')
  ~enum                    <-  K('enum')
  ~effect                  <-  K('effect')
  ~perform                 <-  K('perform')
  ~handle                  <-  K('handle')
  ~with                    <-  K('with')

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

}  // namespace culebra
