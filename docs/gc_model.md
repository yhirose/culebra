# Culebra Memory Model — the verified current state, and the road to structural leak-freedom

Status: **Canonical.** This is the single authoritative statement of how RC,
GC, and deterministic `drop` actually behave today — produced by a dual
independent code audit (two reviewers, cross-checked, every claim
code-grounded) after a stale invariant in `jit_gc_design.md` was found to have
poisoned earlier reasoning. Where this doc and another doc disagree, **this doc
wins**; fix the other.

The two goals this document serves, in priority order:

> **G1 — Deterministic `drop`.** An object with a `drop` method fires it
> exactly once, at a predictable point, in a predictable order, identically on
> every backend.
>
> **G2 — Structural leak-freedom.** No unreachable object survives
> indefinitely, and no leak is silently masked — by construction, not by
> per-site vigilance.

§1–§4 describe what is true today. §5 is the gap list: every place where
today's behavior falls short of G1/G2, stated as requirements.

---

## 1. The two-layer model

Culebra's memory management is **RC-primary + tracing-backstop** on both
backends:

- **RC owns memory and `drop` timing.** Interp: values are `shared_ptr`
  (automatic, exact). JIT: values are tagged `i64`; RC placement is owned by
  the layered ownership discipline of `jit_ownership.md` §4 (`Owned` handles,
  scope slots, the unwind-temp window, block-pinned raws); the remaining bare
  retain/release sites are audited carve-outs counted by the
  `check_rc_discipline.sh` ratchet.
  Release-to-zero frees promptly and fires `drop` — this is what makes `drop`
  deterministic (G1).
- **Tracing reclaims what RC cannot**: reference cycles, and anything leaked
  by an RC placement bug. JIT: a conservative mark-sweep over a registry heap
  (`jit_gc.h`), whole-machine-stack root scan, refcount-independent. Interp:
  `InterpGC`, a precise CPython-style gc_refs collector over *tracked node
  kinds only* (see §5-GAP2).

Why both layers are necessary (and neither is retirable):

- RC alone is never leak-free: cycles keep each other's counts positive
  forever, and hand-placement bugs leak. (Swift — compiler-perfect ARC, no
  cycle collector — leaks retain cycles routinely; that is the counterexample
  to "systematic RC suffices".)
- Tracing alone destroys G1: a tracer discovers death only at collect time, so
  `drop` would fire late, unordered, non-deterministically (the Go/Java
  finalizer model). Deterministic finalization under shared ownership requires
  full reference counting.

So the architecture is not "RC with a temporary crutch" — **the tracer is a
permanent, load-bearing component**, and the design question is only how to
stop it from *silently masking* RC bugs (G2, §5-GAP5).

## 2. Deterministic `drop` — the four-path union

`drop` fires from four paths, **deduped to exactly-once by the per-object
`dropped` flag** (JIT: `JitObject::dropped`; interp:
`OrderedSymbolMap::dropped`). The flag, not the call sites, is the invariant.

| path | trigger | mechanism |
|---|---|---|
| (a) release-to-zero | last reference released | JIT `_culebra_value_release_impl` OBJECT case fires drop *first*, then tears down proto/slots/sidecars. Interp: the prop-map `shared_ptr` deleter → `_destroy_prop_map` → `_call_drop_if_present`. |
| (b) explicit `obj.drop()` | user call | `culebra_runtime_explicit_drop` / interp eval; object stays alive (the flag just prevents a second fire). |
| (c) scope exit | owned region resolution | Both backends run a **localized Bacon-Rajan trial deletion** over the scope's registered drop-bearing objects (`owned_scope_exit` / `_owned_resolve_ambiguous`): unreachable-from-outside → fire now (cycle members included); externally reachable → survives, compacted to the parent scope. Budget `kNodeBudget = 4096`; overflow → all survive (safe direction: late, never early). |
| (d) GC backstop finalize | collect finds dead set | **PEP-442 style pre-sweep pass** — JIT `_jit_gc_finalize_dead`, interp `_owned_gc_backstop`: fire the dead set's `drop`s while the structure is intact, then sweep/clear reclaims *memory only* under `_drop_suppressed`, so nothing re-fires. **Cycle members DO fire `drop`.** |

