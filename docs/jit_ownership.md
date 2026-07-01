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

The trap (learned the hard way — two reverted attempts on the lazy-combinator
leak, see [[project_jit_gc_rewrite]] Task #4): while a runtime helper is
*constructing* a value (e.g. `iter_map` building an iterator: `cell_new`,
`closure_new`, each of which allocates and can trigger a collect), the
freshly-built cells and the upstream are kept alive **only by their refcount**.
The collect does **not** scan the C++ helper's native stack frame — it scans
the JIT shadow-stack / registered roots. So a refcount that *looks* redundant
for ownership can be **load-bearing for rooting**: drop it and a
construction-time collect frees an un-rooted object → SIGSEGV, reproducible
only under heap pressure (passes isolated probes and ASan, fails the full
corpus).

**Design consequence:** the two concerns must be *separated*, not both carried
by "an extra retain":

- **Rooting** is owned by the GC layer: the precise-GC shadow-stack roots
  (`jit_gc_design.md` §12, [[project_jit_gc_rewrite]] Phase 1), plus rooting of
  in-flight temporaries created inside runtime helpers. Once liveness across a
  safepoint is guaranteed independently, an ownership handle is *purely* about
  "who calls release."
- **Ownership** is then free to be minimized/normalized (a redundant release
  can be removed) without any risk of premature free.

Any ownership scheme that ignores this will keep hitting the Task #4 wall.

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

### 4.4 Borrows can't be released

A borrowed value is only ever handled through `.borrow()` (or a distinct
`Borrowed` view). There is no API that both borrows and releases, so
"releasing a borrowed operand" (a double-free source) becomes unrepresentable.

### 4.5 Rooting is the GC layer's job, not the refcount's

Per §2: liveness across a collect is guaranteed by the precise-GC shadow-stack
(named/temporary slots) plus rooting of in-flight temporaries inside runtime
helpers (e.g. pin the cells/upstream while `iter_map` builds them, or register
them on the shadow-stack before the first allocation that can collect). With
rooting decoupled, `Owned`/slot ownership is purely about release timing, so
minimizing refcounts never risks a premature free.

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

- **Done:** the `Owned` handle exists and is proven zero-behavior-change on the
  operator slice (binary/comparison/unary operators), with the GC/leak/difftest
  gates green ([[project_jit_gc_rewrite]]). Deterministic-drop RC and the
  mark-sweep backstop are shipped (`jit_gc_design.md`).
- **The through-line work:** progressively route every `compile()` producer and
  consumer through `Owned` + scope slots, deleting the hand-placed
  retain/release as each site is converted — until §5's invariants hold
  codebase-wide and the bare calls are gone. This is the "flip `compile()` to
  `Owned` returns" refactor: large but *type-driven* (the double-consume assert
  and the move-only handle guide each conversion; a missed release becomes a
  dropped `Owned`, not a leak).
- **Unblocks in order:** (1) close the rooting/ownership split (§2/§4.5) so
  removing redundant refs is safe → (2) finish the ownership flip → (3) the RC
  is leak-free → (4) `gc_refs` / precise GC can retire the conservative
  backstop's over-retention → scalar speedup.

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
