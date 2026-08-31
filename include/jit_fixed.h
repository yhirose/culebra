#pragma once

// FixedArray / FixedSet / FixedMap views (byte ops shared with the
// [i] index hooks), packable registration, object any-key runtime,
// inline caches and class meta/instance construction.
//
// Runtime-layer fragment of rt.h, split out for readability. These
// fragments rely on rt.h's #include block and are included by rt.h in a
// fixed sequence (see rt.h); they are not standalone headers.

// Continues the runtime extern "C" block opened in jit_runtime.h
// (split across jit_fixed.h / jit_dispatch.h / jit_iter.h).
extern "C" {

// --- FixedArray view byte helpers (used by both the [i] index hooks here
// and the FixedArray view native methods below) -------------------------
inline std::shared_ptr<culebra::SharedBufferCore> _jit_fa_core(JitObject* v) {
  int64_t id = v->slots[v->find_slot("__fa_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core)
    throw culebra::CulebraError("ValueError", "SharedBuffer has been dropped");
  return core;
}
inline int64_t _jit_fa_field_long(JitObject* v, const char* key) {
  return v->slots[v->find_slot(key)].value.data;
}
inline culebra::PackableField _jit_fa_elem_field(JitObject* v) {
  return culebra::PackableField::scalar(culebra::packable_scalar_name(
      static_cast<int>(_jit_fa_field_long(v, "__fa_ecode__"))));
}
inline int64_t _jit_fa_len(JitObject* v) {
  auto core = _jit_fa_core(v);
  int32_t n;
  std::memcpy(&n, core->data + _jit_fa_field_long(v, "__fa_off__"), 4);
  return n;
}
inline uint8_t* _jit_fa_elem_ptr(JitObject* v, int64_t i) {
  return _jit_fa_core(v)->data + _jit_fa_field_long(v, "__fa_off__") +
         _jit_fa_field_long(v, "__fa_dataoff__") +
         i * _jit_fa_field_long(v, "__fa_esize__");
}
inline JitValue _jit_fa_get(JitObject* v, int64_t i, int64_t line = 0,
                            int64_t col = 0) {
  int64_t n = _jit_fa_len(v);
  if (i < 0) i += n;
  if (i < 0 || i >= n)
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  return _jit_packable_read_field(_jit_fa_elem_ptr(v, i), _jit_fa_elem_field(v));
}
inline void _jit_fa_set(JitObject* v, int64_t i, int8_t tag, int64_t data,
                        int64_t line = 0, int64_t col = 0) {
  int64_t n = _jit_fa_len(v);
  if (i < 0) i += n;
  if (i < 0 || i >= n)
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  _jit_packable_write_field(_jit_fa_elem_ptr(v, i), _jit_fa_elem_field(v), tag,
                            data);
}

// Registry of native (C++-bodied) closure fn_ptrs. A native closure is not
// Sendable: it can't be rebuilt on another Runtime, and its captures may
// hold raw same-heap pointers (the iterator wrappers cache upstream closure
// pointers, the ns-method closure a NsMethod*) that would silently cross
// the heap boundary — the JIT serializer rejects them like the interp's
// body==nullptr check (sendable.h). Every C++-side closure builder
// registers its thunk; the multifn dispatcher stays out (the serializer
// rebuilds it from the method tables). Mutex-guarded: child isolates build
// natives concurrently. The set is bounded by the number of distinct C++
// thunk addresses, so it only ever holds a handful of entries.
inline std::mutex& _jit_native_fns_mutex() {
  static std::mutex m;
  return m;
}
inline std::unordered_set<const void*>& _jit_native_fns() {
  static std::unordered_set<const void*> s;
  return s;
}
inline void _jit_register_native_fn(const void* fn_ptr) {
  // Per-thread memo: iterator wrappers register on every construction, so
  // skip the mutex once this thread has seen the thunk (the global set is
  // append-only, so a hit can never go stale).
  static thread_local std::unordered_set<const void*> seen;
  if (!seen.insert(fn_ptr).second) return;
  std::lock_guard<std::mutex> lk(_jit_native_fns_mutex());
  _jit_native_fns().insert(fn_ptr);
}
inline bool _jit_is_native_fn(const void* fn_ptr) {
  std::lock_guard<std::mutex> lk(_jit_native_fns_mutex());
  return _jit_native_fns().contains(fn_ptr);
}

// Getter registry — the JIT twin of interp's FunctionValue::is_getter. A class
// getter's compiled body address is registered here when its class declaration
// runs; a bare property read (`obj.name`, no call parens) that resolves to a
// proto method whose fn_ptr is in this set invokes it 0-arg instead of yielding
// a bound method (culebra_runtime_bind_method_value). Append-only and keyed by
// the unique compiled-body address, so a hit never goes stale and can't collide
// with a plain method. Shares the native-fn mutex (both are cold, decl-time).
inline std::unordered_set<const void*>& _jit_getter_fns() {
  static std::unordered_set<const void*> s;
  return s;
}
inline void _jit_register_getter_fn(const void* fn_ptr) {
  static thread_local std::unordered_set<const void*> seen;
  if (!seen.insert(fn_ptr).second) return;
  std::lock_guard<std::mutex> lk(_jit_native_fns_mutex());
  _jit_getter_fns().insert(fn_ptr);
}
inline bool _jit_is_getter_fn(const void* fn_ptr) {
  std::lock_guard<std::mutex> lk(_jit_native_fns_mutex());
  return _jit_getter_fns().contains(fn_ptr);
}

// Emitted-code entry point: register a compiled getter closure by its body
// address (only known once the class declaration runs).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_getter(
    JitClosure* method) {
  _jit_register_getter_fn(method->fn_ptr);
}

// Emitted-code entry point: a class declaration registers its synthesized
// constructor here at runtime (the compiled address only exists then). The
// ctor is native on the interp side (body == nullptr), so neither backend
// can ship it — registering makes capturing a class object or `C.new`
// reject with the interp's exact message.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_native_fn(
    void* fn_ptr) {
  _jit_register_native_fn(fn_ptr);
}

// Handle plumbing shared by every native-handle builder (Proc / File /
// Foreign / wrap.h-generated handles): a typed slot read, the
// captureless method-closure constructor, and the bind helper. Live
// here (not stdlib_jit.h) so wrap.h's generated thunks can use them.
inline int64_t _jit_handle_long(JitObject* h, const char* key) {
  size_t i = h->find_slot(key);
  return i == static_cast<size_t>(-1) ? -1 : h->slots[i].value.data;
}

// Native-handle method param metadata (kwargs binding) — built by
// _jit_make_handle_meta (defined after JitParamMeta below) and passed
// straight to the bind chokepoint, so no process-global relay is needed.
struct JitParamMeta;
inline const JitParamMeta* _jit_make_handle_meta(
    std::vector<std::string> names, std::vector<bool> has_default);
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_param_meta(
    void* fn_ptr, const JitParamMeta* meta);

inline JitClosure* _jit_make_handle_method(
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*), size_t arity,
    const JitParamMeta* meta = nullptr) {
  _jit_register_native_fn(reinterpret_cast<const void*>(fn));
  auto* cls = new JitClosure();
  cls->refcount = 1;
  cls->fn_ptr = reinterpret_cast<void*>(fn);
  cls->n_captures = 0;
  cls->captures = nullptr;
  cls->arity = arity;
  // Seed this thread's param-meta table so the kwargs resolver can bind
  // by name. The table is thread_local and register is idempotent (same
  // fn_ptr → same meta), so concurrent isolates building the same handle
  // each seed their own table with no shared mutable state.
  if (meta)
    culebra_runtime_register_param_meta(reinterpret_cast<void*>(fn), meta);
  _gc_register(cls, GC_TAG_FUNC);
  return cls;
}

// Slot a handle method onto `h` — shared by every native-handle builder
// (Proc / File / Foreign). `meta` (non-null) registers the method's
// param names so it accepts keyword arguments like the interp; the
// drop / no-param methods pass nullptr.
//
// A method bound here is entered through a thunk ABI with no line/col, so a
// positionless error escaping it prints without a location where the interp
// stamps the call AST. The entry point owns the backfill: wrap the body in
// `_jit_at_call_site(...)` (see the Http handles, and
// surface_native_error_at_call_site for every wrapped C++ class).
// Not done here for all of them: a `drop` runs from the drop protocol at scope
// exit, where the published call site belongs to some unrelated earlier call.
inline void _jit_handle_bind_method(
    JitObject* h, const char* name,
    void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*), size_t ar,
    const JitParamMeta* meta = nullptr) {
  h->set_or_append(name,
      JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
          _jit_make_handle_method(f, ar, meta))}, false);
}


// A captureless native method closure (reads its state from `self`). Generic
// JIT-object helper — channel/isolate/SharedBuffer handles use it too. Lives
// here (not sendable_jit.h) so the FixedArray view, ODR-used from this file,
// is fully defined where it is used.
inline JitClosure* _jit_native_method(
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
  _jit_register_native_fn(reinterpret_cast<const void*>(fn));
  auto* c = new JitClosure();
  c->refcount = 1;
  c->fn_ptr = reinterpret_cast<void*>(fn);
  c->n_captures = 0;
  c->captures = nullptr;
  c->arity = 0;
  _gc_register(c, GC_TAG_FUNC);
  return c;
}

// Attach a captureless native method `fn` under name `nm` to a view or
// iterator object. Shared by every Fixed{Array,Set,Map} view + iterator
// builder (an immutable, borrowed function slot).
inline void _jit_view_method(
    JitObject* o, const char* nm,
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
  o->set_or_append(
      nm, JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(fn))},
      false);
}

