# Culebra Standard Library

This document is the **API reference** for Culebra's built-in
library: the namespace objects (`Math`, `IO`, `Sys`, `FS`, `Time`,
`Args`, `Random`, `String`) that group the runtime utilities.
Everything described here is available without any `import`
statement.

For an introductory tour and usage idioms see
[`guide.md` §14](guide.md#14-standard-library-tour). For library
implementation details and rationale see
[`internals.md`](internals.md).

Language-level built-ins — `to_long`, `to_float`, `to_string`,
`type_of`, `range`, `iota` — are specified in
[§18 of the language spec](language.md). The matcher family
(`assert_true` / `assert_eq` / `assert_throws` / etc.) is documented
in [§13 below](#13-matchers). Methods on built-in types (`String`,
`Array`, `Object`) are specified in
[§17 of the language spec](language.md).

The CLI (`src/main.cc`) additionally installs `puts` and `print` as
globals aliased to `IO.puts` / `IO.print` (see [§20 of the language
spec](language.md)). Embedders that use `culebra::environment()`
directly get a clean namespace without those aliases.

Conventions used below:

* Types follow the annotations described in [§14 of the language
  spec](language.md). `Any` denotes any value.
* Throws clauses describe runtime errors of the form
  `type error at L:C.` etc. See [§15 of the language spec](language.md).

## Index

1. [`Math`](#1-math) — numeric utilities, constants, integer sequences
2. [`IO`](#2-io) — output, stdin (file I/O lives under `FS`)
3. [`FS`](#3-fs) — path manipulation, file/dir queries and mutations
4. [`File`](#4-file) — stateful handle for streaming read/write/seek
5. [`Time`](#5-time) — `Instant` / `Duration` classes, ISO 8601, calendar arithmetic, nanosecond precision
6. [`Random`](#6-random) — seedable PRNG (uniform, gauss, shuffle, weighted_choice)
7. [`Sys`](#7-sys) — argv, exit, env
8. [`Tensor`](#8-tensor) — N-dimensional numeric tensor with a BLAS-backed lazy graph
9. [`JSON`](#9-json) — stringify / parse round-trip
10. [`Args`](#10-args) — declarative CLI argument parser (positional / option / subcommand / `--help`)
11. [`Proc`](#11-proc) — run external commands synchronously, capture stdout/stderr/exit
12. [`Isolate`](#12-isolate) — run a closure on another thread (own heap), copy values across the boundary
13. [Matchers](#13-matchers) — `assert_true` / `assert_eq` / `assert_throws` / `assert_close` family
14. [`Regex`](#14-regex) — linear-time, grapheme-aware regular expressions
15. [Design notes](#15-design-notes)
16. [Not included (yet)](#16-not-included-yet)

**Where to find what**

| Need… | Look at |
|---|---|
| Constants (π, e, inf, nan) | [§1 Math constants](#math-pi) |
| Scalar arithmetic (abs, min, max, log, exp, sqrt, floor, ceil, round) | [§1 Math](#1-math) |
| Print to stdout | `IO.puts` (with newline + quoting) / `IO.print` (raw) |
| Read a whole file | `FS.read` (throws on failure) |
| Stream a file (lines / chunks / seek) | [§4 File](#4-file) — `File.open` / `File.with` |
| Path manipulation (join, basename, dirname, stem, extension) | [§3 FS](#3-fs) |
| Stat / walk / glob / copy / rename / symlink | [§3 FS](#3-fs) |
| Directory listing / create / remove | `FS.list_dir`, `FS.mkdir`, `FS.remove` |
| `Instant` / `Duration`, ISO 8601, calendar arithmetic | [§5 Time](#5-time) |
| Random numbers | `Random.int`, `.uniform`, `.gauss`, `.shuffle`, `.weighted_choice` |
| CLI argument parsing | [§10 Args](#10-args) |
| Process info | `Sys.argv`, `Sys.exit`, `Sys.env` |
| Run an external command | [§11 Proc](#11-proc) — `Proc.run(["git", "status"])` |
| Run work on another thread (CPU parallelism) | [§12 Isolate](#12-isolate) — `Isolate.spawn(\|\| fib(40))` |
| String / Array / Object methods | [language spec §17](language.md) |
| Integer sequences (`range`, `iota`) | [language spec §18](language.md) |
| Conversion (`to_long`, `to_float`, `to_string`, `type_of`) | [language spec §18](language.md) |

---

## 1. `Math`

Numeric utilities. Integer-only routines (`pow`, `sign`, `clamp`)
preserve `Long` input; the Float-domain routines (`log`, `exp`,
`sqrt`, …) accept either `Long` or `Float` and return the shape
documented below. See [§4](language.md#4-types) and
[§7](language.md#7-expressions) of the language spec for how `Long`
and `Float` interact.

Sub-groups in this section: **constants** (`Math.pi`, `Math.e`,
`Math.inf`, `Math.nan`) — **scalar ops** (`abs`, `min`, `max`,
`log`, `exp`, `sqrt`, `floor`, `ceil`, `round`, `pow`, `sign`,
`clamp`). Integer-sequence factories `range` / `iota` are
language-core globals — see [§18](language.md#18-core-built-in-functions).

### Constants

`Math.pi`, `Math.e`, `Math.inf`, `Math.nan` are `Float` properties.
Both backends evaluate these as compile-time constants under `--jit`.

<a id="math-pi"></a>
#### `Math.pi`

`π` ≈ `3.141592653589793`.

#### `Math.e`

Euler's number, ≈ `2.718281828459045`.

#### `Math.inf`

Positive infinity (`Math.inf > 1e308 == true`). Negate with `-Math.inf`.

#### `Math.nan`

Quiet NaN. Note `Math.nan == Math.nan` is `false` per IEEE-754.

```culebra
puts(Math.pi)              # 3.141592653589793
puts(Math.e)               # 2.718281828459045
puts(Math.inf > 1e308)     # true
puts(Math.nan == Math.nan) # false
```

### Scalar operations

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

---

## 2. `IO`

Output and standard input. File reading/writing lives under `FS`
(`FS.read` / `FS.write` / `FS.exists`).

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
# doctest: skip
puts('name?')
name = IO.input()
puts("Hello, {name}")
```

`IO` is the standard-stream and console namespace. File reading and
writing live under `FS` (`FS.read` / `FS.write` / `FS.exists`).

---

## 3. `FS`

Filesystem path manipulation and directory operations. Backed by
`std::filesystem`; mutating calls throw structured `IOError`
(`{kind: "IOError", message, line, col}`) on failure so an embedder
can attribute the error to its source location.

### Path manipulation

#### `FS.join(parts...: String) -> String`

Concatenate path components with the platform separator. Zero
arguments returns `""`. Trailing separators in components are
respected — the operator behaves like `std::filesystem::path::operator/=`.

```culebra
puts(FS.join('a', 'b', 'c.txt'))      # => 'a/b/c.txt'
puts(FS.join('/usr', 'local', 'bin')) # => '/usr/local/bin'
puts(FS.join())                       # => ''
```

#### `FS.basename(path: String) -> String`

Final path component (filename + extension). Trailing separator
yields `""`.

```culebra
puts(FS.basename('a/b/c.txt'))  # => 'c.txt'
puts(FS.basename('/'))          # => ''
```

#### `FS.dirname(path: String) -> String`

Parent path. `""` for paths with no parent (`'c.txt' -> ''`).

#### `FS.extension(path: String) -> String`

File extension *including the leading dot*, or `""` for paths with
none. Dotfiles (`.hidden`) are treated as having no extension —
matches `std::filesystem::path::extension`.

```culebra
puts(FS.extension('a/b/c.txt'))  # => '.txt'
puts(FS.extension('.hidden'))    # => ''
```

#### `FS.stem(path: String) -> String`

Basename without the trailing extension.

```culebra
puts(FS.stem('a/b/c.txt'))  # => 'c'
```

### Whole-file read / write

#### `FS.read(path: String) -> String`

Read the entire file at `path` into a `String` (open + read + close in
one call). Always binary: the result is a byte string that round-trips
arbitrary content. For incremental/streaming reads use a `File` handle.
Throws `IOError` if the file is missing, unreadable, or a directory.

```culebra
# doctest: skip
contents = FS.read('data.txt')
```

#### `FS.write(path: String, content: String) -> Nil`

Write `content` to `path`, creating or overwriting it. Binary, no
newline translation. Throws `IOError` if the parent directory is
missing or the path is not writable.

```culebra
# doctest: skip
FS.write('out.txt', 'hello\n')
```

### Queries

#### `FS.exists(path: String) -> Bool`

Whether anything exists at `path`. Does not distinguish files,
directories or symlinks. An empty or invalid path returns `false`.

#### `FS.is_file(path: String) -> Bool`

True iff `path` is a regular file. Symlinks are followed.

#### `FS.is_dir(path: String) -> Bool`

True iff `path` is a directory. Symlinks are followed.

#### `FS.size(path: String) -> Long`

File size in bytes. Throws `IOError` if `path` is missing or not a
regular file.

### Directory mutations

#### `FS.list_dir(path: String) -> Array<String>`

Direct children of `path` as bare filenames (no `.` / `..`, no
prefix). Order is filesystem-defined — sort explicitly if needed.
Throws `IOError` if `path` is not a directory.

```culebra
# doctest: skip
let names = FS.list_dir('/tmp/build')
assert_true(names.contains('out.o'))
```

#### `FS.mkdir(path: String) -> Nil`

Create a directory at `path`, including any missing parents
(`mkdir -p` semantics). No-op if the directory already exists.
Throws `IOError` if the path exists as a file or creation fails.

#### `FS.remove(path: String, recursive: Bool = false) -> Nil`

Remove a file or directory. By default removes a file or *empty*
directory and throws `IOError` if the directory is non-empty. With
`recursive: true` it removes a directory tree (`rm -rf`). Throws
`IOError` if the path doesn't exist or isn't removable.

```culebra
# doctest: skip
FS.remove('/tmp/build/out.o')
FS.remove('/tmp/build', recursive: true)
```

#### `FS.rename(src: String, dst: String) -> Nil`

Rename / move `src` to `dst` (atomic within one filesystem). Throws
`IOError` on failure.

#### `FS.copy(src: String, dst: String, recursive: Bool = false) -> Nil`

Copy a file, overwriting `dst` if it exists. With `recursive: true`
copies a directory tree. Throws `IOError` on failure.

### Stat / metadata

#### `FS.stat(path: String) -> Object`

Return `{size, is_dir, is_file, is_symlink, mtime}` for `path`. `size`
is bytes (0 for non-regular files); `mtime` is seconds since the Unix
epoch; `is_symlink` reflects the link itself while the other fields
follow it. Throws `IOError` if the path doesn't exist.

```culebra
# doctest: skip
let st = FS.stat('config.toml')
puts(st.size)
```

### Recursive traversal

#### `FS.walk(path: String) -> Array<String>`

Every path under `path`, recursive, depth-first. Each entry is a full
path. Throws `IOError` if `path` isn't a directory.

#### `FS.glob(pattern: String) -> Array<String>`

Paths matching a glob `pattern`, sorted. Supports `*`, `?`, `[...]`
per segment and `**` for recursive descent. This is shell-style glob,
distinct from `Regex`.

```culebra
# doctest: skip
let sources = FS.glob('src/**/*.cul')
```

### Path resolution

#### `FS.abspath(path: String) -> String`

Absolute, normalized form of `path` (relative to the current
directory). Does not resolve symlinks.

#### `FS.realpath(path: String) -> String`

Canonical path with symlinks resolved (`weakly_canonical`; missing
trailing components are kept).

#### `FS.normpath(path: String) -> String`

Lexically normalize (collapse `.` / `..` / duplicate separators)
without touching the filesystem.

#### `FS.is_abs(path: String) -> Bool`

Whether `path` is absolute.

### Symlinks

#### `FS.symlink(target: String, link: String) -> Nil`

Create a symbolic link at `link` pointing to `target`.

#### `FS.readlink(path: String) -> String`

Read a symlink's target. Throws `IOError` if `path` isn't a symlink.

#### `FS.is_symlink(path: String) -> Bool`

Whether `path` is a symbolic link (the link itself, not its target).

---

## 4. `File`

A `File` is a **stateful handle** for streaming I/O — the complement
to `FS`'s one-shot whole-file operations (`FS.read` / `FS.write`).
Open one with `File.open` or the scoped `File.with`. All I/O is binary
(no text-mode newline translation); `String` is a byte string, so any
content round-trips.

The handle implements four method groups — **Reader** (`read` /
`lines` / `chunks`), **Writer** (`write` / `flush`), **Seekable**
(`seek` / `tell`), **Closeable** (`close`). Which are valid depends on
the open mode.

### Opening

#### `File.open(path: String, mode: String = "r") -> File`

Open `path`. `mode` is `"r"` (read), `"w"` (truncate + write), or
`"a"` (append). Throws `ValueError` for any other mode, `IOError` if
the file can't be opened.

#### `File.with(path: String, mode: String = "r", fn: Function) -> Any`

Open `path`, call `fn(handle)`, and close the handle on every exit
path (normal, `return`, or exception). Returns `fn`'s value. This is
the native equivalent of `open` + `defer { close }`, and the clearest
choice when the handle's lifetime fits one block.

```culebra
# doctest: skip
let head = File.with('big.log', 'r', fn (f) { f.read(256) })
```

### Resource safety — three ways to close

| Pattern | Use when |
|---|---|
| `File.with(p, m, fn (f) { … })` | the handle's lifetime fits one block |
| `let f = File.open(p, m); defer { f.close() }` | the handle outlives a block / mixes with other logic |
| `for line in File.open(p).lines() { … }` | streaming; the iterator closes on loop exit (incl. `break`) |

A handle never explicitly closed is closed by a GC backstop, but don't
rely on it — prefer one of the three patterns above.

### Reader methods

#### `File.read() -> String` / `File.read(n: Long) -> String`

Streaming read from the current position: `read()` returns the rest of
the file, `read(n)` at most `n` bytes (fewer at EOF). For a one-shot
whole-file read without a handle, use `FS.read(path)`.

#### `File.lines() -> Iterator<String>`

Iterate lines, each with its trailing newline stripped (`\n`, `\r\n`,
and `\r` are all recognized). The iterator owns the handle and closes
it when the loop ends or breaks.

```culebra
# doctest: skip
for line in File.open('access.log').lines() {
  if line.contains('ERROR') { puts(line) }
}
```

#### `File.chunks(n: Long) -> Iterator<String>`

Iterate fixed-size byte chunks of at most `n` bytes (the last may be
shorter). Same close-on-exit contract as `lines()`.

### Writer methods

#### `File.write(data: String) -> Nil`

Write `data` at the current position (raw bytes, no newline
translation). Throws `IOError` on a read-only handle.

#### `File.flush() -> Nil`

Flush buffered writes to the OS.

### Seekable methods

#### `File.seek(offset: Long, whence: String = "set") -> Nil`

Move the cursor. `whence` is `"set"` (from start), `"cur"` (relative),
or `"end"` (from end; use a negative `offset`).

#### `File.tell() -> Long`

Current byte offset.

### Closeable

#### `File.close() -> Nil`

Close the handle, flushing writes. Idempotent — closing twice is a
no-op. Operating on a closed handle throws `IOError`.

---

## 5. `Time`

Wall-clock + monotonic time, ISO 8601 round-trip, calendar
arithmetic. The module exposes two classes — `Instant` (a point in
time) and `Duration` (an interval) — backed internally by `i64`
nanoseconds since the Unix epoch (range ±292 years, full nanosecond
precision).

Timezone handling is **UTC + local only** (named zones like
`Asia/Tokyo` are deferred). Methods accept a kw-only `utc:` flag;
`iso` defaults to UTC (`utc: true`) because Z-suffixed ISO 8601 is
the common interop wire form, others default to local.

### Acquisition

#### `Time.now() -> Instant`

Current wall-clock time. Subject to NTP / manual clock adjustments —
use `Time.monotonic` for measuring elapsed time.

#### `Time.monotonic() -> Float`

Seconds (with sub-second precision) elapsed since the first call /
process start. Strictly non-decreasing; immune to wall-clock
changes. The primary tool for benchmarks and timeouts.

```culebra
# doctest: skip
let t0 = Time.monotonic()
do_work()
puts("elapsed: {Time.monotonic() - t0} s")
```

#### `Time.sleep(secs: Float) -> Nil`

Block the current thread for at least `secs` seconds. Negative or
zero is a no-op.

### `Instant` constructors

#### `Time.from_iso(s: String) -> Instant`

Parse an ISO 8601 timestamp. Accepted variants:

- `2026-05-20T15:30:00Z`
- `2026-05-20T15:30:00.123Z`
- `2026-05-20T15:30:00.000123456Z` (full nanosecond precision)
- `2026-05-20T15:30:00+09:00`
- `2026-05-20T15:30:00-0900`
- `2026-05-20` (date only — UTC midnight)
- `2026-05-20T15:30` (seconds omitted)

Throws `ValueError` on a malformed input.

#### `Time.from_unix(secs: Long|Float) -> Instant`

From Unix epoch seconds (Float gives sub-second precision).

#### `Time.from_parts(p: Object, utc: false) -> Instant`

Compose from a parts dict — the inverse of `Instant.parts`.
Recognised keys: `year`, `month`, `day`, `hour`, `minute`, `second`,
`nanosecond` (defaults: `month=1`, `day=1`, others 0). Extra keys
are ignored.

#### `Time.parse(s: String, fmt: String) -> Instant`

Strict strftime parse for non-ISO inputs. The format follows POSIX
`strptime`. Throws `ValueError` if `s` doesn't match `fmt`. Result
is interpreted as local time.

```culebra
Time.parse("2026/05/20 15:30:00", "%Y/%m/%d %H:%M:%S")
```

### `Instant` methods

#### `t.iso(utc: true) -> String`

Format as ISO 8601 with full nanosecond precision (fractional
component omitted when zero). UTC by default (`...Z`); pass
`utc: false` for local time with `±HH:MM` offset.

#### `t.format(fmt: String, utc: false) -> String`

Format with a strftime format string. Local time by default.

```culebra
# doctest: skip
t.format("%Y-%m-%d %H:%M:%S")             # local
t.format("%Y%m%d", utc: true)             # 20260520
```

#### `t.parts(utc: false) -> Object`

Decompose into `{year, month, day, hour, minute, second,
nanosecond, weekday, dayofyear}`. `weekday` follows ISO 8601
(`0=Mon`, `6=Sun`); `dayofyear` is 1-based (`1..366`).

```culebra
let p = Time.now().parts()
if p.hour >= 9 && p.hour < 17 { puts("business hours") }
```

#### `t.weekday(utc: false) -> Long`

Just the weekday component (0=Mon..6=Sun) — avoids the `parts()`
allocation when only the weekday is needed.

#### `t.add(years=0, months=0, days=0, hours=0, minutes=0, seconds=0, utc: false) -> Instant`

Calendar arithmetic. `years` / `months` use **clamp-to-end-of-month**
semantics: `2026-01-31 + 1 month → 2026-02-28`,
`2024-01-31 + 1 month → 2024-02-29` (leap year). Sub-day fields
compose as straightforward addition.

```culebra
let next_month   = Time.now().add(months: 1)
let next_quarter = Time.now().add(months: 3)
let next_year    = Time.now().add(years: 1)
```

#### `t.start_of(unit: String, utc: false) -> Instant`

Truncate to the start of a calendar unit. `unit` ∈ `"year"` /
`"month"` / `"day"` / `"hour"` / `"minute"`. Throws `ValueError`
on any other unit.

```culebra
# doctest: skip
let day_bucket  = t.start_of("day")
let hour_bucket = t.start_of("hour")
```

#### `t.unix() -> Float`, `t.unix_nanos() -> Long`

Unix epoch as Float seconds (lossy past ~400ns near current epoch)
or Long nanoseconds (lossless).

### `Duration` constructors

```culebra
# doctest: skip
Time.seconds(n)        # n seconds
Time.milliseconds(n)
Time.minutes(n)
Time.hours(n)
Time.days(n)
```

`n` may be Long or Float — fractional units round to the nearest
nanosecond.

### `Duration` methods

#### `d.seconds() / .milliseconds() / .minutes() / .hours() / .days() -> Float`

Express the duration in the requested unit (always Float so
fractional units round-trip).

#### `d.abs() -> Duration`

Absolute value of a (possibly negative) duration.

### Operator overloads

```culebra
# doctest: skip
let t = Time.now()
let one_hour = Time.hours(1)

t + one_hour            # Instant + Duration → Instant
t - one_hour            # Instant - Duration → Instant
t1 - t2                 # Instant - Instant → Duration

Time.minutes(1) + Time.seconds(30)   # → Duration (90s)
one_hour * 2                          # → Duration
one_hour / 2                          # → Duration
-one_hour                             # → Duration

a < b, a <= b, a == b                 # natural ordering on both types
```

---

## 6. `Random`

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

## 7. `Sys`

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
# doctest: skip
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

## 8. `Tensor`

N-dimensional numeric tensor. Builds a lazy computation graph and
launches BLAS / vDSP kernels through `Tensor.eval(...)` to materialize
values. Supported dtypes are `Float32` (default) and `Float64`.
Shapes can be supplied as variadic args or as an `[m, n]` Array.
`transpose`, `slice`, and `reshape` produce zero-copy views.

```culebra
let A = Tensor.from([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])  # [2, 3]
let B = Tensor.randn(3, 2)
let C = A.dot(B) + 1.0                # lazy: builds the graph only
Tensor.eval(C)                        # BLAS GEMM runs here
puts(C.shape())                       # [2, 2]
puts(C.to_array())                    # [[..., ...], [..., ...]]
```

### Construction (namespace functions)

#### `Tensor.zeros(...) -> Tensor` / `Tensor.ones(...)` / `Tensor.randn(...)`

The shape is variadic (`Tensor.zeros(3, 4)`) or an Array
(`Tensor.zeros([3, 4])`). The dtype is a string `"f32"` or `"f64"`
placed as the **first argument**, Julia-style:

```culebra
let a   = Tensor.zeros(3, 4)              # F32 default
let a64 = Tensor.zeros("f64", 3, 4)       # explicit
let dims = [3, 4]
let b   = Tensor.zeros(dims)              # computed shape
let r   = Tensor.randn(2, 3)              # standard normal
```

#### `Tensor.from(arr: Array) -> Tensor`

Converts a nested Culebra Array into a Tensor. Accepts 1D
(`[1.0, 2.0]`) or 2D (`[[1.0, 2.0], [3.0, 4.0]]`); stored as F32:

```culebra
let v = Tensor.from([1.0, 2.0, 3.0, 4.0])      # [4]
let m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
```

#### `Tensor.from_csv(path: String) -> Tensor`

Reads a CSV file directly into a contiguous Tensor. Always returns
**rank-2** — a single-column CSV becomes `[N, 1]` (the bias-vector
form). Skips the nested Array intermediary, which is 3-5× faster than
the `Tensor.from(load_2d(path))` pattern (measured on MNIST):

```culebra
# doctest: skip
let W1 = Tensor.from_csv("W1.csv")    # [30, 784]
let b1 = Tensor.from_csv("b1.csv")    # [30, 1]
let X  = Tensor.from_csv("X.csv")     # [N, 784]
```

#### `Tensor.eval(t1, t2, ...) -> Nil`

Takes a variable number of Tensor arguments and evaluates the
dependency graph in topological order. Shared subexpressions are
computed once. Call this **at least once per mini-batch boundary** in
a training loop — otherwise the graph accumulates and memory grows
unbounded.

```culebra
# doctest: skip
W2 = W2 - d2.dot(a1.transpose()) * lr
b2 = b2 - d2.sum(1).reshape([N_OUT, 1]) * lr
W1 = W1 - d1.dot(xb.transpose()) * lr
b1 = b1 - d1.sum(1).reshape([N_HID, 1]) * lr
Tensor.eval(W1, b1, W2, b2)              # evaluate all four in one pass
```

### Activation functions

Instance methods on a Tensor. A user class may still define its own
`relu` / `sigmoid` / `softmax` (e.g. microGPT's `Value.relu()`) — method
lookup gives the class method priority over the builtin, so there is no
clash.

```culebra
# doctest: skip
let h = z.sigmoid()        # 1/(1+exp(-z)) elementwise
let r = x.relu()           # max(0, x)
let p = logits.softmax()   # over the last axis, online-stable
```

### Tensor methods

Shape ops, linear algebra, and reductions use method syntax:

| Method | Returns | Description |
|---|---|---|
| `.shape() -> Array` | Array of Long | shape as an Array |
| `.dot(other: Tensor) -> Tensor` | lazy | matrix product; both operands rank-2 |
| `.linear_sigmoid(x, b) -> Tensor` | lazy | fused `sigmoid(self @ x + b)` |
| `.pow(exp) -> Tensor` | lazy | elementwise power; `exp` is Tensor or scalar |
| `.transpose() -> Tensor` | view | reverse all axes (matrix transpose for rank-2) |
| `.slice(start, end) -> Tensor` | view | take axis 0 in `[start, end)` |
| `.reshape(dims: Array) -> Tensor` | view | contiguous input only; new shape |
| `.sum() -> Float` | scalar | sum of all elements (forces eval) |
| `.sum(axis: Long) -> Tensor` | lazy | reduce one axis |
| `.mean() / .mean(axis)` | Float / Tensor | likewise |
| `.max() / .max(axis)` | Float / Tensor | likewise |
| `.argmax(axis: Long) -> Tensor` | lazy | reduce one axis to indices stored as Float |
| `.to_array() -> Array` | eager | convert to a Culebra Array (forces eval) |

### Operator overloading

`+ - * /` are broadcasting elementwise (numpy / silarray rules).
Scalars mix automatically:

```culebra
let M = Tensor.ones(3, 4)
let v = Tensor.ones(4)            # broadcasts to [3, 4]
let r = Tensor.ones(3, 1)         # broadcasts to [3, 4]
Tensor.eval(M + v, M + r, M + 1.0, M * 2.0)
```

The `@` operator is not implemented (use `.dot()`).

### Compound assignment (`+=` `-=` `*=` `/=` `**=`) and in-place writes

Compound assignment writes back into the LHS Tensor's buffer (no
fresh Tensor is allocated) when **the LHS owns its buffer and the
shape matches the RHS broadcast result**. Views, unevaluated graph
nodes, and shape mismatches automatically fall back to the ordinary
path (a fresh Tensor).

```culebra
# doctest: skip
mut W = Tensor.randn('f32', 1024, 256)
let alias = W
W -= grad * lr     # writes directly into W's buffer
Tensor.eval(alias) # alias.to_array() sees the updated values
```

The SGD-style update `W = W - grad * lr` allocates a W-sized buffer
every step. `-=` removes that allocation; the gap widens for larger
weights and longer loops (measured: 5000 steps on a 1024×256 f32
weight, plain `=` 5.5 s → `-=` 3.6 s).

Supported ops: `+= -= *= /= **=` (`%=` and `@=` are not — `%` has no
defined Tensor semantics, and `@` changes the output shape so
in-place is unsafe).

### dtype / shape constraints

- dtype is F32 or F64 only. Binops and `dot` require matching dtypes
  (no implicit promotion).
- `.dot()` is rank-2 only. Batched 3D+ matmul is future work.
- `.reshape()` requires contiguous input (a post-`transpose` reshape
  must materialize first — currently go through
  `Tensor.from((...).to_array())` explicitly).
- `.softmax()` also requires contiguous input.

### Backends

Phase 1 is **CPU only**.

- macOS: Accelerate framework (`cblas_sgemm/dgemm`, scalar fallback
  for `sigmoid`).
- Linux: OpenBLAS (`find_package(BLAS)`).

Future Metal / CUDA support will be added through the dispatch table
in `tensor_backend.h`. The API stays the same; the silarray-style
globals `use_cpu()` / `use_metal()` / `use_cuda()` will toggle the
target.

---

## 9. `JSON`

Round-trip between Culebra values and JSON text. Both backends ship
the same surface.

### `JSON.stringify(v, indent=0, sort_keys=false, lines=false) -> String`

Serialize `v` to a JSON string.

* `indent > 0` pretty-prints with that many spaces per nesting level
  (newline after each comma, `": "` separator). `indent <= 0` is the
  compact form.
* `sort_keys=true` walks `Object` keys alphabetically instead of
  insertion order — useful for deterministic diff / hash output.
* `lines=true` emits **JSON Lines**: each element of an `Array` /
  `Tuple` / `Set` becomes its own compact line, terminated by `\n`.
  An empty collection yields the empty string. Incompatible with
  `indent > 0` and with non-list receivers (`TypeError` in either
  case).

For backwards compatibility, a positional 2nd argument is still
accepted as `indent`: `JSON.stringify(v, 2)` is `JSON.stringify(v,
indent: 2)`.

Supported value types:

| Culebra            | JSON                            |
|--------------------|---------------------------------|
| `Nil`              | `null`                          |
| `Bool`             | `true` / `false`                |
| `Long`, `Float`    | number (non-finite Float raises `ValueError`) |
| `String`           | quoted string with `\n`, `\t`, `\r`, `\"`, `\\`, `\u00xx` escapes |
| `Array`            | JSON array                      |
| `Tuple`            | JSON array (same shape as `Array`) |
| `Set`              | JSON array, members in insertion order |
| `Object` (String keys only) | JSON object, keys in insertion order |

`Function`, `Tensor`, and Objects carrying non-String keys are not
serializable — `stringify` throws `TypeError` for these.

### `JSON.parse(s, lines=false, number_mode='auto') -> Any`

Parse a JSON string into a Culebra value.

* `number_mode='auto'` (default): integers (no decimal point or
  exponent) read as `Long`; everything else as `Float`.
* `number_mode='float'` reads every number as `Float` — useful for
  round-trip safety when the producer treats numbers uniformly.
* `lines=true` parses **JSON Lines**: split `s` on `\n`, parse each
  non-empty line, return an `Array` of the per-line values.

Malformed input raises `ValueError` and the structured Error Object
exposes the JSON-internal position via `e.line` / `e.col` (both
1-based, pointing at the offending character):

```culebra
let r = try { JSON.parse('{"a": ,}'); nil } catch e { e }
puts(r.message)           # JSON.parse: expected value at 1:7.
puts("{r.line}:{r.col}")  # 1:7
```

Examples:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
puts(JSON.stringify(v))                              # compact
puts(JSON.stringify(v, indent: 2))                   # pretty
puts(JSON.stringify(v, sort_keys: true))             # alphabetical
puts(JSON.stringify([1, 2, 3], lines: true))         # JSONL
let back = JSON.parse(JSON.stringify(v))
puts(back.name)                                      # alice
let arr = JSON.parse("1\n2\n3\n", lines: true)
puts(arr)                                            # [1, 2, 3]
```

JIT note: built-in `JSON.{stringify, parse}` accept kwargs on both
backends, including literal `**{key: val, ...}` splats AND dynamic
`**variable` splats. Literal splats flatten at compile time (no
runtime overhead); dynamic splats route through a per-built-in
runtime adapter that enumerates the splat Object's keys on the
fly — same algorithm interp uses.

---

## 10. `Args`

Declarative CLI argument parser. The spec is a culebra `Object`
listing positionals, options, and subcommands; `Args.parse` returns
an `Object` whose fields match the spec, prints help on `--help`,
and exits with status 2 on parse errors. For programmatic control,
`Args.try_parse` raises an `{kind: "ArgParseError", message}` or
`{kind: "ArgParseHelp", help}` value instead.

### `Args.parse(argv: Array<String>, spec: Object) -> Object`

Parse `argv` (typically `Sys.argv`) against `spec`. On `--help` or
`-h`, prints help to stdout and `Sys.exit(0)`. On any parse error,
prints `error: <message>` to stderr and `Sys.exit(2)`.

### `Args.try_parse(argv, spec) -> Object`

Same engine, but raises on error / help instead of exiting. Use
this from tests or when wrapping into a custom UX.

### `Args.help(spec: Object) -> String`

Produce the help text the parser would print on `--help`, without
parsing or exiting. Useful for embedding into a wider message.

### Spec format

Each argument is an `Object` with these fields:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | `String` | (required) | field name in the parsed result |
| `type` | `String` | `"String"` | `"String"` / `"Long"` / `"Float"` / `"Bool"` |
| `short` | `String` | (none) | short flag letter (e.g. `"v"` → `-v`). Marks it as an option. |
| `default` | `Any` | (none) | default value. Marks it as optional. |
| `doc` | `String` | `""` | help-text description |
| `repeated` | `Bool` | `false` | collect multiple values into an `Array` |

Argument **type** of `Bool` means the option is a **flag** that
consumes no value (`--verbose` / `-v`). All other types consume the
next token (`--count 5` / `--count=5`).

An argument is **positional** unless it has `short` or `default`.
Positional args are matched in spec order; a positional with a
`default` is optional.

### Example

```culebra
# doctest: skip
let spec = {
  name: "wc-lite",
  doc:  "count lines and words",
  args: [
    {name: "input",   type: "String", doc: "input file"},
    {name: "lines",   short: "l", type: "Bool", default: false, doc: "count lines"},
    {name: "words",   short: "w", type: "Bool", default: false, doc: "count words"},
    {name: "encoding",            type: "String", default: "utf-8"}
  ]
}

let args = Args.parse(Sys.argv, spec)
puts(args.input)            # String
if args.lines { puts("lines: ...") }
if args.words { puts("words: ...") }
puts("encoding: {args.encoding}")
```

```
$ ./wc-lite -l file.txt
lines: ...
encoding: utf-8

$ ./wc-lite --help
wc-lite - count lines and words

Usage: wc-lite [options] <input>

Arguments:
  input    input file

Options:
  -l, --lines        count lines
  -w, --words        count words
      --encoding
  -h, --help         show this help and exit
```

### Subcommands

`spec.subcommands` is an `Array<Object>` where each element is a
sub-spec (same shape as the top-level spec, minus its own
`subcommands` typically). When present, the first positional token
selects a subcommand; the parsed result has a `subcommand` field
naming the selected command, and the rest of the spec's args are
parsed as that subcommand's:

```culebra
# doctest: skip
let spec = {
  name: "git-lite",
  subcommands: [
    {name: "add",    args: [{name: "files",   type: "String", repeated: true}]},
    {name: "commit", args: [{name: "message", short: "m", type: "String"}]}
  ]
}

match Args.parse(Sys.argv, spec).subcommand {
  "add"    => stage_files(args.files),
  "commit" => commit_with_message(args.message)
}
```

### Error handling

`Args.parse` exits on any error. `Args.try_parse` instead throws:

```culebra
let r = try { Args.try_parse(["--bogus"], spec) } catch e { e }
# r == {kind: "ArgParseError", message: "unknown option '--bogus'"}
```

The `kind` of a thrown value is one of:

| `kind` | Meaning | Extra fields |
|---|---|---|
| `ArgParseError` | parse failure (unknown opt, type mismatch, missing required, etc.) | `message` |
| `ArgParseHelp` | user passed `--help` / `-h` | `help` (the help-text string) |

---

## 11. `Proc`

Run an external command synchronously (blocking) and capture its
output. The command is an `Array<String>` — `cmd[0]` is the executable
(PATH-resolved) and the rest are arguments. There is no shell, so no
quoting or injection concerns: `["git", "commit", "-m", msg]` passes
`msg` verbatim however it's spelled.

### `Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false) -> Object`

Runs `cmd` to completion and returns a result Object:

| field | type | meaning |
|---|---|---|
| `code` | `Long` | exit status on normal exit; `-1` when killed by a signal |
| `stdout` | `String` | everything the command wrote to stdout (captured whole) |
| `stderr` | `String` | everything it wrote to stderr |
| `ok` | `Bool` | `true` iff `code == 0` and `signal == nil` |
| `signal` | `String?` | signal name (`"SIGTERM"`, `"SIGKILL"`, …) if killed, else `nil` |
| `error` | `String?` | spawn-failure message; `nil` whenever the command actually ran. Only `Proc.all` sets this (its allSettled errors) — for `Proc.run` it is always `nil`, since a spawn failure throws instead. |
| `timed_out` | `Bool` | `true` if the command was killed for exceeding its `timeout`; `false` otherwise. |

Keyword arguments:

- `cwd: String` — working directory for the child (default: inherit the parent's).
- `env: Object` — environment variables, merged onto the parent's so
  `PATH` and friends survive (default: inherit unchanged). Values must
  be `String`.
- `stdin: String` — bytes written to the child's standard input, which
  is then closed (default: empty).
- `check: Bool` — when `true`, a non-zero exit, signal death, or timeout
  throws `ProcessError` instead of returning a `{ok: false}` result (default: `false`).
- `timeout: Long` — milliseconds; if the command runs longer it is killed
  (`SIGTERM`, then `SIGKILL` after a short grace) and the result has
  `ok: false` and `timed_out: true` (default: `0` = no limit). Only the
  command itself is killed, not any grandchildren it spawned (matching
  Python/Node defaults). A process that closes stdout/stderr but keeps
  running may not be reached by the timeout.

A **non-zero exit** or **signal death** is a normal result, not an error —
branch on `ok` / `code` / `signal`. Only a **spawn failure** (e.g. the
executable doesn't exist) or a failure under `check: true` throws
`ProcessError`. Passing a non-Array, a non-String element, or an empty
command throws `TypeError` / `ValueError`.

```culebra
# doctest: skip
let r = Proc.run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
if r.ok {
  IO.puts("on branch " + r.stdout.trim())
} else {
  IO.print(r.stderr)
}

# Feed stdin and read the transformed output.
let up = Proc.run(["tr", "a-z", "A-Z"], stdin: "hello\n")
assert_eq(up.stdout, "HELLO\n")

# Run in a directory with an extra env var; throw on failure.
Proc.run(["make", "install"], cwd: "/src/app", env: {PREFIX: "/usr/local"}, check: true)
```

Output is buffered in full, so a command that emits gigabytes will use
that much memory. stdout and stderr are drained concurrently, so a
command that fills both will not deadlock.

### `Proc.all(commands: Array<Array<String>>, limit: Long = <cpus>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0) -> Array<Object>`

Runs many commands in parallel and returns their result Objects in input
order. Each command is its own `Array<String>` (same form as `Proc.run`'s
first argument). At most `limit` run at once — `limit` defaults to the
online CPU count; pass a smaller number to throttle, a larger one to widen.
`timeout` (ms, `0` = none) applies per command, measured from each command's
own start, and flags the result with `timed_out: true` when it fires.

By default this is **allSettled**: one command failing never aborts the
others. A command that ran and exited non-zero is `{ok: false, code: N,
error: nil}`; a command that *couldn't start* (e.g. the executable doesn't
exist) is `{ok: false, error: "<message>"}` — neither throws. An empty list
returns `[]`.

With **`fail_fast: true`** the call instead stops at the first failure
(non-zero exit, signal, timeout, or spawn failure), `SIGKILL`s the still-
running commands, and throws `ProcessError` naming the offending command —
the `Promise.all` shape (as opposed to the default `Promise.allSettled`). If
every command succeeds it returns the result array as usual.

**`retries`** re-runs a command that failed, up to that many extra times; the
reported result is the final attempt's. A retry slots back into the `limit`
pool as it becomes free. When combined with `fail_fast`, a command only counts
as failed once its retries are exhausted.

```culebra
# doctest: skip
let results = Proc.all([
  ["git", "fetch", "origin"],
  ["npm", "test"],
  ["cargo", "build"],
], limit: 2)
for r in results {
  if !r.ok { IO.print(r.error ?? r.stderr) }
}
```

### `Proc.race(commands: Array<Array<String>>) -> Object`

Starts every command, returns the result Object of the **first to finish**,
and sends `SIGKILL` to the rest (then reaps them). Useful for racing
redundant providers or "first mirror to respond wins". An empty list throws
`ValueError`.

```culebra
# doctest: skip
let fastest = Proc.race([
  ["curl", "-s", "https://mirror-a.example/file"],
  ["curl", "-s", "https://mirror-b.example/file"],
])
IO.print(fastest.stdout)
```

### `Proc.spawn(cmd: Array<String>, cwd=nil, env=nil, stdin="") -> handle`

Starts a command and returns immediately with a **live handle**, without
waiting for it. The handle has three methods:

| method | returns | meaning |
|---|---|---|
| `h.wait()` | result Object | block until the child exits, draining its output |
| `h.poll()` | result Object or `nil` | the result if it has exited, else `nil` (non-blocking) |
| `h.kill(sig = 15)` | `nil` | send a signal (`SIGTERM` by default); the next `wait`/`poll` reaps |

`wait()` / `poll()` are idempotent — once the child has been reaped, both
return the same cached result Object (the usual `{code, stdout, stderr, ok,
signal, error, timed_out}`). A spawn failure throws `ProcessError`, like
`Proc.run`. A handle that is dropped without ever being waited on is reaped by
the GC (the child is `SIGKILL`ed), so it won't linger as a zombie — but
explicitly `wait()`ing or `kill()`ing is clearer. As with the other verbs, only
the direct child is signalled, not any grandchildren.

```culebra
# doctest: skip
let server = Proc.spawn(["python", "-m", "http.server", "8000"])
# ... do work against the server ...
server.kill()                 # SIGTERM
let r = server.wait()
IO.puts("server exited via " + (r.signal ?? to_string(r.code)))

# Run something and poll for completion without blocking.
let job = Proc.spawn(["make", "-j4"])
while job.poll() == nil {
  IO.print(".")               # ...other work...
}
```

`stdin` is fed once at spawn time and then closed. Incremental streaming I/O
and pipelines (`a | b`) are planned.

---

## 12. `Isolate`

Run a closure on its own OS thread with its own garbage-collected heap, for
real CPU parallelism. Isolates share no mutable memory: a value crosses the
boundary by being **copied**, so two isolates can never race on the same
object. This is the thread-level counterpart to [§11 `Proc`](#11-proc) (which
parallelizes across processes).

> `Isolate.spawn`, `Channel`, and `Parallel` all work under both the interpreter
> and `--jit` (a closure crosses as a shared code reference — the AST in the
> interpreter, the compiled `fn_ptr` in the JIT — plus copied captures, and runs
> on the child's own heap).

### `Isolate.spawn(fn, *args) -> handle`

Runs `fn` on another thread and returns immediately with a live handle. Any
positional `args` are passed to `fn`.

```culebra
# doctest: skip
let h = Isolate.spawn(|| 1 + 2)
h.join()                       # => 3

let h2 = Isolate.spawn(|n| n * n, 7)
h2.join()                      # => 49
```

The handle has:

| method | returns | meaning |
|---|---|---|
| `h.join()` | the closure's return value | block until the isolate finishes, then hand back its (copied) result |
| `h.poll()` | the result or `nil` | the result if finished, else `nil` (non-blocking) |

If the closure throws, the exception is re-raised on the thread that calls
`join()`, with its original kind preserved. A handle dropped without `join()` is
joined by the GC, so no thread is left dangling.

### Sendable: what can cross the boundary

The closure and its arguments — and the value it returns — must be **Sendable**.
A violation throws `SendError` at the `spawn` site (never a silent copy):

| Sendable | Not Sendable |
|---|---|
| numbers, `String`, `Bool`, `nil` | a native handle (`Proc` / `File` / isolate handle) |
| `Array` / `Object` / `Set` / `Tuple` of Sendable values | a `Tensor` (share via a buffer instead — planned) |
| `enum` / data-class instances | a closure that captures a `mut` variable |
| a closure capturing only Sendable values | a value that refers to itself (a cycle) |
| a free function (`fn name(...)`), captured by reference | |

Because captures are copied, a closure that mutates a captured collection
mutates **its own copy** — the parent's value is untouched:

```culebra
# doctest: skip
let xs = [1, 2, 3]
let h = Isolate.spawn(fn () { xs.push(99); xs.size() })
h.join()                       # => 4   (the isolate's copy)
xs                             # => [1, 2, 3]   (parent unchanged)
```

A `mut` capture is rejected rather than silently snapshotted — pass the value
as an argument instead:

```culebra
# doctest: skip
mut total = 0
Isolate.spawn(|| total)        # SendError: captures the mutable variable 'total'
Isolate.spawn(|t| t, total)    # ok — passed by value
```

### Parallelism cap

Live isolates are capped (default: the machine's core count; override with the
`CULEBRA_ISOLATE_LIMIT` environment variable). A spawn beyond the cap runs
**synchronously on the current thread** instead of starting a new one — so
recursive parallelism can't explode into thousands of threads. `join()` still
returns the same result; only the timing differs.

```culebra
# doctest: skip
# Parallel map: split work across isolates, then collect.
let parts = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
mut handles = []
for p in parts { handles.push(Isolate.spawn(|| p.reduce(0, |a, b| a + b))) }
mut total = 0
for h in handles { total = total + h.join() }
total                          # => 45
```

### Cancellation

An isolate is cooperatively cancellable. Dropping its handle without `join()`
(or letting the GC collect it) signals the isolate to stop; it unwinds at its
next statement or blocking-channel boundary, so a runaway or idle isolate never
hangs the program.

### Channels — `Channel.new(cap = 1) -> (tx, rx)`

A channel is a bounded, blocking queue for passing values between isolates. It
returns a **(tx, rx)** pair (Rust `mpsc` style): `tx.send(v)` enqueues, `rx`
yields values. A channel endpoint is the one exception to the Sendable rules —
it is shared (by reference), so a closure can capture `tx`/`rx` and carry it
into a spawned isolate. Values still cross by copy; only the channel itself is
shared.

```culebra
# doctest: skip
let (tx, rx) = Channel.new(10)        # bounded; capacity 10
let prod = Isolate.spawn(fn () {
  for line in source() { tx.send(parse(line)) }
  tx.drop()                            # release this sender
})
tx.drop()                              # release the parent's sender too
for record in rx { process(record) }   # ends when every tx is dropped
prod.join()
```

| method | on | meaning |
|---|---|---|
| `tx.send(v)` | tx | enqueue `v` (blocks while the buffer is full); throws `ChannelError` if the channel is closed |
| `tx.clone()` / `rx.clone()` | both | another endpoint over the same channel (multi-producer / multi-consumer) |
| `tx.drop()` / `rx.drop()` | both | release this endpoint |
| `rx.recv()` | rx | block for one value; returns `nil` once the channel is closed and drained |
| `for v in rx { ... }` | rx | drain until closed (the clean end-of-stream form) |

**Auto-close is the deadlock-safety net.** The active senders are counted;
when the **last `tx` is dropped** (a producer isolate finishing, normally or by
throwing) the channel closes and any `for v in rx` ends. So a producer that
crashes won't hang its consumer — surface the failure by `join()`-ing the
producer. Note the multi-producer trap: every `tx` (including the parent's
original) must be dropped for the channel to close — drop the ones you don't
keep.

Bounded to capacity >= 1 (rendezvous channels are planned next).

### Parallel — `Parallel.map` / `each` / `map_settled` / `race`

The high-level form: apply a function to every element of an array across a pool
of isolates, without managing handles yourself. `fn` and every element must be
Sendable (the same rules as `Isolate.spawn`).

```culebra
# doctest: skip
Parallel.map([1, 2, 3, 4], |x| x * x)         # => [1, 4, 9, 16]  (input order)
Parallel.map(urls, |u| fetch(u), limit: 8)    # at most 8 live isolates
Parallel.each(jobs, |j| process(j))           # side effects; returns nil
Parallel.map_settled(urls, |u| fetch(u))      # => [{ok, value, error}, ...]
Parallel.race(mirrors, |m| download(m))       # first success wins
```

| call | returns | notes |
|---|---|---|
| `Parallel.map(items, fn, limit = <cores>)` | `Array` | one result per element, **in input order**; fail-fast |
| `Parallel.each(items, fn, limit = <cores>)` | `nil` | for side effects; no results collected; fail-fast |
| `Parallel.map_settled(items, fn, limit = <cores>)` | `Array` | one `Result` per element, in input order; **never fail-fast** |
| `Parallel.race(items, fn, limit = <cores>)` | `Any` | the **first** element to succeed; cancels the rest |

`limit` caps how many isolates run at once (default: the core count); the
elements are pulled from one shared queue, so it is `limit` isolates total, not
one per element.

**`map` / `each` are fail-fast:** the first element to throw stops the rest and
is re-raised as `ParallelError` naming the element index — e.g. `Parallel.map:
element[2] failed: ...`.

**`map_settled` never fail-fasts:** every element yields a `Result` Object
`{ok: Bool, value: Any, error: String?}` (the same shape as `Proc.all`), so one
failure doesn't lose the others' work — `r.ok ? r.value : r.error`.

**`race` returns the first success** and cancels the remaining elements; if
*every* element throws it raises `ParallelError`, and on an empty array it raises
`ParallelError` (there is no result to return).

(An `on_progress` callback and `map_reduce` are planned.)

---

## 13. Matchers

Assertion matchers for tests and runtime invariant checks. All ten
matchers are global names bound on every environment (no `import`
required). Failure throws a Culebra Object with the conventional
`{kind: "AssertionError", message: ...}` shape; user code can
`try/catch` it.

There is no `assert` keyword or builtin — instead, use a specific
matcher for each kind of check. For production invariants, write the
`if`/`throw` directly:

```culebra
# doctest: skip
if (!cond) {
  throw {kind: "AssertionError", message: "invariant violated"}
}
```

### Truthiness matchers

* **`assert_true(x: Bool) -> Nil`** — pass if `x` is truthy. Throws
  `AssertionError` with message `assert_true failed:\n  value: {x}`
  on failure. `x` must be `Bool`, `Long`, or `Float`; other types
  raise `TypeError` (culebra has no Python-style truthiness — empty
  strings / arrays are not falsy).
* **`assert_false(x: Bool) -> Nil`** — mirror of `assert_true`.

### Comparison matchers

Each comparison matcher dispatches through the same operator as the
plain expression — `assert_eq(a, b)` is equivalent to `a == b`, so
`__eq__` / `__lt__` / `__le__` on class instances are honored.
Failure message names both operands via `to_string` (respecting any
user `__str__`).

* **`assert_eq(a, b) -> Nil`** — `a == b`.
* **`assert_ne(a, b) -> Nil`** — `a != b`.
* **`assert_lt(a, b) -> Nil`** — `a < b`.
* **`assert_le(a, b) -> Nil`** — `a <= b`.
* **`assert_gt(a, b) -> Nil`** — `a > b`.
* **`assert_ge(a, b) -> Nil`** — `a >= b`.

```culebra
assert_eq(1 + 1, 2)                                # passes silently

let r = try { assert_eq("foo", "bar"); nil } catch e { e }
puts(r.kind)         # => 'AssertionError'
puts(r.message)
# => |
# 'assert_eq failed:
#   left:  foo
#   right: bar'
```

### `assert_throws(kind: String, f: Function) -> Nil`

Invoke 0-arg `f()` and assert it throws an error whose `kind` matches
`kind`. Built-in errors (`ZeroDivisionError`, `TypeError`, etc.)
expose `e.kind` directly; user `throw {kind: "X", ...}` matches the
same way. `f` must take 0 parameters — otherwise `ArityError`.

```culebra
assert_throws("ZeroDivisionError", fn() { let _ = 1 / 0 })
assert_throws("MyError", fn() {
  throw {kind: "MyError", message: "boom"}
})
```

### `assert_close(a: Float, b: Float, tol: Float) -> Nil`

Pass if `|a - b| <= tol`. NaN in `a`, `b`, or `tol` deliberately
fails (a naive `diff > tol` check would silently pass NaN). Use this
in place of `assert_eq` for floating-point comparisons.

```culebra
assert_close(3.14, 3.1415, 0.01)
```

### Implementation note

The matcher family is defined in culebra source (not native C++) and
bound via the lazy module mechanism so that all three backends
(interp / JIT / AOT) share one implementation. Operator dispatch
(`==`, `<` etc.) inside the matchers is the same operator dispatch
each backend already implements, so `__eq__` / `__lt__` semantics
agree without any matcher-specific drift logic.

---

## 14. `Regex`

Linear-time, grapheme-aware regular expressions (engine: `include/regexlib.h`).
Patterns match by Unicode **extended grapheme cluster**, not code point — `.`
consumes one user-perceived character (so `/./` matches `🇯🇵` as a single
element). Matching runs in **linear time** (Thompson NFA / Pike VM with a lazy
DFA fast path), so catastrophic backtracking cannot occur and there are no
backreferences. Offsets are **byte offsets** (Go-style), always on grapheme
boundaries.

A `Regex` is **compiled once and reused** (`Regex.compile` — the compiled
program is the expensive part), then queried with methods:

**Write patterns as single-quoted raw strings** (`'\d+'`, not `"\\d+"`): single
quotes do no escape processing and no `{...}` interpolation, so `\d` and `{n}`
pass through verbatim (the Python `r"..."` idiom). For a pattern that also
contains an apostrophe (e.g. a tokenizer's `'s`/`'t`), use a backtick raw
string `` `...` `` — also raw, but it may hold `'`, `"`, and `{`. Flags are
either passed to `compile` as a string (`Regex.compile('hello', "i")`) or inline
in the pattern: `(?i)` case-insensitive, `(?m)` multiline, `(?s)` dotall.

| Constructor / static | Result |
| --- | --- |
| `Regex.compile(pat)` | `Regex` — compile (reused); bad pattern raises |
| `Regex.compile(pat, flags)` | `Regex` — `flags` a string of `"i"` / `"m"` / `"s"` |
| `Regex.escape(s)` | `String` — backslash-quote every metacharacter so `s` matches literally |

| Method | Result |
| --- | --- |
| `re.test(s)` | `Bool` — does the pattern match anywhere in `s` |
| `re.find(s)` | `Match` or `nil` — leftmost match |
| `re.match(s)` | `Match` or `nil` — match anchored at the start |
| `re.find_all(s)` | `[Match]` — all non-overlapping matches |
| `re.find_all_str(s)` | `[String]` — just the matched texts (no `Match` objects; ~12× faster on match-dense input) |
| `re.find_all_index(s)` | `[Int]` — flat byte spans `[s0, e0, s1, e1, …]` (positions only, one allocation total) |
| `re.count(s)` | `Int` — number of non-overlapping matches (no objects allocated) |
| `re.find_iter(s)` | `Iterator<Match>` — lazy; supports early exit (`.take(n)`) |
| `re.replace_all(s, repl)` | `String` — `repl` is a template (`$1` / `$<name>` / `$$`) **or** a `fn (Match) -> String` |
| `re.split(s)` | `[String]` — split `s` on matches |

A `Match` is a data object (and `nil` means no match):

| Field | Meaning |
| --- | --- |
| `m.value` | the whole-match text (`String`) |
| `m.start`, `m.end` | byte offsets |
| `m.groups` | `[Group \| nil]`; `groups[0]` is the whole match |
| `m.named` | `{name: Group}` — named captures |

`Group` has `.value`, `.start`, `.end`. An invalid pattern raises `RegexError`.

```culebra
let d = Regex.compile('\d+')
d.test("abc 123")                                // => true
Regex.compile('\w+').find("  hello world").value // => "hello"
d.find("no digits")                              // => nil
d.find_all("a1 b22 c333").size()                 // => 3

let m = Regex.compile('(\d{4})-(\d{2})').find("2026-05")
m.groups[1].value                                // => "2026"
m.named["..."]                                   // named via (?<name>...)

d.replace_all("a1 b22 c333", "#")                // => "a# b# c#"
Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1') // => "y.x"
d.replace_all("a1 b22", fn (m) { "<{m.value}>" })// => "a<1> b<22>" (callback)
Regex.compile('\s+').split("the quick  brown")   // => ["the", "quick", "brown"]
Regex.compile('hello', "i").test("HELLO world")  // => true (flag arg)
d.find("xyz")?.value ?? "none"                   // composes with ?. / ??

for m in d.find_iter("a1 b22") { ... }           // lazy; stop early any time
d.find_iter("1 2 3").take(2).collect().size()    // => 2 (no full scan)
Regex.escape("a.b(c)")                           // => `a\.b\(c\)` (literal match)
```

The supported syntax (literal / `.` / character classes / `* + ? {n,m}` greedy
and lazy / `|` / capturing and named groups / `\d \w \s \b` / lookahead /
variable-length lookbehind / `\p{…}` Unicode properties) and the full matching
model and resource limits are documented in `docs/regexlib.md`.

---

## 15. Design notes

### Namespace-first, CLI-aliased globals

The library adds **no global names beyond the matcher family**:
everything else lives under `Math`, `IO`, `Random`, or `Sys`. This
keeps `culebra::environment()` free of surprises for embedders who
use Culebra as a scripting engine inside a host application.

For CLI scripting, however, `puts` / `print` are so pervasive that
writing `IO.puts` everywhere adds friction. The CLI binary
(`src/main.cc`) installs them as globals right after constructing
the environment — pointing to the same function values that live
under `IO`, so there is no duplication. V8 takes an analogous
approach: the engine provides no `print`, and the `d8` shell
installs one.

### Namespaces as first-class values

Every stdlib namespace (`Math`, `IO`, `FS`, `Random`, `Sys`,
`Tensor`, `JSON`) is an `Object`. You can bind it to a name, pass
it as an argument, or store it in a collection, and method calls
on that binding observe the same semantics as direct calls:

```culebra
let io = IO
io.puts("hello")              # same as IO.puts("hello")

fn run_with(ns, x) { ns.puts(x) }
run_with(IO, "via parameter")
```

Both backends honor this. The JIT/AOT slow path goes through a
runtime dispatcher (`stdlib_jit.h::kNsMethods`) while the syntactic
fast path (`IO.puts(x)` directly) keeps its inlined IR emission.

### Free function vs method

Free functions (in namespaces) are used when the operation either
constructs a value from nothing (`iota`, `IO.input`) or applies
uniformly to multiple types (`type_of`, `to_string`). Operations
that are *about* a specific type use method syntax — see §17 of the
language spec for String/Array/Object methods.

### Error-by-throw versus `nil` returns

The library prefers throwing on unrecoverable type errors
(`to_long('abc')`, `FS.read(...)` on a missing file) and returning
sentinel values for "found or not" predicates (`IO.input()` returns
`''` on EOF). This keeps hot paths simple without requiring a
`try`/`catch` mechanism.

---

## 16. Not included (yet)

### Trigonometry

`Math.sin` / `Math.cos` / `Math.tan` / `Math.atan2` are not yet
exposed. Random drawing and the core transcendentals (`log`, `exp`,
`sqrt`) are available; trig entries can be added when a concrete use
case lands.

### Date, time

Deferred. Scripts that need these today can call out through
`FS.read` / `FS.write` with a helper process.

### Collections beyond `Array`/`Object`

No `Set`, `Queue`, `Tuple`, etc. Use `Array` and `Object` for now.

---

See also: [`docs/language.md`](language.md) for the language
specification.
