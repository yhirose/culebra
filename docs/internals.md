Culebra Internals
=================

This document is the developer-facing companion to the user-facing
guide. It records the *how* (implementation strategy, library choices,
internal data structures) and the *why-not* (designs that were
considered and rejected). It is English-only by policy — contributors
are expected to read English; user-facing docs (`guide.md`,
`language.md`, `stdlib.md`) are bilingual.

Contents
--------

1. [Architecture overview](#1-architecture-overview)
2. [Parser (cpp-peglib)](#2-parser-cpp-peglib)
3. [Interpreter](#3-interpreter)
4. [JIT (LLVM ORC)](#4-jit-llvm-orc)
5. [AOT codegen](#5-aot-codegen)
6. [String / Unicode](#6-string--unicode)
7. [Regex](#7-regex)
8. [Tensor](#8-tensor)
9. [HTTP](#9-http)
10. [Module system](#10-module-system)
11. [Build & vendor](#11-build--vendor)
12. [Test runner](#12-test-runner)
13. [Design rejected](#13-design-rejected)

---

1. Architecture overview
------------------------

One AST, three execution paths. The same parser feeds all of them.

```
       .cul source
            │
            ▼
   cpp-peglib parser   ─►   AST (shared_ptr nodes)
            │
   ┌────────┼────────────────────┐
   ▼        ▼                    ▼
Interpreter  JIT (LLVM ORC)      AOT codegen
(tree walk)  (in-process)        (LLVM .o → linker → exe)
```

Why this layout:

- **Sharing the AST** keeps semantics identical across backends. New
  language features land in the AST + interpreter first, then are
  matched by the JIT and AOT paths ([[project_dual_backend_policy]]).
- **No bytecode tier.** The interpreter walks AST nodes directly; the
  JIT lowers AST to LLVM IR. A bytecode middle layer is rejected as
  added complexity without measured benefit at this scale.
- **The driver binary `culebra`** is the CLI; it can run any of the
  three paths depending on flags (`--jit`, `build`, default interp).

Header roots:

- `include/ast.h` — AST node hierarchy.
- `include/interp.h` — tree-walking interpreter.
- `include/jit.h` — ORC JIT (compiled only with `CULEBRA_ENABLE_JIT`).
- `include/aot.h` — AOT compile to LLVM `.o`, link via system cc.
- `include/runtime/` — runtime helpers visible from JIT and AOT.

2. Parser (cpp-peglib)
----------------------

The grammar lives in `include/grammar.h` as a single PEG specification
fed to `peg::parser`. cpp-peglib gives us:

- PEG semantics (greedy, no left recursion as a hard rule).
- Each grammar production maps to an AST builder via a semantic
  action.
- Source position is propagated to every AST node automatically.

Why cpp-peglib (vs hand-written recursive descent):

- The grammar is in one file, readable as a spec next to
  `language.md`.
- Semantic actions stay close to syntax; nothing leaks into the
  lexer/parser dichotomy.
- Performance has not been a bottleneck — parsing time is dominated
  by interpreter/JIT compile, not by the parser.

AST nodes are constructed as `shared_ptr` from the parse callbacks.
Identifier resolution is deferred to a post-parse pass so the parser
can be context-free.

`ParseError` carries `file`, `line`, `col`, and a short message; it is
formatted by the CLI driver in a `clang`-like fixed-width style.

3. Interpreter
--------------

### Value layout

`Value` is a tagged union (`std::variant` in practice) over the eight
built-in types. Boxing happens via `shared_ptr` for the four reference
types (`String`, `Array`, `Object`, `Function`); the four scalars are
in-line.

Refcount is `shared_ptr`'s default. A small cycle GC sweeps Objects
that hold mutual references; it is invoked opportunistically rather
than scheduled, since most programs do not generate cycles.

### Scoping

`Environment` is a linked frame: each frame holds a string → Value
map, and a parent pointer to the enclosing frame. Closure capture is
"closes over the frame" — the closure holds a `shared_ptr<Environment>`,
which keeps the frame alive after the enclosing function returns.

### shared_from_this lifetime

`Interpreter` owns four long-lived lambdas that need to survive their
caller (callbacks installed into runtime values). These lambdas
capture `[self = shared_from_this(), ...]` so the interpreter object
itself is kept alive as long as any escaped callback survives. New
runtime-callback lambdas in this codebase MUST follow the same
pattern; see [[project_interpreter_lifetime]] for the original
incident.

### Error propagation

`throw` raises a `culebra::ThrowSignal` (C++ exception) carrying the
thrown Value and source location. `try`/`catch`/`defer` are
implemented by the visitor walking AST nodes for `BlockStmt`,
`TryStmt`, and `DeferStmt`, maintaining a per-frame defer stack and
LIFO execution on every exit path.

4. JIT (LLVM ORC)
-----------------

The JIT lowers AST to LLVM IR at function granularity (whole-module
lowering for the script's top-level) and hands the module to ORC v2
for `-O2` compilation. Symbols in the host process (runtime helpers,
allocator, BLAS) are exposed to JIT'd code via ORC's
`DefinitionGenerator` mechanism.

### Runtime symbol resolution

Runtime helpers (`culebra_runtime_*`) are visible at link time. On
macOS they are visible by default; on ELF/Linux they require
`-rdynamic` (CMake's `ENABLE_EXPORTS` property). See
[[project_aot_no_pie]] for the related Linux PIE/-fPIC story.

### Inline cache

Property access and method dispatch use a per-callsite IC: the first
lookup walks the Object layout and writes the resolved offset (or
Function pointer) into a `JITCallSite` slot. Subsequent calls fast-
path through the slot if the receiver's shape matches. Miss reverts
to a slow path and updates the slot.

### Namespace dispatch table

`include/stdlib_jit.h` exposes `kNsMethods`, a table from
`(namespace, method)` to a JIT-callable runtime function pointer.
Bare namespace methods (`Math.abs`, `IO.puts`) are looked up here at
codegen time, sidestepping the general lookup overhead. Adding a new
stdlib method requires one row here plus the corresponding interp
implementation; see [[project_jit_namespace_dispatch]] and the
`add-stdlib-namespace` skill.

A debug-only drift check at startup verifies that every method in
`kNsMethods` exists in the interpreter table — preventing the two
backends from silently diverging ([[feedback_check_jit_interp_symmetry]]).

### HOF fusion

`range(n).filter(p).map(f).take(k).collect()` is recognized as a
fusable chain and lowered to a single counter loop with `p` and `f`
inlined. The pattern is matched on the AST shape after the parser
runs; the JIT then emits a tight loop in IR. The interpreter does not
fuse; its iterator chain implementation is also lazy but allocates a
small wrapper per stage.

5. AOT codegen
--------------

`culebra build foo.cul -o foo` walks the module graph (Ch.10),
lowers each reachable top-level to LLVM IR, emits a non-PIC `.o`, and
hands the object plus the runtime archive to the system C++ compiler
for the final link.

### Tree-shaking

The module graph and the AST together give the set of reachable
top-level names. Runtime helpers (~200) are partitioned by feature
group; only the groups that are statically referenced from the user
program are linked. A "hello world" using `puts` pulls in IO plus the
Long printer, and nothing else.

### Two runtime archives

- `libculebra_rt.a` — full runtime, includes `Tensor` and its BLAS
  bind.
- `libculebra_rt_no_tensor.a` — `Tensor` entry points replaced with
  abort-on-call stubs. Because the static call graph from
  `culebra_runtime_num_add` to `cblas_*` is broken, the linker drops
  the Accelerate / BLAS framework from the binary.

`culebra build` scans the AST for any bare `Tensor` identifier and
picks the archive accordingly.

### Linux -no-pie

LLVM's `TargetMachine` emits non-PIC `.o`. Ubuntu's `gcc` defaults to
PIE linking, which fails with "failed to set dynamic section sizes".
The driver passes `-no-pie` to the linker on Linux to resolve this.
See [[project_aot_no_pie]] for the diagnostic chain.

### Cross-compile

`--target=<triple>` + user-provided `--sysroot=` and `--rt-lib=`.
LLVM `AllTargets*` components are linked into the host `culebra`
driver so it can emit for any LLVM-supported triple. The runtime
archive itself must be built for the target — there is no bundled
sysroot yet ([[project_binary_build_roadmap]] Phase E MVP).

6. String / Unicode
-------------------

### Today

Strings are `std::string` underneath (UTF-8). Byte indexing is
`std::string::operator[]`; scalar iteration uses cpp-unicodelib's
`utf8` namespace to walk codepoint by codepoint.

Methods like `split`, `replace`, `trim` operate at the byte level by
default. `length()` returns scalars; `size()` returns bytes; this
distinction is documented in `guide.md` §4.2.

### JIT/AOT representation: inline length header

The interpreter's `std::string` carries its length, so an embedded NUL
is an ordinary byte. The JIT/AOT backend must match that, but a
`JitValue` is one `{tag, i64}` slot — the length cannot ride in the
value, so it lives in the heap/`.rodata` object. A `TAG_STRING` payload
points at the bytes of a length-prefixed buffer
([[project_jit_string_repr]]):

```
[ uint64_t len ][ bytes... ][ '\0' ]
                 ^ TAG_STRING data points here; len at data[-8].
```

This is the BSTR / Zig sentinel-slice / CPython shape: length is
authoritative and read in O(1) (`_str_len`), so embedded NUL is a plain
byte; the trailing NUL is retained so a string without one passes to C
APIs (paths, `%s`) as-is. A `{ptr, len}` descriptor was rejected — it
adds an indirection on the hot path and a per-surface allocation for
borrowed names, and collides with `TAG_STRINGVIEW`'s layout.

Invariant: every `TAG_STRING` is header-backed. The only producers are
`_culebra_heap_str` (runtime) and `emit_str_literal` (a `.rodata`
`ConstantStruct`); literal and heap strings share one layout, so readers
never branch on origin. Borrowed shape names surfaced as String values
(object keys, `class` / variant / enum names) go through `_intern_str`
to gain a header. A debug `assert` in `_str_len` catches a header-less
pointer mis-tagged as `TAG_STRING`. Folding strings into the cycle GC
(they currently leak) is the header's natural next use.

### Planned: StringView, StringLike, lazy graphemes

[[project_string_model]] decided:

- **`StringView`** is a `std::string_view`-like type — a borrow over
  an existing `String`'s bytes, with the same byte/scalar API. It is
  parameter-only (cannot be stored in an Object), to avoid lifetime
  hazards.
- **`StringLike`** is a multimethod dispatch tag (Ch.10 of `guide.md`):
  functions can be defined for `StringLike`, picking up both `String`
  and `StringView` callers without copying.
- **`graphemes()`** returns a lazy iterator over grapheme clusters
  (UAX #29), using cpp-unicodelib's grapheme break table.

Implementation order: `StringView` (interp + JIT) → `StringLike`
multimethod hook (depends on Ch.10 dispatch IC) → `graphemes()`.

7. Regex
--------

> Status: Planned ([[project_regex_self_hosted]]).

### Library choice

A self-hosted `cpp-regexlib` is the plan: incubate the regex engine
inside `include/regex/` of this repo, then split into a standalone
`yhirose/cpp-regexlib` library once the API stabilizes. RE2 was
considered but ruled out because:

- Adding a Google project to the vendor tree is a heavy dependency
  for one stdlib namespace.
- We want grapheme-cluster matching as the default; bolting it onto
  RE2 would be more code than writing our own engine.

### Engine model

- NFA-based with linear-time guarantees (no catastrophic
  backtracking).
- Grapheme-cluster as the unit of `.` and character classes, sharing
  the grapheme break table with `String.graphemes()` (Ch.6).
- PCRE-compatible subset for the MVP. Lookaround and backreferences
  are deferred — they break linear time and most user code does not
  need them.

### Surface area

`Regex.compile(pattern)` returns a compiled regex object;
`re.match(s)` / `re.find_all(s)` / `re.replace(s, repl)`. Match
results carry byte offsets, scalar offsets, and captured groups.

8. Tensor
---------

### TNode

`Tensor` is a `shared_ptr<TNode>`. A `TNode` holds:

- `shape: vector<int64_t>`
- `strides: vector<int64_t>`
- `data: shared_ptr<Float[]>` (F64 today)
- `offset: int64_t`

Views (transpose, slice, broadcast) reuse the same `data` with
adjusted shape/stride/offset, so most operations are zero-copy.

### BLAS routing

Element-wise ops use small inline loops with autovectorization hints.
Matmul (`@`) routes through `cblas_dgemm` after laying out contiguous
buffers if needed. The BLAS provider is platform-dependent:

- macOS: Apple Accelerate (linked at runtime by default).
- Linux: OpenBLAS.

The choice is in `CMakeLists.txt`; AOT builds pick it up via the
appropriate runtime archive.

### Broadcast

Standard NumPy-style broadcasting: shapes are right-aligned, missing
dimensions are size-1, size-1 dimensions stretch. Implemented by
adjusting strides to zero on broadcast axes, then iterating with a
generic n-dim loop.

### Lazy shape (planned tuning)

Reductions (`sum`, `mean` over axes) currently materialize an
intermediate buffer when reshape-then-reduce is requested. A lazy
shape-graph pass that fuses these is on the table for Tensor steady-
state tuning ([[project_roadmap]] §② performance).

### F32/F64 trade-off

F64 today; F32 deferred. The MNIST benchmark uses F64 against numpy
F64 to keep numerical results comparable. F32 will become a `dtype`
parameter when there is concrete demand.

### GPU (planned)

[[project_matrix_gpu_roadmap]] — a separate `Matrix` (or `GTensor`)
primitive will host the CUDA / Metal Shading Language path, leaving
`Tensor` as the CPU/BLAS primitive. Two reasons for the split:

- CPU and GPU have radically different memory ownership semantics.
- Tensor's `shared_ptr<Float[]>` does not map onto GPU device memory
  without a host/device aware wrapper.

9. HTTP
-------

> Status: Planned (Tier 1, [[project_http_strategy]]).

### Library choice

cpp-httplib provides blocking HTTP/1.1, HTTP/2, SSE, and WebSocket in
one header. TLS via statically-linked BoringSSL. async/await is *not*
adopted — concurrency is via threads.

### Why blocking + threads, not async/await

- Async/await across an interpreted dynamic language is a large
  semantic addition (colored functions, executor model) for a tiny
  win at the scale we target.
- cpp-httplib covers everything we need (SSE, WS) without forcing
  async.
- Scale ceiling at a few thousand concurrent connections is enough
  for the workloads Culebra is positioned for (CLI tools, small
  servers, embed-in-host).

### Surface area

```
HTTP.get(url, **opts)
HTTP.post(url, body, **opts)
HTTP.request(method, url, **opts)
HTTP.serve(host, port, handler)
HTTP.sse(host, port, handler)
HTTP.websocket(...)
```

Server side runs on a thread pool sized by `**opts`.

10. Module system
-----------------

### Resolver

Module build starts with one entry file. The resolver iterates:

1. Parse entry; collect unresolved top-level identifiers.
2. For each unresolved identifier `x`, search sibling `.cul` files
   (same directory) for a top-level binding named `x`. If found,
   add that file to the build set.
3. Parse newly-added files; collect their unresolved identifiers.
4. Repeat until a fixed point.

The graph is directed: edges go from "file containing the reference"
to "file containing the binding".

### Cycle detection

Strongly connected components in the file graph are computed via
Tarjan's algorithm. Any non-trivial SCC is a cycle and is rejected
with a precise file/line citation pointing to one reference in the
cycle. Refactor through a shared third file.

### Entry env isolation

Bindings in the entry file are *not* visible from imported files;
bindings in imported files are visible from everywhere in the build.
This matches Go's package model (every non-main file is "the
package") with the entry treated like a top-level `main`.

The asymmetry is intentional. It means an entry script can use throw-
away names (`mut tmp = ...`) without polluting the helpers it imports.

### Why no explicit `import` statement

A 1500-line program may pull in 30 helper files. Forcing each
`import` saves nothing — the tool derives the same graph from the
unresolved identifiers. The cost (an extra resolver pass) is paid
once at build time; the gain is a faster authoring loop and free
tree-shaking ([[project_module_system]]).

11. Build & vendor
------------------

### Vendor tree (`vendor/`)

| Library | Purpose | Linkage |
|---|---|---|
| `cpp-peglib` | PEG parser | header-only |
| `cpp-linenoise` | REPL line editor | header-only |
| `cpp-unicodelib` | Unicode tables (scalar, grapheme, case) | header-only |
| `cpp-embedlib` | Bake static archives into the driver binary (`cpp_embedlib_add()`) | header-only |
| `cpp-regexlib` *(planned)* | Regex engine (Ch.7) | header-only (planned) |
| `cpp-httplib` *(planned)* | HTTP stack (Ch.9) | header-only (planned) |
| BoringSSL *(planned)* | TLS for HTTP (Ch.9) | static archive |

All non-trivial dependencies are header-only or statically linked, so
`culebra build` produces self-contained binaries.

### CMake structure

- `CMakeLists.txt` (top-level) — defines the `culebra` driver,
  optional LLVM linkage, the two runtime archives, and embed tests.
- `vendor/cpp-embedlib/cmake/cpp-embedlib.cmake` — provides
  `cpp_embedlib_add()` used to bake `libculebra_rt.a` and
  `libculebra_rt_no_tensor.a` into the driver.

`option(CULEBRA_ENABLE_JIT)` controls LLVM linkage. With JIT off, the
driver is ~1 MB and has no LLVM dependency.

### Dependency policy

- Prefer header-only vendor libraries over package-managed
  dependencies. The repo should clone-and-build with no system
  packages besides a C++23 compiler (and optionally LLVM for the
  JIT).
- No git submodules — `vendor/` is committed.
- Adding a new vendor lib requires a Why entry in this chapter.

12. Test runner
---------------

> Status: Draft. Designed in a parallel work cycle
> ([[project_culebra_test_docs_dependency]]). This section will be
> rewritten once the CLI is finalized.

Direction (subject to change):

- `culebra test [path]` discovers `*_spec.cul` and runs flat
  `test "..." { }` blocks. No `describe` nesting; hierarchy comes
  from directories.
- `culebra test --doc <markdown>` extracts ` ```culebra ` blocks per
  the doctest convention ([[project_doctest_convention]]) and runs
  each block in an isolated scope.
- Backend selection: `--interp` / `--jit` / `--aot` (default: all).
- Reporter: TAP-compatible plus a colored human format.

The doctest extractor is the most interesting internal: it parses
the markdown for fenced blocks tagged `culebra`, scans block-leading
`# doctest:` lines for directives, and assembles expected stdout
from `# =>` and `# => |` markers.

13. Design rejected
-------------------

This chapter records proposals that were considered and not adopted.
The list is append-only: entries stay even if a decision is later
revisited (with a "Reopened: <date>" note). Each entry follows a
fixed shape: a one-line status, the alternative that *was* adopted,
the reasoning, and a pointer to the relevant memory file.

### 13.1 Pipeline operator `|>`

**Status:** Rejected.
**Adopted instead:** UFCS (`guide.md` Ch.10).
**Reasoning:** `x.f(...)` reads left-to-right the same as `x |> f`,
and *also* serves as the resolution path for free functions over
user types. Adding `|>` would split the idiom space (some libraries
would use UFCS, others `|>`) without functional gain. Other languages
with pipelines (Elixir, F#) lack a UFCS, which makes `|>` the only
left-to-right form there.
**Source:** [[feedback_culebra_pipeline_ufcs]].

### 13.2 Explicit `import` statement

**Status:** Rejected.
**Adopted instead:** Implicit imports via top-level identifier
resolution (`guide.md` Ch.12).
**Reasoning:** Each `import` line is redundant with the unresolved
identifier graph that the resolver already builds. The resolver pays
the same cost either way; users save typing and the build stays
correct. Tree-shaking is unaffected (the resolver still produces a
reachable set).
**Source:** [[project_module_system]], [[project_binary_build_roadmap]].

### 13.3 Set / Tuple operators (`|`, `&`, `^`, `+`)

**Status:** Rejected (removed mid-development).
**Adopted instead:** Method calls (`.union`, `.intersect`, etc.) when
Set / Tuple land.
**Reasoning:** Lambda syntax `|x| expr` and binary `|` create a
parser ambiguity that was costly to resolve while keeping both
features usable. Method syntax has no such ambiguity. The set/tuple
operator forms were removed before they shipped.
**Update:** The binary-`|` ambiguity was later resolved for the
numeric bit-OR operator (a parallel bit-or-free expression ladder for
parameter defaults, unified back to the same AST tags via cpp-peglib's
`{ ast_name }` instruction). Set / Tuple keep the method form by
design, but the "ambiguity is unresolvable" part of the reasoning no
longer holds.
**Source:** [[project_roadmap]] §③.

### 13.4 Scalar bump allocator

**Status:** Rejected after measurement.
**Adopted instead:** Default `shared_ptr`-backed allocation.
**Reasoning:** The hypothesis was that microgpt's scalar `Value`
graph was allocator-bound. A `thread_local` pool prototype was
measured at parity (~1.6% noise) with the baseline. Realizing the
expected 14× gap to Tensor would require structural changes
(recursive release loop, etc.), which is out of scope for the
benefit. Lesson: profile before optimizing object layout
([[feedback_profile_first]]).
**Source:** [[project_microgpt_state]].

### 13.5 TypeScript-specific type features

**Status:** Rejected (scope decision).
**Adopted instead:** Stay at Rust/Swift expressiveness: Union /
Optional / Tuple / Trait/Protocol / Generic / Nominal typing
(`guide.md` Ch.13).
**Reasoning:** Conditional types, mapped types, template literal
types, and the utility-type family (`Pick`, `Omit`, `Partial`,
`Required`, ...) are designed for typing arbitrary JavaScript
libraries that pre-existed the type system. Culebra owns its own
stdlib; we do not need TS-grade type-level computation. The
implementation cost is large; the benefit is small for a fresh
language.
**Source:** [[project_type_system_vision]].

### 13.6 async/await

**Status:** Rejected.
**Adopted instead:** Blocking I/O + threads.
**Reasoning:** "Colored functions" (async/sync split) and an executor
model are heavy additions for an interpreted dynamic language. The
HTTP and similar stacks target the few-thousand-connection ceiling,
which is well within reach of blocking + threads. cpp-httplib (the
planned HTTP backend) supports SSE and WebSocket without async, so
no functional capability is lost.
**Source:** [[project_http_strategy]].

### 13.7 Release machinery (deferred, not rejected)

**Status:** Deferred until 1.0. Not a reject — a "not yet".
**Reasoning:** Pre-1.0 the API surface still moves. Version tags,
CHANGELOG, Homebrew formula, and GitHub Releases lock in expectations
that would be costly to honour and then break. These come after the
type system lands and the language goes 1.0.
**Source:** [[feedback_no_premature_release]].
