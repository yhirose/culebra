# JIT GC Rewrite — Design Spec (Conservative, Generational, Non-Moving)

Status: **Design / not yet implemented.** Supersedes the current JIT
reference-counting + minor-only generational collector.

This document is the authoritative spec for the rewrite. It is written
to be implementable phase by phase, prioritising **safety and code
simplicity first**, performance later.

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

The minor-only generational collector then **amplifies every leak into
a permanent one**: it treats a non-zero refcount as a live root,
promotes such objects to the old generation, and never re-scans old.
So a single missed release leaks forever (microgpt JIT grew to ~5 GB).

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

Replace the JIT's manual RC + minor-only collector with a
**conservative, generational, non-moving mark-sweep tracing GC**
(the Ruby MRI / Boehm model).

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
- **Phase 2+ — page/bitmap heap (perf).** Replace the registry with
  size-segregated **pages** + a per-page **allocation bitmap** (object
  starts, for O(1) `is_gc_object`) and a **mark bitmap**. Free lists per
  size class; allocation refills from fresh pages. This only swaps the
  `gc_alloc`/`is_gc_object`/iteration internals — mark/sweep/roots are
  unchanged.

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

## 7. Removing manual RC from codegen

- Delete all `emit_value_retain` / `emit_value_release` emission and the
  `culebra_runtime_value_retain/release/swap_owned` helpers.
- Delete `_GcTracker` (refcount-based reachability, promote-and-forget).
- Delete `_culebra_invoke_method0/1`'s retain/release, the
  `compile_method_call` uniform release, the `dispatch_arr_iter`
  retain, `release_scope_slots`, the loop-body releases — **all the
  ownership bookkeeping I've been patching disappears.** Codegen just
  allocates and uses values.
- `array_push` etc. no longer "steal" a +1 — there is no +1. They store
  the `{tag,data}`; the pushed object stays alive because it is now
  reachable from the array (and from the stack until then).

This is the bulk of the simplification the rewrite buys.

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

- **Interp** is untouched (`shared_ptr`). Objects that cross the
  interp/JIT boundary (Tensor `impl` is a `shared_ptr<TensorImpl>`)
  keep their existing handle: the JIT `JitTensor` struct is GC-managed,
  its `impl` shared_ptr is released by `~JitTensor` at sweep. No moving,
  so raw pointers handed to C++ stay valid.
- **Isolates / threads** (concurrency roadmap): each thread registers
  its stack base; collection is stop-the-world across the isolate's
  threads at safepoints (allocation points + loop back-edges). Per-isolate
  heaps mean most collections are thread-local. Conservative scan needs
  no per-thread shadow stack — just each thread's stack bounds.

## 12. Future migration to precise / moving (Option C)

When compaction or copying-nursery allocation throughput is shown by
measurement to be needed, migrate roots from conservative scan to
**LLVM Statepoints** (`gc.statepoint`/`gc.relocate`, the modern LLVM GC
path that superseded `gcroot`). That enables relocation → compaction +
bump-allocated copying young gen. The object model (§3), marking (§6),
generational structure (§8), and safety devices (§9) are designed to
**carry over**; only root finding (§5) and the moving/compaction of the
heap change. Keeping the heap non-moving now is what makes that a
localised future change rather than a second rewrite.

## 13. Phasing

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
  - Wired behind a CMake flag (`CULEBRA_NEW_GC`), default OFF, compiled
    alongside the current collector (compile-time exclusive); structs use
    `GcHeader` and `new JitX()`→`gc_alloc`, with retain/release as no-ops
    (allocate-only). CI builds both.

  Phase 0 ships nothing user-visible; it de-risks Phase 1.
- **Phase 1 — conservative full mark-sweep.** Conservative root scan +
  mark (reuse `enumerate_children`) + sweep (struct dtors). **Delete all
  manual RC** (§7). Acceptance: full suite green under GC stress; the
  leak regression tests pass on JIT; microgpt JIT RSS flat across steps.
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

1. No `emit_value_retain` / `emit_value_release` in codegen (Phase 1+).
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
