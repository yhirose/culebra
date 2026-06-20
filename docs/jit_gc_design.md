# JIT GC Rewrite — Design Spec (Conservative, Generational, Non-Moving)

Status: **Shipped (2026-05-30).** The JIT GC is manual reference counting
plus a conservative, non-moving mark-sweep backstop.

This document is the authoritative spec, written to be implementable phase by
phase, prioritising **safety and code simplicity first**, performance later.

---

## 1. Motivation

The current JIT does **manual reference counting**: codegen emits
`emit_value_retain` / `emit_value_release` at every ownership boundary
(assignment, call args, method receivers, operator operands, scope
exit). This is the source of a whole bug class:

- **Leaks** when a release is missing (method receivers, operator
  operands, loop body values, …).
- **Double-frees** when a release is added where the value was already
  consumed (the ownership convention is *non-uniform* across call
  boundaries — even operator operands turned out to be inconsistent).

Manual RC also cannot reclaim **reference cycles**, and without a tracing
backstop a missed release or a cycle is **permanent** — it persists for the
process lifetime (microgpt JIT grew to ~5 GB). A backstop is what turns these
into at-worst-delayed reclamation.

The **interpreter has none of these problems** because it uses
`shared_ptr` — refcounting that is *automatic and exact* (C++
destructors). The interp is the safety bar. The JIT abandoned exact RC
for hand-emitted RC to avoid `shared_ptr` atomic-refcount overhead, but
did not pair it with either (a) a rigorously enforced ownership
convention or (b) a tracing backstop.

**Comparison with established runtimes** (see survey in commit
history / memory):

- CPython/PHP: RC + **backup tracing** collector → leaks self-heal.
- Java/Go/V8/Lua: **tracing** GC, no manual RC; roots via stack
  maps/safepoints or conservative scan.
- Swift: compiler-inserted RC verified by **OSSA** (+0/+1 typed).
- Ruby/Boehm: **conservative stack scanning** → C-extension locals are
  auto-rooted; no manual rooting at all.

## 2. Decision

