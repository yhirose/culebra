#pragma once

#ifdef CULEBRA_JIT_ENABLED

// Multimethod dispatch, enum (sum type) support, @derive reflective
// methods, JitParamMeta and the call/kwargs machinery.
//
// Runtime-layer fragment of jit.h, split out for readability. These
// fragments rely on jit.h's #include block and are included by jit.h in a
// fixed sequence (see jit.h); they are not standalone headers.

// Continues the runtime extern "C" block opened in jit_runtime.h
// (split across jit_fixed.h / jit_dispatch.h / jit_iter.h).
extern "C" {

// --- Multimethod dispatch (interp-parity, see eval_multifn_decl) ---
//
// The two tables below are process-wide statics, mirroring the
// single-threaded scoping of `culebra_thrown_tag` etc. (see the
// runtime-globals comment near the top of this file). Multiple JIT
// instances in the same process — and successive runs that share a
// process — accumulate entries; if that becomes a real configuration
// (e.g. a long-lived host embedding repeated culebra invocations) we
// should switch to a per-JitContext registry. For the supported
// single-binary single-run case, both tables are bounded by the
// number of multimethods declared in the script and are torn down at
// process exit alongside the closures they reference.

// Forward declarations — closure_new and value_release_impl are
// defined later in this file but used by the multimethod runtime.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity);

// --- Enum (sum type) support ---------------------------------------
// Call-site source position for the JitFn-ABI dispatcher thunk. The thunk
// is invoked through the fixed (cls, this, n_args, args) closure ABI, which
// has no slot for line/col, so a plain `f(1)` that fails to dispatch can't
// otherwise locate the error. The general call codegen stores the call-site
// position here just before an indirect closure call; the thunk reads it on
// the (cold) DispatchError path. Only read after a store on the same call,
// so the single i64-pair store per call is the only cost on the hot path.
// (_jit_call_site_line/col are defined earlier, next to arity_missing.)

// Source position of a higher-order call's callback ARGUMENT, stored by the
// JIT right before a HOF runtime call (after the argument is evaluated). A
// non-Function callback is reported here, matching the interp's typed-param
// check, which attributes "parameter '<name>' expects Function" to the
// argument's location — distinct from `_jit_call_site`, which carries the HOF
// *call* site for a callback's own per-element throw.
inline thread_local int64_t _jit_callback_arg_line = 0;
inline thread_local int64_t _jit_callback_arg_col = 0;

// Source position of the FIRST argument of an indirect closure call, stored
// just before the call (alongside _jit_call_site). A stdlib method used as a
// value (`let f = FS.read; f(5)`) reaches its arg0 type check through the
// closure ABI, which carries no per-arg position; the ns-method dispatch reads
// this so the error points at the argument like the interp binder, not at the
// call site. Set on every indirect call (defaults to the call site when there
// is no argument), so it is never stale.
inline thread_local int64_t _jit_call_arg0_line = 0;
inline thread_local int64_t _jit_call_arg0_col = 0;

// Build a variant instance: tagged with `class` = variant name and
// `__enum` = parent enum name, with the `arity` declared payload fields
// `_0.._{arity-1}` taking ownership of the caller's args (object_set
// transfers the +1, mirroring the default-ctor path). `n_args` is what the
// caller actually passed: too few raises "missing required argument '_k'"
// and too many drops (and releases) the extras — matching the interpreter's
// ctor binding, which binds `_0.._{arity-1}` positionally. `line`/`col`
// locate the arity error (the direct-call emit passes the call site; the
// ctor-as-value thunk passes the recorded `_jit_call_site`).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_build_variant(
    const char* variant_name, const char* enum_name, int64_t n_args,
    JitValue* args, int64_t arity, int64_t line, int64_t col) {
  if (n_args < arity) {
    // Mirror interp: report the first unbound positional field, after
    // releasing the args we took ownership of but won't store.
    auto missing = culebra::positional_field_name(static_cast<size_t>(n_args));
    for (int64_t i = 0; i < n_args; i++) {
      _culebra_value_release_impl(args[i].tag, args[i].data);
    }
    throw culebra::CulebraError(
        "ArityError", culebra::missing_required_arg_message(missing),
        line, col);
  }
  auto* inst = culebra_runtime_object_new();
  culebra_runtime_object_set(inst, "class", /*mut*/ false, TAG_STRING,
                             reinterpret_cast<int64_t>(variant_name), 0, 0);
  culebra_runtime_object_set(inst, "__enum", /*mut*/ false, TAG_STRING,
                             reinterpret_cast<int64_t>(enum_name), 0, 0);
  for (int64_t i = 0; i < arity; i++) {
    auto fname = culebra::positional_field_name(static_cast<size_t>(i));
    culebra_runtime_object_set(inst, fname.data(), /*mut*/ false, args[i].tag,
                               args[i].data, 0, 0);
  }
  // Drop excess positional args (interp ignores them); release the +1 we
  // were handed so they don't leak.
  for (int64_t i = arity; i < n_args; i++) {
    _culebra_value_release_impl(args[i].tag, args[i].data);
  }
  for (auto& entry : inst->slots) entry.mut = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
}

// Side table mapping a payload-variant constructor closure to its
// (variant, enum) names — recovered by the shared thunk below. Mirrors
// _jit_multifn_dispatcher_names: the closure carries no captures.
// thread_local: keyed by per-thread JIT-compiled closure pointers, so
// there is nothing to share across host threads. Isolating per thread
// removes the data race that a process-wide static would have (unlike
// trait_registry, which is intentionally shared + mutex-guarded because
// every isolate must see the same trait definitions).
inline std::map<JitClosure*, std::pair<std::string, std::string>>&
_jit_variant_ctor_info() {
  static thread_local std::map<JitClosure*, std::pair<std::string, std::string>>
      tbl;
  return tbl;
}

// JitFn-ABI shared thunk installed as `fn_ptr` on every payload-variant
// constructor closure. Recovers the variant/enum names from the side
// table and builds the instance from the call args.
inline void _jit_variant_ctor_thunk(JitValue* __ret, JitClosure* cls, int8_t self_tag,
                                         int64_t self_data, int64_t n_args,
                                         JitValue* args) {
  JitValue self_val{self_tag, self_data};
  // A direct `E.V(x)` call reaches here through the method-call ABI, which
  // hands `self` (the enum namespace) at +1 for the callee to consume. This
  // thunk builds a fresh variant instead of forwarding `self` to a body, so
  // it must drop that +1 or the namespace strands one reference per call.
  // (An indirect `let f = E.V; f(x)` passes nil — a no-op release.)
  _culebra_value_release_impl(self_val.tag, self_val.data);
  auto& info = _jit_variant_ctor_info();
  auto it = info.find(cls);
  if (it == info.end()) { *__ret = {TAG_NIL, 0}; return; }
  { *__ret = culebra_runtime_build_variant(
      _intern_str(it->second.first), _intern_str(it->second.second), n_args,
      args, static_cast<int64_t>(cls->arity), _jit_call_site_line,
      _jit_call_site_col); return; }
}

// Create a payload-variant constructor closure (`Result.Ok`): a closure
// over the shared thunk with the variant/enum names recorded in the
// side table. Returns +1 (caller owns; the enum namespace slot takes it).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure*
culebra_runtime_make_variant_ctor(const char* variant_name,
                                    const char* enum_name, int64_t arity) {
  _jit_register_native_fn(
      reinterpret_cast<const void*>(&_jit_variant_ctor_thunk));
  auto* cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_jit_variant_ctor_thunk), /*n_captures=*/0,
      static_cast<size_t>(arity));
  _jit_variant_ctor_info()[cls] = {std::string(variant_name),
                                    std::string(enum_name)};
  return cls;
}

// --- @derive reflective methods ---------------------------------------
//
// `@derive(Eq, Hash, Show, Comparable)` injects methods that walk an
// instance's own data fields at call time. The JIT mirrors the variant
// ctor pattern: each derived method is a captureless closure over a
// shared thunk, so no per-class codegen is needed. The thunks recover
// `self` from the JitFn ABI and forward to the reflective helpers below
// — no side table, since the helpers read everything off the instance.
// AOT inherits this for free: the closures are built by runtime calls
// emitted in the class-decl IR, and the thunks/helpers live in the rt
// library.

// Class-name tag stored on every instance ("class" -> String). Empty if
// absent (defensive; class-sugar instances always carry it).
inline std::string_view _jit_derived_class_tag(JitObject* obj) {
  auto idx = obj->find_slot("class");
  if (idx == static_cast<size_t>(-1)) return {};
  const auto& v = obj->slots[idx].value;
  if (v.tag != TAG_STRING) return {};
  return _culebra_str_view(v.tag, v.data);
}

// Three-way compare for `cmp`, mirroring the interp's derived body: `==`
// decides sameness, and anything else is ordered by `<`. Both operands here
// are the twins of what the interp reaches — `_culebra_value_equal` for
// `Value::operator==`, `_culebra_value_ord` for `Value::ord_compare` — so a
// field pair the language refuses to order (Array, Object, mixed types)
// raises the same TypeError, rather than an order invented from the raw
// payloads. Positionless: every caller runs under _jit_at_call_site.
inline int _jit_derived_cmp3(const JitValue& a, const JitValue& b) {
  if (_culebra_value_equal(a.tag, a.data, b.tag, b.data)) return 0;
  return _culebra_value_ord(a.tag, a.data, b.tag, b.data,
                            [](double x, double y) { return x < y; }, 0, 0)
             ? -1
             : 1;
}

// eq(other): same class tag + every data field equal (JitValueEq, so
// nested user/derived eq composes). A non-Object or different class is
// unequal.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_derived_eq(
    JitObject* lhs, JitValue other) {
  if (other.tag != TAG_OBJECT) return {TAG_BOOL, 0};
  auto* rhs = reinterpret_cast<JitObject*>(other.data);
  if (_jit_derived_class_tag(lhs) != _jit_derived_class_tag(rhs))
    return {TAG_BOOL, 0};
  bool eq = true;
  lhs->for_each([&](std::string_view name, const JitObjectEntry& e) {
    if (!eq || name == "class" || name == "__enum" || e.value.tag == TAG_FUNC)
      return;
    auto idx = rhs->find_slot(name);
    if (idx == static_cast<size_t>(-1)) {
      eq = false;
      return;
    }
    if (!JitValueEq{}(e.value, rhs->slots[idx].value)) eq = false;
  });
  return {TAG_BOOL, eq ? 1 : 0};
}

// hash(): combine the class-name hash with each data field's hash
// (JitValueHash composes nested user/derived hashes).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_derived_hash(
    JitObject* obj) {
  size_t h = std::hash<std::string_view>{}(_jit_derived_class_tag(obj));
  obj->for_each([&](std::string_view name, const JitObjectEntry& e) {
    if (name == "class" || name == "__enum" || e.value.tag == TAG_FUNC) return;
    h = h * 31 + JitValueHash{}(e.value);
  });
  return static_cast<int64_t>(h);
}

// to_s(): "ClassName(f1, f2, ...)" with each field's value repr (strings
// quoted, matching the interpreter's `Value::str()`).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_derived_show(
    JitObject* obj) {
  std::string s(_jit_derived_class_tag(obj));
  s += "(";
  bool first = true;
  obj->for_each([&](std::string_view name, const JitObjectEntry& e) {
    if (name == "class" || name == "__enum" || e.value.tag == TAG_FUNC) return;
    if (!first) s += ", ";
    first = false;
    s += _culebra_value_to_str_impl(e.value.tag, e.value.data);
  });
  s += ")";
  return _culebra_heap_str(s);
}

// cmp(other): lexicographic over data fields in declaration order.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_derived_cmp(
    JitObject* lhs, JitValue other) {
  if (other.tag != TAG_OBJECT) return 0;
  auto* rhs = reinterpret_cast<JitObject*>(other.data);
  int64_t result = 0;
  bool done = false;
  lhs->for_each([&](std::string_view name, const JitObjectEntry& e) {
    if (done || name == "class" || name == "__enum" || e.value.tag == TAG_FUNC)
      return;
    auto idx = rhs->find_slot(name);
    if (idx == static_cast<size_t>(-1)) {
      done = true;
      return;
    }
    int c = _jit_derived_cmp3(e.value, rhs->slots[idx].value);
    if (c != 0) {
      result = c;
      done = true;
    }
  });
  return result;
}

