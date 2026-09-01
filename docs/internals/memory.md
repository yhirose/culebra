Memory and Resource Management: Implementation
===============================================

This document describes how Culebra's memory management is
implemented. It is not a specification: the observable contract —
what a program can rely on — is normative in
[`language.md` §17](../language.md#17-memory-model) and
[§25](../language.md#25-appendix-vm--jit-divergence). This
document explains *how* that contract is met and why the implementation
takes the shape it does. Where this document and `language.md` disagree,
`language.md` wins.

The Japanese mirror of this file is
[`memory.ja.md`](memory.ja.md). For why the design stands where it
does — deterministic destruction with no `weak` in the language —
see the essay [`essays/memory.md`](../essays/memory.md). The engine
this runtime serves is described in [`vm.md`](vm.md).

Contents
--------

1. [Reference counting primary, tracing backstop](#1-reference-counting-primary-tracing-backstop)
2. [Where reference counts are placed](#2-where-reference-counts-are-placed)
3. [The value model](#3-the-value-model)
4. [The ownership discipline of the LLVM lowering](#4-the-ownership-discipline-of-the-llvm-lowering)
5. [Detecting reference-counting bugs](#5-detecting-reference-counting-bugs)
6. [The tracing backstop](#6-the-tracing-backstop)
7. [Design lineage](#7-design-lineage)

---

## 1. Reference counting primary, tracing backstop

Culebra's memory management is **reference counting (RC), primary,
with a tracing collector as a backstop**. The two engine lanes — the
bytecode executor and the LLVM lowering — share one runtime, one heap
and one collector. Neither half of the design is retirable in favor of
the other:

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

So the two mechanisms answer two different questions. RC decides *when*
an object dies and drives `drop` timing. The tracing collector
answers *what RC cannot*: it reclaims cycles, and — as a backstop —
anything a reference-counting placement bug would otherwise leak
permanently.

Manual reference counting has exactly two ways to go wrong:

- **A missed release** (a leak): an object outlives every reference
  to it because some code path failed to decrement.
- **An extra release** (a use-after-free / double-free): an object is
  freed while a live reference to it still exists, because some code
  path decremented a reference it never owned.

Getting placement right on every control-flow path — fall-through,
`break`, `continue`, an early `return`, an exception unwinding through
several scopes — by hand, is hard. The rest of this document is about
the three places placement happens and how each is kept correct.

## 2. Where reference counts are placed

There is no `shared_ptr` anywhere on the hot path: retain and release
are explicit operations, placed by three different authors.

**The bytecode compiler.** `vm::Compiler` emits `Retain` / `Release` /
`Take` into the instruction stream, and both consumers — the executor
and the LLVM lowering — merely execute them. The compiler's placement
is therefore the one that matters most, and it is verified once:
every expression leaves a `+1` in its result register, a statement's
temporaries are swept at its end, and a throw is torn down from
per-chunk unwind tables (the scopes' slot ranges and the in-flight
temporaries at each pc) under the *borrow operand contract* — an
operator or helper borrows its operands, so a throw leaves every
register frame-owned and the tables are the single releaser.
[`vm.md`](vm.md) §5.2 and §5.5 describe the format; the gates in §5 of
this document verify the placement on both lanes.

**The LLVM lowering's own codegen.** Lowering one bytecode instruction
often means several LLVM values in flight — a helper's result held
across another call, a value handed into a container, a receiver
borrowed while its method runs. Those transient `+1`s are C++ objects
inside `struct JIT` while the IR is being built, and their releases
are emitted by the lowering, not by the compiler. §4 is the discipline
that makes those placements correct by construction.

**The runtime helpers.** A `culebra_runtime_*` helper that is handed a
value follows one of a closed set of ownership contracts (§4.3), and
its C++ uses two RAII forms: `JitOwnedVal` (an owned local released on
every exit unless consumed) and `JitUnwindRelease` (a release that runs
only if the helper throws).

## 3. The value model

A value is 16 bytes, `JitValue { int64_t tag; int64_t data; }`.
Refcounted heap structs — `JitClosure`, `JitArray`, `JitObject`, tuples,
sets, tensors, and the `JitCell` that boxes a captured variable — keep
their `int64_t refcount` at offset zero, so a retain or release is one
memory operation whichever lane performs it. Strings are the exception:
`TAG_STRING` and `TAG_STRINGVIEW` carry an inline length header where a
refcount would be, and are reclaimed by the tracing collector alone
(§6). Nil, Bool, Long and Float are immediates.

Objects are allocated from a per-`Runtime` slab allocator
(`jit_slab.h`): size-segregated, carving 64 KB chunks, non-moving, and
lock-free because a `Runtime` — and with it the heap, the collector, the
defer stack and the exception carrier — is one per thread. The
collector's own per-object metadata lives outside the object in an
address-keyed registry (§6.3), so the retain/release code never touches
a header it does not own.

Deterministic `drop` rides on the same count: an object whose refcount
reaches zero runs its `drop` before its children are released. Objects
that have a `drop` are also registered on a per-`Runtime` *owned stack*
(`jit_owned.h`) from the moment `drop` is bound; a scope exit resolves
the entries above its mark, so a drop-having object that escaped into a
cycle inside the scope is still dropped at the scope boundary
(`culebra_runtime_owned_scope_exit`). The full contract — timing, order,
suppression at program exit — is `language.md` §17.

## 4. The ownership discipline of the LLVM lowering

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
justify in review, not a valid implementation choice;
`tools/checks/check_rc_discipline.sh` ratchets the count of such sites in
`jit.h` so it can only shrink.

### 4.2 Ownership and rooting are different jobs

A single refcount is doing two independent jobs at once, and
conflating them is what makes a naive "this retain looks redundant,
remove it" edit dangerous:

1. **Ownership / release** decides *when* an object is freed
   (release-to-zero, which is also what fires `drop`).
2. **Rooting / liveness** decides *what stays alive across a
   collection*.

The second job is handled, independently of any single retain, by the
collector's conservative root scan (§6.2): a value held in a stack frame
or a register window is a root purely by virtue of being there —
provided the refcount accounting is accurate, which is exactly what the
discipline below establishes. Removing a "redundant-looking" retain is
therefore never a rooting question; it requires a complete accounting
proof — tracing every producer and consumer of that value's reference
count — never the addition of new rooting machinery.

A standing diagnostic makes the two failure modes distinguishable:
`CULEBRA_GC_NEVER=1` disables every collection. A crash that
**survives** it cannot be a rooting problem — nothing was collected — so
it is an over-release; a crash that **disappears** was a rooting gap.
Every crash investigated this way has turned out to be the former.

### 4.3 The layered design

Each layer below removes exactly one way to leak or double-free.
Combined, producing a leak or double-free requires breaking a C++
type or RAII invariant — a compile error, not a silent runtime
accident.

**Uniform convention.** Every function that lowers an expression
returns a value the caller owns one reference to (a `+1` value). A
parameter or a method receiver is *borrowed* (`+0`): the caller retains
before the call, and the callee consumes the reference it is handed
rather than retaining its own copy. A value stored into a scope slot is
owned by that slot from that point on.

**A move-only handle for transient values (`JIT::Owned`).** A `+1`
value produced by lowering one instruction and consumed by the next is
held in a move-only RAII handle, not a bare pointer. The handle exposes
three operations: `borrow()` (read without consuming), `consume()`
(hand the reference onward exactly once — into a slot, a call, or a
return — marking the handle spent), or `drop()` (release immediately
for a value that dies rather than being handed on). **Consuming an
already-spent handle is a compile-time abort** — a double-free is
turned into a build failure. If the handle is still holding a reference
when it goes out of scope, its destructor releases it. This handle only
covers a *straight-line* region of code; a value that must survive a
loop iteration, or that is consumed on one branch and released on
another, needs the next layer.

**Scope slots for values that escape a straight-line region.** A
value that must outlive its originating region is consumed into a
scope slot. The scope-unwind machinery then releases that slot on
*every* exit path — normal fall-through, `break`/`continue`, an early
`return`, and exception unwind (the last via a per-region cleanup
landing pad, eliminated when the region provably cannot throw). The
same mechanism covers every scope-like region uniformly — lexical
blocks, loop bodies, `match`/`try` bodies, the `for` loop's iterable
scope, and the function frame itself.

**The function frame's unwind path is a ladder, not one pad.** The
frame is the region every throwing call falls back to, so a single
landing pad for it would collect one unwind edge per call and read
every slot in the frame — which asks the compiler for a merge value
per slot per edge, quadratic in the size of the function. Instead the
frame's teardown is split into a rung per owned binding: a call
unwinds to the rung for the bindings alive where it stands, and that
rung releases its own slot before falling into the one below. A rung
is only reachable from after its binding's initializing store, so no
merge values are needed at all, and the release order down the ladder
is the same reverse-declaration order the normal exit uses. The top
level is an ordinary frame here — a program's own bindings are
released when an uncaught error leaves it, just like a function's.

**The exception object is a resource too, and only the pad that ends
the throw holds it.** A landing pad that *handles* an exception opens it
through the C++ ABI (`__cxa_begin_catch`) and closes it again
(`__cxa_end_catch`) — that pairing is what frees the exception object,
since the ABI reclaims it when the handler count returns to zero. A pad
that only runs a region's releases and passes the throw on never opens
it: the count travels untouched from the throw to the pad that ends it,
so a cleanup pad has nothing to balance and no way to strand anything.
That is also how Clang emits a cleanup, and it keeps the cost of a
throw proportional to the frames it crosses rather than to the pads on
the way.

The two kinds are built by one emitter, which is where the choice is
made. A cleanup region — its landing pad, its releases, and the single
edge that carries the throw onward — is one object (`JIT::CleanupPad`)
whose destructor emits that edge, so a region cannot be opened without
being continued; a handler is opened by `emit_handler_prologue` and
closed on the handled arm or, when it re-raises, on the rethrow's own
unwind edge (`emit_handler_rethrow`). A missing pairing would be
invisible to every value-level leak check, because what is stranded is
the C++ exception object rather than a Culebra value, so
`tools/checks/check_eh_balance.sh` reads the emitted IR instead: only a
handler prologue may open an exception, and every rethrow has an unwind
edge that closes it.

**Borrowed values expose no release operation.** A borrowed value's
handle offers only the read operation; there is no API that both
borrows and releases the same value. "A borrowed operand was
released" is therefore not a representable bug — the type system has
no method call that could cause it.

**The automatic unwind-temp window.** The `Owned` handle releases
through a C++ destructor — which a runtime, LLVM-level `throw` cannot
run, because it unwinds through generated code, not through C++ stack
frames. Any value held in such a handle, live across a call that could
throw, would leak on the exception path unless a guard had been
hand-placed for that exact shape — and no finite test corpus enumerates
every shape. The unwind edge is therefore structural instead of
enumerated: every live handle registers itself with the `JIT` as it is
created; `emit_call` turns every may-throw call into an `invoke`, and
around it (`open_unwind_window` / `close_unwind_window`) every
still-live, not-otherwise-covered handle is spilled into a per-function
cleanup slot that every cleanup path releases first. A value already
covered by one of the contracts below is marked as such and skipped,
which is what prevents the window from double-freeing it.

**Ownership contracts for values crossing a helper call.** A value
whose ownership must transfer across a call into a runtime helper
follows exactly one of a closed set of contracts — never more than
one, since two cleaners assigned to the same value is a double-free
by construction:

| Contract | Who releases on the unwind path | Used for |
|---|---|---|
| Caller-cleans | A per-region cleanup pad at the call site (`ThrowGuard`), eliminated when the region cannot throw | Builtin-method receivers and arguments, a call's callee expression, an assignment's target, the UFCS callee |
| Callee-cleans-on-throw | A guard covering the helper's whole body (`JitUnwindRelease`), so every throw releases the caller's temps — the user-dispatch window's and the direct error's alike | Operator implementations, index/property access under receiver ownership, the not-a-function error path |
| Callee-consumes-on-every-exit | An owned-argument handle declared at the helper's entry (`JitOwnedVal`), releasing on both normal return and unwind | Native method entry points, higher-order-function accumulators and callbacks |
| Borrow-neutral invoke | The invoker mints the reference the callee consumes and touches nothing else, so the call is refcount-neutral whether the body returns or throws | Every window where user-defined dispatch runs — operator overloads, `eq`/`hash`/`cmp`, `__index__`, property getters |
| Transfer | The incoming reference is returned as-is, or handed directly into a capture cell or slot | Iterator-self methods, a lazily-built combinator's capture cell |

The last two rows are one rule seen from both sides, and the split
matters: a dispatch window is often handed a value the *container*
owns — an array's slots while its comparator runs, a dictionary's
stored key during a lookup — where there is no caller temp to clean at
all. An invoker that released on the unwind path would free those
elements out from under their owner, so cleanup belongs to whoever
minted the `+1`, never to the shared dispatch helper.

**A bare reference is confined to the block where it was produced
(`Pinned`).** The last escape hatch is the moment a reference leaves
its handle. Reading a value out of an `Owned` used to yield a bare LLVM
value that could cross basic-block boundaries with nothing tracking it
— every leak found in that era had exactly this shape. The rule
adopted instead: a bare reference may only be used inside the basic
block in which it was produced. This is sound and complete because
every call that might throw is compiled as an `invoke`, which always
terminates its containing block — so "still in the same block" already
implies "no unwind edge has run since the reference was produced."
`consume()` returns a `Pinned` token that records its origin block;
using it in another block aborts in every build configuration
(`rc_pin_violation`), so the mistake is caught the moment the offending
code is *compiled*, not merely when it happens to run. A small number of
genuinely block-crossing shapes (mutually exclusive dispatch arms
converging on a phi — `OwnedPhi`; a value already owned by a scope
slot; the prologue transfer that declares a local) use
`consume_unchecked()`, each with its own declared releaser; the count
of such sites is ratcheted. Every function that lowers a sub-expression
returns one of these handles (or an empty one on failure) rather than
a bare reference — so a bare `+1` never crosses a C++ function-return
boundary between lowering steps.

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
5. Rooting is provided by the collector's root scan, independent of
   ownership refcounts (§4.2).
6. Cycles and any residual leak are reclaimed by the tracing backstop
   (§6); the backstop is not relied on for steady-state memory
   behavior — with ownership correct, it does real work only rarely.
7. Every handle live across a call that might throw has exactly one
   releaser on the unwind path: the automatic spill window by
   default, or a declared ownership contract — never both.
8. A bare reference exists only inside the block where it was
   produced; every deliberate exception is a declared, audited site.
9. No function that lowers a sub-expression returns a bare reference
   across its own return boundary.

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
  engine boundary crossings), is why a moving collector using
  LLVM Statepoints was not adopted as the rooting mechanism (§6.4) —
  supporting it would require a value-representation rewrite far
  larger than this ownership discipline.
- **The two lanes must remain behaviorally symmetric.** A change
  to this discipline is internal by construction and must never
  alter observable behavior, error messages, or the order in which
  checks run, relative to the executor and the released binary the
  gates diff against.
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
nothing ever surfaces the mistake. The detectors below exist so that a
placement bug fails a gate instead of shipping as elevated memory use.

### 5.1 A reference-accounting classifier

CPython's `gc_refs` algorithm can be run over this heap
(`Heap::compute_gc_refs`): for each tracked object, start from its
refcount and subtract one for every reference held by another tracked
object (found by walking children); what remains is the count
contributed by things *outside* the heap — stack values, globals,
borrows. `CULEBRA_GC_REFS=1` makes this the collector's root-finding
(seed from objects with a positive residue, no stack scan at all), and
comparing its answer against the conservative scanner's on the same
heap classifies every object the two disagree about:

- **Conservative-dead, `gc_refs`-retained, and reachable only from
  other tracked objects**: a reference cycle — expected, and exactly
  what the backstop exists to reclaim.
- **Conservative-dead, `gc_refs`-retained, and *not* reachable from
  other tracked objects**: the object's refcount is higher than the
  number of real references pointing at it. This is a definite
  placement bug — a missed release — with no innocent explanation.

The second category is a **zero-false-positive** leak detector: only an
accounting error can produce it. `tools/analysis/gc_leak_check.sh`
(the rc-leak battery in `just test`) runs a pattern file both ways and
compares live counts.

### 5.2 Catching leaks before they ship

`CULEBRA_GC_LEAK_ABORT=1` arms a debug-and-CI-only detector: every
object records its allocation backtrace at birth, and at one quiescent
point — after the top-level program has finished but while module and
namespace roots are still wired up, the only point where "inflated
refcount and otherwise dead" unambiguously means "leaked" rather than
"not yet observed to be reachable" — `Heap::maybe_audit_leaks` runs the
classifier and aborts with the leaked object's birth site if it finds
one. `tools/difftest/leak_abort_suite.sh` runs the whole generated
corpus under it, throw paths included, on both lanes; every case that
aborts must be named in `leak_abort_allow.txt`, and that list is empty.
The audit needs a non-LTO build, because LTO's altered stack layout
under-reports conservative roots; production builds keep reclaiming
leaked objects quietly rather than aborting, since an abort would be a
denial-of-service vector in a shipped binary.

### 5.3 Growth-based leak fuzzing

`tools/difftest/leak.sh` (leak-fuzz in `just test`) runs each corpus
case N times under `CULEBRA_GC_NEVER=1` — backstop off, so an RC leak
cannot be masked — and measures `GC.stat().rc_objects` growth per lane.
The judgment is an absolute per lane, recorded in `leak_baseline.txt` as
a regression baseline: a case that grows and is not listed fails, a
listed case that stops growing is reported so the file can shrink. The
entries it holds are reference cycles the program itself writes (`a.me =
a` and its spellings, mutually recursive `fn` declarations) — permanent
by definition, since a cycle leaks with the cycle collector off. A new
entry is classified before it is filed by cutting one edge of the
program: a cycle's growth falls to zero, a leaked retain's does not.
This gate discards cases whose warm-up throws, which is why §5.2 exists
beside it.

### 5.4 The other lanes

`CULEBRA_GC_STRESS=1` collects on every allocation (SpiderMonkey's
`gcZeal` model), so a rooting or marking bug surfaces deterministically;
the symmetry sweep and `vm_cases` run under it. `just test-assert`
builds without `NDEBUG` and runs the same sweep, because every other
lane is a `Release` build in which the runtime's assertions never
execute. `CULEBRA_GC_NEVER=1` is the diagnostic discriminator of §4.2.

## 6. The tracing backstop

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
job is reclaiming what RC structurally cannot — plus strings, which are
traced-only (§3).

### 6.2 Conservative root-finding

At the moment a collection runs (`Heap::scan_roots`), every machine
word that, read as a pointer, lands on a valid object's starting
address is treated as a candidate root. The scanner does not consult a
value's tag — it tests every eight-byte word uniformly, so a heap
pointer held in a tagged integer is found regardless of what its tag
says. Root sources are: the machine stack of the mutator thread, from
the stack pointer at the moment collection runs up to the thread's
stack base (`Heap::stack_base`; this covers lowered frames, the
executor's register windows, and every C++ helper's locals);
callee-saved registers, flushed to the stack by a `setjmp` before
scanning begins; and the explicitly registered roots outside any stack
— the namespace tables, module caches, REPL session cells, the defer
stack, the exception carrier, multimethod bodies — enumerated by the
runtime's root function (`set_extra_roots_fn`) or pinned individually
(`Heap::pin`).

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
stretches. Where it does not hold — wasm — the collector changes
strategy (§6.5).

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

**Object model.** Every collector-tracked struct keeps its `int64_t
refcount` at offset zero — the retain/release code is untouched by
any of this. The collector's own bookkeeping (mark bit, type tag,
size) lives in a separate registry, `gc::GcRegistry`, an
open-addressing address→header map with no per-entry heap allocation;
the `std::unordered_map` it replaced accounted for the entire
allocation-churn overhead the collector added on object-heavy
workloads. Variable-length payloads (an array's backing storage, an
object's property slots, a closure's captures) remain owned by
ordinary C++ containers inside the struct and are released by the
struct's own destructor at sweep time, so sweep cleanup is C++
destruction, not a bespoke reclaimer per type.

**Triggers.** `Heap::maybe_collect` runs on every registration, with
two thresholds: a count-based one (allocations since the last collect,
amortized against the live set — tuned for a heap RC keeps small) and a
bytes-based one (a growth factor over the post-collect live bytes —
required for traced-only strings, whose only reclaimer this is).

**Mark.** Starting from the root set (§6.2), trace reachability
transitively through each type's child enumerator to a fixed point.
Because conservative roots are pinned in place — nothing moves — there
is no pointer-update pass afterward.

**Finalize, then sweep.** Before anything is freed, the runtime's
finalize hook runs each unmarked object's `drop` exactly once, with the
whole dead set still intact and its refcounts pinned so a drop body
cannot free a sibling ahead of its sweep (`language.md` §17 states the
contract). Then the registry is walked and every unmarked object's
destructor runs and its slot is reclaimed. A full mark-and-sweep from
the actual root set reclaims any unreachable object regardless of how
stale its refcount is — the property that makes leaks recoverable
rather than permanent.

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
   Culebra hands raw pointers to C++ for Tensor and host
   interop, and the tagged-integer value representation (§4.5) stuffs
   pointers into plain integers — a representation LLVM's Statepoint
   mechanism, the standard way to make a JIT's roots precise, cannot
   track. This is the same reason CPython's, Lua's, and Ruby's
   default collectors all remain non-moving.

The throughput advantage moving *uniquely* provides — compaction, and
collection cost proportional to the number of survivors rather than
the size of the heap — is a second-order concern for Culebra today;
the slab allocator (§3) recovers most of a non-moving collector's
achievable throughput without paying either prerequisite. Revisiting
this decision would require either measuring that heap fragmentation
is a real, material cost, or a separate redesign of raw-pointer
interop behind handles or pinning.

`CULEBRA_GC_PRECISE_ONLY=1` and `CULEBRA_GC_ORACLE=1` are development
modes for that question: the first collects from the precise
(non-stack) roots alone, the second measures how much of the
conservative live set the precise roots already cover.

### 6.5 Safepoints on wasm

On wasm the soundness argument of §6.2 is false: wasm locals — the
register file values actually live in — sit outside linear memory, and
`setjmp` spills nothing there. An allocation site is therefore never a
safe place to collect: the object just allocated, and any value a
runtime helper holds only in a local, are invisible to the scan.

Under `gc::kDeferToSafepoint` (on for `__EMSCRIPTEN__` only), no
collection runs inline. A threshold trip sets a pending flag, and the
bytecode executor polls `Heap::safepoint_collect()` at its instruction
boundaries, where every live value of every frame sits in a register
window — a linear-memory array the scan does see. One hazard survives
the move: a helper suspended *between* two VM frames (an iterator op
whose callback is running) may hold the only reference in its own
locals. The invariant is **no collection while any such frame is on
the stack**, enforced at one choke point: every helper-to-user call
passes through `_jit_invoke` (`jit_value.h`), whose
`SafepointUnsafeScope` defers the poll for the call's duration. The
dispatch sites audited to keep callee, receiver and arguments in
registers for the whole call use `_jit_invoke_rooted` and stay
collectable — which is what keeps a `Canvas.run` game loop collecting
every tick. The marking is fail-safe by construction: an unaudited new
call path only defers collection until the next eligible poll; memory
grows, nothing is freed wrongly. Explicit collects (`GC.stat`) gate the
same way. On native builds the whole protocol folds away.

### 6.6 Safety devices

- `CULEBRA_GC_STRESS=1` collects on every allocation (§5.4).
- `GC.stat()` exposes `live_objects`, `rc_objects` (refcounted objects
  only — the count the leak fuzzer watches) and `heap_bytes` for
  diagnostic use; `CULEBRA_GC_BIRTH_SITE=1` records an allocation
  backtrace per object for the leak audit's report.
- The leak-regression gates of §5 form the memory acceptance gate.

The collector's own C++ implementation follows the same discipline it
enforces on generated code: its teardown is an ordinary struct
destructor, stop-the-world coordination is a scope guard
(`CollectPause`), and global root registration is scope-lifetime — no
hand-paired setup/teardown calls anywhere in its own implementation
either.

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
own, and it lowers a dynamically typed bytecode rather than a
functional IR with whole-program type inference, so none of the
systems below transfers directly. Each contributes one structural
idea:

| System | What this design takes from it |
|---|---|
| Rust (affine ownership, `Drop`) | Move-or-consume as the fundamental value discipline; a borrow is categorically never freed |
| C++ RAII | The implementation vehicle itself — a handle whose destructor emits the release, correct automatically across early returns and exceptions |
| Swift ARC | The idea of *deriving* retain/release placement from one uniform convention, rather than hand-placing each site |
| CPython (`gc_refs`) | The reference-accounting classifier of §5.1, and the pre-sweep finalization pass (PEP 442) |
| Perceus (Koka) | The closest structural match among precise, deterministic-drop reference-counting designs |
| Nim (ORC) | Validation that "RC plus a cycle-collecting backstop" is a viable combination to ship, not merely a theoretical compromise |
| Boehm, Ruby | The conservative root scan of §6.2 and the soundness argument behind it |
| MLIR / Swift SIL ownership | The end state this design is oriented toward: ownership as a property an intermediate representation can check, not a comment-level convention a reviewer has to enforce by inspection |

The converged shape — the compiler derives every retain/release
placement from a uniform convention, and no codegen path hand-writes
one — is what the bytecode compiler does for the instruction stream and
what §4 implements for the lowering, using C++ RAII handles layered over
LLVM IR generation.
