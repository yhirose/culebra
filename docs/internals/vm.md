The Bytecode VM: Architecture
=============================

This document describes how Culebra's execution engine is built. It is
not a specification: the observable language contract is normative in
[`language.md`](../language.md), and where the two disagree,
`language.md` wins. The memory-management half of the runtime — reference
counting, the ownership discipline of the LLVM lowering, and the tracing
backstop — has its own document, [`memory.md`](memory.md).

The Japanese mirror of this file is [`vm.ja.md`](vm.ja.md).

Contents
--------

1. [Overview](#1-overview)
2. [The pipeline of a run](#2-the-pipeline-of-a-run)
3. [The runtime layer](#3-the-runtime-layer)
4. [The shared front end](#4-the-shared-front-end)
5. [The bytecode](#5-the-bytecode)
6. [The executor](#6-the-executor)
7. [The LLVM lowering](#7-the-llvm-lowering)
8. [Sessions and hosts](#8-sessions-and-hosts)
9. [Build configurations](#9-build-configurations)
10. [Verification](#10-verification)
11. [Design decisions](#11-design-decisions)
12. [History](#12-history)

---

## 1. Overview

One front end, two consumers. The parser produces an AST; a bytecode
compiler turns it into a register-based, slot-resolved bytecode; and the
same bytecode is consumed either by an interpreter loop (the *executor*,
the default engine) or by an LLVM lowering (`--jit`, and `culebra build`
for ahead-of-time binaries).

```text
  .cul source
      │  parse (peglib grammar)
      ▼
     AST
      │  AST→AST transforms (generators, algebraic effects)
      ▼
  FnAnalysis      locals / slots, captures, EH + defer regions       fn_analysis.h
      ▼
  vm::Compiler    →  VmProgram (chunks of bytecode)                   vm.h
      │
      ├──► vm::Exec        interpreter loop (default, `--vm`)          vm.h
      │
      └──► vm::Lowering    LLVM IR → ORC JIT (`--jit`)                 vm_lowering.h
                           LLVM IR → object file (`culebra build`)
```

Both consumers run on one runtime layer (`rt.h`): the value model, the
`extern "C"` helpers that implement operators, containers, dispatch and
the standard library, the slab allocator, and the collector. The
executor calls those helpers directly; the lowering emits calls to them.
Because the two lanes execute the same instruction sequence over the
same helpers, the project's central requirement — identical behaviour,
identical error kind/message/position, and identical check *order* on
every lane — is a structural property of the pipeline rather than a
discipline maintained by hand.

Where things live:

| Component | Header | Notes |
|---|---|---|
| Runtime value model and helpers | `rt.h` and the `jit_*.h` fragments it includes | LLVM-free; the whole stdlib sits on it (`stdlib_jit.h`) |
| Front-end analysis | `fn_analysis.h` | `FuncInfo` / `FnAnalysis`, shared by both consumers |
| Bytecode format, compiler, executor | `vm.h` | `Op`, `Chunk`, `VmProgram`, `vm::Compiler`, `vm::Exec` |
| LLVM lowering, `--jit`, AOT | `vm_lowering.h` | the only VM header that needs LLVM |
| LLVM codegen context | `jit.h` | `struct JIT`: emitters, ownership handles, ORC/`exec`, object cache |
| Sessions (REPL, `culebra test`, embedding) | `vm_session.h`, `vm_repl.h`, `test_engine.h`, `vm_embed.h` | top-level bindings that outlive a program |
| Debugger | `debug_engine.h`, `vm_debug.h`, `dap.h` | the DAP protocol over a six-question engine interface |
| Canonical stdlib signatures | `canon_sigs.h`, `canon_sigs.gen.h` | parameter names/types/defaults every lane binds against |
| Collector, slab | `jit_gc.h`, `jit_slab.h` | see [`memory.md`](memory.md) |

The `jit_` prefix on the runtime fragments is historical: they were the
first ~10k lines of `jit.h` before the runtime was split out, and they
are now shared by both engines.

## 2. The pipeline of a run

`culebra prog.cul` (`src/main.cc`):

1. **Load.** `ModuleLoader::load_program` parses the entry file and every
   module it imports, returning a `LoadedModule` list in topological order
   (dependencies first, entry last). Parsing goes through
   `parse_with_transforms`, which runs the generator and effects
   transforms (§11) on the AST.
2. **Splice the stdlib preamble.** `splice_stdlib_preamble` scans the
   ASTs' tokens for stdlib names (`Time`, `Regex`, `Path`, the `assert_*`
   family, …) and prepends a synthesized `<stdlib>` module holding the
   builders of exactly the lazy modules the program names
   (`stdlib_preamble.h`, sources in `stdlib_preambles.gen.h`). A stdlib
   name therefore resolves at compile time, when the compiler sees it.
   The two lowering lanes then take the modules this binary has **baked**
   out of the preamble again (`resolve_baked_preamble`): the build compiled
   each stdlib module to a native object of its own
   (`culebra_preamble_cc`, one `culebra_preamble_<Name>` entry each,
   carried by the driver and by `libculebra_rt.a`), and the lowered
   program opens with a call to each entry — which performs the same
   `_lazy_ns_register` the spliced source would — instead of carrying
   ~20k lines of IR per module.
   The executor keeps compiling the spliced source, so the symmetry gate
   compares the baked code against it on every run;
   `CULEBRA_PREAMBLE_SOURCE=1` makes the lowering lanes splice again.
3. **Compile.** `vm::Compiler::compile_modules` peels the preamble off
   the front, compiles each dependency in a scope of its own, then the
   entry module, into one `VmProgram`. The program's chunk 0 is the entry
   module's top level; every function literal, method body and
   constructor adds a chunk.
4. **Run.** Either `vm::Exec::run(prog)` or
   `vm::run_modules_via_llvm(modules, …)` — the latter compiles the same
   program and hands it to `vm::Lowering`.

`--vm-dump` prints the bytecode instead of running it
(`vm::dump(prog)`), and `--jit --emit-llvm` prints the lowered IR. Both
are the first tools to reach for when the two lanes disagree.

The other entry points are thin variations on the same four steps:

| Entry | Compiles with | Runs on | Notes |
|---|---|---|---|
| `culebra prog.cul` | `compile_modules` | executor | the default |
| `culebra --jit prog.cul` | `compile_modules` | lowering (ORC) | `--jit-faststart`, `-O<n>`, `--emit-llvm` |
| `culebra build prog.cul` | `compile_modules` | lowering (object file) | linked against `libculebra_rt.a` |
| REPL (`culebra` with no file) | `compile_repl_line` per input | executor | session cells, §8.1 |
| `culebra test` | `compile_session_modules` per file | executor | §8.2 |
| `culebra test --doc` | `compile_modules` per block | executor or lowering | fresh `Runtime` per block |
| `culebra dap` | `compile_modules` with `Debug::Step` | executor | §8.3 |
| `vm::Embed` (C++ host) | `compile_session_modules` | executor | §8.4 |
| Playground (wasm) | `compile_modules` | executor | §9 |

Engine selection is explicit inside the repository: every `just` recipe
and CI workflow sets `CULEBRA_REQUIRE_EXPLICIT_ENGINE=1`, under which a
launch that picks the default engine aborts (`require_explicit_engine`
in `main.cc`). A test lane is thereby forced to name what it measures,
so a change of default cannot silently move a gate onto a different
engine. The only intentional exception is the release-diff gate (§10.3),
whose subject *is* the default.

## 3. The runtime layer

`rt.h` is what both consumers stand on. It includes, in a fixed order,
the value model (`jit_value.h`), the owned-resource stack for
deterministic `drop` (`jit_owned.h`), strings (`jit_string.h`), the core
`extern "C"` helpers (`jit_runtime.h`), fixed-layout views and class
construction (`jit_fixed.h`), multimethod dispatch and the keyword-call
machinery (`jit_dispatch.h`), the iterator protocol (`jit_iter.h`), and
the reference-counting implementation (`jit_mem.h`). None of it names
LLVM.

### 3.1 The value model

A value is 16 bytes: `JitValue { int64_t tag; int64_t data; }`. The tag
is an `int64` rather than an `int8` for an ABI reason: a `{i8, i64}`
return is coerced differently by the C compiler and by LLVM, and the
lowering's emitted calls must match the helpers' C signatures exactly.

| tag | payload |
|---|---|
| `TAG_NIL`, `TAG_BOOL`, `TAG_LONG`, `TAG_FLOAT` | immediate (a Float is the `double`'s bit pattern) |
| `TAG_STRING`, `TAG_STRINGVIEW` | pointer to an inline length header + bytes; traced by the collector, not refcounted |
| `TAG_ARRAY`, `TAG_TUPLE`, `TAG_SET`, `TAG_OBJECT`, `TAG_FUNC`, `TAG_TENSOR` | pointer to a refcounted heap struct (`JitArray`, `JitObject`, `JitClosure`, …) |
| `TAG_UNFILLED`, `TAG_KWREST`, `TAG_NO_SELF` | sentinels used by the calling convention (a defaulted parameter the caller left out, the `**rest` marker, "no receiver") |

Every refcounted struct keeps its `int64_t refcount` at offset zero, so a
retain or release is one memory operation whichever lane emits it. The
collector's own per-object metadata lives outside the object, in an
address-keyed registry (§`memory.md` 6.3).

The heap objects a VM needs are the runtime's existing ones:

- `JitCell` — a refcounted box holding one value; a captured variable
  lives in one (the Lua "upvalue" shape). Cells are also how forward
  references and REPL session bindings are represented (§4.2, §8.1).
- `JitClosure` — `fn_ptr` + an array of `JitCell*` captures + arity. A
  closure the executor builds has a VM trampoline as `fn_ptr` and its
  chunk descriptor in `captures[0]`; a closure the lowering builds has a
  native function there. Both are called through the same *JitFn ABI*:
  `void (JitValue* ret, JitClosure*, int8_t self_tag, int64_t self_data,
  int64_t n_args, JitValue* args)`.
- `JitObject` with `Shape` — a V8-style hidden class plus fixed slots,
  inline caches (`JitPropIC`) at property sites, and an any-key side map
  for non-String keys. An instance holds a `+1` on its class object
  (`JitObject::cls`).
- `JitArray`, `JitSet`, tuples, `JitTensor`.

Objects are allocated from a per-`Runtime` slab allocator
(`jit_slab.h`): size-segregated, non-moving, no locking, because a
`Runtime` — the thread-local root of the heap, the namespace caches, the
class and overload registries, the defer stack and the exception carrier
— is one per isolate. Two isolates share code and nothing else (§3.4).

### 3.2 The helpers

Compiled code and the executor reach the language's semantics through
`culebra_runtime_*` functions: arithmetic and comparison dispatch
(including operator overloads), indexing and property access, container
construction, the multimethod registry, class meta and instance
construction, the keyword-argument resolver (`JitParamMeta`), the
iterator protocol, `throw`/`try` translation and the defer stack, and
`drop` resolution. A helper owns the diagnostic it raises — kind,
message and the policy for which position it reports — so both lanes
report the same error because they call the same function.

The multimethod registry resolves only when it has to. A dispatcher
whose table holds exactly one untyped overload records that overload as
a monomorphic shortcut (`JitMultifnDispatcher`, refreshed on every
table mutation), and a positional call its arity accepts invokes it
directly — the plain annotation-free `fn name`, which would otherwise
pay type scoring and a trait walk to choose among one candidate.

Helpers follow a small set of ownership contracts for the values they
are handed (borrow, consume-on-every-exit, transfer); `memory.md` §4.3
tabulates them. Two RAII forms recur in the helpers' own C++:
`JitOwnedVal` (an owned argument released on every exit) and
`JitUnwindRelease` (release only if the helper throws).

The per-thread state a call frame touches sits in two objects:
`_jit_thread` (`jit_runtime.h`) holds every source position a call
publishes and the recursion depth, and `_culebra_rt` (`shared.h`) holds
this thread's current `Runtime` alongside a cache of its default. The
grouping is not tidiness. Mach-O has no initial-exec TLS model, so every
`thread_local` variable a helper touches costs its own call into dyld's
`_tlv_get_addr` — and publishing a call site used to touch twelve. On a
one-line function called in a loop, three quarters of the time went
there; collecting the variables into these two objects, changing no
value either holds, halved what a culebra call costs.

### 3.3 The standard library

`stdlib_jit.h` binds the standard library: native namespaces (`Math`,
`IO`, `Random`, `FS`, `Net`, `Canvas`, …) as one `kNsRows_*` table each
that the runtime resolves by name — grouped per namespace so an AOT
binary links only the namespaces its source names (`ns_groups()`,
`deployment.md` §4) — the built-in globals (`to_string`, `type_of`,
`range`, …) as `kBuiltinFns`, and the value-type methods. A program
installs it once with `install_jit_stdlib()`, which fills the
`ExtensionHooks` that `install_extension` (`rt.h`) registers — the same
seam an embedder uses to add its own namespaces (`deployment.md` §2).
The hooks carry no AST: what a call to an extension emits is decided by
the bytecode compiler, so the hooks only *declare* helpers on a module
being lowered (`declare_runtime`, the one member that needs LLVM) and
answer `is_builtin_var`.

The stdlib modules written in Culebra (`Time`, `Regex`, `Path`,
`Vector`, the assertion helpers, the effects runtime `__Eff`, …) are
compiled from source by the same compiler as user code, through the
preamble splice of §2.

The parameter lists every native declares — names, types, defaults,
keyword-only and rest markers, arity bounds — are one generated table,
`canon_sigs.gen.h`, read through `canon_sigs.h` by the compiler (for
compile-time checks and `f.params` introspection), by the runtime's
binder (for keyword calls and typed-parameter errors), and by the AOT
archive. A signature change edits that table.

### 3.4 Isolates

`Isolate.spawn`, `Channel` and `Parallel` are built on `isolate_core.h`
(the channel registry, fan-in, the worker pool, teardown joins — all
engine-neutral, speaking `SendNode`) and `sendable_jit.h` (the value
serializer). A closure crosses a thread boundary as its code reference
plus positionally copied captures: the child rebuilds the cells in the
same order on a `Runtime` of its own. A closure that captures a `mut`
binding is refused (`Chunk::mut_capture_names` carries the name for the
message), as is one whose descriptor names a native constructor.

## 4. The shared front end

`FnAnalysis` (`fn_analysis.h`) runs over each function's AST — the
module top level included — before it is compiled, and produces a
`FuncInfo`:

- **Locals and free variables.** Which names are the function's own,
  which are captured from an enclosing function (`free_vars`, with a
  parallel `free_var_mut` and `free_var_lazy`), and which of its own
  locals nested closures capture (`captured_locals` — these are the
  ones promoted to cells).
- **EH and defer flags.** Whether the body contains `try` or a scope with
  `defer` (`has_eh`), a defer at any depth (`has_any_defer`, which is
  what makes `return`/`break`/`continue` run pending defers), and the
  scopes and `try` regions that own defers (`scope_has_defer`,
  `try_region_has_defer`).
- **The body's own name.** An undecorated `fn name` or a `let name =
  fn …` literal sees its own name as a prologue-bound local
  (`own_name`, `own_name_source`), not as a capture of the declaring
  scope's cell — the capture would close a refcount ring (cell →
  closure → body → cell) that only the tracing backstop could reclaim.
- `uses_fn`, `uses_args`: whether the `fn` recursion handle or the
  overflow-argument Array is ever read, so frames that never mention
  them pay nothing.

The analysis is injected with one predicate, `is_builtin_var`, so it
stays independent of the stdlib machinery; shadowing is checked by
`lint::check_shadow`, single-sourced with `culebra lint`.

### 4.1 Declaration semantics

A declaration takes effect from the point it *runs*, not across the
whole statement list. The compiler models this with cells and a runtime
mutability bit:

- Every `fn name` of a statement list, and every `let` that a later
  closure captures before it is declared, is **pre-declared at scope
  entry** as an owned cell holding an unbound sentinel. A closure built
  earlier in the list captures a real cell (mutual recursion works); a
  read before the declaring statement ran raises `NameError` through
  `UnboundErr`, the read guard on lazy cells.
- The arms of `if` / `cond` do not open a scope, so a name several arms
  declare shares **one cell**, and which declaration ran is a fact of
  this call: the compiler records each declaration's `mut` in a slot
  beside the cell (`Binding::mut_slot`) and a bare write consults it.
- A declaration never writes through a borrowed capture: `fn () { let
  sh = sh + 1 }` shadows the enclosing `sh` rather than assigning it,
  because owning the cell is what distinguishes the two.
- A captured loop variable gets a fresh cell per iteration.

### 4.2 Names outside the function

A bare stdlib global (`println`, `to_string`, …) compiles to `NsGet`,
which resolves the name per `Runtime` through
`culebra_runtime_namespace_get` and yields an ordinary function value —
so a direct call and a call through a value (`let f = to_string; f()`)
share one code path and one set of diagnostics. A lexical binding still
shadows the builtin. A bare assignment to a stdlib global name is an
`ImmutableError`.

## 5. The bytecode

### 5.1 Format

An instruction is fixed-width: an `Op` and four `int32` operands
(`Insn { Op op; int32_t a, b, c, d; }`). Registers are the frame's slots,
each a `JitValue`; a chunk declares how many it needs (`num_slots`, at
most `kMaxSlots` = 8192 — a bound on machine stack per executor frame,
not on the format). There is no serialization: the bytecode is an
in-memory contract between the compiler and the two consumers, free to
change at any commit.

A `VmProgram` is a vector of `Chunk`s (chunk 0 is the top level; function
literals are reserved in creation order, so nested literals interleave
freely), plus the per-chunk `JitParamMeta` the keyword resolver reads
(`param_metas`) and, once the executor prepares it, one `VmFnDesc` per
chunk for its closures to point at.

A `Chunk` carries, besides `code`:

| field | what it is for |
|---|---|
| `consts`, `str_arena` | scalars, and string constants laid out in the runtime's string format so helpers read them like heap strings |
| `positions` | a run-length side table `insn → (line, col)`; error paths read it, so positions are structural rather than hand-threaded |
| `slot_names`, `slot_debug` | debug tables: a name per slot always, and per-binding live ranges (`SlotDebug`) when compiled for a debug session |
| `arity`, `required`, `param_names/types/has_default/mut`, `kwargs_rest_idx`, `first_kw_only_idx`, `cb_min/cb_max`, `variadic`, `return_type`, `multifn_name` | the signature: what a caller binds against and what `f.params` reports |
| `self_slot`, `fn_slot`, `fn_bound_slot`, `is_getter`, `forwards_args`, `counts_frame` | frame shape: where the receiver and the `fn` handle land, whether this is a getter body or the synthetic constructor thunk |
| `capture_src_slots`, `mut_capture_names` | the capture list: for each free variable, the creating frame's slot holding its cell |
| `slot_rank`, `slot_cell_rank` | declaration order (release ladders walk newest-first) and, per slot, when it became a cell — an index can be a temporary early on and a captured binding's cell later |
| `cleanups`, `temp_points`/`temp_slots`, `defer_mark_slot`, `owned_depths` | the unwind tables (§5.5) |
| `call_argpos`, `kwcalls`, `arity_checks`, `name_tables` | per-call argument positions, keyword-call layouts, built-in arity arms, class method-name tables |
| `call_targets` | per call instruction, the one function chunk its callee was resolved to, how that chunk relates to the value in the register (`Chunk::Reach`), and whether the callee is read straight out of a cell (§5.3) |

### 5.2 Ownership in the instruction stream

Reference counting is explicit: the compiler emits `Retain` and
`Release`, and the consumers only execute them. Four value-movement ops
set the vocabulary — `LoadConst` (a raw copy; constants are never
refcounted), `Move` (raw copy, paired with `Retain` for a borrow), `Take`
(transfer: the source becomes nil), `Release`. Every expression leaves a
`+1` in its result register; a statement's temporaries are released by a
sweep at its end.

The **borrow operand contract** governs throws: an operator or helper
borrows its operands, so when it throws every register is still
frame-owned and the unwind tables (§5.5) are the single releaser. This
is what makes the throw path's cleanup a table walk rather than a
per-site decision.

Captured variables live in cells through six ops — `CellNew`, `CellGet`,
`CellSet`, `CellRelease`, `BindCapture`, `ImmutErr` — so the refcount of
shared mutable state stays visible in the stream. `MakeClosure` fills a
new closure's captures from the callee chunk's `capture_src_slots`.

### 5.3 The opcode families

143 opcodes, grouped:

| family | ops | notes |
|---|---|---|
| values | `LoadConst` `Move` `Take` `Retain` `Release` | §5.2 |
| arithmetic, bitwise, comparison | `Neg` `Not` `Add` … `Pow` `MatMul` `BitAnd` … `Shr` `BitNot` `Eq` … `Ge` `JumpIfSame` | each is one runtime dispatch; `d=1` on an arithmetic op marks a compound assignment's in-place Tensor step |
| containers | `ArrayNew/Append/Push/Extend/Resize` `TupleNew/Push` `SetNew/Add` `ObjectNew/Set/SetAny/Merge` `RangeNew` `ChkLong` | the container absorbs the element's `+1` |
| access | `Index` `IndexWr` `IndexCo` `IndexSet` `PropSet` `PropWr` `PropCo` `PropVal` `PropRaw` `HasProp` `NsWrChk` `NilChk` | read / write / coalescing-write forms of subscript and property access; `PropVal` is a plain property read that may invoke a getter |
| calls | `Call` `CallM` `CallKw` `CallRecv` `Ret` `RecEnter` `RecLeave` `ArgsRest` `KwRest` `JumpIfFilled` `ChkArg` `ChkTypeAt` `PosSnap` `BoundPos` | the JitFn ABI; `CallM` resolves a method (user or built-in) on its receiver; `RecEnter` counts the frame against the recursion limit after the parameters are bound |
| built-in methods | `MethGate` `ChkParam` `BMeth` `BArity` `CbType` `ArityChk` `BareMethChk` | §5.4 |
| closures and names | `MakeClosure` `CellNew` `CellGet` `CellSet` `CellRelease` `BindCapture` `ImmutErr` `UnboundErr` `NsGet` `LazyNsReg` `FnHandle` `ModReg` `ModGet` | §4.1, §4.2; `ModReg`/`ModGet` publish and read a module's export object |
| functions and classes | `MultifnReg` `MfSelf` `ClsSelf` `ClassMeta` `ClassObj` `MakeInst` `FieldInit` `BindStatic` `RegGetter` `SelfMerge` `DeriveFn` `RegPack` `EnumVariant` `TraitReg` `TraitDefault` `TraitReset` `ClsParamsChk` `ClsParamsWalk` `WkErr` | `MultifnReg` registers a body into the runtime's arity-dispatch registry; class declarations build a meta and register members |
| patterns | `TypeMatch` `SeqChk` `SeqGet` `SeqRest` `ObjGet` `DestrErr` `JumpIfTag` | `match` arms and destructuring; a failed test jumps to the next arm with nothing live |
| control flow | `Jump` `JumpIfFalse` `JumpIfTrue` `JumpIfNil` `JumpIfNotNil` `Halt` | `JumpIfFalse` carries the shared truthiness coercion (a non-Bool condition is a TypeError) |
| loops | `ForPrep` `ForLoop` `ForOpen` `ForNext` `ForDispose` `Safepoint` | a counted `for` over a Long range is the fused pair; anything else walks a 12-slot cursor (`ForSlot`) through the protocol |
| exceptions and defer | `Throw` `RaiseErr` `DeferMark` `DeferPush` `DeferRunTo` `OwnedMark` `OwnedExit` `DropSuppress` `Drop` | §5.5; `OwnedMark`/`OwnedExit` bracket a scope on the owned-resource stack for deterministic `drop` |
| strings and output | `Fmt` `StrCat` `Disp` `Println` `SetOpPos` | interpolation, and the `println(<one arg>)` peephole |
| sessions and debug | `ReplCell` `ReplBind` `DbgStmt` | §8.1, §8.3 |

**A callee the compiler can name.** A `let name = fn …` its statement
list declares once is bound to that literal for good: it takes no `mut`,
nothing can rebind it, and the one thing that could change it is a
second declaration of the same name in the same scope. The compiler
tracks that as `Binding::Known` — one record holding the chunk, how it is
reached, the cell the answer is anchored to, and (below) the constructor a
`.new` on the name would enter. A capture inherits it, since the cell it
borrows is the one that binding owns, and a later declaration writing that
cell strikes what was resolved through it. The per-call answer goes in
`call_targets`. Only the code is static:
the closure still rides the register, because its captures are the
caller's. Both consumers then skip the same three things — the
`TAG_FUNC` gate with its two cold probes, the parameter-meta lookup
behind `check_pos_count_cls` (the cap is the callee chunk's and the
positional count is the site's, so the answer is a compile-time one),
and the `fn_ptr` indirection. The executor enters `run_frame` on the
named chunk; the lowering emits a direct call to that chunk's function.

This shape has no run-time fallback: the site does not test what it was
handed. What checks it is an `assert` in the executor's resolved arm,
against the closure that actually turns up — so the assert lane (§10,
`just test-assert` and CI's linux-assert job) runs the whole sweep with the
prediction armed, and a release build pays nothing.

`Chunk::Reach` names which of three shapes a resolved site is, and they are
alternatives rather than flags: `Direct` is the one just described, `Mono`
and `Guarded` are the two below. Each owes at most one run-time question,
which is why the executor asks it in one place (`resolved_entry`, shared by
`Call` and `CallM`) and the lowering emits one diamond for whichever shape
has one.

**And a `fn name`.** A `fn name` binds a dispatcher, not a closure: its
overloads live in the runtime's registry, so the body is not reachable
from the call site at all. One shape makes it reachable anyway — a name
the statement list declares exactly once, with no annotated parameter.
Only a same-scope second declaration appends to a dispatcher's table
(the `into` operand of `MultifnReg`), so such a table holds its one
untyped entry for as long as the dispatcher lives, and that entry is
the only method dispatch could ever pick. The compiler records it as
`Known::chunk` with `via_mono` set, and marks the site `Reach::Mono`;
the dispatcher carries that body in its second capture cell, rewritten
whenever the table is. A resolved site reads it — three loads, no
registry — and enters the named chunk with the body as the frame's
closure. Only the argument counts the lone overload accepts are
recorded, so every other count still reaches the dispatcher and its
`DispatchError`. The body's own name is the same grant: `MfSelf` yields
the dispatcher this body was registered into, which is what makes plain
recursion a direct call.

The question this shape asks is whether that capture still holds a body. A
null payload falls through to the dynamic arm the site would have taken
anyway — and the assert above covers this arm too, since what it is handed
is the body the dispatcher led to.

**And the constructor behind a class name.** `C.new(args)` is the one step
down a postfix chain that resolves: a constructor arrives through the class
object rather than by being the head, so `head_callee` never sees it. A class
declaration binds its name under the same "written exactly once" rule, so
`Known::ctor` inherits that argument rather than needing a new one, is
anchored to the same cell, and is struck by the same revocation. The grant is
refused where the answer can move at run time — an overload set makes the
constructor a dispatcher rather than a chunk, and a decorator that is not one
of the compile-time markers may hand back something that is not the class.

Inside a member the class name is read off the **receiver** (`ClsSelf`), so
its value is a run-time question even though its chunk is not: a method value
moved onto a foreign object would resolve the same name to a different class.
Such a site is `Reach::Guarded` — it keeps the chunk and asks, before
entering, whether this callee really is that chunk's closure. The executor
asks with `call_target_holds` (the predicate the assert above uses); the
lowering compares the closure's `fn_ptr` against the function the target
names. A miss takes the dynamic arm it would have taken all along. A class
name reached from the declaring scope or through a capture keeps the
unguarded answer.

**Borrowing the callee.** A call borrows what it calls: the caller's
`+1` is never handed to the callee, on any arm and on any lane. So the
callee does not need a register at all when the name behind it is one
the compiler already knows cannot change under the call. That is the
third fact `call_targets` carries: the `b` operand names a **cell**, and
the value inside it is the callee. Both the read's `Retain` and the
`Release` that matched it at the end of the statement disappear, and the
executor loses a whole instruction — the site is a `Call` and nothing
else. The same table row carries this whether or not the site's chunk
was resolved: where the callee is read is a fact about the instruction,
not about the code it reaches, so a re-declaration that strikes the
chunk leaves the bit alone.

Two properties make the borrow sound, and both are about the cell rather
than the value. Nothing writes it under the call: the binding takes no
`mut` and is neither conditional nor a session cell, which leaves the
declaration as its only writer, and a declaration is a statement of this
frame — the frame blocked inside the call. And nothing frees it under
the call: the cell is released by the ladder of the frame that owns the
slot, which is that same frame. A **capture** has the first property but
not the second — its cell belongs to the running closure, and nothing at
the site says the closure outlives the call — so a captured name is
copied as before, and the test is `slot_cell_` rather than `is_cell`.
`Exec::BorrowWitness` checks the claim in the assert lane (§10.2): the
cell still holds the same value when the call is over, the throw path
included.

### 5.4 Built-in methods are a table

The value-type methods (`'ab'.upper()`, `xs.map(f)`, `it.count()`, …)
are driven by one table, `bmeth_specs()`: one `BMethSpec` row per
`(name, argc)` with a receiver tag mask, per-argument declared types,
the optional trailing parameter's default, an optional keyword-only
parameter, and an id. The compiler reads the receiver gate and the
parameter checks out of the row (`MethGate` → `ChkParam` → `BMeth`), and
the executor and the lowering switch on the id. Which receiver resolves
a name at which arity decides between an `ArityError` and a method miss,
so **a spec's receiver mask must equal the set of receivers that resolve
that name at that arity** — everything outside the mask is answered as a
miss. Higher-order forms (`map`, `filter`, `sorted(by:)`, …) type-check
their callback parameter first, as the binder walks parameters in
order.

### 5.5 Exceptions, `defer`, and unwinding

A `try` region is static: a scope entry in `Chunk::cleanups` with a
`handler` pc and a `caught_slot`. Every lexical scope, loop body, `try`
body and `match` arm records one `Cleanup` entry as it closes
(innermost-first order), with its slot range, its `defer` mark slot if
it declares defers, how many cells existed at that point
(`cells_before`), and its parent.

A throw at `pc` is torn down scope by scope:

1. the statement's in-flight temporaries (`chunk_temps_at(pc)`, above
   the floor a catching `try` establishes) are released;
2. for each enclosing scope, innermost outward: run its pending defers
   back to its mark, release its own slots newest-declaration-first
   (`release_order_by_rank`, the same order the normal exit uses),
   disposing a `for-in` iterator at its rung;
3. at a scope with a handler, `culebra_runtime_try_translate` classifies
   the exception: a Culebra error materializes as an error object and a
   user throw already carries a value — either lands in `caught_slot` and
   execution resumes at the handler; a foreign C++ exception keeps
   unwinding;
4. leaving the frame runs the frame's own defers, resolves the owned
   region once (`culebra_runtime_owned_scope_exit`) and uncounts the
   recursion depth.

`defer` bodies are 0-arity closures pushed on the runtime's global LIFO
defer stack (`DeferPush`); marks are taken per frame and per scope
(`DeferMark`) and `DeferRunTo` runs back to one. A `try` ends its region
before the body's fall-through defer run, so a defer that throws at the
try body's normal exit escapes its own `catch`. Program exit releases
the top level's bindings without firing their `drop`
(`suppress_frame_drop`, `language.md` §17).

### 5.6 What the compiler rejects

The compiler throws `Unsupported` for a construct it cannot compile;
`compile_unit` turns that into a `VmError`. Inside a function literal the
rejection is caught by `compile_fn_chunk` and becomes a chunk whose whole
body raises it, so the module still compiles and only a call that
reaches the construct fails. The remaining rejections are a handful of
structural shapes — a binding alternative inside an or-pattern, a
`perform` in a control-flow condition — that the language rejects on
every lane.

## 6. The executor

`vm::Exec` is a switch-dispatch interpreter. `run_frame` allocates the
frame's register window as a variable-length array on the machine stack
(sized from the chunk's `num_slots`, so a small function pays for a
small frame), binds the parameters, the receiver and the `fn` handle in
the prologue, and enters `dispatch`. The window is on the C++ stack for
the collector's sake: the conservative scan finds every register as a
root without registration.

### 6.1 Closures and the trampoline

An executor closure is a real `JitClosure` whose `fn_ptr` is
`Exec::trampoline` (or `getter_trampoline`, so the runtime's getter
registry — keyed by `fn_ptr` — can still tell a getter body apart). Its
`captures[0]` is a cell holding a `VmFnDesc` (`{program, chunk}`); the
real captures follow. Native code calls a VM function exactly as it
calls a lowered one, and the runtime's helpers that need to know more —
the keyword resolver (`_jit_closure_meta_hook`), the lazy-namespace
rebuilder, the sendability check, the native-constructor test — read
the descriptor through hooks `Exec::prepare` installs.

The descriptor cell belongs to the closure, one per `MakeClosure`. A cell
shared by every closure of a chunk was tried and is wrong: closures of
one program run on several isolates at once, and a `JitCell`'s refcount
is a plain `int64_t` because the runtime is single-threaded per
`Runtime`. Sharing nothing across closures is what keeps the invariant.

### 6.2 Throws

`run_frame` wraps `dispatch` in `catch (...)` and calls `unwind`, which
walks the tables of §5.5 and either resumes at a handler or re-raises
with the frame uncounted. Uncaught errors are formatted at the engine
boundary (`run_prepared`): a user `throw` becomes the same `uncaught: …`
line every lane prints, and a positionless `CulebraError` is backfilled
from the last published op position. An interrupt reaches neither: it is
a `culebra::Interrupted`, a type of its own deriving from nothing, so a
handler written to report errors cannot name a type that catches one. It
still reaches script code the same way — the pads classify through the
pending carrier (§5.5), never through the C++ type.

### 6.3 Safepoints

Loops emit `Safepoint`, which polls the process-wide wake flag (Ctrl-C
and per-isolate cancel) and throws `Interrupted`. On wasm the same poll
is where the collector runs (§9): a threshold trip only sets a pending
flag, and the executor collects at the next instruction boundary, where
every live value of every frame sits in a register window the scan can
see.

### 6.4 Debug support

Compiled with `Debug::Step`, every statement of user source emits
`DbgStmt`, which calls the thread-local `DbgState::hook`; `run_frame`
pushes a `DbgFrame` (program, chunk, register window, pc) on entry and
pops it on every exit. `Debug::Break` emits only what the `debugger`
statement needs. A plain run (`Debug::Off`) emits neither.

## 7. The LLVM lowering

`vm::Lowering` (`vm_lowering.h`) lowers a `VmProgram` chunk by chunk:
each chunk becomes one LLVM function with the JitFn ABI, each register
an entry-block `alloca` that mem2reg promotes to SSA, each `Insn` a few
IR instructions or a call into the runtime. Where the executor calls a
helper, the lowering calls the corresponding emitter on `struct JIT`
(`emit_arith_step`, `emit_comparison_i1`, `value_to_bool`, the property
inline caches), so a construct's dispatch is defined once and consumed
twice.

Two entry points share the lowering: `run_program` builds a module,
optimizes it (`JIT::optimize_module`, the `-O2` default pipeline) and
hands it to ORC through `JIT::exec` — the isolate-join and
teardown-collect guards and the uncaught-error conversion live there;
`build_object` emits a `TargetMachine` object file plus a C `main` that
hands `__culebra_main` to `culebra_aot_bootstrap` in `libculebra_rt.a`
(`deployment.md` §1). The embedding names `deployment.md` publishes —
`JIT::run`, `JIT::run_modules`, `JIT::build_object` — are defined over
these.

Nothing host-side is registered for a lowered program: the parameter
metadata a keyword call resolves against is emitted as globals of the
module (`param_meta_global`, at the first `MakeClosure` site that needs
it, because `CreateGlobalString` needs an insertion block), which is
what makes the same lowering valid for an object file.

`--jit-faststart` skips the IR pipeline and takes the backend's fast
paths (`JIT::apply_fast_codegen`; the two levels move together), and
`CULEBRA_JIT_CACHE` enables an object cache keyed by
`JIT::jit_module_name` (sources, options). The two are not
interchangeable: the cache is installed on the IR→object compile layer,
so a hit skips the backend but never `JIT::optimize_module`, which
`run_program` has already run by then — a warm `-O2` launch still pays
the whole IR pipeline. Neither is what makes a `--jit` start-up cheap:
the stdlib modules a program names are not in the module at all (§2,
the baked preamble), so what gets lowered is the user's code.
`Lowering::build_preamble_object` is the same lowering
under a per-module entry name, run by `culebra_preamble_cc` at build;
`lower_program` opens `__culebra_main` with a call to each baked entry
the program names, and the lane's link supplies the symbol — the JIT
defines it from the driver's table (`JIT::define_baked_preambles`), the
built binary pulls the archive member.

One pass of that pipeline is the lowering's own. The four refcount
helpers each open with a guard — the value pair against
`_is_refcounted_value_tag` and a null payload, the cell pair against a
null cell — so a call that reaches one with constants satisfying it does
nothing. LLVM cannot see that, because in the emitted module the runtime
is an opaque declaration. And the constants are mostly the optimizer's
own doing rather than the emitter's: at `-O0` a whole test file emits
none, because the tag is still a load out of a register slot; what the
pipeline settles is the `Long` SROA promoted out of that slot, the
`TAG_NO_SELF` receiver a plain call passes, the null cell of a capture
that turned out to be direct. So `JIT::DropSettledRefcounts` drops them
inside the pipeline — LLVM's ARC optimizer in miniature — after each
`InstCombine`, and once more at the end, because loop peeling and the
vectorizers settle tags no peephole round sees. Only the code goes away:
what the calls would have done is nothing, on every lane. Nothing else
would notice if the peephole stopped firing, so `optimize_module`
asserts that none survive (§10.2).

That covers the tags a constant settles. The tags that stay a run-time
question are the common case on the frame path — a released parameter, a
receiver, a slot a scope tears down — and each of those used to be a call
into the opaque helper to be told the value owns nothing. So the release
emitter (`emit_value_release`) now opens with the helper's own test in
IR: the refcounted tags all fit a 32-bit mask, so membership is one shift
out of it, and the call sits behind it. The helper keeps its copy of the
test — this is a fast path, not the contract — and a tag the emitter
already knows is answered where it stands, so a settled release emits no
branch at all rather than one for the peephole above to fold.

That branch is IR at every release site, which is the cost side: on
`tests/test_core.cul` the optimized module grows by about a third and
`--jit` takes half again as long to reach the first instruction. The
executor lowers nothing, so what pays it is `--jit` and `culebra build` —
the two lanes chosen for throughput.

### 7.1 Ownership in the lowering

The lowering's C++ holds every transient `+1` in `JIT::Owned`, a
move-only RAII handle that must be consumed exactly once; a value read
out of it is `Pinned` to the basic block it was consumed in; a call that
may throw is emitted as `invoke`, spilling the live handles into a
per-function cleanup slot for the call's duration. `memory.md` §4 is the
full account. Cleanup pads are built by `JIT::CleanupPad`, whose
destructor emits the edge that continues the unwind, so a region cannot
be opened without being continued; only a handler prologue opens the
exception (`emit_handler_prologue`), and `tools/check_eh_balance.sh`
verifies that over the emitted IR.

### 7.2 Unwind shape

The lowering mirrors the executor's tables (§5.5) block for block: one
cleanup step per scope, entered at the rung for the bindings alive at
the throwing site, descending a shared chain (`fn.release.3 →
fn.release.2 → … → fn.unwind`) so each slot's release exists once. The
statement temporaries abandoned by a throw get one pad per distinct set,
sharing rungs by prefix, with a single re-raise at the foot. A `try`
scope's step classifies the exception after its releases
(`emit_classify_tail`), the order the executor uses.

A value live into a landing pad must be spilled — the unwinder restores
callee-saved registers and nothing else — so a function with many
throwing calls pays register pressure at `-O2` that the retired AST
codegen did not (its scope slots were never SSA). That is the same fact
as the lowered code running 2–4.5× faster on loops: the bytecode
register file is promoted to SSA. Measured on `tests/test_core.cul`, the
`-O2` compile is about 1.2× the old codegen's and the `-O0` compile is
not slower; four pad shapes were tried and the one in the tree is the
best of them (a single per-scope pad minimizes IR but produces
700-wide phis that take `llc` 30 s).

## 8. Sessions and hosts

The executor is the engine wherever a program's top-level bindings must
outlive the program that made them, or where compiling every body would
buy latency and nothing else. The lowering lane has no REPL, no debugger
and no unit-test host.

### 8.1 Sessions

`vm::ReplSession` (`vm.h`) owns a cell per top-level name, minted holding
the unbound sentinel and pinned as a GC root. A unit compiled with
`repl = true` (`compile_repl_line`, `compile_session_modules`) binds its
top-level names through `ReplCell` (load the session's cell for a name)
and `ReplBind` (declare/assign with the three mutability modes), so a
later input sees them and a closure built earlier sees what a later
input stores. `ReplCell` is re-emitted at each use rather than hoisted:
the binding is scope-wide, the instruction is not, and a name first
mentioned inside one `if` arm must still be loaded when a different arm
runs.

`vm::Session` (`vm_session.h`) adds what both session consumers need:
the retained programs (a closure reaches its bytecode through a
descriptor pointing into its program, so a program a session ran must
stay alive), the one-time built-in-traits prologue, and the **stdlib
delta** — a session sees one input at a time, so each input registers
only the lazy modules no earlier input named; registering a builder
twice would mint a second instance of a namespace.

A function literal in a session unit **captures the cell** for every name
it does not bind, rather than leaving the body to load it by name. A body
that loads a name by name does it on whatever thread runs it, and an
isolate or a `Parallel` worker has neither the session nor the `Runtime`
the cell was minted in — a closure crosses a thread boundary as
fn_ptr-plus-captures, no names (§3.4). The capture carries the binding's
`shadowed_builtin` flag with it, so a cell that is still unbound means
the stdlib global of that name, resolved on the thread that reads it; the
sendable encoding has a kind for that sentinel so such a capture can
cross at all. The cost is a capture per distinct free name, stdlib names
included — measured at ~30 ns per capture per closure instantiation.

Teardown runs the other way round from construction: a session's cells go
back before its retained programs and before the `Runtime`, because
releasing a cell can run Culebra code — a `drop` body — and that code
reaches its bytecode through a descriptor pointing into a program.
`release_all` drains its map rather than walking it, since a `drop` can
mint a cell behind the cursor.

The REPL (`vm_repl.h`) is one `Session` fed line by line, with the last
statement's value echoed from the session's result cell.

### 8.2 `culebra test`

The unit-test runner (`test_runner.h`) is engine-neutral above a small
`TestHost` interface — run a file, read a global, walk an Array or an
Object, call a function, describe the current throw — and `VmTestHost`
(`test_engine.h`) answers it from a `Session` of its own. `test` and `parametrize` are Culebra source
(`src/preambles/test_ambient.cul`), so the registry the host reads back
is an ordinary Array the program built. Each file compiles as a session
unit, which is what lets the runner call back into it after it has
returned. Values cross the interface as indices into the host's own
store, released to a mark when the test that used them ends, so a
fixture's `drop` fires into the right test's captured output.

**A file is a program.** Each gets its own `Runtime` — where the
namespace caches and the class and overload registries live — its own
session and cells, its own entry script (`Sys.script`, and the directory
`Embed.dir(...)` resolves against), and its own isolate-join guard; and
the runner runs that file's own tests while the scope is open rather than
loading every file first. Nothing a file writes at the top level reaches
the next one. A file may `import`: its module list runs as one session
unit, and `Session::run_modules` asks for the stdlib delta itself when
the list does not already carry a spliced preamble.

The gate runs every `tests/*.cul` through the runner, asserting the exit
code and the file count — these files register no `test(...)`, so
`passed` says nothing about coverage. It is the only lane that exercises
session scoping over the suite; the symmetry sweep runs the same files as
scripts.

The doctest runner (`doctest_runner.h`) needs no session: it takes a
`BlockRunner` — `(name, code) → {ok, kind, message}` — and `main.cc`
supplies one per engine, giving every block a fresh `Runtime`.

### 8.3 The debugger

`dap.h` keeps the DAP protocol, breakpoint tables, the pause/resume state
machine and output forwarding, and asks the engine six questions through
`DebugEngine` (`debug_engine.h`): run, frames, variables, has_name,
evaluate, set_variable. `VmDebugEngine` compiles with `Debug::Step` and
answers from the chunks' tables: `slot_debug` live ranges say which names
a frame stopped at `pc` can see and where their values are (a slot alone
cannot — the same index is a temporary in one scope and a binding in the
next), and `DbgState::frames` is the call stack.

Every query runs *on the parked debuggee thread*, from inside the stop
hook: a frame's register window is that thread's machine stack, and the
`Runtime` and the collector are per-thread. `evaluate` and
`set_variable` compile the expression as a REPL line against the frame's
bindings (`vm_debug.h`), which is how `set_variable`'s `ImmutableError`
comes free.

### 8.4 Embedding

`vm::Embed` (`vm_embed.h`) is the C++ host API (`deployment.md` §2): a
session whose bindings outlive the scripts it runs, so a host can run
source and then read a global or call a function. `vm::Value` is the
owning handle for values crossing the boundary — every retain and
release stays inside it. Each `Embed` carries its own `ReplSession` and
swaps it in for the duration of every call, so two embeds on one thread
share nothing.

## 9. Build configurations

`CULEBRA_JIT_ENABLED` means "LLVM is linked". It guards `jit.h`,
`vm_lowering.h`, the `declare_runtime` member of `stdlib_jit.h`, and the
AOT bootstrap. Everything else — the runtime layer, the compiler, the
executor, the stdlib, the sessions — builds without it, so a build
without LLVM (`cmake` with the JIT option off, `just build-no-jit`) is a
complete engine with one lane, and `--version` says which lanes a binary
has (`vm`, or `vm+jit`). `just test-no-jit` runs that configuration
rather than merely linking it.

The AOT runtime archives (`libculebra_rt*.a`, `src/runtime/`) compile the
same headers as the driver into a library a built program links; the
`culebra_runtime_*` symbol set the lowering names must exist in both
(`tools/check_jit_host_symbols.sh`, `tools/check_rt_archive_tls.sh`).

The Playground (`playground/wasm_main.cc`, built with `em++`) is the
executor on wasm. Two platform facts shape it. A `Runtime` is created per
run rather than per page, because the namespace caches point into the
program that built them. And the conservative collector cannot see wasm
locals — they live outside linear memory, and `setjmp` spills nothing
there — so under `gc::kDeferToSafepoint` (on for `__EMSCRIPTEN__` only)
no collection runs inline: a threshold trip sets a flag, the executor
polls `safepoint_collect()` at instruction boundaries, and every
helper-to-user call passes through `_jit_invoke`, whose
`SafepointUnsafeScope` defers the poll while a helper suspended between
two VM frames may hold the only reference in its locals. Dispatch sites
audited to keep everything in registers use `_jit_invoke_rooted` and
stay collectable. `just check-playground` runs the committed wasm under
node, each case twice in one instance, against the native executor.

## 10. Verification

The engines are held to each other, to frozen expectations, and to the
previous release. Every lane names its engine (§2), and every comparison
folds the exit code in — stdout alone lets a segfault read as agreement.

### 10.1 Symmetry between the lanes

| gate | what it compares | where |
|---|---|---|
| vm/jit symmetry | every `tests/*.cul` on `--vm` and `--jit`, stdout + exit code | `just test-dev`, `just test` |
| isolate suite | `tests/isolate/*.cul` on both lanes | same |
| `vm_cases` | `tools/bench/vm_cases/`: both lanes against frozen `expected/` outputs and exit codes, then again under `CULEBRA_GC_STRESS=1` | same (`compare.sh`; `--freeze` re-records an intended change) |
| doctest | every documentation block on both lanes | `just doctest` |
| difftest | the generated template-combinator corpus (`tools/difftest/gen.cul`, ~17k cases), `--vm` against `--jit` byte for byte | `just test` (`tools/difftest/run.sh`) |
| AOT | `culebra build` output == `--jit` output per test | `just test` |
| codegen backends | `-O0` and `--jit-faststart` against `--vm` | `just test` |

`misc/run_all_backends.sh` is the single-script form of the symmetry
check, used by the Windows CI jobs.

### 10.2 Checks on the emitted IR

- `culebra --jit --emit-llvm f.cul | opt -passes=verify` — the standing
  check for lowering work; `run_program` also verifies every module it
  builds and throws on failure.
- **IR diffing.** A refactor that must not change codegen is verified by
  `--jit -O0 --emit-llvm` over all of `tests/*.cul` before and after,
  byte-identical, stderr included.
- `tools/check_eh_balance.sh` (every `__cxa_begin_catch` is closed; no
  rethrow without an unwind edge), `tools/check_alloca_discipline.sh`
  (scratch slots stay in the entry block — a non-entry `alloca` in a loop
  grows the stack every pass), `tools/check_rc_discipline.sh` (the count
  of hand-placed retain/release sites in `jit.h` may only shrink).
- **Asserts.** Three invariants no output can betray ride the assert
  lane (`just test-assert`, CI's `linux-assert`), which runs the whole
  sweep with `NDEBUG` off: the resolved call's predicted chunk is the
  closure that turns up (§5.4), a borrowed callee's cell still holds the
  same value when its call is over (§5.4), and no settled refcount call
  survives the pipeline (§7).

### 10.3 The previous release as oracle

Both consumers are handed bytecode by one compiler, so a compiler bug
makes both lanes give the same wrong answer and §10.1 stays green. The
independent second implementation is the previous release's binary:
already built, frozen, downloadable for three platforms.
`tools/difftest/release_diff.sh` runs the generated corpus under a
baseline binary and under this build and reports every case whose
behaviour changed; both sides run on their default engine, with no flag.
Every difference must be named as a glob over the case label in
`tools/difftest/release_diff_allow.txt` — an unlisted change fails the
gate, a listed pattern matching nothing is reported so the file can
shrink once the release that needed it ships — which makes that file the
draft of the release notes. A baseline that predates a syntax the corpus
now uses is handled per case (`::: unsupported`), and the comparator has
its own self-test (`release_diff_selftest.sh`) so a quiet report is a
comparison that was checked. CI runs it on every master push against the
latest published release.

### 10.4 Coverage of the shared surface

`just coverage` (`tools/coverage/run.sh`, `-DCULEBRA_COVERAGE=ON`)
measures which functions of the shared-fate surface — `vm::Compiler`,
the `culebra_runtime_*` helpers, the `JIT` emitters — are reached *only*
by the generated corpus and by no hand-written test.
`tools/coverage/corpus_only_coverage.txt` is that set as a ratchet: a
new corpus-only function fails the report, and the file is empty. A
35-minute instrumented run, outside the per-PR gates.

### 10.5 Memory

The leak gates (leak-fuzz, leak-abort, the rc-leak battery, GC stress,
the assert lane) are described with the collector in `memory.md` §5–6.

## 11. Design decisions

- **Register-based, not stack-based.** Registers map directly onto the
  frame layout the analysis already computes and onto SSA values in the
  lowering; the interpreter loop dispatches fewer times per expression.
- **RC explicit in the stream.** One emitter, two consumers: the leak
  gates verify the compiler's placement once, and the executor and the
  lowering inherit it. The alternative — each consumer deciding its own
  retains — is two placements to keep in agreement.
- **Positions and debug tables ride the bytecode.** A side table per
  chunk makes error positions and the debugger structural rather than
  hand-carried by every emitter.
- **Bytecode is internal.** No serialization, no version, never written
  to disk. That is what lets the format change whenever a construct
  needs it to.
- **Generators and effects are AST→AST transforms.**
  `generator_transform.h` rewrites a `yield`ing function into a class
  implementing the iterator protocol; `effects_transform.h` rewrites
  `effect fn` / `perform` / `handle` into plain source over the `__Eff`
  runtime. Both lower control flow through a flat-dispatch CPS state
  machine with locals on the state instance. The engines need no
  generator- or effect-specific support, so they agree by construction.
  Frame suspension in the VM would be a simplification, not a
  requirement.
- **Built-in methods are data.** A table row per `(name, argc)` keeps the
  reject decision, the executor and the lowering on one definition
  (§5.4).
- **A named callee buys the dispatch, not the body.** Resolving a call
  to one chunk (§5.3) takes about a fifth off a monomorphic call on
  `--jit`, and a few percent on the executor. It does not get the callee
  inlined: at `-O2` the cost model declines a body that carries its own
  landing pads, and forcing the inline measures the same, because what
  remains per call is opaque runtime helpers — the call-position
  publish, the recursion counter's enter/leave, the owned-scope bracket,
  and the argument's retain/release pair — which SROA cannot cancel
  across a call to an external symbol. The `fn name` declaration form is
  not resolved at all: its cell holds the multimethod dispatcher, and
  the body closure lives only in the registry.
- **Sessions are cells, not a second name resolver.** The REPL, the test
  host, the embedding API and the debugger's `evaluate` all reuse the
  same mechanism (§8.1).
- **Stdlib written in Culebra is compiled like user code.** The lazy
  preamble modules go through the same compiler, so a stdlib module
  cannot behave differently from a user module that says the same thing.
- **Prior art.** Every mature dynamic-language implementation with both
  an interpreter and a JIT shares a bytecode between them — CPython, Ruby
  (YARV + YJIT), Lua/LuaJIT, V8 (Ignition + TurboFan), SpiderMonkey. The
  runtime value model is documented in `memory.md` §3–4, with its design
  lineage in `memory.md` §7.

Known costs, accepted: `--jit` compiles about 1.2× slower than the
retired AST codegen at `-O2` for the reason in §7.2; a script that names
several lazy stdlib modules and then does nothing starts ~7 ms later on
the executor than the tree-walker did, because the preamble is compiled
rather than walked (real programs come out ahead — the preamble
bytecode is not cached for that reason); the lowering lane has no REPL
or debugger.

## 12. History

The bytecode VM replaced a tree-walking interpreter in 2026. The VM
entered as a third engine beside the interpreter and the AST-walking
JIT, was diffed against both until the generated corpus found no
divergence, then became the front end of the JIT (the AST codegen was
deleted), then the default engine (v0.3.0), and finally the only engine
once the interpreter and its oracles were retired (v0.3.1 is the last
release carrying both). The release-diff gate of §10.3 is what replaced
the interpreter as the independent second opinion. The commit history
of `include/vm.h`, `include/vm_lowering.h` and `include/rt.h` records
the migration; the design proposal that started it and the per-phase
findings live in that history rather than here.