// JitFn-ABI thunks installed as `fn_ptr` on each derived-method closure.
// The ABI is callee-consumes on EVERY exit: the invoker (_culebra_invoke_method*,
// or a codegen call site) hands `self` and each arg at +1 and never gives them
// back, exactly as a compiled frame's slots release on a normal return and
// through its cleanup pad on a throw. So these hold theirs in the same
// JitMethodSelf / JitMethodArgs pair every other native endpoint uses — a
// nested user `eq`/`cmp` throwing out of a derived body would strand them
// otherwise, and without the release at all every derived
// `==`/`!=`/`cmp`/`hash`/`show` leaks its operand(s).
// `eq(other)` / `cmp(other)` declare a required parameter on the interp side
// (a native FunctionValue with one param), so calling either with no argument
// is the ordinary missing-required ArityError — raised before the body, as any
// binder would. Shared by the two 1-parameter thunks below; `hash()` / `to_s()`
// take none and so have nothing to check.
inline void _jit_derived_require_other(int64_t n) {
  if (n >= 1) return;
  static const char* const kNames[] = {"other", nullptr};
  culebra_runtime_arity_missing(kNames, 0, 0, 0);
}

inline void _jit_derived_eq_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                       JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodArgs _a{n, args};
  JitMethodSelf _s{self};
  _jit_derived_require_other(n);
  JitValue r{TAG_BOOL, 0};
  // The thunk ABI carries no line/col; backfill the walker's positionless
  // nesting ValueError from the published call site (all four thunks).
  if (self.tag == TAG_OBJECT)
    r = _jit_at_call_site([&] {
      return culebra_runtime_derived_eq(reinterpret_cast<JitObject*>(self.data),
                                        args[0]);
    });
  { *__ret = r; return; }
}
inline void _jit_derived_hash_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                         JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodArgs _a{n, args};
  JitMethodSelf _s{self};
  JitValue r{TAG_LONG, 0};
  if (self.tag == TAG_OBJECT)
    r = {TAG_LONG, _jit_at_call_site([&] {
           return culebra_runtime_derived_hash(
               reinterpret_cast<JitObject*>(self.data));
         })};
  { *__ret = r; return; }
}
inline void _jit_derived_show_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                         JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodArgs _a{n, args};
  JitMethodSelf _s{self};
  const char* s = (self.tag == TAG_OBJECT)
                      ? _jit_at_call_site([&] {
                          return culebra_runtime_derived_show(
                              reinterpret_cast<JitObject*>(self.data));
                        })
                      : _culebra_heap_str("");
  { *__ret = {TAG_STRING, reinterpret_cast<int64_t>(s)}; return; }
}
inline void _jit_derived_cmp_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                        JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodArgs _a{n, args};
  JitMethodSelf _s{self};
  _jit_derived_require_other(n);
  JitValue r{TAG_LONG, 0};
  if (self.tag == TAG_OBJECT)
    r = {TAG_LONG, _jit_at_call_site([&] {
           return culebra_runtime_derived_cmp(
               reinterpret_cast<JitObject*>(self.data), args[0]);
         })};
  { *__ret = r; return; }
}

// Build a derived-method closure. `kind`: 0=eq, 1=hash, 2=show, 3=cmp.
// Returns +1 (the class meta slot takes ownership).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure*
culebra_runtime_make_derived_method(int64_t kind) {
  void* thunk = nullptr;
  size_t arity = 0;
  switch (kind) {
    case 0: thunk = reinterpret_cast<void*>(&_jit_derived_eq_thunk); arity = 1; break;
    case 1: thunk = reinterpret_cast<void*>(&_jit_derived_hash_thunk); arity = 0; break;
    case 2: thunk = reinterpret_cast<void*>(&_jit_derived_show_thunk); arity = 0; break;
    case 3: thunk = reinterpret_cast<void*>(&_jit_derived_cmp_thunk); arity = 1; break;
  }
  _jit_register_native_fn(thunk);
  return culebra_runtime_closure_new(thunk, /*n_captures=*/0, arity);
}

// Parameter metadata attached to JIT-compiled user functions. The side
// table below is keyed by `fn_ptr` (the underlying JitFn pointer) and
// populated at JIT-module init time for each FUNCTION literal. It lets
// `culebra_runtime_call_with_kwargs` resolve names → slab positions
// at runtime — supporting indirect callees (captures, method values,
// UFCS) and dynamic `**variable` splats. Built-in closures created by
// `culebra_runtime_closure_new` from runtime helpers (iter wrappers
// etc.) never register meta and so reject kwargs.
struct JitParamMeta {
  const char* const* names;  // pointers into module-level globals
  // Bit i is set if param i has a default expression. The callee
  // prologue handles defaults inline; the resolver only uses this to
  // decide whether an unfilled middle slot is OK (default) or an
  // ArityError (required).
  const uint8_t* has_default_bits;
  size_t n_params;
  // Index of the `**rest` catch-all parameter, or -1 if none. The
  // runtime resolver builds an Object from unconsumed kwargs and
  // places it in slab[kwargs_rest_idx].
  int64_t kwargs_rest_idx;
  // Index of the first keyword-only parameter (after a `*` separator
  // with regular params before it), or -1. Used by dynamic-callee
  // dispatch to enforce kw-only at runtime when a closure is captured
  // and called positionally (`let g = f; g(1, 2)` where f is kw-only).
  int64_t first_kw_only_idx;
  // Introspection-only fields appended at the end so existing kwargs
  // dispatch consumers stay unaffected. `fn_name`/`return_type` are
  // always non-null cstrings (empty string if unset). `mut_bits` is a
  // bitmap (one bit per param). `type_names` is an array of n_params
  // cstrings (each non-null, "" if unannotated).
  const char* fn_name;
  const char* return_type;
  const uint8_t* mut_bits;
  const char* const* type_names;
  // Original declared annotation per param (before class type-param
  // neutralization). Used by introspection so `fn.params[i].type`
  // surfaces `T` / `Array<T>` for Generic class methods. Same layout
  // as `type_names`; empty string when unannotated.
  const char* const* declared_type_names;
  // Positional callback-arity bounds (single source of truth shared with
  // interp via `callback_arity_accepts`): `cb_min` is the required
  // positional count, `cb_max` the total regular positional count, or -1
  // when a `*args` catch-all removes the upper bound. Consulted by
  // `_culebra_expect_callback` so the JIT accepts/rejects higher-order
  // callbacks exactly like the interpreter.
  int64_t cb_min;
  int64_t cb_max;
};

// Layout matches the LLVM struct emitted in emit_param_meta_global.
// Adding a field requires updating both — the assert catches drift.
static_assert(sizeof(JitParamMeta) == 12 * sizeof(int64_t),
              "JitParamMeta C++ / LLVM layout drift");

// Build (once) a stable JitParamMeta from a handle method's param names
// and per-param has-default flags. Only the fields the kwargs resolver
// reads are meaningful (names / has_default_bits / n_params /
// kwargs_rest_idx / first_kw_only_idx / cb_min / cb_max); the rest get
// benign defaults — the handle method's own thunk does the type check
// and the body, not the resolver. No `*`/`**rest` (handle methods have
// fixed positional params + optional trailing defaults).
inline const JitParamMeta* _jit_make_handle_meta(
    std::vector<std::string> names, std::vector<bool> has_default) {
  struct Stable {
    std::vector<std::string> names;
    std::vector<const char*> name_ptrs;
    std::vector<const char*> empties;
    std::vector<uint8_t> def_bits, mut_bits;
    JitParamMeta meta;
  };
  static std::vector<std::unique_ptr<Stable>> storage;
  auto s = std::make_unique<Stable>();
  s->names = std::move(names);
  size_t n = s->names.size();
  s->name_ptrs.reserve(n);
  for (auto& nm : s->names) s->name_ptrs.push_back(nm.c_str());
  s->empties.assign(n, "");
  s->def_bits.assign((n + 7) / 8, 0);
  s->mut_bits.assign((n + 7) / 8, 0);
  int64_t cb_min = static_cast<int64_t>(n);
  for (size_t i = 0; i < n; i++) {
    if (i < has_default.size() && has_default[i]) {
      s->def_bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
      if (static_cast<int64_t>(i) < cb_min) cb_min = static_cast<int64_t>(i);
    }
  }
  s->meta = JitParamMeta{s->name_ptrs.data(),
                         s->def_bits.data(),
                         n,
                         /*kwargs_rest_idx=*/-1,
                         /*first_kw_only_idx=*/-1,
                         /*fn_name=*/"",
                         /*return_type=*/"",
                         s->mut_bits.data(),
                         /*type_names=*/s->empties.data(),
                         /*declared_type_names=*/s->empties.data(),
                         cb_min,
                         static_cast<int64_t>(n)};
  const JitParamMeta* ret = &s->meta;
  storage.push_back(std::move(s));
  return ret;
}


