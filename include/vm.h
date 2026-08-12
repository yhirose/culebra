#pragma once

// The bytecode VM — the third backend (docs/internals/vm.md §7, Phase 1),
// grown out of the Phase 0 spike: a register-based, RC-explicit,
// lowering-friendly bytecode; a compiler whose front end is the shared
// FnAnalysis (fn_analysis.h); a VM executor running on the JIT's runtime
// value model; and an LLVM lowering of the same bytecode, kept alive so the
// format stays consumable by both engines. Hidden behind --vm / --vm-dump /
// --vm-llvm; constructs outside the supported slice are rejected at compile
// time ("VmError") — no interp fallback. The format is an in-memory
// contract only.
//
// Current slice (the expression core + basic control flow): Long / Float /
// Bool / nil / String literals; Array (incl. sized `[v](n, d)` and `...`
// spread elements), Tuple, and Set literals; interpolated and triple strings
// (embedded expressions, format specs, the shared block dedent);
// `let` / `let mut` / reassignment /
// compound assignment (op= and ??=) of plain identifiers; arithmetic
// + - * / % ** and unary - ! + ~ with the JIT's dispatch shape; the
// Long-only bitwise/shift ops & | ^ << >>;
// comparisons (including chains) and `&&` / `||` / `??`;
// `if` / `else if` / `else` (as an expression) and the ternary; `while`;
// counted range `for`; `break` / `continue`; single-argument `println`;
// range values (`a..b`, `a..=b`, `by step`, open ends, the bare `..`) and
// slicing (`xs[1..3]` and stored-range keys — Array copy / String view /
// Tuple new tuple; write-context range keys fall to the runtime Long-key
// error like both backends);
// fn literals with closures (captured locals promoted to JitCells, the
// JIT's cell mechanism — forward-reference capture is still rejected);
// patterns — literals, `_`, bindings, typed bindings over primitive type
// names, or-patterns of non-binding alternatives, and array / tuple / object
// patterns with nesting, `...rest` and sinks — driving both `match` arms
// (with guards) and destructuring declarations / parallel assignment;
// `fn name` declarations with arity-dispatch overloads through the shared
// multimethod runtime, incl. self/mutual recursion via pre-declared
// dispatcher cells (reads before the decl runs raise NameError);
// the bare stdlib globals (to_string, type_of, range, ... — kBuiltinFns'
// native functions), resolved per-Runtime through the JIT's namespace_get
// slow path and called like any closure. Lazy source modules (Time,
// assert_*, ...) need the preamble splice, which the VM lane skips, so
// those names stay rejected.

#ifdef CULEBRA_JIT_ENABLED

#include <fn_analysis.h>
#include <jit.h>
#include <parser.h>
#include <range_bounds.h>
#include <shared.h>
#include <stdlib_jit.h>  // culebra_runtime_println + the rt::println decl hook

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace culebra::vm {

inline constexpr int32_t kMaxSlots = 256;

// One fixed-width instruction. Registers are frame slots holding JitValue.
// RC is explicit in the stream (vm.md §4): the compiler emits Retain/Release;
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
  Mod,         // with the instruction's line/col from the position table
  Pow,         // regs[a] = regs[b] ** regs[c] via num_pow_borrow — no inline
               // fast path (the AST JIT's generic tail is the same call; its
               // literal-exponent peepholes are numeric-guarded, so every
               // input agrees with the helper)
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
               // whose receiver gate (built-in d's tag mask, else the
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
  Ret,         // return regs[a] from the frame (+1 transfers to the caller)
  CellNew,     // regs[a] = new JitCell absorbing regs[b]'s +1; regs[b] = nil.
               // Releases the cell previously in regs[a] (null on first run —
               // a loop's per-iteration redeclaration, like make_cell_slot).
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
  MultifnReg,  // regs[a] = dispatcher (+1) after registering closure regs[b]
               // as an overload of multimethod key consts[c] (a chunk-owned
               // string); d = the body's chunk index, whose param_names feed
               // the registry (types stay null — arity-only dispatch in the
               // slice). The registry absorbs the body's +1; regs[b] = nil.
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
  Disp,        // regs[a] = display string of regs[b] (value_to_display,
               // borrow) — a bare `{expr}` piece. The result is a fresh
               // heap string: TAG_STRING, so outside RC entirely.
  Fmt,         // regs[a] = format_value(regs[b], spec consts[c]) with this
               // instruction's line/col (the piece node's position — spec
               // errors report there) — a `{expr:spec}` piece
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
  Safepoint,   // interrupt poll — every loop back edge carries one
  Halt,
};

struct Insn {
  Op op;
  int32_t a = 0, b = 0, c = 0, d = 0;
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
  Union, Intersect, Diff, SymDiff, Subset, Superset, Add, Remove,
  ToArray,                              // Set only
};

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
// to_array: three unrelated value tables bind it — Set, Tuple, and Tensor
// (`grep '"to_array"sv'` across interpreter.h's builtins tables), each with
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
// No gate at all: `to_string` is the display conversion every value has, so
// no receiver can fail to resolve it.
inline constexpr BRecvMask kRecvAny = 0xFFFF;

// A parameter's declared type, checked with the interp binder's wording at the
// argument's own position. `Any` is an undeclared parameter — no check at all,
// so the compiler emits no ChkParam for it. `String` is strict (unlike
// StrLike, a StringView fails it) — `join`'s `sep` is declared plain "String"
// in the interp table, not the StringLike trait.
enum class BParam : uint8_t { Any, Long, StrLike, Array, String, Set };

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
      {"split", 1, Split, kRecvStrLike, 1, nullptr, {StrLike}, {"sep"}},
      {"starts_with", 1, StartsWith, kRecvStrLike, 1, nullptr,
       {StrLike}, {"prefix"}},
      {"ends_with", 1, EndsWith, kRecvStrLike, 1, nullptr,
       {StrLike}, {"suffix"}},
      // Array. `push`/`insert` take the value's +1 off the register, so the
      // compiler nils the run before the call (see bmeth_consumes_args);
      // `pop`/`remove_at` hand one back the other way.
      {"push", 1, Push, kRecvArray, 1, nullptr, {Any}, {"arg"}},
      {"pop", 0, Pop, kRecvArray, 0, nullptr, {}, {}},
      {"insert", 2, Insert, kRecvArray, 2, nullptr, {Long, Any}, {"i", "x"}},
      {"remove_at", 1, RemoveAt, kRecvArray, 1, nullptr, {Long}, {"i"}},
      {"extend", 1, Extend, kRecvArray, 1, nullptr, {Array}, {"other"}},
      {"reverse", 0, Reverse, kRecvArray, 0, nullptr, {}, {}},
      {"index_of", 1, IndexOf, kRecvArray, 1, nullptr, {Any}, {"v"}},
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
      // `reverse:` is kw-only in the interp/JIT signature; the VM's call
      // compiler rejects every keyword argument at compile time already
      // (reject_kwargs), so this arm is reachable only without it.
      {"sorted", 0, Sorted, kRecvArray, 0, nullptr, {}, {}},
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
      // is kw-only in the interp/JIT signature; the VM's reject_kwargs makes
      // this arm reachable only without it, so the call is always `(f)`.
      {"sort_by", 1, SortBy, kRecvArray, 1, nullptr, {Any}, {"f"}, {}},
      {"sorted_by", 1, SortedBy, kRecvArray, 1, nullptr, {Any}, {"f"}, {}},
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
      {"remove", 1, Remove, kRecvSet, 1, nullptr, {Any}, {"x"}},
      {"to_array", 0, ToArray, kRecvToArray, 0, nullptr, {}, {}},
  };
  return kSpecs;
}

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

// The name a parameter's declared type carries into the error message.
inline const char* bmeth_param_type(BParam p) {
  switch (p) {
    case BParam::Long: return "Long";
    case BParam::Array: return "Array";
    case BParam::String: return "String";
    case BParam::Set: return "Set";
    default: return "StringLike";
  }
}

// Whether the built-in takes its arguments' `+1` off the registers.
inline bool bmeth_consumes_args(BMeth id) {
  return id == BMeth::Push || id == BMeth::Insert || id == BMeth::Add ||
         // The higher-order group: the runtime helper owns the callback (and
         // reduce's seed) from entry on every exit, like a JIT AST callsite's
         // hof_owned list.
         id == BMeth::Map || id == BMeth::Filter || id == BMeth::ForEach ||
         id == BMeth::AnyOf || id == BMeth::All || id == BMeth::Find ||
         id == BMeth::FlatMap || id == BMeth::MinBy || id == BMeth::MaxBy ||
         id == BMeth::Reduce || id == BMeth::GroupBy ||
         id == BMeth::Partition || id == BMeth::SortBy ||
         id == BMeth::SortedBy;
}

// The accepted tags, tested inline like the JIT arms' own gates
// (coerce_strlike_cstr, emit_builtin_long_arg). The test cannot be left to
// culebra_runtime_type_check alone: `StringLike` is a built-in TRAIT, and its
// registry entry comes from the preamble the VM lanes do not splice, so the
// runtime check would reject a perfectly good String there.
inline bool bmeth_param_ok(BParam p, int8_t tag) {
  switch (p) {
    case BParam::Any: return true;
    case BParam::Long: return tag == TAG_LONG;
    case BParam::Array: return tag == TAG_ARRAY;
    case BParam::StrLike:
      return tag == TAG_STRING || tag == TAG_STRINGVIEW;
    case BParam::String: return tag == TAG_STRING;
    case BParam::Set: return tag == TAG_SET;
  }
  return false;
}

// Whether a parameter's check covers this receiver. A polymorphic built-in
// declares its parameter per arm, so the same argument is checked on the
// String receiver and waved through on the Array one.
inline bool bmeth_param_applies(const BMethSpec& s, int32_t i, int8_t tag) {
  return s.param_when[i] == 0 || bmeth_receiver_ok(s.param_when[i], tag);
}

// The message a rejected argument carries, in the interp binder's wording.
inline std::string bmeth_param_message(const BMethSpec& s, int32_t i) {
  return std::format("type error: parameter '{}' expects {}", s.pnames[i],
                     bmeth_param_type(s.params[i]));
}

// The gate fields (receiver mask, iterator shape) are the same across an id's
// arities, so the first row answers for all of them.
inline const BMethSpec& bmeth_gate_spec(BMeth id) {
  for (const auto& s : bmeth_specs())
    if (s.id == id) return s;
  return bmeth_specs()[0];  // unreachable: every id has a spec
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
  auto arr = [](const JitValue& v) {
    return reinterpret_cast<JitArray*>(v.data);
  };
  auto st = [](const JitValue& v) { return reinterpret_cast<JitSet*>(v.data); };
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
                          cstr(recv), cstr(args[0])))};
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
      culebra_runtime_array_reverse(arr(recv));
      return JitValue{TAG_NIL, 0};
    case BMeth::IndexOf:
      // A too-deep element raises a positionless ValueError from the compare.
      culebra_runtime_set_op_pos(line, col);
      return JitValue{TAG_LONG, culebra_runtime_array_index_of(
                                    arr(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data)};
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
      // `reverse:` is kw-only and the VM rejects every keyword argument at
      // compile time, so this arm is reachable only without it.
      return JitValue{TAG_ARRAY,
                      reinterpret_cast<int64_t>(culebra_runtime_array_sorted(
                          arr(recv), /*reverse=*/false, line, col))};
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
      // `reverse:` is kw-only and the VM rejects every keyword argument at
      // compile time, so this arm is reachable only without it.
      culebra_runtime_array_sort_by(arr(recv), static_cast<int8_t>(args[0].tag),
                                    args[0].data, /*reverse=*/false, line,
                                    col);
      return JitValue{TAG_NIL, 0};
    case BMeth::SortedBy:
      return JitValue{
          TAG_ARRAY,
          reinterpret_cast<int64_t>(culebra_runtime_array_sorted_by(
              arr(recv), static_cast<int8_t>(args[0].tag), args[0].data,
              /*reverse=*/false, line, col))};
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
          return JitValue{
              TAG_ARRAY,
              reinterpret_cast<int64_t>(culebra_runtime_tensor_to_array(
                  reinterpret_cast<JitTensor*>(recv.data)))};
        default:  // TAG_SET, the gate's remaining case
          return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                         culebra_runtime_set_to_array(
                                             st(recv)))};
      }
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
  int32_t num_slots = 0;
  std::vector<PosEntry> positions;
  std::vector<std::string> slot_names;  // debug table, always emitted
  // Function-chunk metadata (chunk 0 — the module top level — has none).
  // Params occupy slots [0, arity); `fn_slot` binds the `fn` recursion
  // handle when the body reads it (FuncInfo::uses_fn), -1 otherwise.
  int32_t arity = 0;
  std::vector<std::string> param_names;  // for the ArityError message
  int32_t fn_slot = -1;
  // Capture list: for each free var, the slot (in the CREATING frame's
  // numbering) holding its cell pointer. A fn literal has exactly one
  // creation site — its MakeClosure — so the list lives with the callee
  // chunk instead of being encoded into the instruction.
  std::vector<int32_t> capture_src_slots;
  // try/catch region: a throw at pc in [start, end) lands at `handler` with
  // the caught value in `caught_slot` (written by the dispatcher / the
  // lowered landingpad prelude before the jump). The handler opens with a
  // bytecode release ladder for every slot the region allocated — sound
  // because Release/CellRelease are destructive and nil-safe, and under the
  // borrow operand contract a throw leaves every register frame-owned.
  // Entries are pushed innermost-first (a nested try compiles before its
  // encloser finishes), so the first entry containing pc is the target.
  struct EhRegion {
    uint32_t start, end, handler;
    int32_t caught_slot;
  };
  std::vector<EhRegion> eh;
  // Frame-level defer mark slot (-1 when the chunk has no defers). Taken by
  // the chunk's first instruction; a throw that no region catches runs the
  // frame's pending defers back to it before unwinding out (Exec::run_frame's
  // catch-all, the lowering's frame cleanup pad) — the observable slice of
  // the JIT's frame cleanup ladder. Slot releases still fall to the GC
  // backstop on that path (the known Phase 1 residual).
  int32_t defer_mark_slot = -1;
};

struct VmProgram;

// What the executor's closures point at: the trampoline recovers the chunk
// to interpret from one of these (stashed as a Long in the closure's capture
// cell). Exec::run fills `descs` — compile_module returns the program by
// value, so its final address exists only at run time.
struct VmFnDesc {
  const VmProgram* prog;
  int32_t chunk;
};

// A compiled module: chunk 0 is the top level; every function literal adds
// one (reserved in creation order, so nested literals interleave freely).
struct VmProgram {
  std::vector<Chunk> chunks;
  std::vector<VmFnDesc> descs;  // filled by Exec::run, one per chunk
  // One shared descriptor cell per chunk (also Exec::run's): MakeClosure
  // retains it instead of allocating a cell per closure created.
  std::vector<JitCell*> desc_cells;
};

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
// the scope stack below, mirroring the JIT's Scope/VarSlot walk.
// One Compiler instance per chunk; the program and the analysis are shared.
// Everything outside the slice throws VmError at compile time.
class Compiler {
 public:
  static VmProgram compile_module(const peg::Ast& ast) {
    VmProgram prog;
    FnAnalysis analysis(&JIT::is_builtin_var);
    // lint::check_shadow (parity) + the per-fn FuncInfo compile_fn_chunk
    // reads; the returned top-level info carries chunk 0's captured_locals.
    FuncInfo top_info = analysis.analyze_program(ast);
    prog.chunks.emplace_back();  // reserve index 0 for the top level
    int multifn_uid = 0;  // program-wide (the JIT's multifn_uid_counter_)
    Compiler main(prog, analysis, /*in_function=*/false, &top_info,
                  &multifn_uid);
    // The top-level frame mark (JIT main's fn.mark): first insn, so a
    // throw at any pc finds it populated. The Halt epilogue runs to it —
    // before the top scope's releases, hence the inlined block below.
    main.establish_frame_defer_mark(ast, top_info);
    {
      using namespace peg::udl;
      main.push_scope();
      main.predeclare_multifns(ast);
      if (ast.tag == "STATEMENTS"_) {
        for (const auto& n : ast.nodes) main.compile_statement(*n);
      } else {
        main.compile_statement(ast);
      }
      if (main.frame_defer_mark_ >= 0)
        main.emit(Op::DeferRunTo, main.frame_defer_mark_);
      main.pop_scope();
    }
    main.emit(Op::Halt);
    main.chunk_.num_slots = main.high_water_;
    prog.chunks[0] = std::move(main.chunk_);
    return prog;
  }

