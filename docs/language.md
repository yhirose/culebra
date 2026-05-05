Culebra Language Specification
==============================

This document defines the syntax and runtime semantics of the Culebra
programming language. It is normative: where the two backends
(interpreter in `include/interpreter.h` and JIT in `include/jit.h`)
disagree, the interpreter is considered authoritative and the JIT
tracks its behavior.

Table of contents
-----------------

1. Overview and philosophy
2. Lexical structure
3. Grammar
4. Types
5. Values and identity
6. Variables and scope
7. Expressions
8. Strings and interpolation
9. Arrays
10. Objects
11. Functions and closures
12. Control flow (`if`, `while`)
13. Pattern matching (`match`)
14. Optional type annotations
15. Error handling
16. Memory model (RC + cycle collector)
17. Built-in type methods
18. Command-line interface
19. Known limitations
20. Examples

---

1. Overview and philosophy
--------------------------

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

2. Lexical structure
--------------------

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

    nil  true  false  mut  debugger  return  while  if  else  fn  match

The parser also recognizes `let` as an optional prefix in assignments.
Type annotation names (`Nil`, `Bool`, `Long`, `String`, `Array`,
`Object`, `Function`, `Any`) are *not* reserved; they are contextual
and only recognized after `:` or `->`.

### Literals

* Integer: `NUMBER <- [0-9]+`. All numbers are 64-bit signed integers
  (`Long`). No floating point, hex, or octal literals.
* String: `'...'` (no escapes, no interpolation). Single quote is the
  terminator; apostrophes inside must be avoided. (No escape syntax
  yet.)
* Interpolated string: `"...{expr}..."`. `{expr}` embeds any
  expression; literal `{` is not expressible.
* Boolean: `true`, `false`.
* Nil: `nil`.

### Operators and punctuation

    ==  !=  <=  <  >=  >        # comparison
    +  -  *  /  %               # arithmetic
    !                           # logical not
    &&  ||                      # logical and/or (short-circuit)
    =                           # assignment
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

3. Grammar
----------

The full PEG grammar is in [`include/parser.h`](../include/parser.h).
Selected rules:

    PROGRAM     <- STATEMENTS
    STATEMENT   <- DEBUGGER / RETURN / LEXICAL_SCOPE / EXPRESSION
    EXPRESSION  <- ASSIGNMENT / LOGICAL_OR
    ASSIGNMENT  <- LET MUTABLE PRIMARY (ARGUMENTS / INDEX / DOT)*
                   TYPE_ANNOTATION? '=' EXPRESSION
    PRIMARY     <- WHILE / IF / MATCH / FUNCTION / OBJECT / ARRAY
                 / NIL / BOOLEAN / NUMBER / IDENTIFIER
                 / STRING / INTERPOLATED_STRING / '(' EXPRESSION ')'

### Operator precedence

From lowest to highest:

1. `||`
2. `&&`
3. `==`, `!=`, `<`, `<=`, `>`, `>=`
4. Binary `+`, `-`
5. Binary `*`, `/`, `%`
6. Unary `+`, `-`, `!` (right-associative)
7. Call / index / dot (`x(...)`, `x[i]`, `x.k`) — left-associative

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

4. Types
--------

Culebra has exactly seven types:

| Type       | Description                             |
|------------|-----------------------------------------|
| `Nil`      | Single value `nil`                      |
| `Bool`     | `true` or `false`                       |
| `Long`     | 64-bit signed integer                   |
| `String`   | Immutable heap-allocated byte string    |
| `Array`    | Mutable ordered collection of values    |
| `Object`   | Mutable map of string keys to values    |
| `Function` | Closure (function pointer + captures)   |

There is no implicit conversion between types. Arithmetic,
comparison, and boolean operators will raise `type error` when given
the wrong kind of value (see §15).

`Any` is only valid in type annotations and matches any value.

---

5. Values and identity
----------------------

* `Nil`, `Bool`, and `Long` are value types: they are compared and
  copied by value. `==` compares by the underlying data.
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

