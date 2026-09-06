#pragma once

// What an object file may not contain: an address belonging to the process
// that wrote it. `culebra build` and the stdlib preamble objects both outlive
// this process, so a compile-time pointer baked in as a literal is a dead
// address in the run that matters — a segfault where the page is unmapped,
// and silent garbage where it happens not to be.
//
// The rule is checked at the one exit both object emitters share
// (Lowering::emit_object_file) rather than trusted at each site that turns a
// compile-time value into module bytes, because a missed site produces no
// build error at all. Split out here so it can be tested without the
// compiler behind lowering.h; the caller supplies the addresses its own
// program holds.

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <unordered_set>

namespace culebra::vm {

// True when `mod` carries one of `live` as an integer constant, anywhere a
// constant can hide: a global's initializer, an instruction operand, and
// through aggregates and constant expressions. Sets `found` to the value.
inline bool module_bakes_address(const llvm::Module& mod,
                                 const std::unordered_set<uint64_t>& live,
                                 uint64_t& found) {
  if (live.empty()) return false;
  llvm::SmallPtrSet<const llvm::Constant*, 32> seen;
  auto scan = [&](const llvm::Value* v, auto&& self) -> bool {
    const auto* c = llvm::dyn_cast_or_null<llvm::Constant>(v);
    if (!c || !seen.insert(c).second) return false;
    if (const auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c)) {
      if (ci->getBitWidth() > 64 || !live.count(ci->getZExtValue()))
        return false;
      found = ci->getZExtValue();
      return true;
    }
    // An array of plain integers is folded into raw bytes, which are not
    // operands — walking operands alone reads a baked pointer as an empty
    // aggregate. This is the form a spec's baked values take, so the walk
    // watched the very site it was written for and saw nothing until this
    // arm existed (tests/aot_baked_address_test.cc pins it).
    if (const auto* cds = llvm::dyn_cast<llvm::ConstantDataSequential>(c)) {
      auto* el = cds->getElementType();
      if (!el->isIntegerTy() || el->getIntegerBitWidth() > 64) return false;
      for (unsigned i = 0, n = cds->getNumElements(); i < n; i++) {
        if (!live.count(cds->getElementAsInteger(i))) continue;
        found = cds->getElementAsInteger(i);
        return true;
      }
      return false;
    }
    for (const auto& op : c->operands())
      if (self(op.get(), self)) return true;
    return false;
  };
  for (const auto& g : mod.globals())
    if (g.hasInitializer() && scan(g.getInitializer(), scan)) return true;
  for (const auto& f : mod)
    for (const auto& bb : f)
      for (const auto& in : bb)
        for (const auto& op : in.operands())
          if (scan(op.get(), scan)) return true;
  return false;
}

}  // namespace culebra::vm
