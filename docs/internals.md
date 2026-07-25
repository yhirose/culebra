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

- `include/grammar_def.h` — the PEG grammar, single source of truth.
- `include/parser.h` — the cpp-peglib parser plus the AST accessors
  (`view_for`, `view_match`, `view_method`, …) every backend reads nodes
  through.
  There is no `ast.h`: the AST *is* `peg::Ast`.
- `include/interpreter.h` — tree-walking interpreter.
- `include/jit.h` — ORC JIT **and** the AOT object emitter (compiled
  only with `CULEBRA_ENABLE_JIT`); they share one lowering, so the two
  paths cannot drift.
- `include/jit_*.h` — the JIT split by concern: `jit_value` (tagging),
  `jit_mem` / `jit_slab` / `jit_gc` / `jit_owned` (memory), `jit_string`,
  `jit_iter`, `jit_dispatch`, `jit_runtime` (helpers callable from
  emitted code).
- `include/stdlib_interp.h` / `include/stdlib_jit.h` — the two halves
  of the standard library, kept symmetric by the drift check below.
- `include/runtime/` — the AOT-side runtime entry points
  (`runtime_aot.h`, `aot_scan.h`, `rt_macros.h`).

Memory management (reference counting, the tracing backstop, and the
JIT's structural leak-freedom discipline) is a cross-cutting concern
covered on its own in Ch.13–15.

## 2. Parser (cpp-peglib)

The grammar lives in `include/grammar_def.h` as a single PEG
specification fed to `peg::parser`. cpp-peglib gives us:

- PEG semantics (greedy, no left recursion as a hard rule).
- A generic AST (`peg::Ast`) built by the parser itself — productions
  do not need per-node C++ classes.
- Source position is propagated to every AST node automatically.

### Grammar blob

Meta-parsing the grammar text costs ~10 ms, which every process start
would otherwise pay. `tools/gen_grammar_blob.cc` serializes the
compiled grammar into `include/grammar_blob.h` (`just gen-blob`), and
`parser.h` loads that blob instead — guarded by
`grammar_blob_key()`, a hash of the grammar source: if the grammar was
edited without regenerating, or a cpp-peglib bump changed the layout,
the key mismatches and the parser falls back to `load_grammar()`. Both
paths then enable the AST and packrat parsing identically
([[project_startup_grammar_snapshot]]).

Why cpp-peglib (vs hand-written recursive descent):

- The grammar is in one file, readable as a spec next to
  `language.md`.
- Semantic actions stay close to syntax; nothing leaks into the
  lexer/parser dichotomy.
- Performance has not been a bottleneck — parsing time is dominated
  by interpreter/JIT compile, not by the parser.

AST nodes are `shared_ptr<peg::Ast>` produced by cpp-peglib itself.
Identifier resolution is deferred to a post-parse pass so the parser
can be context-free.

Two consequences worth knowing before touching the grammar:

- **Token text is a `string_view` into the source buffer.** Whoever
  owns the AST must keep the source string alive alongside it — that
  is why `LoadedModule` holds `source` (and `aux_sources` for spliced
  preamble text) next to the AST ([[feedback_ast_source_lifetime]]).
- **Every backend must read nodes through the `view_*` accessors** in
  `parser.h`, not by indexing `ast.nodes[i]`. cpp-peglib's
  `AstOptimizer` collapses single-child nodes, so raw indices shift
  when a production gains an optional element — the recurring source
  of "grammar changed, one walker not updated" bugs
  ([[project_grammar_accessor]]).

Parse failures surface as a `SyntaxError` carrying file, line, column,
and the parser's diagnostic, formatted by the CLI driver in a
`clang`-like fixed-width style.

## 3. Interpreter

### Value layout

`Value` is a `Type` tag (the twelve names `type_of` can return) plus a
`std::any` payload. `Nil` / `Bool` / `Long` / `Float` sit in the
payload directly; `String` holds a `std::string` by value; the
container types hold a struct whose *interior* is a `shared_ptr`
(`ArrayValue::values` is a `shared_ptr<vector<Value>>`, an object's
property map likewise), which is what gives them reference semantics
while `Value` itself stays copyable.

`Value`'s move constructor must `std::move` the `std::any` — a "move"
that copies deep-copies the boxed payload on every argument bind,
return, and swap (measured ~13% on a move-heavy loop). A move that
copies is a defect, not a pessimization.

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

Two C++ exception types carry the two kinds of throw:

- **User `throw expr`** throws the `Value` itself (`eval_throw` is
  literally `throw eval(...)`), so any culebra value can be thrown and
  caught unchanged.
- **Runtime errors** throw `culebra::CulebraError` (a
  `std::runtime_error` subclass carrying `kind` / `line` / `col`). Its
  constructor *and its copy/move constructors* publish the error to a
  thread-local pending slot, so a JIT catch pad can materialize the
  Object without re-inspecting the in-flight exception — an isolate
  re-throwing a stored error by copy has to publish too, which is why
  the copy constructor is not defaulted.

`catch` translates either into the `{kind, message, line, col}` Object
the language specifies. Defers live on the `Environment`
(`env->deferred`) and `run_deferred` pops them LIFO on every exit
path; a defer that itself throws propagates and abandons the rest of
that scope's defers (Swift's rule, not Go's). Scope-exit drop
resolution piggybacks on the tail of the same function, so defers
always run before the scope's resources are released.

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

Property access uses a V8/SpiderMonkey-style **monomorphic** inline
cache: every call site owns a private IC global holding
`{Shape*, slot_offset}` (`JitPropIC`; the write side is
`JitPropSetIC`, which also caches the property's mutability). The fast
path emits no runtime call at all — load `obj->shape`, compare against
the cached `Shape*`, and on a hit index straight into the slot vector.
A miss calls `culebra_runtime_object_get_ic`, which performs the real
lookup and refills the IC.

Shapes are interned process-wide (`ShapeRegistry`), so the comparison
is a pointer equality, and objects built the same way share one shape.

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

One cleanup pad covers both sides that can throw — the advance blocks and
the body — disposing the iterator before it re-raises. That span is what
makes §18.5's "dispose on any exception leaving the loop" true of a
throwing `next()` / `has_next()`, of an interrupt at the loop safepoint,
and of a generator body that throws while suspended, whose registered
defers only run through that dispose. The interpreter's `eval_for` covers
its producer call and its safepoint for the same reason.

The three values the loop owns — the iterable, the value the protocol is
derived from, and the iterator `iter()` returned — are scope slots of the
statement's own scope, declared in that order so LIFO teardown drops the
iterator before its source. That is what makes the exits uniform: the
ordinary scope machinery releases all three on drain, `break`, early
`return`, and any throw, including one out of `next()` / `has_next()`
mid-drain — so the cursor's own cleanup shrinks to calling `dispose()`.
The iterator used to be a bare cursor alloca instead, invisible to every
cleanup pad, and a throwing producer stranded one iterator per loop run
(Ch.15 §15.5 invariant 3, which the slot restores). The inline
`reduce` loops hold their accumulator the same way — the seed is live
from before the protocol is opened until the first iteration hands it to
the body scope, so it is registered with a construction cleanup that
releases it if the open, `has_next()`, or the count expression throws.

Emitting the body once is what makes nesting affordable. Inlining it under
each container arm instead multiplies it by six per level — a triple nest
emitted the innermost body 216 times, IR grew about 6.4x per level, and a
four-deep nest over a four-line program produced a million lines of IR.

`for v in a..b` (with or without `by <step>`) skips all of this and runs
a direct Long counter instead: no Range object, no heap iterator, and no
`{done,value}` object per step. Both backends do it, on different
evidence — the JIT fuses the range *literal* at compile time, the
interpreter fuses any range *value* it is handed — so a range that
reaches the loop through a variable is fused on one backend and generic
on the other. That is safe only because the counted loop is not a second
implementation of range semantics: the bounds, the step's sign, the
inclusive-end adjustment and the two errors (unbounded range, zero step)
come from the same decoder the generic range iterator uses, and the loop
body, scope, defer and break handling are shared with the generic path
rather than copied. The interpreter's win is the loop *entry*: opening a
generic cursor allocates a Range object plus an iterator object, about
3 µs, which is what made an inner loop re-entered per outer iteration
cost more than the work inside it.

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

The object a module returns is a plain object literal, so it carries no
namespace marker of its own; each backend stamps one on where it builds the
module — `Environment::resolve_from_lazy` for the interpreter,
`_jit_namespace_get_or_build` for the JIT and AOT (child isolates rebuild
through the same function). Both consult `lazy_namespace_static_name`
(shared.h) so the two paths cannot drift, and the marker makes unknown-member
reads raise the same `AttributeError` a C++ namespace's do. `Path` is absent
from that list on purpose: its module returns a class, and class property
misses read as `nil`.

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

- `libculebra_rt.a` — base runtime. Every feature choke in it is a
  **weak-symbol stub**, so on its own it references no tensor kernels,
  OpenSSL, zlib, SQLite, or window/GPU frameworks.
- One archive per feature, each holding the strong choke that
  overrides the weak stub:

  | archive | namespace it gates | what it drags in |
  |---|---|---|
  | `libculebra_rt_tensor.a` | `Tensor` | cpp-tensorlib; Accelerate + Metal on macOS |
  | `libculebra_rt_http.a` | `Http` | cpp-httplib + static OpenSSL |
  | `libculebra_rt_compress.a` | `Compress` | zlib |
  | `libculebra_rt_sqlite.a` | `SQLite` | the vendored SQLite amalgamation |
  | `libculebra_rt_canvas.a` | `Canvas` (windowed build) | raylib + SDL3 |
  | `libculebra_rt_scene.a` | `Scene` | raylib + SDL3 |
  | `libculebra_rt_webview.a` | `Webview` / `Desktop` | the OS WebView framework |
  | `libculebra_rt_wrap.a` | wrapped C++ classes | whatever the wrapped library needs |

`culebra build` always links the base, then **force-loads** a feature
archive only when the AST references its namespace — the strong choke
overrides the weak stub. So an unused feature links neither its archive
nor its external libraries. This is N+1 archives, not a 2^N matrix
([[project_linear_rt_archives]]).

The gating contract is easy to break from the *outside*: a vendored
library that calls a device allocator unconditionally (rather than
through its own runtime hook) compiles that reference into every
binary, and a program that never mentions `Tensor` then fails to link.
Re-run the full gate, AOT included, after every submodule bump — a
small diff can still break the contract.

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
default. `size()` returns bytes; there is no scalar-counting `length()`
— counting scalars or grapheme clusters goes through the iterators
(`code_points()`, `graphemes()`), which keeps the O(n) cost visible at
the call site. The user-facing side of this split is `guide.md` §4.2.

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

### StringView, StringLike, lazy graphemes

All three shipped ([[project_string_model]]).

**`StringView`** is a borrow over another string's bytes with the same
byte/scalar API, and its own `type_of` name. It is a first-class value,
not a parameter-only one: it can be stored in an Object or Array,
returned, and outlive the binding it was sliced from. Each backend
keeps the backing alive its own way, so no view can dangle:

- **Interp** — `Value::StringView` holds a `StringViewPayload
  { shared_ptr<const std::string> source; string_view sv; }`. The
  `shared_ptr` is the ownership edge; many views share one source.
- **JIT/AOT** — `TAG_STRINGVIEW` points at a heap `JitStringView
  { const char* ptr; uint64_t len; const char* owner_base; }` (24 bytes,
  `ptr`@0 / `len`@8 fixed because codegen loads the length at offset 8).
  `owner_base` is the registered GC base pointer of the backing String,
  or `nullptr` when the bytes are not GC-tracked (a literal's `.rodata`,
  an interned name). Strings are traced-only, so that edge is what roots
  the backing during a sweep (`_jit_gc_enumerate_children`).

The producers are `slice` / `split` / `split_iter` / `view` / `iter` /
`graphemes`, which is why iterating a large string allocates no
per-step copy.

**`StringLike`** is an annotation tag, not a type: `x: StringLike`
accepts both `String` and `StringView` (`Value::is_stringlike`), so a
helper reads a view without forcing a copy. Because multimethod
specificity scores annotations, it also serves as the dispatch tag for
"either string flavor".

**`graphemes()`** is a lazy iterator over extended grapheme clusters
(UAX #29) built on cpp-unicodelib's grapheme break table; it yields
one-cluster `StringView`s, so a clustered walk is allocation-free per
step as well.

Residual cost: `contains` / `starts_with` / `ends_with` on a
`StringView` materialize a temporary NUL-terminated copy per call. It
is reclaimed by the tracing backstop like any other String, but hot
loops over many views should `.to_string()` once instead.

## 7. Regex

### Library choice

The engine is `vendor/cpp-regexlib` — written for culebra, then split
out as a standalone single-header library ([[project_regex_self_hosted]]).
RE2 was considered and ruled out: it is a heavy vendor dependency for
one stdlib namespace, and bolting grapheme-cluster matching onto it
would have been more code than writing the engine.

### Engine model

- **Linear time by construction.** Matching is leftmost-first (Perl
  alternation / quantifier priority), run by a lazy DFA with a
  Thompson-NFA Pike VM as the semantic fallback. Neither backtracks,
  so catastrophic backtracking cannot occur — backreferences are
  therefore not supported, by the same design choice.
- **The match unit is the extended grapheme cluster**, not the code
  point: `.` consumes one user-perceived character, so `.` against an
  emoji ZWJ sequence matches the whole cluster. Offsets are byte
  offsets into the original UTF-8 subject and always land on cluster
  boundaries. A code-point unit is selectable.
- The Unicode tables are generated into the header, so the engine
  links nothing.

### Surface area

`Regex.compile(pattern, flags?)` returns a Regex object;
`re.test/match/find/find_all/split/replace_all` are its methods, and
`Regex.escape` quotes a literal. `re"..."` literals are sugar for
`compile` with the flags folded into the pattern as an inline group.

A Match is a plain data Object — `{value, start, end, groups, named}`,
with byte offsets and named captures under `named`. That shape is
built in `stdlib_interp.h` and mirrored by the JIT, so a match reads
identically on every backend.

## 8. Tensor

### TensorImpl

`Tensor` is a `shared_ptr<TensorImpl>` — an Op-tagged autograd tape
node whose value is a `tl::array` (the vendored `cpp-tensorlib` array
handle):

- `op: Op` + `inputs: vector<shared_ptr<TensorImpl>>` — the tape edge
- `shape: TensorShape`, `dtype: Dtype` (F32 only)
- `value: tl::array` — a lazy graph node, a zero-copy view, or a
  materialized buffer, depending on how it was built
- `grad`, `requires_grad`, `is_view`

Cycles are impossible by construction: `inputs` only points
input-ward, and `grad` is always a materialized `Const` with no
inputs, so RC alone reclaims a tape.

Views (transpose, reshape, slice) set `is_view` and share the base
buffer, so they stay zero-copy; `tensor_inplace_binop` refuses an
in-place write through one, since it would mutate the base.

### Kernel routing

Culebra owns the autograd (VJP) rules, the `TensorNoGradGuard`, the
language bindings, and the broadcast rules; `cpp-tensorlib` owns the
lazy graph, peephole fusion, `eval` (topological), the device
backends, and the buffer pool. Every evaluation funnels through the
`tensor_eval_node` choke point, which is what let the engine swap
underneath without touching the binding layer.

The device backends are CPU (AVX2 / NEON kernels; Accelerate supplies
the BLAS-shaped ones on macOS) and GPU (Metal on macOS, CUDA on
Linux / Windows). AOT builds pick up the matching runtime archive;
the per-binary gating goes through the `TL_RUNTIME_HOOKS` opt-in so a
tensor-free program links neither the GPU frameworks nor the kernels.

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

### GPU

There is no separate GPU type: the same `Tensor` runs on either
device, because `tl::array` — not culebra — owns the buffer and knows
which device it lives on. The split into a distinct `Matrix` /
`GTensor` primitive was planned while `TNode` held a raw
`shared_ptr<float[]>`; adopting `cpp-tensorlib` removed the reason for
it (see [`_history.md`](_history.md)).

Selection is process-global and switchable at runtime —
`Tensor.use_cpu()` / `use_gpu()` / `use_auto()`, with
`Tensor.gpu_available()` reporting whether a backend is compiled in
and reachable. `use_auto` (the default) picks per op by problem size,
since small tensors lose to kernel-launch overhead.

## 9. HTTP

### Library choice

cpp-httplib (vendored) provides blocking HTTP/1.1, SSE, and WebSocket
in one header. TLS is statically-linked OpenSSL — on macOS from
Homebrew's `openssl@3`, on Linux from the distro dev package — behind
the `CULEBRA_ENABLE_HTTP` option. async/await is *not* adopted —
concurrency is via threads.

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
Http.get / post / put / delete / head / request   # one-shot client
Http.client(base_url, **opts)                     # session (keep-alive, cookies)
Http.server()                                     # -> srv.get/post/... + listen
Http.sse(...) / Http.ws(...)                      # streaming clients
```

`Http.server()` returns a routed server object; `listen` blocks while
`listen_async` runs it on a worker pool (the `Desktop` facade uses the
async form). Both sides are blocking-with-threads, never async.

## 10. Module system

### Loader

`ModuleLoader` (`include/module_loader.h`) is shared by interp, JIT,
and AOT, so all three observe the same evaluation order, the same
caching, and the same I/O / parse / cycle errors. It does not evaluate
anything — it returns parsed modules and lets each backend walk them.

`load_recursive` is a depth-first walk over `import` statements:

1. Canonicalize the path (`resolve_module_path` — every component
   agrees on this key, or the register/lookup pair would miss).
2. If the path is already on the in-progress stack → cycle error; if
   it is already loaded → return the cached index.
3. Read the source into a **heap-allocated** `shared_ptr<string>`, so
   its `data()` survives moves of the enclosing `LoadedModule` — the
   AST's tokens are `string_view`s into those bytes.
4. Parse, `validate_module`, then extract the file's imports and
   recurse into each **before** recording self, so dependencies land
   at lower indices and the result vector is topologically sorted.

After the walk, `lint::check_module` runs over every loaded module:
the sound static diagnostics abort the program before any backend
evaluates, which is what makes them backend-independent.

### Cycle detection

The in-progress `stack_` is the cycle detector: re-entering a path
still on it throws `ImportError` with the cycle spelled out. There is
no separate SCC pass — a DFS over an import graph gets this for free.
Refactor through a shared third file.

### Module scope

Each dependency is evaluated in its own scope; its top-level bindings
stay private except for the names collected by `export`, which become
one immutable export Object bound under the importer's chosen name.
The entry module evaluates last and shares the caller-facing scope, so
its top-levels remain visible to whoever invoked the program.

### Why explicit imports

Deriving the graph from unresolved identifiers was tried and dropped.
An explicit statement is the only place a reader or a tool has to look
to know a file's dependencies; it gives `culebra lint` an unambiguous
unused-import warning (and a safe `--fix`), and it lets the AOT
bundler and tree-shaker work from a graph that is known at parse time
rather than inferred.

## 11. Build & vendor

### Vendor tree (`vendor/`)

| Library | Purpose | Linkage |
|---|---|---|
| `cpp-peglib` | PEG parser (Ch.2) | header-only |
| `cpp-linenoise` | REPL line editor | header-only |
| `cpp-unicodelib` | Unicode tables (scalar, grapheme, case) | header-only |
| `cpp-embedlib` | Bake static archives into the driver binary (`cpp_embedlib_add()`) | header-only |
| `cpp-regexlib` | Regex engine (Ch.7) | header-only |
| `cpp-httplib` | HTTP stack (Ch.9) | header-only (+ static OpenSSL for TLS) |
| `cpp-tensorlib` | Tensor engine and device backends (Ch.8) | header-only |
| `raylib` + `SDL` | Window / input backend for `Canvas` and `Scene` | static archives, built from source (cached, below) |
| `webview` | Native WebView window for `Webview` / `Desktop` | header-only (+ the OS WebView framework) |
| `sqlite` | SQLite amalgamation for the `SQLite` namespace | compiled in-tree |

Except for OpenSSL and the OS-provided frameworks, every dependency is
header-only or built from vendored source, so `culebra build` produces
self-contained binaries.

### CMake structure

- `CMakeLists.txt` (top-level) — defines the `culebra` driver,
  optional LLVM linkage, the base + per-feature runtime archives, and
  embed tests.
- `vendor/cpp-embedlib/cmake/cpp-embedlib.cmake` — provides
  `cpp_embedlib_add()` used to bake `libculebra_rt.a` and every
  per-feature archive (Ch.5) into the driver.

Feature options gate both the namespace and its archive:
`CULEBRA_ENABLE_JIT` (LLVM linkage — off gives a ~1 MB driver with no
LLVM dependency), `CULEBRA_ENABLE_HTTP`, `CULEBRA_ENABLE_SQLITE`,
`CULEBRA_ENABLE_WEBVIEW` (on by default; self-disables on Linux
without the GTK4 / WebKitGTK dev packages),
`CULEBRA_ENABLE_CANVAS_WINDOW` (on by default where a window works —
macOS today; the environment variable `CULEBRA_CANVAS_WINDOW_DEFAULT=OFF`
flips that default for every configure in a job, which is how CI opts
out), and the windowed opt-in `CULEBRA_ENABLE_SCENE`.

Both windowed namespaces link the same vendored SDL3 + raylib statics.
Those depend only on their own sources and the target platform — never
on culebra's build type, LTO or JIT settings — so they are installed
once into `~/.cache/culebra/deps/<platform>-sdl<rev>-raylib<rev>/`
(root overridable with `CULEBRA_DEPS_CACHE`) and every build dir and
worktree that resolves the same key links them without rebuilding;
a submodule bump lands under a new key, so a stale artifact can't be
picked up. A cold build installs into a staging dir and
`cmake/publish_dep.cmake` renames that into place, so the cache entry
appears atomically and a concurrent cold build elsewhere never reads a
half-written archive.

### Dependency policy

- Prefer header-only vendor libraries over package-managed
  dependencies. The repo should clone-and-build with no system
  packages besides a C++23 compiler (and optionally LLVM for the
  JIT); OpenSSL for the HTTP feature is the one exception.
- `vendor/` is git submodules, pinned per commit — a fresh clone needs
  `git submodule update --init --recursive` before it will build.
  (`sqlite` and `webview` are committed sources rather than
  submodules, being single amalgamated files.)
- Adding a new vendor lib requires a Why entry in this chapter.

## 12. Test runner

`culebra test [paths...]` (`include/test_runner.h`) walks each
directory argument for `test_*.cul`, takes file arguments as-is, and
registers `test` / `@test` / `@parametrize` as ambient globals for the
duration — the matcher family is a language-level global and needs no
such injection. Flags: `--filter` (name substring), `--bail [n]`,
`--list`, and `--reporter json`.

Two reporters exist (`Reporter::Default`, `Reporter::Json`). The JSON
one emits NDJSON — one object per event — and captures the test's own
stdout into the event rather than interleaving it with the stream,
which is what makes the output machine-consumable.

**Fixtures are dependency-injected by name.** A test's positional
parameters are resolved against the surrounding environment, so any
`fn` in scope can be a fixture with no decorator. Resolution is
memoized per test (one instance shared by direct and transitive
mentions, fresh across tests) and detects fixture cycles, throwing
`CycleError` with the chain.

The doctest runner (`include/doctest_runner.h`) is the other half:
it scans markdown for fenced ` ```culebra ` blocks, reads a
block-leading `# doctest:` directive, and assembles expected stdout
from `# =>` / `# => |` markers with `# !!` for an expected throw
([[project_doctest_convention]]). Only `skip` is honored today; the
`compile-only` and backend-filter directives are reserved, and blocks
carrying them run normally. Every block runs in a fresh interpreter —
the runner is interp-only.

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
