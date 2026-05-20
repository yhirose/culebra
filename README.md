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

[`samples/microgpt`](samples/microgpt/) ports Karpathy's autograd
microgpt as both a scalar `Value` graph and a Tensor port. Apple
Silicon, training only:

**Scalar `Value` autograd, 100 training steps** (mean of 5 runs)

| implementation         |   time |  ms/step |
|------------------------|-------:|---------:|
| Python                 | 6.81 s |   ~63 ms |
| Culebra `--jit` scalar | 5.77 s |   ~52 ms |

Below ~50 steps Python wins because Culebra pays ~1 s of JIT
warmup. Past that, Culebra runs ~17% faster per step.

**Tensor port, 100 training steps** (single run)

| implementation         |   time |  ms/step |
|------------------------|-------:|---------:|
| Culebra `--jit` scalar | 6.21 s |   ~52 ms |
| Culebra Tensor         | ~1.6 s |  ~3.5 ms |

The Tensor port is **~15× faster per step** than the scalar
version: thousands of `Value` allocations per step → a few hundred
`TNode`s per step plus BLAS-routed linear layers. See
[`samples/microgpt/README.md`](samples/microgpt/README.md) for the
full breakdown.

### MNIST MLP

[`samples/mnist`](samples/mnist/) trains a 784–30–10 sigmoid MLP on
MNIST and benchmarks across seven implementations: numpy, pure
Python, PyTorch CPU/MPS, Julia, Culebra `--jit` (scalar), and Culebra
Tensor. Mean of 3 external runs.

**Inference** (warm, 10000 test images)

| implementation       |    time |
|----------------------|--------:|
| numpy (BLAS, F64)    | 0.022 s |
| pure Python          | 10.90 s |
| PyTorch CPU (F32)    | 0.003 s |
| PyTorch MPS (F32)    | 0.002 s |
| Julia (F64)          | 0.007 s |
| Culebra `--jit`      |  7.41 s |
| Culebra Tensor (F32) | 0.002 s |

**Training** (1 epoch, 10000 samples)

| implementation          |    time |
|-------------------------|--------:|
| numpy (BLAS, F64)       | 0.080 s |
| pure Python (cold¹)     | 27.25 s |
| PyTorch CPU (F32)       | 0.065 s |
| PyTorch MPS (F32)       | 0.273 s |
| Julia (F64)             | 0.075 s |
| Culebra `--jit` (cold¹) | 19.45 s |
| Culebra Tensor (F32)    | 0.077 s |

¹ scalar epochs use `CYCLES=1`; only the cold cycle is reported.

All seven agree on predictions (inference 0.9551, training 0.9079;
Culebra Tensor lands at 0.9081, FP-epsilon away from the F64
reference). The Tensor port routes matmul through Apple Accelerate
/ OpenBLAS and lands in the BLAS-bound cluster — within 1.2× of
PyTorch CPU on training warm. PyTorch MPS is 3.4× slower than CPU on
this size: the 30-hidden MLP is too small to amortize GPU launch
latency. See [`samples/mnist/README.md`](samples/mnist/README.md)
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
* Embedding from C++: [`docs/embedding.md`](docs/embedding.md)
  / [日本語](docs/embedding.ja.md)
* Standalone binary build: [`docs/binary_build.md`](docs/binary_build.md)
  / [日本語](docs/binary_build.ja.md)

Build
-----

Requires a C++23 compiler and `just`. JIT also needs LLVM 17+
(`brew install llvm` / `apt install llvm-17-dev` / `dnf install
llvm-devel` / `winget install LLVM.LLVM`).

```bash
just build              # with JIT
just build-no-jit       # interpreter only, ~1 MB binary
just test               # run .cul tests on interpreter
just test-jit           # run .cul tests on JIT
just verify             # diff interp vs JIT + embedding smoke + AOT smoke
./build/culebra --shell                # REPL  (--jit for JIT REPL)
./build/culebra        samples/fib.cul # interpreter
./build/culebra --jit  samples/fib.cul # JIT
```

### Standalone binaries

`culebra build` compiles a `.cul` source ahead-of-time into a
self-contained executable. No LLVM at runtime; tree-shaking drops
the ~200 runtime helpers a program doesn't reference. Tensor-free
programs also drop the Accelerate / BLAS framework dependency.

```bash
./build/culebra build samples/fizzbuzz.cul -o ./fizzbuzz
./fizzbuzz                                              # standalone, ~350 KB on macOS
otool -L ./fizzbuzz                                     # no Accelerate, no LLVM
```

Cross-compile is supported via `--target=<triple>` (LLVM triple)
with user-provided `--sysroot=` and `--rt-lib=`. See
[`docs/binary_build.md`](docs/binary_build.md) for the full
workflow.

License
-------

[MIT](LICENSE)