// --- FixedArray view methods + iterator (native, captureless) -----------
inline int64_t _jit_fa_self_long(JitValue self, const char* key) {
  return _jit_fa_field_long(reinterpret_cast<JitObject*>(self.data), key);
}
inline int64_t _jit_fa_arg_index(JitValue a) {
  return a.tag == TAG_LONG    ? a.data
       : a.tag == TAG_FLOAT   ? static_cast<int64_t>(_culebra_float_to_double(a.data))
                              : 0;
}
// Releases the bound `self` (the native-method ABI passes it +1) on scope
// exit — success or exception — so a throwing native method can't leak it.
// An alias: the semantics are exactly an owned value never consumed; the name
// records the ABI contract at each method's entry.
using JitMethodSelf = JitOwnedVal;
// Releases every argument the method ABI passed at +1 (callee-consumes) on scope
// exit — success or exception — mirroring JitMethodSelf for `self`. For native
// methods that neither forward nor manually release their args, this is the
// throw-safe consume: the caller does NOT clean a native method's operands on
// unwind, so the callee must (channel send, with_lock's callback). Guarding the
// whole args span also closes the direct-error throw path a manual per-branch
// release would miss.
struct JitMethodArgs {
  int64_t n;
  JitValue* args;
  ~JitMethodArgs() {
    for (int64_t i = 0; i < n; i++)
      culebra_runtime_value_release(args[i].tag, args[i].data);
  }
};
inline void _jit_fa_size(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  { *__ret = {TAG_LONG, _jit_fa_len(reinterpret_cast<JitObject*>(self.data))}; return; }
}
inline void _jit_fa_capacity(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  _jit_fa_core(reinterpret_cast<JitObject*>(self.data));  // dropped-buffer check
  { *__ret = {TAG_LONG, _jit_fa_self_long(self, "__fa_cap__")}; return; }
}
inline void _jit_fa_push(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                             JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  auto core = _jit_fa_core(v);
  int64_t len = _jit_fa_len(v);
  int64_t cap = _jit_fa_self_long(self, "__fa_cap__");
  if (len >= cap)
    throw culebra::CulebraError(
        "IndexError", culebra::format("FixedArray is full (capacity {})", cap));
  if (n >= 1)
    _jit_packable_write_field(_jit_fa_elem_ptr(v, len), _jit_fa_elem_field(v),
                              args[0].tag, args[0].data);
  int32_t nl = static_cast<int32_t>(len + 1);
  std::memcpy(core->data + _jit_fa_field_long(v, "__fa_off__"), &nl, 4);
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_fa_get_m(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                              JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  { *__ret = _jit_fa_get(reinterpret_cast<JitObject*>(self.data),
                     n >= 1 ? _jit_fa_arg_index(args[0]) : 0); return; }
}
inline void _jit_fa_set_m(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                              JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  if (n >= 2)
    _jit_fa_set(reinterpret_cast<JitObject*>(self.data),
                _jit_fa_arg_index(args[0]), args[1].tag, args[1].data);
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_fa_iter_self(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  culebra_runtime_value_retain(self.tag, self.data);
  { *__ret = self; return; }
}
inline void _jit_fa_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                      JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  { *__ret = {TAG_BOOL, _jit_fa_self_long(self, "_pos") < _jit_fa_len(it) ? 1 : 0}; return; }
}
inline void _jit_fa_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  int64_t pos = _jit_fa_self_long(self, "_pos");
  JitValue r = _jit_fa_get(it, pos);
  it->set_or_append("_pos", JitValue{TAG_LONG, pos + 1}, true);
  { *__ret = r; return; }
}
inline void _jit_fa_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  auto* it = culebra_runtime_object_new();
  for (const char* k : {"__fa_id__", "__fa_off__", "__fa_cap__",
                        "__fa_dataoff__", "__fa_esize__", "__fa_ecode__"})
    it->set_or_append(k, JitValue{TAG_LONG, _jit_fa_field_long(v, k)}, false);
  it->set_or_append("_pos", JitValue{TAG_LONG, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(it, nm, f);
  };
  meth("iter", _jit_fa_iter_self);
  meth("has_next", _jit_fa_iter_has_next);
  meth("next", _jit_fa_iter_next);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
inline JitValue _jit_make_fixed_array_view(int64_t id, int64_t abs_off,
                                           const culebra::PackableField& f) {
  auto* h = culebra_runtime_object_new();
  h->is_fixed_array_view = true;
  h->set_or_append("__fa_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__fa_off__", JitValue{TAG_LONG, abs_off}, false);
  h->set_or_append("__fa_cap__", JitValue{TAG_LONG, static_cast<long>(f.layout.capacity)}, false);
  h->set_or_append("__fa_dataoff__", JitValue{TAG_LONG, static_cast<long>(f.layout.data_offset)}, false);
  h->set_or_append("__fa_esize__", JitValue{TAG_LONG, static_cast<long>(f.layout.elem_size)}, false);
  h->set_or_append("__fa_ecode__", JitValue{TAG_LONG, culebra::packable_scalar_code(f.layout.elem_type)}, false);
  auto meth = [&](const char* nm,
                  void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(h, nm, fn);
  };
  meth("size", _jit_fa_size);
  meth("capacity", _jit_fa_capacity);
  meth("push", _jit_fa_push);
  meth("get", _jit_fa_get_m);
  meth("set", _jit_fa_set_m);
  meth("iter", _jit_fa_iter);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- FixedSet<T,N> view: open-addressed hash set, byte ops shared with the
// interp via culebra::fixed_probe. State is read from `self` slots. ---------
inline std::shared_ptr<culebra::SharedBufferCore> _jit_fs_core(JitObject* v) {
  int64_t id = v->slots[v->find_slot("__fs_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core)
    throw culebra::CulebraError("ValueError", "SharedBuffer has been dropped");
  return core;
}
inline int64_t _jit_fs_long(JitObject* v, const char* k) {
  return v->slots[v->find_slot(k)].value.data;
}
inline uint8_t* _jit_fs_base(JitObject* v) {
  return _jit_fs_core(v)->data + _jit_fs_long(v, "__fs_off__");
}
inline int64_t _jit_fs_count(JitObject* v) {
  int32_t n; std::memcpy(&n, _jit_fs_base(v), 4); return n;
}
inline void _jit_fs_set_count(JitObject* v, int64_t n) {
  int32_t x = static_cast<int32_t>(n); std::memcpy(_jit_fs_base(v), &x, 4);
}
inline uint8_t* _jit_fs_states(JitObject* v) { return _jit_fs_base(v) + 4; }
inline uint8_t* _jit_fs_vals(JitObject* v) {
  return _jit_fs_base(v) + _jit_fs_long(v, "__fs_dataoff__");
}
inline culebra::PackableField _jit_fs_elem_field(JitObject* v) {
  return culebra::PackableField::scalar(culebra::packable_scalar_name(
      static_cast<int>(_jit_fs_long(v, "__fs_ecode__"))));
}
// Encode a scalar arg into key bytes (a fixed scalar is ≤ 8 bytes); a wrong
// type raises TypeError via _jit_packable_write_field. Returns elem_size.
inline int64_t _jit_fs_encode(JitObject* v, int8_t tag, int64_t data, uint8_t* out) {
  std::memset(out, 0, 8);
  _jit_packable_write_field(out, _jit_fs_elem_field(v), tag, data);
  return _jit_fs_long(v, "__fs_esize__");
}
inline void _jit_fs_size(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  { *__ret = {TAG_LONG, _jit_fs_count(reinterpret_cast<JitObject*>(self.data))}; return; }
}
inline void _jit_fs_capacity(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  _jit_fs_core(v);  // dropped-buffer check
  { *__ret = {TAG_LONG, _jit_fs_long(v, "__fs_cap__")}; return; }
}
inline void _jit_fs_contains(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                 JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  uint8_t key[8];
  int64_t esize = _jit_fs_encode(v, args[0].tag, args[0].data, key);
  int64_t cap = _jit_fs_long(v, "__fs_cap__");
  int64_t slot = culebra::fixed_probe(_jit_fs_states(v), _jit_fs_vals(v), cap,
                                   esize, esize, key, nullptr);
  { *__ret = {TAG_BOOL, slot >= 0 ? 1 : 0}; return; }
}
inline void _jit_fs_add(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                            JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  uint8_t key[8];
  int64_t esize = _jit_fs_encode(v, args[0].tag, args[0].data, key);
  int64_t cap = _jit_fs_long(v, "__fs_cap__");
  int64_t insert = -1;
  int64_t slot = culebra::fixed_probe(_jit_fs_states(v), _jit_fs_vals(v), cap,
                                   esize, esize, key, &insert);
  if (slot >= 0) { *__ret = {TAG_NIL, 0}; return; }
  if (insert < 0)
    throw culebra::CulebraError(
        "CapacityError", culebra::format("FixedSet is full (capacity {})", cap));
  std::memcpy(_jit_fs_vals(v) + insert * esize, key, esize);
  _jit_fs_states(v)[insert] = culebra::kFixedFull;
  _jit_fs_set_count(v, _jit_fs_count(v) + 1);
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_fs_remove(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                               JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  uint8_t key[8];
  int64_t esize = _jit_fs_encode(v, args[0].tag, args[0].data, key);
  int64_t cap = _jit_fs_long(v, "__fs_cap__");
  int64_t slot = culebra::fixed_probe(_jit_fs_states(v), _jit_fs_vals(v), cap,
                                   esize, esize, key, nullptr);
  if (slot < 0) { *__ret = {TAG_BOOL, 0}; return; }
  _jit_fs_states(v)[slot] = culebra::kFixedTomb;
  _jit_fs_set_count(v, _jit_fs_count(v) - 1);
  { *__ret = {TAG_BOOL, 1}; return; }
}
inline int64_t _jit_fs_next_full(JitObject* it, int64_t from) {
  int64_t cap = _jit_fs_long(it, "__fs_cap__");
  uint8_t* st = _jit_fs_states(it);
  for (int64_t i = from; i < cap; i++)
    if (st[i] == culebra::kFixedFull) return i;
  return cap;
}
inline void _jit_fs_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                      JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  { *__ret = {TAG_BOOL, _jit_fs_next_full(it, _jit_fs_long(it, "_pos")) <
                            _jit_fs_long(it, "__fs_cap__") ? 1 : 0}; return; }
}
inline void _jit_fs_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  int64_t idx = _jit_fs_next_full(it, _jit_fs_long(it, "_pos"));
  JitValue r = _jit_packable_read_field(
      _jit_fs_vals(it) + idx * _jit_fs_long(it, "__fs_esize__"),
      _jit_fs_elem_field(it));
  it->set_or_append("_pos", JitValue{TAG_LONG, idx + 1}, true);
  { *__ret = r; return; }
}
inline void _jit_fs_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  auto* it = culebra_runtime_object_new();
  for (const char* k : {"__fs_id__", "__fs_off__", "__fs_cap__",
                        "__fs_dataoff__", "__fs_esize__", "__fs_ecode__"})
    it->set_or_append(k, JitValue{TAG_LONG, _jit_fs_long(v, k)}, false);
  it->set_or_append("_pos", JitValue{TAG_LONG, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(it, nm, f);
  };
  meth("iter", _jit_fa_iter_self);
  meth("has_next", _jit_fs_iter_has_next);
  meth("next", _jit_fs_iter_next);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
inline JitValue _jit_make_fixed_set_view(int64_t id, int64_t abs_off,
                                         const culebra::PackableField& f) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__fs_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__fs_off__", JitValue{TAG_LONG, abs_off}, false);
  h->set_or_append("__fs_cap__", JitValue{TAG_LONG, static_cast<long>(f.layout.capacity)}, false);
  h->set_or_append("__fs_dataoff__", JitValue{TAG_LONG, static_cast<long>(f.layout.data_offset)}, false);
  h->set_or_append("__fs_esize__", JitValue{TAG_LONG, static_cast<long>(f.layout.elem_size)}, false);
  h->set_or_append("__fs_ecode__", JitValue{TAG_LONG, culebra::packable_scalar_code(f.layout.elem_type)}, false);
  auto meth = [&](const char* nm,
                  void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(h, nm, fn);
  };
  meth("size", _jit_fs_size);
  meth("capacity", _jit_fs_capacity);
  meth("contains", _jit_fs_contains);
  meth("add", _jit_fs_add);
  meth("remove", _jit_fs_remove);
  meth("iter", _jit_fs_iter);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// --- FixedMap<K,V,N> view: open-addressed hash map, byte ops shared via
// culebra::fixed_probe. ----------------------------------------------------
inline int64_t _jit_fm_long(JitObject* v, const char* k) {
  return v->slots[v->find_slot(k)].value.data;
}
inline std::shared_ptr<culebra::SharedBufferCore> _jit_fm_core(JitObject* v) {
  auto core = culebra::lookup_shared_buffer(_jit_fm_long(v, "__fm_id__"));
  if (!core)
    throw culebra::CulebraError("ValueError", "SharedBuffer has been dropped");
  return core;
}
inline uint8_t* _jit_fm_base(JitObject* v) {
  return _jit_fm_core(v)->data + _jit_fm_long(v, "__fm_off__");
}
inline int64_t _jit_fm_count(JitObject* v) {
  int32_t n; std::memcpy(&n, _jit_fm_base(v), 4); return n;
}
inline void _jit_fm_set_count(JitObject* v, int64_t n) {
  int32_t x = static_cast<int32_t>(n); std::memcpy(_jit_fm_base(v), &x, 4);
}
inline uint8_t* _jit_fm_states(JitObject* v) { return _jit_fm_base(v) + 4; }
inline uint8_t* _jit_fm_keys(JitObject* v) {
  return _jit_fm_base(v) + _jit_fm_long(v, "__fm_koff__");
}
inline uint8_t* _jit_fm_vals(JitObject* v) {
  return _jit_fm_base(v) + _jit_fm_long(v, "__fm_voff__");
}
inline culebra::PackableField _jit_fm_field(JitObject* v, const char* code_key) {
  return culebra::PackableField::scalar(
      culebra::packable_scalar_name(static_cast<int>(_jit_fm_long(v, code_key))));
}
inline int64_t _jit_fm_enc_key(JitObject* v, int8_t tag, int64_t data,
                            uint8_t* out) {
  std::memset(out, 0, 8);
  _jit_packable_write_field(out, _jit_fm_field(v, "__fm_kcode__"), tag, data);
  return _jit_fm_long(v, "__fm_ksize__");
}
inline void _jit_fm_size(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  { *__ret = {TAG_LONG, _jit_fm_count(reinterpret_cast<JitObject*>(self.data))}; return; }
}
inline void _jit_fm_capacity(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  _jit_fm_core(v);
  { *__ret = {TAG_LONG, _jit_fm_long(v, "__fm_cap__")}; return; }
}
inline int64_t _jit_fm_find(JitObject* v, int8_t tag, int64_t data,
                           int64_t* insert) {
  uint8_t key[8];
  int64_t ksize = _jit_fm_enc_key(v, tag, data, key);
  int64_t cap = _jit_fm_long(v, "__fm_cap__");
  return culebra::fixed_probe(_jit_fm_states(v), _jit_fm_keys(v), cap, ksize,
                              ksize, key, insert);
}
inline void _jit_fm_contains(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                 JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  { *__ret = {TAG_BOOL, _jit_fm_find(v, args[0].tag, args[0].data, nullptr) >= 0 ? 1 : 0}; return; }
}
inline void _jit_fm_get(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                            JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  int64_t slot = _jit_fm_find(v, args[0].tag, args[0].data, nullptr);
  if (slot < 0) { *__ret = {TAG_NIL, 0}; return; }
  { *__ret = _jit_packable_read_field(
      _jit_fm_vals(v) + slot * _jit_fm_long(v, "__fm_vsize__"),
      _jit_fm_field(v, "__fm_vcode__")); return; }
}
inline void _jit_fm_set(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                            JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  uint8_t key[8];
  int64_t ksize = _jit_fm_enc_key(v, args[0].tag, args[0].data, key);
  int64_t cap = _jit_fm_long(v, "__fm_cap__");
  int64_t insert = -1;
  int64_t slot = culebra::fixed_probe(_jit_fm_states(v), _jit_fm_keys(v), cap,
                                   ksize, ksize, key, &insert);
  if (slot < 0) {
    if (insert < 0)
      throw culebra::CulebraError(
          "CapacityError", culebra::format("FixedMap is full (capacity {})", cap));
    std::memcpy(_jit_fm_keys(v) + insert * ksize, key, ksize);
    _jit_fm_states(v)[insert] = culebra::kFixedFull;
    _jit_fm_set_count(v, _jit_fm_count(v) + 1);
    slot = insert;
  }
  _jit_packable_write_field(
      _jit_fm_vals(v) + slot * _jit_fm_long(v, "__fm_vsize__"),
      _jit_fm_field(v, "__fm_vcode__"), args[1].tag, args[1].data);
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_fm_remove(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                               JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  int64_t slot = _jit_fm_find(v, args[0].tag, args[0].data, nullptr);
  if (slot < 0) { *__ret = {TAG_BOOL, 0}; return; }
  _jit_fm_states(v)[slot] = culebra::kFixedTomb;
  _jit_fm_set_count(v, _jit_fm_count(v) - 1);
  { *__ret = {TAG_BOOL, 1}; return; }
}
inline void _jit_fm_keys_m(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  int64_t cap = _jit_fm_long(v, "__fm_cap__");
  int64_t ksize = _jit_fm_long(v, "__fm_ksize__");
  uint8_t* st = _jit_fm_states(v);
  auto* arr = culebra_runtime_array_new();
  for (int64_t i = 0; i < cap; i++)
    if (st[i] == culebra::kFixedFull) {
      JitValue k = _jit_packable_read_field(_jit_fm_keys(v) + i * ksize,
                                            _jit_fm_field(v, "__fm_kcode__"));
      culebra_runtime_array_push(arr, k.tag, k.data);
    }
  { *__ret = {TAG_ARRAY, reinterpret_cast<int64_t>(arr)}; return; }
}
inline int64_t _jit_fm_next_full(JitObject* it, int64_t from) {
  int64_t cap = _jit_fm_long(it, "__fm_cap__");
  uint8_t* st = _jit_fm_states(it);
  for (int64_t i = from; i < cap; i++)
    if (st[i] == culebra::kFixedFull) return i;
  return cap;
}
inline void _jit_fm_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                      JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  { *__ret = {TAG_BOOL, _jit_fm_next_full(it, _jit_fm_long(it, "_pos")) <
                            _jit_fm_long(it, "__fm_cap__") ? 1 : 0}; return; }
}
inline void _jit_fm_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  int64_t idx = _jit_fm_next_full(it, _jit_fm_long(it, "_pos"));
  JitValue k = _jit_packable_read_field(
      _jit_fm_keys(it) + idx * _jit_fm_long(it, "__fm_ksize__"),
      _jit_fm_field(it, "__fm_kcode__"));
  JitValue val = _jit_packable_read_field(
      _jit_fm_vals(it) + idx * _jit_fm_long(it, "__fm_vsize__"),
      _jit_fm_field(it, "__fm_vcode__"));
  auto* tup = culebra_runtime_tuple_new();
  culebra_runtime_tuple_push(tup, k.tag, k.data);
  culebra_runtime_tuple_push(tup, val.tag, val.data);
  it->set_or_append("_pos", JitValue{TAG_LONG, idx + 1}, true);
  { *__ret = {TAG_TUPLE, reinterpret_cast<int64_t>(tup)}; return; }
}
inline void _jit_fm_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* v = reinterpret_cast<JitObject*>(self.data);
  auto* it = culebra_runtime_object_new();
  for (const char* k : {"__fm_id__", "__fm_off__", "__fm_cap__", "__fm_koff__",
                        "__fm_voff__", "__fm_ksize__", "__fm_vsize__",
                        "__fm_kcode__", "__fm_vcode__"})
    it->set_or_append(k, JitValue{TAG_LONG, _jit_fm_long(v, k)}, false);
  it->set_or_append("_pos", JitValue{TAG_LONG, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(it, nm, f);
  };
  meth("iter", _jit_fa_iter_self);
  meth("has_next", _jit_fm_iter_has_next);
  meth("next", _jit_fm_iter_next);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
inline JitValue _jit_make_fixed_map_view(int64_t id, int64_t abs_off,
                                         const culebra::PackableField& f) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__fm_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__fm_off__", JitValue{TAG_LONG, abs_off}, false);
  h->set_or_append("__fm_cap__", JitValue{TAG_LONG, static_cast<long>(f.layout.capacity)}, false);
  h->set_or_append("__fm_koff__", JitValue{TAG_LONG, static_cast<long>(f.layout.data_offset)}, false);
  h->set_or_append("__fm_voff__", JitValue{TAG_LONG, static_cast<long>(f.layout.val_offset)}, false);
  h->set_or_append("__fm_ksize__", JitValue{TAG_LONG, static_cast<long>(f.layout.elem_size)}, false);
  h->set_or_append("__fm_vsize__", JitValue{TAG_LONG, static_cast<long>(f.layout.val_size)}, false);
  h->set_or_append("__fm_kcode__", JitValue{TAG_LONG, culebra::packable_scalar_code(f.layout.elem_type)}, false);
  h->set_or_append("__fm_vcode__", JitValue{TAG_LONG, culebra::packable_scalar_code(f.layout.val_type)}, false);
  auto meth = [&](const char* nm,
                  void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    _jit_view_method(h, nm, fn);
  };
  meth("size", _jit_fm_size);
  meth("capacity", _jit_fm_capacity);
  meth("contains", _jit_fm_contains);
  meth("get", _jit_fm_get);
  meth("set", _jit_fm_set);
  meth("remove", _jit_fm_remove);
  meth("keys", _jit_fm_keys_m);
  meth("iter", _jit_fm_iter);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// `buf[i]` -> a fresh packed-view handle over element `i` (refcount 1,
// caller owns it; the backing bytes outlive the view via the registry).
inline JitObject* _jit_shared_buffer_index(JitObject* buf, int64_t idx,
                                           int64_t line, int64_t col) {
  int64_t id = buf->slots[buf->find_slot("__sharedbuffer_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core) {
    throw culebra::CulebraError("ValueError",
        "SharedBuffer has been dropped", line, col);
  }
  int64_t n = static_cast<long>(core->count);
  if (idx < 0) idx += n;
  if (idx < 0 || idx >= n) {
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  }
  auto* view = culebra_runtime_object_new();
  view->is_packed_view = true;
  culebra_runtime_object_set(view, "__packedview_id__", false, TAG_LONG, id, 0, 0);
  culebra_runtime_object_set(view, "__packedview_index__", false, TAG_LONG, idx, 0, 0);
  return view;
}

// Register a @packable class layout at *runtime* (when its declaration
// executes), from a compact "name:Type;name:Type" spec the codegen emits.
// AOT compiles and runs in separate processes, so a compile-time
// registration would be invisible to the standalone binary — the layout
// must land in the running process's registry. JIT/interp run in the same
// process, so this is equivalent there. The spec's field types are already
// lint-validated, so compute_packable_layout won't throw here.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_packable(
    const char* name, const char* spec) {
  std::vector<std::pair<std::string, std::string>> fields;
  std::string_view s(spec);
  size_t i = 0;
  while (i < s.size()) {
    size_t semi = s.find(';', i);
    if (semi == std::string_view::npos) semi = s.size();
    auto seg = s.substr(i, semi - i);
    auto colon = seg.find(':');
    if (colon != std::string_view::npos) {
      fields.emplace_back(std::string(seg.substr(0, colon)),
                          std::string(seg.substr(colon + 1)));
    }
    i = semi + 1;
  }
  culebra::register_packable_layout(name, culebra::compute_packable_layout(name, fields));
}

// Register a @packable enum's tagged-union layout at runtime (the codegen
// emits this when the enum declaration executes, so AOT sees it too). The
// spec is `variant:type,type;variant:;...`. Lint already validated it, so the
// validate-and-register throw is only a safety net.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_packable_enum(
    const char* name, const char* spec) {
  culebra::validate_and_register_packable_enum(
      name, culebra::parse_packable_enum_spec(spec));
}

// `[...x]` array spread: append an iterable's elements to `arr` (each
// retained). MVP sources: Array / Tuple / Set. The spread value's own
// reference is dropped by the caller.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_extend(
    JitArray* arr, int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_ARRAY || tag == TAG_TUPLE) {
    auto* src = reinterpret_cast<JitArray*>(data);
    // Size read once: `a.extend(a)` (the method form) aliases arr and src, so
    // re-reading it would grow forever. `items` is re-read per step because
    // push may have reallocated it.
    size_t n = src->size;
    for (size_t i = 0; i < n; i++) {
      culebra_runtime_value_retain(src->items[i].tag, src->items[i].data);
      culebra_runtime_array_push(arr, src->items[i].tag, src->items[i].data);
    }
  } else if (tag == TAG_SET) {
    auto* s = reinterpret_cast<JitSet*>(data);
    for (auto& m : s->members) {
      culebra_runtime_value_retain(m.tag, m.data);
      culebra_runtime_array_push(arr, m.tag, m.data);
    }
  } else {
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("cannot spread {} into an array (Array/Tuple/Set only)",
                        _culebra_tag_name(tag)),
        line, col);
  }
}

