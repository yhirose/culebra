Culebra Programming Language
============================

> **Status:** under active development, not yet released — APIs and syntax may change.

A scripting language small enough to embed, fast enough for Tensor,
simple enough for agents.

Run it as a script, JIT it for speed, or ship it as a standalone
binary — from one file, with no compile step:

```bash
culebra script.cul                        # run as a script (interpreter)
culebra --jit script.cul                  # JIT-compiled
culebra build script.cul -o app && ./app  # ship as a standalone binary
```

Highlights
----------

### Small, fast, runs anywhere

- **CLI script.** Cold start in tens of milliseconds.
- **Standalone binary.** A few MB on macOS arm64 (vs tens of MB for
  comparable tools), no runtime dependencies.
- **Embedded library.** Embeds the interpreter into a C++ host, no
  LLVM dependency.
- **Cross-platform.** macOS / Linux / Windows; cross-compile to any
  LLVM target from any host.

### Friendly to agent loops

Fast cold start, single-file programs, and implicit imports add up
to a runtime that doesn't punish an agent for invoking it every turn.

- **Implicit imports.** No `import` lines for an agent (or human) to
  forget or misspell.
- **One file, one program.** Run it without a project layout.
- **Ship the result.** `culebra build` turns the script the agent just
  wrote into a binary it can hand back to the user.

### Immutable by default, mutability opt-in

A bare `x = 1` is an immutable binding — reassigning it is an error;
write `mut x = 1` for one you intend to change. No declaration keyword
for the common case.

### Batteries included, in one binary

File I/O, CLI argument parsing, structured data, time, math, and
randomness ship in the single binary — no package manager, no lockfile.
Text processing, networking, and crypto/encoding are next, same policy.

### Built-in Tensor

`Tensor` is a language primitive, not a library. Arithmetic and `@`
are operators; matmul routes through Accelerate / OpenBLAS.

```culebra
x = tensor([[1.0, 2.0], [3.0, 4.0]])
y = x @ x.transpose()                # BLAS-routed
```

Language features
-----------------

- **Pattern matching.** Literals, type guards, destructuring, and rest
  patterns, with optional `if` guards.
- **String interpolation.** `"head={head}, rest={tail.size()}"`.
- **Gradual typing.** Annotations runtime-checked at boundaries;
  Union / Optional / Tuple / Trait / Generic land pre-1.0.
- **UFCS.** Free functions callable as methods (`x.f(y)` ≡ `f(x, y)`).
- **Multiple dispatch.** Free functions dispatch on argument types
  across interp / JIT / AOT.
- **Traits.** Built-in `Iterable` / `Iterator`; user-defined traits
  with default methods.
- **Closures and `class` form.** Both supported, same encapsulation
  semantics.
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
the ~200 runtime helpers a program doesn't reference. Tensor-free
programs also drop the Accelerate / BLAS framework dependency.

```bash
culebra build path/to/script.cul -o ./out
./out                       # standalone, no LLVM at runtime
```

Cross-compile is also supported — see
[`docs/binary_build.md`](docs/binary_build.md).

Embedding in a C++ host
-----------------------

The interpreter (no LLVM dependency) embeds into a C++23 host
via a minimal environment API:

```cpp
#include <culebra.h>
#include <stdlib_interp.h>

int main() {
  auto env = culebra::environment({});  // stdlib bound

  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", "1 + 2", 5, msgs);

  culebra::Value val;
  culebra::interpret(ast, env, val, msgs, culebra::Debugger());
  // val.to_long() == 3
}
```

See [`docs/embedding.md`](docs/embedding.md) for the JIT path,
threading, and host-function registration.

Design choices
--------------

The rationale lives in [`docs/internals.md`](docs/internals.md)
(rejected designs in §13). Headline decisions:

- **Two backends, one AST.** Interpreter and JIT both maintained;
  no plan to consolidate.
- **Predictable threaded concurrency.** No `async`/`await`. Stack
  traces stay readable, the debugger sees every frame, and library
  authors don't write a colored version of every function. Targeted
  at services up to a few-thousand-connection ceiling — the shape
  SQLite, Redis, and most line-of-business backends already use.
- **Gradual typing without a compile step.** Annotations are
  runtime-checked at boundaries; startup stays instant. Union /
  Optional / Tuple / Trait / Generic are decided and pre-1.0 required.
- **Immutable by default, no declaration keyword.** A bare `x = 1`
  creates an *immutable* binding — reassigning it is an error. Use `mut`
  for a variable you intend to change (`mut x = 1; x = 2`), so `mut`
  marks exactly what changes; `let` is an optional, explicit marker for
  the immutable form. The default binding is both safe and ceremony-free,
  and mutation is visible where it happens.
- **UFCS, not pipeline.** `x.f(...)` doubles as the resolution path
  for free functions over user types.
- **Implicit imports.** No explicit `import` statement — the resolver
  walks unresolved identifiers across sibling files.
- **Batteries included.** Everyday scripting needs ship in the binary,
  not as third-party packages.
- **Pre-1.0.** No version tags, CHANGELOG, or package registry yet —
  surface still moves.

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
* Embedding from C++: [`docs/embedding.md`](docs/embedding.md)
  / [日本語](docs/embedding.ja.md)
* Wrapping C++ libraries (`culebra wrap`): [`docs/wrapping.md`](docs/wrapping.md)
  / [日本語](docs/wrapping.ja.md)
* Standalone binary build: [`docs/binary_build.md`](docs/binary_build.md)
  / [日本語](docs/binary_build.ja.md)
* Implementation internals (en only): [`docs/internals.md`](docs/internals.md)

Building from source and running the tests:
[`CONTRIBUTING.md`](CONTRIBUTING.md).

License
-------

[MIT](LICENSE)
