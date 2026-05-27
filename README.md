Culebra Programming Language
============================

A scripting language small enough to embed, fast enough for Tensor,
simple enough for agents.

Run it as a script, ship it as a binary, or embed it in your app —
without a compile step.

```culebra
describe = fn (v) {
  match v {
    0                  => 'zero',
    n: Long if n > 100 => "big ({n})",
    [head, ...tail]    => "head={head}, rest={tail.size()}",
    {name, age}        => "{name}, {age}",
    _                  => 'other'
  }
}
```

Why Culebra
-----------

### Small, fast, runs anywhere

| form                                | size   | startup |
|-------------------------------------|-------:|--------:|
| CLI script (`culebra script.cul`)   | —      | 28 ms   |
| `--jit`                             | —      | 50 ms   |
| Standalone binary (`culebra build`) | 350 KB | 98 ms   |
| Embedded in a C++ host              | ~1 MB  | —       |

Python imports `json` in more time than a Culebra script finishes.
Static binaries from comparable tools land in the tens of MB; Culebra
ships in 350 KB. No language runtime to install, no virtualenv.

### Numerics, in the language

`Tensor` is a primitive, not a library. Arithmetic and `@` are language
operators; matmul routes through Accelerate / OpenBLAS.

```culebra
x = tensor([[1.0, 2.0], [3.0, 4.0]])
y = x @ x.transpose()                # BLAS-routed
```

On the MNIST MLP, inference is ~2× faster than numpy (F64); training
lands in the same BLAS-bound cluster as numpy / Julia / PyTorch CPU.
See [`benchmarks/mnist/`](benchmarks/mnist/).

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

At a glance
-----------

```culebra
# Closures capture and mutate outer scope
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }
}
c = make_counter(); puts(c()); puts(c())     # 1, 2

# `class` form — same encapsulation, more concise
class Counter {
  new()  { this.n = 0 }
  tick() { this.n = this.n + 1; this.n }
}
c = Counter.new(); puts(c.tick()); puts(c.tick())   # 1, 2

# Lambda shorthand + iterator chain
[1, 2, 3, 4].map(|x| x * x).filter(|x| x > 4)       # [9, 16]
```

More examples in [`docs/guide.md`](docs/guide.md).

Performance
-----------

Culebra targets three forms of performance: fast startup, small
binaries, and BLAS-class numerics. Scalar throughput against mature
JITs (V8, Julia) is not a claim Culebra makes.

- Tensor lands in the BLAS-bound cluster with numpy / Julia / PyTorch
  CPU on the MNIST MLP. See [`benchmarks/mnist/`](benchmarks/mnist/)
  and [`benchmarks/microgpt/`](benchmarks/microgpt/).
- JIT is tens of times faster than the interpreter on compute-bound
  loops. Run `just perf` for current numbers.
- Allocator-heavy code can favor the interpreter.

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
- **Batteries-included, tiered.** Core stdlib (Math/IO/Sys/FS/Time/
  Args/Random/String) ships today; Tier 1 (Regex/HTTP/Hash+Encoding)
  is next.
- **Pre-1.0.** No version tags, CHANGELOG, or package registry yet —
  surface still moves.

Where Culebra is at home
------------------------

- **LLM agent glue.** Cold start in 28 ms, HTTP / JSON in stdlib,
  implicit imports, ships as a 350 KB binary the agent can hand to
  the user.
- **Embedded scripting in C++ apps.** Modern syntax, ~1 MB footprint,
  no separate language runtime to install.
- **Modern Unix CLI tools.** `fd` / `ripgrep`-class utilities,
  distributed as static binaries.
- **Small-scale services.** Threaded blocking I/O up to a few thousand
  concurrent connections — the shape most line-of-business services
  actually need.

Where it isn't: 10k+ concurrent connections, compile-time type
guarantees, mainstream ecosystem reach. Other languages cover those
well; Culebra doesn't try to.

Build and ship
--------------

Requires a C++23 compiler and `just`. JIT also needs LLVM 17+
(`brew install llvm` / `apt install llvm-17-dev` / `dnf install
llvm-devel` / `winget install LLVM.LLVM`).

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB binary
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
otool -L ./out              # no Accelerate, no LLVM
```

Cross-compile is supported via `--target=<triple>` (LLVM triple)
with user-provided `--sysroot=` and `--rt-lib=`. See
[`docs/binary_build.md`](docs/binary_build.md) for the full workflow.

### Embedding in a C++ host

The interpreter (~1 MB, no LLVM dependency) embeds into a C++23 host
via a minimal environment API. See [`docs/embedding.md`](docs/embedding.md).

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
