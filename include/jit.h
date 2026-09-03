#pragma once

#ifdef CULEBRA_JIT_ENABLED

// The value model, the extern "C" helpers and the front-end contract that
// this compiler and the VM executor both build on. Conservative backstop
// collector included from there too.
#include <rt.h>

#include <module_loader.h>  // LoadedModule — the embedding entries' argument
#include <stdlib_preamble.h>  // baked_preambles — symbols every JITDylib defines

#include <filesystem>  // the object cache's directory

#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/xxhash.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Target/TargetMachine.h"

#ifdef _WIN32
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"  // orc::absoluteSymbols
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"  // RTDyld layer
#include "llvm/ExecutionEngine/SectionMemoryManager.h"  // JIT memory manager
#include "llvm/Support/Memory.h"  // sys::Memory / MemoryBlock (near-image mapper)
#include <cxxabi.h>  // __cxa_begin_catch / __cxa_end_catch / __cxa_rethrow
#include <unwind.h>  // _Unwind_Resume_or_Rethrow (the rethrow relay's exit)
// The mingw SEH personality has no public header; declare it to take its address
// for the absolute-symbol map (see define_windows_eh_symbols) and for the
// unwind-table fixup in WinSEHMemoryManager below.
extern "C" int __gxx_personality_seh0(...);
// libgcc's stack-probe helper. LLVM's X86 backend emits a call to `___chkstk_ms`
// in the prologue of any JIT'd function whose frame exceeds one page (4 KB) — a
// user method body reaches that once its args/ret-slot allocas add up — to touch
// the guard pages before moving %rsp. Like the EH entry points it lives in
// libgcc (excluded from export), so declare it to take its in-process address for
// the absolute-symbol map. Its real ABI is nonstandard (size in %rax, preserves
// all regs); this signature exists only to name the symbol for &-taking.
extern "C" void ___chkstk_ms();
#endif

namespace culebra {

// The bytecode VM's LLVM lowering (vm_lowering.h) reuses this compiler as its
// codegen context, so it is a JIT friend like JitExtension below. The compiler
// and executor it shares a bytecode with need nothing from here — they build
// on rt.h alone.
namespace vm {
struct Lowering;
}

#ifdef _WIN32
// RTDyld memory manager that registers Win64 unwind tables for JIT'd code, so a
// throw can unwind *through* a JIT frame in-process (reaching its defers and an
// outer catch). Without this the seh0 personality is emitted correctly but the
// OS SEH unwinder never learns the JIT'd frames exist: LLVM's RTDyld collects
// each object's `.pdata` (an array of RUNTIME_FUNCTION) and calls
// registerEHFrames(), but its in-process body is a Win64 no-op (llvm#24607). We
// override it to call RtlAddFunctionTable against the object's image base — the
// lowest section address, which all the `.pdata`/`.xdata` RVAs are relative to.
// This is the mingw/Itanium half of the old clang-interpreter Win64 example;
// culebra throws via __cxa_throw + __gxx_personality_seh0 (not MSVC
// _CxxThrowException), so none of that example's throw-record image-base fixup
// is needed.
//
// Reachability: the `.xdata` handler field and RtlAddFunctionTable's base are
// both 32-bit image-relative, so the whole thing only works when the JIT slab
// lands within 4 GB of the runtime's __gxx_personality_seh0. By default
// VirtualAlloc puts the slab in the low address space, gigabytes below the
// ASLR'd main image — the personality RVA can't reach it and the OS unwinder
// jumps into the slab instead. NearImageMapper (below) forces every page just
// under the main image so the RVA fits.
//
// Maps LLVM's page requests (SectionMemoryManager's default source) into a
// 2 GB window immediately below the main module, via VirtualAlloc2 with an
// address requirement. That keeps personality - slab_base positive and well
// under 4 GB, so RTDyld writes a correct ADDR32NB RVA and the OS computes the
// handler as slab_base + RVA == the real personality. Resolved dynamically so a
// mingw import lib without VirtualAlloc2, or a failed reservation, falls back to
// the default allocator (JIT still runs; that object just won't unwind).
class NearImageMapper : public llvm::SectionMemoryManager::MemoryMapper {
  using Purpose = llvm::SectionMemoryManager::AllocationPurpose;
  using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                         MEM_EXTENDED_PARAMETER*, ULONG);

 public:
  llvm::sys::MemoryBlock allocateMappedMemory(
      Purpose /*P*/, size_t NumBytes,
      const llvm::sys::MemoryBlock* const NearBlock, unsigned Flags,
      std::error_code& EC) override {
    static VirtualAlloc2Fn va2 = reinterpret_cast<VirtualAlloc2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    if (va2) {
      if (!low_) compute_window();
      const size_t gran = 0x10000;  // Windows allocation granularity
      size_t sz = (NumBytes + gran - 1) & ~(gran - 1);
      MEM_ADDRESS_REQUIREMENTS req{};
      req.LowestStartingAddress = reinterpret_cast<PVOID>(low_);
      req.HighestEndingAddress = reinterpret_cast<PVOID>(high_);
      req.Alignment = 0;
      MEM_EXTENDED_PARAMETER ep{};
      ep.Type = MemExtendedParameterAddressRequirements;
      ep.Pointer = &req;
      void* p = va2(nullptr, nullptr, sz, MEM_RESERVE | MEM_COMMIT,
                    to_win_prot(Flags), &ep, 1);
      if (p) {
        EC = std::error_code();
        return llvm::sys::MemoryBlock(p, sz);
      }
    }
    // No VirtualAlloc2, or the window is full: fall back to the OS default.
    return llvm::sys::Memory::allocateMappedMemory(NumBytes, NearBlock, Flags, EC);
  }
  std::error_code protectMappedMemory(const llvm::sys::MemoryBlock& Block,
                                      unsigned Flags) override {
    return llvm::sys::Memory::protectMappedMemory(Block, Flags);
  }
  std::error_code releaseMappedMemory(llvm::sys::MemoryBlock& M) override {
    return llvm::sys::Memory::releaseMappedMemory(M);
  }

 private:
  void compute_window() {
    auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    // Reserve strictly below the image base so every personality RVA is
    // positive; a 2 GB window keeps it comfortably under the 4 GB ADDR32NB cap.
    high_ = base - 1;  // module base is 64K-aligned, so this ends in 0xFFFF
    uintptr_t span = 0x80000000ull;
    low_ = base > span ? base - span : 0x10000;
  }
  static DWORD to_win_prot(unsigned f) {
    using M = llvm::sys::Memory;
    bool w = f & M::MF_WRITE, x = f & M::MF_EXEC, r = f & M::MF_READ;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
  }
  uintptr_t low_ = 0, high_ = 0;
};

inline NearImageMapper& near_image_mapper() {
  static NearImageMapper m;
  return m;
}

class WinSEHMemoryManager : public llvm::SectionMemoryManager {
 public:
  WinSEHMemoryManager() : llvm::SectionMemoryManager(&near_image_mapper()) {}

  uint8_t* allocateCodeSection(uintptr_t Size, unsigned Align, unsigned ID,
                               llvm::StringRef Name) override {
    uint8_t* p = SectionMemoryManager::allocateCodeSection(Size, Align, ID, Name);
    note_base(p);
    return p;
  }
  uint8_t* allocateDataSection(uintptr_t Size, unsigned Align, unsigned ID,
                               llvm::StringRef Name, bool RO) override {
    uint8_t* p =
        SectionMemoryManager::allocateDataSection(Size, Align, ID, Name, RO);
    note_base(p);
    return p;
  }
  // Addr points at the loaded `.pdata` section = a RUNTIME_FUNCTION array. We do
  // not chain to the base implementation: its in-process path feeds the block to
  // the DWARF __register_frame machinery, which would misread these COFF unwind
  // records. We keep our own list for teardown instead.
  void registerEHFrames(uint8_t* Addr, uint64_t /*LoadAddr*/,
                        size_t Size) override {
    auto* fns = reinterpret_cast<PRUNTIME_FUNCTION>(Addr);
    DWORD n = static_cast<DWORD>(Size / sizeof(RUNTIME_FUNCTION));
    if (n && image_base_ && ::RtlAddFunctionTable(fns, n, image_base_)) {
      registered_.push_back(fns);
      fixup_personality(fns, n);
    }
  }
  void deregisterEHFrames() override {
    for (auto* fns : registered_) ::RtlDeleteFunctionTable(fns);
    registered_.clear();
  }

 private:
  // RTDyld loads .xdata (with setProcessAllSections) but does not relocate the
  // exception-handler RVA inside each UNWIND_INFO — it keeps the object-file
  // placeholder, so the OS unwinder dispatches into the middle of .xdata and
  // crashes. Point every EH/UH handler at the real __gxx_personality_seh0. The
  // near-image mapper guarantees personality - image_base fits in the 32-bit
  // RVA. See the Win64 UNWIND_INFO layout: byte0 = Version:3|Flags:5, byte2 =
  // CountOfCodes, then that many 2-byte codes (padded to an even count),
  // immediately followed by the handler RVA when UNW_FLAG_[EU]HANDLER is set.
  void fixup_personality(PRUNTIME_FUNCTION fns, DWORD n) {
    const uintptr_t base = image_base_;
    const uint32_t rva = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&__gxx_personality_seh0) - base);
    for (DWORD i = 0; i < n; ++i) {
      uint32_t unwind = reinterpret_cast<const uint32_t*>(&fns[i])[2];
      auto* ui = reinterpret_cast<uint8_t*>(base + unwind);
      if (!((ui[0] >> 3) & 0x3)) continue;  // no EH/UH handler on this frame
      uint8_t codes = ui[2];
      auto* handler =
          reinterpret_cast<uint32_t*>(ui + 4 + ((codes + 1) & ~1) * 2);
      DWORD old;
      if (::VirtualProtect(handler, sizeof(uint32_t), PAGE_READWRITE, &old)) {
        *handler = rva;
        DWORD tmp;
        ::VirtualProtect(handler, sizeof(uint32_t), old, &tmp);
      }
    }
  }
  void note_base(uint8_t* p) {
    if (!p) return;
    auto a = reinterpret_cast<uintptr_t>(p);
    if (!image_base_ || a < image_base_) image_base_ = a;
  }
  uintptr_t image_base_ = 0;
  std::vector<PRUNTIME_FUNCTION> registered_;
};
#endif  // _WIN32

// --- JIT compiler implementation ---

struct JIT {
  // The front-end analysis (FuncInfo, the locals/free-var/EH-defer passes)
  // lives in fn_analysis.h, shared with the bytecode compiler
  // (docs/internals/vm.md §4); `analysis_` below holds this compilation's
  // instance and its accumulated results.

 private:
  struct Owned;  // defined below (the +1 ownership handle)

 public:
  // When inside a `try { ... }` region, points at the landingpad BB
  // that catches `CulebraException` for that region. Any call emitted
  // while this is non-null is emitted as `invoke` with this as the
  // unwind destination, so a user `throw` propagates back to the
  // nearest enclosing `try`. Nested try blocks save/restore.
  llvm::BasicBlock* current_lpad_ = nullptr;

  // Unified call-site emitter: `invoke` if inside a try, else `call`.
  // All JIT-generated call sites (runtime functions, user functions,
  // closures) go through this so user `throw` propagates uniformly.
  // A nounwind callee can never unwind, so it needs no landing-pad edge
  // even inside a try — emit it as a plain `call`.
  llvm::CallBase* emit_call(llvm::FunctionCallee callee,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    bool may_throw = !f || !f->doesNotThrow();
    if (current_lpad_ && may_throw) {
      // Automatic unwind-temp window: spill the live Owned +1s around
      // this one invoke so the cleanup-pad chain releases them on a throw.
      auto armed = open_unwind_window();
      auto fn = builder_.GetInsertBlock()->getParent();
      auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
      auto inv =
          builder_.CreateInvoke(callee, contBB, current_lpad_, args, name);
      builder_.SetInsertPoint(contBB);
      close_unwind_window(armed);
      return inv;
    }
    return builder_.CreateCall(callee, args, name);
  }

  // Call a runtime helper that returns a 16-byte JitValue BY VALUE, recovering
  // the result. On Win64 the C ABI (GCC) returns a 16-byte aggregate through a
  // hidden pointer passed as the first integer argument (RCX), but LLVM's
  // raw-IR `{i64,i64}` return is two-register — the two disagree, so every
  // argument shifts one slot and the callee reads garbage (tags come out
  // "Unknown", positions become junk). Match GCC by passing an explicit result
  // slot as a leading pointer argument and calling a void-returning form: the
  // slot lands in RCX exactly where GCC's implicit sret pointer goes, so the
  // remaining args line up. On SysV a 16-byte struct is returned in two
  // registers by both LLVM and GCC, so it's a plain value-returning call.
  llvm::Value* emit_value_call(llvm::FunctionCallee callee,
                               llvm::ArrayRef<llvm::Value*> args,
                               const llvm::Twine& name = "") {
#ifdef _WIN32
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto* origTy = callee.getFunctionType();
    std::vector<llvm::Type*> params;
    params.reserve(origTy->getNumParams() + 1);
    params.push_back(ptrTy);  // result slot (matches GCC's hidden sret ptr)
    params.insert(params.end(), origTy->param_begin(), origTy->param_end());
    auto* voidTy = llvm::FunctionType::get(builder_.getVoidTy(), params,
                                           origTy->isVarArg());
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::Value* slot;
    {
      llvm::IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      slot = eb.CreateAlloca(valueType_, nullptr, "sret.slot");
    }
    std::vector<llvm::Value*> newArgs;
    newArgs.reserve(args.size() + 1);
    newArgs.push_back(slot);
    newArgs.insert(newArgs.end(), args.begin(), args.end());
    emit_call(llvm::FunctionCallee(voidTy, callee.getCallee()), newArgs);
    return builder_.CreateLoad(valueType_, slot, name);
#else
    return emit_call(callee, args, name);
#endif
  }

  // Loop safepoint: inline a relaxed load of the process "wake" flag and a cold
  // branch to the throwing slow path. One byte load + one not-taken branch on the
  // hot path; the truth check (Ctrl+C vs per-isolate cancel) and the throw live
  // in the rarely-taken `culebra_runtime_safepoint`. The wake flag is set by both
  // a real Ctrl+C and a per-isolate cancel, so even a JIT isolate's tight loop —
  // whose only interrupt is its `core->interrupt`, invisible to inlined code — is
  // interruptible, mirroring the interpreter's per-iteration poll. The enclosing
  // function gets a personality via scan_eh_defer flagging loops as has_eh, so the
  // throw unwinds cleanly even with no try in scope.
  void emit_safepoint() {
    auto i8Ty = builder_.getInt8Ty();
    auto fn = builder_.GetInsertBlock()->getParent();
    auto gv = module_->getOrInsertGlobal("culebra_g_wake", i8Ty);
    auto ld = builder_.CreateLoad(i8Ty, gv, "wake");
    ld->setAtomic(llvm::AtomicOrdering::Monotonic);
    ld->setAlignment(llvm::Align(1));
    auto hit = builder_.CreateICmpNE(
        ld, llvm::ConstantInt::get(i8Ty, 0), "sigint.hit");
    auto slowBB = llvm::BasicBlock::Create(ctx_, "safepoint.slow", fn);
    auto contBB = llvm::BasicBlock::Create(ctx_, "safepoint.cont", fn);
    llvm::MDBuilder mdb(ctx_);
    builder_.CreateCondBr(hit, slowBB, contBB,
                          mdb.createBranchWeights(1, 1u << 20));
    builder_.SetInsertPoint(slowBB);
    // Throws Interrupted when still set; returns (rejoining the loop) if a
    // racing consumer already cleared the flag.
    emit_call(module_->getFunction(rt::safepoint), {});
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(contBB);
    }
    builder_.SetInsertPoint(contBB);
  }

  // Indirect-call overload (function pointer + explicit FunctionType).
  // Delegates to the FunctionCallee path: an indirect callee has no
  // Function* to query, so dyn_cast there fails and it always takes the
  // unwinding path inside a try — the behaviour this overload had directly.
  llvm::CallBase* emit_call(llvm::FunctionType* fty, llvm::Value* fnPtr,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    return emit_call(llvm::FunctionCallee(fty, fnPtr), args, name);
  }

  // Re-raise from a pad that *handled* the exception — it called
  // `__cxa_begin_catch`, so the ABI's handler count is open and has to be
  // closed again before the exception travels on. The only such re-raise is
  // emit_lpad_classify's foreign arm; a cleanup pad never opens the exception
  // at all (see CleanupPad) and continues the unwind directly.
  //
  // If `outerLpad` is non-null the exception reaches that pad within the same
  // LLVM function; otherwise it propagates out to the caller.
  void emit_handler_rethrow(llvm::BasicBlock* outerLpad) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto rethrowFn = module_->getOrInsertFunction(
        "__cxa_rethrow", builder_.getVoidTy());
    auto fn = builder_.GetInsertBlock()->getParent();

    // `__cxa_rethrow` negates the exception's handler count and restarts the
    // unwind; the ABI closes the rethrowing handler on the rethrow's own
    // unwind edge, which is where this pad's `__cxa_begin_catch`
    // (emit_handler_prologue) is paired off. Same shape GCC emits for
    // `catch (...) { throw; }`. Without the pairing the count never returns to
    // zero, so libsupc++ never runs `_Unwind_DeleteException` and the
    // exception object is stranded. The `end_catch` belongs on this edge and
    // not at the receiving pad's head: that pad also collects ordinary invoke
    // edges, where ending a catch would discard a live exception.
    auto relayBB = llvm::BasicBlock::Create(ctx_, "rethrow.relay", fn);
    auto deadBB = llvm::BasicBlock::Create(ctx_, "rethrow.dead", fn);
    builder_.CreateInvoke(rethrowFn, deadBB, relayBB, {});
    builder_.SetInsertPoint(deadBB);
    builder_.CreateUnreachable();

    emit_landingpad(relayBB, "rethrow.exc", /*open=*/false);
    builder_.CreateCall(
        module_->getOrInsertFunction("__cxa_end_catch", builder_.getVoidTy()));
    auto excPtr = builder_.CreateLoad(ptrTy, exception_slot(), "exc");
    // The exception is closed but still alive (a rethrown handler's end_catch
    // pops it without deleting): carry it on to the enclosing pad in this
    // frame, or out of the function when there is none.
    auto resumeFn = module_->getOrInsertFunction(rt_unwind_resume,
                                                 builder_.getInt32Ty(), ptrTy);
    if (outerLpad) {
      auto goneBB = llvm::BasicBlock::Create(ctx_, "rethrow.gone", fn);
      builder_.CreateInvoke(resumeFn, goneBB, outerLpad, {excPtr});
      builder_.SetInsertPoint(goneBB);
    } else {
      builder_.CreateCall(resumeFn, {excPtr});
    }
    builder_.CreateUnreachable();
  }

  // Itanium-ABI unwinder entry the rethrow relay continues on (libgcc; the
  // same call `__cxa_rethrow` makes internally). Named once so the Windows
  // absolute-symbol table below and the emitter cannot drift apart.
  static constexpr const char* rt_unwind_resume = "_Unwind_Resume_or_Rethrow";

  // Exception-safe incremental construction of a heap value (a container
  // literal appends elements; each element expression can throw). Returns an
  // entry-block alloca holding the in-construction value: store the container
  // into it, create an empty `cleanupBB`, set it as `current_lpad_` around the
  // element compilation, restore `current_lpad_`, then call
  // finish_construction_cleanup. The value is passed to the cleanup through
  // this alloca (loaded there) rather than as an SSA value live across the
  // invoke unwind edges — a call-result SSA live into a landingpad crashes
  // SDAG-O0's register coalescer.
  llvm::Value* make_build_guard(llvm::Value* containerVal) {
    auto a = make_pending_guard();
    builder_.CreateStore(containerVal, a);
    return a;
  }

  // A nil-initialised guard slot for an in-flight `+1` temporary that is
  // computed but not yet consumed while a later sub-call can throw (a heap
  // object key before its value, a spread source before its consuming call).
  // Store the temp before the risky call and clear it (store nil) after the
  // call consumes/releases it; `finish_construction_cleanup` releases whatever
  // it holds on the throw path. Releasing nil is a no-op, so an unused guard
  // is harmless (and dead-code eliminates when no cleanup is emitted).
  llvm::Value* make_pending_guard() {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto a = entryB.CreateAlloca(valueType_, nullptr, "build.guard");
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_), a);
    return a;
  }

  void clear_pending_guard(llvm::Value* guard) {
    builder_.CreateStore(llvm::ConstantAggregateZero::get(valueType_), guard);
  }

  // Release whatever a guard slot still holds, then nil it so an outer pad on
  // the same unwind chain no-ops. Slot is nil outside its window, and
  // releasing nil is a no-op, so this is unconditional at any cleanup site.
  void release_pending_guard(llvm::Value* guard) {
    emit_value_release(builder_.CreateLoad(valueType_, guard, "pending.temp"));
    clear_pending_guard(guard);
  }

  // Catch-all landingpad at `padBB`, leaving the insertion point there.
  // `open` says whether this pad takes the exception over: a pad that *ends*
  // the throw (emit_lpad_classify's handled arm, the iterator dispose
  // swallow) opens it with `__cxa_begin_catch` and owes exactly one
  // `__cxa_end_catch`; a pad that only cleans up and hands the throw on does
  // not (CleanupPad), so the ABI's handler count reaches the real handler
  // untouched and the exception object is freed exactly once.
  llvm::LandingPadInst* emit_landingpad(llvm::BasicBlock* padBB,
                                        const char* name, bool open) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    builder_.SetInsertPoint(padBB);
    auto lpadTy = llvm::StructType::get(ptrTy, builder_.getInt32Ty());
    auto lpad = builder_.CreateLandingPad(lpadTy, 1, name);
    // Catch-all rather than `cleanup`: phase 1 stops at the innermost pad, so
    // `defer` and `drop` run even for a throw nothing catches — the guarantee
    // docs/language.md makes — instead of depending on a handler further out.
    lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));
    auto excPtr = builder_.CreateExtractValue(lpad, {0});
    if (open) {
      builder_.CreateCall(
          module_->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy),
          {excPtr});
    } else {
      builder_.CreateStore(excPtr, exception_slot());
    }
    return lpad;
  }

  // A pad that handles the exception: opens it, and owes an `__cxa_end_catch`
  // on every path out (emit_handler_rethrow provides it for the re-raising one).
  llvm::LandingPadInst* emit_handler_prologue(llvm::BasicBlock* padBB,
                                              const char* name) {
    return emit_landingpad(padBB, name, /*open=*/true);
  }


  // Take over the exception a cleanup pad stored (emit_landingpad's
  // `open=false` arm) so the classification below can end the throw. The VM
  // lowering's try scopes run their scope cleanup between the two, which is
  // why the pad head and the classification are separable here.
  void emit_open_exception() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    builder_.CreateCall(
        module_->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy),
        {builder_.CreateLoad(ptrTy, exception_slot(), "exc")});
  }

  // emit_lpad_classify's second half, from the current insertion point with
  // the exception already open.
  void emit_classify_tail(llvm::BasicBlock* foreignTarget,
                          llvm::Value* depthSlot, llvm::Value* caughtSlot,
                          llvm::BasicBlock* handlerBB) {
    auto fn = builder_.GetInsertBlock()->getParent();
    builder_.CreateCall(module_->getOrInsertFunction(
        "culebra_runtime_try_translate", builder_.getVoidTy()));
    auto flagVal = builder_.CreateCall(
        module_->getOrInsertFunction("culebra_runtime_get_is_throw",
                                     builder_.getInt8Ty()),
        {}, "is_throw");
    auto isOurs = builder_.CreateICmpNE(flagVal, builder_.getInt8(0));
    auto handleBB = llvm::BasicBlock::Create(ctx_, "lpad.handle", fn);
    auto foreignBB = llvm::BasicBlock::Create(ctx_, "lpad.foreign", fn);
    builder_.CreateCondBr(isOurs, handleBB, foreignBB);

    builder_.SetInsertPoint(foreignBB);
    emit_handler_rethrow(foreignTarget);

    builder_.SetInsertPoint(handleBB);
    builder_.CreateCall(module_->getOrInsertFunction(
        "culebra_runtime_clear_is_throw", builder_.getVoidTy()));
    builder_.CreateCall(
        module_->getOrInsertFunction("__cxa_end_catch", builder_.getVoidTy()));
    builder_.CreateCall(
        module_->getOrInsertFunction(rt::recursion_restore,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty()),
        {builder_.CreateLoad(builder_.getInt64Ty(), depthSlot, "rec.d")});
    auto tagVal = builder_.CreateCall(
        module_->getOrInsertFunction("culebra_runtime_get_thrown_tag",
                                     builder_.getInt8Ty()),
        {}, "exc.tag");
    auto dataVal = builder_.CreateCall(
        module_->getOrInsertFunction("culebra_runtime_get_thrown_data",
                                     builder_.getInt64Ty()),
        {}, "exc.data");
    builder_.CreateStore(make_value(tagVal, dataVal), caughtSlot);
    builder_.CreateBr(handlerBB);
  }

  // If any construction sub-call could throw, its invoke made `cleanupBB` a
  // predecessor: fill it with "release the partial values (loaded from each
  // `guard`), re-raise". Otherwise the block is unused and erased, leaving
  // zero happy-path overhead (e.g. `[this, o]`, whose elements are
  // non-throwing identifiers, emits no cleanup — and the guard stores dead-code
  // eliminate). Restores the caller's insertion point.
  void finish_construction_cleanup(llvm::BasicBlock* cleanupBB,
                                   llvm::ArrayRef<llvm::Value*> guards,
                                   llvm::BasicBlock* outerLpad) {
    CleanupPad pad(*this, outerLpad);
    if (!pad.open(cleanupBB, "build.lpad")) return;
    // In-flight expression temporaries die first, then the partial
    // container — the same order as finish_scope_cleanup / the frame ladder.
    // Draining here and not only at the frame's ladder keeps the guarantee
    // local: this pad hands the throw to `outerLpad`, which is null for a
    // construction that is not nested inside one, so nothing downstream would
    // drain the pool. The nil-clear makes any outer pad on the same unwind
    // chain a no-op, so draining at every link never double-frees.
    release_unwind_temps();
    for (auto* guard : guards)
      emit_value_release(
          builder_.CreateLoad(valueType_, guard, "build.partial"));
  }



  // C++ personality routine for this module's target. Windows (mingw) uses the
  // SEH personality `__gxx_personality_seh0` with the *same* Itanium-shaped
  // landingpad IR the rest of the JIT emits — only the personality name differs
  // (confirmed on the real mingw LLVM: landingpad+seh0 lowers to COFF EH tables
  // that interoperate with libstdc++/libgcc; MSVC funclets are NOT needed since
  // culebra links libstdc++, not the MSVC STL). Every other target uses v0.
  // Read from the module's target triple — set before any emission in every
  // codegen entry (JIT exec, AOT build_object) — so a cross-compiled Windows
  // object also gets seh0.
  llvm::Constant* get_personality_fn() {
    auto fty = llvm::FunctionType::get(builder_.getInt32Ty(), {}, true);
    const char* name = module_->getTargetTriple().isOSWindows()
                           ? "__gxx_personality_seh0"
                           : "__gxx_personality_v0";
    auto callee = module_->getOrInsertFunction(name, fty);
    return llvm::cast<llvm::Constant>(callee.getCallee());
  }

  // AArch64's early if-conversion speculates a small `if` arm into fcsel
  // whatever the branch's odds, and an arm that assigns a loop-carried
  // Float then puts fcmp+fcsel on the loop's critical path (the scalars
  // row of tools/bench/vector_loop.cul; the Vector2 row's arm is too big
  // to convert and never paid it). x86 does not run the pass by default,
  // and neither does the generic CPU that AOT compiles for — the call
  // from the AOT path keeps the two from diverging if that changes.
  static void tune_backend() {
    auto& opts = llvm::cl::getRegisteredOptions();
    if (auto it = opts.find("aarch64-enable-early-ifcvt"); it != opts.end())
      static_cast<llvm::cl::opt<bool>*>(it->second)->setValue(false);
  }

  // Process-wide LLVM target init. Concurrent callers race on the
  // built-in target registry, so guard with std::call_once. Each
  // JIT::run() goes through this before touching ORC.
  static void ensure_native_target_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      tune_backend();
    });
  }

  // Cross-compile target init. With CULEBRA_LLVM_ALL_TARGETS the driver
  // links every LLVM backend (~31 MB) and registers them all so that
  // `culebra build --target=<triple>` accepts any triple. Without the
  // option the driver only links Native + WebAssembly (default), so
  // only those two are registered here. Lazy + idempotent; only the
  // AOT path pays this cost.
  static void ensure_all_targets_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
