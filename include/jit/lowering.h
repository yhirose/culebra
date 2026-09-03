#pragma once

// The bytecode's second consumer: LLVM IR. `--jit` and `culebra build` (AOT)
// compile the very chunks vm.h's executor would have run, so this is the one
// part of the VM that needs LLVM — and the only reason the compiled lanes
// still split on CULEBRA_JIT_ENABLED (docs/internals/vm.md §9).

#ifdef CULEBRA_JIT_ENABLED

#include <jit/jit.h>
#include <aot/scan.h>  // aot_collect_names — which namespaces to link
#include <stdlib/preamble.h>    // resolve_baked_preamble — which not to lower
#include <vm/vm.h>

#include <memory>
#include <string>
#include <vector>

namespace culebra::vm {

// Bytecode -> LLVM IR, reusing the JIT object as the codegen context and the
// existing exec() scaffold (ORC, isolate-join and teardown-collect guards,
// uncaught-error conversion). This is the Phase 3 shape: jit.h consuming
// bytecode instead of the AST. Where the executor above calls a runtime
// helper, the lowering calls the JIT's own emitter for the same construct
// (emit_arith_step, emit_comparison_i1, value_to_bool) — one dispatch
// definition, two consumers.

struct Lowering {
  // One chunk's parameter metadata, as globals of the module being built —
  // JIT::emit_param_meta_global, fed from the chunk instead of from
  // a function literal's AST. Called from the MakeClosure site that first
  // needs it and cached by the caller: the globals are constant and the
  // registration idempotent, so every site naming a chunk registers the same
  // one. It has to be a site rather than a prepass because
  // IRBuilder::CreateGlobalString takes the module from the insertion block,
  // which only a chunk being lowered has.
  static llvm::Constant* param_meta_global(JIT& j, const Chunk& c,
                                           llvm::Function* fn) {
    std::vector<bool> has_default, muts;
    for (size_t k = 0; k < c.param_names.size(); k++) {
      has_default.push_back(k < c.param_has_default.size() &&
                            c.param_has_default[k] != 0);
      muts.push_back(k < c.param_mut.size() && c.param_mut[k] != 0);
    }
    auto idx = [](int32_t v) {
      return v >= 0 ? std::optional<size_t>(static_cast<size_t>(v))
                    : std::nullopt;
    };
    return j.emit_param_meta_global(
        fn, c.param_names, has_default, idx(c.kwargs_rest_idx),
        idx(c.first_kw_only_idx), c.multifn_name, c.return_type, muts,
        c.param_types, c.param_declared_types, c.cb_min, c.cb_max,
        c.mut_capture_names);
  }

  // The JitFn ABI signature (rt_value.inc.h), spelled once for both the chunk
  // function creation and the indirect call site.
  static llvm::FunctionType* jit_fn_type(llvm::IRBuilder<>& b,
                                         llvm::Type* ptrTy) {
    return llvm::FunctionType::get(
        b.getVoidTy(),
        {ptrTy, ptrTy, b.getInt8Ty(), b.getInt64Ty(), b.getInt64Ty(), ptrTy},
        false);
  }

  // `module_name` keys the backend object cache (JIT::jit_module_name): the
  // caller passes the name derived from the sources it compiled, or leaves it
  // empty for a program with nothing stable to key on. `fast_codegen` is
  // `--jit-faststart`, which skips the IR pipeline and takes the backend's
  // fast paths.
  static void run_program(const VmProgram& p, bool emit_llvm, int opt_level,
                          bool fast_codegen = false,
                          const std::string& module_name = "vm",
                          std::span<const BakedPreamble* const> baked = {}) {
    using namespace llvm;
    JIT::ensure_native_target_init();
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>(
        module_name.empty() ? "vm" : module_name, *ctx);
    JIT::apply_target(*mod, Triple(sys::getDefaultTargetTriple()));
    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    lower_program(jit, p, "__culebra_main", baked);
    if (opt_level > 0) JIT::optimize_module(*mod, opt_level);
    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      JIT::exec(std::move(ctx), std::move(mod), fast_codegen);
    }
  }

