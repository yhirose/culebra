Culebra Programming Language
============================

> **Status:** under active development, not yet released — APIs and syntax may change.

A scripting language small enough to embed, fast enough for Tensor,
simple enough for agents.

Run it as a script, ship it as a binary, or embed it in your app —
without a compile step.

```culebra
# hello.cul
describe = fn (v) {
  match v {
    0                  => 'zero',
    n: Long if n > 100 => "big ({n})",
    [head, ...tail]    => "head={head}, rest={tail.size()}",
    {name, age}        => "{name}, {age}",
    "hello"            => "world!",
    _                  => 'other'
  }
}

puts(describe("hello"))
```

```bash
> culebra hello.cul # Interpreter
world!

> culebra --jit hello.cul # JIT
world!

> culebra build hello.cul -o hello # AOT
> ./hello
world!
```

Why Culebra
-----------

### Small, fast, runs anywhere

- **CLI script.** 28 ms cold start (50 ms with `--jit`).
- **Standalone binary.** 350 KB (vs tens of MB for comparable tools),
  2 ms startup, 98 ms to build.
- **Embedded library.** ~1 MB, embeds into a C++ host.
- **Cross-platform.** macOS / Linux / Windows; cross-compile to any
  LLVM target from any host.

### Friendly to agent loops

Cold start in 28 ms, single-file programs, and implicit imports add up
to a runtime that doesn't punish an agent for invoking it every turn.

- **Implicit imports.** No `import` lines for an agent (or human) to
  forget or misspell.
- **One file, one program.** Run it without a project layout.
- **Doctests in comments.** `1 + 1  # => 2` is an executable assertion
  under `culebra test`.
- **Structural assert diagnostics.** `assert_eq` prints a diff, not a
  boolean.
- **Ship the result.** `culebra build` turns the script the agent just
  wrote into a binary it can hand back to the user.

### Batteries included, in one binary

Common scripting needs — file I/O, CLI argument parsing, structured
data, time, math, randomness — ship in the single binary. Text
processing, networking, and crypto/encoding are next on the roadmap,
same single-binary policy. No package manager, no lockfile.

### Built-in Tensor

`Tensor` is a language primitive, not a library. Arithmetic and `@`
are operators; matmul routes through Accelerate / OpenBLAS.

```culebra
x = tensor([[1.0, 2.0], [3.0, 4.0]])
y = x @ x.transpose()                # BLAS-routed
```

Where Culebra is at home
------------------------

- **LLM agent glue.** Cold start in 28 ms, batteries-included stdlib,
  implicit imports, ships as a 350 KB binary the agent can hand to
  the user.
- **Embedded scripting in C++ apps.** Modern syntax, ~1 MB footprint,
  no separate language runtime to install.
- **Modern Unix CLI tools.** `fd` / `ripgrep`-class utilities,
  distributed as static binaries.
- **Small-scale services.** Threaded blocking I/O up to a few thousand
  concurrent connections — the shape most line-of-business services
  actually need.

Language features
-----------------

- **Pattern matching.** Literals, type guards, destructuring, rest
  patterns — see the opening example.
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

Build and ship
--------------

Requires a C++23 compiler and `just`. JIT also needs LLVM 17+.

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB binary
just dev                # fast no-LTO build into build-dev/ (inner loop)
just test-dev           # quick interp==JIT check vs build-dev/ (after each edit)
just test               # all backends + embedding smoke (commit gate)

./build/culebra --shell                   # REPL  (--jit for JIT REPL)
./build/culebra        path/to/script.cul # interpreter
./build/culebra --jit  path/to/script.cul # JIT
```

### Standalone binaries

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops
the ~200 runtime helpers a program doesn't reference. Tensor-free
programs also drop the Accelerate / BLAS framework dependency.

```bash
./build/culebra build path/to/script.cul -o ./out
./out                       # standalone, ~350 KB on macOS
```

Cross-compile is also supported — see
[`docs/binary_build.md`](docs/binary_build.md).

### Embedding in a C++ host

The interpreter (~1 MB, no LLVM dependency) embeds into a C++23 host
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
  runtime-checked at boundaries; the REPL stays at 28 ms. Union /
  Optional / Tuple / Trait / Generic are decided and pre-1.0 required.
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
  loops. Run `just perf` for current numbers.
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
* Standalone binary build: [`docs/binary_build.md`](docs/binary_build.md)
  / [日本語](docs/binary_build.ja.md)
* Implementation internals (en only): [`docs/internals.md`](docs/internals.md)

License
-------

[MIT](LICENSE)
