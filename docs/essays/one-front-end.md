# Retiring a Tree-Walking Interpreter

*September 1, 2026*

culebra ran on a tree-walking interpreter for nine years; it is in the first
commit, from 2017. An LLVM JIT joined it in May 2026 and walked the same
syntax tree a second time, deciding every rule of the language again in its
own way. That arrangement lasted about three months. In August 2026 both
were replaced by a single front end — a compiler to bytecode — with two
consumers: an executor, and a lowering to LLVM IR. The interpreter was
deleted four days after the switch shipped.

This essay is about why that seemed worth doing, what it cost, and what had
to be true before deleting 24,000 lines of working code was a defensible
thing to do. The architecture itself is documented in
[`internals/vm.md`](../internals/vm.md); what follows is the reasoning
around it, for anyone standing at the same fork.

## Why two engines did not last

The first reason was not speed. It was that a language rule had two homes.

Adding a standard-library method meant adding it twice. A change to how a
call resolves its receiver meant changing two resolvers that had been
written months apart by different reasoning. And the two would drift, in
ways nobody noticed until a generated test corpus happened to hit the shape.
The interpreter evaluated a UFCS call's receiver before its arguments; the
JIT, on one particular path, compiled the arguments first. So a program
whose argument had a side effect ran in a different order depending on the
flag you passed:

```culebra
# doctest: skip
let g = 5
(5).g(boom())     # the interpreter: a member TypeError, boom never runs
                  # the JIT:         boom() ran first
```

That is a small bug. There were many small bugs of exactly that kind, and
each one had to be found, because nothing about the structure prevented
them.

The second reason is that the field had already answered this question, and
culebra had arrived at the unusual shape by an ordinary route: write the
interpreter first, want speed later, bolt a compiler onto the same tree.
Every mature dynamic-language implementation that runs both an interpreter
and a compiler puts a bytecode between the source and both of them —
CPython, Ruby with YARV and YJIT, Lua and LuaJIT, V8 with Ignition and
TurboFan, SpiderMonkey. These are projects with far more people and far more
at stake than this one, and they independently converged on the same shape.
When your design is the outlier against that much prior art, the burden of
proof is on the outlier.

## Deciding it in code rather than in argument

A proposal like this can be discussed indefinitely, because both sides are
speculating about a system nobody has built. So the design document
committed to two questions that could be answered by building one construct
— a counted `for` loop — and nothing else:

- **Q1** — is a bytecode-to-LLVM lowering significantly smaller and simpler
  than the AST-to-LLVM code generator it would replace?
- **Q2** — does a bytecode VM beat the tree-walker by at least 1.5×?

If either answer was no, the plan was to write down why in the design
document and stop. The spike ran on 2026-08-09 and both answers were yes:
the VM was about 25× faster on the benchmark, and the lowering for the whole
slice came to 176 lines against roughly 183 load-bearing lines for the
AST path's `for` alone.

The more useful part of that write-up is the part that argues against
itself. The 1.5× bar turned out to have almost no discriminating power,
because the tree-walker's counted loop allocated a scope and inserted into a
name map on every single iteration — the bar was going to clear itself. The
number worth carrying forward was not 25× but the 14 ns per iteration
dispatch floor. And four caveats were recorded in the same commit: every
value in the slice was an integer, so the reference-counting discipline the
format was supposed to prove was only vacuously exercised; the slice threw
no exceptions, so the lowering emitted no landing pads and part of its size
advantage was going to be spent on them later; evaluation order was untested
because the slice had no side effects; and the slot allocator was a
placeholder for an analysis pass that did not exist yet.

A spike that only records its wins is not evidence. Writing down what it did
*not* prove is what makes the verdict usable three weeks later, when you are
deciding whether the thing you actually built matches the thing you
measured.

## What it cost

Three things, and the first two were the easy ones to accept.

**Compilation got slower.** At `-O2`, `--jit` compiles about 1.2× slower
than the AST code generator did. The reason turned out to be worth
understanding rather than fixing. A value that is live when an exception
passes through has to be spilled to the stack, because the unwinder restores
callee-saved registers and nothing else. The old code generator's scope
slots were never SSA values, so its cleanup paths cost nothing to keep
alive. The lowering promotes the bytecode's register file to SSA — and that
promotion is precisely what makes loops run 2–4.5× faster. The compile-time
bill and the run-time win are the same fact. Four different shapes of
cleanup code were tried; the one that minimizes IR produces 700-wide phi
nodes that take the backend thirty seconds. So the 1.2× stays, documented as
a known cost rather than an open bug.