Ordering (`<`, `<=`, `>`, `>=`) is defined for:

* `Long` and `Bool` — numeric ordering (booleans order `false` <
  `true`).
* `String` — lexicographic byte ordering.
* `Nil` — `nil` compares equal to `nil` and always returns `false`
  for ordering comparisons.

Ordering values of different types raises `type error`.

---

6. Variables and scope
----------------------

### Declaration and assignment

    x = 10           # bare assignment
    let y = 20       # let binding (immutable)
    mut z = 30       # mut binding (mutable)
    let mut w = 40   # equivalent to `mut w = 40`

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
  pattern (see `samples/class.cul`), an enclosing function's mutable
  binding is the object's private field. Accidentally shadowing it —
  typically by writing `mut x = ...` intending a new local — silently
  breaks the object. Making this a compile-time error is worth the
  small restriction.
* **Block scope is a computation staging area.** Inside a single
  function, a `{ ... }` block is a local calculation region.
  Rebinding a name there (`let a = transform(a)`) is a common,
  intentional pattern, not a bug. No reason to restrict it.
* **Globals form a shared vocabulary.** Builtins (`min`, `puts`,
  `range`) and top-level names are understood to be ambient. Locals
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

7. Expressions
--------------

### Arithmetic

`+`, `-`, `*`, `/`, `%` operate on `Long`. Both operands must be
`Long`; otherwise `type error`. Integer division truncates toward zero.
Division or modulo by zero raises `divide by 0 error at L:C`.

### Comparison

* `==`, `!=`: any two values; see §5 for reference-type semantics.
* `<`, `<=`, `>`, `>=`: same-typed operands only, else `type error`.

### Logical

* `!x`: requires `x` convertible to bool.
* `x && y`: evaluates `x`; if falsy returns `x`, else evaluates and
  returns `y`. Short-circuit.
* `x || y`: evaluates `x`; if truthy returns `x`, else evaluates and
  returns `y`. Short-circuit.

### Truthiness

Only `Bool` and `Long` are convertible to bool:

* `Bool`: itself.
* `Long`: `0` is false, all others true.
* `Nil`, `String`, `Array`, `Object`, `Function`: **not convertible** —
  using one in a boolean context (e.g., `if s { ... }`) raises
  `type error`.

(This is intentionally strict. Wrap with an explicit check such as
`s != nil` or `arr.size() > 0` if needed.)

### Unary

`+x` is a no-op (must be `Long`). `-x` negates a `Long`.

### Parentheses

`(expr)` groups and does not introduce a scope.

---

8. Strings and interpolation
----------------------------

### String literals

    'hello'
    'it''s quite'   # not supported: there are currently no escapes

Single-quoted strings are taken verbatim between the quotes.

### Interpolated strings

    "hello {name}"
    "sum = {a + b}"
    "nested: {if x > 0 { 'pos' } else { 'neg' }}"

A `"..."` string consists of plain text segments and `{expr}`
segments. Each expression is evaluated, converted to its display form
(see below), and concatenated with the surrounding text. `{{` and `}}`
for literal braces are not supported; plan around that or use single
quotes.

### Display conversion

When an expression inside `"..."` is not a `String`, its display form
is inserted:

* `Nil` → `nil`
* `Bool` → `true` or `false`
* `Long` → decimal
* `Array` → `[v1, v2, ...]` with each element's `puts` form (strings
  in brackets are quoted, e.g. `['hi']`)
* `Object` → `{key: val, mut key2: val2, ...}` sorted alphabetically
* `Function` → `[function]`

String values inside `"..."` are inserted verbatim (no quotes); this
is the one place display differs from `puts`.

Cyclic data (`a.c = a`) displays as `{...}` / `[...]` to avoid
infinite recursion.

---

9. Arrays
---------

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

10. Objects
-----------

### Construction

    {}                                     # empty object
    {name: 'alice', age: 30}               # two properties
    {mut counter: 0, name: 'x'}            # explicit mut on a property

