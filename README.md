Culebra Programming Language
============================

A small, dynamically-typed scripting language with Rust-inspired
syntax, written in C++23. Two backends share one AST: a tree-walking
interpreter and an LLVM ORC JIT.

* `let` / `mut` / `fn` / `|x| expr` / `match`, string interpolation,
  first-class closures.
* Eight built-in types, no hidden globals. Refcount + cycle GC.
  Per-callsite inline caches and HOF fusion in the JIT.
* Same AST runs on the tree-walking interpreter (no LLVM needed) or
  on LLVM ORC at `-O2`.
* `~1 MB` interpreter binary for embedding, or full JIT for
  performance-critical loops. CLI exposes `puts` / `Sys.argv`;
  embedders work with a minimal environment.
* Type-annotated operator overloading, intended for users coming
  from Ruby or Python.

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

### Pure scalar: MNIST MLP

[`samples/mnist`](samples/mnist/) trains a 784–30–10 sigmoid MLP on
MNIST and benchmarks both inference and training. Total wall time,
mean of 5 runs:

**Inference** (1000 test images)

| implementation   |  time   | ratio (vs pure) |
|------------------|--------:|----------------:|
| numpy (BLAS)     |  0.22 s |          0.18×  |
| pure Python      |  1.20 s |          1.00×  |
| Culebra `--jit`  |  1.11 s |          0.93×  |

**Training** (1 epoch, mini-batch SGD)

| implementation   | N=1000  | N=5000  | N=10000 |
|------------------|--------:|--------:|--------:|
| numpy (BLAS)     |  0.24 s |  0.44 s |  0.63 s |
| pure Python      |  3.97 s | 15.17 s | 28.94 s |
| Culebra `--jit`  |  7.07 s | 15.39 s | 25.81 s |

All three agree on predictions (inference accuracy 0.954, training
0.622 / 0.867 / 0.886). JIT beats pure Python on inference, ties at
N=5000 training, and pulls 11% ahead by N=10000 — the JIT's fixed
per-module codegen cost (~3–4 s) takes a few thousand mini-batches
to amortize. See [`samples/mnist/README.md`](samples/mnist/README.md)
for the full analysis.

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
