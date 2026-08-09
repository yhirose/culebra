# RAII Without Having to Think About Cycles

*August 6, 2026*

For a long time, memory management design has split into two traditions.

One is the RAII tradition, established by C++ and reinforced by Rust with
ownership and a type system. A value's lifetime is tied to a scope, and the
destructor runs the moment the last reference disappears. Files close, locks
release, buffers go back. Because "when" is visible by reading the source,
resource management and memory management can ride on a single mechanism.
The cost is that reference cycles remain the programmer's responsibility —
C++'s `weak_ptr`, Rust's `Weak`, Swift ARC's `weak` / `unowned` are all
tools for a human to declare "this one is not ownership." Forget to declare
it, and the object leaks forever.

The other is the tracing-GC tradition: Java, Go, C#, JavaScript. Whatever
becomes unreachable gets collected eventually, so cycles are a non-issue
from the start. What's lost instead is "when."
Finalizers run late, in an unspecified order, and sometimes with no
guarantee of running at all (Go's `runtime.SetFinalizer`; Java's deprecated
`finalize`). So these languages end up moving resource release onto a
separate mechanism, decoupled from memory management —
try-with-resources, `defer`, `using`, the `with` statement. The programmer
no longer has to think about lifetimes, and in exchange, for any type that
holds "something other than memory," still writes the closing by hand.

culebra tries to stand between the two. **Resource management is RAII by
default, memory management is RAII wherever it can be, and reference cycles
plus a few edge cases are handed to the GC.** Put differently: it's a
language with the deterministic destruction of C++ or Swift that has no
word corresponding to `weak` anywhere in it. The edge cases that do go to
the GC — top-level bindings, cycles formed only by closures, extremely
large release cascades — are collected in "What this costs" below.

