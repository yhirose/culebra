Culebra Standard Library
========================

This document specifies the **built-in library** of Culebra: the
namespace objects (`Math`, `IO`, `Sys`) that group the runtime
utilities. Everything described here is available without any
`import` statement.

Language-level built-ins — `assert`, `to_long`, `to_string`,
`type_of` — are specified in
[§18 of the language spec](language.md). Methods on built-in types
(`String`, `Array`, `Object`) are specified in
[§17 of the language spec](language.md).

The CLI (`src/main.cc`) additionally installs `puts` and `print` as
globals aliased to `IO.puts` / `IO.print` (see [§19 of the language
spec](language.md)). Embedders that use `culebra::environment()`
directly get a clean namespace without those aliases.

Conventions used below:

* Types follow the annotations described in [§14 of the language
  spec](language.md). `Any` denotes any value.
* Throws clauses describe runtime errors of the form
  `type error at L:C.` etc. See [§15 of the language spec](language.md).

Index
-----

1. `Math`
2. `IO`
3. `Random`
4. `Sys`
5. Design notes
6. Not included (yet)

---

1. `Math`
---------

Numeric utilities. Integer-only routines (`pow`, `sign`, `clamp`,
`iota`, `range`) preserve `Long` input; the Float-domain routines
(`log`, `exp`, `sqrt`, …) accept either `Long` or `Float` and return
the shape documented below. See [§4](language.md#4-types) and
[§7](language.md#7-expressions) of the language spec for how `Long`
and `Float` interact.

### Constants

`Math.pi`, `Math.e`, `Math.inf`, `Math.nan` are `Float` properties
with the obvious values (`π`, `e`, positive infinity, and a quiet
NaN). Both backends evaluate these as compile-time constants under
`--jit`.

```culebra
puts(Math.pi)              # 3.141592653589793
puts(Math.e)               # 2.718281828459045
puts(Math.inf > 1e308)     # true
puts(Math.nan == Math.nan) # false
```

### `Math.abs(x: Long|Float) -> Long|Float`

Absolute value. Returns `Long` for `Long` input, `Float` for `Float`
input.

```culebra
puts(Math.abs(-7))     # 7
puts(Math.abs(-7.5))   # 7.5
```

### `Math.min(a, b, ...) -> Long|Float`, `Math.max(a, b, ...) -> Long|Float`

Smallest / largest of two or more numeric arguments. Returns `Long`
when every argument is `Long`; any `Float` argument promotes the
result to `Float`. At least two arguments are required; fewer — or
any non-numeric argument — raises `type error`.

```culebra
puts(Math.min(3, 1, 4, 1, 5))   # 1
puts(Math.max(1.5, 2, 0.5))     # 2.0
```

### `Math.log(x: Long|Float) -> Float`

Natural logarithm of `x`. `Math.log(0)` is `-inf`; `Math.log` of a
negative value is `nan`. The return type is always `Float` even when
the result is mathematically an integer.

### `Math.exp(x: Long|Float) -> Float`

`e` raised to `x`.

### `Math.sqrt(x: Long|Float) -> Float`

Principal square root. `Math.sqrt(-1.0)` is `nan`.

### `Math.floor(x: Long|Float) -> Long`, `Math.ceil(x: Long|Float) -> Long`, `Math.round(x: Long|Float) -> Long`

Round a numeric value to an integer. `Long` input is returned
unchanged. `Math.floor` rounds toward `-∞`, `Math.ceil` toward `+∞`,
and `Math.round` uses **banker's rounding** (round half to even,
matching Python's built-in `round()`).

```culebra
puts(Math.floor(-1.5))   # -2
puts(Math.ceil(-1.5))    # -1
puts(Math.round(2.5))    # 2      (ties to even)
puts(Math.round(3.5))    # 4
```

### `Math.pow(base: Long, exp: Long) -> Long`

Integer exponentiation. `base ** exp`, computed by repeated squaring.
`Math.pow(x, 0)` is `1` for every `x` (including `0`).

**Throws**: `type error at L:C.` if `exp < 0`.

Kept for back-compat; **prefer the `**` operator** which also handles
`Float` and negative exponents (see language spec §7).

```culebra
puts(Math.pow(2, 10))    # 1024
puts(Math.pow(7, 0))     # 1
puts(Math.pow(-3, 3))    # -27
```

### `Math.sign(x: Long) -> Long`

Returns `-1` for negative, `0` for zero, `1` for positive.

```culebra
puts(Math.sign(-5))      # -1
puts(Math.sign(0))       # 0
puts(Math.sign(42))      # 1
```

### `Math.clamp(x: Long, lo: Long, hi: Long) -> Long`

Clamp `x` to the inclusive range `[lo, hi]`. No error is raised when
`lo > hi`; the result in that case is `hi`.

```culebra
puts(Math.clamp(5, 0, 10))   # 5
puts(Math.clamp(-5, 0, 10))  # 0
puts(Math.clamp(15, 0, 10))  # 10
```

### `Math.iota(n: Long) -> Array` / `Math.iota(start: Long, end: Long) -> Array`

Generate a new `Array` of consecutive integers. Named after APL /
C++ `std::iota` / Scheme SRFI-1 — materialise a run of integers as an
array. The **lazy** counterpart is `Math.range` (same arguments,
iterator return) — prefer it for `for`-in loops and use `Math.iota`
when you actually need the full `Array`.

* `Math.iota(n)` returns `[0, 1, ..., n-1]`. If `n <= 0`, an empty array.
* `Math.iota(start, end)` returns `[start, start+1, ..., end-1]`. If
  `start >= end`, an empty array.

```culebra
puts(Math.iota(3))         # [0, 1, 2]
puts(Math.iota(2, 5))      # [2, 3, 4]
puts(Math.iota(5, 2))      # []
```

### `Math.range(n: Long) -> Iterator` / `Math.range(start: Long, end: Long) -> Iterator`

Lazy counterpart to `Math.iota`: returns an iterator yielding the
same integer sequence one element at a time. Use with `for`-in or
iterator method chains (`Math.range(N).map(...).reduce(...)` etc.)
to iterate in **constant additional memory** regardless of the range
size. Empty-range conventions match `iota`: `n <= 0` or
`start >= end` yields an iterator that completes immediately.

```culebra
for i in Math.range(5)     { puts(i) }     # 0, 1, 2, 3, 4
for i in Math.range(2, 6)  { puts(i) }     # 2, 3, 4, 5

# Constant memory even for huge bounds
for i in Math.range(1_000_000_000) {
  if i > 3 { break }
  puts(i)
}
```

**JIT**: `Math.range` returns a JIT-native iterator Object under
`--jit`; for-in and the lazy iterator methods drive it at protocol
speed (one closure call per step). Use `Math.iota` when you want an
eager `Array` for maximum per-element throughput; use `Math.range`
when you want constant-memory streaming over a potentially huge
sequence. See language.md §17.5.

---

2. `IO`
-------

Output, standard input, and file I/O.

### `IO.puts(x: Any) -> Nil`

Print `x` followed by a newline to standard output. Reference types
are formatted the same way as `Array.str_array()` /
`Object.str_object()`, and strings are printed **with surrounding
single quotes**.

```culebra
IO.puts('hi')       # → 'hi'
IO.puts(42)         # → 42
IO.puts([1, 'a'])   # → [1, 'a']
```

### `IO.print(x: Any) -> Nil`

Write `x` to standard output **without a trailing newline**, using
`to_string` formatting (strings are **unquoted**). Useful for
building a single line of output from several writes.

```culebra
IO.print('Hello, ')
IO.print('world!')
IO.puts('')         # → Hello, world!
```

### `IO.input() -> String`

Read a single line from standard input. The trailing newline is
stripped. Returns `''` (empty string) on end-of-file.

```culebra
puts('name?')
name = IO.input()
puts("Hello, {name}")
```

### `IO.read(path: String) -> String`

Read the entire file at `path` into a `String`.

**Throws**: `type error at L:C.` if the file cannot be opened.

```culebra
contents = IO.read('data.txt')
```

### `IO.write(path: String, content: String) -> Nil`

Write `content` to the file at `path`, creating or overwriting it.

**Throws**: `type error at L:C.` if the file cannot be opened for
writing.

```culebra
IO.write('out.txt', 'hello\n')
```

### `IO.exists(path: String) -> Bool`

Return whether an entry exists at `path`. Does not distinguish
regular files from directories or symlinks. An empty or invalid path
returns `false`. Useful for the check-then-download pattern without
needing `try`/`catch`.

```culebra
if !IO.exists('data.txt') {
  IO.write('data.txt', 'hello')
}
```

---

3. `Random`
-----------

Random-number generation. The process has a single shared
Mersenne-Twister-64 engine, shared between the interpreter and JIT
backends; `Random.seed(n)` resets it and makes every subsequent draw
reproducible within one program execution. Without a `seed` call the
engine is initialised from `std::random_device`.

### `Random.seed(n: Long) -> Nil`

Reseed the shared PRNG. Same `n` → same sequence.

```culebra
Random.seed(42)
```

### `Random.int(lo: Long, hi: Long) -> Long`

Uniform integer in the half-open range `[lo, hi)`. Requires `hi > lo`,
otherwise `type error`.

```culebra
Random.seed(0)
puts(Random.int(0, 10))        # 0..9
```

### `Random.uniform(lo: Float, hi: Float) -> Float`

Uniform real in the half-open range `[lo, hi)`. `Long` arguments are
accepted and promoted to `Float`.

### `Random.gauss(mu: Float, sigma: Float) -> Float`

A sample from a Gaussian distribution with the given mean and
standard deviation. `Long` arguments are promoted to `Float`.

```culebra
Random.gauss(0.0, 1.0)         # standard normal
```

### `Random.shuffle(a: Array) -> Nil`

Fisher–Yates in-place permutation. Returns `nil`; the argument is
mutated.

### `Random.weighted_choice(pop: Array, weights: Array) -> Any`

Draw a single element from `pop` with probability proportional to the
matching `weights` entry. Weights must all be numeric and of the same
length as `pop`; empty or mismatched inputs raise `type error`.
Weights of `0` are never selected.

```culebra
Random.weighted_choice(['hit', 'miss'], [1, 9])   # ~10% 'hit'
```

---

4. `Sys`
--------

Process-level information.

### `Sys.argv -> Array`

Array of `String` arguments passed to the script on the command
line. Everything after a standalone `--` is captured; the
`culebra` executable and script paths themselves are excluded.
Empty when no `--` block was given or when running in the REPL.

```culebra
# $ culebra run.cul -- hello world
puts(Sys.argv)        # ['hello', 'world']
```

### `Sys.exit(code: Long) -> Nil`

Terminate the process immediately with the given exit code. Does
not return; pending `defer` statements are *not* run.

```culebra
if error_occurred { Sys.exit(1) }
```

### `Sys.env(name: String) -> String`

Return the value of the environment variable `name`, or `''` (empty
string) if it is not set. Use `.size() > 0` to distinguish an unset
variable from one set to the empty string.

```culebra
puts(Sys.env('HOME'))          # '/Users/alice'
puts(Sys.env('NOT_A_VAR'))     # ''
```

---

5. Design notes
---------------

### Namespace-first, CLI-aliased globals

The library adds **no global names**: everything lives under
`Math`, `IO`, `Random`, or `Sys`. This keeps `culebra::environment()`
free of surprises for embedders who use Culebra as a scripting
engine inside a host application.

For CLI scripting, however, `puts` / `print` are so pervasive that
writing `IO.puts` everywhere adds friction. The CLI binary
(`src/main.cc`) installs them as globals right after constructing
the environment — pointing to the same function values that live
under `IO`, so there is no duplication. V8 takes an analogous
approach: the engine provides no `print`, and the `d8` shell
installs one.

### Free function vs method

Free functions (in namespaces) are used when the operation either
constructs a value from nothing (`Math.iota`, `IO.input`) or applies
uniformly to multiple types (`type_of`, `to_string`). Operations
that are *about* a specific type use method syntax — see §17 of the
language spec for String/Array/Object methods.

### Error-by-throw versus `nil` returns

The library prefers throwing on unrecoverable type errors
(`to_long('abc')`, `IO.read(...)` on a missing file) and returning
sentinel values for "found or not" predicates (`IO.input()` returns
`''` on EOF). This keeps hot paths simple without requiring a
`try`/`catch` mechanism.

---

6. Not included (yet)
---------------------

### Trigonometry

`Math.sin` / `Math.cos` / `Math.tan` / `Math.atan2` are not yet
exposed. Random drawing and the core transcendentals (`log`, `exp`,
`sqrt`) are available; trig entries can be added when a concrete use
case lands.

### Regular expressions

Deferred; would require either a built-in regex engine or a vendored
dependency.

### Date, time

Deferred. Scripts that need these today can call out through
`IO.read` / `IO.write` with a helper process.

### Collections beyond `Array`/`Object`

No `Set`, `Queue`, `Tuple`, etc. Use `Array` and `Object` for now.

---

See also: [`docs/language.md`](language.md) for the language
specification.