Ordering contract: LIFO (newest-first) within a scope; defers run before slot
release on every exit path; parent-before-child for the RC cascade. The
interp's five scope-exit verdicts (`kOwnedSurvive/Leave/Cascade/Release/Fire`)
exist solely to replay the JIT's drop order — acyclic candidates are left to
the real `shared_ptr` cascade on purpose.

Load-bearing subtleties (do not "simplify" these away):

- `_culebra_call_drop_if_present` **pins refcount to `1<<40` during the body
  and restores the entry count after**. The huge pin absorbs unbounded
  re-entrant releases (a drop body doing `this.me = nil` on its own cycle);
  the *restore* (not zero) is required because paths (b)/(c)/(d) arrive with
  live references.
- Scope-exit survivors are re-pushed **before any drop fires** — a drop body
  may register new higher-id entries, and appending survivors after them would
  corrupt the id-ordered stack.
- `culebra_runtime_throw` deliberately does **not** retain the payload: the
  carrier takes over the thrower's `+1`. Re-adding a retain there reintroduces
  a fixed leak (immortal caught payloads, suppressed drops).
- Multifn dispatchers: the RC path releases table-held body refs
  (`release_bodies=true`); the sweep path must not (bodies have their own
  sweep entries) — the asymmetry is a double-free guard, and table detach must
  precede body release (a fired drop may re-declare the same name).