// `{...x}` object spread: merge another Object's string-keyed entries
// into `dst` (later keys win). Merged keys are made mutable so explicit
// properties after the spread can override them (matches interp). The
// spread value's own reference is dropped by the caller.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_merge(
    JitObject* dst, int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag != TAG_OBJECT) {
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("cannot spread {} into an object (Object only)",
                        _culebra_tag_name(tag)),
        line, col);
  }
  auto* src = reinterpret_cast<JitObject*>(data);
  src->for_each([&](std::string_view name, const JitObjectEntry& e) {
    culebra_runtime_value_retain(e.value.tag, e.value.data);
    culebra_runtime_object_set(dst, std::string(name).c_str(), /*mut=*/true,
                               e.value.tag, e.value.data, line, col,
                               /*is_init=*/true);
  });
}

// Hashable-key access into an Object. String keys unify with the
// shape-based `obj.foo` path so `obj["x"] = v` and `obj.x = v` reach
// the same slot; everything else (Long/Float/Bool/Nil/Tuple) lands in
// the sidecar AnyKeyMap. Object-literal keys are pre-filtered by the
// grammar (no String literal keys), so the TAG_STRING branch only
// fires for runtime-keyed subscript ops.
//
// The shape-path call below passes 0/0 for the IC slot, so a String-
// keyed subscript write never inline-caches. Hot loops should still
// prefer `obj.x = v` over `obj["x"] = v` for that reason.
//
// Refcount contract: this helper consumes the caller's +1 to the key.
// On insert, the +1 transfers to the map's stored alias; the helper
// adds a second +1 for the key_order entry. On update / throw it
// explicitly releases the caller's +1 since the existing stored alias
// already covers the slot.
// `obj[key] = value` fallback to a user-defined `__setindex__(key, value)`.
// Returns true when the method exists (and was invoked), else false.
inline bool _jit_try_object_setindex(JitObject* obj, int8_t key_tag,
                                     int64_t key_data, int8_t val_tag,
                                     int64_t val_data) {
  // Class-instance feature only (the call sites also gate on `proto` to
  // skip the slot probe for plain dicts; this keeps the helper self-safe).
  if (!obj->proto) return false;
  auto* cls = _lookup_special(TAG_OBJECT, reinterpret_cast<int64_t>(obj),
                              "__setindex__");
  if (!cls) return false;
  // Borrows the key and value: the caller consumes them on the normal path and
  // guards them over this call for the throw path.
  auto r = _culebra_invoke_method2(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(obj)}, {key_tag, key_data},
      {val_tag, val_data});
  _culebra_value_release_impl(r.tag, r.data);  // discard the return value
  return true;
}

