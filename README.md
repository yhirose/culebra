Culebra Programming Language
============================

A small, dynamically-typed scripting language written in C++23. Ships
with both a tree-walking interpreter and an LLVM ORC JIT compiler for
the same AST.

Features
--------

* Dynamic typing with 7 core types; first-class functions with lexical
  closures
* Rust-flavored syntax (`let`, `mut`, `fn`); optional type annotations
  with runtime checks
* Pattern matching (literals, bindings, typed, or-patterns, array/object
  destructuring, guards)
* String interpolation; rich built-in methods on arrays, objects, and
  strings
* Minimal standard library grouped under `Math`, `IO`, `Sys`
  namespaces; CLI args exposed as `Sys.argv`. The library adds no
  globals — the CLI installs `puts` / `print` as aliases for
  scripting ergonomics; embedders get a clean environment
* Reference counting plus a mark-and-sweep cycle collector
* LLVM ORC JIT with `-O2` by default, or pure-C++ interpreter (no LLVM
  required)

Quick look
----------

```culebra
# Optional type annotations
add = fn (a: Long, b: Long) -> Long { a + b }
puts(add(3, 4))                        # 7

# Closures capture and mutate outer scope
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` reassigns the captured outer
}
c = make_counter()
puts(c()); puts(c())                   # 1, 2

# Pattern matching
describe = fn (v) {
  match v {
    0                     => 'zero',
    1 | 2 | 3             => 'small',
    n: Long if n > 100    => "big ({n})",
    s: String             => "str ({s})",
    [head, ...tail]       => "head={head}, rest={tail.size()}",
    {name, age}           => "{name}, {age}",
    _                     => 'other'
  }
}

# String interpolation and recursion
fib = fn (x) {
  if x < 2 { x } else { self(x - 2) + self(x - 1) }
}
mut i = 0
while i < 10 { puts("{i}: {fib(i)}"); i = i + 1 }
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

Build and run
-------------

Requires a C++23 compiler and `just`. The JIT build also needs LLVM
(tested with 22.x; `brew install llvm`).

```bash
just build              # with JIT (release)
just build-no-jit       # interpreter only, ~1MB binary
just test
just bench-all

./build/culebra               samples/fib.cul  # interpreter
./build/culebra --jit         samples/fib.cul  # JIT
./build/culebra --shell                        # REPL (add --jit for JIT REPL)
./build/culebra --ast         samples/fib.cul  # print AST
./build/culebra --jit --emit-llvm samples/fib.cul
./build/culebra --debug       samples/debug.cul
```

Architecture
------------

* [`include/parser.h`](include/parser.h) — PEG grammar (via [cpp-peglib](vendor/cpp-peglib)) that builds the AST.
* [`include/interpreter.h`](include/interpreter.h) — tree-walking interpreter.
* [`include/jit.h`](include/jit.h) — LLVM ORC JIT. Compiles the same AST using a tagged `%Value = { i8, i64 }` representation; heap types share an `i64` refcount header and participate in the cycle collector.
* [`include/repl.h`](include/repl.h), [`include/debugger.h`](include/debugger.h) — REPL and interactive CLI debugger.

Both backends share the same parser and AST. Adding a feature usually
means: grammar tweak, `eval_*` in the interpreter, and `compile_*` in
the JIT.

Performance
-----------

Times from `just bench-all` on an Apple Silicon laptop (`-O2`, LLVM 22):

| benchmark          | interp | jit   | speedup |
|--------------------|-------:|------:|--------:|
| fib(0..33)         | ~9.3s  | ~1.6s |  ~5.9×  |
| sum(0..10,000,000) | ~7.0s  | ~1.6s |  ~4.5×  |
| closure_counter    | ~2.6s  | ~1.5s |  ~1.7×  |
| array_push (100k)  | ~1.8s  | ~1.5s |  ~1.2×  |
| object_churn (100k)| ~1.6s  | ~1.6s |  ~1.0×  |
| string_build       | ~1.4s  | ~1.5s |  ~1.0×  |

Both backends share a ~1.4s fixed startup: cpp-peglib compiles the PEG
grammar at process init. That sets the JIT floor and dominates small
workloads. The JIT wins on compute-heavy code (fib, sum); on
allocation-heavy code, RC overhead dominates and the two are on par.

License
-------

[MIT](LICENSE)
