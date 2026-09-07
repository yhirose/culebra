# Example RFC: StateMachine, a hierarchical state machine

> A real feature (`StateMachine` in `docs/stdlib.md`), written up after
> the fact in the RFC template. It's the "stayed entirely in a preamble"
> case: #7 is yes, so #8 is N/A.

- Status: Accepted
- Author: yhirose
- Date: 2026-09-02

## 1. What

A stdlib `StateMachine` class: nested states, event bubbling to
ancestors, entry/exit hooks, guarded transitions. Built either from a
description `Object` or from a text DSL.

## 2. Use cases

1. **Flat, guarded transitions.** A vending machine: `idle`/`ready`
   states, and a `select` event that dispatches to `dispense` or
   `reject` depending on a balance guard.
2. **Nested states with entry/exit.** A media player: `playing` is
   composite (`normal`/`seeking` inside it), `stop` is declared once on
   `playing` and reached from either child by bubbling, and moving
   between `normal`/`seeking` must *not* re-run `playing`'s
   `entry`/`exit`.

## 3. Why existing features fall short

Nothing modelled "current state" as a first-class thing. Contributors
were hand-rolling a dispatch `Object` (`state -> event -> handler`) and
re-deriving hierarchy, bubbling and entry/exit by hand each time. There
was no library help for "which ancestor handles this event" or "which
entry/exit hooks run between two arbitrary states".

## 4. Syntax

**Option A: description `Object`**

```culebra
let bump = fn (ctx, ev) { ctx.n += 1 }
let m = StateMachine.new({
  off: {initial: true, on: {flip: {action: bump, target: 'on'}}},
  on: {on: {flip: {target: 'off'}}},
}, context: {mut n: 0})
m.fire('flip')
inspect(m.state())  # => 'on'
```

Pros: guards and actions are ordinary closures, no name-resolution
step, no new grammar. Cons: verbose once guards and hierarchy show up;
everything is bracket nesting.

**Option B: text DSL**

```culebra
let vending = `
  Vending {
    idle initial {
      coin: add_coin -> ready
    }
    ready {
      coin: add_coin -> ready
      select[enough]:  dispense -> idle
      select[!enough]: reject   -> ready
    }
  }
`
let m = StateMachine.parse(vending,
  guards: {enough: fn (ctx, ev) { ctx.balance >= 100 }},
  actions: {
    add_coin: fn (ctx, ev) { ctx.balance += ev.payload },
    dispense: fn (ctx, ev) { ctx.balance -= 100 },
  },
  context: {mut balance: 0})
```

Pros: reads like a statechart diagram, and matches how people already
draw state machines. Cons: an extra small grammar to maintain, and
indirection through name tables.

Both were kept. The `Object` form was needed anyway as the DSL's compile
target, so exposing it cost nothing extra, and each form fits a
different authoring style.

## 5. Performance

The first `fire()` recomputed a transition's domain, target and
entry path on every call. Those are functions of (source, target)
alone, so they moved to build time and are computed once per machine,
not per event. Measured on a 7-state machine over 160k events: 3350 to
2050 ns per fire on the executor, 1460 to 878 ns under `--jit`, for
+19 µs of one-time build cost.

## 6. Safety

Every state, guard and action name is resolved when the machine is
built, not at first use. A typo in a transition target, a missing
initial state, two initial siblings, or a composite state without an
initial child all raise `StateMachineError` at build time rather than on
the first event that would have reached the broken part. `can_fire`
runs guards speculatively, so guards must be free of side effects
(documented, not enforced).

## 7. Can this be done in a preamble (pure .cul), with no core changes?

**Yes, entirely.** `StateMachine.new` and `StateMachine.parse` are
ordinary stdlib classes. The DSL is parsed with the native `_Peg`
primitives directly rather than the public `Peg` module, so a program
that never mentions `Peg` doesn't pull that lazy namespace in.

## 8. Implementation size estimate

N/A: #7 is yes.

## 9. Backend symmetry

No new bytecode ops and no new core dispatch. It's stdlib closures and
Object property access, so the executor, `--jit` and AOT needed no
per-backend logic. The library's own transition-selection order (first
matching guard wins) still needs the usual "same order on every
backend" check, but that falls out of ordinary Array iteration
semantics, not anything new here.

## Notes

Deliberately not modelled: parallel regions and history states (SCXML's
`<parallel>` and `<history>`). No driving use case surfaced them.
Revisit if one does.
