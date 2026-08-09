#pragma once

#ifdef CULEBRA_JIT_ENABLED

// `class` declaration codegen (JIT::compile_class_decl).
//
// Compiler-layer fragment of jit.h, split out for readability. Holds the
// out-of-line definition of a `JIT` member declared in the class body;
// relies on jit.h's #include block and the complete `JIT` definition, and
// is included by jit.h after the class (not a standalone header).

namespace culebra {

// `class Name { new(...){...}  m(...){...} }` — compiles each method
// plus the user-new body as regular closures, then emits a synthetic
// constructor closure that delegates to the runtime instance builder.
// `Name` binds the resulting namespace object in the current scope.
inline JIT::Owned JIT::compile_class_decl(const peg::Ast& ast) {
  using namespace peg::udl;
  auto ptrTy = llvm::PointerType::get(ctx_, 0);

  auto members = collect_class_members(ast);
  const size_t dec_end = members.dec_end;
  const std::string& class_name = members.class_name;
  const bool is_packable = members.is_packable;
  auto& new_asts = members.new_asts;
  const bool has_new = !new_asts.empty();
  auto& method_names = members.method_names;
  auto& method_asts = members.method_asts;
  auto& static_names = members.static_names;
  auto& static_asts = members.static_asts;
  auto& static_field_names = members.static_field_names;
  auto& static_field_asts = members.static_field_asts;
  auto& packable_fields = members.packable_fields;
  auto& derive_methods = members.derive_methods;

  // Install this class's Generic type params for the method-body
  // compilations below, under an RAII guard that restores the outer
  // class's params on any unwind. collect_class_members parsed the params
  // without touching the member slot, so an exception there left the outer
  // class's params intact; from here the guard covers all later throws.
  struct ClassScopeGuard {
    std::vector<std::string_view>& slot;
    std::vector<std::string_view> saved;
    ~ClassScopeGuard() { slot = std::move(saved); }
  } class_scope_guard{class_type_params_,
                      std::move(class_type_params_)};
  class_type_params_ = std::move(members.type_params);

  // Pre-allocate a cell for `Name` so method closures — which will
  // almost always reference the class for `Name.new(...)` or match
  // tagging — can capture it before the namespace itself exists.
  // The cell starts nil; we patch it to the real class value once
  // the methods have been compiled and the ctor closure is built.
  // Reuse a forward-ref pre-allocated slot if one already exists in
  // the current scope (statements above this class may have captured
  // it). Otherwise create a fresh cell.
  VarSlot classSlot;
  if (!scopes_.empty()) {
    auto& curSlots = scopes_.back().slots;
    auto it = curSlots.find(class_name);
    if (it != curSlots.end() && it->second.kind == VarSlot::Cell) {
      classSlot = it->second;
    } else {
      auto nilVal = make_nil();
      classSlot = make_cell_slot(class_name, nilVal);
      define_var(class_name, classSlot);
    }
  } else {
    auto nilVal = make_nil();
    classSlot = make_cell_slot(class_name, nilVal);
    define_var(class_name, classSlot);
  }

  // Compile each method into a closure %Value (+1 owned). A getter's body
  // address is registered at runtime so a bare `obj.name` read auto-invokes
  // it (culebra_runtime_bind_method_value). Getters are never overloaded, so
  // the closure survives the multimethod grouping below unchanged; register
  // here where each maps 1:1 to its source method.
  std::vector<Owned> method_vals;
  method_vals.reserve(method_asts.size() + derive_methods.size());
  auto ptrTyReg = llvm::PointerType::get(ctx_, 0);
  for (auto* m : method_asts) {
    Owned mval = compile_function(*m);
    if (culebra::view_method(*m).is_getter) {
      auto closPtr =
          builder_.CreateIntToPtr(extract_data(mval.borrow()), ptrTyReg);
      emit_call(module_->getOrInsertFunction(rt::register_getter,
                                             builder_.getVoidTy(), ptrTyReg),
                {closPtr});
    }
    method_vals.push_back(std::move(mval));
  }

  // An overload set on a well-known name can't satisfy the 0-arg
  // contract (the interp rejects the grouped dispatcher via its params
  // check); emit the contract error so it fires when the declaration
  // executes — the same timing as the single-method check inside
  // build_class_meta.
  {
    std::set<std::string> seen, thrown;
    for (const auto& n : method_names) {
      if (!seen.insert(n).second && is_well_known_prop(n) &&
          thrown.insert(n).second) {
        emit_set_op_pos();
        emit_call(module_->getOrInsertFunction(rt::wk_contract_error,
                                               builder_.getVoidTy(),
                                               llvm::PointerType::get(ctx_, 0)),
                  {get_or_create_global_str(n, ".wkname")});
      }
    }
  }
  // Done before the @derive append; a derived method whose name a user
  // overloads is already skipped (user_defined check in collect).
  group_method_overloads(method_names, method_vals, method_asts, class_name);
  // Append @derive methods: captureless closures over shared runtime
  // thunks (mirrors the variant-ctor pattern). They land in the class
  // meta alongside user methods, so dispatch + Set/Object key lookup
  // (JitValueEq/Hash via proto) find them transparently.
  for (auto& [mname, kind] : derive_methods) {
    auto closPtr = emit_call(
        module_->getOrInsertFunction(rt::make_derived_method, ptrTy,
                                     builder_.getInt64Ty()),
        {builder_.getInt64(kind)}, "derive.closure");
    method_vals.push_back(own(make_func(closPtr)));
    method_names.push_back(mname);
  }
  // Synthetic field-init function: assigns each declared field on `self`
  // (declaration order, per instance). Its FuncInfo is keyed by the
  // CLASS_DECL node (see the CLASS_DECL branch of visit_for_frees).
  // With a user `new`, the closure is bound under a hidden scope slot the
  // `new` body captures and invokes right after its parameter binding —
  // interp parity: initializers run only once the ctor args bound
  // successfully (arity/type/default errors fire first, with no field
  // side effects). Without a `new` there is no binding step, so the
  // closure rides the ctor captures and build_class_instance calls it.
  // Compiled BEFORE the `new` body so the hidden slot exists when the
  // body's closure captures it.
  Owned field_init_val;
  if (!members.field_inits.empty()) {
    field_init_val =
        compile_fn_common(&ast, /*params_ast=*/nullptr,
                          /*body_ast=*/nullptr, /*returnType=*/{},
                          /*declName=*/{}, /*outParamMeta=*/nullptr,
                          &members.field_inits,
                          /*field_init_is_ctor=*/!has_new);
    if (has_new) {
      // Every `new` overload's body captures the SAME field-init closure
      // via this hidden slot and invokes it after binding its params
      // (only the picked overload runs, so field inits fire exactly once).
      auto slot_name = field_init_slot_name(ast);
      auto slot = make_cell_slot(slot_name, field_init_val.consume());
      define_var(slot_name, slot);
    }
  }

  std::vector<Owned> static_vals;
  static_vals.reserve(static_asts.size());
  for (auto* m : static_asts) {
    static_vals.push_back(compile_function(*m));
  }
  // Static methods overload too — merge same-named statics into one
  // dispatcher (own per-class uid keeps them off the instance-method
  // table). The picked static body ignores the forwarded `self`.
  group_method_overloads(static_names, static_vals, static_asts, class_name);

  std::vector<Owned> static_field_vals;
  static_field_vals.reserve(static_field_asts.size());
  for (auto* m : static_field_asts) {
    static_field_vals.push_back(compile(*m->nodes[2]));
  }

  // Build the shared class meta object once per class declaration:
  // a JitObject with all method closures set as immutable props.
  // Each instance points its `proto` at this meta, so special-method
  // dispatch + general property lookup fall through here without
  // copying the methods into every instance's own props.
  llvm::Value* metaPtr;
  {
    llvm::Value* methodSlab;
    llvm::Value* nameArrayPtr;
    size_t n_methods = method_vals.size();
    if (n_methods > 0) {
      methodSlab = builder_.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(n_methods)),
          "meta.methods");
      for (size_t i = 0; i < n_methods; i++) {
        auto slot = builder_.CreateInBoundsGEP(
            valueType_, methodSlab,
            {builder_.getInt64(static_cast<int64_t>(i))});
        builder_.CreateStore(method_vals[i].consume(), slot);
      }
      std::vector<llvm::Constant*> names;
      names.reserve(n_methods);
      for (const auto& m : method_names) {
        names.push_back(llvm::cast<llvm::Constant>(
            get_or_create_global_str(m, ".mname")));
      }
      auto nameArrayTy = llvm::ArrayType::get(ptrTy, n_methods);
      auto nameArrayConst = llvm::ConstantArray::get(nameArrayTy, names);
      nameArrayPtr = new llvm::GlobalVariable(
          *module_, nameArrayTy, /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, nameArrayConst, ".mnames");
    } else {
      methodSlab = llvm::ConstantPointerNull::get(ptrTy);
      nameArrayPtr = llvm::ConstantPointerNull::get(ptrTy);
    }
    // build_class_meta runs the well-known contract over the method
    // template, so publish the declaration's position for that throw —
    // the interp stamps it with the CLASS_DECL node. PosGuard keeps
    // current_line_ on that node here, even after the method bodies
    // above compiled.
    emit_set_op_pos();
    metaPtr = emit_call(
        module_->getOrInsertFunction(rt::build_class_meta, ptrTy, ptrTy,
                                     ptrTy, builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {nameArrayPtr, methodSlab,
         builder_.getInt64(static_cast<int64_t>(n_methods)),
         builder_.getInt64(
             culebra::is_lowered_state_class(class_name, ast.path))},
        "class.meta");
  }
  Owned metaVal = own(make_object(metaPtr));

