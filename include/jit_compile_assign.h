#pragma once

#ifdef CULEBRA_JIT_ENABLED

// Complex-lvalue assignment codegen (JIT::compile_assign_complex).
//
// Compiler-layer fragment of jit.h, split out for readability. Holds the
// out-of-line definition of a `JIT` member declared in the class body;
// relies on jit.h's #include block and the complete `JIT` definition, and
// is included by jit.h after the class (not a standalone header).

namespace culebra {

// Complex lvalue: `obj.prop = e`, `arr[idx] = e`, and chains
// (`self.d[i] = v`). Rolls the receiver through one Owned handle across
// the intermediate postfixes, then dispatches the final INDEX / DOT set.
template <class CompileRhs>
JIT::Owned JIT::compile_assign_complex(const peg::Ast& ast,
                                       const culebra::AssignmentView& av,
                                       llvm::Value* rval, bool nil_coalesce,
                                       CompileRhs&& compile_rhs) {
  using namespace peg::udl;
  auto lvaloff = av.lvaloff;
  auto lvalcnt = av.lvalcnt;
  bool compound = av.compound;
  auto base_op = av.op_base;
  // Runtime-inserted slots default to mutable (matches interp's
  // eval_assign_complex and the existing spread-merge rule). Only affects
  // the new-slot append path — an existing slot's mut flag is untouched
  // (object_set_any / _jit_overwrite_slot ignore this parameter unless
  // is_init, which this call site never sets).
  bool mut = true;
  // A write-context read of an Array element (`a[i] += v`, `a[i] ??= v`)
  // uses array_set's bounds rule — a negative index is IndexError, never
  // the plain read's from-the-end normalization. Without this guard
  // array_get would read `a[-1]` as the last element and the compound
  // step (or the `??=` keep test) would run on it: interp says IndexError,
  // the JIT used to say the step's TypeError — or nothing at all.
  auto guard_write_index = [&](llvm::Value* idx) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto negBB = llvm::BasicBlock::Create(ctx_, "widx.neg", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "widx.ok", fn);
    builder_.CreateCondBr(
        builder_.CreateICmpSLT(idx, builder_.getInt64(0)), negBB, okBB);
    builder_.SetInsertPoint(negBB);
    emit_throw_error("IndexError", "index out of range",
                     ast.nodes[lvaloff]->line, ast.nodes[lvaloff]->column);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
  };
  // Complex lvalue (obj.prop, arr[idx]). The receiver rolls through one
  // Owned handle (the compile_call idiom): each step borrows the current
  // +1 and move-assigning the step's fresh +1 releases the previous link
  // at the same insertion point the hand-placed releases used to occupy.
  // While a step compiles (its helpers may throw), the +1 stays registered
  // and the unwind-temp window releases it on the throw edge — a
  // bare intermediate receiver used to strand whenever a later step threw
  // (`a[i][j] = v` with an OOB inner index).
  Owned lvalOwned = compile(*ast.nodes[lvaloff]);

  // Process intermediate postfixes (all but the last)
  auto end = lvaloff + lvalcnt - 1;
  for (auto i = lvaloff + 1; i < end; i++) {
    const auto& postfix = *ast.nodes[i];
    switch (postfix.original_tag) {
      case "INDEX"_: {
        // compile_index_access (emit_point_index) returns +1-owned, so the
        // move-assign just releases the previous receiver — no re-retain.
        // The old swap_owned double-counted the object-index path (a fresh
        // SharedBuffer view leaked; a dict slot's refcount inflated); the
        // array path is now retained inside emit_point_index so this stays
        // balanced.
        lvalOwned = compile_index_access(postfix, lvalOwned.borrow());
        break;
      }
      case "DOT"_: {
        // Intermediate `.prop` in a chain like `self.d[i] = v`: read the
        // property (borrowed) and promote to +1, mirroring the INDEX case
        // so the trailing release doesn't underflow the receiver.
        // swap_owned retains the property before releasing the receiver,
        // absorbing the receiver's +1 (consumed into the same call).
        auto prop = emit_property_get(lvalOwned.borrow(),
                                         std::string(postfix.token));
        emit_value_swap_owned(prop, lvalOwned.consume());
        lvalOwned = own(prop);
        break;
      }
      case "ARGUMENTS"_: {
        // Borrowed-callee dispatch; the move-assign releases the owned
        // temp after (see the compile_call ARGUMENTS note).
        lvalOwned = compile_function_call(postfix, lvalOwned.borrow());
        break;
      }
      default:
        throw std::runtime_error(
            "complex lvalue path not supported in JIT");
    }
  }

