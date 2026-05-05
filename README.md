Culebra Programming Language
============================

A small, dynamically-typed scripting language with Rust-flavored
syntax, written in C++23. Two backends share one AST: a tree-walking
interpreter and an LLVM ORC JIT.

**Why Culebra**

* **Familiar feel.** `let` / `mut` / `fn` / `|x| expr` / `match`,
  string interpolation, first-class closures.
* **Small surface, real engine.** Eight types, no hidden globals;
  refcount + cycle GC; per-callsite inline caches and HOF fusion in
  the JIT. Same AST runs on the interpreter (no LLVM needed) or on
  LLVM ORC at `-O2`.
* **Embed or script.** `~1 MB` interpreter binary, or full JIT for
  hot loops. CLI exposes `puts` / `Sys.argv`; embedders get a clean
  environment.

For who: people who want something Ruby/Python-shaped but with
typed dunders and an honest JIT to study or extend.

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

JIT wins on compute; allocator-heavy and trivial code can favor the
interpreter. End-to-end stress test:
[`samples/microgpt`](samples/microgpt/) ports Karpathy's scalar
autograd microgpt — Culebra `--jit` runs ~17% faster than CPython
at 200+ steps.

Quick look
----------

```culebra
# Closures capture and mutate outer scope
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }
}
c = make_counter(); puts(c()); puts(c())   # 1, 2

# `class` sugar — same encapsulation, terser; carries a `class:` tag
class Counter {
  new()  { this.n = 0 }
  tick() { this.n = this.n + 1; this.n }
}

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

# Lambda sugar + iterator chain
[1, 2, 3, 4].map(|x| x * x).filter(|x| x > 4)   # [9, 16]
```

More in [`samples/`](samples/).

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
