Culebra Internals
=================

This document is the developer-facing companion to the user-facing
guide. It records the *how* (implementation strategy, library choices,
internal data structures). Designs that were considered and not
adopted live in [`_history.md`](_history.md), not here. Every doc is
bilingual; the Japanese mirror of this file is
[`internals.ja.md`](internals.ja.md) and must be kept in sync with it.

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
13. [Memory model: RC, GC, and deterministic drop](#13-memory-model-rc-gc-and-deterministic-drop)
14. [JIT GC backstop](#14-jit-gc-backstop)
15. [JIT ownership: structural leak-freedom](#15-jit-ownership-structural-leak-freedom)
16. [Algebraic effects (source transform)](#16-algebraic-effects-source-transform)

Rejected and withdrawn designs are collected in [`_history.md`](_history.md).

---

## 1. Architecture overview

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

Memory management (reference counting, the tracing backstop, and the
JIT's structural leak-freedom discipline) is a cross-cutting concern
covered on its own in Ch.13–15.

## 2. Parser (cpp-peglib)

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

## 3. Interpreter

### Value layout

`Value` is a tagged union (`std::variant` in practice) over the eight
built-in types. Boxing happens via `shared_ptr` for the four reference
types (`String`, `Array`, `Object`, `Function`); the four scalars are
in-line.

Refcounting is `shared_ptr`'s default — automatic and exact, so leaks
and double-frees cannot occur by construction on this backend. A
precise cycle collector (`InterpGC`) reclaims reference cycles that RC
alone cannot; see Ch.13 for the full RC/GC/drop model shared with the
JIT, and Ch.13 §"Rooting" for how `InterpGC` finds its roots without
scanning the C++ stack.

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

## 4. JIT (LLVM ORC)

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
Bare namespace methods (`Math.abs`, `IO.inspect`) are looked up here at
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

### for-in cursor

`for x in xs` has to work over Array, Tuple, Set, Object keys, a Range,
any object carrying an `iter` property, and String — seven surfaces with
three ways to step: an indexed array walk, a UTF-8 scalar walk, and the
`has_next`/`next` iterator protocol. They share one cursor. The tag
dispatch at the top of the loop only *opens* the cursor (recording a kind
plus the state that kind needs), a single loop head switches on the kind
to pick the matching advance, and every advance hands its element to one
body. Exit is shared too: the iterator dispose is guarded on the cursor's
iterator slot, which the array and string cursors leave nil.

Emitting the body once is what makes nesting affordable. Inlining it under
each container arm instead multiplies it by six per level — a triple nest
emitted the innermost body 216 times, IR grew about 6.4x per level, and a
four-deep nest over a four-line program produced a million lines of IR.

`for v in a..b` over a literal range skips all of this and compiles to a
direct Long counter loop: no Range object, no heap iterator, and no
`{done,value}` object per step.

### Stdlib preamble splicing

Several stdlib modules (`Time`, `Term`, `Args`, `Regex`, `Log`, `Path`,
`Canvas`, the matcher family, `__Eff`) are written in culebra rather than
C++ so all three backends share one implementation. The interpreter binds
them lazily per environment; the JIT and AOT paths instead splice the ones
a program uses into the entry module's AST, as statements ahead of the
user's. Spliced, not concatenated onto the source text — user nodes keep
the line and column they were parsed with, so error locations match the
interpreter.

Which modules get spliced is decided from the entry module's parsed token
set, which makes the match name-exact: a module's name in a comment, or
inside a longer identifier, does not pull it in. Each module costs roughly
a second of JIT compile time, so the distinction is not academic.

Unlike the interpreter, JIT-generated code manages heap values (Object,
Array, Func, Set, Tensor, Cell, String) through hand-emitted retain/
release IR rather than `shared_ptr`. That discipline — how ownership is
tracked, how the tracing backstop reclaims what RC cannot, and how
leaks/double-frees were made structurally impossible rather than
patched site by site — is the subject of Ch.13–15.

## 5. AOT codegen

`culebra build foo.cul -o foo` walks the module graph (Ch.10),
lowers each reachable top-level to LLVM IR, emits a non-PIC `.o`, and
hands the object plus the runtime archive to the system C++ compiler
for the final link.

### Tree-shaking

The module graph and the AST together give the set of reachable
top-level names. Runtime helpers (~200) are partitioned by feature
group; only the groups that are statically referenced from the user
program are linked. A "hello world" using `inspect` pulls in IO plus the
Long printer, and nothing else.

### Runtime archives (base + per-feature)

- `libculebra_rt.a` — base runtime. The `Tensor` / `Http` / `Compress`
  chokes are **weak-symbol stubs**, so on its own it references no BLAS,
  OpenSSL, or zlib.
- `libculebra_rt_tensor.a` / `libculebra_rt_http.a` /
  `libculebra_rt_compress.a` — the strong chokes for each feature
  (pulling BLAS / OpenSSL+zlib / zlib respectively).

`culebra build` always links the base, then **force-loads** a feature
archive only when the AST references its namespace (`Tensor` / `Http` /
`Compress`) — the strong choke overrides the weak stub. So an unused
feature links neither its archive nor its external libraries. This is
N+1 archives, not a 2^N matrix; the `culebra wrap` archive
(`libculebra_rt_wrap.a`) rides the same usage gate.

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

## 6. String / Unicode

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
pointer mis-tagged as `TAG_STRING`.

`String`/`StringView` carry no refcount — they are reclaimed by the
tracing backstop, not by RC release-to-zero (Ch.13–14 cover why and
how; see Ch.13's "traced-only" note).

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

## 7. Regex

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

## 8. Tensor

### TNode

`Tensor` is a `shared_ptr<TNode>`. A `TNode` holds:

- `shape: vector<int64_t>`
- `strides: vector<int64_t>`
- `data: shared_ptr<float[]>` (F32)
- `offset: int64_t`

Views (transpose, slice, broadcast) reuse the same `data` with
adjusted shape/stride/offset, so most operations are zero-copy.

### BLAS routing

Element-wise ops use small inline loops with autovectorization hints.
Matmul (`@`) routes through `cblas_sgemm` after laying out contiguous
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

### dtype

F32 only. F64 was removed (2026-07): it does not exist on Metal and
runs at 1/32-1/64 speed on consumer NVIDIA GPUs, so it would have
been a CPU-only dtype. The `Dtype` enum remains as the seam for a
future BF16 storage type; scalar entry/exit points (`.item()`,
`.sum()`, `.to_array()`) surface `Float`.

### GPU (planned)

[[project_matrix_gpu_roadmap]] — a separate `Matrix` (or `GTensor`)
primitive will host the CUDA / Metal Shading Language path, leaving
`Tensor` as the CPU/BLAS primitive. Two reasons for the split:

- CPU and GPU have radically different memory ownership semantics.
- Tensor's `shared_ptr<Float[]>` does not map onto GPU device memory
  without a host/device aware wrapper.

## 9. HTTP

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

## 10. Module system

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

## 11. Build & vendor

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
  optional LLVM linkage, the base + per-feature runtime archives, and
  embed tests.
- `vendor/cpp-embedlib/cmake/cpp-embedlib.cmake` — provides
  `cpp_embedlib_add()` used to bake `libculebra_rt.a` and its
  per-feature archives (`libculebra_rt_tensor.a`, `_http`, `_compress`)
  into the driver.

`option(CULEBRA_ENABLE_JIT)` controls LLVM linkage. With JIT off, the
driver is ~1 MB and has no LLVM dependency.

### Dependency policy

- Prefer header-only vendor libraries over package-managed
  dependencies. The repo should clone-and-build with no system
  packages besides a C++23 compiler (and optionally LLVM for the
  JIT).
- No git submodules — `vendor/` is committed.
- Adding a new vendor lib requires a Why entry in this chapter.

## 12. Test runner

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

## 13. Memory model: RC, GC, and deterministic drop

Culebra's memory management is **RC-primary + tracing-backstop** on
both backends. This chapter states how reference counting, the
tracing collector, and deterministic `drop` behave; Ch.14 is the JIT
collector's implementation and Ch.15 is how the JIT keeps RC placement
correct in the first place. Where any of the three disagree, this
chapter wins.

### 13.1 Two layers, both permanent

- **RC owns memory and `drop` timing.** Interp: values are
  `shared_ptr` (automatic, exact). JIT: values are tagged `i64`; RC
  placement follows the ownership discipline of Ch.15 — the remaining
  bare retain/release sites are audited carve-outs counted by the
  `tools/check_rc_discipline.sh` ratchet. Release-to-zero frees
  promptly and fires `drop`, which is what makes `drop` deterministic.
- **Tracing reclaims what RC cannot**: reference cycles, and anything
  an RC placement bug leaks. JIT: a conservative mark-sweep over a
  registry heap (Ch.14). Interp: `InterpGC`, a precise CPython-style
  `gc_refs` collector.

Neither layer is retirable. RC alone is never leak-free — cycles keep
each other's counts positive forever, and hand-placement bugs leak
(even Swift's compiler-perfect ARC leaks retain cycles routinely, the
standard counterexample to "systematic RC suffices"). Tracing alone
destroys deterministic `drop` — a tracer discovers death only at
collect time, so `drop` fires late, unordered, non-deterministically
(the Go/Java finalizer model), and deterministic finalization under
shared ownership requires full reference counting. So the tracer is a
**permanent, load-bearing component**, not a temporary crutch; the open
question is only how to stop it from *silently masking* RC bugs
(§13.4–13.5, Ch.15 §15.2).

### 13.2 Deterministic `drop` — the four-path union

`drop` fires from four paths, **deduped to exactly-once by a per-
object `dropped` flag** (JIT: `JitObject::dropped`; interp:
`OrderedSymbolMap::dropped`). The flag, not the call sites, is the
invariant:

| path | trigger | mechanism |
|---|---|---|
| (a) release-to-zero | last reference released | JIT `_culebra_value_release_impl` fires drop first, then tears down proto/slots/sidecars. Interp: the prop-map `shared_ptr` deleter → `_call_drop_if_present`. |
| (b) explicit `obj.drop()` | user call | object stays alive; the flag just prevents a second fire. |
| (c) scope exit | owned-region resolution | both backends run a localized Bacon-Rajan trial deletion over the scope's drop-bearing objects: unreachable-from-outside fires now (cycle members included); externally reachable survives, compacted to the parent scope. |
| (d) GC backstop finalize | collect finds a dead set | a **PEP-442-style pre-sweep pass** fires the dead set's `drop`s while the structure is still intact; sweep then reclaims *memory only*. **Cycle members DO fire `drop`.** |

Ordering: LIFO within a scope; defers run before slot release on every
exit path; parent-before-child for the RC cascade.

A few load-bearing subtleties: firing `drop` **pins the refcount to a
sentinel during the body and restores the entry count after**, so an
unbounded re-entrant release (a `drop` body nulling its own cycle
member) is absorbed — *restoring* rather than zeroing, because paths
(b)/(c)/(d) arrive with live references still held. `throw` deliberately
does **not** retain the payload — the carrier takes over the thrower's
`+1`; retaining there would reintroduce a leak. Top-level bindings
intentionally never fire `drop` at program exit (§13.6).

### 13.3 What is refcounted, and what is traced-only

Refcounted (refcount at struct offset 0, uniformly): `Func` (closure),
`Array`, `Tuple`, `Object`, `Tensor`, `Set`, plus internal capture
`Cell`s. Nil/Bool/Long/Float are immediate values, never refcounted.

**`String`/`StringView` carry no refcount** — retain/release are
no-ops on these tags; they are reclaimed **only** by the tracing
backstop. Until 2026-07 this was a genuine permanent leak (JIT strings
were `malloc`ed with no reclaimer at all). Two barriers made strings
visible to the collector: owner-edge tracking (a `StringView` roots the
`String` it borrows from) and routing transient JIT string bytes
through a per-Runtime byte slab the collector can enumerate. A dead
string is now reclaimed at the next collect like any other traced-only
object — at the cost of being backstop-only rather than RC-reclaimed
(acceptable: strings are typically short-lived or immortal-interned,
not cycle-prone). The interp has no traced-only concept — every interp
heap value, including `std::string`, is an ordinary `shared_ptr`
object.

### 13.4 Rooting — the two backends do it differently

- **JIT (shipping default):** a conservative scan of the entire
  machine stack roots every in-flight `JitValue` regardless of
  refcount — see Ch.14 §14.3 for the scan mechanism and its
  correctness argument. An alternative `gc_refs` mode
  (`CULEBRA_GC_REFS=1`, off by default) is the CPython algorithm
  instead (refcount minus in-heap edges, no stack scan), whose
  soundness is exactly as good as the RC accounting. A diagnostic
  classifier built on `gc_refs` (§13.5) sorts conservative-dead-but-
  gc_refs-retained objects into *inflated-RC* (a definite RC placement
  leak) vs *transitively-held* (a cycle) — a zero-false-positive RC-leak
  detector.
- **Interp:** no C++ stack scan at all. Safety is *scheduling*:
  collection runs only at statement boundaries, with roots hand-walked
  from the current env chain plus `FrameRootGuard` entries.

Consequence: "rooting is independent of any single retain" is
**JIT-only**, and under `gc_refs` rooting is only as sound as the RC
accounting — an early-but-balanced release is a use-after-free window
that the conservative scan silently absorbs (by rooting strictly more)
but `gc_refs` does not. This is why the conservative scan is not simply
replaced by `gc_refs`; see Ch.15 §15.2 for the consequence this has for
the ownership design.

### 13.5 Detecting RC bugs without relying on coverage

A placement bug historically just meant "the backstop quietly reclaims
it, nobody ever knows." A debug/CI-only detector closes that blind
spot: `CULEBRA_GC_LEAK_ABORT=1` captures each object's allocation
backtrace and, at one quiescent safepoint per program (JIT teardown,
after the top level returns but while module/namespace roots are still
wired — the only point where "inflated RC and dead" unambiguously means
"leaked"), the inflated-RC classifier aborts with the object's birth
site. This runs as a standing CI phase over the whole difftest corpus
(`tools/difftest/leak_abort_suite.sh`), throw paths included, with a
cycle-only allowlist. It is a no-LTO/debug tool (LTO's altered stack
layout under-reports); production keeps quietly reclaiming, an accepted
trade (a crash would itself be a DoS vector).

### 13.6 Accepted `drop`-timing carve-outs

Three cases where `drop` is not *exactly* deterministic are accepted as
documented language semantics — matching the Python/Swift norm, and
each degrades only *timing*, never actual reclamation: (1) a scope
resolving an extreme number of drop-bearing objects at once
(`kNodeBudget` overflow) defers the excess to the backstop; (2)
top-level bindings never fire `drop` at program exit, by design, on
both backends — stdlib resources (`File`, sockets) still flush/close on
exit via C++ RAII regardless, so only a user-defined `drop`'s *extra*
side effect at top-level scope is affected, and `defer` / explicit
`.drop()` / `with` remain available there; (3) a self-capturing
closure's cycle fires one collect later on the interpreter than on the
JIT (drop *count* is symmetric, *timing* differs by one collect).

Every mainstream RC/GC language ships equivalent carve-outs (Python's
module-level `__del__` is not guaranteed at interpreter exit; Go's
`defer` does not run on `os.Exit`; C++ static-dtor order is not
guaranteed under `_exit`/signal/`abort`) — this is "deterministic drop
as strong as Python/Swift," not "zero carve-out," and closing them
further isn't pursued because the standing `defer`/`with`/`.drop()`
escape hatches already cover what it would buy.

## 14. JIT GC backstop

The JIT's tracing collector is a **conservative, non-moving, mark-
sweep backstop** running alongside the kept manual RC (Ch.13) — not a
replacement for it. This chapter is the collector's implementation;
Ch.13 covers the RC/drop model it backstops, Ch.15 covers how RC
placement is kept correct in the first place.

### 14.1 Why a backstop

Manual RC in generated code leaks (a missed release) and double-frees
(a release of an already-consumed value) — getting placement right on
every control-flow path (fall-through, `break`, `continue`, early
`return`, exception unwind) by hand is hard, and without a backstop a
missed release or a cycle is *permanent* (a JIT microgpt run once grew
to ~5 GB RSS this way). Pure tracing (no RC at all) was considered and
rejected for the reason given in §13.1: deterministic `drop` requires
full RC. So RC stays primary, and the collector's only job is
reclaiming the residue RC cannot (cycles, missed-release leaks).

### 14.2 Object model and heap

GC-tracked structs (`JitObject`, `JitArray`, `JitCell`, `JitClosure`,
`JitSet`, `JitTensor`) keep `int64_t refcount` at struct offset 0 (the
existing retain/release IR is undisturbed). The collector's own
per-object metadata (mark bit, type tag, generation) lives in a
**registry** (an address → metadata map) rather than an in-object
header — the smaller change, since it revives the retain/release IR
verbatim instead of rewriting every codegen emit site.

Variable-length buffers (`JitArray::items`, `JitObject::slots`, closure
captures) stay C++-owned, not GC-allocated: they are released by the
struct's ordinary C++ destructor at sweep, reached during marking via
`enumerate_children`. This keeps the collector simple and sweep
cleanup automatic.

The registry started as a `std::unordered_map`, whose per-object node
allocation was measured as the entire alloc-churn overhead on
object/array-heavy workloads (+12–21%). It was replaced with an
open-addressing flat hash map (no per-entry allocation, tombstone
reuse), recovering the overhead. A size-class/region allocator (the
Go/JSC model) is the next lever *if* allocation throughput is later
measured to need it — not pursued without measurement.

### 14.3 Root finding (conservative)

At collection time, any machine word that, read as a pointer, lands on
a valid object start is a candidate root. Sources: (1) the machine
stack of every mutator thread, from the collection point's SP up to
the thread's stack base — covering both JIT-generated and C++
runtime-helper frames, so a `JitValue` held in any C++ builtin local is
rooted automatically; (2) callee-saved registers, flushed to the stack
at collection entry; (3) explicit global roots registered with the
collector (namespace table, module caches, REPL globals, exception
carriers, the defer stack).

**Correctness argument:** any GC pointer live across a call that can
trigger collection must, by the platform calling convention, be either
spilled to the stack or held in a callee-saved register before the
call. At the moment collection runs — deep inside the allocator — every
such value is therefore found by (1)+(2). This is the same argument
that makes Boehm/Ruby conservative GC sound under an optimizing
compiler.

A `JitValue` is `{tag, data}`; the scanner does not consult the tag —
it tests every 8-byte word, so a heap pointer is found regardless of
tag. A non-pointer scalar that happens to alias a heap address is a
*false* root (bounded over-retention, not a correctness problem).
`data` always points to the object base (no interior pointers), which
keeps validation simple. One detail that cost real debug time: root
scanning must start at the **stack pointer**, not the frame pointer —
locals/spills below the frame pointer would otherwise be missed under
`-O2` and higher.

### 14.4 Collect: mark and sweep

**Mark:** from the root set, trace transitively; for each live object
set its mark bit and push its children (via `enumerate_children`) onto
the mark stack, to a fixpoint. Conservative roots are pinned — nothing
moves, so there's nothing to update.

**Sweep:** walk the registry; for each unmarked object, run its C++
destructor exactly once and reclaim the slot. A full mark-sweep from
real roots reclaims any unreachable object regardless of stale
bookkeeping — the property the old, non-tracing collector lacked, and
the reason leaks used to be permanent.

Finalization (the pre-sweep `drop` pass) is covered in Ch.13 §13.2
path (d); it belongs to the drop model, not the sweep mechanism, and
is not duplicated here.

### 14.5 Generational layer (future, not correctness-relevant)

The current mark-sweep is non-generational and correct on its own. A
generational layer (young/old, minor collects tracing young + a
remembered set) is a throughput optimization to add only once the base
is measured to need it. Its one hard requirement, noted here because
it constrains any future implementation, is a **write barrier**: every
store of a heap value into an old object's field must record the old
object in a remembered set, or a minor collect can free a live young
object reachable only via that old→young edge.

### 14.6 Safety devices

- **`CULEBRA_GC_STRESS=1`** collects on every allocation, surfacing
  rooting/marking bugs deterministically instead of flakily (the
  SpiderMonkey `gcZeal` model); the suite runs green under it.
- **`GC.stat()`** exposes live-object/heap-byte counts.
- **Leak regression tests** (`tests/test_gc_no_leak.cul`) are the JIT
  acceptance gate, mirrored from the interpreter's green baseline.
- **Debug fill** poisons freed slots; use-after-free asserts instead of
  silently reading garbage.
- **Heap verify** (debug builds) walks all live objects and checks
  every child pointer resolves to a valid heap object.

The GC's own C++ implementation follows the same RAII discipline it
enforces on the runtime: teardown is a struct destructor, stop-the-
world coordination is a scope guard, global-root registration is
scope-lifetime.

### 14.7 Cross-backend: the interpreter's precise collector

The interpreter's `InterpGC` is precise (CPython-style `gc_refs`
subtraction + BFS + clear), not conservative — refcounts are exact
`shared_ptr` counts, so it needs no fallback stack scan. It tracks
Array backing vectors, captured Environments, Object property maps,
and Tuple/Set members as cycle nodes, so the same cycle shapes the JIT
backstop reclaims are reclaimed on the interpreter, keeping the two
backends behaviorally symmetric.

Being precise means `InterpGC` is correct **only if every live value is
reachable from a registered root** — two classes of stack-only
liveness are rooted explicitly, or a value is swept mid-program
(surfacing as a spurious `NameError`): (1) active env chains — collect
is deferred to the next statement boundary, and at that safe point live
envs are the current statement's `env` plus every caller's env still on
the C++ call stack, each pushed via an RAII `FrameRootGuard`; (2)
in-flight C++-stack temporaries — deferring collect to the statement
boundary (rather than the allocation point) also covers a freshly built
self-capturing closure held only as a C++ return value in transit, since
by the next boundary it is already stored into a rooted env.

Objects crossing the interp/JIT boundary (a `Tensor`'s `impl` is a
`shared_ptr<TensorImpl>`) keep their existing handle unchanged by
either collector — nothing moves, so raw pointers handed to C++
interop stay valid across a collect.

### 14.8 Why not a precise/moving collector

A moving collector was considered and rejected, not deferred, because
Culebra fails two of its hard prerequisites today: (1) **precise
rooting** — a conservative (maybe-pointer) root cannot be updated when
its referent moves, so moving requires precise roots first; (2) **no
raw pointers escaping to GC-uncooperative code** — Culebra hands raw
pointers to C++ for Tensor and interpreter interop, and the tagged
`JitValue {tag, data}` representation stuffs pointers into integers,
which LLVM Statepoints cannot track. This is the same reason CPython,
Lua, and Ruby's default GC stay non-moving. The throughput lever moving
*uniquely* buys — compaction, collection cost proportional to
survivors — is second-order for Culebra today; a size-class/Immix-style
bump-region allocator (§14.2) gets most of the non-moving throughput
without paying the rooting/interop redesign cost. Revisit only if
fragmentation is measured to be a real cost, or if raw-pointer interop
is redesigned behind handles/pinning.

### 14.9 Invariants

1. Manual RC is kept and owns memory + deterministic `drop`; the
   collector is a backstop, never a replacement. Struct offset 0 stays
   `int64_t refcount`; collector metadata lives in the registry.
2. **Soundness (unconditional):** a collection never frees an object
   reachable from a root or another live object.
3. **Completeness is best-effort, not exact:** a conservative collect
   may over-retain an unreachable object when a stale stack/register
   word looks like a pointer to it — safe, bounded, expected. A
   `collect_precise(roots)` entry (explicit roots, no stack scan)
   exists for tests that need the exact reclaimed set.
4. Sweep runs each dead object's C++ destructor exactly once.
5. (If/when generational lands) every old→young store is recorded by
   the write barrier before the next minor collect.
6. The full suite is green under `CULEBRA_GC_STRESS=1`.

## 15. JIT ownership: structural leak-freedom

Ch.13–14 describe the RC/drop model and the backstop collector; this
chapter is the standing design for *how the JIT keeps RC placement
correct in the first place* — making leaks and double-frees in
generated code structurally impossible rather than a class of bug
fixed one call site at a time.

### 15.1 The rule

**Leaks and double-frees must be prevented by construction — the way
RAII (C++) and ownership/`Drop` (Rust) prevent them — not patched case
by case.** Hunting one leaking codegen site at a time is explicitly not
the strategy: every such point fix leaves the next un-audited path
exposed. New codegen that produces or consumes a heap value uses the
ownership layer below; a bare `emit_value_retain`/`emit_value_release`
in a *new* code path is a design smell to justify in review or,
better, refactor away.

Only the JIT has this problem — the interpreter's `shared_ptr` values
already make leaks and double-frees impossible by construction. The
JIT abandoned exact RC for hand-emitted retain/release (to avoid
`shared_ptr` atomic-refcount overhead) without originally pairing it
with either a rigorously enforced convention or a tracing backstop.
This chapter is that convention; Ch.14 is the backstop.

### 15.2 Ownership and rooting are different jobs

A refcount silently does two independent jobs, and conflating them is
what makes naive "this retain looks redundant, remove it" fixes crash:
(1) **ownership/release** decides *when* an object is freed
(release-to-zero, which also fires `drop`); (2) **rooting/liveness**
decides *what stays alive across a collect*.

**Rooting is already guaranteed independently of any single retain**,
by both shipping collectors (Ch.13 §13.4, Ch.14 §14.3) — a native
frame's `+1` is itself a rooting edge, so an in-flight temporary is a
root by construction, provided RC accounting is accurate (which is
exactly what this chapter's discipline establishes). So the
precondition for removing a "redundant-looking" retain is an
accounting proof — a refcount trace of the object's whole economy plus
the full test gate — never new rooting machinery.

A standing diagnostic makes the two failure modes distinguishable:
`CULEBRA_GC_NEVER=1` disables every collect; a crash that **survives**
it is a pure ownership bug (an over-release), one that disappears was a
rooting gap. Every crash investigated this way has turned out to be
the former — rooting has not been the source of a real bug since both
collectors shipped.

### 15.3 Prior art

The design synthesizes established RC/ownership models:

| System | What we take |
|---|---|
| Rust (affine ownership, `Drop`) | move-or-drop as the value discipline; a borrow is never freed |
| C++ RAII | the implementation vehicle — a handle whose destructor emits the release, correct across early-returns/exceptions |
| Swift ARC | *derive* retain/release placement from a uniform convention rather than hand-placing it |
| Perceus (Koka) | precise, deterministic-drop RC as the closest structural match |
| Nim ORC | validates "RC + cycle backstop" as a shipping combination |
| MLIR/Swift SIL ownership | the end state to aim at — ownership as a checked IR property, not a comment convention |

None fits wholesale — no borrow checker in codegen, and Culebra lowers
a dynamic AST rather than a functional IR with whole-program inference
— but the converged shape (**the compiler derives RC placement from a
uniform convention; nobody hand-writes retain/release**) is what §15.4
implements via C++ RAII handles over LLVM IR.

### 15.4 The design, layer by layer

Each layer removes one way to leak; together, a leak requires breaking
a C++ type/RAII invariant — a compile error, not a silent runtime leak.

**Uniform convention.** `compile(expr)` returns a `+1`-owned value for
every expression node. Parameters/receivers are borrowed (`+0`): the
caller retains before the call and the callee consumes the ref it's
handed. A value stored in a scope slot is owned by that slot; a `Cell`
does not retain on store, so the caller must own the ref it hands in.

**`Owned` — the handle for a transient `+1`.** A move-only RAII handle
(`Owned { JIT*, llvm::Value*, bool consumed }`) exposes `.borrow()`
(read without consuming), `.consume()` (hand the `+1` onward — into a
slot, a call, a return — and mark the handle spent; a **double-consume
is a codegen-time abort**, turning that bug class into a build failure
rather than a double-free), and `.drop()` (emit the release
immediately, for a value that dies rather than being handed on, since a
scope-exit destructor would place the release too late). The
destructor releases if the handle is still owned when it goes out of
scope. `Owned` only covers **straight-line temporaries** — a value
living across a loop iteration, or consumed on one control-flow arm and
released on another, needs the next layer.

**Scope slots own escaping values.** A value that must outlive the
current straight-line region is `.consume()`d into a scope slot; the
existing scope-unwind machinery releases it on **every** exit path —
fall-through, `break`/`continue`, early `return`, and exception unwind,
the last via a per-region cleanup landingpad (dead-code-eliminated when
the region can't throw) — one mechanism instead of a bespoke release
per site. This closed a real throw-path leak class: a scope's owned
locals, and their `drop()`, used to simply never run on `throw`. It
covers every scope-like region: lexical scopes, loop/match/try bodies,
the `for` iterable scope, and the function frame itself.

**Borrows can't be released.** A borrowed value is only ever touched
through `.borrow()` — no API both borrows and releases, so "released a
borrowed operand" is unrepresentable.

**Rooting is the collector's job, not the refcount's.** Per §15.2, no
scoped-pin/shadow-stack machinery is needed to keep in-flight
temporaries alive across a collect — both shipping collectors already
guarantee that. `Owned`/slot ownership is purely about *release
timing*.

**Cycles are swept, not counted.** Reference cycles are out of scope
for any RC discipline (Rust's `Rc` has the identical gap); the
mark-sweep backstop (Ch.14) reclaims cycles and any residue. With
ownership correct, the backstop is non-load-bearing in the steady
state — rare, not per-step — which is the performance point of doing
this at all.

**Helper ownership contracts.** Every codegen-owned `+1` live across a
may-throw runtime call has exactly one releaser on the unwind edge,
drawn from a closed set of contracts:

| Contract | Mechanism | Used for |
|---|---|---|
| Caller-cleans | `ThrowGuard`, a per-region RAII cleanup pad, eliminated when the region can't throw | builtin-method receivers/args, `compile_call`'s callee, the assignment lvalue, the UFCS callee |
| Callee-cleans-on-direct-throw | a guard inside the helper, armed only after user dispatch declines | operator entries, index/property-get under `own_receiver`, the not-a-function edge |
| Callee-consumes-on-every-exit | an owned-arg handle declared at entry, releasing on normal return *and* unwind | native method endpoints, HOF accumulators and callbacks |
| Invoker-cleans | invoker retains; callee frame releases on normal return; invoker's guard releases iff the callee throws | every user-dispatch window (`__op__`/`eq`/`hash`/`cmp`/`__index__`/getters) |
| Transfer | return the incoming `+1` as the result, or hand it into a capture cell/slot | iter-self methods, lazy-combinator capture cells |

Two contracts may never cover the same edge (two cleaners on one value
is a double-free); the mutual exclusion is always "user dispatch
declined" for helper guards and "the owner is entered" for consumed
values.

**The automatic unwind-temp window.** `Owned` releases through a C++
destructor, which a *runtime* LLVM-level throw cannot run — so
historically, any codegen-owned `+1` live across a may-throw call
leaked unless someone hand-placed a guard, and no corpus spelled every
such shape. Instead, the ownership layer now owns the unwind edge by
construction: every live `Owned` is registered with the JIT;
`emit_call` — the one point where a may-throw call gets its unwind
edge — spills every live, uncovered `Owned` into a per-function pool
slot for the duration of that call, and every cleanup pad releases the
pool first. A value already covered by a helper contract is declared
`UnwindCovered` and skipped, avoiding a double-free.
`CULEBRA_JIT_NO_UNWIND_TEMPS=1` disables it as a diagnostic twin to
`CULEBRA_GC_NEVER`.

**Block-pinned raws.** The last escape hatch was the moment a `+1` left
its handle: `consume()` used to return a bare `llvm::Value*` that could
cross basic-block boundaries with no layer tracking it — every recent
leak in practice was this shape. The invariant: a bare `+1` may only be
used in the basic block where it was consumed, which is sound and
complete because `emit_call` turns every may-throw call into an
`invoke` that terminates the current block — same block implies no
unwind edge ran while the value was bare. `consume()` now returns a
`Pinned` token recording its pin block; using it outside that block is
a codegen-time abort in every build mode, caught the first time the
pattern is *compiled* since the difftest corpus compiles nearly every
construct. `OwnedPhi` is the checked construct all `%Value` phis are
built through (each incoming declared and consumed in its own arm
block). `consume_unchecked()` is a justified, ratchet-counted escape
hatch for the handful of shapes that genuinely cross a block boundary
(mutually exclusive dispatch arms, a crossing already owned by a scope
slot, the prologue transfer into `declare_local`). The `compile_*`
return seam is typed to match: every `compile_*` helper returns
`Owned` (or an empty handle on decline), so a bare `+1` never crosses a
`compile_*` C++ return.

### 15.5 Invariants

If these hold, a leak requires breaking a C++ type/RAII invariant (a
build-time failure), not a runtime accident:

1. Every `+1` transient value is held in an `Owned` or immediately
   consumed.
2. `Owned` is consumed exactly once **or** dropped exactly once — never
   both, never neither (move-only + destructor + double-consume abort).
3. Every escaping value is consumed into exactly one scope slot; scope
   unwind releases every slot on every exit path.
4. Borrowed values are never released.
5. Rooting is provided by the GC layer independently of ownership
   refcounts.
6. Cycles and residue are reclaimed by the backstop; the backstop is
   not relied on for steady-state memory.
7. Every `Owned` `+1` live across a may-throw call has exactly one
   releaser on the unwind edge — the automatic window by default, or a
   helper contract declared via `UnwindCovered` — never both.
8. A bare `+1` exists only inside the basic block where it was
   consumed (`Pinned`); every `%Value` phi is built through `OwnedPhi`;
   a deliberate crossing is a `consume_unchecked` site with a declared
   releaser.
9. No `compile_*` helper returns a bare `llvm::Value*` — a `+1` crosses
   a compile-layer C++ return only inside an `Owned`.

Corollary: no correct codegen path contains a bare, hand-placed
`retain`/`release`. Remaining bare calls are audited migration debt,
counted (never allowed to grow) by `tools/check_rc_discipline.sh`, and
fall into a few legitimate categories the `Owned` layer isn't meant to
cover: per-*iteration* releases inside an emitted loop body, slot/scope
primitives below the `Owned` layer, values threaded as a function
parameter down a chain rather than held as a straight-line temporary,
and branch-spanning consume-or-release pairs where one arm consumes and
a different arm releases the same value.

### 15.6 Constraints specific to Culebra

- **Tagged `i64` values, not pointers.** Root/child enumeration reads
  the tag to decide whether `data` is a heap pointer; this, plus raw
  pointers escaping to Tensor/interpreter interop, is why LLVM
  Statepoints were rejected as the rooting mechanism (Ch.14 §14.8) — a
  value-ABI rewrite to support them would dwarf the ownership work.
- **The two backends must stay symmetric.** An ownership change is
  JIT-internal and must not alter observable behavior, error messages,
  or check timing/order versus the interpreter
  ([[feedback_check_jit_interp_symmetry]]).
- **Dynamic dispatch.** Method/operator targets are resolved at
  runtime, so ownership conventions are enforced dynamically at the
  call boundary, not by a static type of the callee.
- **A whole-function IR ownership verifier was considered and
  rejected**, adversarially reviewed: it would be blind to C++ helper
  interiors, and "balanced retains/releases" does not imply "correct
  lifetime" (an early-but-balanced release can still be a
  use-after-free). The block-pinned accounting above is deliberately
  narrower — it reasons only about codegen's own `Owned`/`Pinned`
  handles, not heap aliasing or object lifetime, which is why it
  sidesteps the rejected verifier's fatal objections.

## 16. Algebraic effects (source transform)

`effect fn`, `perform`, and `handle … with` are understood by no
backend. They are rewritten *at parse time* into plain culebra source —
synthesized classes plus calls into the `__Eff` runtime preamble — which
is then re-parsed and run by the interpreter, JIT, and AOT paths with no
effects-specific support of their own. The three backends stay symmetric
for free ([[feedback_check_jit_interp_symmetry]]). Generators use the
same lowering machinery, so this pass and the generator transform share
their source-slice and local-rewrite helpers.

Header root: `include/effects_transform.h` (the pass). The driver runtime
lives in `src/preambles/effects.cul` as the `__Eff` module.

Lowering follows a dynamic-scope, one-shot-resume model:

- **`effect fn f(...) { BODY }`** (with a body) lowers to a normal fn
  that *returns a computation object* — a flat-dispatch state machine
  whose `_step(rv)` runs the body until the next suspension point, then
  hands control back to the driver. A signature-only `effect fn op(...)`
  declares an operation and lowers to a throwing stub.
- **Suspension points** are a statement-level `perform op(args)`
  (SUSPEND) or a statement-level call to another effect fn (DELEGATE).
  The body may use arbitrary control flow — `if` / `while` / `for`
  (desugared to `while`) with `break` / `continue` / `return` — lowered
  by a flat-dispatch CPS builder (`build_dispatch`, the same shape the
  generator transform uses). An expression-nested `perform` is first
  hoisted to statement level by an A-normalization pre-pass
  (`anf_program`), so the CPS layer only ever sees statement-level
  suspensions.
- **`handle { BODY } with op(params, resume) { H }`** lowers to
  `__Eff.handle(<BODY as a computation>, "op", <handler adapter>)`. The
  driver walks the dynamically-scoped handler stack; `resume` is the
  one-shot continuation — an ordinary RC value, so leak-safety is
  inherited from the generator machinery it mirrors
  ([[project_rc_gc_correct_model]]). A plain (non-effect) fn can still
  `perform` an operation via `__Eff.perform_direct`, so effectful calls
  are not confined to `effect fn` bodies.

The computation object and driver share a return-tag protocol: each
`_step(rv)` returns a tag that tells the driver what happened.

```
0 = DONE      self._eff_val holds the computation's result
1 = SUSPEND   self._eff_op / self._eff_args describe a perform
2 = DELEGATE  self._eff_delegate is a sub-computation to drive
```

Two constructs are rejected at transform time — symmetrically, so no
backend miscompiles them: a `perform` in a control-flow condition or
iterable, and a nested `handle` that captures an enclosing binding.

Because the feature is entirely source-to-source, its cost surfaces as
extra generated source for the parser to chew on, not as backend
complexity; `CULEBRA_TRANSFORM_STATS=1` reports the culebra source volume
each transform emits (`=2` dumps the lowered source itself).

## 17. Net: raw sockets

### Where the logic lives

`include/net.h` is a value-neutral core in the shape of `http.h` /
`sqlite.h` / `proc.h`: no `Value` / `JitValue` / GC types, so both
`stdlib_interp.h` and `stdlib_jit.h` include it without pulling in
each other. The backends only marshal values and map an `IoStatus`
onto culebra semantics.

Framing (`read` / `read_line` / `read_exact` / `read_all`) lives in
the core, over a per-socket read buffer — not in the two adapters.
That is the difference between "the backends agree because both were
written carefully" and "the backends agree because there is one
implementation". Only the error *wording* is duplicated (a `ctx`
string per call site), and the sweep in `tests/test_net.cul` locks
that down.

### Blocking, non-blocking, and Ctrl+C

Sockets are non-blocking underneath, and every operation is
`wait_ready()` then retry. Two reasons:

- a poll/select readiness hint is advisory — a blocking `recv()`
  behind one can still stall past its timeout;
- `wait_ready()` polls in 100 ms slices and calls
  `throw_if_interrupted()`, so a blocked `accept` / `read` raises the
  cooperative `Interrupted` under interp, JIT, and AOT alike. The JIT
  has no inter-statement safepoint, so a post-call check would not be
  symmetric (same reasoning as `proc.h`).

### Handle table

A script handle is an `int64` index into a thread-local table, never
a raw fd — a forged or stale index is bounds-checked into a graceful
error and can never be dereferenced (the posture of `sqlite.h` and
File). Thread-local is correct because a socket handle is
`__nonsendable__`: it never crosses an isolate, so it never crosses a
thread. Every raw fd is owned by an `FdGuard` until it is interned,
so no error path — including the `Interrupted` thrown out of
`wait_ready()` — can leak one.

### Concurrent serve: the fd crosses, the handle doesn't

`listener.serve(handler, workers:)` accepts on the calling thread and
runs handlers on a pool, each worker owning its own culebra runtime —
the `Http.server` model, minus httplib. The subtle part: a socket
handle is `__nonsendable__` and lives in a thread-local table, so it
*cannot* be handed to a worker. What crosses the boundary is the raw
accepted **fd**; the worker interns it into its own table and builds
the handle there. The invariant is preserved rather than excepted.

The handler is serialized once at `serve` — so a non-Sendable handler
fails there, not on the first connection — and rebuilt per worker,
exactly as the Http server's route handlers are.

Backpressure is the queue: `submit` blocks while the job queue is
full, so a fast accept loop cannot outrun the workers without bound
(the kernel's listen backlog is the real buffer). On the way out — a
Ctrl+C leaves through the `Interrupted` throw — the pool's destructor
drops connections that never started and joins the in-flight ones.

### No AOT usage-gating axis

Unlike Tensor / Http / Compress / SQLite, `Net` pulls in no external
library: plain BSD sockets (plus `ws2_32` on Windows). There is
nothing to weak-stub and nothing to force-load, so it sits in the
base runtime archive and `aot_scan.h` gains no `aot_uses_net`.

### Platform notes

Windows waits with `select()` rather than `WSAPoll()`, which does not
report a failed non-blocking connect. SIGPIPE is suppressed per
socket (`SO_NOSIGPIPE`) or per send (`MSG_NOSIGNAL`). The Emscripten
(Playground) build has no raw sockets, so every entry point reports
that up front instead of half-working on emulated calls.
