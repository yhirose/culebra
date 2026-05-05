Culebra Programming Language
============================

A small, dynamically-typed scripting language with Rust-inspired
syntax, written in C++23. Two backends share one AST: a tree-walking
interpreter and an LLVM ORC JIT.

**Why Culebra**

* **Familiar syntax.** `let` / `mut` / `fn` / `|x| expr` / `match`,
  string interpolation, first-class closures.
* **Small surface, capable engine.** Eight types, no hidden globals;
  refcount + cycle GC; per-callsite inline caches and HOF fusion in
  the JIT. Same AST runs on the interpreter (no LLVM needed) or on
  LLVM ORC at `-O2`.
* **Embed or script.** `~1 MB` interpreter binary, or full JIT for
  performance-critical loops. CLI exposes `puts` / `Sys.argv`;
  embedders work with a minimal environment.

Intended audience: developers who want a language similar in style
to Ruby or Python, but with type-annotated operator overloading and
a native-code JIT built on LLVM ORC — a codebase suitable for study
and extension.

Performance
-----------

`just bench-all` on Apple Silicon (`-O2`, LLVM 22):

| benchmark          | interp | jit    | speedup |
|--------------------|-------:|-------:|--------:|
| fib(0..33)         | ~8.2s  | ~0.17s | ~48×    |
| sum(0..10,000,000) | ~5.8s  | ~0.15s | ~39×    |
| closure_counter    | ~1.2s  | ~0.05s | ~24×    |
| array_push (100k)  | ~0.29s | ~0.08s | ~3.6×   |
| object_churn (100k)| ~0.16s | ~0.26s | ~0.6×   |

The JIT is faster on compute-bound workloads; allocator-heavy
and trivial code may favor the interpreter.

### End-to-end: microgpt

[`samples/microgpt`](samples/microgpt/) ports Karpathy's scalar
autograd microgpt. Apple Silicon, single core, training only
(mean of 5 runs):

| step | Python  | Culebra `--jit` | ratio (Cul/Py) |
|-----:|--------:|----------------:|---------------:|
|   20 |  1.39 s |          1.99 s |          1.43× |
|  100 |  6.81 s |          5.77 s |          0.85× |
|  200 | 13.06 s |         10.85 s |          0.83× |

Below ~50 steps Python wins because Culebra pays ~1 s of JIT
warmup. Past that, Culebra runs ~17% faster per step. See
[`samples/microgpt/README.md`](samples/microgpt/README.md) for
the full breakdown.

### Pure inference: MNIST MLP

[`samples/mnist`](samples/mnist/) trains a 784–30–10 sigmoid MLP on
MNIST (numpy) and runs inference for 1000 test images in four
implementations. Total wall time, mean of 5 runs:

| implementation     |  time   | ratio (vs pure) |
|--------------------|--------:|----------------:|
| numpy (BLAS)       |  0.34 s |          0.27×  |
| pure Python        |  1.24 s |          1.00×  |
| Culebra `--jit`    |  1.24 s |          1.00×  |
| Culebra interp     | 28.03 s |          22.6×  |

All four agree on predictions (accuracy 0.954). Unlike microgpt, this
workload is pure scalar forward pass — no Object dispatch — so JIT
ties pure Python rather than beating it. The numpy column is BLAS,
included as a reference; closing that gap requires a built-in matrix
primitive, which is on the Culebra roadmap (CUDA / MSL backed). See
[`samples/mnist/README.md`](samples/mnist/README.md) for the breakdown.

At a glance
-----------

```culebra
# Closures capture and mutate outer scope
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }
}
c = make_counter(); puts(c()); puts(c())   # 1, 2

# `class` form — same encapsulation, more concise; carries a `class:` tag
class Counter {
  new()  { this.n = 0 }
  tick() { this.n = this.n + 1; this.n }
}
c = Counter.new(); puts(c.tick()); puts(c.tick())   # 1, 2

# Pattern matching
describe = fn (v) {
  match v {
    0                  => 'zero',
    n: Long if n > 100 => "big ({n})",
    [head, ...tail]    => "head={head}, rest={tail.size()}",
    {name, age}        => "{name}, {age}",
    _                  => 'other'
  }
}

# Lambda shorthand + iterator chain
[1, 2, 3, 4].map(|x| x * x).filter(|x| x > 4)   # [9, 16]
```

More examples in [`samples/`](samples/).

Documentation
-------------

* Tutorial (5 min): [`docs/tutorial.md`](docs/tutorial.md)
  / [日本語](docs/tutorial.ja.md)
* Language specification: [`docs/language.md`](docs/language.md)
  / [日本語](docs/language.ja.md)
* Standard library reference: [`docs/stdlib.md`](docs/stdlib.md)
  / [日本語](docs/stdlib.ja.md)

Build
-----

Requires a C++23 compiler and `just`. JIT also needs LLVM 17+
(`brew install llvm` / `apt install llvm-17-dev` / `dnf install
llvm-devel` / `winget install LLVM.LLVM`).

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB binary
just test
./build/culebra --shell                # REPL  (--jit for JIT REPL)
./build/culebra        samples/fib.cul # interpreter
./build/culebra --jit  samples/fib.cul # JIT
```

License
-------

[MIT](LICENSE)