// thread_local: keyed by per-thread JIT-compiled fn pointers (see the
// note on _jit_variant_ctor_info). No cross-thread sharing needed.
inline std::unordered_map<void*, const JitParamMeta*>&
_jit_param_meta_table() {
  static thread_local std::unordered_map<void*, const JitParamMeta*> tbl;
  return tbl;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_param_meta(
    void* fn_ptr, const JitParamMeta* meta) {
  _jit_param_meta_table()[fn_ptr] = meta;
}

inline const JitParamMeta* _jit_lookup_param_meta(void* fn_ptr) {
  auto& tbl = _jit_param_meta_table();
  auto it = tbl.find(fn_ptr);
  return it == tbl.end() ? nullptr : it->second;
}

// thread_local: fn_ptr -> names of free vars the lambda captured as `mut`.
// Same keying/thread rationale as _jit_param_meta_table. Registered at closure
// construction for lambdas that have any mut capture, read by jit_serialize to
// reject them at an isolate boundary — interp parity with sendable.h, where a
// `mut` capture would silently snapshot rather than track the parent's value.
inline std::unordered_map<void*, std::vector<std::string>>&
_jit_mut_capture_table() {
  static thread_local std::unordered_map<void*, std::vector<std::string>> tbl;
  return tbl;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_mut_captures(
    void* fn_ptr, const char* const* names, int64_t n) {
  auto& v = _jit_mut_capture_table()[fn_ptr];
  if (!v.empty()) return;  // idempotent: same fn_ptr always has the same set
  v.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; i++) v.emplace_back(names[i]);
}

// The first `mut`-captured free-var name, or nullptr if the lambda captured
// none (including lambdas never registered — they have no mut captures).
inline const std::string* _jit_first_mut_capture(void* fn_ptr) {
  auto& tbl = _jit_mut_capture_table();
  auto it = tbl.find(fn_ptr);
  return (it == tbl.end() || it->second.empty()) ? nullptr : &it->second.front();
}

// Same question for a closure whose code is not a distinct fn_ptr — see
// _jit_closure_meta_hook, which this mirrors. The VM executor installs it;
// null for anything it does not recognise, so the fn_ptr table stands.
inline const std::string* (*_jit_closure_mut_capture_hook)(JitClosure*) =
    nullptr;

// The question every sendability check actually asks: does THIS closure
// capture a mutable binding? Routes to whichever of the two knows.
inline const std::string* _jit_first_mut_capture_of(JitClosure* c) {
  if (_jit_closure_mut_capture_hook) {
    if (const std::string* nm = _jit_closure_mut_capture_hook(c)) return nm;
  }
  return _jit_first_mut_capture(c->fn_ptr);
}

// Third question of the same shape: is this closure's body something no
// other Runtime can rebuild? The fn_ptr registry answers for a C++-bodied
// closure; the executor's constructor thunks are ordinary chunks, so they
// need the hook to be rejected as their interp/JIT twins are (a class
// object carries its ctor, so this is what makes sending one an error).
inline bool (*_jit_closure_is_native_hook)(JitClosure*) = nullptr;

// Hook for closures whose code is not a distinct fn_ptr: the VM executor
// interprets every chunk through one entry point, so its closures cannot key
// the per-fn JitParamMeta table either. It installs this to answer with the
// chunk's own metadata; returns null for anything it does not recognise, so
// the regular lookup stands. (The VM's lowering lane needs no hook — there
// each chunk is its own function, and it registers like the AST path.)
inline const JitParamMeta* (*_jit_closure_meta_hook)(JitClosure*) = nullptr;

// The same seam for the other kind of shared entry point: every native stdlib
// closure runs through one trampoline, so its signature cannot key the per-fn
// table either. stdlib_jit.h installs this — it owns the derivation from the
// canonical interp parameter list, which is where a native's signature lives.
// Kept separate from the hook above because both can be installed at once (a
// VM run reaches native closures too).
inline const JitParamMeta* (*_jit_native_meta_hook)(JitClosure*) = nullptr;

// The other half of the same seam: which capture cell carries the chunk a
// closure runs, for the one place that has to REBUILD a closure from what it
// recorded rather than call the one it was handed — the lazy-namespace
// builder registry, whose entries outlive the closure they came from. Null
// for anything but a VM-executor closure (a distinct fn_ptr is enough there).
inline JitCell* (*_jit_closure_desc_hook)(JitClosure*) = nullptr;

// Hook for stdlib namespace methods (FS/Proc/...). All such methods share
// one trampoline fn_ptr, so they can't key the per-fn JitParamMeta table;
// stdlib_jit.h installs this hook to resolve a kwarg call against the
// NsParamMeta carried in the closure's capture. Returns true if it handled
// the call (writing the result to *out); false if `cls` isn't an ns-method
// closure, so the regular meta-lookup path runs. Ownership of the +1 on
// each positional/kwarg/splat transfers to the hook when it returns true.
inline bool (*_jit_ns_kwarg_hook)(
    JitClosure* cls, JitValue self_val, int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs, int64_t line, int64_t col,
    JitValue* out) = nullptr;

// stdlib_jit.h installs this hook so an ns-method closure passed as a HOF
// callback is arity-gated by its real declared bounds. ns-method closures
// share one trampoline fn_ptr (so they carry no per-fn JitParamMeta) and a
// kwarg-capable method is created with JIT_VARIADIC_ARITY, which would
// otherwise accept any callback arity. The hook writes the method's
// callback bounds (cb_max < 0 = variadic) derived from the SAME canonical
// params the interp's check_callback_arity reads, and returns true; it
// returns false for non-ns closures so the regular path runs.
inline bool (*_jit_ns_callback_arity_hook)(
    JitClosure* cls, int64_t* cb_min, int64_t* cb_max) = nullptr;

// Enforce kw-only at runtime for dynamic-callee positional calls.
// `let g = f; g(1, 2)` where f is kw-only would otherwise fill the
// kw-only slot positionally without the compile-time static check
// firing — this runtime guard catches that case to match the interp.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_check_pos_count(
    void* fn_ptr, int64_t n_pos, int64_t line, int64_t col) {
  const JitParamMeta* meta = _jit_lookup_param_meta(fn_ptr);
  if (!meta) return;
  culebra::throw_if_too_many_positionals(meta->first_kw_only_idx, n_pos,
                                          line, col);
}

// The same guard for a callee whose code is not a distinct fn_ptr (the VM
// executor's closures all share one), asked by closure instead.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_check_pos_count_cls(
    JitClosure* cls, int64_t n_pos, int64_t line, int64_t col) {
  const JitParamMeta* meta = _jit_lookup_param_meta(cls->fn_ptr);
  if (!meta && _jit_closure_meta_hook) meta = _jit_closure_meta_hook(cls);
  if (!meta) return;
  culebra::throw_if_too_many_positionals(meta->first_kw_only_idx, n_pos,
                                          line, col);
}

struct JitMultiMethodEntry {
  std::vector<std::string> param_types;  // empty = "Any"
  // Regular param names, parallel to param_types — a required param omitted
  // positionally may be covered by a keyword of the same name.
  std::vector<std::string> param_names;
  JitClosure* body;                       // +1 owned by this entry
  bool variadic = false;                  // has a `*args` catch-all
  // Required regular params (no default); matches arity [min_params,
  // param_types.size()]. Mirrors interp's MultiMethod::min_params.
  size_t min_params = 0;
};

// Per-Runtime substate (not thread_local): holds +1 closure refs from this
// Runtime's JIT run. A substate so it follows ~Runtime's slot-ordered teardown
// — it must outlive the module/namespace tables, whose destructors release
// dispatcher closures that consult this registry (see the kSlotJitMultimethods
// note + _jit_multifn_forget). One Runtime per thread, so this stays per-thread.
inline std::map<std::string, std::vector<JitMultiMethodEntry>>&
_jit_multimethods() {
  return culebra::runtime_substate<
      std::map<std::string, std::vector<JitMultiMethodEntry>>>(
      culebra::kSlotJitMultimethods);
}

// Recover the user-facing name from a multimethod registry key. Keys are
// `name\x1f<uid>`, one per dispatcher (see
// culebra_runtime_multifn_register_and_install); the suffix is internal, so
// diagnostics strip it back to the source name.
inline std::string_view _jit_multifn_display(std::string_view key) {
  auto sep = key.find('\x1f');
  return sep == std::string_view::npos ? key : key.substr(0, sep);
}

// Trait default-method bodies on the JIT side. Outer map keyed by
// trait name, inner by method name; each slot holds a +1 on its
// closure, released when the Runtime dies. Per-Runtime substate (same
// rationale as _jit_multimethods above; see the kSlotJitTraitDefaults
// note for the teardown ordering). The shared trait *definitions* live
// in culebra::trait_registry (mutex-guarded); only the compiled
// default-method closures are per-Runtime, so this table is isolated
// rather than locked.
struct _JitTraitDefaultTable {
  std::unordered_map<std::string,
                     std::unordered_map<std::string, JitClosure*>> entries;
  ~_JitTraitDefaultTable() {
    for (auto& [_, methods] : entries)
      for (auto& [__, cls] : methods)
        if (cls)
          _culebra_value_release_impl(TAG_FUNC,
                                      reinterpret_cast<int64_t>(cls));
  }
};
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, JitClosure*>>&
_jit_trait_default_impls() {
  return culebra::runtime_substate<_JitTraitDefaultTable>(
             culebra::kSlotJitTraitDefaults).entries;
}

// Runtime registration hook called from compile_trait_decl's emitted
// IR. The compiled default-method JitValue is unpacked at the call
// site; we receive the closure pointer plus the trait/method names
// as process-lifetime cstrings (module-level globals).
//
// Caller's `closure` arrives at +1 and the table takes that ownership
// outright — same seam as multifn_register_and_install. Install before
// releasing the displaced entry: that release can cascade through the
// displaced closure's capture chain, which re-enters the registry (the
// GC root walk reads it), so it must not observe a slot still holding a
// pointer that is being freed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_register_trait_default(const char* trait_name,
                                        const char* method_name,
                                        JitClosure* closure) {
  if (!closure) return;
  auto& slot = _jit_trait_default_impls()[trait_name][method_name];
  auto* displaced = slot;
  slot = closure;
  if (displaced) {
    _culebra_value_release_impl(TAG_FUNC,
                                reinterpret_cast<int64_t>(displaced));
  }
}

// Drop every registered default for `trait_name`, releasing each held +1.
// Emitted at the top of a trait declaration so a re-declared trait starts
// clean instead of stranding the old bodies' refs (the interp's
// `defaults.clear()`). Detach before release, same order as the register
// above.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_trait_defaults_reset(const char* trait_name) {
  auto& tbl = _jit_trait_default_impls();
  auto it = tbl.find(trait_name);
  if (it == tbl.end()) return;
  auto doomed = std::move(it->second);
  it->second.clear();
  for (auto& [_, cls] : doomed)
    if (cls)
      _culebra_value_release_impl(TAG_FUNC,
                                  reinterpret_cast<int64_t>(cls));
}

// Install the trait contract when its declaration *executes* — the interp's
// `register_trait` at the end of eval_trait_decl. A declaration replaces any
// earlier one of the same name outright, so a re-declaration that drops a
// required method or a supertrait really drops it.
//
// `spec` is `name:arity:has_default;...` and `supers` is `Name;...`, both
// compile-time constants (the shape culebra_runtime_register_packable uses).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_register_trait(const char* trait_name, const char* spec,
                                const char* supers) {
  culebra::TraitDef def;
  def.name = trait_name;
  std::string_view s(spec);
  for (size_t i = 0; i < s.size();) {
    size_t semi = s.find(';', i);
    if (semi == std::string_view::npos) semi = s.size();
    auto seg = s.substr(i, semi - i);
    i = semi + 1;
    auto c1 = seg.find(':');
    if (c1 == std::string_view::npos) continue;
    auto c2 = seg.find(':', c1 + 1);
    if (c2 == std::string_view::npos) continue;
    size_t arity = 0;
    for (char ch : seg.substr(c1 + 1, c2 - c1 - 1))
      arity = arity * 10 + static_cast<size_t>(ch - '0');
    def.methods.push_back({std::string(seg.substr(0, c1)), arity,
                           seg[c2 + 1] == '1'});
  }
  std::string_view sup(supers);
  for (size_t i = 0; i < sup.size();) {
    size_t semi = sup.find(';', i);
    if (semi == std::string_view::npos) semi = sup.size();
    if (semi > i) def.supertraits.emplace_back(sup.substr(i, semi - i));
    i = semi + 1;
  }
  culebra::register_trait(std::move(def));
}

// Side table mapping a dispatcher closure pointer to its multimethod
// name, so the shared static thunk can recover which name to dispatch
// for. A dispatcher is kept alive only by its env/module binding (a
// normal GC root) and owns its bodies via _jit_gc_enumerate_children, so
// it is reclaimed when its scope dies (entry dropped in _jit_multifn_forget).
// Per-Runtime substate (not thread_local): keyed by this Runtime's dispatcher
// closure pointers. A substate (paired with _jit_multimethods) so it outlives
// the module/namespace tables under ~Runtime's slot-ordered teardown — those
// tables release dispatcher closures that consult this map. One Runtime per
// thread, so no cross-thread sharing. See the kSlotJitMultifnNames note.
inline std::map<JitClosure*, std::string>&
_jit_multifn_dispatcher_names() {
  return culebra::runtime_substate<std::map<JitClosure*, std::string>>(
      culebra::kSlotJitMultifnNames);
}

// Non-owning body→dispatcher uplinks: the multifn self-recursion handle.
// A body's only +1 lives in its dispatcher's table entry, so the body can
// never outlive its dispatcher — the classic parent pointer. Entries are
// maintained wherever the table's are: registration inserts (and drops a
// displaced body's), _jit_multifn_forget clears the dead dispatcher's.
inline std::map<JitClosure*, JitClosure*>& _jit_multifn_body_uplinks() {
  return culebra::runtime_substate<std::map<JitClosure*, JitClosure*>>(
      culebra::kSlotJitMultifnUplinks);
}

// The self-handle read a multifn body's prologue emits: the dispatcher
// this body was registered into, +1 for the frame (alive for the whole
// call — the caller invoked us through it). A miss — a body handle that
// escaped via `fn` and outlived its dispatcher — returns the unbound
// sentinel, which the binding's read guard turns into the NameError an
// undeclared name gets.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue
culebra_runtime_multifn_self(JitClosure* body) {
  auto& uplinks = _jit_multifn_body_uplinks();
  auto it = uplinks.find(body);
  if (it == uplinks.end()) return JitValue{TAG_NO_SELF, 0};
  it->second->refcount++;
  return JitValue{TAG_FUNC, reinterpret_cast<int64_t>(it->second)};
}

