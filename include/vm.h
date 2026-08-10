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
// counted range `for`; `break` / `continue`; single-argument `println`.

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
  int32_t a = 0, b = 0, c = 0;
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

// AST -> Chunk. The front end is the shared FnAnalysis — the same passes the
// JIT runs — so the shadow check and (once functions land) the capture/EH
// analysis are backend-symmetric by construction. Slot assignment itself is
// the scope stack below, mirroring the JIT's Scope/VarSlot walk; FuncInfo's
// captured_locals/EH flags gain consumers when closures and try arrive.
// Everything outside the slice throws VmError at compile time.
class Compiler {
 public:
  Chunk compile_module(const peg::Ast& ast) {
    FnAnalysis analysis(&JIT::is_builtin_var);
    analysis.analyze_program(ast);  // runs lint::check_shadow (parity) and
                                    // will feed slot/capture layout as the
                                    // slice grows past plain locals
    compile_block(ast);
    emit(Op::Halt);
    chunk_.num_slots = high_water_;
    return std::move(chunk_);
  }

 private:
  struct Binding {
    std::string name;
    int32_t slot;
    bool is_mut;
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

  Chunk chunk_;
  std::vector<Scope> scopes_;
  std::vector<LoopCtx> loops_;
  int32_t next_slot_ = 0;
  int32_t named_top_ = 0;  // one past the highest live named slot; the temp
                           // sweep rolls next_slot_ back to at most this, so
                           // a let declared mid-statement keeps its slot
  int32_t high_water_ = 0;
  std::vector<int32_t> stmt_temps_;
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

  size_t emit(Op op, int32_t a = 0, int32_t b = 0, int32_t c = 0) {
    if (chunk_.positions.empty() ||
        chunk_.positions.back().line != pend_line_ ||
        chunk_.positions.back().col != pend_col_) {
      chunk_.positions.push_back(
          {static_cast<uint32_t>(chunk_.code.size()), pend_line_, pend_col_});
    }
    chunk_.code.push_back({op, a, b, c});
    return chunk_.code.size() - 1;
  }

  int32_t alloc_raw(const peg::Ast& at, std::string name) {
    if (next_slot_ >= kMaxSlots) reject(at, "frame larger than 256 slots");
    int32_t s = next_slot_++;
    if (s >= static_cast<int32_t>(chunk_.slot_names.size()))
      chunk_.slot_names.resize(s + 1);
    chunk_.slot_names[s] = std::move(name);
    high_water_ = std::max(high_water_, next_slot_);
    return s;
  }

  int32_t alloc_slot(const peg::Ast& at, std::string name) {
    int32_t s = alloc_raw(at, std::move(name));
    named_top_ = next_slot_;
    return s;
  }

  int32_t alloc_temp(const peg::Ast& at) {
    int32_t s = alloc_raw(at, "(tmp)");
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
  // JIT's frame ladder). Capped at named_top_: slots above it are statement
  // temps owned by a live TempScope, which releases them itself.
  void release_down_to(int32_t watermark) {
    for (int32_t s = std::min(next_slot_, named_top_) - 1; s >= watermark; --s)
      emit(Op::Release, s);
  }

  // Emits the scope's Releases and returns its slots to the allocator.
  void pop_scope() {
    const auto& sc = scopes_.back();
    release_down_to(sc.slot_watermark);
    next_slot_ = sc.slot_watermark;
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
    next_slot_ = std::max(slot_base, named_top_);
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
        compile_statement_inner(ast);
        break;
      case "ASSIGNMENT"_:
        store_into(dst, compile_assignment(ast), /*dst_is_fresh=*/true);
        break;
      case "CALL"_:
        compile_println(ast);
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
      case "CALL"_:
        compile_println(ast);
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
      default:
        compile_expr(ast);  // expression statement; temps swept by the caller
        break;
    }
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
      int32_t slot = alloc_slot(*tgt, std::string(tgt->token));
      store_into(slot, compile_expr(*av.rhs), /*dst_is_fresh=*/true);
      scopes_.back().bindings.push_back(
          {std::string(tgt->token), slot, av.is_mut});
      return {slot, false};
    }
    const Binding* b = lookup(tgt->token);
    if (!b) reject(*tgt, "assignment to an undeclared name");
    if (!b->is_mut) reject(*tgt, "assignment to an immutable binding");
    store_into(b->slot, compile_expr(*av.rhs));
    return {b->slot, false};
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

    push_scope();
    int32_t base = alloc_slot(ast, "(for.cur)");
    alloc_slot(ast, "(for.end)");
    alloc_slot(ast, "(for.step)");
    alloc_slot(ast, "(for.flags)");
    int32_t var = alloc_slot(id, std::string(id.token));

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

    scopes_.back().bindings.push_back({std::string(id.token), var, false});
    loops_.push_back({next_slot_, {}, {}});
    size_t body_ix = chunk_.code.size();
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

