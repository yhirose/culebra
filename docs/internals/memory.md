Memory and Resource Management: Implementation
===============================================

This document describes how Culebra's memory management is
implemented. It is not a specification: the observable contract —
what a program can rely on — is normative in
[`language.md` §17](../language.md#17-memory-model) and
[§25](../language.md#25-appendix-interpreter--jit-divergence). This
document explains *how* that contract is met, on each backend, and
why the implementation takes the shape it does. Where this document
and `language.md` disagree, `language.md` wins.

The Japanese mirror of this file is
[`memory.ja.md`](memory.ja.md).

Contents
--------

1. [Two engines, one contract](#1-two-engines-one-contract)
2. [The interpreter: exact reference counting](#2-the-interpreter-exact-reference-counting)
3. [The JIT's problem: reference counting without `shared_ptr`](#3-the-jits-problem-reference-counting-without-shared_ptr)
4. [The JIT's ownership discipline](#4-the-jits-ownership-discipline)
5. [Detecting reference-counting bugs](#5-detecting-reference-counting-bugs)
6. [The JIT's tracing backstop](#6-the-jits-tracing-backstop)
7. [Design lineage](#7-design-lineage)

---

## 1. Two engines, one contract

Culebra's memory management is **reference counting (RC), primary,
with a tracing collector as a backstop** — on both the interpreter
and the JIT. Neither engine is retirable in favor of the other:

- **RC alone is not leak-free.** A reference cycle (`a.c = a`) keeps
  every member's count above zero forever; no amount of correct RC
  placement reclaims it. This is not specific to Culebra — Swift's
  ARC, generated entirely by the compiler with no room for
  programmer error, leaks retain cycles routinely. It is the standard
  counterexample to "systematic RC suffices."
- **Tracing alone cannot give deterministic `drop`.** A tracing
  collector discovers that an object is dead only when it runs a
  collection. Between the last reference disappearing and the next
  collection, the object is inert but not yet finalized — so `drop`
  fires late, and in whatever order the collector's sweep happens to
  visit objects. Go and Java's finalizers behave exactly this way,
  which is why neither language offers deterministic destruction.
  Deterministic finalization under shared ownership is only possible
  with exact reference counting: the count reaching zero *is* the
  signal, and it happens at the instant the last reference is
  released.

So the two engines answer two different questions. RC decides *when*
an object dies and drives `drop` timing. The tracing collector
answers *what RC cannot*: it reclaims cycles, and — as a backstop —
anything a reference-counting placement bug would otherwise leak
permanently.

### The two failure modes RC placement can produce

Manual reference-counting has exactly two ways to go wrong:

- **A missed release** (a leak): an object outlives every reference
  to it because some code path failed to decrement.
- **An extra release** (a use-after-free / double-free): an object is
  freed while a live reference to it still exists, because some code
  path decremented a reference it never owned.

Getting placement right on every control-flow path — fall-through,
`break`, `continue`, an early `return`, an exception unwinding through
several scopes — by hand, is hard. §3 and §4 describe why the two
backends face this problem to a very different degree.

## 2. The interpreter: exact reference counting

The interpreter represents every heap value as a C++ `shared_ptr`.
Its reference-counting is therefore **exact and automatic by
construction**: the compiler, not a human, emits every increment and
decrement, and `shared_ptr`'s destructor cannot be skipped by a
control-flow path the way a hand-placed `release()` call can. Leaks
and double-frees from RC placement are not a class of bug the
interpreter has — its only remaining exposure is reference cycles,
handled by `InterpGC`.

### `InterpGC`: a precise, CPython-style collector

`InterpGC` is **precise**, not conservative: because every refcount is
an exact `shared_ptr` count, the collector does not need a fallback
scan of the C++ stack to find roots. It uses the CPython algorithm —
for each tracked object, subtract the number of references held by
other tracked objects (in-heap edges) from its total refcount; what
remains is the refcount contributed by things *outside* the tracked
set (stack locals, other live data). An object whose subtracted count
is zero is reachable only from within the tracked set — i.e., part of
a garbage cycle — and can be reclaimed. This is `gc_refs` subtraction
followed by a breadth-first walk to confirm the classification, then
a clearing pass.

`InterpGC` tracks Array backing vectors, captured Environments,
Object property maps, and Tuple/Set members as cycle nodes — the same
cycle shapes the JIT's backstop reclaims, which is what keeps the two
backends behaviorally symmetric (`language.md` §17 makes no
backend-specific exception for cycle shapes).

**Precision has a precondition**: every live value must be reachable
from a registered root, or the collector can sweep something still in
use — surfacing later as a spurious `NameError`. Two classes of
stack-only liveness require explicit rooting to satisfy this:

- **Active environment chains.** Collection is deferred to the next
  *statement boundary* rather than running at an arbitrary point.
  At each such boundary, the live environments are the current
  statement's `env` plus every caller's `env` still on the C++ call
  stack — each pushed onto an explicit root set via an RAII
  `FrameRootGuard` for the duration of the call.
- **In-flight C++-stack temporaries.** A freshly built self-capturing
  closure, held only as a C++ return value in transit between two
  functions, is not reachable from any environment yet. Deferring
  collection to the statement boundary (rather than the allocation
  point) covers this for free: by the time the next boundary is
  reached, the value has already been stored into a rooted
  environment.

Because collection never runs mid-statement, the interpreter needs no
conservative stack scan at all — safety here is a scheduling
guarantee, not a scanning one.

## 3. The JIT's problem: reference counting without `shared_ptr`

The JIT does not represent heap values as `shared_ptr`. It emits
retain and release as explicit LLVM IR at the point codegen decides
they are needed, to avoid the overhead of `shared_ptr`'s atomic
refcount operations on every value touch. This choice reopens both
failure modes from §1 — a missed release, an extra release — as
ordinary compiler bugs: every code path the compiler emits has to
place its retains and releases correctly, by hand, with nothing like
a C++ destructor to fall back on if a path is missed.

Two structural responses to this exist in Culebra, and both are
necessary:

- **§4**: a discipline that makes correct-by-construction retain/
  release placement the only way to write new codegen, so that a
  large and growing class of leaks and double-frees cannot be
  introduced in the first place.
- **§6**: a tracing collector, exactly analogous to `InterpGC` in
  role, that reclaims whatever residue the discipline in §4 still
  lets through (chiefly reference cycles, which no RC discipline —
  Rust's `Rc` included — can resolve on its own).

## 4. The JIT's ownership discipline

### 4.1 The goal

Leaks and double-frees in generated code must be prevented **by
construction** — the way RAII prevents a forgotten `delete` in C++,
and the way affine ownership prevents a use-after-move in Rust — not
found and patched one call site at a time. Patching individual sites
does not converge: every fix leaves the next, not-yet-audited code
path exposed to the same mistake.

New codegen that produces or consumes a heap value is required to go
through the ownership layer described below. A bare, hand-emitted
retain or release call in a *new* code path is treated as a defect to
justify in review, not a valid implementation choice.

### 4.2 Ownership and rooting are different jobs

A single refcount is doing two independent jobs at once, and
conflating them is what makes a naive "this retain looks redundant,
remove it" edit dangerous:

1. **Ownership / release** decides *when* an object is freed
   (release-to-zero, which is also what fires `drop`).
2. **Rooting / liveness** decides *what stays alive across a
   collection*.

The second job is already handled, independently of any single
retain, by both collectors described in this document: a value held
in a native stack frame's `+1` is a root purely by virtue of being on
the stack (§2) or the conservative scan (§6.2) — provided the
refcount accounting is accurate, which is exactly what the discipline
below establishes. Removing a "redundant-looking" retain is therefore
never a rooting question; it requires a complete accounting proof —
tracing every producer and consumer of that value's reference count —
never the addition of new rooting machinery.

A standing diagnostic makes the two failure modes distinguishable
during investigation: disabling every collection (an environment
variable toggle) turns any bug that **survives** into a pure ownership
bug (an over-release); a bug that disappears under this toggle was a
rooting gap instead. In practice, every crash investigated this way
has turned out to be the former.

### 4.3 The layered design

Each layer below removes exactly one way to leak or double-free.
Combined, producing a leak or double-free requires breaking a C++
type or RAII invariant — a compile error, not a silent runtime
accident.

**Uniform convention.** Every expression-compiling function returns a
value the caller owns one reference to (a `+1` value). A parameter or
a method receiver is *borrowed* (`+0`): the caller retains before the
call, and the callee consumes the reference it is handed rather than
retaining its own copy. A value stored into a scope slot is owned by
that slot from that point on.

**A move-only handle for transient values.** A `+1` value produced by
compiling one expression and consumed by the next is held in a
move-only RAII handle, not a bare pointer. The handle exposes three
operations: read without consuming, hand the reference onward exactly
once (into a slot, a call, or a function return — marking the handle
spent), or release the reference immediately for a value that dies
rather than being handed on. **Consuming an already-spent handle is a
compile-time abort** — a double-free is turned into a build failure.
If the handle is still holding a reference when it goes out of scope,
its destructor releases it — the same guarantee C++ RAII gives a
local variable. This handle only covers a *straight-line* region of
code; a value that must survive a loop iteration, or that is consumed
on one control-flow branch and released on a different one, needs the
next layer.

**Scope slots for values that escape a straight-line region.** A
value that must outlive its originating region is consumed into a
scope slot. The existing scope-unwind machinery then releases that
slot on *every* exit path — normal fall-through, `break`/`continue`,
an early `return`, and exception unwind (the last via a per-region
cleanup landing pad, eliminated by the compiler entirely when the
region provably cannot throw). This closed a real, previously-shipped
leak class: a scope's owned locals, and their `drop`, used to simply
never run when the scope was exited by `throw`. The same mechanism
covers every scope-like region uniformly — lexical blocks, loop
bodies, `match`/`try` bodies, the `for` loop's iterable scope, and the
function frame itself.

**Borrowed values expose no release operation.** A borrowed value's
handle offers only the read operation; there is no API that both
borrows and releases the same value. "A borrowed operand was
released" is therefore not a representable bug — the type system has
no method call that could cause it.

**The automatic unwind-temp window.** The move-only handle above
releases through a C++ destructor — which a runtime, LLVM-level
`throw` cannot run, because it unwinds through generated code, not
through C++ stack frames. Historically this meant any value held in
such a handle, live across a call that could throw, leaked on the
exception path unless a human had hand-placed a guard for that exact
shape — and no finite test corpus enumerates every shape. The fix
makes the unwind edge structural instead of enumerated: every live
handle is registered with the compiler as it is created; at the one
point where a call gets an exception-unwind edge at all, every
still-live, not-otherwise-covered handle is spilled into a
per-function cleanup slot for the duration of that call, and every
cleanup path releases that slot first. A value already covered by one
of the ownership contracts below is marked as such and skipped here,
which is what prevents this window from double-freeing it.

**Ownership contracts for values crossing a helper call.** A value
whose ownership must transfer across a call into a runtime helper
follows exactly one of a closed set of contracts — never more than
one, since two cleaners assigned to the same value is a double-free
by construction:

| Contract | Who releases on the unwind path | Used for |
|---|---|---|
| Caller-cleans | A per-region cleanup pad at the call site, eliminated when the region cannot throw | Builtin-method receivers and arguments, a call's callee expression, an assignment's target, the UFCS callee |
| Callee-cleans-on-direct-throw | A guard inside the helper itself, armed only once user-level dispatch has been ruled out | Operator implementations, index/property access under receiver ownership, the not-a-function error path |
| Callee-consumes-on-every-exit | An owned-argument handle declared at the helper's entry, releasing on both normal return and unwind | Native method entry points, higher-order-function accumulators and callbacks |
| Invoker-cleans | The invoker retains before the call; the callee's own frame releases on normal return; the invoker's guard releases only if the callee throws | Every window where user-defined dispatch runs — operator overloads, `eq`/`hash`/`cmp`, `__index__`, property getters |
| Transfer | The incoming reference is returned as-is, or handed directly into a capture cell or slot | Iterator-self methods, a lazily-built combinator's capture cell |

**A bare reference is confined to the block where it was produced.**
The last remaining escape hatch was the moment a reference left its
handle: reading a value out of its handle used to yield a bare LLVM
value that could then cross basic-block boundaries with nothing
tracking it — in practice, every recently discovered leak had exactly
this shape. The rule adopted instead: a bare reference may only be
used inside the basic block in which it was produced. This is sound
and complete because every call that might throw is compiled as an
`invoke` instruction, which always terminates its containing block —
so "still in the same block" already implies "no unwind edge has run
since the reference was produced." Reading a value out of its handle
now returns a token that records its origin block; using that token
outside that block is a compile-time abort in every build
configuration, so the mistake is caught the moment the offending code
is *compiled*, not merely when it happens to run. A small number of
genuinely block-crossing shapes (mutually exclusive dispatch arms
converging on a `phi` node, a value already owned by a scope slot,
the prologue transfer that declares a local) use an explicitly
unchecked variant of this operation, each with its own declared
releaser; the count of such sites is tracked and never allowed to
grow. Every function that compiles a sub-expression returns one of
these handles (or an empty one on failure) rather than a bare
reference — so a bare `+1` never crosses a C++ function-return
boundary between compiling steps.

### 4.4 What "no correct codegen leaks" reduces to

If every rule in §4.3 holds:

1. Every transient `+1` value is held in a handle, or has already
   been consumed.
2. A handle is consumed exactly once, or released exactly once — the
   move-only type plus its destructor plus the double-consume abort
   rule make "both" or "neither" unrepresentable.
3. Every value that escapes its originating region is consumed into
   exactly one scope slot, and scope unwind releases every slot on
   every exit path.
4. A borrowed value is never released, because no such operation
   exists on a borrowed handle.
5. Rooting is provided by whichever collector is active, independent
   of ownership refcounts (§4.2).
6. Cycles and any residual leak are reclaimed by the tracing backstop
   (§6); the backstop is not relied on for steady-state memory
   behavior — with ownership correct, it does real work only rarely.
7. Every handle live across a call that might throw has exactly one
   releaser on the unwind path: the automatic spill window by
   default, or a declared ownership contract — never both.
8. A bare reference exists only inside the block where it was
   produced; every deliberate exception is a declared,
   audited site.
9. No function that compiles a sub-expression returns a bare
   reference across its own return boundary.

The corollary: a codegen path that leaks or double-frees requires
breaking one of these nine invariants, each of which is enforced by a
C++ type or a compile-time check — not a runtime accident to be
caught by testing. Remaining hand-placed retain/release calls outside
this discipline are audited migration debt, counted and never allowed
to grow, and fall into a small number of legitimate categories the
handle layer is not meant to cover: per-iteration releases inside an
emitted loop body, slot/scope primitives that sit *below* the handle
layer, values threaded down a parameter chain rather than held as a
straight-line temporary, and branch-spanning consume/release pairs
where one arm consumes a value and a different arm releases it.

### 4.5 Constraints specific to this implementation

- **Values are tagged 64-bit integers, not typed pointers.** Deciding
  whether a value is a heap pointer at all requires reading its tag.
  This, combined with raw pointers escaping to C++ interop (Tensor,
  interpreter boundary crossings), is why a moving collector using
  LLVM Statepoints was not adopted as the rooting mechanism (§6.4) —
  supporting it would require a value-representation rewrite far
  larger than this ownership discipline.
- **The two backends must remain behaviorally symmetric.** A change
  to this discipline is JIT-internal by construction and must never
  alter observable behavior, error messages, or the order in which
  checks run, relative to the interpreter.
- **Dispatch is resolved at runtime.** Method and operator targets are
  not known statically, so ownership contracts are enforced at the
  call boundary dynamically, not by the static type of a callee.
- **A whole-function IR verifier was evaluated and rejected.** Such a
  verifier would be blind to the interior of C++ helper functions,
  and "every retain is matched by a release" does not imply "every
  value was alive for exactly as long as it needed to be" — an
  early-but-balanced release can still leave a live use-after-free
  window. The block-confinement rule in §4.3 is deliberately narrower
  in scope: it reasons only about this discipline's own handles, not
  about heap aliasing or object lifetime in general, which is
  precisely what lets it sidestep the objections that sank the
  whole-function verifier.

## 5. Detecting reference-counting bugs

A reference-counting placement bug that goes undetected has a
specific, unhelpful failure signature: the tracing backstop (§6)
quietly reclaims the leaked object at its next collection, and
nothing ever surfaces the mistake. Historically this meant a
placement bug could ship silently and only become visible much later,
as elevated memory use with no clear cause.

### 5.1 A reference-accounting classifier

`InterpGC`'s `gc_refs` algorithm (§2) can be run diagnostically on the
JIT heap as an alternative to conservative stack scanning: for each
tracked object, subtract in-heap reference edges from its total
refcount. Comparing this classifier's answer against the conservative
scanner's answer (§6.2) on the same heap produces a precise diagnosis
of any object the two disagree about:

- **Conservative-dead, but `gc_refs`-retained, and reachable only
  from other tracked objects**: this is a reference cycle — expected,
  and exactly what the backstop exists to reclaim.
- **Conservative-dead, but `gc_refs`-retained, and *not* reachable
  from other tracked objects**: the object's refcount is higher than
  the number of real references pointing at it. This is a definite
  reference-counting placement bug — a missed release — with no
  possible innocent explanation.

This gives a **zero-false-positive** leak detector: the second
category can only be produced by an actual accounting error, never by
ordinary program structure.

### 5.2 Catching leaks before they ship

Built on the classifier above, a debug-and-CI-only detector captures
every object's allocation backtrace at creation and, at one carefully
chosen quiescent point in program execution — after the top-level
program has finished running, but while module and namespace roots
are still wired up, the only point where "inflated refcount and
otherwise dead" unambiguously means "leaked" rather than "not yet
observed to be reachable" — runs the classifier and aborts with the
leaked object's birth site if it finds one. This runs as a standing
phase of continuous integration over the full conformance test corpus,
including every exception-throwing code path, with an explicit
allowlist for the reference-cycle case (which is not a bug). It is
restricted to debug, non-LTO builds, because LTO's altered stack
layout under-reports conservative roots; production builds keep
reclaiming leaked objects quietly rather than aborting, since an
abort would itself be a denial-of-service vector in a shipped binary.

### 5.3 Distinguishing ownership bugs from rooting gaps

A separate diagnostic toggle disables every collection outright. A
crash that **survives** having collection disabled cannot be a
rooting problem — nothing was ever collected — so it is a pure
ownership bug: something was released while still live. A crash that
**disappears** when collection is disabled was instead a rooting gap:
some live value was not found by the root scanner and was collected
out from under its user. This toggle is the operational counterpart
to the conceptual split in §4.2, and in practice every crash
investigated this way has turned out to be an ownership bug — the
root-finding mechanisms in §2 and §6.2 have not been the source of a
real bug since both collectors shipped.

## 6. The JIT's tracing backstop

### 6.1 Why a backstop, not just the discipline in §4

The discipline in §4 makes correctly-placed retain/release the only
way to write new codegen, but it does not — and cannot — resolve
reference cycles; no RC discipline can, by construction (Rust's `Rc`
has the identical limitation). Without a reclaiming mechanism for
cycles, a leak of this shape is permanent: a single long-running
program that repeatedly builds and discards cyclic structures grows
without bound. Pure tracing (discarding manual RC entirely) was
considered and rejected for the reason given in §1: it would give up
deterministic `drop`. So RC stays primary, and this collector's only
job is reclaiming what RC structurally cannot.

### 6.2 Conservative root-finding

At the moment a collection runs, every machine word that, read as a
pointer, lands on a valid object's starting address is treated as a
candidate root. The scanner does not consult a value's tag to decide
whether to treat it as a pointer — it tests every eight-byte word
uniformly, so a heap pointer held in a tagged integer is found
regardless of what its tag says. Root sources are: the machine stack
of every mutator thread, from the stack pointer at the moment
collection runs up to the thread's stack base (covering both
JIT-generated frames and C++ runtime-helper frames, so a value held
in any C++ builtin's local variable is rooted automatically);
callee-saved registers, flushed to the stack before scanning begins;
and a set of explicitly registered global roots (the namespace table,
module caches, REPL globals, exception carriers, the defer stack).

**Why this is sound.** Any collector-tracked pointer that is live
across a call which might trigger a collection must, by the
platform's calling convention, already be either spilled to the stack
or held in a callee-saved register at the point that call is made —
there is nowhere else for the compiler to have put it. At the exact
moment a collection runs — deep inside the allocator, on the far side
of that call — every such value is therefore already found by the
stack-plus-register scan. This is the same argument that makes
Boehm-style and Ruby's conservative collectors sound even under an
optimizing compiler that keeps values in registers for extended
stretches.

A non-pointer scalar that happens to alias a valid heap address
produces a **false root** — the object it points at is kept alive
though nothing actually references it. This is a bounded
over-retention, not a correctness problem: soundness (§6.3) only
requires that nothing *live* is ever freed, and over-retention cannot
violate that. One detail cost real debugging time to get right: root
scanning must begin at the stack **pointer**, not the frame pointer —
under `-O2` and higher, locals and register spills living below the
frame pointer would otherwise be missed entirely.

### 6.3 Collection: mark and sweep

**Object model.** Every collector-tracked struct keeps its existing
`int64_t refcount` field at offset zero — the retain/release IR
emitted by codegen is untouched by any of this. The collector's own
per-object bookkeeping (mark bit, type tag, generation) lives in a
separate registry — an address-to-metadata map — rather than in the
object itself, because this is the smaller change: it reuses the
existing retain/release IR verbatim instead of rewriting every
codegen site that touches an object header. Variable-length payloads
(an array's backing storage, an object's property slots, a closure's
captured values) remain owned by ordinary C++ containers inside the
struct and are released by the struct's own destructor at sweep time,
reached by walking the object's children during marking. This keeps
the collector's own logic simple: sweep cleanup is automatic C++
destruction, not a bespoke reclaimer per type.

The registry began as a `std::unordered_map`; measurement showed its
per-entry node allocation accounted for the *entire* allocation-churn
overhead this collector added on object- and array-heavy workloads
(12–21%). Replacing it with an open-addressing flat hash map — no
per-entry heap allocation, tombstone slots reused on insert —
eliminated that overhead. A size-class or region-based allocator (the
model used by the Go runtime and JavaScriptCore) is the next
available lever if allocation throughput is later measured to need
it; it has not been pursued without that measurement.

**Mark.** Starting from the root set (§6.2), trace reachability
transitively: for each object reached, set its mark bit and push its
children onto a mark stack, to a fixed point. Because conservative
roots are pinned in place — nothing about this collector moves
objects — there is no pointer-update pass to run afterward.

**Sweep.** Walk the registry; for every object whose mark bit was not
set, run its C++ destructor exactly once and reclaim its registry
slot. A full mark-and-sweep from the actual root set reclaims any
unreachable object regardless of how stale or inconsistent any
per-object bookkeeping might otherwise have become — this is the
property the reference-counting-only design that preceded this
collector lacked, and the reason leaks used to be permanent rather
than eventually recovered.

Finalization — running each dead object's `drop` before its memory is
reclaimed — happens in a pre-sweep pass, described as part of the
`drop` model in `language.md` §17 rather than repeated here; it is
not a property of the sweep mechanism itself.

### 6.4 Why not a moving or fully precise collector

A moving collector was evaluated and rejected outright, not merely
deferred, because two of its hard prerequisites are unmet:

1. **Precise rooting.** A conservative root is, definitionally, "a
   word that might be a pointer" — such a root cannot be updated to
   point at an object's new location after a move, because the
   collector cannot be certain it was a pointer to begin with. Moving
   objects safely requires precise roots as a prerequisite, not
   something that can be added alongside conservatism.
2. **No raw pointers escaping to collector-uncooperative code.**
   Culebra hands raw pointers to C++ for Tensor and interpreter
   interop, and the tagged-integer value representation (§4.5) stuffs
   pointers into plain integers — a representation LLVM's Statepoint
   mechanism, the standard way to make a JIT's roots precise, cannot
   track. This is the same reason CPython's, Lua's, and Ruby's
   default collectors all remain non-moving.

The throughput advantage moving *uniquely* provides — compaction, and
collection cost proportional to the number of survivors rather than
the size of the heap — is a second-order concern for Culebra today; a
size-class or Immix-style bump-region allocator (§6.3) recovers most
of a non-moving collector's achievable throughput without paying
either of the two prerequisite redesigns above. Revisiting this
decision would require either measuring that heap fragmentation is a
real, material cost, or a separate redesign of raw-pointer interop
behind handles or pinning.

### 6.5 The interpreter's collector, by comparison

`InterpGC` (§2) and this collector reclaim the same cycle shapes,
which keeps the two backends behaviorally symmetric — a program that
leaks or fails to leak a given cycle shape does so identically on
either backend. The two differ entirely in mechanism: `InterpGC` is
precise because exact `shared_ptr` refcounts make a stack scan
unnecessary; this collector is conservative because the JIT's
hand-emitted RC gives it no such exactness to rely on, so it falls
back to scanning for anything that merely looks like a pointer.

Objects that cross the interpreter/JIT boundary — a `Tensor`'s
implementation, held as a `shared_ptr` regardless of which backend is
driving it — keep the same handle across a collection under either
collector, because neither collector moves anything. Raw pointers
handed to C++ interop code therefore remain valid across a collection
on both backends.

### 6.6 Safety devices

- A stress mode collects on every single allocation rather than on
  the normal adaptive schedule, surfacing rooting or marking bugs
  deterministically instead of leaving them to appear flakily under
  timing-dependent conditions (the same model SpiderMonkey's `gcZeal`
  uses); the full conformance suite runs green under this mode.
- A statistics query exposes live-object and live-byte counts for
  diagnostic use.
- A dedicated set of leak-regression tests forms the JIT's memory
  acceptance gate, mirrored from the interpreter's already-green
  baseline for the same test shapes.
- In debug builds, freed slots are poisoned with a recognizable fill
  pattern, so a use-after-free reads garbage that asserts loudly
  instead of silently returning plausible-looking data.
- A debug-build heap verifier walks every live object and confirms
  that every child pointer it holds resolves to a valid, currently
  live heap object.

The collector's own C++ implementation follows the same discipline it
enforces on generated code: its teardown is an ordinary struct
destructor, stop-the-world coordination is a scope guard, and global
root registration is scope-lifetime — no hand-paired setup/teardown
calls anywhere in its own implementation either.

### 6.7 A future generational layer

The current collector is non-generational and correct as it stands; a
generational layer (a young generation collected far more often than
an old one, with a remembered set bridging the two) is a pure
throughput optimization, worth adding only once the non-generational
baseline is measured to need it — not before. Its one hard
requirement, recorded here because it constrains any future
implementation rather than because it exists yet: every store of a
heap value into an already-old object's field must be recorded by a
write barrier in a remembered set. Without one, a minor collection
that only traces the young generation could free a young object that
is reachable solely through such an old-to-young edge.

## 7. Design lineage

The ownership discipline in §4 does not adopt any single existing
system wholesale — Culebra's codegen has no borrow checker of its
own, and it lowers a dynamic AST rather than a functional IR with
whole-program type inference, so none of the systems below transfers
directly. Each contributes one structural idea:

| System | What this design takes from it |
|---|---|
| Rust (affine ownership, `Drop`) | Move-or-consume as the fundamental value discipline; a borrow is categorically never freed |
| C++ RAII | The implementation vehicle itself — a handle whose destructor emits the release, correct automatically across early returns and exceptions |
| Swift ARC | The idea of *deriving* retain/release placement from one uniform convention, rather than hand-placing each site |
| Perceus (Koka) | The closest structural match among precise, deterministic-drop reference-counting designs |
| Nim (ORC) | Validation that "RC plus a cycle-collecting backstop" is a viable combination to ship, not merely a theoretical compromise |
| MLIR / Swift SIL ownership | The end state this design is oriented toward: ownership as a property an intermediate representation can check, not a comment-level convention a reviewer has to enforce by inspection |

The converged shape — the compiler derives every retain/release
placement from a uniform convention, and no codegen path hand-writes
one — is what §4 implements using C++ RAII handles layered over LLVM
IR generation.