// Function-value introspection. `cls` is a JitClosure*; `prop` is
// one of "name" / "return_type" / "params". Mirrors the interp side's
// Value::eval_property dispatch for Value::Function. Unknown
// properties return Nil. Used by emit_property_get when the
// receiver tag is TAG_FUNC.
// Bound-method trampoline address (defined below): a method read as a value
// captures its receiver in a wrapper closure whose fn_ptr is this thunk.
CULEBRA_RT_INLINE void _jit_bound_method_thunk(JitValue* __ret, JitClosure*, int8_t, int64_t,
                                                   int64_t, JitValue*);

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue
culebra_runtime_fn_introspect_get(JitClosure* cls, const char* prop) {
  // A bound method presents its underlying method's signature (the interp's
  // _wrap_method_with_this carries name/params/return_type through). Look
  // through to the captured method before resolving the metadata.
  if (cls && cls->fn_ptr == reinterpret_cast<void*>(&_jit_bound_method_thunk))
    cls = reinterpret_cast<JitClosure*>(cls->captures[1]->value.data);
  // A closure whose code is not a distinct fn_ptr (the VM executor's all
  // share one) answers by closure instead — the check_pos_count_cls seam.
  auto meta_of = [](JitClosure* c) -> const JitParamMeta* {
    if (!c) return nullptr;
    if (const JitParamMeta* m = _jit_lookup_param_meta(c->fn_ptr)) return m;
    if (_jit_closure_meta_hook) {
      if (const JitParamMeta* m = _jit_closure_meta_hook(c)) return m;
    }
    return _jit_native_meta_hook ? _jit_native_meta_hook(c) : nullptr;
  };
  const JitParamMeta* meta = meta_of(cls);
  // Multifn dispatcher fallback: dispatchers share a single thunk
  // address, so per-name body meta isn't keyed by fn_ptr. Walk the
  // dispatcher→name registry to find the body's meta — surfaces the
  // first registered method's signature (interp parity with the
  // dispatcher exposing the first method's params/name/return_type).
  if (!meta && cls) {
    auto& dispatchers = _jit_multifn_dispatcher_names();
    auto disp_it = dispatchers.find(cls);
    if (disp_it != dispatchers.end()) {
      auto& tbl = _jit_multimethods();
      auto method_it = tbl.find(disp_it->second);
      if (method_it != tbl.end() && !method_it->second.empty()) {
        meta = meta_of(method_it->second.front().body);
      }
    }
  }
  if (std::strcmp(prop, "name") == 0) {
    const char* n = (meta && meta->fn_name) ? meta->fn_name : "";
    auto* heap = _culebra_heap_str(std::string(n));
    return {TAG_STRING, reinterpret_cast<int64_t>(heap)};
  }
  if (std::strcmp(prop, "return_type") == 0) {
    const char* r = (meta && meta->return_type) ? meta->return_type : "";
    auto* heap = _culebra_heap_str(culebra::canonicalize_type_annotation(r));
    return {TAG_STRING, reinterpret_cast<int64_t>(heap)};
  }
  if (std::strcmp(prop, "params") == 0) {
    auto* arr = culebra_runtime_array_new();
    if (meta) {
      for (size_t i = 0; i < meta->n_params; i++) {
        auto* o = culebra_runtime_object_new();
        auto* nname = _culebra_heap_str(std::string(meta->names[i]));
        culebra_runtime_object_set(o, "name", false, TAG_STRING,
                                    reinterpret_cast<int64_t>(nname), 0, 0);
        bool m = meta->mut_bits &&
                 (meta->mut_bits[i / 8] & (1u << (i % 8)));
        culebra_runtime_object_set(o, "mut", false, TAG_BOOL,
                                    m ? 1 : 0, 0, 0);
        // Prefer declared (preserves `T`); fall back to effective.
        const char* tn = nullptr;
        if (meta->declared_type_names && meta->declared_type_names[i] &&
            meta->declared_type_names[i][0] != '\0') {
          tn = meta->declared_type_names[i];
        } else if (meta->type_names && meta->type_names[i]) {
          tn = meta->type_names[i];
        }
        auto* tname = _culebra_heap_str(
            culebra::canonicalize_type_annotation(tn ? tn : ""));
        culebra_runtime_object_set(o, "type", false, TAG_STRING,
                                    reinterpret_cast<int64_t>(tname), 0, 0);
        bool hd = meta->has_default_bits[i / 8] & (1u << (i % 8));
        culebra_runtime_object_set(o, "has_default", false, TAG_BOOL,
                                    hd ? 1 : 0, 0, 0);
        bool kr = meta->kwargs_rest_idx >= 0 &&
                  static_cast<int64_t>(i) == meta->kwargs_rest_idx;
        // kwargs_rest is reported via its dedicated flag — exclude it
        // from kw_only to match interp, which constructs the **rest
        // Parameter with kw_only=false regardless of separator order.
        bool ko = !kr && meta->first_kw_only_idx >= 0 &&
                  static_cast<int64_t>(i) >= meta->first_kw_only_idx;
        culebra_runtime_object_set(o, "kw_only", false, TAG_BOOL,
                                    ko ? 1 : 0, 0, 0);
        culebra_runtime_object_set(o, "kwargs_rest", false, TAG_BOOL,
                                    kr ? 1 : 0, 0, 0);
        culebra_runtime_array_push(arr, TAG_OBJECT,
                                    reinterpret_cast<int64_t>(o));
      }
    }
    return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
  }
  return {TAG_NIL, 0};
}

extern "C++" {  // C++ linkage (these take std::vector&); we sit in extern "C"
// --- Backstop collector callbacks (forward-declared near _gc_heap) ---------
// Push a JitValue's heap pointer as a GC child/root if it is refcounted.
inline void _gc_push_value(std::vector<void*>& out, const JitValue& v) {
  // TAG_STRING / TAG_STRINGVIEW are traced but not refcounted, so admit them
  // explicitly: a string or view held only inside a container must be marked
  // from its parent. A literal's data is not in the registry, so push() drops
  // it harmlessly; the view descriptor and its owner_base backing are, and get
  // marked (the descriptor here, the backing via enumerate_children).
  if (v.data && (_is_refcounted_value_tag(v.tag) || _jit_gc_is_traced_only(v.tag)))
    out.push_back(reinterpret_cast<void*>(v.data));
}

// extern "C" to match the definition's linkage (we sit in an extern "C++"
// island; the thunk itself lives in the surrounding extern "C" block).
extern "C" void _jit_multifn_dispatcher_thunk(JitValue*, JitClosure*, int8_t,
                                              int64_t, int64_t, JitValue*);

// A `fn name` dispatcher shares the one dispatch thunk; its overloads live in
// the thread_local `_jit_multimethods()` table, keyed by name. The table is
// invisible to normal GC tracing, so a dispatcher owns its method bodies via
// the three hooks below (children → keep alive while reachable; release/sweep
// → drop the table entry when the dispatcher dies). This replaces pinning
// every body as a permanent root, so a dead `fn name` is reclaimed like the
// interpreter's (its dispatcher shared_ptr releasing the table on scope exit).
inline bool _jit_is_multifn_dispatcher(JitClosure* c) {
  return c && c->fn_ptr == reinterpret_cast<void*>(&_jit_multifn_dispatcher_thunk);
}

// Forward-declared above for the trait-conformance walk: a dispatcher's own
// arity is just one overload's, so report the widest overload's positional
// count (mirrors the interp multimethod_for_each_overload walk).
inline size_t _jit_dispatcher_max_arity(JitClosure* cls) {
  size_t arity = cls ? cls->arity : 0;
  if (_jit_is_multifn_dispatcher(cls)) {
    auto& names = _jit_multifn_dispatcher_names();
    if (auto nit = names.find(cls); nit != names.end()) {
      auto& tbl = _jit_multimethods();
      if (auto mit = tbl.find(nit->second); mit != tbl.end())
        for (const auto& e : mit->second)
          arity = std::max(arity, e.param_types.size());
    }
  }
  return arity;
}

// Push a dispatcher's overload bodies as mark-phase children.
inline void _jit_multifn_push_bodies(JitClosure* c, std::vector<void*>& out) {
  auto& names = _jit_multifn_dispatcher_names();
  auto nit = names.find(c);
  if (nit == names.end()) return;
  auto& tbl = _jit_multimethods();
  auto mit = tbl.find(nit->second);
  if (mit == tbl.end()) return;
  for (auto& e : mit->second)
    if (e.body) out.push_back(e.body);
}

// Drop a dead dispatcher's table + name entries. `release_bodies` is true on
// the RC path (refcount hit 0 with no cycle → release each body's table-held
// +1; the cascade is precise, so pending `drop`s fire) and false on the
// sweep path (the bodies are reclaimed by their own sweep entries — releasing
// here would double-free).
inline void _jit_multifn_forget(JitClosure* c, bool release_bodies) {
  auto& names = _jit_multifn_dispatcher_names();
  auto nit = names.find(c);
  if (nit == names.end()) return;
  auto& tbl = _jit_multimethods();
  // Detach the table + name entries FIRST, then release: the natural
  // refcount-0 cascade fires pending `drop`s (user code) that may
  // re-declare this very name — it must find a clean table and a fresh
  // dispatcher, not the dying one. (Cycle-held remains park for the GC
  // backstop's finalize pass.)
  std::vector<JitMultiMethodEntry> doomed;
  if (auto mit = tbl.find(nit->second); mit != tbl.end()) {
    // The bodies' self-recursion uplinks die with the table entry on both
    // paths — the sweep reclaims the bodies through their own entries, and
    // a stale uplink would dangle at the raw-pointer layer.
    for (auto& e : mit->second)
      if (e.body) _jit_multifn_body_uplinks().erase(e.body);
    if (release_bodies) doomed = std::move(mit->second);
    tbl.erase(mit);
  }
  names.erase(nit);
  for (auto& e : doomed)
    if (e.body)
      _culebra_value_release_impl(TAG_FUNC,
                                  reinterpret_cast<int64_t>(e.body));
}

// Children of a live object, for the mark phase. Pushes raw pointers (the heap
// re-reads the tag from its registry). Captures (Cells) and `proto` are direct
// object pointers; all other children are refcounted JitValue payloads.
inline void _jit_gc_enumerate_children(void* obj, uint8_t tag,
                                       std::vector<void*>& out) {
  switch (tag) {
    case GC_TAG_FUNC: {
      auto* c = static_cast<JitClosure*>(obj);
      for (size_t i = 0; i < c->n_captures; i++)
        if (c->captures[i]) out.push_back(c->captures[i]);
      if (_jit_is_multifn_dispatcher(c)) _jit_multifn_push_bodies(c, out);
      break;
    }
    case GC_TAG_ARRAY:
    case GC_TAG_TUPLE: {
      auto* a = static_cast<JitArray*>(obj);
      for (size_t i = 0; i < a->size; i++) _gc_push_value(out, a->items[i]);
      break;
    }
    case GC_TAG_SET: {
      auto* s = static_cast<JitSet*>(obj);
      for (auto& m : s->members) _gc_push_value(out, m);
      break;
    }
    case GC_TAG_OBJECT: {
      auto* o = static_cast<JitObject*>(obj);
      for (auto& e : o->slots) _gc_push_value(out, e.value);
      // Walk each sidecar it still has, independently — NOT the map
      // through key_order. The release cascade tears key_order down
      // before it releases the map's entries, so a collect fired from an
      // entry's drop body mid-cascade would otherwise see no non-string
      // children at all and condemn the not-yet-released ones (their
      // finalize/sweep frees them under the cascade's feet — a
      // nondeterministic early drop, then a slab-corrupting release).
      // Marking is set-based, so pushing a key from both sidecars is
      // harmless.
      if (o->key_order)
        for (const auto& k : *o->key_order) _gc_push_value(out, k);
      if (o->non_string_props) {
        for (const auto& [k, e] : *o->non_string_props) {
          _gc_push_value(out, k);
          _gc_push_value(out, e.value);
        }
      }
      if (o->proto) out.push_back(o->proto);
      break;
    }
    case GC_TAG_CELL:
      _gc_push_value(out, static_cast<JitCell*>(obj)->value);
      break;
    case GC_TAG_STRINGVIEW: {
      // A view's single child is the backing String it borrows. Rooting it
      // keeps the traced-only backing alive for as long as the view is
      // reachable. owner_base is a registered String base or nullptr (an
      // untracked literal / interned name), which the mark drops harmlessly.
      auto* base = static_cast<JitStringView*>(obj)->owner_base;
      if (base) out.push_back(const_cast<char*>(base));
      break;
    }
  }
}

// Roots held in containers outside any scanned stack. Exhaustive over the
// jit.h-local global tables; the cached namespace objects (built in
// stdlib_jit.h) are instead pinned at creation, so they need no entry here.
inline void _jit_gc_enumerate_roots(std::vector<void*>& out) {
  for (auto& [_, v] : _jit_module_table()) _gc_push_value(out, v);
  for (auto& [_, methods] : _jit_trait_default_impls())
    for (auto& [__, cls] : methods)
      if (cls) out.push_back(cls);
  // Multimethod dispatchers and their bodies are NOT pinned here: a dispatcher
  // is kept alive by its env / module binding (a normal GC root) and owns its
  // bodies via _jit_gc_enumerate_children, so a `fn name` whose scope dies is
  // reclaimed (table entry dropped in release/sweep) — symmetric with interp.
  for (auto& v : _culebra_defer_stack()) _gc_push_value(out, v);
  auto& rt = culebra::current_runtime();
  if (rt.thrown_data)
    _gc_push_value(out, JitValue{rt.thrown_tag, rt.thrown_data});
  // In-flight algebraic-effect abort payloads (jit_runtime.h): each lives only
  // inside its unwinding CulebraEffAbort exception object, off the scanned
  // stack, so a collect mid-unwind would sweep it without this root.
  for (auto& v : _eff_abort_inflight) _gc_push_value(out, v);
}

// Backstop reclaim of one unmarked object: free its owned C++ buffers and
// `delete` the struct. NO `drop`, NO recursive child release — children are
// reclaimed by their own sweep entry, so each object is freed exactly once.
inline void _jit_gc_sweep_object(void* obj, uint8_t tag) {
  switch (tag) {
    case GC_TAG_FUNC: {
      auto* c = static_cast<JitClosure*>(obj);
      // A swept dispatcher is cycle garbage; drop its table entry (its bodies
      // have their own sweep entries, so don't release them here).
      if (_jit_is_multifn_dispatcher(c))
        _jit_multifn_forget(c, /*release_bodies=*/false);
      std::free(c->captures);
      delete c;
      break;
    }
    case GC_TAG_ARRAY:
    case GC_TAG_TUPLE: {
      auto* a = static_cast<JitArray*>(obj);
      delete[] a->items;
      delete a;
      break;
    }
    case GC_TAG_OBJECT: {
      auto* o = static_cast<JitObject*>(obj);
      _jit_owned_unregister(o);  // owned-stack tombstone (sweep path)
      delete o->key_order;
      delete o->non_string_props;
      delete o;
      break;
    }
    case GC_TAG_TENSOR:
      delete static_cast<JitTensor*>(obj);  // ~JitTensor drops the shared_ptr
      break;
    case GC_TAG_SET: {
      auto* s = static_cast<JitSet*>(obj);
      delete s->index;
      delete s;
      break;
    }
    case GC_TAG_CELL:
      delete static_cast<JitCell*>(obj);
      break;
    case GC_TAG_STRING: {
      // Registered at the bytes pointer; the slab base is the length header
      // just before it (see _str_alloc). No destructor, no children. Free
      // back to the slab with the exact byte count _str_alloc requested
      // (header + len + NUL), recomputed from the length header before it is
      // overwritten by the free-list link.
      auto* data = static_cast<char*>(obj);
      size_t total = sizeof(JitStrHeader) + _str_len(data) + 1;
      _slab().free(data - sizeof(JitStrHeader), total);
      break;
    }
    case GC_TAG_STRINGVIEW:
      // The descriptor is a trivial POD (borrowed ptr/len + owner_base edge);
      // the backing it borrows is a separate GC node, reclaimed on its own.
      std::free(obj);
      break;
  }
}
}  // extern "C++"

