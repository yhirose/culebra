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
// Bool / nil / String / Array literals; `let` / `let mut` / reassignment of
// plain identifiers; arithmetic + - * / % and unary - ! + with the JIT's
// dispatch shape; comparisons (including chains) and `&&` / `||` / `??`;
// `if` / `else if` / `else` (as an expression) and the ternary; `while`;
// counted range `for`; `break` / `continue`; single-argument `println`;
// fn literals with closures (captured locals promoted to JitCells, the
// JIT's cell mechanism — forward-reference capture is still rejected);
// `match` over leaf patterns (literals, `_`, bindings, typed bindings over
// primitive type names, or-patterns of non-binding leaves) with guards;
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
  Eq,          // regs[a] = Bool(regs[b] == regs[c]): both-Long inline, else
  Ne,          // value_equal; ordering ops below go through
  Lt,          // value_{less,leq,greater,geq} (line/col-carrying, matching
  Le,          // the interp's nil-ordering and __lt__ dispatch)
  Gt,          //
  Ge,          //
  ArrayNew,    // regs[a] = fresh empty Array (+1)
  ArrayAppend, // append regs[c] into array regs[a] at index b; regs[c] = nil
               // (the array absorbs the +1, mirroring compile_array)
  Jump,        // pc = a
  JumpIfFalse, // if !to_bool(regs[a]) pc = b (strict Bool truthiness)
  JumpIfTrue,  // if to_bool(regs[a]) pc = b
  JumpIfNotNil,// if regs[a].tag != TAG_NIL pc = b (`??` short-circuit)
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
    if (av.compound) reject(ast, "compound assignment");
    if (!av.type_annotation.empty()) reject(ast, "type annotation");
    auto* tgt = culebra::assign_name_target(ast, av);
    if (!tgt) reject(ast, "non-identifier assignment target");
    if (tgt->token == "_") reject(*tgt, "sink binding");
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

  // General call: callee expression, positional args in a contiguous run of
  // owned temps (the JitFn ABI's arg slab), one Call op. Evaluation order is
  // callee first, then args left to right — both backends' order.
  ExprResult compile_call(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.size() != 2 || ast.nodes[1]->original_tag != "ARGUMENTS"_)
      reject(ast, "call chain / method call");
    auto callee = compile_expr(*ast.nodes[0]);
    const auto& args = *ast.nodes[1];
    for (const auto& a : args.nodes) {
      if (a->tag == "KWARG"_ || a->original_tag == "KWARG"_)
        reject(*a, "keyword argument");
      if (a->tag == "KWARG_SPLAT"_ || a->original_tag == "KWARG_SPLAT"_)
        reject(*a, "kwargs splat");
    }
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

  // `match` as an expression, over the leaf-pattern slice. The subject is
  // owned by a statement temp across the arms (the JIT holds it in a
  // dedicated subject scope; here the statement sweep / the break-return
  // temp releases are the single releaser). Every arm runs test → bind →
  // guard → body: a leaf pattern cannot bind before its tests pass, so a
  // failed test jumps to the next arm with nothing live, and only a guard
  // failure has a binding to release. The body block writes the shared
  // result slot exactly once (compile_if's shape); no arm matched → nil.
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
      // try_pattern default), and a captured one lives in a cell like any
      // local (the compile_try catch-binding shape).
      int32_t bound = -1;
      bool bound_cell = false;
      if (const peg::Ast* nn = pattern_binding_node(pat)) {
        auto name = std::string(nn->token);
        bound_cell = info_->captured_locals.contains(name);
        bound = alloc_slot(*nn, name);
        if (bound_cell) {
          store_new_cell(*nn, bound, {subj, false});
          mark_cell_slot(bound);
        } else {
          store_into(bound, {subj, false}, /*dst_is_fresh=*/true);
        }
        scopes_.back().bindings.push_back({name, bound, /*is_mut=*/true,
                                           bound_cell});
      }
      size_t body_idx = 1;
      size_t guard_fail = SIZE_MAX;
      if (arm->nodes[body_idx]->tag == "GUARD"_) {
        auto g = compile_expr(*arm->nodes[body_idx]->nodes[0]);
        // The test reads the pending MATCH position: a non-Bool guard's
        // TypeError reports the match node in both existing lanes.
        guard_fail = emit(Op::JumpIfFalse, g.slot);
        body_idx++;
      }
      // The arm body is its own defer scope (scan_eh_defer's MATCH case
      // keys the body node): defers fire when the arm's braces close, the
      // arm value already owned in `res`.
      compile_block_into(*arm->nodes[body_idx], res,
                         /*defer_key=*/arm->nodes[body_idx].get());
      pop_scope();  // the taken path's binding release
      end_jumps.push_back(emit(Op::Jump));
      if (guard_fail != SIZE_MAX) {
        // Guard-fail: the binding is live on this path; release it, then
        // fall through into the next arm's tests.
        patch_to_here(guard_fail);
        if (bound >= 0)
          emit(bound_cell ? Op::CellRelease : Op::Release, bound);
      }
      for (size_t ix : fail_jumps) patch_to_here(ix);
    }
    for (size_t ix : end_jumps) patch_to_here(ix);
    return {res, true};
  }

  // Emits the tests for one leaf pattern: fall through on match, jump (via
  // `fail`) on mismatch. Bindings are NOT emitted here — the caller binds
  // after the whole pattern passed, which is what makes the fail edges
  // release-free.
  void compile_pattern_test(const peg::Ast& pat, int32_t subj,
                            std::vector<size_t>& fail) {
    using namespace peg::udl;
    // A PATTERN node with children is an or-pattern: alternatives tried in
    // order, first match wins. A binding alternative would bind on some
    // paths only — the JIT/interp allow it (the bind simply happens on the
    // alternative that matched), but the two-phase test/bind split here
    // cannot express it, so it stays outside the slice.
    if (pat.tag == "PATTERN"_ && !pat.nodes.empty()) {
      if (pattern_binding_node(pat))
        reject(pat, "binding in an or-pattern");
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
      default:
        reject(pat, std::format("pattern '{}'", pat.name));
    }
  }

  // The single name a leaf pattern binds (nullptr when none): the
  // IDENTIFIER itself, or a TYPED_IDENT's name child. `_` sinks bind
  // nothing. For an or-PATTERN, any binding alternative — used both to
  // reject or-bindings and to find the arm's binding.
  const peg::Ast* pattern_binding_node(const peg::Ast& pat) const {
    using namespace peg::udl;
    if (pat.tag == "PATTERN"_) {
      for (const auto& sub : pat.nodes)
        if (auto* n = pattern_binding_node(*sub)) return n;
      return nullptr;
    }
    const peg::Ast* nn = nullptr;
    if (pat.tag == "IDENTIFIER"_) nn = &pat;
    else if (pat.tag == "TYPED_IDENT"_) nn = pat.nodes[0].get();
    if (nn && !is_sink_name(std::string(nn->token))) return nn;
    return nullptr;
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
        // All-literal pieces fold to one constant at compile time; pieces
        // with embedded expressions need calls, outside the slice.
        std::string folded;
        for (const auto& piece : ast.nodes) {
          if (piece->tag != "INTERPOLATED_CONTENT"_)
            reject(*piece, "string interpolation");
          folded += culebra::decode_interpolated_content(piece->token);
        }
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst_str(folded));
        return {t, true};
      }
      case "ARRAY"_: {
        if (ast.nodes.size() > 1) reject(ast, "sized array literal");
        int32_t t = alloc_temp(ast);
        emit(Op::ArrayNew, t);
        const auto& seq = *ast.nodes[0];
        for (size_t i = 0; i < seq.nodes.size(); i++) {
          if (seq.nodes[i]->tag == "SPREAD_ELEM"_)
            reject(*seq.nodes[i], "array spread");
          // ArrayAppend absorbs a +1: owned_src retains a borrowed slot
          // into a temp and drops a consumed temp from the sweep list.
          auto v = compile_expr(*seq.nodes[i]);
          emit(Op::ArrayAppend, t, static_cast<int32_t>(i),
               owned_src(*seq.nodes[i], v));
        }
        return {t, true};
      }
      case "IDENTIFIER"_: {
        const Binding* b = lookup(ast.token);
        if (b) return read_binding(ast, *b);
        if (is_stdlib_global(ast.token)) {
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
      "Div",       "Mod",       "Eq",         "Ne",           "Lt",
      "Le",        "Gt",        "Ge",         "ArrayNew",     "ArrayAppend",
      "Jump",      "JumpIfFalse", "JumpIfTrue", "JumpIfNotNil", "JumpIfTag",
      "MakeClosure", "Call",    "Ret",
      "CellNew",   "CellGet",   "CellSet",    "CellRelease",  "BindCapture",
      "ImmutErr",  "UnboundErr", "MultifnReg", "NsGet",
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
      verifyFunction(*fns[i]);
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
        case Op::Throw:
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfNotNil:
        case Op::JumpIfTag:
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
        case Op::Call: {
          auto [line, col] = chunk_pos_at(c, i);
          b.CreateCall(j.module_->getOrInsertFunction(
                           rt::set_call_site, b.getVoidTy(), i64Ty, i64Ty),
                       {b.getInt64(line), b.getInt64(col)});
          auto calleeV = load_slot(in.b);
          auto tag = j.extract_tag(calleeV);
          auto isFunc = b.CreateICmpEQ(tag, b.getInt8(TAG_FUNC));
          auto errBB = BasicBlock::Create(j.ctx_, "call.err", fn);
          auto okBB = BasicBlock::Create(j.ctx_, "call.ok", fn);
          b.CreateCondBr(isFunc, okBB, errBB);
          b.SetInsertPoint(errBB);
          // Same shape as the executor's cold path: no Object values exist
          // in the slice, so the JIT's __call__/ctor probes cannot hit.
          j.emit_call(
              j.module_->getOrInsertFunction(rt::type_error_typed,
                                             b.getVoidTy(), i64Ty, i64Ty,
                                             ptrTy, b.getInt8Ty()),
              {b.getInt64(line), b.getInt64(col),
               b.CreateGlobalString("Function"), tag});
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          b.SetInsertPoint(okBB);
          auto clsPtr = b.CreateIntToPtr(j.extract_data(calleeV), ptrTy);
          auto fnFieldPtr =
              b.CreateStructGEP(j.closureType_, clsPtr, 1, "fn.ptr");
          auto fnPtr = b.CreateLoad(ptrTy, fnFieldPtr, "fn");
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          llvm::Value* slab;
          if (in.d > 0) {
            slab = eb.CreateAlloca(ArrayType::get(j.valueType_, in.d),
                                   nullptr, "call.args");
            for (int32_t k = 0; k < in.d; ++k) {
              auto* dstp = b.CreateConstGEP2_64(
                  ArrayType::get(j.valueType_, in.d), slab, 0,
                  static_cast<uint64_t>(k));
              b.CreateStore(load_slot(in.c + k), dstp);
            }
          } else {
            slab = ConstantPointerNull::get(cast<PointerType>(ptrTy));
          }
          auto* retTmp = eb.CreateAlloca(j.valueType_, nullptr, "call.ret");
          auto jitFnTy = jit_fn_type(b, ptrTy);
          // The callee consumes each arg's +1 on every exit (the JitFn ABI),
          // so the arg slots go nil BEFORE the call — the slab alloca keeps
          // the values alive for the callee, and a region's release ladder
          // cannot double-release them on the unwind edge.
          for (int32_t k = 0; k < in.d; ++k)
            b.CreateStore(j.make_nil(), slots[in.c + k]);
          j.emit_call(jitFnTy, fnPtr,
                      {retTmp, clsPtr, b.getInt8(TAG_NO_SELF),
                       b.getInt64(0), b.getInt64(in.d), slab});
          b.CreateStore(b.CreateLoad(j.valueType_, retTmp), slots[in.a]);
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
      if (pred_empty(framePad)) {
        framePad->eraseFromParent();
      } else {
        j.emit_catch_all_prologue(framePad, "vm.frame.exc");
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
        j.emit_rethrow(nullptr);
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
