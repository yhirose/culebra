# Culebra Language Specification

This document defines the syntax and runtime semantics of the Culebra
programming language. It is normative: where the two backends
(interpreter in `include/interpreter.h` and JIT in `include/jit.h`)
disagree, the interpreter is considered authoritative and the JIT
tracks its behavior.

For an introductory tour with runnable examples, see
[`handbook.md`](handbook.md). For API reference of the standard library see
[`stdlib.md`](stdlib.md).

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
    * [`class` sugar](#class-sugar)
    * [Operator overloading](#operator-overloading)
    * [Tuples](#tuples)
    * [Sets](#sets)
11. [Functions and closures](#11-functions-and-closures)
    * [Generators (`yield`)](#generators-yield)
12. [Control flow (`if`, `cond`, `while`, `for`)](#12-control-flow)
13. [Pattern matching (`match`)](#13-pattern-matching)
14. [Optional type annotations](#14-optional-type-annotations)
    * [Sum types (`enum`)](#sum-types-enum)
    * [Traits and protocols](#traits-and-protocols)
15. [Error handling](#15-error-handling)
16. [Algebraic effects](#16-algebraic-effects)
17. [Memory model (RC + cycle collector)](#17-memory-model)
18. [Built-in type methods (incl. iterator protocol)](#18-built-in-type-methods)
19. [Core built-in functions](#19-core-built-in-functions)
20. [Multimethods](#20-multimethods)
21. [Decorators](#21-decorators)
22. [Command-line interface](#22-command-line-interface)
23. [Known limitations](#23-known-limitations)
24. [Modules](#24-modules)
25. [Appendix: interpreter ↔ JIT divergence](#25-appendix-interpreter--jit-divergence)
26. [Appendix: conformance test mapping](#26-appendix-conformance-test-mapping)

---

## 1. Overview and philosophy

Culebra is a dynamically-typed scripting language. Its priorities
are:

* **Orthogonal features.** New capability arrives as a feature that
  composes with the rest, not as a special case in the grammar.
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

Reserved words that cannot name a variable:

    nil  true  false  mut  debugger  return  while  for  in  if  unless  else
    fn  match  break  continue  throw  try  catch  defer
    class  trait  enum  import  export  yield

Assigning to one is a `SyntaxError` (`let class = 1`). The reservation
covers the assignment target only: a parameter, a `for` variable, an
object key, and a property name are all positions where no keyword can
start, so they accept any of these words.

Four of them — `return`, `throw`, `break`, `continue` — are the
exception when *read* rather than bound. They are expressions (§12), so
an occurrence of one in expression position is the control-transfer
form, never a variable reference. `for break in xs { f(break) }` binds
`break` and then leaves the loop instead of passing it. Object keys and
property names are unaffected (`{break: 1}` and `o.break` still work),
since neither is an expression position.

The grammar's remaining keywords are *contextual* — each is recognized
only where its own construct can begin, and is an ordinary identifier
everywhere else:

    cond  nobreak  from  with  effect  perform  handle  static  get  by

`static` and `get` are markers inside a class body and `by` is
contextual inside a range literal (`0..10 by 2`). The other seven
introduce constructs with a distinctive shape — `cond {` … `}` needs at
least one `test => body` arm, `nobreak` follows a loop body, `from`
follows an `import` — so `let with = 1` and `if handle { … }` both name
a variable. Type annotation names (`Nil`, `Bool`, `Long`, `Float`,
`String`, `Array`, `Object`, `Function`, `Any`) are not reserved
either; they are contextual and only recognized after `:` or `->`.

The parser also recognizes `let` as an optional prefix in assignments.

### Literals

* Integer: a `NUMBER` is a 64-bit signed integer (`Long`), written in
  decimal or with a radix prefix — hex `0xFF`, octal `0o755`, binary
  `0b1010` (prefix letter is case-insensitive; digits match the base).
* Float: `FLOAT <- [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)? / [0-9]+ [eE] [-+]? [0-9]+`.
  A literal with either a decimal point (followed by digits, not a
  bare trailing dot) or an `e`/`E` exponent is a `Float` (IEEE 754
  binary64). Examples: `1.0`, `0.5`, `-2.5` (unary minus applied to
  `2.5`), `1e-5`, `1.5e3`.
* String: `'...'` is a **raw** string — every character between the
  quotes is taken literally, including backslashes. There is no escape
  syntax, no interpolation, and apostrophes inside are not expressible
  (use a backtick string `` `...` `` for raw content containing `'`).
* Interpolated string: `"...{expr}..."`. `{expr}` embeds any expression.
  Recognized escape sequences: `\n` `\r` `\t` `\\` `\"` `\{` (use `\{`
  to embed a literal `{` without starting an interpolation; raw `}` is
  fine since it's only special as the interpolation terminator),
  `\xHH` (exactly two hex digits → one raw byte, `0x00`–`0xFF`), `\uXXXX`
  (exactly four hex digits → a BMP Unicode scalar value, UTF-8 encoded),
  and `\UXXXXXXXX` (exactly eight hex digits → a Unicode scalar value,
  UTF-8 encoded; the only form that reaches beyond the BMP). Both reject
  values above `U+10FFFF` and the surrogate range. An unknown `\X` is
  preserved as the two literal characters `\` and `X`.
* Boolean: `true`, `false`.
* Nil: `nil`.

**Trailing comma.** A single trailing comma is allowed after the last
element of an array `[1, 2,]`, object `{a: 1,}`, set `{1, 2,}`, tuple
`(1, 2,)`, argument list `f(1, 2,)`, parameter list `fn g(a, b,)`,
lambda params `|a, b,|`, and destructuring pattern `let [a, b,] = …`
(cleaner diffs, and reordering without touching the neighbours).
A leading or doubled comma is still a syntax error. Note `(x,)` and
`{x,}` are *not* merely cosmetic: the comma marks a one-element tuple
and a one-element set respectively.

### Operators and punctuation

    ==  !=  <=  <  >=  >        # comparison
    +  -  *  /  %  **           # arithmetic (`**` exponentiation); `+` also concatenates strings and Arrays
    @                           # matmul (user-defined via `__matmul__`)
    |  &  ^  <<  >>  ~          # bitwise or / and / xor / shifts / complement (Long only)
    !                           # logical not
    &&  ||                      # logical and/or (short-circuit)
    ??                          # nil coalesce (lower precedence than ||)
    ?.  ?[ ]                    # optional chaining / optional index
    ? :                         # ternary conditional `c ? a : b` (right-assoc)
    !!                          # non-null assertion (postfix)
    ..  ..=                     # range literals (exclusive / inclusive)
    =                           # assignment
    +=  -=  *=  /=  %=  **=  @= # compound assignment
    ??=                         # nil-coalescing assignment
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

The full PEG grammar is in
[`include/grammar_def.h`](../include/grammar_def.h), the single source
of truth that `parser.h` loads. Its top-level rules:

    PROGRAM     <- STATEMENTS
    STATEMENTS  <- (STATEMENT ((';' / newline) STATEMENT?)*)?
    STATEMENT   <- DEBUGGER / RETURN / THROW / YIELD_FROM / YIELD / BREAK
                 / CONTINUE / DEFER / IMPORT_STMT / EXPORT_STMT
                 / EFFECT_FN_DECL / MULTIFN_DECL / ENUM_DECL / CLASS_DECL
                 / TRAIT_DECL / LEXICAL_SCOPE / EXPRESSION
    EXPRESSION  <- DESTRUCTURE_ASSIGN / PLACE_ASSIGN / ASSIGNMENT / TRY
                 / CONDITIONAL
    ASSIGNMENT  <- LET MUTABLE PRIMARY (ARGUMENTS / INDEX / DOT)*
                   TYPE_ANNOTATION? ASSIGN_OP EXPRESSION
    CALL        <- PRIMARY (ARGUMENTS / INDEX / SAFE_DOT / SAFE_INDEX
                            / DOT / NONNULL)*
    PRIMARY     <- WHILE / FOR / IF / MATCH / COND / HANDLE / PERFORM
                 / RETURN / THROW / BREAK / CONTINUE
                 / FUNCTION / LAMBDA / OBJECT / SET / ARRAY / NIL
                 / BOOLEAN / FLOAT / NUMBER / REGEX_LIT / IDENTIFIER
                 / TRIPLE_STRING / STRING / RAW_STRING
                 / INTERPOLATED_STRING / TUPLE / '(' EXPRESSION ')'

Statements are the constructs that may not appear in expression
position: the declaration forms, `yield` / `yield from`, `defer`, and the
module forms. Everything else — including `if`, `while`, `for`, `match`,
`cond`, `handle` and `perform` — is a `PRIMARY`, which is what makes
them usable as values (§7).

The four **diverging** forms — `return`, `throw`, `break`, `continue` —
are `PRIMARY` too, so they may sit wherever an expression may. They never
produce a value; reaching one transfers control instead, so there is
nothing for the surrounding expression to consume (§12).

### Operator precedence

From lowest to highest. Below item 1 sit assignment (`=`, lowest), then the
ternary `c ? a : b` (right-associative — `a ? b : c ? d : e` is
`a ? b : (c ? d : e)`), then `??`; so `a ?? b ? c : d` is `(a ?? b) ? c : d`
and `x = c ? a : b` is `x = (c ? a : b)`.

1. `||`
2. `&&`
3. `==`, `!=`, `<`, `<=`, `>`, `>=`
4. Binary `+`, `-`
5. Binary `*`, `/`, `%`, `@` (matmul)
6. Unary `+`, `-`, `!`, `~` (right-associative)
7. `**` (right-associative). Binds tighter than a unary prefix —
   `-2**2 == -4` — but the RHS of `**` itself accepts one, so
   `2 ** -1 == 0.5` and `2 ** -3 ** 2 == 0.00195…` both parse.
8. Call / index / dot (`x(...)`, `x[i]`, `x.k`) — left-associative

A unary prefix binds tighter than `*`, so `-a * b` is `(-a) * b`. For
numbers the two readings agree, but a type defining `__neg__` and
`__mul__` sees which one runs first:

```culebra
class N {
  new(v) {
    self.v = v
  }
  __neg__() {
    println('neg')
    N(-self.v)
  }
  __mul__(o) {
    println('mul')
    N(self.v * o.v)
  }
}
_ = -N(2) * N(3)
# => |
# neg
# mul
```

`=` is right-associative but appears only in `ASSIGNMENT`, not as a
general expression operator.

### Expression-oriented syntax

Most constructs are expressions that yield a value:

* `if { a } else { b }` yields the taken branch's value.
* `match x { ... }` yields the matching arm's body value (or `nil`).
* `cond { test => a, _ => b }` yields the body of the first truthy
  `test`, or `nil` if none match (see [`cond`](#cond)).
* `while` always yields `nil`.
* A block `{ ... }` in statement position is a `LEXICAL_SCOPE` and
  yields `nil`; in expression position `{` starts an `OBJECT` literal.
* The last expression evaluated in a sequence is the sequence's value.

---

## 4. Types

Culebra has exactly twelve types — the names `type_of` can return:

| Type         | Description                                                                          |
|--------------|--------------------------------------------------------------------------------------|
| `Nil`        | Single value `nil`                                                                   |
| `Bool`       | `true` or `false`                                                                    |
| `Long`       | 64-bit signed integer                                                                |
| `Float`      | IEEE 754 binary64 (double precision)                                                 |
| `String`     | Immutable heap-allocated byte string                                                 |
| `StringView` | Immutable zero-copy borrow of another string's bytes, produced by `slice` / `split` / `view` (§18.1) |
| `Array`      | Mutable ordered collection of values                                                 |
| `Object`     | Mutable map of hashable keys to values (String keys go through a fast shape path)    |
| `Function`   | Closure (function pointer + captures)                                                |
| `Tensor`     | N-dimensional numeric tensor with a lazy graph evaluated on CPU or GPU (see [stdlib §8](stdlib.md#8-tensor)) |
| `Tuple`      | Immutable, hashable sequence (`(1, 2)`) — usable as an Object/Set key                |
| `Set`        | Insertion-ordered collection of unique hashable values (`{1, 2, 3}`)                 |

Classes, enum variants, modules, iterators, and stdlib namespaces are
all `Object`s; `type_of` reports `'Object'` for them.

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
* `Array`, `Object`, `Tuple`, `Set`: compare by **value** (structural,
  recursing through elements). Only `Function` and
  `Tensor` compare by reference identity. (Value equality does not make
  arrays hashable — they still can't be Object/Set keys.)

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

* **Globals and builtins** (`inspect`, `min`, or any top-level binding)
  may always be shadowed, so writing `mut min = arr[0]` in a local
  function costs nothing.
* **Block-scope shadowing within the same function** is allowed. A
  `{ ... }` block may introduce a new `let`/`mut` binding of a name
  already declared in the enclosing function body:

        fn () {
          a = 0
          { let a = 1; ... }   # OK: new binding scoped to the block
        }

The restriction applies only at function boundaries: closure-captured
state is mutated via bare `x = v`, and a new local is declared with
`let`/`mut`.

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
* A `for` and a `while` body introduce a **fresh scope per iteration**. A
  binding introduced in the body neither leaks out of the loop nor persists
  across iterations (so a bare immutable `x = …` re-declares each pass rather
  than re-assigning), and a body `defer` fires at the end of every iteration.
  An `if` body shares the enclosing scope (it does not iterate, so nothing
  collides).
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

The three positions get different policies because each serves a
different purpose in the closure-as-object idiom:

* **Captured state is object state.** In the closure-based object
  pattern (see [`handbook.md` §9.2](handbook.md#92-the-closure-based-alternative)), an enclosing function's mutable
  binding is the object's private field. Accidentally shadowing it —
  typically by writing `mut x = ...` intending a new local — silently
  breaks the object. Making this a compile-time error is worth the
  small restriction.
* **Block scope is a computation staging area.** Inside a single
  function, a `{ ... }` block is a local calculation region.
  Rebinding a name there (`let a = transform(a)`) is a common,
  intentional pattern, not a bug. No reason to restrict it.
* **Globals form a shared vocabulary.** Builtins (`inspect`, `to_string`,
  `Math`, `IO`) and top-level names are understood to be ambient. Locals
  like `mut min = arr[0]` are an ergonomic idiom, not a confusion
  risk. Requiring renames would be friction without safety gain.

The effect is that the rule catches the bug class it is designed to
catch — confusion between "new local" and "outer reassignment" —
without interfering with the two situations where shadowing is
natural.

---

## 7. Expressions

### Arithmetic

`+`, `-`, `*`, `/`, `%`, `**` take two numeric operands (`Long` or
`Float`); a non-numeric operand raises `type error`.

* **Both `Long` → `Long`.** Integer arithmetic; division and modulo
  truncate toward zero (not floored, so the sign of the operands does
  not change the direction).
  `7 / 2 == 3`, `-7 % 3 == -1`. Overflow wraps (no bignum).
  For the floored remainder — the one that wraps a negative index into
  `0..n` instead of leaving it negative — use
  [`Math.wrap`](stdlib.md#mathwrapx-long-n-long---long).
* **Either operand `Float` → `Float`.** The `Long` operand is
  promoted to `Float` and the operation runs in IEEE 754 binary64.
  `1 + 2.0 == 3.0`, `3 / 2.0 == 1.5`.

Division or modulo by zero raises `divide by 0 error at L:C` for both
`Long / 0` and `Float / 0.0`.

`**` (exponentiation) has a slightly richer rule:

* `Long ** non-negative Long → Long` (integer exponentiation; wraps
  on overflow). `2 ** 10 == 1024`.
* `Long ** negative Long → Float`. `2 ** -1 == 0.5`. `0 ** -1`
  raises `divide by 0 error`.
* Either operand `Float` → `Float` (via `std::pow`). `2.0 ** 0.5 ≈
  1.4142`.
* `**` is right-associative and binds tighter than unary minus. Its
  RHS accepts a unary prefix, so both `2 ** 3 ** 4 == 2 ** 81` and
  `2 ** -1 == 0.5` parse.

### Bitwise

`|` (or), `&` (and), `^` (xor), `<<` / `>>` (left / arithmetic-right
shift), and unary `~` (complement) operate on **two `Long` operands**
(one for `~`); any non-`Long` operand raises `type error`. `>>` is an
arithmetic (sign-preserving) shift, matching `Long`'s signedness. Shifts
wrap like the rest of `Long` arithmetic (no bignum); the shift count is
taken modulo 64 (its low 6 bits), so `1 << 64 == 1` and a negative count
wraps the same way.

    0b1100 & 0b1010   # → 8
    12 ^ 10           # → 6
    0b1010 | 0b0101   # → 15
    1 << 4            # → 16
    ~0                # → -1
    5 & ~1            # → 4   (clear the low bit)
    READ | WRITE      # combine disjoint flag bits

Precedence: comparison `<` or `|` `<` xor `^` `<` and `&` `<` shift
`<<`/`>>` `<` additive `+`. So `1 << 2 + 1 == 8` (`1 << (2+1)`),
`2 & 3 ^ 1 == 3` (`(2 & 3) ^ 1`), and `1 | 2 == 3` (`(1 | 2) == 3`).
`~` binds at the unary level.

**`|` and `|...|` lambdas:** a bit-OR `|` works everywhere a normal
expression is parsed, including a lambda body (`|x| x | 1`). The one
exception is a *parameter default*, where a top-level `|` would be
ambiguous with the closing `|` of the parameter list — write it with
parentheses there: `|x = (A | B)|`. `||` remains logical-or.

### Comparison

* `==`, `!=`: any two values; see §5 for reference-type semantics
  and for numeric cross-type equality (`1 == 1.0`).
* `<`, `<=`, `>`, `>=`: same-typed operands, with the exception that
  `Long` and `Float` compare by numeric value (`1 < 1.5` works). Any
  other cross-type ordering raises `type error`.
* **Chaining**: `a < b < c` means `(a < b) && (b < c)` — the middle
  operand `b` is evaluated once, and the chain short-circuits to `false`
  at the first failing link. Any mix of comparison
  operators chains: `0 <= i < n`, `lo < x <= hi`, `a == b == c`.

### Logical

* `!x`: requires `x` convertible to bool.
* `x && y`: evaluates `x`; if falsy returns `x`, else evaluates and
  returns `y`. Short-circuit.
* `x || y`: evaluates `x`; if truthy returns `x`, else evaluates and
  returns `y`. Short-circuit.
* `x ?? y`: returns `x` if it is not `nil`, else `y`. Short-circuit
  (RHS not evaluated when LHS is non-nil). Lower precedence than `||`.
  Chains left-associatively: `a ?? b ?? c` = `(a ?? b) ?? c`.

### Null-safe access

These operators complement `??` and the `T?` Optional type (§14) for
working with possibly-nil values.

* `a?.b` / `a?.m(...)` — **optional chaining**: if `a` is `nil` the
  whole remaining chain short-circuits to `nil` (the property read or
  method call is skipped, including its arguments). Otherwise behaves
  like `a.b` / `a.m(...)`. `a?.b.c` short-circuits the entire `.c` too;
  use `a?.b?.c` to guard each link.
* `a?[k]` — **optional index**: `nil` receiver short-circuits to `nil`,
  otherwise indexes like `a[k]` (Array / Tuple / Object).
* `expr!!` — **non-null assertion**: passes `expr` through unchanged, or
  raises `NilError` if it is `nil`. Postfix, so it chains: `a!!.b`,
  `a!![0]`.
* `a ??= b` — **nil-coalescing assignment**: assigns `b` to `a` only
  when `a` currently reads as `nil`, short-circuiting `b` otherwise (`b`
  is not evaluated on the non-nil path). The target may be a plain
  variable, `obj.key`, `obj[k]` (including a class instance's
  `__index__`/`__setindex__` fallback), or `arr[i]`. An absent Object
  key reads as `nil` for this purpose, same as a plain `obj.key` read
  (§10) — so `obj[k] ??= v` inserts `k` when it is missing, honoring the
  usual mutable-by-default rule for a runtime-inserted key (§10) and the
  existing slot's `mut` flag when the key is already present but nil. An
  Array index does not auto-extend: an out-of-range `i` still raises
  `IndexError`. Not supported on a `FixedArray`/`SharedBuffer` element or
  a `@packable` packed field (no `nil` sentinel for a packed scalar), or
  on a `Shared.new` view (unconditionally immutable).

### Truthiness

Only `Bool`, `Long`, and `Float` are convertible to bool:

* `Bool`: itself.
* `Long`: `0` is false, all others true.
* `Float`: `0.0` (and `-0.0`) are false. Every other finite value is
  true, and `NaN` is true — truthiness asks "is this a number at all",
  not "is this a usable number".
* `Nil`, `String`, `Array`, `Object`, `Function`: **not convertible** —
  using one in a boolean context (e.g., `if s { ... }`) raises
  `type error`.

(This is intentionally strict. Wrap with an explicit check such as
`s != nil` or `!arr.empty()` if needed.)

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
  then the store is performed — the value is computed before the
  place it lands in is resolved.
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
A literal apostrophe is the one byte a `'...'` string cannot hold — use
a backtick string (below) when the content contains `'`. Both raw and
interpolated strings may span multiple lines (a newline in the source is
part of the value), so `'\d+'` is a ready-made regex literal and
`"...\n..."` a multi-line template.

### Backtick raw string literals

    `it's`              # holds a single quote
    `say "hi"`          # holds double quotes — no escaping
    `\d{4}-\d{2}`       # regex with quantifiers, verbatim

Backtick strings (Go-style) are **raw like `'...'` but may also contain
`'`, `"`, and `{`** — every byte between the backticks is verbatim, with
no escapes and no interpolation, and they may span multiple lines. The
only byte a backtick string cannot hold is a backtick itself. They are
the cleanest form for regex patterns that mix apostrophes and braces
(e.g. a GPT-2 pre-tokenizer: `` `'s| ?\p{L}+|\s+(?!\S)` ``), where neither
`'...'` (no apostrophes) nor `"..."` (interpolates `{...}`) fits.

### Regex literals

    re'\d+'             # a compiled Regex, same as Regex.compile('\d+')
    re"\d{4}-\d{2}"     # `re` makes the body raw, so {4} is a quantifier
    re`["']\w+`         # backtick body: holds both ' and "
    re"hello"i          # trailing flags (i / m / s)
    re"^${word}$"       # ${expr} interpolation (escaped / spliced, see below)

A `re'...'` / `re"..."` / `` re`...` `` literal evaluates to a compiled
[`Regex`](stdlib.md#14-regex) value — it is exactly `Regex.compile(<body>,
<flags>)` and supports the whole Regex object API, so it chains directly:

    re'\w+'.find("  hello world").value   # => "hello"
    re"hello"i.test("HELLO")              # => true

The body is **raw regardless of the quote** — the `re` prefix turns off
escape decoding, so `\d`, `\w`, and `{n}` quantifiers pass through verbatim
and the closing quote is the only delimiter.

The one structured form is **`${expr}` interpolation**. It uses `$`, not
the bare `{` of a normal `"..."`, precisely so it cannot collide with a
`{n}` quantifier (a `$` anchor is never quantified in practice):

    let word = "a.b"
    re"^${word}$".test("a.b")             # => true
    re"^${word}$".test("axb")             # => false — the `.` is escaped

Interpolation is **type-driven and injection-safe by default**:

  - a **String** (or any non-Regex value, stringified) is *escaped*, so it
    matches **literally** — metacharacters in the value lose their meaning;
  - a compiled **`Regex`** is spliced as a non-capturing group `(?:src)`,
    so its quantifiers and alternation **compose** into the pattern (the
    engine applies any inline flags it carries to the whole match).

```
let digits = re"\d+"
re"id=${digits};".test("id=123;")         # => true — composed
```

To write a literal `$` immediately before a `{`, use `\$` (the regex
escape for a literal dollar), which also suppresses interpolation:
`re"\${2}"` matches two dollar signs. Still build a **fully dynamic**
pattern with `Regex.compile(...)`:

    let n = 4
    Regex.compile('\d{' + n.to_s() + '}')

Optional trailing flag letters (`i` case-insensitive, `m` multi-line, `s`
dot-matches-newline) follow the closing quote: `re"^b"m`. There is no
`/.../ ` form: `/` is division, and disambiguating the two needs a
stateful lexer culebra deliberately avoids. `re` is only a regex prefix
when a quote immediately follows — otherwise it is an ordinary identifier
(`let re = 5` is fine).

### Triple-quoted strings

    """multi-line
    with "quotes" and 'apostrophes' and {interpolation}"""

`"""..."""` is interpolated like `"..."` (the same `{expr}` / `{x:spec}`
forms and `\n` / `\{` escapes) but single and double quotes inside need
no escaping — only `"""` closes it. Handy for embedded LLM prompts and
snippets that contain quotes. Use `\{` for a literal open brace.

#### Block form (dedented)

When the opening `"""` is immediately followed by a newline, the string is
a *block string*. The newline after the opening `"""` and the newline before
the closing `"""` are not part of the value, and every line is dedented by
the indentation of the closing `"""`, so the literal can be indented to match
the surrounding code without that indentation leaking into the string:

    let html = """
        <html>
        </html>
        """
    # => "<html>\n</html>"

The dedent is resolved once, when the literal is built (the JIT folds a
pure-literal block into a single constant) — there is no separate runtime
`trimIndent`-style pass over an already-allocated string. Relative indentation
beyond the closing delimiter is preserved, and blank lines are normalized to
empty. A non-blank line indented less than the closing `"""`, or a closing
`"""` that does not sit on its own line, is a `SyntaxError`.

A triple string whose content begins on the opening line (including the
single-line `"""..."""` form) is *not* a block string: it is taken raw, with
no dedent, exactly as before.

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

| escape       | byte / codepoint                                                 |
| ------------ | ----------------------------------------------------------------- |
| `\n`         | newline (0x0A)                                                     |
| `\r`         | carriage return                                                    |
| `\t`         | tab                                                                |
| `\\`         | backslash                                                          |
| `\"`         | double quote                                                       |
| `\{`         | literal `{` (does not start an interpolation)                      |
| `\xHH`       | one raw byte from two hex digits (`0x00`–`0xFF`)                   |
| `\uXXXX`     | a BMP Unicode scalar value (exactly 4 hex digits), UTF-8 encoded   |
| `\UXXXXXXXX` | a Unicode scalar value (exactly 8 hex digits), UTF-8 encoded       |

`\xHH` writes a raw byte — it can produce bytes that aren't valid UTF-8
(e.g. `"\xff"`), since a `String` is a byte string. `\uXXXX` / `\UXXXXXXXX`
write a codepoint: both reject values above `U+10FFFF` and the surrogate
range `U+D800`–`U+DFFF` (not Unicode scalar values); only `\U` reaches
beyond the BMP. A `}` does not need
escaping outside of an interpolation. An unknown `\X` is preserved
unchanged as two characters (`\` and `X`). An embedded NUL (`\x00` /
`\u0000`) is an ordinary byte: `size()` counts it and every String
operation preserves it — a `String` never terminates at a NUL.

### Format specs

An interpolation may carry a format spec after a colon — `{expr:spec}`.
The spec is the C++ `std::format` mini-language:
`[[fill]align][sign][#][0][width][.precision][type]`.

    "{pi:.2f}"        # → 3.14   (fixed-point, 2 decimals)
    "{5:.2f}"         # → 5.00   (a Long is coerced for a float spec)
    "{n:05}"          # → 00042  (zero-padded width)
    "{255:#x}"        # → 0xff   (hex with prefix)
    "{name:>10}"      # → right-aligned in 10 columns
    "{score:+}"       # → +42    (explicit sign)

Numeric values honor the spec's type char: a float type (`f`/`e`/`g`)
formats Longs and Floats as floating-point, an integer type
(`d`/`x`/`o`/`b`) formats as an integer. Other values format their
display string (so width / alignment apply). An invalid spec for the
value's type raises `ValueError`. `std::format` is followed exactly, so
`,` digit grouping is not supported (the locale `L` option covers that
case instead).

A spec may itself carry `{expr}` fields, so a width or a precision can be
computed rather than written out:

    "{s:>{w}}"        # right-aligned in `w` columns
    "{x:.{p}f}"       # `p` decimals
    "{x:{w}.{p}f}"    # both, in one spec

A field is an ordinary expression, evaluated where it stands (left to
right, after the value being formatted). It must be a `Long` — it is
spliced into the spec as decimal text, which is what a width or precision
is — and anything else is a `TypeError` at the field's own position. A
field means exactly what the same digits written by hand would mean, so a
width of `0` reads as the zero-pad flag rather than "no width": fine for a
number (`"{n:>{0}}"`), a `ValueError` for a string, just as `"{s:>0}"`
already is. It works anywhere an interpolation does:

```culebra
let w = 6
inspect(["a", "bb"].map(|s| "{s:>{w}}"))  # => ['     a', '    bb']
```

### Display conversion

When an expression inside `"..."` is not a `String`, its display form
is inserted:

* `Nil` → `nil`
* `Bool` → `true` or `false`
* `Long` → decimal
* `Float` → shortest round-trip decimal, always with a `.` or an
  exponent so the type is distinguishable from `Long`: `1.0`, `0.5`,
  `-2.5`, `1e-05`, `nan`, `inf`, `-inf`
* `Array` → `[v1, v2, ...]` with each element's `inspect` form (strings
  in brackets are quoted, e.g. `['hi']`)
* `Object` → `{key: val, mut key2: val2, ...}` in insertion order
* `Function` → `[function]`

String values inside `"..."` are inserted verbatim (no quotes); this
is the one place display differs from `inspect`.

Cyclic data (`a.c = a`) displays as `{...}` / `[...]` to avoid
infinite recursion.

### Concatenation

The `+` operator joins two strings into a new `String`:

    'foo' + 'bar'              # 'foobar'
    'a' + 'b' + 'c'            # 'abc'

`+=` appends in place (strings are immutable, so this rebinds the
variable / element / field to the joined result):

    let mut s = 'a'
    s += 'b'                   # s is now 'ab'

Both operands must be a `String` or `StringView`; the result is always
an owned `String`. Concatenating a string with a non-string (`'n: ' + 1`)
is a `TypeError` — use interpolation (`"n: {1}"`) or `to_string` to mix
types.

Arrays concatenate the same way (§9); no other type does.

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

**Spread.** A `...iterable` element splices another collection's
elements into the literal:

    [0, ...a, 4]             # → [0, <a's elements>, 4]
    [...a, ...b]             # concatenate

Spread sources are `Array`, `Tuple`, and `Set` (a non-iterable raises
`TypeError`).

**Concatenation.** `+` joins two Arrays into a new one, the same way it
joins two Strings (§8). Spread is the more general form — it takes
`Tuple` / `Set` sources and can splice into the middle of a literal —
while `+` reads better for the plain two-Array case and chains:

    a + b                    # new Array; a and b unchanged
    a += b                   # rebinds a to the concatenation
    [...a, ...b]             # same result as a + b

The copy is shallow: elements are shared with the operands, as with
`slice`. To append in place instead of building a new Array, use
`a.extend(b)` (§18.2).

### Access and mutation

    arr[i]            # index access, negative indices count from end
    arr[-1]           # last element
    arr[i] = v        # set; element must already exist (no auto-extend)
    arr.size()        # element count (Long)
    arr.push(x)       # append to end, returns nil

`arr[i]` out of range raises `index out of range at L:C`.

### Slicing

A range index (`seq[a..b]`, `seq[a..=b]`) returns a sub-sequence instead
of a single element. `..` is end-exclusive, `..=` end-inclusive; either
endpoint may be negative (counted from the end). Either endpoint may also
be **omitted** — an open start defaults to `0`, an open end to the
sequence length: `xs[2..]` drops the first two, `xs[..3]` keeps the first
three, `xs[..]` copies the whole sequence. Out-of-range endpoints
**clamp**, and a start past the end yields an empty result, so slicing
never raises on bounds.

Arrays return a **shallow copy** — the slice's spine is independent of
the source, but elements are shared (a reference-semantic array sliced
into a view would alias surprisingly, so copy is the safe default). For
a zero-copy lazy window over a large array, use the iterator instead
(`xs.iter().skip(a)`). Strings return a **byte-unit view** (Go-style
byte indexing; a slice that lands mid-codepoint keeps the raw bytes).
Tuples return a tuple.

A range is a **first-class value** (`let r = 1..3`) — store it, pass it
to a function, and use it to subscript later (`xs[r]`). A bounded range
is also iterable (`for i in 1..4`); an unbounded one (`2..`) has no
iteration end and raises if iterated.

```culebra
let xs = [10, 20, 30, 40, 50]
inspect(xs[1..3])    # => [20, 30]
inspect(xs[1..=3])   # => [20, 30, 40]
inspect(xs[-3..-1])  # => [30, 40]
inspect(xs[2..])     # => [30, 40, 50]
inspect(xs[..3])     # => [10, 20, 30]
```

```culebra
let xs = [10, 20, 30, 40, 50]
inspect(xs[1..100])  # => [20, 30, 40, 50]
inspect(xs[3..1])    # => []
let r = 1..3
inspect(xs[r])  # => [20, 30]
```

```culebra
# Shallow copy: mutating the source does not touch the slice.
mut xs = [10, 20, 30]
let s = xs[0..2]
xs[0] = 99
inspect(s)  # => [10, 20]
```

```culebra
inspect("hello"[1..3])   # => 'el'
inspect("hello"[1..=3])  # => 'ell'
```

### Equality and ordering

Arrays compare by **value** (structural): `[1, 2] == [1, 2]` is true,
recursing through nested elements. Tuples, Sets, and
Objects are likewise value-equal; only `Function` and `Tensor` keep
reference identity. (Arrays remain *unhashable* — value equality does
not make them usable as Object/Set keys.) Ordering operators (`<` etc.)
are not defined on arrays.

---

## 10. Objects

### Construction

    {}                                     # empty object
    {name: 'alice', age: 30}               # two properties
    {mut counter: 0, name: 'x'}            # explicit mut on a property
    {name, age}                            # shorthand — same as {name: name, age: age}

Property order in the source is irrelevant for equality or access,
but the order is preserved for display and iteration: keys come back
in the order they were first written.

The shorthand form `{x}` is equivalent to `{x: x}`: it reuses the
identifier as both key and value, looking up `x` in the current scope.
`mut` is allowed (`{mut n}`), which declares the property mutable while
the value still comes from the binding `n`.

**Spread / merge.** A `...obj` member copies another Object's entries
in; later keys win, so it doubles as config override:

    {...defaults, ...overrides}             # merge (overrides win)
    {...base, port: 8080}                   # override one field

Merged entries are mutable. Spreading a non-Object raises `TypeError`.

### Display conventions

The default formatter (used when an Object has no `__str__`) hoists a
String `class:` property to a prefix, and omits it from the property
list. So a class-sugar instance with fields `x`, `y` displays as
`Point {mut x: 3, mut y: 4}` instead of `{class: 'Point', mut x: 3,
mut y: 4}`. Plain objects that happen to carry a `class:` key get the
same treatment; a non-String `class` field is displayed as a regular
property.

### Enumeration

`keys()`, `size()`, `to_string()`, `for k, v in obj`, spread (`{...obj}`)
and `JSON.stringify` all enumerate the same set: the object's own
entries, in insertion order. For a class-sugar instance that is the
`class:` tag, then the declared fields in declaration order, then the
ones the constructor and methods set with `self.x = y`. Methods are
not own entries — they live on one per-class method table every instance
delegates to (see §25) — so they never appear:

    class P { new() { self.a = 1 }  m() { 2 } }
    let p = P()
    p.keys()                  # ['class', 'a']
    p.size()                  # 2
    p.to_string()             # 'P {mut a: 1}'  (class hoisted to the prefix)
    JSON.stringify(p)         # '{"class":"P","a":1}'
    {...p}.keys()             # ['class', 'a'] — a plain Object, no methods

`has(key)` is the exception: it answers the *lookup* question, so it also
sees class methods — but not dict builtins:

    p.has('m')                # true  — a class method
    p.has('size')             # false — a dict builtin, not an entry

The data accessors stay own-entry-only: `p.get('m', fallback)` returns
the fallback and `p['m']` raises `KeyError`. Assigning to a method name
(`p.m = 1`) creates an own field that shadows the method.

### Access and mutation

    obj.key              # read, returns nil if absent
    obj.key = v          # set (creates as mutable if absent; see below)
    obj[k]               # subscript read with a non-String key (see below)
    obj.size()           # property count (Long), includes non-String keys
    obj.has(key)         # Bool — accepts String, Long, Float, Bool, Nil, Tuple
    obj.keys()           # Array of keys in insertion order (interleaved)
    obj.remove(key)      # remove the entry for `key` (any hashable key); no-op if absent

Assigning to an *existing* property that was declared without `mut`
raises `immutable property 'key' at L:C`. A property created at
runtime by `obj.key = v` (or `obj[k] = v`, `get_or_put`, `to_object`,
`group_by`) is mutable by default — the same rule the `{...spread}`
merge already uses ("merged entries are mutable"). Only an Object
*literal*'s own keys are immutable by default (`{key: v}`; `mut key: v`
opts a literal key in — see Construction, above).

Dot-form names (`obj.key`) are identifiers (`[A-Za-z_][A-Za-z0-9_]*`)
that go through the fast shape path. Non-identifier keys reach the
subscript path below.

A key may also be a **string literal** — `'...'`, a backtick string, or a
`"..."` with no interpolation — which is how you write keys that aren't
identifiers (hyphens, spaces, etc.):

    let headers = {"User-Agent": "curl", "X-Trace-Id": id}
    headers["User-Agent"]    # read non-identifier keys with [ ]

A key must be a compile-time constant, so an *interpolating* `"...{x}..."`
is a SyntaxError; build a dynamic key with `o[k] = v` instead.

### Non-String keys

In addition to String keys (`{name: 'alice'}`), Object literals accept
`Long`, `Float`, `Bool`, `Nil`, and `Tuple` keys:

    let grid = {(0, 0): 'origin', (1, 0): 'east'}
    let by_id = {1: 'one', 2: 'two', 3.14: 'pi'}
    let by_nil = {nil: 'unknown'}

Non-String keys live in a sidecar map and are read with `obj[k]`:

    grid[(0, 0)]   # 'origin'
    by_id[1]       # 'one'

Key identity is type-strict, deliberately *finer* than the `==`
operator. Keys of different types are never the same key, so
`1`, `1.0`, and `true` are three distinct keys even though `1 == 1.0`
holds for the `==` operator. A literal that *repeats* a key keeps the
last write —
`{1: 'a', 1: 'b'}` is `{1: 'b'}` — the same last-wins rule a literal
always uses for a duplicated key, including String keys.

String and non-String keys share a single insertion-order record, so
`to_string` renders mixed-key Objects with the keys interleaved in
their actual write order.

### Subscript assignment

`obj[k] = v` writes any hashable key — `Long`, `Float`, `Bool`, `Nil`,
`Tuple`, or a runtime `String`:

    mut bag = {}
    let k = 'alpha'
    bag[k]     = 1                  # runtime String key
    bag[42]    = 'long'
    bag[(1,2)] = 'tuple'
    inspect(bag[k])                    # 1
    inspect(bag[(1,2)])                # 'tuple'

String keys unify with the shape-based `obj.foo` path — both forms
reach the same slot:

    mut o = {}
    o['x'] = 1
    inspect(o.x)                       # 1
    o.y = 2
    inspect(o['y'])                    # 2

A key created at runtime is mutable, so writing it again succeeds
(unlike a same-named Object *literal* key, which stays immutable
without an explicit `mut`):

    mut tally = {}
    for w in ['a', 'b', 'a'] {
      tally[w] ??= 0    # insert 0 on the first sighting (§7, nil-coalescing assignment)
      tally[w] += 1
    }
    inspect(tally)                      # {a: 2, b: 1}

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

0. If `receiver` is a built-in namespace (`IO`, `Math`, …), only its own
   members resolve — see [Namespaces are closed](#namespaces-are-closed)
   below. UFCS is off for such a receiver.
1. If `receiver` exposes a property or built-in method named `name`,
   invoke it. For `Object` / `Array`, user-defined properties win
   over built-ins; built-ins fill in otherwise. String methods are
   the only choice for `String` receivers. A default implementation from
   a trait the receiver's class conforms to (§14) counts as one of its
   methods here, so it resolves at this step rather than falling
   through to UFCS. When the resolved value is a `Function`, `self`
   is bound to `receiver` for the duration of the call.
2. Otherwise, if a free function named `name` is visible in the
   enclosing scope, call it with `receiver` as the first argument and
   the remaining arguments as-is. This is **Uniform Function Call
   Syntax (UFCS)**, matching the D / Nim convention.
3. Otherwise the property lookup returns `nil`; the subsequent call
   fails with a type error.

```culebra
o = {n: 10, add: fn (x) {
  x + self.n
}}
# A receiver call binds self, so `add` reads o.n:
inspect(o.add(5))  # => 15

double = fn (x) {
  x * 2
}
42.double()  # UFCS → double(42) → 84

word_count = fn (s) {
  s.split(' ').size()
}
'hello world'.word_count()  # UFCS → 2

# Existing methods always win — a user-defined `size` is shadowed by
# the Array/Object/String built-in `size`.
size = fn (x) {
  99
}
[1, 2, 3].size()  # 3 (builtin), not 99
```

UFCS only fires when DOT is immediately followed by an argument list;
bare property access (`x.name` without `()`) never uses UFCS. A UFCS
invocation does **not** bind `self` to the receiver — the call is
semantically a free-function call with the receiver in the first
positional slot.

Two names dispatch outside the built-in tables and still follow this
order. A class instance's synthesized `parameters()` (§10) is a property
for step 1, so a global `parameters` never claims one. The explicit-drop
form `x.drop()` (§17) sits *below* step 2 instead: a receiver carrying
no `drop` of its own hands the call to a free `drop` in scope, and only
a receiver that resolves the name — a handle with its own `drop`, or no
candidate in scope at all — reaches the at-most-once guard.

#### Built-in methods bind positionally

A built-in method (`Array` / `String` / `Set` / `Tuple` / dict /
iterator / `Tensor`) binds its arguments **by position**. The one name a
keyword argument may carry is a *keyword-only* parameter — one with no
positional slot at all, like `sorted`'s `reverse:`. A keyword naming an
ordinary parameter is a `TypeError`, even where that parameter exists,
and a `**` splat is never accepted:

```culebra
# doctest: skip
[3, 1, 2].sorted(reverse: true)     # → [3, 2, 1]  (keyword-only)
'hello world'.truncate(8)           # → 'hello...'
'hello world'.truncate(max: 8)      # TypeError: built-in method 'truncate'
                                    # does not accept keyword arguments
[3, 1].sorted(**{'reverse': true})  # the same TypeError
```

The rule is about the *callee*, not the name: a user-defined method of
the same name is an ordinary function, so `doc.truncate(max: 8)` binds
its keyword normally. It is also about a receiver that **resolves** the
name — the property is read before any argument binds, so
`[1, 2].take(n: 1)` (no `take` on `Array`) is the ordinary method miss
rather than the keyword error.

#### The argument list runs before anything binds

`x.m(a, b)` reads the property, evaluates **every** argument left to
right, and only then binds them. So a wrongly typed argument and a
method the receiver does not have are both reported *after* the whole
list has run:

```culebra
# doctest: skip
'abc'.truncate(bad(), loud())   # both run, then the `max` type error
'ab'.push(loud())               # loud() runs, then "expected Function, got Nil"
```

The one thing that fails earlier is a scalar receiver: `nil`, `Bool`,
`Long` and `Float` carry no members at all, so `(5).push(loud())` fails
at the property read and `loud()` never runs.

#### Namespaces are closed

A built-in namespace exposes a fixed member set, so an unknown member is
a typo or a removed API rather than an extension point. Reading one
raises `AttributeError: namespace 'IO' has no member 'zzz'`, and UFCS
does not step in — a free function named `zzz` cannot fill the gap:

```culebra
# doctest: skip
zzz = fn (ns, v) {
  v
}
IO.zzz(9)         # AttributeError, not zzz(IO, 9)
Math.to_string()  # AttributeError, not to_string(Math)
```

The dict built-ins are the exception: `IO.keys()`, `Sys.has('script')`
and the rest of the `Object` table still answer, because a namespace is
an `Object`. Plain objects are unaffected — `{a: 1}.zzz(9)` is UFCS as
usual.

Writing a namespace member follows the same fixed-set rule: `Ns.zzz = v`
and `Ns.zzz ??= v` raise the identical `AttributeError` rather than
minting a phantom member (`Ns.zzz += v` already raised on a missing
property under the general compound-assignment rule below). Writing an
*existing* member follows the ordinary assignment rules for that slot —
most stdlib namespace members are registered immutable, so the common
outcome there is `ImmutableError`, not a silent overwrite:

```culebra
# doctest: skip
Math.zzz = 1   # AttributeError: namespace 'Math' has no member 'zzz'
Math.pi = 3.0  # ImmutableError: immutable property 'pi'
```

A bare read of a **built-in** method (`let m = [1, 2].map`, no parens)
is not a first-class value — it raises `TypeError: built-in method
'map' cannot be used as a value (call it, or wrap it in a lambda)`.
The reject applies only when the receiver's own built-in table carries
the name; a built-in-*named* property the receiver simply lacks reads
as `nil` like any other miss (`{a: 1}.map`, or `C.join` on a class
object whose `join` is an instance method). User-defined methods read
as bound, first-class values (see `self` below).

`self` resolves in two steps. A call that supplied a receiver binds it
for the duration of the call, and that dynamic binding always wins. A
body reached without one — a plain `f(x)`, a UFCS invocation, a
function handed to a built-in like `map`, a `defer` block — falls back
to the `self` of the lexically enclosing function, walking outward as
far as needed, so a nested `fn`, lambda, or closure returned from a
method keeps seeing the method's receiver. Only when neither a
receiver nor an enclosing `self` exists does the read raise
`NameError: undefined variable 'self'`, exactly as any other unbound
name does, and at the point of the read rather than on entry to the
body.

Reading a function-valued property *as a value* (`let m = o.f`, no
call parens) returns a **bound method**: a fresh wrapper that carries
`o` as its `self`. The binding is permanent — attaching the wrapper to
another object and calling it as a method still runs with the original
receiver — and each read mints a new wrapper, so `o.f == o.f` is
`false`. This applies uniformly to object-literal properties, class
methods, statics, and constructors (`let mk = C.new`). Inside a body,
the implicit recursion handle `fn` refers to the value the call was
made through, so in a method-invoked frame `fn` is the bound wrapper —
recursing through `fn(...)` or returning `fn` keeps the current
receiver.

**JIT**: UFCS is supported under `--jit`. Resolution happens at
runtime: if the receiver carries a property by that name the method
path wins, otherwise the name is looked up as a free function and
invoked with the receiver as its first argument.

### `class` sugar

Closure-based objects remain the canonical OO idiom. The `class` form
is a lightweight alternative that desugars to the same runtime shape:

    class Car {
      new(mpr)  { self.miles = 0; self.mpr = mpr }
      run(n)    { self.miles = self.miles + self.mpr * n }
      total()   { self.miles }
    }
    c = Car.new(5); c.run(3); inspect(c.total())
    inspect(c.class)            # 'Car' — nominal tag for match / debugging

Semantics:

* The decl binds `Car` to an `Object` with a single `new` property.
* `Car.new(...)` returns a fresh `Object` carrying `class: 'Car'` plus
  the fields the constructor sets. `self` is bound to that object for the
  duration of the constructor body. Methods are not copied onto the
  instance: every non-`new` method lives on a single per-class method
  table each instance delegates to, so `c.run` resolves through it while
  `c.keys()` reports only `class` and the fields (see *Enumeration*).
* Fields created via `self.x = y` inside constructors and methods are
  **mutable by default** (unlike bare `o.x = y`, which creates an
  immutable property). This matches the idiom of classes whose methods
  routinely mutate instance state.
* The `new` method is optional; without it the class accepts no
  arguments and returns an instance carrying only `class:`.
* `self` is immutable inside the constructor body. Attempting
  `self = newObj` raises `ImmutableError`. The constructor always
  returns the originally allocated
  instance — an explicit `return value` discards `value`. Identity-swap
  factories live as `static` methods (below) or as plain top-level
  functions.
* Methods prefixed with `static` live on the class object itself
  (not on instances), providing class-as-namespace for factories,
  constants-as-functions, and helpers. `Shape.create(...)` resolves
  via the usual property-access mechanism; the receiver is the
  class object, not an instance, so `self` is unavailable inside
  a static body:

      class Shape {
        new ()                  { self.kind = 'unknown' }
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
      inspect(c.kind)              # 'circle'

  Static methods are not visible through instances (`c.circle(...)`
  raises `TypeError` because the instance has no `circle` property):
  a static is reached through the class, never through an instance.
* `static NAME = EXPRESSION` declares a class-level immutable constant
  (a static field), evaluated eagerly at class declaration time and
  installed as a property of the class object alongside static methods:

      class Circle {
        new (r)         { self.radius = r }
        static PI       = 3.14
        static MAX      = 100
        area ()         { self.radius * self.radius * Circle.PI }
      }
      inspect(Circle.PI)         # 3.14
      inspect(Circle.MAX)        # 100

  The value expression can be arbitrary (`static SUM = [1,2,3].sum()`),
  evaluated in the enclosing scope at class declaration time. Like
  static methods, static fields are immutable (`Circle.PI = 2` raises
  `ImmutableError`) and not visible through instances.
* `NAME = EXPRESSION` / `NAME: Type` / `NAME: Type = EXPRESSION`
  declares an **instance field** — mutable per-instance state
  initialized before the `new` body runs. The type annotation is
  optional, as everywhere else in the language:

      class Player {
        score = 0
        name:  String
        tags  = []
        best  = self.score + 10
        new (name) { self.name = name }
      }
      let p = Player.new('rocci')
      inspect(p.score)           # 0
      inspect(p.best)            # 10
      p.tags.push('bird')     # each instance gets its own Array

  Initializer semantics:

  - **Per instance**: the initializer expression runs once for every
    `C.new(...)` call — `tags: Array = []` gives each instance an
    independent Array — nothing is shared between instances.
  - **Declaration order, `self` in scope**: an initializer may read
    fields declared above it and call methods (`best` above). Reading a
    field declared *below* yields `nil` — order is meaningful.
  - **After argument binding, before the `new` body**: initializers see
    a fully bound call — an arity/type/default error on the ctor call
    fires first, with no field side effects — and the `new` body in
    turn sees every declared field. Constructor parameters are *not*
    visible inside initializers (initializers close over the class's
    defining scope, not over the constructor call); pass ctor args to
    fields
    explicitly with `self.x = a` in the body.
  - A typed field without an initializer (`name: String` above) takes
    its type's zero value: `0` / `0.0` / `''` / `false`; reference
    types (`Array`, `Object`, ...) default to `nil`. The untyped form
    always carries an initializer (there is no type to infer a zero
    value from).
  - Declared fields are mutable instance state, exactly like fields
    created via `self.x = y` in the constructor.

  The declared type is documentation (like parameter annotations on the
  runtime-check model, §14); `@packable` classes (§21) additionally read
  it to compute their fixed byte layout, so their fields must be typed
  (`x = 7` in a `@packable` class is a SyntaxError).
* `get NAME () { ... }` declares a **getter** — a no-parameter method
  that is invoked on a bare property read, with no call parentheses:

      class Circle {
        new (r)      { self.radius = r }
        get area ()  { self.radius * self.radius * 3.14 }
        get name ()  { "circle" }
      }
      let c = Circle.new(4)
      inspect(c.area)          # 50.24  — reads like a field
      inspect(c.area())        # 50.24  — the call spelling also works

  A getter reads `self` like any method but presents as a property, so a
  fluent chain drops its parentheses (`p.parent.name` rather than
  `p.parent().name()`). Reserve getters for pure, total, O(1) derivations
  (an inherent quality of the value); anything that does I/O or can fail
  should stay an ordinary method, so the absence of parentheses signals
  "no side effects". A getter takes
  no parameters (`get f (x)` is a syntax error), and `get` is contextual:
  a member literally named `get` (`get () { ... }`) is still an ordinary
  method. Both spellings (`obj.name` and `obj.name()`) invoke the getter
  identically on the interpreter, the JIT, and AOT.
* Both the interpreter and the JIT compile classes. Instance
  construction is a small runtime call — `new` itself is a regular
  JIT closure whose captures are the method closures plus the user's
  `new` body, and a runtime helper wires them into the fresh object.
* Well-known methods like `drop` (§17) can be written as ordinary
  class methods. Under the JIT, methods are held on a shared per-class
  meta object via prototype delegation, but the auto-drop lookup walks
  the proto chain — `class C { drop() { ... } }` fires as expected
  with `self` bound to the instance.

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
      new(x, y)   { self.x = x; self.y = y }
      __add__(r)  { Vec.new(self.x + r.x, self.y + r.y) }
      __mul__(r)  { Vec.new(self.x * r, self.y * r) }    # scalar
      __eq__(r)   { self.x == r.x && self.y == r.y }
    }
    a = Vec.new(1, 2)
    b = Vec.new(3, 4)
    c = (a + b) * 2           # Vec(8, 12)

**Trait-method fallback.** When a class does not define the explicit
`__eq__` / `__lt__` dunders, the comparison operators fall back to the
`Eq` and `Comparable` trait methods: `==` / `!=` route through
`eq(other)`, and `<` / `<=` / `>` / `>=` derive from `cmp(other)` (the
canonical `Comparable` method). This is what makes a `@derive(Eq,
Comparable)` (or hand-written `eq` / `cmp`) class usable with operators
directly — `a == b` and `a < b` work without writing `__eq__` / `__lt__`.
Precedence is **explicit dunder > trait method > default** (structural
equality for `==`; a `type error` for ordering an `Object` with neither).
Routing `==` through `eq` also keeps the operator consistent with the
key equality used by `Object` / `Set` lookups.

**Subscripting.** A class instance can define `__index__(key)` and
`__setindex__(key, value)` so `obj[k]` and `obj[k] = v` delegate to it —
a user collection wrapper then subscripts like a built-in. These fire
for keys the object doesn't hold as a direct property (so named-field
access `obj["field"]` still reads the field); slicing a user type
(`obj[a..b]`) is not routed through `__index__`. Both backends dispatch
identically.

```culebra
class Grid {
  new() {
    self.d = [10, 20, 30]
  }
  __index__(i) {
    self.d[i]
  }
  __setindex__(i, v) {
    self.d[i] = v
  }
}
let g = Grid.new()
g[1] = 99
inspect(g[1])  # => 99
```

**Calling (`__call__`).** A class instance can define `__call__(*args)`
so `obj(args)` invokes it — the twin of `__index__`. `obj(x)` is exactly
`obj.__call__(x)`, with the instance bound as `self`. This gives the
`model(x)` idiom for layered/composable values (a model whose `__call__`
runs its sub-layers' `__call__`). Like the subscript hooks it fires only
on class instances, so a plain dict holding a `__call__` key stays an
ordinary value. Both backends dispatch identically.

```culebra
class Adder {
  new(b) {
    self.b = b
  }
  __call__(x) {
    self.b + x
  }
}
let add3 = Adder.new(3)
inspect(add3(10))           # => 13
inspect(add3.__call__(10))  # => 13
```

A class with `__call__` structurally satisfies the `Function` type, so a
callable instance is a first-class function value: pass it to a
higher-order builtin (`map` / `filter` / `reduce` / `sort_by`) or bind it
to any `Function`-annotated parameter, and it is invoked through its
`__call__`.

```culebra
class Scale {
  new(k) {
    self.k = k
  }
  __call__(x) {
    x * self.k
  }
}
inspect([1, 2, 3].map(Scale.new(10)))  # => [10, 20, 30]

fn apply_twice(f: Function, x) {
  f(f(x))
}
inspect(apply_twice(Scale.new(2), 5))  # => 20
```

`__call__` accepts the full call form — positional, `*args`, and keyword
arguments — so `obj(x: 1)` binds `x` against `__call__`'s parameters like
any method call.

### Custom string representation (`__str__`)

Defining a 0-arg `__str__` method on an `Object` lets the value
supply its own display form for `inspect` / `print` / `println`, string
interpolation (`"{x}"`), and `to_string(x)`. The method must
return a `String`; anything else is a type error. `Object`s
without `__str__` use the default formatter (`{key: value, ...}`).

    class Matrix {
      new(r, c)  { self.rows = r; self.cols = c }
      __str__()  { "Matrix {self.rows}x{self.cols}" }
    }
    m = Matrix.new(2, 3)
    inspect(m)                     # Matrix 2x3
    inspect("shape: {m}")          # 'shape: Matrix 2x3'
    to_string(m)                   # 'Matrix 2x3'

The dispatch is top-level only: when `__str__` is involved inside a
nested structure (`inspect([m])`), the outer formatter's default
recursive walk still uses `str()` for each element, so the custom
form only appears for the operand passed directly to the display
hook. Deeper customization can be layered on once a concrete need
arises.

`__str__` bodies should **not** recursively invoke `inspect(self)` or
interpolate `"{self}"` — there is no built-in recursion guard, so
that form loops until the call stack is exhausted. Produce the
final string via direct property access (`self.x`) instead.

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
          n: Long => Vec.new(self.x * n, self.y * n),   # scalar
          _       => Vec.new(self.x * r.x, self.y * r.y) # elementwise
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

    class Value { new(x) { self.x = x } }
    class GPT {
      new() {
        self.layers = range(2).map(|_| {
          W: range(4).map(|_|
            range(4).map(|_| Value.new(0.0)).collect()
          ).collect()
        }).collect()
        self.wte = range(8).map(|_| Value.new(0.0)).collect()
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

Equality is element-wise (and `Array` is value-equal too, so nesting
recurses):

    (1, 2) == (1, 2)                 # true
    ([1], 2) == ([1], 2)             # true  (inner Arrays are value-equal)

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
| `empty()`     | `Bool` — is `size()` zero?                         |
| `presence()`  | The tuple unchanged if non-empty, else `nil`       |
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

**Parallel / swap assignment.** Dropping `let` reassigns *existing*
variables instead of declaring new ones — the RHS is fully evaluated
before any binding, so swaps and rotates need no temporary:

    mut a = 1
    mut b = 2
    (a, b) = (b, a)              # swap → a == 2, b == 1
    (x, y, z) = (y, z, x)        # rotate

Each target behaves exactly as it would on its own: an existing binding
is reassigned (an immutable one raises `ImmutableError`), and a name
that is not visible is declared, just as bare `x = v` does. Array and
object patterns work too: `[p, q, _] = xs`, `{g, h} = rec`.

The pattern is matched against the value before any target is written,
so a shape mismatch (`ValueError`) writes nothing — `[a, 5] = [9, 6]`
leaves `a` alone rather than assigning `9` and then failing on the `5`.
An `ImmutableError` is a different failure: it is raised as that target
is written, so targets to its left already hold their new values.

**Index and property targets.** A target may also be an index or
property chain, so swapping elements needs no temporary either:

    (p[i], p[j]) = (p[j], p[i])              # swap two elements
    (m[0][1], m[1][0]) = (m[1][0], m[0][1])
    (self.front, self.back) = (self.back, self.front)
    (a, o.count) = f()                       # names and chains may mix

A chain target writes through the same rules as the single-target form
(`p[i] = v`, `o.a = v`) — including `__setindex__` on a class instance,
and including the fact that an immutable *binding* holding a mutable
container still permits `xs[0] = v`.

The right-hand side is evaluated once, must be an `Array` or `Tuple`,
and must hold exactly one element per target; anything else raises
`ValueError`. Its elements are read before any target is written, so a
target may safely refer to the right-hand side itself
(`(p[1], p[0]) = p`), and a mismatch writes nothing at all. Targets are
then written left to right, each evaluating its own receiver and index
at that point.

Targets are a flat list: nested patterns and `...rest` belong to the
pattern forms above and do not mix with chains. A call is never a
target (`f() = v` is a `SyntaxError`).

---

## Sets

A `Set` is an insertion-ordered collection of unique hashable values.
Literal syntax uses braces with at least two elements (or a trailing
comma) so it doesn't collide with the empty-Object literal `{}` or the
`{key: value}` Object shorthand:

    let s   = {1, 2, 3}
    let one = {42,}                  # trailing comma forces a 1-element Set
    let mixed = {1, 'two', 3.14, true}

Duplicate elements collapse on construction. Membership uses the same
key identity as Object keys, which distinguishes types rather than
numeric value — so `{1, 1.0, true}` keeps all three elements, exactly
as those three are three distinct Object keys.

Equality ignores order:

    {1, 2, 3} == {3, 2, 1}           # true

Built-in methods:

| Method         | Description                                        |
|----------------|----------------------------------------------------|
| `size()`       | Element count (`Long`)                             |
| `empty()`      | `Bool` — is `size()` zero?                         |
| `presence()`   | The set unchanged if non-empty, else `nil`         |
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
through methods for consistency. Operators otherwise stay reserved for
math types (Long, Float, Tensor, user numeric classes via `__add__`
etc.); the one exception is `+`, which also concatenates strings (§8)
and Arrays (§9). A `Set` has no `+` — `to_array()` first, or `union`.

---

## 11. Functions and closures

### Function literals

    fn () { nil }
    fn (x) { x + 1 }
    fn (mut x) { x = x + 1; x }
    fn (a: Long, b: Long) -> Long { a + b }
    fn (name, greeting = 'hello') { "{greeting}, {name}" }

Functions are first-class values and the only way to define a reusable
piece of code. Bind a literal like any other value
(`name = fn (...) { ... }`), or use the declaration form
`fn name(...) { ... }`. Only the declaration form can be a
[generator](#generators-yield) or take part in [multimethod
dispatch](#20-multimethods), and only it gives `fn.name` a source-level
name.

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
* Self-reference via `fn` works the same as in a named function.
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
  the function's definition environment, extended with the bindings the
  frame makes before its parameters — the receiver `self`, the recursion
  handle `fn`, and any earlier parameter — so both `fn (a, b = a + 1)`
  and `m(k = self.n)` work. In a constructor the instance already exists
  but its field initializers have not run yet, so `self`'s fields read
  `nil` there. Default parameters must follow all required parameters.
* A parameter may be a **destructuring pattern** — `fn ({x, y})`,
  `fn ([a, b])`, `fn ((k, v))`, and the lambda form `|{a, b}|`. The
  argument is matched against the pattern at entry and its names are
  bound in the body; a shape mismatch raises `ValueError`. Patterns
  nest (`fn ({user: {name}})`) and mix with normal params
  (`fn (factor, {x, y})`).

      fn dist({x, y}) { x * x + y * y }
      dist({x: 3, y: 4})          # → 25
* A final parameter written `*name` is a **positional catch-all**: it
  collects every positional argument beyond the regular params into an
  `Array` (empty when there are none). It must be the last parameter.

      fn f(first, *rest) { [first, rest] }
      f(1, 2, 3)                  # → [1, [2, 3]]
      f(1)                        # → [1, []]

  A `*args` declaration also opts the function into **variadic
  dispatch**: it matches any call with at least the regular-param count,
  but a fixed-arity overload always wins a tie, and among variadic
  candidates the one with more regular params is the more specific.

      fn h(x: Long) { "exact" }
      fn h(*xs)     { "variadic" }
      h(1)                        # → 'exact'  (fixed-arity wins)
      h(1, 2)                     # → 'variadic'

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
* Unknown kwarg names are a `TypeError` unless the callee declares a
  `**rest` catch-all, which absorbs them.
* A `**` splat operand must be an `Object` with `String` keys only.
* Defaults are re-evaluated on every call, so a mutable default
  (`fn f(xs = [])`) is a fresh value each time rather than one
  accumulating across calls.

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

### Recursion: `fn`

Within a function body, `fn` refers to the function value currently
being executed. Recursion without giving the function a name:

    fib = fn (x) { if x < 2 { x } else { fn(x - 1) + fn(x - 2) } }

### Methods: `self`

`self` is bound for the duration of a method call. Outside a method
call, `self` is not in scope; accessing it raises
`undefined variable 'self'`.

### Closures

Function literals capture their lexical environment by reference.

* Variables read inside a nested function look up the scope chain
  until found.
* `x = v` inside a nested function reassigns an outer binding if `x`
  exists in an outer scope; otherwise creates a new local.
* Captured mutable variables (e.g., via `mut x` in an outer scope) are
  shared: changes are visible to every closure that captured them and
  to the outer scope itself — capture is by reference, not by value.

Example:

    make_counter = fn () {
      mut n = 0
      fn () { n = n + 1; n }
    }
    c1 = make_counter()
    c2 = make_counter()
    inspect(c1())   # 1
    inspect(c1())   # 2
    inspect(c2())   # 1, independent

In the JIT, captured mutable variables are allocated in heap **cells**
so that multiple closures can share the same slot. See §17.

### Generators (`yield`)

A `fn` declaration whose body contains `yield` is a **generator
function**. Calling it does not run the body: it returns an Iterator
(§18.5), and the body advances one `yield` at a time as the consumer
pulls values out of it.

```culebra
fn counter() {
  yield 1
  yield 2
  yield 3
}
inspect(counter().collect())  # => [1, 2, 3]
```

`yield expr` is a **statement**, not an expression. It hands a value to
the consumer and evaluates to nothing itself, so `let x = yield 1` is a
syntax error. Values travel out of a generator only — there is no
`send()` and no way to resume a suspended body with a value.

`yield from expr` delegates to any iterable — an `Array`, a `String`,
a lazy chain, or another generator — yielding each of its elements
before the delegating body resumes:

```culebra
fn inner() {
  yield 'x'
}
fn mixed() {
  yield from [1, 2]
  yield from inner()
  yield 'done'
}
inspect(mixed().collect())  # => [1, 2, 'x', 'done']
```

Recursive traversals compose from it, which is the usual reason to
reach for delegation:

```culebra
fn walk(node) {
  yield node.value
  for kid in node.kids {
    yield from walk(kid)
  }
}
let leaf = {value: 3, kids: []}
inspect(walk({value: 1, kids: [{value: 2, kids: [leaf]}]}).collect())  # => [1, 2, 3]
```

**The generator object.** What the call returns is an ordinary
Iterator: it carries `iter` / `has_next` / `next` / `dispose`, so it
drives `for`-in, the lazy combinator set (§18.5), and any other
consumer of the protocol. Suspension makes unbounded sources practical:

```culebra
fn nat() {
  mut i = 0
  while true {
    yield i
    i += 1
  }
}
inspect(nat().map(|x| x * x).take(5).collect())  # => [0, 1, 4, 9, 16]
```

The body starts on the first `has_next()`, not at the call. A generator
is **one-shot**: once drained it stays exhausted, and iterating the same
object again produces nothing. Call the function again for a fresh run.

**Inside the body**, `yield` may appear at any depth in `while`, `for`
and `if` bodies, and `break` / `continue` / `return` behave as they do
in a normal function — `return` ends the generation. A `defer`
registered before a suspension point runs when the generator is
**disposed**: loop exit, `break`, an early `return`, an exception, or a
terminal method finishing the drain (§18.5). Cleanup is therefore tied
to the consumer's exit path, not to the body reaching its end.

```culebra
fn two() {
  defer {
    inspect('closed')
  }
  yield 1
  yield 2
}
for v in two() {
  inspect(v)
  break
}
# => |
# 1
# 'closed'
```

**Restrictions.**

* `yield` may not appear inside a `try` / `catch` or a `defer` block.
  The parser rejects it with `SyntaxError: yield cannot appear inside a
  try-catch or defer block.` To guard a yielded value, put the `try`
  in the expression (`yield try { ... } catch e { ... }`); to clean up,
  use a `defer` at the top level of the body as above.
* Only `fn name(...) { ... }` **declarations** are transformed into
  generators — at the top level or nested inside another function. A
  `yield` anywhere else — in a class method, in an object property's
  function, in a `fn` expression assigned to a variable, or at the top
  level of a file — is rejected at parse time:

      SyntaxError: yield can only appear inside a `fn name(...) { ... }`
      declaration body — a class method, an object property's function,
      or a fn expression cannot be a generator. Declare a named fn and
      call it instead.

  The check runs over what the transform pass leaves behind, so every
  backend rejects the same programs at the same position.
* `self` may not be referenced in a generator's body. The body is
  lowered into methods of a synthesized state class, so a bare
  `self` there could only name that internal object — never a receiver
  (a generator is a named fn and cannot be a method). The parser
  rejects it:

      SyntaxError: self is not available inside a generator body (a
      function that uses yield) — bind it outside first (let me = self)
      and use that variable, or pass it as a parameter.

  The rule reaches a `fn` / lambda **defined** in the body, which would
  otherwise read that state object through the enclosing method's
  `self`. One shape is exempt: a function that is an object property
  (`yield {m: fn () { self.x }}`), whose `self` is the dynamic receiver
  of the object it is called on (§10), never the state object. A
  property name, object key, or kwarg label spelled `self` is not a
  reference either. To reach an enclosing receiver, capture it first:
  `let me = self` outside the generator, then use `me` inside. An
  `effect fn` body is the same shape and refuses the same way; a
  `handle` body is not — it is spliced where it was written, so an
  enclosing method's `self` is still that method's receiver (§16).
* A body local keeps plain-variable semantics even though the lowering
  stores it on the state object: a local holding a function is a value,
  not a method of that object, so `f == f` stays true, calling it (`f()`)
  passes no receiver exactly as it would outside a generator, and passing
  it on leaves the receiver to whatever call follows (`holder.f = f`,
  then `holder.f()` sees `holder`). The generator's own protocol methods
  are not locals and bind as usual (§10). An `effect fn` body and a
  `handle` body promote their locals the same way, so this rule holds
  there too (§16) — it is only the `self` rule above that treats the two
  differently.
* A generator body cannot `perform` a bare effect operation or declare
  an `effect fn`; a self-contained `handle { ... }` expression inside
  the body does work (§16).

Generators are compiled by a source-level transform shared by all three
backends, so an identical program yields identical values under the
interpreter, the JIT, and an AOT binary.

---

## 12. Control flow

### `if`

    if cond { then_block }
    if cond { then_block } else { else_block }
    if c1 { b1 } else if c2 { b2 } else { b3 }
    if init; cond { … }

`cond` must be truthy-convertible (`Bool` or `Long`). `if` is an
expression; its value is the taken branch's last expression, or `nil`
if no branch is taken (no `else` and the `if` was false).

Like `while`, `if` accepts an optional **init clause** — declarations
before the condition, split by `;` — scoped to the whole `if` / `else
if` / `else` chain (the same form as [`while`](#while)):

```culebra
# doctest: skip
if mut d = compute(n); d > threshold {
  use(d)  # d is in scope here …
} else {
  fallback(d)  # … and here, but not after the chain
}
```

Each binding must be a declaration (`let` / `mut`); a bare `if x = 0;
…` is a `SyntaxError`. Multiple bindings use `,`
(`if mut a = f(), mut b = g(); …`).

### Statement modifiers (`if` / `unless`)

    stmt if cond
    stmt unless cond

A trailing `if` / `unless` (Ruby's statement modifiers) runs `stmt`
only when `cond` is (`if`) or is not (`unless`) truthy — `unless` is
exactly `if !cond`. It desugars to an ordinary `if cond { stmt }` /
`if !cond { stmt }` before either backend sees it, so it is `nil`
when the condition doesn't hold, same as a bodyless `if`:

```culebra
mut i = 0
while i < 10 {
  i = i + 1
  break if i > 5
  continue unless i % 2 == 0
  println(i)
}
# => 2
# => 4
```

The modifier attaches to a whole **statement**, not an arbitrary
expression — `f(1 if true)` is a `SyntaxError`, exactly as in Ruby.
Its value only shows through when the modified statement is the last
one in a block or function body:

```culebra
fn grade(v) {
  return "small" if v < 10
  return "big" unless v < 100
  "medium"
}
inspect(grade(5))    # => 'small'
inspect(grade(500))  # => 'big'
```

`unless` is a reserved word only at the assignment-target position,
like the rest of the [hard-reserved keywords](#keywords) — it stays a
valid parameter name, object key, and property name.

### `cond`

    cond { test => body, ..., _ => default }

The subjectless multi-way conditional (Elixir's `cond`, Kotlin's
argless `when`): arms are tried top to bottom, and the value of the
first arm whose `test` is truthy becomes the value of the whole
expression. `_` is the always-match wildcard, conventionally last —
arms after a matched `_` never run. If no arm matches, the value is
`nil` (like an unmatched `match`).

```culebra
fn grade(n) {
  cond {
    n >= 90 => 'A',
    n >= 80 => 'B',
    _ => 'C',
  }
}
inspect(grade(85))  # => 'B'
```

A `test` follows the same truthiness rule as `if` (`Bool` or `Long`,
zero falsy), and may be any expression — calls, `&&`/`||`,
comparisons. An arm body follows the same
[expression-or-block](#arm-bodies) rule as `match` arms: a bare
expression, or a brace block that yields its last statement's value
and forms its own scope.

Use `cond` in place of a `match true { _ if … => ... }` guard chain
when there is nothing to match on — it reads as a priority-ordered
list of conditions rather than a match against a dummy subject.

### `while`

    while cond { body }
    while init; cond { body }

`while` is a statement; its value is `nil`. `break` and `continue`
work inside the loop body. The body is a fresh scope per iteration
(like `for` — see [Scope](#scope)).

An optional **init clause** — a comma-separated list of declarations
before the condition, split by `;` — binds variables scoped to the
loop, so a counter no longer leaks into the enclosing scope:

    while mut i = 0; i < len {
      out.push(i)
      i = i + 1
    }
    # `i` is not visible here

The init variables persist across iterations (a body `i = i + 2`
re-assigns) and are dropped when the loop exits by any path (normal,
`break`, or an exception). Multiple bindings use `,`:

    while mut i = 0, mut j = xs.size() - 1; i < j { i = i + 1; j = j - 1 }

Each binding must be a declaration (`let` or `mut`); a bare
`while x = 0; …` is a `SyntaxError` (it would reassign an outer `x`
rather than scope one to the loop). Destructuring binds too:
`while mut (a, b) = pair; …`.

### `for` ... `in`

    for var in iterable { body }

Iterates by calling `iterable.iter()` once, then driving the returned
iterator with `has_next()` / `next()`: while `has_next()` is `true`,
`next()` produces the element bound to `var` in a fresh scope per
iteration (see §18.5).

```culebra
for x in [1, 2, 3] {
  inspect(x)
}

for k in {b: 2, a: 1} {
  inspect(k)
}  # keys, ascending

for i in 0..10 {
  inspect(i)
}  # exclusive (0..9)
for i in 0..=10 {
  inspect(i)
}  # inclusive (0..10)
```

Range values `a..b` (exclusive) and `a..=b` (inclusive) iterate the
same lazy integer sequence as `range`. A bounded range (both `Long`
endpoints present) is iterable; an open-ended range used for slicing
(`xs[2..]`) has no iteration end and raises if iterated.

A range takes an optional `by <step>` clause to iterate by something
other than 1, including descending (`step` negative):

```culebra
for i in 0..10 by 2 {
  inspect(i)
}  # 0, 2, 4, 6, 8
for i in 10..0 by -2 {
  inspect(i)
}  # 10, 8, 6, 4, 2
```

`step` must not be `0` (raises `ValueError` when the range is
iterated). Slicing (`xs[a..b by n]`) ignores `step` — it only affects
iteration.

**Destructuring loop variable.** The `var` may be a pattern, matched
against each element's shape (a mismatch raises `ValueError`).
Comma-separated targets without parens are sugar for a tuple pattern:
`for k, v in xs` means `for (k, v) in xs`. The bracket form `[a, b]` and
the tuple/bare form `(a, b)` / `a, b` are interchangeable — either matches
any indexed sequence (`Array` or `Tuple`) of the right length, so the
pattern's punctuation is style, not a type constraint. This holds for
assignment destructuring too: `[a, b] = (1, 2)` and `(a, b) = [1, 2]`
both bind.

```culebra
# doctest: skip
for [a, b] in [[1, 2], [3, 4]] { inspect(a + b) }      # array pattern
for (k, v) in [(1, 'a'), (2, 'b')] { inspect(k) }      # tuple pattern
for k, v in {a: 1, b: 2} { inspect("{k}={v}") }        # bare comma == (k, v)
for i, v in xs.enumerate() { ... }                  # (index, value) tuples
```

The iterator protocol (see §18.5) requires the target to be either an
`Object` (or subtype `Array`) with an `iter` method, or an object
already playing the iterator role with `has_next` / `next` methods.
Passing any other type raises `type error`.

Iterating a `Set` walks a snapshot of the members taken when the iterator
is created: mutations made by the loop body change the set but not the
walk. An `Object` snapshots only its keys — removed keys are skipped and
values are read live (see §18.5). An `Array` is read live, one index per
step.

`for` is a statement; its value is `nil`. Shadow rules apply to
`var`: if it would shadow a closure-captured name from an enclosing
function, the script is rejected (see §6).

**JIT**: `for` / `break` / `continue` compile under `--jit` for
direct iteration over `Array`, `Object` (yields `(key, value)` pairs in
insertion order), and `String` (UTF-8 scalar walk). Objects that carry their
own `iter` property (user-defined iterators, `range`,
`String.code_points()` / `.graphemes()`, iterator method chains) are
driven through the iterator protocol at runtime — same semantics as
the interpreter, with a native-loop fast path preserved for the
Array/String/keys cases.

### `break` and `continue`

    break           # exit the innermost enclosing loop
    continue        # skip to the next iteration of the innermost loop

Valid only inside `for` or `while`. Using them outside a loop is a
`SyntaxError`, checked before the program runs. `break` / `continue` do
not carry a value (the loop's value remains `nil`).

Both are **expressions** (§3), so they need no statement position of
their own — a `match` arm (`')' => break`), a `cond` arm, or either side
of a ternary takes one directly. See [Arm bodies](#arm-bodies) for the
arm form, and [Diverging expressions](#diverging-expressions) for what
this means in the middle of a larger expression.

### `nobreak` (loop-else)

    while cond { body } nobreak { … }
    for var in iterable { body } nobreak { … }

An optional `nobreak { … }` block after a `while` or `for` runs **only
when the loop finishes normally** — the condition became false, or the
iterator was exhausted — and is **skipped when the loop exits via
`break`** (and also by `return` or a throw, which unwind past it). These
`nobreak` names the condition it tests — it runs when no `break`
happened — so the block needs no comment to say when it fires. It
carries no value (the loop stays `nil`).

The canonical use is search — the block is the "not found" branch, with
no flag variable:

```culebra
# doctest: skip
fn find(xs, target) {
  for x in xs {
    if x == target {
      return "found"
    }
  } nobreak {
    return "not found"  # only reached if the loop never broke
  }
}
```

A `while` init clause is in scope in its `nobreak` block (the counter
survives to the post-loop step); a `for` loop variable is not (it is
per-iteration and already gone). A `break` / `continue` inside a
`nobreak` block belongs to an *enclosing* loop, since the block runs
after this loop has finished. `nobreak` is a contextual keyword —
recognized only in this trailing position, so it stays usable as an
ordinary identifier elsewhere.

### `return`

Valid only inside a function body. Exits the enclosing function with
the given value (or `nil`). `return` outside any function is a
`SyntaxError` (checked before the program runs) — a script ends at its
last statement, or exits early via `Sys.exit`.

Like `break` and `continue`, `return` is an expression, so
`_ => return x` is a legal `match` arm.

### Diverging expressions

`return`, `throw`, `break` and `continue` share a property: none of them
produces a value. Reaching one transfers control elsewhere, so the
expression it sits inside never finishes evaluating. That is why all
four are expressions rather than statements — there is no value for the
surrounding expression to be given, and so no position where one would
be ill-typed:

```culebra
# doctest: skip
let width = "v" + match n {
  # the `+` never completes when n is 4
  4 => break,
  _ => "x",
}
```

Consequences worth knowing:

* Operands evaluated before the diverging one are discarded. In
  `f(a(), match n { 0 => break, _ => b() })`, `a()` has already run when
  the `break` is taken; the call to `f` never happens.
* `defer` blocks still fire on the way out, innermost first, exactly as
  they would for the statement spelling (§15).
* `break` / `continue` bind to the nearest enclosing *loop*, never to
  the `match` or `cond` they appear in — those are expressions, not
  control-flow scopes, so there is no "exit the match" reading to
  confuse them with.
* The check that a `break` has an enclosing loop, and a `return` an
  enclosing function, runs before the program does and is a
  `SyntaxError` in expression position just as in statement position.

Being an expression does not make one bind tighter. `return`'s operand
is a whole expression in either position, so `return 1 + 2` returns `3`
— in a `match` arm exactly as at the head of a statement.

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

Like `while` and `if`, `match` accepts an optional **init clause** —
declarations before the subject, split by `;` — scoped to the subject
and every arm (the same form as [`while`](#while)):

    match mut x = compute(n); x {
      0 => "zero",
      v if v > x - 1 => v,    # init var `x` is in scope in guards …
      _ => x                   # … and arm bodies, but not after the match
    }

Each binding must be a declaration (`let` / `mut`); a bare
`match x = 0; …` is a `SyntaxError`. Multiple bindings use `,`
(`match mut a = f(), mut b = g(); a + b { … }`). The init variables are
dropped when the match is left by any path (a matched arm, the no-match
fall-through, or an exception).

### Patterns

| Form              | Matches                                  |
|-------------------|------------------------------------------|
| `0`, `'x'`, `"x"`, `nil`, `true` | Literal equality (a string pattern is `'...'`, a backtick string, or a `"..."` with no interpolation) |
| `name`            | Any value, binds it to `name`            |
| `_`               | Any value, no binding                    |
| `name: Type`      | Value whose type is `Type`; binds        |
| `p1 \| p2 \| p3`  | Any of the sub-patterns matches; alternatives cannot bind |
| `[p1, p2, ...]`   | `Array` of exactly the same length       |
| `[p1, ...rest]`   | `Array` of ≥ `n−1` elements; `rest` is a fresh `Array` of the remainder |
| `[a, ...m, z]`    | Rest can be in the middle; pre/post positions match fixed elements |
| `{k1, k2}`        | `Object` containing at least those keys; binds each `ki` to `obj.ki` (shorthand) |
| `{k1: p1, k2: p2}` | `Object` whose `k1` matches `p1` and `k2` matches `p2`. Nests freely (`{user: {name}}`). Mixes with shorthand. |
| `{}`              | Any `Object` (keys ignored)              |
| `(p1, p2, ...)`   | `Tuple` of exactly the same arity; element-wise sub-patterns |
| `Ok(p1, ...)`     | Enum constructor: matches the variant `Ok` and destructures its positional payload against the sub-patterns. Qualified form `Result.Ok(p)` also works. See "Sum types". |

### Semantics

* A literal pattern must be a compile-time constant. An *interpolating*
  `"...{x}..."` is a SyntaxError — match against the runtime value with a
  guard instead (`s if s == x => ...`).
* Patterns are tried left-to-right, and sub-patterns are evaluated
  depth-first.
* A binding `name` introduced by the pattern is visible in the guard
  and the body.
* `|` (or) sub-patterns cannot bind: a name inside an alternative would
  exist only on the paths that took it, so any binding there is a
  `SyntaxError` (`a | _`, `5 | a`, `Ok(x) | Err(x)`). Write literals and
  `_` inside `|`, and one arm (or one pattern) per binding shape.
* `Array` patterns require an exact length unless a `...rest` element
  is present; objects do not require exact key sets — extra keys are
  ignored.
* The rest array is a newly-allocated `Array` with elements from the
  subject shallow-copied; mutating it does not affect the subject,
  but mutating a reference-typed element mutates the shared object.
* The `match` subject is evaluated exactly once.

### Arm bodies

An arm body is normally a single expression. To run several statements,
write a brace **block** — the arm evaluates to the block's last
statement value:

    let kind = match tok {
      '+' => { let p = prec(tok); register(p); "op" },
      _   => "atom",
    }

A block arm is its own scope: pattern bindings and any `let` inside it are
visible only within the arm, and a `defer` fires when the arm's braces
close (LIFO, before the arm value is consumed). `return` / `break` /
`continue` inside a block arm behave as they would anywhere — and still run
the arm's pending defers on the way out.

An arm that does nothing *but* transfer control needs no block, since all
four control-transfer forms are expressions (§12):

    match tok {
      ')' => break,
      '!' => throw "unexpected",
      ' ' => continue,
      _   => tok,
    }

The arm parser tries an expression first, so a brace that *is* a valid
literal keeps its literal meaning, not a block:

    match x {
      0 => {},          # empty Object
      1 => {a: v},      # Object literal
      2 => {p, q, r},   # Set literal (two or more elements)
      _ => { f(); g() } # block: runs f() then g(), yields g()'s value
    }

One sharp edge falls out of object **shorthand**: a brace wrapping a single
bare identifier, `{ v }`, is the object `{v: v}`, not a block yielding `v`.
Write `_ => v` (no braces) for that, or `{ v; }` to force a block. Shorthand
reads a lone `{ break }` / `{ continue }` the same way, and since no variable
may be named `break`, that arm raises `NameError` — write the bare
`_ => break`, which is what you meant.

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

Multimethod dispatch ([§20](#20-multimethods)) understands Union
parameter annotations by scoring each alternative and taking the
best match — `fn area(s: Square | Circle)` dispatches on either
exact class. A bare concrete type outranks any Union that contains
it: defining both `fn pick(x: Long)` and `fn pick(x: Long | Float)`
routes a Long arg to the concrete `pick`, while a Float still goes
through the Union version.

### Optional types (`T?`)

A trailing `?` on a type name is sugar for `T | Nil` — it makes nil an
accepted value while still enforcing the base type for non-nil values.

    fn id(x: Long?) -> Long? { x }
    id(5)      # → 5
    id(nil)    # → nil
    id("s")    # !! rejected (not Long, not nil)

`?` works on any type name, including Generic outers (`Array<Long>?`)
and in `let` / parameter / return annotations (`let a: String? = nil`).
Introspection canonicalizes it to the Union form: `fn.params[0].type`
of `x: Long?` reads `"Long | Nil"`. Pair with the null-safe operators
below (`?.`, `?[]`, `!!`, `??`, `??=`) for ergonomic nil handling.

### Function types (`fn(T) -> U`)

The bare `Function` type accepts any callable. To document a
higher-order parameter's *shape* — what it takes and returns — write a
function type: `fn(T1, T2) -> R`.

    fn apply(f: fn(Long) -> Long, x: Long) -> Long { f(x) }
    apply(|n| n * 2, 21)        # → 42

    fn make_adder(n: Long) -> fn(Long) -> Long { |x| x + n }
    let add5 = make_adder(5)
    add5(37)                    # → 42

The parameter list may be empty (`fn() -> String`), hold several types
(`fn(Long, Long) -> Long`), or nest (`fn(fn(Long) -> Long) -> Long`).
A callable class instance (one with a `__call__` method) satisfies a
function type just like a closure does:

    class Doubler { new() {} __call__(n) { n * 2 } }
    apply(Doubler.new(), 21)    # → 42

Like Generic element types, the parameter and return types are
**documentation in the MVP** — the runtime checks only that the value
is callable, not its arity or the types it actually accepts. A
non-callable is rejected:

    apply(99, 21)               # !! type error: ... expects fn(Long) -> Long

The return is a single type, so a top-level `|` after `->` belongs to
the surrounding Union: `fn(A) -> B | C` parses as `(fn(A) -> B) | C` (a
Union of *a function returning B* and *C*). An optional **return** uses
`?` on the return type — `fn(A) -> B?` is a function whose result is
`B | Nil`; the function itself is still required:

    fn run(f: fn(Long) -> Long?) -> Long { f(0) ?? -1 }
    run(|n| nil)                # → -1
    run(nil)                    # !! type error (a function is required)

Multimethod dispatch ([§20](#20-multimethods)) treats a function-type
parameter like `Function`: a closure or `__call__` instance routes to
the `fn(...) -> ...` overload, while a concrete type (`Long`) routes to
its own overload.

### Generic types

A type name may carry type parameters in angle brackets:
`Array<Long>`, `Array<Array<Long>>`, `Array<Long | Float>`. Nesting
is supported, and Generic args may themselves be Unions. The only
built-in container type today is `Array`; other Generic outer names
(`Box<T>`, `Pair<K, V>`, ...) come from user-declared classes
(see "Generic class declarations" below).

    fn first(xs: Array<Long>) -> Long { xs[0] }
    fn lookup(k: String) -> Array<Long> | Nil { ... }

**Element-level runtime checks are no-ops**: only the outer type is
checked at the boundary — verifying the element type would mean walking
the collection on every call. The args exist for documentation and for
multimethod
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
      new (v: T) { self.v = v }
    }

    class Pair<K, V> {
      new (k, v) { self.k = k; self.v = v }
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
      new(amount) { self.amount = amount }
      cmp(other) { self.amount - other.amount }   # conforms to Comparable
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

A **composite bound** `<T: A + B>` requires the argument to conform to
**all** parts (the all-of dual of a Union). Spacing is tolerated
(`<T:A+B>` works too).

    fn both<T: Hashable + Stringer>(x: T) { x }
    both(5)        # OK — Long conforms to both
    both([1, 2])   # !! Array is Stringer but not Hashable -> rejected

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
* Class declarations inside another class's body (currently a
  SyntaxError, see "Where class declarations may appear" above).

### Nil and annotations (null-safety policy)

Culebra keeps `nil` universal but lets annotations opt into runtime
null safety, in three tiers:

* **No annotation** — anything goes, including `nil` (the default
  dynamic behavior).
* **`T` (non-optional)** — `nil` is *rejected*. A function parameter
  typed `x: Long` will not accept `nil` (the call is reported as a
  dispatch/type error); a `let x: Long = nil` raises a type error.
* **`T?` (optional)** — `nil` is *accepted*, and the base type is still
  enforced for non-nil values (`x: Long?` takes a Long or nil, but not
  a String).

This is a **runtime** policy — there is no static null checker (by
design). The null-safe operators pair with it:
`?.` / `?[]` navigate possibly-nil values, `!!` asserts non-nil
(raising `NilError`), and `??` / `??=` supply or store fallbacks.

There is no `late` / `lateinit` keyword: in static languages that
feature exists to satisfy a *static* non-null checker for deferred
initialization, which culebra does not have. Use a plain (nullable)
field and assert with `!!` at the use site, or guard with `??`. For
lazily-computed / memoized fields, the idiom is a nil-checked accessor
method — `self._data ??= load()` works directly (`??=` supports
`obj.key` targets, not just simple variables):

    class Cache {
      new() { self._data = nil }
      data() {
        self._data ??= load()
        self._data
      }
    }

### Where annotations are *not* checked

* Arithmetic / boolean operators check their own operand types;
  annotations on local intermediates do not add extra coverage.
* Object property values have no annotation slot.
* Array element types are not tracked.
* `mut x: T` on a local does not re-check on later reassignment —
  the check happens once at the annotated declaration.

Annotations are primarily a **documentation and boundary check**
feature, not a type system.

### Sum types (`enum`)

An `enum` declares a sum type — a fixed set of variants, each
optionally carrying positional payload. Generic parameters are
supported.

    enum Result<T, E> {
      Ok(T),
      Err(E),
    }
    enum Color { Red, Green, Blue }      # nullary variants
    enum Shape { Circle(Float), Rect(Float, Float), Origin }

Each variant lowers to a **variant-as-class**: the enum name is bound
as a namespace, and constructing a value goes through it. Payload
variants are constructors; nullary variants are singleton values.

    let r = Result.Ok(5)        # payload variant — call the constructor
    let c = Color.Red           # nullary variant — a singleton value
    let s = Shape.Rect(3.0, 4.0)
    Result.Ok                   # the constructor is a first-class value

A variant instance is tagged with both the variant name and the parent
enum name, so type annotations / patterns match at either level:

    fn handle(r) {
      match r {
        Ok(x)  => x,            # constructor pattern, binds payload
        Err(e) => -1,
      }
    }
    match v {
      x: Ok      => "the Ok variant",     # variant type pattern
      x: Result  => "some Result",        # enum type pattern (any variant)
      _          => "other",
    }

Payload is positional and reachable as `_0`, `_1`, … (`r._0`), though
constructor patterns are the idiomatic accessor.

#### Notes and limitations

* The canonical sum-type style is an untyped parameter plus a `match`:
  `fn area(s) { match s { Circle(r) => ..., ... } }`.
* Multimethod dispatch keys on the **variant** (`fn f(x: Ok)`), not on
  the enum name (`fn f(x: Result)` is not yet a dispatch key) — use a
  `match` for enum-level dispatch.
* No methods-in-enum block (use free functions + UFCS), no named
  payload fields, no explicit discriminant values, and no static
  exhaustiveness check (a non-matching `match` yields `nil`).
* Nullary variants are matched with a type pattern (`_: Origin`), not a
  parens-free constructor pattern.

### Traits and protocols

A `trait` declares a named contract — the set of methods a value
must provide to be treated under that name in dispatch and Generic
Bound positions.

    trait Greeter {
      hello() -> String
    }

**Structural conformance**: any class whose methods cover the trait's
required ones (name + arity) automatically satisfies the trait —
no `impl Foo for Bar` block needed: conformance is decided by the
methods a class actually has.

    class Bob {
      new(name) { self.name = name }
      hello() { "hi, {self.name}" }
    }

    fn greet(x: Greeter) { IO.inspect(x.hello()) }
    greet(Bob.new("Alice"))                # → "hi, Alice"

A class missing required methods is rejected at the dispatch
boundary (DispatchError).

#### Default methods

A trait method may carry a body. Conforming classes inherit it for
free; defining the same name on the class overrides:

    trait Counter {
      current() -> Long
      next() -> Long { self.current() + 1 }     # default
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

A default is a method the conforming class inherits, so it ranks with
the class's own methods: only an own property or a declared method
outranks it, and it outranks everything the runtime supplies for an
object — the `Object` built-in methods (`size`, `keys`, ...), the
synthesized `parameters()`, and the duck-typed iterator method set.

A trait declares each method name once. Two same-name methods are a
`SyntaxError`: the contract and the default bodies are keyed by name,
so a trait has no overload set to merge them into (a class does — see
"Method overloading").

A trait declaration takes effect when it runs, like a `class`: a trait
inside a branch that is never taken registers nothing, and re-declaring
a name replaces the earlier contract and defaults outright.

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

`Iterator` + `Iterable` formalize the for-in protocol. A class that
exposes `iter() -> Iterator`,
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
inline:

    fn min<T: Comparable>(a: T, b: T) -> T {
      if a.lt(b) { a } else { b }
    }

The bound is **enforced** at the call boundary (no compile-time
check; culebra's type checks are runtime) — see "Bound constraints"
under Generic types for the full semantics (lowering, dispatch
ordering, lenient unification). A composite bound `<T: A + B>` is
also supported — the argument must conform to **all** parts (the
all-of dual of a Union). `where` clauses are not planned (the inline
form already covers the cases).

#### Dispatch tie-break

A trait param scores between Object and concrete in multifn
dispatch: a method with `x: Pri` wins over a method with
`x: Stringer` when the arg is a Pri instance; `x: Stringer` is
chosen when only the trait path matches.

#### Trait inheritance

A trait may declare supertraits after a colon: `trait Ord: Eq`,
`trait Both: Eq, Show`. The supertrait's methods are flattened into
the subtrait, so a value conforming to `Ord` must supply **both**
`Eq`'s and `Ord`'s required methods. This works transitively
(`Done: Ord` pulls in `Eq` too) and a supertrait's *default* methods
become available on the subtrait.

    trait Eq  { eq(other) -> Bool }
    trait Ord: Eq { cmp(other) -> Long }   # conforming to Ord needs eq + cmp

    fn sorted<T: Ord>(xs: Array<T>) { ... }

Supertraits must be declared before the trait that inherits them
(declaration order resolves the flatten). Because conformance for
built-in primitive types is name-based (a hard-coded table), trait
inheritance affects **structural (class) conformance** only — a
primitive does not retroactively satisfy a user trait via its
supertrait.

#### Deriving methods (`@derive`)

The `@derive(...)` class decorator generates standard conformance
methods automatically, so a data class doesn't have to spell out
boilerplate `eq` / `hash` / `to_s` / `cmp`. Four names are derivable,
each mapping to one method:

| Derive | Generated method | Behavior |
|---|---|---|
| `Eq` | `eq(other)` | true when `other` is the same class and every data field is equal |
| `Hash` | `hash()` | combines the class name and each data field's hash |
| `Show` | `to_s()` | `"ClassName(f1, f2, ...)"` with each field's value repr |
| `Comparable` | `cmp(other)` | lexicographic over data fields, in declaration order |

    @derive(Eq, Hash, Show, Comparable)
    class Point {
      new(x, y) { self.x = x; self.y = y }
    }

    let a = Point.new(1, 2)
    let b = Point.new(1, 2)
    a.eq(b)            # → true
    a.hash() == b.hash()   # → true
    a.to_s()           # → "Point(1, 2)"
    a.cmp(Point.new(1, 3)) # → -1

Because `@derive` only supplies the trait's *required* method, the
trait defaults follow automatically: deriving `Eq` gives you `neq`,
and deriving `Comparable` gives you the full `lt` / `le` / `gt` / `ge`
suite. Deriving `Eq` + `Hash` makes the class usable as an `Object` /
`Set` key.

The generated methods are **reflective** — they walk the instance's
own data fields at call time (skipping methods and the internal class
tag), so they stay correct as fields change. Details:

* **User definitions win.** A derived method whose name the class
  already declares is skipped, so you can derive most and hand-write
  one (e.g. `@derive(Eq, Hash)` plus a custom `to_s`).
* **`@derive` is compiler-recognized**, not a function — `Eq` / `Hash`
  / `Show` / `Comparable` are the only accepted names (anything else is
  a SyntaxError). It composes with regular decorators on the same class.
* **Nominal:** derived `eq` requires the same class tag, so two classes
  with identical fields never compare equal.
* **`cmp` ordering** compares each field pair exactly as `<` does: equal
  fields (by `==`) move on to the next, and the first unequal pair decides.
  A pair `<` refuses to order — an Array, an Object, two different types —
  is the same `cannot compare` TypeError there, so `cmp` orders the numeric
  and string fields and no others.
* **`eq` and `cmp` take `other`**, `hash` and `to_s` take nothing; calling
  one without its argument is the ordinary missing-required ArityError.

#### Limitations (Phase 4 MVP)

* `impl Foo for Bar` block is unsupported — conformance is purely
  structural. (Phase 4+: revisit if explicit conformance is wanted.)
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
  `trait X { foo() { self.foo() } }` will stack-overflow if a
  conforming class doesn't override `foo`. No depth guard is
  installed yet.
* **JIT trait-default closure refcount**: each declared default
  method is held at +1 in a per-Runtime table. Re-declaring a trait
  releases the displaced bodies, and the rest are released when the
  Runtime is destroyed, so long-running JIT hosts re-compiling many
  sessions do not accumulate them.
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
    catch e { inspect("error: {e}") }

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
* `throw` is an expression (§12), so it can be the whole of a `match`
  arm (`_ => throw "unexpected"`) with no block around it.

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

### Interruption (Ctrl+C)

Ctrl+C raises a cooperative, catchable `Interrupted` rather than
killing the process outright:

    try {
      serve()                       # long-running loop
    } catch e {
      inspect("shutting down: {e.kind}")   # → Interrupted
      cleanup()
    }

* The running computation stops at the next loop iteration or statement
  boundary and throws `Interrupted`. A tight loop (even `while true {}`)
  is interruptible, as is a wait on `IO.stdin().read()` / `IO.input` (blocking
  on stdin), a blocking `Http` request (connect, response wait, or body
  transfer), or a blocking `Proc.run` / `Proc.all` / `Proc.race` (the child
  is killed) — a single press breaks the wait, not just the second.
* It unwinds like any exception, so `defer` blocks run on the way out.
* If you `catch` it, execution resumes normally — the interrupt is
  one-shot, so a server / REPL can treat Ctrl+C as "cancel the current
  request" and keep going.
* If it reaches the top uncaught, top-level defers run and the program
  exits with status `130` (128 + SIGINT), the conventional code.
* A second Ctrl+C while the first is still pending (a wedged program
  that never reaches a safepoint) force-terminates with the default
  disposition.

In the REPL, Ctrl+C interrupts the running evaluation and returns to the
prompt instead of killing the session (the `Interrupted` is caught by the
read-eval loop); a second press during a wedged eval still force-quits.

To receive Ctrl+C as a value instead of a throw — the graceful-shutdown
pattern for a long-running service — register a channel with `Signal.notify`
(see the stdlib guide's *Signal* section).

The behaviour is identical under the interpreter, JIT, and AOT binaries.
`Interrupted` carries no source position (`line`/`col` are `0`): the
interrupt is asynchronous, not tied to a particular expression.

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
      if e.kind == 'IndexError' { inspect("out of range at line {e.line}") }
      else { throw e }
    }

Standard `kind` values, with the exact trigger condition and
catchability for each. Every kind populates `e.kind`, `e.message`,
`e.line`, and `e.col` identically on the interpreter, the JIT, and
AOT builds (unless noted).

| Kind | Trigger | Catchable |
|---|---|---|
| `TypeError` | Arithmetic / comparison on incompatible operand types; calling a non-callable; failing `: T` annotation; `to_long`/`to_float` on non-coercible value; `__str__` returning non-String; `*` splat of non-Array / `**` splat of non-Object; built-in arg-type check failure; mixed positional + keyword targeting the same parameter; duplicate keyword; more positionals than the `*` separator allows (`takes N positional arguments but M given`). | yes |
| `ZeroDivisionError` | Integer `/`, `%`, `**` with negative exponent collapsing to division; float `/` or `%` with RHS == 0. | yes |
| `NameError` | Read of an undefined identifier; compound assignment (`x += rhs`) on undefined `x`; REPL global lookup miss. A name bound in *no* scope (and not a builtin) is caught before evaluation — see Compile-time errors; a name read before its own later declaration runs (use-before-def) stays a runtime error. | yes¹ |
| `ImmutableError` | Assignment to a `let` (non-`mut`) binding; assignment to an immutable Object property or `Dict` entry; rebinding `self` inside a constructor body. | yes |
| `KeyError` | Dict subscript on absent key; Object subscript on absent key. | yes |
| `IndexError` | Array / String / Tensor index out of range; Tensor slice out of bounds; Tensor reduction axis out of range. | yes |
| `ValueError` | Destructure pattern mismatch (`[a, b] = ...` shape mismatch); Tensor shape / dtype mismatch; `[].min()` or other empty-collection reductions; numeric conversion of malformed string; JSON parse failure; walking a value nested deeper than 1000 levels (see the value-nesting bound below). | yes |
| `AttributeError` | Compound assignment (`o.x += ...`) on a missing property; reading or writing (`=`/`??=`) a member a builtin namespace doesn't have (namespaces are closed — a class or plain dict still reads/writes freely). | yes |
| `ArityError` | Call missing a required argument — too few positional args to a function or a class constructor; more positional args than a *built-in* or namespace function accepts. Surplus positionals to a **user** function are not an error: they overflow into `__ARGS__` (§19). | yes |
| `DispatchError` | Multimethod call with no matching method or with ambiguous specificity tie (§20). | yes |
| `AssertionError` | Matcher failure (`assert_true` / `assert_eq` / etc.) or user `throw {kind: "AssertionError", ...}`. Message names both operands for comparison matchers. | yes |
| `SyntaxError` | Structural errors raised during AST lowering: `**rest` not last param, duplicate `*` separator, non-default param after default, `compound let`, `break` / `continue` outside loop. Surfaces at function decl evaluation, before that function runs. | yes |
| `ShadowError` | Static shadow analyzer (§6) detected a binding that shadows a captured outer name. Fires before any user `try` block can observe it. | **no** (pre-eval analyzer) |
| `IOError` | `FS` / `File` / stdlib file ops failing (missing path, permission, closed handle); `Tensor.from_csv` failure. | yes |
| `ProcessError` | `Proc.run` spawn failure (e.g. the executable doesn't exist), or a non-zero exit / signal death under `check: true`. | yes |
| `SendError` | A value that is not Sendable was passed across an isolate boundary (`Isolate.spawn` / `tx.send`) — a native handle, a `Tensor`, a closure capturing a `mut`, or a cyclic value. | yes |
| `ChannelError` | `tx.send` on a channel whose receivers/senders have all gone (closed). | yes |
| `ParallelError` | A `Parallel.map` / `Parallel.each` element threw; carries the failing element's index and cause (fail-fast). | yes |
| `DropContractError` | `drop` / `iter` / `has_next` / `next` property bound to a non-Function or non-zero-arity function. | yes |
| `RecursionError` | Function-call depth exceeded the fixed limit of 1000 frames. Every user-function entry counts one frame (fn, lambda, method, constructor — field initializers run inside the constructor's frame); built-in helpers and multimethod dispatch do not. The limit and the reported depth are identical on every backend, and reported at the call site. The count unwinds with `throw`, so a `catch` regains the full budget. | yes |
| `RuntimeError` | Fallback when interp catches an unconverted `std::runtime_error` from a not-yet-migrated throw site. `e.line == 0` and `e.col == 0` are possible in this case only. | yes |

Uncaught errors print as `Kind: message` and exit with non-zero
status. User-thrown values via `throw expr` print as `uncaught: {value}`.

¹ The `NameError` for a name that is bound in *no* enclosing scope and is
not a builtin is the exception: it is caught before evaluation (see
Compile-time errors) and so is *not* catchable. Every other `NameError` —
notably use-before-def — is a runtime error and is catchable.

### The value-nesting bound

A loop can build a value of unbounded depth (`a = [a]` repeated). Every
operation that walks a value's nesting — printing (`inspect` / `print` /
interpolation / `to_string` / `join`), `==` / `!=`, `hash`, membership
(`contains` / `index_of`), sending across an isolate boundary
(`Isolate.spawn` arguments, `tx.send`, `Shared.new` — where a chain of
closures nesting through captures counts the same way), and the
`@derive`d methods — stops at 1000 levels and raises a catchable
`ValueError` (`nesting too deep (limit 1000)`) at the call site,
identically on every backend. A same-pointer comparison (`a == a`)
answers without walking. Building, indexing, and dropping a deeper value
stay safe at any depth — teardown is bounded internally, not by this
error. `JSON` / `TOML` apply the same limit to their own trees (see the
stdlib reference).

A `Tensor`'s autograd graph is not value nesting and is not subject to
this bound: `.backward()` and dropping an unevaluated graph both stay
safe at any depth, with no `ValueError` cap. A computation graph has no
natural "too deep" — an RNN unrolled over a long sequence is a
legitimate graph, not malformed data — so both are internally bounded
without an artificial depth limit (see the Autograd section of the
stdlib reference).

### Compile-time errors

Two checks run when the program is loaded, before any `try` block runs —
so user code cannot catch them. Both abort with the same `Kind: message`
format and run identically on every backend.

* **`ShadowError`** — a binding that shadows a captured outer name; see
  "Shadow prohibition" in §6 for the rule.
* **`NameError` (statically undefined)** — a variable read whose name is
  bound in *no* lexical scope and is not a builtin. This is *certain* to
  fail at runtime, so — like an unknown name in a statically-checked
  language — it is reported before evaluation rather than waiting for the
  (possibly never-taken) path that reads it. The check is sound: it flags
  only names that resolve nowhere, so it never rejects a valid program.

The complementary cases stay at *runtime*, catchable by `try`/`catch`,
because they cannot be proven to fail statically: **use-before-def** (a
name *is* bound in scope but is read before its declaration executes — the
binding might run first on a later loop iteration), `ImmutableError`, and
missing / unknown kwargs.

### Assertion API

There is no `assert` keyword or builtin. For tests, use the matcher
family (`assert_true` / `assert_eq` / etc., see §19 and `docs/stdlib.md`).
For production invariants, throw an Object:

```culebra
# doctest: skip
if !cond {
  throw {kind: "AssertionError", message: "..."}
}
```

Assertion control flow is written out rather than hidden behind a
keyword that a build flag can switch off.

### JIT support

The JIT backend supports `throw` / `try` / `catch` / `defer` with
semantics matching the tree interpreter for the common cases:

* `throw` / `try` / `catch` propagate across function boundaries via
  LLVM `invoke` / `landingpad` and the Itanium C++ personality.
* `defer` registers a closure on a defer stack; a scope cleanup
  landingpad runs it on fall-through, `return`, and throw-unwind
  paths. This holds at every level — inside a lexical-scope block
  (`{ defer { ... } ... }`), directly in a function body, and at the
  top level — so cleanup fires even when an uncaught throw escapes
  the function or the program.

Internal runtime errors (`TypeError`, `ZeroDivisionError`, etc.) flow
through user `try/catch` as structured Error Objects on both backends
— see "Runtime errors" above.

---

## 16. Algebraic effects

An **effect** is an operation whose meaning is supplied by the dynamically
enclosing context rather than fixed at the call site. Code `perform`s an
operation; a `handle` block installed higher on the call stack decides what
the operation does — and whether, and how many times, to **resume** the code
that performed it. One mechanism expresses generators, exceptions, cooperative
scheduling, and backtracking search without each needing dedicated syntax.

Effects lower, at parse time, to ordinary Culebra classes plus a small runtime
(the same compile-time transform the generators use), so all three backends
run identical code and behave identically.

### Declaring an effect

`effect fn` introduces either an **operation** (no body) or an **effectful
function** (with a body):

    effect fn log(msg)              # operation: performed, handled elsewhere
    effect fn greet(name) {         # effectful fn: may perform operations
      perform log("hi {name}")
      name
    }

An operation declaration only names the operation and its parameters; invoking
it directly is an error — it must be reached through `perform`.

An effectful function's body is lowered into a synthesized computation class,
the same shape a generator body takes, so `self` may not be referenced there
(`self is not available inside an effect fn body`) — pass what you mean as a
parameter, or bind it outside as `let me = self`. A `handle { ... }` body is
not restricted: it stays where it was written, so inside a method `self` is
still that method's receiver. Both promote their locals onto the synthesized
object, where a local holding a function stays a plain variable (§11).

### Performing an operation

`perform op(args)` suspends the current computation and transfers to the
nearest enclosing handler for `op`:

```culebra
effect fn ask()
let x = handle {
  let n = perform ask()
  n + 1
} with ask(resume) {
  resume(10)
}
inspect(x)  # => 11
```

### Handling

`handle { BODY } with op(params…, resume) { CLAUSE }` runs `BODY` with a
handler installed for the duration. When `BODY` (or anything it calls)
performs `op`, the matching clause runs with the operation's arguments bound to
the leading parameters and the **continuation** bound to the last parameter
(`resume` above) — a first-class function that resumes the performing code with
the value passed to it.

A handler may resume, or not — declining to call `resume` discards the rest of
the performing computation:

```culebra
effect fn fail()
let r = handle {
  perform fail()
  "unreachable"
} with fail(resume) {
  "aborted"
}
inspect(r)  # => 'aborted'
```

#### Multiple operations, and a `return` clause

One `handle` may carry several `with` clauses (one per operation) and an
optional `with return(v) { … }` that maps `BODY`'s normal-completion value:

```culebra
effect fn get()
effect fn put(v)
mut cell = 0
let out = handle {
  let a = perform get()
  perform put(a + 5)
  perform get()
} with get(k) { k(cell) }
  with put(v, k) { cell = v; k(nil) }
  with return(v) { "final={v}" }
inspect(out)  # => 'final=5'
```

The `return` clause applies only to *normal* completion. When a handler aborts
(never resumes), its own value is the result and `return` does not run.

### Resuming more than once (multi-shot)

A continuation is multi-shot: a handler may call `resume` any number of times,
and each call independently re-runs the rest of the performing computation from
its suspension point. This expresses nondeterminism / backtracking:

```culebra
effect fn choose(a, b)
let all = handle {
  let x = perform choose(1, 2)
  let y = perform choose(10, 20)
  x + y
} with choose(a, b, k) {
  [k(a), k(b)]
}
inspect(all)  # => [[11, 21], [12, 22]]
```

Forks share referenced heap values (arrays, objects) — the fork is a shallow
copy — while independent scalar state is copied per fork.

### Dynamic scope and effectful calls

Handlers are **dynamically** scoped: a `perform` reaches the nearest handler on
the current call stack, not the lexically nearest. Calling an effectful
`effect fn` from a handled body delegates into it, so its `perform`s reach the
same handlers:

```culebra
effect fn ask2()
effect fn double() {
  let n = perform ask2()
  n * 2
}
inspect(handle { double() } with ask2(k) { k(21) })  # => 42
```

### Plain functions and effects

Ordinary (non-`effect`) functions participate fully. A `perform` in a plain
fn dispatches straight off the dynamic handler stack, and a plain fn may call
an effect fn (and vice versa) — including through first-class uses like
`.map(f)`:

```culebra
effect fn ask()
fn greet() {
  # a plain fn performing an effect
  let name = perform ask()
  "hi " + name
}
inspect(handle { greet() } with ask(k) { k("ana") })  # => 'hi ana'
```

What makes this work is a classification of each handler clause at parse
time:

* **tail-resumptive** — `resume(v)` called exactly once, as the clause's
  final statement. The clause runs like a plain function call at the perform
  point; the native call stack is the continuation. This is the common case
  (dependency injection, state, mocking, logging). No continuation is
  captured, so a long chain of tail performs uses constant stack, and a
  `defer` in the clause fires when the clause returns — before the performing
  code proceeds — under both plain-fn and `effect fn` dispatch.
* **abort** — the clause never calls `resume`. Its result becomes the
  `handle`'s result: the stack between the perform point and the `handle`
  unwinds, running `defer`s on the way, and the unwind is **not** observable
  by `try/catch` in between (it is not an exception).
* **full-control** — anything else (multi-shot, a non-tail `resume`, storing
  or passing `resume`). The continuation must be captured, which only an
  `effect fn` body can support: reaching a full-control handler from a plain
  fn raises `EffectError`. This is the one remaining case where the
  `effect fn` marker is required.

The classification is deliberately conservative: any use of `resume` other
than a lone tail call classifies the clause as full-control (which is always
safe — it only means direct dispatch from plain code refuses it).

### Capturing an enclosing binding

A nested `handle` — written inside an effectful `effect fn` body or another
`handle` body — may read and write the enclosing computation's locals:

```culebra
effect fn outer()
effect fn inner()
effect fn work() {
  let base = perform outer()
  handle {
    base + perform inner()
  } with inner(k) { k(100) }
}
inspect(handle { work() } with outer(k) { k(5) })  # => 105
```

Effects compose with generators in both directions — a `handle` expression
inside a generator body, and a named generator fn inside an effect body:

```culebra
effect fn scale()
fn doubled() {
  yield handle { perform scale() * 2 } with scale(k) { k(10) }
  yield 7
}
inspect(doubled().collect())  # => [20, 7]
```

### Semantics and limitations

* Performing an operation with **no handler** raises `EffectError`.
* Reaching a **full-control** handler clause (multi-shot / non-tail `resume`)
  from a `perform` or effect-fn call in ordinary code raises `EffectError`:
  the native frames in between cannot be captured as a continuation. Route
  such performs through an `effect fn` run under the `handle`.
* Inside an **effect fn or `handle` body**, a `perform` is supported only in
  **statement position** or an unconditionally-evaluated operand. A `perform`
  in a short-circuit (`&&` / `||` / `??`) operand, a ternary arm, a
  method-chain receiver, or a control-flow condition is rejected at parse
  time (symmetrically on every backend). In a **plain fn**, a `perform` is an
  ordinary expression with no positional restrictions.
* Errors inside an effect body report the **original source line**; the
  column is approximate (the lowering shifts it). An unhandled `perform`'s
  `EffectError` carries the perform's line as `e.line`.
* Effects and generators **compose**: a named generator fn declared in an
  `effect fn` / `handle` body works (and may read the body's locals), a
  self-contained `handle { … }` expression works inside a generator body
  (including in a yielded expression or a loop), and a bare `perform` in a
  generator body dispatches dynamically — against the handlers installed at
  the `.next()` call that runs it. The remaining boundaries, rejected at
  parse time (symmetrically): a **bare `yield`** in an effect body (the body
  itself is not a generator — wrap the yield in a nested generator fn) and an
  **`effect fn` declaration** inside a generator body.
* A `defer` at an effect fn or `handle` body's statement level runs when the
  body is left by **any** path: normal completion, a `throw` unwinding through
  it, or an abort — whether a handler clause returns without resuming or an
  abort *signal* unwinds the driver from a plain-fn `perform` or a
  cross-handle abort. A tail or abort clause that exits by `throw` without
  resuming also runs the suspended body's defers; only a **full-control**
  clause's throw leaves them pending — it may have kept `resume`, and a kept
  continuation runs its defers when a resumed fork completes. A `defer`
  nested in control flow, or a `perform` inside a `defer`, is rejected at
  parse time.
* A **named fn** declared in an effect body must sit at the body's statement
  level (one buried in nested control flow is rejected at parse time); it may
  be a generator and may reference the body's locals.
* Handlers are **isolate-local**. A `handle` installed on one thread is not
  visible to a spawned isolate; a `perform` inside the child reaches only
  handlers installed within that same isolate (an unhandled operation there
  raises `EffectError`, which propagates to the parent on `join`). This
  follows the concurrency invariant that a script value — and a continuation
  is one — never crosses an isolate boundary.

---

## 17. Memory model

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
cycle collector that runs a mark-and-sweep periodically (an adaptive
threshold — see §25 for the exact schedule, which differs between
backends) and at program exit.

* Subject to collection: `Array`, `Object`, `Tuple`, `Set`, and the
  environments captured by closures (plus `Closure` / `Cell` in the
  JIT). Both backends reclaim every container cycle shape, including
  one routed purely through `Object` property maps.
* `String` is not refcounted. The interpreter manages it with a
  `shared_ptr` (freed deterministically, so it is never leaked). The
  JIT allocates string bytes from a per-`Runtime` slab and treats
  `String` as a **traced-only** value: it carries no refcount to hit
  zero, so the tracing sweep above is its sole reclaimer rather than
  a cycle-only backstop.

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
# doctest: skip
File.open = fn (path) {
  h = _native_open(path)
  {
    read: fn () {
      _native_read(h)
    },
    drop: fn () {
      _native_close(h)
    },  # called on scope exit
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

**Failed construction**: a class instance exists from the moment
`C.new(...)` is entered — before its arguments bind and before its
field initializers run — so a constructor call that throws anywhere
(a missing or wrong-typed argument, a throwing default, a field
initializer, the body) still drops the half-built instance. A
dispatch miss on an overloaded `new` throws before any instance is
built, so nothing is dropped there.

**Reentrant drop bodies**: a `drop` body may mutate the object's own
fields, including releasing a reference that is itself part of a
cycle with the object being dropped (`self.other = nil`). This is
safe — it cannot cause `drop` to fire a second time or corrupt the
cascade; `drop` runs exactly once no matter what its own body
releases.

**Cycles**: a reference cycle does not block `drop`. A cycle whose
members are owned by a scope — created under it and unreachable from
outside when it exits — is dropped **at that scope's exit**, newest
member first, on both backends:

```culebra
let log = []
make_thing = fn (id) {
  {id: id, drop: fn () {
    log.push(id)
  }}
}
{
  let a = make_thing('a')
  let b = make_thing('b')
  a.other = b
  b.other = a
}             # both drop at the block's exit, despite the cycle
inspect(log)  # => ['b', 'a']
```

The owning scope is wherever the cycle last escaped to: a cycle that
rides out of a function as its return value drops at the *caller's*
scope exit once discarded. A resource captured by a sibling
**closure**, or cycled through its own closure slot, also drops at the
owning scope's exit under the JIT and AOT; under the interpreter,
whole-env capture defers those shapes to its next **collection**
instead — the same drops, one collection later. Two shapes miss the
deterministic window on every backend: a closure-only cycle that holds
the resource without the resource referencing it back (e.g. a
recursive local `fn` capturing it), and cyclic garbage discarded
directly at the **top level** (a scope that never exits). Those fire
at the **GC backstop**:
a collection finalizes every orphaned resource exactly once — before
reclaiming memory, while the structure is still intact — in the
spirit of Python's PEP 442. Backstop timing is collection-driven
(force one with `GC.stat()`), and finalization order within one
collection is unspecified. The collection at **program exit** is the
exception: like top-level bindings (below), an orphan that survives to
exit is *not* finalized — its memory is reclaimed but `drop` does not
run, on every backend. When cleanup must happen at a known point,
break the cycle manually before the last reference is released (e.g.
`a.other = nil`), use an explicit `.drop()`, or `defer` (§15) at the
scope that owns the resource.

**Very large cycles**: the scope-exit cascade above has a size limit
tuned to keep ordinary programs synchronous; a scope that drops an
unusually large number of cycle-bearing objects at once (thousands,
not the handful in typical code) may not resolve all of them at that
scope's exit — the excess defers to the GC backstop instead, with the
same unspecified-order, PEP-442-style finalization described above.
Nothing is lost, only delayed; ordinary programs never approach this
size.

**Binding-scope caveat**: `drop` fires reliably when the object is
held in a **block-scoped** binding whose `drop` function was produced
by a **factory function** (so the closure captures the factory's
call env, not the surrounding scope). The idiom mirrors tutorial §5
closures-as-objects and works out of the box:

```culebra
make_thing = fn () {
  {drop: fn () {
    inspect('cleaned')
  }}  # captures make_thing's env
}
{
  let t = make_thing()  # block-scoped binding
}  # drop fires here
```

Binding a `drop`-bearing object at the **top level** leaves it alive
until program exit *without* running `drop`, on every backend: every
backend suppresses `drop` where the top-level scope is released, and
an **uncaught** error is the same exit — the scopes the error passes
through release deterministically (each one's `defer`s, then its own
bindings, innermost first), but the top level's own bindings still
leak un-dropped. For script-wide resources, prefer `defer` (§15) or
explicit cleanup, which run on both exits.

**JIT**: auto-drop fires under `--jit` with the same timing as the
interpreter — at scope exit, cycle members included. Closure-held
shapes also resolve at scope exit under the JIT (the interpreter
defers those to its next collection; backstop-only shapes as above);
top-level orphans go to the GC backstop on both, and top-level
*bindings* still leak un-dropped at program exit. The well-known property contract
(`drop`/`iter`/`has_next`/`next` must be a 0-arg `Function`) is enforced at
assignment time on both backends, on every write surface: literal
properties, computed subscript keys (`o[k] = v`), native builders
(`JSON.parse` etc.), values rebuilt on the receiving side of a channel
transfer, class declarations (a class whose method template binds a
well-known name to a non-conforming function — wrong arity, or an
overload set — raises `DropContractError` when the declaration runs),
and trait default implementations, which reach every conforming
instance and are checked where the `trait` is declared. A body-less
trait method only states a requirement, so it binds nothing and is not
checked.
When the target slot is immutable, `ImmutableError` wins over the
contract check, and a failed check leaves the old value in place.
`static` members are a namespace, not part of the instance protocol:
a static named `drop` (any arity) is an ordinary function — it is not
contract-checked and is never auto-invoked.

---

## 18. Built-in type methods

The methods below are part of the language: they are available on any
value of the corresponding type without any `import`, and cannot be
shadowed by user code (for `String`, which has no property store; for
`Array`/`Object`, a user-defined property of the same name wins and
the built-in is a fallback).

Global built-in functions (`inspect`, `to_string`, `Math.*`, `IO.*`,
matcher family `assert_true` / `assert_eq` / etc.) are specified
separately in [`docs/stdlib.md`](stdlib.md).

**Well-known method names (protocols).** Several method names are
recognized by the runtime and let plain `Object`s opt into language
features. They are checked by name on both backends:

| Method | Purpose | Defined in |
|---|---|---|
| `__add__`, `__sub__`, `__mul__`, `__div__`, `__mod__`, `__pow__`, `__matmul__`, `__neg__`, `__eq__`, `__lt__`, `__le__` | Operator overloading | §10 |
| `__str__` | Custom display form | §10 |
| `drop` | RAII cleanup hook | §17 |
| `iter`, `next` | Iterator protocol | §18.5 |
| `class` (property, not a method) | Nominal tag for `match` / debug | §10 |

Conventions:

* Types follow §14. `Any` denotes any value.
* Positional indices are zero-based. Negative indices where noted
  count from the end (`-1` is the last element).
* Length and indexing for `String` are in **bytes** (the Go model).
  Unicode-aware traversal is explicit: `iter()` / `code_points()` walk
  scalars and `graphemes()` walks extended grapheme clusters.

### 18.1 String methods

All string methods accept `String` or `StringView` as receiver (and
where shown, as argument — `StringLike`). They return new values; the
receiver is never mutated.

| Signature                                       | Description                          |
|-------------------------------------------------|--------------------------------------|
| `s.size() -> Long`                              | Byte length.                         |
| `s.empty() -> Bool`                              | Whether `size() == 0`.               |
| `s.presence() -> String \| StringView \| Nil`   | `s` unchanged if non-empty, else `nil` — pairs with `??`/`?.` for "use it if there's anything there" (`x.presence() ?? default`). |
| `s.upper() -> String`                           | Uppercase, full Unicode case mapping (UAX #21). A mapping may change the length: `'ß'.upper()` is `'SS'`. |
| `s.lower() -> String`                           | Lowercase, same mapping rules.       |
| `s.capitalize() -> String`                      | First letter titlecase, the rest lowercase (Python/Ruby `capitalize`). |
| `s.title() -> String`                           | Every word's first letter titlecase, the rest lowercase. Word boundaries are UAX #29, so `"o'neil".title()` is `"O'neil"` — the apostrophe does not start a word. |
| `s.normalize(form: StringLike = "NFC") -> String` | Unicode normalization: `"NFC"`, `"NFD"`, `"NFKC"`, or `"NFKD"`. Composed and decomposed spellings of the same text compare equal after `normalize()`; equality itself is by bytes (§8), so it does not normalize for you. Any other `form` is a `ValueError`. |
| `s.eq_ignore_case(other: StringLike) -> Bool`   | Default caseless matching (full case folding), so `'Straße'.eq_ignore_case('STRASSE')` is `true`. Canonical equivalence is deliberately not folded in — `normalize()` both sides for that. |
| `s.reverse() -> String`                         | Reversed by Extended Grapheme Cluster (UAX #29), so an emoji ZWJ sequence or a base+combining pair survives intact. A new String — unlike `Array.reverse()`, which mutates in place and returns `nil`. |
| `s.repeat(n: Long) -> String`                   | `n` copies concatenated. `n == 0` → `""`; a negative `n` is a `ValueError`, as is a result too large to allocate. |
| `s.truncate(max: Long, ellipsis: StringLike = "...") -> String` | `s` unchanged if it already fits in `max` bytes; otherwise cut so the result (content + `ellipsis`) is exactly `max` bytes. Byte-based like `slice`, so a multi-byte scalar can split at the cut point. `max < 0`, or too small to fit `ellipsis`, is a `ValueError`. |
| `s.trim() -> String`                            | Remove leading/trailing Unicode whitespace (the `White_Space` property, so NBSP and U+3000 count, not just ` `/`\t`/`\n`/`\r`). |
| `s.trim_start(chars: StringLike = "") -> String` | Trim from the start. No arg → whitespace; `chars` → leading scalars in that set (no ranges). |
| `s.trim_end(chars: StringLike = "") -> String`  | Trim from the end. e.g. `s.trim_end("\n")`. |
| `s.tr(from: StringLike, to: StringLike) -> String` | Per-scalar translation, character-list form — no `a-z` ranges or `^`. Each scalar of `s` found in `from` becomes the scalar at the same position in `to`; a shorter `to` repeats its last scalar, an empty `to` deletes. `s.tr("０１２３４５６７８９", "0123456789")`. |
| `s.split(sep: StringLike, limit: Long = 0) -> Array<StringView>` | Split on every occurrence of `sep`. Empty `sep` → `[s]`. Elements share a single source. `limit` caps how many pieces come back, the last one holding the remainder (`'a.b.c'.split('.', 2)` → `['a', 'b.c']`); `0` is uncapped and a negative one is a `ValueError`. |
| `s.rsplit(sep: StringLike, limit: Long = 0) -> Array<StringView>` | The same pieces, but `limit` is filled from the right: `'a.b.c'.rsplit('.', 2)` → `['a.b', 'c']`. The result stays in left-to-right order, and without a `limit` the two agree. |
| `s.split_whitespace() -> Array<StringView>`     | Split on runs of Unicode whitespace, with no empty piece at either end: `'  a  b '.split_whitespace()` is `['a', 'b']`, where `split(' ')` keeps the empties. |
| `s.split_once(sep: StringLike) -> Tuple \| Nil` | The two halves around the *first* `sep`, or `nil` when it does not occur — the shape a `key=value` parse wants, where `split` would break on a value containing `sep`. An empty `sep` never splits, so `nil`. |
| `s.rsplit_once(sep: StringLike) -> Tuple \| Nil` | The halves around the *last* `sep`, or `nil`. |
| `s.split_iter(sep: StringLike) -> Iterator<StringView>` | Lazy variant of `split`. Short-circuits with `.take(n)` over huge inputs. |
| `s.lines() -> Array<StringView>`                | Split on `\n`, `\r\n`, or `\r`. The terminator is dropped and a trailing one yields no final empty element, so `"a\nb\n".lines()` is `["a", "b"]` — the same line boundaries as `File.lines()`. Eager like `split`. |
| `s.replace(pat: String \| Regex, repl: String \| Function) -> String` | Replace **every** occurrence; chains. `pat` a `String` → literal; `pat` a `Regex` (incl. `re'…'`) → regex, so `repl` may be a `$1` / `$<name>` template or a `fn (Match) -> String`. A stdlib helper reached by UFCS — `s.replace(p, r)` is `replace(s, p, r)`. |
| `s.contains(sub: StringLike) -> Bool`           | Whether `sub` appears anywhere. Empty `sub` → `true`. |
| `s.count(sub: StringLike) -> Long`              | Non-overlapping occurrences of `sub`, so `"aaaa".count("aa")` is `2`. Empty `sub` → `0`, matching `split("")` yielding the receiver whole. Literal only — a compiled `Regex` counts its own matches. |
| `s.starts_with(prefix: StringLike) -> Bool`     | Whether `s` begins with `prefix`.    |
| `s.ends_with(suffix: StringLike) -> Bool`       | Whether `s` ends with `suffix`.      |
| `s.index_of(sub: StringLike, start: Long = 0) -> Long` | Byte offset of the first `sub` at or after `start`, or `-1` — the same "-1 means absent" convention `Array.index_of` uses. A negative `start` counts from the end, like `slice`'s indices. |
| `s.last_index_of(sub: StringLike) -> Long`      | Byte offset of the last `sub`, or `-1`. |
| `s.strip_prefix(prefix: StringLike) -> String`  | `s` without that exact prefix, or `s` unchanged when it is not there. Unlike `trim_start(chars)`, which trims a *set* of scalars, this matches the whole affix once. |
| `s.strip_suffix(suffix: StringLike) -> String`  | `s` without that exact suffix, or `s` unchanged. |
| `s.replace_first(pat: String \| Regex, repl: String \| Function) -> String` | Like `replace`, but only the first occurrence. Same `pat`/`repl` rules. |
| `s.is_digit() -> Bool`                          | Whether `s` is non-empty AND every scalar is a decimal digit (General_Category `Nd`, so fullwidth `'１２３'` counts). |
| `s.is_alpha() -> Bool`                          | Non-empty and every scalar is `Alphabetic`. |
| `s.is_alnum() -> Bool`                          | Non-empty and every scalar is `Alphabetic` or a number. |
| `s.is_space() -> Bool`                          | Non-empty and every scalar is `White_Space`. |
| `s.is_ascii() -> Bool`                          | Non-empty and every byte is below `0x80`. Empty is `false` here too — one rule for the whole `is_*` family rather than a per-method exception. |
| `s.slice(start: Long, end: Long) -> StringView` | Substring `[start, end)` borrowing from the receiver's bytes. Negative indices count from end; `start` clamped to `[0, size()]`, `end` to `[start, size()]`. |
| `s.view() -> StringView`                        | A view aliasing the receiver's bytes (no copy). |
| `s.to_string() -> String`                       | Materialize an owning `String` (no-op for `String`; copies for `StringView`). |
| `s.iter() -> Iterator<StringView>`              | Lazy walk yielding **one-scalar `StringView`s** (UTF-8 scalar). What `for c in s { ... }` uses internally. Invalid bytes yield as one-byte substrings. |
| `s.code_points() -> Iterator<Long>`             | Lazy walk yielding **Unicode scalar values** as `Long` (`U+0000`–`U+10FFFF`). For numeric / range / classification work where the per-scalar allocation of `iter` is wasteful. Invalid bytes yield as `0`–`255`. |
| `s.graphemes() -> Iterator<StringView>`         | Lazy walk yielding **Extended Grapheme Clusters** (UAX #29) — one user-perceived character per step (e.g. an emoji ZWJ sequence is a single element). |
| `s.bytes() -> Iterator<Long>`                   | Lazy walk yielding the receiver's **raw UTF-8 bytes** as `Long` (`0`–`255`), one byte per step — no decoding, unlike `code_points`. For when the encoding itself is wanted (hashing, tokenizer vocabularies, wire formats). |
| `String.from_code_point(cp: Long) -> String`    | The inverse of `code_points()`: one Unicode scalar value in, a one-character `String` out. Raises `ValueError` for `cp` above `U+10FFFF` or in the surrogate range `U+D800`–`U+DFFF` — the same boundary the `\u`/`\U` literal escapes (§4.1) reject at parse time. |
| `String.from_code_points(cps: Array) -> String` | The plural inverse of `code_points()`: an `Array` of Unicode scalar values in, a `String` out. Each element passes through the same gate as `from_code_point`, so `String.from_code_points([cp]) == String.from_code_point(cp)`; a non-`Long` element is a `TypeError`, an out-of-range one a `ValueError`. |
| `String.from_bytes(bytes: Array) -> String`     | The inverse of `bytes()`: an `Array` of raw byte values (`0`–`255`) in, a `String` out. **No UTF-8 validation**, and no error on malformed input: culebra `String`s tolerate invalid UTF-8 (same as `iter()`), so `String.from_bytes(s.bytes().collect()) == s` holds for every `String`, including ones with invalid sequences. A non-`Long` element is a `TypeError`, an out-of-range one (outside `0`–`255`) a `ValueError`. |

```culebra
# 'é' is 2 UTF-8 bytes, so 'café' is 5 bytes
inspect('café'.bytes().collect())                    # => [99, 97, 102, 195, 169]
inspect(String.from_code_point(233))                  # => 'é'
inspect(String.from_bytes([99, 97, 102, 195, 169]))   # => 'café'
inspect(String.from_code_points([99, 97, 102, 233]))  # => 'café'
```

#### StringView

`StringView` is a borrowed view over an owning `String`'s bytes.
`slice` / `split` / `view` / `iter` / `graphemes` return `StringView`
instead of `String` to avoid per-call copies. The view keeps its
source alive (shared ownership), so a view outlives the temp that
created it:

```culebra
let v = 'hello world'.slice(6, 11)
inspect(v)  # => 'world'
# Equality is by bytes, so a view compares equal to a String:
inspect(v == 'world')  # => true
inspect(type_of(v))    # => 'StringView'
# to_string() materializes an owned String:
inspect(v.to_string())  # => 'world'
```

Use `.to_string()` when you need an owning `String` (storing in a
data structure, returning from a long-lived function, etc.). Most
APIs declared with `StringLike` accept either flavor directly.

**Known limitation** (cycle B): calling `.contains()` / `.starts_with()`
/ `.ends_with()` etc. on a `StringView` materializes a temporary cstr
copy per call. It is reclaimed by the tracing collector like any other
`String` (§17), not leaked, but hot-looping over a large sequence of
views still means avoidable per-call allocation — materialize the view
once with `.to_string()` in that case. (Object-key normalization
between `String` and `StringView` is not a limitation — see §18.3.)

```culebra
inspect('hello'.size())              # => 5
inspect('HeLLo'.lower())             # => 'hello'
inspect('  hi  '.trim())             # => 'hi'
inspect('a,b,c'.split(','))          # => ['a', 'b', 'c']
inspect('hello'.slice(1, 4))         # => 'ell'
inspect('hello'.slice(-3, -1))       # => 'll'
inspect('hello world'.truncate(8))   # => 'hello...'
inspect(''.presence() ?? 'default')  # => 'default'

# Three views of the same string: bytes, scalars, clusters
inspect('café'.size())                 # => 5
inspect('café'.code_points().count())  # => 4
inspect('café'.graphemes().count())    # => 4

# Emoji ZWJ sequence: 5 scalars, 1 grapheme
inspect('👨‍👩‍👧'.code_points().count())  # => 5
inspect('👨‍👩‍👧'.graphemes().count())    # => 1

# Numeric ops via code_points
upper = 'Hello World'.code_points().filter(fn (cp) {
  cp >= 65 && cp <= 90
}).count()  # 2 ('H', 'W')
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

### 18.2 Array methods

Methods marked **mutating** modify the receiver in place and return
`nil` (except `pop` and `remove_at`, which return the removed
element); others return a new `Array` and leave the receiver
unchanged.

A callback that mutates the receiver is allowed, and the walk follows
the live array: the size is re-read before every step, so the walk ends
where the array ends. Shrinking the receiver ends the walk early;
growing it keeps the walk going. This is the same rule `for x in a`
follows (§12), and it covers `map`, `filter`, `for_each`, `reduce`,
`find`, `any`, `all`, and `flat_map`.

```culebra
mut a = [1, 2, 3, 4]
mut seen = []
a.for_each(fn (x) {
  seen.push(x)
  a.pop()
})
inspect(seen)  # => [1, 2]
```

| Signature                                   | Description                           |
|---------------------------------------------|---------------------------------------|
| `a.size() -> Long`                          | Number of elements.                   |
| `a.empty() -> Bool`                          | Whether `size() == 0`.                |
| `a.presence() -> Array \| Nil`               | `a` unchanged (same Array, not a copy) if non-empty, else `nil` — pairs with `??`/`?.` for "use it if there's anything there" (`xs.presence()?.join(",") ?? default`). |
| `a.push(x: Any) -> Nil` *(mutating)*        | Append `x` to the end.                |
| `a.pop() -> Any` *(mutating)*               | Remove and return the last element. `nil` if empty. |
| `a.extend(other: Array) -> Nil` *(mutating)* | Append every element of `other`. `a.extend(a)` appends the elements `a` had on entry. For `Tuple` / `Set` sources use spread (§9). |
| `a.insert(i: Long, x: Any) -> Nil` *(mutating)* | Insert `x` at position `i`, shifting the rest right. Negative `i` counts from the end, like `a[i]`; `i == a.size()` is the append slot. Out of range raises `IndexError`. |
| `a.remove_at(i: Long) -> Any` *(mutating)*  | Remove the element at `i` and return it, shifting the rest left. Negative `i` counts from the end. Unlike `insert`, `i == a.size()` is out of range — raises `IndexError`, as does any `i` on an empty array. |
| `a.get(i: Long, fallback: Any) -> Any`      | The element at `i` (negative counts from the end, like `a[i]`), or `fallback` if out of range. Read-only, never throws. |
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
| `a.sum() -> Long \| Float`                  | Sum of all elements, each `Long` or `Float`. Stays `Long` while every element is a `Long`, becomes `Float` once any element is one. Empty → `0`. |
| `a.product() -> Long \| Float`              | Product of all elements, promoting like `sum`. Empty → `1`. |
| `a.min() -> Any`                            | Smallest element, compared numerically across `Long` / `Float`. The element itself is returned, so its own type survives. Throws on empty. |
| `a.max() -> Any`                            | Largest element. Same rule as `min`. |
| `a.min_by(f: Function) -> Any`              | Element whose key `f(x)` is smallest; `f` must take one parameter and return a `Long` or `Float`. Each key is computed once, and ties keep the earlier element. Throws on empty. |
| `a.max_by(f: Function) -> Any`              | Element whose key `f(x)` is largest. Same rules as `min_by`. |
| `a.to_set() -> Set`                         | Fresh `Set` of the elements in first-seen order, duplicates dropped. Set literals aside, this is how a `Set` is built from a collection. Unhashable elements throw. |
| `a.to_object() -> Object`                   | Fresh `Object` from `(key, value)` tuples — the inverse of `Object.iter()`, so a table can be built as an expression instead of a `mut` + loop. Keys keep first-seen order and a repeat overwrites in place (last value, first position). Entries are **immutable**, like `group_by`'s; `{...built}` is the mutable copy. An element that is not a 2-tuple raises `TypeError`; an unhashable key throws like any other. |
| `a.group_by(f: Function) -> Object`         | Buckets elements into Arrays keyed by `f(x)`, in first-seen key order; `f` must take one parameter and return a hashable key. |
| `a.partition(p: Function) -> Tuple`         | One-pass split into `(matching, non_matching)`, order preserved in both halves. `p` must take one parameter. Destructures: `let (yes, no) = xs.partition(p)`. |
| `a.unzip() -> Tuple`                        | Split `(a, b)` pairs into `(Array, Array)` — the inverse of `zip`. Each element must be a 2-element `Tuple` or a `{first, second}` Object (either pair spelling is accepted); anything else raises `TypeError`. Destructures: `let (xs, ys) = pairs.unzip()`. |
| `a.sort(reverse: Bool = false) -> Nil` *(mutating)* | Stable-sort in place in natural order — elements compare by the same rule as `<`, so an Object's `__lt__` / `cmp` is honored (a `Path` array sorts) and incomparable elements throw (leaving the array as it was). Keyword-only `reverse: true` sorts descending (still stable). |
| `a.sorted(reverse: Bool = false) -> Array` | Like `sort` but returns a new sorted Array, leaving the receiver unchanged — so it chains (`xs.sorted().join(",")`). `reverse: true` for stable descending. |
| `a.sort_by(key: Function, reverse: Bool = false) -> Nil` *(mutating)* | Stable-sort in place using `key(x)` as the comparison key (ascending). `key` must take one parameter and return a comparable value (`Long` / `String` / `Bool`). Keyword-only `reverse: true` sorts descending (still stable). |
| `a.sorted_by(key: Function, reverse: Bool = false) -> Array` | Like `sort_by` but returns a new sorted Array, leaving the receiver unchanged — so it chains (`xs.sorted_by(f).join(",")`). `reverse: true` for stable descending. |

```culebra
mut a = [1, 2, 3]
a.push(4)
inspect(a.pop())  # => 4
a.extend([9, 8])
inspect(a)  # => [1, 2, 3, 9, 8]
a.insert(0, 0)
inspect(a)                             # => [0, 1, 2, 3, 9, 8]
inspect(a.remove_at(-1))               # => 8
inspect([1, 2] + [3])                  # => [1, 2, 3]
inspect([10, 20, 30, 40].slice(1, 3))  # => [20, 30]
inspect(['a', 'b', 'c'].join('-'))     # => 'a-b-c'
inspect([1, 2, 3].contains(2))         # => true
inspect([10, 20, 30].index_of(99))     # => -1
inspect([].presence() ?? 'default')    # => 'default'

inspect([1, 2, 3].map(fn (x) {
  x * x
}))  # => [1, 4, 9]
inspect([1, 2, 3, 4].filter(fn (x) {
  x % 2 == 0
}))  # => [2, 4]
inspect([1, 2, 3, 4].reduce(0, fn (acc, x) {
  acc + x
}))  # => 10

inspect([3, 1, 4, 1, 5].find(fn (x) {
  x > 3
}))  # => 4
inspect([1, 2, 3].any(fn (x) {
  x > 2
}))  # => true
inspect([1, 2, 3].all(fn (x) {
  x > 0
}))  # => true
inspect([1, 2, 3].flat_map(fn (x) {
  [x, x * 10]
}))  # => [1, 10, 2, 20, 3, 30]

mut words = ['banana', 'fig', 'apple']
words.sort_by(fn (s) {
  s.size()
})
inspect(words)  # => ['fig', 'apple', 'banana']
```

**Callback arity.** A higher-order method calls its callback with a fixed
number of arguments (one for `map` / `filter` / `find` / …, two for
`reduce`), and the callback must accept exactly that many. A function with
the wrong fixed arity is a `TypeError` — checked once before iterating, so it
fails even on an empty receiver:

```culebra
[1, 2, 3].reduce(0, |x| x)  # !! reduce expects a 2-parameter function
```

```culebra
[1, 2, 3].map(fn (a, b) {
  a
})  # !! map expects a 1-parameter function
```

A `*args` callback absorbs whatever it is given, so it works at any arity —
the per-call arguments arrive as the rest Array:

```culebra
inspect([10, 20].map(fn (*xs) {
  xs.size()
}))  # => [1, 1]
inspect([1, 2, 3].reduce(0, fn (a, *xs) {
  a + xs.size()
}))  # => 3
```

This is why `range` / `iota` (variadic builtins) can be passed directly as
callbacks. The rule is identical under the interpreter, `--jit`, and AOT.

**Callback parameter types.** A type annotation on a callback parameter is
enforced on every invocation, exactly like a direct call — the first
wrong-typed element raises a `TypeError`:

```culebra
[1, 'x'].map(fn (v: Long) {
  v * 2
})  # !! parameter 'v' expects Long
```

### 18.3 Object methods

| Signature                        | Description                                |
|----------------------------------|--------------------------------------------|
| `o.size() -> Long`               | Number of own properties.                  |
| `o.empty() -> Bool`               | Whether `size() == 0`.                     |
| `o.presence() -> Object \| Nil`  | `o` unchanged (same Object, not a copy) if non-empty, else `nil` — pairs with `??`/`?.` for "use it if there's anything there". |
| `o.keys() -> Array`              | `Array` of keys in insertion order (matches display order — §8). |
| `o.values() -> Iterator`        | Lazy iterator of values in insertion order — the value-only view of `o.iter()` (which yields `(key, value)` pairs). Chains / `collect`s like any iterator. |
| `o.has(key: String) -> Bool`     | Whether `o` has an own property named `key`, or (for a class instance) a method of that name. Ignores built-in method names. |
| `o.get(key, fallback) -> value`  | The value for `key`, or `fallback` if absent. Read-only — never inserts. |
| `o.get_or_put(key, init) -> value` *(mutating)* | The value for `key`; on a miss, store `init` and return it (sharing storage, so `o.get_or_put(k, [] ).push(x)` grows the stored array). When `init` is a function it is called lazily — only a miss pays for it: `o.get_or_put(k, || [])`. |
| `o.remove(key: String) -> Nil` *(mutating)* | Delete the property named `key` if present. |

A `String` and a byte-equal `StringView` (e.g. `s[0..2]`) are the **same key** — they hit the same slot across every operation above and `o[key] = v`.

```culebra
o = {b: 2, a: 1, c: 3}
# keys() and values() both walk in insertion order:
inspect(o.keys())              # => ['b', 'a', 'c']
inspect(o.values().collect())  # => [2, 1, 3]
inspect(o.has('a'))            # => true
# An absent key yields the fallback and is not inserted:
inspect(o.get('z', 0))  # => 0

# Group items by their first letter (StringView keys):
mut groups = {}
for w in ['apple', 'avocado', 'banana'] {
  groups.get_or_put(w[0..1], || []).push(w)
}
inspect(groups)  # => {mut a: ['apple', 'avocado'], mut b: ['banana']}

mut p = {a: 1, b: 2}
p.remove('a')
inspect(p)  # => {b: 2}
```

`o.iter()` yields `(key, value)` tuples and `to_object()` consumes them, so
a whole table is one expression — no `mut` accumulator to declare:

```culebra
mut prices = {apple: 100, banana: 80}
# Remap the values:
inspect(prices.iter().map(|(k, v)| (k, v * 2)).to_object())
# => {mut apple: 200, mut banana: 160}
# Invert it:
inspect(prices.iter().map(|(k, v)| (v, k)).to_object())
# => {mut 100: 'apple', mut 80: 'banana'}
```

### 18.4 Special identifiers

| Identifier | Visibility               | Meaning                              |
|------------|--------------------------|--------------------------------------|
| `fn`       | Inside every function    | The currently-executing function.    |
| `self`     | Inside method calls      | The method's receiver.               |

### 18.5 Iterator protocol

`for x in expr { ... }` (§12) requires `expr` to participate in the
iterator protocol. The protocol uses three **well-known method names**
on `Object` (and its subtype `Array`), with a `has_next` gate in front
of each `next`:

| Method | Shape | Called on | Returns |
|---|---|---|---|
| `iter` | `fn () -> Object` | an **Iterable** | an **Iterator** (may be `self`) |
| `has_next` | `fn () -> Bool` | an **Iterator** | whether another element is available |
| `next` | `fn () -> Any` | an **Iterator** | the next element (called only when `has_next()` was truthy) |

A built-in iterator called past its end yields `nil` rather than
raising, the same as a drained generator. Since `nil` is also a
perfectly good element, gate on `has_next()` to tell the two apart.

**Contract, enforced at property assignment**: binding `iter`,
`has_next` or `next` to a non-`Function` value, or to a function with
non-zero arity, raises `DropContractError: type error: '<name>' must be
a Function taking no arguments.` at the assignment site (mirrors the
`drop` contract — §17). The three names are reserved as a set, so a
protocol member always holds something callable; a lookalike data field
(`has_next_at`) is unaffected.

**Checked when the iterator is opened**: the `for`-in head validates
before the first step, and a lazy chain validates when a terminal
(`collect`, `count`, ...) first drives it — building the chain pulls
nothing, so that is where the error appears:

| Situation | Error | Reported at |
|---|---|---|
| `iter()` returned a non-`Object` | `TypeError: type error: iter() did not return an Object` | the iterable expression |
| iterator missing `has_next` or `next` | `TypeError: type error: iterator missing has_next()/next()` | the iterable expression, or the terminal that drove the chain |

**Optional `dispose`**: if the iterator has a zero-arity `dispose`, it
is called on every exit path — normal drain, `break`, early `return`, or
an exception leaving the loop, whichever side raised it: the loop body,
`has_next()` / `next()`, or a generator body that throws while suspended.
Generators use it to run the defers registered inside a suspended body.
A `dispose` that itself throws while an exception is already unwinding is
swallowed, so the enclosing `catch` still sees the original error.

The same contract covers the **terminal iterator methods**: whoever
drives the protocol closes it. Every terminal — draining
(`collect`, `count`, ...), early-exiting (`find`, `any`, `first`, ...),
or one that throws mid-drain (from its callback or the producer) —
calls `dispose` exactly once on the way out, after the result is
computed and before it is handed back. On the throwing paths the
original error propagates and a throwing `dispose` is swallowed; on a
successful drain a throwing `dispose` propagates, like `break`.

A lazy chain is one iterator from the consumer's side, so closing it closes
its source: `map` / `filter` / `take` / `zip` / … forward `dispose` to the
iterator(s) they pull from (both, for the two-source combinators). So
`for x in gen().map(f) { break }` runs `gen()`'s defers just like
`for x in gen() { break }` does. A chain over a source with no `dispose`
carries none itself.

    let countdown = fn (start) {
      mut i = start
      {
        iter:     fn () { self },
        has_next: fn () { i > 0 },
        next:     fn () { let v = i; i = i - 1; v }
      }
    }
    for v in countdown(3) { inspect(v) }   # 3, 2, 1

**Iterators are also Iterables**: an iterator's `iter` should return
itself, so `for x in some_iterator { ... }` works without a separate
Iterable wrapper.

**Built-in iterables**:

| Type | `iter()` yields | Order |
|---|---|---|
| `Array` | elements | index order (0..size-1) |
| `Object` | `(key, value)` tuples | insertion order (matches `o.keys()`) |
| `String` | one-scalar `String` per UTF-8 code point | byte order |

`String` iteration walks the UTF-8 buffer lazily, so iterating a
100 MB string with `break` after a few steps does not materialize the
rest. The yielded values are 1-scalar `String`s, not integer code
points — use `.map` on the iterator to project into whatever shape
you need.

Iterating an `Object` yields `(key, value)` pairs, so the
natural loop is `for k, v in obj` and `obj.iter()` is the entries view
(it chains and `collect`s like any iterator). The single-axis views are
`obj.keys()` (an `Array`) and `obj.values()` (a lazy iterator):

```culebra
for k, v in {a: 1, b: 2} {
  inspect("{k}={v}")
}  # 'a=1' then 'b=2'
for k in {a: 1, b: 2}.keys() {
  inspect(k)
}  # 'a' then 'b'
for v in {a: 1, b: 2}.values() {
  inspect(v)
}  # 1 then 2
```

**Object iter and mutation**: iteration is over a snapshot of the keys
taken at loop start, so mutating the object in the loop body is safe —
there is no fail-fast guard. Keys **added** during the loop
are not visited; keys **removed** are skipped; each value is read **live**
at its step. This makes the setdefault pattern — adding derived entries
while iterating — work without copying. The `for k, v in obj` sugar uses
the same protocol.

```culebra
mut o = {mut x: 1, mut y: 2}
for k, v in o.iter() {
  o[k] = 99
}             # update existing values
inspect(o.x)  # => 99
```

```culebra
mut books = {a: ('alpha', 1)}
for reading, n in books.values() {
  # add aliases while iterating
  if !books.has(reading) {
    books[reading] = (reading, n)
  }
}
inspect(books.has('alpha'))  # => true
```

**Iterator methods**: any Object exposing the iterator interface —
`next` together with `has_next`, or `iter` — picks up the lazy iterator
method set below, which drives the receiver through `has_next()` /
`next()`. This means a user `iter()` result (a plain `{has_next, next}`
object) and a generator (§11 "Generators") chain the same combinators
as a built-in iterator, not just `range`/array iterators. Non-terminal methods return
a new Iterator; terminal methods consume the iterator and return a
concrete value.

```culebra
fn nums() {
  yield 1
  yield 2
  yield 3
  yield 4
}
inspect(nums().filter(|x| x % 2 == 0).map(|x| x * 10).collect())  # => [20, 40]
```

| Non-terminal | Result | Notes |
|---|---|---|
| `it.map(f)` | Iterator | yields `f(x)` for each upstream `x` |
| `it.filter(p)` | Iterator | yields only `x` where `p(x)` is truthy |
| `it.take(n)` | Iterator | first `n` elements, then `done` |
| `it.skip(n)` | Iterator | discards first `n` elements on first `next()` |
| `it.take_while(p)` | Iterator | yields until the first `p(x)` that is falsy |
| `it.skip_while(p)` | Iterator | discards the leading run where `p(x)` is truthy, then yields the rest unconditionally |
| `it.step_by(n)` | Iterator | the first element, then every `n`-th one after it; `n` must be at least 1 |
| `it.distinct()` | Iterator | first occurrence of each element, later duplicates dropped (elements must be hashable) |
| `it.tap(f)` | Iterator | runs `f(x)` for its side effect and passes `x` through unchanged — a probe for a lazy chain |
| `it.scan(init, f)` | Iterator | running fold: yields `acc = f(acc, x)` at each step, starting from `init`. `init` itself is not yielded, so the output length matches the input's |
| `it.flatten()` | Iterator | removes one level of nesting; each element must be iterable (same coercion as `flat_map`) |
| `it.chunk_by(f)` | Iterator | groups **adjacent** elements sharing a key `f(x)` into Arrays. A key that reappears after a different one starts a new run — unlike `group_by`, which buckets by key regardless of position |
| `it.chunks(n)` | Iterator | groups elements into Arrays of `n` (the last group may be shorter); `n` must be at least 1 |
| `it.windows(n)` | Iterator | sliding window of the last `n` elements as an Array, advancing by one each step; `n` must be at least 1 |
| `it.flat_map(f)` | Iterator | `f(x)` must return an iterable; results concatenated |
| `it.chain(other)` | Iterator | yields `it` then `other` |
| `it.zip(other)` | Iterator | yields `{first, second}` pairs; stops at the shorter side |
| `it.enumerate()` | Iterator | yields `(index, value)` tuples with `index` starting at `0`. Also a direct `Array` method (`arr.enumerate()`) returning a lazy iterator. |

| Terminal | Result | Notes |
|---|---|---|
| `it.collect()` | `Array` | materialize into an Array |
| `it.join(sep)` | `String` | concatenate elements with `sep` between them (each rendered as by `to_string`); like `Array.join`, so `xs.map(...).join(",")` needs no intermediate `.collect()` |
| `it.for_each(f)` | `Nil` | invoke `f(x)` for side effects |
| `it.reduce(init, f)` | Any | left fold: `acc = f(acc, x)` starting from `init` |
| `it.find(p)` | Any \| `nil` | first `x` where `p(x)` is truthy, else `nil` |
| `it.any(p)` | `Bool` | `true` if any `p(x)` is truthy |
| `it.all(p)` | `Bool` | `true` if every `p(x)` is truthy (empty → `true`) |
| `it.count()` | `Long` | number of elements consumed |
| `it.first()` | Any \| `nil` | first element, else `nil`; pulls only one |
| `it.last()` | Any \| `nil` | last element, else `nil` |
| `it.nth(n)` | Any \| `nil` | element at 0-based `n`, else `nil`; pulls only `n + 1`. Negative `n` raises `ValueError` |
| `it.position(p)` | `Long` \| `nil` | 0-based index of the first `x` where `p(x)` is truthy, else `nil` (unlike `Array.index_of`, which answers `-1`) |
| `it.contains(v)` | `Bool` | `true` if some element equals `v` (structural equality, as `Array.contains`) |
| `it.sum()` | `Long` \| `Float` | sum of all elements; `Long` while every element is a `Long`, `Float` once any element is one (empty → `0`) |
| `it.product()` | `Long` \| `Float` | product of all elements, promoting like `sum` (empty → `1`) |
| `it.min()` | Any | smallest element, compared numerically; the element is returned, so its own type survives. Throws on empty |
| `it.max()` | Any | largest element, same rule as `min` |
| `it.min_by(f)` | Any | element with the smallest key `f(x)`; ties keep the earlier one. Throws on empty |
| `it.max_by(f)` | Any | element with the largest key `f(x)`; ties keep the earlier one. Throws on empty |
| `it.to_set()` | `Set` | members in first-seen order, duplicates dropped |
| `it.to_object()` | `Object` | `(key, value)` tuples into an Object — the inverse of `Object.iter()`. Keys in first-seen order, a repeat overwriting in place; entries immutable. A non-2-tuple element raises `TypeError` |
| `it.group_by(f)` | `Object` | buckets elements into Arrays keyed by `f(x)`, in first-seen key order |
| `it.partition(p)` | `Tuple` | `(matching, non_matching)` in one pass, order preserved in both halves |
| `it.unzip()` | `Tuple` | `(Array, Array)` — the inverse of `zip`. Each element must be a 2-element `Tuple` or a `{first, second}` Object (either pair spelling is accepted); anything else raises `TypeError` |

Every terminal disposes the iterator it drove (see **Optional
`dispose`** above), including the early-exiting ones — so after
`it.find(p)` the source is closed, and a follow-up terminal on the same
`it` yields nothing (`[]` from `collect`), exactly as after a `break`:

```culebra
fn g() {
  yield 1
  yield 2
  yield 3
}
let it = g()
inspect(it.find(|x| x == 2))  # => 2
inspect(it.collect())         # => []
```

The `find` also ran `g()`'s defers; the empty second answer is the
disposed source reporting done.

**Eager vs lazy**: `Array` has its own eager `map` / `filter` /
`for_each` / `reduce` / `find` / `any` / `all` / `flat_map` (§18.2)
which all return a new `Array`. Calling them on an `Array` dispatches
to the eager versions; call `.iter()` first to opt into the lazy
chain. The eager form is the default because a chain that materialises
each step is easier to reason about; laziness is what you ask for when
the sequence is large or unbounded.

```culebra
# doctest: skip
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
    iter: fn () {
      self
    },  # Iterator is its own Iterable
    has_next: fn () {
      i > 0
    },
    next: fn () {
      v = i
      i -= 1
      v
    },
  }
}

for x in countdown(3) {
  inspect(x)
}  # 3, 2, 1
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

## 19. Core built-in functions

The functions below are part of the language proper: they are bound
into every execution environment as global names and cannot be
replaced. The first group (`to_long` / `to_float` / `to_string`,
`type_of`) is tied to language semantics — source-position errors,
type introspection, and the display convention. The second group
(`range`, `iota`, `grid`) provides the canonical integer-sequence
factories; both backends recognise `range`/`iota` for fusion /
specialisation, and they are the standard form used in `for`-in
loops throughout the language. `repeat` is a related but separate
eager `Array` constructor — `n` copies of one value rather than an
integer sequence. The matcher family (`assert_true` /
`assert_eq` / `assert_throws` / `assert_close` / etc.) is a third
group of globals — see
[`docs/stdlib.md`](stdlib.md) for the full reference. The broader
standard library (namespaced under `Math`, `IO`, `Sys`) is also
documented in `stdlib.md`. Output primitives `inspect`, `print`, and
`println` are CLI-installed globals (§22).

All of these globals are **first-class values**: bind one to a variable
or hand it to a higher-order function and it behaves like any closure,
on both backends.

```culebra
inspect([1, 2, 3].map(type_of))                     # => ['Long', 'Long', 'Long']
inspect([1, 2, 3].map(range).map(|r| r.collect()))  # => [[0], [0, 1], [0, 1, 2]]
let f = range
inspect(f(0, 10, step: 2).collect())  # => [0, 2, 4, 6, 8]
```

A direct call is still the fast path; the closure form is used only
when the name appears in value position. `range` / `iota` accept their
1-2 positional bounds and `range`'s keyword-only `step` through that
closure too (`range(0, 10, **{step: 2})` works), with the same
`ArityError` / unknown-keyword diagnostics as the direct call.

### `to_long(v: Any, *, base: Long = 10) -> Long`

Convert `v` to `Long`:

* `Long` → itself.
* `Float` → truncated toward zero.
  `to_long(3.7) == 3`, `to_long(-3.7) == -3`.
* `String` → parsed as a signed integer in `base`; leading/trailing
  whitespace is allowed, anything else fails.
* Other types raise `type error`.

`base` is keyword-only and ranges over 2–36. For base 16 / 8 / 2 the
matching `0x` / `0o` / `0b` prefix is accepted, so a literal copied out
of source parses as itself. Naming a `base` for a non-String `v` is an
error rather than a silently ignored argument — keyword-only also keeps
`to_long` a one-parameter function, so `map(to_long)` still binds.

**Throws**: `type error at L:C.` on an unparseable string, a
non-numeric / non-string argument, or a `base` given for one;
`ValueError` for a `base` outside 2–36.

```culebra
inspect(to_long('42'))             # => 42
inspect(to_long('-7'))             # => -7
inspect(to_long(3.9))              # => 3
inspect(to_long('ff', base: 16))   # => 255
inspect(to_long('0b1010', base: 2))  # => 10
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
inspect(to_float(3))       # => 3.0
inspect(to_float('1.5'))   # => 1.5
inspect(to_float('1e-5'))  # => 1e-05
```

### `to_string(v: Any) -> String`

Convert `v` to its display form (same formatting as interpolation
inserts — strings come through unquoted). See §8 for the display
convention. `Float` uses the shortest round-trip decimal and always
carries either a decimal point or an exponent, so the type is
visually distinguishable from `Long`.

```culebra
inspect(to_string(42))      # => '42'
inspect(to_string(1.0))     # => '1.0'
inspect(to_string(1e-5))    # => '1e-05'
inspect(to_string([1, 2]))  # => '[1, 2]'
inspect(to_string('hi'))    # => 'hi'
```

### `type_of(v: Any) -> String`

Return the runtime type name of `v`. One of
`'Nil'`, `'Bool'`, `'Long'`, `'Float'`, `'String'`, `'Array'`,
`'Object'`, `'Function'`, `'Tensor'`, `'Tuple'`, `'Set'`.

```culebra
inspect(type_of(42))      # => 'Long'
inspect(type_of(1.5))     # => 'Float'
inspect(type_of('hi'))    # => 'String'
inspect(type_of([1, 2]))  # => 'Array'
inspect(type_of((1, 2)))  # => 'Tuple'
inspect(type_of({1, 2}))  # => 'Set'
```

### `range(n: Long, *, step: Long = 1) -> Iterator` / `range(start: Long, end: Long, *, step: Long = 1) -> Iterator`

Lazy integer-sequence factory: returns an Iterator (§18.5) that
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
for i in range(5) {
  inspect(i)
}  # 0, 1, 2, 3, 4
for i in range(2, 6) {
  inspect(i)
}  # 2, 3, 4, 5
for i in range(0, 10, step: 2) {
  inspect(i)
}  # 0, 2, 4, 6, 8
for i in range(5, 0, step: -1) {
  inspect(i)
}  # 5, 4, 3, 2, 1

# Constant memory even for huge bounds
for i in range(1000000000) {
  if i > 3 {
    break
  }
  inspect(i)
}
```

**JIT**: `range` returns a JIT-native iterator Object, and the
`range(N).<HOF>(...)` method-chain pattern is fused into a direct
counter loop. See §18.5.

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
inspect(iota(3))     # => [0, 1, 2]
inspect(iota(2, 5))  # => [2, 3, 4]
inspect(iota(5, 2))  # => []
```

### `repeat(n: Long, value: Any) -> Array`

Materialise an `Array` of `n` copies of `value`. `n < 0` raises
`ValueError`; `n == 0` returns `[]`. Every copy is the same
`value` — if it's a mutable reference (`Array`/`Object`), all `n`
slots alias one instance, the same sharing `[value] * n` gives in
Python or `Array(n).fill(value)` in JavaScript.

```culebra
inspect(repeat(3, 0))    # => [0, 0, 0]
inspect(repeat(0, "x"))  # => []

let shared = repeat(2, [])
shared[0].push(1)
# Both slots are the same Array
inspect(shared)  # => [[1], [1]]
```

### `grid(x_range: Range, y_range: Range) -> Iterator`

Lazy cartesian-product factory over two bounded integer ranges:
returns an Iterator (§18.5) that yields `(x, y)` Tuples, `x` varying
fastest — the same order the nested loop `for y in y_range { for x
in x_range { ... } }` walks, in one line and constant additional
memory. Both arguments must be bounded `Range` values (`a..b` /
`a..=b`, `by` step); an open-ended range or `step: 0` raises the
same errors `for`-in over a Range does. If either range is empty,
the whole product is empty.

```culebra
for (x, y) in grid(0..3, 0..2) {
  inspect((x, y))
}  # (0,0), (1,0), (2,0), (0,1), (1,1), (2,1)
```

```culebra
inspect(grid(0..2, 0..2).collect())
# => [(0, 0), (1, 0), (0, 1), (1, 1)]
# An empty x or y range makes the whole product empty.
inspect(grid(0..0, 0..3).collect())  # => []
```

### `__ARGS__` (variadic catch-all binding)

Inside any function body, the implicit local `__ARGS__` is bound to
an `Array` of positional arguments that overflowed the declared
parameters. Use it when a fn takes a variable number of trailing
values without declaring an explicit `**rest` (which catches
*keyword* args, not positional).

```culebra
let logger = fn (level) {
  inspect("[{level}] " + __ARGS__.join(' '))
}
logger('info', 'building', 'fizzbuzz')  # → '[info] building fizzbuzz'
```

Because that overflow always has somewhere to land, passing a user
function more positional args than it declares is **not** an error —
unlike a built-in, which raises `ArityError` (§15). A bare `*` in the
parameter list is the way to cap positionals on a user function:
overflow past it is a `TypeError` rather than reaching `__ARGS__`.

`__ARGS__` does **not** receive keyword arguments — those go through
the explicit param list or `**rest`. Prefer the explicit `*args`
parameter (see *Parameters*) when you want a *named* overflow binding
and variadic dispatch; `__ARGS__` is the implicit form that always
accompanies it.

### Function introspection

Every `Function` value exposes three read-only properties for
introspection:

| Property        | Type           | Description                                                                 |
|-----------------|----------------|-----------------------------------------------------------------------------|
| `fn.name`       | `String`       | Source-level declaration name (`fn name(...)`) or `""` for anonymous fns    |
| `fn.return_type`| `String`       | Return-type annotation (`fn f() -> X`) or `""` if unannotated               |
| `fn.params`     | `Array<Object>`| Per-parameter metadata; each entry has `name / mut / type / has_default / kw_only / kwargs_rest` |

```culebra
fn greet(name: String, *, prefix = "hi") {
  "{prefix}, {name}"
}

inspect(greet.name)         # => 'greet'
inspect(greet.return_type)  # => ''
let ps = greet.params
inspect(ps.size())          # => 2
inspect(ps[0].name)         # => 'name'
inspect(ps[0].type)         # => 'String'
inspect(ps[1].kw_only)      # => true
inspect(ps[1].has_default)  # => true
```

`fn.params` returns a fresh `Array` each access; mutating it has no
effect on the function. Multifn dispatchers expose the **first
registered method's** signature (overloads after the first do not
appear).

---

## 20. Multimethods

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

**Default parameters and arity.** A method with default parameters
matches any call whose positional-argument count is between its
required count (params without a default) and its total param count;
the unsupplied tail is filled from the defaults. Among equally
type-specific matches, the one that fills fewer parameters by default
wins (a more exact arity is more specific).

A **keyword argument** may also cover a required parameter that the
positional arguments didn't fill — keywords contribute to *which
methods are applicable*. Selection itself still scores on the
positional arguments only, so two overloads that differ solely by a
keyword (or keyword-supplied type) are **ambiguous**, not silently
disambiguated. A required parameter supplied by neither a positional
argument nor a keyword leaves the call unmatched (`DispatchError`).

```
fn at(a, b = 10) { a + b }
at(1)              # → 11   (b defaulted)
at(1, 2)           # → 3
at(1, b: 2)        # → 3    (b by keyword)
at(a: 1, b: 2)     # → 3    (a — required — covered by keyword)
at(b: 9)           # !! DispatchError — required `a` not supplied
```

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
class Square { new(side) { self.side = side } }
class Circle { new(r)    { self.r    = r    } }

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

Overwriting is confined to one **execution** of the declaring scope. A
declaration inside a function or a loop body builds a fresh overload set
every time it runs, so each body keeps the captures of the activation that
declared it — an earlier `fn` value is never re-pointed at a later run's
bodies. The same holds for the class overload sets below.

```
fn make(tag) {
  fn m(a)    { "{tag}-one" }
  fn m(a, b) { "{tag}-two" }
  m
}
let a = make("A")
let b = make("B")
[a(1), b(1)]  # → ["A-one", "B-one"]
```

### Coexistence with existing features

* Ordinary local bindings `let f = fn(...) {...}` continue to work
  unchanged.
* Method dispatch on `obj.method()` is unaffected. Methods are still
  defined inside a `class` body and called through `self.`.
* A method whose parameters are all `Any` serves as a catch-all.

### Constraints

* **Top-level / free functions only.** Nested declarations inside a
  block, and class methods, are not subject to this mechanism.
* **Errors.** With no matching method the runtime raises
  `no matching method`; with a tie in specificity it raises
  `ambiguous dispatch`. Both halt the program immediately rather than
  surfacing as catchable runtime exceptions (§15, §24).

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

### Method multidispatch (instance methods)

A class may declare several instance methods with the **same name but
different positional-param-type signatures**. They merge into one
dispatcher; `obj.method(args)` then picks the overload on the runtime
types of the explicit arguments. The receiver is fixed by the property
lookup — only the arguments are scored — and `self` is bound into the
picked overload:

```culebra
class Calc {
  new() {}
  go(x: Long) {
    "long: {x}"
  }
  go(x: String) {
    "string: {x}"
  }
  go(x: Long, y: Long) {
    "sum: {x + y}"
  }
}
let c = Calc.new()
c.go(1)     # → "long: 1"
c.go("a")   # → "string: a"
c.go(2, 3)  # → "sum: 5"
```

The same scoring, default-param, `*args`, kwarg, and `**rest` rules as
free-function multimethods apply (a keyword argument flows into the
picked overload). A call with no matching overload raises a catchable
`DispatchError`, exactly like a free-function multimethod.

A class declaring a name **once** keeps a plain method (no dispatcher,
no overhead). The following stay compile-time errors: two methods with
an identical signature, a field and a method sharing a name, and a
duplicate field. Constructors (`new`) and operator/dunder methods
(`__add__`, `__eq__`, `__call__`, …) overload too (see below).

### Method multidispatch (static methods)

Static methods overload the same way. Same-named `static` methods with
different positional-param-type signatures merge into one dispatcher on
the class object; `Cls.method(args)` picks on the argument types. No
`self` participates — a static call binds none:

```culebra
class Vec {
  static make(x: Long) {
    "long: {x}"
  }
  static make(x: String) {
    "str: {x}"
  }
  static make(x: Long, y: Long) {
    "pair: {x}, {y}"
  }
}
Vec.make(5)     # → "long: 5"
Vec.make("hi")  # → "str: hi"
Vec.make(1, 2)  # → "pair: 1, 2"
```

Static and instance overload sets are independent: a static and an
instance method may share a name, each with its own overloads. Two
static methods with an identical signature, and a static field clashing
with a static method name, stay compile-time errors.

### Constructor multidispatch (`new`)

A class may declare several `new` bodies with different
positional-param-type signatures. `C(args)` (or `C.new(args)`) dispatches
on the runtime types of the arguments, exactly like method overloads:

```culebra
class Point {
  new(x: Long, y: Long) {
    self.tag = "xy"
    self.x = x
    self.y = y
  }
  new(s: String) {
    self.tag = "str"
    self.x = s.size()
    self.y = 0
  }
  new() {
    self.tag = "empty"
    self.x = -1
    self.y = -1
  }
}
Point(3, 4).tag  # → "xy"
Point("hi").tag  # → "str"
Point().tag      # → "empty"
```

The overload is picked **before any instance is allocated**, so a call
matching no overload raises a catchable `DispatchError` with no side
effects — no instance is built and no field initializer (§10) runs. When
an overload is picked, its declared field initializers run once, then its
body, with `self` immutable throughout (a `self = …` reassignment raises
`ImmutableError` in every overload, as for a single `new`).

Default parameters, keyword arguments, and `*args` follow the same rules
as method overloads. A class declaring `new` **once** keeps a plain
constructor (no dispatcher, no overhead); two `new` bodies with an
identical signature stay a compile-time error.

### Dunder / operator multidispatch

Operator methods (`__add__`, `__eq__`, `__lt__`, `__index__`, `__call__`,
…, §10) are ordinary instance methods reached through the operator-lookup
path, so several with distinct operand-type signatures merge into one
dispatcher; the operator then picks the overload on the operand's runtime
type:

```culebra
class Vec {
  new(x: Long, y: Long) {
    self.x = x
    self.y = y
  }
  __add__(o: Vec) {
    Vec(self.x + o.x, self.y + o.y)
  }  # elementwise
  __add__(n: Long) {
    Vec(self.x + n, self.y + n)
  }  # scalar
}
let v = Vec(1, 2)
v + Vec(10, 20)  # → Vec(11, 22)   (Vec overload)
v + 5            # → Vec(6, 7)     (Long overload)
```

Commutative auto-reflection is unaffected: `5 + v` still reflects to
`v.__add__(5)` and picks the `Long` overload. An operand matching no
overload raises a catchable `DispatchError` (kind, message, and position
agree across backends) — the same rule as a method with a typed parameter
that the operand doesn't satisfy. A class declaring a dunder **once** keeps
a plain method; two dunder bodies with an identical signature stay a
compile-time error.

---

## 21. Decorators

A `@expr` line before a `fn` or `class` declaration wraps the
declared value through `expr` before binding it to the original name.
Stacked decorators apply bottom-up — the one closest to the
declaration runs first, the topmost runs last:

```culebra
# doctest: skip
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
let tag = fn (f) {
  fn () {
    "[{f()}]"
  }
}

@tag
fn greet() {
  "hello"
}

greet()  # "[hello]"
```

For a class, the decorator gets the class object and returns the
object the variable will hold:

```culebra
let mark = fn (cls) {
  cls.marked = true
  cls
}

@mark
class Point {
  new(x, y) {
    self.x = x
    self.y = y
  }
}

Point.marked  # true
```

### Interaction with multimethods

A decorated `fn name(...)` does **not** participate in multimethod
dispatch — the decorator's return value binds directly to `name`.
Combine the patterns by writing the multimethod first and then a
separate decorated wrapper if you need both.

### Built-in decorator: `@packable`

`@packable` is not a callable — it marks a class as a **fixed-layout
value type**. Its fields carry a scalar type annotation with an optional
default (`x: Float32 = 0.0`), and the decorator fixes their byte layout
(C-ABI alignment). Packable classes back `SharedBuffer`, the zero-copy
buffer shared across isolates — see
[stdlib §12 SharedBuffer](stdlib.md#sharedbuffer--zero-copy-shared-fixed-layout-data).

### Why `@` doesn't collide with the matmul operator

`@` is also the binary matrix-multiplication operator (PEP 465).
The two uses never overlap: decorator `@` only matches at statement
prefix position, while matmul `@` is parsed inside an expression
between two operands and never crosses a newline. So
`}\n@deco fn ...` is unambiguously two statements (a decorator on a
fresh declaration), not a matmul continuation of the preceding
expression.

---

## 22. Command-line interface

    culebra [flags] [script.cul | -] [arg ...]
    culebra <command> [command-flags]

Everything after the script path is captured verbatim and exposed to the
script as `Sys.argv` — no `--` needed; see
[`docs/stdlib.md`](stdlib.md). A standalone `--` before the script is an
optional escape hatch: it stops flag parsing, so the next argument is taken
as the script even if it begins with a dash.

`-` reads the script from stdin instead of a file, e.g.
`curl ... | culebra -`. `Sys.script` is `nil` there, same as in the REPL; a
file actually named `-` is still reachable by spelling the path, e.g. `./-`.

### Flags

| Flag           | Effect                                                    |
|----------------|-----------------------------------------------------------|
| `--shell`      | Start the REPL (the default when no script is given).     |
| `--ast`        | Print the parsed AST instead of running it.               |
| `--debug`      | Print debug diagnostics while running.                    |
| `--jit`        | Use the LLVM ORC JIT instead of the tree-walking interpreter. |
| `--jit-faststart` | Like `--jit`, but skips both the IR and the machine-code optimizers, cutting JIT warmup time ~40x at a small steady-state throughput cost (~12% on pure-script hot loops, ~0% when hot work is in the C++/BLAS runtime). Implies `-O0`. Output matches `--jit`/interp. |
| `--emit-llvm`  | With `--jit`, print the generated IR and exit.            |
| `-O0`..`-O3`   | With `--jit`, select the LLVM optimization level. Default `-O2`. |
| `-h`, `--help` | Print the option / command summary and exit.              |
| `--version`    | Print the version and available backends, then exit.      |

### Subcommands

The binary carries the toolchain as well. Six subcommands exist; the
development four are documented in [`tooling.md`](tooling.md) and the two
packaging ones in [`deployment.md`](deployment.md).

| Command | Effect | Reference |
|---|---|---|
| `test [paths...]` | Run tests and doctests (`--filter`, `--doc`, `--reporter`, `--bail`, `--list`). | [`tooling.md` §1](tooling.md#1-testing-culebra-test) |
| `lint [paths...]` | Report static errors and warnings without running (`--fix` removes unused imports). | [`tooling.md` §2](tooling.md#2-linting-culebra-lint) |
| `fmt [paths...]` | Reformat source to the canonical style (`-i` in place, `-l` list, `--check`). | [`tooling.md` §3](tooling.md#3-formatting-culebra-fmt) |
| `dap` | Speak the Debug Adapter Protocol over stdin/stdout; an editor launches it. | [`tooling.md` §4](tooling.md#4-debugging-culebra-dap) |
| `build <in.cul> -o <out>` | Compile ahead-of-time into a standalone executable (§24 covers the module graph it bundles; `culebra build --help` lists the cross-compile flags). | [`deployment.md` §1](deployment.md#1-standalone-binary-build-culebra-build) |
| `wrap` | Build an extended `culebra` binary that exposes your own C++ classes as builtins. | [`deployment.md` §3](deployment.md#3-wrapping-c-libraries-culebra-wrap) |


If no script is provided, the REPL is launched automatically. It
always runs on the interpreter — a prompt line is never a hot loop,
so `--jit` applies to scripts only and passing it here just prints a
note. Session state is preserved across inputs: `let`, `mut`, and
`fn` declarations from one input are visible to subsequent inputs,
including from inside nested closures.

Each accepted input's result is echoed, except when it is `nil` —
`println(...)`, a `fn` declaration and an `if` without an else branch
all evaluate to nil, and echoing it would double every line of output.
Output written by the program itself (`println`, `print`) is unaffected.

The REPL persists input history across sessions. The path is
`$CULEBRA_HISTFILE` if set, otherwise `$XDG_STATE_HOME/culebra/history`
when defined, otherwise `~/.culebra_history`. History is rewritten
after each accepted line so a crash mid-session doesn't lose it.

### CLI-installed globals

The CLI binary adds three globals to the script environment before
running user code:

| Global    | Aliased to    |
|-----------|---------------|
| `inspect` | `IO.inspect`  |
| `print`   | `IO.print`    |
| `println` | `IO.println`  |

These are convenience shortcuts for the most common output calls;
they point to the same function values that live under `IO`, so
`inspect(x)` and `IO.inspect(x)` are fully equivalent. Embedders that use
`culebra::environment()` directly do not receive these aliases —
their environment contains only `Math`, `IO`, `Sys`, and the core
built-ins from §19.

---

## 23. Known limitations

* No big integers or bignums; `Long` overflow wraps.
* Single-quoted `'...'` strings are raw (no escapes, no interpolation,
  no embedded apostrophes). Double-quoted `"..."` strings recognize a
  fixed escape set (`\n \r \t \\ \" \{`); the `{{` / `}}` form for
  literal braces is not supported.
* `String` is byte-indexed (`size` / `slice` count bytes); Unicode
  work goes through `code_points()` / `graphemes()` iterators.
* `Array` / `Object` / `Tuple` / `Set` equality is structural (by value);
  only `Function` / `Tensor` compare by reference identity.
* Type annotations are enforced only at function boundaries and
  annotated assignments; they do not make the language static.
* Pattern matching has no exhaustiveness check.
* Dot-form property names are identifiers only (`obj.foo`). Non-String
  hashable keys (`Long`, `Float`, `Bool`, `Nil`, `Tuple`) reach the
  Object via the subscript path (`obj[k]`) and live in a sidecar map.
  Runtime `String` keys via `obj[k]` unify with the shape — `obj['x']`
  and `obj.x` reach the same slot. See §10 "Subscript assignment".
* Only `fn name(...)` declarations can be generators; `yield` anywhere
  else — a class method, an object property's function, a `fn`
  expression, or the top level of a file — is a parse-time
  `SyntaxError` (§11 "Generators"). A method that needs to yield
  delegates to a named fn declared beside it.

---

## 24. Modules

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
      new (x, y) { self.x = x; self.y = y }
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

## 25. Appendix: interpreter ↔ JIT divergence

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
  `__str__` display (§10), and the iterator protocol (§18.5).**
* **`throw` / `try` / `catch` / `defer` (§15).** Including
  function-level and top-level `defer` firing on the throw-unwind
  path. The JIT lowers throws to LLVM `invoke` / `landingpad` with
  the Itanium personality; observable propagation is identical.
* **Auto-drop (§17).** Fires on every refcount-to-zero transition,
  whether triggered by scope exit, property overwrite, or array
  rebind. Cascade order — parent before child — is the same.
  Scope-owned cycle members drop at their scope's exit on both
  backends; closure-held and top-level cycles fire at the GC backstop
  (collection-timed, order unspecified — see §17).
* **Error reporting.** Every `kind` listed in §15 fires under the
  same trigger condition on both backends; `e.message`, `e.line`,
  and `e.col` are populated identically. Uncaught errors print as
  `Kind: message at L:C.`.
* **Class sugar (§10), `static` methods, immutable `self`, and the
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
* **Cycle-collector scheduling.** Both backends run a full,
  non-generational mark-sweep on an adaptive threshold — the
  interpreter re-arms at twice the surviving live set (floored at
  10,000 allocations), the JIT additionally weighs live bytes — so the
  collect *timing* differs between backends and between runs. What a
  program observes does not: cycle members still fire `drop` in the
  pre-sweep pass either way (§17).
* **Forward-reference pre-allocation.** The JIT scans each function
  body and pre-allocates capture cells so closures compiled before
  their `let` declaration still see the post-declaration value.
  The interpreter resolves names dynamically against the live env.

### Technical differences without behavioral impact

These do not affect any program-visible behavior, but operators
embedding Culebra should be aware:

* **Cycle collector internals.** The interpreter's `InterpGC` runs a
  precise gc_refs mark-sweep over its tracked containers (`Array`,
  `Object`, `Tuple`, `Set`, and captured `Environment`s); the JIT uses
  a conservative stack-scanning mark-sweep. Both reclaim every cycle
  shape — including one routed purely through `Object` property maps —
  so this is an implementation difference, not a behavioral one.
* **Thread safety.** Most runtime state — the interpreter and JIT
  garbage collectors, the defer stack, the interrupt flag — lives in
  a `thread_local` `Runtime`, so concurrent isolates on separate
  threads do not share it. The two process-global intern tables, the
  `ShapeRegistry` and the trait registry, are mutex-guarded.
  Execution within a single `Runtime` is
  single-threaded: an embedder that drives one `Runtime` from
  multiple threads must serialize the calls itself.

### Top-level drop note (§17)

Top-level bindings to `drop`-bearing objects live until program exit
without running `drop`, on either backend (see §17 for the worked
example): the interpreter never tears down its cyclic global
environment, and the JIT/AOT suppress drop at the top-level scope
release to match. Use `defer` or a factory function for script-wide
resources regardless of which backend you run on.

When in doubt, the interpreter is authoritative — any JIT deviation
in observable behavior is treated as a bug.

---

## 26. Appendix: conformance test mapping

Every section of this spec has at least one corresponding test file
under `tests/`. `just test` runs interp/JIT diff, AOT diff, and
embedding C++ smoke in one pass; `just test aot` runs only the AOT
diff. The mapping below points to the primary owner; some tests
touch multiple sections, marked "(broad)".

| Test file | Spec sections verified |
|---|---|
| `tests/test_core.cul` | §6, §7, §8, §9, §10, §11, §12, §15, §18, §19 (broad — primary unit-test catch-all) |
| `tests/test_class.cul` | §10 (class sugar, operator overloading, `__str__`, auto-reflection, static methods), §11 |
| `tests/test_class_parameters.cul` | §10 (auto-synthesized `parameters()`) |
| `tests/test_decorator.cul` | §21 |
| `tests/test_defer.cul` | §15 (`defer`, scope-guard pattern) |
| `tests/test_forward_ref.cul` | §6 (scope), §11 (closures), §20 |
| `tests/test_iter.cul` | §12 (`for ... in`), §18 (iterator protocol, String methods), §19 (`range`, `iota`) |
| `tests/test_iter_combinators.cul` | §18 (lazy combinator families, unbounded-source laziness) |
| `tests/test_iter_terminal.cul` | §18 (terminal iterator methods, §18.5 protocol contract) |
| `tests/test_kwargs.cul` | §11 (keyword arguments, `**` splat), §20 (kwargs in multimethods), §7 (evaluation order for mixed calls) |
| `tests/test_match_class.cul` | §13 (type patterns) |
| `tests/test_multidispatch.cul` | §20 |
| `tests/test_object_keys.cul` | §10 (non-String keys) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`, all `kind` values catchable) |
| `tests/test_set.cul` | §10 (sets) |
| `tests/test_tuple.cul` | §10 (tuples, destructuring) |
| `tests/test_ufcs.cul` | §10 (methods, UFCS), §19 (`__ARGS__`) |
| `tests/test_args.cul` | stdlib §10 (`Args`) |
| `tests/test_fs.cul` | stdlib §3 (`FS`) |
| `tests/test_json.cul` | stdlib §9 (`JSON`) |
| `tests/test_tensor.cul` | stdlib §8 (`Tensor`) |
| `tests/test_time.cul` | stdlib §5 (`Time`) |
| `tests/test_import.cul` | §24 (Modules) — uses `tests/test_import_helpers/*.cul` as dependencies |

Every test file in `tests/` is required to pass on both backends
with identical stdout — `just test` enforces it. Interactive
features that are inherently backend-specific (debugger hooks
exercised by REPL state) are tested through `tests/embedding/`
C++ smoke tests rather than as `.cul` scripts.
