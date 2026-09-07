# Example RFC: Algebraic effects

> A real feature ("Algebraic effects" in `docs/language.md`), written up
> after the fact in the RFC template. It's the "needed core changes"
> case: #7 is no, so #8 and #9 carry real weight.
>
> Terminology note: when this landed, the executor was a tree-walking
> interpreter (the commit history calls it "interp"). It's a bytecode VM
> now, so the absolute numbers in #5 no longer apply; the ratios are
> still the point.

- Status: Accepted
- Author: yhirose
- Date: 2026-07-16 (thin slice) to 2026-07-23 (GC-root fix)

## 1. What

`effect fn` / `perform` / `handle { } with op(resume) { }`: resumable,
composable effect handlers in the Koka / OCaml 5 family, compiled by a
source transform into the same CPS engine that already powers
generators and `yield`.

## 2. Use cases

1. **Dependency injection without threading a parameter through every
   call.** Deep code can `perform ask()` for a config value, a logger,
   or a mock answer in tests, and the caller decides what `ask` resolves
   to with `handle ... with ask(resume) { resume(v) }`. No `ctx`
   parameter has to be added to every function signature in between.
2. **Multi-shot exploration.** A search or backtracking computation
   suspends once and is resumed *more than once* with different values
   from the same suspension point, to explore several continuations
   from one place in the code. A generator can't do this: its
   continuation is one-shot.

## 3. Why existing features fall short

Generators already give one-shot suspension, but have no handler
concept (no "who answers this suspension is chosen at the call site"),
no delegation to an enclosing handler, and a continuation that can only
be resumed once. Plain closures and callbacks solve some of this but
force a context object through every function in the call chain, which
is exactly the boilerplate dependency injection and effects exist to
remove.

## 4. Syntax

**Option A (chosen): Koka-style evidence passing, source-transformed to
explicit control flow.**

```culebra
effect fn ask()
let v = handle { perform ask() * 2 } with ask(resume) { resume(21) }
inspect(v)  # => 42
```

Pros: continuations end up as ordinary refcounted heap values, so the
project's structural leak-freedom extends to effects for free. Cons:
needs a real transform pass, not just new runtime primitives.

**Option B: OCaml 5 native effect handlers** (real one-shot
continuations captured on the native stack). Rejected. A native
continuation lives outside GC/RC tracking; dropping one without
resuming it is a leak the ownership model has no way to see, which
opens a new leak class that the "JIT leaks must be structurally
impossible" requirement can't tolerate.

**Option C: pure library, no language support** (manual
continuation-passing style, or a handler object threaded by hand).
Rejected. Unusable in practice: it's the boilerplate the feature exists
to remove, and it doesn't compose across independently written
functions.

## 5. Performance

`perform` cost about 166 µs on the interpreter of the time (tail-clause
fast path) versus 4.5 µs for a plain call, and about 2x a generator
`yield` (83 µs), since both share the same CPS engine written in
culebra; about 6 µs under `--jit`. Handler lookup went from O(n) in
handler-stack depth to O(1) (7.5 to 5.5 µs) by keying a dictionary by
operation name.

JIT compile time for effect-heavy files was initially disproportionate
(one test file: 29.7 s). Traced to the transform emitting 43 to 45 lines
of generated culebra per site, not to codegen, which is more
IR-efficient per line than hand-written code. Further reduction was
capped around 16% without rewriting the lowering, so it stopped there.

## 6. Safety

The riskiest area of the whole feature. Two examples:

- The C++ exception carrying an aborted handler's payload had no GC
  root. Safe by accident on macOS, where the conservative stack scan
  happened to see the pointer, but a deterministic crash on Linux under
  GC stress. Linux CI caught it after the push and before any release.
- Multi-shot continuations are cloned with a native shallow copy. An
  early version of that copy didn't retain the class's method table, so
  a second clone's method dispatch hit a dangling pointer.

Leak-freedom itself held throughout, because continuations are ordinary
RC heap objects (Option A's whole rationale) rather than native stack
frames.

## 7. Can this be done in a preamble (pure .cul), with no core changes?

**No.** Needs new grammar (contextual keywords `effect`, `perform`,
`handle`, `with`), a new AST-to-source transform pass, and JIT-side
changes: free-variable analysis had to treat the generated builder as a
scope boundary, GC root enumeration had to learn about in-flight abort
payloads, and a native primitive for continuation cloning was added.

## 8. Implementation size estimate

Large. A new transform (`include/frontend/effects_transform.h`, about
2000 lines today), a new runtime preamble
(`src/preambles/effects.cul`, about 300 lines), grammar changes, and
JIT runtime/GC touches. Landed over about a week of incremental,
individually gated steps (thin slice, then A-normalization,
control-flow bodies, multi-shot resume, nested-handle capture, dynamic
`perform` with O(1) dispatch), each a full-gate-green checkpoint.

## 9. Backend symmetry

The choice to source-transform to CPS (Option A) exists specifically to
make backend symmetry structural: every backend runs the exact same
lowered culebra code, instead of each needing its own native `perform`
and `handle` that could drift apart.

That didn't make symmetry free. Real asymmetries still surfaced during
implementation (a nested handler clause capturing an enclosing variable
worked under the interpreter's lexical scoping but threw under the JIT's
explicit-capture model, until fixed structurally), and the GC-root bug
in #6 was Linux-only. Source-transform lowers the *risk* of asymmetry;
it doesn't remove the need to check both backends and both OSes.

## Notes

Deliberately out of scope: OCaml-5-style native continuations (#4).
Scoped out for a later cycle: lifting `perform`'s position restrictions
inside short-circuit/ternary operands and method-chain receivers.
Reachable, but judged not worth the extra transform complexity yet.
`perform` inside a `try` expression is a permanent rejection.