// String and StringView are the same dict key (a `==`-equal pair). Normalize
// a StringView key (e.g. `s[0..1]`) to a borrowed-cstr TAG_STRING so it lands
// in the same String slot as `obj["k"]`, releasing the view's +1. `owned` must
// outlive the lookup that follows — it backs the cstr. Other tags are left
// untouched, so the String/Long/sidecar fast paths pay only one branch.
inline void _jit_normalize_str_key(int8_t& key_tag, int64_t& key_data,
                                              std::string& owned) {
  if (key_tag == TAG_STRINGVIEW) {
    auto* sv = reinterpret_cast<JitStringView*>(key_data);
    owned.assign(sv->ptr, sv->len);  // copy bytes before releasing the view
    _culebra_value_release_impl(key_tag, key_data);
    key_tag = TAG_STRING;
    key_data = reinterpret_cast<int64_t>(owned.c_str());
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_any(
    JitObject* obj, int8_t key_tag, int64_t key_data, bool mut,
    int8_t val_tag, int64_t val_data, int64_t line, int64_t col,
    bool is_init) {
  std::string _kbuf;
  _jit_normalize_str_key(key_tag, key_data, _kbuf);
  // `arr[i] = v` on a FixedArray view: write element i into the inline bytes
  // (the index coerces Long/Float like the interp).
  if (obj->is_fixed_array_view) {
    int64_t i = (key_tag == TAG_LONG) ? key_data
           : (key_tag == TAG_FLOAT)
               ? static_cast<int64_t>(_culebra_float_to_double(key_data))
               : throw culebra::CulebraError("TypeError",
                     "type error: expected Long or Float", line, col);
    _jit_fa_set(obj, i, val_tag, val_data, line, col);
    return;
  }
  // `buf[i] = v` on a SharedBuffer: a packed record has no Value form, so
  // reject (set fields via `buf[i].field = v`). Matches the interp guard.
  if (_jit_is_shared_buffer(obj)) {
    _culebra_value_release_impl(val_tag, val_data);
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    throw culebra::CulebraError("TypeError",
        "cannot assign to a SharedBuffer element directly; "
        "set fields via buf[i].field = value", line, col);
  }
  // Shared.new views are immutable — index writes included.
  if (obj->is_shared_val) {
    _culebra_value_release_impl(val_tag, val_data);
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    throw culebra::CulebraError("ImmutableError",
                                "Shared values are immutable", line, col);
  }
  if (key_tag == TAG_STRING) {
    // Subscript overloading is a class-instance feature (proto != null); a
    // class instance may define `__setindex__` for keys that aren't one of
    // its own slots. A plain dict short-circuits on `proto` and sets the
    // slot directly, so its hot path keeps a single lookup (string key is
    // non-refcounted, so only the consumed value is released here).
    if (obj->proto &&
        obj->find_slot(reinterpret_cast<const char*>(key_data)) ==
            static_cast<size_t>(-1)) {
      // A user `__setindex__` body's throw unwinds past the release below, so
      // guard the value's +1 across the dispatch.
      JitUnwindRelease g{JitValue{val_tag, val_data}};
      if (_jit_try_object_setindex(obj, key_tag, key_data, val_tag, val_data)) {
        _culebra_value_release_impl(val_tag, val_data);
        return;
      }
    }
    culebra_runtime_object_set(obj, reinterpret_cast<const char*>(key_data),
                               mut, val_tag, val_data, line, col, is_init);
    return;
  }
  // Everything below consumes the key and value this helper was handed, and
  // both a user `hash()` (the sidecar probes below hash the key) and a user
  // `__setindex__` can throw in the middle — one guard releases them on every
  // such edge. The paths that hand the refs on return immediately.
  JitUnwindRelease g{JitValue{key_tag, key_data}, JitValue{val_tag, val_data}};
  // A class instance may route a not-yet-stored key to __setindex__ before
  // the sidecar is activated (key + value are consumed by this helper's
  // contract). A plain dict skips this probe entirely.
  if (obj->proto) {
    bool exists = obj->non_string_props &&
                  obj->non_string_props->count(JitValue{key_tag, key_data});
    if (!exists &&
        _jit_try_object_setindex(obj, key_tag, key_data, val_tag, val_data)) {
      _culebra_value_release_impl(key_tag, key_data);
      _culebra_value_release_impl(val_tag, val_data);
      return;
    }
  }
  // The sidecar's half of the frozen field set — checked before the lazy
  // activation below so a refused write leaves the instance untouched. A
  // `@value` instance can never hold a non-String key (this is the only
  // branch that mints one), so any write reaching here is an add. The guard
  // above owns the key and the value on this edge; throw without releasing.
  if (_jit_value_add_refused(obj, /*is_init=*/false))
    _jit_throw_value_add(nullptr, line, col);
  if (!obj->non_string_props) {
    obj->non_string_props = new JitObject::AnyKeyMap();
    // First non-String key: activate key_order and back-fill with the
    // String keys already in `shape->names` so interleaved order survives.
    obj->key_order = new std::vector<JitValue>();
    if (obj->shape) {
      obj->key_order->reserve(obj->shape->names.size() + 1);
      for (const auto& name : obj->shape->names) {
        obj->key_order->push_back(
            {TAG_STRING, reinterpret_cast<int64_t>(_intern_str(name))});
      }
    }
  }
  JitValue key{key_tag, key_data};
  auto& m = *obj->non_string_props;
  auto it = m.find(key);
  if (it == m.end()) {
    m.emplace(key, JitObjectEntry{JitValue{val_tag, val_data}, mut});
    // Caller's +1 transferred to the map's stored alias. Add a second
    // +1 for the key_order entry.
    culebra_runtime_value_retain(key_tag, key_data);
    obj->key_order->push_back(key);
    return;
  }
  // Object-literal construction (`is_init`) overwrites a duplicate key
  // last-wins like the interp's `initialize`; only a post-construction
  // `o[k] = v` (is_init=false) honors the slot's immutable flag.
  if (!is_init && !it->second.mut)
    throw culebra::CulebraError("ImmutableError",
                                "immutable entry on non-String key", line, col);
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  it->second.value.tag = val_tag;
  it->second.value.data = val_data;
  if (is_init) it->second.mut = mut;
  _culebra_value_release_impl(key_tag, key_data);
}

// Consumes the caller's +1 to the key on the refcounted (sidecar)
// path so a Tuple-keyed `obj[k]` read does not leak. TAG_STRING keys
// are non-refcounted (just borrowed cstrings), so they need no release.
// `obj[key]` fallback to a user-defined `__index__(key)`. Returns true and
// writes the (+1 owned) result to out when the method exists, else false.
inline bool _jit_try_object_index(JitObject* obj, int8_t key_tag,
                                  int64_t key_data, int8_t* out_tag,
                                  int64_t* out_data) {
  // Subscript overloading is a class-instance feature (matches interp's
  // class_tag gate); a plain dict never routes to __index__.
  if (!obj->proto) return false;
  auto r = _try_special_binop(TAG_OBJECT, reinterpret_cast<int64_t>(obj),
                              key_tag, key_data, "__index__");
  if (!r) return false;
  *out_tag = r->tag;
  *out_data = r->data;
  return true;
}

// A capture-group slot (`{value,start,end}` object, or nil when the group
// did not participate) -> its value string (+1 owned), or nil. Mirrors the
// interp match_capture's group_value lambda.
inline JitValue _jit_match_group_value(JitValue g) {
  if (g.tag != TAG_OBJECT) return JitValue{TAG_NIL, 0};
  auto* go = reinterpret_cast<JitObject*>(g.data);
  size_t vi = go->find_slot("value");
  if (vi == static_cast<size_t>(-1)) return JitValue{TAG_NIL, 0};
  JitValue v = go->slots[vi].value;
  culebra_runtime_value_retain(v.tag, v.data);
  return v;
}

// `m[key]` on a Regex match: Long -> positional group (negative wraps like an
// array), String/StringView -> named group. Any miss is nil. Returns a +1
// owned value. The interp twin is Interpreter::match_capture.
inline JitValue _jit_match_index(JitObject* m, int8_t key_tag,
                                 int64_t key_data) {
  if (key_tag == TAG_LONG) {
    size_t gi = m->find_slot("groups");
    if (gi == static_cast<size_t>(-1)) return JitValue{TAG_NIL, 0};
    auto* arr = reinterpret_cast<JitArray*>(m->slots[gi].value.data);
    int64_t n = static_cast<long>(arr->size);
    int64_t i = key_data;
    if (i < 0) i += n;
    if (i < 0 || i >= n) return JitValue{TAG_NIL, 0};
    return _jit_match_group_value(arr->items[i]);
  }
  if (key_tag == TAG_STRING || key_tag == TAG_STRINGVIEW) {
    size_t ni = m->find_slot("named");
    if (ni == static_cast<size_t>(-1)) return JitValue{TAG_NIL, 0};
    auto* named = reinterpret_cast<JitObject*>(m->slots[ni].value.data);
    size_t si = named->find_slot(_culebra_str_view(key_tag, key_data));
    if (si == static_cast<size_t>(-1)) return JitValue{TAG_NIL, 0};
    return _jit_match_group_value(named->slots[si].value);
  }
  return JitValue{TAG_NIL, 0};
}

// Shared "own slot" lookup for a plain-dict receiver: String keys through
// the shape table, everything else through the non-string sidecar. Shared
// by object_get_any (throws KeyError on a miss) and object_get_for_coalesce
// (`??=`, reports a miss as found=false instead) — same slot-lookup logic,
// different miss policy. Consumes a non-String key's +1 on a hit; leaves it
// untouched on a miss so each caller makes its own miss-path decision
// (__index__ fallback, throw, or "not found") and handles the key itself.
inline bool _jit_try_own_slot(JitObject* obj, int8_t key_tag, int64_t key_data,
                              int8_t* out_tag, int64_t* out_data) {
  if (key_tag == TAG_STRING) {
    auto idx = obj->find_slot(reinterpret_cast<const char*>(key_data));
    if (idx != static_cast<size_t>(-1)) {
      *out_tag = obj->slots[idx].value.tag;
      *out_data = obj->slots[idx].value.data;
      culebra_runtime_value_retain(*out_tag, *out_data);
      return true;
    }
  } else if (obj->non_string_props) {
    auto it = obj->non_string_props->find(JitValue{key_tag, key_data});
    if (it != obj->non_string_props->end()) {
      *out_tag = it->second.value.tag;
      *out_data = it->second.value.data;
      culebra_runtime_value_retain(*out_tag, *out_data);
      _culebra_value_release_impl(key_tag, key_data);
      return true;
    }
  }
  return false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get_any(
    JitObject* obj, int8_t key_tag, int64_t key_data,
    int8_t* out_tag, int64_t* out_data, int64_t line, int64_t col,
    bool own_receiver) {
  std::string _kbuf;
  _jit_normalize_str_key(key_tag, key_data, _kbuf);
  // When the codegen index path (`obj[k]`) owns the receiver +1, a lookup that
  // throws must release it — the caller only releases on the normal path and
  // there is no landing pad on the unwind edge. One guard covers every throw
  // here: the direct failures (bad-key TypeError, OOB IndexError, the
  // shared-val reader's errors, KeyError) and a user `__index__` body's alike,
  // since the dispatch only borrows what it is handed. The C++/compound-assign
  // callers borrow the receiver and pass false; a TAG_STRING key is a borrowed
  // cstring (never released). Nil guard entries no-op.
  const JitValue recv_guard = own_receiver
      ? JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(obj)}
      : JitValue{TAG_NIL, 0};
  const JitValue key_guard = key_tag != TAG_STRING
      ? JitValue{key_tag, key_data}
      : JitValue{TAG_NIL, 0};
  JitUnwindRelease g{key_guard, recv_guard};
  // `arr[i]` on a FixedArray view: read element i from the inline bytes.
  if (obj->is_fixed_array_view) {
    int64_t i = (key_tag == TAG_LONG) ? key_data
           : (key_tag == TAG_FLOAT)
               ? static_cast<int64_t>(_culebra_float_to_double(key_data))
               : throw culebra::CulebraError("TypeError",
                     "type error: expected Long or Float", line, col);
    JitValue r = _jit_fa_get(obj, i, line, col);
    *out_tag = r.tag;
    *out_data = r.data;
    return;
  }
  // `buf[i]` on a SharedBuffer: hand back a packed view over element `i`
  // (the index coerces Long/Float like the interp's `key.to_long()`).
  if (_jit_is_shared_buffer(obj)) {
    int64_t idx = (key_tag == TAG_LONG)    ? key_data
             : (key_tag == TAG_FLOAT)   ? static_cast<int64_t>(_culebra_float_to_double(key_data))
             : throw culebra::CulebraError("TypeError",
                   "type error: expected Long or Float", line, col);
    auto* view = _jit_shared_buffer_index(obj, idx, line, col);
    *out_tag = TAG_OBJECT;
    *out_data = reinterpret_cast<int64_t>(view);
    return;
  }
  // Shared.new view: `view[key]` reads the frozen tree (Object key
  // lookup / Array-Tuple positional). Consumes a refcounted key per
  // this helper's contract; the result is +1.
  if (_jit_is_shared_val(obj)) {
    JitValue r = culebra::_jit_shared_val_index(obj, key_tag, key_data,
                                                line, col);
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    // _jit_shared_val_child hands back a container sub-view BORROWED (+0; the
    // parent view's memo owns the +1) or a fresh primitive leaf. Retain so
    // this helper is uniformly +1-owned like every other branch — index
    // callers release the receiver without a promotion retain, and a borrowed
    // sub-view would otherwise be freed by that release while the parent
    // still points at it (a use-after-free the memo's next reader would hit).
    culebra_runtime_value_retain(r.tag, r.data);
    *out_tag = r.tag;
    *out_data = r.data;
    return;
  }
  // `m[i]` / `m["name"]` on a Regex match: index its capture groups (Long ->
  // positional, String/StringView -> named). A miss is nil. Record fields
  // (`m.value`, spans) stay on dot access.
  if (obj->is_match) {
    JitValue r = _jit_match_index(obj, key_tag, key_data);
    *out_tag = r.tag;
    *out_data = r.data;
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    return;
  }
  // Slot hit: String keys are unified with shape access (see object_set_any);
  // other keys live in the non-String sidecar.
  if (_jit_try_own_slot(obj, key_tag, key_data, out_tag, out_data)) return;
  // Miss → user `__index__` overload.
  if (_jit_try_object_index(obj, key_tag, key_data, out_tag, out_data)) {
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    return;
  }
  throw culebra::CulebraError("KeyError", "key not present", line, col);
}

// Read-only lookup for `o[k] ??= v`: unlike object_get_any, a plain-dict
// miss returns found=false (mirrors the interp's `obj.has(key) ? obj.get
// (key) : nil` treatment — a plain `o[k]` read still throws KeyError via
// object_get_any). A class instance's `__index__` is still consulted on a
// miss, exactly like the plain-read path. Only the plain-dict / class-
// instance receivers are supported (this is `??=`'s complex-lvalue path,
// not the general subscript-read path) — the receiver dispatch in
// compile_assign_complex only calls this for a TAG_OBJECT lval that isn't
// a FixedArray view / SharedBuffer / Shared.new view (those reject `??=`
// before reaching here, matching the interp).
// Consumes the caller's +1 to a non-String key on every exit; a TAG_STRING
// key is a borrowed cstring (never released). The receiver is borrowed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_get_for_coalesce(
    JitObject* obj, int8_t key_tag, int64_t key_data,
    int8_t* out_tag, int64_t* out_data, int64_t line, int64_t col) {
  std::string _kbuf;
  _jit_normalize_str_key(key_tag, key_data, _kbuf);
  const JitValue key_guard = key_tag != TAG_STRING
      ? JitValue{key_tag, key_data}
      : JitValue{TAG_NIL, 0};
  JitUnwindRelease g{key_guard};
  if (_jit_try_own_slot(obj, key_tag, key_data, out_tag, out_data)) return true;
  // Miss → user `__index__` overload, else "not found" (nil for `??=`).
  if (_jit_try_object_index(obj, key_tag, key_data, out_tag, out_data)) {
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    return true;
  }
  if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
  // Caller loads *out_tag/*out_data unconditionally before checking the
  // return value — always leave them well-defined.
  *out_tag = TAG_NIL;
  *out_data = 0;
  return false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_object_has_any(
    JitObject* obj, int8_t key_tag, int64_t key_data) {
  if (!obj->non_string_props) return 0;
  return obj->non_string_props->contains({key_tag, key_data}) ? 1 : 0;
}

// Fast path for the property-write IC. Caller (JIT-emitted IR) has
// already verified `obj->shape == ic->expected_shape`, so this just
// dispatches on the cached mode:
//   * update (expected == result): overwrite slots[offset], honoring
//     the existing entry's mut flag.
//   * transition (expected != result): grow `slots` by one (the
//     reserved capacity from append_slot's first-call reserve(8)
//     usually means no realloc), bump shape to result_shape.
// `key` is borrowed only for the immutable-error message; never freed.
// Mirrors culebra_runtime_object_set's two contract obligations exactly —
// check_wk on the update path, the well-known check before a new own slot
// is created, and the drop-registration hook after — since a repeat call
// at this site (a warm loop constructing many instances, say) takes THIS
// path, not object_set / object_set_ic, so those checks have to live here
// too or a reassigned `drop`/`iter`/... contract violation, or a `drop`
// slot's owned-stack registration, would silently go unenforced once the
// cache warms up.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_fast(
    JitObject* obj, const char* key, JitPropSetIC* ic, int8_t tag,
    int64_t data, int64_t line, int64_t col, bool mut, bool is_init) {
  if (ic->expected_shape == ic->result_shape) {
    _jit_overwrite_slot(obj->slots[ic->offset], key, tag, data, mut, is_init,
                        line, col, /*check_wk=*/true);
  } else {
    _jit_reject_value_add(obj, key, tag, data, line, col, is_init);
    _culebra_check_well_known_prop(key, tag, data);
    if (obj->slots.capacity() == 0) obj->slots.reserve(8);
    obj->slots.push_back({JitValue{tag, data}, ic->prop_mut != 0});
    obj->shape = static_cast<culebra::Shape*>(ic->result_shape);
    if (obj->key_order) {
      obj->key_order->push_back(
          {TAG_STRING,
           reinterpret_cast<int64_t>(_intern_str(obj->shape->names.back()))});
    }
  }
  if (std::string_view(key) == "drop") _jit_owned_bind_drop(obj);
}

// The two receivers a property write never stores a slot on. `view.field = v`
// on a @packable packed view writes straight into the shared backing bytes
// (zero copy); a Shared.new view is immutable on every write surface. Neither
// name ever becomes an own slot, so the write always reaches an uncached path
// — the IC below stays cold for both, and the bytecode VM, which keeps no
// per-site cache at all, comes through the entry after it. Returns true when
// the write was handled here. Matches the interp's packed_view_set arm.
inline bool _jit_prop_write_intercepted(JitObject* obj, const char* key,
                                        int8_t tag, int64_t data,
                                        int64_t line, int64_t col,
                                        int64_t mem_line, int64_t mem_col) {
  if (_jit_is_packed_view(obj)) {
    // A packed field's own errors report at the member (the interp's
    // packed_view_set takes the DOT node's position); everything else on a
    // property write reports at the statement.
    _jit_packed_view_set(obj, key, tag, data, mem_line ? mem_line : line,
                         mem_line ? mem_col : col);
    return true;
  }
  if (obj->is_shared_val) {
    _culebra_value_release_impl(tag, data);
    throw culebra::CulebraError("ImmutableError",
                                "Shared values are immutable", line, col);
  }
  return false;
}

// Property write with no per-site inline cache — the bytecode VM's `o.k = v`.
// Same interceptions the IC slow path performs, then the plain store.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_uncached(
    JitObject* obj, const char* key, bool mut, int8_t tag, int64_t data,
    int64_t line, int64_t col, bool is_init, int64_t mem_line,
    int64_t mem_col) {
  if (_jit_prop_write_intercepted(obj, key, tag, data, line, col, mem_line,
                                  mem_col))
    return;
  culebra_runtime_object_set(obj, key, mut, tag, data, line, col, is_init);
}

