#pragma once

#ifdef CULEBRA_JIT_ENABLED

// FUNCTION / METHOD / synthetic-ctor codegen (JIT::compile_fn_common).
//
// Compiler-layer fragment of jit.h, split out for readability. Holds the
// out-of-line definition of a `JIT` member declared in the class body;
// relies on jit.h's #include block and the complete `JIT` definition, and
// is included by jit.h after the class (not a standalone header).

namespace culebra {

// Emit the LLVM function for a FUNCTION / METHOD / synthetic-ctor AST.
// `info_key` matches what `analyze_fn_common` used, so `analysis_.func_info`
// lookup finds the free-var / captured-local sets.
inline JIT::Owned JIT::compile_fn_common(
    const peg::Ast* info_key, const peg::Ast* params_ast,
    std::shared_ptr<peg::Ast> body_ast, std::string_view returnType,
    std::string_view declName, llvm::Constant** outParamMeta,
    const std::vector<JitFieldInit>* field_inits, bool field_init_is_ctor) {
  using namespace llvm;
  using namespace peg::udl;
  auto ptrTy = PointerType::get(ctx_, 0);

  // Snapshot class_type_params_ for the *immediate* function's params
  // and clear the member so nested closures (lambdas / fn inside the
  // body) don't pick up the outer class's params — matches interp's
  // neutralize_type_params, which only touches the top-level method.
  // Restored on scope exit.
  auto active_class_type_params = std::move(class_type_params_);
  class_type_params_.clear();
  struct ClassParamsGuard {
    std::vector<std::string_view>& slot;
    std::vector<std::string_view> saved;
    ~ClassParamsGuard() { slot = std::move(saved); }
  } class_params_guard{class_type_params_,
                       std::vector<std::string_view>(active_class_type_params)};

  auto infoIt = analysis_.func_info.find(info_key);
  if (infoIt == analysis_.func_info.end()) {
    throw std::runtime_error("missing func_info for function");
  }
  FuncInfo& info = infoIt->second;

  // Snapshot outer mut / lazy flags for each captured free var *before*
  // we descend into the body. emit_closure_build later re-populates
  // these (also from the outer scope), but the inner body's free-var
  // bindings consume them during their own compilation, which happens
  // first — so the snapshot must be taken here.
  info.free_var_mut.assign(info.free_vars.size(), false);
  info.free_var_lazy.assign(info.free_vars.size(), false);
  for (size_t i = 0; i < info.free_vars.size(); i++) {
    auto outer_slot = lookup_var(info.free_vars[i]);
    if (outer_slot) {
      info.free_var_mut[i] = outer_slot->mut;
      info.free_var_lazy[i] = outer_slot->lazy || outer_slot->unbound_guard;
    }
  }

  std::vector<std::string> paramNames;
  // Owning std::string so lower_type_params (which returns a
  // fresh string) is safe to push; downstream consumers (emit_type_check,
  // paramTypeStrs) take string_view and accept either.
  std::vector<std::string> paramTypeNames;
  // Original declared annotation (pre-neutralization) — used by
  // introspection so `fn.params[i].type` surfaces `T` / `Array<T>`
  // for Generic class methods, not the rewritten `Any`.
  std::vector<std::string> paramDeclaredTypeNames;
  std::vector<const peg::Ast*> paramDefaults;
  std::vector<bool> paramMuts;
  // Parallel to paramNames: the destructuring pattern for a `fn ({a,b})`
  // param (nullptr for normal params). Unpacked at the prologue's end.
  std::vector<const peg::Ast*> paramPatterns;
  std::optional<size_t> firstDefaulted;
  std::optional<size_t> kwargsRestIdx;
  std::optional<size_t> firstKwOnlyIdx;
  // `*args` catch-all name (bound from the overflow Array below). It
  // must be the last parameter, so nothing follows it in paramNames
  // and the overflow boundary stays at declaredArity.
  std::optional<std::string> argsRestName;
  if (params_ast) for (auto& node : params_ast->nodes) {
    auto pv = culebra::view_parameter(*node);
    if (argsRestName) {
      throw culebra::CulebraError("SyntaxError",
          "'*args' must be the last parameter",
          static_cast<long>(node->line), static_cast<long>(node->column));
    }
    if (pv.is_args_rest) {
      if (firstKwOnlyIdx) {
        throw culebra::CulebraError("SyntaxError",
            "'*args' cannot follow a '*' separator",
            static_cast<long>(node->line), static_cast<long>(node->column));
      }
      argsRestName = std::string(pv.name);
      continue;  // not a positional slot; bound from overflow below
    }
    if (pv.is_kw_only_sep) {
      if (!firstKwOnlyIdx) firstKwOnlyIdx = paramNames.size();
      continue;
    }
    if (pv.is_kwargs_rest) {
      paramNames.push_back(std::string(pv.name));
      paramTypeNames.push_back({});
      paramDeclaredTypeNames.push_back({});
      paramDefaults.push_back(nullptr);
      paramMuts.push_back(false);
      paramPatterns.push_back(nullptr);
      kwargsRestIdx = paramNames.size() - 1;
      continue;
    }
    if (pv.pattern) {
      // Destructuring param: synthetic positional slot, unpacked below.
      paramNames.push_back(
          std::string(culebra::destructure_param_name(paramNames.size())));
      paramTypeNames.push_back({});
      paramDeclaredTypeNames.push_back({});
      paramDefaults.push_back(nullptr);
      paramMuts.push_back(false);
      paramPatterns.push_back(pv.pattern);
      continue;
    }
    // A required positional param after a defaulted one is a SyntaxError
    // (kw-only params, which follow `*`, are exempt) — interp rejects this
    // in its param builder; mirror it here so the JIT doesn't silently
    // accept `fn f(a = 1, b)`.
    if (!pv.default_value && firstDefaulted && !firstKwOnlyIdx) {
      throw culebra::CulebraError("SyntaxError", std::format(
          "non-default parameter '{}' follows a default parameter",
          std::string(pv.name)),
          static_cast<long>(pv.name_line), static_cast<long>(pv.name_col));
    }
    paramPatterns.push_back(nullptr);
    paramNames.push_back(std::string(pv.name));
    auto raw = std::string(pv.type_annotation);
    paramDeclaredTypeNames.push_back(raw);
    auto tname = raw;
    // Recursive rewrite: bare `T`, `Array<T>`, `T | Long`, etc. all
    // collapse to "Any" / canonical-rewritten. Uses the snapshot
    // from the immediate enclosing class so nested fns in the body
    // don't inherit the rewrite (matches interp).
    if (!active_class_type_params.empty() && !tname.empty()) {
      tname = culebra::lower_type_params(
          tname, active_class_type_params);
    }
    paramTypeNames.push_back(tname);
    paramDefaults.push_back(pv.default_value);
    paramMuts.push_back(pv.is_mut);
    if (pv.default_value && !firstDefaulted) {
      firstDefaulted = paramNames.size() - 1;
    }
  }

  auto fn = declare_closure_fn("__culebra_fn_", info.has_eh);

  CompilerStateSaver saver(*this);
  current_info_ = &info;
  current_return_type_ = returnType;

  auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
  builder_.SetInsertPoint(entryBB);
  push_scope();

  if (!returnType.empty()) emit_return_pos_snapshot();

  llvm::Value* fnMark = nullptr;
  // Establish a defer-stack mark whenever the body has *any* defer (fn-level
  // or inside a nested lexical scope / match arm), so `compile_return` /
  // `compile_break` / `compile_continue` run the still-pending defers on an
  // early exit. interp runs these via its scope-unwind catch-all; without
  // the mark the JIT would silently skip a nested scope's defer on `return`.
  if (info.has_any_defer) {
    fnMark = builder_.CreateCall(
        module_->getFunction(rt::defer_mark), {}, "fn.mark");
    current_fn_defer_mark_ = fnMark;
  }
  // Function-level cleanup ladder. Every throwing call in the frame that is
  // not already caught by a nested try/scope/loop cleanup unwinds into it; on
  // the throw path it runs the fn-level defers, releases the frame's owned
  // slots (the params region + the body's top-level locals — the body BLOCK
  // compiles into this frame scope, not a nested LEXICAL_SCOPE), and resolves
  // the escaped/cyclic drop remainder, matching the interpreter's scope-unwind
  // teardown. Synthesized ctor / multifn-dispatcher closures build their own
  // frames outside compile_fn_common and are unaffected.
  open_frame_cleanup_ladder();

  auto argIt = fn->arg_begin();
  current_sret_ = &*argIt++;  // __ret out-pointer (result slot)
  auto clsArg = &*argIt++;
  current_closure_arg_ = clsArg;
  auto selfTagArg = &*argIt++;
  auto selfDataArg = &*argIt++;
  auto nArgsArg = &*argIt++;
  auto argsArg = &*argIt++;
  // Reconstruct the receiver `self` from the two scalar ABI args. The
  // insert point is already at `entryBB`, so this emits into the prologue.
  llvm::Value* selfArg = make_value(selfTagArg, selfDataArg);

  // Bodies declared inside a class or trait are written against a receiver
  // (a detached static — `let f = C.mk; f()` — is the one shape that still
  // arrives without one, and folds to nil as it did before the sentinel).
  // Reads of `self` there skip the guard; every other body keeps the
  // sentinel for it to see.
  in_receiver_frame_ = info_key->tag == "METHOD"_ ||
                       info_key->tag == "TRAIT_METHOD"_ ||
                       field_inits != nullptr;
  if (in_receiver_frame_) {
    auto absent = builder_.CreateICmpEQ(
        selfTagArg, builder_.getInt8(TAG_NO_SELF), "self.absent");
    selfArg = builder_.CreateSelect(absent, make_nil(), selfArg, "self");
  }

  // Arity guard: matching the interpreter, it is an error to call
  // with fewer args than declared. Overflow is allowed (lands in
  // `__ARGS__`). Required-arity is the number of params WITHOUT a
  // default (leading portion, since defaults must be trailing).
  size_t declaredArity = paramNames.size();
  size_t requiredArity = firstDefaulted.value_or(declaredArity);
  // A `**rest` slot is never required: a call carrying no keyword content
  // skips the resolver entirely and lands here directly, and the interp
  // binds an empty Object there rather than reporting a missing argument.
  // The slot itself is filled below, from the overflow or as `{}`.
  if (kwargsRestIdx && *kwargsRestIdx < requiredArity)
    requiredArity = *kwargsRestIdx;
  // A destructuring parameter takes no default, so it is required wherever
  // it sits. lint rejects one after a defaulted parameter; this keeps the
  // prologue from reading an unfilled slab entry if that check is bypassed.
  for (size_t i = requiredArity; i < paramPatterns.size(); i++)
    if (paramPatterns[i]) requiredArity = i + 1;
  {
    auto need = builder_.getInt64(static_cast<int64_t>(requiredArity));
    auto tooFew = builder_.CreateICmpULT(nArgsArg, need);
    auto okBB = BasicBlock::Create(ctx_, "arity.ok", fn);
    auto errBB = BasicBlock::Create(ctx_, "arity.err", fn);
    builder_.CreateCondBr(tooFew, errBB, okBB);

    builder_.SetInsertPoint(errBB);
    // callee-cleans-on-direct-binding-error: this arity check runs in the
    // prologue *before* `self`/params enter the frame's cleanup scope, so
    // the ladder's base rung releases nothing on this throw. The +1s the
    // caller transferred — the receiver `self` and every passed positional
    // — would otherwise strand (a ctor receiver stranding on `C.new()`, a
    // method receiver on `obj.m()` with a missing arg). Release them here,
    // matching the in-body throw path where the slot cleanup does the same.
    // (For a ctor body, build_class_instance also holds a +1 it releases in
    // its own catch — this drops the slot-consume +1, so together the count
    // reaches zero exactly once.)
    emit_value_release(selfArg);
    emit_call(
        module_->getOrInsertFunction(
            rt::release_overflow_args, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {argsArg, builder_.getInt64(0), nArgsArg});
    // Constant char* array of declared param names so the runtime can name
    // the first missing slot ("missing required argument 'X'"). Matches
    // interp's bind_call_args message.
    std::vector<llvm::Constant*> nameConsts;
    nameConsts.reserve(paramNames.size());
    for (const auto& n : paramNames) {
      nameConsts.push_back(llvm::cast<llvm::Constant>(
          get_or_create_global_str(n, ".paramname")));
    }
    auto namesGlobal = build_str_ptr_array(nameConsts, ".paramnames");
    emit_call(
        module_->getOrInsertFunction(
            rt::arity_missing, builder_.getVoidTy(),
            ptrTy, builder_.getInt64Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {namesGlobal, nArgsArg,
         current_line_val(), current_column_val()});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
  }

  // `fn` = make_func(__cls__): calling it re-enters this fn with this
  // closure. make_func doesn't retain, so retain explicitly so the
  // owned `fn` slot has the +1 it will hand back at function exit.
  {
    auto fnVal = make_func(clsArg);
    emit_value_retain(fnVal);
    define_var("fn", make_stack_slot("fn", fnVal));
  }
  // Cache for the receiver-bound `fn` handle (interp parity: a method
  // call's `fn` is the bound wrapper, so an escaped handle keeps its
  // receiver). Built lazily on the first value-read of `fn`
  // (culebra_runtime_fn_handle); a direct `fn(...)` call bypasses it.
  // The owned slot releases the cached thunk on frame exit.
  if (info.uses_fn) {
    define_var("fn.bound", make_stack_slot("fn.bound", make_nil()));
  }

  // Lexical fallback: a frame whose body (or nested closure) reads
  // `self` captures the enclosing frame's self cell. A dynamic receiver
  // still wins — the merge picks the arg unless it is NO_SELF, mirroring
  // the interp, where a receiver call binds `self` in the callEnv and a
  // plain call falls through the def_env chain. Receiver frames never
  // carry the capture (analyze_fn_common strips it), so methods keep the
  // arg-only path below.
  auto selfFvIt =
      std::find(info.free_vars.begin(), info.free_vars.end(), "self");
  if (selfFvIt != info.free_vars.end()) {
    size_t selfIdx = selfFvIt - info.free_vars.begin();
    auto capturesFieldPtr =
        builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
    auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
    auto cellSlotPtr = builder_.CreateInBoundsGEP(
        ptrTy, capturesArr, {builder_.getInt64(selfIdx)}, "self.cellslot");
    auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, "self.cell");
    // The helper picks the receiver (its +1 transfers through) or the
    // captured cell's value (retained inside — the RC stays behind the
    // runtime seam, not a bare codegen retain).
    selfArg = emit_value_call(
        module_->getOrInsertFunction(rt::self_merge, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(), ptrTy),
        {extract_tag(selfArg), extract_data(selfArg), cellPtr}, "self");
  }

  // `self` is from arg (or the lexical merge above); if captured,
  // allocate cell. The slot owns a +1 either way.
  if (info.captured_locals.contains("self")) {
    define_var("self", make_cell_slot("self", selfArg));
  } else {
    define_var("self", make_stack_slot("self", selfArg));
  }

  // Free variables bound BEFORE params so that default expressions on
  // parameters can reference captured outer variables. These are
  // borrowed cell refs — the closure object owns them.
  for (size_t i = 0; i < info.free_vars.size(); i++) {
    const auto& fv = info.free_vars[i];
    // `self`'s capture feeds the lexical merge above; the merged value
    // lives in the frame's own self slot, never as a borrowed cell ref.
    if (fv == "self") continue;
    auto capturesFieldPtr =
        builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
    auto capturesArr =
        builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
    auto cellSlotPtr = builder_.CreateInBoundsGEP(
        ptrTy, capturesArr, {builder_.getInt64(i)}, "cell.slot");
    auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, fv + ".cell");
    llvm::IRBuilder<> entryB(entryBB, entryBB->begin());
    auto holder = entryB.CreateAlloca(ptrTy, nullptr, fv);
    builder_.CreateStore(cellPtr, holder);
    bool fv_mut = i < info.free_var_mut.size() ? info.free_var_mut[i] : false;
    VarSlot cap{VarSlot::Cell, holder, /*owned=*/false, fv_mut};
    // A capture of a lazy forward-ref cell guards reads for the unbound
    // sentinel — a call before the declaring statement ran is the
    // interp's NameError, not a nil placeholder flowing into the body.
    // UFCS candidates are excepted: their sentinel must decline the call
    // gate exactly like the nil an unreachable candidate binds.
    cap.unbound_guard = i < info.free_var_lazy.size() &&
                        info.free_var_lazy[i] &&
                        !info.optional_free_vars.contains(fv);
    define_var(fv, cap);
  }

  // The multifn body's own name: delivered by the dispatch
  // (culebra_runtime_multifn_self) rather than captured — a capture would
  // close the refcount ring cell → dispatcher → body → cell that only the
  // tracing backstop can reclaim (see FuncInfo::mf_self). A cell, so a
  // nested closure captures it like any local's; guarded, so a body handle
  // that escaped via `fn` and outlived its dispatcher reads as the
  // undeclared name's NameError. Bound with the captures, ahead of the
  // params, so a default expression can reference it.
  if (!info.mf_self.empty() &&
      (info.mf_self_used || info.captured_locals.contains(info.mf_self))) {
    auto selfFn = emit_value_call(
        module_->getOrInsertFunction(rt::multifn_self, valueType_, ptrTy),
        {clsArg}, info.mf_self);
    auto slot = make_cell_slot(info.mf_self, selfFn);
    slot.unbound_guard = true;
    define_var(info.mf_self, slot);
  }

  // Typed-param error positions resolve per call at runtime (interp
  // parity: a positional argument reports at its own expression, a
  // kwarg-/default-filled slot at the call site). The common case — no
  // earlier default — resolves on the type-check's FAILURE path
  // (culebra_runtime_type_check_param), keeping the success path at the
  // pre-existing one-call cost. From the first default (or `_` sink,
  // whose release can run a drop) onward, user code may run inside the
  // binding loop and clobber the position thread-locals before the
  // check, so those params snapshot their position eagerly here.
  size_t eagerPosFrom = paramNames.size();
  for (size_t i = 0; i < paramNames.size(); i++) {
    if (paramDefaults[i] || is_sink_name(paramNames[i])) {
      eagerPosFrom = i;
      break;
    }
  }
  std::vector<std::pair<llvm::Value*, llvm::Value*>> paramErrPos(
      paramNames.size(), {nullptr, nullptr});
  for (size_t i = eagerPosFrom; i < paramNames.size(); i++) {
    if (paramTypeNames[i].empty()) continue;
    auto packed = emit_call(
        module_->getOrInsertFunction(
            rt::param_pos, builder_.getInt64Ty(), builder_.getInt64Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {builder_.getInt64(static_cast<int64_t>(i)), current_line_val(),
         current_column_val()},
        paramNames[i] + ".errpos");
    // _jit_unpack_pos, emitted as IR because the value is only known at
    // runtime.
    paramErrPos[i] = {
        builder_.CreateLShr(packed, builder_.getInt64(32)),
        builder_.CreateAnd(packed, builder_.getInt64(0xffffffff))};
  }

  // Declared parameters: for non-defaulted params, load from args[i]
  // (the caller transferred each +1 into the slab). For defaulted
  // params, branch on `i < n_args && slab[i].tag != TAG_UNFILLED`:
  // either take the slab entry or compile the default expression
  // inline, then PHI-merge. TAG_UNFILLED is the kwargs-resolver's
  // middle-gap sentinel.
  for (size_t i = 0; i < paramNames.size(); i++) {
    const auto& name = paramNames[i];
    llvm::Value* argVal = nullptr;
    // The `**rest` slot binds like a defaulted param whose default is a
    // fresh empty Object: a call with no keyword content never reaches the
    // resolver, so the slot arrives unfilled (or past n_args entirely) and
    // the callee must still see a bound variable, as the interp's does.
    bool isKwRest = kwargsRestIdx && *kwargsRestIdx == i;
    if (!paramDefaults[i] && !isKwRest) {
      auto slotPtr = builder_.CreateInBoundsGEP(
          valueType_, argsArg, {builder_.getInt64(static_cast<int64_t>(i))},
          name + ".slot");
      argVal = builder_.CreateLoad(valueType_, slotPtr, name);
    } else {
      auto hasIdx = builder_.CreateICmpUGT(
          nArgsArg, builder_.getInt64(static_cast<int64_t>(i)),
          name + ".has");
      auto takeBB = BasicBlock::Create(ctx_, name + ".take", fn);
      auto checkBB = BasicBlock::Create(ctx_, name + ".check", fn);
      auto defBB = BasicBlock::Create(ctx_, name + ".def", fn);
      auto mergeBB = BasicBlock::Create(ctx_, name + ".merge", fn);
      builder_.CreateCondBr(hasIdx, checkBB, defBB);

      builder_.SetInsertPoint(checkBB);
      auto slotPtr = builder_.CreateInBoundsGEP(
          valueType_, argsArg, {builder_.getInt64(static_cast<int64_t>(i))},
          name + ".slot");
      // Load tag first; if TAG_UNFILLED, fall through to default. The
      // value-load happens after the tag check so we never observe
      // the sentinel's data.
      auto slotTag = builder_.CreateLoad(builder_.getInt8Ty(),
                                          slotPtr, name + ".tag");
      auto isUnfilled = builder_.CreateICmpEQ(
          slotTag, builder_.getInt8(TAG_UNFILLED), name + ".unf");
      builder_.CreateCondBr(isUnfilled, defBB, takeBB);

      OwnedPhi merge(this, name + ".phi");
      builder_.SetInsertPoint(takeBB);
      merge.add_incoming(builder_.CreateLoad(valueType_, slotPtr, name));
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(defBB);
      if (isKwRest) {
        merge.add_incoming(own(make_object(emit_call(
            module_->getOrInsertFunction(rt::object_new, ptrTy), {},
            name + ".empty"))));
      } else {
        // The default runs ahead of this frame's own `recursion_enter`, so a
        // default that re-enters the same function (`fn h(a = 5, b = h(1))`)
        // would recurse uncounted and die as an uncatchable stack overflow.
        // Count the evaluation as one frame — the interp's RecursionFrame
        // around resolve_param_default. A throw skips the leave; the enclosing
        // frame's cleanup pad / catch entry restores the count, exactly as it
        // does for an inlined HOF body (emit_unary_lambda_body).
        emit_call(module_->getOrInsertFunction(rt::recursion_enter,
                                               builder_.getInt64Ty()),
                  {}, "def.rec");
        // compile() already yields a +1-owned value — the same ownership the
        // caller transfers into the arg slab for a passed argument — so the
        // slot below absorbs it directly. An extra retain here would leak the
        // default's +1 (the slot only releases one ref at scope exit).
        auto defVal = compile(*paramDefaults[i]);
        builder_.CreateCall(module_->getOrInsertFunction(rt::recursion_leave,
                                                         builder_.getVoidTy()),
                            {});
        merge.add_incoming(std::move(defVal));
      }
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      // The merged +1 crosses the remaining prologue emissions (type
      // check) into declare_local, which absorbs it (a transfer) — the
      // pre-existing prologue choreography, hence unchecked.
      argVal = merge.finish(mergeBB).consume_unchecked();
    }
    if (!paramTypeNames[i].empty()) {
      if (paramErrPos[i].first) {
        emit_type_check_at(argVal, paramTypeNames[i],
                           std::string("parameter '") + name + "'",
                           paramErrPos[i].first, paramErrPos[i].second);
      } else {
        // Cold-path position resolution: no user code ran since entry,
        // so the caller's thread-locals are still live at throw time.
        emit_type_check_param(argVal, paramTypeNames[i],
                              std::string("parameter '") + name + "'", i);
      }
    }
    if (is_sink_name(name)) {
      // `fn(_, _, x)`: drop each `_` arg's +1 transfer instead of
      // binding it. Allows repeated `_` params without slot collision.
      emit_value_release(argVal);
    } else if (info.captured_locals.contains(name)) {
      define_var(name, make_cell_slot(name, argVal, /*is_mut=*/paramMuts[i]));
    } else {
      define_var(name, make_stack_slot(name, argVal, /*is_mut=*/paramMuts[i]));
    }
  }

  // __ARGS__: Array of overflow args (args[declaredArity..n_args)).
  // Built only when the body references it (FuncInfo::uses_args).
  // Otherwise we still need to release the +1 retains the caller
  // transferred for each overflow slot — but the dedicated helper
  // (release_overflow_args) skips the Array allocation entirely.
  if (info.uses_args || argsRestName) {
    auto argsArr = emit_call(
        module_->getOrInsertFunction(
            rt::args_slice_to_array, ptrTy, ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {argsArg,
         builder_.getInt64(static_cast<int64_t>(declaredArity)),
         nArgsArg},
        "args.arr");
    auto argsVal = make_array(argsArr);
    // The same overflow Array backs both `__ARGS__` and a named `*args`
    // param when both are live; the second binding needs its own +1.
    if (info.uses_args && argsRestName) emit_value_retain(argsVal);
    if (info.uses_args) {
      if (info.captured_locals.contains("__ARGS__")) {
        define_var("__ARGS__", make_cell_slot("__ARGS__", argsVal));
      } else {
        define_var("__ARGS__", make_stack_slot("__ARGS__", argsVal));
      }
    }
    if (argsRestName) {
      const std::string& nm = *argsRestName;
      if (info.captured_locals.contains(nm)) {
        define_var(nm, make_cell_slot(nm, argsVal));
      } else {
        define_var(nm, make_stack_slot(nm, argsVal, /*is_mut=*/false));
      }
    }
  } else {
    // Conditional: only call the release helper when there *are*
    // overflow args (n_args > declaredArity). For the typical case
    // where the caller passes the declared count exactly, this
    // collapses to a single compare + not-taken branch — cheaper
    // than always entering the helper.
    auto fn = builder_.GetInsertBlock()->getParent();
    auto needRelBB = llvm::BasicBlock::Create(ctx_, "args.release", fn);
    auto skipBB = llvm::BasicBlock::Create(ctx_, "args.no_release", fn);
    auto declI64 =
        builder_.getInt64(static_cast<int64_t>(declaredArity));
    auto hasOverflow = builder_.CreateICmpUGT(nArgsArg, declI64,
                                              "args.has_overflow");
    builder_.CreateCondBr(hasOverflow, needRelBB, skipBB);
    builder_.SetInsertPoint(needRelBB);
    emit_call(
        module_->getOrInsertFunction(
            rt::release_overflow_args, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {argsArg, declI64, nArgsArg});
    builder_.CreateBr(skipBB);
    builder_.SetInsertPoint(skipBB);
  }

  // Recursion guard: count this frame. After the param binding and type
  // checks (whose errors outrank RecursionError — interp checks them
  // caller-side before entering the body closure), before the destructure
  // unpack and any user code (field inits, body). The post-enter depth is
  // stashed so cleanup pads can restore it while unwinding (see
  // current_rec_depth_slot_).
  if (body_ast || field_init_is_ctor) {
    // The slot starts at the "never entered" sentinel so the cleanup pad's
    // restore is a no-op when the enter itself threw (the store below only
    // runs once it returned).
    {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      current_rec_depth_slot_ =
          entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "rec.slot");
      entryB.CreateStore(builder_.getInt64(-1), current_rec_depth_slot_);
    }
    auto newDepth =
        emit_call(module_->getOrInsertFunction(rt::recursion_enter,
                                               builder_.getInt64Ty()),
                  {}, "rec.depth");
    builder_.CreateStore(newDepth, current_rec_depth_slot_);
  }

  // Unpack destructuring params (`fn ({a, b})`): the synthetic slot was
  // bound above; emit_pattern binds the pattern's names from it. A shape
  // mismatch throws the same ValueError as the interp.
  for (size_t i = 0; i < paramPatterns.size(); i++) {
    if (!paramPatterns[i]) continue;
    auto slot = lookup_var(paramNames[i]);
    if (!slot) continue;
    // Borrow the synthetic slot's value: emit_pattern only reads the subject
    // (leaf bindings retain their own elements), and the owned synthetic slot
    // keeps the tuple alive until scope exit, where it is released. A
    // retaining load_slot here adds a +1 that nothing consumes or releases —
    // a per-call leak of the destructured argument.
    auto val = load_slot_raw(*slot, paramNames[i]);
    auto matched = emit_pattern(*paramPatterns[i], val, /*is_mut=*/false);
    auto pfn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "param.destr.ok", pfn);
    auto failBB = llvm::BasicBlock::Create(ctx_, "param.destr.fail", pfn);
    builder_.CreateCondBr(matched, okBB, failBB);
    builder_.SetInsertPoint(failBB);
    emit_call(
        module_->getOrInsertFunction(rt::destructure_mismatch,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {builder_.getInt64(paramPatterns[i]->line),
         builder_.getInt64(paramPatterns[i]->column)});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
  }

  // A class `new` body carries a hidden free var binding its class's
  // field-init closure (see field_init_slot_name): invoke it here, after
  // parameter binding but before the first body statement — interp
  // parity (init_instance_fields runs between arg binding and the body;
  // arity/type/default errors above fire with no field side effects).
  for (const auto& fv : info.free_vars) {
    if (!fv.starts_with('\x1f')) continue;
    auto finitSlot = lookup_var(fv);
    auto selfSlot = lookup_var("self");
    if (!finitSlot || !selfSlot) continue;
    auto finitVal = load_slot_raw(*finitSlot, "finit");
    auto selfVal = load_slot_raw(*selfSlot, "self");
    auto finitPtr = builder_.CreateIntToPtr(extract_data(finitVal), ptrTy,
                                            "finit.cls");
    emit_call(
        module_->getOrInsertFunction(rt::run_field_init,
                                     builder_.getVoidTy(), ptrTy,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {finitPtr, extract_tag(selfVal), extract_data(selfVal)});
  }

  // Forward-reference support: closure capture happens lazily through
  // pre-allocated cells. See `pre_allocate_forward_refs` doc.
  if (body_ast) {
    pre_allocate_forward_refs(*body_ast);
  } else if (field_inits) {
    for (const auto& f : *field_inits) {
      if (f.init) pre_allocate_forward_refs(*f.init);
    }
  }

  // The body's result stays in its Owned across the epilogue: the scope
  // teardown below branches through owned.exit/owned.cont blocks, so a
  // bare +1 would cross them — it is consumed directly into the
  // ret. (On the unfixable niche path — a drop that throws during a
  // *normal* return, which propagates out of the frame with no pad — the
  // value still strands; see the current_lpad_ note below.)
  Owned bodyOwned;
  if (body_ast) {
    bodyOwned = compile(*body_ast);
  } else {
    // Field-init mode: assign each declared field on `self`, declaration
    // order. is_init=true mirrors the interp's insert_or_assign — a
    // property an earlier initializer's method call may have created is
    // overwritten regardless of its mut flag, and ends up mut like any
    // `self.x = ...` field (promote_all_mut equivalent).
    auto selfSlot = lookup_var("self");
    auto selfVal = load_slot_raw(*selfSlot, "self");
    auto selfPtr = builder_.CreateIntToPtr(extract_data(selfVal),
                                           ptrTy, "self.obj");
    for (const auto& f : *field_inits) {
      Owned v = f.init ? compile(*f.init) : emit_zero_value(f.type);
      emit_object_set(selfPtr, std::string(f.name), /*mut=*/true,
                      extract_tag(v.borrow()), extract_data(v.borrow()),
                      /*is_init=*/true);
      v.consume();  // the property slot absorbed the +1
    }
    bodyOwned = own(make_nil());
  }

  // The body is done: the normal-exit epilogue below is not an unwind, so its
  // scope-exit owned-region resolution runs as a plain call, not an invoke
  // back into the ladder. Keeping current_lpad_ here would make the normal
  // return's owned_scope_exit a predecessor of the top rung, defeating the
  // pred_empty erase for a body that never throws. (A drop that itself throws
  // during a *normal* return propagates out of the frame — matching the prior
  // behaviour of a function with no fn-level defer.)
  current_lpad_ = nullptr;

  if (!builder_.GetInsertBlock()->getTerminator()) {
    if (!returnType.empty()) {
      emit_return_type_check(bodyOwned.borrow());
    }
    // Run function-level defers before returning.
    if (fnMark) {
      builder_.CreateCall(
          module_->getFunction(rt::defer_run_to), {fnMark});
    }
    // Uncount this frame after its defers (interp: run_deferred fires
    // inside the guarded closure) but before the scope releases — their
    // drop() bodies run at the caller's depth there (callEnv teardown).
    if (current_rec_depth_slot_) {
      builder_.CreateCall(module_->getOrInsertFunction(
                              rt::recursion_leave, builder_.getVoidTy()),
                          {});
    }
    release_all_scopes_for_exit();
    builder_.CreateStore(bodyOwned.consume(), current_sret_);
    builder_.CreateRetVoid();
  } else {
    // The body ended in its own terminator (return/throw): the compiled
    // value is dead residue in a terminated block — discard without
    // emitting a release (the dtor would place invalid IR after the
    // terminator). The unconverted consume() result carries no check.
    (void)bodyOwned.consume();
  }

  finish_frame_cleanup_ladder(fnMark, /*suppress_drop=*/false);

  pop_scope();
  verifyFunction(*fn);

  // Restore outer context before emitting the closure at the caller's
  // insertion point — the cell captures come from the outer scope.
  saver.restore();
  // Positional callback-arity bounds, mirroring interp's
  // builtin_arity_bounds: regular params are those before the kw-only
  // separator and the `**kwargs` slot (which is always last); `*args`
  // makes the upper bound unbounded (cb_max = -1). Consulted by
  // _culebra_expect_callback so HOF callback arity matches the interpreter.
  size_t regular_end = firstKwOnlyIdx ? *firstKwOnlyIdx
                     : kwargsRestIdx ? *kwargsRestIdx
                                     : paramNames.size();
  long cbMin = 0;
  for (size_t i = 0; i < regular_end; i++) {
    if (!paramDefaults[i]) cbMin++;
  }
  long cbMax = argsRestName ? -1 : static_cast<long>(regular_end);
  auto* paramMeta = emit_param_meta_global(
      fn, paramNames, paramDefaults, kwargsRestIdx, firstKwOnlyIdx,
      std::string(declName), std::string(returnType), paramMuts,
      paramTypeNames, paramDeclaredTypeNames, cbMin, cbMax);
  if (outParamMeta) *outParamMeta = paramMeta;
  return own(emit_closure_build(fn, info, paramNames.size(), paramMeta));
}

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
