# JIT Ownership — Making Leaks Structurally Impossible

Status: **Design / north star.** This document is the authoritative statement
of *how* the JIT manages the lifetime of heap values, and the standing rule for
all future work in this area. It is the ownership counterpart to
[`jit_gc_design.md`](jit_gc_design.md) (which covers the tracing backstop and
the object/heap model).

---

## 0. The rule (non-negotiable)

**Leaks and double-frees must be made structurally impossible — prevented by
construction, the way RAII (C++) and ownership/`Drop` (Rust) prevent them — not
patched case by case.**

- **No ad-hoc leak fixes.** Hunting one leaking codegen site at a time is
  explicitly *not* the strategy. Every such fix we have shipped bought a real
  improvement but left the underlying hazard (the next un-audited path) in
  place. The manual-RC design is "a nest of leaks"; we remove the nest, not
  the eggs.
- **Elegant and simple over clever.** Prefer one small, correct abstraction
  that the whole codegen threads values through, over scattered special cases.
- **No exceptions.** New codegen that produces or consumes a heap value uses
  the ownership layer. A bare `emit_value_retain` / `emit_value_release` in a
  new code path is a design smell and should be justified in review or, better,
  refactored away.

Why this is worth a structural investment: the leak class is the last thing
keeping the fast reclaim paths (deterministic RC drop, and eventually
`gc_refs` / precise GC) from being load-bearing. A leak-free RC is the
prerequisite for retiring the conservative backstop's over-retention, which is
the dominant scalar-microgpt slowdown (see `jit_gc_design.md` §1 and
[[project_jit_gc_rewrite]]).

---

## 1. Scope of the problem

Culebra has two execution backends, and **only the JIT has this problem**:

- **Tree interpreter** — values are `shared_ptr`. Refcounting is automatic and
  exact; leaks and double-frees cannot occur by construction. This is our
  proof that the *semantics* are fine; the JIT just lacks the mechanism.
- **JIT (LLVM) / AOT** — codegen emits `retain` / `release` calls by hand at
  every ownership boundary (call args, method receivers, operator operands,
  loop-body values, scope exit, exception edges, …). Getting this exactly
  right on every control-flow path — normal fall-through, `break`, `continue`,
  early `return`, and exception unwinding — is where every leak and every
  double-free has come from.

The goal is to give the JIT an *automatic and exact* ownership discipline of
its own, matching the interpreter's guarantees without `shared_ptr`.

Heap value kinds the discipline must cover (tagged `i64`, `data` = `ptrtoint`):
`Object`, `Array`/`Tuple`, `Func` (closure), `Cell`, `Set`, `Tensor`, and the
inline-length `String`. Scalars (`Long`/`Float`/`Bool`/`Nil`) are never
refcounted; every op is tag-aware and no-ops on them.

---

## 2. The one insight that must shape the design: ownership ≠ rooting

A refcount in today's JIT silently does **two independent jobs**, and
conflating them is what makes naive "remove the redundant retain" fixes crash:

1. **Ownership / release** — decides *when* an object is freed
   (release-to-zero, which also fires deterministic `drop`).
2. **Rooting / liveness** — decides *what stays alive across a GC collect*.

Both jobs matter, but they are guaranteed by **different layers**, and a
"remove the redundant retain" fix must be judged against the right one.

**Rooting is already provided independently of any single retain — by both
shipping collectors** (verified 2026-07-02, see below):

- The **conservative** collector scans the *whole machine stack* — every JIT
  frame *and* every C++ runtime-helper frame — plus setjmp-flushed
  callee-saved registers (`jit_gc.h scan_roots`). A helper's in-flight locals
  (cells, upstream, half-built closures) are on that stack, so a
  construction-time collect roots them regardless of their refcount.
- The **gc_refs** collector (the endgame) seeds roots from refcounts: any
  object whose refcount exceeds its in-heap references is a root, and marking
  is transitive from there. A native frame's `+1` *is* an external ref, so an
  in-flight temporary is a root by construction — provided the RC accounting
  is correct, which is exactly what this document's ownership discipline
  establishes. Rooting soundness under gc_refs ≡ RC accuracy.

