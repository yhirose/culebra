#pragma once

// FixedArray / BoundedArray / BoundedSet / BoundedMap views (byte ops shared
// with the [i] index hooks), packable registration, object any-key runtime,
// inline caches, class meta/instance construction and the @packable record
// codec under String.pack.
//
// Runtime-layer fragment of rt.h, split out for readability. These
// fragments rely on rt.h's #include block and are included by rt.h in a
// fixed sequence (see rt.h); they are not standalone headers.

// Continues the runtime extern "C" block opened in rt_runtime.inc.h
// (split across rt_fixed.inc.h / rt_dispatch.inc.h / rt_iter.inc.h).
extern "C" {

// Handle plumbing shared by every native-handle builder (Proc / File /
// Foreign / wrap.h-generated handles): a typed slot read, the
// captureless method-closure constructor, and the bind helper. Live
// here (not stdlib_rt.h) so wrap.h's generated thunks can use them.
inline int64_t _jit_handle_long(JitObject* h, const char* key) {
  size_t i = h->find_slot(key);
  return i == static_cast<size_t>(-1) ? -1 : h->slots[i].value.data;
}

// Every closure is born through this one constructor, which is where what it
// is (rt_value.inc.h JIT_CLOSURE_*) and what its body declares are both
// fixed. Defined in rt_dispatch.inc.h, included after this file.
struct JitParamMeta;
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity, uint64_t flags,
    const JitParamMeta* meta);

// Native-handle method param metadata (kwargs binding) — built by
// _jit_make_handle_meta (defined after JitParamMeta below) and carried by
// the closure itself, so the kwargs resolver can bind by name.
inline const JitParamMeta* _jit_make_handle_meta(
    std::vector<std::string> names, std::vector<bool> has_default);

inline JitClosure* _jit_make_handle_method(
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*), size_t arity,
    const JitParamMeta* meta = nullptr, uint64_t extra_flags = 0) {
  return culebra_runtime_closure_new(reinterpret_cast<void*>(fn),
                                     /*n_captures=*/0, arity,
                                     JIT_CLOSURE_NATIVE | extra_flags, meta);
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
    const JitParamMeta* meta = nullptr, uint64_t extra_flags = 0) {
  h->set_or_append(name,
      JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
          _jit_make_handle_method(f, ar, meta, extra_flags))}, false);
}

// A handle method a value may carry into another Isolate
// (JIT_CLOSURE_PORTABLE): the body must read nothing but `self` and its args,
// and `meta` must outlive every Runtime (a function-local static), since the
// receiver rebuilds the closure over the same raw pointer.
inline void _jit_bind_portable_method(
    JitObject* h, const char* name,
    void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*), size_t ar,
    const JitParamMeta* meta = nullptr) {
  _jit_handle_bind_method(h, name, f, ar, meta, JIT_CLOSURE_PORTABLE);
}


// A captureless native method closure (reads its state from `self`), for the
// metas the runtime's handles share.
inline JitClosure* _jit_native_method(
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
  return culebra_runtime_closure_new(reinterpret_cast<void*>(fn),
                                     /*n_captures=*/0, /*arity=*/0,
                                     JIT_CLOSURE_NATIVE, /*meta=*/nullptr);
}

// Bind the captureless native method `fn` as `nm` on a handle's meta (an
// immutable slot).
inline void _jit_view_method(
    JitObject* o, const char* nm,
    void (*fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
  o->set_or_append(
      nm, JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(fn))},
      false);
}

// --- View methods + iterators (native, captureless) ------------------------
// A packed view's index — an array view or a SharedBuffer — which coerces a Long
// or a Float as the interp's `key.to_long()` does.
inline int64_t _jit_view_index(int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_LONG) return data;
  if (tag == TAG_FLOAT)
    return culebra::double_to_long(_culebra_float_to_double(data), line, col);
  throw culebra::CulebraError("TypeError", "type error: expected Long or Float",
                              line, col);
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

// `for x in it`'s `it.iter()` on an iterator: the iterator itself.
inline void _jit_iter_self(JitValue* __ret, JitClosure*, int8_t self_tag,
                           int64_t self_data, int64_t, JitValue*) {
  culebra_runtime_value_retain(self_tag, self_data);
  *__ret = {self_tag, self_data};
}

// The view a method was called on: its methods are only reached through one.
inline JitBufView& _jit_self_view(JitValue self) {
  auto* v = self.tag == TAG_OBJECT
                ? _jit_buf_view(reinterpret_cast<JitObject*>(self.data))
                : nullptr;
  if (!v) throw culebra::CulebraError("TypeError", "not a SharedBuffer view");
  return *v;
}

// A view's for-in cursor (a handle, see JitHandle): the view it walks, and
// how far it has come.
struct JitBufViewIter final : JitHandle {
  JitValue view;  // +1 on the view
  int64_t pos = 0;
  explicit JitBufViewIter(JitObject* v)
      : JitHandle(Kind::BufViewIter),
        view{TAG_OBJECT, reinterpret_cast<int64_t>(v)} {
    culebra_runtime_value_retain(view.tag, view.data);
  }
  JitBufView& target() const {
    return *_jit_buf_view(reinterpret_cast<JitObject*>(view.data));
  }
  std::string show() const override {
    return "[" + target().what().substr(2) + " iterator]";
  }
  bool same(const JitHandle&) const override { return false; }
  void each_value(Visit visit, void* ctx) override { visit(ctx, view); }
};
inline JitBufViewIter& _jit_self_view_iter(JitValue self) {
  auto* h = self.tag == TAG_OBJECT
                ? reinterpret_cast<JitObject*>(self.data)->handle_of()
                : nullptr;
  if (!h || h->kind != JitHandle::Kind::BufViewIter)
    throw culebra::CulebraError("TypeError", "not a SharedBuffer view iterator");
  return *static_cast<JitBufViewIter*>(h);
}

extern "C++" {
// The native-method ABI around a view's method `F`: `self` and the args come
// at +1 and go on every exit, `F` sees them borrowed, and a call with fewer
// arguments than `Need` names is the missing-required ArityError. What `F`
// raises without a position reports at the call, as on every lane.
template <JitValue (*F)(JitBufView&, JitValue*),
          const char* const* Need = nullptr>
void _jit_view_native(JitValue* ret, JitClosure*, int8_t self_tag,
                      int64_t self_data, int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  JitMethodArgs _a{n, args};
  auto& v = _jit_self_view(self);
  if constexpr (Need != nullptr) {
    int64_t need = 0;
    while (Need[need]) need++;
    if (n < need) culebra_runtime_arity_missing(Need, n, 0, 0);
  }
  *ret = _jit_at_call_site([&] { return F(v, args); });
}
template <JitValue (*F)(JitBufViewIter&)>
void _jit_view_iter_native(JitValue* ret, JitClosure*, int8_t self_tag,
                           int64_t self_data, int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  JitMethodArgs _a{n, args};
  auto& it = _jit_self_view_iter(self);
  *ret = _jit_at_call_site([&] { return F(it); });
}
inline constexpr const char* kViewIndex[] = {"index", nullptr};
inline constexpr const char* kViewIndexValue[] = {"index", "value", nullptr};
inline constexpr const char* kViewValue[] = {"value", nullptr};
inline constexpr const char* kViewKey[] = {"key", nullptr};
inline constexpr const char* kViewKeyValue[] = {"key", "value", nullptr};
}  // extern "C++"

inline JitValue _jit_view_size(JitBufView& v, JitValue*) {
  return {TAG_LONG, _jit_coll_count(v.info(), v.bytes())};
}
inline JitValue _jit_view_capacity(JitBufView& v, JitValue*) {
  v.core();  // a dropped buffer's view has none
  return {TAG_LONG, static_cast<int64_t>(v.info().capacity)};
}
inline void _jit_view_iter(JitValue* __ret, JitClosure*, int8_t self_tag,
                           int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto& v = _jit_self_view(self);
  *__ret = _jit_make_handle(
      new JitBufViewIter(reinterpret_cast<JitObject*>(self.data)),
      _jit_buf_view_meta(v.of, false, true));
}

// --- FixedArray / BoundedArray ----------------------------------------------
inline culebra::PackableField _jit_view_elem(const JitBufView& v) {
  return culebra::PackableField::scalar(v.info().elem_type);
}
// Element `i` (a negative one from the end) of an array view; past its end
// an IndexError.
inline uint8_t* _jit_fa_at(const JitBufView& v, int64_t i, int64_t line = 0,
                           int64_t col = 0) {
  uint8_t* p = v.bytes(line, col);
  int64_t n = _jit_coll_count(v.info(), p);
  if (i < 0) i += n;
  if (i < 0 || i >= n)
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  return _jit_coll_elem(v.info(), p, i);
}
inline JitValue _jit_fa_get(const JitBufView& v, int64_t i, int64_t line = 0,
                            int64_t col = 0) {
  return _jit_packable_read_field(_jit_fa_at(v, i, line, col),
                                  _jit_view_elem(v));
}
// Borrows the value.
inline void _jit_fa_set(const JitBufView& v, int64_t i, int8_t tag,
                        int64_t data, int64_t line = 0, int64_t col = 0) {
  _jit_packable_write_field(_jit_fa_at(v, i, line, col), _jit_view_elem(v),
                            tag, data);
}
inline JitValue _jit_fa_push(JitBufView& v, JitValue* args) {
  uint8_t* p = v.bytes();
  int64_t len = _jit_coll_count(v.info(), p);
  auto cap = static_cast<int64_t>(v.info().capacity);
  if (len >= cap)
    throw culebra::CulebraError(
        "IndexError", culebra::format("BoundedArray is full (capacity {})", cap));
  _jit_packable_write_field(_jit_coll_elem(v.info(), p, len), _jit_view_elem(v),
                            args[0].tag, args[0].data);
  _jit_coll_set_count(p, len + 1);
  return {TAG_NIL, 0};
}
inline JitValue _jit_fa_get_m(JitBufView& v, JitValue* args) {
  return _jit_fa_get(v, _jit_view_index(args[0].tag, args[0].data, 0, 0));
}
inline JitValue _jit_fa_set_m(JitBufView& v, JitValue* args) {
  _jit_fa_set(v, _jit_view_index(args[0].tag, args[0].data, 0, 0), args[1].tag,
              args[1].data);
  return {TAG_NIL, 0};
}
inline JitValue _jit_fa_iter_has_next(JitBufViewIter& it) {
  auto& v = it.target();
  return {TAG_BOOL, it.pos < _jit_coll_count(v.info(), v.bytes()) ? 1 : 0};
}
inline JitValue _jit_fa_iter_next(JitBufViewIter& it) {
  JitValue r = _jit_fa_get(it.target(), it.pos);
  it.pos++;
  return r;
}

