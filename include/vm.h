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
// JIT's cell mechanism — forward-reference capture is still rejected).

#ifdef CULEBRA_JIT_ENABLED

#include <fn_analysis.h>
#include <jit.h>
#include <parser.h>
#include <range_bounds.h>
#include <shared.h>
#include <stdlib_jit.h>  // culebra_runtime_println + the rt::println decl hook

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
  MakeClosure, // regs[a] = new closure (+1) over function chunk b. In the
               // executor its fn_ptr is Exec::trampoline and captures[0]
               // holds the chunk's descriptor (a Long-valued cell, so its
               // release is a no-op — the bound-method thunk precedent); in
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
  Throw,       // user `throw`: regs[a]'s +1 transfers to the thrown-value
               // carrier (culebra_runtime_throw); regs[a] is nil'd BEFORE the
               // raise so a handler's release ladder cannot double-release
               // the payload. Never falls through.
  ForPrep,     // control quad at base=a: {cur, end, step, flags}. Rejects a
               // zero step (ValueError at the range expression's position,
               // like rt::range_step_check), stores inclusive (c) into the
               // flags slot, jumps to the ForLoop at b.
  ForLoop,     // quad at a: constructs RangeBounds (range_bounds.h — the
               // sequence oracle all backends share); if !done(): take(),
               // write back cur/exhausted, release + rebind the loop var
               // slot c, jump to the body at b; else fall through.
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
};