The rooting gap is real only for the *parked* precise-shadow-stack route
(`jit_gc_design.md` §12 Phase 2 wall): a shadow stack does not see native
helper frames or values still in registers. If that route is ever revived, the
in-flight pinning problem revives with it; nothing shipping depends on it.

**The Task #4 trap, re-diagnosed:** the two reverted attempts on the
lazy-combinator leak ([[project_jit_gc_rewrite]] Task #4) were originally
blamed on a rooting gap. Re-running both reverted patches with
`CULEBRA_GC_NEVER=1` (a diagnostic that disables *every* collect) still
crashes `test_iter.cul` 8/8 — with no collector running at all. Both crashes
are therefore **pure RC over-release** (a release-to-zero while a reference
is still held), not GC sweeps. The discriminator is now permanent tooling:
a crash that survives `CULEBRA_GC_NEVER=1` is an ownership bug; one that
disappears is a rooting bug.

**Design consequence:** a retain that *looks* redundant may still be
load-bearing — but for *ownership at another call site* (a caller that hands
no `+1`, a consumer that releases unconditionally), never for rooting under
the shipping collectors. So the precondition for removing one is an
accounting proof — a refcount trace of the object's whole economy plus the
full gate — not new rooting machinery.

---

## 3. Prior art, and what each contributes

| System | Mechanism | What we take | Why it doesn't fully fit |
|---|---|---|---|
| **Rust** | affine ownership + borrow checker, `Drop` at scope end | *move-or-drop* as the value discipline; borrows never free | no borrow checker in codegen; cycles still need a backstop (same as `Rc`) |
| **C++ RAII** | destructor emits cleanup, follows host control flow | the *implementation vehicle*: a C++ handle whose dtor emits `release` IR, correct across codegen's own early-returns/exceptions | only naturally covers straight-line temporaries |
| **Swift ARC** | compiler *inserts* retain/release from a uniform convention, then a peephole cancels redundant pairs | **don't hand-place — derive from convention, then optimize** | ObjC-interop conventions are heavier than we need |
| **Perceus (Koka)** | compiler-inserted *precise* RC with drop specialization + in-place reuse | closest match: precise, deterministic drop, reuse; pairs with a cycle collector | designed for a functional IR; we lower a dynamic AST |
| **Nim ORC** | RC + cycle collector (Bacon-Rajan) | validates "RC + cycle backstop" as a shipping combo (our chosen model) | trial-deletion cycle cost; we already have a mark-sweep |
| **Lobster** | compile-time lifetime analysis elides most RC ops in a *dynamic-ish* language | proof that a dynamic language can elide RC statically | relies on whole-program type inference we don't have |
| **MLIR / Swift SIL ownership** | ownership is *in the IR*; a verifier + a dedicated pass insert/So check cleanup | end state: ownership as a checked IR property, not convention-by-comment | a full ownership SSA is a large build |
| **Regions / arenas** (MLKit, Zig) | bump-allocate per scope, free the region at scope end | matches scope-bounded temporaries with zero per-object RC | escaping/shared values still need RC |

The synthesis the field converged on — and the one that fits Culebra — is
**Swift/Perceus-style: the compiler owns RC placement from a uniform
convention; the programmer never hand-writes retain/release.** Culebra realizes
that with C++ RAII handles over LLVM IR, reusing the scope machinery it already
has.

---

## 4. The Culebra design

A layered discipline. Each layer removes one way to leak; together they make a
leak require *breaking a type/RAII invariant*, which is a compile error in the
C++ codegen, not a silent runtime leak.

### 4.1 A uniform ownership convention (the contract)

Single-source, documented, and true today (verified by the Phase-0 fresh-source
ownership probes, [[project_jit_gc_rewrite]]):

- **`compile(expr)` returns a `+1`-owned value.** Every expression node.
- **Parameters / receivers are borrowed (`+0`).** The callee frame consumes the
  one ref the *caller* provides (the caller retains before the call); a native
  callee with no frame consumes it explicitly. `this`/args follow this.
- **Slots own.** A value stored in a scope slot is owned by that slot.
- **Cells do not retain on store.** `cell_new(tag,data)` stores raw; the *caller*
  must own the ref it hands in, and cell-release releases the stored value.
  (This is exactly why the lazy-combinator "internal retain" is load-bearing —
  it supplies the cell's owned ref.)

The convention is the spec. Everything below is machinery that *enforces* it.

### 4.2 `Owned` — the only handle for a transient `+1` value

A C++ RAII handle (already in `jit.h`, piloted on operators — see
[[project_jit_gc_rewrite]] "B (Owned RAII) pilot"):

- `Owned { JIT*, llvm::Value*, bool consumed }` — non-copyable, move-only.
- `.borrow()` — read tag/data **without** consuming (for borrowed args).
- `.consume()` — hand the `+1` onward (into a slot, a call, a return); marks
  spent. Double-consume is a **codegen-time `assert`/abort** — a bug becomes a
  build failure, not a double-free.
- destructor — if still owned, emit `emit_value_release` (the RAII `Drop`).
- move-assign releases the old value first.

Straight-line temporaries held only as `Owned` are **leak- and
double-free-proof by the C++ type system**: they are either consumed exactly
once or released exactly once, on whatever path the codegen takes (including
codegen's own early returns and exceptions). `compile_power`'s `**1` fast path
is the canonical demonstration: `base` is `consume()`d on one arm and dropped
on the others, and control-flow-correct release falls out automatically.

### 4.3 Escaping values are owned by exactly one scope slot

A value that must outlive the current straight-line region (bound to a name,
captured, kept across a loop/branch) is `.consume()`d into a **scope slot**
(`make_stack_slot` + `define_var`). The existing scope-unwind machinery then
releases it **on every exit path**:

- normal fall-through / block end → `pop_scope`
- `break` / `continue` → the loop's cleanup edge
- early `return` → `release_all_scopes_for_exit`
- exception → the fn-level / lexical / match-arm landingpads

This is *one* mechanism for "escape," reused everywhere, instead of a bespoke
release at each site. Loops get a fresh scope per iteration (already true), so
per-iteration temporaries drop each turn. Iterators/generators that carry a
`dispose` register it with the same scope machinery so early exits run it — the
gap here is the current for-in raw-alloca iterator ([[project_jit_gc_rewrite]]
Task #2), which is a *missing* scope registration, exactly the kind of hole
this layer closes structurally.

**Mechanism note — the two exit families are cleaned up by different code, and
a fix must cover both** (learned by a reverted first attempt at container-literal
exception-safety):

- **Compile-time-emitted exits** (normal fall-through, `break`/`continue`,
  early `return`) run `release_scope_slots` / `release_all_scopes_for_exit`,
  which *statically emit* the releases for the scopes known at that point. A
  fresh `make_stack_slot` + `define_var` is covered here for free.
- **Runtime exception unwind** does not fall through those compile-time
  emitters — it lands in a cleanup landingpad. Historically that landingpad ran
  only `defer_run_to(mark)`, so a throw skipped `release_scope_slots` and the
  owned-region drop entirely: a scope's owned locals leaked and their `drop()`
  never fired on `throw` (only on fall-through / `return`). **This is now
  closed** — see "Generalized scope cleanup" below.

Consequence for this layer: to make a construction region exception-safe, the
in-flight value's release must ride the exception edge, not a scope slot (which
only covers the normal/return exits — a `make_stack_slot`-only "fix" silently
leaks on the throw path; verified: `[mkheap(), boom(), mkheap()]` still leaked
~2/loop with the slot in place).

**Implemented mechanism — per-region cleanup landingpad, gated on can-throw**
(shipped for container literals; `compile_array` / `_object` / `_tuple` / `_set`):

- Create an empty `cleanupBB` and set it as `current_lpad_` only around the
  element/key/value compilation. A sub-expression that can throw compiles to an
  `invoke` whose unwind edge targets `cleanupBB`; a non-throwing one (a plain
  identifier / literal) compiles to a plain call and adds no edge.
- After construction, `finish_construction_cleanup`: if `cleanupBB` has no
  predecessors (nothing could throw), **erase it** — zero happy-path overhead,
  which is why `[this, o]` / `[1.0, 1.0]` pay nothing. Otherwise fill it with
  "release the partial value(s), re-raise to the outer landingpad."
- The partial value is passed to the cleanup through an **entry alloca**
  (`make_build_guard`), loaded in the landingpad — never as a call-result SSA
  live across the invoke edges (that form crashes SDAG-O0's register
  coalescer). `make_pending_guard` / `clear_pending_guard` cover the in-flight
  temporaries not yet consumed across a risky call (a heap object key before
  its value; a spread source before extend/merge); each guard is nil outside
  its window, and releasing nil is a no-op.
- A landingpad requires the function to carry a personality (a leaf function
  otherwise has none — a null-personality codegen crash); set it idempotently
  when cleanup is actually emitted.

This is preferred over registering releases on the **defer stack**: the defer
stack stores *closures* to invoke (heavyweight per literal) and would run on
normal exits too (needing a cancel), whereas the cleanup landingpad is
exception-path-only and dead-code-eliminates to nothing when unneeded. The
same landingpad shape also serves the for-in protocol loop; its raw-alloca
iterator on early `return` is covered too (compile_return walks
`iter_cleanup_stack_` and emits the same dispose+release, innermost first).

**Generalized scope cleanup — `finish_scope_cleanup` (shipped).** The same
per-region, pred_empty-gated landingpad now backs *every* scope-like region, so
a `throw` releases owned slots and fires `drop()` exactly as fall-through does.
On the exception edge it mirrors the region's normal exit — run the region's
defers (`defer_run_to`), then `release_scope_slots` (which fires `drop` via
refcount-0 for every non-escaped resource) — then re-raises to the enclosing
landingpad. It must run while the scope is still the live top-of-stack (its
slot allocas are entry-zero-initialised, so a throw before a binding is
assigned releases nil). Wired into: lexical scopes, `while` / `for` bodies,
`match` arms **and the match subject scope**, `try` bodies (released before the
catch handler runs), **the `for` statement's own iterable scope** (which also
holds the Set branch's `set_to_array` temp — before this wiring a throw
unwinding out of a for-in stranded the iterable's slot ref: no `drop`, and a
real per-call leak whenever the frame wasn't re-entered, since only a
re-execution's declaration store would release the previous strand), and the
function frame — the fn-level pad is now emitted
whenever the body can throw (not only with a fn-level `defer`), which covers a
function whose owned locals live in the *frame* scope, since the body `BLOCK`
compiles into that frame, not a nested `LEXICAL_SCOPE`. The escaped/cyclic drop
remainder is resolved once at the frame level
(`release_all_scopes_for_exit`'s `owned_scope_exit` over the whole frame mark),
not per scope, so each cold cleanup block stays minimal. Normal-exit scope
teardown keeps `current_lpad_` null so its `owned_scope_exit` is a plain call
and never becomes a predecessor of a cleanup pad (which would defeat the DCE).
The order matches the interpreter's unwind (defers before resource release), so
`defer`/`drop` interleave identically on the throw path. Cost: functions with
may-throw calls now emit `invoke` (unwind edge) instead of plain `call`, a
small one-time compile-time increase; per-step execution is unchanged.

### 4.4 Borrows can't be released

A borrowed value is only ever handled through `.borrow()` (or a distinct
`Borrowed` view). There is no API that both borrows and releases, so
"releasing a borrowed operand" (a double-free source) becomes unrepresentable.

### 4.5 Rooting is the GC layer's job, not the refcount's

Per §2: liveness across a collect is already guaranteed by the collector
itself — the conservative scan covers every native frame, and gc_refs roots
anything with an external refcount. No scoped-pin / shadow-stack machinery
for in-flight helper temporaries is needed (and none may be added for this
purpose; the shadow-stack codegen was removed once already as dead overhead).
With rooting independently guaranteed, `Owned`/slot ownership is purely about
release timing — and the safety condition for minimizing refcounts is RC
accounting accuracy, provable per change with a refcount trace,
`CULEBRA_GC_NEVER` discrimination, and the full gate.

### 4.6 Cycles are swept, not counted

Reference cycles are out of scope for any RC discipline (Rust has the same
`Rc`-cycle gap). The mark-sweep backstop (or `gc_refs`) reclaims cycles and any
residue. With ownership single-sourced and correct, the backstop becomes
**non-load-bearing** (rare, not per-step), which is the whole performance point.

---

## 5. Invariants that make leaks impossible

If these hold, a leak requires breaking a C++ type/RAII invariant (a build-time
failure), not a runtime accident:

1. Every `+1` transient value is held in an `Owned` (or immediately consumed).
2. `Owned` is consumed exactly once **or** dropped exactly once — never both,
   never neither (enforced by move-only + dtor + double-consume assert).
3. Every escaping value is consumed into exactly one scope slot; scope unwind
   releases every slot on every exit path (normal/break/continue/return/throw).
4. Borrowed values are never released (no API allows it).
5. Rooting is provided by the GC layer independently of ownership refcounts.
6. Cycles + residue are reclaimed by the backstop; the backstop is not relied
   on for steady-state memory.

Corollary: no correct codegen path contains a bare, hand-placed
`retain`/`release`. Existing bare calls are migration debt, not the pattern.

---

## 6. State and migration path

- **Done:** the `Owned` handle exists and is proven zero-behavior-change
  (byte-identical IR, or leak-fix-only IR deltas) on six slices, with the GC/leak/difftest gates green
  ([[project_jit_gc_rewrite]]):
  - **operators** (binary/comparison/unary) — the pilot;
  - **call arguments** — every ARG_LIST producer (positional, static-kwargs
    resolver, runtime-kwargs buckets) holds its `+1`s as `Owned`;
    `consume_all(vector<Owned>&&)` is the single handoff into the raw call
    emitters, whose raws may be referenced from mutually-exclusive dispatch
    branches (method/UFCS, call/`__call__`) — one consume per runtime path;
  - **method receiver** — `compile_method_call` takes `Owned receiver`;
    consuming children get explicit `.consume()` handoffs, borrowing arms
    (explicit `drop`, builtin methods) rely on the handle dtor, and the
    ufcs-builtin hook contract is explicit (non-null result = hook consumed
    the `+1`);
  - **postfix chain** — `compile_call` / `compile_call_with_builtins` roll
    the chain value (callee → property view → index result → call result)
    through one `Owned` handle: call dispatch borrows and the move-assign
    of the fresh result releases the previous link; INDEX consumes into
    `emit_index_step`; the property-get `swap_owned` arm marks the handle
    consumed (the runtime primitive trades the receiver's `+1` for the
    view's); `try_fuse_iter_map_collect` follows the hook contract
    (declines on AST shape alone, a hit consumed the `+1`);
  - **container-literal elements** — array/tuple/set/object literals hold
    every element, spread source, key, and value as `Owned`;
    slot-absorbing runtime calls are explicit `.consume()` points, spread
    sources scope their handle so the dtor lands where the hand-placed
    release was, and the exception-path pending/build-guard machinery is
    untouched (the Owned layer covers only the normal path). This slice's
    receiver-map pass caught a real SIGSEGV: `[](n, heapDefault)` filled n
    slots with unretained aliases of one `+1` (fixed: `array_resize`
    retains per slot; the default is borrowed);
  - **decorator callees** — the three application sites (fn/class/enum
    declarations) hold the compiled decorator as `Owned`; the raw call
    borrows it and the dtor releases it (a let-bound closure decorator
    leaked one callee ref per application before). The ufcs-builtin hook's
    throw arms were audited in the same pass: every arm now consumes the
    receiver/arg `+1` before raising (release-before-throw is safe there —
    `value_to_long`'s error path never derefs `data`);
  - **iter-protocol dispose/iterable** — the protocol's apparent balance
    was a compensating pair: the proto branch orphaned the iterable's slot
    ref while the dispose call double-consumed the iterator (frame consume
    + skipBB release), and the two cancelled exactly only for
    self-returning iterators (`iter()` = `this`, i.e. generators). For a
    distinct iterator the pair broke: the iterable's `drop` never fired
    under the JIT and the skipBB release landed on an already-freed
    iterator — a real SIGSEGV at scale, not just a leak. Fixed as one
    change, both sides at once: the dispose call retains the iterator
    before handing it to the frame (the iter()-call convention), and the
    proto branch keeps the iterable's `+1` in the loop's scope slot so
    pop_scope releases it exactly once, like the range/keys branches.
    Drop/dispose timing is interp-symmetric for both iterator shapes
    (pinned by `tests/test_iter_dispose_ownership.cul` and battery
    patterns `forin_proto_iterable` / `forin_generator` — the former
    crashes the pre-fix binary).
  Mapping the receiver flows also surfaced five real receiver `+1` leaks
  (auto-`parameters()`, Set `.add`/`.remove` fast dispatch, fused
  `map+collect`, inlined-lambda `for_each`/`reduce` over iterators) — fixed
  and pinned by leak-battery patterns. Deterministic-drop RC and the
  mark-sweep backstop are shipped (`jit_gc_design.md`).
- **The flip itself is done: `compile()` returns `Owned`.** Every producer
  now hands its `+1` through the move-only handle, so any *new* call site
  must make an explicit ownership choice — consume, borrow, or drop — and a
  double hand-off asserts at codegen time. The conversion was
  behaviour-preserving by construction: previously-converted sites
  (`own(compile(…))`) now take the handle directly, and every remaining
  legacy site consumes it at the call (`compile(…).consume()` — exactly the
  old raw `+1` contract), verified byte-identical IR at -O2/-O0 plus the
  full gates. The `.consume()` calls are the grep-able inventory of what's
  left to convert.
- **The through-line work:** convert those `.consume()` clusters to real
  `Owned` flow (dtor releases, borrows) cluster by cluster, deleting the
  hand-placed retain/release as each converts — until §5's invariants hold
  codebase-wide. Notes for those slices: a probe sweep found **no
  normal-path leaks** in the remaining clusters (statement discard /
  `cond` / guards / interpolation / `??` are flat; `if`/`while`/logical
  conditions only accept Bool/Long/Float, so a heap `+1` can't reach them
  on the normal path — only their *throw* edges carry heap refs, and those
  are the per-region cleanup pads' job, not `Owned`'s, §4.3). Beware
  release-position changes: e.g. `compile_statements` frees the previous
  statement's result *before* compiling the next one — a naive move-assign
  rewrite would emit the release after it, shifting drop timing observed by
  the interpreter. Also remaining: the method-dispatch children's internals
  (`compile_user_method_over_builtin` / set-mutate / method-or-ufcs, which
  still share consumed raws across their runtime-exclusive arms).
- **Unblocks in order:** (1) the rooting/ownership split (§2/§4.5) is closed
  by evidence — both shipping collectors root in-flight temporaries
  independently, so removing a redundant ref needs an accounting proof, not
  rooting machinery → (2) finish the ownership flip → (3) the RC is leak-free
  → (4) `gc_refs` can retire the conservative backstop's over-retention →
  scalar speedup.

Every phase keeps the hard gates: interp/JIT symmetry, difftest corpus,
`CULEBRA_GC_STRESS`, ASan/UBSan, **and the full `just test` gate** (leak fixes
must run the full gate — isolated probes/ASan can pass while the accumulated
corpus crashes; and `bin ... | tail` masks a crash exit code — check with
`>/dev/null 2>&1; echo $?`).

---

## 7. Constraints specific to Culebra

- **Tagged `i64` values, not pointers.** Root enumeration reads the tag to
  decide if `data` is a heap pointer. No value-ABI rewrite (statepoints were
  rejected for this reason — `jit_gc_design.md` §12).
- **Two backends must stay symmetric.** Any ownership change is JIT-internal and
  must not alter observable behaviour, messages, or check timing/order vs the
  interpreter (the standing symmetry requirement, [[feedback_check_jit_interp_symmetry]]).
- **Dynamic dispatch.** Method/operator targets are resolved at runtime, so
  ownership conventions are enforced dynamically at the call boundary, not by a
  static type of the callee.
- **Elegance bar.** Macros/helpers used sparingly ([[feedback_macros_sparingly]]);
  the abstraction should read like the surrounding codegen, not bolt a framework
  onto it.
