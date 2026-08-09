#pragma once

// Phase 0 spike of the shared bytecode VM (docs/internals/vm.md §7): a
// register-based bytecode for the counted range-`for` slice, a bytecode
// compiler for that slice, a VM executor running on the JIT's runtime value
// model, and an LLVM lowering of the same bytecode. Hidden behind --vm-spike;
// anything outside the slice is rejected at compile time ("SpikeError").
// Not a product surface — the format is an in-memory contract only.

#ifdef CULEBRA_JIT_ENABLED

#include <jit.h>
#include <parser.h>
#include <range_bounds.h>
#include <shared.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

extern "C" void culebra_runtime_println(int8_t tag, int64_t data);

namespace culebra::vmspike {

inline constexpr int32_t kMaxSlots = 256;

// One fixed-width instruction. Registers are frame slots holding JitValue.
// RC is explicit in the stream (vm.md §4): the compiler emits Retain/Release;
// the VM and the LLVM lowering only execute them. Everything in the slice is
// TAG_LONG/TAG_NIL, so the RC ops are runtime no-ops — the point is that the
// format carries them.
enum class Op : uint8_t {
  LoadConst,  // regs[a] = consts[b]
  Move,       // regs[a] = regs[b] (raw copy; pair with Retain for a borrow)
  Take,       // regs[a] = regs[b]; regs[b] = nil (ownership transfer)
  Retain,     // retain regs[a]
  Release,    // release regs[a]; regs[a] = nil — destructive, so every slot
              // has one owner and sweeps cannot double-release
  Neg,        // regs[a] = -regs[b] (Long, wrapping)
  Add,        // regs[a] = regs[b] + regs[c] (Long, wrapping)
  Sub,        // regs[a] = regs[b] - regs[c]
  Mul,        // regs[a] = regs[b] * regs[c]
  Jump,       // pc = a
  ForPrep,    // control quad at base=a: {cur, end, step, flags}. Rejects a
              // zero step (ValueError at the range expression's position,
              // like rt::range_step_check), stores inclusive (c) into the
              // flags slot, jumps to the ForLoop at b.
  ForLoop,    // quad at a: constructs RangeBounds (range_bounds.h — the
              // sequence oracle all backends share); if !done(): take(),
              // write back cur/exhausted, release + rebind the loop var
              // slot c, jump to the body at b; else fall through.
  Println,    // culebra_runtime_println(regs[a])
  Safepoint,  // interrupt poll — every loop back edge carries one
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
  std::vector<JitValue> consts;  // Long/nil only in the slice
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

// AST -> Chunk for the spike slice: top-level statements; `let` / `let mut`
// of a plain identifier; reassignment of a visible `let mut` slot; Long
// literals, identifiers, unary minus, + - *; counted range `for` with an
// identifier binding; break/continue; single-argument `println`. Everything
// else throws SpikeError at compile time — no interp fallback.
class SpikeCompiler {
 public:
  Chunk compile_module(const peg::Ast& ast) {
    using namespace peg::udl;
    push_scope();
    if (ast.tag == "STATEMENTS"_) {
      for (const auto& n : ast.nodes) compile_statement(*n);
    } else {
      compile_statement(ast);
    }
    pop_scope();
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
    throw CulebraError("SpikeError", "--vm-spike: unsupported: " + what,
                       static_cast<int64_t>(ast.line),
                       static_cast<int64_t>(ast.column));
  }

  void stamp(const peg::Ast& ast) {
    if (ast.line) {
      pend_line_ = static_cast<uint32_t>(ast.line);
      pend_col_ = static_cast<uint32_t>(ast.column);
    }
  }

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

  int32_t kconst(int64_t v) {
    chunk_.consts.push_back(JitValue{TAG_LONG, v});
    return static_cast<int32_t>(chunk_.consts.size()) - 1;
  }

  void push_scope() { scopes_.push_back({{}, next_slot_}); }