// --- BoundedSet<T,N> / BoundedMap<K,V,N>: open-addressed hash tables over the
// bytes, probed by culebra::fixed_probe. -------------------------------------
// The slot of `key` in the table at `p`, or -1; `*insert` (when asked) the
// slot an add would take, or -1 when it is full, and `encoded` (when asked)
// the key's bytes. A key of the wrong type is a TypeError
// (_jit_packable_write_field).
inline int64_t _jit_table_find(const JitBufView& v, uint8_t* p, JitValue key,
                               int64_t* insert = nullptr,
                               uint8_t* encoded = nullptr) {
  uint8_t local[8];
  if (!encoded) encoded = local;
  std::memset(encoded, 0, 8);
  _jit_packable_write_field(encoded, _jit_view_elem(v), key.tag, key.data);
  const auto& l = v.info();
  return culebra::fixed_probe(_jit_coll_states(p), _jit_coll_elem(l, p, 0),
                              l.capacity, l.elem_size, l.elem_size, encoded,
                              insert);
}
// The slot `key` has or takes, adding it when it is new: full, a
// CapacityError naming `what`.
inline int64_t _jit_table_put(const JitBufView& v, uint8_t* p, JitValue key,
                              const char* what) {
  uint8_t encoded[8];
  int64_t insert = -1;
  int64_t slot = _jit_table_find(v, p, key, &insert, encoded);
  if (slot >= 0) return slot;
  if (insert < 0)
    throw culebra::CulebraError(
        "CapacityError",
        culebra::format("{} is full (capacity {})", what, v.info().capacity));
  std::memcpy(_jit_coll_elem(v.info(), p, insert), encoded, v.info().elem_size);
  _jit_coll_states(p)[insert] = culebra::kFixedFull;
  _jit_coll_set_count(p, _jit_coll_count(v.info(), p) + 1);
  return insert;
}
// The first occupied slot at `from` or after it, or the capacity.
inline int64_t _jit_table_next(const JitBufView& v, const uint8_t* p,
                               int64_t from) {
  auto cap = static_cast<int64_t>(v.info().capacity);
  for (int64_t i = from; i < cap; i++)
    if (_jit_coll_full(p, i)) return i;
  return cap;
}

inline JitValue _jit_table_contains(JitBufView& v, JitValue* args) {
  return {TAG_BOOL, _jit_table_find(v, v.bytes(), args[0]) >= 0 ? 1 : 0};
}
inline JitValue _jit_table_remove(JitBufView& v, JitValue* args) {
  uint8_t* p = v.bytes();
  int64_t slot = _jit_table_find(v, p, args[0]);
  if (slot < 0) return {TAG_BOOL, 0};
  _jit_coll_states(p)[slot] = culebra::kFixedTomb;
  _jit_coll_set_count(p, _jit_coll_count(v.info(), p) - 1);
  return {TAG_BOOL, 1};
}
inline JitValue _jit_fs_add(JitBufView& v, JitValue* args) {
  _jit_table_put(v, v.bytes(), args[0], "BoundedSet");
  return {TAG_NIL, 0};
}
inline JitValue _jit_fm_get(JitBufView& v, JitValue* args) {
  uint8_t* p = v.bytes();
  int64_t slot = _jit_table_find(v, p, args[0]);
  return slot < 0 ? JitValue{TAG_NIL, 0} : _jit_coll_read_val(v.info(), p, slot);
}
inline JitValue _jit_fm_set(JitBufView& v, JitValue* args) {
  uint8_t* p = v.bytes();
  int64_t slot = _jit_table_put(v, p, args[0], "BoundedMap");
  _jit_packable_write_field(_jit_coll_val(v.info(), p, slot),
                            culebra::PackableField::scalar(v.info().val_type),
                            args[1].tag, args[1].data);
  return {TAG_NIL, 0};
}
inline JitValue _jit_fm_keys(JitBufView& v, JitValue*) {
  uint8_t* p = v.bytes();
  auto* arr = culebra_runtime_array_new();
  auto cap = static_cast<int64_t>(v.info().capacity);
  for (int64_t i = _jit_table_next(v, p, 0); i < cap;
       i = _jit_table_next(v, p, i + 1)) {
    JitValue k = _jit_coll_read(v.info(), p, i);
    culebra_runtime_array_push(arr, k.tag, k.data);
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
}
inline JitValue _jit_table_iter_has_next(JitBufViewIter& it) {
  auto& v = it.target();
  return {TAG_BOOL, _jit_table_next(v, v.bytes(), it.pos) <
                            static_cast<int64_t>(v.info().capacity)
                        ? 1
                        : 0};
}
// The cursor's next occupied slot, which it moves past; past the last one an
// IndexError, as an array view's cursor raises.
inline int64_t _jit_table_iter_take(JitBufViewIter& it, const uint8_t* p) {
  auto& v = it.target();
  int64_t slot = _jit_table_next(v, p, it.pos);
  if (slot >= static_cast<int64_t>(v.info().capacity))
    throw culebra::CulebraError("IndexError", "index out of range");
  it.pos = slot + 1;
  return slot;
}
inline JitValue _jit_fs_iter_next(JitBufViewIter& it) {
  const uint8_t* p = it.target().bytes();
  return _jit_coll_read(it.target().info(), p, _jit_table_iter_take(it, p));
}
inline JitValue _jit_fm_iter_next(JitBufViewIter& it) {
  const uint8_t* p = it.target().bytes();
  const auto& l = it.target().info();
  int64_t slot = _jit_table_iter_take(it, p);
  JitValue k = _jit_coll_read(l, p, slot);
  JitValue val = _jit_coll_read_val(l, p, slot);
  auto* tup = culebra_runtime_tuple_new();
  culebra_runtime_tuple_push(tup, k.tag, k.data);
  culebra_runtime_tuple_push(tup, val.tag, val.data);
  return {TAG_TUPLE, reinterpret_cast<int64_t>(tup)};
}

// The methods read their state from `self`. A record's meta has none, since
// its names are its fields.
inline JitObject* _jit_buf_view_meta(JitBufView::Of of, bool fixed,
                                     bool iter) {
  using Of = JitBufView::Of;
  int key = static_cast<int>(of) * 4 + (iter ? 1 : fixed ? 2 : 0);
  return _jit_pinned_meta(culebra::kSlotBufViewMetas, key, [&] {
    auto* m = culebra_runtime_object_new();
    if (iter) {
      _jit_view_method(m, "iter", _jit_iter_self);
      _jit_view_method(m, "has_next",
                       of == Of::Array
                           ? _jit_view_iter_native<_jit_fa_iter_has_next>
                           : _jit_view_iter_native<_jit_table_iter_has_next>);
      _jit_view_method(m, "next",
                       of == Of::Array ? _jit_view_iter_native<_jit_fa_iter_next>
                       : of == Of::Set ? _jit_view_iter_native<_jit_fs_iter_next>
                                       : _jit_view_iter_native<_jit_fm_iter_next>);
      return m;
    }
    if (of == Of::Record) return m;
    _jit_view_method(m, "size", _jit_view_native<_jit_view_size>);
    _jit_view_method(m, "capacity", _jit_view_native<_jit_view_capacity>);
    _jit_view_method(m, "iter", _jit_view_iter);
    switch (of) {
      case Of::Record: break;
      case Of::Array:
        if (!fixed)
          _jit_view_method(m, "push", _jit_view_native<_jit_fa_push, kViewValue>);
        _jit_view_method(m, "get", _jit_view_native<_jit_fa_get_m, kViewIndex>);
        _jit_view_method(m, "set",
                         _jit_view_native<_jit_fa_set_m, kViewIndexValue>);
        break;
      case Of::Set:
        _jit_view_method(m, "contains",
                         _jit_view_native<_jit_table_contains, kViewValue>);
        _jit_view_method(m, "add", _jit_view_native<_jit_fs_add, kViewValue>);
        _jit_view_method(m, "remove",
                         _jit_view_native<_jit_table_remove, kViewValue>);
        break;
      case Of::Map:
        _jit_view_method(m, "contains",
                         _jit_view_native<_jit_table_contains, kViewKey>);
        _jit_view_method(m, "get", _jit_view_native<_jit_fm_get, kViewKey>);
        _jit_view_method(m, "set", _jit_view_native<_jit_fm_set, kViewKeyValue>);
        _jit_view_method(m, "remove",
                         _jit_view_native<_jit_table_remove, kViewKey>);
        _jit_view_method(m, "keys", _jit_view_native<_jit_fm_keys>);
        break;
    }
    return m;
  });
}

// `buf[i]` -> a fresh view of record `i` (refcount 1, caller owns it).
inline JitObject* _jit_shared_buffer_index(JitObject* buf, int64_t idx,
                                           int64_t line, int64_t col) {
  auto* core = _jit_buffer_core(buf, line, col);
  auto n = static_cast<int64_t>(core->count);
  if (idx < 0) idx += n;
  if (idx < 0 || idx >= n) {
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  }
  JitValue v = _jit_make_buf_view(
      new JitBufView(buf, idx * static_cast<int64_t>(core->layout.stride)));
  return reinterpret_cast<JitObject*>(v.data);
}

// Register a @packable class layout at *runtime* (when its declaration
// executes), from a compact "name:Type;name:Type" spec the codegen emits.
// AOT compiles and runs in separate processes, so a compile-time
// registration would be invisible to the standalone binary — the layout
// must land in the running process's registry. JIT/interp run in the same
// process, so this is equivalent there. The spec's field types are already
// lint-validated, so compute_packable_layout won't throw here.
// `cls` is the class object: its meta keeps the class's own copy, with the
// shape and byte-stream size String.pack reads (JitRecordInfo). The
// name-keyed registry is what SharedBuffer and a nested field look up.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_register_packable(
    const char* name, const char* spec, JitObject* cls) {
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
  auto layout = culebra::compute_packable_layout(name, fields);
  if (auto* meta = cls ? cls->class_meta_of() : nullptr) {
    auto info = std::make_unique<JitRecordInfo>();
    info->layout = layout;
    auto& shapes = culebra::shape_registry();
    info->shape = shapes.root();
    for (const auto& f : info->layout.fields)
      info->shape = shapes.transition_add(
          info->shape, f.name, culebra::field_type_for_annotation(f.type));
    if (auto size = culebra::packable_wire_size(info->layout, &info->no_wire))
      info->wire_size = *size;
    meta->specials->record = std::move(info);
  }
  culebra::register_packable_layout(name, std::move(layout));
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
// retained). Sources: Array / Tuple / Set, and a view that reads as one. The
// spread value's own reference is dropped by the caller.
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
      culebra_runtime_value_retain(m.value().tag, m.value().data);
      culebra_runtime_array_push(arr, m.value().tag, m.value().data);
    }
  } else if (JitValue whole; tag == TAG_OBJECT &&
             _jit_view_plain(reinterpret_cast<JitObject*>(data), &whole, line,
                             col)) {
    // A view is spread as the value it reads as.
    JitOwnedVal plain(whole);
    culebra_runtime_array_extend(arr, plain.v.tag, plain.v.data, line, col);
  } else {
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("cannot spread {} into an array (Array/Tuple/Set only)",
                        _culebra_tag_name(tag)),
        line, col);
  }
}

