A Shared Bytecode VM: Design Proposal
=====================================

**Status: Phases 0–2 done and merged to `master`; Phase 3 under way
on branch `vm-phase3`.** Sections 1–9 record the motivation, the
target architecture, and the migration plan for replacing the
tree-walking interpreter with a bytecode VM that shares its value
representation — and its front end — with the JIT. The Phase 0 spike
(§7) answered both exit questions yes
([§10](#10-phase-0-spike-results)), Phase 1 built the third backend
on the same branch (§10's postscript), and Phase 2 took it to the
parity bar §7 sets ([§11](#11-phase-2-full-parity-results)). Phase 3
moved both compiled entries — `--jit` and `culebra build` — onto the
bytecode and deleted the AST codegen they left behind, meeting §7's
exit criterion: one consumer reads the AST, and it is the bytecode
compiler ([§12](#12-phase-3-folding-the-jit-onto-the-bytecode)).
Phase 4 — retiring the tree-walker — is unimplemented. The observable language
contract in [`language.md`](../language.md) is unaffected: this is a
change of engine, not of language. Where this document and
`language.md` disagree, `language.md` wins.

The Japanese mirror of this file is [`vm.ja.md`](vm.ja.md).

Contents
--------

1. [Summary](#1-summary)
2. [Motivation: one semantics, two implementations](#2-motivation-one-semantics-two-implementations)
3. [What already exists](#3-what-already-exists)
4. [Target architecture](#4-target-architecture)
5. [Value representation](#5-value-representation)
6. [Memory management](#6-memory-management)
7. [Migration plan](#7-migration-plan)
8. [Risks and open questions](#8-risks-and-open-questions)
9. [Prior art](#9-prior-art)
10. [Phase 0 spike: results](#10-phase-0-spike-results)
11. [Phase 2: full parity — results](#11-phase-2-full-parity-results)
12. [Phase 3: folding the JIT onto the bytecode](#12-phase-3-folding-the-jit-onto-the-bytecode)

---

## 1. Summary

Retire the tree-walking interpreter. In its place: a bytecode
compiler shared by both backends, a VM that executes the bytecode,
and an LLVM lowering that compiles the same bytecode.

```text
             (today)                          (proposed)

   AST ──> interpreter.h  (walk #1)     AST ──> analysis ──> bytecode
   AST ──> jit.h          (walk #2)                           │
                                              ┌───────────────┤
                                              ▼               ▼
                                         VM executor    LLVM lowering
```

The VM adopts the JIT's existing runtime value model (`JitValue`,
`JitCell`, `JitClosure`, `Shape`, the slab allocator, RC + cycle
collector) as the single value representation for the whole system.
The interpreter's `Value` / `Environment` model is retired with the
tree-walker.

## 2. Motivation: one semantics, two implementations

Culebra's core requirement is three-dimensional symmetry across
backends: behavior, error kind/message/position, and the timing and
order of checks. Today that symmetry is maintained by hand, because
the same semantic decisions are written twice:

- Two independent AST walkers: `interpreter.h` (12.6k lines) and
  `jit.h` (16.2k lines + 1.7k in `jit_compile_*.h` fragments)
  each dispatch over the same node tags. The JIT-side headers carry
  over 150 comments citing interp parity or mirroring — each one a
  place where a human re-derived the other backend's behavior.
- Two stdlib bindings: `stdlib_interp.h` (7.9k lines) over `Value`,
  `stdlib_jit.h` (10.3k lines) over tag+data. The calling convention
  is already single-sourced (the JIT derives `NsParamMeta` from the
  interp's canonical parameter lists), and the Math kernels are now
  shared (`stdlib_math.h`), but every other namespace still
  implements its semantics twice.
- Two serializers for isolate transfer: `sendable.h` (724 lines) and
  `sendable_jit.h` (1.8k lines) both lower to the same `SendNode`
  tree from different value representations.

The cost is not hypothetical. Unifying just the Math kernels
immediately surfaced a live divergence: the interpreter compared
Long `min`/`max` arguments through `double`, collapsing neighbors
past 2^53, while the JIT compared exact 64-bit (fixed in
`a7e7cd7`). Earlier, `trim_start` sat outside the differential
corpus for two months with a live arity asymmetry (see
`tools/check_difftest_coverage.sh`). Both bugs are of the same
species: a decision duplicated across representations drifted.

A shared bytecode layer turns the third symmetry dimension — check
timing and order — from a discipline into a structural property:
both backends consume the same instruction sequence, so checks
happen in the same order because they *are* the same checks.

Two secondary motivations:

- **`jit.h` shrinks.** Lowering bytecode to LLVM IR is a much
  smaller step than lowering an AST: the ~200 direct `nodes[i]`
  reads scattered through the JIT move into the bytecode compiler,
  written once. (This mirrors what the Wado compiler found with its
  WIR layer: with a low-level IR in front, codegen collapses toward
  a thin emit loop.)
- **The interpreter gets faster.** The tree-walker resolves every
  variable read by name — a hash lookup per access, walking an
  `Environment` parent chain on miss. Bytecode resolves slots at
  compile time. Value sizes shrink too (see §5). Reference numbers
  on `tools/bench/fib.cul` (fib 28, `-O3` build): interp 3.9s, JIT
  0.56s including compilation. A VM will not close that 7x gap —
  most of it is native code, not representation — but 1.5–2.5x over
  the tree-walker is the range register VMs typically reach.

## 3. What already exists

This proposal invents less than it appears to. Three of the four
pieces are already in the tree:

**The runtime data model.** The JIT's heap objects are already a
VM's heap objects: `JitCell` (refcount + value) is a Lua-style
upvalue box; `JitClosure` (fn_ptr + `JitCell**` captures + arity)
is a classic VM closure; `Shape` is a V8-style hidden class; the
slab allocator, RC discipline, and cycle collector are the working
heap of `docs/internals/memory.md` §3–6. The VM adopts these
as-is — no new representation is designed.

**The front-end analysis.** A bytecode compiler needs to know which
variable lives in which slot and what each function captures. That
analysis exists today inside `jit.h` — `collect_fn_locals`,
`visit_for_frees`, `analyze_fn_common`, `scan_eh_defer`, `FuncInfo`
— but is JIT-only; the tree-walker does name resolution dynamically
instead. Under this proposal the analysis becomes the shared
bytecode compiler's front end, consumed by both backends.

**The oracle.** Rewriting an interpreter is normally dangerous
because there is no authority on what the rewrite must do. Culebra
already has one: the differential corpus (`tools/difftest`), the
interp-vs-JIT sweep in `just test-dev`, and
`misc/run_all_backends.sh`. During migration the tree-walker stays
in the tree as the reference implementation, and every VM increment
is diffed against it with machinery that already runs in CI.

What does *not* exist: the bytecode format, the VM execution loop,
and the debug-info tables. That is the actual new work.

## 4. Target architecture

```text
  .cul source
      │  parse (peglib grammar, unchanged)
      ▼
     AST
      │  AST-level transforms (generator/effects — see §8)
      ▼
  analysis        locals/slots, captures, EH regions   (lifted from jit.h)
      ▼
  bytecode        slot-resolved, RC-explicit, lowering-friendly
      │
      ├────────────► VM executor        (new; default engine from Phase 4)
      │
      └────────────► LLVM lowering      (jit.h, rewritten smaller in Phase 3)
```

Design commitments, held loosely until the spike (§7) confirms them:

- **Register-based, not stack-based.** Registers map directly onto
  the JIT's SSA values and the frame layout the analysis already
  computes; Lua's experience says the interpreter loop also gets
  fewer dispatches per expression.
- **Variables are slot indices.** Name→slot resolution happens once,
  in the bytecode compiler. The `Environment` chain and its
  per-access hash lookups disappear. Captured variables live in
  `JitCell`s exactly as JIT closures capture them today.
- **RC operations are explicit in the instruction stream.** The
  bytecode compiler emits retain/release the way JIT codegen emits
  them today, under the same ownership discipline
  (`memory.md` §4). The VM executes them; the LLVM lowering compiles
  them. One emitter, two consumers — the leak gates (`leak.sh`,
  `CULEBRA_GC_STRESS`, `just test-assert`) apply to both.
- **Positions ride the bytecode.** A side table maps instruction
  offset → line/col; error paths read it. This is what makes error
  positions structurally symmetric instead of hand-carried.
- **Debug tables are first-class.** Slot → name and offset →
  line/col tables are emitted always (they are small); REPL, DAP,
  and the debugger consume them (§8).
- **Bytecode is internal.** No serialization format, no version
  guarantee, never written to disk. It is an in-memory contract
  between the compiler and the two engines, free to change at any
  commit.

## 5. Value representation

The unified representation is the JIT's, unchanged:

| | interp `Value` (retired) | `JitValue` (adopted) |
|---|---|---|
| value | 24 B (`Type` + `std::any`) | 16 B (`{i64 tag, i64 data}`) |
| String | `std::string` boxed in `any` | slab header, one allocation |
| Array | 96 B payload + `shared_ptr<vector>` | `JitArray` ≤ 48 B |
| Function | 368 B payload, boxed | `JitClosure` ≤ 48 B |
| Object | name→value map | `Shape` + fixed slots ≤ 128 B |
| management | `shared_ptr` (atomic RC) | RC (non-atomic) + slab + cycle GC |

Downstream effects of retiring `Value`:

- `sendable.h` / `sendable_jit.h` collapse to one serializer.
- The AOT runtime archive stops embedding the interpreter:
  `culebra_rt.cc` includes `stdlib_interp.h` today only because the
  canonical parameter spec is expressed as interp `FunctionValue`s.
  With one representation the canonical spec is plain data.
- The stdlib binding is written once. The kernel pattern from
  `stdlib_math.h` survives unchanged — kernels keep owning the
  semantic decisions — but the per-backend boundary shims are no
  longer needed.
- `JitValue`'s quirks are kept, including the i64 tag (an ABI
  requirement: a `{i8, i64}` return is coerced differently by the C
  compiler and the JIT — see the comment at `jit_value.h`). The VM
  does not care; the JIT still does.

## 6. Memory management

The contract of `language.md` §17 (deterministic `drop`, RC primary,
tracing backstop) is unchanged. What changes is *who writes the RC
operations for the interpreted path*: today the tree-walker gets
correctness from `shared_ptr` structurally; under the VM, the
bytecode compiler emits retain/release, and the structural guarantee
moves up one level — it is the *emitter* that is verified, once,
rather than 200 hand-written eval sites.

This is the same trade the JIT already made, with the same
mitigations already in place: the ownership discipline of
`memory.md` §4, the RC ratchets in `just test-dev`, the leak sweeps,
`CULEBRA_GC_STRESS`, and the `NDEBUG`-off assert lane. The VM adds
one new obligation: the interpreter loop itself must maintain the
invariants between instructions (a register file is a GC root set).

## 7. Migration plan

The ordering principle: **the VM enters as a third backend, and
nothing is restructured or deleted until it has reached full
parity.** The two existing engines stay untouched — and keep
shipping — while the new one grows against them; deep sharing with
the JIT begins only after the bytecode has been proven by the full
corpus; the tree-walker is deleted last, together with the checks
that existed only to keep two hand-maintained walkers honest. Every
phase boundary is an ordinary landing: at each checkpoint the tree
builds, every gate is green, and the product is fully usable.

**Phase 0 — spike (bounded: days, not weeks).** Pick one
non-trivial construct (`for` over a range, or `match`). Define its
bytecode; write the VM loop for it; write the LLVM lowering for it.
The `jit.h` rewrite is deferred to Phase 3, but the bytecode format
must be born lowering-friendly — an instruction set that only an
interpreter can consume would poison every later phase, and this is
the cheapest moment to find out. Two exit questions, answered with
code:

1. Is the bytecode→LLVM lowering meaningfully smaller and simpler
   than today's AST→LLVM for the same construct (`compile_for`:
   320 lines, `compile_match`: 165 lines)?
2. Does the VM beat the tree-walker on a micro-benchmark of that
   construct by ≥ 1.5x?

If either answer is no, stop: the two-day artifact is the evidence,
and this document gets a "rejected, here is why" postscript instead
of two months of sunk work.

**Phase 1 — the third backend.** Build the bytecode compiler and
the VM executor incrementally (expression core → control flow →
closures → EH/`defer`), as a new engine beside the existing two
(a `--vm` flag). The first concrete step is lifting the analysis
passes (`collect_fn_locals`, `visit_for_frees`, `analyze_fn_common`,
`scan_eh_defer`, `FuncInfo`) out of `jit.h` into a shared header —
they become the bytecode compiler's front end, and `jit.h` keeps
consuming them unchanged. Two commitments hold throughout the
phase:

- **The VM rides the JIT's runtime layer from day one**: `JitValue`
  values, the `culebra_runtime_*` helpers, `kNsMethods` dispatch,
  slab/RC/cycle-GC. "Full parity" therefore does not mean a third
  stdlib binding — most of the stdlib arrives through the runtime
  layer the JIT already calls.
- **The VM consumes the same AST-level transforms**
  (generator/effects) as the other two backends. No semantic
  remodeling while three backends must agree; frame-based
  generators are a post-unification simplification (§8), not a
  migration-time one.

The differential machinery gains a lane: the corpus and
`misc/run_all_backends.sh` compare all three engines.

**Phase 2 — full parity.** Everything the tree-walker does, the VM
does: isolates and sendable transfer, REPL, the doctest runner, DAP
and the debugger on the VM's debug tables. Exit criterion: every
gate that today runs interp + JIT runs all three, green, and the
corpus finds no divergence. Latent interp-vs-JIT divergences that
the third lane surfaces are fixed as they appear — no pre-cleaning
campaign is needed; the Math `min`/`max` case (§2) shows what to
expect.

**Phase 3 — share the front end.** With the bytecode format
battle-tested by the full corpus, restructure the JIT: rewrite
`jit.h` to lower bytecode instead of AST. Deferring this until
after Phase 2 means bytecode-format churn during construction
always had one consumer, not two. Exit criterion: exactly one AST
consumer remains (the bytecode compiler), and the ~200 `nodes[i]`
reads are gone from the JIT. This is where the `jit.h` shrinkage
promised in §2 is collected.

**Phase 4 — retire the tree-walker.** Flip the default engine to
the VM after a soak period; delete `interpreter.h`,
`stdlib_interp.h`, the `Value` model and its serializer; prune the
checks that existed only because two walkers were maintained by
hand — the eval_X/compile_X dispatch-symmetry ratchet, the dual
serializer tests. The differential corpus is **not** among them: it
re-points at VM vs LLVM, because two consumers of one bytecode can
still diverge in lowering, and the corpus is what catches that. The
leak gates, `CULEBRA_GC_STRESS`, and the assert lane survive
unchanged.

## 8. Risks and open questions

- ~~**DAP/debugger semantics**~~ (`dap.h`, 873 lines, deeply coupled
  to `Value`/`Environment`). Step semantics and scope enumeration
  must be reimplemented over debug tables. Standard machinery, real
  work; part of Phase 2's parity bar. **Settled in Phase 2** (§11.2):
  `dap.h` kept the protocol and gave everything else to a six-question
  `DebugEngine` interface, and the VM answers it from per-binding live
  ranges plus a frame stack. The unforeseen part was not the tables
  but the threading — see §11.2.
- ~~**Lazy stdlib resolution.**~~ `Environment::initialize_lazy`
  resolves stdlib modules on first *name lookup* — a mechanism that
  assumes name-based access. Slot resolution needs a different trigger
  (likely: resolve at bytecode-compile time when the compiler sees
  the name). **Settled in Phase 2** as predicted: the VM compiles the
  preamble itself, so a stdlib name resolves when the compiler sees
  it.
- **Generator/effects transforms** (`generator_transform.h` 1.5k,
  `effects_transform.h` 1.9k lines) stay AST→AST through the
  three-backend period — the VM compiles their output, so the three
  engines agree by construction. Rewriting them as frame suspension
  is a post-Phase-4 simplification (this is how CPython and Lua
  model generators/coroutines) and gets its own mini-spike then; it
  is deliberately *not* part of the migration.
- **The default engine changes.** `culebra prog.cul` runs new code
  from Phase 4. The differential corpus is the mitigation, and the
  phase ordering keeps the old engine available until parity is
  proven — but the risk window is real and long.
- **Performance floor.** The VM must not lose to the tree-walker
  anywhere that matters. Watch interpreter-startup-sensitive uses
  (scripts that run for milliseconds; bytecode compilation is added
  latency where the tree-walker starts instantly).
- **Two front ends' worth of maintenance during migration.** Phases
  1–3 run three backends in the tree. The isolation is what the
  phase structure buys; the cost is CI time and attention.

## 9. Prior art

Every mature dynamic-language implementation that runs both an
interpreter and a JIT shares a bytecode between them: CPython,
Ruby (YARV + YJIT), Lua/LuaJIT, V8 (Ignition + TurboFan),
SpiderMonkey. Culebra's current shape — one AST walked independently
by two full implementations — is the unusual one, and the cost shows
up exactly where this document started: symmetry by hand.

The nearest ancestor of this proposal is the Wado compiler's WIR
layer (a tree-shaped IR one step above wasm, introduced after its
codegen grew to 18k lines): the observed effect there — codegen
collapsing to a thin emitter, new compiler phases becoming cheap to
add — is the effect §2 predicts for `jit.h`. The value model this
proposal adopts is already documented in
[`memory.md`](memory.md) §3–6; the design lineage note there
(§7) applies to the VM unchanged.

## 10. Phase 0 spike: results

Run 2026-08-09 on branch `vm-spike`, for the counted range-`for`
construct as §7 prescribes. Numbers below are post-cleanup (a
`/simplify` pass removed several provably-dead RC ops from the VM's
hot loop — see the branch history for the pre-cleanup figures, which
already cleared the bar). What was built (hidden flags `--vm-spike`
/ `--vm-spike-llvm` / `--vm-spike-dump`, `include/vm_spike.h`, ~700
lines total):

- a register-based bytecode — slot-resolved variables, RC ops
  explicit in the instruction stream, a run-length position side
  table, an always-generated slot-name debug table — with fused
  `ForPrep`/`ForLoop` loop opcodes whose semantics are
  `RangeBounds::done()/take()` (`range_bounds.h`), the same sequence
  oracle the interp fast path reads and the JIT hand-emits;
- a bytecode compiler for a Long-only slice (`let`/`let mut`,
  reassignment of a visible `let mut` slot, `+ - *` and unary minus,
  nested counted `for` with identifier bindings, `break`/`continue`,
  single-argument `println`; everything else rejects at compile
  time);
- a switch-dispatch VM executor on the JIT runtime value model
  (registers in a C++ stack array, so the conservative GC scan roots
  them);
- an LLVM lowering of the same bytecode, reusing the JIT's codegen
  context and the existing ORC `exec` scaffold as a `JIT` friend.
  `decode_range_layout` moved from `jit.h` to `parser.h` as the
  first shared-frontend lift.

All three lanes agree on the correctness set
(`tools/bench/vm_spike_cases/`): inclusive/exclusive/stepped/empty
ranges, both int64 overflow edges, nesting, shadowing,
break/continue, and the zero-step error down to kind, message, and
position.

**Q2 — does the VM beat the tree-walker by ≥1.5×? Yes: ≈34×.**
`just build` (-O3 + LTO) binary at the branch head, hyperfine,
10 runs, idle machine:

| bench (iterations) | interp | `--vm-spike` | `--jit` (compile incl.) | `--vm-spike-llvm` |
|---|---|---|---|---|
| `for_range.cul` (25M, minimal body) | 9.15 s | **0.270 s (33.9×)** | 0.462 s (19.8×) | 0.056 s (163×) |
| `for_range_dense.cul` (4M, dense body) | 6.53 s | **0.192 s (34.0×)** | 0.337 s (19.4×) | 0.101 s (64×) |

Per-iteration: interp ≈366 ns, VM ≈11 ns (minimal body). The gap is
dominated by the tree-walker's per-iteration `make_scope` +
name-map insert, which its counted fast path still pays. Three
side-findings: at these program sizes the VM also beats the JIT on
wall clock (LLVM compilation dominates the JIT lane); the
bytecode→LLVM lane starts in ≈56 ms because it compiles only the
tiny chunk module (no preamble splice); and startup on a one-line
script is ≈4.4 ms for the VM lane vs ≈4.9 ms interp — the §8
startup-latency concern did not materialize at this scale. Because
the interp cost baseline is so high, the 1.5× bar had little
discriminating power; the informative number for Phase 1 is the
≈11 ns/iteration dispatch floor.

**Q1 — is bytecode→LLVM significantly smaller than AST→LLVM? Yes,
≈2–3× for the construct, with the heavy machinery gone.** Counted
on the same tree:

| unit | lines |
|---|---|
| AST→LLVM, `compile_for` whole | 309 |
| AST→LLVM, counted-fast-path total (fast-path block 35 + `compile_for_counted_range` 72 + `emit_for_body_with_owned_binding` 95 + `for_break_target` 16) | ≈218 |
| of which load-bearing for the slice (no pattern/defer arms) | ≈183 |
| bytecode→LLVM, whole slice (`lower_chunk`, all 15 opcodes) | 151 |
| bytecode→LLVM, loop share (ForPrep/ForLoop/Jump cases + block scaffolding) | ≈82 |
| bytecode compiler FOR case (engine-shared) | 51 |

The lowering for the whole slice is smaller than the AST path's
`for` construct alone, and the qualitative difference is larger than
the ratio: the lowering has no scope chain, no `Owned` handles, no
`PosGuard` threading, and no AST re-decoding — slots became plain
allocas (mem2reg), positions a table lookup, and scoping/RC
placement moved into the engine-shared bytecode compiler where they
are written once.

Caveats recorded for Phase 1, in fairness to both verdicts:

- The RC discipline is only vacuously exercised — every slice value
  is `TAG_LONG`/`TAG_NIL`, so `Retain`/`Release` are runtime no-ops.
  The spike proves the format *carries* RC, not that an emitter's
  placement discipline is correct for heap values.
- The slice has no throw-path cleanup obligations, so the lowering
  emits no landing pads. A real Phase 1 format needs EH region info;
  part of `lower_chunk`'s size advantage will be spent there.
- Endpoint/step evaluation order and once-vs-per-iteration are
  untested by the correctness set (slice expressions are
  side-effect-free); re-verify with effectful endpoints in Phase 1.
- The spike's scope-stack slot allocator is a placeholder for the
  lifted analysis passes (§3); the Q1 verdict assumes that swap.

Postscript — Phase 1 opened on the same branch, and the spike
graduated rather than being discarded. The analysis passes moved out
of `jit.h` into `fn_analysis.h` (the first §7 step; `-O0 --emit-llvm`
IR is byte-identical across the corpus before and after the lift),
and `vm_spike.h` grew into `include/vm.h`: namespace `culebra::vm`,
flags renamed to `--vm` / `--vm-dump` / `--vm-llvm`, rejections now
`VmError`, the correctness set now `tools/bench/vm_cases/`. The
slice gained the expression core and basic control flow — Float /
Bool / nil / String and Array literals (Array makes
`Retain`/`Release` real, answering the first caveat above),
arithmetic and comparisons with the JIT's exact dispatch shape (the
lowering calls the same `emit_arith_step` / `emit_comparison_i1` /
`value_to_bool` emitters the AST path uses), `&&` / `||` / `??`,
comparison chains, `if`/ternary as expressions, `while`, and
`FnAnalysis` as the compiler's front end (the shadow check now runs
on the VM lane; free_vars gates the non-capturing slice). Functions
followed: non-capturing function literals compile to chunks of
their own, calls go through the JitFn ABI — a function value is a
real `JitClosure` (the executor's carry a trampoline + descriptor
cell, the lowered module's are native functions), so the ArityError
/ "expected Function" / RecursionError semantics, positions
included, come from the same runtime machinery the JIT uses —
with `return`, the `fn` recursion handle, and dropped extra args.
All three lanes agree on the extended set, including error
kind/message/position for type mismatches, division by zero,
non-Bool conditions, missing arguments, non-callable callees, and
the recursion limit. Closures completed the function story: a
captured local is promoted to a `JitCell` — the JIT's own cell
mechanism — through six dedicated ops (`CellNew` / `CellGet` /
`CellSet` / `CellRelease` / `BindCapture` / `ImmutErr`), so the RC
of shared mutable state stays explicit in the instruction stream;
`MakeClosure` fills the captures from the chunk's capture list
(each fn literal has exactly one creation site), captured loop
variables get a fresh cell per iteration, assignment to a non-`mut`
binding is now the interp's runtime ImmutableError on every lane
(a never-executed assignment stays silent), and forward-reference
capture is the one shape still rejected. `throw` and `try`/`catch`
answered the spike's EH-format caveat: the format carries static
try regions (`EhRegion`: pc range, handler pc, caught slot), the
operand contract switched to borrow-twin helpers (`num_*_borrow`
etc. — same dispatch bodies, no operand release on throw) so a
throw leaves every register frame-owned and the handler's bytecode
release ladder is the one releaser (cell slots are pinned so the
ladder's Release-vs-CellRelease choice is static), the executor
catches around its dispatch loop and re-enters at the handler, and
the lowering maps regions straight onto landingpads — `emit_call`'s
existing invoke conversion plus the same carrier classification
(`try_translate`) the JIT's `compile_try` uses, so CulebraErrors
materialize as the same error objects and foreign exceptions keep
unwinding. `defer` closed out EH: a defer body is a 0-arity chunk
through the existing `MakeClosure` machinery (captures and all),
and three ops (`DeferMark` / `DeferPush` / `DeferRunTo`) drive the
same global LIFO defer stack the JIT uses — frame-level marks
(`has_any_defer`, the chunk's first insn) that `return`, the Halt
epilogue, and frame-escaping throws run to, plus per-scope marks
(`scope_has_defer`) for lexical scopes, loop bodies, and try/catch
bodies. A try takes a region mark and ends its region before the
body's fall-through defer run, so a defer throwing at the try
body's normal exit escapes its own catch, exactly as the interp's
`run_deferred` placement has it; the handler opens with
run-defers-then-release-ladder. A throw no region catches runs the
frame's pending defers on the way out (the executor in `run_frame`'s
catch-all, the lowering via a frame-level cleanup pad) — the
observable slice of the JIT's frame cleanup ladder, with slot
releases still on the GC backstop. Porting defer flushed out two
JIT bugs that had been breaking interp/JIT agreement all along —
try/catch-body defers fired at function exit instead of block exit,
and break/continue skipped defers pending in a nested lexical
scope — both fixed in `fn_analysis.h`/`jit.h`, so the three-lane
comparison now covers every defer case.

`match` followed, over the leaf-pattern slice: literals (with the
tag checked before the value, so no numeric coercion — `1` never
matches `1.0`), `_`, bindings, typed bindings over the primitive
type names (unions included, generic args stripped), or-patterns of
non-binding alternatives, and guards. One new op sufficed:
`JumpIfTag`, the pattern's tag gate; the value check reuses `Eq`
behind the gate, and the guard reuses `JumpIfFalse` — whose
`to_bool` coercion (Bool as-is, Long/Float numeric, anything else a
TypeError attributed to the match node) turned out to be exactly
the guard's semantics in both existing lanes. Compilation orders
each arm test → bind → guard → body, so a failed test jumps to the
next arm with nothing live and only a guard failure has a binding
to release; arm bodies are their own defer scopes (the
`scan_eh_defer` MATCH case), the subject lives in one
statement-owned temp across the arms, and a binding alternative
inside an or-pattern is the one rejected shape (it would bind on
some paths only). Porting match surfaced a latent compiler bug:
consuming a temp erased *every* sweep-list entry with that slot
index, and arm-scope rollback is the first construct that makes two
list entries share an index — the list shrank below an inner
statement's watermark, and the sweep's zero-filled resize emitted
`Release r0` onto a live cell slot (a segfault). Temps are now
forgotten one entry at a time. `return` also learned to release
in-flight temps of enclosing statements (a match subject held
across arms) instead of leaving them to the GC backstop.

`fn name` declarations came next, with arity-dispatch overloads:
`MultifnReg` registers each body chunk's closure into the same
runtime multimethod registry the JIT uses
(`multifn_register_and_install`, arity-only — null type strings), so
same-scope overloads merge into one dispatcher, a nested-scope decl
shadows through its own per-scope registry key, a same-arity re-decl
replaces its table entry, and DispatchError kind/message/position
come from the shared dispatcher thunk. Recursion works because the
compiler pre-declares every `fn name` of a statement list at scope
entry — an owned cell holding an unbound sentinel — so a closure
built earlier in the list captures a real cell (mutual recursion),
and a read before the decl statement runs raises the interp's
NameError through `UnboundErr`, the read guard on lazy dispatcher
bindings. Probing that machinery interp-vs-JIT before porting (the
defer-cycle procedure) exposed another latent master bug, this time
in the JIT: a closure capture *materializes* the lazy forward-ref
cell, erasing the "declaration never ran" signal its null-pointer
read guard relied on, so a pre-decl read let a nil placeholder flow
into user code where the interp raises NameError — with a plain
`let` as much as a `fn name`. Fixed in `jit.h` first (the sentinel +
value-guard design the VM now mirrors), regression in
`tests/test_forward_ref.cul`.

The stdlib then arrived through the runtime layer, exactly as the
Phase 1 sketch has it ("most of the stdlib arrives through the
runtime binding — no third stdlib port"): one new op, `NsGet`,
resolves a bare builtin global by name through
`culebra_runtime_namespace_get` — the per-Runtime cached closure the
JIT's own slow path (`emit_builtin_var_get`) returns; the lowering
just calls that emitter — and the result is an ordinary function
value for the existing generic `Call` op. That reaches every native
`kBuiltinFns` global (`to_string` / `to_long` / `to_float` /
`type_of` / `hash` / `inspect` / `print` / `println` / `range` /
`iota` / `grid`) with no per-name code, because probing showed the
interp and the JIT already agree that a direct builtin call and a
through-value call (`let f = to_string; f()`) produce identical
results and errors — kind, message, and call-site position, typed
parameter checks included: the binder trampoline attributes
everything to `set_call_site`, which the VM's `Call` op already
publishes. Reads of the same name compare equal (`f == to_string` —
one cached closure per Runtime), a lexical binding still shadows the
builtin, and `println(<one arg>)` keeps its dedicated-op peephole
while every other shape — bare `println()`, wrong arity, println as
a value — takes the generic route and the runtime's own diagnostics.
The lazy source modules (`Time`, `Regex`, the `assert_*` family, ...)
stay rejected: they are built by the preamble splice, which the VM
lane deliberately skips. One boundary became visible in the process:
`FnAnalysis`' locals set is function-granular, so a builtin name
that is *also* declared somewhere in the same function (say a
block-scoped `let to_string` after a fn literal that reads the
builtin) turns the fn's read into a forward-reference capture —
rejected, like every other forward-reference capture in the slice,
where the JIT resolves it at the use site.

Phase 1's last item — the differential machinery gains a lane —
landed in two pieces, each sized to what the slice can honestly
check. The curated corpus (`tools/bench/vm_cases/`: the three-lane
compare plus the same sweep under collect-on-every-allocation) is
now a gate: `just test-dev`, `just test`, and the ci-light CI shard
run it, and since the corpus holds only in-slice programs, any
VmError there is an output mismatch — a slice regression turns the
gate red instead of relying on a manually run script. And
`misc/run_all_backends.sh`, the shared single-script symmetry
helper the Windows CI jobs call, grew `--vm` / `--vm-llvm` lanes: a
matching output passes, an out-of-slice reject (the one
`--vm: unsupported: ...` contract every reject shares) prints the
lane as SKIP (visible, still green), and anything else fails — so
each test's VM lanes light up on their own as the slice grows. The
generated difftest corpus stays two-lane for now: its chunks are
textually prepended with the probe preamble, which sits far outside
the slice (classes, method calls), so every chunk would reject at
compile time; a per-case skip mechanism only earns its keep with
Phase 2 coverage, and that is where it belongs.

## 11. Phase 2: full parity — results

Run 2026-08-10 through 2026-08-17 on the same `vm-spike` branch, in
46 batches, one construct family per batch. §7 states the bar: *every
gate that today runs interp + JIT runs all three, green, and the
corpus finds no divergence.* That bar is met. This section records
where the gates stand, what the bytecode grew into, what the third
lane found in the other two, and what is still outside the slice.
§10's postscript describes the slice as Phase 1 left it and is kept
as that record; §11.4 below is the current boundary.

### 11.1 Where the gates stand

| gate | three-lane status |
|---|---|
| generated difftest corpus | 17,262 cases through interp / `--jit` / `--vm`; 0 divergences, **0 skips** (`tools/difftest/vm_skip_ceiling.txt` is a ratchet, currently 0) |
| curated `tools/bench/vm_cases/` | 177 cases, `--vm` and `--vm-llvm` each diffed against interp, then the same sweep again under `CULEBRA_GC_STRESS=1` |
| `tests/*.cul` symmetry, and the isolate suite | four lanes: interp, `--jit`, `--vm`, `--vm-llvm` |
| `just doctest` | 465 documentation blocks on all three engines (`just doctest LANE=interp\|vm\|jit\|all`) |
| `dap_test` (ctest) | the debug-adapter scenarios on both debug engines |

The three surfaces the tree-walker used to own were rebuilt as seams
rather than duplicated. The doctest runner takes a `BlockRunner` —
`(name, code) -> {ok, kind, message}` — and keeps block extraction,
stdout capture and marker matching for itself.
`dap.h` was reduced to the protocol layer over a six-question
`DebugEngine` interface (`include/debug_engine.h`), with the VM's
answers in `include/vm_debug.h`. And the REPL runs on the VM through
session cells. The user-visible surface is `culebra --vm` (a REPL when
given no file), `culebra test --doc --vm`, and `culebra dap --vm`;
`--vm` and `--vm-llvm` remain hidden flags, so `language.md` and the
CLI documentation are unchanged.

### 11.2 What the bytecode grew into

142 opcodes; `include/vm.h` is ~14.9k lines. Three structural
decisions carried most of the surface area, and are the parts worth
knowing before Phase 3 touches this format:

- **The stdlib arrives through the runtime layer, then through the
  preamble.** Phase 1's `NsGet` reached every native builtin global
  by name. Phase 2 added the second half: the VM compiles the stdlib
  preamble itself, which is what made `Path`, `Regex`, `Vector`, the
  `assert_*` family and the effects runtime work. §8's lazy-stdlib
  question resolved the way it predicted — resolution happens when
  the compiler sees the name.
- **Built-in methods are a table, not code.** One `BMethSpec` row per
  `(name, argc)` — receiver tag mask, per-argument declared type,
  optional-argument defaults, an id — drives the executor, the LLVM
  lowering, and the reject decision from a single place, carried by
  three ops (`MethGate` → `ChkParam` → `BMeth`). The invariant that
  makes it safe is narrow and easy to get wrong: **a spec's receiver
  mask must equal the set of receivers that resolve that name at that
  arity**, because everything outside the mask is answered as a method
  miss. Adding a `BParam` kind means adding a case in *both* the
  executor's predicate and the lowering's switch; a missing case falls
  into a `default` that silently accepts the wrong types on one lane
  only.
- **The debugger reads debug tables and nothing else.** Per-binding
  live ranges (`Chunk::SlotDebug`, fed exclusively through
  `push_binding` so the ranges cannot drift from name lookup), a frame
  stack pushed by `run_frame`, and one op (`DbgStmt`) for statement
  boundaries. The part that did not follow from the tables was the
  threading: a frame's register window is the debuggee thread's
  machine stack and the GC is thread-local, so every debugger query
  runs *on the parked debuggee thread* through a job pump — the old
  implementation querying from the DAP thread had been true by luck.
  `evaluate` reuses the REPL session machinery, which is what makes
  `setVariable`'s ImmutableError correct without a second
  implementation of name resolution.

### 11.3 What the third lane found

§2 predicted that a third implementation would surface asymmetries
the two hand-maintained ones had kept between them. It did, in nearly
every batch that reached a new construct, and the method that produced
them is the transferable part: **before compiling a construct, run a
probe of it on interp and the JIT and diff the two.** A batch's probe
was 50–200 one-line programs; where they disagreed, the fix landed in
`interpreter.h` / `jit.h` as its own commit *before* the VM work
started, so the VM was never written against a moving target.
Representative findings:

- A `for` body of exactly one statement never reached a GC safe point
  in the interpreter. The parser collapses a one-statement body, and
  the statement dispatch was the only poll site, so
  `for i in 0..4M { let a = [i] }` held 1,406 MB where it should hold
  34 MB — 5,094 MB when the body was a call to a one-statement
  function.
- The same collapse, on the debugger side: no breakpoint could land in
  a one-line lambda, a one-statement `try` body, a `catch` arm, or a
  `cond` / `match` arm.
- `next()` past the end of a drained iterator threw `StopIteration` in
  the interpreter and returned nil in the JIT — for all ten built-in
  iterator sources, and *inside the interpreter itself* between
  built-ins and generators. Settled as nil, documented in
  `language.md` §18.5.
- Keyword arguments to built-in methods: the JIT compiled `KWARG`
  nodes as positional values for every shape but one, so
  `'ab'.truncate(max: 3)` disagreed. Built-ins now accept a keyword
  only as a kw-only parameter name, decided by one predicate.
- Argument-list evaluation order: an object-ish receiver miss skipped
  argument evaluation in the JIT at ~25 sites, where the interpreter
  runs the whole list first. Only a scalar receiver's error precedes
  its arguments.
- Name resolution: a declaration takes effect from the point it runs,
  not across the whole statement list. Six shapes disagreed, one of
  them with no closure involved at all.
- A `Range`'s `class` slot held a bare `static const char[]` in the
  JIT, so a structural `==` on two ranges read a length prefix that
  was not there and died.

The VM's own bugs are less interesting except for one class, which is
structural: **an unverified LLVM module runs anyway.** `run_program`'s
`verifyFunction` call had been discarding its verdict since it was
written; giving it a stream and letting it throw immediately exposed a
cross-chunk `alloca` leak in the lowering that every gate had been
green over.
`culebra --vm-llvm --emit-llvm f.cul | opt -passes=verify` is the
standing check for lowering work, and IR diffing against `-O0
--emit-llvm` is the standing check for refactors that must not change
codegen.

### 11.4 The slice closed, and what that took

The grammar is in. Every one of the 204 `tests/*.cul` compiles and
runs on the VM lanes: no module rejects, and no function literal is
left as a poisoned chunk. (207 and 17,262 after the branch was rebased
onto master, which brought its own tests and corpus cases with it.)

Getting there meant reading the gate carefully, because a green gate
was not saying what it looked like it was saying. The symmetry gate
excuses a file's mismatch as a skip on either of two signals: the run
printed `--vm: unsupported:`, or *the dump holds a poisoned chunk
anywhere in the file*. The second one is the wide net — one
out-of-slice construct inside one lambda excuses every divergence in
the file, including divergences that have nothing to do with it. The
last four constructs to land (typed variable declarations, Generic
type parameters, `@`, multi-module scripts) each removed a poison
marker and, in doing so, published divergences that had been sitting
behind it:

- a compound assignment whose step mutates a Tensor in place was an
  ImmutableError on the VM and a mutation on both other engines — and
  through an immutable *property* it was a mutation-then-throw on the
  JIT, the worst of the three;
- `return` / `break` / `continue` in expression position (`let y =
  return e`, `f(return e)`, `[1, break]`) compiled on the VM as if
  they produced a value and fell through, where both other engines
  leave the frame. The VM had no diverging-statement arm in
  `compile_expr` at all;
- a Generic's type parameter in the return position (§11.3's list)
  came from the same sweep.

The lesson generalises past this project: **a skip predicate that
keys on a whole file is a mask, not a filter.** Whatever it excuses,
it excuses completely.

Every divergence this phase surfaced is closed but one, and the
exception is closed by construction rather than by a fix. A diverging
expression in receiver position (`(return x).size()`) leaves the frame
on all three now — `eval()` bails on a pending completion at entry, and
a postfix chain, which dispatches on the value in hand rather than
through `eval`, had to learn the same check. `[self] = [5]` inside a
function is ImmutableError everywhere and a declaration at the top
level everywhere. A native builtin reports its real signature —
derived from the canonical interpreter parameter list that already
feeds the JIT's binder, answered through the closure-keyed seam the
VM's chunks established. An `Isolate.spawn` handle carries its id
visibly on every engine, which is what a `Channel` endpoint and an
`FS.watch` handle already did.

The one left is the JIT's alone: two sibling `if` arms that bare-assign
the same name, where a closure captures it, raise ImmutableError there
while the interpreter and both VM lanes bind it correctly. The runtime
sentinel that fixed the uncaptured case is documented in its own code
as never set for cell slots, and setting it there means moving where a
cell is created — which is what gives a captured loop variable a fresh
cell per iteration. Phase 3 removes the question instead of answering
it: once `jit.h` lowers bytecode, the JIT inherits the VM's decision,
which is already right.

Phase 3 — rewriting `jit.h` to lower bytecode instead of AST — began
where this section ends; §12 is its record.

---

## 12. Phase 3: folding the JIT onto the bytecode

Started 2026-08-18 on branch `vm-phase3`, from the `master` commit
Phase 2 landed at. Both compiled entries — `--jit` and `culebra build`
— lower bytecode (§12.2), and the AST codegen they left behind is
gone (§12.4): `include/jit.h` went from 16,446 lines to 5,271, and one
consumer reads the AST, which is what §7 asked for. §12.5 is what the
phase still owes.

### 12.1 A lane's exit code is half of its answer

The phase opened with a probe rather than a change: all 207
`tests/*.cul` through `--jit` and `--vm-llvm`, diffed. It reported
203 matches, four differences, no skips — and the four should not
have been news, because the symmetry gate runs that comparison on
every `just test-dev`.

It ran it with the VM lanes' exit status thrown away. Only stdout was
compared, so a lane that printed nothing and then died read as equal
to the interpreter: an uncaught throw (rc 255) and a segfault
(rc 139) both leave stdout empty, which is exactly what a file whose
asserts all hold prints. The status was dropped for a reason — an
out-of-slice reject also exits nonzero, and the sweep wants that to
be a skip. The fix is to ask the skip question about the status too
rather than not asking: rc≠0 enters the same mismatch branch stdout
does, and `vm_skipped` decides skip-or-fail there as it already did.
The report gained the rc and the lane's stderr, without which a crash
reads as an empty diff.

Three files had been green this way, and all three were one
mechanism. An `if` / `cond` hoists what its arms declare, because
neither opens a scope for them; the compiler minted a cell per hoist
and chained the cells through `Binding::shadowed`. So a name declared
inside nested arms got one cell per level, and a name two sequential
`if`s both declare got one cell each, where the JIT registers a
single `VarSlot::runtime_decl` slot per name and lets a later arm
find it. The chain also crashed: a nested hoist's `CellNew` sits
inside one arm, so a sibling arm reading the name walked the chain
into a cell its path never allocated and dereferenced a zeroed slot —
`tests/test_args.cul` segfaulted on any short non-Bool option,
through the two structurally identical option arms in
`Args.try_parse`.

The repair follows the JIT twice over. One cell for the name, shared
by every arm — and, since several arms now write one binding, the
compiler stops answering for its mutability: which declaration ran is
this call's fact, so it goes in a slot beside the cell
(`Binding::mut_slot`, the JIT's `VarSlot::mut_alloca`). A declaration
records its own `mut` there; a bare write asks the cell whether any
declaration has landed at all before checking it. Without the runtime
bit the last arm compiled spoke for every arm, and
`if a { let n = 1 }` / `if !a { mut n = 2 }` accepted a write the
interpreter refuses. The third divergence was adjacent: a declaration
must not write through a borrowed capture, which reads as a cell
binding like any other, so `fn () { let sh = sh + 1 }` had been
assigning the enclosing `sh` instead of shadowing it. Owning the cell
is what distinguishes the two.

### 12.2 Both compiled entries lower bytecode

`--jit` is now `run_modules_via_llvm` and `culebra build` is
`build_object_from_modules`. The corpus had been checking that
lowering as `--vm-llvm` since Phase 2, so the behavioural risk was
small and the behavioural result was uneventful: 17,262 cases,
`interp == jit`, from the first run.

Three things the AST entries had and the lowering did not:

- `--jit-faststart` and the backend object cache. Both are properties
  of how a module is executed rather than of how it was built, so
  `run_program` took `fast_codegen` and a module name, and
  `JIT::jit_module_name` keys the cache from the same sources as
  before.
- **Parameter metadata that outlives the compiler.** The lowering
  registered `&VmProgram::param_metas[i]`, a pointer into the
  compiling process's heap — fine for a JIT, dangling for an object
  file, and the only real obstacle AOT presented. The AST path
  already emitted that metadata as globals of the module being built
  (`emit_param_meta_global`); the lowering uses it, and the function
  lost its last AST parameter on the way (it only ever asked whether
  a default existed). The rest of the lowering was audited for the
  same mistake: every other host value reaches the module through a
  runtime extraction, and this was the only pointer burnt into IR.
- The module/program split, hand-written in three places — main.cc's
  script lane, its doctest lane, and the AOT driver.
  `Compiler::compile_modules` takes the loader's list and peels the
  spliced `<stdlib>` prologue off the front once.

One mechanical trap is worth recording: `IRBuilder::CreateGlobalString`
takes its module from the insertion point's basic block, so it cannot
be called before lowering starts. The metadata globals are built at
the first `MakeClosure` site that needs one and cached, not in a
prepass.

### 12.3 Moving a consumer moves what the gates measure

That was the cost of the switch, and none of it was visible in a
behavioural comparison. `--jit` is a lane several gates name
explicitly; when the lane changed engines those gates began measuring
a different implementation, and what the bytecode path had been
getting wrong in private became a regression in public. In the order
it surfaced:

- **leak-fuzz** (corpus RC-leak regression, interp against the JIT
  lane) reported 11 new leaks in two clusters, both pre-existing —
  `--vm` leaks them identically — and both the same shape, a rule
  stated in one place and not kept on one path. An explicit
  `x.drop()` borrows its receiver and leaves the statement sweep to
  release it, but inside the UFCS dispatch the candidate arm hands
  that same receiver to a call whose `Take` drops the temp from the
  sweep list for *every* arm, so the drop arm stranded the `+1` (ten
  receiver shapes). And a for-in destructuring bind runs per
  iteration while a nested pattern's intermediate container was left
  to a sweep that fires once for the whole loop, stranding it on
  every iteration but the last. `leak_baseline.txt` shrank by one on
  the way: a nested-fn recursion case the AST path leaked and the
  bytecode path does not.
- **leak-abort-suite** is a different gate, not a stricter one:
  leak-fuzz discards a case whose warm-up throws, so it does not see
  throw paths at all. It found three more clusters. Two were runtime
  helpers that release the value they are handed on every normal exit
  — `culebra_runtime_set_add_method`,
  `culebra_runtime_object_remove_any` — and therefore own it, but
  left the unwind edge out; hashing the argument is where an
  unhashable one raises, which is exactly the path that skipped the
  release. `JitUnwindRelease` is the RAII form of that convention and
  now covers both. The third is the transferable one: **a shared
  emitter brings its own cleanup assumptions with it.**
  `emit_for_open_protocol` holds the iterator in an `Owned` handle
  while it validates the protocol — its comment says so, and that is
  what makes a rejected protocol throw with the `+1` still in flight
  — but that pool lives in the LLVM function, where the bytecode temp
  table the VM's cleanup pads stand on cannot see it, and nothing
  drained it. Each scope pad now drains the pool as the AST path's
  pads do; releasing nil-clears a slot, so an enclosing pad draining
  again is a no-op.
- **Two capacity limits sized for an opt-in lane.** The rc-leak
  battery (`tools/analysis/gc_leak_check.sh`) reported `error` on all
  40 rows, which is not a leak but a failure to measure: its pattern
  file compiles to a 382-slot frame and `kMaxSlots` was 256, so
  `--jit` rejected a program the AST path had compiled. Nothing about
  the format wanted 256 — an instruction's operands are `int32`, and
  both engines size their register window from the chunk's own
  `num_slots`. What the number bounds is how much machine stack one
  executor frame may claim, and the frames that approach it are top
  levels, whose slots count their bindings and temps and which never
  recurse. It is 8192, and overflow stays a clean `VmError`.
  `kMaxOwnedDepth`, the nested-scope depth, was the same limit in the
  same disguise: 64, where the interpreter accepts 200 with no limit
  at all, and it stayed at 64 because `run_frame` declared a fixed
  `int64_t marks[kMaxOwnedDepth]` — charging every frame for the
  deepest nesting the budget allows. That is the trap Phase 2 already
  fixed for the register window, one array over. Sized from the
  chunk's own `owned_depths`, the budget can be 1024 while only a
  frame that nests that deep pays for it.
- A leak this work itself introduced, visible the moment the battery
  could run again: 5,086 live objects where 89 were expected, one per
  pass. `emit_conditional_rebind` opened a temp scope for its
  sentinel probe, so the slot number rolled back when the scope
  closed and the read the assignment returns took that same number,
  overwriting the probe's `+1` rather than releasing it. In a
  register VM a temp scope is a numbering as much as a lifetime; the
  identical probe in `assign_shadowing` already lived in the
  enclosing statement's temps, which is where this one belongs.
- **A counted `for i in a..b` never checked its bounds were Longs.**
  The fast path walks a Long counter. `compile_range` `ChkLong`s each
  endpoint right where it compiles it — for the evaluation order that
  puts a bad bound's error before a later bound's side effects — and
  the counted path was the one range site without those checks. So
  `for i in 1.5..3` ran no iterations and said nothing where the
  interpreter raises, and `for i in 1..3 by 0.5` walked a step of its
  own invention, which loops forever: that is what timed
  `jit_error_pos_test` out rather than merely failing it.

The general form: **a green gate is scoped to a lane, and a lane is
an implementation, not a name.** Behavioural equivalence carried over
on the first run, because the corpus had been comparing that lowering
for a phase already. Nothing that names `--jit` did.

### 12.4 Deleting the codegen

`include/jit.h` went from 16,446 lines to 5,271; `stdlib_jit.h` lost
1,996; three fragment headers (`jit_compile_assign.h`,
`jit_compile_class.h`, `jit_compile_fn.h`) went entirely.

Membership is decided per overload and per parameter: a member goes if
it takes an AST it cannot do without. "Mentions `peg::Ast`" is the
wrong predicate twice over. Several value-based helpers carry a
`const peg::Ast* at = nullptr` tail for error positions
(`emit_type_check`, `emit_object_set`, `compile_function_call_raw`),
and several names carry both an AST overload and a value overload the
lowering still calls. Either mistake takes live code with it, and the
build says so — but only after a full rebuild, which is why the
predicate is worth getting right before the first cut rather than
after.

The mechanical part had failed once already, on brace counting: string
literals and comments in this file hold braces, so a counter that
believes them walks off the end of a member and takes the class's
closing brace with it. What works is indentation — clang-format puts
every member of a top-level struct at two spaces and closes its body
with a line that is exactly `  }` — which is exact, and cheap enough
to re-run from a clean tree each time the predicate is refined.

Three things the cut surfaced that a smaller change would not have:

- **The AST codegen extended past `jit.h`.** `stdlib_jit.h`'s
  `JitExtension` compiled `Math.sin(x)`, `IO.print(x)` and the bare
  globals from their ASTs, reached through eight `ExtensionHooks`
  function pointers. Six of the eight took an AST; the hook table is
  down to the two that do not (`declare_runtime`, `is_builtin_var`).
- **What the deletion stranded was larger than the deletion.**
  Removing the AST members left some sixty more members, structs and
  fields that only they had called: closure building, constructor
  emission, the variable-slot constructors, the safe-navigation
  helpers, an unwritten call-root position. C++ says nothing about a
  private member nobody calls, so the way to find them is to iterate —
  scan for members with no call site, delete, scan again — which took
  four rounds to reach a fixed point.
- **Two public entries had to come back.** `JIT::run` and
  `JIT::build_object` are what [`deployment.md`](../deployment.md)
  tells embedders to call. They are declared in `jit.h` and defined in
  `vm.h` over the bytecode lanes, so the documented API is unchanged
  and the engine underneath it is the new one.

Two helpers stayed but moved. `ArgScan` and `scan_arg_list` are AST
*analysis* rather than codegen and belong to the bytecode compiler
now: the struct and the scan sit in `parser.h` beside
`check_arg_list`, and the built-in keyword predicate moved into the
compiler itself.

The ratchet that compared the two AST walkers was re-pointed rather
than retired. `tools/check_dispatch_symmetry.sh` diffed interp's
`_eval_dispatch` case labels against `JIT::compile`'s; it now diffs
them against the compiler's two switches (statement position and
expression position). The allowlist shrank to one tag, `STRING`, which
the interpreter folds through its `is_token` fallthrough.

The deletion is verified by IR rather than by the gates: `--jit -O0
--emit-llvm` over all 207 `tests/*.cul`, byte-identical before and
after, stderr included. That is what makes a fourteen-thousand-line
deletion a mechanical change instead of a risky one — a passing gate
would only say the tests still pass.

### 12.5 Faster code, slower compiles

`--jit` reaches LLVM through a different front end now, so the phase
owed a measurement. Both binaries are `just build` (-O3 + LTO) from
this tree — master's AST codegen against the branch's bytecode
lowering, the same LLVM underneath — hyperfine, 10 runs, idle machine:

| bench | master (AST) | branch (bytecode) | |
|---|---|---|---|
| `for_range.cul` (25M iterations, minimal body) | 478.5 ms | **106.8 ms** | 4.48× faster |
| `for_range_dense.cul` (4M, dense body) | 343.4 ms | **149.9 ms** | 2.29× faster |
| `fib.cul` (fib(28), recursion) | 585.9 ms | 603.1 ms | 1.03× slower |
| `hello.cul` (startup + compile) | 41.3 ms | 55.3 ms | 1.34× slower |
| `tests/test_core.cul` (compile-dominated) | 3.58 s | 5.32 s | 1.49× slower |

The split is clean: **loop-heavy code runs several times faster, and
everything pays more to compile.** The runtime win is the counted loop
— the lowering emits the shape the optimizer wants — and `fib` shows
that a call-dominated program is unchanged, which is what "same LLVM
underneath" predicts.

The compile-time loss is one cause measured twice. The lowering emits
**about 30% more IR** than the AST codegen (`test_core` 329,785 lines
against 429,311 at `-O0`; `hello` 3,329 against 4,218), and that
volume is paid once in codegen and again in the optimizer (a separate
run, so its `-O2` row sits a little above the table above):

| `test_core` | master | branch | |
|---|---|---|---|
| `-O0` (no optimizer) | 3.18 s | 4.22 s | +1.03 s |
| `-O2` (default) | 3.69 s | 5.33 s | +1.64 s |
| → what the optimizer costs | 0.51 s | 1.11 s | +0.60 s |
| `-O0 --jit-faststart` | 1.26 s | 1.70 s | +0.44 s |

The ratio holds near 1.3–1.4× at every optimization level, including
the fast-codegen path, which is what a volume problem looks like: no
single pass is at fault. So the target for a later cycle is the IR the
lowering emits per bytecode op, not the front end that produces the
bytecode — the front end itself is not where the second is going.

### 12.6 What Phase 3 still owes

- **The IR volume above.** A JIT pays its compile time on every run,
  so 30% is worth going after.
- `--vm-llvm` is the same engine as `--jit` today, which makes it a
  duplicate flag rather than a lane. Retiring it means re-pointing
  every gate that names it.
- One defect that is not this phase's but is on the VM: under
  parallel load `tests/isolate/test_lazy_ns_parse_race_jit.cul`
  segfaults on `--vm` alone — 7 runs in 40 at ten processes wide,
  against 0 in 40 on each of interp, `--jit` and `--vm-llvm`.