This essay is about what that position is worth, and what it takes
internally to hold it. The implementation details live in
[`docs/internals/memory.md`](../internals/memory.md), and the observable
contract in [`language.md` §17](../language.md#17-memory-model). Where
this essay and `language.md` disagree, `language.md` wins.

## What you don't write

Start from the user's side. An object with a zero-argument `drop` property
has it called when the last reference goes away:

```culebra
{
  r = {drop: fn () {
    inspect('released')
  }}
  inspect('in scope')
}
inspect('after')
# => |
# 'in scope'
# 'released'
# 'after'
```

So far, any language with reference counting does the same thing. It gets
interesting once you build a cycle:

```culebra
let closed = []
make_node = fn (name) {
  {name: name, children: [], drop: fn () {
    closed.push(name)
  }}
}
{
  let root = make_node('root')
  let leaf = make_node('leaf')
  root.children.push(leaf)
  leaf.parent = root  # a back-reference to the parent. no weak needed
}
inspect(closed)  # => ['leaf', 'root']
```

`leaf.parent` is a reference cycle. Build the same shape with plain
`shared_ptr`, or in Swift without a `weak var parent`, and neither node is
ever released, so `drop` never runs.

In culebra, this cycle is released at the `}` where the block ends, and
`drop` runs there. Not "collected at some point" as in a GC language, but
deterministically finished. The order among cycle members is **newest
binding first**, which is why `leaf` closes before `root` here — it was
created second. (An ordinary, non-cyclic cascade goes the other way, parent
before child; §17 has the exact rules.)

The same property matters more when you look at it from the resource side.
File descriptors and database connections — unlike memory, these are
scarce, and a few hundred milliseconds of delay in releasing one is an
observable failure. culebra's standard library puts them on top of `drop`,
and only a handle you never close falls through to the GC backstop:

```culebra
# doctest: skip
# 1. scoped helper: closes on every exit path — normal, return, throw
let head = File.with('big.log', 'r', fn (f) {
  f.read(256)
})

# 2. when the lifetime is wider than one block
let f = File.open(path, 'w')
defer {
  f.close()
}

# 3. the iterator owns the handle. it closes even when you break out
for line in File.open(path).lines() {
  if line == '' {
    break
  }
  process(line)
}
```

The third one says it best. `File.open(path).lines()` is bound to no
variable at all. The handle still closes at the moment the loop is left via
`break` — and the same holds for leaving by exception. In a GC language,
this shape is one you're told not to write, because there's no telling when
the fd closes.

So the things a culebra user doesn't write are: `weak` and `unowned`;
ownership vessels like `Rc<RefCell<...>>`; and the double bookkeeping of
"the GC looks after memory but I close the files myself."

## Why there's no `weak`

Adding `weak` would have been the easier road to implement. It wasn't taken
because it is a tool that **moves responsibility for correctness onto the
user**.

Nobody sets out to write a reference cycle. A tree whose nodes point back at
their parent, a graph where an observer remembers what it observes, a cache,
a doubly-linked list, a recursive closure that captured itself in order to
call itself — every one of these is a natural design, and the cycle shows up
as a byproduct. In a language with static types and borrow checking, making
you write "this is not ownership" into the type is coherent. Demand the same
in a dynamically typed language people write like a script, and what
actually increases is the work of hunting for where the cycles are, not the
number of leaks avoided. Swift ARC — where the compiler generates every
retain and release, leaving no room for programmer error in placement —
still delegates cycles to a declaration, `weak` or `unowned`, and that is
where the leaks that remain concentrate. The structural limit shows through:
no amount of rigor in an RC discipline unties a cycle by itself.

So culebra inverted it. Cycles are **the language's problem**. The user
declares no ownership shapes and worries about no phrasing that would keep
`drop` from running. In exchange, cycle reclamation alone is delegated to a
mechanism outside RC — a tracing collector.

What matters here is which one leads. **RC decides "when," and tracing takes
on only what RC cannot do.** Never the reverse. Going pure tracing (dropping
hand-written RC entirely) was considered, but that means giving up
deterministic `drop`, and with it everything above — the file closing at the
`}`. Under shared ownership, strict reference counting is the only thing
that can give deterministic finalization: the count reaching zero *is* the
signal, and it happens at the instant the last reference is released. A GC
has no instrument that observes that instant.

## Implementation: two engines, one contract

culebra has two backends (the interpreter and the JIT; AOT uses the same
code generation as the JIT), and the same program has to produce the same
`drop` at the same point on both — the one documented exception being that
closure-captured shapes are deferred to the interpreter's next collection,
spelled out in §17. But the work each engine does to hold that agreement
turned out to be completely different.

### The interpreter: the side where the problem doesn't exist

The interpreter represents heap values as C++ `shared_ptr`. Its reference
counts are therefore strict **by construction**. Increments and decrements
are written by the compiler rather than a human, and a `shared_ptr`
destructor is not skipped by a `break`, an early `return`, or an exception
unwind. Leaks and double frees caused by RC placement aren't among the kinds
of bugs this side can have.

The only exposure left is cycles, so the collector can be built for exactly
that. The interpreter's collector uses CPython's algorithm: for each tracked
object, subtract from its refcount the number of references held by other
tracked objects (the in-heap edges); if nothing remains, nobody outside the
tracked set points at it — it's a garbage cycle. This is a **precise**
collector, and it works precisely because the `shared_ptr` counts are
exact; it never has to look at the C++ stack.

The interesting part is that what upholds that precision is not analysis but
**scheduling**. Collection never runs at an arbitrary moment; it's deferred
to the next **statement boundary**. There genuinely is a precarious instant
where a freshly built self-referential closure is in flight as a C++ return
value between two functions, reachable from no environment yet — but by the
time the next statement boundary arrives, that value has certainly been
stored into some environment. What remains is pushing the environments of
callers still live on the C++ stack onto an explicit root set, which an
RAII guard does for the duration of each call. Deciding "it never runs
inside the dangerous window" removes the need for a conservative stack scan
entirely (internals §2 has the details).

### The JIT: a world without `shared_ptr`

The JIT emits retains and releases as explicit LLVM IR, per value. That
avoids the cost of `shared_ptr`'s atomic refcount traffic on every touch,
but the choice brings back, wholesale, a problem the interpreter side never
had. Every code path the compiler generates has to place its retains and
releases correctly by hand, and there is no destructor to save you when you
miss one. There are exactly two failure shapes: **missing release** (a leak)
and **excess release** (use-after-free). And fall-through, `break`,
`continue`, early `return`, and exception unwinds crossing several scopes
all count as code paths.

Finding and fixing these one at a time doesn't converge, as it turned out.
Each fix leaves the next unaudited path just as defenseless against the same
mistake. So the approach changed: **make correctly placed retain/release the
only way to write new codegen at all.**

### Layering RAII over generated code

The tool that made that possible was the same RAII culebra offers its users,
which is the part of this design I like best. Over the C++ code that emits
LLVM IR sits a layer of ownership built out of C++ destructors. Each layer
closes one route by which a leak or a double free could happen; the main
ones (internals §4.3 has the full stack):

- **A single convention.** Every function that compiles an expression
  returns a value the caller owns one reference to (a `+1` value).
  Parameters and receivers are *borrowed* (`+0`). Rather than judging the
  placement of each retain and release individually, the placement is
  **derived** from one convention — an idea borrowed from Swift ARC.
- **A move-only handle.** A `+1` value lives in a move-only RAII handle, not
  a raw pointer. The handle offers only "read," "hand off exactly once," and
  "release right here." Consuming an already-consumed handle is a
  **compile-time abort** — a double free turned into a build failure. If it
  dies without being handed off, its destructor releases.
- **Values escaping a straight-line region are owned by a scope slot.**
  Values that live across loop iterations, or that are consumed on one
  branch and dropped on another, move into a scope slot. The existing scope
  unwind machinery releases it on *every* exit path: ordinary fall-through,
  `break` / `continue`, early `return`, exception unwind. This closed a leak
  class that had actually shipped — the `drop` of a scope-owned local used
  to never run at all when the scope was left by a `throw`.
- **Borrowed values have no release operation.** The borrowed handle type
  has no release method, so "released something I only borrowed" is not
  expressible.
- **Raw references are confined to the basic block that produced them.** The
  last escape hatch left was the instant a value is read out of a handle.
  Now the read returns a token that records the originating block, and using
  it in another block is a compile-time abort. Every call that can throw is
  compiled as an `invoke`, and an `invoke` always terminates its own block,
  so "still in the same block" is equivalent to "no unwind edge has been
  crossed since this reference was born" — that's why the rule is both sound
  and complete, and in practice every leak found recently had exactly this
  shape. The handful of shapes that genuinely do cross blocks use an
  explicitly unchecked variant; those sites are counted and never allowed to
  grow.

Stacked up, causing a leak or a double free requires **breaking one of the
C++ types or RAII invariants** — which surfaces as a compile error, not a
runtime accident. The few places where ownership genuinely has to move
across a runtime helper have a closed set of contracts, and by construction
no value is ever assigned two cleaners — that would be a double free by
definition.

Writing a whole-function IR verifier was on the table, and was rejected
after consideration. It can't see inside C++ helper functions, and "every
retain has a matching release" does not mean "every value lived exactly as
long as it had to." An early but balanced release still leaves a live
use-after-free window. That the block-confinement rule above is
deliberately narrower in scope — it reasons only about this discipline's own
handles — is also why it sidesteps the objection that sank the verifier.

### The backstop: conservative, and non-moving

No amount of discipline unties a cycle — a limit by construction that Rust's
`Rc` shares. So the JIT side has a tracing collector too. Its role is
exactly the interpreter collector's — both backends reclaiming the same
cycle shapes is part of keeping behavior symmetric — but the mechanism is
the opposite. The JIT's hand-written RC has none of `shared_ptr`'s
strictness, so a precise collector is off the table. Root finding is
**conservative** instead.

At the moment of collection, every mutator thread's machine stack and its
callee-saved registers are scanned uniformly, eight bytes at a time, and any
word landing on a valid object's start address is treated as a root. Tags
are not consulted — a heap pointer hidden inside a tagged integer is found
regardless of what the tag says. This is sound as a consequence of the
calling convention: any pointer live across a call that could trigger
collection must already be spilled to the stack or held in a callee-saved
register. It's the same argument that underwrites Boehm's and Ruby's
conservative collectors. (internals §6.2 has the scan's details and the set
of explicitly registered global roots.)

An integer that happens to match a valid heap address creates a **false
root** and keeps a dead object alive. That's bounded over-retention, not a
correctness problem — soundness only demands that live things are never
released.

The collector **moves nothing**. This is a settled decision rather than a
deferred one, for two reasons. A conservative root is by definition "a word
that might be a pointer," so it can't be rewritten to an object's new
location. And culebra hands raw pointers to C++ for Tensor and interpreter
interop, which — together with a value representation of tagged 64-bit
integers — rules out the standard way to make JIT roots precise (LLVM
Statepoints). That second reason, raw pointers escaping to code that does
not cooperate with the collector, is also why CPython's, Lua's, and Ruby's
default collectors are all non-moving. What moving uniquely buys —
compaction, and a collection cost proportional to survivors rather than heap
size — is a second-order concern for culebra as it stands.

A dead object's `drop` runs before its memory is reclaimed, while the
structure is still intact. That's the same spirit as CPython's PEP 442, and
it guarantees a finalizer never observes a half-torn object graph.

The collector's own C++ implementation follows the discipline it imposes on
generated code: its teardown is an ordinary struct destructor, the
stop-the-world coordination is a scope guard, and global root registration
is tied to a scope's lifetime. There is not one hand-paired
setup/teardown call anywhere in it.

## Having a GC makes RC bugs go quiet

"RC plus a GC backstop" has a side effect that doesn't get discussed much:
**RC placement bugs stop producing symptoms.** Place one retain too many and
the object leaks, becomes unreachable, and the next collection quietly takes
it. The program runs correctly, the tests are green, and slowly climbing
memory use is the only clue left, much later. The backstop is a safety net
and an evidence-destroying device at the same time.

So a separate mechanism exists to detect exactly that. There are two ways to
decide whether an object on the JIT heap is dead — the interpreter's
algorithm (precise reference accounting, subtracting in-heap edges from
refcounts) run over that heap for diagnostics, and the JIT's own
conservative stack scan. Looking at the objects those **two independent
opinions** disagree about yields an exact diagnosis:

- Dead by the conservative scan, alive by reference accounting, and *not*
  reachable from any other tracked object → this object's refcount is higher
  than the number of references actually pointing at it. That's a
  **definite RC placement bug**, with no innocent explanation available.
- Dead by the conservative scan, alive by reference accounting, and
  reachable from another tracked object → just a reference cycle. Expected,
  and the whole reason the backstop exists.

The first category has **zero false positives**. It cannot arise from
ordinary program structure, so one occurrence is always a bug. On top of
this classifier sits a leak detector, restricted to debug (and non-LTO)
builds and to CI: it records an allocation backtrace for every object, runs
the classifier at one carefully chosen quiescent point, and aborts with the
allocation site attached if it finds anything. That point is after the
top-level program has finished executing, while the module and namespace
roots are still wired up — the only place where "refcount in excess,
everything else dead" definitely means "leaking" rather than "merely not
observed reachable yet." This runs as a permanent
CI phase over the whole conformance corpus, exception-throwing paths
included. Shipping builds don't abort — an abort is itself a
denial-of-service vector — and quietly keep reclaiming instead.

One more diagnostic toggle disables all collection. What it exploits is that
a single refcount is really doing two separate jobs — **ownership** (when to
release) and **rooting** (what stays alive across a collection). A crash
that **remains** with collection disabled can't be a rooting problem, since
nothing is being reclaimed, so it's a pure ownership bug: something live got
released. A crash that **disappears** was, conversely, a missed root. In
practice every crash
investigated this way has been the former, and neither collector's root
finding has been the cause of a real bug since they shipped.

The collector also has a stress mode. Instead of the adaptive schedule it
collects **on every allocation**, so bugs that otherwise appear at the whim
of timing surface deterministically (the same idea as SpiderMonkey's
`gcZeal`). The full conformance suite runs green under it.

## What this costs

The position isn't free.

**Top-level bindings are not dropped.** Bind an object with a `drop` at the
top level and it survives to program exit on every backend, with `drop`
never running. The interpreter's top-level environment is one large cycle —
the functions bound there capture it themselves — so it is never destroyed,
and the JIT and AOT deliberately suppress the drop to match. Program-wide
resources want `defer` or explicit cleanup. This lands on the same side as
Rust, whose `static` items are never dropped — Rust does not guarantee that
destructors run at all (`mem::forget` isn't even `unsafe`), on the reasoning
that exit-time cleanup is not the language's job. Swift's globals are
likewise never deinitialized. Where culebra parts from them is not here but
on cycles, which it takes on itself.

**Closure-only cycles and extremely large cascades go to the GC.** A cycle
formed only by closures with no back-reference from the resource (a
recursive local `fn` capturing it, say) falls outside the deterministic
window and fires on the backstop's timing — collection-driven, forceable
with `GC.stat()`. The same applies to the excess when a single scope
releases thousands of cycle-owned objects at once, since the cascade has a
size cap tuned to keep ordinary programs synchronous. Finalization order is
unspecified for both, and an orphan that survives to program exit has its
memory reclaimed without `drop` running at all. Determinism comes with a
footnote naming the shapes in this section.

**Conservative scanning over-retains, and moving collection is off the table
for good.** False roots are bounded but real, so something dead can linger
an extra collection (nothing live is ever released by mistake). The
non-moving decision only reopens if fragmentation is measured to cost, or
the raw-pointer interop moves behind handles or pinning.

**The runtime cost is paid twice.** Carrying two mechanisms means carrying
both refcount traffic on every touch of a value and a periodic mark-sweep;
a language with only one of them pays only one of those. And the tracing
side is non-generational, so every collection walks the whole live heap
where a generational minor collection would trace only the young generation
— that layer is pure throughput optimization, so the policy is to add it
only once the non-generational baseline is measured to be insufficient. The
JIT abandoning `shared_ptr` and emitting retains and releases as explicit IR
is an attempt to cut one half of that double cost — the atomic refcount
operation on every touch — and the ownership discipline described above is
what it pays in exchange. Part of the collector's own overhead was measured
and removed: the allocation churn of the registry holding per-object
bookkeeping (12–21%) went away when it moved to an open-addressing flat hash
map (internals §6.3).

And finally, the biggest one: **the implementation costs a lot of code and
complexity** — the cost is in building it, not in how fast it runs. A
GC-only language needs one collector. An RC-only language just makes the
user write `weak`. culebra has both, has to make two backends whose
mechanisms are opposites agree on the behavior, and carries a detector on
top to cover the "the GC hides RC bugs" problem. The one word `weak` that
users no longer write shows up as all of this structure on the
implementation side. The reason it still seems worth it is that a forgotten
`weak` fails quietly and irreproducibly in a user's program, whereas the
implementation's complexity can be concentrated in one place and checked by
CI every day. That discipline is counted, too: the hand-written
retain/release calls still outside it are tracked as audited migration debt
and are not allowed to grow.

## Lineage

None of this is any one system adopted whole: culebra's codegen has no
borrow checker of its own, and it lowers a dynamic AST rather than a
functional IR with whole-program type inference, so none of the systems
below transfers as-is. It borrows one structural idea from each.

| Source | What was borrowed |
|---|---|
| C++ RAII | The implementation vehicle itself. A handle whose destructor emits the release is automatically correct across early returns and exceptions |
| Rust (affine ownership, `Drop`) | Making move-or-consume the basic discipline for a value. Borrows are, by classification, never released |
| Swift ARC | *Deriving* retain/release placement from one coherent convention instead of judging each site individually |
| CPython | Precise cycle detection (the `gc_refs` subtraction) and PEP 442's ordering, finalizing before the sweep |
| Perceus (Koka) | The closest structural match among precise, deterministic-drop reference-counting designs |
| Nim (ORC) | Confirmation that "RC as primary + a cycle-collecting backstop" is a shippable combination, not a theoretical compromise |
| Boehm GC, Ruby | The argument that a conservative stack scan stays sound under an optimizing compiler |
| SpiderMonkey (`gcZeal`) | The stress mode that collects on every allocation, turning timing-dependent bugs deterministic |
| MLIR / Swift SIL ownership | The destination it aims at: ownership as a property an intermediate representation can check, not a comment-level promise reviewers uphold by eye |

## In sum

culebra's memory management doesn't answer the question of whether RAII or
GC is right. It's a design that dodges the question: **resources need RAII,
cycles need a GC, so carry both.** What the user sees is just two things —
the file closes at the `}`, and pointing back at the parent does nothing at
all.

If the machinery behind that has a one-line summary, it's that **the RAII
handed to users is also what the implementation is built out of.** Whether
the design holds together is probably a question of how far that nesting can
be sustained.