// `obj[key] = value` fallback to a user-defined `__setindex__(key, value)`.
// Returns true when the method exists (and was invoked), else false.
inline bool _jit_try_object_setindex(JitObject* obj, int8_t key_tag,
                                     int64_t key_data, int8_t val_tag,
                                     int64_t val_data, int64_t line,
                                     int64_t col) {
  // Class-instance feature only (the call sites also gate on `proto` to
  // skip the slot probe for plain dicts; this keeps the helper self-safe).
  if (!obj->proto()) return false;
  auto* cls = _lookup_special(TAG_OBJECT, reinterpret_cast<int64_t>(obj),
                              Special::SetIndex);
  if (!cls) return false;
  culebra_runtime_set_op_pos(line, col);
  // Borrows the key and value: the caller consumes them on the normal path and
  // guards them over this call for the throw path.
  auto r = _culebra_invoke_method2(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(obj)}, {key_tag, key_data},
      {val_tag, val_data});
  _culebra_value_release_impl(r.tag, r.data);  // discard the return value
  return true;
}

// String and StringView are the same dict key (a `==`-equal pair): the slot
// either reaches is found by the name it spells, and this is that name, or
// null for a key of any other type. `owned` backs a view's bytes as a C string
// and must outlive the lookup. The key itself stays the value the program
// wrote, which is what goes on to anything that reads it as one (a class's
// `__index__`, the insertion order, a Shared view): the name is no String, it
// has no length header.
inline const char* _jit_key_name(int8_t key_tag, int64_t key_data,
                                 std::string& owned) {
  if (key_tag == TAG_STRING) return reinterpret_cast<const char*>(key_data);
  if (key_tag != TAG_STRINGVIEW) return nullptr;
  auto* sv = reinterpret_cast<JitStringView*>(key_data);
  owned.assign(sv->ptr, sv->len);
  return owned.c_str();
}

// A Shared view, or a JitHandle (a SharedBuffer or a view of one, a channel
// end): what it holds is not entries.
inline bool _jit_is_handle(JitObject* obj) {
  return obj->is_shared_val || obj->has_handle;
}
// ...so what walks an Object's own entries refuses one.
inline void _jit_require_own_entries(JitObject* obj, int64_t line = 0,
                                     int64_t col = 0) {
  if (_jit_is_handle(obj))
    throw culebra::CulebraError(
        "TypeError",
        "a Shared view, a SharedBuffer or a view of one, or a channel end has "
        "no entries of its own to walk",
        line, col);
}

// Whether a subscript on `obj` is a question about its own entries. A handle
// reads one its own way, ahead of any entry, and so does a Regex match (whose
// entries are its fields, and whose subscript is a capture group).
inline bool _jit_keyed_by_entries(JitObject* obj) {
  return !(_jit_is_handle(obj) || obj->is_match);
}

// A class instance's `__setindex__` is asked first about a String key that is
// none of the instance's own slots.
inline bool _jit_setindex_first(JitObject* obj, const char* name) {
  return obj->proto() && obj->find_slot(name) == static_cast<size_t>(-1);
}

// The sidecar store for a key already hashed: insert, or overwrite the value
// of an equal stored key. Consumes the key's +1 and the value's on every exit:
// on insert the key's +1 becomes the map's, and key_order takes a second.
inline void _jit_object_set_hashed(JitObject* obj, const JitHashedKey& key,
                                   bool mut, JitValue val, int64_t line,
                                   int64_t col, bool is_init) {
  JitUnwindRelease g{key.value(), val};
  if (!obj->non_string_props) {
    obj->non_string_props = new JitObject::AnyKeyMap();
    // First non-String key: activate key_order and back-fill with the
    // String keys already present so interleaved order survives. A
    // dictionary-mode object already keeps key_order eagerly.
    if (!obj->key_order) {
      obj->key_order = new std::vector<JitHashedKey>();
      if (obj->shape) {
        obj->key_order->reserve(obj->prop_size() + 1);
        for (size_t i = 0; i < obj->prop_size(); i++)
          obj->key_order->push_back(JitObject::name_key(obj->prop_name(i)));
      }
    }
  }
  auto [it, inserted] = obj->non_string_props->try_emplace(
      key, JitObjectEntry{val, mut});
  if (inserted) {
    culebra_runtime_value_retain(key.value().tag, key.value().data);
    obj->key_order->push_back(key);
    return;
  }
  // Object-literal construction (`is_init`) overwrites a duplicate key
  // last-wins like the interp's `initialize`; only a post-construction
  // `o[k] = v` (is_init=false) honors the slot's immutable flag.
  if (!is_init && !it->second.mut)
    throw culebra::CulebraError("ImmutableError",
                                "immutable entry on non-String key", line, col);
  if (is_init) it->second.mut = mut;
  _jit_replace_value(it->second.value, val.tag, val.data);
  _culebra_value_release_impl(key.value().tag, key.value().data);
}

// Hashable-key access into an Object. String keys unify with the
// shape-based `obj.foo` path so `obj["x"] = v` and `obj.x = v` reach
// the same slot; everything else (Long/Float/Bool/Nil/Tuple) lands in
// the sidecar AnyKeyMap. Object-literal keys are pre-filtered by the
// grammar (no String literal keys), so the named branch only fires
// for runtime-keyed subscript ops.
//
// The shape-path call below passes 0/0 for the IC slot, so a String-
// keyed subscript write never inline-caches. Hot loops should still
// prefer `obj.x = v` over `obj["x"] = v` for that reason.
//
// Consumes the caller's +1 to the key and the value on every exit. `name` is
// the key's `_jit_key_name`; a caller that has hashed the key already passes
// that as `hashed`.
inline void _jit_object_set_any(JitObject* obj, const char* name,
                                int8_t key_tag, int64_t key_data,
                                const std::optional<JitHashedKey>& hashed,
                                bool mut, int8_t val_tag, int64_t val_data,
                                int64_t line, int64_t col, bool is_init) {
  if (_jit_is_handle(obj)) {
    // Only a number names an array view's element, and no handle keeps the key.
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    // `arr[i] = v` on an array view: write element i into the inline bytes
    // (the index coerces Long/Float like the interp).
    if (auto* view = _jit_array_view(obj)) {
      JitOwnedVal val{val_tag, val_data};
      int64_t i = _jit_view_index(key_tag, key_data, line, col);
      _jit_fa_set(*view, i, val_tag, val_data, line, col);
      return;
    }
    // `rec["x"] = v` on a record view is its field's write.
    if (auto* view = _jit_record_view(obj)) {
      if (name)
        return _jit_packed_view_set(*view, name, val_tag, val_data, line, col);
      _culebra_value_release_impl(val_tag, val_data);
      throw culebra::CulebraError(
          "TypeError", "a @packable record view's fields are named by a String",
          line, col);
    }
    // Every other handle refuses the write.
    // `buf[i] = v` on a SharedBuffer: a packed record has no Value form, so
    // reject (set fields via `buf[i].field = v`). Matches the interp guard.
    if (_jit_is_shared_buffer(obj)) {
      _culebra_value_release_impl(val_tag, val_data);
      throw culebra::CulebraError("TypeError",
          "cannot assign to a SharedBuffer element directly; "
          "set fields via buf[i].field = value", line, col);
    }
    // Shared.new views are immutable — index writes included.
    if (obj->is_shared_val) {
      _culebra_value_release_impl(val_tag, val_data);
      throw culebra::CulebraError("ImmutableError",
                                  "Shared values are immutable", line, col);
    }
    _jit_handle_reject_write(obj, val_tag, val_data, line, col);
  }
  if (name) {
    // Subscript overloading is a class-instance feature (proto != null); a
    // class instance may define `__setindex__` for keys that aren't one of
    // its own slots. A plain dict short-circuits on `proto` and sets the
    // slot directly, so its hot path keeps a single lookup (string key is
    // non-refcounted, so only the consumed value is released here).
    if (_jit_setindex_first(obj, name)) {
      // A user `__setindex__` body's throw unwinds past the release below, so
      // guard the value's +1 across the dispatch.
      JitUnwindRelease g{JitValue{val_tag, val_data}};
      if (_jit_try_object_setindex(obj, key_tag, key_data, val_tag, val_data, line, col)) {
        _culebra_value_release_impl(val_tag, val_data);
        return;
      }
    }
    culebra_runtime_object_set(obj, name, mut, val_tag, val_data, line, col,
                               is_init);
    return;
  }
  culebra_runtime_set_op_pos(line, col);  // a key's hash/eq entry site
  // Everything below consumes the key and value this helper was handed, and
  // both a user `hash()` and a user `__setindex__` can throw in the middle —
  // one guard releases them on every such edge. The paths that hand the refs
  // on return immediately.
  JitUnwindRelease g{JitValue{key_tag, key_data}, JitValue{val_tag, val_data}};
  JitValue unhashable;
  auto key = hashed ? hashed
                    : JitHashedKey::try_of({key_tag, key_data}, &unhashable);
  // A class instance may route a not-yet-stored key to __setindex__ before
  // the sidecar is activated (key + value are consumed by this helper's
  // contract). A plain dict skips this probe entirely.
  if (obj->proto() && !(key && _jit_sidecar_at(obj, *key)) &&
      _jit_try_object_setindex(obj, key_tag, key_data, val_tag, val_data, line,
                               col)) {
    _culebra_value_release_impl(key_tag, key_data);
    _culebra_value_release_impl(val_tag, val_data);
    return;
  }
  // The sidecar's half of the frozen field set — checked before the store so
  // a refused write leaves the instance untouched. A `@value` instance can
  // never hold a non-String key (this is the only branch that mints one), so
  // any write reaching here is an add. The guard above owns the key and the
  // value on this edge; throw without releasing.
  if (_jit_value_add_refused(obj, /*is_init=*/false))
    _jit_throw_value_add(nullptr, line, col);
  if (!key) _jit_throw_unhashable(unhashable);
  g.disarm();
  _jit_object_set_hashed(obj, *key, mut, {val_tag, val_data}, line, col,
                         is_init);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_any(
    JitObject* obj, int8_t key_tag, int64_t key_data, bool mut,
    int8_t val_tag, int64_t val_data, int64_t line, int64_t col,
    bool is_init) {
  std::string _kbuf;
  _jit_object_set_any(obj, _jit_key_name(key_tag, key_data, _kbuf), key_tag,
                      key_data, std::nullopt, mut, val_tag, val_data, line, col,
                      is_init);
}

// Whether `obj[key] = v`, for a String key a caller holds as its name and no
// value (a host), is the store culebra_runtime_object_set makes by that name.
// False where the key goes on as a value, so the caller has to make it one: a
// class's `__setindex__` receives it, or the receiver is a handle.
inline bool culebra_runtime_object_sets_named(JitObject* obj,
                                              const char* name) {
  return !_jit_is_handle(obj) && !_jit_setindex_first(obj, name);
}

// `{...x}` object spread: merge another Object's own entries into `dst`, in
// its insertion order (later keys win). Merged keys are made mutable so
// explicit properties after the spread can override them. A non-String key
// moves with the hash it is stored under. The spread value's own reference is
// dropped by the caller.
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
  // A view is spread as the value it reads as; a handle of another kind has
  // no entries to copy.
  if (JitValue whole; _jit_view_plain(src, &whole, line, col)) {
    JitOwnedVal plain(whole);
    culebra_runtime_object_merge(dst, plain.v.tag, plain.v.data, line, col);
    return;
  }
  _jit_require_own_entries(src, line, col);
  // Each insert takes its own +1 to the value, and to a non-String key. One
  // into the sidecar can run a key's `eq`, which may change `src`: the walk
  // allows for that, and the caller holds `src` itself.
  _jit_each_own_entry(
      src,
      [&](std::string_view name, const JitObjectEntry& entry) {
        JitValue v = entry.value;
        culebra_runtime_value_retain(v.tag, v.data);
        culebra_runtime_object_set(dst, std::string(name).c_str(), /*mut=*/true,
                                   v.tag, v.data, line, col, /*is_init=*/true);
      },
      [&](const JitHashedKey& k, const JitObjectEntry& entry) {
        JitValue v = entry.value;
        culebra_runtime_value_retain(k.value().tag, k.value().data);
        culebra_runtime_value_retain(v.tag, v.data);
        _jit_object_set_hashed(dst, k, /*mut=*/true, v, line, col,
                               /*is_init=*/true);
      });
}

