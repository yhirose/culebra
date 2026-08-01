Culebra Programming Language
============================

> **Status:** pre-1.0 and under active development — APIs and syntax may change.

A scripting language small enough to embed, fast enough for Tensor,
simple enough for agents.

Run it as a script, JIT it for speed, or ship it as a standalone
binary — from one file, with no compile step:

```bash
culebra script.cul                        # run as a script (interpreter)
culebra --jit script.cul                  # JIT-compiled
culebra build script.cul -o app && ./app  # ship as a standalone binary
```

Download
--------

These always point at the newest release. Each archive holds the binary
and the license; `culebra --version` reports which release it is.

| Platform | Download |
|---|---|
| macOS (Apple Silicon) | [culebra-macos-arm64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz) |
| Linux (x86-64) | [culebra-linux-x64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-linux-x64.tar.gz) |
| Windows (x86-64) | [culebra-windows-x64.zip](https://github.com/yhirose/culebra/releases/latest/download/culebra-windows-x64.zip) |

Extracting from the command line also avoids the quarantine flag macOS
puts on anything unpacked through Finder — the binaries are unsigned:

```bash
curl -fsSL https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz | tar xz
sudo mv culebra-*/culebra /usr/local/bin/
culebra --version
```

Checksums and every release's notes are on the
[releases page](https://github.com/yhirose/culebra/releases).

Highlights
----------

### Small, fast, runs anywhere

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

File I/O, CLI argument parsing, structured data (JSON — with optional
JSONC comments and trailing commas — CSV, TOML), time, math, and
randomness ship in the single binary — no package manager, no lockfile.
Text processing (regex), an HTTP client, and hashing / encoding /
compression ship too, same policy.

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

Language features
-----------------

- **Pattern matching.** Literals, type guards, destructuring, and rest
  patterns, with optional `if` guards.
- **String interpolation.** `"head={head}, rest={tail.size()}"`.
- **Gradual typing.** Annotations runtime-checked at boundaries;
  Union, Optional, Tuple, Trait and Generic all check today.
- **UFCS.** Free functions callable as methods (`x.f(y)` ≡ `f(x, y)`).
- **Multiple dispatch.** Free functions dispatch on argument types
  across interp / JIT / AOT.
- **Traits.** Built-in `Iterable` / `Iterator`; user-defined traits
  with default methods.
- **Closures and `class` form.** Both supported, same encapsulation
  semantics.
- **Algebraic effects.** `effect fn` / `perform` / `handle … with`, with
  multi-shot `resume` — one mechanism covering generators, exceptions,
  dependency injection, and backtracking search.
- **Doctests in comments.** `1 + 1  # => 2` is an executable
  assertion under `culebra test`.
- **Structural assert diagnostics.** `assert_eq` prints a diff.
- **Defer / RAII.** Scope-bound cleanup.
- **Threaded concurrency.** No `async`/`await`; blocking I/O on
  shared-nothing threads.

Standalone binaries
-------------------

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops
the ~450 runtime helpers a program doesn't reference. Tensor-free
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

The rationale lives in [`docs/internals.md`](docs/internals.md).

- **Two backends, one AST.** Interpreter and JIT both maintained;
  no plan to consolidate.
- **Predictable threaded concurrency.** No `async`/`await`. Stack
  traces stay readable, the debugger sees every frame, and library
  authors don't write a colored version of every function. Targeted
  at services up to a few-thousand-connection ceiling — the shape
  SQLite, Redis, and most line-of-business backends already use.
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
* Tooling — the test runner, linter, formatter and debug adapter
  (`culebra test` / `lint` / `fmt` / `dap`):
  [`docs/tooling.md`](docs/tooling.md)
  / [日本語](docs/tooling.ja.md)
* Deployment — standalone binary build, embedding from C++, wrapping
  C++ libraries (`culebra build` / `culebra wrap`):
  [`docs/deployment.md`](docs/deployment.md)
  / [日本語](docs/deployment.ja.md)
* Implementation internals: [`docs/internals.md`](docs/internals.md)
  / [日本語](docs/internals.ja.md)
* Context pack — the syntax, the carried-over mistakes and every stdlib
  signature condensed into one file to paste into an LLM prompt:
  [`docs/llm.md`](docs/llm.md)
  / [日本語](docs/llm.ja.md)
* Agent rules — the same starting point in the form a coding agent
  reads, to append to `CLAUDE.md`, `.github/copilot-instructions.md` or
  `AGENTS.md` (`culebra docs agent`):
  [`docs/agent.md`](docs/agent.md)
  / [日本語](docs/agent.ja.md)

Building from source and running the tests: [`CONTRIBUTING.md`](CONTRIBUTING.md).

License
-------

[MIT](LICENSE)
