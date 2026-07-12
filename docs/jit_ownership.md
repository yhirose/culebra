# JIT Ownership — Making Leaks Structurally Impossible

Status: **Design / north star — and, as of 2026-07, shipped.** Every layer in
§4 is implemented and gated (§4.8 unwind-temp window, §4.9 block-pinned raws +
typed `compile_*` seam, the rc-discipline ratchets). This document is the
authoritative statement of *how* the JIT manages the lifetime of heap values,
and the standing rule for all future work in this area. It is the ownership counterpart to
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
- `.drop()` — the explicit-release twin of `.consume()`: emit the release
  **now**, at the current insertion point, and mark spent. For a value that is
  simply dead rather than handed on, and whose release must land before later
  IR (a `make_*` that emits, a branch arm, an out-param load) where the
  scope-exit dtor would place it too late. A default-constructed (empty)
  `Owned` drops to a no-op, so it cleanly models an optional arg.
- destructor — if still owned, emit `emit_value_release` (the RAII `Drop`).
- move-assign releases the old value first.

A `.drop()`/dtor releases at the builder's *current* point, so `Owned` covers
**straight-line temporaries only**. A value whose lifetime is one loop
*iteration* inside an emitted loop is not straight-line; keep such a release
bare (or scope an `Owned` to the loop-body region) — see §6.

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
release at each site. **A same-name rebinding within one scope releases the
prior slot's owned `+1` before overwriting it** (`define_var` →
`release_slot_value`): the LIFO teardown list records only the first slot for
a name, so a silent overwrite would orphan the earlier binding. The canonical
trigger was the implicit `self`/`this` slot (bound unconditionally for
recursion / the receiver) shadowed by a same-named parameter — which the lint
pass now also rejects pre-eval (`self`/`this` are reserved parameter names),
so the two fixes are belt-and-suspenders: the reject stops the source, the
release keeps *any* rebinding leak-free by construction. The same slot
machinery also carries **branch-spanning operands** as an anonymous region
scope: a 3+ comparison chain (`a < b < c`) lowers across short-circuit blocks,
so each operand is consumed into an owned `cmpchain.N` slot of a scope pushed
around the chain (the `compile_match` subject pattern) — entry-nil-initialised
slots mean the shared merge releases exactly what the taken path materialised,
and the region's cleanup pad covers the throw edges. The two slots entering a
comparison are blanked for exactly that call: the comparison helpers own their
operands' throw edges (§4.7), so the region releasing them too would
double-free. (The logical operators need none of this: strict truthiness
rejects heap conditions with the operand released by `to_bool`, and `??` never
overwrites a heap candidate — a nil is the only thing replaced.) Loops get a
fresh scope per iteration (already true), so per-iteration temporaries drop
each turn. Iterators/generators that carry a
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

### 4.7 Helper ownership contracts — the callee-cleans table

Every codegen-owned `+1` that is live across a may-throw runtime call has
exactly one cleaner on the unwind edge, drawn from this closed set of
contracts. This table is the (for now human-readable) input the GAP4-ENFORCE
accounting pass will check against: an emitter holding a `+1` across an
`invoke` must be able to name which row covers it.