// Consumes the caller's +1 to the key on the refcounted (sidecar)
// path so a Tuple-keyed `obj[k]` read does not leak. TAG_STRING keys
// are non-refcounted (just borrowed cstrings), so they need no release.
// `obj[key]` fallback to a user-defined `__index__(key)`. Returns true and
// writes the (+1 owned) result to out when the method exists, else false.
inline bool _jit_try_object_index(JitObject* obj, int8_t key_tag,
                                  int64_t key_data, int8_t* out_tag,
                                  int64_t* out_data, int64_t line,
                                  int64_t col) {
  // Subscript overloading is a class-instance feature (matches interp's
  // class_tag gate); a plain dict never routes to __index__.
  if (!obj->proto()) return false;
  culebra_runtime_set_op_pos(line, col);
  auto r = _try_special_binop(TAG_OBJECT, reinterpret_cast<int64_t>(obj),
                              key_tag, key_data, Special::Index);
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

// The slot a String key's name finds, as a +1 value.
inline bool _jit_named_slot(JitObject* obj, std::string_view name,
                            int8_t* out_tag, int64_t* out_data) {
  auto idx = obj->find_slot(name);
  if (idx == static_cast<size_t>(-1)) return false;
  *out_tag = obj->slots[idx].value.tag;
  *out_data = obj->slots[idx].value.data;
  culebra_runtime_value_retain(*out_tag, *out_data);
  return true;
}

// Shared "own slot" lookup for a plain-dict receiver: String keys through
// the shape table, everything else through the non-string sidecar. Shared
// by object_get_any (throws KeyError on a miss) and object_get_for_coalesce
// (`??=`, reports a miss as found=false instead) — same slot-lookup logic,
// different miss policy. Consumes a non-String key's +1 on a hit; leaves it
// untouched on a miss so each caller makes its own miss-path decision
// (__index__ fallback, throw, or "not found") and handles the key itself. A
// key that cannot be hashed is a miss, and `*unhashable` (nil otherwise)
// names why, so the caller can raise once a class's `__index__` declines it.
// `name` is the key's `_jit_key_name`.
inline bool _jit_try_own_slot(JitObject* obj, const char* name, int8_t key_tag,
                              int64_t key_data, int8_t* out_tag,
                              int64_t* out_data, JitValue* unhashable) {
  if (name) return _jit_named_slot(obj, name, out_tag, out_data);
  auto it = _jit_sidecar_probe(obj, {key_tag, key_data}, unhashable);
  if (!it) return false;
  *out_tag = (*it)->second.value.tag;
  *out_data = (*it)->second.value.data;
  culebra_runtime_value_retain(*out_tag, *out_data);
  _culebra_value_release_impl(key_tag, key_data);
  return true;
}

// `obj[key]` for a String key a caller holds as its name and no value (a
// host): the entry, +1, when a slot of the receiver's own answers. False
// where the key goes on as a value, so the caller has to make it one: past a
// miss (a class's `__index__` receives it), or for a receiver that reads
// subscripts its own way.
inline bool culebra_runtime_object_get_named(JitObject* obj,
                                             std::string_view name,
                                             int8_t* out_tag,
                                             int64_t* out_data) {
  return _jit_keyed_by_entries(obj) &&
         _jit_named_slot(obj, name, out_tag, out_data);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get_any(
    JitObject* obj, int8_t key_tag, int64_t key_data,
    int8_t* out_tag, int64_t* out_data, int64_t line, int64_t col,
    bool own_receiver) {
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
  if (!_jit_keyed_by_entries(obj)) {
    // `arr[i]` on an array view: read element i from the inline bytes.
    if (auto* view = _jit_array_view(obj)) {
      int64_t i = _jit_view_index(key_tag, key_data, line, col);
      JitValue r = _jit_fa_get(*view, i, line, col);
      *out_tag = r.tag;
      *out_data = r.data;
      return;
    }
    // `rec["x"]` on a record view: its field `x` (+1).
    if (auto* view = _jit_record_view(obj)) {
      std::string kbuf;
      if (const char* name = _jit_key_name(key_tag, key_data, kbuf)) {
        JitValue r = _jit_packed_view_get(*view, name, line, col);
        culebra_runtime_value_retain(r.tag, r.data);
        if (key_tag != TAG_STRING)
          _culebra_value_release_impl(key_tag, key_data);
        *out_tag = r.tag;
        *out_data = r.data;
        return;
      }
    }
    // `buf[i]` on a SharedBuffer: hand back a packed view over element `i`
    // (the index coerces Long/Float like the interp's `key.to_long()`).
    if (_jit_is_shared_buffer(obj)) {
      int64_t idx = _jit_view_index(key_tag, key_data, line, col);
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
      // _jit_shared_val_child hands back a container sub-view BORROWED (+0;
      // the parent view's memo owns the +1) or a fresh primitive leaf. Retain
      // so this helper is uniformly +1-owned like every other branch — index
      // callers release the receiver without a promotion retain, and a
      // borrowed sub-view would otherwise be freed by that release while the
      // parent still points at it (a use-after-free the memo's next reader
      // would hit).
      culebra_runtime_value_retain(r.tag, r.data);
      *out_tag = r.tag;
      *out_data = r.data;
      return;
    }
    // `m[i]` / `m["name"]` on a Regex match: index its capture groups (Long
    // -> positional, String/StringView -> named). A miss is nil. Record
    // fields (`m.value`, spans) stay on dot access.
    if (obj->is_match) {
      JitValue r = _jit_match_index(obj, key_tag, key_data);
      *out_tag = r.tag;
      *out_data = r.data;
      if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
      return;
    }
  }
  // Slot hit: String keys are unified with shape access (see object_set_any);
  // other keys live in the non-String sidecar, whose probe hashes the key.
  std::string _kbuf;
  const char* name = _jit_key_name(key_tag, key_data, _kbuf);
  if (!name) culebra_runtime_set_op_pos(line, col);
  JitValue unhashable{TAG_NIL, 0};
  if (_jit_try_own_slot(obj, name, key_tag, key_data, out_tag, out_data,
                        &unhashable))
    return;
  // Miss → user `__index__` overload.
  if (_jit_try_object_index(obj, key_tag, key_data, out_tag, out_data, line,
                            col)) {
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    return;
  }
  // A key that could never be stored is a TypeError, not a missing key.
  if (unhashable.tag != TAG_NIL) _jit_throw_unhashable(unhashable);
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
// a BoundedArray view / SharedBuffer / Shared.new view (those reject `??=`
// before reaching here, matching the interp).
// Consumes the caller's +1 to a non-String key on every exit; a TAG_STRING
// key is a borrowed cstring (never released). The receiver is borrowed.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_get_for_coalesce(
    JitObject* obj, int8_t key_tag, int64_t key_data,
    int8_t* out_tag, int64_t* out_data, int64_t line, int64_t col) {
  std::string _kbuf;
  const char* name = _jit_key_name(key_tag, key_data, _kbuf);
  const JitValue key_guard = key_tag != TAG_STRING
      ? JitValue{key_tag, key_data}
      : JitValue{TAG_NIL, 0};
  JitUnwindRelease g{key_guard};
  if (!name) culebra_runtime_set_op_pos(line, col);
  JitValue unhashable{TAG_NIL, 0};
  if (_jit_try_own_slot(obj, name, key_tag, key_data, out_tag, out_data,
                        &unhashable))
    return true;
  // Miss → user `__index__` overload, else "not found" (nil for `??=`).
  if (_jit_try_object_index(obj, key_tag, key_data, out_tag, out_data, line,
                            col)) {
    if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
    return true;
  }
  if (unhashable.tag != TAG_NIL) _jit_throw_unhashable(unhashable);  // as object_get_any's miss
  if (key_tag != TAG_STRING) _culebra_value_release_impl(key_tag, key_data);
  // Caller loads *out_tag/*out_data unconditionally before checking the
  // return value — always leave them well-defined.
  *out_tag = TAG_NIL;
  *out_data = 0;
  return false;
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
//
// Both obligations key off the *name*, which every caller spells as a
// compile-time literal, so `key_kind` carries the two answers as constants
// (bit 0: is_well_known_prop, bit 1: the name is `drop`) instead of
// re-deriving them per write. The write path is hot — a `self.x = v` in a
// loop reached this function twice through `strlen` on a string the emitter
// already knew.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set_fast(
    JitObject* obj, const char* key, JitPropSetIC* ic, int8_t tag,
    int64_t data, int64_t line, int64_t col, bool mut, bool is_init,
    int8_t key_kind) {
  if (ic->expected_shape == ic->result_shape) {
    if (!_jit_tag_fits_field_type(
            tag, static_cast<culebra::FieldType>(ic->declared)))
      _jit_field_type_error(key, static_cast<culebra::FieldType>(ic->declared),
                            tag, data, line, col);
    _jit_overwrite_slot(obj->slots[ic->offset], key, tag, data, mut, is_init,
                        line, col, /*check_wk=*/(key_kind & 1) != 0);
  } else {
    _jit_reject_value_add(obj, key, tag, data, line, col, is_init);
    if (key_kind & 1) _culebra_check_well_known_prop(key, tag, data);
    if (obj->slots.capacity() == 0) obj->slots.reserve(8);
    obj->slots.push_back({JitValue{tag, data}, ic->prop_mut != 0});
    obj->shape = static_cast<culebra::Shape*>(ic->result_shape);
    if (obj->key_order)
      obj->key_order->push_back(
          JitObject::name_key(obj->prop_name(obj->prop_size() - 1)));
  }
  if (key_kind & 2) _jit_owned_bind_drop(obj);
}

// object_set_fast's update alone, for a site owing neither contract check:
// true after the store (its +1 absorbed), false and nothing touched for a
// transition, a refused tag or an immutable slot — the caller then takes
// object_set_fast, which words the error. The shape test comes first: a
// transition's offset is past the end of slots. Never throws.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_set_update(
    JitObject* obj, JitPropSetIC* ic, int8_t tag, int64_t data) {
  if (ic->expected_shape != ic->result_shape) return false;
  if (!_jit_tag_fits_field_type(tag,
                                static_cast<culebra::FieldType>(ic->declared)))
    return false;
  auto& e = obj->slots[ic->offset];
  if (!e.mut) return false;
  _jit_replace_value(e.value, tag, data);
  return true;
}

// One property of an object literal whose whole key set is static
// (ObjectNewShaped): the slot is already there, at an index the compiler
// resolved from the same key list, so the store is an indexed overwrite with
// object_set's is_init semantics — last-wins on a duplicate key, the
// well-known contract on the four protocol names, `drop` registration —
// without the name lookup. `key_kind` carries the two name-keyed answers
// (culebra::prop_key_kind), as object_set_fast's does. Consumes the value's
// +1 on every exit, the contract throw included.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_slot_init(
    JitObject* obj, int64_t idx, const char* key, bool mut, int8_t tag,
    int64_t data, int8_t key_kind) {
  if (key_kind & 1) _culebra_check_well_known_prop(key, tag, data);
  auto& e = obj->slots[static_cast<size_t>(idx)];
  e.mut = mut;
  _jit_replace_value(e.value, tag, data);
  if (key_kind & 2) _jit_owned_bind_drop(obj);
}

