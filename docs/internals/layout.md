Source Layout: What a Name Has to Mean
======================================

Where a file goes and what it is called. Every rule below keeps a name
meaning what it says.
`tools/checks/check_header_naming.sh` and
`tools/checks/check_layering.sh` now hold what can be held mechanically;
this file says what the rules are for.

The architecture these files implement is [`vm.md`](vm.md); the memory
half is [`memory.md`](memory.md). The Japanese mirror of this file is
[`layout.ja.md`](layout.ja.md).

Contents
--------

1. [The directory is the layer](#1-the-directory-is-the-layer)
2. [`jit` means LLVM](#2-jit-means-llvm)
3. [`.gen.h` means generated](#3-genh-means-generated)
4. [`.inc.h` is not a header](#4-inch-is-not-a-header)
5. [One file per standard-library namespace](#5-one-file-per-standard-library-namespace)
6. [Adding a file](#6-adding-a-file)

---

## 1. The directory is the layer

`include/` holds one public header and ten directories, and which
directory a file is in *is* which layer it belongs to. An include is
written the way it reads — `<rt/value.inc.h>`, `<stdlib/http.h>` — and
the one `include_directories(./include)` in `CMakeLists.txt` resolves all of
them (the vendored libraries add their own directories beside it).

| Directory | What is in it | May include |
|---|---|---|
| `base/` | engine-neutral foundation: errors, `Runtime` slots, the interrupt, formatting, handle tables | nothing above it |
| `frontend/` | grammar, parser, lint, function analysis, the AST→AST transforms, module loading | `base` |
| `rt/` | the runtime both engines stand on: value model, collector, slab, the `extern "C"` helpers | `base` |
| `conc/` | isolates, channels, the neutral `SendNode`, frozen shared values | `base`, `rt` |
| `interop/` | binding C++ classes and instances into the language | `base`, `rt` |
| `stdlib/` | one header per namespace, the kernels under them, the binding layer joining them to the value model | everything below |
| `vm/` | bytecode, the compiler, the executor, sessions, the embedding API | `base`, `frontend`, `rt`, `conc`, `stdlib` |
| `jit/` | the LLVM lowering and the codegen context | everything but `cli` |
| `aot/` | what `culebra build` needs past the lowering | `base`, `rt`, `frontend` |
| `cli/` | the subcommands: test, fmt, lint, docs, init, repl, dap, lsp, toolchain, embed-flags | everything |

The stack is the point, and the gate holds it. Three inversions exist and
are recorded with their counts, so each can only shrink:

- **`rt` → `frontend` (2)**: `rt.h` includes the parser for one call to
  `peg::codepoint_length` — it needs peglib, not the parser — and
  `fn_analysis.h` for the `FuncInfo` / `FnAnalysis` that `jit.h` reads
  back through it rather than including itself.
- **`rt` → `stdlib` (1)**: `Tensor` is a value the runtime knows about,
  so `rt.h` includes its header even though `Tensor` is also a namespace
  with its own AOT axis.
- **`stdlib` → `jit` (1)**: one member of `stdlib/bindings.h`, under
  `CULEBRA_JIT_ENABLED`, declares the runtime's helpers on an LLVM
  module.

This is what the directories buy: "which layer is this file in" is
answered by the path, so the gate can hold it, and a new inversion is an
`#include` the gate rejects rather than one that compiles and passes every
test.

## 2. `jit` means LLVM

`jit/` is the only directory whose files include an `<llvm/...>` header.
Everything the two engines share — the value model, the collector,
strings, dispatch, the iterator protocol, the whole standard library — is
LLVM-free by construction. That is not a tidiness point: it is what lets
a build without the JIT still run programs ([`vm.md`](vm.md) §9), and
what lets the WASM playground exist at all.

One header outside `jit/` names `llvm::`: `stdlib/bindings.h`, in the one
member described above. It is a wart rather than a design — the rest of
that header is engine-neutral — and the gate holds an explicit list of
the files allowed to name LLVM, which may shrink and never grow. Growing
it means some new part of the tree stopped building without LLVM, which
is worth deciding on purpose.

## 3. `.gen.h` means generated

A generated file is one a recipe rewrites, so editing it by hand loses
the edit at the next `just gen-*`. The suffix marks which files those
are, and it has to be true in both directions: a generated file without it
hides that edits are lost, and a hand-maintained file with it claims a
generator that does not exist. `frontend/grammar_blob.gen.h` is
generated; `stdlib/canon_sigs_table.h` is maintained by hand. The gate
checks that every committed
`.gen.h` is named somewhere under `justfile`, `misc` or `tools`, and that every path a
generator writes under `include/` ends in `.gen.h`.

## 4. `.inc.h` is not a header

`rt/rt.h` is one translation unit's worth of runtime split across ten
files for size. The nine `rt/*.inc.h` fragments are its body, not
headers in any useful sense: they rely on rt.h's include block rather
than their own, one `extern "C"` block opens in `rt/runtime.inc.h` and
closes three files later in `rt/iter.inc.h`, and rt.h includes them in a
fixed order that does not survive reordering.

`.inc.h` puts that constraint in the name, where a comment at the top of
rt.h would be met only by a reader already there — the job Ruby's `.inc` does for the files its interpreter textually
includes — and the gate enforces the half that matters: only `rt/rt.h`
may include one.

## 5. One file per standard-library namespace

A user-visible namespace implemented in C++ gets one header in `stdlib/`
named after it in lowercase: `Http` is `stdlib/http.h`, `SQLite` is
`stdlib/sqlite.h`, `PEG` is `stdlib/peg.h`. That is the pattern a new
namespace follows. The small ones (`Random`, `FS`, `IO`, `Sys`) have no
header and are implemented inside the binding layer, and the namespaces
behind a CMake option (`Scene`, `Webview`) live in
`src/runtime/culebra_rt_*.cc`.

Several kinds of file share the directory without being namespaces
themselves: support headers a namespace needs (`image.h` and `font_ttf.h`
behind `Canvas`, `pcm_block.h`, `pixel_size.h`, `http_method.h`,
`keynames.h`, `search_model.h`), the engine-neutral kernels beneath them
(`kernels.h`, `math.h`), the compiler's signature tables (`canon_sigs.h`,
`canon_sigs_table.h`), the preamble loader and its generated blob
(`preamble.h`, `preambles.gen.h`), and the binding layer that joins all of
them to the value model (`bindings.h`). Namespaces written in Culebra
rather than C++ live in `src/preambles/<name>.cul`. Some have a header
beside them (`audio`, `canvas`, `fst`, `peg`, `regex`, `term`) for their
native part, and the rest (`dir`, `llm`, `log`, `time`, `kauai` and others)
have none.

## 6. Adding a file

- **A stdlib namespace**: `include/stdlib/<name>.h`, plus its binder in
  `stdlib/bindings.h` and its canonical signatures in
  `stdlib/canon_sigs_table.h`.
- **A runtime fragment**: it belongs to `rt/rt.h`, so
  `rt/<what>.inc.h`, added to rt.h's ordered block. If it needs includes
  of its own it is not a fragment; it is a header, and it gets a plain
  `.h`.
- **Something only the LLVM lowering uses**: `jit/`. Putting it anywhere
  else and reaching for LLVM there is a decision, not a detail.
- **A generated file**: `<name>.gen.h` in the layer that owns it, with
  the recipe that writes it in the justfile and a `check-` recipe that
  fails when it is stale.
- **Anything at all**: it goes in the lowest layer that can hold it. An
  include that points upward fails `just check-layering`, which runs
  inside `just test-dev`.