  // Write `mod` to `out_path` as an object file. The tail both object
  // emitters share; `who` names the failing command in a diagnostic.
  static int emit_object_file(llvm::TargetMachine& tm, llvm::Module& mod,
                              const std::string& out_path, const char* who) {
    using namespace llvm;
    std::error_code EC;
    raw_fd_ostream OS(out_path, EC, sys::fs::OF_None);
    if (EC) {
      std::fprintf(stderr, "%s: cannot open %s: %s\n", who, out_path.c_str(),
                   EC.message().c_str());
      return 1;
    }
    legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, OS, nullptr, CodeGenFileType::ObjectFile)) {
      std::fprintf(stderr, "%s: target does not support object emission\n",
                   who);
      return 1;
    }
    pm.run(mod);
    OS.flush();
    return 0;
  }

  // A stdlib preamble module as an object of its own (culebra_preamble_cc):
  // the lowering of its program with the top level under `entry` — one name
  // per module, so every module's object links into one binary — and no C
  // main or namespace list. Position-independent, since the driver links it
  // too. Symmetry is by construction: the same Compiler and lowering the
  // spliced source would go through at start-up.
  static int build_preamble_object(const VmProgram& p,
                                   const std::string& out_path, int opt_level,
                                   const std::string& entry) {
    using namespace llvm;
    JIT::ensure_native_target_init();
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>(entry, *ctx);
    std::string err;
    auto tm = JIT::apply_target(*mod, Triple(sys::getDefaultTargetTriple()),
                                &err, /*pic=*/true);
    if (!tm) {
      std::fprintf(stderr, "culebra build-preamble: %s\n", err.c_str());
      return 1;
    }
    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    lower_program(jit, p, entry.c_str());
    if (opt_level > 0) JIT::optimize_module(*mod, opt_level);
    return emit_object_file(*tm, *mod, out_path, "culebra build-preamble");
  }

  // AOT: the same lowering, to a TargetMachine object file instead of ORC,
  // plus the C `int main(int, char**)` that hands `__culebra_main` to
  // `culebra_aot_bootstrap` (libculebra_rt.a), and the stdlib namespace list
  // the archive's ns_groups() reads, from `names` (aot_collect_names). The
  // `baked` entries are plain calls in __culebra_main (lower_program), so
  // each is an undefined symbol the link resolves from the archive — one
  // member per named module, none for the rest. An empty `target_triple`
  // means the host. Returns 0 on success.
  // JIT::build_object's tail, over bytecode.
  static int build_object(const VmProgram& p, const std::string& out_path,
                          int opt_level, bool emit_llvm,
                          const std::string& target_triple,
                          const AotNames& names,
                          std::span<const BakedPreamble* const> baked = {}) {
    using namespace llvm;
    if (target_triple.empty()) {
      JIT::ensure_native_target_init();
    } else {
      JIT::ensure_all_targets_init();
    }
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);
    std::string err;
    auto tm = JIT::apply_target(
        *mod,
        Triple(target_triple.empty() ? sys::getDefaultTargetTriple()
                                     : target_triple),
        &err);
    if (!tm) {
      std::fprintf(stderr, "culebra build: %s\n", err.c_str());
      return 1;
    }

    IRBuilder<> builder(*ctx);
    JIT jit(ctx.get(), mod.get(), builder);
    auto* mainFn = lower_program(jit, p, "__culebra_main", baked);

    auto i32 = builder.getInt32Ty();
    auto ptrTy = PointerType::get(*ctx, 0);
    auto bootstrapFn = Function::Create(
        FunctionType::get(i32, {i32, ptrTy, ptrTy}, false),
        GlobalValue::ExternalLinkage, "culebra_aot_bootstrap", mod.get());
    auto cMain = Function::Create(
        FunctionType::get(i32, {i32, ptrTy}, false),
        GlobalValue::ExternalLinkage, "main", mod.get());
    builder.SetInsertPoint(BasicBlock::Create(*ctx, "entry", cMain));
    auto argIt = cMain->arg_begin();
    llvm::Value* argcArg = &*argIt++;
    llvm::Value* argvArg = &*argIt;
    builder.CreateRet(
        builder.CreateCall(bootstrapFn, {argcArg, argvArg, mainFn}));
    verifyFunction(*cMain);

    // culebra_aot_ns_groups[] / _count: one NsGroupRef {ns, group} per stdlib
    // namespace. A named namespace's group is an external reference — the one
    // thing that keeps its rows, signatures and adapters through the link's
    // dead-stripping — and an unnamed one's is null, which the archive reports
    // as a scan miss if the program reaches it anyway.
    {
      static_assert(sizeof(NsGroupRef) == 2 * sizeof(void*));
      auto i64 = builder.getInt64Ty();
      auto refTy = StructType::get(*ctx, {ptrTy, ptrTy});
      std::vector<Constant*> refs;
      for (const auto& g : kNsGroups) {
        Constant* group = ConstantPointerNull::get(ptrTy);
        if (aot_named(names, g.ns)) {
          group = mod->getOrInsertGlobal(ns_group_symbol(g.ns),
                                         builder.getInt8Ty());
        }
        refs.push_back(ConstantStruct::get(
            refTy, {builder.CreateGlobalString(g.ns, ".ns"), group}));
      }
      auto arrTy = ArrayType::get(refTy, refs.size());
      new GlobalVariable(*mod, arrTy, true, GlobalValue::ExternalLinkage,
                         ConstantArray::get(arrTy, refs),
                         "culebra_aot_ns_groups");
      new GlobalVariable(*mod, i64, true, GlobalValue::ExternalLinkage,
                         ConstantInt::get(i64, refs.size()),
                         "culebra_aot_ns_group_count");
    }

    if (opt_level > 0) JIT::optimize_module(*mod, opt_level);
    if (emit_llvm) mod->print(outs(), nullptr);

    return emit_object_file(*tm, *mod, out_path, "culebra build");
  }

  // Lower the whole program into `j`'s module and return its top-level
  // function, named `entry`. Shared by everything a lowered program can
  // become: code in this process (run_program), a program object
  // (build_object), and one baked stdlib module (build_preamble_object,
  // which is why the name is a parameter). `baked` names the stdlib modules
  // resolve_baked_preamble took out of the program: the entry opens with a
  // call to each one's `culebra_preamble_<Name>`, where the spliced
  // registrations used to be, and the lane's link supplies the symbol —
  // the JIT from the driver's table (JIT::define_baked_preambles), the
  // built binary from libculebra_rt.a.
  static llvm::Function* lower_program(
      JIT& j, const VmProgram& p, const char* entry = "__culebra_main",
      std::span<const BakedPreamble* const> baked = {}) {
    using namespace llvm;
    j.declare_runtime_functions();

    // One LLVM function per chunk: the top level keeps the __culebra_main
    // entry shape; function chunks get the JitFn ABI, so MakeClosure hands
    // their address to closure_new exactly like a JIT-compiled function.
    auto ptrTy = PointerType::get(j.ctx_, 0);
    auto jitFnTy = jit_fn_type(j.builder_, ptrTy);
    std::vector<Function*> fns(p.chunks.size());
    fns[0] = Function::Create(FunctionType::get(j.builder_.getVoidTy(), false),
                              Function::ExternalLinkage, entry, j.module_);
    for (size_t i = 1; i < p.chunks.size(); ++i) {
      fns[i] = Function::Create(jitFnTy, Function::InternalLinkage,
                                std::format("__vm_fn_{}", i), j.module_);
    }
    // Filled on first use, per chunk (see param_meta_global).
    std::vector<llvm::Constant*> metas(p.chunks.size(), nullptr);
    for (size_t i = 0; i < p.chunks.size(); ++i) lower_chunk(j, p, i, fns, metas);
    if (!baked.empty()) {
      auto& entryBB = fns[0]->getEntryBlock();
      IRBuilder<> b(&entryBB, entryBB.begin());
      for (const auto* bp : baked)
        b.CreateCall(j.module_->getOrInsertFunction(
            baked_preamble_symbol(bp->name), b.getVoidTy()));
    }
    for (auto* fn : fns) {
      // Report and stop: verifyFunction's one-argument form returns the
      // verdict and prints nothing, so ignoring it let malformed IR (an
      // i1 result compared against an i8 zero, from a runtime helper whose
      // declared return type the arm mis-read) reach the optimizer and run.
      std::string err;
      raw_string_ostream os(err);
      if (verifyFunction(*fn, &os))
        throw std::runtime_error("vm: lowered IR failed verification: " + err);
    }
    return fns[0];
  }

  static void lower_chunk(JIT& j, const VmProgram& p, size_t chunk_idx,
                          const std::vector<llvm::Function*>& fns,
                          std::vector<llvm::Constant*>& metas) {
    using namespace llvm;
    const Chunk& c = p.chunks[chunk_idx];
    auto* fn = fns[chunk_idx];
    auto& b = j.builder_;
    auto i64Ty = b.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(j.ctx_, 0);
    // Every chunk carries a cleanup pad per lexical scope (they release the
    // scope's slots on the way out), so it always needs a personality.
    if (!c.cleanups.empty()) fn->setPersonalityFn(j.get_personality_fn());
    b.SetInsertPoint(BasicBlock::Create(j.ctx_, "entry", fn));
    // Per-LLVM-function JIT state: one chunk is one function, and a cleanup
    // pad's exception slot must be an alloca in THIS function's entry block.
    j.exc_slot_ = nullptr;
    // The unwind-temp pool is entry-block allocas too, so it belongs to one
    // function as much as the exception slot does — the shared emitters (the
    // for-in head's calls among them) spill through it.
    j.unwind_temp_slots_.clear();
    j.unwind_covered_.clear();
    // Same reason: the owned stack's hot pointer is cached as an SSA value
    // fetched in the function's prologue, so it cannot cross a chunk. Fetch
    // it here rather than at first use — a first use inside a try region
    // would be an invoke, whose result does not dominate the other blocks
    // that go on to read the cache.
    j.current_owned_hot_ = nullptr;
    llvm::Value* markArr = nullptr;
    if (c.owned_depths > 0) {
      j.current_owned_hot_ = b.CreateCall(
          j.module_->getOrInsertFunction(rt::owned_hot, b.getInt64Ty()), {},
          "owned.hot");
      // The frame's mark array, the executor's `marks` — entry-block, so a
      // loop body's scope entry does not grow the stack every turn.
      markArr = b.CreateAlloca(i64Ty, b.getInt64(c.owned_depths), "owned.marks");
    }
    auto owned_mark_ptr = [&](int32_t d) {
      return b.CreateConstInBoundsGEP1_64(i64Ty, markArr, d, "owned.mark.p");
    };
    auto load_owned_mark = [&](int32_t d) {
      return b.CreateLoad(i64Ty, owned_mark_ptr(d), "owned.mark");
    };

    // Frame-ABI arguments (function chunks only; see JitFn in rt_value.inc.h).
    llvm::Value* retPtr = nullptr;
    llvm::Value* clsArg = nullptr;
    llvm::Value* selfTag = nullptr;
    llvm::Value* selfData = nullptr;
    llvm::Value* nArgs = nullptr;
    llvm::Value* argsPtr = nullptr;
    if (chunk_idx != 0) {
      retPtr = fn->getArg(0);
      clsArg = fn->getArg(1);
      selfTag = fn->getArg(2);
      selfData = fn->getArg(3);
      nArgs = fn->getArg(4);
      argsPtr = fn->getArg(5);
    }

    // One Value alloca per slot, nil-initialized — mem2reg turns these into
    // SSA. (llvm::Value spelled out: the enclosing namespace's culebra::Value
    // wins over the using-directive.)
    std::vector<llvm::Value*> slots(c.num_slots);
    for (int32_t s = 0; s < c.num_slots; ++s) {
      slots[s] = b.CreateAlloca(j.valueType_, nullptr, c.slot_names[s]);
      b.CreateStore(j.make_nil(), slots[s]);
    }
    auto load_slot = [&](int32_t s) {
      return b.CreateLoad(j.valueType_, slots[s]);
    };
    // The value inside a cell whose pointer is already loaded — CellGet's
    // read, without its retain (a borrowed callee's, see Op::Call).
    auto load_cell_value = [&](llvm::Value* cellSlotVal) {
      auto cellPtr = b.CreateIntToPtr(j.extract_data(cellSlotVal), ptrTy);
      return b.CreateLoad(
          j.valueType_, b.CreateStructGEP(j.cellType_, cellPtr, 1, "cell.vp"),
          "cell.val");
    };
    // One for-in cursor per statement, keyed by its slot run's base. The
    // Values live in the run (so the ladders free them); the bookkeeping the
    // walk needs in native types lives in entry-block scratch, invisible to
    // the ladders because none of it is refcounted.
    std::map<int32_t, JIT::ForCursor> for_cursors;
    auto for_cursor = [&](int32_t base) -> JIT::ForCursor& {
      auto it = for_cursors.find(base);
      if (it != for_cursors.end()) return it->second;
      auto cur = j.make_for_cursor();
      cur.obj = slots[base + kForSrc];
      cur.iter = slots[base + kForIter];
      cur.elem = slots[base + kForElem];
      return for_cursors.emplace(base, cur).first->second;
    };
    // ForDispose, and the same sequence the cleanup pads run at the rung that
    // frees a loop's iterator. Guarded on the run's own latch, so the two
    // ladders a `break` walks cannot close the iterator twice.
    auto emit_for_dispose = [&](int32_t base, bool swallow) {
      auto doBB = BasicBlock::Create(j.ctx_, "vm.for.dispose", fn);
      auto doneBB = BasicBlock::Create(j.ctx_, "vm.for.disposed", fn);
      b.CreateCondBr(b.CreateICmpNE(
                         j.extract_data(load_slot(base + kForDisposed)),
                         b.getInt64(0)),
                     doneBB, doBB);
      b.SetInsertPoint(doBB);
      b.CreateStore(j.make_long(b.getInt64(1)), slots[base + kForDisposed]);
      j.emit_iter_dispose_if_active(slots[base + kForIter], "vm.for", swallow);
      b.CreateBr(doneBB);
      b.SetInsertPoint(doneBB);
    };
    // A const-table string as a plain C string global (the runtime entries
    // that take `const char*` rather than a culebra String value).
    auto vm_str_const = [&](int32_t k, const char* name) {
      return j.get_or_create_global_str(
          std::string(
              _str_sv(reinterpret_cast<const char*>(c.consts[k].data))),
          name);
    };

    // The property writes' shared receiver gate: fall through only for an
    // Object receiver, otherwise the interp's own error text from a
    // terminated block.
    auto require_object_recv = [&](llvm::Value* tag, const char* pfx) {
      auto okBB = BasicBlock::Create(j.ctx_, std::string(pfx) + ".ok", fn);
      auto errBB = BasicBlock::Create(j.ctx_, std::string(pfx) + ".err", fn);
      b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_OBJECT)), okBB, errBB);
      b.SetInsertPoint(errBB);
      j.emit_type_error_typed("Object, Array, or Tensor", tag);
      b.CreateUnreachable();
      b.SetInsertPoint(okBB);
    };

    // Where the overflow arguments begin, filled by the prologue and read
    // by ArgsRest (the two are far apart in the instruction stream).
    llvm::Value* argsFromSlot = nullptr;
    // Function-chunk prologue, mirroring both the JIT prologue and
    // Exec::run_frame: arity guard (ArityError at the published call site),
    // param binding, overflow-arg release, the `fn` handle, and the
    // recursion guard after binding.
    if (chunk_idx != 0) {
      if (c.required > 0 && !c.forwards_args) {
        auto tooFew =
            b.CreateICmpSLT(nArgs, b.getInt64(c.required), "too.few");
        auto errBB = BasicBlock::Create(j.ctx_, "arity.err", fn);
        auto okBB = BasicBlock::Create(j.ctx_, "arity.ok", fn);
        b.CreateCondBr(tooFew, errBB, okBB);
        b.SetInsertPoint(errBB);
        std::vector<Constant*> nameptrs;
        for (const auto& n : c.param_names)
          nameptrs.push_back(j.get_or_create_global_str(n, ".paramname"));
        auto* namesG = j.build_str_ptr_array(nameptrs, ".paramnames");
        // The receiver's +1 would strand on this edge (the frame's ladder
        // covers nothing yet) — the JIT's arity error releases it here too.
        j.emit_value_release(j.make_value(selfTag, selfData));
        // The prologue's own fallback position is unused: set_call_site
        // always ran on the caller side, and arity_missing prefers it.
        b.CreateCall(j.module_->getOrInsertFunction(
                         rt::arity_missing, b.getVoidTy(), ptrTy, i64Ty,
                         i64Ty, i64Ty),
                     {namesG, nArgs, b.getInt64(0), b.getInt64(0)});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
      }
      // Where the caller's values stop being bindings — see the executor's
      // own run_frame for why the rest slot's marker decides it. The same
      // answer bounds the overflow, which ArgsRest reads back from the slot.
      IRBuilder<> ebArgs(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      argsFromSlot = ebArgs.CreateAlloca(i64Ty, nullptr, "args.from");
      llvm::Value* fromArgs = b.getInt64(c.arity);
      if (c.kwargs_rest_idx >= 0) {
        int64_t regular_end = c.first_kw_only_idx >= 0 ? c.first_kw_only_idx
                                                       : c.kwargs_rest_idx;
        auto* preBB = b.GetInsertBlock();
        auto probeBB = BasicBlock::Create(j.ctx_, "kwrest.probe", fn);
        auto joinBB = BasicBlock::Create(j.ctx_, "kwrest.join", fn);
        b.CreateCondBr(b.CreateICmpSGT(nArgs, b.getInt64(c.kwargs_rest_idx)),
                       probeBB, joinBB);
        b.SetInsertPoint(probeBB);
        auto* rp = b.CreateConstGEP1_64(
            j.valueType_, argsPtr, static_cast<uint64_t>(c.kwargs_rest_idx));
        auto* marked = b.CreateICmpEQ(
            j.extract_tag(b.CreateLoad(j.valueType_, rp)),
            b.getInt8(TAG_KWREST), "kwrest.marked");
        b.CreateBr(joinBB);
        b.SetInsertPoint(joinBB);
        auto* phi = b.CreatePHI(b.getInt1Ty(), 2, "kwrest.resolved");
        phi->addIncoming(b.getInt1(false), preBB);
        phi->addIncoming(marked, probeBB);
        fromArgs = b.CreateSelect(phi, b.getInt64(c.arity),
                                  b.getInt64(regular_end), "args.from");
      }
      b.CreateStore(fromArgs, argsFromSlot);
      // Required slots are there by the guard above; a defaulted one may not
      // be, so its slot takes the TAG_UNFILLED sentinel its JumpIfFilled
      // tests (reading past the caller's slab is the thing to avoid).
      for (int32_t i = 0; !c.forwards_args && i < c.arity; ++i) {
        if (i < c.required && c.kwargs_rest_idx < 0) {
          auto* src = b.CreateConstGEP1_64(j.valueType_, argsPtr,
                                           static_cast<uint64_t>(i));
          b.CreateStore(b.CreateLoad(j.valueType_, src), slots[i]);
          continue;
        }
        auto has = b.CreateAnd(
            b.CreateICmpSGT(nArgs, b.getInt64(i), "arg.has"),
            b.CreateICmpSGT(fromArgs, b.getInt64(i), "arg.binds"));
        auto takeBB = BasicBlock::Create(j.ctx_, "arg.take", fn);
        auto unfBB = BasicBlock::Create(j.ctx_, "arg.unfilled", fn);
        auto contBB = BasicBlock::Create(j.ctx_, "arg.cont", fn);
        b.CreateCondBr(has, takeBB, unfBB);
        b.SetInsertPoint(takeBB);
        auto* src = b.CreateConstGEP1_64(j.valueType_, argsPtr,
                                         static_cast<uint64_t>(i));
        b.CreateStore(b.CreateLoad(j.valueType_, src), slots[i]);
        b.CreateBr(contBB);
        b.SetInsertPoint(unfBB);
        b.CreateStore(j.make_value(b.getInt8(TAG_UNFILLED), b.getInt64(0)),
                      slots[i]);
        b.CreateBr(contBB);
        b.SetInsertPoint(contBB);
      }
      // Release overflow args: for (iv = fromArgs; iv < nArgs; iv++). A
      // chunk that reads them keeps their `+1`s for the Array its ArgsRest
      // instruction builds.
      if (!c.forwards_args && !c.keeps_args) {
        auto* fromBB = b.GetInsertBlock();
        auto hdrBB = BasicBlock::Create(j.ctx_, "extras.hdr", fn);
        auto bodyBB = BasicBlock::Create(j.ctx_, "extras.body", fn);
        auto doneBB = BasicBlock::Create(j.ctx_, "extras.done", fn);
        b.CreateBr(hdrBB);
        b.SetInsertPoint(hdrBB);
        auto* iv = b.CreatePHI(i64Ty, 2, "iv");
        iv->addIncoming(fromArgs, fromBB);
        b.CreateCondBr(b.CreateICmpSLT(iv, nArgs), bodyBB, doneBB);
        b.SetInsertPoint(bodyBB);
        auto* src = b.CreateGEP(j.valueType_, argsPtr, iv);
        j.emit_value_release(b.CreateLoad(j.valueType_, src));
        auto* next = b.CreateAdd(iv, b.getInt64(1));
        iv->addIncoming(next, b.GetInsertBlock());
        b.CreateBr(hdrBB);
        b.SetInsertPoint(doneBB);
      }
      // The receiver, mirroring Exec::run_frame: a receiver frame's slot
      // takes the +1 (its teardown releases it), a SelfMerge frame keeps the
      // raw pair so the sentinel stays visible, and every other frame drops
      // it — the JIT ctor thunk's release of the class object it was called
      // on.
      if (c.self_slot >= 0) {
        auto selfVal = j.make_value(selfTag, selfData);
        if (!c.self_raw) {
          auto absent = b.CreateICmpEQ(selfTag, b.getInt8(TAG_NO_SELF),
                                       "self.absent");
          selfVal = b.CreateSelect(absent, j.make_nil(), selfVal, "self");
        }
        b.CreateStore(selfVal, slots[c.self_slot]);
      } else {
        j.emit_value_release(j.make_value(selfTag, selfData));
      }
      if (c.fn_slot >= 0) {
        auto fnVal = j.make_func(clsArg);
        b.CreateStore(fnVal, slots[c.fn_slot]);
        j.emit_value_retain(fnVal);
      }
    }
    // Region handlers (and the frame pad below) restore the recursion count
    // to the frame's own level (Exec::run_frame's frame_depth, the JIT's
    // try.rec snapshot — the depth is constant within a frame). A function
    // chunk's RecEnter instruction fills it once the parameters bind; the
    // "never entered" sentinel keeps every restore a no-op until then, so a
    // throw out of a default expression leaves the count alone.
    llvm::Value* depthSlot = nullptr;
    if (!c.cleanups.empty()) {
      IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      depthSlot = eb.CreateAlloca(i64Ty, nullptr, "eh.depth");
      llvm::Value* depth0 = b.getInt64(-1);
      if (chunk_idx == 0)
        depth0 = b.CreateCall(
            j.module_->getOrInsertFunction(rt::recursion_depth, i64Ty), {},
            "rec.depth");
      b.CreateStore(depth0, depthSlot);
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
        case Op::WkErr:
        case Op::DestrErr:
        case Op::Throw:
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfNotNil:
        case Op::JumpIfNil:
        case Op::JumpIfSame:
        case Op::JumpIfTag:
        case Op::JumpIfFilled:
        case Op::SeqChk:   // pattern mismatch edges, same encoding
        case Op::ObjGet:
        case Op::TypeMatch:
          mark(in.b);
          mark(static_cast<int32_t>(i) + 1);
          break;
        case Op::ForNext:  // the drained edge, and the step's own body
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

    // One landingpad per lexical scope, chained outward: it runs that scope's
    // pending defers, releases its own bindings, and hands the throw to its
    // encloser — the JIT's per-region cleanup pads, and the twin of
    // Exec::unwind. A try scope's pad classifies the exception after its
    // cleanup and enters the catch (its handler pc opens a block for the
    // binding). Setting current_lpad_ per instruction turns every may-throw
    // helper call into an invoke — emit_call's standard behavior, no
    // lowering-specific plumbing.
    for (const auto& cu : c.cleanups)
      if (cu.handler != Chunk::kNoHandler)
        mark(static_cast<int32_t>(cu.handler));
    std::vector<BasicBlock*> pads(c.cleanups.size());
    for (size_t k = 0; k < c.cleanups.size(); ++k)
      pads[k] = BasicBlock::Create(j.ctx_, std::format("vm.scope.{}", k), fn);
    // The call site, with this call's per-argument positions when it has
    // any — the JIT's emit_call_position_publish, whose packed array is
    // rodata here too (every position is a compile-time constant). The
    // executor's publish_call_site is the same decision.
    auto publish_site = [&](int64_t line, int64_t col,
                            const std::vector<int64_t>* argpos) {
      if (argpos) {
        std::vector<Constant*> packed;
        size_t n = std::min(argpos->size(),
                            static_cast<size_t>(_JIT_ARGPOS_MAX));
        packed.reserve(n);
        for (size_t k = 0; k < n; ++k) packed.push_back(b.getInt64((*argpos)[k]));
        auto* arrTy = llvm::ArrayType::get(i64Ty, n);
        auto* posG = new llvm::GlobalVariable(
            *j.module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, packed), ".vm.argpos");
        b.CreateCall(
            j.module_->getOrInsertFunction(rt::set_call_positions,
                                           b.getVoidTy(), i64Ty, i64Ty,
                                           i64Ty, ptrTy),
            {b.getInt64(line), b.getInt64(col),
             b.getInt64(static_cast<int64_t>(n)), posG});
      } else {
        b.CreateCall(
            j.module_->getOrInsertFunction(rt::set_call_site, b.getVoidTy(),
                                           i64Ty, i64Ty),
            {b.getInt64(line), b.getInt64(col)});
      }
    };
    // The tail every hand-off shares: the arg slab, the pre-call nil of the
    // slots the callee consumes, and the ABI call. Those slots go nil BEFORE
    // the call — the slab alloca keeps the values alive for the callee and a
    // region's release ladder cannot double-release them on the unwind edge.
    // The receiver pair is already loaded, since it is read from a slot this
    // nils. Returns the call's result value.
    auto emit_abi_call = [&](llvm::FunctionCallee callee, llvm::Value* clsPtr,
                             llvm::Value* selfTagV, llvm::Value* selfDataV,
                             int32_t selfSlot, int32_t argBase,
                             int32_t argc) -> llvm::Value* {
      IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      llvm::Value* slab = ConstantPointerNull::get(cast<PointerType>(ptrTy));
      if (argc > 0) {
        auto* arrTy = ArrayType::get(j.valueType_, argc);
        slab = eb.CreateAlloca(arrTy, nullptr, "call.args");
        for (int32_t k = 0; k < argc; ++k)
          b.CreateStore(load_slot(argBase + k),
                        b.CreateConstGEP2_64(arrTy, slab, 0,
                                             static_cast<uint64_t>(k)));
      }
      auto* retTmp = eb.CreateAlloca(j.valueType_, nullptr, "call.ret");
      for (int32_t k = 0; k < argc; ++k)
        b.CreateStore(j.make_nil(), slots[argBase + k]);
      if (selfSlot >= 0) b.CreateStore(j.make_nil(), slots[selfSlot]);
      j.emit_call(callee, {retTmp, clsPtr, selfTagV, selfDataV,
                           b.getInt64(argc), slab});
      return b.CreateLoad(j.valueType_, retTmp);
    };
    // The JitFn hand-off, shared by Call / CallM and BMeth's user-method arm:
    // the TAG_FUNC gate and the tail above. `selfSlot < 0` is a plain call
    // (TAG_NO_SELF rides the receiver pair). Returns the call's result value.
    auto emit_invoke = [&](llvm::Value* calleeV, int32_t selfSlot,
                           int32_t argBase, int32_t argc, int64_t line,
                           int64_t col,
                           const std::vector<int64_t>* argpos = nullptr,
                           Chunk::CallTarget tgt = {}) -> llvm::Value* {
      publish_site(line, col, argpos);
      // Both resolved shapes end here: a direct call to the named chunk's
      // own function, with the receiver pair the ABI wants.
      auto emit_direct = [&](int32_t chunk, llvm::Value* clsPtr) {
        auto selfV = selfSlot >= 0 ? load_slot(selfSlot) : nullptr;
        return emit_abi_call(
            fns[static_cast<size_t>(chunk)], clsPtr,
            selfV ? j.extract_tag(selfV) : b.getInt8(TAG_NO_SELF),
            selfV ? j.extract_data(selfV) : b.getInt64(0), selfSlot, argBase,
            argc);
      };
      // A callee the compiler named (Chunk::call_targets): its code is this
      // module's own function, so the Function gate and the two probes have
      // no work to do and the ABI call is direct — which is what lets the
      // inliner see through it. The closure still rides the register: only
      // the code is static, its captures are the caller's.
      llvm::Value* fastResult = nullptr;
      llvm::BasicBlock* fastEndBB = nullptr;
      llvm::BasicBlock* mergeBB = nullptr;
      if (tgt.chunk >= 0) {
        auto clsPtr = b.CreateIntToPtr(j.extract_data(calleeV), ptrTy);
        // Statically resolvable too: the cap is the callee chunk's, the
        // count is this site's. Only the throwing case emits anything, and
        // it emits the very call the dynamic arm would have made. (A Mono
        // site needs none: it is recorded only for counts the overload
        // accepts, which stop at the keyword-only run.)
        auto emit_cap_check = [&] {
          int32_t cap =
              p.chunks[static_cast<size_t>(tgt.chunk)].first_kw_only_idx;
          if (cap >= 0 && argc > cap)
            j.emit_call(j.module_->getOrInsertFunction(
                            rt::check_pos_count_cls, b.getVoidTy(), ptrTy,
                            i64Ty, i64Ty, i64Ty),
                        {clsPtr, b.getInt64(argc), b.getInt64(line),
                         b.getInt64(col)});
        };
        // The one run-time question the site's shape owes, or null where it
        // owes none. Both shapes that have one emit the same diamond: the
        // direct call on the taken side, and on the other the arm the site
        // would have taken all along.
        llvm::Value* holds = nullptr;
        llvm::BasicBlock* dynBB = nullptr;
        auto open_diamond = [&] {
          dynBB = BasicBlock::Create(j.ctx_, "call.dyn", fn);
          mergeBB = BasicBlock::Create(j.ctx_, "call.join", fn);
        };
        switch (tgt.reach) {
          case Chunk::Reach::Direct:
            break;
          case Chunk::Reach::Mono: {
            // The code is static, but the closure the frame reads is the
            // dispatcher's monomorphic body, three loads away in its second
            // capture cell. A null payload means the shortcut is gone.
            auto caps = b.CreateLoad(
                ptrTy, b.CreateStructGEP(j.closureType_, clsPtr, 3),
                "mono.caps");
            auto cellPtr = b.CreateLoad(
                ptrTy,
                b.CreateInBoundsGEP(ptrTy, caps,
                                    {b.getInt64(kMultifnMonoCapture)}),
                "mono.cell");
            auto valPtr =
                b.CreateStructGEP(j.cellType_, cellPtr, 1, "mono.vp");
            auto body = b.CreateLoad(
                i64Ty, b.CreateStructGEP(j.valueType_, valPtr, 1),
                "mono.body");
            holds = b.CreateICmpNE(body, b.getInt64(0));
            clsPtr = b.CreateIntToPtr(body, ptrTy);
            open_diamond();
            break;
          }
          case Chunk::Reach::Guarded: {
            // The resolution rests on the callee being that chunk's closure,
            // which a member reading its own class name off a foreign
            // receiver would not be. One pointer compare against the
            // function the target names — but only once the callee is known
            // to BE a closure: a non-Function has no `fn_ptr` to read, and
            // dereferencing its payload would fault where the executor's
            // call_target_holds simply answers no. Same question, same
            // order, on both lanes; the miss lands in the dynamic arm, whose
            // TAG_FUNC test raises the TypeError it always did.
            open_diamond();
            auto guardBB = BasicBlock::Create(j.ctx_, "call.guard", fn);
            b.CreateCondBr(
                b.CreateICmpEQ(j.extract_tag(calleeV), b.getInt8(TAG_FUNC)),
                guardBB, dynBB);
            b.SetInsertPoint(guardBB);
            holds = b.CreateICmpEQ(
                b.CreateLoad(ptrTy,
                             b.CreateStructGEP(j.closureType_, clsPtr, 1),
                             "cls.fn"),
                fns[static_cast<size_t>(tgt.chunk)]);
            break;
          }
        }
        if (!holds) {
          emit_cap_check();
          return emit_direct(tgt.chunk, clsPtr);
        }
        auto fastBB = BasicBlock::Create(j.ctx_, "call.resolved", fn);
        b.CreateCondBr(holds, fastBB, dynBB);
        b.SetInsertPoint(fastBB);
        if (tgt.reach != Chunk::Reach::Mono) emit_cap_check();
        fastResult = emit_direct(tgt.chunk, clsPtr);
        fastEndBB = b.GetInsertBlock();
        b.CreateBr(mergeBB);
        b.SetInsertPoint(dynBB);
      }
      auto tag = j.extract_tag(calleeV);
      auto* fromBB = b.GetInsertBlock();
      auto probeBB = BasicBlock::Create(j.ctx_, "call.probe", fn);
      auto errBB = BasicBlock::Create(j.ctx_, "call.err", fn);
      auto okBB = BasicBlock::Create(j.ctx_, "call.ok", fn);
      auto overloadBB = BasicBlock::Create(j.ctx_, "call.overload", fn);
      auto ctorBB = BasicBlock::Create(j.ctx_, "call.tryctor", fn);
      b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_FUNC)), okBB, probeBB);
      // The two cold-path probes, in the JIT's order: a callable instance
      // (`obj(args)` with a `__call__` method, own or a trait default)
      // becomes a method call on itself, and `C(args)` builds an instance
      // through the class object's borrowed `new` — the is_class gate keeps
      // a plain dict with a "new" key inert, so its TypeError is unchanged.
      auto* callee0 = calleeV;
      b.SetInsertPoint(probeBB);
      auto callM = j.emit_value_call(
          j.module_->getOrInsertFunction(rt::class_call_method, j.valueType_,
                                         b.getInt8Ty(), i64Ty),
          {tag, j.extract_data(callee0)}, "call.method");
      b.CreateCondBr(
          b.CreateICmpEQ(j.extract_tag(callM), b.getInt8(TAG_FUNC)),
          overloadBB, ctorBB);
      // The instance becomes the receiver: mint the +1 its frame consumes,
      // and release the receiver this call started with — on this arm
      // nothing else takes it (the JIT's own_self_on_error release).
      b.SetInsertPoint(overloadBB);
      j.emit_value_retain(callee0);
      if (selfSlot >= 0) j.emit_value_release(load_slot(selfSlot));
      auto* overloadEndBB = b.GetInsertBlock();
      b.CreateBr(okBB);

      b.SetInsertPoint(ctorBB);
      auto ctorV = j.emit_value_call(
          j.module_->getOrInsertFunction(rt::class_new_method, j.valueType_,
                                         b.getInt8Ty(), i64Ty),
          {tag, j.extract_data(callee0)}, "ctor.method");
      auto ctorOkBB = BasicBlock::Create(j.ctx_, "call.ctor", fn);
      b.CreateCondBr(
          b.CreateICmpEQ(j.extract_tag(ctorV), b.getInt8(TAG_FUNC)), ctorOkBB,
          errBB);
      // The class object becomes the constructor's receiver (the instance
      // keeps a +1 on it, JitObject::cls), exactly as the `__call__` arm
      // hands over the instance: mint its +1, release the one this call
      // started with.
      b.SetInsertPoint(ctorOkBB);
      j.emit_value_retain(callee0);
      if (selfSlot >= 0) j.emit_value_release(load_slot(selfSlot));
      auto* ctorEndBB = b.GetInsertBlock();
      b.CreateBr(okBB);
      b.SetInsertPoint(errBB);
      j.emit_call(j.module_->getOrInsertFunction(rt::type_error_typed,
                                                 b.getVoidTy(), i64Ty, i64Ty,
                                                 ptrTy, b.getInt8Ty()),
                  {b.getInt64(line), b.getInt64(col),
                   b.CreateGlobalString("Function"), tag});
      if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
      b.SetInsertPoint(okBB);
      auto* calleePhi = b.CreatePHI(j.valueType_, 3, "call.callee");
      calleePhi->addIncoming(callee0, fromBB);
      calleePhi->addIncoming(callM, overloadEndBB);
      calleePhi->addIncoming(ctorV, ctorEndBB);
      calleeV = calleePhi;
      // True on the two probe arms, where the receiver is the value that
      // was called — the callable instance, or the class object — rather
      // than this instruction's own.
      auto* ovPhi = b.CreatePHI(b.getInt1Ty(), 3, "call.overload");
      ovPhi->addIncoming(b.getInt1(false), fromBB);
      ovPhi->addIncoming(b.getInt1(true), overloadEndBB);
      ovPhi->addIncoming(b.getInt1(true), ctorEndBB);
      auto clsPtr = b.CreateIntToPtr(j.extract_data(calleeV), ptrTy);
      auto fnFieldPtr = b.CreateStructGEP(j.closureType_, clsPtr, 1, "fn.ptr");
      auto fnPtr = b.CreateLoad(ptrTy, fnFieldPtr, "fn");
      // A keyword-only parameter cannot be filled positionally, and the
      // callee is only known here — the JIT's own guard, at the same point
      // in its call.
      j.emit_call(j.module_->getOrInsertFunction(rt::check_pos_count_cls,
                                                 b.getVoidTy(), ptrTy, i64Ty,
                                                 i64Ty, i64Ty),
                  {clsPtr, b.getInt64(argc), b.getInt64(line),
                   b.getInt64(col)});
      auto selfV = selfSlot >= 0 ? load_slot(selfSlot) : nullptr;
      auto dynResult = emit_abi_call(
          llvm::FunctionCallee(jit_fn_type(b, ptrTy), fnPtr), clsPtr,
          b.CreateSelect(ovPhi, j.extract_tag(callee0),
                         selfV ? j.extract_tag(selfV)
                               : b.getInt8(TAG_NO_SELF)),
          b.CreateSelect(ovPhi, j.extract_data(callee0),
                         selfV ? j.extract_data(selfV) : b.getInt64(0)),
          selfSlot, argBase, argc);
      if (!mergeBB) return dynResult;
      auto* dynEndBB = b.GetInsertBlock();
      b.CreateBr(mergeBB);
      mergeBB->moveAfter(dynEndBB);
      b.SetInsertPoint(mergeBB);
      auto* joinPhi = b.CreatePHI(j.valueType_, 2, "call.resolved.join");
      joinPhi->addIncoming(fastResult, fastEndBB);
      joinPhi->addIncoming(dynResult, dynEndBB);
      return joinPhi;
    };

    // The temporaries in flight at a pc die before any scope's defers run, so
    // an instruction that has some unwinds to a prelude of its own that
    // releases them and then continues into the scope chain (the JIT's
    // release_unwind_temps, whose pool this stands in for). Preludes are
    // shared by every instruction with the same scope and the same set, and
    // filled after the walk like the scope pads themselves.
    struct TempPad {
      BasicBlock* bb;
      int32_t scope;
      std::vector<int32_t> temps;
    };
    std::vector<TempPad> temp_pads;
    // Keyed by what the pad releases, not by where it was asked for: two
    // statements abandoning the same temporaries want the same entry.
    std::map<std::pair<int32_t, std::vector<int32_t>>, BasicBlock*> temp_ix;
    auto lpad_for = [&](size_t pc) -> BasicBlock* {
      int32_t k = chunk_innermost_cleanup(c, pc);
      if (k < 0) return nullptr;
      BasicBlock* chain = pads[static_cast<size_t>(k)];
      // Only what this throw abandons — Exec::unwind's own floor.
      int32_t floor = chunk_temp_floor(c, pc);
      std::vector<int32_t> temps;
      for (int32_t s : chunk_temps_at(c, pc))
        if (s >= floor) temps.push_back(s);
      if (temps.empty()) return chain;
      // This runs per instruction, so the hit path (many pcs share one pad)
      // builds one vector, not two.
      auto key = std::pair{k, std::move(temps)};
      auto it = temp_ix.find(key);
      if (it != temp_ix.end()) return it->second;
      auto* bb = BasicBlock::Create(
          j.ctx_, std::format("vm.temps.{}", temp_pads.size()), fn);
      temp_pads.push_back({bb, k, key.second});
      temp_ix.emplace(std::move(key), bb);
      return bb;
    };

    // Pass 2: linear walk over the instructions. The chunk's position table
    // feeds the JIT's position state, so every emitter that bakes line/col
    // into calls (arith, comparisons, to_bool) attributes exactly as the
    // interp reports.
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
          // d=1 (a compound step) picks the in-place `_borrow` twin — the
          // executor's dispatch above and emit_arith_step share the choice.
          auto r = j.emit_arith_step(load_slot(in.b), load_slot(in.c), op,
                                     /*inplace=*/in.d != 0);
          b.CreateStore(r, slots[in.a]);
          break;
        }
        case Op::Pow: {
          // emit_arith_step's "**" arm, in-place when d=1.
          auto r = j.emit_arith_step(load_slot(in.b), load_slot(in.c), "**",
                                     /*inplace=*/in.d != 0);
          b.CreateStore(r, slots[in.a]);
          break;
        }
        case Op::JumpIfSame: {
          auto cur = load_slot(in.a);
          auto isTensor = b.CreateICmpEQ(j.extract_tag(cur),
                                         b.getInt8(TAG_TENSOR), "same.ten");
          auto sameData = b.CreateICmpEQ(j.extract_data(cur),
                                         j.extract_data(load_slot(in.c)),
                                         "same.data");
          b.CreateCondBr(b.CreateAnd(isTensor, sameData, "same.inplace"),
                         blocks.at(in.b),
                         blocks.at(static_cast<int32_t>(i) + 1));
          break;
        }
        case Op::MatMul: {
          // emit_arith_step's "@" arm has no in-place twin to choose; the
          // `_borrow` one is the VM's, as everywhere else.
          auto r = j.emit_arith_step(load_slot(in.b), load_slot(in.c), "@");
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
        case Op::ObjectNew: {
          auto obj = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_new, ptrTy), {},
              "vm.obj");
          b.CreateStore(j.make_object(obj), slots[in.a]);
          break;
        }
        case Op::ObjectNewShaped: {
          const auto& spec = c.object_shape_specs[in.b];
          auto [cacheGlobal, keysArray] = j.build_shape_cache_globals(
              spec.keys, ".obj.shape", j.obj_shape_counter_);
          auto obj = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_new_shaped, ptrTy,
                                             ptrTy, ptrTy, i64Ty),
              {cacheGlobal, keysArray,
               b.getInt64(static_cast<int64_t>(spec.keys.size()))},
              "vm.obj.shaped");
          b.CreateStore(j.make_object(obj), slots[in.a]);
          break;
        }
        case Op::ObjectSet: {
          // Unlike set_add, object_set consumes the value on EVERY exit
          // (including the positionless well-known-contract throw) — pull
          // tag/data into locals and nil the slot BEFORE the call, so a
          // throw into the landing pad never finds a stale value to
          // double-release.
          auto obj = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          auto vt = j.extract_tag(v);
          auto vd = j.extract_data(v);
          b.CreateStore(j.make_nil(), slots[in.b]);
          auto* nm = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto keyPtr = j.get_or_create_global_str(nm, ".vm.objkey");
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::object_set, b.getVoidTy(), ptrTy, ptrTy, b.getInt1Ty(),
                  b.getInt8Ty(), i64Ty, i64Ty, i64Ty, b.getInt1Ty()),
              {obj, keyPtr, b.getInt1(in.d != 0), vt, vd,
               j.current_line_val(), j.current_column_val(),
               b.getInt1(true)});
          break;
        }
        case Op::ObjectSetAny: {
          // object_set_any consumes both the key and the value on every
          // exit (including the positionless unhashable/well-known throw)
          // — same pre-nil-before-call reasoning as ObjectSet.
          auto obj = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto k = load_slot(in.b);
          auto kt = j.extract_tag(k);
          auto kd = j.extract_data(k);
          auto v = load_slot(in.c);
          auto vt = j.extract_tag(v);
          auto vd = j.extract_data(v);
          b.CreateStore(j.make_nil(), slots[in.b]);
          b.CreateStore(j.make_nil(), slots[in.c]);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::object_set_any, b.getVoidTy(), ptrTy, b.getInt8Ty(),
                  i64Ty, b.getInt1Ty(), b.getInt8Ty(), i64Ty, i64Ty, i64Ty,
                  b.getInt1Ty()),
              {obj, kt, kd, b.getInt1(in.d != 0), vt, vd,
               j.current_line_val(), j.current_column_val(),
               b.getInt1(true)});
          break;
        }
        case Op::ObjectMerge: {
          // object_merge borrows the source (retains each copied entry) —
          // the slot keeps its +1, freed later by the statement sweep.
          auto obj = b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy);
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::object_merge, b.getVoidTy(), ptrTy, b.getInt8Ty(),
                  i64Ty, i64Ty, i64Ty),
              {obj, j.extract_tag(v), j.extract_data(v),
               j.current_line_val(), j.current_column_val()});
          break;
        }
        case Op::ModReg: {
          auto v = load_slot(in.a);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::module_register,
                                             b.getVoidTy(), ptrTy,
                                             b.getInt8Ty(), i64Ty),
              {vm_str_const(in.b, ".vm.modpath"), j.extract_tag(v),
               j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.a]);  // the table took the +1
          break;
        }
        case Op::ModGet: {
          auto [line, col] = chunk_pos_at(c, i);
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          auto tagSlot = eb.CreateAlloca(b.getInt8Ty(), nullptr, "mod.tag");
          auto dataSlot = eb.CreateAlloca(i64Ty, nullptr, "mod.data");
          j.emit_call(
              j.module_->getOrInsertFunction(rt::module_get, b.getVoidTy(),
                                             ptrTy, ptrTy, ptrTy, i64Ty,
                                             i64Ty),
              {vm_str_const(in.b, ".vm.modpath"), tagSlot, dataSlot,
               b.getInt64(line), b.getInt64(col)});
          b.CreateStore(j.make_value(b.CreateLoad(b.getInt8Ty(), tagSlot),
                                     b.CreateLoad(i64Ty, dataSlot)),
                        slots[in.a]);
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
          // Entry-block allocas: this arm can sit inside a loop body, and a
          // current-block alloca would grow the stack every iteration.
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vseq.tag");
          auto outData = eb.CreateAlloca(i64Ty, nullptr, "vseq.data");
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
        case Op::EnumVariant: {
          auto [line, col] = chunk_pos_at(c, i);
          // Header-backed literals: build_variant stores both names as
          // culebra Strings, whose length lives in the bytes before the
          // pointer (a bare C global would read garbage there).
          auto name = [&](int32_t k) {
            return j.emit_str_literal(std::string(
                _str_sv(reinterpret_cast<const char*>(c.consts[k].data))));
          };
          auto variant = name(in.c);
          auto en = name(in.d);
          if (in.b == 0) {
            auto inst = j.emit_value_call(
                j.module_->getOrInsertFunction(rt::build_variant, j.valueType_,
                                               ptrTy, ptrTy, i64Ty, ptrTy,
                                               i64Ty, i64Ty, i64Ty),
                {variant, en, b.getInt64(0),
                 llvm::ConstantPointerNull::get(ptrTy), b.getInt64(0),
                 b.getInt64(line), b.getInt64(col)},
                "vm.variant");
            b.CreateStore(inst, slots[in.a]);
          } else {
            auto ctor = j.emit_call(
                j.module_->getOrInsertFunction(rt::make_variant_ctor, ptrTy,
                                               ptrTy, ptrTy, i64Ty),
                {variant, en, b.getInt64(in.b)}, "vm.varctor");
            b.CreateStore(j.make_func(ctor), slots[in.a]);
          }
          break;
        }
        case Op::TypeMatch: {
          auto v = load_slot(in.a);
          auto expected = vm_str_const(in.c, ".vm.tm.name");
          auto ok = j.emit_call(
              j.module_->getOrInsertFunction(rt::type_matches, b.getInt1Ty(),
                                             b.getInt8Ty(), i64Ty, ptrTy),
              {j.extract_tag(v), j.extract_data(v), expected}, "vm.tm");
          b.CreateCondBr(ok, blocks.at(static_cast<int32_t>(i) + 1),
                         blocks.at(in.b));
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
          // Entry-block allocas — same loop-body stack-growth trap as SeqGet.
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vobj.tag");
          auto outData = eb.CreateAlloca(i64Ty, nullptr, "vobj.data");
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
          // The JIT's own emitters, so the resolve, the inline cache and
          // the `self` binding are the same IR a `x.name` read compiles to.
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          auto recv = load_slot(in.b);
          // own_receiver=false: the register keeps its +1 across the read,
          // and the handler ladder is its sole releaser on a throw edge.
          auto view = j.emit_property_get(recv, key);
          if (in.op == Op::PropRaw) {
            // A borrowed object-slot view, which the register must own — except
            // for the three introspection names, whose fn_mode merge hands back
            // a +1 on every arm already.
            if (!fn_introspection_name(key)) j.emit_value_retain(view);
            b.CreateStore(view, slots[in.a]);
          } else {
            b.CreateStore(j.emit_property_value_read(recv, view, key),
                          slots[in.a]);
          }
          break;
        }
        case Op::HasProp: {
          // The UFCS gate — has_prop_apply's twin, term for term: the baked
          // tag mask, then an Object receiver's own probe. It reads a
          // concrete slot where interp's receiver_has_property also honours a
          // trait default; a built-in name reaches this gate only after the
          // user-method arm has declined, which is what makes the two agree.
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          auto recv = load_slot(in.b);
          auto tagv = j.extract_tag(recv);
          llvm::Value* stat = b.getFalse();
          for (int8_t t = 0; t < 16; ++t)
            if (bmeth_receiver_ok(static_cast<BRecvMask>(in.d & 0xFFFF), t))
              stat = b.CreateOr(stat, b.CreateICmpEQ(tagv, b.getInt8(t)));
          auto entryBB = b.GetInsertBlock();
          auto objBB = BasicBlock::Create(j.ctx_, "vhp.obj", fn);
          auto contBB = BasicBlock::Create(j.ctx_, "vhp.cont", fn);
          b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_OBJECT)), objBB,
                         contBB);
          b.SetInsertPoint(objBB);
          llvm::Value* objHit = j.emit_receiver_resolves_method(recv, key);
          if (in.d & kHasPropIterBit)
            objHit = b.CreateOr(
                objHit,
                j.emit_object_has(
                    b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                    j.get_or_create_global_str("next", ".vhp.next")));
          auto objEnd = b.GetInsertBlock();
          b.CreateBr(contBB);
          b.SetInsertPoint(contBB);
          auto objPhi = b.CreatePHI(b.getInt1Ty(), 2, "vhp.obj.hit");
          objPhi->addIncoming(b.getFalse(), entryBB);
          objPhi->addIncoming(objHit, objEnd);
          b.CreateStore(j.make_bool(b.CreateOr(stat, objPhi)), slots[in.a]);
          break;
        }
        case Op::Drop: {
          // The at-most-once guard, borrowing the receiver — the compiler's
          // emit_explicit_drop without its consume (a register keeps owning
          // what it holds).
          auto recv = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::explicit_drop, b.getVoidTy(),
                                             b.getInt8Ty(), i64Ty),
              {j.extract_tag(recv), j.extract_data(recv)});
          b.CreateStore(j.make_nil(), slots[in.a]);
          break;
        }
        case Op::ClsParamsChk: {
          // compile_class_parameters_call's `useAuto`: an Object carrying a
          // `class` tag and resolving no `parameters` of its own.
          auto recv = load_slot(in.b);
          auto entryBB = b.GetInsertBlock();
          auto objBB = BasicBlock::Create(j.ctx_, "vcp.obj", fn);
          auto contBB = BasicBlock::Create(j.ctx_, "vcp.cont", fn);
          b.CreateCondBr(
              b.CreateICmpEQ(j.extract_tag(recv), b.getInt8(TAG_OBJECT)), objBB,
              contBB);
          b.SetInsertPoint(objBB);
          auto objPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
          auto hasClass = j.emit_object_has(
              objPtr, j.get_or_create_global_str("class", ".vcp.ck"));
          auto hasUser = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_has_or_trait_default,
                                             b.getInt1Ty(), ptrTy, ptrTy),
              {objPtr, j.get_or_create_global_str("parameters", ".vcp.pk")},
              "vcp.has.user");
          auto useAuto = b.CreateAnd(hasClass, b.CreateNot(hasUser));
          auto objEnd2 = b.GetInsertBlock();
          b.CreateBr(contBB);
          b.SetInsertPoint(contBB);
          auto phi = b.CreatePHI(b.getInt1Ty(), 2, "vcp.auto");
          phi->addIncoming(b.getFalse(), entryBB);
          phi->addIncoming(useAuto, objEnd2);
          b.CreateStore(j.make_bool(phi), slots[in.a]);
          break;
        }
        case Op::ClsParamsWalk:
          b.CreateStore(
              j.emit_value_call(
                  j.module_->getOrInsertFunction(rt::class_parameters_walk,
                                                 j.valueType_, ptrTy),
                  {b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy)},
                  "vcp.walk"),
              slots[in.a]);
          break;
        case Op::MethGate: {
          // checkBB then the receiver gate, in compile_user_method_over_
          // builtin's order, with the JIT's own property emitter.
          std::string key(_str_sv(
              reinterpret_cast<const char*>(c.consts[in.c].data)));
          const BMethSpec& gate = bmeth_specs()[in.d];
          auto id = gate.id;
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
          auto mask = gate.recv;
          // The gate-passing edges land here first when this call publishes
          // its own position (see bmeth_publishes_call_pos) — the user arm
          // above must not, since its callee publishes its own call site.
          auto okBB = bmeth_publishes_call_pos(id)
                          ? BasicBlock::Create(j.ctx_, "vbm.pubpos", fn)
                          : contBB;
          if (mask == kRecvAny) {
            b.CreateBr(okBB);  // no gate: every receiver resolves the name
          } else {
            auto badBB = BasicBlock::Create(j.ctx_, "vbm.rcverr", fn);
            // An iterator-protocol name resolves on an Object only when the
            // object carries the protocol; a plain dict merely lacks it.
            llvm::BasicBlock* shapeBB = okBB;
            if (gate.obj_iter_shaped && bmeth_receiver_ok(mask, TAG_OBJECT))
              shapeBB = BasicBlock::Create(j.ctx_, "vbm.itershape", fn);
            auto sw = b.CreateSwitch(tag, badBB, std::popcount(mask));
            for (int8_t t = 0; t < 16; ++t)
              if (bmeth_receiver_ok(mask, t))
                sw->addCase(b.getInt8(t), t == TAG_OBJECT ? shapeBB : okBB);
            if (shapeBB != okBB) {
              b.SetInsertPoint(shapeBB);
              b.CreateCondBr(
                  j.emit_object_has(
                      b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                      j.get_or_create_global_str("next", ".it.next")),
                  okBB, badBB);
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
          if (okBB != contBB) {
            b.SetInsertPoint(okBB);
            j.emit_set_op_pos();
            b.CreateBr(contBB);
          }

          b.SetInsertPoint(contBB);
          break;
        }
        case Op::CallRecv: {
          auto recv = load_slot(in.a);
          auto key = vm_str_const(in.c, ".vm.callrecv.key");
          b.CreateStore(
              j.emit_value_call(
                  j.module_->getOrInsertFunction(rt::call_receiver,
                                                 j.valueType_, b.getInt8Ty(),
                                                 i64Ty, ptrTy),
                  {j.extract_tag(recv), j.extract_data(recv), key},
                  "vm.callrecv"),
              slots[in.a]);
          break;
        }
        case Op::CbType: {
          auto [line, col] = chunk_pos_at(c, i);
          auto gate = load_slot(in.b);
          auto chkBB = BasicBlock::Create(j.ctx_, "vbm.cb.chk", fn);
          auto contBB = BasicBlock::Create(j.ctx_, "vbm.cb.cont", fn);
          b.CreateCondBr(
              b.CreateAnd(
                  b.CreateICmpEQ(j.extract_tag(gate), b.getInt8(TAG_NO_SELF)),
                  b.CreateICmpEQ(j.extract_data(gate),
                                 b.getInt64(kBMethGateBuiltin))),
              chkBB, contBB);
          b.SetInsertPoint(chkBB);
          // Same two calls the executor makes: publish the argument's site
          // (the helper's throw reads it), then check the declared type.
          j.emit_call(j.module_->getFunction(rt::set_callback_arg_site),
                      {b.getInt64(line), b.getInt64(col)});
          auto cb = load_slot(in.a);
          j.emit_call(j.module_->getFunction(rt::check_callback_type),
                      {j.extract_tag(cb), j.extract_data(cb),
                       vm_str_const(in.c, ".cb.param")});
          b.CreateBr(contBB);
          b.SetInsertPoint(contBB);
          break;
        }
        case Op::ArityChk: {
          // One arm per receiver tag the rival arity resolves on — the
          // message is a compile-time fact of (tag, shape), so only the
          // iterator-shape probe survives into the IR.
          const BMethSpec& spec = bmeth_specs()[in.c];
          auto [line, col] = chunk_pos_at(c, i);
          auto gate = load_slot(in.a);
          auto recv = load_slot(in.a + 1);
          auto contBB = BasicBlock::Create(j.ctx_, "vbm.ar.cont", fn);
          auto missBB = BasicBlock::Create(j.ctx_, "vbm.ar.miss", fn);
          b.CreateCondBr(
              b.CreateAnd(
                  b.CreateICmpEQ(j.extract_tag(gate), b.getInt8(TAG_NO_SELF)),
                  b.CreateICmpEQ(j.extract_data(gate),
                                 b.getInt64(kBMethGateMiss))),
              missBB, contBB);
          auto throw_msg = [&](const std::string& msg) {
            j.emit_throw_error("ArityError", msg, line, col);
            if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          };
          std::vector<std::pair<int8_t, llvm::BasicBlock*>> arms;
          for (int8_t t = 0; t < 16; ++t) {
            auto plain = bmeth_rival_arity_message(spec, t, false);
            auto shaped = bmeth_rival_arity_message(spec, t, true);
            if (plain.empty() && shaped.empty()) continue;
            auto armBB = BasicBlock::Create(j.ctx_, "vbm.ar.t", fn);
            arms.emplace_back(t, armBB);
            b.SetInsertPoint(armBB);
            if (t != TAG_OBJECT || plain == shaped) {
              throw_msg(plain.empty() ? shaped : plain);
              continue;
            }
            auto shapedBB = BasicBlock::Create(j.ctx_, "vbm.ar.shaped", fn);
            auto plainBB = BasicBlock::Create(j.ctx_, "vbm.ar.plain", fn);
            b.CreateCondBr(
                j.emit_object_has(
                    b.CreateIntToPtr(j.extract_data(recv), ptrTy),
                    j.get_or_create_global_str("next", ".it.next")),
                shapedBB, plainBB);
            b.SetInsertPoint(shapedBB);
            if (shaped.empty()) b.CreateBr(contBB); else throw_msg(shaped);
            b.SetInsertPoint(plainBB);
            if (plain.empty()) b.CreateBr(contBB); else throw_msg(plain);
          }
          b.SetInsertPoint(missBB);
          auto sw = b.CreateSwitch(j.extract_tag(recv), contBB,
                                   static_cast<unsigned>(arms.size()));
          for (auto& [t, armBB] : arms) sw->addCase(b.getInt8(t), armBB);
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
          // The same tag set the executor tests, walked bit by bit — see
          // bmeth_param_tags. `Any` emits no ChkParam at all, so the mask
          // here always names a real set.
          llvm::Value* ok = b.getFalse();
          auto tags = bmeth_param_tags(kind);
          for (int8_t t = 0; t < 16; ++t)
            if (bmeth_receiver_ok(tags, t))
              ok = b.CreateOr(ok, b.CreateICmpEQ(argTag, b.getInt8(t)));
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
          b.CreateStore(emit_invoke(gate, in.b + 1, in.b + 2, in.d, line, col,
                                    chunk_argpos_at(c, i)),
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
          // A keyword-only slot as the helper's `bool`: the type check ran
          // before the call, so the payload is the Bool's own bit.
          auto kw_flag = [&](int32_t k) {
            return b.CreateICmpNE(j.extract_data(arg(k)), b.getInt64(0),
                                  "vbm.kw");
          };
          // A non-Function callback reports at its own argument: publish the
          // site the runtime helper's check reads.
          if (int8_t cb =
                  bmeth_callback_arg(static_cast<BMeth>(in.c),
                                     static_cast<int8_t>(in.d));
              cb >= 0) {
            if (const auto* ap = chunk_argpos_at(c, i);
                ap && cb < static_cast<int8_t>(ap->size()))
              j.emit_call(
                  j.module_->getOrInsertFunction(rt::set_callback_arg_site,
                                                 b.getVoidTy(), i64Ty, i64Ty),
                  {b.getInt64((*ap)[cb] >> 32),
                   b.getInt64((*ap)[cb] & 0xffffffff)});
          }
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
                  {cstr(recv), cstr(arg(0)), j.extract_data(arg(1)),
                   b.getInt1(false), j.current_line_val(),
                   j.current_column_val()},
                  "vbm.sp"));
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
            case BMeth::IndexOfFrom:
            case BMeth::IndexOf: {
              // Two receivers under one name: the Array arm and the String
              // arm, joined through an entry-block alloca like the other
              // polymorphic ones. String/StringView is the default, which is
              // how the executor reads it too.
              bool is_rev = static_cast<BMeth>(in.c) == BMeth::Reverse;
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.rio");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.rio.arr", fn);
              auto strBB = BasicBlock::Create(j.ctx_, "vbm.rio.str", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.rio.join", fn);
              auto sw = b.CreateSwitch(j.extract_tag(recv), strBB, 1);
              sw->addCase(b.getInt8(TAG_ARRAY), arrBB);
              b.SetInsertPoint(arrBB);
              if (is_rev) {
                j.emit_call(j.module_->getOrInsertFunction(
                                rt::array_reverse, b.getVoidTy(), ptrTy),
                            {arr()});
                b.CreateStore(j.make_nil(), out);
              } else {
                // A too-deep element raises a positionless ValueError from
                // the compare.
                j.emit_set_op_pos();
                b.CreateStore(
                    j.make_long(j.emit_call(
                        j.module_->getOrInsertFunction(
                            rt::array_index_of, i64Ty, ptrTy, b.getInt8Ty(),
                            i64Ty),
                        {arr(), j.extract_tag(arg(0)),
                         j.extract_data(arg(0))},
                        "vbm.iof")),
                    out);
              }
              b.CreateBr(joinBB);
              b.SetInsertPoint(strBB);
              if (is_rev) {
                b.CreateStore(
                    j.make_string(j.emit_call(
                        j.module_->getFunction(rt::str_reverse), {cstr(recv)},
                        "vbm.srev")),
                    out);
              } else {
                b.CreateStore(
                    j.make_long(j.emit_call(
                        j.module_->getFunction(rt::str_index_of),
                        {cstr(recv), cstr(arg(0)), j.extract_data(arg(1))},
                        "vbm.siof")),
                    out);
              }
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out, "vbm.rio.v");
              break;
            }
            case BMeth::Title:
              res = str_fn(rt::str_title, {cstr(recv)});
              break;
            case BMeth::Normalize:
              res = str_fn(rt::str_normalize,
                           {cstr(recv), cstr(arg(0)), b.getInt64(line),
                            b.getInt64(col)});
              break;
            case BMeth::StripPrefix:
              res = str_fn(rt::str_strip_prefix, {cstr(recv), cstr(arg(0))});
              break;
            case BMeth::StripSuffix:
              res = str_fn(rt::str_strip_suffix, {cstr(recv), cstr(arg(0))});
              break;
            case BMeth::EqIgnoreCase:
              // Declared i1-returning, so the result rides straight into the
              // Bool (the StartsWith/EndsWith shape).
              res = j.make_bool(j.emit_call(
                  j.module_->getFunction(rt::str_eq_ignore_case),
                  {cstr(recv), cstr(arg(0))}, "vbm.eqic"));
              break;
            case BMeth::LastIndexOf:
              res = j.make_long(j.emit_call(
                  j.module_->getFunction(rt::str_last_index_of),
                  {cstr(recv), cstr(arg(0))}, "vbm.liof"));
              break;
            case BMeth::RSplit:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::str_split),
                  {cstr(recv), cstr(arg(0)), j.extract_data(arg(1)),
                   b.getInt1(true), j.current_line_val(),
                   j.current_column_val()},
                  "vbm.rsp"));
              break;
            case BMeth::SplitWhitespace:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::str_split_whitespace),
                  {cstr(recv)}, "vbm.spw"));
              break;
            case BMeth::IsDigit:
            case BMeth::IsAlpha:
            case BMeth::IsAlnum:
            case BMeth::IsSpace:
            case BMeth::IsAscii:
              res = j.make_bool(j.emit_call(
                  j.module_->getFunction(rt::str_is_class),
                  {cstr(recv),
                   b.getInt64(bmeth_str_class(static_cast<BMeth>(in.c)))},
                  "vbm.isc"));
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
              // The gate already proved Array; `reverse:` rides the trailing
              // keyword-only slot, type-checked before the call.
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::array_sorted),
                  {arr(), kw_flag(0), b.getInt64(line), b.getInt64(col)},
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
              j.emit_call(
                  j.module_->getFunction(rt::array_sort_by),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   kw_flag(1), b.getInt64(line), b.getInt64(col)});
              res = j.make_nil();
              break;
            case BMeth::SortedBy:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::array_sorted_by),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   kw_flag(1), b.getInt64(line), b.getInt64(col)},
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
              bool is_add = static_cast<BMeth>(in.c) == BMeth::Add;
              auto setArm = [&]() {
                auto r = j.emit_call(
                    j.module_->getOrInsertFunction(
                        is_add ? rt::set_add_method : rt::set_remove,
                        b.getInt8Ty(), ptrTy, b.getInt8Ty(), i64Ty, i64Ty,
                        i64Ty),
                    {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                     b.getInt64(line), b.getInt64(col)},
                    "vbm.setm");
                return j.make_bool(b.CreateICmpNE(r, b.getInt8(0)));
              };
              if (is_add) {  // an Object's `add` is a user property, never here
                res = setArm();
                break;
              }
              // `remove` splits by receiver: a Set drops a member and answers
              // whether it held one, an Object drops a dict key and answers
              // nil. The key stays slot-owned on both arms — the dict store
              // takes a `+1`, so mint it (see the executor's own arm).
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.rm");
              auto setBB = BasicBlock::Create(j.ctx_, "vbm.rm.set", fn);
              auto objBB2 = BasicBlock::Create(j.ctx_, "vbm.rm.obj", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.rm.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_OBJECT)),
                             objBB2, setBB);
              b.SetInsertPoint(setBB);
              b.CreateStore(setArm(), out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(objBB2);
              j.emit_value_retain(arg(0));
              j.emit_call(
                  j.module_->getOrInsertFunction(rt::object_remove_any,
                                                 b.getVoidTy(), ptrTy,
                                                 b.getInt8Ty(), i64Ty, i64Ty,
                                                 i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(line), b.getInt64(col)});
              b.CreateStore(j.make_nil(), out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
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
              j.emit_set_op_pos();  // positionless above rank 2
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
            case BMeth::Keys:
              res = j.make_array(j.emit_call(
                  j.module_->getOrInsertFunction(rt::object_keys, ptrTy,
                                                 ptrTy),
                  {arr()}, "vbm.keys"));
              break;
            case BMeth::Has:
              res = j.make_bool(j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::object_has_value, b.getInt1Ty(), ptrTy,
                      b.getInt8Ty(), i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0))},
                  "vbm.has"));
              break;
            // Array indexes by Long, Object looks up by key — the gate
            // proved the tag is one of the two.
            case BMeth::Get: {
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.get");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.get.arr", fn);
              auto objBB = BasicBlock::Create(j.ctx_, "vbm.get.obj", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.get.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             objBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  j.emit_value_call(
                      j.module_->getOrInsertFunction(
                          rt::array_get_default, j.valueType_, ptrTy, i64Ty,
                          b.getInt8Ty(), i64Ty),
                      {arr(), j.extract_data(arg(0)), j.extract_tag(arg(1)),
                       j.extract_data(arg(1))},
                      "vbm.get.a"),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(objBB);
              b.CreateStore(
                  j.emit_value_call(
                      j.module_->getOrInsertFunction(
                          rt::object_get_default, j.valueType_, ptrTy,
                          b.getInt8Ty(), i64Ty, b.getInt8Ty(), i64Ty, i64Ty,
                          i64Ty),
                      {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                       j.extract_tag(arg(1)), j.extract_data(arg(1)),
                       b.getInt64(line), b.getInt64(col)},
                      "vbm.get.o"),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::GetOrPut:
              res = j.emit_value_call(
                  j.module_->getOrInsertFunction(
                      rt::object_get_or_put, j.valueType_, ptrTy,
                      b.getInt8Ty(), i64Ty, b.getInt8Ty(), i64Ty, i64Ty,
                      i64Ty),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   j.extract_tag(arg(1)), j.extract_data(arg(1)),
                   b.getInt64(line), b.getInt64(col)},
                  "vbm.gop");
              break;
            // --- The iterator sources ---------------------------------
            case BMeth::Iter: {
              // One block per receiver family, merged through an entry-block
              // alloca (ToArray's shape): Array/Tuple walk their members, a
              // Set walks a snapshot of its own, an Object asks its `iter`
              // before falling back to the (key, value) walk, and a String
              // yields its scalars.
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.it");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.it.arr", fn);
              auto setBB = BasicBlock::Create(j.ctx_, "vbm.it.set", fn);
              auto objBB2 = BasicBlock::Create(j.ctx_, "vbm.it.obj", fn);
              auto strBB = BasicBlock::Create(j.ctx_, "vbm.it.str", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.it.join", fn);
              auto arrIterFn = j.module_->getOrInsertFunction(rt::array_iter,
                                                              ptrTy, ptrTy);
              auto sw = b.CreateSwitch(tagv, strBB, 4);
              sw->addCase(b.getInt8(TAG_ARRAY), arrBB);
              sw->addCase(b.getInt8(TAG_TUPLE), arrBB);
              sw->addCase(b.getInt8(TAG_SET), setBB);
              sw->addCase(b.getInt8(TAG_OBJECT), objBB2);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  j.make_object(j.emit_call(arrIterFn, {arr()}, "vbm.it.a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(setBB);
              b.CreateStore(
                  j.make_object(j.emit_call(
                      j.module_->getOrInsertFunction(rt::set_iter, ptrTy,
                                                     ptrTy),
                      {arr()}, "vbm.it.s")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(objBB2);
              {
                // A Range walks its start..end sequence — split it out before
                // the generic Object path, which would walk the Range
                // object's own key/value pairs.
                auto rangeBB = BasicBlock::Create(j.ctx_, "vbm.it.rng", fn);
                auto plainBB = BasicBlock::Create(j.ctx_, "vbm.it.plain", fn);
                b.CreateCondBr(j.emit_is_range(recv), rangeBB, plainBB);
                b.SetInsertPoint(rangeBB);
                b.CreateStore(
                    j.make_object(j.emit_call(
                        j.module_->getOrInsertFunction(rt::range_iter, ptrTy,
                                                       i64Ty, i64Ty, i64Ty),
                        {j.extract_data(recv), b.getInt64(line),
                         b.getInt64(col)},
                        "vbm.it.r")),
                    out);
                b.CreateBr(joinBB);
                b.SetInsertPoint(plainBB);
              }
              b.CreateStore(
                  j.make_object(j.emit_call(
                      j.module_->getOrInsertFunction(rt::object_iter_dispatch,
                                                     ptrTy, ptrTy),
                      {arr()}, "vbm.it.o")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(strBB);
              b.CreateStore(
                  j.make_object(j.emit_call(
                      j.module_->getOrInsertFunction(rt::str_scalars, ptrTy,
                                                     ptrTy),
                      {cstr(recv)}, "vbm.it.t")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::CodePoints:
            case BMeth::Bytes:
            case BMeth::Graphemes: {
              auto id2 = static_cast<BMeth>(in.c);
              const char* sym = id2 == BMeth::CodePoints ? rt::str_code_points
                                : id2 == BMeth::Bytes    ? rt::str_bytes
                                                         : rt::str_graphemes;
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(sym, ptrTy, ptrTy),
                  {cstr(recv)}, "vbm.sw"));
              break;
            }
            case BMeth::SplitIter: {
              // "Lazy in API, eager underneath": split, then walk the pieces.
              auto pieces = j.emit_call(
                  j.module_->getFunction(rt::str_split),
                  {cstr(recv), cstr(arg(0)), b.getInt64(0), b.getInt1(false),
                   j.current_line_val(), j.current_column_val()},
                  "vbm.spi.a");
              auto it = j.emit_call(
                  j.module_->getOrInsertFunction(rt::array_iter, ptrTy, ptrTy),
                  {pieces}, "vbm.spi");
              j.emit_value_release(j.make_array(pieces));
              res = j.make_object(it);
              break;
            }
            case BMeth::StrCount:
              res = j.make_long(j.emit_call(
                  j.module_->getOrInsertFunction(rt::str_count, i64Ty, ptrTy,
                                                 ptrTy),
                  {cstr(recv), cstr(arg(0))}, "vbm.cnt"));
              break;
            case BMeth::Enumerate:
              // One runtime entry for both receivers — it normalises an Array
              // into a walker itself.
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(rt::enumerate_any, ptrTy,
                                                 b.getInt8Ty(), i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv)}, "vbm.enum"));
              break;
            case BMeth::ToObject:
            case BMeth::Unzip: {
              bool to_object = static_cast<BMeth>(in.c) == BMeth::ToObject;
              auto tagv = j.extract_tag(recv);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.tou");
              auto arrBB = BasicBlock::Create(j.ctx_, "vbm.tou.arr", fn);
              auto iterBB = BasicBlock::Create(j.ctx_, "vbm.tou.iter", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.tou.join", fn);
              auto wrap = [&](llvm::Value* p) {
                return to_object ? j.make_object(p) : j.make_tuple(p);
              };
              b.CreateCondBr(b.CreateICmpEQ(tagv, b.getInt8(TAG_ARRAY)), arrBB,
                             iterBB);
              b.SetInsertPoint(arrBB);
              b.CreateStore(
                  wrap(j.emit_call(
                      j.module_->getOrInsertFunction(
                          to_object ? rt::array_to_object : rt::array_unzip,
                          ptrTy, ptrTy, i64Ty, i64Ty),
                      {arr(), b.getInt64(line), b.getInt64(col)}, "vbm.tou.a")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(iterBB);
              j.emit_set_op_pos();  // the drive's own protocol error
              b.CreateStore(
                  wrap(j.emit_call(
                      j.module_->getOrInsertFunction(
                          to_object ? rt::iter_to_object : rt::iter_unzip,
                          ptrTy, b.getInt8Ty(), i64Ty, i64Ty, i64Ty),
                      {tagv, j.extract_data(recv), b.getInt64(line),
                       b.getInt64(col)},
                      "vbm.tou.i")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            // --- Iterator-only: the gate proved an iterator-shaped Object ---
            case BMeth::Take:
            case BMeth::Skip:
              // No position argument: neither can fail on its own, and the
              // MethGate publish already covers the drive's protocol error.
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(
                      static_cast<BMeth>(in.c) == BMeth::Take ? rt::iter_take
                                                              : rt::iter_skip,
                      ptrTy, b.getInt8Ty(), i64Ty, i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_data(arg(0))},
                  "vbm.itn"));
              break;
            case BMeth::StepBy:
            case BMeth::Chunks:
            case BMeth::Windows: {
              auto id2 = static_cast<BMeth>(in.c);
              const char* sym = id2 == BMeth::StepBy  ? rt::iter_step_by
                                : id2 == BMeth::Chunks ? rt::iter_chunks
                                                       : rt::iter_windows;
              // Each raises "n must be at least 1" from its own position.
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(sym, ptrTy, b.getInt8Ty(),
                                                 i64Ty, i64Ty, i64Ty, i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_data(arg(0)), b.getInt64(line), b.getInt64(col)},
                  "vbm.itnp"));
              break;
            }
            case BMeth::TakeWhile:
            case BMeth::SkipWhile:
            case BMeth::Tap:
            case BMeth::ChunkBy: {
              auto id2 = static_cast<BMeth>(in.c);
              const char* sym = id2 == BMeth::TakeWhile ? rt::iter_take_while
                                : id2 == BMeth::SkipWhile
                                    ? rt::iter_skip_while
                                : id2 == BMeth::Tap ? rt::iter_tap
                                                    : rt::iter_chunk_by;
              // The callback left the register run nil'd above
              // (bmeth_consumes_args): the factory owns it from here on.
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(sym, ptrTy, b.getInt8Ty(),
                                                 i64Ty, b.getInt8Ty(), i64Ty,
                                                 i64Ty, i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(line), b.getInt64(col)},
                  "vbm.itcb"));
              break;
            }
            case BMeth::Scan:
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(
                      rt::iter_scan, ptrTy, b.getInt8Ty(), i64Ty,
                      b.getInt8Ty(), i64Ty, b.getInt8Ty(), i64Ty, i64Ty,
                      i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   j.extract_tag(arg(1)), j.extract_data(arg(1)),
                   b.getInt64(line), b.getInt64(col)},
                  "vbm.scan"));
              break;
            case BMeth::Chain:
            case BMeth::Zip:
              // The other iterable is borrowed: the factory coerces it and
              // retains what it stores, so the slot keeps its own `+1`.
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(
                      static_cast<BMeth>(in.c) == BMeth::Chain ? rt::iter_chain
                                                               : rt::iter_zip,
                      ptrTy, b.getInt8Ty(), i64Ty, b.getInt8Ty(), i64Ty, i64Ty,
                      i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(line), b.getInt64(col)},
                  "vbm.itpair"));
              break;
            case BMeth::Collect:
              res = j.make_array(j.emit_call(
                  j.module_->getOrInsertFunction(rt::iter_collect, ptrTy,
                                                 b.getInt8Ty(), i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv)}, "vbm.coll"));
              break;
            case BMeth::IterCount:
              res = j.make_long(j.emit_call(
                  j.module_->getOrInsertFunction(rt::iter_count, i64Ty,
                                                 b.getInt8Ty(), i64Ty),
                  {j.extract_tag(recv), j.extract_data(recv)}, "vbm.icnt"));
              break;
            case BMeth::First:
            case BMeth::Last:
            case BMeth::Nth:
            case BMeth::Position: {
              // An exhausted iterator answers nil, so the element's `+1`
              // arrives through out-params like Find's.
              auto id2 = static_cast<BMeth>(in.c);
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto outTag = eb.CreateAlloca(b.getInt8Ty(), nullptr, "vbm.io.t");
              auto outData = eb.CreateAlloca(i64Ty, nullptr, "vbm.io.d");
              llvm::SmallVector<llvm::Value*, 8> a{j.extract_tag(recv),
                                                   j.extract_data(recv)};
              llvm::SmallVector<llvm::Type*, 8> ty{b.getInt8Ty(), i64Ty};
              if (id2 == BMeth::Nth) {
                a.push_back(j.extract_data(arg(0)));
                ty.push_back(i64Ty);
              } else if (id2 == BMeth::Position) {
                a.push_back(j.extract_tag(arg(0)));
                a.push_back(j.extract_data(arg(0)));
                ty.push_back(b.getInt8Ty());
                ty.push_back(i64Ty);
              }
              if (id2 == BMeth::Nth || id2 == BMeth::Position) {
                a.push_back(b.getInt64(line));
                a.push_back(b.getInt64(col));
                ty.push_back(i64Ty);
                ty.push_back(i64Ty);
              }
              a.push_back(outTag);
              a.push_back(outData);
              ty.push_back(ptrTy);
              ty.push_back(ptrTy);
              const char* sym = id2 == BMeth::First  ? rt::iter_first
                                : id2 == BMeth::Last ? rt::iter_last
                                : id2 == BMeth::Nth  ? rt::iter_nth
                                                     : rt::iter_position;
              j.emit_call(
                  j.module_->getOrInsertFunction(
                      sym, llvm::FunctionType::get(b.getVoidTy(), ty, false)),
                  a);
              res = j.make_value(b.CreateLoad(b.getInt8Ty(), outTag),
                                 b.CreateLoad(i64Ty, outData));
              break;
            }
            // Tensor-only. The gate proved TAG_TENSOR, so `arr()` is the
            // JitTensor*. Each helper that can raise does so positionless, so
            // the arms that can stamp first, exactly as the executor's do.
            case BMeth::Shape:
              res = j.make_array(j.emit_call(
                  j.module_->getFunction(rt::tensor_shape), {arr()},
                  "vbm.tshape"));
              break;
            case BMeth::Pow:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_binop),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(static_cast<int64_t>(culebra::Op::Pow))},
                  "vbm.tpow"));
              break;
            case BMeth::TGt:
            case BMeth::TLt:
            case BMeth::TGe:
            case BMeth::TLe:
            case BMeth::TEq:
            case BMeth::TNe: {
              culebra::Op cmp_op;
              switch (static_cast<BMeth>(in.c)) {
                case BMeth::TGt: cmp_op = culebra::Op::Gt; break;
                case BMeth::TLt: cmp_op = culebra::Op::Lt; break;
                case BMeth::TGe: cmp_op = culebra::Op::Ge; break;
                case BMeth::TLe: cmp_op = culebra::Op::Le; break;
                case BMeth::TEq: cmp_op = culebra::Op::Eq; break;
                default: cmp_op = culebra::Op::Ne; break;
              }
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_binop),
                  {j.extract_tag(recv), j.extract_data(recv),
                   j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   b.getInt64(static_cast<int64_t>(cmp_op))},
                  "vbm.tcmp"));
              break;
            }
            case BMeth::Clip:
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_clamp),
                  {arr(), j.extract_tag(arg(0)), j.extract_data(arg(0)),
                   j.extract_tag(arg(1)), j.extract_data(arg(1))},
                  "vbm.tclamp"));
              break;
            case BMeth::Rope:
              // pos (arg 0) is Long-typed -- raw data, no tag, same as
              // SumAxis/MaxAxis/Argmax's axis above; base (arg 1) is Any,
              // tag+data like clamp's lo/hi.
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_rope),
                  {arr(), j.extract_data(arg(0)), j.extract_tag(arg(1)),
                   j.extract_data(arg(1))},
                  "vbm.trope"));
              break;
            case BMeth::Transpose:
            case BMeth::Clone:
            case BMeth::RequiresGrad:
            case BMeth::Grad:
            case BMeth::Backward:
            case BMeth::ZeroGrad:
            case BMeth::Detach: {
              auto id2 = static_cast<BMeth>(in.c);
              const char* sym =
                  id2 == BMeth::Transpose      ? rt::tensor_transpose
                  : id2 == BMeth::Clone        ? rt::tensor_clone
                  : id2 == BMeth::RequiresGrad ? rt::tensor_requires_grad
                  : id2 == BMeth::Grad         ? rt::tensor_grad
                  : id2 == BMeth::Backward     ? rt::tensor_backward
                  : id2 == BMeth::ZeroGrad     ? rt::tensor_zero_grad
                                               : rt::tensor_detach;
              res = j.make_tensor(j.emit_call(j.module_->getFunction(sym),
                                              {arr()}, "vbm.tuna"));
              break;
            }
            case BMeth::Relu:
            case BMeth::Sigmoid:
            case BMeth::Softmax:
            case BMeth::TensorLog:
            case BMeth::Tanh:
            case BMeth::Sin:
            case BMeth::Cos:
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_unary),
                  {arr(),
                   b.getInt64(bmeth_unary_op(static_cast<BMeth>(in.c)))},
                  "vbm.tact"));
              break;
            case BMeth::Reshape:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_reshape),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tresh"));
              break;
            case BMeth::Mean:
              res = val_fn(rt::tensor_reduce_all,
                           {arr(), b.getInt64(static_cast<int64_t>(
                                       culebra::Op::Mean))});
              break;
            case BMeth::SumAxis:
            case BMeth::MeanAxis:
            case BMeth::MaxAxis:
            case BMeth::Argmax: {
              // `Long?` on all but argmax, so an explicit nil is the axis-less
              // reduction — one runtime test, the reading the interp's binder
              // gives it.
              auto op = b.getInt64(bmeth_reduce_op(static_cast<BMeth>(in.c)));
              IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
              auto out = eb.CreateAlloca(j.valueType_, nullptr, "vbm.trd");
              auto allBB = BasicBlock::Create(j.ctx_, "vbm.trd.all", fn);
              auto axBB = BasicBlock::Create(j.ctx_, "vbm.trd.ax", fn);
              auto joinBB = BasicBlock::Create(j.ctx_, "vbm.trd.join", fn);
              b.CreateCondBr(b.CreateICmpEQ(j.extract_tag(arg(0)),
                                            b.getInt8(TAG_NIL)),
                             allBB, axBB);
              b.SetInsertPoint(allBB);
              b.CreateStore(val_fn(rt::tensor_reduce_all, {arr(), op}), out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(axBB);
              j.emit_set_op_pos();  // out-of-range axis
              b.CreateStore(
                  j.make_tensor(j.emit_call(
                      j.module_->getFunction(rt::tensor_reduce_axis),
                      {arr(), op, j.extract_data(arg(0))}, "vbm.trax")),
                  out);
              b.CreateBr(joinBB);
              b.SetInsertPoint(joinBB);
              res = b.CreateLoad(j.valueType_, out);
              break;
            }
            case BMeth::Dot:
              j.emit_set_op_pos();  // rank check
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_dot),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tdot"));
              break;
            case BMeth::LinearSigmoid:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_linear_sigmoid),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy),
                   b.CreateIntToPtr(j.extract_data(arg(1)), ptrTy)},
                  "vbm.tls"));
              break;
            case BMeth::IndexSelect:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_index_select),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tidxsel"));
              break;
            case BMeth::SoftmaxCrossEntropy:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_softmax_cross_entropy),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tsmce"));
              break;
            case BMeth::Item:
              j.emit_set_op_pos();  // multi-element check
              res = val_fn(rt::tensor_item, {arr()});
              break;
            case BMeth::Unfold:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_unfold),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tunf"));
              break;
            case BMeth::Pad:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_pad),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tpad"));
              break;
            case BMeth::Fold:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_fold),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tfold"));
              break;
            case BMeth::Permute:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_permute),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tperm"));
              break;
            case BMeth::Narrow:
              j.emit_set_op_pos();
              res = j.make_tensor(j.emit_call(
                  j.module_->getFunction(rt::tensor_narrow),
                  {arr(), b.CreateIntToPtr(j.extract_data(arg(0)), ptrTy)},
                  "vbm.tnarrow"));
              break;
            case BMeth::Sort:
              // In place, answering nil.
              j.emit_call(j.module_->getFunction(rt::array_sort),
                          {arr(), kw_flag(0), b.getInt64(line),
                           b.getInt64(col)});
              res = j.make_nil();
              break;
            case BMeth::Values:
              res = j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(rt::object_values, ptrTy,
                                                 ptrTy),
                  {arr()}, "vbm.values"));
              break;
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
          // receiver's register is live until the statement sweep, so no
          // "receiver died between the two reads" window exists to protect
          // against.
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
        case Op::NsWrChk: {
          auto recv = load_slot(in.a);
          auto isObj = b.CreateICmpEQ(j.extract_tag(recv),
                                      b.getInt8(TAG_OBJECT), "nswr.is_obj");
          auto chkBB = BasicBlock::Create(j.ctx_, "vm.nswr.chk", fn);
          auto okBB = BasicBlock::Create(j.ctx_, "vm.nswr.ok", fn);
          b.CreateCondBr(isObj, chkBB, okBB);
          b.SetInsertPoint(chkBB);
          auto* nm = reinterpret_cast<const char*>(c.consts[in.c].data);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::check_namespace_write,
                                             b.getVoidTy(), ptrTy, ptrTy,
                                             i64Ty, i64Ty),
              {b.CreateIntToPtr(j.extract_data(recv), ptrTy),
               j.get_or_create_global_str(nm, ".vm.propname"),
               j.current_line_val(), j.current_column_val()});
          b.CreateBr(okBB);
          b.SetInsertPoint(okBB);
          break;
        }
        case Op::PropSet: {
          auto recv = load_slot(in.a);
          auto val = load_slot(in.b);
          auto tag = j.extract_tag(recv);
          require_object_recv(tag, "pset");
          // The retain feeds object_set's consuming store; the slot keeps
          // its +1 (IndexSet's rule) so the expression still reads it.
          j.emit_value_retain(val);
          auto* nm = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto objPtr = b.CreateIntToPtr(j.extract_data(recv), ptrTy);
          auto keyPtr = j.get_or_create_global_str(nm, ".vm.propname");
          auto i8Ty = b.getInt8Ty();

          // Per-callsite transitioning-write IC (JitPropSetIC): the same
          // "a plain `self.x = value` at this exact site always sees the
          // same starting shape" observation fix 4 already exploited for
          // property reads, applied to writes. Sentinel (void*)1 for both
          // shape fields — never null, never a real heap Shape* — so a
          // cold cache can't spuriously match a freshly-allocated
          // instance's shape == nullptr (JitPropIC's own convention).
          auto icTy =
              llvm::StructType::get(j.ctx_, {ptrTy, ptrTy, i64Ty, i8Ty});
          auto* sentinelPtr = llvm::ConstantExpr::getIntToPtr(
              llvm::ConstantInt::get(i64Ty, 1), ptrTy);
          auto* icInit = llvm::ConstantStruct::get(
              icTy, {sentinelPtr, sentinelPtr,
                     llvm::ConstantInt::get(i64Ty, 0), b.getInt8(0)});
          auto* icGlobal = new llvm::GlobalVariable(
              *j.module_, icTy, /*isConstant=*/false,
              llvm::GlobalValue::PrivateLinkage, icInit,
              ".prop.set.ic." + std::to_string(j.prop_set_ic_counter_++));

          auto shapeFieldPtr = b.CreateConstInBoundsGEP1_64(
              i8Ty, objPtr, offsetof(JitObject, shape), "pset.shape.fieldp");
          auto objShape = b.CreateLoad(ptrTy, shapeFieldPtr, "pset.obj.shape");
          auto icExpectedPtr =
              b.CreateStructGEP(icTy, icGlobal, 0, "pset.ic.exp.p");
          auto icExpected = b.CreateLoad(ptrTy, icExpectedPtr, "pset.ic.exp");
          auto shapeMatch =
              b.CreateICmpEQ(objShape, icExpected, "pset.shape.match");

          auto fastBB = BasicBlock::Create(j.ctx_, "pset.fast", fn);
          auto slowBB = BasicBlock::Create(j.ctx_, "pset.slow", fn);
          auto mergeBB = BasicBlock::Create(j.ctx_, "pset.merge", fn);
          b.CreateCondBr(shapeMatch, fastBB, slowBB);

          b.SetInsertPoint(fastBB);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::object_set_fast, b.getVoidTy(), ptrTy, ptrTy, ptrTy,
                  i8Ty, i64Ty, i64Ty, i64Ty, b.getInt1Ty(), b.getInt1Ty()),
              {objPtr, keyPtr, icGlobal, j.extract_tag(val),
               j.extract_data(val), j.current_line_val(),
               j.current_column_val(), b.getInt1(true), b.getInt1(false)});
          b.CreateBr(mergeBB);

          b.SetInsertPoint(slowBB);
          j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::object_set_ic, b.getVoidTy(), ptrTy, ptrTy, ptrTy,
                  b.getInt1Ty(), i8Ty, i64Ty, i64Ty, i64Ty, b.getInt1Ty(),
                  i64Ty, i64Ty),
              {objPtr, keyPtr, icGlobal, b.getInt1(true), j.extract_tag(val),
               j.extract_data(val), j.current_line_val(),
               j.current_column_val(), b.getInt1(false),
               b.getInt64(c.consts[in.d].data >> 32),
               b.getInt64(c.consts[in.d].data & 0xffffffff)});
          b.CreateBr(mergeBB);

          b.SetInsertPoint(mergeBB);
          break;
        }
        case Op::PropWr: {
          auto recv = load_slot(in.b);
          auto tag = j.extract_tag(recv);
          require_object_recv(tag, "pwr");
          auto objData = j.extract_data(recv);
          auto objPtr = b.CreateIntToPtr(objData, ptrTy);
          // Shared-view reject ahead of the existence check (the interp's
          // order), at the statement.
          auto kind = j.emit_call(
              j.module_->getOrInsertFunction(rt::nc_receiver_kind,
                                             b.getInt8Ty(), i64Ty),
              {objData}, "pwr.kind");
          auto svBB = BasicBlock::Create(j.ctx_, "pwr.sverr", fn);
          auto hasBB = BasicBlock::Create(j.ctx_, "pwr.has", fn);
          b.CreateCondBr(b.CreateICmpEQ(kind, b.getInt8(2)), svBB, hasBB);
          b.SetInsertPoint(svBB);
          j.emit_throw_error("ImmutableError", "Shared values are immutable",
                             j.current_line_, j.current_column_);
          b.CreateUnreachable();
          b.SetInsertPoint(hasBB);
          auto* nm = reinterpret_cast<const char*>(c.consts[in.c].data);
          auto keyPtr = j.get_or_create_global_str(nm, ".vm.propname");
          auto has = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_has, b.getInt1Ty(),
                                             ptrTy, ptrTy),
              {objPtr, keyPtr}, "pwr.has");
          auto readBB = BasicBlock::Create(j.ctx_, "pwr.read", fn);
          auto missBB = BasicBlock::Create(j.ctx_, "pwr.miss", fn);
          b.CreateCondBr(has, readBB, missBB);
          b.SetInsertPoint(missBB);
          {
            // The miss anchors at the DOT node, packed into consts[d].
            int64_t pk = c.consts[in.d].data;
            j.emit_call(
                j.module_->getOrInsertFunction(rt::compound_missing_property,
                                               b.getVoidTy(), i64Ty, i64Ty),
                {b.getInt64(pk >> 32), b.getInt64(pk & 0xffffffff)});
            b.CreateUnreachable();
          }
          b.SetInsertPoint(readBB);
          auto view = j.emit_property_get(recv, nm);
          j.emit_value_retain(view);
          b.CreateStore(view, slots[in.a]);
          break;
        }
        case Op::PropCo: {
          auto recv = load_slot(in.b);
          auto tag = j.extract_tag(recv);
          require_object_recv(tag, "pco");
          auto objData = j.extract_data(recv);
          // The nc receiver-kind rejects ahead of the read (the JIT's nc
          // DOT dispatch: Shared view / packed field), at the statement.
          auto kind = j.emit_call(
              j.module_->getOrInsertFunction(rt::nc_receiver_kind,
                                             b.getInt8Ty(), i64Ty),
              {objData}, "pco.kind");
          auto svBB = BasicBlock::Create(j.ctx_, "pco.sverr", fn);
          auto pkBB = BasicBlock::Create(j.ctx_, "pco.packederr", fn);
          auto readBB = BasicBlock::Create(j.ctx_, "pco.read", fn);
          auto sw = b.CreateSwitch(kind, readBB, 2);
          sw->addCase(b.getInt8(2), svBB);
          sw->addCase(b.getInt8(4), pkBB);
          b.SetInsertPoint(svBB);
          j.emit_throw_error("ImmutableError", "Shared values are immutable",
                             j.current_line_, j.current_column_);
          b.CreateUnreachable();
          b.SetInsertPoint(pkBB);
          j.emit_throw_error("TypeError",
                             "`?" "?=` is not supported on a packed field",
                             j.current_line_, j.current_column_);
          b.CreateUnreachable();
          b.SetInsertPoint(readBB);
          {
            // Pin the read at the DOT node (consts[d]): a namespace's
            // unknown member raises its AttributeError there, the interp's
            // reject_namespace_write anchor. A dict miss stays nil.
            auto* nm = reinterpret_cast<const char*>(c.consts[in.c].data);
            int64_t pk = c.consts[in.d].data;
            auto sl = j.current_line_;
            auto sc = j.current_column_;
            j.current_line_ = static_cast<size_t>(pk >> 32);
            j.current_column_ = static_cast<size_t>(pk & 0xffffffff);
            auto view = j.emit_property_get(recv, nm);
            j.current_line_ = sl;
            j.current_column_ = sc;
            j.emit_value_retain(view);
            b.CreateStore(view, slots[in.a]);
          }
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
          // The body's metadata — the parameter names a keyword call binds
          // against, the `mut` bindings `Isolate.spawn` rejects it on, what
          // introspection reports — is a global of THIS module, not a
          // pointer into the compiler's heap: an AOT object outlives the
          // process that wrote it, so a baked address would be dangling by
          // the time the built program ran. Cached per chunk: the globals
          // are constant, so every site naming a chunk gets the same one.
          if (!metas[in.b])
            metas[in.b] = param_meta_global(j, p.chunks[in.b], fns[in.b]);
          auto cls = j.emit_call(
              j.module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                             i64Ty, i64Ty, i64Ty, ptrTy),
              {fns[in.b], b.getInt64(static_cast<int64_t>(n)),
               b.getInt64(f.arity),
               b.getInt64(static_cast<int64_t>(chunk_closure_flags(f))),
               metas[in.b]},
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
          auto tgt = chunk_call_target_at(c, i);
          // A borrowed callee reads the cell here and takes no `+1`: the
          // cell's own reference outlives the call, so the retain the copy
          // would have paid — and the release that matched it — are both
          // gone (Chunk::CallTarget::callee_in_cell).
          b.CreateStore(emit_invoke(tgt.callee_in_cell
                                        ? load_cell_value(load_slot(in.b))
                                        : load_slot(in.b),
                                    meth ? in.c : -1, in.c + (meth ? 1 : 0),
                                    in.d, line, col, chunk_argpos_at(c, i),
                                    tgt),
                        slots[in.a]);
          break;
        }
        case Op::CallKw: {
          const Chunk::KwCall& kc = c.kwcalls[in.d];
          auto [line, col] = chunk_pos_at(c, i);
          // Positional arguments report at themselves, keyword ones at the
          // call — see the executor's own arm.
          publish_site(line, col, chunk_argpos_at(c, i));
          int32_t off = kc.has_receiver ? 1 : 0;
          int32_t total = off + kc.n_pos + kc.n_kw + kc.n_splat;
          auto callee = load_slot(in.b);
          // The two cold probes, in the plain call's order: a callable
          // instance becomes both callee and receiver, a class object hands
          // over its `new`. Everything merges through entry-block scratch,
          // the shape the rest of the lowering uses for a per-arm result.
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          auto* calleeSlot = eb.CreateAlloca(j.valueType_, nullptr, "kw.callee");
          auto* selfSlot = eb.CreateAlloca(j.valueType_, nullptr, "kw.self");
          b.CreateStore(callee, calleeSlot);
          b.CreateStore(kc.has_receiver ? load_slot(in.c)
                                        : j.make_value(b.getInt8(TAG_NO_SELF),
                                                       b.getInt64(0)),
                        selfSlot);
          auto probeBB = BasicBlock::Create(j.ctx_, "kw.probe", fn);
          auto readyBB = BasicBlock::Create(j.ctx_, "kw.ready", fn);
          b.CreateCondBr(
              b.CreateICmpEQ(j.extract_tag(callee), b.getInt8(TAG_FUNC)),
              readyBB, probeBB);
          b.SetInsertPoint(probeBB);
          {
            auto ovBB = BasicBlock::Create(j.ctx_, "kw.overload", fn);
            auto ctorBB = BasicBlock::Create(j.ctx_, "kw.ctor", fn);
            auto errBB = BasicBlock::Create(j.ctx_, "kw.err", fn);
            auto callM = j.emit_value_call(
                j.module_->getOrInsertFunction(rt::class_call_method,
                                               j.valueType_, b.getInt8Ty(),
                                               i64Ty),
                {j.extract_tag(callee), j.extract_data(callee)}, "kw.method");
            b.CreateCondBr(
                b.CreateICmpEQ(j.extract_tag(callM), b.getInt8(TAG_FUNC)),
                ovBB, ctorBB);
            b.SetInsertPoint(ovBB);
            j.emit_value_retain(callee);
            if (kc.has_receiver) {
              j.emit_value_release(load_slot(in.c));
              b.CreateStore(j.make_nil(), slots[in.c]);
            }
            b.CreateStore(callee, selfSlot);
            b.CreateStore(callM, calleeSlot);
            b.CreateBr(readyBB);
            b.SetInsertPoint(ctorBB);
            auto ctorV = j.emit_value_call(
                j.module_->getOrInsertFunction(rt::class_new_method,
                                               j.valueType_, b.getInt8Ty(),
                                               i64Ty),
                {j.extract_tag(callee), j.extract_data(callee)}, "kw.ctor.m");
            b.CreateStore(ctorV, calleeSlot);
            auto ctorOkBB = BasicBlock::Create(j.ctx_, "kw.ctor.ok", fn);
            b.CreateCondBr(
                b.CreateICmpEQ(j.extract_tag(ctorV), b.getInt8(TAG_FUNC)),
                ctorOkBB, errBB);
            // The class object becomes the constructor's receiver, as the
            // callable instance does on the arm above.
            b.SetInsertPoint(ctorOkBB);
            j.emit_value_retain(callee);
            if (kc.has_receiver) {
              j.emit_value_release(load_slot(in.c));
              b.CreateStore(j.make_nil(), slots[in.c]);
            }
            b.CreateStore(callee, selfSlot);
            b.CreateBr(readyBB);
            b.SetInsertPoint(errBB);
            j.emit_call(j.module_->getOrInsertFunction(
                            rt::type_error_typed, b.getVoidTy(), i64Ty, i64Ty,
                            ptrTy, b.getInt8Ty()),
                        {b.getInt64(line), b.getInt64(col),
                         b.CreateGlobalString("Function"),
                         j.extract_tag(callee)});
            if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          }
          b.SetInsertPoint(readyBB);
          // The values, in one slab: positionals, then keyword values, then
          // the `**` operands. The resolver consumes every one of them, so
          // the slots are nil'd before the call — it is their only owner
          // from there on, throw paths included.
          llvm::Value* slab = ConstantPointerNull::get(cast<PointerType>(ptrTy));
          int32_t n_vals = total - off;
          if (n_vals > 0) {
            auto* arrTy = ArrayType::get(j.valueType_, n_vals);
            slab = eb.CreateAlloca(arrTy, nullptr, "kw.args");
            for (int32_t k = 0; k < n_vals; ++k)
              b.CreateStore(load_slot(in.c + off + k),
                            b.CreateConstGEP2_64(arrTy, slab, 0,
                                                 static_cast<uint64_t>(k)));
          }
          std::vector<Constant*> keyPtrs;
          for (const char* k : kc.kw_keys)
            keyPtrs.push_back(j.get_or_create_global_str(
                std::string(_str_sv(k)), ".vm.kwname"));
          llvm::Value* keys =
              kc.n_kw > 0
                  ? j.build_str_ptr_array(keyPtrs, ".vm.kwnames")
                  : ConstantPointerNull::get(cast<PointerType>(ptrTy));
          for (int32_t k = 0; k < total; ++k)
            b.CreateStore(j.make_nil(), slots[in.c + k]);
          auto self = b.CreateLoad(j.valueType_, selfSlot);
          auto vslab = slab;
          auto gep = [&](int32_t at) -> llvm::Value* {
            if (n_vals == 0) return vslab;
            return b.CreateConstGEP2_64(ArrayType::get(j.valueType_, n_vals),
                                        vslab, 0, static_cast<uint64_t>(at));
          };
          b.CreateStore(
              j.emit_value_call(
                  j.module_->getOrInsertFunction(
                      rt::call_with_kwargs, j.valueType_, ptrTy,
                      b.getInt8Ty(), i64Ty, i64Ty, ptrTy, i64Ty, ptrTy, ptrTy,
                      i64Ty, ptrTy, i64Ty, i64Ty),
                  {b.CreateIntToPtr(j.extract_data(
                                        b.CreateLoad(j.valueType_, calleeSlot)),
                                    ptrTy),
                   j.extract_tag(self), j.extract_data(self),
                   b.getInt64(kc.n_pos), gep(0), b.getInt64(kc.n_kw), keys,
                   gep(kc.n_pos), b.getInt64(kc.n_splat),
                   gep(kc.n_pos + kc.n_kw), b.getInt64(line),
                   b.getInt64(col)},
                  "kw.res"),
              slots[in.a]);
          break;
        }
        case Op::RaiseErr: {
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_throw_error(
              reinterpret_cast<const char*>(c.consts[in.b].data),
              std::string(_str_sv(
                  reinterpret_cast<const char*>(c.consts[in.c].data))),
              line, col);
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          // Instructions follow in the same run — the throw is unconditional,
          // so they are dead, but they still need somewhere well-formed to
          // land (a throw inside a region ends its block with an invoke).
          b.SetInsertPoint(BasicBlock::Create(j.ctx_, "raise.after", fn));
          break;
        }
        case Op::Ret: {
          if (c.counts_frame) {
            b.CreateCall(j.module_->getOrInsertFunction(rt::recursion_leave,
                                                        b.getVoidTy()));
          }
          b.CreateStore(load_slot(in.a), retPtr);
          b.CreateRetVoid();
          break;
        }
        case Op::CellNew: {
          // Release the previous generation's cell (null on the first pass),
          // then wrap the value, at the declaration point.
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
          auto v = load_cell_value(load_slot(in.b));
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
          auto v = in.c ? load_cell_value(load_slot(in.a)) : load_slot(in.a);
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
          auto nameG = j.get_or_create_global_str(f.multifn_name, ".vm.mfname");
          // The callee chunk's signature as .rodata (emit_multifn_register's
          // arrays, via the shared builder): declared types where it has
          // them, null where the param is untyped.
          // Only the REGULAR parameters take part in dispatch — see the
          // executor's own arm.
          auto regular_end = static_cast<size_t>(
              f.first_kw_only_idx >= 0 ? f.first_kw_only_idx
              : f.kwargs_rest_idx >= 0 ? f.kwargs_rest_idx
                                       : f.arity);
          auto nparams = static_cast<int64_t>(regular_end);
          int64_t min_arity = 0;
          std::vector<llvm::Constant*> nameCs;
          std::vector<llvm::Constant*> typeCs;
          for (size_t k = 0; k < regular_end; ++k) {
            nameCs.push_back(
                j.get_or_create_global_str(f.param_names[k], ".vm.mfpn"));
            typeCs.push_back(
                f.param_types[k].empty()
                    ? llvm::ConstantPointerNull::get(ptrTy)
                    : llvm::cast<llvm::Constant>(j.get_or_create_global_str(
                          f.param_types[k], ".vm.mfpt")));
            if (k >= f.param_has_default.size() || !f.param_has_default[k])
              min_arity++;
          }
          auto namesPtr = j.build_str_ptr_array(nameCs, ".vm.mfnames");
          auto typesPtr = j.build_str_ptr_array(typeCs, ".vm.mftypes");
          auto bodyPtr =
              b.CreateIntToPtr(j.extract_data(load_slot(in.b)), ptrTy);
          llvm::Value* intoPtr = llvm::ConstantPointerNull::get(ptrTy);
          if (in.c >= 0)
            intoPtr = b.CreateIntToPtr(j.extract_data(load_slot(in.c)), ptrTy);
          auto disp = j.emit_call(
              j.module_->getOrInsertFunction(
                  rt::multifn_register_and_install, ptrTy, ptrTy, ptrTy,
                  ptrTy, ptrTy, i64Ty, i64Ty, i64Ty, ptrTy),
              {nameG, intoPtr, bodyPtr, typesPtr, b.getInt64(nparams),
               b.getInt64(f.variadic ? 1 : 0), b.getInt64(min_arity),
               namesPtr},
              "vm.mf.disp");
          b.CreateStore(j.make_nil(), slots[in.b]);
          b.CreateStore(j.make_func(disp), slots[in.a]);
          break;
        }
        case Op::MfSelf: {
          // 16-byte by-value return — emit_value_call for the Win64 sret ABI.
          auto v = j.emit_value_call(
              j.module_->getOrInsertFunction(rt::multifn_self, j.valueType_,
                                             ptrTy),
              {clsArg}, "mf.self");
          b.CreateStore(v, slots[in.a]);
          break;
        }
        case Op::ClsSelf: {
          auto selfV = load_slot(in.b);
          b.CreateStore(
              j.emit_value_call(
                  j.module_->getOrInsertFunction(rt::class_self, j.valueType_,
                                                 b.getInt8Ty(), i64Ty),
                  {j.extract_tag(selfV), j.extract_data(selfV)}, "cls.self"),
              slots[in.a]);
          break;
        }
        case Op::WkErr: {
          j.emit_call(
              j.module_->getOrInsertFunction(rt::wk_contract_error,
                                             b.getVoidTy(), ptrTy),
              {vm_str_const(in.a, ".vm.wkname")});
          if (!b.GetInsertBlock()->getTerminator()) b.CreateUnreachable();
          break;
        }
        case Op::ClassMeta: {
          // The method run is spilled into one entry-block slab — the
          // JIT's meta.methods alloca — and the names ride as .rodata.
          std::vector<llvm::Constant*> nameCs;
          for (const auto& nm : c.name_tables[in.d])
            nameCs.push_back(j.get_or_create_global_str(nm, ".vm.mname"));
          auto namesPtr = in.c ? j.build_str_ptr_array(nameCs, ".vm.mnames")
                               : llvm::ConstantPointerNull::get(ptrTy);
          llvm::Value* slab = llvm::ConstantPointerNull::get(ptrTy);
          if (in.c) {
            IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
            slab = eb.CreateAlloca(j.valueType_, b.getInt64(in.c),
                                   "vm.meta.methods");
            for (int32_t i = 0; i < in.c; ++i) {
              b.CreateStore(load_slot(in.b + i),
                            b.CreateInBoundsGEP(j.valueType_, slab,
                                                {b.getInt64(i)}));
              b.CreateStore(j.make_nil(), slots[in.b + i]);  // +1 handed over
            }
          }
          auto meta = j.emit_call(
              j.module_->getOrInsertFunction(rt::build_class_meta, ptrTy,
                                             ptrTy, ptrTy, i64Ty, i64Ty),
              {namesPtr, slab, b.getInt64(in.c),
               b.getInt64(c.name_table_flags[in.d])},
              "vm.class.meta");
          b.CreateStore(j.make_object(meta), slots[in.a]);
          break;
        }
        case Op::DeriveFn: {
          auto cl = j.emit_call(
              j.module_->getOrInsertFunction(rt::make_derived_method, ptrTy,
                                             i64Ty),
              {b.getInt64(in.b)}, "vm.derive.closure");
          b.CreateStore(j.make_func(cl), slots[in.a]);
          break;
        }
        case Op::RegPack: {
          j.emit_call(
              j.module_->getOrInsertFunction(
                  in.d ? rt::register_packable_enum : rt::register_packable,
                  b.getVoidTy(), ptrTy, ptrTy),
              {vm_str_const(in.a, ".vm.pkg.name"),
               vm_str_const(in.b, ".vm.pkg.spec")});
          break;
        }
        case Op::ClassObj: {
          auto o = j.emit_call(
              j.module_->getOrInsertFunction(rt::object_new, ptrTy), {},
              "vm.class.ns");
          j.emit_call(j.module_->getOrInsertFunction(rt::mark_class,
                                                     b.getVoidTy(), ptrTy),
                      {o});
          b.CreateStore(j.make_object(o), slots[in.a]);
          break;
        }
        case Op::BindStatic: {
          auto v = load_slot(in.c);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::object_bind_static,
                                             b.getVoidTy(), ptrTy, ptrTy,
                                             b.getInt8Ty(), i64Ty),
              {b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy),
               vm_str_const(in.b, ".vm.key"),
               j.extract_tag(v), j.extract_data(v)});
          b.CreateStore(j.make_nil(), slots[in.c]);  // the slot took the +1
          break;
        }
        case Op::MakeInst: {
          // The frame's own ABI arguments, forwarded untouched — the helper
          // consumes each +1 on every exit (the `new` body's ABI, its
          // field-init guard, or its own release loop).
          auto meta = load_slot(in.b);
          auto finit = load_slot(in.b + 1);
          auto body = load_slot(in.b + 2);
          auto clsV = load_slot(in.d);  // the receiver: the class object
          auto inst = j.emit_value_call(
              j.module_->getOrInsertFunction(
                  rt::build_class_instance, j.valueType_, ptrTy, ptrTy,
                  b.getInt8Ty(), i64Ty, b.getInt8Ty(), i64Ty, b.getInt8Ty(),
                  i64Ty, i64Ty, ptrTy),
              {// The instance stores this as its `class` TAG_STRING, so it
               // must be header-backed (the length sits at data[-8]) — a
               // bare C global would make every later read walk garbage.
               j.emit_str_literal(_str_sv(reinterpret_cast<const char*>(
                   c.consts[in.c].data))),
               b.CreateIntToPtr(j.extract_data(meta), ptrTy),
               j.extract_tag(clsV), j.extract_data(clsV),
               j.extract_tag(finit), j.extract_data(finit),
               j.extract_tag(body), j.extract_data(body), nArgs, argsPtr},
              "vm.instance");
          b.CreateStore(inst, slots[in.a]);
          break;
        }
        case Op::ValueBox: {
          const auto& spec = c.value_box_specs[in.c];
          const int64_t n_fields = static_cast<int64_t>(spec.keys.size()) - 1;
          auto [cacheGlobal, keysArray] = j.build_shape_cache_globals(
              spec.keys, ".value.box", j.value_box_counter_);
          // The field values are scattered across N separate slot allocas
          // (each Chunk slot is its own alloca, not a contiguous array), so
          // pack them into one entry-block alloca first — MakeInst's own
          // args array is a frame's ABI array already contiguous; this one
          // has to be built, the same way a call site's own argument slab
          // is (jit.h's own entry-block-alloca call-args pattern).
          llvm::Value* fieldsPtr;
          if (n_fields == 0) {
            fieldsPtr = llvm::ConstantPointerNull::get(ptrTy);
          } else {
            IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
            fieldsPtr = eb.CreateAlloca(j.valueType_, b.getInt64(n_fields),
                                        ".value.box.fields");
            for (int64_t i = 0; i < n_fields; i++) {
              auto v = load_slot(in.b + static_cast<int32_t>(i));
              auto gep = b.CreateInBoundsGEP(j.valueType_, fieldsPtr,
                                             {b.getInt64(i)});
              b.CreateStore(v, gep);
            }
          }
          auto metaV = load_slot(in.d);
          auto inst = j.emit_value_call(
              j.module_->getOrInsertFunction(
                  rt::materialize_value, j.valueType_, ptrTy, ptrTy, i64Ty,
                  ptrTy, ptrTy, ptrTy),
              {cacheGlobal, keysArray,
               b.getInt64(static_cast<int64_t>(spec.keys.size())),
               b.CreateIntToPtr(j.extract_data(metaV), ptrTy),
               // Same header-backed literal MakeInst builds for the
               // instance's own "class" TAG_STRING value.
               j.emit_str_literal(_str_sv(spec.class_name)), fieldsPtr},
              "vm.value.box");
          b.CreateStore(inst, slots[in.a]);
          break;
        }
        case Op::FieldInit: {
          auto self = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::run_field_init, b.getVoidTy(),
                                             ptrTy, b.getInt8Ty(), i64Ty),
              {b.CreateIntToPtr(j.extract_data(load_slot(in.a)), ptrTy),
               j.extract_tag(self), j.extract_data(self)});
          break;
        }
        case Op::SelfMerge: {
          auto abi = load_slot(in.b);
          b.CreateStore(j.make_nil(), slots[in.b]);  // the +1 transfers
          auto merged = j.emit_value_call(
              j.module_->getOrInsertFunction(rt::self_merge, j.valueType_,
                                             b.getInt8Ty(), i64Ty, ptrTy),
              {j.extract_tag(abi), j.extract_data(abi),
               b.CreateIntToPtr(j.extract_data(load_slot(in.c)), ptrTy)},
              "vm.self");
          b.CreateStore(merged, slots[in.a]);
          break;
        }
        case Op::TraitReset:
          j.emit_call(
              j.module_->getOrInsertFunction(rt::trait_defaults_reset,
                                             b.getVoidTy(), ptrTy),
              {vm_str_const(in.a, ".vm.tname")});
          break;
        case Op::TraitDefault: {
          auto fn = load_slot(in.c);
          b.CreateStore(j.make_nil(), slots[in.c]);  // the registry takes it
          j.emit_call(
              j.module_->getOrInsertFunction(rt::register_trait_default,
                                             b.getVoidTy(), ptrTy, ptrTy,
                                             ptrTy),
              {vm_str_const(in.a, ".vm.tname"),
               vm_str_const(in.b, ".vm.tmethod"),
               b.CreateIntToPtr(j.extract_data(fn), ptrTy)});
          break;
        }
        case Op::TraitReg:
          j.emit_call(
              j.module_->getOrInsertFunction(rt::register_trait,
                                             b.getVoidTy(), ptrTy, ptrTy,
                                             ptrTy),
              {vm_str_const(in.a, ".vm.tname"),
               vm_str_const(in.b, ".vm.tspec"),
               vm_str_const(in.c, ".vm.tsuper")});
          break;
        case Op::PosSnap: {
          int64_t def = c.consts[in.b].data;
          auto packed = j.emit_call(
              j.module_->getOrInsertFunction(rt::param_pos, i64Ty, i64Ty,
                                             i64Ty, i64Ty),
              {b.getInt64(in.c), b.getInt64(def >> 32),
               b.getInt64(def & 0xffffffff)},
              "vm.errpos");
          b.CreateStore(j.make_value(b.getInt8(TAG_LONG), packed),
                        slots[in.a]);
          break;
        }
        case Op::ChkTypeAt: {
          auto v = load_slot(in.a);
          auto [line, col] = chunk_pos_at(c, i);
          llvm::Value* lineV = b.getInt64(line);
          llvm::Value* colV = b.getInt64(col);
          if (in.d >= 0) {
            auto packed = j.extract_data(load_slot(in.d));
            lineV = b.CreateLShr(packed, b.getInt64(32));
            colV = b.CreateAnd(packed, b.getInt64(0xffffffff));
          }
          j.emit_call(
              j.module_->getOrInsertFunction(rt::type_check, b.getVoidTy(),
                                             b.getInt8Ty(), i64Ty, ptrTy,
                                             ptrTy, i64Ty, i64Ty),
              {j.extract_tag(v), j.extract_data(v),
               vm_str_const(in.b, ".vm.chktype"),
               vm_str_const(in.c, ".vm.chkctx"), lineV, colV});
          break;
        }
        case Op::ChkArg: {
          auto [line, col] = chunk_pos_at(c, i);
          auto v = load_slot(in.a);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::type_check_param,
                                             b.getVoidTy(), b.getInt8Ty(),
                                             i64Ty, ptrTy, ptrTy, i64Ty,
                                             i64Ty, i64Ty),
              {j.extract_tag(v), j.extract_data(v),
               vm_str_const(in.b, ".vm.chktype"),
               vm_str_const(in.c, ".vm.chkctx"), b.getInt64(in.d),
               b.getInt64(line), b.getInt64(col)});
          break;
        }
        case Op::JumpIfFilled: {
          auto unf = b.CreateICmpEQ(j.extract_tag(load_slot(in.a)),
                                    b.getInt8(TAG_UNFILLED), "arg.unf");
          b.CreateCondBr(unf, blocks.at(static_cast<int32_t>(i) + 1),
                         blocks.at(in.b));
          break;
        }
        case Op::ArgsRest: {
          // The caller's `+1` on each overflow argument moves into the
          // Array; the prologue left them alone for this chunk.
          b.CreateStore(
              j.make_array(j.emit_call(
                  j.module_->getOrInsertFunction(rt::args_slice_to_array,
                                                 ptrTy, ptrTy, i64Ty, i64Ty),
                  {argsPtr, b.CreateLoad(i64Ty, argsFromSlot), nArgs},
                  "args.arr")),
              slots[in.a]);
          break;
        }
        case Op::KwRest: {
          // The marked Object arrives already owned by this slot; anything
          // else means no keyword content reached the call.
          auto v = load_slot(in.a);
          auto markedBB = BasicBlock::Create(j.ctx_, "kwrest.take", fn);
          auto emptyBB = BasicBlock::Create(j.ctx_, "kwrest.empty", fn);
          auto joinBB = BasicBlock::Create(j.ctx_, "kwrest.done", fn);
          b.CreateCondBr(b.CreateICmpEQ(j.extract_tag(v),
                                        b.getInt8(TAG_KWREST)),
                         markedBB, emptyBB);
          b.SetInsertPoint(markedBB);
          b.CreateStore(j.make_value(b.getInt8(TAG_OBJECT), j.extract_data(v)),
                        slots[in.a]);
          b.CreateBr(joinBB);
          b.SetInsertPoint(emptyBB);
          b.CreateStore(
              j.make_object(j.emit_call(
                  j.module_->getOrInsertFunction(rt::object_new, ptrTy), {},
                  "kwrest.new")),
              slots[in.a]);
          b.CreateBr(joinBB);
          b.SetInsertPoint(joinBB);
          break;
        }
        case Op::RecEnter: {
          auto d = j.emit_call(
              j.module_->getOrInsertFunction(rt::recursion_enter, i64Ty), {},
              "rec.depth");
          if (in.a && depthSlot) b.CreateStore(d, depthSlot);
          break;
        }
        case Op::RecLeave:
          b.CreateCall(j.module_->getOrInsertFunction(rt::recursion_leave,
                                                      b.getVoidTy()));
          break;
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
        case Op::BoundPos:
          // Same shape: the UFCS boundary publish over this row's position.
          j.emit_set_call_boundary();
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
          // d=1: the spec was assembled into a register by the pieces above.
          llvm::Value* specPtr =
              in.d ? b.CreateIntToPtr(j.extract_data(load_slot(in.c)), ptrTy)
                   : vm_str_const(in.c, ".fmtspec");
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
          auto [line, col] = chunk_pos_at(c, i);
          j.emit_call(j.module_->getOrInsertFunction(
                          rt::throw_, b.getVoidTy(), b.getInt8Ty(), i64Ty,
                          i64Ty, i64Ty),
                      {j.extract_tag(v), j.extract_data(v),
                       b.getInt64(line), b.getInt64(col)});
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
        case Op::ForOpen: {
          auto [line, col] = chunk_pos_at(c, i);
          auto& cur = for_cursor(in.a);
          // A fresh walk: the latch belongs to this execution, not to the
          // previous one that left the slot behind.
          b.CreateStore(j.make_long(b.getInt64(0)),
                        slots[in.a + kForDisposed]);
          auto head = j.emit_for_open_dispatch(
              cur, load_slot(in.a + kForIterable), slots[in.a + kForSetArr],
              slots[in.a + kForSrc], static_cast<size_t>(line),
              static_cast<size_t>(col));
          b.SetInsertPoint(head);
          break;
        }
        case Op::ForNext: {
          auto& cur = for_cursor(in.a);
          int32_t line = in.c, col = in.d;  // baked at the emit site
          auto advArrayBB = BasicBlock::Create(j.ctx_, "vm.for.adv.arr", fn);
          auto advProtoBB = BasicBlock::Create(j.ctx_, "vm.for.adv.proto", fn);
          auto advStringBB = BasicBlock::Create(j.ctx_, "vm.for.adv.str", fn);
          auto kindV = b.CreateLoad(b.getInt8Ty(), cur.kind, "vm.for.kind");
          auto sw = b.CreateSwitch(kindV, advArrayBB, 3);
          sw->addCase(b.getInt8(FOR_ARRAY), advArrayBB);
          sw->addCase(b.getInt8(FOR_PROTO), advProtoBB);
          sw->addCase(b.getInt8(FOR_STRING), advStringBB);
          auto* bodyBB = blocks.at(static_cast<int32_t>(i) + 1);
          auto* exitBB = blocks.at(in.b);
          j.emit_for_advance_array(cur, advArrayBB, bodyBB, exitBB);
          j.emit_for_advance_protocol(cur, advProtoBB, bodyBB, exitBB,
                                      static_cast<size_t>(line),
                                      static_cast<size_t>(col));
          j.emit_for_advance_string(cur, advStringBB, bodyBB, exitBB);
          break;
        }
        case Op::ForDispose:
          emit_for_dispose(in.a, in.d != 0);
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
          j.emit_set_op_pos();  // the too-deep walk's backfill anchor
          j.emit_call(j.module_->getFunction(rt::println),
                      {j.extract_tag(v), j.extract_data(v)});
          break;
        }
        case Op::ToFloat:
          b.CreateStore(j.emit_to_float_step(load_slot(in.b)), slots[in.a]);
          break;
        case Op::NsCall: {
          auto [line, col] = chunk_pos_at(c, i);
          std::vector<llvm::Value*> argv;
          for (int32_t k = 0; k < in.d; ++k) argv.push_back(load_slot(in.b + k));
          // The helper arm's argument run: an entry-block scratch like a
          // call's slab, which the inline arms never touch.
          IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
          llvm::Value* slab = ConstantPointerNull::get(cast<PointerType>(ptrTy));
          if (in.d > 0)
            slab = eb.CreateAlloca(ArrayType::get(j.valueType_, in.d), nullptr,
                                   "ns.args");
          b.CreateStore(j.emit_ns_call(static_cast<NsFn>(in.c), argv, slab,
                                       line, col),
                        slots[in.a]);
          break;
        }
        case Op::Safepoint:
          j.emit_safepoint();
          break;
        case Op::DropSuppress:
          b.CreateCall(
              j.module_->getOrInsertFunction(rt::set_drop_suppressed,
                                             b.getVoidTy(), b.getInt8Ty()),
              {b.getInt8(static_cast<int8_t>(in.a))});
          break;
        case Op::BArity: {
          auto r = load_slot(in.a);
          auto tag = j.extract_tag(r);
          for (const auto& arm : c.arity_checks[in.b]) {
            auto badBB = BasicBlock::Create(j.ctx_, "barity.bad", fn);
            auto okBB = BasicBlock::Create(j.ctx_, "barity.ok", fn);
            llvm::Value* hit = nullptr;
            if (arm.tag >= 0) {
              hit = b.CreateICmpEQ(tag, b.getInt8(arm.tag));
            } else {
              // object_has probes the pointer, so both questions are only
              // safe once the tag is known — the JIT's own arm order.
              auto shapeBB = BasicBlock::Create(j.ctx_, "barity.shape", fn);
              b.CreateCondBr(b.CreateICmpEQ(tag, b.getInt8(TAG_OBJECT)),
                             shapeBB, okBB);
              b.SetInsertPoint(shapeBB);
              auto objPtr = b.CreateIntToPtr(j.extract_data(r), ptrTy);
              auto name = std::string(_str_sv(reinterpret_cast<const char*>(
                  c.consts[arm.name_k].data)));
              auto own = j.emit_object_has(
                  objPtr, j.get_or_create_global_str(name, ".vm.barity.name"));
              // A view never takes the table's diagnostic —
              // Exec::object_takes_builtin_table, in IR.
              auto plain = b.CreateICmpEQ(
                  j.emit_call(j.module_->getOrInsertFunction(
                                  rt::nc_receiver_kind, b.getInt8Ty(), i64Ty),
                              {j.extract_data(r)}, "barity.kind"),
                  b.getInt8(0));
              hit = b.CreateAnd(plain, b.CreateNot(own));
              if (arm.tag == Chunk::kArityIter)
                hit = b.CreateAnd(
                    hit, j.emit_object_has(objPtr,
                                           j.get_or_create_global_str(
                                               "next", ".vm.it.next")));
            }
            b.CreateCondBr(hit, badBB, okBB);
            b.SetInsertPoint(badBB);
            j.emit_throw_error(
                reinterpret_cast<const char*>(c.consts[arm.kind_k].data),
                std::string(_str_sv(reinterpret_cast<const char*>(
                    c.consts[arm.msg_k].data))),
                arm.line, arm.col);
            if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(okBB);
            b.SetInsertPoint(okBB);
          }
          break;
        }
        case Op::LazyNsReg: {
          auto v = load_slot(in.b);
          j.emit_call(
              j.module_->getOrInsertFunction(rt::lazy_ns_register,
                                             b.getVoidTy(), ptrTy,
                                             b.getInt8Ty(), i64Ty),
              {j.emit_str_literal(std::string(_str_sv(
                   reinterpret_cast<const char*>(c.consts[in.c].data)))),
               j.extract_tag(v), j.extract_data(v)});
          break;
        }
        case Op::FnHandle: {
          auto selfV = load_slot(in.b);
          b.CreateStore(
              j.emit_value_call(
                  j.module_->getOrInsertFunction(
                      rt::fn_handle, j.valueType_, b.getInt8Ty(), i64Ty,
                      ptrTy, ptrTy),
                  {j.extract_tag(selfV), j.extract_data(selfV),
                   b.CreateIntToPtr(j.extract_data(load_slot(in.c)), ptrTy),
                   slots[in.d]},
                  "fn.handle"),
              slots[in.a]);
          break;
        }
        case Op::OwnedMark:
          // next_id sits at offset 0 of the hot fields, which the JIT's own
          // scope entry loads the same way.
          b.CreateStore(b.CreateLoad(i64Ty, b.CreateIntToPtr(
                                                j.owned_hot_ptr(), ptrTy)),
                        owned_mark_ptr(in.a));
          break;
        case Op::OwnedExit:
          j.emit_owned_scope_exit(load_owned_mark(in.a));
          break;
        case Op::ReplCell:
        case Op::ReplBind:
          // REPL-only, and the REPL runs on the executor: it is tier 0, and
          // a line is never a hot loop (include/repl.h — the same reason
          // `--jit` does not get its own REPL). No lane emits these into a
          // chunk this pass can be handed, so there is nothing here to
          // write that could be tested.
          throw std::runtime_error("vm: REPL op in a lowered chunk");
        case Op::DbgStmt:
          // Only a forced boundary reaches this pass: a debug session runs
          // on the executor (it reads register windows and frame stacks
          // that native code does not keep), so `--debug` — where the
          // `debugger` statement is the only boundary emitted — is the one
          // mode a lowered chunk carries. Same minimal break as the JIT's.
          if (!in.a) throw std::runtime_error("vm: step boundary in a lowered chunk");
          j.emit_call(j.module_->getOrInsertFunction(
                          rt::debugger_break, b.getVoidTy(), ptrTy, i64Ty,
                          i64Ty),
                      {b.CreateGlobalString(p.source_path, ".dbgpath"),
                       b.getInt64(static_cast<int64_t>(j.current_line_)),
                       b.getInt64(static_cast<int64_t>(j.current_column_))});
          break;
        case Op::Halt:
          b.CreateRetVoid();
          break;
      }
    }
    j.current_lpad_ = nullptr;
    auto release_slot_ir = [&](int32_t s, bool as_cell) {
      if (s < 0 || s >= c.num_slots) return;
      if (as_cell)
        j.emit_cell_release(
            b.CreateIntToPtr(j.extract_data(load_slot(s)), ptrTy));
      else
        j.emit_value_release(load_slot(s));
      b.CreateStore(j.make_nil(), slots[s]);
    };
    auto restoreFn = j.module_->getOrInsertFunction(rt::recursion_restore,
                                                    b.getVoidTy(), i64Ty);

    // The temp preludes first: their re-raise edges are what keep the scope
    // pads they continue into alive (a pad nothing unwinds to is erased).
    //
    // A scope's entries share one descent chain rather than each carrying its
    // own copy of the releases — the frame ladder's shape. The abandoned sets
    // of one scope are stacks with a common floor, so a set that is another's
    // prefix costs no rung of its own, and the whole scope continues the
    // unwind through a single re-raise edge at the foot.
    std::map<int32_t, std::vector<const TempPad*>> temps_by_scope;
    for (const auto& tp : temp_pads) temps_by_scope[tp.scope].push_back(&tp);
    for (const auto& [k, group] : temps_by_scope) {
      JIT::CleanupPad pad(j, pads[static_cast<size_t>(k)]);
      BasicBlock* foot = nullptr;
      std::map<std::vector<int32_t>, BasicBlock*> rungs;
      // The block that releases `t.back()` and descends the rest, building
      // whatever rungs of the chain do not exist yet.
      auto rung_for = [&](const std::vector<int32_t>& t) {
        if (!foot) foot = BasicBlock::Create(j.ctx_, "vm.temps.done", fn);
        BasicBlock* next = foot;
        std::vector<int32_t> pre;  // grown in place; copied only per new rung
        pre.reserve(t.size());
        for (size_t n = 1; n <= t.size(); ++n) {
          pre.push_back(t[n - 1]);
          if (auto it = rungs.find(pre); it != rungs.end()) {
            next = it->second;
            continue;
          }
          auto* bb = BasicBlock::Create(
              j.ctx_, std::format("vm.temps.rel{}", pre.back()), fn);
          auto* saved = b.GetInsertBlock();
          b.SetInsertPoint(bb);
          release_slot_ir(pre.back(), /*as_cell=*/false);
          b.CreateBr(next);
          b.SetInsertPoint(saved);
          rungs.emplace(pre, bb);
          next = bb;
        }
        return next;
      };
      for (const auto* tp : group) {
        if (!pad.open(tp->bb, "vm.temps.exc")) continue;
        auto* entry = b.GetInsertBlock();
        auto* target = rung_for(tp->temps);
        b.SetInsertPoint(entry);
        b.CreateBr(target);
      }
      // The re-raise the pad's destructor emits belongs at the foot, where
      // every entry has finished descending.
      if (foot) b.SetInsertPoint(foot);
    }

    // Then the scope pads, innermost outward, so each one's re-raise edge
    // exists before its encloser decides whether anything reaches it. Order
    // within a pad is Exec::unwind's: restore this frame's depth (the
    // unwound callees never ran their `leave`, and a defer body's own calls
    // must count from here), run the scope's pending defers, release its
    // bindings in reverse declaration order, and — at the frame — uncount
    // before the throw travels on.
    for (size_t k = 0; k < c.cleanups.size(); ++k) {
      const auto& cu = c.cleanups[k];
      bool frame = cu.parent < 0;
      JIT::CleanupPad pad(
          j, frame ? nullptr : pads[static_cast<size_t>(cu.parent)]);
      if (!pad.open(pads[k], "vm.scope.exc")) continue;
      auto d = b.CreateLoad(i64Ty, depthSlot, "rec.d");
      b.CreateCall(restoreFn, {d});
      // Drain the JIT's own unwind-temp pool too. A shared emitter called from
      // an arm above can park a `+1` in an Owned handle rather than a register
      // — emit_for_open_protocol holds the iterator that way while it
      // validates the protocol — and that pool lives in the LLVM function, so
      // the bytecode temp table (which the preludes above stand on) cannot see
      // it. Without this a rejected iterator stranded its object
      // (`for x in {iter: fn () { {a: 1} }}`). Releasing nil-clears each slot,
      // so an enclosing pad draining again is a no-op.
      j.release_unwind_temps();
      if (cu.defer_mark_slot >= 0) {
        auto markV = b.CreateLoad(j.valueType_, slots[cu.defer_mark_slot]);
        b.CreateCall(j.module_->getFunction(rt::defer_run_to),
                     {j.extract_data(markV)});
      }
      bool hush = frame && c.suppress_frame_drop;
      auto suppress = [&](int v) {
        b.CreateCall(j.module_->getOrInsertFunction(rt::set_drop_suppressed,
                                                    b.getVoidTy(),
                                                    b.getInt8Ty()),
                     {b.getInt8(static_cast<int8_t>(v))});
      };
      if (hush) suppress(1);
      for (int32_t s : chunk_release_order(c, cu.slot_lo, cu.slot_hi)) {
        // A for-in's own step closes its iterator here, swallowing a
        // throwing dispose — an exception is already in flight (docs §18.5).
        if (cu.dispose_base >= 0 && s == cu.dispose_base + kForIter)
          emit_for_dispose(cu.dispose_base, /*swallow=*/true);
        release_slot_ir(s, chunk_slot_is_cell(c, s, cu.cells_before));
      }
      if (hush) suppress(0);
      // The frame's step resolves the owned region once — see the
      // executor's own unwind for why only here.
      if (frame && !hush && c.owned_frame_depth >= 0)
        j.emit_owned_scope_exit(load_owned_mark(c.owned_frame_depth));
      if (frame && chunk_idx != 0)
        b.CreateCall(restoreFn, {b.CreateSub(d, b.getInt64(1), "rec.d1")});
      if (cu.handler == Chunk::kNoHandler) continue;  // pad dtor re-raises
      // A try scope classifies what it just cleaned up after: our own throw
      // enters the catch, a foreign one travels on to the encloser (interp's
      // catch(...) sees the same order — cleanup, then the decision).
      j.emit_open_exception();
      j.emit_classify_tail(frame ? nullptr
                                 : pads[static_cast<size_t>(cu.parent)],
                           depthSlot, slots[cu.caught_slot],
                           blocks.at(static_cast<int32_t>(cu.handler)));
    }
  }
};