 private:
  Compiler(VmProgram& prog, FnAnalysis& analysis, bool in_function,
           const FuncInfo* info, int* multifn_uid)
      : prog_(prog),
        analysis_(analysis),
        in_function_(in_function),
        info_(info),
        multifn_uid_(multifn_uid) {}

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
  };
  struct Scope {
    std::vector<Binding> bindings;
    int32_t slot_watermark;  // next_slot_ at scope entry; pop rolls back
    // Lexical key into the process-global multimethod registry for each
    // `fn name` declared directly in this scope: same-scope overloads
    // share one key (one dispatcher + method table), a same-named decl in
    // a different scope gets a distinct key (the JIT's
    // Scope::multifn_keys / multifn_scope_key).
    std::map<std::string, std::string> multifn_keys;
  };
  struct LoopCtx {
    int32_t slot_watermark;  // slots >= this are inner to the loop scope
    size_t defer_watermark;  // defer_scopes_.size() at loop entry: entries
                             // above it are scopes a break/continue jumps
                             // out of, and the first (outermost) one's mark
                             // bounds every defer the iteration pushed
    std::vector<size_t> break_jumps;
    std::vector<size_t> continue_jumps;
  };
  struct ExprResult {
    int32_t slot;
    bool owned;  // true: statement temp holding a +1; false: named slot
  };

  VmProgram& prog_;
  FnAnalysis& analysis_;
  bool in_function_;
  const FuncInfo* info_;  // this chunk's analysis (captured_locals gate)
  int* multifn_uid_;      // shared across nested chunk compilers
  Chunk chunk_;
  std::vector<Scope> scopes_;
  std::vector<LoopCtx> loops_;
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
  // Parallel: the slot owns a cell (a captured local's CellNew target), so
  // scope exit emits CellRelease instead of Release. Borrowed capture slots
  // stay false — the closure owns their cell ref.
  std::vector<bool> slot_cell_;
  // Owned cell slots are PINNED: never returned to the allocator, so a slot
  // is cell-or-plain for the chunk's whole lifetime. A try handler's release
  // ladder chooses Release vs CellRelease statically per slot — a slot that
  // hosted a cell in one generation and a plain heap value in another would
  // make that choice wrong at runtime. The wasted indices are bounded by the
  // number of captured bindings (kMaxSlots rejects overflow).
  int32_t pin_floor_ = 0;
  // Frame-level defer mark slot (mirrors the JIT's fn.mark, gated on
  // has_any_defer): `return` and the Halt epilogue run to it, as does the
  // executor / frame pad when a throw escapes every region.
  int32_t frame_defer_mark_ = -1;
  // Mark slots of the open scopes that declared their own defers, outermost
  // first (the JIT's Scope::defer_mark, flattened): break/continue run to
  // the first entry above the loop's defer_watermark.
  std::vector<int32_t> defer_scopes_;

  void mark_cell_slot(int32_t s) {
    slot_cell_[s] = true;
    pin_floor_ = std::max(pin_floor_, s + 1);
  }
  uint32_t pend_line_ = 0, pend_col_ = 0;

  [[noreturn]] static void reject(const peg::Ast& ast, const std::string& what) {
    throw CulebraError("VmError", "--vm: unsupported: " + what,
                       static_cast<int64_t>(ast.line),
                       static_cast<int64_t>(ast.column));
  }

  void stamp(const peg::Ast& ast) {
    if (ast.line) {
      pend_line_ = static_cast<uint32_t>(ast.line);
      pend_col_ = static_cast<uint32_t>(ast.column);
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
    chunk_.code.push_back({op, a, b, c, d});
    return chunk_.code.size() - 1;
  }

  // Jump-target operand encoding, single-sourced: an unconditional Jump
  // carries its target in `.a`, every conditional (and ForPrep) in `.b`.
  void patch_jump(size_t ix, size_t target) {
    auto& insn = chunk_.code[ix];
    (insn.op == Op::Jump ? insn.a : insn.b) = static_cast<int32_t>(target);
  }
  void patch_to_here(size_t ix) { patch_jump(ix, chunk_.code.size()); }

  int32_t alloc_raw(const peg::Ast& at, std::string name, bool named) {
    if (next_slot_ >= kMaxSlots) reject(at, "frame larger than 256 slots");
    int32_t s = next_slot_++;
    if (s >= static_cast<int32_t>(chunk_.slot_names.size()))
      chunk_.slot_names.resize(s + 1);
    chunk_.slot_names[s] = std::move(name);
    if (s >= static_cast<int32_t>(slot_named_.size())) {
      slot_named_.resize(s + 1);
      slot_cell_.resize(s + 1);
    }
    slot_named_[s] = named;
    slot_cell_[s] = false;
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

  void push_scope() { scopes_.push_back({{}, next_slot_}); }

  // The release ladder every scope exit uses (reverse order, mirroring the
  // JIT's frame ladder). Releases only the named slots: statement temps in
  // the range are owned by a live TempScope (or are a frame's return-value
  // slot), which release on their own paths.
  void release_down_to(int32_t watermark) {
    for (int32_t s = next_slot_ - 1; s >= watermark; --s)
      if (slot_named_[s])
        emit(slot_cell_[s] ? Op::CellRelease : Op::Release, s);
  }

  // Emits the scope's Releases and returns its slots to the allocator
  // (pinned cell slots stay allocated; their stale named/cell flags only
  // cost an outer ladder a redundant nil-safe CellRelease).
  void pop_scope() {
    const auto& sc = scopes_.back();
    release_down_to(sc.slot_watermark);
    next_slot_ = std::max(sc.slot_watermark, pin_floor_);
    named_top_ = std::min(named_top_, next_slot_);
    scopes_.pop_back();
  }

  const Binding* lookup(std::string_view name) const {
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

  // Declaration point of a captured local: a fresh cell absorbs the value.
  void store_new_cell(const peg::Ast& at, int32_t dst, ExprResult r) {
    emit(Op::CellNew, dst, owned_src(at, r));
  }

  // Reassignment through a cell binding (own or captured).
  void store_cell(const peg::Ast& at, int32_t dst, ExprResult r) {
    emit(Op::CellSet, dst, owned_src(at, r));
  }

  // A read of a cell binding: the value comes out retained, so the result
  // is an owned temp (JIT load_slot), unlike a plain slot's borrow. A lazy
  // dispatcher cell read before its decl ran guards for the unbound
  // sentinel (NameError at the reference, interp parity).
  ExprResult read_binding(const peg::Ast& at, const Binding& b) {
    if (!b.is_cell) return {b.slot, false};
    int32_t t = alloc_temp(at);
    emit(Op::CellGet, t, b.slot);
    if (b.lazy) emit(Op::UnboundErr, t, kconst_str(b.name));
    return {t, true};
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
    push_scope();
    predeclare_multifns(ast);
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
    push_scope();
    predeclare_multifns(ast);
    compile_body_into(ast, dst);
    ds.close();
    pop_scope();
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

  void compile_statement(const peg::Ast& ast) {
    stamp(ast);
    TempScope ts(*this);
    compile_statement_inner(ast);
  }

  void compile_statement_inner(const peg::Ast& ast) {
    using namespace peg::udl;
    switch (ast.tag) {
      case "STATEMENTS"_:
        compile_block(ast);
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
        emit(Op::Ret, rv);
        break;
      }
      default:
        compile_expr(ast);  // expression statement; temps swept by the caller
        break;
    }
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

  // Scope-entry pre-declaration of `fn name` dispatcher cells (the multifn
  // slice of the JIT's pre_allocate_forward_refs): every name a
  // MULTIFN_DECL in this statement list declares gets an owned cell holding
  // the unbound sentinel before any statement runs, so a closure built
  // earlier in the list captures a real cell (mutual recursion resolves)
  // and a read before the decl statement ran raises NameError through the
  // binding's lazy guard. One reused temp feeds every CellNew, so the only
  // lasting slots are the pinned cells themselves.
  void predeclare_multifns(const peg::Ast& ast) {
    using namespace peg::udl;
    std::vector<std::pair<const peg::Ast*, std::string>> decls;
    auto handle = [&](const peg::Ast& node) {
      if (node.tag != "MULTIFN_DECL"_) return;
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) i++;
      auto name = std::string(
          culebra::parse_generic_head(node.nodes[i]->token).outer);
      for (const auto& [n_, nm] : decls)
        if (nm == name) return;  // overloads share the first decl's cell
      decls.emplace_back(node.nodes[i].get(), std::move(name));
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
    if (decls.empty()) return;
    TempScope ts(*this);
    std::vector<int32_t> cslots;
    for (const auto& [at, name] : decls) cslots.push_back(alloc_slot(*at, name));
    int32_t tmp = alloc_temp(ast);
    for (size_t k = 0; k < decls.size(); ++k) {
      emit(Op::LoadConst, tmp, kconst({TAG_NO_SELF, 0}));
      emit(Op::CellNew, cslots[k], tmp);  // nils tmp; reloaded per name
      mark_cell_slot(cslots[k]);
      scopes_.back().bindings.push_back({decls[k].second, cslots[k],
                                         /*is_mut=*/false, /*is_cell=*/true,
                                         /*lazy=*/true});
    }
  }

  // `fn name(params) { body }` — a free named-function declaration. The
  // slice registers arity-dispatch overloads through the same runtime
  // multimethod registry the JIT uses (multifn_register_and_install):
  // same-scope overloads merge into one dispatcher, a nested-scope decl
  // shadows through its own per-scope registry key, a same-arity re-decl
  // replaces its table entry, and DispatchError kind/message/position come
  // from the shared dispatcher thunk. Installing stores the dispatcher
  // into the cell predeclare_multifns created, flipping it from the
  // unbound sentinel.
  void compile_multifn_decl(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes[0]->tag == "DECORATOR"_) reject(ast, "decorator");
    auto head = culebra::parse_generic_head(ast.nodes[0]->token);
    if (!head.args.empty())
      reject(*ast.nodes[0], "generic type parameters");
    auto name = std::string(head.outer);
    int32_t idx = compile_fn_chunk(ast, ast.nodes[1].get(),
                                   *ast.nodes.back());
    int32_t cls = alloc_temp(ast);
    emit(Op::MakeClosure, cls, idx);
    // Same-scope overloads share one registry key (the binding's scope is
    // the current one — predeclare ran at its entry).
    auto& keys = scopes_.back().multifn_keys;
    auto it = keys.find(name);
    if (it == keys.end()) {
      it = keys.emplace(name, name + '\x1f' +
                                  std::to_string((*multifn_uid_)++)).first;
    }
    int32_t t = alloc_temp(ast);
    emit(Op::MultifnReg, t, cls, kconst_str(it->second), idx);
    forget_temp(cls);  // the registry absorbed the body's +1 (reg is nil)
    const Binding* b = lookup(name);
    if (!b || !b->is_cell)
      reject(ast, std::format("fn '{}' declared here", name));
    emit(Op::CellSet, b->slot, owned_src(ast, {t, true}));
  }

  // Resolve a nested chunk's capture list in the creating frame. The mut
  // flag rides along (the JIT's free_var_mut snapshot): a capture of a
  // capture keeps the original binding's flag by construction.
  struct CaptureList {
    std::vector<int32_t> slots;
    std::vector<bool> muts;
    std::vector<bool> lazys;  // the JIT's free_var_lazy snapshot
  };
  CaptureList resolve_captures(const peg::Ast& ast, const FuncInfo& info) {
    CaptureList caps;
    for (const auto& fv : info.free_vars) {
      if (fv == "self") reject(ast, "closure capture of 'self'");
      if (info.optional_free_vars.contains(fv))
        reject(ast, std::format("UFCS candidate capture of '{}'", fv));
      const Binding* b = lookup(fv);
      // The JIT covers this with lazy forward-ref cells
      // (pre_allocate_forward_refs); outside the slice for now — except
      // `fn name` dispatcher cells, which predeclare_multifns creates at
      // scope entry, so mutual recursion resolves right here.
      if (!b) reject(ast, std::format("forward-reference capture of '{}'", fv));
      caps.slots.push_back(b->slot);
      caps.muts.push_back(b->is_mut);
      caps.lazys.push_back(b->lazy);
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
  int32_t compile_fn_chunk(const peg::Ast& ast, const peg::Ast* params,
                           const peg::Ast& body) {
    using namespace peg::udl;
    const FuncInfo& info = analysis_.func_info.at(&ast);
    if (info.uses_args) reject(ast, "__ARGS__");
    for (const auto& n : ast.nodes)
      if (n->tag == "RETURN_TYPE"_) reject(*n, "return type annotation");
    if (params) {
      for (const auto& p : params->nodes) {
        if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p))
          reject(*p, "keyword-only / rest parameter");
        if (culebra::is_pattern_param(*p)) reject(*p, "pattern parameter");
        if (culebra::extract_default_expr(*p)) reject(*p, "default argument");
        for (const auto& pc : p->nodes)
          if (pc->tag == "TYPE_ANNOTATION"_) reject(*p, "typed parameter");
      }
    }
    auto caps = resolve_captures(ast, info);

    int32_t idx = static_cast<int32_t>(prog_.chunks.size());
    prog_.chunks.emplace_back();  // reserve the index; nested fns append
    Compiler fc(prog_, analysis_, /*in_function=*/true, &info, multifn_uid_);
    fc.stamp(ast);
    fc.push_scope();  // the frame scope: params + captures + the `fn` handle
    fc.chunk_.arity =
        params ? static_cast<int32_t>(params->nodes.size()) : 0;
    fc.chunk_.capture_src_slots = std::move(caps.slots);
    // Params occupy the ABI slots [0, arity). A captured param moves into a
    // fresh cell right after (the JIT's make_cell_slot on a param); the ABI
    // slot stays behind as an anonymous drained slot.
    struct CellPromo {
      std::string name;
      int32_t abi_slot;
      bool is_mut;
      const peg::Ast* at;
    };
    std::vector<CellPromo> promos;
    if (params) {
      for (const auto& p : params->nodes) {
        auto pv = culebra::view_parameter(*p);
        auto name = std::string(pv.name);
        int32_t slot = fc.alloc_slot(*p, name);
        fc.chunk_.param_names.push_back(name);
        if (is_sink_name(name)) continue;
        if (info.captured_locals.contains(name)) {
          promos.push_back({name, slot, pv.is_mut, p.get()});
        } else {
          fc.scopes_.back().bindings.push_back({name, slot, pv.is_mut});
        }
      }
    }
    if (info.uses_fn) {
      fc.chunk_.fn_slot = fc.alloc_slot(ast, "fn");
      fc.scopes_.back().bindings.push_back({"fn", fc.chunk_.fn_slot, false});
    }
    fc.establish_frame_defer_mark(ast, info);
    for (const auto& pr : promos) {
      int32_t cslot = fc.alloc_slot(*pr.at, pr.name);
      fc.emit(Op::CellNew, cslot, pr.abi_slot);
      fc.mark_cell_slot(cslot);
      fc.scopes_.back().bindings.push_back({pr.name, cslot, pr.is_mut, true});
    }
    // Bind the captures: borrowed cell pointers out of the closure. The
    // slots are named-but-not-cell, so frame teardown's Release is a no-op
    // on them (the closure owns the refs). The lazy flag rides along so a
    // captured dispatcher cell read before its decl still NameErrors.
    for (size_t i = 0; i < info.free_vars.size(); ++i) {
      int32_t s = fc.alloc_slot(ast, info.free_vars[i]);
      fc.emit(Op::BindCapture, s, static_cast<int32_t>(i));
      fc.scopes_.back().bindings.push_back(
          {info.free_vars[i], s, caps.muts[i], true, caps.lazys[i]});
    }
    int32_t rv = fc.alloc_temp(ast);
    fc.compile_block_into(body, rv);
    // Frame defers run before the frame scope's releases (interp's
    // run_deferred(callEnv) order); `return` emits its own copy.
    if (fc.frame_defer_mark_ >= 0)
      fc.emit(Op::DeferRunTo, fc.frame_defer_mark_);
    fc.pop_scope();
    fc.emit(Op::Ret, rv);
    fc.chunk_.num_slots = fc.high_water_;
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
    // symmetric by rejecting instead of quietly diverging.
    if (analysis_.func_info.at(&ast).uses_fn)
      reject(ast, "recursion handle `fn` inside defer");
    return compile_fn_chunk(ast, /*params=*/nullptr, *ast.nodes[0]);
  }

  // Evaluates to the assigned value (interp parity: `let c = if true
  // { let x = 5 }` reads 5) — returned as a borrow of the target slot.
  ExprResult compile_assignment(const peg::Ast& ast) {
    auto av = culebra::view_assignment(ast);
    if (!av.type_annotation.empty()) reject(ast, "type annotation");
    auto* tgt = culebra::assign_name_target(ast, av);
    if (!tgt) {
      if (av.lvalcnt > 1) return compile_assign_index(ast, av);
      reject(ast, "non-identifier assignment target");
    }
    if (tgt->token == "_") reject(*tgt, "sink binding");
    if (av.compound) return compile_compound_assign(ast, av, *tgt);
    if (av.is_let || av.is_mut) {  // interp's assign_name: let||mut declares
      // Slot reserved before the RHS so temps stack above it; the name only
      // becomes visible after the RHS (let x = x reads the outer x).
      auto name = std::string(tgt->token);
      bool cell = info_->captured_locals.contains(name);
      int32_t slot = alloc_slot(*tgt, name);
      if (cell) {
        store_new_cell(*tgt, slot, compile_expr(*av.rhs));
        mark_cell_slot(slot);
      } else {
        store_into(slot, compile_expr(*av.rhs), /*dst_is_fresh=*/true);
      }
      scopes_.back().bindings.push_back({name, slot, av.is_mut, cell});
      return read_binding(*tgt, scopes_.back().bindings.back());
    }
    const Binding* b = lookup(tgt->token);
    if (!b) reject(*tgt, "assignment to an undeclared name");
    auto r = compile_expr(*av.rhs);
    if (!b->is_mut) {
      // Runtime ImmutableError, after the RHS ran — matching the interp/JIT
      // order and keeping a never-executed assignment silent. The RHS temp
      // strands like any value abandoned by a throw (conservative backstop).
      StampGuard pos(*this, ast);
      emit(Op::ImmutErr, kconst_str(tgt->token));
      return {b->slot, false};  // unreachable
    }
    if (b->is_cell) store_cell(*tgt, b->slot, r);
    else store_into(b->slot, r);
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
    if (!b && !is_stdlib_global(tgt.token))
      reject(tgt, "compound assignment to an undeclared name");

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
      auto rhs = compile_expr(*av.rhs);
      if (!b->is_mut) {
        emit(Op::ImmutErr, kconst_str(tgt.token));
      } else if (b->is_cell) {
        store_cell(tgt, b->slot, rhs);
      } else {
        store_into(b->slot, rhs);
      }
      patch_to_here(skip);
      return read_binding(tgt, *b);
    }

    Op op = compound_op(ast, base);

    auto rhs = compile_expr(*av.rhs);
    ExprResult cur;
    if (b) {
      cur = read_binding(tgt, *b);
    } else {
      int32_t t = alloc_temp(tgt);
      emit(Op::NsGet, t, kconst_str(tgt.token));
      cur = {t, true};
    }
    int32_t t = alloc_temp(ast);
    emit(op, t, cur.slot, rhs.slot);
    if (!b || !b->is_mut) {
      // Runtime ImmutableError after the step ran (builtins are immutable
      // root bindings); the step result strands like any throw-abandoned
      // temp (conservative backstop).
      emit(Op::ImmutErr, kconst_str(tgt.token));
      return {t, false};  // unreachable
    }
    if (b->is_cell) store_cell(tgt, b->slot, {t, true});
    else store_into(b->slot, {t, true});
    return read_binding(tgt, *b);
  }

  // The compound-step op table, shared by the scalar and index forms;
  // anything else (`@=`) is out of slice.
  Op compound_op(const peg::Ast& ast, std::string_view base) {
    if (base == "+") return Op::Add;
    if (base == "-") return Op::Sub;
    if (base == "*") return Op::Mul;
    if (base == "/") return Op::Div;
    if (base == "%") return Op::Mod;
    if (base == "**") return Op::Pow;
    reject(ast, std::format("operator '{}='", base));
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
    if (av.is_let || av.is_mut) reject(ast, "declaring a complex target");
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
    if (fin.original_tag != "INDEX"_) reject(fin, "property assignment");

    auto chain_prefix = [&] {
      auto recv = compile_expr(*ast.nodes[av.lvaloff]);
      for (size_t i = av.lvaloff + 1; i < end; ++i) {
        const auto& post = *ast.nodes[i];
        if (post.original_tag == "ARGUMENTS"_)
          recv = compile_call_step(ast, post, recv);
        else if (post.original_tag == "INDEX"_)
          recv = compile_index_read(ast, post, recv, Op::Index);
        else if (post.original_tag == "DOT"_) {
          // Intermediate `.p` / `.m(...)` in a receiver prefix
          // (`d.next.field[0] = v`) — plain rvalue reads, the same steps the
          // chain fold uses. `?.` cannot appear here: the grammar has no
          // safe-navigating lvalue.
          if (i + 1 < end && ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
            recv = compile_method_call(ast, post, *ast.nodes[i + 1], recv);
            ++i;
          } else {
            recv = compile_property_read(ast, post, recv);
          }
        } else
          reject(post, "call chain");
      }
      return recv;
    };

    if (av.compound && av.op_base == "??") {
      auto recv = chain_prefix();
      auto key = compile_expr(fin);
      int32_t cur = alloc_temp(ast);
      emit(Op::IndexCo, cur, recv.slot, key.slot);
      size_t skip = emit(Op::JumpIfNotNil, cur);
      auto rhs = compile_expr(*av.rhs);
      emit(Op::IndexSet, recv.slot, key.slot, rhs.slot);
      store_into(cur, rhs);
      patch_to_here(skip);
      return {cur, true};
    }
    if (av.compound) {
      Op op = compound_op(ast, av.op_base);
      auto rhs = compile_expr(*av.rhs);
      auto recv = chain_prefix();
      auto key = compile_expr(fin);
      int32_t cur = alloc_temp(ast);
      emit(Op::IndexWr, cur, recv.slot, key.slot);
      int32_t t = alloc_temp(ast);
      emit(op, t, cur, rhs.slot);
      emit(Op::IndexSet, recv.slot, key.slot, t);
      return {t, true};
    }
    auto rhs = compile_expr(*av.rhs);
    auto recv = chain_prefix();
    auto key = compile_expr(fin);
    emit(Op::IndexSet, recv.slot, key.slot, rhs.slot);
    return rhs;
  }

  void compile_for(const peg::Ast& ast) {
    using namespace peg::udl;
    auto fv = culebra::view_for(ast);
    if (fv.nobreak) reject(ast, "nobreak");
    const auto& id = *fv.binding;
    if (id.tag != "IDENTIFIER"_ || !id.is_token)
      reject(id, "pattern loop binding");
    if (id.token == "_") reject(id, "sink loop binding");
    if (fv.iter->tag != "RANGE"_) reject(*fv.iter, "non-range iterable");
    auto lay = culebra::decode_range_layout(*fv.iter);
    if (!lay.start || !lay.end) reject(*fv.iter, "open-ended range");

    // A captured loop variable gets a fresh cell each iteration (the
    // interp's per-iteration scope): ForLoop keeps writing a hidden plain
    // slot, and the body opens with a CellNew from it — so every closure
    // made in iteration N holds iteration N's value.
    bool cell = info_->captured_locals.contains(std::string(id.token));

    push_scope();
    int32_t base = alloc_slot(ast, "(for.cur)");
    alloc_slot(ast, "(for.end)");
    alloc_slot(ast, "(for.step)");
    alloc_slot(ast, "(for.done)");
    int32_t var = alloc_slot(id, cell ? "(for.val)" : std::string(id.token));
    int32_t bind = var;
    if (cell) {
      bind = alloc_slot(id, std::string(id.token));
      mark_cell_slot(bind);
    }

    // Endpoints evaluate before the binding exists, in source order, with
    // errors attributed to the range expression — same as both backends.
    stamp(*fv.iter);
    store_into(base + 0, compile_expr(*lay.start), /*dst_is_fresh=*/true);
    store_into(base + 1, compile_expr(*lay.end), /*dst_is_fresh=*/true);
    if (lay.step) {
      store_into(base + 2, compile_expr(*lay.step), /*dst_is_fresh=*/true);
    } else {
      emit(Op::LoadConst, base + 2, kconst_long(1));
    }
    size_t prep = emit(Op::ForPrep, base);

    scopes_.back().bindings.push_back({std::string(id.token), bind, false, cell});
    loops_.push_back({next_slot_, defer_scopes_.size(), {}, {}});
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
  }

  void compile_while(const peg::Ast& ast) {
    auto wv = culebra::view_while(ast);
    if (wv.init) reject(*wv.init, "while init clause");
    if (wv.nobreak) reject(ast, "nobreak");

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

    loops_.push_back({next_slot_, defer_scopes_.size(), {}, {}});
    compile_block(*wv.body);
    emit(Op::Jump, static_cast<int32_t>(top_ix));
    size_t exit_ix = chunk_.code.size();
    patch_jump(exit_jump, exit_ix);

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps) patch_jump(j, top_ix);
    for (size_t j : lc.break_jumps) patch_jump(j, exit_ix);
    loops_.pop_back();
  }

  // The stdlib names the slice reaches: kBuiltinFns' bare native globals,
  // resolvable through culebra_runtime_namespace_get with no preamble
  // splice. The effects primitives are excluded (their emitting transforms
  // are rejected); lazy source modules (Time, assert_*, ...) need the
  // preamble, so they fall through to the unresolved-identifier reject.
  static bool is_stdlib_global(std::string_view name) {
    if (name.starts_with("__eff_")) return false;
    for (const auto& m : kBuiltinFns)
      if (name == m.name) return true;
    return false;
  }

  // The namespace VALUES the slice reaches: the natively built ones, taken
  // straight from the resolver's own predicate (kNsMethods / wrap.h classes /
  // constant groups) so the two cannot drift. The lazy source modules (Time,
  // Regex, Term, ...) are deliberately absent — their objects are produced by
  // preamble builder closures the VM lane never splices, so they stay on the
  // unresolved-identifier reject, same as their bare functions.
  static bool is_stdlib_namespace(std::string_view name) {
    return _is_known_ns(name);
  }

  // Direct 1-positional-arg `println(<expr>)` — the dedicated-op peephole,
  // mirroring the JIT's own direct-println emit (stdlib_jit.h), so both
  // compiled lanes skip the resolver + closure invoke for the common shape.
  // Every other shape (bare `println()`, wrong arity, kwargs, println as a
  // value) takes the generic NsGet + Call route and the runtime's own
  // diagnostics.
  bool is_direct_println(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.size() != 2 || ast.nodes[0]->tag != "IDENTIFIER"_ ||
        ast.nodes[0]->token != "println" || lookup("println") ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_ ||
        ast.nodes[1]->nodes.size() != 1)
      return false;
    const auto& a0 = *ast.nodes[1]->nodes[0];
    return a0.tag != "KWARG"_ && a0.original_tag != "KWARG"_ &&
           a0.tag != "KWARG_SPLAT"_ && a0.original_tag != "KWARG_SPLAT"_;
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
    auto res = compile_expr(*ast.nodes[0]);
    for (size_t i = 1; i < ast.nodes.size(); ++i) {
      const auto& post = *ast.nodes[i];
      if (post.original_tag == "ARGUMENTS"_)
        res = compile_call_step(ast, post, res);
      else if (post.original_tag == "INDEX"_)
        res = compile_index_read(ast, post, res, Op::Index);
      else if (post.original_tag == "SAFE_INDEX"_) {
        nil_jumps.push_back(emit(Op::JumpIfNil, res.slot));
        res = compile_index_read(ast, post, res, Op::Index);
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
    reject_fn_introspection(post);
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
  // `.name` / `.params` / `.return_type` resolve a Function receiver's
  // signature out of the JitParamMeta side table, which is keyed by fn_ptr —
  // and the executor's closures all share one fn_ptr (Exec::trampoline), so
  // there is nothing per-function to key on. Rejected by name on every
  // receiver rather than answered wrong on a Function one; no object the
  // slice can build carries a field of these names anyway.
  void reject_fn_introspection(const peg::Ast& post) {
    if (JIT::fn_introspection_name(post.token))
      reject(post, std::format("function introspection '{}'", post.token));
  }

  void reject_out_of_slice_method(const peg::Ast& post,
                                  const BMethSpec* spec) {
    reject_fn_introspection(post);
    std::string_view name = post.token;
    // A built-in that subsumes its global (`to_string`) needs no UFCS arm:
    // the built-in performs the same conversion on every receiver the global
    // would have taken. Only the stdlib half of the test is exempt — a name
    // the user declared shadows the built-in on both backends, and that stays
    // out of slice.
    bool subsumes = spec && spec->subsumes_global;
    if (lookup(name) || (is_stdlib_global(name) && !subsumes))
      reject(post, std::format("UFCS candidate '{}'", name));
    if (spec) return;
    if (culebra::is_builtin_method_name(name))
      reject(post, std::format("built-in method '{}'", name));
    if (name == "drop" || name == "parameters")
      reject(post, std::format("'{}' dispatch", name));
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
    const BMethSpec* spec = bmeth_lookup(post.token, args.nodes.size());
    reject_out_of_slice_method(post, spec);
    reject_kwargs(args);
    if (spec) return compile_builtin_method(at, post, args, recv, *spec);
    StampGuard pos(*this, at);
    int32_t callee = alloc_temp(at);
    emit(Op::PropRaw, callee, recv.slot, kconst_str(post.token));
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    alloc_temp(at);             // [0] = the receiver
    for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
    store_into(base, recv, /*dst_is_fresh=*/true);
    for (int32_t i = 0; i < argc; i++)
      store_into(base + 1 + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(at);
    emit(Op::CallM, t, callee, base, argc);
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
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    alloc_temp(at);             // [0] = the gate (user method or sentinel)
    alloc_temp(at);             // [1] = the receiver
    for (int32_t i = 0; i < spec.nargs; i++)
      alloc_temp(i < argc ? *args.nodes[i] : at);
    emit(Op::MethGate, base, recv.slot, kconst_str(post.token),
         static_cast<int32_t>(spec.id));
    store_into(base + 1, recv, /*dst_is_fresh=*/true);
    for (int32_t i = 0; i < argc; i++)
      store_into(base + 2 + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    // Every argument runs before any is bound — the interp binder's order,
    // so the checks come as one run after the whole list, each still stamped
    // at its own argument.
    for (int32_t i = 0; i < argc; i++) {
      if (spec.params[i] == BParam::Any) continue;  // undeclared: no check
      StampGuard arg_pos(*this, *args.nodes[i]);
      // The spec's own index is the whole operand: type, parameter name and
      // the receivers the check covers all come back out of the table.
      emit(Op::ChkParam, base + 2 + i, base, bmeth_spec_index(spec), i);
    }
    // The omitted optional argument: the interp's declared default, so the
    // op's arity is fixed per built-in and needs no absent-argument arm.
    if (spec.nargs > argc)
      emit(Op::LoadConst, base + 2 + argc, kconst_str(spec.def));
    int32_t t = alloc_temp(at);
    emit(Op::BMeth, t, base, static_cast<int32_t>(spec.id), spec.nargs);
    return {t, true};
  }

  // Positional-only argument lists: the runtime kwarg resolver is out of
  // slice, so both call forms turn a keyword away at compile time.
  void reject_kwargs(const peg::Ast& args) {
    using namespace peg::udl;
    for (const auto& a : args.nodes) {
      if (a->tag == "KWARG"_ || a->original_tag == "KWARG"_)
        reject(*a, "keyword argument");
      if (a->tag == "KWARG_SPLAT"_ || a->original_tag == "KWARG_SPLAT"_)
        reject(*a, "kwargs splat");
    }
  }

  // One argument-list postfix: positional args in a contiguous run of
  // owned temps (the JitFn ABI's arg slab), one Call op. Evaluation order
  // is callee first, then args left to right — both backends' order.
  ExprResult compile_call_step(const peg::Ast& ast, const peg::Ast& args,
                               ExprResult callee) {
    reject_kwargs(args);
    int32_t argc = static_cast<int32_t>(args.nodes.size());
    int32_t base = next_slot_;  // alloc_raw is sequential: a contiguous run
    for (int32_t i = 0; i < argc; i++) alloc_temp(*args.nodes[i]);
    for (int32_t i = 0; i < argc; i++)
      store_into(base + i, compile_expr(*args.nodes[i]),
                 /*dst_is_fresh=*/true);
    int32_t t = alloc_temp(ast);
    emit(Op::Call, t, callee.slot, base, argc);
    return {t, true};
  }

  // One `[k]` postfix applied to `recv`. `op` picks the read flavor: Index
  // for an rvalue read, IndexWr/IndexCo for compile_assign_index's
  // write-context reads. A range key — literal (`xs[1..3]`) or stored —
  // rides the same Index op: its runtime is-range dispatch slices before
  // the receiver arms (emit_index_step's order). The write-context reads
  // stay point-only like the JIT's compound dispatch: a range key falls
  // into their Long-key check ("expected Long, got Object"). The op is
  // stamped at the enclosing node `at`, where both backends anchor every
  // index error.
  ExprResult compile_index_read(const peg::Ast& at, const peg::Ast& post,
                                ExprResult recv, Op op) {
    auto key = compile_expr(post);
    StampGuard pos(*this, at);
    int32_t t = alloc_temp(at);
    emit(op, t, recv.slot, key.slot);
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
    if (iv.init) reject(*iv.init, "if init clause");
    int32_t res = alloc_temp(ast);
    std::vector<size_t> end_jumps;
    size_t i = iv.arm_off;
    for (; i + 1 < ast.nodes.size(); i += 2) {
      auto cond = compile_expr(*ast.nodes[i]);
      size_t skip = emit(Op::JumpIfFalse, cond.slot);
      compile_block_into(*ast.nodes[i + 1], res);
      end_jumps.push_back(emit(Op::Jump));
      patch_to_here(skip);
    }
    if (i < ast.nodes.size()) {  // trailing else block
      compile_block_into(*ast.nodes[i], res);
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
      reject(op_node, std::format("operator '{}'", t));
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

  // `try BODY catch name HANDLER` as an expression. The body's instructions
  // form an EhRegion; the handler opens with the region's release ladder —
  // every slot the body allocated, the Release/CellRelease choice static per
  // slot thanks to cell-slot pinning — then binds the caught value (mutable,
  // the interp's catch-binding default) and runs into the same result slot.
  // Normal-path exits (fall-through, break/continue/return crossing the
  // region) release through the regular scope machinery; the ladder only
  // runs on the exception path, where every emit is nil-safe.
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
    int32_t wm = next_slot_;
    auto start = static_cast<uint32_t>(chunk_.code.size());
    // The body inline (compile_block_into minus its own DeferScope): the
    // region must END before the body's fall-through defer run, because a
    // defer throwing at the try body's NORMAL exit escapes this catch
    // (interp runs run_deferred(tryEnv) outside its try/catch pair), while
    // one throwing at a NESTED scope's exit — still inside the region — is
    // caught here, exactly as the interp's nesting has it.
    if (body_scope_defer) defer_scopes_.push_back(rmark);
    push_scope();
    predeclare_multifns(*ast.nodes[0]);
    compile_body_into(*ast.nodes[0], res);
    auto end = static_cast<uint32_t>(chunk_.code.size());
    if (body_scope_defer) {
      emit(Op::DeferRunTo, rmark);
      defer_scopes_.pop_back();
    }
    pop_scope();
    size_t end_jump = emit(Op::Jump);
    auto handler = static_cast<uint32_t>(chunk_.code.size());
    // Handler: pending defers first (they may still read captured cells),
    // then the region's release ladder, then the catch binding. A defer
    // throwing HERE is outside [start, end): it propagates to the next
    // region out / the frame, never back into this catch (interp order).
    if (rmark >= 0) emit(Op::DeferRunTo, rmark);
    for (int32_t s = high_water_ - 1; s >= wm; --s)
      emit(slot_cell_[s] ? Op::CellRelease : Op::Release, s);
    push_scope();
    auto name = std::string(id.token);
    if (is_sink_name(name)) {
      emit(Op::Release, caught);  // `catch _`: drop the payload's +1
    } else {
      bool cell = info_->captured_locals.contains(name);
      int32_t e = alloc_slot(id, name);
      if (cell) {
        emit(Op::CellNew, e, caught);
        mark_cell_slot(e);
      } else {
        emit(Op::Take, e, caught);
      }
      scopes_.back().bindings.push_back({name, e, /*is_mut=*/true, cell});
    }
    // The catch body is its own defer scope (scan_eh_defer keys the node);
    // handler code sits outside the region, so its defers behave like any
    // scope's — a throwing one propagates outward, past this try.
    compile_block_into(*ast.nodes[2], res, /*defer_key=*/ast.nodes[2].get());
    pop_scope();
    patch_to_here(end_jump);
    chunk_.eh.push_back({start, end, handler, caught});
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
    if (mv.init) reject(*mv.init, "match init clause");
    int32_t res = alloc_temp(ast);
    int32_t subj = alloc_temp(ast);
    store_into(subj, compile_expr(*mv.subject), /*dst_is_fresh=*/true);
    std::vector<size_t> end_jumps;
    for (const auto& arm : mv.arms->nodes) {
      // arm->nodes: PATTERN (GUARD)? body
      const auto& pat = *arm->nodes[0];
      push_scope();
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
      bool cell = info_->captured_locals.contains(name);
      int32_t slot = alloc_slot(ident, name);
      if (cell) {
        store_new_cell(at, slot, v);
        mark_cell_slot(slot);
      } else {
        store_into(slot, v, /*dst_is_fresh=*/true);
      }
      scopes_.back().bindings.push_back({name, slot, is_mut, cell});
      return;
    }
    const Binding* b = lookup(name);
    // A name nothing visible holds would be declared by the interp; the VM
    // rejects that for bare `x = v` too (compile_assignment), so the two
    // assignment forms stay on the same slice boundary.
    if (!b) reject(ident, "assignment to an undeclared name");
    if (!b->is_mut) {
      emit(Op::ImmutErr, kconst_str(name));  // at the statement's stamp
      return;  // never falls through
    }
    if (b->is_cell) store_cell(at, b->slot, v);
    else store_into(b->slot, v);
  }

  // Emits the tests for one leaf pattern: fall through on match, jump (via
  // `fail`) on mismatch. Bindings are NOT emitted here — the caller binds
  // after the whole pattern passed, which is what makes the fail edges
  // release-free.
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
    auto tag_gate = [&](const std::vector<int8_t>& tags) {
      std::vector<size_t> ok;
      for (int8_t t : tags)
        ok.push_back(emit(Op::JumpIfTag, subj, 0, t));
      fail.push_back(emit(Op::Jump));
      for (size_t ix : ok) patch_to_here(ix);
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
      case "TYPED_IDENT"_: {
        auto tags = type_name_tags(*pat.nodes[1]);
        if (!tags.empty()) tag_gate(tags);
        return;
      }
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
      default:
        reject(pat, std::format("pattern '{}'", pat.name));
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
  void compile_obj_pattern_test(const peg::Ast& pat, int32_t subj,
                                std::vector<size_t>& fail) {
    using namespace peg::udl;
    std::vector<size_t> ok;
    ok.push_back(emit(Op::JumpIfTag, subj, 0, TAG_OBJECT));
    fail.push_back(emit(Op::Jump));
    for (size_t ix : ok) patch_to_here(ix);
    for (const auto& entry : pat.nodes) {
      bool full = entry->tag == "OBJECT_PAT_ENTRY"_;
      const peg::Ast* sub = full ? entry->nodes[1].get() : nullptr;
      auto key = full ? entry->nodes[0]->token : entry->token;
      int32_t t = alloc_temp(*entry);
      fail.push_back(emit(Op::ObjGet, t, 0, subj, kconst_str(key)));
      if (sub && pattern_has_test(*sub)) compile_pattern_test(*sub, t, fail);
    }
  }

  // Type-annotation name(s) → accepted tag set over the shared
  // _culebra_primitive_type_tag table (the JIT's TYPED_IDENT emitter reads
  // the same one): generic args stripped (`Array<Long>` gates on Array),
  // unions OR their alternatives, `Any` gates nothing (empty result).
  // Trait / user-class names need the runtime's type system — outside the
  // slice.
  std::vector<int8_t> type_name_tags(const peg::Ast& type_node) {
    auto full = type_node.token;
    std::vector<int8_t> tags;
    auto add_single = [&](std::string_view tn) -> bool {  // false: Any
      if (tn.find('<') != std::string_view::npos)
        tn = culebra::parse_generic_head(tn).outer;
      if (tn == "Any") return false;
      auto t = _culebra_primitive_type_tag(tn);
      if (!t) reject(type_node, std::format("type name '{}' in pattern", tn));
      if (std::find(tags.begin(), tags.end(), *t) == tags.end())
        tags.push_back(*t);
      return true;
    };
    if (full.find('|') != std::string_view::npos) {
      for (auto cand : culebra::split_union_types(full))
        if (!add_single(cand)) return {};  // Any alternative: no gate
    } else if (!add_single(full)) {
      return {};
    }
    return tags;
  }

  // Interpolated / triple strings over a normalized piece list: literal
  // chunks fold at compile time (consecutive ones into one constant, so a
  // pure-literal string is a single LoadConst with no concat chain — the
  // compile_triple_string fold); each `{expr}` piece renders through Disp,
  // or Fmt with its spec, and StrCat chains the accumulator left-to-right
  // (compile_interpolated_string's order). Strings aren't RC'd, so the
  // temps carry no release pressure.
  ExprResult compile_interp_pieces(
      const peg::Ast& ast, const std::vector<culebra::InterpPiece>& pieces) {
    using namespace peg::udl;
    std::string pending;
    int32_t acc = -1;
    auto cat = [&](int32_t piece) {
      if (acc < 0) {
        acc = piece;
        return;
      }
      int32_t t = alloc_temp(ast);
      emit(Op::StrCat, t, acc, piece);
      acc = t;
    };
    auto flush = [&] {
      if (pending.empty()) return;
      int32_t t = alloc_temp(ast);
      emit(Op::LoadConst, t, kconst_str(pending));
      pending.clear();
      cat(t);
    };
    for (const auto& p : pieces) {
      if (!p.expr) {
        pending += culebra::decode_interpolated_content(p.text);
        continue;
      }
      flush();
      const peg::Ast& node = *p.expr;
      const peg::Ast* expr_node = &node;
      const peg::Ast* spec = nullptr;
      if (node.tag == "INTERP_EXPR"_) {
        expr_node = node.nodes[0].get();
        if (node.nodes.size() > 1) spec = node.nodes[1].get();
      }
      auto v = compile_expr(*expr_node);
      // SetOpPos rides the enclosing string literal's stamp (the pending
      // position here); the render op itself is stamped at the piece so
      // Fmt's spec errors report the `{expr:spec}` node.
      emit(Op::SetOpPos);
      int32_t t = alloc_temp(ast);
      StampGuard pos(*this, node);
      if (spec)
        emit(Op::Fmt, t, v.slot, kconst_str(spec->token));
      else
        emit(Op::Disp, t, v.slot);
      cat(t);
    }
    flush();
    if (acc < 0) {  // no pieces at all: the empty string
      acc = alloc_temp(ast);
      emit(Op::LoadConst, acc, kconst_str(""));
    }
    return {acc, true};
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
      case "IDENTIFIER"_: {
        const Binding* b = lookup(ast.token);
        if (b) return read_binding(ast, *b);
        if (is_stdlib_global(ast.token) || is_stdlib_namespace(ast.token)) {
          int32_t t = alloc_temp(ast);
          emit(Op::NsGet, t, kconst_str(ast.token));
          return {t, true};
        }
        reject(ast, std::format("unresolved identifier '{}'", ast.token));
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
      case "SHIFT"_: {
        auto acc = compile_expr(*ast.nodes[0]);
        for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
          auto op_tok = ast.nodes[i]->token;
          Op op;
          if (op_tok == "|") op = Op::BitOr;
          else if (op_tok == "^") op = Op::BitXor;
          else if (op_tok == "&") op = Op::BitAnd;
          else if (op_tok == "<<") op = Op::Shl;
          else if (op_tok == ">>") op = Op::Shr;
          else reject(*ast.nodes[i], std::format("operator '{}'", op_tok));
          auto rhs = compile_expr(*ast.nodes[i + 1]);
          int32_t t = alloc_temp(ast);
          emit(op, t, acc.slot, rhs.slot);
          acc = {t, true};
        }
        return acc;
      }
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_: {
        auto acc = compile_expr(*ast.nodes[0]);
        for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
          auto op_tok = ast.nodes[i]->token;
          Op op;
          if (op_tok == "+") op = Op::Add;
          else if (op_tok == "-") op = Op::Sub;
          else if (op_tok == "*") op = Op::Mul;
          else if (op_tok == "/") op = Op::Div;
          else if (op_tok == "%") op = Op::Mod;
          else reject(*ast.nodes[i], std::format("operator '{}'", op_tok));
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
      case "ASSIGNMENT"_:  // expression position: `let r = (w += 2)`
        return compile_assignment(ast);
      case "DESTRUCTURE_ASSIGN"_:
        return compile_destructure_assign(ast);
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
      case "FUNCTION"_:
      case "LAMBDA"_: {
        int32_t idx = compile_fn_chunk(ast, ast.nodes[0].get(), *ast.nodes[1]);
        int32_t t = alloc_temp(ast);
        emit(Op::MakeClosure, t, idx);
        return {t, true};
      }
      case "CALL"_: {
        if (is_direct_println(ast)) {
          emit(Op::Println, compile_expr(*ast.nodes[1]->nodes[0]).slot);
          int32_t t = alloc_temp(ast);
          emit(Op::LoadConst, t, kconst({TAG_NIL, 0}));
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
        reject(ast, std::format("expression '{}'", ast.name));
    }
  }
};

inline std::string dump(const Chunk& c) {
  static constexpr const char* kNames[] = {
      "LoadConst", "Move",      "Take",       "Retain",       "Release",
      "Neg",       "Not",       "Add",        "Sub",          "Mul",
      "Div",       "Mod",       "Pow",        "BitAnd",       "BitOr",
      "BitXor",    "Shl",       "Shr",        "BitNot",
      "Eq",        "Ne",        "Lt",
      "Le",        "Gt",        "Ge",         "ArrayNew",     "ArrayAppend",
      "ArrayPush", "ArrayExtend", "ArrayResize",
      "TupleNew",  "TuplePush", "SetNew",     "SetAdd",
      "RangeNew",  "ChkLong",   "NilChk",
      "Index",     "IndexWr",   "IndexCo",    "IndexSet",
      "PropVal",   "BareMethChk", "MethGate", "ChkParam", "BMeth", "PropRaw",
      "SeqChk",    "SeqGet",    "SeqRest",    "ObjGet",       "DestrErr",
      "Jump",      "JumpIfFalse", "JumpIfTrue", "JumpIfNotNil", "JumpIfNil",
      "JumpIfTag",
      "MakeClosure", "Call",    "CallM",      "Ret",
      "CellNew",   "CellGet",   "CellSet",    "CellRelease",  "BindCapture",
      "ImmutErr",  "UnboundErr", "MultifnReg", "NsGet",
      "SetOpPos",  "Disp",      "Fmt",        "StrCat",
      "Throw",     "DeferMark",  "DeferPush",    "DeferRunTo",
      "ForPrep",   "ForLoop",   "Println",    "Safepoint",    "Halt"};
  static_assert(std::size(kNames) == static_cast<size_t>(Op::Halt) + 1);
  std::string out;
  out += std::format("; slots: {}\n", c.num_slots);
  if (!c.capture_src_slots.empty()) {
    out += "; captures from creator slots:";
    for (auto s : c.capture_src_slots) out += std::format(" r{}", s);
    out += "\n";
  }
  for (size_t s = 0; s < c.slot_names.size(); ++s)
    out += std::format(";   r{} = {}\n", s, c.slot_names[s]);
  for (size_t i = 0; i < c.code.size(); ++i) {
    const auto& in = c.code[i];
    auto [line, col] = chunk_pos_at(c, i);
    out += std::format("{:4}: {:<12} {:4} {:4} {:4} {:4}   ; {}:{}\n", i,
                       kNames[static_cast<size_t>(in.op)], in.a, in.b, in.c,
                       in.d, line, col);
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
      out += std::format("; == fn {} (arity {}) ==\n", i, c.arity);
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
  static void run(VmProgram& p) {
    p.descs.resize(p.chunks.size());
    p.desc_cells.resize(p.chunks.size());
    for (size_t i = 0; i < p.chunks.size(); ++i) {
      p.descs[i] = {&p, static_cast<int32_t>(i)};
      p.desc_cells[i] = culebra_runtime_cell_new(
          TAG_LONG, reinterpret_cast<int64_t>(&p.descs[i]));
      // desc_cells is a C++-held root the conservative stack scan cannot
      // see (a heap vector) — pin for the run, the namespace-cache pattern.
      _gc_heap().pin(p.desc_cells[i]);
    }
    // Drop the pin and the program's +1 on every exit path; a closure still
    // alive on the uncaught-throw path holds its own retain (GC backstop
    // territory).
    struct CellGuard {
      VmProgram& p;
      ~CellGuard() {
        for (auto* cl : p.desc_cells) {
          _gc_heap().unpin(cl);
          culebra_runtime_cell_release(cl);
        }
        p.desc_cells.clear();
      }
    } guard{p};
    try {
      run_frame(p, 0, nullptr, 0, nullptr);
    } catch (const CulebraException& e) {
      // Uncaught user throw: format first, then consume the carrier's
      // reference — JIT::exec's boundary, so main.cc prints the same
      // "uncaught: ..." on every lane.
      auto s = _culebra_uncaught_display(e.tag, e.data);
      _culebra_value_release_impl(e.tag, e.data);
      throw std::runtime_error(std::format("uncaught: {}", s));
    } catch (CulebraError& e) {
      // Backfill a positionless error from the published op position at
      // the engine boundary — JIT::exec's rule (the interp stamps at its
      // eval() boundary; an Interrupted stays 0:0 on every lane).
      if (e.kind != "Interrupted") _jit_backfill_op_pos(e);
      throw;
    }
  }

  // JitFn-ABI entry: native code (and the executor's own Call op) reaches a
  // VM function through the closure's fn_ptr like any other closure; the
  // descriptor in captures[0] says which chunk to interpret. The receiver
  // scalars are unused until methods enter the slice.
  static void trampoline(JitValue* ret, JitClosure* cls, int8_t /*self_tag*/,
                         int64_t /*self_data*/, int64_t n_args,
                         JitValue* args) {
    auto* d = reinterpret_cast<const VmFnDesc*>(cls->captures[0]->value.data);
    *ret = run_frame(*d->prog, d->chunk, cls, n_args, args);
  }

  static JitValue run_frame(const VmProgram& p, int32_t chunk_idx,
                            JitClosure* cls, int64_t n_args, JitValue* args) {
    const Chunk& c = p.chunks[chunk_idx];
    if (c.num_slots > kMaxSlots)
      throw CulebraError("VmError", "--vm: frame too large");
    // Nil only the frame's live window (zero-init == {TAG_NIL, 0}); the
    // executor never touches slots >= num_slots, and the conservative GC
    // scan tolerates stack garbage above them.
    JitValue regs[kMaxSlots];
    std::memset(regs, 0, sizeof(JitValue) * static_cast<size_t>(c.num_slots));
    if (chunk_idx != 0) {
      // Param binding, mirroring the JIT prologue: too few args raises the
      // interp's ArityError at the published call site; extras drop (their
      // +1 released) since __ARGS__ is outside the slice. The recursion
      // guard runs after binding (TypeError-before-RecursionError order),
      // and `leave` only on the normal return — unwinding skips it, like
      // the JIT's frames.
      if (n_args < c.arity) {
        for (int64_t i = 0; i < n_args; ++i)
          culebra_runtime_value_release(static_cast<int8_t>(args[i].tag),
                                        args[i].data);
        // The runtime helper owns the diagnostic (message + prefer-the-
        // published-call-site policy) for every lane; cold path.
        std::vector<const char*> names;
        names.reserve(c.param_names.size());
        for (const auto& nm : c.param_names) names.push_back(nm.c_str());
        culebra_runtime_arity_missing(names.data(), n_args, 0, 0);
      }
      for (int32_t i = 0; i < c.arity; ++i) regs[i] = args[i];
      for (int64_t i = c.arity; i < n_args; ++i)
        culebra_runtime_value_release(static_cast<int8_t>(args[i].tag),
                                      args[i].data);
      if (c.fn_slot >= 0) {
        regs[c.fn_slot] =
            JitValue{TAG_FUNC, reinterpret_cast<int64_t>(cls)};
        culebra_runtime_value_retain(TAG_FUNC, regs[c.fn_slot].data);
      }
    }
    // A try handler restores the recursion count to this frame's own level
    // (frames unwound between the throw and the handler never ran their
    // `leave`) — the JIT's try.rec snapshot, hoisted to the prologue since
    // the depth is constant within a frame.
    int64_t frame_depth = chunk_idx != 0
                              ? culebra_runtime_recursion_enter()
                              : culebra_runtime_recursion_depth();
    const Insn* code = c.code.data();
    size_t pc = 0;
    // Dispatch, re-entering at a region's handler when a throw lands inside
    // one. Classification shares the JIT landingpad's carrier machinery:
    // try_translate materializes a CulebraError as an error Object, a user
    // throw already carries a value, and a foreign exception leaves
    // is_throw=0 and keeps unwinding (as does any throw outside a region).
    for (;;) {
      try {
        return dispatch(p, c, code, regs, pc, chunk_idx, cls);
      } catch (...) {
        const Chunk::EhRegion* r = nullptr;
        for (const auto& e : c.eh)
          if (e.start <= pc && pc < e.end) {
            r = &e;  // innermost-first table order: first hit wins
            break;
          }
        if (!r) {
          // No region: run the frame's pending defers before unwinding out
          // (the observable slice of the JIT's frame cleanup ladder; slot
          // releases stay on the GC backstop). Ladder order around the run:
          // restore this frame's depth first — the unwound callees never ran
          // their `leave`, and a defer body's own calls must count from
          // here — then uncount the frame. The tag check is belt-and-
          // braces: DeferMark is the chunk's first instruction.
          if (c.defer_mark_slot >= 0 &&
              regs[c.defer_mark_slot].tag == TAG_LONG) {
            culebra_runtime_recursion_restore(frame_depth);
            culebra_runtime_defer_run_to(regs[c.defer_mark_slot].data);
            if (chunk_idx != 0)
              culebra_runtime_recursion_restore(frame_depth - 1);
          }
          throw;
        }
        culebra_runtime_try_translate();
        if (!culebra_runtime_get_is_throw()) throw;
        culebra_runtime_clear_is_throw();
        culebra_runtime_recursion_restore(frame_depth);
        regs[r->caught_slot] = JitValue{culebra_runtime_get_thrown_tag(),
                                        culebra_runtime_get_thrown_data()};
        pc = static_cast<size_t>(r->handler);
      }
    }
  }

  // The dispatch loop proper: runs until Ret/Halt, or unwinds with `pc`
  // still at the faulting instruction (run_frame's catch consults it).
  static JitValue dispatch(const VmProgram& p, const Chunk& c,
                           const Insn* code, JitValue* regs, size_t& pc,
                           int32_t chunk_idx, JitClosure* cls) {
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

    for (;;) {
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
            auto [line, col] = chunk_pos_at(c, pc);
            auto lt = static_cast<int8_t>(l.tag);
            auto rt = static_cast<int8_t>(r.tag);
            switch (in.op) {
              case Op::Add:
                out = culebra_runtime_num_add_borrow(lt, l.data, rt, r.data,
                                                     line, col);
                break;
              case Op::Sub:
                out = culebra_runtime_num_sub_borrow(lt, l.data, rt, r.data,
                                                     line, col);
                break;
              case Op::Mul:
                out = culebra_runtime_num_mul_borrow(lt, l.data, rt, r.data,
                                                     line, col);
                break;
              case Op::Div:
                out = culebra_runtime_num_div_borrow(lt, l.data, rt, r.data,
                                                     line, col);
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
          regs[in.a] = culebra_runtime_num_pow_borrow(
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
          // scratch entry per execution is all it needs. emit_property_get's
          // fn_mode branch has no mirror here — the three introspection names
          // are a compile-time reject, so `view` is always the borrowed
          // object-slot form.
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
          const BMethSpec& gate = bmeth_gate_spec(static_cast<BMeth>(in.d));
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
            culebra_runtime_set_call_site(line, col);
            if (gate.tag != TAG_FUNC)
              culebra_runtime_type_error_typed(
                  line, col, "Function", static_cast<int8_t>(gate.tag));
            JitValue r;
            try {
              r = _jit_invoke(reinterpret_cast<JitClosure*>(gate.data),
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
              reinterpret_cast<void*>(&trampoline), 1 + n,
              static_cast<size_t>(f.arity));
          culebra_runtime_cell_retain(p.desc_cells[in.b]);
          mc->captures[0] = p.desc_cells[in.b];
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
          const JitValue& callee = regs[in.b];
          auto [line, col] = chunk_pos_at(c, pc);
          culebra_runtime_set_call_site(line, col);
          if (callee.tag != TAG_FUNC) {
            // No Object/class values exist in the slice yet, so the JIT's
            // __call__/ctor probes cannot hit — the plain TypeError is the
            // whole cold path for now.
            culebra_runtime_type_error_typed(
                line, col, "Function", static_cast<int8_t>(callee.tag));
          }
          JitValue r;
          try {
            r = _jit_invoke(reinterpret_cast<JitClosure*>(callee.data),
                            JitValue{TAG_NO_SELF, 0}, in.d,
                            in.d ? &regs[in.c] : nullptr);
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
          culebra_runtime_set_call_site(line, col);
          if (callee.tag != TAG_FUNC) {
            // Where a missing method lands: the property read gave nil, so
            // this is the interp's "expected Function, got Nil". The receiver
            // and args stay register-owned — nothing has been handed over
            // yet, so the enclosing ladder is still their releaser.
            culebra_runtime_type_error_typed(
                line, col, "Function", static_cast<int8_t>(callee.tag));
          }
          // The run is receiver-then-args; the callee consumes all of it.
          // culebra_runtime_call_receiver is not mirrored: it only rewrites a
          // lowered state object's promoted body local into "no receiver",
          // and those protos come from the generator / effects transforms the
          // slice rejects outright.
          JitValue r;
          try {
            r = _jit_invoke(reinterpret_cast<JitClosure*>(callee.data),
                            regs[in.c], in.d,
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
        case Op::Ret: {
          JitValue rv = regs[in.a];
          if (chunk_idx != 0) culebra_runtime_recursion_leave();
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
          if (regs[in.a].tag == TAG_NO_SELF) {
            auto [line, col] = chunk_pos_at(c, pc);
            auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
            culebra_runtime_throw_error(
                "NameError",
                std::format("undefined variable '{}'", nm).c_str(),
                line, col);
          }
          ++pc;
          break;
        case Op::MultifnReg: {
          const Chunk& f = p.chunks[in.d];
          // Arity-only dispatch: null type strings, the chunk's stable
          // param-name storage for kwarg coverage (unused in the slice).
          std::vector<const char*> names;
          std::vector<const char*> types(f.param_names.size(), nullptr);
          names.reserve(f.param_names.size());
          for (const auto& n : f.param_names) names.push_back(n.c_str());
          auto n = static_cast<int64_t>(f.param_names.size());
          auto* disp = culebra_runtime_multifn_register_and_install(
              reinterpret_cast<const char*>(c.consts[in.c].data),
              reinterpret_cast<JitClosure*>(regs[in.b].data),
              n ? types.data() : nullptr, n, /*variadic=*/0,
              /*min_arity=*/n, n ? names.data() : nullptr);
          regs[in.b] = JitValue{TAG_NIL, 0};  // the registry took the +1
          regs[in.a] = JitValue{TAG_FUNC, reinterpret_cast<int64_t>(disp)};
          ++pc;
          break;
        }
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
        case Op::Disp:
          regs[in.a] = JitValue{
              TAG_STRING,
              reinterpret_cast<int64_t>(culebra_runtime_value_to_display(
                  static_cast<int8_t>(regs[in.b].tag), regs[in.b].data))};
          ++pc;
          break;
        case Op::Fmt: {
          auto [line, col] = chunk_pos_at(c, pc);
          regs[in.a] = JitValue{
              TAG_STRING,
              reinterpret_cast<int64_t>(culebra_runtime_format_value(
                  static_cast<int8_t>(regs[in.b].tag), regs[in.b].data,
                  reinterpret_cast<const char*>(c.consts[in.c].data), line,
                  col))};
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
          culebra_runtime_throw(static_cast<int8_t>(v.tag), v.data);
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
        case Op::Println:
          culebra_runtime_println(static_cast<int8_t>(regs[in.a].tag),
                                  regs[in.a].data);
          ++pc;
          break;
        case Op::Safepoint:
          if (culebra_g_wake.load(std::memory_order_relaxed))
            culebra::throw_if_interrupted();
          ++pc;
          break;
        case Op::Halt:
          return JitValue{TAG_NIL, 0};
      }
    }
  }
};

// Bytecode -> LLVM IR, reusing the JIT object as the codegen context and the
// existing exec() scaffold (ORC, isolate-join and teardown-collect guards,
// uncaught-error conversion). This is the Phase 3 shape: jit.h consuming
// bytecode instead of the AST. Where the executor above calls a runtime
// helper, the lowering calls the JIT's own emitter for the same construct
// (emit_arith_step, emit_comparison_i1, value_to_bool) — one dispatch
// definition, two consumers.
struct Lowering {
  // The JitFn ABI signature (jit_value.h), spelled once for both the chunk
  // function creation and the indirect call site.
  static llvm::FunctionType* jit_fn_type(llvm::IRBuilder<>& b,
                                         llvm::Type* ptrTy) {
    return llvm::FunctionType::get(
        b.getVoidTy(),
        {ptrTy, ptrTy, b.getInt8Ty(), b.getInt64Ty(), b.getInt64Ty(), ptrTy},
        false);
  }

  static void run_program(const VmProgram& p, bool emit_llvm, int opt_level) {
    using namespace llvm;
    JIT::ensure_native_target_init();
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("vm", *ctx);
    JIT::apply_target(*mod, Triple(sys::getDefaultTargetTriple()));
    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    jit.declare_runtime_functions();
    // Registers are borrowed by the dispatch helpers; a region's release
    // ladder owns the throw path (see the Op enum's contract notes).
    jit.vm_borrow_ops_ = true;

    // One LLVM function per chunk: the top level keeps the __culebra_main
    // entry shape; function chunks get the JitFn ABI, so MakeClosure hands
    // their address to closure_new exactly like a JIT-compiled function.
    auto ptrTy = PointerType::get(*ctx, 0);
    auto jitFnTy = jit_fn_type(builder, ptrTy);
    std::vector<Function*> fns(p.chunks.size());
    fns[0] = Function::Create(FunctionType::get(builder.getVoidTy(), false),
                              Function::ExternalLinkage, "__culebra_main",
                              mod.get());
    for (size_t i = 1; i < p.chunks.size(); ++i) {
      fns[i] = Function::Create(jitFnTy, Function::InternalLinkage,
                                std::format("__vm_fn_{}", i), mod.get());
    }
    for (size_t i = 0; i < p.chunks.size(); ++i) {
      lower_chunk(jit, p, i, fns);
      // Report and stop: verifyFunction's one-argument form returns the
      // verdict and prints nothing, so ignoring it let malformed IR (an
      // i1 result compared against an i8 zero, from a runtime helper whose
      // declared return type the arm mis-read) reach the optimizer and run.
      std::string err;
      raw_string_ostream os(err);
      if (verifyFunction(*fns[i], &os))
        throw std::runtime_error("vm: lowered IR failed verification: " + err);
    }
    if (opt_level > 0) JIT::optimize_module(*mod, opt_level);
    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      JIT::exec(std::move(ctx), std::move(mod));
    }
  }

  static void lower_chunk(JIT& j, const VmProgram& p, size_t chunk_idx,
                          const std::vector<llvm::Function*>& fns) {
    using namespace llvm;
    const Chunk& c = p.chunks[chunk_idx];
    auto* fn = fns[chunk_idx];
    auto& b = j.builder_;
    auto i64Ty = b.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(j.ctx_, 0);
    bool frame_defers = c.defer_mark_slot >= 0;
    if (!c.eh.empty() || frame_defers)
      fn->setPersonalityFn(j.get_personality_fn());
    b.SetInsertPoint(BasicBlock::Create(j.ctx_, "entry", fn));
    // Per-LLVM-function JIT state, which the AST path resets through
    // CompilerStateSaver at each nested fn literal: one chunk is one
    // function, and a cleanup pad's exception slot must be an alloca in
    // THIS function's entry block.
    j.exc_slot_ = nullptr;

    // Frame-ABI arguments (function chunks only; see JitFn in jit_value.h).
    llvm::Value* retPtr = nullptr;
    llvm::Value* clsArg = nullptr;
    llvm::Value* nArgs = nullptr;
    llvm::Value* argsPtr = nullptr;
    if (chunk_idx != 0) {
      retPtr = fn->getArg(0);
      clsArg = fn->getArg(1);
      nArgs = fn->getArg(4);
      argsPtr = fn->getArg(5);
    }

    // One Value alloca per slot, nil-initialized — mem2reg turns these into
    // SSA; this is what replaces the AST path's Scope/VarSlot machinery.
    // (llvm::Value spelled out: the enclosing namespace's culebra::Value
    // wins over the using-directive.)
    std::vector<llvm::Value*> slots(c.num_slots);
    for (int32_t s = 0; s < c.num_slots; ++s) {
      slots[s] = b.CreateAlloca(j.valueType_, nullptr, c.slot_names[s]);
      b.CreateStore(j.make_nil(), slots[s]);
    }
    auto load_slot = [&](int32_t s) {
      return b.CreateLoad(j.valueType_, slots[s]);
    };

    // Function-chunk prologue, mirroring both the JIT prologue and
    // Exec::run_frame: arity guard (ArityError at the published call site),
    // param binding, overflow-arg release, the `fn` handle, and the
    // recursion guard after binding.
    if (chunk_idx != 0) {
      if (c.arity > 0) {
        auto tooFew = b.CreateICmpSLT(nArgs, b.getInt64(c.arity), "too.few");
        auto errBB = BasicBlock::Create(j.ctx_, "arity.err", fn);
        auto okBB = BasicBlock::Create(j.ctx_, "arity.ok", fn);
        b.CreateCondBr(tooFew, errBB, okBB);
        b.SetInsertPoint(errBB);
        std::vector<Constant*> nameptrs;
        for (const auto& n : c.param_names)
          nameptrs.push_back(j.get_or_create_global_str(n, ".paramname"));
        auto* namesG = j.build_str_ptr_array(nameptrs, ".paramnames");
        // The prologue's own fallback position is unused: set_call_site
        // always ran on the caller side, and arity_missing prefers it.
        b.CreateCall(j.module_->getOrInsertFunction(
                         rt::arity_missing, b.getVoidTy(), ptrTy, i64Ty,
                         i64Ty, i64Ty),
                     {namesG, nArgs, b.getInt64(0), b.getInt64(0)});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        for (int32_t i = 0; i < c.arity; ++i) {
          auto* src = b.CreateConstGEP1_64(j.valueType_, argsPtr,
                                           static_cast<uint64_t>(i));
          b.CreateStore(b.CreateLoad(j.valueType_, src), slots[i]);
        }
      }
      // Release overflow args: for (iv = arity; iv < nArgs; iv++).
      {
        auto* fromBB = b.GetInsertBlock();
        auto hdrBB = BasicBlock::Create(j.ctx_, "extras.hdr", fn);
        auto bodyBB = BasicBlock::Create(j.ctx_, "extras.body", fn);
        auto doneBB = BasicBlock::Create(j.ctx_, "extras.done", fn);
        b.CreateBr(hdrBB);
        b.SetInsertPoint(hdrBB);
        auto* iv = b.CreatePHI(i64Ty, 2, "iv");
        iv->addIncoming(b.getInt64(c.arity), fromBB);
        b.CreateCondBr(b.CreateICmpSLT(iv, nArgs), bodyBB, doneBB);
        b.SetInsertPoint(bodyBB);
        auto* src = b.CreateGEP(j.valueType_, argsPtr, iv);
        j.emit_value_release(b.CreateLoad(j.valueType_, src));
        auto* next = b.CreateAdd(iv, b.getInt64(1));
        iv->addIncoming(next, b.GetInsertBlock());
        b.CreateBr(hdrBB);
        b.SetInsertPoint(doneBB);
      }
      if (c.fn_slot >= 0) {
        auto fnVal = j.make_func(clsArg);
        b.CreateStore(fnVal, slots[c.fn_slot]);
        j.emit_value_retain(fnVal);
      }
    }
    // Region handlers (and the frame pad below) restore the recursion count
    // to the frame's own level (Exec::run_frame's frame_depth, the JIT's
    // try.rec snapshot hoisted to the prologue — the depth is constant
    // within a frame).
    llvm::Value* depthSlot = nullptr;
    {
      bool need_depth = !c.eh.empty() || frame_defers;
      llvm::Value* depth =
          chunk_idx != 0
              ? b.CreateCall(j.module_->getOrInsertFunction(
                                 rt::recursion_enter, i64Ty),
                             {}, "rec.depth")
              : (need_depth
                     ? b.CreateCall(j.module_->getOrInsertFunction(
                                        rt::recursion_depth, i64Ty),
                                    {}, "rec.depth")
                     : nullptr);
      if (need_depth) {
        IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        depthSlot = eb.CreateAlloca(i64Ty, nullptr, "eh.depth");
        b.CreateStore(depth, depthSlot);
      }
    }

    // Pass 1: every jump target opens a basic block. A conditional jump's
    // fall-through (and the insn after a terminator — dead code a
    // break/continue/return can leave) opens one too, so pass 2 never emits
    // into a terminated block.
    std::map<int32_t, BasicBlock*> blocks;
    auto mark = [&](int32_t t) {
      if (t < static_cast<int32_t>(c.code.size()) && !blocks.count(t))
        blocks[t] = BasicBlock::Create(j.ctx_, std::format("L{}", t), fn);
    };
    for (size_t i = 0; i < c.code.size(); ++i) {
      const auto& in = c.code[i];
      switch (in.op) {
        case Op::Jump:
          mark(in.a);
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::Ret:
        case Op::ImmutErr:  // lowers to a noreturn call + unreachable
        case Op::DestrErr:
        case Op::Throw:
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfNotNil:
        case Op::JumpIfNil:
        case Op::JumpIfTag:
        case Op::SeqChk:   // pattern mismatch edges, same encoding
        case Op::ObjGet:
          mark(in.b);
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::ForPrep:
          mark(in.b);
          break;
        case Op::ForLoop:
          mark(in.b);
          mark(static_cast<int32_t>(i) + 1);  // done() falls through here
          break;
        default:
          break;
      }
    }

    // Each region opens a handler block at its handler pc (the bytecode
    // release ladder lives there) and gets a landingpad prelude block,
    // filled after the main walk. Setting current_lpad_ per instruction
    // turns every may-throw helper call inside a region into an invoke —
    // emit_call's standard behavior, no lowering-specific plumbing.
    for (const auto& r : c.eh) mark(static_cast<int32_t>(r.handler));
    std::vector<BasicBlock*> lpads(c.eh.size());
    for (size_t k = 0; k < c.eh.size(); ++k)
      lpads[k] = BasicBlock::Create(j.ctx_, std::format("eh.lpad.{}", k), fn);
    // Frame-level cleanup pad (chunks with defers only): a throw outside
    // every region runs the frame's pending defers before unwinding out —
    // the observable slice of the JIT's frame cleanup ladder; slot releases
    // stay on the GC backstop, like the executor's catch-all.
    BasicBlock* framePad =
        frame_defers ? BasicBlock::Create(j.ctx_, "vm.frame.pad", fn)
                     : nullptr;
    // The JitFn hand-off, shared by Call / CallM and BMeth's user-method arm:
    // the TAG_FUNC gate, the arg slab, and the ABI call. `selfSlot < 0` is a
    // plain call (TAG_NO_SELF rides the receiver pair). The callee consumes
    // the receiver and every argument on every exit, so those slots go nil
    // BEFORE the call — the slab alloca keeps the values alive for the callee
    // and a region's release ladder cannot double-release them on the unwind
    // edge. Returns the call's result value.
    auto emit_invoke = [&](llvm::Value* calleeV, int32_t selfSlot,
                           int32_t argBase, int32_t argc, int64_t line,
                           int64_t col) -> llvm::Value* {
      b.CreateCall(j.module_->getOrInsertFunction(rt::set_call_site,
                                                  b.getVoidTy(), i64Ty, i64Ty),
                   {b.getInt64(line), b.getInt64(col)});
      auto tag = j.extract_tag(calleeV);
      auto errBB = BasicBlock::Create(j.ctx_, "call.err", fn);
      auto okBB = BasicBlock::Create(j.ctx_, "call.ok", fn);
      b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_FUNC)), okBB, errBB);
      b.SetInsertPoint(errBB);
      // Same shape as the executor's cold path: no class values exist in the
      // slice, so the JIT's __call__/ctor probes cannot hit.
      j.emit_call(j.module_->getOrInsertFunction(rt::type_error_typed,
                                                 b.getVoidTy(), i64Ty, i64Ty,
                                                 ptrTy, b.getInt8Ty()),
                  {b.getInt64(line), b.getInt64(col),
                   b.CreateGlobalString("Function"), tag});
      if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
      b.SetInsertPoint(okBB);
      auto clsPtr = b.CreateIntToPtr(j.extract_data(calleeV), ptrTy);
      auto fnFieldPtr = b.CreateStructGEP(j.closureType_, clsPtr, 1, "fn.ptr");
      auto fnPtr = b.CreateLoad(ptrTy, fnFieldPtr, "fn");
      IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      llvm::Value* slab;
      if (argc > 0) {
        slab = eb.CreateAlloca(ArrayType::get(j.valueType_, argc), nullptr,
                               "call.args");
        for (int32_t k = 0; k < argc; ++k) {
          auto* dstp = b.CreateConstGEP2_64(ArrayType::get(j.valueType_, argc),
                                            slab, 0, static_cast<uint64_t>(k));
          b.CreateStore(load_slot(argBase + k), dstp);
        }
      } else {
        slab = ConstantPointerNull::get(cast<PointerType>(ptrTy));
      }
      auto* retTmp = eb.CreateAlloca(j.valueType_, nullptr, "call.ret");
      auto selfV = selfSlot >= 0 ? load_slot(selfSlot) : nullptr;
      for (int32_t k = 0; k < argc; ++k)
        b.CreateStore(j.make_nil(), slots[argBase + k]);
      if (selfSlot >= 0) b.CreateStore(j.make_nil(), slots[selfSlot]);
      j.emit_call(jit_fn_type(b, ptrTy), fnPtr,
                  {retTmp, clsPtr,
                   selfV ? j.extract_tag(selfV) : b.getInt8(TAG_NO_SELF),
                   selfV ? j.extract_data(selfV) : b.getInt64(0),
                   b.getInt64(argc), slab});
      return b.CreateLoad(j.valueType_, retTmp);
    };

    auto lpad_for = [&](size_t pc) -> BasicBlock* {
      for (size_t k = 0; k < c.eh.size(); ++k)
        if (c.eh[k].start <= pc && pc < c.eh[k].end) return lpads[k];
      return framePad;  // innermost-first table order: first hit wins
    };

    // Pass 2: linear walk over the instructions. The chunk's position table
    // feeds the JIT's position state, so every emitter that bakes line/col
    // into calls (arith, comparisons, to_bool) attributes exactly as the
    // AST path would.
    for (size_t i = 0; i < c.code.size(); ++i) {
      if (auto it = blocks.find(static_cast<int32_t>(i)); it != blocks.end()) {
        if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(it->second);
        b.SetInsertPoint(it->second);
      }
      {
        auto [line, col] = chunk_pos_at(c, i);
        j.current_line_ = static_cast<size_t>(line);
        j.current_column_ = static_cast<size_t>(col);
      }
      j.current_lpad_ = lpad_for(i);
      const auto& in = c.code[i];
      switch (in.op) {
        case Op::LoadConst: {
          const auto& k = c.consts[in.b];
          llvm::Value* v = nullptr;
          switch (k.tag) {
            case TAG_LONG: v = j.make_long(b.getInt64(k.data)); break;
            case TAG_BOOL: v = j.make_bool(b.getInt1(k.data != 0)); break;
            case TAG_NIL: v = j.make_nil(); break;
            case TAG_NO_SELF: v = j.make_no_self(); break;  // unbound sentinel
            case TAG_FLOAT: {
              double d;
              std::memcpy(&d, &k.data, 8);
              v = j.make_float(
                  llvm::ConstantFP::get(b.getDoubleTy(), d));
              break;
            }
            case TAG_STRING: {
              // Re-emit the chunk's string constant as module .rodata (the
              // same layout), so the lowered module owns its bytes.
              v = j.make_string(j.emit_str_literal(
                  _str_sv(reinterpret_cast<const char*>(k.data))));
              break;
            }
            default:
              throw std::runtime_error("vm: unexpected const tag");
          }
          b.CreateStore(v, slots[in.a]);
          break;
        }
        case Op::Move:
          b.CreateStore(load_slot(in.b), slots[in.a]);
          break;
        case Op::Take:
          b.CreateStore(load_slot(in.b), slots[in.a]);
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        case Op::Retain:
          j.emit_value_retain(load_slot(in.a));
          break;
        case Op::Release:
          j.emit_value_release(load_slot(in.a));
          b.CreateStore(j.make_nil(), slots[in.a]);
          break;
        case Op::Neg:
          b.CreateStore(j.emit_neg_step(load_slot(in.b)), slots[in.a]);
          break;
        case Op::Not: {
          auto t = j.value_to_bool(load_slot(in.b));
          b.CreateStore(j.make_bool(b.CreateNot(t, "not")), slots[in.a]);
          break;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
          const char* op = in.op == Op::Add   ? "+"
                           : in.op == Op::Sub ? "-"
                           : in.op == Op::Mul ? "*"
                           : in.op == Op::Div ? "/"
                                              : "%";
          auto r = j.emit_arith_step(load_slot(in.b), load_slot(in.c), op);
          b.CreateStore(r, slots[in.a]);
          break;
        }
        case Op::Pow: {
          // emit_arith_step's "**" arm picks num_pow_borrow under the VM
          // operand contract (vm_borrow_ops_).
          auto r = j.emit_arith_step(load_slot(in.b), load_slot(in.c), "**");
          b.CreateStore(r, slots[in.a]);
          break;
        }
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr: {
          const char* op = in.op == Op::BitAnd   ? "&"
                           : in.op == Op::BitOr  ? "|"
                           : in.op == Op::BitXor ? "^"
                           : in.op == Op::Shl    ? "<<"
                                                 : ">>";
          auto r = j.emit_bitwise_step(load_slot(in.b), load_slot(in.c), op);
          b.CreateStore(r, slots[in.a]);
          break;
        }
        case Op::BitNot:
          b.CreateStore(j.emit_bnot_step(load_slot(in.b)), slots[in.a]);
          break;
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
          const char* op = in.op == Op::Eq   ? "=="
                           : in.op == Op::Ne ? "!="
                           : in.op == Op::Lt ? "<"
                           : in.op == Op::Le ? "<="
                           : in.op == Op::Gt ? ">"
                                             : ">=";
          auto r = j.emit_comparison_i1(load_slot(in.b), load_slot(in.c), op);
          b.CreateStore(j.make_bool(r), slots[in.a]);
          break;
        }
        case Op::ArrayNew: {
          auto arr = j.emit_call(
              j.module_->getOrInsertFunction(rt::array_new, ptrTy), {},
              "arr");
          b.CreateStore(j.make_array(arr), slots[in.a]);
          break;
        }
        case Op::ArrayAppend: {
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.c);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::array_set_or_push, b.getVoidTy(), ptrTy, i64Ty,
                  b.getInt8Ty(), i64Ty),
              {arr, b.getInt64(in.b), j.extract_tag(v), j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.c]);
          break;
        }
        case Op::ArrayPush: {
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::array_push, b.getVoidTy(),
                                             ptrTy, b.getInt8Ty(), i64Ty),
              {arr, j.extract_tag(v), j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        }
        case Op::ArrayExtend: {
          // Borrows the source register (compile_array's spread arm); the
          // non-iterable throw unwinds through the region pad with every
          // slot still frame-owned.
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::array_extend, b.getVoidTy(), ptrTy, b.getInt8Ty(),
                  i64Ty, i64Ty, i64Ty),
              {arr, j.extract_tag(v), j.extract_data(v),
               j.current_line_val(), j.current_column_val()});
          break;
        }
        case Op::ArrayResize: {
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto count = j.value_to_long(load_slot(in.b));
          llvm::Value* defTag = b.getInt8(TAG_NIL);
          llvm::Value* defData = b.getInt64(0);
          if (in.c >= 0) {
            auto d = load_slot(in.c);
            defTag = j.extract_tag(d);
            defData = j.extract_data(d);
          }
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::array_resize, b.getVoidTy(), ptrTy, i64Ty,
                  b.getInt8Ty(), i64Ty, i64Ty, i64Ty),
              {arr, count, defTag, defData, j.current_line_val(),
               j.current_column_val()});
          break;
        }
        case Op::TupleNew: {
          auto tup = j.emit_call(
              j.module_->getOrInsertFunction(rt::tuple_new, ptrTy), {},
              "tup");
          b.CreateStore(j.make_tuple(tup), slots[in.a]);
          break;
        }
        case Op::TuplePush: {
          auto tup = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::tuple_push, b.getVoidTy(),
                                             ptrTy, b.getInt8Ty(), i64Ty),
              {tup, j.extract_tag(v), j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        }
        case Op::SetNew: {
          auto s = j.emit_call(
              j.module_->getOrInsertFunction(rt::set_new, ptrTy), {}, "set");
          b.CreateStore(j.make_set(s), slots[in.a]);
          break;
        }
        case Op::SetAdd: {
          // The SetOpPos lowered just before published the literal's
          // position for the positionless unhashable throw.
          auto s = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::set_add, b.getVoidTy(),
                                             ptrTy, b.getInt8Ty(), i64Ty),
              {s, j.extract_tag(v), j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        }
        case Op::RangeNew: {
          bool hs = in.c & 1, he = in.c & 2;
          auto res = j.emit_make_range(
              hs ? j.extract_data(load_slot(in.b)) : nullptr,
              he ? j.extract_data(load_slot(in.b + 1)) : nullptr, in.c & 4,
              j.extract_data(load_slot(in.b + 2)));
          b.CreateStore(res, slots[in.a]);
          break;
        }
        case Op::ChkLong:
          // value_to_long's error branch is the whole point; the Long
          // payload is discarded.
          j.value_to_long(load_slot(in.a));
          break;
        case Op::NilChk: {
          // emit_nonnull_assert's shape, with the position from the chunk
          // table (the `!!` token's stamp).
          auto isNil = b.CreateICmpEQ(j.extract_tag(load_slot(in.a)),
                                      b.getInt8(TAG_NIL), "vm.nonnull.isnil");
          auto errBB = BasicBlock::Create(j.ctx_, "vm.nonnull.nil", fn);
          auto okBB = BasicBlock::Create(j.ctx_, "vm.nonnull.ok", fn);
          b.CreateCondBr(isNil, errBB, okBB);
          b.SetInsertPoint(errBB);
          j.emit_throw_error("NilError", "`!!` applied to nil",
                             j.current_line_, j.current_column_);
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          b.SetInsertPoint(okBB);
          break;
        }
        case Op::SeqChk: {
          // Array-or-Tuple tag, then the size the pattern asked for. Both
          // arms are pure loads, so this is a plain CondBr — no cleanup, no
          // position: a pattern test cannot throw.
          auto v = load_slot(in.a);
          auto tag = j.extract_tag(v);
          auto isSeq = b.CreateOr(
              b.CreateICmpEQ(tag, b.getInt8(TAG_ARRAY)),
              b.CreateICmpEQ(tag, b.getInt8(TAG_TUPLE)), "vseq.is_seq");
          auto sizeBB = BasicBlock::Create(j.ctx_, "vseq.size", fn);
          auto* fall = blocks.at(static_cast<int32_t>(i) + 1);
          b.CreateCondBr(isSeq, sizeBB, blocks.at(in.b));
          b.SetInsertPoint(sizeBB);
          auto n = j.emit_call(
              j.module_->getOrInsertFunction(rt::array_size, i64Ty, ptrTy),
              {b.CreateIntToPtr(j.extract_data(v), ptrTy)}, "vseq.n");
          auto want = b.getInt64(in.c);
          b.CreateCondBr(in.d ? b.CreateICmpSGE(n, want)
                              : b.CreateICmpEQ(n, want),
                         fall, blocks.at(in.b));
          break;
        }
        case Op::SeqGet: {
          // array_get hands back a borrowed element; the register owns what
          // it holds, so mint the slot's own reference.
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          llvm::Value* at = b.getInt64(in.c);
          if (in.c < 0) {
            auto n = j.emit_call(
                j.module_->getOrInsertFunction(rt::array_size, i64Ty, ptrTy),
                {arr}, "vseq.n");
            at = b.CreateAdd(n, at, "vseq.from_end");
          }
          auto outTag = b.CreateAlloca(b.getInt8Ty(), nullptr, "vseq.tag");
          auto outData = b.CreateAlloca(i64Ty, nullptr, "vseq.data");
          j.emit_call(
              j.module_->getOrInsertFunction(rt::array_get, b.getVoidTy(),
                                             ptrTy, i64Ty, ptrTy, ptrTy, i64Ty,
                                             i64Ty),
              {arr, at, outTag, outData, j.current_line_val(),
               j.current_column_val()});
          auto v = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                b.CreateLoad(i64Ty, outData));
          j.emit_value_retain(v);
          b.CreateStore(v, slots[in.a]);
          break;
        }
        case Op::SeqRest: {
          auto arr = b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          auto n = j.emit_call(
              j.module_->getOrInsertFunction(rt::array_size, i64Ty, ptrTy),
              {arr}, "vrest.n");
          auto out = j.emit_call(
              j.module_->getOrInsertFunction(rt::array_slice, ptrTy, ptrTy,
                                             i64Ty, i64Ty),
              {arr, b.getInt64(in.c), b.CreateSub(n, b.getInt64(in.d))},
              "vrest.arr");
          b.CreateStore(j.make_array(out), slots[in.a]);
          break;
        }
        case Op::ObjGet: {
          auto v = load_slot(in.c);
          auto key = j.emit_str_literal(
              _str_sv(reinterpret_cast<const char*>(c.consts[in.d].data)));
          auto isObj = b.CreateICmpEQ(j.extract_tag(v),
                                      b.getInt8(TAG_OBJECT), "vobj.is_obj");
          auto hasBB = BasicBlock::Create(j.ctx_, "vobj.has", fn);
          auto getBB = BasicBlock::Create(j.ctx_, "vobj.get", fn);
          b.CreateCondBr(isObj, hasBB, blocks.at(in.b));
          b.SetInsertPoint(hasBB);
          auto obj = b.CreateIntToPtr(j.extract_data(v), ptrTy);
          auto has = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_has, b.getInt1Ty(),
                                             ptrTy, ptrTy),
              {obj, key}, "vobj.hit");
          b.CreateCondBr(has, getBB, blocks.at(in.b));
          b.SetInsertPoint(getBB);
          auto outTag = b.CreateAlloca(b.getInt8Ty(), nullptr, "vobj.tag");
          auto outData = b.CreateAlloca(i64Ty, nullptr, "vobj.data");
          j.emit_call(
              j.module_->getOrInsertFunction(rt::object_get, b.getVoidTy(),
                                             ptrTy, ptrTy, ptrTy, ptrTy),
              {obj, key, outTag, outData});
          auto got = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                  b.CreateLoad(i64Ty, outData));
          j.emit_value_retain(got);  // object_get borrows the slot
          b.CreateStore(got, slots[in.a]);
          b.CreateBr(blocks.at(static_cast<int32_t>(i) + 1));
          break;
        }
        case Op::DestrErr: {
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::destructure_mismatch,
                                             b.getVoidTy(), i64Ty, i64Ty),
              {b.getInt64(line), b.getInt64(col)});
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          break;
        }
        case Op::Index: {
          // emit_point_index consumes the key on its returning paths and
          // releases both operands on its throw edges — and the slice arm's
          // emit_slice_value releases both on its throw edge; the registers
          // must stay slot-owned (the handler ladder is the sole slot
          // releaser), so retain both up front — the emitters' releases
          // cancel the retains, and the surviving +1s are dropped on the
          // normal paths. The result is +1 on both arms.
          auto recv = load_slot(in.b);
          auto key = load_slot(in.c);
          j.emit_value_retain(recv);
          j.emit_value_retain(key);
          auto cond = j.emit_is_range(key);
          auto sliceBB = BasicBlock::Create(j.ctx_, "vidx.slice", fn);
          auto pointBB = BasicBlock::Create(j.ctx_, "vidx.point", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "vidx.merge", fn);
          b.CreateCondBr(cond, sliceBB, pointBB);
          b.SetInsertPoint(sliceBB);
          {
            // Slice reads both operands; drop both minted +1s here.
            auto res = j.emit_slice_value(recv, key);
            j.emit_value_release(recv);
            j.emit_value_release(key);
            b.CreateStore(res, slots[in.a]);
            b.CreateBr(mergeBB);
          }
          b.SetInsertPoint(pointBB);
          {
            auto res = j.emit_point_index(recv, key);
            j.emit_value_release(recv);
            b.CreateStore(res, slots[in.a]);
            b.CreateBr(mergeBB);
          }
          b.SetInsertPoint(mergeBB);
          break;
        }
        case Op::PropVal:
        case Op::PropRaw: {
          // The AST path's own emitters, so the resolve, the inline cache and
          // the `self` binding are the same IR a `x.name` read compiles to.
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          auto recv = load_slot(in.b);
          // own_receiver=false: the register keeps its +1 across the read,
          // and the handler ladder is its sole releaser on a throw edge.
          auto view = j.emit_property_get(recv, key);
          if (in.op == Op::PropRaw) {
            // Always the borrowed object-slot form: the three introspection
            // names, whose fn_mode view arrives +1, are a compile-time reject.
            j.emit_value_retain(view);
            b.CreateStore(view, slots[in.a]);
          } else {
            b.CreateStore(j.emit_property_value_read(recv, view, key),
                          slots[in.a]);
          }
          break;
        }
        case Op::MethGate: {
          // checkBB then the receiver gate, in compile_user_method_over_
          // builtin's order, with the AST path's own property emitter.
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          auto id = static_cast<BMeth>(in.d);
          auto recv = load_slot(in.b);
          auto tag = j.extract_tag(recv);
          b.CreateStore(j.make_no_self(), slots[in.a]);
          auto objBB = BasicBlock::Create(j.ctx_, "vbm.obj", fn);
          auto gateBB = BasicBlock::Create(j.ctx_, "vbm.gate", fn);
          auto userBB = BasicBlock::Create(j.ctx_, "vbm.user", fn);
          auto contBB = BasicBlock::Create(j.ctx_, "vbm.cont", fn);
          b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_OBJECT)), objBB,
                         gateBB);

          b.SetInsertPoint(objBB);
          // own_receiver=false: the register keeps its +1 across the read.
          auto view = j.emit_property_get(recv, key);
          auto toUser = b.CreateOr(
              b.CreateICmpEQ(j.extract_tag(view), b.getInt8(TAG_FUNC)),
              j.emit_has_own_field(recv, key));
          if (culebra::is_object_builtin_method_name(key)) {
            // A Shared view carries no dict builtins: every name it lacks is
            // a frozen-tree read, not the dict table's.
            toUser = b.CreateOr(
                toUser,
                b.CreateICmpNE(
                    j.emit_call(j.module_->getFunction(rt::is_shared_val),
                                {j.extract_data(recv)}, "vbm.isview"),
                    b.getInt8(0)));
          }
          b.CreateCondBr(toUser, userBB, gateBB);

          b.SetInsertPoint(userBB);
          j.emit_value_retain(view);  // the slot owns what it holds
          b.CreateStore(view, slots[in.a]);
          b.CreateBr(contBB);

          b.SetInsertPoint(gateBB);
          const BMethSpec& gate = bmeth_gate_spec(id);
          auto mask = gate.recv;
          if (mask == kRecvAny) {
            b.CreateBr(contBB);  // no gate: every receiver resolves the name
          } else {
            auto badBB = BasicBlock::Create(j.ctx_, "vbm.rcverr", fn);
            // An iterator-protocol name resolves on an Object only when the
            // object carries the protocol; a plain dict merely lacks it.
            llvm::BasicBlock* shapeBB = contBB;
            if (gate.obj_iter_shaped && bmeth_receiver_ok(mask, TAG_OBJECT))
              shapeBB = BasicBlock::Create(j.ctx_, "vbm.itershape", fn);
            auto sw = b.CreateSwitch(tag, badBB, std::popcount(mask));
            for (int8_t t = 0; t < 16; ++t)
              if (bmeth_receiver_ok(mask, t))
                sw->addCase(b.getInt8(t), t == TAG_OBJECT ? shapeBB : contBB);
            if (shapeBB != contBB) {
              b.SetInsertPoint(shapeBB);
              b.CreateCondBr(
                  j.emit_object_has(
                      b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                      j.get_or_create_global_str("next", ".it.next")),
                  contBB, badBB);
            }
            // A scalar receiver fails here and now; anything else only lacks
            // the method, which BMeth reports once the arguments have run.
            b.SetInsertPoint(badBB);
            auto scalarBB = BasicBlock::Create(j.ctx_, "vbm.scalar", fn);
            auto missBB = BasicBlock::Create(j.ctx_, "vbm.miss", fn);
            b.CreateCondBr(j.emit_is_scalar_tag(tag), scalarBB, missBB);
            b.SetInsertPoint(scalarBB);
            j.emit_type_error_typed("Object, Array, or Tensor", tag);
            b.CreateUnreachable();
            b.SetInsertPoint(missBB);
            b.CreateStore(j.make_value(b.getInt8(TAG_NO_SELF),
                                       b.getInt64(kBMethGateMiss)),
                          slots[in.a]);
            b.CreateBr(contBB);
          }

          b.SetInsertPoint(contBB);
          break;
        }
        case Op::ChkParam: {
          const BMethSpec& spec = bmeth_specs()[in.c];
          auto kind = spec.params[in.d];
          auto gate = load_slot(in.b);
          auto gateOkBB = BasicBlock::Create(j.ctx_, "vbm.chk.gate", fn);
          auto chkBB = BasicBlock::Create(j.ctx_, "vbm.chk", fn);
          auto errBB = BasicBlock::Create(j.ctx_, "vbm.chk.err", fn);
          auto contBB = BasicBlock::Create(j.ctx_, "vbm.chk.cont", fn);
          // A user method shadowing the built-in binds by its own signature,
          // and a miss binds nothing at all.
          b.CreateCondBr(
              b.CreateAnd(
                  b.CreateICmpEQ(j.extract_tag(gate), b.getInt8(TAG_NO_SELF)),
                  b.CreateICmpEQ(j.extract_data(gate),
                                 b.getInt64(kBMethGateBuiltin))),
              gateOkBB, contBB);
          b.SetInsertPoint(gateOkBB);
          // A per-arm parameter is only declared on some receivers; regs[b+1]
          // is the receiver (BMeth's own layout), so the arm decides here.
          if (spec.param_when[in.d] == 0) {
            b.CreateBr(chkBB);
          } else {
            auto mask = spec.param_when[in.d];
            auto sw = b.CreateSwitch(j.extract_tag(load_slot(in.b + 1)), contBB,
                                     std::popcount(mask));
            for (int8_t t = 0; t < 16; ++t)
              if (bmeth_receiver_ok(mask, t)) sw->addCase(b.getInt8(t), chkBB);
          }
          b.SetInsertPoint(chkBB);
          // The accepted tags inline, like the JIT arms' own gates — see
          // bmeth_param_ok for why the runtime check can't stand alone.
          auto argTag = j.extract_tag(load_slot(in.a));
          llvm::Value* ok = nullptr;
          switch (kind) {
            case BParam::Long:
              ok = b.CreateICmpEQ(argTag, b.getInt8(TAG_LONG));
              break;
            case BParam::Array:
              ok = b.CreateICmpEQ(argTag, b.getInt8(TAG_ARRAY));
              break;
            case BParam::String:
              ok = b.CreateICmpEQ(argTag, b.getInt8(TAG_STRING));
              break;
            case BParam::Set:
              ok = b.CreateICmpEQ(argTag, b.getInt8(TAG_SET));
              break;
            default:  // StrLike; Any emits no check at all
              ok = b.CreateOr(
                  b.CreateICmpEQ(argTag, b.getInt8(TAG_STRING)),
                  b.CreateICmpEQ(argTag, b.getInt8(TAG_STRINGVIEW)));
              break;
          }
          b.CreateCondBr(ok, contBB, errBB);
          b.SetInsertPoint(errBB);
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_throw_error("TypeError", bmeth_param_message(spec, in.d), line,
                             col);
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          b.SetInsertPoint(contBB);
          break;
        }
        case Op::BMeth: {
          auto [line, col] = chunk_pos_at(c, i);
          auto gate = load_slot(in.b);
          auto userBB = BasicBlock::Create(j.ctx_, "vbm.call", fn);
          auto biBB = BasicBlock::Create(j.ctx_, "vbm.builtin", fn);
          auto sentBB = BasicBlock::Create(j.ctx_, "vbm.sentinel", fn);
          auto missBB = BasicBlock::Create(j.ctx_, "vbm.missfail", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "vbm.merge", fn);
          b.CreateCondBr(
              b.CreateICmpEQ(j.extract_tag(gate), b.getInt8(TAG_NO_SELF)),
              sentBB, userBB);

          // The receiver resolved no method at all: MethGate deferred that to
          // here so the arguments run first, as the interp's order has them.
          b.SetInsertPoint(sentBB);
          b.CreateCondBr(b.CreateICmpEQ(j.extract_data(gate),
                                        b.getInt64(kBMethGateMiss)),
                         missBB, biBB);
          b.SetInsertPoint(missBB);
          j.emit_type_error_typed("Function", b.getInt8(TAG_NIL));
          b.CreateUnreachable();

          b.SetInsertPoint(userBB);
          // The shadowing user method: CallM's hand-off, one slot over.
          b.CreateStore(emit_invoke(gate, in.b + 1, in.b + 2, in.d, line, col),
                        slots[in.a]);
          b.CreateBr(mergeBB);

          b.SetInsertPoint(biBB);
          // Receiver and arguments stay slot-owned (borrowed by the built-in),
          // so nothing here consumes a slot and the ladder keeps releasing
          // them; the result is a fresh +1.
          auto recv = load_slot(in.b + 1);
          llvm::SmallVector<llvm::Value*, 2> argv;
          for (int32_t k = 0; k < in.d; ++k)
            argv.push_back(load_slot(in.b + 2 + k));
          auto arg = [&](int32_t k) { return argv[k]; };
          auto arr = [&] {
            return b.CreateIntToPtr(j.extract_data(recv), ptrTy);
          };
          // The consuming built-ins take the arguments off the registers
          // before the call, so the helper is their only owner from there on.
          if (bmeth_consumes_args(static_cast<BMeth>(in.c)))
            for (int32_t k = 0; k < in.d; ++k)
              b.CreateStore(j.make_nil(), slots[in.b + 2 + k]);
          auto cstr = [&](llvm::Value* v) {
            return j.emit_call(j.module_->getFunction(rt::strlike_to_cstr),
                               {j.extract_tag(v), j.extract_data(v)}, "vbm.s");
          };
          auto str_fn = [&](const char* rt_name,
                            llvm::ArrayRef<llvm::Value*> a) {
            return j.make_string(
                j.emit_call(j.module_->getFunction(rt_name), a, "vbm.r"));
          };
          // sum/product/min/max/tensor_reduce_all return %Value by the
          // struct-return convention emit_value_call matches (see its own
          // comment) — a bare emit_call would read the wrong registers.
          auto val_fn = [&](const char* rt_name,
                            llvm::ArrayRef<llvm::Value*> a) {
            return j.emit_value_call(j.module_->getFunction(rt_name), a,
                                     "vbm.r");
          };
          llvm::Value* res = nullptr;
          switch (static_cast<BMeth>(in.c)) {
            case BMeth::Size:
              res = j.make_long(j.emit_size_probe(recv, j.extract_tag(recv),
                                                  "vbm.size"));
              break;
            case BMeth::Empty:
              res = j.make_bool(b.CreateICmpEQ(
                  j.emit_size_probe(recv, j.extract_tag(recv), "vbm.empty"),
                  b.getInt64(0)));
              break;
            case BMeth::Presence: {
              // Non-empty yields the receiver retained — a second owner is
              // handing it out; empty yields nil.
              auto size =
                  j.emit_size_probe(recv, j.extract_tag(recv), "vbm.presence");
              auto emptyBB = BasicBlock::Create(j.ctx_, "vbm.pres.e", fn);
              auto fullBB = BasicBlock::Create(j.ctx_, "vbm.pres.f", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.pres.j", fn);
              // Entry-block alloca: this arm can sit inside a loop body.
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.pres");
              b.CreateCondBr(b.CreateICmpEQ(size, b.getInt64(0)), emptyBB,
                             fullBB);
              b.SetInsertPoint(emptyBB);
              b.CreateStore(j.make_nil(), out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(fullBB);
              auto kept = load_slot(in.b + 1);
              j.emit_value_retain(kept);
              b.CreateStore(kept, out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Upper: res = str_fn(rt::str_upper, {cstr(recv)}); break;
            case BMeth::Lower: res = str_fn(rt::str_lower, {cstr(recv)}); break;
            case BMeth::Capitalize:
              res = str_fn(rt::str_capitalize, {cstr(recv)});
              break;
            case BMeth::Trim: res = str_fn(rt::str_trim, {cstr(recv)}); break;
            case BMeth::Lines:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::str_lines), {cstr(recv)}, "vbm.l"));
              break;
            case BMeth::View:
              res = j.make_stringview(j.emit_call(
                  j.module_->getFunction(rt::strlike_view),
                  {j.extract_tag(recv), j.extract_data(recv)}, "vbm.v"));
              break;
            case BMeth::Repeat:
              res = str_fn(rt::str_repeat,
                           {cstr(recv), j.extract_data(arg(0)),
                            b.getInt64(line), b.getInt64(col)});
              break;
            case BMeth::Truncate:
              res = str_fn(rt::str_truncate,
                           {cstr(recv), j.extract_data(arg(0)), cstr(arg(1)),
                            b.getInt64(line), b.getInt64(col)});
              break;
            case BMeth::TrimStart:
              res = str_fn(rt::str_trim_start, {cstr(recv), cstr(arg(0))});
              break;
            case BMeth::TrimEnd:
              res = str_fn(rt::str_trim_end, {cstr(recv), cstr(arg(0))});
              break;
            case BMeth::Tr:
              res = str_fn(rt::str_tr,
                           {cstr(recv), cstr(arg(0)), cstr(arg(1))});
              break;
            case BMeth::Split:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::str_split),
                  {cstr(recv), cstr(arg(0))}, "vbm.sp"));
              break;
            case BMeth::StartsWith:
            case BMeth::EndsWith:
              // The predicates are declared i1-returning, so the result
              // rides straight into the Bool (the AST arms' own shape).
              res = j.make_bool(j.emit_call(
                  j.module_->getFunction(
                      static_cast<BMeth>(in.c) == BMeth::StartsWith
                          ? rt::str_starts_with
                          : rt::str_ends_with),
                  {cstr(recv), cstr(arg(0))}, "vbm.p"));
              break;
            case BMeth::Push:
              j.emit_call(
                  j.module_->getOrInsertFunction(rt::array_push, b.getVoidTy(),
                                                 ptrTy, b.getInt8Ty(), i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0))});
              res = j.make_nil();
              break;
            case BMeth::Insert:
              j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::array_insert, b.getVoidTy(), ptrTy, i64Ty,
                      b.getInt8Ty(), i64Ty, i64Ty, i64Ty),
                  {arr(), j.extract_data(arg(0)), j.extract_tag(arg(1)),
                   j.extract_data(arg(1)), b.getInt64(line),
                   b.getInt64(col)});
              res = j.make_nil();
              break;
            case BMeth::Extend:
              j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::array_extend, b.getVoidTy(), ptrTy, b.getInt8Ty(),
                      i64Ty, i64Ty, i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(line), b.getInt64(col)});
              res = j.make_nil();
              break;
            case BMeth::Reverse:
              j.emit_call(j.module_->getOrInsertFunction(
                              rt::array_reverse, b.getVoidTy(), ptrTy),
                          {arr()});
              res = j.make_nil();
              break;
            case BMeth::IndexOf:
              j.emit_set_op_pos();  // positionless ValueError on a deep element
              res = j.make_long(j.emit_call(
                  j.module_->getOrInsertFunction(rt::array_index_of, i64Ty,
                                                 ptrTy, b.getInt8Ty(), i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0))},
                  "vbm.iof"));
              break;
            case BMeth::Slice:
            case BMeth::Contains: {
              // The polymorphic arms: one block per receiver, merged through
              // an entry-block alloca (this arm can sit inside a loop body).
              // String/StringView is the switch's default, the executor's own
              // reading. The gate proved the tag is one of the cases.
              bool is_slice = static_cast<BMeth>(in.c) == BMeth::Slice;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.poly");
              auto strBB = BasicBlock::Create(j.ctx_, "vbm.p.str", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.p.join", fn);
              auto arm = [&](const char* name) {
                return BasicBlock::Create(j.ctx_, name, fn);
              };
              auto boolv = [&](llvm::Value* v) {
                return j.make_bool(
                    v->getType()->isIntegerTy(1)
                        ? v
                        : b.CreateICmpNE(
                              v, llvm::ConstantInt::get(v->getType(), 0)));
              };
              if (is_slice) {
                auto arrBB = arm("vbm.sl.arr");
                auto tenBB = arm("vbm.sl.ten");
                auto sw = b.CreateSwitch(tagv, strBB, 3);
                sw->addCase(b.getInt8(TAG_ARRAY), arrBB);
                sw->addCase(b.getInt8(TAG_TENSOR), tenBB);
                b.SetInsertPoint(arrBB);
                b.CreateStore(
                    j.make_array(j.emit_call(
                        j.module_->getFunction(rt::array_slice2),
                        {arr(), j.extract_data(arg(0)), j.extract_data(arg(1))},
                        "vbm.sl")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(tenBB);
                // The engine's out-of-bounds IndexError arrives positionless.
                j.emit_set_op_pos();
                b.CreateStore(
                    j.make_tensor(j.emit_call(
                        j.module_->getFunction(rt::tensor_slice),
                        {b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                         j.extract_data(arg(0)), j.extract_data(arg(1))},
                        "vbm.slt")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(strBB);
                b.CreateStore(
                    j.make_stringview(j.emit_call(
                        j.module_->getFunction(rt::strlike_slice_view),
                        {tagv, j.extract_data(recv), j.extract_data(arg(0)),
                         j.extract_data(arg(1))},
                        "vbm.sls")),
                    out);
                b.CreateBr(joinBB);
              } else {
                auto arrBB = arm("vbm.ct.arr");
                auto setBB = arm("vbm.ct.set");
                auto tupBB = arm("vbm.ct.tup");
                auto iterBB = arm("vbm.ct.iter");
                // The needle rides into four arms, so it is taken apart here,
                // ahead of the switch: this block ends at that terminator.
                auto nTag = j.extract_tag(arg(0));
                auto nData = j.extract_data(arg(0));
                auto sw = b.CreateSwitch(tagv, strBB, 5);
                sw->addCase(b.getInt8(TAG_ARRAY), arrBB);
                sw->addCase(b.getInt8(TAG_SET), setBB);
                sw->addCase(b.getInt8(TAG_TUPLE), tupBB);
                sw->addCase(b.getInt8(TAG_OBJECT), iterBB);
                b.SetInsertPoint(arrBB);
                // A too-deep element raises a positionless ValueError.
                j.emit_set_op_pos();
                b.CreateStore(
                    boolv(j.emit_call(j.module_->getFunction(rt::array_contains),
                                      {arr(), nTag, nData}, "vbm.ct")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(setBB);
                b.CreateStore(
                    boolv(j.emit_call(
                        j.module_->getOrInsertFunction(
                            rt::set_contains, b.getInt1Ty(), ptrTy,
                            b.getInt8Ty(), i64Ty, i64Ty, i64Ty),
                        {b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                         nTag, nData, b.getInt64(line), b.getInt64(col)},
                        "vbm.cts")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(tupBB);
                j.emit_set_op_pos();
                b.CreateStore(
                    boolv(j.emit_call(
                        j.module_->getOrInsertFunction(rt::tuple_contains,
                                                       b.getInt8Ty(), ptrTy,
                                                       b.getInt8Ty(), i64Ty),
                        {arr(), nTag, nData}, "vbm.ctt")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(iterBB);
                // The iterator protocol itself: an object that does not carry
                // it fails inside the drive, the same error interp reports.
                j.emit_set_op_pos();
                b.CreateStore(
                    boolv(j.emit_call(j.module_->getFunction(rt::iter_contains),
                                      {tagv, j.extract_data(recv), nTag, nData},
                                      "vbm.cti")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(strBB);
                b.CreateStore(
                    boolv(j.emit_call(j.module_->getFunction(rt::str_contains),
                                      {cstr(recv), cstr(arg(0))}, "vbm.ctstr")),
                    out);
                b.CreateBr(joinBB);
              }
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::ToString:
              // The display form of any value — a too-deep one raises
              // positionless.
              j.emit_set_op_pos();
              res = j.make_string(j.emit_call(
                  j.module_->getFunction(rt::value_to_display),
                  {j.extract_tag(recv), j.extract_data(recv)}, "vbm.ts"));
              break;
            // iterator_builtins()'s eager trio: the gate already let through
            // only Array/Tensor/an iterator-shaped Object, so each arm is a
            // straight dispatch on the tag the gate proved — one block per
            // arm, merged through an entry-block alloca like Slice/Contains.
            case BMeth::Join: {
              // A non-String element's display can raise the too-deep
              // ValueError, positionless like ToString's.
              j.emit_set_op_pos();
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.jn");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.jn.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.jn.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.jn.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  str_fn(rt::array_join, {arr(), cstr(arg(0))}), out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  str_fn(rt::iter_join,
                        {tagv, j.extract_data(recv), cstr(arg(0))}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Sum:
            case BMeth::Max: {
              bool is_sum = static_cast<BMeth>(in.c) == BMeth::Sum;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.sx");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.sx.arr", fn);
              auto tenBB = BasicBlock::Create(j.ctx_, "vbm.sx.ten", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.sx.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.sx.join", fn);
              auto sw = b.CreateSwitch(tagv, iterBB, 2);
              sw->addCase(b.getInt8(TAG_ARRAY), arrBB);
              sw->addCase(b.getInt8(TAG_TENSOR), tenBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  val_fn(is_sum ? rt::array_sum : rt::array_max,
                        {arr(), b.getInt64(line), b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(tenBB);
              b.CreateStore(
                  val_fn(rt::tensor_reduce_all,
                        {arr(), b.getInt64(static_cast<int64_t>(
                                    is_sum ? culebra::Op::Sum
                                           : culebra::Op::Max))}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  val_fn(is_sum ? rt::iter_sum : rt::iter_max,
                        {tagv, j.extract_data(recv), b.getInt64(line),
                         b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Product:
            case BMeth::Min: {
              bool is_product = static_cast<BMeth>(in.c) == BMeth::Product;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.pn");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.pn.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.pn.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.pn.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  val_fn(is_product ? rt::array_product : rt::array_min,
                        {arr(), b.getInt64(line), b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  val_fn(is_product ? rt::iter_product : rt::iter_min,
                        {tagv, j.extract_data(recv), b.getInt64(line),
                         b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::ToSet: {
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.ts2");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.ts2.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.ts2.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.ts2.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  j.make_set(j.emit_call(
                      j.module_->getFunction(rt::array_to_set),
                      {arr(), b.getInt64(line), b.getInt64(col)}, "vbm.ts2a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  j.make_set(j.emit_call(
                      j.module_->getFunction(rt::iter_to_set),
                      {tagv, j.extract_data(recv), b.getInt64(line),
                       b.getInt64(col)},
                      "vbm.ts2i")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Sorted:
              // `reverse:` is kw-only and the VM rejects every keyword
              // argument at compile time, so this arm is reachable only
              // without it — the gate already proved Array.
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::array_sorted),
                  {arr(), b.getInt1(false), b.getInt64(line), b.getInt64(col)},
                  "vbm.srt"));
              break;
            case BMeth::Distinct:
              // The gate proved TAG_OBJECT (iterator-shaped) — no other tag
              // reaches here, so this is a straight call, no branch.
              res = j.make_object(
                  j.emit_call(j.module_->getFunction(rt::iter_distinct),
                             {j.extract_tag(recv), j.extract_data(recv),
                              b.getInt64(line), b.getInt64(col)},
                             "vbm.dis"));
              break;
            case BMeth::Flatten:
              res = j.make_object(
                  j.emit_call(j.module_->getFunction(rt::iter_flatten),
                             {j.extract_tag(recv), j.extract_data(recv),
                              b.getInt64(line), b.getInt64(col)},
                             "vbm.fl"));
              break;
            // Higher-order group: one block per receiver arm, merged through
            // an entry-block alloca like Sum/Max/ToSet above — the gate
            // already proved Array or an iterator-shaped Object. The
            // callback (and reduce's seed) already left the register run
            // nil'd above (bmeth_consumes_args), so `arg(k)` here is the
            // helper's sole owner from the call on.
            case BMeth::Map:
            case BMeth::Filter:
            case BMeth::FlatMap: {
              auto id = static_cast<BMeth>(in.c);
              const char* arrSym = id == BMeth::Map      ? rt::array_map
                                   : id == BMeth::Filter  ? rt::array_filter
                                                          : rt::array_flat_map;
              const char* iterSym = id == BMeth::Map      ? rt::iter_map
                                    : id == BMeth::Filter  ? rt::iter_filter
                                                           : rt::iter_flat_map;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.hof");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.hof.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.hof.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.hof.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  j.make_array(j.emit_call(
                      j.module_->getFunction(arrSym),
                      {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                       b.getInt64(line), b.getInt64(col)},
                      "vbm.hof.a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  j.make_object(j.emit_call(
                      j.module_->getFunction(iterSym),
                      {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                       j.extract_data(arg(0)), b.getInt64(line),
                       b.getInt64(col)},
                      "vbm.hof.i")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::ForEach: {
              auto tagv = j.extract_tag(recv);
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.fe.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.fe.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.fe.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              j.emit_call(j.module_->getFunction(rt::array_for_each),
                         {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                          b.getInt64(line), b.getInt64(col)});
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              j.emit_call(j.module_->getFunction(rt::iter_for_each),
                         {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                          j.extract_data(arg(0)), b.getInt64(line),
                          b.getInt64(col)});
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = j.make_nil();
              break;
            }
            case BMeth::AnyOf:
            case BMeth::All: {
              bool is_any = static_cast<BMeth>(in.c) == BMeth::AnyOf;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.aa");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.aa.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.aa.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.aa.join", fn);
              auto asBool = [&](llvm::Value* v) {
                return j.make_bool(b.CreateICmpNE(
                    v, llvm::ConstantInt::get(v->getType(), 0)));
              };
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  asBool(j.emit_call(
                      j.module_->getFunction(is_any ? rt::array_any
                                                    : rt::array_all),
                      {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                       b.getInt64(line), b.getInt64(col)},
                      "vbm.aa.a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  asBool(j.emit_call(
                      j.module_->getFunction(is_any ? rt::iter_any
                                                    : rt::iter_all),
                      {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                       j.extract_data(arg(0)), b.getInt64(line),
                       b.getInt64(col)},
                      "vbm.aa.i")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Find: {
              // The winning element's +1 arrives through out-params, like
              // Pop/RemoveAt below.
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vbm.fd.t");
              auto outData = eb.CreateAlloca(i64Ty, nullptr, "vbm.fd.d");
              auto tagv = j.extract_tag(recv);
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.fd.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.fd.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.fd.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              j.emit_call(j.module_->getFunction(rt::array_find),
                         {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                          b.getInt64(line), b.getInt64(col), outTag, outData});
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              j.emit_call(j.module_->getFunction(rt::iter_find),
                         {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                          j.extract_data(arg(0)), b.getInt64(line),
                          b.getInt64(col), outTag, outData});
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                 b.CreateLoad(i64Ty, outData));
              break;
            }
            case BMeth::MinBy:
            case BMeth::MaxBy: {
              bool is_min = static_cast<BMeth>(in.c) == BMeth::MinBy;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.mb");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.mb.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.mb.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.mb.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  val_fn(is_min ? rt::array_min_by : rt::array_max_by,
                        {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                         b.getInt64(line), b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  val_fn(is_min ? rt::iter_min_by : rt::iter_max_by,
                        {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                         j.extract_data(arg(0)), b.getInt64(line),
                         b.getInt64(col)}),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Reduce: {
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vbm.rd.t");
              auto outData = eb.CreateAlloca(i64Ty, nullptr, "vbm.rd.d");
              auto tagv = j.extract_tag(recv);
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.rd.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.rd.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.rd.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              j.emit_call(
                  j.module_->getFunction(rt::array_reduce),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   j.extract_tag(arg(1)), j.extract_data(arg(1)),
                   b.getInt64(line), b.getInt64(col), outTag, outData});
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              j.emit_call(
                  j.module_->getFunction(rt::iter_reduce),
                  {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                   j.extract_data(arg(0)), j.extract_tag(arg(1)),
                   j.extract_data(arg(1)), b.getInt64(line), b.getInt64(col),
                   outTag, outData});
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                 b.CreateLoad(i64Ty, outData));
              break;
            }
            case BMeth::GroupBy:
            case BMeth::Partition: {
              bool is_group_by = static_cast<BMeth>(in.c) == BMeth::GroupBy;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.gp");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.gp.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.gp.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.gp.join", fn);
              // A Tuple is a JitArray under a different tag (make_tuple's
              // own reading) — partition's runtime helper already returns
              // the pair that way.
              auto wrap = [&](llvm::Value* ptr) {
                return is_group_by ? j.make_object(ptr) : j.make_tuple(ptr);
              };
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  wrap(j.emit_call(
                      j.module_->getFunction(is_group_by
                                                  ? rt::array_group_by
                                                  : rt::array_partition),
                      {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                       b.getInt64(line), b.getInt64(col)},
                      "vbm.gp.a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              b.CreateStore(
                  wrap(j.emit_call(
                      j.module_->getFunction(is_group_by
                                                  ? rt::iter_group_by
                                                  : rt::iter_partition),
                      {tagv, j.extract_data(recv), j.extract_tag(arg(0)),
                       j.extract_data(arg(0)), b.getInt64(line),
                       b.getInt64(col)},
                      "vbm.gp.i")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::SortBy:
              // `reverse:` is kw-only and the VM rejects every keyword
              // argument at compile time, so this arm is reachable only
              // without it — the gate already proved Array.
              j.emit_call(
                  j.module_->getFunction(rt::array_sort_by),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt1(false), b.getInt64(line), b.getInt64(col)});
              res = j.make_nil();
              break;
            case BMeth::SortedBy:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::array_sorted_by),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt1(false), b.getInt64(line), b.getInt64(col)},
                  "vbm.sb"));
              break;
            case BMeth::Pop:
            case BMeth::RemoveAt: {
              // The element's +1 arrives through out-params. Entry-block
              // allocas: this arm can sit inside a loop body.
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vbm.rt");
              auto outData = eb.CreateAlloca(i64Ty, nullptr, "vbm.rd");
              if (static_cast<BMeth>(in.c) == BMeth::Pop) {
                j.emit_call(
                    j.module_->getOrInsertFunction(rt::array_pop, b.getVoidTy(),
                                                   ptrTy, ptrTy, ptrTy),
                    {arr(), outTag, outData});
              } else {
                j.emit_call(
                    j.module_->getOrInsertFunction(
                        rt::array_remove_at, b.getVoidTy(), ptrTy, i64Ty, ptrTy,
                        ptrTy, i64Ty, i64Ty),
                    {arr(), j.extract_data(arg(0)), outTag, outData,
                     b.getInt64(line), b.getInt64(col)});
              }
              res = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                 b.CreateLoad(i64Ty, outData));
              break;
            }
            // Set-only: both operands stay slot-owned (borrowed by the
            // runtime helper), like Contains'.
            case BMeth::Union:
            case BMeth::Intersect:
            case BMeth::Diff:
            case BMeth::SymDiff: {
              auto id = static_cast<BMeth>(in.c);
              const char* rt_name = id == BMeth::Union       ? rt::set_union
                                    : id == BMeth::Intersect ? rt::set_intersect
                                    : id == BMeth::Diff      ? rt::set_diff
                                                              : rt::set_sym_diff;
              res = j.make_set(j.emit_call(
                  j.module_->getOrInsertFunction(rt_name, ptrTy, ptrTy, ptrTy),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.set"));
              break;
            }
            case BMeth::Subset:
            case BMeth::Superset: {
              const char* rt_name = static_cast<BMeth>(in.c) == BMeth::Subset
                                        ? rt::set_subset
                                        : rt::set_superset;
              auto r = j.emit_call(
                  j.module_->getOrInsertFunction(rt_name, b.getInt8Ty(), ptrTy,
                                                 ptrTy),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.setp");
              res = j.make_bool(b.CreateICmpNE(r, b.getInt8(0)));
              break;
            }
            // `add` absorbed the argument's `+1` already (bmeth_consumes_args
            // nils the slot above); `remove` only hashes it for lookup.
            case BMeth::Add:
            case BMeth::Remove: {
              const char* rt_name = static_cast<BMeth>(in.c) == BMeth::Add
                                        ? rt::set_add_method
                                        : rt::set_remove;
              auto r = j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt_name, b.getInt8Ty(), ptrTy, b.getInt8Ty(), i64Ty,
                      i64Ty, i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(line), b.getInt64(col)},
                  "vbm.setm");
              res = j.make_bool(b.CreateICmpNE(r, b.getInt8(0)));
              break;
            }
            // Three unrelated value tables bind this name — Set, Tuple,
            // Tensor. One block per receiver, merged like Slice/Contains
            // (the gate already proved the tag is one of the three).
            case BMeth::ToArray: {
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.ta");
              auto tupBB = BasicBlock::Create(j.ctx_, "vbm.ta.tup", fn);
              auto tenBB = BasicBlock::Create(j.ctx_, "vbm.ta.ten", fn);
              auto setBB = BasicBlock::Create(j.ctx_, "vbm.ta.set", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.ta.join", fn);
              auto sw = b.CreateSwitch(tagv, setBB, 2);
              sw->addCase(b.getInt8(TAG_TUPLE), tupBB);
              sw->addCase(b.getInt8(TAG_TENSOR), tenBB);
              b.SetInsertPoint(tupBB);
              b.CreateStore(
                  j.make_array(j.emit_call(
                      j.module_->getOrInsertFunction(rt::tuple_to_array,
                                                     ptrTy, ptrTy),
                      {arr()}, "vbm.ta.t")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(tenBB);
              b.CreateStore(
                  j.make_array(j.emit_call(
                      j.module_->getFunction(rt::tensor_to_array),
                      {b.CreateIntToPtr(j.extract_data(recv), ptrTy)},
                      "vbm.ta.n")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(setBB);
              b.CreateStore(
                  j.make_array(j.emit_call(
                      j.module_->getOrInsertFunction(rt::set_to_array, ptrTy,
                                                     ptrTy),
                      {arr()}, "vbm.ta.s")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
          }
          b.CreateStore(res, slots[in.a]);
          b.CreateBr(mergeBB);
          b.SetInsertPoint(mergeBB);
          break;
        }
        case Op::BareMethChk: {
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          auto [line, col] = chunk_pos_at(c, i);
          auto recv = load_slot(in.b);
          // has-own is computed here rather than carried from PropVal: the
          // receiver's register is live until the statement sweep, so there
          // is no window the AST path's "while the receiver is still live"
          // ordering protects against.
          j.emit_reject_bare_builtin_method(load_slot(in.a),
                                            j.emit_has_own_field(recv, key),
                                            recv, key, line, col);
          break;
        }
        case Op::IndexWr:
        case Op::IndexCo: {
          // The write-context reads — the read halves of the JIT's
          // compound (IndexWr) and `??=` (IndexCo) index dispatches, with
          // the register-borrow contract: nothing is consumed from a slot,
          // the helpers' consuming key contract is fed by a retain, and
          // the +1 result lands in the destination slot.
          auto recv = load_slot(in.b);
          auto key = load_slot(in.c);
          auto tag = j.extract_tag(recv);
          auto arrBB = BasicBlock::Create(j.ctx_, "iwr.arr", fn);
          auto chkObjBB = BasicBlock::Create(j.ctx_, "iwr.chk_obj", fn);
          auto objBB = BasicBlock::Create(j.ctx_, "iwr.obj", fn);
          auto errBB = BasicBlock::Create(j.ctx_, "iwr.err", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "iwr.merge", fn);
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "iwr.tag");
          auto outData = eb.CreateAlloca(i64Ty, nullptr, "iwr.data");
          b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_ARRAY)), arrBB,
                         chkObjBB);
          b.SetInsertPoint(chkObjBB);
          b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_OBJECT)), objBB,
                         errBB);
          b.SetInsertPoint(errBB);
          j.emit_type_error_typed("Array", tag);
          b.CreateUnreachable();

          // Array arm: array_set's bounds rule (a negative index is
          // IndexError — guard_write_index), then array_get; the borrowed
          // element is retained for the register.
          b.SetInsertPoint(arrBB);
          {
            auto idx = j.value_to_long(key);
            auto negBB = BasicBlock::Create(j.ctx_, "iwr.neg", fn);
            auto okBB = BasicBlock::Create(j.ctx_, "iwr.ok", fn);
            b.CreateCondBr(b.CreateICmpSLT(idx, b.getInt64(0)), negBB, okBB);
            b.SetInsertPoint(negBB);
            j.emit_throw_error("IndexError", "index out of range",
                               j.current_line_, j.current_column_);
            b.CreateUnreachable();
            b.SetInsertPoint(okBB);
            auto arrPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
            j.emit_call(
                j.module_->getOrInsertFunction(rt::array_get, b.getVoidTy(),
                                               ptrTy, i64Ty, ptrTy, ptrTy,
                                               i64Ty, i64Ty),
                {arrPtr, idx, outTag, outData, j.current_line_val(),
                 j.current_column_val()});
            j.emit_value_retain(
                j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                             b.CreateLoad(i64Ty, outData)));
            b.CreateBr(mergeBB);
          }

          // Object arm: IndexCo passes the nc receiver-kind rejects (none
          // constructible in the slice; mirrored 1:1) and reads through
          // object_get_for_coalesce (a plain-dict miss is nil); IndexWr
          // reads through object_get_any (KeyError on a miss). Both
          // consume the key — the retain keeps the register the owner.
          b.SetInsertPoint(objBB);
          {
            auto objPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
            if (in.op == Op::IndexCo) {
              auto kind = j.emit_call(
                  j.module_->getOrInsertFunction(rt::nc_receiver_kind,
                                                 b.getInt8Ty(), i64Ty),
                  {j.extract_data(recv)}, "iwr.kind");
              auto favBB = BasicBlock::Create(j.ctx_, "iwr.faverr", fn);
              auto svBB = BasicBlock::Create(j.ctx_, "iwr.sverr", fn);
              auto sbBB = BasicBlock::Create(j.ctx_, "iwr.sberr", fn);
              auto plainBB = BasicBlock::Create(j.ctx_, "iwr.plain", fn);
              auto sw = b.CreateSwitch(kind, plainBB, 3);
              sw->addCase(b.getInt8(1), favBB);
              sw->addCase(b.getInt8(2), svBB);
              sw->addCase(b.getInt8(3), sbBB);
              b.SetInsertPoint(favBB);
              j.emit_throw_error(
                  "TypeError", "`?" "?=` is not supported on a FixedArray element",
                  j.current_line_, j.current_column_);
              b.CreateUnreachable();
              b.SetInsertPoint(svBB);
              j.emit_throw_error("ImmutableError",
                                 "Shared values are immutable",
                                 j.current_line_, j.current_column_);
              b.CreateUnreachable();
              b.SetInsertPoint(sbBB);
              j.emit_throw_error(
                  "TypeError",
                  "cannot assign to a SharedBuffer element directly; "
                  "set fields via buf[i].field = value",
                  j.current_line_, j.current_column_);
              b.CreateUnreachable();
              b.SetInsertPoint(plainBB);
              j.emit_value_retain(key);
              j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::object_get_for_coalesce, b.getInt1Ty(), ptrTy,
                      b.getInt8Ty(), i64Ty, ptrTy, ptrTy, i64Ty, i64Ty),
                  {objPtr, j.extract_tag(key), j.extract_data(key), outTag,
                   outData, j.current_line_val(), j.current_column_val()});
            } else {
              j.emit_value_retain(key);
              j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::object_get_any, b.getVoidTy(), ptrTy,
                      b.getInt8Ty(), i64Ty, ptrTy, ptrTy, i64Ty, i64Ty,
                      b.getInt1Ty()),
                  {objPtr, j.extract_tag(key), j.extract_data(key), outTag,
                   outData, j.current_line_val(), j.current_column_val(),
                   b.getInt1(false)});
            }
            b.CreateBr(mergeBB);
          }

          b.SetInsertPoint(mergeBB);
          b.CreateStore(j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                     b.CreateLoad(i64Ty, outData)),
                        slots[in.a]);
          break;
        }
        case Op::IndexSet: {
          auto recv = load_slot(in.a);
          auto key = load_slot(in.b);
          auto val = load_slot(in.c);
          auto tag = j.extract_tag(recv);
          auto arrBB = BasicBlock::Create(j.ctx_, "iset.arr", fn);
          auto chkObjBB = BasicBlock::Create(j.ctx_, "iset.chk_obj", fn);
          auto objBB = BasicBlock::Create(j.ctx_, "iset.obj", fn);
          auto errBB = BasicBlock::Create(j.ctx_, "iset.err", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "iset.merge", fn);
          b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_ARRAY)), arrBB,
                         chkObjBB);
          b.SetInsertPoint(chkObjBB);
          b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_OBJECT)), objBB,
                         errBB);
          b.SetInsertPoint(errBB);
          j.emit_type_error_typed("Array", tag);
          b.CreateUnreachable();

          // The stores consume a +1 of the value (and, for the Object arm,
          // of the key); the registers keep their own — the assignment
          // expression still reads the value slot afterwards. On array_set's
          // OOB throw the minted +1 strands to the GC backstop, like the
          // JIT's rval.
          b.SetInsertPoint(arrBB);
          {
            auto idx = j.value_to_long(key);
            auto arrPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
            j.emit_value_retain(val);
            j.emit_call(
                j.module_->getOrInsertFunction(rt::array_set, b.getVoidTy(),
                                               ptrTy, i64Ty, b.getInt8Ty(),
                                               i64Ty, i64Ty, i64Ty),
                {arrPtr, idx, j.extract_tag(val), j.extract_data(val),
                 j.current_line_val(), j.current_column_val()});
            b.CreateBr(mergeBB);
          }
          b.SetInsertPoint(objBB);
          {
            auto objPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
            j.emit_value_retain(key);
            j.emit_value_retain(val);
            j.emit_call(
                j.module_->getOrInsertFunction(
                    rt::object_set_any, b.getVoidTy(), ptrTy,
                    b.getInt8Ty(), i64Ty, b.getInt1Ty(), b.getInt8Ty(),
                    i64Ty, i64Ty, i64Ty, b.getInt1Ty()),
                {objPtr, j.extract_tag(key), j.extract_data(key),
                 b.getInt1(true), j.extract_tag(val), j.extract_data(val),
                 j.current_line_val(), j.current_column_val(),
                 b.getInt1(false)});
            b.CreateBr(mergeBB);
          }
          b.SetInsertPoint(mergeBB);
          break;
        }
        case Op::Jump:
          b.CreateBr(blocks.at(in.a));
          break;
        case Op::JumpIfFalse:
        case Op::JumpIfTrue: {
          auto t = j.value_to_bool(load_slot(in.a));
          auto* taken = blocks.at(in.b);
          auto* fall = blocks.at(static_cast<int32_t>(i) + 1);
          if (in.op == Op::JumpIfFalse)
            b.CreateCondBr(t, fall, taken);
          else
            b.CreateCondBr(t, taken, fall);
          break;
        }
        case Op::JumpIfNotNil: {
          auto notNil = b.CreateICmpNE(j.extract_tag(load_slot(in.a)),
                                       b.getInt8(TAG_NIL), "notnil");
          b.CreateCondBr(notNil, blocks.at(in.b),
                         blocks.at(static_cast<int32_t>(i) + 1));
          break;
        }
        case Op::JumpIfNil: {
          auto isNil = b.CreateICmpEQ(j.extract_tag(load_slot(in.a)),
                                      b.getInt8(TAG_NIL), "isnil");
          b.CreateCondBr(isNil, blocks.at(in.b),
                         blocks.at(static_cast<int32_t>(i) + 1));
          break;
        }
        case Op::JumpIfTag: {
          auto tagIs = b.CreateICmpEQ(
              j.extract_tag(load_slot(in.a)),
              b.getInt8(static_cast<uint8_t>(in.c)), "tag.is");
          b.CreateCondBr(tagIs, blocks.at(in.b),
                         blocks.at(static_cast<int32_t>(i) + 1));
          break;
        }
        case Op::MakeClosure: {
          const Chunk& f = p.chunks[in.b];
          auto n = f.capture_src_slots.size();
          auto cls = j.emit_call(
              j.module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                             i64Ty, i64Ty),
              {fns[in.b], b.getInt64(static_cast<int64_t>(n)),
               b.getInt64(f.arity)},
              "cls");
          if (n > 0) {
            auto capsFieldPtr =
                b.CreateStructGEP(j.closureType_, cls, 3, "caps.ptr");
            auto capsArr = b.CreateLoad(ptrTy, capsFieldPtr, "caps");
            for (size_t k = 0; k < n; ++k) {
              auto cellPtr = b.CreateIntToPtr(
                  j.extract_data(load_slot(f.capture_src_slots[k])), ptrTy);
              auto dst = b.CreateInBoundsGEP(
                  ptrTy, capsArr, {b.getInt64(static_cast<int64_t>(k))});
              b.CreateStore(cellPtr, dst);
              j.emit_cell_retain(cellPtr);  // the closure owns a ref
            }
          }
          b.CreateStore(j.make_func(cls), slots[in.a]);
          break;
        }
        case Op::Call:
        case Op::CallM: {
          // One arm for both: a method call differs only in where the arg run
          // starts (its head slot is the receiver) and in what rides the ABI's
          // receiver pair. culebra_runtime_call_receiver is not mirrored — see
          // the executor's CallM.
          bool meth = in.op == Op::CallM;
          auto [line, col] = chunk_pos_at(c, i);
          b.CreateStore(emit_invoke(load_slot(in.b), meth ? in.c : -1,
                                    in.c + (meth ? 1 : 0), in.d, line, col),
                        slots[in.a]);
          break;
        }
        case Op::Ret: {
          b.CreateCall(j.module_->getOrInsertFunction(rt::recursion_leave,
                                                      b.getVoidTy()));
          b.CreateStore(load_slot(in.a), retPtr);
          b.CreateRetVoid();
          break;
        }
        case Op::CellNew: {
          // Release the previous generation's cell (null on the first pass),
          // then wrap the value — make_cell_slot's declaration-point shape.
          j.emit_cell_release(
              b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy));
          auto v = load_slot(in.b);
          auto cellPtr = j.emit_call(
              j.module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                             b.getInt8Ty(), i64Ty),
              {j.extract_tag(v), j.extract_data(v)}, "cell");
          b.CreateStore(j.make_long(b.CreatePtrToInt(cellPtr, i64Ty)),
                        slots[in.a]);
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        }
        case Op::CellGet: {
          auto cellPtr =
              b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          auto valPtr = b.CreateStructGEP(j.cellType_, cellPtr, 1, "cell.vp");
          auto v = b.CreateLoad(j.valueType_, valPtr, "cell.val");
          j.emit_value_retain(v);
          b.CreateStore(v, slots[in.a]);
          break;
        }
        case Op::CellSet: {
          // store_slot's order: read old, store new, release old.
          auto cellPtr =
              b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto valPtr = b.CreateStructGEP(j.cellType_, cellPtr, 1, "cell.vp");
          auto old = b.CreateLoad(j.valueType_, valPtr, "cell.old");
          b.CreateStore(load_slot(in.b), valPtr);
          j.emit_value_release(old);
          b.CreateStore(j.make_nil(), slots[in.b]);
          break;
        }
        case Op::CellRelease:
          j.emit_cell_release(
              b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy));
          b.CreateStore(j.make_nil(), slots[in.a]);
          break;
        case Op::BindCapture: {
          // Lowered closures carry no descriptor: captures[b] directly.
          auto capsFieldPtr =
              b.CreateStructGEP(j.closureType_, clsArg, 3, "caps.ptr");
          auto capsArr = b.CreateLoad(ptrTy, capsFieldPtr, "caps");
          auto cellSlot = b.CreateInBoundsGEP(ptrTy, capsArr,
                                              {b.getInt64(in.b)}, "cell.slot");
          auto cellPtr = b.CreateLoad(ptrTy, cellSlot, "cell");
          b.CreateStore(j.make_long(b.CreatePtrToInt(cellPtr, i64Ty)),
                        slots[in.a]);
          break;
        }
        case Op::ImmutErr: {
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_immutable_assign_throw(
              reinterpret_cast<const char*>(c.consts[in.a].data),
              static_cast<size_t>(line), static_cast<size_t>(col));
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          break;
        }
        case Op::UnboundErr: {
          auto v = load_slot(in.a);
          auto unbound = b.CreateICmpEQ(j.extract_tag(v),
                                        b.getInt8(TAG_NO_SELF), "vm.unbound");
          auto errBB = BasicBlock::Create(j.ctx_, "vm.unbound.err", fn);
          auto okBB = BasicBlock::Create(j.ctx_, "vm.unbound.ok", fn);
          b.CreateCondBr(unbound, errBB, okBB);
          b.SetInsertPoint(errBB);
          auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
          j.emit_throw_error("NameError",
                             std::format("undefined variable '{}'", nm),
                             j.current_line_, j.current_column_);
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          b.SetInsertPoint(okBB);
          break;
        }
        case Op::MultifnReg: {
          const Chunk& f = p.chunks[in.d];
          auto keyG = j.get_or_create_global_str(
              _str_sv(reinterpret_cast<const char*>(c.consts[in.c].data)),
              ".vm.mfkey");
          // Arity-only dispatch: a null-typed entry per param, the chunk's
          // param names as .rodata (emit_multifn_register's arrays, via the
          // shared builder).
          auto nparams = static_cast<int64_t>(f.param_names.size());
          std::vector<llvm::Constant*> nameCs;
          std::vector<llvm::Constant*> typeCs;
          for (const auto& nm : f.param_names) {
            nameCs.push_back(j.get_or_create_global_str(nm, ".vm.mfpn"));
            typeCs.push_back(llvm::ConstantPointerNull::get(ptrTy));
          }
          auto namesPtr = j.build_str_ptr_array(nameCs, ".vm.mfnames");
          auto typesPtr = j.build_str_ptr_array(typeCs, ".vm.mftypes");
          auto bodyPtr =
              b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          auto disp = j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::multifn_register_and_install, ptrTy, ptrTy, ptrTy,
                  ptrTy, i64Ty, i64Ty, i64Ty, ptrTy),
              {keyG, bodyPtr, typesPtr, b.getInt64(nparams), b.getInt64(0),
               b.getInt64(nparams), namesPtr},
              "vm.mf.disp");
          b.CreateStore(j.make_nil(), slots[in.b]);
          b.CreateStore(j.make_func(disp), slots[in.a]);
          break;
        }
        case Op::NsGet: {
          // The JIT's own bare-builtin slow path; nothrow for allowlisted
          // names, so a plain call even inside a region.
          auto* nm = reinterpret_cast<const char*>(c.consts[in.b].data);
          b.CreateStore(j.emit_builtin_var_get(nm), slots[in.a]);
          break;
        }
        case Op::SetOpPos:
          // Uses the JIT's own emitter (rt::set_op_pos over the current
          // position state, already fed from this instruction's table row).
          j.emit_set_op_pos();
          break;
        case Op::Disp: {
          auto v = load_slot(in.b);
          auto s = j.emit_call(
              j.module_->getOrInsertFunction(rt::value_to_display, ptrTy,
                                             b.getInt8Ty(), i64Ty),
              {j.extract_tag(v), j.extract_data(v)}, "disp");
          b.CreateStore(j.make_string(s), slots[in.a]);
          break;
        }
        case Op::Fmt: {
          auto v = load_slot(in.b);
          auto specPtr = j.get_or_create_global_str(
              std::string(
                  _str_sv(reinterpret_cast<const char*>(c.consts[in.c].data))),
              ".fmtspec");
          auto [line, col] = chunk_pos_at(c, i);
          auto s = j.emit_call(
              j.module_->getOrInsertFunction(rt::format_value, ptrTy,
                                             b.getInt8Ty(), i64Ty, ptrTy,
                                             i64Ty, i64Ty),
              {j.extract_tag(v), j.extract_data(v), specPtr, b.getInt64(line),
               b.getInt64(col)},
              "fmt");
          b.CreateStore(j.make_string(s), slots[in.a]);
          break;
        }
        case Op::StrCat: {
          auto l = b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          auto r = b.CreateIntToPtr(j.extract_data(load_slot(in.c)), ptrTy);
          auto s = j.emit_call(
              j.module_->getOrInsertFunction(rt::str_concat, ptrTy, ptrTy,
                                             ptrTy),
              {l, r}, "concat");
          b.CreateStore(j.make_string(s), slots[in.a]);
          break;
        }
        case Op::Throw: {
          auto v = load_slot(in.a);
          b.CreateStore(j.make_nil(), slots[in.a]);
          j.emit_call(j.module_->getOrInsertFunction(
                          rt::throw_, b.getVoidTy(), b.getInt8Ty(), i64Ty),
                      {j.extract_tag(v), j.extract_data(v)});
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          break;
        }
        case Op::DeferMark: {
          // defer_mark is nothrow: a plain call, like the JIT's marks.
          auto m = b.CreateCall(j.module_->getFunction(rt::defer_mark), {},
                                "defer.mark");
          b.CreateStore(j.make_long(m), slots[in.a]);
          break;
        }
        case Op::DeferPush: {
          auto v = load_slot(in.a);
          b.CreateCall(j.module_->getFunction(rt::defer_push),
                       {j.extract_tag(v), j.extract_data(v)});
          break;
        }
        case Op::DeferRunTo:
          // May throw (a defer body throws): emit_call routes the unwind
          // edge through the enclosing region's pad / the frame pad.
          j.emit_call(j.module_->getFunction(rt::defer_run_to),
                      {j.extract_data(load_slot(in.a))});
          break;
        case Op::ForPrep: {
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_range_step_check(j.extract_data(load_slot(in.a + 2)), line,
                                  col);
          b.CreateStore(j.make_long(b.getInt64(0)), slots[in.a + 3]);
          b.CreateBr(blocks.at(in.b));
          break;
        }
        case Op::ForLoop: {
          // RangeBounds::done()/take() spelled as IR; inclusive is the
          // instruction's `d` immediate, so the compares are picked here.
          auto cur = j.extract_data(load_slot(in.a));
          auto end = j.extract_data(load_slot(in.a + 1));
          auto step = j.extract_data(load_slot(in.a + 2));
          auto exhausted = b.CreateICmpNE(
              j.extract_data(load_slot(in.a + 3)), b.getInt64(0));
          auto up = in.d ? b.CreateICmpSGT(cur, end)
                         : b.CreateICmpSGE(cur, end);
          auto down = in.d ? b.CreateICmpSLT(cur, end)
                           : b.CreateICmpSLE(cur, end);
          auto step_pos = b.CreateICmpSGT(step, b.getInt64(0));
          auto done =
              b.CreateOr(exhausted, b.CreateSelect(step_pos, up, down));
          auto advBB = BasicBlock::Create(j.ctx_, "for.adv", fn);
          b.CreateCondBr(done, blocks.at(static_cast<int32_t>(i) + 1), advBB);
          b.SetInsertPoint(advBB);
          auto sadd = Intrinsic::getOrInsertDeclaration(
              j.module_, Intrinsic::sadd_with_overflow, {i64Ty});
          auto res = b.CreateCall(sadd, {cur, step});
          b.CreateStore(j.make_long(b.CreateExtractValue(res, 0)),
                        slots[in.a]);
          b.CreateStore(
              j.make_long(b.CreateZExt(b.CreateExtractValue(res, 1), i64Ty)),
              slots[in.a + 3]);
          j.emit_value_release(load_slot(in.c));
          b.CreateStore(j.make_long(cur), slots[in.c]);
          b.CreateBr(blocks.at(in.b));
          break;
        }
        case Op::Println: {
          auto v = load_slot(in.a);
          j.emit_call(j.module_->getFunction(rt::println),
                      {j.extract_tag(v), j.extract_data(v)});
          break;
        }
        case Op::Safepoint:
          j.emit_safepoint();
          break;
        case Op::Halt:
          b.CreateRetVoid();
          break;
      }
    }
    j.current_lpad_ = nullptr;

    // Fill the landingpad preludes: emit_lpad_classify (compile_try's pad,
    // minus the AST path's scope state) classifies via the carriers, binds
    // the caught value, and jumps to the bytecode handler — whose release
    // ladder then runs as ordinary instructions.
    for (size_t k = 0; k < c.eh.size(); ++k) {
      const auto& r = c.eh[k];
      // A foreign exception rethrows toward the enclosing region's pad; with
      // no enclosing region it still owes the frame's defers on its way out
      // (interp's catch(...) runs run_deferred too), hence the frame pad.
      BasicBlock* outer = framePad;
      for (size_t m = k + 1; m < c.eh.size(); ++m)
        if (c.eh[m].start <= r.start && r.end <= c.eh[m].end) {
          outer = lpads[m];
          break;
        }
      j.emit_lpad_classify(lpads[k], outer, depthSlot, slots[r.caught_slot],
                           blocks.at(static_cast<int32_t>(r.handler)));
    }

    // Fill the frame pad (see its creation above). Ladder order: restore
    // this frame's depth first — the unwound callees never ran their
    // `leave`, and a defer body's own calls must count from here — then
    // run the pending defers (a plain call: a defer throwing mid-handling
    // propagates out, as the executor's catch does), uncount the frame,
    // and re-raise out of the function.
    if (framePad) {
      JIT::CleanupPad pad(j, /*outerLpad=*/nullptr);
      if (pad.open(framePad, "vm.frame.exc")) {
        auto restoreFn = j.module_->getOrInsertFunction(
            rt::recursion_restore, b.getVoidTy(), i64Ty);
        auto d = b.CreateLoad(i64Ty, depthSlot, "rec.d");
        b.CreateCall(restoreFn, {d});
        auto markV = b.CreateLoad(j.valueType_, slots[c.defer_mark_slot]);
        b.CreateCall(j.module_->getFunction(rt::defer_run_to),
                     {j.extract_data(markV)});
        if (chunk_idx != 0) {
          b.CreateCall(restoreFn,
                       {b.CreateSub(d, b.getInt64(1), "rec.d1")});
        }
      }
    }
  }
};

inline void run_program_via_llvm(const VmProgram& prog, bool emit_llvm,
                                 int opt_level) {
  Lowering::run_program(prog, emit_llvm, opt_level);
}

}  // namespace culebra::vm

#endif  // CULEBRA_JIT_ENABLED