**Startup got slower for one shape of program.** A script that names several
lazy standard-library modules and then does nothing starts about 7 ms later,
because the preamble is now compiled instead of walked. Any program that
does real work makes that back immediately. The mitigation — caching the
preamble's bytecode — was deliberately not built, because the only programs
that would feel it are the ones doing nothing.

**And the expensive one: the second opinion.** Two independently written
implementations of a language are a test oracle. Run the same generated
program through both and any disagreement is a bug in one of them, without
anyone having to say in advance what the right answer was. That is how the
UFCS ordering bug above was found, and dozens like it. After the change,
both consumers are handed bytecode by the same compiler — so a bug in that
compiler produces the same wrong answer on both lanes, and the differential
gate stays green while the language quietly changes.

## What replaces an oracle

This is the part of the decision that generalizes least and matters most, so
it is worth being explicit about the options the field offers.

There is an **executable specification independent of any implementation**:
test262 for JavaScript, ruby/spec, the WebAssembly spec tests, Zig's
behavior tests. This is the only kind of oracle that does not rot, because
it encodes intent rather than the behavior of some particular binary.

There is **checking against a sibling implementation**: SQLite's SQL Logic
Test running the same queries through other database engines, differential
fuzzing between JavaScript engines. It is enormously effective and it is
simply unavailable to a language with one implementation and no standard.

And there is **checking a version against the version before it**: Rust's
crater runs the ecosystem against a new compiler and reports what changed.
culebra has no ecosystem to run, but it does have its own releases.

So the interpreter's role was handed to the previous release's binary. It is
already built, already frozen, and downloadable for three platforms. The
release gate runs the same generated corpus — about 17,000 programs — under
the baseline binary and under the current tree, with both sides on their
default engine and no flags, and every behavioral difference has to be named
by a pattern in an allowlist file. An unnamed change fails the build. In
practice that file becomes the first draft of the release notes, which is a
pleasant side effect of being forced to explain every difference you
introduce.

Seen that way, deleting the interpreter did not destroy the oracle so much
as seal it. v0.3.1 is the last release that carries both engines. For the
subset of the language frozen into that binary, an independently written
second implementation goes on answering, permanently, through the
release-to-release gate.

The durable answer is still the first kind, and that is where the effort
went afterwards: `docs/language.md` is now the normative reference, with a
hand-written behavior suite rather than a second engine as its enforcement.

## Making the deletion safe

Four practices did most of the work, and none of them is specific to
culebra.

**Pin the expected answer on the old engines before writing the new one.**
Every time a construct was about to be added to the VM, a small program
exercising it was run on both existing engines first, and the two answers
compared. Where they disagreed, the disagreement was resolved and fixed as
its own commit before any VM code was written. Seventeen commits fixing bugs
that already existed in the JIT landed that way during the port. Porting an
engine turns out to be an audit of the engines you already have, and it is a
better audit than reading the code, because it asks a question the code has
to answer.

**Check that your gates are looking at what you think.** Partway through,
the symmetry sweep that compared the engines was found to be comparing
standard output and throwing the VM lanes' exit status away. An uncaught
exception and a segmentation fault both leave standard output empty, which
is exactly what a passing test file prints — so a crash read as agreement.
Three test files were green that way, and one of them was a segfault. The
status had been dropped for a defensible reason (an out-of-slice rejection
also exits nonzero, and the sweep wanted that to be a skip); the fix was to
ask the skip question about the status too, instead of not asking.

**Stage it so that everything before the delete is reversible.** The VM
entered as a third engine behind a hidden flag, with the other two still in
charge. Then it became the front end of the JIT, and the AST code generator
was deleted — `jit.h` went from 16,446 lines to 5,271. Then it became the
default engine, and that release sat in the world for a while. Only then was
the interpreter removed. The switch to default was, satisfyingly, verified
for free: because the release-diff gate runs both sides on "no flag, default
engine," flipping the default turned that gate into a tree-walker-versus-VM
comparison without a line of it being edited. Across 17,262 generated
programs, every difference it reported was a standard-library addition made
since the previous release, and nothing else moved.

The release that shipped the switch then failed to build on all three
platforms — because the packaging script smoke-tests the stripped binary on
each engine, and a check added earlier had been landed everywhere except
there. That path only runs when a version tag is pushed, so nothing had
executed it in the weeks since. It is a small, ordinary failure with a
general shape worth keeping: *a step that only a release can reach is a step
a release discovers is broken.*

