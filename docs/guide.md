The Culebra Guide
=================

A small, dynamically-typed scripting language with Rust-inspired
syntax. Two backends share one AST: a tree-walking interpreter and an
LLVM ORC JIT. This guide walks you from "hello" to embedding Culebra
in a C++ host. For the formal grammar see [`language.md`](language.md);
for the API reference see [`stdlib.md`](stdlib.md); for implementation
internals see [`internals.md`](internals.md).

> **Doctest convention.** Every ` ```culebra ` block in this guide is
> a runnable example. Lines ending in `# => <value>` show expected
> stdout; `# !! <pattern>` shows an expected `throw`. A block prefixed
> with `# doctest: skip` is illustrative only (typically because it
> needs multiple files, network access, or the `culebra test` runner).
> Blocks are independent: each runs in a fresh scope. Full convention
> and directive list: Ch.17.1.

> **Status labels.** Section headings without a label describe the
> implementation as of today. Labels that appear: **Draft** (under
> implementation, API may change), **Planned** (decided, not yet
> implemented), **Deprecated** (slated for removal). Features that
> were considered and rejected live in [`_history.md`](_history.md).

Contents
--------

- **Part I — The Language Core**
  1. [Hello & setup](#1-hello--setup)
  2. [Values, bindings, and control flow](#2-values-bindings-and-control-flow)
  3. [Functions and closures](#3-functions-and-closures)
  4. [Strings](#4-strings)
  5. [Iterators](#5-iterators)
  6. [Pattern matching](#6-pattern-matching)
  7. [Error handling and RAII](#7-error-handling-and-raii)
  8. [Algebraic effects](#8-algebraic-effects)
- **Part II — Tools for abstraction**
  9. [Classes](#9-classes)
  10. [Operator overloading](#10-operator-overloading)
  11. [UFCS and multimethods](#11-ufcs-and-multimethods)
  12. [Decorators](#12-decorators)
  13. [Modules](#13-modules)
- **Part III — Types and libraries**
  14. [Type system](#14-type-system)
  15. [Standard library tour](#15-standard-library-tour)
  16. [Tensor primitive](#16-tensor-primitive)
- **Part IV — Verification and deployment**
  17. [Testing (`culebra test`)](#17-testing-culebra-test)
  18. [Linting (`culebra lint`)](#18-linting-culebra-lint)
  19. [Formatting (`culebra fmt`)](#19-formatting-culebra-fmt)
  20. [AOT binary build](#20-aot-binary-build)
  21. [Embedding overview](#21-embedding-overview)

## 0. Design philosophy

Read this once; the rest of the guide assumes these choices.

- **Two backends, one AST.** A tree-walking interpreter and an LLVM
  ORC JIT share the same AST. The interpreter has no LLVM dependency
  (~1 MB binary, good for embedding); the JIT runs the same program
  at `-O2`. Both are maintained — neither is going away.
- **Eight everyday types.** `Nil`, `Bool`, `Long`, `Float`, `String`,
  `Array`, `Object`, `Function`, plus four specialized ones
  (`StringView`, `Tuple`, `Set`, `Tensor`). Everything else (classes,
  modules, errors) builds on `Object`.
- **Rust-inspired surface.** `let` / `mut` / `fn` / `match` / block-as-
  expression. Closures are first-class, errors are values, no hidden
  globals.
- **UFCS, not pipeline.** Any free function `f(x, ...)` can be called
  as `x.f(...)`. A pipeline operator was considered and rejected (see
  [`_history.md`](_history.md)).
- **Explicit, static modules.** A file exposes bindings with
  `export { ... }` and a consumer binds them with
  `import name from './path.cul'`. Both forms are top-level only, so
  the dependency graph is known at parse time — which is what makes
  AOT bundling and tree-shaking possible (Ch.13).
- **No async/await.** I/O is blocking by design; concurrency comes
  from threads, not coroutines. HTTP and similar networked stacks are
  blocking with the typical scale ceiling of a few thousand
  connections.
- **Batteries-included, tiered.** The core (Math/IO/FS/File/Sys/
  Random/String/Time/Args) and Tier 1 (Regex/Http/Hash/Encoding/
  Compress/JSON/CSV/TOML/SQLite/UUID/Log/Term/Canvas) both ship
  today; Tier 2/3 (Crypto, Sockets) follows demand — see Ch.15.
- **Pre-1.0.** Source and APIs may change. There is no release
  machinery (no version tags, no CHANGELOG, no Homebrew formula) yet
  — that comes after 1.0.

---

Part I — The Language Core
==========================

## 1. Hello & setup

Build the interpreter (and JIT, if LLVM 20+ is installed):

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB
just dev                # fast no-LTO -O1 build into build-dev/ (inner loop)
just test-dev           # quick interp==JIT check vs build-dev/ (after each edit)
just test               # all backends + embed smoke (parallel; JOBS=1 to serialize)
just test wrap          # `culebra wrap` end-to-end (not part of `just test`)
./build/culebra --shell # REPL (add --jit for the JIT REPL)
```

A Culebra source file uses the `.cul` extension. Run it with the
`culebra` binary:

```bash
echo "inspect('hello, culebra!')" > hello.cul
./build/culebra hello.cul            # interpreter
./build/culebra --jit hello.cul      # JIT (same output)
./build/culebra --jit-faststart hello.cul # JIT, fast startup
./build/culebra --help                    # all options and commands
```

All three backends produce identical observable output (the whole
interp↔JIT differential corpus is verified). `--jit-faststart` skips both
the IR and the machine-code optimizers, which cuts **JIT warmup**
(startup/codegen time) by roughly **40x** for a small steady-state cost —
about 12% on pure-script hot loops, and ~0% when the heavy work lives in
the C++/BLAS runtime (e.g. `Tensor`). It implies `-O0`; passing another
`-O` alongside it is an error. Prefer it for short scripts or BLAS-bound
runs; the default `--jit` (`-O2`) keeps the best steady-state throughput.

Comments start with `#` (line) or `/* ... */` (block). Statements may
end with `;`; newlines also separate statements. The recommended style
is to omit `;` at end-of-line.

```culebra
# this is a comment
inspect('hello')          # => 'hello'
```

`inspect` prints values in quoted, inspectable form, so strings appear
with surrounding quotes (`'hello'`) and reference types in their literal
shape. Use `println` for raw, unquoted text with a trailing newline, or
`print` to omit the newline too — see Ch.15.

## 2. Values, bindings, and control flow

### 2.1 The eight everyday types

```culebra
inspect(type_of(nil))            # => 'Nil'
inspect(type_of(true))           # => 'Bool'
inspect(type_of(42))             # => 'Long'
inspect(type_of(3.14))           # => 'Float'
inspect(type_of('hi'))           # => 'String'
inspect(type_of([1, 2]))         # => 'Array'
inspect(type_of({a: 1}))         # => 'Object'
inspect(type_of(fn () { 1 }))    # => 'Function'
```

Four more show up once you reach for them: `StringView` (Ch.4.4),
`Tuple` and `Set` (Ch.14.2), and `Tensor` (Ch.16). The full table is
[language.md §4](language.md).

### 2.2 Bindings: bare, `let`, `mut`

```culebra
x = 10              # bare: new immutable binding, or reassign outer
let y = 20          # let: new immutable binding (must not shadow outer)
mut z = 30          # mut: new mutable binding
z = z + 1           # mut allows reassignment
z += 1              # compound (`-= *= /= %= **= @=` work the same way)
inspect(z)             # => 32
```

A bare assignment searches outward through enclosing scopes and
reassigns the nearest matching binding; if nothing matches, it creates
a new binding in the current scope. This is what makes the
closure-based object pattern work (Ch.9).

> **Heads-up:** Bindings are immutable by default, so a bare `x = 1`
> followed by `x = 2` in the same scope is an error (`immutable variable
> 'x'`). Add `mut` when you intend to reassign (`mut x = 1; x = 2`).
> `let` is optional — it makes the (already immutable) intent explicit and
> forbids shadowing an outer binding.

### 2.3 Shadow prohibition

Introducing a new binding (`let`, `mut`, parameter, or `match`
pattern) is a compile-time error if it would shadow a *captured outer*
variable. Block-local rebinding (the same name inside one function)
is fine.

```culebra
make = fn () {
  mut n = 0
  fn () {
    # let n = 1   # error: would shadow captured `n`
    n = n + 1     # bare reassignment of the captured `n` — OK
    n
  }
}
c = make()
inspect(c())                     # => 1
inspect(c())                     # => 2
```

### 2.4 Control flow

`if` and `match` (Ch.6) are expressions; their value is the chosen
branch. `while` and `for` are statements (value is `nil`).

```culebra
x = 7
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
inspect(sign)                    # => 1

mut i = 0
while i < 3 { inspect(i); i = i + 1 }
# => |
# 0
# 1
# 2

for n in 0..3   { inspect(n) }   # exclusive range
# => |
# 0
# 1
# 2

for n in 0..=2  { inspect(n) }   # inclusive range
# => |
# 0
# 1
# 2

for n in 0..10 by 3 { inspect(n) }   # stepped range
# => |
# 0
# 3
# 6
# 9

for k, v in {a: 1, b: 2} { inspect("{k}={v}") }   # Objects yield key, value
# => |
# 'a=1'
# 'b=2'
```

`break` and `continue` work inside `while` and `for`.

### 2.5 `nobreak`, init clauses, `cond`, and `? :`

A loop may carry a `nobreak` block that runs only when the loop
finished without a `break` — Python's `for ... else` without the
confusing keyword:

```culebra
mut found = nil
for n in [1, 3, 5] {
  if n % 2 == 0 { found = n; break }
} nobreak {
  inspect('no even number')      # => 'no even number'
}
```

`while`, `if`, and `match` accept an **init clause** — a binding
scoped to the construct, separated by `;` — so a loop variable doesn't
leak into the surrounding scope:

```culebra
while mut i = 0; i < 3 { i = i + 1 }
if let n = 6; n > 5 { inspect('big') }     # => 'big'
```

For multi-way choices there is `cond` (a subjectless `match`) and the
ternary `? :` for the two-way case:

```culebra
grade = fn (n) {
  cond {
    n >= 90 => 'A',
    n >= 80 => 'B',
    _       => 'C'
  }
}
inspect(grade(85))               # => 'B'
inspect(grade(50) == 'C' ? 'ok' : 'no')   # => 'ok'
```

Captured state *is* the object's state in the closure-based pattern
(Ch.9), so silently shadowing it would break the object; block-local
rebinding, by contrast, is a routine pattern and stays legal. Full
rule set and design rationale: [language.md §6](language.md).

## 3. Functions and closures

### 3.1 `fn` and `|x|`

```culebra
add = fn (a, b) { a + b }
inspect(add(2, 3))               # => 5

# Type annotations are optional; see Ch.14
add_typed = fn (a: Long, b: Long) -> Long { a + b }
inspect(add_typed(2, 3))         # => 5

# |x| expr is shorthand for fn (x) { expr }
square = |x| x * x
inspect(square(7))               # => 49

# Use `fn` for recursion (the function refers to itself)
fib = fn (x) {
  if x < 2 { x } else { fn(x - 2) + fn(x - 1) }
}
inspect(fib(10))                 # => 55
```

### 3.2 Closures

Inner functions capture outer bindings by reference. `mut` makes the
captured binding writable.

```culebra
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` reassigns the captured `n`
}
c = make_counter()
inspect(c())                     # => 1
inspect(c())                     # => 2
inspect(c())                     # => 3
```

### 3.3 Keyword arguments and `**splat`

Parameters can declare defaults. A `*` marker after the last
positional makes the rest keyword-only; `**rest` collects unknown
keywords into an Object.

```culebra
greet = fn (name, *, greeting = 'hi', **opts) {
  prefix = if opts.has('formal') && opts.formal { 'Mr./Ms. ' } else { '' }
  "{greeting}, {prefix}{name}"
}
inspect(greet('alice'))                       # => 'hi, alice'
inspect(greet('alice', greeting: 'hello'))    # => 'hello, alice'
inspect(greet('bob', formal: true))           # => 'hi, Mr./Ms. bob'

# `**` splats an Object as keyword arguments:
opts = {greeting: 'yo', formal: false}
inspect(greet('carol', **opts))               # => 'yo, carol'
```

A `*` marker forces the caller to name the option, keeping long
parameter lists readable across reorders and extensions. A final
`*rest` parameter is the positional counterpart — it collects the
extra positional arguments into an Array:

```culebra
sum_all = fn (first, *rest) {
  mut t = first
  for v in rest { t = t + v }
  t
}
inspect(sum_all(1, 2, 3, 4))                  # => 10
```

Full parameter, default, and splat semantics: [language.md
§11](language.md).

## 4. Strings

### 4.1 Interpolation and concatenation

```culebra
name = 'Culebra'
inspect("hello, {name}!")                   # => 'hello, Culebra!'
inspect("two plus three is {2 + 3}")        # => 'two plus three is 5'
inspect('a' + 'b' + 'c')                    # => 'abc'
```

### 4.2 Iteration and indexing

Strings iterate by Unicode *scalar* (one codepoint per step), not by
byte. Indexing is by byte offset into the UTF-8 representation; out-
of-bounds raises.

```culebra
for c in 'café' { inspect(c) }
# => |
# 'c'
# 'a'
# 'f'
# 'é'

inspect('café'.size())            # => 5
```

`size()` counts bytes in the UTF-8 representation (`é` is 2 bytes, so
`'café'` is 5), while the `for` loop above steps one Unicode scalar at
a time (four steps).

### 4.3 Common methods

```culebra
inspect('hello world'.split(' '))        # => ['hello', 'world']
inspect('  hi  '.trim())                 # => 'hi'
inspect('abc'.upper())                   # => 'ABC'
inspect('foo'.starts_with('fo'))         # => true
inspect(['a', 'b', 'c'].join('-'))       # => 'a-b-c'
```

See [language.md §18.1](language.md) for the full method list.

### 4.4 `StringView`, `StringLike`, lazy graphemes

`.slice()`, `.split()`, and `.view()` return a `StringView` — a
zero-copy borrow into the original string's bytes, kept alive by a
shared owner even if the original binding goes out of scope. A
parameter typed `StringLike` accepts either a `String` or a
`StringView`, so helpers don't force a copy just to read.

```culebra
print_first_grapheme = fn (s: StringLike) {
  for g in s.graphemes() { inspect(g); break }
}
print_first_grapheme('café')          # => 'c'

inspect(type_of('hello'.slice(1, 4)))    # => 'StringView'
inspect('hello'.slice(1, 4))             # => 'ell'
```

`.graphemes()` walks Unicode *extended grapheme clusters* lazily —
each step is one user-perceived character, even a multi-codepoint
emoji family joined by ZWJ:

```culebra
inspect('a👨‍👩‍👧b'.graphemes().collect().size())    # => 3
inspect('café'.graphemes().collect().size())        # => 4
```

Full `StringView`/grapheme API: [language.md §18.1](language.md). Design
discussion: [`internals.md` §6](internals.md).

### Why Go-style byte indexing?

Swift/Python 3 hide the bytes-vs-scalars distinction behind an
opaque `Character` / `str` index, convenient until you interop with
sockets or files. Go exposes byte offsets and offers explicit `rune`
iteration on top; Culebra follows that model, adding lazy grapheme
iteration (above) for display work.

## 5. Iterators

### 5.1 `range` (lazy) vs `iota` (eager)

```culebra
# range builds nothing; for-loops consume it lazily
for i in range(3) { inspect(i) }
# => |
# 0
# 1
# 2

# iota allocates an Array
inspect(iota(3))                 # => [0, 1, 2]
inspect(iota(2, 5))              # => [2, 3, 4]
```

### 5.2 Lazy chains

`.iter()` turns an Array into a lazy iterator. Chains stop at the
first consumer (`collect`, `reduce`, `find`, ...) and never build
intermediate Arrays.

```culebra
result = range(1000)
  .filter(|x| x % 2 == 0)
  .map(|x| x * 3)
  .take(5)
  .collect()
inspect(result)                  # => [0, 6, 12, 18, 24]

total = range(1, 11).reduce(0, |a, x| a + x)
inspect(total)                   # => 55

inspect([1, 2, 3, 4].iter().any(|x| x > 3))      # => true
inspect([10, 20, 30].iter().find(|x| x > 15))    # => 20
```

### 5.3 `enumerate`, `zip`, `flat_map`, `skip`, `take_while`

```culebra
for i, v in ['fizz', 'buzz', 'bang'].enumerate() {
  inspect("{i}: {v}")
}
# => |
# '0: fizz'
# '1: buzz'
# '2: bang'

for p in [1, 2, 3].iter().zip(['a', 'b', 'c']) {
  inspect("{p.first} / {p.second}")
}
# => |
# '1 / a'
# '2 / b'
# '3 / c'

flat = [[1, 2], [3], [4, 5, 6]].iter().flat_map(|xs| xs).collect()
inspect(flat)                    # => [1, 2, 3, 4, 5, 6]

head = range(100).skip(10).take_while(|x| x < 15).collect()
inspect(head)                    # => [10, 11, 12, 13, 14]

# chunks: fixed-size groups (last may be shorter)
inspect([1, 2, 3, 4, 5].iter().chunks(2).collect())
# => [[1, 2], [3, 4], [5]]

# windows: sliding view, advancing one element at a time
inspect([1, 2, 3, 4].iter().windows(2).collect())
# => [[1, 2], [2, 3], [3, 4]]
```

### 5.4 User-defined iterators

Implement three methods: `iter()` (returning the iterator itself, by
convention), `has_next()` (returning a `Bool`), and `next()` (returning
the next element). Full protocol, including the early-termination
guarantee for lazy chains: [language.md §18.5](language.md).

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter:     fn () { self },
    has_next: fn () { i > 0 },
    next:     fn () { v = i; i = i - 1; v }
  }
}

for v in countdown(3) { inspect(v) }
# => |
# 3
# 2
# 1
```

### 5.5 Generators with `yield`

A `fn` body containing `yield` becomes a generator: calling it doesn't
run the body, it returns an iterator (Ch.5.4's `iter`/`has_next`/`next`
protocol, so it works with `for` and every lazy-chain method). `yield
from` delegates to another iterable.

```culebra
fn countdown(start) {
  mut i = start
  while i > 0 { yield i; i = i - 1 }
}
for v in countdown(3) { inspect(v) }
# => |
# 3
# 2
# 1

fn chunk(arr, n) {
  mut buf = []
  for v in arr {
    buf.push(v)
    if buf.size() >= n { yield buf; buf = [] }
  }
  if buf.size() > 0 { yield buf }
}
inspect(chunk([1, 2, 3, 4, 5], 2).collect())    # => [[1, 2], [3, 4], [5]]
```

## 6. Pattern matching

### 6.1 The basics

```culebra
describe = fn (x) {
  match x {
    0                  => 'zero',
    1 | 2 | 3          => 'small',
    n: Long if n > 100 => "big ({n})",
    n: Long            => "int ({n})",
    s: String          => "str ({s})",
    true               => 'TRUE',
    false              => 'FALSE',
    nil                => 'NIL',
    _                  => 'other'
  }
}
inspect(describe(0))             # => 'zero'
inspect(describe(2))             # => 'small'
inspect(describe(999))           # => 'big (999)'
inspect(describe('hi'))          # => 'str (hi)'
inspect(describe([1]))           # => 'other'
```

### 6.2 As an expression

`match` produces a value; use it inside computations.

```culebra
classify = fn (n: Long) -> Long {
  match n {
    n if n < 0 => -1,
    0          => 0,
    _          => 1
  }
}
inspect(classify(-5))            # => -1
inspect(classify(0))             # => 0
inspect(classify(7))             # => 1
```

### 6.3 Destructuring

```culebra
shape = fn (a) {
  match a {
    []              => 'empty',
    [x]             => "one ({x})",
    [x, y]          => "two ({x},{y})",
    [head, ...tail] => "head={head}, rest={tail.size()}",
  }
}
inspect(shape([]))               # => 'empty'
inspect(shape([10, 20]))         # => 'two (10,20)'
inspect(shape([1, 2, 3, 4]))     # => 'head=1, rest=3'

first_name = fn (people) {
  match people {
    [{name}, ..._] => name,
    _              => 'none'
  }
}
inspect(first_name([{name: 'x'}, {name: 'y'}]))     # => 'x'
inspect(first_name([]))                              # => 'none'
```

### 6.4 Recursion

```culebra
is_even = fn (n) {
  match n {
    0 => true,
    1 => false,
    _ => fn(n - 2)
  }
}
inspect(is_even(10))             # => true
inspect(is_even(7))              # => false
```

### Why no exhaustiveness check?

Without a static type system, exhaustiveness on `Object` shape would
cost more to track than it saves; the `_` arm (or a guarded final
pattern) makes intent explicit instead. Details and the Union-type
exception: [language.md §13](language.md).

## 7. Error handling and RAII

### 7.1 `throw`, `try`, `catch`

The thrown value can be any Culebra value — String, Object, anything.

```culebra
validate = fn (x) {
  if x < 0 { throw "negative: {x}" }
  x
}

try {
  inspect(validate(42))          # => 42
  inspect(validate(-1))          # throws, the next line is unreached
  inspect('unreached')
} catch e {
  inspect("caught: {e}")         # => 'caught: negative: -1'
}
```

### 7.2 `try` as an expression

```culebra
validate = fn (x) {
  if x < 0 { throw "negative: {x}" }
  x
}
safe = fn (x) {
  try { validate(x) } catch _ { 0 }
}
inspect(safe(7))                 # => 7
inspect(safe(-99))               # => 0
```

### 7.3 `defer`

`defer { ... }` registers a cleanup that runs on every exit path from
the *enclosing block* (normal fall-through, `return`, or `throw`) in
LIFO order — at block, function-body, and top level alike, on every
backend. Put the `defer` in an inner `{ }` when the cleanup should
happen before the rest of the function continues. Full exit-path and
ordering rules: [language.md §15](language.md).

```culebra
demo = fn (fail) {
  {
    defer { inspect('cleanup A') }
    defer { inspect('cleanup B') }
    if fail { throw 'failed' }
    inspect('work done')
  }
}

demo(false)
# => |
# 'work done'
# 'cleanup B'
# 'cleanup A'
```

### 7.4 RAII via `drop`

If an Object has a no-arg Function-typed `drop` property, the runtime
calls it when the last reference goes away. Define `drop` via a
factory and bind in a block scope so the refcount truly hits zero on
scope exit.

```culebra
make_resource = fn (id) {
  { drop: fn () { inspect("R{id} released") } }
}

inspect('enter')
{
  r = make_resource('X')
}
inspect('exit')
# => |
# 'enter'
# 'RX released'
# 'exit'
```

`drop` cascades — when the outer object is released, its members
(themselves holding refs to `drop`-bearing objects) release in turn.
Full memory model (RC + cycle collection): [language.md §17](language.md).

### 7.5 Scope guard pattern

When cleanup needs to be registered from code that can't place its
own `defer` (e.g. a callback that wants cleanup at the caller's
scope), a small helper object holding a list of cleanup closures
does the job — run in LIFO order from one `defer` at the call site.
Full worked example: [language.md §15](language.md).

### Why allow any value to be thrown?

A typical `throw "msg"` is enough for scripts; classed errors (Ch.9)
are enough for libraries; you don't need a hierarchy to start. Catch
arms can pattern-match on whatever shape the thrower used (Ch.6).

---

## 8. Algebraic effects

An *effect* lets code invoke an operation whose meaning is decided by its
caller. You `perform` an operation; a `handle` block up the call stack chooses
what it does and whether to *resume* the code that performed it. One mechanism
covers generators, exceptions, dependency injection, and backtracking search.

### 8.1 `perform` and `handle`

Declare an operation with `effect fn` and no body, `perform` it, and give a
`handle` block a `with` clause. The clause receives the operation's arguments
and a `resume` continuation:

```culebra
effect fn ask()

let answer = handle {
  let n = perform ask()
  n * 2
} with ask(resume) {
  resume(21)
}
inspect(answer)   # => 42
```

`resume(21)` continues the handled body at the `perform ask()` point with the
value `21`, so `n` is `21` and the body yields `42`.

### 8.2 Handlers thread state

A handler runs on every `perform`, so it can interpret operations against state
it owns — here a cell read by `get` and written by `put`:

```culebra
effect fn get()
effect fn put(v)

effect fn counter() {
  perform put(perform get() + 1)
  perform put(perform get() + 1)
  perform get()
}

mut cell = 0
let n = handle { counter() } with get(k) { k(cell) }
                             with put(v, k) { cell = v; k(nil) }
inspect(n)   # => 2
```

Any function may perform: a plain fn's `perform` dispatches to the handlers
installed on the current call stack, and the `effect fn` marker is needed only
when a handler resumes more than once or in non-tail position (a captured
continuation). One `handle` can carry a `with` clause per operation.

### 8.3 Resuming more than once

A continuation is multi-shot — a handler may `resume` any number of times, and
each call independently re-runs the rest of the performing code. Returning both
resumptions explores both choices:

```culebra
effect fn choose(a, b)

let both = handle {
  let x = perform choose(1, 2)
  x * 10
} with choose(a, b, k) {
  [k(a), k(b)]
}
inspect(both)   # => [10, 20]
```

### 8.4 Handlers that never resume

A clause that does not call `resume` discards the rest of the computation —
exactly an exception. A `perform` with no handler raises `EffectError`, so
effects double as recoverable, typed failures:

```culebra
effect fn raise(msg)

effect fn safeDiv(a, b) {
  if b == 0 { perform raise("div by zero") }
  a / b
}

inspect(handle { safeDiv(10, 2) } with raise(m, k) { -1 })   # => 5
inspect(handle { safeDiv(10, 0) } with raise(m, k) { -1 })   # => -1
```

Add `with return(v) { … }` to map the normal-completion value, and write a
`handle` inside an effectful body to capture and resume from an enclosing
computation. Full reference and limitations: [language.md §16](language.md).

A worked example lives in
[`examples/effects/queen.cul`](../examples/effects/queen.cul): an N-queens
search whose `search` never mentions backtracking — `perform choose(...)` and
`perform reject()` get their meaning from the enclosing `handle`, so one body
runs as an exhaustive enumeration, a first-answer search, or a placement
counter depending only on the handler.

---

Part II — Tools for abstraction
================================

## 9. Classes

### 9.1 Syntax

`class` declares a constructor (`new`) and methods. Fields set via
`self.x = ...` are mutable by default. Instances carry a readable
`class:` tag.

```culebra
class Car {
  new(mpr)  { self.miles = 0; self.mpr = mpr }
  run(n)    { self.miles = self.miles + self.mpr * n }
  total()   { "total: {self.miles} miles" }
}

car = Car.new(5)
car.run(1); car.run(2)
inspect(car.total())             # => 'total: 15 miles'
inspect(car.class)               # => 'Car'
```

Calling the class itself is shorthand for `.new` — `Car(5)` is exactly
`Car.new(5)`, keyword arguments and all. Use whichever reads better; a
class is callable the way its constructor is.

```culebra
class Point { new(x, y) { self.x = x; self.y = y } }
p = Point(3, 4)               # same as Point.new(3, 4)
inspect("{p.x},{p.y}")           # => '3,4'
```

Fields can also be **declared** in the class body with a default.
Every instance gets its own copy, materialized before `new` runs, so a
field exists (with a known value) even on a path the constructor
doesn't touch. A `get` method is a computed property — called without
parentheses:

```culebra
class Temp {
  celsius = 0.0
  scale   = 'C'
  new(c) { self.celsius = c }
  get fahrenheit() { self.celsius * 9.0 / 5.0 + 32.0 }
}

t = Temp.new(100.0)
inspect(t.fahrenheit)            # => 212.0
inspect(t.scale)                 # => 'C'
```

### 9.2 The closure-based alternative

A class is sugar; the same encapsulation works with a factory that
returns an Object literal. State lives in captured locals (truly
private). Both styles are first-class.

```culebra
Car2 = {
  new: fn (mpr) {
    mut miles = 0
    {
      run:   fn (n) { miles = miles + mpr * n },
      total: fn () { "total: {miles} miles" }
    }
  }
}

car = Car2.new(5)
car.run(1); car.run(2)
inspect(car.total())             # => 'total: 15 miles'
```

Use `class` when you want the `class:` tag and matchable shape; use
the closure form when private state matters more than the tag.

### 9.3 Static methods and fields

A `static` marker on a method or field puts it on the class itself
(no instance needed), which is the natural home for factories and
class-level constants.

```culebra
class Circle {
  new(r)          { self.r = r }
  static PI       = 3.14
  static unit()   { Circle.new(1) }
  area()          { self.r * self.r * Circle.PI }
}
inspect(Circle.unit().area())    # => 3.14
inspect(Circle.PI)               # => 3.14
```

Static fields are eagerly evaluated once, at class declaration time.

### Why support both `class` and closure-based OO?

Closures-as-objects came first and remain the right answer for
disposable encapsulation (e.g. one-off iterators, scope guards). The
`class` form earns its keep when an object travels far and needs an
identity (the `class:` tag, used by `match` and debug output).

## 10. Operator overloading

### 10.1 Special methods

Arithmetic, comparison, indexing, and call operators each map to a
dunder method (`__add__`, `__eq__`, `__lt__`, `__index__`, `__call__`,
...) that a class can define to participate in the operator. The
reverse-side method (`__radd__`, ...) is *not* supported — place your
overload on the type that owns the operation. Full method table and
dispatch rules: [language.md §10](language.md) (Operator overloading).

### 10.2 A worked example: 2-D vectors

```culebra
class Vec2 {
  new(x, y)   { self.x = x; self.y = y }
  __add__(o)  { Vec2.new(self.x + o.x, self.y + o.y) }
  __sub__(o)  { Vec2.new(self.x - o.x, self.y - o.y) }
  __mul__(k)  { Vec2.new(self.x * k, self.y * k) }
  __neg__()   { Vec2.new(-self.x, -self.y) }
  __eq__(o)   { self.x == o.x && self.y == o.y }
  show()      { "({self.x}, {self.y})" }
}

a = Vec2.new(1, 2)
b = Vec2.new(3, 4)
inspect((a + b).show())          # => '(4, 6)'
inspect((b - a).show())          # => '(2, 2)'
inspect((a * 3).show())          # => '(3, 6)'
inspect((-a).show())             # => '(-1, -2)'
inspect(a == Vec2.new(1, 2))     # => true
```

### 10.3 `__call__` for callable instances

A class that defines `__call__` makes its instances directly callable.

```culebra
class Adder {
  new(n)        { self.n = n }
  __call__(x)   { x + self.n }
}

add5 = Adder.new(5)
inspect(add5(10))                # => 15
inspect(add5(99))                # => 104
```

## 11. UFCS and multimethods

### 11.1 UFCS resolution order

For `x.name(args)`, an existing property/method named `name` always
wins; otherwise a free function `name` in scope is called as
`name(x, args)`. Full resolution order (incl. the `DOT` + call-list
requirement): [language.md §10](language.md) (Methods and UFCS).

```culebra
double = fn (x) { x * 2 }
inspect(42.double())                                  # => 84
inspect('hello world'.split(' ').size())              # => 2

# Existing methods always win — a user `reverse` does NOT
# override Array's built-in `reverse`.
reverse = fn (x) { inspect('user reverse NOT called') }
mut a = [1, 2, 3]
a.reverse()
inspect(a)                                            # => [3, 2, 1]
```

### 11.2 Multimethods (free function multiple dispatch)

Define several functions with the same name but different parameter
types. The runtime picks the most specific match based on the
declared types of the arguments.

```culebra
class Circle { new(r) { self.r = r } }
class Square { new(s) { self.s = s } }

fn area(c: Circle) { 3.14159 * c.r * c.r }
fn area(s: Square) { s.s * s.s }
fn area(n: Long)   { n }                     # fallback for numbers

inspect(area(Circle.new(2)))                    # => 12.56636
inspect(area(Square.new(3)))                    # => 9
inspect(area(10))                               # => 10
```

The dispatch covers free positional arguments, keyword arguments, and
`**splat`, and a Union-annotated parameter (`x: Long | String`,
Ch.14.2) also participates. Full dispatch/specificity rules:
[language.md §20](language.md).

Instance methods dispatch the same way — a class may declare several
methods with the same name but different parameter types:

```culebra
class Calc {
  new() {}
  go(x: Long)   { "long" }
  go(x: String) { "string" }
}
c = Calc.new()
inspect(c.go(1))                  # => 'long'
inspect(c.go('a'))                # => 'string'
```

### 11.3 Dispatch extensions

> **Status: Planned.** Per-callsite inline caching for hot dispatch
> paths is on the roadmap; today each call re-resolves the overload
> set. Class-based (nominal) inheritance was considered and rejected —
> polymorphism over a family of types goes through trait dispatch
> (Ch.14.3) instead, since it composes with UFCS without adding a
> subtyping story.

### Why free functions first?

Multimethods on free functions compose with UFCS and with imported
namespaces without surprises (no implicit subtyping). Method
multimethods need a precedence story (own-class vs UFCS vs free)
that we'd rather lock down with a real workload than guess.

## 12. Decorators

### 12.1 `@deco`

`@deco` before a `fn` (or `class`) binds the result of `deco(original)`
to the original name. Full semantics, incl. the multimethod
interaction: [language.md §21](language.md).

```culebra
log = fn (f) {
  fn (x) {
    inspect("calling with {x}")
    f(x)
  }
}

@log
fn double(x) { x * 2 }

inspect(double(7))
# => |
# 'calling with 7'
# 14
```

### 12.2 Factories and stacking

```culebra
prefix = fn (tag) {
  fn (f) {
    fn () {
      inspect("[{tag}]")
      f()
    }
  }
}

@prefix('A')
@prefix('B')
fn greet() { inspect('hi') }

greet()
# => |
# '[A]'
# '[B]'
# 'hi'
```

Outer decorator wraps the result of the inner; reading top-to-bottom
matches the execution order.

### 12.3 Memoize, a real example

```culebra
memoize = fn (f) {
  mut cache = {}
  fn (x) {
    k = to_string(x)
    if !cache.has(k) { cache[k] = f(x) }
    cache[k]
  }
}

@memoize
fn slow_square(x) { x * x }

inspect(slow_square(7))          # => 49
inspect(slow_square(7))          # => 49
```

### 12.4 `fn.params` introspection

A `Function` value exposes its declared signature, which decorators
can use to write signature-aware wrappers (`@autograd`, `@trace`, ...).

```culebra
add_typed = fn (a: Long, b: Long) -> Long { a + b }
inspect(add_typed.params.map(|p| p.name))    # => ['a', 'b']
inspect(add_typed.return_type)               # => 'Long'
```

A decorated function is bound as a single value (the wrapped
closure), which is incompatible with the "many `fn`s sharing one
name" shape of multimethods — choose one or the other per name (full
rule: [language.md §21](language.md)).

## 13. Modules

### 13.1 `export` and `import`

A module lists what it exposes with `export`; a consumer binds the
whole module under one name with `import`.

```culebra
# doctest: skip
# lib.cul
let greet = fn (name) { "hello, {name}" }
let PI    = 3.14159
let helper = fn () { 'internal' }   # not exported

export { greet, PI }
```

```culebra
# doctest: skip
# main.cul — same directory as lib.cul
import lib from './lib.cul'

inspect(lib.greet('world'))      # => 'hello, world'
inspect(lib.PI)                  # => 3.14159
inspect(lib.helper)              # => nil — not on the export Object
```

The path is a single-quoted literal resolved relative to the
importing file's directory. Several `export` statements in one file
are merged, so a module can declare a few helpers, export them,
and keep going.

### 13.2 Top-level only, evaluated once

`import` and `export` may appear only as top-level statements — inside
a function or an `if` branch they are a `SyntaxError`. That is what
lets the loader determine the whole dependency graph at parse time,
which both the AOT bundler and the tree-shaker rely on (Ch.20).

Each module is evaluated once per program, in dependency order, in its
own scope: its top-level bindings stay private except for the export
Object. A circular import (A imports B imports A) is rejected with an
`ImportError` naming the cycle. Full resolution, caching, and error
rules: [language.md §24](language.md).

### Why explicit?

An explicit `import` line is the only place a reader — or a tool —
has to look to know what a file depends on. It also gives `culebra
lint` an unambiguous unused-import warning (and a `--fix` for it,
Ch.18), and gives the AOT build a graph it can bundle without
guessing.

See [`internals.md` §10](internals.md) for the loader design and
cycle-detection algorithm.

---

Part III — Types and libraries
==============================

## 14. Type system

### 14.1 Today: optional annotations + `Any`

Annotations are *runtime* checks at three boundaries: variable
assignment, function parameter passing, and function return. There is
no static narrowing. Full annotation semantics: [language.md
§14](language.md).

```culebra
add = fn (a: Long, b: Long) -> Long { a + b }
inspect(add(3, 4))               # => 7

# Any matches everything; typed and dynamic parameters can mix
identity = fn (x: Any) -> Any { x }
describe = fn (v, label: String) -> String { "{label}: {v}" }
inspect(identity(42))                  # => 42
inspect(describe([1, 2], 'array'))     # => 'array: [1, 2]'
```

`type_of` (Ch.2.1) is the runtime introspection for built-in types.
`match` arms with `n: ClassName` (Ch.6) match instances of that
class.

### 14.2 Union, Optional, Tuple

`Long | String` accepts either alternative; `T?` is sugar for
`T | Nil`; `(Long, String)` is a fixed-size, immutable, element-wise-
equal `Tuple`. Full semantics: [language.md §14](language.md) (Union
types / Optional types) and [language.md §10](language.md) (Tuples).

```culebra
show = fn (x: Long | String) -> String { to_string(x) }
inspect(show(1))                  # => '1'
inspect(show('hi'))               # => 'hi'

pair = (1, 'one')
inspect(type_of(pair))            # => 'Tuple'
inspect(pair == (1, 'one'))       # => true
```

### 14.3 Trait / Protocol

`trait` declares a set of required methods; any class whose methods
match (by name and arity) conforms — no explicit `impl` needed, and a
class missing a required method fails dispatch (`DispatchError`)
rather than matching silently. Traits can also carry default-
implemented methods and be derived with `@derive`. Full spec: [language.md
§14](language.md) (Traits and protocols). See [`_history.md`](_history.md)
for why nominal (class) inheritance was rejected in favor of this
structural model (Ch.11.3).

```culebra
trait Greeter { hello() -> String }

class Bob {
  new(name)  { self.name = name }
  hello()    { "hi, {self.name}" }
}

greet = fn (x: Greeter) -> String { x.hello() }
inspect(greet(Bob.new('Alice')))   # => 'hi, Alice'
```

### 14.4 Generics

`Array<Long>` and similar annotations document the element type and
feed multimethod specificity (Ch.11.2); the element check itself is a
no-op — Culebra follows Rust/Swift generics in spirit but does not pay
for runtime element checking on every access. Bound constraints and
generic class declarations: [language.md §14](language.md).

```culebra
first = fn (xs: Array<Long>) -> Long { xs[0] }
inspect(first([1, 2, 3]))         # => 1
```

### What Culebra deliberately does *not* take from TypeScript

Conditional types, mapped types, template literal types, and the
suite of utility types (`Pick`, `Omit`, ...) sit at a complexity
level that is too high for a small dynamic language. The target is
Rust/Swift expressiveness, not TS expressiveness.

## 15. Standard library tour

The CLI driver exposes `inspect` / `print` / `println` as aliases for
`IO.inspect` / `IO.print` / `IO.println`. Embedders that build their own
environment see the namespaces but not the bare aliases.

### 15.1 Core built-ins

```culebra
inspect(to_long('42'))           # => 42
inspect(to_string([1, 2]))       # => '[1, 2]'
inspect(iota(2, 5))              # => [2, 3, 4]
assert_eq(1 + 1, 2)           # passes silently; throws on failure
```

Full list: [language.md §19](language.md).

### 15.2 `Math`

```culebra
inspect(Math.abs(-7))            # => 7
inspect(Math.min(3, 5))          # => 3
inspect(Math.max(3, 5))          # => 5
inspect(Math.pow(2, 10))         # => 1024
inspect(Math.sign(-42))          # => -1
inspect(Math.clamp(15, 0, 10))   # => 10
```

Full reference: [`stdlib.md` §1](stdlib.md).

### 15.3 `IO`

```culebra
print('Hello, '); print('world!'); print("\n")   # => Hello, world!
# IO.input()                 # read a line from stdin
# FS.write('out.txt', 'hi')  # write a file
# FS.read('in.txt')          # read a file
```

Full reference: [`stdlib.md` §2](stdlib.md).

### 15.4 `Sys`, `Random`, `FS`, `Time`, `Args`

```culebra
inspect(Sys.argv)                # => []
# Sys.env('HOME')             # process env
# Sys.exit(0)                 # terminate

inspect(Random.int(0, 100) >= 0)          # => true

# Path — a fluent wrapper over FS that carries a path around:
#   let cfg = Path.new('/etc') / 'app.conf'   # `/` joins
#   cfg.parent().name(); cfg.read()           # properties + FS ops
# FS.* and File.open also accept a Path directly.
```

Full reference: `Sys` [`stdlib.md` §7](stdlib.md), `Random` §6, `FS`/`Path`
§3, `Time` §5, `Args` §10.

### 15.5 `Regex`

Linear-time matching (NFA-based, no catastrophic backtracking),
grapheme-cluster–aware by default — no `/u` flag needed.

```culebra
re = Regex.compile('\d+')
inspect(re.test('order #42'))    # => true

# `re"..."` literals are sugar for Regex.compile(pattern, flags);
# the body is raw, so `\d` passes through as-is.
inspect(re'\d+'.test('abc 123')) # => true
```

Full API: [`stdlib.md` §14](stdlib.md).

### 15.6 `Hash`, `Encoding`, `Compress`

`Hash.sha256`/`sha1`/`sha512`/`md5` and their `hmac_*` variants return
hex digests; `Encoding.base64`/`hex`/`url`/`html` each expose
`.encode`/`.decode`; `Compress.gzip`/`gunzip` round out the data
namespaces. JSON is its own top-level `JSON.parse`/`JSON.stringify` —
there is no `Encoding.json`. Full reference: [`stdlib.md`](stdlib.md)
§16 (Encoding), §17 (Compress), §18 (Hash).

### 15.7 `Http`

Blocking client and server, SSE and WebSocket included, TLS via
statically linked OpenSSL. No `async`/`await` — concurrency is via
threads; scale ceiling is a few thousand connections.

```culebra
# doctest: skip
res = Http.get('https://example.com')
inspect(res.status)              # => 200

# Server
srv = Http.server()
srv.get('/', fn (req) { 'hello' })
srv.listen('127.0.0.1', 8080)
```

Streaming, routing, and the client session API: [`stdlib.md`
§15](stdlib.md).

### 15.8 The rest of the library

Not covered above, all documented in [`stdlib.md`](stdlib.md):
`JSON` (§9), `Proc` (§11 — run external commands), `Isolate` /
`Channel` / `Parallel` / `Shared` (§12 — threads and message passing),
`CSV` (§19), `Env` (§20 — dotenv), `UUID` (§21), `Term` (§22 — TUIs),
`Log` (§23), `TOML` (§24), `SQLite` (§25), `Canvas` (§26 — 2D games),
`Scene` (§27 — 3D, opt-in), and `Desktop` / `Webview` (§28 — a native
WebView desktop app in one call).

### 15.9 Still planned

> **Status: Planned (Tier 2/3).** `Crypto` (asymmetric/TLS primitives
> beyond `Hash`) and `Sockets` (raw TCP/UDP). No firm ordering yet —
> demand-driven, per the tiering in Ch.0.

## 16. Tensor primitive

### 16.1 Construction, matmul, broadcasting

`Tensor` is a built-in n-dimensional array backed by the vendored
`cpp-tensorlib` engine (vectorized CPU kernels, Metal on macOS, CUDA
on Linux/Windows). Storage is F32; scalar results surface as `Float`.
Matmul (`dot`) builds a lazy graph that `Tensor.eval` runs as one
fused kernel; elementwise ops broadcast like NumPy. Full API (shapes,
reductions, autograd, device selection):
[`stdlib.md` §8](stdlib.md).

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
b = Tensor.from([10.0, 20.0, 30.0])
inspect((a + b).to_array())      # => [11.0, 22.0, 33.0]
inspect(a.sum())                 # => 6.0

m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
c = m.dot(m)
Tensor.eval(c)
inspect(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

### 16.2 Choosing a device

The same `Tensor` type runs on the GPU — there is no separate GPU
type. Device selection is process-global and switchable at runtime:

```culebra
inspect(type_of(Tensor.gpu_available()))   # => 'Bool'
# Tensor.use_gpu()    # force the GPU backend (Metal / CUDA)
# Tensor.use_cpu()    # force the CPU backend
# Tensor.use_auto()   # per-op choice by problem size (the default)
```

`Tensor.gpu_available()` reports whether a GPU backend was compiled in
and is reachable; `use_auto` falls back to CPU when it isn't. Whether
the GPU wins depends on shape — small tensors lose to kernel-launch
overhead — so `use_auto` is the right default.

### Why a dedicated engine?

Matrix-heavy code (MLP inference, microgpt) shipped with a hand-
written O(n³) loop was orders of magnitude slower than NumPy. Routing
through tuned kernels gets us within ~1.2× of PyTorch CPU on the MNIST
sizes that this codebase actually trains. The full benchmark is in
[`benchmarks/mnist/README.md`](../benchmarks/mnist/README.md) and
[`benchmarks/microgpt/README.md`](../benchmarks/microgpt/README.md).

For the dtype rationale, allocator choice, and lazy-shape
discussion, see [`internals.md` §8](internals.md).

---

Part IV — Verification and deployment
======================================

## 17. Testing (`culebra test`)

> `test()` / `@test` / `@parametrize`, the matcher family, and
> dependency-injected fixtures (any fn in env, no decorator) are
> implemented and work via `culebra test [path]`.

### 17.1 Doctest convention (final)

Every ` ```culebra ` block in this guide, in `language.md`, and in
`stdlib.md` follows this convention:

- `# => <value>` — expected stdout (one line)
- `# => |` followed by `# <line>` lines — expected multi-line stdout
- `# !! <pattern>` — expected `throw`, matched as substring
- `# doctest: <directive>` (block-leading line) — modes:
  - `skip` — illustration only, do not run (e.g. *Planned* features)
  - `compile-only` — syntax check only
  - `interp-only` / `jit-only` / `aot-only` — backend filter

Blocks are independent; no `setup`/`teardown` across blocks.

Run them with `culebra test --doc <path>` (or `just doctest`), which
extracts every block, runs it in a fresh interpreter, and checks output
against the markers. The runner currently honors `skip`; the
`compile-only` and backend-filter directives are reserved (such blocks
run normally for now).

### 17.2 Writing tests

Three forms are available — the call form and the `@test` decorator
are equivalent; pick whichever reads better at the call site.
`@parametrize` registers one test per case.

```culebra
# doctest: skip
# tests/test_string.cul

# Call form
test("interpolation embeds Long", fn() {
  let x = 42
  assert_eq("hi {x}", "hi 42")
})

# Decorator form — fn name becomes the test name
@test
fn interpolation_embeds_float() {
  let pi = 3.14
  assert_eq("π = {pi}", "π = 3.14")
}

# Parametrize — one test per case, named `<fn>[i]`
@parametrize([(1, 2, 3), (2, 3, 5), (10, 20, 30)])
fn adds_correctly(a, b, want) {
  assert_eq(a + b, want)
}
```

**No `describe` nesting.** Group by file path (`tests/strings/`) and
by `/` in the test name (`"Array/push: appends element"`).

**Fixtures by DI.** A test's positional parameters are resolved by
name against the surrounding env: any fn in env can be a fixture, no
decorator required. Fixtures can themselves take fixtures.

```culebra
# doctest: skip
fn db()       { { users: [], next_id: 1 } }
fn user(db)   { db.users.push({ id: 1, name: "alice" }); db.users[0] }

@test
fn user_has_name(user) {
  assert_eq(user.name, "alice")
}
```

Within one test, each fixture is evaluated **once** — multiple
mentions (direct + transitive) share the same instance. Across tests,
fixtures are fresh.

**Cleanup via class `drop`.** Resources needing teardown wrap
themselves in a class with a `drop` method (§7.4). The runtime's
ref-count finalization fires when the per-test fixture cache is
released at test end.

```culebra
# doctest: skip
class TestDB {
  new()    { self.conn = Database.connect("memory") }
  drop()   { self.conn.close() }
  users()  { self.conn.users }
}

fn db() { TestDB.new() }

@test
fn user_count(db) {
  db.users().create("alice")
  assert_eq(db.users().count(), 1)
  # db drops at test end -> conn.close()
}
```

`defer` inside a fixture body would fire when the fixture fn returns
(before the test runs), so class `drop` is the right tool for cleanup.
Long-lived state shared across files (e.g. a model loaded once) goes
at module top level instead, since the module system (Ch.13) caches
the binding.

**Matchers.** Assertions in tests use the matcher family — there is
no `assert` keyword or builtin. Matchers are **3-backend globals**
(bound on every environment, same as `inspect` / `Math`), so they work
identically under `culebra script.cul`, `culebra --jit script.cul`,
`culebra build`, and `culebra test`:

```culebra
# doctest: skip
assert_eq(arr.len(), 3)                 # == ; shows both sides on failure
assert_throws("TypeError", fn() { let _ = 1 + 'b' })
assert_close(0.1 + 0.2, 0.3, 1e-9)      # |a - b| <= tol
```

Full matcher list (`assert_true`/`false`/`ne`/`lt`/`le`/`gt`/`ge` and
the `__eq__`/`__lt__` dispatch rule): [`stdlib.md` §13](stdlib.md).

**Production invariants.** For `if (!cond) throw {...}` checks outside
the test suite, write the `if`/`throw` directly (Go-style, see
[language §15](language.md)). There is no separate `assert` keyword to
disable in production builds.

### 17.3 Running

`culebra test [path]` discovers test files. When invoked through this
subcommand, `test` / `@test` / `@parametrize` become **ambient
globals** alongside the always-available matcher family — no `import`
required. This mirrors how `inspect` / `print` are ambient under
script-execution mode but absent from `culebra::environment()` (see
[stdlib §29](stdlib.md)).

```sh
culebra test                       # discover & run from current dir
culebra test tests/strings/        # run a subtree
culebra test --filter "Array/push" # name-substring filter
culebra test --reporter json       # NDJSON output (one JSON per line)
culebra test --bail                # stop after the first failure
culebra test --bail 3              # stop after 3 failures
culebra test --list                # discover only; print test names
```

Discovery: any path that is a file is included as-is; any path that
is a directory is walked recursively for files matching `test_*.cul`.
Exit code is `0` when all tests pass, `1` when any fail.

**Reporters.** Default is human-readable. `--reporter json` emits one
JSON object per line (NDJSON) — useful for agent loops and CI:

```
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul","stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,"stdout":""}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

User `inspect(...)` from inside a test is captured into the event's
`stdout` field rather than interleaved with the NDJSON stream, and
failure events carry a `snippet` with the failing line marked `>` for
context.

The legacy `tests/*.cul` suite under `just test` (matchers + no
`test()` calls) runs each file directly via `./build/culebra <f>`,
`--jit <f>`, and `culebra build <f>`. Matchers are language-level
globals, so the same file is exercised under all three backends
without going through `culebra test`.

### 17.4 Doctests and planned extensions

`culebra test --doc <path>` extracts every ` ```culebra ` block from
the markdown under `<path>` and runs it against the markers in 17.1
(`just doctest` runs it over `docs/`). Still planned:

- **Explicit `import { test } from "std/test"`** — for code that
  doesn't run under `culebra test` (e.g. embedded test helpers).
- **`--backend interp|jit|aot`** — currently the runner uses interp;
  selecting backends per run is on the roadmap.
- **Parallel execution** — sequential today; parallel default is
  optional once the JIT/AOT backends are wired in.

## 18. Linting (`culebra lint`)

`culebra lint [paths...]` reports static problems **without running**
the program, and exits non-zero so CI can gate on it (0 = clean, 1 =
warnings only, 2 = errors). It reuses the same load-stage static analysis
every backend already runs (so the errors it reports are exactly the ones
that would abort a run) and adds advisory warnings on top.

```bash
culebra lint app.cul
# app.cul:12:7: warning: unused variable 'tmp'
# app.cul:20:3: error: undefined variable 'reuslt'

culebra lint .          # recurse into every .cul under the current directory,
                         # like `culebra fmt -i .`
```

What it reports today:

- **Errors** — the sound, certain-to-fail set: `break` / `continue` /
  `return` out of place, malformed parameter or assignment forms,
  duplicate parameters or class members, shadowing, and reads of a name
  bound nowhere (the undefined-variable subset). These already abort any
  run; `lint` just surfaces them all at once instead of stopping at the
  first.
- **Warnings** — advisory findings that don't stop a run:
  - **Unused local variable** — a `let` / `mut` binding inside a function
    that is never read.
  - **Unused top-level binding** — a top-level `let` / `mut` that the
    module never reads and never re-exports. Function / class / enum /
    trait declarations are the module's export surface and are never
    flagged.
  - **Unused import** — an `import`ed name the module never uses.
  - **Unreachable code** — a statement that can never run because a
    `return` / `throw` / `break` / `continue` precedes it in the same
    block.

  A leading underscore (`_x`, or the bare sink `_`) marks a binding as
  intentionally unused and is never flagged. **Parameters are not
  flagged**: an unused parameter is overwhelmingly intentional in Culebra
  — a multidispatch clause or method signature fixes the arity, and a
  higher-order callback (a route handler `fn(req)`, an `|i| 4.0`) ignores
  an argument it must still declare — so the check would be all noise.

`culebra lint --fix <paths...>` mechanically removes unused-import lines —
the only warning safe to autofix unattended, since deleting a dead
`import` can never change behavior (unlike an unused `let`/`mut`, whose
initializer may carry a side effect). Every other warning stays
report-only. After editing, the fixed source is re-parsed and re-linted;
the rewrite is written only if that re-check confirms the imports are
gone and no new error appeared — the same re-parse safety net `culebra
fmt` uses.

```bash
culebra lint --fix app.cul
# app.cul: fixed 1 unused import
```

Planned: a `--format json` mode for editor / LSP integration, and inline
`# lint: ignore` suppression.

## 19. Formatting (`culebra fmt`)

`culebra fmt [files...]` reformats source to one canonical style:
normalized operator spacing, two-space indentation, brace blocks laid out
multi-line, and argument lists / collection literals wrapped when they
exceed the line width. It is opinionated and zero-config (no style flags),
in the spirit of `gofmt`.

```bash
culebra fmt app.cul          # write the formatted source to stdout
culebra fmt -i app.cul       # rewrite the file in place
culebra fmt -i .             # format every .cul under the current directory
culebra fmt --check app.cul  # exit 1 if it isn't already formatted (CI gate)
culebra fmt -l src/*.cul     # list the files that would change
cat app.cul | culebra fmt -  # stdin -> stdout (editor format-on-save)
```

A directory argument is scanned recursively for `.cul` files, so
`culebra fmt -i .` formats a whole project and `culebra fmt --check .`
gates it in CI.

Comments are preserved: a leading comment stays above the statement it
introduces, a trailing comment stays on the same line, and a single blank
line between statements is kept (runs of blank lines collapse to one).
match / cond arms, class / trait / enum members, destructuring patterns,
and parameter lists are all normalized; long binary expressions and method
chains wrap at the line width.

How it works: the source is parsed, re-printed from the syntax tree, and
then **re-parsed and compared** against the original — if formatting would
change the program's meaning, or would drop or duplicate a comment, `fmt`
refuses and leaves the file untouched rather than risk corrupting it.
Formatting is idempotent: running it twice yields the same result as
running it once.

### Editor integration

The stdin form (`culebra fmt -`) is the format hook: each integration
formats the whole buffer and applies the result only when it exits
zero, leaving the buffer untouched on a parse/safety error.

- **VSCode** — the bundled extension (`misc/vscode/`) registers a
  document formatting provider, so **Format Document** and
  `editor.formatOnSave` work for `.cul` files out of the box (rebuild
  with `build-vsix.sh` / `install.sh`).
- **Zed** — add an external formatter in `settings.json`:
  `"formatter": { "external": { "command": "culebra", "arguments": ["fmt", "-"] } }`
  under `"languages": { "Culebra": { ... } }`.
- **Vim/Neovim** — the bundled `ftplugin` provides a `:CulebraFmt`
  command (cursor preserved, untouched on error); set
  `let g:culebra_fmt_autosave = 1` for format-on-save. It deliberately
  skips `gq` / `'formatprg'`, which would replace an unparseable range
  with empty output.
- Any other editor with a format-on-save hook can pipe the buffer
  through `culebra fmt -` the same way.

## 20. AOT binary build

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops the
runtime helpers your program doesn't reference. Tensor-free programs
also drop the Accelerate / Metal frameworks the tensor engine needs.

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone, ~350 KB on macOS
otool -L ./out                            # no Accelerate, no Metal, no LLVM
```

### 20.1 Cross-compile

```bash
./build/culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./out-linux
```

See [`deployment.md`](deployment.md) for the runtime-archive
build, sysroot expectations, and the full cross-compile workflow.

### Why tree-shaking matters

A "hello world" using `inspect` doesn't need the tensor or HTTP
runtime glue. Tracing the call graph from the entry file lets the
linker drop unreferenced runtime helpers (~200 of them) and, when no
`Tensor` reference is found, swap in a no-tensor archive. The result
is a few hundred KB instead of a few MB.

## 21. Embedding overview

Culebra is a header-friendly C++23 library. Minimal embed:

```cpp
#include <culebra.h>

int main() {
  auto env  = culebra::environment();
  auto value = culebra::eval(env, R"(
    add = fn (a, b) { a + b }
    add(40, 2)
  )");
  std::cout << culebra::to_string(value) << "\n";   // 42
}
```

The embedder constructs the environment, so `IO` is present but
`inspect` / `print` (the CLI aliases) are not — your host owns I/O.
Errors surface as `culebra::Error` exceptions carrying the original
thrown value plus line/column.

See [`deployment.md`](deployment.md) for environment customization,
value conversion, hosting the JIT, and the AOT-archive embed pathway
(`libculebra_rt.a`).

---

Where to go next
----------------

- Formal grammar and evaluation rules: [`language.md`](language.md)
- API reference: [`stdlib.md`](stdlib.md)
- Implementation internals: [`internals.md`](internals.md)
- Binary builds, embedding, and wrapping: [`deployment.md`](deployment.md)
- Larger worked example: [`benchmarks/microgpt/`](../benchmarks/microgpt/)
- Interactive REPL: `./build/culebra --shell`