  // Final postfix - do the assignment. The receiver `lval` is +1 owned and
  // released on the normal path at each case's merge; guard it so an
  // assignment-error throw — ImmutableError on an immutable slot or a
  // Shared.new view (`o.a = v` on a literal, `s.a = v`), a KeyError / OOB /
  // type error from the get/set helpers — releases it on the unwind edge
  // instead of stranding it. The guard fires only on the throw edge, so it
  // does not double-release the normal-path `emit_value_release(lval)`.
  // The raw deliberately crosses the per-case dispatch blocks below with the
  // guard as its sole unwind-edge releaser — hence unchecked.
  llvm::Value* lval = lvalOwned.consume_unchecked();
  ThrowGuard lval_guard(this, {lval});
  const auto& finalPostfix = *ast.nodes[end];
  switch (finalPostfix.original_tag) {
    case "INDEX"_: {
      auto ptrTy = llvm::PointerType::get(ctx_, 0);
      auto tag = extract_tag(lval);
      auto fn = builder_.GetInsertBlock()->getParent();
      // The out-param slots for every read-back below live in the entry
      // block: a compound or `??=` index assignment is ordinary loop-body
      // code, and an alloca left in place would grow the frame once per
      // iteration until the stack runs out (~2M rounds on 8 MB).
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      if (nil_coalesce) {
        // `lval[k] ??= v`: own dispatch (Array element / Object key) —
        // `rval` is null here (the RHS is compiled lazily below, only on
        // the branch that needs it), so this cannot share the
        // compound/plain dispatch below, which assumes a real `rval`.
        auto ncArrBB = llvm::BasicBlock::Create(ctx_, "nc.arr", fn);
        auto ncObjBB = llvm::BasicBlock::Create(ctx_, "nc.obj", fn);
        auto ncErrBB = llvm::BasicBlock::Create(ctx_, "nc.err", fn);
        auto ncMergeBB = llvm::BasicBlock::Create(ctx_, "nc.merge", fn);
        auto ncIsArr = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
        auto ncChkObjBB = llvm::BasicBlock::Create(ctx_, "nc.chk_obj", fn);
        builder_.CreateCondBr(ncIsArr, ncArrBB, ncChkObjBB);
        builder_.SetInsertPoint(ncChkObjBB);
        auto ncIsObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
        auto ncObjKindBB = llvm::BasicBlock::Create(ctx_, "nc.obj_kind", fn);
        builder_.CreateCondBr(ncIsObj, ncObjKindBB, ncErrBB);

        builder_.SetInsertPoint(ncErrBB);
        // Interp reports every non-indexable assignment target as "expected
        // Array" (the read path's wording), even though Object works too.
        emit_type_error_typed("Array", tag);
        builder_.CreateUnreachable();

        // FixedArray view / SharedBuffer / Shared.new receivers are all
        // TAG_OBJECT under the hood but reject `??=` before reaching the
        // plain-dict logic below — mirrors the interp's is_fixed_array_view
        // / is_shared_val_view / is_shared_buffer guards ahead of its
        // Object handling. One call classifies all three kinds (plus
        // packed view, unused on this path) instead of three chained
        // is_* calls — see nc_receiver_kind.
        builder_.SetInsertPoint(ncObjKindBB);
        auto objData = extract_data(lval);
        auto ncKind = emit_call(
            module_->getOrInsertFunction(rt::nc_receiver_kind,
                                         builder_.getInt8Ty(),
                                         builder_.getInt64Ty()),
            {objData}, "nc.kind");
        auto favErrBB = llvm::BasicBlock::Create(ctx_, "nc.faverr", fn);
        auto svErrBB = llvm::BasicBlock::Create(ctx_, "nc.sverr", fn);
        auto sbErrBB = llvm::BasicBlock::Create(ctx_, "nc.sberr", fn);
        auto ncPlainBB = llvm::BasicBlock::Create(ctx_, "nc.plain", fn);
        auto ncKindSw = builder_.CreateSwitch(ncKind, ncPlainBB, 3);
        ncKindSw->addCase(builder_.getInt8(1), favErrBB);
        ncKindSw->addCase(builder_.getInt8(2), svErrBB);
        ncKindSw->addCase(builder_.getInt8(3), sbErrBB);

        // The FixedArray and Shared-view rejects are positionless in the
        // interp (statement backfill); only the SharedBuffer wording below
        // carries the subscript's own position there.
        builder_.SetInsertPoint(favErrBB);
        emit_throw_error("TypeError",
            "`?" "?=` is not supported on a FixedArray element",
            ast.line, ast.column);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(svErrBB);
        emit_throw_error("ImmutableError", "Shared values are immutable",
                         ast.line, ast.column);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(sbErrBB);
        emit_throw_error("TypeError",
            "cannot assign to a SharedBuffer element directly; "
            "set fields via buf[i].field = value",
            finalPostfix.line, finalPostfix.column);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(ncPlainBB);
        builder_.CreateBr(ncObjBB);

        OwnedPhi ncMerge(this, "nc.r");

        // Array element: replaces a nil element in place. No auto-extend
        // — an out-of-range index still raises IndexError via array_get.
        builder_.SetInsertPoint(ncArrBB);
        {
          auto arrPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
          auto idxVal = compile(finalPostfix).consume();
          auto idx = value_to_long(idxVal);
          guard_write_index(idx);
          auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr,
                                            "ncidx.out.tag");
          auto outData = entryB.CreateAlloca(builder_.getInt64Ty(), nullptr,
                                             "ncidx.out.data");
          emit_call(
              module_->getOrInsertFunction(
                  rt::array_get, builder_.getVoidTy(), ptrTy,
                  builder_.getInt64Ty(), ptrTy, ptrTy,
                  builder_.getInt64Ty(), builder_.getInt64Ty()),
              {arrPtr, idx, outTag, outData, current_line_val(),
               current_column_val()});
          auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
          auto curData = builder_.CreateLoad(builder_.getInt64Ty(), outData);
          llvm::Value* cur = make_value(curTag, curData);
          auto isNil = builder_.CreateICmpEQ(curTag, builder_.getInt8(TAG_NIL));
          auto arrAssignBB = llvm::BasicBlock::Create(ctx_, "nc.arr.assign", fn);
          auto arrKeepBB = llvm::BasicBlock::Create(ctx_, "nc.arr.keep", fn);
          builder_.CreateCondBr(isNil, arrAssignBB, arrKeepBB);

          builder_.SetInsertPoint(arrKeepBB);
          // array_get returns a +0 borrow; retain for the merge result.
          emit_value_retain(cur);
          ncMerge.add_incoming(own(cur));
          builder_.CreateBr(ncMergeBB);

          builder_.SetInsertPoint(arrAssignBB);
          llvm::Value* newVal = compile_rhs();  // +1, lazily emitted here (pinned -> raw immediately)
          emit_call(
              module_->getOrInsertFunction(
                  rt::array_set, builder_.getVoidTy(), ptrTy,
                  builder_.getInt64Ty(), builder_.getInt8Ty(),
                  builder_.getInt64Ty(), builder_.getInt64Ty(),
                  builder_.getInt64Ty()),
              {arrPtr, idx, extract_tag(newVal), extract_data(newVal),
               current_line_val(), current_column_val()});
          emit_value_retain(newVal);
          ncMerge.add_incoming(own(newVal));
          builder_.CreateBr(ncMergeBB);
        }

        // Object key: culebra_runtime_object_get_for_coalesce reports a
        // plain-dict miss as not-found (rather than throwing KeyError,
        // matching a plain `o[k]` read) and still consults a class
        // instance's __index__. A miss or an existing nil value writes
        // `v` back through the same object_set_any codepath the plain
        // `o[k] = v` case below uses (mut-checked on an existing slot,
        // mutable-by-default on insert per Stage 1, __setindex__-routed
        // for a class instance without an own slot).
        builder_.SetInsertPoint(ncObjBB);
        {
          auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
          Owned keyO = compile(finalPostfix);
          emit_value_retain(keyO.borrow());
          auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr,
                                            "ncobj.out.tag");
          auto outData = entryB.CreateAlloca(builder_.getInt64Ty(), nullptr,
                                             "ncobj.out.data");
          auto found = emit_call(
              module_->getOrInsertFunction(
                  rt::object_get_for_coalesce, builder_.getInt1Ty(), ptrTy,
                  builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy, ptrTy,
                  builder_.getInt64Ty(), builder_.getInt64Ty()),
              {objPtr, extract_tag(keyO.borrow()), extract_data(keyO.borrow()),
               outTag, outData, current_line_val(), current_column_val()},
              "nc.found");
          auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
          auto curData = builder_.CreateLoad(builder_.getInt64Ty(), outData);
          llvm::Value* cur = make_value(curTag, curData);
          auto isNilTag =
              builder_.CreateICmpEQ(curTag, builder_.getInt8(TAG_NIL));
          auto notFound = builder_.CreateNot(found);
          auto needsWrite = builder_.CreateOr(notFound, isNilTag);
          auto objAssignBB = llvm::BasicBlock::Create(ctx_, "nc.obj.assign", fn);
          auto objKeepBB = llvm::BasicBlock::Create(ctx_, "nc.obj.keep", fn);
          builder_.CreateCondBr(needsWrite, objAssignBB, objKeepBB);

          builder_.SetInsertPoint(objKeepBB);
          // Found and non-nil: `cur` is +1 (object_get_for_coalesce
          // matches object_get_any's contract). keyO's ref is unused on
          // this path.
          emit_value_release(keyO.borrow());
          ncMerge.add_incoming(own(cur));
          builder_.CreateBr(ncMergeBB);

          builder_.SetInsertPoint(objAssignBB);
          // Not found (cur is nil, a no-op release) or found holding an
          // existing +1 nil value — either way `cur` is discarded.
          emit_value_release(cur);
          llvm::Value* newValObj = compile_rhs();  // +1, lazily emitted here (pinned -> raw immediately)
          auto keyVal = keyO.consume();
          emit_call(
              module_->getOrInsertFunction(
                  rt::object_set_any, builder_.getVoidTy(), ptrTy,
                  builder_.getInt8Ty(), builder_.getInt64Ty(),
                  builder_.getInt1Ty(), builder_.getInt8Ty(),
                  builder_.getInt64Ty(), builder_.getInt64Ty(),
                  builder_.getInt64Ty(), builder_.getInt1Ty()),
              {objPtr, extract_tag(keyVal), extract_data(keyVal),
               builder_.getInt1(mut), extract_tag(newValObj),
               extract_data(newValObj),
               builder_.getInt64(static_cast<int64_t>(ast.nodes[lvaloff]->line)),
               builder_.getInt64(
                   static_cast<int64_t>(ast.nodes[lvaloff]->column)),
               builder_.getInt1(false)});
          emit_value_retain(newValObj);
          ncMerge.add_incoming(own(newValObj));
          builder_.CreateBr(ncMergeBB);
        }

        builder_.SetInsertPoint(ncMergeBB);
        Owned ncResult = ncMerge.finish(ncMergeBB);
        emit_value_release(lval);
        return ncResult;
      }
      // Dispatch on receiver type. Array + Object support both plain
      // and compound assignment; the Object compound path uses
      // object_get_any (throws KeyError on missing) and routes the
      // updated value back through object_set_any (mut-checked).
      auto arrBB = llvm::BasicBlock::Create(ctx_, "set.arr", fn);
      auto objBB = llvm::BasicBlock::Create(ctx_, "set.obj", fn);
      auto errBB = llvm::BasicBlock::Create(ctx_, "set.err", fn);
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "set.merge", fn);
      auto isArr =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
      auto chkObjBB =
          llvm::BasicBlock::Create(ctx_, "set.chk_obj", fn);
      builder_.CreateCondBr(isArr, arrBB, chkObjBB);
      builder_.SetInsertPoint(chkObjBB);
      auto isObj =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
      builder_.CreateCondBr(isObj, objBB, errBB);

      builder_.SetInsertPoint(errBB);
      // Same wording as the read path and interp: "expected Array".
      emit_type_error_typed("Array", tag);
      builder_.CreateUnreachable();

      // Array path
      builder_.SetInsertPoint(arrBB);
      auto arrPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
      auto idxVal = compile(finalPostfix).consume();
      auto idx = value_to_long(idxVal);
      llvm::Value* to_store_arr = rval;
      if (compound) {
        guard_write_index(idx);
        auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(),
                                          nullptr, "cidx.out.tag");
        auto outData = entryB.CreateAlloca(builder_.getInt64Ty(),
                                           nullptr, "cidx.out.data");
        emit_call(
            module_->getOrInsertFunction(
                rt::array_get, builder_.getVoidTy(), ptrTy,
                builder_.getInt64Ty(), ptrTy, ptrTy,
                builder_.getInt64Ty(), builder_.getInt64Ty()),
            {arrPtr, idx, outTag, outData, current_line_val(),
             current_column_val()});
        auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
        auto curData = builder_.CreateLoad(builder_.getInt64Ty(), outData);
        llvm::Value* cur = make_value(curTag, curData);
        // array_get returns a +0 borrow; emit_arith_step does not consume
        // operands; the Tensor in-place path retains lhs itself before
        // returning. So no retain/release is needed on `cur`.
        to_store_arr = emit_arith_step(cur, rval, base_op, /*inplace=*/true);
        emit_value_release(rval);
      }
      emit_call(
          module_->getOrInsertFunction(
              rt::array_set, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty()),
          {arrPtr, idx, extract_tag(to_store_arr),
           extract_data(to_store_arr), current_line_val(),
           current_column_val()});
      // `to_store_arr`'s +1 (rval directly for plain set, or the
      // arith-step result for compound) is consumed by array_set;
      // re-retain so the merge sees a +1 result. own() marks the minted
      // +1 as this arm's merge contribution (the SSA value predates the
      // arm, so the produced-here check doesn't apply).
      emit_value_retain(to_store_arr);
      OwnedPhi setMerge(this, "set.r");
      setMerge.add_incoming(own(to_store_arr));
      builder_.CreateBr(mergeBB);

      // Object path: see culebra_runtime_object_set_any. Compound
      // form reads the current value first via object_get_any (which
      // throws KeyError on a missing slot, matching interp's
      // `obj.has(key)` precondition) and applies the arith step in
      // place before writing back.
      builder_.SetInsertPoint(objBB);
      auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
      // The key rides the compound read-modify sequence in an Owned: it
      // crosses the object_get_any invoke (KeyError on a missing slot)
      // and the arith step (a user __add__ may throw), and a bare +1
      // used to strand on both edges. The window spills it around
      // each call; it is consumed at the object_set_any handoff below.
      Owned keyO = compile(finalPostfix);
      llvm::Value* to_store_obj = rval;
      if (compound) {
        // object_get_any and object_set_any both consume the caller's
        // +1 to the key. Retain once up front so each call sees a
        // separate +1 (the minted ref feeds object_get_any; keyO keeps
        // the object_set_any ref).
        emit_value_retain(keyO.borrow());
        auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(),
                                          nullptr, "cobj.out.tag");
        auto outData = entryB.CreateAlloca(builder_.getInt64Ty(),
                                           nullptr, "cobj.out.data");
        emit_call(
            module_->getOrInsertFunction(
                rt::object_get_any, builder_.getVoidTy(), ptrTy,
                builder_.getInt8Ty(), builder_.getInt64Ty(), ptrTy,
                ptrTy, builder_.getInt64Ty(), builder_.getInt64Ty(),
                builder_.getInt1Ty()),
            {objPtr, extract_tag(keyO.borrow()), extract_data(keyO.borrow()),
             outTag, outData, current_line_val(), current_column_val(),
             // Compound-assign target is borrowed here — do not release it.
             builder_.getInt1(false)});
        auto curTag = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
        auto curData =
            builder_.CreateLoad(builder_.getInt64Ty(), outData);
        llvm::Value* cur = make_value(curTag, curData);
        to_store_obj =
            emit_arith_step(cur, rval, base_op, /*inplace=*/true);
        emit_value_release(rval);
        emit_value_release(cur);
      }
      auto keyVal = keyO.consume();
      emit_call(
          module_->getOrInsertFunction(
              rt::object_set_any, builder_.getVoidTy(), ptrTy,
              builder_.getInt8Ty(), builder_.getInt64Ty(),
              builder_.getInt1Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty(), builder_.getInt1Ty()),
          {objPtr, extract_tag(keyVal), extract_data(keyVal),
           builder_.getInt1(mut), extract_tag(to_store_obj),
           extract_data(to_store_obj),
           // Point an ImmutableError at the assignment target's start
           // (`p` in `p[k] = v`), matching interp — not the subscript.
           builder_.getInt64(static_cast<int64_t>(ast.nodes[lvaloff]->line)),
           builder_.getInt64(
               static_cast<int64_t>(ast.nodes[lvaloff]->column)),
           // Post-construction subscript-set honors the immutable flag.
           builder_.getInt1(false)});
      // object_set_any consumed to_store_obj's +1; re-retain for the
      // merge so callers see a +1 result.
      emit_value_retain(to_store_obj);
      setMerge.add_incoming(own(to_store_obj));
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      Owned result = setMerge.finish(mergeBB);
      emit_value_release(lval);
      return result;
    }
    case "DOT"_: {
      auto ptrTy = llvm::PointerType::get(ctx_, 0);
      auto tag = extract_tag(lval);
      auto isObj =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
      auto fn = builder_.GetInsertBlock()->getParent();
      auto okBB = llvm::BasicBlock::Create(ctx_, "propset.ok", fn);
      auto errBB = llvm::BasicBlock::Create(ctx_, "propset.err", fn);
      builder_.CreateCondBr(isObj, okBB, errBB);

      builder_.SetInsertPoint(errBB);
      // The read path's wording (interp's to_object), even though only
      // Object is writable — matches the interp's write reject.
      emit_type_error_typed("Object, Array, or Tensor", tag);
      builder_.CreateUnreachable();

      builder_.SetInsertPoint(okBB);
      auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
      auto name = std::string(finalPostfix.token);
      if (nil_coalesce) {
        // `o.k ??= v`: matches a plain `o.k` read (emit_property_get
        // returns nil on a missing property, unlike the `compound`
        // branch's stricter object_has-or-AttributeError below). On a
        // nil result, evaluate and write `v` through the same
        // emit_object_set the plain `o.k = v` case below uses (mut-
        // checked on an existing slot, mutable-by-default on insert per
        // Stage 1).
        // Shared.new views reject `??=` unconditionally, before even
        // attempting a read (matches the interp's is_shared_val_view
        // guard, which fires ahead of every DOT write form) — otherwise
        // a frozen field holding a non-nil value would short-circuit
        // silently instead of raising ImmutableError like every other
        // write to a Shared.new view. A @packable packed view rejects
        // `??=` outright too (matches the interp) — a packed scalar has
        // no `nil` sentinel to coalesce against, unlike a full Object
        // property. One call classifies both kinds (plus FixedArray
        // view / SharedBuffer, unreachable for a `.field` receiver)
        // instead of two chained is_* calls — see nc_receiver_kind.
        auto ncDotKind = emit_call(
            module_->getOrInsertFunction(rt::nc_receiver_kind,
                                         builder_.getInt8Ty(),
                                         builder_.getInt64Ty()),
            {extract_data(lval)}, "ncprop.kind");
        auto svDotErrBB = llvm::BasicBlock::Create(ctx_, "ncprop.sverr", fn);
        auto packedErrBB = llvm::BasicBlock::Create(ctx_, "ncprop.packederr", fn);
        auto ncDotPlainBB = llvm::BasicBlock::Create(ctx_, "ncprop.plain", fn);
        auto ncDotKindSw = builder_.CreateSwitch(ncDotKind, ncDotPlainBB, 2);
        ncDotKindSw->addCase(builder_.getInt8(2), svDotErrBB);
        ncDotKindSw->addCase(builder_.getInt8(4), packedErrBB);

        // Both rejects are positionless in the interp (the eval wrapper
        // backfills the statement), so anchor at the statement, not the
        // property token.
        builder_.SetInsertPoint(svDotErrBB);
        emit_throw_error("ImmutableError", "Shared values are immutable",
                         ast.line, ast.column);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(packedErrBB);
        emit_throw_error("TypeError",
            "`?" "?=` is not supported on a packed field",
            ast.line, ast.column);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(ncDotPlainBB);
        // The interp's `??=` rejects a namespace's unknown member at the
        // DOT node (reject_namespace_write, the write-context anchor),
        // not the chain head a plain read reports; pin the read there.
        auto sav_l = current_line_, sav_c = current_column_;
        if (finalPostfix.line) {
          current_line_ = finalPostfix.line;
          current_column_ = finalPostfix.column;
        }
        auto cur = emit_property_get(lval, name);  // +0 borrowed
        current_line_ = sav_l;
        current_column_ = sav_c;
        auto isNil =
            builder_.CreateICmpEQ(extract_tag(cur), builder_.getInt8(TAG_NIL));
        auto ncAssignBB = llvm::BasicBlock::Create(ctx_, "ncprop.assign", fn);
        auto ncKeepBB = llvm::BasicBlock::Create(ctx_, "ncprop.keep", fn);
        auto ncMergeBB = llvm::BasicBlock::Create(ctx_, "ncprop.merge", fn);
        builder_.CreateCondBr(isNil, ncAssignBB, ncKeepBB);

        OwnedPhi ncMerge(this, "ncprop.r");

        builder_.SetInsertPoint(ncKeepBB);
        emit_value_retain(cur);  // +0 borrow -> +1 for the merge result
        ncMerge.add_incoming(own(cur));
        builder_.CreateBr(ncMergeBB);

        builder_.SetInsertPoint(ncAssignBB);
        llvm::Value* newVal = compile_rhs();  // +1 (pinned -> raw immediately)
        emit_value_retain(newVal);  // balances emit_object_set's consume
        emit_object_set(objPtr, name, mut, extract_tag(newVal),
                        extract_data(newVal));
        ncMerge.add_incoming(own(newVal));
        builder_.CreateBr(ncMergeBB);

        builder_.SetInsertPoint(ncMergeBB);
        Owned ncResult = ncMerge.finish(ncMergeBB);
        emit_value_release(lval);
        return ncResult;
      }
      // For compound (`o.x += rhs`), read current → apply op → write back.
      llvm::Value* to_store = rval;
      if (compound) {
        // A Shared.new view is immutable on every write surface, and the
        // interp checks that ahead of its find_prop — so the missing-
        // property pre-check below must not turn the reject into
        // "missing property". Positionless in the interp: anchor at the
        // statement. (A @packable packed view stays on the pre-check
        // path — its fields resolve through object_has like a plain
        // property, matching the interp's packed_view_get/set arm.)
        auto cKind = emit_call(
            module_->getOrInsertFunction(rt::nc_receiver_kind,
                                         builder_.getInt8Ty(),
                                         builder_.getInt64Ty()),
            {extract_data(lval)}, "cprop.kind");
        auto csvErrBB = llvm::BasicBlock::Create(ctx_, "cprop.sverr", fn);
        auto cchkBB = llvm::BasicBlock::Create(ctx_, "cprop.chk", fn);
        builder_.CreateCondBr(
            builder_.CreateICmpEQ(cKind, builder_.getInt8(2)), csvErrBB,
            cchkBB);
        builder_.SetInsertPoint(csvErrBB);
        emit_throw_error("ImmutableError", "Shared values are immutable",
                         ast.line, ast.column);
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(cchkBB);
        // Pre-check that the property exists. Without this the
        // JIT would read a missing slot as nil and then `emit_arith_step`
        // would throw TypeError ("nil + N"), whereas the interp throws
        // AttributeError with a more specific message. Match interp.
        auto namePtr = get_or_create_global_str(name, ".propname");
        auto has = emit_call(
            module_->getOrInsertFunction(rt::object_has,
                                         builder_.getInt1Ty(),
                                         ptrTy, ptrTy),
            {objPtr, namePtr}, "propset.has");
        auto hasBB = llvm::BasicBlock::Create(ctx_, "propset.has", fn);
        auto missBB = llvm::BasicBlock::Create(ctx_, "propset.miss", fn);
        builder_.CreateCondBr(has, hasBB, missBB);

        builder_.SetInsertPoint(missBB);
        emit_call(
            module_->getOrInsertFunction(rt::compound_missing_property,
                                         builder_.getVoidTy(),
                                         builder_.getInt64Ty(),
                                         builder_.getInt64Ty()),
            {builder_.getInt64(finalPostfix.line),
             builder_.getInt64(finalPostfix.column)});
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(hasBB);
        // emit_property_get returns +0 borrowed (no slot retain),
        // so cur does not need a matching release; only rval does.
        auto cur = emit_property_get(lval, name);
        to_store = emit_arith_step(cur, rval, base_op, /*inplace=*/true);
        emit_value_release(rval);
      } else {
        // Plain `o.k = v` on a closed namespace: an absent own member
        // isn't a legitimate extension point (mirrors interp's
        // is_namespace branch in eval_assign_complex) — check ahead of
        // object_set so a typo can't mint a phantom member. `??=` needs
        // no counterpart: its current-value read already goes through
        // the checked read path (emit_property_get) before ever reaching
        // object_set.
        auto namePtr = get_or_create_global_str(name, ".propname");
        emit_call(
            module_->getOrInsertFunction(rt::check_namespace_write,
                                         builder_.getVoidTy(),
                                         ptrTy, ptrTy,
                                         builder_.getInt64Ty(),
                                         builder_.getInt64Ty()),
            {objPtr, namePtr, builder_.getInt64(finalPostfix.line),
             builder_.getInt64(finalPostfix.column)});
      }
      // Retain the result ref *before* the set: `object_set` consumes one
      // reference to `to_store`. For a normal slot that consume is a
      // transfer (the value lives on in the slot), but the @packable
      // packed-view path *releases* it — the enum/record value is copied
      // into the backing bytes and has no slot owner. Retaining after the
      // call would then touch freed memory, and the statement-level
      // release would double free. Retaining first balances both paths:
      // the set consumes the original ref, this retained ref is the
      // expression result.
      emit_value_retain(to_store);
      emit_object_set(objPtr, name, mut, extract_tag(to_store),
                      extract_data(to_store));
      emit_value_release(lval);  // release the lvalue's ref
      return own(to_store);
    }
    default:
      // Final postfix is a call (`f() = v`) — no storage to write. Rejected
      // pre-eval by lint::check_module on every backend; position omitted so
      // compile()'s wrapper backfills this node's line/col.
      throw culebra::CulebraError(
          "SyntaxError", "cannot assign to a function call result.");
  }
}

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
