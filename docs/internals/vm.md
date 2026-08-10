A Shared Bytecode VM: Design Proposal
=====================================

**Status: proposal; Phase 0 spike passed.** Sections 1–9 record the
motivation, the target architecture, and the migration plan for
replacing the tree-walking interpreter with a bytecode VM that shares
its value representation — and its front end — with the JIT. The
Phase 0 spike (§7) has since been run and answered both exit
questions yes — results in [§10](#10-phase-0-spike-results);
everything beyond the spike remains unimplemented. The observable language contract in
[`language.md`](../language.md) is unaffected: this is a change of
engine, not of language. Where this document and `language.md`
disagree, `language.md` wins.

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

- **DAP/debugger semantics** (`dap.h`, 873 lines, deeply coupled to
  `Value`/`Environment`). Step semantics and scope enumeration must
  be reimplemented over debug tables. Standard machinery, real
  work; part of Phase 2's parity bar.
- **Lazy stdlib resolution.** `Environment::initialize_lazy` resolves
  stdlib modules on first *name lookup* — a mechanism that assumes
  name-based access. Slot resolution needs a different trigger
  (likely: resolve at bytecode-compile time when the compiler sees
  the name).
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
