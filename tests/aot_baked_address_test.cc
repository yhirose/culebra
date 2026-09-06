// The control for the object file's no-live-address rule
// (include/jit/baked_address.h). Without one the check has the failure mode
// it exists to prevent: it watches the site it was written for and reports
// nothing.
//
// That is not hypothetical. The first version of the walk followed operands
// only, and LLVM folds an array of plain integers into a ConstantDataArray,
// whose elements are raw bytes rather than operands — exactly the shape a
// spec's baked values take, so it passed a module carrying the bug verbatim.
// Every form below is one a lowering actually emits.
//
// Checks are explicit rather than `assert`: every lane builds Release, where
// an assert compiles out and a control that cannot fail is no control.
//
// Compile + run directly, e.g.:
//   clang++ -std=c++23 -O2 -Iinclude $(llvm-config --cxxflags --ldflags \
//     --libs core support) tests/aot_baked_address_test.cc -o /tmp/ba && /tmp/ba

#include "jit/baked_address.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include <cstdio>

using culebra::vm::module_bakes_address;

// A plausible heap address: big, and not a value any program would spell.
static constexpr uint64_t kLive = 0x6000027353c8ULL;
static constexpr uint64_t kOther = 0x600002735400ULL;

static int failures = 0;

static void check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "aot_baked_address_test FAIL: %s\n", what);
  failures++;
}

namespace {

llvm::Constant* i64(llvm::LLVMContext& ctx, uint64_t v) {
  return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), v);
}

llvm::Constant* i32(llvm::LLVMContext& ctx, uint64_t v) {
  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), v);
}

// The scan's answer for a module holding `init` as a private global.
bool bakes_global(llvm::LLVMContext& ctx, llvm::Constant* init,
                  uint64_t& found) {
  llvm::Module mod("probe", ctx);
  new llvm::GlobalVariable(mod, init->getType(), /*isConstant=*/true,
                           llvm::GlobalValue::PrivateLinkage, init, ".probe");
  return module_bakes_address(mod, {kLive}, found);
}

}  // namespace

int main() {
  llvm::LLVMContext ctx;
  uint64_t found = 0;

  // Nothing to look for: an empty set never reports.
  {
    llvm::Module mod("empty", ctx);
    check(!module_bakes_address(mod, {}, found), "empty set reports nothing");
  }

  // A bare i64 constant, the simplest form.
  found = 0;
  check(bakes_global(ctx, i64(ctx, kLive), found), "bare i64 caught");
  check(found == kLive, "bare i64 reports the value");
  check(!bakes_global(ctx, i64(ctx, kOther), found), "an unrelated i64 passes");

  auto* i64Ty = llvm::Type::getInt64Ty(ctx);

  // An array of i64 — folded to a ConstantDataArray, whose elements are raw
  // bytes rather than operands. THE regression this file exists for: it is
  // the form Op::FieldsInit's zeros array takes, {tag, data} pairs.
  {
    auto* arrTy = llvm::ArrayType::get(i64Ty, 4);
    auto* packed = llvm::ConstantArray::get(
        arrTy, {i64(ctx, 2), i64(ctx, 0), i64(ctx, 4), i64(ctx, kLive)});
    // The fold is what blinds an operand-only walk. If LLVM ever stops
    // folding, this case would pass for the wrong reason.
    check(llvm::isa<llvm::ConstantDataSequential>(packed),
          "an i64 array still folds to raw data");
    found = 0;
    check(bakes_global(ctx, packed, found), "i64 array caught");
    check(found == kLive, "i64 array reports the value");
    auto* clean = llvm::ConstantArray::get(
        arrTy, {i64(ctx, 2), i64(ctx, 0), i64(ctx, 4), i64(ctx, kOther)});
    check(!bakes_global(ctx, clean, found), "an unrelated i64 array passes");
  }

  // Nested in a struct, reached through operands.
  {
    auto* innerTy = llvm::StructType::get(ctx, llvm::ArrayRef<llvm::Type*>{i64Ty});
    auto* inner = llvm::ConstantStruct::get(innerTy, {i64(ctx, kLive)});
    auto* outerTy =
        llvm::StructType::get(ctx, llvm::ArrayRef<llvm::Type*>{innerTy});
    auto* outer = llvm::ConstantStruct::get(outerTy, {inner});
    found = 0;
    check(bakes_global(ctx, outer, found), "nested struct caught");
    check(found == kLive, "nested struct reports the value");
  }

  // Behind a constant expression, the form a folded inttoptr takes.
  {
    llvm::Module mod("expr", ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    new llvm::GlobalVariable(
        mod, ptrTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantExpr::getIntToPtr(i64(ctx, kLive), ptrTy), ".expr");
    found = 0;
    check(module_bakes_address(mod, {kLive}, found), "constant expr caught");
    check(found == kLive, "constant expr reports the value");
  }

  // An instruction operand rather than a global initializer.
  {
    llvm::Module mod("fn", ctx);
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(i64Ty, {i64Ty}, false),
        llvm::GlobalValue::ExternalLinkage, "f", mod);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    b.CreateRet(b.CreateAdd(fn->getArg(0), i64(ctx, kLive)));
    found = 0;
    check(module_bakes_address(mod, {kLive}, found), "instruction operand caught");
    check(found == kLive, "instruction operand reports the value");
  }

  // What a correct lowering emits: the bytes are module .rodata and the baked
  // value points at that global, so no address is ever a literal.
  {
    llvm::Module mod("clean", ctx);
    auto* bytes = llvm::ConstantDataArray::getString(ctx, "", /*AddNull=*/true);
    auto* strTy = llvm::StructType::get(ctx, {i64Ty, bytes->getType()});
    auto* str = new llvm::GlobalVariable(
        mod, strTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(strTy, {i64(ctx, 0), bytes}), ".str");
    llvm::Constant* idx[] = {i32(ctx, 0), i32(ctx, 1), i32(ctx, 0)};
    auto* data = llvm::ConstantExpr::getInBoundsGetElementPtr(strTy, str, idx);
    auto* zerosTy = llvm::ArrayType::get(i64Ty, 2);
    new llvm::GlobalVariable(
        mod, zerosTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(
            zerosTy,
            {i64(ctx, 4), llvm::ConstantExpr::getPtrToInt(data, i64Ty)}),
        ".zeros");
    check(!module_bakes_address(mod, {kLive}, found),
          "a module that owns its bytes passes");
  }

  if (failures) return 1;
  std::printf("aot_baked_address_test OK\n");
  return 0;
}