#ifdef CULEBRA_LLVM_ALL_TARGETS
      llvm::InitializeAllTargetInfos();
      llvm::InitializeAllTargets();
      llvm::InitializeAllTargetMCs();
      llvm::InitializeAllAsmPrinters();
      llvm::InitializeAllAsmParsers();
#else
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();
      LLVMInitializeWebAssemblyTargetInfo();
      LLVMInitializeWebAssemblyTarget();
      LLVMInitializeWebAssemblyTargetMC();
      LLVMInitializeWebAssemblyAsmPrinter();
      LLVMInitializeWebAssemblyAsmParser();
#endif
      tune_backend();
    });
  }

  // On-disk cache for compiled native objects. It answers the "warmup
  // without trading steady-state" question: the backend codegen output
  // (.o) is stored under a content key (build salt + flags + source), so a
  // later run of the same program skips instruction-selection + register-
  // allocation — the dominant warmup cost — and loads byte-identical O2
  // code. Per-step throughput is unaffected (no FastISel involved). Only
  // modules named with kCacheKeyTag participate; a plainly-named module
  // bypasses the cache so it can never collide on a shared object slot.
  static constexpr const char* kCacheKeyTag = "culebra#";

  class FileObjectCache : public llvm::ObjectCache {
   public:
    explicit FileObjectCache(std::string dir) : dir_(std::move(dir)) {
      std::error_code ec;
      std::filesystem::create_directories(dir_, ec);
      if (const char* mb = std::getenv("CULEBRA_JIT_CACHE_MAX_MB"))
        max_bytes_ = std::strtoull(mb, nullptr, 10) * 1024 * 1024;
    }
    void notifyObjectCompiled(const llvm::Module* m,
                              llvm::MemoryBufferRef obj) override {
      auto p = path_for(m);
      if (p.empty()) return;
      std::ofstream out(p, std::ios::binary);
      out.write(obj.getBufferStart(),
                static_cast<std::streamsize>(obj.getBufferSize()));
      out.close();
      evict_if_over_cap();
    }
    std::unique_ptr<llvm::MemoryBuffer> getObject(
        const llvm::Module* m) override {
      auto p = path_for(m);
      if (p.empty()) return nullptr;
      auto buf = llvm::MemoryBuffer::getFile(p, /*IsText=*/false);
      if (!buf) return nullptr;
      return llvm::MemoryBuffer::getMemBufferCopy(
          (*buf)->getBuffer(), (*buf)->getBufferIdentifier());
    }

   private:
    // Only a content-keyed module (kCacheKeyTag prefix, stamped by
    // jit_module_name) is cacheable; anything else gets an empty path and
    // bypasses the cache. The key already encodes source + build salt, so
    // resolving it here costs nothing — no IR reprint.
    std::string path_for(const llvm::Module* m) const {
      llvm::StringRef id = m->getModuleIdentifier();
      if (!id.starts_with(kCacheKeyTag)) return {};
      return dir_ + "/" + id.str() + ".o";
    }
    // Keep the cache under a soft byte cap by evicting least-recently-used
    // objects. Runs only after a miss writes a new object, so the directory
    // walk is amortized against a full backend compile.
    void evict_if_over_cap() {
      namespace fs = std::filesystem;
      std::error_code ec;
      std::vector<std::pair<fs::file_time_type, fs::path>> entries;
      uintmax_t total = 0;
      for (auto& e : fs::directory_iterator(dir_, ec)) {
        if (!e.is_regular_file(ec)) continue;
        total += e.file_size(ec);
        entries.push_back({e.last_write_time(ec), e.path()});
      }
      if (total <= max_bytes_) return;
      std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (auto& [mtime, path] : entries) {
        if (total <= max_bytes_) break;
        auto sz = fs::file_size(path, ec);
        if (fs::remove(path, ec)) total -= sz;
      }
    }
    std::string dir_;
    uintmax_t max_bytes_ = 512ull * 1024 * 1024;  // 512 MB; CULEBRA_JIT_CACHE_MAX_MB
  };

  // Resolve the JIT object-cache directory, or nullopt when disabled.
  // CULEBRA_JIT_CACHE: unset / "off" / "0" / "" -> disabled;
  // "1" / "on" / "auto" -> the OS cache dir (~/Library/Caches/culebra/jit,
  // $XDG_CACHE_HOME/culebra/jit, ...); any other value -> that path.
  static std::optional<std::string> jit_cache_dir() {
    const char* env = std::getenv("CULEBRA_JIT_CACHE");
    if (!env) return std::nullopt;
    std::string_view v(env);
    if (v.empty() || v == "off" || v == "0") return std::nullopt;
    if (v == "1" || v == "on" || v == "auto") {
      llvm::SmallString<128> dir;
      if (!llvm::sys::path::cache_directory(dir)) return std::nullopt;
      llvm::sys::path::append(dir, "culebra", "jit");
      return std::string(dir.str());
    }
    return std::string(env);
  }

  // A salt uniquely identifying this binary's codegen ABI, computed once.
  // The executable's path + size + mtime invalidate the cache on any
  // rebuild — even an incremental one that left __DATE__/__TIME__ unchanged
  // — and the LLVM version + host triple guard a toolchain or target swap.
  // Cheap: a single stat, no binary read.
  static const std::string& jit_cache_salt() {
    static const std::string salt = [] {
      std::string s = LLVM_VERSION_STRING;
      s += '|';
      s += llvm::sys::getDefaultTargetTriple();
      std::string exe = llvm::sys::fs::getMainExecutable(
          nullptr, reinterpret_cast<void*>(&create_jit_instance));
      llvm::sys::fs::file_status st;
      if (!exe.empty() && !llvm::sys::fs::status(exe, st)) {
        s += '|' + exe + '|' + std::to_string(st.getSize()) + '|' +
             std::to_string(llvm::sys::toTimeT(st.getLastModificationTime()));
      }
      return s;
    }();
    return salt;
  }

  // Module name for a JIT run. With the cache on, encode a content key
  // (build salt + codegen flags + every module's source) under kCacheKeyTag
  // so a later run of the same program finds its prior backend object;
  // otherwise the plain name (which the cache ignores). Single source for
  // both the single- and multi-module entry points.
  static std::string jit_module_name(const std::vector<LoadedModule>& modules,
                                     bool fast_codegen, int opt_level) {
    if (!jit_cache_dir()) return "culebra";
    std::string acc = jit_cache_salt();
    acc += fast_codegen ? "|fast|O" : "|O";
    acc += std::to_string(opt_level);
    // A source-less module (an embedder's bare AST) can't be keyed — two
    // different programs would share one entry, so don't cache at all.
    for (const auto& m : modules) {
      if (!m.source) return "culebra";
      acc += '|' + *m.source;
    }
    return std::string(kCacheKeyTag) +
           std::to_string(llvm::xxh3_64bits(acc));
  }

  // Backend configuration for `--jit-faststart`, in one place because both
  // LLJIT construction paths below need it.
  //
  // `CodeGenOptLevel::None` selects FastISel plus RegAllocFast, and
  // RegAllocFast is the half of the warmup win that lives in the backend —
  // FastISel over the greedy allocator recovers almost none of it. Skipping
  // the IR pipeline buys about as much again. It is also unsound above a
  // threshold: RegAllocFast miscompiles a landing pad carrying more than ten
  // live phi values, running out of scratch registers and sourcing the
  // eleventh from x1, which the Itanium EH ABI has already overwritten with
  // the exception selector. The corrupted value then flows into the pad's
  // release calls and the process dies dereferencing it. See
  // tests/llvm_regalloc_landingpad_phi/ for a ~40-line reproducer with no
  // culebra involvement.
  //
  // We stay outside that threshold structurally rather than by luck: our
  // own codegen never puts a phi in a landing pad — every value crossing
  // an unwind edge goes through an alloca — so at IR-O0 the pads
  // carry zero phis. Only mem2reg/SROA, which run from IR-O1 up, promote
  // those allocas into pad phis (measured: 26-47 of them on the tests that
  // used to crash). Hence `--jit-faststart` pins the IR pipeline to O0
  // too; main.cc rejects an explicit conflicting -O rather than silently
  // reinstating the broken combination.
  static void apply_fast_codegen(llvm::orc::JITTargetMachineBuilder& jtmb) {
    jtmb.setCodeGenOptLevel(llvm::CodeGenOptLevel::None);
  }

  // Build a fresh, ready-to-use LLJIT instance with the host's process
  // symbols available. Used by one-shot `exec` (script mode).
  //
  // `fast_codegen` drops the *backend* to FastISel + fast regalloc; callers
  // pin the IR pipeline to O0 alongside it, never on its own — see
  // apply_fast_codegen above for the LLVM bug that rules out the mixed
  // configuration. The machine code is worse, so this trades steady-state
  // throughput for startup latency — a clear win when the hot compute lives
  // in the C++/BLAS runtime (Tensor) rather than in JIT-emitted arithmetic
  // (scalar).
  static std::unique_ptr<llvm::orc::LLJIT> create_jit_instance(
      bool fast_codegen = false) {
    using namespace llvm;
    orc::LLJITBuilder lb;
    // Route compilation through a custom compiler when an object cache is
    // requested, so backend output can be reused across runs. The opt
    // level is applied here too (instead of the separate JTMB path) to
    // keep a single place that configures codegen.
    auto cache_dir = jit_cache_dir();
    if (cache_dir) {
      static FileObjectCache cache{*cache_dir};
      lb.setCompileFunctionCreator(
          [fast_codegen](orc::JITTargetMachineBuilder jtmb)
              -> Expected<std::unique_ptr<orc::IRCompileLayer::IRCompiler>> {
            if (fast_codegen) apply_fast_codegen(jtmb);
            auto tm = jtmb.createTargetMachine();
            if (!tm) return tm.takeError();
            return std::make_unique<orc::TMOwningSimpleCompiler>(
                std::move(*tm), &cache);
          });
    } else if (fast_codegen) {
      auto jtmb = cantFail(orc::JITTargetMachineBuilder::detectHost());
      apply_fast_codegen(jtmb);
      lb.setJITTargetMachineBuilder(std::move(jtmb));
    }
#ifdef _WIN32
    // Windows LLJIT already defaults to RTDyld; swap in the same layer backed by
    // a memory manager that registers Win64 unwind tables (WinSEHMemoryManager),
    // so JIT'd exceptions can unwind in-process. Transparent for non-throwing
    // code — it only adds RtlAddFunctionTable calls for each object's .pdata.
    lb.setObjectLinkingLayerCreator(
        [](orc::ExecutionSession& es)
            -> Expected<std::unique_ptr<orc::ObjectLayer>> {
          auto layer = std::make_unique<orc::RTDyldObjectLinkingLayer>(
              es, [](const MemoryBuffer&)
                      -> std::unique_ptr<RuntimeDyld::MemoryManager> {
                return std::make_unique<WinSEHMemoryManager>();
              });
          // Force-load .pdata/.xdata. They are orphan sections — no code
          // relocation references them — so RuntimeDyld skips them by default,
          // and RuntimeDyldCOFFX86_64::registerEHFrames then has no .pdata to
          // hand our memory manager. Without this the unwind tables never reach
          // the OS and a JIT'd throw crashes instead of unwinding.
          layer->setProcessAllSections(true);
          return layer;
        });
#endif
    auto jit = cantFail(lb.create());
    auto& jd = jit->getMainJITDylib();
    auto gen = cantFail(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix()));
    jd.addGenerator(std::move(gen));
    define_baked_preambles(*jit, jd);
#ifdef _WIN32
    define_windows_eh_symbols(*jit, jd);