// Slow path for the property-write IC. Performs the full lookup
// (mirrors `culebra_runtime_object_set`), then refreshes the IC so
// subsequent writes at this site with the same starting shape hit
// the fast path. Caches `obj->shape` *as-observed* (including
// nullptr) so fresh-Object writes can hit the fast path on the
// second instance — overwriting it with `root()` would mean a
// permanent miss for objects that always start out with no shape.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_ic(
    JitObject* obj, const char* key, JitPropSetIC* ic, bool mut,
    int8_t tag, int64_t data, int64_t line, int64_t col, bool is_init,
    int64_t mem_line, int64_t mem_col) {
  if (_jit_prop_write_intercepted(obj, key, tag, data, line, col, mem_line,
                                  mem_col))
    return;
  auto* before = obj->shape;
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) {
    _jit_reject_value_add(obj, key, tag, data, line, col, is_init);
    _culebra_check_well_known_prop(key, tag, data);
    auto* base = before ? before : culebra::shape_registry().root();
    auto* result = culebra::shape_registry().transition_add(base, key);
    ic->expected_shape = before;
    ic->result_shape = result;
    ic->offset = result->names.size() - 1;
    ic->prop_mut = mut ? 1 : 0;
    if (obj->slots.capacity() == 0) obj->slots.reserve(8);
    obj->slots.push_back({JitValue{tag, data}, mut});
    obj->shape = result;
    if (obj->key_order) {
      obj->key_order->push_back(
          {TAG_STRING,
           reinterpret_cast<int64_t>(_intern_str(result->names.back()))});
    }
  } else {
    _jit_overwrite_slot(obj->slots[idx], key, tag, data, mut, is_init, line,
                        col, /*check_wk=*/true);
    ic->expected_shape = before;
    ic->result_shape = before;
    ic->offset = idx;
    ic->prop_mut = obj->slots[idx].mut ? 1 : 0;
  }
  if (std::string_view(key) == "drop") _jit_owned_bind_drop(obj);
}

