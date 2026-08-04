Culebra Programming Language
============================

[![CI](https://github.com/yhirose/culebra/actions/workflows/ci.yml/badge.svg)](https://github.com/yhirose/culebra/actions/workflows/ci.yml)

> **Status:** pre-1.0 and under active development — APIs and syntax may change.

A dynamically-typed cross-platform scripting language with three
backends: an interpreter, an LLVM JIT, and an ahead-of-time build that
emits a standalone binary. Write scripts, CLI tools, machine learning,
desktop apps, and games.

The stdlib, test runner, linter, formatter, debugger and docs are all
in that one executable. Nothing else to install!

Quickstart
----------

Paste this into a terminal on macOS (Apple Silicon) — it downloads
culebra, writes an example to `hello.cul`, and runs that same source
file three ways: as a script, JIT-compiled, and built into a
standalone binary.

```bash
curl -fsSL https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz | tar xz
export PATH="$PWD/culebra-macos-arm64:$PATH"
culebra --version

cat > hello.cul <<'EOF'
let people = [
  { name: 'Taro', greeting: 'Konnichiwa' },
  { name: 'John', greeting: 'Hello' },
  { name: 'Ada',  greeting: 'Bonjour' },
]

for p in people.sorted_by(|p| p.name) {
  println("{p.greeting}, {p.name}!")
}
EOF

culebra hello.cul                            # interpreter
culebra --jit hello.cul                      # JIT
culebra build hello.cul -o hello && ./hello  # AOT: compile once, ship the binary
```

Optional — this one also touches your editor's config
(VSCode/Vim/Neovim) and writes `AGENTS.md`/`CLAUDE.md` into the
current directory, so it isn't bundled into the block above:

```bash
culebra init
```

`culebra init` installs syntax highlighting and the debug adapter for
whichever of VSCode, Vim, or Neovim it finds on this machine, and adds
coding-agent instructions to `AGENTS.md` (or
`CLAUDE.md`/`.github/copilot-instructions.md` if one already exists) —
reopen `hello.cul` afterward and the highlighting is already on, and
Claude Code or another agent reading `AGENTS.md` already knows the
conventions.

For Linux (x86-64), swap the first line for `culebra-linux-x64.tar.gz`;
Windows and a permanent, system-wide install are below.

| Platform | Download |
|---|---|
| macOS (Apple Silicon) | [culebra-macos-arm64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz) |
| Linux (x86-64) | [culebra-linux-x64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-linux-x64.tar.gz) |
| Windows (x86-64) | [culebra-windows-x64.zip](https://github.com/yhirose/culebra/releases/latest/download/culebra-windows-x64.zip) |

These always point at the newest release; `culebra --version` reports
which one you have. Extracting from the command line also avoids the
quarantine flag macOS puts on anything unpacked through Finder — the
binaries are unsigned. To put `culebra` on `PATH` permanently instead
of just for this shell:

```bash
sudo mv culebra-*/culebra /usr/local/bin/
```

Checksums and every release's notes are on the
[releases page](https://github.com/yhirose/culebra/releases).

Highlights
----------

### Cold start, single binary, cross-platform

- **CLI script.** Cold start in tens of milliseconds.
- **Standalone binary.** `culebra build` emits a single executable —
  nothing to install alongside it, no runtime dependencies.
- **Embedded library.** Embeds the interpreter into a C++ host.
- **Cross-platform.** macOS / Linux / Windows; cross-compile to any
  LLVM target from any host.

### Friendly to agent loops

Fast cold start, single-file programs, and a stdlib that is already in
scope add up to a runtime that doesn't punish an agent for invoking it
every turn.

- **No import lines for the stdlib.** `JSON`, `Http`, `FS`, `Tensor`
  and the rest are bound before the program runs.
- **One file, one program.** Run it without a project layout.
- **Ship the result.** `culebra build` turns the script the agent just
  wrote into a binary it can hand back to the user.

### Immutable by default, mutability opt-in

A bare `x = 1` is an immutable binding — reassigning it is an error;
write `mut x = 1` for one you intend to change. No declaration keyword
for the common case.

### Batteries included, in one binary

The whole standard library is bound before the program runs — no
package manager, no lockfile:

- **Data.** JSON (with optional JSONC comments and trailing commas),
  CSV, TOML, `.env`, UUID, and SQLite — the amalgamation is compiled
  in, so there is no system library to install.
- **Text.** Grapheme-aware regex, encodings (base64 / hex / url / HTML
  entities), SHA / MD5 digests and HMAC, gzip / deflate.
- **System.** Paths and whole-file I/O, streaming file handles,
  subprocesses, time, math, randomness, CLI argument parsing, and
  leveled structured logging.
- **Network.** An HTTP/HTTPS client and server (routes, static files,
  Server-Sent Events, WebSocket), plus raw TCP / UDP sockets and name
  resolution underneath.
- **Concurrency.** Isolates, channels, `Parallel`, shared buffers, and
  Ctrl+C delivered as a channel message.
- **Terminal.** `Term` — colour, cursor control, the alternate screen,
  and key/mouse input for TUIs, downsampled to whatever the terminal
  supports (and silent under `NO_COLOR`).
- **3D.** `Scene` — a retained-mode 3D renderer for procedural geometry
  with physically based lighting. Opt-in
  (`-DCULEBRA_ENABLE_SCENE=ON`) and macOS-only today.

### Built-in Tensor

`Tensor` is a language primitive, not a library. Arithmetic is
operators over a lazy graph that `Tensor.eval` materializes; matmul is
`.dot()`. Ops run on the CPU (AVX2 / NEON kernels, Accelerate on
macOS) or the GPU (Metal on macOS, CUDA where `nvcc` built it in),
picked per op by size unless a `Tensor.use_*()` call pins one.

```culebra
x = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
y = x.dot(x.transpose())
Tensor.eval(y)                       # [[5.0, 11.0], [11.0, 25.0]]
```

### Embedded assets

`Embed.dir(name)` hands a directory of files to your program with no
code change across backends: the interpreter and JIT read it live
from disk — edit a file, rerun — and `culebra build` bakes every byte
into the executable, so the shipped binary needs nothing alongside it.

```culebra
let assets = Embed.dir("dist")           # index.html, favicon.ico, ...
println(assets.exists("index.html"))     # => true
println(assets.exists("favicon.ico"))    # => true
```

### Desktop app creation

A desktop GUI in web tech: a local HTTP server supplies the UI, the
OS's own WebView engine displays it, and `culebra build` ships the
whole thing — server, routes, and embedded assets — as one binary.

```culebra
Desktop.run({
  title: "Hello from culebra",
  assets: Embed.dir("dist"),             # index.html, favicon.ico, ...
  routes: fn (srv) {
    srv.get("/api/hello", fn (req) { "hi from the embedded server" })
  },
})
```

### 2D Canvas

An immediate-mode 2D framebuffer — draw, `present`, poll input,
repeat — with sprites, text, and tone/music. It opens a real window on
macOS, Linux and Windows; a run that declares itself headless does the
same pixel work and displays nothing, which is how the tests and a
displayless server run it.

```culebra
Canvas.run(160, 160, fn () {
  Canvas.clear(Canvas.rgba(20, 24, 40))
  Canvas.rect(20, 76, 8, 8, Canvas.rgba(220, 60, 60))
  false  # stop after one frame
})
```

Language features
-----------------

- **Pattern matching.** Literals, type guards, destructuring, and rest
  patterns, with optional `if` guards.
- **Sum types.** `enum Shape { Circle(Float), Rect(Float, Float) }`,
  generic where you need it, matched by variant or by enum.
- **Tuples and sets.** `(3, 4)` is immutable and hashable; `{1, 2, 3}`
  is an insertion-ordered set of unique values.
- **String interpolation.** `"head={head}, rest={tail.size()}"`, with
  format specs, triple-quoted blocks that dedent, and `re"…"` regex
  literals.
- **Gradual typing.** Annotations runtime-checked at boundaries;
  Union, Optional, Tuple, Trait and Generic all check today.
- **UFCS.** Free functions callable as methods (`x.f(y)` ≡ `f(x, y)`).
- **Multiple dispatch.** Free functions dispatch on argument types
  across interp / JIT / AOT.
- **Traits.** Built-in `Iterable` / `Iterator`; user-defined traits
  with default methods.
- **Closures and `class` form.** Both supported, same encapsulation
  semantics.
- **Generators.** A `fn` whose body contains `yield` returns an
  Iterator; `yield from` delegates to any iterable.
- **Algebraic effects.** `effect fn` / `perform` / `handle … with`, with
  multi-shot `resume` — one mechanism covering generators, exceptions,
  dependency injection, and backtracking search.
- **Decorators.** `@expr` before a `fn` or `class` wraps the declared
  value before it is bound.
- **Doctests in comments.** `1 + 1  # => 2` is an executable
  assertion under `culebra test`.
- **Structural assert diagnostics.** `assert_eq` prints a diff.
- **Defer / RAII.** Scope-bound cleanup.
- **Threaded concurrency.** No `async`/`await`. `Isolate.spawn` runs a
  closure on its own thread with its own heap, values cross by copy,
  and `Channel` carries them — so two isolates cannot race on one
  object. `SharedBuffer` (fixed-layout, zero-copy) and `Shared`
  (immutable, by reference) opt out of the copy where it matters.

The toolchain is the same binary
--------------------------------

Every subcommand below is part of the executable that runs a program.

| Command | What it does |
|---|---|
| `culebra test [paths...]` | run tests and the doctests written in comments |
| `culebra lint [paths...]` | report static problems without running the program |
| `culebra fmt [paths...]` | reformat source to the canonical style |
| `culebra dap` | speak the Debug Adapter Protocol over stdio (VSCode / Vim / Zed) |
| `culebra docs [topic]` | read and search the embedded reference docs |
| `culebra build <in.cul> -o <out>` | compile ahead-of-time into a standalone executable |
| `culebra wrap` | build an extended binary exposing your own C++ classes |

Details are in [`docs/tooling.md`](docs/tooling.md).

Standalone binaries
-------------------

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops
the ~500 runtime helpers a program doesn't reference. Tensor-free
programs also drop the Accelerate / Metal framework dependency.

```bash
culebra build path/to/script.cul -o ./out
./out
```

Cross-compile is also supported. Binary sizes per feature axis are in
[`docs/deployment.md`](docs/deployment.md#1-standalone-binary-build-culebra-build).

Embedding in a C++ host
-----------------------

The interpreter (no LLVM dependency) embeds into a C++23 host
via a minimal environment API:

```cpp
#include <culebra.h>
#include <stdlib_interp.h>

int main() {
  auto env = culebra::environment();  // stdlib bound

  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", "1 + 2", 5, msgs);

  culebra::Value val;
  culebra::interpret(ast, env, val, msgs, culebra::Debugger());
  // val.to_long() == 3
}
```

See [`docs/deployment.md`](docs/deployment.md#2-embedding-culebra-in-a-c-host)
for the JIT path, threading, and host-function registration.

Design choices
--------------

- **Two backends, one AST.** Interpreter and JIT both maintained;
  no plan to consolidate.
- **Predictable threaded concurrency.** No `async`/`await`. Stack
  traces stay readable, the debugger sees every frame, and library
  authors don't write a colored version of every function. Targeted
  at services up to a few-thousand-connection ceiling — the shape
  SQLite, Redis, and most line-of-business backends already use. The
  reasoning is written out in
  [`docs/essays/concurrency.md`](docs/essays/concurrency.md).
- **Gradual typing without a compile step.** Annotations are
  runtime-checked at boundaries; startup stays instant. Union,
  Optional, Tuple, Trait and Generic annotations are all in.
- **Immutable by default, no declaration keyword.** A bare `x = 1`
  creates an *immutable* binding — reassigning it is an error. Use `mut`
  for a variable you intend to change (`mut x = 1; x = 2`), so `mut`
  marks exactly what changes; `let` is an optional, explicit marker for
  the immutable form. The default binding is both safe and ceremony-free,
  and mutation is visible where it happens.
- **UFCS, not pipeline.** `x.f(...)` doubles as the resolution path
  for free functions over user types.
- **The stdlib needs no import.** Namespaces are bound before the
  program runs, so a one-file script has no import block. Splitting
  across files uses top-level `import` / `export`, which keeps the
  dependency graph known at parse time — what AOT bundling and
  tree-shaking need.
- **Batteries included.** Everyday scripting needs ship in the binary,
  not as third-party packages.
- **Pre-1.0.** Releases follow semver's 0.x reading: a minor bump may
  break things. No package registry yet — download a binary or build it.

Performance
-----------

Culebra targets three forms of performance: fast startup, small
binaries, and BLAS-class numerics.

- Tensor lands in the BLAS-bound cluster with numpy / Julia / PyTorch
  CPU on the MNIST MLP. See [`benchmarks/mnist/`](benchmarks/mnist/)
  and [`benchmarks/microgpt/`](benchmarks/microgpt/).
- JIT is tens of times faster than the interpreter on compute-bound
  loops.
- Allocator-heavy code can favor the interpreter.

Documentation
-------------

* The Culebra Guide: [`docs/guide.md`](docs/guide.md)
  / [日本語](docs/guide.ja.md)
* Language specification: [`docs/language.md`](docs/language.md)
  / [日本語](docs/language.ja.md)
* Standard library reference: [`docs/stdlib.md`](docs/stdlib.md)
  / [日本語](docs/stdlib.ja.md)
* Tooling — the test runner, linter, formatter, debug adapter and
  embedded docs (`culebra test` / `lint` / `fmt` / `dap` / `docs`):
  [`docs/tooling.md`](docs/tooling.md)
  / [日本語](docs/tooling.ja.md)
* Deployment — standalone binary build, embedding from C++, wrapping
  C++ libraries (`culebra build` / `culebra wrap`):
  [`docs/deployment.md`](docs/deployment.md)
  / [日本語](docs/deployment.ja.md)
* Context pack — the syntax, the carried-over mistakes and every stdlib
  signature condensed into one file to paste into an LLM prompt:
  [`docs/llm.md`](docs/llm.md)
  / [日本語](docs/llm.ja.md)
* Agent rules — the same starting point in the form a coding agent
  reads, to append to `CLAUDE.md`, `.github/copilot-instructions.md` or
  `AGENTS.md` (`culebra docs agent`):
  [`docs/agent.md`](docs/agent.md)
  / [日本語](docs/agent.ja.md)
* Essays — how a design decision was reached, at the length the
  specification has no room for:
  [`docs/essays/`](docs/essays/)

Building from source and running the tests: [`CONTRIBUTING.md`](CONTRIBUTING.md).

License
-------

[MIT](LICENSE)
