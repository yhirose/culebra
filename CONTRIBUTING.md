Contributing to Culebra
=======================

Building from source
--------------------

Requires a C++23 compiler and [`just`](https://github.com/casey/just).
The JIT and AOT (`culebra build`) backends also need LLVM 20+; the
interpreter-only build has no LLVM dependency. The toolchain versions
CI builds against are in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

```bash
just build              # with JIT (Release + LTO)
just build-no-jit       # interpreter only, ~1 MB binary, no LLVM
just clean              # remove build/, build-dev/, build-asan/
```

`just build` produces `./build/culebra`:

```bash
./build/culebra --shell                   # REPL  (--jit for JIT REPL)
./build/culebra        path/to/script.cul # interpreter
./build/culebra --jit  path/to/script.cul # JIT
./build/culebra build  path/to/script.cul -o ./out   # AOT standalone binary
```

Inner-loop development
----------------------

`just dev` is the fast path: LTO off and a separate `build-dev/` tree so
it doesn't fight `just build`'s cache. Pair it with `ccache` (auto-detected
by CMake) for near-instant rebuilds.

```bash
just dev                # fast no-LTO build into build-dev/ (inner loop)
just test-dev           # quick interp == JIT check against build-dev/
```

Run `just test-dev` after each edit. It does not cover AOT or the C++
embedding smoke — run the full gate before opening a PR.

Testing
-------

```bash
just test               # commit gate: interp vs JIT vs AOT + embedding smoke
just doctest            # run the `# =>` doctests in docs and tests/doctest
just difftest           # differential interp-vs-JIT corpus (tools/difftest)
just perf               # microbench regression check (tests/perf/*.cul)
```

`just test` is the gate to run before every commit. `just perf` is not
part of it — microbench runtimes are noisy and machine-dependent.

Grammar
-------

`misc/culebra.peg` and the `cul.vim` keyword list are generated from
`include/parser.h`:

```bash
just sync-grammar        # regenerate after touching the grammar
just check-grammar-sync  # CI gate: verify they are in sync
```