Property order in the source is irrelevant for equality or access, but
the alphabetical order is used for display and iteration.

### Access and mutation

    obj.key              # read, returns nil if absent
    obj.key = v          # set (creates if absent); mut required on existing
    obj.size()           # property count (Long)
    obj.has(key)         # not built in; use method call on user object

Assigning to an existing property that was declared without `mut`
raises `immutable property 'key' at L:C`.

Property names are identifiers (`[A-Za-z_][A-Za-z0-9_]*`).
Computed / string-indexed properties are not supported.

### Methods and `this`

When `obj.m(args)` is called and `obj.m` is a `Function`, the function
is invoked with `this` bound to `obj`. The function must use `this`
explicitly to access the receiver; it is otherwise a regular free
function.

    o = { n: 10, add: fn (x) { x + this.n } }
    puts(o.add(5))              # 15

---

11. Functions and closures
--------------------------

### Function literals

    fn () { nil }
    fn (x) { x + 1 }
    fn (mut x) { x = x + 1; x }
    fn (a: Long, b: Long) -> Long { a + b }

Functions are first-class values and the only way to define a reusable
piece of code. There is no `fn name(...)` declaration syntax; use
`name = fn (...) { ... }`.

### Parameters

* `mut` on a parameter makes that parameter binding mutable inside
  the body. Without `mut`, reassigning the parameter raises
  `immutable variable`.
* Optional type annotations are enforced on entry (§14).

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

12. Control flow
----------------

### `if`

    if cond { then_block }
    if cond { then_block } else { else_block }
    if c1 { b1 } else if c2 { b2 } else { b3 }

`cond` must be truthy-convertible (`Bool` or `Long`). `if` is an
expression; its value is the taken branch's last expression, or `nil`
if no branch is taken (no `else` and the `if` was false).

### `while`

    while cond { body }

`while` is a statement; its value is `nil`. There are no `break` or
`continue` constructs; use early `return` inside a function, or a
boolean guard.

### `return`

Valid only inside a function body. Exits the enclosing function with
the given value (or `nil`). Using `return` at top level is an error.

### `debugger`

When the program is run under `--debug`, encountering `debugger`
pauses execution and drops into a simple REPL debugger showing the
current source line. Without `--debug`, `debugger` is a no-op.

---

13. Pattern matching
--------------------

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
| `{k1, k2}`        | `Object` containing at least those keys; binds each `ki` to `obj.ki` |
| `{}`              | Any `Object` (keys ignored)              |

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

14. Optional type annotations
-----------------------------

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

    Nil  Bool  Long  String  Array  Object  Function  Any

`Any` always matches. Unknown type names fail the check and raise
`type error`.

### Where annotations are *not* checked

* Arithmetic / boolean operators check their own operand types;
  annotations on local intermediates do not add extra coverage.
* Object property values have no annotation slot.
* Array element types are not tracked.
* `mut x: T` on a local does not re-check on later reassignment —
  the check happens once at the annotated declaration.

Annotations are primarily a **documentation and boundary check**
feature, not a type system.

---

15. Error handling
------------------

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

Runtime errors currently abort the program with a diagnostic of the
form (they do **not** flow through `throw`/`catch`):

    type error at L:C.
    type error: parameter 'name' expects T at L:C.
    divide by 0 error at L:C.
    index out of range at L:C.
    immutable variable 'x'...
    immutable property 'key' at L:C.
    undefined variable 'name'...
    assert failed at L:C.
    cannot shadow outer variable 'x' (...) at L:C.

`L` and `C` are 1-based line and column of the offending AST node.
Integrating these with `try`/`catch` is future work.

### `assert(cond)`

Evaluates `cond`; if falsy, aborts with `assert failed at L:C.`. Used
as a lightweight testing primitive (see `samples/test.cul`).

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

Runtime errors (type error, divide by 0, etc.) continue to bypass
user `try/catch` on both backends.

---

16. Memory model
----------------

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

---

17. Built-in type methods
-------------------------