| Contract | Mechanism | Who uses it |
|---|---|---|
| **Caller-cleans (codegen)** | `ThrowGuard` — a per-region cleanup pad packaged as RAII; pred-empty DCE keeps the happy path free | builtin-method receivers and operand args; the callee closure in `compile_call`; the assignment lvalue across the final-assignment switch; the UFCS free-function callee; sort_by's `reverse:` coercion window (the callback's `+1` has no owner until the sorter is entered) |
| **Callee-cleans-on-direct-throw** | `JitUnwindRelease` in the helper — releases IFF the scope unwinds; armed only after user dispatch has declined | operator entries (arith / ordering / `==` coercion / `to_bool` / neg / matmul); `object_get_any` (receiver under `own_receiver`, plus a refcounted key); `culebra_runtime_prop_get` (under `own_receiver`); `_culebra_expect_callback` (reject throw, all tags); the not-a-function edge in `compile_function_call_raw` (flag-gated per call site: `own_this_on_error` / `own_args_on_error`); the receiver-resolution error block in `dispatch_arr_iter` (releases the values already consume()d for the HOF runtime, which never runs on that edge) |
| **Callee-consumes-on-every-exit** | `JitOwnedVal` / `JitMethodSelf` / `JitMethodArgs` declared at entry — releases on normal return AND unwind | native method endpoints (sendable channel/isolate/shared-val/buffer methods, FixedArray methods, `@wrap` foreign thunks); `@packable` stores; HOF helper accumulators (`out`/`acc`/computed sort keys); **the HOF callback itself** (`JitHofCallback` for eager drivers, the lazy factories' capture cell) |
| **Invoker-cleans** | `JitUnwindRelease` in `_culebra_invoke_method*` — the invoker retains the operands, the callee frame releases them on a NORMAL return, the invoker's guard releases them iff the callee throws | every user-dispatch window (`__op__` / `eq` / `hash` / `cmp` / `__index__` / getter bodies). Corollary: the derived-method thunks (`_jit_derived_thunk_consume`) release on the normal path ONLY — releasing on throw too would double-free once the invoker unwinds |
| **Transfer** | return the incoming `+1` as the result (or hand it into a capture cell / slot) | iter-self methods; `_culebra_capture_callback` into a lazy combinator's cell; `emit_point_index`'s uniformly-`+1` result |

The gate rules that fall out of the table:

- The flags `own_receiver` (helper ABI) and `own_this_on_error` /
  `own_args_on_error` (emitter parameters) are the same declaration — "this
  call site owns the value; the helper is the sole releaser on its
  direct-error edge" — expressed at the two layers. A new helper that can
  raise a direct error while handed an owned value must take the same gate,
  not invent a new cleanup shape.
- Two contracts may never overlap on one edge: two cleaners on the same value
  double-free (ASan-confirmed three times: operator operands guarded codegen-
  side over user dispatch; the pre-unification HOF blanket guard; the
  callable adapter's throw-path release once the callback became
  callee-consumes). The mutual exclusion for helpers is always "user dispatch
  declined" — which is why `JitUnwindRelease` is armed after the dispatch
  attempt, never around it — and for consumed values "the owner is entered",
  which is why a codegen guard over a consumed value must close before the
  consuming call.
- One known ordering-sensitive spot: `wrap.h`'s `jit_check_args` releases
  `self` on its binder-throw itself, so the thunk's `JitMethodSelf` is
  declared AFTER the check — declaring it first would double-free that edge.

### 4.8 The automatic unwind-temp window (GAP4-ENFORCE)

§4.2's `Owned` handle releases through a C++ destructor, which a runtime
LLVM-level throw cannot run. Historically that meant **every** "codegen-owned
`+1` live across a may-throw call" was a leak unless someone hand-placed a
guard: a binop's lhs while the rhs compiles (`mk() + boom()`), an argument
while a later argument compiles (`g(mk(), boom())`), an index receiver while
the key compiles (`mk()[boom()]`), a method receiver while its arguments
compile. The leak-fuzzer corpus never spelled these shapes, so C①–⑨ shipped
without them; a six-shape probe confirmed all of them leaking.

Instead of guarding sites one by one (§0 forbids that), the ownership layer
now owns the unwind edge **by construction**:

- **Registry.** Every live `Owned` holding a heap-capable value is registered
  with the JIT (constant scalars are skipped). Registration is pure codegen
  bookkeeping; consume/drop/move maintain it. The registry, the slot pool and
  the coverage stack are per LLVM function (`CompilerStateSaver` swaps them).
- **Window.** `emit_call`, the single point where a may-throw call gets its
  unwind edge, spills every live, uncovered `Owned` into a per-function pool
  slot (an entry alloca, nil-initialised) for exactly the duration of that one
  invoke: store before, nil-clear in the continuation. Coverage is complete by
  construction — the only runtime events that can unwind are the calls
  `emit_call` emits. A call with no live temps pays nothing.
- **Release.** Every scope-family cleanup pad (`finish_scope_cleanup`, the
  fn-level pad) releases the pool first — before the region's defers, matching
  the interpreter, where a throwing expression's temporaries die as its eval
  frames unwind ahead of any enclosing block's defers — then nil-clears each
  slot so outer pads on the same chain no-op. Outside a window every slot is
  nil, so the cold-path cost is a handful of no-op releases.
- **Coverage (`UnwindCovered`).** Where a §4.7 contract already names another
  unwind-edge releaser — an operator helper's direct-error release, the
  invoker guard over user `__op__`/getter dispatch, a `ThrowGuard`'s own pad —
  the value is declared covered for that region and the window skips it.
  Spilling it too would double-free (the §4.7 overlap trap, ASan-confirmed).
  `ThrowGuard` declares its values covered automatically; the hand-written
  declarations sit exactly at the contract call sites (`emit_binop_dispatch`,
  `emit_comparison_i1`, `value_to_bool`, neg/matmul/pow, `prop_get` under
  `own_receiver`, the getter dispatch, the well-known-property check), so the
  §4.7 table now has a machine-visible footprint in the code.
- **Timeline discipline.** `consume()` is the codegen-time marker for "the
  emitted code from here on owns the +1", so it must be called **before**
  emitting code that consumes the value at runtime — not after. The two
  emission hooks that consumed late (`try_fuse_iter_map_collect`, the
  ufcs-builtin hook) let the window spill a value the emitted code had already
  released (an ASan-confirmed teardown UAF); both now consume up front and
  re-own on a declined (no-IR) hook.
- **Kill switch.** `CULEBRA_JIT_NO_UNWIND_TEMPS=1` compiles with the windows
  off — the discriminator (CULEBRA_GC_NEVER's sibling) for deciding whether an
  ASan report is a window double-free or a pre-existing over-release.

This layer subsumed the hand-placed pending-guard stores in the container
literals (a spread source, a heap object key awaiting its value) — their
`Owned` handles are now window-covered, so the hand-placed stores are gone
and `make_pending_guard` survives as the slot primitive (for
`make_build_guard` and for the window's own pool slots). Pinned by
`tools/difftest/leak_abort.sh` Case 4 (the six probe shapes must stay quiet
under the teardown audit).

### 4.9 Block-pinned raws — no bare `+1` crosses a basic block (raw-across-BB ENFORCE)

§4.2–§4.8 protect a `+1` **while it is in a handle**. The last escape hatch
was the moment it left one: `consume()` returned a bare `llvm::Value*` that
codegen could carry across basic-block boundaries (branches, phi merges),
where no layer tracked it — a missed release on one CFG edge was a silent
leak. Every recent product leak (comparison chain, UFCS-kwargs receiver,
compound `obj[k] op=` key, `slice(start, end)` first argument, the fn/return
/try epilogues) was this class. This section closes it.

- **The invariant.** A bare `+1` may only be used in the basic block where it
  was consumed. This is *sound and complete* because `emit_call` turns every
  may-throw call into an `invoke` that terminates the current block (§4.8):
  same block ⟹ no unwind edge and no branch ran while the value was bare.
- **`Pinned`.** `consume()` returns a `Pinned` token recording the pin block
  (constants and null sentinels are exempt — no `+1` to strand). Its
  conversions to `llvm::Value*` check the builder still sits on that block;
  a violation calls `rc_pin_violation` — a loud abort in **every build mode**
  with the codegen source position and pin→use block names. The difftest
  corpus (5641 cases) compiles nearly every construct, so a violating pattern
  is caught the first time it is *compiled* — no runtime leak repro needed.
- **`OwnedPhi`.** The checked merge construct: every `%Value` phi in jit.h is
  built through it (ratchet: hand-built `CreatePHI(valueType_` = 0). Each
  incoming is declared **in its arm block** — `add_incoming(Owned&&)`
  consumes on the spot; a raw incoming must be a constant, produced in the
  current block, or the invoke whose normal dest the builder sits on — and
  `finish(mergeBB)` verifies every recorded arm's terminator still targets
  the merge (an `emit_call` slipped in after `add_incoming` would have
  re-terminated the arm toward its `call.cont`). Emits the identical phi IR;
  the safety is codegen-time bookkeeping.
- **`consume_unchecked()`.** The justified escape hatch, each site carrying a
  one-line rationale and counted by the ratchet: the batch handoff into
  mutually-exclusive dispatch arms (each arm the sole releaser on its runtime
  path — the `consume_all` pattern), a crossing whose `+1` is owned by a
  scope slot (§4.3), and the prologue transfer into `declare_local`.
- **The `compile_*` return seam is typed (closed 2026-07-12).** Every
  `compile_*` helper — the node compilers behind `compile()`'s dispatch, the
  whole call family, the extension compile hooks (`ExtensionHooks`), and the
  stdlib implementations behind them — returns `Owned`; a helper that
  declines returns an empty handle. `compile()`'s old `own(compiled)`
  re-owning boundary is gone: the switch result *is* the `Owned`, so no bare
  `+1` ever crosses a `compile_*` C++ return. The return type now encodes
  the ownership contract — `Owned` = the `+1` transfers, raw `llvm::Value*`
  = borrowed/scalar, which is why the two borrowed-contract emitters were
  renamed out of the family (`emit_property_get` returns `+0`,
  `emit_comparison_i1` returns an `i1`; `emit_interp_fragment` returns a
  C-string pointer). Ratchet: `llvm::Value*`-returning `compile_*` = 0 in
  jit.h + stdlib_jit.h. The only remaining raw form is the explicitly-typed
  `llvm::Value* x = ….consume();` assignment (converts at the assignment —
  also ratcheted to 0).

The Phase-2 flip surfaced and fixed a real-strand family beyond the known
bugs: values held bare across scope teardown / defer runs / a second
argument's compile (fn & return epilogues, try/catch and match-arm results,
the lvalue postfix chain, `obj.get/get_or_put` keys, compound-assign keys,
`slice` bounds, `take_while`'s callback, destructure rvals). Each now rides
an `Owned` so the §4.8 window releases it on the edge that used to strand it.

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
7. Every `Owned` `+1` live across a may-throw call has exactly one releaser on
   the unwind edge: the automatic unwind-temp window (§4.8) by default, or the
   §4.7 contract its call site declares via `UnwindCovered` — never both.
8. A bare `+1` exists only inside the basic block where it was consumed
   (`Pinned`, §4.9); every `%Value` phi is built through `OwnedPhi`; a
   deliberate crossing is a `consume_unchecked` site with a declared per-edge
   releaser, ratcheted by tools/check_rc_discipline.sh.
9. No `compile_*` helper (core or extension hook) returns a bare
   `llvm::Value*` — a `+1` crosses a compile-layer C++ return only inside an
   `Owned` (§4.9, ratcheted to 0). A raw-returning `compile_*` signature is
   the reopened seam, not a style choice.

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
  - **lazy-combinator receiver** — `dispatch_arr_iter`'s obj arm retained
    the receiver, but no arm consumes it (lazy factories make their own
    `+1` internally; terminal drivers only pull values; the receiver's
    `Owned` handle already releases once). It leaked ~9 objects/loop on
    `.iter().filter/flat_map/find/any`. Removing it (borrow, uniform with
    the eager Array arm) unmasked a second, pre-existing bug:
    `_iter_flat_map_fast_fn` released the pulled value *after*
    `_culebra_invoke1` had already consumed it — an identity callback
    `fn(xs){xs}` over heap elements double-frees the inner array (integers
    hid it: releasing a Long is a no-op; the receiver leak hid it further
    by keeping the heap alive). This is exactly the §2 lesson — a
    redundant-looking retain that was masking an over-release elsewhere,
    not a rooting gap (the `CULEBRA_GC_NEVER` diagnostic proved the crash
    is RC over-release with every collect disabled). Both prior Task #4
    attempts crashed here for want of the flat_map fix. Pinned by battery
    patterns `iter_filter_lazy` / `iter_flat_map` / `iter_find_driver` /
    `iter_any_driver`.
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
- **The through-line work (done to its audited residue):** convert those
  `.consume()` clusters to real `Owned` flow (dtor releases, borrows) cluster by cluster,
  deleting the hand-placed retain/release as each converts — until §5's
  invariants hold codebase-wide. **Done so far** (slices A1–A6, all verified
  `--emit-llvm` 0-diff — hand-placed `emit_value_release` in `jit.h` 81→45):
  every `compile_builtin_method` HOF callback (map/filter/for_each/reduce/
  find/any-all/flat_map, and the lazy-iterator factories take_while/chain/zip
  plus the array sorters sort_by/sorted_by — A5) and every builtin-method
  operand arg (tensor pow/reshape/dot/linear_sigmoid; set union/intersect/
  diff/sym_diff/subset/superset; string join/index_of/contains/tr/trim/split/
  split_iter/starts_with/ends_with) now holds its value as `Owned`. Two
  further straight-line sites went the same way (A6): a defer thunk's closure
  (`own(emit_closure_build(...))`) and the array rest-destructure's sliced
  tail (a two-arm consume-or-drop — the sink `..._` arm drops, the named arm
  hands its `+1` to `declare_local`). Two shapes recur: where the release sat
  immediately before `return <value>` the handle just falls off scope (a bare
  `return <value>` emits no IR, so the dtor lands at the same point); where IR
  follows the release (a `make_*`, a branch arm, an out-param load) use
  `Owned::drop()` — release now, mark spent — or a C++ block scope.
  **`Owned::drop()` (§4.2) was added for exactly this.**
- **The straight-line flip is complete; every remaining bare release is a
  documented carve-out.** After A1–A6 the ~45 `emit_value_release` left in
  `jit.h` were audited site-by-site and each falls into a class the `Owned`
  handle is *not* meant to cover (§4.2). None is a clean straight-line
  `compile()`-temp discard — those are all converted. The carve-out classes:
  - **loop-body / inline-loop emitters** (`emit_inlined_*`, `*_inline_loop`,
    the for-in/iter body and dispose/iterable cleanup, the build-guard cleanup
    loop): a per-*iteration* `+1` whose scope-exit dtor would fire at the
    builder's current point — the wrong place inside an emitted loop.
  - **slot / scope primitives** (`store_slot_raw` releasing the old value,
    the scope-slot release helper): infrastructure below the `Owned` layer.
  - **threaded chain values** (`emit_index_step`'s receiver, the postfix
    lvalue temporaries): the value is a function *parameter* handed down a
    chain, not a straight-line temporary; ownership is threaded, not scoped.
  - **branch-spanning consume-or-release** (the non-literal index `key`
    consumed by the point arm but released by the slice arm; compound-assign
    `cur`/`rval`/`lval` across the in-place-Tensor rebind branches): one arm
    consumes, another releases — a single dtor can't model both.
  - **conditional release** (the defer body result, freed only when the block
    has no terminator; a scope-exit dtor would emit a release into the
    terminated/unreachable path).
  - **statement-sequencing** (`compile_statements` / standalone block): the
    result is loop-accumulated and freed *before* the next statement compiles
    — a naive move-assign rewrite shifts the drop past it, changing timing the
    interpreter observes. (Since converted: the loop now rides an `Owned`
    with an explicit `drop()` ahead of each `compile`, which lands the
    release on the same instruction the hand-placed one occupied.)
  - **shared-receiver dispatch children** (`compile_user_method_over_builtin`
    / set-mutate / method-or-ufcs): the receiver/callee raw is shared across
    runtime-exclusive arms; consumed once per path, not scoped.
  A probe sweep found **no normal-path leaks** across these (statement
  discard / `cond` / guards / interpolation / `??` are flat; `if`/`while`/
  logical conditions only accept Bool/Long/Float, so a heap `+1` can't reach
  them on the normal path — only their *throw* edges carry heap refs, the
  per-region cleanup pads' job, §4.3). The 28 `emit_value_retain` are the
  dual set (slot stores, pattern bindings, `this` hand-offs) — legitimate
  ownership *generation*, not debt. These bare calls stay bare, counted by
  the `tools/check_rc_discipline.sh` ratchet (the population may only
  shrink) and guarded by the CI leak batteries
  ([[project_gc_safety_phase_plan]]).
- **Throw-path cleanup — shipped across the board (leak-fuzzer C①–C⑨,
  2026-07).** The leak-fuzzer (run under the loud teardown audit,
  `CULEBRA_GC_LEAK_ABORT`) showed the corpus's *throwing* cases were never
  measured by the growth gate (its `_p` warmup catches and skips a throw), so
  "cycle-only" held only for the non-throwing subset. The throw edges are
  closed by two complementary mechanisms, chosen per site by who owns the
  in-flight `+1`:
  - **Codegen owns it → `ThrowGuard`** (a §4.3 cleanup pad packaged as an RAII
    handle): the callee closure in `compile_call` (the callee frame borrows
    `__cls__`, so a throw stranded it), the builtin-method receiver and operand
    args (`[1,2,3].enumerate("x")`), the assignment lvalue across the
    final-assignment switch (`o.a = v` on an ImmutableError), tensor/set/string
    operand args, the UFCS free-function callee, and the HOF callback (see the
    tag-gated note below).
  - **The helper throws it directly → callee-cleans-on-direct-throw.** Every
    runtime helper that raises a *direct* error — no user dispatch ran —
    releases the owned operands it was handed before raising. This is mutually
    exclusive with user `__op__`/method dispatch (whose callee frame +
    the invoker's unwind guard already clean a throw; guarding there double-frees,
    ASan-confirmed), so each edge has exactly one releaser. Shipped for: the
    operator helpers (`_arith_guard_numeric`, `num_matmul`/`num_neg`,
    `to_bool`, and the ordering ops via the `_value_<name>_borrow`/public
    split so sort comparators keep the borrow contract — C①); index/slice
    (`object_get_any` under the `own_receiver` gate — C③, extended to the
    shared-val arm in C⑤); the not-a-function error path
    (`compile_function_call_raw`, gated per call site via
    `own_this_on_error`/`own_args_on_error` because the
    unresolved-builtin path already frees the receiver on that edge — a
    blanket release double-frees); `==`/`!=` non-Bool coercion (C⑤); the
    property-read cold path (`culebra_runtime_prop_get` under `own_receiver` —
    C⑧); and the HOF callback-reject arm (C⑦).
  - **Two subtle contracts worth restating.** (1) The HOF callback is
    **callee-consumes, uniformly**: the codegen `consume()`s the callback's
    `+1` into the HOF runtime, which owns it on every exit — `JitHofCallback`
    for the eager drivers, the capture cell for the lazy factories, the
    reject guard for a validation throw. The only codegen-side cleanup left
    is for edges where no helper runs: the receiver-resolution error
    (`dispatch_arr_iter`'s error block releases the consumed values) and
    sort_by's `reverse:` coercion window (a scoped `ThrowGuard`). This
    replaced an earlier tag-gated split (Function guarded codegen-side,
    adapter freed helper-side) whose compensating adapter-side release
    freed a lazily-captured instance out from under its capture cell — a
    teardown use-after-free on a mid-iteration `__call__` throw. (2)
    `emit_point_index` returns uniformly `+1` (C⑨) — the array path retains
    the borrowed slot, the shared-val memoized sub-view is retained in
    `object_get_any` — so both INDEX promotion sites just release the receiver
    (the old borrowed-promotion `swap_owned` double-counted the object path).
  - **Native methods use helper-side RAII** (`JitMethodSelf`/`JitMethodArgs`):
    a native callee (channel send, `with_lock`, @wrap foreign thunks) consumes
    its `+1` self/args on *every* exit, because the caller does not clean a
    native method's operands on unwind (C⑤/C⑨).
  - The suite-wide GAP5 gate (`tools/difftest/leak_abort_suite.sh`, a standing
    `just test` phase) pins all of the above: any new inflated-RC leak on any
    corpus case, throw paths included, fails with the object's birth site.
- **The `compile_*` return seam is closed (2026-07-12, §4.9).** All 64
  `compile_*` helpers in jit.h, the extension compile hooks, and the stdlib
  implementations return `Owned`; `compile()`'s `own(compiled)` boundary is
  deleted. Migrating the call sites converted the remaining raw batch
  carriers (class/enum/multifn decl method + static + dispatcher vectors,
  decorator rolls, the for-in protocol iterator) onto `Owned`/`consume()`
  handoffs with the absorbing registry/slab/slot named at each consume. Three
  crossings stay raw by proof, as `consume_unchecked` sites with rationale
  comments (the two `call.phi` late-merge arms — an `Owned` held across the
  sibling arms would spill a non-dominating SSA value into the §4.8 window —
  and the for-protocol iterator owned by its alloca slot); the ratchet
  ceiling moved 11→14 for exactly these. The §4.9 pin caught the one
  conversion slip in flight (the iterator raw reused across property-get
  blocks) at difftest compile time. Two ratchets pin the seam:
  raw-returning `compile_*` = 0 and typed consume-assignments = 0.
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