  // Build one constructor closure for a single `new` overload (or the
  // synthesized default when `na` is null). Captures: meta at [0]; the
  // `new` body at [1] (with a user ctor) OR the field-init closure at [1]
  // (default ctor for a class with fields) — never both, since a `new`
  // body reaches the field-init through its own hidden capture. `keep_meta`
  // retains a fresh meta ref (an overload set installs N closures over one
  // shared meta); a single ctor consumes metaVal's original +1 outright.
  auto build_ctor_closure = [&](const peg::Ast* na, bool keep_meta)
      -> llvm::Value* {
    Owned nbody;
    size_t narity = 0;
    llvm::Constant* np_meta = nullptr;
    if (na) {
      nbody = compile_function(*na, &np_meta);
      narity = culebra::view_method(*na).params->nodes.size();
    }
    const bool pass_finit = !members.field_inits.empty() && !na;
    auto ctor_fn = emit_constructor_fn(class_name, na != nullptr,
                                       pass_finit, narity);
    // The synthesized ctor is native on the interp side (body == nullptr,
    // not rebuildable), so neither backend ships it: register the compiled
    // thunk so a captured class object / `C.new` value rejects at the
    // serialize boundary with the interp's message. Runtime call — the
    // ctor's address only exists once the declaration runs (and AOT runs
    // in a separate process from compilation).
    emit_call(module_->getOrInsertFunction(rt::register_native_fn,
                                           builder_.getVoidTy(), ptrTy),
              {ctor_fn});
    // Register the `new` body's param meta under the synthesized ctor
    // wrapper's fn_ptr. The kwargs resolver (call_with_kwargs) keys meta
    // by fn_ptr, so `C.new(x: 1)` then binds keyword args into a positional
    // slab exactly as the interp's constructor binder does, before the
    // wrapper forwards that slab to the body. For an overload set the
    // dispatcher picks first, then recurses into the picked ctor wrapper —
    // which finds its own meta here.
    if (np_meta) {
      emit_call(
          module_->getOrInsertFunction(rt::register_param_meta,
                                       builder_.getVoidTy(), ptrTy, ptrTy),
          {ctor_fn, np_meta});
    }
    size_t n_captures = 1 + (na ? 1 : 0) + (pass_finit ? 1 : 0);
    auto closurePtr = emit_call(
        module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {ctor_fn,
         builder_.getInt64(static_cast<int64_t>(n_captures)),
         builder_.getInt64(static_cast<int64_t>(narity))},
        "ctor.closure");
    auto capturesFieldPtr =
        builder_.CreateStructGEP(closureType_, closurePtr, 3);
    auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr);
    auto install_capture = [&](size_t i, llvm::Value* v) {
      // cell_new takes ownership of the %Value's +1 by storing the raw
      // tag/data without retaining. Storing the cell pointer into the
      // captures array likewise transfers the cell's +1 to the closure.
      auto cellPtr = emit_call(
          module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty()),
          {extract_tag(v), extract_data(v)}, "ctor.cell");
      auto dst = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr,
          {builder_.getInt64(static_cast<int64_t>(i))});
      builder_.CreateStore(cellPtr, dst);
    };
    if (keep_meta) {
      // One class meta is shared by every `new` overload's ctor closure —
      // each capture[0] needs its own +1 (build_class_instance's proto
      // retain is separate). A genuine multi-capture fan-out, not a
      // throw-safety carve-out, so it stays a bare retain (rc-discipline
      // ceiling accounts for this one site). metaVal's original +1 is
      // released once, after the loop (metaVal.drop()).
      emit_value_retain(metaVal.borrow());
      install_capture(0, metaVal.borrow());
    } else {
      install_capture(0, metaVal.consume());
    }
    if (nbody.borrow()) {
      install_capture(1, nbody.consume());
    } else if (pass_finit) {
      install_capture(1, field_init_val.consume());
    }
    return closurePtr;
  };

  // The class's "new": a lone ctor closure, or — for an overload set — a
  // multimethod dispatcher over the per-overload ctor closures (keyed off
  // the interp's shared multifn machinery, picked BEFORE any instance is
  // built). `C(args)` reaches it through the same call path either way.
  Owned ctorOwned;
  if (new_asts.size() <= 1) {
    ctorOwned = own(make_func(
        build_ctor_closure(has_new ? new_asts[0] : nullptr,
                           /*keep_meta=*/false)));
  } else {
    auto regKey =
        class_multifn_key("new", class_name, multifn_uid_counter_++);
    for (auto* na : new_asts) {
      auto ctorPtr = build_ctor_closure(na, /*keep_meta=*/true);
      // Each registration hands back a +1 to the same shared dispatcher;
      // keep only the last (own() releases the earlier +1s). The registry
      // absorbs each ctor closure's +1 as its overload body.
      ctorOwned = own(emit_multifn_register(
          regKey, ctorPtr, *culebra::view_method(*na).params,
          class_type_params_));
    }
    // The keep_meta retains left metaVal's original +1 undistributed;
    // release it so meta is owned only by the N ctor closures (+ instances).
    metaVal.drop();
  }

  // Build the class namespace object: { new: ctorClosure }.
  auto classObj = emit_call(
      module_->getOrInsertFunction(rt::object_new, ptrTy), {},
      "class.ns");
  // Mark it callable: `C(args)` dispatches to `new` (compile_function_call_raw
  // cold path). The flag also keeps a plain dict with a "new" key inert.
  emit_call(module_->getOrInsertFunction(rt::mark_class,
                                         builder_.getVoidTy(), ptrTy),
            {classObj});
  // Namespace fill goes through the raw bind (interp: `emplace`), not
  // emit_object_set — statics named `drop` are ordinary functions, so
  // neither the well-known contract nor the owned/drop registration
  // applies to the class object.
  auto emit_bind_static = [&](const std::string& name, Owned val) {
    emit_call(
        module_->getOrInsertFunction(rt::object_bind_static,
                                     builder_.getVoidTy(), ptrTy, ptrTy,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {classObj, get_or_create_global_str(name, ".key"),
         extract_tag(val.borrow()), extract_data(val.borrow())});
    val.consume();  // the slot absorbs the +1
  };
  emit_bind_static("new", std::move(ctorOwned));
  for (size_t i = 0; i < static_vals.size(); i++) {
    emit_bind_static(static_names[i], std::move(static_vals[i]));
  }
  for (size_t i = 0; i < static_field_vals.size(); i++) {
    emit_bind_static(static_field_names[i], std::move(static_field_vals[i]));
  }
  // @packable: register the fixed C-ABI layout and mark the class object
  // so SharedBuffer.new can recover the class name. Registration is
  // emitted as a *runtime* call (executed when the class declaration runs)
  // because AOT compiles and runs in separate processes — a compile-time
  // registration would be invisible to the standalone binary. The field
  // spec is "name:Type;..."; field types are already lint-validated.
  if (is_packable) {
    std::string spec;
    for (const auto& [fname, ftype] : packable_fields) {
      if (!spec.empty()) spec += ';';
      spec += fname; spec += ':'; spec += ftype;
    }
    emit_call(
        module_->getOrInsertFunction(rt::register_packable,
                                     builder_.getVoidTy(), ptrTy, ptrTy),
        {get_or_create_global_str(class_name, ".pkg.name"),
         get_or_create_global_str(spec, ".pkg.spec")});
    auto markerVal = make_string(emit_str_literal(class_name));
    emit_bind_static("__packable__", own(markerVal));
  }
  Owned classVal = own(make_object(classObj));

  // Apply decorators (bottom-up): each takes the current class
  // value and returns the new one. Decorated class still ends up
  // in the same `classSlot` so method-capture references resolve
  // to the decorated value at call time.
  for (size_t i = dec_end; i > 0; --i) {
    // `@derive(...)` was consumed into method injection above, and
    // `@packable` is a layout constraint, not a callable decorator.
    if (!culebra::view_derive(*ast.nodes[i - 1]).empty()) continue;
    if (culebra::is_packable_decorator(*ast.nodes[i - 1])) continue;
    const auto& dec_expr = *ast.nodes[i - 1]->nodes[0];
    // Borrowed-callee dispatch; see the enum decorator loop.
    Owned decoCallee = compile(dec_expr);
    classVal = compile_function_call_raw(decoCallee.borrow(), nullptr,
                                         {classVal.consume()});
  }

  // Patch the pre-allocated cell with the real class namespace.
  // `store_slot` transfers `classVal`'s +1 into the cell; nothing
  // else here consumes the SSA value.
  store_slot(classSlot, classVal.consume());
  return own(make_nil());
}

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