**Do the things that are only possible while both engines are alive,
first.** Coverage measurement found twenty-four lines in twelve functions of
the shared surface that no hand-written test had ever executed — they were
reached only by the generated corpus, which would have no independent
opinion once there was one engine. The commit that fixed this put the
reasoning plainly: *the tree-walker's last job is to say what they should
do; once there is one engine, a new test's expected value is whatever that
engine prints, and "the test agrees with the implementation" is not
evidence.* Every one of those cases was written against the tree-walker and
confirmed byte-identical on the other two lanes before being frozen. The
same shape appears again in the deletion itself: the canonical table of
standard-library signatures was generated from the interpreter's
environment, with an assertion that the generated table and the live
environment agreed, and only then were the readers switched over.

That last piece exposed the thing an outside view would never have guessed.
The interpreter was not only an engine; it was a data source. The `--vm`
binary had been constructing the interpreter's entire standard-library
environment at run time purely to read signatures out of it, and the
surviving engine's compiler included the retiring engine's header. Measure
what depends on the component you intend to delete. Do not reason about it.

## A note on how others have done this

The closest well-documented public example of this kind of transition is
Zig's move from its C++ bootstrap compiler to the self-hosted one: the
default switched in 0.10 with the old compiler still reachable by a flag,
and it was removed in 0.11. The Zig project has been open about the tradeoff
that follows, and it is the one worth internalizing — **deleting an
implementation freezes parity at the moment you delete**. Capabilities the
old implementation had and the new one did not were not merely postponed,
they were unavailable, and in that case for a considerable time. That is the
whole reason the coverage backfill above came before the deletion rather
than after it. Having a public project document that experience honestly is
worth a great deal to everyone who reads it afterwards.

The opposite decision is equally reasonable in its own context. LuaJIT keeps
a hand-written interpreter alongside its trace compiler, but as the fallback
a trace exit lands in — an architectural necessity, not a second opinion.
V8 keeps Ignition because a bytecode interpreter is the right tier zero for
code that runs once. Neither is a case of maintaining two front ends. The
question is never "one engine or two"; it is whether your engines share the
place where the language is decided.

## Where it landed

The tree-walking interpreter was removed in v0.4.0, sixteen days after the
spike that proposed replacing it. Hot loops run 2–4.5× faster than they did.
About 24,000 lines went away; the runtime archive that ahead-of-time
compilation links against halved, from 20.2 MB to 10.3 MB, and its defined
symbols dropped from 6,433 to 1,472. A build without LLVM has a real engine
again, which is what the browser Playground is. And the release-diff gate
landed the deletion with an empty allowlist: the whole removal, with not one
observable difference to explain.

It did not all come for free. The engine switch exposed one real bug that
neither engine's own testing would have found: strings in the runtime are
reclaimed by tracing rather than by reference counting, and moving the
default engine carried that representation into the one build — WebAssembly
— where the conservative stack scan cannot see a root at all, so the
Playground began losing live values after about ten thousand frames. That
one is written up in [the memory essay](memory.md), because it belongs to
the collector's story more than to this one.

## What generalizes

A second implementation is a bug detector and a source of bugs at the same
time, and which of the two dominates depends on how many people are keeping
them in step. With a large team and a specification, the detector wins:
that is why the projects named above run differential tests against each
other and against test262 and are right to. With one person, every addition
to the standard library is a synchronization tax paid twice, and the
integration failures found at the end of this migration were, without
exception, the shape of "one side grew and the other's table did not follow"
— bugs that could not have existed had there been one implementation.

So the honest form of the lesson is not "delete your interpreter." It is
that the second implementation should be a decision you keep re-making with
current numbers, rather than an arrangement you inherit from the order in
which you happened to build things. culebra had two engines because it wrote
an interpreter in 2017 and wanted speed in 2026 — which is a reason about
history, not about design.

## Lineage

The shape this landed in is not original, and the specific debts are worth
naming: CPython, Ruby, Lua, V8 and SpiderMonkey for putting a bytecode
between the source and both engines, and for demonstrating over decades that
it is the stable answer; Lua for the fused loop instructions the
counted `for` uses, and for the example of a small team that keeps one
implementation and a reference manual that is the specification; Zig for
being open about what a compiler migration costs on the far side; test262,
ruby/spec and the WebAssembly spec tests for the demonstration that the only
oracle which does not rot is a specification written down independently of
any implementation; SQLite and Rust's crater for the two other honest
answers to the question of what you check against when you cannot check
against yourself.
