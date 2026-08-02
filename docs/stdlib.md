# Culebra Standard Library

This document is the **API reference** for Culebra's built-in
library: the namespace objects (`Math`, `IO`, `Sys`, `FS`, `Time`,
`Args`, `Random`, `String`) that group the runtime utilities.
Everything described here is available without any `import`
statement.

For an introductory tour and usage idioms see
[`guide.md` §14](guide.md#14-standard-library).

Language-level built-ins — `to_long`, `to_float`, `to_string`,
`type_of`, `range`, `iota` — are specified in
[§19 of the language spec](language.md). The matcher family
(`assert_true` / `assert_eq` / `assert_throws` / etc.) is documented
in [§13 below](#13-matchers). Methods on built-in types (`String`,
`Array`, `Object`) are specified in
[§18 of the language spec](language.md).

The CLI (`src/main.cc`) additionally installs `inspect`, `print`, and
`println` as globals aliased to `IO.inspect` / `IO.print` / `IO.println`
(see [§22 of the language spec](language.md)). Embedders that use
`culebra::environment()` directly get a clean namespace without those
aliases.

Conventions used below:

* Types follow the annotations described in [§14 of the language
  spec](language.md). `Any` denotes any value.
* Throws clauses describe runtime errors of the form
  `type error at L:C.` etc. See [§15 of the language spec](language.md).
* Namespaces are **closed**: reading a member a namespace doesn't have
  raises `AttributeError: namespace 'IO' has no member 'read_all'` at the
  access site, rather than yielding `nil`. This turns a typo or a use of a
  removed API into an immediate, catchable error instead of a confusing
  failure at a later call. (Plain dicts keep the permissive rule — a missing
  key reads as `nil`.) The dict builtins `keys`/`values`/`has`/`get`/`size`
  still work on a namespace. `Path` is the one exception: it is a class, not
  a namespace, so a missing property reads as `nil` the way it does on any
  other class.

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
15. [`Http`](#15-http) — synchronous HTTP/HTTPS client (get/post/put/delete/head/request), server (routes, static files, WebSocket), and Server-Sent Events
16. [`Encoding`](#16-encoding) — text codecs by scheme (`Encoding.html`, `Encoding.base64`, `Encoding.hex`, `Encoding.url`)
17. [`Compress`](#17-compress) — gzip / deflate (de)compression for data and files
18. [`Hash`](#18-hash) — SHA-256/SHA-1/SHA-512/MD5 digests and HMAC (hex output)
19. [`CSV`](#19-csv) — parse / stringify RFC 4180-ish comma-separated values
20. [`Env`](#20-env) — parse / load dotenv-style `.env` files
21. [`UUID`](#21-uuid) — generate v4 (random) and v7 (time-ordered) UUIDs
22. [`Term`](#22-term) — terminal colour, cursor control, size, and key/mouse input for TUIs
23. [`Log`](#23-log) — leveled, structured logging to stderr (text / JSON, child loggers)
24. [`TOML`](#24-toml) — parse / stringify TOML configuration
25. [`SQLite`](#25-sqlite) — embedded SQL database (query / execute / prepared statements / transactions)
26. [`Canvas`](#26-canvas) — immediate-mode 2D framebuffer for games (shapes, sprites, offscreen targets, text, keys/mouse, tone, sound, music)
27. [`Scene`](#27-scene) — retained-mode 3D renderer for procedural geometry (opt-in, macOS-only)
28. [`Net`](#28-net) — raw TCP / UDP sockets and name resolution (the layer under `Http`)
29. [`Desktop` / `Webview`](#29-desktop--webview) — native WebView desktop app: local HTTP server + window, one call
30. [Design notes](#30-design-notes)
31. [Not included (yet)](#31-not-included-yet)

**Where to find what**

| Need… | Look at |
|---|---|
| Constants (π, e, inf, nan) | [§1 Math constants](#math-pi) |
| Scalar arithmetic (abs, min, max, log, exp, sqrt, floor, ceil, round) | [§1 Math](#1-math) |
| Trigonometry (sin, cos, tan, asin, acos, atan, atan2; radians) | [§1 Math](#1-math) |
| Print to stdout | `IO.inspect` (newline + quoting) / `IO.println` (newline, raw) / `IO.print` (raw, no newline) |
| Read a whole file | `FS.read` (throws on failure) |
| Stream a file (lines / chunks / seek) | [§4 File](#4-file) — `File.open` / `File.with` |
| Path manipulation (join, basename, dirname, stem, extension) | [§3 FS](#3-fs); fluent `Path` wrapper: [§3 `Path`](#path--the-fluent-wrapper) |
| Stat / walk / glob / copy / rename / symlink / chmod / chown | [§3 FS](#3-fs) |
| Directory listing / create / remove | `FS.list_dir`, `FS.mkdir`, `FS.remove` |
| `Instant` / `Duration`, ISO 8601, calendar arithmetic | [§5 Time](#5-time) |
| Wrap an index that can go negative into `0..n` | [§1 Math](#1-math) — `Math.wrap(i, n)` (`%` truncates, so it stays negative) |
| Random numbers | `Random.int`, `.uniform`, `.gauss`, `.shuffle`, `.weighted_choice` |
| CLI argument parsing | [§10 Args](#10-args) |
| Process info | `Sys.argv`, `Sys.exit`, `Sys.env`, `Sys.set_env`, `Sys.getcwd`, `Sys.chdir`, `Sys.executable`, `Sys.script` |
| Run an external command | [§11 Proc](#11-proc) — `Proc.run(["git", "status"])` |
| Call an HTTP/HTTPS API | [§15 Http](#15-http) — `Http.get("https://api.example/x")` |
| Serve HTTP — routes, static files, WebSocket | [§15 `Http.server()`](#httpserver---object) — `Http.server().get("/", h).listen(8080)` |
| Speak a raw TCP / UDP protocol, resolve a hostname | [§28 Net](#28-net) — `Net.connect(host, port)` / `Net.listen(port)` / `Net.udp()` / `Net.resolve(host)` |
| Escape / unescape HTML entities | [§16 Encoding](#16-encoding) — `Encoding.html.unescape("a &amp; b")` |
| Encode / decode base64, hex, url | [§16 Encoding](#16-encoding) — `Encoding.base64.encode(s)` |
| gzip / gunzip data or files | [§17 Compress](#17-compress) — `Compress.gzip(s)` / `Compress.gunzip(z)` |
| Compress without the gzip envelope | [§17 Compress](#17-compress) — `Compress.deflate(s, level: 9)` (decode with `Compress.gunzip`) |
| Hash / checksum / HMAC | [§18 Hash](#18-hash) — `Hash.sha256(s)` / `Hash.hmac_sha256(key, s)` |
| Parse / write CSV | [§19 CSV](#19-csv) — `CSV.parse(text)` / `CSV.stringify(rows)` |
| Parse / write TOML | [§24 TOML](#24-toml) — `TOML.parse(text)` / `TOML.stringify(obj)` |
| Query an embedded SQL database | [§25 SQLite](#25-sqlite) — `SQLite.open(path)` → `db.query(sql, params)` / `db.execute(...)` |
| Load a `.env` config file | [§20 Env](#20-env) — `Env.load(".env")` / `Env.parse(text)` |
| Generate a UUID | [§21 UUID](#21-uuid) — `UUID.v4()` / `UUID.v7()` |
| Leveled / structured logging | [§23 Log](#23-log) — `Log.info("msg", {k: v})` / `Log.with({req: id})` |
| Run work on another thread (CPU parallelism) | [§12 Isolate](#12-isolate) — `Isolate.spawn(\|\| fib(40))` |
| Share fixed-layout data across threads/processes (zero copy) | [§12 SharedBuffer](#sharedbuffer--zero-copy-shared-fixed-layout-data) — `SharedBuffer.new(n, Vec2)` / `.file` / `.shared` |
| Share variable-length read-only data across threads (no copy) | [§12 Shared](#shared--immutable-values-shared-by-reference) — `Shared.new(value)` |
| Handle Ctrl+C / SIGINT gracefully | [§12 Signal](#signal--signalnotify--signalreset) — `Signal.notify(tx)` / `Signal.reset()` |
| Desktop GUI (native WebView + local server) | [§29 Desktop](#29-desktop--webview) — `Desktop.run({title, assets, routes})` |
| Heap introspection / leak checks | [§7 GC](#gc--heap-introspection) — `GC.stat()` → `{live_objects, rc_objects, heap_bytes}` |
| String / Array / Object methods | [language spec §18](language.md) |
| Integer sequences (`range`, `iota`) | [language spec §19](language.md) |
| Conversion (`to_long`, `to_float`, `to_string`, `type_of`) | [language spec §19](language.md) |

---

## 1. `Math`

Numeric utilities. Integer-only routines (`pow`, `sign`, `clamp`,
`wrap`) preserve `Long` input; the Float-domain routines (`log`, `exp`,
`sqrt`, …) accept either `Long` or `Float` and return the shape
documented below. See [§4](language.md#4-types) and
[§7](language.md#7-expressions) of the language spec for how `Long`
and `Float` interact.

Sub-groups in this section: **constants** (`Math.pi`, `Math.e`,
`Math.inf`, `Math.nan`) — **scalar ops** (`abs`, `min`, `max`,
`log`, `exp`, `sqrt`, `floor`, `ceil`, `round`, `pow`, `sign`,
`clamp`, `wrap`) — **trigonometry** (`sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, in radians). Integer-sequence factories `range` / `iota`
are language-core globals — see [§19](language.md#19-core-built-in-functions).

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
inspect(Math.pi)              # => 3.141592653589793
inspect(Math.e)               # => 2.718281828459045
inspect(Math.inf > 1e308)     # => true
inspect(Math.nan == Math.nan) # => false
```

### Scalar operations

### `Math.abs(x: Long|Float) -> Long|Float`

Absolute value. Returns `Long` for `Long` input, `Float` for `Float`
input.

```culebra
inspect(Math.abs(-7))     # => 7
inspect(Math.abs(-7.5))   # => 7.5
```

### `Math.min(a, b, ...) -> Long|Float`, `Math.max(a, b, ...) -> Long|Float`

Smallest / largest of two or more numeric arguments. Returns `Long`
when every argument is `Long`; any `Float` argument promotes the
result to `Float`. At least two arguments are required; fewer — or
any non-numeric argument — raises `type error`.

```culebra
inspect(Math.min(3, 1, 4, 1, 5))   # => 1
inspect(Math.max(1.5, 2, 0.5))     # => 2.0
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
inspect(Math.sin(Math.pi / 2))   # => 1.0
inspect(Math.cos(0))             # => 1.0
```

### `Math.asin(x) -> Float`, `Math.acos(x) -> Float`, `Math.atan(x) -> Float`, `Math.atan2(y, x) -> Float`

Inverse trigonometric functions, returning radians. `asin` / `acos`
expect `x` in `[-1, 1]` (else `nan`). `Math.atan2(y, x)` is the
quadrant-aware arctangent of `y / x`.

```culebra
inspect(Math.atan2(1.0, 1.0))    # => 0.7853981633974483
# (that is pi/4)
```

### `Math.floor(x: Long|Float) -> Long`, `Math.ceil(x: Long|Float) -> Long`, `Math.round(x: Long|Float) -> Long`

Round a numeric value to an integer. `Long` input is returned
unchanged. `Math.floor` rounds toward `-∞`, `Math.ceil` toward `+∞`,
and `Math.round` uses **banker's rounding** (round half to even).

```culebra
inspect(Math.floor(-1.5))   # => -2
inspect(Math.ceil(-1.5))    # => -1
# A tie rounds to the even neighbour, so 2.5 and 3.5 both land on even:
inspect(Math.round(2.5))    # => 2
inspect(Math.round(3.5))    # => 4
```

### `Math.pow(base: Long, exp: Long) -> Long`

Integer exponentiation. `base ** exp`, computed by repeated squaring.
`Math.pow(x, 0)` is `1` for every `x` (including `0`).

**Throws**: `type error at L:C.` if `exp < 0`.

Kept for back-compat; **prefer the `**` operator** which also handles
`Float` and negative exponents (see language spec §7).

```culebra
inspect(Math.pow(2, 10))    # => 1024
inspect(Math.pow(7, 0))     # => 1
inspect(Math.pow(-3, 3))    # => -27
```

### `Math.sign(x: Long) -> Long`

Returns `-1` for negative, `0` for zero, `1` for positive.

```culebra
inspect(Math.sign(-5))      # => -1
inspect(Math.sign(0))       # => 0
inspect(Math.sign(42))      # => 1
```

### `Math.clamp(x: Long, lo: Long, hi: Long) -> Long`

Clamp `x` to the inclusive range `[lo, hi]`. No error is raised when
`lo > hi`; the result in that case is `hi`.

```culebra
inspect(Math.clamp(5, 0, 10))   # => 5
inspect(Math.clamp(-5, 0, 10))  # => 0
inspect(Math.clamp(15, 0, 10))  # => 10
```

### `Math.wrap(x: Long, n: Long) -> Long`

Wrap `x` into a span of `n` — the **floored** remainder, which `%` does
not give: `%` truncates, so its result carries the sign of `x`
([language spec §7](language.md#arithmetic)). `Math.wrap`'s result carries
the sign of `n`, so a positive `n` always lands it in `[0, n)`. That is
what a circular index wants: the element before index 0 is the last one,
not a negative subscript.

```culebra
inspect(Math.wrap(3, 320))     # => 3
inspect(Math.wrap(-3, 320))    # => 317
inspect(-3 % 320)              # => -3
inspect(Math.wrap(320, 320))   # => 0
```

The two agree wherever `x` is non-negative, so `Math.wrap` is only worth
reaching for when `x` can go below zero — a scroll offset, a wrapped
tile coordinate, an angle stepped backwards:

```culebra
let frames = ['a', 'b', 'c']
let prev = fn (i) { frames[Math.wrap(i - 1, frames.size())] }
inspect(prev(0))               # => 'c'
```

A negative `n` mirrors the whole thing — the result lands in `(n, 0]` —
and `n` of `0` raises `divide by 0 error`, exactly as `x % 0` does.

---

## 2. `IO`

Output and standard input. File reading/writing lives under `FS`
(`FS.read` / `FS.write` / `FS.exists`).

### `IO.inspect(x: Any) -> Nil`

Print `x` followed by a newline to standard output. Reference types
are formatted the same way as `Array.str_array()` /
`Object.str_object()`, and strings are printed **with surrounding
single quotes**.

```culebra
IO.inspect('hi')       # → 'hi'
IO.inspect(42)         # → 42
IO.inspect([1, 'a'])   # → [1, 'a']
```

### `IO.print(x: Any) -> Nil`

Write `x` to standard output **without a trailing newline**, using
`to_string` formatting (strings are **unquoted**). Useful for
building a single line of output from several writes.

```culebra
IO.print('Hello, ')
IO.print('world!')
IO.println('')      # → Hello, world!
```

### `IO.println(x: Any) -> Nil`

Print `x` followed by a newline to standard output, using `to_string`
formatting (strings are **unquoted**) — the raw-display twin of
`inspect`, and the newline-appending twin of `print`.

```culebra
IO.println('hi')       # → hi
IO.println(42)         # → 42
```

### `IO.input() -> String`

Read a single line from standard input. The trailing newline is
stripped. Returns `''` (empty string) on end-of-file.

```culebra
# doctest: skip
println('name?')
name = IO.input()
println("Hello, {name}")
```

### `IO.stdin() -> reader`

Return a read-only handle over standard input, sharing the same reader shape
as a `File` handle (so source-generic code works over either):

| Method | Result |
| --- | --- |
| `.read()` | the rest of standard input to EOF (the portable replacement for `FS.read("/dev/stdin")`, which is POSIX-only); empty string on immediate EOF |
| `.read(n: Long)` | up to `n` bytes (fewer only at EOF) |
| `.lines()` | a lazy iterator over input lines, trailing newline stripped, stopping at EOF |

`.lines()` streams in constant memory — the idiom for Unix filters over large
or unbounded input (`tail -f \| script`). Reads are blocking and interruptible
(a single Ctrl+C breaks the wait). Standard input is single-consumer; the
methods share one underlying buffer, so `.read(n)` then `.lines()` continues
where the byte read left off.

```culebra
# doctest: skip
# Filter: uppercase lines containing "error".
for line in IO.stdin().lines() {
    if line.contains("error") { IO.println(line.upper()) }
}

let src = if IO.stdin_is_terminal() { read_clipboard() } else { IO.stdin().read() }
```

### `IO.einspect(x: Any) -> Nil` / `IO.eprint(x: Any) -> Nil` / `IO.eprintln(x: Any) -> Nil`

Write to standard error — the twins of `inspect` / `print` / `println`.
`einspect` quotes strings and adds a newline (like `inspect`); `eprint`
writes the raw display form with no trailing newline (like `print`);
`eprintln` writes the raw display form with a trailing newline (like
`println`). Use for diagnostics that shouldn't mix into stdout.

```culebra
# doctest: skip
IO.einspect("warning: retrying")     # → stderr
if !ok { IO.eprint("error: {msg}\n") }
IO.eprintln("done")
```

### `IO.stdin_is_terminal() -> Bool` / `IO.stdout_is_terminal() -> Bool` / `IO.stderr_is_terminal() -> Bool`

Whether the given standard stream is connected to a terminal (POSIX
`isatty`). Lets a script branch on interactivity: prompt vs. read a
pipe (stdin), colorize vs. emit plain output (stdout / stderr).
Each returns `false` when the stream is
redirected to a file or pipe.

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { FS.read("/dev/stdin") }
if IO.stdout_is_terminal() { println(colorize(msg)) } else { println(msg) }
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
inspect(FS.join('a', 'b', 'c.txt'))      # => 'a/b/c.txt'
inspect(FS.join('/usr', 'local', 'bin')) # => '/usr/local/bin'
inspect(FS.join())                       # => ''
```

#### `FS.basename(path: String) -> String`

Final path component (filename + extension). Trailing separator
yields `""`.

```culebra
inspect(FS.basename('a/b/c.txt'))  # => 'c.txt'
inspect(FS.basename('/'))          # => ''
```

#### `FS.dirname(path: String) -> String`

Parent path. `""` for paths with no parent (`'c.txt' -> ''`).

#### `FS.extension(path: String) -> String`

File extension *including the leading dot*, or `""` for paths with
none. Dotfiles (`.hidden`) are treated as having no extension —
matches `std::filesystem::path::extension`.

```culebra
inspect(FS.extension('a/b/c.txt'))  # => '.txt'
inspect(FS.extension('.hidden'))    # => ''
```

#### `FS.stem(path: String) -> String`

Basename without the trailing extension.

```culebra
inspect(FS.stem('a/b/c.txt'))  # => 'c'
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

#### `FS.chmod(path: String, mode: Long) -> Nil`

Set the permission bits of `path` to `mode` — an integer, usually written
as an octal literal (`0o755`, `0o644`). `mode` is masked to the low 12 bits
(`rwx` for owner/group/other plus setuid/setgid/sticky) and replaces the
existing bits. Read the current bits back with `FS.stat(path).mode`. Throws
`IOError` if the path is missing or the permissions can't be changed.

```culebra
# doctest: skip
FS.chmod('deploy.sh', 0o755)       # make executable
inspect(FS.stat('deploy.sh').mode)    # 493  (0o755)
```

#### `FS.chown(path: String, owner = nil, group = nil) -> Nil`

Change the owner and/or group of `path`. `owner` and `group` each accept a
name (`String`), a numeric id (`Long`), or `nil` to leave that one unchanged.
Read the current ids back with `FS.stat(path).uid` / `.gid`. Changing the
owner usually requires root; changing the group works for a group you belong
to. Throws `IOError` on a missing path, an unknown user/group name, or a
permission failure; a non-String/Long/Nil argument is a `TypeError`.

```culebra
# doctest: skip
FS.chown('app.log', group: 'staff')      # set group by name, keep owner
FS.chown('data', 'deploy', 'deploy')     # set both by name (root)
```

### Stat / metadata

#### `FS.stat(path: String) -> Object`

Return `{size, is_dir, is_file, is_symlink, mtime, mode, uid, gid}` for
`path`. `size` is bytes (0 for non-regular files); `mtime` is seconds since
the Unix epoch; `mode` is the permission bits as an integer (compare with
octal, e.g. `st.mode == 0o644`); `uid` / `gid` are the owner and group ids;
`is_symlink` reflects the link itself while the other fields follow it. Throws
`IOError` if the path doesn't exist.

```culebra
# doctest: skip
let st = FS.stat('config.toml')
inspect(st.size)
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

### `Path` — the fluent wrapper

`Path` is a thin, **immutable** wrapper over the `FS.*` helpers. `FS` is
the primitive layer (raw `String` paths in, `String`/values out); `Path`
is the sugar layer that carries the path around so you stop threading path
strings by hand. Every operation returns a fresh `Path` (or a `String` for
components) — a `Path` is never mutated in place.

Reach for `FS.*` in short scripts; reach for `Path` when a path is built
up in stages or passed through several operations, where the `/` operator
and property chains read more clearly:

```culebra
# doctest: skip
let root = Path.new(FS.dirname(Sys.script)).resolve()
for src in root.glob("*/content.src.js") {
  let dst = src.parent / "content.js"         # vs FS.join(FS.dirname(src), …)
  dst.write(transform(src.read()))
  IO.print("{src.parent.name}/content.js\n")  # vs FS.basename(FS.dirname(src))
}
```

Construct with `Path.new(s)`, where `s` is a `String` or another `Path`.
Anywhere a method takes a path (`join`, `/`, `rename`, `==`) it likewise
accepts a `String` or a `Path`. String interpolation (`"{p}"`) and
`to_string(p)` yield the raw path string.

| Member | Returns | Delegates to |
|---|---|---|
| `p / other` / `p.join(other)` | `Path` | `FS.join` |
| `p.name` | `String` (final component) | `FS.basename` |
| `p.stem` | `String` (name without suffix) | `FS.stem` |
| `p.suffix` | `String` (extension incl. dot) | `FS.extension` |
| `p.parent` | `Path` | `FS.dirname` |
| `p.resolve()` | `Path` (absolute) | `FS.abspath` |
| `p.exists()` / `p.is_file()` / `p.is_dir()` | `Bool` | `FS.*` |
| `p.read()` / `p.write(s)` | `String` / `Nil` | `FS.read` / `FS.write` |
| `p.mkdir()` | `Nil` (creates parents) | `FS.mkdir` |
| `p.remove(recursive=false)` | `Nil` | `FS.remove` |
| `p.rename(dst)` | `Path` (the destination) | `FS.rename` |
| `p.list()` | `Array<Path>` (dir entries) | `FS.list_dir` |
| `p.glob(pattern)` | `Array<Path>` | `FS.glob` |
| `p.walk()` | `Array<Path>` (recursive) | `FS.walk` |
| `p.str()` | `String` (escape hatch) | — |

`name`, `stem`, `suffix`, and `parent` are **getters** (pure string
derivations), read without parentheses — `p.parent.name`, not
`p.parent().name()` (the call spelling still works). The filesystem
operations below stay methods because they do I/O and can throw.

Two `Path`s compare by their inner path string: `==` (also against a
`String`) and `<` / `<=` / `>` / `>=`, so a `Path` array sorts
(`paths.sorted()`) and works with `min` / `max` / `Set`. Ordering against a
non-path is a `TypeError` (no meaningful answer), whereas `==` against a
non-path is simply `false`.

The `FS.*` helpers and `File.open` / `File.with` accept a `Path` anywhere
they take a path — their path parameters are `String | Path` — so a `Path`
flows straight through without `.str()`:

```culebra
# doctest: skip
let cfg = Path.new("/etc") / "app.conf"
let text = FS.read(cfg)                 # FS.read(String | Path)
for line in File.open(cfg).lines() { }  # File.open(String | Path)
```

Only the *path-taking* stdlib functions opt in this way. `Path` and
`String` remain **distinct types** everywhere else: a plain `fn(x: String)`
does *not* accept a `Path`, so the type boundary stays meaningful — use
`p.str()` (or `"{p}"`) to hand a raw string to a `String`-only API.

---

## 4. `File`

A `File` is a **stateful handle** for streaming I/O — the complement
to `FS`'s one-shot whole-file operations (`FS.read` / `FS.write`).
Open one with `File.open` or the scoped `File.with`. All I/O is binary
(no text-mode newline translation); `String` is a byte string, so any
content round-trips.

The handle (`f` below) implements four method groups — **Reader** (`read` /
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

#### `f.read() -> String` / `f.read(n: Long) -> String`

Streaming read from the current position: `read()` returns the rest of
the file, `read(n)` at most `n` bytes (fewer at EOF). For a one-shot
whole-file read without a handle, use `FS.read(path)`.

#### `f.lines() -> Iterator<String>`

Iterate lines, each with its trailing newline stripped (`\n`, `\r\n`,
and `\r` are all recognized). The iterator owns the handle and closes
it when the loop ends or breaks.

```culebra
# doctest: skip
for line in File.open('access.log').lines() {
  if line.contains('ERROR') { inspect(line) }
}
```

#### `f.chunks(n: Long) -> Iterator<String>`

Iterate fixed-size byte chunks of at most `n` bytes (the last may be
shorter). Same close-on-exit contract as `lines()`.

### Writer methods

#### `f.write(data: String) -> Nil`

Write `data` at the current position (raw bytes, no newline
translation). Throws `IOError` on a read-only handle.

#### `f.flush() -> Nil`

Flush buffered writes to the OS.

### Seekable methods

#### `f.seek(offset: Long, whence: String = "set") -> Nil`

Move the cursor. `whence` is `"set"` (from start), `"cur"` (relative),
or `"end"` (from end; use a negative `offset`).

#### `f.tell() -> Long`

Current byte offset.

### Closeable

#### `f.close() -> Nil`

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
inspect("elapsed: {Time.monotonic() - t0} s")
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
if p.hour >= 9 && p.hour < 17 { inspect("business hours") }
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

Both classes wrap a nanosecond count, but they are not interchangeable:
a combination with no meaning — `t1 + t2`, `t < one_hour`, or a bare
number on either side — raises a `TypeError` naming what it got instead
of quietly doing the arithmetic. Equality is the exception: `==` against
any other type is `false`, never an error.

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
inspect(Random.int(0, 10))        # 0..9
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
after it** is captured as `argv`.
culebra's own flags (`--jit`, `--debug`, …) must precede the script
path. Empty when the script is invoked with no trailing arguments or
when running in the REPL.

```culebra
# doctest: skip
# $ culebra run.cul hello world
inspect(Sys.argv)        # ['hello', 'world']
# $ culebra --jit run.cul hello   →  ['hello']   (--jit is culebra's)
```

A standalone `--` is an optional escape hatch: it stops flag parsing,
so the next argument becomes the script even if it begins with a dash
(e.g. `culebra -- -weird.cul`). It is otherwise unnecessary.

A binary built by `culebra build` reports the arguments it was run
with, the same way and with the same skip of the program name. The
value is process-wide, so an [isolate](#12-isolate) or an HTTP handler
running on a worker thread reads the same array as the main thread.

### `Sys.exit(code: Long) -> Nil`

Terminate the process immediately with the given exit code. Does
not return; pending `defer` statements are *not* run.

```culebra
# doctest: skip
if error_occurred { Sys.exit(1) }
```

### `Sys.env(name: String) -> String`

Return the value of the environment variable `name`, or `''` (empty
string) if it is not set. Use `!v.empty()` to distinguish an unset
variable from one set to the empty string.

```culebra
# doctest: skip
inspect(Sys.env('HOME'))          # '/Users/alice'
inspect(Sys.env('NOT_A_VAR'))     # ''
```

### `Sys.set_env(name: String, value: String) -> Nil`

Set the environment variable `name` to `value`, overwriting any existing
value. The change is visible to this process (via `Sys.env`) and to child
processes spawned afterwards (e.g. `Proc.run`). Raises `IOError` on failure
(for example, an invalid variable name).

```culebra
Sys.set_env('CULEBRA_MODE', 'fast')
inspect(Sys.env('CULEBRA_MODE'))  # => 'fast'
```

### `Sys.getcwd() -> String`

Return the absolute path of the current working directory. Symlinks in the
path are resolved. Raises `IOError` if the directory cannot be determined
(for example, it was removed out from under the process).

```culebra
# doctest: skip
inspect(Sys.getcwd())             # '/Users/alice/project'
```

### `Sys.chdir(path: String) -> Nil`

Change the process working directory to `path`. Raises `IOError` if the path
does not exist or is not a directory.

```culebra
# doctest: skip
Sys.chdir('/tmp')
inspect(Sys.getcwd())             # '/tmp' (or its resolved path)
```

### `Sys.executable -> String`

Absolute path to the running culebra binary. Use it to launch a worker copy of
the interpreter — e.g. `Proc.run([Sys.executable, "worker.cul"], ...)` — instead
of relying on `culebra` being on `PATH`. (In an AOT-built program it is the path
to that standalone binary.)

```culebra
# doctest: skip
inspect(Sys.executable)           # '/usr/local/bin/culebra'
```

### `Sys.script -> String?`

Absolute path of the running script — the `__file__` analogue. Use it to resolve
files next to the script instead of relying on the current working directory:
`FS.join(FS.dirname(Sys.script), "data.txt")`. It is `nil` when there is no
source file at runtime — the REPL, a piped `stdin`, or an AOT-built binary (which
carries no `.cul`; use `Sys.executable` there).

```culebra
# doctest: skip
inspect(Sys.script)               # '/Users/alice/project/build.cul'
```

### `Sys.time() -> Float`

Seconds elapsed on a **monotonic** clock, anchored at the first call in
the process. It never jumps backwards with a wall-clock adjustment, which
makes it the right tool for timing a block of code; use `Time.now()` (§5)
when you need an actual date. `Time.monotonic()` (§5) reads the same
monotonic clock; each function anchors its own origin at its first
call, so subtract two readings of the *same* function rather than
mixing the two.

```culebra
let t0 = Sys.time()
let sum = range(1000).reduce(0, |a, x| a + x)
inspect(Sys.time() - t0 >= 0.0)   # => true
```

### `GC` — heap introspection

`GC.stat()` runs a full collection and returns an `Object` describing the
live heap right after it:

| key | type | meaning |
|---|---|---|
| `live_objects` | `Long` | number of reachable heap objects |
| `rc_objects` | `Long` | reachable *refcounted* objects (excludes traced-only Strings/StringViews) |
| `heap_bytes` | `Long` | bytes those objects occupy |

Because it collects first, the numbers report *reachable* state, not cycle
residue still awaiting sweep. The call itself allocates the result `Object`,
so back-to-back readings differ by a small constant — measure a delta around
the code under test rather than an absolute count.

```culebra
# doctest: skip
let base = GC.stat().live_objects
build_some_structure()
inspect(GC.stat().live_objects - base)   # objects retained by the structure
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
let A = Tensor.from([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])   # [2, 3]
let B = Tensor.from([[1.0, 0.0], [0.0, 1.0], [1.0, 1.0]]) # [3, 2]
let C = A.dot(B) + 1.0                # lazy: builds the graph only
Tensor.eval(C)                        # BLAS GEMM runs here
inspect(C.shape())                    # => [2, 2]
inspect(C.to_array())                 # => [[5.0, 6.0], [11.0, 12.0]]
```

### Construction (namespace functions)

#### `Tensor.zeros(...) -> Tensor` / `Tensor.ones(...)` / `Tensor.randn(...)`

The shape is variadic (`Tensor.zeros(3, 4)`) or an Array
(`Tensor.zeros([3, 4])`). The dtype is a string tag placed as the
**first argument**, Julia-style. `"f32"` is the only dtype (float64
has no fast path on GPU backends):

```culebra
let a   = Tensor.zeros(3, 4)              # F32 default
let a32 = Tensor.zeros("f32", 3, 4)       # explicit
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

#### `Tensor.concat(parts: Array) -> Tensor`

Stacks tensors along axis 0 (rows) into one materialized Tensor. Every
part must share the same dtype and the same dims past axis 0; the
result's row count is the sum of the parts'. Differentiable — the
gradient slices back into each part's row range.

```culebra
let a = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
let b = Tensor.from([[5.0, 6.0]])              # [1, 2]
let c = Tensor.concat([a, b])                  # [3, 2]
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
W2 -= d2.dot(a1.transpose()) * lr
b2 -= d2.sum(1).reshape([N_OUT, 1]) * lr
W1 -= d1.dot(xb.transpose()) * lr
b1 -= d1.sum(1).reshape([N_HID, 1]) * lr
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
let l = p.log()            # natural log, elementwise
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
| `.item() -> Float` | eager | extract the lone element as a Float; throws unless the tensor holds exactly one element (any rank) |

`.item()` is the scalar exit point, complementing `.to_array()` (which is for
shaped data): use it to read a loss or any single-element result without
reshaping. `loss.item()` replaces `to_float(loss.to_array()[0])`.

### Autograd (reverse-mode)

The Tensor primitive carries a native reverse-mode autodiff engine: the
forward graph doubles as the tape, and `.backward()` walks it in C++. No
script-level wrapper is needed — the same op that computes a value also
knows its vector-Jacobian product. The tape is recorded only for ops that
feed a `requires_grad` leaf; forward-only work (inference, or a training
loop that writes its own backward) records none, so it costs no more than
the underlying tensor library.

| Method | Returns | Description |
|---|---|---|
| `.requires_grad() -> Tensor` | self | mark a leaf to accumulate grad; chainable |
| `.backward() -> Nil` | — | seed `dL/dself = 1` and propagate to every leaf |
| `.grad() -> Tensor` | Tensor | the accumulated gradient (zeros until `backward`) |
| `.zero_grad() -> Nil` | — | clear the gradient before the next step |
| `.detach() -> Tensor` | Tensor | a materialized copy with no graph and no grad |

`requires_grad` propagates forward: any op with a grad-tracking input
produces a grad-tracking output. Differentiable ops include `+ - * /`,
`.pow()` (w.r.t. the base), `.dot()`, axis `.sum()` / `.mean()`,
`.relu()`, `.sigmoid()`, `.softmax()`, `.log()`, `.transpose()`,
`.reshape()`, `.slice()`, and `Tensor.concat()`. Gradients un-broadcast
automatically, so a bias added across a batch sums back to its shape.

```culebra
let w = Tensor.from([[2.0, 0.0], [0.0, 3.0]]).requires_grad()
let x = Tensor.from([[1.0], [1.0]]).requires_grad()
let y = w.dot(x)              # [2, 1]
let loss = (y * y).sum(0).sum(0)
loss.backward()
Tensor.eval(w.grad(), x.grad())
let gw = w.grad().to_array()  # dL/dw
```

A typical training step zeroes grads, runs the forward, calls
`.backward()`, reads `.grad()` for the optimizer update, then `.detach()`
on the new weights to start the next step from a clean leaf. The
`benchmarks/microgpt/microgpt_tensor.cul` transformer is a full worked
example (embeddings, attention with a KV cache, RMSNorm, MLP,
cross-entropy, Adam) built entirely on these methods.

`.backward()` reads gradients from the forward buffers, so it implies a
`Tensor.eval` of the loss. `.grad()` returns a Tensor like any other —
materialize it with `Tensor.eval` before `.to_array()`.

`Tensor.no_grad(fn) -> Any` runs `fn` with grad tracking suppressed:
ops inside build no autograd graph (so no tape and no `requires_grad`
flow), and the call returns whatever `fn` returns. Use it for inference
or any forward you will not backprop through.

```culebra
# doctest: skip
let logits = Tensor.no_grad(fn () { model_forward(x) })
```

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

- dtype is F32 only (float64 has no fast path on GPU backends; scalar
  entry/exit points like `.item()` / `.to_array()` surface `Float`).
- `.dot()` is rank-2 only. Batched 3D+ matmul is future work.
- `.reshape()` requires contiguous input (a post-`transpose` reshape
  must materialize first — currently go through
  `Tensor.from((...).to_array())` explicitly).
- `.softmax()` also requires contiguous input.

### Backends and device selection

Evaluation is delegated to the vendored `cpp-tensorlib` engine
(`vendor/cpp-tensorlib`), which owns the lazy graph, kernel fusion,
and the device backends:

- **CPU** — vectorized kernels (AVX2 / NEON); Accelerate supplies the
  BLAS-shaped kernels on macOS.
- **GPU** — Metal on macOS, CUDA on Linux / Windows.

The device is process-global (shared by interpreter, JIT, and AOT) and
switchable at runtime:

| Function | Effect |
| --- | --- |
| `Tensor.use_cpu() -> Nil` | evaluate every op on the CPU |
| `Tensor.use_gpu() -> Nil` | evaluate on the GPU backend |
| `Tensor.use_auto() -> Nil` | choose per op by problem size (the default) |
| `Tensor.gpu_available() -> Bool` | whether a GPU backend is compiled in and reachable |
| `Tensor.device() -> String` | the selection in effect: `'cpu'`, `'gpu'` or `'auto'` |

```culebra
inspect(type_of(Tensor.gpu_available()))    # => 'Bool'
inspect(Tensor.device())                    # => 'auto'
```

`device()` reports the selection, not where a given op ran — under
`'auto'` that is decided per op, and an op sent to a GPU that turned
out to be unreachable still lands on the CPU.

`use_auto` is the default because small tensors lose to kernel-launch
overhead: it keeps those on the CPU and sends only the ops big enough
to pay for the trip. Calling `use_cpu()` / `use_gpu()` anywhere —
including before the first tensor exists — pins every later op to that
device instead.

`use_gpu()` on a build with no reachable GPU falls back to the CPU
path rather than throwing, so a program stays portable; check
`gpu_available()` when the choice matters. Storage stays F32 on every
device (see the dtype constraints above).

Metal needs nothing at build time. CUDA is compiled in when `nvcc` is
found (`CULEBRA_TENSOR_CUDA=AUTO`, the default; `ON` turns a missing
`nvcc` into a configure error and `OFF` skips the backend). The CUDA
driver itself is loaded at run time, so a CUDA-enabled binary still
runs on a machine with no GPU — `gpu_available()` just reports
`false`.

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

### `JSON.parse(s, lines=false, number_mode='auto', jsonc=false) -> Any`

Parse a JSON string into a Culebra value.

* `number_mode='auto'` (default): integers (no decimal point or
  exponent) read as `Long`; everything else as `Float`.
* `number_mode='float'` reads every number as `Float` — useful for
  round-trip safety when the producer treats numbers uniformly.
* `lines=true` parses **JSON Lines**: split `s` on `\n`, parse each
  non-empty line, return an `Array` of the per-line values.
* `jsonc=true` parses **JSONC**: tolerate `//` line comments, `/* */`
  block comments, and trailing commas in objects and arrays — for
  reading existing config files (`tsconfig.json`, VSCode `settings.json`)
  without stripping them first. The default is strict JSON, which rejects
  comments and trailing commas with a `ValueError`.

Malformed input raises `ValueError` and the structured Error Object
exposes the JSON-internal position via `e.line` / `e.col` (both
1-based, pointing at the offending character):

```culebra
let r = try { JSON.parse('{"a": ,}'); nil } catch e { e }
inspect(r.message)           # => 'JSON.parse: expected value'
inspect("{r.line}:{r.col}")  # => '1:7'
```

Examples:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
# The default is compact; `sort_keys` orders the keys alphabetically.
inspect(JSON.stringify(v))                  # => '{"name":"alice","age":30,"tags":["admin","staff"]}'
inspect(JSON.stringify(v, sort_keys: true)) # => '{"age":30,"name":"alice","tags":["admin","staff"]}'
let back = JSON.parse(JSON.stringify(v))
inspect(back.name)                          # => 'alice'
let arr = JSON.parse("1\n2\n3\n", lines: true)
inspect(arr)                                # => [1, 2, 3]
let cfg = JSON.parse('{
  // comments and trailing commas are allowed
  "port": 8080,
}', jsonc: true)
inspect(cfg.port)                           # => 8080
```

`indent` pretty-prints and `lines` emits JSON Lines; both produce
multi-line output:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
inspect(JSON.stringify(v, indent: 2))
inspect(JSON.stringify([1, 2, 3], lines: true))
# => |
# '{
#   "name": "alice",
#   "age": 30,
#   "tags": [
#     "admin",
#     "staff"
#   ]
# }'
# '1
# 2
# 3
# '
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
Positionals are matched in spec order and every one of them is
required. A `default` makes the argument an **option** instead, given
by its long form (`--encoding utf-8`); `short` adds the one-letter form
(`-l`). An optional *positional* is therefore not expressible — an
argument that may be omitted carries a `default` and is spelled
`--name value`.

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
inspect(args.input)            # String
if args.lines { inspect("lines: ...") }
if args.words { inspect("words: ...") }
inspect("encoding: {args.encoding}")
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
  command itself is killed, not any grandchildren it spawned. A process
  that closes stdout/stderr but keeps
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
  IO.inspect("on branch " + r.stdout.trim())
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
IO.inspect("server exited via " + (r.signal ?? to_string(r.code)))

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
for h in handles { total += h.join() }
total                          # => 45
```

### Cancellation

An isolate is cooperatively cancellable. Dropping its handle without `join()`
(or letting the GC collect it) signals the isolate to stop; it unwinds at its
next statement or blocking-channel boundary, so a runaway or idle isolate never
hangs the program.

### Channels — `Channel.new(cap = 1) -> (tx, rx)`

A channel is a bounded, blocking queue for passing values between isolates. It
returns a **(tx, rx)** pair: `tx.send(v)` enqueues, `rx`
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
inspect("shutting down…")
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

Same buffer, backed by a memory-mapped file (POSIX `mmap(MAP_SHARED)`, Windows
`CreateFileMapping`). Writes go to the file's pages — **persistent** (the file
outlives the process) and visible to any other process that maps the same
`path`. The handle gains a `flush()` method (flush the dirty pages to disk); the
file is an ordinary file you remove with `FS.remove(path)`. Pointing `path` at a
RAM-backed location (e.g. `/dev/shm/...` on Linux) gives shared memory without
disk durability.

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let buf = SharedBuffer.file("/tmp/grid.bin", 100, Cell)
buf[0].v = 42
buf.flush()                   # durable on disk
```

#### `SharedBuffer.shared(count, Class) -> buffer`

Same buffer, backed by **anonymous** shared memory (a name-less fd — `memfd` on
Linux, an immediately-unlinked POSIX shm object on macOS; a pagefile-backed
`CreateFileMapping` on Windows). It touches no disk; the kernel frees it once
every handle is dropped. Its purpose is to be handed to a **child process** via
`Proc.run` / `Proc.spawn` `share:`
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
inspect(buf.size)                # => 3
buf[0].x = 1.5                # writes the bytes in place
let v = buf[0]                # a stored view aliases the same element
v.y = 2.5
inspect([buf[0].x, buf[0].y])    # => [1.5, 2.5]
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
inspect(grid[0].v)               # the child's write, read back here
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
inspect(tally[0].n)              # 8000 exactly — no lost updates
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
  raise `TypeError`: there is no implicit truthiness, so empty strings
  and arrays are not falsy.
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
inspect(r.kind)         # => 'AssertionError'
inspect(r.message)
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

Linear-time, grapheme-aware regular expressions (engine: vendored
[cpp-regexlib](https://github.com/yhirose/cpp-regexlib)).
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
pass through verbatim. For a pattern that also
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
hide the `compile` step. Reusing one
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
| `Regex.replace_all(pat, s, repl)` | `String` — template or `fn (Match) -> String` repl, every match |
| `Regex.replace_first(pat, s, repl)` | `String` — same `repl`, only the leftmost match |

```culebra
inspect(Regex.find('(\d+)', "ab12")[1])            # => '12'
inspect(Regex.test('(?i)hello', "HELLO"))          # => true
inspect(Regex.replace_all('[;；]', "a;b；c", "、"))  # => 'a、b、c'
# A miss is nil, so it composes with `?.` and `??`:
inspect(Regex.find('x', "y")?.value ?? "none")     # => 'none'
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
| `re.replace_all(s, repl)` | `String` — `repl` is a template (`$1` / `$<name>` / `$$`) **or** a `fn (Match) -> String`, every match |
| `re.replace_first(s, repl)` | `String` — same `repl` grammar, only the leftmost match; unchanged if there is no match |
| `re.split(s)` | `[String]` — split `s` on matches |

**Choosing a bulk API.** `find_all` builds a full `Match` object (text, spans,
`groups`, `named`) per match; on match-dense input that object construction —
not the matching — dominates, costing tens of times more than the engine's raw
scan. When you do not need per-match captures, reach for the lean variant:
`count` when you only need the number, `find_all_index` for byte spans,
`find_all_str` for the matched texts, or `find_iter` when you stop early.
Keep `find_all` for when you actually consume `groups` / `named` per match.

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
inspect(d.test("abc 123"))                                # => true
inspect(Regex.compile('\w+').find("  hello world").value) # => 'hello'
inspect(d.find("no digits"))                              # => nil
inspect(d.find_all("a1 b22 c333").size())                 # => 3
```

Captures reach by position (`m[1]`) and by name (`m["year"]`); `m[0]` is
the whole match and a miss reads as `nil`. The `Group` objects under
`m.groups` / `m.named` carry spans as well as the text:

```culebra
let m = Regex.compile('(?<year>\d{4})-(\d{2})').find("2026-05")
inspect(m[1])                    # => '2026'
inspect(m["year"])               # => '2026'
inspect(m[0])                    # => '2026-05'
inspect(m[9] ?? "none")          # => 'none'
inspect(m.groups[1].value)       # => '2026'
inspect(m.named["year"].value)   # => '2026'
```

Replacing and splitting — a replacement is either a `$n` template or a
function of the `Match`. `replace_all` replaces every match; `replace_first`
replaces only the leftmost one and leaves the rest of the string untouched
(a no-op — returns `s` unchanged — if there is no match):

```culebra
let d = Regex.compile('\d+')
inspect(d.replace_all("a1 b22 c333", "#"))                        # => 'a# b# c#'
inspect(d.replace_first("a1 b22 c333", "#"))                      # => 'a# b22 c333'
inspect(Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1')) # => 'y.x'
inspect(d.replace_all("a1 b22", fn (m) { "<{m.value}>" }))        # => 'a<1> b<22>'
inspect(Regex.compile('\s+').split("the quick  brown"))           # => ['the', 'quick', 'brown']
inspect(Regex.compile('hello', "i").test("HELLO world"))          # => true
```

`find_iter` is lazy, so a scan can stop early — `for m in d.find_iter(s) { break }`
walks no further than the match it broke on:

```culebra
let d = Regex.compile('\d+')
inspect(d.find_iter("1 2 3").take(2).collect().size())   # => 2
inspect(Regex.escape("a.b(c)"))                          # => 'a\.b\(c\)'
```

The supported syntax (literal / `.` / character classes / `* + ? {n,m}` greedy
and lazy / `|` / capturing and named groups / `\d \w \s \b` / lookahead /
variable-length lookbehind / `\p{…}` Unicode properties) and the full matching
model and resource limits are documented in the vendored engine,
[cpp-regexlib](https://github.com/yhirose/cpp-regexlib) (`vendor/cpp-regexlib`).

---

## 15. `Http`

Synchronous HTTP/HTTPS client (engine: vendored `cpp-httplib` + OpenSSL,
statically linked). Each call **blocks** until the response arrives — there is
no async/await. The same namespace also serves: `Http.server()` below carries
routes, static files, and WebSocket, and `Http.sse` / `Http.ws` speak the two
streaming protocols as a client. TLS is automatic for `https://` URLs; the system trust store is
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
| `Http.client(base_url, headers=nil, timeout=0, follow_redirects=true)` | a persistent client handle (base URL + default headers + connection reuse); see below |
| `Http.server()` | an HTTP server handle (register routes + `static` + `ws`, then `listen`); see below |
| `Http.ws(url)` | connect a WebSocket client; returns a handle (`send`/`receive`/`for`/`close`/`is_open`); see below |

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
- `files: Object` (`post` / `put` / `request` only) — a `multipart/form-data`
  body (text fields and file parts, with streaming); see Multipart below. At most
  one of `body` / `json` / `form` / `files` may be given (else `TypeError`).
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
  IO.inspect(users.size().to_string())
} else {
  IO.inspect("request failed: {r.status}")
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
Http.get("https://example.com/big.csv", into: fn (chunk) { bytes += chunk.size() })

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
  !chunk.empty() ? chunk : nil            # nil signals end-of-stream
}, content_type: "application/octet-stream")
```

The producer runs on the calling thread (it may mutate captured state); if it
throws, the upload is aborted and the error propagates. A non-`String`/non-`nil`
return is a `TypeError`.

**Multipart uploads — the `files:` argument.** Pass `files:` (on `post` / `put` /
`request`) to send a `multipart/form-data` body; the `Content-Type` (with a
generated boundary) is set for you. `files:` is an `Object` whose each entry is
one part, keyed by field name. A part value is:

* a **`String`** — a plain text field.
* an **`Object`** with exactly one body source — `content:` (a `String` held in
  memory), `path:` (a `String` file path, streamed from disk), or `stream:` (a
  producer `Function`, streamed) — plus optional `filename:` and
  `content_type:`. A `path:` part defaults `filename` to the file's base name.
* an **`Array`** of any of the above — repeated parts under the same field name
  (e.g. multiple files in one `photos` field).

A part that uses `path:` or `stream:` is streamed chunked, so a large file or a
slow-to-produce part never has to live in memory all at once. A `stream:`
producer follows the same contract as a streaming `body:` — return the next
chunk `String`, or `nil` to end the part.

```culebra
# doctest: skip
# text field + an in-memory file part
Http.post(url, files: {
  title: "My report",
  doc:   { content: "a,b,c\n1,2,3\n", filename: "data.csv", content_type: "text/csv" },
})

# a large file streamed straight from disk (never buffered whole)
Http.post(url, files: { clip: { path: "/movies/big.mp4", content_type: "video/mp4" } })

# a slow-to-produce part streamed from a producer
mut row = 0
Http.post(url, files: {
  export: { filename: "rows.csv", content_type: "text/csv", stream: fn () {
    row += 1
    row <= 1000 ? "{row},{compute(row)}\n" : nil
  } },
})

# repeated parts under one field name via an Array
Http.post(url, files: {
  caption: "trip",
  photos:  [ { path: "./1.jpg" }, { path: "./2.jpg" } ],
})
```

A part value that isn't a `String` / `Object` / `Array`, an `Object` without
exactly one of `content` / `path` / `stream`, a non-`String` `content` / `path`,
or a non-`Function` `stream` is a `TypeError`; a `path` that can't be opened is
an `IOError`.

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

### `Http.client(base_url, headers=nil, timeout=0, follow_redirects=true) -> Object`

A persistent client that **reuses one keep-alive connection** across requests and
carries a **base URL** and **default headers** — for hitting the same API many
times without repeating the host, auth header, and re-doing the TLS handshake
each call. Returns a handle with `get` / `post` / `put` / `delete` / `head` /
`request` (the same kwargs as the free `Http.*` functions, but the first
argument is a path joined onto `base_url`) plus `close`.

| argument | meaning |
|---|---|
| `base_url` | scheme + host (+ optional path prefix), e.g. `https://api.example.com/v1` |
| `headers` | default headers layered under every request's (per-request wins per key) |
| `timeout` / `follow_redirects` | connection-level defaults (not per-request) |

```culebra
# doctest: skip
let api = Http.client("https://api.example.com/v1",
                      headers: {Authorization: "Bearer " + token},
                      timeout: 30)

let me   = api.get("/me").json()              # → GET https://api.example.com/v1/me
let user = api.get("/users/42").json()        # reuses the same connection
api.post("/users", json: {name: "alice"})     # Authorization header rides along

api.get("/items", headers: {"Idempotency-Key": k})   # merged over the defaults
api.close()                                    # release the connection
```

A request method's first argument is a path: a leading-slash or bare relative
path is joined onto `base_url` (`/me` → `…/v1/me`); an absolute URL (with its own
scheme) bypasses `base_url` but still gets the default headers. Per-request
`headers` are merged over the client's defaults (a per-key match overrides).
`close()` releases the connection; the GC also closes a client that goes out of
scope (so an explicit `close` is optional). A request after `close` is an
`HttpError`. The handle is non-sendable (one connection, one thread): to fan out,
make a client per isolate.

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

### `Http.server() -> Object`

An HTTP server. Register routes with `get`/`post`/`put`/`delete`/`patch`/`options`,
serve files with `static`, then `listen` to accept connections. Each handler is a
`fn(req) -> response` closure.

```culebra
# doctest: skip
let srv = Http.server()
srv.get("/", fn(req) { "Hello, world!" })
srv.get("/users/:id", fn(req) { "user " + req.params["id"] })
srv.post("/echo", fn(req) { req.body })
srv.get("/json", fn(req) {
  { status: 201, body: '{"ok":true}', content_type: "application/json",
    headers: {"X-Trace": req.headers["X-Request-Id"]} }
})
srv.static("/assets", "./public")
srv.listen(8080)                 # blocks; Ctrl+C to stop
```

| Method | Effect |
| --- | --- |
| `get/post/put/delete/patch/options(pattern, handler)` | register `handler` (a `fn(req)->response`) for that method and route `pattern`; returns the server (so calls chain) |
| `static(mount, dir)` | serve static files at the URL prefix `mount`; `dir` is a String path (a live on-disk directory) or an `Embed.dir(...)` descriptor (baked into the binary under AOT — see [Embed](#embed)) |
| `sink.write(chunk)` | (inside a `stream:` closure) push one chunk; returns `false` if the client has disconnected |
| `bind(port, host="0.0.0.0") -> Long` | open the listening socket and return the port it got; `port=0` asks the OS for an ephemeral one. Once only, and not after the server has served |
| `serve(workers=0)` | run the accept loop on a bound socket until interrupted (blocks the calling thread). Handlers run on a worker pool, never on the accept loop, so a slow handler can't block accepting new connections — and handlers must be **Sendable**. `workers=0` (default) picks a CPU-scaled pool size; pass a positive count to fix it |
| `serve_async(workers=0)` | same, but serve on a background pool and return immediately; stop with `stop()` |
| `listen(port, host="0.0.0.0", workers=0)` | `bind` + `serve` in one call. Returns only once stopped |
| `listen_async(port, host="0.0.0.0", workers=0) -> Long` | `bind` + `serve_async`; returns the bound port |
| `stop()` | stop a background (`listen_async`) server and join its thread (can be called from another thread) |
| `close()` | stop serving and release the server (the GC also closes one that goes out of scope) |

**The request `req`** is an object with `method`, `path`, and `body` (Strings), and
`headers`, `query` (the parsed query string), and `params` (matched route path
parameters, e.g. `:id` → `req.params["id"]`) as objects of String — Express-style
names. A missing key reads as `nil`.

**The handler's return value** becomes the response:

- a `String` → `200`, `text/plain`, that string as the body;
- an `Object` `{status?, body?, headers?, content_type?}` → full control (absent
  fields default to `200` / `""` / `text/plain`); `headers` is an object of String;
- an `Object` with a `stream:` Function → a chunked (streaming) response — see below;
- `nil` → `200` with an empty body.

A handler that raises becomes a `500` whose body is the error message. An unmatched
route is a `404`.

**Streaming responses (SSE / chunked).** To send a response incrementally —
without buffering the whole body — return an Object whose `stream` field is a
`fn(sink)` closure. The body is sent chunked; `status`, `content_type`, and
`headers` apply as usual (`body` and `stream` together is a `TypeError`). The
closure is called with a `sink` handle; `sink.write(chunk)` pushes one chunk and
returns `false` if the client has gone away (so a long loop can stop early):

```culebra
# doctest: skip
srv.get("/events", fn(req) {
  { content_type: "text/event-stream",
    headers: {"Cache-Control": "no-cache"},
    stream: fn(sink) {
      for i in 0..10 { sink.write("data: " + i.to_string() + "\n\n") }
    } }
})
```

The stream closure runs on the worker thread, so under `workers: 1` a long-lived
stream occupies the single thread (use `workers: N` for concurrent streams).
An exception raised mid-stream aborts the connection (the status line is already
sent, so it cannot become a `500`); wrap the body in a `try` if you need to handle
it.

**WebSocket — `srv.ws(pattern, fn(req, ws))`.** Register a WebSocket route; the
handler runs a long-lived loop for the connection, holding one worker until it
returns. The `ws` handle reads and writes messages:

| Method | Effect |
| --- | --- |
| `for msg in ws { … }` | iterate inbound messages (each a `String`); ends when the peer closes |
| `ws.receive()` | the next inbound message as a `String`, or `nil` when the peer closes |
| `ws.send(msg)` | send a text message; returns `false` if the peer has gone away |
| `ws.close()` | close the connection |
| `ws.is_open()` | whether the connection is still open |

```culebra
# doctest: skip
srv.ws("/echo", fn(req, ws) { for msg in ws { ws.send(msg) } })
srv.ws("/chat", fn(req, ws) {
  while true {
    let m = ws.receive()
    if m == nil { break }              # peer closed
    ws.send(req.path + ": " + m)
  }
})
```

A WebSocket connection occupies its worker for its whole lifetime, so `workers: N`
sets the number of simultaneous WebSocket connections (with `workers: 1` a single
connection blocks the server). Connect to a server from culebra with
[`Http.ws`](#httpwsurl---object). The server now covers request/response, route
parameters, static files, streaming, and WebSocket.

**Concurrency.** Handlers always run on a pool of worker threads — never on the
accept loop — so a slow handler can't block accepting new connections, and a
browser (which opens several connections in parallel) isn't serialized. `workers`
sets the pool size:

- `workers: 0` (default) — a CPU-scaled pool (at least 4, at most 8). At least 4
  because a browser opens several connections to load a page and each keep-alive
  connection holds a worker briefly; capped because each worker carries its own
  runtime.
- `workers: N` — a fixed pool of N. Requests are handled in **true
  parallel**: no global interpreter lock stands between them.

Each worker has its own runtime, so handlers must be **Sendable**: they can't
capture mutable variables or non-Sendable values. Share large read-only data with
[`Shared.new`](#12-isolate) (one copy across all workers) and open per-worker
resources (a DB connection) inside the handler. A non-Sendable handler is a
`SendError` at `listen`, naming the offending route. (To keep mutable state, route
it through `Shared` / a channel / a hub — the same rules as `Isolate`.)

```culebra
# doctest: skip
let model = Shared.new(load_weights())          # one read-only copy, shared by all workers
let srv = Http.server()
srv.post("/predict", fn(req) { infer(model, req.body) })
srv.listen(8080, workers: 8)                     # 8 handlers run in parallel
```

**Background servers — `listen_async` + `stop`.** To serve while the main thread
does other work — e.g. behind a GUI — use `listen_async`, which serves on a
background thread and returns immediately; `stop()` halts it (and can be called
from another thread). The server keeps running only while you hold the handle, so
keep `srv` referenced for as long as it should serve. Because the handlers run off
the calling thread, they must be **Sendable** (as with `workers > 1`). A bind
failure is reported synchronously as an `HttpError`. A server is single-use:
once it has served, starting it again is an `HttpError` — create a new
`Http.server()` to serve again.

```culebra
# doctest: skip
let srv = Http.server()
srv.get("/health", fn(req) { "ok" })
srv.listen_async(8080, workers: 4)   # returns immediately
# … do other work; call Http.get("http://127.0.0.1:8080/health") …
srv.stop()                           # stop and join the background thread
```

Alternatively, a blocking `listen` inside an isolate also works — dropping the
isolate (or `Ctrl+C`) stops the accept loop:

```culebra
# doctest: skip
let srv_iso = Isolate.spawn(fn() {
  let srv = Http.server()
  srv.get("/health", fn(req) { "ok" })
  srv.listen(8080, workers: 4)
})
# … use Http.get("http://127.0.0.1:8080/health") from the main thread …
srv_iso.drop()                   # signals the server to stop, then joins
```

**Knowing when it is up, and on which port — `bind` + `serve`.** A blocking
`listen` never returns, so it can report nothing: neither "the socket is open"
nor, for a `port 0` bind, which port the OS chose. Splitting the two gives a
point between them where both are known. `bind` returning **is** the readiness
signal — the socket is open with a backlog, so a connection made after it is
queued by the kernel even before `serve` starts accepting.

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
let srv_iso = Isolate.spawn(fn() {
  let srv = Http.server()
  srv.get("/health", fn(req) { "ok" })
  tx.send(srv.bind(0))           # 0 = any free port; hand the number out
  tx.drop()
  srv.serve()                    # blocks here
})
tx.drop()                        # the parent's own sender copy
let base = "http://127.0.0.1:" + rx.recv().to_string()
inspect(Http.get(base + "/health").body)     # => 'ok'
srv_iso.drop()
```

Nothing about that is channel-specific — the port is a plain `Long`, so log it,
put it in a `Shared`, or write it to a file. It is also what a handler must
capture to know its own address: the server handle is **non-Sendable**, so a
handler that reads `srv` is rejected, while a captured `Long` copies fine.

```culebra
# doctest: skip
let srv = Http.server()
let port = srv.bind(0)
Log.info("http server listening", { port: port })
srv.get("/whoami", fn(req) { "http://127.0.0.1:" + port.to_string() })
srv.serve(workers: 4)
```

On the calling thread `listen_async` is enough on its own — it returns the bound
port, so the split is only needed where the call blocks. Routes may be registered
either side of `bind`; only `serve` needs them in place. A `serve` on an unbound
handle, a second `bind`, and a `listen` after `bind` are each a catchable
`HttpError`, and leave the recorded routes intact for a later attempt. A bind
that fails leaves the handle unbound rather than spent, so another port can be
tried.

### `Http.ws(url) -> Object`

Connect a WebSocket client to `url` (`ws://host:port/path`) and return a handle
with the same shape as the server-side `ws`: `send(msg)`, `receive()` (a `String`,
or `nil` once the peer closes), `for msg in ws`, `close()`, and `is_open()`. A bad
URL or a failed connect is an `HttpError`.

```culebra
# doctest: skip
let ws = Http.ws("ws://127.0.0.1:8080/echo")
ws.send("hello")
inspect(ws.receive())               # => the echoed message
for msg in ws { handle(msg) }    # drains messages until the server closes
ws.close()
```

### Embed

`Embed.dir(name)` returns a directory descriptor for `srv.static(mount, ...)`
that resolves *per backend*, with no code change:

- **Run from source** (interpreter / JIT): it serves the live on-disk directory
  `name`, resolved relative to the entry script — so editing a file and
  reloading shows the change immediately (a real dev loop).
- **`culebra build`** (AOT): the directory is walked at build time and its bytes
  are baked into the executable; the binary serves them with no external files.
  The build prints what it embedded (`embedded N file(s) (… bytes) from '…'`).

```culebra
# doctest: skip
let srv = Http.server()
srv.static("/", Embed.dir("dist"))     # whole frontend, one line
srv.get("/api/ping", fn(req) { '{"ok":true}' })
srv.listen(8080)
```

`name` must be a string literal so the AOT build can find and bake it; a
computed path still works from source but isn't embedded. The Content-Type is
inferred from each file's extension, a request for a directory (or `/`) serves
its `index.html`, and a path not in the directory falls through to the
registered routes (so an API route always wins). `Embed.dir` is independent of
`Http` — it produces a plain descriptor any consumer could serve.

`culebra build` compiles the baked assets against the culebra headers, so it
needs a source checkout: the one the binary was built from by default, or
`$CULEBRA_HOME` when that is set. With neither it stops with an error rather
than producing a binary.

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
inspect(Encoding.html.escape("a & b < c"))          # => 'a &amp; b &lt; c'
inspect(Encoding.html.escape("it's fine"))          # => 'it&#39;s fine'
inspect(Encoding.html.unescape("Tom &amp; Jerry"))  # => 'Tom & Jerry'
inspect(Encoding.html.unescape("caf&eacute; &mdash; x")) # => 'café — x'
inspect(Encoding.html.unescape("&#65;&#x42;"))      # => 'AB'
inspect(Encoding.html.unescape("&#12354;"))         # => 'あ'
inspect(Encoding.html.unescape("&unknownent;"))     # => '&unknownent;'
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
inspect(Encoding.base64.encode("user:pass"))   # => 'dXNlcjpwYXNz'
inspect(Encoding.base64.decode("dXNlcjpwYXNz")) # => 'user:pass'
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
inspect(Encoding.hex.encode("abc"))   # => '616263'
inspect(Encoding.hex.decode("616263")) # => 'abc'
inspect(Encoding.hex.decode("00FF").size()) # => 2
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
inspect(Encoding.url.encode("a b&c"))   # => 'a%20b%26c'
inspect(Encoding.url.decode("a%20b%26c")) # => 'a b&c'
inspect(Encoding.url.encode("café"))    # => 'caf%C3%A9'
```

---

## 17. `Compress`

gzip / deflate (de)compression, backed by zlib. Every function is binary-safe
(embedded NUL bytes survive a round trip).

| Function | Result |
| --- | --- |
| `Compress.gzip(data: String) -> String` | gzip-compressed bytes (RFC 1952 wrapper); interoperates with the standard `gzip` tool |
| `Compress.gunzip(data: String) -> String` | the decompressed bytes; raises `ValueError` on malformed input |
| `Compress.deflate(data: String, level: Long = -1) -> String` | zlib-compressed bytes (RFC 1950 wrapper) — `gzip` minus its gzip-specific header |

`gunzip` auto-detects the header, so it decompresses both `gzip` and
`deflate` output with the one function — there is no separate `inflate`.
A truncated or unrecognized input raises `ValueError`.

```culebra
let original = "the quick brown fox the quick brown fox the quick brown fox the quick brown fox"
let z = Compress.gzip(original)
inspect(z.size() < original.size())          # => true
inspect(Compress.gunzip(z) == original)      # => true
```

```culebra
# doctest: skip
# Read a .gz file and write one back
let text = Compress.gunzip(FS.read("logs.gz"))
FS.write("out.gz", Compress.gzip(text))
```

`deflate` differs from `gzip` only in the wrapper: no filename/mtime header,
no CRC-32 trailer, just a 2-byte header and an Adler-32 checksum — the format
a PNG's `IDAT` chunk holds, and the smaller choice when the gzip envelope adds
nothing (an in-memory blob, a value embedded in another container).

```culebra
let text = "the quick brown fox the quick brown fox the quick brown fox"
let z2 = Compress.deflate(text)          # zlib wrapper, not gzip's
inspect(Compress.gunzip(z2) == text)     # the same decoder handles both
# => true
inspect(z2.size() < Compress.gzip(text).size())   # no gzip header to pay for
# => true
```

`level` follows zlib's own convention: `-1` (the default) picks zlib's
built-in tradeoff, `0` stores the input with no compression, and `9` spends
the most time for the smallest output. A value outside `-1..9` raises
`ValueError` at the same call.

```culebra
let text = "the quick brown fox the quick brown fox the quick brown fox"
inspect(Compress.deflate(text, level: 9).size() <=
        Compress.deflate(text, level: 0).size())   # => true
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
inspect(Hash.sha256("abc"))
# => 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
inspect(Hash.md5("abc"))
# => '900150983cd24fb0d6963f7d28e17f72'
inspect(Hash.hmac_sha256("Jefe", "what do ya want for nothing?"))
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
| `CSV.parse(text, delimiter=",", header=false, types=nil) -> Array` | rows of String fields, or (with `header`) an Array of Objects |
| `CSV.stringify(rows: Array, delimiter: String = ",") -> String` | CSV text; each row must be an Array, each field rendered like `to_string` |

The `delimiter:` option (a single byte; first byte used) selects the field
separator — pass `"\t"` for TSV. It may be given positionally or by keyword.

By default `parse` is lenient (no errors): every field comes back as a `String`
(numbers are not inferred), an empty input yields no rows, and a trailing newline
does not add an empty final row. Both LF and CRLF end a record. `stringify`
renders each field with the same conversion as `to_string`, so scalars like
numbers and `Bool` serialize naturally; `stringify(parse(text))` round-trips
well-formed input.

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25")
inspect(rows[1])                                         # => ['alice', '30']
inspect(CSV.stringify([["a,b", "c"], [1, 2]]) == "\"a,b\",c\n1,2")   # => true
inspect(CSV.parse("a\tb", delimiter: "\t")[0])           # => ['a', 'b']
```

**Header mode — `header: true`.** The first row names the columns, and each later
row becomes an `Object` keyed by those names (instead of a positional Array):

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25", header: true)
inspect(rows[0]["name"])                                 # => 'alice'
```

A header with no data rows (or empty input) yields `[]`. A duplicate header name,
or a data row whose field count differs from the header, is a `ValueError`.

**Typed columns — `types:`.** Pass `types:` (an `Object` mapping a header name to
`"String"` / `"Long"` / `"Float"` / `"Bool"`) to coerce those columns; columns
not listed stay `String`. Coercion is **explicit, never inferred** — so a value
like a ZIP code or ID declared `String` keeps its exact text (no leading-zero or
precision loss). `types:` requires `header: true`.

```culebra
let rows = CSV.parse("name,age,active\nalice,30,true", header: true,
                     types: {age: "Long", active: "Bool"})
# age is a real Long now (not a String), so arithmetic works:
inspect(rows[0]["age"] + 1)                              # => 31
# a ZIP code stays exact text — no number inference:
let z = CSV.parse("zip\n01234", header: true, types: {zip: "String"})
inspect(z[0]["zip"])                                     # => '01234'
```

An unknown type name, a `types` key that names no column, or a cell that can't be
coerced (e.g. `"hello"` as a `Long`, or an empty cell) is a `ValueError` tagged
with the record number and column. Bool accepts exactly `"true"` / `"false"`;
coercion does not trim surrounding whitespace.

Constraint validation (ranges, regex, allowed sets, uniqueness) is intentionally
*not* built in — that is a separate, general concern. Convert ad-hoc columns with
`to_long` / `to_float` if you don't want a `types:` map.

---

## 20. `Env`

Parse and load dotenv-style `.env` configuration, shared byte-for-byte across
backends.

| Function | Result |
| --- | --- |
| `Env.parse(text: String) -> Object` | parse dotenv text into an `Object` of `String` values (no side effects) |
| `Env.load(path: String = ".env", override: Bool = false) -> Object` | read a file, parse it, set each entry into the process environment, and return the parsed `Object` |

Each entry is one line, `KEY=VALUE`:

- blank lines and lines whose first non-space character is `#` are ignored;
- a leading `export ` is stripped (so shell-style `.env` files load as-is);
- the key is trimmed; a line without `=` or with an empty key is skipped;
- a double-quoted value (`"..."`) honours `\n`, `\t`, `\r`, `\\`, and `\"`
  escapes; a single-quoted value (`'...'`) is raw; an unquoted value is
  trimmed and an inline ` # comment` (a `#` preceded by whitespace) is removed,
  while a `#` not preceded by whitespace stays literal;
- a duplicate key keeps its first position with the last value winning.

Parsing is lenient (malformed lines are skipped). Multi-line values are not
supported. `Env.load` raises `IOError` if the file cannot be opened. By default
it does **not** overwrite a variable already set in the environment (the real
environment wins); pass `override: true` to replace existing values. Loaded
variables are visible via `Sys.env` and inherited by children spawned afterward
(e.g. `Proc.run`).

```culebra
let cfg = Env.parse("# app config\nPORT=8080\nNAME=\"my app\"\nDEBUG=true")
inspect(cfg["PORT"])                 # => '8080'
inspect(cfg["NAME"])                 # => 'my app'
inspect(cfg["DEBUG"])                # => 'true'
```

```culebra
# doctest: skip
Env.load(".env")                  # set vars from ./.env (if present)
inspect(Sys.env("PORT"))
```

To use values as numbers or booleans, convert at the use site with `to_long` /
`to_float` (every value is a `String`).

---

## 21. `UUID`

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
inspect(UUID.v4().size())          # => 36
inspect(UUID.v4() != UUID.v4())    # => true
```

---

## 22. `Term`

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
| `Term.style(fg:, bg:, bold:, dim:, underline:, reverse:) -> String` | an SGR parameter string for a `Screen` cell; `fg`/`bg` take a 256-colour index or an `(r,g,b)` tuple |

Colours adapt to the terminal's **capability level** — `0` none, `1` 16,
`2` 256, `3` truecolour — detected from `isatty`, `NO_COLOR` (present ⇒ off),
`FORCE_COLOR`, `COLORTERM`, and `TERM`. A colour beyond the level is
downsampled (truecolour → nearest 256 → nearest 16), and at level 0 nothing
is emitted, so piped or `NO_COLOR` output stays plain. `Term.level()` reads
the level and `Term.set_level(n)` overrides it. The `fg`/`bg`/`rgb`/`bold`/…
helpers wrap a string for direct printing; `Term.style(...)` produces the
style passed to `screen.set` / `screen.put` for coloured cells.

```culebra
inspect(Term.bold(Term.fg("alert", 196)))      # bold bright-red "alert" (printed)
let st = Term.style(fg: (255, 128, 0), bold: true)   # for a Screen cell
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

### Piped input

`Term.attach_tty() -> Bool` reattaches stdin to the controlling terminal,
replacing whatever stdin currently is (a pipe, a redirected file). Raw mode
and key reads (`Term.app`, `Term.poll`, `Term.read_key`) only work off a real
tty, so a script fed content on stdin (`some-command | script.cul`) can drain
that pipe first, then call `Term.attach_tty()` to switch to interactive key
input — the same pattern `less` uses. Returns `false` (stdin is left
untouched) when there is no controlling terminal at all, e.g. running fully
detached or under CI.

```culebra
# doctest: skip
let content = IO.stdin().read()   # drain the pipe first
if !Term.attach_tty() {
  println(content)                # no terminal to be interactive on
  Sys.exit(0)
}
Term.app(fn (screen) { ... })
```

### Input events

Input is a single event model: `Term.poll(timeout)` — also reachable as
`screen.poll(timeout)` inside a `Term.app` callback — returns one event
**Object**, or `nil` when nothing arrived within `timeout` seconds.
`Term.parse(raw)` turns an already-read escape sequence into the same
shape. A `kind` field discriminates; modifiers are booleans.

| `kind` | Fields |
| --- | --- |
| `"key"` | `key`, `ctrl`, `shift`, `alt` |
| `"mouse"` | `event`, `button`, `x`, `y`, `ctrl`, `shift`, `alt` |
| `"resize"` | `cols`, `rows` |

For a **key**, `key` is a printable character (`"q"`, `" "`) or a name:
`"up"` / `"down"` / `"left"` / `"right"`, `"enter"`, `"escape"`, `"tab"`,
`"backspace"`, `"insert"`, `"delete"`, `"home"`, `"end"`, `"pageup"`,
`"pagedown"`, `"f1"`…`"f12"`. Modifiers are reported in `ctrl` / `shift` /
`alt` (e.g. Ctrl+Right → `{key: "right", ctrl: true}`, Ctrl+C →
`{key: "c", ctrl: true}`, Alt+x → `{key: "x", alt: true}`).

For a **mouse** event (see below), `event` is `"press"` / `"release"` /
`"drag"` / `"scroll"`; `button` is `"left"` / `"middle"` / `"right"` /
`"wheel_up"` / `"wheel_down"`; `x` / `y` are 0-based cells.

`Term.resized() -> Bool` is the lower-level resize flag (true once after a
SIGWINCH); `poll` turns it into a `"resize"` event.

Mouse reporting is opt-in: enable it with `Term.app(..., mouse: true)` (or
print `Term.mouse_on()` / `Term.mouse_off()` yourself).

```culebra
# doctest: skip
let ev = screen.poll(0.1)
if ev != nil {
  if ev.kind == "key" && ev.key == "q" { ... }
  else if ev.kind == "mouse" && ev.event == "press" { ... ev.x, ev.y ... }
}
```

### `Term.app` and `Screen`

`Term.app(fn (screen) { ... }, mouse: false)` enters raw mode and the
alternate screen, hides the cursor, watches for resizes (and enables mouse
reporting when `mouse: true`), and **restores the terminal on exit**
— normal return, an exception, or Ctrl+C — via `defer`. The callback
receives a `Screen`:

| Method | Effect |
| --- | --- |
| `screen.size()` / `cols()` / `rows()` | terminal dimensions |
| `screen.clear()` | reset the back buffer to a blank frame (current size) |
| `screen.set(x, y, glyph, style = "")` | place one grapheme (with an optional `Term.style`) into the back buffer |
| `screen.put(x, y, s, style = "")` | lay the graphemes of `s` into successive cells, all in `style` |
| `screen.render() -> String` | minimal escapes to update the screen from the last frame (and advance the front buffer) |
| `screen.flush()` | print `render()` and flush |
| `screen.poll(timeout) -> Object?` | wait up to `timeout` seconds for an input event (key / mouse / resize), or `nil` |

A `Screen` is a double-buffered grid of cells, each holding a glyph and an
optional style. `flush` emits **only the cells that changed** since the last
frame, with the minimal SGR transitions between styles, so live UIs update
without flicker and with minimal output; wide glyphs occupy two cells and a
resize forces a full repaint. Build a frame with `clear` + `set` / `put`
(passing a `Term.style(...)` for colour), then `flush`; read input with `poll`
(which doubles as the per-frame delay).

```culebra
Term.app(fn (s) {
  s.clear()
  s.put(2, 1, "hello")
  s.flush()
  s.poll(2.0)            # wait up to 2s for a keypress
})
```

---

## 23. `Log`

Leveled, structured logging to **standard error** (so it never pollutes the
program's stdout data, which a pipe like `myscript | jq` consumes).

| Function | Result |
| --- | --- |
| `Log.debug(msg: String, fields: Object = {})` | log at `debug` |
| `Log.info(msg, fields = {})` | log at `info` |
| `Log.warn(msg, fields = {})` | log at `warn` |
| `Log.error(msg, fields = {})` | log at `error` |
| `Log.with(fields: Object) -> logger` | a child logger that binds `fields` into every record |
| `Log.set_level(level: String) -> Nil` | set the threshold (`"debug" < "info" < "warn" < "error"`) |
| `Log.set_format(format: String) -> Nil` | `"text"` (default) or `"json"` |

Each call takes a message and an optional `Object` of structured fields. Only
records at or above the threshold are emitted (default `info`, so `debug` is
dropped). The threshold and format default from the `LOG_LEVEL` / `LOG_FORMAT`
environment variables and can be overridden with `set_level` / `set_format`; an
unknown level or format raises. A timestamp (ISO 8601 UTC) is always included.

`text` is human-readable (the level is colored when stderr is a terminal):

```
2026-06-24T21:30:01Z info request done method=GET status=200 ms=12.4
```

`json` emits one object per line (JSON Lines — pipe to `jq` or a log shipper):

```json
{"time":"2026-06-24T21:30:01Z","level":"info","msg":"request done","method":"GET","status":200,"ms":12.4}
```

`Log.with(fields)` returns a **child logger** that carries `fields` on every
line, so request- or job-scoped context is bound once instead of repeated.
Children share the global level/format and can nest. The reserved keys `time`,
`level`, and `msg` always win over a field of the same name.

```culebra
# doctest: skip
Log.info("server started")
Log.set_level("debug")
Log.debug("cache miss", {key: "user:42"})

let log = Log.with({request_id: id})   # bind context once
log.info("received")                    # ...carried on every line
log.error("upstream failed", {status: 502})
```

Values are rendered with `to_string` in text mode and serialized via the JSON
namespace in json mode. For a fatal condition, log at `error` then
`Sys.exit(1)` — there is no separate `fatal` level.

---

## 24. `TOML`

Parse and render [TOML](https://toml.io) configuration, shared byte-for-byte
across backends. The grammar and serialization live in a value-neutral core,
so the interpreter, JIT, and AOT agree exactly.

| Function | Result |
| --- | --- |
| `TOML.parse(text: String) -> Object` | the document as a nested `Object` |
| `TOML.stringify(v: Object, sort_keys: Bool = false) -> String` | TOML text; sub-tables become `[section]` headers |

`parse` accepts the TOML v1.0 surface: bare / quoted / dotted keys,
`[table]` and `[[array.of.tables]]` headers, the four string forms (basic,
literal, and their multiline `"""` / `'''` variants), integers
(decimal / `0x` / `0o` / `0b`, with `_` separators), floats (including
`inf` / `nan`), booleans, arrays, and inline tables. Values map as:

| TOML | Culebra |
| --- | --- |
| table / inline table | `Object` (insertion order preserved) |
| array | `Array` |
| array of tables `[[x]]` | `Array<Object>` |
| string (any of the four forms) | `String` |
| integer | `Long` |
| float | `Float` |
| boolean | `Bool` |
| date-time / date / time | `String` (kept raw — there is no date type) |

Date-times come back as their raw text rather than a dedicated type, so a
round trip re-quotes them as ordinary strings. Malformed input raises a
`ValueError` whose `e.line` / `e.col` (both 1-based) point at the offending
character:

```culebra
let r = try { TOML.parse("x = "); nil } catch e { e }
inspect(r.message)            # => 'TOML.parse: expected value'
inspect("{r.line}:{r.col}")   # => '1:5'
```

`stringify` takes an `Object` (a TOML document is always a table) and renders
each scalar / array / inline value first, then expands sub-tables into
`[section]` headers and table arrays into `[[…]]` blocks, so every bare key
precedes the header that would otherwise capture it. Floats always carry a
decimal point so they parse back as floats. `sort_keys: true` walks keys
alphabetically for deterministic output. Functions, tensors, and Objects with
non-String keys are not serializable — `stringify` throws `TypeError`.

```culebra
let cfg = TOML.parse("""
title = "demo"
ports = [80, 443]

[server]
host = "localhost"
""")
inspect(cfg.title)            # => 'demo'
inspect(cfg.server.host)      # => 'localhost'
```

`stringify` renders bare keys before the headers that would capture them:

```culebra
inspect(TOML.stringify({a: 1, b: {c: 2}}))
# => |
# 'a = 1
#
# [b]
# c = 2
# '
```

---

## 25. `SQLite`

Embedded SQL database backed by [SQLite](https://sqlite.org) (the amalgamation
is vendored and compiled in — no system library is required). `SQLite.open`
returns a stateful **Database** handle; the high-level `execute` / `query` /
`transaction` methods cover everyday CRUD, and `prepare` returns a reusable
**Statement** handle for hot loops. Both handles close deterministically when
they leave scope, so an explicit `close` / `finalize` is optional.

| Function | Result |
| --- | --- |
| `SQLite.open(path: String) -> Database` | open (or create) a database; `":memory:"` for an in-memory one |
| `SQLite.version() -> String` | the linked SQLite library version, e.g. `"3.53.2"` |

### Database

| Method | Result |
| --- | --- |
| `db.execute(sql: String, params = nil) -> Long` | run one statement; returns rows affected |
| `db.query(sql: String, params = nil) -> Array<Object>` | run a query; each row is an `Object` keyed by column name |
| `db.prepare(sql: String) -> Statement` | compile a reusable statement |
| `db.transaction(fn: Function) -> Any` | `BEGIN`, run `fn`, `COMMIT`; any throw triggers `ROLLBACK` and re-raises |
| `db.close()` | close the connection (also runs automatically on scope exit) |

### Statement

| Method | Result |
| --- | --- |
| `stmt.run(params = nil) -> Long` | execute (INSERT/UPDATE/DELETE); returns rows affected |
| `stmt.query(params = nil) -> Array<Object>` | execute a query and collect the rows |
| `stmt.finalize()` | release the statement (also runs automatically on scope exit) |

### Parameters

`params` binds placeholders in the SQL. An **Array** binds positional `?`
placeholders left to right; an **Object** binds named `:name` (or `@name` /
`$name`) placeholders by key:

```culebra
# doctest: skip
db.execute("INSERT INTO users VALUES (?, ?)", [1, "Alice"])
db.query("SELECT * FROM users WHERE id = :id", {id: 1})
```

### Type mapping

Values are mapped by the column's runtime type (reads) or the culebra value's
type (writes):

| SQLite | Culebra |
| --- | --- |
| INTEGER | `Long` |
| REAL | `Float` |
| TEXT | `String` |
| BLOB | `String` (raw bytes) |
| NULL | `nil` |

On the write side a `Bool` binds as `0` / `1`; binding any other type (an
`Array`, `Object`, function, …) raises `TypeError`. SQL or constraint errors
raise `SQLiteError` carrying SQLite's own message:

```culebra
let db = SQLite.open(":memory:")
db.execute("CREATE TABLE users (id INTEGER, name TEXT)")
db.execute("INSERT INTO users VALUES (?, ?)", [1, "Alice"])

let rows = db.query("SELECT * FROM users")
inspect(rows[0]["name"])      # => 'Alice'

# a reusable prepared statement
let ins = db.prepare("INSERT INTO users VALUES (?, ?)")
for u in [[2, "Bob"], [3, "Carol"]] { ins.run(u) }
ins.finalize()

# all-or-nothing
db.transaction(fn () {
  db.execute("UPDATE users SET name = 'Bob!' WHERE id = 2")
})

let r = try { db.query("SELECT * FROM missing"); nil } catch e { e }
inspect(r.kind)               # => 'SQLiteError'

db.close()
```

A Database / Statement handle is tied to the thread (isolate) that created it —
it is not `Sendable` and cannot be passed across an `Isolate` / `Channel`
boundary. Transactions do not nest (use `SAVEPOINT` directly if you need that).

---

## 26. `Canvas`

An immediate-mode 2D framebuffer for little games and pixel graphics: draw a
frame, `present` it, poll input, repeat. Colours are packed RGBA `Long`s and
the buffer can be any size (a WASM-4-style 160×160 is typical). In the WASM
Playground a Canvas program runs in the **Canvas tab** — frames are shown on a
`<canvas>`, keyboard/pointer feed the input, and `tone` plays through WebAudio.
Natively a build **opens a real desktop window** on macOS, Linux and Windows,
using vendored static raylib + SDL3, the same backend the
`Scene` namespace links. Building it on Linux needs SDL3's documented build
dependencies present (`vendor/SDL/docs/README-linux.md`); SDL3's configure fails
outright when the X11 or audio headers it probes for are missing, so a machine
without them should configure with `-DCULEBRA_ENABLE_CANVAS_WINDOW=OFF`. Windows
needs no such packages — the mingw-w64 toolchain already has the headers SDL3's
Win32 backends want. The resulting binary still runs anywhere: SDL3 loads
X11/GL/audio (`opengl32.dll` and friends on Windows) on first use, so a window
build adds no load-time library dependency and starts fine on a headless
server. Each `present` uploads the frame, upscales it with
nearest-neighbour to a comfortable window size, and blocks to vsync at 60 fps;
the keyboard and mouse feed `Canvas.buttons`/`Canvas.mouse`, and closing the
window (or Esc) ends the `run` loop. **Headless is declared, never inferred**:
in a build without the window backend — or in any run with
`CULEBRA_CANVAS_HEADLESS` set to anything but `0`/`off` — the backend is
**headless**: the pixel and sprite ops run identically (so behaviour is the same
across interpreter / JIT / AOT and testable via `Canvas.get_pixel`), but nothing
is displayed, input reads as "no button", and `tone` is silent. That variable is
how a displayless server — or the test suite, since every `just` recipe exports
it — runs a window-capable binary; `-DCULEBRA_ENABLE_CANVAS_WINDOW=OFF` goes
further and leaves raylib out of the build entirely. A window build that
declared neither and cannot open a window (no display, no usable GL) raises a
`RuntimeError` at the first `present` naming the variable, rather than
guessing that a silent headless run is what was wanted. Outside headless, native
`tone` plays through a small software APU mixed on raylib's audio thread (see
Audio below), lazily opening the audio device on first use.

### Colour

`Canvas.rgba(r, g, b, a = 255) -> Long` packs four 0–255 channels into one
`Long` (byte order `[r, g, b, a]`). Every drawing call takes such a colour,
and **the alpha composites**: 255 draws opaque, 0 draws nothing, and anything
between blends the shape over what is already there (integer source-over —
`(src*a + dst*(255-a) + 127) / 255` per colour channel, so all three backends
round identically, and an opaque buffer stays opaque). `rgba(0, 0, 0, 128)`
is a half-dark overlay; drawing it twice darkens twice. The two exceptions
are `clear`, which replaces every pixel with the given value (a frame reset,
not a wash), and `set_pixel`, which stores the value raw so it pairs with
`get_pixel` (writing transparency is what those two are for).

`Canvas.rgb_to_hsv(r, g, b) -> Tuple` and `Canvas.hsv_to_rgb(h, s, v) -> Tuple`
convert between the two colour models — the RGB side in the same 0–255
channels as `rgba`, the HSV side each of hue/saturation/value in `0.0..1.0`.
HSV is where a palette gets *derived*: boosting saturation, narrowing a
light/dark pair toward each other, or shifting hue are all one-line
expressions in HSV that have no simple RGB equivalent. `Canvas.hsv(h, s, v, a
= 255) -> Long` packs the result directly, mirroring `rgba`.

```culebra
inspect(Canvas.rgb_to_hsv(255, 0, 0))     # => (0.0, 1.0, 1.0)
inspect(Canvas.hsv_to_rgb(0.0, 1.0, 1.0)) # => (255, 0, 0)
inspect(Canvas.hsv(0.0, 1.0, 1.0) == Canvas.rgba(255, 0, 0))  # => true

# saturate a base colour by 40%, in HSV, then pack it
let (h, s, v) = Canvas.rgb_to_hsv(180, 140, 200)
inspect(Canvas.hsv(h, Math.min(1.0, s * 1.4), v))  # => 4291327148
```

The pair round-trips exactly on the 0–255 grid — `hsv_to_rgb` rounds each
channel, so feeding `rgb_to_hsv`'s result straight back into `hsv_to_rgb`
reproduces the original `r, g, b` for every input, not just a
visually-close approximation.

### Drawing

| Function | Effect |
| --- | --- |
| `Canvas.init(w, h)` | allocate (or resize) the framebuffer; `Canvas.run` does this for you |
| `Canvas.clear(color)` | replace the whole target with `color` (no compositing) |
| `Canvas.set_pixel(x, y, color)` | store one pixel raw (off-buffer writes are ignored) |
| `Canvas.get_pixel(x, y) -> Long` | read a pixel (0 off-buffer) — for pixel-readback collision |
| `Canvas.rect(x, y, w, h, color, fill = true)` | rectangle, clipped |
| `Canvas.line(x1, y1, x2, y2, color)` | line, both endpoints included |
| `Canvas.circle(cx, cy, r, color, fill = true)` | circle centred on `(cx, cy)` |
| `Canvas.ellipse(cx, cy, rx, ry, color, fill = true)` | ellipse with per-axis radii |
| `Canvas.triangle(x1, y1, x2, y2, x3, y3, color, fill = true)` | triangle |
| `Canvas.polygon(points, color, fill = true)` | polygon from a flat vertex list |
| `Canvas.width()` / `Canvas.height() -> Long` | current draw-target dimensions |
| `Canvas.to_png() -> String` | the current draw target's pixels as PNG bytes |
| `Canvas.present()` | show the frame (see the loop below) |

The framebuffer and the sprite registry belong to one isolate — the first one
to touch them, by drawing or by reading (`width`, `get_pixel`). A second
isolate is refused rather than raced: it finds an empty canvas (no pixels, no
sprite handles), and the calls that can report say why — `init` and
`draw_to`/`to_png` raise `RuntimeError`. A worker can own the canvas, as long
as no other isolate has touched it.

Every shape takes `fill: false` to draw its one-pixel outline instead of the
filled interior: for `rect` that is the outermost ring of the same fill, for
`circle` / `ellipse` the connected edge, and for `triangle` / `polygon` the
closed chain of `line`-drawn edges (whose vertices, unlike the half-open
fill, are included). `line` walks its major axis one pixel at a time with the
minor coordinate rounded to nearest, and sorts its endpoints first, so
`line(a, b)` and `line(b, a)` draw identical pixels. A circle of radius `r`
spans `2r + 1` pixels across its centre row — `(cx ± r, cy)` lies on the
circle — and a radius of 0 is a single pixel; negative radii draw nothing.

Every position and size argument is `Long|Float`. A `Float` is rounded toward
−∞ (pixel *n* covers `[n, n+1)`), so adjacent spans tile without a gap and a
negative coordinate stays off-buffer instead of snapping onto column 0.
Non-finite and out-of-range values saturate rather than trap, and `Math.nan`
reads as 0. This lets a program that computes positions in floating point — a
projection, a scroll offset — pass them straight in instead of rounding each
one itself. Colours, blit flags, alpha and sprite handles remain `Long`.

`Canvas.polygon` takes `points` as a flat `[x0, y0, x1, y1, …]` list of at
least three vertices — Longs or Floats, like every other coordinate — and
closes the outline itself; a trailing half-pair is ignored. It fills by the
even-odd rule, so a concave outline hollows out the way you would draw it.
`Canvas.triangle` is the same fill from three vertices spelled out, which is
what the conventional shape call looks like (raylib, SDL and GPU rasterizers
all take three) and avoids building an `Array` per call on a hot path.
`Canvas.circle` is `ellipse` with one radius; both compute each row's
half-width with an exact integer square root, so every backend rasterizes the
identical curve.

Rows and spans are half-open, the same convention as `rect`: a row belongs to
the edge whose vertical span contains it, and each filled span covers
`[xl, xr)`. Shapes sharing an edge therefore tile with no seam and no
double-drawn pixel — a rectangle cut along its diagonal into two triangles
rasterizes back to exactly the rectangle. The interpolation is integer
throughout, so every backend produces the identical shape, and coordinates
saturate into a ±2³⁰ guard band so no input can overflow it.

### The window

A window exists only in a desktop build that opened one — headless builds and
the browser have none, and the entry points below are no-ops there. `Canvas`
is a framebuffer first; this is the one part of it the OS owns.

| Function | Effect |
| --- | --- |
| `Canvas.title(name)` | name the window; call it before the loop starts |

The browser is a deliberate no-op rather than a missing feature: the tab's
title belongs to the page hosting the canvas, not to the program drawing on it.

### Sprites

`Canvas.Sprite.new(pixels, w, h, palette = nil)` uploads a sprite once and
returns a handle to blit cheaply each frame. `pixels` is a flat, row-major
array: packed-RGBA `Long`s, or — when `palette` is given — indices into that
palette (compact indexed art). A fully transparent source pixel is skipped
(the shape mask), a partially transparent one — a PNG's anti-aliased edge —
blends over what is there, so sprites composite. A sprite frees its pixels
when the last reference to it goes away.

`Canvas.Sprite.new(png: String)` instead decodes PNG bytes — `FS.read` of an
image file, say, since a `String` is a byte string — and takes the size from
the image, so there is nothing to pass alongside the data. `Canvas.Sprite.from_png(data)` is the same thing under
a name that reads as one at the call site. Greyscale, palette (with `tRNS`),
truecolour and 16-bit-per-channel images all decode to the same packed-RGBA
layout the framebuffer uses; anything undecodable raises
`ValueError: not a valid PNG image`.

| Method | Effect |
| --- | --- |
| `sprite.draw(x, y, flip_x = false, flip_y = false, transpose = false)` | blit the whole sprite to `(x, y)`; `transpose` swaps X/Y (a diagonal reflection — combine with a flip for a 90° rotation) |
| `sprite.draw_sub(x, y, sx, sy, sw, sh, flip_x = false, flip_y = false, transpose = false)` | blit a sub-rectangle (for sprite sheets) |
| `sprite.draw_scaled(x, y, w, h, flip_x = false, flip_y = false, smooth = false, alpha = 255)` | blit into the `w`×`h` rectangle at `(x, y)`, resampling to fit |
| `sprite.draw_sub_scaled(x, y, w, h, sx, sy, sw, sh, flip_x = false, flip_y = false, smooth = false, alpha = 255)` | the same from a sub-rectangle |
| `sprite.to_png() -> String` | the sprite's pixels as PNG bytes — `from_png`'s inverse |
| `sprite.width()` / `sprite.height()` | sprite dimensions |

The scaling blits sample nearest-neighbour, so pixel art scaled up stays crisp;
`smooth` box-averages the source instead when the sprite shrinks (it is ignored
when neither axis shrinks). There is no scaled `transpose`. `alpha` (0–255)
scales the whole blit: each pixel composites at its own source alpha times
`alpha`, so an opaque sprite at `alpha: 128` is half-blended and a
half-transparent edge pixel under it blends at a quarter. A fully transparent
source pixel is skipped and never contributes, whether it is sampled directly
or averaged over. A destination or source rectangle with a non-positive side
draws nothing.

### Offscreen drawing

`Canvas.Sprite.blank(w, h, color = 0)` creates an empty sprite, and
`Canvas.draw_to(sprite, fn () { ... })` redirects every drawing call into it
for the duration of the closure — `clear`, the shapes, `text`, other
sprites' `draw`, plus `Canvas.width()` / `height()` and `get_pixel`, which
follow the target so centring code keeps working offscreen. `present()`
always shows the framebuffer. The previous target is restored on every exit
path (including a throw), and `draw_to` nests like the call stack it is.

```culebra
# doctest: skip
let bgd = Canvas.Sprite.blank(320, 240)
Canvas.draw_to(bgd, fn () {          # render the backdrop once…
  Canvas.clear(sky)
  for i in 0..50 { Canvas.circle(Random.below(320), Random.below(240), 2, star) }
})
Canvas.run(320, 240, fn () {
  bgd.draw(0, 0)                     # …then each frame is one blit
  true
})
```

Two things refuse with a `ValueError`: drawing a sprite onto itself (a blit
would read its own writes), and freeing the sprite currently drawn to. Both
are backend-symmetric, like every Canvas error.

### Saving an image

`sprite.to_png()` and `Canvas.to_png()` answer PNG bytes, so writing one out
is `FS.write` of the result and reading it back is the `from_png` that already
existed. `Canvas.to_png()` follows the **current draw target**, the way
`width` / `height` / `get_pixel` do — inside a `draw_to` it encodes the sprite
being drawn into, outside it encodes the framebuffer.

```culebra
# doctest: skip
Canvas.init(320, 240)
Canvas.clear(Canvas.rgba(24, 24, 32))
Canvas.circle(160, 120, 40, Canvas.rgba(240, 180, 90))
FS.write("shot.png", Canvas.to_png())          # a screenshot

let tile = Canvas.Sprite.blank(16, 16)         # or render one offscreen
Canvas.draw_to(tile, fn () { Canvas.clear(Canvas.rgba(80, 200, 120)) })
FS.write("tile.png", tile.to_png())
```

Output is 8-bit truecolour with alpha, one `IDAT`, each row filtered by the
choice that scores smallest — so flat, dithered pixel art compresses close to
what a dedicated encoder gets. An image with no pixels (`Canvas.init(0, 0)`)
and a sprite handle that has been freed both raise `ValueError`.

### Text

`Canvas.text(s, x, y, color, scale = 1)` draws `s` in the built-in 8×8 bitmap
font (the WASM-4 runtime font, covering printable ASCII 32–126, upper- and
lower-case; characters outside that range are skipped). Each font pixel
becomes a `scale`×`scale` block and the advance follows at `8 * scale` px per
character — `scale: 2` is the title-screen size. A non-positive scale draws
nothing. `Canvas.text_width(s, scale = 1) -> Long` gives the pixel width, for
centring or right-aligning.

### Input

Input is polled each frame (it reflects the current state, not an event queue).

| Function | Result |
| --- | --- |
| `Canvas.buttons() -> Long` | bitmask of held buttons |
| `Canvas.mouse() -> Object` | `{x, y, buttons}` in framebuffer pixels |
| `Canvas.key(name) -> Bool` | one named key is held now |
| `Canvas.key_queue() -> Array` | drain this frame's key presses (names) |
| `Canvas.typed() -> String` | drain the characters the user typed |

Button bits are the constants `Canvas.LEFT`, `RIGHT`, `UP`, `DOWN` (arrow keys,
and WASD as a second set) and `Canvas.A`, `B` (action keys — `A` is Space/Z,
`B` is X). For edge
detection, `Canvas.Input.new()` tracks the previous frame:

| Method | Result |
| --- | --- |
| `input.update()` | sample this frame's buttons (call once per frame) |
| `input.down(btn) -> Bool` | button is held now |
| `input.pressed(btn) -> Bool` | button went down **this** frame (the flap trigger) |

Beyond the six buttons, `Canvas.key` reports any key by name — **the same
vocabulary `Term.read_key` uses**, so key handling code moves between the two
namespaces unchanged: a printable character (`"a"`, `" "`, `"-"`) or a
special-key name (`"up"` / `"down"` / `"left"` / `"right"`, `"enter"`,
`"escape"`, `"tab"`, `"backspace"`, `"insert"`, `"delete"`, `"home"`,
`"end"`, `"pageup"`, `"pagedown"`, `"f1"`…`"f12"`). `"space"` is accepted as
a readable alias for `" "`; an unknown name is simply never held. Letters
name the physical key regardless of Shift (`"a"` covers both cases).

`Canvas.key_queue()` returns the keys pressed since it was last called — the
edge events, for bindings that must not repeat while held — and
`Canvas.typed()` returns the characters typed (Shift, layout and IME
applied), which is what a name-entry screen reads. Both drain destructively
and are capped at 256 entries (oldest first out), so call each from one place
per frame and hand the result around. Natively, the first `typed()` call
turns the platform's text input on — a program that only polls keys never
sees an IME popup. Headless, nothing is held and the queues are empty.

### Audio

`Canvas.tone(freq, dur, vol = 100, wave = 0, end_freq = nil, attack = 0,
decay = 0, release = 0, peak = nil, duty = 2)` plays a note through a small
WASM-4-style APU. In its simplest form `Canvas.tone(freq, dur)` is a `dur`-frame
note (frames at ~60fps) at `freq` Hz. The optional arguments expose the full
envelope: the pitch slides `freq → end_freq` while an ADSR envelope
(`attack`/`decay`/`release` in frames, `dur` as the sustain length) shapes the
volume from 0 up to `peak`, down to the sustain `vol` (0–100), and back to 0.
`wave` selects the channel — `Canvas.PULSE` / `PULSE2` (with a `duty` cycle:
`Canvas.DUTY_EIGHTH` / `DUTY_QUARTER` / `DUTY_HALF` / `DUTY_THREE_QUARTER`),
`Canvas.TRIANGLE`, `Canvas.NOISE`, or the culebra extension `Canvas.SAWTOOTH`.
Each channel is monophonic (a new note cuts the previous one). A synthesized
waveform is raw and four channels can sound at once, so `tone` mixes its
0–100 well under full scale — file-backed audio (`Sound`, `music`) is already
mixed and plays at the file's own level for `vol = 100`.

In the browser, `tone` plays through WebAudio (an oscillator per channel, a
`PeriodicWave` for the pulse duty cycle, a filtered noise buffer). Natively it
plays through a small software synth mixed on raylib's audio thread (naive —
not band-limited — oscillators; noise is a filtered PRNG), started lazily on
first use so a program that never calls `tone` never opens an audio device.
Audio is silent on the headless native backend, and stays silent (no device
opened, no crash) on a machine with no audio hardware. Native audio and
WebAudio are independent implementations tuned to sound similar, not
sample-identical — unlike the pixel ops, `tone` isn't required to produce
bit-identical output across backends.

### Sound effects

`Canvas.Sound.new(data)` decodes a one-shot sample from its bytes — WAV, MP3
or Ogg Vorbis, the bytes-in convention of `Sprite.from_png` — and plays it
per call, which is the recorded-sample counterpart to `tone`'s synthesis (an
explosion from a file rather than a swept square wave).

| Method | Effect |
| --- | --- |
| `sound.play(vol = 100)` | play from the start (restarts if still playing) |
| `sound.stop()` | stop the voice |
| `sound.playing() -> Bool` | still audible? |

Each `Sound` is one voice — `play` while playing restarts it, like the host
samplers underneath — and the decoded sample is freed with the last
reference. Bytes that are none of the three formats raise
`ValueError: not a valid WAV, MP3 or Ogg audio stream` on every backend;
past that check, an undecodable stream stays silent, and headless (or with no
audio device) everything no-ops with `playing()` false, exactly like
`music`.

### Music

`Canvas.music(data, loop = true, vol = 100, start = 0.0)` plays an MP3 or Ogg
Vorbis file from its bytes (a `String`, e.g. from `FS.read` — the same
bytes-in convention as `Sprite.from_png`). There is **one slot**, in the
manner of pygame's `mixer.music`: playing a new file replaces the old one, and
no handle reaches the script. `vol` is 0–100, `100` being the file's own
level; `start` is seconds into the file. Bytes that are neither MP3 nor Ogg raise
`ValueError: not a valid MP3 or Ogg audio stream` on every backend; a stream
that passes that check but fails to decode stays silent.

| Function | Effect |
| --- | --- |
| `Canvas.music(data, loop = true, vol = 100, start = 0.0)` | decode and play (replaces the current file) |
| `Canvas.music_stop()` | stop and unload |
| `Canvas.music_pause()` / `Canvas.music_resume()` | pause / pick up where it left off |
| `Canvas.music_volume(vol)` | change the volume (0–100) |
| `Canvas.music_seek(seconds)` | jump to a position |
| `Canvas.music_playing() -> Bool` | is anything audible right now |

With nothing loaded, the controls are no-ops and `music_playing()` is `false`
— matching `tone`'s clamp-don't-throw convention, the format check is the one
error. Natively the stream is decoded incrementally and its buffers are
refilled from `present()`, so music only advances while frames are being
presented — a program that stops presenting pauses its music with it.
Headless (and on a machine with no audio device) everything no-ops. In the
browser the file is decoded by WebAudio; note Ogg support there depends on
the browser (Safari historically decodes only MP3), while natively both
formats always work.

### The game loop

`Canvas.run(w, h, tick, frames = 600)` sets up a `w`×`h` framebuffer and calls
`tick()` once per frame, presenting after each. `tick` returns `false` to stop
(e.g. the player quit). In the interactive Playground build `present()` waits
for the browser's next animation frame, so the loop paces itself and yields
cooperatively. Otherwise the loop stops after `frames`, so a run nobody can end
can't spin forever. Staying unbounded takes both halves: the frames must reach a
display (not a build without the window backend, not `CULEBRA_CANVAS_HEADLESS`,
not a machine with no display) *and* the run must be interactive (not a piped
native run, not a non-JSPI browser). A headless run from a terminal meets only
the second — it shows nothing, takes no input and has no close box — so it is
capped like any other automated run.

```culebra
# doctest: skip
let red = Canvas.rgba(220, 60, 60)
mut x = 0
Canvas.run(160, 160, fn () {
  Canvas.clear(Canvas.rgba(20, 24, 40))
  Canvas.rect(x, 76, 8, 8, red)
  x = (x + 2) % 160
  true
})
```

---

## 27. `Scene`

A retained-mode renderer for 3D built from procedural geometry: you populate a
scene graph of nodes — primitives (boxes, spheres, cylinders, planes) and
hand-built meshes — give them materials and transforms, place a camera, and
render. The lighting is physically based (metallic/roughness materials, a
directional sun with two-cascade shadows, sky/fog, and a post stack of SSAA,
ambient occlusion, bloom, and depth of field), so the output is more than
flat-shaded primitives.

`Scene` is **not a game engine**. It has no physics or collision, no
model/texture import (geometry is procedural or built vertex-by-vertex, and
textures are generated in-process), no skeletal animation, and no mouse input.
It targets 3D you *construct* — visualisations, procedural scenes, vehicle or
flight demos with a chase camera — rather than asset-driven games. A
racing demo — circuit mesh, chase camera, gamepad steering — is the
shape it is designed around.

`Scene` is **opt-in and currently macOS-only**. It is not in the default build;
enable it with `-DCULEBRA_ENABLE_SCENE=ON`, which builds the vendored static
SDL3 + raylib backend. A windowed backend for Linux/Windows and the browser is
not available yet, so — unlike `Canvas` — `Scene` programs neither run headless
nor in the Playground.

### The view and the frame loop

`Scene.View.new(w, h, title)` opens a window, and raises a `RuntimeError` when
it cannot (no usable display/GL) — unlike `Canvas` there is no headless mode to
fall back to, because everything a `View` does needs the GPU. Positions and
sizes are `Float`
world units; colours are three or four `0–255` integer channels, and a channel
outside that range clamps to it. A frame is
either a 3D pass with a 2D overlay (`render_3d()` → overlay draws → `present()`)
or pure 2D (`begin2d()` → draws → `present()`).

| Method | Effect |
| --- | --- |
| `view.target_fps(fps)` | cap the frame rate |
| `view.closing() -> Bool` | window close requested (loop until true) |
| `view.dt() -> Float` | seconds since the previous frame |
| `view.width()` / `view.height() -> Float` | window size |
| `view.camera(px,py,pz, tx,ty,tz, ux,uy,uz, fov)` | eye position, look-at target, up vector, vertical FOV |
| `view.render_3d()` | render the scene graph, then open the frame for a 2D overlay |
| `view.begin2d()` | open a pure-2D frame (no 3D pass) |
| `view.present()` | finish and show the frame |

### Scene graph

`view.add_node()` adds an empty node; the `add_*` helpers add geometry and
return the new node. Nodes nest (`node.add_node()`, `node.add_box()`, …) and the
transform methods are fluent, so a subtree builds in one expression. Build
persistent geometry once and move it each frame.

| Method | Effect |
| --- | --- |
| `view.add_box(w, h, d)` / `add_sphere(r)` / `add_cylinder(r, h)` / `add_plane(w, d)` | add a primitive node (a `node` adds the same shapes as a child) |
| `view.add_mesh()` / `node.add_mesh()` | add an empty custom mesh (below) |
| `node.move(x, y, z)` | set position |
| `node.yaw(a)` / `pitch(a)` / `roll(a)` | rotate about one axis (radians) |
| `node.spin(x, y, z, a)` / `euler(x, y, z)` | axis-angle / Euler rotation |
| `node.scale(s)` / `scale3(x, y, z)` | uniform / per-axis scale |
| `node.tint(r, g, b)` | per-node colour |
| `node.material(id)` | assign a material (below) |
| `node.hide()` / `show()` / `name(n)` | visibility / label |
| `node.x()` / `y()` / `z() -> Float` | read back position |

A custom mesh is built from vertices and triangles, then finalised:
`m.vertex(x, y, z, nx, ny, nz)` (or `vertex_uv(…, u, v)`) adds a vertex,
`m.tri(a, b, c)` a triangle by vertex index, and `m.build()` uploads it. (raylib
uses a 16-bit index buffer, so a mesh caps at 65535 vertices; `build()` rejects
more.) An uploaded mesh belongs to the view that uploaded it: a node kept past
`view.drop()` stays usable as a transform, but draws nothing in a later view.

### Materials, lighting, textures

Materials are created on the view and referenced by id:

| Method | Result |
| --- | --- |
| `view.material(r, g, b) -> id` | flat-colour material |
| `view.material_pbr(r, g, b, metallic, roughness) -> id` | PBR material (`metallic`/`roughness` are 0–1) |
| `view.material_tex(tex, r, g, b) -> id` / `material_tex_pbr(tex, r, g, b, metallic, roughness) -> id` | textured material |

Textures are generated in-process (there is no image-file loader):
`view.checker(px, checks, r1,g1,b1, r2,g2,b2) -> tex` makes a checkerboard,
`view.grain(px, r, g, b, amt) -> tex` a noise texture, and `view.canvas(w, h) ->
tex` opens a render-to-texture you paint with the 2D calls (`rect`/`text`/…) and
close with `view.canvas_end()` — how the demo draws liveries and signage. One
canvas is open at a time: a second `canvas()` before the close is refused, and a
frame opened with one still open ends it first. The texture is upright on the
mesh whichever way its UVs run.

Lighting is set on the view:

| Method | Effect |
| --- | --- |
| `view.background(r, g, b)` | clear colour |
| `view.sky(tr,tg,tb, br,bg,bb)` | zenith → horizon gradient (also the reflected environment) |
| `view.sun(dx,dy,dz, intensity, r,g,b)` | directional light (two-cascade shadows); `(0, 0, 0)` names no direction and is refused |
| `view.ambient(intensity, r, g, b)` | fill light |
| `view.fog(start, end, r, g, b)` | distance fog |
| `view.screenshot(path)` | save the current frame to a PNG |

### 2D overlay

After `render_3d()` (or `begin2d()`) these draw on top, for a HUD:

| Method | Effect |
| --- | --- |
| `view.text(s, x, y, size, r, g, b)` | draw text |
| `view.rect(x, y, w, h, r, g, b)` | filled rectangle |
| `view.circle(x, y, radius, r, g, b)` | filled circle |
| `view.line(x0, y0, x1, y1, thick, r, g, b)` | line |
| `view.alpha(a)` | opacity (0–255) for subsequent overlay draws |

### Input

Input is polled from the view each frame. Keyboard keys and gamepad
axes/buttons are raw integer codes (raylib key codes; SDL game-controller
indices) — there are no named constants:

| Method | Result |
| --- | --- |
| `view.held(key) -> Bool` | key is down (e.g. `262`–`265` = arrows, `32` = space) |
| `view.pressed(key) -> Bool` | key went down this frame |
| `view.pad_available() -> Bool` | a gamepad is connected |
| `view.pad_axis(n) -> Float` | axis value (sticks, triggers) |
| `view.pad_button(n) -> Bool` / `pad_pressed(n) -> Bool` | button held / just pressed |
| `view.rumble(left, right, sec)` | haptics (Sony pads and XInput; Xbox-on-macOS is silent) |
| `view.pad_name() -> String` / `view.gamepad_mappings(db)` | pad identity / load an SDL mapping DB |

### Audio

`Scene.Sound.new(path)` is a one-shot effect; `Scene.Music.new(path)` is a
streamed track. Both take a file path and support `volume(v)`, `pitch(p)`, and
`pan(p)`. `Sound` adds `play` / `stop` / `playing`; `Music` adds `pause` /
`resume` / `looping(on)` and needs `update()` called each frame to keep its
buffer fed.

### A minimal scene

```culebra
# doctest: skip
let view = Scene.View.new(960, 540, "spinner")
view.target_fps(60)
view.background(30, 34, 42)
view.sun(0.5, -0.8, -0.3, 1.2, 255, 245, 230)
view.ambient(0.4, 180, 200, 220)

let gold = view.material_pbr(230, 180, 60, 0.9, 0.3)
let box = view.add_box(2.0, 2.0, 2.0).material(gold)

mut a = 0.0
while !view.closing() {
  a += view.dt()
  box.yaw(a)
  view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
  view.render_3d()
  view.text("culebra scene", 20.0, 20.0, 28, 235, 235, 240)
  view.present()
}
view.drop()
```

---

## 28. `Net`

Raw TCP and UDP sockets, plus name resolution — the layer under
[§15 Http](#15-http). Blocking, with an optional per-socket timeout: the same
threads-not-async model as `Http`, so a server that must not block the main
thread runs inside an [`Isolate`](#12-isolate).

A transport failure — refused connect, unresolvable host, reset peer, timeout —
raises `NetError`. The message is the OS's own wording, except a timeout, which
always reads `timed out`. A blocked `read` / `accept` / `recv_from` stays
interruptible: one Ctrl+C raises `Interrupted` rather than hanging.

Socket handles are **not Sendable** — a socket belongs to the thread that opened
it. To serve from another thread, open it inside that isolate.

### `Net.connect(host: String, port: Long, timeout: Long = 0) -> Socket`

Connect to `host:port`. `timeout` is in milliseconds (`0` = wait forever) and
bounds the connect, then becomes the socket's read/write timeout.

```culebra
# doctest: skip
let s = Net.connect("example.com", 80, timeout: 5000)
s.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
s.shutdown_write()                  # tell the server the request is complete
inspect(s.read())                      # read until the server closes
s.close()
```

**The `Socket` handle** — the reader/writer shape of [§4 File](#4-file), so
code that only reads works over either:

| Method | Effect |
| --- | --- |
| `read(n = nil)` | up to `n` bytes (a short read is normal on a socket); `nil` reads until the peer closes. `""` at EOF |
| `read_line()` | one line, terminator stripped; `nil` once the stream ends |
| `read_exact(n)` | exactly `n` bytes; a peer that closes early is a `NetError`, not a short read |
| `lines()` | line iterator, ending when the peer closes. Unlike a file's `f.lines()` it does **not** close the socket (you usually reply afterwards) |
| `write(data)` | write every byte of `data` |
| `shutdown_write()` | half-close: signal EOF to the peer while still reading its reply |
| `local_addr()` / `peer_addr()` | `{host, port}` of this end / the other end |
| `set_timeout(ms)` | read/write timeout in ms; `0` waits forever |
| `set_nodelay(on = true)` | disable Nagle's algorithm (send small writes immediately) |
| `is_open()` / `close()` | liveness; `close` is idempotent (the GC also closes one that goes out of scope) |

A line ends at `\n` only, and a trailing `\r` is stripped — so CRLF protocols
read cleanly. (A file's `f.lines()` also splits on a lone `\r`; a socket cannot, because
that would need a one-byte lookahead that can block when a CRLF arrives split
across two packets.)

### `Net.listen(port: Long, host: String = "0.0.0.0", backlog: Long = 0) -> Listener`

Bind and listen. `port: 0` asks the OS for a free port, readable back as
`listener.port` — the reliable way to avoid a fixed-port collision in tests.

```culebra
# doctest: skip
let server = Net.listen(7000)
println("listening on " + server.port.to_string())
for conn in server {                    # accept in a loop
  conn.write("hello " + conn.peer_addr().host + "\n")
  conn.close()
}
```

| Member | Effect |
| --- | --- |
| `port` / `host` | the address actually bound (an ephemeral port shows up here) |
| `accept()` | block until a connection arrives, returning a `Socket` |
| `for conn in listener` | accept in a loop; the body `break`s to stop |
| `serve(handler, workers = 0)` | accept in a loop and run `handler(conn)` on a worker pool; blocks until interrupted |
| `set_timeout(ms)` | bound how long `accept` waits before raising; accepted sockets inherit it |
| `is_open()` / `close()` | as on `Socket` |

`accept` and `for conn in listener` are sequential — the handling runs on the
accepting thread, so one slow connection blocks the next.

### `listener.serve(handler, workers = 0)` — the concurrent form

`serve` keeps accepting while handlers run on a pool of worker threads, so a
slow connection can't stall the accept loop:

```culebra
# doctest: skip
let server = Net.listen(7000)
server.serve(fn(conn) {
  for line in conn.lines() { conn.write(line.upper() + "\n") }
}, workers: 8)                      # blocks until Ctrl+C
```

- `workers: 0` (the default) picks a CPU-scaled pool (at least 4, at most 8);
  a positive count fixes it. Requests are handled in **true parallel** — each
  worker has its own runtime, with no global lock.
- Because each worker rebuilds the handler on its own heap, the handler must be
  **Sendable**: it can't capture mutable variables or non-Sendable values. A
  non-Sendable handler is a `SendError` raised by `serve` itself, not on the
  first connection. Share read-only data with [`Shared.new`](#12-isolate) and
  open per-connection resources inside the handler.
- The connection is closed when the handler returns. A handler that raises
  closes **only its own** connection — the worker and the server keep going
  (there is no response to turn into a `500`, unlike `Http.server`).
- `serve` blocks; Ctrl+C (or dropping the isolate it runs in) stops the accept
  loop, waits for the in-flight handlers, and raises `Interrupted`. To serve
  while the main thread works, run it inside an [`Isolate`](#12-isolate).

For HTTP specifically, prefer [`Http.server`](#httpserver---object) — it carries
routing, static files, streaming and WebSocket over the same worker model.

### `Net.udp(port: Long = 0, host: String = "0.0.0.0") -> UdpSocket`

Open a datagram socket bound to `host:port` (`port: 0` = ephemeral).

```culebra
# doctest: skip
let sock = Net.udp(9000)
sock.set_timeout(2000)
let msg = sock.recv_from()              # {data, host, port}
sock.send_to("ack", msg.host, msg.port)
```

| Member | Effect |
| --- | --- |
| `port` / `host` | the bound address |
| `send_to(data, host, port)` | send one datagram (whole or not at all — no partial write) |
| `recv_from(max = 65536)` | receive one datagram as `{data, host, port}`; an oversized datagram is truncated to `max`, as UDP dictates |
| `set_broadcast(on = true)` | allow sending to a broadcast address |
| `set_timeout(ms)` / `is_open()` / `close()` | as on `Socket` |

UDP has no EOF and no connection: an empty datagram is data, and a peer that
went away is silence, not an error.

### `Net.resolve(host: String) -> Array<String>`

The numeric addresses `host` resolves to, in resolver order, deduplicated. A
numeric address resolves to itself; a name that doesn't resolve is a `NetError`.

```culebra
# doctest: skip
inspect(Net.resolve("localhost"))       # => ["127.0.0.1", "::1"]
```

### Not available in the Playground

The browser has no raw sockets, so every `Net` call in a WebAssembly build
raises `NetError: networking is not available in this build`.

---

## 29. `Desktop` / `Webview`

A desktop GUI written in web tech: a local HTTP server supplies the UI, a
**native WebView** window displays it, and the whole thing ships as one
binary. `Webview` wraps the OS's own engine (WKWebView on macOS, WebKitGTK
on Linux, WebView2 on Windows) — no bundled browser. Both namespaces are
in the default build (`-DCULEBRA_ENABLE_WEBVIEW=OFF` opts out; they
self-disable when the engine's headers are absent — the GTK4 / WebKitGTK dev
packages on Linux, `WebView2.h` on Windows), and `culebra build` links the
WebView frameworks only for programs that actually reference them.

Those headers are a build-time requirement only. The binary loads its engine
when a window is created — through the vendored header's own loader on Windows,
through `dlopen` on Linux — so it starts on a machine that has none, and a
program that never opens a window runs there unaffected. Asking for a window
without an engine raises `webview: failed to create window`.

### `Desktop.run(config: Object) -> Nil`

The one-call facade: start the server, open the window on it, block until
the window closes, then stop the server.

| Key | Default | Meaning |
| --- | --- | --- |
| `title` | `'culebra'` | window title |
| `size` | window default | `[width, height]` in pixels |
| `assets` | — | static root served at `/` — typically `Embed.dir('dist')`, which reads from disk in dev and is baked into the binary under `culebra build` |
| `routes` | — | `fn (srv) { ... }` to register the app's own routes on the `Http` server (§15) |
| `port` | `8731`, then OS-assigned | loopback port the server binds. Unset: try `8731`; if it's taken (say, another culebra desktop app), fall back to an OS-assigned free port — note the page's origin, and so its `localStorage`, changes with the port. Set explicitly: bind exactly that port or fail |
| `workers` | `4` | server worker threads |

A `POST /__quit` route is registered for you, so the page can close the
app (`fetch('/__quit', {method: 'POST'})`); `Desktop.quit()` does the same
from culebra code.

```culebra
# doctest: skip
Desktop.run({
  title: 'culebra desktop',
  size: [720, 560],
  assets: Embed.dir('dist'),
  routes: fn (srv) {
    srv.get('/api/hello', fn (req) {
      { content_type: 'application/json',
        body: JSON.stringify({ message: 'hello' }) }
    })
  }
})
```

### `Webview.Window` — the raw binding

Use it directly when you don't want a server at all (inline HTML, or a
remote URL).

| Method | Effect |
| --- | --- |
| `Webview.Window.new()` | create a window |
| `w.set_title(title)` | set the title |
| `w.set_size(width, height)` | resize |
| `w.set_html(html)` | load an HTML string |
| `w.navigate(url)` | load a URL |
| `w.run()` | run the native event loop; blocks until terminated |
| `w.terminate()` | end this window's `run()` |
| `Webview.Window.quit()` | terminate whichever window is currently in `run()` — callable from another thread, e.g. an HTTP handler |
| `Webview.Window.is_running()` | `true` while a window is pumping its event loop. Not needed before `quit()` — a quit that arrives earlier is held — but lets another thread wait for the loop to be up |

```culebra
# doctest: skip
let w = Webview.Window.new()
w.set_title('hello')
w.set_size(480, 320)
w.set_html('<h1>hi from culebra</h1>')
w.run()
```

**When the window becomes visible.** On macOS the window stays transparent
until the page puts its first frame on screen, so an app opens on its own
content instead of flashing an empty white rectangle first. A page that never
reports a frame — JavaScript turned off, a navigation that never completes —
gets the window shown anyway after 1.5 s, blank rather than invisible. Windows
and Linux show the window as soon as `set_size` runs, so the empty frame is
still briefly visible there.

---

## 30. Design notes

### Namespace-first, CLI-aliased globals

The library adds **no global names beyond the matcher family**:
everything else lives under `Math`, `IO`, `Random`, or `Sys`. This
keeps `culebra::environment()` free of surprises for embedders who
use Culebra as a scripting engine inside a host application.

For CLI scripting, however, `inspect` / `print` / `println` are so
pervasive that writing `IO.inspect` everywhere adds friction. The CLI
binary (`src/main.cc`) installs them as globals right after
constructing the environment — pointing to the same function values
that live under `IO`, so there is no duplication. V8 takes an
analogous approach: the engine provides no `print`, and the `d8` shell
installs one.

### Namespaces as first-class values

Every stdlib namespace (`Math`, `IO`, `FS`, `Random`, `Sys`,
`Tensor`, `JSON`) is an `Object`. You can bind it to a name, pass
it as an argument, or store it in a collection, and method calls
on that binding observe the same semantics as direct calls:

```culebra
let io = IO
io.inspect("hello")              # same as IO.inspect("hello")

fn run_with(ns, x) { ns.inspect(x) }
run_with(IO, "via parameter")
```

Both backends honor self. The JIT/AOT slow path goes through a
runtime dispatcher (`stdlib_jit.h::kNsMethods`) while the syntactic
fast path (`IO.inspect(x)` directly) keeps its inlined IR emission.

### Free function vs method

Free functions (in namespaces) are used when the operation either
constructs a value from nothing (`iota`, `IO.input`) or applies
uniformly to multiple types (`type_of`, `to_string`). Operations
that are *about* a specific type use method syntax — see §18 of the
language spec for String/Array/Object methods.

### Error-by-throw versus `nil` returns

The library prefers throwing on unrecoverable type errors
(`to_long('abc')`, `FS.read(...)` on a missing file) and returning
sentinel values for "found or not" predicates (`IO.input()` returns
`''` on EOF). This keeps hot paths simple without requiring a
`try`/`catch` mechanism.

---

## 31. Not included (yet)

### Heavier data structures

No `Queue`, `Deque`, or priority-heap type. `Set` and `Tuple` are
language built-ins (see [`docs/language.md`](language.md)); reach for
`Array` and `Object` for everything else.

### OS extras

No file watcher, and no TLS on a raw socket (`Net` is plaintext;
[§15 Http](#15-http) carries its own TLS). Shell out through
[§11 Proc](#11-proc) when you need them.

---

See also: [`docs/language.md`](language.md) for the language
specification.