// Runtime type name for dispatch. For primitives this is the same as
// `_culebra_tag_name`, but for class instances (TAG_OBJECT with a
// `class:` String slot) we substitute the class name — that's why
// this can't share `_culebra_tag_name`'s tag-only switch. Mirrors
// `value_dyn_type` in interpreter.h.
inline std::string_view _jit_value_dyn_type(JitValue v) {
  switch (v.tag) {
    case TAG_NIL:        return "Nil";
    case TAG_BOOL:       return "Bool";
    case TAG_LONG:       return "Long";
    case TAG_FLOAT:      return "Float";
    case TAG_STRING:     return "String";
    case TAG_STRINGVIEW: return "StringView";
    case TAG_ARRAY:      return "Array";
    case TAG_TUPLE:      return "Tuple";
    case TAG_SET:        return "Set";
    case TAG_FUNC:       return "Function";
    case TAG_TENSOR:     return "Tensor";
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(v.data);
      if (auto idx = obj->find_slot("class");
          idx != static_cast<size_t>(-1)) {
        const auto& slot = obj->slots[idx].value;
        if (slot.tag == TAG_STRING) {
          return std::string_view(reinterpret_cast<const char*>(slot.data));
        }
      }
      return "Object";
    }
  }
  return "Object";
}

// Adapter: supplies the param-types accessor for the shared pick
// algorithm in interpreter.h.
inline int64_t _jit_multifn_pick(
    const std::vector<JitMultiMethodEntry>& methods,
    const std::vector<std::string_view>& arg_types,
    const std::vector<std::string_view>& kwarg_keys = {}) {
  return culebra::multifn_pick(
      methods, arg_types, kwarg_keys,
      [](const JitMultiMethodEntry& m) -> const std::vector<std::string>& {
        return m.param_types;
      },
      [](const JitMultiMethodEntry& m) { return m.variadic; },
      [](const JitMultiMethodEntry& m) { return m.min_params; },
      [](const JitMultiMethodEntry& m) -> const std::vector<std::string>& {
        return m.param_names;
      });
}

// Recover (body, name) for a multifn dispatcher closure given the
// positional arg types. Throws DispatchError on no-match / ambiguous;
// `release` runs before every throw, letting the kwargs-path caller
// drop the +1s it owns. Returns the picked method's body closure.
struct MultifnPick {
  JitClosure* body;
  std::string_view name;
};
inline MultifnPick _jit_multifn_resolve(
    JitClosure* cls, JitValue* positional, int64_t n_pos,
    int64_t line, int64_t col,
    std::function<void()> release = nullptr,
    const std::vector<std::string_view>& kwarg_keys = {}) {
  auto name_it = _jit_multifn_dispatcher_names().find(cls);
  if (name_it == _jit_multifn_dispatcher_names().end()) {
    if (release) release();
    throw std::runtime_error("internal: multimethod dispatcher missing name");
  }
  auto& tbl = _jit_multimethods();
  auto m_it = tbl.find(name_it->second);
  auto fail = [&](const char* what) -> std::string {
    if (release) release();
    return std::format("{} for `{}`", what,
                       _jit_multifn_display(name_it->second));
  };
  if (m_it == tbl.end()) {
    throw culebra::CulebraError("DispatchError",
        fail("no matching method"), line, col);
  }
  std::vector<std::string_view> arg_types(static_cast<size_t>(n_pos));
  for (size_t i = 0; i < arg_types.size(); i++) {
    arg_types[i] = _jit_value_dyn_type(positional[i]);
  }
  // Pre-populate trait conformance cache so multifn_specificity can
  // score `fn show(x: Stringer)` style trait params without holding
  // the arg instances. Mirrors interp's pick_method warm-up.
  for (const auto& trait_name : culebra::snapshot_trait_names()) {
    for (size_t i = 0; i < arg_types.size(); i++) {
      (void)_culebra_type_matches_single(positional[i].tag,
                                          positional[i].data, trait_name);
    }
  }
  auto pick = _jit_multifn_pick(m_it->second, arg_types, kwarg_keys);
  if (pick == -1) {
    throw culebra::CulebraError("DispatchError",
        fail("no matching method"), line, col);
  }
  if (pick == -2) {
    throw culebra::CulebraError("DispatchError",
        fail("ambiguous dispatch"), line, col);
  }
  return {m_it->second[static_cast<size_t>(pick)].body, name_it->second};
}

// JitFn-ABI shared thunk installed as `fn_ptr` on every multimethod
// dispatcher closure. The `cls` identity (load-bearing comparison in
// `culebra_runtime_call_with_kwargs` to intercept the kwargs path) is
// resolved through `_jit_multifn_dispatcher_names()` to recover the
// multimethod name, then dispatched via `_jit_multifn_resolve`.
inline void _jit_multifn_dispatcher_thunk(JitValue* __ret, JitClosure* cls,
                                          int8_t self_val_tag,
                                          int64_t self_val_data,
                                          int64_t n_args, JitValue* args) {
  JitValue self_val{self_val_tag, self_val_data};
  // On a normal dispatch `self_val` (a method receiver) and every arg are
  // forwarded to the picked body, which consumes them (callee-consumes). A
  // DispatchError throws before that hand-off, so the receiver + args would
  // strand — release them on the failure path (callee-cleans-on-dispatch-
  // error). A free-function dispatcher passes nil `self` (a no-op release).
  auto picked = _jit_multifn_resolve(
      cls, args, n_args, _jit_call_site_line, _jit_call_site_col, [&]() {
        _culebra_value_release_impl(self_val.tag, self_val.data);
        for (int64_t i = 0; i < n_args; i++)
          _culebra_value_release_impl(args[i].tag, args[i].data);
      });
  // Forward `self_val` to the picked body. For a free-function dispatcher the
  // call site passes nil and the body ignores it; for a class method-overload
  // dispatcher it carries the receiver, so the picked overload sees `self`.
  *__ret = _jit_invoke(picked.body, self_val, n_args, args);
}

// Record the call-site position read by `_jit_multifn_dispatcher_thunk` on
// the DispatchError path. Emitted by compile_function_call_raw just before
// an indirect closure call. Kept trivial so it inlines to two stores.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_call_site(
    int64_t line, int64_t col) {
  _jit_call_site_line = line;
  _jit_call_site_col = col;
  // The boundary position defaults to the call site (they coincide for
  // every non-UFCS call shape); a pending override published just before
  // this call (set_call_boundary — the UFCS chain head) wins and is
  // consumed, so it can never leak into a later call.
  if (_jit_pending_boundary_line || _jit_pending_boundary_col) {
    _jit_call_boundary_line = _jit_pending_boundary_line;
    _jit_call_boundary_col = _jit_pending_boundary_col;
    _jit_pending_boundary_line = 0;
    _jit_pending_boundary_col = 0;
  } else {
    _jit_call_boundary_line = line;
    _jit_call_boundary_col = col;
  }
  // Default the arg0 position to the call site: it is read only on the
  // (callback) body-coercion arg0 type-error path, where the call site is the
  // right position. The as-value path instead threads per-arg positions below.
  _jit_call_arg0_line = line;
  _jit_call_arg0_col = col;
  // Reset the per-arg position count: only the indirect (as-value) call path
  // repopulates it (via set_arg_pos) right after this. A HOF callback reaches
  // its per-element call with the count still 0, marking it as the callback
  // path so the dispatch uses body-coercion wording.
  _jit_argpos_n = 0;
}

// Publish the boundary position of the NEXT call (see _jit_call_boundary_*):
// a UFCS site's chain head, emitted after the arguments and immediately
// before the call, so the set_call_site inside the call consumes it before
// any other call can.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_call_boundary(
    int64_t line, int64_t col) {
  _jit_pending_boundary_line = line;
  _jit_pending_boundary_col = col;
}

// Record the position of the indirect call's i-th argument (see _jit_argpos_*),
// emitted per argument just before the call. Bounds-checked; the count tracks
// the highest index seen so the trampoline knows how many are valid.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_arg_pos(
    int64_t i, int64_t line, int64_t col) {
  if (i < 0 || i >= _JIT_ARGPOS_MAX) return;
  _jit_argpos_line[i] = line;
  _jit_argpos_col[i] = col;
  if (static_cast<int>(i) + 1 > _jit_argpos_n)
    _jit_argpos_n = static_cast<int>(i) + 1;
}

