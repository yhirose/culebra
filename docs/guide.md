The Culebra Guide
=================

A small, dynamically-typed scripting language: immutable bindings by
default, blocks that evaluate to a value, and pattern matching in the
core. Two backends share one AST — a tree-walking interpreter and an
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
> and directive list: [`tooling.md` §1](tooling.md#doctests).

> **Status labels.** Section headings without a label describe the
> implementation as of today. Labels that appear: **Draft** (under
> implementation, API may change), **Planned** (decided, not yet
> implemented), **Deprecated** (slated for removal).

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
  10. [UFCS and multimethods](#10-ufcs-and-multimethods)
  11. [Decorators](#11-decorators)
  12. [Modules](#12-modules)
- **Part III — Types and libraries**
  13. [Type system](#13-type-system)
  14. [Standard library](#14-standard-library)
- **Part IV — Verification and deployment**
  15. [Tooling (`test`, `lint`, `fmt`, debug)](#15-tooling-test-lint-fmt-debug)
  16. [AOT binary build](#16-aot-binary-build)
  17. [Embedding overview](#17-embedding-overview)

## 0. Design philosophy

Read this once; the rest of the guide assumes these choices.

- **Two backends, one AST.** A tree-walking interpreter and an LLVM
  ORC JIT share the same AST. The interpreter has no LLVM dependency
  (a ~15 MB driver against ~100 MB once LLVM is linked in); the JIT
  runs the same program at `-O2`. Both are maintained — neither is
  going away.
- **Eight everyday types.** `Nil`, `Bool`, `Long`, `Float`, `String`,
  `Array`, `Object`, `Function`, plus four specialized ones
  (`StringView`, `Tuple`, `Set`, `Tensor`). Everything else (classes,
  modules, errors) builds on `Object`.
- **Immutable by default, `mut` to opt in.** A binding cannot be
  reassigned unless it says so, and a block evaluates to its last
  expression, so `let x = if c { a } else { b }` is ordinary code.
  Closures are first-class, errors are values, no hidden globals.
- **UFCS, not pipeline.** Any free function `f(x, ...)` can be called
  as `x.f(...)`. A pipeline operator was considered and rejected.
- **Explicit, static modules.** A file exposes bindings with
  `export { ... }` and a consumer binds them with
  `import name from './path.cul'`. Both forms are top-level only, so
  the dependency graph is known at parse time — which is what makes
  AOT bundling and tree-shaking possible (Ch.12).
- **No async/await.** I/O is blocking by design; concurrency comes
  from threads, not coroutines. HTTP and similar networked stacks are
  blocking with the typical scale ceiling of a few thousand
  connections.
- **Batteries-included, tiered.** The core (Math/IO/FS/File/Sys/
  Random/String/Time/Args) and Tier 1 (Regex/Http/Hash/Encoding/
  Compress/JSON/CSV/TOML/SQLite/UUID/Log/Term/Canvas) both ship
  today; Tier 2/3 (Crypto, Sockets) follows demand — see Ch.14.
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
just build-no-jit       # interpreter only, ~15 MB
just dev                # fast no-LTO -O1 build into build-dev/ (inner loop)
just test-dev           # quick interp==JIT check vs build-dev/ (after each edit)
just test               # all backends + embed smoke (parallel; JOBS=1 to serialize)
just test wrap          # `culebra wrap` end-to-end (not part of `just test`)
just install            # copy the Release binary to /usr/local/bin (`just install ~/.local` for a user install)
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

Comments start with `#` or `//` (line), or `/* ... */` (block). Statements may
end with `;`; newlines also separate statements. The recommended style
is to omit `;` at end-of-line.

```culebra
# this is a comment
inspect('hello')          # => 'hello'
```

`inspect` prints values in quoted, inspectable form, so strings appear
with surrounding quotes (`'hello'`) and reference types in their literal
shape. Use `println` for raw, unquoted text with a trailing newline, or
`print` to omit the newline too — see Ch.14.

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
`Tuple` and `Set` (Ch.13.2), and `Tensor` (Ch.14.2). The full table is
[language.md §4](language.md).

### 2.2 Bindings: bare, `let`, `mut`

```culebra
x = 10              # bare: new immutable binding, or reassign outer
let y = 20          # let: new immutable binding (must not shadow outer)
mut z = 30          # mut: new mutable binding
z = 31              # mut allows reassignment
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
    n += 1        # bare reassignment of the captured `n` — OK
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

size = match x {                 # match is an expression too (Ch.6)
  0           => 'zero',
  n if n < 10 => 'small',
  _           => 'large'
}
inspect(size)                    # => 'small'

mut i = 0
while i < 3 { inspect(i); i += 1 }
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

`break` leaves the loop; `continue` skips to the next iteration. Both
work inside `while` and `for`.

```culebra
for n in 0..10 {
  if n % 2 == 1 { continue }      # skip the odd numbers
  if n > 4 { break }              # stop once past 4
  inspect(n)
}
# => |
# 0
# 2
# 4
```

### 2.5 `nobreak`, init clauses, `cond`, and `? :`

A loop may carry a `nobreak` block that runs only when the loop
finished without a `break`. The keyword names the condition it tests,
so the block needs no comment to say when it runs:

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
while mut i = 0; i < 3 { i += 1 }
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

# Type annotations are optional; see Ch.13
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
  fn () { n += 1; n }      # a bare assignment reassigns the captured `n`
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
  for v in rest { t += v }
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

### 4.2 Iteration and slicing

Strings iterate by Unicode *scalar* (one codepoint per step), not by
byte. There is no `s[i]` subscript — a String is not an indexable
container, and `'café'[0]` is a `TypeError`. Substrings come from
`slice`, whose offsets are byte offsets into the UTF-8 representation
(§4.4).

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

### Why byte indexing?

Hiding the bytes-vs-scalars distinction behind an opaque character
index reads well until the string meets a socket or a file, where
every length and offset is a byte count and the two models have to be
reconciled at each boundary. Culebra exposes the byte offsets directly
and puts the other views beside them: explicit code-point iteration
for scalar work, and the lazy grapheme iteration above for display.

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
    next:     fn () { v = i; i -= 1; v }
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
  while i > 0 { yield i; i -= 1 }
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
  if !buf.empty() { yield buf }
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

Add `with return(v) { … }` to map the normal-completion value. It runs only on
*normal* completion, so a clause that aborts bypasses it:

```culebra
effect fn ask()

let out = handle {
  let n = perform ask()
  n + 1
} with ask(k) { k(41) }
  with return(v) { "final={v}" }
inspect(out)   # => 'final=42'
```

A `handle` may also be written inside an effectful body, to capture and resume
from an enclosing computation. Full reference and limitations:
[language.md §16](language.md).

The pattern that pays off most is search. An N-queens `search` that
performs `choose(...)` for a column and `reject()` for a dead end never
mentions backtracking; the enclosing `handle` decides what those mean, so
one body runs as an exhaustive enumeration, a first-answer search, or a
placement counter depending only on the handler.

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
  run(n)    { self.miles += self.mpr * n }
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
      run:   fn (n) { miles += mpr * n },
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

### 9.4 Operator overloading

Arithmetic, comparison, indexing, and call operators each map to a
dunder method (`__add__`, `__eq__`, `__lt__`, `__index__`, `__call__`,
...) that a class can define to participate in the operator. The
reverse-side method (`__radd__`, ...) is *not* supported — place your
overload on the type that owns the operation. Full method table and
dispatch rules: [language.md §10](language.md) (Operator overloading).

#### A worked example: 2-D vectors

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

#### Subscripting

`__index__(key)` and `__setindex__(key, value)` make `obj[k]` and
`obj[k] = v` delegate to the class, so a wrapper subscripts like a
built-in. They fire only for keys the object doesn't hold as a direct
property.

```culebra
class Grid {
  new()               { self.d = [10, 20, 30] }
  __index__(i)        { self.d[i] }
  __setindex__(i, v)  { self.d[i] = v }
}

g = Grid.new()
g[1] = 99
inspect(g[0])                    # => 10
inspect(g[1])                    # => 99
```

### 9.5 `__call__` for callable instances

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

### Why support both `class` and closure-based OO?

Closures-as-objects came first and remain the right answer for
disposable encapsulation (e.g. one-off iterators, scope guards). The
`class` form earns its keep when an object travels far and needs an
identity (the `class:` tag, used by `match` and debug output).

## 10. UFCS and multimethods

### 10.1 UFCS resolution order

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

### 10.2 Multimethods (free function multiple dispatch)

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
Ch.13.2) also participates. Full dispatch/specificity rules:
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

### 10.3 Dispatch extensions

> **Status: Planned.** Per-callsite inline caching for hot dispatch
> paths is on the roadmap; today each call re-resolves the overload
> set. Class-based (nominal) inheritance was considered and rejected —
> polymorphism over a family of types goes through trait dispatch
> (Ch.13.3) instead, since it composes with UFCS without adding a
> subtyping story.

### Why free functions first?

Multimethods on free functions compose with UFCS and with imported
namespaces without surprises (no implicit subtyping). Method
multimethods need a precedence story (own-class vs UFCS vs free)
that we'd rather lock down with a real workload than guess.

## 11. Decorators

### 11.1 `@deco`

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

### 11.2 Factories and stacking

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

### 11.3 Memoize, a real example

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

### 11.4 `fn.params` introspection

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

## 12. Modules

### 12.1 `export` and `import`

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

### 12.2 Top-level only, evaluated once

`import` and `export` may appear only as top-level statements — inside
a function or an `if` branch they are a `SyntaxError`. That is what
lets the loader determine the whole dependency graph at parse time,
which both the AOT bundler and the tree-shaker rely on (Ch.16).

Each module is evaluated once per program, in dependency order, in its
own scope: its top-level bindings stay private except for the export
Object. A circular import (A imports B imports A) is rejected with an
`ImportError` naming the cycle. Full resolution, caching, and error
rules: [language.md §24](language.md).

### Why explicit?

An explicit `import` line is the only place a reader — or a tool —
has to look to know what a file depends on. It also gives `culebra
lint` an unambiguous unused-import warning (and a `--fix` for it,
Ch.15), and gives the AOT build a graph it can bundle without
guessing.

See [`internals.md` §10](internals.md) for the loader design and
cycle-detection algorithm.

---

Part III — Types and libraries
==============================

## 13. Type system

### 13.1 Today: optional annotations + `Any`

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

### 13.2 Union, Optional, Tuple, Set

`Long | String` accepts either alternative; `T?` is sugar for
`T | Nil`; `(Long, String)` is a fixed-size, immutable, element-wise-
equal `Tuple`. Full semantics: [language.md §14](language.md) (Union
types / Optional types) and [language.md §10](language.md) (Tuples /
Sets).

```culebra
show = fn (x: Long | String) -> String { to_string(x) }
inspect(show(1))                  # => '1'
inspect(show('hi'))               # => 'hi'

pair = (1, 'one')
inspect(type_of(pair))            # => 'Tuple'
inspect(pair == (1, 'one'))       # => true
```

A `Set` is an insertion-ordered collection of unique hashable values.
Its literal needs two elements — or a trailing comma — so it can't be
confused with the empty Object `{}` or the `{key: value}` shorthand.
Duplicates collapse on construction and equality ignores order.

```culebra
s = {1, 2, 3, 3}
inspect(s.size())                 # => 3
inspect(s.contains(2))            # => true
inspect({1, 2, 3} == {3, 2, 1})   # => true
inspect({42,})                    # => {42}
```

Set operations are methods, not operators (`|` is taken by lambda
parameters):

```culebra
inspect({1, 2}.union({2, 3}))            # => {1, 2, 3}
inspect({1, 2, 3}.intersect({2, 3, 4}))  # => {2, 3}
inspect({1, 2, 3}.diff({3,}))            # => {1, 2}
```

### 13.3 Trait / Protocol

`trait` declares a set of required methods; any class whose methods
match (by name and arity) conforms — no explicit `impl` needed, and a
class missing a required method fails dispatch (`DispatchError`)
rather than matching silently. Traits can also carry default-
implemented methods and be derived with `@derive`. Nominal (class)
inheritance was considered and rejected in favour of this structural
model (Ch.10.3). Full spec: [language.md §14](language.md) (Traits and
protocols).

```culebra
trait Greeter { hello() -> String }

class Bob {
  new(name)  { self.name = name }
  hello()    { "hi, {self.name}" }
}

greet = fn (x: Greeter) -> String { x.hello() }
inspect(greet(Bob.new('Alice')))   # => 'hi, Alice'
```

A trait method with a body is a **default**: conforming classes inherit
it, and declaring the same name on the class overrides it.

```culebra
trait Counter {
  current() -> Long
  next() -> Long { self.current() + 1 }   # default body
}

class Zero {
  new()      {}
  current()  { 0 }
}
inspect(Zero.new().next())         # => 1
```

`@derive(...)` generates the conformance methods a data class would
otherwise spell out — `Eq` → `eq`, `Hash` → `hash`, `Show` → `to_s`,
`Comparable` → `cmp`. A method the class declares itself is never
overwritten, so you can derive most and hand-write one.

```culebra
@derive(Eq, Hash, Show)
class Point { new(x, y) { self.x = x; self.y = y } }

inspect(Point.new(1, 2).eq(Point.new(1, 2)))   # => true
inspect(Point.new(1, 2).to_s())                # => 'Point(1, 2)'
```

### 13.4 Generics

`Array<Long>` and similar annotations document the element type and
feed multimethod specificity (Ch.10.2); the element check itself is a
no-op. The parameter is there for the reader and for dispatch —
enforcing it would mean walking the collection on every call, which is
not what these annotations are for. Bound constraints and generic class
declarations: [language.md §14](language.md).

```culebra
first = fn (xs: Array<Long>) -> Long { xs[0] }
inspect(first([1, 2, 3]))         # => 1
```

### Where the annotations stop

Types that compute other types — conditions over types, transformations
of one shape into another, types built from string patterns — are
deliberately absent. Each is a small language of its own to learn,
implement and debug, and annotations here have a narrower job: catching
a wrong value where it crosses a boundary. Anything a type-level
program would express, a runtime check expresses more directly.

## 14. Standard library

Every namespace below is available without an `import`. The CLI driver
additionally installs `inspect` / `print` / `println` as aliases for
`IO.inspect` / `IO.print` / `IO.println`; embedders that build their own
environment get the namespaces but not the bare aliases.

```culebra
inspect(to_long('42'))               # => 42
inspect(Math.clamp(15, 0, 10))       # => 10
inspect(re'\d+'.test('order #42'))   # => true
inspect(JSON.stringify({a: 1}))      # => '{"a":1}'
```

### 14.1 What ships

| Area | Namespaces |
|---|---|
| Numbers and text | `Math`, `Regex` |
| Files, processes, environment | `FS`, `File`, `Path`, `Proc`, `Sys`, `Env` |
| Data formats | `JSON`, `CSV`, `TOML`, `Encoding`, `Compress`, `Hash`, `UUID` |
| Network | `Http`, `Net` |
| Concurrency | `Isolate`, `Channel`, `Parallel`, `Shared`, `SharedBuffer` |
| Storage | `SQLite` |
| Time, CLI, logging | `Time`, `Args`, `Log` |
| Terminal and graphics | `Term`, `Canvas`, `Scene`, `Desktop` |

[`stdlib.md`](stdlib.md) documents each of them. Its index lists the
namespaces in order, and the "Where to find what" table right below it
maps a task — hash something, parse a config file, run a subprocess —
onto the section that does it. What is deliberately *not* included yet
is recorded there too, at the end.

The built-ins that are not namespaced — `to_long`, `to_string`,
`type_of`, `range`, `iota`, and the matcher family — are specified in
[language.md §19](language.md#19-core-built-in-functions).

### 14.2 `Tensor`

`Tensor` is an n-dimensional array built into the language rather than
a namespace over it, backed by the vendored `cpp-tensorlib` engine
(vectorized CPU kernels, Metal on macOS, CUDA on Linux/Windows).
Storage is F32 and scalar results surface as `Float`. Elementwise ops
broadcast like NumPy; `dot` builds a lazy graph that `Tensor.eval` runs
as one fused kernel.

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
inspect((a + a).to_array())      # => [2.0, 4.0, 6.0]

m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
c = m.dot(m)
Tensor.eval(c)
inspect(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

The same type runs on the GPU — there is no separate GPU type.
`Tensor.use_gpu()` / `use_cpu()` / `use_auto()` switch the backend
process-wide, and `use_auto` (the default) picks per operation by
problem size, since small tensors lose to kernel-launch overhead.
Shapes, reductions, autograd and device details:
[`stdlib.md` §8](stdlib.md#8-tensor). The dtype, allocator and
lazy-shape rationale is in [`internals.md` §8](internals.md).

## 15. Tooling (`test`, `lint`, `fmt`, debug)

The `culebra` binary is also the toolchain. The test runner, the linter,
the formatter and the debug adapter are subcommands of the same
executable, so there is nothing extra to install. This chapter is a tour;
the flag-by-flag reference is [`tooling.md`](tooling.md).

### 15.1 Tests

A test file is a `.cul` file named `test_*.cul`. Inside it, `test()`, the
`@test` decorator and `@parametrize` are ambient — no import — and
assertions use the matcher family:

```culebra
# doctest: skip
# tests/test_math.cul
@test
fn adds_correctly() {
  assert_eq(1 + 2, 3)
}

@parametrize([(1, 2, 3), (10, 20, 30)])
fn adds_each(a, b, want) {
  assert_eq(a + b, want)
}
```

```sh
culebra test                        # discover & run from the current dir
culebra test --filter "Array/push"  # run a subset by name
```

A test's parameters are resolved by name against the surrounding
environment, so any function in scope can serve as a fixture, and a
fixture that returns a class instance gets its `drop` called at test end
(Ch.7.4). The same runner also executes the examples in this guide —
`culebra test --doc docs` — which is why every ` ```culebra ` block here
carries its expected output.

### 15.2 Lint and format

`culebra lint` reports what static analysis can see without running the
program: errors that would abort a run anyway, plus advisory warnings
(unused variable / import / unreachable code). It exits 0 clean, 1 for
warnings, 2 for errors, so CI can gate on it.

```bash
culebra lint .          # recurse over every .cul below the current dir
culebra lint --fix .    # additionally delete unused import lines
```

`culebra fmt` is the zero-config formatter — no style flags. It parses,
re-prints, then **re-parses and compares** before writing, so a format
that would change meaning or drop a comment is refused rather than
applied.

```bash
culebra fmt -i .        # format the project in place
culebra fmt --check .   # exit 1 if anything is unformatted (CI gate)
```

Editors hook the formatter through the stdin form (`culebra fmt -`); the
bundled VSCode, Zed and Vim integrations are already wired to it.

### 15.3 Debugging

`culebra dap` speaks the Debug Adapter Protocol over stdio, so
breakpoints, stepping, call stacks, watch expressions and editing a `mut`
variable mid-run all work in any DAP-capable editor. Your editor launches
the adapter; you rarely run it by hand. Debugging runs in the interpreter
— don't pass `--jit`.

A bare `debugger` statement in the source forces a stop wherever you put
it, with no configuration at all. Per-editor setup (VSCode, Vim,
Zed) is in [`tooling.md` §4](tooling.md#4-debugging-culebra-dap).

## 16. AOT binary build

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops the
runtime helpers your program doesn't reference. Tensor-free programs
also drop the Accelerate / Metal frameworks the tensor engine needs.

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone, ~6 MB on macOS
otool -L ./out                            # no Accelerate, no Metal, no LLVM
```

### 16.1 Cross-compile

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
linker drop unreferenced runtime helpers (~450 of them) and, when no
`Tensor` reference is found, swap in a no-tensor archive. The result
is ~6 MB where a program force-loading every feature archive is
~12.5 MB.

## 17. Embedding overview

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
