#pragma once

// The bytecode VM (docs/internals/vm.md): a register-based, RC-explicit,
// lowering-friendly bytecode; a compiler whose front end is the shared
// FnAnalysis (fn_analysis.h); and the executor — the default engine — running
// on the shared runtime value model (rt.h). The same bytecode's other
// consumer, the LLVM lowering that `--jit` and `culebra build` (AOT) compile,
// lives in vm_lowering.h; this header needs no LLVM, so it is the engine of a
// build without the JIT. `--vm` names the executor explicitly and `--vm-dump`
// prints the bytecode. The format is an in-memory contract only: a construct
// the compiler cannot express is a compile-time "VmError", never a fallback.

#include <builtin_signatures.h>
#include <fn_analysis.h>
#include <module_loader.h>
#include <parser.h>
#include <range_bounds.h>
#include <rt.h>  // the shared value model and runtime helpers
#include <shared.h>
#include <stdlib_jit.h>  // culebra_runtime_println + the rt::println decl hook

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace culebra::vm {

// The frame's slot budget. Nothing about the format needs one — an
// instruction's operands are int32, and both engines size their register
// window from the chunk's own `num_slots` — so this only bounds how much
// machine stack one executor frame may claim (16 bytes a slot). It was 256
// while the compiled lanes were opt-in, which measurement says is inside the
// range real programs use: the leak battery's pattern file wants 382 and the
// widest file in tests/ wants 253. A top level is what grows (its slots count
// its bindings and temps) and a top level does not recurse, so the frames that
// can approach this are the ones that only ever exist once; a recursive
// function's own frame stays small. Exceeding it is still a clean VmError
// rather than a stack overflow.
inline constexpr int32_t kMaxSlots = 8192;
// How deeply one frame may nest lexical scopes: each one takes an entry in
// the frame's owned-mark array (Op::OwnedMark).
// Now that each frame's mark array is sized from its own owned_depths (see
// run_frame), this bounds only the frame that actually nests that deep — and
// 64 was inside the range a program may ask for, which the interpreter accepts
// without a limit at all.
inline constexpr int32_t kMaxOwnedDepth = 1024;

// One fixed-width instruction. Registers are frame slots holding JitValue.
// RC is explicit in the stream (vm.md §5.2): the compiler emits Retain/Release;
// the VM and the LLVM lowering only execute them. Constants are scalars or
// chunk-owned strings — neither is refcounted — so LoadConst is a raw copy;
// Array is the slice's RC-real type, and its +1 flows through the same
// Take/Release discipline as everything else.
enum class Op : uint8_t {
  LoadConst,   // regs[a] = consts[b]
  Move,        // regs[a] = regs[b] (raw copy; pair with Retain for a borrow)
  Take,        // regs[a] = regs[b]; regs[b] = nil (ownership transfer)
  Retain,      // retain regs[a]
  Release,     // release regs[a]; regs[a] = nil — destructive, so every slot
               // has one owner and sweeps cannot double-release
  Neg,         // regs[a] = -regs[b]: Long inline, else num_neg (line/col)
  Not,         // regs[a] = !to_bool(regs[b]) — strict Bool truthiness
  Add,         // regs[a] = regs[b] + regs[c]; these five mirror the JIT's
  Sub,         // emit_binop_dispatch: both-Long inline (wrapping; Div/Mod
  Mul,         // zero-guarded), both-numeric via double, else the num_*
  Div,         // runtime helper (string concat, __op__ dispatch, TypeError)
  Mod,         // with the instruction's line/col from the position table.
               // d=1 marks a compound-assignment step: a Tensor lhs mutates
               // in place through the num_inplace_*_borrow twin (interp's
               // try_tensor_inplace / the JIT's emit_arith_step(inplace)),
               // observable through aliases. Mod has no in-place form.
  Pow,         // regs[a] = regs[b] ** regs[c] via num_pow_borrow — no inline
               // fast path (the AST JIT's generic tail is the same call; its
               // literal-exponent peepholes are numeric-guarded, so every
               // input agrees with the helper)
  JumpIfSame,  // jump to b when regs[a] is a Tensor and regs[c] holds the very
               // same handle: the compound step mutated its receiver in place
               // rather than producing a new value, so the assignment rebinds
               // nothing and the target's mutability does not gate it (the
               // interp returns before env->assign; the JIT branches around
               // its compound.rebind block on the same test).
  MatMul,      // regs[a] = regs[b] @ regs[c] via num_matmul_borrow. `@` has
               // no numeric meaning and no in-place form (the result's shape
               // is not the lhs's), so `@=` steps through this op too: it is
               // always a `__matmul__` dispatch, and a TypeError otherwise.
  BitAnd,      // regs[a] = regs[b] & regs[c]; the five bitwise ops are
  BitOr,       // Long-only — anything else raises the typed TypeError
  BitXor,      // ("expected Long, got X", lhs reported first), matching
  Shl,         // emit_bitwise_step. Shift counts mask to the low 6 bits;
  Shr,         // `<<` wraps (unsigned shift), `>>` is arithmetic.
  BitNot,      // regs[a] = ~regs[b], Long-only like the binary five
  Eq,          // regs[a] = Bool(regs[b] == regs[c]): both-Long inline, else
  Ne,          // value_equal; ordering ops below go through
  Lt,          // value_{less,leq,greater,geq} (line/col-carrying, matching
  Le,          // the interp's nil-ordering and __lt__ dispatch)
  Gt,          //
  Ge,          //
  ArrayNew,    // regs[a] = fresh empty Array (+1)
  ArrayAppend, // append regs[c] into array regs[a] at index b; regs[c] = nil
               // (the array absorbs the +1, mirroring compile_array)
  ArrayPush,   // append regs[b] to array regs[a] — a spread-mode literal's
               // non-spread element, where the running index no longer
               // aligns (compile_array's has_spread switch); the array
               // absorbs the +1, regs[b] = nil
  ArrayExtend, // splice regs[b]'s elements into array regs[a] (`...x`).
               // array_extend borrows the source (retaining each copied
               // element) — the register keeps its +1; a non-Array/Tuple/Set
               // source raises the typed TypeError with this instruction's
               // line/col (the spread element's position)
  ArrayResize, // sized-literal prefill: resize array regs[a] to regs[b]
               // (strict Long, else "expected Long"; a negative count is
               // ValueError) filling with regs[c], or nil when c == -1.
               // The default is borrowed — each filled slot retains its own
               // alias ref. Positioned at the count expression's node.
  TupleNew,    // regs[a] = fresh empty Tuple (+1)
  TuplePush,   // append regs[b] into tuple regs[a]; regs[b] = nil (absorbed)
  SetNew,      // regs[a] = fresh empty Set (+1)
  SetAdd,      // add regs[b] into set regs[a]; regs[b] = nil — set_add
               // absorbs the +1 (releases it on a duplicate). Hashing an
               // unhashable element throws positionless before the absorb:
               // the register still owns the value (the handler ladder
               // frees it) and the SetOpPos published just before (the
               // literal's position, compile_set's emit order) anchors it.
  ObjectNew,   // regs[a] = fresh empty Object (+1)
  ObjectNewShaped, // regs[a] = fresh Object (+1) whose Shape and (nil,
               // immutable) slots are pre-built from
               // chunk.object_shape_specs[b]'s static key list — emitted
               // instead of ObjectNew only when every property of a literal
               // is a plain IDENTIFIER key (compile_object's eligibility
               // check), so the ObjectSet run that follows always finds its
               // slot already present and never calls transition_add. See
               // culebra_runtime_object_new_shaped.
  ObjectSet,   // an IDENTIFIER/shorthand property: regs[a].consts[c] =
               // regs[b] (absorbed), consts[c] mutable iff d — object_set
               // with is_init=true, so a duplicate key (or a later spread)
               // overwrites last-wins like the interp's initialize, rather
               // than tripping the immutable-entry guard meant for `o.x = v`.
               // A well-known name (next/has_next/iter/drop) validates the
               // contract and throws positionless; compile_object emits
               // SetOpPos with the literal's own position right before,
               // only for those four names (emit_object_set's condition).
  ObjectSetAny,// a non-IDENTIFIER literal key (String/Float/Number/Nil/Bool/
               // Tuple): regs[a][regs[b]] = regs[c] (both absorbed), mutable
               // iff d — object_set_any with is_init=true. Hashing an
               // unhashable key throws positionless; compile_object always
               // publishes SetOpPos with the literal's own position right
               // before (also covers a well-known String-literal key, e.g.
               // `{"next": 5}`, which routes through the same contract
               // check as the IDENTIFIER path).
  ObjectMerge, // `{...obj}`: merge regs[b]'s entries into regs[a] (later
               // keys win); object_merge borrows the source (retaining each
               // copied entry, is_init=true so it can override an already-
               // immutable key) — the register keeps its +1. A non-Object
               // source raises the typed TypeError at this instruction's
               // position, which compile_object stamps at the SPREAD_ELEM
               // node (compile_array's ArrayExtend precedent), not the
               // literal's own position.
  ModReg,      // hand the runtime module table regs[a] — a dependency
               // module's export Object — under the absolute path consts[b],
               // absorbing the register's +1 (module_register). The importing
               // module's ModGet reads it back out by the same key, which is
               // why both spell the path the way resolve_module_path does.
  ModGet,      // regs[a] = the export Object registered for the absolute path
               // consts[b], as a fresh +1 (module_get). A path nothing
               // registered is an ImportError at this instruction — the
               // loader makes that unreachable from a normal run, since it
               // walks every `import` before any of them compiles.
  RangeNew,    // regs[a] = fresh Range object (+1) from the contiguous run
               // regs[b..b+2] = start, end, step (each Long — ChkLong ran
               // right after its endpoint compiled; step defaults via a
               // LoadConst 1). c packs has_start | has_end<<1 | inclusive<<2
               // (an absent endpoint's slot is unread). make_range only
               // allocates — never throws.
  ChkLong,     // if regs[a].tag != Long, throw the typed TypeError
               // ("expected Long, got X") at this instruction's position —
               // a range endpoint's strictness check, emitted between
               // endpoint compiles so `0.5..t()` throws before t() runs
               // (the JIT's value_to_long-after-each-compile order).
  NilChk,      // `expr!!`: if regs[a] is nil, throw NilError at this
               // instruction's position (the `!!` token — both backends'
               // anchor, unlike the index errors' chain-head). Usually
               // falls through; the value passes unchanged.
  Index,       // regs[a] = regs[b][regs[c]] (+1). A range-valued key
               // (is_range — the full-shape gate) slices the receiver via
               // culebra_runtime_slice (Array copy / String view / Tuple
               // new tuple, else "expected Array"), checked before the
               // receiver dispatch like emit_index_step. Otherwise the
               // plain point read, emit_point_index's dispatch: Array/Tuple
               // by Long key (a negative index counts from the end), Object
               // by value key (object_get_any — KeyError on a plain-dict
               // miss), else "expected Array". The key stays register-owned
               // (a retain feeds the consuming Object helper).
  IndexWr,     // like Index, but the write-context read of `a[i] op= v`:
               // the JIT's compound set dispatch — Array-only (a Tuple is
               // not writable) with array_set's bounds rule (a negative
               // index is IndexError, never normalized), Object unchanged.
  IndexCo,     // regs[a] = current value for `a[i] ??= v`: the Array arm
               // reads like IndexWr; the Object arm passes the nc
               // receiver-kind rejects, then object_get_for_coalesce —
               // a plain-dict miss is nil (the write test), not KeyError.
  IndexSet,    // regs[a][regs[b]] = regs[c]: Array by Long via array_set,
               // Object via object_set_any (runtime-inserted slots default
               // mutable, is_init=false), else "expected Array". The value
               // stays register-owned (a retain feeds the consuming store),
               // so the assignment expression still reads it afterwards.
  PropSet,     // regs[a].consts[c] = regs[b]: object_set with is_init=false
               // (an existing slot's mut flag decides; a fresh slot defaults
               // mutable). A non-Object receiver is the write reject's typed
               // TypeError ("expected Object, Array, or Tensor" — the read
               // wording, both backends'). The value stays register-owned
               // (IndexSet's retain-feeds-the-store rule). All throws carry
               // this instruction's position (the assignment statement)
               // except the well-known contract on insert, which is
               // positionless — compile emits SetOpPos right before, for
               // those four names only (the ObjectSet literal's condition) —
               // and a packed field's own error, which anchors at the DOT
               // node (packed line<<32|col in consts[d], as PropWr's miss).
  PropWr,      // regs[a] = property consts[c] of regs[b], the write-context
               // read of `o.k op= v`: receiver gate as PropSet, then the
               // Shared-view ImmutableError (ahead of the existence check,
               // the interp's order), then the missing-property
               // AttributeError — anchored at the DOT node, whose packed
               // line<<32|col rides consts[d] (every other throw here uses
               // the statement, this instruction's own stamp). The hit is a
               // raw retained view (+1), never bound.
  PropCo,      // regs[a] = current value for `o.k ??= v`: receiver gate,
               // then the nc receiver-kind rejects (Shared view / packed
               // field, at the statement), then a plain property read — a
               // miss is nil (the JumpIfNotNil write test), and a closed
               // namespace's unknown member raises its AttributeError at
               // the DOT node (consts[d], the interp's
               // reject_namespace_write anchor) before the RHS ever runs.
  NsWrChk,     // the plain `o.k = v` namespace-typo check: when regs[a] is
               // an Object, culebra_runtime_check_namespace_write raises
               // AttributeError at this instruction's position (the DOT
               // node) for a closed namespace's unknown member. A
               // non-Object receiver falls through silently — PropSet's
               // receiver gate reports it, same observable order.
  PropVal,     // regs[a] = property consts[c] of regs[b], read AS A VALUE
               // (+1) — a bare `x.name`, the JIT's non-call DOT arm. The
               // resolve is emit_property_get's (function introspection on a
               // Function receiver, own slot then proto on an Object, a
               // container's miss as nil, a scalar's TypeError), then
               // bind_method_value / getter_or_value: a function-valued
               // property comes back bound to its receiver, a getter fires.
               // Stamped at the chain head, both backends' anchor for the
               // read's errors.
  BareMethChk, // the bare built-in method reject: when regs[a] is nil and
               // regs[b] has no own field consts[c], let
               // culebra_runtime_bare_builtin_reject decide from the interp's
               // own tables whether that receiver would have dispatched
               // consts[c] as a built-in method — `let m = 'ab'.size` is a
               // TypeError on every backend, any other miss stays nil.
               // Emitted only for a built-in method name (the JIT's
               // compile-time filter) and stamped at the DOT node: the
               // method name's own position, NOT the chain head.
  MethGate,    // the gate a built-in method call passes before any argument
               // evaluates: regs[a] = the user-defined method shadowing the
               // built-in, or the TAG_NO_SELF sentinel when the built-in
               // itself answers. An Object receiver is asked first — the read
               // is emit_property_get's, so a namespace's unknown member
               // raises its AttributeError right here, and the shadow test is
               // compile_user_method_over_builtin's checkBB (a Function-valued
               // read, an own/proto slot of the name, or a Shared view of a
               // dict-builtin name). Every other receiver takes the built-in,
               // whose receiver gate (specs[d]'s tag mask, else the
               // resolution error) runs here too — ahead of the arguments,
               // both backends' order: `(5).repeat(boom())` never evaluates
               // boom(). consts[c] names the method; positioned at the chain
               // head, where both backends anchor a receiver's failure.
  ChkParam,    // a built-in parameter's declared-type check: unless regs[b]
               // (MethGate's gate slot) still holds the sentinel — a user
               // method binds by its own signature, not the built-in's —
               // check regs[a] against parameter d of spec c and raise the
               // interp binder's wording at this instruction's position: the
               // argument's own node. A polymorphic built-in declares the
               // parameter per receiver arm, so the spec's own mask decides
               // whether the check covers regs[b+1] (the receiver) at all:
               // `'ab'.contains(x)` wants StringLike, `[1].contains(x)` takes
               // anything.
  CallRecv,    // the receiver a method call hands over, filtered through
               // culebra_runtime_call_receiver: a promoted body local named
               // by an own slot of a lowering's state object is storage, so
               // calling it passes no receiver. regs[a] is the run head,
               // consts[c] the property name; the drop releases in place.
  CbType,      // a callback parameter's declared `Function` type, checked on
               // its own so a keyword-only sibling's check cannot answer
               // first (`sorted_by(5, reverse: 5)` names `f`, the parameter
               // the binder reaches first). regs[a] is the callback, regs[b]
               // MethGate's gate slot, consts[c] the parameter name. The
               // arity half stays inside the sorter, where interp's own body
               // checks it. Positioned at the callback's argument.
  ArityChk,    // the count-based ArityError a receiver owes when it resolves
               // this name at a DIFFERENT arity than the one written — the
               // one case a miss must not report as "expected Function, got
               // Nil" (`'ab'.count()` is String.count(sub) called wrong, not
               // the iterator's count). regs[a] is MethGate's gate slot,
               // regs[a+1] the receiver, c the spec: only emitted where the
               // table itself holds a rival arity for the name, and it runs
               // BEFORE the arguments, where the interp's own check sits.
               // Positioned at the ARGUMENTS node, interp's anchor for every
               // count-based built-in ArityError.
  BMeth,       // regs[a] = built-in method c over the run at b: regs[b] is
               // MethGate's gate slot, regs[b+1] the receiver, and
               // regs[b+2..b+2+d) the arguments — defaults already
               // materialized, so d is the built-in's own arity. A gate slot
               // holding anything but the sentinel is the shadowing user
               // method, called through the JitFn ABI exactly as CallM does
               // (consuming receiver and args; a non-Function there is the
               // interp's "expected Function, got Long"). The built-in arm
               // BORROWS the receiver and the arguments — the statement sweep
               // stays their sole releaser — and yields a fresh +1.
  PropRaw,     // regs[a] = the raw property view of consts[c] on regs[b],
               // retained (+1) — the callee half of `x.m(...)`. Unlike
               // PropVal it neither binds `self` into a wrapper nor runs the
               // bare-method reject: CallM passes the receiver as `self`
               // itself, compile_method_call's property tail.
  HasProp,     // regs[a] = Bool: does regs[b] resolve consts[c] as a property
               // of its own? The UFCS gate (interp's receiver_has_property /
               // the JIT's method-or-UFCS branch). `d` carries the static half
               // — the tags whose own built-in table binds the name, plus
               // kHasPropIterBit for an iterator-protocol name — and the Object
               // arm is probed here: an own/proto slot, a conforming
               // instance's trait default, or a closed namespace (which
               // answers every name itself). Nothrow.
  Drop,        // regs[a] = nil, running regs[b]'s `drop` through the
               // at-most-once guard (culebra_runtime_explicit_drop). The
               // receiver is borrowed; a receiver with no `drop` is a no-op.
               // Explicit `x.drop()` only — the automatic one rides the
               // scope ladder.
  ClsParamsChk,  // regs[a] = Bool: is regs[b] a class instance with no
               // `parameters` of its own (own/proto slot or trait default)?
               // That is exactly when the synthesized walker below answers,
               // and its negation hands the call to the ordinary gate.
  ClsParamsWalk,  // regs[a] = the synthesized `parameters()` of the class
               // instance regs[b] — a flat Array of the fields that hold
               // class instances (culebra_runtime_class_parameters_walk).
               // Borrows the receiver.
  SeqChk,      // destructuring gate: unless regs[a] is an Array or Tuple of
               // exactly c elements (d=0) — at least c, when the pattern has
               // a `...rest` (d=1) — pc = b. One JitArray backs both tags, so
               // either pattern form matches either value, the unification
               // interp's indexed_sequence applies. Never throws.
  SeqGet,      // regs[a] = element c of the sequence regs[b], retained (+1);
               // a negative c counts back from the end, which is where a
               // post-rest element sits. Not the language's `a[i]`: a SeqChk
               // has already fixed the shape and the length, so this reads a
               // slot that exists and raises nothing.
  SeqRest,     // regs[a] = a fresh Array (+1) holding regs[b]'s elements from
               // c up to size - d — a `...rest` tail, bounded by the same
               // SeqChk minimum.
  ObjGet,      // unless regs[c] is an Object holding key consts[d], pc = b;
               // otherwise regs[a] = that property (+1). One op for an object
               // pattern entry's presence test and its read.
  DestrErr,    // throw the destructure mismatch ValueError at this
               // instruction's position — stamped at the DESTRUCTURE_ASSIGN
               // node, where the other backends anchor it. Never falls
               // through.
  Jump,        // pc = a
  JumpIfFalse, // if !to_bool(regs[a]) pc = b (strict Bool truthiness)
  JumpIfTrue,  // if to_bool(regs[a]) pc = b
  JumpIfNotNil,// if regs[a].tag != TAG_NIL pc = b (`??` short-circuit)
  JumpIfNil,   // if regs[a].tag == TAG_NIL pc = b — a `?[` receiver guard:
               // the target is the chain's merge point, so a nil receiver
               // collapses the whole remaining postfix chain (key
               // expressions included) to nil, both backends' safe-nav
  JumpIfTag,   // if regs[a].tag == c pc = b — a pattern's tag gate; a
               // match test never throws, so it carries no position

  MakeClosure, // regs[a] = new closure (+1) over function chunk b. In the
               // executor its fn_ptr is Exec::trampoline and captures[0]
               // holds the chunk's shared descriptor cell, retained per
               // closure (Long-valued, so its value release is a no-op —
               // the bound-method thunk precedent); in
               // the lowered module it is the chunk's native function. The
               // chunk's capture_src_slots fill the (remaining) captures
               // with retained cell pointers from this frame.
  Call,        // regs[a] = call regs[b] with args regs[c..c+d). Publishes
               // the call site (set_call_site), checks TAG_FUNC (else the
               // interp's "expected Function" TypeError), and goes through
               // the JitFn ABI — the callee takes ownership of each arg, so
               // the arg slots are nil'd after the call.
  CallM,       // regs[a] = call regs[b] with self = regs[c] and args
               // regs[c+1..c+1+d) — one contiguous run whose head is the
               // receiver. Same JitFn ABI as Call (the callee consumes the
               // receiver and every arg, so the whole run is nil'd after),
               // and the same TAG_FUNC check — which is where a missing
               // method surfaces, as "expected Function, got Nil".
  CallKw,      // regs[a] = call regs[b] with the keyword-carrying argument
               // list described by kwcalls[d] over the run at c: the
               // receiver (when the spec says so), then the positionals,
               // the keyword values and the `**` operands, each kind in its
               // own stretch. The runtime resolver binds names against the
               // callee's parameter metadata and consumes every value, so
               // the whole run is nil'd after — Call's contract, one
               // resolver deeper.
  RaiseErr,    // throw consts[b] (kind) / consts[c] (message) at this
               // instruction's position. For an error the compiler can see
               // but the language raises when control reaches it — a
               // repeated keyword argument, say — so it stays catchable.
  Ret,         // return regs[a] from the frame (+1 transfers to the caller)
  CellNew,     // regs[a] = new JitCell absorbing regs[b]'s +1; regs[b] = nil.
               // Releases the cell previously in regs[a] (null on first run —
               // a loop's per-iteration redeclaration).
               // The cell pointer rides the reg as a Long, so the plain
               // Release/Retain ops are no-ops on it (the descriptor-cell
               // precedent); only CellRelease touches the cell's refcount.
  CellGet,     // regs[a] = cell(regs[b])->value, retained (+1) — load_slot
  CellSet,     // cell(regs[a])->value = regs[b] (absorbs the +1, releases the
               // old value after the store, like store_slot); regs[b] = nil
  CellRelease, // release cell regs[a]; regs[a] = nil — an owned cell slot's
               // scope exit (borrowed capture slots take the no-op Release)
  BindCapture, // regs[a] = closure's captures[b] as a borrowed cell pointer
               // (executor offsets past the descriptor at captures[0])
  ImmutErr,    // throw ImmutableError for name consts[a] (runtime, after the
               // RHS evaluated — `if false { x = 1 }` stays silent, interp
               // parity); never falls through
  UnboundErr,  // if regs[a] is the unbound sentinel (TAG_NO_SELF), throw
               // NameError for name consts[b] — the read guard on a `fn
               // name` dispatcher cell read before its decl statement ran
               // (the JIT's emit_unbound_value_guard). Usually falls through.
               // c: regs[a] is the CELL, so the sentinel to test is the
               // value inside it (a borrowed callee, which never copies).
  MultifnReg,  // regs[a] = dispatcher (+1) after registering closure regs[b]
               // as one of its overloads. regs[c] (c >= 0) is the dispatcher
               // an earlier arm of this same declaration installed, borrowed —
               // c = -1 mints a fresh dispatcher over a fresh table, which is
               // what makes a re-run declaration keep its own overloads. d =
               // the body's chunk index, supplying the display name and the
               // param_names the registry records (types stay null — arity-only
               // dispatch in the slice). The registry absorbs the body's +1;
               // regs[b] = nil.
  MfSelf,      // regs[a] = this frame's multifn self-handle: the dispatcher
               // the executing body was registered into (+1), or the unbound
               // sentinel if it outlived it (culebra_runtime_multifn_self).
               // Prologue-only; the binding it feeds is lazy, so reads guard
               // the sentinel with the usual NameError.
  ClsSelf,     // regs[a] = the class a member frame runs for: the receiver
               // regs[b]'s class object (an instance's JitObject::cls, or
               // the receiver itself when a static's), +1, or the unbound
               // sentinel with no receiver of the kind
               // (culebra_runtime_class_self). Prologue-only, like MfSelf.
  WkErr,       // throw the well-known-property contract error for name
               // consts[a] — a well-known name that carries an overload set,
               // whose dispatcher build_class_meta would never get to reject.
               // Positionless, so a SetOpPos at the CLASS_DECL precedes it.
               // Never falls through.
  ClassMeta,   // regs[a] = the shared class meta Object (+1) built from the
               // method closures in the run regs[b .. b+c), named by the
               // chunk's name table d (build_class_meta). object_set
               // consumes every closure's +1 — including on the
               // well-known-contract throw, which is positionless, so a
               // SetOpPos stamped at the CLASS_DECL precedes it — so the
               // whole run is nil'd.
  DeriveFn,    // regs[a] = a captureless closure (+1) over the shared runtime
               // thunk for @derive kind b (0=eq/1=hash/2=to_s/3=cmp —
               // culebra_runtime_make_derived_method). Emitted into the class
               // meta's method run alongside the user methods, so dispatch
               // and Set/Object key lookup find it the same way. nothrow.
  RegPack,     // register the @packable byte layout of class consts[a] from
               // the "name:Type;…" spec consts[b] (culebra_runtime_register_
               // packable), at the declaration — the layout must land in the
               // running process, which under AOT is not the compiling one.
               // The field types are lint-validated, so nothrow. d=1 reads
               // the spec as an enum's tagged union instead.
  EnumVariant, // regs[a] = one variant of enum consts[d], named consts[c]:
               // with arity b=0 the singleton instance it always is
               // (build_variant), otherwise the constructor closure that
               // builds one from b positional payload fields
               // (make_variant_ctor). Both yield a fresh +1.
  TypeMatch,   // a pattern's type test: unless regs[a] satisfies the type
               // name consts[c] (culebra_runtime_type_matches — a class tag,
               // an enum's variant or parent, a trait's conformance, `T?`,
               // `fn(…) -> R`), jump to b. A constructor pattern gates on
               // TAG_OBJECT first, since a primitive whose name the variant
               // collides with answers true; a `v: T` annotation needs no
               // such gate, having already taken the inline tag compare for
               // every name that is a primitive's.
  ClassObj,    // regs[a] = fresh Object (+1) marked `is_class`, so `C(args)`
               // finds its `new` (object_new + mark_class)
  BindStatic,  // bind regs[c] into class object regs[a] under name consts[b]
               // (object_bind_static — the raw emplace the class namespace
               // uses, so a member named `drop` stays an ordinary function
               // and the well-known contract does not apply). The slot
               // absorbs the +1; regs[c] = nil.
  MakeInst,    // regs[a] = a fresh instance (+1) of class consts[c]:
               // build_class_instance over the borrowed {meta, field-init,
               // new-body} triple at regs[b .. b+3), the constructor's
               // receiver regs[d] — the class object, which the instance
               // keeps a +1 on (JitObject::cls) — and the frame's own
               // argument run. The helper consumes every argument's +1 on
               // every exit (the body absorbs them, a default ctor releases
               // them), so the argument run is nil'd. Emitted as the whole
               // body of a synthetic constructor chunk.
  FieldInit,   // run the field-init closure regs[a] on receiver regs[b]
               // (run_field_init; both borrowed). Emitted at the top of a
               // `new` body, after parameter binding — interp's timing, so
               // an arity error fires with no field side effects.
  RegGetter,   // register closure regs[a] as a getter, so a bare `obj.name`
               // read invokes it (culebra_runtime_register_getter, keyed by
               // the closure's fn_ptr). Emitted once per getter method, at
               // its declaration — the JIT's register_getter call.
  SelfMerge,   // regs[a] = the frame's `self`: the ABI receiver when the
               // call supplied one, else the value in the captured cell
               // regs[b] (rt::self_merge, +1 either way). Only a frame that
               // captured an enclosing `self` emits it — a method's own
               // receiver arrives through the chunk's self_slot.
  TraitReset,  // drop every registered default of trait consts[a]
               // (culebra_runtime_trait_defaults_reset). First instruction
               // of a trait declaration, so a re-declaration that drops a
               // default really drops it (the interp's defaults.clear()).
  TraitDefault,  // register closure regs[c] as trait consts[a]'s default
               // for method consts[b]
               // (culebra_runtime_register_trait_default). The registry
               // absorbs the +1; regs[c] = nil.
  TraitReg,    // install the contract of trait consts[a] from its
               // `name:arity:has_default;…` spec consts[b] and `Super;…`
               // list consts[c] (culebra_runtime_register_trait). Last
               // instruction of the declaration, where the interp's
               // register_trait sits: a well-known contract throw above
               // leaves the previous declaration's contract standing.
  PosSnap,     // regs[a] = the position a type error on argument index c
               // reports (c = -1: the return value), packed line<<32|col —
               // the call site, or the declaration position consts[b] when
               // the caller published none (culebra_runtime_param_pos).
               // Taken in the prologue: the body's own calls (and a default
               // expression's) overwrite the caller's position long before
               // the check runs.
  ChkTypeAt,   // check regs[a] against the type consts[b] in the context
               // consts[c] ("return value", "parameter 'x'"), reporting at
               // the position in regs[d] (PosSnap's snapshot) — or, with
               // d < 0, at this instruction's own position, which is what an
               // assignment's `x: T = e` annotation wants. The return check
               // is emitted before the frame's defers run, as the JIT's is.
  ChkArg,      // check regs[a] against the declared type consts[b] for
               // parameter consts[c] at argument index d, resolving the
               // report position on the FAILURE path
               // (culebra_runtime_type_check_param: the argument's own
               // expression, or the call site for a default-filled slot).
               // The eager form above is what a param after the first
               // default takes instead — user code has run by then.
  JumpIfFilled,  // jump to b when regs[a] is a supplied argument; falls
               // through to the default expression when the prologue left
               // the slot TAG_UNFILLED.
  ArgsRest,    // regs[a] = the overflow arguments as a fresh Array: what the
               // caller passed past the last regular parameter. Their `+1`s
               // are the caller's transfer, taken over here (the prologue
               // leaves them alone for chunks that reach them), so the Array
               // is the sole owner. Backs `__ARGS__` and a named `*args`.
  KwRest,      // bind the `**rest` slot regs[a]: the keyword resolver's own
               // Object when it marked the slot (TAG_KWREST, retagged here),
               // and a fresh empty one otherwise. No positional can fill it —
               // whatever a plain call left at that index is an overflow
               // argument, already released by the prologue.
  RecEnter,    // count a frame (culebra_runtime_recursion_enter). a=1 is
               // this frame's own, emitted after the parameters bind so a
               // typed-param TypeError outranks RecursionError (the JIT
               // prologue's order); it also stashes the depth the frame's
               // handlers restore to. a=0 brackets a default expression,
               // which runs before the frame is counted and would otherwise
               // recurse uncounted — paired with RecLeave.
  RecLeave,    // uncount the default expression's frame. Skipped by a throw,
               // like the JIT's; the enclosing frame's restore corrects it.
  NsGet,       // regs[a] = stdlib global consts[b] (+1), resolved through
               // culebra_runtime_namespace_get — the per-Runtime cached
               // closure the JIT's emit_builtin_var_get slow path returns,
               // so `f == to_string` holds across reads. Compile-time
               // allowlisted; the resolver's NameError path is unreachable.
  SetOpPos,    // publish this instruction's line/col as the pending op
               // position (culebra_runtime_set_op_pos). Emitted before an
               // interpolation piece renders — stamped at the string
               // literal, so a positionless throw in the display walk
               // (nesting too deep) backfills there, the JIT's emit order.
  BoundPos,    // publish this instruction's line/col as the NEXT call's
               // boundary (culebra_runtime_set_call_boundary) — where a
               // positionless error escaping that call lands, the interp's
               // eval() boundary. Emitted at a UFCS site, stamped at the
               // postfix chain node, right before the Call (whose
               // set_call_site consumes the pending pair); every other call
               // shape leaves the boundary at its call site.
  Disp,        // regs[a] = display string of regs[b] (value_to_display,
               // borrow) — a bare `{expr}` piece. The result is a fresh
               // heap string: TAG_STRING, so outside RC entirely.
  Fmt,         // regs[a] = format_value(regs[b], spec) with this instruction's
               // line/col (the piece node's position — spec errors report
               // there) — a `{expr:spec}` piece. The spec is consts[c], or
               // regs[c] when d=1: a spec carrying its own `{field}`
               // (`"{s:>{w}}"`) is assembled at run time out of the same
               // literal/display pieces the other engines concatenate.
  StrCat,      // regs[a] = str_concat(regs[b], regs[c]); both operands are
               // Strings by construction (interpolation pieces)
  Throw,       // user `throw`: regs[a]'s +1 transfers to the thrown-value
               // carrier (culebra_runtime_throw); regs[a] is nil'd BEFORE the
               // raise so a handler's release ladder cannot double-release
               // the payload. Never falls through.
  DeferMark,   // regs[a] = defer-stack mark (a Long; culebra_runtime_
               // defer_mark). Frame marks are the chunk's first insn, so a
               // throw at any pc finds the slot populated.
  DeferPush,   // push closure regs[a] onto the global defer stack (borrow;
               // the runtime retains — compile_defer's push-then-drop)
  DeferRunTo,  // culebra_runtime_defer_run_to(regs[a]): pop and run every
               // defer above the mark, LIFO. A defer body that throws drops
               // the rest to the mark and propagates (interp/Swift order).
  ForOpen,     // open a for-in over the iterable in regs[a+kForIterable]:
               // switch on its tag, fill the cursor run at a, and park what
               // the protocol derives from and the iterator `iter()` returns
               // in the run's own slots (so every exit path releases them).
               // Throws for a non-iterable and for a broken protocol, both at
               // the iterable expression — this instruction's position.
  ForNext,     // advance the cursor at a: on a step, regs[a+kForElem] takes
               // the element's +1 and execution falls through; on a drained
               // iterator, jump to b. c/d carry the statement's line/col as
               // immediates (the step runs per iteration; no table search).
               // The element sits in that slot only until the binding below
               // takes it over, which is what makes the unwind ladder's
               // release of it exactly-once.
  ForDispose,  // close the iterator at a+kForIter if it carries `dispose`,
               // once (a's kForDisposed slot is the latch every exit path
               // shares). d=1 swallows a throwing dispose — docs §18.5 only
               // does that while an exception is already unwinding. Emitted
               // by the release ladders, at the rung that frees the iterator.
  ForPrep,     // control quad at base=a: {cur, end, step, exhausted}. Rejects
               // a zero step (ValueError at the range expression's position,
               // like rt::range_step_check), zeroes the exhausted slot, jumps
               // to the ForLoop at b.
  ForLoop,     // quad at a: constructs RangeBounds (range_bounds.h — the
               // sequence oracle all backends share; inclusive rides `d` as
               // a per-loop immediate); if !done(): take(), write back
               // cur/exhausted, release + rebind the loop var slot c, jump
               // to the body at b; else fall through.
  Println,     // culebra_runtime_println(regs[a])
  ToFloat,     // regs[a] = to_float(regs[b]), borrowing b: the two numeric
               // tags inline (a Float passes through, a Long widens), and
               // every other input — a String to parse, anything else to
               // reject — goes to to_float_any with the instruction's
               // line/col, so the parse and the TypeError stay identical
  Safepoint,   // interrupt poll — every loop back edge carries one
  DropSuppress,  // culebra_runtime_set_drop_suppressed(a): brackets the top
                 // level's own release ladder, whose bindings leak
                 // un-dropped at program exit (docs §17)
  BArity,        // the diagnostic a built-in method name owes when the table
                 // that resolves it on regs[a] would not bind this argument
                 // shape — one arm per receiver, baked from the interp's own
                 // parameter lists (builtin_call_verdict). Falls through
                 // for a receiver that does not resolve the name at all: the
                 // property read after it answers that one.
  LazyNsReg,     // record regs[b] as the builder of the lazy stdlib module
                 // named consts[c] — the `_lazy_ns_register` intrinsic the
                 // preamble splice emits, which is how a compiled lane
                 // reaches `Time` / `assert_eq` / `__Eff` at all
  FnHandle,      // regs[a] = the value-read of `fn` in a receiver frame: the
                 // wrapper binding regs[b] to the frame's own closure
                 // (regs[c] holds it), cached in regs[d]
  OwnedMark,     // marks[a] = the owned stack's next id, taken at scope
                 // entry. The marks are a small frame array of their own,
                 // indexed by the scope's static depth, NOT registers: a
                 // register a scope leaves behind gets reused, and a later
                 // generation of that index may be a cell — whose CellNew
                 // releases the previous generation blindly and would read
                 // this Long as a pointer. (Defer marks dodge that by being
                 // named slots their own ladder nils; there is no room for
                 // that here, since the exit reads its mark AFTER the ladder.)
  OwnedExit,     // resolve the owned region above marks[a] (deterministic
                 // drop for what refcounting alone cannot reclaim: escaped
                 // or cyclic resources). Emitted after the scope's release
                 // ladder, so anything held only by its bindings has
                 // already died the ordinary way.
  ReplCell,      // regs[a] = the REPL session's cell for name consts[b], as
                 // a Long. Minted holding the unbound sentinel on first
                 // reference, so reading a name no line has declared raises
                 // the same NameError a forward reference does. Borrowed:
                 // the session owns the cell for the whole session, which is
                 // what lets the closure one line builds and the line that
                 // later fills the name share it — and why the slot takes
                 // the ladder's plain (no-op) Release, like a capture's.
  ReplBind,      // record what name consts[b] means in the session now.
                 // a = 0 / 1: the declaration that just stored bound it
                 // `let` / `mut`, so a later line can take a `mut` back.
                 // a = 2: a plain `x = v` is about to store — the name's own
                 // mutability decides (ImmutableError when it has none), and
                 // an undeclared name is simply declared by the store that
                 // follows, unless c says the name is a built-in root
                 // binding, which is immutable (`println = 5`).
  DbgStmt,       // a statement boundary in user source, emitted only when the
                 // unit was compiled for debugging. a = 1 for the `debugger`
                 // statement, which breaks whether or not anything asked to
                 // stop here. With a debug session attached the instruction
                 // hands it this frame; with none (a plain `--debug` run) a
                 // forced one drops into the same minimal break the JIT's
                 // `debugger` compiles to.
  Halt,
};

struct Insn {
  Op op;
  int32_t a = 0, b = 0, c = 0, d = 0;
};

// A generic for-in's cursor: one contiguous slot run, allocated by
// compile_for_generic and addressed by these offsets from its base. The
// leading five hold Values the loop's scope owns, so the ordinary release
// ladder frees them on every exit; the rest are plain Longs the walk keeps
// its place in, whose release is a no-op. `kForIter` is the rung the ladder
// disposes at — after the element, before the value it was derived from,
// which is the order the other two backends tear a loop down in.
enum ForSlot : int32_t {
  // The latch sits at the base so the ladder — which walks the run downwards
  // — clears it AFTER the dispose that sets it. Anywhere above kForIter and
  // the write would outlive its own release, leaving a stale Long for
  // whatever the slot index becomes next.
  kForDisposed = 0,  // 1 once dispose ran: no exit path repeats it
  kForIterable = 1,  // the iterable expression's result
  kForSetArr = 2,    // a Set's materialised member Array (nil otherwise)
  kForSrc = 3,       // what the protocol derives from (nil for array/string)
  kForIter = 4,      // the iterator `iter()` returned (nil for array/string)
  kForElem = 5,      // this iteration's element, nil outside the hand-over
  kForKind = 6,      // ForKind: which cursor walks this iterable
  kForPos = 7,       // element index / byte offset
  kForCount = 8,     // element count / byte length
  kForPtr = 9,       // array storage / string bytes
  kForHasNext = 10,  // the hoisted has_next closure
  kForNext = 11,     // the hoisted next closure
  kForSlots = 12,
};

// --- Value-type built-in methods -------------------------------------------
//
// The slice answers a named set of built-in methods, keyed by (name, argc) —
// compile_builtin_method's own dispatch granularity. A shape outside the table
// (`'ab'.repeat()`, the 0-arg `it.count()`) is a compile-time reject rather
// than a half-answer, because which receiver resolves a name at which arity is
// what decides between an ArityError and a method miss, and only the tables
// know. One table drives all three lanes: the compiler reads the receiver gate
// and the parameter checks out of it, the executor and the lowering switch on
// the id.
enum class BMeth : uint8_t {
  Size, Empty, Presence,                        // any sized receiver
  Upper, Lower, Capitalize, Trim, Lines, View,  // String/StringView, no args
  Repeat, Truncate, TrimStart, TrimEnd, Tr, Split, StartsWith, EndsWith,
  // String, added alongside the rest of the String surface: `reverse` and
  // `index_of` are not here because an Array binds those names too — they
  // share one spec whose arm dispatches on the receiver's tag.
  Title, Normalize, EqIgnoreCase, LastIndexOf, StripPrefix, StripSuffix,
  // `index_of(sub, start)` is String-only where the 1-arg form is also an
  // Array's, and MethGate reads a gate's receiver mask from the id alone —
  // so the two arities cannot share one.
  IndexOfFrom,
  RSplit, SplitWhitespace, IsDigit, IsAlpha, IsAlnum, IsSpace, IsAscii,
  Push, Pop, Insert, RemoveAt, Extend, Reverse, IndexOf,  // Array
  Slice, Contains, ToString,                    // polymorphic, eager
  Join, Sum, Product, Min, Max, ToSet,  // Array(+Tensor)+iterator, eager
  Sorted,                                // Array only, eager
  Distinct, Flatten,                    // iterator-shaped Object only
  // Array(+iterator)-dual, callback-taking: the eager arm drains an Array in
  // place, the lazy arm (Map/Filter/FlatMap) hands back a new iterator
  // instead of draining. The callback's own type/arity check lives inside
  // the runtime helper (JitHofCallback / _culebra_capture_callback), so no
  // param is declared here — see the BMethSpec rows below.
  // `AnyOf` (not `Any`): BMethSpec rows below bring both this enum and
  // BParam into scope with `using enum`, and BParam::Any would collide.
  Map, Filter, ForEach, AnyOf, All, Find, FlatMap, MinBy, MaxBy, Reduce,
  GroupBy, Partition,
  SortBy, SortedBy,                     // Array only, mutates / eager
  Union, Intersect, Diff, SymDiff, Subset, Superset, Add,
  Remove,                               // Set element, or an Object's dict key
  ToArray,                              // Set only
  Keys, Has, GetOrPut,                   // Object (dict) only
  Get,                                    // Array or Object (dual, eager)
  // The iterator sources: every spelling that turns a value into a fresh
  // iterator object. `Iter` is the general one; the String walkers and
  // SplitIter answer a String's own; Enumerate/ToObject/Unzip are
  // Array-or-iterator duals like ToSet.
  Iter, CodePoints, Bytes, Graphemes, SplitIter, StrCount,
  Enumerate, ToObject, Unzip,
  // Iterator-only, on an iterator-shaped Object: the lazy adapters that wrap
  // one iterator in another, and the terminals that drive one to its end.
  Take, Skip, TakeWhile, SkipWhile, Tap, ChunkBy, StepBy, Scan, Chunks,
  Windows, Chain, Zip,
  Collect, IterCount, First, Last, Nth, Position,
  // Tensor-only. The autograd five and the activations are no-arg; the four
  // axis reductions share tensor_reduce_axis and differ only by their op.
  Shape, Pow, Transpose, Clone, RequiresGrad, Grad, Backward, ZeroGrad,
  Detach, Relu, Sigmoid, Softmax, TensorLog, Reshape, Mean,
  SumAxis, MeanAxis, MaxAxis, Argmax, Dot, LinearSigmoid, Item, IndexSelect,
  // im2col's own building blocks. Each takes one params Array ([axis, win,
  // step] / [axis, before, after] / [axis, orig_size, step]) rather than 3
  // positional Longs, same reason Reshape takes a dims Array: BMethSpec's
  // own arg-passing caps at 2.
  Unfold, Pad, Fold,
  // General axis reorder — a distinct name from Transpose (which only
  // reverses every axis) rather than an overload of it, matching PyTorch's
  // own transpose()/permute() split. One axes Array, same reason as above.
  Permute,
  Sort,                                 // Array only, in place, returns nil
  Values,                               // Object (dict) only
};

// The reduction each axis-ful id asks tensor_reduce_axis for.
inline int64_t bmeth_reduce_op(BMeth id) {
  switch (id) {
    case BMeth::MeanAxis: return static_cast<int64_t>(culebra::Op::Mean);
    case BMeth::MaxAxis: return static_cast<int64_t>(culebra::Op::Max);
    case BMeth::Argmax: return static_cast<int64_t>(culebra::Op::Argmax);
    default: return static_cast<int64_t>(culebra::Op::Sum);
  }
}

// The `which` selector culebra_runtime_str_is_class switches on, in its
// order — the JIT's kClasses table reads the same one.
inline int64_t bmeth_str_class(BMeth id) {
  switch (id) {
    case BMeth::IsDigit: return 0;
    case BMeth::IsAlpha: return 1;
    case BMeth::IsAlnum: return 2;
    case BMeth::IsSpace: return 3;
    default:             return 4;  // IsAscii
  }
}

// The elementwise unary each activation id asks tensor_unary for.
inline int64_t bmeth_unary_op(BMeth id) {
  switch (id) {
    case BMeth::Sigmoid: return static_cast<int64_t>(culebra::Op::Sigmoid);
    case BMeth::Softmax: return static_cast<int64_t>(culebra::Op::Softmax);
    case BMeth::TensorLog: return static_cast<int64_t>(culebra::Op::Log);
    default: return static_cast<int64_t>(culebra::Op::Relu);
  }
}

// The receivers a built-in resolves on, one bit per value tag. A gate's set
// must be exactly the set of receivers whose built-in table holds the name at
// this arity, because everything else takes emit_receiver_resolution_error's
// two-way answer outright — a scalar cannot hold members at all ("expected
// Object, Array, or Tensor"), anything else simply lacks the method ("expected
// Function, got Nil") — instead of asking the tables again.
using BRecvMask = uint16_t;

inline constexpr BRecvMask bmeth_tag_bit(int8_t tag) {
  return static_cast<BRecvMask>(1u << tag);
}

inline constexpr BRecvMask kRecvStrLike =
    bmeth_tag_bit(TAG_STRING) | bmeth_tag_bit(TAG_STRINGVIEW);
inline constexpr BRecvMask kRecvArray = bmeth_tag_bit(TAG_ARRAY);
// union/intersect/diff/sym_diff/subset/superset/add/remove: Set is the only
// receiver a value table binds these names on at this arity (an Object's own
// `remove` — dict key deletion, a different signature entirely — resolves
// through MethGate's object arm, which reads the object's own property
// before this gate is ever tested, so the two never collide).
inline constexpr BRecvMask kRecvSet = bmeth_tag_bit(TAG_SET);
// remove: a Set drops a member, an Object drops a dict key — two unrelated
// signatures (Bool vs nil) under one name, so the Object arm is as much a
// receiver of it as the Set arm. MethGate's own read of the receiver's
// property still wins first, which is how a class's own `remove` method (or a
// Shared view's frozen-tree read) shadows both.
inline constexpr BRecvMask kRecvSetOrObject =
    kRecvSet | bmeth_tag_bit(TAG_OBJECT);
// to_array: three unrelated value tables bind it — Set, Tuple, and Tensor
// (the canon tables' method rows), each with
// its own conversion. No Array arm exists (an Array already is one).
inline constexpr BRecvMask kRecvToArray =
    kRecvSet | bmeth_tag_bit(TAG_TUPLE) | bmeth_tag_bit(TAG_TENSOR);
// emit_size_probe's set. `contains` resolves on the same six tags today, but
// the two are spelled out separately so that changing one cannot move the
// other: they are equal by coincidence, not by construction.
inline constexpr BRecvMask kRecvSized =
    kRecvStrLike | kRecvArray | bmeth_tag_bit(TAG_TUPLE) |
    bmeth_tag_bit(TAG_OBJECT) | bmeth_tag_bit(TAG_SET);
inline constexpr BRecvMask kRecvContains =
    kRecvStrLike | kRecvArray | bmeth_tag_bit(TAG_TUPLE) |
    bmeth_tag_bit(TAG_OBJECT) | bmeth_tag_bit(TAG_SET);
inline constexpr BRecvMask kRecvSliceable =
    kRecvStrLike | kRecvArray | bmeth_tag_bit(TAG_TENSOR);
// The Array(+Tensor)-or-iterator-shaped-Object receiver gate join/sum/
// product/min/max/to_set/distinct/flatten all share: an Array (plus Tensor
// for sum/max) resolves the name from its own value table, and an
// iterator-shaped Object resolves it by running the iterator_builtins()
// version on it (a Terminal drains, a Lazy one — distinct/flatten — hands
// back a new iterator instead) — two arms sharing one gate, split by
// obj_iter_shaped the same way `contains` splits its five.
inline constexpr BRecvMask kRecvArrayIter =
    kRecvArray | bmeth_tag_bit(TAG_OBJECT);
inline constexpr BRecvMask kRecvArrayTensorIter =
    kRecvArray | bmeth_tag_bit(TAG_TENSOR) | bmeth_tag_bit(TAG_OBJECT);
// distinct/flatten have no eager Array arm at all (no such spelling exists in
// any value table) — only an iterator-shaped Object resolves them, so an
// Array receiver takes the ordinary method-miss path, same as any name it
// doesn't have.
inline constexpr BRecvMask kRecvIterOnly = bmeth_tag_bit(TAG_OBJECT);
// keys/has/get_or_put: any plain dict, not just an iterator-shaped one —
// unlike kRecvIterOnly's distinct/flatten, ObjectValue::builtins() answers
// for every Object regardless of an own/proto `next`.
inline constexpr BRecvMask kRecvObject = bmeth_tag_bit(TAG_OBJECT);
// get: Array indexes by Long, Object looks up by key — both read-only,
// neither requiring the iterator shape.
inline constexpr BRecvMask kRecvArrayOrObject = kRecvArray | kRecvObject;
// `iter`: five value tables bind it (Object, Array, String, Set, Tuple) and
// each answers a different walker. Unlike the iterator-only names this one
// needs no iterator shape on an Object — every dict has it, and it yields the
// (key, value) pairs unless the object defines an `iter` of its own.
inline constexpr BRecvMask kRecvIterSource =
    kRecvStrLike | kRecvArray | bmeth_tag_bit(TAG_TUPLE) |
    bmeth_tag_bit(TAG_OBJECT) | bmeth_tag_bit(TAG_SET);
// No gate at all: `to_string` is the display conversion every value has, so
// no receiver can fail to resolve it.
// TensorValue::builtins() is the only table binding shape/pow/transpose/
// clone/the autograd five/the activations/reshape/mean/argmax/dot/
// linear_sigmoid/item, so a Tensor is the only receiver that resolves them.
inline constexpr BRecvMask kRecvTensor = bmeth_tag_bit(TAG_TENSOR);
inline constexpr BRecvMask kRecvAny = 0xFFFF;

// A parameter's declared type, checked with the interp binder's wording at the
// argument's own position. `Any` is an undeclared parameter — no check at all,
// so the compiler emits no ChkParam for it. `String` is strict (unlike
// StrLike, a StringView fails it) — `join`'s `sep` is declared plain "String"
// in the interp table, not the StringLike trait.
enum class BParam : uint8_t {
  Any, Long, StrLike, Array, String, Set, Tensor,
  // `Long?` — the reduction axis, optional in the interp signature, so an
  // explicit nil is as good as omitting it.
  LongOpt,
  // The sort family's `reverse:`, the one declared type a keyword-only
  // parameter carries. Strict: `nil` is as wrong as a Long.
  Bool,
};

struct BMethSpec {
  std::string_view name;
  int8_t argc;           // positional arguments written at the call site
  BMeth id;
  BRecvMask recv;
  int8_t nargs;          // arguments the op reads: argc, or argc + 1 with `def`
  const char* def;       // the trailing optional parameter's default literal
  BParam params[2];
  const char* pnames[2];  // the interp's parameter names, for the type error
  // The receivers each check applies to: a polymorphic built-in declares its
  // parameter per arm (`'ab'.contains(x)` wants StringLike; the Array/Set/
  // Tuple/iterator arms take anything). 0 is "every receiver".
  BRecvMask param_when[2];
  // Whether the same-named stdlib global computes the same thing, so the call
  // needs no UFCS fallback to reach it (`to_string` alone).
  bool subsumes_global;
  // Whether an Object receiver must be iterator-shaped to resolve the name at
  // all. `contains` is an iterator-protocol method as well as a container one,
  // so a plain dict simply lacks it (interp's is_iterator_shaped, the JIT's
  // iterator receiver gate — both reduce to an own/proto `next`, since `iter`
  // is a dict builtin every Object "has").
  bool obj_iter_shaped;
  // The keyword-only parameter this built-in declares, or null. It occupies
  // the trailing slot the way `def`'s optional positional does — the call
  // site fills it from the keyword of that name, and the compiler loads the
  // interp's default (`false`, the only one any built-in declares) when the
  // call leaves it out. `builtin_method_accepts_keyword` is the rule this
  // mirrors: a keyword may name a keyword-only parameter and nothing else.
  const char* kw = nullptr;
  // The positional parameter whose declared type is `Function`, if the
  // keyword shares the signature with one. Its check comes first — the binder
  // walks parameters in order — and the sorter's own would be too late.
  int8_t callback_param = -1;
};

inline std::span<const BMethSpec> bmeth_specs() {
  using enum BMeth;
  using enum BParam;
  static constexpr BMethSpec kSpecs[] = {
      {"size", 0, Size, kRecvSized, 0, nullptr, {}, {}},
      {"empty", 0, Empty, kRecvSized, 0, nullptr, {}, {}},
      {"presence", 0, Presence, kRecvSized, 0, nullptr, {}, {}},
      {"upper", 0, Upper, kRecvStrLike, 0, nullptr, {}, {}},
      {"lower", 0, Lower, kRecvStrLike, 0, nullptr, {}, {}},
      {"capitalize", 0, Capitalize, kRecvStrLike, 0, nullptr, {}, {}},
      {"trim", 0, Trim, kRecvStrLike, 0, nullptr, {}, {}},
      {"lines", 0, Lines, kRecvStrLike, 0, nullptr, {}, {}},
      {"view", 0, View, kRecvStrLike, 0, nullptr, {}, {}},
      {"repeat", 1, Repeat, kRecvStrLike, 1, nullptr, {Long}, {"n"}},
      // The optional tail is filled by the compiler with the interp's own
      // default, so the op sees one fixed arity per id.
      {"truncate", 1, Truncate, kRecvStrLike, 2, "...",
       {Long, StrLike}, {"max", "ellipsis"}},
      {"truncate", 2, Truncate, kRecvStrLike, 2, nullptr,
       {Long, StrLike}, {"max", "ellipsis"}},
      {"trim_start", 0, TrimStart, kRecvStrLike, 1, "",
       {StrLike}, {"chars"}},
      {"trim_start", 1, TrimStart, kRecvStrLike, 1, nullptr,
       {StrLike}, {"chars"}},
      {"trim_end", 0, TrimEnd, kRecvStrLike, 1, "", {StrLike}, {"chars"}},
      {"trim_end", 1, TrimEnd, kRecvStrLike, 1, nullptr,
       {StrLike}, {"chars"}},
      {"tr", 2, Tr, kRecvStrLike, 2, nullptr,
       {StrLike, StrLike}, {"from", "to"}},
      {"split", 1, Split, kRecvStrLike, 2, "0", {StrLike, Long},
       {"sep", "limit"}},
      {"starts_with", 1, StartsWith, kRecvStrLike, 1, nullptr,
       {StrLike}, {"prefix"}},
      {"ends_with", 1, EndsWith, kRecvStrLike, 1, nullptr,
       {StrLike}, {"suffix"}},
      {"split", 2, Split, kRecvStrLike, 2, nullptr,
       {StrLike, Long}, {"sep", "limit"}},
      {"rsplit", 1, RSplit, kRecvStrLike, 2, "0", {StrLike, Long},
       {"sep", "limit"}},
      {"rsplit", 2, RSplit, kRecvStrLike, 2, nullptr, {StrLike, Long},
       {"sep", "limit"}},
      {"split_whitespace", 0, SplitWhitespace, kRecvStrLike, 0, nullptr,
       {}, {}},
      {"title", 0, Title, kRecvStrLike, 0, nullptr, {}, {}},
      {"normalize", 0, Normalize, kRecvStrLike, 1, "NFC", {StrLike},
       {"form"}},
      {"normalize", 1, Normalize, kRecvStrLike, 1, nullptr, {StrLike},
       {"form"}},
      {"eq_ignore_case", 1, EqIgnoreCase, kRecvStrLike, 1, nullptr,
       {StrLike}, {"other"}},
      {"index_of", 2, IndexOfFrom, kRecvStrLike, 2, nullptr, {StrLike, Long},
       {"sub", "start"}},
      {"last_index_of", 1, LastIndexOf, kRecvStrLike, 1, nullptr,
       {StrLike}, {"sub"}},
      {"strip_prefix", 1, StripPrefix, kRecvStrLike, 1, nullptr,
       {StrLike}, {"prefix"}},
      {"strip_suffix", 1, StripSuffix, kRecvStrLike, 1, nullptr,
       {StrLike}, {"suffix"}},
      // The is_* family: one selector apart, the order
      // culebra_runtime_str_is_class switches on.
      {"is_digit", 0, IsDigit, kRecvStrLike, 0, nullptr, {}, {}},
      {"is_alpha", 0, IsAlpha, kRecvStrLike, 0, nullptr, {}, {}},
      {"is_alnum", 0, IsAlnum, kRecvStrLike, 0, nullptr, {}, {}},
      {"is_space", 0, IsSpace, kRecvStrLike, 0, nullptr, {}, {}},
      {"is_ascii", 0, IsAscii, kRecvStrLike, 0, nullptr, {}, {}},
      // Array. `push`/`insert` take the value's +1 off the register, so the
      // compiler nils the run before the call (see bmeth_consumes_args);
      // `pop`/`remove_at` hand one back the other way.
      {"push", 1, Push, kRecvArray, 1, nullptr, {Any}, {"arg"}},
      {"pop", 0, Pop, kRecvArray, 0, nullptr, {}, {}},
      {"insert", 2, Insert, kRecvArray, 2, nullptr, {Long, Any}, {"i", "x"}},
      {"remove_at", 1, RemoveAt, kRecvArray, 1, nullptr, {Long}, {"i"}},
      {"extend", 1, Extend, kRecvArray, 1, nullptr, {Array}, {"other"}},
      // Two receivers, two meanings under one name: an Array reverses itself
      // in place and answers nil, a String hands back a fresh reversed one.
      // `index_of` likewise — an Array searches by value equality (Any), a
      // String by substring (StringLike), which is what param_when says.
      {"reverse", 0, Reverse, kRecvArray | kRecvStrLike, 0, nullptr, {}, {}},
      {"index_of", 1, IndexOf, kRecvArray | kRecvStrLike, 2, "0",
       {StrLike, Long}, {"sub", "start"}, {kRecvStrLike, kRecvStrLike}},
      // Polymorphic: one name, several receivers, and — for `contains` — a
      // declared parameter on the String arm alone.
      {"slice", 2, Slice, kRecvSliceable, 2, nullptr, {Long, Long},
       {"start", "end"}},
      {"contains", 1, Contains, kRecvContains, 1, nullptr, {StrLike}, {"sub"},
       {kRecvStrLike}, /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"to_string", 0, ToString, kRecvAny, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/true},
      // join/sum/product/min/max/to_set: Array's own value table plus an
      // iterator-shaped Object (each is Terminal — it drains), Tensor
      // joining only where a reduction method exists for it (sum/max, not
      // product/min — no such spelling in TensorValue::builtins()). `sep`
      // is checked on every receiver alike (param_when 0), since it is not
      // itself polymorphic.
      {"join", 1, Join, kRecvArrayIter, 1, nullptr, {String}, {"sep"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"sum", 0, Sum, kRecvArrayTensorIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"product", 0, Product, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"min", 0, Min, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"max", 0, Max, kRecvArrayTensorIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"to_set", 0, ToSet, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // sorted: Array only — no iterator arm, no Tensor (both probed misses).
      // `reverse:` is the keyword-only parameter, so the op reads one slot
      // more than the call writes positionally: the keyword's value, or the
      // declared default when the call omits it.
      {"sorted", 0, Sorted, kRecvArray, 1, nullptr, {Bool}, {"reverse"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/false, /*kw=*/"reverse"},
      {"distinct", 0, Distinct, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"flatten", 0, Flatten, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // Higher-order: Array(+iterator)-dual, one callback. `Any` param here
      // means no ChkParam — the callback's own type/arity error comes from
      // inside the runtime helper (JitHofCallback), already worded and
      // positioned identically to a hand-written check would be.
      {"map", 1, Map, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"filter", 1, Filter, kRecvArrayIter, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"for_each", 1, ForEach, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"any", 1, AnyOf, kRecvArrayIter, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"all", 1, All, kRecvArrayIter, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"find", 1, Find, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"flat_map", 1, FlatMap, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"min_by", 1, MinBy, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"max_by", 1, MaxBy, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"reduce", 2, Reduce, kRecvArrayIter, 2, nullptr, {Any, Any},
       {"init", "f"}, {}, /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"group_by", 1, GroupBy, kRecvArrayIter, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"partition", 1, Partition, kRecvArrayIter, 1, nullptr, {Any}, {"p"},
       {}, /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // sort_by/sorted_by: Array only (no iterator arm exists). `reverse:`
      // is kw-only in the interp/JIT signature; the spec's kw field is what
      // keeps a keyword call on this arm (resolved_call's spec->kw gate).
      {"sort_by", 1, SortBy, kRecvArray, 2, nullptr, {Any, Bool},
       {"f", "reverse"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/false, /*kw=*/"reverse",
       /*callback_param=*/0},
      {"sorted_by", 1, SortedBy, kRecvArray, 2, nullptr, {Any, Bool},
       {"f", "reverse"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/false, /*kw=*/"reverse",
       /*callback_param=*/0},
      // Set: no polymorphism, so no param_when narrowing — `other` is
      // declared plain "Set" in the interp table, checked on the one
      // receiver that ever reaches here. `add`/`remove`'s element is `Any`
      // (undeclared in the interp table); an unhashable element's error
      // comes from inside the runtime helper's insert/erase, not ChkParam.
      {"union", 1, Union, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"intersect", 1, Intersect, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"diff", 1, Diff, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"sym_diff", 1, SymDiff, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"subset", 1, Subset, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"superset", 1, Superset, kRecvSet, 1, nullptr, {Set}, {"other"}},
      {"add", 1, Add, kRecvSet, 1, nullptr, {Any}, {"x"}},
      {"remove", 1, Remove, kRecvSetOrObject, 1, nullptr, {Any}, {"x"}},
      {"to_array", 0, ToArray, kRecvToArray, 0, nullptr, {}, {}},
      // Object (dict): keys/has/get_or_put resolve on any plain Object, not
      // just an iterator-shaped one — MethGate's Object arm already reads the
      // receiver's own property first, so an own `get_or_put` field (or a
      // Shared.new view, which the interp routes to the same "not a Function"
      // miss) shadows the builtin before this gate is ever tested, same as
      // Set's `add`/`remove`. `has`/`get_or_put`'s key and `get_or_put`'s
      // init are undeclared in the interp table (`Any`) — an unhashable key's
      // error comes from inside the runtime store, not ChkParam.
      {"keys", 0, Keys, kRecvObject, 0, nullptr, {}, {}},
      {"has", 1, Has, kRecvObject, 1, nullptr, {Any}, {"key"}},
      {"get_or_put", 2, GetOrPut, kRecvObject, 2, nullptr, {Any, Any},
       {"key", "init"}},
      // get: Array indexes by Long (interp's own param name is "i" there,
      // not "key" — the two receivers use unrelated interp tables), Object
      // looks up by any key. The index check applies to the Array arm only;
      // Object's key is never type-checked (an unhashable one just misses).
      {"get", 2, Get, kRecvArrayOrObject, 2, nullptr, {Long, Any},
       {"i", "fallback"}, {kRecvArray}},
      // The iterator sources. `iter` walks whichever of the five receivers it
      // is handed; the String walkers take one apiece. `split_iter` is the
      // eager `split` wrapped in an Array walker, so it shares split's `sep`.
      {"iter", 0, Iter, kRecvIterSource, 0, nullptr, {}, {}},
      {"code_points", 0, CodePoints, kRecvStrLike, 0, nullptr, {}, {}},
      {"bytes", 0, Bytes, kRecvStrLike, 0, nullptr, {}, {}},
      {"graphemes", 0, Graphemes, kRecvStrLike, 0, nullptr, {}, {}},
      {"split_iter", 1, SplitIter, kRecvStrLike, 1, nullptr, {StrLike},
       {"sep"}},
      // `count` is the one name two tables bind at DIFFERENT arities — a
      // String counts a substring, an iterator counts what it yields — so the
      // (name, argc) key alone keeps the two apart.
      {"count", 1, StrCount, kRecvStrLike, 1, nullptr, {StrLike}, {"sub"}},
      {"count", 0, IterCount, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // Array-or-iterator duals, like ToSet: `enumerate` normalises both
      // receivers through one runtime entry, to_object/unzip keep an arm each.
      {"enumerate", 0, Enumerate, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"to_object", 0, ToObject, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"unzip", 0, Unzip, kRecvArrayIter, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // Iterator-only. The `n` of take/skip/step_by/chunks/windows/nth is
      // declared Long in the interp table and rejected by its binder, so it
      // takes a ChkParam; a callback is `Any` here for the same reason the
      // higher-order group's is (its own check lives in the runtime helper,
      // already worded and positioned identically), and so is chain/zip's
      // iterable (the "not iterable" error comes from the factory's own
      // coercion, anchored at the call).
      {"take", 1, Take, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"skip", 1, Skip, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"step_by", 1, StepBy, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"chunks", 1, Chunks, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"windows", 1, Windows, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"nth", 1, Nth, kRecvIterOnly, 1, nullptr, {Long}, {"n"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"take_while", 1, TakeWhile, kRecvIterOnly, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"skip_while", 1, SkipWhile, kRecvIterOnly, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"tap", 1, Tap, kRecvIterOnly, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"chunk_by", 1, ChunkBy, kRecvIterOnly, 1, nullptr, {Any}, {"f"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"position", 1, Position, kRecvIterOnly, 1, nullptr, {Any}, {"p"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"scan", 2, Scan, kRecvIterOnly, 2, nullptr, {Any, Any}, {"init", "f"},
       {}, /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"chain", 1, Chain, kRecvIterOnly, 1, nullptr, {Any}, {"other"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"zip", 1, Zip, kRecvIterOnly, 1, nullptr, {Any}, {"other"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"collect", 0, Collect, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"first", 0, First, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      {"last", 0, Last, kRecvIterOnly, 0, nullptr, {}, {}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/true},
      // Tensor-only. Each of these names appears in exactly one built-in
      // table (TensorValue::builtins()), so the gate is the one tag and every
      // other receiver takes the flat resolution answer.
      {"shape", 0, Shape, kRecvTensor, 0, nullptr, {}, {}},
      // `exp` is undeclared in the interp table — a Tensor or any number
      // lifts, anything else is the generic type error from inside the binop.
      {"pow", 1, Pow, kRecvTensor, 1, nullptr, {Any}, {"exp"}},
      {"transpose", 0, Transpose, kRecvTensor, 0, nullptr, {}, {}},
      {"clone", 0, Clone, kRecvTensor, 0, nullptr, {}, {}},
      {"requires_grad", 0, RequiresGrad, kRecvTensor, 0, nullptr, {}, {}},
      {"grad", 0, Grad, kRecvTensor, 0, nullptr, {}, {}},
      {"backward", 0, Backward, kRecvTensor, 0, nullptr, {}, {}},
      {"zero_grad", 0, ZeroGrad, kRecvTensor, 0, nullptr, {}, {}},
      {"detach", 0, Detach, kRecvTensor, 0, nullptr, {}, {}},
      {"relu", 0, Relu, kRecvTensor, 0, nullptr, {}, {}},
      {"sigmoid", 0, Sigmoid, kRecvTensor, 0, nullptr, {}, {}},
      {"softmax", 0, Softmax, kRecvTensor, 0, nullptr, {}, {}},
      {"log", 0, TensorLog, kRecvTensor, 0, nullptr, {}, {}},
      {"reshape", 1, Reshape, kRecvTensor, 1, nullptr, {Array}, {"dims"}},
      // The reductions. `sum` and `max` already have an axis-less row above
      // (Array / Tensor / iterator); `mean` has no such spelling outside
      // Tensor, so both of its rows live here. The axis is `Long?` on the
      // three that may omit it and plain `Long` on argmax, which may not —
      // the interp's own signatures, so the wording follows.
      {"mean", 0, Mean, kRecvTensor, 0, nullptr, {}, {}},
      {"sum", 1, SumAxis, kRecvTensor, 1, nullptr, {LongOpt}, {"axis"}},
      {"mean", 1, MeanAxis, kRecvTensor, 1, nullptr, {LongOpt}, {"axis"}},
      {"max", 1, MaxAxis, kRecvTensor, 1, nullptr, {LongOpt}, {"axis"}},
      {"argmax", 1, Argmax, kRecvTensor, 1, nullptr, {Long}, {"axis"}},
      {"dot", 1, Dot, kRecvTensor, 1, nullptr, {Tensor}, {"other"}},
      {"index_select", 1, IndexSelect, kRecvTensor, 1, nullptr, {Tensor},
       {"indices"}},
      {"linear_sigmoid", 2, LinearSigmoid, kRecvTensor, 2, nullptr,
       {Tensor, Tensor}, {"x", "b"}},
      {"item", 0, Item, kRecvTensor, 0, nullptr, {}, {}},
      // im2col's building blocks: one params Array each ([axis, win, step] /
      // [axis, before, after] / [axis, orig_size, step]), same reason
      // reshape takes a dims Array above — nargs caps at 2.
      {"unfold", 1, Unfold, kRecvTensor, 1, nullptr, {Array}, {"params"}},
      {"pad", 1, Pad, kRecvTensor, 1, nullptr, {Array}, {"params"}},
      {"fold", 1, Fold, kRecvTensor, 1, nullptr, {Array}, {"params"}},
      // General axis reorder — a distinct name from transpose (0-arg,
      // reverses every axis) rather than an overload of it.
      {"permute", 1, Permute, kRecvTensor, 1, nullptr, {Array}, {"axes"}},
      // sort: Array's in-place, nil-returning twin of `sorted`, keyword-only
      // `reverse:` in the same way.
      {"sort", 0, Sort, kRecvArray, 1, nullptr, {Bool}, {"reverse"}, {},
       /*subsumes_global=*/false, /*obj_iter_shaped=*/false, /*kw=*/"reverse"},
      // values: the value-only view of an Object's `iter()`, a dict builtin
      // like `keys` (so a namespace resolves it too).
      {"values", 0, Values, kRecvObject, 0, nullptr, {}, {}},
  };
  return kSpecs;
}

// The static half of the UFCS gate (interp's receiver_has_property): the
// tags whose own built-in table binds this name. Read from the very tables
// the interpreter consults, so the lanes cannot disagree about which
// receiver owns a name. The Object arm is a
// runtime probe and lives in the op itself; `kHasPropIterBit` rides along in
// the same operand to say the name is an iterator-protocol one, which an
// iterator-shaped Object resolves as well.
inline constexpr int32_t kHasPropIterBit = 1 << 16;

inline const BMethSpec* bmeth_lookup(std::string_view name, size_t argc) {
  for (const auto& s : bmeth_specs())
    if (s.name == name && s.argc == static_cast<int8_t>(argc)) return &s;
  return nullptr;
}

// The spec's own position, the name ChkParam carries: the check's type, its
// parameter name and the receivers it applies to all come back out of the
// table, so the instruction holds no copy of any of them.
inline int32_t bmeth_spec_index(const BMethSpec& s) {
  return static_cast<int32_t>(&s - bmeth_specs().data());
}

// Whether `tag` is a receiver the gate lets through.
inline bool bmeth_receiver_ok(BRecvMask recv, int8_t tag) {
  if (recv == kRecvAny) return true;  // no gate: every receiver resolves it
  return tag >= 0 && tag < 16 && (recv & bmeth_tag_bit(tag)) != 0;
}

// The UFCS gate's answer, executor-side (interp's receiver_has_property). `tags` is the operand the compiler baked
// from the very tables those two read — the static half. The Object arm is a
// runtime probe, since own/proto membership needs the pointer.
inline bool has_prop_apply(int32_t tags, const JitValue& recv,
                           const char* key) {
  if (bmeth_receiver_ok(static_cast<BRecvMask>(tags & 0xFFFF),
                        static_cast<int8_t>(recv.tag)))
    return true;
  if (recv.tag != TAG_OBJECT) return false;
  auto* obj = reinterpret_cast<JitObject*>(recv.data);
  // A namespace has a closed member set, so it answers every name itself —
  // as a member, or as eval_property's AttributeError. An own/proto property
  // wins even holding a non-Function, and a conforming trait's default counts
  // as a property too (both inside object_has_or_trait_default).
  if (culebra_runtime_is_namespace(recv.data) ||
      culebra_runtime_object_has_or_trait_default(obj, key))
    return true;
  // An iterator-shaped Object resolves the whole lazy method set through
  // eval_property's duck-typed fallback; `next` is what shapes it — a
  // concrete slot, like interp's is_iterator_shaped reads.
  return (tags & kHasPropIterBit) != 0 &&
         culebra_runtime_object_has(obj, "next");
}

// The name a parameter's declared type carries into the error message.
inline const char* bmeth_param_type(BParam p) {
  switch (p) {
    case BParam::Long: return "Long";
    case BParam::Array: return "Array";
    case BParam::String: return "String";
    case BParam::Set: return "Set";
    case BParam::Tensor: return "Tensor";
    case BParam::LongOpt: return "Long?";
    case BParam::Bool: return "Bool";
    default: return "StringLike";
  }
}

// Whether the built-in takes its arguments' `+1` off the registers.
// The higher-order group, listed once: built-ins whose callback (declared
// LAST — reduce and scan put their seed first) the runtime helper owns from
// entry on every exit, like a JIT AST callsite's hof_owned list. The sort
// family sits outside it: its callback is not the last slot (the
// keyword-only `reverse:` rides behind it, see BMethSpec::callback_param).
inline bool bmeth_is_hof(BMeth id) {
  switch (id) {
    case BMeth::Map: case BMeth::Filter: case BMeth::ForEach:
    case BMeth::AnyOf: case BMeth::All: case BMeth::Find:
    case BMeth::FlatMap: case BMeth::MinBy: case BMeth::MaxBy:
    case BMeth::Reduce: case BMeth::GroupBy: case BMeth::Partition:
    // The iterator adapters that capture a callback (and scan's seed): the
    // factory owns it from entry, exactly as the AST arms' own
    // `f.consume()` before the call hands it over.
    case BMeth::TakeWhile: case BMeth::SkipWhile: case BMeth::Tap:
    case BMeth::ChunkBy: case BMeth::Position: case BMeth::Scan:
      return true;
    default:
      return false;
  }
}

inline bool bmeth_consumes_args(BMeth id) {
  return bmeth_is_hof(id) || id == BMeth::SortBy || id == BMeth::SortedBy ||
         id == BMeth::Push || id == BMeth::Insert || id == BMeth::Add ||
         // has/get/get_or_put: the runtime store takes the key's (and, on a
         // hit or a String key aside, the fallback/init's) `+1` off the
         // registers, mirroring the JIT AST arms' own Owned-consuming
         // discipline (object_has_value / array_get_default /
         // object_get_default / object_get_or_put all consume what they are
         // handed).
         id == BMeth::Has || id == BMeth::Get || id == BMeth::GetOrPut;
}

// The argument index a built-in's callback sits at, or -1 when it takes
// none. A non-Function there reports at that argument, not at the call —
// the runtime helper reads the site this publishes
// (culebra_runtime_set_callback_arg_site), the way the AST arms publish it
// just before handing the callback over.
inline int8_t bmeth_callback_arg(BMeth id, int8_t nargs) {
  if (id == BMeth::SortBy || id == BMeth::SortedBy) return 0;
  return bmeth_is_hof(id) ? static_cast<int8_t>(nargs - 1)
                          : static_cast<int8_t>(-1);
}

// Whether some OTHER row binds the same name — i.e. whether a receiver can
// resolve this name at an arity the call did not write. `count` is the only
// such name today (a String counts a substring, an iterator counts what it
// yields), and it is why a miss cannot always take the flat resolution error.
inline bool bmeth_has_rival_arity(const BMethSpec& s) {
  for (const auto& o : bmeth_specs())
    if (o.name == s.name && o.argc != s.argc) return true;
  return false;
}

// The count-based ArityError this receiver owes: the arities the OTHER rows
// of the name resolve on its tag, if any. Empty when the receiver simply
// lacks the name, which is the flat "expected Function, got Nil" instead.
inline std::string bmeth_rival_arity_message(const BMethSpec& s, int8_t tag,
                                             bool iter_shaped) {
  int8_t lo = -1, hi = -1;
  for (const auto& o : bmeth_specs()) {
    if (o.name != s.name || o.argc == s.argc) continue;
    if (!bmeth_receiver_ok(o.recv, tag)) continue;
    if (o.obj_iter_shaped && tag == TAG_OBJECT && !iter_shaped) continue;
    if (lo < 0 || o.argc < lo) lo = o.argc;
    if (o.argc > hi) hi = o.argc;
  }
  if (lo < 0) return {};
  return culebra::builtin_arity_error_message(s.name, lo, hi, s.argc);
}

// Whether the call publishes its own position before the arguments run.
// A terminal that drives a broken upstream raises the iterator-protocol
// error without a position of its own — the chain was built elsewhere — and
// the interp reports such a throw at the expression it is evaluating, i.e.
// this call: after the receiver gate, before the arguments.
inline bool bmeth_publishes_call_pos(BMeth id) {
  return id == BMeth::Collect || id == BMeth::IterCount ||
         id == BMeth::Take || id == BMeth::Skip || id == BMeth::TakeWhile ||
         id == BMeth::Chunks || id == BMeth::Windows || id == BMeth::Chain ||
         id == BMeth::Zip;
}

// The accepted tags, tested inline like the JIT arms' own gates
// (coerce_strlike_cstr, emit_builtin_long_arg). The test cannot be left to
// culebra_runtime_type_check alone: `StringLike` is a built-in TRAIT, and its
// registry entry comes from the preamble the VM lanes do not splice, so the
// runtime check would reject a perfectly good String there.
//
// One mask drives both lanes — the executor's test below and the lowering's
// branch. Writing the tags out twice is how a new kind ends up silently
// swallowed by the other lane's `default` arm (a String param once read as
// StringLike, a Set param once rejected outright); with the set itself
// shared, the two cannot drift.
inline constexpr BRecvMask bmeth_param_tags(BParam p) {
  switch (p) {
    case BParam::Any: return kRecvAny;
    case BParam::Long: return bmeth_tag_bit(TAG_LONG);
    case BParam::Array: return bmeth_tag_bit(TAG_ARRAY);
    case BParam::StrLike: return kRecvStrLike;
    case BParam::String: return bmeth_tag_bit(TAG_STRING);
    case BParam::Set: return kRecvSet;
    case BParam::Tensor: return kRecvTensor;
    case BParam::LongOpt:
      return bmeth_tag_bit(TAG_LONG) | bmeth_tag_bit(TAG_NIL);
    case BParam::Bool: return bmeth_tag_bit(TAG_BOOL);
  }
  return 0;
}

inline bool bmeth_param_ok(BParam p, int8_t tag) {
  return bmeth_receiver_ok(bmeth_param_tags(p), tag);
}

// Whether a parameter's check covers this receiver. A polymorphic built-in
// declares its parameter per arm, so the same argument is checked on the
// String receiver and waved through on the Array one.
inline bool bmeth_param_applies(const BMethSpec& s, int32_t i, int8_t tag) {
  return s.param_when[i] == 0 || bmeth_receiver_ok(s.param_when[i], tag);
}

// The message a rejected argument carries, in the interp binder's wording.
inline std::string bmeth_param_message(const BMethSpec& s, int32_t i) {
  return culebra::format("type error: parameter '{}' expects {}", s.pnames[i],
                         bmeth_param_type(s.params[i]));
}

// emit_receiver_resolution_error's two halves, executor-side. A scalar
// cannot carry members at all and interp says so at the property read —
// before the arguments run, so MethGate raises it on the spot. Anything else
// merely lacks the method, which is only known once the arguments have run:
// MethGate marks the gate slot and BMeth raises it at the far end.
inline bool bmeth_scalar_tag(int8_t tag) {
  return tag == TAG_NIL || tag == TAG_BOOL || tag == TAG_LONG ||
         tag == TAG_FLOAT;
}

inline void bmeth_scalar_receiver_error(int8_t tag, int64_t line,
                                        int64_t col) {
  culebra_runtime_type_error_typed(line, col, "Object, Array, or Tensor", tag);
}

inline void bmeth_miss_error(int64_t line, int64_t col) {
  culebra_runtime_type_error_typed(line, col, "Function",
                                   static_cast<int8_t>(TAG_NIL));
}

// The gate slot's two sentinels: `{TAG_NO_SELF, 0}` is "the built-in
// answers" (make_no_self), `{TAG_NO_SELF, 1}` is "no receiver resolves this
// name". Anything else is the shadowing user method itself.
inline constexpr int64_t kBMethGateBuiltin = 0;
inline constexpr int64_t kBMethGateMiss = 1;

// emit_size_probe's arms, executor-side. The receiver gate ran first, so
// every tag reaching here has a length.
inline int64_t bmeth_size(const JitValue& v) {
  switch (v.tag) {
    case TAG_ARRAY:
    case TAG_TUPLE:
      return culebra_runtime_array_size(
          reinterpret_cast<JitArray*>(v.data));
    case TAG_OBJECT:
      return culebra_runtime_object_size(
          reinterpret_cast<JitObject*>(v.data));
    case TAG_STRING:
      return culebra_runtime_str_size(reinterpret_cast<const char*>(v.data));
    case TAG_STRINGVIEW:
      return static_cast<int64_t>(
          reinterpret_cast<JitStringView*>(v.data)->len);
    default:
      return culebra_runtime_set_size(reinterpret_cast<JitSet*>(v.data));
  }
}

// The built-in itself: receiver and arguments are already gated and
// type-checked, so each arm is the runtime call the JIT's own arm makes.
// Both operands stay borrowed; the result is a fresh +1.
inline JitValue bmeth_apply(BMeth id, const JitValue& recv,
                            const JitValue* args, int64_t line, int64_t col) {
  auto cstr = [](const JitValue& v) {
    return culebra_runtime_strlike_to_cstr(static_cast<int8_t>(v.tag), v.data);
  };
  auto str = [](const char* s) {
    return JitValue{TAG_STRING, reinterpret_cast<int64_t>(s)};
  };
  // A keyword-only slot: the type check ran before the call, so the value is
  // a Bool (or the compiler's own default) by the time it lands here.
  auto kw_flag = [](const JitValue& v) { return v.data != 0; };
  auto arr = [](const JitValue& v) {
    return reinterpret_cast<JitArray*>(v.data);
  };
  auto st = [](const JitValue& v) { return reinterpret_cast<JitSet*>(v.data); };
  auto ten = [](const JitValue& v) {
    return reinterpret_cast<JitTensor*>(v.data);
  };
  auto obj = [](JitObject* o) {
    return JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(o)};
  };
  switch (id) {
    case BMeth::Size:
      return JitValue{TAG_LONG, bmeth_size(recv)};
    case BMeth::Empty:
      return JitValue{TAG_BOOL, bmeth_size(recv) == 0 ? 1 : 0};
    case BMeth::Presence:
      // The receiver itself when non-empty — a second owner hands it out.
      if (bmeth_size(recv) == 0) return JitValue{TAG_NIL, 0};
      culebra_runtime_value_retain(static_cast<int8_t>(recv.tag), recv.data);
      return recv;
    case BMeth::Upper:
      return str(culebra_runtime_str_upper(cstr(recv)));
    case BMeth::Lower:
      return str(culebra_runtime_str_lower(cstr(recv)));
    case BMeth::Capitalize:
      return str(culebra_runtime_str_capitalize(cstr(recv)));
    case BMeth::Trim:
      return str(culebra_runtime_str_trim(cstr(recv)));
    case BMeth::Lines:
      return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                     culebra_runtime_str_lines(cstr(recv)))};
    case BMeth::View:
      return JitValue{TAG_STRINGVIEW,
                      reinterpret_cast<int64_t>(culebra_runtime_strlike_view(
                          static_cast<int8_t>(recv.tag), recv.data))};
    case BMeth::Repeat:
      return str(culebra_runtime_str_repeat(cstr(recv), args[0].data, line,
                                            col));
    case BMeth::Truncate:
      return str(culebra_runtime_str_truncate(cstr(recv), args[0].data,
                                              cstr(args[1]), line, col));
    case BMeth::TrimStart:
      return str(culebra_runtime_str_trim_start(cstr(recv), cstr(args[0])));
    case BMeth::TrimEnd:
      return str(culebra_runtime_str_trim_end(cstr(recv), cstr(args[0])));
    case BMeth::Tr:
      return str(culebra_runtime_str_tr(cstr(recv), cstr(args[0]),
                                        cstr(args[1])));
    case BMeth::Split:
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(culebra_runtime_str_split(
                          cstr(recv), cstr(args[0]), args[1].data,
                          /*from_right=*/false, line, col))};
    case BMeth::StartsWith:
      return JitValue{TAG_BOOL, culebra_runtime_str_starts_with(
                                    cstr(recv), cstr(args[0])) ? 1 : 0};
    case BMeth::EndsWith:
      return JitValue{TAG_BOOL, culebra_runtime_str_ends_with(
                                    cstr(recv), cstr(args[0])) ? 1 : 0};
    case BMeth::Push:
      culebra_runtime_array_push(arr(recv), static_cast<int8_t>(args[0].tag),
                                 args[0].data);
      return JitValue{TAG_NIL, 0};
    case BMeth::Pop:
    case BMeth::RemoveAt: {
      // The removed element's +1 moves out through the out-params; an empty
      // array pops nil, and a bad index throws from inside remove_at.
      int8_t tag = TAG_NIL;
      int64_t data = 0;
      if (id == BMeth::Pop)
        culebra_runtime_array_pop(arr(recv), &tag, &data);
      else
        culebra_runtime_array_remove_at(arr(recv), args[0].data, &tag, &data,
                                        line, col);
      return JitValue{tag, data};
    }
    case BMeth::Insert:
      culebra_runtime_array_insert(arr(recv), args[0].data,
                                   static_cast<int8_t>(args[1].tag),
                                   args[1].data, line, col);
      return JitValue{TAG_NIL, 0};
    case BMeth::Extend:
      culebra_runtime_array_extend(arr(recv), static_cast<int8_t>(args[0].tag),
                                   args[0].data, line, col);
      return JitValue{TAG_NIL, 0};
    case BMeth::Reverse:
      if (recv.tag != TAG_ARRAY) return str(culebra_runtime_str_reverse(cstr(recv)));
      culebra_runtime_array_reverse(arr(recv));
      return JitValue{TAG_NIL, 0};
    case BMeth::IndexOfFrom:
    case BMeth::IndexOf:
      if (recv.tag != TAG_ARRAY) {
        return JitValue{TAG_LONG,
                        culebra_runtime_str_index_of(
                            cstr(recv), cstr(args[0]), args[1].data)};
      }
      // A too-deep element raises a positionless ValueError from the compare.
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_LONG, culebra_runtime_array_index_of(
                                    arr(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data)};
    case BMeth::Title:
      return str(culebra_runtime_str_title(cstr(recv)));
    case BMeth::Normalize:
      return str(culebra_runtime_str_normalize(cstr(recv), cstr(args[0]), line,
                                               col));
    case BMeth::EqIgnoreCase:
      return JitValue{TAG_BOOL, culebra_runtime_str_eq_ignore_case(
                                    cstr(recv), cstr(args[0])) ? 1 : 0};
    case BMeth::LastIndexOf:
      return JitValue{TAG_LONG, culebra_runtime_str_last_index_of(
                                    cstr(recv), cstr(args[0]))};
    case BMeth::StripPrefix:
      return str(culebra_runtime_str_strip_prefix(cstr(recv), cstr(args[0])));
    case BMeth::StripSuffix:
      return str(culebra_runtime_str_strip_suffix(cstr(recv), cstr(args[0])));
    case BMeth::RSplit:
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(culebra_runtime_str_split(
                          cstr(recv), cstr(args[0]), args[1].data,
                          /*from_right=*/true, line, col))};
    case BMeth::SplitWhitespace:
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_str_split_whitespace(cstr(recv)))};
    case BMeth::IsDigit:
    case BMeth::IsAlpha:
    case BMeth::IsAlnum:
    case BMeth::IsSpace:
    case BMeth::IsAscii:
      return JitValue{TAG_BOOL,
                      culebra_runtime_str_is_class(cstr(recv),
                                                   bmeth_str_class(id))
                          ? 1 : 0};
    // The polymorphic arms dispatch on the receiver the gate let through.
    // String/StringView is the default arm here and in the lowering's switch,
    // so the two lanes read the same way.
    case BMeth::Slice:
      if (recv.tag == TAG_ARRAY)
        return JitValue{TAG_ARRAY,
                        reinterpret_cast<int64_t>(culebra_runtime_array_slice2(
                            arr(recv), args[0].data, args[1].data))};
      if (recv.tag == TAG_TENSOR) {
        // The engine's out-of-bounds IndexError arrives positionless.
        culebra_runtime_set_op_pos(line, col);
        return JitValue{TAG_TENSOR,
                        reinterpret_cast<int64_t>(culebra_runtime_tensor_slice(
                            reinterpret_cast<JitTensor*>(recv.data),
                            args[0].data, args[1].data))};
      }
      return JitValue{
          TAG_STRINGVIEW,
          reinterpret_cast<int64_t>(culebra_runtime_strlike_slice_view(
              static_cast<int8_t>(recv.tag), recv.data, args[0].data,
              args[1].data))};
    case BMeth::Contains: {
      auto found = [&]() -> bool {
        switch (recv.tag) {
          case TAG_ARRAY:
            // A too-deep element raises a positionless ValueError.
            culebra_runtime_set_op_pos(line, col);
            return culebra_runtime_array_contains(
                arr(recv), static_cast<int8_t>(args[0].tag), args[0].data);
          case TAG_SET:
            return culebra_runtime_set_contains(
                reinterpret_cast<JitSet*>(recv.data),
                static_cast<int8_t>(args[0].tag), args[0].data, line, col);
          case TAG_TUPLE:
            culebra_runtime_set_op_pos(line, col);
            return culebra_runtime_tuple_contains(
                       arr(recv), static_cast<int8_t>(args[0].tag),
                       args[0].data) != 0;
          case TAG_OBJECT:
            // The iterator protocol itself: an object that does not carry it
            // fails inside the drive, the same error interp reports.
            culebra_runtime_set_op_pos(line, col);
            return culebra_runtime_iter_contains(
                       static_cast<int8_t>(recv.tag), recv.data,
                       static_cast<int8_t>(args[0].tag), args[0].data) != 0;
          default:
            return culebra_runtime_str_contains(cstr(recv), cstr(args[0]));
        }
      }();
      return JitValue{TAG_BOOL, found ? 1 : 0};
    }
    case BMeth::ToString:
      // The display form of any value — a too-deep one raises positionless.
      culebra_runtime_set_op_pos(line, col);
      return str(culebra_runtime_value_to_display(
          static_cast<int8_t>(recv.tag), recv.data));
    // join/sum/product/min/max/to_set/sorted/distinct/flatten: the gate
    // already let through only Array/Tensor/an iterator-shaped Object, so
    // each arm is a straight dispatch on the tag the gate proved.
    case BMeth::Join:
      // A non-String element's display can raise the too-deep ValueError,
      // positionless like ToString's.
      culebra_runtime_set_op_pos(line, col);
      if (recv.tag == TAG_ARRAY)
        return str(culebra_runtime_array_join(arr(recv), cstr(args[0])));
      return str(culebra_runtime_iter_join(static_cast<int8_t>(recv.tag),
                                           recv.data, cstr(args[0])));
    case BMeth::Sum:
      if (recv.tag == TAG_ARRAY)
        return culebra_runtime_array_sum(arr(recv), line, col);
      if (recv.tag == TAG_TENSOR)
        return culebra_runtime_tensor_reduce_all(
            reinterpret_cast<JitTensor*>(recv.data),
            static_cast<int64_t>(culebra::Op::Sum));
      return culebra_runtime_iter_sum(static_cast<int8_t>(recv.tag), recv.data,
                                      line, col);
    case BMeth::Product:
      if (recv.tag == TAG_ARRAY) return culebra_runtime_array_product(arr(recv), line, col);
      return culebra_runtime_iter_product(static_cast<int8_t>(recv.tag),
                                          recv.data, line, col);
    case BMeth::Min:
      if (recv.tag == TAG_ARRAY) return culebra_runtime_array_min(arr(recv), line, col);
      return culebra_runtime_iter_min(static_cast<int8_t>(recv.tag), recv.data,
                                      line, col);
    case BMeth::Max:
      if (recv.tag == TAG_ARRAY)
        return culebra_runtime_array_max(arr(recv), line, col);
      if (recv.tag == TAG_TENSOR)
        return culebra_runtime_tensor_reduce_all(
            reinterpret_cast<JitTensor*>(recv.data),
            static_cast<int64_t>(culebra::Op::Max));
      return culebra_runtime_iter_max(static_cast<int8_t>(recv.tag), recv.data,
                                      line, col);
    case BMeth::ToSet:
      if (recv.tag == TAG_ARRAY)
        return JitValue{TAG_SET, reinterpret_cast<int64_t>(
                                     culebra_runtime_array_to_set(
                                         arr(recv), line, col))};
      return JitValue{
          TAG_SET,
          reinterpret_cast<int64_t>(culebra_runtime_iter_to_set(
              static_cast<int8_t>(recv.tag), recv.data, line, col))};
    case BMeth::Sorted:
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(culebra_runtime_array_sorted(
                          arr(recv), kw_flag(args[0]), line, col))};
    case BMeth::Distinct:
      // The gate proved TAG_OBJECT (iterator-shaped) — no other tag reaches
      // here.
      return JitValue{
          TAG_OBJECT,
          reinterpret_cast<int64_t>(culebra_runtime_iter_distinct(
              static_cast<int8_t>(recv.tag), recv.data, line, col))};
    case BMeth::Flatten:
      return JitValue{
          TAG_OBJECT,
          reinterpret_cast<int64_t>(culebra_runtime_iter_flatten(
              static_cast<int8_t>(recv.tag), recv.data, line, col))};
    // Higher-order group: the gate already let through only Array or an
    // iterator-shaped Object, so each arm dispatches on the tag the gate
    // proved — the callback itself stays borrowed to the runtime helper,
    // which is its sole owner from the call on (bmeth_consumes_args nils
    // the slot before this runs).
    case BMeth::Map:
      if (recv.tag == TAG_ARRAY)
        return JitValue{TAG_ARRAY,
                        reinterpret_cast<int64_t>(culebra_runtime_array_map(
                            arr(recv), static_cast<int8_t>(args[0].tag),
                            args[0].data, line, col))};
      return JitValue{TAG_OBJECT,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_map(
                          static_cast<int8_t>(recv.tag), recv.data,
                          static_cast<int8_t>(args[0].tag), args[0].data,
                          line, col))};
    case BMeth::Filter:
      if (recv.tag == TAG_ARRAY)
        return JitValue{TAG_ARRAY,
                        reinterpret_cast<int64_t>(culebra_runtime_array_filter(
                            arr(recv), static_cast<int8_t>(args[0].tag),
                            args[0].data, line, col))};
      return JitValue{TAG_OBJECT,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_filter(
                          static_cast<int8_t>(recv.tag), recv.data,
                          static_cast<int8_t>(args[0].tag), args[0].data,
                          line, col))};
    case BMeth::ForEach:
      if (recv.tag == TAG_ARRAY)
        culebra_runtime_array_for_each(arr(recv),
                                       static_cast<int8_t>(args[0].tag),
                                       args[0].data, line, col);
      else
        culebra_runtime_iter_for_each(static_cast<int8_t>(recv.tag),
                                      recv.data,
                                      static_cast<int8_t>(args[0].tag),
                                      args[0].data, line, col);
      return JitValue{TAG_NIL, 0};
    case BMeth::AnyOf:
    case BMeth::All: {
      bool is_any = id == BMeth::AnyOf;
      int64_t r;
      if (recv.tag == TAG_ARRAY)
        r = is_any ? culebra_runtime_array_any(arr(recv),
                                               static_cast<int8_t>(args[0].tag),
                                               args[0].data, line, col)
                  : culebra_runtime_array_all(
                        arr(recv), static_cast<int8_t>(args[0].tag),
                        args[0].data, line, col);
      else
        r = is_any
                ? culebra_runtime_iter_any(
                      static_cast<int8_t>(recv.tag), recv.data,
                      static_cast<int8_t>(args[0].tag), args[0].data, line,
                      col)
                : culebra_runtime_iter_all(
                      static_cast<int8_t>(recv.tag), recv.data,
                      static_cast<int8_t>(args[0].tag), args[0].data, line,
                      col);
      return JitValue{TAG_BOOL, r != 0 ? 1 : 0};
    }
    case BMeth::Find: {
      // The winning element's +1 arrives through out-params; nothing found
      // answers nil.
      int8_t tag = TAG_NIL;
      int64_t data = 0;
      if (recv.tag == TAG_ARRAY)
        culebra_runtime_array_find(arr(recv), static_cast<int8_t>(args[0].tag),
                                   args[0].data, line, col, &tag, &data);
      else
        culebra_runtime_iter_find(static_cast<int8_t>(recv.tag), recv.data,
                                  static_cast<int8_t>(args[0].tag),
                                  args[0].data, line, col, &tag, &data);
      return JitValue{tag, data};
    }
    case BMeth::FlatMap:
      if (recv.tag == TAG_ARRAY)
        return JitValue{
            TAG_ARRAY,
            reinterpret_cast<int64_t>(culebra_runtime_array_flat_map(
                arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
                line, col))};
      return JitValue{TAG_OBJECT,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_flat_map(
                          static_cast<int8_t>(recv.tag), recv.data,
                          static_cast<int8_t>(args[0].tag), args[0].data,
                          line, col))};
    case BMeth::MinBy:
      if (recv.tag == TAG_ARRAY)
        return culebra_runtime_array_min_by(
            arr(recv), static_cast<int8_t>(args[0].tag), args[0].data, line,
            col);
      return culebra_runtime_iter_min_by(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col);
    case BMeth::MaxBy:
      if (recv.tag == TAG_ARRAY)
        return culebra_runtime_array_max_by(
            arr(recv), static_cast<int8_t>(args[0].tag), args[0].data, line,
            col);
      return culebra_runtime_iter_max_by(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col);
    case BMeth::Reduce: {
      int8_t tag = TAG_NIL;
      int64_t data = 0;
      if (recv.tag == TAG_ARRAY)
        culebra_runtime_array_reduce(
            arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
            static_cast<int8_t>(args[1].tag), args[1].data, line, col, &tag,
            &data);
      else
        culebra_runtime_iter_reduce(
            static_cast<int8_t>(recv.tag), recv.data,
            static_cast<int8_t>(args[0].tag), args[0].data,
            static_cast<int8_t>(args[1].tag), args[1].data, line, col, &tag,
            &data);
      return JitValue{tag, data};
    }
    case BMeth::GroupBy:
      if (recv.tag == TAG_ARRAY)
        return JitValue{
            TAG_OBJECT,
            reinterpret_cast<int64_t>(culebra_runtime_array_group_by(
                arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
                line, col))};
      return JitValue{TAG_OBJECT,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_group_by(
                          static_cast<int8_t>(recv.tag), recv.data,
                          static_cast<int8_t>(args[0].tag), args[0].data,
                          line, col))};
    case BMeth::Partition:
      // A Tuple is a JitArray under a different tag (make_tuple's own
      // reading) — the runtime helper already returns the pair that way.
      if (recv.tag == TAG_ARRAY)
        return JitValue{
            TAG_TUPLE,
            reinterpret_cast<int64_t>(culebra_runtime_array_partition(
                arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
                line, col))};
      return JitValue{TAG_TUPLE,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_partition(
                          static_cast<int8_t>(recv.tag), recv.data,
                          static_cast<int8_t>(args[0].tag), args[0].data,
                          line, col))};
    case BMeth::SortBy:
      culebra_runtime_array_sort_by(arr(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data, kw_flag(args[1]), line, col);
      return JitValue{TAG_NIL, 0};
    case BMeth::SortedBy:
      return JitValue{
          TAG_ARRAY,
          reinterpret_cast<int64_t>(culebra_runtime_array_sorted_by(
              arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
              kw_flag(args[1]), line, col))};
    // Set-only: union/intersect/diff/sym_diff/subset/superset borrow both
    // operands (the runtime helper retains fresh copies into a new Set, or
    // just compares), so `args[0]` stays register-owned like Contains'.
    case BMeth::Union:
      return JitValue{TAG_SET, reinterpret_cast<int64_t>(
                                   culebra_runtime_set_union(st(recv),
                                                             st(args[0])))};
    case BMeth::Intersect:
      return JitValue{
          TAG_SET, reinterpret_cast<int64_t>(culebra_runtime_set_intersect(
                       st(recv), st(args[0])))};
    case BMeth::Diff:
      return JitValue{TAG_SET, reinterpret_cast<int64_t>(
                                   culebra_runtime_set_diff(st(recv),
                                                            st(args[0])))};
    case BMeth::SymDiff:
      return JitValue{
          TAG_SET, reinterpret_cast<int64_t>(culebra_runtime_set_sym_diff(
                       st(recv), st(args[0])))};
    case BMeth::Subset:
      return JitValue{TAG_BOOL,
                      culebra_runtime_set_subset(st(recv), st(args[0]))};
    case BMeth::Superset:
      return JitValue{TAG_BOOL,
                      culebra_runtime_set_superset(st(recv), st(args[0]))};
    // `add` absorbs the argument's `+1` (bmeth_consumes_args); `remove` only
    // hashes it for lookup, same as Contains.
    case BMeth::Add:
      return JitValue{TAG_BOOL, culebra_runtime_set_add_method(
                                    st(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data, line, col)};
    case BMeth::Remove:
      if (recv.tag == TAG_OBJECT) {
        // Dict key deletion, a different signature under the same name: it
        // answers nil, and the store takes the key's `+1`. The argument stays
        // register-owned like the Set arm's, so mint the reference the store
        // consumes rather than handing over the slot's. (A Shared view never
        // reaches here — MethGate routes its every built-in name to the user
        // arm — so the helper's own release-then-reject path is unreachable.)
        culebra_runtime_value_retain(static_cast<int8_t>(args[0].tag),
                                     args[0].data);
        culebra_runtime_object_remove_any(
            reinterpret_cast<JitObject*>(recv.data),
            static_cast<int8_t>(args[0].tag), args[0].data, line, col);
        return JitValue{TAG_NIL, 0};
      }
      return JitValue{TAG_BOOL, culebra_runtime_set_remove(
                                    st(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data, line, col)};
    // Three unrelated value tables bind this name — Set, Tuple, Tensor.
    case BMeth::ToArray:
      switch (recv.tag) {
        case TAG_TUPLE:
          return JitValue{TAG_ARRAY,
                          reinterpret_cast<int64_t>(
                              culebra_runtime_tuple_to_array(arr(recv)))};
        case TAG_TENSOR:
          // Above rank 2 the conversion raises positionless.
          culebra_runtime_set_op_pos(line, col);
          return JitValue{
              TAG_ARRAY,
              reinterpret_cast<int64_t>(culebra_runtime_tensor_to_array(
                  reinterpret_cast<JitTensor*>(recv.data)))};
        default:  // TAG_SET, the gate's remaining case
          return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                         culebra_runtime_set_to_array(
                                             st(recv)))};
      }
    case BMeth::Keys:
      return JitValue{
          TAG_ARRAY,
          reinterpret_cast<int64_t>(culebra_runtime_object_keys(
              reinterpret_cast<JitObject*>(recv.data)))};
    case BMeth::Has:
      return JitValue{
          TAG_BOOL,
          culebra_runtime_object_has_value(
              reinterpret_cast<JitObject*>(recv.data),
              static_cast<int8_t>(args[0].tag), args[0].data) ? 1 : 0};
    // Array indexes by Long, Object looks up by key — the gate proved the
    // tag is one of the two.
    case BMeth::Get:
      if (recv.tag == TAG_ARRAY)
        return culebra_runtime_array_get_default(
            arr(recv), args[0].data, static_cast<int8_t>(args[1].tag),
            args[1].data);
      return culebra_runtime_object_get_default(
          reinterpret_cast<JitObject*>(recv.data),
          static_cast<int8_t>(args[0].tag), args[0].data,
          static_cast<int8_t>(args[1].tag), args[1].data, line, col);
    case BMeth::GetOrPut:
      return culebra_runtime_object_get_or_put(
          reinterpret_cast<JitObject*>(recv.data),
          static_cast<int8_t>(args[0].tag), args[0].data,
          static_cast<int8_t>(args[1].tag), args[1].data, line, col);
    // --- The iterator sources ---------------------------------------------
    case BMeth::Iter:
      switch (recv.tag) {
        case TAG_ARRAY:
        case TAG_TUPLE:  // a Tuple is a JitArray under another tag
          return obj(culebra_runtime_array_iter(arr(recv)));
        case TAG_SET:
          return obj(culebra_runtime_set_iter(st(recv)));
        case TAG_OBJECT:
          // A Range walks its start..end sequence — split it out before the
          // generic Object path, which would walk the Range object's own
          // key/value pairs.
          if (culebra_runtime_is_range(static_cast<int8_t>(recv.tag),
                                       recv.data))
            return obj(culebra_runtime_range_iter(recv.data, line, col));
          // An `iter` of the receiver's own is what MethGate would have found;
          // reaching here means the dict builtin, which walks (key, value) —
          // the dispatch still asks, matching the AST arm's own call.
          return obj(culebra_runtime_object_iter_dispatch(
              reinterpret_cast<JitObject*>(recv.data)));
        default:  // TAG_STRING / TAG_STRINGVIEW, the gate's remaining cases
          return obj(culebra_runtime_str_scalars(cstr(recv)));
      }
    case BMeth::CodePoints:
      return obj(culebra_runtime_str_code_points(cstr(recv)));
    case BMeth::Bytes:
      return obj(culebra_runtime_str_bytes(cstr(recv)));
    case BMeth::Graphemes:
      return obj(culebra_runtime_str_graphemes(cstr(recv)));
    case BMeth::SplitIter: {
      // "Lazy in API, eager underneath": split first, then walk the pieces.
      // array_iter takes its own `+1` on the Array, so split's fresh one is
      // released here and the snapshot lives exactly as long as the iterator.
      auto* pieces = culebra_runtime_str_split(cstr(recv), cstr(args[0]),
                                              /*limit=*/0,
                                              /*from_right=*/false, line, col);
      auto* it = culebra_runtime_array_iter(pieces);
      culebra_runtime_value_release(TAG_ARRAY,
                                    reinterpret_cast<int64_t>(pieces));
      return obj(it);
    }
    case BMeth::StrCount:
      return JitValue{TAG_LONG,
                      culebra_runtime_str_count(cstr(recv), cstr(args[0]))};
    case BMeth::Enumerate:
      // One runtime entry for both receivers: it normalises an Array into a
      // walker itself, then yields `(index, value)` tuples.
      return obj(culebra_runtime_enumerate_any(static_cast<int8_t>(recv.tag),
                                               recv.data));
    case BMeth::ToObject:
      if (recv.tag == TAG_ARRAY)
        return obj(culebra_runtime_array_to_object(arr(recv), line, col));
      culebra_runtime_set_op_pos(line, col);  // the drive's protocol error
      return obj(culebra_runtime_iter_to_object(static_cast<int8_t>(recv.tag),
                                                recv.data, line, col));
    case BMeth::Unzip:
      if (recv.tag == TAG_ARRAY)
        return JitValue{TAG_TUPLE,
                        reinterpret_cast<int64_t>(
                            culebra_runtime_array_unzip(arr(recv), line, col))};
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TUPLE,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_unzip(
                          static_cast<int8_t>(recv.tag), recv.data, line,
                          col))};
    // --- Iterator-only: the gate proved an iterator-shaped Object ----------
    case BMeth::Take:
      return obj(culebra_runtime_iter_take(static_cast<int8_t>(recv.tag),
                                           recv.data, args[0].data));
    case BMeth::Skip:
      return obj(culebra_runtime_iter_skip(static_cast<int8_t>(recv.tag),
                                           recv.data, args[0].data));
    case BMeth::StepBy:
      return obj(culebra_runtime_iter_step_by(static_cast<int8_t>(recv.tag),
                                              recv.data, args[0].data, line,
                                              col));
    case BMeth::Chunks:
      return obj(culebra_runtime_iter_chunks(static_cast<int8_t>(recv.tag),
                                             recv.data, args[0].data, line,
                                             col));
    case BMeth::Windows:
      return obj(culebra_runtime_iter_windows(static_cast<int8_t>(recv.tag),
                                              recv.data, args[0].data, line,
                                              col));
    case BMeth::TakeWhile:
      return obj(culebra_runtime_iter_take_while(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::SkipWhile:
      return obj(culebra_runtime_iter_skip_while(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::Tap:
      return obj(culebra_runtime_iter_tap(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::ChunkBy:
      return obj(culebra_runtime_iter_chunk_by(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::Scan:
      return obj(culebra_runtime_iter_scan(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data,
          static_cast<int8_t>(args[1].tag), args[1].data, line, col));
    case BMeth::Chain:
      return obj(culebra_runtime_iter_chain(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::Zip:
      return obj(culebra_runtime_iter_zip(
          static_cast<int8_t>(recv.tag), recv.data,
          static_cast<int8_t>(args[0].tag), args[0].data, line, col));
    case BMeth::Collect:
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(culebra_runtime_iter_collect(
                          static_cast<int8_t>(recv.tag), recv.data))};
    case BMeth::IterCount:
      return JitValue{TAG_LONG,
                      culebra_runtime_iter_count(static_cast<int8_t>(recv.tag),
                                                 recv.data)};
    case BMeth::First:
    case BMeth::Last:
    case BMeth::Nth:
    case BMeth::Position: {
      // An exhausted iterator answers nil, so the element's `+1` arrives
      // through out-params like Find's.
      int8_t tag = TAG_NIL;
      int64_t data = 0;
      auto it = static_cast<int8_t>(recv.tag);
      if (id == BMeth::First)
        culebra_runtime_iter_first(it, recv.data, &tag, &data);
      else if (id == BMeth::Last)
        culebra_runtime_iter_last(it, recv.data, &tag, &data);
      else if (id == BMeth::Nth)
        culebra_runtime_iter_nth(it, recv.data, args[0].data, line, col, &tag,
                                 &data);
      else
        culebra_runtime_iter_position(it, recv.data,
                                      static_cast<int8_t>(args[0].tag),
                                      args[0].data, line, col, &tag, &data);
      return JitValue{tag, data};
    }
    // Tensor-only. The gate proved TAG_TENSOR, so every arm reads the
    // receiver as one. Each helper that can raise does so positionless (the
    // engine has no notion of source), hence the stamp before the call.
    case BMeth::Shape:
      return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                     culebra_runtime_tensor_shape(ten(recv)))};
    case BMeth::Pow:
      culebra_runtime_set_op_pos(line, col);
      return JitValue{
          TAG_TENSOR,
          reinterpret_cast<int64_t>(culebra_runtime_tensor_binop(
              static_cast<int8_t>(recv.tag), recv.data,
              static_cast<int8_t>(args[0].tag), args[0].data,
              static_cast<int64_t>(culebra::Op::Pow)))};
    case BMeth::Transpose:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_tensor_transpose(ten(recv)))};
    case BMeth::Clone:
      return JitValue{TAG_TENSOR, reinterpret_cast<int64_t>(
                                      culebra_runtime_tensor_clone(ten(recv)))};
    case BMeth::RequiresGrad:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_tensor_requires_grad(ten(recv)))};
    case BMeth::Grad:
      return JitValue{TAG_TENSOR, reinterpret_cast<int64_t>(
                                      culebra_runtime_tensor_grad(ten(recv)))};
    case BMeth::Backward:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_tensor_backward(ten(recv)))};
    case BMeth::ZeroGrad:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_tensor_zero_grad(ten(recv)))};
    case BMeth::Detach:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(
                          culebra_runtime_tensor_detach(ten(recv)))};
    case BMeth::Relu:
    case BMeth::Sigmoid:
    case BMeth::Softmax:
    case BMeth::TensorLog:
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_unary(
                          ten(recv), bmeth_unary_op(id)))};
    case BMeth::Reshape:
      // A bad element type, a count mismatch and a negative dim all arrive
      // positionless.
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_reshape(
                          ten(recv), arr(args[0])))};
    case BMeth::Mean:
      return culebra_runtime_tensor_reduce_all(
          ten(recv), static_cast<int64_t>(culebra::Op::Mean));
    case BMeth::SumAxis:
    case BMeth::MeanAxis:
    case BMeth::MaxAxis:
    case BMeth::Argmax:
      // The axis is `Long?` on all but argmax, so an explicit nil means the
      // axis-less reduction — the same reading the interp's binder gives it.
      if (args[0].tag == TAG_NIL)
        return culebra_runtime_tensor_reduce_all(ten(recv),
                                                 bmeth_reduce_op(id));
      culebra_runtime_set_op_pos(line, col);  // out-of-range axis
      return JitValue{
          TAG_TENSOR,
          reinterpret_cast<int64_t>(culebra_runtime_tensor_reduce_axis(
              ten(recv), bmeth_reduce_op(id), args[0].data))};
    case BMeth::Dot:
      culebra_runtime_set_op_pos(line, col);  // rank check
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_dot(
                          ten(recv), ten(args[0])))};
    case BMeth::IndexSelect:
      culebra_runtime_set_op_pos(line, col);
      return JitValue{
          TAG_TENSOR,
          reinterpret_cast<int64_t>(culebra_runtime_tensor_index_select(
              ten(recv), ten(args[0])))};
    case BMeth::LinearSigmoid:
      culebra_runtime_set_op_pos(line, col);
      return JitValue{
          TAG_TENSOR,
          reinterpret_cast<int64_t>(culebra_runtime_tensor_linear_sigmoid(
              ten(recv), ten(args[0]), ten(args[1])))};
    case BMeth::Item:
      culebra_runtime_set_op_pos(line, col);  // multi-element check
      return culebra_runtime_tensor_item(ten(recv));
    case BMeth::Unfold:
      // A bad param count/type, a negative axis out of range, or a bad
      // win/step all arrive positionless.
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_unfold(
                          ten(recv), arr(args[0])))};
    case BMeth::Pad:
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_pad(
                          ten(recv), arr(args[0])))};
    case BMeth::Fold:
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_fold(
                          ten(recv), arr(args[0])))};
    case BMeth::Permute:
      // A bad axes count/type, or axes that aren't a genuine permutation,
      // arrive positionless.
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_TENSOR,
                      reinterpret_cast<int64_t>(culebra_runtime_tensor_permute(
                          ten(recv), arr(args[0])))};
    case BMeth::Sort:
      // In place, answering nil.
      culebra_runtime_array_sort(arr(recv), kw_flag(args[0]), line, col);
      return JitValue{TAG_NIL, 0};
    case BMeth::Values:
      return JitValue{TAG_OBJECT,
                      reinterpret_cast<int64_t>(culebra_runtime_object_values(
                          reinterpret_cast<JitObject*>(recv.data)))};
  }
  return JitValue{TAG_NIL, 0};  // unreachable: every id has an arm
}

// Run-length position side table: entry covers [first_insn, next.first_insn).
// This is what makes error positions structural instead of hand-threaded.
struct PosEntry {
  uint32_t first_insn;
  uint32_t line, col;
};

struct Chunk {
  std::vector<Insn> code;
  std::vector<JitValue> consts;  // scalars, or TAG_STRING into str_arena
  // Owns the string constants' bytes in the runtime string layout
  // ({i64 len} then NUL-terminated bytes; TAG_STRING data points at the
  // bytes, len at data[-8]) — the same shape the JIT bakes into .rodata,
  // so every helper reads them identically. Strings are not refcounted
  // (the conservative GC owns heap strings); these live as long as the
  // chunk, like the JIT module's globals.
  std::vector<std::unique_ptr<char[]>> str_arena;
  // One entry per Op::ObjectNewShaped site (compile_object's eligibility
  // check — every key a plain IDENTIFIER, dedup'd in first-occurrence
  // order). `keys` is baked once at compile time (raw pointers into
  // `str_arena`, stable for the chunk's lifetime); `shape` starts null and
  // is resolved on the site's first execution, then reused forever — every
  // execution of the same literal walks the identical name sequence, so the
  // Shape it resolves to is always the same canonical pointer
  // (ShapeRegistry::transition_add's own lock+cache already makes that
  // resolution idempotent), the same "benign race, no atomics needed"
  // argument the property IC (JitPropIC) already relies on. A Shape* is a
  // per-PROCESS pointer, not a per-program one — the JIT lowering must
  // resolve it the same lazy way, never bake it at AOT-compile time.
  // `mutable`: the executor reaches this through a `const Chunk&` (the
  // bytecode is logically immutable during a run), but the cache is a
  // physical detail of execution, not part of the compiled program.
  struct ObjectShapeSpec {
    mutable void* shape = nullptr;
    std::vector<const char*> keys;
  };
  std::vector<ObjectShapeSpec> object_shape_specs;
  int32_t num_slots = 0;
  // Cell slots in creation order: a slot's rank, against a cleanup step's
  // `cells_before`, says whether it already owned a cell at that point in the
  // code. The plain "is this slot a cell" question has no chunk-wide answer —
  // an index can be a temporary early on and a captured binding's cell later,
  // and the ladders' Release / CellRelease choice differs between the two.
  static constexpr uint32_t kNotACell = 0xffffffffu;
  std::vector<uint32_t> slot_cell_rank;
  // Declaration order of each slot's binding: the release ladders walk a
  // scope's slots newest-declaration first, which is slot order everywhere
  // except a forward-ref pre-declaration (its cell is minted at the head of
  // the statement list, its binding lands later).
  std::vector<uint32_t> slot_rank;
  std::vector<PosEntry> positions;
  std::vector<std::string> slot_names;  // debug table, always emitted
  // One binding's live range, which is what a debugger's scope enumeration
  // asks for: the names a frame paused at pc can see, and where their values
  // are. A slot alone cannot answer it — the same index is a temporary in one
  // scope and a binding in the next — so each binding records the instruction
  // its declaration landed at and the one its scope closed at. Emitted only
  // for a unit compiled for debugging (see UnitOpts::debug).
  struct SlotDebug {
    int32_t slot;
    uint32_t start, end;  // [start, end): where the name is in scope
    bool is_mut;
    bool is_cell;  // the slot holds the cell, not the value
    std::string name;
  };
  std::vector<SlotDebug> slot_debug;
  // Does this chunk carry statement boundaries? A frame whose chunk has
  // none is not user code (a constructor thunk, a stdlib module's body):
  // it has no line to show and nowhere to stop, so a call stack folds it
  // away the way the interpreter's folds its internal delegations.
  bool has_dbg = false;
  // Function-chunk metadata (chunk 0 — the module top level — has none).
  // Params occupy slots [0, arity); `fn_slot` holds the frame's own closure
  // when the body reads it — as the `fn` recursion handle
  // (FuncInfo::uses_fn) or as the `let` name a literal initializes
  // (FuncInfo::own_name) — and is -1 otherwise.
  int32_t arity = 0;
  std::vector<std::string> param_names;  // for the ArityError message
  // Effective parameter types (empty = untyped), parallel to param_names —
  // the overload signature MultifnReg registers. Dispatch reads them; the
  // per-entry checks are ChkArg / ChkTypeAt instructions. A Generic class's
  // type params are neutralized to Any here, so `param_declared_types` keeps
  // the annotation as written for `f.params[i].type` to report.
  std::vector<std::string> param_types;
  std::vector<std::string> param_declared_types;
  // Parameters that must be supplied: the leading run without a default
  // (defaults are trailing). The prologue's arity guard counts against this
  // and leaves each unsupplied slot TAG_UNFILLED for JumpIfFilled.
  int32_t required = 0;
  // Which parameters carry a default, parallel to param_names: the resolver
  // needs it to tell an unfilled middle slot (defaulted, so TAG_UNFILLED)
  // from a missing required one. `**rest` counts as defaulted — its default
  // is the empty Object the prologue builds.
  std::vector<uint8_t> param_has_default;
  // The `**rest` catch-all's ABI index, and the first keyword-only slot's
  // (after a `*` separator), or -1. Both are what a keyword call resolves
  // names against — see the JitParamMeta the program builds per chunk.
  int32_t kwargs_rest_idx = -1;
  int32_t first_kw_only_idx = -1;
  // Positional callback-arity bounds (cb_max = -1 when a `*args` catch-all
  // removes the upper bound), the same pair the JIT hands the HOF gate.
  int32_t cb_min = 0;
  int32_t cb_max = 0;
  // Whether the body reaches the overflow arguments (`__ARGS__`, or a named
  // `*args`): the prologue keeps their `+1`s for the Array it builds instead
  // of releasing them.
  bool keeps_args = false;
  // Whether a `*args` catch-all removes this overload's upper arity bound.
  bool variadic = false;
  int32_t fn_slot = -1;
  // Where a receiver frame caches the receiver-bound wrapper a value-read of
  // `fn` yields (-1 without a receiver, where the raw closure is the answer).
  // Owned, so the frame ladder releases the single +1 the cache holds.
  int32_t fn_bound_slot = -1;
  // Source name of an overload body (`fn name`, a class member, `new`), read
  // by MultifnReg: it is the user-facing half of the registry key, so a
  // DispatchError names the declaration rather than the internal id. It is
  // also what `f.name` reports, so a plain `fn f` carries it too.
  std::string multifn_name;
  // What `f.params[i].mut` and `f.return_type` report, parallel to
  // param_names for the first (a declared type is param_types').
  std::vector<uint8_t> param_mut;
  std::string return_type;
  // Receiver frames (a class method, a `new` body, the synthetic field-init
  // thunk) bind the JitFn ABI's receiver here; -1 means the frame takes no
  // receiver, and the incoming +1 is released on entry (the JIT's ctor thunk
  // does the same with the class object it is called on). A frame that
  // captured an enclosing `self` also takes the slot, but raw: SelfMerge
  // needs to see the NO_SELF sentinel to know a plain call from a receiver
  // call, so the prologue skips the nil rewrite there.
  int32_t self_slot = -1;
  bool self_raw = false;
  // A getter body (`get x() { … }`). The runtime's getter registry is keyed
  // by a closure's fn_ptr, and every executor closure shares one interpreter
  // entry point — so a getter chunk's closures get their own (see
  // Exec::getter_trampoline), which is what makes the key mean "this code is
  // a getter" in that lane too.
  bool is_getter = false;
  // Method-name tables for ClassMeta, indexed by its `d` operand. Stable
  // storage: the executor hands build_class_meta an array of these c_str()s.
  std::vector<std::vector<std::string>> name_tables;
  // Parallel to name_tables: whether that class is a lowering's state class,
  // whose own slots are promoted body locals rather than methods (the flag
  // build_class_meta stores on the meta, and the receiver rule reads back).
  std::vector<uint8_t> name_table_flags;
  // Capture list: for each free var, the slot (in the CREATING frame's
  // numbering) holding its cell pointer. A fn literal has exactly one
  // creation site — its MakeClosure — so the list lives with the callee
  // chunk instead of being encoded into the instruction.
  std::vector<int32_t> capture_src_slots;
  // Names of the `mut` bindings this chunk closes over, in capture order.
  // `Isolate.spawn` rejects a closure that captures one (the child's copy
  // would silently diverge from the parent's), and the check needs the
  // name for its message. The lowering keys the same fact by fn_ptr; the
  // executor's closures all share one, so the chunk carries it instead.
  std::vector<std::string> mut_capture_names;
  // Does this frame count itself against the recursion limit? Every body
  // does, in its prologue; a constructor thunk forwards its arguments and
  // has no prologue, so it must not uncount on the way out either. It did,
  // and the drift was observable: one `C.new()` left the counter at -1, so
  // a method chain after it got one frame more than the other backends.
  bool counts_frame = false;
  // One lexical scope's unwind step. A throw at pc runs, from the innermost
  // scope containing it outward: that scope's pending defers, then its own
  // named slots in reverse declaration order — the interpreter's per-scope
  // unwind, which the JIT emits as one cleanup pad per region
  // (finish_scope_cleanup). Chaining the steps rather than releasing the
  // whole frame at once is what keeps `defer` and `drop` interleaved the way
  // the other two backends interleave them.
  //
  // A try body's scope carries `handler`: reaching it ends the unwind, with
  // the caught value in `caught_slot` (written by the dispatcher / the
  // lowered landingpad prelude before the jump). Entries are recorded as
  // scopes close, hence innermost-first: the first one containing pc is the
  // innermost, and `parent` walks outward until -1 ends at the frame.
  // Releasing a range is sound even where an inner step already emptied part
  // of it — Release/CellRelease are destructive and nil-safe, and under the
  // borrow operand contract a throw leaves every register frame-owned.
  static constexpr uint32_t kNoHandler = 0xffffffffu;
  struct Cleanup {
    uint32_t start, end;
    int32_t parent;
    int32_t defer_mark_slot;   // -1: the scope declares no defer
    int32_t slot_lo, slot_hi;  // its named slots, released hi-1 down to lo
    uint32_t cells_before;     // cells that existed here (see slot_cell_rank)
    uint32_t handler = kNoHandler;
    int32_t caught_slot = -1;
    // A for-in's cursor base when this step is that loop's own scope: the
    // ladder closes the iterator as it reaches `dispose_base + kForIter`,
    // so the iteration's bindings — released by the inner steps already
    // walked — die before it, as they do on every other exit.
    int32_t dispose_base = -1;
  };
  std::vector<Cleanup> cleanups;
  // The statement temporaries live at each pc, delta-coded (an entry only
  // where the set changes; the last one at or before the faulting pc
  // applies). They hold the in-flight `+1`s of the expression that threw and
  // die before the innermost scope's defers — the JIT's release_unwind_temps,
  // whose pool this table stands in for.
  struct TempPoint {
    uint32_t pc, off, len;
  };
  std::vector<TempPoint> temp_points;
  std::vector<int32_t> temp_slots;  // flat backing for TempPoint
  // Program exit releases the top level's own bindings without firing their
  // `drop` (docs §17, and the JIT's suppressed __culebra_main exit): true for
  // chunk 0 only, read by the frame step of both the normal and throw paths.
  bool suppress_frame_drop = false;
  // The frame's own entry in the mark array (-1 for chunk 0, whose exit
  // drops nothing), and how many entries the array needs. `return` and the
  // unwind's frame step resolve from the frame's.
  int32_t owned_frame_depth = -1;
  int32_t owned_depths = 0;
  // Frame-level defer mark slot (-1 when the chunk has no defers). Taken by
  // the chunk's first instruction; a throw that no region catches runs the
  // frame's pending defers back to it before unwinding out (Exec::run_frame's
  // catch-all, the lowering's frame cleanup pad) — the observable slice of
  // the JIT's frame cleanup ladder. Slot releases still fall to the GC
  // backstop on that path (the known Phase 1 residual).
  int32_t defer_mark_slot = -1;
  // The synthetic constructor chunk hands its ABI arguments to
  // build_class_instance untouched (the JIT ctor thunk's shape), so the
  // prologue neither copies them into slots nor releases the surplus — the
  // `new` body it invokes owns both.
  bool forwards_args = false;
  // Per-call argument positions (packed line<<32|col, in argument order),
  // keyed by the Call / CallM instruction's index and kept sorted by it.
  // The JIT bakes the same array into .rodata and hands it to
  // culebra_runtime_set_call_positions at the call site, which is what
  // lets a callee's typed-parameter error report at the argument
  // expression rather than at the call. Entries only exist for calls that
  // pass arguments.
  std::vector<std::pair<uint32_t, std::vector<int64_t>>> call_argpos;
  // The function chunk each Call / CallM was resolved to, indexed by the
  // instruction, -1 where the callee stays a run-time question. A resolved
  // callee is a name bound once, by a `fn` literal, and never rebound. Only
  // the CODE is static — the closure still rides the register, since its
  // captures are the caller's business — so a resolved site skips the
  // Function gate, answers check_pos_count_cls from the chunk's own
  // signature, and reaches the body without the fn_ptr indirection: the
  // executor enters the frame directly and the lowering emits a direct
  // call. Indexed rather than a sorted side table like call_argpos: the
  // executor's dispatch loop asks once per call, and the answer has to
  // cost less than what it saves. Empty when nothing resolved.
  //
  // `reach` says how the chunk relates to the value in the register, which
  // is what decides whether the site owes a run-time question before it may
  // enter. The three are alternatives, not flags — a site has exactly one
  // shape — and they ride two tag bits of the stored word rather than
  // widening the row: this is the one side table the dispatch loop reads on
  // every call.
  //
  // `callee_in_cell` is the one fact that is not about the code: the `b`
  // operand names a slot holding a CELL, and the callee is the value inside
  // it. The compiler emits that for a name bound once into a cell this frame
  // owns, where the read needs neither a register of its own nor the `+1` a
  // register would have to pay for — the cell's own reference outlives the
  // call (see borrowed_call_head). It survives a revocation, since it
  // describes the instruction's operand rather than the analysis.
  enum class Reach : uint8_t {
    // The callee IS that chunk's closure, and the compiler owes no check —
    // run_resolved's assert is the whole verification.
    Direct = 0,
    // The callee is a `fn name` dispatcher, so the code is static but the
    // closure to enter is the dispatcher's monomorphic body rather than the
    // register's value. Nil there means the shortcut is gone.
    Mono = 1,
    // The chunk is the answer only if the callee really is that chunk's
    // closure: a class member reads its own class name off the RECEIVER
    // (Op::ClsSelf), which is this class only when the receiver is one of
    // its instances — a method value moved onto a foreign object is not.
    Guarded = 2,
  };
  struct CallTarget {
    int32_t chunk = -1;
    Reach reach = Reach::Direct;
    bool callee_in_cell = false;
  };
  std::vector<int32_t> call_targets;
  // One per CallKw: how its register run splits into positionals, keyword
  // values and `**` operands, plus the keyword names the resolver binds them
  // by — interned into this chunk's str_arena, so the executor hands the
  // array straight to the runtime resolver.
  struct KwCall {
    bool has_receiver;
    int32_t n_pos, n_kw, n_splat;
    std::vector<const char*> kw_keys;
  };
  std::vector<KwCall> kwcalls;
  // One BArity check: the receivers that resolve the name, and what each owes.
  // `tag` is a receiver tag, or kArityObj for any Object (the dict table) /
  // kArityIter for an iterator-shaped one — the order eval_property resolves
  // in, so at most one of the two ever applies.
  static constexpr int8_t kArityObj = -1;
  static constexpr int8_t kArityIter = -2;
  struct ArityArm {
    int8_t tag;
    int32_t kind_k, msg_k, name_k;  // constant-pool indices
    uint32_t line, col;
  };
  std::vector<std::vector<ArityArm>> arity_checks;

  // Take another chunk's signature as this one's: everything a caller binds
  // against, and nothing about the frame that binds it. The synthetic
  // constructor uses it to speak for the `new` body it forwards to.
  void adopt_signature(const Chunk& src) {
    arity = src.arity;
    required = src.required;
    param_names = src.param_names;
    param_types = src.param_types;
    param_has_default = src.param_has_default;
    kwargs_rest_idx = src.kwargs_rest_idx;
    first_kw_only_idx = src.first_kw_only_idx;
    cb_min = src.cb_min;
    cb_max = src.cb_max;
    variadic = src.variadic;
  }
};

// The argument positions the call at `ix` published, or null when it has
// none (the call site stands in — the interp binder's own fallback).
inline const std::vector<int64_t>* chunk_argpos_at(const Chunk& c, size_t ix) {
  auto it = std::lower_bound(
      c.call_argpos.begin(), c.call_argpos.end(), static_cast<uint32_t>(ix),
      [](const auto& e, uint32_t k) { return e.first < k; });
  if (it == c.call_argpos.end() || it->first != static_cast<uint32_t>(ix))
    return nullptr;
  return &it->second;
}

// The function chunk the call at `ix` was resolved to, with chunk -1 when its
// callee stays a run-time question (Chunk::call_targets).
// A row also exists for an unresolved site whose callee is read through a
// cell, so the chunk is biased by one: -1 rides inside the word rather than
// standing for the empty row.
inline constexpr int32_t kNoCallTarget = -1;
inline int32_t encode_call_target(Chunk::CallTarget t) {
  assert(t.chunk >= -1 && t.chunk < (1 << 28));  // the tag bits' room
  return ((t.chunk + 1) << 3) | (static_cast<int32_t>(t.reach) << 1) |
         (t.callee_in_cell ? 1 : 0);
}
inline Chunk::CallTarget decode_call_target(int32_t w) {
  if (w < 0) return {};
  return {(w >> 3) - 1, static_cast<Chunk::Reach>((w >> 1) & 3),
          (w & 1) != 0};
}
inline Chunk::CallTarget chunk_call_target_at(const Chunk& c, size_t ix) {
  return decode_call_target(ix < c.call_targets.size() ? c.call_targets[ix]
                                                      : kNoCallTarget);
}

// Did slot `s` already own a cell where a cleanup step with this many cells
// behind it runs? (Chunk::slot_cell_rank.)
inline bool chunk_slot_is_cell(const Chunk& c, int32_t s,
                               uint32_t cells_before) {
  return s >= 0 && s < static_cast<int32_t>(c.slot_cell_rank.size()) &&
         c.slot_cell_rank[s] < cells_before;
}

// The slots of a range, in the order a release ladder drops them: newest
// declaration first. Equivalent to walking hi-1 down to lo whenever
// declaration order and slot order agree, which is everywhere a forward
// reference did not move a cell ahead of its binding. The one
// implementation both the emitted scope ladders (Compiler::release_order)
// and the runtime unwind (chunk_release_order) call, so throw-path drop
// order agrees with normal-exit drop order by construction.
inline std::vector<int32_t> release_order_by_rank(
    std::span<const uint32_t> ranks, int32_t lo, int32_t hi) {
  std::vector<int32_t> order;
  for (int32_t s = hi - 1; s >= lo; --s) order.push_back(s);
  std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
    auto rank = [&](int32_t s) {
      return s < static_cast<int32_t>(ranks.size())
                 ? ranks[s]
                 : static_cast<uint32_t>(s);
    };
    return rank(a) > rank(b);
  });
  return order;
}

inline std::vector<int32_t> chunk_release_order(const Chunk& c, int32_t lo,
                                                int32_t hi) {
  return release_order_by_rank(c.slot_rank, lo, hi);
}

// The innermost scope whose range covers `pc`, or -1 when the throw is
// outside every scope (the prologue) and only the frame step owes it
// anything. Innermost-first table order makes the first hit the answer.
inline int32_t chunk_innermost_cleanup(const Chunk& c, size_t pc) {
  for (size_t k = 0; k < c.cleanups.size(); ++k)
    if (c.cleanups[k].start <= pc && pc < c.cleanups[k].end)
      return static_cast<int32_t>(k);
  return -1;
}

// The statement temporaries live at `pc`, newest last (the unwind releases
// them in reverse).
inline std::span<const int32_t> chunk_temps_at(const Chunk& c, size_t pc) {
  auto it = std::upper_bound(
      c.temp_points.begin(), c.temp_points.end(), static_cast<uint32_t>(pc),
      [](uint32_t k, const auto& e) { return k < e.pc; });
  if (it == c.temp_points.begin()) return {};
  --it;
  return {c.temp_slots.data() + it->off, it->len};
}

// The lowest slot an in-flight temporary may sit at and still be abandoned by
// a throw here. A culebra throw is caught by the first enclosing try, and the
// statement it resumes into is the one holding that try — so a temporary
// BELOW that scope belongs to a statement the catch carries on evaluating
// (`to_string(try { … } catch e { … })` holds its callee in one), while
// everything above it belongs to a statement the throw abandoned. With no try
// on the way out, the throw leaves the frame and every temp is abandoned.
inline int32_t chunk_temp_floor(const Chunk& c, size_t pc) {
  for (int32_t k = chunk_innermost_cleanup(c, pc); k >= 0;) {
    const auto& cu = c.cleanups[static_cast<size_t>(k)];
    if (cu.handler != Chunk::kNoHandler) return cu.slot_lo;
    k = cu.parent;
  }
  return 0;
}

struct VmProgram;

// What the executor's closures point at: the trampoline recovers the chunk
// to interpret from one of these (stashed as a Long in the closure's capture
// cell). Exec::run fills `descs` — compile_module returns the program by
// value, so its final address exists only at run time.
struct VmFnDesc {
  const VmProgram* prog;
  int32_t chunk;
};

// A chunk's parameter metadata in the shape the runtime's keyword resolver
// reads. The resolver is handed a `const JitParamMeta*` and indexes arrays of
// cstrings out of it, so the strings and the arrays need somewhere stable to
// live: this owns them for the program's lifetime, like the module-level
// globals the lowering bakes for the same purpose.
struct VmChunkMeta {
  std::vector<const char*> names;
  std::vector<const char*> types;
  std::vector<const char*> declared_types;
  std::vector<uint8_t> has_default_bits;
  std::vector<uint8_t> mut_bits;
  std::string fn_name;
  std::string return_type;
  JitParamMeta meta{};
};

// A compiled module: chunk 0 is the top level; every function literal adds
// one (reserved in creation order, so nested literals interleave freely).
struct VmProgram {
  std::vector<Chunk> chunks;
  std::vector<VmFnDesc> descs;  // filled by Exec::run, one per chunk
  // One per chunk, built before the run: what a keyword call binds against.
  std::vector<std::unique_ptr<VmChunkMeta>> param_metas;
  // The entry module's path, for the debug instructions: they only exist in
  // statements compiled from it (the stdlib prologues get none), so one path
  // per program answers "where is this frame stopped".
  std::string source_path;
  // Compiler scratch, cleared before the program is handed back: where each
  // resolved call site landed, keyed by the cell its callee traces back to
  // (Binding::Known::cell). A capture chain ends at the cell the declaring
  // frame owns, so every site a name's value reaches — however deep — is
  // under one key, and a re-declaration writing that cell strikes them all.
  struct CallSiteRef {
    int32_t chunk;
    uint32_t pc;
  };
  std::map<int64_t, std::vector<CallSiteRef>> resolved_by_cell;
  int64_t next_known_cell = 0;
};

// Build the keyword-resolver's view of every chunk. Called once, before the
// program runs, from whichever lane is about to execute it.
inline void build_param_metas(VmProgram& p) {
  if (!p.param_metas.empty()) return;
  p.param_metas.reserve(p.chunks.size());
  for (const auto& c : p.chunks) {
    auto m = std::make_unique<VmChunkMeta>();
    m->fn_name = c.multifn_name;
    m->return_type = c.return_type;
    for (size_t i = 0; i < c.param_names.size(); i++) {
      m->names.push_back(c.param_names[i].c_str());
      m->types.push_back(i < c.param_types.size() ? c.param_types[i].c_str()
                                                  : "");
      m->declared_types.push_back(
          i < c.param_declared_types.size()
              ? c.param_declared_types[i].c_str()
              : "");
    }
    size_t n = m->names.size();
    m->has_default_bits.assign((n + 7) / 8, 0);
    m->mut_bits.assign((n + 7) / 8, 0);
    for (size_t i = 0; i < n && i < c.param_has_default.size(); i++)
      if (c.param_has_default[i])
        m->has_default_bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    for (size_t i = 0; i < n && i < c.param_mut.size(); i++)
      if (c.param_mut[i])
        m->mut_bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    m->meta = JitParamMeta{m->names.data(),
                           m->has_default_bits.data(),
                           n,
                           c.kwargs_rest_idx,
                           c.first_kw_only_idx,
                           m->fn_name.c_str(),
                           m->return_type.c_str(),
                           m->mut_bits.data(),
                           m->types.data(),
                           m->declared_types.data(),
                           c.cb_min,
                           c.cb_max};
    p.param_metas.push_back(std::move(m));
  }
}

// What `ReplBind` records — the three ways a name's mutability is decided.
enum class ReplBindMode : int32_t { Let = 0, Mut = 1, Assign = 2 };

// How much debug instrumentation a compiled unit carries. `Off` is what a
// plain run compiles: `debugger` is the no-op it is on the other backends,
// and no statement costs an instruction it would not otherwise emit.
enum class Debug : int32_t {
  Off = 0,
  Break = 1,  // `--debug`: only `debugger` breaks (the JIT's `debugger`)
  Step = 2,   // a debug session: every statement in user source can stop
};

// One live executor frame, as a debugger reads it. The register window is on
// the executor's machine stack, so an entry means something only while that
// frame is still on it — which is exactly as long as the hook holds the
// thread. `line` is the boundary the frame is stopped at: for the innermost
// frame the statement about to run, for every other one the call it is
// waiting on, which is what a call stack shows.
struct DbgFrame {
  const VmProgram* prog;
  const Chunk* chunk;
  JitValue* regs;
  size_t pc = 0;
  int64_t line = 0, col = 0;
};

// What a statement boundary reports. `force` marks the `debugger` statement,
// which breaks whether or not anything asked to stop here.
using DbgHook = std::function<void(bool force, int64_t line, int64_t col)>;

// Per-thread, for the reason the interpreter's debug substate is: the frames
// belong to whichever thread runs the program, and every query runs on that
// thread while it is parked inside the hook.
struct DbgState {
  DbgHook hook;
  std::vector<DbgFrame> frames;
  bool tracking = false;  // a session is attached, so frames are kept
};

inline DbgState& dbg_state() {
  static thread_local DbgState s;
  return s;
}

// The REPL's top level: one cell per name, alive for the whole session. It
// is the interp REPL's single persistent Environment, entry for entry — a
// line compiles to its own program and nothing of that program's frame
// survives it, so what a declaration must outlive the line in is here. Cells
// are also the currency of the VM's closure captures, which is what makes a
// closure built on one line see the value a later line puts in the name
// (probed: `let h = fn(){ later }` then `let later = 5`).
class ReplSession {
 public:
  // The cell for `name`, minted holding the unbound sentinel — so a name no
  // line has declared reads as the NameError a lazy forward reference gives.
  JitCell* cell(std::string_view name) {
    if (auto it = entries_.find(name); it != entries_.end()) return it->second.cell;
    auto* c = culebra_runtime_cell_new(TAG_NO_SELF, 0);
    // A map node is a C++-held root the conservative stack scan cannot see,
    // so every cell is pinned for the session — Exec::run's descriptor
    // cells, same reason.
    _gc_heap().pin(c);
    return entries_.emplace(std::string(name), Entry{c, false}).first->second.cell;
  }

  // Whether a declaration for `name` has actually run. Derived from the cell
  // rather than tracked separately: `let x = boom()` mints the cell and then
  // throws, and the interp leaves `x` undeclared for exactly that reason.
  bool declared(std::string_view name) {
    auto it = entries_.find(name);
    return it != entries_.end() && it->second.cell->value.tag != TAG_NO_SELF;
  }

  bool is_mut(std::string_view name) {
    auto it = entries_.find(name);
    return it != entries_.end() && it->second.is_mut;
  }

  // What the name holds right now. The compiler reads this to decide whether
  // a `fn name` declaration appends to a dispatcher an earlier line
  // installed; the session cannot change between compiling a line and
  // running it, so the answer still holds when the code runs.
  JitValue value(std::string_view name) {
    auto it = entries_.find(name);
    return it == entries_.end() ? JitValue{TAG_NO_SELF, 0} : it->second.cell->value;
  }

  void set_mut(std::string_view name, bool is_mut) {
    (void)cell(name);  // ReplCell has normally minted it already
    entries_.find(name)->second.is_mut = is_mut;
  }

  // Hands every cell back. The session a thread falls back to lives as long
  // as its Runtime and never calls this; a debugger's does, since it builds
  // one session per expression it evaluates (see ReplSessionSwap).
  void release_all() {
    // Drained rather than walked: releasing a cell can run culebra code (a
    // `drop` body), and that code can mint a cell of its own — an entry
    // inserted behind the cursor of a plain loop would be cleared without ever
    // being unpinned.
    while (!entries_.empty()) {
      auto batch = std::exchange(entries_, {});
      for (auto& [name, e] : batch) {
        _gc_heap().unpin(e.cell);
        culebra_runtime_cell_release(e.cell);
      }
    }
  }

 private:
  struct Entry {
    JitCell* cell;
    bool is_mut;
  };
  std::map<std::string, Entry, std::less<>> entries_;
};

// One session per Runtime: the REPL, `culebra test` and a debugger each open
// one, and the compiler and the executor have to agree on which it is. A debugger evaluating an expression
// in a paused frame borrows the same machinery for the length of that one
// expression — the frame's names are its session — so which session is
// current is a swappable pointer rather than a fixed object.
inline ReplSession*& current_repl_session() {
  // thread_local (the dbg_state shape): an embedding host may run sessions
  // on several threads, and a debugger's expression session must not leak
  // into another thread's REPL.
  static thread_local ReplSession* cur = nullptr;
  return cur;
}

inline ReplSession& repl_session() {
  auto* cur = current_repl_session();
  if (cur) return *cur;
  // Per Runtime, not per process: a session's cells live on a Runtime's heap
  // (kSlotReplSession).
  return culebra::runtime_substate<ReplSession>(culebra::kSlotReplSession);
}

// Makes `session` the one the compiler and the executor see, and hands its
// cells back on the way out.
struct ReplSessionSwap {
  ReplSession session;
  ReplSession* saved;
  ReplSessionSwap() : saved(current_repl_session()) {
    current_repl_session() = &session;
  }
  ~ReplSessionSwap() {
    // Released while still current: a cell's last reference can run culebra
    // code — a `drop` body, a closure's captures — and that code is this
    // session's, so it must resolve its session cells here and not in
    // whichever session happened to be current before.
    session.release_all();
    current_repl_session() = saved;
  }
  ReplSessionSwap(const ReplSessionSwap&) = delete;
  ReplSessionSwap& operator=(const ReplSessionSwap&) = delete;
};

// ReplBind's work. What mutability a REPL name carries is session state, not
// something the line writing it can settle on its own: an earlier line may
// have declared it `mut` and a later one may take that back while a closure
// compiled in between still holds the cell — which the interp gets right by
// checking its env entry on every write, and so does this.
inline void repl_bind(const char* name, ReplBindMode mode, bool builtin,
                      std::pair<int64_t, int64_t> pos) {
  auto& s = repl_session();
  if (mode != ReplBindMode::Assign) {
    s.set_mut(name, mode == ReplBindMode::Mut);
    return;
  }
  // A write to a name no line has declared: the store that follows declares
  // it, immutably (`w = 3` then `w = 4` is an ImmutableError). A stdlib name
  // is the exception — a bare assignment does not shadow a root binding, it
  // is refused by it (`println = 5`).
  if (!s.declared(name)) {
    if (builtin) culebra_runtime_immutable_assign(name, pos.first, pos.second);
    return;
  }
  if (!s.is_mut(name))
    culebra_runtime_immutable_assign(name, pos.first, pos.second);
}

// Where the value of a line's last statement lands, for the prompt to echo.
// `\x1f` cannot appear in an identifier, so the name collides with nothing a
// user can type (field_init_slot_name's trick).
inline constexpr const char* kReplResultName = "\x1f__repl_result";

inline std::pair<int64_t, int64_t> chunk_pos_at(const Chunk& c, size_t pc) {
  // `positions` is built append-only in emit order, so it is sorted by
  // first_insn — binary-search the covering entry (this runs on the
  // executor's slow paths, e.g. once per Call).
  auto it = std::upper_bound(
      c.positions.begin(), c.positions.end(), pc,
      [](size_t v, const PosEntry& e) { return v < e.first_insn; });
  if (it == c.positions.begin()) return {0, 0};
  --it;
  return {it->line, it->col};
}

// AST -> VmProgram. The front end is the shared FnAnalysis — the same passes
// the JIT runs — so the shadow check and the per-function capture/flag
// analysis are backend-symmetric by construction (captured_locals decides
// which bindings live in cells; free_vars becomes each chunk's capture
// list; uses_fn places the recursion handle). Slot assignment itself is
// the scope stack below.
// What compile_fn_chunk needs beyond parameters and a body: a class member's
// wiring, and the answers only the declaring scope has. `receiver` binds the
// ABI's `self`.
struct MemberOpts {
  bool receiver = false;
  // This chunk IS the synthetic field-init thunk: its body is the declared
  // field stores rather than the AST's.
  const std::vector<const peg::Ast*>* thunk_fields = nullptr;
  // This chunk is a `new` body with declared fields to put in place before
  // its own statements run. At most one of the two is set, on the question
  // compile_class_decl's fields_need_a_thunk asks: run the thunk
  // `field_init_owner` names (reached through the hidden capture fn_analysis
  // gave this body), or emit the same stores right here. A class with no
  // declared fields sets neither.
  const peg::Ast* field_init_owner = nullptr;
  const std::vector<const peg::Ast*>* prologue_fields = nullptr;
  // The enclosing class's constructor chunk, reserved before the members
  // compiled, so `Name.new(...)` inside a member resolves — behind the guard
  // its receiver-read name owes (Chunk::Reach::Guarded).
  int32_t owner_ctor_chunk = -1;
  // The enclosing class's Generic parameters (`class Box<T>`): a member's
  // annotations lower against them, exactly as the JIT's class_type_params_
  // does — an unbounded `T` is documentation and checks as Any.
  const std::vector<std::string_view>* type_params = nullptr;
  // This is the ONLY `fn name` declaration of its name in the scope around
  // it (Scope::sole_multifn, and not a REPL line appending to an earlier
  // one), so its dispatcher's table has one entry for as long as the
  // dispatcher lives. The body's own recursive calls stand on that: `name`
  // inside reads the dispatcher, and this says which chunk it reaches.
  bool sole_multifn = false;
};

// What a call in a postfix chain reaches, when the compiler can name it: the
// `fn` literal the head expression IS, a binding bound once to one
// (Binding::Known), or the constructor of a class the head names. `cell` is
// where that value lives, so a later declaration overwriting it can strike
// the site; `reach` is how the chunk relates to the value the site will hold
// — the same three shapes Chunk::CallTarget records.
struct StaticCallee {
  int32_t chunk = -1;
  int64_t cell = -1;
  Chunk::Reach reach = Chunk::Reach::Direct;
};

// What a construct outside the slice raises while the module compiles. A
// function literal catches it and becomes a chunk whose whole body is the
// rejection (compile_fn_chunk), so the rest of the module still compiles and
// only a call that actually reaches the construct sees VmError; anywhere
// else it reaches compile_module and the module is rejected as a whole.
struct Unsupported {
  std::string what;
  size_t line, col;
};

// One Compiler instance per chunk; the program and the analysis are shared.
// Everything outside the slice raises VmError — at compile time for the
// module, at run time for the function literal that holds it.
class Compiler {
 public:
  // `deps` are the entry module's dependencies in the loader's topological
  // order (every module after the ones it imports), which is the order they
  // are compiled in: each runs to completion, in a scope of
  // its own, before the module that imports it.
  static VmProgram compile_module(
      const peg::Ast& ast, const peg::Ast* stdlib = nullptr,
      Debug debug = Debug::Off,
      const std::vector<const peg::Ast*>& deps = {}) {
    return compile_unit(ast, {.stdlib = stdlib, .debug = debug, .deps = &deps});
  }

  // A loader's module list — dependencies first, the entry module last, with
  // the synthesized stdlib preamble spliced in front when the engine asked
  // for it — as one program. The preamble is a prologue to the entry module
  // rather than a module of its own (compile_module_impl runs it as one), so
  // it comes off the front here; everything between it and the entry is a
  // real dependency, already topologically ordered.
  static VmProgram compile_modules(const std::vector<LoadedModule>& modules,
                                   Debug debug = Debug::Off) {
    return compile_module_list(modules, /*repl=*/false, debug);
  }

  // One REPL input. The line's top-level bindings land in the session's
  // cells rather than this program's slots, so the next line still sees them
  // (and a closure this line builds sees what a later one puts there), and
  // the last statement's value goes to the session's result cell for the
  // prompt to echo. The prologues are the session's, not the line's — see
  // compile_repl_prologue.
  static VmProgram compile_repl_line(const peg::Ast& ast) {
    return compile_unit(ast, {.builtin_traits = false, .repl = true});
  }

  // A loader's whole module list as one SESSION unit — the embedding lane
  // (vm_embed.h). The entry module's top-level bindings land in the current
  // session's cells so the host can read a global or call a function after
  // the run returns; the dependencies compile exactly as compile_modules'
  // do (each in a scope of its own, so a dep never sees the entry's
  // later-declared names — running the modules one by one through
  // compile_repl_line instead would late-bind a dep's closures to session
  // cells the entry fills afterwards, breaking that isolation). The built-in
  // traits are the session's one-time prologue (Session::run_builtin_traits),
  // not this unit's.
  static VmProgram compile_session_modules(
      const std::vector<LoadedModule>& modules, Debug debug = Debug::Off) {
    return compile_module_list(modules, /*repl=*/true, debug);
  }

  // What a REPL session runs before its first input, and again whenever a
  // later line first names a stdlib namespace: the built-in traits, then the
  // lazy-namespace registrations. An ordinary module — a registration binds
  // no name, so none of it has to outlive the program. The session feeds the
  // built-in traits in as source, so nothing is spliced here.
  // Also how culebra_preamble_cc compiles one stdlib module for baking: the
  // unit is the same shape, registrations with no traits prologue. Handing
  // that module to compile_modules instead would peel it off as the preamble
  // and compile it again as the entry, doubling every baked object.
  static VmProgram compile_stdlib_prologue(const peg::Ast& ast) {
    return compile_unit(ast, {.builtin_traits = false});
  }

 private:
  // The one home of the loader-list splitting rule: an optional spliced
  // stdlib preamble up front, dependencies in topological order, the entry
  // module last. Both public module entries above are two-liners over this.
  // A session unit compiles without the built-in traits prologue — they are
  // the session's one-time registration (Session::run_builtin_traits).
  static VmProgram compile_module_list(
      const std::vector<LoadedModule>& modules, bool repl, Debug debug) {
    if (modules.empty()) return {};
    const peg::Ast* stdlib = nullptr;
    size_t first_dep = 0;
    if (modules.front().abs_path == kStdlibPreamblePath) {
      stdlib = modules.front().ast.get();
      first_dep = 1;
    }
    std::vector<const peg::Ast*> deps;
    for (size_t i = first_dep; i + 1 < modules.size(); i++)
      deps.push_back(modules[i].ast.get());
    return compile_unit(*modules.back().ast,
                        {.stdlib = stdlib, .builtin_traits = !repl,
                         .repl = repl, .debug = debug, .deps = &deps});
  }

  struct UnitOpts {
    // The `<stdlib>` module the loader splices ahead of the entry one.
    const peg::Ast* stdlib = nullptr;
    bool builtin_traits = true;
    // Top-level bindings live in the REPL session's cells (see ReplSession).
    bool repl = false;
    Debug debug = Debug::Off;
    // The entry module's dependencies, deps-first. Null (the REPL's and the
    // prologue's case) reads as none.
    const std::vector<const peg::Ast*>* deps = nullptr;
  };

  static VmProgram compile_unit(const peg::Ast& ast, UnitOpts opts) {
    try {
      return compile_module_impl(ast, opts);
    } catch (const Unsupported& u) {
      throw CulebraError("VmError", "--vm: unsupported: " + u.what,
                         static_cast<int64_t>(u.line),
                         static_cast<int64_t>(u.col));
    }
  }

  static VmProgram compile_module_impl(const peg::Ast& ast, UnitOpts opts) {
    const peg::Ast* stdlib = opts.stdlib;
    VmProgram prog;
    FnAnalysis analysis(&is_builtin_var);
    // The built-in traits (Eq's `neq`, Comparable's four comparisons, ...)
    // reach interp and JIT as a synthetic module `with_builtin_traits`
    // prepends; this lane compiles a single module, so the very same
    // declarations run as a prologue instead. A trait declaration binds no
    // name, so nothing of it lands in the user's scope — only in the
    // registry a property read consults.
    const auto* preamble = opts.builtin_traits
                               ? culebra::parse_builtin_traits_preamble().get()
                               : nullptr;
    if (preamble) (void)analysis.analyze_program(*preamble);
    // The stdlib preamble, which the loader splices as its own module ahead
    // of the entry one: it declares each lazy module's builder and registers
    // it, so a lane that skips it cannot resolve `Time` or `assert_eq` at
    // all. It runs as a second prologue rather than a second module — the
    // same shape the built-in traits take, and the JIT bundles it into the
    // one IR for the same reason.
    if (stdlib) (void)analysis.analyze_program(*stdlib);
    // Each dependency's top-level analysis is its own — free variables and
    // which locals need cells are per module, as the JIT's per-module
    // main_info_ is. A deque so the pointers handed to `main` stay put.
    std::deque<FuncInfo> dep_infos;
    if (opts.deps) {
      for (const auto* d : *opts.deps)
        dep_infos.push_back(analysis.analyze_program(*d));
    }
    // lint::check_shadow (parity) + the per-fn FuncInfo compile_fn_chunk
    // reads; the returned top-level info carries chunk 0's captured_locals.
    FuncInfo top_info = analysis.analyze_program(ast, opts.repl);
    prog.chunks.emplace_back();  // reserve index 0 for the top level
    Compiler main(prog, analysis, /*in_function=*/false, &top_info);
    main.repl_ = opts.repl;
    // A statement carries a debug instruction only when it comes from the
    // entry module: the prologues below are the stdlib's, and a debugger
    // stops in user source (the interp's hook is keyed the same way, by the
    // `<builtin>` path its lazy modules evaluate under).
    main.debug_ = opts.debug;
    prog.source_path = ast.path;
    // The top-level frame mark (JIT main's fn.mark): first insn, so a
    // throw at any pc finds it populated. The Halt epilogue runs to it —
    // before the top scope's releases, hence the inlined block below.
    main.establish_frame_defer_mark(ast, top_info);
    {
      using namespace peg::udl;
      main.push_scope(ast, /*owned_mark=*/false);
      auto run_prologue = [&](const peg::Ast* p) {
        if (!p) return;
        main.predeclare_forward_refs(*p);
        if (p->tag == "STATEMENTS"_) {
          for (const auto& n : p->nodes) main.compile_statement(*n);
        } else {
          main.compile_statement(*p);
        }
      };
      main.predeclare_forward_refs(ast);
      run_prologue(preamble);
      run_prologue(stdlib);
      // Each dependency runs to completion in a scope of its own, then hands
      // the runtime module table the Object its `export` statements name.
      // The scope closes before the next module opens, which is what keeps a
      // dependency's top-level names out of every other module (the interp's
      // per-module env).
      for (size_t i = 0; opts.deps && i < opts.deps->size(); i++) {
        const peg::Ast& dep = *(*opts.deps)[i];
        const FuncInfo* saved = main.info_;
        // A dependency is module-scoped even in a session unit
        // (compile_session_modules): under repl_ its unresolved names would
        // fall back to session cells — which the ENTRY fills — so a dep
        // could read a name the entry declares after the import (measured:
        // module_scope_smoke's read-side contract broke exactly this way).
        const bool saved_repl = main.repl_;
        main.repl_ = false;
        main.info_ = &dep_infos[i];
        main.push_scope(dep, /*owned_mark=*/false);
        run_prologue(&dep);
        main.emit_module_export(dep);
        main.pop_scope();
        main.info_ = saved;
        main.repl_ = saved_repl;
      }
      if (opts.repl) {
        // The interp REPL echoes what `interpret()` returns — the value of
        // the last statement — so the line's body compiles the way a
        // function's does, and the result goes where the prompt can read it
        // after the program ends. Storing it into a session cell rather than
        // handing it back keeps the temp's own lifetime honest: the driver
        // releases the cell right after echoing, which is where the interp's
        // `val` dies too (probed — a `drop` fires before the next prompt).
        int32_t rv = main.alloc_slot(ast, "(repl.result)");
        main.compile_body_into(ast, rv);
        int32_t cell = main.session_slot(ast, kReplResultName);
        main.store_cell(ast, cell, {rv, true});
      } else if (ast.tag == "STATEMENTS"_) {
        for (const auto& n : ast.nodes) main.compile_statement(*n);
      } else {
        main.compile_statement(ast);
      }
      if (main.frame_defer_mark_ >= 0)
        main.emit(Op::DeferRunTo, main.frame_defer_mark_);
      // Program exit releases the top level's bindings without running their
      // `drop` — docs §17, and the JIT's suppressed __culebra_main epilogue.
      // The throw path is program exit too, so the frame step of the unwind
      // walk suppresses the same way (Chunk::suppress_frame_drop).
      main.emit(Op::DropSuppress, 1);
      main.pop_scope();
      main.emit(Op::DropSuppress, 0);
    }
    main.emit(Op::Halt);
    main.chunk_.suppress_frame_drop = true;
    main.finalize_chunk();
    prog.chunks[0] = std::move(main.chunk_);
    // The revocation bookkeeping dies with the compile: the chunks now carry
    // every answer either consumer reads.
    prog.resolved_by_cell.clear();
    return prog;
  }

  Compiler(VmProgram& prog, FnAnalysis& analysis, bool in_function,
           const FuncInfo* info, int32_t chunk_idx = 0)
      : prog_(prog),
        analysis_(analysis),
        in_function_(in_function),
        info_(info),
        chunk_idx_(chunk_idx) {}

  struct Binding {
    std::string name;
    int32_t slot;
    bool is_mut;
    // The slot holds a cell pointer, not the value: reads go through
    // CellGet, writes through CellSet. True for a captured local's owned
    // cell and for a capture bound from the closure (borrowed).
    bool is_cell = false;
    // Pre-declared `fn name` dispatcher cell (or a capture of one): the
    // cell holds the unbound sentinel until the decl statement runs, so
    // reads emit UnboundErr (the JIT's lazy forward-ref read guard).
    bool lazy = false;
    // What a lazy pre-declaration shadows, when a binding of the same
    // name was already visible where it landed. A declaration only takes
    // effect from the statement that runs it, so until then reads and
    // writes go here instead.
    // `shadowed_builtin` says the same for a stdlib global.
    std::shared_ptr<Binding> shadowed;
    bool shadowed_builtin = false;
    // Pre-declared for a collapsed `if` arm, whose declaration may not run
    // at all — and, when the `if` has several arms declaring the name, runs
    // in exactly one of them. So the declaration statement fills the cell
    // without settling the binding: it stays lazy, so every arm finds the
    // same cell and a read still asks the sentinel whether any arm ran.
    bool conditional = false;
    // A conditional binding's `mut` is the runtime fact its declaration
    // wrote, not something the compiler can name: the arms declare the same
    // name with their own mutability and only one of them runs, so a later
    // write must ask which did. Holds a Bool, false until a declaration
    // lands. -1 for every other binding, whose `is_mut` above is already
    // the whole answer.
    int32_t mut_slot = -1;
    // Pre-declared for a bare `x = v` site, which IS the declaration: that
    // write fills the cell instead of being checked against it. Cleared
    // once it lands.
    bool awaits_implicit = false;
    // A REPL session binding: the slot holds the session's cell for the
    // name (borrowed), and its mutability is the session's to answer, so
    // every write asks at run time rather than trusting `is_mut` — see
    // Op::ReplBind.
    bool session = false;
    // Where the name became visible, for Chunk::SlotDebug. Set by
    // push_binding, the one door into a scope.
    uint32_t debug_start = 0;
    // What reading this name tells the compiler, and the cell that answer
    // is anchored to. One record because the parts are granted together
    // (settle_callee), carried together through a capture list, and struck
    // together (revoke_known) — a fact that arrived on its own would be a
    // fourth thing to remember to clear.
    struct Known {
      // The function chunk every read of this name yields a closure over,
      // or -1 where the value is a run-time question. Granted only to a
      // binding written exactly once, by the `fn` literal its declaration
      // compiled (grant_known_chunk); a capture inherits it, since the cell
      // it borrows is the very one that binding owns.
      int32_t chunk = -1;
      // How that chunk is reached: a `fn name` binds a dispatcher, so its
      // code is one indirection further — the chunk of the single untyped
      // overload the table holds (grant_mono_chunk). At most one grant ever
      // lands on a binding, so the pair is the answer and its shape, not
      // two answers.
      bool via_mono = false;
      // Which cell that value lives in, program-wide (-1 for a plain slot).
      // A re-declaration of the same name in the same scope writes THAT
      // cell rather than minting a second one, so the closures built
      // between the two declarations would call the first function's code —
      // the id is how the second one finds the sites it invalidates
      // (revoke_known).
      int64_t cell = -1;
      // The constructor chunk `Name.new(...)` reaches, for a name a class
      // declaration wrote (grant_known_ctor). A different question from
      // `chunk`: it is about calling `.new` ON the value rather than calling
      // the value. Same "written exactly once" rule, and the same
      // revocation — a class name is bound through a cell, so a
      // re-declaration strikes its sites through `cell` too. Granted only
      // where the answer cannot move at run time: one `new` (an overload set
      // is a dispatcher, not a chunk) and no decorator, which is free to
      // hand back something that is not the class.
      int32_t ctor = -1;
    };
    Known known;
  };
  struct Scope {
    // A deque, not a vector: a `Binding*` from lookup / predeclared_here is
    // held across the RHS compile that follows it, and compiling a closure
    // there can register a REPL session binding in this same scope — a
    // vector's reallocation would leave the caller writing through a dangling
    // pointer.
    std::deque<Binding> bindings;
    int32_t slot_watermark;  // next_slot_ at scope entry; pop rolls back
    // Where the scope's current cleanup segment begins, the mark its own
    // defers stand on (-1 when it declares none), and the segments it has
    // left behind: together they are the Chunk::Cleanup entries the throw
    // path walks.
    uint32_t start_pc = 0;
    int32_t defer_mark = -1;
    std::vector<size_t> segments;
    // A generic for-in's cursor base when this is that loop's own scope
    // (Chunk::Cleanup::dispose_base); -1 for every other scope.
    int32_t dispose_base = -1;
    // `fn name`s already declared directly in this scope. A later
    // same-scope overload appends to the dispatcher the binding already
    // holds; the first one mints a fresh dispatcher + table, so neither an
    // unrelated scope's overloads nor the previous activation's bleed in
    // (the JIT's Scope::multifn_decls).
    std::set<std::string> multifn_decls;
    // Of those, the names this statement list declares exactly ONCE — the
    // only shape whose dispatcher table can never grow a second entry, since
    // `into` (the append) is emitted only by a same-scope second declaration.
    // Filled by predeclare_forward_refs, which walks the whole list before
    // any of it compiles; what grant_mono_chunk stands on.
    std::set<std::string> sole_multifn;
    // This scope's entry in the frame's owned-mark array — its own static
    // depth, or -1 where the scope resolves nothing (chunk 0's frame scope,
    // since program exit does not drop).
    int32_t owned_mark = -1;
  };
  struct LoopCtx {
    int32_t slot_watermark;  // slots >= this are inner to the loop scope
    size_t defer_watermark;  // defer_scopes_.size() at loop entry: entries
                             // above it are scopes a break/continue jumps
                             // out of, and the first (outermost) one's mark
                             // bounds every defer the iteration pushed
    std::vector<size_t> break_jumps;
    std::vector<size_t> continue_jumps;
    // A `nobreak { … }` clause runs only when the loop was not broken out
    // of, so a break has to say so: this slot (-1 without the clause) holds
    // the flag the tail tests, in the scope enclosing the loop's own.
    int32_t broke_slot = -1;
    // The loop body's scope (pushed right after this context): its owned
    // mark is the lowest of the scopes a break or continue abandons, so
    // resolving from it covers them all.
    size_t body_scope_index = 0;
  };
  struct ExprResult {
    int32_t slot;
    bool owned;  // true: statement temp holding a +1; false: named slot
    // The function chunk this expression IS, when it is a `fn` literal
    // (-1 otherwise). What a declaration passes on to its binding, and what
    // an immediately-invoked literal calls — see grant_known_chunk.
    int32_t chunk = -1;
  };

  VmProgram& prog_;
  FnAnalysis& analysis_;
  bool in_function_;
  // Compiling one REPL input: a name this program does not bind is the
  // session's rather than a rejection, and the top level's own declarations
  // bind there too. Inherited by every nested chunk's Compiler — a free
  // variable resolves to a session cell from any depth, the way the interp's
  // environment chain reaches the REPL's one global env from any closure.
  bool repl_ = false;
  // How much debug instrumentation this unit carries, inherited by every
  // nested chunk's Compiler (a breakpoint has to reach a function body).
  Debug debug_ = Debug::Off;
  const FuncInfo* info_;  // this chunk's analysis (captured_locals gate)
  Chunk chunk_;
  // Where `chunk_` will land in prog_.chunks — reserved before the body
  // compiles, so a resolved call site can name the chunk it sits in while
  // that chunk is still being built.
  int32_t chunk_idx_ = 0;
  // A deque for the same reason Scope::bindings is one, one level up: a
  // vector reallocating on push_scope relocates every Scope (their deques'
  // move constructors are not noexcept, so it copies them), and a `Binding*`
  // held across the RHS that opened the scope would be left dangling.
  std::deque<Scope> scopes_;
  std::vector<LoopCtx> loops_;
  // Cursor bases of the generic for-ins whose scope is still open, so every
  // release ladder emitted inside them closes their iterators in place.
  std::vector<int32_t> for_bases_;
  int32_t next_slot_ = 0;
  int32_t named_top_ = 0;  // one past the highest live named slot; the temp
                           // sweep rolls next_slot_ back to at most this, so
                           // a let declared mid-statement keeps its slot
  int32_t high_water_ = 0;
  std::map<std::string, int32_t> str_const_ix_;  // kconst_str interning
  std::vector<int32_t> stmt_temps_;
  // Per-slot named flag for the CURRENT generation of each slot (alloc_raw
  // rewrites it on reuse). Scope-exit releases consult this instead of a
  // "named slots sit below all temps" watermark — a frame's return-value
  // temp is allocated before the body's named slots, so that ordering
  // assumption does not hold once functions exist.
  std::vector<bool> slot_named_;
  // Order the release ladder walks, newest declaration first — the JIT's
  // per-binding release registration. Slot order says the same thing
  // everywhere except a forward-ref pre-declaration, whose cell is minted
  // at the head of the statement list but whose binding is the statement
  // that fills it, so its rank is re-stamped there.
  std::vector<uint32_t> slot_rank_;
  uint32_t next_rank_ = 0;
  // Parallel: the slot owns a cell (a captured local's CellNew target), so
  // scope exit emits CellRelease instead of Release. Borrowed capture slots
  // stay false — the closure owns their cell ref.
  std::vector<bool> slot_cell_;
  // Owned cell slots never go back to the allocator, and alloc_cell_slot
  // hands out only an index no earlier generation used, so a slot is
  // cell-or-plain for the chunk's whole lifetime — what lets every release
  // ladder pick Release vs CellRelease statically.
  int32_t pin_floor_ = 0;
  // Frame-level defer mark slot (mirrors the JIT's fn.mark, gated on
  // has_any_defer): `return` and the Halt epilogue run to it, as does the
  // executor / frame pad when a throw escapes every region.
  int32_t frame_defer_mark_ = -1;
  // Declared return type of this frame (const index) and the slot holding
  // the position its violation reports; -1 when the function declares none.
  int32_t ret_type_ = -1;
  std::string ret_type_str_;  // same annotation, for emit_type_check_gate
  int32_t ret_ctx_ = -1;
  int32_t ret_pos_slot_ = -1;
  // Mark slots of the open scopes that declared their own defers, outermost
  // first (the JIT's Scope::defer_mark, flattened): break/continue run to
  // the first entry above the loop's defer_watermark.
  std::vector<int32_t> defer_scopes_;
  // A mark taken for the scope that is about to be pushed (DeferScope and
  // compile_try establish theirs before the push, since the slot has to
  // outlive the scope's own releases). push_scope consumes it.
  int32_t pending_scope_mark_ = -1;
  // Cells created so far, and the cleanup segments of the frame scope (which
  // answers for the whole chunk, prologue included).
  uint32_t n_cells_ = 0;
  std::vector<size_t> frame_segments_;

  // A slot that owns a cell is pinned from here on, so it stays a cell for the
  // rest of the chunk — but the generations BEFORE this point may have used
  // the index for a temporary, and their ladders must still pick the plain
  // Release. Ranking the cell and closing every open scope's current cleanup
  // segment is what dates the two apart (Chunk::slot_cell_rank).
  int32_t alloc_cell_slot(const peg::Ast& at, std::string name) {
    split_cleanup_segments();
    int32_t s = alloc_slot(at, std::move(name));
    slot_cell_[s] = true;
    if (s >= static_cast<int32_t>(chunk_.slot_cell_rank.size()))
      chunk_.slot_cell_rank.resize(s + 1, Chunk::kNotACell);
    chunk_.slot_cell_rank[s] = n_cells_++;
    pin_floor_ = std::max(pin_floor_, s + 1);
    return s;
  }

  // End every open scope's current cleanup segment here and start a new one:
  // the segments differ only in how many cells existed, which is what decides
  // Release vs CellRelease for the slots each releases.
  void split_cleanup_segments() {
    for (auto sc = scopes_.rbegin(); sc != scopes_.rend(); ++sc) {
      auto pc = static_cast<uint32_t>(chunk_.code.size());
      if (sc->start_pc == pc) continue;  // nothing emitted in this segment
      record_cleanup(*sc, pc);
      sc->start_pc = pc;
    }
  }

  // One segment of a scope's unwind step, in the innermost-first order the
  // walk relies on. compile_try turns the segments of a try body's scope into
  // catching ones.
  void record_cleanup(Scope& sc, uint32_t end) {
    sc.segments.push_back(chunk_.cleanups.size());
    chunk_.cleanups.push_back({sc.start_pc, end, /*parent=*/-1, sc.defer_mark,
                               sc.slot_watermark, named_top_, n_cells_,
                               Chunk::kNoHandler, -1, sc.dispose_base});
  }
  uint32_t pend_line_ = 0, pend_col_ = 0;

  [[noreturn]] static void reject(const peg::Ast& ast, const std::string& what) {
    throw Unsupported{what, ast.line, ast.column};
  }

  void stamp(const peg::Ast& ast) {
    if (ast.line) {
      pend_line_ = static_cast<uint32_t>(ast.line);
      pend_col_ = static_cast<uint32_t>(ast.column);
    }
  }

  // The same, from a position an analysis produced rather than a node.
  void stamp_at(size_t line, size_t col) {
    if (line) {
      pend_line_ = static_cast<uint32_t>(line);
      pend_col_ = static_cast<uint32_t>(col);
    }
  }

  // The innermost compile_expr frame owns the pending position while it
  // emits, and restores the caller's on exit — the compiler-time analog of
  // the JIT's PosGuard, so an operator instruction is attributed to its own
  // expression node, not to the last leaf of its operands.
  struct StampGuard {
    Compiler& c;
    uint32_t l, col;
    StampGuard(Compiler& c_, const peg::Ast& ast)
        : c(c_), l(c_.pend_line_), col(c_.pend_col_) {
      c.stamp(ast);
    }
    ~StampGuard() {
      c.pend_line_ = l;
      c.pend_col_ = col;
    }
  };

  size_t emit(Op op, int32_t a = 0, int32_t b = 0, int32_t c = 0,
              int32_t d = 0) {
    if (chunk_.positions.empty() ||
        chunk_.positions.back().line != pend_line_ ||
        chunk_.positions.back().col != pend_col_) {
      chunk_.positions.push_back(
          {static_cast<uint32_t>(chunk_.code.size()), pend_line_, pend_col_});
    }
    record_temp_point();
    chunk_.code.push_back({op, a, b, c, d});
    return chunk_.code.size() - 1;
  }

  // Delta-code the live statement temporaries for the instruction about to be
  // emitted: the unwind path releases exactly these before it runs any defer.
  // Unchanged sets (whole runs of instructions inside one expression) reuse
  // the previous entry.
  void record_temp_point() {
    if (!chunk_.temp_points.empty()) {
      const auto& last = chunk_.temp_points.back();
      if (last.len == stmt_temps_.size() &&
          std::equal(stmt_temps_.begin(), stmt_temps_.end(),
                     chunk_.temp_slots.begin() + last.off))
        return;
    } else if (stmt_temps_.empty()) {
      return;
    }
    chunk_.temp_points.push_back(
        {static_cast<uint32_t>(chunk_.code.size()),
         static_cast<uint32_t>(chunk_.temp_slots.size()),
         static_cast<uint32_t>(stmt_temps_.size())});
    chunk_.temp_slots.insert(chunk_.temp_slots.end(), stmt_temps_.begin(),
                             stmt_temps_.end());
  }

  // Jump-target operand encoding, single-sourced: an unconditional Jump
  // carries its target in `.a`, every conditional (and ForPrep) in `.b`.
  void patch_jump(size_t ix, size_t target) {
    auto& insn = chunk_.code[ix];
    (insn.op == Op::Jump ? insn.a : insn.b) = static_cast<int32_t>(target);
  }
  void patch_to_here(size_t ix) { patch_jump(ix, chunk_.code.size()); }

  int32_t alloc_raw(const peg::Ast& at, std::string name, bool named) {
    // Every binding this frame declares passes through here — the one gate
    // on the frame's slot budget.
    if (next_slot_ >= kMaxSlots)
      reject(at, culebra::format("frame larger than {} slots", kMaxSlots));
    int32_t s = next_slot_++;
    if (s >= static_cast<int32_t>(chunk_.slot_names.size()))
      chunk_.slot_names.resize(s + 1);
    chunk_.slot_names[s] = std::move(name);
    if (s >= static_cast<int32_t>(slot_named_.size())) {
      slot_named_.resize(s + 1);
      slot_cell_.resize(s + 1);
      slot_rank_.resize(s + 1);
    }
    slot_named_[s] = named;
    slot_cell_[s] = false;
    slot_rank_[s] = next_rank_++;
    high_water_ = std::max(high_water_, next_slot_);
    return s;
  }

  int32_t alloc_slot(const peg::Ast& at, std::string name) {
    int32_t s = alloc_raw(at, std::move(name), /*named=*/true);
    named_top_ = next_slot_;
    return s;
  }

  int32_t alloc_temp(const peg::Ast& at) {
    int32_t s = alloc_raw(at, "(tmp)", /*named=*/false);
    stmt_temps_.push_back(s);
    return s;
  }

  int32_t kconst(JitValue v) {
    chunk_.consts.push_back(v);
    return static_cast<int32_t>(chunk_.consts.size()) - 1;
  }

  int32_t kconst_long(int64_t v) { return kconst({TAG_LONG, v}); }

  // A declaration position packed for PosSnap's last-resort argument (the
  // JIT bakes the same current_line_/column_ pair into its prologue call).
  int32_t def_pos_const(const peg::Ast& at) {
    return kconst_long((static_cast<int64_t>(at.line) << 32) |
                       static_cast<int64_t>(at.column));
  }

  // Record where the just-emitted call's arguments were written, so a
  // typed-parameter error in the callee reports at the argument expression
  // (emit_call_position_publish's rodata array, per instruction). A null
  // node — the UFCS receiver, which has no argument expression — takes the
  // call's own position, as the JIT's null entry does.
  void record_call_argpos(size_t ix, const peg::Ast& at,
                          std::vector<const peg::Ast*> arg_asts) {
    if (arg_asts.empty()) return;
    std::vector<int64_t> packed;
    packed.reserve(arg_asts.size());
    for (const auto* a : arg_asts) {
      const peg::Ast& n = a ? *a : at;
      packed.push_back((static_cast<int64_t>(n.line) << 32) |
                       static_cast<int64_t>(n.column));
    }
    chunk_.call_argpos.emplace_back(static_cast<uint32_t>(ix),
                                    std::move(packed));
  }

  // What both grants refuse: a binding whose value is a run-time question —
  // reassignment (`mut`, or a conditional arm's), and a REPL session cell the
  // next line can refill.
  static bool binding_writes_once(const Binding& b) {
    return !b.is_mut && !b.session && !b.conditional && !b.shadowed_builtin &&
           !b.awaits_implicit && b.mut_slot < 0;
  }

  // `b` reads as a closure over `chunk` for as long as it is in scope: its
  // declaration is the only write, and it is the `fn` literal just
  // compiled. A lazy forward reference declines too — its sentinel is a
  // value a read still has to test.
  void grant_known_chunk(Binding& b, int32_t chunk) {
    if (chunk < 0 || b.lazy || !binding_writes_once(b)) return;
    settle_callee(b, chunk, /*via_mono=*/false);
  }

  // `b` is a `fn name` whose dispatcher can only ever hold `chunk`'s body.
  // `lazy` is allowed here and nowhere else: a `fn` binding never sheds it,
  // because its declaration is hoisted and a read before it ran has to keep
  // testing for the sentinel — and having passed that test, the value IS
  // this declaration's dispatcher (which is also why a `shadowed` fallback,
  // where it need not be, declines). What rules the table out from growing
  // is the caller's (Scope::sole_multifn); what rules out a second write to
  // the cell is the same revocation grant_known_chunk relies on.
  void grant_mono_chunk(Binding& b, int32_t chunk) {
    if (chunk < 0 || b.shadowed || !binding_writes_once(b) ||
        !mono_eligible_chunk(chunk))
      return;
    settle_callee(b, chunk, /*via_mono=*/true);
  }

  void settle_callee(Binding& b, int32_t chunk, bool via_mono) {
    b.known = {chunk, via_mono, b.is_cell ? prog_.next_known_cell++ : -1};
  }

  // `b` is a class declaration's name, and `Name.new(...)` reaches `chunk`
  // for as long as it is in scope. Same "the declaration is the only write"
  // rule as grant_known_chunk, and it takes a cell id for the same reason: a
  // re-declaration has to strike the sites that resolved through it. The
  // caller decides eligibility — see Binding::Known::ctor.
  void grant_known_ctor(Binding& b, int32_t chunk) {
    if (chunk < 0 || !binding_writes_once(b)) return;
    b.known.ctor = chunk;
    if (b.is_cell && b.known.cell < 0) b.known.cell = prog_.next_known_cell++;
  }

  // A second declaration of the same name in this scope writes the cell the
  // first one owns, so a site resolved through it may now call the wrong
  // code. Strike every one of them — including the sites that ran before
  // the re-declaration and were right to resolve, since telling those apart
  // means knowing whether a loop wraps both.
  void revoke_known(Binding& b) {
    if (b.known.cell >= 0) {
      auto it = prog_.resolved_by_cell.find(b.known.cell);
      if (it != prog_.resolved_by_cell.end()) {
        for (const auto& s : it->second) {
          // Only the chunk is struck. Where the callee is read matters to
          // the instruction's own operand — the `b` of a borrowed site names
          // a cell either way — so that bit survives the revocation.
          auto& w = call_targets_of(s.chunk)[s.pc];
          w = decode_call_target(w).callee_in_cell
                  ? encode_call_target({-1, Chunk::Reach::Direct, true})
                  : kNoCallTarget;
        }
        prog_.resolved_by_cell.erase(it);
      }
    }
    b.known = {};
  }

  // Where a resolved site's entry lives. A cell only ever reaches sites in
  // the chunk that declared it — still being built, so its table is
  // `chunk_` — and in the nested chunks that captured it, which finished
  // and moved into the program before the re-declaration could run.
  std::vector<int32_t>& call_targets_of(int32_t chunk) {
    return chunk == chunk_idx_
               ? chunk_.call_targets
               : prog_.chunks[static_cast<size_t>(chunk)].call_targets;
  }

  // A chunk's signature, whether or not its compile has finished: the one
  // being built lives in `chunk_` and is not in the program yet, which is
  // exactly the chunk a `fn name`'s own recursive call names.
  const Chunk& chunk_ref(int32_t chunk) const {
    return chunk == chunk_idx_ ? chunk_
                               : prog_.chunks[static_cast<size_t>(chunk)];
  }

  // Would a positional call of `argc` arguments reach `chunk`'s body through
  // its dispatcher? The window the lone overload accepts is the chunk's own
  // positional bounds — `cb_min`/`cb_max` are that same "regular parameters
  // only, floor opened by a defaulted tail" derivation, computed once when
  // the chunk was compiled (`cb_max < 0` is the `*args` catch-all).
  bool mono_window_admits(int32_t chunk, int32_t argc) const {
    const Chunk& f = chunk_ref(chunk);
    return argc >= f.cb_min && (f.cb_max < 0 || argc <= f.cb_max);
  }

  // Every regular parameter annotation-free — the shape `multifn_pick` can
  // never be asked about, and so the shape a lone table entry is a
  // monomorphic shortcut for (_jit_multifn_refresh_mono).
  bool mono_eligible_chunk(int32_t chunk) const {
    const Chunk& f = chunk_ref(chunk);
    for (const auto& t : f.param_types)
      if (!t.empty()) return false;
    return true;
  }

  // Record that the call at `ix` reaches a named chunk, under the cell its
  // callee traces back to (-1 for a plain slot, which no re-declaration can
  // reach) — and, independently of that, whether the site reads its callee
  // through a cell.
  void record_call_target(size_t ix, StaticCallee target, int32_t argc,
                          bool callee_in_cell = false) {
    int32_t chunk = target.chunk;
    // A dispatcher's body is reached only for the arities its one overload
    // accepts; every other count is the DispatchError the dynamic arm
    // raises, so those sites stay unresolved.
    if (chunk >= 0 && target.reach == Chunk::Reach::Mono &&
        !mono_window_admits(chunk, argc))
      chunk = -1;
    if (chunk < 0 && !callee_in_cell) return;
    if (chunk_.call_targets.size() <= ix)
      chunk_.call_targets.resize(ix + 1, kNoCallTarget);
    chunk_.call_targets[ix] = encode_call_target(
        {chunk, chunk >= 0 ? target.reach : Chunk::Reach::Direct,
         callee_in_cell});
    if (chunk >= 0 && target.cell >= 0)
      prog_.resolved_by_cell[target.cell].push_back(
          {chunk_idx_, static_cast<uint32_t>(ix)});
  }

  // Runtime string layout via _str_init (jit_string.h — the same header +
  // NUL shape the JIT bakes into .rodata); the value points at the bytes
  // (see the str_arena comment on Chunk). Repeated spellings (a lazy
  // binding's name read at every site) intern to one entry.
  int32_t kconst_str(std::string_view bytes) {
    auto [it, fresh] = str_const_ix_.try_emplace(std::string(bytes), 0);
    if (!fresh) return it->second;
    auto buf =
        std::make_unique<char[]>(sizeof(JitStrHeader) + bytes.size() + 1);
    char* data = _str_init(buf.get(), bytes.size());
    std::memcpy(data, bytes.data(), bytes.size());
    chunk_.str_arena.push_back(std::move(buf));
    it->second = kconst({TAG_STRING, reinterpret_cast<int64_t>(data)});
    return it->second;
  }

  // Freeze what the runtime needs of the frame layout: the slot count, the
  // cell ranks each cleanup step reads to pick its release op, and the
  // scope chain the throw path walks.
  void finalize_chunk() {
    chunk_.num_slots = high_water_;
    chunk_.slot_cell_rank.resize(static_cast<size_t>(high_water_),
                                 Chunk::kNotACell);
    slot_rank_.resize(static_cast<size_t>(high_water_));
    chunk_.slot_rank = slot_rank_;
    if (chunk_.cleanups.empty()) return;
    // The frame scope's segments carry the frame's defers, and the outermost
    // pair stretches over the whole chunk — a throw in the prologue, before
    // any scope opened, still owes those defers and the frame's releases.
    for (size_t ix : frame_segments_) {
      auto& seg = chunk_.cleanups[ix];
      if (seg.defer_mark_slot < 0) seg.defer_mark_slot = frame_defer_mark_;
    }
    if (!frame_segments_.empty()) {
      chunk_.cleanups[frame_segments_.front()].start = 0;
      chunk_.cleanups[frame_segments_.back()].end =
          static_cast<uint32_t>(chunk_.code.size());
    }
    // Chain the scopes: segments are innermost-first, so a scope's encloser is
    // the first later entry whose range contains it (splits close every open
    // scope at once, so the two always nest cleanly). A frame segment has no
    // encloser and keeps parent -1, which is what makes it the frame step.
    for (size_t k = 0; k + 1 < chunk_.cleanups.size(); ++k) {
      auto& cu = chunk_.cleanups[k];
      for (size_t m = k + 1; m < chunk_.cleanups.size(); ++m) {
        const auto& out = chunk_.cleanups[m];
        if (out.start <= cu.start && cu.end <= out.end) {
          cu.parent = static_cast<int32_t>(m);
          break;
        }
      }
    }
  }

  // A scope opens where its first instruction will land, and adopts the mark
  // its DeferScope took just before it (the mark slot belongs to the
  // enclosing scope, so it is established outside this push).
  //
  // Its own first slot is the owned-stack watermark its exit resolves — the
  // JIT's per-scope `owned_mark` SSA value, which a register machine has to
  // keep somewhere. A frame scope takes none here: its slots [0, arity) are
  // the ABI's, so establish_frame_owned_mark places it once the parameters
  // are laid out.
  // The optional init clause of `if` / `while` / `match` (`if mut x = f(); …`)
  // scopes its bindings to the whole construct: one scope around the lot,
  // with the bindings as ordinary declarations inside it. They outlive every
  // arm and every iteration (a captured one is a single cell, not one per
  // turn) and die at the construct's exit, where this scope's ladder runs.
  // A block of the construct is not a defer scope on any backend, and this
  // scope does not make it one — no DeferScope is opened here.
  struct InitScope {
    Compiler& c;
    bool open;
    InitScope(Compiler& comp, const peg::Ast& at, const peg::Ast* init)
        : c(comp), open(init != nullptr) {
      if (!open) return;
      c.push_scope(at);
      for (const auto& b : init->nodes) c.compile_statement(*b);
    }
    ~InitScope() {
      if (open) c.pop_scope();
    }
    InitScope(const InitScope&) = delete;
    InitScope& operator=(const InitScope&) = delete;
  };

  void push_scope(const peg::Ast& at, bool owned_mark = true) {
    scopes_.push_back({{}, next_slot_,
                       static_cast<uint32_t>(chunk_.code.size()),
                       std::exchange(pending_scope_mark_, -1)});
    if (owned_mark) take_owned_mark(at);
  }

  // The one door into a scope's binding list, so the debug table's live
  // ranges cannot drift from what `lookup` answers: a name is visible from
  // the instruction its declaration landed at until its scope closes.
  Binding& push_binding(Binding b) {
    b.debug_start = static_cast<uint32_t>(chunk_.code.size());
    scopes_.back().bindings.push_back(std::move(b));
    return scopes_.back().bindings.back();
  }

  // A closing scope's bindings, as the ranges a paused frame is read
  // through. Only a unit compiled for debugging carries them.
  void record_slot_debug(const Scope& sc) {
    if (debug_ == Debug::Off) return;
    auto end = static_cast<uint32_t>(chunk_.code.size());
    for (const auto& b : sc.bindings) {
      if (b.slot < 0 || b.session) continue;  // a session cell is not a slot
      chunk_.slot_debug.push_back({b.slot, b.debug_start, end, b.is_mut,
                                   b.is_cell, b.name});
    }
  }

  // The frame's own watermark, taken after the parameter slots and before
  // the prologue runs any user code (a default expression can already build
  // a resource). `return` and the unwind's frame step resolve from it: it is
  // the lowest mark in the frame, so one run covers every scope still open.
  void establish_frame_owned_mark(const peg::Ast& at) {
    take_owned_mark(at);
    chunk_.owned_frame_depth = scopes_.front().owned_mark;
  }

  // The current scope's slot in the frame's mark array: its static depth.
  void take_owned_mark(const peg::Ast& at) {
    auto d = static_cast<int32_t>(scopes_.size()) - 1;
    if (d >= kMaxOwnedDepth)
      reject(at, culebra::format("scopes nested deeper than {}", kMaxOwnedDepth));
    scopes_.back().owned_mark = d;
    chunk_.owned_depths = std::max(chunk_.owned_depths, d + 1);
    emit(Op::OwnedMark, d);
  }

  // The release ladder every scope exit uses (reverse order, mirroring the
  // JIT's frame ladder). Releases only the named slots: statement temps in
  // the range are owned by a live TempScope (or are a frame's return-value
  // slot), which release on their own paths.
  // The compile-time caller of the same release_order_by_rank the runtime
  // unwind uses (the ranks are final by the time any ladder is emitted, so
  // both answer the same order).
  std::vector<int32_t> release_order(int32_t lo, int32_t hi) const {
    return release_order_by_rank(slot_rank_, lo, hi);
  }

  void release_down_to(int32_t watermark) {
    for (int32_t s : release_order(watermark, next_slot_)) {
      // An open for-in closes its iterator at the rung that frees it, so a
      // `return` out of the body — and the loop's own exit — reach it with
      // the iteration's bindings, released by the rungs above, already gone.
      // The instruction is a no-op once the loop has disposed, so the two
      // ladders a `break` walks cannot double-close it.
      for (int32_t base : for_bases_)
        if (s == base + kForIter) emit(Op::ForDispose, base);
      if (slot_named_[s])
        emit(slot_cell_[s] ? Op::CellRelease : Op::Release, s);
    }
  }

  // Emits the scope's Releases and returns its slots to the allocator
  // (pinned cell slots stay allocated; their stale named flag only costs an
  // outer ladder a redundant nil-safe release). The scope also leaves behind
  // its cleanup segments — the same defers and the same ladder, for the
  // throw path — and those are what compile_try turns into catching ones.
  std::vector<size_t> pop_scope() {
    auto& sc = scopes_.back();
    record_slot_debug(sc);
    record_cleanup(sc, static_cast<uint32_t>(chunk_.code.size()));
    auto segments = std::move(sc.segments);
    if (scopes_.size() == 1) frame_segments_ = segments;  // the frame scope
    release_down_to(sc.slot_watermark);
    // After the ladder, so anything held only by this scope's bindings has
    // already died through the ordinary refcount-0 path (the JIT's
    // pop_scope order); what is left is escaped or cyclic.
    if (sc.owned_mark >= 0) emit(Op::OwnedExit, sc.owned_mark);
    next_slot_ = std::max(sc.slot_watermark, pin_floor_);
    named_top_ = std::min(named_top_, next_slot_);
    scopes_.pop_back();
    return segments;
  }

  const Binding* lookup(std::string_view name) const {
    return const_cast<Compiler*>(this)->lookup_mut(name);
  }

  // The same walk for the one caller that settles a binding it found by name
  // rather than by pre-declaration (compile_multifn_decl's grant).
  Binding* lookup_mut(std::string_view name) {
    for (auto sc = scopes_.rbegin(); sc != scopes_.rend(); ++sc)
      for (auto b = sc->bindings.rbegin(); b != sc->bindings.rend(); ++b)
        if (b->name == name) return &*b;
    return nullptr;
  }

  // Emit Releases for (and forget) the temps allocated after `base` — the
  // TempScope dtor body, callable mid-statement where the releases must
  // precede a branch (the while condition). Release is destructive, so a
  // Take'n temp releases as nil.
  void sweep_temps(size_t base, int32_t slot_base) {
    for (size_t i = stmt_temps_.size(); i > base; --i)
      emit(Op::Release, stmt_temps_[i - 1]);
    stmt_temps_.resize(base);
    next_slot_ = std::max({slot_base, named_top_, pin_floor_});
  }

  // Statement temps live until the end of the statement that made them.
  struct TempScope {
    Compiler& c;
    size_t base;
    int32_t slot_base;
    explicit TempScope(Compiler& c_)
        : c(c_), base(c_.stmt_temps_.size()), slot_base(c_.next_slot_) {}
    ~TempScope() { c.sweep_temps(base, slot_base); }
  };

  // Forget the consumed temp's most recent sweep-list entry — and only
  // that one. Scope rollback lets a later match arm reuse an earlier arm's
  // temp slot INDEX while the earlier arm's entry is still in the list, so
  // an erase-by-value would take the outer entry with it, shrink the list
  // below an inner TempScope's base, and the dtor's resize would then
  // zero-fill — emitting Releases of slot 0 (a live cell slot; segfaulted).
  // The most recent equal entry is always the one being consumed.
  void forget_temp(int32_t slot) {
    for (auto it = stmt_temps_.rbegin(); it != stmt_temps_.rend(); ++it) {
      if (*it == slot) {
        stmt_temps_.erase(std::next(it).base());
        return;
      }
    }
  }

  // Release dst's previous value, then either transfer the temp's +1 or
  // copy-and-retain the borrowed slot. The one store rule every write uses.
  // `dst_is_fresh` skips the Release for a slot known to hold nil (a
  // just-allocated slot, or an if-arm result written once per disjoint path).
  // A Take'n temp is dropped from the statement's sweep list — its +1 moved,
  // so the end-of-statement Release would be a provable no-op.
  void store_into(int32_t dst, ExprResult r, bool dst_is_fresh = false) {
    if (!r.owned && r.slot == dst) return;  // self-assign (`x = x`): the
                                            // Release-then-copy would read
                                            // back the just-nil'd slot
    if (!dst_is_fresh) emit(Op::Release, dst);
    if (r.owned) {
      emit(Op::Take, dst, r.slot);
      forget_temp(r.slot);
    } else {
      emit(Op::Move, dst, r.slot);
      emit(Op::Retain, dst);
    }
  }

  // The cell ops absorb an owned +1 from a register: hand them `r` as an
  // owned temp (retaining a borrowed slot into one first, the ArrayAppend
  // pattern), and drop a consumed temp from the sweep list like store_into.
  int32_t owned_src(const peg::Ast& at, ExprResult r) {
    if (!r.owned) {
      int32_t src = alloc_temp(at);
      emit(Op::Move, src, r.slot);
      emit(Op::Retain, src);
      return src;
    }
    forget_temp(r.slot);
    return r.slot;
  }

  // One `self.name = value` per declared field, declaration order, is_init
  // like the JIT's emit_object_set — an initializer that reached the property
  // through a method call is overwritten regardless of its mut flag. Emitted
  // either as the synthetic thunk's body or straight into a `new` body's
  // prologue (see fields_need_a_thunk); one function so the two cannot drift.
  void emit_declared_field_stores(const std::vector<const peg::Ast*>& fields) {
    for (const auto* f : fields) {
      TempScope fts(*this);
      auto mv = culebra::view_method(*f);
      ExprResult v = mv.value
                         ? compile_expr(*mv.value)
                         : ExprResult{emit_zero_value(*f, mv.type_annotation),
                                      true};
      emit(Op::ObjectSet, chunk_.self_slot, owned_src(*f, v),
           kconst_str(std::string(mv.name)), /*mut=*/1);
    }
  }

  // A typed field with no initializer takes its type's zero (the JIT's
  // emit_zero_value over the shared zero_kind_for_type table).
  int32_t emit_zero_value(const peg::Ast& at, std::string_view type) {
    int32_t t = alloc_temp(at);
    int32_t k = 0;
    switch (culebra::zero_kind_for_type(type)) {
      case culebra::ZeroKind::Float:
        k = kconst({TAG_FLOAT, _culebra_double_to_bits(0.0)});
        break;
      case culebra::ZeroKind::Bool: k = kconst({TAG_BOOL, 0}); break;
      case culebra::ZeroKind::String: k = kconst_str(""); break;
      case culebra::ZeroKind::Long: k = kconst_long(0); break;
      case culebra::ZeroKind::Nil: k = kconst({TAG_NIL, 0}); break;
    }
    emit(Op::LoadConst, t, k);
    return t;
  }

  // Declaration point of a captured local: a fresh cell absorbs the value.
  void store_new_cell(const peg::Ast& at, int32_t dst, ExprResult r) {
    emit(Op::CellNew, dst, owned_src(at, r));
  }

  // Every write into an existing cell — a reassignment, and every
  // declaration form that fills the cell a forward reference already
  // minted (`fn`, a decorated `fn`, a class, an enum).
  void store_cell(const peg::Ast& at, int32_t dst, ExprResult r) {
    invalidate_cell(dst);
    emit(Op::CellSet, dst, owned_src(at, r));
  }

  // A write to a cell a binding was resolved through invalidates what was
  // resolved: the closures already holding that cell now reach other code.
  // `CellNew` needs no such step — its slot is freshly allocated for a
  // binding that does not exist yet.
  void invalidate_cell(int32_t slot) {
    for (auto& sc : scopes_)
      for (auto& b : sc.bindings)
        if (b.slot == slot && (b.known.chunk >= 0 || b.known.ctor >= 0))
          revoke_known(b);
  }

  // A read of a cell binding: the value comes out retained, so the result
  // is an owned temp, unlike a plain slot's borrow. A lazy
  // dispatcher cell read before its decl ran guards for the unbound
  // sentinel (NameError at the reference, interp parity).
  // `unbound_guard=false` is the UFCS candidate load: a lazy dispatcher
  // cell still holding its sentinel must decline the gate like any other
  // non-Function (interp's env->has is false before the decl ran), not
  // raise the read's NameError — the JIT's optional-candidate rule.
  // The lazy cell this scope pre-declared for `name`, if the declaration
  // has not landed yet — the statement that binds the name fills that
  // cell rather than allocating its own (see predeclare_forward_refs).
  Binding* predeclared_here(const std::string& name) {
    for (auto& b : scopes_.back().bindings)
      if (b.lazy && b.name == name) return &b;
    return nullptr;
  }

  // The conditional pre-declaration this scope already holds for `name`.
  // An `if`/`cond` hoists what its arms declare, and collect_escaping_decls
  // recurses into nested arms — so an enclosing construct has already minted
  // the cell a nested one would, and an earlier construct in the same scope
  // has minted the one a later one would. The name is a single binding here,
  // and a later arm finds it, so both cases must reuse this cell rather than
  // mint a second one: minting again put the `CellNew` inside one arm,
  // leaving a sibling arm's read to walk the shadow chain into a cell that
  // path never allocated (a segfault), and made a second `if`'s bare declare
  // of the name a fresh binding instead of the reassignment interp reports.
  Binding* conditional_here(const std::string& name) {
    auto* b = predeclared_here(name);
    return b && b->conditional ? b : nullptr;
  }

  // A settled cell binding of `name` in this scope — what a re-declaration
  // writes through, since a closure may already hold that cell. Only a cell
  // this frame OWNS (slot_cell_) qualifies: a capture bound from the closure
  // reads as a cell too, but it belongs to the frame that made it, and a
  // declaration shadows such a name rather than writing the enclosing
  // binding — the JIT's rule that a declaration never reuses a borrowed
  // capture. Writing through it made `let sh = 7` visible as 8 outside a
  // `fn () { let sh = sh + 1 }` that ran.
  Binding* captured_here(const std::string& name) {
    for (auto& b : scopes_.back().bindings)
      if (!b.lazy && !b.session && b.is_cell && b.name == name &&
          b.slot < static_cast<int32_t>(slot_cell_.size()) &&
          slot_cell_[b.slot])
        return &b;
    return nullptr;
  }

  // A REPL line's own top level — where a declaration binds into the
  // session instead of this program's frame. A block or a function inside
  // the line scopes its locals the ordinary way, exactly as the interp does
  // with a nested Environment.
  bool repl_top() const { return repl_ && !in_function_ && scopes_.size() == 1; }

  // A slot holding the session's cell for `name`. Plain, not a cell slot:
  // the session owns the cell, so the scope's ladder must leave it alone —
  // the borrowed-capture treatment, where Release on a Long is a no-op.
  int32_t session_slot(const peg::Ast& at, std::string_view name) {
    int32_t slot = alloc_slot(at, std::string(name));
    emit(Op::ReplCell, slot, kconst_str(name));
    return slot;
  }

  // Refill a session binding's slot before it is used. The ReplCell that
  // fills it is emitted where the name is FIRST mentioned, which can be inside
  // a branch a later use skips — the binding is scope-wide by design, the
  // instruction is not. Idempotent: the session hands back the one cell it
  // owns. Skipped when it would repeat the instruction just emitted, which is
  // the common case of materializing a binding and reading it at once.
  void ensure_session_slot(const Binding& b) {
    if (!b.session) return;
    if (!chunk_.code.empty()) {
      const auto& last = chunk_.code.back();
      if (last.op == Op::ReplCell && last.a == b.slot) return;
    }
    emit(Op::ReplCell, b.slot, kconst_str(b.name));
  }

  // Register the session's binding for `name` in this scope, so every later
  // read, write and capture in the line goes through the one slot. Lazy: the
  // cell may still hold the unbound sentinel, and then the name means the
  // stdlib global of that name, or nothing at all (`read_shadowing` /
  // `UnboundErr` answer either way, at run time — which is the only time the
  // answer is known).
  Binding& bind_session(const peg::Ast& at, const std::string& name) {
    Binding b{name, session_slot(at, name), repl_session().is_mut(name),
              /*is_cell=*/true, /*lazy=*/true};
    b.shadowed_builtin = is_stdlib_global(name) || is_stdlib_namespace(name);
    b.session = true;
    return push_binding(std::move(b));
  }

  // An earlier input of the session declared `name` (the interp's environment
  // chain answering it).
  bool session_declared(std::string_view name) const {
    return repl_ && repl_session().declared(name);
  }

  // Whether a bare write to `name`, bound by nothing in scope, is the
  // session's: always at a line's top level (Op::ReplBind decides
  // declare-or-reassign when the write runs); in a block or function only
  // when an earlier input declared it — otherwise it declares a local, as
  // in a script.
  bool session_owns(std::string_view name) const {
    return repl_top() || session_declared(name);
  }

  // The binding a bare write to `name` lands in: what is in scope, else the
  // session's cell when it owns the name. Null for `self` and the stdlib
  // globals (bound already, nothing here to write) — and, off the REPL, for
  // any name nothing binds.
  const Binding* lookup_or_session(const peg::Ast& at, const std::string& name) {
    if (const Binding* b = lookup(name)) return b;
    return session_owns(name) ? &bind_session(at, name) : nullptr;
  }

  // Record what a declaration just bound. Only session bindings need it:
  // a frame binding's mutability is settled by the `let` / `mut` the
  // compiler is looking at, while a session name outlives this line and may
  // have been declared the other way by an earlier one.
  void emit_session_decl_bind(const Binding& b, bool is_mut) {
    if (!b.session) return;
    emit(Op::ReplBind,
         static_cast<int32_t>(is_mut ? ReplBindMode::Mut : ReplBindMode::Let),
         kconst_str(b.name));
  }

  // Whether a bare `x = v` on this name declares rather than reassigns —
  // interp's assign_name, where a name the environment chain cannot answer is
  // bound by the write. `self` is bound in every frame there, and a stdlib
  // global is an ordinary immutable binding of the root env, so both are
  // reassignments (and refuse).
  bool declares_implicitly(const std::string& name) {
    if (session_owns(name)) return false;
    // A cell pre-declared for this very write (a closure above captured the
    // name ahead of it): the write is still the declaration, so it fills the
    // cell rather than being mut-checked against the placeholder.
    // A conditional one cannot answer here at all: whether an arm already
    // declared the name is this call's fact, so emit_rebind asks the cell
    // when the write runs.
    if (Binding* pre = predeclared_here(name))
      return !pre->conditional && pre->awaits_implicit;
    if (lookup(name)) return false;
    return name != "self" && !is_stdlib_namespace(name) &&
           !is_stdlib_global(name);
  }

  // The mutability check and the store behind `x = v` on a binding already
  // in scope. Returns false when the check is a compile-time one and already
  // lost, so the caller knows its own trailing code is unreachable.
  bool emit_rebind(const peg::Ast& at, const Binding& b, ExprResult r) {
    ensure_session_slot(b);
    if (b.session) {
      emit(Op::ReplBind, static_cast<int32_t>(ReplBindMode::Assign),
           kconst_str(b.name), b.shadowed_builtin ? 1 : 0);
    } else if (b.conditional && b.mut_slot >= 0) {
      return emit_conditional_rebind(at, b, r);
    } else if (!b.is_mut) {
      emit(Op::ImmutErr, kconst_str(b.name));
      return false;
    }
    if (b.is_cell) store_cell(at, b.slot, r);
    else store_into(b.slot, r);
    return true;
  }

  // A bare write to a conditionally-declared name, where "is this the
  // declaration or a reassignment" is a runtime fact: only one arm runs, so
  // the sentinel still in the cell means no declaration has landed and this
  // write is one (immutably — a bare declare never carries `mut`), while a
  // value there means the arm that ran already bound the name and this write
  // is checked against the mutability THAT arm wrote.
  bool emit_conditional_rebind(const peg::Ast& at, const Binding& b,
                               ExprResult r) {
    // The probe belongs to the enclosing statement's temps, as
    // assign_shadowing's does: a scope of its own here would roll the slot
    // number back, and the read this assignment returns would take the same
    // number and overwrite the probe's `+1` instead of releasing it.
    int32_t probe = alloc_temp(at);
    emit(Op::CellGet, probe, b.slot);
    size_t to_declare = emit(Op::JumpIfTag, probe, 0, TAG_NO_SELF);
    // Reassignment: the declaration's own `mut` decides.
    size_t to_immut = emit(Op::JumpIfFalse, b.mut_slot);
    store_cell(at, b.slot, r);
    size_t to_join = emit(Op::Jump, 0);
    patch_to_here(to_immut);
    emit(Op::ImmutErr, kconst_str(b.name));
    // Declaration: fill the cell and record that it was not a `mut` one.
    patch_to_here(to_declare);
    store_cell(at, b.slot, r);
    emit(Op::LoadConst, b.mut_slot, kconst({TAG_BOOL, 0}));
    patch_to_here(to_join);
    return true;
  }

  // Read a forward-ref pre-declaration that shadows an outer binding:
  // while the cell still holds the unbound sentinel the declaration has
  // not run, so the name means the shadowed binding — what the interp's
  // environment chain answers.
  ExprResult read_shadowing(const peg::Ast& at, const Binding& b) {
    int32_t out = alloc_temp(at);
    emit(Op::CellGet, out, b.slot);
    size_t to_outer = emit(Op::JumpIfTag, out, 0, TAG_NO_SELF);
    size_t to_join = emit(Op::Jump, 0);
    patch_to_here(to_outer);
    if (b.shadowed) {
      store_into(out, read_binding(at, *b.shadowed));
    } else {
      emit(Op::Release, out);
      emit(Op::NsGet, out, kconst_str(b.name));
    }
    patch_to_here(to_join);
    return {out, true};
  }

  // Write half of read_shadowing: the sentinel still in the cell means
  // the declaration has not run, so the assignment goes to the shadowed
  // binding under that binding's `mut`; afterwards to the cell under its
  // own. Only one arm ever executes, so both may consume the RHS temp.
  ExprResult assign_shadowing(const peg::Ast& ast, const peg::Ast& tgt,
                              const Binding& b, ExprResult r) {
    int32_t probe = alloc_temp(tgt);
    emit(Op::CellGet, probe, b.slot);
    size_t to_outer = emit(Op::JumpIfTag, probe, 0, TAG_NO_SELF);
    if (!b.is_mut) {
      StampGuard pos(*this, ast);
      emit(Op::ImmutErr, kconst_str(b.name));
    } else {
      store_cell(tgt, b.slot, r);
    }
    size_t to_join = emit(Op::Jump, 0);
    patch_to_here(to_outer);
    if (!b.shadowed->is_mut) {
      StampGuard pos(*this, ast);
      emit(Op::ImmutErr, kconst_str(b.name));
    } else if (b.shadowed->is_cell) {
      store_cell(tgt, b.shadowed->slot, r);
    } else {
      store_into(b.shadowed->slot, r);
    }
    patch_to_here(to_join);
    return read_binding(tgt, b);
  }

  ExprResult read_binding(const peg::Ast& at, const Binding& b,
                          bool unbound_guard = true) {
    ensure_session_slot(b);
    if (b.lazy && unbound_guard && (b.shadowed || b.shadowed_builtin))
      return read_shadowing(at, b);
    if (!b.is_cell) {
      // A plain slot can carry the sentinel too: a frame that captured `self`
      // where no enclosing frame had a receiver holds one until a call
      // supplies a dynamic one, and reading it there is the interp's
      // NameError.
      if (b.lazy && unbound_guard)
        emit(Op::UnboundErr, b.slot, kconst_str(b.name));
      return {b.slot, false};
    }
    int32_t t = alloc_temp(at);
    emit(Op::CellGet, t, b.slot);
    if (b.lazy && unbound_guard) emit(Op::UnboundErr, t, kconst_str(b.name));
    return {t, true};
  }

  // The same read for a callee, without the copy. A call borrows what it
  // calls (the register's `+1` is never handed over), so a name whose value
  // cannot change while the call runs needs no register of its own: the Call
  // reads the cell itself, and the retain — plus the release that pays for
  // it at the end of the statement — are both gone. The guard the lazy read
  // owes still runs, and at the name's own position; it just asks the cell.
  //
  // What makes it safe is the cell, not the value. Two halves:
  //   - nothing writes it under the call. `binding_writes_once` leaves only
  //     the declaration as a writer (store_cell, whose invalidate_cell is
  //     the same revocation the resolved-callee grants rely on), and a
  //     declaration is a statement of this frame — which is blocked inside
  //     the call it would have to interleave with.
  //   - nothing frees it under the call: the cell is released by the ladder
  //     of the frame that owns the slot, and that is this same frame.
  // A CAPTURE has the first half but not the second — its cell belongs to
  // the running closure, and nothing here says the closure outlives the call
  // — so the test is `slot_cell_`, not `is_cell` alone. Exec::BorrowWitness
  // checks the claim in the assert lanes.
  const Binding* borrowed_call_head(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.size() < 2 || ast.nodes[0]->tag != "IDENTIFIER"_)
      return nullptr;
    const auto& post = *ast.nodes[1];
    if (post.original_tag != "ARGUMENTS"_ || has_kwargs(post)) return nullptr;
    const Binding* b = lookup(ast.nodes[0]->token);
    if (!b || !b->is_cell || b->shadowed || !binding_writes_once(*b))
      return nullptr;
    if (b->slot >= static_cast<int32_t>(slot_cell_.size()) ||
        !slot_cell_[b->slot])
      return nullptr;
    return b;
  }

  ExprResult read_borrowed_head(const peg::Ast& at, const Binding& b) {
    if (b.lazy) {
      StampGuard pos(*this, at);
      emit(Op::UnboundErr, b.slot, kconst_str(b.name), /*in_cell=*/1);
    }
    return {b.slot, false};
  }

  // Scope-level defer mark, established around a block whose scope declares
  // its own defers (scan_eh_defer's scope_has_defer, keyed by `key` — the
  // block node for loop bodies, the LEXICAL_SCOPE node for `{ ... }`). The
  // mark slot lives in the ENCLOSING scope (it must survive this scope's
  // release ladder); DeferMark re-executes per entry, so one slot serves a
  // loop body's every iteration. Exits run before the scope's releases —
  // fall-through here, break/continue via defer_scopes_, a throw via the
  // region handler's mark (compile_try) or the frame's (run_frame).
  struct DeferScope {
    Compiler& c;
    int32_t mark = -1;
    DeferScope(Compiler& c_, const peg::Ast& key) : c(c_) {
      if (c.analysis_.scope_has_defer.contains(&key)) {
        mark = c.alloc_slot(key, "(defer.mark)");
        c.emit(Op::DeferMark, mark);
        c.defer_scopes_.push_back(mark);
        c.pending_scope_mark_ = mark;  // the scope this brackets adopts it
      }
    }
    // Emit the fall-through run; pop before the caller's pop_scope so the
    // releases that follow are no longer "inside" this defer scope.
    void close() {
      if (mark < 0) return;
      c.emit(Op::DeferRunTo, mark);
      c.defer_scopes_.pop_back();
      mark = -1;
    }
    ~DeferScope() {
      if (mark >= 0) c.defer_scopes_.pop_back();  // reject-unwind path
    }
  };

  void compile_block(const peg::Ast& ast, const peg::Ast* defer_key = nullptr) {
    using namespace peg::udl;
    DeferScope ds(*this, defer_key ? *defer_key : ast);
    push_scope(ast);
    predeclare_forward_refs(ast);
    if (ast.tag == "STATEMENTS"_) {
      for (const auto& n : ast.nodes) compile_statement(*n);
    } else {
      compile_statement(ast);
    }
    ds.close();
    pop_scope();
  }

  // A block in value position: statements run normally, and the last one's
  // value lands in `dst` (nil for the valueless statements) before the block
  // scope's bindings are released. Every path writes `dst` at most once and
  // arrives with it still nil, so the stores skip the pre-Release.
  void compile_block_into(const peg::Ast& ast, int32_t dst,
                          const peg::Ast* defer_key = nullptr) {
    using namespace peg::udl;
    DeferScope ds(*this, defer_key ? *defer_key : ast);
    push_scope(ast);
    predeclare_forward_refs(ast);
    compile_body_into(ast, dst);
    ds.close();
    pop_scope();
  }

  // An `if` / `cond` arm body in value position. It gets no scope of its own:
  // the enclosing one holds what it declares (collect_escaping_decls pinned
  // those names to lazy cells first) and owns any `defer` it registers, which
  // is the interpreter's eval_if / eval_cond — and its DeferHandoff — exactly.
  void compile_arm_into(const peg::Ast& body, int32_t dst) {
    using namespace peg::udl;
    if (body.tag == "STATEMENTS"_) {
      TempScope ts(*this);
      compile_body_into(body, dst);
      return;
    }
    compile_value_into(body, dst);
  }

  // compile_block_into's core, scope management left to the caller —
  // compile_try brackets it with the region's own mark/end placement.
  void compile_body_into(const peg::Ast& ast, int32_t dst) {
    using namespace peg::udl;
    if (ast.tag == "STATEMENTS"_) {
      for (size_t i = 0; i + 1 < ast.nodes.size(); i++)
        compile_statement(*ast.nodes[i]);
      if (!ast.nodes.empty()) compile_value_into(*ast.nodes.back(), dst);
    } else {
      compile_value_into(ast, dst);
    }
  }

  // One statement in value position. FOR/WHILE evaluate to nil; BREAK /
  // CONTINUE jump away before `dst` is ever read, so they store nothing.
  void compile_value_into(const peg::Ast& ast, int32_t dst) {
    using namespace peg::udl;
    stamp(ast);
    emit_dbg_stmt(ast);
    TempScope ts(*this);
    switch (ast.tag) {
      case "STATEMENTS"_:
        compile_block_into(ast, dst);
        break;
      case "FOR"_:
      case "WHILE"_:
      case "BREAK"_:
      case "CONTINUE"_:
      case "RETURN"_:
      case "LEXICAL_SCOPE"_:
      case "DEFER"_:
      case "MULTIFN_DECL"_:  // a decl evaluates to nil (interp parity)
      case "CLASS_DECL"_:
      case "ENUM_DECL"_:
      case "TRAIT_DECL"_:
      case "DEBUGGER"_:
        compile_statement_inner(ast);
        break;
      case "ASSIGNMENT"_:
        store_into(dst, compile_assignment(ast), /*dst_is_fresh=*/true);
        break;
      default:
        store_into(dst, compile_expr(ast), /*dst_is_fresh=*/true);
        break;
    }
  }

  // The statement boundary a debugger stops at — the interpreter's
  // statement_boundary, in instruction form. Only statements written in the
  // entry module carry one: the stdlib prologues compile into this same
  // chunk, and a debugger stops in user source (the interp's hook skips its
  // `<builtin>` path for the same reason). `debugger` breaks under plain
  // `--debug` too, which is all that mode emits.
  void emit_dbg_stmt(const peg::Ast& ast) {
    using namespace peg::udl;
    if (debug_ == Debug::Off || ast.tag == "STATEMENTS"_) return;
    bool force = ast.tag == "DEBUGGER"_;
    if (!force && debug_ != Debug::Step) return;
    if (ast.path != prog_.source_path) return;
    emit(Op::DbgStmt, force ? 1 : 0);
    chunk_.has_dbg = true;
  }

  void compile_statement(const peg::Ast& ast) {
    stamp(ast);
    emit_dbg_stmt(ast);
    TempScope ts(*this);
    compile_statement_inner(ast);
  }

  void compile_statement_inner(const peg::Ast& ast) {
    using namespace peg::udl;
    switch (ast.tag) {
      case "STATEMENTS"_:
        // A statement list in STATEMENT position is a transform's expansion
        // of one statement into several (the effects decl lowers to a pair),
        // not a block: what it declares belongs to the enclosing scope, the
        // way the one statement it replaced would have. A real `{ … }` is a
        // LEXICAL_SCOPE and opens its own scope below.
        predeclare_forward_refs(ast);
        for (const auto& n : ast.nodes) compile_statement(*n);
        break;
      case "ASSIGNMENT"_:
        compile_assignment(ast);
        break;
      case "FOR"_:
        compile_for(ast);
        break;
      case "WHILE"_:
        compile_while(ast);
        break;
      case "LEXICAL_SCOPE"_:
        // `{ ... }` statement: its own scope, and its own defer scope
        // (scan_eh_defer keys the LEXICAL_SCOPE node; the child is the
        // STATEMENTS list, or a lone collapsed statement).
        compile_block(*ast.nodes[0], /*defer_key=*/&ast);
        break;
      case "DEBUGGER"_:
        // The break itself is emit_dbg_stmt's, at the head of the statement:
        // one boundary, forced. Without `--debug` the statement compiles to
        // nothing at all, which is what it does on the other two backends.
        break;
      case "IMPORT_STMT"_:
        compile_import(ast);
        break;
      case "EXPORT_STMT"_:
        // Nothing at the point it is written: emit_module_export walks the
        // module's statements once its body is done, so the whole binding
        // dance lives in one place (the interp's eval, the JIT's
        // emit_build_and_register_export).
        break;
      case "DEFER"_: {
        // `defer { ... }` — the body becomes a 0-arity chunk (the same
        // closure machinery as a fn literal, captures included), pushed
        // onto the global defer stack. DeferPush borrows; the statement
        // temp's sweep drops our +1 (compile_defer's push-then-drop).
        int32_t t = alloc_temp(ast);
        emit(Op::MakeClosure, t, compile_defer_chunk(ast));
        emit(Op::DeferPush, t);
        break;
      }
      case "MULTIFN_DECL"_:
        compile_multifn_decl(ast);
        break;
      case "CLASS_DECL"_:
        compile_class_decl(ast);
        break;
      case "ENUM_DECL"_:
        compile_enum_decl(ast);
        break;
      case "TRAIT_DECL"_:
        compile_trait_decl(ast);
        break;
      case "BREAK"_:
      case "CONTINUE"_: {
        bool brk = ast.tag == "BREAK"_;
        if (loops_.empty())
          reject(ast, brk ? "break outside a loop" : "continue outside a loop");
        auto& lc = loops_.back();
        // A break/continue jumps out mid-statement: the enclosing
        // statements' in-flight temps still hold values (interp frees them
        // as the signal unwinds the eval frames). Release nils them, which
        // both frees a heap temp and keeps a later generation of the slot
        // honest — a stale scalar left behind reads as a cell pointer once
        // the slot is reused as a loop variable's cell (CellNew releases
        // the previous generation blindly). Only this iteration's temps are
        // ours; an enclosing statement below the loop still needs its own.
        for (int32_t t : stmt_temps_)
          if (t >= lc.slot_watermark) emit(Op::Release, t);
        // Then the pending defers — everything above the outermost defer
        // scope opened inside the loop — before the named-slot releases
        // (interp unwinds scope by scope, defers first).
        if (defer_scopes_.size() > lc.defer_watermark)
          emit(Op::DeferRunTo, defer_scopes_[lc.defer_watermark]);
        release_down_to(lc.slot_watermark);
        // The body scope's mark bounds every scope this jump abandons.
        if (lc.body_scope_index < scopes_.size() &&
            scopes_[lc.body_scope_index].owned_mark >= 0)
          emit(Op::OwnedExit, scopes_[lc.body_scope_index].owned_mark);
        if (brk && lc.broke_slot >= 0)
          emit(Op::LoadConst, lc.broke_slot, kconst_long(1));
        (brk ? lc.break_jumps : lc.continue_jumps).push_back(emit(Op::Jump));
        break;
      }
      case "RETURN"_: {
        if (!in_function_) reject(ast, "return outside a function");
        // The return value survives its statement's temp sweep in a
        // dedicated slot (the while-condition pattern), then every named
        // slot of the frame releases before the value leaves — a return
        // from any depth tears the whole frame scope stack down.
        int32_t rv = alloc_temp(ast);
        {
          size_t base = stmt_temps_.size();
          int32_t slot_base = next_slot_;
          if (!ast.nodes.empty())
            store_into(rv, compile_expr(*ast.nodes[0]), /*dst_is_fresh=*/true);
          else
            emit(Op::LoadConst, rv, kconst({TAG_NIL, 0}));
          sweep_temps(base, slot_base);
        }
        emit_return_type_check(rv);
        // In-flight temps of enclosing statements (a match subject held
        // across arms, a half-built array) still hold their +1s; the frame
        // is exiting, so release them here like break/continue do instead
        // of leaving them to the GC backstop. `rv` rides past.
        for (int32_t t : stmt_temps_)
          if (t != rv) emit(Op::Release, t);
        // Run every still-pending defer of the frame before its slots
        // release (the fn.mark bounds them all — nested scope marks sit
        // above it, so one run covers a return from any depth).
        if (frame_defer_mark_ >= 0) emit(Op::DeferRunTo, frame_defer_mark_);
        release_down_to(0);
        // One resolution for every scope the return abandons: the frame's
        // mark is the lowest of them (owned ids are monotonic).
        if (chunk_.owned_frame_depth >= 0)
          emit(Op::OwnedExit, chunk_.owned_frame_depth);
        emit(Op::Ret, rv);
        break;
      }
      default:
        compile_expr(ast);  // expression statement; temps swept by the caller
        break;
    }
  }

  // The declared return type's check, at every exit the frame has: ahead of
  // the defers and the scope teardown, where the JIT emits it. No-op for a
  // function without one.
  void emit_return_type_check(int32_t rv) {
    if (ret_type_ < 0) return;
    if (ret_ctx_ < 0) ret_ctx_ = kconst_str("return value");
    std::vector<size_t> skip;
    emit_type_check_gate(ret_type_str_, rv, skip);
    emit(Op::ChkTypeAt, rv, ret_type_, ret_ctx_, ret_pos_slot_);
    for (size_t ix : skip) patch_to_here(ix);
  }

  // Frame-level defer mark (the JIT's fn.mark, gated on has_any_defer):
  // allocated and taken as the chunk's FIRST instruction, so the executor's
  // catch-all / the lowering's frame pad can trust the slot at any pc.
  void establish_frame_defer_mark(const peg::Ast& at, const FuncInfo& info) {
    if (!info.has_any_defer) return;
    frame_defer_mark_ = alloc_slot(at, "(defer.mark)");
    chunk_.defer_mark_slot = frame_defer_mark_;
    emit(Op::DeferMark, frame_defer_mark_);
  }

  // Scope-entry pre-declaration of `fn name` dispatcher cells: every name a
  // MULTIFN_DECL in this statement list declares gets an owned cell holding
  // the unbound sentinel before any statement runs, so a closure built
  // earlier in the list captures a real cell (mutual recursion resolves)
  // and a read before the decl statement ran raises NameError through the
  // binding's lazy guard. One reused temp feeds every CellNew, so the only
  // lasting slots are the pinned cells themselves.
  // Every name a function literal inside this subtree closes over. The
  // analysis already computed each literal's free_vars, so this is just a
  // union over the nodes it keyed.
  void collect_literal_frees(const peg::Ast& node,
                             std::set<std::string>& out) const {
    if (auto it = analysis_.func_info.find(&node);
        it != analysis_.func_info.end()) {
      out.insert(it->second.free_vars.begin(), it->second.free_vars.end());
    }
    for (const auto& c : node.nodes) collect_literal_frees(*c, out);
  }

  // One name a statement list will declare, and how. `implicit` marks a bare
  // `x = v` site: that statement IS the declaration, so the write must fill
  // the cell rather than be checked against it.
  struct Decl {
    const peg::Ast* at;
    std::string name;
    bool is_mut;
    bool implicit;
  };
  using DeclList = std::vector<Decl>;

  static void add_decl(DeclList& decls, const peg::Ast* at, std::string name,
                       bool is_mut, bool implicit = false) {
    for (const auto& d : decls)
      if (d.name == name) return;  // overloads share the first decl's cell
    decls.push_back({at, std::move(name), is_mut, implicit});
  }

  // Mint one lazy cell per name and register it in the current scope,
  // shadowing whatever the name meant here. A read before the declaration
  // runs finds the sentinel and falls through to the shadowed binding (or to
  // the NameError); the declaration statement fills the cell it finds rather
  // than minting its own (predeclared_here).
  void predeclare_cells(const peg::Ast& ast, const DeclList& decls,
                        bool conditional = false) {
    if (decls.empty()) return;
    // What each name means until its declaration runs, captured before
    // any of the new bindings shadow it.
    std::vector<std::shared_ptr<Binding>> shadowed;
    std::vector<bool> shadowed_builtin;
    for (const auto& d : decls) {
      const Binding* prev = lookup(d.name);
      shadowed.push_back(prev ? std::make_shared<Binding>(*prev) : nullptr);
      shadowed_builtin.push_back(!prev && is_stdlib_global(d.name));
    }
    // At the REPL's top level the cell is the session's and already holds
    // whatever an earlier line put in the name, so a pre-declaration does not
    // blank it: until this line's declaration runs the name still means what
    // it meant, which is what the read guards below would have said anyway.
    if (repl_top()) {
      for (size_t k = 0; k < decls.size(); ++k) {
        Binding& b = bind_session(*decls[k].at, decls[k].name);
        b.is_mut = decls[k].is_mut;
        b.shadowed = shadowed[k];
        b.conditional = conditional;
      }
      return;
    }
    TempScope ts(*this);
    std::vector<int32_t> cslots;
    for (const auto& d : decls)
      cslots.push_back(alloc_cell_slot(*d.at, d.name));
    // A conditional binding's mutability is decided when a declaration runs,
    // so it needs a slot to say so in (Binding::mut_slot). Taken before the
    // shared temp below so it outlives the statement, like the cells.
    std::vector<int32_t> mslots;
    if (conditional)
      for (const auto& d : decls)
        mslots.push_back(alloc_slot(*d.at, "(" + d.name + ".decl_mut)"));
    int32_t tmp = alloc_temp(ast);
    for (size_t k = 0; k < decls.size(); ++k) {
      emit(Op::LoadConst, tmp, kconst({TAG_NO_SELF, 0}));
      emit(Op::CellNew, cslots[k], tmp);  // nils tmp; reloaded per name
      Binding b{decls[k].name, cslots[k], decls[k].is_mut, /*is_cell=*/true,
                /*lazy=*/true};
      b.shadowed = shadowed[k];
      b.shadowed_builtin = shadowed_builtin[k];
      b.conditional = conditional;
      b.awaits_implicit = decls[k].implicit;
      if (conditional) {
        b.mut_slot = mslots[k];
        emit(Op::LoadConst, b.mut_slot, kconst({TAG_BOOL, 0}));
      }
      push_binding(std::move(b));
    }
  }

  // predeclare_cells for an `if`/`cond` hoist: the names this scope already
  // pre-declared conditionally keep the cell they have (conditional_here).
  void predeclare_conditional_cells(const peg::Ast& ast,
                                    const DeclList& decls) {
    DeclList fresh;
    for (const auto& d : decls)
      if (!conditional_here(d.name)) fresh.push_back(d);
    predeclare_cells(ast, fresh, /*conditional=*/true);
  }

  // A declaration filling a pre-declared cell settles the binding — unless
  // the pre-declaration was a conditional one, which stays lazy forever.
  static void settle_predeclared(Binding& pre) {
    if (pre.conditional) return;
    pre.lazy = false;
    pre.shadowed.reset();
    pre.shadowed_builtin = false;
  }

  // What an `if` / `cond` arm body declares into the scope AROUND it. Neither
  // construct opens a scope for its arms — interp's eval_if and eval_cond run
  // the body in the enclosing environment (run_loop_body guards for a block;
  // these do not) — so a `let` there declares outside, on every backend,
  // whatever the statement count. A nested arm passes its own declarations up
  // the same way, which is why this recurses; a `{ ... }` statement inside one
  // is a LEXICAL_SCOPE and keeps its own.
  void collect_escaping_decls(const peg::Ast& node, DeclList& decls) {
    using namespace peg::udl;
    if (node.tag == "STATEMENTS"_) {
      for (const auto& n : node.nodes) collect_escaping_decls(*n, decls);
      return;
    }
    if (node.tag == "COND"_) {
      for (const auto& arm : node.nodes)
        collect_escaping_decls(*arm->nodes[1], decls);
      return;
    }
    if (node.tag == "IF"_) {
      auto iv = culebra::view_if(node);
      // An init clause opens a scope of its own around the whole `if`, so
      // nothing inside can reach past it.
      if (iv.init) return;
      // compile_if's own walk: (cond, body) pairs, then a trailing else.
      size_t i = iv.arm_off;
      for (; i + 1 < node.nodes.size(); i += 2)
        collect_escaping_decls(*node.nodes[i + 1], decls);
      if (i < node.nodes.size()) collect_escaping_decls(*node.nodes[i], decls);
      return;
    }
    if (node.tag == "MULTIFN_DECL"_ || node.tag == "CLASS_DECL"_ ||
        node.tag == "ENUM_DECL"_) {
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) i++;
      add_decl(decls, node.nodes[i].get(),
               std::string(culebra::parse_generic_head(node.nodes[i]->token).outer),
               /*is_mut=*/false);
      return;
    }
    if (node.tag == "DESTRUCTURE_ASSIGN"_) {
      // `let [a, b] = …`: every leaf it binds escapes too, each anchored at
      // the statement (compile_destructure_assign's own reading of the node).
      bool is_mut = node.nodes[1]->token == "mut";
      if (node.nodes[0]->token != "let" && !is_mut) return;
      culebra::for_each_pattern_binding(
          *node.nodes[2], [&](std::string_view name, size_t, size_t) {
            add_decl(decls, &node, std::string(name), is_mut);
          });
      return;
    }
    if (node.tag != "ASSIGNMENT"_) return;
    auto av = culebra::view_assignment(node);
    if (av.compound) return;
    if (const auto* target = culebra::assign_name_target(node, av))
      if (!culebra::is_sink_name(target->token)) {
        bool implicit = !av.is_let && !av.is_mut;
        // A bare write declares only where nothing already answers the name.
        if (implicit && !declares_implicitly(std::string(target->token))) return;
        add_decl(decls, target, std::string(target->token), av.is_mut, implicit);
      }
  }

  void predeclare_forward_refs(const peg::Ast& ast) {
    using namespace peg::udl;
    // `fn name` always pre-declares (its dispatcher cell is how self- and
    // mutual recursion resolve); every other declaration only when a
    // nested closure in this list captures the name.
    DeclList decls;
    auto add = [&](const peg::Ast* at, std::string name, bool is_mut,
                   bool implicit = false) {
      add_decl(decls, at, std::move(name), is_mut, implicit);
    };
    // `referenced` grows with the free vars of every function literal
    // already passed, so a declaration is pre-declared only when its name
    // is genuinely referred to ahead of it. Widening it to every captured
    // local would move the cell's slot — and with it the release ladder's
    // position — ahead of locals declared earlier, reordering their drops.
    std::set<std::string> referenced;
    // How many `fn name` declarations this list has per name — the count,
    // not the set, since one is the interesting number (Scope::sole_multifn).
    // A decorated declaration counts too: it binds what its decorators
    // returned, so a name that has one is not a plain dispatcher.
    std::map<std::string, int> multifn_count;
    auto handle = [&](const peg::Ast& node) {
      std::set<std::string> here;
      collect_literal_frees(node, here);
      auto forward_ref = [&](const std::string& name) {
        return referenced.contains(name) || here.contains(name);
      };
      if (node.tag == "MULTIFN_DECL"_ || node.tag == "CLASS_DECL"_ ||
          node.tag == "ENUM_DECL"_) {
        size_t i = 0;
        while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) i++;
        auto name = std::string(
            culebra::parse_generic_head(node.nodes[i]->token).outer);
        // `fn name` always pre-declares: its dispatcher cell is how self-
        // and mutual recursion resolve, and a dispatcher has no drop.
        if (node.tag == "MULTIFN_DECL"_) multifn_count[name]++;
        if (node.tag == "MULTIFN_DECL"_ ||
            (info_->captured_locals.contains(name) && forward_ref(name))) {
          add(node.nodes[i].get(), std::move(name), /*is_mut=*/false);
        }
        referenced.insert(here.begin(), here.end());
        return;
      }
      if (node.tag != "ASSIGNMENT"_) {
        referenced.insert(here.begin(), here.end());
        return;
      }
      auto av = culebra::view_assignment(node);
      const auto* target =
          av.compound ? nullptr : culebra::assign_name_target(node, av);
      if (target) {
        auto name = std::string(target->token);
        // A bare `x = ...` declares only where nothing named `x` is in
        // scope; otherwise the statement reassigns, and pre-declaring
        // here would shadow what it means to write.
        bool implicit = !av.is_let && !av.is_mut;
        if (!culebra::is_sink_name(name) &&
            info_->captured_locals.contains(name) && forward_ref(name) &&
            (!implicit || !lookup(name))) {
          add(target, std::move(name), av.is_mut, implicit);
        }
      }
      referenced.insert(here.begin(), here.end());
    };
    if (ast.tag == "STATEMENTS"_) {
      // One level of list nesting, like the JIT's pre-pass: a `{ ... }`
      // block statement carries its list one child down.
      for (const auto& n : ast.nodes) {
        if (n->tag == "STATEMENTS"_) {
          for (const auto& inner : n->nodes) handle(*inner);
        } else {
          handle(*n);
        }
      }
    } else {
      handle(ast);
    }
    for (const auto& [name, n] : multifn_count)
      if (n == 1) scopes_.back().sole_multifn.insert(name);
    predeclare_cells(ast, decls);
  }

  // `fn name(params) { body }` — a free named-function declaration. The
  // slice registers arity-dispatch overloads through the same runtime
  // multimethod registry the JIT uses (multifn_register_and_install):
  // same-scope overloads merge into one dispatcher, a nested-scope decl —
  // or another activation of this one — gets its own, a same-arity re-decl
  // replaces its table entry, and DispatchError kind/message/position come
  // from the shared dispatcher thunk. Installing stores the dispatcher
  // into the cell predeclare_forward_refs created, flipping it from the
  // unbound sentinel.
  void compile_multifn_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    size_t dec_end = 0;
    while (dec_end < ast.nodes.size() &&
           ast.nodes[dec_end]->tag == "DECORATOR"_)
      dec_end++;
    auto head = culebra::parse_generic_head(ast.nodes[dec_end]->token);
    auto name = std::string(head.outer);
    // `fn id<T>(x: T)`: the declaration's own Generic parameters, which its
    // annotations lower against the way a Generic class's members lower
    // against the class's. The name is the outer half — the type parameters
    // are erased everywhere else, so two arities of `id` still overload.
    std::vector<std::string_view> type_params;
    if (!head.args.empty()) type_params = culebra::split_generic_args(head.args);
    MemberOpts mo;
    if (!type_params.empty()) mo.type_params = &type_params;
    // At the REPL the session IS the enclosing scope: a `fn f(a)` line
    // appends to the dispatcher an earlier `fn f()` installed, so the name
    // is not the sole declaration of its dispatcher however this line reads.
    // Settled before the body compiles, because the body's own recursive
    // calls stand on the same answer (compile_fn_chunk's own-name block).
    Binding* b = lookup_mut(name);
    bool session_overload =
        b && b->session && repl_session().value(name).tag == TAG_FUNC;
    mo.sole_multifn =
        scopes_.back().sole_multifn.contains(name) && !session_overload;
    if (dec_end > 0) {
      // A decorated declaration binds what the decorators return, so it
      // never reaches the multimethod registry: the name holds the
      // outermost wrapper, and the innermost decorator is applied first.
      compile_decorated_fn(ast, dec_end, name, mo);
      return;
    }
    int32_t idx = compile_fn_chunk(ast, ast.nodes[1].get(),
                                   *ast.nodes.back(), mo);
    prog_.chunks[idx].multifn_name = name;
    int32_t cls = alloc_temp(ast);
    emit(Op::MakeClosure, cls, idx);
    if (!b || !b->is_cell)
      reject(ast, culebra::format("fn '{}' declared here", name));
    // A same-scope overload appends to the dispatcher the binding already
    // holds (the binding's scope is the current one — predeclare ran at its
    // entry); the first decl passes none and mints a fresh one. The session
    // overload above is that same append one REPL line later (probed); the
    // registry sorts out a value that is not one of its dispatchers, so the
    // test is only that the name holds a function at all.
    int32_t into = -1;
    if (scopes_.back().multifn_decls.contains(name) || session_overload) {
      into = alloc_temp(ast);
      emit(Op::CellGet, into, b->slot);
    }
    scopes_.back().multifn_decls.insert(name);
    int32_t t = alloc_temp(ast);
    emit(Op::MultifnReg, t, cls, into, idx);
    forget_temp(cls);  // the registry absorbed the body's +1 (reg is nil)
    store_cell(ast, b->slot, {t, true});
    emit_session_decl_bind(*b, /*is_mut=*/false);
    // The name now reads as a dispatcher over exactly one untyped overload,
    // and will for as long as the binding lives — nothing else can append
    // to its table, and store_cell above already struck whatever an earlier
    // declaration of the same name had resolved.
    if (mo.sole_multifn) grant_mono_chunk(*b, idx);
  }

  // Feed `val` through the declaration's decorators, innermost first: the one
  // nearest the declaration sees the raw value, and each above wraps what the
  // one below returned. `@packable`, `@value` and `@derive(...)` are compiler
  // directives, not callable, and are the reason the loop can skip an entry.
  // Returns the slot holding the outermost result (+1).
  int32_t apply_decorators(const peg::Ast& ast, size_t dec_end, int32_t val) {
    for (size_t i = dec_end; i > 0; --i) {
      const auto& dec = *ast.nodes[i - 1];
      if (culebra::is_compile_time_decorator(dec)) continue;
      const auto& dec_expr = *dec.nodes[0];
      auto callee = compile_expr(dec_expr);
      int32_t arg = alloc_temp(ast);  // the one-argument run
      store_into(arg, {val, true}, /*dst_is_fresh=*/true);
      int32_t out = alloc_temp(ast);
      StampGuard pos(*this, dec_expr);
      emit(Op::Call, out, callee.slot, arg, 1);
      val = out;
    }
    return val;
  }

  // `@dec fn name(...) {…}` — the declaration binds the decorator's result.
  void compile_decorated_fn(const peg::Ast& ast, size_t dec_end,
                            const std::string& name, MemberOpts mo) {
    int32_t idx = compile_fn_chunk(ast, ast.nodes[dec_end + 1].get(),
                                   *ast.nodes.back(), mo);
    // The declaration's own name, as the undecorated path sets it: what the
    // decorator receives is still `fn <name>`, and a decorator that dispatches
    // on `f.name` is the ordinary reason to have written one.
    prog_.chunks[idx].multifn_name = name;
    int32_t val = alloc_temp(ast);
    emit(Op::MakeClosure, val, idx);
    val = apply_decorators(ast, dec_end, val);
    const Binding* b = lookup(name);
    if (!b || !b->is_cell)
      reject(ast, culebra::format("fn '{}' declared here", name));
    store_cell(ast, b->slot, {val, true});
    emit_session_decl_bind(*b, /*is_mut=*/false);
  }

  // Group same-named class members, in first-appearance order: each entry
  // lists the positions that share one name. A name declared once keeps its
  // bare closure (no dispatcher, no overhead), the rest merge.
  static std::vector<std::vector<size_t>> group_overloads(
      const std::vector<std::string>& names) {
    std::vector<std::vector<size_t>> groups;
    std::vector<std::string> seen;
    for (size_t i = 0; i < names.size(); i++) {
      auto it = std::find(seen.begin(), seen.end(), names[i]);
      if (it == seen.end()) {
        seen.push_back(names[i]);
        groups.push_back({i});
      } else {
        groups[static_cast<size_t>(it - seen.begin())].push_back(i);
      }
    }
    return groups;
  }

  static std::vector<std::string> grouped_names(
      const std::vector<std::vector<size_t>>& groups,
      const std::vector<std::string>& names) {
    std::vector<std::string> out;
    out.reserve(groups.size());
    for (const auto& g : groups) out.push_back(names[g.front()]);
    return out;
  }

  // Materialize one member name into `dst`: a lone closure, or — for an
  // overload set — the dispatcher its arms register into. Each arm's body
  // rides a scratch temp the registry empties, and every append after the
  // first hands back a duplicate +1 to the same dispatcher, so that one
  // rides a temp too; `dst` is the single reference that outlives the
  // statement.
  void emit_member_closure(int32_t dst, const std::vector<size_t>& arms,
                           const std::vector<const peg::Ast*>& asts,
                           const std::vector<int32_t>& chunks) {
    auto make_arm = [&](size_t i, int32_t slot) {
      emit(Op::MakeClosure, slot, chunks[i]);
      // A getter registers where it maps 1:1 to its source method, before
      // the meta absorbs it (the JIT's register_getter site). An overloaded
      // one still registers, and still loses the bare-read invoke: the
      // dispatcher that replaces it is not a getter.
      if (culebra::view_method(*asts[i]).is_getter) emit(Op::RegGetter, slot);
    };
    if (arms.size() == 1) {
      make_arm(arms.front(), dst);
      return;
    }
    int32_t into = -1;
    for (size_t k = 0; k < arms.size(); k++) {
      TempScope ats(*this);
      int32_t body = alloc_temp(*asts[arms[k]]);
      make_arm(arms[k], body);
      int32_t out = k == 0 ? dst : alloc_temp(*asts[arms[k]]);
      emit(Op::MultifnReg, out, body, into, chunks[arms[k]]);
      forget_temp(body);  // the registry absorbed the body's +1 (reg is nil)
      into = dst;
    }
  }

  // `class Name { new(...) {...}  m(...) {...}  get g() {...}
  //               static f(...) {...}  static K = e  x = e  y: T = e }`.
  // The runtime shape is the JIT's: one shared meta Object holding the
  // instance methods (getters among them, registered so a bare read invokes
  // them), instances delegating to it through `proto`, and a class namespace
  // Object carrying `new` plus the statics, marked so `C(args)` reaches the
  // constructor. Field initializers become a synthetic thunk the `new` body
  // runs after its parameters bound. Same-named members with distinct
  // signatures merge into one dispatcher through the runtime multimethod
  // registry, exactly as a free `fn name` overload set does.
  // The declaration cell a class or enum name binds through before its body
  // compiles: fill the cell an earlier forward reference already minted (so
  // both readers see the same value), bind in the session at the REPL's top
  // level, or mint a fresh nil cell. The binding comes back for
  // emit_session_decl_bind.
  struct DeclCell {
    int32_t slot;
    Binding* binding;
  };
  DeclCell bind_decl_cell(const peg::Ast& ast, const std::string& name) {
    if (Binding* pre = predeclared_here(name)) {
      slot_rank_[pre->slot] = next_rank_++;
      settle_predeclared(*pre);
      return {pre->slot, pre};
    }
    if (repl_top()) {
      Binding& sb = bind_session(ast, name);
      sb.lazy = false;
      sb.shadowed_builtin = false;
      return {sb.slot, &sb};
    }
    int32_t slot = alloc_cell_slot(ast, name);
    {
      TempScope ts(*this);
      int32_t t = alloc_temp(ast);
      emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
      emit(Op::CellNew, slot, t);
    }
    push_binding({name, slot, /*is_mut=*/false, /*is_cell=*/true});
    return {slot, &scopes_.back().bindings.back()};
  }

  void compile_class_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    size_t dec_end = 0;
    while (dec_end < ast.nodes.size() &&
           ast.nodes[dec_end]->tag == "DECORATOR"_)
      dec_end++;
    // `@derive(...)`, `@packable` and `@value` are compiler-recognized
    // directives, not callable decorators — apply_decorators skips them; every
    // other one is called on the finished class value below.
    bool is_packable = false;
    bool is_value = false;
    for (size_t i = 0; i < dec_end; i++) {
      if (culebra::is_packable_decorator(*ast.nodes[i])) is_packable = true;
      if (culebra::is_value_decorator(*ast.nodes[i])) is_value = true;
    }
    auto class_head =
        culebra::parse_generic_head(ast.nodes[dec_end]->token);
    auto class_name = std::string(class_head.outer);
    // The one clause of the `@value` contract that is about the decorators
    // rather than a member; the per-member half is require_value_member,
    // called from the collection loop below. lint reports both pre-eval.
    if (is_value && is_packable)
      throw culebra::CulebraError("SyntaxError",
                                  culebra::value_packable_message(class_name));
    // A member's annotations lower against the class's Generic params, as
    // the JIT's class_type_params_ does.
    auto type_params = culebra::split_generic_args(class_head.args);

    // Names may repeat: same-named members with distinct signatures are an
    // overload set (lint has already rejected two identical signatures, so
    // within the slice — where a typed parameter is itself rejected — the
    // arms always differ in arity).
    std::vector<const peg::Ast*> new_asts;
    std::vector<std::string> method_names;
    std::vector<const peg::Ast*> method_asts;
    std::vector<std::string> static_names;
    std::vector<const peg::Ast*> static_asts;
    std::vector<const peg::Ast*> static_fields;
    std::vector<const peg::Ast*> fields;
    // Declared (name, type) pairs in field order — the @packable layout spec.
    std::vector<std::pair<std::string, std::string>> packable_fields;
    for (size_t i = dec_end + 1; i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto mv = culebra::view_method(m);
      // The shared member checks both other backends run while collecting:
      // a getter takes no parameters (and is a method, not a field form), and
      // a @packable field carries the type its bytes are laid out from (lint
      // reports both first; these are the same safety net the others keep).
      if (mv.is_getter) culebra::require_getter_no_params(mv, class_name);
      if (is_packable && mv.is_field && !mv.is_static)
        culebra::require_typed_packable_field(mv, class_name);
      if (is_value) culebra::require_value_member(mv, class_name);
      if (is_packable && mv.is_typed_field)
        packable_fields.emplace_back(mv.name, mv.type_annotation);
      // Member classification, mirroring collect_class_members on both
      // backends — including its wart: a *typed* field is an instance field
      // even when written `static`, so `static T: Long = 5` lands on every
      // instance and the class object never carries it (probed, and the two
      // agree, so the slice inherits it rather than inventing a third
      // reading).
      if (mv.is_typed_field) {
        fields.push_back(&m);
        continue;
      }
      if (mv.is_field) {
        (mv.is_static ? static_fields : fields).push_back(&m);
        continue;
      }
      if (mv.is_static) {
        static_names.emplace_back(mv.name);
        static_asts.push_back(&m);
        continue;
      }
      if (mv.name == "new") {
        new_asts.push_back(&m);
        continue;
      }
      method_names.emplace_back(mv.name);
      method_asts.push_back(&m);
    }
    // The clause the per-member scan cannot answer on its own: a member that
    // writes `self.<undeclared>` gives one instance a field its siblings
    // lack, which is the fixed shape the contract is for. Needs the whole
    // field set, so it runs after the loop — lint reports it in the same
    // place, from the same scan.
    if (is_value) {
      std::set<std::string, std::less<>> declared;
      for (const auto* f : fields)
        declared.emplace(culebra::view_method(*f).name);
      auto writes = culebra::find_value_self_writes_in_members(
          ast, dec_end + 1, declared);
      if (!writes.empty())
        throw culebra::CulebraError(
            "SyntaxError",
            culebra::value_undeclared_self_write_message(class_name,
                                                         writes.front().field),
            static_cast<long>(writes.front().line),
            static_cast<long>(writes.front().col));
    }
    // The class joins the registry only once its own members have passed, and
    // after the scan above rather than before it, so a field naming the class
    // being declared is still refused — a value cannot contain itself.
    if (is_value) culebra::register_value_class(class_name);
    const peg::Ast* new_ast = new_asts.empty() ? nullptr : new_asts.front();
    // Resolve `@derive(...)` into the (method name, runtime kind) pairs to
    // append to the meta, after the members so a name the class declares
    // itself wins (collect_class_members' user_defined rule). An unknown
    // trait name is lint's, reported before anything runs; the throw here is
    // the same safety net the other backends keep.
    std::vector<std::pair<std::string, int>> derive_methods;
    auto declares = [&](std::string_view n) {
      return std::find(method_names.begin(), method_names.end(), n) !=
             method_names.end();
    };
    auto derives = [&](std::string_view n) {
      return std::any_of(derive_methods.begin(), derive_methods.end(),
                         [&](const auto& d) { return d.first == n; });
    };
    for (size_t i = 0; i < dec_end; i++) {
      for (auto trait : culebra::view_derive(*ast.nodes[i])) {
        auto dm = culebra::derive_method_for(trait);
        if (!declares(dm.name))
          derive_methods.emplace_back(std::string(dm.name), dm.kind);
      }
    }
    // A value compares by its fields, not by identity, so `@value` implies
    // the Eq the user would otherwise write out. Synthesized as @derive(Eq,
    // Hash)'s methods so `==` (which falls back to `eq`) and key equality see
    // the same one — and the two stay a consistent pair, which is why writing
    // an equality of your own opts out of both.
    if (is_value && !declares("eq") && !declares("__eq__")) {
      if (!derives("eq")) derive_methods.emplace_back("eq", 0);
      if (!declares("hash") && !derives("hash"))
        derive_methods.emplace_back("hash", 1);
    }
    // A well-known name with an overload set can't satisfy the 0-arg
    // contract: the grouped dispatcher replaces the arms, so
    // build_class_meta never sees one to reject. Collected here, thrown
    // where build_class_meta would have thrown (the JIT's
    // emit_wk_overload_errors).
    std::vector<std::string> wk_overloaded;
    {
      std::set<std::string> seen, taken;
      for (const auto& n : method_names)
        if (!seen.insert(n).second && culebra::is_well_known_prop(n) &&
            taken.insert(n).second)
          wk_overloaded.push_back(n);
    }

    // The class name binds before any body compiles: a method that says
    // `Name.new(...)` captures this cell, which the finished class value
    // lands in below (the JIT's pre-allocated class slot). When a closure
    // earlier in the statement list already forward-referenced the name,
    // predeclare_forward_refs minted that cell — fill it rather than mint
    // a second one, so both readers see the same class.
    auto [class_slot, decl_binding] = bind_decl_cell(ast, class_name);
    // `Name.new(...)` reaches one chunk, and the compiler can say which —
    // unless an overload set makes the constructor a dispatcher, or a
    // decorator that is not a compile-time one is free to hand back
    // something that is not this class (is_compile_time_decorator, the same
    // predicate apply_decorators skips on). Reserved before the methods
    // compile so a method constructing
    // its own class resolves too, which is where the constructions in a
    // `@value` operator live.
    int32_t ctor_chunk_idx = -1;
    if (new_asts.size() < 2 &&
        std::all_of(ast.nodes.begin(), ast.nodes.begin() + dec_end,
                    [](const auto& d) {
                      return culebra::is_compile_time_decorator(*d);
                    })) {
      ctor_chunk_idx = static_cast<int32_t>(prog_.chunks.size());
      prog_.chunks.emplace_back();
      grant_known_ctor(*decl_binding, ctor_chunk_idx);
    }

    TempScope ts(*this);
    // Static field values evaluate here, at the declaration, in the
    // enclosing scope (the JIT's static_field_vals) — the only user code a
    // class declaration runs, so it stays ahead of the closure building,
    // which keeps the method run below contiguous.
    std::vector<int32_t> static_field_vals;
    for (auto* m : static_fields) {
      auto mv = culebra::view_method(*m);
      static_field_vals.push_back(
          owned_src(*m, mv.value ? compile_expr(*mv.value)
                                 : ExprResult{emit_zero_value(
                                                  *m, mv.type_annotation),
                                              true}));
    }
    std::vector<int32_t> method_chunks;
    for (auto* m : method_asts) {
      auto mv = culebra::view_method(*m);
      method_chunks.push_back(
          compile_fn_chunk(*m, mv.params, **mv.body,
                           {.receiver = true,
                            .owner_ctor_chunk = ctor_chunk_idx,
                            .type_params = &type_params}));
      auto& ch = prog_.chunks[method_chunks.back()];
      ch.multifn_name = std::string(mv.name);
      if (mv.is_getter) ch.is_getter = true;
    }
    std::vector<int32_t> static_chunks;
    for (auto* m : static_asts) {
      auto mv = culebra::view_method(*m);
      // A static body is a receiver frame too: `S.f()` binds the class
      // object as its `self`, which is what the interp's namespace call
      // does (probed — a detached `let f = S.f` keeps that receiver).
      static_chunks.push_back(
          compile_fn_chunk(*m, mv.params, **mv.body,
                           {.receiver = true, .type_params = &type_params}));
      prog_.chunks[static_chunks.back()].multifn_name = std::string(mv.name);
    }
    // The field-init thunk is compiled — and bound under its hidden name —
    // before the `new` body, whose capture list fn_analysis already points
    // at it. A `new` body that reaches it pays a culebra frame per
    // construction, so it has to earn that: an initializer expression has to
    // be evaluated somewhere, and a plain `x: Float` does not. (A class with
    // no `new` body needs the thunk regardless — build_class_instance
    // invokes it, there being nothing else to do the stores.) fn_analysis
    // decides the hidden capture on this same question; keep the two
    // answers together.
    const bool fields_need_a_thunk =
        std::any_of(fields.begin(), fields.end(), [](const peg::Ast* f) {
          return culebra::view_method(*f).value != nullptr;
        });
    int32_t finit_slot = -1;
    if (!fields.empty() && (fields_need_a_thunk || !new_ast)) {
      int32_t idx = compile_fn_chunk(ast, /*params=*/nullptr, ast,
                                     {.receiver = true,
                                      .thunk_fields = &fields,
                                      .type_params = &type_params});
      int32_t t = alloc_temp(ast);
      emit(Op::MakeClosure, t, idx);
      finit_slot = alloc_cell_slot(ast, "(field.init)");
      emit(Op::CellNew, finit_slot, owned_src(ast, {t, true}));
      if (new_ast) {
        push_binding(
            {culebra::field_init_slot_name(ast), finit_slot,
             /*is_mut=*/false, /*is_cell=*/true});
      }
    }
    std::vector<int32_t> body_chunks;
    for (auto* m : new_asts) {
      auto mv = culebra::view_method(*m);
      body_chunks.push_back(compile_fn_chunk(
          *m, mv.params, **mv.body,
          {.receiver = true,
           .field_init_owner = fields_need_a_thunk ? &ast : nullptr,
           .prologue_fields =
               (!fields.empty() && !fields_need_a_thunk) ? &fields : nullptr,
           .owner_ctor_chunk = ctor_chunk_idx,
           .type_params = &type_params}));
    }

    // The shared meta: one closure per member name in a contiguous run —
    // an overloaded name contributing its dispatcher instead, and each
    // @derive method its captureless thunk closure, appended after the user
    // methods as the JIT appends them.
    auto groups = group_overloads(method_names);
    int32_t meta = alloc_temp(ast);
    {
      auto n_run = static_cast<int32_t>(groups.size() + derive_methods.size());
      int32_t run = next_slot_;
      for (int32_t k = 0; k < n_run; k++) alloc_temp(ast);
      for (size_t g = 0; g < groups.size(); g++)
        emit_member_closure(run + static_cast<int32_t>(g), groups[g],
                            method_asts, method_chunks);
      auto names = grouped_names(groups, method_names);
      for (size_t d = 0; d < derive_methods.size(); d++) {
        emit(Op::DeriveFn,
             run + static_cast<int32_t>(groups.size() + d),
             derive_methods[d].second);
        names.push_back(derive_methods[d].first);
      }
      auto table = static_cast<int32_t>(chunk_.name_tables.size());
      chunk_.name_tables.push_back(std::move(names));
      // A method that names its class (in its body, or in a closure it
      // builds) needs the class object with it: the send path ships the
      // two together, which is where such an instance is refused.
      bool names_class = false;
      for (auto* m : method_asts) {
        const auto& mi = analysis_.func_info.at(m);
        if (!mi.own_name.empty() &&
            (mi.own_name_used || mi.captured_locals.contains(mi.own_name)))
          names_class = true;
      }
      chunk_.name_table_flags.push_back(static_cast<uint8_t>(
          (culebra::is_lowered_state_class(class_name, ast.path)
               ? kClassMetaLoweredState
               : 0) |
          (names_class ? kClassMetaNamesClass : 0) |
          (is_value ? kClassMetaValue : 0)));
      // Both contract throws below are positionless and the interp stamps
      // them at the declaration.
      emit(Op::SetOpPos);
      for (const auto& n : wk_overloaded) emit(Op::WkErr, kconst_str(n));
      emit(Op::ClassMeta, meta, run, n_run, table);
      for (int32_t k = 0; k < n_run; k++)
        forget_temp(run + k);  // the meta took each +1
    }

    // The constructor closure captures {meta, field-init, `new` body}; each
    // capture is a cell in this frame, like every other closure's.
    int32_t meta_cell = alloc_cell_slot(ast, "(class.meta)");
    emit(Op::CellNew, meta_cell, owned_src(ast, {meta, true}));
    int32_t nil_cell = -1;
    auto cell_or_nil = [&](int32_t cell) {
      if (cell >= 0) return cell;
      if (nil_cell < 0) {
        TempScope nts(*this);
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
        nil_cell = alloc_cell_slot(ast, "(class.absent)");
        emit(Op::CellNew, nil_cell, t);
      }
      return nil_cell;
    };
    // A `new` body reaches the field-init through its own hidden capture,
    // so the constructor passes one or the other, never both — the split
    // build_class_instance expects. One constructor closure per `new`: an
    // overload set registers them all under a shared dispatcher, so the arm
    // is picked before any instance is allocated. They share one meta cell
    // (the JIT gives each closure its own cell and so has to hand out one
    // meta reference apiece).
    int32_t ctor = alloc_temp(ast);
    int32_t into = -1;
    for (size_t k = 0; k < std::max<size_t>(new_asts.size(), 1); k++) {
      const peg::Ast* na = new_asts.empty() ? nullptr : new_asts[k];
      int32_t body_cell = -1;
      if (na) {
        TempScope bts(*this);
        int32_t t = alloc_temp(*na);
        emit(Op::MakeClosure, t, body_chunks[k]);
        body_cell = alloc_cell_slot(*na, "(class.new)");
        emit(Op::CellNew, body_cell, owned_src(*na, {t, true}));
      }
      std::vector<int32_t> ctor_caps{
          meta_cell, na ? cell_or_nil(-1) : cell_or_nil(finit_slot),
          na ? body_cell : cell_or_nil(-1)};
      int32_t ctor_chunk = compile_ctor_chunk(
          ast, class_name, ctor_caps, na ? body_chunks[k] : -1, ctor_chunk_idx);
      if (new_asts.size() < 2) {
        emit(Op::MakeClosure, ctor, ctor_chunk);
        break;
      }
      prog_.chunks[ctor_chunk].multifn_name = "new";
      TempScope cts(*this);
      int32_t c = alloc_temp(*na);
      emit(Op::MakeClosure, c, ctor_chunk);
      int32_t out = k == 0 ? ctor : alloc_temp(*na);
      emit(Op::MultifnReg, out, c, into, ctor_chunk);
      forget_temp(c);  // the registry absorbed the constructor's +1
      into = ctor;
    }
    int32_t cls = alloc_temp(ast);
    emit(Op::ClassObj, cls);
    // The class namespace: `new`, then the statics and their fields. The raw
    // bind is deliberate — a static named `drop` is an ordinary function
    // here, so neither the well-known contract nor the drop registration
    // applies (the JIT's emit_bind_static).
    emit(Op::BindStatic, cls, kconst_str("new"), owned_src(ast, {ctor, true}));
    // Statics overload the same way, on their own dispatcher — the class
    // object and the instance meta are separate namespaces.
    for (const auto& arms : group_overloads(static_names)) {
      int32_t t = alloc_temp(*static_asts[arms.front()]);
      emit_member_closure(t, arms, static_asts, static_chunks);
      emit(Op::BindStatic, cls, kconst_str(static_names[arms.front()]),
           owned_src(*static_asts[arms.front()], {t, true}));
    }
    for (size_t i = 0; i < static_fields.size(); i++) {
      emit(Op::BindStatic, cls,
           kconst_str(std::string(culebra::view_method(*static_fields[i]).name)),
           static_field_vals[i]);
      forget_temp(static_field_vals[i]);  // the slot absorbed the +1
    }
    // @packable: register the byte layout and mark the class object so
    // `SharedBuffer.new(n, Cls)` can recover the class name from it. The
    // marker is a plain String, which is not refcounted, so the bind's
    // absorbed +1 costs nothing.
    if (is_packable) {
      std::string spec;
      for (const auto& [fname, ftype] : packable_fields) {
        if (!spec.empty()) spec += ';';
        spec += fname;
        spec += ':';
        spec += ftype;
      }
      emit(Op::RegPack, kconst_str(class_name), kconst_str(spec));
      int32_t marker = alloc_temp(ast);
      emit(Op::LoadConst, marker, kconst_str(class_name));
      emit(Op::BindStatic, cls, kconst_str("__packable__"), marker);
      forget_temp(marker);  // the slot absorbed the +1
    }
    // The name binds what the decorators return — the class object itself when
    // they hand it back, as `@mark` does, and anything else when they do not.
    cls = apply_decorators(ast, dec_end, cls);
    store_cell(ast, class_slot, {cls, true});
    emit_session_decl_bind(*decl_binding, /*is_mut=*/false);
  }

  // `enum Name { A(Long), B }` — a namespace object whose members are the
  // variants: a nullary one is the singleton instance it always is, a
  // payload one the constructor that builds an instance with positional
  // `_0.._n` fields. Both other backends build exactly this object, so
  // `E.A(5)`, `E.B`, `type_of`, matching and `@packable` all follow from it
  // with nothing else to teach. The name binds through a cell first, like a
  // class's, so a variant referenced from inside the declaration resolves.
  void compile_enum_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    size_t dec_end = culebra::first_non_decorator_index(ast);
    auto enum_name =
        std::string(culebra::parse_generic_head(ast.nodes[dec_end]->token).outer);
    bool is_packable = false;
    for (size_t i = 0; i < dec_end; i++) {
      if (culebra::is_packable_decorator(*ast.nodes[i])) is_packable = true;
      // Lint's, pre-eval; the throws are the class form's safety net.
      if (!culebra::view_derive(*ast.nodes[i]).empty())
        throw culebra::CulebraError("SyntaxError",
                                    culebra::derive_on_enum_message(enum_name));
      if (culebra::is_value_decorator(*ast.nodes[i]))
        throw culebra::CulebraError("SyntaxError",
                                    culebra::value_on_enum_message(enum_name));
    }
    StampGuard pos(*this, ast);
    // A closure earlier in the list may already hold this name's cell
    // (predeclare_forward_refs); fill that one, as compile_class_decl does.
    auto [enum_slot, decl_binding] = bind_decl_cell(ast, enum_name);

    TempScope ts(*this);
    int32_t obj = alloc_temp(ast);
    emit(Op::ObjectNew, obj);
    std::string spec;
    for (size_t i = dec_end + 1; i < ast.nodes.size(); i++) {
      auto vv = culebra::view_variant(*ast.nodes[i]);
      std::string variant(vv.name);
      if (is_packable) {
        if (!spec.empty()) spec += ';';
        spec += variant;
        spec += ':';
        for (size_t f = 0; f < vv.field_types.size(); f++) {
          if (f) spec += ',';
          spec += std::string(vv.field_types[f]);
        }
      }
      int32_t v = alloc_temp(*ast.nodes[i]);
      emit(Op::EnumVariant, v, static_cast<int32_t>(vv.arity),
           kconst_str(variant), kconst_str(enum_name));
      // The member is immutable, as it is on both other backends — a
      // variant is not reassignable through the enum object.
      emit(Op::ObjectSet, obj, v, kconst_str(variant), 0);
      forget_temp(v);  // the store absorbed the +1
    }
    // @packable: the tagged-union layout has to land in the process that
    // RUNS, which under AOT is not the one that compiled — so it registers
    // at the declaration, like a class's.
    if (is_packable)
      emit(Op::RegPack, kconst_str(enum_name), kconst_str(spec), 0, 1);
    obj = apply_decorators(ast, dec_end, obj);
    store_cell(ast, enum_slot, {obj, true});
    emit_session_decl_bind(*decl_binding, /*is_mut=*/false);
  }

  // `trait Name: Super { req(); def() { ... } }` — a contract in the shared
  // registry plus one closure per default body, and no binding: a trait is
  // not a value (`inspect(T)` is the undeclared name's error on every
  // backend), so the whole declaration is a run of registrations in the
  // order the interp performs them.
  void compile_trait_decl(const peg::Ast& ast) {
    auto spec = culebra::trait_decl_spec(ast);
    int32_t name_k = kconst_str(spec.name);
    emit(Op::TraitReset, name_k);
    for (size_t i = culebra::first_non_decorator_index(ast) + 1;
         i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto tv = culebra::view_trait_method(m);
      if (!tv.body) continue;  // signature-only: contract, no body
      bool has_param = false;
      (void)culebra::trait_method_arity(*tv.params, &has_param);
      // A default reaches every conforming instance without passing the
      // object_set chokepoint, so the well-known contract is enforced at
      // the declaration — before the closure exists for a +1 to strand on.
      if (culebra::is_well_known_prop(tv.name) && has_param) {
        emit(Op::SetOpPos);
        emit(Op::WkErr, kconst_str(std::string(tv.name)));
      }
      // A default is a method: its frame owns the ABI receiver as `self`.
      int32_t idx =
          compile_fn_chunk(m, tv.params, *tv.body, {.receiver = true});
      TempScope ts(*this);
      int32_t t = alloc_temp(m);
      emit(Op::MakeClosure, t, idx);
      emit(Op::TraitDefault, name_k, kconst_str(std::string(tv.name)), t);
      forget_temp(t);  // the registry took the +1
    }
    emit(Op::TraitReg, name_k, kconst_str(spec.methods),
         kconst_str(spec.supers));
  }

  // The synthetic constructor: `build_class_instance` over the captured
  // {meta, field-init, body} triple and the frame's own arguments, which it
  // forwards untouched: the instance is allocated FIRST and the `new` body's own
  // prologue binds the arguments, so an arity or typed-param error reports
  // that body's parameter names, fires before any field initializer, and
  // leaves a half-built instance behind for its `drop`.
  // `reserved` is an index compile_class_decl took before the methods
  // compiled, so a method that constructs its own class resolves against a
  // chunk that does not exist yet — the same reserve-then-fill order
  // compile_fn_chunk already uses for nested chunks. -1 appends as usual.
  int32_t compile_ctor_chunk(const peg::Ast& ast, const std::string& class_name,
                             const std::vector<int32_t>& caps,
                             int32_t body_chunk, int32_t reserved = -1) {
    int32_t idx = reserved;
    if (idx < 0) {
      idx = static_cast<int32_t>(prog_.chunks.size());
      prog_.chunks.emplace_back();
    }
    Compiler fc(prog_, analysis_, /*in_function=*/true, info_, idx);
    fc.repl_ = repl_;
    fc.debug_ = debug_;
    fc.stamp(ast);
    fc.push_scope(ast, /*owned_mark=*/false);
    fc.chunk_.capture_src_slots = caps;
    fc.chunk_.forwards_args = true;
    // The receiver is the class object — `C(...)` and `C.new(...)` both
    // hand it over — and the instance keeps a +1 on it (JitObject::cls).
    // Raw: a call with no receiver builds an instance of no class.
    fc.chunk_.self_slot = fc.alloc_slot(ast, "self");
    fc.chunk_.self_raw = true;
    // `C.new(...)` binds against this closure, so the constructor publishes
    // the `new` body's signature verbatim — the overload registry reads it
    // off the chunk, and so does the keyword resolver, whose view of which
    // parameters carry a default decides an ArityError from a bind. The
    // body's own prologue is the one that binds; describing the signature
    // twice is what let the two drift.
    if (body_chunk >= 0) fc.chunk_.adopt_signature(prog_.chunks[body_chunk]);
    int32_t cap0 = -1;
    for (size_t i = 0; i < caps.size(); i++) {
      int32_t s = fc.alloc_slot(ast, "(class.capture)");
      if (i == 0) cap0 = s;
      fc.emit(Op::BindCapture, s, static_cast<int32_t>(i));
    }
    // The instance rides a temp: the scope ladder below releases named
    // slots, and the frame's result must outlive it.
    int32_t rv = fc.alloc_temp(ast);
    {
      TempScope ts(fc);
      int32_t run = fc.next_slot_;
      for (size_t i = 0; i < caps.size(); i++)
        fc.emit(Op::CellGet, fc.alloc_temp(ast),
                cap0 + static_cast<int32_t>(i));
      fc.emit(Op::MakeInst, rv, run, fc.kconst_str(class_name),
              fc.chunk_.self_slot);
    }  // the sweep drops the three borrowed reads
    fc.pop_scope();
    fc.emit(Op::Ret, rv);
    fc.finalize_chunk();
    prog_.chunks[idx] = std::move(fc.chunk_);
    return idx;
  }

  // Resolve a nested chunk's capture list in the creating frame. The callee
  // re-derives nothing about a capture, so every flag its binding needs rides
  // along; a capture of a capture keeps the original's by construction.
  struct CaptureList {
    std::vector<int32_t> slots;
    std::vector<bool> muts;
    std::vector<bool> lazys;
    std::vector<bool> shadowed_builtins;  // unbound cell => the stdlib global
    // The chunk the captured cell's value is known to hold, and that cell's
    // identity — a capture of a capture keeps the original's, so every site
    // stays reachable from the one cell a re-declaration would overwrite.
    std::vector<Binding::Known> knowns;

    // A capture with nothing behind it (a sentinel cell): every flag off.
    void push(int32_t slot) {
      slots.push_back(slot);
      muts.push_back(false);
      lazys.push_back(false);
      shadowed_builtins.push_back(false);
      knowns.emplace_back();
    }
    void push(const Binding& b) {
      slots.push_back(b.slot);
      muts.push_back(b.is_mut);
      lazys.push_back(b.lazy);
      shadowed_builtins.push_back(b.shadowed_builtin);
      knowns.push_back(b.known);
    }
  };
  // A capture with nothing to capture: a cell holding `v`, in a slot shared
  // by every such site in this chunk. The contents never change — nothing can
  // assign to `self`, and a name with no binding has nothing to assign
  // through — so one slot serves them all, which is what keeps a chunk full
  // of such closures inside the 256 a frame has (a cell slot is pinned for
  // the frame's life). Each site still writes it, so the cell is always
  // created on the path that captures it.
  int32_t self_none_slot_ = -1;
  int32_t ufcs_none_slot_ = -1;
  int32_t none_cell(const peg::Ast& at, JitValue v, int32_t& slot,
                    const char* name) {
    if (slot < 0) slot = alloc_cell_slot(at, name);
    TempScope ts(*this);
    int32_t t = alloc_temp(at);
    emit(Op::LoadConst, t, kconst(v));
    emit(Op::CellNew, slot, t);
    return slot;
  }

  CaptureList resolve_captures(const peg::Ast& ast, const FuncInfo& info) {
    CaptureList caps;
    for (const auto& fv : info.free_vars) {
      const Binding* b = lookup(fv);
      // A UFCS candidate read only as a method name is an OPTIONAL free
      // variable: the enclosing frames may not bind it at all, and that is
      // not an error — the call site's Function gate simply declines and the
      // receiver answers for itself. Feed the closure a fresh nil cell, the
      // same thing emit_closure_build does for a candidate out of reach.
      // `self` is a free variable of any function that mentions it, and no
      // enclosing frame need have a receiver: a call may still supply one
      // dynamically, and where none does, reading it is a NameError rather
      // than a compile-time matter. Feed the closure a sentinel cell — what
      // emit_closure_build hands the same capture.
      if (!b && fv == "self") {
        caps.push(none_cell(ast, {TAG_NO_SELF, 0}, self_none_slot_,
                            "(self.none)"));
        continue;
      }
      // A REPL line's free variable that nothing here binds is the session's.
      // The captured cell is the one a later line's declaration fills, which
      // is what lets a closure see a name declared after it (probed) — and it
      // subsumes the UFCS-candidate case below, since an unbound session cell
      // holds the sentinel that declines the call site's Function gate just
      // as a nil one does.
      if (!b && repl_) b = &bind_session(ast, fv);
      if (!b && info.optional_free_vars.contains(fv)) {
        caps.push(none_cell(ast, {TAG_NIL, 0}, ufcs_none_slot_,
                            "(ufcs.none)"));
        continue;
      }
      // predeclare_forward_refs put a lazy cell in place for every name
      // this statement list declares and a closure here captures, so a
      // forward reference resolves above. Anything still missing is a
      // name no statement list on the way in declares.
      if (!b) reject(ast, culebra::format("forward-reference capture of '{}'", fv));
      // Same reason a read refills it: MakeClosure may sit in a branch the
      // slot's own ReplCell does not dominate.
      ensure_session_slot(*b);
      caps.push(*b);
    }
    return caps;
  }

  // One function literal -> one chunk, compiled by a fresh Compiler (frame
  // state is per chunk; the program and analysis are shared). free_vars
  // resolve against THIS frame's bindings — each must already be a declared
  // cell binding (its own CellNew slot, or a capture passed through) —
  // and become the callee chunk's capture list. uses_fn places the
  // recursion-handle slot. A MULTIFN_DECL body reuses this whole path with
  // its own params/body children (its name child shifts them by one); a
  // `defer` body reuses it with `params` null — a 0-arity thunk over the
  // same closure machinery and frame epilogue.
  //
  // A function literal is also where the slice stops being fatal: anything
  // inside one that the slice does not cover collapses the chunk to the
  // rejection itself (see emit_poison_chunk), so the module still compiles
  // and only a call that reaches the construct raises VmError. The chunks a
  // discarded body appended are discarded with it — nothing outside it holds
  // their indices.
  int32_t compile_fn_chunk(const peg::Ast& ast, const peg::Ast* params,
                           const peg::Ast& body, MemberOpts mo = MemberOpts()) {
    size_t mark = prog_.chunks.size();
    try {
      return compile_fn_chunk_impl(ast, params, body, mo);
    } catch (const Unsupported& u) {
      // The chunks the abandoned attempt appended go away, and with them
      // any call site it resolved: the refs kept for revocation must not
      // outlive the chunks they index.
      for (auto& [cell, sites] : prog_.resolved_by_cell)
        std::erase_if(sites, [&](const auto& s) {
          return static_cast<size_t>(s.chunk) >= mark;
        });
      prog_.chunks.resize(mark);
      return emit_poison_chunk(ast, u);
    }
  }

  // The whole body of a chunk the slice rejected: raise, at the position the
  // rejected construct sits on. It declares no parameters and no captures —
  // the caller's MakeClosure reads both from here — and `variadic` keeps
  // every call shape (and every callback-arity probe) from reporting an
  // arity error in front of the rejection.
  int32_t emit_poison_chunk(const peg::Ast& ast, const Unsupported& u) {
    int32_t idx = static_cast<int32_t>(prog_.chunks.size());
    prog_.chunks.emplace_back();
    Compiler fc(prog_, analysis_, /*in_function=*/true, info_, idx);
    fc.repl_ = repl_;
    fc.debug_ = debug_;
    fc.chunk_.variadic = true;
    fc.chunk_.cb_max = -1;
    int32_t rv = fc.alloc_raw(ast, "(unsupported)", /*named=*/false);
    fc.emit_raise("VmError", "--vm: unsupported: " + u.what, u.line, u.col);
    fc.emit(Op::Ret, rv);  // dead: the raise above never falls through
    fc.finalize_chunk();
    prog_.chunks[idx] = std::move(fc.chunk_);
    return idx;
  }

  int32_t compile_fn_chunk_impl(const peg::Ast& ast, const peg::Ast* params,
                                const peg::Ast& body, MemberOpts mo) {
    using namespace peg::udl;
    const FuncInfo& info = analysis_.func_info.at(&ast);
    std::string_view return_type;
    for (const auto& n : ast.nodes)
      if (n->tag == "RETURN_TYPE"_) return_type = n->token;
    auto caps = resolve_captures(ast, info);

    int32_t idx = static_cast<int32_t>(prog_.chunks.size());
    prog_.chunks.emplace_back();  // reserve the index; nested fns append
    Compiler fc(prog_, analysis_, /*in_function=*/true, &info, idx);
    fc.repl_ = repl_;
    fc.debug_ = debug_;
    fc.stamp(ast);
    // The frame scope: params + captures + the `fn` handle. Its owned mark
    // waits until the ABI slots are laid out (establish_frame_owned_mark).
    fc.push_scope(ast, /*owned_mark=*/false);
    fc.chunk_.arity =
        params ? static_cast<int32_t>(params->nodes.size()) : 0;
    fc.chunk_.capture_src_slots = std::move(caps.slots);
    for (size_t i = 0; i < caps.muts.size() && i < info.free_vars.size(); ++i)
      if (caps.muts[i]) fc.chunk_.mut_capture_names.push_back(info.free_vars[i]);
    // Params occupy the ABI slots [0, arity). A captured param moves into a
    // fresh cell right after; the ABI slot stays behind as an anonymous
    // drained slot.
    struct CellPromo {
      std::string name;
      int32_t abi_slot;
      bool is_mut;
      const peg::Ast* at;
    };
    std::vector<CellPromo> promos;
    // A destructuring param (`fn ([a, b])`) is one positional ABI slot under
    // the JIT's synthetic name (which also feeds the ArityError message);
    // the prologue below unpacks it. No binding: the synthetic name is not
    // referencable (interp/JIT NameError; here the usual unresolved reject).
    struct PatParam {
      const peg::Ast* pat;
      int32_t slot;
    };
    std::vector<PatParam> pat_params;
    // The declared parameters, in ABI order. Slots and the name/type tables
    // are laid out here; binding them (default fill, type check, cell
    // promotion) waits until the captures are in place, since a default
    // expression reads them — the JIT prologue's order.
    struct ParamPlan {
      const peg::Ast* at;
      std::string name;
      std::string type;
      const peg::Ast* default_expr;
      int32_t slot;
      int32_t abi_index;
      bool is_mut;
      bool sink;
      int32_t pos_slot = -1;  // PosSnap's eager snapshot, -1 = cold path
    };
    std::vector<ParamPlan> plans;
    // The `**rest` slot, bound at the end of the prologue: it is the one
    // parameter no positional can fill, so its slot arrives holding either
    // the resolver's marked Object or nothing at all.
    int32_t rest_slot = -1;
    std::string rest_name;
    const peg::Ast* rest_at = nullptr;
    std::string args_rest_name;
    const peg::Ast* args_rest_at = nullptr;
    if (params) {
      for (const auto& p : params->nodes) {
        auto pv = culebra::view_parameter(*p);
        if (culebra::is_kw_only_sep(*p)) {
          // `*` takes no ABI slot: it only says where the keyword-only run
          // begins, which is the cap on how many positionals may arrive.
          if (fc.chunk_.first_kw_only_idx < 0)
            fc.chunk_.first_kw_only_idx =
                static_cast<int32_t>(fc.chunk_.param_names.size());
          fc.chunk_.arity--;
          continue;
        }
        if (pv.is_args_rest) {
          // `*args` takes no ABI slot: it names the overflow Array the
          // prologue builds, the same one `__ARGS__` reads.
          args_rest_name = std::string(pv.name);
          args_rest_at = p.get();
          fc.chunk_.arity--;
          continue;
        }
        if (pv.is_kwargs_rest) {
          rest_name = std::string(pv.name);
          rest_at = p.get();
          rest_slot = fc.alloc_slot(*p, rest_name);
          fc.chunk_.kwargs_rest_idx =
              static_cast<int32_t>(fc.chunk_.param_names.size());
          fc.chunk_.param_names.push_back(rest_name);
          fc.chunk_.param_types.emplace_back();
          fc.chunk_.param_declared_types.emplace_back();
          fc.chunk_.param_has_default.push_back(1);  // the empty Object
          fc.chunk_.param_mut.push_back(0);
          continue;
        }
        if (pv.pattern) {
          auto synth = std::string(
              culebra::destructure_param_name(fc.chunk_.param_names.size()));
          int32_t slot = fc.alloc_slot(*p, synth);
          fc.chunk_.param_names.push_back(synth);
          fc.chunk_.param_types.emplace_back();
          fc.chunk_.param_declared_types.emplace_back();
          fc.chunk_.param_has_default.push_back(0);
          fc.chunk_.param_mut.push_back(0);
          pat_params.push_back({pv.pattern, slot});
          continue;
        }
        auto name = std::string(pv.name);
        int32_t slot = fc.alloc_slot(*p, name);
        auto type = mo.type_params && !mo.type_params->empty()
                        ? culebra::lower_type_params(pv.type_annotation,
                                                     *mo.type_params)
                        : std::string(pv.type_annotation);
        auto abi = static_cast<int32_t>(fc.chunk_.param_names.size());
        fc.chunk_.param_names.push_back(name);
        fc.chunk_.param_types.push_back(type);
        fc.chunk_.param_declared_types.emplace_back(pv.type_annotation);
        fc.chunk_.param_has_default.push_back(pv.default_value ? 1 : 0);
        fc.chunk_.param_mut.push_back(pv.is_mut ? 1 : 0);
        plans.push_back({p.get(), name, std::move(type), pv.default_value,
                         slot, abi, pv.is_mut, is_sink_name(name)});
      }
    }
    // Required parameters are the leading run without a default (lint
    // rejects a required one after a defaulted one), so the arity guard is
    // a single count and the unsupplied tail is exactly the defaulted one.
    fc.chunk_.required = fc.chunk_.arity;
    for (const auto& pl : plans)
      if (pl.default_expr) {
        fc.chunk_.required = pl.abi_index;
        break;
      }
    // A destructuring parameter carries no default, so it is required
    // wherever it sits — lint rejects one after a defaulted parameter, and
    // this is the same safety net the JIT prologue keeps.
    for (const auto& pp : pat_params)
      if (pp.slot >= fc.chunk_.required) fc.chunk_.required = pp.slot + 1;
    // A `**rest` slot is never required: a call with no keyword content
    // never reaches the resolver, and the prologue binds an empty Object.
    if (fc.chunk_.kwargs_rest_idx >= 0 &&
        fc.chunk_.kwargs_rest_idx < fc.chunk_.required)
      fc.chunk_.required = fc.chunk_.kwargs_rest_idx;
    // Positional callback-arity bounds: the regular parameters are those
    // before the keyword-only run and the `**rest` slot (`*args` is out of
    // slice, so the upper bound is always finite).
    {
      int32_t regular_end = fc.chunk_.first_kw_only_idx >= 0
                                ? fc.chunk_.first_kw_only_idx
                            : fc.chunk_.kwargs_rest_idx >= 0
                                ? fc.chunk_.kwargs_rest_idx
                                : fc.chunk_.arity;
      fc.chunk_.variadic = !args_rest_name.empty();
      fc.chunk_.cb_max = fc.chunk_.variadic ? -1 : regular_end;
      fc.chunk_.cb_min = 0;
      for (int32_t i = 0; i < regular_end; i++)
        if (i >= static_cast<int32_t>(fc.chunk_.param_has_default.size()) ||
            !fc.chunk_.param_has_default[i])
          fc.chunk_.cb_min++;
    }
    // The ABI receiver. A receiver frame owns it in a slot of its own —
    // immutable, so `self = v` raises the interp's ImmutableError — and the
    // frame teardown releases it; every other frame releases it on entry.
    bool captures_self =
        !mo.receiver && std::find(info.free_vars.begin(), info.free_vars.end(),
                                  "self") != info.free_vars.end();
    if (mo.receiver || captures_self) {
      fc.chunk_.self_slot = fc.alloc_slot(ast, captures_self ? "(self.arg)"
                                                             : "self");
      fc.chunk_.self_raw = captures_self;
    }
    if (mo.receiver) {
      if (info.captured_locals.contains("self")) {
        promos.push_back({"self", fc.chunk_.self_slot, false, &ast});
      } else {
        fc.push_binding(
            {"self", fc.chunk_.self_slot, false});
      }
    }
    // The frame's own closure: the `fn` handle, and a literal's own `let`
    // name (bound below, once the captures are in place).
    bool own_name_live =
        !info.own_name.empty() &&
        (info.own_name_used || info.captured_locals.contains(info.own_name));
    if (info.uses_fn ||
        (own_name_live &&
         info.own_name_source == FuncInfo::OwnNameSource::Closure))
      fc.chunk_.fn_slot = fc.alloc_slot(ast, "fn");
    if (info.uses_fn) {
      Binding& fnb = fc.push_binding({"fn", fc.chunk_.fn_slot, false});
      // A method's `fn` IS the bound wrapper (interp: the handle a method
      // call binds), so recursion and any escapee keep the original
      // receiver. The cache makes repeated reads compare equal and leaves
      // exactly one +1 for the frame to release.
      if (fc.chunk_.self_slot >= 0)
        fc.chunk_.fn_bound_slot = fc.alloc_slot(ast, "(fn.bound)");
      // The recursion handle is this frame's own closure, so its code is
      // this chunk. Where the wrapper stands in, a read yields that
      // instead — compile_call resolves the direct `fn(...)` call itself.
      if (fc.chunk_.fn_bound_slot < 0) fc.grant_known_chunk(fnb, idx);
    }
    fc.establish_frame_defer_mark(ast, info);
    fc.establish_frame_owned_mark(ast);
    // Where a return-value type error reports: resolved once here, as the
    // JIT's prologue snapshot does (see PosSnap).
    if (!return_type.empty()) {
      // Two forms, as the parameters have: the chunk keeps the annotation as
      // written, which is what `f.return_type` reports, while the check runs
      // against the lowered one — a Generic's unbounded `T` checks as Any.
      fc.chunk_.return_type = std::string(return_type);
      fc.ret_type_str_ =
          mo.type_params && !mo.type_params->empty()
              ? culebra::lower_type_params(return_type, *mo.type_params)
              : std::string(return_type);
      fc.ret_type_ = fc.kconst_str(fc.ret_type_str_);
      fc.ret_pos_slot_ = fc.alloc_slot(ast, "(ret.pos)");
      fc.emit(Op::PosSnap, fc.ret_pos_slot_, fc.def_pos_const(ast), -1);
    }
    // The body's own name: delivered by the frame rather than captured —
    // the capture would ring cell → dispatcher/closure/class → body → cell
    // (see FuncInfo::own_name). A multifn's arrives through the dispatch
    // and a class member's through its receiver, lazy either way so the
    // escaped-past-its-owner corners read as the undeclared name's
    // NameError; a literal's is the closure the prologue already put in
    // fn_slot. Each becomes a cell when a nested closure captures it, like
    // any local's. Ahead of the promotions below, which move a captured
    // `self` out of the receiver slot the class member's read wants.
    if (own_name_live) {
      using Src = FuncInfo::OwnNameSource;
      bool captured = info.captured_locals.contains(info.own_name);
      if (info.own_name_source == Src::Closure && !captured) {
        fc.grant_known_chunk(
            fc.push_binding({info.own_name, fc.chunk_.fn_slot, false}), idx);
      } else {
        int32_t cslot = fc.alloc_cell_slot(ast, info.own_name);
        {
          TempScope mts(fc);
          if (info.own_name_source == Src::Closure) {
            fc.emit(Op::CellNew, cslot,
                    fc.owned_src(ast, {fc.chunk_.fn_slot, false}));
          } else {
            int32_t t = fc.alloc_temp(ast);
            if (info.own_name_source == Src::Dispatch)
              fc.emit(Op::MfSelf, t);
            else
              fc.emit(Op::ClsSelf, t, fc.chunk_.self_slot);
            fc.emit(Op::CellNew, cslot, t);  // nils t; the sweep is a no-op
          }
        }
        Binding& ob = fc.push_binding(
            {info.own_name, cslot, /*is_mut=*/false, /*is_cell=*/true,
             /*lazy=*/info.own_name_source != Src::Closure});
        // `Name.new(...)` in a member. The name is read from the receiver,
        // so the value is a run-time question and the site takes the guard —
        // but the chunk it would reach is known, which is what an unboxed
        // representation needs at the constructions inside an operator.
        if (info.own_name_source == Src::Receiver && mo.owner_ctor_chunk >= 0)
          ob.known.ctor = mo.owner_ctor_chunk;
        // A literal's own `let` name reads as the very closure running the
        // frame; a dispatch's or a class member's is the dispatcher/class,
        // which the lazy flag already keeps out of grant_known_chunk.
        if (info.own_name_source == Src::Closure) {
          fc.grant_known_chunk(ob, idx);
        } else if (info.own_name_source == Src::Dispatch && mo.sole_multifn) {
          // The dispatcher this body was registered into, and — this being
          // the only declaration of its name — the one whose table holds
          // this body alone. So `name(...)` inside the body reaches this
          // very chunk, which is what makes plain recursion a direct call.
          // Nothing can rewrite the cell MfSelf was just stored in, so the
          // grant needs no revocation site of its own.
          fc.grant_mono_chunk(ob, idx);
        }
      }
    }
    for (const auto& pr : promos) {
      int32_t cslot = fc.alloc_cell_slot(*pr.at, pr.name);
      fc.emit(Op::CellNew, cslot, pr.abi_slot);
      fc.push_binding({pr.name, cslot, pr.is_mut, true});
    }
    // Bind the captures: borrowed cell pointers out of the closure. The
    // slots are named-but-not-cell, so frame teardown's Release is a no-op
    // on them (the closure owns the refs). The lazy flag rides along so a
    // captured dispatcher cell read before its decl still NameErrors.
    int32_t self_cap = -1;
    for (size_t i = 0; i < info.free_vars.size(); ++i) {
      int32_t s = fc.alloc_slot(ast, info.free_vars[i]);
      fc.emit(Op::BindCapture, s, static_cast<int32_t>(i));
      // A captured enclosing `self` feeds the merge below instead of
      // binding directly: a call that supplies a receiver still wins.
      if (info.free_vars[i] == "self") {
        self_cap = s;
        continue;
      }
      Binding cap{info.free_vars[i], s, caps.muts[i], true, caps.lazys[i]};
      cap.shadowed_builtin = caps.shadowed_builtins[i];
      cap.known = caps.knowns[i];
      fc.push_binding(std::move(cap));
    }
    if (self_cap >= 0) {
      int32_t s = fc.alloc_slot(ast, "self");
      fc.emit(Op::SelfMerge, s, fc.chunk_.self_slot, self_cap);
      // Guarded either way: the merge yields the sentinel when neither the
      // call nor the capture supplied a receiver, and reading it is the
      // interp's NameError. A real receiver is never the sentinel, so the
      // guard costs a compare on the frames that do have one.
      if (info.captured_locals.contains("self")) {
        int32_t cslot = fc.alloc_cell_slot(ast, "self");
        fc.emit(Op::CellNew, cslot, s);
        fc.push_binding(
            {"self", cslot, false, true, /*lazy=*/true});
      } else {
        fc.push_binding(
            {"self", s, false, /*is_cell=*/false, /*lazy=*/true});
      }
    }
    // Bind the declared parameters, now that the captures, `self` and `fn`
    // are in place for a default expression to read. Where a typed param's
    // error reports resolves per call: the common case (no earlier default)
    // on the check's failure path, but from the first default — or `_` sink,
    // whose release can run a drop — onward user code runs inside this loop
    // and would overwrite the caller's position, so those snapshot it here
    // first. The JIT prologue's eagerPosFrom, instruction for instruction.
    {
      size_t eager_from = plans.size();
      for (size_t i = 0; i < plans.size(); ++i)
        if (plans[i].default_expr || plans[i].sink) {
          eager_from = i;
          break;
        }
      for (size_t i = eager_from; i < plans.size(); ++i) {
        if (plans[i].type.empty()) continue;
        plans[i].pos_slot =
            fc.alloc_slot(*plans[i].at, plans[i].name + ".errpos");
        fc.emit(Op::PosSnap, plans[i].pos_slot, fc.def_pos_const(ast),
                plans[i].abi_index);
      }
    }
    for (const auto& pl : plans) {
      if (pl.default_expr) {
        // The slot the prologue left TAG_UNFILLED takes the default's own
        // +1 — the same ownership a passed argument transfers. The frame is
        // not counted yet, so the evaluation counts as one of its own (a
        // default that re-enters its function would otherwise overflow the
        // C stack uncounted); a throw skips the leave and the enclosing
        // frame's restore corrects the count, as in the JIT.
        size_t filled = fc.emit(Op::JumpIfFilled, pl.slot);
        {
          TempScope ts(fc);
          StampGuard pos(fc, *pl.default_expr);
          fc.emit(Op::RecEnter, 0);
          fc.store_into(pl.slot, fc.compile_expr(*pl.default_expr),
                        /*dst_is_fresh=*/true);
          fc.emit(Op::RecLeave);
        }
        fc.patch_to_here(filled);
      }
      if (!pl.type.empty()) {
        int32_t ty = fc.kconst_str(pl.type);
        int32_t ctx = fc.kconst_str(culebra::format("parameter '{}'", pl.name));
        std::vector<size_t> skip;
        fc.emit_type_check_gate(pl.type, pl.slot, skip);
        if (pl.pos_slot >= 0)
          fc.emit(Op::ChkTypeAt, pl.slot, ty, ctx, pl.pos_slot);
        else
          fc.emit(Op::ChkArg, pl.slot, ty, ctx, pl.abi_index);
        for (size_t ix : skip) fc.patch_to_here(ix);
      }
      if (pl.sink) {
        // `fn (_, x)`: the argument's +1 is dropped rather than bound, so a
        // repeated `_` needs no slot of its own.
        fc.emit(Op::Release, pl.slot);
      } else if (info.captured_locals.contains(pl.name)) {
        // A captured param moves into a fresh cell; the ABI slot stays
        // behind as an anonymous drained one.
        int32_t cslot = fc.alloc_cell_slot(*pl.at, pl.name);
        fc.emit(Op::CellNew, cslot, pl.slot);
        fc.push_binding({pl.name, cslot, pl.is_mut, true});
      } else {
        fc.push_binding({pl.name, pl.slot, pl.is_mut});
      }
    }
    // The overflow arguments, as one Array: `__ARGS__` and a named `*args`
    // are two names for it, so it is built once and both bindings share it.
    fc.chunk_.keeps_args = info.uses_args || !args_rest_name.empty();
    if (fc.chunk_.keeps_args) {
      int32_t aslot = fc.alloc_slot(ast, "(args.rest)");
      fc.emit(Op::ArgsRest, aslot);
      auto bind_args = [&](const std::string& nm, const peg::Ast& at,
                           int32_t src) {
        int32_t slot = src;
        if (info.captured_locals.contains(nm)) {
          slot = fc.alloc_cell_slot(at, nm);
          fc.emit(Op::CellNew, slot, src);
          fc.push_binding({nm, slot, false, true});
        } else {
          fc.push_binding({nm, slot, false});
        }
      };
      if (info.uses_args && !args_rest_name.empty()) {
        // Both live: the second binding needs a `+1` of its own.
        int32_t second = fc.alloc_slot(ast, "(args.rest.2)");
        fc.emit(Op::Move, second, aslot);
        fc.emit(Op::Retain, second);
        bind_args("__ARGS__", ast, aslot);
        bind_args(args_rest_name, *args_rest_at, second);
      } else if (info.uses_args) {
        bind_args("__ARGS__", ast, aslot);
      } else {
        bind_args(args_rest_name, *args_rest_at, aslot);
      }
    }
    // The `**rest` slot last: it is the resolver's alone, so the prologue
    // either adopts the Object it marked or binds a fresh empty one.
    if (rest_slot >= 0) {
      fc.emit(Op::KwRest, rest_slot);
      fc.push_binding({rest_name, rest_slot, false});
      if (info.captured_locals.contains(rest_name)) {
        int32_t cslot = fc.alloc_cell_slot(*rest_at, rest_name);
        fc.emit(Op::CellNew, cslot, rest_slot);
        fc.scopes_.back().bindings.pop_back();
        fc.push_binding({rest_name, cslot, false, true});
      }
    }
    // Count the frame, after the parameters bound and their types checked
    // (a TypeError there outranks RecursionError — the interp checks
    // caller-side, before entering the body closure) and before any user
    // code the body runs.
    fc.emit(Op::RecEnter, 1);
    fc.chunk_.counts_frame = true;
    // Unpack destructuring params, left to right: the test-then-bind walks
    // of compile_destructure_assign against the synthetic slot (borrowed —
    // it stays behind as an anonymous drained slot). Every throw anchors at
    // the pattern node (interp/JIT's destructure_mismatch anchor), and the
    // leaves declare immutable frame bindings, cells included.
    for (const auto& pp : pat_params) {
      StampGuard pos(fc, *pp.pat);
      TempScope ts(fc);
      std::vector<size_t> fail;
      fc.compile_pattern_test(*pp.pat, pp.slot, fail);
      fc.compile_pattern_bind(*pp.pat, pp.slot, /*subj_owned=*/false, fail,
                              /*is_mut=*/false, /*declares=*/true);
      size_t done = fc.emit(Op::Jump);
      for (size_t ix : fail) fc.patch_to_here(ix);
      fc.emit(Op::DestrErr);
      fc.patch_to_here(done);
    }
    // A `new` body runs the class's field initializers here — after the
    // parameters bound, before the first body statement (interp's
    // init_instance_fields timing: an arity error leaves no field behind).
    if (mo.field_init_owner) {
      TempScope fts(fc);
      const Binding* fb =
          fc.lookup(culebra::field_init_slot_name(*mo.field_init_owner));
      int32_t t = fc.alloc_temp(ast);
      fc.emit(Op::CellGet, t, fb->slot);
      fc.emit(Op::FieldInit, t, fc.chunk_.self_slot);
    } else if (mo.prologue_fields) {
      // The same stores, in this frame instead of a thunk's. A declaration
      // with no initializer only has to put the field there, and a whole
      // culebra frame to do it was the reason declared fields cost MORE than
      // assigning in the constructor — the opposite of what a value type
      // wants (docs/language.md §21).
      fc.emit_declared_field_stores(*mo.prologue_fields);
    }
    int32_t rv = fc.alloc_temp(ast);
    if (mo.thunk_fields) {
      // The synthetic field-init thunk's whole body.
      fc.emit_declared_field_stores(*mo.thunk_fields);
    } else {
      // The body compiles INTO the frame scope rather than a nested one (the
      // JIT's "the body BLOCK is this frame's scope"): its locals belong to
      // the frame, so they are released after the frame's defers run, not
      // before — and the unwind ladder covers them.
      fc.predeclare_forward_refs(body);
      fc.compile_body_into(body, rv);
    }
    fc.emit_return_type_check(rv);
    // Frame defers run before the frame scope's releases (interp's
    // run_deferred(callEnv) order); `return` emits its own copy.
    if (fc.frame_defer_mark_ >= 0)
      fc.emit(Op::DeferRunTo, fc.frame_defer_mark_);
    fc.pop_scope();
    fc.emit(Op::Ret, rv);
    fc.finalize_chunk();
    prog_.chunks[idx] = std::move(fc.chunk_);
    return idx;
  }

  // One `defer { ... }` body -> one 0-arity chunk (analyze_defer's FuncInfo
  // drives the same capture cells; the thunk's +1 result crosses the ABI
  // like any return and defer_run_to releases it right after the invoke).
  // `return` inside exits only the thunk (interp's flow_discard, the JIT
  // thunk's local ret) — in_function_=true gives exactly that.
  int32_t compile_defer_chunk(const peg::Ast& ast) {
    // The JIT thunk binds only free vars, so an enclosing fn's recursion
    // handle is not reachable from a defer body there; keep the lanes
    // symmetric by rejecting instead of quietly diverging. The rejection
    // rides the thunk like any other (compile_fn_chunk's poison window).
    if (analysis_.func_info.at(&ast).uses_fn)
      return emit_poison_chunk(
          ast, Unsupported{"recursion handle `fn` inside defer", ast.line,
                           ast.column});
    return compile_fn_chunk(ast, /*params=*/nullptr, *ast.nodes[0]);
  }

  // `import name from './rel.cul'` — the dependency already ran (the loader
  // orders every module after the ones it imports), so its export Object is
  // in the runtime table under the absolute path both sides spell through
  // resolve_module_path. The name binds immutably here, like the interp's
  // env->initialize.
  void compile_import(const peg::Ast& ast) {
    if (ast.path.empty()) {
      // No module to resolve against: the REPL and a direct eval, in the
      // wording both other engines use.
      throw CulebraError(
          "ImportError",
          "`import` is not supported in this context (REPL or direct "
          "eval); run via `culebra script.cul`",
          static_cast<int>(ast.line), static_cast<int>(ast.column));
    }
    auto canon = culebra::resolve_module_path(
        std::string(ast.nodes[1]->token),
        std::filesystem::path(ast.path).parent_path());
    StampGuard pos(*this, ast);
    int32_t t = alloc_temp(ast);
    emit(Op::ModGet, t, kconst_str(canon.string()));
    bind_pattern_name(ast, *ast.nodes[0], t, /*src_owned=*/true,
                      /*is_mut=*/false, /*declares=*/true);
  }

  // A dependency module's body is done: collect what its `export` statements
  // name into one Object and hand it to the module table. Read through
  // compile_expr, so a name no statement bound raises the same NameError the
  // interp's extract_export does — at run time, where the read happens.
  void emit_module_export(const peg::Ast& mod) {
    using namespace peg::udl;
    StampGuard pos(*this, mod);
    int32_t obj = alloc_temp(mod);
    emit(Op::ObjectNew, obj);
    const peg::Ast* stmts =
        mod.tag == "STATEMENTS"_ || mod.original_tag == "STATEMENTS"_
            ? &mod
            : (mod.nodes.empty() ? nullptr : mod.nodes[0].get());
    if (stmts) {
      for (const auto& s : stmts->nodes) {
        if (s->tag != "EXPORT_STMT"_) continue;
        for (const auto& id : s->nodes) {
          auto v = compile_expr(*id);
          emit(Op::ObjectSet, obj, owned_src(*id, v), kconst_str(id->token),
               /*mut=*/0);
        }
      }
    }
    emit(Op::ModReg, obj, kconst_str(std::string(mod.path)));
  }

  // An assignment's RHS, with `x: T = e`'s annotation checked the moment the
  // value exists. Every shape the annotation is grammatical on — a
  // declaration, a rebind, a property or index write, `op=`, `??=` — checks
  // the RHS and nothing else: `x: Long += 1.0` throws on the 1.0 even where
  // the sum would be a Long, and a Float sum passes silently (probed on both
  // backends). This is the JIT's compile_rhs lambda, at the one point every
  // caller here already funnels through. `??=` inherits the short-circuit for
  // free, because it compiles its RHS inside the nil branch.
  ExprResult compile_assign_rhs(const peg::Ast& ast,
                                const culebra::AssignmentView& av) {
    auto r = compile_expr(*av.rhs);
    if (!av.type_annotation.empty()) {
      StampGuard pos(*this, ast);
      std::vector<size_t> skip;
      emit_type_check_gate(av.type_annotation, r.slot, skip);
      emit(Op::ChkTypeAt, r.slot, kconst_str(av.type_annotation),
           kconst_str("assignment"), -1);  // d<0: report at this instruction
      for (size_t ix : skip) patch_to_here(ix);
    }
    return r;
  }

  // Evaluates to the assigned value (interp parity: `let c = if true
  // { let x = 5 }` reads 5) — returned as a borrow of the target slot.
  ExprResult compile_assignment(const peg::Ast& ast) {
    auto av = culebra::view_assignment(ast);
    auto* tgt = culebra::assign_name_target(ast, av);
    if (!tgt) {
      if (av.lvalcnt > 1) return compile_assign_index(ast, av);
      reject(ast, "non-identifier assignment target");
    }
    // Sink: `let _ = expr` / `_ = expr` binds nothing. The RHS runs for its
    // effects and its value is the assignment's, held by the statement temp
    // that releases it — so a resource written to `_` is dropped where the
    // statement ends, not at the end of the scope. A COMPOUND assignment is
    // not a sink: it reads its target, and nothing ever binds `_`, so it
    // takes the undefined-name path below.
    if (tgt->token == "_" && !av.compound) return compile_assign_rhs(ast, av);
    if (av.compound) return compile_compound_assign(ast, av, *tgt);
    // interp's assign_name: `let` / `mut` declares, and so does a bare write
    // to a name nothing here binds — immutably, in this scope, so a second
    // write refuses. `self` and the stdlib globals are bound already (below).
    const auto name = std::string(tgt->token);
    bool declares = av.is_let || av.is_mut || declares_implicitly(name);
    bool decl_mut = av.is_mut;
    if (declares) {
      // Slot reserved before the RHS so temps stack above it; the name only
      // becomes visible after the RHS (let x = x reads the outer x).
      // A forward reference already gave this name its cell — closures
      // built above hold it, so the declaration fills that cell instead
      // of minting a second one, and the binding stops being lazy.
      if (Binding* pre = predeclared_here(name)) {
        auto rhs = compile_assign_rhs(ast, av);
        store_cell(*tgt, pre->slot, rhs);
        slot_rank_[pre->slot] = next_rank_++;  // released as declared here
        emit_session_decl_bind(*pre, decl_mut);
        // A conditional binding is shared by every arm that declares the
        // name, so its mutability is whatever the arm that RAN said — recorded
        // where a later write can read it rather than in `is_mut`, which the
        // last arm compiled would otherwise answer for all of them.
        if (pre->mut_slot >= 0)
          emit(Op::LoadConst, pre->mut_slot, kconst({TAG_BOOL, decl_mut}));
        else
          pre->is_mut = decl_mut;
        settle_predeclared(*pre);
        grant_known_chunk(*pre, rhs.chunk);
        return read_binding(*tgt, *pre);
      }
      // Re-declaring a captured name in the same scope writes the cell the
      // closures already hold, rather than minting a second one: the interp
      // overwrites the environment's entry, so a closure built before the
      // second declaration reads its value (probed on both backends).
      if (Binding* held = captured_here(name)) {
        auto rhs = compile_assign_rhs(ast, av);
        store_cell(*tgt, held->slot, rhs);
        slot_rank_[held->slot] = next_rank_++;  // redeclared here
        emit_session_decl_bind(*held, decl_mut);
        held->is_mut = decl_mut;
        grant_known_chunk(*held, rhs.chunk);
        return read_binding(*tgt, *held);
      }
      // A REPL line's top-level declaration binds in the session, so the
      // next line still finds it — and a redeclaration lands in the very
      // cell the closures of earlier lines hold, which is what makes them
      // read the new value (probed).
      // It is registered before the RHS compiles, and still lazy there, so
      // `let x = x` reads what the name meant before this statement — the
      // store is what changes it, as everywhere else.
      if (repl_top()) {
        Binding& sb = bind_session(*tgt, name);
        store_cell(*tgt, sb.slot, compile_assign_rhs(ast, av));
        emit_session_decl_bind(sb, decl_mut);
        sb.is_mut = decl_mut;
        sb.lazy = false;
        sb.shadowed_builtin = false;
        return read_binding(*tgt, sb);
      }
      bool cell = info_->captured_locals.contains(name);
      int32_t slot = cell ? alloc_cell_slot(*tgt, name) : alloc_slot(*tgt, name);
      auto rhs = compile_assign_rhs(ast, av);
      if (cell) {
        store_new_cell(*tgt, slot, rhs);
      } else {
        store_into(slot, rhs, /*dst_is_fresh=*/true);
      }
      push_binding({name, slot, decl_mut, cell});
      grant_known_chunk(scopes_.back().bindings.back(), rhs.chunk);
      return read_binding(*tgt, scopes_.back().bindings.back());
    }
    const Binding* b = lookup_or_session(*tgt, name);
    // `self` is a binding of every frame, and a stdlib global an immutable
    // root-env one, so a write to either is a reassignment even where nothing
    // bound it here — the same ImmutableError, after the RHS has run.
    if (!b) {
      compile_assign_rhs(ast, av);
      StampGuard pos(*this, ast);
      emit(Op::ImmutErr, kconst_str(name));
      int32_t t = alloc_temp(*tgt);
      return {t, true};  // unreachable
    }
    auto r = compile_assign_rhs(ast, av);
    // Before its declaration runs the name still means the binding the
    // pre-declaration shadows, so the write lands there and is checked
    // against that binding's own mutability.
    if (b->lazy && b->shadowed) return assign_shadowing(ast, *tgt, *b, r);
    {
      // Runtime ImmutableError, after the RHS ran — matching the interp/JIT
      // order and keeping a never-executed assignment silent. The RHS temp
      // strands like any value abandoned by a throw (conservative backstop).
      StampGuard pos(*this, ast);
      if (!emit_rebind(*tgt, *b, r)) return {b->slot, false};  // unreachable
    }
    return read_binding(*tgt, *b);
  }

  // `x op= e` / `x ??= e` on a plain identifier, the compile_assign_var
  // order: the RHS evaluates first, then the current value loads, the step
  // runs, and only the rebind checks mutability — a step TypeError beats
  // the ImmutableError. `??=` reverses the front half: the current value's
  // nil test gates whether the RHS (and every later check) runs at all.
  ExprResult compile_compound_assign(const peg::Ast& ast,
                                     const culebra::AssignmentView& av,
                                     const peg::Ast& tgt) {
    StampGuard pos(*this, ast);
    if (av.is_let || av.is_mut) {
      // Compile-time like the JIT (the interp raises the same SyntaxError
      // when the statement runs).
      throw CulebraError("SyntaxError",
                         "compound assignment cannot declare a new variable.",
                         static_cast<int>(ast.line),
                         static_cast<int>(ast.column));
    }
    auto base = av.op_base;
    const Binding* b = lookup(tgt.token);
    // Only a name that already holds a value has one to step from: a stdlib
    // global does, and at the REPL so does anything the session declared.
    bool global = is_stdlib_global(tgt.token) || is_stdlib_namespace(tgt.token);
    if (!b && ((repl_ && global) || session_declared(tgt.token)))
      b = &bind_session(tgt, std::string(tgt.token));
    if (!b && !global) {
      // Unlike a bare `x = v`, a compound one never declares — it is a
      // NameError. `op=` raises it with the RHS already run (probed); `??=`
      // reads the target before the RHS, so nothing of it runs.
      if (base != "??") compile_assign_rhs(ast, av);
      emit(Op::RaiseErr, 0, kconst_str("NameError"),
           kconst_str(culebra::format(
               "compound assignment on undefined name '{}'", tgt.token)));
      return {alloc_temp(tgt), true};  // unreachable
    }

    if (base == "??") {
      if (!b) {
        // A builtin global never reads nil, so `??=` keeps it and the RHS
        // never runs (the JIT's builtin-compound arm).
        int32_t t = alloc_temp(tgt);
        emit(Op::NsGet, t, kconst_str(tgt.token));
        return {t, true};
      }
      auto cur = read_binding(tgt, *b);
      size_t skip = emit(Op::JumpIfNotNil, cur.slot);
      auto rhs = compile_assign_rhs(ast, av);
      emit_rebind(tgt, *b, rhs);
      patch_to_here(skip);
      return read_binding(tgt, *b);
    }

    Op op = compound_op(ast, base);

    auto rhs = compile_assign_rhs(ast, av);
    ExprResult cur;
    if (b) {
      cur = read_binding(tgt, *b);
    } else {
      int32_t t = alloc_temp(tgt);
      emit(Op::NsGet, t, kconst_str(tgt.token));
      cur = {t, true};
    }
    int32_t t = alloc_temp(ast);
    emit(op, t, cur.slot, rhs.slot, /*inplace=*/1);
    if (!b) {
      // Runtime ImmutableError after the step ran (builtins are immutable
      // root bindings); the step result strands like any throw-abandoned
      // temp (conservative backstop).
      emit(Op::ImmutErr, kconst_str(tgt.token));
      return {t, false};  // unreachable
    }
    // A Tensor step mutates its receiver and hands back that same handle, so
    // nothing is rebound and the target's `mut` does not gate it: the interp
    // returns before env->assign, and the JIT branches around its rebind.
    // Only the rebinding arm consumes the step's temp; the other leaves it to
    // the statement sweep (assign_shadowing's arms are the same shape).
    size_t in_place = emit(Op::JumpIfSame, cur.slot, 0, t);
    emit_rebind(tgt, *b, {t, true});
    patch_to_here(in_place);
    return read_binding(tgt, *b);
  }

  // The compound-step op table, shared by the scalar and index forms;
  // an operator the table doesn't know is rejected.
  Op compound_op(const peg::Ast& ast, std::string_view base) {
    if (base == "+") return Op::Add;
    if (base == "-") return Op::Sub;
    if (base == "*") return Op::Mul;
    if (base == "/") return Op::Div;
    if (base == "%") return Op::Mod;
    if (base == "**") return Op::Pow;
    if (base == "@") return Op::MatMul;
    reject(ast, culebra::format("operator '{}='", base));
  }

  // Complex index lvalue: `a[i] = v`, `a[i] op= v`, `a[i] ??= v`, and
  // chains (`n[i][j] = v` — intermediate INDEX / ARGUMENTS postfixes ride
  // the rvalue fold). A range key compiles like any other and falls into
  // the write ops' Long-key check ("expected Long, got Object") — the
  // probed both-backend behavior for `a[1..2] = v`. Evaluation
  // order is the probed both-backend one: RHS first (plain/compound), then
  // the receiver chain, then the key; `??=` evaluates receiver and key,
  // and the RHS only when the current value is nil. Every op is stamped at
  // the assignment node — the receiver head, where both backends anchor
  // every index error (array_set / object_set_any positions included).
  ExprResult compile_assign_index(const peg::Ast& ast,
                                  const culebra::AssignmentView& av) {
    using namespace peg::udl;
    // A `let` / `mut` prefix on a complex target declares nothing: an
    // element or a property write is the same write with or without it (a
    // dict's own key comes out mutable either way, and an immutable one
    // still refuses — probed on both backends), so the prefix is dropped.
    StampGuard pos(*this, ast);
    size_t end = av.lvaloff + static_cast<size_t>(av.lvalcnt) - 1;
    const auto& fin = *ast.nodes[end];
    if (fin.original_tag == "ARGUMENTS"_) {
      // Lint rejects `f() = v` pre-eval on every backend; mirror the
      // JIT's defensive throw (position backfilled by the wrapper there,
      // explicit here).
      throw CulebraError("SyntaxError",
                         "cannot assign to a function call result.",
                         static_cast<int64_t>(ast.line),
                         static_cast<int64_t>(ast.column));
    }
    auto chain_prefix = [&] {
      return compile_lvalue_prefix(ast, av.lvaloff, end);
    };

    if (fin.original_tag == "DOT"_) {
      // `o.k = v` / `o.k op= v` / `o.k ??= v`. The receiver gate and every
      // object_set throw anchor at the assignment statement; the DOT node
      // is the anchor for the two write-context property errors (the
      // namespace typo and the compound miss), packed into a Long const
      // for the ops that need both positions.
      int32_t name = kconst_str(fin.token);
      int32_t dotpos = kconst_long((static_cast<int64_t>(fin.line) << 32) |
                                   static_cast<int64_t>(fin.column));
      if (av.compound && av.op_base == "??") {
        auto recv = chain_prefix();
        int32_t cur = alloc_temp(ast);
        emit(Op::PropCo, cur, recv.slot, name, dotpos);
        size_t skip = emit(Op::JumpIfNotNil, cur);
        auto rhs = compile_assign_rhs(ast, av);
        emit_prop_set(fin, recv.slot, rhs.slot, /*ns_check=*/false);
        store_into(cur, rhs);
        patch_to_here(skip);
        return {cur, true};
      }
      if (av.compound) {
        Op op = compound_op(ast, av.op_base);
        auto rhs = compile_assign_rhs(ast, av);
        auto recv = chain_prefix();
        int32_t cur = alloc_temp(ast);
        emit(Op::PropWr, cur, recv.slot, name, dotpos);
        int32_t t = alloc_temp(ast);
        emit(op, t, cur, rhs.slot, /*inplace=*/1);
        // No namespace check on the write-back: a namespace's unknown
        // member already fell out of PropWr, and a known one overwrites
        // into its own (immutable) slot — the interp's order. A Tensor step
        // that mutated the receiver in place writes nothing back at all, so
        // an immutable slot does not refuse it (the plain-variable arm above
        // makes the same test).
        size_t in_place = emit(Op::JumpIfSame, cur, 0, t);
        emit_prop_set(fin, recv.slot, t, /*ns_check=*/false);
        patch_to_here(in_place);
        return {t, true};
      }
      auto rhs = compile_assign_rhs(ast, av);
      auto recv = chain_prefix();
      emit_prop_set(fin, recv.slot, rhs.slot, /*ns_check=*/true);
      return rhs;
    }
    if (fin.original_tag != "INDEX"_) reject(fin, "property assignment");

    if (av.compound && av.op_base == "??") {
      auto recv = chain_prefix();
      auto key = compile_expr(fin);
      int32_t cur = alloc_temp(ast);
      emit(Op::IndexCo, cur, recv.slot, key.slot);
      size_t skip = emit(Op::JumpIfNotNil, cur);
      auto rhs = compile_assign_rhs(ast, av);
      emit(Op::IndexSet, recv.slot, key.slot, rhs.slot);
      store_into(cur, rhs);
      patch_to_here(skip);
      return {cur, true};
    }
    if (av.compound) {
      Op op = compound_op(ast, av.op_base);
      auto rhs = compile_assign_rhs(ast, av);
      auto recv = chain_prefix();
      auto key = compile_expr(fin);
      int32_t cur = alloc_temp(ast);
      emit(Op::IndexWr, cur, recv.slot, key.slot);
      int32_t t = alloc_temp(ast);
      emit(op, t, cur, rhs.slot, /*inplace=*/1);
      emit(Op::IndexSet, recv.slot, key.slot, t);
      return {t, true};
    }
    auto rhs = compile_assign_rhs(ast, av);
    auto recv = chain_prefix();
    auto key = compile_expr(fin);
    emit(Op::IndexSet, recv.slot, key.slot, rhs.slot);
    return rhs;
  }

  // The receiver-prefix fold shared by every complex lvalue (`a.b[i].c`'s
  // steps before the final set): each intermediate postfix is a plain
  // rvalue read. `?.` cannot appear — the grammar has no safe-navigating
  // lvalue.
  ExprResult compile_lvalue_prefix(const peg::Ast& at, size_t lvaloff,
                                   size_t end) {
    using namespace peg::udl;
    auto recv = compile_expr(*at.nodes[lvaloff]);
    for (size_t i = lvaloff + 1; i < end; ++i) {
      const auto& post = *at.nodes[i];
      if (post.original_tag == "ARGUMENTS"_)
        recv = compile_call_step(at, post, recv);
      else if (post.original_tag == "INDEX"_)
        recv = compile_index_read(at, post, recv);
      else if (post.original_tag == "DOT"_) {
        if (i + 1 < end && at.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
          recv = compile_method_call(at, post, *at.nodes[i + 1], recv);
          ++i;
        } else {
          recv = compile_property_read(at, post, recv);
        }
      } else
        reject(post, "call chain");
    }
    return recv;
  }

  // The final `.name = value` store, shared by the three DOT assignment
  // forms and a PLACE target: the namespace-typo check at the DOT node
  // (plain writes only — a compound/coalesce write-back's member already
  // resolved), the well-known contract's position publish, then PropSet.
  // The value slot keeps its +1, so callers read it as the expression's
  // value; the statement sweep is its releaser.
  void emit_prop_set(const peg::Ast& dot, int32_t recv, int32_t val,
                     bool ns_check) {
    int32_t name = kconst_str(dot.token);
    int32_t dotpos = kconst_long((static_cast<int64_t>(dot.line) << 32) |
                                 static_cast<int64_t>(dot.column));
    if (ns_check) {
      StampGuard dp(*this, dot);
      emit(Op::NsWrChk, recv, 0, name);
    }
    // The insert path's well-known contract throw is positionless; publish
    // the statement position for those four names (compile_object's rule).
    if (culebra::is_well_known_prop(dot.token)) emit(Op::SetOpPos);
    emit(Op::PropSet, recv, val, name, dotpos);
  }

  // `(a, p[0], o.k) = e` — parallel assignment. The RHS evaluates once and
  // its elements snapshot into temps (SeqGet) before any target writes, so
  // a target aliasing the RHS still reads the pre-write values; the writes
  // then run left-to-right, and a mid-list throw leaves earlier targets
  // written (both backends' probed order — unlike DESTRUCTURE_ASSIGN's
  // two-phase all-or-nothing). Shape mismatch is the destructure
  // ValueError at the statement, checked before anything writes. A
  // plain-name target follows bare `x = v`, declare included.
  ExprResult compile_place_assign(const peg::Ast& ast) {
    using namespace peg::udl;
    auto pv = culebra::view_place_assign(ast);
    StampGuard pos(*this, ast);
    int32_t rv = alloc_temp(ast);
    store_into(rv, compile_expr(*pv.rhs), /*dst_is_fresh=*/true);
    std::vector<size_t> fail;
    fail.push_back(
        emit(Op::SeqChk, rv, 0, static_cast<int32_t>(pv.count), 0));
    std::vector<int32_t> elems(pv.count, -1);
    for (size_t i = 0; i < pv.count; ++i) {
      const auto& t = *ast.nodes[i];
      if (t.tag != "PLACE"_ && t.token == "_") continue;  // sink: no read
      elems[i] = alloc_temp(t);
      emit(Op::SeqGet, elems[i], rv, static_cast<int32_t>(i));
    }
    for (size_t i = 0; i < pv.count; ++i) {
      const auto& t = *ast.nodes[i];
      if (elems[i] < 0) continue;
      if (t.tag == "PLACE"_) {
        compile_place_target(t, elems[i]);
        continue;
      }
      // Bare name: `x = v` semantics, declare included. The element temp's
      // +1 moves into the binding; an immutable name throws here, after
      // the earlier targets already wrote (left-to-right).
      bind_pattern_name(t, t, elems[i], /*src_owned=*/true, /*is_mut=*/false,
                        /*declares=*/false);
    }
    size_t done = emit(Op::Jump);
    for (size_t ix : fail) patch_to_here(ix);
    emit(Op::DestrErr);
    patch_to_here(done);
    return {rv, false};
  }

  // One PLACE target's write with an element temp as the value — the
  // plain (non-compound) final-set arms of compile_assign_index. The
  // element keeps its +1 through the store (IndexSet/PropSet both
  // retain-feed); the statement sweep frees it.
  void compile_place_target(const peg::Ast& place, int32_t val) {
    using namespace peg::udl;
    size_t end = place.nodes.size() - 1;
    const auto& fin = *place.nodes[end];
    if (fin.original_tag == "ARGUMENTS"_) {
      throw CulebraError("SyntaxError",
                         "cannot assign to a function call result.",
                         static_cast<int64_t>(place.line),
                         static_cast<int64_t>(place.column));
    }
    auto recv = compile_lvalue_prefix(place, 0, end);
    if (fin.original_tag == "DOT"_) {
      emit_prop_set(fin, recv.slot, val, /*ns_check=*/true);
      return;
    }
    if (fin.original_tag != "INDEX"_) reject(fin, "property assignment");
    auto key = compile_expr(fin);
    emit(Op::IndexSet, recv.slot, key.slot, val);
  }

  void compile_for(const peg::Ast& ast) {
    using namespace peg::udl;
    auto fv = culebra::view_for(ast);
    const auto& id = *fv.binding;
    // Fast path: `for <ident> in <a>..<b>` walks a Long counter, as the JIT's
    // compile_for_counted_range does. Everything else — a pattern binding, a
    // sink, an unbounded range, any other iterable — opens the protocol.
    if (id.tag == "IDENTIFIER"_ && id.is_token && id.token != "_" &&
        fv.iter->tag == "RANGE"_) {
      auto lay = culebra::decode_range_layout(*fv.iter);
      if (lay.start && lay.end) {
        compile_for_counted_range(ast, fv, lay);
        return;
      }
    }
    compile_for_generic(ast, fv);
  }

  // Walk anything the iterator protocol reaches: an Array/Tuple or a Set's
  // members by index, a String by scalar, and an Object through `iter()` —
  // its own for a user iterator or a generator, the range iterator for a
  // Range, the built-in key iterator otherwise. One cursor run holds all of
  // it, and the loop's scope owns every `+1` in it (see ForSlot).
  void compile_for_generic(const peg::Ast& ast, const culebra::ForView& fv) {
    using namespace peg::udl;
    const auto& id = *fv.binding;
    bool ident = id.tag == "IDENTIFIER"_ && id.is_token;
    bool sink = ident && culebra::is_sink_name(std::string(id.token));
    bool cell =
        ident && !sink && info_->captured_locals.contains(std::string(id.token));

    int32_t broke = alloc_broke_slot(ast, fv.nobreak);
    push_scope(ast);
    int32_t base = alloc_slot(ast, "(for.disposed)");
    alloc_slot(ast, "(for.iterable)");
    alloc_slot(ast, "(for.set.arr)");
    alloc_slot(ast, "(for.src)");
    alloc_slot(ast, "(for.iter)");
    alloc_slot(ast, "(for.elem)");
    alloc_slot(ast, "(for.kind)");
    alloc_slot(ast, "(for.pos)");
    alloc_slot(ast, "(for.count)");
    alloc_slot(ast, "(for.ptr)");
    alloc_slot(ast, "(for.has_next)");
    alloc_slot(ast, "(for.next)");
    scopes_.back().dispose_base = base;
    for_bases_.push_back(base);

    // The iterable is evaluated once, before the loop, and both the
    // not-iterable error and a broken protocol report there.
    stamp(*fv.iter);
    store_into(base + kForIterable, compile_expr(*fv.iter),
               /*dst_is_fresh=*/true);
    emit(Op::ForOpen, base);

    loops_.push_back({next_slot_, defer_scopes_.size(), {}, {}, broke,
                      scopes_.size()});
    size_t head_ix = chunk_.code.size();
    // A step's positionless throws report at the statement, not at the
    // iterable expression the open reports at. The position rides in c/d as
    // immediates: the step runs once per iteration, and a positions-table
    // search there is the loop's hottest constant.
    stamp(ast);
    size_t next_ix = emit(Op::ForNext, base, 0,
                          static_cast<int32_t>(pend_line_),
                          static_cast<int32_t>(pend_col_));
    emit(Op::Safepoint);  // the JIT polls at the top of the body

    {
      // One scope per iteration, holding the binding and the body's own
      // locals: the ladder that closes it releases the newest first, so the
      // body's resources die before the element and both die before the
      // iterator (docs §18.5).
      DeferScope ds(*this, *fv.body);
      push_scope(ast);
      if (sink) {
        emit(Op::Release, base + kForElem);  // nothing binds it
      } else if (ident) {
        int32_t var = alloc_slot(id, cell ? "(for.val)" : std::string(id.token));
        int32_t bind = cell ? alloc_cell_slot(id, std::string(id.token)) : var;
        emit(Op::Take, var, base + kForElem);
        if (cell) emit(Op::CellNew, bind, var);
        push_binding(
            {std::string(id.token), bind, false, cell});
      } else {
        // A destructuring binding: the leaves retain their own sub-elements,
        // so the element's own `+1` is released once the shape matched. A
        // mismatch is an error leaving the loop, and the unwind ladder
        // closes the iterator on the way out.
        //
        // The bind gets its own temp scope because this one runs per
        // iteration: a nested pattern's intermediate reads a container into a
        // temp and leaves it to the sweep (compile_pattern_bind's contract),
        // and the enclosing statement's sweep fires once for the whole loop —
        // so every iteration but the last stranded that `+1`
        // (`for k, (a, b) in {...}` leaked one object per extra entry).
        StampGuard pos(*this, id);
        TempScope pts(*this);
        std::vector<size_t> fail;
        compile_pattern_test(id, base + kForElem, fail);
        compile_pattern_bind(id, base + kForElem, /*subj_owned=*/false, fail,
                             /*is_mut=*/false, /*declares=*/true);
        size_t ok = emit(Op::Jump);
        for (size_t ix : fail) patch_to_here(ix);
        emit(Op::DestrErr);
        patch_to_here(ok);
        emit(Op::Release, base + kForElem);
      }
      predeclare_forward_refs(*fv.body);
      if (fv.body->tag == "STATEMENTS"_) {
        for (const auto& n : fv.body->nodes) compile_statement(*n);
      } else {
        compile_statement(*fv.body);
      }
      ds.close();
      pop_scope();
    }

    emit(Op::Jump, static_cast<int32_t>(head_ix));
    size_t exit_ix = chunk_.code.size();
    patch_jump(next_ix, exit_ix);

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps) patch_jump(j, head_ix);
    for (size_t j : lc.break_jumps) patch_jump(j, exit_ix);
    loops_.pop_back();
    // The drain and break paths close the iterator here rather than from
    // pop_scope's ladder: this instruction is still inside the scope's own
    // unwind range, so a throwing dispose runs that scope's releases on its
    // way out (the ladder itself sits past the range's end). A `return` and
    // an unwind reach it through the ladder and the cleanup step instead —
    // both from inside the range already — and the run's latch is what keeps
    // the three from closing it twice.
    emit(Op::ForDispose, base);
    for_bases_.pop_back();
    pop_scope();
    // After the iterator is closed and the iteration's slots are gone: the
    // order the other backends run it in.
    compile_nobreak_tail(fv.nobreak, broke);
  }

  void compile_for_counted_range(const peg::Ast& ast,
                                 const culebra::ForView& fv,
                                 const culebra::RangeLayout& lay) {
    using namespace peg::udl;
    const auto& id = *fv.binding;
    // A captured loop variable gets a fresh cell each iteration (the
    // interp's per-iteration scope): ForLoop keeps writing a hidden plain
    // slot, and the body opens with a CellNew from it — so every closure
    // made in iteration N holds iteration N's value.
    bool cell = info_->captured_locals.contains(std::string(id.token));

    int32_t broke = alloc_broke_slot(ast, fv.nobreak);
    push_scope(ast);
    int32_t base = alloc_slot(ast, "(for.cur)");
    alloc_slot(ast, "(for.end)");
    alloc_slot(ast, "(for.step)");
    alloc_slot(ast, "(for.done)");
    int32_t var = alloc_slot(id, cell ? "(for.val)" : std::string(id.token));
    int32_t bind = var;
    if (cell) bind = alloc_cell_slot(id, std::string(id.token));

    // Endpoints evaluate before the binding exists, in source order, with
    // errors attributed to the range expression — same as both backends. Each
    // is ChkLong'd right after it compiles, compile_range's order and for its
    // reason: the counter is a Long, and a Float bound is the interpreter's
    // TypeError rather than something to truncate (`for i in 1.5..3` ran no
    // iterations here, and `by 0.5` walked a step of its own invention).
    stamp(*fv.iter);
    store_into(base + 0, compile_expr(*lay.start), /*dst_is_fresh=*/true);
    emit(Op::ChkLong, base + 0);
    store_into(base + 1, compile_expr(*lay.end), /*dst_is_fresh=*/true);
    emit(Op::ChkLong, base + 1);
    if (lay.step) {
      store_into(base + 2, compile_expr(*lay.step), /*dst_is_fresh=*/true);
      emit(Op::ChkLong, base + 2);
    } else {
      emit(Op::LoadConst, base + 2, kconst_long(1));
    }
    size_t prep = emit(Op::ForPrep, base);

    push_binding({std::string(id.token), bind, false, cell});
    loops_.push_back({next_slot_, defer_scopes_.size(), {}, {}, broke,
                      scopes_.size()});
    size_t body_ix = chunk_.code.size();
    if (cell) emit(Op::CellNew, bind, var);
    compile_block(*fv.body);
    size_t safepoint_ix = emit(Op::Safepoint);
    patch_jump(prep, chunk_.code.size());
    emit(Op::ForLoop, base, static_cast<int32_t>(body_ix), var,
         lay.inclusive ? 1 : 0);
    size_t exit_ix = chunk_.code.size();

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps) patch_jump(j, safepoint_ix);
    for (size_t j : lc.break_jumps) patch_jump(j, exit_ix);
    loops_.pop_back();
    pop_scope();
    compile_nobreak_tail(fv.nobreak, broke);
  }

  // A loop's `nobreak { … }` tail: run the block unless a break set the
  // flag. Emitted where the loop's own teardown has already happened (the
  // iterator is closed, the iteration's bindings are gone) — the point both
  // other backends run it from.
  void compile_nobreak_tail(const peg::Ast* nobreak, int32_t broke_slot) {
    if (!nobreak) return;
    size_t skip = emit(Op::JumpIfTrue, broke_slot);
    compile_block(*nobreak);
    patch_to_here(skip);
  }

  // The flag slot a loop with a `nobreak` clause needs, cleared before the
  // loop runs; -1 when the loop has no clause.
  int32_t alloc_broke_slot(const peg::Ast& ast, const peg::Ast* nobreak) {
    if (!nobreak) return -1;
    int32_t s = alloc_slot(ast, "(loop.broke)");
    emit(Op::LoadConst, s, kconst_long(0));
    return s;
  }

  void compile_while(const peg::Ast& ast) {
    auto wv = culebra::view_while(ast);
    // The init bindings are evaluated once, before the first condition, and
    // stay live through the `nobreak` tail — hence the scope around it all.
    InitScope init(*this, ast, wv.init);
    int32_t broke = alloc_broke_slot(ast, wv.nobreak);

    // The condition re-evaluates every iteration, so its temps must be
    // swept inside the iteration — and before the exit branch, or a heap
    // temp materialised by the condition would stay live on the exit path.
    // The Bool result survives the sweep in a dedicated slot.
    int32_t cond_slot = alloc_temp(ast);
    size_t top_ix = chunk_.code.size();
    emit(Op::Safepoint);
    {
      size_t base = stmt_temps_.size();
      int32_t slot_base = next_slot_;
      store_into(cond_slot, compile_expr(*wv.cond));
      sweep_temps(base, slot_base);
    }
    size_t exit_jump = emit(Op::JumpIfFalse, cond_slot);

    loops_.push_back({next_slot_, defer_scopes_.size(), {}, {}, broke,
                      scopes_.size()});
    compile_block(*wv.body);
    emit(Op::Jump, static_cast<int32_t>(top_ix));
    size_t exit_ix = chunk_.code.size();
    patch_jump(exit_jump, exit_ix);

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps) patch_jump(j, top_ix);
    for (size_t j : lc.break_jumps) patch_jump(j, exit_ix);
    loops_.pop_back();
    compile_nobreak_tail(wv.nobreak, broke);
  }

  // The stdlib names the slice reaches: kBuiltinFns' bare native globals and
  // the lazy source modules' bare functions (`assert_eq`, the matcher family,
  // `replace`), all resolvable through culebra_runtime_namespace_get. The
  // lazy half works because this lane compiles the preamble now, which is
  // what registers the builder closure the resolver invokes. The effects
  // primitives stay out (their lowered declarations are declined outright).
  static bool is_stdlib_global(std::string_view name) {
    return _is_bare_stdlib_fn(name);
  }

  // The namespace VALUES the slice reaches: the natively built ones, taken
  // straight from the resolver's own predicate (the ns groups / wrap.h classes /
  // constant groups) so the two cannot drift, plus the lazy source modules —
  // built on demand by the builder the compiled preamble registered.
  // Which bare names compile to a resolver lookup instead of a binding: the
  // JIT's own answer (`is_builtin_var` — the same predicate the shared
  // free-var analysis was handed), so the two cannot drift. It covers the
  // natively built namespaces, the internal primitive ones the preamble
  // itself calls (`_Time`, `_Canvas`, ...), and the lazy source modules.
  static bool is_stdlib_namespace(std::string_view name) {
    return is_builtin_var(std::string(name));
  }

  // Direct 1-positional-arg call to the unshadowed global `name` — the
  // dedicated-op peephole, mirroring the JIT's own direct emits
  // (stdlib_jit.h), so both compiled lanes skip the resolver + closure
  // invoke for the common shape. `println` takes it because the call is the
  // whole statement; `to_float` because the conversion is two tag tests and
  // the invoke costs more than the work. Every other shape (bare `f()`,
  // wrong arity, kwargs, the name as a value, a shadowing binding) takes the
  // generic NsGet + Call route and the runtime's own diagnostics.
  bool is_direct_global_call(const peg::Ast& ast, std::string_view name) {
    using namespace peg::udl;
    if (ast.nodes.size() != 2 || ast.nodes[0]->tag != "IDENTIFIER"_ ||
        ast.nodes[0]->token != name || lookup(name) ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_ ||
        ast.nodes[1]->nodes.size() != 1)
      return false;
    const auto& a0 = *ast.nodes[1]->nodes[0];
    return !is_kwarg(a0) && !is_kwarg_splat(a0);
  }

  StaticCallee head_callee(const peg::Ast& head, const ExprResult& res) {
    using namespace peg::udl;
    if (res.chunk >= 0) return {res.chunk};
    if (head.tag != "IDENTIFIER"_) return {};
    const Binding* b = lookup(head.token);
    if (!b) return {};
    return {b->known.chunk, b->known.cell,
            b->known.via_mono ? Chunk::Reach::Mono : Chunk::Reach::Direct};
  }

  // What `Name.new(...)` reaches. A constructor arrives through the class
  // object rather than by being the head, so it is the one step down a
  // postfix chain that resolves — and only that step: `f().new(...)` is
  // whatever the call returned, and `a.b.new(...)` is whatever `a.b` is.
  // Asked at the property call itself, which is where the shape is still
  // visible and where the CallM it answers for is emitted.
  StaticCallee postfix_ctor_callee(const peg::Ast& at, const peg::Ast& post) {
    using namespace peg::udl;
    if (at.nodes.size() < 2 || at.nodes[1].get() != &post ||
        post.original_tag != "DOT"_ || post.token != "new")
      return {};
    const auto& head = *at.nodes[0];
    if (head.tag != "IDENTIFIER"_) return {};
    const Binding* b = lookup(head.token);
    if (!b) return {};
    // `lazy` is exactly "the value of this name is still a run-time
    // question": a member's own class name is read off the receiver, and a
    // forward reference holds a sentinel until its declaration runs. Both
    // keep the chunk and check it at the site.
    return {b->known.ctor, b->known.cell,
            b->lazy ? Chunk::Reach::Guarded : Chunk::Reach::Direct};
  }

  // Postfix chain: `f(x)`, `a[i]`, `a?[i]`, `x.p`, `x.m(...)`, `x?.m(...)`,
  // `x!!`, and their compositions (`n[i][j]`, `f()[k]`, `Math.abs(-3).nope`),
  // folded left to right over one rolling result — the same walk
  // compile_assign_index runs over a receiver prefix. A `?[` / `?.` chain
  // routes every exit through one merge temp (begin_safe_nav's diamond,
  // compile_if's nil-fresh result discipline): each guard's JumpIfNil
  // targets the merge point past the store, so a nil receiver skips the
  // rest of the chain — key expressions, arguments and `!!` checks
  // included — and leaves the merge temp nil.
  ExprResult compile_call(const peg::Ast& ast) {
    using namespace peg::udl;
    bool has_safe = false;
    for (size_t i = 1; i < ast.nodes.size(); ++i) {
      auto t = ast.nodes[i]->original_tag;
      if (t == "SAFE_INDEX"_ || t == "SAFE_DOT"_) has_safe = true;
    }
    int32_t out = has_safe ? alloc_temp(ast) : -1;
    std::vector<size_t> nil_jumps;
    // Direct `fn(...)` recursion in a receiver frame re-passes the frame's
    // own receiver, so the callee sees the same one — the JIT's
    // fn_direct_call, which reads the raw slot rather than minting the
    // wrapper compile_expr would.
    bool fn_direct = ast.nodes[0]->tag == "IDENTIFIER"_ &&
                     ast.nodes[0]->token == "fn" &&
                     chunk_.fn_bound_slot >= 0 && ast.nodes.size() > 1 &&
                     ast.nodes[1]->original_tag == "ARGUMENTS"_;
    if (auto r = compile_lazy_ns_register(ast)) return *r;
    // A head the call can read out of its cell when it runs, rather than
    // copying into a register first (borrowed_call_head). `res.slot` is then
    // the CELL's slot, which only the Call the next loop turn emits knows how
    // to read — so this is the one shape where the head result is not a value.
    const Binding* borrowed = fn_direct ? nullptr : borrowed_call_head(ast);
    auto res = fn_direct  ? ExprResult{chunk_.fn_slot, false}
               : borrowed ? read_borrowed_head(*ast.nodes[0], *borrowed)
                          : compile_expr(*ast.nodes[0]);
    for (size_t i = 1; i < ast.nodes.size(); ++i) {
      const auto& post = *ast.nodes[i];
      if (post.original_tag == "ARGUMENTS"_) {
        // Only the head's own call reaches a callee the compiler can name —
        // `f()()` calls whatever the first call returned. A direct `fn(...)`
        // re-enters the very chunk it runs in.
        StaticCallee tgt{};
        if (i == 1)
          tgt = fn_direct ? StaticCallee{chunk_idx_}
                          : head_callee(*ast.nodes[0], res);
        res = i == 1 && fn_direct
                  ? compile_self_call_step(ast, post, res, tgt)
                  : compile_call_step(ast, post, res, tgt,
                                      /*callee_in_cell=*/i == 1 && borrowed);
      } else if (post.original_tag == "INDEX"_)
        res = compile_index_read(ast, post, res);
      else if (post.original_tag == "SAFE_INDEX"_) {
        nil_jumps.push_back(emit(Op::JumpIfNil, res.slot));
        res = compile_index_read(ast, post, res);
      } else if (post.original_tag == "DOT"_ ||
                 post.original_tag == "SAFE_DOT"_) {
        if (post.original_tag == "SAFE_DOT"_)
          nil_jumps.push_back(emit(Op::JumpIfNil, res.slot));
        if (i + 1 < ast.nodes.size() &&
            ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
          res = compile_method_call(ast, post, *ast.nodes[i + 1], res);
          ++i;  // consume ARGUMENTS
        } else {
          res = compile_property_read(ast, post, res);
        }
      } else if (post.original_tag == "NONNULL"_) {
        StampGuard pos(*this, post);
        emit(Op::NilChk, res.slot);
      } else
        reject(post, "call chain");
    }
    if (!has_safe) return res;
    store_into(out, res, /*dst_is_fresh=*/true);
    for (size_t j : nil_jumps) patch_to_here(j);
    return {out, true};
  }

  // One `.name` postfix with no call after it: the property value read,
  // followed by the bare built-in method reject when the name could be one
  // (the JIT emits that cold check under the same compile-time filter).
  ExprResult compile_property_read(const peg::Ast& at, const peg::Ast& post,
                                   ExprResult recv) {
    int32_t t;
    {
      StampGuard pos(*this, at);
      t = alloc_temp(at);
      emit(Op::PropVal, t, recv.slot, kconst_str(post.token));
    }
    if (culebra::is_builtin_method_name(post.token)) {
      StampGuard pos(*this, post);  // the method name's own position
      emit(Op::BareMethChk, t, recv.slot, kconst_str(post.token));
    }
    return {t, true};
  }

  // The method names the slice cannot answer. Each is decided by NAME (and,
  // for a built-in, by argument count) at compile time — the receiver's type
  // is only known at run time, and for these the JIT resolves against
  // machinery with no runtime counterpart the executor could call:
  //   - a built-in method the table above does not carry at this arity is
  //     inline tag dispatch (compile_builtin_method); `Math.keys()` is the
  //     dict builtin on a namespace object, not the closed-member read the
  //     property path would do
  //   - `drop` / `parameters` have their own dispatches (explicit_drop, the
  //     synthesized class walker)
  //   - a name that resolves as a free function is a UFCS candidate:
  //     `3.dbl()` is `dbl(3)`, decided by a runtime has-property gate — so it
  //     is rejected even when the table implements the built-in, whose own
  //     gate has no UFCS arm
  // The sets come from shared tables (builtin_method_names, kBuiltinFns), so
  // the boundary cannot drift from the backends'.
  //
  // The UFCS test only consults THIS chunk's scopes, and that is the whole
  // test: FnAnalysis already counts a candidate living in an enclosing frame
  // as a free variable of the reading function, so resolve_captures rejects it
  // at the fn literal ("UFCS candidate capture of ..."), and a candidate that
  // is a builtin never enters scopes_ at all — is_stdlib_global catches those.
  // `.name` / `.params` / `.return_type` on a Function receiver are answered
  // by culebra_runtime_fn_introspect_get, which reads the signature out of a
  // side table keyed by code address. The executor's closures all share one
  // address, so they answer through _jit_closure_meta_hook instead — the seam
  // the keyword resolver and the arity check already use — and the chunk meta
  // the hook hands back carries the name, the return type and the parameters.

  // The compile-time UFCS candidate for `name` at this site, if any:
  // a scope binding first (Function-ness is a
  // runtime fact; a non-Function declines the gate there), then a bare
  // stdlib global the resolver owns. A built-in that subsumes its global
  // (`to_string`) needs no UFCS arm: the built-in performs the same
  // conversion on every receiver the global would have taken. Only the
  // stdlib half of the test is exempt — a name the user declared shadows
  // the built-in on both backends, and that stays out of slice.
  // The static half of the UFCS gate, baked into the HasProp operand: the
  // tags whose own built-in table binds this name, plus a bit saying the name
  // is an iterator-protocol one (which an iterator-shaped Object resolves as
  // well). Read from the very tables interp's receiver_has_property
  // consults, so the lanes cannot disagree about which receiver owns a name.
  static int32_t has_prop_gate_operand(std::string_view name) {
    int32_t m = 0;
    for (auto& [t, tbl] : builtin_value_tables())
      if (tbl->count(name)) m |= bmeth_tag_bit(t);
    if (fn_introspection_name(name)) m |= bmeth_tag_bit(TAG_FUNC);
    // A dict builtin resolves on every Object, whatever its shape.
    if (dict_builtin_table()->count(name)) m |= bmeth_tag_bit(TAG_OBJECT);
    if (culebra::canon_iterator_sigs().count(name)) m |= kHasPropIterBit;
    return m;
  }

  enum class UfcsCand { None, Binding, Global };
  UfcsCand ufcs_candidate(std::string_view name, const BMethSpec* spec) {
    if (lookup(name)) return UfcsCand::Binding;
    bool subsumes = spec && spec->subsumes_global;
    if (is_stdlib_global(name) && !subsumes) return UfcsCand::Global;
    return UfcsCand::None;
  }

  // The check the JIT emits in front of an unresolved built-in method call,
  // as one op over a baked table. Every verdict comes from the interp's own
  // parameter lists, so the three lanes cannot disagree; a receiver whose
  // table BINDS this shape is one the slice has no implementation for, and
  // that stays a rejection rather than a guess.
  void emit_builtin_arity_check(const peg::Ast& at, const peg::Ast& post,
                                const peg::Ast& args, ExprResult recv) {
    std::string method(post.token);
    if (!culebra::is_builtin_method_name(method)) return;
    auto scan = culebra::scan_arg_list(args);
    auto argc = static_cast<int64_t>(args.nodes.size());
    std::vector<Chunk::ArityArm> arms;
    auto take = [&](const auto& tbl, int8_t tag) {
      auto v = builtin_call_verdict(tbl, method, scan, argc);
      if (v.kind == BuiltinVerdict::Kind::Valid)
        reject(post, culebra::format("built-in method '{}'", method));
      if (v.kind != BuiltinVerdict::Kind::Error) return false;
      // Rich errors anchor at the call's callee (a parenthesized receiver
      // puts the CALL node on the `(`, which is not where the interp
      // reports), count-based ones at the ARGUMENTS node.
      auto [rl, rc] = culebra::call_callee_position(at);
      arms.push_back({tag, kconst_str(v.err_kind), kconst_str(v.msg),
                      kconst_str(method),
                      static_cast<uint32_t>(v.at_call_root ? rl : args.line),
                      static_cast<uint32_t>(v.at_call_root ? rc
                                                           : args.column)});
      return true;
    };
    for (auto& [t, tbl] : builtin_value_tables()) take(*tbl, t);
    // Any Object resolves the dict table; only an iterator-shaped one reaches
    // the iterator table — eval_property's order, so at most one applies. Both
    // stand behind the receiver's OWN members, which win over either table
    // (`range(0, 2)` carries its own `iter`, and takes no diagnostic from the
    // dict one) — the arm carries the name so the check can ask.
    if (!take(*dict_builtin_table(), Chunk::kArityObj))
      take(culebra::canon_iterator_sigs(), Chunk::kArityIter);
    if (arms.empty()) return;
    StampGuard pos(*this, args);
    emit(Op::BArity, recv.slot,
         static_cast<int32_t>(chunk_.arity_checks.size()));
    chunk_.arity_checks.push_back(std::move(arms));
  }

  // The keyword diagnostics a BUILT-IN owes, over the same per-receiver arms
  // the arity check uses. Two errors live here, in the interp's own order:
  // a keyword a built-in cannot bind (any name but a keyword-only
  // parameter's, and every `**` splat), and then the argument list's
  // structural errors. Both are raised before a single argument runs, and
  // only by a receiver that resolves the name in a built-in table — a member
  // of the receiver's own binds the call itself, which the arms' name test
  // leaves alone. Answers whether the call is doomed, so the caller can send
  // what is left (a member of that name, a miss) down the property read.
  bool emit_builtin_kwargs_error(const peg::Ast& post, const peg::Ast& args,
                                 ExprResult recv) {
    std::string method(post.token);
    if (!culebra::is_builtin_method_name(method)) return false;
    std::string kind, msg;
    auto line = static_cast<uint32_t>(args.line);
    auto col = static_cast<uint32_t>(args.column);
    if (!builtin_method_keywords_bindable(method, args)) {
      kind = "TypeError";
      msg = culebra::builtin_method_kwargs_error_message(method);
    } else if (auto e = culebra::check_arg_list(args)) {
      kind = e->kind;
      msg = e->message;
      line = static_cast<uint32_t>(e->line);
      col = static_cast<uint32_t>(e->col);
    } else {
      return false;
    }
    std::vector<Chunk::ArityArm> arms;
    auto add = [&](int8_t tag) {
      arms.push_back({tag, kconst_str(kind), kconst_str(msg),
                      kconst_str(method), line, col});
    };
    auto has_name = [&](const auto& tbl) {
      return builtin_method_sig(tbl, method) != nullptr;
    };
    for (auto& [t, tbl] : builtin_value_tables())
      if (has_name(*tbl)) add(t);
    // eval_property's order: any Object resolves the dict table, and only an
    // iterator-shaped one reaches the iterator table.
    if (has_name(*dict_builtin_table()))
      add(Chunk::kArityObj);
    else if (has_name(culebra::canon_iterator_sigs()))
      add(Chunk::kArityIter);
    if (arms.empty()) return false;
    StampGuard pos(*this, args);
    emit(Op::BArity, recv.slot,
         static_cast<int32_t>(chunk_.arity_checks.size()));
    chunk_.arity_checks.push_back(std::move(arms));
    return true;
  }

  // The at-most-once explicit drop. The guard only reads the receiver, so the
  // op borrows it — but this call is still the receiver's last reader on its
  // path, so an owned one is released here rather than left to the statement
  // sweep. It has to be: inside the UFCS dispatch the candidate arm hands the
  // same receiver to the call, and that Take drops the temp from the sweep
  // list for every arm, so a borrow here stranded the +1 whenever the drop
  // arm was the one that ran (`let drop = 5; ([1, 2]).drop()` leaked the
  // array). Consuming unconditionally keeps one rule for every arm instead of
  // one per dispatch shape. The call's value is nil whatever the drop body
  // returned.
  ExprResult emit_explicit_drop(const peg::Ast& at, ExprResult r) {
    int32_t t = alloc_temp(at);
    emit(Op::Drop, t, r.slot);
    if (r.owned) {
      emit(Op::Release, r.slot);
      forget_temp(r.slot);
    }
    return {t, true};
  }

  // `x.parameters(...)`: the synthesized walker when the receiver is a class
  // instance resolving no `parameters` of its own, and the ordinary dispatch
  // otherwise. compile_class_parameters_call's shape, with its `useAuto`
  // check as one op and its other two arms folded into the general tail —
  // that tail's HasProp gate asks exactly what the JIT's propBB asks.
  ExprResult compile_class_parameters(const peg::Ast& at, const peg::Ast& post,
                                      const peg::Ast& args, ExprResult recv) {
    StampGuard pos(*this, at);
    int32_t out = alloc_temp(at);
    int32_t gate = alloc_temp(at);
    emit(Op::ClsParamsChk, gate, recv.slot);
    size_t to_normal = emit(Op::JumpIfFalse, gate);
    {
      // The arguments run on every arm; here they are only evaluated.
      TempScope ts(*this);
      for (const auto& a : args.nodes) compile_expr(*a);
    }
    emit(Op::ClsParamsWalk, out, recv.slot);
    // The walker only borrows, but this arm is the receiver's last reader —
    // and the other arms consume it. Release here rather than leaving it to
    // the statement sweep: a temporary receiver's automatic drop runs as soon
    // as the call is over, ahead of whatever the result feeds.
    if (recv.owned) emit(Op::Release, recv.slot);
    size_t done = emit(Op::Jump);
    patch_to_here(to_normal);
    store_into(out, compile_method_tail(at, post, args, recv),
               /*dst_is_fresh=*/true);
    patch_to_here(done);
    return {out, true};
  }

  // `x.m(args)` — compile_method_call's property tail. The property resolves
  // FIRST, before any argument compiles: that is the probed order on both
  // backends (a namespace's AttributeError fires without evaluating the
  // arguments; a plain dict's miss evaluates them and then fails the
  // TAG_FUNC check). Receiver and arguments then share one contiguous run
  // with the receiver at its head — CallM hands that head over as `self`,
  // the JitFn ABI's receiver, instead of minting a bound-method wrapper.
  ExprResult compile_method_call(const peg::Ast& at, const peg::Ast& post,
                                 const peg::Ast& args, ExprResult recv) {
    // The synthesized `parameters()`: a class instance that resolves no
    // `parameters` of its own answers with the walker over its fields, and
    // nothing else about the call matters — surplus positional arguments are
    // evaluated and ignored, as they are for any other 0-parameter method.
    // Everything the check rules out falls into the ordinary dispatch, whose
    // HasProp gate asks the very question the JIT's own `else` arm asks (an
    // own slot, a trait default, or a namespace).
    if (post.token == "parameters" && !has_kwargs(args))
      return compile_class_parameters(at, post, args, recv);
    return compile_method_tail(at, post, args, recv);
  }

  ExprResult compile_method_tail(const peg::Ast& at, const peg::Ast& post,
                                 const peg::Ast& args, ExprResult recv) {
    // By POSITIONAL count: a keyword binds by name, so it is not one of the
    // arguments a built-in's shape is written in.
    const BMethSpec* spec =
        bmeth_lookup(post.token, positional_args(args).size());
    // Explicit `x.drop()` runs the at-most-once guard rather than a method:
    // an explicit drop suppresses the automatic one. interp's UFCS block sits
    // ABOVE it, so a receiver resolving no `drop` of its own hands the call to
    // a free `drop`, and a non-Function binding of the name comes back here.
    bool is_drop = post.token == "drop" && args.nodes.empty();
    // The call a receiver that DOES resolve this name gets: the built-in when
    // the table carries this shape, otherwise the diagnostic those receivers
    // owe (arity check first) and
    // then the ordinary property read.
    auto resolved_call = [&](ExprResult r) -> ExprResult {
      if (is_drop) return emit_explicit_drop(at, r);
      // A built-in method binds positionally; the one keyword it takes names
      // a keyword-only parameter, and the spec carries that slot. A keyword
      // it cannot bind dooms the call for every receiver that resolves the
      // name as a built-in, and what is left — a member of that name, a miss
      // — takes the ordinary property read either way.
      if (has_kwargs(args)) {
        if (emit_builtin_kwargs_error(post, args, r))
          return compile_property_call(at, post, args, r);
        if (spec && spec->kw)
          return compile_builtin_method(at, post, args, r, *spec);
        emit_builtin_arity_check(at, post, args, r);
        return compile_property_call(at, post, args, r);
      }
      if (spec) return compile_builtin_method(at, post, args, r, *spec);
      emit_builtin_arity_check(at, post, args, r);
      return compile_property_call(at, post, args, r);
    };
    // What a declined candidate leaves behind. For `drop` that is the guard
    // again (the JIT hands compile_resolved_or_ufcs the same body twice).
    auto declined_call = [&](ExprResult r) -> ExprResult {
      if (is_drop) return emit_explicit_drop(at, r);
      return compile_property_call(at, post, args, r);
    };
    // A built-in that subsumes its same-named global (`to_string`) does so
    // only for a call it can bind: with a keyword it cannot, the global is
    // the callee the interp reaches, and its own binder answers.
    UfcsCand cand =
        ufcs_candidate(post.token, has_kwargs(args) ? nullptr : spec);
    if (cand == UfcsCand::None) return resolved_call(recv);
    // A visible candidate: the runtime gate decides, interp's eval_call
    // order — resolution first, arguments inside whichever arm runs. Every
    // arm writes the merge temp once per disjoint path (compile_if's
    // discipline), and each consumes the receiver exactly once at runtime
    // (a Take fires only on the arm that executes).
    StampGuard pos(*this, at);
    int32_t out = alloc_temp(at);
    int32_t gate = alloc_temp(at);
    emit(Op::HasProp, gate, recv.slot, kconst_str(post.token),
         has_prop_gate_operand(post.token));
    size_t to_ufcs = emit(Op::JumpIfFalse, gate);
    // hit arm: the receiver resolves the name itself.
    store_into(out, resolved_call(recv), /*dst_is_fresh=*/true);
    size_t done_hit = emit(Op::Jump);
    patch_to_here(to_ufcs);
    // The candidate load. A lazy dispatcher cell's sentinel declines the
    // Function test below like interp's pre-decl env miss (no UnboundErr);
    // NsGet is the resolver's cached closure, always a Function.
    const Binding* cb =
        cand == UfcsCand::Binding ? lookup(post.token) : nullptr;
    ExprResult candv = cb ? read_binding(post, *cb, /*unbound_guard=*/false)
                          : [&] {
                              int32_t t = alloc_temp(post);
                              emit(Op::NsGet, t, kconst_str(post.token));
                              return ExprResult{t, true};
                            }();
    if (cb && cb->lazy && is_stdlib_global(post.token)) {
      // A predeclared `fn` shadowing a stdlib global: until its decl
      // statement runs, interp's env walk still resolves the GLOBAL, so a
      // sentinel substitutes the resolver's closure instead of declining.
      // The sentinel is not RC'd, so NsGet's +1 overwrites it in place.
      size_t subst = emit(Op::JumpIfTag, candv.slot, 0, TAG_NO_SELF);
      size_t over = emit(Op::Jump);
      patch_to_here(subst);
      emit(Op::NsGet, candv.slot, kconst_str(post.token));
      patch_to_here(over);
    }
    size_t to_call = emit(Op::JumpIfTag, candv.slot, 0, TAG_FUNC);
    // A non-Function candidate declines, and the gate already said this
    // receiver does not resolve the name — so what is left is the ordinary
    // property read and its own errors (a scalar's member TypeError before
    // the arguments run, an object-ish miss after). NOT resolved_call: a
    // built-in with no receiver gate at all (`to_string`) would answer here
    // over a receiver the gate just ruled out.
    store_into(out, declined_call(recv), /*dst_is_fresh=*/true);
    size_t done_miss = emit(Op::Jump);
    patch_to_here(to_call);
    // UFCS: `name(receiver, args...)`. The receiver rides as positional[0];
    // the call site is the ARGUMENTS node (interp's eval_ufcs_call, the
    // JIT's CallSiteAt(argsAst)).
    if (has_kwargs(args)) {
      // The resolver binds the names against the candidate's own parameters.
      // No call boundary is published here — the JIT leaves its kwargs arm
      // unwired too, so a position-less error keeps reporting at the call
      // site on every backend.
      StampGuard call_pos(*this, args);
      store_into(out,
                 compile_kwargs_call(at, args, candv.slot, /*recv=*/nullptr,
                                     &recv, &post),
                 /*dst_is_fresh=*/true);
    } else {
      int32_t argc = static_cast<int32_t>(args.nodes.size());
      int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
      alloc_temp(at);             // [0] = the receiver
      for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
      store_into(base, recv, /*dst_is_fresh=*/true);
      for (int32_t i = 0; i < argc; i++)
        store_into(base + 1 + i, compile_expr(*args.nodes[i]),
                   /*dst_is_fresh=*/true);
      int32_t t = alloc_temp(at);
      emit(Op::BoundPos);  // ambient stamp = the chain node `at`
      {
        StampGuard call_pos(*this, args);
        size_t ix = emit(Op::Call, t, candv.slot, base, argc + 1);
        // positional[0] is the receiver: it has no argument expression, and
        // the interp reports a parameter error on it at the method name.
        std::vector<const peg::Ast*> asts{&post};
        for (const auto& a : args.nodes) asts.push_back(a.get());
        record_call_argpos(ix, args, std::move(asts));
      }
      store_into(out, {t, true}, /*dst_is_fresh=*/true);
    }
    patch_to_here(done_hit);
    patch_to_here(done_miss);
    return {out, true};
  }

  // The no-candidate tail (and both non-UFCS arms of the gated form):
  // resolve the property, then the arguments, then the JitFn call.
  ExprResult compile_property_call(const peg::Ast& at, const peg::Ast& post,
                                   const peg::Ast& args, ExprResult recv) {
    StampGuard pos(*this, at);
    int32_t callee = alloc_temp(at);
    emit(Op::PropRaw, callee, recv.slot, kconst_str(post.token));
    if (has_kwargs(args))
      return compile_kwargs_call(at, args, callee, &recv);
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    alloc_temp(at);             // [0] = the receiver
    for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
    store_into(base, recv, /*dst_is_fresh=*/true);
    // A promoted body local of a lowering's state object is storage, not a
    // method: calling one passes no receiver, which is a run-time fact about
    // the receiver's proto, asked at this same point.
    emit(Op::CallRecv, base, 0, kconst_str(post.token));
    for (int32_t i = 0; i < argc; i++)
      store_into(base + 1 + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(at);
    size_t ix = emit(Op::CallM, t, callee, base, argc);
    record_call_target(ix, postfix_ctor_callee(at, post), argc);
    std::vector<const peg::Ast*> asts;
    for (const auto& a : args.nodes) asts.push_back(a.get());
    record_call_argpos(ix, args, std::move(asts));
    return {t, true};
  }

  // `x.m(args)` where `m` is a built-in the table implements at this arity.
  // The run is one slot wider than a plain method call's: its head is
  // MethGate's gate slot, which decides between the built-in and a
  // user-defined method of the same name and reaches BMeth as the choice.
  // The emission order IS the evaluation order both backends show: resolve
  // the property and gate the receiver, then the arguments left to right,
  // each type-checked at its own position the moment it lands.
  ExprResult compile_builtin_method(const peg::Ast& at, const peg::Ast& post,
                                    const peg::Ast& args, ExprResult recv,
                                    const BMethSpec& spec) {
    StampGuard pos(*this, at);  // the chain head: the gate's and BMeth's anchor
    auto pos_args = positional_args(args);
    const peg::Ast* kw_val = spec.kw ? kwarg_value(args, spec.kw) : nullptr;
    int32_t argc = static_cast<int32_t>(pos_args.size());
    // A keyword-bearing call needs its own merge temp: the gate's answer
    // decides between the built-in and the resolver, and only one arm runs.
    int32_t out = kw_val ? alloc_temp(at) : -1;
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    alloc_temp(at);             // [0] = the gate (user method or sentinel)
    alloc_temp(at);             // [1] = the receiver
    for (int32_t i = 0; i < spec.nargs; i++)
      alloc_temp(i < argc ? *pos_args[i] : at);
    emit(Op::MethGate, base, recv.slot, kconst_str(post.token),
         bmeth_spec_index(spec));
    size_t to_builtin = 0;
    size_t done_user = 0;
    if (kw_val) {
      // A member of the receiver's own binds the keyword the way any other
      // call does — through the resolver, against that member's parameters.
      // The gate slot IS that member, so the property is read only once (a
      // getter of the name must not run twice).
      to_builtin = emit(Op::JumpIfTag, base, 0, TAG_NO_SELF);
      store_into(out, compile_kwargs_call(at, args, base, &recv),
                 /*dst_is_fresh=*/true);
      done_user = emit(Op::Jump);
      patch_to_here(to_builtin);
    }
    store_into(base + 1, recv, /*dst_is_fresh=*/true);
    // A name two receivers spell at different arities owes an ArityError,
    // not a method miss — and the interp raises it before the arguments run.
    if (bmeth_has_rival_arity(spec)) {
      StampGuard args_pos(*this, args);
      emit(Op::ArityChk, base, 0, bmeth_spec_index(spec));
    }
    for (int32_t i = 0; i < argc; i++)
      store_into(base + 2 + i, compile_expr(*pos_args[i]),
                 /*dst_is_fresh=*/true);
    // The keyword's value is written last, which is also where the source
    // puts it: a keyword may not precede a positional argument.
    if (kw_val)
      store_into(base + 2 + argc, compile_expr(*kw_val),
                 /*dst_is_fresh=*/true);
    // Every argument runs before any is bound — the interp binder's order,
    // so the checks come as one run after the whole list, each still stamped
    // at its own argument.
    for (int32_t i = 0; i < argc; i++) {
      if (spec.params[i] == BParam::Any) continue;  // undeclared: no check
      StampGuard arg_pos(*this, *pos_args[i]);
      // The spec's own index is the whole operand: type, parameter name and
      // the receivers the check covers all come back out of the table.
      emit(Op::ChkParam, base + 2 + i, base, bmeth_spec_index(spec), i);
    }
    if (kw_val) {
      // Parameter order, not argument order: an undeclared callback ahead of
      // the keyword still owes its `Function` check first.
      if (spec.callback_param >= 0) {
        StampGuard cb_pos(*this, *pos_args[spec.callback_param]);
        emit(Op::CbType, base + 2 + spec.callback_param, base,
             kconst_str(spec.pnames[spec.callback_param]));
      }
      // A keyword-capable built-in reports its binder errors at the call
      // root, where the interp's own rich errors point — not at the value.
      emit(Op::ChkParam, base + 2 + argc, base, bmeth_spec_index(spec), argc);
    } else if (spec.kw) {
      // The keyword the call left out: its declared default is `false`, the
      // only one any built-in's keyword-only parameter carries.
      emit(Op::LoadConst, base + 2 + argc, kconst({TAG_BOOL, 0}));
    } else if (spec.nargs > argc) {
      // The omitted optional argument: the interp's declared default, so the
      // op's arity is fixed per built-in and needs no absent-argument arm.
      // The literal is materialised in the type the parameter it fills
      // declares — `truncate`'s ellipsis is a String, `split`'s limit a Long.
      emit(Op::LoadConst, base + 2 + argc,
           spec.params[argc] == BParam::Long
               ? kconst_long(std::strtoll(spec.def, nullptr, 10))
               : kconst_str(spec.def));
    }
    int32_t t = alloc_temp(at);
    size_t ix = emit(Op::BMeth, t, base, static_cast<int32_t>(spec.id),
                     spec.nargs);
    // A user method shadowing the built-in name takes the gate slot and is
    // called from here, so this site publishes argument positions like any
    // other call — its typed parameters report at the argument expression.
    std::vector<const peg::Ast*> asts;
    for (const auto* a : pos_args) asts.push_back(a);
    record_call_argpos(ix, args, std::move(asts));
    if (!kw_val) return {t, true};
    store_into(out, {t, true}, /*dst_is_fresh=*/true);
    patch_to_here(done_user);
    return {out, true};
  }

  // KWARG / KWARG_SPLAT argument nodes, under whichever of tag/original_tag
  // the AST optimizer left them.
  static bool is_kwarg(const peg::Ast& a) {
    using namespace peg::udl;
    return a.tag == "KWARG"_ || a.original_tag == "KWARG"_;
  }
  static bool is_kwarg_splat(const peg::Ast& a) {
    using namespace peg::udl;
    return a.tag == "KWARG_SPLAT"_ || a.original_tag == "KWARG_SPLAT"_;
  }

  // The call's positional arguments: a keyword binds by name, so it takes no
  // slot in the run the built-in reads.
  static std::vector<const peg::Ast*> positional_args(const peg::Ast& args) {
    std::vector<const peg::Ast*> out;
    for (const auto& a : args.nodes)
      if (!is_kwarg(*a) && !is_kwarg_splat(*a)) out.push_back(a.get());
    return out;
  }

  // The expression this call gives the keyword `name`, or null. First wins,
  // like the binder — a repeat is a structural error raised before any of it.
  static const peg::Ast* kwarg_value(const peg::Ast& args,
                                     std::string_view name) {
    for (const auto& a : args.nodes)
      if (is_kwarg(*a) && a->nodes[0]->token == name) return a->nodes[1].get();
    return nullptr;
  }

  // Whether this argument list carries keyword content at all.
  static bool has_kwargs(const peg::Ast& args) {
    for (const auto& a : args.nodes)
      if (is_kwarg(*a) || is_kwarg_splat(*a)) return true;
    return false;
  }

  // `f(a, k: v, **o)` / `o.m(k: v)` — the runtime resolver binds the names
  // against the callee's parameter metadata, so the whole list rides one op.
  // The three kinds keep their own stretch of one contiguous run (that is
  // how the resolver is handed them), while the VALUES evaluate in source
  // order, which is the order both backends show.
  // `pos0` is a value that rides as positional[0] with no argument expression
  // of its own — the UFCS receiver, whose parameter errors report at the
  // method name (`pos0_at`), the way the positional UFCS call reports them.
  ExprResult compile_kwargs_call(const peg::Ast& at, const peg::Ast& args,
                                 int32_t callee_slot, ExprResult* recv,
                                 ExprResult* pos0 = nullptr,
                                 const peg::Ast* pos0_at = nullptr) {
    using namespace peg::udl;
    // Structural errors — a positional after a keyword, a repeated keyword —
    // are the interp's to raise where it scans the list: before any argument
    // runs, and catchable.
    if (auto e = culebra::check_arg_list(args)) {
      emit_raise(e->kind, e->message, e->line, e->col);
    }
    struct Slotted {
      const peg::Ast* value;
      int32_t slot;
    };
    std::vector<Slotted> in_order;
    std::vector<std::string_view> keys;
    int32_t n_pos = 0, n_kw = 0, n_splat = 0;
    for (const auto& a : args.nodes) {
      if (is_kwarg_splat(*a))
        n_splat++;
      else if (is_kwarg(*a))
        n_kw++;
      else
        n_pos++;
    }
    int32_t off = recv ? 1 : 0;
    int32_t lead = pos0 ? 1 : 0;  // the UFCS receiver, positional[0]
    n_pos += lead;
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    if (recv) alloc_temp(at);
    for (int32_t i = 0; i < n_pos + n_kw + n_splat; i++) alloc_temp(at);
    if (recv) store_into(base, *recv, /*dst_is_fresh=*/true);
    if (pos0) store_into(base + off, *pos0, /*dst_is_fresh=*/true);
    int32_t pi = lead, ki = 0, si = 0;
    for (const auto& a : args.nodes) {
      if (is_kwarg_splat(*a)) {
        in_order.push_back({a->nodes[0].get(),
                            base + off + n_pos + n_kw + si++});
      } else if (is_kwarg(*a)) {
        keys.push_back(a->nodes[0]->token);
        in_order.push_back({a->nodes[1].get(), base + off + n_pos + ki++});
      } else {
        in_order.push_back({a.get(), base + off + pi++});
      }
    }
    for (const auto& s : in_order)
      store_into(s.slot, compile_expr(*s.value), /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(at);
    auto spec = static_cast<int32_t>(chunk_.kwcalls.size());
    Chunk::KwCall kc{recv != nullptr, n_pos, n_kw, n_splat, {}};
    for (auto k : keys)
      kc.kw_keys.push_back(
          reinterpret_cast<const char*>(chunk_.consts[kconst_str(k)].data));
    chunk_.kwcalls.push_back(std::move(kc));
    // The binder's own errors report at the CALLEE, which a parenthesized
    // one puts past the `(` the call node starts at — the anchor the arity
    // check already reads out of the same helper. A UFCS call is the
    // exception: interp's eval_ufcs_call sets its site at the ARGUMENTS
    // node, which is the stamp this already carries.
    if (!pos0) {
      auto [cl, cc] = culebra::call_callee_position(at);
      stamp_at(cl, cc);
    }
    size_t ix = emit(Op::CallKw, t, callee_slot, base, spec);
    // Only the positionals: they bind to the parameters of the same index,
    // which is what an argument-position table indexes by. A keyword value
    // reports at the call site on every backend, so leaving its parameter
    // without an entry is the agreement, not a gap.
    std::vector<const peg::Ast*> asts;
    if (pos0_at) asts.push_back(pos0_at);
    for (const auto& a : args.nodes)
      if (!is_kwarg(*a) && !is_kwarg_splat(*a)) asts.push_back(a.get());
    record_call_argpos(ix, args, std::move(asts));
    return {t, true};
  }

  // Throw `kind`/`message` at the given source position when control
  // reaches here — the shape an error the compiler can see but the language
  // raises at run time takes (the interp's own timing, and catchable).
  void emit_raise(std::string_view kind, const std::string& message,
                  size_t line, size_t col) {
    stamp_at(line, col);
    emit(Op::RaiseErr, 0, kconst_str(kind), kconst_str(message));
  }

  // One argument-list postfix: positional args in a contiguous run of
  // owned temps (the JitFn ABI's arg slab), one Call op. Evaluation order
  // is callee first, then args left to right — both backends' order.
  ExprResult compile_call_step(const peg::Ast& ast, const peg::Ast& args,
                               ExprResult callee, StaticCallee target = {},
                               bool callee_in_cell = false) {
    if (has_kwargs(args))
      return compile_kwargs_call(ast, args, callee.slot, nullptr);
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
    for (int32_t i = 0; i < argc; i++)
      store_into(base + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(ast);
    size_t ix = emit(Op::Call, t, callee.slot, base, argc);
    record_call_target(ix, target, argc, callee_in_cell);
    std::vector<const peg::Ast*> asts;
    for (const auto& a : args.nodes) asts.push_back(a.get());
    record_call_argpos(ix, args, std::move(asts));
    return {t, true};
  }

  // `_lazy_ns_register("Name", fn () { … })` — the intrinsic the stdlib
  // splice emits at top level, one per lazy source module. It is not a call
  // the language has: the name resolves to nothing, and the builder must be
  // recorded rather than invoked. arg0 is always a string literal, so the
  // name is a constant here as it is a rodata pointer in the JIT's arm.
  std::optional<ExprResult> compile_lazy_ns_register(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes[0]->tag != "IDENTIFIER"_ ||
        ast.nodes[0]->token != "_lazy_ns_register" || ast.nodes.size() < 2 ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_)
      return std::nullopt;
    const auto& args = *ast.nodes[1];
    if (args.nodes.size() != 2) return std::nullopt;
    const peg::Ast* nameNode = args.nodes[0].get();
    while (nameNode->nodes.size() == 1) nameNode = nameNode->nodes[0].get();
    StampGuard pos(*this, ast);
    auto builder = compile_expr(*args.nodes[1]);
    emit(Op::LazyNsReg, 0, builder.slot,
         kconst_str(std::string(nameNode->token)));
    int32_t t = alloc_temp(ast);
    emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
    return ExprResult{t, true};
  }

  // A direct `fn(...)` in a receiver frame: the frame's own closure called
  // with the frame's own receiver. CallM's run already has that shape — the
  // receiver at its head — so this is compile_property_call's tail with the
  // callee taken from the `fn` slot instead of a property read.
  ExprResult compile_self_call_step(const peg::Ast& at, const peg::Ast& args,
                                    ExprResult callee,
                                    StaticCallee target = {}) {
    ExprResult recv{chunk_.self_slot, false};
    if (has_kwargs(args))
      return compile_kwargs_call(at, args, callee.slot, &recv);
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    alloc_temp(at);             // [0] = the receiver
    for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
    store_into(base, recv, /*dst_is_fresh=*/true);
    for (int32_t i = 0; i < argc; i++)
      store_into(base + 1 + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(at);
    size_t ix = emit(Op::CallM, t, callee.slot, base, argc);
    record_call_target(ix, target, argc);
    std::vector<const peg::Ast*> asts;
    for (const auto& a : args.nodes) asts.push_back(a.get());
    record_call_argpos(ix, args, std::move(asts));
    return {t, true};
  }

  // One `[k]` postfix applied to `recv`, as an rvalue read (the write-context
  // IndexWr/IndexCo reads, emitted inline by compile_assign_index, stay
  // point-only like the JIT's compound dispatch: a range key falls into their
  // Long-key check). A range key — literal (`xs[1..3]`) or stored — rides the
  // same Index op: its runtime is-range dispatch slices before the receiver
  // arms (emit_index_step's order). The op is stamped at the enclosing node
  // `at`, where both backends anchor every index error.
  ExprResult compile_index_read(const peg::Ast& at, const peg::Ast& post,
                                ExprResult recv) {
    auto key = compile_expr(post);
    StampGuard pos(*this, at);
    int32_t t = alloc_temp(at);
    emit(Op::Index, t, recv.slot, key.slot);
    return {t, true};
  }

  // `a..b` / `a..=b` (optionally `by step`) and the bare `..`: a Range
  // object over a contiguous start/end/step slot run. Each present
  // endpoint is ChkLong'd right after it compiles — `0.5..t()` throws
  // before t() runs, the JIT's value_to_long-after-each-compile order —
  // with every check and the RangeNew stamped at the range node, both
  // backends' anchor for endpoint type errors.
  ExprResult compile_range(const peg::Ast& ast) {
    using namespace peg::udl;
    culebra::RangeLayout lay{};
    if (ast.tag != "RANGE_OPERATOR"_) lay = culebra::decode_range_layout(ast);
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    for (int i = 0; i < 3; i++) alloc_temp(ast);
    if (lay.start) {
      store_into(base + 0, compile_expr(*lay.start), /*dst_is_fresh=*/true);
      emit(Op::ChkLong, base + 0);
    }
    if (lay.end) {
      store_into(base + 1, compile_expr(*lay.end), /*dst_is_fresh=*/true);
      emit(Op::ChkLong, base + 1);
    }
    if (lay.step) {
      store_into(base + 2, compile_expr(*lay.step), /*dst_is_fresh=*/true);
      emit(Op::ChkLong, base + 2);
    } else {
      emit(Op::LoadConst, base + 2, kconst_long(1));
    }
    int32_t t = alloc_temp(ast);
    emit(Op::RangeNew, t, base,
         (lay.start ? 1 : 0) | (lay.end ? 2 : 0) | (lay.inclusive ? 4 : 0));
    return {t, true};
  }

  // `if` / ternary in any position: a result temp starts nil, the taken
  // arm's block writes it exactly once, every arm jumps to the common end.
  // No else-arm leaves it nil (interp parity).
  ExprResult compile_if(const peg::Ast& ast) {
    auto iv = culebra::view_if(ast);
    // The result outlives the init scope — it is the whole construct's value
    // — so its slot is taken before that scope opens.
    int32_t res = alloc_temp(ast);
    InitScope init(*this, ast, iv.init);
    // An arm body declares into the scope around the `if`. Whether it ran is
    // a run-time fact, so the names take the pre-declaration a forward
    // reference takes: one lazy cell per name for the whole `if` — shared by
    // the arms, so `if c { let a = 1 } else { let a = 2 }` reads whichever arm
    // ran — shadowing what the name meant before, and still holding the
    // sentinel (hence NameError) when no arm declared it. An init clause opens
    // a scope of its own, so nothing there escapes and collect_escaping_decls
    // declines to look.
    DeclList escaping;
    collect_escaping_decls(ast, escaping);
    predeclare_conditional_cells(ast, escaping);
    auto compile_arm = [&](const peg::Ast& body) { compile_arm_into(body, res); };
    std::vector<size_t> end_jumps;
    size_t i = iv.arm_off;
    for (; i + 1 < ast.nodes.size(); i += 2) {
      auto cond = compile_expr(*ast.nodes[i]);
      size_t skip = emit(Op::JumpIfFalse, cond.slot);
      compile_arm(*ast.nodes[i + 1]);
      end_jumps.push_back(emit(Op::Jump));
      patch_to_here(skip);
    }
    if (i < ast.nodes.size()) {  // trailing else block
      compile_arm(*ast.nodes[i]);
    }
    for (size_t j : end_jumps) patch_to_here(j);
    return {res, true};
  }

  // `cond { test => body, ..., _ => default }` — the subjectless conditional,
  // compile_if's shape with a test per arm. A `_` WILDCARD test is the
  // unconditional default, so the arms after it are dead and never compiled;
  // with none reached the result stays nil. Arm bodies declare into the scope
  // around the `cond`, as an `if` arm's do.
  ExprResult compile_cond(const peg::Ast& ast) {
    using namespace peg::udl;
    int32_t res = alloc_temp(ast);
    DeclList escaping;
    collect_escaping_decls(ast, escaping);
    predeclare_conditional_cells(ast, escaping);
    std::vector<size_t> end_jumps;
    for (const auto& arm : ast.nodes) {  // each COND_ARM: [test, body]
      const auto& test = *arm->nodes[0];
      if (test.tag == "WILDCARD"_) {
        compile_arm_into(*arm->nodes[1], res);
        break;
      }
      auto c = compile_expr(test);
      size_t skip = emit(Op::JumpIfFalse, c.slot);
      compile_arm_into(*arm->nodes[1], res);
      end_jumps.push_back(emit(Op::Jump));
      patch_to_here(skip);
    }
    for (size_t j : end_jumps) patch_to_here(j);
    return {res, true};
  }

  // `&&` / `||` / `??`: the taken operand's value (not a coercion) is the
  // result, JIT/interp-style. Each operand lands in the result slot, then
  // the short-circuit test reads it back — to_bool enforces strict Bool
  // truthiness for `&&` / `||`; `??` only tests the nil tag.
  ExprResult compile_short_circuit(const peg::Ast& ast, Op jump_op) {
    int32_t res = alloc_temp(ast);
    std::vector<size_t> end_jumps;
    for (size_t i = 0; i < ast.nodes.size(); i++) {
      store_into(res, compile_expr(*ast.nodes[i]), /*dst_is_fresh=*/i == 0);
      if (i + 1 < ast.nodes.size())
        end_jumps.push_back(emit(jump_op, res));
    }
    for (size_t j : end_jumps) patch_to_here(j);
    return {res, true};
  }

  // A single comparison, or the chain `a < b < c` = `(a<b) && (b<c)` with
  // each middle operand evaluated once (Python semantics, mirroring
  // compile_condition). The chain's Bool lands in `res`; a failing link
  // leaves its false there and short-circuits.
  ExprResult compile_condition(const peg::Ast& ast) {
    auto cmp_op = [&](const peg::Ast& op_node) -> Op {
      auto t = op_node.token;
      if (t == "==") return Op::Eq;
      if (t == "!=") return Op::Ne;
      if (t == "<") return Op::Lt;
      if (t == "<=") return Op::Le;
      if (t == ">") return Op::Gt;
      if (t == ">=") return Op::Ge;
      reject(op_node, culebra::format("operator '{}'", t));
    };
    auto lhs = compile_expr(*ast.nodes[0]);
    if (ast.nodes.size() == 3) {
      auto rhs = compile_expr(*ast.nodes[2]);
      int32_t t = alloc_temp(ast);
      emit(cmp_op(*ast.nodes[1]), t, lhs.slot, rhs.slot);
      return {t, true};
    }
    int32_t res = alloc_temp(ast);
    std::vector<size_t> false_jumps;
    for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
      auto rhs = compile_expr(*ast.nodes[i + 1]);
      int32_t t = alloc_temp(ast);
      emit(cmp_op(*ast.nodes[i]), t, lhs.slot, rhs.slot);
      store_into(res, {t, true}, /*dst_is_fresh=*/i == 1);
      if (i + 3 < ast.nodes.size())
        false_jumps.push_back(emit(Op::JumpIfFalse, res));
      lhs = rhs;
    }
    for (size_t j : false_jumps) patch_to_here(j);
    return {res, true};
  }

  // `try BODY catch name HANDLER` as an expression. The body's scope is an
  // ordinary Cleanup entry that also carries a handler: the unwind walk has
  // already run the nested scopes' steps and this scope's own (its defers,
  // then its slots) by the time the handler binds the caught value (mutable,
  // the interp's catch-binding default) and runs into the same result slot.
  // Normal-path exits (fall-through, break/continue/return crossing the
  // region) release through the regular scope machinery.
  ExprResult compile_try(const peg::Ast& ast) {
    using namespace peg::udl;
    const auto& id = *ast.nodes[1];
    int32_t res = alloc_temp(ast);
    int32_t caught = alloc_temp(ast);
    // Region defer mark: taken before the region opens whenever any defer
    // can be pending inside it — the body's own scope-level defers share it
    // (the stack height is identical at region entry and body-scope entry),
    // and defers of nested scopes sit above it, so the handler's single run
    // covers a throw from any depth. The slot lives below `wm`, out of the
    // release ladder's range.
    int32_t rmark = -1;
    if (analysis_.try_region_has_defer.contains(&ast)) {
      rmark = alloc_slot(ast, "(try.mark)");
      emit(Op::DeferMark, rmark);
    }
    bool body_scope_defer =
        analysis_.scope_has_defer.contains(ast.nodes[0].get());
    // The body inline (compile_block_into minus its own DeferScope): the
    // catching part of the body's scope must END before its fall-through
    // defer run, because a defer throwing at the try body's NORMAL exit
    // escapes this catch (interp runs run_deferred(tryEnv) outside its
    // try/catch pair), while one throwing at a NESTED scope's exit — still
    // inside the region — is caught here, exactly as the interp's nesting has
    // it. The tail keeps a cleanup entry of its own so the body's bindings
    // still release as that escaping throw passes them.
    if (body_scope_defer) defer_scopes_.push_back(rmark);
    pending_scope_mark_ = rmark;
    push_scope(ast);
    predeclare_forward_refs(*ast.nodes[0]);
    compile_body_into(*ast.nodes[0], res);
    auto end = static_cast<uint32_t>(chunk_.code.size());
    if (body_scope_defer) {
      emit(Op::DeferRunTo, rmark);
      defer_scopes_.pop_back();
    }
    auto region = pop_scope();
    size_t end_jump = emit(Op::Jump);
    auto handler = static_cast<uint32_t>(chunk_.code.size());
    for (size_t ix : region) {
      auto& cu = chunk_.cleanups[ix];
      if (cu.start >= end) continue;  // wholly in the fall-through tail
      auto tail_end = cu.end;
      cu.end = std::min(cu.end, end);
      cu.handler = handler;
      cu.caught_slot = caught;
      // The tail releases the same bindings but catches nothing.
      if (tail_end > end)
        chunk_.cleanups.push_back({end, tail_end, /*parent=*/-1,
                                   cu.defer_mark_slot, cu.slot_lo, cu.slot_hi,
                                   cu.cells_before});
    }
    push_scope(ast);
    auto name = std::string(id.token);
    if (is_sink_name(name)) {
      emit(Op::Release, caught);  // `catch _`: drop the payload's +1
    } else {
      bool cell = info_->captured_locals.contains(name);
      int32_t e = cell ? alloc_cell_slot(id, name) : alloc_slot(id, name);
      if (cell) {
        emit(Op::CellNew, e, caught);
      } else {
        emit(Op::Take, e, caught);
      }
      push_binding({name, e, /*is_mut=*/true, cell});
    }
    // The catch body is its own defer scope (scan_eh_defer keys the node);
    // handler code sits outside the region, so its defers behave like any
    // scope's — a throwing one propagates outward, past this try.
    compile_block_into(*ast.nodes[2], res, /*defer_key=*/ast.nodes[2].get());
    pop_scope();
    patch_to_here(end_jump);
    return {res, true};
  }

  // `match` as an expression. The subject is owned by a statement temp across
  // the arms (the JIT holds it in a dedicated subject scope; here the
  // statement sweep / the break-return temp releases are the single
  // releaser). Every arm runs test → bind → guard → body, the two-phase walk
  // compile_destructure_assign shares: no pattern binds before all of its
  // tests pass, so a failed test jumps to the next arm with nothing live, and
  // only a guard failure has bindings to release. The body block writes the
  // shared result slot exactly once (compile_if's shape); no arm matched → nil.
  ExprResult compile_match(const peg::Ast& ast) {
    using namespace peg::udl;
    auto mv = culebra::view_match(ast);
    int32_t res = alloc_temp(ast);  // the construct's value: outside the scope
    InitScope init(*this, ast, mv.init);
    int32_t subj = alloc_temp(ast);
    store_into(subj, compile_expr(*mv.subject), /*dst_is_fresh=*/true);
    std::vector<size_t> end_jumps;
    for (const auto& arm : mv.arms->nodes) {
      // arm->nodes: PATTERN (GUARD)? body
      const auto& pat = *arm->nodes[0];
      push_scope(ast);
      std::vector<size_t> fail_jumps;  // patched to the next arm's start
      compile_pattern_test(pat, subj, fail_jumps);
      // Bind once every test passed. Arm bindings are mutable (interp's
      // try_pattern default) and declare like a `let`, so a captured one
      // lives in a cell (the compile_try catch-binding shape). The subject is
      // borrowed — it belongs to the statement temp, and a leaf retains its
      // own reference. An ObjGet in this walk cannot miss (the tests proved
      // every key present), so its edge joins the mismatch edge unused.
      compile_pattern_bind(pat, subj, /*subj_owned=*/false, fail_jumps,
                           /*is_mut=*/true, /*declares=*/true);
      size_t body_idx = 1;
      if (arm->nodes[body_idx]->tag == "GUARD"_) {
        auto g = compile_expr(*arm->nodes[body_idx]->nodes[0]);
        // Branch on the guard being TAKEN, so the failing path is the fall
        // through and its release ladder is emitted here — with the arm's
        // scope still open, which is what lets the whole pattern's bindings
        // (not just one) be released by the ordinary scope ladder. The test
        // reads the pending MATCH position: a non-Bool guard's TypeError
        // reports the match node in both existing lanes.
        size_t take = emit(Op::JumpIfTrue, g.slot);
        release_down_to(scopes_.back().slot_watermark);
        fail_jumps.push_back(emit(Op::Jump));
        patch_to_here(take);
        body_idx++;
      }
      // The arm body is its own defer scope (scan_eh_defer's MATCH case
      // keys the body node): defers fire when the arm's braces close, the
      // arm value already owned in `res`.
      compile_block_into(*arm->nodes[body_idx], res,
                         /*defer_key=*/arm->nodes[body_idx].get());
      int32_t arm_top = next_slot_;
      pop_scope();  // the taken path's binding release
      // Arms are alternative paths but ONE statement, so they must not share
      // slot indices: the scope rollback would hand the next arm a slot this
      // arm still has a temp entry for (its `+1` dropped by the second write,
      // and the statement sweep releasing it twice), and could re-declare a
      // plain slot as a captured binding's cell — the kind a release ladder
      // picks statically per slot. Keeping the water mark costs slots in
      // proportion to the whole match, which kMaxSlots bounds.
      next_slot_ = arm_top;
      end_jumps.push_back(emit(Op::Jump));
      for (size_t ix : fail_jumps) patch_to_here(ix);
    }
    for (size_t ix : end_jumps) patch_to_here(ix);
    return {res, true};
  }

  // `let [a, b] = e` / `let {x} = e` / `[a, b] = e`. The pattern is matched
  // before a single target is written — the two-phase walk the interp and
  // the JIT share, which is what makes a mismatch leave the targets alone
  // and every fail edge release-free (nothing but statement temps is live).
  // Evaluates to the right-hand value, like the other assignment forms.
  ExprResult compile_destructure_assign(const peg::Ast& ast) {
    bool declares =
        ast.nodes[0]->token == "let" || ast.nodes[1]->token == "mut";
    bool is_mut = ast.nodes[1]->token == "mut";
    const auto& pat = *ast.nodes[2];
    // Both of this statement's throws — the mismatch and a leaf's
    // ImmutableError — report the destructure node, so the walks run under
    // its stamp (the RHS's own guards restore it).
    StampGuard pos(*this, ast);
    // The value lands in a temp before the pattern names exist, so
    // `let [x] = x` reads the outer `x` (compile_assignment's rule).
    int32_t rv = alloc_temp(ast);
    store_into(rv, compile_expr(*ast.nodes[3]), /*dst_is_fresh=*/true);
    std::vector<size_t> fail;
    compile_pattern_test(pat, rv, fail);
    // The subject is the statement's value too, so the walk borrows it; the
    // element temps it reads are handed over instead.
    compile_pattern_bind(pat, rv, /*subj_owned=*/false, fail, is_mut, declares);
    size_t done = emit(Op::Jump);
    for (size_t ix : fail) patch_to_here(ix);
    emit(Op::DestrErr);
    patch_to_here(done);
    return {rv, false};
  }

  // Writes the pattern's bindings, the tests already passed. Element reads
  // repeat (an Array read is pure, and nothing between the walks can change
  // the subject); the `fail` edges an ObjGet needs are the mismatch's, dead
  // on this walk. A leaf consumes its subject register's `+1` when the
  // caller offers it (`subj_owned`, true for the element temps read here);
  // a container ignores the offer, since its children read the temp more
  // than once and the statement sweep is the single releaser.
  void compile_pattern_bind(const peg::Ast& pat, int32_t subj, bool subj_owned,
                            std::vector<size_t>& fail, bool is_mut,
                            bool declares) {
    using namespace peg::udl;
    switch (pat.tag) {
      case "IDENTIFIER"_:
        bind_pattern_name(pat, pat, subj, subj_owned, is_mut, declares);
        return;
      case "TYPED_IDENT"_:
        bind_pattern_name(pat, *pat.nodes[0], subj, subj_owned, is_mut,
                          declares);
        return;
      case "ARRAY_PATTERN"_:
      case "FOR_BINDING"_:
      case "TUPLE_PATTERN"_: {
        bool too_many = false;
        int rest = rest_index(pat, pat.tag == "ARRAY_PATTERN"_, too_many);
        const auto& elems = pat.nodes;
        for (size_t i = 0; i < elems.size(); i++) {
          if (!culebra::find_pattern_binding(*elems[i])) continue;
          int32_t t = alloc_temp(pat);
          if (static_cast<int>(i) == rest) {
            emit(Op::SeqRest, t, subj, static_cast<int32_t>(i),
                 static_cast<int32_t>(elems.size() - 1));
            bind_pattern_name(*elems[i], *elems[i]->nodes[0], t,
                              /*src_owned=*/true, is_mut, declares);
            continue;
          }
          int32_t at = rest >= 0 && static_cast<int>(i) > rest
                           ? static_cast<int32_t>(i) -
                                 static_cast<int32_t>(elems.size())
                           : static_cast<int32_t>(i);
          emit(Op::SeqGet, t, subj, at);
          compile_pattern_bind(*elems[i], t, /*subj_owned=*/true, fail, is_mut,
                               declares);
        }
        return;
      }
      case "CTOR_PATTERN"_: {
        // The test walk already proved the variant and every payload field,
        // so this walk only reads what a sub-pattern binds.
        for (size_t i = 1; i < pat.nodes.size(); i++) {
          if (!culebra::find_pattern_binding(*pat.nodes[i])) continue;
          int32_t t = alloc_temp(*pat.nodes[i]);
          fail.push_back(
              emit(Op::ObjGet, t, 0, subj,
                   kconst_str(culebra::positional_field_name(i - 1))));
          compile_pattern_bind(*pat.nodes[i], t, /*subj_owned=*/true, fail,
                               is_mut, declares);
        }
        return;
      }
      case "OBJECT_PATTERN"_: {
        for (const auto& entry : pat.nodes) {
          bool full = entry->tag == "OBJECT_PAT_ENTRY"_;
          const peg::Ast* sub = full ? entry->nodes[1].get() : nullptr;
          if (sub ? !culebra::find_pattern_binding(*sub)
                  : culebra::is_sink_name(entry->token))
            continue;
          auto key = full ? entry->nodes[0]->token : entry->token;
          int32_t t = alloc_temp(*entry);
          fail.push_back(emit(Op::ObjGet, t, 0, subj, kconst_str(key)));
          if (sub)
            compile_pattern_bind(*sub, t, /*subj_owned=*/true, fail, is_mut,
                                 declares);
          else
            bind_pattern_name(*entry, *entry, t, /*src_owned=*/true, is_mut,
                              declares);
        }
        return;
      }
      default:
        return;  // literals and `_` bind nothing
    }
  }

  // One pattern leaf's write. A declaring form (`let` / `mut`) binds a new
  // name exactly as `let x = v` does, cell included when the name is
  // captured; a `let`-less one reassigns the visible binding with `x = v`'s
  // rules, mutability check and all. An owned `src` hands its `+1` to the
  // store (`_` drops it back to the sweep, as does the unreachable code
  // after an ImmutErr).
  void bind_pattern_name(const peg::Ast& at, const peg::Ast& ident,
                         int32_t src, bool src_owned, bool is_mut,
                         bool declares) {
    auto name = std::string(ident.token);
    if (culebra::is_sink_name(name)) return;
    ExprResult v{src, src_owned};
    if (declares) {
      // A leaf whose name was pre-declared here fills that cell instead of
      // minting a second one — compile_assign_var's rule, which a collapsed
      // `if` arm's declaring destructure now reaches too.
      if (Binding* pre = predeclared_here(name)) {
        store_cell(at, pre->slot, v);
        slot_rank_[pre->slot] = next_rank_++;
        emit_session_decl_bind(*pre, is_mut);
        pre->is_mut = is_mut;
        settle_predeclared(*pre);
        return;
      }
      // A REPL line's top-level leaf binds in the session, like `let x = v`.
      if (repl_top()) {
        Binding& sb = bind_session(ident, name);
        store_cell(at, sb.slot, v);
        emit_session_decl_bind(sb, is_mut);
        sb.is_mut = is_mut;
        sb.lazy = false;
        sb.shadowed_builtin = false;
        return;
      }
      bool cell = info_->captured_locals.contains(name);
      int32_t slot =
          cell ? alloc_cell_slot(ident, name) : alloc_slot(ident, name);
      if (cell) {
        store_new_cell(at, slot, v);
      } else {
        store_into(slot, v, /*dst_is_fresh=*/true);
      }
      push_binding({name, slot, is_mut, cell});
      return;
    }
    const Binding* b = lookup_or_session(ident, name);
    // A leaf naming nothing visible declares it, exactly as bare `x = v`
    // does — immutably. (`self` is not special here: the interp's pattern
    // walk binds it like any other leaf, probed on both backends.)
    if (!b && !is_stdlib_namespace(name) && !is_stdlib_global(name)) {
      bind_pattern_name(at, ident, src, src_owned, /*is_mut=*/false,
                        /*declares=*/true);
      return;
    }
    if (!b) {  // a stdlib global: an existing immutable binding, so it refuses
      emit(Op::ImmutErr, kconst_str(name));
      return;
    }
    emit_rebind(at, *b, v);  // at the statement's stamp
  }

  // Emits the tests for one leaf pattern: fall through on match, jump (via
  // `fail`) on mismatch. Bindings are NOT emitted here — the caller binds
  // after the whole pattern passed, which is what makes the fail edges
  // release-free.
  // The pattern testers' shared tag gate: fall through when the subject's
  // tag is in `tags`, otherwise jump via `fail`.
  void emit_tag_gate(int32_t subj, std::initializer_list<int8_t> tags,
                     std::vector<size_t>& fail) {
    std::vector<size_t> ok;
    for (int8_t t : tags) ok.push_back(emit(Op::JumpIfTag, subj, 0, t));
    fail.push_back(emit(Op::Jump));
    for (size_t ix : ok) patch_to_here(ix);
  }

  void compile_pattern_test(const peg::Ast& pat, int32_t subj,
                            std::vector<size_t>& fail) {
    using namespace peg::udl;
    // A PATTERN node with children is an or-pattern: alternatives tried in
    // order, first match wins. An alternative never binds — parse rejects
    // that shape (`reject_or_pattern_binding`), which is what lets the tests
    // below stand alone from the binding pass.
    if (pat.tag == "PATTERN"_ && !pat.nodes.empty()) {
      std::vector<size_t> ok_jumps;
      for (size_t i = 0; i < pat.nodes.size(); i++) {
        std::vector<size_t> alt_fail;
        compile_pattern_test(*pat.nodes[i], subj, alt_fail);
        if (i + 1 < pat.nodes.size()) {
          ok_jumps.push_back(emit(Op::Jump));
          for (size_t ix : alt_fail) patch_to_here(ix);
        } else {
          fail.insert(fail.end(), alt_fail.begin(), alt_fail.end());
        }
      }
      for (size_t ix : ok_jumps) patch_to_here(ix);
      return;
    }
    // Tag gate: fall through when the subject's tag is in `tags`.
    auto tag_gate = [&](std::initializer_list<int8_t> tags) {
      emit_tag_gate(subj, tags, fail);
    };
    // Value check after the gate: Eq against the literal constant. The
    // gate makes Eq's dispatch exact (a Float subject never numerically
    // equals a Long pattern — interp's try_pattern checks type first).
    auto lit_eq = [&](int32_t kidx) {
      int32_t k = alloc_temp(pat);
      emit(Op::LoadConst, k, kidx);
      int32_t t = alloc_temp(pat);
      emit(Op::Eq, t, subj, k);
      fail.push_back(emit(Op::JumpIfFalse, t));
    };
    switch (pat.tag) {
      case "WILDCARD"_:
      case "IDENTIFIER"_:
        return;  // always matches; IDENTIFIER binds in the caller
      case "TYPED_IDENT"_:
        compile_type_gate(*pat.nodes[1], subj, fail);
        return;
      case "NIL"_:
        tag_gate({TAG_NIL});
        return;
      case "BOOLEAN"_:
        tag_gate({TAG_BOOL});
        lit_eq(kconst({TAG_BOOL, pat.token == "true"}));
        return;
      case "NUMBER"_:
        tag_gate({TAG_LONG});
        lit_eq(kconst_long(culebra::parse_integer_literal(pat.token)));
        return;
      case "FLOAT"_: {
        tag_gate({TAG_FLOAT});
        lit_eq(kconst({TAG_FLOAT, _culebra_double_to_bits(
                                      pat.token_to_number<double>())}));
        return;
      }
      case "STRING"_:
      case "INTERPOLATED_CONTENT"_:
      case "INTERPOLATED_STRING"_: {
        // A string literal pattern matches String and StringView subjects
        // by content (try_pattern's to_string_view comparison; Eq on two
        // strlikes is that comparison). Only constant interpolated forms
        // are valid — an embedded expression makes the pattern a
        // never-match, mirroring interp/JIT's pre-execution-lint defense.
        std::string s;
        if (pat.tag == "STRING"_) {
          s = std::string(pat.token);
        } else if (pat.tag == "INTERPOLATED_CONTENT"_) {
          s = culebra::decode_interpolated_content(pat.token);
        } else {
          for (const auto& child : pat.nodes) {
            if (child->tag != "INTERPOLATED_CONTENT"_) {
              fail.push_back(emit(Op::Jump));
              return;
            }
            s += culebra::decode_interpolated_content(child->token);
          }
        }
        tag_gate({TAG_STRING, TAG_STRINGVIEW});
        lit_eq(kconst_str(s));
        return;
      }
      case "ARRAY_PATTERN"_:
        compile_seq_pattern_test(pat, subj, fail, /*allow_rest=*/true);
        return;
      case "FOR_BINDING"_:  // multi-target `for k, v in …`: a tuple's shape
      case "TUPLE_PATTERN"_:
        compile_seq_pattern_test(pat, subj, fail, /*allow_rest=*/false);
        return;
      case "OBJECT_PATTERN"_:
        compile_obj_pattern_test(pat, subj, fail);
        return;
      case "CTOR_PATTERN"_:
        compile_ctor_pattern_test(pat, subj, fail);
        return;
      default:
        reject(pat, culebra::format("pattern '{}'", pat.name));
    }
  }

  // Does matching this pattern test anything, or does it accept every value?
  // A pattern that only binds needs no element read in the testing walk.
  static bool pattern_has_test(const peg::Ast& pat) {
    using namespace peg::udl;
    return !(pat.tag == "WILDCARD"_ || pat.tag == "IDENTIFIER"_ ||
             pat.tag == "REST_PATTERN"_);
  }

  // The `...rest` element's position in an array pattern, or -1. A second
  // rest makes the pattern a never-match in the other backends (interp bails
  // out of ARRAY_PATTERN, the JIT leaves the extra REST_PATTERN to its
  // default-false arm), which `too_many` reports back.
  static int rest_index(const peg::Ast& pat, bool allow_rest,
                        bool& too_many) {
    using namespace peg::udl;
    int found = -1;
    too_many = false;
    if (!allow_rest) return -1;  // a tuple pattern's grammar has no rest
    for (size_t i = 0; i < pat.nodes.size(); i++) {
      if (pat.nodes[i]->tag != "REST_PATTERN"_) continue;
      if (found >= 0) {
        too_many = true;
        return found;
      }
      found = static_cast<int>(i);
    }
    return found;
  }

  // `[p, q]` / `(p, q)` / `[h, ...t, z]`: shape and length, then the element
  // sub-patterns that test something. Post-rest elements index from the end,
  // so the tail's length never has to reach a register.
  void compile_seq_pattern_test(const peg::Ast& pat, int32_t subj,
                                std::vector<size_t>& fail, bool allow_rest) {
    bool too_many = false;
    int rest = rest_index(pat, allow_rest, too_many);
    if (too_many) {
      fail.push_back(emit(Op::Jump));  // at most one rest: never matches
      return;
    }
    const auto& elems = pat.nodes;
    auto fixed = static_cast<int32_t>(elems.size() - (rest >= 0 ? 1 : 0));
    fail.push_back(emit(Op::SeqChk, subj, 0, fixed, rest >= 0 ? 1 : 0));
    for (size_t i = 0; i < elems.size(); i++) {
      if (static_cast<int>(i) == rest || !pattern_has_test(*elems[i])) continue;
      int32_t at = rest >= 0 && static_cast<int>(i) > rest
                       ? static_cast<int32_t>(i) - static_cast<int32_t>(
                                                       elems.size())
                       : static_cast<int32_t>(i);
      int32_t t = alloc_temp(pat);
      emit(Op::SeqGet, t, subj, at);
      compile_pattern_test(*elems[i], t, fail);
    }
  }

  // `{k}` / `{k: p}`: an Object, then every named key present (extra keys are
  // ignored). The tag gate stands on its own so `{}` still rejects a
  // non-Object, which no entry would be left to catch.
  // `Ok(x)` / `Result.Ok(x)`: an enum variant by its name (the part after
  // any `.`), then its positional payload fields against the sub-patterns.
  // The TAG_OBJECT gate comes first because type_matches answers true for a
  // primitive whose name the variant collides with (`Long(x)`), and the
  // field reads below would take that scalar for an object.
  void compile_ctor_pattern_test(const peg::Ast& pat, int32_t subj,
                                 std::vector<size_t>& fail) {
    auto path = pat.nodes[0]->token;
    auto dot = path.rfind('.');
    auto variant =
        dot == std::string_view::npos ? path : path.substr(dot + 1);
    emit_tag_gate(subj, {TAG_OBJECT}, fail);
    fail.push_back(emit(Op::TypeMatch, subj, 0, kconst_str(variant)));
    for (size_t i = 1; i < pat.nodes.size(); i++) {
      int32_t t = alloc_temp(*pat.nodes[i]);
      fail.push_back(emit(Op::ObjGet, t, 0, subj,
                          kconst_str(culebra::positional_field_name(i - 1))));
      if (pattern_has_test(*pat.nodes[i]))
        compile_pattern_test(*pat.nodes[i], t, fail);
    }
  }

  void compile_obj_pattern_test(const peg::Ast& pat, int32_t subj,
                                std::vector<size_t>& fail) {
    using namespace peg::udl;
    emit_tag_gate(subj, {TAG_OBJECT}, fail);
    for (const auto& entry : pat.nodes) {
      bool full = entry->tag == "OBJECT_PAT_ENTRY"_;
      const peg::Ast* sub = full ? entry->nodes[1].get() : nullptr;
      auto key = full ? entry->nodes[0]->token : entry->token;
      int32_t t = alloc_temp(*entry);
      fail.push_back(emit(Op::ObjGet, t, 0, subj, kconst_str(key)));
      if (sub && pattern_has_test(*sub)) compile_pattern_test(*sub, t, fail);
    }
  }

  // One alternative of an annotation, as a tag compare that jumps (via
  // `skip`) when it matches. Mirrors _culebra_type_matches_single's own
  // structure in order: a callable or an all-of bound is not a single tag, a
  // trailing `?` also admits Nil, generic args are documentation, and the
  // base case is that function's `_culebra_tag_name(tag) == expected` test —
  // which is why a hit here can never disagree with it. Anything else (a
  // user class, an enum, a trait) emits nothing and leaves the decision to
  // the check itself.
  void emit_type_check_gate_alt(std::string_view tn, int32_t subj,
                          std::vector<size_t>& skip) {
    if (culebra::is_fn_type(tn) || culebra::has_toplevel_plus(tn)) return;
    if (!tn.empty() && tn.back() == '?') {
      skip.push_back(emit(Op::JumpIfTag, subj, 0, TAG_NIL));
      emit_type_check_gate_alt(tn.substr(0, tn.size() - 1), subj, skip);
      return;
    }
    if (tn.find('<') != std::string_view::npos)
      tn = culebra::parse_generic_head(tn).outer;
    if (auto tag = _culebra_primitive_type_tag(tn))
      skip.push_back(emit(Op::JumpIfTag, subj, 0, *tag));
  }

  // Tag fast path in front of a ChkArg / ChkTypeAt: a matching alternative
  // jumps past the check, a miss falls into it. The check is emitted
  // unchanged, so every error keeps its kind, message and position, and the
  // gate can only ever skip a check that would have passed — the annotation
  // is a compile-time constant that the runtime otherwise re-scans on every
  // call. Splitting with the runtime's own depth-aware `has_toplevel_pipe`
  // keeps `Array<Long | Float>` one alternative, as _culebra_value_matches_type
  // reads it. Emitted as bytecode, so the executor, `--jit` and AOT all get it.
  void emit_type_check_gate(std::string_view full, int32_t subj,
                            std::vector<size_t>& skip) {
    if (culebra::has_toplevel_pipe(full)) {
      for (auto cand : culebra::split_union_types(full))
        emit_type_check_gate_alt(cand, subj, skip);
    } else {
      emit_type_check_gate_alt(full, subj, skip);
    }
  }

  // A pattern's type annotation: fall through when the subject satisfies it,
  // jump (via `fail`) when it doesn't. Mirrors the JIT's TYPED_IDENT emitter
  // alternative for alternative — a primitive name is a tag compare over the
  // shared _culebra_primitive_type_tag table (generic args stripped, so
  // `Array<Long>` gates on Array), and every other name — a user class, an
  // enum, a trait, `T?`, `fn(…) -> R` — asks the runtime's type_matches, the
  // one the interp's type_matches mirrors. A union ORs its alternatives, and
  // an `Any` alternative gates nothing at all.
  void compile_type_gate(const peg::Ast& type_node, int32_t subj,
                         std::vector<size_t>& fail) {
    auto full = type_node.token;
    // (tag, name): a set tag takes the inline compare, otherwise the name
    // goes to the runtime. Collected before emitting so an `Any` alternative
    // can drop the whole gate without leaving half of it behind.
    std::vector<std::pair<std::optional<int8_t>, std::string_view>> alts;
    auto add_single = [&](std::string_view tn) -> bool {  // false: Any
      auto head = tn;
      if (head.find('<') != std::string_view::npos)
        head = culebra::parse_generic_head(head).outer;
      if (head == "Any") return false;
      alts.push_back({_culebra_primitive_type_tag(head), head});
      return true;
    };
    if (full.find('|') != std::string_view::npos) {
      for (auto cand : culebra::split_union_types(full))
        if (!add_single(cand)) return;
    } else if (!add_single(full)) {
      return;
    }
    std::vector<size_t> ok;
    for (const auto& [tag, name] : alts) {
      if (tag) {
        ok.push_back(emit(Op::JumpIfTag, subj, 0, *tag));
        continue;
      }
      // TypeMatch falls through on a match; this chain wants the opposite
      // polarity, so the mismatch edge is what continues to the next
      // alternative.
      size_t nomatch = emit(Op::TypeMatch, subj, 0, kconst_str(name));
      ok.push_back(emit(Op::Jump));
      patch_to_here(nomatch);
    }
    fail.push_back(emit(Op::Jump));
    for (size_t ix : ok) patch_to_here(ix);
  }

  // Interpolated / triple strings over a normalized piece list: literal
  // chunks fold at compile time (consecutive ones into one constant, so a
  // pure-literal string is a single LoadConst with no concat chain — the
  // compile_triple_string fold); each `{expr}` piece renders through Disp,
  // or Fmt with its spec, and StrCat chains the accumulator left-to-right
  // (compile_interpolated_string's order). Strings aren't RC'd, so the
  // temps carry no release pressure.
  // Left-fold StrCat accumulator: the first piece becomes the accumulator,
  // later ones chain through a temp, and finish() answers the empty string
  // when nothing was added. Shared by the interpolation and format-spec
  // builders — the ownership-sensitive concat logic lives once.
  struct StrCatAcc {
    Compiler& c;
    const peg::Ast& at;
    int32_t acc = -1;
    void add(int32_t piece) {
      if (acc < 0) {
        acc = piece;
        return;
      }
      int32_t t = c.alloc_temp(at);
      c.emit(Op::StrCat, t, acc, piece);
      acc = t;
    }
    int32_t finish() {
      if (acc < 0) {
        acc = c.alloc_temp(at);
        c.emit(Op::LoadConst, acc, c.kconst_str(""));
      }
      return acc;
    }
  };

  ExprResult compile_interp_pieces(
      const peg::Ast& ast, const std::vector<culebra::InterpPiece>& pieces) {
    using namespace peg::udl;
    std::string pending;
    StrCatAcc cat{*this, ast};
    auto flush = [&] {
      if (pending.empty()) return;
      int32_t t = alloc_temp(ast);
      emit(Op::LoadConst, t, kconst_str(pending));
      pending.clear();
      cat.add(t);
    };
    for (const auto& p : pieces) {
      if (!p.expr) {
        pending += culebra::decode_interpolated_content(p.text);
        continue;
      }
      flush();
      const peg::Ast& node = *p.expr;
      auto view = culebra::view_interp_expr(node);
      auto v = compile_expr(*view.value);
      // A spec with its own `{field}` is built before the render op, out of
      // the value that is already in hand — the fields are ordinary
      // expressions and run in source order, as the other engines run them.
      int32_t spec_slot = -1;
      if (view.spec && !view.constant_spec)
        spec_slot = compile_format_spec(*view.spec);
      // SetOpPos rides the enclosing string literal's stamp (the pending
      // position here); the render op itself is stamped at the piece so
      // Fmt's spec errors report the `{expr:spec}` node.
      emit(Op::SetOpPos);
      int32_t t = alloc_temp(ast);
      StampGuard pos(*this, node);
      if (spec_slot >= 0)
        emit(Op::Fmt, t, v.slot, spec_slot, 1);
      else if (view.spec)
        emit(Op::Fmt, t, v.slot, kconst_str(view.spec_text));
      else
        emit(Op::Disp, t, v.slot);
      cat.add(t);
    }
    flush();
    return {cat.finish(), true};
  }

  // A spec carrying nested `{field}`s (`"{s:>{w}}"`): literal chunks are
  // constants, each field's Long becomes its decimal text, and the pieces
  // concatenate left to right — the interp's build_format_spec piece for
  // piece, so both assemble the same spec and reject the same non-Long
  // field at the field's own position.
  int32_t compile_format_spec(const peg::Ast& spec) {
    StrCatAcc acc{*this, spec};
    for (const auto& node : spec.nodes) {
      auto piece = culebra::view_spec_piece(*node);
      if (!piece.expr) {
        int32_t t = alloc_temp(spec);
        emit(Op::LoadConst, t, kconst_str(piece.text));
        acc.add(t);
        continue;
      }
      const auto& field = *piece.expr;
      auto v = compile_expr(field);
      {
        StampGuard fpos(*this, field);
        size_t skip = emit(Op::JumpIfTag, v.slot, 0, TAG_LONG);
        emit(Op::ChkTypeAt, v.slot, kconst_str("Long"),
             kconst_str(culebra::kSpecFieldContext), -1);
        patch_to_here(skip);
      }
      int32_t t = alloc_temp(field);
      emit(Op::Disp, t, v.slot);
      acc.add(t);
    }
    return acc.finish();
  }

  ExprResult compile_expr(const peg::Ast& ast) {
    using namespace peg::udl;
    StampGuard pos(*this, ast);
    switch (ast.tag) {
      case "NUMBER"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t,
             kconst_long(parse_integer_literal(ast.token)));
        return {t, true};
      }
      case "FLOAT"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t,
             kconst({TAG_FLOAT,
                     _culebra_double_to_bits(ast.token_to_number<double>())}));
        return {t, true};
      }
      case "BOOLEAN"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_BOOL, ast.token == "true"}));
        return {t, true};
      }
      case "NIL"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
        return {t, true};
      }
      case "STRING"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst_str(ast.token));
        return {t, true};
      }
      case "INTERPOLATED_STRING"_: {
        std::vector<culebra::InterpPiece> pieces;
        pieces.reserve(ast.nodes.size());
        for (const auto& piece : ast.nodes) {
          if (piece->tag == "INTERPOLATED_CONTENT"_)
            pieces.push_back({std::string(piece->token), nullptr});
          else
            pieces.push_back({{}, piece.get()});
        }
        return compile_interp_pieces(ast, pieces);
      }
      case "TRIPLE_STRING"_:
        // normalize_triple_pieces is the shared dedent authority (its
        // SyntaxError for a mis-indented close surfaces at compile time
        // here, like the JIT).
        return compile_interp_pieces(ast, culebra::normalize_triple_pieces(ast));
      case "ARRAY"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::ArrayNew, t);
        if (ast.nodes.size() > 1) {  // sized: [v](n) / [v](n, d)
          auto count = compile_expr(*ast.nodes[1]);
          ExprResult def{-1, false};
          if (ast.nodes.size() > 2) def = compile_expr(*ast.nodes[2]);
          // Count and default stay register-owned (array_resize borrows
          // both); every count error anchors at the array literal, the
          // interp's eval boundary / the JIT's PosGuard position.
          StampGuard pos(*this, ast);
          emit(Op::ArrayResize, t, count.slot, def.slot);
        }
        const auto& seq = *ast.nodes[0];
        bool has_spread = false;
        for (const auto& e : seq.nodes)
          if (e->tag == "SPREAD_ELEM"_) has_spread = true;
        for (size_t i = 0; i < seq.nodes.size(); i++) {
          const auto& e = *seq.nodes[i];
          if (e.tag == "SPREAD_ELEM"_) {
            // array_extend borrows the source — no owned_src: a temp's +1
            // falls to the statement sweep, a binding stays untouched.
            auto v = compile_expr(*e.nodes[0]);
            StampGuard pos(*this, e);
            emit(Op::ArrayExtend, t, v.slot);
          } else if (has_spread) {
            // A spread broke the index alignment: pure append, the JIT's
            // has_spread switch. (A sized literal cannot reach here — the
            // sized+spread mix is a parse-time SyntaxError.)
            auto v = compile_expr(e);
            emit(Op::ArrayPush, t, owned_src(e, v));
          } else {
            // ArrayAppend absorbs a +1: owned_src retains a borrowed slot
            // into a temp and drops a consumed temp from the sweep list.
            auto v = compile_expr(e);
            emit(Op::ArrayAppend, t, static_cast<int32_t>(i),
                 owned_src(e, v));
          }
        }
        return {t, true};
      }
      case "RANGE"_:
      case "RANGE_OPERATOR"_:  // bare `..`: open both ends
        return compile_range(ast);
      case "TUPLE"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::TupleNew, t);
        for (const auto& n : ast.nodes) {
          auto v = compile_expr(*n);
          emit(Op::TuplePush, t, owned_src(*n, v));
        }
        return {t, true};
      }
      case "SET"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::SetNew, t);
        for (const auto& n : ast.nodes) {
          auto v = compile_expr(*n);
          // set_add's unhashable throw is positionless — publish the
          // literal's position first, compile_set's emit order.
          StampGuard pos(*this, ast);
          emit(Op::SetOpPos);
          emit(Op::SetAdd, t, owned_src(*n, v));
        }
        return {t, true};
      }
      case "OBJECT"_: {
        int32_t t = alloc_temp(ast);
        // A literal whose properties are all plain IDENTIFIER keys — the
        // same subset the loop below always routes through Op::ObjectSet,
        // never ObjectSetAny (non-IDENTIFIER key) or ObjectMerge (spread) —
        // has an entirely static final key set. Pre-build its Shape and
        // slots once instead of transitioning through one Shape per key on
        // every execution (see ObjectNewShaped).
        bool all_static_keys = true;
        for (const auto& prop : ast.nodes) {
          if (prop->tag == "SPREAD_ELEM"_ ||
              culebra::view_object_property(*prop).key->tag !=
                  "IDENTIFIER"_) {
            all_static_keys = false;
            break;
          }
        }
        if (all_static_keys) {
          Chunk::ObjectShapeSpec spec;
          for (const auto& prop : ast.nodes) {
            auto key = culebra::view_object_property(*prop).key->token;
            auto* data = reinterpret_cast<const char*>(
                chunk_.consts[kconst_str(key)].data);
            // Dedup in first-occurrence order — matches the shape a
            // duplicate key would transition to via sequential appends
            // (find_slot hits the first one, later ObjectSets overwrite it).
            if (std::find_if(spec.keys.begin(), spec.keys.end(),
                             [&](const char* k) { return key == k; }) ==
                spec.keys.end())
              spec.keys.push_back(data);
          }
          int32_t spec_idx =
              static_cast<int32_t>(chunk_.object_shape_specs.size());
          chunk_.object_shape_specs.push_back(std::move(spec));
          emit(Op::ObjectNewShaped, t, spec_idx);
        } else {
          emit(Op::ObjectNew, t);
        }
        for (const auto& prop : ast.nodes) {
          if (prop->tag == "SPREAD_ELEM"_) {
            // object_merge borrows the source; no owned_src — a temp's +1
            // falls to the statement sweep, a binding stays untouched.
            auto v = compile_expr(*prop->nodes[0]);
            // The non-Object-source TypeError carries this instruction's own
            // position — anchor it at the spread element, compile_array's
            // ArrayExtend precedent (distinct entries can each misreport).
            StampGuard pos(*this, *prop);
            emit(Op::ObjectMerge, t, v.slot);
            continue;
          }
          auto pv = culebra::view_object_property(*prop);
          if (pv.key->tag != "IDENTIFIER"_) {
            // Non-IDENTIFIER literal key (String/Float/Number/Nil/Bool/
            // Tuple) — object_set_any. Both key and value are absorbed.
            auto key = compile_expr(*pv.key);
            auto val = compile_expr(*pv.value);
            // Hashing an unhashable key throws positionless, and a String
            // key can also hit the well-known-name contract check (both
            // positionless) — publish the literal's own position first,
            // compile_set's / emit_object_set's emit order.
            StampGuard pos(*this, ast);
            emit(Op::SetOpPos);
            emit(Op::ObjectSetAny, t, owned_src(*pv.key, key),
                 owned_src(*pv.value, val), pv.is_mut ? 1 : 0);
            continue;
          }
          // IDENTIFIER key: shorthand `{x}` reads the variable through the
          // ordinary identifier path (lookup / stdlib global / the same
          // compile-time "unresolved identifier" reject as a bare read —
          // interp's shorthand is a plain scope read too, so an undefined
          // name never reaches a runtime NameError in this slice).
          auto val = compile_expr(*pv.value);
          if (culebra::is_well_known_prop(pv.key->token)) {
            // The contract error is positionless; publish this literal's
            // position first (emit_object_set's condition — confined to the
            // four protocol names, a plain property pays nothing).
            StampGuard pos(*this, ast);
            emit(Op::SetOpPos);
          }
          emit(Op::ObjectSet, t, owned_src(*pv.value, val),
               kconst_str(pv.key->token), pv.is_mut ? 1 : 0);
        }
        return {t, true};
      }
      case "IDENTIFIER"_: {
        // A receiver frame's `fn` reads as the bound wrapper (the JIT's
        // fn.handle path); a direct `fn(...)` call skips it and re-passes
        // the receiver instead — see compile_call.
        if (ast.token == "fn" && chunk_.fn_bound_slot >= 0) {
          int32_t t = alloc_temp(ast);
          emit(Op::FnHandle, t, chunk_.self_slot, chunk_.fn_slot,
               chunk_.fn_bound_slot);
          return {t, true};
        }
        const Binding* b = lookup(ast.token);
        if (b) return read_binding(ast, *b);
        // `self` is not an unresolved name: every frame has one, and a frame
        // with no receiver holds the sentinel whose read is the interp's
        // NameError. Outside any function there is no slot to hold it, so
        // materialize the sentinel and let the same guard answer.
        // The exception is a session that already holds a `self`: only a
        // debugger builds one, and it does so from a paused method frame,
        // where `self` is exactly the receiver that frame is running on.
        if (ast.token == "self" && !session_declared("self")) {
          int32_t t = alloc_temp(ast);
          emit(Op::LoadConst, t, kconst({TAG_NO_SELF, 0}));
          emit(Op::UnboundErr, t, kconst_str("self"));
          return {t, true};
        }
        // A REPL line's unresolved names belong to the session, and its cell
        // outranks the stdlib global of the same name — `let to_string = 7`
        // on one line makes `to_string` read 7 on the next. Which of the two
        // it is stays a run-time question (the cell is still unbound when the
        // stdlib wins), and read_shadowing already asks it that way.
        if (repl_) {
          // Inside a function the name is normally a free variable whose cell
          // the enclosing frame captured, which is what lets the body run on
          // another thread. A name declared only in a block or arm that did
          // not run reaches here too: nothing captured it, and its unbound
          // cell raises the NameError the script lane raises.
          return read_binding(ast, bind_session(ast, std::string(ast.token)));
        }
        if (is_stdlib_global(ast.token) || is_stdlib_namespace(ast.token)) {
          int32_t t = alloc_temp(ast);
          emit(Op::NsGet, t, kconst_str(ast.token));
          return {t, true};
        }
        // Nothing here binds the name and no stdlib global answers it: the
        // read is the interpreter's run-time NameError, raised where it
        // stands. Compile-time rejection would decline programs that only
        // mention the name on a path they never take (a doc example whose
        // point IS the error, `if false { … }`), and the differential gates
        // are what still catch a name the VM failed to bind — the
        // interpreter answers those, so the lanes diverge rather than agree.
        {
          int32_t t = alloc_temp(ast);
          emit(Op::RaiseErr, 0, kconst_str("NameError"),
               kconst_str(culebra::format("undefined variable '{}'", ast.token)));
          return {t, true};  // unreachable
        }
      }
      case "UNARY_MINUS"_: {
        auto r = compile_expr(*ast.nodes[1]);  // nodes[0] is the operator
        int32_t t = alloc_temp(ast);
        emit(Op::Neg, t, r.slot);
        return {t, true};
      }
      case "UNARY_NOT"_: {
        auto r = compile_expr(*ast.nodes[1]);
        int32_t t = alloc_temp(ast);
        emit(Op::Not, t, r.slot);
        return {t, true};
      }
      case "UNARY_PLUS"_:
        return compile_expr(*ast.nodes[1]);
      case "UNARY_BNOT"_: {
        auto r = compile_expr(*ast.nodes[1]);
        int32_t t = alloc_temp(ast);
        emit(Op::BitNot, t, r.slot);
        return {t, true};
      }
      case "POWER"_: {
        // [base, POWER_OPERATOR, exponent]; right-associativity is the
        // grammar's recursion. Always the Pow op — see its enum note on
        // why the JIT's literal peepholes need no mirror here.
        auto base = compile_expr(*ast.nodes[0]);
        auto exp = compile_expr(*ast.nodes[2]);
        int32_t t = alloc_temp(ast);
        emit(Op::Pow, t, base.slot, exp.slot);
        return {t, true};
      }
      case "BIT_OR"_:
      case "BIT_XOR"_:
      case "BIT_AND"_:
      case "SHIFT"_:
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_: {
        // One left fold for every [operand, op-token]* chain; the eleven
        // tokens are disjoint across the levels, so one lookup serves all.
        auto acc = compile_expr(*ast.nodes[0]);
        for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
          auto op_tok = ast.nodes[i]->token;
          Op op;
          if (op_tok == "|") op = Op::BitOr;
          else if (op_tok == "^") op = Op::BitXor;
          else if (op_tok == "&") op = Op::BitAnd;
          else if (op_tok == "<<") op = Op::Shl;
          else if (op_tok == ">>") op = Op::Shr;
          else if (op_tok == "+") op = Op::Add;
          else if (op_tok == "-") op = Op::Sub;
          else if (op_tok == "*") op = Op::Mul;
          else if (op_tok == "/") op = Op::Div;
          else if (op_tok == "%") op = Op::Mod;
          else if (op_tok == "@") op = Op::MatMul;
          else reject(*ast.nodes[i], culebra::format("operator '{}'", op_tok));
          auto rhs = compile_expr(*ast.nodes[i + 1]);
          int32_t t = alloc_temp(ast);
          emit(op, t, acc.slot, rhs.slot);
          acc = {t, true};
        }
        return acc;
      }
      case "CONDITION"_:
        return compile_condition(ast);
      case "LOGICAL_AND"_:
        return compile_short_circuit(ast, Op::JumpIfFalse);
      case "LOGICAL_OR"_:
        return compile_short_circuit(ast, Op::JumpIfTrue);
      case "NIL_COALESCE"_:
        return compile_short_circuit(ast, Op::JumpIfNotNil);
      case "IF"_:
      case "CONDITIONAL"_:
        return compile_if(ast);
      case "COND"_:
        return compile_cond(ast);
      case "ASSIGNMENT"_:  // expression position: `let r = (w += 2)`
        return compile_assignment(ast);
      case "DESTRUCTURE_ASSIGN"_:
        return compile_destructure_assign(ast);
      case "PLACE_ASSIGN"_:
        return compile_place_assign(ast);
      case "TRY"_:
        return compile_try(ast);
      case "MATCH"_:
        return compile_match(ast);
      case "THROW"_: {
        int32_t src = owned_src(ast, compile_expr(*ast.nodes[0]));
        emit(Op::Throw, src);
        // Dummy nil for the (unreachable) expression value, keeping the
        // stream well-formed — the compile_throw pattern.
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
        return {t, true};
      }
      case "RETURN"_:
      case "BREAK"_:
      case "CONTINUE"_: {
        // The other diverging statements, reached wherever an expression is
        // (`let y = return e`, `f(return e)`, `[1, break]`): they never
        // produce a value, so the statement compiles as itself and the
        // dummy nil above stands in for the one nothing reads.
        compile_statement_inner(ast);
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
        return {t, true};
      }
      case "FUNCTION"_:
      case "LAMBDA"_: {
        // [PARAMETERS, RETURN_TYPE?, BLOCK] — the body is always last.
        int32_t idx =
            compile_fn_chunk(ast, ast.nodes[0].get(), *ast.nodes.back());
        int32_t t = alloc_temp(ast);
        emit(Op::MakeClosure, t, idx);
        return {t, true, idx};
      }
      case "CALL"_: {
        if (is_direct_global_call(ast, "println")) {
          emit(Op::Println, compile_expr(*ast.nodes[1]->nodes[0]).slot);
          int32_t t = alloc_temp(ast);
          emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
          return {t, true};
        }
        if (is_direct_global_call(ast, "to_float")) {
          int32_t v = compile_expr(*ast.nodes[1]->nodes[0]).slot;
          int32_t t = alloc_temp(ast);
          // The adapter reported its parse / type error at the call site
          // (_jit_thread.call_col), not at the argument — stamp the op after
          // the argument has compiled with positions of its own.
          StampGuard pos(*this, ast);
          emit(Op::ToFloat, t, v);
          return {t, true};
        }
        return compile_call(ast);
      }
      case "WHILE"_:
      case "FOR"_: {
        // Loops in value position evaluate to nil (interp parity:
        // `let d = while false { 1 }` reads nil).
        if (ast.tag == "WHILE"_) compile_while(ast);
        else compile_for(ast);
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
        return {t, true};
      }
      default:
        if (!ast.is_token && ast.nodes.size() == 1)
          return compile_expr(*ast.nodes[0]);
        reject(ast, culebra::format("expression '{}'", ast.name));
    }
  }
};

inline std::string dump(const Chunk& c) {
  static constexpr const char* kNames[] = {
      "LoadConst", "Move",      "Take",       "Retain",       "Release",
      "Neg",       "Not",       "Add",        "Sub",          "Mul",
      "Div",       "Mod",       "Pow",        "JumpIfSame",   "MatMul",
      "BitAnd",    "BitOr",     "BitXor",     "Shl",          "Shr",
      "BitNot",
      "Eq",        "Ne",        "Lt",
      "Le",        "Gt",        "Ge",         "ArrayNew",     "ArrayAppend",
      "ArrayPush", "ArrayExtend", "ArrayResize",
      "TupleNew",  "TuplePush", "SetNew",     "SetAdd",
      "ObjectNew", "ObjectNewShaped", "ObjectSet", "ObjectSetAny", "ObjectMerge",
      "ModReg",    "ModGet",
      "RangeNew",  "ChkLong",   "NilChk",
      "Index",     "IndexWr",   "IndexCo",    "IndexSet",
      "PropSet",   "PropWr",    "PropCo",     "NsWrChk",
      "PropVal",   "BareMethChk", "MethGate", "ChkParam", "CallRecv",
      "CbType",
      "ArityChk",  "BMeth",
      "PropRaw",
      "HasProp",   "Drop",      "ClsParamsChk", "ClsParamsWalk",
      "SeqChk",    "SeqGet",    "SeqRest",    "ObjGet",       "DestrErr",
      "Jump",      "JumpIfFalse", "JumpIfTrue", "JumpIfNotNil", "JumpIfNil",
      "JumpIfTag",
      "MakeClosure", "Call",    "CallM",      "CallKw",  "RaiseErr", "Ret",
      "CellNew",   "CellGet",   "CellSet",    "CellRelease",  "BindCapture",
      "ImmutErr",  "UnboundErr", "MultifnReg", "MfSelf",       "ClsSelf",      "WkErr",
      "ClassMeta", "DeriveFn",  "RegPack",    "EnumVariant",  "TypeMatch",
      "ClassObj",  "BindStatic",
      "MakeInst",  "FieldInit", "RegGetter",  "SelfMerge",
      "TraitReset", "TraitDefault", "TraitReg", "PosSnap", "ChkTypeAt",
      "ChkArg",    "JumpIfFilled", "ArgsRest",   "KwRest",     "RecEnter",
      "RecLeave",
      "NsGet",
      "SetOpPos",  "BoundPos",  "Disp",       "Fmt",          "StrCat",
      "Throw",     "DeferMark",  "DeferPush",    "DeferRunTo",
      "ForOpen",   "ForNext",   "ForDispose",
      "ForPrep",   "ForLoop",   "Println",    "ToFloat",      "Safepoint",
      "DropSuppress",
      "BArity",    "LazyNsReg", "FnHandle",  "OwnedMark", "OwnedExit",
      "ReplCell",  "ReplBind",  "DbgStmt",
      "Halt"};
  static_assert(std::size(kNames) == static_cast<size_t>(Op::Halt) + 1);
  std::string out;
  out += culebra::format("; slots: {}\n", c.num_slots);
  if (!c.capture_src_slots.empty()) {
    out += "; captures from creator slots:";
    for (auto s : c.capture_src_slots) out += culebra::format(" r{}", s);
    out += "\n";
  }
  for (size_t s = 0; s < c.slot_names.size(); ++s)
    out += culebra::format(";   r{} = {}\n", s, c.slot_names[s]);
  for (size_t i = 0; i < c.code.size(); ++i) {
    const auto& in = c.code[i];
    auto [line, col] = chunk_pos_at(c, i);
    out += culebra::format("{:4}: {:<12} {:4} {:4} {:4} {:4}   ; {}:{}", i,
                           kNames[static_cast<size_t>(in.op)], in.a, in.b, in.c,
                           in.d, line, col);
    if (auto t = chunk_call_target_at(c, i); t.chunk >= 0 || t.callee_in_cell) {
      if (t.chunk >= 0)
        out += culebra::format(
            "  -> chunk {}{}", t.chunk,
            t.reach == Chunk::Reach::Mono      ? " (mono)"
            : t.reach == Chunk::Reach::Guarded ? " (guarded)"
                                               : "");
      if (t.callee_in_cell) out += "  [callee in cell]";
    }
    out += "\n";
  }
  return out;
}

inline std::string dump(const VmProgram& p) {
  std::string out;
  for (size_t i = 0; i < p.chunks.size(); ++i) {
    const auto& c = p.chunks[i];
    if (i == 0) {
      out += "; == main ==\n";
    } else {
      out += culebra::format("; == fn {} (arity {}) ==\n", i, c.arity);
    }
    out += dump(c);
  }
  return out;
}

// The VM executor. Registers live in a C++ stack array so the conservative
// GC stack scan roots them for free. Numeric/comparison dispatch mirrors the
// JIT's emit paths instruction for instruction: the Long×Long fast path is
// inline, everything else funnels into the exact runtime helper the JIT
// calls — so behavior, error kinds/messages, and positions match by
// construction. Helper throws propagate out of run() (no EH in the slice
// yet); the abandoned frame's registers are reclaimed by the conservative
// backstop, mirroring the JIT's uncaught-error path.
struct Exec {
  // The capture holding this closure's chunk descriptor, or null when the
  // closure is not one of ours (its fn_ptr is not a VM trampoline). The
  // lazy-namespace registry rebuilds closures from it (see the desc hook).
  static JitCell* desc_for_closure(JitClosure* cls) {
    if (!cls || (cls->fn_ptr != reinterpret_cast<void*>(&trampoline) &&
                 cls->fn_ptr != reinterpret_cast<void*>(&getter_trampoline)))
      return nullptr;
    if (cls->n_captures == 0 || !cls->captures) return nullptr;
    return cls->captures[0];
  }

  // The validation every other consumer of the seam shares: captures[0]
  // carries a descriptor naming a chunk of a live program.
  static const VmFnDesc* closure_desc(JitClosure* cls) {
    auto* cell = desc_for_closure(cls);
    if (!cell) return nullptr;
    const auto* d = reinterpret_cast<const VmFnDesc*>(cell->value.data);
    if (!d || !d->prog ||
        static_cast<size_t>(d->chunk) >= d->prog->chunks.size())
      return nullptr;
    return d;
  }

  // What a keyword call binds against, for closures this executor runs. They
  // all share one fn_ptr, so the per-fn table cannot hold them; the chunk
  // behind the closure is in its descriptor capture, and the program built a
  // JitParamMeta for each.
  static const JitParamMeta* meta_for_closure(JitClosure* cls) {
    const auto* d = closure_desc(cls);
    if (!d || static_cast<size_t>(d->chunk) >= d->prog->param_metas.size())
      return nullptr;
    return &d->prog->param_metas[d->chunk]->meta;
  }

  // Third consumer of the same seam: whether this closure captures a `mut`
  // binding, which is what makes it unsendable. The chunk recorded the
  // names at compile time (see Chunk::mut_capture_names).
  static const std::string* mut_capture_for_closure(JitClosure* cls) {
    const auto* d = closure_desc(cls);
    if (!d) return nullptr;
    const auto& names = d->prog->chunks[d->chunk].mut_capture_names;
    return names.empty() ? nullptr : &names.front();
  }

  // A constructor thunk is this executor's synthetic `new`, and the interp
  // registers its own as native — which is what makes sending a class object
  // (it carries its `new`) the SendError it is everywhere else. An ordinary chunk closure stays
  // sendable: its descriptor names a chunk of the same program.
  static bool is_native_closure(JitClosure* cls) {
    const auto* d = closure_desc(cls);
    return d && d->prog->chunks[d->chunk].forwards_args;
  }

  // Safe for programs that outlive their own execution too (the REPL's
  // case): a closure one line built stays callable from a later one, since
  // the descriptor a closure carries is its own cell.
  static void run(VmProgram& p) {
    prepare(p);
    run_prepared(p);
  }

  static void prepare(VmProgram& p) {
    build_param_metas(p);
    _jit_closure_meta_hook = &meta_for_closure;
    _jit_closure_desc_hook = &desc_for_closure;
    _jit_closure_mut_capture_hook = &mut_capture_for_closure;
    _jit_closure_is_native_hook = &is_native_closure;
    p.descs.resize(p.chunks.size());
    for (size_t i = 0; i < p.chunks.size(); ++i)
      p.descs[i] = {&p, static_cast<int32_t>(i)};
  }

  static void run_prepared(VmProgram& p) {
    try {
      run_frame(p, 0, nullptr, 0, nullptr);
    } catch (const CulebraException& e) {
      // Uncaught user throw: format first, then consume the carrier's
      // reference — JIT::exec's boundary, so main.cc prints the same
      // "uncaught: ..." on every lane.
      auto s = format_uncaught_throw(e);
      _culebra_value_release_impl(e.tag, e.data);
      throw std::runtime_error(std::move(s));
    } catch (CulebraError& e) {
      // Backfill a positionless error from the published op position at
      // the engine boundary — JIT::exec's rule.
      _jit_backfill_op_pos(e);
      throw;
    }
  }

  // JitFn-ABI entry: native code (and the executor's own Call op) reaches a
  // VM function through the closure's fn_ptr like any other closure; the
  // descriptor in captures[0] says which chunk to interpret. The receiver
  // scalars are unused until methods enter the slice.
  static void trampoline(JitValue* ret, JitClosure* cls, int8_t self_tag,
                         int64_t self_data, int64_t n_args, JitValue* args) {
    auto* d = reinterpret_cast<const VmFnDesc*>(cls->captures[0]->value.data);
    *ret = run_frame(*d->prog, d->chunk, cls, n_args, args, self_tag,
                     self_data);
  }

  // The same entry point, for getter bodies only. The runtime decides
  // whether a bare `obj.name` read invokes the method by looking its
  // fn_ptr up in the getter registry — a key that identifies the code
  // behind a closure. In the lowering each chunk is its own function, so
  // that key already distinguishes getters; here one interpreter runs
  // every chunk, so getters need a second door to keep the key honest.
  static void getter_trampoline(JitValue* ret, JitClosure* cls,
                                int8_t self_tag, int64_t self_data,
                                int64_t n_args, JitValue* args) {
    trampoline(ret, cls, self_tag, self_data, n_args, args);
  }

  static JitValue run_frame(const VmProgram& p, int32_t chunk_idx,
                            JitClosure* cls, int64_t n_args, JitValue* args,
                            int8_t self_tag = TAG_NO_SELF,
                            int64_t self_data = 0) {
    const Chunk& c = p.chunks[chunk_idx];
    if (c.num_slots > kMaxSlots)
      throw CulebraError("VmError", "--vm: frame too large");
    // Only the frame's live window, and only as much machine stack as it
    // needs. A fixed kMaxSlots array cost 4 KB of C stack per culebra
    // frame whatever the chunk's size, which put the recursion limit out
    // of reach on any shape that nests two frames per level: a `new`
    // recursing through its ctor thunk ran out of an 8 MB stack at ~760
    // levels and took SIGSEGV where the other backends raise
    // RecursionError. The window stays on the machine stack so the
    // conservative collector keeps finding these roots by scanning it
    // (a heap buffer would be invisible without registering it).
    // zero-init == {TAG_NIL, 0}; the executor never reads past num_slots.
    JitValue regs[c.num_slots > 0 ? c.num_slots : 1];
    std::memset(regs, 0, sizeof(JitValue) * static_cast<size_t>(c.num_slots));
    // A debug session keeps the frame stack: the entry goes up as soon as
    // the window exists and comes down on every exit, throw included.
    struct DbgFrameGuard {
      bool on;
      DbgFrameGuard(bool on, const VmProgram& p, const Chunk& c, JitValue* regs)
          : on(on) {
        if (on) dbg_state().frames.push_back({&p, &c, regs});
      }
      ~DbgFrameGuard() {
        if (on) dbg_state().frames.pop_back();
      }
    } dbg_frame{dbg_state().tracking, p, c, regs};
    if (chunk_idx != 0) {
      // Param binding, mirroring the JIT prologue: fewer args than the
      // required count raises the interp's ArityError at the published call
      // site; extras drop (their +1 released) since __ARGS__ is outside the
      // slice. The recursion guard is a prologue instruction, after the
      // parameters bind (TypeError-before-RecursionError order), and `leave`
      // runs only on the normal return — unwinding skips it, like the JIT's
      // frames.
      if (!c.forwards_args && n_args < c.required) {
        for (int64_t i = 0; i < n_args; ++i)
          culebra_runtime_value_release(static_cast<int8_t>(args[i].tag),
                                        args[i].data);
        // The receiver's +1 would strand here too — the JIT's arity error
        // releases it in the same prologue, before the frame's cleanup
        // ladder covers anything.
        culebra_runtime_value_release(self_tag, self_data);
        // The runtime helper owns the diagnostic (message + prefer-the-
        // published-call-site policy) for every lane; cold path.
        std::vector<const char*> names;
        names.reserve(c.param_names.size());
        for (const auto& nm : c.param_names) names.push_back(nm.c_str());
        culebra_runtime_arity_missing(names.data(), n_args, 0, 0);
      }
      // A slot the caller did not fill takes the sentinel its defaulted
      // param's JumpIfFilled tests (the kwargs resolver's middle-gap tag,
      // which is also what a compiled caller leaves in the slab). A frame
      // that forwards its arguments (the ctor thunk) touches neither.
      if (!c.forwards_args) {
        // Where the caller's values stop being bindings: a resolved slab
        // fills every slot up to the arity, while a plain positional call to
        // a function with a `**rest` has nothing to bind past its last
        // regular parameter — the rest of what it passed is overflow. The
        // resolver's marker on the rest slot is what tells the two apart.
        int64_t from_args = c.arity;
        if (c.kwargs_rest_idx >= 0 &&
            !(c.kwargs_rest_idx < n_args &&
              args[c.kwargs_rest_idx].tag == TAG_KWREST))
          from_args = c.first_kw_only_idx >= 0 ? c.first_kw_only_idx
                                               : c.kwargs_rest_idx;
        for (int32_t i = 0; i < c.arity; ++i)
          regs[i] = (i < from_args && i < n_args) ? args[i]
                                                  : JitValue{TAG_UNFILLED, 0};
        // A chunk that reads the overflow keeps their `+1`s for the Array
        // its ArgsRest instruction builds.
        if (!c.keeps_args)
          for (int64_t i = from_args; i < n_args; ++i)
            culebra_runtime_value_release(static_cast<int8_t>(args[i].tag),
                                          args[i].data);
      }
      // The receiver: a receiver frame's slot takes the +1 (and the frame
      // teardown releases it); every other frame drops it, like the JIT's
      // constructor thunk does with the class object it was called on. An
      // absent receiver (TAG_NO_SELF) reads as nil, the interp's plain-call
      // fallthrough.
      if (c.self_slot >= 0) {
        regs[c.self_slot] = (self_tag == TAG_NO_SELF && !c.self_raw)
                                ? JitValue{TAG_NIL, 0}
                                : JitValue{self_tag, self_data};
      } else {
        culebra_runtime_value_release(self_tag, self_data);
      }
      if (c.fn_slot >= 0) {
        regs[c.fn_slot] =
            JitValue{TAG_FUNC, reinterpret_cast<int64_t>(cls)};
        culebra_runtime_value_retain(TAG_FUNC, regs[c.fn_slot].data);
      }
    }
    // A try handler restores the recursion count to this frame's own level
    // (frames unwound between the throw and the handler never ran their
    // `leave`) — the JIT's try.rec snapshot, taken where its prologue takes
    // it: the RecEnter instruction, once the parameters are bound. Until it
    // runs the frame is uncounted, and the "never entered" sentinel makes
    // every restore below a no-op (a default expression that throws unwinds
    // through a frame that never touched the count).
    int64_t frame_depth = chunk_idx != 0 ? -1
                                         : culebra_runtime_recursion_depth();
    const Insn* code = c.code.data();
    // The frame's owned-stack watermarks, one per open scope depth. Kept
    // out of the register file on purpose — see Op::OwnedMark. Sized from the
    // chunk like the register window above, not from kMaxOwnedDepth: a fixed
    // array charges every frame for the deepest nesting the budget allows,
    // which is what put the register window's recursion limit out of reach
    // before Phase 2 shrank it.
    int64_t marks[c.owned_depths > 0 ? c.owned_depths : 1];
    size_t pc = 0;
    // Dispatch, re-entering at a try scope's handler when a throw lands
    // inside one. Classification shares the JIT landingpad's carrier
    // machinery: try_translate materializes a CulebraError as an error
    // Object, a user throw already carries a value, and a foreign exception
    // leaves is_throw=0 and keeps unwinding (as does any throw with no
    // enclosing try).
    for (;;) {
      try {
        return dispatch(p, c, code, regs, pc, chunk_idx, cls, frame_depth,
                        n_args, args, marks);
      } catch (...) {
        if (unwind(c, regs, pc, chunk_idx, frame_depth, marks)) continue;
        throw;
      }
    }
  }

  // The throw path: the interpreter's scope-by-scope teardown, which the JIT
  // emits as a chain of cleanup pads and the lowering mirrors block for
  // block. The in-flight temporaries of the throwing expression die first,
  // then each enclosing scope runs its pending defers and releases its own
  // bindings, innermost outward — so a `defer` and the `drop`s of the scope
  // it guards interleave the way they do on the other two backends. Returns
  // true when a try scope caught it (`pc` then sits at its handler);
  // otherwise the frame is uncounted and the caller re-raises.
  static bool unwind(const Chunk& c, JitValue* regs, size_t& pc,
                     int32_t chunk_idx, int64_t frame_depth,
                     const int64_t* marks) {
    // Restore this frame's depth before any of it runs: the unwound callees
    // never ran their `leave`, and the defers (and any `drop` the releases
    // fire) must count from here. Every restore ignores the "never entered"
    // sentinel.
    culebra_runtime_recursion_restore(frame_depth);
    // A temporary is never a cell in its own generation, so it always takes
    // the plain release — and nils its slot, which is what makes the range
    // release below safe on an index a later generation turned into a cell.
    auto temps = chunk_temps_at(c, pc);
    int32_t floor = chunk_temp_floor(c, pc);
    for (size_t i = temps.size(); i > 0; --i)
      if (temps[i - 1] >= floor)
        release_slot(c, regs, temps[i - 1], /*as_cell=*/false);
    for (int32_t k = chunk_innermost_cleanup(c, pc); k >= 0;) {
      const auto& cu = c.cleanups[static_cast<size_t>(k)];
      bool frame = cu.parent < 0;
      // The tag check is belt-and-braces: a scope's DeferMark runs before
      // any instruction its range covers.
      if (cu.defer_mark_slot >= 0 &&
          regs[cu.defer_mark_slot].tag == TAG_LONG)
        culebra_runtime_defer_run_to(regs[cu.defer_mark_slot].data);
      bool hush = frame && c.suppress_frame_drop;
      if (hush) culebra_runtime_set_drop_suppressed(1);
      for (int32_t s : chunk_release_order(c, cu.slot_lo, cu.slot_hi)) {
        // A for-in's own step closes its iterator here: the rungs above have
        // already released the element, and this one releases the iterator
        // itself. The dispose is swallowed — an exception is in flight.
        if (cu.dispose_base >= 0 && s == cu.dispose_base + kForIter)
          for_dispose(regs + cu.dispose_base, /*swallow=*/true);
        release_slot(c, regs, s, chunk_slot_is_cell(c, s, cu.cells_before));
      }
      if (hush) culebra_runtime_set_drop_suppressed(0);
      // The frame's own step resolves the owned region once, after its
      // releases — the JIT resolves at its frame cleanup pad and nowhere
      // else on the throw path, so an inner scope's escaped resources wait
      // for the frame here too. Suppressed at program exit.
      if (frame && !hush && c.owned_frame_depth >= 0)
        culebra_runtime_owned_scope_exit(marks[c.owned_frame_depth]);
      if (cu.handler != Chunk::kNoHandler) {
        culebra_runtime_try_translate();
        if (culebra_runtime_get_is_throw()) {
          culebra_runtime_clear_is_throw();
          regs[cu.caught_slot] = JitValue{culebra_runtime_get_thrown_tag(),
                                          culebra_runtime_get_thrown_data()};
          pc = static_cast<size_t>(cu.handler);
          return true;
        }
        // Foreign: keep unwinding, the enclosing scopes' cleanup included.
      }
      k = cu.parent;
    }
    // Uncount the frame on the way out (chunk 0 is the program itself).
    if (chunk_idx != 0) culebra_runtime_recursion_restore(frame_depth - 1);
    return false;
  }

  // One slot's release, with the caller deciding cell or plain the way the
  // compiled ladders do (per generation — see Chunk::slot_cell_rank).
  // Release is destructive and nil-safe, so a slot an inner step already
  // emptied costs one no-op.
  static void release_slot(const Chunk& c, JitValue* regs, int32_t s,
                           bool as_cell) {
    if (s < 0 || s >= c.num_slots) return;
    if (as_cell)
      culebra_runtime_cell_release(reinterpret_cast<JitCell*>(regs[s].data));
    else
      culebra_runtime_value_release(static_cast<int8_t>(regs[s].tag),
                                    regs[s].data);
    regs[s] = JitValue{TAG_NIL, 0};
  }

  // --- for-in (the ForSlot cursor) --------------------------------------
  // `cur` points at the run's base, so the three below read the same layout
  // the compiler laid out and the lowering re-emits — each mirrors one of
  // the JIT's for-in emitters, over the same runtime helpers.

  // The tag dispatch: pick the cursor that walks this iterable and fill it.
  // The JIT twin is emit_for_open_dispatch.
  static void for_open(JitValue* cur, int64_t line, int64_t col) {
    cur[kForDisposed] = JitValue{TAG_LONG, 0};  // re-entry: a fresh walk
    JitValue it = cur[kForIterable];
    auto proto_open = [&](JitValue src) {
      // The slot owns what the protocol derives from, so the `iter()` frame
      // gets its own `+1` for `self` and the two refs die with two slots.
      cur[kForSrc] = src;
      JitPropIC ic{};  // per-call scratch: the executor never reads a cache
      JitValue iter_fn = culebra_runtime_prop_get(
          static_cast<int8_t>(src.tag), src.data, "iter", &ic, line, col,
          /*own_receiver=*/false);
      if (iter_fn.tag != TAG_FUNC) {
        culebra_runtime_type_error_typed(line, col, "Function",
                                         static_cast<int8_t>(iter_fn.tag));
      }
      culebra_runtime_value_retain(static_cast<int8_t>(src.tag), src.data);
      JitValue iter = _jit_invoke(reinterpret_cast<JitClosure*>(iter_fn.data),
                                  src, 0, nullptr);
      JitClosure* has_next = nullptr;
      JitClosure* next = nullptr;
      {
        // The open validates before the slot takes the iterator over, so a
        // broken one is released here rather than left to the ladder.
        JitUnwindRelease guard{iter};
        culebra_runtime_iter_protocol_open(static_cast<int8_t>(iter.tag),
                                           iter.data, line, col, &has_next,
                                           &next);
      }
      cur[kForIter] = iter;
      cur[kForHasNext] = {TAG_LONG, reinterpret_cast<int64_t>(has_next)};
      cur[kForNext] = {TAG_LONG, reinterpret_cast<int64_t>(next)};
      cur[kForKind] = {TAG_LONG, FOR_PROTO};
    };
    auto open_counted = [&](int64_t ptr, int64_t count, int64_t kind) {
      cur[kForPtr] = {TAG_LONG, ptr};
      cur[kForCount] = {TAG_LONG, count};
      cur[kForPos] = {TAG_LONG, 0};
      cur[kForKind] = {TAG_LONG, kind};
    };
    switch (it.tag) {
      case TAG_ARRAY:
      case TAG_TUPLE: {
        auto* arr = reinterpret_cast<JitArray*>(it.data);
        open_counted(it.data, culebra_runtime_array_size(arr), FOR_ARRAY);
        return;
      }
      case TAG_SET: {
        // A Set walks a temporary Array of its members; the slot owns it.
        auto* members = culebra_runtime_set_to_array(
            reinterpret_cast<JitSet*>(it.data));
        cur[kForSetArr] = {TAG_ARRAY, reinterpret_cast<int64_t>(members)};
        open_counted(reinterpret_cast<int64_t>(members),
                     culebra_runtime_array_size(members), FOR_ARRAY);
        return;
      }
      case TAG_STRING:
      case TAG_STRINGVIEW: {
        auto* bytes = culebra_runtime_strlike_to_cstr(
            static_cast<int8_t>(it.tag), it.data);
        open_counted(reinterpret_cast<int64_t>(bytes),
                     culebra_runtime_str_size(bytes), FOR_STRING);
        return;
      }
      case TAG_OBJECT: {
        // A Range walks its own iterator, a value carrying `iter` drives
        // itself, and anything else takes the built-in key iterator — the
        // JIT's three object arms, converging on one protocol open.
        if (culebra_runtime_is_range(static_cast<int8_t>(it.tag), it.data)) {
          auto* ri = culebra_runtime_range_iter(it.data, line, col);
          proto_open({TAG_OBJECT, reinterpret_cast<int64_t>(ri)});
          return;
        }
        auto* obj = reinterpret_cast<JitObject*>(it.data);
        if (culebra_runtime_object_has(obj, "iter")) {
          culebra_runtime_value_retain(static_cast<int8_t>(it.tag), it.data);
          proto_open(it);
          return;
        }
        auto* keys = culebra_runtime_object_iter(obj);
        proto_open({TAG_OBJECT, reinterpret_cast<int64_t>(keys)});
        return;
      }
      default:
        culebra_runtime_type_error_typed(
            line, col, "Array, Tuple, Set, Object, or String",
            static_cast<int8_t>(it.tag));
    }
  }

  // One step. True with the element's `+1` parked in kForElem, false when
  // the walk is over (emit_for_advance_{array,protocol,string}).
  static bool for_next(JitValue* cur) {
    int8_t tag = TAG_NIL;
    int64_t data = 0;
    switch (cur[kForKind].data) {
      case FOR_ARRAY: {
        // Re-read the size rather than trust the opening count: the body may
        // shrink the receiver, and the walk ends where the live array does.
        auto* arr = reinterpret_cast<JitArray*>(cur[kForPtr].data);
        int64_t i = cur[kForPos].data;
        if (i >= culebra_runtime_array_size(arr)) return false;
        cur[kForPos].data = i + 1;
        culebra_runtime_array_get(arr, i, &tag, &data, 0, 0);
        // Read straight out of the container: the slot wants its own +1.
        culebra_runtime_value_retain(tag, data);
        break;
      }
      case FOR_STRING: {
        auto* bytes = reinterpret_cast<const char*>(cur[kForPtr].data);
        int64_t off = cur[kForPos].data;
        int64_t n = culebra_runtime_utf8_scalar_len(bytes, off,
                                                    cur[kForCount].data);
        if (n == 0) return false;
        cur[kForPos].data = off + n;
        // A 1-scalar view into the source buffer, as `s.iter()` yields.
        data = reinterpret_cast<int64_t>(
            culebra_runtime_str_scalar_view(bytes, off, n));
        tag = TAG_STRINGVIEW;
        culebra_runtime_value_retain(tag, data);
        break;
      }
      default: {
        // iter_advance transfers the step value's +1 already.
        if (!culebra_runtime_iter_advance(
                reinterpret_cast<JitClosure*>(cur[kForHasNext].data),
                reinterpret_cast<JitClosure*>(cur[kForNext].data),
                static_cast<int8_t>(cur[kForIter].tag), cur[kForIter].data,
                &tag, &data))
          return false;
        break;
      }
    }
    cur[kForElem] = JitValue{tag, data};
    return true;
  }

  // Close the iterator if it carries `dispose`, once. The slot keeps its own
  // ref throughout — the ladder that runs this frees it a rung later — so a
  // throwing dispose strands nothing (emit_iter_dispose_if_active).
  static void for_dispose(JitValue* cur, bool swallow) {
    if (cur[kForDisposed].data) return;
    cur[kForDisposed] = JitValue{TAG_LONG, 1};
    JitValue iter = cur[kForIter];
    if (iter.tag == TAG_NIL) return;
    auto* obj = reinterpret_cast<JitObject*>(iter.data);
    if (!culebra_runtime_object_has(obj, "dispose")) return;
    JitPropIC ic{};
    JitValue fn = culebra_runtime_prop_get(static_cast<int8_t>(iter.tag),
                                           iter.data, "dispose", &ic, 0, 0,
                                           /*own_receiver=*/false);
    if (fn.tag != TAG_FUNC) return;
    // The frame gets its own `+1` for `self`, as the iter() call above did.
    auto call = [&] {
      culebra_runtime_value_retain(static_cast<int8_t>(iter.tag), iter.data);
      JitValue r = _jit_invoke(reinterpret_cast<JitClosure*>(fn.data), iter, 0,
                               nullptr);
      culebra_runtime_value_release(static_cast<int8_t>(r.tag), r.data);
    };
    if (!swallow) {
      call();
      return;
    }
    // An exception is already unwinding: the dispose's own throw must not
    // replace it, and neither must the value it threw — the thrown-value
    // carrier is a global that a culebra throw overwrites.
    int8_t flag = 0, tag = 0;
    int64_t data = 0;
    culebra_runtime_save_thrown(&flag, &tag, &data);
    try {
      call();
    } catch (...) {
    }
    culebra_runtime_restore_thrown(flag, tag, data);
  }

  // The call site, with this call's per-argument positions when it has any
  // — one runtime entry publishes both, and set_call_site alone (which
  // resets the count) is what an argument-less call wants.
  // Whether an Object receiver reaches the builtin tables at all. A view
  // (Shared / packed / fixed-array) answers from what it wraps, so it misses
  // the table rather than taking its diagnostic. A namespace is NOT excluded:
  // the dict builtins are the one family it does resolve (`Math.size()` is
  // 24), and every other name is absent from the table anyway, so it reaches
  // its own AttributeError through the property read that follows.
  static bool object_takes_builtin_table(JitObject* o) {
    return culebra_runtime_nc_receiver_kind(reinterpret_cast<int64_t>(o)) == 0;
  }

  // The owned stack's next id, read the way compiled code reads it: off the
  // hot fields whose address the runtime hands out (next_id is at offset 0).
  static int64_t owned_next_id() {
    return *reinterpret_cast<const int64_t*>(
        static_cast<uintptr_t>(culebra_runtime_owned_hot()));
  }

  static void publish_call_site(const Chunk& c, size_t pc, int64_t line,
                                int64_t col) {
    if (const auto* ap = chunk_argpos_at(c, pc))
      culebra_runtime_set_call_positions(
          line, col, static_cast<int64_t>(ap->size()), ap->data());
    else
      culebra_runtime_set_call_site(line, col);
  }

  // What a borrowed callee stands on, checked where the assert lanes run it
  // (`just test-assert`, CI's linux-assert): the cell the call read through
  // still holds the same value once the call is over — the throw path
  // included. That is the whole claim borrowed_call_head makes; a cell that
  // could be written under its own call would leave the frame running on a
  // closure nothing holds, so it is worth a lane rather than a comment.
  // Release builds keep an empty destructor and the members fall away.
  struct BorrowWitness {
    const JitValue* at;
    JitValue entry;
    explicit BorrowWitness(const JitValue* at)
        : at(at), entry(at ? *at : JitValue{TAG_NIL, 0}) {}
    ~BorrowWitness() {
      assert(!at || (at->tag == entry.tag && at->data == entry.data));
    }
  };

  // The frame a resolved call site enters (Chunk::call_targets): the
  // Function gate and the keyword-only guard both have static answers and
  // the chunk to run is known, so neither the cold probes nor the closure's
  // own fn_ptr is consulted. `run` is the receiver-and-arguments run the
  // callee consumes, nil'd on every exit as the dynamic arms do.
  static JitValue run_resolved(const VmProgram& p, int32_t tgt,
                               const JitValue& callee, JitValue self,
                               JitValue* run, int32_t n_run, int32_t argc,
                               int64_t line, int64_t col) {
    assert(call_target_holds(p, callee, tgt));
    culebra::throw_if_too_many_positionals(
        p.chunks[static_cast<size_t>(tgt)].first_kw_only_idx, argc, line, col);
    struct Drain {
      JitValue* run;
      int32_t n;
      ~Drain() {
        for (int32_t i = 0; i < n; ++i) run[i] = JitValue{TAG_NIL, 0};
      }
    } drain{run, n_run};
    return run_frame(p, tgt, reinterpret_cast<JitClosure*>(callee.data), argc,
                     argc ? run + (n_run - argc) : nullptr,
                     static_cast<int8_t>(self.tag), self.data);
  }

  // Which closure a Reach::Mono site enters: the dispatcher's monomorphic
  // body, since a dispatcher's own captures are not the ones the body's
  // frame reads. Nil where that shortcut is gone, and the site then takes
  // the dynamic arm and its DispatchError.
  static JitValue mono_callee(const JitValue& callee) {
    auto* body =
        _jit_dispatcher_mono_body(reinterpret_cast<JitClosure*>(callee.data));
    return body ? JitValue{TAG_FUNC, reinterpret_cast<int64_t>(body)}
                : JitValue{TAG_NIL, 0};
  }

  // The closure a resolved site enters, and whether its shape's one run-time
  // question — if it has one — came out yes. A `no` sends the site down the
  // dynamic arm it would have taken all along. Both Call and CallM ask here,
  // so a shape is never handled by one and ignored by the other.
  static bool resolved_entry(const VmProgram& p, Chunk::CallTarget tgt,
                             const JitValue& callee, JitValue& entered) {
    entered = callee;
    switch (tgt.reach) {
      case Chunk::Reach::Direct:
        return true;
      case Chunk::Reach::Mono:
        entered = mono_callee(callee);
        return entered.tag == TAG_FUNC;
      case Chunk::Reach::Guarded:
        return call_target_holds(p, callee, tgt.chunk);
    }
    return false;
  }

  // Is the closure a resolved site was handed the chunk the compiler named?
  // A resolved site does not ask at run time — the analysis has no fallback
  // — so this is what the assert lane checks, over the same sweep every
  // other build runs (`just test-assert`, and CI's linux-assert job).
  static bool call_target_holds(const VmProgram& p, const JitValue& callee,
                                int32_t tgt) {
    if (callee.tag != TAG_FUNC) return false;
    const auto* d = closure_desc(reinterpret_cast<JitClosure*>(callee.data));
    return d && d->prog == &p && d->chunk == tgt;
  }

  // The dispatch loop proper: runs until Ret/Halt, or unwinds with `pc`
  // still at the faulting instruction (run_frame's catch consults it).
  static JitValue dispatch(const VmProgram& p, const Chunk& c,
                           const Insn* code, JitValue* regs, size_t& pc,
                           int32_t chunk_idx, JitClosure* cls,
                           int64_t& frame_depth, int64_t n_args,
                           JitValue* args, int64_t* marks) {
    // Numeric conversions via the runtime's own bit-punning helpers; both
    // call sites guard with both_num, so coerce_num's throw arm is dead.
    auto as_double = [](const JitValue& v) {
      return _culebra_coerce_num(static_cast<int8_t>(v.tag), v.data);
    };
    auto from_double = [](double d) {
      return JitValue{TAG_FLOAT, _culebra_double_to_bits(d)};
    };
    auto both_long = [](const JitValue& l, const JitValue& r) {
      return l.tag == TAG_LONG && r.tag == TAG_LONG;
    };
    auto both_num = [](const JitValue& l, const JitValue& r) {
      return (l.tag == TAG_LONG || l.tag == TAG_FLOAT) &&
             (r.tag == TAG_LONG || r.tag == TAG_FLOAT);
    };
    // Borrow-contract helpers throughout the dispatch (the `_borrow` twins):
    // operands stay owned by the frame's registers on every path, so a try
    // handler's release ladder is the one releaser after a throw.
    auto to_bool = [&](const JitValue& v, size_t at) {
      if (v.tag == TAG_BOOL) return v.data != 0;
      auto [line, col] = chunk_pos_at(c, at);
      return culebra_runtime_to_bool_borrow(static_cast<int8_t>(v.tag),
                                            v.data, line, col);
    };

    // The safepoint poll below runs per instruction, so resolve the heap
    // once: the Heap lives in the Runtime's substate for this frame's whole
    // execution (RuntimeScope restores any nested switch before control
    // returns here), and hoisting skips the thread-local + lazy-init chain
    // the opaque calls in the loop would otherwise force per iteration.
    [[maybe_unused]] gc::Heap* sp_heap = nullptr;
    if constexpr (gc::kDeferToSafepoint) sp_heap = &_gc_heap();
    for (;;) {
      // The wasm safepoint: the only place a deferred collect runs (jit_gc.h
      // kDeferToSafepoint). Between instructions every live value of every
      // frame sits in a register window on the linear-memory stack, which
      // the conservative scan does see — unless a helper frame that may hold
      // sole references is suspended below us, which safepoint_collect
      // checks. Folds away on native builds.
      if constexpr (gc::kDeferToSafepoint) {
        if (sp_heap->safepoint_pending()) sp_heap->safepoint_collect();
      }
      const Insn& in = code[pc];
      switch (in.op) {
        case Op::LoadConst:
          regs[in.a] = c.consts[in.b];
          ++pc;
          break;
        case Op::Move:
          regs[in.a] = regs[in.b];
          ++pc;
          break;
        case Op::Take:
          regs[in.a] = regs[in.b];
          regs[in.b] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::Retain:
          culebra_runtime_value_retain(static_cast<int8_t>(regs[in.a].tag),
                                       regs[in.a].data);
          ++pc;
          break;
        case Op::Release:
          culebra_runtime_value_release(static_cast<int8_t>(regs[in.a].tag),
                                        regs[in.a].data);
          regs[in.a] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::Neg: {
          const JitValue& v = regs[in.b];
          if (v.tag == TAG_LONG) {
            regs[in.a] = JitValue{
                TAG_LONG, static_cast<int64_t>(
                              0 - static_cast<uint64_t>(v.data))};
          } else {
            auto [line, col] = chunk_pos_at(c, pc);
            regs[in.a] = culebra_runtime_num_neg_borrow(
                static_cast<int8_t>(v.tag), v.data, line, col);
          }
          ++pc;
          break;
        }
        case Op::Not:
          regs[in.a] = JitValue{TAG_BOOL, !to_bool(regs[in.b], pc)};
          ++pc;
          break;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          JitValue out;
          if (both_long(l, r)) {
            auto lu = static_cast<uint64_t>(l.data);
            auto ru = static_cast<uint64_t>(r.data);
            switch (in.op) {
              case Op::Add: out = {TAG_LONG, static_cast<int64_t>(lu + ru)}; break;
              case Op::Sub: out = {TAG_LONG, static_cast<int64_t>(lu - ru)}; break;
              case Op::Mul: out = {TAG_LONG, static_cast<int64_t>(lu * ru)}; break;
              case Op::Div:
              case Op::Mod: {
                if (r.data == 0) {
                  auto [line, col] = chunk_pos_at(c, pc);
                  culebra_runtime_div_zero(line, col);
                }
                out = {TAG_LONG, in.op == Op::Div ? l.data / r.data
                                                  : l.data % r.data};
                break;
              }
              default: __builtin_unreachable();
            }
          } else if (both_num(l, r)) {
            double ld = as_double(l), rd = as_double(r);
            switch (in.op) {
              case Op::Add: out = from_double(ld + rd); break;
              case Op::Sub: out = from_double(ld - rd); break;
              case Op::Mul: out = from_double(ld * rd); break;
              case Op::Div:
              case Op::Mod: {
                if (rd == 0.0) {
                  auto [line, col] = chunk_pos_at(c, pc);
                  culebra_runtime_div_zero(line, col);
                }
                out = from_double(in.op == Op::Div ? ld / rd
                                                   : std::fmod(ld, rd));
                break;
              }
              default: __builtin_unreachable();
            }
          } else {
            // d=1 (a compound step) picks the in-place twin: a Tensor lhs
            // mutates its buffer and returns itself (+1), the interp's
            // try_tensor_inplace. Mod has no in-place form.
            auto [line, col] = chunk_pos_at(c, pc);
            auto lt = static_cast<int8_t>(l.tag);
            auto rt = static_cast<int8_t>(r.tag);
            switch (in.op) {
              case Op::Add:
                out = in.d ? culebra_runtime_num_inplace_add_borrow(
                                 lt, l.data, rt, r.data, line, col)
                           : culebra_runtime_num_add_borrow(lt, l.data, rt,
                                                            r.data, line, col);
                break;
              case Op::Sub:
                out = in.d ? culebra_runtime_num_inplace_sub_borrow(
                                 lt, l.data, rt, r.data, line, col)
                           : culebra_runtime_num_sub_borrow(lt, l.data, rt,
                                                            r.data, line, col);
                break;
              case Op::Mul:
                out = in.d ? culebra_runtime_num_inplace_mul_borrow(
                                 lt, l.data, rt, r.data, line, col)
                           : culebra_runtime_num_mul_borrow(lt, l.data, rt,
                                                            r.data, line, col);
                break;
              case Op::Div:
                out = in.d ? culebra_runtime_num_inplace_div_borrow(
                                 lt, l.data, rt, r.data, line, col)
                           : culebra_runtime_num_div_borrow(lt, l.data, rt,
                                                            r.data, line, col);
                break;
              case Op::Mod:
                out = culebra_runtime_num_mod_borrow(lt, l.data, rt, r.data,
                                                     line, col);
                break;
              default: __builtin_unreachable();
            }
          }
          regs[in.a] = out;
          ++pc;
          break;
        }
        case Op::Pow: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          auto [line, col] = chunk_pos_at(c, pc);
          auto lt = static_cast<int8_t>(l.tag);
          auto rt = static_cast<int8_t>(r.tag);
          regs[in.a] =
              in.d ? culebra_runtime_num_inplace_pow_borrow(lt, l.data, rt,
                                                            r.data, line, col)
                   : culebra_runtime_num_pow_borrow(lt, l.data, rt, r.data,
                                                    line, col);
          ++pc;
          break;
        }
        case Op::JumpIfSame: {
          const JitValue& cur = regs[in.a];
          if (cur.tag == TAG_TENSOR && cur.data == regs[in.c].data)
            pc = static_cast<size_t>(in.b);
          else
            ++pc;
          break;
        }
        case Op::MatMul: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          auto [line, col] = chunk_pos_at(c, pc);
          regs[in.a] = culebra_runtime_num_matmul_borrow(
              static_cast<int8_t>(l.tag), l.data, static_cast<int8_t>(r.tag),
              r.data, line, col);
          ++pc;
          break;
        }
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          if (l.tag != TAG_LONG || r.tag != TAG_LONG) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_type_error_typed(
                line, col, "Long",
                static_cast<int8_t>(l.tag != TAG_LONG ? l.tag : r.tag));
          }
          int64_t ld = l.data, rd = r.data, out;
          switch (in.op) {
            case Op::BitAnd: out = ld & rd; break;
            case Op::BitOr:  out = ld | rd; break;
            case Op::BitXor: out = ld ^ rd; break;
            // Count masked to the low 6 bits (the interp/JIT rule); `<<`
            // runs unsigned so value overflow wraps instead of being UB.
            case Op::Shl:
              out = static_cast<int64_t>(static_cast<uint64_t>(ld)
                                         << (rd & 63));
              break;
            default: out = ld >> (rd & 63); break;  // Shr (arithmetic)
          }
          regs[in.a] = JitValue{TAG_LONG, out};
          ++pc;
          break;
        }
        case Op::BitNot: {
          const JitValue& v = regs[in.b];
          if (v.tag != TAG_LONG) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_type_error_typed(line, col, "Long",
                                             static_cast<int8_t>(v.tag));
          }
          regs[in.a] = JitValue{TAG_LONG, ~v.data};
          ++pc;
          break;
        }
        case Op::Eq:
        case Op::Ne: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          bool eq;
          if (both_long(l, r)) {
            eq = l.data == r.data;
          } else {
            // The JIT publishes the operator position before the equality
            // helper (a user __eq__'s bool coercion throws positionless and
            // gets backfilled); the VM does the same through the same hook.
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_set_op_pos(line, col);
            eq = culebra_runtime_value_equal_borrow(
                static_cast<int8_t>(l.tag), l.data,
                static_cast<int8_t>(r.tag), r.data);
          }
          regs[in.a] = JitValue{TAG_BOOL, in.op == Op::Eq ? eq : !eq};
          ++pc;
          break;
        }
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
          const JitValue& l = regs[in.b];
          const JitValue& r = regs[in.c];
          bool res;
          if (both_long(l, r)) {
            switch (in.op) {
              case Op::Lt: res = l.data < r.data; break;
              case Op::Le: res = l.data <= r.data; break;
              case Op::Gt: res = l.data > r.data; break;
              default:     res = l.data >= r.data; break;
            }
          } else {
            auto [line, col] = chunk_pos_at(c, pc);
            auto lt = static_cast<int8_t>(l.tag);
            auto rt = static_cast<int8_t>(r.tag);
            switch (in.op) {
              case Op::Lt:
                res = culebra_runtime_value_less_borrow(lt, l.data, rt,
                                                        r.data, line, col);
                break;
              case Op::Le:
                res = culebra_runtime_value_leq_borrow(lt, l.data, rt, r.data,
                                                       line, col);
                break;
              case Op::Gt:
                res = culebra_runtime_value_greater_borrow(lt, l.data, rt,
                                                           r.data, line, col);
                break;
              default:
                res = culebra_runtime_value_geq_borrow(lt, l.data, rt, r.data,
                                                       line, col);
                break;
            }
          }
          regs[in.a] = JitValue{TAG_BOOL, res};
          ++pc;
          break;
        }
        case Op::ArrayNew:
          regs[in.a] = JitValue{
              TAG_ARRAY,
              reinterpret_cast<int64_t>(culebra_runtime_array_new())};
          ++pc;
          break;
        case Op::ArrayAppend:
          culebra_runtime_array_set_or_push(
              reinterpret_cast<JitArray*>(regs[in.a].data), in.b,
              static_cast<int8_t>(regs[in.c].tag), regs[in.c].data);
          regs[in.c] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::ArrayPush:
          culebra_runtime_array_push(
              reinterpret_cast<JitArray*>(regs[in.a].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          regs[in.b] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::ArrayExtend: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_array_extend(
              reinterpret_cast<JitArray*>(regs[in.a].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data, line,
              col);
          ++pc;
          break;
        }
        case Op::ArrayResize: {
          const JitValue& cnt = regs[in.b];
          auto [line, col] = chunk_pos_at(c, pc);
          if (cnt.tag != TAG_LONG)  // value_to_long's strict gate
            culebra_runtime_type_error_typed(line, col, "Long",
                                             static_cast<int8_t>(cnt.tag));
          int8_t dt = TAG_NIL;
          int64_t dd = 0;
          if (in.c >= 0) {
            dt = static_cast<int8_t>(regs[in.c].tag);
            dd = regs[in.c].data;
          }
          culebra_runtime_array_resize(
              reinterpret_cast<JitArray*>(regs[in.a].data), cnt.data, dt, dd,
              line, col);
          ++pc;
          break;
        }
        case Op::TupleNew:
          regs[in.a] = JitValue{
              TAG_TUPLE,
              reinterpret_cast<int64_t>(culebra_runtime_tuple_new())};
          ++pc;
          break;
        case Op::TuplePush:
          culebra_runtime_tuple_push(
              reinterpret_cast<JitArray*>(regs[in.a].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          regs[in.b] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::SetNew:
          regs[in.a] = JitValue{
              TAG_SET, reinterpret_cast<int64_t>(culebra_runtime_set_new())};
          ++pc;
          break;
        case Op::SetAdd:
          // On the unhashable throw the register still owns the +1 (the
          // handler ladder frees it); the SetOpPos published just before
          // anchors the positionless error at the literal.
          culebra_runtime_set_add(reinterpret_cast<JitSet*>(regs[in.a].data),
                                  static_cast<int8_t>(regs[in.b].tag),
                                  regs[in.b].data);
          regs[in.b] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::ObjectNew:
          regs[in.a] = JitValue{
              TAG_OBJECT,
              reinterpret_cast<int64_t>(culebra_runtime_object_new())};
          ++pc;
          break;
        case Op::ObjectNewShaped: {
          auto& spec = c.object_shape_specs[in.b];
          regs[in.a] = JitValue{
              TAG_OBJECT,
              reinterpret_cast<int64_t>(culebra_runtime_object_new_shaped(
                  &spec.shape, spec.keys.data(),
                  static_cast<int64_t>(spec.keys.size())))};
          ++pc;
          break;
        }
        case Op::ObjectSet: {
          // Unlike set_add, object_set consumes the value on EVERY exit,
          // including the positionless well-known-contract throw — nil the
          // register first so the handler ladder never double-releases.
          int8_t vt = static_cast<int8_t>(regs[in.b].tag);
          int64_t vd = regs[in.b].data;
          regs[in.b] = JitValue{TAG_NIL, 0};
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_object_set(
              reinterpret_cast<JitObject*>(regs[in.a].data),
              reinterpret_cast<const char*>(c.consts[in.c].data), in.d != 0,
              vt, vd, line, col, /*is_init=*/true);
          ++pc;
          break;
        }
        case Op::ObjectSetAny: {
          // object_set_any consumes both the key and the value on every
          // exit (including the positionless unhashable/well-known throw,
          // unlike set_add) — nil both registers first.
          int8_t kt = static_cast<int8_t>(regs[in.b].tag);
          int64_t kd = regs[in.b].data;
          int8_t vt = static_cast<int8_t>(regs[in.c].tag);
          int64_t vd = regs[in.c].data;
          regs[in.b] = JitValue{TAG_NIL, 0};
          regs[in.c] = JitValue{TAG_NIL, 0};
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_object_set_any(
              reinterpret_cast<JitObject*>(regs[in.a].data), kt, kd,
              in.d != 0, vt, vd, line, col, /*is_init=*/true);
          ++pc;
          break;
        }
        case Op::ObjectMerge: {
          // object_merge borrows the source (retains each copied entry) —
          // the register keeps its +1, freed later by the statement sweep.
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_object_merge(
              reinterpret_cast<JitObject*>(regs[in.a].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data, line,
              col);
          ++pc;
          break;
        }
        case Op::ModReg: {
          const JitValue& v = regs[in.a];
          culebra_runtime_module_register(
              reinterpret_cast<const char*>(c.consts[in.b].data),
              static_cast<int8_t>(v.tag), v.data);
          regs[in.a] = JitValue{TAG_NIL, 0};  // the table took the +1
          ++pc;
          break;
        }
        case Op::ModGet: {
          auto [line, col] = chunk_pos_at(c, pc);
          int8_t tag = TAG_NIL;
          int64_t data = 0;
          culebra_runtime_module_get(
              reinterpret_cast<const char*>(c.consts[in.b].data), &tag, &data,
              line, col);
          regs[in.a] = JitValue{tag, data};
          ++pc;
          break;
        }
        case Op::RangeNew: {
          bool hs = in.c & 1, he = in.c & 2;
          auto* o = culebra_runtime_make_range(
              hs ? 1 : 0, hs ? regs[in.b].data : 0, he ? 1 : 0,
              he ? regs[in.b + 1].data : 0, (in.c & 4) ? 1 : 0,
              regs[in.b + 2].data);
          regs[in.a] = JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(o)};
          ++pc;
          break;
        }
        case Op::ChkLong: {
          if (regs[in.a].tag != TAG_LONG) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_type_error_typed(
                line, col, "Long", static_cast<int8_t>(regs[in.a].tag));
          }
          ++pc;
          break;
        }
        case Op::NilChk: {
          if (regs[in.a].tag == TAG_NIL) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_throw_error("NilError", "`!!` applied to nil",
                                        line, col);
          }
          ++pc;
          break;
        }
        case Op::Index:
        case Op::IndexWr: {
          const JitValue& recv = regs[in.b];
          const JitValue& key = regs[in.c];
          auto [line, col] = chunk_pos_at(c, pc);
          bool wr = in.op == Op::IndexWr;
          // Range-valued key: slice, dispatched ahead of the receiver arms
          // (emit_index_step's order — an Object receiver slices too, into
          // culebra_runtime_slice's "expected Array"). Read-only on both
          // operands; the result is fresh +1.
          if (!wr && culebra_runtime_is_range(static_cast<int8_t>(key.tag),
                                              key.data)) {
            int8_t t;
            int64_t d;
            culebra_runtime_slice(static_cast<int8_t>(recv.tag), recv.data,
                                  key.data, &t, &d, line, col);
            regs[in.a] = JitValue{t, d};
            ++pc;
            break;
          }
          if (recv.tag == TAG_ARRAY || (!wr && recv.tag == TAG_TUPLE)) {
            if (key.tag != TAG_LONG)
              culebra_runtime_type_error_typed(
                  line, col, "Long", static_cast<int8_t>(key.tag));
            // Write-context read: array_set's bounds rule (guard_write_index).
            if (wr && key.data < 0)
              culebra_runtime_throw_error("IndexError", "index out of range",
                                          line, col);
            int8_t t;
            int64_t d;
            culebra_runtime_array_get(reinterpret_cast<JitArray*>(recv.data),
                                      key.data, &t, &d, line, col);
            culebra_runtime_value_retain(t, d);  // array_get borrows the slot
            regs[in.a] = JitValue{t, d};
          } else if (recv.tag == TAG_OBJECT) {
            // object_get_any consumes a non-String key on every path;
            // retain so the register keeps its owner +1. The result is +1.
            culebra_runtime_value_retain(static_cast<int8_t>(key.tag),
                                         key.data);
            int8_t t;
            int64_t d;
            culebra_runtime_object_get_any(
                reinterpret_cast<JitObject*>(recv.data),
                static_cast<int8_t>(key.tag), key.data, &t, &d, line, col,
                /*own_receiver=*/false);
            regs[in.a] = JitValue{t, d};
          } else {
            culebra_runtime_type_error_typed(
                line, col, "Array", static_cast<int8_t>(recv.tag));
          }
          ++pc;
          break;
        }
        case Op::PropVal:
        case Op::PropRaw: {
          const JitValue& recv = regs[in.b];
          const char* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto [line, col] = chunk_pos_at(c, pc);
          // emit_property_get without its per-site inline cache: the executor
          // always takes the cold funnel, and object_get_ic only WRITES the
          // cache (the fast path that reads it lives in emitted code), so one
          // scratch entry per execution is all it needs.
          // emit_property_get's fn_mode branch, mirrored: a Function receiver
          // answers the three signature names from its own metadata, and the
          // result is a fresh value rather than a borrowed object slot.
          if (recv.tag == TAG_FUNC && fn_introspection_name(key)) {
            regs[in.a] = culebra_runtime_fn_introspect_get(
                reinterpret_cast<JitClosure*>(recv.data), key);
            ++pc;
            break;
          }
          JitPropIC ic{};
          JitValue view = culebra_runtime_prop_get(
              static_cast<int8_t>(recv.tag), recv.data, key, &ic, line, col,
              /*own_receiver=*/false);
          if (in.op == Op::PropRaw) {
            // The register owns what it holds, so the borrowed view becomes a
            // +1 here; the statement sweep is its releaser.
            culebra_runtime_value_retain(static_cast<int8_t>(view.tag),
                                         view.data);
            regs[in.a] = view;
          } else {
            regs[in.a] = culebra_runtime_bind_method_value(
                static_cast<int8_t>(recv.tag), recv.data,
                static_cast<int8_t>(view.tag), view.data, key);
          }
          ++pc;
          break;
        }
        case Op::HasProp: {
          const char* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          regs[in.a] = JitValue{
              TAG_BOOL, has_prop_apply(in.d, regs[in.b], key) ? 1 : 0};
          ++pc;
          break;
        }
        case Op::Drop: {
          const JitValue& recv = regs[in.b];
          culebra_runtime_explicit_drop(static_cast<int8_t>(recv.tag),
                                        recv.data);
          regs[in.a] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        }
        case Op::ClsParamsChk: {
          const JitValue& recv = regs[in.b];
          bool use_auto = false;
          if (recv.tag == TAG_OBJECT) {
            auto* obj = reinterpret_cast<JitObject*>(recv.data);
            use_auto =
                culebra_runtime_object_has(obj, "class") &&
                !culebra_runtime_object_has_or_trait_default(obj, "parameters");
          }
          regs[in.a] = JitValue{TAG_BOOL, use_auto ? 1 : 0};
          ++pc;
          break;
        }
        case Op::ClsParamsWalk:
          regs[in.a] = culebra_runtime_class_parameters_walk(
              reinterpret_cast<JitObject*>(regs[in.b].data));
          ++pc;
          break;
        case Op::MethGate: {
          const JitValue& recv = regs[in.b];
          const char* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto [line, col] = chunk_pos_at(c, pc);
          regs[in.a] = JitValue{TAG_NO_SELF, kBMethGateBuiltin};
          if (recv.tag == TAG_OBJECT) {
            // checkBB: the object's own read decides. A namespace raises its
            // AttributeError from inside this read (a dict-builtin name is
            // exempt there, which is why `Math.size()` reaches the built-in).
            JitPropIC ic{};
            JitValue view = culebra_runtime_prop_get(
                TAG_OBJECT, recv.data, key, &ic, line, col,
                /*own_receiver=*/false);
            // An own slot wins even holding a non-Function: `{size: 5}.size()`
            // is "expected Function, got Long", not the dict builtin. A Shared
            // view carries no dict builtins at all — every name it lacks is a
            // frozen-tree read, so it takes the user arm too.
            if (view.tag == TAG_FUNC ||
                culebra_runtime_object_has_own_field(TAG_OBJECT, recv.data,
                                                     key) ||
                (culebra::is_object_builtin_method_name(key) &&
                 culebra_runtime_is_shared_val(recv.data))) {
              // The register owns what it holds; the sweep is its releaser.
              culebra_runtime_value_retain(static_cast<int8_t>(view.tag),
                                           view.data);
              regs[in.a] = view;
              ++pc;
              break;
            }
          }
          // d indexes the spec row (gate fields are identical across an
          // id's arities, so this call's own row answers).
          const BMethSpec& gate = bmeth_specs()[in.d];
          bool ok = bmeth_receiver_ok(gate.recv, static_cast<int8_t>(recv.tag));
          // An iterator-protocol name resolves on an Object only when the
          // object carries the protocol; a plain dict merely lacks it.
          if (ok && gate.obj_iter_shaped && recv.tag == TAG_OBJECT)
            ok = culebra_runtime_object_has(
                reinterpret_cast<JitObject*>(recv.data), "next");
          if (!ok) {
            if (bmeth_scalar_tag(static_cast<int8_t>(recv.tag)))
              bmeth_scalar_receiver_error(static_cast<int8_t>(recv.tag), line,
                                          col);
            regs[in.a] = JitValue{TAG_NO_SELF, kBMethGateMiss};
          } else if (bmeth_publishes_call_pos(gate.id)) {
            culebra_runtime_set_op_pos(line, col);
          }
          ++pc;
          break;
        }
        case Op::CallRecv:
          regs[in.a] = culebra_runtime_call_receiver(
              static_cast<int8_t>(regs[in.a].tag), regs[in.a].data,
              reinterpret_cast<const char*>(c.consts[in.c].data));
          ++pc;
          break;
        case Op::CbType: {
          if (regs[in.b].tag == TAG_NO_SELF &&
              regs[in.b].data == kBMethGateBuiltin) {
            // The helper reports at the callback's own argument, which it
            // reads from the site BMeth publishes later — publish it here,
            // where this check is the first reader.
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_set_callback_arg_site(line, col);
            culebra_runtime_check_callback_type(
                static_cast<int8_t>(regs[in.a].tag), regs[in.a].data,
                reinterpret_cast<const char*>(c.consts[in.c].data));
          }
          ++pc;
          break;
        }
        case Op::ArityChk: {
          const JitValue& gate = regs[in.a];
          if (gate.tag == TAG_NO_SELF && gate.data == kBMethGateMiss) {
            const JitValue& recv = regs[in.a + 1];
            bool shaped =
                recv.tag == TAG_OBJECT &&
                culebra_runtime_object_has(
                    reinterpret_cast<JitObject*>(recv.data), "next");
            auto msg = bmeth_rival_arity_message(
                bmeth_specs()[in.c], static_cast<int8_t>(recv.tag), shaped);
            if (!msg.empty()) {
              auto [line, col] = chunk_pos_at(c, pc);
              culebra_runtime_throw_error("ArityError", msg.c_str(), line, col);
            }
          }
          ++pc;
          break;
        }
        case Op::ChkParam: {
          // A user method shadowing the built-in binds by its own signature,
          // and a miss binds nothing at all. regs[b+1] is the receiver, which
          // decides whether a per-arm check applies here (BMeth's own layout).
          const BMethSpec& spec = bmeth_specs()[in.c];
          if (regs[in.b].tag == TAG_NO_SELF &&
              regs[in.b].data == kBMethGateBuiltin &&
              bmeth_param_applies(spec, in.d,
                                  static_cast<int8_t>(regs[in.b + 1].tag)) &&
              !bmeth_param_ok(spec.params[in.d],
                              static_cast<int8_t>(regs[in.a].tag))) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_throw_error(
                "TypeError", bmeth_param_message(spec, in.d).c_str(), line,
                col);
          }
          ++pc;
          break;
        }
        case Op::BMeth: {
          const JitValue& gate = regs[in.b];
          auto [line, col] = chunk_pos_at(c, pc);
          if (gate.tag == TAG_NO_SELF && gate.data == kBMethGateMiss)
            bmeth_miss_error(line, col);  // the arguments have run by now
          if (gate.tag != TAG_NO_SELF) {
            // The shadowing user method: CallM's hand-off, one slot over.
            publish_call_site(c, pc, line, col);
            if (gate.tag != TAG_FUNC)
              culebra_runtime_type_error_typed(
                  line, col, "Function", static_cast<int8_t>(gate.tag));
            JitValue r;
            try {
              // Rooted: gate (regs[b]), receiver (regs[b+1]) and args
              // (regs[b+2..], nil'd only after the return) stay in this
              // frame's registers for the call's duration (jit_value.h).
              r = _jit_invoke_rooted(reinterpret_cast<JitClosure*>(gate.data),
                                     regs[in.b + 1], in.d,
                                     in.d ? &regs[in.b + 2] : nullptr);
            } catch (...) {
              for (int32_t i = 1; i <= in.d + 1; ++i)
                regs[in.b + i] = JitValue{TAG_NIL, 0};
              throw;
            }
            for (int32_t i = 1; i <= in.d + 1; ++i)
              regs[in.b + i] = JitValue{TAG_NIL, 0};
            regs[in.a] = r;
            ++pc;
            break;
          }
          // The built-in borrows the receiver and the arguments — every one of
          // them stays register-owned, so the statement sweep is their sole
          // releaser and a throwing helper strands nothing. The two that take
          // the arguments' `+1` are the exception: hand the values over and
          // nil the run first, so the helper is their only owner from the
          // call on (`array_insert` releases on its own throw edge).
          auto id = static_cast<BMeth>(in.c);
          // A non-Function callback reports at its own argument: publish the
          // site the runtime helper's check reads.
          if (int8_t cb = bmeth_callback_arg(id, static_cast<int8_t>(in.d));
              cb >= 0) {
            if (const auto* ap = chunk_argpos_at(c, pc);
                ap && cb < static_cast<int8_t>(ap->size()))
              culebra_runtime_set_callback_arg_site((*ap)[cb] >> 32,
                                                    (*ap)[cb] & 0xffffffff);
          }
          bool consumes = bmeth_consumes_args(id);
          JitValue argv[2] = {JitValue{TAG_NIL, 0}, JitValue{TAG_NIL, 0}};
          for (int32_t k = 0; consumes && k < in.d; ++k) {
            argv[k] = regs[in.b + 2 + k];
            regs[in.b + 2 + k] = JitValue{TAG_NIL, 0};
          }
          regs[in.a] = bmeth_apply(id, regs[in.b + 1],
                                   consumes ? argv : &regs[in.b + 2], line,
                                   col);
          ++pc;
          break;
        }
        case Op::BareMethChk: {
          const JitValue& recv = regs[in.b];
          const char* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          if (regs[in.a].tag == TAG_NIL &&
              !culebra_runtime_object_has_own_field(
                  static_cast<int8_t>(recv.tag), recv.data, key)) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_bare_builtin_reject(static_cast<int8_t>(recv.tag),
                                                recv.data, key, line, col);
          }
          ++pc;
          break;
        }
        case Op::IndexCo: {
          const JitValue& recv = regs[in.b];
          const JitValue& key = regs[in.c];
          auto [line, col] = chunk_pos_at(c, pc);
          if (recv.tag == TAG_ARRAY) {
            if (key.tag != TAG_LONG)
              culebra_runtime_type_error_typed(
                  line, col, "Long", static_cast<int8_t>(key.tag));
            if (key.data < 0)
              culebra_runtime_throw_error("IndexError", "index out of range",
                                          line, col);
            int8_t t;
            int64_t d;
            culebra_runtime_array_get(reinterpret_cast<JitArray*>(recv.data),
                                      key.data, &t, &d, line, col);
            culebra_runtime_value_retain(t, d);
            regs[in.a] = JitValue{t, d};
          } else if (recv.tag == TAG_OBJECT) {
            // The nc receiver-kind rejects fire ahead of the read, the
            // JIT's nc dispatch — none of the three kinds is constructible
            // in the slice, but the mirror keeps the arm 1:1.
            switch (culebra_runtime_nc_receiver_kind(recv.data)) {
              case 1:
                culebra_runtime_throw_error(
                    "TypeError",
                    "`?" "?=` is not supported on a FixedArray element",
                    line, col);
                break;
              case 2:
                culebra_runtime_throw_error(
                    "ImmutableError", "Shared values are immutable", line,
                    col);
                break;
              case 3:
                culebra_runtime_throw_error(
                    "TypeError",
                    "cannot assign to a SharedBuffer element directly; "
                    "set fields via buf[i].field = value",
                    line, col);
                break;
              default:
                break;
            }
            culebra_runtime_value_retain(static_cast<int8_t>(key.tag),
                                         key.data);
            int8_t t;
            int64_t d;
            // A plain-dict miss leaves nil (found=false) — exactly the
            // write test the JumpIfNotNil that follows performs.
            culebra_runtime_object_get_for_coalesce(
                reinterpret_cast<JitObject*>(recv.data),
                static_cast<int8_t>(key.tag), key.data, &t, &d, line, col);
            regs[in.a] = JitValue{t, d};
          } else {
            culebra_runtime_type_error_typed(
                line, col, "Array", static_cast<int8_t>(recv.tag));
          }
          ++pc;
          break;
        }
        case Op::IndexSet: {
          const JitValue& recv = regs[in.a];
          const JitValue& key = regs[in.b];
          const JitValue& val = regs[in.c];
          auto [line, col] = chunk_pos_at(c, pc);
          if (recv.tag == TAG_ARRAY) {
            if (key.tag != TAG_LONG)
              culebra_runtime_type_error_typed(
                  line, col, "Long", static_cast<int8_t>(key.tag));
            // The store consumes a +1; the value register keeps its own
            // (the assignment expression reads it afterwards). On the OOB
            // throw the minted +1 strands to the GC backstop, like the
            // JIT's rval.
            culebra_runtime_value_retain(static_cast<int8_t>(val.tag),
                                         val.data);
            culebra_runtime_array_set(reinterpret_cast<JitArray*>(recv.data),
                                      key.data, static_cast<int8_t>(val.tag),
                                      val.data, line, col);
          } else if (recv.tag == TAG_OBJECT) {
            // object_set_any consumes the key and the value on every path.
            culebra_runtime_value_retain(static_cast<int8_t>(key.tag),
                                         key.data);
            culebra_runtime_value_retain(static_cast<int8_t>(val.tag),
                                         val.data);
            culebra_runtime_object_set_any(
                reinterpret_cast<JitObject*>(recv.data),
                static_cast<int8_t>(key.tag), key.data, /*mut=*/true,
                static_cast<int8_t>(val.tag), val.data, line, col,
                /*is_init=*/false);
          } else {
            culebra_runtime_type_error_typed(
                line, col, "Array", static_cast<int8_t>(recv.tag));
          }
          ++pc;
          break;
        }
        case Op::NsWrChk: {
          const JitValue& recv = regs[in.a];
          if (recv.tag == TAG_OBJECT) {
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_check_namespace_write(
                reinterpret_cast<JitObject*>(recv.data),
                reinterpret_cast<const char*>(c.consts[in.c].data), line,
                col);
          }
          ++pc;
          break;
        }
        case Op::PropSet: {
          const JitValue& recv = regs[in.a];
          const JitValue& val = regs[in.b];
          auto [line, col] = chunk_pos_at(c, pc);
          if (recv.tag != TAG_OBJECT)
            culebra_runtime_type_error_typed(
                line, col, "Object, Array, or Tensor",
                static_cast<int8_t>(recv.tag));
          // The retain feeds object_set's consuming store; the register
          // keeps its +1 so the assignment expression reads it afterwards.
          culebra_runtime_value_retain(static_cast<int8_t>(val.tag), val.data);
          int64_t dotpk = c.consts[in.d].data;
          culebra_runtime_object_set_uncached(
              reinterpret_cast<JitObject*>(recv.data),
              reinterpret_cast<const char*>(c.consts[in.c].data),
              /*mut=*/true, static_cast<int8_t>(val.tag), val.data, line, col,
              /*is_init=*/false, dotpk >> 32, dotpk & 0xffffffff);
          ++pc;
          break;
        }
        case Op::PropWr: {
          const JitValue& recv = regs[in.b];
          const auto* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto [line, col] = chunk_pos_at(c, pc);
          if (recv.tag != TAG_OBJECT)
            culebra_runtime_type_error_typed(
                line, col, "Object, Array, or Tensor",
                static_cast<int8_t>(recv.tag));
          // The Shared-view reject runs ahead of the existence check (the
          // interp's is_shared_val_view-before-find_prop order), at the
          // statement; the miss anchors at the DOT node from consts[d].
          if (culebra_runtime_nc_receiver_kind(recv.data) == 2)
            culebra_runtime_throw_error("ImmutableError",
                                        "Shared values are immutable", line,
                                        col);
          auto* obj = reinterpret_cast<JitObject*>(recv.data);
          if (!culebra_runtime_object_has(obj, key)) {
            int64_t pk = c.consts[in.d].data;
            culebra_runtime_compound_missing_property(pk >> 32,
                                                      pk & 0xffffffff);
          }
          JitPropIC ic{};
          JitValue view = culebra_runtime_prop_get(TAG_OBJECT, recv.data, key,
                                                   &ic, line, col,
                                                   /*own_receiver=*/false);
          culebra_runtime_value_retain(static_cast<int8_t>(view.tag),
                                       view.data);
          regs[in.a] = view;
          ++pc;
          break;
        }
        case Op::PropCo: {
          const JitValue& recv = regs[in.b];
          const auto* key = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto [line, col] = chunk_pos_at(c, pc);
          if (recv.tag != TAG_OBJECT)
            culebra_runtime_type_error_typed(
                line, col, "Object, Array, or Tensor",
                static_cast<int8_t>(recv.tag));
          // The nc receiver-kind rejects fire ahead of the read (the JIT's
          // nc DOT dispatch — only the Shared-view kind is constructible in
          // the slice, but the mirror keeps the arm 1:1).
          switch (culebra_runtime_nc_receiver_kind(recv.data)) {
            case 2:
              culebra_runtime_throw_error("ImmutableError",
                                          "Shared values are immutable", line,
                                          col);
              break;
            case 4:
              culebra_runtime_throw_error(
                  "TypeError", "`?" "?=` is not supported on a packed field",
                  line, col);
              break;
            default:
              break;
          }
          // A plain read: a dict miss is nil (the JumpIfNotNil write test);
          // a namespace's unknown member raises AttributeError at the DOT
          // node (consts[d]) — before the RHS ever runs.
          int64_t pk = c.consts[in.d].data;
          JitPropIC ic{};
          JitValue view = culebra_runtime_prop_get(
              TAG_OBJECT, recv.data, key, &ic, pk >> 32, pk & 0xffffffff,
              /*own_receiver=*/false);
          culebra_runtime_value_retain(static_cast<int8_t>(view.tag),
                                       view.data);
          regs[in.a] = view;
          ++pc;
          break;
        }
        case Op::SeqChk: {
          const JitValue& v = regs[in.a];
          bool ok = v.tag == TAG_ARRAY || v.tag == TAG_TUPLE;
          if (ok) {
            int64_t n = culebra_runtime_array_size(
                reinterpret_cast<JitArray*>(v.data));
            ok = in.d ? n >= in.c : n == in.c;
          }
          if (ok) ++pc;
          else pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::SeqGet: {
          auto* arr = reinterpret_cast<JitArray*>(regs[in.b].data);
          int64_t at = in.c >= 0 ? in.c : culebra_runtime_array_size(arr) + in.c;
          auto [line, col] = chunk_pos_at(c, pc);
          int8_t t;
          int64_t d;
          culebra_runtime_array_get(arr, at, &t, &d, line, col);
          culebra_runtime_value_retain(t, d);  // array_get borrows the slot
          regs[in.a] = JitValue{t, d};
          ++pc;
          break;
        }
        case Op::SeqRest: {
          auto* arr = reinterpret_cast<JitArray*>(regs[in.b].data);
          auto* out = culebra_runtime_array_slice(
              arr, in.c, culebra_runtime_array_size(arr) - in.d);
          regs[in.a] = JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)};
          ++pc;
          break;
        }
        case Op::ObjGet: {
          const JitValue& recv = regs[in.c];
          const auto* key = reinterpret_cast<const char*>(c.consts[in.d].data);
          if (recv.tag != TAG_OBJECT ||
              !culebra_runtime_object_has(
                  reinterpret_cast<JitObject*>(recv.data), key)) {
            pc = static_cast<size_t>(in.b);
            break;
          }
          int8_t t;
          int64_t d;
          culebra_runtime_object_get(reinterpret_cast<JitObject*>(recv.data),
                                     key, &t, &d);
          culebra_runtime_value_retain(t, d);  // object_get borrows the slot
          regs[in.a] = JitValue{t, d};
          ++pc;
          break;
        }
        case Op::DestrErr: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra::throw_destructure_mismatch_at(line, col);
          break;
        }
        case Op::Jump:
          pc = static_cast<size_t>(in.a);
          break;
        case Op::JumpIfFalse:
          if (!to_bool(regs[in.a], pc)) pc = static_cast<size_t>(in.b);
          else ++pc;
          break;
        case Op::JumpIfTrue:
          if (to_bool(regs[in.a], pc)) pc = static_cast<size_t>(in.b);
          else ++pc;
          break;
        case Op::JumpIfNotNil:
          if (regs[in.a].tag != TAG_NIL) pc = static_cast<size_t>(in.b);
          else ++pc;
          break;
        case Op::JumpIfNil:
          if (regs[in.a].tag == TAG_NIL) pc = static_cast<size_t>(in.b);
          else ++pc;
          break;
        case Op::JumpIfTag:
          if (regs[in.a].tag == static_cast<int64_t>(in.c))
            pc = static_cast<size_t>(in.b);
          else ++pc;
          break;
        case Op::MakeClosure: {
          const Chunk& f = p.chunks[in.b];
          auto n = f.capture_src_slots.size();
          auto* mc = culebra_runtime_closure_new(
              f.is_getter ? reinterpret_cast<void*>(&getter_trampoline)
                          : reinterpret_cast<void*>(&trampoline),
              1 + n, static_cast<size_t>(f.arity));
          // The descriptor rides in a cell of this closure's own, like every
          // other capture: cells are refcounted non-atomically and freed into
          // the slab of the Runtime that allocated them, so one shared per
          // chunk would be retained and released by every isolate at once.
          mc->captures[0] = culebra_runtime_cell_new(
              TAG_LONG, reinterpret_cast<int64_t>(&p.descs[in.b]));
          // Fill the captures from the creating frame's cell slots, each
          // retained — emit_closure_build's loop.
          for (size_t i = 0; i < n; ++i) {
            auto* cell = reinterpret_cast<JitCell*>(
                regs[f.capture_src_slots[i]].data);
            culebra_runtime_cell_retain(cell);
            mc->captures[1 + i] = cell;
          }
          regs[in.a] = JitValue{TAG_FUNC, reinterpret_cast<int64_t>(mc)};
          ++pc;
          break;
        }
        case Op::Call: {
          auto tgt = chunk_call_target_at(c, pc);
          // A borrowed callee: the register holds the CELL and the value
          // inside it is what runs, with nothing minted for the call — the
          // cell's own reference is what keeps it alive
          // (Chunk::CallTarget::callee_in_cell).
          const JitValue& callee =
              tgt.callee_in_cell
                  ? reinterpret_cast<JitCell*>(regs[in.b].data)->value
                  : regs[in.b];
          BorrowWitness witness{tgt.callee_in_cell ? &callee : nullptr};
          auto [line, col] = chunk_pos_at(c, pc);
          publish_call_site(c, pc, line, col);
          JitValue target = callee;
          JitValue self{TAG_NO_SELF, 0};
          if (JitValue entered;
              tgt.chunk >= 0 && resolved_entry(p, tgt, callee, entered)) {
            regs[in.a] = run_resolved(p, tgt.chunk, entered, self, &regs[in.c],
                                      in.d, in.d, line, col);
            ++pc;
            break;
          }
          if (target.tag != TAG_FUNC) {
            // The two cold-path probes, in the JIT's order: a callable
            // instance (`obj(args)` with a `__call__` method, own or a
            // trait default) becomes a method call on itself, and a class
            // object's `new` builds an instance. Both are borrowed reads —
            // the register keeps its own +1 — so the receiver a `__call__`
            // frame consumes is minted here.
            target = culebra_runtime_class_call_method(
                static_cast<int8_t>(callee.tag), callee.data);
            if (target.tag == TAG_FUNC) {
              culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                           callee.data);
              self = callee;
            } else {
              // `C(args)`: the class object is the constructor's receiver
              // — the instance keeps a +1 on it (JitObject::cls).
              target = culebra_runtime_class_new_method(
                  static_cast<int8_t>(callee.tag), callee.data);
              if (target.tag == TAG_FUNC) {
                culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                             callee.data);
                self = callee;
              }
            }
          }
          if (target.tag != TAG_FUNC) {
            culebra_runtime_type_error_typed(
                line, col, "Function", static_cast<int8_t>(callee.tag));
          }
          // A keyword-only parameter cannot be filled positionally, and the
          // callee is only known here — the JIT's own guard, at the same
          // point in its call.
          culebra_runtime_check_pos_count_cls(
              reinterpret_cast<JitClosure*>(target.data), in.d, line, col);
          JitValue r;
          try {
            // Rooted: callee (regs[b]), receiver (== callee on the __call__
            // path) and args (regs[c..], nil'd only after the return) all
            // stay in this frame's registers for the call's duration, so a
            // safepoint collect beneath it sees them (jit_value.h).
            r = _jit_invoke_rooted(reinterpret_cast<JitClosure*>(target.data),
                                   self, in.d, in.d ? &regs[in.c] : nullptr);
          } catch (...) {
            // The callee consumed each arg's +1 on its own unwind path too
            // (the JitFn ABI); nil the slab slots so an enclosing region's
            // release ladder cannot double-release them.
            for (int32_t i = 0; i < in.d; ++i)
              regs[in.c + i] = JitValue{TAG_NIL, 0};
            throw;
          }
          for (int32_t i = 0; i < in.d; ++i)
            regs[in.c + i] = JitValue{TAG_NIL, 0};  // callee took ownership
          regs[in.a] = r;
          ++pc;
          break;
        }
        case Op::CallM: {
          const JitValue& callee = regs[in.b];
          auto [line, col] = chunk_pos_at(c, pc);
          publish_call_site(c, pc, line, col);
          JitValue target = callee;
          JitValue self = regs[in.c];
          auto tgt = chunk_call_target_at(c, pc);
          if (JitValue entered;
              tgt.chunk >= 0 && resolved_entry(p, tgt, callee, entered)) {
            regs[in.a] = run_resolved(p, tgt.chunk, entered, self, &regs[in.c],
                                      in.d + 1, in.d, line, col);
            ++pc;
            break;
          }
          if (target.tag != TAG_FUNC) {
            // A method value that is itself a callable instance
            // (`{m: Adder.new(1)}.m(41)`): it becomes both the callee and
            // the receiver, and the original receiver — which nothing takes
            // now — is released here, as the JIT's cold path does.
            target = culebra_runtime_class_call_method(
                static_cast<int8_t>(callee.tag), callee.data);
            if (target.tag == TAG_FUNC) {
              culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                           callee.data);
              culebra_runtime_value_release(static_cast<int8_t>(self.tag),
                                            self.data);
              self = callee;
            } else {
              // A class object reached as a member (`Canvas.Font(bytes)`):
              // its `new` builds the instance, with the class object as its
              // receiver in place of the one the call started with — the
              // plain-call and kwargs arms probe in this same order.
              target = culebra_runtime_class_new_method(
                  static_cast<int8_t>(callee.tag), callee.data);
              if (target.tag == TAG_FUNC) {
                culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                             callee.data);
                culebra_runtime_value_release(static_cast<int8_t>(self.tag),
                                              self.data);
                self = callee;
              }
            }
          }
          if (target.tag != TAG_FUNC) {
            // Where a missing method lands: the property read gave nil, so
            // this is the interp's "expected Function, got Nil". The receiver
            // and args stay register-owned — nothing has been handed over
            // yet, so the enclosing ladder is still their releaser.
            culebra_runtime_type_error_typed(
                line, col, "Function", static_cast<int8_t>(callee.tag));
          }
          // A keyword-only parameter cannot be filled positionally, and the
          // callee is only known here — the JIT's own guard, at the same
          // point in its call.
          culebra_runtime_check_pos_count_cls(
              reinterpret_cast<JitClosure*>(target.data), in.d, line, col);
          // The run is receiver-then-args; the callee consumes all of it.
          // culebra_runtime_call_receiver is not mirrored: it only rewrites a
          // lowered state object's promoted body local into "no receiver",
          // and those protos come from the generator / effects transforms the
          // slice rejects outright.
          JitValue r;
          try {
            // Rooted: callee (regs[b]), receiver (regs[c]) and args
            // (regs[c+1..], nil'd only after the return) stay in this
            // frame's registers for the call's duration (jit_value.h).
            r = _jit_invoke_rooted(reinterpret_cast<JitClosure*>(target.data),
                                   self, in.d,
                                   in.d ? &regs[in.c + 1] : nullptr);
          } catch (...) {
            for (int32_t i = 0; i <= in.d; ++i)
              regs[in.c + i] = JitValue{TAG_NIL, 0};
            throw;
          }
          for (int32_t i = 0; i <= in.d; ++i)
            regs[in.c + i] = JitValue{TAG_NIL, 0};
          regs[in.a] = r;
          ++pc;
          break;
        }
        case Op::CallKw: {
          const Chunk::KwCall& kc = c.kwcalls[in.d];
          auto [line, col] = chunk_pos_at(c, pc);
          // The positionals bind by index and report at their own expression;
          // a keyword value's parameter has no entry, so it falls back to the
          // call site — the interp's own split (record_call_argpos).
          publish_call_site(c, pc, line, col);
          int32_t off = kc.has_receiver ? 1 : 0;
          int32_t total = off + kc.n_pos + kc.n_kw + kc.n_splat;
          JitValue callee = regs[in.b];
          JitValue self = kc.has_receiver ? regs[in.c]
                                          : JitValue{TAG_NO_SELF, 0};
          if (callee.tag != TAG_FUNC) {
            // The same two cold probes the plain call makes, in the same
            // order: a callable instance becomes both callee and receiver
            // (releasing the receiver this call started with, which nothing
            // else takes now), and a class object hands over its `new`.
            JitValue m = culebra_runtime_class_call_method(
                static_cast<int8_t>(callee.tag), callee.data);
            if (m.tag == TAG_FUNC) {
              culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                           callee.data);
              if (kc.has_receiver) {
                culebra_runtime_value_release(static_cast<int8_t>(self.tag),
                                              self.data);
                regs[in.c] = JitValue{TAG_NIL, 0};
              }
              self = callee;
            } else {
              m = culebra_runtime_class_new_method(
                  static_cast<int8_t>(callee.tag), callee.data);
              if (m.tag == TAG_FUNC) {
                culebra_runtime_value_retain(static_cast<int8_t>(callee.tag),
                                             callee.data);
                if (kc.has_receiver) {
                  culebra_runtime_value_release(
                      static_cast<int8_t>(self.tag), self.data);
                  regs[in.c] = JitValue{TAG_NIL, 0};
                }
                self = callee;
              }
            }
            if (m.tag != TAG_FUNC)
              culebra_runtime_type_error_typed(
                  line, col, "Function", static_cast<int8_t>(callee.tag));
            callee = m;
          }
          // The resolver consumes the receiver and every value it is handed,
          // so hand them over and nil the run first — it is their only owner
          // from the call on, throw paths included.
          std::vector<JitValue> vals(regs + in.c, regs + in.c + total);
          for (int32_t i = 0; i < total; ++i)
            regs[in.c + i] = JitValue{TAG_NIL, 0};
          JitValue* pos_p = vals.data() + off;
          regs[in.a] = culebra_runtime_call_with_kwargs(
              reinterpret_cast<JitClosure*>(callee.data),
              static_cast<int8_t>(self.tag), self.data, kc.n_pos, pos_p,
              kc.n_kw, kc.kw_keys.data(), pos_p + kc.n_pos, kc.n_splat,
              pos_p + kc.n_pos + kc.n_kw, line, col);
          ++pc;
          break;
        }
        case Op::RaiseErr: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_throw_error(
              reinterpret_cast<const char*>(c.consts[in.b].data),
              reinterpret_cast<const char*>(c.consts[in.c].data), line, col);
          break;  // unreachable — the helper always throws
        }
        case Op::Ret: {
          JitValue rv = regs[in.a];
          if (c.counts_frame) culebra_runtime_recursion_leave();
          return rv;
        }
        case Op::CellNew: {
          culebra_runtime_cell_release(
              reinterpret_cast<JitCell*>(regs[in.a].data));
          auto* cell = culebra_runtime_cell_new(
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          regs[in.b] = JitValue{TAG_NIL, 0};
          regs[in.a] = JitValue{TAG_LONG, reinterpret_cast<int64_t>(cell)};
          ++pc;
          break;
        }
        case Op::CellGet: {
          auto* cell = reinterpret_cast<JitCell*>(regs[in.b].data);
          regs[in.a] = cell->value;
          culebra_runtime_value_retain(static_cast<int8_t>(regs[in.a].tag),
                                       regs[in.a].data);
          ++pc;
          break;
        }
        case Op::CellSet: {
          auto* cell = reinterpret_cast<JitCell*>(regs[in.a].data);
          JitValue old = cell->value;
          cell->value = regs[in.b];
          regs[in.b] = JitValue{TAG_NIL, 0};
          culebra_runtime_value_release(static_cast<int8_t>(old.tag),
                                        old.data);
          ++pc;
          break;
        }
        case Op::CellRelease:
          culebra_runtime_cell_release(
              reinterpret_cast<JitCell*>(regs[in.a].data));
          regs[in.a] = JitValue{TAG_NIL, 0};
          ++pc;
          break;
        case Op::BindCapture:
          // captures[0] is the descriptor; user captures follow. Borrowed:
          // no retain, and the slot's frame-teardown Release is a no-op.
          regs[in.a] = JitValue{
              TAG_LONG, reinterpret_cast<int64_t>(cls->captures[1 + in.b])};
          ++pc;
          break;
        case Op::ImmutErr: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_immutable_assign(
              reinterpret_cast<const char*>(c.consts[in.a].data), line, col);
          break;  // unreachable — the helper always throws
        }
        case Op::UnboundErr:
          if ((in.c ? reinterpret_cast<JitCell*>(regs[in.a].data)->value
                    : regs[in.a])
                  .tag == TAG_NO_SELF) {
            auto [line, col] = chunk_pos_at(c, pc);
            auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
            culebra_runtime_throw_error(
                "NameError",
                culebra::format("undefined variable '{}'", nm).c_str(),
                line, col);
          }
          ++pc;
          break;
        case Op::MultifnReg: {
          const Chunk& f = p.chunks[in.d];
          // The overload's signature straight off the callee chunk: declared
          // types (null where untyped) drive type dispatch, `required` is the
          // arity floor a defaulted tail opens up, and the stable param-name
          // storage covers kwargs (unused in the slice).
          // Only the REGULAR parameters take part in dispatch: a keyword-only
          // slot and the `**rest` catch-all are never filled positionally, so
          // an overload's positional arity stops where they begin.
          auto regular_end = static_cast<size_t>(
              f.first_kw_only_idx >= 0     ? f.first_kw_only_idx
              : f.kwargs_rest_idx >= 0     ? f.kwargs_rest_idx
                                           : f.arity);
          std::vector<const char*> names;
          std::vector<const char*> types;
          names.reserve(regular_end);
          types.reserve(regular_end);
          int64_t min_arity = 0;
          for (size_t k = 0; k < regular_end; ++k) {
            names.push_back(f.param_names[k].c_str());
            types.push_back(f.param_types[k].empty() ? nullptr
                                                     : f.param_types[k].c_str());
            if (k >= f.param_has_default.size() || !f.param_has_default[k])
              min_arity++;
          }
          auto n = static_cast<int64_t>(names.size());
          auto* disp = culebra_runtime_multifn_register_and_install(
              f.multifn_name.c_str(),
              in.c >= 0 ? reinterpret_cast<JitClosure*>(regs[in.c].data)
                        : nullptr,
              reinterpret_cast<JitClosure*>(regs[in.b].data),
              n ? types.data() : nullptr, n, f.variadic ? 1 : 0,
              /*min_arity=*/min_arity, n ? names.data() : nullptr);
          regs[in.b] = JitValue{TAG_NIL, 0};  // the registry took the +1
          regs[in.a] = JitValue{TAG_FUNC, reinterpret_cast<int64_t>(disp)};
          ++pc;
          break;
        }
        case Op::MfSelf:
          regs[in.a] = culebra_runtime_multifn_self(cls);
          ++pc;
          break;
        case Op::ClsSelf:
          regs[in.a] = culebra_runtime_class_self(
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          ++pc;
          break;
        case Op::WkErr:
          culebra_runtime_wk_contract_error(
              reinterpret_cast<const char*>(c.consts[in.a].data));
          break;  // unreachable — the helper always throws
        case Op::ClassMeta: {
          // Cold path (once per declaration): the names table lives in the
          // chunk, so the array of c_str()s is built here.
          const auto& tbl = c.name_tables[in.d];
          std::vector<const char*> names;
          names.reserve(tbl.size());
          for (const auto& n : tbl) names.push_back(n.c_str());
          auto n_methods = static_cast<int64_t>(in.c);
          // The run is handed over in place: a copy into a heap vector
          // would be invisible to the conservative stack scan, and the
          // first allocation inside build_class_meta would collect every
          // method closure out from under it. object_set consumes each +1
          // — on the contract throw too, where the helper releases the
          // not-yet-bound tail itself — so the registers are emptied only
          // once the call is past, on both edges.
          JitObject* meta;
          try {
            meta = culebra_runtime_build_class_meta(
                names.data(), &regs[in.b], n_methods,
                c.name_table_flags[in.d]);
          } catch (...) {
            for (int32_t i = 0; i < in.c; ++i)
              regs[in.b + i] = JitValue{TAG_NIL, 0};
            throw;
          }
          for (int32_t i = 0; i < in.c; ++i)
            regs[in.b + i] = JitValue{TAG_NIL, 0};
          regs[in.a] = JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(meta)};
          ++pc;
          break;
        }
        case Op::DeriveFn: {
          auto* cl = culebra_runtime_make_derived_method(in.b);
          regs[in.a] = JitValue{TAG_FUNC, reinterpret_cast<int64_t>(cl)};
          ++pc;
          break;
        }
        case Op::RegPack:
          if (in.d)
            culebra_runtime_register_packable_enum(
                reinterpret_cast<const char*>(c.consts[in.a].data),
                reinterpret_cast<const char*>(c.consts[in.b].data));
          else
            culebra_runtime_register_packable(
                reinterpret_cast<const char*>(c.consts[in.a].data),
                reinterpret_cast<const char*>(c.consts[in.b].data));
          ++pc;
          break;
        case Op::EnumVariant: {
          const char* variant =
              reinterpret_cast<const char*>(c.consts[in.c].data);
          const char* en = reinterpret_cast<const char*>(c.consts[in.d].data);
          auto [line, col] = chunk_pos_at(c, pc);
          if (in.b == 0) {
            regs[in.a] = culebra_runtime_build_variant(variant, en, 0, nullptr,
                                                       0, line, col);
          } else {
            regs[in.a] = JitValue{
                TAG_FUNC, reinterpret_cast<int64_t>(
                              culebra_runtime_make_variant_ctor(variant, en,
                                                                in.b))};
          }
          ++pc;
          break;
        }
        case Op::TypeMatch: {
          const JitValue& v = regs[in.a];
          if (culebra_runtime_type_matches(
                  static_cast<int8_t>(v.tag), v.data,
                  reinterpret_cast<const char*>(c.consts[in.c].data)))
            ++pc;
          else
            pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::ClassObj: {
          auto* o = culebra_runtime_object_new();
          culebra_runtime_mark_class(o);
          regs[in.a] = JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(o)};
          ++pc;
          break;
        }
        case Op::BindStatic: {
          culebra_runtime_object_bind_static(
              reinterpret_cast<JitObject*>(regs[in.a].data),
              reinterpret_cast<const char*>(c.consts[in.b].data),
              static_cast<int8_t>(regs[in.c].tag), regs[in.c].data);
          regs[in.c] = JitValue{TAG_NIL, 0};  // the slot absorbed the +1
          ++pc;
          break;
        }
        case Op::MakeInst: {
          const JitValue& meta = regs[in.b];
          const JitValue& finit = regs[in.b + 1];
          const JitValue& body = regs[in.b + 2];
          // The frame's own arguments, forwarded: the helper consumes each
          // +1 on every exit (the `new` body's ABI, its field-init guard,
          // or its own release loop when there is no user `new`).
          regs[in.a] = culebra_runtime_build_class_instance(
              reinterpret_cast<const char*>(c.consts[in.c].data),
              reinterpret_cast<JitObject*>(meta.data),
              static_cast<int8_t>(regs[in.d].tag), regs[in.d].data,
              static_cast<int8_t>(finit.tag), finit.data,
              static_cast<int8_t>(body.tag), body.data, n_args, args);
          ++pc;
          break;
        }
        case Op::FieldInit:
          culebra_runtime_run_field_init(
              reinterpret_cast<JitClosure*>(regs[in.a].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          ++pc;
          break;
        case Op::RegGetter:
          // Registers this lane's getter entry point, which every getter
          // closure here shares — an idempotent insert, unlike the lowering
          // where each getter chunk registers its own function.
          culebra_runtime_register_getter(
              reinterpret_cast<JitClosure*>(regs[in.a].data));
          ++pc;
          break;
        case Op::SelfMerge: {
          // A receiver's +1 transfers straight through, so the raw slot is
          // emptied rather than released; with none it holds the sentinel.
          JitValue abi = regs[in.b];
          regs[in.b] = JitValue{TAG_NIL, 0};
          regs[in.a] = culebra_runtime_self_merge(
              static_cast<int8_t>(abi.tag), abi.data,
              reinterpret_cast<JitCell*>(regs[in.c].data));
          ++pc;
          break;
        }
        case Op::TraitReset: {
          culebra_runtime_trait_defaults_reset(
              reinterpret_cast<const char*>(c.consts[in.a].data));
          ++pc;
          break;
        }
        case Op::TraitDefault: {
          JitValue fn = regs[in.c];
          regs[in.c] = JitValue{TAG_NIL, 0};  // the registry takes the +1
          culebra_runtime_register_trait_default(
              reinterpret_cast<const char*>(c.consts[in.a].data),
              reinterpret_cast<const char*>(c.consts[in.b].data),
              reinterpret_cast<JitClosure*>(fn.data));
          ++pc;
          break;
        }
        case Op::TraitReg: {
          culebra_runtime_register_trait(
              reinterpret_cast<const char*>(c.consts[in.a].data),
              reinterpret_cast<const char*>(c.consts[in.b].data),
              reinterpret_cast<const char*>(c.consts[in.c].data));
          ++pc;
          break;
        }
        case Op::PosSnap: {
          int64_t def = c.consts[in.b].data;
          regs[in.a] = JitValue{
              TAG_LONG, culebra_runtime_param_pos(in.c, def >> 32,
                                                  def & 0xffffffff)};
          ++pc;
          break;
        }
        case Op::ChkTypeAt: {
          auto [line, col] = chunk_pos_at(c, pc);
          if (in.d >= 0) {
            auto pos = _jit_unpack_pos(regs[in.d].data);
            line = pos.line;
            col = pos.col;
          }
          const JitValue& v = regs[in.a];
          culebra_runtime_type_check(
              static_cast<int8_t>(v.tag), v.data,
              reinterpret_cast<const char*>(c.consts[in.b].data),
              reinterpret_cast<const char*>(c.consts[in.c].data), line, col);
          ++pc;
          break;
        }
        case Op::ChkArg: {
          auto [line, col] = chunk_pos_at(c, pc);
          const JitValue& v = regs[in.a];
          culebra_runtime_type_check_param(
              static_cast<int8_t>(v.tag), v.data,
              reinterpret_cast<const char*>(c.consts[in.b].data),
              reinterpret_cast<const char*>(c.consts[in.c].data), in.d, line,
              col);
          ++pc;
          break;
        }
        case Op::JumpIfFilled:
          pc = regs[in.a].tag == TAG_UNFILLED ? pc + 1
                                              : static_cast<size_t>(in.b);
          break;
        case Op::ArgsRest: {
          // The caller's `+1` on each overflow argument moves into the
          // Array; run_frame left them alone for this chunk.
          int64_t from = c.arity;
          if (c.kwargs_rest_idx >= 0 &&
              !(c.kwargs_rest_idx < n_args &&
                args[c.kwargs_rest_idx].tag == TAG_KWREST))
            from = c.first_kw_only_idx >= 0 ? c.first_kw_only_idx
                                            : c.kwargs_rest_idx;
          regs[in.a] = JitValue{
              TAG_ARRAY, reinterpret_cast<int64_t>(
                             culebra_runtime_args_slice_to_array(
                                 args, from, n_args))};
          ++pc;
          break;
        }
        case Op::KwRest:
          // The marked Object arrives already owned by this slot; anything
          // else means no keyword content reached the call.
          regs[in.a] =
              regs[in.a].tag == TAG_KWREST
                  ? JitValue{TAG_OBJECT, regs[in.a].data}
                  : JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(
                                             culebra_runtime_object_new())};
          ++pc;
          break;
        case Op::RecEnter: {
          int64_t d = culebra_runtime_recursion_enter();
          if (in.a) frame_depth = d;
          ++pc;
          break;
        }
        case Op::RecLeave:
          culebra_runtime_recursion_leave();
          ++pc;
          break;
        case Op::NsGet: {
          int8_t tag;
          int64_t data;
          culebra_runtime_namespace_get(
              reinterpret_cast<const char*>(c.consts[in.b].data), &tag, &data);
          regs[in.a] = JitValue{tag, data};  // the resolver's +1
          ++pc;
          break;
        }
        case Op::SetOpPos: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_set_op_pos(line, col);
          ++pc;
          break;
        }
        case Op::BoundPos: {
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_set_call_boundary(line, col);
          ++pc;
          break;
        }
        case Op::Disp:
          regs[in.a] = JitValue{
              TAG_STRING,
              reinterpret_cast<int64_t>(culebra_runtime_value_to_display(
                  static_cast<int8_t>(regs[in.b].tag), regs[in.b].data))};
          ++pc;
          break;
        case Op::Fmt: {
          auto [line, col] = chunk_pos_at(c, pc);
          const char* spec = reinterpret_cast<const char*>(
              in.d ? regs[in.c].data : c.consts[in.c].data);
          regs[in.a] = JitValue{
              TAG_STRING,
              reinterpret_cast<int64_t>(culebra_runtime_format_value(
                  static_cast<int8_t>(regs[in.b].tag), regs[in.b].data, spec,
                  line, col))};
          ++pc;
          break;
        }
        case Op::StrCat:
          regs[in.a] = JitValue{
              TAG_STRING,
              reinterpret_cast<int64_t>(culebra_runtime_str_concat(
                  reinterpret_cast<const char*>(regs[in.b].data),
                  reinterpret_cast<const char*>(regs[in.c].data)))};
          ++pc;
          break;
        case Op::Throw: {
          JitValue v = regs[in.a];
          regs[in.a] = JitValue{TAG_NIL, 0};  // the +1 rides the carrier now
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_throw(static_cast<int8_t>(v.tag), v.data, line, col);
          break;  // unreachable — throw never returns
        }
        case Op::DeferMark:
          regs[in.a] = JitValue{TAG_LONG, culebra_runtime_defer_mark()};
          ++pc;
          break;
        case Op::DeferPush:
          // Borrow: the runtime retains, the frame keeps its +1 (the
          // statement temp's sweep drops it).
          culebra_runtime_defer_push(static_cast<int8_t>(regs[in.a].tag),
                                     regs[in.a].data);
          ++pc;
          break;
        case Op::DeferRunTo:
          culebra_runtime_defer_run_to(regs[in.a].data);
          ++pc;
          break;
        case Op::ForOpen: {
          auto [line, col] = chunk_pos_at(c, pc);
          for_open(regs + in.a, line, col);
          ++pc;
          break;
        }
        case Op::ForNext: {
          // A step raises positionless (a `has_next()` answer with no
          // truthiness, a protocol lost mid-walk); the interpreter reports
          // those at the statement it was running — baked into c/d at
          // compile time.
          culebra_runtime_set_op_pos(in.c, in.d);
          if (for_next(regs + in.a)) ++pc;
          else pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::ForDispose:
          for_dispose(regs + in.a, in.d != 0);
          ++pc;
          break;
        case Op::ForPrep: {
          int64_t step = regs[in.a + 2].data;
          if (step == 0) {
            // The runtime helper is the sole owner of this diagnostic
            // (jit_iter.h); routing through it keeps the lanes identical.
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_range_step_check(step, line, col);
          }
          regs[in.a + 3] = JitValue{TAG_LONG, 0};
          pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::ForLoop: {
          RangeBounds rb{regs[in.a].data, regs[in.a + 1].data,
                         regs[in.a + 2].data, in.d != 0,
                         regs[in.a + 3].data != 0};
          if (rb.done()) {
            ++pc;
            break;
          }
          int64_t v = rb.take();
          regs[in.a] = JitValue{TAG_LONG, rb.cur};
          regs[in.a + 3] = JitValue{TAG_LONG, rb.exhausted ? 1 : 0};
          culebra_runtime_value_release(static_cast<int8_t>(regs[in.c].tag),
                                        regs[in.c].data);
          regs[in.c] = JitValue{TAG_LONG, v};
          pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::Println: {
          // The str walker inside raises a positionless ValueError on a
          // too-deep value; publish this row's position so the boundary
          // backfill lands there (the JIT's emit_output_call order).
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_set_op_pos(line, col);
          culebra_runtime_println(static_cast<int8_t>(regs[in.a].tag),
                                  regs[in.a].data);
          ++pc;
          break;
        }
        case Op::ToFloat: {
          const JitValue& v = regs[in.b];
          if (v.tag == TAG_FLOAT) {
            regs[in.a] = v;
          } else if (v.tag == TAG_LONG) {
            regs[in.a] = {TAG_FLOAT, _culebra_double_to_bits(
                                         static_cast<double>(v.data))};
          } else {
            auto [line, col] = chunk_pos_at(c, pc);
            regs[in.a] = culebra_runtime_to_float_any(
                static_cast<int8_t>(v.tag), v.data, line, col);
          }
          ++pc;
          break;
        }
        case Op::Safepoint:
          if (culebra_g_wake.load(std::memory_order_relaxed))
            culebra::throw_if_interrupted();
          ++pc;
          break;
        case Op::DropSuppress:
          culebra_runtime_set_drop_suppressed(static_cast<int8_t>(in.a));
          ++pc;
          break;
        case Op::BArity: {
          const JitValue& r = regs[in.a];
          for (const auto& arm : c.arity_checks[in.b]) {
            bool hit;
            if (arm.tag >= 0) {
              hit = r.tag == arm.tag;
            } else {
              auto* o = reinterpret_cast<JitObject*>(r.data);
              hit = r.tag == TAG_OBJECT && object_takes_builtin_table(o) &&
                    !culebra_runtime_object_has(
                        o, reinterpret_cast<const char*>(
                               c.consts[arm.name_k].data)) &&
                    (arm.tag == Chunk::kArityObj ||
                     culebra_runtime_object_has(o, "next"));
            }
            if (!hit) continue;
            culebra_runtime_throw_error(
                reinterpret_cast<const char*>(c.consts[arm.kind_k].data),
                reinterpret_cast<const char*>(c.consts[arm.msg_k].data),
                arm.line, arm.col);
          }
          ++pc;
          break;
        }
        case Op::LazyNsReg:
          culebra_runtime_lazy_ns_register(
              reinterpret_cast<const char*>(c.consts[in.c].data),
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data);
          ++pc;
          break;
        case Op::FnHandle:
          regs[in.a] = culebra_runtime_fn_handle(
              static_cast<int8_t>(regs[in.b].tag), regs[in.b].data,
              reinterpret_cast<JitClosure*>(regs[in.c].data), &regs[in.d]);
          ++pc;
          break;
        case Op::OwnedMark:
          marks[in.a] = owned_next_id();
          ++pc;
          break;
        case Op::OwnedExit:
          culebra_runtime_owned_scope_exit(marks[in.a]);
          ++pc;
          break;
        case Op::ReplCell: {
          auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
          regs[in.a] = JitValue{
              TAG_LONG,
              reinterpret_cast<int64_t>(repl_session().cell(nm))};
          ++pc;
          break;
        }
        case Op::ReplBind: {
          auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
          repl_bind(nm, static_cast<ReplBindMode>(in.a), in.c != 0,
                    chunk_pos_at(c, pc));
          ++pc;
          break;
        }
        case Op::DbgStmt: {
          auto [line, col] = chunk_pos_at(c, pc);
          auto& st = dbg_state();
          if (st.tracking && !st.frames.empty()) {
            auto& f = st.frames.back();
            f.pc = pc;
            f.line = line;
            f.col = col;
          }
          ++pc;
          if (st.hook) {
            st.hook(in.a != 0, line, col);
          } else if (in.a) {
            // Plain `--debug`, no session: the minimal break the JIT's
            // `debugger` compiles to (show the source, any input resumes).
            culebra_runtime_debugger_break(p.source_path.c_str(), line, col);
          }
          break;
        }
        case Op::Halt:
          return JitValue{TAG_NIL, 0};
      }
    }
  }
};

// The executor lane for a loader's module list: what `--vm` runs, and the
// twin of run_modules_via_llvm (vm_lowering.h) for the consumer that only
// wants the program run. A consumer whose closures outlive the run compiles
// it itself and keeps it (RetainedProgram below, `culebra dap --vm`).
inline void run_modules(const std::vector<LoadedModule>& modules,
                        Debug dbg = Debug::Off) {
  if (modules.empty()) return;
  auto prog = Compiler::compile_modules(modules, dbg);
  Exec::run(prog);
}

// A program and everything its compilation read. Chunks intern their own
// string constants, but a closure the program built reaches its bytecode
// through a descriptor pointing into the program, so a session that can call
// the closure later must keep the program — and the source and AST it was
// compiled from — alive too.
struct RetainedProgram {
  std::shared_ptr<std::string> source;  // null when the AST has no file
  std::shared_ptr<peg::Ast> ast;
  std::unique_ptr<VmProgram> prog;
};

// The session-lifetime owner of retained programs: every program a line
// compiled stays alive until no retained closure can be called again.
class RetainedRuns {
 public:
  RetainedProgram& keep(std::shared_ptr<std::string> source,
                        std::shared_ptr<peg::Ast> ast,
                        std::unique_ptr<VmProgram> prog) {
    return runs_.emplace_back(
        RetainedProgram{std::move(source), std::move(ast), std::move(prog)});
  }

 private:
  std::deque<RetainedProgram> runs_;
};

}  // namespace culebra::vm