// Per-element invoke for the EAGER higher-order helpers: publish the HOF
// call site first so the callee prologue's typed-param position snapshot
// (culebra_runtime_param_pos) never reads a stale position left by the
// previous element's body — its inner calls overwrite the thread-locals.
// The lazy combinators get the same per-element refresh from
// _iter_publish_call_site / their stashed position cells.
inline JitValue _culebra_invoke1_at(JitClosure* fn, JitValue a,
                                    int64_t line, int64_t col) {
  culebra_runtime_set_call_site(line, col);
  return _culebra_invoke1(fn, a);
}
inline JitValue _culebra_invoke2_at(JitClosure* fn, JitValue a, JitValue b,
                                    int64_t line, int64_t col) {
  culebra_runtime_set_call_site(line, col);
  return _culebra_invoke2(fn, a, b);
}

// Publish the current op's source position for the positionless-error backfill
// (see `_jit_op_line`). Emitted just before a fallible runtime call; trivial so
// it inlines to two stores on the cold-relative-to-the-call path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_op_pos(
    int64_t line, int64_t col) {
  _jit_op_line = line;
  _jit_op_col = col;
}

// Resolve the position a typed-parameter error should report for param `idx`,
// interp-binder style: a positional argument reports at its own expression
// (set_arg_pos), a kwarg-/default-filled slot at the call site (set_call_site);
// the baked def position is the last resort for C++-driven entries that never
// ran set_call_site. Returned packed (line << 32 | col) so the prologue
// snapshot is one call per typed param. Snapshotted BEFORE defaults bind —
// a default expression's own calls clobber the thread-locals.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_param_pos(
    int64_t idx, int64_t def_line, int64_t def_col) {
  int64_t l = def_line, c = def_col;
  if (idx >= 0 && idx < _jit_argpos_n) {
    l = _jit_argpos_line[idx];
    c = _jit_argpos_col[idx];
  } else if (_jit_call_site_line) {
    l = _jit_call_site_line;
    c = _jit_call_site_col;
  }
  return _jit_pack_pos(l, c);
}

// Batched form of set_call_site + N× set_arg_pos: one runtime call per
// call site instead of N+1. `packed` is a compile-time constant array of
// (line << 32 | col) — every position the codegen publishes is static —
// so the emitter puts it in rodata. The resulting thread-local state is
// identical to the per-arg calls, including `_jit_argpos_n`, whose
// "0 = HOF body-coercion path" meaning downstream readers rely on.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_call_positions(
    int64_t line, int64_t col, int64_t n, const int64_t* packed) {
  culebra_runtime_set_call_site(line, col);
  int64_t k = n < _JIT_ARGPOS_MAX ? n : _JIT_ARGPOS_MAX;
  for (int64_t i = 0; i < k; i++) {
    auto pos = _jit_unpack_pos(packed[i]);
    _jit_argpos_line[i] = pos.line;
    _jit_argpos_col[i] = pos.col;
  }
  _jit_argpos_n = static_cast<int>(k);
}

// Record the position of a HOF's callback argument (see _jit_callback_arg_*),
// emitted by the JIT just before a HOF runtime call so a non-Function
// callback's type error points at the argument like the interp.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_callback_arg_site(
    int64_t line, int64_t col) {
  _jit_callback_arg_line = line;
  _jit_callback_arg_col = col;
}

// Append a method to a multimethod table (replacing an entry with an
// identical param-type sequence — same-declaration overload semantics).
// `into` is the dispatcher this declaration already installed earlier in
// the SAME activation (a preceding same-scope overload, or the previous
// arm of one class member's overload set); passing null mints a fresh
// dispatcher over a fresh table. That is the interp's rule (has_own on
// the declaring env frame): a decl re-run in a new activation — a `fn`
// inside a loop or a re-entered function, a class declaration executed
// twice — gets its own table, so its bodies keep that activation's
// captures instead of being displaced by the next run. The registry key
// is therefore per-dispatcher and internal; only its `name\x1f` prefix is
// user-facing (_jit_multifn_display). Returns the dispatcher with a +1 for
// the caller (the env binding), which owns the matching release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure*
culebra_runtime_multifn_register_and_install(const char* name_cstr,
                                             JitClosure* into,
                                             JitClosure* body,
                                             const char* const* param_types,
                                             int64_t n_param_types,
                                             int64_t variadic,
                                             int64_t min_arity,
                                             const char* const* param_names) {
  JitMultiMethodEntry method;
  // Caller's `body` arrives at +1; the table takes that ownership
  // outright. On replace, the displaced body's +1 is released below.
  method.body = body;
  method.variadic = variadic != 0;
  method.min_params = static_cast<size_t>(min_arity);
  method.param_types.reserve(static_cast<size_t>(n_param_types));
  method.param_names.reserve(static_cast<size_t>(n_param_types));
  for (int64_t i = 0; i < n_param_types; i++) {
    // Canonicalize so `Long|Float` and `Long | Float` dedup to one entry.
    method.param_types.emplace_back(culebra::canonicalize_type_annotation(
        param_types[i] ? param_types[i] : ""));
    method.param_names.emplace_back(
        param_names && param_names[i] ? param_names[i] : "");
  }

  // Resolve the dispatcher FIRST (+1 handed to the caller), before the
  // displaced body's release below: that release can cascade into the
  // previous declaration's whole capture chain and kill this very
  // dispatcher mid-install — its forget would then erase the table entry
  // the new body was just installed into (a re-declared `fn` would
  // dispatch into a void). The caller's +1 pins the dispatcher across the
  // cascade, so forget can only run once no newer registration owns it.
  auto& names = _jit_multifn_dispatcher_names();
  auto it = into ? names.find(into) : names.end();
  JitClosure* dispatcher = nullptr;
  std::string name;
  if (it != names.end()) {
    into->refcount++;  // hand a +1 back to the caller
    dispatcher = into;
    name = it->second;
  } else {
    // A fresh activation of this declaration: its own dispatcher over its
    // own table. The suffix only has to be unique within the Runtime's
    // table map; process-wide so a table deserialized from another
    // Runtime (sendable_jit.h keeps the sender's key) can't be collided
    // into by a locally minted one.
    static std::atomic<uint64_t> gen{0};
    name = std::string(name_cstr) + '\x1f' +
           std::to_string(gen.fetch_add(1, std::memory_order_relaxed));
    // closure_new returns +1; the caller becomes the owner.
    dispatcher = culebra_runtime_closure_new(
        reinterpret_cast<void*>(&_jit_multifn_dispatcher_thunk),
        /*n_captures=*/0, /*arity=*/n_param_types);
    names[dispatcher] = name;
  }

  auto& tbl = _jit_multimethods();
  auto& methods = tbl[name];
  bool replaced = false;
  for (auto& existing : methods) {
    if (existing.param_types == method.param_types) {
      // Install the new entry BEFORE releasing the displaced body's
      // table +1: the cascade fires pending `drop`s (user code) that
      // may dispatch this very name, so the table must not hold the
      // freed body. Cycle-held remains park for the GC backstop.
      JitClosure* displaced = existing.body;
      existing = std::move(method);
      replaced = true;
      _jit_multifn_body_uplinks().erase(displaced);
      _culebra_value_release_impl(TAG_FUNC,
                                  reinterpret_cast<int64_t>(displaced));
      break;
    }
  }
  if (!replaced) methods.push_back(std::move(method));
  // The body's self-recursion uplink (culebra_runtime_multifn_self); leaves
  // with the table entry, so it can never outlive the dispatcher it names.
  _jit_multifn_body_uplinks()[body] = dispatcher;
  return dispatcher;
}

// Auto-synthesized `class.parameters()` walker, mirroring the interp
// helper in interpreter.h::_walk_collect_params. Walks `val`
// recursively: Arrays are descended element-wise, plain Object dicts
// (no `class:` tag) are descended into their property values, and
// class instances are collected as leaves. Scalars are skipped.
// Property keys starting with '_' are skipped (private/cache fields).
// Each collected leaf is appended to `out` with its refcount bumped
// (+1 retained); the caller owns `out`'s eventual release.
inline void _jit_walk_collect_params(JitValue v, JitArray* out);

inline void _jit_walk_collect_params_object(JitObject* obj, JitArray* out) {
  // Both backends iterate Object properties in insertion order. The
  // Shape's names vector already stores them in declaration order.
  if (!obj->shape) return;
  for (size_t i = 0; i < obj->shape->names.size(); i++) {
    std::string_view key(obj->shape->names[i]);
    if (key == "class") continue;
    if (!key.empty() && key[0] == '_') continue;
    _jit_walk_collect_params(obj->slots[i].value, out);
  }
}

inline void _jit_walk_collect_params(JitValue v, JitArray* out) {
  if (v.tag == TAG_ARRAY) {
    auto* arr = reinterpret_cast<JitArray*>(v.data);
    for (size_t i = 0; i < arr->size; i++) {
      _jit_walk_collect_params(arr->items[i], out);
    }
  } else if (v.tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(v.data);
    if (obj->has_own("class")) {
      // Class instance — leaf parameter, collect.
      culebra_runtime_value_retain(v.tag, v.data);
      culebra_runtime_array_push(out, v.tag, v.data);
    } else {
      // Plain Object dict — recurse into values.
      _jit_walk_collect_params_object(obj, out);
    }
  }
}

// Entry point invoked from JIT IR for `model.parameters()` when the
// receiver is a class instance and has no user-defined `parameters`
// method. Returns a fresh +1 Array of class-instance Values.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_class_parameters_walk(
    JitObject* obj) {
  auto* result = culebra_runtime_array_new();
  _jit_walk_collect_params_object(obj, result);
  return {TAG_ARRAY, reinterpret_cast<int64_t>(result)};
}

// --- Cell runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitCell* culebra_runtime_cell_new(int8_t tag,
                                                               int64_t data) {
  auto* c = new JitCell();
  c->refcount = 1;
  c->value.tag = tag;
  c->value.data = data;
  _gc_register(c, GC_TAG_CELL);
  return c;
}

// --- Closure runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity) {
  auto* c = new JitClosure();
  c->refcount = 1;
  c->fn_ptr = fn_ptr;
  c->n_captures = n_captures;
  // calloc (zero-init): the caller fills captures[i] after this returns, and a
  // collect in that window would otherwise read uninitialised slots as roots.
  c->captures =
      n_captures ? static_cast<JitCell**>(
                       std::calloc(n_captures, sizeof(JitCell*)))
                 : nullptr;
  c->arity = arity;
  _gc_register(c, GC_TAG_FUNC);
  return c;
}

// Trampoline for a bound method value (the JIT twin of interp's
// _wrap_method_with_this). A method read as a VALUE — `let g = obj.m` — is
// wrapped in a closure whose captures are [receiver, method]. A value call
// (`g()`, a HOF, `__call__`) reaches every closure through its fn_ptr with
// `self` = Nil; this thunk substitutes the captured receiver so the method
// body sees the right `self`. It forwards args/arity unchanged, so it composes
// with every existing invoke path and with the callee's own arg handling.
inline void _jit_bound_method_thunk(JitValue* __ret,
    JitClosure* self, int8_t in_self_tag, int64_t in_self_data,
    int64_t n_args, JitValue* args) {
  // The binding is permanent: an incoming receiver (the wrapper attached to
  // another object and method-called) is IGNORED, but its +1 was transferred
  // to this callee — consume it. Held to scope exit, NOT released eagerly:
  // the incoming +1 may be the last ref to the object whose slot holds THIS
  // wrapper, and freeing it up front frees the capture cells mid-call
  // (ASan-confirmed UAF). NO_SELF/nil value calls make the release a no-op.
  JitOwnedVal in_self_guard(JitValue{in_self_tag, in_self_data});
  JitValue receiver = self->captures[0]->value;
  auto* method = reinterpret_cast<JitClosure*>(self->captures[1]->value.data);
  // The callee frame takes ownership of `self` on entry (a direct call retains
  // the receiver before passing it). Retain the captured receiver per call so
  // repeated invocations don't drain the wrapper's single held reference.
  // The callee consumes that +1 on EVERY exit — a compiled frame's self slot
  // releases on a normal return AND via its fn-level cleanup pad on a throw,
  // and native endpoints hold self in RAII (JitMethodSelf) — so no throw
  // guard belongs here. (Same contract as _culebra_callable_adapter, where
  // an extra guard release was an ASan-confirmed use-after-free.)
  culebra_runtime_value_retain(receiver.tag, receiver.data);
  { *__ret = _jit_invoke(method, receiver, n_args, args); return; }
}