> **DECISION REVISED 2026-05-30 — CPython-style hybrid, NOT pure tracing.**
> The original pure-tracing decision below is *superseded*. Implementation
> (Phase 1 commits 1–2) and a design review surfaced a hard constraint the
> original missed: **`drop` is a deterministic, scope-exit finalizer** (the
> interp fires it via `shared_ptr`; `test_drop` asserts ordering + the
> cross-backend symmetry rule requires the JIT to match). **Deterministic
> finalization under shared ownership logically requires full reference
> counting** — a drop-bearing object held inside a non-drop container can
> only fire its `drop` deterministically if that container's death is
> deterministic too, which recurses to *every* object being reference
> counted. "RC only on drop objects" is non-deterministic for
> container-held objects, so it does not work.
>
> Therefore: **keep the manual RC** (it owns memory + immediate reuse +
> deterministic `drop`), and **add a sound, conservative, non-moving full
> mark-sweep collector that runs as a *backstop*** — reclaiming the residue RC
> cannot: reference **cycles** and objects leaked by a **missed release** (both
> become unreachable from roots, so a full mark-sweep frees them regardless of
> their stale refcount). This is the CPython / PHP model (§1's first survey
> bullet).
>
> **Value proposition:** RC stays the primary memory manager; the conservative
> mark-sweep backstop reclaims only the residue RC cannot. RC bugs are thereby
> downgraded from *permanent leak* to *slightly delayed reclamation*, which
> dissolves the microgpt ~5 GB class.
>
> **§7 is corrected, not achieved:** "delete all manual RC" is logically
> incompatible with deterministic `drop`. The retain/release machinery
> stays; what changes is the collector.
>
> **Layout (plan A):** `int64_t refcount` stays at struct offset 0 (the
> existing retain/release IR is undisturbed). The collector's per-object
> metadata (mark bit, type tag, size) lives in the **registry** (an
> address→metadata map), NOT an in-object header — counter-intuitively the
> *smaller* rollback, since it revives the original retain/release IR
> verbatim instead of rewriting 92 emit sites to a trailing field. A later
> perf phase MAY move the mark bit to a trailing in-object field (offset 0
> stays `refcount`, so the IR is still untouched).
>
> **Finalization invariant (Phase 1+):** **`drop` is fired by RC's
> release-to-zero exactly once; sweep NEVER fires `drop`.** RC, on
> release-to-zero, fires `drop` + frees memory + de-registers. Only objects
> RC could not reclaim (cycles / missed-release) reach the backstop, which
> reclaims their *memory only* (cycle members do not fire `drop` — matches
> the current behavior and Python's `__del__` rule). So `drop` runs
> structurally at most once; there is no double-finalization path. Any
> "sweep fires drop" scaffolding from the pure-tracing draft must be removed.
>
> The sections below (object model, registry heap, conservative root
> scanning, mark-sweep, safety devices) still apply — they describe the
> **backstop collector**. Read "the collector" as "the backstop", not "the
> sole memory manager". The Phasing (§13) and "remove manual RC" (§7) are
> the parts rewritten by this revision.

--- *original (superseded) decision follows* ---

Replace the JIT's manual RC with a **conservative, generational,
non-moving mark-sweep tracing GC** (the Ruby MRI / Boehm model).

Rationale (prioritising safety + simplicity):

- **No manual acquire/release anywhere.** Generated code and C++
  runtime functions just hold values on the machine stack; the
  collector finds roots by scanning the stack + registers. This deletes
  the entire bug class above — including the temporaries inside C++
  builtins (the receiver/operand leaks) — for free, exactly the way
  Ruby C extensions need no manual rooting.
- **Lowest-risk integration.** The JIT already keeps values in stack
  slots; we mostly *delete* the retain/release emission rather than add
  machinery (no shadow stack, no statepoints).
- **Safe failure direction.** A misidentified pointer over-retains
  (a bounded, rare leak on 64-bit) — it never corrupts memory. Contrast
  precise rooting, where a *missed* root is a use-after-free.

**Non-goals (now):** moving / compaction / copying nursery. These need
precise roots. The migration target for those is **LLVM Statepoints
(`gc.statepoint`)** — see §12. We deliberately accept a non-moving,
free-list heap for the first rewrite.

**Scope:** the JIT runtime only. The interpreter keeps its `shared_ptr`
model (already safe). Cross-backend shared objects (e.g. Tensor) keep
working via the existing handle boundary; see §11.

## 3. Object model

GC manages the six refcounted heap structs: `JitObject`, `JitArray`,
`JitCell`, `JitClosure`, `JitSet`, `JitTensor`.

Each gets a **GC header** (replacing the current `int64_t refcount`
first field — refcount is removed entirely):

```
struct GcHeader {
  uint8_t  mark;       // mark bit (+ colour for future incremental)
  uint8_t  type_tag;   // GC_TAG_* — drives enumerate_children & dtor
  uint8_t  generation; // 0 = young, 1 = old (Phase 2)
  // padding; total kept at 8 bytes so existing IR GEP offsets shift
  // uniformly. (Codegen field offsets are regenerated, not hand-kept.)
};
```

**Variable-length buffers stay C++-owned, not GC-allocated.**
`JitArray::items`, `JitObject::slots` (a `std::vector`), the
`AnyKeyMap`, `key_order`, closure `captures`, etc. remain owned by their
struct and are released by the struct's **C++ destructor** at sweep
(RAII). The GC only tracks the fixed-size structs; their contents
(child `JitValue`s) are reached during marking via
`enumerate_children` (which already exists and is reused verbatim).

This keeps the GC simple (uniform fixed-ish nodes) and makes sweep
cleanup automatic (`~JitArray()` frees `items`, `~JitObject()` frees the
vector and sidecars — no hand-written teardown).

## 4. Heap & allocation

**Simplicity first: start with a registry heap; optimise later.** The
allocator is an *implementation detail of `gc_alloc` / `is_gc_object`*,
not part of the GC algorithm — so it can start dead simple and be
optimised without touching mark/sweep/roots.

- **Phase 0/1 — registry heap.** `gc_alloc` = `malloc(header+payload)` +
  record the object pointer in a registry (`address → {type_tag, size}`).
  `is_gc_object(p)` (§5) = the registry contains `p` as an object start.
  Trivially correct, no pages/bitmaps/size-classes. Validates roots,
  marking, sweep, and the safety tooling with minimal code. (Sweep frees
  via the registry.)
- **Phase 2a — flat-array registry (DONE).** The first registry used
  `std::unordered_map<void*, GcHeader>`, whose per-object node malloc/free
  was measured as the entire alloc-churn overhead (+12–21% on object/array
  churn; realistic workloads unaffected). Replaced with `GcRegistry`, an
  open-addressing flat hash map storing `{key, header}` inline (no per-entry
  allocation, tombstone reuse) — recovered the overhead (churn 1.21×→1.02×).
  Only `adopt`/`forget`/`find`/iteration changed; mark/sweep/roots are
  unchanged. Under plan A the runtime still `new`s the structs (RC owns the
  memory); the registry is a side table.
- **Phase 2b+ — size-class / region allocator (future, only if measured).**
  If allocation throughput is later shown to need more, a non-moving
  size-segregated / Immix-style bump-region allocator (Go / JSC model) is the
  next step — it again only swaps `gc_alloc`/`is_gc_object`/iteration.
  **Moving / copying is deliberately NOT on this path** (see §12).

`gc_alloc(size, type_tag)` is the runtime entry that replaces today's
`new JitObject()` etc.; it never calls a C++ destructor implicitly.
Allocation is the GC trigger point.

## 5. Root finding (conservative)

At collection time, the collector treats as a **candidate root** any
machine word that, read as a pointer, lands on a valid object start in
a GC page (validated via the page allocation bitmap + size-class
alignment). Sources of candidate words:

1. **Machine stack** of every mutator thread, from the collection
   point's SP up to the thread's recorded stack base. This covers both
   JIT-generated frames and C++ runtime frames — so a `JitValue` held
   in any C++ builtin local is rooted automatically.
2. **Callee-saved + all registers**, flushed to the stack at collection
   entry (via a register-flush routine / `setjmp`-style spill) and
   scanned as part of the stack.
3. **Explicit global roots**, registered with the GC: the namespace
   object table, module caches, REPL globals, exception carriers, the
   defer stack — anything holding a `JitValue` outside a scanned stack.
   (Analogue of Ruby's `rb_gc_register_address`.)

**Correctness argument (why optimized JIT code is safe):** any GC
pointer live across a call that can trigger GC must, by the platform
calling convention, be either spilled to the stack or held in a
callee-saved register before the call — caller-saved registers are not
preserved. At the moment GC runs (inside `gc_alloc`, deep in the call
chain), every such value is therefore on the machine stack or in a
flushed callee-saved register, and is found by (1)+(2). This is exactly
why Boehm/Ruby conservative GC is sound under an optimizing compiler.

**Representation note.** A `JitValue` is `{int8 tag, int64 data}`. The
scanner does not consult the tag; it tests each 8-byte word, so a heap
`data` pointer is found regardless of tag. A non-pointer scalar (a
`Long` whose bits coincide with a heap address) can be a *false* root —
bounded over-retention, acceptable. `data` always points to the object
base (no interior pointers), simplifying validation.

## 6. Collect: mark & sweep

**Mark:** from the root set, trace transitively. For each live object,
set its mark bit and push its children — obtained by the existing
`enumerate_children(ptr, type_tag, out)` — onto the mark stack. Iterate
to fixpoint. Conservative roots are pinned (non-moving, so nothing to
update).

**Sweep:** walk all pages; for each unmarked object, run its C++
destructor (`~JitObject` etc. — frees `items`/`slots`/sidecars via
RAII) and return the slot to the free list; clear all mark bits.

A full mark-sweep from real roots **reclaims any unreachable object
regardless of any stale bookkeeping** — this is the property the
current collector lacks and the reason leaks become permanent today.

## 6a. Finalization (`drop`) — deterministic, owned by RC (CPython model)

**Decision (2026-05-30; see §2 revision banner).** `drop` stays a
**deterministic, scope-exit** finalizer — the interp fires it via `shared_ptr`
and the JIT must match (the cross-backend symmetry rule; `test_drop` asserts
parent-before-child ordering). Deterministic finalization under shared
ownership **requires full reference counting** (a drop-bearing object held in
a non-drop container fires its `drop` deterministically only if the
container's death is deterministic too — which recurses to every object). So:

- **RC owns memory + `drop`.** Retain/release stay. Release-to-zero fires
  `drop` (once), runs the C++ teardown (frees `items`/`slots`/sidecars/`impl`),
  and de-registers the object — exactly the current behavior. `drop` timing is
  the interp's `shared_ptr` timing.
- **The collector is a backstop, not the memory owner.** It reclaims only what
  RC cannot: **cycles** and objects leaked by a **missed release**. These are
  unreachable from roots, so a full mark-sweep frees them regardless of their
  stale refcount.
- **Sweep NEVER fires `drop`.** Backstop-reclaimed objects are cycle members /
  leaked residue; their `drop` is intentionally skipped (matches the current
  behavior and Python's `__del__` rule — cycle finalizer order is undefined).
- **Finalization invariant:** `drop` is fired by RC release-to-zero **at most
  once**; the sweep path frees *memory only*. RC reclaims an object before it
  ever reaches the sweep set (it is de-registered on free), so there is no
  double-finalization and no double-free path.

The retain/release machinery is kept (it owns memory and deterministic
`drop`); the conservative backstop reclaims only the residue RC cannot.

## 7. RC + backstop division of labour

- **RC owns memory.** `emit_value_retain` / `emit_value_release` /
  `culebra_runtime_value_*` / cell retain-release / the scope-exit releases /
  `array_push`'s +1 steal all stay — RC is the primary manager.
- **The backstop reclaims the residue.** The conservative full mark-sweep
  (`jit_gc.h`) frees what RC cannot — cycles and missed-release leaks.
  Allocation registers each object with the collector; it runs periodically +
  at exit and reclaims the RC-unreachable residue.
- **Sweep teardown** reuses the existing per-type teardown (the body of
  `_culebra_value_release_impl`) to free buffers, **minus** the `drop` call
  and **minus** the recursive child release (children are reclaimed by their
  own sweep / RC), and frees + de-registers the object.
- `release_scope_slots`, loop-body releases, method-receiver releases, etc.
  all **stay** — they are correct RC bookkeeping.

The simplification the rewrite buys is now **a sound collector** (cycles +
missed-release residue self-heal), not the deletion of RC.

## 8. Generational layer (Phase 2 — perf, not correctness)

Non-generational full mark-sweep (Phases 0–1) is *correct and safe* on
its own; generational is a throughput optimization added only after the
base is solid.

- **Young / old by `generation` byte.** Allocations are young; objects
  surviving N collections are promoted to old (still non-moving — just
  a flag; no copy).
- **Minor collect** traces young + the **remembered set**; majors trace
  everything.
- **Write barrier (required for soundness of minor collect).** Every
  store of a heap `JitValue` into an old object's field records the old
  object in a **card table / remembered set**, so a minor collect does
  not miss an old→young edge and free a live young object. This is the
  soundness device the current generational collector lacks. Codegen
  emits the barrier at object-field stores (a cheap card mark); this is
  *structural*, not per-value RC, so it is not the error-prone kind of
  manual bookkeeping.

## 9. Safety devices (build these alongside, per the survey)

- **GC stress mode** (`CULEBRA_GC_STRESS=1`): collect on *every*
  allocation. Surfaces rooting/marking bugs deterministically instead of
  flakily (SpiderMonkey `gcZeal` model). Run the suite under it in CI.
- **`GC.stat()`** (already implemented): `live_objects` / `heap_bytes`
  introspection — repurposed to count GC-tracked live objects.
- **Leak regression tests** (`tests/test_gc_no_leak.cul`, already green
  on interp) become the JIT acceptance gate.
- **Debug fill** freed slots with a poison pattern; assert on use.
- **Heap verify** pass (debug): walk all objects, check every child
  pointer is a valid heap object — catches marking/enumeration bugs.

## 10. RAII in the C++ implementation

The GC itself is written with RAII, never hand acquire/release:

- Object teardown is the struct **destructor** (sweep just calls it).
- Stop-the-world / safepoint coordination uses a scope guard
  (`StopTheWorld stw;` resumes mutators in its dtor).
- Internal GC data structures use standard containers / smart pointers.
- Global-root registration uses a scoped registrar where lifetimes are
  scoped.

## 11. Cross-backend & threading

- **Interp** keeps `shared_ptr` RC. Its accurate RC means the only residue
  is reference *cycles*, reclaimed by `InterpGC` — a precise CPython-style
  cycle collector (gc_refs subtraction + BFS + clear). It tracks Array ValVecs
  and captured Environments, so the closure↔environment cycle that the JIT
  backstop reclaims is reclaimed on the interpreter too — the two backends stay
  behaviorally symmetric. (Object-/Tuple-/Set-rooted cycles are not yet tracked
  interp-side; the conservative JIT backstop covers all.) Objects that cross
  the interp/JIT boundary (Tensor `impl` is a `shared_ptr<TensorImpl>`) keep
  their existing handle: the JIT `JitTensor` struct is GC-managed, its `impl`
  shared_ptr is released by `~JitTensor` at sweep. No moving, so raw pointers
  handed to C++ stay valid.
  - **Rooting: the precise collector is precise only if every live value is
    reachable from a registered root.** `InterpGC` does **not** scan the C++
    stack, so two classes of live-but-stack-only values must be rooted
    explicitly or they get swept mid-program (a use-after-free surfacing as a
    spurious `NameError` for a captured variable):
    1. **Active env chains.** Collection only runs at **statement
       boundaries** (`bump()` sets a `pending_` flag; `collect()` actually
       fires from the STATEMENT dispatch and from `invoke_user_function`'s
       entry). At that safe point the live envs are the current statement's
       `env` plus every caller's `env` still on the C++ call stack. Each call
       pushes its caller env onto `frame_roots_` (RAII `FrameRootGuard`), and
       `collect()` seeds the mark set by walking the full `outer` chain of the
       current env and every `frame_roots_` entry. Without this, a value bound
       directly in an *untracked* scope env (e.g. `mut loss` in a `while` body
       that defines no closure of its own) is unreachable from any tracked node
       and its captured def_envs are swept.
    2. **In-flight C++-stack temporaries.** Deferring the actual collect off
       `bump()` to the next statement safe point also fixes the freshly-built
       self-capturing closure held only as a C++ return value in transit — at a
       statement boundary it has already been stored into a rooted env.
- **Isolates / threads** (concurrency roadmap): each thread registers
  its stack base; collection is stop-the-world across the isolate's
  threads at safepoints (allocation points + loop back-edges). Per-isolate
  heaps mean most collections are thread-local. Conservative scan needs
  no per-thread shadow stack — just each thread's stack bounds.

## 12. Precise / moving (Option C) — conditional future, NOT the default path

> **Decision (2026-05-30): moving/copying GC is deliberately NOT pursued for
> culebra.** It is kept here only as a conditional future option, not the
> roadmap.

**Why moving is the wrong fit.** A moving collector has two hard
prerequisites culebra fails today: (1) **precise rooting** — you must update
every root when an object moves, and a *conservative* root (a maybe-pointer)
cannot be updated, so moving ⟹ precise; (2) **no raw object pointers escaping
to GC-uncooperative code** — culebra hands raw pointers to C++ for Tensor and
interpreter interop (§3 keeps the heap non-moving precisely so those stay
valid), and the tagged `JitValue {tag, i64 data}` representation stuffs
pointers into integers, which LLVM Statepoints cannot track. This is the same
reason CPython, Lua, Ruby (default), and Go stay non-moving. Reworking the
value ABI + the interop boundary to enable moving would dwarf the GC itself.

**The throughput lever is non-moving anyway.** The win moving *uniquely* buys
is compaction (anti-fragmentation) + collection-cost ∝ survivors — both
second-order for culebra now. Fast *allocation* is achievable non-moving via a
size-class / Immix bump-region allocator (§4 Phase 2b, the Go/JSC model). So
even when throughput is the goal, the next step is a better non-moving
allocator, not moving.

**Revisit triggers (only then reopen this).** (1) fragmentation is shown by
measurement to be a real cost a non-moving allocator can't address; or (2) the
raw-pointer interop is redesigned behind handles/pinning. If reopened: migrate
roots from conservative scan to **LLVM Statepoints** (`gc.statepoint` /
`gc.relocate`). The object model (§3), marking (§6), generational structure
(§8), and safety devices (§9) are designed to **carry over**; only root
finding (§5) and heap moving change — keeping the heap non-moving now is what
makes that a localised change rather than a second rewrite.

**Note on precision vs speed.** Precise rooting *without* moving buys only
precision (it would make the conservative collector's bounded last-batch
retention — see the self-closure leak gate — exact), not speed. That precision
alone does not justify the Statepoints cost; it rides along only if moving is
ever adopted.

## 13. Phasing

> **Shipped (2026-05-30).** Phases 0–1 + 2a landed on master; the conservative
> backstop is the JIT's collector, alongside the kept RC. Phase 2b /
> generational / §12 remain future, measurement-gated.

- **Phase 0 — scaffolding (allocate-only; validate the scary parts in
  isolation).** The two genuinely dangerous parts of a conservative GC
  are (1) *does the scan find every live root?* (a miss → use-after-free
  in Phase 1) and (2) *is pointer validation correct?*. Phase 0 builds
  and validates both **without ever freeing** — so the scanner can be
  run and *asserted* against known-live objects, and the false-root rate
  measured, while programs still work (allocate-only leaks, fine for the
  gate). Only once the scanner is trusted does Phase 1 sweep based on it.

  Built as a **self-contained module** (`include/jit_gc.h`) with no
  dependency on the JIT, unit-tested standalone first:
  - `GcHeader` (8 bytes, replaces the `refcount` field; carries
    `type_tag` for sweep's per-type dtor dispatch + `mark`/`generation`).
  - **Registry heap** (§4): `gc_alloc` = malloc + register; `is_gc_object`
    = registry lookup. + unit tests (live=true, interior/random/freed=
    false).
  - **Conservative scanner** `gc_scan_roots`: `setjmp` to flush
    callee-saved registers onto the stack, capture stack base
    (`pthread_get_stackaddr_np`), walk `[sp, base)` word-aligned, test
    each word with `is_gc_object`; plus explicit global-root registry.
    + a test that a known object held in a local is found as a root.
  - `GC.stat()` repointed to the new heap; `CULEBRA_GC_STRESS` runs
    scan+heap-verify each alloc; poison-fill infra; heap-verify
    (`enumerate_children` children all pass `is_gc_object`).
  - Compiled allocate-only and validated standalone before the in-tree
    wiring landed.

  Phase 0 ships nothing user-visible; it de-risks Phase 1.
- **Phase 1 — conservative full mark-sweep backstop. DONE (2026-05-30).**
  Conservative root scan + explicit container roots (module/namespace tables,
  trait-default + multimethod closures, REPL globals, defer stack, exception
  carrier; cached namespace objects pinned) + mark (`enumerate_children`) +
  sweep (per-type buffer teardown, NO `drop`, NO recursive child release).
  Manual RC is **kept** (§2 revision corrected §7's "delete all RC"); the
  collector runs as a backstop on a threshold (adaptive `max(100k, live*2)`),
  at `GC.stat()`, and at exit, with `CULEBRA_GC_STRESS=1` collecting every
  allocation. Achieved: the three leak gates pass on JIT, the full suite is
  interp/JIT symmetric (the self-closure gate is JIT-only green — interp
  cannot break the cycle), the suite is crash-free under GC stress (root
  set is complete), OFF is unchanged, and microgpt JIT RSS is flat.
- **Phase 2 — generational.** `generation` byte, promotion, write
  barrier + card table, minor/major. Acceptance: throughput recovered
  to ≥ current on the benchmark set, still green under stress.
- **Phase 3 (future) — precise/moving via Statepoints (§12).** Only if
  measurement justifies it.

## 14. Risks & open questions

- **Register/stack scanning portability** (ARM64 macOS first): correct
  register flush, stack-base capture per thread, no values lost to
  register-only liveness across `gc_alloc`. Validate with GC stress.
- **False-root over-retention rate** on real workloads — measure;
  expected negligible on 64-bit.
- **Throughput** of free-list (non-moving) allocation vs the future
  copying nursery — accept for now; Phase 2 generational recovers most.
- **`gc_alloc` as the only safepoint** is fine for single-threaded;
  multi-thread needs back-edge safepoints too (concurrency phase).
- **LLVM keeping a GC pointer only in a caller-saved register across a
  non-`gc_alloc` call** that *transitively* allocates — covered by the
  calling-convention argument (§5), but verify under stress with
  inlining on.

## 15. Invariants (must always hold)

1. **(Corrected by §2 revision.)** Manual RC is KEPT: `emit_value_retain` /
   `emit_value_release` stay in codegen and own memory + deterministic `drop`.
   The collector is a backstop, not a replacement for RC. Layout plan A:
   struct offset 0 is `int64_t refcount`; the collector's per-object metadata
   (mark/tag/size) lives in the heap's address→metadata registry, NOT in the
   object.
1a. **GC substate outlives the value-holding substates.** `~Runtime` destroys
   substates in **reverse slot order** so the GC heaps (`kSlotInterpGc`,
   `kSlotJitGc` — the lowest slots) are torn down LAST. Forward order frees the
   GC first, and any later substate destructor that releases a JitValue (module
   / namespace tables, test registry, defer stack) would then call `forget()`
   into a freed heap — a use-after-free. Each slot is nulled after deletion so a
   release that still resolves a substate mid-teardown revives an empty one.
2. **Soundness (the safety guarantee):** a collection NEVER frees an
   object reachable from a root or another live object. This must hold
   unconditionally.
3. **Completeness is best-effort, NOT exact.** A *conservative* collect
   may over-retain an unreachable object when a stale stack/register
   word looks like a pointer to it. This is safe (a bounded, rare leak
   on 64-bit) and expected. Consequence: code and tests may assume
   "reachable survives", never an exact reclaimed set. The deterministic
   `collect_precise(roots)` entry (explicit roots, no stack scan) exists
   for tests that need exact mark+sweep behaviour.
4. Sweep runs each dead object's C++ destructor exactly once.
5. (Phase 2) Every old→young heap store is recorded by the write
   barrier before the next minor collect.
6. The suite is green under `CULEBRA_GC_STRESS=1`.

### Validated in isolation (Phase 0, `include/jit_gc.h` + tests)

- `is_object` pointer validation (live / interior / random / freed).
- Conservative root scan finds stack + global roots at **-O0…-O3**.
  Root finding must start at the **stack pointer**, not
  `__builtin_frame_address(0)` (the frame pointer) — locals/spills sit
  below FP and would be missed (this failed at -O2 until fixed).
- Mark-sweep: `collect_precise` reclaims the exact unreachable set;
  conservative `collect` is sound (all reachable survive).
