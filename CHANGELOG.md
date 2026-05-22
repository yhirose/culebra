# Changelog

All notable changes to culebra are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2026-05-22

First tagged release. Culebra is a header-only C++23 scripting
language with both a tree-walking interpreter and an LLVM ORC JIT,
plus an ahead-of-time path that produces standalone executables.

### Backends

- **Tree-walking interpreter** — header-only, exception-based control
  flow, generational GC, full language coverage.
- **LLVM ORC JIT** — shape-based object layout, inline caches for
  property access, inlined `Math.range` + iterator HOFs, ~10× faster
  than the interpreter on numeric loops.
- **Ahead-of-time binary build** (`culebra build`) — emits a single
  static executable via the LLVM TargetMachine + a host `cc` link
  step. fizzbuzz produces a ~350 KB binary. Supports
  `--target=<triple>` cross-compile with user-provided runtime
  archive (`--rt-lib=<path>`).

### Language

- `let` / `let mut` bindings with optional type annotations
- `fn name(...) {...}` declarations with multi-method dispatch
  (free-function multimethods on positional argument types)
- Single-expression lambda sugar `|x| x * 2`
- `class Name { new(...) {...}, method(...) {...} }` with operator
  overloading via `__add__` / `__sub__` / `__mul__` / `__div__` /
  `__neg__` / `__pow__` / `__matmul__` / `__eq__` / `__lt__` /
  `__le__` / `__str__`
- `match v { x: Type => ..., pat | pat => ... }` pattern matching
  (interpreter; the JIT supports literal/identifier patterns —
  type patterns return `nil`)
- `try` / `catch` / `defer` with structured Error objects
  (`{kind, message, line, col}`)
- Keyword arguments, `*` kw-only marker, `**rest` collector,
  `**splat` at call sites
- `for ... in iter` with iterator protocol and Unicode-aware string
  iteration (UAX #29 graphemes, code points)
- UFCS — method-call syntax on any value
- Mixed-key Objects (`{1: 'a', 'k': 'b', (1, 2): 'c'}`), insertion
  order preserved
- Tuple and Set primitives with method-only operations
- String interpolation `"value = {x}"`

### Standard library

- `Math` — `abs`, `min`, `max`, `pow`, `log`, `exp`, `sqrt`, `floor`,
  `ceil`, `round`, `range`, `iota`, `sign`, `clamp`; constants
  `pi`, `e`, `inf`, `nan`
- `IO` — `puts`, `print`, `input`, `read`, `write`, `exists`
- `FS` — `join`, `basename`, `dirname`, `extension`, `stem`, `exists`,
  `is_file`, `is_dir`, `size`, `list_dir`, `mkdir`, `remove`
- `Time` — `Instant` / `Duration` classes, nanosecond precision,
  ISO 8601 round-trip with sub-second support, calendar arithmetic
  with month-end clamp, operator overloads (`t1 - t2 → Duration`,
  `t + d → Instant`, `d * n`, etc.)
- `Random` — seedable PRNG, `int`, `uniform`, `gauss`, `shuffle`,
  `weighted_choice`
- `Sys` — `argv`, `exit`, `env`
- `Tensor` — N-dimensional numeric tensor with BLAS-backed lazy graph,
  autograd, matmul (`@`), broadcast, MNIST inference + training
  surpasses NumPy
- `JSON` — `stringify` (with `indent` / `lines` / `sort_keys` /
  `number_mode`) and `parse`
- `Args` — declarative CLI argument parser: positional, options,
  `Bool` flags, `default`, `repeated`, subcommands, auto `--help`

### Embedding

- `culebra::environment(argv)` — fresh env with core globals + stdlib
- `culebra::parse` / `culebra::interpret` / `culebra::JIT::run`
- `culebra::call(env, name, args)` — host invocation of script
  functions
- `culebra::define(env, name, fn)` — type-deduced registration of
  C++ host functions
- Per-`Runtime` isolation via `RuntimeScope` (PRNG, exception
  carriers, defer stack, JIT hooks)
- `culebra_rt` / `culebra_rt_no_tensor` static archives for AOT
  binaries; embedders pass the path via `CULEBRA_RT_LIBPATH`

### Tooling

- Multi-line REPL with history, paren/brace tracking, interpreter +
  JIT backends share session state
- `just verify` runs the differential test suite (interp ↔ JIT
  stdout diff + AOT binaries match `--jit` + embedding smoke
  tests) — wired into CI on Ubuntu + macOS
- `culebra build --emit-llvm` dumps the AOT IR for inspection

### Documentation

- `docs/language.md` — language reference
- `docs/stdlib.md` — stdlib reference (Math, IO, FS, Time, Random,
  Sys, Tensor, JSON, Args)
- `docs/embedding.md` — embedding contract, threading, multi-Runtime,
  AOT runtime archive
- `docs/binary_build.md` — `culebra build` workflow
- `docs/tutorial.md` — incremental walk-through
- All four guides published in English and Japanese (`.ja.md`)

[0.1.0]: https://github.com/yhirose/culebra/releases/tag/v0.1.0