inline std::pair<int64_t, int64_t> chunk_pos_at(const Chunk& c, size_t pc) {
  int64_t line = 0, col = 0;
  for (const auto& p : c.positions) {
    if (p.first_insn > pc) break;
    line = p.line;
    col = p.col;
  }
  return {line, col};
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
    Compiler main(prog, analysis, /*in_function=*/false, &top_info);
    main.compile_block(ast);
    main.emit(Op::Halt);
    main.chunk_.num_slots = main.high_water_;
    prog.chunks[0] = std::move(main.chunk_);
    return prog;
  }

 private:
  Compiler(VmProgram& prog, FnAnalysis& analysis, bool in_function,
           const FuncInfo* info)
      : prog_(prog),
        analysis_(analysis),
        in_function_(in_function),
        info_(info) {}

  struct Binding {
    std::string name;
    int32_t slot;
    bool is_mut;
    // The slot holds a cell pointer, not the value: reads go through
    // CellGet, writes through CellSet. True for a captured local's owned
    // cell and for a capture bound from the closure (borrowed).
    bool is_cell = false;
  };
  struct Scope {
    std::vector<Binding> bindings;
    int32_t slot_watermark;  // next_slot_ at scope entry; pop rolls back
  };
  struct LoopCtx {
    int32_t slot_watermark;  // slots >= this are inner to the loop scope
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
  Chunk chunk_;
  std::vector<Scope> scopes_;
  std::vector<LoopCtx> loops_;
  int32_t next_slot_ = 0;
  int32_t named_top_ = 0;  // one past the highest live named slot; the temp
                           // sweep rolls next_slot_ back to at most this, so
                           // a let declared mid-statement keeps its slot
  int32_t high_water_ = 0;
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

  // Runtime string layout: {i64 len} then bytes + NUL; the value points at
  // the bytes (see the str_arena comment on Chunk).
  int32_t kconst_str(std::string_view bytes) {
    auto buf = std::make_unique<char[]>(8 + bytes.size() + 1);
    int64_t len = static_cast<int64_t>(bytes.size());
    std::memcpy(buf.get(), &len, 8);
    std::memcpy(buf.get() + 8, bytes.data(), bytes.size());
    buf[8 + bytes.size()] = '\0';
    auto data = reinterpret_cast<int64_t>(buf.get() + 8);
    chunk_.str_arena.push_back(std::move(buf));
    return kconst({TAG_STRING, data});
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
      std::erase(stmt_temps_, r.slot);
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
    std::erase(stmt_temps_, r.slot);
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
  // is an owned temp (JIT load_slot), unlike a plain slot's borrow.
  ExprResult read_binding(const peg::Ast& at, const Binding& b) {
    if (!b.is_cell) return {b.slot, false};
    int32_t t = alloc_temp(at);
    emit(Op::CellGet, t, b.slot);
    return {t, true};
  }

  void compile_block(const peg::Ast& ast) {
    using namespace peg::udl;
    push_scope();
    if (ast.tag == "STATEMENTS"_) {
      for (const auto& n : ast.nodes) compile_statement(*n);
    } else {
      compile_statement(ast);
    }
    pop_scope();
  }

  // A block in value position: statements run normally, and the last one's
  // value lands in `dst` (nil for the valueless statements) before the block
  // scope's bindings are released. Every path writes `dst` at most once and
  // arrives with it still nil, so the stores skip the pre-Release.
  void compile_block_into(const peg::Ast& ast, int32_t dst) {
    using namespace peg::udl;
    push_scope();
    if (ast.tag == "STATEMENTS"_) {
      for (size_t i = 0; i + 1 < ast.nodes.size(); i++)
        compile_statement(*ast.nodes[i]);
      if (!ast.nodes.empty()) compile_value_into(*ast.nodes.back(), dst);
    } else {
      compile_value_into(ast, dst);
    }
    pop_scope();
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
      case "BREAK"_:
      case "CONTINUE"_: {
        bool brk = ast.tag == "BREAK"_;
        if (loops_.empty())
          reject(ast, brk ? "break outside a loop" : "continue outside a loop");
        auto& lc = loops_.back();
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
        release_down_to(0);
        emit(Op::Ret, rv);
        break;
      }
      default:
        compile_expr(ast);  // expression statement; temps swept by the caller
        break;
    }
  }

  // One function literal -> one chunk, compiled by a fresh Compiler (frame
  // state is per chunk; the program and analysis are shared). free_vars
  // resolve against THIS frame's bindings — each must already be a declared
  // cell binding (its own CellNew slot, or a capture passed through) —
  // and become the callee chunk's capture list. uses_fn places the
  // recursion-handle slot.
  int32_t compile_fn_chunk(const peg::Ast& ast) {
    using namespace peg::udl;
    const FuncInfo& info = analysis_.func_info.at(&ast);
    if (info.uses_args) reject(ast, "__ARGS__");
    for (const auto& n : ast.nodes)
      if (n->tag == "RETURN_TYPE"_) reject(*n, "return type annotation");
    const auto& params = *ast.nodes[0];
    for (const auto& p : params.nodes) {
      if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p))
        reject(*p, "keyword-only / rest parameter");
      if (culebra::is_pattern_param(*p)) reject(*p, "pattern parameter");
      if (culebra::extract_default_expr(*p)) reject(*p, "default argument");
      for (const auto& pc : p->nodes)
        if (pc->tag == "TYPE_ANNOTATION"_) reject(*p, "typed parameter");
    }
    const auto& body = *ast.nodes[1];

    // Resolve the capture list in the creating frame. The mut flag rides
    // along (the JIT's free_var_mut snapshot): a capture of a capture
    // keeps the original binding's flag by construction.
    std::vector<int32_t> cap_slots;
    std::vector<bool> cap_muts;
    for (const auto& fv : info.free_vars) {
      if (fv == "self") reject(ast, "closure capture of 'self'");
      if (info.optional_free_vars.contains(fv))
        reject(ast, std::format("UFCS candidate capture of '{}'", fv));
      const Binding* b = lookup(fv);
      // The JIT covers this with lazy forward-ref cells
      // (pre_allocate_forward_refs); outside the slice for now.
      if (!b) reject(ast, std::format("forward-reference capture of '{}'", fv));
      cap_slots.push_back(b->slot);
      cap_muts.push_back(b->is_mut);
    }

    int32_t idx = static_cast<int32_t>(prog_.chunks.size());
    prog_.chunks.emplace_back();  // reserve the index; nested fns append
    Compiler fc(prog_, analysis_, /*in_function=*/true, &info);
    fc.stamp(ast);
    fc.push_scope();  // the frame scope: params + captures + the `fn` handle
    fc.chunk_.arity = static_cast<int32_t>(params.nodes.size());
    fc.chunk_.capture_src_slots = std::move(cap_slots);
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
    for (const auto& p : params.nodes) {
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
    if (info.uses_fn) {
      fc.chunk_.fn_slot = fc.alloc_slot(ast, "fn");
      fc.scopes_.back().bindings.push_back({"fn", fc.chunk_.fn_slot, false});
    }
    for (const auto& pr : promos) {
      int32_t cslot = fc.alloc_slot(*pr.at, pr.name);
      fc.emit(Op::CellNew, cslot, pr.abi_slot);
      fc.mark_cell_slot(cslot);
      fc.scopes_.back().bindings.push_back({pr.name, cslot, pr.is_mut, true});
    }
    // Bind the captures: borrowed cell pointers out of the closure. The
    // slots are named-but-not-cell, so frame teardown's Release is a no-op
    // on them (the closure owns the refs).
    for (size_t i = 0; i < info.free_vars.size(); ++i) {
      int32_t s = fc.alloc_slot(ast, info.free_vars[i]);
      fc.emit(Op::BindCapture, s, static_cast<int32_t>(i));
      fc.scopes_.back().bindings.push_back(
          {info.free_vars[i], s, cap_muts[i], true});
    }
    int32_t rv = fc.alloc_temp(ast);
    fc.compile_block_into(body, rv);
    fc.pop_scope();
    fc.emit(Op::Ret, rv);
    fc.chunk_.num_slots = fc.high_water_;
    prog_.chunks[idx] = std::move(fc.chunk_);
    return idx;
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
    if (av.is_let) {
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
    alloc_slot(ast, "(for.flags)");
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
    size_t prep = emit(Op::ForPrep, base, 0, lay.inclusive ? 1 : 0);

    scopes_.back().bindings.push_back({std::string(id.token), bind, false, cell});
    loops_.push_back({next_slot_, {}, {}});
    size_t body_ix = chunk_.code.size();
    if (cell) emit(Op::CellNew, bind, var);
    compile_block(*fv.body);
    size_t safepoint_ix = emit(Op::Safepoint);
    size_t test_ix = chunk_.code.size();
    chunk_.code[prep].b = static_cast<int32_t>(test_ix);
    emit(Op::ForLoop, base, static_cast<int32_t>(body_ix), var);
    size_t exit_ix = chunk_.code.size();

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps)
      chunk_.code[j].a = static_cast<int32_t>(safepoint_ix);
    for (size_t j : lc.break_jumps)
      chunk_.code[j].a = static_cast<int32_t>(exit_ix);
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

    loops_.push_back({next_slot_, {}, {}});
    compile_block(*wv.body);
    emit(Op::Jump, static_cast<int32_t>(top_ix));
    size_t exit_ix = chunk_.code.size();
    chunk_.code[exit_jump].b = static_cast<int32_t>(exit_ix);

    auto& lc = loops_.back();
    for (size_t j : lc.continue_jumps)
      chunk_.code[j].a = static_cast<int32_t>(top_ix);
    for (size_t j : lc.break_jumps)
      chunk_.code[j].a = static_cast<int32_t>(exit_ix);
    loops_.pop_back();
  }

  // Direct `println(<expr>)` — the slice's I/O primitive, one dedicated op.
  // The caller (compile_expr's CALL case) has already matched the shape.
  void compile_println(const peg::Ast& ast) {
    const auto& args = *ast.nodes[1];
    if (args.nodes.size() != 1) reject(args, "println takes one argument here");
    emit(Op::Println, compile_expr(*args.nodes[0]).slot);
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
      chunk_.code[skip].b = static_cast<int32_t>(chunk_.code.size());
    }
    if (i < ast.nodes.size()) {  // trailing else block
      compile_block_into(*ast.nodes[i], res);
    }
    for (size_t j : end_jumps)
      chunk_.code[j].a = static_cast<int32_t>(chunk_.code.size());
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
    for (size_t j : end_jumps)
      chunk_.code[j].b = static_cast<int32_t>(chunk_.code.size());
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
    for (size_t j : false_jumps)
      chunk_.code[j].b = static_cast<int32_t>(chunk_.code.size());
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
    int32_t wm = next_slot_;
    auto start = static_cast<uint32_t>(chunk_.code.size());
    compile_block_into(*ast.nodes[0], res);
    size_t end_jump = emit(Op::Jump);
    auto handler = static_cast<uint32_t>(chunk_.code.size());
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
    compile_block_into(*ast.nodes[2], res);
    pop_scope();
    chunk_.code[end_jump].a = static_cast<int32_t>(chunk_.code.size());
    chunk_.eh.push_back(
        {start, static_cast<uint32_t>(end_jump), handler, caught});
    return {res, true};
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
        double d = ast.token_to_number<double>();
        int64_t bits;
        std::memcpy(&bits, &d, 8);
        emit(Op::LoadConst, t, kconst({TAG_FLOAT, bits}));
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
          auto v = compile_expr(*seq.nodes[i]);
          // ArrayAppend absorbs a +1: hand it an owned temp (retain a
          // borrowed slot into one first). The append nils the source, so
          // the statement sweep releases it as a no-op.
          int32_t src = v.slot;
          if (!v.owned) {
            src = alloc_temp(*seq.nodes[i]);
            emit(Op::Move, src, v.slot);
            emit(Op::Retain, src);
          }
          emit(Op::ArrayAppend, t, static_cast<int32_t>(i), src);
        }
        return {t, true};
      }
      case "IDENTIFIER"_: {
        const Binding* b = lookup(ast.token);
        if (!b)
          reject(ast, std::format("unresolved identifier '{}'", ast.token));
        return read_binding(ast, *b);
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
        int32_t idx = compile_fn_chunk(ast);
        int32_t t = alloc_temp(ast);
        emit(Op::MakeClosure, t, idx);
        return {t, true};
      }
      case "CALL"_: {
        if (ast.nodes.size() == 2 && ast.nodes[0]->tag == "IDENTIFIER"_ &&
            ast.nodes[0]->token == "println" && !lookup("println") &&
            ast.nodes[1]->original_tag == "ARGUMENTS"_) {
          compile_println(ast);
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
      "Jump",      "JumpIfFalse", "JumpIfTrue", "JumpIfNotNil",
      "MakeClosure", "Call",    "Ret",
      "CellNew",   "CellGet",   "CellSet",    "CellRelease",  "BindCapture",
      "ImmutErr",  "Throw",
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
    for (size_t i = 0; i < p.chunks.size(); ++i)
      p.descs[i] = {&p, static_cast<int32_t>(i)};
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
    JitValue regs[kMaxSlots] = {};  // zero-init == {TAG_NIL, 0}
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
        culebra::throw_missing_required_arg_at(
            c.param_names[static_cast<size_t>(n_args)], _jit_call_site_line,
            _jit_call_site_col);
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
        if (!r) throw;
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
    auto as_double = [](const JitValue& v) {
      if (v.tag == TAG_LONG) return static_cast<double>(v.data);
      double d;
      std::memcpy(&d, &v.data, 8);
      return d;
    };
    auto from_double = [](double d) {
      int64_t bits;
      std::memcpy(&bits, &d, 8);
      return JitValue{TAG_FLOAT, bits};
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
        case Op::MakeClosure: {
          const Chunk& f = p.chunks[in.b];
          auto n = f.capture_src_slots.size();
          auto* mc = culebra_runtime_closure_new(
              reinterpret_cast<void*>(&trampoline), 1 + n,
              static_cast<size_t>(f.arity));
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
        case Op::Throw: {
          JitValue v = regs[in.a];
          regs[in.a] = JitValue{TAG_NIL, 0};  // the +1 rides the carrier now
          culebra_runtime_throw(static_cast<int8_t>(v.tag), v.data);
          break;  // unreachable — throw never returns
        }
        case Op::ForPrep: {
          int64_t step = regs[in.a + 2].data;
          if (step == 0) {
            // The runtime helper is the sole owner of this diagnostic
            // (jit_iter.h); routing through it keeps the lanes identical.
            auto [line, col] = chunk_pos_at(c, pc);
            culebra_runtime_range_step_check(step, line, col);
          }
          regs[in.a + 3] = JitValue{TAG_LONG, in.c ? 2 : 0};
          pc = static_cast<size_t>(in.b);
          break;
        }
        case Op::ForLoop: {
          int64_t flags = regs[in.a + 3].data;
          RangeBounds rb{regs[in.a].data, regs[in.a + 1].data,
                         regs[in.a + 2].data, (flags & 2) != 0,
                         (flags & 1) != 0};
          if (rb.done()) {
            ++pc;
            break;
          }
          int64_t v = rb.take();
          regs[in.a] = JitValue{TAG_LONG, rb.cur};
          regs[in.a + 3] =
              JitValue{TAG_LONG, (flags & 2) | (rb.exhausted ? 1 : 0)};
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
    auto jitFnTy = FunctionType::get(
        builder.getVoidTy(),
        {ptrTy, ptrTy, builder.getInt8Ty(), builder.getInt64Ty(),
         builder.getInt64Ty(), ptrTy},
        false);
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
    if (!c.eh.empty()) fn->setPersonalityFn(j.get_personality_fn());
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
          nameptrs.push_back(b.CreateGlobalString(n));
        auto arrTy = ArrayType::get(ptrTy, nameptrs.size());
        auto* namesG = new GlobalVariable(
            *j.module_, arrTy, /*isConstant=*/true,
            GlobalValue::PrivateLinkage, ConstantArray::get(arrTy, nameptrs),
            ".paramnames");
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
    // Region handlers restore the recursion count to the frame's own level
    // (Exec::run_frame's frame_depth, the JIT's try.rec snapshot hoisted to
    // the prologue — the depth is constant within a frame).
    llvm::Value* depthSlot = nullptr;
    {
      llvm::Value* depth =
          chunk_idx != 0
              ? b.CreateCall(j.module_->getOrInsertFunction(
                                 rt::recursion_enter, i64Ty),
                             {}, "rec.depth")
              : (c.eh.empty()
                     ? nullptr
                     : b.CreateCall(j.module_->getOrInsertFunction(
                                        rt::recursion_depth, i64Ty),
                                    {}, "rec.depth"));
      if (!c.eh.empty()) {
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
    auto lpad_for = [&](size_t pc) -> BasicBlock* {
      for (size_t k = 0; k < c.eh.size(); ++k)
        if (c.eh[k].start <= pc && pc < c.eh[k].end) return lpads[k];
      return nullptr;  // innermost-first table order: first hit wins
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
              auto* bytes = reinterpret_cast<const char*>(k.data);
              int64_t len;
              std::memcpy(&len, bytes - 8, 8);
              v = j.make_string(j.emit_str_literal(
                  std::string_view(bytes, static_cast<size_t>(len))));
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
        case Op::Neg: {
          auto v = load_slot(in.b);
          auto isLong = b.CreateICmpEQ(j.extract_tag(v),
                                       b.getInt8(TAG_LONG));
          auto intBB = BasicBlock::Create(j.ctx_, "neg.int", fn);
          auto slowBB = BasicBlock::Create(j.ctx_, "neg.slow", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "neg.merge", fn);
          b.CreateCondBr(isLong, intBB, slowBB);
          b.SetInsertPoint(intBB);
          b.CreateStore(j.make_long(b.CreateNeg(j.extract_data(v))),
                        slots[in.a]);
          b.CreateBr(mergeBB);
          b.SetInsertPoint(slowBB);
          auto r = j.emit_value_call(
              j.module_->getOrInsertFunction(
                  rt::num_neg_borrow, j.valueType_, b.getInt8Ty(), i64Ty,
                  i64Ty, i64Ty),
              {j.extract_tag(v), j.extract_data(v), j.current_line_val(),
               j.current_column_val()}, "neg.num");
          b.CreateStore(r, slots[in.a]);
          b.CreateBr(mergeBB);
          b.SetInsertPoint(mergeBB);
          break;
        }
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
          auto jitFnTy = FunctionType::get(
              b.getVoidTy(),
              {ptrTy, ptrTy, b.getInt8Ty(), i64Ty, i64Ty, ptrTy}, false);
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
        case Op::Throw: {
          auto v = load_slot(in.a);
          b.CreateStore(j.make_nil(), slots[in.a]);
          j.emit_call(j.module_->getOrInsertFunction(
                          rt::throw_, b.getVoidTy(), b.getInt8Ty(), i64Ty),
                      {j.extract_tag(v), j.extract_data(v)});
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          break;
        }
        case Op::ForPrep: {
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_range_step_check(j.extract_data(load_slot(in.a + 2)), line,
                                  col);
          b.CreateStore(j.make_long(b.getInt64(in.c ? 2 : 0)),
                        slots[in.a + 3]);
          b.CreateBr(blocks.at(in.b));
          break;
        }
        case Op::ForLoop: {
          // RangeBounds::done()/take() spelled as IR; inclusive rides the
          // flags slot as a per-loop constant the optimizer folds.
          auto cur = j.extract_data(load_slot(in.a));
          auto end = j.extract_data(load_slot(in.a + 1));
          auto step = j.extract_data(load_slot(in.a + 2));
          auto flags = j.extract_data(load_slot(in.a + 3));
          auto exhausted =
              b.CreateICmpNE(b.CreateAnd(flags, b.getInt64(1)), b.getInt64(0));
          auto inclusive =
              b.CreateICmpNE(b.CreateAnd(flags, b.getInt64(2)), b.getInt64(0));
          auto up = b.CreateSelect(inclusive, b.CreateICmpSGT(cur, end),
                                   b.CreateICmpSGE(cur, end));
          auto down = b.CreateSelect(inclusive, b.CreateICmpSLT(cur, end),
                                     b.CreateICmpSLE(cur, end));
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
          auto newflags =
              b.CreateOr(b.CreateAnd(flags, b.getInt64(2)),
                         b.CreateZExt(b.CreateExtractValue(res, 1), i64Ty));
          b.CreateStore(j.make_long(newflags), slots[in.a + 3]);
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

    // Fill the landingpad preludes (compile_try's pad, minus the AST path's
    // scope state): classify via the carriers, rethrow a foreign exception
    // toward the enclosing region's pad (or out of the function), and on
    // the handled path restore the recursion depth, bind the caught value,
    // and jump to the bytecode handler — whose release ladder then runs as
    // ordinary instructions.
    for (size_t k = 0; k < c.eh.size(); ++k) {
      const auto& r = c.eh[k];
      b.SetInsertPoint(lpads[k]);
      auto lpadTy = StructType::get(ptrTy, b.getInt32Ty());
      auto lp = b.CreateLandingPad(lpadTy, 1, "exc");
      lp->addClause(ConstantPointerNull::get(cast<PointerType>(ptrTy)));
      auto excPtr = b.CreateExtractValue(lp, {0}, "exc.ptr");
      b.CreateCall(
          j.module_->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy),
          {excPtr});
      b.CreateCall(j.module_->getOrInsertFunction(
          "culebra_runtime_try_translate", b.getVoidTy()));
      auto flag = b.CreateCall(
          j.module_->getOrInsertFunction("culebra_runtime_get_is_throw",
                                         b.getInt8Ty()),
          {}, "is_throw");
      auto ours = b.CreateICmpNE(flag, b.getInt8(0));
      auto handleBB =
          BasicBlock::Create(j.ctx_, std::format("eh.handle.{}", k), fn);
      auto foreignBB =
          BasicBlock::Create(j.ctx_, std::format("eh.foreign.{}", k), fn);
      b.CreateCondBr(ours, handleBB, foreignBB);

      b.SetInsertPoint(foreignBB);
      {
        BasicBlock* outer = nullptr;
        for (size_t m = k + 1; m < c.eh.size(); ++m)
          if (c.eh[m].start <= r.start && r.end <= c.eh[m].end) {
            outer = lpads[m];
            break;
          }
        j.emit_rethrow(outer);
      }

      b.SetInsertPoint(handleBB);
      b.CreateCall(j.module_->getOrInsertFunction(
          "culebra_runtime_clear_is_throw", b.getVoidTy()));
      b.CreateCall(
          j.module_->getOrInsertFunction("__cxa_end_catch", b.getVoidTy()));
      b.CreateCall(j.module_->getOrInsertFunction(rt::recursion_restore,
                                                  b.getVoidTy(), i64Ty),
                   {b.CreateLoad(i64Ty, depthSlot, "rec.d")});
      auto tagV = b.CreateCall(
          j.module_->getOrInsertFunction("culebra_runtime_get_thrown_tag",
                                         b.getInt8Ty()),
          {}, "exc.tag");
      auto dataV = b.CreateCall(
          j.module_->getOrInsertFunction("culebra_runtime_get_thrown_data",
                                         i64Ty),
          {}, "exc.data");
      b.CreateStore(j.make_value(tagV, dataV), slots[r.caught_slot]);
      b.CreateBr(blocks.at(static_cast<int32_t>(r.handler)));
    }
  }
};

inline void run_program_via_llvm(const VmProgram& prog, bool emit_llvm,
                                 int opt_level) {
  Lowering::run_program(prog, emit_llvm, opt_level);
}

}  // namespace culebra::vm

#endif  // CULEBRA_JIT_ENABLED