// See through bound-method wrappers to the underlying method for
// introspection-style checks (callback arity bounds, param metadata):
// the thunk's own fn_ptr carries no meta, the wrapped method's does —
// the interp's wrapper copies the underlying params the same way.
// Invocation is NOT unwrapped; calls go through the thunk so `self`
// stays bound.
inline JitClosure* _jit_unwrap_bound_method(JitClosure* cls) {
  while (cls->fn_ptr ==
         reinterpret_cast<void*>(&_jit_bound_method_thunk)) {
    cls = reinterpret_cast<JitClosure*>(cls->captures[1]->value.data);
  }
  return cls;
}

// Resolve a property read AS A VALUE (`obj.name`, not `obj.name(...)`). When it
// is a class method — a TAG_FUNC reached through the proto, NOT an own slot —
// bind `self`=receiver into a wrapper so a later call sees the right receiver
// (interp's _wrap_method_with_this). Own-slot functions (namespace methods,
// constructors, lambda fields) and builtins keep their raw value; the own/proto
// split is decided by an own-slot lookup on `key` (a lambda stored in a field
// is an own slot, a method lives only on the proto). Returns a +1-owned value
// in every case (retaining the view, or the receiver+method into the wrapper's
// cells); the caller releases the receiver separately.
// Invoke a getter body 0-arg with `self`=receiver, returning its +1-owned
// result. The receiver is only borrowed (_culebra_invoke_method0 mints the
// callee's own +1), so a throwing getter leaves the caller's ref intact.
// Shared by the bare-read and introspection-name paths.
CULEBRA_RT_INLINE JitValue _jit_invoke_getter(JitClosure* method,
                                              int8_t recv_tag,
                                              int64_t recv_data) {
  return _culebra_invoke_method0(method, {recv_tag, recv_data});
}

// Build the [receiver, method] bound wrapper (shared by property value
// reads and the `fn` handle). Registers the thunk as native so
// sendable_jit rejects an escapee with the interp's SendError (a bound
// method has no params_ast there). Returns a +1-owned TAG_FUNC value.
CULEBRA_RT_INLINE JitValue _jit_make_bound_method(int8_t recv_tag,
                                                  int64_t recv_data,
                                                  JitClosure* method) {
  _jit_register_native_fn(
      reinterpret_cast<const void*>(&_jit_bound_method_thunk));
  auto* bound = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_jit_bound_method_thunk), 2, method->arity);
  culebra_runtime_value_retain(recv_tag, recv_data);
  bound->captures[0] = culebra_runtime_cell_new(recv_tag, recv_data);
  JitValue mval{TAG_FUNC, reinterpret_cast<int64_t>(method)};
  culebra_runtime_value_retain(mval.tag, mval.data);
  bound->captures[1] = culebra_runtime_cell_new(mval.tag, mval.data);
  return {TAG_FUNC, reinterpret_cast<int64_t>(bound)};
}

// The receiver a method call hands over — or none, when the callee is a
// promoted body local. A lowering's state object carries its body's locals as
// own slots, so `f()` in that body arrives here as `self.f()`; it is a bare
// call and must not acquire a receiver on the way in, exactly as the value
// read of the same slot does not acquire one (bind_method_value). Consumes the
// +1 the call site was going to pass on. Twin of the interp's eval_property
// branch, which reaches both spellings through one condition.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_call_receiver(
    int8_t recv_tag, int64_t recv_data, const char* key) {
  if (recv_tag == TAG_OBJECT) {
    auto* recv = reinterpret_cast<JitObject*>(recv_data);
    if (recv->proto && recv->proto->is_lowered_state && recv->has_own(key)) {
      _culebra_value_release_impl(recv_tag, recv_data);
      return {TAG_NO_SELF, 0};
    }
  }
  return {recv_tag, recv_data};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_bind_method_value(
    int8_t recv_tag, int64_t recv_data, int8_t view_tag, int64_t view_data,
    const char* key) {
  if (view_tag == TAG_FUNC && recv_tag == TAG_OBJECT) {
    auto* method = reinterpret_cast<JitClosure*>(view_data);
    // A promoted body local is storage, not a method — hand the raw function
    // back unbound (culebra::is_lowered_state_class). Own slots only: the
    // state class's own methods live on the proto and bind as usual. Twin of
    // the interp's eval_property branch.
    auto* recv = reinterpret_cast<JitObject*>(recv_data);
    if (recv->proto && recv->proto->is_lowered_state && recv->has_own(key)) {
      culebra_runtime_value_retain(view_tag, view_data);
      return {view_tag, view_data};
    }
    // A getter read as a value auto-invokes 0-arg with `self`=receiver
    // (interp's eval_property getter branch). Reached only on the bare-read
    // path; `obj.name()` compiles as a method call and never lands here, so
    // both spellings yield the same value. `view` is borrowed here (the
    // caller releases the receiver separately), so nothing to release.
    // Only class-compiled getters register; own-slot lambdas never match.
    if (_jit_is_getter_fn(method->fn_ptr)) {
      return _jit_invoke_getter(method, recv_tag, recv_data);
    }
    // Bind `self`=receiver for EVERY function-valued property read — own
    // slot, proto method, static, ctor alike. The interp's
    // _wrap_method_with_this wraps them all, a fresh wrapper per read
    // (so `o.f == o.f` is false there, and now here).
    return _jit_make_bound_method(recv_tag, recv_data, method);
  }
  culebra_runtime_value_retain(view_tag, view_data);
  return {view_tag, view_data};
}

// Value-read of the `fn` recursion handle (see compile_identifier). With a
// receiver (or lexical fallback) in `self`, the handle is the bound wrapper
// — interp parity: a method call's `fn` IS the wrapper, so recursion and
// escapees keep the original receiver, and a later dynamic receiver does
// NOT override it (the thunk ignores the incoming self). Cached per frame
// in `*cache` (an owned slot: reads compare equal, the frame exit releases
// the single +1). Without a receiver the raw closure is returned — a plain
// call's `fn` stays the raw, still-Sendable function.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_fn_handle(
    int8_t self_tag, int64_t self_data, JitClosure* cls, JitValue* cache) {
  if (self_tag == TAG_NO_SELF) {
    JitValue raw{TAG_FUNC, reinterpret_cast<int64_t>(cls)};
    culebra_runtime_value_retain(raw.tag, raw.data);
    return raw;
  }
  if (cache->tag != TAG_FUNC) {
    *cache = _jit_make_bound_method(self_tag, self_data, cls);
  }
  culebra_runtime_value_retain(cache->tag, cache->data);
  return *cache;
}

// Frame-prologue lexical merge for `self` (see compile_fn_common): a
// dynamic receiver wins (its +1 transfers straight through); with none,
// fall back to the captured enclosing frame's self cell, minting the
// slot's +1 here. NO_SELF in both means the frame keeps the sentinel and
// a read raises the interp's NameError via the read guard.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_self_merge(
    int8_t self_tag, int64_t self_data, JitCell* lex_cell) {
  if (self_tag != TAG_NO_SELF) return {self_tag, self_data};
  JitValue v = lex_cell->value;
  culebra_runtime_value_retain(v.tag, v.data);
  return v;
}

// The `.name` / `.params` / `.return_type` value-read path skips
// bind_method_value (those names are function-introspection on a TAG_FUNC
// receiver, whose resolved `view` is a fresh +1-owned result, not a borrowed
// method to bind). But on a class instance those same names may be getters, so
// fire them here. `view` is +1 OWNED (emit_property_get's fn_mode retains
// the object path too), so on the getter branch release the owned method view
// after invoking; on the pass-through branch return it unchanged (ownership
// flows to the caller, which does own(view) with no extra retain).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_getter_or_value(
    int8_t recv_tag, int64_t recv_data, int8_t view_tag, int64_t view_data,
    const char* key) {
  if (view_tag == TAG_FUNC && recv_tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(recv_data);
    if (obj->proto && obj->find_slot(key) == static_cast<size_t>(-1)) {
      auto* method = reinterpret_cast<JitClosure*>(view_data);
      if (_jit_is_getter_fn(method->fn_ptr)) {
        JitValue r = _jit_invoke_getter(method, recv_tag, recv_data);
        _culebra_value_release_impl(view_tag, view_data);  // drop owned view
        return r;
      }
    }
  }
  return {view_tag, view_data};
}

