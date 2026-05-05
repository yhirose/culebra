Culebra Programming Language
============================

A small, dynamically-typed scripting language written in C++23.
Culebra ships with both a tree-walking interpreter and an LLVM ORC JIT
compiler for the same AST, so the same program can be run either
directly or JIT-compiled to native code.

Features
--------

* Dynamic typing: `Nil`, `Bool`, `Long`, `String`, `Array`, `Object`,
  `Function`
* First-class functions with lexical closures (`self` for recursion,
  `this` for method calls)
* Rust-flavored syntax: `let`, `mut`, `fn`, expressions instead of
  statements where possible
* String interpolation: `"hello {name}"`
* Arrays and objects with rich built-in methods — `.map()`, `.filter()`,
  `.reduce()`, `.slice()`, `.join()`, `.keys()`, etc.
* **Optional type annotations** with runtime boundary checks
  (`let x: Long = 10`, `fn add(a: Long, b: Long) -> Long { ... }`)
* **Pattern matching** with literal, variable-binding, wildcard, typed,
  or-combined, and array/object destructuring patterns, plus guards
* **LLVM ORC JIT compiler** with `-O2` optimization by default
* **Reference counting** plus a Python-style **cycle collector** in
  both the interpreter and the JIT runtime
* REPL (interpreter or JIT), `--debug` breakpoint on `debugger`
  statements
* Line/column-aware runtime error messages

Quick look
----------

```culebra
# Optional type annotations
add = fn (a: Long, b: Long) -> Long { a + b }
puts(add(3, 4))                        # 7

# Closures capture and mutate outer scope
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }
}
c = make_counter()
puts(c())                              # 1
puts(c())                              # 2

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
while i < 10 {
  puts("{i}: {fib(i)}")
  i = i + 1
}
```

See [`samples/`](samples/) for more examples (`fib.cul`, `closure.cul`,
`class.cul`, `match.cul`, `types.cul`, ...).

Documentation
-------------

* Language specification: [`docs/language.md`](docs/language.md)
  / [日本語](docs/language.ja.md)
* Standard library reference: [`docs/stdlib.md`](docs/stdlib.md)
  / [日本語](docs/stdlib.ja.md)

Build
-----

Requires a C++23 compiler and `just`. The JIT build also needs LLVM
(tested with 22.x, installed via `brew install llvm`).

```bash
# Build with JIT (release, recommended)
just build

# Or without JIT (interpreter only)
just build-no-jit

# Run tests
just test

# Run the full benchmark suite
just bench-all
```

Running scripts
---------------

```bash
# Interpreter (default)
./build/culebra samples/fib.cul

# JIT compiled
./build/culebra --jit samples/fib.cul

# Interactive REPL (interpreter)
./build/culebra --shell

# Interactive REPL (JIT; state is not preserved between inputs)
./build/culebra --jit --shell

# Print AST
./build/culebra --ast samples/fib.cul

# Emit LLVM IR (JIT mode)
./build/culebra --jit --emit-llvm samples/fib.cul

# Choose LLVM optimization level (0..3, default 2)
./build/culebra --jit -O0 samples/fib.cul

# Drop into the CLI debugger on `debugger` statements
./build/culebra --debug samples/debug.cul
```

Architecture
------------

* [`include/parser.h`](include/parser.h) — PEG grammar (via
  [cpp-peglib](vendor/cpp-peglib)) that builds the AST.
* [`include/interpreter.h`](include/interpreter.h) — tree-walking
  interpreter. Values are `std::any`-backed, containers use
  `std::shared_ptr`, cycles are reclaimed by an auxiliary
  mark-and-sweep GC registered on creation.
* [`include/jit.h`](include/jit.h) — LLVM ORC JIT. Compiles the same
  AST to LLVM IR using a tagged `%Value = { i8, i64 }` representation.
  Heap types (`JitCell`, `JitClosure`, `JitArray`, `JitObject`) share a
  common `i64` refcount header and participate in the cycle collector.
* [`include/repl.h`](include/repl.h), [`include/debugger.h`](include/debugger.h)
  — REPL and interactive CLI debugger.

Both backends share the same parser and AST. Adding a new language
feature usually means: grammar tweak in `parser.h`, an `eval_*` method
in the interpreter, and a `compile_*` method in the JIT.

Performance
-----------

Times from `just bench-all` on an Apple Silicon laptop (`-O2`,
release build, LLVM 22):

| benchmark          | interp | jit   | speedup |
|--------------------|-------:|------:|--------:|
| fib(0..33)         | ~9.3s  | ~1.6s |  ~5.9×  |
| sum(0..10,000,000) | ~7.0s  | ~1.6s |  ~4.5×  |
| closure_counter    | ~2.6s  | ~1.5s |  ~1.7×  |
| array_push (100k)  | ~1.8s  | ~1.5s |  ~1.2×  |
| object_churn (100k)| ~1.6s  | ~1.6s |  ~1.0×  |
| string_build       | ~1.4s  | ~1.5s |  ~1.0×  |

Both backends share a ~1.4s fixed startup: cpp-peglib compiles the PEG
grammar at process init. That sets the floor of the JIT column and
dominates small workloads. The JIT's real speedup is in *execution*
time, most visible on compute-heavy workloads (fib, sum); for
allocation-heavy ones, RC overhead dominates and the two backends are
roughly on par.

The interpreter is pure C++ and needs no LLVM at build time; use
`just build-no-jit` for a ~1MB binary (still with the same parser
startup cost).

License
-------

[MIT](LICENSE)