// Build a structured Error Object for a C++ exception that has reached a
// try/catch landingpad, from the pending carrier a CulebraError recorded at its
// construction (see culebra_note_pending_error). This replaces an older
// `try { throw; } catch` re-inspection, which needed an active __cxa_begin_catch
// context — a dependency Windows SEH funclet EH can't provide inside a catchpad.
// On success, populates the thrown-value carriers as if the user had run
// `throw error_object`. A user `throw` already set `is_throw` with a real Value,
// so it early-outs. A foreign C++ exception (no pending carrier) leaves
// `is_throw=0` so the caller propagates it with __cxa_rethrow.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_try_translate() {
  auto& rt = culebra::current_runtime();
  if (rt.is_throw) return;        // user throw already carries a Value
  if (!rt.pending_error) return;  // foreign exception — the pad rethrows it
  // A positionless runtime error (line/col 0) adopts the last published op
  // position, matching the old _jit_backfill_op_pos path.
  int64_t line = rt.pending_line, col = rt.pending_col;
  if (line == 0 && col == 0) {
    line = _jit_thread.op_line;
    col = _jit_thread.op_col;
  }
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(
      obj, "kind", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(rt.pending_kind)), 0, 0);
  culebra_runtime_object_set(
      obj, "message", false, TAG_STRING,
      reinterpret_cast<int64_t>(_culebra_heap_str(rt.pending_msg)), 0, 0);
  culebra_runtime_object_set(obj, "line", false, TAG_LONG, line, 0, 0);
  culebra_runtime_object_set(obj, "col", false, TAG_LONG, col, 0, 0);
  rt.thrown_tag = TAG_OBJECT;
  rt.thrown_data = reinterpret_cast<int64_t>(obj);
  rt.is_throw = 1;
  rt.pending_error = 0;  // consumed
}

