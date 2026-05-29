# Culebra Language Specification

This document defines the syntax and runtime semantics of the Culebra
programming language. It is normative: where the two backends
(interpreter in `include/interpreter.h` and JIT in `include/jit.h`)
disagree, the interpreter is considered authoritative and the JIT
tracks its behavior.

For an introductory tour with runnable examples, see
[`guide.md`](guide.md). For API reference of the standard library see
[`stdlib.md`](stdlib.md). For implementation internals (parser, JIT
codegen, AOT tree-shaking, rejected designs) see
[`internals.md`](internals.md).

## Table of contents

1. [Overview and philosophy](#1-overview-and-philosophy)
2. [Lexical structure](#2-lexical-structure)
3. [Grammar](#3-grammar)
4. [Types](#4-types)
5. [Values and identity](#5-values-and-identity)
6. [Variables and scope](#6-variables-and-scope)
7. [Expressions](#7-expressions)
8. [Strings and interpolation](#8-strings-and-interpolation)
9. [Arrays](#9-arrays)
10. [Objects](#10-objects)
    * [Tuples](#tuples)
    * [Sets](#sets)
11. [Functions and closures](#11-functions-and-closures)
12. [Control flow (`if`, `while`, `for`)](#12-control-flow)
13. [Pattern matching (`match`)](#13-pattern-matching)
14. [Optional type annotations](#14-optional-type-annotations)
15. [Error handling](#15-error-handling)
16. [Memory model (RC + cycle collector)](#16-memory-model)
17. [Built-in type methods (incl. iterator protocol)](#17-built-in-type-methods)
18. [Core built-in functions](#18-core-built-in-functions)
19. [Multimethods](#19-multimethods)
20. [Decorators](#20-decorators)
21. [Command-line interface](#21-command-line-interface)
22. [Known limitations](#22-known-limitations)
23. [Modules](#23-modules)
24. [Appendix: interpreter ↔ JIT divergence](#24-appendix-interpreter--jit-divergence)
25. [Appendix: conformance test mapping](#25-appendix-conformance-test-mapping)

---

## 1. Overview and philosophy

Culebra is a small, dynamically-typed scripting language with a
Rust-flavored syntax. Its priorities are:

* **Small surface area.** A few orthogonal features: first-class
  functions, closures, arrays, objects, pattern matching, optional
  type annotations.
* **Two backends, one AST.** A tree-walking interpreter and an LLVM
  ORC JIT share the same parser and AST, so features must be
  implementable in both.
* **Predictable memory.** Reference counting in both backends; a
  cycle collector reclaims cyclic garbage.

The language is dynamic at its core. Type annotations are optional and
enforce their invariants at runtime boundaries only; they do not turn
Culebra into a statically-typed language.

---

## 2. Lexical structure

### Source encoding

Source text is UTF-8. Identifiers use ASCII only.

### Line terminators and whitespace

Line terminators: `\n`, `\r`, `\r\n`. Whitespace: space, tab, and line
terminators.

### Comments

* Line comments: `# ...` or `// ...` until end of line.
* Block comments: `/* ... */`, not nestable.

### Identifiers

    IdentInitChar <- [a-zA-Z_]
    IdentChar     <- [a-zA-Z0-9_]
    IDENTIFIER    <- IdentInitChar IdentChar*

Identifiers are case-sensitive.

### Keywords

Reserved words that cannot be used as identifiers in declarations:

    nil  true  false  mut  debugger  return  while  for  in  if  else
    fn  match  break  continue  throw  try  catch  defer

The parser also recognizes `let` as an optional prefix in assignments.
Type annotation names (`Nil`, `Bool`, `Long`, `Float`, `String`,
`Array`, `Object`, `Function`, `Any`) are *not* reserved; they are
contextual and only recognized after `:` or `->`.

### Literals

* Integer: `NUMBER <- [0-9]+`. A `NUMBER` is a 64-bit signed integer
  (`Long`). No hex or octal literals.
* Float: `FLOAT <- [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)? / [0-9]+ [eE] [-+]? [0-9]+`.
  A literal with either a decimal point (followed by digits, not a
  bare trailing dot) or an `e`/`E` exponent is a `Float` (IEEE 754
  binary64). Examples: `1.0`, `0.5`, `-2.5` (unary minus applied to
  `2.5`), `1e-5`, `1.5e3`.
* String: `'...'` is a **raw** string — every character between the
  quotes is taken literally, including backslashes. There is no escape
  syntax, no interpolation, and apostrophes inside are not expressible.
* Interpolated string: `"...{expr}..."`. `{expr}` embeds any expression.
  Recognized escape sequences: `\n` `\r` `\t` `\\` `\"` `\{` (use `\{`
  to embed a literal `{` without starting an interpolation; raw `}` is
  fine since it's only special as the interpolation terminator). An
  unknown `\X` is preserved as the two literal characters `\` and `X`.
* Boolean: `true`, `false`.
* Nil: `nil`.

### Operators and punctuation

    ==  !=  <=  <  >=  >        # comparison
    +  -  *  /  %  **           # arithmetic (`**` exponentiation)
    @                           # matmul (user-defined via `__matmul__`)
    !                           # logical not
    &&  ||                      # logical and/or (short-circuit)
    ??                          # nil coalesce (lower precedence than ||)
    ..  ..=                     # range literals (exclusive / inclusive)
    =                           # assignment
    +=  -=  *=  /=  %=  **=  @= # compound assignment
    =>                          # match arm separator
    ->                          # return type
    ...                         # rest pattern
    |                           # or-pattern separator
    :                           # type annotation
    ( )  [ ]  { }               # grouping / container delimiters
    ,  ;                        # separators
    .                           # member access

### Statement separators

Statements are separated by `;` or by a line terminator. Empty lines
between statements are fine.

---

## 3. Grammar

The full PEG grammar is in [`include/parser.h`](../include/parser.h).
Selected rules:

    PROGRAM     <- STATEMENTS
    STATEMENT   <- DEBUGGER / RETURN / LEXICAL_SCOPE / EXPRESSION
    EXPRESSION  <- ASSIGNMENT / LOGICAL_OR
    ASSIGNMENT  <- LET MUTABLE PRIMARY (ARGUMENTS / INDEX / DOT)*
                   TYPE_ANNOTATION? '=' EXPRESSION
    PRIMARY     <- WHILE / IF / MATCH / FUNCTION / OBJECT / ARRAY
                 / NIL / BOOLEAN / FLOAT / NUMBER / IDENTIFIER
                 / STRING / INTERPOLATED_STRING / '(' EXPRESSION ')'

### Operator precedence

From lowest to highest:

1. `||`
2. `&&`
3. `==`, `!=`, `<`, `<=`, `>`, `>=`
4. Binary `+`, `-`
5. Binary `*`, `/`, `%`, `@` (matmul)
6. Unary `+`, `-`, `!` (right-associative)
7. `**` (right-associative). Binds tighter than unary minus —
   `-2**2 == -4` — but the RHS of `**` itself accepts a unary
   prefix, so `2 ** -1 == 0.5` and `2 ** -3 ** 2 == 0.00195…` both
   parse as in Python.
8. Call / index / dot (`x(...)`, `x[i]`, `x.k`) — left-associative

`=` is right-associative but appears only in `ASSIGNMENT`, not as a
general expression operator.

### Expression-oriented syntax

Most constructs are expressions that yield a value:

* `if { a } else { b }` yields the taken branch's value.
* `match x { ... }` yields the matching arm's body value (or `nil`).
* `while` always yields `nil`.
* A block `{ ... }` in statement position is a `LEXICAL_SCOPE` and
  yields `nil`; in expression position `{` starts an `OBJECT` literal.
* The last expression evaluated in a sequence is the sequence's value.

---

## 4. Types

Culebra has exactly eleven types:

| Type       | Description                                                                          |
|------------|--------------------------------------------------------------------------------------|
| `Nil`      | Single value `nil`                                                                   |
| `Bool`     | `true` or `false`                                                                    |
| `Long`     | 64-bit signed integer                                                                |
| `Float`    | IEEE 754 binary64 (double precision)                                                 |
| `String`   | Immutable heap-allocated byte string                                                 |
| `Array`    | Mutable ordered collection of values                                                 |
| `Object`   | Mutable map of hashable keys to values (String keys go through a fast shape path)    |
| `Function` | Closure (function pointer + captures)                                                |
| `Tensor`   | N-dimensional numeric tensor with BLAS-backed lazy graph (see [stdlib §5](stdlib.md#5-tensor)) |
| `Tuple`    | Immutable, hashable sequence (`(1, 2)`) — usable as an Object/Set key                |
| `Set`      | Insertion-ordered collection of unique hashable values (`{1, 2, 3}`)                 |

Arithmetic and comparison between `Long` and `Float` promote the
`Long` operand to `Float` automatically — see §7. Outside that
numeric pair there is no implicit conversion between types, and
arithmetic, comparison, or boolean operators given the wrong kind of
value raise `type error` (see §15).

`Any` is only valid in type annotations and matches any value.

**Both backends**: `Float` is fully supported in the tree-walking
interpreter and the LLVM ORC JIT. Long-only code paths keep their
existing inline integer codegen under `--jit`; Float values take a
narrow Long↔Float promotion slow path that matches interpreter
semantics bit-for-bit.

---

## 5. Values and identity

* `Nil`, `Bool`, `Long`, and `Float` are value types: they are
  compared and copied by value. `==` compares by the underlying data.
* `String`, `Array`, `Object`, and `Function` are reference types:
  variables hold a reference to a heap-allocated object. Assignment
  and passing copy the reference, not the object. Mutation through
  one variable is visible through another that refers to the same
  object.

`==` on reference types:

* `String`: compares by contents.
* `Array`, `Object`, `Function`: currently compares by reference
  identity (same pointer). Two arrays with the same elements but
  distinct allocations are not equal.

Cross-type numeric equality: `Long` and `Float` compare by numeric
value. `1 == 1.0`, `0 == 0.0`. `NaN` compares unequal to everything
including itself.

Ordering (`<`, `<=`, `>`, `>=`) is defined for:

* `Long`, `Float`, and `Bool` — numeric ordering (booleans order
  `false` < `true`; `Long` and `Float` mix by value via promotion).
* `String` — lexicographic byte ordering.
* `Nil` — `nil` compares equal to `nil` and always returns `false`
  for ordering comparisons.

Ordering values of different types (outside the numeric pair) raises
`type error`.

---

## 6. Variables and scope

### Declaration and assignment

    x = 10           # bare assignment
    let y = 20       # let binding (immutable)
    mut z = 30       # mut binding (mutable)
    let mut w = 40   # equivalent to `mut w = 40`
    let {name, age} = person           # object destructure (shorthand)
    let {name: nm, age: a} = person    # rename / value-match per entry
    let {user: {name}} = req           # nested destructure
    let [a, b, ...rest] = xs           # array destructure (rest allowed)
    let (x, y) = pair                  # tuple destructure
    let mut {x, y} = point             # destructure with mutable bindings

Assignment with a simple identifier LHS is handled as follows:

* If `let` or `mut` is present, a new binding is created in the current
  scope. It is a compile-time error to shadow a variable captured from
  an enclosing function (see "Shadow prohibition" below).
* Without `let`/`mut`, bare `x = v` searches the scope chain:
  * If `x` exists in any visible scope (including outer closures and
    the global scope), that binding is reassigned. This may update
    captured variables in outer scopes (see §11). This is the
    mechanism by which closure-based objects mutate their state.
  * Otherwise a new (immutable) binding is created in the current
    function's scope.

### Compound assignment

Seven compound-assignment operators rewrite `LHS OP= RHS` as
`LHS = LHS OP RHS`, with the side-effect that the LHS is evaluated
exactly once. They cannot be combined with `let` or `mut` (compound
assignment only updates an existing binding):

    x += 1     # x = x + 1
    x -= y     # x = x - y
    x *= 2
    x /= 3
    x %= 5
    x **= 2
    x @= M     # matrix multiply (via Tensor / __matmul__)

The LHS may be an identifier, an array element (`a[i]`), or an object
property (`o.x`). Index expressions and property names are evaluated
exactly once. Compound assignment to an undefined name is an error.

    a[next_idx()] += 1   # next_idx() is called once
    o.count += delta
    bogus += 1           # error: compound assignment on undefined name

**Tensor interaction (in-place writes).** When the LHS is a Tensor
that owns its storage and the result fits the LHS's shape, `+=`, `-=`,
`*=`, `/=`, and `**=` write the result back into the existing buffer
instead of allocating a fresh Tensor. (`%=` has no Tensor semantics
and `@=` changes the output shape, so neither is in-place.) Other references to the same
Tensor see the update — this matches NumPy semantics:

    mut W = Tensor.randn('f32', 1024, 256)
    let alias = W
    W -= grad * lr   # mutates W's buffer
    Tensor.eval(alias)  # alias.to_array() observes the new values

`W = W - grad * lr` allocates a new Tensor every step, so for SGD-style
updates over large weight tensors `-=` is materially faster (the
per-step weight allocation goes away). When the LHS is a view, an
unevaluated graph node, or has a shape that the RHS does not broadcast
into, the runtime falls back transparently to the regular new-Tensor
path — no observable behaviour change.

### The `_` sink

`_` is a non-binding sink. In any binding form it evaluates the
right-hand side (so side effects still run) but discards the value
instead of introducing a name. The same form may therefore appear
repeatedly within one scope, and the shadow-prohibition rule below
does not apply to `_`.

    let _ = side_effect()                  # value dropped
    let _ = 1; let _ = 2                   # repeated _ in one scope is fine
    for _ in 0..n { count = count + 1 }    # body runs n times, no iter var
    fn (_, _, x) { x }                     # only the third arg is bound
    try { ... } catch _ { recover() }      # error value dropped
    let [first, _, third] = [1, 2, 3]      # array slot ignored
    let [_, ..._, last] = xs               # head and middle dropped
    match v { [a, _, c] => a + c }         # pattern slot is a sink

Reading `_` is an error — the binding never happens, so a later `_`
reference raises `undefined variable '_'`. The sink rule applies
uniformly to `let`/`mut` declarations, `for ... in`, function
parameters, `try ... catch`, and pattern slots inside destructure /
`match`. (Inside an `Object` pattern `{ _, x }` is a special case:
`_` still requires the object to carry a literal `_` key, and is not
a sink there — use a positional pattern if you want to discard.)

### Shadow prohibition

Introducing a new binding is an error if a variable with the same
name exists in an enclosing function (closure-captured). The rule
applies uniformly to three kinds of binding introduction:

* `let` / `mut` declarations
* function parameters (`fn (name) { ... }`)
* `match` pattern bindings (`match v { name => ... }`)

For example:

    make_bumper = fn () {
      mut count = 0
      bump = fn () {
        mut count = 10     # error: cannot shadow outer variable 'count'
      }
      incr = fn (count) {  # error: parameter shadows captured 'count'
        count + 1
      }
      peek = fn (v) {
        match v {
          count: Long => count    # error: pattern shadows captured
        }
      }
    }

This catches a common class of bugs where a nested scope introduces
the same name as a captured variable, silently breaking the intent
to reassign the outer binding. Renaming the inner variable removes
the ambiguity.

Shadowing is *allowed* in two cases:

* **Globals and builtins** (`puts`, `min`, or any top-level binding)
  may always be shadowed. This keeps the Ruby-style feel of writing
  `mut min = arr[0]` in a local function without friction.
* **Block-scope shadowing within the same function** is allowed. A
  `{ ... }` block may introduce a new `let`/`mut` binding of a name
  already declared in the enclosing function body:

        fn () {
          a = 0
          { let a = 1; ... }   # OK: new binding scoped to the block
        }

The restriction applies only at function boundaries. The design
follows the spirit of C++'s `-Wshadow` and mirrors Scheme's
`set!`/`define` distinction: closure-captured state is mutated via
bare `x = v`, and a new local is declared with `let`/`mut`.

### Mutability

Bindings are immutable by default. Reassignment without `let` to an
immutable binding raises `immutable variable 'x'...`.

`mut` on the binding allows reassignment, but it does not make the
underlying object immutable or vice versa: you can push to an
`Array` even if the binding is immutable, since the pointer itself
is not changing.

### Scope

* Each function body introduces a new function-level scope. Parameters
  are bound there.
* A block `{ ... }` in statement position (`LEXICAL_SCOPE`) introduces
  a nested lexical scope that ends at `}`.
* `match` arms introduce a scope that covers the arm's guard and body;
  variable bindings from the pattern are visible there.

### Assignment targets

Complex assignment targets (LHS) are supported:

    arr[0] = x              # index assignment
    obj.key = v             # property assignment (creates key if absent)

### Design note: why three-tier shadow rules

Culebra treats the three shadow axes independently:

| Scope relationship | Culebra | Typical convention |
|---|---|---|
| Across a function boundary (closure capture) | **Error** | Warning or allowed |
| Within the same function (block scope) | Allowed | Warning or allowed |
| A global / builtin name | Allowed | Warning or allowed |

Most languages apply one policy to all three (either all-allowed like
Rust/Swift, or all-warned like C++ `-Wshadow` / ESLint `no-shadow`).
Culebra splits them because each axis serves a different purpose in a
Scheme-influenced, closure-as-object idiom:

* **Captured state is object state.** In the closure-based object
  pattern (see [`guide.md` §8.2](guide.md#82-the-closure-based-alternative)), an enclosing function's mutable
  binding is the object's private field. Accidentally shadowing it —
  typically by writing `mut x = ...` intending a new local — silently
  breaks the object. Making this a compile-time error is worth the
  small restriction.
* **Block scope is a computation staging area.** Inside a single
  function, a `{ ... }` block is a local calculation region.
  Rebinding a name there (`let a = transform(a)`) is a common,
  intentional pattern, not a bug. No reason to restrict it.
* **Globals form a shared vocabulary.** Builtins (`puts`, `to_string`,
  `Math`, `IO`) and top-level names are understood to be ambient. Locals
  like `mut min = arr[0]` are an ergonomic idiom, not a confusion
  risk. Requiring renames would be friction without safety gain.

The effect is that the rule catches the bug class it is designed to
catch — confusion between "new local" and "outer reassignment" —
without interfering with the two situations where shadowing is
natural. Prior-art summary:

* Python's `nonlocal` solves the same problem by declaring at function
  entry that a name refers to an outer binding. Culebra's default
  (bare `x = v` reaching outer) already expresses this at the use
  site, so the declaration is unnecessary — but the risk of
  accidental rebinding remains, and that is what shadow prohibition
  closes.
* Scheme's `set!` / `define` split avoids the same confusion by giving
  mutation and binding different names. Culebra expresses the same
  distinction through `let`/`mut` vs. bare `=`, and the shadow check
  enforces it.

---

## 7. Expressions

### Arithmetic

`+`, `-`, `*`, `/`, `%`, `**` take two numeric operands (`Long` or
`Float`); a non-numeric operand raises `type error`.

* **Both `Long` → `Long`.** Integer arithmetic; division and modulo
  truncate toward zero (C semantics, not Python's floor division).
  `7 / 2 == 3`, `-7 % 3 == -1`. Overflow wraps (no bignum).
* **Either operand `Float` → `Float`.** The `Long` operand is
  promoted to `Float` and the operation runs in IEEE 754 binary64.
  `1 + 2.0 == 3.0`, `3 / 2.0 == 1.5`.

Division or modulo by zero raises `divide by 0 error at L:C` for both
`Long / 0` and `Float / 0.0` (matches Python's `ZeroDivisionError`).

`**` (exponentiation) has a slightly richer rule:

* `Long ** non-negative Long → Long` (integer exponentiation; wraps
  on overflow). `2 ** 10 == 1024`.
* `Long ** negative Long → Float`. `2 ** -1 == 0.5`. `0 ** -1`
  raises `divide by 0 error`.
* Either operand `Float` → `Float` (via `std::pow`). `2.0 ** 0.5 ≈
  1.4142`.
* `**` is right-associative and binds tighter than unary minus. Its
  RHS accepts a unary prefix, so both `2 ** 3 ** 4 == 2 ** 81` and
  `2 ** -1 == 0.5` parse as in Python.

### Comparison

* `==`, `!=`: any two values; see §5 for reference-type semantics
  and for numeric cross-type equality (`1 == 1.0`).
* `<`, `<=`, `>`, `>=`: same-typed operands, with the exception that
  `Long` and `Float` compare by numeric value (`1 < 1.5` works). Any
  other cross-type ordering raises `type error`.

### Logical

* `!x`: requires `x` convertible to bool.
* `x && y`: evaluates `x`; if falsy returns `x`, else evaluates and
  returns `y`. Short-circuit.
* `x || y`: evaluates `x`; if truthy returns `x`, else evaluates and
  returns `y`. Short-circuit.
* `x ?? y`: returns `x` if it is not `nil`, else `y`. Short-circuit
  (RHS not evaluated when LHS is non-nil). Lower precedence than `||`.
  Chains left-associatively: `a ?? b ?? c` = `(a ?? b) ?? c`.

### Truthiness

Only `Bool`, `Long`, and `Float` are convertible to bool:

* `Bool`: itself.
* `Long`: `0` is false, all others true.
* `Float`: `0.0` (and `-0.0`) are false. Every other finite value is
  true, and `NaN` is true (Python's `bool(float('nan'))` also
  returns `True`).
* `Nil`, `String`, `Array`, `Object`, `Function`: **not convertible** —
  using one in a boolean context (e.g., `if s { ... }`) raises
  `type error`.

(This is intentionally strict. Wrap with an explicit check such as
`s != nil` or `arr.size() > 0` if needed.)

### Unary

`+x` is a no-op (must be numeric). `-x` negates a `Long` or `Float`.

### Parentheses

`(expr)` groups and does not introduce a scope.

### Method call

A method call takes the form `receiver.name(args)`. The full
resolution rules — including method/UFCS dispatch order and how
built-ins interact with user-defined properties — are specified in
§10 ("Methods and UFCS"). Operator overloading (`+`, `-`, `*`, `==`,
`@`, …) and `__str__` are also defined there.

### Evaluation order

Sub-expressions are evaluated **left-to-right in source order**.
This rule is normative on every backend; the JIT may rearrange
intermediate IR but must preserve the observable effect order.

* **Function call arguments.** `f(p1, p2, **splat, k: kv)` evaluates
  `p1`, then `p2`, then `splat`, then `kv` — exactly the order they
  appear at the call site, even when positional, `**` splat, and
  keyword arguments are interleaved.
* **Array, object, tuple, and set literals.** Element/property
  expressions evaluate in source order. For `{a: v1, b: v2}` it is
  `v1` then `v2`. For `[n; default]` (the size-prefixed form) the
  count expression evaluates before the default expression.
* **Binary operators `+`, `-`, `*`, `/`, `%`, `==`, etc.** Left
  operand first, then right operand, then the operation.
* **`||`, `&&`, `??`.** Short-circuit at the first decisive operand
  — `||` stops at the first truthy value, `&&` at the first falsy,
  `??` at the first non-`nil`. The trailing operands are not
  evaluated.
* **Assignment `lval = rhs`.** RHS evaluates first, then the LHS
  target chain (and its final subscript key for `obj[k] = ...`),
  then the store is performed. This is the opposite of Python's
  "LHS targets first" rule — culebra matches Lua / Ruby here.
* **Compound assignment `lval op= rhs`.** RHS evaluates first; the
  LHS target chain (including any subscript key) is then evaluated
  **exactly once**, the implicit read and the store both reuse the
  same evaluated chain. Subscript keys are not re-evaluated.
* **Method call.** Receiver evaluates first, then the argument list
  in source order (as above).

When in doubt, write the expression in steps using temporaries —
the spec exists so you don't need to.

---

## 8. Strings and interpolation

### Raw string literals

    'hello'
    'C:\path\to\file'   # backslashes are literal — no escape decoding
    'a\nb'              # 4 characters: a, \, n, b

Single-quoted strings are **raw**: every character is taken verbatim
between the quotes. There are no escape sequences and no interpolation.
A literal apostrophe inside a raw string is not expressible — use a
double-quoted string with `\'` or `\"` if you need quote characters.

### Interpolated strings

    "hello {name}"
    "sum = {a + b}"
    "nested: {if x > 0 { 'pos' } else { 'neg' }}"
    "line one\nline two"
    "literal brace: \{ and tab:\there"

A `"..."` string consists of plain text segments and `{expr}` segments.
Each expression is evaluated, converted to its display form (see below),
and concatenated with the surrounding text.

Escape sequences (in plain-text segments only):

| escape | byte               |
| ------ | ------------------ |
| `\n`   | newline (0x0A)     |
| `\r`   | carriage return    |
| `\t`   | tab                |
| `\\`   | backslash          |
| `\"`   | double quote       |
| `\{`   | literal `{` (does not start an interpolation) |

A `}` does not need escaping outside of an interpolation. An unknown
`\X` is preserved unchanged as two characters (`\` and `X`).

### Display conversion

When an expression inside `"..."` is not a `String`, its display form
is inserted:

* `Nil` → `nil`
* `Bool` → `true` or `false`
* `Long` → decimal
* `Float` → shortest round-trip decimal, always with a `.` or an
  exponent so the type is distinguishable from `Long`: `1.0`, `0.5`,
  `-2.5`, `1e-05`, `nan`, `inf`, `-inf`
* `Array` → `[v1, v2, ...]` with each element's `puts` form (strings
  in brackets are quoted, e.g. `['hi']`)
* `Object` → `{key: val, mut key2: val2, ...}` in insertion order
* `Function` → `[function]`

String values inside `"..."` are inserted verbatim (no quotes); this
is the one place display differs from `puts`.

Cyclic data (`a.c = a`) displays as `{...}` / `[...]` to avoid
infinite recursion.

---

## 9. Arrays

### Construction

    []                       # empty array
    [1, 2, 3]                # three elements
    [1, 2, 3](5, 0)          # 5-element array: [1, 2, 3, 0, 0]
    [](5, nil)               # 5-element array of nils
    [1](10, 0)               # [1, 0, 0, 0, 0, 0, 0, 0, 0, 0]

The optional `(count, default)` tail first fills the array to `count`
elements with `default`, then overwrites the first positions with any
literal values. Only the default form is available; omit `default` to
get `nil` fill.

### Access and mutation

    arr[i]            # index access, negative indices count from end
    arr[-1]           # last element
    arr[i] = v        # set; element must already exist (no auto-extend)
    arr.size()        # element count (Long)
    arr.push(x)       # append to end, returns nil

`arr[i]` out of range raises `index out of range at L:C`.

### Equality and ordering

Arrays compare by reference identity. There is no built-in deep
equality.

---

## 10. Objects

### Construction

    {}                                     # empty object
    {name: 'alice', age: 30}               # two properties
    {mut counter: 0, name: 'x'}            # explicit mut on a property
    {name, age}                            # shorthand — same as {name: name, age: age}

Property order in the source is irrelevant for equality or access,
but the order is preserved for display and iteration (insertion
order, matching Python 3.7+ dict and Ruby Hash).

The shorthand form `{x}` is equivalent to `{x: x}`: it reuses the
identifier as both key and value, looking up `x` in the current scope.
`mut` is allowed (`{mut n}`), which declares the property mutable while
the value still comes from the binding `n`.

### Display conventions

The default formatter (used when an Object has no `__str__`) hoists a
String `class:` property to a prefix, and omits it from the property
list. So a class-sugar instance with fields `x`, `y` displays as
`Point {mut x: 3, mut y: 4}` instead of `{class: 'Point', mut x: 3,
mut y: 4}`. Plain objects that happen to carry a `class:` key get the
same treatment; a non-String `class` field is displayed as a regular
property.

### Access and mutation

    obj.key              # read, returns nil if absent
    obj.key = v          # set (creates if absent); mut required on existing
    obj[k]               # subscript read with a non-String key (see below)
    obj.size()           # property count (Long), includes non-String keys
    obj.has(key)         # Bool — accepts String, Long, Float, Bool, Nil, Tuple
    obj.keys()           # Array of keys in insertion order (interleaved)
    obj.remove(key)      # remove the entry for `key` (any hashable key); no-op if absent

Assigning to an existing property that was declared without `mut`
raises `immutable property 'key' at L:C`.

Dot-form names (`obj.key`) are identifiers (`[A-Za-z_][A-Za-z0-9_]*`)
that go through the fast shape path. Non-identifier keys reach the
subscript path below.

### Non-String keys

In addition to String keys (`{name: 'alice'}`), Object literals accept
`Long`, `Float`, `Bool`, `Nil`, and `Tuple` keys:

    let grid = {(0, 0): 'origin', (1, 0): 'east'}
    let by_id = {1: 'one', 2: 'two', 3.14: 'pi'}
    let by_nil = {nil: 'unknown'}

Non-String keys live in a sidecar map and are read with `obj[k]`:

    grid[(0, 0)]   # 'origin'
    by_id[1]       # 'one'

Numerically-equivalent keys collide following Python's rule:
`hash(1) == hash(1.0) == hash(true)`, so `{1: 'a', 1.0: 'b'}` keeps
only one entry (the last write wins).

String and non-String keys share a single insertion-order record, so
`to_string` renders mixed-key Objects with the keys interleaved in
their actual write order — matching Python and Ruby Hash semantics.

### Subscript assignment

`obj[k] = v` writes any hashable key — `Long`, `Float`, `Bool`, `Nil`,
`Tuple`, or a runtime `String`:

    mut bag = {}
    let k = 'alpha'
    bag[k]     = 1                  # runtime String key
    bag[42]    = 'long'
    bag[(1,2)] = 'tuple'
    puts(bag[k])                    # 1
    puts(bag[(1,2)])                # 'tuple'

String keys unify with the shape-based `obj.foo` path — both forms
reach the same slot:

    mut o = {}
    o['x'] = 1
    puts(o.x)                       # 1
    o.y = 2
    puts(o['y'])                    # 2

Existing slots honor their `mut` flag, regardless of which form the
write uses; `obj[k] = v` on an immutable slot raises `ImmutableError`.
Non-String keys live in a sidecar map. Compound forms (`obj[k] += v`)
update the slot in place and require the key to already exist
(`KeyError` otherwise) and the slot to be mutable.

Caveat — JIT shape growth: each *unique* runtime `String` key used as
an Object property name allocates a `Shape` in a process-wide registry
that is never reclaimed. Programs that feed unbounded user-supplied
strings into `obj[k] = v` will accumulate shapes monotonically. For
that kind of workload use a non-String key (e.g. wrap in a `Tuple`)
to keep the data in the sidecar instead.

### Methods and UFCS

A method call `receiver.name(args)` resolves in this order:

1. If `receiver` exposes a property or built-in method named `name`,
   invoke it. For `Object` / `Array`, user-defined properties win
   over built-ins; built-ins fill in otherwise. String methods are
   the only choice for `String` receivers. When the resolved value is
   a `Function`, `this` is bound to `receiver` for the duration of
   the call.
2. Otherwise, if a free function named `name` is visible in the
   enclosing scope, call it with `receiver` as the first argument and
   the remaining arguments as-is. This is **Uniform Function Call
   Syntax (UFCS)**, matching the D / Nim convention.
3. Otherwise the property lookup returns `nil`; the subsequent call
   fails with a type error.

```culebra
o = { n: 10, add: fn (x) { x + this.n } }
puts(o.add(5))                   # 15  (method, this = o)

double = fn (x) { x * 2 }
42.double()                      # UFCS → double(42) → 84

word_count = fn (s) { s.split(' ').size() }
'hello world'.word_count()       # UFCS → 2

# Existing methods always win — a user-defined `size` is shadowed by
# the Array/Object/String built-in `size`.
size = fn (x) { 99 }
[1, 2, 3].size()                 # 3 (builtin), not 99
```

UFCS only fires when DOT is immediately followed by an argument list;
bare property access (`x.name` without `()`) never uses UFCS. `this`
is **not** bound inside UFCS invocations — the call is semantically a
free-function call with the receiver in the first positional slot.

**JIT**: UFCS is supported under `--jit`. Resolution happens at
runtime: if the receiver carries a property by that name the method
path wins, otherwise the name is looked up as a free function and
invoked with the receiver as its first argument.

### `class` sugar

Closure-based objects remain the canonical OO idiom. The `class` form
is a lightweight alternative that desugars to the same runtime shape:

    class Car {
      new(mpr)  { this.miles = 0; this.mpr = mpr }
      run(n)    { this.miles = this.miles + this.mpr * n }
      total()   { this.miles }
    }
    c = Car.new(5); c.run(3); puts(c.total())
    puts(c.class)            # 'Car' — nominal tag for match / debugging

Semantics:

* The decl binds `Car` to an `Object` with a single `new` property.
* `Car.new(...)` returns a fresh `Object` carrying `class: 'Car'` plus
  every non-`new` method as a property. `this` is bound to that object
  for the duration of the constructor body.
* Fields created via `this.x = y` inside constructors and methods are
  **mutable by default** (unlike bare `o.x = y`, which creates an
  immutable property). This matches the idiom of classes whose methods
  routinely mutate instance state.
* The `new` method is optional; without it the class accepts no
  arguments and returns an instance with only methods and `class:`.
* `this` is immutable inside the constructor body. Attempting
  `this = newObj` raises `ImmutableError` (matching Java, Crystal,
  Ruby). The constructor always returns the originally allocated
  instance — an explicit `return value` discards `value`. Identity-swap
  factories live as `static` methods (below) or as plain top-level
  functions.
* Methods prefixed with `static` live on the class object itself
  (not on instances), providing class-as-namespace for factories,
  constants-as-functions, and helpers. `Shape.create(...)` resolves
  via the usual property-access mechanism; the receiver is the
  class object, not an instance, so `this` is unavailable inside
  a static body:

      class Shape {
        new ()                  { this.kind = 'unknown' }
        area ()                 { 0 }
        static circle (r)       {
          let s = Shape.new()
          s.kind = 'circle'; s.radius = r; s
        }
        static square (side)    {
          let s = Shape.new()
          s.kind = 'square'; s.side = side; s
        }
      }
      let c = Shape.circle(4)   # static factory
      let s = Shape.square(3)
      puts(c.kind)              # 'circle'

  Static methods are not visible through instances (`c.circle(...)`
  raises `TypeError` because the instance has no `circle` property).
  This mirrors Java / C# / Kotlin / Crystal class-method semantics.
* `static NAME = EXPRESSION` declares a class-level immutable constant
  (a static field), evaluated eagerly at class declaration time and
  installed as a property of the class object alongside static methods:

      class Circle {
        new (r)         { this.radius = r }
        static PI       = 3.14
        static MAX      = 100
        area ()         { this.radius * this.radius * Circle.PI }
      }
      puts(Circle.PI)         # 3.14
      puts(Circle.MAX)        # 100

  The value expression can be arbitrary (`static SUM = [1,2,3].sum()`),
  evaluated in the enclosing scope at class declaration time. Field
  declarations require the `static` keyword — a bare `FOO = expr` in a
  class body is a syntax error. Like static methods, static fields are
  immutable (`Circle.PI = 2` raises `ImmutableError`) and not visible
  through instances.
* Both the interpreter and the JIT compile classes. Instance
  construction is a small runtime call — `new` itself is a regular
  JIT closure whose captures are the method closures plus the user's
  `new` body, and a runtime helper wires them into the fresh object.
* Well-known methods like `drop` (§16) can be written as ordinary
  class methods. Under the JIT, methods are held on a shared per-class
  meta object via prototype delegation, but the auto-drop lookup walks
  the proto chain — `class C { drop() { ... } }` fires as expected
  with `this` bound to the instance.

### Operator overloading

Any `Object` (whether produced by `class` sugar or a plain literal)
can participate in arithmetic and comparison by defining special
methods. Dispatch happens at runtime: if the left operand is an
`Object` with the matching special method, it is called with the
right operand as its sole argument; otherwise the built-in numeric
path runs. **Both the interpreter and the JIT** route through the
same special-method protocol, so `Object` arithmetic compiles.
Classes defined via `class` sugar participate identically since
their instances are plain `Object`s with methods attached.

| Operator     | Special method | Notes                                 |
|--------------|----------------|---------------------------------------|
| `a + b`      | `__add__`      |                                       |
| `a - b`      | `__sub__`      |                                       |
| `a * b`      | `__mul__`      |                                       |
| `a / b`      | `__div__`      |                                       |
| `a % b`      | `__mod__`      |                                       |
| `a ** b`     | `__pow__`      |                                       |
| `a @ b`      | `__matmul__`   | Matrix multiply (PEP 465). Same precedence as `*`. Has no built-in numeric meaning — operand without `__matmul__` raises `type error`. |
| `-a`         | `__neg__`      | 0-arg method on `a`                   |
| `a == b`     | `__eq__`       | `!=` derives by negation              |
| `a < b`      | `__lt__`       | `>=` derives by negation              |
| `a <= b`     | `__le__`       | If missing, derived as `__lt__` or `__eq__`; `>` derives by negation |

Example:

    class Vec {
      new(x, y)   { this.x = x; this.y = y }
      __add__(r)  { Vec.new(this.x + r.x, this.y + r.y) }
      __mul__(r)  { Vec.new(this.x * r, this.y * r) }    # scalar
      __eq__(r)   { this.x == r.x && this.y == r.y }
    }
    a = Vec.new(1, 2)
    b = Vec.new(3, 4)
    c = (a + b) * 2           # Vec(8, 12)

### Custom string representation (`__str__`)

Defining a 0-arg `__str__` method on an `Object` lets the value
supply its own display form for `puts` / `print`, string
interpolation (`"{x}"`), and `to_string(x)`. The method must
return a `String`; anything else is a type error. `Object`s
without `__str__` use the default formatter (`{key: value, ...}`).

    class Matrix {
      new(r, c)  { this.rows = r; this.cols = c }
      __str__()  { "Matrix {this.rows}x{this.cols}" }
    }
    m = Matrix.new(2, 3)
    puts(m)                        # Matrix 2x3
    puts("shape: {m}")             # 'shape: Matrix 2x3'
    to_string(m)                   # 'Matrix 2x3'

The dispatch is top-level only: when `__str__` is involved inside a
nested structure (`puts([m])`), the outer formatter's default
recursive walk still uses `str()` for each element, so the custom
form only appears for the operand passed directly to the display
hook. Deeper customization can be layered on once a concrete need
arises.

`__str__` bodies should **not** recursively invoke `puts(this)` or
interpolate `"{this}"` — there is no built-in recursion guard, so
that form loops until the call stack is exhausted. Produce the
final string via direct property access (`this.x`) instead.

### Auto-reflection

For commutative operators (`+`, `*`, `==`), when the LHS doesn't carry
the matching special method (e.g., it's a `Long` or `Float`) but the
RHS is an `Object` that does, the call reflects: `lhs op rhs` becomes
`rhs.__op__(lhs)`.
The `rhs`-side method receives the scalar as its argument and is
expected to handle it (typically with a `match` on the argument type):

    class Vec { ...
      __mul__(r) {
        match r {
          n: Long => Vec.new(this.x * n, this.y * n),   # scalar
          _       => Vec.new(this.x * r.x, this.y * r.y) # elementwise
        }
      }
    }
    a = 2 * Vec.new(3, 4)        # reflects → Vec.__mul__(2) → Vec(6, 8)

Non-commutative operators (`-`, `/`, `%`, `**`, `@`, `<`, `<=`) do
**not** reflect — `rhs.__op__(lhs)` would compute the wrong answer.
For those, the LHS must be an Object carrying the matching special method.

### Auto-synthesized `parameters()`

Every class instance gains a 0-arg `parameters()` method that returns a
flat `Array` of every class-instance value reachable from its own
fields. The walker descends through `Array` elements and plain `Object`
dicts (no `class:` tag), and collects each class instance it encounters
as a leaf without recursing into it. Skipped during the walk: the
`class:` tag itself, and any field whose name starts with `_` (treated
as private/cache state). Iteration is in insertion order on both
backends.

    class Value { new(x) { this.x = x } }
    class GPT {
      new() {
        this.layers = range(2).map(|_| {
          W: range(4).map(|_|
            range(4).map(|_| Value.new(0.0)).collect()
          ).collect()
        }).collect()
        this.wte = range(8).map(|_| Value.new(0.0)).collect()
      }
    }
    let model = GPT.new()
    model.parameters().size()    # 40 — every Value collected as a leaf

A class instance is a leaf: the walker stops at it rather than
recursing into its fields. Use plain Object dicts (no `class:` tag)
for intermediate grouping that should be transparent to the
enumeration, and reserve class instances for the leaves you want to
treat as the actual parameters.

A user-defined `parameters` method (any property of that name) takes
precedence over the synthesized one — useful when the field walk is
not the intended enumeration. Both the interpreter and the JIT
synthesize the method through the same walker; output ordering is
identical.

---

## Tuples

A `Tuple` is an immutable, fixed-arity sequence — the hashable
counterpart of `Array`. Literal syntax uses parentheses with at least
one comma so it doesn't collide with a parenthesized expression:

    let pair    = (3, 4)
    let single  = (1,)              # trailing comma marks a 1-tuple
    let triple  = (1, 'two', 3.14)

Element access is by index, same as `Array`:

    pair[0]                          # 3
    triple[-1]                       # 3.14

Equality is element-wise (in contrast to `Array`, which is by
reference identity):

    (1, 2) == (1, 2)                 # true
    ([1], 2) == ([1], 2)             # false  (inner Arrays compare by reference)

Tuples are hashable when their elements are, so they double as Object
and Set keys:

    let grid = {(0, 0): 'origin', (1, 0): 'east'}
    let visited = {(0, 0), (1, 1)}   # Set of Tuples

Iteration with `for x in t { ... }` walks the elements in order. Both
backends implement these semantics identically.

Built-in methods:

| Method        | Description                                        |
|---------------|----------------------------------------------------|
| `size()`      | Element count (`Long`)                             |
| `contains(x)` | `Bool` — is `x` an element?                        |
| `to_array()`  | Fresh `Array` with the same elements               |
| `iter()`      | Iterator yielding elements in index order          |

### Destructuring

A tuple binding `let (a, b) = t` unpacks the tuple into named slots.
Element count must match exactly; mismatches throw `ValueError`. Tuple
patterns also work in `match` arms and nest naturally:

    let (x, y, z) = (10, 'hi', 3.14)
    let (p, (q, r)) = (1, (2, 3))    # nested

    let kind = match (1, 2) {
      (0, 0) => 'origin',
      (a, b) => 'other',
    }

---

## Sets

A `Set` is an insertion-ordered collection of unique hashable values.
Literal syntax uses braces with at least two elements (or a trailing
comma) so it doesn't collide with the empty-Object literal `{}` or the
`{key: value}` Object shorthand:

    let s   = {1, 2, 3}
    let one = {42,}                  # trailing comma forces a 1-element Set
    let mixed = {1, 'two', 3.14, true}

Duplicate elements collapse on construction; numerically-equivalent
keys collide the same way Object keys do (`{1, 1.0, true}` keeps one
element).

Equality ignores order:

    {1, 2, 3} == {3, 2, 1}           # true

Built-in methods:

| Method         | Description                                        |
|----------------|----------------------------------------------------|
| `size()`       | Element count (`Long`)                             |
| `contains(x)`  | `Bool` — is `x` a member?                          |
| `union(b)`     | New `Set` of all elements from this and `b`        |
| `intersect(b)` | New `Set` of elements present in both              |
| `diff(b)`      | New `Set` of elements in this but not in `b`       |
| `sym_diff(b)`  | New `Set` of elements in either but not both       |
| `subset(b)`    | `Bool` — every element of self is in `b`           |
| `superset(b)`  | `Bool` — every element of `b` is in self           |
| `to_array()`   | Fresh `Array` with the members in insertion order  |
| `iter()`       | Iterator yielding members in insertion order       |
| `add(x)`       | Insert `x`; returns `true` if newly added |
| `remove(x)`    | Remove `x`; returns `true` if present |

The set-operation methods preserve the left operand's insertion order
for elements that survive. `.add(x)` and `.remove(x)` may shadow a
user-defined Object method of the same name (e.g. a `Calculator.add(1)`
class method), so both backends emit a runtime tag dispatch at the
call site: Set receivers go to the set primitive, Object receivers go
to the user property. Works on interp and JIT (and AOT).

Set operations are method-only: `union`, `intersect`, `diff`,
`sym_diff`. There are no `|` / `&` / `-` / `^` operator forms —
the `|` close delimiter of lambda parameters made the operator
ambiguous to a stateless PEG parser, so all four operations route
through methods for consistency. Operators stay reserved for math
types (Long, Float, Tensor, user numeric classes via `__add__`
etc.).

---

## 11. Functions and closures

### Function literals

    fn () { nil }
    fn (x) { x + 1 }
    fn (mut x) { x = x + 1; x }
    fn (a: Long, b: Long) -> Long { a + b }
    fn (name, greeting = 'hello') { "{greeting}, {name}" }

Functions are first-class values and the only way to define a reusable
piece of code. There is no `fn name(...)` declaration syntax; use
`name = fn (...) { ... }`.

### Lambda sugar `|x| ...`

A lighter form is available for the common case of passing a tiny
function to a higher-order call:

    add   = |x, y| x + y                 # expression body
    sq    = |x| x * x
    noop  = || 42                         # zero params
    abs_v = |x| if x < 0 { -x } else { x } # if/while/for/match/try
                                          # are expressions, so they
                                          # work as the body
    xs.map(|x| x * 2)                     # passes cleanly as a functor

The body must be a **single expression**. When multiple statements,
intermediate `let`, or side effects are needed, use `fn (...) { ... }`
instead:

    clamp = fn (v, lo, hi) {
      mut x = v
      if x < lo { x = lo }
      if x > hi { x = hi }
      x
    }

Otherwise semantically identical to `fn (...) expr`:

* Captures variables from the enclosing scope.
* Accepts the same `mut` / type-annotation / default-value parameter
  forms: `|mut x = 10| x + 1` works just like `fn (mut x = 10) x + 1`.
* Self-reference via `self` works the same as in `fn`.
* No return-type annotation slot (keep lambdas short; use `fn` for
  functions whose signature deserves the annotation).

The `|` delimiter is not ambiguous with pattern alternation (which
only appears inside `match` arms) or logical `||` (which never starts
an expression).

### Parameters

* `mut` on a parameter makes that parameter binding mutable inside
  the body. Without `mut`, reassigning the parameter raises
  `immutable variable`.
* Optional type annotations are enforced on entry (§14).
* A parameter may have a default value via `name = expr`. When the
  caller omits the argument, the default is evaluated on each call in
  the function's definition environment, extended with any earlier
  parameter bindings — so `fn (a, b = a + 1)` works. Default
  parameters must follow all required parameters.

### Keyword arguments and `**` splat

A call site may pass arguments by name (`name: value`) and/or expand
an Object as kwargs (`**obj`). Forms:

    let f = fn (x, y = 10, z = 100) { x + y + z }

    f(1, 2, 3)                  # positional
    f(1, z: 5)                  # kwargs (y defaults)
    f(z: 3, y: 2, x: 1)         # any order

    let opts = {y: 7, z: 8}
    f(1, **opts)                # splat an Object as kwargs
    f(1, **opts, z: 100)        # explicit kwarg overrides splat
    f(1, **{y: 2}, **{y: 5})    # multiple splats: later wins

Catch-all `**rest` collects every kwarg that isn't claimed by a
declared parameter into an Object, bound to the rest parameter's
name. It must be the last parameter.

    let route = fn (path, **opts) {
      route_internal(path, opts.method, opts.headers, opts.body)
    }

    route('/users', method: 'GET')             # opts = {method: 'GET'}
    route('/users', method: 'POST', body: '…') # opts = {method, body}

Keyword-only parameters: a bare `*` in the parameter list marks the
boundary; parameters after it can only be passed by name.

    let g = fn (x, *, y, z = 10) { x + y + z }

    g(1, y: 2)            # 13
    g(1, y: 2, z: 3)      # 6
    g(1, 2)               # TypeError: takes 1 positional argument but 2 given
    g(1)                  # ArityError: missing required argument 'y'

Rules:

* Positional arguments must come before any kwarg or splat. Mixing
  the other way (`f(x: 1, 2)`) is a `SyntaxError`.
* The same name may not appear twice as an explicit kwarg.
* A name cannot appear both positionally and as a kwarg.
* Unknown kwarg names are a `TypeError` — there is no `**rest`
  catch-all on the definition side in this milestone.
* A `**` splat operand must be an `Object` with `String` keys only.
* Defaults are re-evaluated on every call (Ruby/Scala flavor — no
  Python-style "evaluated at def time" trap).

JIT support: kwargs and `**` splat work on both backends for the
common patterns:

* Direct calls to user functions: `f(x, y: 2)` where `f` is in scope.
* Captured / passed-around closures: `let g = make_fn(...); g(y: 2)`.
* UFCS: `x.free_fn(y: 2)` becomes `free_fn(x, y: 2)`.
* Object methods: `obj.method(y: 2)`.
* Built-in `JSON.{stringify, parse}`: kwargs for `indent`,
  `sort_keys`, `lines`, `number_mode`.
* Dynamic `**variable` splat against user functions: resolved at
  runtime through closure-attached parameter metadata.

Middle-gap defaults (e.g. `f(1, z: 3)` against `(x, y=10, z=20)`)
work in JIT via a `TAG_UNFILLED` sentinel passed through the slab;
the callee prologue picks up the inline default expression.

JIT limitations:

* Other namespace built-ins (`Math`, `IO`, `Random`, `Sys`) take only
  positional arguments — kwargs against them surface a clean
  `SyntaxError` at compile time.
* Dynamic `**variable` splat against built-in dispatchers (e.g.
  `JSON.stringify(v, **opts)`) works on both backends via a
  per-built-in kwarg adapter (`JSON.stringify` and `JSON.parse`
  ship adapters; other namespaces are positional-only and still
  reject kwargs at compile time).
* Compile-time JIT errors (e.g. `positional argument follows keyword
  argument`) are detected during IR emission and bypass `try/catch`;
  interp throws the same errors at runtime where they can be caught.
  This is a structural difference between the two execution models.

### Return

* The body is a block. The last expression of the block is the
  function's return value.
* `return expr` returns early; `return` (no expression) returns `nil`.
* If a return type is declared (`-> T`), the returned value is checked
  against `T` before control leaves the function, both for natural
  fallthrough and explicit `return`.

### Recursion: `self`

Within a function body, `self` refers to the function value currently
being executed. Recursion without giving the function a name:

    fib = fn (x) { if x < 2 { x } else { self(x - 1) + self(x - 2) } }

### Methods: `this`

`this` is bound for the duration of a method call. Outside a method
call, `this` is not in scope; accessing it raises
`undefined variable 'this'...`.

### Closures

Function literals capture their lexical environment by reference.

* Variables read inside a nested function look up the scope chain
  until found.
* `x = v` inside a nested function reassigns an outer binding if `x`
  exists in an outer scope; otherwise creates a new local.
* Captured mutable variables (e.g., via `mut x` in an outer scope) are
  shared: changes are visible to every closure that captured them and
  to the outer scope itself. This mirrors JavaScript/Ruby, not Python.

Example:

    make_counter = fn () {
      mut n = 0
      fn () { n = n + 1; n }
    }
    c1 = make_counter()
    c2 = make_counter()
    puts(c1())   # 1
    puts(c1())   # 2
    puts(c2())   # 1, independent

In the JIT, captured mutable variables are allocated in heap **cells**
so that multiple closures can share the same slot. See §16.

---

## 12. Control flow

### `if`

    if cond { then_block }
    if cond { then_block } else { else_block }
    if c1 { b1 } else if c2 { b2 } else { b3 }

`cond` must be truthy-convertible (`Bool` or `Long`). `if` is an
expression; its value is the taken branch's last expression, or `nil`
if no branch is taken (no `else` and the `if` was false).

### `while`

    while cond { body }

`while` is a statement; its value is `nil`. `break` and `continue`
work inside the loop body.

### `for` ... `in`

    for var in iterable { body }

Iterates by calling `iterable.iter()` once, then repeatedly invoking
`next()` on the returned iterator until `next()` returns an object
whose `done` is truthy. Each step's `value` is bound to `var` in a
fresh scope per iteration.

```culebra
for x in [1, 2, 3] { puts(x) }

for k in {b: 2, a: 1} { puts(k) }   # keys, ascending

for i in 0..10 { puts(i) }          # exclusive (0..9)
for i in 0..=10 { puts(i) }         # inclusive (0..10)
```

Range literals `a..b` (exclusive) and `a..=b` (inclusive) return the
same lazy integer iterator as `range` — no up-front allocation.
Endpoints must be `Long`.

The iterator protocol (see §17.5) requires the target to be either an
`Object` (or subtype `Array`) with an `iter` method, or an object
already playing the iterator role with a `next` method. Passing any
other type raises `type error`.

`for` is a statement; its value is `nil`. Shadow rules apply to
`var`: if it would shadow a closure-captured name from an enclosing
function, the script is rejected (see §6).

**JIT**: `for` / `break` / `continue` compile under `--jit` for
direct iteration over `Array`, `Object` (yields keys in ascending
order), and `String` (UTF-8 scalar walk). Objects that carry their
own `iter` property (user-defined iterators, `range`,
`String.code_points()` / `.graphemes()`, iterator method chains) are
driven through the iterator protocol at runtime — same semantics as
the interpreter, with a native-loop fast path preserved for the
Array/String/keys cases.

### `break` and `continue`

    break           # exit the innermost enclosing loop
    continue        # skip to the next iteration of the innermost loop

Valid only inside `for` or `while`. Using them outside a loop
propagates up as a runtime error. `break` / `continue` do not carry a
value (the loop's value remains `nil`).

### `return`

Valid only inside a function body. Exits the enclosing function with
the given value (or `nil`). Using `return` at top level is an error.

### `debugger`

When the program is run under `--debug`, encountering `debugger`
pauses execution and drops into a simple REPL debugger showing the
current source line. Without `--debug`, `debugger` is a no-op.

---

## 13. Pattern matching

    match subject {
      pattern1 (if guard1)? => body1,
      pattern2 (if guard2)? => body2,
      ...
    }

`match` is an expression. Arms are tried top-to-bottom; the first arm
whose pattern succeeds (and whose guard evaluates truthy) runs its
body and the result is the value of the `match`. If no arm matches,
the value is `nil`.

### Patterns

| Form              | Matches                                  |
|-------------------|------------------------------------------|
| `0`, `'x'`, `nil`, `true` | Literal equality                  |
| `name`            | Any value, binds it to `name`            |
| `_`               | Any value, no binding                    |
| `name: Type`      | Value whose type is `Type`; binds        |
| `p1 \| p2 \| p3`  | Any of the sub-patterns matches          |
| `[p1, p2, ...]`   | `Array` of exactly the same length       |
| `[p1, ...rest]`   | `Array` of ≥ `n−1` elements; `rest` is a fresh `Array` of the remainder |
| `[a, ...m, z]`    | Rest can be in the middle; pre/post positions match fixed elements |
| `{k1, k2}`        | `Object` containing at least those keys; binds each `ki` to `obj.ki` (shorthand) |
| `{k1: p1, k2: p2}` | `Object` whose `k1` matches `p1` and `k2` matches `p2`. Nests freely (`{user: {name}}`). Mixes with shorthand. |
| `{}`              | Any `Object` (keys ignored)              |
| `(p1, p2, ...)`   | `Tuple` of exactly the same arity; element-wise sub-patterns |

### Semantics

* Patterns are tried left-to-right, and sub-patterns are evaluated
  depth-first.
* A binding `name` introduced by the pattern is visible in the guard
  and the body.
* `|` (or) sub-patterns with inconsistent bindings have undefined
  bindings in the non-matching branches; put only literals (or
  identical binding structures) inside `|` for predictable behavior.
* `Array` patterns require an exact length unless a `...rest` element
  is present; objects do not require exact key sets — extra keys are
  ignored.
* The rest array is a newly-allocated `Array` with elements from the
  subject shallow-copied; mutating it does not affect the subject,
  but mutating a reference-typed element mutates the shared object.
* The `match` subject is evaluated exactly once.

### Exhaustiveness

No static exhaustiveness check is performed. If no arm matches, the
`match` yields `nil`. Add `_ => nil` or a typed fallback explicitly
if that bothers you.

---

## 14. Optional type annotations

Culebra is dynamically typed; type annotations are optional and
enforce their invariant at three specific runtime points:

1. On entry to a function, each parameter's value is checked against
   its annotation.
2. On function return (either fallthrough or explicit `return`), the
   result is checked against the declared return type.
3. On assignment with an annotated target, the RHS value is checked
   against the annotation.

### Syntax

    let x: Long = 10
    let mut s: String = 'hi'
    fn (a: Long, b: Long) -> Long { a + b }

### Recognized type names

    Nil  Bool  Long  Float  String  Array  Object  Function  Any

`Any` always matches. Unknown type names fail the check and raise
`type error`. Class names declared with `class C { ... }` are also
valid annotations and accept any instance of that class.

### Union types

An annotation may list alternatives separated by `|`. The runtime
check accepts the value when it matches **any** alternative.

    fn show(x: Long | Float) -> String { to_string(x) }
    show(1)      # → '1'
    show(2.5)    # → '2.5'
    show("hi")   # !! type error

    fn lookup(k: String) -> Long | Nil {
      if k == "answer" { 42 } else { nil }
    }

Union annotations are valid wherever a single type annotation is —
parameters, return types, and `let` / `let mut` declarations:

    let id: Long | String = "u-42"
    let mut count: Long | Nil = nil
    count = 0           # OK: re-check is *not* run on reassignment

Whitespace around `|` is tolerated (`Long|Float`, `Long | Float`).
Class names compose with primitives (`Square | Circle`,
`String | Nil`). Single-alternative annotations remain unchanged in
behavior — `Long` and `Long|` are not equivalent (the latter is a
parse error).

`Object` inside a Union is a **catch-all for class instances**
(any value carrying a `class:` tag), not for every value: primitives
fall through it. So `Long | Object` accepts a Long *and* any class
instance, but a String still fails. Use `Any` if you want to accept
everything.

If any alternative is an **undeclared class name**, the runtime
treats it the same as a class annotation that no instance can
satisfy — the alt simply never matches, so it's effectively dead.
Other alternatives in the Union still match normally.

Multimethod dispatch ([§19](#19-multimethods)) understands Union
parameter annotations by scoring each alternative and taking the
best match — `fn area(s: Square | Circle)` dispatches on either
exact class. A bare concrete type outranks any Union that contains
it: defining both `fn pick(x: Long)` and `fn pick(x: Long | Float)`
routes a Long arg to the concrete `pick`, while a Float still goes
through the Union version.

### Generic types

A type name may carry type parameters in angle brackets:
`Array<Long>`, `Array<Array<Long>>`, `Array<Long | Float>`. Nesting
is supported, and Generic args may themselves be Unions. The only
built-in container type today is `Array`; other Generic outer names
(`Box<T>`, `Pair<K, V>`, ...) come from user-declared classes
(see "Generic class declarations" below).

    fn first(xs: Array<Long>) -> Long { xs[0] }
    fn lookup(k: String) -> Array<Long> | Nil { ... }

Like Rust and Swift's monomorphized generics, **element-level
runtime checks are no-ops**: only the outer type is checked at
the boundary. The args exist for documentation and for multimethod
dispatch tie-breaks.

    fn first(xs: Array<Long>) { xs[0] }
    first([1, "two", 3])   # OK — element type is not enforced

Whitespace inside `<>` is tolerated and canonicalized:
`Array < Long >`, `Array<  Long  >` and `Array<Long>` all surface
as `"Array<Long>"` in `fn.params[i].type`.

Multimethod dispatch tie-break: a Generic param is more specific
than its bare outer-only form. With both
`fn show(xs: Array)` and `fn show(xs: Array<Long>)` defined, an
`Array` arg routes to the Generic version.

#### Where class declarations may appear

Classes are top-level constructs. `class C { ... }` is rejected
when it appears directly inside another class's body — declarations
must live at top level, or inside a fn / lambda / lexical-scope
(`{ ... }`) block that re-opens a fresh scope. The restriction
keeps the mental model flat; namespacing is handled by `import`
and `export`, and inner helpers by free functions / UFCS.

    class Bad {
      m() {
        class Inner { ... }       # !! SyntaxError
      }
    }

    # OK — fn body is a scope boundary
    fn make_inner() {
      class Inner { ... }
      Inner.new()
    }

#### Generic class declarations

A class can declare type parameters:

    class Box<T> {
      new (v: T) { this.v = v }
    }

    class Pair<K, V> {
      new (k, v) { this.k = k; this.v = v }
    }

An unbounded type parameter is documentation — it makes method
signatures readable but the runtime sees it as `Any`. A *bounded*
parameter (`<T: Comparable>`) is enforced; see "Bound constraints"
below. The class is bound under the outer name (`Box`, `Pair`), and
instances carry the outer class tag:

    let b = Box.new(42)
    b.class                 # → 'Box'

Annotations referring to a Generic class use the same `<...>`
syntax as built-in Generic types and behave the same way (outer
match):

    fn unbox(b: Box<Long>) -> Long { b.v }

#### Bound constraints

A type parameter may carry a bound — a trait the bound type must
conform to: `<T: Comparable>`. The bound applies on both free
functions and classes, and is enforced at the call boundary: a value
that does not conform to the bound is not a valid argument for that
parameter. Inside the body, the bound's default methods are available
on object arguments (`a.gt(b)` for a `Comparable` class).

    class Money {
      new(amount) { this.amount = amount }
      cmp(other) { this.amount - other.amount }   # conforms to Comparable
    }
    fn pick_max<T: Comparable>(a: T, b: T) {
      if a.gt(b) { a } else { b }                 # gt is a Comparable default
    }
    pick_max(Money.new(10), Money.new(25)).amount   # → 25
    pick_max([1], [2])                              # !! no matching method

Bounds are *lowered to the bound trait* at declaration time, so they
reuse the ordinary trait-conformance machinery. Three consequences:

* **Dispatch specificity** sits at the trait level: a concrete
  overload beats a bounded one, and a bounded one beats an unbounded
  `<T>` catch-all.

      fn rank(x: Long) { "concrete" }
      fn rank<T: Comparable>(x: T) { "bounded" }
      fn rank<T>(x: T) { "unbounded" }
      rank(5)     # → "concrete"   (exact type)
      rank(1.5)   # → "bounded"    (Float conforms to Comparable)
      rank([1])   # → "unbounded"  (Array conforms to neither)

* **Lenient unification**: a type parameter that appears in several
  positions (`a: T, b: T`) checks the bound at *each* position
  independently — repeated `T` is not forced to a single concrete
  type. A function taking `(a: T, b: T)` accepts `(1, 2.0)` (both
  `Comparable`).

* The bound enforces conformance, but the **primitive method-surface
  limitation** still applies: `a.gt(b)` only dispatches when `a` is an
  object. For primitive arguments use the native `<` / `>` operators
  in the body instead.

An unbounded `<T>` accepts any argument (it lowers to `Any`); it
exists to name a parameter and to lose to more specific overloads.

#### Known limitations

* Two overloads sharing an outer Generic name (`fn show(xs:
  Array<Long>)` vs `fn show(xs: Array<String>)`) both score
  4 in dispatch — calls with an Array arg are reported as
  ambiguous. Resolves naturally once element-level checks ship
  (Phase 2b).
* Type parameters in introspection: `fn.params[i].type` reports
  the declared form (`T`, `Array<T>`) for readability, but the
  runtime type check only sees the outer / `Any`. Don't rely on
  introspected `T` as a real type for further dispatch decisions.

#### Planned (Phase 2b)

* Expression-position type args (`Box<Long>.new(42)`) — the parser
  currently reserves `<...>` for type-annotation contexts only.
* Opt-in element runtime check.
* Composite bounds (`<T: A + B>`), `where` clauses, and trait
  inheritance (`trait Ord: Eq`) — single bounds ship today.
* Class declarations inside another class's body (currently a
  SyntaxError, see "Where class declarations may appear" above).

### Where annotations are *not* checked

* Arithmetic / boolean operators check their own operand types;
  annotations on local intermediates do not add extra coverage.
* Object property values have no annotation slot.
* Array element types are not tracked.
* `mut x: T` on a local does not re-check on later reassignment —
  the check happens once at the annotated declaration.

Annotations are primarily a **documentation and boundary check**
feature, not a type system.

### Traits and protocols

A `trait` declares a named contract — the set of methods a value
must provide to be treated under that name in dispatch and Generic
Bound positions.

    trait Greeter {
      hello() -> String
    }

**Structural conformance**: any class whose methods cover the trait's
required ones (name + arity) automatically satisfies the trait —
no `impl Foo for Bar` block needed. The model mirrors Go interfaces
and Python's `__str__`-style magic methods.

    class Bob {
      new(name) { this.name = name }
      hello() { "hi, {this.name}" }
    }

    fn greet(x: Greeter) { IO.puts(x.hello()) }
    greet(Bob.new("Alice"))                # → "hi, Alice"

A class missing required methods is rejected at the dispatch
boundary (DispatchError).

#### Default methods

A trait method may carry a body. Conforming classes inherit it for
free; defining the same name on the class overrides:

    trait Counter {
      current() -> Long
      next() -> Long { this.current() + 1 }     # default
    }

    class Zero {
      new() {}
      current() { 0 }
    }
    Zero.new().next()                # → 1 (default)

    class Five {
      new() {}
      current() { 5 }
      next() { 99 }                  # override
    }
    Five.new().next()                # → 99

#### Built-in traits

The runtime ships seven foundational traits as a preamble — they
are visible without `import`:

| Trait | Required | Defaults |
|---|---|---|
| `Stringer` | `to_s() -> String` | — |
| `Eq` | `eq(other) -> Bool` | `neq` |
| `Comparable` | `cmp(other) -> Long` | `lt`, `le`, `gt`, `ge` |
| `StringLike` | `to_string_view() -> StringView` | — |
| `Hashable` | `hash() -> Long` | — |
| `Iterator` | `has_next() -> Bool`, `next() -> Any` | — |
| `Iterable` | `iter() -> Iterator` | — |

A class with only `cmp` automatically gets the six-way comparison
suite; a class with `eq` gets `neq`; a class with `to_s` is
displayable wherever a `Stringer` is expected. `StringLike` accepts
any byte-readable string value at API boundaries — `String` and
`StringView` both conform out of the box. `Hashable` is the
contract `Object` / `Set` keys check at insertion: a user class
becomes a valid key by defining `hash()` (returning `Long`) and
`eq(other)`.

`Iterator` + `Iterable` formalize the for-in protocol (Kotlin /
Java style). A class that exposes `iter() -> Iterator`,
`has_next() -> Bool`, and `next() -> Any` is iterable and works
with every pipeline method (`map` / `filter` / `take` / `collect` /
...). `has_next()` is required to be idempotent on repeat calls —
runtime wrappers cache one lookahead so a `has_next()` peek doesn't
consume the next value. `next()` may yield any value including
`nil` (nil terminator designs lose this); pairing with `has_next()`
is the contract for end-of-stream detection.

Built-in primitives also conform via a hard-coded table — no
class wrapper required:

| Primitive | Stringer | Eq | Comparable | StringLike | Hashable | Iterable |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| Nil / Bool | ✓ | ✓ (Bool) | ✓ (Bool only) | — | ✓ | — |
| Long / Float | ✓ | ✓ | ✓ | — | ✓ | — |
| String / StringView | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Tuple | ✓ | ✓ | — | — | ✓ | ✓ |
| Array / Set | ✓ | ✓ | — | — | — | ✓ |
| Object (bare literal) | ✓ | ✓ | — | — | — | ✓ |
| Tensor | ✓ | ✓ | — | — | — | — |
| Function | ✓ | — | — | — | — | — |

So `fn show(x: Stringer) { to_string(x) }` accepts `show(42)` and
`show([1, 2, 3])` without requiring a class wrapper. Note that
trait method calls (`x.to_s()`) only resolve on class instances —
primitives have no method-dispatch surface, so the trait reaches
them via `fn` boundaries (`fn show(x: Stringer)`), not via the
`.method()` syntax. The same pattern applies to `Hashable`:
primitives go through the `hash(v)` global builtin (companion to
`to_string`); class instances also accept the direct `x.hash()`
call. For Hashable user classes used as Object / Set keys the
matching `eq(other)` method is required so the container's
equality check stays consistent with the hash.

#### Generic Bound

A type parameter may carry a single trait as its bound, declared
inline (Rust style):

    fn min<T: Comparable>(a: T, b: T) -> T {
      if a.lt(b) { a } else { b }
    }

The bound is **enforced** at the call boundary (no compile-time
check; culebra's type checks are runtime) — see "Bound constraints"
under Generic types for the full semantics (lowering, dispatch
ordering, lenient unification). Multiple bounds (`<T: A + B>`) and
`where` clauses are not yet supported.

#### Dispatch tie-break

A trait param scores between Object and concrete in multifn
dispatch: a method with `x: Pri` wins over a method with
`x: Stringer` when the arg is a Pri instance; `x: Stringer` is
chosen when only the trait path matches.

#### Limitations (Phase 4 MVP)

* `impl Foo for Bar` block is unsupported — conformance is purely
  structural. (Phase 4+: revisit if explicit conformance is wanted.)
* Trait inheritance (`trait Ord: Eq`) is not yet supported.
* Bound composition (`<T: A + B>`) and `where` clauses — Phase 4+.
  Single bounds (`<T: Comparable>`) are enforced today.
* **Operator overloads bypass trait defaults**: `<` / `==` etc.
  invoke `__lt__` / `__eq__` directly on the class; Comparable's
  default `lt` / `le` etc. only fire when called as `x.lt(y)`. A
  class that defines `cmp` and uses `lo < hi` must also define
  `__lt__` (the special-method path), or call `.lt()` explicitly.
* **Same-name defaults across traits are non-deterministic**: if
  two registered traits both provide a `to_s` default and an
  instance conforms to both, the dispatch picks the one the
  internal hash map iterates first. Avoid declaring same-named
  defaults across overlapping traits.
* **Self-recursive trait defaults are user responsibility**:
  `trait X { foo() { this.foo() } }` will stack-overflow if a
  conforming class doesn't override `foo`. No depth guard is
  installed yet.
* **JIT trait-default closure refcount**: each declared default
  method retains +1 reference for the lifetime of the JIT module.
  Single-shot JIT or AOT (one main → exit) is unaffected; long-
  running JIT hosts re-compiling many sessions accumulate slow
  growth. Phase 4+ refcount cleanup pending.
* **multifn dispatch overhead**: every call walks the full
  trait_registry × args matrix once per warmup, even for functions
  with no trait-typed parameters. Cache amortizes repeats, but
  ~3 × n_args probes per call show up in hot loops. Phase 4+
  optimization can skip the warmup when no method references a
  trait.
* **JIT trait-default fallback is not IC-cached**: property
  accesses that resolve through a trait default re-walk the
  default table on each call. Hot loops on `instance.default()`
  pay this overhead repeatedly. Phase 4+ adds an inline-cache
  shape for the trait-default slot.
* **AOT trait re-declaration**: each method registers via per-
  method runtime upserts, so re-declaring `trait Eq { eq(other) }`
  after the preamble's `Eq { eq, neq }` leaves `neq` in place.
  REPL / hot-reload contexts should declare traits fully or restart.
* Traits, like classes, may only be declared at top level.

---

## 15. Error handling

### `throw` / `try` / `catch`

Culebra supports user-raised exceptions via `throw` and catching via
`try`/`catch`. The thrown value may be **any** Culebra value.

    throw 'something bad'
    throw {kind: 'io', msg: 'file not found', path: p}

    try { risky() }
    catch e { puts("error: {e}") }

Semantics:

* `throw expr` evaluates `expr` and propagates it as a Culebra value
  until caught by the nearest enclosing `try`. It crosses function
  boundaries and propagates through any number of frames.
* `try { A } catch name { B }` evaluates `A` in a fresh scope; on
  `throw` within `A`, `B` is evaluated in another fresh scope with
  `name` bound to the thrown value. The whole `try`/`catch` is an
  expression yielding the value of whichever block ran last.
* An uncaught `throw` at the top level is reported with `uncaught: ...`
  and the program exits with a non-zero status.
* `throw` is distinct from `return`: a function's early `return`
  unwinds only that function; a user `throw` travels past enclosing
  functions.

### `defer`

A `defer { BLOCK }` statement registers `BLOCK` to run when the
**enclosing lexical scope** exits, in LIFO order. Defers run on
*every* exit path — normal completion, early `return`, or `throw`.

    fn work() {
      {
        f = open_tmp()
        defer { f.close() }     # fires when this inner block exits
        process(f)
      }                         # ← f.close() runs here
      more_work()
    }

Block scope (not function scope) means:

* `defer` in a loop body fires on every iteration — matches the
  programmer's intent of per-iteration cleanup.
* `defer` in a conditional block fires only if that block actually ran.
* To tie cleanup to the function, place `defer` at the top of the
  function body (the function body itself is a block).
* Top-level `defer` runs when the program exits.

A `return` inside a defer body exits only the defer closure, not the
enclosing function. `throw` inside a defer body aborts that defer and
propagates as a regular exception.

### Scope guard pattern

When cleanup must be registered from code that cannot place its own
`defer` (e.g., a callback that wants cleanup at the caller's scope),
a small helper object is enough — the language does not ship a
built-in for it:

    make_guard = fn () {
      mut fns = []
      {
        add: fn (f) { fns.push(f) },
        run: fn () {
          mut i = fns.size() - 1
          while i >= 0 { fns[i](); i = i - 1 }
          fns = []
        }
      }
    }

    process = fn (items) {
      g = make_guard()
      defer { g.run() }
      mut f = nil
      items.for_each(fn (item) {
        if f == nil {
          f = open('out')
          g.add(fn () { f.close() })
        }
        write(f, item)
      })
    }

### Runtime errors

Internal runtime errors (type mismatches, division by zero, missing
properties, ...) surface to user `try`/`catch` as **structured Error
Objects** with four properties:

| Property | Type | Description |
|---|---|---|
| `kind` | `String` | Error category, e.g. `'TypeError'`. See list below. |
| `message` | `String` | Human-readable description (includes `at L:C.`). |
| `line` | `Long` | 1-based source line of the offending AST node, `0` if unknown. |
| `col` | `Long` | 1-based source column, `0` if unknown. |

User code can branch on `e.kind`:

    try { let x = arr[100] }
    catch e {
      if e.kind == 'IndexError' { puts("out of range at line {e.line}") }
      else { throw e }
    }

Standard `kind` values, with the exact trigger condition and
catchability for each. Every kind populates `e.kind`, `e.message`,
`e.line`, and `e.col` identically on the interpreter, the JIT, and
AOT builds (unless noted).

| Kind | Trigger | Catchable |
|---|---|---|
| `TypeError` | Arithmetic / comparison on incompatible operand types; calling a non-callable; failing `: T` annotation; `to_long`/`to_float` on non-coercible value; `__str__` returning non-String; `*` splat of non-Array / `**` splat of non-Object; built-in arg-type check failure; mixed positional + keyword targeting the same parameter; duplicate keyword. | yes |
| `ZeroDivisionError` | Integer `/`, `%`, `**` with negative exponent collapsing to division; float `/` or `%` with RHS == 0. | yes |
| `NameError` | Read of undefined identifier; compound assignment (`x += rhs`) on undefined `x`; REPL global lookup miss. | yes |
| `ImmutableError` | Assignment to a `let` (non-`mut`) binding; assignment to an immutable Object property or `Dict` entry; rebinding `this` inside a constructor body. | yes |
| `KeyError` | Dict subscript on absent key; Object subscript on absent key. | yes |
| `IndexError` | Array / String / Tensor index out of range; Tensor slice out of bounds; Tensor reduction axis out of range. | yes |
| `ValueError` | Destructure pattern mismatch (`[a, b] = ...` shape mismatch); Tensor shape / dtype mismatch; `[].min()` or other empty-collection reductions; numeric conversion of malformed string; JSON parse failure. | yes |
| `AttributeError` | Compound assignment (`o.x += ...`) on a missing property. | yes |
| `ArityError` | Call missing a required argument; too few or too many positional args; class instantiation arity mismatch. | yes |
| `DispatchError` | Multimethod call with no matching method or with ambiguous specificity tie (§19). | yes |
| `AssertionError` | Matcher failure (`assert_true` / `assert_eq` / etc.) or user `throw {kind: "AssertionError", ...}`. Message names both operands for comparison matchers. | yes |
| `SyntaxError` | Structural errors raised during AST lowering: `**rest` not last param, duplicate `*` separator, non-default param after default, `compound let`, `break` / `continue` outside loop. Surfaces at function decl evaluation, before that function runs. | yes |
| `ShadowError` | Static shadow analyzer (§6) detected a binding that shadows a captured outer name. Fires before any user `try` block can observe it. | **no** (pre-eval analyzer) |
| `IOError` | `read_file` / `write_file` / stdlib file ops failing; `Tensor.load` failure. | yes |
| `DropContractError` | `drop` / `iter` / `next` property bound to a non-Function or non-zero-arity function. | yes |
| `RuntimeError` | Fallback when interp catches an unconverted `std::runtime_error` from a not-yet-migrated throw site; JIT REPL `repl_set` outside a session. `e.line == 0` and `e.col == 0` are possible in this case only. | yes |

Uncaught errors print as `Kind: message` and exit with non-zero
status. User-thrown values via `throw expr` print as `uncaught: {value}`.

### Compile-time errors

`ShadowError` is detected when the program is loaded, before any
`try` block runs — so user code cannot catch it. It aborts the
program with the same `Kind: message` format. The check runs in both
backends; see "Shadow prohibition" in §6 for the rule.

`NameError` (undefined variable), `ImmutableError`, missing/unknown
kwargs and similar errors are deliberately raised at *runtime* on
both backends so `try { ... } catch e { ... }` can observe them.

### Assertion API

There is no `assert` keyword or builtin. For tests, use the matcher
family (`assert_true` / `assert_eq` / etc., see §18 and `docs/stdlib.md`).
For production invariants, throw an Object:

```culebra
if (!cond) {
  throw {kind: "AssertionError", message: "..."}
}
```

This mirrors Go's `if x == nil { return errors.New(...) }` and Ruby's
`raise` style — assertion control flow is explicit rather than a magic
keyword.

### JIT support

The JIT backend supports `throw` / `try` / `catch` / `defer` with
semantics matching the tree interpreter for the common cases:

* `throw` / `try` / `catch` propagate across function boundaries via
  LLVM `invoke` / `landingpad` and the Itanium C++ personality.
* `defer` inside a lexical-scope block (`{ defer { ... } ... }`)
  registers a closure on a global defer stack; a scope cleanup
  landingpad runs it on fall-through, `return`, and throw-unwind
  paths.
* **Limitation**: `defer` written directly in a function body or at
  the top level (not inside a nested `{ }`) only runs on *normal*
  exit or `return`. An uncaught throw that escapes the function or
  program skips it. For throw-path cleanup, wrap the defer in a
  block (`fn () { { defer { ... } ... } }`).

Internal runtime errors (`TypeError`, `ZeroDivisionError`, etc.) flow
through user `try/catch` as structured Error Objects on both backends
— see "Runtime errors" above.

---

## 16. Memory model

### Reference counting

All heap-allocated types (`Array`, `Object`, `Function`, and in the
JIT internal `Cell`s and `Closure`s) are reference-counted.

* An object's refcount starts at 1 when created.
* Binding to a new variable, pushing into an array, or storing into
  an object property increments the refcount.
* Overwriting a variable, property, or array slot decrements the
  overwritten value's refcount.
* When the refcount hits zero, the object is freed and each of its
  held references is decremented.

### Cycle collection

Pure reference counting cannot reclaim cycles (`a.c = a`). Both
backends register every refcounted heap object with an auxiliary
cycle collector that runs a Python-style mark-and-sweep periodically
(every 10,000 new allocations) and at program exit.

* Subject to collection: `Array`, `Object`, and in the JIT `Closure`
  and `Cell`.
* Not tracked: `String` (leaked for the program's lifetime — small
  and simple).

Cyclic data is retrieved and mutated normally; once no external root
keeps the cycle alive, the collector frees it on the next cycle.

### Display of cycles

Printing a cycle produces `{...}` / `[...]` at the re-entry point
rather than infinite recursion (see §8).

### Auto-drop (RAII)

If an `Object` has a `Function`-typed property named `drop` that takes
no arguments, the runtime calls it automatically when the object's
properties map is about to be released. The contract is enforced at
assignment: binding `drop` to a non-function value, or to a function
with non-zero arity, raises a `type error`.

```culebra
File.open = fn (path) {
  h = _native_open(path)
  {
    read: fn () { _native_read(h) },
    drop: fn () { _native_close(h) }    # called on scope exit
  }
}

{
  let f = File.open('data.txt')
  process(f.read())
}
# f's drop ran here
```

**Cascade**: when a parent's `drop` returns, its properties map is
cleared, which decrements each child's reference count. Children
whose count reaches zero have their own `drop` invoked, and so on.
Parent-before-child order is guaranteed; sibling order among a
single parent's properties is **not specified** (currently follows
`std::map` destruction order but that is an implementation detail).

**Exceptions**: an exception thrown from `drop` is logged to stderr
and swallowed so that the rest of the cleanup cascade proceeds.

**Cycles**: cyclic references among `Object`s are reclaimed by the
cycle collector (see above), but `drop` is **not** called on cycle
members. This matches Python's `__del__` rule for cyclic garbage —
the order of finalization across cycle members would be
non-deterministic, so the rule is "no finalizers in cycles." The
guarantee applies on both backends: the interpreter has no cycle
collector and leaks cycles (so drops never fire either way), and the
JIT's cycle collector explicitly skips drop calls. To run cleanup
on a resource held by a cyclic structure, break the cycle manually
before the last reference is released (e.g. `a.other = nil`), or use
`defer` (§15) at the scope that owns the resource.

**Binding-scope caveat**: `drop` fires reliably when the object is
held in a **block-scoped** binding whose `drop` function was produced
by a **factory function** (so the closure captures the factory's
call env, not the surrounding scope). The idiom mirrors tutorial §5
closures-as-objects and works out of the box:

```culebra
make_thing = fn () {
  { drop: fn () { puts('cleaned') } }   # captures make_thing's env
}
{
  let t = make_thing()                  # block-scoped binding
}                                        # drop fires here
```

Binding a `drop`-bearing object at the **top level** may leave it
alive until program exit because the drop function captures the
top-level environment, creating an environment-level cycle that the
object-only cycle collector does not break. For script-wide
resources, prefer `defer` (§15) or explicit cleanup.

**JIT**: auto-drop fires under `--jit` with the same timing as the
interpreter — at scope exit and when the cycle collector breaks an
unreachable cycle. The well-known property contract (`drop`/`iter`/
`next` must be a 0-arg `Function`) is enforced at assignment time on
both backends.

---

## 17. Built-in type methods

The methods below are part of the language: they are available on any
value of the corresponding type without any `import`, and cannot be
shadowed by user code (for `String`, which has no property store; for
`Array`/`Object`, a user-defined property of the same name wins and
the built-in is a fallback).

Global built-in functions (`puts`, `to_string`, `Math.*`, `IO.*`,
matcher family `assert_true` / `assert_eq` / etc.) are specified
separately in [`docs/stdlib.md`](stdlib.md).

**Well-known method names (protocols).** Several method names are
recognized by the runtime and let plain `Object`s opt into language
features. They are checked by name on both backends:

| Method | Purpose | Defined in |
|---|---|---|
| `__add__`, `__sub__`, `__mul__`, `__div__`, `__mod__`, `__pow__`, `__matmul__`, `__neg__`, `__eq__`, `__lt__`, `__le__` | Operator overloading | §10 |
| `__str__` | Custom display form | §10 |
| `drop` | RAII cleanup hook | §16 |
| `iter`, `next` | Iterator protocol | §17.5 |
| `class` (property, not a method) | Nominal tag for `match` / debug | §10 |

Conventions:

* Types follow §14. `Any` denotes any value.
* Positional indices are zero-based. Negative indices where noted
  count from the end (`-1` is the last element).
* Length and indexing for `String` are in **bytes**; Culebra strings
  are not Unicode-aware.

### 17.1 String methods

All string methods accept `String` or `StringView` as receiver (and
where shown, as argument — `StringLike`). They return new values; the
receiver is never mutated.

| Signature                                       | Description                          |
|-------------------------------------------------|--------------------------------------|
| `s.size() -> Long`                              | Byte length.                         |
| `s.upper() -> String`                           | ASCII uppercase.                     |
| `s.lower() -> String`                           | ASCII lowercase.                     |
| `s.trim() -> String`                            | Remove leading/trailing whitespace (` `, `\t`, `\n`, `\r`). |
| `s.split(sep: StringLike) -> Array<StringView>` | Split on every occurrence of `sep`. Empty `sep` → `[s]`. Elements share a single source. |
| `s.split_iter(sep: StringLike) -> Iterator<StringView>` | Lazy variant of `split`. Short-circuits with `.take(n)` over huge inputs. |
| `s.contains(sub: StringLike) -> Bool`           | Whether `sub` appears anywhere. Empty `sub` → `true`. |
| `s.starts_with(prefix: StringLike) -> Bool`     | Whether `s` begins with `prefix`.    |
| `s.ends_with(suffix: StringLike) -> Bool`       | Whether `s` ends with `suffix`.      |
| `s.slice(start: Long, end: Long) -> StringView` | Substring `[start, end)` borrowing from the receiver's bytes. Negative indices count from end; `start` clamped to `[0, size()]`, `end` to `[start, size()]`. |
| `s.view() -> StringView`                        | A view aliasing the receiver's bytes (no copy). |
| `s.to_string() -> String`                       | Materialize an owning `String` (no-op for `String`; copies for `StringView`). |
| `s.iter() -> Iterator<StringView>`              | Lazy walk yielding **one-scalar `StringView`s** (UTF-8 scalar). What `for c in s { ... }` uses internally. Invalid bytes yield as one-byte substrings. |
| `s.code_points() -> Iterator<Long>`             | Lazy walk yielding **Unicode scalar values** as `Long` (`U+0000`–`U+10FFFF`). For numeric / range / classification work where the per-scalar allocation of `iter` is wasteful. Invalid bytes yield as `0`–`255`. |
| `s.graphemes() -> Iterator<StringView>`         | Lazy walk yielding **Extended Grapheme Clusters** (UAX #29) — one user-perceived character per step (e.g. an emoji ZWJ sequence is a single element). |

#### StringView

`StringView` is a borrowed view over an owning `String`'s bytes.
`slice` / `split` / `view` / `iter` / `graphemes` return `StringView`
instead of `String` to avoid per-call copies. The view keeps its
source alive (shared ownership), so a view outlives the temp that
created it:

```culebra
let v = 'hello world'.slice(6, 11)
puts(v)                              # 'world'
puts(v == 'world')                   # true  (byte equality across flavors)
puts(type_of(v))                     # 'StringView'
puts(v.to_string())                  # 'world' (materialized String)
```

Use `.to_string()` when you need an owning `String` (storing in a
data structure, returning from a long-lived function, etc.). Most
APIs declared with `StringLike` accept either flavor directly.

**Known limitations** (cycle B):
- Object keys: a `StringView` and a `String` with identical bytes are
  separate keys. Call `.to_string()` on the view before using it as a
  key. Cross-flavor `==` and hashing still treat the bytes as equal,
  but storage routing splits them.
- JIT `s.iter()` yields `Long` codepoints (interp yields `StringView`).
  For backend-uniform behavior prefer `s.code_points()` (always
  `Long`) or `for c in s` (native loop, byte-uniform).
- In long-running JIT programs, calling `.contains()` / `.starts_with()`
  / `.ends_with()` etc. on a `StringView` materializes a cstr copy
  that lives until process exit (existing culebra string-leak model).
  Materialize the view once with `.to_string()` if hot-looping over a
  large sequence of views.

```culebra
puts('hello'.size())              # 5
puts('HeLLo'.lower())             # 'hello'
puts('  hi  '.trim())             # 'hi'
puts('a,b,c'.split(','))          # ['a', 'b', 'c']
puts('hello'.slice(1, 4))         # 'ell'
puts('hello'.slice(-3, -1))       # 'll'

# Three views of the same string
puts('café'.size())               # 5  (bytes)
puts('café'.code_points().count()) # 4  (scalars)
puts('café'.graphemes().count())   # 4  (clusters)

# Emoji ZWJ sequence: 5 scalars, 1 grapheme
puts('👨‍👩‍👧'.code_points().count())  # 5
puts('👨‍👩‍👧'.graphemes().count())    # 1

# Numeric ops via code_points
upper = 'Hello World'.code_points()
  .filter(fn (cp) { cp >= 65 && cp <= 90 })
  .count()                        # 2 ('H', 'W')
```

All three iterators **decode the source string on demand**: each
`next()` only touches as much of the UTF-8 buffer as that step needs.
`s.graphemes().take(3).collect()` on a multi-megabyte `s` therefore
reads only enough bytes to resolve the first three clusters. The
per-iterator state (decode offset, lookahead buffer) is independent,
so multiple iterators derived from the same String walk in parallel
without interfering with each other.

**JIT**: `iter` / `code_points` / `graphemes` return iterator Objects
that the JIT drives through the same protocol path as user-defined
iterators. Semantics match the interpreter; throughput is dominated
by the per-step closure dispatch. For maximum speed over Arrays and
direct String scalars, prefer `for c in s { ... }` (native loop).

### 17.2 Array methods

Methods marked **mutating** modify the receiver in place and return
`nil` (except `pop`, which returns the removed element); others
return a new `Array` and leave the receiver unchanged.

| Signature                                   | Description                           |
|---------------------------------------------|---------------------------------------|
| `a.size() -> Long`                          | Number of elements.                   |
| `a.push(x: Any) -> Nil` *(mutating)*        | Append `x` to the end.                |
| `a.pop() -> Any` *(mutating)*               | Remove and return the last element. `nil` if empty. |
| `a.slice(start: Long, end: Long) -> Array`  | Shallow subarray `[start, end)`. Same clamping as `String.slice`. |
| `a.join(sep: String) -> String`             | Concatenate elements via `to_string` (strings unquoted), separated by `sep`. |
| `a.contains(v: Any) -> Bool`                | Whether `v == elem` for some element (reference types: identity; value types: contents). |
| `a.index_of(v: Any) -> Long`                | Index of first equal element, else `-1`. |
| `a.reverse() -> Nil` *(mutating)*           | Reverse in place.                     |
| `a.map(f: Function) -> Array`               | New array of `f(x)` for each element. `f` must take one parameter. |
| `a.filter(f: Function) -> Array`            | New array of elements for which `f(x)` is truthy. `f` must take one parameter. |
| `a.for_each(f: Function) -> Nil`            | Call `f(x)` for each element for side effects. `f` must take one parameter. |
| `a.reduce(init: Any, f: Function) -> Any`   | Fold: start with `init`, apply `acc = f(acc, x)` for each element, return final `acc`. `f` must take two parameters. |
| `a.find(f: Function) -> Any`                | First element for which `f(x)` is truthy, else `nil`. `f` must take one parameter. |
| `a.any(f: Function) -> Bool`                | `true` if `f(x)` is truthy for any element, else `false`. `f` must take one parameter. |
| `a.all(f: Function) -> Bool`                | `true` if `f(x)` is truthy for every element (or if empty), else `false`. `f` must take one parameter. |
| `a.flat_map(f: Function) -> Array`          | Concatenate `f(x)` for each element; each `f(x)` must be an `Array`. `f` must take one parameter. |
| `a.sum() -> Long`                           | Sum of all elements. All elements must be `Long`. Empty → `0`. |
| `a.product() -> Long`                       | Product of all elements. All elements must be `Long`. Empty → `1`. |
| `a.min() -> Long`                           | Smallest element. All elements must be `Long`. Throws on empty. |
| `a.max() -> Long`                           | Largest element. All elements must be `Long`. Throws on empty. |
| `a.sort_by(key: Function) -> Nil` *(mutating)* | Stable-sort in place using `key(x)` as the comparison key (ascending). `key` must take one parameter and return a comparable value (`Long` / `String` / `Bool`). |

```culebra
mut a = [1, 2, 3]
a.push(4)
puts(a.pop())                      # 4
puts([10, 20, 30, 40].slice(1, 3)) # [20, 30]
puts(['a', 'b', 'c'].join('-'))    # 'a-b-c'
puts([1, 2, 3].contains(2))        # true
puts([10, 20, 30].index_of(99))    # -1

puts([1, 2, 3].map(fn (x) { x * x }))           # [1, 4, 9]
puts([1, 2, 3, 4].filter(fn (x) { x % 2 == 0 })) # [2, 4]
puts([1, 2, 3, 4].reduce(0, fn (acc, x) { acc + x })) # 10

puts([3, 1, 4, 1, 5].find(fn (x) { x > 3 }))    # 4
puts([1, 2, 3].any(fn (x) { x > 2 }))           # true
puts([1, 2, 3].all(fn (x) { x > 0 }))           # true
puts([1, 2, 3].flat_map(fn (x) { [x, x * 10] })) # [1, 10, 2, 20, 3, 30]

mut words = ['banana', 'fig', 'apple']
words.sort_by(fn (s) { s.size() })
puts(words)                                      # ['fig', 'apple', 'banana']
```

### 17.3 Object methods

| Signature                        | Description                                |
|----------------------------------|--------------------------------------------|
| `o.size() -> Long`               | Number of own properties.                  |
| `o.keys() -> Array`              | `Array` of `String` keys in insertion order (matches display order — §8). |
| `o.has(key: String) -> Bool`     | Whether `o` has an own property named `key`. Ignores built-in method names. |
| `o.remove(key: String) -> Nil` *(mutating)* | Delete the property named `key` if present. |

```culebra
o = {b: 2, a: 1, c: 3}
puts(o.keys())   # ['b', 'a', 'c']  (insertion order)
puts(o.has('a')) # true
mut p = {a: 1, b: 2}
p.remove('a')
puts(p)          # {b: 2}
```

### 17.4 Special identifiers

| Identifier | Visibility               | Meaning                              |
|------------|--------------------------|--------------------------------------|
| `self`     | Inside every function    | The currently-executing function.    |
| `this`     | Inside method calls      | The method's receiver.               |

### 17.5 Iterator protocol

`for x in expr { ... }` (§12) requires `expr` to participate in the
iterator protocol. The protocol uses two **well-known method names**
on `Object` (and its subtype `Array`):

| Method | Shape | Called on | Returns |
|---|---|---|---|
| `iter` | `fn () -> Object` | an **Iterable** | an **Iterator** (may be `this`) |
| `next` | `fn () -> Object` | an **Iterator** | a step object `{ done: Bool, value: Any }` |

**Contract, enforced at property assignment**: binding `iter` or
`next` to a non-`Function` value, or to a function with non-zero
arity, raises `type error` at the assignment site (mirrors the `drop`
contract — §16).

**Step object shape**:

- `done`: truthy → iteration is complete; the loop exits without
  binding `value`.
- `value`: the value yielded for this step. Absent when `done` is
  truthy.

**Iterators are also Iterables**: an iterator's `iter` should return
itself, so `for x in some_iterator { ... }` works without a separate
Iterable wrapper.

**Built-in iterables**:

| Type | `iter()` yields | Order |
|---|---|---|
| `Array` | elements | index order (0..size-1) |
| `Object` | keys | insertion order (matches `o.keys()`) |
| `String` | one-scalar `String` per UTF-8 code point | byte order |

`String` iteration walks the UTF-8 buffer lazily, so iterating a
100 MB string with `break` after a few steps does not materialize the
rest. The yielded values are 1-scalar `String`s, not integer code
points — use `.map` on the iterator to project into whatever shape
you need.

**Object iter and structural mutation**: rewriting the *value* of an
existing key during iteration is allowed; adding or removing keys
raises `RuntimeError` (matches Python `dict` semantics). The `for k in
obj` sugar uses the same protocol.

```culebra
mut o = {mut x: 1, mut y: 2}
for k in o.iter() { o[k] = 99 }
puts(o.x)            # => 99
```

```culebra
mut o = {a: 1}
for k in o.iter() { o["b"] = 2 }
                     # !! Object changed size during iteration
```

**Iterator methods**: any Object that has both `iter` and `next`
properties (whether built-in or user-defined) picks up the lazy
iterator method set below. Non-terminal methods return a new
Iterator; terminal methods consume the iterator and return a
concrete value.

| Non-terminal | Result | Notes |
|---|---|---|
| `it.map(f)` | Iterator | yields `f(x)` for each upstream `x` |
| `it.filter(p)` | Iterator | yields only `x` where `p(x)` is truthy |
| `it.take(n)` | Iterator | first `n` elements, then `done` |
| `it.skip(n)` | Iterator | discards first `n` elements on first `next()` |
| `it.take_while(p)` | Iterator | yields until the first `p(x)` that is falsy |
| `it.flat_map(f)` | Iterator | `f(x)` must return an iterable; results concatenated |
| `it.chain(other)` | Iterator | yields `it` then `other` |
| `it.zip(other)` | Iterator | yields `{first, second}` pairs; stops at the shorter side |
| `it.enumerate()` | Iterator | yields `{index, value}` with `index` starting at `0` |

| Terminal | Result | Notes |
|---|---|---|
| `it.collect()` | `Array` | materialize into an Array |
| `it.for_each(f)` | `Nil` | invoke `f(x)` for side effects |
| `it.reduce(init, f)` | Any | left fold: `acc = f(acc, x)` starting from `init` |
| `it.find(p)` | Any \| `nil` | first `x` where `p(x)` is truthy, else `nil` |
| `it.any(p)` | `Bool` | `true` if any `p(x)` is truthy |
| `it.all(p)` | `Bool` | `true` if every `p(x)` is truthy (empty → `true`) |
| `it.count()` | `Long` | number of elements consumed |
| `it.sum()` | `Long` | sum of all elements (all must be `Long`; empty → `0`) |
| `it.product()` | `Long` | product of all elements (all must be `Long`; empty → `1`) |
| `it.min()` | `Long` | smallest element (all must be `Long`; throws on empty) |
| `it.max()` | `Long` | largest element (all must be `Long`; throws on empty) |

**Eager vs lazy**: `Array` has its own eager `map` / `filter` /
`for_each` / `reduce` / `find` / `any` / `all` / `flat_map` (§17.2)
which all return a new `Array`. Calling them on an `Array` dispatches
to the eager versions; call `.iter()` first to opt into the lazy
chain. This mirrors Swift's `arr` vs `arr.lazy`, Kotlin's `list` vs
`list.asSequence()`, and Python's list comprehension vs generator.

```culebra
# Eager: allocates two intermediate Arrays
arr.map(f).filter(g)

# Lazy: single pass, no intermediate Arrays
arr.iter().map(f).filter(g).collect()

# Lazy with early termination: only touches 4 elements
range(1000000).filter(f).map(g).take(4).collect()
```

**User-defined example**:

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter: fn () { this },                     # Iterator is its own Iterable
    next: fn () {
      if i <= 0 { { done: true } }
      else {
        v = i
        i = i - 1
        { done: false, value: v }
      }
    }
  }
}

for x in countdown(3) { puts(x) }              # 3, 2, 1
```

**JIT**: everything in this section — for-in driving the protocol,
user-defined iterators, lazy iterator method chains, `range`,
`String.code_points()` / `.graphemes()`, and Array-eager method
chaining on `[...].map(...).filter(...)` — runs under `--jit` with
interpreter-equivalent semantics. The Array/String/keys fast paths
stay native (one load per element); iterator-protocol driving pays a
per-step closure dispatch, which is inherent to a dynamic-language
iterator chain. Use `iota` + `Array.map` / `.filter` / `.reduce`
when you want eager materialization and maximum throughput; use
`range` + lazy methods when you want constant-memory streaming.

---

## 18. Core built-in functions

The functions below are part of the language proper: they are bound
into every execution environment as global names and cannot be
replaced. The first group (`to_long` / `to_float` / `to_string`,
`type_of`) is tied to language semantics — source-position errors,
type introspection, and the display convention. The second group
(`range`, `iota`) provides the canonical integer-sequence factories;
both backends recognise them for fusion / specialisation, and they
are the standard form used in `for`-in loops throughout the language.
The matcher family (`assert_true` / `assert_eq` / `assert_throws` /
`assert_close` / etc.) is a third group of globals — see
[`docs/stdlib.md`](stdlib.md) for the full reference. The broader
standard library (namespaced under `Math`, `IO`, `Sys`) is also
documented in `stdlib.md`. Output primitives `puts` and `print` are
CLI-installed globals (§21).

### `to_long(v: Any) -> Long`

Convert `v` to `Long`:

* `Long` → itself.
* `Float` → truncated toward zero (matches Python's `int()`).
  `to_long(3.7) == 3`, `to_long(-3.7) == -3`.
* `String` → parsed as a base-10 signed integer; leading/trailing
  whitespace is allowed, anything else fails.
* Other types raise `type error`.

**Throws**: `type error at L:C.` on an unparseable string or a
non-numeric / non-string argument.

```culebra
puts(to_long('42'))    # 42
puts(to_long('-7'))    # -7
puts(to_long(3.9))     # 3
```

### `to_float(v: Any) -> Float`

Convert `v` to `Float`:

* `Float` → itself.
* `Long` → promoted to `Float` (exact for absolute values up to
  2⁵³; larger magnitudes may lose precision).
* `String` → parsed as a decimal or exponent-form float; leading /
  trailing whitespace is allowed.
* Other types raise `type error`.

```culebra
puts(to_float(3))         # 3.0
puts(to_float('1.5'))     # 1.5
puts(to_float('1e-5'))    # 1e-05
```

### `to_string(v: Any) -> String`

Convert `v` to its display form (same formatting as interpolation
inserts — strings come through unquoted). See §8 for the display
convention. `Float` uses the shortest round-trip decimal and always
carries either a decimal point or an exponent, so the type is
visually distinguishable from `Long`.

```culebra
puts(to_string(42))         # '42'
puts(to_string(1.0))        # '1.0'
puts(to_string(1e-5))       # '1e-05'
puts(to_string([1, 2]))     # '[1, 2]'
puts(to_string('hi'))       # 'hi'
```

### `type_of(v: Any) -> String`

Return the runtime type name of `v`. One of
`'Nil'`, `'Bool'`, `'Long'`, `'Float'`, `'String'`, `'Array'`,
`'Object'`, `'Function'`, `'Tensor'`, `'Tuple'`, `'Set'`.

```culebra
puts(type_of(42))          # 'Long'
puts(type_of(1.5))         # 'Float'
puts(type_of('hi'))        # 'String'
puts(type_of([1, 2]))      # 'Array'
puts(type_of((1, 2)))      # 'Tuple'
puts(type_of({1, 2}))      # 'Set'
```

### `range(n: Long, *, step: Long = 1) -> Iterator` / `range(start: Long, end: Long, *, step: Long = 1) -> Iterator`

Lazy integer-sequence factory: returns an Iterator (§17.5) that
yields integers one at a time. Use with `for`-in or iterator method
chains to iterate in **constant additional memory** regardless of
the range size.

* `range(n)` yields `0, 1, ..., n-1`. If `n <= 0`, the iterator
  completes immediately.
* `range(start, end)` yields `start, start+1, ..., end-1`. If
  `start >= end`, completes immediately.
* `step:` is keyword-only. Positive values count up (exclusive end);
  negative values count down (exclusive end). `step: 0` raises
  `ValueError`.

```culebra
for i in range(5)              { puts(i) }   # 0, 1, 2, 3, 4
for i in range(2, 6)           { puts(i) }   # 2, 3, 4, 5
for i in range(0, 10, step: 2) { puts(i) }   # 0, 2, 4, 6, 8
for i in range(5, 0, step: -1) { puts(i) }   # 5, 4, 3, 2, 1

# Constant memory even for huge bounds
for i in range(1_000_000_000) {
  if i > 3 { break }
  puts(i)
}
```

**JIT**: `range` returns a JIT-native iterator Object, and the
`range(N).<HOF>(...)` method-chain pattern is fused into a direct
counter loop. See §17.5.

### `iota(n: Long) -> Array` / `iota(start: Long, end: Long) -> Array`

Eager counterpart to `range`: materialise an `Array` of the same
sequence. Named after APL / C++ `std::iota` / Scheme SRFI-1. Prefer
`range` for `for`-in loops; use `iota` when you actually need the
full `Array` (e.g. to index into it, or to pass to a function that
expects an `Array`).

* `iota(n)` returns `[0, 1, ..., n-1]`. If `n <= 0`, an empty array.
* `iota(start, end)` returns `[start, start+1, ..., end-1]`. If
  `start >= end`, an empty array.

```culebra
puts(iota(3))         # [0, 1, 2]
puts(iota(2, 5))      # [2, 3, 4]
puts(iota(5, 2))      # []
```

### `__ARGS__` (variadic catch-all binding)

Inside any function body, the implicit local `__ARGS__` is bound to
an `Array` of positional arguments that overflowed the declared
parameters. Use it when a fn takes a variable number of trailing
values without declaring an explicit `**rest` (which catches
*keyword* args, not positional).

```culebra
let logger = fn (level) {
  puts("[{level}] " + __ARGS__.join(' '))
}
logger('info', 'building', 'fizzbuzz')   # → '[info] building fizzbuzz'
```

`__ARGS__` does **not** receive keyword arguments — those go through
the explicit param list or `**rest`. If a fn declares no params and
is called with arguments, every argument lands in `__ARGS__`.

### Function introspection

Every `Function` value exposes three read-only properties for
introspection:

| Property        | Type           | Description                                                                 |
|-----------------|----------------|-----------------------------------------------------------------------------|
| `fn.name`       | `String`       | Source-level declaration name (`fn name(...)`) or `""` for anonymous fns    |
| `fn.return_type`| `String`       | Return-type annotation (`fn f() -> X`) or `""` if unannotated               |
| `fn.params`     | `Array<Object>`| Per-parameter metadata; each entry has `name / mut / type / has_default / kw_only / kwargs_rest` |

```culebra
fn greet(name: String, *, prefix = "hi") { "{prefix}, {name}" }

puts(greet.name)                  # → 'greet'
puts(greet.return_type)           # → ''
let ps = greet.params
puts(ps.size())                   # → 2
puts(ps[0].name)                  # → 'name'
puts(ps[0].type)                  # → 'String'
puts(ps[1].kw_only)               # → true
puts(ps[1].has_default)           # → true
```

`fn.params` returns a fresh `Array` each access; mutating it has no
effect on the function. Multifn dispatchers expose the **first
registered method's** signature (overloads after the first do not
appear).

---

## 19. Multimethods

Multiple top-level `fn name(params) body` declarations sharing a name
form a **multimethod** when their parameters have differing type
annotations (§14). At a call site, the most specifically matching
method is selected based on the runtime types of the arguments.

```
fn area(s: Long)   { s * s }
fn area(s: Float)  { s * s }
fn area(s: String) { s.size() }

area(5)        # → 25     (Long)
area(5.0)      # → 25.0   (Float)
area("hello")  # → 5      (String)
```

Anonymous function expressions `let f = fn(...) {...}` are unaffected.
Multimethods only apply to **top-level `fn name(...)` declarations**.

### Syntax

```
MULTIFN_DECL <- 'fn' IDENTIFIER PARAMETERS RETURN_TYPE? BLOCK
```

When `fn name(...)` appears in statement position, this rule takes
precedence. Anonymous `fn(...) {...}` continues to be an expression
(closure), as before.

### Specificity rules

For each argument *i*, the parameter annotation and the runtime type
of the argument yield a specificity score:

| Annotation                                              | Argument                                 | Score    |
|---------------------------------------------------------|------------------------------------------|----------|
| `Any` (no annotation)                                   | anything                                 | 0        |
| `Object`                                                | class instance (e.g. `Square`, `Circle`) | 1        |
| Exact match (`Long`, `Float`, ..., concrete class name) | same type                                | 2        |
| otherwise                                               | —                                        | no match |

The per-argument scores form a tuple. A method is selected when its
score tuple is **at least as large as the other in every position and
strictly greater in at least one**. If two tuples compare equal, an
**ambiguous dispatch** error is raised. If two tuples are
incomparable (each wins on some position), the earlier-declared
method takes precedence.

```
fn label(x: Long, y)       { "long-any"  }
fn label(x, y: Long)       { "any-long"  }
fn label(x: Long, y: Long) { "long-long" }   # most specific
fn label(x, y)             { "any-any"   }

label(1, 2)         # → "long-long"
label(1, "x")       # → "long-any"
label("x", 1)       # → "any-long"
label("x", "y")     # → "any-any"
```

### Class instance dispatch

Instances created by `class` declarations (§10) dispatch on their
class name:

```
class Square { new(side) { this.side = side } }
class Circle { new(r)    { this.r    = r    } }

fn shape_area(s: Square) { s.side * s.side }
fn shape_area(c: Circle) { 3.14 * c.r * c.r }

shape_area(Square.new(4))  # → 16
shape_area(Circle.new(2))  # → 12.56
```

This relies on the `class:` String property that class sugar attaches
to each instance (§10). The `Object` annotation is looser and matches
any class instance.

### Same-name, same-signature redeclaration

A subsequent declaration whose **parameter type sequence matches an
existing entry exactly** overwrites that entry's body. The semantics
are intended for REPL iteration.

```
fn greet(name: String) { "hi, {name}"    }
fn greet(name: String) { "hello, {name}" }   # overwrites

greet("alice")  # → "hello, alice"
```

### Coexistence with existing features

* Ordinary local bindings `let f = fn(...) {...}` continue to work
  unchanged.
* Method dispatch on `obj.method()` is unaffected. Methods are still
  defined inside a `class` body and called through `this.`.
* A method whose parameters are all `Any` serves as a catch-all.

### Constraints

* **Top-level / free functions only.** Nested declarations inside a
  block, and class methods, are not subject to this mechanism.
* **Errors.** With no matching method the runtime raises
  `no matching method`; with a tie in specificity it raises
  `ambiguous dispatch`. Both halt the program immediately rather than
  surfacing as catchable runtime exceptions (§15, §23).

### Keyword arguments and multimethods

Dispatch picks on the **positional** argument types only (Julia-style
kwsorter). Keyword arguments and `**` splats flow through dispatch
into the picked method's signature, where they bind via the regular
kwargs rules (§11).

```
fn paint(s: String, *, color = "red")  { "{color} {s}" }
fn paint(n: Long,   *, color = "blue") { "{color} {n}" }

paint("circle")              # → "red circle"     (String)
paint(7)                     # → "blue 7"         (Long)
paint("box", color: "green") # → "green box"
paint(7, **{color: "gold"})  # → "gold 7"
```

Each method's own kw-only defaults, `**rest` catch-all, and parameter
names are independent — only the positional signature participates in
dispatch.

---

## 20. Decorators

A `@expr` line before a `fn` or `class` declaration wraps the
declared value through `expr` before binding it to the original name.
Stacked decorators apply bottom-up — the one closest to the
declaration runs first, the topmost runs last:

```culebra
@a
@b
fn foo() { ... }
```

is sugar for `foo = a(b(<original fn value>))`.

### Decorator expression

Each `@` is followed by any expression of the form `name`,
`name.attr`, or `name(args)` — i.e. a `CALL` chain. The expression
is evaluated **once** at declaration time in the enclosing scope;
its result must be callable (a function or a class with a single
positional parameter). Multi-arg factories — `@combine(a, b)` — work
by returning a one-argument decorator from the factory call.

### Bindings

The decorator receives the **unbound** declared value and returns
the value that ends up in the variable:

```culebra
let tag = fn(f) { fn() { "[{f()}]" } }

@tag
fn greet() { "hello" }

greet()  # "[hello]"
```

For a class, the decorator gets the class object and returns the
object the variable will hold:

```culebra
let mark = fn(cls) { cls.marked = true; cls }

@mark
class Point { new(x, y) { this.x = x; this.y = y } }

Point.marked  # true
```

### Interaction with multimethods

A decorated `fn name(...)` does **not** participate in multimethod
dispatch — the decorator's return value binds directly to `name`.
Combine the patterns by writing the multimethod first and then a
separate decorated wrapper if you need both.

### Why `@` doesn't collide with the matmul operator

`@` is also the binary matrix-multiplication operator (PEP 465).
The two uses never overlap: decorator `@` only matches at statement
prefix position, while matmul `@` is parsed inside an expression
between two operands and never crosses a newline. So
`}\n@deco fn ...` is unambiguously two statements (a decorator on a
fresh declaration), not a matmul continuation of the preceding
expression.

---

## 21. Command-line interface

    culebra [flags] [script.cul ...] [-- arg ...]

Everything after a standalone `--` is captured verbatim and exposed to
the script as `Sys.argv` (see [`docs/stdlib.md`](stdlib.md)). Without
a `--`, `Sys.argv` is empty.

### Flags

| Flag           | Effect                                                    |
|----------------|-----------------------------------------------------------|
| `--shell`      | Drop into the REPL after running any scripts.             |
| `--ast`        | Print the parsed AST before running.                      |
| `--debug`      | Enable the CLI debugger; `debugger` statements break in.  |
| `--jit`        | Use the LLVM ORC JIT instead of the tree-walking interpreter. |
| `--emit-llvm`  | With `--jit`, print the generated IR and exit.            |
| `-O0`..`-O3`   | With `--jit`, select the LLVM optimization level. Default `-O2`. |

If no script is provided, the REPL is launched automatically. Both
the interpreter REPL and the JIT REPL preserve session state across
inputs — `let`, `mut`, and `fn` declarations from one input are
visible to subsequent inputs, including from inside nested closures.
The JIT REPL keeps a single `LLJIT` instance alive for the session
and adds each input as a new module; top-level bindings are stored
in a per-session dictionary that runtime helpers read through a
thread-local pointer, so closures defined at the prompt can resolve
top-level names at call time.

Free-variable references inside REPL-defined closures pay one hash
lookup per access (the closure body emits a `repl_get` call instead
of capturing a cell). Bodies declared inside a function literal —
not at the prompt — keep the normal capture machinery and are
unaffected.

The REPL persists input history across sessions. The path is
`$CULEBRA_HISTFILE` if set, otherwise `$XDG_STATE_HOME/culebra/history`
when defined, otherwise `~/.culebra_history`. History is rewritten
after each accepted line so a crash mid-session doesn't lose it
(matching Python and Node REPL convention).

### CLI-installed globals

The CLI binary adds two globals to the script environment before
running user code:

| Global  | Aliased to  |
|---------|-------------|
| `puts`  | `IO.puts`   |
| `print` | `IO.print`  |

These are convenience shortcuts for the most common output calls;
they point to the same function values that live under `IO`, so
`puts(x)` and `IO.puts(x)` are fully equivalent. Embedders that use
`culebra::environment()` directly do not receive these aliases —
their environment contains only `Math`, `IO`, `Sys`, and the core
built-ins from §18.

---

## 22. Known limitations

* No big integers or bignums; `Long` overflow wraps.
* Single-quoted `'...'` strings are raw (no escapes, no interpolation,
  no embedded apostrophes). Double-quoted `"..."` strings recognize a
  fixed escape set (`\n \r \t \\ \" \{`); the `{{` / `}}` form for
  literal braces is not supported.
* `String` is byte-indexed (`size` / `slice` count bytes); Unicode
  work goes through `code_points()` / `graphemes()` iterators.
* `Array` and `Object` equality compare by reference, not structural.
* Runtime errors (`type error`, `divide by 0`, `index out of range`,
  etc.) abort the program and do **not** flow through user `throw` /
  `try` / `catch` — those channels are reserved for user-raised
  values.
* JIT `defer` at function / top-level scope does not run on the
  throw-unwind path (see §15); wrap the defer in a nested block for
  throw-safe cleanup.
* No module / `import` system yet.
* Type annotations are enforced only at function boundaries and
  annotated assignments; they do not make the language static.
* Pattern matching has no exhaustiveness check.
* `match` arm bodies must be single expressions — `{ ... }` in an arm
  body is parsed as an `Object` literal, not a block.
* Dot-form property names are identifiers only (`obj.foo`). Non-String
  hashable keys (`Long`, `Float`, `Bool`, `Nil`, `Tuple`) reach the
  Object via the subscript path (`obj[k]`) and live in a sidecar map.
  Runtime `String` keys via `obj[k]` unify with the shape — `obj['x']`
  and `obj.x` reach the same slot. See §10 "Subscript assignment".

---

## 23. Modules

A program may span multiple files. One file `imports` another by
name; the imported file may `export` selected bindings to expose
them to importers. The system is static and minimal — paths are
literal strings, imports and exports appear only at the top level,
and the runtime never resolves a name across files at call time.

### Module syntax

A module's source is an ordinary culebra file. It exposes bindings
through one or more `export` statements:

    # math_utils.cul
    let add = fn (a, b) { a + b }
    let sub = fn (a, b) { a - b }
    let helper = fn () { ... }      # internal, not exported

    class Pair {
      new (x, y) { this.x = x; this.y = y }
    }

    export { add, sub, Pair }

A consumer of the module imports it through a single name:

    # main.cul
    import math from './math_utils.cul'

    math.add(2, 3)            # 5
    let p = math.Pair.new(1, 2)
    math.helper               # nil — not on the export Object

### Statements

| Form | Meaning |
|---|---|
| `import NAME from 'path'` | Loads the module at `path` and binds its export Object under `NAME` in the current file. |
| `export { N1, N2, ... }` | Adds each listed local name to the exporting module's export Object. |

The path is a STRING literal (single-quoted, non-interpolated). It
is resolved relative to the importing module's directory. Absolute
paths are accepted unchanged.

### Top-level only

Both `import` and `export` may only appear as top-level statements
of a module. They are syntactically rejected inside functions,
`if` branches, blocks, and so on:

    let f = fn () {
      import lib from './lib.cul'    # SyntaxError
    }

This rule guarantees that the dependency graph and the set of
exported names are determined at parse time, which is required
for both bundled AOT builds and tree-shaking analysis.

### Evaluation rules

1. **Dependency resolution.** The driver loads the entry module,
   walks its `import` statements to find dependencies, recurses,
   and records absolute paths in load order. The resulting graph
   is topologically sorted (every module appears after its
   dependencies).
2. **Per-module scope.** Each dependency is evaluated in its own
   fresh scope. Top-level bindings inside the module are visible
   only within that file plus its export Object — they do not
   leak into the importer.
3. **Caching.** The same absolute path is evaluated at most once
   per program; subsequent imports retrieve the cached export
   Object.
4. **Export Object.** After a dependency's body finishes, the
   runtime collects every name listed in its `EXPORT_STMT`s into
   a single Object (immutable properties). Multiple `export`
   statements are **merged** in source order — a file may issue
   `export` more than once.
5. **Entry module.** The entry module evaluates last and shares
   the caller-facing scope, so its top-level bindings remain
   visible to whoever invoked the program.

### Exports

* A module without any `export` statement produces an empty
  Object. `import x from "..."` still succeeds; missing members
  read back as `nil` per the standard Object property rule
  (§10). Useful for side-effect-only modules.
* Multiple `export { ... }` statements in a single file are
  merged. This lets a file declare a few helpers, export them,
  declare more, export more.
* Listing the same name twice — whether in one `EXPORT_STMT` or
  spread across multiple — is a `SyntaxError` (parse-time). The
  name must be defined as a local binding in the module before the
  export references it.

### Errors

| Trigger | `kind` |
|---|---|
| `import` from a file that doesn't exist | `IOError` |
| Imported file fails to parse | `SyntaxError` |
| Circular import (A imports B which imports A) | `ImportError` |
| `import` or `export` outside top level | `SyntaxError` |
| Duplicate name in one `export` statement | `SyntaxError` |
| `export { foo }` but `foo` is not defined in the module | `NameError` |

(Missing-member lookup on an imported namespace is *not* an
error — `mod.unknown` returns `nil`, matching the rule for
plain Objects in §10.)

All errors carry `kind` / `message` / `line` / `col` and are
surfaced identically by both backends. `IOError`, `SyntaxError`,
and `NameError` join the existing catalog in §15; `ImportError`
is added for the circular-dependency case (catchable).

### Backend equivalence

Both backends produce identical observable behavior for any
module program. They differ only in how dependencies are wired
internally:

* **Interpreter.** Walks the loader's vector, evaluates each
  dependency in a fresh `Environment`, and stores its export
  Object in `Interpreter::module_cache_` keyed by absolute path.
  `IMPORT_STMT` reads the cache and binds the resulting Object
  in the importer's scope.
* **JIT / AOT.** Compiles every module's body into the same
  `__culebra_main` function, scope-isolated. After each
  dependency's body, IR emits `culebra_runtime_module_register`
  with the absolute path and the export Object. `IMPORT_STMT`
  IR emits `culebra_runtime_module_get` with the same path and
  binds the result.

The module table is a `thread_local` keyed by absolute path, so
the same module compiled into different JIT sessions on the
same thread observes the same caching rules without sharing
state between threads.

### Conformance tests

`tests/test_import.cul` exercises the happy path on both
backends (basic import, mixed function / class exports, multiple
`export` statements, chained imports). The supporting modules
live under `tests/test_import_helpers/`. Error cases
(circular imports, top-level violations, duplicate exports) are
covered by inline `try { ... } catch { ... }` in the same file
where the failing source is itself another helper.

---

## 24. Appendix: interpreter ↔ JIT divergence

The interpreter (`include/interpreter.h`) is normative. The JIT
(`include/jit.h`) compiles the same AST and is required to produce
**identical observable behavior** for every program — same return
values, same side-effect ordering, same error `kind` / `message` /
location. Internal representation is free to differ as long as the
externally observable behavior matches; any deviation in observable
behavior is a bug in the JIT.

### Identical observable behavior

The following are guaranteed identical across interpreter, JIT, and
AOT builds:

* **Numerics (§7).** Long overflow wrap, Float IEEE-754 (NaN, inf,
  signed zero), division/modulo by zero raising
  `ZeroDivisionError`, comparison and truthiness rules.
* **Argument evaluation order.** Left-to-right in source order for
  positional, keyword, and `**`-splat arguments, with `||` / `&&` /
  `??` short-circuiting at the first decisive operand. The same
  rule applies to array and object literals.
* **Method dispatch and UFCS (§10), operator special methods (§10),
  `__str__` display (§10), and the iterator protocol (§17.5).**
* **`throw` / `try` / `catch` / `defer` (§15).** Including
  function-level and top-level `defer` firing on the throw-unwind
  path. The JIT lowers throws to LLVM `invoke` / `landingpad` with
  the Itanium personality; observable propagation is identical.
* **Auto-drop (§16).** Fires on every refcount-to-zero transition,
  whether triggered by scope exit, property overwrite, or array
  rebind. Cascade order — parent before child — is the same. Drop
  is **not** invoked on cycle members on either backend (see §16).
* **Error reporting.** Every `kind` listed in §15 fires under the
  same trigger condition on both backends; `e.message`, `e.line`,
  and `e.col` are populated identically. Uncaught errors print as
  `Kind: message at L:C.`.
* **Class sugar (§10), `static` methods, immutable `this`, and the
  auto-synthesized `parameters()` reflection.**
* **Module-scope evaluation.** Statement order, top-level closures,
  forward-reference resolution, and decorator application all
  follow the same rules.

### Permitted internal optimizations

These change how the program executes but not what it observes:

* **Object property storage.** The JIT uses a process-interned
  hidden-class (Shape) layout with vector-backed slots and
  per-callsite inline caches. The interpreter uses an insertion-
  ordered map. Iteration order over an Object's keys is insertion
  order on both.
* **HOF inlining.** `array.map / filter / for_each / reduce` and
  `range(N).<HOF>(...)` / `iter.map(λ).collect()` compile their
  lambda bodies directly into the iteration loop. Side-effect
  order is preserved.
* **Class method storage.** The JIT places methods on a shared
  per-class meta object reached via prototype delegation; the
  interpreter copies methods onto each instance. `obj.m` returns a
  bound function on either backend.
* **Generational cycle collector.** Both backends mark-and-sweep
  the young set every 10,000 new allocations; the JIT additionally
  splits young/old with vector-backed tracking.
* **Forward-reference pre-allocation.** The JIT scans each function
  body and pre-allocates capture cells so closures compiled before
  their `let` declaration still see the post-declaration value.
  The interpreter resolves names dynamically against the live env.

### Technical differences without behavioral impact

These do not affect any program-visible behavior, but operators
embedding Culebra should be aware:

* **Cycle collector tracking scope.** The interpreter's `InterpGC`
  tracks `Array` (so cycles routed through an array are reclaimed).
  The JIT's `_GcTracker` additionally tracks `Object`, `Closure`,
  and `Cell`. Cycles formed purely between `Object`s without
  passing through an array leak under the interpreter, but neither
  backend invokes `drop` on cycle members either way.
* **REPL persistence storage.** The interpreter persists session
  globals in the top-level `Environment` scope. The JIT uses a
  thread-local `JitReplGlobals` dict accessed via
  `culebra_runtime_repl_{get,set}`. User code observes the same
  binding behavior across statements.
* **Thread safety.** Neither backend is thread-safe: `_GcTracker`,
  `InterpGC`, `ShapeRegistry`, and the deferred-stack state are
  process-wide. Embedders that share a Culebra runtime across
  threads must serialize calls themselves.

### Top-level drop note (§16)

Top-level bindings to `drop`-bearing objects may live until program
exit due to environment-level cycles on either backend (see §16 for
the worked example). The mechanism is not JIT-specific; use `defer`
or a factory function for script-wide resources regardless of which
backend you run on.

When in doubt, the interpreter is authoritative — any JIT deviation
in observable behavior is treated as a bug.

---

## 25. Appendix: conformance test mapping

Every section of this spec has at least one corresponding test file
under `tests/`. `just test` runs interp/JIT diff, AOT diff, and
embedding C++ smoke in one pass; `just test aot` runs only the AOT
diff. The mapping below points to the primary owner; some tests
touch multiple sections, marked "(broad)".

| Test file | Spec sections verified |
|---|---|
| `tests/test_core.cul` | §6, §7, §8, §9, §10, §11, §12, §15, §17, §18 (broad — primary unit-test catch-all) |
| `tests/test_class.cul` | §10 (class sugar, operator overloading, `__str__`, auto-reflection, static methods), §11 |
| `tests/test_class_parameters.cul` | §10 (auto-synthesized `parameters()`) |
| `tests/test_decorator.cul` | §20 |
| `tests/test_defer.cul` | §15 (`defer`, scope-guard pattern) |
| `tests/test_forward_ref.cul` | §6 (scope), §11 (closures), §19 |
| `tests/test_iter.cul` | §12 (`for ... in`), §17 (iterator protocol, String methods), §18 (`range`, `iota`) |
| `tests/test_kwargs.cul` | §11 (keyword arguments, `**` splat), §19 (kwargs in multimethods), §7 (evaluation order for mixed calls) |
| `tests/test_match_class.cul` | §13 (type patterns) |
| `tests/test_multidispatch.cul` | §19 |
| `tests/test_object_keys.cul` | §10 (non-String keys) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`, all `kind` values catchable) |
| `tests/test_set.cul` | §10 (sets) |
| `tests/test_tuple.cul` | §10 (tuples, destructuring) |
| `tests/test_ufcs.cul` | §10 (methods, UFCS), §18 (`__ARGS__`) |
| `tests/test_args.cul` | stdlib §9 (`Args`) |
| `tests/test_fs.cul` | stdlib §3 (`FS`) |
| `tests/test_json.cul` | stdlib §8 (`JSON`) |
| `tests/test_tensor.cul` | stdlib §7 (`Tensor`) |
| `tests/test_time.cul` | stdlib §4 (`Time`) |
| `tests/test_import.cul` | §23 (Modules) — uses `tests/test_import_helpers/*.cul` as dependencies |

Every test file in `tests/` is required to pass on both backends
with identical stdout — `just test` enforces it. Interactive
features that are inherently backend-specific (debugger hooks
exercised by REPL state) are tested through `tests/embedding/`
C++ smoke tests rather than as `.cul` scripts.