// The compiled lane for a loader's module list: what `--jit` runs. One
// bytecode program, lowered to one LLVM module, keyed for the object cache
// by the module set and the compile options. `baked` is what the splice took
// out of the preamble for this lane (stdlib_preamble.h): entries the lowered
// program calls instead of compiling their source.
inline void run_modules_via_llvm(const std::vector<LoadedModule>& modules,
                                 std::span<const BakedPreamble* const> baked,
                                 bool emit_llvm = false, bool debug = false,
                                 int opt_level = 2,
                                 bool fast_codegen = false) {
  if (modules.empty()) return;
  // The baked modules' `@value` declarations, parsed for the compiler —
  // without them the unbox splice cannot fire on a stdlib class this lane
  // never compiles the source of (Vector2, ...).
  auto value_decls = parse_baked_value_decls(baked);
  auto prog = Compiler::compile_modules(
      modules, debug ? Debug::Break : Debug::Off, &value_decls);
  // Nothing host-side to prepare: a body's metadata is a global of the
  // lowered module, handed to each closure at its MakeClosure site (see
  // Lowering's param_meta_global).
  Lowering::run_program(prog, emit_llvm, opt_level, fast_codegen,
                        JIT::jit_module_name(modules, fast_codegen, opt_level),
                        baked);
}