// Runtime kwarg resolver: routes a call carrying keyword arguments
// and/or dynamic `**splat` operands against a closure whose parameter
// names live in the JitParamMeta side table. Mirrors the interp's
// `bind_call_args` algorithm — splats merge first (later wins),
// explicit kwargs layer on top. Missing defaulted slots get the
// `TAG_UNFILLED` sentinel; the callee prologue's existing default
// branch picks them up. The slab also carries any overflow positional
// args past the formal arity so `__ARGS__` continues to work.
//
// Refcount contract: caller transfers +1 on each positional, kwargs
// value, and splat Object. All transferred values either flow into
// the dispatched call (consumed by the callee frame) or are released
// here on the throw paths.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_call_with_kwargs(
    JitClosure* cls, int8_t self_val_tag, int64_t self_val_data,
    int64_t n_pos, JitValue* positional,
    int64_t n_kw, const char* const* kw_keys, JitValue* kw_vals,
    int64_t n_splat, JitValue* splat_objs,
    int64_t line, int64_t col) {
  JitValue self_val{self_val_tag, self_val_data};
  auto release_owned = [&](size_t skip_pos = 0) {
    // Drop every +1 the caller transferred to us that hasn't already
    // been consumed downstream. `skip_pos` lets us skip positionals
    // that have already been moved into a slab cell about to be
    // handed to the callee.
    for (int64_t i = static_cast<int64_t>(skip_pos); i < n_pos; i++) {
      _culebra_value_release_impl(positional[i].tag, positional[i].data);
    }
    for (int64_t i = 0; i < n_kw; i++) {
      _culebra_value_release_impl(kw_vals[i].tag, kw_vals[i].data);
    }
    for (int64_t i = 0; i < n_splat; i++) {
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
    }
    // callee-cleans-on-direct-binding-error: `self` is passed +1 (the callee
    // frame consumes it on the normal hand-off at the tail). Every throw here
    // aborts before that hand-off, so the receiver would strand (a method
    // receiver on `obj.m(bad: 1)`). release_owned runs only on the throw /
    // dispatch-failure paths — never before a hand-off — so this is safe.
    _culebra_value_release_impl(self_val.tag, self_val.data);
  };

  // A bound method value (`obj.m` read as a value) wraps [receiver, method];
  // its trampoline fn_ptr carries no JitParamMeta, so recurse into the
  // underlying method with `self` = the captured receiver (retained, since the
  // callee frame consumes `self` — same as the positional trampoline). Without
  // this a kwarg call `g(x: 1)` would wrongly hit the "no keyword arguments"
  // reject on the wrapper's null meta.
  if (cls->fn_ptr == reinterpret_cast<void*>(&_jit_bound_method_thunk)) {
    JitValue receiver = cls->captures[0]->value;
    auto* method = reinterpret_cast<JitClosure*>(cls->captures[1]->value.data);
    culebra_runtime_value_retain(receiver.tag, receiver.data);
    return culebra_runtime_call_with_kwargs(
        method, static_cast<int8_t>(receiver.tag), receiver.data, n_pos,
        positional, n_kw, kw_keys, kw_vals, n_splat, splat_objs, line, col);
  }

  // stdlib namespace methods route through the hook FIRST — before the
  // splat validation below — because an ns-method (strict_arity) checks its
  // positional arity *before* validating splat operands, mirroring the
  // interp's strict_arity block: `JSON.stringify(**5)` is `ArityError: got 0`,
  // not a splat TypeError. The resolver does that ordering itself; reaching the
  // splat-first validation below would pre-empt it. (User fns / multifn
  // dispatchers, handled after, are splat-first like the interp's user binder.)
  if (_jit_ns_kwarg_hook) {
    JitValue out;
    if (_jit_ns_kwarg_hook(cls, self_val, n_pos, positional, n_kw, kw_keys,
                           kw_vals, n_splat, splat_objs, line, col, &out)) {
      return out;
    }
  }

  // Validate `**` splat operands up front — before the multifn dispatcher
  // branch / meta lookup (but after the ns hook, see above). A non-Object or
  // non-String-keyed operand must raise the interp's TypeError ("**: splat
  // operand must be Object" / "**: splat key must be String"), not be silently
  // skipped while building the dispatcher's kwarg key set and then surface as a
  // DispatchError.
  for (int64_t i = 0; i < n_splat; i++) {
    auto sv = splat_objs[i];
    if (sv.tag != TAG_OBJECT) {
      release_owned();
      throw culebra::CulebraError("TypeError", std::format(
          "**: splat operand must be Object, got {}",
          _culebra_tag_name(sv.tag)), line, col);
    }
    auto* obj = reinterpret_cast<JitObject*>(sv.data);
    if (obj->non_string_props && !obj->non_string_props->empty()) {
      release_owned();
      throw culebra::CulebraError("TypeError",
          "**: splat key must be String", line, col);
    }
  }

  // Multifn dispatchers route through here too: their fn_ptr has no
  // JitParamMeta (kwargs flow on top of the existing positional pick,
  // Julia kwsorter style). Pick on positional types, then recurse
  // into the picked method — which is a regular user fn, never a
  // dispatcher, so the recursion bottoms out in the meta-lookup
  // branch below.
  if (cls->fn_ptr == reinterpret_cast<void*>(&_jit_multifn_dispatcher_thunk)) {
    // Keyword names (explicit + `**` splat) so dispatch can let a kwarg cover
    // a required param. Mirrors interp's __KWARGS__ key set.
    std::vector<std::string_view> kwarg_keys;
    for (int64_t i = 0; i < n_kw; i++) kwarg_keys.push_back(kw_keys[i]);
    for (int64_t i = 0; i < n_splat; i++) {
      // Operands were validated as Objects at function entry.
      auto* obj = reinterpret_cast<JitObject*>(splat_objs[i].data);
      if (obj->shape)
        for (const auto& nm : obj->shape->names) kwarg_keys.push_back(nm);
    }
    auto picked = _jit_multifn_resolve(
        cls, positional, n_pos, line, col, release_owned, kwarg_keys);
    return culebra_runtime_call_with_kwargs(
        picked.body, static_cast<int8_t>(self_val.tag), self_val.data, n_pos,
        positional, n_kw, kw_keys, kw_vals,
        n_splat, splat_objs, line, col);
  }

  const JitParamMeta* meta = _jit_lookup_param_meta(cls->fn_ptr);
  if (!meta && _jit_closure_meta_hook) meta = _jit_closure_meta_hook(cls);
  if (!meta) {
    // No parameter metadata means the closure's parameters live behind its
    // own prologue or trampoline (a VM function, a native builtin wrapper) —
    // the side table only describes AST-compiled functions. With keyword
    // content present that is a real error: nothing here can bind a name.
    // Without any (a 0-kwarg call routed through this entry, e.g.
    // get_or_put's lazy-init thunk), there is no binder work at all — hand
    // the positional slab straight to the closure like an ordinary call
    // site, publishing the call site first so the callee's own arity and
    // recursion guards report there (where the interp anchors these too).
    // The hand-off contract is the same as the meta path's tail below.
    if (n_kw == 0 && n_splat == 0) {
      culebra_runtime_set_call_site(line, col);
      return _jit_invoke(cls, self_val, n_pos, positional);
    }
    release_owned();
    throw culebra::CulebraError("TypeError",
        "function does not accept keyword arguments", line, col);
  }

  // Merge each splat Object's String-keyed entries into a single
  // name → JitValue map. Operands were already validated (Object +
  // String keys) at function entry, so no re-check here.
  culebra::MergedKwargs<JitValue> merged;
  for (int64_t i = 0; i < n_splat; i++) {
    auto* obj = reinterpret_cast<JitObject*>(splat_objs[i].data);
    if (!obj->shape) continue;
    for (size_t k = 0; k < obj->shape->names.size(); k++) {
      // Retain each value so the merged map owns +1; the matching
      // release lands either on the slab transfer or in the catch
      // paths below.
      auto& sv_entry = obj->slots[k].value;
      culebra_runtime_value_retain(sv_entry.tag, sv_entry.data);
      auto it = merged.find(obj->shape->names[k]);
      if (it != merged.end())
        _culebra_value_release_impl(it->second.tag, it->second.data);
      merged.set(obj->shape->names[k], sv_entry);
    }
  }

  // Layer explicit kwargs on top (overwriting any splat-contributed
  // binding for the same key; duplicate among explicit names was
  // already rejected at the IR scan).
  for (int64_t i = 0; i < n_kw; i++) {
    auto it = merged.find(kw_keys[i]);
    if (it != merged.end())
      _culebra_value_release_impl(it->second.tag, it->second.data);
    merged.set(kw_keys[i], kw_vals[i]);
  }

  // Build resolved slab: one entry per formal param, plus extras.
  // Required slots must be filled by positional or merged kwargs.
  size_t arity = meta->n_params;
  // Positionals only reach the regular parameters: a keyword-only slot and
  // the `**rest` catch-all are named-only, so anything past the last regular
  // one is an overflow argument (the interp's regular_param_count boundary).
  size_t regular_end = arity;
  if (meta->first_kw_only_idx >= 0)
    regular_end = static_cast<size_t>(meta->first_kw_only_idx);
  else if (meta->kwargs_rest_idx >= 0)
    regular_end = static_cast<size_t>(meta->kwargs_rest_idx);
  size_t n_extras = (n_pos > static_cast<int64_t>(regular_end))
                        ? static_cast<size_t>(n_pos) - regular_end
                        : 0;
  std::vector<JitValue> slab(arity + n_extras);
  std::vector<bool> filled(arity, false);

  for (size_t i = 0; i < regular_end && i < static_cast<size_t>(n_pos); i++) {
    auto it = merged.find(meta->names[i]);
    if (it != merged.end()) {
      _culebra_value_release_impl(it->second.tag, it->second.data);
      merged.erase(it);
      // Release everything we still own and the splat refs we already
      // dropped via release_owned were not in scope yet.
      for (size_t k = 0; k < i; k++) {
        _culebra_value_release_impl(slab[k].tag, slab[k].data);
      }
      for (size_t k = i; k < static_cast<size_t>(n_pos); k++) {
        _culebra_value_release_impl(positional[k].tag, positional[k].data);
      }
      for (auto& [_, v] : merged) {
        _culebra_value_release_impl(v.tag, v.data);
      }
      for (int64_t k = 0; k < n_splat; k++) {
        _culebra_value_release_impl(splat_objs[k].tag, splat_objs[k].data);
      }
      _culebra_value_release_impl(self_val.tag, self_val.data);
      throw culebra::CulebraError("TypeError",
          culebra::positional_kw_conflict_message(meta->names[i]), line, col);
    }
    slab[i] = positional[i];
    filled[i] = true;
  }

  for (size_t i = std::min(static_cast<size_t>(n_pos), regular_end);
       i < arity; i++) {
    if (static_cast<int64_t>(i) == meta->kwargs_rest_idx) continue;
    auto it = merged.find(meta->names[i]);
    if (it != merged.end()) {
      slab[i] = it->second;
      filled[i] = true;
      merged.erase(it);
    }
  }

  // Validate required slots are filled *before* rejecting leftover kwargs,
  // mirroring the interp binder's order: it walks formal params in
  // declaration order (raising "missing required argument" as it hits an
  // unfilled required slot) and only treats still-unconsumed kwargs as
  // "unknown" afterwards. So `f(bad: 1)` on `fn f(x)` is a missing-x
  // ArityError, not an unknown-bad TypeError. Defaulted slots get
  // TAG_UNFILLED so the callee prologue takes the default branch. The
  // `**rest` slot is always satisfied (filled below, even if empty), so
  // skip it here.
  for (size_t i = 0; i < arity; i++) {
    if (filled[i]) continue;
    if (static_cast<int64_t>(i) == meta->kwargs_rest_idx) continue;
    bool defaulted =
        meta->has_default_bits[i / 8] & (1u << (i % 8));
    if (!defaulted) {
      auto missing = meta->names[i];
      for (size_t k = 0; k < arity; k++) {
        if (filled[k]) _culebra_value_release_impl(slab[k].tag,
                                                    slab[k].data);
      }
      // Leftover kwargs are still owned here (the unknown-keyword check
      // below has not run yet); release them on this early-exit path.
      for (auto& [_, v] : merged) {
        _culebra_value_release_impl(v.tag, v.data);
      }
      for (int64_t k = 0; k < n_splat; k++) {
        _culebra_value_release_impl(splat_objs[k].tag, splat_objs[k].data);
      }
      _culebra_value_release_impl(self_val.tag, self_val.data);
      throw culebra::CulebraError("ArityError",
          culebra::missing_required_arg_message(missing), line, col);
    }
    slab[i] = {TAG_UNFILLED, 0};
  }

  // Leftover kwargs flow into a `**rest` slot if declared, otherwise
  // they are unknown to this function. The rest slot is always
  // populated (even with an empty Object) so the callee always sees
  // a bound variable.
  if (meta->kwargs_rest_idx >= 0) {
    auto* rest_obj = culebra_runtime_object_new();
    for (auto& [k, v] : merged) {
      culebra_runtime_object_set(rest_obj, std::string(k).c_str(),
                                  /*mut=*/false, v.tag, v.data, line, col);
    }
    merged.clear();
    // TAG_KWREST, not TAG_OBJECT: the callee prologue reads this slot to tell
    // a resolved slab from a plain positional call, where the same index
    // carries an overflow argument instead.
    slab[meta->kwargs_rest_idx] = {TAG_KWREST,
                                    reinterpret_cast<int64_t>(rest_obj)};
    filled[meta->kwargs_rest_idx] = true;
  } else if (!merged.empty()) {
    auto bad_name = std::string(culebra::canonical_unknown_kwarg(merged));
    for (auto& [_, v] : merged) {
      _culebra_value_release_impl(v.tag, v.data);
    }
    for (size_t i = 0; i < slab.size(); i++) {
      if (i < arity && !filled[i]) continue;
      _culebra_value_release_impl(slab[i].tag, slab[i].data);
    }
    for (int64_t i = 0; i < n_splat; i++) {
      _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
    }
    _culebra_value_release_impl(self_val.tag, self_val.data);
    throw culebra::CulebraError("TypeError",
        culebra::unknown_kwarg_message(bad_name), line, col);
  }

  // Extras past the last regular parameter flow into `__ARGS__`, which the
  // callee reads from the slab's tail — past the arity, so the named-only
  // slots in between keep their bindings.
  for (size_t i = 0; i < n_extras; i++) {
    slab[arity + i] = positional[regular_end + i];
  }

  // Splat Objects are no longer needed: their values were retained on
  // entry to merged and either consumed into the slab or released
  // above. Drop our +1 to each Object itself.
  for (int64_t i = 0; i < n_splat; i++) {
    _culebra_value_release_impl(splat_objs[i].tag, splat_objs[i].data);
  }

  return _jit_invoke(cls, self_val, static_cast<int64_t>(slab.size()),
                     slab.data());
}

}  // extern "C" (block continues in jit_iter.h)

#endif  // CULEBRA_JIT_ENABLED