// Write `(tag, data)` to the out-params; nil if entry is null.
inline void _write_value_out(const JitObjectEntry* entry, int8_t* out_tag,
                              int64_t* out_data) {
  if (entry) {
    *out_tag = entry->value.tag;
    *out_data = entry->value.data;
  } else {
    *out_tag = 0;
    *out_data = 0;
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get(
    JitObject* obj, const char* key, int8_t* out_tag, int64_t* out_data) {
  _write_value_out(_find_property(obj, key), out_tag, out_data);
}

// Slow path for the per-callsite IC emitted by emit_property_get.
// On an own-property hit, refreshes `ic->shape` / `ic->offset` so the
// next read with the same shape stays on the inlined fast path. Proto
// hits don't update the cache because the fast path keys on
// `obj->shape`; caching the proto's offset there would load the wrong
// slot. `ic` is borrowed; never released.
// Forward declaration for the trait-default table referenced below.
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, JitClosure*>>&
_jit_trait_default_impls();

// The registered default named `key` whose trait this instance conforms to,
// or null. Conformance is cached inside _culebra_type_matches_single. One
// source for the three askers: the property read, the UFCS gate, and the
// `__call__` lookup — each keeps its own gate on what may ask.
inline JitClosure* _jit_find_trait_default(JitObject* obj, const char* key) {
  for (auto& [trait_name, methods] : _jit_trait_default_impls()) {
    auto m_it = methods.find(key);
    if (m_it == methods.end() || !m_it->second) continue;
    if (_culebra_type_matches_single(TAG_OBJECT,
                                     reinterpret_cast<int64_t>(obj),
                                     trait_name))
      return m_it->second;
  }
  return nullptr;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_object_get_ic(
    JitObject* obj, const char* key, JitPropIC* ic, int64_t line,
    int64_t col) {
  // @packable handles: a packed view's `.field` reads the backing bytes
  // (zero copy); a buffer's `.size`/`.count`/`.len` reports its length.
  // Returns a primitive — no retain needed. (Always reaches the slow path:
  // these names are never own slots, so the IC stays cold.)
  if (_jit_is_packed_view(obj)) {
    return _jit_packed_view_get(obj, key, line, col);
  }
  if (_jit_is_shared_buffer(obj)) {
    std::string_view k(key);
    if (k == "size" || k == "count" || k == "len") {
      return {TAG_LONG,
              obj->slots[obj->find_slot("__sharedbuffer_count__")].value.data};
    }
  }
  // Cached proto hit: this receiver shape's own scan missed last time and the
  // proto resolved `key` at proto_offset, and neither shape has changed since
  // — so both linear scans are already answered. This is the shape a class
  // instance's every method read has (data in own slots, methods on the
  // shared meta behind `proto`).
  if (obj->shape == ic->owner_shape && obj->proto &&
      obj->proto->shape == ic->proto_shape) {
    return obj->proto->slots[ic->proto_offset].value;
  }
  if (obj->shape) {
    auto idx = obj->shape->offset(key);
    if (idx != static_cast<size_t>(-1)) {
      ic->shape = obj->shape;
      ic->offset = idx;
      return obj->slots[idx].value;
    }
  }
  if (obj->proto) {
    // The proto's own slot — _find_property's own first step, so the
    // resolution order is unchanged; only this hit is cacheable, and a name
    // it answers from a second level falls through to the walk below.
    auto idx = obj->proto->find_slot(key);
    if (idx != static_cast<size_t>(-1)) {
      ic->owner_shape = obj->shape;
      ic->proto_shape = obj->proto->shape;
      ic->proto_offset = idx;
      return obj->proto->slots[idx].value;
    }
    if (auto* proto_entry = _find_property(obj->proto, key))
      return proto_entry->value;
  }
  // Shared.new view: own slots (markers + reader methods) resolved
  // above; anything else reads the frozen tree (nil on miss, like a
  // plain Object). The returned value is +0-borrowed: a string leaf is a
  // fresh heap string (leak-bounded by design like every JIT string), a
  // container child is a sub-view memoized on the parent view (see
  // _jit_shared_val_child) and released on the parent's teardown — no
  // backstop-reclaimed orphan.
  if (_jit_is_shared_val(obj)) {
    return culebra::_jit_shared_val_prop(obj, key, line, col);
  }
  // A builtin namespace has a closed member set — an unknown member is a typo
  // or a removed API. Raise here (naming the member) instead of returning nil,
  // matching interp's eval_property. Builtin method names (keys/size/has/...)
  // are excluded: the interp's ObjectValue::has() reports them present, so a
  // call like `IO.keys()` dispatches the builtin; the JIT's user-method-over-
  // builtin probe reads this same path and must see nil (not a throw) to fall
  // through to the builtin. UFCS (`Ns.free_fn()`) is resolved before this cold
  // path, so it is unaffected. Placed before the trait-default scan to match
  // interp's ordering: a closed namespace never picks up a user trait default.
  if (obj->is_namespace && !culebra::is_object_builtin_method_name(key)) {
    culebra::throw_namespace_missing_member_at(obj->ns_name, key, line, col);
  }
  // Trait default-method fallback (T4 part 2).
  if (auto* d = _jit_find_trait_default(obj, key))
    return {TAG_FUNC, reinterpret_cast<int64_t>(d)};
  return {TAG_NIL, 0};
}

// Consolidated cold path for a JIT property read `recv.key`. The JIT
// inlines only the monomorphic object fast path (shape hit -> direct slot
// read); every other case funnels here, so the receiver-tag semantics live
// in one place instead of being re-emitted as IR at every read site:
//   - Object -> object_get_ic (IC miss: real lookup + IC refresh)
//   - container-ish receiver -> missing member reads as Nil (mirrors the
//     interpreter's permissive member read; a `.foo()` on that Nil then
//     fails with the usual "expected Function, got Nil")
//   - scalar -> TypeError, the fixed engine-wide wording.
//
// `own_receiver` gate (C⑧, mirrors object_get_any's C③ contract): a bare
// property read `recv.key` on a +1-owned receiver (the postfix chain's rolling
// handle) strands that +1 when this cold path throws — a dropped Shared view's
// ClosedError, a closed namespace's AttributeError, a corrupt-view ValueError,
// or the scalar TypeError. All of those are DIRECT errors (no user dispatch:
// a getter is invoked later in emit_property_value_read, where the receiver
// rides the unwind-temp window), so this helper is the sole releaser on the
// unwind edge. Callers that merely borrow the receiver (compound-assign
// intermediates, the iterator protocol) pass false. The fast inline path never
// reaches here, so the gate costs the cold path one i1 arg and no IR at the
// monomorphic read site.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_prop_get(
    int8_t recv_tag, int64_t recv_data, const char* key, JitPropIC* ic,
    int64_t line, int64_t col, bool own_receiver) {
  JitUnwindRelease g{own_receiver ? JitValue{recv_tag, recv_data}
                                  : JitValue{TAG_NIL, 0}};
  if (recv_tag == TAG_OBJECT) {
    return culebra_runtime_object_get_ic(
        reinterpret_cast<JitObject*>(recv_data), key, ic, line, col);
  }
  switch (recv_tag) {
    case TAG_FUNC: case TAG_STRING: case TAG_ARRAY: case TAG_TUPLE:
    case TAG_SET: case TAG_STRINGVIEW: case TAG_TENSOR:
      return {TAG_NIL, 0};
    default:
      culebra_runtime_type_error_typed(line, col, "Object, Array, or Tensor",
                                       recv_tag);
  }
  return {TAG_NIL, 0};  // unreachable: type_error_typed throws
}

// Cold path for JIT truthiness. The JIT inlines only the monomorphic Bool
// fast path (data != 0); Long/Float/error funnel here, so the strict to_bool
// semantics live in one place instead of a 5-block switch at every condition
// site. Nil and other tags raise; NaN is
// truthy (d != 0.0 is true for NaN, matching Python's bool(float('nan'))).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_to_bool_borrow(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  switch (tag) {
    case TAG_LONG:
      return data != 0;
    case TAG_FLOAT: {
      double d;
      std::memcpy(&d, &data, sizeof(d));
      return d != 0.0;
    }
    default:
      culebra_runtime_type_error_typed(line, col, "Bool, Long, or Float", tag);
  }
  return false;  // unreachable: type_error_typed throws
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_has(JitObject* obj,
                                                             const char* key) {
  // A packed view's "properties" are its @packable fields (not real
  // slots), so the compound-assign existence pre-check (`view.x += v`)
  // resolves against the layout.
  if (_jit_is_packed_view(obj)) {
    auto [core, base] = _jit_packed_view_record(obj);
    (void)base;
    return _jit_packed_view_layout(obj, *core).find(key) != nullptr;
  }
  return _find_property(obj, key) != nullptr;
}

// The UFCS gate's question for an Object receiver: does it resolve `key` as a
// property of its own? Own slots and the class meta answer for themselves;
// past those, a conforming instance also owns every trait default of that
// name, which is what keeps a same-named free function from hijacking the
// call (interp asks this in receiver_has_property). Gated on `proto` — a
// plain dict is not a class instance and inherits nothing — so a dict's
// lookup costs one extra null test.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool
culebra_runtime_object_has_or_trait_default(JitObject* obj, const char* key) {
  if (culebra_runtime_object_has(obj, key)) return true;
  if (!obj->proto) return false;
  return _jit_find_trait_default(obj, key) != nullptr;
}

// True iff `tag/data` is an Object carrying an OWN slot named `key` (proto /
// class methods excluded). Lets a user field that shadows a built-in method
// name (e.g. an autograd `grad` field set to nil) stay a first-class field,
// matching interp's `obj.has_own` precedence in eval_property — without it the
// bare-reference reject misreads a nil-valued own field as a method handle.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_has_own_field(
    int8_t tag, int64_t data, const char* key) {
  if (tag != TAG_OBJECT) return false;
  return reinterpret_cast<JitObject*>(data)->has_own(key);
}

// `Ns.member = v` where `member` isn't already an own member of the closed
// namespace `Ns` — same closed-member-set rule as a read
// (culebra_runtime_object_get_ic above), checked at the plain-assignment
// site so a typo minting a phantom member is caught immediately instead of
// silently succeeding. `??=`/compound need no counterpart call: `??=`
// re-reads the current value through the checked read path before ever
// reaching this write, and compound already pre-checks existence via
// object_has (jit.h compile_assign_complex).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_check_namespace_write(
    JitObject* obj, const char* key, int64_t line, int64_t col) {
  if (obj->is_namespace && !obj->has_own(key) &&
      !culebra::is_object_builtin_method_name(key)) {
    culebra::throw_namespace_missing_member_at(obj->ns_name, key, line, col);
  }
}

// `obj(args)` overload resolution: returns the class instance's
// `__call__` method as a borrowed TAG_FUNC JitValue, honoring own slots,
// the class meta (proto), and inherited trait defaults — the same
// resolution order as culebra_runtime_object_get_ic. Gated on
// `proto != null` so a plain dict that merely holds a "__call__" key
// stays non-callable, matching the interp class_tag gate and the
// __index__ design. Returns Nil for any non-instance / missing method,
// so the caller raises "type error: expected Function".
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_class_call_method(
    int8_t tag, int64_t data) {
  if (tag != TAG_OBJECT) return {TAG_NIL, 0};
  auto* obj = reinterpret_cast<JitObject*>(data);
  if (!obj->proto) return {TAG_NIL, 0};
  if (auto* e = _find_property(obj, "__call__")) {
    if (e->value.tag == TAG_FUNC) return e->value;
  }
  if (auto* d = _jit_find_trait_default(obj, "__call__"))
    return {TAG_FUNC, reinterpret_cast<int64_t>(d)};
  return {TAG_NIL, 0};
}

// Flag a freshly built class namespace object as callable — `C(args)`
// dispatches to its `new` (see culebra_runtime_class_new_method). Emitted by
// compile_class_decl; mirrors interp's `ObjectValue::is_class = true`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_mark_class(
    JitObject* o) {
  o->is_class = true;
}

// `C(args)` construction: returns the class object's `new` constructor as a
// borrowed TAG_FUNC JitValue, or Nil for a non-class. Gated on `is_class`
// (set only by CLASS_DECL) so a plain dict holding a "new" key stays
// non-callable, matching the interp is_class gate. The constructor needs no
// `self` (it builds its own instance), so the caller invokes it with a nil
// receiver — the twin of class_call_method but for the class object itself.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_class_new_method(
    int8_t tag, int64_t data) {
  if (tag != TAG_OBJECT) return {TAG_NIL, 0};
  auto* obj = reinterpret_cast<JitObject*>(data);
  if (!obj->is_class) return {TAG_NIL, 0};
  if (auto* e = _find_property(obj, "new")) {
    if (e->value.tag == TAG_FUNC) return e->value;
  }
  return {TAG_NIL, 0};
}

// `match v { x: ClassName => ... }` predicate. Returns true when
// `obj` carries a String `class` property whose value equals
// `expected`. Mirrors `culebra::class_tag()` + name comparison in
// the interp's `type_matches`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_class_matches(
    JitObject* obj, const char* expected) {
  auto* entry = _find_property(obj, "class");
  if (!entry || entry->value.tag != TAG_STRING) return false;
  auto* cls = reinterpret_cast<const char*>(entry->value.data);
  return cls && expected && std::strcmp(cls, expected) == 0;
}

// Generic `obj.has(k)`. String keys go through the shape path (matches
// `_find_property`'s proto walk); non-String keys check the sidecar.
//
// Consumes the caller's +1 to the key on the refcounted (sidecar)
// path so a Tuple-keyed `obj.has(k)` does not leak. TAG_STRING data is
// a borrowed cstring (non-refcounted) and needs no release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_has_value(
    JitObject* obj, int8_t tag, int64_t data) {
  std::string _kbuf;
  _jit_normalize_str_key(tag, data, _kbuf);
  if (tag == TAG_STRING) {
    auto* cstr = reinterpret_cast<const char*>(data);
    return _find_property(obj, cstr) != nullptr;
  }
  bool result = obj->non_string_props
                    ? obj->non_string_props->contains({tag, data})
                    : false;
  _culebra_value_release_impl(tag, data);
  return result;
}