// For a caller that spliced the whole preamble — the doctest runner, whose
// one module list feeds both lanes, and an embedder — the same run with the
// baked modules taken back out here (stdlib_preamble.h). The CLI's --jit
// path passes them in instead, having never parsed their source.
inline void run_modules_via_llvm(const std::vector<LoadedModule>& modules,
                                 bool emit_llvm = false, bool debug = false,
                                 int opt_level = 2,
                                 bool fast_codegen = false) {
  if (modules.empty()) return;
  auto res = resolve_baked_preamble(modules, /*force_source=*/false);
  run_modules_via_llvm(res.modules, res.baked, emit_llvm, debug, opt_level,
                       fast_codegen);
}

// `culebra build`: the same program the JIT lane runs, emitted as an object
// file. Nothing host-side is registered here — the metadata a keyword call
// resolves against is a global of the module (Lowering::param_meta_globals),
// and the stdlib namespaces it can reach are a list in the module too — so
// the built binary carries its own. The namespace scan runs over every
// module, the spliced preamble included, so a lazy module's `_Canvas` counts
// once `Canvas` pulled it in; over-linking costs bytes, a miss is a namespace
// that throws at first use, which is why it is textual and not a resolution.
inline int build_object_from_modules(
    const std::vector<LoadedModule>& modules, const std::string& out_path,
    int opt_level = 2, bool emit_llvm = false,
    const std::string& target_triple = "", bool preamble_from_source = false) {
  if (modules.empty()) {
    std::fprintf(stderr, "culebra build: empty module list\n");
    return 1;
  }
  // Baked stdlib modules become references into the archive rather than
  // code in this object (stdlib_preamble.h); a cross build keeps the
  // source, since its archive is the user's.
  auto res = resolve_baked_preamble(modules, preamble_from_source);
  // As the run lane above: the baked modules' `@value` declarations still
  // reach the compiler, so both lanes emit the same spliced bytecode.
  auto value_decls = parse_baked_value_decls(res.baked);
  auto prog = Compiler::compile_modules(res.modules, Debug::Off, &value_decls);
  // Over `modules`, NOT `res.modules`: a baked module's source is what names
  // the namespaces it needs (the Canvas preamble's `_Canvas`, ...), and the
  // resolver just took that source out of the list. Scanning the resolved
  // list would link no group for them and the baked code would raise
  // "namespace is not linked" at its first use.
  AotNames names;
  for (const auto& m : modules)
    if (m.ast) aot_collect_names(*m.ast, names);
  return Lowering::build_object(prog, out_path, opt_level, emit_llvm,
                                target_triple, names, res.baked);
}

}  // namespace culebra::vm

namespace culebra {

// The embedding entries declared in jit.h. They are the same two lanes the
// driver takes, under the names docs/deployment.md publishes.
inline void JIT::run(const std::shared_ptr<peg::Ast>& ast, bool emit_llvm,
                     bool debug, int opt_level) {
  run_modules({{.ast = ast}}, emit_llvm, debug, opt_level);
}

inline void JIT::run_modules(const std::vector<LoadedModule>& modules,
                             bool emit_llvm, bool debug, int opt_level,
                             bool fast_codegen) {
  vm::run_modules_via_llvm(modules, emit_llvm, debug, opt_level, fast_codegen);
}

inline int JIT::build_object(const std::vector<LoadedModule>& modules,
                             const std::string& out_path, int opt_level,
                             bool emit_llvm,
                             const std::string& target_triple) {
  return vm::build_object_from_modules(modules, out_path, opt_level, emit_llvm,
                                       target_triple);
}

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