- Top-level bindings intentionally never fire drop at program exit (JIT
  suppression flag ≙ interp's never-torn-down global env). Symmetric by
  construction.

## 3. What is refcounted, and what is not

Refcounted (refcount at struct offset 0, uniformly): `Func` (closure),
`Array`, `Tuple`, `Object`, `Tensor`, `Set`, plus internal capture `Cell`s.

**Not refcounted: `String`/`StringView`** — JIT strings are `malloc`ed and
**leak for process lifetime, by design**. Retain/release are silent no-ops on
strings, which also means string ownership mistakes are invisible to any RC
audit. Nil/Bool/Long/Float are immediate values.

## 4. Rooting — the two backends do it differently

- **JIT (shipping default):** conservative scan of the *entire machine stack*
  (`setjmp` register flush, SP→stack-base walk), refcount-independent. Every
  C++ helper frame's raw in-flight `JitValue` is rooted whether or not its
  refcount is right. `gc_refs` mode (`CULEBRA_GC_REFS=1`, off by default) is
  the CPython algorithm — refcount minus in-heap edges, **no stack scan**; its
  soundness ≡ RC accounting accuracy. `collect_refs_diag` classifies
  conservative-dead-but-gc_refs-retained objects into *inflated-RC* (definite
  RC placement leak) vs *transitively-held* (cycle) — a ready-made
  zero-false-positive RC-leak detector.
- **Interp:** **no C++ stack scan at all.** Safety is *scheduling*: collection
  runs only at statement boundaries, with roots hand-walked from the current
  env chain + `FrameRootGuard` entries. Any future mid-expression collect
  shortcut would sweep in-flight temporaries.

Consequence: "rooting is provided independently of any single retain" (the
`jit_ownership.md` §2 claim) is **JIT-only**. And under `gc_refs`, rooting is
only as sound as the RC accounting — the conservative scan roots strictly more
(borrowed in-flight helper temporaries) than `gc_refs` can. This is why the
conservative scan cannot be replaced by `gc_refs` on the strength of a
*balance* proof alone (balance ≠ lifetime; an early-but-balanced release is a
UAF window under `gc_refs` that the conservative scan silently absorbs).

## 5. The gaps — requirements for G1/G2

Each gap is stated as: what is broken today → the requirement it imposes.
These are the *complete* known set as of the dual audit (2026-07).

### GAP1 — `break`/`continue` skip owned-slot release (JIT). **CLOSED (2026-07).**
Was: `compile_break`/`compile_continue` ran the iteration's defers and jumped;
they did **not** run `release_scope_slots` for the abandoned iteration scopes.
The per-iteration owned slots leaked to the backstop — and because the stale
slot alloca kept the object conservatively rooted, a drop-bearing object bound
in a loop body exited by `break` **never fired `drop`** (not late — never, until
frame exit at best). The interp releases the same slots automatically
(`shared_ptr` unwinding through the C++ `BreakSignal` path), so this was a
**backend asymmetry in drop semantics**, strictly worse than a leak.
**Fix:** each `LoopBlocks` records the loop body scope's index in `scopes_`;
`compile_break`/`compile_continue` now call `emit_loop_scope_exit`, which
releases the owned slots of every open scope from the innermost one down to
that body scope (inclusive) and resolves the owned region at the body scope's
mark — the same `release_scope_slots` + `emit_owned_scope_exit` mechanism
fall-through uses (`scopes_` is left intact, so the loop's own
`finish_and_pop_scope` still runs on the now-dead fall-through path). break's
target already releases the loop's outer scope (the for-in iterable) at the
loop end, so only the per-iteration scopes are closed here. Verified: interp/JIT
drop-timing symmetry (`tests/test_drop_loop_control.cul`), leak-fuzz gate (no
new leaks), difftest corpus, full gate. Note the leak-fuzzer's per-iteration
growth oracle does **not** catch this class (a one-shot `break` is not
per-iteration growth; a `continue`'s slot is overwritten by the next
iteration's store) — it is a drop-*timing* bug, caught only by the drop test.

### GAP2 — Interp cycle blind spot: pure Object↔Object / Tuple / Set cycles. **CLOSED (2026-07).**
Was: `InterpGC` tracked only Array backing vectors and closure Environments, so
a cycle routed entirely through Object property maps (or Tuple/Set containers)
was invisible — memory never reclaimed, and if it escaped its birth scope
`drop` never fired.
**Fix:** Object prop maps, Tuple elements, and Set members are now tracked
cycle nodes (`GcKind` {Env,Vec,Set,Map}; Map/Set entries carry their sidecar
weak_ptrs, locked into `live` for the whole collect so a sibling's clear can't
free a still-to-be-swept sidecar mid-sweep). The container edge enumeration is
single-sourced into `gc_for_each_container_backing` / `gc_for_each_child`
(the Set `index`-key edge, absent in both collectors before, is now counted).
Two hidden-capture under-counts that the node change turned into spurious roots
were fixed: synthetic `drop`-`this` views emit no edge (`is_synthetic`), and
`_wrap_method_with_this` holds its receiver in one shared Value that both the
eval closure and the collector hook share (a second copy left the receiver
rooted). Verified: `tests/test_interp_cycle_gc.cul`, GC_STRESS over the corpus
(over-collection/UAF detector), difftest, leak-fuzz, full gate. The
language-spec "cycles are a hazard" carve-out (§16 / §24 + ja mirrors) is
deleted. **Remainder (follow-up):** route `_owned_resolve_ambiguous` through the
same shared enumeration so its still-separate container walk cannot re-drift
(it currently under-counts the Set `index`, a conservative pin, not a bug).

### GAP3 — Runtime helper interiors leak on the C++ throw path (JIT). **G2 violation — the P0 class.**
Iterator/HOF helpers (`iter_collect`'s `out`, `iter_reduce`'s `acc`,
`_iter_filter_fast_fn`/`_iter_take_while_fast_fn`'s pending element,
`array_map`'s half-built `out`, …) hold raw `+1` `JitValue`s across calls that
re-enter user code and can throw. On throw they are dropped on the C++ stack
with no RAII — reclaimed only by the backstop (masked, G2 violation), and the
leak-fuzzer pins them (the 47-entry baseline's acyclic majority).
`JitOwnedVal` (move-only owned wrapper, dtor releases on unwind, `consume()`
to hand out) exists and is proven on `iter_find`.
**Requirement — FIX:** every helper-held owned `JitValue` lives in a
`JitOwnedVal` (or its `JitMethodSelf`/`JitMethodArgs` ABI aliases where those fit). Bounded
work: the callback-re-entrant helper set is ~24 functions.
**Requirement — ENFORCE (this is what makes it *structural*, not just fixed):**
a static gate that fails the build on a **raw owned retain inside a runtime
helper body** — i.e. a `culebra_runtime_value_retain` (or a `+1`-returning
helper's result) held in a bare `JitValue` local rather than a guard, across a
call that can throw. Without this, GAP3 is "fixed the current 24" +
coverage-bound regression net; *with* it, a future 25th helper that hand-holds
a raw `+1` **cannot compile**, so the throw-path leak can never re-hide. This
is the C++-side analogue of codegen's double-consume assert.
**Status — FIX landed (leak-fuzzer C①–C⑨, 2026-07):** the known helper-held
leaks are closed (RAII guards + callee-cleans-on-direct-throw convention,
`docs/jit_ownership.md` §4.3/§6) and the suite-wide GAP5 gate pins them.
**Status — ENFORCE landed as a ratchet gate (2026-07):**
`tools/check_rc_discipline.sh`, a standing `just test`/`test-dev` phase,
counts every bare RC call per runtime file (stdlib_jit.h / sendable_jit.h)
plus the codegen-side bare emit sites, and fails the build if any count
grows. A future helper that hand-holds a raw `+1` instead of using the RAII
forms trips the ceiling; converting a site lowers it (ceilings only shrink).
This is a ratchet rather than a per-site compile error — the remaining bare
calls are audited carve-outs, each justified in the script's comments.

### GAP4 — Codegen-side exception edges: the dominant fuzzer leaks. **G2 violation.**
The biggest measured leaks (exception-through-iterator ~10 obj/iter with
integer elements — i.e. chain temporaries, not elements) are **caller-side**:
the `.iter()` chain / receiver / in-flight temporaries are released on the
normal path but stranded on the throw edge, in exactly the carve-out shapes
`Owned`'s straight-line dtor cannot model (threaded chains, loop bodies,
branch-spanning consume-or-release). The per-region cleanup-landingpad
machinery (`finish_scope_cleanup`, build/pending guards) is the shipped,
correct mechanism — these sites just aren't wired into it.
**Requirement — FIX:** every carve-out that currently holds a `+1` across a
may-throw region rides a cleanup pad. Drive the leak-fuzzer baseline to its
cycle-only residue, each closed class pinned by a battery pattern.
**Requirement — ENFORCE (structural):** a **codegen-time accounting pass over
the `Owned` handles**: every `Owned` produced must be consumed-or-dropped on
*every* control-flow path the emitter takes, including exception edges — a
codegen-time abort otherwise (the all-paths generalization of the existing
straight-line double-consume assert). This is NOT the rejected whole-IR
ownership verifier: it reasons only about the codegen's own `Owned` handles
(finite, emit-time-visible), not heap aliasing or object lifetime, so it
sidesteps that verifier's fatal objections (helper interiors → covered
separately by GAP3's C++ RAII; balance ≠ lifetime → discharge-accounting makes
no lifetime claim). The genuinely hard part is that the carve-out *shapes*
(threaded chains, shared-receiver dispatch, branch-spanning consume-or-release)
are exactly the ones a single `Owned` can't currently model — closing GAP4
structurally means giving those shapes an `Owned`-family abstraction the
accounting can see. Without ENFORCE, a *new* carve-out silently reintroduces a
throw-path leak; with it, an unaccounted `+1` is a build failure.
**Status — FIX landed (leak-fuzzer C②–C⑨, 2026-07):** the known caller-side
throw-edge leaks are closed (`ThrowGuard` cleanup pads + per-helper
callee-cleans contracts, `docs/jit_ownership.md` §6), the leak-fuzz baseline
and the abort-suite allowlist are both cycle-only, and the suite-wide GAP5
gate pins the throw paths.
**Status — ENFORCE landed (2026-07-11), as a *constructive* mechanism rather
than an assert:** the automatic unwind-temp window
(`docs/jit_ownership.md` §4.8). Every live `Owned` `+1` is spilled around each
may-throw call and released by the scope-chain cleanup pads, so an emitter
holding a `+1` across a throwing sub-compile is *correct by default* — the
leak cannot be written, which is stronger than aborting on it. Call sites
whose callee already cleans the unwind edge declare it (`UnwindCovered`, the
machine-visible §4.7 contract), keeping exactly one releaser per edge. The
probe class this closed (binop lhs / call argument / index receiver / `==`
lhs / method receiver held across a later throwing operand) was leaking in
every shape despite the cycle-only corpus baselines — the corpus never spelled
those shapes; `tools/difftest/leak_abort.sh` Case 4 now pins them.
**Status — the last escape hatch closed (2026-07-12,
`docs/jit_ownership.md` §4.9):** the window protects a `+1` *in a handle*;
a bare `llvm::Value*` from `consume()` carried across a basic block was still
invisible to every layer. `consume()` now returns a block-pinned token
(using the raw outside its pin block aborts codegen, in every build mode),
every `%Value` phi is built through the checked `OwnedPhi`, and every
`compile_*` helper (core and extension hooks) returns `Owned` rather than a
raw `+1`. Because the difftest corpus *compiles* nearly every construct, a
violating pattern is caught at compile time with no runtime leak repro —
coverage moved from executed paths to compiled paths.

### GAP5 — The backstop silently masks RC bugs. **G2 violation — the meta-gap.**
Today a placement bug ships as "backstop reclaims it, nobody ever knows".
Detection is coverage-bound (tests + fuzzer); an unexercised path's leak is
invisible until a user reports memory growth — an unacceptable feedback loop.
The structural fix is GAP3+GAP4 (make placement bugs impossible at the
source); the *detection* fix already has shipped machinery:
`collect_refs_diag`'s inflated-RC classifier is a zero-false-positive definite
RC-leak detector that covers helper interiors too.
**Requirement:** in debug/CI builds, an inflated-RC classification aborts with
the object's birth site (loud, actionable). The leak-fuzz gate
(`tools/difftest/leak.sh`, baseline-driven) stays as the corpus-wide
regression net, its baseline monotonically shrinking to the cycle-only
residue. Production keeps reclaiming (a crash would be a DoS trade), which is
acceptable *because* GAP3/GAP4 remove the bug class at the source rather than
relying on detection.

**Status — machinery shipped (2026-07).** `CULEBRA_GC_LEAK_ABORT=1` arms it:
the JIT captures each object's allocation backtrace (`Heap::birth_sites_`, only
when armed) and, at program teardown, `Heap::maybe_audit_leaks` classifies the
heap and aborts with the birth site of every inflated-RC object. Two facts the
original plan understated:
- **The audit runs at ONE quiescent safepoint, not per-collect.** Mid-
  expression, a freshly allocated object legitimately has refcount 1 and zero
  internal edges (looks inflated) while its only reference sits in a register
  the conservative scan has not spilled — indistinguishable from a true orphan
  at a snapshot. So the audit is hooked into the JIT teardown collect
  (`run_modules`' `CollectGuard`), after the top-level body returns but while
  the module/namespace roots are still wired: every legitimate newborn has been
  rooted or released, so an inflated-and-dead object there is a real leak. It
  runs regardless of `CULEBRA_GC_NEVER`, so the fuzzer's backstop-off mode still
  gets a loud teardown check. Verified zero-false-positive on clean code (fn-
  and top-level-scoped) and benign self-cycles (classified transitively-held,
  not inflated); fires with an exact birth site on a real acyclic leak
  (`Shared.new` sub-view) and on throw-path inner-scope leaks.
- **A no-LTO (debug/CI) tool.** The audit rides the conservative scan's
  *completeness*, which is best-effort by design (a stale stack word may alias a
  dead object as live). LTO's altered stack layout widens that gap enough that
  the LTO release build under-reports (misses leaks the no-LTO build catches),
  so the detector targets the no-LTO gate/dev builds — exactly the "debug/CI"
  the requirement names. Production (LTO) keeping quiet is the intended DoS
  trade, not a regression.
- **Armed suite-wide (C⑥, 2026-07).** With the non-cycle baseline leaks
  closed, the audit now runs over the whole difftest corpus as a standing
  `just test` phase (`tools/difftest/leak_abort_suite.sh`, allowlist
  `leak_abort_allow.txt` — cycle-only, 2 entries). A new inflated-RC leak on
  any corpus case, including throw paths the growth gate cannot measure,
  fails the gate with the object's birth site. The `leak-abort` smoke test
  (`tools/difftest/leak_abort.sh`) still guards the detector itself
  (fires-on-leak + zero-FP), and `just leak-abort FILE` runs any script under
  it to localize a reported leak.

### GAP7 — Residual `drop`-timing carve-outs. **G1, minor — ACCEPTED (2026-07-11).**
Three cases where `drop` is not *exactly* deterministic, all **accepted as
documented language semantics** — they match the Python/Swift norm, and each
degrades only timing, never a resource's actual reclamation:
1. **`kNodeBudget = 4096` overflow** — a scope resolving >4096 drop-bearing
   objects at once defers the overflow to the backstop (late, never early).
   Rare (huge resource graph in one scope).
2. **Top-level bindings never fire `drop` at program exit**, by design (JIT
   suppression flag ≙ interp's never-torn-down global env). A global with a
   `drop` will not finalize at exit.
3. **Self-captured-env timing asymmetry** — interp force-pins a self-capturing
   env, firing its cycle's `drop` one collect later than the JIT. Drop *count*
   is symmetric; *timing* differs by one collect.
**Decision: accept all three** — the "deterministic drop as strong as
Python/Swift" bar, not "zero-carve-out". Rationale:
- Every mainstream RC/GC language ships the identical carve-outs: Python's
  module-level `__del__` is not guaranteed at interpreter exit and its cyclic
  finalizers were unreachable pre-PEP-442; Go's `defer` does not run on
  `os.Exit`; C++/C static-dtor and `atexit` order/execution is not guaranteed
  under `_exit`/signal/`abort`. This is the accepted state of the art.
- The one carve-out with user-visible stakes (#2) has **no impact on the
  standard library's resources**: `File` is a `std::fstream` in a per-Runtime
  table, so normal-exit teardown (or `Sys.exit`) runs the stream's destructor
  and flushes/closes regardless of whether `drop` fired — verified by writing
  an unflushed top-level file and reading it back after both a normal exit and
  `Sys.exit`, on interp and JIT. #2 is observable only for a *user-defined*
  `drop` whose side effect (a goodbye packet, a remote commit) has no
  lower-level RAII backstop AND whose object is bound at top level — and
  Culebra already offers `with` / `defer` / explicit `.drop()` for exactly
  that case, the same escape hatch Python/Go added after the fact.
Closing them (raising the budget, a drop-at-exit pass, aligning the interp
pin) is possible but not pursued: #1/#3 buy almost nothing, and #2's
top-level-at-exit pass would trade the interp/JIT symmetry requirement
([[feedback_check_jit_interp_symmetry]]) for a guarantee no shipping language
makes. Revisit only if Culebra is ever positioned on Rust-grade deterministic
resource management as an explicit selling point.

### GAP6 — Stale docs poison reasoning. **Meta.**
This audit exists because `jit_gc_design.md` §0/§6a still said "cycle members
do not fire drop" long after the PEP-442 finalize shipped (fixed 2026-07).
Two more comment-vs-code drifts found: `release_scope_slots`' docblock claims
params are borrowed slots (they are owned; only capture cells are borrowed),
and `release_all_scopes_for_exit`'s "throw paths still leak" comment predated
`finish_scope_cleanup` (throw is covered; break/continue was the part still
true — now closed by GAP1's fix; comment updated).
**Requirement:** invariant-stating banners live in ONE doc (this one); other
docs link here. A behavior change that touches an invariant updates this doc
in the same commit ([[feedback_sync_code_docs_memory]]).

## 6. What "done" looks like — and the fix/enforce line

There are **two finish lines**, and the gap list crosses them at different
points. Be explicit about which one is the target.

### G1 — deterministic `drop`
- **Practical (as strong as Python/Swift):** GAP1 + GAP2 closed (break/continue
  and escaped cycles fire drop on both backends, every shape), four-path union
  + `dropped` flag stays the single mechanism (any fifth path goes through
  `_culebra_call_drop_if_present`). GAP7's three carve-outs accepted as
  documented semantics.
- **Zero-carve-out:** additionally close GAP7 (budget, top-level-at-exit,
  interp pin timing).

### G2 — structural leak-freedom + no silent masking
G2 has two halves; they finish at different points:
- **"No unreachable object survives" — ALREADY DONE.** The tracer guarantees
  this by reachability, permanently. No gap.
- **"No leak silently masked" — two tiers:**
  - **Practical (very strong, coverage-bound) — REACHED (2026-07):**
    GAP3-FIX + GAP4-FIX (all *known* throw-path leaks closed) + GAP5
    (inflated-RC detector loud in debug/CI, armed suite-wide) + the leak-fuzz
    gate and the abort-suite allowlist both at their cycle-only residue.
    Result: no known leak, new ones caught in CI, loud birth-site aborts.
  - **Structural (coverage-independent, "can't hide") — REACHED (2026-07):**
    additionally GAP3-ENFORCE (the per-file bare-RC ratchet gate) +
    GAP4-ENFORCE (the §4.8 automatic unwind-temp window — the throw-path
    leak is correct-by-default rather than an abort) + the §4.9 block-pin
    (`consume()` returns a block-pinned token; a bare `+1` crossing a basic
    block aborts codegen; every `compile_*` helper returns `Owned`, so no
    raw `+1` crosses a compile-layer C++ return). A *new* leak now requires
    either raising a ratchet ceiling in review or writing through a
    `consume_unchecked` escape hatch with a per-site rationale — it cannot
    appear silently.

**Where this leaves us (2026-07-12):** deterministic drop is
Python/Swift-strong (G1 practical, GAP7 carve-outs accepted as semantics),
and both G2 tiers are reached. The residual leak surface is: cycles (the
tracer's permanent job, allowlisted as the 2-entry cycle-only residue) and
the audited carve-out sites the ratchets count (each with a written
rationale). The corpus gates are now the safety net, not the discovery
mechanism — a new leaking pattern is caught when it is first *compiled*
(§4.9) or first run in CI (GAP5), not when a user reports memory growth.

Explicit non-goals, decided with data this cycle: retiring the tracing
collector (contradicts G2 — tracing IS the structural guarantee; and `gc_refs`
roots strictly less than the conservative scan for in-flight helper
temporaries); a whole-function IR ownership verifier (adversarially reviewed
and rejected: blind to helper interiors, balance ≠ lifetime — note GAP4-ENFORCE
is a *narrower* Owned-handle accounting, not this); Perceus reuse/elision
(measured dead: slab already reuses 98.7%, sharing is genuine).
