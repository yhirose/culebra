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
> with `# doctest: skip` is illustrative only (typically a *Planned*
> feature). Blocks are independent: each runs in a fresh scope.

> **Status labels.** Section headings without a label describe the
> implementation as of today. Labels that appear: **Draft** (under
> implementation, API may change), **Planned** (decided, not yet
> implemented), **Deprecated** (slated for removal). Features that
> were considered and rejected live in [`internals.md` §13](internals.md).

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
- **Part II — Tools for abstraction**
  8. [Classes](#8-classes)
  9. [Operator overloading](#9-operator-overloading)
  10. [UFCS and multimethods](#10-ufcs-and-multimethods)
  11. [Decorators](#11-decorators)
  12. [Modules](#12-modules)
- **Part III — Types and libraries**
  13. [Type system](#13-type-system)
  14. [Standard library tour](#14-standard-library-tour)
  15. [Tensor primitive](#15-tensor-primitive)
- **Part IV — Verification and deployment**
  16. [Testing (`culebra test`)](#16-testing-culebra-test)
  17. [AOT binary build](#17-aot-binary-build)
  18. [Embedding overview](#18-embedding-overview)

0. Design philosophy
--------------------

Read this once; the rest of the guide assumes these choices.

- **Two backends, one AST.** A tree-walking interpreter and an LLVM
  ORC JIT share the same AST. The interpreter has no LLVM dependency
  (~1 MB binary, good for embedding); the JIT runs the same program
  at `-O2`. Both are maintained — neither is going away.
- **Eight built-in types.** `Nil`, `Bool`, `Long`, `Float`, `String`,
  `Array`, `Object`, `Function`. Everything else (classes, modules,
  errors) builds on `Object`.
- **Rust-inspired surface.** `let` / `mut` / `fn` / `match` / block-as-
  expression. Closures are first-class, errors are values, no hidden
  globals.
- **UFCS, not pipeline.** Any free function `f(x, ...)` can be called
  as `x.f(...)`. A pipeline operator was considered and rejected (see
  [`internals.md` §13](internals.md)).
- **Implicit imports, no explicit `import` statement.** Bare references
  to top-level identifiers cross file boundaries within a module
  build (Ch.12). Explicit `import` was considered and rejected.
- **No async/await.** I/O is blocking by design; concurrency comes
  from threads, not coroutines. HTTP and similar networked stacks are
  blocking with the typical scale ceiling of a few thousand
  connections.
- **Batteries-included, tiered.** Core stdlib (Math/IO/Sys/Random/
  String/FS/Time/Args) ships today. Tier 1 (Regex/HTTP/Hash+Encoding)
  is the next priority — see Ch.14.
- **Pre-1.0.** Source and APIs may change. There is no release
  machinery (no version tags, no CHANGELOG, no Homebrew formula) yet
  — that comes after 1.0.

---

Part I — The Language Core
==========================

1. Hello & setup
----------------

Build the interpreter (and JIT, if LLVM 17+ is installed):

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB
just dev                # fast no-LTO build into build-dev/ (inner loop)
just test-dev           # quick interp==JIT check vs build-dev/ (after each edit)
just test               # all backends + embed smoke (parallel; JOBS=1 to serialize)
./build/culebra --shell # REPL (add --jit for the JIT REPL)
```

A Culebra source file uses the `.cul` extension. Run it with the
`culebra` binary:

```bash
echo "puts('hello, culebra!')" > hello.cul
./build/culebra hello.cul          # interpreter
./build/culebra --jit hello.cul    # JIT (same output)
./build/culebra --help             # all options and commands
```

Comments start with `#` (line) or `/* ... */` (block). Statements may
end with `;`; newlines also separate statements. The recommended style
is to omit `;` at end-of-line.

```culebra
# this is a comment
puts('hello')          # => 'hello'
```

`puts` prints values in *inspect* form, so strings appear with
surrounding quotes (`'hello'`) and reference types in their literal
shape. Use `print` for raw, unquoted text (it also omits the trailing
newline) — see Ch.11.

2. Values, bindings, and control flow
-------------------------------------

### 2.1 The eight types

```culebra
puts(type_of(nil))            # => 'Nil'
puts(type_of(true))           # => 'Bool'
puts(type_of(42))             # => 'Long'
puts(type_of(3.14))           # => 'Float'
puts(type_of('hi'))           # => 'String'
puts(type_of([1, 2]))         # => 'Array'
puts(type_of({a: 1}))         # => 'Object'
puts(type_of(fn () { 1 }))    # => 'Function'
```

### 2.2 Bindings: bare, `let`, `mut`

```culebra
x = 10              # bare: new immutable binding, or reassign outer
let y = 20          # let: new immutable binding (must not shadow outer)
mut z = 30          # mut: new mutable binding
z = z + 1           # mut allows reassignment
z += 1              # compound (`-= *= /= %= **= @=` work the same way)
puts(z)             # => 32
```

A bare assignment searches outward through enclosing scopes and
reassigns the nearest matching binding; if nothing matches, it creates
a new binding in the current scope. This is what makes the
closure-based object pattern work (Ch.8).

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
puts(c())                     # => 1
puts(c())                     # => 2
```

### 2.4 Control flow

`if` and `match` (Ch.6) are expressions; their value is the chosen
branch. `while` and `for` are statements (value is `nil`).

```culebra
x = 7
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
puts(sign)                    # => 1

mut i = 0
while i < 3 { puts(i); i = i + 1 }
# => |
# 0
# 1
# 2

for n in 0..3   { puts(n) }   # exclusive range
# => |
# 0
# 1
# 2

for n in 0..=2  { puts(n) }   # inclusive range
# => |
# 0
# 1
# 2
```

`break` and `continue` work inside `while` and `for`.

### Why split shadow rules by axis?

Captured state *is* the object's state in the closure-based pattern;
silently shadowing it would break the object. Block-local rebinding,
in contrast, is a routine pattern (`let a = transform(a)` in a single
function). Most languages apply one policy to all axes; Culebra
splits them because each axis serves a different purpose.

3. Functions and closures
-------------------------

### 3.1 `fn` and `|x|`

```culebra
add = fn (a, b) { a + b }
puts(add(2, 3))               # => 5

# Type annotations are optional; see Ch.13
add_typed = fn (a: Long, b: Long) -> Long { a + b }
puts(add_typed(2, 3))         # => 5

# |x| expr is shorthand for fn (x) { expr }
square = |x| x * x
puts(square(7))               # => 49

# Use `self` for recursion (the function refers to itself)
fib = fn (x) {
  if x < 2 { x } else { self(x - 2) + self(x - 1) }
}
puts(fib(10))                 # => 55
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
puts(c())                     # => 1
puts(c())                     # => 2
puts(c())                     # => 3
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
puts(greet('alice'))                       # => 'hi, alice'
puts(greet('alice', greeting: 'hello'))    # => 'hello, alice'
puts(greet('bob', formal: true))           # => 'hi, Mr./Ms. bob'

# `**` splats an Object as keyword arguments:
opts = {greeting: 'yo', formal: false}
puts(greet('carol', **opts))               # => 'yo, carol'
```

### Why keyword-only?

A `*` marker forces the caller to name the option, which keeps long
parameter lists readable and lets you reorder/extend them without
breaking call sites. Free positional rest (`*args`) is intentionally
omitted: Array literals fill that role.

4. Strings
----------

### 4.1 Interpolation and concatenation

```culebra
name = 'Culebra'
puts("hello, {name}!")                   # => 'hello, Culebra!'
puts("two plus three is {2 + 3}")        # => 'two plus three is 5'
puts('a' + 'b' + 'c')                    # => 'abc'
```

### 4.2 Iteration and indexing

Strings iterate by Unicode *scalar* (one codepoint per step), not by
byte. Indexing is by byte offset into the UTF-8 representation; out-
of-bounds raises.

```culebra
for c in 'café' { puts(c) }
# => |
# 'c'
# 'a'
# 'f'
# 'é'

puts('café'.size())            # => 5
```

`size()` counts bytes in the UTF-8 representation (`é` is 2 bytes, so
`'café'` is 5), while the `for` loop above steps one Unicode scalar at
a time (four steps).

### 4.3 Common methods

```culebra
puts('hello world'.split(' '))        # => ['hello', 'world']
puts('  hi  '.trim())                 # => 'hi'
puts('abc'.upper())                   # => 'ABC'
puts('foo'.starts_with('fo'))         # => true
puts(['a', 'b', 'c'].join('-'))       # => 'a-b-c'
```

See [`stdlib.md` §String](stdlib.md) for the full list.

### 4.4 `StringView`, `StringLike`, lazy graphemes

> **Status: Planned.** Not implemented yet. See
> [`internals.md` §6](internals.md) for the design discussion.
> Together these let user code accept either `String` or a borrow
> without copying, and walk grapheme clusters lazily.

```culebra
# doctest: skip
# Parameter type StringLike accepts both String and StringView:
print_first_grapheme = fn (s: StringLike) {
  for g in s.graphemes() { puts(g); break }
}
print_first_grapheme('🇯🇵 hello')   # planned: => 🇯🇵
```

### Why Go-style byte indexing?

Swift/Python 3 hide the bytes-vs-scalars distinction behind an
opaque `Character` / `str` index, which is convenient until you
interop with sockets or files. Go exposes byte offsets and offers
explicit `rune` iteration on top; Culebra follows that model:
predictable I/O semantics, with scalar iteration when you need it,
and (planned) lazy grapheme iteration for display work.

5. Iterators
------------

### 5.1 `range` (lazy) vs `iota` (eager)

```culebra
# range builds nothing; for-loops consume it lazily
for i in range(3) { puts(i) }
# => |
# 0
# 1
# 2

# iota allocates an Array
puts(iota(3))                 # => [0, 1, 2]
puts(iota(2, 5))              # => [2, 3, 4]
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
puts(result)                  # => [0, 6, 12, 18, 24]

total = range(1, 11).reduce(0, |a, x| a + x)
puts(total)                   # => 55

puts([1, 2, 3, 4].iter().any(|x| x > 3))      # => true
puts([10, 20, 30].iter().find(|x| x > 15))    # => 20
```

### 5.3 `enumerate`, `zip`, `flat_map`, `skip`, `take_while`

```culebra
for i, v in ['fizz', 'buzz', 'bang'].enumerate() {
  puts("{i}: {v}")
}
# => |
# '0: fizz'
# '1: buzz'
# '2: bang'

for p in [1, 2, 3].iter().zip(['a', 'b', 'c']) {
  puts("{p.first} / {p.second}")
}
# => |
# '1 / a'
# '2 / b'
# '3 / c'

flat = [[1, 2], [3], [4, 5, 6]].iter().flat_map(|xs| xs).collect()
puts(flat)                    # => [1, 2, 3, 4, 5, 6]

head = range(100).skip(10).take_while(|x| x < 15).collect()
puts(head)                    # => [10, 11, 12, 13, 14]
```

### 5.4 User-defined iterators

Implement three methods: `iter()` (returning the iterator itself, by
convention), `has_next()` (returning a `Bool`), and `next()` (returning
the next element).

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter:     fn () { this },
    has_next: fn () { i > 0 },
    next:     fn () { v = i; i = i - 1; v }
  }
}

for v in countdown(3) { puts(v) }
# => |
# 3
# 2
# 1
```

### 5.5 Generators with `yield`

> **Status: Planned.** A `yield`-based generator syntax is on the
> roadmap. Today, write iterators by hand (5.4) or compose with
> `range`/`iter()` (5.1–5.3).

### Why no pipeline `|>`?

`x.f(...)` reads the same way and also serves as the resolution path
for free functions over user types (Ch.10). Adding `|>` would split
the idiom space without functional gain.

6. Pattern matching
-------------------

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
puts(describe(0))             # => 'zero'
puts(describe(2))             # => 'small'
puts(describe(999))           # => 'big (999)'
puts(describe('hi'))          # => 'str (hi)'
puts(describe([1]))           # => 'other'
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
puts(classify(-5))            # => -1
puts(classify(0))             # => 0
puts(classify(7))             # => 1
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
puts(shape([]))               # => 'empty'
puts(shape([10, 20]))         # => 'two (10,20)'
puts(shape([1, 2, 3, 4]))     # => 'head=1, rest=3'

first_name = fn (people) {
  match people {
    [{name}, ..._] => name,
    _              => 'none'
  }
}
puts(first_name([{name: 'x'}, {name: 'y'}]))     # => 'x'
puts(first_name([]))                              # => 'none'
```

### 6.4 Recursion

```culebra
is_even = fn (n) {
  match n {
    0 => true,
    1 => false,
    _ => self(n - 2)
  }
}
puts(is_even(10))             # => true
puts(is_even(7))              # => false
```

### Why no exhaustiveness check?

Without a static type system, exhaustiveness on `Object` shape would
require runtime tracking that costs more than it saves. The `_`
arm (or a guarded final pattern) makes intent explicit. When the
type system grows (Ch.13), Union exhaustiveness becomes feasible.

7. Error handling and RAII
--------------------------

### 7.1 `throw`, `try`, `catch`

The thrown value can be any Culebra value — String, Object, anything.

```culebra
validate = fn (x) {
  if x < 0 { throw "negative: {x}" }
  x
}

try {
  puts(validate(42))          # => 42
  puts(validate(-1))          # throws, the next line is unreached
  puts('unreached')
} catch e {
  puts("caught: {e}")         # => 'caught: negative: -1'
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
puts(safe(7))                 # => 7
puts(safe(-99))               # => 0
```

### 7.3 `defer`

`defer { ... }` registers a cleanup that runs on every exit path from
the *enclosing block* (normal fall-through, `return`, or `throw`) in
LIFO order. Wrap the protected code in `{ }` so the cleanups also fire
when the body throws.

```culebra
demo = fn (fail) {
  {
    defer { puts('cleanup A') }
    defer { puts('cleanup B') }
    if fail { throw 'failed' }
    puts('work done')
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
  { drop: fn () { puts("R{id} released") } }
}

puts('enter')
{
  r = make_resource('X')
}
puts('exit')
# => |
# 'enter'
# 'RX released'
# 'exit'
```

`drop` cascades: when the outer object is released, its members
(themselves holding refs to inner objects with `drop`) release in
turn.

### 7.5 Scope guard pattern

```culebra
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
  {
    g = make_guard()
    defer { g.run() }

    items.for_each(fn (item) {
      g.add(fn () { puts("close {item}") })
      puts("open {item}")
    })
  }
}

process(['a', 'b'])
# => |
# 'open a'
# 'open b'
# 'close b'
# 'close a'
```

### Why allow any value to be thrown?

A typical `throw "msg"` is enough for scripts; classed errors (Ch.8)
are enough for libraries; you don't need a hierarchy to start. Catch
arms can pattern-match on whatever shape the thrower used (Ch.6).

---

Part II — Tools for abstraction
================================

8. Classes
----------

### 8.1 Syntax

`class` declares a constructor (`new`) and methods. Fields set via
`this.x = ...` are mutable by default. Instances carry a readable
`class:` tag.

```culebra
class Car {
  new(mpr)  { this.miles = 0; this.mpr = mpr }
  run(n)    { this.miles = this.miles + this.mpr * n }
  total()   { "total: {this.miles} miles" }
}

car = Car.new(5)
car.run(1); car.run(2)
puts(car.total())             # => 'total: 15 miles'
puts(car.class)               # => 'Car'
```

### 8.2 The closure-based alternative

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
puts(car.total())             # => 'total: 15 miles'
```

Use `class` when you want the `class:` tag and matchable shape; use
the closure form when private state matters more than the tag.

### 8.3 Static methods

> **Status: Planned.** Today, factories live as free functions
> alongside the class:
>
> ```culebra
> class Shape { new(name) { this.name = name } }
> make_circle = fn (r) { Shape.new("circle r={r}") }
> puts(make_circle(3).name)         # => circle r=3
> ```
>
> Once static methods land, the factory will be `Shape.make_circle(3)`.

### Why support both `class` and closure-based OO?

Closures-as-objects came first and remain the right answer for
disposable encapsulation (e.g. one-off iterators, scope guards). The
`class` form earns its keep when an object travels far and needs an
identity (the `class:` tag, used by `match` and debug output).

9. Operator overloading
-----------------------

### 9.1 Special methods

| operator | method        | typical use         |
|----------|---------------|---------------------|
| `+`      | `__add__`     | numeric / vec       |
| `-`      | `__sub__`     | numeric / vec       |
| `*`      | `__mul__`     | numeric / scalar    |
| `/`      | `__div__`     | numeric             |
| `%`      | `__mod__`     | numeric             |
| `**`     | `__pow__`     | numeric             |
| `@`      | `__matmul__`  | matrix multiply     |
| unary -  | `__neg__`     | negation            |
| `==`     | `__eq__`      | equality            |
| `<`      | `__lt__`      | ordering (`<=` etc. follow) |
| `()`     | `__call__`    | callable instances  |
| `[i]`    | `__index__`   | indexing            |

The reverse-side method (`__radd__`, ...) is *not* supported; place
your overload on the type that owns the operation.

### 9.2 A worked example: 2-D vectors

```culebra
class Vec2 {
  new(x, y)   { this.x = x; this.y = y }
  __add__(o)  { Vec2.new(this.x + o.x, this.y + o.y) }
  __sub__(o)  { Vec2.new(this.x - o.x, this.y - o.y) }
  __mul__(k)  { Vec2.new(this.x * k, this.y * k) }
  __neg__()   { Vec2.new(-this.x, -this.y) }
  __eq__(o)   { this.x == o.x && this.y == o.y }
  show()      { "({this.x}, {this.y})" }
}

a = Vec2.new(1, 2)
b = Vec2.new(3, 4)
puts((a + b).show())          # => '(4, 6)'
puts((b - a).show())          # => '(2, 2)'
puts((a * 3).show())          # => '(3, 6)'
puts((-a).show())             # => '(-1, -2)'
puts(a == Vec2.new(1, 2))     # => true
```

### 9.3 `__call__` for callable instances

> **Status: Planned.** Calling an instance directly (`add5(10)`) is not
> yet wired up. For now, give the class a named method and call that
> (`add5.apply(10)`).

```culebra
# doctest: skip
class Adder {
  new(n)        { this.n = n }
  __call__(x)   { x + this.n }
}

add5 = Adder.new(5)
puts(add5(10))                # => 15
puts(add5(99))                # => 104
```

10. UFCS and multimethods
-------------------------

### 10.1 UFCS resolution order

For `x.name(args)`:

1. If `x` has a property/method named `name`, use it.
2. Else, if a free function `name` is in scope, call it as
   `name(x, args)`.
3. Else, property access yields `nil`; a call on `nil` errors.

```culebra
double = fn (x) { x * 2 }
puts(42.double())                                  # => 84
puts('hello world'.split(' ').size())              # => 2

# Existing methods always win — a user `reverse` does NOT
# override Array's built-in `reverse`.
reverse = fn (x) { puts('user reverse NOT called') }
mut a = [1, 2, 3]
a.reverse()
puts(a)                                            # => [3, 2, 1]
```

### 10.2 Multimethods (free function multiple dispatch)

Define several functions with the same name but different parameter
types. The runtime picks the most specific match based on the
declared types of the arguments.

```culebra
class Circle { new(r) { this.r = r } }
class Square { new(s) { this.s = s } }

fn area(c: Circle) { 3.14159 * c.r * c.r }
fn area(s: Square) { s.s * s.s }
fn area(n: Long)   { n }                     # fallback for numbers

puts(area(Circle.new(2)))                    # => 12.56636
puts(area(Square.new(3)))                    # => 9
puts(area(10))                               # => 10
```

The dispatch covers free positional arguments, keyword arguments, and
`**splat`. Instance methods are *not* yet multimethod-dispatched (see
10.3).

### 10.3 Dispatch extensions

> **Status: Planned.** All four are decided but not yet implemented.
> Today, work around with explicit `match v.class` or with separate
> function names.
>
> - **Class inheritance dispatch** — a parameter `x: Shape` accepts
>   subclasses of `Shape`, with the closest match chosen.
> - **Union annotation dispatch** — `fn f(x: Long | String)` selects
>   on the runtime type of `x`.
> - **Method multimethods** — `class.method` participates in the same
>   resolution that free functions enjoy today.
> - **Dispatch IC** — per-callsite inline cache for hot dispatch.

### Why free functions first?

Multimethods on free functions compose with UFCS and with imported
namespaces without surprises (no implicit subtyping). Method
multimethods need a precedence story (own-class vs UFCS vs free)
that we'd rather lock down with a real workload than guess.

11. Decorators
--------------

### 11.1 `@deco`

`@deco` before a `fn` (or `class`) binds the result of `deco(original)`
to the original name.

```culebra
log = fn (f) {
  fn (x) {
    puts("calling with {x}")
    f(x)
  }
}

@log
fn double(x) { x * 2 }

puts(double(7))
# => |
# 'calling with 7'
# 14
```

### 11.2 Factories and stacking

```culebra
prefix = fn (tag) {
  fn (f) {
    fn () {
      puts("[{tag}]")
      f()
    }
  }
}

@prefix('A')
@prefix('B')
fn greet() { puts('hi') }

greet()
# => |
# '[A]'
# '[B]'
# 'hi'
```

Outer decorator wraps the result of the inner; here `@prefix('A')`
wraps the function already wrapped by `@prefix('B')`. Reading top-to-
bottom matches the execution order.

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

puts(slow_square(7))          # => 49
puts(slow_square(7))          # => 49
```

### 11.4 `fn.params` introspection

> **Status: Planned.** Will expose a function's declared parameter
> names and types, enabling decorators like `@autograd` and `@trace`
> that need to know the signature.

### Why don't decorators and multimethods coexist?

A decorated function is bound as a single value (the wrapped
closure), which is incompatible with the "many `fn`s sharing one
name" shape of multimethods. Choose one or the other per name.

12. Modules
-----------

### 12.1 Implicit imports

A Culebra "module build" starts at one entry file and pulls in every
sibling `.cul` file the entry transitively references by top-level
identifier.

```culebra
# lib.cul
greet = fn (name) { "hello, {name}" }
PI    = 3.14159
```

```culebra
# doctest: skip
# main.cul — same directory as lib.cul
puts(greet('world'))          # => hello, world
puts(PI)                      # => 3.14159
```

Running `culebra main.cul` discovers `lib.cul` automatically by
following the unresolved name `greet`. There is no `import`
statement.

### 12.2 Entry-env isolation

Bindings introduced *in the entry file* (`main.cul` above) are not
visible from imported files. Bindings introduced *in imported files*
become visible everywhere the build can reach. This matches Go's
intuition (every file in a package is "the package"), with the entry
treated like a top-level main.

### 12.3 Cycles

Cyclic references between files are detected at module-build time
and rejected with a precise file/line. Refactor through a shared
third file.

### Why implicit?

A 1500-line program may pull in 30 helper files. Forcing each
`import` saves nothing — the tool can derive the same graph from
the unresolved identifiers. The cost is a faster, simpler authoring
loop, and tree-shaking still works because the resolver knows which
top-levels are reachable (Ch.17).

See [`internals.md` §10](internals.md) for the resolver design and
cycle-detection algorithm.

---

Part III — Types and libraries
==============================

13. Type system
---------------

### 13.1 Today: optional annotations + `Any`

Annotations are *runtime* checks at three boundaries: variable
assignment, function parameter passing, and function return.

```culebra
let x: Long = 10
puts(x)                       # => 10

add = fn (a: Long, b: Long) -> Long { a + b }
puts(add(3, 4))               # => 7

# Any matches everything
identity = fn (x: Any) -> Any { x }
puts(identity(42))            # => 42
puts(identity('hi'))          # => 'hi'

# Mix typed and dynamic parameters
describe = fn (v, label: String) -> String { "{label}: {v}" }
puts(describe([1, 2], 'array'))     # => 'array: [1, 2]'
```

`type_of` (Ch.2.1) is the runtime introspection for built-in types.
`match` arms with `n: ClassName` (Ch.6) match instances of that
class.

### 13.2 Union, Optional, Tuple

> **Status: Planned.** `Long | String`, `T?` (= `T | Nil`), and
> `(Long, String)` are decided but not yet implemented.

```culebra
# doctest: skip
let id: Long | String = 42
let maybe: Long?      = nil
let pair: (Long, String) = (1, 'one')
```

### 13.3 Trait / Protocol

> **Status: Planned.** Structural and nominal both in scope; the
> trade-off (and which to introduce first) is open. See
> [`internals.md` §13](internals.md).

### 13.4 Generics

> **Status: Planned.** Required before 1.0. Adding generics after
> the language has a large library is expensive (Java 5 era
> taught us); Culebra commits to landing them while the surface area
> is still small.

### What Culebra deliberately does *not* take from TypeScript

Conditional types, mapped types, template literal types, and the
suite of utility types (`Pick`, `Omit`, ...) sit at a complexity
level that is too high for a small dynamic language. The target is
Rust/Swift expressiveness, not TS expressiveness.

14. Standard library tour
-------------------------

The CLI driver exposes `puts` / `print` as aliases for `IO.puts` /
`IO.print`. Embedders that build their own environment see the
namespaces but not the bare aliases.

### 14.1 Core built-ins

```culebra
puts(to_long('42'))           # => 42
puts(to_long('  -7 '))        # => -7
puts(to_string(42))           # => '42'
puts(to_string([1, 2]))       # => '[1, 2]'
puts(type_of(42))             # => 'Long'
puts(iota(3))                 # => [0, 1, 2]
puts(iota(2, 5))              # => [2, 3, 4]
assert_eq(1 + 1, 2)           # passes silently; throws on failure
```

### 14.2 `Math`

```culebra
puts(Math.abs(-7))            # => 7
puts(Math.min(3, 5))          # => 3
puts(Math.max(3, 5))          # => 5
puts(Math.pow(2, 10))         # => 1024
puts(Math.sign(-42))          # => -1
puts(Math.clamp(15, 0, 10))   # => 10
```

### 14.3 `IO`

```culebra
print('Hello, '); print('world!'); print("\n")   # => Hello, world!
# IO.input()                 # read a line from stdin
# FS.write('out.txt', 'hi')  # write a file
# FS.read('in.txt')          # read a file
```

### 14.4 `Sys`, `Random`, `String`, `FS`, `Time`, `Args`

Brief tour; full reference in [`stdlib.md`](stdlib.md).

```culebra
puts(Sys.argv)                # => []
# Sys.env('HOME')             # process env
# Sys.exit(0)                 # terminate

puts(Random.int(0, 100) >= 0)          # => true

# String, FS, Time, Args namespaces — see stdlib.md
```

### 14.5 `Regex`

> **Status: Planned (Tier 1).** Linear-time matching (NFA-based,
> no catastrophic backtracking), grapheme-cluster–aware by default.
> No `/u` flag needed: matching always operates at the grapheme
> level for `.` and character classes.

```culebra
# doctest: skip
re = Regex.compile("\\d+")
m = re.match('order #42')
puts(m.text)                  # planned: => 42
```

### 14.6 `Hash` + `Encoding`

> **Status: Partly implemented.** `Encoding.base64`, `Encoding.hex`,
> `Encoding.url`, `Encoding.html` (`.encode`/`.decode`) and `Compress.gzip`
> / `Compress.gunzip` are available (see stdlib §16–17). JSON is the
> top-level `JSON` namespace (`JSON.parse` / `JSON.stringify`) — there is
> no `Encoding.json`. **Planned (Tier 1):** `Hash.sha256(bytes)`,
> `Hash.md5`.

### 14.7 `HTTP`

> **Status: Planned (Tier 1).** Blocking, SSE and WebSocket
> included, TLS via statically linked BoringSSL. No `async`/`await`
> — concurrency is via threads. Scale ceiling: a few thousand
> connections.

```culebra
# doctest: skip
res = HTTP.get('https://example.com')
puts(res.status)              # planned: => 200

# Server
HTTP.serve('127.0.0.1', 8080, fn (req) {
  HTTP.response(200, 'hello')
})
```

### 14.8 More planned

> **Status: Planned (Tier 2/3).** `Compression` (gzip), `Crypto`,
> `Process` (subprocess), `Sockets` (raw TCP/UDP). No firm
> ordering yet; demand-driven.

### Why "batteries-included, tiered"?

Picking up Culebra for a CLI tool or a small server should not
require pulling in a package manager. Tier 1 (Regex / HTTP /
Hash+Encoding) covers most everyday scripts; Tier 2/3 follows when
a concrete user pushes a feature into the critical path.

15. Tensor primitive
--------------------

### 15.1 Construction and arithmetic

`Tensor` is a built-in n-dimensional array routed through BLAS
(Apple Accelerate on macOS, OpenBLAS on Linux). The element type is
`Float` (F64) today.

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
b = Tensor.from([10.0, 20.0, 30.0])
puts((a + b).to_array())      # => [11.0, 22.0, 33.0]
puts((a * 2.0).to_array())    # => [2.0, 4.0, 6.0]
puts(a.sum())                 # => 6.0
```

### 15.2 Shape and matmul

```culebra
m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
puts(m.shape())               # => [2, 2]

# Matmul (`dot`) builds a lazy graph; `Tensor.eval` runs the BLAS kernel.
c = m.dot(m)
Tensor.eval(c)
puts(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

### 15.3 Broadcasting

```culebra
row = Tensor.from([1.0, 2.0, 3.0])
col = Tensor.from([[10.0], [20.0]])
puts((row + col).to_array())  # => [[11.0, 12.0, 13.0], [21.0, 22.0, 23.0]]
```

### 15.4 GPU primitive

> **Status: Planned.** A CUDA / Metal Shading Language backend is
> on the roadmap, exposed as a separate `Matrix` (or `GTensor`)
> primitive — `Tensor` stays CPU-only.

### Why route through BLAS?

Matrix-heavy code (MLP inference, microgpt) shipped with a hand-
written O(n³) loop was orders of magnitude slower than NumPy. BLAS
gets us within ~1.2× of PyTorch CPU on the MNIST sizes that this
codebase actually trains. The full benchmark is in
[`benchmarks/mnist/README.md`](../benchmarks/mnist/README.md) and
[`benchmarks/microgpt/README.md`](../benchmarks/microgpt/README.md).

For the F32/F64 trade-off, allocator choice, and lazy-shape
discussion, see [`internals.md` §8](internals.md).

---

Part IV — Verification and deployment
======================================

16. Testing (`culebra test`)
----------------------------

> `test()` / `@test` / `@parametrize`, the matcher family, and
> dependency-injected fixtures (any fn in env, no decorator) are
> implemented and work via `culebra test [path]`.

### 16.1 Doctest convention (final)

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

### 16.2 Writing tests

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
  new()    { this.conn = Database.connect("memory") }
  drop()   { this.conn.close() }
  users()  { this.conn.users }
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

Long-lived state shared across files — e.g. a model loaded once —
goes at module top level and is imported by each test file. The
module system caches the binding, so `import` always returns the
same instance.

**Matchers.** Assertions in tests use the matcher family — there is
no `assert` keyword or builtin. Matchers are **3-backend globals**
(bound on every environment, same as `puts` / `Math`), so they work
identically under `culebra script.cul`, `culebra --jit script.cul`,
`culebra build`, and `culebra test`:

```culebra
# doctest: skip
assert_true(x)                          # x is truthy
assert_false(x)                         # x is falsy
assert_eq(arr.len(), 3)                 # == ; shows both sides on failure
assert_ne(status, "error")              # !=
assert_lt(elapsed, 1.0)                 # <
assert_le(count, max)                   # <=
assert_gt(score, 0)                     # >
assert_ge(items.len(), 1)               # >=
assert_throws("TypeError", fn() { let _ = 1 + 'b' })
assert_close(0.1 + 0.2, 0.3, 1e-9)      # |a - b| <= tol
```

- `assert_eq` / `assert_ne` / `assert_lt` / `assert_le` / `assert_gt` /
  `assert_ge` use the same `__eq__` / `__lt__` / `__le__` dispatch as
  the operators, so `assert_eq(p1, p2)` and the expression `p1 == p2`
  agree for class instances.
- `assert_throws(kind, fn)` invokes 0-arg `fn()` and asserts it throws
  with the given `kind` (for built-in errors) or `.kind` property (for
  `throw { kind: ..., message: ... }`).
- `assert_close(a, b, tol)` asserts `|a - b| <= tol`. NaN counts as
  failure (a naive `>` would silently pass divergent computations).

**Production invariants.** For `if (!cond) throw {...}` checks outside
the test suite, write the `if`/`throw` directly (Go-style, see
[language §15](language.md)). There is no separate `assert` keyword to
disable in production builds.

### 16.3 Running

`culebra test [path]` discovers test files. When invoked through this
subcommand, `test` / `@test` / `@parametrize` become **ambient
globals** alongside the always-available matcher family — no `import`
required. This mirrors how `puts` / `print` are ambient under
script-execution mode but absent from `culebra::environment()` (see
[stdlib §11](stdlib.md)).

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
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul",
 "stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,
 "source":"tests/test_math.cul",
 "snippet":" 10  @test\n 11  fn divides_correctly() {\n 12>   assert_eq(6/2, 4)\n 13  }\n",
 "stdout":""}
{"event":"file_error","source":"tests/test_bad.cul","kind":"SyntaxError",
 "message":"..."}
{"event":"test_list","name":"divides_correctly","source":"tests/test_math.cul"}
{"event":"list_end","count":42}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

In JSON mode, user `puts(...)` from inside a test is captured into the
event's `stdout` field rather than interleaved with the NDJSON stream.
Failure events carry a `snippet` with the failing line marked `>` and
two lines of context on either side, so a consumer can show the
relevant code without an extra file read.

The legacy `tests/*.cul` suite under `just test` (matchers + no
`test()` calls) runs each file directly via `./build/culebra <f>`,
`--jit <f>`, and `culebra build <f>`. Matchers are language-level
globals, so the same file is exercised under all three backends
without going through `culebra test`.

### 16.4 Planned extensions

- **Explicit `import { test } from "std/test"`** — for code that
  doesn't run under `culebra test` (e.g. embedded test helpers).
- **`culebra test --doc docs/`** — extract and run ` ```culebra `
  blocks from markdown using the convention in 16.1.
- **`--backend interp|jit|aot`** — currently the runner uses interp;
  selecting backends per run is on the roadmap.
- **Parallel execution** — sequential today; parallel default is
  optional once the JIT/AOT backends are wired in.

17. AOT binary build
--------------------

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops the
runtime helpers your program doesn't reference. Tensor-free programs
also drop the Accelerate / BLAS framework dependency.

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone, ~350 KB on macOS
otool -L ./out                            # no Accelerate, no LLVM
```

### 17.1 Cross-compile

```bash
./build/culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt_no_tensor.a \
  -o ./out-linux
```

See [`binary_build.md`](binary_build.md) for the runtime-archive
build, sysroot expectations, and the full cross-compile workflow.

### Why tree-shaking matters

A "hello world" using `puts` doesn't need the FFT or HTTP runtime
glue. Tracing the call graph from the entry file lets the linker
drop unreferenced runtime helpers (~200 of them) and, when no
`Tensor` reference is found, swap in a no-BLAS archive. The result is
a few hundred KB instead of a few MB.

18. Embedding overview
----------------------

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
`puts` / `print` (the CLI aliases) are not — your host owns I/O.
Errors surface as `culebra::Error` exceptions carrying the original
thrown value plus line/column.

See [`embedding.md`](embedding.md) for environment customization,
value conversion, hosting the JIT, and the AOT-archive embed pathway
(`libculebra_rt.a`).

---

Where to go next
----------------

- Formal grammar and evaluation rules: [`language.md`](language.md)
- API reference: [`stdlib.md`](stdlib.md)
- Implementation internals: [`internals.md`](internals.md)
- Larger worked example: [`benchmarks/microgpt/`](../benchmarks/microgpt/)
- Interactive REPL: `./build/culebra --shell`