  // Emits the scope's Releases (reverse order, mirroring the JIT's frame
  // ladder) and returns its slots to the allocator.
  void pop_scope() {
    const auto& sc = scopes_.back();
    for (int32_t s = next_slot_ - 1; s >= sc.slot_watermark; --s)
      emit(Op::Release, s);
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

  // Statement temps live until the end of the statement that made them;
  // Release is destructive, so a Take'n temp releases as nil.
  struct TempScope {
    SpikeCompiler& c;
    size_t base;
    int32_t slot_base;
    explicit TempScope(SpikeCompiler& c_)
        : c(c_), base(c_.stmt_temps_.size()), slot_base(c_.next_slot_) {}
    ~TempScope() {
      for (size_t i = c.stmt_temps_.size(); i > base; --i)
        c.emit(Op::Release, c.stmt_temps_[i - 1]);
      c.stmt_temps_.resize(base);
      c.next_slot_ = std::max(slot_base, c.named_top_);
    }
  };

  // Release dst's previous value, then either transfer the temp's +1 or
  // copy-and-retain the borrowed slot. The one store rule every write uses.
  void store_into(int32_t dst, ExprResult r) {
    emit(Op::Release, dst);
    if (r.owned) {
      emit(Op::Take, dst, r.slot);
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

  void compile_statement(const peg::Ast& ast) {
    using namespace peg::udl;
    stamp(ast);
    TempScope ts(*this);
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
      case "CALL"_:
        compile_println(ast);
        break;
      case "BREAK"_: {
        if (loops_.empty()) reject(ast, "break outside a loop");
        auto& lc = loops_.back();
        for (int32_t s = next_slot_ - 1; s >= lc.slot_watermark; --s)
          emit(Op::Release, s);
        lc.break_jumps.push_back(emit(Op::Jump));
        break;
      }
      case "CONTINUE"_: {
        if (loops_.empty()) reject(ast, "continue outside a loop");
        auto& lc = loops_.back();
        for (int32_t s = next_slot_ - 1; s >= lc.slot_watermark; --s)
          emit(Op::Release, s);
        lc.continue_jumps.push_back(emit(Op::Jump));
        break;
      }
      default:
        compile_expr(ast);  // expression statement; temps swept by ts
        break;
    }
  }

  void compile_assignment(const peg::Ast& ast) {
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
      auto r = compile_expr(*av.rhs);
      stamp(ast);
      store_into(slot, r);
      scopes_.back().bindings.push_back(
          {std::string(tgt->token), slot, av.is_mut});
    } else {
      const Binding* b = lookup(tgt->token);
      if (!b) reject(*tgt, "assignment to an undeclared name");
      if (!b->is_mut) reject(*tgt, "assignment to an immutable binding");
      auto r = compile_expr(*av.rhs);
      stamp(ast);
      store_into(b->slot, r);
    }
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
    store_into(base + 0, compile_expr(*lay.start));
    store_into(base + 1, compile_expr(*lay.end));
    if (lay.step) {
      store_into(base + 2, compile_expr(*lay.step));
    } else {
      emit(Op::LoadConst, base + 2, kconst(1));
    }
    stamp(*fv.iter);
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

  void compile_println(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.nodes.size() != 2 || ast.nodes[0]->tag != "IDENTIFIER"_ ||
        ast.nodes[0]->token != "println" || lookup("println") ||
        ast.nodes[1]->original_tag != "ARGUMENTS"_)
      reject(ast, "call (only a direct println(<expr>) is in the slice)");
    const auto& args = *ast.nodes[1];
    if (args.nodes.size() != 1) reject(args, "println takes one argument here");
    auto r = compile_expr(*args.nodes[0]);
    stamp(ast);
    emit(Op::Println, r.slot);
  }

  ExprResult compile_expr(const peg::Ast& ast) {
    using namespace peg::udl;
    switch (ast.tag) {
      case "NUMBER"_: {
        int32_t t = alloc_temp(ast);
        emit(Op::LoadConst, t, kconst(parse_integer_literal(ast.token)));
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
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_: {
        auto acc = compile_expr(*ast.nodes[0]);
        for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
          auto op_tok = ast.nodes[i]->token;
          Op op;
          if (op_tok == "+") op = Op::Add;
          else if (op_tok == "-") op = Op::Sub;
          else if (op_tok == "*") op = Op::Mul;
          else reject(*ast.nodes[i], std::format("operator '{}'", op_tok));
          auto rhs = compile_expr(*ast.nodes[i + 1]);
          int32_t t = alloc_temp(ast);
          emit(op, t, acc.slot, rhs.slot);
          acc = {t, true};
        }
        return acc;
      }
      default:
        reject(ast, std::format("expression '{}'", ast.name));
    }
  }
};

inline std::string dump(const Chunk& c) {
  static constexpr const char* kNames[] = {
      "LoadConst", "Move",    "Take",    "Retain",  "Release",
      "Neg",       "Add",     "Sub",     "Mul",     "Jump",
      "ForPrep",   "ForLoop", "Println", "Safepoint", "Halt"};
  std::string out;
  out += std::format("; slots: {}\n", c.num_slots);
  for (size_t s = 0; s < c.slot_names.size(); ++s)
    out += std::format(";   r{} = {}\n", s, c.slot_names[s]);
  for (size_t i = 0; i < c.code.size(); ++i) {
    const auto& in = c.code[i];
    auto [line, col] = chunk_pos_at(c, i);
    out += std::format("{:4}: {:<10} {:4} {:4} {:4}   ; {}:{}\n", i,
                       kNames[static_cast<size_t>(in.op)], in.a, in.b, in.c,
                       line, col);
  }
  return out;
}

// The VM executor. Registers live in a C++ stack array so the conservative
// GC stack scan roots them for free (irrelevant for the Long-only slice, but
// it is the shape a real VM frame must have).
struct SpikeVM {
  static void run(const Chunk& c) {
    if (c.num_slots > kMaxSlots)
      throw CulebraError("SpikeError", "--vm-spike: frame too large");
    JitValue regs[kMaxSlots] = {};  // zero-init == {TAG_NIL, 0}
    const Insn* code = c.code.data();
    size_t pc = 0;
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
        case Op::Neg:
          regs[in.a] = JitValue{
              TAG_LONG, static_cast<int64_t>(
                            0 - static_cast<uint64_t>(regs[in.b].data))};
          ++pc;
          break;
        case Op::Add:
          regs[in.a] = JitValue{
              TAG_LONG, static_cast<int64_t>(
                            static_cast<uint64_t>(regs[in.b].data) +
                            static_cast<uint64_t>(regs[in.c].data))};
          ++pc;
          break;
        case Op::Sub:
          regs[in.a] = JitValue{
              TAG_LONG, static_cast<int64_t>(
                            static_cast<uint64_t>(regs[in.b].data) -
                            static_cast<uint64_t>(regs[in.c].data))};
          ++pc;
          break;
        case Op::Mul:
          regs[in.a] = JitValue{
              TAG_LONG, static_cast<int64_t>(
                            static_cast<uint64_t>(regs[in.b].data) *
                            static_cast<uint64_t>(regs[in.c].data))};
          ++pc;
          break;
        case Op::Jump:
          pc = static_cast<size_t>(in.a);
          break;
        case Op::ForPrep: {
          int64_t step = regs[in.a + 2].data;
          if (step == 0) {
            auto [line, col] = chunk_pos_at(c, pc);
            throw CulebraError("ValueError", "range() step must not be zero",
                               line, col);
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

// LLVM lowering of the same bytecode — filled in by the spike's LLVM step.
inline void run_chunk_via_llvm(const Chunk& chunk, bool emit_llvm) {
  (void)chunk;
  (void)emit_llvm;
  throw CulebraError("SpikeError", "--vm-spike-llvm: not implemented yet");
}

}  // namespace culebra::vmspike

#endif  // CULEBRA_JIT_ENABLED
