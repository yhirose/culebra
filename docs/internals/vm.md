A Shared Bytecode VM: Design Proposal
=====================================

**Status: Phases 0–4 done and merged to `master`.** Sections 1–9
record the motivation, the target architecture, and the migration
plan for replacing the tree-walking interpreter with a bytecode VM
that shares its value representation — and its front end — with the
JIT. The Phase 0 spike (§7) answered both exit questions yes
([§10](#10-phase-0-spike-results)), Phase 1 built the third backend
on the same branch (§10's postscript), and Phase 2 took it to the
parity bar §7 sets ([§11](#11-phase-2-full-parity-results)). Phase 3
moved both compiled entries — `--jit` and `culebra build` — onto the
bytecode and deleted the AST codegen they left behind, meeting §7's
exit criterion: one consumer reads the AST, and it is the bytecode
compiler ([§12](#12-phase-3-folding-the-jit-onto-the-bytecode)).
Phase 4 retired the tree-walking interpreter itself — its body, the
`--tree` flag, and the oracles that were still pinned to it — leaving
the bytecode VM as the sole executor
([§13](#13-phase-4-retiring-the-tree-walker)). The observable language
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
13. [Phase 4: retiring the tree-walker](#13-phase-4-retiring-the-tree-walker)

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

A body's *own* name takes a different route from its cell. A `fn name`
body never captures the dispatcher cell: `FuncInfo::own_name` seeds the
name as a local of the body and the prologue binds it from the dispatch
(`MfSelf`), because the capture would be a ring — cell → dispatcher →
body → cell — that refcounting cannot break. `let name = fn …` closes
the same ring through its own closure, and the leak gate's absolute
per-lane measure (§13) showed it: one cell and one closure retained per
call, on both lanes, for every self-recursive `let`. So a literal that
directly initializes an immutable binding now goes through the same
seam — its own name is a local bound to the frame's running closure
(the `fn` handle's slot), cell-promoted when a nested closure captures
it. The read is observably the binding's, since a `let` the statement
list declares once can never be rebound; the two places a rebinding IS
possible are exactly where the cell stays — a second declaration of the
name in the same list, and a REPL session's top-level `let`, which a
later line may redeclare into the same session cell.
`tests/test_fn_own_name.cul` pins the corners.

A class member closes the widest of these rings — cls → ctor → meta →
method → cell → cls, one class object, meta and method-closure set
retained per call for a class declared in a function (the gate's dmd
entries) — and takes the same seam through its *receiver*: `ClsSelf`
binds the class name from the instance's own class object, or from the
class itself in a static's frame. That took two structural moves. An
instance now holds a +1 on its class object (`JitObject::cls`, sharing
the trailing slot with `ns_name` — the roles are exclusive — to stay
inside the 128-byte slab class), which is what keeps `M` answerable
from a method after the declaring scope died; and both construction
spellings hand the class over as the constructor's receiver (`C(...)`
now does what `C.new(...)` always did), which is where the ctor reads
it. A member run with no receiver of its class — a decorator that
stashed the raw closure, an instance another isolate rebuilt — reads
the NameError sentinel, the multifn precedent. Sending keeps its old
answer through a meta flag (`names_class`): an instance whose methods
name the class ships the class object along and is refused on its
native constructor, exactly where the old cell capture was; every
other instance leaves its class behind, as before. Decorated classes
keep the capture (the binding is the decorator's result), like
decorated `fn`s — and so does a REPL session's top-level class, which
a later line may redeclare, the same corner the `let` literal leaves
to the cell. `tests/test_class_own_name.cul` pins these.

The last of the gate's compiler-made rings was not in a compiler at all
but in the source-to-source transforms (§8). A generator / effect body
becomes a state machine whose locals live on a state instance, so a
closure written in the body read them as `self.<name>` and captured the
instance — while the instance held the closure back, because `let h =
fn …` is a body local too. The effects transform closed the same ring by
hand for a named fn, wrapping the declaration in an IIFE that passed the
instance in as `_eff_self`. Two changes retire both. A named fn now
*stays a declaration*, emitted where it was written and then stored into
its promoted slot — the only spelling that can be a generator, and its
recursion binds to the declaration (the multifn uplink) rather than
reading back the slot it was just stored into. And a promoted local that
any closure in the body reads is boxed: the state instance holds a
one-field box, each state-machine method binds it once at entry as a
plain local, and the closure captures *that*. Neither end reaches the
other, so the ring is gone without changing what the closure sees —
a body with no closures lowers exactly as before. The rings that remain
in the gate's baseline are the ones an ordinary program writes in the
same shape: mutually recursive `fn` declarations ring through their
dispatcher cells wherever they appear, and the effect-body twin measures
the same as the plain one. `tests/test_generator.cul` and
`tests/test_effects_resume.cul` pin the corners — writes through a
closure, a defer body, multi-shot resume over a boxed local, a named fn
called from a closure declared before or after it, and a local read by a
closure and a nested handle at once.

One thing a box is not free to change is what a *fork* isolates. A
multi-shot `resume` forks by shallow-copying every frame, so a promoted
scalar was copied per fork; a box is a heap value, and the copy aliased
it — two forks would have written the same local. The frame therefore
finishes its own clone: every computation now also exposes
`_eff_refork()`, which re-boxes its boxed locals, and the driver's
`_fork` calls it on each copy. What the body itself shares — an array it
pushes into — stays aliased across forks, as it always did.

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
| generated difftest corpus | 17,262 cases through interp / `--jit` / `--vm`; 0 divergences, **0 skips** (the skips were a ratchet; it reached 0 and was retired in §13.5) |
| curated `tools/bench/vm_cases/` | 177 cases, `--vm` and `--vm-llvm` each diffed against interp, then the same sweep again under `CULEBRA_GC_STRESS=1` |
| `tests/*.cul` symmetry, and the isolate suite | four lanes: interp, `--jit`, `--vm`, `--vm-llvm` |
| `just doctest` | 465 documentation blocks on all three engines (`just doctest LANE=interp\|vm\|jit\|all`) |
| `dap_test` (ctest) | the debug-adapter scenarios on both debug engines |

This is the table as Phase 2 left it. The `--vm-llvm` lanes in it are
`--jit` lanes today — §12.11 says why the flag went.

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
`culebra --jit --emit-llvm f.cul | opt -passes=verify` is the
standing check for lowering work, and IR diffing against `-O0
--emit-llvm` is the standing check for refactors that must not change
codegen. (The lane was spelled `--vm-llvm` until §12.11 retired it.)

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

- ~~**The cleanup that is left.**~~ Settled, and not by a fix: the IR
  volume of §12.5 is gone (§12.8), the residual `-O2` second is
  machine-code generation rather than the optimizer (§12.9), and it is
  register pressure rather than instruction count (§12.10). It is the
  price of promoting the bytecode register file to SSA, which is the
  same property that makes the lowered code faster to run. Four pad
  shapes were measured; the one in the tree is the best of them.
- ~~`--vm-llvm` is the same engine as `--jit` today, which makes it a
  duplicate flag rather than a lane.~~ Retired (§12.11).

### 12.7 A shared cell is a shared heap

One defect was on the VM rather than on this phase: under parallel
load `tests/isolate/test_lazy_ns_parse_race_jit.cul` segfaulted on
`--vm` alone — 7 runs in 40 at ten processes wide, against 0 in 40 on
each of interp, `--jit` and `--vm-llvm`. The lane split was the whole
clue. A closure the executor builds reaches its bytecode through a
descriptor in capture 0, and `Exec::prepare` minted one descriptor cell
per chunk for the entire program: every closure built from that chunk
retained that one cell. The lowered lanes have no such cell — each
chunk is its own native function, so the fn_ptr says everything.

A shared cell is a shared heap object under a discipline that has none.
`JitCell::refcount` is a plain `int64_t`, because the runtime is
single-threaded by construction: one `Runtime` per isolate, each with
its own slab and its own GC heap. When a `Parallel.map` starts
twenty-four children and every one of them resolves `Regex`, `Path`,
`Term`, `Time` and `Canvas` on its own thread, the closures in those
module bodies retain and release the parent's cell at once. Lost
updates drive the count to zero while references remain, and whichever
child gets there frees the parent's memory into its own slab.

ThreadSanitizer named it on the first run — 59 reports, every one on a
cell `Exec::prepare` had allocated, from the two sites that retain one
(`MakeClosure` and the lazy-namespace builder rebuild). ASan had
nothing to say, the same split the earlier parse-ledger races showed.

The fix is to stop sharing. The descriptor rides in a cell of the
closure's own, like every other capture and like the deserializer had
always rebuilt one; `desc_cells`, its pin, `release_descs` and the run
guard go with it, and the lazy-namespace registry keeps the descriptor
value rather than the cell so each Runtime builds a module through a
cell it allocated itself. That is 27 lines less code. The price is one
cell allocation — a slab pop plus a GC registration — per closure
built: 15% on a loop whose whole body is building and calling one (3M
iterations, 0.41s to 0.47s), and nothing measurable on `tests/perf`,
where a closure is built once and called in the loop.

TSan goes to 0 reports on that test and 0 across all twenty isolate
tests; the stress repro goes from 7/40 to 0/80.

### 12.8 The IR volume was all cleanup

§12.5 left "about 30% more IR" as a volume to go after without saying
where it sat. Attributing a `-O0` module to its basic-block families
answers that in one pass. For a hello-world, of 2,763 instructions:

| block family | branch | master (AST) |
|---|---|---|
| unwind cleanup | **1,045 (37.8%)** | 291 (15.5%) |
| everything else | 1,718 | 1,581 |
| landing pads | 38 | 13 |
| `_Unwind_Resume_or_Rethrow` | 39 | 8 |

The non-cleanup IR was within 8% of the AST path's. The whole
regression was in the pads — 86% of the excess instructions.

The cause is a shape, not an amount of work. A throw abandons whatever
temporaries are in flight, and the lowering asked for that set at every
site that can throw, memoized it, and gave each distinct set a landing
pad carrying its own straight-line copy of the releases and its own
re-raise edge. The AST codegen had spent its cleanup differently: one
pad per scope, descending a chain of one-slot release blocks entered at
the rung the pad needs (`fn.release.3 → fn.release.2 → fn.release.1 →
fn.unwind`), so a slot's release exists once no matter how many sites
abandon it.

The chain was already there for the frame's bindings — `CleanupPad`
documents "a multi-entry region (the frame's ladder) points the builder
at the end of its shared descent chain". The temporaries just were not
using it. They are now: one chain per scope, a rung per temporary
shared by prefix (the sets of a scope are stacks over a common floor,
so a set that is another's prefix costs no rung of its own), a single
re-raise at the foot, and an entry that is a landing pad and a branch.
The pad index is keyed by what it releases rather than by the span it
was asked for, so two statements abandoning the same temporaries share
one entry.

| `tests/test_core.cul` | master (AST) | branch before | branch after |
|---|---|---|---|
| `-O0` IR instructions | 234,293 | 353,220 | **232,289** |
| `-O0` compile | 3.06 s | 4.05 s | **2.99 s** |
| `-O2` compile | 3.56 s | 5.30 s | **4.35 s** |

The volume is gone: the lowering now emits marginally less IR than the
codegen it replaced, and at `-O0` it is no longer slower to compile.
What is left is the `-O2` row, and it is a different problem from the
one §12.5 identified — §12.9 takes it apart.

### 12.9 The rest of `-O2` is not the optimizer

Subtracting the `-O0` compile from the `-O2` one and calling the
difference "the optimizer" is wrong, and it sent §12.5 after the wrong
half. Both runs also generate machine code, and they generate it from
different IR: `-O0` hands the codegen the module as emitted, `-O2`
hands it a module a third the size. The difference is optimizer time
*minus* whatever the codegen saves on smaller input, and those two move
independently.

Run them apart instead. The JIT's pipeline is
`buildPerModuleDefaultPipeline(O2)`, which is what `opt
-passes='default<O2>'` runs, so the same `-O0` module each binary emits
can be pushed through `opt` and then `llc` outside culebra
(`tests/test_core.cul`, wall clock):

| | master (AST) | branch (bytecode) | |
|---|---|---|---|
| `opt -O2` | 1.48 s | 1.58 s | +7% |
| `llc` on the `-O0` IR | 3.13 s | 2.81 s | branch cheaper |
| `llc` on the `-O2` IR | 1.76 s | **2.36 s** | **+34%** |
| opt + llc at `-O2` | 3.24 s | 3.94 s | 1.22× |

The last row is culebra's own 3.56 s against 4.35 s, same ratio, and
the `-O0` row is culebra's own 3.06 s against 2.99 s — the branch
really is cheaper there. **The optimizer was never the problem: it
costs 7% more. Machine-code generation costs 34% more.**

Why: codegen is superlinear in function size, and the two modules
divide the same work differently.

| after `-O2` | master | branch |
|---|---|---|
| instructions | 95,925 | 102,699 |
| functions | 199 | 148 |
| Σ n^1.9 over function sizes | 1.393e8 | **1.838e8 (1.32×)** |

1.32× predicted against 1.34× measured. The exponent is the one
[the test splits] already established for this codebase's compile
times, so the fit is not a coincidence — it is the same law seen from
the other end.

Two things follow, and they point opposite ways. The branch's smaller
function count is not a defect: the AST codegen was **duplicating**
callback bodies. `xs.map(fn (x) { … }).filter(fn (x) { … }).sum()`
makes master emit four copies of each lambda — one per dispatch arm —
where the lowering emits one chunk each (15 functions against 9 on that
line alone). Deduplicating is right, and it is why the branch's
non-cleanup IR is 9,379 instructions *smaller* after `-O2`.

What is left is cleanup, again, and §12.8 did not finish it:

| after `-O2` | master | branch |
|---|---|---|
| cleanup instructions | 8,826 (9.2%) | **24,979 (24.3%)** |

The 16,153-instruction excess is more than the 6,774 the branch is
larger overall. Scale the branch's functions down by removing it and
Σ n^1.9 falls by a factor of 0.73, which puts `llc` at 1.72 s against
master's 1.76 s — **the whole remaining gap is that cleanup**.

Closing it means fewer landing pads, not shorter ones: §12.8 shortened
each pad, but there is still one per distinct set of abandoned
temporaries where master had one per scope. Master could afford that
because its pending temporaries lived in dedicated slots
(`build.guard`) that nothing else ever reuses, so one pad draining them
covers every site in the scope. The lowering's temporaries are
bytecode registers, and a register is reused across generations — the
executor's own unwind says so, in the comment explaining why a
temporary nils its slot ("safe on an index a later generation turned
into a cell"). Releasing a scope's union of temp slots would therefore
release a later generation's *cell* with a plain release. So the
per-site sets are load-bearing, and collapsing them means giving the
lowering unwind slots of its own rather than reusing the register file
— a change to the temp discipline both VM lanes share, which is a
larger piece of work than §12.8 was.

[the test splits]: the ~N^1.9 in single-function size that made the
JIT-lane test files worth splitting.

### 12.10 The throw path costs registers, not instructions

§12.9 ends on a prediction — remove the cleanup excess and the gap
closes — and names one landing pad per scope as the way to do it. Both
halves were built and measured. The prediction is wrong, and the way it
is wrong says what the residual second actually buys.

Start with the obstacle §12.9 named, because it turned out to be
removable. A coarse release cannot reach a live binding as long as no
register is ever both a temporary and a binding of the same scope, and
the compiler can simply guarantee that: `Scope::temp_high` remembers
how far up the scope has spent registers on temporaries, and
`alloc_slot` steps over it. Then a step can release the union of every
temporary its segment ever held — a register the throw did not reach is
nil — and the per-site sets are not load-bearing after all. That costs
about a sixth more registers per frame, and it works.

It also produces the smallest optimized module of anything measured
here — 89,984 instructions against master's 95,925 — and takes `llc`
**33.8 seconds**. Every invoke in the scope now unwinds to one block,
and mem2reg answers a block with 705 predecessors that reads a dozen
registers with a phi per register 705 entries wide. Total phi weight
goes from 72,519 to 640,872.

So volume is not the metric. The measurement that settles what is:
leave the pads and their edges exactly where they are and have them
release *nothing*.

| `tests/test_core.cul` | `-O0` IR | `-O2` IR | `opt` | `llc` | sum |
|---|---|---|---|---|---|
| master (AST codegen) | 234,293 | 95,925 | 1.72 | 1.66 | 3.38 |
| branch, as it stands | 232,289 | 102,699 | 1.84 | 2.42 | **4.26** |
| branch, pads release nothing | 202,134 | 81,935 | 1.43 | 1.60 | **3.03** |

The EH structure is free. **The entire gap is what the pads read.**

`llc -time-passes` says where it goes: Greedy Register Allocator 0.43 s
against master's 0.17, Register Coalescer 0.29 against 0.07 — 0.47 s of
the 0.76 s difference. Instruction selection is the same on both. And
the assembly shows it directly: 43,703 stack references against 19,790
with the pads emptied, over the same 175 functions. A value live into a
landing pad has to be spilled — the unwinder restores callee-saved
registers per the frame's CFI and nothing else — so every register a
step still needs crosses the edge in memory, at every site that can
throw.

That is why the AST codegen's throw path is cheap to compile and this
one is not. Master keeps far more of its frame in memory: 6,051 stores
survive `-O2` against the branch's 2,726. Its scope slots were never
SSA values to begin with, so releasing them at a pad costs no live
range. The lowering promotes the bytecode register file with mem2reg —
which is exactly what makes the lowered code 2–4.5× faster than the
codegen it replaced (§12.5). The compile-time bill and the run-time win
are the same fact.

Four pad shapes, all measured on the same file:

| pad shape | `-O0` IR | `-O2` IR | max phi | `opt`+`llc` |
|---|---|---|---|---|
| one per abandoned set, shared chain (§12.8) | 232,289 | 102,699 | 63 | **4.26** |
| one per scope, union release | 225,282 | **89,984** | 705 | 37.6 |
| one per statement, shared chain | 246,801 | 105,519 | 73 | 4.95 |
| one per statement, straight-line | 328,994 | 113,705 | 176 | 5.55 |

The shape §12.8 arrived at is the best of them, and the two axes pull
against each other: sharing a pad across sites shrinks the module and
widens the phis, splitting it narrows the phis and grows the module.
One more thing was tried and did nothing — having a temp prelude branch
straight into its scope step instead of re-raising into it, which drops
a landing pad and an `_Unwind_Resume_or_Rethrow` per prelude (4.18 s
against 4.24 s; SimplifyCFG had been folding the round trip already).

What would close it is not a pad shape. It is keeping the registers a
cleanup step reads out of SSA — which is giving back the run-time win
to buy compile time, and on a language where `--jit` compiles once and
runs a loop, that is the wrong trade. Phase 3 stops here: 1.2× to
compile, 2–4.5× faster to run.

### 12.11 Retiring `--vm-llvm`

A flag is a lane only while something runs differently behind it.
`--vm-llvm` stopped being one in §12.2, and the two were diffed before
it came out: `--emit-llvm` from each over five files differed in one
line, a `__finit_<hex>` symbol name in `test_class.cul` — which changes
between two runs of `--jit` as well. What was left were two properties
of the invocation rather than of the engine. `--vm-llvm` did not accept
`--jit-faststart`, and it named its module `"vm"`, so the object cache
never keyed on it; the cache is off unless `CULEBRA_JIT_CACHE` is set,
so no gate had been using it either way.

Which left the gates saying something untrue.
`check_alloca_discipline.sh` scanned one probe twice through one
emitter and printed `alloca-discipline OK (jit + vm lowering: ...)`.
`check_eh_balance.sh` reported a `jit` lane and a `vm` lane over the
same codegen. The symmetry sweep ran all 207 `tests/*.cul` through
`--jit` and then again through `--vm-llvm`. A gate that names two
engines where there is one is worse than a gate that names one: the
extra name is exactly the part a reader has no way to check. That is
the reason to retire the flag, and the minutes are a side effect.

They are real minutes. Restoring the fourth lane as what it was — a
second `--jit` pass — puts the sweep at 84 s against 42 s for three,
and `just test-dev` at 226 s against 186 s.

Two sites named the flag for a reason and were re-pointed rather than
cut. `tools/bench/vm_cases/compare.sh` has its own 177-case corpus, and
that lane was real coverage of the lowering; it is spelled `--jit` now
and costs the same. The isolate suite was the one place where deleting
would have deleted coverage: its `jit` lane ran only the 18
`*_jit.cul`, while the `--vm-llvm` lane ran all 20, so two files would
have been left compiled by nothing. They pass under `--jit` —
`Isolate.spawn`, `Channel` and `Parallel` have been symmetric since
Phase 2 — so the `jit` lane takes the whole directory, which is what
the `--vm-llvm` lane had been proving all along. In the eh gate the
`vm` lane's small probe was kept and re-pointed too: a try/catch
entered once per loop iteration is a shape the full probe does not
have.

`--vm` and `--vm-dump` stay. The executor is a second engine, and
neither flag has an equivalent under `--jit`.

## 13. Phase 4: retiring the tree-walker

### 13.1 Naming the engine

The default engine had no name. `--jit` and `--vm` each set a field,
and the tree-walker was whatever ran when neither did — `vm == Off &&
!jit`, a definition by exclusion that no caller could spell. That
costs nothing while the default stays put. Phase 4 moves it, and every
caller that never said which engine it wanted moves with it in
silence: a lane that had been measuring the tree-walker starts
measuring the VM and prints the same OK either way.

So the first batch adds the missing name and then makes its absence
loud. `--tree` selects the tree-walker explicitly. It is hidden from
`--help` — it exists for the callers inside this repo, and the
deletion batch removes it again — and it is parsed outside the
`CULEBRA_JIT_ENABLED` guard, so the same command line works against
the no-JIT build, which has exactly one engine to name.

Finding the callers was not a grep. `CULEBRA_REQUIRE_EXPLICIT_ENGINE=1`
turns an implicit pick into an abort, and the full gate then reports
them by failing. The check sits at each site that picks a default of
its own, because there are five and they are unrelated: a script run,
the REPL, `dap`, the doctest runner and the unit-test runner each
carry their own `Interp`. `fmt`, `lint`, `docs`, `--ast` and
`--version` run no user code, so they are never asked.

It aborts rather than exiting non-zero. Several lanes expect a
non-zero status — the leak-abort suite reads SIGABRT as its own
signal, the CLI tests assert on `rc` — and a tidy failure exit would
have been read as the thing those lanes were testing for.

That turned up 105 launches across 24 files: the justfile's interp
lanes, the difftest generator and its three runners, twelve of the
fourteen `tests/*.sh`, `dap_test.cc`'s `execlp` of the adapter, the CI
steps that invoke the binary without going through `just`, and one
place with no shell in it at all — `tests/isolate/test_proc_share_jit.cul`
starts child culebra processes through `Proc.run`, and a child
inherits the variable from its parent. The generator deserves its own
line: `tools/difftest/gen.cul` is the program that writes the corpus
every other lane is compared over (§2), so which engine runs it is not
a detail.

Two of them appeared only under the full gate. `just test-dev` was
green with `lint_test` and `dap_test` still launching bare, because
ctest runs in `just test` alone — §12.3 from the other side: what a
gate measures is not what its name suggests, and the fast lane is a
subset picked for speed.

One more appeared only in CI, and it is the same shape as the isolate
test: the Windows SharedBuffer smoke writes its program as a heredoc
inside `ci.yml` and starts two children with `Proc.spawn([exe, ...])`.
A culebra program spelled inside a workflow file is reachable by
neither the sweep over `.cul` files nor the sweep over shell commands,
and no local lane runs it. The ratchet found it the only way it could
be found — by running.

The variable is now set by default for every justfile recipe and in
both workflows, which is the point of the batch. A one-time sweep
would decay; with the ratchet in place a new recipe that launches the
binary bare aborts the first time it runs. Nothing outside `just`
changed: a bare `culebra prog.cul` still runs the tree-walker, and
will until the default flips.

### 13.2 Measuring what only the corpus reaches

Deleting the tree-walker costs the differential oracle: after it, the executor
and the LLVM lowering consume bytecode from the same compiler, so a bug in the
compiler — or in a runtime helper, or in the shared emitter — makes both lanes
give the same wrong answer and the corpus stays green (§7). The question this
batch answers is how much of that shared surface the hand-maintained suites
would still hold up if the corpus went away.

`-DCULEBRA_COVERAGE=ON` instruments the driver: `-O0 -fno-inline`, so every
`inline` helper in the shared headers keeps a body of its own to count, and
`-fprofile-update=atomic`, because the isolate suite runs counters from several
threads. The flags go on the `culebra` target alone — the measured surface is
header-only and lands in that translation unit, so vendor targets stay clean —
and the build opts out of `--gc-sections`, which is otherwise free to discard
the counter section of a function nothing calls, which is the population being
measured.

The surface is keyed on the demangled name, not the file. `vm.h` holds the
bytecode compiler, the executor and the lowering in one file, and only the
first is shared fate; `culebra::vm::Compiler::`, `culebra::JIT::` and the
`culebra_runtime_*` / `culebra::_jit_*` helpers are counted, `vm::Exec::` and
`vm::Lowering::` are not. That also spares the report a line-range map that
every edit to `vm.h` would invalidate.

Two profiles, not one: the durable suites and those plus the generated corpus.
The durable half is the gate's `ci-light` lane — the largest that carries no
generated corpus — plus the `tests/*_test.sh` CLI scripts, the doctest blocks
and a `--jit-faststart` pass, each on `--vm` and `--jit` and never on `--tree`,
which shares neither the compiler nor the runtime. gcda files are written
through `GCOV_PREFIX` rather than into the build tree, because libgcov's
read-modify-write is not concurrency-safe; a shim gives every process its own
prefix and `gcov-tool merge` folds them.

The durable half runs serially, and that is a finding rather than a default.
Run at `xargs -P 20`, it measured *less* of the runtime than the serial version
had — 605 live functions against 663 — because twenty instrumented `-O0`
processes make the load-sensitive suites (the Http server, net, isolate) start
losing runs. Per-process startup is indeed most of the wall clock, but buying
it back moves the number being measured, and a measurement that follows machine
load is not one. The corpus keeps its parallelism: those chunks are pure
computation, bind no ports and start no children. Thirty-two minutes, most of
it the `--jit` lane at `-O0`.

| | functions | | | lines | | |
|---|---|---|---|---|---|---|
| | total | live | corpus-only | total | live | corpus-only |
| compiler | 265 | 238 | **0** | 3169 | 3034 | **0** |
| emitter | 186 | 163 | **0** | 2152 | 1996 | **0** |
| runtime | 923 | 830 | 2 | 6017 | 5406 | 24 |

**The compiler and the shared emitter have nothing the corpus reaches alone.**
A function-level zero on its own would prove little — a function both sets
execute can still hold a branch only the corpus takes — which is why the line
column is there, and it agrees. Everything the corpus adds is in the runtime:
24 lines across 12 functions.

Those numbers took three corrections to reach — the parallelism above, the two
retired prefixes below, and one more. The script had been launched directly
rather than through `just`, so it ran without `CULEBRA_CANVAS_HEADLESS`;
`tests/test_canvas_module.cul` fails an assertion a third of the way in without
it, and every Canvas call past that line went unexecuted, presenting nine
Canvas functions as holes that were never holes.

All three pushed the same way, and that is the part worth keeping. A durable
half that loses coverage can only *inflate* the corpus-only set, never deflate
it, so the two zeros were never at risk; only the work list was. `run.sh` now
pins that environment rather than inheriting it, and — the general form of the
same bug — counts the durable invocations that exit non-zero and refuses to
report a corpus-only set while any did, since a program that died early has
executed less than it should and every line it missed lands in the corpus
column.

What is left is mostly error edges — the `TAG_FUNC` and `TAG_SET` arms of
`type_error_typed`, the arithmetic guards for `/` and unary `-`, `array_set`'s
`IndexError`, `Tensor.item`'s arity message, the release-on-throw line in
`build_variant`, the stale-handle refusal in `canvas_target`. Two are not:
`Tensor.no_grad` had never been called by anything hand-written, and shared-view
indexing in `sendable_jit.h` — the `KeyError` edge, the scan over non-String
keys and the `Long`/`Bool` key comparison — was reached only by generated
input.

Two things the report prints about itself, because a headline of zeros is
exactly the shape a broken measurement takes. A `SHARED` prefix that matches
no function at all is an error rather than an empty row — the first run of the
check retired two prefixes that had stopped existing, one of them removed in
Phase 3. And the surface is an allowlist, so the report ends with a tally of
this repo's own functions that no prefix claimed and no exclusion named. That
tally is what moved `culebra::gc` and `culebra::_canvas_detail` into the table
above: reached through the runtime helpers rather than named by either engine,
but a bug in them is a bug both engines inherit. Around 5100 functions remain
outside it — `_make_iterator`, `wrap_detail`, `http`, `EffectsLowerer` and the
tooling — so the boundary is a live question rather than a settled one, and
printing it every run is what keeps it from going quiet.

One caveat this measurement cannot answer: coverage is reachability, not
verification. A line both sets execute is a line the durable suite *runs*, not
one whose behaviour it pins — an assertion has to exist for that. What the
zero establishes is narrower and still worth having: after the corpus is
re-pointed at executor-vs-lowering, the hand-written suites are not walking
past whole regions of the compiler unexecuted.

### 13.3 Writing the tests the corpus was standing in for

Twenty-four lines in twelve functions, and the tree-walker's last job was to
say what each of them should do. That is the part of the oracle that expires:
once there is one engine, a new test's expected value is whatever that engine
prints, and "the test agrees with the implementation" is not evidence. While
the tree-walker is still here, a case can be written against it and confirmed
byte-identical on `--vm` and `--jit` before being frozen — which is what
happened to every case below.

They went into the suites that already owned the behaviour rather than into a
file of their own. `tests/test_shared.cul` gained the read edges of a frozen
view: a missing String key, an Object frozen with `Long` and `Bool` keys, an
Array view indexed by a String (`expected Long, got String`) and one indexed
past its end. `tests/test_runtime_errors.cul` gained the two tag names an
index error had never had to render — `got Function` and `got Set` — plus the
guards on `/` and unary `-` and the `IndexError` from writing past an array.
`tests/test_tensor.cul` gained `Tensor.no_grad`, which nothing hand-written had
ever called, and `.item()` on a two-element tensor. The rest are one case each:
removing a non-String key from an object holding none
(`tests/test_object_keys.cul`), excess positional arguments to a variant
constructor (`tests/test_enum.cul`), `join` over an iterator of Strings
(`tests/test_iter_terminal.cul`), `draw_to` into a sprite whose pixels are
already freed (`tests/test_canvas_module.cul`), and `Math.max()` with nothing
to reduce (`tests/test_math_kernels.cul`) — that last one reaching a lambda
that exists only to supply a source position to a throw.

Re-measured, the corpus-only set is empty: zero functions and zero lines, in
all three columns. `tools/coverage/corpus_only_coverage.txt` records that as a
ratchet with the same two-sided contract the leak-abort allowlist has — a
corpus-only function not listed there fails the report, and a listed one the
suites have since reached is reported so the file can shrink. Both directions
were exercised before the file was committed: a name that no longer qualifies
prints "shrink", and a profile with an empty durable half produces 1231 new
entries and exit 1. A ratchet nobody has seen fail is a comment.

It keys on the function in both columns, not just the first. A new branch that
only the corpus takes, inside a function both sets already enter, adds no
corpus-only *function* but does add corpus-only *lines* — and the owner of
those lines has to be listed too, or the report fails. Keying the line side on
`(file, line)` would have been the obvious alternative and is wrong for the
reason §13.2 gives for the function side: line numbers move under every edit,
and a ratchet that churns is a ratchet people delete.

What it does not have is a cadence. `just coverage` is a 35-minute
instrumented measurement, correctly outside `just test` and outside per-PR CI,
which leaves the file's state discoverable only by someone who volunteers the
time. Rather than leave that implicit, the moment is named in the file: re-run
before B5 re-points the corpus at executor-vs-lowering, because after that the
measurement is answering a different question and this baseline stops being
comparable. A weekly scheduled job is the alternative and would cost an
instrumented LLVM build plus a serial sweep on a 4-core runner; that is a real
bill, and it is not paid here.

The list being empty is not the same as the suites pinning the behaviour of
everything they run — §13.2's caveat still holds, and coverage cannot tell an
assertion from a bare call. What it does settle is narrower: when the corpus is
re-pointed at executor-vs-lowering, no region of the shared surface is left
with generated input as its only reader.

### 13.4 Giving the no-LLVM build an engine

Deleting the tree-walker takes an engine away from every build. Most builds
have a second one; three do not. The CMake option that enables the JIT
defaults to `OFF`, so a plain `cmake ..` produces one of them; the
`build-no-jit` CI lane is another; the WASM Playground is the third, built by
emcc against no LLVM at all. Under §13's plan that deletion lands in B7, and
if it landed with the guard as it stood, those three would have gone from one
engine to none.

The obstacle was never the VM. Measured before any of this: the first
`llvm::` in `vm.h` was 11,000 lines in, inside `Lowering`; the compiler and
the executor above it named LLVM nowhere. Seven of the eight runtime
fragments did not either. What stood in the way was the guard. Every one of
those fragments opened with `#ifdef CULEBRA_JIT_ENABLED`, so a build without
LLVM had no `JitValue`, no runtime helpers, no `builtin_signatures.h` — not
"an engine it could not reach" but the value model that engine is written in.

So the batch is a split, not a port. `rt.h` is new and holds what both
consumers stand on: the include prelude those fragments were relying on
`jit.h` to have opened, the eight fragments themselves, and the small
front-end contract below. `jit.h` keeps the LLVM includes and `struct JIT`.
`vm.h` keeps the compiler and the executor; `vm_lowering.h` is new and holds
the lowering, `run_modules_via_llvm`, `build_object_from_modules` and the
three `JIT::` embedding entries defined over them. `CULEBRA_JIT_ENABLED` now
reads as "LLVM is linked", and what it guards is `jit.h`, `vm_lowering.h`, one
member of `stdlib_jit.h`, and the AOT bootstrap — which needs no LLVM itself
but only exists where `culebra build` can emit an object to link it into.

Four things had to leave `struct JIT`, because the bytecode compiler read
them through it and would otherwise have needed a class it cannot have:
`ExtensionHooks` with `install_extension` / `current_hooks`, the
`is_builtin_var` predicate free-variable analysis consults, the
`fn_introspection_name` predicate, and the `ForKind` cursor tags both engines
switch on. None of them ever needed a JIT — they are the compile-time
contract between the front end and whatever the stdlib installed, parked in
the class that happened to be the only compiler at the time. They are
namespace-scope in `rt.h` now. `install_extension` is the one with a
published name (`docs/deployment.md`), and it moved with it.

Two places in the "LLVM-free" runtime did need LLVM, and both are about
handing something to LLVM rather than about the runtime. `jit_mem.h` ended
with the Win64 RTDyld memory manager that calls `RtlAddFunctionTable` on each
object RTDyld loads, so a throw can unwind through a frame the JIT just wrote.
Guarding it in place looked sufficient and is not: those two classes *derive*
from `llvm::SectionMemoryManager`, and `rt.h` is read before `jit.h` opens the
LLVM headers, so on Windows with the JIT on they would be parsed against an
undeclared base. Nothing on Linux can show that — the only lane that would
have is `windows-jit-build`. They moved to `jit.h`, beside the ORC layer that
is their one caller, and the mingw EH declarations they share with it went
along; the guard disappeared with the move, which is the tell that in-place
was the wrong depth. The other case is `JitExtension::declare_runtime`, which
declares the runtime's signatures on the module being built — one member of a
9,000-line header, holding its only `llvm::` line. That one is guarded where
it sits: the hook is documented as nullable, and an omitted designated
initializer is the null a no-LLVM build wants.

One seam is left twisted and is worth naming rather than hiding. `vm.h`
includes `stdlib_jit.h` for the stdlib the executor resolves against, and
`stdlib_jit.h` includes `jit.h` for that one member's definition — so in a
build that *has* the JIT, `vm.h` still reaches LLVM transitively. No
translation unit pays for it (a JIT build's `culebra.h` includes `jit.h`
directly anyway), and the claim that `vm.h` needs no LLVM is settled by a
configuration that compiles rather than by the include graph. Straightening it
means splitting `stdlib_jit.h` the way `jit.h` was split here, which is B7's
neighbourhood — `stdlib_jit.h`'s other half is the native module binding that
batch has to move anyway.

Three things fell out. `JIT::known_builtin_methods()` was a forwarder to
`culebra::builtin_method_names()` in `shared.h`, and the startup drift check
that was its only caller now reads the source directly — which also means the
check runs in every configuration instead of only where LLVM is. `repl.h`
included `jit.h` and used nothing from it. And `vm::Exec` and `vm::Compiler`
were declared friends of `JIT` to reach exactly the members that just left, so
those two friendships went with them; `vm::Lowering`, which really does use
the JIT's emitters, is the only one left.

The move needed no IR diff, because it is provably not a change. Everything
from `Lowering` to the end of the file went to `vm_lowering.h` byte for byte;
the only textual edit anywhere in the moved region is dropping a `JIT::`
qualifier from five names that now resolve in the enclosing namespace to the
same values. A script reconstructs the new file from `HEAD`'s and diffs, which
says more than re-emitting IR would, and costs seconds. The full gate ran the
lowering over the corpus anyway.

The gate around this configuration changed shape, for §13.1's reason.
`build-no-jit` proved a link, and a link cannot notice an engine going
missing: the whole failure this batch exists to prevent — a binary with one
engine where there should be two — compiles and links perfectly. `just
test-no-jit` runs the thing: the `vm_cases` corpus on both `--vm` and
`--tree` through the same `compare.sh` the gate's own VM phase uses, plus the
REPL, plus `--version` — which now names what the build has (`interp+vm`, or
`interp+vm+jit`) rather than implying that a build without LLVM has only the
interpreter. The CI job runs that instead of the build alone. Reusing
`compare.sh` rather than writing the comparison again is not only economy: it
already folds each lane's **exit code** into the comparison, and a hand-rolled
`diff <(a) <(b)` discards both — §12.3's lesson, which this repo has paid for
once, is that reading only stdout lets a SEGV pass as "equal".

One of the three is only half done, and saying so is the point of naming it
here. The `build-no-jit` lane and a default `cmake ..` now have the executor
and run on it under `--vm`; the Playground does not, because
`playground/wasm_main.cc` includes `interpreter.h` and calls
`interpret_modules` directly and has never had a second engine to pick
between. What this batch changes there is that it now *could*: `vm.h` no
longer requires LLVM, which is the only reason it could not before. Making it
so is a default-engine switch, which is B6's, and it is on B6's list rather
than assumed — a build nothing in this repo exercises by hand is exactly the
one that would be discovered missing an engine after B7.

### 13.5 The oracle a release leaves behind

The tree-walker's last remaining job is to be a second opinion. §13.2 measured
how much of the shared surface the hand-written suites reach and §13.3 wrote
the tests for what they did not, but neither closes the hole §7 names: after
the deletion, the executor and the LLVM lowering are handed bytecode by one
compiler, and a compiler bug makes both lanes give the same wrong answer.
Coverage says the code was executed, not that anything checked what it did.

There is one second implementation that does not have to be maintained,
because it is already built and cannot change: the previous release's binary.
It answers for everything the language could express on the day it shipped, it
is downloadable for three platforms, and no edit to this working tree moves
it. Deleting the tree-walker does not destroy the differential oracle so much
as seal it — for the frozen subset, an independent implementation keeps
answering, release after release.

`tools/difftest/release_diff.sh` is that comparison. It builds the same
generated corpus run.sh uses, runs it under a baseline binary and under this
build, and reports every case whose behaviour changed.

**Both sides run on their default engine, with no flag.** That is the one
place in this repo that leaves the choice implicit, and it is deliberate
enough that the ratchet is unset for those runs: the question here is what
changed for someone who types `culebra prog.cul`, so the default is the
subject rather than an oversight. The baseline predates `--tree` and could not
be told otherwise anyway. It also means that when B6 flips the default, this
gate compares the tree-walker against the VM without being edited — the check
that switch wants most, arriving free.

Not a byte diff, and for a reason run.sh no longer has. The older binary
answers `err=` where this one answers `ok=` for every built-in a release
added, and an `ok=` case may print lines the failing one never reaches, so the
two files stop lining up as text long before they stop lining up as cases.
`_p` emits exactly one record per case under any binary, so the records are
walked positionally and each carries the output printed after it.

A case can also be written in syntax the baseline predates, and that one does
not fail alone — the chunk it sits in fails to parse and every record after it
disappears. The completion guard catches the short chunk and reruns it one
process per case, in parallel; a case that still produces no record is emitted
as `<label> ::: unsupported`. The two sides stay aligned record for record,
and the report can say exactly what the older binary could not express. The
same marker on the head side is a failure rather than a change: that binary
generated the corpus, so a case it cannot run is a case it should not have
written.

Both sides take that path together even though only the baseline ever needs
it, because an error record carries the line and column the throw came from
and those are positions in the chunk program. Rerunning one side per case
renumbers every error record on that side, which would invent a difference in
every case that threw — the whole population the gate exists to read. The
path is otherwise unreachable on demand (it needs a release that predates a
syntax now in the corpus), so `RELEASE_DIFF_FORCE_FALLBACK=1` takes it for
every chunk: run that way against v0.2.0, all 88 chunk-sides rerun per case
and the report is identical to the chunked one, 261 and 261.

**Every difference has to be named in `release_diff_allow.txt`**, which is
what makes that file the draft of the release notes. The contract runs both
ways, like the leak-abort allowlist: an unlisted change fails the gate, and a
listed pattern matching nothing is reported so the file can shrink once the
release that needed it ships. The first run against v0.2.0 found 261 of 17,262
cases changed, and writing them up is what the file now holds:

| what changed | cases |
|---|---|
| String answers fifteen more method names — thirteen new, plus `index_of` and `reverse`, whose receiver mask had admitted only Array | 150 |
| reading one of those fifteen without calling it: nil in 0.2.0, "cannot be used as a value" now | 15 |
| `repeat(n, value)` is a new global, and UFCS puts a global in method position for every receiver the corpus sweeps | 90 |
| a native's declared signature is now canonical on every engine, so a bad call names the parameter instead of counting arguments in a hand-written arm | 6 |

Patterns are globs over the case label, where `*` and `?` are the only
wildcards and everything else — brackets included — is literal. `fnmatch`
was the obvious choice and is wrong: a label is a fragment of culebra source,
so `kw|[1, 2, 3].sorted(bad: 1)` reads as a character class, and an entry
written to name one case would quietly match a hundred.

The comparator has its own smoke test, which is not ceremony. This gate runs
on master pushes only and prints OK when nothing changed, so a comparator that
stopped comparing looks exactly like a quiet week. `release_diff_selftest.sh`
gives it ten synthetic cases — each one a way the comparator has to be able to
*fail* — and `release_diff.sh` runs it before it runs anything else, so a green
report is a comparison that was checked rather than one that merely printed OK.

CI runs it on every master push, against whatever binary is published: no tag,
so a release moves the baseline forward by itself. Not on pull requests —
it compares against a *release*, which says nothing about a branch that has
not landed, and what B6's switch needs watched is the landed commits. It is
also the one lane that cannot run on this machine: release binaries are built
on ubuntu-latest and want a glibc newer than this box has, which is why the
script takes the baseline as an argument rather than fetching it. The
measurement above came from building the v0.2.0 tag locally instead.

Finally, the corpus itself changed sides. run.sh's primary comparison is now
`--vm` against `--jit` — executor against lowering, the two consumers that
outlive the tree-walker — with `--tree == --vm` as the third lane and the
answer for the compiler between them, for as long as there is a tree-walker to
ask. All three are compared byte for byte now: the VM lane used to be walked
record by record against a ceiling on its skips, because a case outside the
supported slice answered VmError, and that slice closed in Phase 2 with the
ceiling at 0. A VmError in the corpus is an ordinary divergence today, and
`vm_skip_ceiling.txt` is gone.

Re-running `just coverage` before all this — the moment §13.3 named — turned
up a broken instrument rather than a new hole. The corpus-only set is still
empty in all six columns, but 176 of the 1,039 durable invocations exited
non-zero, and the guard B2 added for exactly that ("a durable run that died
early inflates the corpus-only set by what it did not reach") was firing on
every run and had stopped meaning anything. All 176 are curated error cases —
84 of `vm_cases`, 4 of the leak-abort probes, on two lanes each — which end in
an uncaught throw and exit 255 by design, and whose exit codes are checked
where they belong, by `compare.sh` holding the three lanes to the same one.
Those two sweeps now accept 255 and nothing else, so a signal still counts. A
detector that fires on every run is not a detector, and this one had been
reporting a real number about the wrong population since it was written.

### 13.6 The Playground picks an engine

§13.4 left the Playground as the build it had made ready and not moved:
`playground/wasm_main.cc` still included `interpreter.h` and called
`interpret_modules`, because until `vm.h` stopped requiring LLVM there was no
second engine there to pick between. The switch itself is the same three calls
`main.cc`'s `--vm` path makes, in the same order — `install_jit_stdlib()` for
the stdlib the executor resolves names through, `splice_stdlib_preamble()` for
the module that declares the lazy builders, then `Compiler::compile_modules`
and `Exec::run`.

It did not compile, and what it printed is the reason this section exists.
§13.4's claim was that a build without LLVM has an engine, and `just
test-no-jit` runs one to prove it — but that lane is a native Linux build.
wasm is a *different* build without LLVM, and the three things that broke are
about the platform rather than the engine. All three are headers that batch
made reachable, handed to a toolchain that had never read them:

- `jit_gc.h` included `<execinfo.h>` on every platform that is not Windows,
  for the birth-site backtrace GAP5 prints. Emscripten's sysroot has pthreads
  and not that. It degrades the way mingw already did — a `backtrace` that
  returns 0, and an audit that still fires and says "(no birth site)".
- `Heap::stack_base` ended in `#error "unsupported platform"`. wasm keeps its
  value stack outside linear memory, where nothing can scan it; what
  `emscripten_stack_get_base()` returns is the base of the user stack
  emscripten reserves *inside* linear memory, and that is where an
  address-taken local lives — the only locals a conservative scan could ever
  have found.
- `static_assert(sizeof(JitParamMeta) == 12 * sizeof(int64_t))` fails on
  wasm32. It weighs a struct of pointers against a count of `int64_t`, so it
  is a statement about a 64-bit target as much as about the field list, and
  what it exists to catch is drift from the LLVM struct
  `emit_param_meta_global` emits — which is only there where LLVM is. It sits
  under `CULEBRA_JIT_ENABLED` now.

The fourth was the switch's own doing and the switch resolved it. §13.4 signed
off owing a warning: `_jit_shared_val_prop` and `_jit_shared_val_index` are
declared in `jit_runtime.h` and defined in `sendable_jit.h`, which the no-LLVM
include chain does not reach. The executor's lane includes `stdlib_jit.h`,
which does.

So B4's entry needs correcting rather than annotating. "The Playground can now
be switched" was true of `vm.h` and false of the Playground, which that same
batch had stopped compiling, and nothing noticed because nothing built it. The
lesson is narrower than §13.4's own ("a link is not a claim") and sharper:
**`build-no-jit` and the Playground are two configurations, and the ratchet
compiles one of them.** No CI runner has emsdk. §13.4 closed by saying that a
build nothing exercises by hand is the one that would be found missing an
engine after B7; it was already missing a compile, one batch earlier than
predicted.

One more thing fell out of the first rebuild in a while, and it is not about
any of this: `emcc` links this translation unit as C, and 6.0.8 leaves
`operator new` and the libc++ internals undefined where older toolchains
inferred C++ from the input's extension. The driver is `em++`.

Verification is the difftest lanes' in miniature — a script that reaches the
lazy stdlib, a class with a getter, `fib(22)`, the String methods master added,
and twenty thousand closures whose only job is to make the collector run, which
is what exercises the second item above. Its output is byte-identical between
the wasm build and a native `--vm`.

Run it twice, though, and the second one traps. That is the finding this batch
would have shipped, and it is worth the space because nothing about it is
specific to wasm. The page holds one instance for every Run click, so a
`Runtime` outlives a run — and the namespace caches live there, while the
closures a cached namespace holds point into the `VmProgram` that built it,
which the call that built it owns and destroys. Any program naming a lazy
module (`Time`, `Canvas`, `Regex`, the matchers, …) reads a dangling descriptor
the next time. The tree-walker was not exposed to it: it built a fresh
`Environment` per run and its lazy modules were bound there.

The fix was already written down elsewhere in the tree. `doc_block_runner`
gives every doctest block its own `Runtime`, and its comment says why in the
same words — the namespace caches and the class and overload registries live
there. `run_culebra` does that now, with `install_jit_stdlib()` deliberately
left outside it so the hooks land in the process-wide default rather than in
one run's copy. What made the two engines differ here is that per-run
teardown used to be free: the interpreter's state hung off an `Environment`
that the run released, and moving the engine moved that state into a Runtime
that nobody was ending.

The way it was found is the other half. A single run passed — twice, in two
sessions — because a single run is what a smoke test naturally does. It took
running the same program a second time in one instance, which is the thing the
page does and the harness did not.

So the batch ends by making that a lane. `just check-playground`
(`tools/playground/smoke.mjs`) loads the committed wasm under node, runs four
cases twice each in one instance, and holds the output to the same program on
the native executor. It needs no emsdk, which is the whole point: what it
checks is the artifact under `site/playground/` that Pages serves, not one it
builds — so it runs in CI, on every push, beside `release-diff` and on the same
downloaded binary. Both directions were exercised before it landed: with the
per-run `Runtime` commented back out it reports `lazy_stdlib.cul run 2: wasm
trapped: memory access out of bounds` and exits 1, and the other three cases
still pass twice, which is the shape of the bug rather than a blanket failure.
The `full` variant is out of its reach — a JSPI build cannot be instantiated
without `WebAssembly.Suspending` — but the two builds are the same translation
unit, so the engine question is answered by either.

### 13.7 The unit-test runner

Four of the five sites §13.1 found already had a VM lane: a script run, the
REPL, `dap` and the doctest runner each gained one in Phase 2. The fifth had
none. `culebra test`'s unit runner held `Value`s in a C++ registry, called them
through the interpreter's `call` helper, and resolved fixtures out of an
`Environment` — and it said so, rejecting `--vm` outright. Switching the
default without moving it would have left the one subcommand that runs a user's
own suite on the engine being retired.

The engine seam is `debug_engine.h`'s again — an interface with one
implementation per engine, everything above it shared. What made it small is
what went *below* it. `test` and `parametrize` were 90 lines of C++ building
`FunctionValue`s; they are now `src/preambles/test_ambient.cul`, one culebra
source both engines run, and the registry they fill is an ordinary culebra
Array. That moves the parts the runner would otherwise have had to ask an
engine about into the program itself: which parameters a test takes fixtures
for is `f.params.filter(...)` there, and a `@parametrize` case is spread into
an argument list there. What is left for the host is running a program, reading
a global, walking an Array or an Object, and calling a function — nine methods,
none of which lets the two value models meet. Values cross as an index into the
host's own store, released to a mark when the test that used them ends, which
is how a fixture's `drop` still fires into the right test's captured output.

The VM host compiles each file the way the REPL compiles a line: as a session
unit, so its top-level bindings land in `vm::ReplSession`'s cells rather than
in the frame that ran it. That is the whole reason it can work at all — the
runner calls back into a file *after* it has returned, so a registered closure
has to still be callable and a fixture has to still be a name to look up. It is
reuse rather than a workaround: the capability already had a name, and the
debugger was already its second consumer. What it did *not* have was a home —
the REPL owned it — so the retained programs, the token set that keeps a lazy
namespace's builder from being registered twice, and the unit runner itself are
now `vm_session.h`, held by a `vm::Session` the REPL and the test host each
have one of. Owning one rather than borrowing the process-wide session is what
keeps a second run in one process from seeing the first one's registry, which
is the guarantee the deleted C++ registry used to give by clearing itself.

Three bugs surfaced, and the first two are the same bug in two engines'
clothing: *what keeps a value alive*. The interpreter's ambient was parsed from
`TEST_AMBIENT_MODULE_SOURCE`, a `const char*` — which materializes a temporary
`std::string` that dies at the end of the call, leaving every AST token a view
into freed memory and `test` reporting `undefined variable '` with a name made
of whatever was there. The VM host's store retained the result of a call that
already handed it a reference, so a fixture's refcount never reached zero and
`drop` never ran. Both are the shape §13.6 met on the Playground: moving an
engine moves what owns the values, and the previous owner's guarantees do not
come with it.

The third was in the compiler, and it is worth stating on its own because
nothing in this batch caused it.

**A session name reached from two branches of one function crashed the VM.**
The slot holding a session binding's cell is filled by a `ReplCell`
instruction, and the compiler emits instructions in source order — so that
`ReplCell` lands wherever the name was *first mentioned*, which may be inside
an `if` arm. A later mention reuses the binding (correctly: the two mean the
same name) and therefore the same slot, without re-emitting anything. Call the
function so that the first arm is skipped and the second one's `CellGet` reads
a slot nothing filled:

```culebra
let helper = fn () { 7 }
let pick = fn (n) {
  if n == 2 { return helper() }
  if n == 1 { return helper() + 1 }   # segfault: `helper`'s slot is empty
  0
}
```

That is `culebra --vm`'s REPL, on master, with no part of this batch involved
— and it was about to become far easier to reach, because under the test host
*every* top-level binding of a test file is a session binding. The fix
re-emits the `ReplCell` at each use rather than trying to hoist it: the binding
is scope-wide by design and it is the instruction that is not, and re-emitting
is idempotent because the session hands back the one cell it owns. The
regression case lives in `tests/repl_test.sh`, where it segfaults the
pre-fix binary and passes the fixed one.

`culebra test` runs on `--vm` now; the default is still `--tree`, and the gate
holds the two to byte-identical output over both self-suites and both
reporters. `--jit` stays refused, for the reason the REPL and the debugger
refuse it: a test body is not a hot loop, and compiling every one of them buys
latency and nothing else.

### 13.8 Moving the default

Every site now has a lane on the bytecode VM, so the switch itself is one
line. `parse_command_line` records whether the command line named an engine and
then, if it did not, sets the executor — after which every reader downstream
sees an `Options` that names one, and no site spells the default a second time.
`culebra test`, `culebra dap` and the REPL each carry their own parser and each
gained the same two lines. What did need care is ordering: the ratchet's
`require_explicit_engine` has to be asked *before* the engine branch, or a
defaulted run would take the VM lane and return before the check it exists for.

The latency question §13 raised has an answer that depends entirely on what is
being measured, and both halves are worth writing down. On a script that names
several lazy stdlib modules and then does nothing, the VM is **7 ms slower**
(35 → 42 ms). The startup profile says where it goes, and it is not where the
Phase 0 spike guessed: the preamble *parse* costs about the same on both lanes
— the tree-walker pays it too, spread through `interpret_modules` as each
module is first used — and the executor's extra is the bytecode compilation of
that preamble. On a script that names none, the two are within 2%.

On real programs the sign flips. `tests/test_object_keys.cul` is **42% faster**
on the executor, `test_core.cul` slightly faster, the rest even. That is Phase
3's 2–4.5× loop speedup paying back a fixed 7 ms almost immediately, and it is
why the preamble-bytecode blob (`grammar_blob.h`'s shape, named in §13 as the
mitigation) is *not* being built: it would buy back a cost that only a script
doing no work can feel.

One thing the switch cost is visibility. Every gate lane names its engine —
that is what B1's ratchet enforces — so after the default moved, not one of
them would have noticed if it had not. `tests/cli_input_test.sh` is where that
is checked now, and it is the only place in the tree that must run the binary
*without* `CULEBRA_REQUIRE_EXPLICIT_ENGINE`, dropping it the way
`release_diff.sh` does. The observation is the startup profile: the
`interpret_modules` mark exists on the tree-walker's path alone, so its absence
says the executor ran. The exit code is asserted alongside it, because an abort
prints no mark either and would otherwise pass.

### 13.9 The collector meets wasm locals

Two days after v0.3.0 shipped, Rocci Bird died in the Playground with
`RuntimeError: memory access out of bounds` around the ten-thousandth frame.
The crash reduces to eight lines with no Canvas in them — a per-frame
`map(spread).filter().size()` pipeline — and to a one-line cause:
`CULEBRA_GC_NEVER=1` completes the same run. §13.6's smoke was standing right
next to this and could not see it: twenty thousand closures die by refcount
before the collector matters, while what breaks is the collector's *sweep*, on
a value the mark phase never found.

The mark phase never found it because on wasm the conservative scan's central
assumption is false. `scan_roots` flushes callee-saved registers with a
`setjmp` and walks the machine stack; wasm locals — the register file the
values actually live in — sit outside linear memory, and `setjmp` spills
nothing there. §13.4 already wrote down the halfway version of this when
`Heap::stack_base` grew its `__EMSCRIPTEN__` arm: the linear-memory stack
holds "an address-taken local … the only locals a conservative scan could
ever have found". The frame windows of `run_frame` are VLAs, so *they* are
visible; what is not is every value whose only reference sits in a wasm local
— the string `num_add` just built and has not yet stored, or the half-built
array inside `iter_collect` while the mapped closure runs. A threshold
collect fires at an allocation site inside such a helper, the sweep frees the
object, and the freed block corrupts the slab free-list — which is why the
trap surfaces later, inside `SlabAllocator::alloc`, wearing whatever face the
reuse gives it. The tree-walker never met this: its strings are refcounted
(`GC.stat` shows `live == rc` there), so the collector decides no string's
lifetime, and the engine switch is what carried the traced-only
representation into a build whose scanner cannot back it.

The fix defines where collection is legal instead of patching where it was
found illegal. Under `kDeferToSafepoint` (jit_gc.h, on for `__EMSCRIPTEN__`
only) no collect runs inline, ever: a threshold trip sets a pending flag, and
the executor polls `safepoint_collect()` at its instruction boundaries, where
every live value of every frame sits in a register window the scan does see.
One hazard survives the move — a helper suspended *between* two VM frames
(the iterator op whose callback is running) may hold the only reference in
its own locals — and the invariant covering it is: **no collection while any
such frame is on the stack.** Enforcement is one choke point, not a
per-helper audit: every helper-to-user call passes through `_jit_invoke`
(jit_value.h), whose `SafepointUnsafeScope` defers the poll for the call's
duration. The dispatch arms audited to keep callee, receiver and arguments in
registers for the whole call — `Call`, `CallM`, and the builtin gate's
user-method hand-off — use `_jit_invoke_rooted` and stay collectable, which
is what keeps a `Canvas.run` game loop collecting every tick. The marking is
fail-safe by construction: a new call path that never gets audited only
defers collection until the next eligible poll — memory grows, nothing is
freed wrongly — and `CULEBRA_GC_STRESS`, whose threshold of 1 now means
"collect at every eligible poll", hammers exactly this protocol.
`tools/playground/cases/pipeline_churn.cul` runs the reducing pipeline
through `check-playground`, twice per instance like everything there, so the
lane that missed this class of bug once now trips on it in CI. On native
builds the whole protocol folds away to the previous behavior; explicit
collects (`GC.stat`) gate the same way on wasm, so under a helper they defer
rather than sweep.

### 13.10 The deletion

Seven batches, ordered by one principle borrowed from §3: put the
verifications that need both engines alive before the deletions that end
that condition. Everything up to the embedding switch was reversible;
`--tree` was still answering right until the batch that removed it.

The plan's inventory missed three loads the tree-walker was still
carrying, and finding them — not deleting files — was most of the work.
First, **the signatures were the interpreter's.** Every compiled lane
answered "what are `Math.pow`'s parameters" by building the interp's
entire stdlib environment at runtime and reading the `FunctionValue` —
1,601 of the AOT archive's 7,322 `culebra::` symbols were that
machinery. The replacement is a generated table (`canon_sigs.gen.h`),
emitted by a tool that ran while both existed and asserted 1:1 agreement
with the environment it replaced; the generator retired with the engine,
and the table is now the canon a signature edit maintains by hand.
Second, **the compiler's include graph ran through the engine it was
outliving**: `vm.h` reached `interpreter.h` transitively, so B7-b split
the carrier headers (the kernels, the preamble machinery, the isolate
core, the send-tree) until a TU holding `vm.h` alone compiled with zero
interp headers, and a ratchet held the cut. Third, **the oracles**: the
vm_cases baseline, the leak-fuzz reference lane and the corpus generator
were all pinned to `--tree`. Each was re-pointed while the tree-walker
could still countersign the goldens it was handing over (§13.5's
release-diff is the independent second opinion that outlives it).

The embedding API moved last among the reversible batches: `vm::Embed`
(deployment.md §2) replaces `environment()` / `interpret` / `call` /
`define`, with the smokes keeping their contracts across the switch.
Then the two irreversible batches: B7-e deleted the five engine sites
and the flag — after which `--tree` is an unknown option and
`--version` says `vm+jit` — and B7-f deleted the body: `interpreter.h`,
what remained of `stdlib_interp.h`, the interp REPL and debugger, the
interp halves of `wrap.h` and the isolate/sendable layer, and the
drift checks whose other side no longer existed.

Two details from the last batch are worth keeping. The undefined-variable
lint's builtin-name set had been read out of the interp environment; it
is now materialized from the same predicates the compilers resolve names
with (`builtin_var_names` + the ns tables + the lazy groups), so the
lint and the engine cannot disagree about what a bare name means. And
the first compile after the deletion failed inside LLVM's headers:
`termios.h` defines `CR1` as a macro, `term.h` had always leaked it, and
the include order that used to shield LLVM — linenoise's `#undef` block
arriving first via the interp REPL — left with the REPL that carried it.
The `#undef` now lives where the include is.

The bill, measured on the same machine before and after B7-f: the AOT
runtime archive halved (20.2 → 10.3 MB), the driver binary lost 6 MB
(84.8 → 78.7 MB), and the archive's defined `culebra::` symbols went
from 6,433 to 1,472 — with the interp-environment machinery's share at
exactly zero, which is what the inventory's 1,601-symbol figure had
predicted the deletion was worth.

What the deletion did not change is the point of the whole phase:
`tools/difftest/release_diff.sh` compares this tree against the last
released binary — v0.3.1, the final release with both engines — and the
deletion batches landed with its allowlist empty.