  void compile_println(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.size() != 2 || ast.nodes[0]->tag != "IDENTIFIER"_ ||
        ast.nodes[0]->token != "println" || lookup("println") ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_)
      reject(ast, "call (only a direct println(<expr>) is in the slice)");
    const auto& args = *ast.nodes[1];
    if (args.nodes.size() != 1) reject(args, "println takes one argument here");
    emit(Op::Println, compile_expr(*args.nodes[0]).slot);
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
        return {b->slot, false};
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
      "ForPrep",   "ForLoop",   "Println",    "Safepoint",    "Halt"};
  static_assert(std::size(kNames) == static_cast<size_t>(Op::Halt) + 1);
  std::string out;
  out += std::format("; slots: {}\n", c.num_slots);
  for (size_t s = 0; s < c.slot_names.size(); ++s)
    out += std::format(";   r{} = {}\n", s, c.slot_names[s]);
  for (size_t i = 0; i < c.code.size(); ++i) {
    const auto& in = c.code[i];
    auto [line, col] = chunk_pos_at(c, i);
    out += std::format("{:4}: {:<12} {:4} {:4} {:4}   ; {}:{}\n", i,
                       kNames[static_cast<size_t>(in.op)], in.a, in.b, in.c,
                       line, col);
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
  static void run(const Chunk& c) {
    if (c.num_slots > kMaxSlots)
      throw CulebraError("VmError", "--vm: frame too large");
    JitValue regs[kMaxSlots] = {};  // zero-init == {TAG_NIL, 0}
    const Insn* code = c.code.data();
    size_t pc = 0;

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
    auto to_bool = [&](const JitValue& v, size_t at) {
      if (v.tag == TAG_BOOL) return v.data != 0;
      auto [line, col] = chunk_pos_at(c, at);
      return culebra_runtime_to_bool(static_cast<int8_t>(v.tag), v.data,
                                     line, col);
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
            regs[in.a] = culebra_runtime_num_neg(static_cast<int8_t>(v.tag),
                                                 v.data, line, col);
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
                out = culebra_runtime_num_add(lt, l.data, rt, r.data, line, col);
                break;
              case Op::Sub:
                out = culebra_runtime_num_sub(lt, l.data, rt, r.data, line, col);
                break;
              case Op::Mul:
                out = culebra_runtime_num_mul(lt, l.data, rt, r.data, line, col);
                break;
              case Op::Div:
                out = culebra_runtime_num_div(lt, l.data, rt, r.data, line, col);
                break;
              case Op::Mod:
                out = culebra_runtime_num_mod(lt, l.data, rt, r.data, line, col);
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
            eq = culebra_runtime_value_equal(static_cast<int8_t>(l.tag),
                                             l.data,
                                             static_cast<int8_t>(r.tag),
                                             r.data);
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
                res = culebra_runtime_value_less(lt, l.data, rt, r.data,
                                                 line, col);
                break;
              case Op::Le:
                res = culebra_runtime_value_leq(lt, l.data, rt, r.data,
                                                line, col);
                break;
              case Op::Gt:
                res = culebra_runtime_value_greater(lt, l.data, rt, r.data,
                                                    line, col);
                break;
              default:
                res = culebra_runtime_value_geq(lt, l.data, rt, r.data,
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
          return;
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
  static void run_chunk(const Chunk& c, bool emit_llvm, int opt_level) {
    using namespace llvm;
    JIT::ensure_native_target_init();
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("vm", *ctx);
    JIT::apply_target(*mod, Triple(sys::getDefaultTargetTriple()));
    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    jit.declare_runtime_functions();

    auto fn = Function::Create(FunctionType::get(builder.getVoidTy(), false),
                               Function::ExternalLinkage, "__culebra_main",
                               mod.get());
    builder.SetInsertPoint(BasicBlock::Create(*ctx, "entry", fn));
    lower_chunk(jit, c, fn);
    verifyFunction(*fn);
    if (opt_level > 0) JIT::optimize_module(*mod, opt_level);
    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      JIT::exec(std::move(ctx), std::move(mod));
    }
  }

  static void lower_chunk(JIT& j, const Chunk& c, llvm::Function* fn) {
    using namespace llvm;
    auto& b = j.builder_;
    auto i64Ty = b.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(j.ctx_, 0);

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

    // Pass 1: every jump target opens a basic block. A conditional jump's
    // fall-through (and the insn after an unconditional Jump — dead code a
    // break/continue can leave) opens one too, so pass 2 never emits into a
    // terminated block.
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
                  rt::num_neg, j.valueType_, b.getInt8Ty(), i64Ty, i64Ty,
                  i64Ty),
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
  }
};

inline void run_chunk_via_llvm(const Chunk& chunk, bool emit_llvm,
                               int opt_level) {
  Lowering::run_chunk(chunk, emit_llvm, opt_level);
}

}  // namespace culebra::vm

#endif  // CULEBRA_JIT_ENABLED