The methods below are part of the language: they are available on any
value of the corresponding type without any `import`, and cannot be
shadowed by user code (for `String`, which has no property store; for
`Array`/`Object`, a user-defined property of the same name wins and
the built-in is a fallback).

Global built-in functions (`puts`, `assert`, `abs`, `range`, I/O,
etc.) are specified separately in [`docs/stdlib.md`](stdlib.md).

Conventions:

* Types follow §14. `Any` denotes any value.
* Positional indices are zero-based. Negative indices where noted
  count from the end (`-1` is the last element).
* Length and indexing for `String` are in **bytes**; Culebra strings
  are not Unicode-aware.

### 17.1 String methods

All string methods return new `String` values; the receiver is never
mutated.

| Signature                                   | Description                          |
|---------------------------------------------|--------------------------------------|
| `s.size() -> Long`                          | Byte length.                         |
| `s.upper() -> String`                       | ASCII uppercase.                     |
| `s.lower() -> String`                       | ASCII lowercase.                     |
| `s.trim() -> String`                        | Remove leading/trailing whitespace (` `, `\t`, `\n`, `\r`). |
| `s.split(sep: String) -> Array`             | Split on every occurrence of `sep`. Empty `sep` → `[s]`. |
| `s.contains(sub: String) -> Bool`           | Whether `sub` appears anywhere. Empty `sub` → `true`. |
| `s.starts_with(prefix: String) -> Bool`     | Whether `s` begins with `prefix`.    |
| `s.ends_with(suffix: String) -> Bool`       | Whether `s` ends with `suffix`.      |
| `s.slice(start: Long, end: Long) -> String` | Substring `[start, end)`. Negative indices count from end; `start` clamped to `[0, size()]`, `end` to `[start, size()]`. |

```culebra
puts('hello'.size())              # 5
puts('HeLLo'.lower())             # 'hello'
puts('  hi  '.trim())             # 'hi'
puts('a,b,c'.split(','))          # ['a', 'b', 'c']
puts('hello'.slice(1, 4))         # 'ell'
puts('hello'.slice(-3, -1))       # 'll'
```

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
| `o.keys() -> Array`              | `Array` of `String` keys in ascending alphabetical order (matches display order — §8). |
| `o.has(key: String) -> Bool`     | Whether `o` has an own property named `key`. Ignores built-in method names. |
| `o.remove(key: String) -> Nil` *(mutating)* | Delete the property named `key` if present. |

```culebra
o = {b: 2, a: 1, c: 3}
puts(o.keys())   # ['a', 'b', 'c']
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

---

18. Command-line interface
--------------------------

    culebra [flags] [script.cul ...]

### Flags

| Flag           | Effect                                                    |
|----------------|-----------------------------------------------------------|
| `--shell`      | Drop into the REPL after running any scripts.             |
| `--ast`        | Print the parsed AST before running.                      |
| `--debug`      | Enable the CLI debugger; `debugger` statements break in.  |
| `--jit`        | Use the LLVM ORC JIT instead of the tree-walking interpreter. |
| `--emit-llvm`  | With `--jit`, print the generated IR and exit.            |
| `-O0`..`-O3`   | With `--jit`, select the LLVM optimization level. Default `-O2`. |

If no script is provided, the REPL is launched automatically. The JIT
REPL does not preserve state between inputs (each input is a fresh
compilation), while the interpreter REPL does.

---

19. Known limitations
---------------------

* No floating-point numbers, big integers, or bignums.
* No string escape sequences; strings are raw between quotes.
* No string indexing or slicing as a first-class operation.
* Array and object equality compare by reference, not structural.
* No user-level exception handling (`try`/`catch`).
* No module system (`import`) yet.
* No standard library beyond `puts`, `assert`, `.size()`, `.push()`.
* Type annotations are enforced only at function boundaries and
  annotated assignments; they do not make the language static.
* Pattern matching has no exhaustiveness check.
* `match` arm bodies must be single expressions — `{ ... }` in an arm
  body is parsed as an `Object` literal, not a block.