// The declared fields of a class whose declarations carry no initializer,
// put on a fresh instance in one step — each holding its type's zero,
// mutable, in declaration order, exactly what the run of per-field
// `self.x = zero` stores in `new`'s prologue produced, one shape
// transition at a time. The site caches the two shapes it moves between (a
// fresh instance's, with its `class` slot alone, and the one the field list
// transitions it to); an instance arriving with any other shape, or with
// any state the fast path does not reproduce, takes the per-field stores.
// `zeros[i]` is `keys[i]`'s value: a scalar, or the borrowed `''` literal.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_fields_init(
    JitObject* obj, void** shape_cache, const char* const* keys,
    int64_t n_keys, const JitValue* zeros, const uint8_t* types) {
  auto* from = static_cast<culebra::Shape*>(shape_cache[0]);
  if (from && obj->shape == from && !obj->key_order && !obj->frozen) {
    obj->slots.reserve(obj->slots.size() + static_cast<size_t>(n_keys));
    for (int64_t i = 0; i < n_keys; i++)
      obj->slots.push_back({zeros[i], /*mut=*/true});
    obj->shape = static_cast<culebra::Shape*>(shape_cache[1]);
    obj->mut_count += n_keys;
    return;
  }
  auto* before = obj->shape;
  size_t n_before = obj->slots.size();
  for (int64_t i = 0; i < n_keys; i++)
    _jit_object_set_declared(obj, keys[i], /*mut=*/true,
                             static_cast<int8_t>(zeros[i].tag), zeros[i].data,
                             0, 0, /*is_init=*/true,
                             static_cast<culebra::FieldType>(types[i]));
  // Prime the cache only from a run the fast path reproduces exactly: every
  // key appended (none existed, so no overwrite), still shaped, and no
  // insertion-order sidecar to keep in step.
  if (!obj->is_dict && !obj->key_order && before &&
      obj->slots.size() == n_before + static_cast<size_t>(n_keys)) {
    shape_cache[0] = before;
    shape_cache[1] = obj->shape;
  }
}

// The receivers a property write never stores a slot on. `view.field = v`
// on a @packable packed view writes straight into the shared backing bytes
// (zero copy); a Shared.new view is immutable on every write surface; a
// JitHandle has no fields. No name ever becomes an own slot, so the write
// always reaches an uncached path — the IC below stays cold for all three,
// and the bytecode VM, which keeps no per-site cache at all, comes through
// the entry after it. Returns true when
// the write was handled here. Matches the interp's packed_view_set arm.
inline bool _jit_prop_write_intercepted(JitObject* obj, const char* key,
                                        int8_t tag, int64_t data,
                                        int64_t line, int64_t col,
                                        int64_t mem_line, int64_t mem_col) {
  if (auto* view = _jit_record_view(obj)) {
    // A packed field's own errors report at the member (the interp's
    // packed_view_set takes the DOT node's position); everything else on a
    // property write reports at the statement.
    _jit_packed_view_set(*view, key, tag, data, mem_line ? mem_line : line,
                         mem_line ? mem_col : col);
    return true;
  }
  if (obj->is_shared_val) {
    _culebra_value_release_impl(tag, data);
    throw culebra::CulebraError("ImmutableError",
                                "Shared values are immutable", line, col);
  }
  if (obj->has_handle) _jit_handle_reject_write(obj, tag, data, line, col);
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
    // `append_slot` owns the shape transition, and the flip into dictionary
    // mode past `kDictAt` properties.
    idx = obj->append_slot(key, JitValue{tag, data}, mut);
    // A dictionary shares one sentinel shape with every other dictionary, so
    // priming here would let this site's cache match a different object and
    // read its slot `idx`. Leave the cache cold; a dictionary write is a
    // hash insert either way.
    if (!obj->is_dict) {
      ic->expected_shape = before;
      ic->result_shape = obj->shape;
      ic->offset = idx;
      ic->prop_mut = mut ? 1 : 0;
      ic->declared = static_cast<uint8_t>(culebra::FieldType::Any);
    }
  } else {
    auto declared = before ? before->type_at(idx) : culebra::FieldType::Any;
    if (!_jit_tag_fits_field_type(tag, declared))
      _jit_field_type_error(key, declared, tag, data, line, col);
    _jit_overwrite_slot(obj->slots[idx], key, tag, data, mut, is_init, line,
                        col, /*check_wk=*/true);
    if (!obj->is_dict) {
      ic->expected_shape = before;
      ic->result_shape = before;
      ic->offset = idx;
      ic->prop_mut = obj->slots[idx].mut ? 1 : 0;
      ic->declared = static_cast<uint8_t>(declared);
    }
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
// `is_throw=0` so the caller propagates it with __cxa_rethrow. An error the
// handler before this one passed on arrives with the Object that handler saw
// (_PassedError), and that is the Object this one receives.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_try_translate() {
  auto& rt = culebra::current_runtime();
  if (rt.is_throw) {  // user throw already carries a Value
    rt.caught_engine_error = 0;
    rt.pending_passed = 0;  // a throw that replaced the error passed on
    return;
  }
  if (!rt.pending_error) return;  // foreign exception — the pad rethrows it
  auto& passed = _passed_error(rt);
  auto* obj = rt.pending_passed ? passed.take() : nullptr;
  if (!obj) {
    // A positionless runtime error (line/col 0) adopts the last published op
    // position, matching the old _jit_backfill_op_pos path.
    int64_t line = rt.pending_line, col = rt.pending_col;
    if (line == 0 && col == 0) {
      line = _jit_thread.op_line;
      col = _jit_thread.op_col;
    }
    obj = culebra_runtime_object_new();
    culebra_runtime_object_set(
        obj, "kind", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(rt.pending_kind)), 0, 0);
    culebra_runtime_object_set(
        obj, "message", false, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(rt.pending_msg)), 0, 0);
    culebra_runtime_object_set(obj, "line", false, TAG_LONG, line, 0, 0);
    culebra_runtime_object_set(obj, "col", false, TAG_LONG, col, 0, 0);
    obj->is_error = true;
  }
  rt.thrown_tag = TAG_OBJECT;
  rt.thrown_data = reinterpret_cast<int64_t>(obj);
  rt.is_throw = 1;
  rt.pending_error = 0;  // consumed
  rt.pending_passed = 0;
  // An Object left by an error that ended elsewhere goes now, the carriers
  // settled: its release can reach a `drop`, which saves and restores them.
  // How this error arrived is written after it — a handler in that `drop`
  // writes it too, and nothing restores it.
  passed.reset();
  rt.caught_engine_error = 1;
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