// Build a "class meta" object that holds shared method closures for
// proto delegation. Called once per class declaration (compile-time
// emission, runtime allocation), captured in the constructor closure
// so each instance can point its `proto` at the same meta.
//
// The caller hands each method value +1-owned and does not use it again, so
// their references are *transferred* into the meta: object_set stores without
// retaining, and the meta's own destructor releases them. (A stray retain
// here left the caller's original +1 unreleased — a class declared inside a
// function leaked one closure per method on every call.)
// `lowered_state` flags a class a lowering synthesized (generator / effects
// state machine), so a value read of one of its instances' own slots stays
// unbound — see culebra_runtime_bind_method_value and the interp setting
// OrderedSymbolMap::lowered_state on the same object.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_build_class_meta(
    const char* const* method_names, const JitValue* method_vals,
    int64_t n_methods, int64_t flags) {
  auto* meta = culebra_runtime_object_new();
  meta->is_lowered_state = (flags & kClassMetaLoweredState) != 0;
  meta->names_class = (flags & kClassMetaNamesClass) != 0;
  meta->is_value = (flags & kClassMetaValue) != 0;
  int64_t i = 0;
  try {
    for (; i < n_methods; i++) {
      culebra_runtime_object_set(meta, method_names[i], /*mut*/ false,
                                 method_vals[i].tag, method_vals[i].data, 0,
                                 0);
    }
  } catch (...) {
    // Well-known contract error mid-build: the failing bind released its
    // own +1; release the not-yet-bound tail and the half-built meta.
    // Unbind drop first so the release can't fire a user `drop` on the
    // meta (it isn't an instance; the interp's decl throw runs none).
    for (int64_t j = i + 1; j < n_methods; j++)
      _culebra_value_release_impl(method_vals[j].tag, method_vals[j].data);
    _jit_owned_unbind_drop(meta);
    _culebra_value_release_impl(TAG_OBJECT, reinterpret_cast<int64_t>(meta));
    throw;
  }
  // The meta isn't an instance; undo the `has_drop` side-effect (and the
  // owned-stack registration) that `culebra_runtime_object_set` applied
  // when binding the "drop" method.
  _jit_owned_unbind_drop(meta);
  return meta;
}

// Invoke a class's synthetic field-init closure on a freshly bound `self`.
// Emitted into the `new` body's prologue right after parameter binding
// (see compile_fn_common), so declared-field initializers run only once
// the ctor args bound successfully — interp's init_instance_fields timing.
// `self` arrives borrowed; the retain feeds the callee's slot +1.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_run_field_init(
    JitClosure* finit, int8_t self_tag, int64_t self_data) {
  culebra_runtime_value_retain(self_tag, self_data);
  JitValue self_val = {self_tag, self_data};
  auto r = _jit_invoke(finit, self_val, 0, nullptr);
  _culebra_value_release_impl(r.tag, r.data);
}

// Class-sugar constructor body. Mirrors the tree interpreter's
// `eval_class_decl` constructor path:
//   1. Allocate a fresh instance (refcount=1, caller-owned).
//   2. Stamp the `class:` tag.
//   3. Wire the instance's `proto` at the shared class meta object;
//      method lookups fall through to it via _lookup_special /
//      culebra_runtime_object_get / object_has.
//   4. Invoke the synthetic field-init closure with `self` bound —
//      declaration-order, per-instance initializers, mirroring interp's
//      init_instance_fields. Only a `new`-less class passes one here; a
//      user `new` body invokes it itself right after parameter binding
//      (culebra_runtime_run_field_init above), so finit and body are
//      never both non-nil.
//   5. Invoke the user's `new` body (if any) with `self` bound to the
//      new instance. Args are forwarded with +1 ownership intact
//      (JIT ABI); the body's return value is discarded.
//   6. Promote every own property to mutable so later `self.x = y`
//      calls don't trip the immutable-property guard.
//
// `finit_tag`/`finit_data` and `body_tag`/`body_data` describe the
// field-init / user-body closures (TAG_FUNC) or TAG_NIL when absent.
// `class_meta` is borrowed; this helper retains it once per instance to
// balance the matching release in the Object destructor. `cls_tag`/`cls_data`
// is the constructor's receiver — the class object, when the call came
// through one — retained the same way (JitObject::cls).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_build_class_instance(
    const char* class_name, JitObject* class_meta, int8_t cls_tag,
    int64_t cls_data, int8_t finit_tag, int64_t finit_data, int8_t body_tag,
    int64_t body_data, int64_t n_args, JitValue* args) {
  auto* inst = culebra_runtime_object_new();

  // A `@value` instance's field set is closed from here on, not from the
  // freeze at the end: the declared stores below are `is_init` and no user
  // write is, so a constructor cannot slip in a field its siblings lack —
  // through a computed key or an alias any more than through `self.z = v`,
  // which the declaration refuses outright.
  inst->fields_closed = class_meta && class_meta->is_value;
  inst->proto = class_meta;
  // Retain the meta on the instance so it lives at least as long as
  // any of its instances. The matching release runs in the JitObject
  // destructor (release_impl GC_TAG_OBJECT path).
  if (class_meta) class_meta->refcount++;
  if (cls_tag == TAG_OBJECT &&
      reinterpret_cast<JitObject*>(cls_data)->is_class) {
    inst->cls = reinterpret_cast<JitObject*>(cls_data);
    inst->cls->refcount++;
  }
  // Mirror inherited `drop` so the destructor's `has_drop` gate fires,
  // and register the instance on the owned stack (deterministic drop).
  if (class_meta && _find_property(class_meta, "drop"))
    _jit_owned_bind_drop(inst);

  // `class_name` is a process-lifetime LLVM module global; TAG_STRING
  // values are borrowed (no refcount), so we can stash it directly
  // without the per-instance malloc + memcpy that `_culebra_heap_str`
  // would do.
  //
  // Appended directly rather than through culebra_runtime_object_set: on a
  // fresh instance the key always misses, so append_slot re-interns the same
  // one-key shape under the registry's lock per instantiation. That shape is
  // the same for every class in the process (it is keyed by name, not value),
  // so it resolves once here. Nothing else object_set does applies — "class"
  // is not in is_well_known_prop's set and is not "drop".
  static culebra::Shape* const kClassShape = culebra::shape_registry()
      .transition_add(culebra::shape_registry().root(), "class");
  inst->append_slot_shaped(
      kClassShape, {TAG_STRING, reinterpret_cast<int64_t>(class_name)},
      /*mut=*/false);

  JitValue self_val = {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
  // Each _jit_invoke consumes one retained `self` ref (the callee's slot
  // release); the single unwind guard covers the instance's original +1
  // across both calls.
  JitUnwindRelease g{self_val};

  if (finit_tag == TAG_FUNC) {
    auto* finit_cls = reinterpret_cast<JitClosure*>(finit_data);
    culebra_runtime_value_retain(self_val.tag, self_val.data);
    // A field-init throw unwinds before anything has consumed the
    // caller-transferred arg +1s (the `new` body normally absorbs them,
    // but it never runs) — release them on that edge only.
    struct ArgsRelease {
      int64_t n;
      JitValue* a;
      int exc = std::uncaught_exceptions();
      ~ArgsRelease() {
        if (std::uncaught_exceptions() <= exc) return;
        for (int64_t i = 0; i < n; i++)
          culebra_runtime_value_release(a[i].tag, a[i].data);
      }
    } args_guard{n_args, args};
    auto r = _jit_invoke(finit_cls, self_val, 0, nullptr);
    _culebra_value_release_impl(r.tag, r.data);
  }

  if (body_tag == TAG_FUNC) {
    auto* body_cls = reinterpret_cast<JitClosure*>(body_data);
    // Body consumes `self` via the slot +1 on function exit, so retain
    // before handing it off. If the body throws, the instance's +1 is
    // otherwise stranded — the guard releases it on the unwind edge.
    culebra_runtime_value_retain(self_val.tag, self_val.data);
    auto result = _jit_invoke(body_cls, self_val, n_args, args);
    _culebra_value_release_impl(result.tag, result.data);
  } else {
    // Default constructor: release any args the caller transferred.
    for (int64_t i = 0; i < n_args; i++) {
      _culebra_value_release_impl(args[i].tag, args[i].data);
    }
  }

  // A `@value` instance freezes here instead of promoting: its fields are
  // fixed once `new` returns, so every slot stays immutable and a later
  // `v.x = 5` lands on the ordinary ImmutableError, while the flag closes
  // the field set against adds and removes.
  const bool freeze = class_meta && class_meta->is_value;
  for (auto& entry : inst->slots) {
    entry.mut = !freeze;
  }
  inst->frozen = freeze;

  return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
}

// Build a range value `{class:"Range", start, end, inclusive, step}`. An
// absent endpoint (open-ended range) is stored Nil; `step` defaults to 1 and
// is never Nil. Mirrors the interpreter's _make_range so both backends
// represent a range identically. Returns a fresh +1 JitObject.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_make_range(
    int8_t has_start, int64_t start, int8_t has_end, int64_t end,
    int8_t inclusive, int64_t step) {
  // Header-backed: every TAG_STRING must carry the length at data[-8]
  // (_str_len); a bare char array segfaults the structural `==` walk.
  static const struct { JitStrHeader h; char bytes[6]; } kRange = {{5},
                                                                   "Range"};
  auto* o = culebra_runtime_object_new();
  culebra_runtime_object_set(o, "class", false, TAG_STRING,
                             reinterpret_cast<int64_t>(kRange.bytes), 0, 0);
  culebra_runtime_object_set(o, "start", false,
                             has_start ? TAG_LONG : TAG_NIL,
                             has_start ? start : 0, 0, 0);
  culebra_runtime_object_set(o, "end", false, has_end ? TAG_LONG : TAG_NIL,
                             has_end ? end : 0, 0, 0);
  culebra_runtime_object_set(o, "inclusive", false, TAG_BOOL,
                             inclusive ? 1 : 0, 0, 0);
  culebra_runtime_object_set(o, "step", false, TAG_LONG, step, 0, 0);
  return o;
}

// True iff `tag:data` is a range value — an Object carrying the full
// Range shape (_jit_is_range_shaped), not just a class:"Range" tag.
// Cheap: a non-Object short-circuits on the tag before any slot scan.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_is_range(
    int8_t tag, int64_t data) {
  if (tag != TAG_OBJECT) return 0;
  return _jit_is_range_shaped(reinterpret_cast<JitObject*>(data)) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_object_size(
    JitObject* obj) {
  int64_t n = static_cast<int64_t>(obj->prop_size());
  if (obj->non_string_props) {
    n += static_cast<int64_t>(obj->non_string_props->size());
  }
  return n;
}

}  // extern "C" (block continues in jit_dispatch.h)