#endif
    return jit;
  }

  // The baked stdlib modules' entries (stdlib_preamble.h), which a lowered
  // program calls by name (Lowering::lower_program). Defined from the
  // driver's own table rather than found through the process resolver: that
  // one reads each platform's dynamic-symbol/export view, and whether a
  // `culebra_preamble_<Name>` shows up there differs by OS and linker flags
  // (the same seam that once lost every runtime helper to macOS's
  // dead_strip). The table is the address in every build.
  static void define_baked_preambles(llvm::orc::LLJIT& jit,
                                     llvm::orc::JITDylib& jd) {
    using namespace llvm;
    orc::SymbolMap syms;
    for (const auto& bp : baked_preambles())
      syms[jit.mangleAndIntern(baked_preamble_symbol(bp.name))] =
          orc::ExecutorSymbolDef(
              orc::ExecutorAddr::fromPtr(bp.init),
              JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    if (!syms.empty()) cantFail(jd.define(orc::absoluteSymbols(std::move(syms))));
  }

#ifdef _WIN32
  // The process-symbol resolver (GetForCurrentProcess) reads the PE export
  // table, which carries culebra's own runtime symbols but NOT the
  // libstdc++/libgcc C++ EH entry points the JIT'd landingpads reference: the
  // export list is the culebra_* names read off the driver's objects
  // (culebra_export_jit_symbols, CMakeLists.txt — exporting the static libs
  // too would overflow the 65535-export limit once LLVM is linked in). They
  // are still present in this image (culebra's own C++ uses exceptions), so
  // define them for the
  // JIT as absolute symbols pointing at their in-process addresses. The POSIX
  // path needs none of this — -rdynamic exports the static EH symbols too.
  static void define_windows_eh_symbols(llvm::orc::LLJIT& jit,
                                        llvm::orc::JITDylib& jd) {
    using namespace llvm;
    auto& es = jit.getExecutionSession();
    orc::SymbolMap syms;
    auto add = [&](const char* n, void* p) {
      syms[es.intern(n)] = orc::ExecutorSymbolDef(
          orc::ExecutorAddr::fromPtr(p),
          JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    };
    add("__gxx_personality_seh0",
        reinterpret_cast<void*>(&__gxx_personality_seh0));
    // Namespace-qualified: GCC predeclares these globally for its own EH
    // lowering, clang only knows <cxxabi.h>'s declarations.
    add("__cxa_begin_catch", reinterpret_cast<void*>(&abi::__cxa_begin_catch));
    add("__cxa_end_catch", reinterpret_cast<void*>(&abi::__cxa_end_catch));
    add("__cxa_rethrow", reinterpret_cast<void*>(&abi::__cxa_rethrow));
    // Where a pad continues the unwind (CleanupPad, emit_handler_rethrow):
    // libgcc is static here too, so the PE export table cannot supply it.
    add(rt_unwind_resume, reinterpret_cast<void*>(&_Unwind_Resume_or_Rethrow));
    // Stack-probe helper for large-frame prologues (see the extern declaration
    // above). Missing it makes any JIT'd function with a >4 KB frame — e.g. a
    // user class method — call an unresolved address and jump into garbage.
    add("___chkstk_ms", reinterpret_cast<void*>(&___chkstk_ms));
    cantFail(jd.define(orc::absoluteSymbols(std::move(syms))));
  }
#endif

  // Resolve `triple` and stamp the module's data layout and triple, so IR
  // struct field offsets, alignments and pointer sizes match the C++ runtime.
  // Without it a {i8, i64} Value lays out with i64 at offset 1 (no padding),
  // diverging from the 16-byte JitValue the runtime uses — any alloca-backed
  // arg-slab would break.
  static inline std::unique_ptr<llvm::TargetMachine> apply_target(
      llvm::Module& mod, const llvm::Triple& triple,
      std::string* err = nullptr, bool pic = false) {
    std::string lookup_err;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, lookup_err);
    if (!target) {
      if (err) {
        *err = "target lookup failed for '" + triple.str() + "': " + lookup_err;
      }
      return nullptr;
    }
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, "", "", {},
        pic ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_)
            : std::nullopt));
    if (!tm) {
      if (err) *err = "target machine creation failed";
      return nullptr;
    }
    mod.setDataLayout(tm->createDataLayout());
    mod.setTargetTriple(triple);
    return tm;
  }

  // Does this call reach a refcount helper with arguments that make it do
  // nothing? Each helper opens with the guard asked about here — the value
  // pair against `_is_refcounted_value_tag` and a null payload, the cell pair
  // against a null cell — so the answer is theirs rather than a second copy
  // of it.
  static bool is_settled_refcount(const llvm::CallInst& call) {
    const auto* callee = call.getCalledFunction();
    if (!callee) return false;
    const auto name = callee->getName();
    if (name == rt::value_retain || name == rt::value_release) {
      const auto* tag =
          llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(0));
      const auto* data =
          llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(1));
      return (tag && !_is_refcounted_value_tag(
                         static_cast<int8_t>(tag->getZExtValue()))) ||
             (data && data->isZero());
    }
    if (name == rt::cell_retain || name == rt::cell_release)
      return llvm::isa<llvm::ConstantPointerNull>(call.getArgOperand(0));
    return false;
  }

  // Drop the refcount calls whose answer is already settled. LLVM cannot do
  // it on its own: the runtime is an opaque declaration in this module. And
  // the constants are mostly its own doing, not the emitter's — the Long that
  // SROA promoted out of a register slot, the absent receiver a call passes as
  // TAG_NO_SELF, the null cell of a capture that turned out to be direct — so
  // this is a peephole inside the pipeline rather than a check at emission,
  // the way LLVM's ARC optimizer treats objc_retain/objc_release. The calls
  // are `nounwind`, so a settled one is never an `invoke`.
  struct DropSettledRefcounts : llvm::PassInfoMixin<DropSettledRefcounts> {
    llvm::PreservedAnalyses run(llvm::Function& f,
                                llvm::FunctionAnalysisManager&) {
      bool dropped = false;
      for (auto& bb : f) {
        for (auto& in : llvm::make_early_inc_range(bb)) {
          auto* call = llvm::dyn_cast<llvm::CallInst>(&in);
          if (!call || !is_settled_refcount(*call)) continue;
          call->eraseFromParent();
          dropped = true;
        }
      }
      if (!dropped) return llvm::PreservedAnalyses::all();
      llvm::PreservedAnalyses pa;
      pa.preserveSet<llvm::CFGAnalyses>();
      return pa;
    }
  };

  // None survived. Nothing else would notice if the peephole stopped firing —
  // what it drops are no-ops, so every result stays identical — which is why
  // this rides the assert lane (`just test-assert` and CI's `linux-assert`
  // run the whole sweep with it armed, and a release build pays nothing).
  static bool no_settled_refcounts(const llvm::Module& mod) {
    for (const auto& f : mod)
      for (const auto& bb : f)
        for (const auto& in : bb)
          if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&in))
            if (is_settled_refcount(*call)) return false;
    return true;
  }

  // A loop-carried Float travels as the i64 payload of its Value, so the phi
  // that carries one is an i64 whose incoming edges are `bitcast double` and
  // whose uses bitcast it straight back. A machine with separate integer and
  // float registers pays a register move each way, every iteration, on the
  // loop's critical path. InstCombine has this fold, but only for a phi whose
  // every incoming is a bitcast with no other user — and the emitter gives the
  // same bitcast to a second phi (the statement temp that holds the value for
  // its release), so it declines on exactly the loops that would gain most.
  //
  // Take it per connected web of phis instead, constants and poison allowed on
  // the way in, paying a bitcast back at any use that is not itself a bitcast
  // to double. That boundary is the one LLVM's own fold leaves behind when it
  // does fire; in the shapes this catches, those uses sit on the unwind path.
  //
  // What makes it sound is the incoming test alone: every edge already carries
  // a double, or a constant reinterpreted bit for bit as one. The phis change
  // type; no value changes.
  struct PromoteFloatPhis : llvm::PassInfoMixin<PromoteFloatPhis> {
    // The double behind an edge into the web, or null when the edge is not one.
    static llvm::Value* edge_double(llvm::Value* v) {
      auto* bc = llvm::dyn_cast<llvm::BitCastInst>(v);
      return bc && bc->getSrcTy()->isDoubleTy() ? bc->getOperand(0) : nullptr;
    }

    static bool reads_as_double(const llvm::User* u) {
      const auto* bc = llvm::dyn_cast<llvm::BitCastInst>(u);
      return bc && bc->getType()->isDoubleTy();
    }

    static bool carrier(const llvm::Value* v) {
      return llvm::isa<llvm::PHINode>(v) && v->getType()->isIntegerTy(64);
    }

    // The connected component of `seed` under phi-to-phi edges, in `web`.
    // False when an edge into it carries something that is not a double, a
    // constant or poison — the walk still finishes, so `seen` covers the whole
    // component and the caller skips the rest of it too.
    static bool collect(llvm::PHINode* seed,
                        llvm::SmallVectorImpl<llvm::PHINode*>& web,
                        llvm::SmallPtrSetImpl<llvm::PHINode*>& seen) {
      llvm::SmallVector<llvm::PHINode*, 8> work{seed};
      seen.insert(seed);
      web.push_back(seed);
      bool ok = true;
      while (!work.empty()) {
        auto* phi = work.pop_back_val();
        auto reach = [&](llvm::Value* v) {
          if (!carrier(v)) return false;
          auto* p = llvm::cast<llvm::PHINode>(v);
          if (seen.insert(p).second) {
            web.push_back(p);
            work.push_back(p);
          }
          return true;
        };
        for (llvm::Value* in : phi->incoming_values()) {
          if (reach(in) || llvm::isa<llvm::ConstantInt>(in) ||
              llvm::isa<llvm::UndefValue>(in) || edge_double(in))
            continue;
          ok = false;
        }
        for (llvm::User* u : phi->users()) reach(u);
      }
      return ok;
    }

    // Bitcasts the rewrite would remove against the ones it would add. A use
    // that already reads the payload as a double loses its bitcast; an
    // incoming one goes only when this web was its whole audience.
    static bool worth_it(const llvm::SmallVectorImpl<llvm::PHINode*>& web,
                         const llvm::SmallPtrSetImpl<llvm::PHINode*>& in_web) {
      unsigned gain = 0, cost = 0;
      llvm::SmallPtrSet<llvm::Value*, 8> edges;
      for (auto* phi : web) {
        for (llvm::Value* in : phi->incoming_values())
          if (edge_double(in)) edges.insert(in);
        for (const llvm::User* u : phi->users()) {
          if (carrier(u)) continue;
          if (reads_as_double(u)) ++gain;
          else ++cost;
        }
      }
      for (auto* e : edges) {
        bool only_web = true;
        for (const llvm::User* u : e->users())
          if (!carrier(u) || !in_web.contains(llvm::cast<llvm::PHINode>(u)))
            only_web = false;
        if (only_web) ++gain;
      }
      return gain > 0 && gain >= cost;
    }

    static void rewrite(llvm::SmallVectorImpl<llvm::PHINode*>& web) {
      auto& ctx = web.front()->getContext();
      auto* dbl = llvm::Type::getDoubleTy(ctx);
      llvm::SmallDenseMap<llvm::PHINode*, llvm::PHINode*, 8> promoted;
      for (auto* phi : web) {
        llvm::IRBuilder<> b(phi);
        auto* np = b.CreatePHI(dbl, phi->getNumIncomingValues(), phi->getName());
        np->setDebugLoc(phi->getDebugLoc());
        promoted[phi] = np;
      }
      llvm::SmallPtrSet<llvm::Instruction*, 8> stale;
      for (auto* phi : web) {
        auto* np = promoted[phi];
        for (unsigned i = 0, n = phi->getNumIncomingValues(); i < n; ++i) {
          llvm::Value* in = phi->getIncomingValue(i);
          llvm::Value* nv;
          if (carrier(in)) {
            nv = promoted[llvm::cast<llvm::PHINode>(in)];
          } else if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(in)) {
            nv = llvm::ConstantFP::get(
                ctx, llvm::APFloat(llvm::APFloat::IEEEdouble(), c->getValue()));
          } else if (llvm::isa<llvm::UndefValue>(in)) {
            nv = llvm::PoisonValue::get(dbl);
          } else {
            nv = edge_double(in);
            stale.insert(llvm::cast<llvm::Instruction>(in));
          }
          np->addIncoming(nv, phi->getIncomingBlock(i));
        }
      }
      for (auto* phi : web) {
        auto* np = promoted[phi];
        for (llvm::Use& u : llvm::make_early_inc_range(phi->uses())) {
          auto* user = llvm::cast<llvm::Instruction>(u.getUser());
          if (carrier(user)) continue;  // its own incoming edge, already set
          if (reads_as_double(user)) {
            user->replaceAllUsesWith(np);
            user->eraseFromParent();
            continue;
          }
          llvm::IRBuilder<> b(user);
          u.set(b.CreateBitCast(np, phi->getType(), np->getName() + ".bits"));
        }
      }
      // The webs reference each other, so every phi drops its operands before
      // any of them goes.
      for (auto* phi : web)
        phi->replaceAllUsesWith(llvm::PoisonValue::get(phi->getType()));
      for (auto* phi : web) phi->eraseFromParent();
      for (auto* bc : stale)
        if (bc->use_empty()) bc->eraseFromParent();
    }

    llvm::PreservedAnalyses run(llvm::Function& f,
                                llvm::FunctionAnalysisManager&) {
      llvm::SmallVector<llvm::PHINode*, 16> seeds;
      for (auto& bb : f)
        for (auto& phi : bb.phis())
          if (carrier(&phi)) seeds.push_back(&phi);

      bool changed = false;
      llvm::SmallPtrSet<llvm::PHINode*, 16> seen;
      for (auto* seed : seeds) {
        if (seen.contains(seed)) continue;
        llvm::SmallVector<llvm::PHINode*, 8> web;
        llvm::SmallPtrSet<llvm::PHINode*, 8> in_web;
        if (!collect(seed, web, in_web)) {
          seen.insert(in_web.begin(), in_web.end());
          continue;
        }
        seen.insert(in_web.begin(), in_web.end());
        if (!worth_it(web, in_web)) continue;
        rewrite(web);
        changed = true;
      }
      if (!changed) return llvm::PreservedAnalyses::all();
      llvm::PreservedAnalyses pa;
      pa.preserveSet<llvm::CFGAnalyses>();
      return pa;
    }
  };

  static void optimize_module(llvm::Module& mod, int opt_level) {
    using namespace llvm;
    PassBuilder PB;

    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    // After each InstCombine, so a tag the previous round settled is gone
    // before the next one reasons about what the call keeps alive — and once
    // more at the very end, because the module-optimization tail settles tags
    // no peephole round ever sees (loop peeling and the vectorizers run after
    // the last one; six calls across tests/*.cul survived without this).
    PB.registerPeepholeEPCallback(
        [](FunctionPassManager& FPM, OptimizationLevel) {
          FPM.addPass(DropSettledRefcounts());
        });
    // PromoteFloatPhis waits for the very end: the loop passes rotate and
    // rewrite the phis it reads, and it is the one pass here that wants the
    // shape they settle on rather than the shape they were handed.
    PB.registerOptimizerLastEPCallback([](ModulePassManager& MPM,
                                          OptimizationLevel,
                                          ThinOrFullLTOPhase) {
      MPM.addPass(createModuleToFunctionPassAdaptor(DropSettledRefcounts()));
      MPM.addPass(createModuleToFunctionPassAdaptor(PromoteFloatPhis()));
    });

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    OptimizationLevel level;
    switch (opt_level) {
      case 1:
        level = OptimizationLevel::O1;
        break;
      case 2:
        level = OptimizationLevel::O2;
        break;
      case 3:
        level = OptimizationLevel::O3;
        break;
      default:
        level = OptimizationLevel::O2;
        break;
    }

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
    MPM.run(mod, MAM);
    assert(no_settled_refcounts(mod));
    // The lowered IR is verified before it gets here (lower_program); this
    // says the pipeline — PromoteFloatPhis included, which is the one pass
    // here that rewrites types — handed back something still well formed.
    assert(!verifyModule(mod, &errs()));
  }



  // Publish the current node's source position (current_line_/column_, which
  // PosGuard keeps pointing at the innermost compiling node — the method-call
  // node here, since arg compiles restore it) just before a fallible runtime
  // call. A positionless CulebraError the call raises is then backfilled at the
  // exception boundaries (see _jit_thread.op_line). Two i64 stores; emit only before
  // calls whose helper can throw without a position (e.g. Tensor ops).
  void emit_set_op_pos() {
    emit_call(module_->getFunction(rt::set_op_pos),
              {builder_.getInt64(static_cast<int64_t>(current_line_)),
               builder_.getInt64(static_cast<int64_t>(current_column_))});
  }

  // Publish the current node (the postfix chain, per PosGuard) as the NEXT
  // call's boundary — where the interp's eval() boundary stamps a
  // positionless error escaping that call. Emitted at a positional UFCS
  // site after the arguments and immediately before the call, whose
  // set_call_site consumes it; every other call shape (and the kwargs UFCS
  // arms, whose ARG_LIST compiles inside the callee where a nested call
  // would consume the pending pair early) leaves the boundary defaulting
  // to its call site.
  void emit_set_call_boundary() {
    emit_call(module_->getOrInsertFunction(rt::set_call_boundary,
                                           builder_.getVoidTy(),
                                           builder_.getInt64Ty(),
                                           builder_.getInt64Ty()),
              {builder_.getInt64(static_cast<int64_t>(current_line_)),
               builder_.getInt64(static_cast<int64_t>(current_column_))});
  }


  // An inlined HOF body is the *callback's* frame emitted into the caller's,
  // so while it compiles, the callback's analysis — not the caller's — is what
  // says which of its locals a nested closure captures. Without the swap a
  // `let` in the inlined body lands in a plain stack slot and building that
  // nested closure fails with "free var 'x' is not a cell".
  struct InlinedInfoGuard {
    JIT& jit;
    const FuncInfo* saved;
    InlinedInfoGuard(JIT& j, const FuncInfo* info)
        : jit(j), saved(std::exchange(j.current_info_, info)) {}
    ~InlinedInfoGuard() { jit.current_info_ = saved; }
  };

  // The embedding entries (docs/deployment.md): run a program through LLVM
  // in this process, or emit it as an object file. Both compile the program
  // to bytecode and hand that to the lowering, so they are defined in
  // vm_lowering.h where the lowering lives — the JIT itself no longer reads
  // an AST.
  static void run(const std::shared_ptr<peg::Ast>& ast, bool emit_llvm = false,
                  bool debug = false, int opt_level = 2);
  // `modules` comes from ModuleLoader in topological order (deps first,
  // entry last).
  static void run_modules(const std::vector<LoadedModule>& modules,
                          bool emit_llvm = false, bool debug = false,
                          int opt_level = 2, bool fast_codegen = false);
  // The object gains a C `int main(int, char**)` that calls
  // `culebra_aot_bootstrap` (libculebra_rt.a) with `__culebra_main` as the
  // user-main pointer. `target_triple` empty means the host. 0 on success.
  static int build_object(const std::vector<LoadedModule>& modules,
                          const std::string& out_path, int opt_level = 2,
                          bool emit_llvm = false,
                          const std::string& target_triple = "");

 private:
  // Friend declared so extension implementations (e.g. JitExtension in
  // stdlib_jit.h) can reach JIT internals (builder_/module_/make_long/
  // extract_tag/...) without those being part of the public surface.
  friend struct JitExtension;
  friend struct vm::Lowering;

  llvm::LLVMContext& ctx_;
  llvm::Module* module_;
  llvm::IRBuilder<>& builder_;
  llvm::StructType* valueType_;    // {i8, i64}
  llvm::StructType* cellType_;     // {Value}
  llvm::StructType* closureType_;  // {ptr fn, i64 n, ptr captures}

  // The shared front-end analysis (fn_analysis.h) and its results
  // (func_info / scope_has_defer), one instance per compilation.
  FnAnalysis analysis_{&is_builtin_var};
  FuncInfo main_info_;

  // Holds the active function's Generic type-params ({"T", "U"}) while
  // its param annotations are compiled, so each can be lowered
  // (unbounded -> "Any", bounded -> bound trait; see lower_type_params).
  // Set for class methods (`class Foo<T, U>`) and free multifns
  // (compile_multifn_decl); compile_fn_common clears it before
  // descending so nested fns don't inherit it.
  std::vector<std::string_view> class_type_params_;



  // Currently compiling function's info (to know which locals are cells)
  const FuncInfo* current_info_ = nullptr;

  // LLVM function-level state for the function currently being compiled
  llvm::Value* current_closure_arg_ = nullptr;  // __cls__ argument
  llvm::Value* current_sret_ = nullptr;  // JitFn out-pointer (result slot)
  // Compiling a body the language guarantees a receiver for: it folds
  // TAG_NO_SELF to nil on entry, so `self` reads there skip the guard.
  bool in_receiver_frame_ = false;

  // Current AST position for error reporting
  size_t current_line_ = 0;
  size_t current_column_ = 0;



  // Current function's declared return type (empty = unchecked). The token is
  // owned by the AST, which outlives the compilation, so string_view is safe.
  std::string_view current_return_type_;

  // Where a return-value type error reports: the call site, resolved once in
  // the prologue (interp's check_type reads the caller's line/column). Null
  // when the function declares no return type. See emit_return_pos_snapshot.
  std::pair<llvm::Value*, llvm::Value*> current_return_pos_{nullptr, nullptr};

  // Defer-stack mark recorded at the current function's entry. `return`
  // lowers `defer_run_to(this mark)` before emitting `ret` so the
  // function's own defers run regardless of where the return sits.
  llvm::Value* current_fn_defer_mark_ = nullptr;
  // Per-LLVM-function cache of culebra_runtime_owned_hot()'s result
  // (see owned_hot_ptr). Reset wherever current_fn_defer_mark_ is —
  // an SSA value must never leak into a different function.
  llvm::Value* current_owned_hot_ = nullptr;

  // Recursion-guard state for the function being compiled: an entry-block
  // alloca holding the depth AFTER this frame's `recursion_enter`. Non-null
  // exactly when the prologue counted (a user body, or a field-init fn
  // serving as a default ctor) — return paths emit `recursion_leave` and
  // cleanup pads emit `recursion_restore(slot)` under the same gate, so an
  // uncounted frame (defer thunk, ctor wrapper) never underflows the count.
  llvm::Value* current_rec_depth_slot_ = nullptr;

  // Counter for generating unique function names. Process-wide so names
  // stay unique across separate JIT instances that share an `LLJIT` —
  // a per-instance counter would collide on `__culebra_fn_0` symbols.
  static inline std::atomic<int> funcCounter_{0};

  // Counter for per-callsite property-get inline-cache globals.
  int prop_ic_counter_ = 0;

  // Counter for per-callsite ObjectNewShaped shape-cache globals.
  int obj_shape_counter_ = 0;

  // Counter for per-callsite Op::ValueBox shape-cache globals.
  int value_box_counter_ = 0;

  // Counter for per-callsite property-write transitioning-IC globals.
  int prop_set_ic_counter_ = 0;


  JIT(llvm::LLVMContext* ctx, llvm::Module* mod, llvm::IRBuilder<>& builder)
      : ctx_(*ctx), module_(mod), builder_(builder) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    // {i64 tag, i64 data}: two full eightbytes so the struct's by-value ABI
    // (esp. register return) matches the C `JitValue` exactly — see JitValue.
    valueType_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), builder_.getInt64Ty()}, "Value");
    // All refcounted types have i32 refcount as field 0.
    cellType_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), valueType_}, "Cell");
    closureType_ = llvm::StructType::create(
        ctx_,
        {builder_.getInt64Ty(), ptrTy, builder_.getInt64Ty(), ptrTy,
         builder_.getInt64Ty()},
        "Closure");
  }

  // --- Scope management ---

  // Stable address of the owned stack's hot fields, fetched once per
  // function invocation (lazily, at the first scope entry — which
  // dominates every later scope). Lets scope entry/exit read
  // next_id / top_stamp with plain loads instead of runtime calls:
  // tight loop bodies (whose per-iteration scope is almost always an
  // empty region) pay ~3 inline instructions, not two calls.
  llvm::Value* owned_hot_ptr() {
    if (!current_owned_hot_) {
      current_owned_hot_ = emit_call(
          module_->getOrInsertFunction(rt::owned_hot, builder_.getInt64Ty()),
          {}, "owned.hot");
    }
    return current_owned_hot_;
  }

  // Inline empty-region fast path: only when an entry with id >= mark
  // exists (top_stamp > mark) does the slow-path resolution call run.
  void emit_owned_scope_exit(llvm::Value* mark) {
    if (!mark) return;
    auto i64Ty = builder_.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto hot = builder_.CreateIntToPtr(owned_hot_ptr(), ptrTy, "owned.hot.p");
    auto stampP = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, hot, offsetof(JitOwnedStack, top_stamp), "owned.stamp.p");
    auto stamp = builder_.CreateLoad(i64Ty, stampP, "owned.stamp");
    auto need = builder_.CreateICmpUGT(stamp, mark, "owned.nonempty");
    auto fn = builder_.GetInsertBlock()->getParent();
    auto slowBB = llvm::BasicBlock::Create(ctx_, "owned.exit", fn);
    auto contBB = llvm::BasicBlock::Create(ctx_, "owned.cont", fn);
    llvm::MDBuilder mdb(ctx_);
    builder_.CreateCondBr(need, slowBB, contBB,
                          mdb.createBranchWeights(1, 1u << 20));
    builder_.SetInsertPoint(slowBB);
    emit_call(module_->getOrInsertFunction(rt::owned_scope_exit,
                                           builder_.getVoidTy(), i64Ty),
              {mark});
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(contBB);
    }
    builder_.SetInsertPoint(contBB);
  }

  // Store with RC semantics into a frame-owned Value slot: releases the
  // previous contents (after replacing). Caller's `val` ownership is
  // absorbed into the slot.
  void store_slot(llvm::Value* slot, llvm::Value* val) {
    auto old = builder_.CreateLoad(valueType_, slot, "old");
    builder_.CreateStore(val, slot);
    emit_value_release(old);
  }

  // Block-pinned raw `+1`: the checked form of
  // a consumed value. Once a `+1` leaves its Owned handle it is invisible to
  // every ownership layer, so it may only be used inside the basic block
  // where it was consumed — emit_call turns every may-throw call into an
  // invoke that terminates the current block, so "same block" structurally
  // means "no unwind edge and no branch can strand it". The conversion
  // operators enforce that pin; a violation is a compiler bug and aborts
  // loudly (all build modes) rather than shipping a silent leak. Values that
  // must cross a block go through a scope slot, an OwnedPhi merge, or
  // a justified consume_unchecked().
  struct Pinned {
    llvm::Value* val_ = nullptr;
    // Pin block; null = exempt (a null sentinel or a constant %Value, which
    // carries no heap +1 — the same exemption as Owned::reg_()).
    llvm::BasicBlock* bb_ = nullptr;
    JIT* jit_ = nullptr;

    Pinned(JIT* jit, llvm::Value* val) : val_(val), jit_(jit) {
      if (jit_ && val_ && !llvm::isa<llvm::Constant>(val_))
        bb_ = jit_->builder_.GetInsertBlock();
    }

    llvm::Value* get() const {
      if (bb_ && jit_->builder_.GetInsertBlock() != bb_)
        jit_->rc_pin_violation("a consumed +1 was used outside its pin block",
                               bb_);
      return val_;
    }
    operator llvm::Value*() const { return get(); }
    llvm::Value* operator->() const { return get(); }
  };

  // RAII handle for a `+1`-owned Value during codegen: must be consumed
  // exactly once (consume() hands the +1 on); if dropped unconsumed the dtor
  // emits the release, and consuming twice asserts — making an accidental
  // leak and a double-free both structurally impossible. Manages straight-line
  // expression temporaries only; values escaping into branches/loops go
  // through the scope/slot machinery. The dtor releases at the builder's
  // *current* insertion point, so a handle must be consumed-or-dropped while
  // the builder still sits on the block where the value is live.
  struct Owned {
    JIT* jit_ = nullptr;
    llvm::Value* val_ = nullptr;
    bool consumed_ = false;
    // Index into jit_->live_owned_ while this handle owns a heap-capable
    // value; SIZE_MAX when unregistered (empty, consumed, constant scalar).
    // The registry is what lets emit_call spill every live +1 across a
    // may-throw call (the automatic unwind-temp window) — registration is
    // pure codegen bookkeeping and emits no IR.
    size_t live_idx_ = SIZE_MAX;

    Owned() = default;
    Owned(JIT* jit, llvm::Value* val) : jit_(jit), val_(val) { reg_(); }

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    Owned(Owned&& o) noexcept
        : jit_(o.jit_), val_(o.val_), consumed_(o.consumed_) {
      take_registration_(o);
      o.consumed_ = true;
      o.val_ = nullptr;
    }
    Owned& operator=(Owned&& o) noexcept {
      if (this != &o) {
        release_if_owned();
        jit_ = o.jit_;
        val_ = o.val_;
        consumed_ = o.consumed_;
        take_registration_(o);
        o.consumed_ = true;
        o.val_ = nullptr;
      }
      return *this;
    }

    // A constant %Value (nil / literal scalar) can never carry a heap +1, so
    // it needs no unwind window; skipping it keeps the registry (and the
    // spill traffic) limited to values that can actually leak.
    void reg_() {
      if (!jit_ || !val_ || llvm::isa<llvm::Constant>(val_)) return;
      live_idx_ = jit_->live_owned_.size();
      jit_->live_owned_.push_back(this);
    }
    void unreg_() {
      if (live_idx_ == SIZE_MAX) return;
      auto& reg = jit_->live_owned_;
      reg[live_idx_] = reg.back();
      reg[live_idx_]->live_idx_ = live_idx_;
      reg.pop_back();
      live_idx_ = SIZE_MAX;
    }
    void take_registration_(Owned& o) {
      live_idx_ = o.live_idx_;
      o.live_idx_ = SIZE_MAX;
      if (live_idx_ != SIZE_MAX) jit_->live_owned_[live_idx_] = this;
    }

    // Emit the release iff this handle still owns a value.
    void release_if_owned() {
      if (!consumed_ && val_) jit_->emit_value_release(val_);
      unreg_();
    }

    // Read the Value's tag/data for a non-consuming use (operator dispatch,
    // coercion); ownership stays with the handle.
    llvm::Value* borrow() const { return val_; }

    // Hand the `+1` on: returns the raw Value block-pinned (usable
    // only inside the current basic block; see Pinned) and marks the handle
    // spent so its destructor stays silent. Consuming twice is a
    // codegen-time bug.
    Pinned consume() {
      return Pinned(jit_, consume_unchecked());
    }

    // The unpinned escape hatch: hands the raw `+1` on with no
    // block-crossing check. Legal only where a documented contract names
    // another releaser for every CFG edge the raw can cross (a batch handoff
    // into mutually-exclusive dispatch arms, a region-scope slot that owns
    // the value) — justify each call site in a comment; the count is
    // ratcheted by tools/checks/check_rc_discipline.sh.
    llvm::Value* consume_unchecked() {
      assert(!consumed_ && "Owned value consumed twice");
      consumed_ = true;
      unreg_();
      return val_;
    }

    // Release the `+1` now, at the current insertion point, and mark the
    // handle spent (the dtor then stays silent). The explicit-release twin of
    // `consume()` for a value that is simply dead rather than handed on — use
    // it where the release must land before later IR (e.g. a `make_*` that
    // emits) so the scope-exit dtor would place it too late.
    void drop() {
      release_if_owned();
      consumed_ = true;
    }

    ~Owned() { release_if_owned(); }
  };

  // A Pinned/OwnedPhi discipline violation: a raw `+1` crossed a basic block
  // during codegen. This is a bug in the compiler, not the user program —
  // report the codegen site (source position pins which construct) and abort
  // in every build mode, so the difftest corpus catches a violating pattern
  // the first time it is *compiled* (no runtime leak repro needed). Not a
  // CulebraError: compile()'s catch would stamp a user position on it and
  // surface it as a user-facing error.
  [[noreturn]] void rc_pin_violation(const char* what, llvm::BasicBlock* pin) {
    auto* cur = builder_.GetInsertBlock();
    fprintf(stderr,
            "[rc-pin] %s at %zu:%zu (fn %s: %s -> %s) — hold it in an Owned "
            "or a scope slot across the boundary\n",
            what, current_line_, current_column_,
            cur ? cur->getParent()->getName().str().c_str() : "?",
            pin ? pin->getName().str().c_str() : "?",
            cur ? cur->getName().str().c_str() : "?");
    abort();
  }

  // RAII throw-cleanup for borrowed `+1` temporaries that are live across a
  // may-throw call and would otherwise strand on the runtime unwind edge — the
  // borrowed callee closure held across a call (`compile_call`) or a builtin
  // method's borrowed receiver (`compile_builtin_method` /
  // `compile_user_method_over_builtin`), whose helper may raise. The caller's
  // `Owned` handle releases the value on the *normal* path only: a C++ RAII
  // destructor cannot run on an LLVM-level `throw`, so without this the value
  // leaks whenever the callee throws.
  //
  // Reuses the container-literal cleanup machinery: spill each value to
  // an entry alloca (never an SSA value live across the unwind edge — SDAG-O0's
  // register coalescer crashes on that form), redirect `current_lpad_` to a
  // fresh cleanup pad for the guard's lifetime, and on close fill that pad
  // ("release each guarded value, re-raise to the outer lpad") or erase it when
  // nothing inside threw (pred_empty DCE — zero happy-path cost). The normal and
  // throw paths are mutually exclusive, so each value frees exactly once.
  struct ThrowGuard {
    JIT* jit_ = nullptr;
    llvm::BasicBlock* cleanupBB_ = nullptr;
    llvm::BasicBlock* savedLpad_ = nullptr;
    std::vector<llvm::Value*> guards_;
    bool closed_ = false;

    // The guard's own pad is the sole unwind-edge releaser for its values, so
    // they are also declared unwind-covered for its lifetime — the automatic
    // unwind-temp window must not spill them a second time.
    std::vector<llvm::Value*> covered_;

    ThrowGuard(JIT* jit, std::initializer_list<llvm::Value*> vals) : jit_(jit) {
      savedLpad_ = jit_->current_lpad_;
      auto fn = jit_->builder_.GetInsertBlock()->getParent();
      cleanupBB_ = llvm::BasicBlock::Create(jit_->ctx_, "throw.cleanup", fn);
      guards_.reserve(vals.size());
      for (auto* v : vals) guards_.push_back(jit_->make_build_guard(v));
      covered_.assign(vals);
      jit_->cover_on_unwind(covered_);
      jit_->current_lpad_ = cleanupBB_;
    }

    // Fill (or DCE) the cleanup pad and restore the outer landingpad. Idempotent
    // and dtor-invoked, so a straight-line region needs no explicit close.
    void close() {
      if (closed_) return;
      closed_ = true;
      jit_->uncover_on_unwind(covered_);
      jit_->current_lpad_ = savedLpad_;
      jit_->finish_construction_cleanup(cleanupBB_, guards_, savedLpad_);
    }
    ~ThrowGuard() { close(); }

    ThrowGuard(const ThrowGuard&) = delete;
    ThrowGuard& operator=(const ThrowGuard&) = delete;
  };

  // --- The automatic unwind-temp window ---
  //
  // A codegen-owned `+1` held in an `Owned` is released by a C++ destructor,
  // which the runtime's LLVM-level throw cannot run — historically every
  // "Owned live across a may-throw call" site (a binop lhs while the rhs
  // compiles, a receiver while an argument compiles, …) stranded its value on
  // the unwind edge. Instead of guarding such sites one by one, emit_call
  // spills every live, uncovered Owned into a per-function pool slot for
  // exactly the duration of each may-throw call (store before the invoke,
  // nil-clear in the continuation), and the scope-chain cleanup pads release
  // the pool (release_unwind_temps). Coverage is complete by construction:
  // the only runtime events that can unwind are the calls emit_call emits.
  //
  // live_owned_ is the registry of in-flight handles (per LLVM function),
  // unwind_temp_slots_ the slot pool (slot i is
  // reused across calls — windows never overlap at runtime, a frame executes
  // one call at a time), and unwind_covered_ the values currently excluded
  // from windows because a callee-cleans contract names another
  // unwind-edge releaser (a ThrowGuard pad, a callee-cleans helper) — spilling
  // those too would double-free, the ASan-confirmed overlap trap.
  std::vector<Owned*> live_owned_;
  std::vector<llvm::Value*> unwind_temp_slots_;
  std::vector<llvm::Value*> unwind_covered_;
  // True while a cleanup pad's own contents are being emitted: releases
  // emitted inside a pad must not re-arm windows over the pool they are
  // about to release.
  bool emitting_unwind_cleanup_ = false;

  // Entry-block slot holding the exception a cleanup region is carrying, from
  // the landingpad that receives it to the edge that hands it on. One per
  // LLVM function: a frame unwinds one exception at a time, and between those
  // two points a pad calls runtime helpers only — a user `drop` or `defer`
  // body is its own function with its own slot.
  llvm::Value* exc_slot_ = nullptr;

  llvm::Value* exception_slot() {
    if (!exc_slot_) {
      auto fn = builder_.GetInsertBlock()->getParent();
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      exc_slot_ = entryB.CreateAlloca(llvm::PointerType::get(ctx_, 0), nullptr,
                                      "exc.slot");
    }
    return exc_slot_;
  }

  void cover_on_unwind(const std::vector<llvm::Value*>& vals) {
    unwind_covered_.insert(unwind_covered_.end(), vals.begin(), vals.end());
  }
  void uncover_on_unwind(const std::vector<llvm::Value*>& vals) {
    // Erase by value (last occurrence): guards may close out of strict LIFO
    // order (an explicit ThrowGuard::close before an inner cover ends).
    for (auto* v : vals) {
      auto it = std::find(unwind_covered_.rbegin(), unwind_covered_.rend(), v);
      if (it != unwind_covered_.rend()) unwind_covered_.erase(std::next(it).base());
    }
  }

  // Scoped callee-cleans declaration: while alive, the calls emitted have a
  // callee-side cleaner for `vals` on the unwind edge (an operator helper's
  // body-wide release, a pending-guard window), so the automatic unwind-temp
  // window skips them — each edge keeps exactly one releaser.
  struct UnwindCovered {
    JIT* jit_;
    std::vector<llvm::Value*> vals_;
    UnwindCovered(JIT* jit, std::vector<llvm::Value*> vals)
        : jit_(jit), vals_(std::move(vals)) {
      jit_->cover_on_unwind(vals_);
    }
    ~UnwindCovered() { jit_->uncover_on_unwind(vals_); }
    UnwindCovered(const UnwindCovered&) = delete;
    UnwindCovered& operator=(const UnwindCovered&) = delete;
  };

  // Spill every live, uncovered Owned `+1` into a pool slot for the duration
  // of one may-throw call. Returns the number of slots armed.
  size_t open_unwind_window() {
    // Diagnostic kill switch (CULEBRA_GC_NEVER's sibling): compiling with the
    // windows off discriminates "window double-free" from a pre-existing
    // over-release when bisecting an ASan report.
    static bool disabled =
        std::getenv("CULEBRA_JIT_NO_UNWIND_TEMPS") != nullptr;
    if (disabled || emitting_unwind_cleanup_ || live_owned_.empty()) return 0;
    size_t n = 0;
    for (auto* o : live_owned_) {
      if (std::find(unwind_covered_.begin(), unwind_covered_.end(), o->val_) !=
          unwind_covered_.end())
        continue;
      if (n == unwind_temp_slots_.size())
        unwind_temp_slots_.push_back(make_pending_guard());
      builder_.CreateStore(o->val_, unwind_temp_slots_[n++]);
    }
    return n;
  }
  void close_unwind_window(size_t n) {
    for (size_t i = 0; i < n; i++) clear_pending_guard(unwind_temp_slots_[i]);
  }

  // Release (then nil-clear, so an outer pad on the same unwind chain no-ops)
  // the unwind-temp pool. Runs at the top of every scope-family cleanup pad —
  // before the region's defers, matching the interpreter, where the throwing
  // expression's temporaries die as its eval frames unwind, ahead of any
  // enclosing block's defers.
  //
  // A window fills the pool from slot 0 up and clears it again in the call's
  // continuation, so above the widest window every slot is nil and its
  // release is dead code — releasing the whole pool is always safe.
  void release_unwind_temps() {
    for (size_t i = 0; i < unwind_temp_slots_.size(); i++)
      release_pending_guard(unwind_temp_slots_[i]);
  }

  // RAII flag for emitting a cleanup pad's contents (scope teardown,
  // construction cleanup): suppresses window arming inside the pad.
  struct UnwindCleanupEmission {
    JIT* jit_;
    bool prev_;
    explicit UnwindCleanupEmission(JIT* jit)
        : jit_(jit), prev_(jit->emitting_unwind_cleanup_) {
      jit->emitting_unwind_cleanup_ = true;
    }
    ~UnwindCleanupEmission() { jit_->emitting_unwind_cleanup_ = prev_; }
    UnwindCleanupEmission(const UnwindCleanupEmission&) = delete;
    UnwindCleanupEmission& operator=(const UnwindCleanupEmission&) = delete;
  };

  // One lexical cleanup region's throw path: its landingpad entries, the
  // releases they run, and the single edge that carries the throw onward.
  //
  // A cleanup pad does not handle the exception — it runs what the region
  // owes (in-flight temporaries, defers, scope slots, an iterator's dispose)
  // and hands the still-in-flight throw to the enclosing pad, or out of the
  // function. So it never opens the exception: the ABI's handler count stays
  // untouched until the pad that does handle it opens and closes it once,
  // which is what lets libsupc++ free the exception object. Clang emits a
  // cleanup the same way; culebra differs only in the catch-all clause
  // (see emit_landingpad) and in continuing the unwind explicitly, since the
  // enclosing region's pad is a separate block a `resume` would step over.
  //
  // Entries, releases and exit are one object so a region cannot be opened
  // without being continued: the destructor emits the exit, at the current
  // insertion point — `open` leaves the builder in the entry block, which is
  // where a single-entry region's releases go; a multi-entry region (the
  // frame's ladder) points the builder at the end of its shared descent
  // chain before this object dies.
  class CleanupPad {
   public:
    CleanupPad(JIT& jit, llvm::BasicBlock* outerLpad)
        : jit_(jit),
          outer_(outerLpad),
          saved_insert_(jit.builder_.GetInsertBlock()),
          cleanup_scope_(&jit) {}

    // Make `padBB` an entry of this region — unless nothing unwinds to it, in
    // which case it is erased and this returns false. That is the can-throw
    // gate every pad has: a region whose body cannot throw costs nothing.
    bool open(llvm::BasicBlock* padBB, const char* name) {
      if (llvm::pred_empty(padBB)) {
        padBB->eraseFromParent();
        return false;
      }
      // A landingpad needs the function to carry a personality; a body with no
      // try/defer of its own never set one. Idempotent.
      auto fn = padBB->getParent();
      if (!fn->hasPersonalityFn())
        fn->setPersonalityFn(jit_.get_personality_fn());
      jit_.emit_landingpad(padBB, name, /*open=*/false);
      opened_ = true;
      return true;
    }

    ~CleanupPad() {
      auto& b = jit_.builder_;
      if (opened_ && !b.GetInsertBlock()->getTerminator()) {
        auto ptrTy = llvm::PointerType::get(jit_.ctx_, 0);
        auto fn = b.GetInsertBlock()->getParent();
        auto exc = b.CreateLoad(ptrTy, jit_.exception_slot(), "exc");
        auto resumeFn = jit_.module_->getOrInsertFunction(
            rt_unwind_resume, b.getInt32Ty(), ptrTy);
        if (outer_) {
          auto goneBB = llvm::BasicBlock::Create(jit_.ctx_, "unwind.gone", fn);
          b.CreateInvoke(resumeFn, goneBB, outer_, {exc});
          b.SetInsertPoint(goneBB);
        } else {
          b.CreateCall(resumeFn, {exc});
        }
        b.CreateUnreachable();
      }
      b.SetInsertPoint(saved_insert_);
    }

    CleanupPad(const CleanupPad&) = delete;
    CleanupPad& operator=(const CleanupPad&) = delete;

   private:
    JIT& jit_;
    llvm::BasicBlock* outer_;
    llvm::BasicBlock* saved_insert_;
    UnwindCleanupEmission cleanup_scope_;
    bool opened_ = false;
  };

  // Wrap a freshly produced `+1`-owned Value in an ownership handle.
  Owned own(llvm::Value* val) { return Owned(this, val); }

  // Consume a batch of Owned values into raw `+1` Values at the single
  // handoff point into the raw call emitters. The raws may be referenced
  // from mutually-exclusive dispatch branches (method/UFCS, call/__call__),
  // so each +1 is still consumed exactly once per runtime path. Takes the
  // vector by rvalue so the spent handles can't linger at the call site.
  std::vector<llvm::Value*> consume_all(std::vector<Owned>&& vals) {
    std::vector<llvm::Value*> raw;
    raw.reserve(vals.size());
    // Batch handoff into mutually-exclusive dispatch arms: each raw still
    // consumed exactly once per runtime path (see the contract above), so
    // the block-pin check does not apply.
    for (auto& v : vals) raw.push_back(v.consume_unchecked());
    return raw;
  }

  // Owned merge: the checked way to join per-arm `+1` results into a
  // `%Value` phi. Each incoming is captured *in its arm block* — either
  // consumed out of an Owned on the spot (window-covered until that instant)
  // or, for a raw, proven to be a constant or produced in the current block
  // (an intervening may-throw call would have terminated the block). finish()
  // then verifies every recorded arm still branches straight to the merge
  // block — an emit_call slipped in after add_incoming would have
  // re-terminated the arm toward its call.cont, i.e. an unwind edge crossed
  // the bare value — and builds the phi at the merge front. Emits exactly
  // the IR the hand-written CreatePHI/addIncoming pattern did: the safety is
  // codegen-time bookkeeping, not extra instructions.
  struct OwnedPhi {
    JIT* jit_;
    std::string name_;
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming_;

    OwnedPhi(JIT* jit, std::string name) : jit_(jit), name_(std::move(name)) {}

    void add_incoming(Owned&& v) {
      incoming_.emplace_back(v.consume_unchecked(),
                             jit_->builder_.GetInsertBlock());
    }
    void add_incoming(llvm::Value* v) {
      if (v && !llvm::isa<llvm::Constant>(v) && !produced_here_(v))
        jit_->rc_pin_violation("a raw phi incoming was produced outside the "
                               "current arm block",
                               llvm::isa<llvm::Instruction>(v)
                                   ? llvm::cast<llvm::Instruction>(v)
                                         ->getParent()
                                   : nullptr);
      incoming_.emplace_back(v, jit_->builder_.GetInsertBlock());
    }

    // A raw incoming is safe when nothing throw-capable ran while it was
    // bare: produced in the current block (emit_call splits blocks on every
    // may-throw call, so block identity implies that), or produced by the
    // invoke whose normal destination the builder now sits on (the value is
    // born on that edge; its own unwind edge never materialises it).
    bool produced_here_(llvm::Value* v) const {
      auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
      if (!inst) return false;
      auto* cur = jit_->builder_.GetInsertBlock();
      if (inst->getParent() == cur) return true;
      auto* inv = llvm::dyn_cast<llvm::InvokeInst>(inst);
      return inv && inv->getNormalDest() == cur;
    }

    // Build the phi. The caller has already terminated every arm toward
    // `mergeBB` and (typically) sits at the merge; the phi is inserted at
    // the block front so it composes with sibling scalar phis regardless of
    // call order.
    Owned finish(llvm::BasicBlock* mergeBB) {
      for (auto& [v, bb] : incoming_) {
        auto* term = bb->getTerminator();
        bool targets_merge = false;
        for (unsigned i = 0; term && i < term->getNumSuccessors(); ++i)
          if (term->getSuccessor(i) == mergeBB) targets_merge = true;
        if (!targets_merge)
          jit_->rc_pin_violation(
              "a phi arm no longer branches to its merge block (a may-throw "
              "call was emitted after add_incoming)", bb);
        (void)v;
      }
      auto saved = jit_->builder_.saveIP();
      jit_->builder_.SetInsertPoint(mergeBB, mergeBB->begin());
      auto phi = jit_->builder_.CreatePHI(jit_->valueType_,
                                          incoming_.size(), name_);
      for (auto& [v, bb] : incoming_) phi->addIncoming(v, bb);
      jit_->builder_.restoreIP(saved);
      return Owned(jit_, phi);
    }
  };

  // One refcount helper call. Both helpers open with the same
  // `_is_refcounted_value_tag` guard (jit_mem.h), so a tag the emitter already
  // knows answers it here: the call is simply not emitted, rather than emitted
  // for DropSettledRefcounts to sweep back out.
  void emit_refcount_call(const char* sym, llvm::Value* tag,
                          llvm::Value* data) {
    if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(tag))
      if (!_is_refcounted_value_tag(static_cast<int8_t>(k->getZExtValue())))
        return;
    emit_call(module_->getOrInsertFunction(sym, builder_.getVoidTy(),
                                           builder_.getInt8Ty(),
                                           builder_.getInt64Ty()),
              {tag, data});
  }

  // Emit IR to retain a Value (no-op for non-refcounted tags; done in runtime).
  void emit_value_retain(llvm::Value* val) {
    emit_refcount_call(rt::value_retain, extract_tag(val), extract_data(val));
  }

  // Borrow → +1. The caller passes a value it does not own (an element read
  // straight out of a container, a receiver it only borrows) and takes
  // ownership of the result, so whatever slot receives it releases
  // symmetrically. One named seam for the conversion instead of a bare retain
  // at each site.
  llvm::Value* emit_borrow_to_owned(llvm::Value* val) {
    emit_value_retain(val);
    return val;
  }

  // `_is_refcounted_value_tag` as IR: does this tag name a value release and
  // retain do any work for? The mask is folded out of the predicate itself, so
  // a tag that joins the set joins here too. Every refcounted tag is small; the
  // range test is what keeps the sentinels out (TAG_KWREST 125, TAG_NO_SELF
  // 126, TAG_UNFILLED 127) without a poison shift.
  llvm::Value* emit_tag_is_refcounted(llvm::Value* tag) {
    constexpr uint32_t kMask = [] {
      uint32_t m = 0;
      for (int t = 0; t < 32; ++t)
        if (_is_refcounted_value_tag(static_cast<int8_t>(t))) m |= 1u << t;
      return m;
    }();
    static_assert(kMask != 0, "no refcounted tag fits the mask any more");
    auto i32 = builder_.getInt32Ty();
    auto t = builder_.CreateZExt(tag, i32, "rc.tag");
    auto in_range = builder_.CreateICmpULT(t, builder_.getInt32(32));
    auto bit = builder_.CreateAnd(
        builder_.CreateLShr(builder_.getInt32(kMask),
                            builder_.CreateAnd(t, builder_.getInt32(31))),
        builder_.getInt32(1));
    return builder_.CreateAnd(
        in_range, builder_.CreateICmpNE(bit, builder_.getInt32(0)), "rc.owned");
  }

  void emit_value_release(llvm::Value* val) {
    // An undef %Value carries no ownership, so there is nothing to release.
    // Emitting release(undef,undef) is actively unsafe: at -O0 the call reads
    // whatever garbage the arg registers hold and derefs `data` whenever the
    // tag byte lands on a refcounted value (SIGBUS). Producers should yield a
    // real %Value (nil) instead — `compile()` asserts that — but this stays a
    // permanent backstop since releasing undef is never meaningful.
    if (llvm::isa<llvm::UndefValue>(val)) return;
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    // A tag the emitter already knows needs no branch at all.
    if (llvm::isa<llvm::ConstantInt>(tag)) {
      emit_refcount_call(rt::value_release, tag, data);
      return;
    }
    // The helper's own first act is this test, but reaching it costs a call
    // into an opaque symbol — and on the frame path most of what a release
    // sees is a Long, a Float or an absent receiver. Hoisting the test into
    // the caller made a one-line function call 17% cheaper. The helper keeps
    // its copy: this is a fast path, not the contract.
    auto* fn = builder_.GetInsertBlock()->getParent();
    auto ownedBB = llvm::BasicBlock::Create(ctx_, "rc.release", fn);
    auto contBB = llvm::BasicBlock::Create(ctx_, "rc.cont", fn);
    builder_.CreateCondBr(emit_tag_is_refcounted(tag), ownedBB, contBB);
    builder_.SetInsertPoint(ownedBB);
    emit_refcount_call(rt::value_release, tag, data);
    if (!builder_.GetInsertBlock()->getTerminator())
      builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
  }


  void emit_cell_retain(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_retain,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  void emit_cell_release(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_release,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  llvm::Value* current_line_val() {
    return builder_.getInt64(static_cast<int64_t>(current_line_));
  }
  llvm::Value* current_column_val() {
    return builder_.getInt64(static_cast<int64_t>(current_column_));
  }

  // Declare (or fetch) a runtime helper and stamp `noreturn` on it.
  // Every emit_*_throw goes through here so the optimizer treats the
  // call site as terminating.
  llvm::FunctionCallee declare_noreturn(
      const char* name,
      llvm::ArrayRef<llvm::Type*> args) {
    auto fnTy = llvm::FunctionType::get(builder_.getVoidTy(), args, false);
    auto callee = module_->getOrInsertFunction(name, fnTy);
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
      fn->setDoesNotReturn();
    }
    return callee;
  }

  // Emit a `culebra_runtime_immutable_assign(name, line, col)` call
  // that always throws — `compile_assignment` uses this to halt the
  // current path before storing into a non-mut slot.
  void emit_immutable_assign_throw(const std::string& name,
                                    size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto namePtr = get_or_create_global_str(name, ".imm.name");
    emit_call(
        declare_noreturn(rt::immutable_assign,
                         {ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {namePtr, builder_.getInt64(line), builder_.getInt64(col)});
  }


  // Compute "receiver has an own slot `name`" as an i1, while the receiver
  // value is still live (before any swap/release at the call site). A cheap
  // compile-time short-circuit: only built-in method names can ever trigger
  // the reject, so for any other property this is a constant false and emits
  // no runtime call. See emit_reject_bare_builtin_method for why this matters.
  llvm::Value* emit_has_own_field(llvm::Value* receiver,
                                   const std::string& name) {
    if (!culebra::is_builtin_method_name(name)) return builder_.getFalse();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto keyPtr = get_or_create_global_str(name, ".hof.key");
    return emit_call(
        module_->getOrInsertFunction(
            rt::object_has_own_field, builder_.getInt1Ty(),
            builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy),
        {extract_tag(receiver), extract_data(receiver), keyPtr},
        "has.own.field");
  }

  // Reject a bare reference to a value-type built-in method (`let m = x.map`):
  // it is dispatched inline with no closure to hand back, so it's not a
  // first-class value on either backend. `propResult` is the property-get
  // result — Nil for such a method (and for any receiver simply lacking the
  // property); a user-defined own property resolves to a non-Nil value (or,
  // when nil-valued, is caught by `hasOwnField`) and stays first-class. The
  // Nil-and-absent case branches to a cold runtime check that consults the
  // receiver's actual builtin table (the interp's reject fires per receiver
  // type): it throws only when the interp would, so `C.join` / `{}.map` read
  // as nil like every other miss. `receiver` must still be owned-live at this
  // point. No-op when `name` isn't a built-in method name.
  void emit_reject_bare_builtin_method(llvm::Value* propResult,
                                       llvm::Value* hasOwnField,
                                       llvm::Value* receiver,
                                       const std::string& name,
                                       int64_t line, int64_t col) {
    if (!culebra::is_builtin_method_name(name)) return;
    auto fn = builder_.GetInsertBlock()->getParent();
    auto isNil = builder_.CreateICmpEQ(extract_tag(propResult),
                                       builder_.getInt8(TAG_NIL));
    // A user Object with an own property of this name — even nil-valued — is a
    // first-class field, not a built-in method handle (interp returns it via
    // `obj.has` precedence). Check only a genuinely absent property: Nil
    // result AND no own slot (non-Object receivers never have own slots).
    auto reject = builder_.CreateAnd(
        isNil, builder_.CreateNot(hasOwnField), "bm.reject");
    auto coldBB = llvm::BasicBlock::Create(ctx_, "bm.check", fn);
    auto contBB = llvm::BasicBlock::Create(ctx_, "bm.cont", fn);
    builder_.CreateCondBr(reject, coldBB, contBB);
    builder_.SetInsertPoint(coldBB);
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto keyPtr = get_or_create_global_str(name, ".bm.key");
    emit_call(
        module_->getOrInsertFunction(
            rt::bare_builtin_reject, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {extract_tag(receiver), extract_data(receiver), keyPtr,
         builder_.getInt64(line), builder_.getInt64(col)});
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
  }


  // Generic compile-time-detected error → runtime throw. Used to
  // migrate previously-uncatchable `throw culebra::CulebraError(...)`
  // sites in `compile_*` so `try { ... } catch e { ... }` can observe
  // them the same way interp does.
  void emit_throw_error(const char* kind, const std::string& msg,
                         size_t line, size_t col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto kindPtr = get_or_create_global_str(kind, ".thr.kind");
    auto msgPtr = get_or_create_global_str(msg, ".thr.msg");
    emit_call(
        declare_noreturn(rt::throw_error,
                         {ptrTy, ptrTy, builder_.getInt64Ty(),
                          builder_.getInt64Ty()}),
        {kindPtr, msgPtr,
         builder_.getInt64(line), builder_.getInt64(col)});
  }

  // Emit a typed type-error throw with "expected X, got Y" context.
  // `expected` is a compile-time string literal; `got_tag` is the i8
  // LLVM value of the actual operand's runtime tag (e.g. extract_tag(v)).
  void emit_type_error_typed(const char* expected, llvm::Value* got_tag) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto exp_str = builder_.CreateGlobalString(expected);
    emit_call(
        module_->getOrInsertFunction(rt::type_error_typed,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty(),
                                     ptrTy,
                                     builder_.getInt8Ty()),
        {current_line_val(), current_column_val(), exp_str, got_tag});
  }

  void emit_div_zero() {
    emit_call(
        module_->getOrInsertFunction(rt::div_zero,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {current_line_val(), current_column_val()});
  }

  // MIN / -1 wraps like the rest of Long arithmetic (language.md:
  // "Overflow wraps") -- sdiv/srem on that pair is poison (and SIGFPE on
  // x86), so the divisor is nudged to 1 on the edge and the wrapped
  // answer (MIN for /, 0 for %) selected in. Branch-free: two icmps and
  // two selects that the optimizer folds away for constant operands.
  llvm::Value* emit_wrapping_sdiv(llvm::Value* ld, llvm::Value* rd,
                                  bool rem) {
    auto min = llvm::ConstantInt::get(builder_.getInt64Ty(),
                                      std::numeric_limits<int64_t>::min());
    auto neg1 = llvm::ConstantInt::get(builder_.getInt64Ty(), -1);
    auto one = llvm::ConstantInt::get(builder_.getInt64Ty(), 1);
    auto edge = builder_.CreateAnd(builder_.CreateICmpEQ(ld, min, "l.min"),
                                   builder_.CreateICmpEQ(rd, neg1, "r.neg1"),
                                   "div.edge");
    auto safe = builder_.CreateSelect(edge, one, rd, "div.safe");
    if (rem) {
      auto r = builder_.CreateSRem(ld, safe, "mod");
      return builder_.CreateSelect(
          edge, llvm::ConstantInt::get(builder_.getInt64Ty(), 0), r, "mod.w");
    }
    auto q = builder_.CreateSDiv(ld, safe, "div");
    return builder_.CreateSelect(edge, min, q, "div.w");
  }

  // Emit a divide-by-zero check on `divisor`. On zero, throws via
  // emit_div_zero (terminates the basic block); otherwise leaves the
  // builder positioned at the ok-path BB, ready for SDiv/SRem/FDiv/FRem.
  // The compare is selected from `divisor`'s LLVM type; `label_prefix`
  // is used for both BB IR labels (e.g. "div", "fdiv", "mod").
  void emit_div_zero_guard(llvm::Value* divisor, const char* label_prefix) {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::Value* isZero;
    if (divisor->getType()->isFloatingPointTy()) {
      auto zero = llvm::ConstantFP::get(divisor->getType(), 0.0);
      isZero = builder_.CreateFCmpOEQ(divisor, zero, "iszero");
    } else {
      auto zero = llvm::ConstantInt::get(divisor->getType(), 0);
      isZero = builder_.CreateICmpEQ(divisor, zero, "iszero");
    }
    auto zeroBB = llvm::BasicBlock::Create(
        ctx_, std::string(label_prefix) + ".zero", fn);
    auto okBB = llvm::BasicBlock::Create(
        ctx_, std::string(label_prefix) + ".ok", fn);
    builder_.CreateCondBr(isZero, zeroBB, okBB);
    builder_.SetInsertPoint(zeroBB);
    emit_div_zero();
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
  }



  // The declared return type's check, reported at the prologue-resolved
  // call site. Shared by `return v` and the body's tail value.
  void emit_return_type_check(llvm::Value* val) {
    emit_type_check_at(val, current_return_type_, "return value",
                       current_return_pos_.first, current_return_pos_.second);
  }


  // Core type-check emission with a runtime-computed report position —
  // the user-fn prologue passes the per-call position snapshot (see
  // culebra_runtime_param_pos) so a typed-param error points where the
  // interp binder points, not at the baked def position.
  void emit_type_check_at(llvm::Value* val, std::string_view expected_type,
                          std::string_view context, llvm::Value* lineV,
                          llvm::Value* colV) {
    if (expected_type.empty() || expected_type == "Any") return;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    auto expPtr = get_or_create_global_str(expected_type, ".tycheck.exp");
    auto ctxPtr = get_or_create_global_str(context, ".tycheck.ctx");
    emit_call(
        module_->getOrInsertFunction(
            rt::type_check, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy, ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {tag, data, expPtr, ctxPtr, lineV, colV});
  }

  // Cache of compile-time string constants to avoid duplicate globals per
  // emission.
  std::unordered_map<std::string, llvm::Constant*> global_str_cache_;
  llvm::Constant* get_or_create_global_str(std::string_view s,
                                           const char* name_hint) {
    std::string key(s);
    auto it = global_str_cache_.find(key);
    if (it != global_str_cache_.end()) return it->second;
    auto* g = builder_.CreateGlobalString(key, name_hint);
    global_str_cache_.emplace(std::move(key), g);
    return g;
  }

  // Constant char*-array .rodata (a param name/type table): null for an
  // empty list. Shared by emit_multifn_register, the JitFn arity prologue,
  // and the VM lowering's multifn/arity sites.
  llvm::Value* build_str_ptr_array(const std::vector<llvm::Constant*>& ptrs,
                                   const char* tag) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    if (ptrs.empty()) return llvm::ConstantPointerNull::get(ptrTy);
    auto arrayTy = llvm::ArrayType::get(ptrTy, ptrs.size());
    auto gv = new llvm::GlobalVariable(
        *module_, arrayTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayTy, ptrs), tag);
    return builder_.CreateBitCast(gv, ptrTy);
  }

  // The per-callsite Shape-cache pair every static-key-list construction
  // site's lowering needs (ObjectNewShaped, ValueBox): a private, lazily-
  // filled global pointer — a Shape* baked at AOT-compile time would be a
  // dangling address in the compiled binary's own, later, process, so
  // `_jit_resolve_cached_shape` (jit_runtime.h) resolves it on first
  // execution instead — plus the key C-string pointer array that runtime
  // half reads. `prefix` names the globals for readability in an IR dump;
  // `counter` disambiguates repeat callsites sharing one prefix.
  std::pair<llvm::GlobalVariable*, llvm::Value*> build_shape_cache_globals(
      const std::vector<const char*>& keys, const char* prefix,
      int& counter) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto* cacheGlobal = new llvm::GlobalVariable(
        *module_, ptrTy, /*isConstant=*/false,
        llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantPointerNull::get(ptrTy),
        std::string(prefix) + ".cache." + std::to_string(counter++));
    std::vector<llvm::Constant*> keyPtrs;
    keyPtrs.reserve(keys.size());
    std::string keyTag = std::string(prefix) + ".key";
    for (const char* k : keys)
      keyPtrs.push_back(get_or_create_global_str(k, keyTag.c_str()));
    std::string keysTag = std::string(prefix) + ".keys";
    auto keysArray = build_str_ptr_array(keyPtrs, keysTag.c_str());
    return {cacheGlobal, keysArray};
  }


  // --- Value helpers ---

  // `data` must already be i64; callers zext/bitcast/ptrtoint as needed.
  llvm::Value* make_value(uint8_t tag, llvm::Value* data) {
    // Base on a defined zero, not undef: SimplifyCFG can split the two
    // inserts across a branch so the tag-only intermediate ({tag, undef})
    // flows into an aggregate phi. SDAG materializes that undef harmlessly,
    // but FastISel turns it into garbage (a bogus tag -> "got ?"). A zero
    // base makes every intermediate {tag, 0} — defined on all backends.
    llvm::Value* val = llvm::ConstantAggregateZero::get(valueType_);
    // tag occupies the full i64 field (zero-extended); see JitValue.
    val = builder_.CreateInsertValue(val, builder_.getInt64(tag), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_nil() { return make_value(TAG_NIL, builder_.getInt64(0)); }

  // `self` handed to a call with no receiver — see TAG_NO_SELF.
  llvm::Value* make_no_self() {
    return make_value(TAG_NO_SELF, builder_.getInt64(0));
  }

  llvm::Value* make_bool(llvm::Value* b) {
    return make_value(TAG_BOOL, builder_.CreateZExt(b, builder_.getInt64Ty()));
  }

  llvm::Value* make_long(llvm::Value* l) { return make_value(TAG_LONG, l); }

  // Float is stored bit-cast into the i64 payload. The layout of
  // JitValue ({ i8 tag, i64 data }) is unchanged; only the
  // interpretation of `data` differs when tag is TAG_FLOAT.
  llvm::Value* make_float(llvm::Value* d) {
    return make_value(TAG_FLOAT,
                      builder_.CreateBitCast(d, builder_.getInt64Ty(),
                                             "f.bits"));
  }

  // A numeric operand's payload as a double: a Long widens, a Float
  // reinterprets — the promotion every both-numeric arm shares.
  llvm::Value* num_as_double(llvm::Value* isLong, llvm::Value* data,
                             const llvm::Twine& pfx) {
    auto doubleTy = builder_.getDoubleTy();
    return builder_.CreateSelect(
        isLong, builder_.CreateSIToFP(data, doubleTy, pfx + ".l2f"),
        builder_.CreateBitCast(data, doubleTy, pfx + ".f"), pfx + ".d");
  }

  llvm::Value* make_ptr_value(uint8_t tag, llvm::Value* ptr) {
    return make_value(tag,
                      builder_.CreatePtrToInt(ptr, builder_.getInt64Ty()));
  }

  // Zero value for a declared field type (`x: T` with no initializer),
  // single-sourced with the interp's zero_value_for_type through
  // culebra::zero_kind_for_type.
  Owned emit_zero_value(std::string_view type) {
    switch (culebra::zero_kind_for_type(type)) {
      case culebra::ZeroKind::Float:
        return own(make_float(
            llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0)));
      case culebra::ZeroKind::Bool:
        return own(make_bool(builder_.getInt1(false)));
      case culebra::ZeroKind::String:
        return own(make_string(emit_str_literal("")));
      case culebra::ZeroKind::Long:
        return own(make_long(builder_.getInt64(0)));
      case culebra::ZeroKind::Nil:
        break;
    }
    return own(make_nil());
  }

  llvm::Value* make_func(llvm::Value* ptr) { return make_ptr_value(TAG_FUNC, ptr); }
  llvm::Value* make_string(llvm::Value* ptr) { return make_ptr_value(TAG_STRING, ptr); }
  llvm::Value* make_stringview(llvm::Value* ptr) { return make_ptr_value(TAG_STRINGVIEW, ptr); }
  llvm::Value* make_array(llvm::Value* ptr) { return make_ptr_value(TAG_ARRAY, ptr); }
  llvm::Value* make_tuple(llvm::Value* ptr) { return make_ptr_value(TAG_TUPLE, ptr); }
  llvm::Value* make_set(llvm::Value* ptr) { return make_ptr_value(TAG_SET, ptr); }
  llvm::Value* make_object(llvm::Value* ptr) { return make_ptr_value(TAG_OBJECT, ptr); }
  llvm::Value* make_tensor(llvm::Value* ptr) { return make_ptr_value(TAG_TENSOR, ptr); }

  llvm::Value* extract_tag(llvm::Value* v) {
    // The tag field is i64 (see JitValue); truncate to i8 so all downstream
    // tag comparisons (getInt8(TAG_*)) and runtime calls keep their i8 ABI.
    // For a value built by the JIT the high bytes are 0; for one returned by a
    // C runtime fn they are padding — the truncation discards them either way.
    return builder_.CreateTrunc(builder_.CreateExtractValue(v, {0}),
                                builder_.getInt8Ty(), "tag");
  }

  // Widen an i8 tag to the i64 Value tag field for `insertvalue ..., {0}`.
  // Tag logic stays i8 throughout; only the field write needs the full width.
  llvm::Value* i64tag(llvm::Value* tag) {
    return tag->getType()->isIntegerTy(64)
               ? tag
               : builder_.CreateZExt(tag, builder_.getInt64Ty(), "tag.w");
  }

  // Build a Value from a runtime (tag, data) pair — the i8-tag twin of the
  // compile-time make_value above. The single place that widens the tag to the
  // i64 field, so no construction site can forget it (and reopen the
  // native-call tag bug — see JitValue). `data` must already be i64.
  llvm::Value* make_value(llvm::Value* tag, llvm::Value* data) {
    // Zero base, not undef — see the uint8_t overload above (FastISel
    // garbages a {tag, undef} intermediate that SimplifyCFG sinks into a phi).
    llvm::Value* val = llvm::ConstantAggregateZero::get(valueType_);
    val = builder_.CreateInsertValue(val, i64tag(tag), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* extract_data(llvm::Value* v) {
    return builder_.CreateExtractValue(v, {1}, "data");
  }

  // The LLVM half of the JitFn ABI (see the `JitFn` typedef): every closure
  // body, ctor, defer thunk, and indirect call site shares this signature —
  // void(ret:ptr, cls:ptr, self_tag:i8, self_data:i64, n_args:i64, args:ptr).
  // The result is written through the leading out-pointer and the receiver
  // `self` crosses as two scalars — no by-value 16-byte aggregate crosses the
  // boundary in either direction, so the Win64 C ABI and the raw-IR lowering
  // agree (a by-value aggregate return would be sret on Win64 but two-register
  // in raw IR, mis-placing args). One place so an ABI change is a single edit.
  llvm::FunctionType* jitFnCalleeType() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return llvm::FunctionType::get(
        builder_.getVoidTy(),
        {ptrTy, ptrTy, builder_.getInt8Ty(), builder_.getInt64Ty(),
         builder_.getInt64Ty(), ptrTy},
        false);
  }


  // value_to_bool: returns i1. Bool is the monomorphic hot case (comparisons,
  // `&&`/`||`, loop guards) so it is inlined; Long/Float/error funnel through
  // culebra_runtime_to_bool_borrow, which keeps the strict-truthiness
  // semantics (Nil and other tags raise the same TypeError as the
  // interpreter) in one place. The operand stays frame-owned (borrow
  // contract): a region's release ladder owns the throw path.
  llvm::Value* value_to_bool(llvm::Value* v) {
    UnwindCovered cover(this, {v});
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto fastBB = llvm::BasicBlock::Create(ctx_, "tobool.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "tobool.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "tobool.merge", fn);

    auto isBool = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_BOOL));
    builder_.CreateCondBr(isBool, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    auto fastVal = builder_.CreateICmpNE(data, builder_.getInt64(0), "bool.nz");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto slowVal = emit_call(
        module_->getOrInsertFunction(
            rt::to_bool_borrow,
            builder_.getInt1Ty(), i8Ty, i64Ty, i64Ty, i64Ty),
        {tag, data, current_line_val(), current_column_val()});
    // emit_call may have split the block (invoke continuation); the phi's
    // incoming edge is whatever block now holds the branch.
    auto slowPred = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "tobool");
    phi->addIncoming(fastVal, fastBB);
    phi->addIncoming(slowVal, slowPred);
    return phi;
  }

  // value_to_long: returns i64, calls type_error if not Long
  llvm::Value* value_to_long(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "tolong.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tolong.error", fn);

    auto isLong = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
    builder_.CreateCondBr(isLong, okBB, errorBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error_typed("Long", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    return data;
  }


  // --- Builtin-name predicate (fed to FnAnalysis, fn_analysis.h) ---

  // --- Runtime function declarations ---

  void declare_runtime_functions() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    module_->getOrInsertFunction(rt::inspect, builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::type_error,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::type_error_typed,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty());
    module_->getOrInsertFunction(rt::class_parameters_walk, valueType_, ptrTy);
    // multifn_register_and_install:
    //   JitClosure* (const char* name, JitClosure* body,
    //                const char** param_types, int64_t n_param_types,
    //                int64_t variadic, int64_t min_arity,
    //                const char** param_names)
    module_->getOrInsertFunction(rt::multifn_register_and_install,
                                 ptrTy, ptrTy, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 ptrTy);
    // User-level throw: stashes tag+data in globals and raises a
    // C++ exception; try/catch landingpads read the globals back.
    module_->getOrInsertFunction(rt::throw_,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::rethrow,
                                 builder_.getVoidTy());
    // Defer: (push takes tag+data of a closure Value)
    module_->getOrInsertFunction(rt::defer_mark,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_push,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_run_to,
                                 builder_.getVoidTy(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::set_drop_suppressed,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty());
    // Loop safepoint slow path (throws Interrupted on Ctrl+C).
    module_->getOrInsertFunction(rt::safepoint, builder_.getVoidTy());
    module_->getOrInsertFunction(
        rt::type_check, builder_.getVoidTy(),
        builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy, ptrTy,
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::div_zero,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // Core, not Tensor-specific, despite the name: the built-in `Eq` trait's
    // `neq` default (shared.h's builtin_traits_preamble, compiled for every
    // program) reads `!self.eq(other)`, and the front end resolves a call
    // named `eq` to whichever BMethSpec is registered under that name --
    // which is Tensor's, the only one there is. So every program's compiled
    // code contains this call, gated at runtime by the receiver's tag,
    // whether or not that program ever mentions Tensor. Declaring it only
    // from Tensor's own declare_runtime (stdlib_jit.h) left a host that
    // skips install_jit_stdlib() with an undeclared function and a crash in
    // JIT::emit_call, not a script-level error -- getFunction() found
    // nothing to call.
    module_->getOrInsertFunction(rt::tensor_binop, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::debugger_break,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_to_display, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_concat, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_eq,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_cmp,
                                 builder_.getInt32Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_new, ptrTy);
    module_->getOrInsertFunction(rt::array_new_reserved, ptrTy,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_push,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_resize, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_get,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(), ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_set, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(
        rt::array_set_or_push, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_new, ptrTy);
    module_->getOrInsertFunction(
        rt::object_set, builder_.getVoidTy(), ptrTy, ptrTy,
        builder_.getInt1Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getInt1Ty());
    module_->getOrInsertFunction(rt::object_get,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::object_has,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_has_or_trait_default,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_retain,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_release,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::cell_retain,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::cell_release,
                                 builder_.getVoidTy(), ptrTy);

    // Built-in type methods.
    module_->getOrInsertFunction(rt::str_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::str_upper, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_lower, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_capitalize, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_repeat, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_truncate, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_lines, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_count, builder_.getInt64Ty(), ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_trim, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_tr, ptrTy, ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_trim_start, ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_trim_end, ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_split, ptrTy, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt1Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_split_whitespace, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_title, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_normalize, ptrTy, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_reverse, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_eq_ignore_case, builder_.getInt1Ty(),
                                 ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_index_of, builder_.getInt64Ty(), ptrTy,
                                 ptrTy, builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_last_index_of, builder_.getInt64Ty(),
                                 ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_strip_prefix, ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_strip_suffix, ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_is_class, builder_.getInt1Ty(), ptrTy,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_view, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_slice_view, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::strlike_to_cstr, ptrTy,
                                 builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_contains,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_starts_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_ends_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_pop,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_slice2, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_join, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::array_contains,
                                 builder_.getInt1Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_index_of,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_reverse,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::array_insert, builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_remove_at, builder_.getVoidTy(),
                                 ptrTy, builder_.getInt64Ty(), ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_keys, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_remove,
                                 builder_.getVoidTy(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::is_shared_val, builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::is_namespace, builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    // Higher-order array helpers (§17.2): (arr, fn_tag, fn_data, line, col).
    module_->getOrInsertFunction(rt::array_map, ptrTy, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_filter, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_for_each, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_reduce, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt8Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        ptrTy, ptrTy);
    // find (arr, fn_tag, fn_data, line, col, out_tag, out_data)
    module_->getOrInsertFunction(
        rt::array_find, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), ptrTy, ptrTy);
    // any/all (arr, fn_tag, fn_data, line, col) -> i64 (0/1)
    module_->getOrInsertFunction(rt::array_any,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_all,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // flat_map (arr, fn_tag, fn_data, line, col) -> ptr
    module_->getOrInsertFunction(rt::array_flat_map, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // check_callback_type (fn_tag, fn_data, param_name) -> void
    module_->getOrInsertFunction(
        rt::check_callback_type, builder_.getVoidTy(), builder_.getInt8Ty(),
        builder_.getInt64Ty(), ptrTy);
    // sort_by (arr, fn_tag, fn_data, reverse, line, col)
    module_->getOrInsertFunction(
        rt::array_sort_by, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt1Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    // sorted_by — same args, returns a new array (ptr)
    module_->getOrInsertFunction(
        rt::array_sorted_by, ptrTy, ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt1Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    // sort (arr, reverse, line, col) -> void
    module_->getOrInsertFunction(
        rt::array_sort, builder_.getVoidTy(), ptrTy, builder_.getInt1Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    // sorted — same args, returns a new array (ptr)
    module_->getOrInsertFunction(
        rt::array_sorted, ptrTy, ptrTy, builder_.getInt1Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    // sum/product/min/max: (arr, line, col) -> %Value. The result tag is
    // data-dependent (a Float element promotes sum/product; min/max return
    // the winning element), so these cannot return a bare i64.
    module_->getOrInsertFunction(rt::array_sum, valueType_,
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_product, valueType_,
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_min, valueType_,
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_max, valueType_,
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // min_by/max_by: (arr, ft, fd, line, col) -> %Value
    module_->getOrInsertFunction(rt::array_min_by, valueType_, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_max_by, valueType_, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // to_set/to_object/group_by/partition/unzip return a fresh Set / Object /
    // Object / Tuple / Tuple pointer.
    module_->getOrInsertFunction(rt::array_to_set, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_to_object, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_group_by, ptrTy, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_partition, ptrTy, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_unzip, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());

    // Core globals: range/iota are language-level integer iterator/array
    // factories (`for i in range(n) {}`). math_range backs the `..`
    // range syntax too, so its runtime decl must live in the core.
    module_->getOrInsertFunction(rt::iota, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::math_range, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // grid: also a core global (see try_compile_core_global) — two Range
    // value pairs (tag, data) + line/col for the "expects Range" /
    // "unbounded range" / "step must not be zero" diagnostics.
    module_->getOrInsertFunction(rt::grid_new, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::check_pos_count, builder_.getVoidTy(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::set_call_site, builder_.getVoidTy(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::set_op_pos, builder_.getVoidTy(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::set_callback_arg_site,
                                 builder_.getVoidTy(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::set_arg_pos, builder_.getVoidTy(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());

    // RC primitives never raise a culebra exception, so mark them nounwind:
    // emit_call then drops the invoke/landing-pad edge at every RC site.
    for (auto name : {rt::value_retain, rt::value_release, rt::cell_retain,
                      rt::cell_release}) {
      if (auto* f = module_->getFunction(name)) f->setDoesNotThrow();
    }

    auto& h = current_hooks();
    if (h.declare_runtime) h.declare_runtime(*this);
  }

  // Emit a String literal as a .rodata length-prefixed buffer matching the
  // runtime _str_alloc layout: { i64 len, [N+1 x i8] bytes-with-nul }. The
  // returned pointer targets the bytes field, so TAG_STRING data points at
  // the bytes (len at data[-8]) exactly like a heap string. StringRef
  // carries the length, so embedded NUL bytes survive into .rodata.
  llvm::Value* emit_str_literal(std::string_view bytes) {
    auto i64Ty = builder_.getInt64Ty();
    llvm::StringRef raw(bytes.data(), bytes.size());
    auto* bytesConst =
        llvm::ConstantDataArray::getString(ctx_, raw, /*AddNull=*/true);
    auto* structTy = llvm::StructType::get(ctx_, {i64Ty, bytesConst->getType()});
    auto* init = llvm::ConstantStruct::get(
        structTy, {llvm::ConstantInt::get(i64Ty, bytes.size()), bytesConst});
    auto* g = new llvm::GlobalVariable(*module_, structTy, /*isConstant=*/true,
                                       llvm::GlobalValue::PrivateLinkage, init,
                                       ".str");
    g->setAlignment(llvm::Align(8));
    g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    llvm::Value* idx[] = {builder_.getInt32(0), builder_.getInt32(1),
                          builder_.getInt32(0)};
    return builder_.CreateInBoundsGEP(structTy, g, idx, ".str.data");
  }



  llvm::Value* emit_object_has(llvm::Value* objPtr, llvm::Value* keyPtr,
                               const llvm::Twine& name = "has.prop") {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return emit_call(
        module_->getOrInsertFunction(rt::object_has, builder_.getInt1Ty(),
                                     ptrTy, ptrTy),
        {objPtr, keyPtr}, name);
  }

  // The property arm's test in both UFCS gates: an Object receiver takes it
  // when it resolves `method` itself — own slot, class meta, or a trait
  // default it inherits (the wider question only this gate asks; the
  // iterator-protocol and dispose probes read a concrete slot, so they stay on
  // emit_object_has) — or when it is a closed namespace, which resolves the
  // name either way (see emit_object_is_namespace).
  llvm::Value* emit_receiver_resolves_method(llvm::Value* receiver,
                                             const std::string& method) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto data = extract_data(receiver);
    auto objPtr = builder_.CreateIntToPtr(data, ptrTy);
    auto keyPtr = get_or_create_global_str(method, ".mkey");
    auto has = emit_call(
        module_->getOrInsertFunction(rt::object_has_or_trait_default,
                                     builder_.getInt1Ty(), ptrTy, ptrTy),
        {objPtr, keyPtr}, "has.prop.trait");
    return builder_.CreateOr(has, emit_object_is_namespace(data));
  }

  // Materialize a builtin global (`IO`, `Time`, `inspect`, `assert_eq`) as a
  // value: one +1 reference to whatever the runtime resolver hands back for
  // that name on this Runtime. Callers own the result.
  llvm::Value* emit_builtin_var_get(const std::string& name) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto tagSlot = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "ns.tag");
    auto dataSlot =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ns.data");
    auto namePtr = get_or_create_global_str(name, ".ns.name");
    emit_call(module_->getOrInsertFunction(rt::namespace_get,
                                           builder_.getVoidTy(), ptrTy, ptrTy,
                                           ptrTy),
              {namePtr, tagSlot, dataSlot});
    auto tag = builder_.CreateLoad(builder_.getInt8Ty(), tagSlot, "ns.tag.v");
    auto data =
        builder_.CreateLoad(builder_.getInt64Ty(), dataSlot, "ns.data.v");
    return make_value(tag, data);
  }









  // Build a range value via culebra_runtime_make_range. A null start/end is
  // an open endpoint (passed as has_*=false); a null step defaults to 1.
  llvm::Value* emit_make_range(llvm::Value* startV, llvm::Value* endV,
                               bool inclusive, llvm::Value* stepV = nullptr) {
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto obj = emit_call(
        module_->getOrInsertFunction(
            rt::make_range, llvm::PointerType::get(ctx_, 0), i8Ty, i64Ty,
            i8Ty, i64Ty, i8Ty, i64Ty),
        {builder_.getInt8(startV ? 1 : 0),
         startV ? startV : builder_.getInt64(0),
         builder_.getInt8(endV ? 1 : 0), endV ? endV : builder_.getInt64(0),
         builder_.getInt8(inclusive ? 1 : 0),
         stepV ? stepV : builder_.getInt64(1)});
    return make_object(obj);
  }

  // `let`-less destructure (parallel assignment): leaves assign to
  // existing slots instead of declaring. Set for one pattern tree by
  // compile_destructure_assign; defaults to declare for match arms.
  bool pattern_declare_ = true;

  // False while emit_pattern walks a tree for its answer only: the tests are
  // emitted, the bindings (and the retains that feed them) are not.
  // compile_destructure_assign emits both walks so a mismatch writes nothing
  // — see the interp's pattern_bind_ for the rule. Match arms, for-in
  // bindings and destructuring parameters bind into a scope that dies with a
  // failed match, so they emit the binding walk alone.
  bool pattern_bind_ = true;


  // --- Arithmetic ---
  //
  // Float-aware binary arithmetic uses a 3-way dispatch:
  //   1. If both operands are Long, take the inline i64 fast path
  //      (CreateAdd / CreateMul / CreateSDiv / CreateSRem).
  //   2. Otherwise, call a runtime helper that promotes each operand
  //      to double and runs the operation.
  // The branch predictor learns Long-only code paths immediately, so
  // scripts that stay on Long pay essentially no runtime cost.

  // If both operands are TAG_LONG, run `long_path` inline on their i64
  // payloads; otherwise invoke the runtime helper identified by
  // `rt_name`. Kept as a template so the caller's lambda is inlined
  // (no std::function heap allocation per arithmetic AST node).
  //
  // Three branches:
  //   - bothLong  → `long_path(ldata, rdata)` returning a JitValue
  //   - bothNum   → `float_path(lDouble, rDouble)` returning a JitValue
  //                 (typically `make_float(builder_.CreateFAdd(...))`)
  //   - otherwise → `rt_name` runtime call (Object special-method dispatch)
  template <class LongPath, class FloatPath>
  llvm::Value* emit_binop_dispatch(llvm::Value* lhs, llvm::Value* rhs,
                                   const char* rt_name,
                                   LongPath long_path,
                                   FloatPath float_path) {
    // The operands stay frame-owned (the `_borrow` helper contract): a
    // region's release ladder is their one releaser on the throw path, so
    // the unwind-temp window must not spill them (the ASan-confirmed
    // overlap trap).
    UnwindCovered cover(this, {lhs, rhs});
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "binop.int", fn);
    auto checkNumBB =
        llvm::BasicBlock::Create(ctx_, "binop.check_num", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "binop.float", fn);
    auto numBB = llvm::BasicBlock::Create(ctx_, "binop.num", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "binop.merge", fn);

    auto ltag = extract_tag(lhs);
    auto rtag = extract_tag(rhs);
    auto ldata = extract_data(lhs);
    auto rdata = extract_data(rhs);
    auto longTag = builder_.getInt8(TAG_LONG);
    auto floatTag = builder_.getInt8(TAG_FLOAT);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "both.long");
    builder_.CreateCondBr(bothLong, intBB, checkNumBB);

    OwnedPhi merge(this, "binop.r");
    builder_.SetInsertPoint(intBB);
    merge.add_incoming(long_path(ldata, rdata));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(checkNumBB);
    auto lIsFloat = builder_.CreateICmpEQ(ltag, floatTag);
    auto rIsFloat = builder_.CreateICmpEQ(rtag, floatTag);
    auto lIsNum = builder_.CreateOr(lIsLong, lIsFloat);
    auto rIsNum = builder_.CreateOr(rIsLong, rIsFloat);
    auto bothNum = builder_.CreateAnd(lIsNum, rIsNum, "both.num");
    builder_.CreateCondBr(bothNum, floatBB, numBB);

    builder_.SetInsertPoint(floatBB);
    merge.add_incoming(float_path(num_as_double(lIsLong, ldata, "l"),
                                  num_as_double(rIsLong, rdata, "r")));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(numBB);
    // Slow path: the `_borrow` helper leaves the operands' refs alone on
    // every throw — a user `__add__` body's and a builtin TypeError's
    // (`1 + "a"`) alike.
    merge.add_incoming(emit_value_call(
        module_->getOrInsertFunction(rt_name, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {ltag, ldata, rtag, rdata,
         current_line_val(), current_column_val()}, "num.op"));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    return merge.finish(mergeBB).consume();
  }

  // Single arith step for compound-assignment lowering. `inplace=true`
  // swaps the runtime helper for the Tensor-aware in-place variant;
  // Long/Float fast paths are unchanged (those are value types, so
  // in-place doesn't apply).
  llvm::Value* emit_arith_step(llvm::Value* lhs, llvm::Value* rhs,
                               std::string_view op, bool inplace = false) {
    // Frame-owned operands (same as emit_binop_dispatch, which covers again
    // for its own callers): matmul/pow below call their helpers directly.
    UnwindCovered cover(this, {lhs, rhs});
    // One selection point for the (op, in-place) → helper-name mapping:
    // the `_borrow` helper (the lowering's operand contract — registers
    // are frame-owned, a region's release ladder owns the throw path), or
    // its Tensor-aware in-place twin. A mispaired name here compiles fine
    // and only shows as a runtime leak/double-free, so every op routes
    // through this one lambda.
    auto pick = [&](const char* plain, const char* inpl) {
      return inplace ? inpl : plain;
    };
    if (op == "@") {
      // No in-place matmul (output shape differs from lhs).
      return emit_value_call(
          module_->getOrInsertFunction(
              rt::num_matmul_borrow,
              valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {extract_tag(lhs), extract_data(lhs),
           extract_tag(rhs), extract_data(rhs),
           current_line_val(), current_column_val()}, "cmp.matmul");
    }
    if (op == "**") {
      const char* rt_name =
          pick(rt::num_pow_borrow, rt::num_inplace_pow_borrow);
      return emit_value_call(
          module_->getOrInsertFunction(
              rt_name, valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {extract_tag(lhs), extract_data(lhs),
           extract_tag(rhs), extract_data(rhs),
           current_line_val(), current_column_val()}, "cmp.pow");
    }
    char ope = op[0];
    const char* rt_name = nullptr;
    switch (ope) {
      case '+':
        rt_name = pick(rt::num_add_borrow, rt::num_inplace_add_borrow);
        break;
      case '-':
        rt_name = pick(rt::num_sub_borrow, rt::num_inplace_sub_borrow);
        break;
      case '*':
        rt_name = pick(rt::num_mul_borrow, rt::num_inplace_mul_borrow);
        break;
      case '/':
        rt_name = pick(rt::num_div_borrow, rt::num_inplace_div_borrow);
        break;
      case '%':  // mod has no Tensor in-place
        rt_name = rt::num_mod_borrow;
        break;
      default:
        throw std::runtime_error("invalid compound assignment operator");
    }
    return emit_binop_dispatch(
        lhs, rhs, rt_name,
        [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
          switch (ope) {
            case '+': return make_long(builder_.CreateAdd(ld, rd, "add"));
            case '-': return make_long(builder_.CreateSub(ld, rd, "sub"));
            case '*': return make_long(builder_.CreateMul(ld, rd, "mul"));
            case '/':
              emit_div_zero_guard(rd, "div");
              return make_long(emit_wrapping_sdiv(ld, rd, /*rem=*/false));
            case '%':
              emit_div_zero_guard(rd, "mod");
              return make_long(emit_wrapping_sdiv(ld, rd, /*rem=*/true));
          }
          return nullptr;
        },
        [&](llvm::Value* lD, llvm::Value* rD) -> llvm::Value* {
          switch (ope) {
            case '+': return make_float(builder_.CreateFAdd(lD, rD, "fadd"));
            case '-': return make_float(builder_.CreateFSub(lD, rD, "fsub"));
            case '*': return make_float(builder_.CreateFMul(lD, rD, "fmul"));
            case '/':
              emit_div_zero_guard(rD, "fdiv");
              return make_float(builder_.CreateFDiv(lD, rD, "fdiv"));
            case '%':
              emit_div_zero_guard(rD, "fmod");
              return make_float(builder_.CreateFRem(lD, rD, "fmod"));
          }
          return nullptr;
        });
  }

  // `to_float(x)`: the two numeric tags inline (a Float passes through, a
  // Long widens), everything else — a String to parse, anything else to
  // reject — through to_float_any, so the parse and the TypeError stay the
  // helper's own. Mirrors the executor's Op::ToFloat arm.
  llvm::Value* emit_to_float_step(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);
    auto isLong = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
    auto isNum = builder_.CreateOr(
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FLOAT)), isLong);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto numBB = llvm::BasicBlock::Create(ctx_, "tofloat.num", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "tofloat.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "tofloat.merge", fn);
    builder_.CreateCondBr(isNum, numBB, slowBB);

    OwnedPhi merge(this, "tofloat.r");
    builder_.SetInsertPoint(numBB);
    auto widened = builder_.CreateBitCast(
        builder_.CreateSIToFP(data, builder_.getDoubleTy(), "tofloat.w"),
        builder_.getInt64Ty());
    merge.add_incoming(make_value(
        TAG_FLOAT,
        builder_.CreateSelect(isLong, widened, data, "tofloat.d")));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    // The operand stays frame-owned on the throw, as emit_neg_step's does.
    UnwindCovered cover(this, {v});
    merge.add_incoming(emit_value_call(
        module_->getOrInsertFunction(
            rt::to_float_any, valueType_, builder_.getInt8Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {tag, data, current_line_val(), current_column_val()},
        "tofloat.any"));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    return merge.finish(mergeBB).consume();
  }

  // Op::NsCall — the namespace functions vm.h compiles a direct `Math.f(x)`
  // to (nsfn_specs). Where the answer is a double function of numeric
  // operands (the unary Float family, abs, atan2), a numeric tag takes the
  // libm call inline — through the LLVM intrinsic that stands for it where
  // one exists, which lowers to the very call the helper makes — so a known
  // tag lets SCCP fold the dispatch away. Everything else, and every other
  // function, is the runtime's one dispatch over the same helpers
  // (culebra_runtime_ns_call), which also owns every error. `slab` is the
  // entry-block scratch that arm hands the arguments over in.
  llvm::Value* emit_ns_call(NsFn id, const std::vector<llvm::Value*>& args,
                            llvm::Value* slab, int64_t line, int64_t col) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto doubleTy = builder_.getDoubleTy();
    auto i64Ty = builder_.getInt64Ty();
    auto slowBB = llvm::BasicBlock::Create(ctx_, "ns.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ns.merge", fn);
    OwnedPhi merge(this, "ns.r");

    // One tag and payload per operand, shared by every test below.
    llvm::SmallVector<llvm::Value*, 3> tag, data;
    for (auto* v : args) {
      tag.push_back(extract_tag(v));
      data.push_back(extract_data(v));
    }
    auto is_long = [&](size_t k) {
      return builder_.CreateICmpEQ(tag[k], builder_.getInt8(TAG_LONG));
    };
    auto is_num = [&](size_t k) {
      return builder_.CreateOr(
          is_long(k),
          builder_.CreateICmpEQ(tag[k], builder_.getInt8(TAG_FLOAT)));
    };
    auto as_double = [&](size_t k) {
      return num_as_double(is_long(k), data[k], "ns");
    };
    auto libm = [&](const char* name, llvm::ArrayRef<llvm::Value*> xs) {
      std::vector<llvm::Type*> tys(xs.size(), doubleTy);
      return builder_.CreateCall(
          module_->getOrInsertFunction(
              name, llvm::FunctionType::get(doubleTy, tys, false)),
          xs, name);
    };
    // Every arm ends by handing its result to the merge.
    auto done = [&](llvm::Value* r) {
      merge.add_incoming(r);
      builder_.CreateBr(mergeBB);
    };

    switch (id) {
      case NsFn::MathLog:
      case NsFn::MathExp:
      case NsFn::MathSqrt:
      case NsFn::MathSin:
      case NsFn::MathCos:
      case NsFn::MathTan:
      case NsFn::MathAsin:
      case NsFn::MathAcos:
      case NsFn::MathAtan: {
        auto numBB = llvm::BasicBlock::Create(ctx_, "ns.num", fn);
        builder_.CreateCondBr(is_num(0), numBB, slowBB);
        builder_.SetInsertPoint(numBB);
        auto d = as_double(0);
        llvm::Value* r = nullptr;
        switch (id) {
          case NsFn::MathLog:
            r = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::log, d);
            break;
          case NsFn::MathExp:
            r = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::exp, d);
            break;
          case NsFn::MathSqrt:
            r = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, d);
            break;
          case NsFn::MathSin:
            r = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sin, d);
            break;
          case NsFn::MathCos:
            r = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::cos, d);
            break;
          case NsFn::MathTan: r = libm("tan", {d}); break;
          case NsFn::MathAsin: r = libm("asin", {d}); break;
          case NsFn::MathAcos: r = libm("acos", {d}); break;
          case NsFn::MathAtan: r = libm("atan", {d}); break;
          default: assert(false && "not a unary Float function"); break;
        }
        done(make_float(r));
        break;
      }
      case NsFn::MathAtan2: {
        auto numBB = llvm::BasicBlock::Create(ctx_, "ns.num", fn);
        builder_.CreateCondBr(builder_.CreateAnd(is_num(0), is_num(1)), numBB,
                              slowBB);
        builder_.SetInsertPoint(numBB);
        done(make_float(libm("atan2", {as_double(0), as_double(1)})));
        break;
      }
      case NsFn::MathAbs: {
        auto intBB = llvm::BasicBlock::Create(ctx_, "ns.int", fn);
        auto checkBB = llvm::BasicBlock::Create(ctx_, "ns.check", fn);
        auto floatBB = llvm::BasicBlock::Create(ctx_, "ns.float", fn);
        builder_.CreateCondBr(is_long(0), intBB, checkBB);
        builder_.SetInsertPoint(intBB);
        // Wrapping, as the helper's `x < 0 ? -x : x` is on INT64_MIN.
        auto zero = builder_.getInt64(0);
        done(make_long(builder_.CreateSelect(
            builder_.CreateICmpSLT(data[0], zero),
            builder_.CreateSub(zero, data[0]), data[0])));
        builder_.SetInsertPoint(checkBB);
        builder_.CreateCondBr(
            builder_.CreateICmpEQ(tag[0], builder_.getInt8(TAG_FLOAT)), floatBB,
            slowBB);
        builder_.SetInsertPoint(floatBB);
        done(make_float(builder_.CreateUnaryIntrinsic(
            llvm::Intrinsic::fabs, builder_.CreateBitCast(data[0], doubleTy))));
        break;
      }
      default:
        builder_.CreateBr(slowBB);
        break;
    }

    builder_.SetInsertPoint(slowBB);
    if (!args.empty()) {
      auto arrTy = llvm::ArrayType::get(valueType_, args.size());
      for (size_t k = 0; k < args.size(); ++k)
        builder_.CreateStore(args[k],
                             builder_.CreateConstGEP2_64(arrTy, slab, 0, k));
    }
    // The operands stay frame-owned on the helper's throw (borrow contract),
    // as emit_to_float_step's does.
    UnwindCovered cover(this, args);
    done(emit_value_call(
        module_->getOrInsertFunction(
            rt::ns_call, valueType_, builder_.getInt32Ty(),
            llvm::PointerType::get(ctx_, 0), i64Ty, i64Ty, i64Ty),
        {builder_.getInt32(static_cast<int32_t>(id)), slab,
         builder_.getInt64(static_cast<int64_t>(args.size())),
         builder_.getInt64(line), builder_.getInt64(col)},
        "ns.call"));

    builder_.SetInsertPoint(mergeBB);
    return merge.finish(mergeBB).consume();
  }

  // Value-level unary-minus dispatch: Long negates inline, everything else
  // goes to num_neg (the `_borrow` twin under the VM contract). One dispatch
  // definition, two consumers — compile_unary_minus and the VM lowering's
  // Neg (the emit_arith_step precedent).
  llvm::Value* emit_neg_step(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "neg.int", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "neg.float", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "neg.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "neg.merge", fn);
    auto sw = builder_.CreateSwitch(tag, slowBB, 2);
    sw->addCase(builder_.getInt8(TAG_LONG), intBB);
    sw->addCase(builder_.getInt8(TAG_FLOAT), floatBB);

    OwnedPhi merge(this, "neg.r");
    builder_.SetInsertPoint(intBB);
    merge.add_incoming(make_long(builder_.CreateNeg(extract_data(v), "neg")));
    builder_.CreateBr(mergeBB);

    // A known Float tag folds this arm to a bare `fneg` (the comparison and
    // binop Float arms' precedent); the helper below stays for the rest.
    builder_.SetInsertPoint(floatBB);
    merge.add_incoming(make_float(builder_.CreateFNeg(
        builder_.CreateBitCast(extract_data(v), builder_.getDoubleTy()),
        "fneg")));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    // The operand stays frame-owned (borrow contract) on every throw — the
    // helper's direct type error and a user `__neg__` alike (see
    // emit_binop_dispatch).
    UnwindCovered cover(this, {v});
    merge.add_incoming(emit_value_call(
        module_->getOrInsertFunction(
            rt::num_neg_borrow, valueType_,
            builder_.getInt8Ty(), builder_.getInt64Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {extract_tag(v), extract_data(v),
         current_line_val(), current_column_val()}, "neg.num"));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    return merge.finish(mergeBB).consume();
  }

  // `~v` — bitwise complement, Long-only (else the typed TypeError). One
  // definition, two consumers — compile_unary_bnot and the VM lowering's
  // BitNot (the emit_neg_step precedent).
  llvm::Value* emit_bnot_step(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto isLong = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "bnot.int", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "bnot.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "bnot.merge", fn);
    builder_.CreateCondBr(isLong, intBB, errBB);

    OwnedPhi merge(this, "bnot.r");
    builder_.SetInsertPoint(intBB);
    merge.add_incoming(
        make_long(builder_.CreateNot(extract_data(v), "bnot")));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error_typed("Long", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    return merge.finish(mergeBB).consume();
  }

  // One bitwise / shift step (`|` `^` `&` `<<` `>>`), Long-only — a non-Long
  // operand raises the typed TypeError, reporting the lhs first. One
  // definition, two consumers — compile_bitwise and the VM lowering's Bit*
  // ops (the emit_arith_step precedent).
  llvm::Value* emit_bitwise_step(llvm::Value* lhs, llvm::Value* rhs,
                                 std::string_view op) {
    auto longTag = builder_.getInt8(TAG_LONG);
    auto ltag = extract_tag(lhs);
    auto rtag = extract_tag(rhs);
    auto bothLong = builder_.CreateAnd(
        builder_.CreateICmpEQ(ltag, longTag),
        builder_.CreateICmpEQ(rtag, longTag), "bit.bothlong");
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "bit.int", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "bit.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "bit.merge", fn);
    builder_.CreateCondBr(bothLong, intBB, errBB);

    builder_.SetInsertPoint(intBB);
    auto ld = extract_data(lhs);
    auto rd = extract_data(rhs);
    llvm::Value* r;
    if (op == "|") r = builder_.CreateOr(ld, rd, "bor");
    else if (op == "^") r = builder_.CreateXor(ld, rd, "bxor");
    else if (op == "&") r = builder_.CreateAnd(ld, rd, "band");
    else {
      // Mask the shift count to the low 6 bits (operand width) so a count
      // >= 64 is well-defined instead of LLVM `poison` — matching interp's
      // `rhs & 63` (hardware/Java/C# rule). `1 << 64 == 1`.
      auto sh = builder_.CreateAnd(rd, builder_.getInt64(63), "shcnt");
      if (op == "<<") r = builder_.CreateShl(ld, sh, "shl");
      else r = builder_.CreateAShr(ld, sh, "ashr");  // ">>" (signed)
    }
    OwnedPhi step(this, "bit.r");
    step.add_incoming(make_long(r));
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    // Report on whichever operand isn't a Long.
    auto lNotLong = builder_.CreateICmpNE(ltag, longTag);
    auto badTag = builder_.CreateSelect(lNotLong, ltag, rtag);
    emit_type_error_typed("Long", badTag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    return step.finish(mergeBB).consume();
  }

  // --- Exponentiation ---



  // --- Comparison ---

  // Compile one comparison `lhs OPE rhs` to an i1 (boolean, pre-make_bool).
  // The same three-way dispatch as emit_binop_dispatch: both-Long is an
  // icmp, both-numeric promotes to double and is an fcmp, and everything
  // else — String, Nil, a user `__eq__`/`__lt__`/`cmp`, the type errors on
  // reference types — goes through the runtime helper, which owns those
  // semantics. The numeric arms are exactly the helper's own (_culebra_value_
  // equal / _culebra_value_ord compare promoted doubles; an ordered fcmp is
  // false on NaN as C++'s `<` is), so a known tag lets SCCP fold the dispatch
  // away where an always-taken call could not.
  // Factored out so compile_condition can chain `a < b < c` as
  // `(a < b) && (b < c)` with each middle operand evaluated once.
  llvm::Value* emit_comparison_i1(llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& ope_str) {
    // The operands stay frame-owned (borrow contract) on every throw edge of
    // the eq/ordering helpers — direct type error, non-Bool coercion, and a
    // user `__eq__`/`__lt__` dispatch alike.
    UnwindCovered cover(this, {lhs, rhs});
    auto ltag = extract_tag(lhs);
    auto ldata = extract_data(lhs);
    auto rtag = extract_tag(rhs);
    auto rdata = extract_data(rhs);

    bool is_eq = ope_str == "==" || ope_str == "!=";
    llvm::CmpInst::Predicate ipred, fpred;
    const char* ord_rt = nullptr;
    if (is_eq) {
      ipred = llvm::CmpInst::ICMP_EQ;
      fpred = llvm::CmpInst::FCMP_OEQ;
    } else if (ope_str == "<") {
      ipred = llvm::CmpInst::ICMP_SLT;
      fpred = llvm::CmpInst::FCMP_OLT;
      ord_rt = rt::value_less_borrow;
    } else if (ope_str == "<=") {
      ipred = llvm::CmpInst::ICMP_SLE;
      fpred = llvm::CmpInst::FCMP_OLE;
      ord_rt = rt::value_leq_borrow;
    } else if (ope_str == ">") {
      ipred = llvm::CmpInst::ICMP_SGT;
      fpred = llvm::CmpInst::FCMP_OGT;
      ord_rt = rt::value_greater_borrow;
    } else if (ope_str == ">=") {
      ipred = llvm::CmpInst::ICMP_SGE;
      fpred = llvm::CmpInst::FCMP_OGE;
      ord_rt = rt::value_geq_borrow;
    } else {
      throw std::runtime_error("invalid comparison operator");
    }

    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "cmp.int", fn);
    auto checkNumBB = llvm::BasicBlock::Create(ctx_, "cmp.check_num", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "cmp.float", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "cmp.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "cmp.merge", fn);

    auto longTag = builder_.getInt8(TAG_LONG);
    auto floatTag = builder_.getInt8(TAG_FLOAT);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "cmp.bothlong");
    builder_.CreateCondBr(bothLong, intBB, checkNumBB);

    builder_.SetInsertPoint(intBB);
    auto intResult = builder_.CreateICmp(ipred, ldata, rdata, "cmp.i");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(checkNumBB);
    auto lIsFloat = builder_.CreateICmpEQ(ltag, floatTag);
    auto rIsFloat = builder_.CreateICmpEQ(rtag, floatTag);
    auto bothNum = builder_.CreateAnd(builder_.CreateOr(lIsLong, lIsFloat),
                                      builder_.CreateOr(rIsLong, rIsFloat),
                                      "cmp.bothnum");
    builder_.CreateCondBr(bothNum, floatBB, slowBB);

    builder_.SetInsertPoint(floatBB);
    auto floatResult = builder_.CreateFCmp(fpred, num_as_double(lIsLong, ldata, "l"),
                                           num_as_double(rIsLong, rdata, "r"),
                                           "cmp.f");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    // Not guarded on the unwind edge: the operands stay frame-owned (borrow
    // contract), so a region's release ladder is the one releaser on the
    // throw path.
    llvm::Value* slowResult;
    if (is_eq) {
      // `==`/`!=` may invoke a user `__eq__`/`eq` whose bool-coercion throws
      // a positionless error; publish the operator position so the exception
      // boundary backfills it (the ordering ops carry line/col explicitly).
      emit_set_op_pos();
      slowResult = emit_call(
          module_->getOrInsertFunction(
              rt::value_equal_borrow, builder_.getInt1Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {ltag, ldata, rtag, rdata}, "val.eq");
    } else {
      // Separate helpers (rather than deriving <=, >, >= from <) because
      // `nil op nil` is `false` for every ordering operator — not the
      // standard total-order relationship.
      slowResult = emit_call(
          module_->getOrInsertFunction(
              ord_rt, builder_.getInt1Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty()),
          {ltag, ldata, rtag, rdata, current_line_val(),
           current_column_val()},
          "val.ord");
    }
    auto slowEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 3, "cmp.r");
    phi->addIncoming(intResult, intBB);
    phi->addIncoming(floatResult, floatBB);
    phi->addIncoming(slowResult, slowEndBB);
    llvm::Value* result = phi;
    if (ope_str == "!=") result = builder_.CreateNot(result, "neq");
    return result;
  }

  // A literal step needs no runtime zero check — the constant is the proof.
  static bool is_nonzero_constant(llvm::Value* v) {
    auto* c = llvm::dyn_cast<llvm::ConstantInt>(v);
    return c && !c->isZero();
  }

  // A zero step never terminates: reject it before the first iteration, as
  // range_iter does on the generic path. Reported at the range expression's
  // own position (the interpreter's `iter_expr`), not the enclosing `for`.
  // A literal non-zero step is already proven safe, so the call is skipped.
  // Shared by compile_for's counted fast path and the bytecode-VM lowering.
  void emit_range_step_check(llvm::Value* stepV, size_t line, size_t col) {
    if (!stepV || is_nonzero_constant(stepV)) return;
    emit_call(module_->getOrInsertFunction(
                  rt::range_step_check, builder_.getVoidTy(),
                  builder_.getInt64Ty(), builder_.getInt64Ty(),
                  builder_.getInt64Ty()),
              {stepV, builder_.getInt64(static_cast<int64_t>(line)),
               builder_.getInt64(static_cast<int64_t>(col))});
  }

  // Per-loop iteration state. One set of allocas per for-in statement; each
  // container kind fills the subset it needs and the shared head dispatches
  // on `kind`. Uniform state is what lets the body be emitted once.
  struct ForCursor {
    llvm::Value* kind;      // i8    — ForKind
    llvm::Value* src;       // ptr   — array storage / string bytes
    llvm::Value* pos;       // i64   — element index / byte offset
    llvm::Value* count;     // i64   — element count / byte length
    llvm::Value* iter;      // Value — scope slot owning the protocol iterator
                            //         (nil for the array / string cursors)
    llvm::Value* obj;       // Value — scope slot owning the derived-from value
    llvm::Value* has_next;  // ptr   — hoisted has_next closure
    llvm::Value* next;      // ptr   — hoisted next closure
    // A pending guard (make_pending_guard's contract): holds this iteration's
    // element at +1 only between the advance's store and the body binding
    // taking it over, and nil everywhere else. The advance-to-binding window
    // has throw points (the safepoint, a destructure mismatch), so `for.exc`
    // releases this slot; the nil outside the window is what makes that
    // release unconditional and exactly-once.
    llvm::Value* elem;      // Value — this iteration's element, +1 | nil
    llvm::Value* out_tag;   // i8    — array_get / iter_advance scratch
    llvm::Value* out_data;  // i64
  };

  ForCursor make_for_cursor() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto zeroVal = llvm::ConstantAggregateZero::get(valueType_);
    ForCursor c;
    c.kind = entryB.CreateAlloca(i8Ty, nullptr, "for.kind");
    c.src = entryB.CreateAlloca(ptrTy, nullptr, "for.src");
    c.pos = entryB.CreateAlloca(i64Ty, nullptr, "for.pos");
    c.count = entryB.CreateAlloca(i64Ty, nullptr, "for.count");
    c.iter = nullptr;  // both hold a +1: compile_for points them at scope slots
    c.obj = nullptr;
    c.has_next = entryB.CreateAlloca(ptrTy, nullptr, "for.has_next.cls");
    c.next = entryB.CreateAlloca(ptrTy, nullptr, "for.next.cls");
    c.elem = entryB.CreateAlloca(valueType_, nullptr, "for.elem.slot");
    entryB.CreateStore(zeroVal, c.elem);
    c.out_tag = entryB.CreateAlloca(i8Ty, nullptr, "for.out.tag");
    c.out_data = entryB.CreateAlloca(i64Ty, nullptr, "for.out.data");
    return c;
  }

  // Array / Tuple / Set: a direct indexed walk, re-reading the size every
  // step so the walk ends where the live container ends (interp-symmetric;
  // see emit_for_advance_array).
  // Shared tail of the two counted cursors: an array walk and a UTF-8 scalar
  // walk differ only in what they count.
  void open_counted_cursor(const ForCursor& c, llvm::Value* ptr,
                           llvm::Value* count, ForKind kind) {
    builder_.CreateStore(ptr, c.src);
    builder_.CreateStore(count, c.count);
    builder_.CreateStore(builder_.getInt64(0), c.pos);
    builder_.CreateStore(builder_.getInt8(kind), c.kind);
  }

  void emit_for_open_array(const ForCursor& c, llvm::Value* arrPtr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                     ptrTy),
        {arrPtr}, "for.size");
    // The array advance re-reads the size rather than reading `c.count`
    // back (the body can resize the receiver); only the string walk, whose
    // subject cannot change under it, consumes the stored count.
    open_counted_cursor(c, arrPtr, size, FOR_ARRAY);
  }

  void emit_for_advance_array(const ForCursor& c, llvm::BasicBlock* advBB,
                              llvm::BasicBlock* bodyBB,
                              llvm::BasicBlock* exitBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();
    auto fn = advBB->getParent();
    auto stepBB = llvm::BasicBlock::Create(ctx_, "for.next.array.step", fn);

    builder_.SetInsertPoint(advBB);
    auto idx = builder_.CreateLoad(i64Ty, c.pos, "for.i");
    // Re-read the size instead of trusting the cursor's opening count: the
    // body may shrink the receiver, and the walk has to end where the live
    // array ends (interp's rule). Tuples are immutable and the Set branch
    // walks a private materialized copy, so only Array can actually move.
    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                     ptrTy),
        {builder_.CreateLoad(ptrTy, c.src)}, "for.n");
    builder_.CreateCondBr(builder_.CreateICmpSGE(idx, size), exitBB, stepBB);

    builder_.SetInsertPoint(stepBB);
    builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), c.pos);
    emit_call(
        module_->getOrInsertFunction(
            rt::array_get, builder_.getVoidTy(), ptrTy, i64Ty, ptrTy, ptrTy,
            i64Ty, i64Ty),
        {builder_.CreateLoad(ptrTy, c.src), idx, c.out_tag, c.out_data,
         current_line_val(), current_column_val()});
    auto t = builder_.CreateLoad(builder_.getInt8Ty(), c.out_tag);
    auto d = builder_.CreateLoad(i64Ty, c.out_data);
    // The element is read straight out of the container (a borrow); the body
    // slot releases on scope exit, so hand it a matching +1.
    builder_.CreateStore(emit_borrow_to_owned(make_value(t, d)), c.elem);
    builder_.CreateBr(bodyBB);
  }

  // Iterator trait dispose: call `iter.dispose()` if the cursor holds an
  // iterator and it carries one. The array and string cursors leave the slot
  // nil, so they skip the whole sequence, and built-in iterators have no
  // dispose. Releasing the iterator is not this function's job — `iterAlloca`
  // is a frame slot, so the ordinary teardown frees it on every exit, which is
  // also why a throw out of dispose needs no local pad here. Shared by the
  // for-in protocol loop's natural-exit / break, exception, and
  // early-`return` paths (the lowering's emit_for_dispose) — the last is what
  // keeps dispose-on-early-return symmetric with the interpreter.
  void emit_iter_dispose_if_active(llvm::Value* iterAlloca,
                                   const char* label_prefix,
                                   bool swallow_dispose = false) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    // Returned by value: each call feeds one Twine parameter, consumed within
    // that full expression.
    auto name = [&](const char* suffix) {
      return std::string(label_prefix) + suffix;
    };
    auto iterFinal =
        builder_.CreateLoad(valueType_, iterAlloca, name(".iter"));
    auto activeBB = llvm::BasicBlock::Create(ctx_, name(".active"), fn);
    auto disposeBB = llvm::BasicBlock::Create(ctx_, name(".dispose"), fn);
    auto doneBB = llvm::BasicBlock::Create(ctx_, name(".done"), fn);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(extract_tag(iterFinal),
                              builder_.getInt8(TAG_NIL), name(".alive")),
        activeBB, doneBB);

    // object_has is safe only once the iterator is known non-nil.
    builder_.SetInsertPoint(activeBB);
    auto hasDispose = emit_object_has(
        builder_.CreateIntToPtr(extract_data(iterFinal), ptrTy,
                                name(".iter.ptr")),
        get_or_create_global_str("dispose", ".dispose.key"),
        name(".has_dispose"));
    builder_.CreateCondBr(hasDispose, disposeBB, doneBB);

    builder_.SetInsertPoint(disposeBB);
    auto dispose_fn_val = emit_property_get(iterFinal, "dispose");
    // Hand the dispose frame its own +1 for `self` (same convention as the
    // iter() call in emit_for_open_protocol) — the slot keeps holding its own
    // ref until scope teardown, so this retain is the frame's alone.
    emit_value_retain(iterFinal);
    if (!fn->hasPersonalityFn()) fn->setPersonalityFn(get_personality_fn());
    if (swallow_dispose) {
      // On the exception / early-return exit paths the interpreter swallows a
      // throwing dispose() (it must not turn an in-flight throw or a `return`
      // into the dispose's exception). Route the dispose call to a local
      // catch-and-discard landingpad, then carry on. (break / natural drain
      // propagate a throwing dispose, matching the interpreter.)
      // The thrown-value carrier is a global that a culebra throw from
      // dispose() OVERWRITES — swallowing the C++ exception alone still lets
      // the outer catch read dispose's payload instead of the original one.
      // Save the carrier before the call and restore it on both paths.
      auto swallowBB =
          llvm::BasicBlock::Create(ctx_, name(".dispose.swallow"), fn);
      auto restoreBB =
          llvm::BasicBlock::Create(ctx_, name(".dispose.rest"), fn);
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto i8Ty = builder_.getInt8Ty();
      auto i64Ty = builder_.getInt64Ty();
      auto flagA = entryB.CreateAlloca(i8Ty, nullptr, "thr.save.flag");
      auto tagA = entryB.CreateAlloca(i8Ty, nullptr, "thr.save.tag");
      auto dataA = entryB.CreateAlloca(i64Ty, nullptr, "thr.save.data");
      emit_call(module_->getOrInsertFunction(
                    "culebra_runtime_save_thrown", builder_.getVoidTy(),
                    ptrTy, ptrTy, ptrTy),
                {flagA, tagA, dataA});
      auto savedLpad = current_lpad_;
      current_lpad_ = swallowBB;
      compile_function_call_raw(dispose_fn_val, iterFinal, {});
      current_lpad_ = savedLpad;
      builder_.CreateBr(restoreBB);
      emit_handler_prologue(swallowBB, name(".dispose.lpad").c_str());
      builder_.CreateCall(
          module_->getOrInsertFunction("__cxa_end_catch", builder_.getVoidTy()),
          {});
      builder_.CreateBr(restoreBB);
      builder_.SetInsertPoint(restoreBB);
      emit_call(module_->getOrInsertFunction(
                    "culebra_runtime_restore_thrown", builder_.getVoidTy(),
                    i8Ty, i8Ty, i64Ty),
                {builder_.CreateLoad(i8Ty, flagA),
                 builder_.CreateLoad(i8Ty, tagA),
                 builder_.CreateLoad(i64Ty, dataA)});
    } else {
      // A throwing dispose propagates (interp-symmetric) straight into the
      // enclosing pad, which releases the iterator slot along with the rest of
      // the statement's scope — no local unwind edge to hand-place here.
      compile_function_call_raw(dispose_fn_val, iterFinal, {});
    }
    builder_.CreateBr(doneBB);

    builder_.SetInsertPoint(doneBB);
  }

  // Iterator protocol: `c.obj.iter()` yields the iterator, then each `next()`
  // returns `{done, value}` until done. Semantics match `_get_iterator` /
  // `eval_for`'s protocol branch in the interpreter.
  //
  // Refcount flow: the caller has already parked a +1 iterable in `c.obj`
  // (range_iter / object_iter build a fresh one; the user-`iter` arm retains),
  // so every exit path frees it. `iter()` returns a fresh +1 stored in
  // `c.iter`; each method call retains its receiver before invocation (the
  // callee frame releases via its owned `self` slot).
  // `open_line` / `open_col` are the iterable expression's position — where
  // the interpreter reports a broken protocol from, so a bad iterator points
  // at the same source on every backend.
  void emit_for_open_protocol(const ForCursor& c, llvm::Value* open_line,
                              llvm::Value* open_col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto objVal = builder_.CreateLoad(valueType_, c.obj, "for.proto.src");

    auto iter_fn_val = emit_property_get(objVal, "iter");
    emit_value_retain(objVal);  // handed off to `self` slot
    Owned iter_owned = compile_function_call_raw(iter_fn_val, objVal, {});

    // `has_next` + `next` are fixed for an iterator's lifetime — resolve both
    // once so each step is a pair of cached closure dispatches rather than a
    // per-iteration `find_slot`. The runtime open validates the protocol and
    // writes the two slots; it is the only place that turns those property
    // values into closure pointers (see culebra_runtime_iter_protocol_open).
    //
    // Opened while the iterator is still in its Owned handle: rejecting a
    // broken one throws, and the unwind-temp window frees the handle on
    // that edge — the slot has not taken ownership yet at this point.
    emit_call(
        module_->getOrInsertFunction(
            rt::iter_protocol_open, builder_.getVoidTy(), builder_.getInt8Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty(),
            builder_.getInt64Ty(), ptrTy, ptrTy),
        {extract_tag(iter_owned.borrow()), extract_data(iter_owned.borrow()),
         open_line, open_col, c.has_next, c.next});

    // The slot owns the iterator from here (every exit path releases it); the
    // raw deliberately crosses the blocks below with the slot as its per-edge
    // releaser — hence unchecked.
    llvm::Value* iter_val = iter_owned.consume_unchecked();
    builder_.CreateStore(iter_val, c.iter);
    builder_.CreateStore(builder_.getInt8(FOR_PROTO), c.kind);
  }

  void emit_for_advance_protocol(const ForCursor& c, llvm::BasicBlock* advBB,
                                 llvm::BasicBlock* bodyBB,
                                 llvm::BasicBlock* exitBB, size_t step_line,
                                 size_t step_col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = advBB->getParent();
    auto stepBB = llvm::BasicBlock::Create(ctx_, "for.next.proto.step", fn);

    builder_.SetInsertPoint(advBB);
    // A step raises positionless (a non-truthy-typed `has_next()` answer, a
    // broken protocol met mid-chain); the interpreter reports those at the
    // statement it was running, so publish the `for` node for the backfill.
    emit_call(module_->getFunction(rt::set_op_pos),
              {builder_.getInt64(static_cast<int64_t>(step_line)),
               builder_.getInt64(static_cast<int64_t>(step_col))});
    auto iterCur = builder_.CreateLoad(valueType_, c.iter, "iter.cur");
    auto ok = emit_call(
        module_->getFunction(rt::iter_advance),
        {builder_.CreateLoad(ptrTy, c.has_next),
         builder_.CreateLoad(ptrTy, c.next), extract_tag(iterCur),
         extract_data(iterCur), c.out_tag, c.out_data},
        "for.p.ok");
    builder_.CreateCondBr(
        builder_.CreateICmpNE(ok, builder_.getInt64(0), "for.p.alive"), stepBB,
        exitBB);

    builder_.SetInsertPoint(stepBB);
    // iter_advance already transferred the step value's +1.
    auto t = builder_.CreateLoad(builder_.getInt8Ty(), c.out_tag, "for.p.tag");
    auto d =
        builder_.CreateLoad(builder_.getInt64Ty(), c.out_data, "for.p.data");
    builder_.CreateStore(make_value(t, d), c.elem);
    builder_.CreateBr(bodyBB);
  }

  // The for-in head: switch on the iterable's tag, fill `c` with the cursor
  // that walks it, and hand back the block every arm branches to. Shared by
  // compile_for and the bytecode VM's ForOpen, so a loop opens the same way
  // whichever lane compiled it — including which value drives the protocol
  // and which slot ends up owning it.
  //
  // `set_slot` and `own_slot` are the caller's frame-owned Value slots: the
  // Set arm parks its materialised member Array in the first, and the object
  // arms park what `iter()` is called on in the second, so every exit path
  // releases them.
  // `iter_line` / `iter_col` are the iterable expression's position — where
  // the interpreter reports both a non-iterable and a broken protocol.
  llvm::BasicBlock* emit_for_open_dispatch(const ForCursor& c,
                                           llvm::Value* iterable,
                                           llvm::Value* set_slot,
                                           llvm::Value* own_slot,
                                           size_t iter_line, size_t iter_col) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto tag = extract_tag(iterable);
    auto data = extract_data(iterable);

    auto arrayBB = llvm::BasicBlock::Create(ctx_, "for.array", fn);
    auto setBB    = llvm::BasicBlock::Create(ctx_, "for.set", fn);
    auto objectBB = llvm::BasicBlock::Create(ctx_, "for.object", fn);
    auto stringBB = llvm::BasicBlock::Create(ctx_, "for.string", fn);
    auto badBB = llvm::BasicBlock::Create(ctx_, "for.bad_type", fn);
    auto headBB = llvm::BasicBlock::Create(ctx_, "for.head", fn);

    auto sw = builder_.CreateSwitch(tag, badBB, 6);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrayBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), arrayBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objectBB);
    sw->addCase(builder_.getInt8(TAG_STRING), stringBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), stringBB);

    builder_.SetInsertPoint(badBB);
    // Attribute the not-iterable error to the iterable expression (like
    // interp's _get_iterator), not the `for` keyword — compile(iter_expr)
    // above restored current_line_/col to the loop head via PosGuard.
    if (iter_line) current_line_ = iter_line;
    if (iter_col) current_column_ = iter_col;
    emit_type_error_typed("Array, Tuple, Set, Object, or String", tag);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrayBB);
    emit_for_open_array(c, builder_.CreateIntToPtr(data, ptrTy));
    builder_.CreateBr(headBB);

    // Set: materialize members into a fresh Array, then walk it with the
    // array cursor.
    builder_.SetInsertPoint(setBB);
    auto setSrcPtr = builder_.CreateIntToPtr(data, ptrTy);
    auto setMembersArr = emit_call(
        module_->getOrInsertFunction(rt::set_to_array, ptrTy, ptrTy),
        {setSrcPtr}, "for.set.arr");
    store_slot(set_slot, make_array(setMembersArr));
    emit_for_open_array(c, setMembersArr);
    builder_.CreateBr(headBB);

    // Object branch picks which value drives the iterator protocol:
    //   range        → its own range_iter (a fresh +1)
    //   `iter` prop  → the object itself (Math.range, user iterators,
    //                  generators, `.code_points()`/`.graphemes()` et al.)
    //   otherwise    → the builtin key iterator (a fresh +1)
    // All three converge on one protocol setup, so the loop below is
    // emitted once no matter which arm ran.
    builder_.SetInsertPoint(objectBB);
    auto objPtr = builder_.CreateIntToPtr(data, ptrTy);
    // A range value iterates start..end (errors if unbounded). It carries
    // no `iter` property, so it must be handled before the keys fallback.
    auto rangeBB = llvm::BasicBlock::Create(ctx_, "for.obj.range", fn);
    auto notRangeBB = llvm::BasicBlock::Create(ctx_, "for.obj.notrange", fn);
    auto protoOpenBB = llvm::BasicBlock::Create(ctx_, "for.obj.open", fn);
    builder_.CreateCondBr(emit_is_range(iterable), rangeBB, notRangeBB);

    builder_.SetInsertPoint(rangeBB);
    auto rangeIt = emit_call(
        module_->getOrInsertFunction(rt::range_iter, ptrTy, builder_.getInt64Ty(),
                                     builder_.getInt64Ty(), builder_.getInt64Ty()),
        {data, current_line_val(), current_column_val()});
    store_slot(own_slot, make_object(rangeIt));
    builder_.CreateBr(protoOpenBB);

    builder_.SetInsertPoint(notRangeBB);
    auto iterKeyPtr = get_or_create_global_str("iter", ".iter.key");
    auto hasIter = emit_object_has(objPtr, iterKeyPtr);
    auto keysBB = llvm::BasicBlock::Create(ctx_, "for.obj.keys", fn);
    auto protoBB = llvm::BasicBlock::Create(ctx_, "for.obj.proto", fn);
    builder_.CreateCondBr(hasIter, protoBB, keysBB);

    builder_.SetInsertPoint(keysBB);
    // No user-defined `iter`: drive the builtin object_iter so the same
    // mut_count fail-fast that protects `obj.iter()` also covers the
    // `for k in obj` sugar.
    auto objIter = emit_call(module_->getFunction(rt::object_iter),
                             {objPtr});
    store_slot(own_slot, make_object(objIter));
    builder_.CreateBr(protoOpenBB);

    builder_.SetInsertPoint(protoBB);
    // The slot owns the value the iterator is derived from, so the borrowed
    // receiver is retained to match the fresh +1 the other two arms hand
    // over. For a self-returning iterator (a generator's iter() = this) that
    // makes two refs on one object, released once each by the two slots.
    store_slot(own_slot, emit_borrow_to_owned(make_value(TAG_OBJECT, data)));
    builder_.CreateBr(protoOpenBB);

    builder_.SetInsertPoint(protoOpenBB);
    emit_for_open_protocol(c,
                           builder_.getInt64(static_cast<int64_t>(iter_line)),
                           builder_.getInt64(static_cast<int64_t>(iter_col)));
    builder_.CreateBr(headBB);

    builder_.SetInsertPoint(stringBB);
    // For StringView, materialize via strlike_to_cstr (TAG_STRING input
    // returns the same ptr — no copy).
    auto strDataPtr = emit_call(
        module_->getFunction(rt::strlike_to_cstr), {tag, data});
    emit_for_open_string(c, strDataPtr);
    builder_.CreateBr(headBB);
    return headBB;
  }

  void emit_for_open_string(const ForCursor& c, llvm::Value* strPtr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto len = emit_call(
        module_->getOrInsertFunction(rt::str_size, builder_.getInt64Ty(),
                                     ptrTy),
        {strPtr}, "for.slen");
    open_counted_cursor(c, strPtr, len, FOR_STRING);
  }

  void emit_for_advance_string(const ForCursor& c, llvm::BasicBlock* advBB,
                               llvm::BasicBlock* bodyBB,
                               llvm::BasicBlock* exitBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();
    auto fn = advBB->getParent();
    auto stepBB = llvm::BasicBlock::Create(ctx_, "for.next.string.step", fn);

    builder_.SetInsertPoint(advBB);
    auto strPtr = builder_.CreateLoad(ptrTy, c.src);
    auto off = builder_.CreateLoad(i64Ty, c.pos, "for.o");
    auto scalarLen = emit_call(
        module_->getOrInsertFunction(rt::utf8_scalar_len, i64Ty, ptrTy, i64Ty,
                                     i64Ty),
        {strPtr, off, builder_.CreateLoad(i64Ty, c.count)}, "for.slen1");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(scalarLen, builder_.getInt64(0)), exitBB, stepBB);

    builder_.SetInsertPoint(stepBB);
    builder_.CreateStore(builder_.CreateAdd(off, scalarLen), c.pos);
    // Yield a 1-scalar StringView into the source buffer (matches interp
    // and `s.iter()`), not a copied String.
    auto scalarView = emit_call(
        module_->getOrInsertFunction(rt::str_scalar_view, ptrTy, ptrTy, i64Ty,
                                     i64Ty),
        {strPtr, off, scalarLen}, "for.sview");
    // A view into the source buffer; the body slot releases it.
    builder_.CreateStore(emit_borrow_to_owned(make_stringview(scalarView)),
                         c.elem);
    builder_.CreateBr(bodyBB);
  }

  // --- Function ---




  // Emit module-level globals describing this function's parameter
  // list, then return a constant pointer to a JitParamMeta struct. The
  // runtime resolver `culebra_runtime_call_with_kwargs` consults this
  // table via the side map keyed by `fn_ptr`. Every function gets a
  // meta global — a nullary one still needs fn.name / fn.return_type
  // for introspection (the minimal-meta arm below).
  // Takes "does this parameter have a default" as bits rather than the
  // default expressions themselves: the bitmask is all the metadata needs,
  // and asking for bits is what lets the bytecode lowering — which has no
  // AST to hand — emit the same globals.
  llvm::Constant* emit_param_meta_global(
      llvm::Function* fn,
      const std::vector<std::string>& paramNames,
      const std::vector<bool>& paramHasDefault,
      std::optional<size_t> kwargsRestIdx = std::nullopt,
      std::optional<size_t> firstKwOnlyIdx = std::nullopt,
      const std::string& fnName = {},
      const std::string& returnType = {},
      const std::vector<bool>& paramMuts = {},
      const std::vector<std::string>& paramTypes = {},
      const std::vector<std::string>& paramDeclaredTypes = {},
      long cbMin = 0, long cbMax = 0) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i64Ty = builder_.getInt64Ty();
    auto i8Ty = builder_.getInt8Ty();
    auto fnBase = std::string(fn->getName());

    // Nullary functions still need a meta global so fn.name and
    // fn.return_type can be read by introspection. Emit a minimal
    // meta with null param-array pointers and n_params=0; the
    // runtime helper's loops skip element access when n_params==0.
    if (paramNames.empty()) {
      auto fnNameG = builder_.CreateGlobalString(
          fnName, fnBase + ".pmeta.fn");
      auto retTyG = builder_.CreateGlobalString(
          returnType, fnBase + ".pmeta.ret");
      auto metaTy = llvm::StructType::get(ctx_,
          {ptrTy, ptrTy, i64Ty, i64Ty, i64Ty,
           ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, i64Ty, i64Ty});
      auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);
      auto metaInit = llvm::ConstantStruct::get(
          metaTy,
          {nullPtr, nullPtr,
           llvm::ConstantInt::get(i64Ty, 0),
           llvm::ConstantInt::get(i64Ty, -1),
           llvm::ConstantInt::get(i64Ty, -1),
           llvm::ConstantExpr::getBitCast(fnNameG, ptrTy),
           llvm::ConstantExpr::getBitCast(retTyG, ptrTy),
           nullPtr, nullPtr, nullPtr,
           llvm::ConstantInt::get(i64Ty, cbMin),
           llvm::ConstantInt::get(i64Ty, cbMax)});
      auto metaGlobal = new llvm::GlobalVariable(
          *module_, metaTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, metaInit,
          fnBase + ".pmeta");
      return metaGlobal;
    }

    // Names array: one cstring per param.
    std::vector<llvm::Constant*> name_consts;
    name_consts.reserve(paramNames.size());
    for (const auto& nm : paramNames) {
      name_consts.push_back(builder_.CreateGlobalString(
          nm, ".param.name." + fnBase + "." + nm));
    }
    auto namesArrTy = llvm::ArrayType::get(ptrTy, name_consts.size());
    auto namesInit = llvm::ConstantArray::get(namesArrTy, name_consts);
    auto namesGlobal = new llvm::GlobalVariable(
        *module_, namesArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, namesInit,
        fnBase + ".pmeta.names");

    // has_default bitmask: ceil(N/8) bytes, bit i = paramHasDefault[i].
    size_t n_bytes = (paramNames.size() + 7) / 8;
    std::vector<llvm::Constant*> bit_consts(n_bytes,
                                             builder_.getInt8(0));
    for (size_t i = 0; i < paramHasDefault.size(); i++) {
      if (paramHasDefault[i]) {
        auto byte_idx = i / 8;
        auto cur = llvm::cast<llvm::ConstantInt>(bit_consts[byte_idx])
                       ->getZExtValue();
        bit_consts[byte_idx] =
            builder_.getInt8(cur | (1u << (i % 8)));
      }
    }
    auto bitsArrTy = llvm::ArrayType::get(i8Ty, n_bytes);
    auto bitsInit = llvm::ConstantArray::get(bitsArrTy, bit_consts);
    auto bitsGlobal = new llvm::GlobalVariable(
        *module_, bitsArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, bitsInit,
        fnBase + ".pmeta.bits");

    // Introspection-only globals (fn name, return type, param mut bits,
    // param type names). Empty strings replace nullptr so the runtime
    // can always deref these without a null check.
    auto fnNameG = builder_.CreateGlobalString(fnName, fnBase + ".pmeta.fn");
    auto retTyG = builder_.CreateGlobalString(
        returnType, fnBase + ".pmeta.ret");

    std::vector<llvm::Constant*> mut_consts(n_bytes, builder_.getInt8(0));
    for (size_t i = 0; i < paramMuts.size(); i++) {
      if (paramMuts[i]) {
        auto byte_idx = i / 8;
        auto cur = llvm::cast<llvm::ConstantInt>(mut_consts[byte_idx])
                       ->getZExtValue();
        mut_consts[byte_idx] = builder_.getInt8(cur | (1u << (i % 8)));
      }
    }
    auto mutBitsInit = llvm::ConstantArray::get(bitsArrTy, mut_consts);
    auto mutBitsGlobal = new llvm::GlobalVariable(
        *module_, bitsArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, mutBitsInit,
        fnBase + ".pmeta.muts");

    std::vector<llvm::Constant*> type_consts;
    type_consts.reserve(paramNames.size());
    for (size_t i = 0; i < paramNames.size(); i++) {
      const std::string& t = i < paramTypes.size() ? paramTypes[i] : "";
      type_consts.push_back(builder_.CreateGlobalString(
          t, ".param.type." + fnBase + "." + paramNames[i]));
    }
    auto typesArrTy = llvm::ArrayType::get(ptrTy, type_consts.size());
    auto typesInit = llvm::ConstantArray::get(typesArrTy, type_consts);
    auto typesGlobal = new llvm::GlobalVariable(
        *module_, typesArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, typesInit,
        fnBase + ".pmeta.types");

    // Declared (pre-neutralization) types for introspection — same
    // layout as types, falls back to the effective type when missing.
    std::vector<llvm::Constant*> declared_consts;
    declared_consts.reserve(paramNames.size());
    for (size_t i = 0; i < paramNames.size(); i++) {
      const std::string& t = i < paramDeclaredTypes.size() &&
                                 !paramDeclaredTypes[i].empty()
                                 ? paramDeclaredTypes[i]
                                 : (i < paramTypes.size() ? paramTypes[i]
                                                          : std::string());
      declared_consts.push_back(builder_.CreateGlobalString(
          t, ".param.declared." + fnBase + "." + paramNames[i]));
    }
    auto declaredArrTy = llvm::ArrayType::get(ptrTy, declared_consts.size());
    auto declaredInit = llvm::ConstantArray::get(declaredArrTy, declared_consts);
    auto declaredGlobal = new llvm::GlobalVariable(
        *module_, declaredArrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, declaredInit,
        fnBase + ".pmeta.declared");

    // Layout matches JitParamMeta in declaration order — append new
    // fields at the end so kwargs dispatch consumers stay unaffected.
    auto metaTy = llvm::StructType::get(ctx_,
        {ptrTy, ptrTy, i64Ty, i64Ty, i64Ty,
         ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, i64Ty, i64Ty});
    auto metaInit = llvm::ConstantStruct::get(
        metaTy,
        {llvm::ConstantExpr::getBitCast(namesGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(bitsGlobal, ptrTy),
         llvm::ConstantInt::get(i64Ty,
                                static_cast<int64_t>(paramNames.size())),
         llvm::ConstantInt::get(i64Ty,
             kwargsRestIdx ? static_cast<int64_t>(*kwargsRestIdx) : -1),
         llvm::ConstantInt::get(i64Ty,
             firstKwOnlyIdx ? static_cast<int64_t>(*firstKwOnlyIdx) : -1),
         llvm::ConstantExpr::getBitCast(fnNameG, ptrTy),
         llvm::ConstantExpr::getBitCast(retTyG, ptrTy),
         llvm::ConstantExpr::getBitCast(mutBitsGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(typesGlobal, ptrTy),
         llvm::ConstantExpr::getBitCast(declaredGlobal, ptrTy),
         llvm::ConstantInt::get(i64Ty, cbMin),
         llvm::ConstantInt::get(i64Ty, cbMax)});
    auto metaGlobal = new llvm::GlobalVariable(
        *module_, metaTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, metaInit,
        fnBase + ".pmeta");
    return metaGlobal;
  }


  // --- Call ---




  // Resolve `obj.name` read AS A VALUE (no call parens): fire a getter, bind a
  // plain method's `self` (interp's _wrap_method_with_this), or pass a data
  // field / introspection result through. `.name`/`.params`/`.return_type`
  // route to getter_or_value because emit_property_get's fn_mode returns a
  // +1-OWNED `view` for them; every other name yields a BORROWED `view` and
  // routes to bind_method_value. Both return a fresh +1-owned value, so the
  // caller does `own(...)` (the move-assign releases the receiver). Shared by
  // compile_call and compile_call_with_builtins so the two stay in lockstep.
  llvm::Value* emit_property_value_read(llvm::Value* receiver,
                                        llvm::Value* view,
                                        const std::string& name) {
    bool intro = fn_introspection_name(name);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto keyPtr =
        get_or_create_global_str(name, intro ? ".getter.key" : ".bind.key");
    // Both helpers return a 16-byte JitValue by value; route through
    // emit_value_call so the Win64 sret ABI matches the C++ side (no-op on
    // SysV). See the emit_value_call rationale at its definition.
    return emit_value_call(
        module_->getOrInsertFunction(
            intro ? rt::getter_or_value : rt::bind_method_value, valueType_,
            i8Ty, i64Ty, i8Ty, i64Ty, ptrTy),
        {extract_tag(receiver), extract_data(receiver), extract_tag(view),
         extract_data(view), keyPtr},
        intro ? "getter.or.value" : "bind.method");
  }


  // Get a property from an object (TAG_OBJECT required).
  //
  // Inlines a V8/SpiderMonkey-style monomorphic inline cache: each call
  // site owns a private IC global of `{Shape*, slot_offset}`. Fast path
  // (no runtime call): load `obj->shape`, compare with the cached
  // shape, and on hit load `slots.data()[offset].value` directly. Slow
  // path: call `culebra_runtime_object_get_ic`, which looks up the
  // shape and refreshes the IC. Layout assumption: std::vector's first
  // member is its data pointer (true on libc++/libstdc++/MSVC STL).
  llvm::Value* emit_property_get(llvm::Value* receiver,
                                    const std::string& name,
                                    bool own_receiver = false) {
    // Callee-cleans: with own_receiver the slow-path helper
    // (culebra_runtime_prop_get) is the sole releaser of the receiver on
    // its direct-error unwind edge —
    // exclude it from the unwind-temp window. A borrow caller's receiver
    // (own_receiver=false) keeps its own cleaner and needs no window either
    // way; covering only the owning form keeps the declaration precise.
    std::optional<UnwindCovered> cover;
    if (own_receiver) cover.emplace(this, std::initializer_list<llvm::Value*>{receiver});
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();

    auto tag = extract_tag(receiver);
    auto fn = builder_.GetInsertBlock()->getParent();

    // Function introspection: `.name` / `.params` / `.return_type` on a
    // function value follow a separate runtime path. Object IC stays
    // intact for receivers tagged TAG_OBJECT (a user-defined property
    // named `name` still resolves through the IC).
    bool fn_mode = fn_introspection_name(name);
    llvm::BasicBlock* finalMergeBB = nullptr;
    std::optional<OwnedPhi> finalMerge;
    if (fn_mode) {
      auto isFn =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC), "is.fn");
      auto fnBB = llvm::BasicBlock::Create(ctx_, "prop.fn", fn);
      auto notFnBB = llvm::BasicBlock::Create(ctx_, "prop.not_fn", fn);
      finalMergeBB = llvm::BasicBlock::Create(ctx_, "prop.final", fn);
      finalMerge.emplace(this, "prop.result");
      builder_.CreateCondBr(isFn, fnBB, notFnBB);

      builder_.SetInsertPoint(fnBB);
      auto clsPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
      auto keyPtr = builder_.CreateGlobalString(name, ".fn.prop");
      // Caller (compile_call_with_builtins) runs swap_owned on the
      // returned value, which retains the introspection result and releases
      // the original receiver. The Object path leaves receiver alive too —
      // mirror that contract here.
      finalMerge->add_incoming(emit_value_call(
          module_->getOrInsertFunction(rt::fn_introspect_get,
                                       valueType_, ptrTy, ptrTy),
          {clsPtr, keyPtr}, "fn.intro"));
      builder_.CreateBr(finalMergeBB);

      builder_.SetInsertPoint(notFnBB);
    }

    // Tag dispatch. Only the monomorphic object fast path (shape hit ->
    // direct slot read) is inlined; every cold case — IC miss, the
    // permissive "container reads a missing member as Nil" rule, and the
    // scalar TypeError — funnels through culebra_runtime_prop_get, so those
    // semantics live in one runtime function instead of being re-emitted as
    // IR at every read site. Keeping only the fast path inline also halves
    // the per-site block count, which is what SROA's dominance-frontier walk
    // scales with. The for-in iterator protocol only feeds TAG_OBJECT
    // receivers here, so its lookups stay on the fast path.
    auto recvData = extract_data(receiver);
    auto keyPtr = builder_.CreateGlobalString(name, ".key");

    // Per-site inline cache; layout mirrors JitPropIC field for field — the
    // IR reads only {shape, offset}, the runtime helper owns the proto trio.
    // All three shape fields are seeded to the non-null sentinel `(void*)1`
    // so the first read always misses to the slow path: real Shape* are
    // heap-allocated (8-byte aligned, never == 1) and a freshly-allocated
    // Object has shape == null, so the sentinel can never spuriously
    // fast-path into an OOB slots[0].
    auto icTy =
        llvm::StructType::get(ctx_, {ptrTy, i64Ty, ptrTy, ptrTy, i64Ty});
    auto* sentinelPtr = llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(i64Ty, 1), ptrTy);
    auto* icInit = llvm::ConstantStruct::get(
        icTy, {sentinelPtr, llvm::ConstantInt::get(i64Ty, 0), sentinelPtr,
               sentinelPtr, llvm::ConstantInt::get(i64Ty, 0)});
    auto* icGlobal = new llvm::GlobalVariable(
        *module_, icTy, /*isConstant=*/false,
        llvm::GlobalValue::PrivateLinkage, icInit,
        ".prop.ic." + std::to_string(prop_ic_counter_++));

    auto objBB = llvm::BasicBlock::Create(ctx_, "prop.obj", fn);
    auto fastBB = llvm::BasicBlock::Create(ctx_, "prop.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "prop.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "prop.merge", fn);

    // Non-object receivers skip straight to the cold path.
    auto isObj =
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT), "is.obj");
    builder_.CreateCondBr(isObj, objBB, slowBB);

    // Object: take the fast path only when its shape matches the cache.
    builder_.SetInsertPoint(objBB);
    auto objPtr = builder_.CreateIntToPtr(recvData, ptrTy);
    auto shapeFieldPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, objPtr, offsetof(JitObject, shape), "shape.fieldp");
    auto objShape = builder_.CreateLoad(ptrTy, shapeFieldPtr, "obj.shape");
    auto icShapePtr =
        builder_.CreateStructGEP(icTy, icGlobal, 0, "ic.shape.p");
    auto icShape = builder_.CreateLoad(ptrTy, icShapePtr, "ic.shape");
    auto shapeMatch =
        builder_.CreateICmpEQ(objShape, icShape, "shape.match");
    builder_.CreateCondBr(shapeMatch, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    auto icOffsetPtr =
        builder_.CreateStructGEP(icTy, icGlobal, 1, "ic.off.p");
    auto icOffset = builder_.CreateLoad(i64Ty, icOffsetPtr, "ic.off");

    auto slotsFieldPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, objPtr, offsetof(JitObject, slots), "slots.vec.p");
    auto slotsData = builder_.CreateLoad(ptrTy, slotsFieldPtr, "slots.data");

    auto byteOffset = builder_.CreateMul(
        icOffset, llvm::ConstantInt::get(i64Ty, sizeof(JitObjectEntry)),
        "entry.byte.off");
    auto entryPtr = builder_.CreateInBoundsGEP(
        i8Ty, slotsData, byteOffset, "entry.p");

    auto fastTag = builder_.CreateLoad(i8Ty, entryPtr, "fast.tag");
    auto entryDataPtr = builder_.CreateConstInBoundsGEP1_64(
        i8Ty, entryPtr, offsetof(JitValue, data), "entry.data.p");
    auto fastData = builder_.CreateLoad(i64Ty, entryDataPtr, "fast.data");
    builder_.CreateBr(mergeBB);
    auto fastEnd = builder_.GetInsertBlock();

    // Cold path: non-object, IC miss, or scalar error — one runtime call.
    // Returns the value in registers (no out-param allocas: those would
    // escape to the call and survive SROA, dominating the backend's
    // alloca count in property-heavy code).
    builder_.SetInsertPoint(slowBB);
    auto slowResult = emit_value_call(
        module_->getOrInsertFunction(rt::prop_get, valueType_,
                                     i8Ty, i64Ty, ptrTy, ptrTy,
                                     i64Ty, i64Ty, builder_.getInt1Ty()),
        {tag, recvData, keyPtr, icGlobal,
         current_line_val(), current_column_val(),
         builder_.getInt1(own_receiver)},
        "prop.slow");
    auto slowTag = extract_tag(slowResult);
    auto slowData = extract_data(slowResult);
    builder_.CreateBr(mergeBB);
    auto slowEnd = builder_.GetInsertBlock();

    builder_.SetInsertPoint(mergeBB);
    auto tagPhi = builder_.CreatePHI(i8Ty, 2, "prop.tag");
    tagPhi->addIncoming(fastTag, fastEnd);
    tagPhi->addIncoming(slowTag, slowEnd);
    auto dataPhi = builder_.CreatePHI(i64Ty, 2, "prop.data");
    dataPhi->addIncoming(fastData, fastEnd);
    dataPhi->addIncoming(slowData, slowEnd);

    llvm::Value* result = make_value(tagPhi, dataPhi);

    // Merge with the function-introspection branch when active.
    if (fn_mode) {
      // The fn-introspection path (fnBB) yields a fresh +1 value, so the
      // caller releases the receiver without retaining the result (see the
      // `.name`/`.params`/`.return_type` branch in the postfix loop). The
      // object path here returns a BORROWED slot value (+0), so retain it to
      // give the merged result uniform +1 ownership. Without this, an object
      // whose `params`/`name`/`return_type` field holds a heap value (e.g. a
      // server req's `params` sub-object) is returned borrowed but treated as
      // owned → premature free → heap corruption. Scalars no-op (tag-aware),
      // which is why this only surfaced for object-valued fields.
      emit_value_retain(result);
      finalMerge->add_incoming(own(result));
      builder_.CreateBr(finalMergeBB);
      builder_.SetInsertPoint(finalMergeBB);
      return finalMerge->finish(finalMergeBB).consume();
    }
    return result;
  }

  // Point index `arr[key]` — Array/Tuple by Long, Object by Value key.
  // Returns a borrowed slot value (the caller promotes it). The Object path
  // (object_get_any) consumes `key`; the Array/Tuple path takes a Long
  // (non-refcounted), so callers must NOT release `key` themselves.
  llvm::Value* emit_point_index(llvm::Value* arr, llvm::Value* key) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto fn = builder_.GetInsertBlock()->getParent();
    auto tag = extract_tag(arr);

    auto arrBB    = llvm::BasicBlock::Create(ctx_, "idx.arr", fn);
    auto objBB    = llvm::BasicBlock::Create(ctx_, "idx.obj", fn);
    auto errBB    = llvm::BasicBlock::Create(ctx_, "idx.err", fn);
    auto mergeBB  = llvm::BasicBlock::Create(ctx_, "idx.merge", fn);

    // Both Array and Tuple share JitArray storage and the array_get
    // runtime helper indexes by Long for either.
    auto isArr = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
    auto chkTupBB = llvm::BasicBlock::Create(ctx_, "idx.chk_tup", fn);
    auto chkObjBB = llvm::BasicBlock::Create(ctx_, "idx.chk_obj", fn);
    builder_.CreateCondBr(isArr, arrBB, chkTupBB);
    builder_.SetInsertPoint(chkTupBB);
    auto isTup = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_TUPLE));
    builder_.CreateCondBr(isTup, arrBB, chkObjBB);
    builder_.SetInsertPoint(chkObjBB);
    auto isObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
    builder_.CreateCondBr(isObj, objBB, errBB);

    builder_.SetInsertPoint(errBB);
    // Non-indexable receiver (Set, String, …): release the owned receiver and
    // key before the direct type error. emit_point_index consumes `key` on
    // every returning path (callers never release it), but neither the array
    // nor object arm runs here, so a heap key (`aSet[{a:1}]`) would strand.
    // type_error_typed reads only the already extracted `tag` (never derefs a
    // value), so release-before-throw is safe; non-refcounted values no-op.
    emit_value_release(arr);
    emit_value_release(key);
    emit_type_error_typed("Array", tag);
    builder_.CreateUnreachable();

    // Allocate output slots in entry block; reused by both paths.
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(i8Ty, nullptr, "idx.out.tag");
    auto outData = entryB.CreateAlloca(i64Ty, nullptr, "idx.out.data");

    // Array path: index by Long. value_to_long (non-Long key) and array_get
    // (out-of-bounds) both raise a *direct* error — no user dispatch — so the
    // borrowed receiver strands on the unwind edge. Guard it there; the caller
    // releases it on the normal path (swap_owned), so it frees exactly once.
    // `key` is guarded too: the Array/Tuple path never releases it (a valid
    // index is a non-refcounted Long — a no-op), but a heap key (`arr[[9]]`)
    // that trips value_to_long's type error would otherwise strand.
    builder_.SetInsertPoint(arrBB);
    {
      ThrowGuard arr_guard(this, {arr, key});
      auto arrPtr = builder_.CreateIntToPtr(extract_data(arr), ptrTy);
      auto idx = value_to_long(key);
      emit_call(
          module_->getOrInsertFunction(
              rt::array_get, builder_.getVoidTy(), ptrTy, i64Ty, ptrTy,
              ptrTy, i64Ty, i64Ty),
          {arrPtr, idx, outTag, outData, current_line_val(),
           current_column_val()});
    }
    // array_get borrows the element slot; retain so emit_point_index returns a
    // uniformly +1-owned result (the object path's object_get_any already does).
    // Callers then just release the receiver, never re-retaining — the old
    // "promote borrowed via retain" model double-counted the object path (a
    // fresh SharedBuffer view leaked, a dict slot's refcount inflated).
    emit_value_retain(make_value(builder_.CreateLoad(i8Ty, outTag),
                                 builder_.CreateLoad(i64Ty, outData)));
    builder_.CreateBr(mergeBB);

    // Object path: look up by Value key in the non-String sidecar. object_get_any
    // owns the receiver on its *direct* KeyError edge (own_receiver=true) — a
    // user `__index__` dispatch is exempt there (its callee frame already
    // balances the receiver), so a codegen guard is unusable on this path.
    builder_.SetInsertPoint(objBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(arr), ptrTy);
    emit_call(
        module_->getOrInsertFunction(
            rt::object_get_any, builder_.getVoidTy(), ptrTy, i8Ty, i64Ty,
            ptrTy, ptrTy, i64Ty, i64Ty, builder_.getInt1Ty()),
        {objPtr, extract_tag(key), extract_data(key), outTag, outData,
         current_line_val(), current_column_val(), builder_.getInt1(true)});
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto tagLoaded = builder_.CreateLoad(i8Ty, outTag);
    auto dataLoaded = builder_.CreateLoad(i64Ty, outData);
    llvm::Value* result = make_value(tagLoaded, dataLoaded);
    return result;
  }

  // Slice `receiver` by a pre-compiled range value via culebra_runtime_slice
  // (which reads the range's bounds). The result is a fresh +1-owned value.
  llvm::Value* emit_slice_value(llvm::Value* receiver, llvm::Value* range) {
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(i8Ty, nullptr, "slice.out.tag");
    auto outData = entryB.CreateAlloca(i64Ty, nullptr, "slice.out.data");
    {
      // culebra_runtime_slice throws a direct "expected Array" type error for a
      // non-sliceable receiver (no user dispatch), stranding the borrowed
      // receiver and the codegen-owned range temp on the unwind edge — the
      // caller (emit_index_step) releases both only on the normal path. Guard
      // both so a slice on a non-sliceable receiver frees exactly once.
      ThrowGuard guard(this, {receiver, range});
      emit_call(
          module_->getOrInsertFunction(
              rt::slice, builder_.getVoidTy(), i8Ty, i64Ty, i64Ty, ptrTy,
              ptrTy, i64Ty, i64Ty),
          {extract_tag(receiver), extract_data(receiver), extract_data(range),
           outTag, outData, current_line_val(), current_column_val()});
    }
    auto tagLoaded = builder_.CreateLoad(i8Ty, outTag);
    auto dataLoaded = builder_.CreateLoad(i64Ty, outData);
    llvm::Value* result = make_value(tagLoaded, dataLoaded);
    return result;
  }

  // i1: is `key` a range value? (culebra_runtime_is_range short-circuits
  // non-Object tags internally.)
  llvm::Value* emit_is_range(llvm::Value* key) {
    auto i8Ty = builder_.getInt8Ty();
    auto i64Ty = builder_.getInt64Ty();
    auto r = emit_call(
        module_->getOrInsertFunction(rt::is_range, i8Ty, i8Ty, i64Ty),
        {extract_tag(key), extract_data(key)});
    return builder_.CreateICmpNE(r, builder_.getInt8(0));
  }


  // Whether `tag` names a value interp's to_object rejects outright, so a
  // member access on it fails at the property read — before any argument of
  // the call is evaluated.
  llvm::Value* emit_is_scalar_tag(llvm::Value* tag) {
    return builder_.CreateOr(
        builder_.CreateOr(
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_NIL)),
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_BOOL))),
        builder_.CreateOr(
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG)),
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FLOAT))));
  }

  // Emit interp's eval_property resolution-failure error for a builtin
  // method whose receiver is the wrong type: a scalar (Nil/Bool/Long/
  // Float — interp's to_object rejects it) → the member-access error
  // "expected Object, Array, or Tensor, got <T>"; any object-ish value
  // simply lacks the method → "expected Function, got Nil". Assumes the
  // builder is at the failure block and terminates it (unreachable).
  //
  // The object-ish miss is only KNOWN once the arguments have run: interp
  // reads the property (nil), evaluates the arguments, and fails on the
  // call. The bytecode compiler emits that argument evaluation itself
  // (Op::MethGate's miss edge), so this half only has to report.
  void emit_receiver_resolution_error(llvm::Value* tag,
                                      const std::string& prefix) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto scalarBB = llvm::BasicBlock::Create(ctx_, prefix + ".scalar", fn);
    auto objishBB = llvm::BasicBlock::Create(ctx_, prefix + ".objish", fn);
    builder_.CreateCondBr(emit_is_scalar_tag(tag), scalarBB, objishBB);
    builder_.SetInsertPoint(scalarBB);
    emit_type_error_typed("Object, Array, or Tensor", tag);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(objishBB);
    emit_type_error_typed("Function", builder_.getInt8(TAG_NIL));
    builder_.CreateUnreachable();
  }


  // The length probe behind `.size()` / `.empty()` / `.presence()`: switch on
  // the receiver tag (Tuple rides with Array), take each container's length
  // its own way, and merge them into one i64. A tag with no length raises the
  // receiver-resolution error for `label`, which also names the blocks.
  // Returns with the builder inserting at the merge block, so a caller only
  // has to turn the length into its own result.
  llvm::Value* emit_size_probe(llvm::Value* receiver, llvm::Value* tag,
                               const std::string& label) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto block = [&](const char* name) {
      return llvm::BasicBlock::Create(ctx_, label + "." + name, fn);
    };
    auto arrBB = block("arr");
    auto objBB = block("obj");
    auto strBB = block("str");
    auto svBB = block("sv");
    auto setBB = block("set");
    auto errBB = block("err");
    auto mergeBB = block("len");  // not ".merge" — presence names its own

    auto sw = builder_.CreateSwitch(tag, errBB, 6);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_TUPLE), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_STRINGVIEW), svBB);
    sw->addCase(builder_.getInt8(TAG_SET), setBB);

    // The PHI names where each arm actually leaves from, which is not where it
    // started when a helper splits the block.
    llvm::SmallVector<std::pair<llvm::Value*, llvm::BasicBlock*>, 5> arms;
    auto arm = [&](llvm::BasicBlock* bb, auto emit) {
      builder_.SetInsertPoint(bb);
      auto* size = emit(builder_.CreateIntToPtr(extract_data(receiver), ptrTy));
      arms.push_back({size, builder_.GetInsertBlock()});
      builder_.CreateBr(mergeBB);
    };

    arm(arrBB, [&](llvm::Value* p) {
      return emit_call(module_->getFunction(rt::array_size), {p}, "asz");
    });
    arm(objBB, [&](llvm::Value* p) {
      return emit_call(module_->getFunction(rt::object_size), {p}, "osz");
    });
    arm(strBB, [&](llvm::Value* p) {
      return emit_call(module_->getFunction(rt::str_size), {p}, "ssz");
    });
    // JitStringView { ptr, len } — load len at offset 8.
    arm(svBB, [&](llvm::Value* p) -> llvm::Value* {
      auto lenPtr =
          builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), p, 8);
      return builder_.CreateLoad(builder_.getInt64Ty(), lenPtr, "svsz");
    });
    arm(setBB, [&](llvm::Value* p) {
      return emit_call(module_->getOrInsertFunction(
                           rt::set_size, builder_.getInt64Ty(), ptrTy),
                       {p}, "ssz2");
    });

    builder_.SetInsertPoint(errBB);
    emit_receiver_resolution_error(tag, label);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt64Ty(),
                                  static_cast<unsigned>(arms.size()), "sz");
    for (auto& [size, bb] : arms) phi->addIncoming(size, bb);
    return phi;
  }



  // The at-most-once explicit-drop guard itself. Consumes the receiver's `+1`
  // (the runtime call only borrows it) and yields nil, `drop`'s value. The
  // borrow rides an `Owned` so the unwind-temp window covers it: the runtime
  // entry is not nothrow, so this is an invoke wherever a cleanup pad is live.
  Owned emit_explicit_drop(llvm::Value* receiver) {
    Owned recv = own(receiver);
    emit_call(module_->getOrInsertFunction(rt::explicit_drop,
                                           builder_.getVoidTy(),
                                           builder_.getInt8Ty(),
                                           builder_.getInt64Ty()),
              {extract_tag(recv.borrow()), extract_data(recv.borrow())});
    recv.drop();
    return own(make_nil());
  }

  // Whether an Object receiver is a closed builtin namespace. Valid only where
  // the tag is already known to be TAG_OBJECT — it probes the pointer. A
  // namespace resolves every member name itself (as a member, or as the
  // property read's AttributeError), so both backends' UFCS gates count one as
  // having the property.
  llvm::Value* emit_object_is_namespace(llvm::Value* data,
                                        const char* name = "is.ns") {
    return builder_.CreateICmpNE(
        emit_call(module_->getFunction(rt::is_namespace), {data}, name),
        builder_.getInt8(0));
  }



  // Publish the call-site + per-argument positions the callee reads at
  // runtime (set_call_site / set_arg_pos thread-locals): the multifn
  // dispatcher's DispatchError, _jit_ns_method_trampoline, and the user-fn
  // prologue's typed-param snapshot (culebra_runtime_param_pos) all resolve
  // their report position from these. Must be emitted after every argument
  // expression has compiled — an argument's own inner calls clobber the
  // thread-locals — and immediately before the dispatch.
  //
  // set_call_site resets the per-arg count to 0; a HOF callback reaches its
  // per-element call with it still 0 (the body-coercion path). Per-argument
  // positions are published by the lowering's own emitter, which has the
  // bytecode's argument positions to hand (Lowering::emit_call_positions).
  void emit_call_position_publish() {
    emit_call(module_->getFunction(rt::set_call_site),
              {current_line_val(), current_column_val()});
  }

  Owned compile_function_call_raw(
      llvm::Value* callee, llvm::Value* selfVal,
      llvm::ArrayRef<llvm::Value*> userArgs,
      bool check_kw_only = false, bool allow_call_overload = true,
      bool own_self_on_error = false,
      bool own_args_on_error = false) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto tag = extract_tag(callee);
    auto isFunc = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC));

    auto fn = builder_.GetInsertBlock()->getParent();
    auto callBB = llvm::BasicBlock::Create(ctx_, "call.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "call.error", fn);
    builder_.CreateCondBr(isFunc, callBB, errorBB);

    // Cold path: the callee isn't a function. A callable class instance
    // (`obj(args)` where obj defines `__call__`, the twin of `__index__`)
    // dispatches to its `__call__` method here; everything else is a
    // type error. The hot path (callBB) is untouched. `allow_call_overload`
    // is cleared on the self-recursive __call__ invocation below so the
    // reconstruction emits only once (no compile-time recursion).
    builder_.SetInsertPoint(errorBB);
    // `callee` is often BORROWED out of `selfVal`'s own slots (`{m:
    // Adder.new(1)}.m(41)` reads `callee` off `selfVal`'s `m` slot), so pin
    // it with our own +1 before releasing `selfVal` below — otherwise the
    // release can free the object the `class_call_method`/`class_new_method`
    // probes below still need to dereference. None of the three sub-arms
    // (not-a-function, `__call__` overload, ctor dispatch) consumes the
    // ORIGINAL `selfVal` either, so release it here once, covering all
    // three uniformly instead of only the not-a-function tail (where it used
    // to sit, stranding it whenever the overload/ctor arm was taken — the
    // receiver leak `{m: Adder.new(1)}.m(41)` hit). Each arm below drops the
    // `callee` pin exactly once in turn (transferred into `self` on the
    // overload arm, an explicit release on the ctor/not-a-function arms).
    // Most callers pass a nil/borrowed `selfVal` and opt into neither this
    // nor the args release below; `selfVal` may be null (releasing nil is a
    // no-op). Gated on `allow_call_overload` (a C++-time constant, not a
    // runtime branch): the overload/ctor probes this pin protects only exist
    // on that arm, and the self-recursive `__call__` invocation below always
    // passes a confirmed-Function callee, so its own errorBB is dead code —
    // no sense emitting an unreachable pin/unpin pair into it.
    if (allow_call_overload) emit_value_retain(callee);
    if (own_self_on_error && selfVal) emit_value_release(selfVal);
    auto emit_not_function = [&] {
      // The callee is not callable (a method miss lowers to `call nil` —
      // `[1,2,3].first()`). No callee frame will run, so the args the caller
      // handed this call have no consumer and strand on the raise (`selfVal`
      // is already handled above). Args are gated separately by the caller:
      // the method-call fallthrough consumes them and nothing else releases
      // them, so it opts in; the unresolved-builtin path (`{a:1}.map(f)`)
      // already releases its receiver on this edge via its own machinery but
      // strands the callback arg, so it opts in too. Most callers pass no
      // args needing this or opt out.
      if (allow_call_overload)
        emit_value_release(callee);  // drop the pin taken at errorBB entry
      if (own_args_on_error)
        for (auto* a : userArgs) emit_value_release(a);
      emit_type_error_typed("Function", tag);
      builder_.CreateUnreachable();
    };
    llvm::Value* overloadResult = nullptr;
    llvm::BasicBlock* overloadEndBB = nullptr;
    llvm::Value* ctorResult = nullptr;
    llvm::BasicBlock* ctorEndBB = nullptr;
    if (allow_call_overload) {
      auto callMethod = emit_value_call(
          module_->getOrInsertFunction(
              rt::class_call_method, valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {tag, extract_data(callee)}, "call.method");
      auto isCallFn = builder_.CreateICmpEQ(
          extract_tag(callMethod), builder_.getInt8(TAG_FUNC));
      auto overloadBB = llvm::BasicBlock::Create(ctx_, "call.overload", fn);
      auto tryCtorBB = llvm::BasicBlock::Create(ctx_, "call.tryctor", fn);
      builder_.CreateCondBr(isCallFn, overloadBB, tryCtorBB);

      builder_.SetInsertPoint(overloadBB);
      // `callee` becomes `self`: the pin taken at errorBB entry above IS
      // that +1 (the recursive frame consumes it, the convention the normal
      // path shares — see the leak note in the kwargs path); no second
      // retain needed.
      // Late-merge arm raw: the value lives only on this arm's
      // runtime path and is re-owned when the builder revisits the arm to
      // declare it into call.phi below — holding it in an Owned across the
      // other arms' emission would spill a non-dominating SSA value.
      overloadResult = compile_function_call_raw(
          callMethod, callee, userArgs,
          /*check_kw_only=*/false, /*allow_call_overload=*/false)
          .consume_unchecked();
      overloadEndBB = builder_.GetInsertBlock();
      // Terminator added after callBB so all arms can merge at contBB.

      // Not a callable instance: try a class object — `C(args)` dispatches
      // to its `new`. The constructor builds its own instance, so it takes
      // no `self` (nil receiver, no retain), matching interp.
      builder_.SetInsertPoint(tryCtorBB);
      auto newMethod = emit_value_call(
          module_->getOrInsertFunction(
              rt::class_new_method, valueType_,
              builder_.getInt8Ty(), builder_.getInt64Ty()),
          {tag, extract_data(callee)}, "ctor.method");
      auto isNewFn = builder_.CreateICmpEQ(
          extract_tag(newMethod), builder_.getInt8(TAG_FUNC));
      auto ctorBB = llvm::BasicBlock::Create(ctx_, "call.ctor", fn);
      auto notFnBB = llvm::BasicBlock::Create(ctx_, "call.notfn", fn);
      builder_.CreateCondBr(isNewFn, ctorBB, notFnBB);

      builder_.SetInsertPoint(notFnBB);
      emit_not_function();

      builder_.SetInsertPoint(ctorBB);
      // The constructor takes no receiver, so the pin taken at errorBB entry
      // has no further use on this arm — drop it.
      emit_value_release(callee);
      // Late-merge arm raw — same contract as overloadResult above.
      ctorResult = compile_function_call_raw(
          newMethod, /*selfVal=*/nullptr, userArgs,
          /*check_kw_only=*/false, /*allow_call_overload=*/false)
          .consume_unchecked();
      ctorEndBB = builder_.GetInsertBlock();
    } else {
      emit_not_function();
    }

    builder_.SetInsertPoint(callBB);
    auto clsPtr = builder_.CreateIntToPtr(extract_data(callee), ptrTy);
    auto fnFieldPtr =
        builder_.CreateStructGEP(closureType_, clsPtr, 1, "fn.ptr");
    auto fnPtr = builder_.CreateLoad(ptrTy, fnFieldPtr, "fn");

    if (check_kw_only) {
      emit_call(
          module_->getFunction(rt::check_pos_count),
          {fnPtr,
           builder_.getInt64(static_cast<int64_t>(userArgs.size())),
           current_line_val(), current_column_val()});
    }

    // Build the args slab in the entry block (hoisted) so repeated
    // calls within a loop don't grow the stack frame per iteration.
    llvm::Value* argsPtr;
    if (userArgs.empty()) {
      argsPtr = llvm::ConstantPointerNull::get(ptrTy);
    } else {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      argsPtr = entryB.CreateAlloca(
          valueType_,
          builder_.getInt64(static_cast<int64_t>(userArgs.size())),
          "call.args");
      for (size_t i = 0; i < userArgs.size(); i++) {
        auto slot = builder_.CreateInBoundsGEP(
            valueType_, argsPtr,
            {builder_.getInt64(static_cast<int64_t>(i))});
        builder_.CreateStore(userArgs[i], slot);
      }
    }

    emit_call_position_publish();

    auto calleeType = jitFnCalleeType();
    // The result comes back through an out-pointer slot and the receiver `self`
    // crosses as two scalars (tag, data) — no by-value 16-byte aggregate crosses
    // the ABI in either direction, so the raw-IR lowering matches the Win64 C
    // ABI (which would pass an aggregate arg by-reference and return it via
    // sret). Hoist the result slot into the entry block like the args slab.
    llvm::Value* retSlot;
    {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      retSlot = entryB.CreateAlloca(valueType_, nullptr, "call.ret");
    }
    llvm::Value* selfNormal = selfVal ? selfVal : make_no_self();
    std::vector<llvm::Value*> args = {
        retSlot,
        clsPtr,
        extract_tag(selfNormal),
        extract_data(selfNormal),
        builder_.getInt64(static_cast<int64_t>(userArgs.size())),
        argsPtr};
    emit_call(calleeType, fnPtr, args);
    auto callResult = builder_.CreateLoad(valueType_, retSlot, "call.result");

    // No __call__ overload arm (recursive inner call): callBB is the
    // only producer, return directly and keep the original shape.
    if (!allow_call_overload) return own(callResult);

    // Merge the function-call result with the __call__ and constructor
    // dispatch results (three producers: callBB, overload arm, ctor arm).
    // The arm end blocks were left unterminated; each incoming is declared
    // as the builder revisits its arm to emit the branch.
    auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
    OwnedPhi callMerge(this, "call.phi");
    callMerge.add_incoming(own(callResult));
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(overloadEndBB);
    callMerge.add_incoming(own(overloadResult));
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(ctorEndBB);
    callMerge.add_incoming(own(ctorResult));
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
    return callMerge.finish(contBB);
  }

  // --- Execution ---

  static void exec(std::unique_ptr<llvm::LLVMContext> ctx,
                   std::unique_ptr<llvm::Module> mod,
                   bool fast_codegen = false) {
    using namespace llvm;
    auto jit = create_jit_instance(fast_codegen);
    // Reap what the script left running before `jit` (just above) tears down:
    // declared after `jit`, so on any exit path — including an uncaught throw
    // that skips past the script's own h.join() — this runs FIRST during
    // unwind. Every isolate's compiled body is the same JitClosure::fn_ptr
    // living in this LLJIT (sendable_jit.h); one still running when the
    // module's code memory is freed returns into memory the teardown just
    // unmapped. See isolate_teardown_join_hook (shared.h). A watch left open
    // is stopped for the reason script_teardown.h gives.
    struct ScriptTeardownGuard {
      ~ScriptTeardownGuard() {
        if (auto& fn = isolate_teardown_join_hook()) fn();
        fswatch::fs_watch_close_all();
      }
    } script_teardown_guard;
    orc::ThreadSafeContext tsctx(std::move(ctx));
    cantFail(jit->addIRModule(
        orc::ThreadSafeModule(std::move(mod), std::move(tsctx))));

    auto mainFn =
        cantFail(jit->lookup("__culebra_main")).toPtr<void (*)()>();
    // GAP5 detector fixture (debug/tests only): CULEBRA_GC_TEST_LEAK=1 mints
    // a handful of deliberately orphaned +1 arrays here, before the program
    // body runs, so the teardown audit has a guaranteed inflated-RC leak to
    // fire on. The smoke test's fires-on-leak case used to piggyback on a
    // real open product leak and needed a new fixture every time one closed;
    // this knob makes the detector's own regression test self-contained.
    // Run it under CULEBRA_GC_NEVER=1 for a deterministic fixture — without
    // it a mid-run collect may sweep the conservatively-dead orphans before
    // the audit (whether one runs depends on the program's allocation
    // volume). Eight of them: a single orphan can alias a stale stack slot
    // in the conservative scan and under-report (the same knee
    // leak_abort_suite.sh works around), so mint enough that the audit fires
    // deterministically.
    if (std::getenv("CULEBRA_GC_TEST_LEAK")) {
      for (int i = 0; i < 8; i++) (void)culebra_runtime_array_new();
    }
    // Force a collect while the LLJIT (and therefore every closure's
    // `fn_ptr`) is still alive — on BOTH success and throw paths. Without
    // this, leaked residue holding a `drop` survives until the heap is torn
    // down at process exit; by then the JIT module is gone and the drop
    // closure's native code is dangling, which segfaults. RAII guard covers
    // the throw-rethrow branch too (the catch below converts the
    // CulebraException to std::runtime_error, which unwinds before any plain
    // trailing statement would run).
    //
    // Drops are SUPPRESSED across this teardown collect: program exit is the
    // same point §16 leaks top-level bindings without firing `drop`, and an
    // orphan cycle surviving to exit is morally a top-level leftover (the
    // interp has no teardown collect, so it never finalizes these either —
    // cleanup is `with`/`defer`/explicit, not exit-time). Suppressing here
    // keeps the two backends symmetric at exit. The collect still SWEEPS, so
    // the residue's memory is reclaimed before module teardown (only the
    // finalize pass is skipped) — the dangling-native-code segfault above
    // cannot occur because no drop fires.
    struct CollectGuard {
      ~CollectGuard() {
        // GAP5: the top-level body has returned, so the mutator is quiescent
        // (no expression in flight) yet the module/namespace roots are still
        // wired — the one sound point to audit for inflated-RC leaks and abort
        // at their birth site (no-op unless CULEBRA_GC_LEAK_ABORT=1). Do it
        // BEFORE the reclaiming collect, while the leaked residue is still live.
        _gc_heap().maybe_audit_leaks();
        bool saved = _jit_drop_suppressed();
        _jit_drop_suppressed() = true;
        _gc_heap().collect();  // reclaim leaked residue before module teardown
        _jit_drop_suppressed() = saved;
      }
    } collect_guard;
    try {
      mainFn();
    } catch (const CulebraException& e) {
      // Format first, then consume the carrier's reference (the payload's
      // final +1 — releasing first would free it under the formatter).
      auto s = format_uncaught_throw(e);
      _culebra_value_release_impl(e.tag, e.data);
      // Run (best-effort) any top-level defers the uncaught throw
      // skipped, so the global defer stack is drained between runs.
      try {
        culebra_runtime_defer_run_to(0);
      } catch (...) {}
      throw std::runtime_error(std::move(s));
    } catch (culebra::CulebraError& e) {
      // Run (best-effort) any top-level defers the uncaught error skipped, so
      // the global defer stack is drained — mirrors the CulebraException path
      // above and the interpreter's flush_top_defers (e.g. a top-level `defer`
      // still fires on an uncaught Ctrl+C / runtime error).
      try { culebra_runtime_defer_run_to(0); } catch (...) {}
      // Uncaught runtime error (kind/message). Backfill a positionless one
      // from the published op position before it reaches main.cc's formatter;
      // aot_bootstrap does the same.
      _jit_backfill_op_pos(e);
      throw;
    }
  }
};

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