// What a name finds on a Shared view, asked whether it is there (a pattern, a
// host's `has`): a method off the meta every view shares, or the tree's entry
// under that key, borrowed. The view's own slots are its bookkeeping, and a
// dropped view has no entries left to find.
inline bool _jit_shared_view_find(JitObject* view, std::string_view key,
                                  JitValue* out) {
  if (auto* meta = view->proto()) {
    auto idx = meta->find_slot(key);
    if (idx != static_cast<size_t>(-1)) {
      *out = meta->slots[idx].value;
      return true;
    }
  }
  if (_jit_shared_val_dropped(view)) return false;
  return culebra::_jit_shared_val_find(view, key, out, 0, 0);
}

// `view.name`: a method off the meta, or the tree's entry (nil when it has
// none). A method fills the cache's proto half as a class instance's does, so
// the site's own code answers the next read of it.
inline JitValue _jit_shared_view_get(JitObject* view, std::string_view key,
                                     JitPropIC* ic, int64_t line,
                                     int64_t col) {
  auto* meta = view->proto();
  auto idx = meta->find_slot(key);
  if (idx != static_cast<size_t>(-1)) {
    ic->owner_shape = view->shape;
    ic->proto_shape = meta->shape;
    ic->proto_offset = idx;
    return meta->slots[idx].value;
  }
  // +0-borrowed: a string leaf is a fresh heap string (leak-bounded by design
  // like every JIT string), a container child is a sub-view memoized on the
  // parent view (see _jit_shared_val_child) and released on the parent's
  // teardown.
  return culebra::_jit_shared_val_prop(view, key, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_get(
    JitObject* obj, const char* key, int8_t* out_tag, int64_t* out_data) {
  if (obj->is_shared_val) {
    JitValue v{TAG_NIL, 0};
    _jit_shared_view_find(obj, key, &v);
    *out_tag = static_cast<int8_t>(v.tag);
    *out_data = v.data;
    return;
  }
  _write_value_out(_find_property(obj, key), out_tag, out_data);
}

// Slow path for the per-callsite IC emitted by emit_property_get.
// On an own-property hit, refreshes `ic->shape` / `ic->offset` so the
// next read with the same shape stays on the inlined fast path. Proto
// hits don't update the cache because the fast path keys on
// `obj->shape`; caching the proto's offset there would load the wrong
// slot. `ic` is borrowed; never released.
// Forward declarations for the trait-default table referenced below.
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, JitClosure*>>&
_jit_trait_default_impls();
inline JitClosure* _jit_find_shipped_default(JitObject* obj, const char* key,
                                             const std::string* found);

// Two traits that both supply this instance a default `key` leave it
// ambiguous, as two equally specific methods do; the class settles it by
// defining `key` itself. Worded the same whichever trait is met first.
[[noreturn]] inline void _jit_trait_default_ambiguous(const std::string& a,
                                                      const std::string& b,
                                                      const char* key) {
  const auto& [first, second] = std::minmax(a, b);
  throw culebra::CulebraError(
      "DispatchError",
      culebra::format("ambiguous default `{}`: traits {} and {} both supply "
                      "it; define `{}` on the class",
                      key, first, second, key),
      0, 0);
}

// The registered default named `key` whose trait this instance conforms to,
// or null — in a worker Runtime, one its parent shipped is installed here on
// its first miss. Conformance is cached inside _culebra_type_matches_single.
// One source for the three askers: the property read, the UFCS gate, and the
// `__call__` lookup — each keeps its own gate on what may ask.
inline JitClosure* _jit_find_trait_default(JitObject* obj, const char* key) {
  JitClosure* found = nullptr;
  const std::string* from = nullptr;
  for (auto& [trait_name, methods] : _jit_trait_default_impls()) {
    auto m_it = methods.find(key);
    if (m_it == methods.end() || !m_it->second) continue;
    if (!_culebra_type_matches_single(TAG_OBJECT,
                                      reinterpret_cast<int64_t>(obj),
                                      trait_name))
      continue;
    if (from) _jit_trait_default_ambiguous(*from, trait_name, key);
    found = m_it->second;
    from = &trait_name;
  }
  if (auto* shipped = _jit_find_shipped_default(obj, key, from))
    return shipped;
  return found;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_object_get_ic(
    JitObject* obj, const char* key, JitPropIC* ic, int64_t line,
    int64_t col) {
  // @packable handles: a packed view's `.field` reads the backing bytes
  // (zero copy); a buffer's `.size`/`.count`/`.len` reports its length.
  // Borrowed like any other read: a heap value the view mints is kept on it
  // (JitBufView::minted). (Always reaches the slow path: these names are never
  // own slots, so the IC stays cold.)
  if (auto* view = _jit_record_view(obj))
    return _jit_packed_view_get(*view, key, line, col);
  if (_jit_is_shared_buffer(obj)) {
    std::string_view k(key);
    if (k == "size" || k == "count" || k == "len")
      return {TAG_LONG, _jit_shared_buf(obj)->count};
  }
  // A Shared view's own slots are the handle's bookkeeping and answer for no
  // name.
  if (obj->is_shared_val) return _jit_shared_view_get(obj, key, ic, line, col);
  // Cached proto hit: this receiver shape's own scan missed last time and the
  // proto resolved `key` at proto_offset, and neither shape has changed since
  // — so both linear scans are already answered. This is the shape a class
  // instance's every method read has (data in own slots, methods on the
  // shared meta behind `proto`).
  if (obj->shape == ic->owner_shape && obj->proto() &&
      obj->proto()->shape == ic->proto_shape) {
    return obj->proto()->slots[ic->proto_offset].value;
  }
  if (obj->shape) {
    auto idx = obj->find_slot(key);
    if (idx != static_cast<size_t>(-1)) {
      // Same reason as the write cache: every dictionary carries the same
      // sentinel shape, so it must never prime a per-site cache.
      if (!obj->is_dict) {
        ic->shape = obj->shape;
        ic->offset = idx;
      }
      return obj->slots[idx].value;
    }
  }
  if (obj->proto()) {
    // The proto's own slot — _find_property's own first step, so the
    // resolution order is unchanged; only this hit is cacheable, and a name
    // it answers from a second level falls through to the walk below.
    auto idx = obj->proto()->find_slot(key);
    if (idx != static_cast<size_t>(-1)) {
      ic->owner_shape = obj->shape;
      ic->proto_shape = obj->proto()->shape;
      ic->proto_offset = idx;
      return obj->proto()->slots[idx].value;
    }
    if (auto* proto_entry = _find_property(obj->proto(), key))
      return proto_entry->value;
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
  if (auto* view = _jit_record_view(obj)) {
    view->core();  // a dropped buffer's view answers nothing
    return view->layout->find(key) != nullptr;
  }
  if (obj->is_shared_val) {
    JitValue found;
    return _jit_shared_view_find(obj, key, &found);
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
  if (!obj->proto()) return false;
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
  if (!obj->proto()) return {TAG_NIL, 0};
  if (auto* e = _find_property(obj, "__call__")) {
    if (e->value.tag == TAG_FUNC) return e->value;
  }
  if (auto* d = _jit_find_trait_default(obj, "__call__"))
    return {TAG_FUNC, reinterpret_cast<int64_t>(d)};
  return {TAG_NIL, 0};
}

// Flag a freshly built class namespace object as callable — `C(args)`
// dispatches to its `new` (see culebra_runtime_class_new_method) — and hand
// it the meta its instances share, absorbing that +1 (JitObject::
// class_meta_of). Emitted by compile_class_decl.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_mark_class(
    JitObject* o, JitObject* meta) {
  // The trailing slot is the meta's from here on (JitObject's union).
  assert(!o->is_namespace && !o->is_enum && !o->proto());
  o->is_class = true;
  o->instance_meta = meta;
}

// `C(args)` construction: returns the class object's `new` constructor as a
// borrowed TAG_FUNC JitValue, or Nil for a value that has none. Gated on the
// two flags the engine sets itself — `is_class` (CLASS_DECL, and the wrapped
// native classes bindings.h marks) and `is_namespace` (a builtin namespace
// whose own `new` is a factory: Channel, SharedBuffer, Shared) — so a plain
// dict holding a "new" key stays non-callable, which is what the gate is
// for. A namespace without a `new` finds none below and raises the same
// TypeError as before.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_class_new_method(
    int8_t tag, int64_t data) {
  if (tag != TAG_OBJECT) return {TAG_NIL, 0};
  auto* obj = reinterpret_cast<JitObject*>(data);
  if (!obj->is_class && !obj->is_namespace) return {TAG_NIL, 0};
  if (auto* e = _find_property(obj, "new")) {
    if (e->value.tag == TAG_FUNC) return e->value;
  }
  return {TAG_NIL, 0};
}

// The name `type_of` answers: the class that built the value, `Class` for a
// class object, else the tag's.
inline const char* _culebra_type_of_name(int8_t tag, int64_t data) {
  if (tag == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(data);
    if (const char* name = _jit_meta_class_name(o)) return name;
    if (o->is_class) return "Class";
  }
  return _culebra_tag_name(tag);
}

// A method call's callee (`recv.name(...)`) is not a Function. The same
// TypeError a plain call raises, and when the member was simply not there
// (a missing property reads as nil) the name and what it was asked of.
[[noreturn]] CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_method_not_callable(int64_t line, int64_t col,
                                    int8_t callee_tag, const char* name,
                                    int8_t recv_tag, int64_t recv_data) {
  // A lowering's state object keeps its body's locals as own slots, so a bare
  // `f()` there arrives as a method call on it (culebra_runtime_call_receiver,
  // which has already answered when the receiver is TAG_NO_SELF): a plain
  // call, with no member to name.
  auto promoted_local = [&] {
    if (recv_tag == TAG_NO_SELF) return true;
    if (recv_tag != TAG_OBJECT) return false;
    auto* o = reinterpret_cast<JitObject*>(recv_data);
    return o->proto() && o->proto()->is_lowered_state && o->has_own(name);
  };
  if (callee_tag != TAG_NIL || !name || promoted_local())
    culebra_runtime_type_error_typed(line, col, "Function", callee_tag);
  throw culebra::CulebraError(
      "TypeError",
      culebra::type_mismatch_message("Function", "Nil") +
          culebra::missing_method_hint(
              name, _culebra_type_of_name(recv_tag, recv_data)),
      line, col);
}

// `match v { x: ClassName => ... }` predicate. Returns true when `obj` was
// built by a class of that name — read from its meta, so an object cannot
// answer to a name by carrying one. Mirrors `culebra::class_tag()` + name
// comparison in the interp's `type_matches`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_class_matches(
    JitObject* obj, const char* expected) {
  const char* cls = _jit_meta_class_name(obj);
  return cls && expected && std::strcmp(cls, expected) == 0;
}

// Does `obj` have a nominal identity — is it a class-sugar instance, an enum
// variant, or one of the values the runtime builds with a meta of its own?
// The question three sites used to ask as `has("class")`, back when carrying
// the key was the same thing as being one.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_object_is_instance(
    JitObject* obj) {
  return _jit_meta_class_name(obj) != nullptr;
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
  if (const char* name = _jit_key_name(tag, data, _kbuf))
    return _find_property(obj, name) != nullptr;
  // Hashing the key is where an unhashable one raises; the key is this
  // call's to release on that exit as on the normal one.
  JitUnwindRelease key_guard{JitValue{tag, data}};
  bool result = _jit_sidecar_find(obj, {tag, data}).has_value();
  _culebra_value_release_impl(tag, data);
  return result;
}

// A meta's method set is fixed once its properties are set, so the special
// table (an operator or protocol resolved once, not walked per call — see
// JitSpecialTable) and the `drop` gate are cached here. Shared by a
// freshly-declared meta (culebra_runtime_build_class_meta) and one rebuilt
// from a deserialized Object (jit_deserialize, conc/sendable.h), whose
// properties are already in place by the time this runs.
inline void _jit_fill_specials(JitObject* meta) {
  for (size_t s = 0; s < static_cast<size_t>(Special::Count); s++)
    meta->specials->fn[s] = _special_closure(meta, static_cast<Special>(s));
  // An `eq` that fills no slot is a derived one: the class matches by fields.
  meta->specials->eq_by_fields =
      _protocol_member(meta, special_name(Special::EqTrait)) &&
      !meta->specials->fn[static_cast<size_t>(Special::EqTrait)];
  meta->methods_drop = _protocol_member(meta, "drop") != nullptr;
}

// Build a "class meta" object that holds shared method closures for
// proto delegation, plus the class's name. Called once per class
// declaration (compile-time emission, runtime allocation), captured in the
// constructor closure so each instance can point its `proto` at the same
// meta — which is what makes the name reachable from any instance without
// an own slot of its own.
//
// The caller hands each method value +1-owned and does not use it again, so
// their references are *transferred* into the meta: object_set stores without
// retaining, and the meta's own destructor releases them. (A stray retain
// here left the caller's original +1 unreleased — a class declared inside a
// function leaked one closure per method on every call.)
// `lowered_state` flags a class a lowering synthesized (an effects
// state machine), so a value read of one of its instances' own slots stays
// unbound — see culebra_runtime_bind_method_value and the interp setting
// OrderedSymbolMap::lowered_state on the same object.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_build_class_meta(
    const char* class_name, const char* const* method_names,
    const JitValue* method_vals, int64_t n_methods, int64_t flags) {
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
  // The method set is fixed from here on, so the two questions every
  // construction and every operator ask of it are answered once.
  meta->is_class_meta = true;
  meta->specials = new JitSpecialTable();
  // Interned, not borrowed: a meta outlives the chunk or module global the
  // caller's `class_name` points into (a REPL session frees chunks while
  // instances built from them are still bound).
  meta->specials->name = _intern_str(std::string_view(class_name));
  _jit_fill_specials(meta);
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
//   2. Wire the instance's `proto` at the shared class meta object, which is
//      where its class name lives;
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
// A fresh instance of the class whose meta is `class_meta` (built by the
// class object `cls`, or null), with no field yet: its proto wired, a +1 on
// the meta and on the class — released in the JitObject destructor — and a
// `drop` registered on the owned stack. What every way of making an instance
// starts from, `new` or not.
inline JitObject* _jit_class_instance_alloc(JitObject* class_meta,
                                            JitObject* cls) {
  auto* inst = culebra_runtime_object_new();
  // A `@value` instance's field set is closed from here on, not from the
  // freeze at the end: the declared stores that follow are `is_init` and no
  // user write is, so a constructor cannot slip in a field its siblings lack —
  // through a computed key or an alias any more than through `self.z = v`,
  // which the declaration refuses outright.
  inst->fields_closed = class_meta && class_meta->is_value;
  inst->set_proto(class_meta);
  if (class_meta) class_meta->refcount++;
  if (cls) {
    inst->cls = cls;
    cls->refcount++;
  }
  // Mirror inherited `drop` so the destructor's `has_drop` gate fires.
  if (class_meta && class_meta->methods_drop) _jit_owned_bind_drop(inst);
  return inst;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_build_class_instance(
    const char* class_name, JitObject* class_meta, int8_t cls_tag,
    int64_t cls_data, int8_t finit_tag, int64_t finit_data, int8_t body_tag,
    int64_t body_data, int64_t n_args, JitValue* args) {
  auto* inst = _jit_class_instance_alloc(
      class_meta, cls_tag == TAG_OBJECT &&
                          reinterpret_cast<JitObject*>(cls_data)->is_class
                      ? reinterpret_cast<JitObject*>(cls_data)
                      : nullptr);

  // The instance's own slots are its fields and nothing else: its name is
  // on the meta above, reached through `proto`. (`class_name` is still
  // taken, for the meta's benefit on the paths that build one lazily.)

  JitValue self_val = {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
  // Each _jit_invoke consumes one retained `self` ref (the callee's slot
  // release); the instance's original +1 is released on the unwind edge of
  // either call. A catch, not an uncaught_exceptions() probe: the probe
  // reads thread-local state on every construction, the catch costs the
  // normal path nothing.
  if (finit_tag == TAG_FUNC) {
    auto* finit_cls = reinterpret_cast<JitClosure*>(finit_data);
    culebra_runtime_value_retain(self_val.tag, self_val.data);
    try {
      // Hand the `new` body the constructor's call site and argument positions.
      JitBorrowedCallSite site{_jit_thread.call_line, _jit_thread.call_col};
      auto r = _jit_invoke(finit_cls, self_val, 0, nullptr);
      _culebra_value_release_impl(r.tag, r.data);
    } catch (...) {
      // A field-init throw unwinds before anything has consumed the
      // caller-transferred arg +1s (the `new` body normally absorbs them,
      // but it never runs) — release them on this edge only.
      for (int64_t i = 0; i < n_args; i++)
        culebra_runtime_value_release(args[i].tag, args[i].data);
      culebra_runtime_value_release(self_val.tag, self_val.data);
      throw;
    }
  }

  if (body_tag == TAG_FUNC) {
    auto* body_cls = reinterpret_cast<JitClosure*>(body_data);
    // Body consumes `self` via the slot +1 on function exit, so retain
    // before handing it off. If the body throws, the instance's +1 is
    // otherwise stranded — released on the unwind edge.
    culebra_runtime_value_retain(self_val.tag, self_val.data);
    try {
      auto result = _jit_invoke(body_cls, self_val, n_args, args);
      _culebra_value_release_impl(result.tag, result.data);
    } catch (...) {
      culebra_runtime_value_release(self_val.tag, self_val.data);
      throw;
    }
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

// A run's fields — already-computed scalars, none of them refcounted (a
// flat `@value` layout admits only Long/Float/Bool, `is_flat_value_field_type`
// in parser.h) — materialized into a fresh, frozen instance. No `new`, no
// field-init, nothing to run: the inverse of unboxing (spec §15.8,
// "reboxing at boundaries"), for a consumer Compiler::materialize_run's
// caller could not classify as run-native. The run itself is a snapshot
// read here, not a consuming one — its own slots are untouched, so the
// binding keeps running afterward.
//
// `field_values[i]` supplies `keys[i]`'s slot, in the SAME declaration order
// the flat layout unboxed it in — so this instance's Shape, `.keys()` order
// and structural equality are identical to one a boxed `new` of the same
// class would have built. Building the slot list directly means there is no
// add-refused ordering to get backwards: nothing here ever asks whether the
// field set is closed, because nothing here is ever an append.
//
// `cls` is the class object (borrowed), which the instance is built by like
// one `new` made: a member reads its class's name off its receiver
// (culebra_runtime_class_self), reboxed or not.
//
// The Shape cache (`*shape_cache`) is the same per-callsite one
// ObjectNewShaped's runtime half uses — see `_jit_resolve_cached_shape`
// (rt_runtime.inc.h) for why the laziness is load-bearing for AOT.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_materialize_value(
    void** shape_cache, const char* const* keys, int64_t n_keys,
    JitObject* cls, const JitValue* field_values) {
  auto* shape = _jit_resolve_cached_shape(shape_cache, keys, n_keys);
  assert(cls->class_meta_of() && "a run is reboxed by its declared class");
  auto* inst = _jit_class_instance_alloc(cls->class_meta_of(), cls);
  inst->shape = shape;
  inst->slots.reserve(static_cast<size_t>(n_keys));
  for (int64_t i = 0; i < n_keys; i++)
    inst->slots.push_back(JitObjectEntry{field_values[i], /*mut=*/false});
  inst->fields_closed = true;
  inst->frozen = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
}

// --- @packable records as byte streams (String.pack / String.unpack) ------
// packable.h decides the form (declaration order, no padding, one byte order
// per call); these move a class-sugar instance's fields to and from it.

// The @packable class name a value's `__packable__` marker carries — the
// class object SharedBuffer takes — or empty for any other.
inline std::string_view _jit_packable_class_name(JitValue v) {
  if (v.tag != TAG_OBJECT) return {};
  auto* cls = reinterpret_cast<JitObject*>(v.data);
  auto mi = cls->find_slot("__packable__");
  if (mi == static_cast<size_t>(-1) || cls->slots[mi].value.tag != TAG_STRING)
    return {};
  return _str_sv(reinterpret_cast<const char*>(cls->slots[mi].value.data));
}

// One fixed-width scalar between a value and the W-wide word at `p`: the one
// rule for what each kind accepts and yields, under String.pack's bulk loops
// and a record's fields alike. Silent: the caller words a refusal for what it
// was handed (an Array element, a field).
extern "C++" {
enum class JitScalarPut : uint8_t { Ok, WrongType, OutOfRange };
template <class W>
inline JitScalarPut _jit_scalar_put(uint8_t* p, culebra::scalar_bytes::Kind kind,
                                    int64_t lo, int64_t hi, const JitValue& v,
                                    bool big) {
  namespace sb = culebra::scalar_bytes;
  switch (kind) {
    case sb::Kind::Bool:
      if (v.tag != TAG_BOOL) return JitScalarPut::WrongType;
      *p = v.data ? 1 : 0;
      return JitScalarPut::Ok;
    case sb::Kind::Float: {
      double d;
      if (v.tag == TAG_FLOAT) d = _culebra_float_to_double(v.data);
      else if (v.tag == TAG_LONG) d = static_cast<double>(v.data);
      else return JitScalarPut::WrongType;
      if constexpr (sizeof(W) == 8)
        sb::store_as(p, std::bit_cast<uint64_t>(d), big);
      else if constexpr (sizeof(W) == 4)
        sb::store_as(p, std::bit_cast<uint32_t>(
                            static_cast<float>(sb::round_f32(d))), big);
      return JitScalarPut::Ok;
    }
    default:
      if (v.tag != TAG_LONG) return JitScalarPut::WrongType;
      if (v.data < lo || v.data > hi) return JitScalarPut::OutOfRange;
      sb::store_as(p, static_cast<W>(v.data), big);
      return JitScalarPut::Ok;
  }
}
template <class W>
inline JitValue _jit_scalar_get(const uint8_t* p, culebra::scalar_bytes::Kind kind,
                                bool big) {
  namespace sb = culebra::scalar_bytes;
  const W bits = sb::load_as<W>(p, big);
  switch (kind) {
    case sb::Kind::Bool: return {TAG_BOOL, bits != 0};
    case sb::Kind::Float:
      if constexpr (sizeof(W) == 8)
        return {TAG_FLOAT, _culebra_double_to_bits(std::bit_cast<double>(bits))};
      else if constexpr (sizeof(W) == 4)
        return {TAG_FLOAT, _culebra_double_to_bits(std::bit_cast<float>(bits))};
      else
        return {TAG_NIL, 0};  // no 1- or 2-byte float type
    case sb::Kind::Signed:
      return {TAG_LONG,
              static_cast<int64_t>(static_cast<std::make_signed_t<W>>(bits))};
    default: return {TAG_LONG, static_cast<int64_t>(bits)};
  }
}
}  // extern "C++"

// A record type String.pack/unpack was handed: the class object and what its
// declaration fixed (JitRecordInfo, on its meta). Null `cls`: not one.
struct JitRecordType {
  JitObject* cls = nullptr;
  const JitRecordInfo* info = nullptr;
  JitObject* meta() const { return cls->class_meta_of(); }
  const char* name() const { return meta()->specials->name; }
};
// A class with a field that has no byte-stream form is an error, named by `fn`.
inline JitRecordType _jit_record_type(JitValue v, std::string_view fn) {
  if (v.tag != TAG_OBJECT) return {};
  auto* cls = reinterpret_cast<JitObject*>(v.data);
  auto* meta = cls->class_meta_of();
  if (!meta || !meta->specials || !meta->specials->record) return {};
  JitRecordType rt{cls, meta->specials->record.get()};
  if (auto* bad = rt.info->no_wire) {
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("{}: {}.{} is a {}, which has no byte-stream form (a "
                        "scalar, Bytes<N>, BoundedString<N> or FixedArray<T, N> "
                        "field has).",
                        fn, rt.name(), bad->name, bad->type));
  }
  return rt;
}

// Record `rec`'s fields into the `rt.info->wire_size` bytes at `p`. `rec` must
// be an instance of the class itself — its meta, not a class of its name. A
// field left unset (a declared field with no initializer starts nil) writes
// its zero bytes, as in a fresh SharedBuffer record.
inline void _jit_record_write(uint8_t* p, JitValue rec, const JitRecordType& rt,
                              bool big, std::string_view fn, size_t i) {
  namespace sb = culebra::scalar_bytes;
  auto* o = rec.tag == TAG_OBJECT ? reinterpret_cast<JitObject*>(rec.data)
                                  : nullptr;
  if (!o || o->proto() != rt.meta()) {
    const char* got = o && _jit_meta_class_name(o) ? _jit_meta_class_name(o)
                                                   : _culebra_tag_name(rec.tag);
    throw culebra::CulebraError(
        "TypeError", culebra::format("{}: {} takes {} records, got {} at index {}.",
                                     fn, rt.name(), rt.name(), got, i));
  }
  const auto& fields = rt.info->layout.fields;
  // An instance with the shape `new` builds has field k in slot k.
  const bool in_order = o->shape == rt.info->shape;
  for (size_t k = 0; k < fields.size(); k++) {
    const auto& f = fields[k];
    const size_t si = in_order ? k : o->find_slot(f.name);
    JitValue v = si == static_cast<size_t>(-1) ? JitValue{TAG_NIL, 0}
                                               : o->slots[si].value;
    const size_t size = f.layout.wire_size;
    auto fail = [&](const char* kind, std::string_view detail) {
      throw culebra::CulebraError(
          kind, culebra::format("{}: record {}, field `{}`: {}", fn, i, f.name,
                                detail));
    };
    // One scalar of the field's wire type; `elem` names a FixedArray's element.
    auto put = [&](uint8_t* at, const JitValue& x, std::string_view type,
                   std::optional<size_t> elem) {
      const sb::Type& t = *f.layout.wire;
      const auto [lo, hi] = sb::int_range(t);
      auto r = sb::with_word(t.width, [&](auto w) {
        return _jit_scalar_put<decltype(w)>(at, t.kind, lo, hi, x, big);
      });
      if (r == JitScalarPut::Ok) return;
      const bool wrong = r == JitScalarPut::WrongType;
      std::string detail =
          wrong ? culebra::format("{} takes {}, got {}.", type,
                                  sb::value_kind_name(t.kind),
                                  _culebra_tag_name(x.tag))
                : culebra::format("{} is out of range for {} ({} to {}).",
                                  x.data, type, lo, hi);
      fail(wrong ? "TypeError" : "ValueError",
           elem ? culebra::format("element {}: {}", *elem, detail) : detail);
    };
    const bool is_scalar = f.layout.wire && !f.layout.is_fixed_array;
    if (v.tag == TAG_NIL && !is_scalar) {
      std::memset(p, 0, size);
    } else if (f.layout.is_fixed_array) {
      if (v.tag != TAG_ARRAY)
        fail("TypeError", culebra::format("{} takes an Array, got {}.", f.type,
                                          _culebra_tag_name(v.tag)));
      auto* arr = reinterpret_cast<JitArray*>(v.data);
      const size_t n = f.layout.capacity;
      if (arr->size != n)
        fail("ValueError", culebra::format("{} takes exactly {} elements, got {}.",
                                           f.type, n, arr->size));
      const size_t w = f.layout.wire->width;
      for (size_t e = 0; e < n; e++)
        put(p + e * w, arr->items[e], f.layout.elem_type, e);
    } else if (is_scalar) {
      put(p, v, f.type, std::nullopt);
    } else {  // Bytes<N> / BoundedString<N>
      if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
        fail("TypeError", culebra::format("{} takes a String, got {}.", f.type,
                                          _culebra_tag_name(v.tag)));
      auto s = _culebra_str_view(v.tag, v.data);
      if (f.layout.is_bytes && s.size() != size)
        fail("ValueError", culebra::format("{} takes exactly {} bytes, got {}.",
                                           f.type, size, s.size()));
      if (s.size() > size)
        fail("CapacityError", culebra::format("{} holds up to {} bytes, got {}.",
                                              f.type, size, s.size()));
      if (!s.empty()) std::memcpy(p, s.data(), s.size());
      std::memset(p + s.size(), 0, size - s.size());  // C's char[N]: NUL-padded
    }
    p += size;
  }
}

// A new instance holding the record at `p` — built without running `new`: a
// @packable record's whole state is its declared fields, and every one of
// them comes from the bytes. Those fields, in order, are the shape `new`
// builds, resolved once at the declaration, so the slots go in directly (as
// materialize_value's do). A BoundedString<N> reads up to its first NUL, C's
// rule for char[N]; a FixedArray<T, N> is an Array of its N elements.
inline JitValue _jit_record_read(const uint8_t* p, const JitRecordType& rt,
                                 bool big) {
  namespace sb = culebra::scalar_bytes;
  auto* inst = _jit_class_instance_alloc(rt.meta(), rt.cls);
  const auto& fields = rt.info->layout.fields;
  inst->shape = rt.info->shape;
  inst->slots.reserve(fields.size());
  for (const auto& f : fields) {
    const size_t size = f.layout.wire_size;
    auto get = [&](const uint8_t* at) {
      return sb::with_word(f.layout.wire->width, [&](auto w) {
        return _jit_scalar_get<decltype(w)>(at, f.layout.wire->kind, big);
      });
    };
    JitValue v;
    if (f.layout.is_fixed_array) {
      const size_t n = f.layout.capacity;
      const size_t w = f.layout.wire->width;
      auto* arr = _jit_array_new_reserved(static_cast<int64_t>(n), "FixedArray");
      for (size_t e = 0; e < n; e++) arr->items[e] = get(p + e * w);
      arr->size = n;
      v = {TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
    } else if (f.layout.wire) {
      v = get(p);
    } else {  // Bytes<N> / BoundedString<N>
      size_t n = size;
      if (f.layout.is_bounded_string) {
        const void* nul = std::memchr(p, 0, n);
        if (nul) n = static_cast<size_t>(static_cast<const uint8_t*>(nul) - p);
      }
      v = {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(
                           std::string_view(reinterpret_cast<const char*>(p), n)))};
    }
    inst->slots.push_back(JitObjectEntry{v, /*mut=*/true});
    p += size;
  }
  inst->mut_count += static_cast<int64_t>(fields.size());
  return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
}

// Build a range value `{start, end, inclusive, step}` carrying the shared
// Range meta. Each endpoint arrives tagged — Long or Float, already checked
// by the caller, or Nil for an open end; `step` is a Long, defaulting to 1.
// Returns a fresh +1 JitObject.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_make_range(
    int8_t start_tag, int64_t start, int8_t end_tag, int64_t end,
    int8_t inclusive, int64_t step) {
  auto* o = culebra_runtime_object_new();
  // Identity, not shape: every range shares one meta, so _jit_is_range_shaped
  // is a pointer compare and a dict wearing the same four keys is not a range.
  o->set_proto(_jit_range_meta());  // transferred
  culebra_runtime_object_set(o, "start", false, start_tag, start, 0, 0);
  culebra_runtime_object_set(o, "end", false, end_tag, end, 0, 0);
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
  if (_jit_meta_opaque(obj)) return 0;
  int64_t n = static_cast<int64_t>(obj->prop_size());
  if (obj->non_string_props) {
    n += static_cast<int64_t>(obj->non_string_props->size());
  }
  return n;
}

}  // extern "C" (block continues in rt_dispatch.inc.h)

