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
7. [`Sys`](#7-sys) — argv, exit, env, executable; `GC` heap introspection lives here too
8. [`Tensor`](#8-tensor) — N-dimensional numeric tensor with a BLAS-backed lazy graph
9. [`JSON`](#9-json) — stringify / parse round-trip
10. [`Args`](#10-args) — declarative CLI argument parser (positional / option / subcommand / `--help`)
11. [`Proc`](#11-proc) — run external commands synchronously, capture stdout/stderr/exit
12. [`Isolate`](#12-isolate) — run a closure on another thread (own heap), copy values across the boundary; `Channel`, `Parallel`, `Signal` (route Ctrl+C to a channel), `SharedBuffer` (zero-copy shared fixed-layout data), and `Shared` (immutable values shared by reference) live here too
13. [Matchers](#13-matchers) — `assert_true` / `assert_eq` / `assert_throws` / `assert_close` family
14. [`Regex`](#14-regex) — linear-time, grapheme-aware regular expressions
15. [`Http`](#15-http) — synchronous HTTP/HTTPS client (get/post/put/delete/head/request)
16. [`Encoding`](#16-encoding) — text codecs by scheme (`Encoding.html`, `Encoding.base64`, `Encoding.hex`, `Encoding.url`)
17. [`Compress`](#17-compress) — gzip (de)compression for data and files
18. [`Hash`](#18-hash) — SHA-256/SHA-1/SHA-512/MD5 digests and HMAC (hex output)
19. [`CSV`](#19-csv) — parse / stringify RFC 4180-ish comma-separated values
20. [`UUID`](#20-uuid) — generate v4 (random) and v7 (time-ordered) UUIDs
21. [`Term`](#21-term) — terminal colour, cursor control, size, and key input for TUIs
22. [Design notes](#22-design-notes)
23. [Not included (yet)](#23-not-included-yet)

**Where to find what**

| Need… | Look at |
|---|---|
| Constants (π, e, inf, nan) | [§1 Math constants](#math-pi) |
| Scalar arithmetic (abs, min, max, log, exp, sqrt, floor, ceil, round) | [§1 Math](#1-math) |
| Trigonometry (sin, cos, tan, asin, acos, atan, atan2; radians) | [§1 Math](#1-math) |
| Print to stdout | `IO.puts` (with newline + quoting) / `IO.print` (raw) |
| Read a whole file | `FS.read` (throws on failure) |
| Stream a file (lines / chunks / seek) | [§4 File](#4-file) — `File.open` / `File.with` |
| Path manipulation (join, basename, dirname, stem, extension) | [§3 FS](#3-fs) |
| Stat / walk / glob / copy / rename / symlink | [§3 FS](#3-fs) |
| Directory listing / create / remove | `FS.list_dir`, `FS.mkdir`, `FS.remove` |
| `Instant` / `Duration`, ISO 8601, calendar arithmetic | [§5 Time](#5-time) |
| Random numbers | `Random.int`, `.uniform`, `.gauss`, `.shuffle`, `.weighted_choice` |
| CLI argument parsing | [§10 Args](#10-args) |
| Process info | `Sys.argv`, `Sys.exit`, `Sys.env`, `Sys.executable` |
| Run an external command | [§11 Proc](#11-proc) — `Proc.run(["git", "status"])` |
| Call an HTTP/HTTPS API | [§15 Http](#15-http) — `Http.get("https://api.example/x")` |
| Escape / unescape HTML entities | [§16 Encoding](#16-encoding) — `Encoding.html.unescape("a &amp; b")` |
| Encode / decode base64, hex, url | [§16 Encoding](#16-encoding) — `Encoding.base64.encode(s)` |
| gzip / gunzip data or files | [§17 Compress](#17-compress) — `Compress.gzip(s)` / `Compress.gunzip(z)` |
| Hash / checksum / HMAC | [§18 Hash](#18-hash) — `Hash.sha256(s)` / `Hash.hmac_sha256(key, s)` |
| Parse / write CSV | [§19 CSV](#19-csv) — `CSV.parse(text)` / `CSV.stringify(rows)` |
| Generate a UUID | [§20 UUID](#20-uuid) — `UUID.v4()` / `UUID.v7()` |
| Run work on another thread (CPU parallelism) | [§12 Isolate](#12-isolate) — `Isolate.spawn(\|\| fib(40))` |
| Share fixed-layout data across threads/processes (zero copy) | [§12 SharedBuffer](#sharedbuffer--zero-copy-shared-fixed-layout-data) — `SharedBuffer.new(n, Vec2)` / `.file` / `.shared` |
| Share variable-length read-only data across threads (no copy) | [§12 Shared](#shared--immutable-values-shared-by-reference) — `Shared.new(value)` |
| Handle Ctrl+C / SIGINT gracefully | [§12 Signal](#signal--signalnotify--signalreset) — `Signal.notify(tx)` / `Signal.reset()` |
| Heap introspection / leak checks | [§7 GC](#gc--heap-introspection) — `GC.stat()` → `{live_objects, heap_bytes}` |
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
`clamp`) — **trigonometry** (`sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, in radians). Integer-sequence factories `range` / `iota`
are language-core globals — see [§18](language.md#18-core-built-in-functions).

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

### `Math.sin(x) -> Float`, `Math.cos(x) -> Float`, `Math.tan(x) -> Float`

Trigonometric functions; `x` is in **radians** (`Long` or `Float`).

```culebra
puts(Math.sin(Math.pi / 2))   # => 1.0
puts(Math.cos(0))             # => 1.0
```

### `Math.asin(x) -> Float`, `Math.acos(x) -> Float`, `Math.atan(x) -> Float`, `Math.atan2(y, x) -> Float`

Inverse trigonometric functions, returning radians. `asin` / `acos`
expect `x` in `[-1, 1]` (else `nan`). `Math.atan2(y, x)` is the
quadrant-aware arctangent of `y / x`.

```culebra
puts(Math.atan2(1.0, 1.0))    # => 0.7853981633974483
# (that is pi/4)
```

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

### `IO.read_all() -> String`

Read all of standard input to EOF. The portable replacement for
`FS.read("/dev/stdin")` (which is POSIX-only — `/dev/stdin` doesn't exist on
Windows). Empty string on immediate EOF.

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { IO.read_all() }
```

### `IO.eputs(x: Any) -> Nil` / `IO.eprint(x: Any) -> Nil`

Write to standard error — the twins of `puts` / `print`. `eputs` quotes
strings and adds a newline (like `puts`); `eprint` writes the raw display
form (like `print`). Use for diagnostics that shouldn't mix into stdout.

```culebra
# doctest: skip
IO.eputs("warning: retrying")     # → stderr
if !ok { IO.eprint("error: {msg}\n") }
```

### `IO.stdin_is_terminal() -> Bool` / `IO.stdout_is_terminal() -> Bool` / `IO.stderr_is_terminal() -> Bool`

Whether the given standard stream is connected to a terminal (POSIX
`isatty`). Lets a script branch on interactivity: prompt vs. read a
pipe (stdin), colorize vs. emit plain output (stdout / stderr).
Equivalent to Rust `io::stdin().is_terminal()` / Node
`process.stdin.isTTY`. Each returns `false` when the stream is
redirected to a file or pipe.

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { FS.read("/dev/stdin") }
if IO.stdout_is_terminal() { puts(colorize(msg)) } else { puts(msg) }
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
line. The first non-flag argument is the script path; **everything
after it** is captured as `argv` (the Python / Node convention).
culebra's own flags (`--jit`, `--debug`, …) must precede the script
path. Empty when the script is invoked with no trailing arguments or
when running in the REPL.

```culebra
# $ culebra run.cul hello world
puts(Sys.argv)        # ['hello', 'world']
# $ culebra --jit run.cul hello   →  ['hello']   (--jit is culebra's)
```

A standalone `--` is an optional escape hatch: it stops flag parsing,
so the next argument becomes the script even if it begins with a dash
(e.g. `culebra -- -weird.cul`). It is otherwise unnecessary.

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

### `Sys.executable -> String`

Absolute path to the running culebra binary. Use it to launch a worker copy of
the interpreter — e.g. `Proc.run([Sys.executable, "worker.cul"], ...)` — instead
of relying on `culebra` being on `PATH`. (In an AOT-built program it is the path
to that standalone binary.)

```culebra
# doctest: skip
puts(Sys.executable)           # '/usr/local/bin/culebra'
```

### `GC` — heap introspection

`GC.stat()` runs a full collection and returns an `Object` describing the
live heap right after it:

| key | type | meaning |
|---|---|---|
| `live_objects` | `Long` | number of reachable heap objects |
| `heap_bytes` | `Long` | bytes those objects occupy |

Because it collects first, the numbers report *reachable* state, not cycle
residue still awaiting sweep. The call itself allocates the result `Object`,
so back-to-back readings differ by a small constant — measure a delta around
the code under test rather than an absolute count.

```culebra
# doctest: skip
let base = GC.stat().live_objects
build_some_structure()
puts(GC.stat().live_objects - base)   # objects retained by the structure
```

This is the foundation for leak-regression tests (see
`tests/test_gc_no_leak.cul`): assert that a delta stays bounded across many
iterations. Memory is otherwise managed automatically — see the language
spec for the memory model and deterministic `drop`.

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

JIT note: built-in `JSON.{stringify, parse}` route through the same
canonical call resolver every other namespace method uses, so every
call shape behaves identically to the interpreter: positional binding
(`JSON.stringify(v, 2)` sets `indent`, `JSON.parse(s, true)` sets
`lines`), keyword arguments, and both literal `**{...}` and dynamic
`**variable` splats. Used as a first-class value the binding is the
same (`let f = JSON.stringify; f(v, indent: 2)`).

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

### `Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false, timeout=0, share=nil) -> Object`

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

`share: {name: buf}` hands one or more `SharedBuffer.shared(...)` buffers to the
child process (which re-attaches each with `SharedBuffer.receive(name, Class)`).
The child must be a culebra process — typically `[Sys.executable, "worker.cul"]`.
See [SharedBuffer › Sharing across processes](#sharing-across-processes-zero-copy).

### `Proc.all(commands: Array<Array<String>>, limit: Long = <cpus>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0, share: Object? = nil) -> Array<Object>`

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

**`share: {name: buf}`** hands the same `SharedBuffer.shared(...)` buffers to
*every* child (each re-attaches with `SharedBuffer.receive`), exactly as
`Proc.run`'s `share:` does — so a pool of workers can write a shared result
buffer (use `buf.with_lock` for contended cells).

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

### `Proc.race(commands: Array<Array<String>>, share: Object? = nil) -> Object`

Starts every command, returns the result Object of the **first to finish**,
and sends `SIGKILL` to the rest (then reaps them). Useful for racing
redundant providers or "first mirror to respond wins". An empty list throws
`ValueError`. `share: {name: buf}` hands shared buffers to the children, as in
`Proc.run` / `Proc.all`.

```culebra
# doctest: skip
let fastest = Proc.race([
  ["curl", "-s", "https://mirror-a.example/file"],
  ["curl", "-s", "https://mirror-b.example/file"],
])
IO.print(fastest.stdout)
```

### `Proc.spawn(cmd: Array<String>, cwd=nil, env=nil, stdin="", share=nil) -> handle`

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

**`Channel.new(0)` is a rendezvous channel** (capacity 0): `send` does not return
until a receiver takes the value — a synchronous hand-off with no buffering.
Useful for backpressure (a producer can't run ahead of its consumer). Within a
single isolate it deadlocks (the send has no one to hand to), so use it across
isolates. Capacity must be `>= 0`.

#### `Channel.fan_in(sources: [rx]) -> rx`

Merge several receivers into one. The returned `rx` yields values from whichever
source is ready (it waits on all of them at once, like Go's `select` or
core.async's `merge` — event-driven, not polling), and ends once **every** source
is closed. This lets each producer own its **own** channel instead of sharing one
via `tx.clone()`, which sidesteps the multi-producer trap (a forgotten clone-drop
hanging the consumer) — each channel then has exactly one `tx`, dropped 1:1.

```culebra
# doctest: skip
mut handles = []
mut sources = []
for w in workers {
  let (tx, rx) = Channel.new()
  handles.push(Isolate.spawn(fn () { produce(w, tx) }))  # producer's tx auto-drops on exit
  tx.drop()                                               # the parent's own tx (1:1, obvious)
  sources.push(rx)
}
for v in Channel.fan_in(sources) { consume(v) }           # one stream, all producers
for h in handles { h.join() }                             # keep handles alive; collect errors
```

The merge **takes over** the given receivers — read them only through the merged
`rx`, not directly (reading an original races the merge). Order across sources is
not preserved (it is a merge). An empty list yields an immediately-closed `rx`; a
single source is a pass-through.

#### `Channel.fan_in(items, fn) -> rx`

The all-in-one form: spawn one producer isolate per item, running `fn(item, tx)`,
and merge their streams. `fn` sends to its own `tx`; fan_in creates the channels,
drops the parent's senders, and owns the producer handles — so the consumer
writes **no `tx`, no `drop`, and no handle** at all. `fn` and every item must be
Sendable.

```culebra
# doctest: skip
let merged = Channel.fan_in(workers, fn (w, tx) {
  for x in produce(w) { tx.send(x) }
})
for v in merged { consume(v) }
merged.join()        # join the producers; re-raises the first one that errored
```

`merged.join()` (after the stream ends) joins the spawned producers and re-raises
the first error; without it, producer errors are dropped (as with not joining an
`Isolate.spawn` handle). Producers run on their own threads.

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

**`on_progress:` reports completion.** Every method also accepts an
`on_progress: |done, total|` callback. Unlike `fn`, it is **not** Sendable: it
runs on the calling thread, so it can freely read and mutate captured state
(e.g. a counter or a progress bar). It is called as elements finish, with the
running completion count and the total; a throwing callback cancels the run.

```culebra
# doctest: skip
Parallel.map(urls, |u| fetch(u),
             on_progress: |done, total| IO.print("\r" + done.to_string() + "/" + total.to_string()))
```

(`map_reduce` is planned.)

### Signal — `Signal.notify` / `Signal.reset`

By default Ctrl+C raises a cooperative `Interrupted` (see the language guide's
*Interruption* section). For a long-running service you usually want the
opposite: catch the signal at one place and shut down on your own terms.
`Signal.notify(tx)` switches Ctrl+C from *throw* to *deliver* — each press sends
`"SIGINT"` to the channel's `tx` endpoint instead of interrupting the running
code, so the program blocks on `rx.recv()` (or `for sig in rx`) and drives its
own graceful shutdown. This is Go's `signal.Notify` model.

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
Signal.notify(tx)              # Ctrl+C now goes to the channel, not a throw
serve_in_background()
rx.recv()                      # blocks until the first Ctrl+C
puts("shutting down…")
drain_and_close()
```

| call | notes |
|---|---|
| `Signal.notify(tx)` | route Ctrl+C to this channel's `tx` (a non-blocking send — if the buffer is full the extra signal is dropped, as Go does); the throw is suppressed while active. Use a buffered channel (`Channel.new(1)`). |
| `Signal.reset()` | restore the default `Interrupted`-throw behavior and release the channel `notify` was holding. |

`notify` retains its own sender on the channel, so it stays open even after you
`tx.drop()` your copy — `reset()` releases it. There is no force-kill escalation
while notify is active: the program opted to handle signals itself (call
`reset()` if you want a later Ctrl+C to abort). The delivery runs from a
background poller, so a press is observed within a few tens of milliseconds.

### SharedBuffer — zero-copy shared fixed-layout data

`SharedBuffer` holds a flat array of fixed-layout records that several
isolates read and write **without copying** — the one place the isolate
model shares mutable memory, kept safe by restricting the records to fixed
scalar fields (no references, no pointers).

A record type is an ordinary class marked `@packable`, whose fields carry a
type annotation and an optional default:

```culebra
# doctest: skip
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
```

`@packable` fixes the byte layout (C-ABI natural alignment). Every field
must be a fixed scalar — `Float32`, `Float64`/`Float`, `Int8`, `Int16`,
`Int32`, `Int64`/`Long`, `Byte`, or `Bool`. A non-scalar field is a
`SyntaxError` at load time. A field with no default takes the type's zero
value (`0`, `0.0`, `false`).

#### `SharedBuffer.new(count, Class) -> buffer`

Allocates `count` zero-initialized records laid out per `Class`. `buffer.size`
(also `.count` / `.len`) reports the element count. The bytes live in this
process's heap — shareable across isolates (threads), not across processes.

#### `SharedBuffer.file(path, count, Class) -> buffer`

Same buffer, backed by a memory-mapped file (`MAP_SHARED`). Writes go to the
file's pages — **persistent** (the file outlives the process) and visible to any
other process that maps the same `path`. The handle gains a `flush()` method
(`msync` the dirty pages to disk); the file is an ordinary file you remove with
`FS.remove(path)`. Pointing `path` at a RAM-backed location (e.g. `/dev/shm/...`
on Linux) gives shared memory without disk durability.

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let buf = SharedBuffer.file("/tmp/grid.bin", 100, Cell)
buf[0].v = 42
buf.flush()                   # durable on disk
```

#### `SharedBuffer.shared(count, Class) -> buffer`

Same buffer, backed by **anonymous** shared memory (a name-less fd — `memfd` on
Linux, an immediately-unlinked POSIX shm object on macOS). It holds no name and
touches no disk; the kernel frees it once every handle is dropped. Its purpose
is to be handed to a **child process** via `Proc.run` / `Proc.spawn` `share:`
(see [Sharing across processes](#sharing-across-processes-zero-copy) below).

#### `buffer[i] -> view`

Indexing yields a **view** over element `i` (negative indices count from the
end). Reading or writing `view.field` goes straight to the backing bytes — no
per-record object is materialized:

```culebra
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
let buf = SharedBuffer.new(3, Vec2)
puts(buf.size)                # 3
buf[0].x = 1.5                # writes the bytes in place
let v = buf[0]                # a stored view aliases the same element
v.y = 2.5
puts([buf[0].x, buf[0].y])    # [1.5, 2.5]
```

Whole-element assignment (`buf[i] = ...`) is a `TypeError` — a record has no
standalone value form, so set its fields individually. An unknown field is an
`AttributeError`; an out-of-range index is an `IndexError`.

#### Sharing across isolates (zero copy)

A buffer crosses an isolate boundary **by reference** — the child reads and
writes the same bytes. Unlike every other value (copied at the boundary), a
SharedBuffer is shared, the same exception channels make. This makes the
classic data-parallel pattern — workers updating disjoint elements —
allocation-free:

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let cells = SharedBuffer.new(8, Cell)

Parallel.each([0, 1, 2, 3, 4, 5, 6, 7], fn (i) { cells[i].v = i * i })

# cells now holds 0, 1, 4, 9, 16, 25, 36, 49
```

Workers writing **disjoint** elements need no synchronization. Two isolates
writing the **same** element concurrently is a data race — partition the work
(as above) so each element has a single writer, or guard the shared update with
[`with_lock`](#synchronizing-with-bufferwith_lockfn).

#### Sharing across processes (zero copy)

A `SharedBuffer.shared(...)` buffer can be handed to a **child process**, not
just another isolate. The parent passes it to `Proc.run` (or `Proc.spawn`) under
the `share:` keyword — an Object of `name -> buffer` — and the child re-attaches
it by that name with `SharedBuffer.receive(name, Class)`. Both processes map the
same physical pages: writes are visible without copying or serializing.

```culebra
# doctest: skip
# --- parent.cul ---
@packable class Cell { v: Int64 = 0 }
let grid = SharedBuffer.shared(4, Cell)
grid[0].v = 100
Proc.run([Sys.executable, "worker.cul"], share: {grid: grid})
puts(grid[0].v)               # the child's write, read back here
grid.drop()
```

```culebra
# doctest: skip
# --- worker.cul ---
@packable class Cell { v: Int64 = 0 }
let grid = SharedBuffer.receive("grid", Cell)
for i in 0..grid.count { grid[i].v = grid[i].v + (i + 1) * 10 }
grid.drop()
```

`receive` takes only the name and the `@packable` type — the element count comes
from the parent (so `grid.count` matches). The child declares the same
`@packable` class; `receive` checks the record sizes agree and raises a
`ValueError` on a layout mismatch, an unknown name, or a buffer that wasn't a
`SharedBuffer.shared(...)` (heap and file buffers don't cross this way — a file
buffer is shared by re-opening its `path`). `Sys.executable` is the path to the
running culebra binary, for launching a worker copy of the interpreter.

`Proc.run` blocks until the child exits, so its writes are complete on return.
For concurrent children, `Proc.spawn` each and `wait()` them; as with isolates,
let each child own **disjoint** elements so the writes never race.

#### Synchronizing with `buffer.with_lock(fn)`

Disjoint writes need no synchronization. When two writers genuinely must touch
the **same** data — a counter, a multi-field update that has to stay consistent
— `with_lock` is the escape hatch: it runs the callback while holding the
buffer's lock and returns the callback's value. The lock is released on every
exit, including a thrown exception.

```culebra
# doctest: skip
@packable class Counter { n: Int64 = 0 }
let tally = SharedBuffer.new(1, Counter)

Parallel.each(iota(0, 8), fn (w) {
  for _ in 0..1000 {
    tally.with_lock(fn () { tally[0].n = tally[0].n + 1 })
  }
})
puts(tally[0].n)              # 8000 exactly — no lost updates
```

The same call works across **processes**: a `.shared` or `.file` buffer carries
a process-shared lock, so children handed the buffer via `share:` (or another
process re-opening the same file `path`) mutually exclude each other. Keep the
callback short — it serializes every holder. The lock is **not reentrant**:
calling `with_lock` again on the same buffer from inside the callback
deadlocks. A non-function argument is a `TypeError`; a dropped buffer is a
`ValueError`.

A `.file` buffer reserves a small fixed header at the front of the file for this
lock, so its bytes are culebra's container format rather than a bare array of
your records — keep that in mind if an external tool reads the file directly.

#### Variable-count fields: `FixedArray<T, N>`

A `@packable` field may be a `FixedArray<T, N>` — a fixed-**capacity** inline
collection (`N` elements of a scalar `T`) whose **count** varies at runtime. It
is laid out fully inline (`[len][T × N]`, no pointers), so variable-count data
still fits a shared record — the VARCHAR(N) / fixed-array pattern.

```culebra
# doctest: skip
@packable class Body {
  mass: Float32 = 0.0
  trail: FixedArray<Float32, 8>   # up to 8 points, starts empty
}

let bodies = SharedBuffer.new(100, Body)
let b = bodies[0]
b.trail.push(1.5)
b.trail.push(2.5)
b.trail.size()        # => 2   (capacity() => 8)
b.trail[0]            # => 1.5
b.trail[1] = 9.0
for p in b.trail { ... }
```

The view supports `.size()` / `.capacity()` / `.push(v)` / `.get(i)` /
`.set(i, v)` / `arr[i]` (read & write) / `for x in arr`. `push` past capacity
and an out-of-range index raise `IndexError`. The element type must be a fixed
scalar. Assigning the whole field (`record.field = ...`) is a `TypeError` —
mutate it through the view. The view reads/writes the record's bytes in place,
so it shares across isolates with the buffer.

#### Text fields: `FixedString<N>`

A `@packable` field may be a `FixedString<N>` — a fixed-**capacity** inline
UTF-8 string holding up to `N` bytes (`[len][byte × N]`, no pointers). Unlike
`FixedArray`, it is read and written as a whole `String` value — the VARCHAR(N)
pattern:

```culebra
# doctest: skip
@packable class Row {
  id: Int32
  name: FixedString<16>
}

let rows = SharedBuffer.new(100, Row)
rows[0].name = "alice"     # whole-value write (≤ 16 bytes)
rows[0].name               # => "alice"   (a real String)
rows[0].name.upper()       # => "ALICE"   (all String methods apply)
rows[1].name               # => ""        (zero value is empty)
```

`N` is a **byte** capacity. A string of more than `N` bytes raises
`CapacityError`; assigning a non-String raises `TypeError`. The read returns a
fresh `String` copy of the stored bytes, so it shares across isolates with the
buffer (a child isolate's write is visible to the parent's read).

#### Hash collections: `FixedSet<T, N>` and `FixedMap<K, V, N>`

A `@packable` field may also be a `FixedSet<T, N>` (up to `N` scalar values) or
a `FixedMap<K, V, N>` (up to `N` scalar key→value pairs). Both are
open-addressed hash tables laid out fully inline (`[count][states][entries]`,
no pointers), mutated in place through a view:

```culebra
# doctest: skip
@packable class Bag {
  tags:   FixedSet<Int32, 16>
  counts: FixedMap<Int32, Int32, 16>
}

let b = SharedBuffer.new(100, Bag)
let s = b[0].tags
s.add(5); s.add(7); s.add(5)   # the duplicate is a no-op
s.size()                       # => 2   (capacity() => 16)
s.contains(7)                  # => true
s.remove(7)                    # => true (false if absent)
for x in s { ... }

let m = b[0].counts
m.set(42, 1)
m.get(42)                      # => 1   (nil if absent)
m.contains(42)                 # => true
m.set(42, 2)                   # overwrite; size() unchanged
m.remove(42)                   # => true
m.keys()                       # => [Int32, ...]
for k, v in m { ... }          # yields (key, value) tuples
```

`add` / `set` past capacity raise `CapacityError`. Key and value types must be
fixed scalars; equality is by the scalar's bytes (so `FixedSet<Float32>` treats
`0.0` and `-0.0` as distinct). Assigning the whole field is a `TypeError` —
mutate through the view. The bytes live in the record, so the collection shares
across isolates with the buffer.

#### Optional fields: `T?`

A `@packable` field may be an optional scalar `T?` — a value-or-`nil` slot laid
out as `[present:byte][T]`. It is read and written as a whole value: `nil` when
the present byte is clear, otherwise the scalar.

```culebra
# doctest: skip
@packable class Node {
  id:     Int32
  parent: Int32?      # a sparse "no parent" slot
}

let n = SharedBuffer.new(100, Node)
n[0].parent            # => nil   (zero value)
n[0].parent = 5
n[0].parent            # => 5
n[0].parent = nil      # clear it
n[0].parent ?? -1      # => -1
```

`0` is a real value, distinct from `nil`. Only scalar optionals are packable
(`T` must be a fixed scalar). Sparse structures — an id→optional-slot array —
are the main use, alongside packable enums for tagged payloads.

#### Tagged unions: `@packable enum`

An `@packable` enum is a fixed tagged union `[tag:i32][payload]`: every
variant's scalar payload shares one region sized to the largest variant, and
the tag selects which variant is live. A `@packable` class field may then be
that enum type. Use it for component kinds, message types, and other
discriminated payloads in a shared record.

```culebra
# doctest: skip
@packable enum Shape {
  Circle(Float32),
  Rect(Float32, Float32),
  Point
}

@packable class Obj {
  id:    Int32
  shape: Shape
}

let objs = SharedBuffer.new(100, Obj)
objs[0].shape = Shape.Rect(2.0, 3.0)   # write a variant value
match objs[0].shape {                  # read it back and match
  Rect(w, h) => w * h,
  Circle(r)  => 3.14 * r * r,
  Point      => 0.0
}
```

Every variant payload must be a fixed scalar (a non-scalar payload is a
`SyntaxError` at the variant). Writing a value that is not an instance of that
enum raises `TypeError`. Reading reconstructs the variant instance from the
bytes (no enum namespace needed), so a value written by one isolate is
readable by another through the shared buffer.

#### Raw bytes: `Bytes<N>`

A `@packable` field may be a `Bytes<N>` — exactly `N` raw bytes inline, with no
length prefix, read and written as a whole byte `String`. For hash digests,
UUIDs, and other fixed binary blobs.

```culebra
# doctest: skip
@packable class Entry {
  id:     Int32
  digest: Bytes<32>      # e.g. a SHA-256
}

let e = SharedBuffer.new(100, Entry)
e[0].digest = some_32_byte_string
e[0].digest                       # => the 32 bytes (binary-safe)
```

The written `String` must be **exactly** `N` bytes (a `ValueError` otherwise);
a non-String raises `TypeError`. The bytes are binary-safe (embedded NULs are
preserved). Unlike `FixedString<N>` (a variable-length text field with a length
prefix), `Bytes<N>` is a fixed-size blob.

#### Nested records: a `@packable` class field

A `@packable` class field may be another `@packable` class — its record is
stored inline, and `outer.inner` yields a view over those bytes, so a nested
field assignment writes through in place:

```culebra
# doctest: skip
@packable class Point { x: Float32  y: Float32 }
@packable class Line  { id: Int32   start: Point  end: Point }

let lines = SharedBuffer.new(100, Line)
lines[0].start.x = 1.0        # writes into the inline Point's bytes
lines[0].start.y = 2.0
lines[0].start.x             # => 1.0
lines[0].end = lines[0].start # copy a whole sub-record (memcpy)
```

The nested class must be declared (and `@packable`) before the class that
nests it. Assigning a whole sub-record copies the bytes of another record of
the **same** class (otherwise `TypeError`); to set individual fields, go
through the view (`outer.inner.field = v`).

### Shared — immutable values shared by reference

`Shared.new(value)` freezes an ordinary value — any nesting of objects,
arrays, tuples, sets, and scalars — and hands back a **read-only view**
that every isolate can use without copying. It is the lane for
variable-length read-only data (a tokenizer dictionary, a parsed config,
a search index): the channel lane would copy it per task and
`SharedBuffer` requires a fixed layout. One frozen tree, any number of
readers:

```culebra
# doctest: skip
let dict = Shared.new(JSON.parse(FS.read("vocab.json")))

let workers = [0, 1, 2, 3].map(|i| Isolate.spawn(fn () {
  dict["hello"]          # all isolates read the SAME frozen tree
}))
```

Reads look like ordinary collection access — `view.field`, `view[key]`,
`view[i]`, `view.size()`, `view.has(k)`, `view.keys()` / `view.values()`,
and `for ... in view` (an Object view yields `(key, value)` pairs, an
Array/Tuple/Set view yields elements). A scalar field materializes into
the reader's heap; a container field comes back as another shared view
(still no copy). `view.copy()` deep-materializes back into ordinary
mutable values when you need a local scratch copy.

The freeze is the same walk that ships values to isolates, so anything
Sendable freezes — with two extra rejections: **functions** (`Shared`
values are data only) and **native handles** (channels, buffers, other
`Shared` views) raise `SendError`. Every write surface raises
`ImmutableError`; updates are copy-on-write by construction (build a new
value, `Shared.new` it again, hand out the new view). `view.drop()`
releases the reference (idempotent); the tree itself is freed when the
last view anywhere drops, and reads after that raise `ClosedError`.

| | lane | sharing | copy per reader |
|---|---|---|---|
| fixed-layout records | `SharedBuffer` | read **+ write** | none |
| variable-length, read-only | `Shared` | read | none |
| anything, mutable | channel | copy | yes |

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

For a constant pattern, the [`re"..."` literal](language.md#regex-literals) is
shorthand for `Regex.compile(...)` — `re'\d+'` and `re"hello"i` are the same
compiled `Regex`, with the body always raw and flags trailing the closing
quote. A literal also supports `${expr}` interpolation (escaped for a String,
spliced for a `Regex` — see `Regex.interp` below). Use `Regex.compile(...)`
directly when the *whole* pattern is built at runtime.

The pattern and subject accept either string flavor (`String` or `StringView`),
so the `StringView` results of `String.split` / `.slice` compose directly:
`Regex.compile('\d+').find_all(line.slice(0, 80))`.

| Constructor / static | Result |
| --- | --- |
| `Regex.compile(pat)` | `Regex` — compile (reused); bad pattern raises |
| `Regex.compile(pat, flags)` | `Regex` — `flags` a string of `"i"` / `"m"` / `"s"` |
| `Regex.escape(s)` | `String` — backslash-quote every metacharacter so `s` matches literally |
| `Regex.interp(x)` | `String` — splice helper for `re"...${x}..."`: a `Regex` → `(?:src)`, anything else → escaped to match literally |

For a single use, the namespace methods below take the pattern directly and
hide the `compile` step (like Python `re.search` / `re.sub`). Reusing one
pattern across many inputs still wants `Regex.compile(pat)`, but the engine
caches by pattern so the one-shot forms pay no recompile. Put flags inline
(`(?i)` / `(?m)` / `(?s)`).

| One-shot | Equivalent |
| --- | --- |
| `Regex.find(pat, s)` | `Regex.compile(pat).find(s)` — `Match` or `nil` |
| `Regex.match(pat, s)` | anchored match at the start |
| `Regex.find_all(pat, s)` | `[Match]` |
| `Regex.test(pat, s)` | `Bool` |
| `Regex.split(pat, s)` | `[String]` |
| `Regex.replace_all(pat, s, repl)` | `String` — template or `fn (Match) -> String` repl |

```culebra
Regex.find('(\d+)', "ab12")[1]                   // => "12"
Regex.test('(?i)hello', "HELLO")                 // => true (inline flag)
Regex.replace_all('[;；]', "a;b；c", "、")        // => "a、b、c"
Regex.find('x', "y")?.value ?? "none"            // => "none" (composes with ?. / ??)
```

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

Subscripts are the captures accessor: `m[i]` returns positional group `i`'s
text (`m[0]` is the whole match, negative indices wrap like an array) and
`m["name"]` returns a named group's text. Any miss — index out of range, an
unmatched optional group, or an unknown name — is `nil`, so it composes with
`?? ""`. Subscripting reaches only captures, never the record fields, so the
whole match is `m.value` or `m[0]` (not `m["value"]`). Use the dot fields when
you need spans (`m.groups[i].start`).

`Group` has `.value`, `.start`, `.end`. An invalid pattern raises `RegexError`.

```culebra
let d = Regex.compile('\d+')
d.test("abc 123")                                // => true
Regex.compile('\w+').find("  hello world").value // => "hello"
d.find("no digits")                              // => nil
d.find_all("a1 b22 c333").size()                 // => 3

let m = Regex.compile('(?<year>\d{4})-(\d{2})').find("2026-05")
m[1]                                             // => "2026" (positional capture)
m["year"]                                        // => "2026" (named capture)
m[0]                                             // => "2026-05" (whole match)
m[9] ?? "none"                                   // => "none" (miss -> nil)
m.groups[1].value                                // => "2026" (Group object, for spans)
m.named["year"].value                            // => "2026" (named via (?<name>...))

d.replace_all("a1 b22 c333", "#")                // => "a# b# c#"
Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1') // => "y.x"
d.replace_all("a1 b22", fn (m) { "<{m.value}>" })// => "a<1> b<22>" (callback)
Regex.compile('\s+').split("the quick  brown")   // => ["the", "quick", "brown"]
Regex.compile('hello', "i").test("HELLO world")  // => true (flag arg)
d.find("xyz")?.value ?? "none"                   // composes with ?. / ??

for m in d.find_iter("a1 b22") { break }         // lazy; stop early any time
d.find_iter("1 2 3").take(2).collect().size()    // => 2 (no full scan)
Regex.escape("a.b(c)")                           // => `a\.b\(c\)` (literal match)
```

The supported syntax (literal / `.` / character classes / `* + ? {n,m}` greedy
and lazy / `|` / capturing and named groups / `\d \w \s \b` / lookahead /
variable-length lookbehind / `\p{…}` Unicode properties) and the full matching
model and resource limits are documented in `docs/regexlib.md`.

---

## 15. `Http`

Synchronous HTTP/HTTPS client (engine: vendored `cpp-httplib` + OpenSSL,
statically linked). Each call **blocks** until the response arrives — there is
no async/await. TLS is automatic for `https://` URLs; the system trust store is
used to verify server certificates (macOS keychain / the platform CA bundle on
Linux). `gzip` / `deflate` responses are decompressed transparently — `body`
is always the decoded content.

Every method returns a **response Object** and raises only on a *transport*
failure:

| field | type | meaning |
|---|---|---|
| `status` | `Long` | HTTP status code (`200`, `404`, …) |
| `ok` | `Bool` | `true` iff `status` is in `[200, 300)` |
| `reason` | `String` | status reason phrase (`"OK"`, `"Not Found"`, …) |
| `body` | `String` | response body (raw bytes) |
| `headers` | `Object` | response headers, keyed by name (String → String) |
| `json()` | `Any` | parse `body` as JSON (convenience for `JSON.parse(r.body)`) |

A **4xx/5xx response is a normal result** (`ok: false`), not an error — branch
on `status` / `ok`. A **transport failure** (DNS, connection refused, TLS
handshake, timeout) throws `HttpError`. A malformed URL (no scheme/host) also
throws `HttpError`; a bad `headers` value throws `TypeError`.

| Method | Result |
| --- | --- |
| `Http.get(url, headers=nil, timeout=0, follow_redirects=true)` | response Object |
| `Http.delete(url, headers=nil, timeout=0, follow_redirects=true)` | response Object |
| `Http.head(url, headers=nil, timeout=0, follow_redirects=true)` | response Object |
| `Http.post(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | response Object |
| `Http.put(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | response Object |
| `Http.request(method, url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | response Object — any method (PATCH, OPTIONS, …) |
| `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true)` | response Object — streams Server-Sent Events to `on_event`; see below |

Keyword arguments (shared by every method):

- `headers: Object` — request headers; an `Object` whose values are all
  `String`. A non-`String` value is a `TypeError` (default: none).
- `params: Object` — query-string parameters; an `Object` of `String` values,
  appended to the URL percent-encoded (`?k=v&…`, preserving any query already in
  the URL). A non-`String` value is a `TypeError` (default: none).
- `timeout: Long` — per-phase timeout in **seconds** for connect / read / write;
  `0` uses the library default (default: `0`).
- `follow_redirects: Bool` — chase `3xx` `Location` headers (default: `true`).
- `into: String | Function` — stream the response body to a sink instead of
  buffering it; see Streaming below (default: `nil` = buffer into `body`).
- `json: Any` (`post` / `put` / `request` only) — serialize the value to JSON
  and send it as the body with `Content-Type: application/json`.
- `form: Object` (`post` / `put` / `request` only) — an `Object` of `String`
  values sent as an `application/x-www-form-urlencoded` body (percent-encoded).
  At most one of `body` / `json` / `form` may be given (else `TypeError`).
- `body: String | Function` / `content_type: String` (`post` / `put` /
  `request` only) — request body and its `Content-Type` (the header is set only
  when the body is non-empty and no explicit `Content-Type` was passed in
  `headers`). A `String` is sent whole; a `Function` is a **producer** streamed
  chunked — see Streaming below.

```culebra
# doctest: skip
let r = Http.get("https://api.example.com/users", params: {page: "2"})
if r.ok {
  let users = r.json()                 # parse the response body as JSON
  IO.puts(users.size().to_string())
} else {
  IO.puts("request failed: {r.status}")
}

# POST JSON with a header and a timeout (`json:` serializes + sets Content-Type).
let resp = Http.post("https://api.example.com/users",
                     json: {name: "alice"},
                     headers: {Authorization: "Bearer " + token},
                     timeout: 30)
assert_true(resp.ok)

# A transport failure throws; a 404 does not.
let missing = Http.get("https://api.example.com/nope")
assert_eq(missing.ok, false)        # 404 is a normal result
assert_eq(missing.status, 404)
```

The `get`/`post`/etc. methods return the whole body as a single `String` (read
into memory); pair it with [`JSON.parse`](#9-json) for JSON APIs.

**Streaming (download) — the `into:` argument.** For a response too large to
hold in memory, pass `into:` to stream the body to a sink instead of buffering
it. It works on any method and leaves the returned `body` empty (the bytes go to
the sink). `into:` accepts:

* a **`String`** — a file path; the body is written straight to that file.
* a **`Function`** — a `|chunk|` closure called with each chunk as it arrives.
  The callback runs on the calling thread, so it may read and mutate captured
  state; if it throws, the transfer is aborted and the error propagates.

```culebra
# doctest: skip
Http.get("https://example.com/big.tar.gz", into: "big.tar.gz")   # → file

mut bytes = 0
Http.get("https://example.com/big.csv", into: fn (chunk) { bytes = bytes + chunk.size() })

# any method, e.g. a POST whose response streams back:
Http.post("https://example.com/query", body: q, into: fn (chunk) { handle(chunk) })
```

**Streaming (upload).** Symmetrically, pass a `Function` as `body:` (on
`post` / `put` / `request`) to stream the request body chunked, so a large
upload never lives in memory at once. The producer is called repeatedly and
returns the next chunk `String`, or `nil` to end the stream:

```culebra
# doctest: skip
let f = File.open("big.bin")
Http.post(url, body: fn () {
  let chunk = f.read(65536)
  chunk.size() > 0 ? chunk : nil          # nil signals end-of-stream
}, content_type: "application/octet-stream")
```

The producer runs on the calling thread (it may mutate captured state); if it
throws, the upload is aborted and the error propagates. A non-`String`/non-`nil`
return is a `TypeError`.

### `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true) -> Object`

Open a [Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events)
(`text/event-stream`) stream — a long-lived `GET` whose `on_event` callback is
invoked once per event as it arrives. This is the wire format that streaming
LLM and chat APIs use. The call blocks for the life of the stream and returns
the final response Object after the server closes it.

Each event is an Object with three String fields:

| field   | meaning |
|---------|---------|
| `event` | the `event:` type, or `"message"` when the server sends none |
| `data`  | the `data:` payload; multiple `data:` lines are joined with `\n` |
| `id`    | the last `id:` field seen, or `""` |

```culebra
# doctest: skip
Http.sse("https://api.example/v1/stream", fn (e) {
  if e.data == "[DONE]" { return }
  let delta = JSON.parse(e.data)
  IO.print(delta.choices[0].delta.content)
})
```

`Accept: text/event-stream` is sent automatically unless you set `Accept`
yourself. Comment lines (`: ...`) and the `retry:` field are ignored. The
callback runs on the calling thread (it may mutate captured state); returning
ends handling of that event, and throwing aborts the stream and propagates the
error. A transport failure is an `HttpError`.

**Parallel and racing requests** use the general [`Parallel`](#12-isolate)
combinators over `Http.get`, not an HTTP-specific API — the same shape as
`Promise.all`/`race` in JS or `Task.async_stream` in Elixir:

```culebra
# doctest: skip
let urls = ["https://api.example/a", "https://api.example/b"]
Parallel.map(urls, |u| Http.get(u).body)        # all, input order (fail-fast)
Parallel.map_settled(urls, |u| Http.get(u))     # allSettled: [{ok, value, error}, ...]
Parallel.race(urls, |u| Http.get(u))            # first success wins, cancels the rest
```

TLS currently links OpenSSL statically; a future swap to BoringSSL is a build-only
change and does not affect this API (BoringSSL verifies hostnames more strictly, so
a server with a CN-only certificate that works today may then be rejected).

---

## 16. `Encoding`

Text codecs, grouped into a **sub-namespace per scheme** (`Encoding.html`,
`Encoding.base64`, `Encoding.hex`, `Encoding.url`). The codec logic is shared
between the interpreter and the JIT/AOT backends, and every codec is
binary-safe (embedded NUL bytes survive a round trip).

### `Encoding.html`

| Function | Result |
| --- | --- |
| `Encoding.html.escape(s)` | `String` — replace the five HTML-unsafe characters (`& < > " '`) with entities |
| `Encoding.html.unescape(s)` | `String` — turn entity references back into their characters |

`escape` replaces `&` first (so the output is always safe to re-escape) and emits
`&amp;` `&lt;` `&gt;` `&quot;` `&#39;`.

`unescape` handles numeric references `&#DDD;` (decimal) and `&#xHHH;` / `&#XHHH;`
(hexadecimal, any case), plus a practical set of named references — the common
typographic, Latin-1, Greek, math, and currency entities, **not** the full HTML5
table of ~2200 names. A reference must end in `;`; anything that is not a
well-formed, recognized reference is left **exactly as written** (browser-style
leniency), so a bare `&` and unknown entities pass through unchanged.

```culebra
puts(Encoding.html.escape("a & b < c"))          # => 'a &amp; b &lt; c'
puts(Encoding.html.escape("it's fine"))          # => 'it&#39;s fine'
puts(Encoding.html.unescape("Tom &amp; Jerry"))  # => 'Tom & Jerry'
puts(Encoding.html.unescape("caf&eacute; &mdash; x")) # => 'café — x'
puts(Encoding.html.unescape("&#65;&#x42;"))      # => 'AB'
puts(Encoding.html.unescape("&#12354;"))         # => 'あ'
puts(Encoding.html.unescape("&unknownent;"))     # => '&unknownent;'
```

### `Encoding.base64`

| Function | Result |
| --- | --- |
| `Encoding.base64.encode(s)` | `String` — base64 (RFC 4648 standard alphabet, `=` padding) |
| `Encoding.base64.decode(s)` | `String` — the decoded bytes; raises `ValueError` on invalid input |

`encode` is binary-safe (any byte string, including multi-byte UTF-8).
`decode` tolerates ASCII whitespace in the input (so line-wrapped base64
decodes) and `=` padding; an out-of-alphabet character raises `ValueError`.

```culebra
puts(Encoding.base64.encode("user:pass"))   # => 'dXNlcjpwYXNz'
puts(Encoding.base64.decode("dXNlcjpwYXNz")) # => 'user:pass'
```

```culebra
# doctest: skip
# e.g. an HTTP Basic auth header
let cred = Encoding.base64.encode(user + ":" + password)
let r = Http.get(url, headers: {Authorization: "Basic " + cred})
```

### `Encoding.hex`

| Function | Result |
| --- | --- |
| `Encoding.hex.encode(s)` | `String` — lower-case hex, two digits per byte |
| `Encoding.hex.decode(s)` | `String` — the decoded bytes; raises `ValueError` on invalid input |

`encode` always emits lower-case digits. `decode` accepts either case and
raises `ValueError` on an odd-length string or any non-hex character.

```culebra
puts(Encoding.hex.encode("abc"))   # => '616263'
puts(Encoding.hex.decode("616263")) # => 'abc'
puts(Encoding.hex.decode("00FF").size()) # => 2
```

### `Encoding.url`

| Function | Result |
| --- | --- |
| `Encoding.url.encode(s)` | `String` — percent-encode (RFC 3986) |
| `Encoding.url.decode(s)` | `String` — decode percent-escapes |

`encode` keeps the unreserved set `A-Z a-z 0-9 - _ . ~` verbatim and turns every
other byte into `%XX` with upper-case hex, so a space becomes `%20` (not `+`)
and multi-byte UTF-8 is percent-encoded byte by byte. `decode` is lenient: a
`%` not followed by two hex digits is left **as written**, and a literal `+`
stays a `+` (so `encode`/`decode` round-trip exactly).

```culebra
puts(Encoding.url.encode("a b&c"))   # => 'a%20b%26c'
puts(Encoding.url.decode("a%20b%26c")) # => 'a b&c'
puts(Encoding.url.encode("café"))    # => 'caf%C3%A9'
```

---

## 17. `Compress`

gzip (de)compression, backed by zlib. Both functions are binary-safe (embedded
NUL bytes survive a round trip) and interoperate with the standard `gzip` tool.

| Function | Result |
| --- | --- |
| `Compress.gzip(data: String) -> String` | gzip-compressed bytes (RFC 1952 wrapper) |
| `Compress.gunzip(data: String) -> String` | the decompressed bytes; raises `ValueError` on malformed input |

`gunzip` auto-detects the header, so it decompresses both gzip and zlib
(`deflate`) streams. A truncated or non-gzip input raises `ValueError`.

```culebra
let original = "the quick brown fox the quick brown fox the quick brown fox the quick brown fox"
let z = Compress.gzip(original)
puts(z.size() < original.size())          # => true
puts(Compress.gunzip(z) == original)      # => true
```

```culebra
# doctest: skip
# Read a .gz file and write one back
let text = Compress.gunzip(FS.read("logs.gz"))
FS.write("out.gz", Compress.gzip(text))
```

HTTP responses are decompressed transparently by the `Http` client (it sends
`Accept-Encoding` and inflates `Content-Encoding: gzip` automatically), so
`Compress` is for data and files you handle yourself, not for `Http` bodies.

---

## 18. `Hash`

Message digests and HMAC, self-hosted (no OpenSSL dependency) and identical on
every backend. Each function returns the **lowercase hex** digest. Inputs are
binary-safe (embedded NUL bytes are part of the message, not a terminator).

| Function | Result |
| --- | --- |
| `Hash.sha256(data: String) -> String` | 64-char hex SHA-256 digest |
| `Hash.sha1(data: String) -> String` | 40-char hex SHA-1 digest |
| `Hash.sha512(data: String) -> String` | 128-char hex SHA-512 digest |
| `Hash.md5(data: String) -> String` | 32-char hex MD5 digest |
| `Hash.hmac_sha256(key: String, data: String) -> String` | 64-char hex HMAC-SHA-256 |
| `Hash.hmac_sha1(key: String, data: String) -> String` | 40-char hex HMAC-SHA-1 |
| `Hash.hmac_sha512(key: String, data: String) -> String` | 128-char hex HMAC-SHA-512 |

```culebra
puts(Hash.sha256("abc"))
# => 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
puts(Hash.md5("abc"))
# => '900150983cd24fb0d6963f7d28e17f72'
puts(Hash.hmac_sha256("Jefe", "what do ya want for nothing?"))
# => '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'
```

For a raw (non-hex) digest, decode the hex with `Encoding.hex.decode`; to
re-encode in another form, pair it with `Encoding.base64`. MD5 and SHA-1 are
provided for compatibility with existing systems (checksums, legacy APIs) — use
SHA-256 or SHA-512 for new security-sensitive work.

---

## 19. `CSV`

Parse and render comma-separated values (RFC 4180-ish), shared byte-for-byte
across backends. Fields are separated by commas and records by newlines; a
field containing a comma, double quote, or newline is wrapped in double
quotes, and an embedded quote is doubled (`""`).

| Function | Result |
| --- | --- |
| `CSV.parse(text: String, delimiter: String = ",") -> Array<Array<String>>` | rows of String fields |
| `CSV.stringify(rows: Array, delimiter: String = ",") -> String` | CSV text; each row must be an Array, each field rendered like `to_string` |

The `delimiter:` option (a single byte; first byte used) selects the field
separator — pass `"\t"` for TSV. It may be given positionally or by keyword.

`parse` is lenient (no errors): every field comes back as a `String` (numbers
are not inferred), an empty input yields no rows, and a trailing newline does
not add an empty final row. Both LF and CRLF end a record. `stringify` renders
each field with the same conversion as `to_string`, so scalars like numbers
and `Bool` serialize naturally; `stringify(parse(text))` round-trips
well-formed input.

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25")
puts(rows[1])                                         # => ['alice', '30']
puts(CSV.stringify([["a,b", "c"], [1, 2]]) == "\"a,b\",c\n1,2")   # => true
puts(CSV.parse("a\tb", delimiter: "\t")[0])           # => ['a', 'b']
```

To convert numeric columns, map over the parsed fields with `to_long` /
`to_float`. A header row, if present, is just `rows[0]` — there is no separate
header mode.

---

## 20. `UUID`

Generate canonical lowercase UUIDs (the `8-4-4-4-12` hyphenated form). Two
variants:

| Function | Result |
| --- | --- |
| `UUID.v4() -> String` | random UUID (122 random bits) |
| `UUID.v7() -> String` | time-ordered UUID — a 48-bit Unix-millisecond prefix then random, so values sort by creation time (good as database keys) |

Entropy comes from the shared PRNG that `Random.*` uses, so UUIDs are
reproducible under `Random.seed` and are **not** cryptographically secure —
fine for identifiers, not for tokens or secrets. v7 orders by millisecond;
two values created within the same millisecond are not ordered relative to
each other (there is no monotonic counter).

```culebra
puts(UUID.v4().size())          # => 36
puts(UUID.v4() != UUID.v4())    # => true
```

---

## 21. `Term`

Terminal control for building text UIs — colour, cursor positioning, the
alternate screen, terminal size, and non-blocking key input. The colour and
escape helpers are pure functions that return strings, so they compose and
are easy to test; the stateful pieces (raw mode, the render loop) are wrapped
so the terminal is always restored on exit.

### Colour and attributes

Each returns its argument wrapped in the matching ANSI codes (and a reset),
so calls nest:

| Function | Result |
| --- | --- |
| `Term.fg(s, n) -> String` | 256-colour foreground (`n` is 0–255) |
| `Term.bg(s, n) -> String` | 256-colour background |
| `Term.rgb(s, r, g, b) -> String` | 24-bit truecolour foreground |
| `Term.red(s)` / `green` / `yellow` / `blue` / `magenta` / `cyan` / `white` / `black` | named 16-colour foreground |
| `Term.bold(s)` / `Term.dim(s)` / `Term.underline(s)` / `Term.reverse(s)` | text attributes |

Colours adapt to the terminal's **capability level** — `0` none, `1` 16,
`2` 256, `3` truecolour — detected from `isatty`, `NO_COLOR` (present ⇒ off),
`FORCE_COLOR`, `COLORTERM`, and `TERM`. A colour beyond the level is
downsampled (truecolour → nearest 256 → nearest 16), and at level 0 nothing
is emitted, so piped or `NO_COLOR` output stays plain. `Term.level()` reads
the level and `Term.set_level(n)` overrides it.

```culebra
puts(Term.bold(Term.fg("alert", 196)))   # bold bright-red "alert"
```

### Escapes, size, and width

| Function | Result |
| --- | --- |
| `Term.clear() -> String` | clear screen + home the cursor |
| `Term.move(x, y) -> String` | cursor to column `x`, row `y` (0-based) |
| `Term.hide()` / `Term.show()` | hide / show the cursor |
| `Term.cols()` / `Term.rows() -> Long` | terminal size in cells (80×24 off a tty) |
| `Term.size() -> (Long, Long)` | `(cols, rows)` |
| `Term.width(s) -> Long` | display width in columns (wide / emoji = 2, combining = 0) |
| `Term.flush()` | flush buffered output |

### Key input

`Term.key(raw) -> String` normalizes a raw byte sequence to a name:
`"Up"`, `"Down"`, `"Left"`, `"Right"`, `"Enter"`, `"Esc"`, `"Backspace"`, or
the literal character (e.g. `"q"`, `" "`); `""` means no input.
`Term.resized() -> Bool` is true once after the terminal is resized
(SIGWINCH), and `Screen.poll` surfaces a pending resize as the `"Resize"` key.

### `Term.app` and `Screen`

`Term.app(fn (screen) { ... })` enters raw mode and the alternate screen,
hides the cursor, watches for resizes, and **restores the terminal on exit**
— normal return, an exception, or Ctrl+C — via `defer`. The callback
receives a `Screen`:

| Method | Effect |
| --- | --- |
| `screen.size()` / `cols()` / `rows()` | terminal dimensions |
| `screen.clear()` | reset the back buffer to a blank frame (current size) |
| `screen.set(x, y, glyph)` | place one grapheme into the back buffer |
| `screen.put(x, y, s)` | lay the graphemes of `s` into successive cells |
| `screen.render() -> String` | minimal escapes to update the screen from the last frame (and advance the front buffer) |
| `screen.flush()` | print `render()` and flush |
| `screen.poll(timeout) -> String` | wait up to `timeout` seconds for a key (or `"Resize"`), returning its name (or `""`) |

A `Screen` is a double-buffered grid of cells. `flush` emits **only the cells
that changed** since the last frame, so live UIs update without flicker and
with minimal output; wide glyphs occupy two cells and a resize forces a full
repaint. Build a frame with `clear` + `set` / `put`, then `flush`; read input
with `poll` (which doubles as the per-frame delay). Cells hold plain glyphs
(per-cell colour is not yet supported).

```culebra
Term.app(fn (s) {
  s.clear()
  s.put(2, 1, "hello")
  s.flush()
  s.poll(2.0)            # wait up to 2s for a keypress
})
```

See `examples/donut.cul` (a no-input render loop) and `examples/froggy.cul`
(a full keyboard-driven game) for complete programs.

---

## 22. Design notes

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

## 23. Not included (yet)

### Heavier data structures

No `Queue`, `Deque`, or priority-heap type. `Set` and `Tuple` are
language built-ins (see [`docs/language.md`](language.md)); reach for
`Array` and `Object` for everything else.

### Networking / OS extras

No raw TCP/UDP sockets, DNS resolver, SQLite, or file watcher. Shell
out through [§11 Proc](#11-proc) when you need them.

---

See also: [`docs/language.md`](language.md) for the language
specification.
