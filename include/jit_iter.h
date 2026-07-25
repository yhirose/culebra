#pragma once

#ifdef CULEBRA_JIT_ENABLED

// Iterator protocol runtime: terminal / lazy / string iterators and
// higher-order array helpers.
//
// Runtime-layer fragment of jit.h, split out for readability. These
// fragments rely on jit.h's #include block and are included by jit.h in a
// fixed sequence (see jit.h); they are not standalone headers.

// Continues the runtime extern "C" block opened in jit_runtime.h
// (split across jit_fixed.h / jit_dispatch.h / jit_iter.h).
extern "C" {

// --- Iterator protocol runtime --------------------------------------------
//
// Kotlin-style protocol: iterator Objects expose three 0-arg closures —
// `iter` (returns self), `has_next` (Bool), `next` (Any). The JIT
// drives for-in and the terminal/lazy methods through C++ runtime
// helpers so each call site emits one factory / terminal call rather
// than a full closure body. State for built-in factories lives in
// JitCells attached to both trampolines; the last two cells are a
// shared lookahead pair (see `_iter_wrap_new`) that lets `has_next()`
// peek and `next()` consume without re-pulling from `fast_next_fn`.
//
// Conventions:
//   - `_iter_advance_raw(has_next_cls, next_cls, iter, out_tag, out_data)`
//     pulls one value via the cached lookahead when present, otherwise
//     `fast_next_fn` (or the user's has_next/next pair).
//   - HOF receivers are borrowed: terminal methods only pull values
//     (the caller's +1 outlives the call and the caller releases it),
//     and lazy wrapper factories retain what they store — upstream goes
//     into a capture cell backed by the factory's own retain (or a
//     fresh +1 from _iter_coerce_iterable), owned for the wrapper's
//     lifetime and released when the cell dies.

// Forward declared — defined later alongside the array higher-order
// runtime helpers.
// `param_name` is the callback parameter's name in the method's signature
// ("f" or "p"), used to match the interp's typed-param error verbatim.
inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            const char* param_name,
                                            int64_t line, int64_t col);
// Forward-declared so the lazy iterator combinators (defined above the
// definition) can validate + normalize their callback eagerly.
inline JitClosure* _culebra_capture_callback(int8_t fn_tag, int64_t fn_data,
                                             size_t expected_arity,
                                             const char* method_name,
                                             const char* param_name,
                                             int64_t line, int64_t col);

// Acquire a callback for an EAGER/terminal HOF and own it on every exit —
// the callee-consumes side of the HOF contract. The codegen hands the
// callback value at `+1`; `_culebra_expect_callback` normalizes it (a plain
// Function passes through carrying that `+1`; a callable class instance
// becomes a fresh `+1` adapter that absorbed the instance's `+1` into its
// capture cell) and this wrapper releases the normalized closure on any
// exit, including a throw from the per-element call. The lazy combinators
// take the same `+1` into a capture cell via `_culebra_capture_callback`
// instead. Converts to JitClosure* so the call sites read unchanged.
struct JitHofCallback {
  JitClosure* fn;
  JitOwnedVal guard;
  JitHofCallback(int8_t ft, int64_t fd, size_t arity, const char* method,
                 const char* param, int64_t line, int64_t col)
      : fn(_culebra_expect_callback(ft, fd, arity, method, param, line, col)),
        guard(JitValue{TAG_FUNC, reinterpret_cast<int64_t>(fn)}) {}
  JitClosure* operator->() const { return fn; }
  operator JitClosure*() const { return fn; }
};

extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray*
culebra_runtime_object_keys(JitObject* obj);

// Generic "iter returns self" — used by every wrapper we build.
inline void _iter_self_iter_fn(JitValue* __ret, JitClosure*, int8_t self_val_tag, int64_t self_val_data,
                                   int64_t, JitValue*) {
  JitValue self_val{self_val_tag, self_val_data};
  // iter() on a wrapped iterator returns self. The Iterator-protocol calling
  // convention (see compile_for_protocol_loop / _iter_coerce_iterable /
  // object_iter_dispatch) has every caller retain `self` before the call so a
  // script method body can consume it via its frame and return a fresh +1.
  // A native fn has no frame, so it must consume that retain itself: return
  // `self` directly, transferring the caller's +1 to the result. (A stray
  // `retain` here instead of the transfer leaked one ref per `.iter()` on
  // every native wrapped iterator — the for-in protocol loop's ref B.)
  { *__ret = self_val; return; }
}

// Look up a 0-arg method closure on an iterator Object. Returns nullptr
// if the receiver isn't a TAG_OBJECT or doesn't expose `method` as a
// Function. Used once at walk entry so each step skips the per-iteration
// std::map::find.
inline JitClosure* _iter_method_closure(JitValue iter_val,
                                        const char* method) {
  if (iter_val.tag != TAG_OBJECT) return nullptr;
  auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
  // Walk instance + proto: class instances carry methods on their
  // class meta proto, while Phase 2 wrapped iterators put them on
  // the instance directly. `_find_property` handles both.
  auto* entry = _find_property(iter_obj, method);
  if (!entry || entry->value.tag != TAG_FUNC) return nullptr;
  return reinterpret_cast<JitClosure*>(entry->value.data);
}

inline JitClosure* _iter_has_next_closure(JitValue iter_val) {
  return _iter_method_closure(iter_val, "has_next");
}

inline JitClosure* _iter_next_closure(JitValue iter_val) {
  return _iter_method_closure(iter_val, "next");
}

// Pre-resolve an upstream's has_next / next closures into a pair of
// TAG_LONG cells the lazy factories below thread into their captures.
// Folds the 4-line "cell_new for has_next, cell_new for next" pattern
// every map/filter/take/etc. factory used to repeat verbatim — and
// kept their argument order in sync so chain/zip's index shuffle
// doesn't drift again.
struct IterClosureCells {
  JitCell* has_next;
  JitCell* next;
};
inline IterClosureCells _iter_cache_closure_cells(JitValue iv) {
  return {
      culebra_runtime_cell_new(
          TAG_LONG, reinterpret_cast<int64_t>(_iter_has_next_closure(iv))),
      culebra_runtime_cell_new(
          TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv))),
  };
}

// Lookahead state values stored in the state cell appended by
// `_iter_wrap_fast`. The cell carries TAG_LONG with these payloads.
enum : int64_t {
  _ITER_LA_EMPTY   = 0,   // no cached value; pull on demand
  _ITER_LA_FILLED  = 1,   // value cell holds a +1 cached lookahead
  _ITER_LA_DRAINED = 2,   // upstream returned done; has_next stays false
};

// Pull the next value via out-params. For iterators built by
// `_iter_wrap_fast` (i.e. those exposing `fast_next_fn`), consults the
// lookahead state cells appended by that factory so a prior
// `has_next()` peek is consumed transparently. For user-defined
// iterators (no fast_next_fn) calls `has_next()` then `next()` on the
// user-visible closures.
inline bool _iter_advance_raw(JitClosure* has_next_cls, JitClosure* next_cls,
                              JitValue iter_val,
                              int8_t* out_tag, int64_t* out_data) {
  if (iter_val.tag == TAG_OBJECT) {
    auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
    if (iter_obj->fast_next_fn) {
      size_t n = next_cls->n_captures;
      JitCell* state_cell = next_cls->captures[n - 2];
      JitCell* value_cell = next_cls->captures[n - 1];
      if (state_cell->value.data == _ITER_LA_FILLED) {
        // Consume the cached peek; ownership of value_cell's +1
        // transfers to the caller.
        *out_tag = value_cell->value.tag;
        *out_data = value_cell->value.data;
        state_cell->value = {TAG_LONG, _ITER_LA_EMPTY};
        value_cell->value = {TAG_NIL, 0};
        return true;
      }
      if (state_cell->value.data == _ITER_LA_DRAINED) return false;
      bool done;
      iter_obj->fast_next_fn(next_cls, iter_val, &done, out_tag, out_data);
      if (done) {
        state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
        return false;
      }
      return true;
    }
  }
  // Slow path: user-built iterator. Drive its has_next() / next()
  // closures directly; the user owns any caching they need. A non-iterator
  // receiver (no has_next/next) yields null closures — e.g. a terminal
  // method called by name on a builtin namespace like `GC.collect()`.
  // Reject it cleanly instead of calling through a null fn_ptr.
  if (!has_next_cls || !next_cls) {
    throw culebra::CulebraError("TypeError",
                                "type error: target is not iterable");
  }
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  auto hn = _jit_invoke(has_next_cls, iter_val, 0, nullptr);
  bool has = (hn.tag == TAG_BOOL && hn.data != 0);
  _culebra_value_release_impl(hn.tag, hn.data);
  if (!has) return false;
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  auto v = _jit_invoke(next_cls, iter_val, 0, nullptr);
  *out_tag = v.tag;
  *out_data = v.data;
  return true;
}

// Pull the next value from an iterator using pre-cached `has_next` +
// `next` closures. Returns true with `out_v` holding a +1-retained
// Value on success; false on done. Uses the raw fast path when
// available (see `_iter_advance_raw`).
inline bool _iter_pull(JitClosure* has_next_cls, JitClosure* next_cls,
                       JitValue iter_val, JitValue& out_v) {
  // out_tag is i8 (the IR alloca ABI); JitValue::tag is now i64, so write
  // through an i8 temp and widen.
  int8_t tag;
  bool ok = _iter_advance_raw(has_next_cls, next_cls, iter_val, &tag,
                              &out_v.data);
  out_v.tag = tag;
  return ok;
}

// Forward decl: `culebra_runtime_cell_retain` is defined further down
// alongside the other refcount entries (one big TU; the iterator
// factories above need this before that definition is parsed).
extern "C" {
CULEBRA_RT_KEEP void culebra_runtime_cell_retain(JitCell* c);
}

// Build a wrapper iterator Object exposing the Kotlin-style
// `Iterator { has_next() -> Bool, next() -> Any }` + `Iterable.iter()`
// protocol. Both has_next / next closures share the SAME captures
// slab (built once from `captures` + 2 trailing lookahead cells so
// `_iter_advance_raw` and the user-visible trampolines can peek/cache
// transparently).
//
// Captures layout: [user's captures..., state_cell, value_cell].
// state_cell.value.data ∈ { _ITER_LA_EMPTY, _ITER_LA_FILLED, _ITER_LA_DRAINED }.
// value_cell holds a +1-owned lookahead while state == FILLED.
inline JitObject* _iter_wrap_new(
    void (*has_next_fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*),
    void (*next_fn)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*),
    std::initializer_list<JitCell*> captures) {
  auto* state_cell = culebra_runtime_cell_new(TAG_LONG, _ITER_LA_EMPTY);
  auto* value_cell = culebra_runtime_cell_new(TAG_NIL, 0);
  size_t total = captures.size() + 2;
  auto write_captures = [&](JitClosure* cls) {
    size_t i = 0;
    for (auto* c : captures) {
      culebra_runtime_cell_retain(c);
      cls->captures[i++] = c;
    }
    culebra_runtime_cell_retain(state_cell);
    cls->captures[i++] = state_cell;
    culebra_runtime_cell_retain(value_cell);
    cls->captures[i++] = value_cell;
  };
  _jit_register_native_fn(reinterpret_cast<const void*>(&_iter_self_iter_fn));
  _jit_register_native_fn(reinterpret_cast<const void*>(has_next_fn));
  _jit_register_native_fn(reinterpret_cast<const void*>(next_fn));
  auto* iter_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_iter_self_iter_fn), 0, 0);
  auto* has_next_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(has_next_fn), total, 0);
  write_captures(has_next_cls);
  auto* next_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(next_fn), total, 0);
  write_captures(next_cls);
  // Caller transferred +1 on each user cell — release the originals
  // since we now hold one retain per cell per closure (above).
  for (auto* c : captures) _culebra_cell_release(c);
  _culebra_cell_release(state_cell);
  _culebra_cell_release(value_cell);
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(obj, "iter", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(iter_cls), 0, 0);
  culebra_runtime_object_set(obj, "has_next", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(has_next_cls), 0, 0);
  culebra_runtime_object_set(obj, "next", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(next_cls), 0, 0);
  return obj;
}

}  // extern "C" (close briefly for the iterator-wrapper templates)

// has_next() trampoline. Peeks one value via FastFn (caching in the
// state cells) so a subsequent `next()` can consume the cached value
// without re-invoking FastFn. Idempotent on repeat calls.
template <JitIterFastFn FastFn>
inline void _iter_trampoline_has_next_fn(JitValue* __ret, JitClosure* cls, int8_t iv_tag,
                                              int64_t iv_data, int64_t,
                                              JitValue*) {
  JitValue iv{iv_tag, iv_data};
  // Native iterator method: the caller hands `self` (the iterator Object)
  // +1-owned (callee-consumes, like _iter_self_iter_fn and the retain-before-
  // call in _iter_advance_raw's slow path). This releases it on every exit —
  // after the body's use of `cls` (whose captures `iv` keeps alive) — so an
  // explicit `it.has_next()` / `it.next()` (or the for-in-with-yield desugar's
  // `while _g_it.has_next()`) doesn't strand the iterator. Without it the
  // iterator (and its transitively-held source + closures) leaked once per
  // drained iterator.
  JitOwnedVal self_guard(iv);
  size_t n = cls->n_captures;
  JitCell* state_cell = cls->captures[n - 2];
  JitCell* value_cell = cls->captures[n - 1];
  if (state_cell->value.data == _ITER_LA_FILLED) { *__ret = {TAG_BOOL, 1}; return; }
  if (state_cell->value.data == _ITER_LA_DRAINED) { *__ret = {TAG_BOOL, 0}; return; }
  bool done;
  int8_t tag;
  int64_t data;
  FastFn(cls, iv, &done, &tag, &data);
  if (done) {
    state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
    { *__ret = {TAG_BOOL, 0}; return; }
  }
  state_cell->value = {TAG_LONG, _ITER_LA_FILLED};
  value_cell->value = {tag, data};   // +1 transfers into the cell
  { *__ret = {TAG_BOOL, 1}; return; }
}

// next() trampoline. Returns the cached lookahead when present (cheap
// path after has_next()), otherwise pulls a fresh value via FastFn.
// Calling next() past end yields nil; the contract advises pairing
// with has_next() to avoid that case.
template <JitIterFastFn FastFn>
inline void _iter_trampoline_next_fn(JitValue* __ret, JitClosure* cls, int8_t iv_tag,
                                          int64_t iv_data, int64_t,
                                          JitValue*) {
  JitValue iv{iv_tag, iv_data};
  // Consume the caller's `self` +1 on every exit — see the note on
  // _iter_trampoline_has_next_fn. Released after the lookahead value is copied
  // out (the returned +1 is independent of `iv`'s captures).
  JitOwnedVal self_guard(iv);
  size_t n = cls->n_captures;
  JitCell* state_cell = cls->captures[n - 2];
  JitCell* value_cell = cls->captures[n - 1];
  if (state_cell->value.data == _ITER_LA_FILLED) {
    auto v = value_cell->value;
    state_cell->value = {TAG_LONG, _ITER_LA_EMPTY};
    value_cell->value = {TAG_NIL, 0};
    { *__ret = v; return; }  // +1 transfers to caller
  }
  if (state_cell->value.data == _ITER_LA_DRAINED) { *__ret = {TAG_NIL, 0}; return; }
  bool done;
  int8_t tag;
  int64_t data;
  FastFn(cls, iv, &done, &tag, &data);
  if (done) {
    state_cell->value = {TAG_LONG, _ITER_LA_DRAINED};
    { *__ret = {TAG_NIL, 0}; return; }
  }
  { *__ret = {tag, data}; return; }
}

// Build a wrapper Object whose user-visible has_next / next trampoline
// to `FastFn` (shared lookahead cells) and whose `fast_next_fn` slot
// exposes `FastFn` directly so `_iter_advance_raw` can take the fast
// path while still respecting any cached peek.
template <JitIterFastFn FastFn>
inline JitObject* _iter_wrap_fast(std::initializer_list<JitCell*> captures) {
  auto* obj = _iter_wrap_new(&_iter_trampoline_has_next_fn<FastFn>,
                              &_iter_trampoline_next_fn<FastFn>, captures);
  obj->fast_next_fn = FastFn;
  return obj;
}

extern "C" {

// --- Terminal iterator methods --------------------------------------------

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_iter_collect(
    int8_t it, int64_t id) {
  auto* out = culebra_runtime_array_new();
  // Helper-owned accumulator: if a lazy map/filter callback in the chain throws
  // while pulling, this frees `out` (and the elements collected so far) on the
  // unwind edge instead of stranding it. Same shape as array_map's out_guard.
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    culebra_runtime_array_push(out, v.tag, v.data);  // consumes +1
  }
  out_guard.consume();
  return out;
}

// Terminal join: drain the iterator, concatenating elements with `sep`,
// mirroring culebra_runtime_array_join so an iterator `.join(sep)` needs no
// intermediate `.collect()`. Each pulled value carries a +1 ref we release.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_iter_join(
    int8_t it, int64_t id, const char* sep) {
  std::string out;
  bool first = true;
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    if (!first) out += sep;
    first = false;
    if (v.tag == TAG_STRING || v.tag == TAG_STRINGVIEW) {
      out += _culebra_str_view(v.tag, v.data);
    } else {
      out += _culebra_value_to_str_impl(v.tag, v.data);
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
  return _culebra_heap_str(out);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_count(
    int8_t it, int64_t id) {
  int64_t n = 0;
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    _culebra_value_release_impl(v.tag, v.data);
    n++;
  }
  return n;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_for_each(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, "for_each", "f", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1_at(fn, v, line, col);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_reduce(
    int8_t it, int64_t id, int8_t init_tag, int64_t init_data,
    int8_t ft, int64_t fd, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  // See culebra_runtime_array_reduce: own the codegen-consumed seed across the
  // callback validation so an invalid callback releases it instead of stranding.
  JitOwnedVal init_guard{init_tag, init_data};
  JitHofCallback fn(ft, fd, 2, "reduce", "f", line, col);
  init_guard.consume();
  JitValue acc = {init_tag, init_data};
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    acc = _culebra_invoke2_at(fn, acc, v, line, col);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_find(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  JitHofCallback fn(ft, fd, 1, "find", "p", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    JitOwnedVal owned(v);                          // v's original +1 (POC: RAII)
    culebra_runtime_value_retain(v.tag, v.data);   // an extra +1 for the call
    auto r = _culebra_invoke1_at(fn, v, line, col);  // consumes the extra +1
    JitOwnedVal rg(r);                             // result +1
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    if (keep) {
      JitValue kept = owned.consume();            // hand v's +1 to the out-param
      *out_tag = kept.tag;
      *out_data = kept.data;
      return;
    }
    // owned releases v, rg releases r on loop-body scope exit (incl. throw)
  }
  *out_tag = 0;
  *out_data = 0;
}

// position: index of the first match, or nil (out_tag 0) when none matches.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_position(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  JitHofCallback fn(ft, fd, 1, "position", "p", line, col);
  JitValue v;
  int64_t i = 0;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1_at(fn, v, line, col);
    bool hit = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (hit) {
      *out_tag = TAG_LONG;
      *out_data = i;
      return;
    }
    i++;
  }
  *out_tag = 0;
  *out_data = 0;
}

// contains: short-circuiting membership using the same value equality as
// culebra_runtime_array_contains. The needle is borrowed (never stored).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_contains(
    int8_t it, int64_t id, int8_t nt, int64_t nd) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    bool hit = _culebra_value_equal(v.tag, v.data, nt, nd);
    _culebra_value_release_impl(v.tag, v.data);
    if (hit) return 1;
  }
  return 0;
}

// first/last/nth hand the pulled value's +1 straight to the out-param; an
// exhausted iterator answers nil (out_tag 0), matching `find`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_first(
    int8_t it, int64_t id, int8_t* out_tag, int64_t* out_data) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    *out_tag = 0;
    *out_data = 0;
    return;
  }
  *out_tag = v.tag;
  *out_data = v.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_last(
    int8_t it, int64_t id, int8_t* out_tag, int64_t* out_data) {
  JitValue v;
  // Holds the newest pulled value; each replacement releases the previous one,
  // and an unwind out of the pull frees whatever is held.
  JitOwnedVal best(TAG_NIL, 0);
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    best = JitOwnedVal(v);
  }
  auto kept = best.consume();
  *out_tag = kept.tag;
  *out_data = kept.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_iter_nth(
    int8_t it, int64_t id, int64_t n, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  if (n < 0) {
    throw culebra::CulebraError("ValueError", "nth() n must not be negative",
                                line, col);
  }
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  for (int64_t i = 0; i < n; i++) {
    if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
      *out_tag = 0;
      *out_data = 0;
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    *out_tag = 0;
    *out_data = 0;
    return;
  }
  *out_tag = v.tag;
  *out_data = v.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_any(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, "any", "p", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1_at(fn, v, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_all(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, "all", "p", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1_at(fn, v, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

// Sum/product/min/max share the same iterate-and-accumulate shape.
// `min` / `max` on an empty input throw, since there's no natural
// identity for them.

// Numeric accumulator shared by sum/product over both receiver kinds. It
// stays integral while every element is a Long and promotes at the first
// Float, so an all-Long input still answers a Long (`[1, 2].sum()` is 3,
// not 3.0). Mirrors the interp, which accumulates into a Value and lets
// its Long/Float arithmetic promote.
struct _JitNumAcc {
  int64_t i;
  double d;
  bool is_float;

  explicit _JitNumAcc(int64_t init) : i(init), d(0), is_float(false) {}

  void promote() {
    if (!is_float) {
      d = static_cast<double>(i);
      is_float = true;
    }
  }
  JitValue value() const {
    return is_float ? JitValue{TAG_FLOAT, _culebra_double_to_bits(d)}
                    : JitValue{TAG_LONG, i};
  }
};

// Coerce one aggregate element to double, reporting a non-numeric like the
// interp's Value::to_double_coerce ("expected Long or Float, got X"). The
// element's +1 is released before throwing, so the unwind edge strands
// nothing.
inline double _iter_agg_num(JitValue v, int64_t line, int64_t col) {
  if (v.tag == TAG_LONG) return static_cast<double>(v.data);
  if (v.tag == TAG_FLOAT) return _culebra_float_to_double(v.data);
  auto tag = v.tag;
  _culebra_value_release_impl(v.tag, v.data);
  culebra_runtime_type_error_typed(line, col, "Long or Float", tag);
  return 0;  // unreachable
}

// Fold one element into a sum/product accumulator. `mul` selects product.
inline void _iter_agg_step(_JitNumAcc& acc, JitValue v, bool mul,
                           int64_t line, int64_t col) {
  if (v.tag == TAG_LONG && !acc.is_float) {
    if (mul) acc.i *= v.data; else acc.i += v.data;
    return;
  }
  double x = _iter_agg_num(v, line, col);
  acc.promote();
  if (mul) acc.d *= x; else acc.d += x;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_sum(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  _JitNumAcc acc(0);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    _iter_agg_step(acc, v, false, line, col);
  }
  return acc.value();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_product(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  _JitNumAcc acc(1);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    _iter_agg_step(acc, v, true, line, col);
  }
  return acc.value();
}

// min/max return the winning ELEMENT, so a Float input answers a Float and
// a Long input a Long; comparison itself is numeric across both. Ties keep
// the earlier element. Long/Float are immediates, so the retained best
// needs no RC handling.
inline JitValue _iter_minmax(int8_t it, int64_t id, bool want_max,
                             const char* what, int64_t line, int64_t col) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    throw culebra::CulebraError(
        "ValueError", std::string(what) + " of empty Iterator", line, col);
  }
  JitValue best = v;
  double bestd = _iter_agg_num(v, line, col);
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    double x = _iter_agg_num(v, line, col);
    if (want_max ? (x > bestd) : (x < bestd)) {
      best = v;
      bestd = x;
    }
  }
  return best;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_min(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  return _iter_minmax(it, id, false, "min", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_max(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  return _iter_minmax(it, id, true, "max", line, col);
}

// Keyed min/max: the callback supplies the ordering key, the ELEMENT is
// returned. `f` runs once per element. The pulled value carries a +1 that
// the callback call consumes, so the winner is retained explicitly and the
// loser released -- mirroring culebra_runtime_iter_find's handling.
inline JitValue _iter_minmax_by(int8_t it, int64_t id, int8_t ft, int64_t fd,
                                bool want_max, const char* what,
                                int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, what, "f", line, col);
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    throw culebra::CulebraError(
        "ValueError", std::string(what) + " of empty Iterator", line, col);
  }
  auto key_of = [&](JitValue e) {
    culebra_runtime_value_retain(e.tag, e.data);   // the call consumes one
    auto k = _culebra_invoke1_at(fn, e, line, col);
    JitOwnedVal kg(k);
    return _iter_agg_num(k, line, col);
  };
  JitOwnedVal best(v);
  double bestd = key_of(best.borrow());
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) {
    JitOwnedVal cand(v);
    double x = key_of(cand.borrow());
    if (want_max ? (x > bestd) : (x < bestd)) {
      best = std::move(cand);   // releases the old best
      bestd = x;
    }
  }
  return best.consume();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_min_by(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _iter_minmax_by(it, id, ft, fd, false, "min_by", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_iter_max_by(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _iter_minmax_by(it, id, ft, fd, true, "max_by", line, col);
}

// --- Lazy iterator wrappers: each factory returns a new iterator Object.
// Captures are stored in cells attached to the wrapper's `next` closure.
// Cells transfer their +1 to the closure; callers should not retain
// after handing a value to a factory.

// Lazy-combinator call-site plumbing. A lazy map/filter/take_while/flat_map
// invokes its callback per element through the bare closure ABI (no line/
// col), so a builtin handed in as a value (`[..].iter().map(to_long)`) whose
// ns trampoline throws position-less would lose its location — unlike the
// eager HOFs, which record it via `_culebra_expect_callback`. Each factory
// appends a packed `(line<<32|col)` cell as its LAST user capture (just
// before `_iter_wrap_new`'s two trailing lookahead cells), and the fast_fn
// publishes it via `_jit_call_site` right before invoking the callback.
inline JitCell* _iter_pos_cell(int64_t line, int64_t col) {
  return culebra_runtime_cell_new(TAG_LONG,
                                  (line << 32) | (col & 0xFFFFFFFF));
}
inline void _iter_publish_call_site(JitClosure* cls) {
  // Last user capture = captures[n-3]; [n-2]/[n-1] are the lookahead pair.
  int64_t packed = cls->captures[cls->n_captures - 3]->value.data;
  culebra_runtime_set_call_site(packed >> 32, packed & 0xFFFFFFFF);
}

// map: captures upstream + fn. Fast form feeds cascading raw-advance
// through chained wrappers so a deep map/filter/take stack pays zero
// step-object allocations per iteration. Upstream's `next` closure is
// cached at factory time in captures[2] to drop the per-step map::find.
inline void _iter_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  _iter_publish_call_site(cls);
  auto mapped = _culebra_invoke1(fn_cls, {tag, data});
  *done = false;
  *out_tag = mapped.tag;
  *out_data = mapped.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_capture_callback(ft, fd, 1, "map", "f", line, col);
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(TAG_FUNC, reinterpret_cast<int64_t>(fn));
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_map_fast_fn>(
      {up, f, up_has_next_cell, up_next_cell, _iter_pos_cell(line, col)});
}

// filter: captures upstream + predicate + cached upstream next.
inline void _iter_filter_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  for (;;) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    JitValue v = {tag, data};
    culebra_runtime_value_retain(v.tag, v.data);  // for callback
    _iter_publish_call_site(cls);
    auto r = _culebra_invoke1(fn_cls, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      *done = false;
      *out_tag = v.tag;
      *out_data = v.data;
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_filter(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_capture_callback(ft, fd, 1, "filter", "p", line, col);
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(TAG_FUNC, reinterpret_cast<int64_t>(fn));
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_filter_fast_fn>(
      {up, f, up_has_next_cell, up_next_cell, _iter_pos_cell(line, col)});
}

// take: captures upstream + remaining (Long cell, decremented each step)
//       + cached upstream next closure.
inline void _iter_take_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  auto* rem_cell = cls->captures[1];
  if (rem_cell->value.data <= 0) {
    *done = true;
    return;
  }
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  rem_cell->value.data--;
  *done = false;
  *out_tag = tag;
  *out_data = data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_take(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_take_fast_fn>(
      {up, rem, up_has_next_cell, up_next_cell});
}

// skip: captures upstream + remaining-to-skip + cached upstream next.
inline void _iter_skip_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  auto* rem_cell = cls->captures[1];
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  while (rem_cell->value.data > 0) {
    int8_t t;
    int64_t d;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &t, &d)) {
      *done = true;
      return;
    }
    _culebra_value_release_impl(t, d);
    rem_cell->value.data--;
  }
  if (!_iter_advance_raw(up_has_next, up_next, upstream, out_tag, out_data)) {
    *done = true;
    return;
  }
  *done = false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_skip(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_skip_fast_fn>(
      {up, rem, up_has_next_cell, up_next_cell});
}

// take_while: captures upstream + predicate + stopped flag + cached upstream
// next.
inline void _iter_take_while_fast_fn(JitClosure* cls, JitValue, bool* done,
                                     int8_t* out_tag, int64_t* out_data) {
  auto* stopped_cell = cls->captures[2];
  if (stopped_cell->value.data) {
    *done = true;
    return;
  }
  auto upstream = cls->captures[0]->value;
  auto pv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(pv.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  JitValue v = {tag, data};
  culebra_runtime_value_retain(v.tag, v.data);
  _iter_publish_call_site(cls);
  auto r = _culebra_invoke1(fn_cls, v);
  bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
  _culebra_value_release_impl(r.tag, r.data);
  if (!keep) {
    stopped_cell->value.data = 1;
    _culebra_value_release_impl(v.tag, v.data);
    *done = true;
    return;
  }
  *done = false;
  *out_tag = v.tag;
  *out_data = v.data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_take_while(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_capture_callback(ft, fd, 1, "take_while", "p", line, col);
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* p = culebra_runtime_cell_new(TAG_FUNC, reinterpret_cast<int64_t>(fn));
  auto* stopped = culebra_runtime_cell_new(TAG_BOOL, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_take_while_fast_fn>(
      {up, p, stopped, up_has_next_cell, up_next_cell,
       _iter_pos_cell(line, col)});
}

// skip_while: captures upstream + predicate + "still skipping" flag + cached
// upstream next. Once the predicate rejects an element, the flag clears and
// every later step forwards blindly without calling the predicate again.
inline void _iter_skip_while_fast_fn(JitClosure* cls, JitValue, bool* done,
                                     int8_t* out_tag, int64_t* out_data) {
  auto* skipping_cell = cls->captures[2];
  auto upstream = cls->captures[0]->value;
  auto pv = cls->captures[1]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(pv.data);
  for (;;) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    JitValue v = {tag, data};
    if (!skipping_cell->value.data) {
      *done = false;
      *out_tag = v.tag;
      *out_data = v.data;
      return;
    }
    culebra_runtime_value_retain(v.tag, v.data);
    _iter_publish_call_site(cls);
    auto r = _culebra_invoke1(fn_cls, v);
    bool skip = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!skip) {
      skipping_cell->value.data = 0;
      *done = false;
      *out_tag = v.tag;
      *out_data = v.data;
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_skip_while(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_capture_callback(ft, fd, 1, "skip_while", "p", line, col);
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* p = culebra_runtime_cell_new(TAG_FUNC, reinterpret_cast<int64_t>(fn));
  auto* skipping = culebra_runtime_cell_new(TAG_BOOL, 1);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_skip_while_fast_fn>(
      {up, p, skipping, up_has_next_cell, up_next_cell,
       _iter_pos_cell(line, col)});
}

// chunks: captures upstream + size + cached upstream next. Each step pulls
// up to `size` values into a fresh Array (the last chunk may be shorter);
// stops once a pull yields nothing.
inline void _iter_chunks_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  int64_t size = cls->captures[1]->value.data;
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (int64_t i = 0; i < size; i++) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) break;
    culebra_runtime_array_push(out, tag, data);
  }
  if (out->size == 0) {
    *done = true;
    return;
  }
  *done = false;
  *out_tag = TAG_ARRAY;
  *out_data = out_guard.consume().data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_chunks(
    int8_t it, int64_t id, int64_t n, int64_t line, int64_t col) {
  if (n < 1) {
    throw culebra::CulebraError("ValueError",
                                "chunks() n must be at least 1", line, col);
  }
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* size = culebra_runtime_cell_new(TAG_LONG, n);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_chunks_fast_fn>(
      {up, size, up_has_next_cell, up_next_cell});
}

// windows: captures upstream + size + a buffer Array (holds the current
// window's elements) + cached upstream next. Each step tops the buffer up
// to `size`, copies it out as a fresh Array, then drops the buffer's
// oldest element so the next step slides by one.
inline void _iter_windows_fast_fn(JitClosure* cls, JitValue, bool* done,
                                  int8_t* out_tag, int64_t* out_data) {
  int64_t size = cls->captures[1]->value.data;
  auto* buf = reinterpret_cast<JitArray*>(cls->captures[2]->value.data);
  auto upstream = cls->captures[0]->value;
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  while (static_cast<int64_t>(buf->size) < size) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    culebra_runtime_array_push(buf, tag, data);  // absorbs the +1
  }
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (size_t i = 0; i < buf->size; i++) {
    culebra_runtime_value_retain(buf->items[i].tag, buf->items[i].data);
    culebra_runtime_array_push(out, buf->items[i].tag, buf->items[i].data);
  }
  _culebra_value_release_impl(buf->items[0].tag, buf->items[0].data);
  std::memmove(buf->items, buf->items + 1, (buf->size - 1) * sizeof(JitValue));
  buf->size--;
  *done = false;
  *out_tag = TAG_ARRAY;
  *out_data = out_guard.consume().data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_windows(
    int8_t it, int64_t id, int64_t n, int64_t line, int64_t col) {
  if (n < 1) {
    throw culebra::CulebraError("ValueError",
                                "windows() n must be at least 1", line, col);
  }
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* size = culebra_runtime_cell_new(TAG_LONG, n);
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_ARRAY, reinterpret_cast<int64_t>(culebra_runtime_array_new()));
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_windows_fast_fn>(
      {up, size, buf_cell, up_has_next_cell, up_next_cell});
}

// enumerate: captures upstream + index + cached upstream next.
inline void _iter_enumerate_fast_fn(JitClosure* cls, JitValue, bool* done,
                                    int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto* idx_cell = cls->captures[1];
  auto* up_has_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  // Yield `(index, value)` as a Tuple. tuple_push takes ownership of the
  // value's +1 (same as object_set did before), so no extra retain.
  auto* pair = culebra_runtime_tuple_new();
  culebra_runtime_tuple_push(pair, TAG_LONG, idx_cell->value.data);
  culebra_runtime_tuple_push(pair, tag, data);
  idx_cell->value.data++;
  *done = false;
  *out_tag = TAG_TUPLE;
  *out_data = reinterpret_cast<int64_t>(pair);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_enumerate(
    int8_t it, int64_t id) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* idx = culebra_runtime_cell_new(TAG_LONG, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* up_has_next_cell = up_cells.has_next;
  auto* up_next_cell = up_cells.next;
  return _iter_wrap_fast<&_iter_enumerate_fast_fn>(
      {up, idx, up_has_next_cell, up_next_cell});
}

// chain: captures iter1 + iter2 + phase + has_next/next pair for each.
inline void _iter_chain_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* phase_cell = cls->captures[2];
  auto* h1 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* h2 = reinterpret_cast<JitClosure*>(cls->captures[5]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[6]->value.data);
  if (phase_cell->value.data == 0) {
    auto up1 = cls->captures[0]->value;
    if (_iter_advance_raw(h1, n1, up1, out_tag, out_data)) {
      *done = false;
      return;
    }
    phase_cell->value.data = 1;
  }
  auto up2 = cls->captures[1]->value;
  if (_iter_advance_raw(h2, n2, up2, out_tag, out_data)) {
    *done = false;
    return;
  }
  *done = true;
}

inline JitObject* _iter_from_array_obj(int8_t at, int64_t ad);

// Coerce any iterable (Array / Object-with-iter / already-iterator) into an
// iterator Object Value. Returns a fresh +1 (caller owns the result).
// Throws `type error at line:col.` on non-iterable input.
// Forward decl: a range value iterates via the same fast iterator as
// math_range, which is defined further down.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_math_range(
    int64_t start, int64_t end, int64_t step, int64_t line, int64_t col);

// Build a bounded iterator over a range value `data`. An open start or end
// has no defined iteration bound, so an unbounded range is not iterable.
// `step` defaults to 1 when absent (older/manually-built range objects);
// culebra_runtime_math_range validates step != 0.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_range_iter(
    int64_t data, int64_t line, int64_t col) {
  auto* o = reinterpret_cast<JitObject*>(data);
  auto sv = _jit_slot_or_nil(o, "start");
  auto ev = _jit_slot_or_nil(o, "end");
  auto iv = _jit_slot_or_nil(o, "inclusive");
  if (sv.tag == TAG_NIL || ev.tag == TAG_NIL) {
    throw culebra::CulebraError("TypeError", "cannot iterate an unbounded range",
                                line, col);
  }
  auto stv = _jit_slot_or_nil(o, "step");
  int64_t step = stv.tag == TAG_NIL ? 1 : stv.data;
  bool inclusive = iv.tag == TAG_BOOL && iv.data != 0;
  int64_t end = ev.data + (inclusive ? (step > 0 ? 1 : -1) : 0);
  return culebra_runtime_math_range(sv.data, end, step, line, col);
}

inline JitValue _iter_coerce_iterable(int8_t t, int64_t d, int64_t line,
                                      int64_t col) {
  if (culebra_runtime_is_range(t, d)) {
    auto* it = culebra_runtime_range_iter(d, line, col);
    return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
  }
  if (t == TAG_ARRAY || t == TAG_TUPLE) {
    auto* wrapped =
        _iter_from_array_obj(t, d);
    return {TAG_OBJECT, reinterpret_cast<int64_t>(wrapped)};
  }
  if (t == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(d);
    auto idx = o->find_slot("iter");
    if (idx != static_cast<size_t>(-1) &&
        o->slots[idx].value.tag == TAG_FUNC) {
      auto iv = o->slots[idx].value;
      auto* iv_cls = reinterpret_cast<JitClosure*>(iv.data);
      culebra_runtime_value_retain(t, d);
      return _jit_invoke(iv_cls, JitValue{t, d}, 0, nullptr);
    }
  }
  // Match interp's for-in / flat_map coercion wording (throw_type_mismatch),
  // not a JIT-only "target is not iterable".
  culebra::throw_type_mismatch("Array, Tuple, Set, Object, or String",
                               _culebra_tag_name(t), line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_chain(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  // Coerce both inputs so raw Arrays can be chained without the caller
  // having to call `.iter()` first. Each coerce call returns a fresh +1.
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto* phase = culebra_runtime_cell_new(TAG_LONG, 0);
  auto c1 = _iter_cache_closure_cells(iv1);
  auto c2 = _iter_cache_closure_cells(iv2);
  return _iter_wrap_fast<&_iter_chain_fast_fn>(
      {u1, u2, phase, c1.has_next, c1.next, c2.has_next, c2.next});
}

// zip: captures iter1 + iter2 + has_next/next pair for each.
inline void _iter_zip_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto up1 = cls->captures[0]->value;
  auto up2 = cls->captures[1]->value;
  auto* h1 = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* h2 = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[5]->value.data);
  int8_t t1, t2;
  int64_t d1, d2;
  if (!_iter_advance_raw(h1, n1, up1, &t1, &d1)) {
    *done = true;
    return;
  }
  if (!_iter_advance_raw(h2, n2, up2, &t2, &d2)) {
    _culebra_value_release_impl(t1, d1);
    *done = true;
    return;
  }
  auto* pair = culebra_runtime_object_new();
  culebra_runtime_object_set(pair, "first", false, t1, d1, 0, 0);
  culebra_runtime_object_set(pair, "second", false, t2, d2, 0, 0);
  *done = false;
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(pair);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_zip(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto c1 = _iter_cache_closure_cells(iv1);
  auto c2 = _iter_cache_closure_cells(iv2);
  return _iter_wrap_fast<&_iter_zip_fast_fn>(
      {u1, u2, c1.has_next, c1.next, c2.has_next, c2.next});
}

// --- String iterator wrappers --------------------------------------------
// String.code_points() and .graphemes() are implemented as iterator
// factories that wrap the underlying UTF-8 buffer. Each walks the
// String lazily — one decode per next() step — mirroring the interp
// behaviour in `string_builtins()`.

inline void _iter_code_points_fast_fn(JitClosure* cls, JitValue, bool* done,
                                      int8_t* out_tag, int64_t* out_data) {
  auto buf_cell = cls->captures[0];
  auto off_cell = cls->captures[1];
  auto len_cell = cls->captures[2];
  const char* s = reinterpret_cast<const char*>(buf_cell->value.data);
  int64_t off = off_cell->value.data;
  int64_t len = len_cell->value.data;
  if (off >= len) {
    *done = true;
    return;
  }
  char32_t cp;
  size_t bytes;
  if (!unicode::utf8::decode_codepoint(s + off, len - off, bytes, cp)) {
    cp = 0xFFFD;  // U+FFFD replacement; source bytes stay in the String
    bytes = 1;
  }
  off_cell->value.data = off + static_cast<int64_t>(bytes);
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = static_cast<int64_t>(cp);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_code_points(
    const char* s) {
  // Store the backing buffer as a TAG_STRING (not TAG_LONG) so the tracing
  // collector roots it for the iterator's whole lifetime — each step reads
  // `s + off`, so a traced-only String must not be swept while the iterator
  // is live. (A literal / interned base is untracked: the extra root is a
  // harmless no-op.) Lifetime is documented in §16.
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_STRING, reinterpret_cast<int64_t>(s));
  auto* off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(_str_len(s)));
  return _iter_wrap_fast<&_iter_code_points_fast_fn>(
      {buf_cell, off_cell, len_cell});
}

// String.bytes(): yields the receiver's raw UTF-8 bytes as Long (0-255), one
// byte per step — no decoding, unlike code_points. Mirrors the interp's
// `bytes` entry in string_builtins().
inline void _iter_bytes_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  auto buf_cell = cls->captures[0];
  auto off_cell = cls->captures[1];
  auto len_cell = cls->captures[2];
  const char* s = reinterpret_cast<const char*>(buf_cell->value.data);
  int64_t off = off_cell->value.data;
  int64_t len = len_cell->value.data;
  if (off >= len) {
    *done = true;
    return;
  }
  off_cell->value.data = off + 1;
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = static_cast<unsigned char>(s[off]);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_bytes(
    const char* s) {
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_STRING, reinterpret_cast<int64_t>(s));
  auto* off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(_str_len(s)));
  return _iter_wrap_fast<&_iter_bytes_fast_fn>(
      {buf_cell, off_cell, len_cell});
}

// Like code_points, but yields each scalar as a 1-scalar StringView into
// the source buffer — matches interp's `s.iter()` (type StringView, zero
// copy). Invalid bytes yield as one-byte views (docs §17.1).
inline void _iter_str_scalars_fast_fn(JitClosure* cls, JitValue, bool* done,
                                      int8_t* out_tag, int64_t* out_data) {
  auto buf_cell = cls->captures[0];
  auto off_cell = cls->captures[1];
  auto len_cell = cls->captures[2];
  const char* s = reinterpret_cast<const char*>(buf_cell->value.data);
  int64_t off = off_cell->value.data;
  int64_t len = len_cell->value.data;
  if (off >= len) {
    *done = true;
    return;
  }
  char32_t cp;
  size_t bytes;
  if (!unicode::utf8::decode_codepoint(s + off, len - off, bytes, cp)) {
    bytes = 1;  // invalid → one-byte view (raw), like the interp
  }
  auto* v = _culebra_heap_view(s + off, bytes, s);
  off_cell->value.data = off + static_cast<int64_t>(bytes);
  *done = false;
  *out_tag = TAG_STRINGVIEW;
  *out_data = reinterpret_cast<int64_t>(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_scalars(
    const char* s) {
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_STRING, reinterpret_cast<int64_t>(s));
  auto* off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(_str_len(s)));
  return _iter_wrap_fast<&_iter_str_scalars_fast_fn>(
      {buf_cell, off_cell, len_cell});
}

// `for c in s` yields a StringView over the scalar at [off, off+len) of the
// source buffer (matches interp). The view borrows `s` (§16 lifetime).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitStringView* culebra_runtime_str_scalar_view(
    const char* s, int64_t off, int64_t len) {
  return _culebra_heap_view(s + off, static_cast<uint64_t>(len), s);
}

// graphemes: lazy walk yielding Extended Grapheme Cluster boundaries (UAX
// #29) as zero-copy StringViews — one user-perceived character per step.
// Mirrors the interp's streaming `graphemes` (interpreter.h, string_builtins
// `"graphemes"sv`): decode just enough of the source into a rolling window
// of codepoints to confirm the next cluster boundary, so `.take(n)` on a
// multi-MB string only touches the prefix it consumes.
//
// The window buffer is a traced-only String (TAG_STRING, GC-tracked, no
// manual free — see the traced-only-strings barrier work) reinterpreted as
// a char32_t[]. Growing it allocates a fresh buffer via `_str_alloc` and
// leaves the old one for the collector, the same mechanism transient String
// allocations already use; this sidesteps the "no cell tag owns delete of a
// non-trivial C++ type" limitation that kept the original implementation
// eager.
inline constexpr int64_t kGraphemeWindowExtendChunk = 16;

// Grow/refill the window so it holds at least `target` codepoints (or the
// source is exhausted), decoding further UTF-8 scalars from `s` as needed.
inline void _grapheme_window_extend(const char* s, int64_t src_len,
                                    JitCell* src_off_cell,
                                    JitCell* win_cap_cell,
                                    JitCell* win_buf_cell,
                                    JitCell* win_count_cell, int64_t target) {
  int64_t count = win_count_cell->value.data;
  int64_t cap = win_cap_cell->value.data;
  auto* win = reinterpret_cast<char32_t*>(win_buf_cell->value.data);
  int64_t src_off = src_off_cell->value.data;
  while (count < target && src_off < src_len) {
    if (count >= cap) {
      int64_t new_cap = cap == 0 ? kGraphemeWindowExtendChunk : cap * 2;
      char* new_buf = _str_alloc(static_cast<uint64_t>(new_cap) *
                                 sizeof(char32_t));
      if (count > 0) {
        std::memcpy(new_buf, win, static_cast<size_t>(count) * sizeof(char32_t));
      }
      win = reinterpret_cast<char32_t*>(new_buf);
      win_buf_cell->value = JitValue{TAG_STRING, reinterpret_cast<int64_t>(new_buf)};
      cap = new_cap;
      win_cap_cell->value.data = cap;
    }
    char32_t cp;
    size_t bytes;
    if (!unicode::utf8::decode_codepoint(s + src_off,
                                         static_cast<size_t>(src_len - src_off),
                                         bytes, cp)) {
      cp = 0xFFFD;  // U+FFFD replacement; source bytes stay in the String
      bytes = 1;
    }
    win[count] = cp;
    count++;
    src_off += static_cast<int64_t>(bytes);
  }
  win_count_cell->value.data = count;
  src_off_cell->value.data = src_off;
}

inline void _iter_graphemes_fast_fn(JitClosure* cls, JitValue, bool* done,
                                    int8_t* out_tag, int64_t* out_data) {
  auto* buf_cell = cls->captures[0];
  auto* src_len_cell = cls->captures[1];
  auto* src_off_cell = cls->captures[2];
  auto* win_start_cell = cls->captures[3];
  auto* win_count_cell = cls->captures[4];
  auto* win_cap_cell = cls->captures[5];
  auto* win_buf_cell = cls->captures[6];

  const char* s = reinterpret_cast<const char*>(buf_cell->value.data);
  int64_t src_len = src_len_cell->value.data;

  _grapheme_window_extend(s, src_len, src_off_cell, win_cap_cell, win_buf_cell,
                          win_count_cell, win_count_cell->value.data + 1);
  if (win_count_cell->value.data == 0) {
    *done = true;
    return;
  }

  // Grow the window until `grapheme_length` returns strictly less than the
  // available codepoints (a boundary is confirmed inside the buffer) or the
  // source is exhausted — without lookahead a `len == avail` return could
  // still be truncated by a continuation codepoint (matches the interp).
  int64_t cluster_len;
  for (;;) {
    int64_t avail = win_count_cell->value.data;
    auto* win = reinterpret_cast<char32_t*>(win_buf_cell->value.data);
    size_t len = unicode::grapheme_length(win, static_cast<size_t>(avail));
    if (len == 0) len = 1;
    cluster_len = static_cast<int64_t>(len);
    if (cluster_len < avail || src_off_cell->value.data >= src_len) break;
    _grapheme_window_extend(s, src_len, src_off_cell, win_cap_cell,
                            win_buf_cell, win_count_cell,
                            avail + kGraphemeWindowExtendChunk);
  }

  // Re-walk the confirmed cluster's codepoints from the window's start byte
  // offset to find its byte span in the ORIGINAL buffer, so it can be
  // yielded as a zero-copy StringView (matches interp — type StringView).
  // Cheap: cluster_len is almost always 1, rarely more than a handful for
  // ZWJ / regional-indicator sequences.
  int64_t byte_off = win_start_cell->value.data;
  for (int64_t i = 0; i < cluster_len; i++) {
    char32_t cp;
    size_t bytes;
    if (!unicode::utf8::decode_codepoint(s + byte_off,
                                         static_cast<size_t>(src_len - byte_off),
                                         bytes, cp)) {
      bytes = 1;
    }
    byte_off += static_cast<int64_t>(bytes);
  }

  auto* v = _culebra_heap_view(s + win_start_cell->value.data,
                               byte_off - win_start_cell->value.data, s);

  // Drop the consumed cluster_len codepoints from the front of the window.
  auto* win = reinterpret_cast<char32_t*>(win_buf_cell->value.data);
  int64_t remaining = win_count_cell->value.data - cluster_len;
  if (remaining > 0) {
    std::memmove(win, win + cluster_len,
                 static_cast<size_t>(remaining) * sizeof(char32_t));
  }
  win_count_cell->value.data = remaining;
  win_start_cell->value.data = byte_off;

  *done = false;
  *out_tag = TAG_STRINGVIEW;
  *out_data = reinterpret_cast<int64_t>(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_str_graphemes(
    const char* s) {
  // buf_cell roots the source for the iterator's lifetime, same reasoning as
  // code_points/bytes/scalars above. win_buf_cell holds the rolling
  // codepoint window as a traced-only String (grown via _str_alloc, no
  // leak); win_cap_cell==0 / win_buf_cell.data==0 means "not yet allocated".
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_STRING, reinterpret_cast<int64_t>(s));
  auto* src_len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(_str_len(s)));
  auto* src_off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* win_start_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* win_count_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* win_cap_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* win_buf_cell = culebra_runtime_cell_new(TAG_STRING, 0);
  return _iter_wrap_fast<&_iter_graphemes_fast_fn>(
      {buf_cell, src_len_cell, src_off_cell, win_start_cell, win_count_cell,
       win_cap_cell, win_buf_cell});
}

// Wrap a JitArray as a one-shot iterator Object. Used to drive
// flat_map callbacks that return Arrays.
inline void _iter_from_array_fast_fn(JitClosure* cls, JitValue, bool* done,
                                     int8_t* out_tag, int64_t* out_data) {
  auto arr_cell = cls->captures[0];
  auto idx_cell = cls->captures[1];
  auto* arr = reinterpret_cast<JitArray*>(arr_cell->value.data);
  int64_t idx = idx_cell->value.data;
  if (static_cast<size_t>(idx) >= arr->size) {
    *done = true;
    return;
  }
  auto v = arr->items[idx];
  culebra_runtime_value_retain(v.tag, v.data);
  idx_cell->value.data = idx + 1;
  *done = false;
  *out_tag = v.tag;
  *out_data = v.data;
}

inline JitObject* _iter_from_array_obj(int8_t at, int64_t ad) {
  culebra_runtime_value_retain(at, ad);
  auto* arr_cell = culebra_runtime_cell_new(at, ad);
  auto* idx_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  return _iter_wrap_fast<&_iter_from_array_fast_fn>({arr_cell, idx_cell});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_array_iter(
    JitArray* arr) {
  return _iter_from_array_obj(TAG_ARRAY, reinterpret_cast<int64_t>(arr));
}

// `recv.enumerate()`: lazy `(index, value)` iterator over any iterable.
// An Array is turned into an iterator first (so `arr.enumerate()` streams
// without the eager map/filter Array path); an existing iterator object is
// enumerated as-is. Mirrors interp (Array method + iterator combinator).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_enumerate_any(
    int8_t tag, int64_t data) {
  if (tag == TAG_ARRAY || tag == TAG_TUPLE) {
    auto* it = culebra_runtime_array_iter(reinterpret_cast<JitArray*>(data));
    auto* r = culebra_runtime_iter_enumerate(
        TAG_OBJECT, reinterpret_cast<int64_t>(it));
    // iter_enumerate took its own +1; drop the temporary iterator's.
    culebra_runtime_value_release(TAG_OBJECT, reinterpret_cast<int64_t>(it));
    return r;
  }
  if (tag != TAG_OBJECT) {
    culebra_runtime_throw_error(
        "TypeError", "type error: expected Object", 0, 0);
    return nullptr;
  }
  return culebra_runtime_iter_enumerate(tag, data);
}


// `obj.iter()` dispatcher used by JIT — prefers a user-defined `iter`
// method (so an Object that already exposes the Iterator protocol
// returns itself / its underlying state) and only falls back to the
// key iterator from `ObjectValue::builtins()` for plain literals
// without a user iter. Mirrors interp's `_get_iterator`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject*
culebra_runtime_object_iter_dispatch(JitObject* obj);

// Presence check that does NOT consume the key (unlike object_has_value's
// non-String path). Used by the object iterators to skip keys removed
// mid-iteration without disturbing the snapshot array's reference.
inline bool _jit_obj_has_key(JitObject* obj, JitValue key) {
  if (key.tag == TAG_STRING) {
    return _find_property(obj, reinterpret_cast<const char*>(key.data)) !=
           nullptr;
  }
  return obj->non_string_props &&
         obj->non_string_props->contains({key.tag, key.data});
}

// Object for-in / .iter() fast fn — yields `(key, value)` tuples over the
// key snapshot. No structural-mutation guard: keys added during iteration
// are not in the snapshot (not visited), keys removed are skipped, and the
// value is read live. The key gets a fresh +1 for the tuple (the snapshot
// array keeps its own); `object_get_any` returns the value +1 and consumes
// refcounted keys, so retain once more for it.
inline void _iter_from_object_pairs_fast_fn(JitClosure* cls, JitValue,
                                            bool* done, int8_t* out_tag,
                                            int64_t* out_data) {
  auto obj_cell = cls->captures[0];
  auto arr_cell = cls->captures[1];
  auto idx_cell = cls->captures[2];
  auto* obj = reinterpret_cast<JitObject*>(obj_cell->value.data);
  auto* arr = reinterpret_cast<JitArray*>(arr_cell->value.data);
  while (static_cast<size_t>(idx_cell->value.data) < arr->size) {
    auto key = arr->items[idx_cell->value.data];
    idx_cell->value.data++;
    if (!_jit_obj_has_key(obj, key)) continue;  // removed → skip
    int8_t vt;
    int64_t vd;
    culebra_runtime_value_retain(key.tag, key.data);
    culebra_runtime_object_get_any(obj, key.tag, key.data, &vt, &vd, 0, 0,
                                   /*own_receiver=*/false);
    culebra_runtime_value_retain(key.tag, key.data);
    auto* pair = culebra_runtime_tuple_new();
    culebra_runtime_tuple_push(pair, key.tag, key.data);
    culebra_runtime_tuple_push(pair, vt, vd);
    *done = false;
    *out_tag = TAG_TUPLE;
    *out_data = reinterpret_cast<int64_t>(pair);
    return;
  }
  *done = true;
}

// Object.iter(): yield `(key, value)` pairs in insertion order (Ruby-style
// entries view). Iterates a snapshot of the keys taken here; mutating the
// object in the loop body is safe (adds not visited, removes skipped, value
// read live).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_object_iter(
    JitObject* obj) {
  auto* keys = culebra_runtime_object_keys(obj);
  culebra_runtime_value_retain(TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* obj_cell = culebra_runtime_cell_new(
      TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* arr_cell = culebra_runtime_cell_new(
      TAG_ARRAY, reinterpret_cast<int64_t>(keys));
  auto* idx_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  return _iter_wrap_fast<&_iter_from_object_pairs_fast_fn>(
      {obj_cell, arr_cell, idx_cell});
}

// Object.values() fast fn — the value-only view of the pairs iterator.
// Same snapshot + skip-removed + live-read; yields the value directly.
inline void _iter_from_object_values_fast_fn(JitClosure* cls, JitValue,
                                             bool* done, int8_t* out_tag,
                                             int64_t* out_data) {
  auto obj_cell = cls->captures[0];
  auto arr_cell = cls->captures[1];
  auto idx_cell = cls->captures[2];
  auto* obj = reinterpret_cast<JitObject*>(obj_cell->value.data);
  auto* arr = reinterpret_cast<JitArray*>(arr_cell->value.data);
  while (static_cast<size_t>(idx_cell->value.data) < arr->size) {
    auto key = arr->items[idx_cell->value.data];
    idx_cell->value.data++;
    if (!_jit_obj_has_key(obj, key)) continue;  // removed → skip
    culebra_runtime_value_retain(key.tag, key.data);
    culebra_runtime_object_get_any(obj, key.tag, key.data, out_tag, out_data, 0,
                                   0, /*own_receiver=*/false);
    *done = false;
    return;
  }
  *done = true;
}

// Object.values(): lazy iterator over values in insertion order. Snapshots
// the keys (so `next` is O(1) to advance) and reads each value live, with
// the same structural-mutation guard as the key iterator.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_object_values(
    JitObject* obj) {
  auto* keys = culebra_runtime_object_keys(obj);
  culebra_runtime_value_retain(TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* obj_cell = culebra_runtime_cell_new(
      TAG_OBJECT, reinterpret_cast<int64_t>(obj));
  auto* arr_cell = culebra_runtime_cell_new(
      TAG_ARRAY, reinterpret_cast<int64_t>(keys));
  auto* idx_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  return _iter_wrap_fast<&_iter_from_object_values_fast_fn>(
      {obj_cell, arr_cell, idx_cell});
}

// Dispatch helper: prefer the user `iter` method when present so an
// Object that already implements Iterator returns the right iterator
// (range/map/etc. wrapped iterators put `iter` on the instance; class
// instances put it on the proto — `_find_property` covers both), and
// fall back to the key iterator for plain `{...}` literals via
// `ObjectValue::builtins()`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject*
culebra_runtime_object_iter_dispatch(JitObject* obj) {
  auto* entry = _find_property(obj, "iter");
  if (entry && entry->value.tag == TAG_FUNC) {
    auto* cls = reinterpret_cast<JitClosure*>(entry->value.data);
    auto self = JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(obj)};
    // _culebra_invoke_method0 retains `self`, and frees it on a throw from the
    // user iter() body via its JitUnwindRelease (callee-consumes on normal return).
    auto r = _culebra_invoke_method0(cls, self);
    // The user iter() returns an Object (+1) — caller takes that retain.
    return reinterpret_cast<JitObject*>(r.data);
  }
  return culebra_runtime_object_iter(obj);
}

// flat_map captures: upstream, fn, current-inner-iter (nullable),
// has_next/next pair for upstream, has_next/next pair for the current
// inner (refreshed per-element when fn() returns a new iterable), and
// a packed (line << 32 | col) cell for error reporting when the
// callback yields a non-iterable.
inline void _iter_flat_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                                   int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  auto* inner_cell = cls->captures[2];
  auto* up_has_next =
      reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* up_next =
      reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  auto* inner_has_next_cell = cls->captures[5];
  auto* inner_next_cell = cls->captures[6];
  int64_t packed = cls->captures[7]->value.data;
  int64_t line = packed >> 32;
  int64_t col = packed & 0xFFFFFFFF;
  for (;;) {
    auto inner_v = inner_cell->value;
    if (inner_v.tag == TAG_OBJECT && inner_v.data != 0) {
      auto* inner_has_next =
          reinterpret_cast<JitClosure*>(inner_has_next_cell->value.data);
      auto* inner_next =
          reinterpret_cast<JitClosure*>(inner_next_cell->value.data);
      if (_iter_advance_raw(inner_has_next, inner_next, inner_v,
                             out_tag, out_data)) {
        *done = false;
        return;
      }
      _culebra_value_release_impl(inner_v.tag, inner_v.data);
      inner_cell->value = {0, 0};
      inner_has_next_cell->value = {TAG_LONG, 0};
      inner_next_cell->value = {TAG_LONG, 0};
    }
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_has_next, up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    culebra_runtime_set_call_site(line, col);
    // invoke1 consumes the pulled value's +1 (the callee frame takes ownership
    // on entry, per the calling convention) — do NOT release it again here.
    // An identity callback `fn(xs){xs}` returns that same heap object, so a
    // spurious release double-frees it; integer elements hid the bug because
    // releasing a Long is a no-op, and the dispatch_arr_iter receiver leak
    // masked it further by keeping the heap alive.
    auto mapped = _culebra_invoke1(fn_cls, {tag, data});
    auto iv = _iter_coerce_iterable(mapped.tag, mapped.data, line, col);
    _culebra_value_release_impl(mapped.tag, mapped.data);
    inner_cell->value = iv;
    inner_has_next_cell->value = {
        TAG_LONG, reinterpret_cast<int64_t>(_iter_has_next_closure(iv))};
    inner_next_cell->value = {
        TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv))};
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_flat_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line,
    int64_t col) {
  auto* fn = _culebra_capture_callback(ft, fd, 1, "flat_map", "f", line, col);
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(TAG_FUNC, reinterpret_cast<int64_t>(fn));
  auto* inner = culebra_runtime_cell_new(TAG_NIL, 0);
  auto up_cells = _iter_cache_closure_cells({it, id});
  auto* inner_has_next = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* inner_next = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* loc = culebra_runtime_cell_new(
      TAG_LONG, (line << 32) | (col & 0xFFFFFFFF));
  return _iter_wrap_fast<&_iter_flat_map_fast_fn>(
      {up, f, inner, up_cells.has_next, up_cells.next,
       inner_has_next, inner_next, loc});
}

inline void _math_range_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* current_cell = cls->captures[0];
  auto* end_cell = cls->captures[1];
  auto* step_cell = cls->captures[2];
  int64_t current = current_cell->value.data;
  int64_t end = end_cell->value.data;
  int64_t step = step_cell->value.data;
  bool finished = step > 0 ? current >= end : current <= end;
  if (finished) {
    *done = true;
    return;
  }
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = current;
  current_cell->value.data = current + step;
}

// Entry point called from for-in codegen (`rt::iter_advance`): returns
// 1 on a value step with `*out_tag`/`*out_data` holding a +1 Value, 0
// on done. `next_cls` is the `iter.next` closure resolved once before
// the loop; forwarded as the `self` capture slab on the fast path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_iter_advance(
    JitClosure* has_next_cls, JitClosure* next_cls,
    int8_t it, int64_t id, int8_t* out_tag, int64_t* out_data) {
  return _iter_advance_raw(has_next_cls, next_cls, {it, id},
                            out_tag, out_data) ? 1 : 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_math_range(
    int64_t start, int64_t end, int64_t step, int64_t line, int64_t col) {
  if (step == 0) {
    throw culebra::CulebraError("ValueError",
        "range() step must not be zero", line, col);
  }
  auto* current_cell = culebra_runtime_cell_new(TAG_LONG, start);
  auto* end_cell = culebra_runtime_cell_new(TAG_LONG, end);
  auto* step_cell = culebra_runtime_cell_new(TAG_LONG, step);
  return _iter_wrap_fast<&_math_range_fast_fn>(
      {current_cell, end_cell, step_cell});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_iota(int64_t start,
                                                            int64_t end) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = start; i < end; i++) {
    culebra_runtime_array_push(r, /*tag Long*/ 2, i);
  }
  return r;
}

// --- Higher-order array helpers -------------------------------------------
//
// These invoke a user-supplied JIT-compiled closure per element. The call
// signatures below must match compile_function's emitted signature:
//
//   %Value fn(ptr __cls__, %Value this, %Value p1, ...)
//
// Closure arg ownership: each arg Value passed to the JIT'd function is
// consumed (released at function exit). So array elements must be retained
// before being handed to the closure.

// Type-check a higher-order callback argument. Returns the Closure or
// throws. Centralizes the identical preamble shared by all higher-order
// array runtime helpers.
// Forwarding body for the adapter closure that lets a callable class
// instance stand in for a function callback. captures[0] is the instance
// (bound as `self`), captures[1] is its resolved `__call__` closure
// (captured once so the per-element call skips the property lookup). Each
// call dispatches to __call__ with the callback args passed through.
inline void _culebra_callable_adapter(JitValue* __ret, JitClosure* self, int8_t, int64_t,
                                          int64_t n, JitValue* args) {
  JitValue inst = self->captures[0]->value;  // borrowed from the capture cell
  auto* m = reinterpret_cast<JitClosure*>(self->captures[1]->value.data);
  // The compiled __call__ frame consumes this retain on EVERY exit — its
  // `self` slot releases on a normal return, and its fn-level cleanup pad
  // releases it on a throw — so no guard belongs here. (A throw-path guard
  // used to sit here compensating for the codegen's stranded callback `+1`;
  // under the callee-consumes contract that `+1` lives in the capture cell,
  // and the extra release freed the instance out from under it — a teardown
  // use-after-free on a mid-iteration `__call__` throw, ASan-confirmed.)
  culebra_runtime_value_retain(inst.tag, inst.data);
  { *__ret = _jit_invoke(m, inst, n, args); return; }
}

// Whether `cls` accepts exactly `expected` positional callback args, using the
// shared callback-arity rule (interp parity). A builtin variadic ns-method
// closure (JIT_VARIADIC_ARITY) accepts any count — its trampoline validates
// the real arity. A user closure consults its registered param meta's
// cb_min/cb_max; absent meta (rare) falls back to exact match.
inline bool _culebra_callback_arity_ok(JitClosure* cls, size_t expected) {
  // ns-method closures share one trampoline fn_ptr (no per-fn JitParamMeta)
  // and a kwarg-capable method is JIT_VARIADIC_ARITY, which would wrongly
  // accept any callback arity. Consult the hook for its real bounds first so
  // e.g. `map(JSON.stringify)` (1 required + 3 optional) is rejected like the
  // interp's gate.
  if (_jit_ns_callback_arity_hook) {
    long cb_min, cb_max;
    if (_jit_ns_callback_arity_hook(cls, &cb_min, &cb_max)) {
      return culebra::callback_arity_accepts(cb_min, cb_max,
                                             static_cast<long>(expected));
    }
  }
  if (cls->arity == JIT_VARIADIC_ARITY) return true;
  const JitParamMeta* meta = _jit_lookup_param_meta(cls->fn_ptr);
  if (meta) {
    return culebra::callback_arity_accepts(meta->cb_min, meta->cb_max,
                                           static_cast<long>(expected));
  }
  return cls->arity == expected;
}

inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            const char* param_name,
                                            int64_t line, int64_t col) {
  // Record the HOF call site so a callback invoked per element (which the
  // runtime calls through the bare closure ABI, no line/col) can report it on
  // a position-less throw — e.g. a builtin handed in as a value, `["x"].
  // map(Math.abs)`, whose ns trampoline backfills from this. Matches interp's
  // eval-wrapper, which attributes such errors to the enclosing HOF call.
  culebra_runtime_set_call_site(line, col);
  auto accepts = [&](JitClosure* cls) {
    return _culebra_callback_arity_ok(cls, expected_arity);
  };
  // Callee-consumes: the codegen hands the callback value at `+1` and never
  // releases it — this helper's caller (`JitHofCallback` for the eager
  // drivers, `_culebra_capture_callback` for the lazy factories) owns it from
  // here on every exit. A rejection throw is the one edge where neither has
  // taken over yet, so the guard is the sole releaser there — all tags.
  JitUnwindRelease g{JitValue{fn_tag, fn_data}};
  if (fn_tag != TAG_FUNC) {
    // A callable class instance (own/proto `__call__`) stands in for a
    // function: synthesize an adapter closure that forwards to __call__
    // with `self` bound (Option A: structural callable). Mirrors interp's
    // as_callback. proto-gated so a plain dict isn't callable.
    if (fn_tag == TAG_OBJECT) {
      auto* obj = reinterpret_cast<JitObject*>(fn_data);
      auto* e = obj->proto ? _find_property(obj, "__call__") : nullptr;
      if (e && e->value.tag == TAG_FUNC) {
        auto* call_cls = reinterpret_cast<JitClosure*>(e->value.data);
        if (!accepts(call_cls)) {
          throw culebra::CulebraError("TypeError", std::format(
              "type error: {} expects a {}-parameter function",
              method_name, expected_arity), line, col);
        }
        // Capture the instance and its __call__ closure (each +1, released
        // when the adapter is collected). The instance's +1 is the incoming
        // callee-consumes ref, transferred into the capture cell; the
        // resolved closure is a borrowed property read, so it takes a fresh
        // retain. Capturing the closure lets the per-element forward skip
        // the property lookup.
        culebra_runtime_value_retain(TAG_FUNC, e->value.data);
        _jit_register_native_fn(
            reinterpret_cast<const void*>(&_culebra_callable_adapter));
        auto* adapter = culebra_runtime_closure_new(
            reinterpret_cast<void*>(&_culebra_callable_adapter), 2,
            expected_arity);
        adapter->captures[0] = culebra_runtime_cell_new(fn_tag, fn_data);
        adapter->captures[1] =
            culebra_runtime_cell_new(TAG_FUNC, e->value.data);
        return adapter;
      }
    }
    // A non-Function (non-callable) argument: the interp routes it through
    // the method's typed-param check (`<param>: Function`), which reports
    // "parameter '<name>' expects Function" at the ARGUMENT's position. Match
    // both the param name (f/p, per method) and the position (the JIT stores
    // the argument site in _jit_callback_arg_* just before the HOF call).
    throw culebra::CulebraError("TypeError",
        std::format("type error: parameter '{}' expects Function", param_name),
        _jit_callback_arg_line, _jit_callback_arg_col);
  }
  auto* fn = reinterpret_cast<JitClosure*>(fn_data);
  if (!accepts(fn)) {
    throw culebra::CulebraError("TypeError", std::format(
        "type error: {} expects a {}-parameter function",
        method_name, expected_arity), line, col);
  }
  return fn;
}

// Validate + normalize a callback for a LAZY iterator combinator (map/filter/
// take_while/flat_map), which captures it in its wrapper and invokes it per
// element. Callee-consumes the incoming `+1` and returns a +1-owned closure
// for the combinator to capture: a plain Function (the incoming ref passes
// through), or — for a callable class instance (Option A) — an adapter that
// forwards to __call__ with `self` bound. Reuses _culebra_expect_callback
// (the eager-HOF path / interp's as_callback) so all iterator HOFs accept a
// callable identically. The caller transfers the returned reference into its
// capture cell with no extra retain.
inline JitClosure* _culebra_capture_callback(int8_t fn_tag, int64_t fn_data,
                                             size_t expected_arity,
                                             const char* method_name,
                                             const char* param_name,
                                             int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, expected_arity,
                                      method_name, param_name, line, col);
  // Callee-consumes: _expect_callback hands back the incoming `+1` (a plain
  // Function passes through; a callable instance becomes a fresh `+1`
  // adapter), which the capture cell takes over directly.
  return fn;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"map", "f", line, col);
  auto* out = culebra_runtime_array_new();
  // The accumulator is helper-owned (never handed to the callback), so freeing
  // it if a per-element call throws is unambiguous — it releases the elements
  // mapped so far too. (The element passed to the callback is a separate matter:
  // a throwing user callback leaks it, a builtin self-cleans it — that
  // asymmetry is a known follow-up, not guarded here.)
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    culebra_runtime_array_push(out, r.tag, r.data);
  }
  out_guard.consume();
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_filter(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"filter", "f", line, col);
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      culebra_runtime_value_retain(e.tag, e.data);
      culebra_runtime_array_push(out, e.tag, e.data);
    }
  }
  out_guard.consume();
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_for_each(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"for_each", "f", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

// find returns the first matching element (or nil) via out-params.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_find(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  JitHofCallback fn(fn_tag, fn_data, 1,"find", "f", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      culebra_runtime_value_retain(e.tag, e.data);
      *out_tag = e.tag;
      *out_data = e.data;
      return;
    }
  }
  *out_tag = 0;
  *out_data = 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_any(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"any", "f", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_all(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"all", "f", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_flat_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"flat_map", "f", line, col);
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1_at(fn, e, line, col);
    if (r.tag != TAG_ARRAY) {
      _culebra_value_release_impl(r.tag, r.data);
      throw culebra::CulebraError("TypeError",
          "type error: flat_map callback must return an Array", line, col);
    }
    auto* inner = reinterpret_cast<JitArray*>(r.data);
    for (size_t j = 0; j < inner->size; j++) {
      auto ie = inner->items[j];
      culebra_runtime_value_retain(ie.tag, ie.data);
      culebra_runtime_array_push(out, ie.tag, ie.data);
    }
    _culebra_value_release_impl(r.tag, r.data);
  }
  out_guard.consume();
  return out;
}

// sort_by: evaluates the key function on each element once, stable-sorts
// in place by those keys. Mutates. Keys are released on every exit path
// (including during a throw from the key comparison).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_sort_by(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, bool reverse, int64_t line,
    int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1,"sort_by", "f", line, col);
  // Each computed key is held owned, so every exit path — normal, a throw
  // from the key callback, or from the key comparison — releases it.
  std::vector<std::pair<JitOwnedVal, size_t>> keyed;
  keyed.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    keyed.emplace_back(JitOwnedVal(_culebra_invoke1_at(fn, e, line, col)), i);
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [reverse, line, col](const auto& a, const auto& b) {
                     const auto& x = reverse ? b : a;
                     const auto& y = reverse ? a : b;
                     auto xk = x.first.borrow(), yk = y.first.borrow();
                     return _culebra_value_ord(
                         xk.tag, xk.data, yk.tag, yk.data,
                         [](double p, double q) { return p < q; },
                         line, col);
                   });
  std::vector<JitValue> sorted(arr->size);
  for (size_t i = 0; i < keyed.size(); i++) {
    sorted[i] = arr->items[keyed[i].second];
  }
  for (size_t i = 0; i < arr->size; i++) arr->items[i] = sorted[i];
}

// Non-mutating sort_by: returns a fresh array with the elements in sorted
// order; the receiver is untouched, so it chains. The new array owns its own
// refs to the (shared) elements.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_sorted_by(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, bool reverse, int64_t line,
    int64_t col) {
  JitHofCallback fn(fn_tag, fn_data, 1, "sorted_by", "f", line, col);
  // Owned keys release on every exit path (normal, callback throw, or a throw
  // from the key comparison).
  std::vector<std::pair<JitOwnedVal, size_t>> keyed;
  keyed.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    keyed.emplace_back(JitOwnedVal(_culebra_invoke1_at(fn, e, line, col)), i);
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [reverse, line, col](const auto& a, const auto& b) {
                     const auto& x = reverse ? b : a;
                     const auto& y = reverse ? a : b;
                     auto xk = x.first.borrow(), yk = y.first.borrow();
                     return _culebra_value_ord(
                         xk.tag, xk.data, yk.tag, yk.data,
                         [](double p, double q) { return p < q; }, line, col);
                   });
  auto* out = culebra_runtime_array_new();
  for (auto& [k, idx] : keyed) {
    auto e = arr->items[idx];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(out, e.tag, e.data);
  }
  return out;
}

// Keyless natural-order sort (in place). Elements compare by the same rule as
// `<`: `_value_less_borrow` honors an Object's __lt__/cmp (so a Path array
// sorts) and throws for incomparable operands. It is the borrow-contract core
// (no ref consumed), so the array's elements stay owned by `arr` — the public
// `culebra_runtime_value_less` entry would release them on the type-error edge,
// which is correct for codegen operand temps but would corrupt `arr` here.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_sort(
    JitArray* arr, bool reverse, int64_t line, int64_t col) {
  std::stable_sort(arr->items, arr->items + arr->size,
                   [reverse, line, col](const JitValue& a, const JitValue& b) {
                     const auto& x = reverse ? b : a;
                     const auto& y = reverse ? a : b;
                     return _value_less_borrow(x.tag, x.data, y.tag,
                                               y.data, line, col);
                   });
}

// Non-mutating twin of `sort`: returns a fresh array (owning its own refs to
// the shared elements) sorted in natural order; the receiver is untouched, so
// it chains.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_sorted(
    JitArray* arr, bool reverse, int64_t line, int64_t col) {
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard{TAG_ARRAY, reinterpret_cast<int64_t>(out)};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(out, e.tag, e.data);
  }
  culebra_runtime_array_sort(out, reverse, line, col);
  out_guard.consume();
  return out;
}

// reduce returns the final accumulator via out-params (avoids relying on
// cross-language struct-return ABI).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_reduce(
    JitArray* arr, int8_t init_tag, int64_t init_data, int8_t fn_tag,
    int64_t fn_data, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  // Codegen consumed the seed into this call (`+1` transferred). The callback
  // validation below throws before the seed is used, so own it across the
  // construction and release it on that throw; on success `.consume()` hands
  // it back to `acc` (consumed by the first fold step, or returned as-is for an
  // empty array). Declared first so a JitHofCallback ctor throw runs its dtor.
  JitOwnedVal init_guard{init_tag, init_data};
  JitHofCallback fn(fn_tag, fn_data, 2, "reduce", "f", line, col);
  init_guard.consume();
  JitValue acc = {init_tag, init_data};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    acc = _culebra_invoke2_at(fn, acc, e, line, col);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

// Eager Array variants of sum/product/min/max. Same semantics as the
// iterator methods above (Long stays Long, a Float promotes the result,
// min/max return the winning element); empty min/max raise ValueError.
// Elements are borrowed from the Array, so a non-numeric throws without
// any release.

inline double _arr_agg_num(JitValue v, int64_t line, int64_t col) {
  if (v.tag == TAG_LONG) return static_cast<double>(v.data);
  if (v.tag == TAG_FLOAT) return _culebra_float_to_double(v.data);
  culebra_runtime_type_error_typed(line, col, "Long or Float", v.tag);
  return 0;  // unreachable
}

inline JitValue _arr_sum_product(JitArray* arr, bool mul, int64_t line,
                                 int64_t col) {
  _JitNumAcc acc(mul ? 1 : 0);
  for (size_t i = 0; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag == TAG_LONG && !acc.is_float) {
      if (mul) acc.i *= e.data; else acc.i += e.data;
      continue;
    }
    double x = _arr_agg_num(e, line, col);
    acc.promote();
    if (mul) acc.d *= x; else acc.d += x;
  }
  return acc.value();
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_sum(
    JitArray* arr, int64_t line, int64_t col) {
  return _arr_sum_product(arr, false, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_product(
    JitArray* arr, int64_t line, int64_t col) {
  return _arr_sum_product(arr, true, line, col);
}

inline JitValue _arr_minmax(JitArray* arr, bool want_max, const char* what,
                            int64_t line, int64_t col) {
  if (arr->size == 0) {
    throw culebra::CulebraError(
        "ValueError", std::string(what) + " of empty Array", line, col);
  }
  JitValue best = arr->items[0];
  double bestd = _arr_agg_num(best, line, col);
  for (size_t i = 1; i < arr->size; i++) {
    auto& e = arr->items[i];
    double x = _arr_agg_num(e, line, col);
    if (want_max ? (x > bestd) : (x < bestd)) {
      best = e;
      bestd = x;
    }
  }
  return best;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_min(
    JitArray* arr, int64_t line, int64_t col) {
  return _arr_minmax(arr, false, "min", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_max(
    JitArray* arr, int64_t line, int64_t col) {
  return _arr_minmax(arr, true, "max", line, col);
}

// Keyed twin of _arr_minmax. Elements are borrowed from the Array, so only
// the callback's own argument ref and its result need handling.
inline JitValue _arr_minmax_by(JitArray* arr, int8_t ft, int64_t fd,
                               bool want_max, const char* what,
                               int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, what, "f", line, col);
  if (arr->size == 0) {
    throw culebra::CulebraError(
        "ValueError", std::string(what) + " of empty Array", line, col);
  }
  auto key_of = [&](JitValue e) {
    culebra_runtime_value_retain(e.tag, e.data);   // the call consumes one
    auto k = _culebra_invoke1_at(fn, e, line, col);
    JitOwnedVal kg(k);
    return _arr_agg_num(k, line, col);
  };
  JitValue best = arr->items[0];
  double bestd = key_of(best);
  for (size_t i = 1; i < arr->size; i++) {
    double x = key_of(arr->items[i]);
    if (want_max ? (x > bestd) : (x < bestd)) {
      best = arr->items[i];
      bestd = x;
    }
  }
  culebra_runtime_value_retain(best.tag, best.data);  // +1 for the caller
  return best;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_min_by(
    JitArray* arr, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _arr_minmax_by(arr, ft, fd, false, "min_by", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_max_by(
    JitArray* arr, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _arr_minmax_by(arr, ft, fd, true, "max_by", line, col);
}

// Length in UTF-8 bytes of the next scalar at `offset`. Returns 0
// once `offset >= len`; on an invalid lead byte, returns 1 (emit the
// raw byte to avoid stalling the iterator). Mirrors the interpreter's
// String.iter semantics.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_utf8_scalar_len(
    const char* s, int64_t offset, int64_t len) {
  if (offset >= len) return 0;
  auto r = peg::codepoint_length(s + offset, len - offset);
  return r == 0 ? 1 : static_cast<int64_t>(r);
}

// Heap-copy `scalar_len` bytes from `s + offset` into a new String.
// Used by JIT for-in over String to yield one-scalar Strings.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_scalar_at(
    const char* s, int64_t offset, int64_t scalar_len) {
  return _culebra_heap_str(std::string_view(s + offset,
                                            static_cast<size_t>(scalar_len)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_str_size(const char* s) {
  return static_cast<int64_t>(_str_len(s));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_upper(
    const char* s) {
  return _culebra_heap_str(culebra::ascii_upper(std::string(_str_sv(s))));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_lower(
    const char* s) {
  return _culebra_heap_str(culebra::ascii_lower(std::string(_str_sv(s))));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_trim(
    const char* s) {
  auto trimmed = culebra::trim_ascii(_str_sv(s));
  return _culebra_heap_str(trimmed);
}

// `s.tr(from, to)` — per-scalar translation; shares culebra::str_tr with
// the interp so both backends agree byte-for-byte.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_tr(
    const char* s, const char* from, const char* to) {
  return _culebra_heap_str(
      culebra::str_tr(_str_sv(s), _str_sv(from), _str_sv(to)));
}

// `s.trim_start(chars)` / `s.trim_end(chars)` — empty `chars` trims ASCII
// whitespace. Shares culebra::trim_chars with the interp.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_trim_start(
    const char* s, const char* chars) {
  return _culebra_heap_str(
      culebra::trim_chars(_str_sv(s), _str_sv(chars), true, false));
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_trim_end(
    const char* s, const char* chars) {
  return _culebra_heap_str(
      culebra::trim_chars(_str_sv(s), _str_sv(chars), false, true));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_str_split(
    const char* s, const char* sep) {
  auto* r = culebra_runtime_array_new();
  std::string_view sv = _str_sv(s);
  std::string_view sp = _str_sv(sep);
  auto push_view = [&](std::string_view piece) {
    auto* v = _culebra_heap_view(piece.data(), piece.size(), s);
    culebra_runtime_array_push(r, TAG_STRINGVIEW,
                               reinterpret_cast<int64_t>(v));
  };
  if (sp.empty()) {
    push_view(sv);
    return r;
  }
  size_t pos = 0;
  while (true) {
    auto p = sv.find(sp, pos);
    if (p == std::string_view::npos) {
      push_view(sv.substr(pos));
      break;
    }
    push_view(sv.substr(pos, p - pos));
    pos = p + sp.size();
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_contains(
    const char* s, const char* sub) {
  return _str_sv(s).find(_str_sv(sub)) != std::string_view::npos;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_starts_with(
    const char* s, const char* prefix) {
  return _str_sv(s).starts_with(_str_sv(prefix));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_ends_with(
    const char* s, const char* suffix) {
  return _str_sv(s).ends_with(_str_sv(suffix));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_slice(
    const char* s, int64_t start, int64_t end) {
  int64_t size = static_cast<int64_t>(_str_len(s));
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  return _culebra_heap_str(
      std::string_view(s + start, static_cast<size_t>(end - start)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitStringView*
culebra_runtime_strlike_view(int8_t tag, int64_t data) {
  auto sv = _culebra_str_view(tag, data);
  return _culebra_heap_view(sv.data(), sv.size(), _view_owner_base(tag, data));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitStringView*
culebra_runtime_strlike_slice_view(int8_t tag, int64_t data,
                                   int64_t start, int64_t end) {
  auto sv = _culebra_str_view(tag, data);
  int64_t size = static_cast<int64_t>(sv.size());
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  return _culebra_heap_view(sv.data() + start,
                            static_cast<size_t>(end - start),
                            _view_owner_base(tag, data));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char*
culebra_runtime_strlike_to_cstr(int8_t tag, int64_t data) {
  if (tag == TAG_STRING) return reinterpret_cast<const char*>(data);
  auto* v = reinterpret_cast<const JitStringView*>(data);
  return _culebra_heap_str(std::string_view(v->ptr, v->len));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_pop(
    JitArray* arr, int8_t* out_tag, int64_t* out_data) {
  if (arr->size == 0) {
    *out_tag = 0;
    *out_data = 0;
    return;
  }
  auto& last = arr->items[arr->size - 1];
  *out_tag = last.tag;
  *out_data = last.data;
  arr->size--;
}

// Slice with negative indices and clamping. Returns a new array with retained
// elements (independent refcount).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_slice2(
    JitArray* src, int64_t start, int64_t end) {
  int64_t size = static_cast<int64_t>(src->size);
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  auto* r = culebra_runtime_array_new();
  for (int64_t i = start; i < end; i++) {
    auto& e = src->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(r, e.tag, e.data);
  }
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_array_join(
    JitArray* arr, const char* sep) {
  std::string out;
  for (size_t i = 0; i < arr->size; i++) {
    if (i > 0) out += sep;
    auto tag = arr->items[i].tag;
    auto data = arr->items[i].data;
    if (tag == TAG_STRING || tag == TAG_STRINGVIEW) {
      out += _culebra_str_view(tag, data);
    } else {
      out += _culebra_value_to_str_impl(tag, data);
    }
  }
  return _culebra_heap_str(out);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_index_of(
    JitArray* arr, int8_t tag, int64_t data) {
  for (size_t i = 0; i < arr->size; i++) {
    if (_culebra_value_equal(arr->items[i].tag, arr->items[i].data, tag, data))
      return static_cast<int64_t>(i);
  }
  return -1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_array_contains(
    JitArray* arr, int8_t tag, int64_t data) {
  return culebra_runtime_array_index_of(arr, tag, data) >= 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_reverse(JitArray* arr) {
  if (arr->size < 2) return;
  for (size_t i = 0, j = arr->size - 1; i < j; i++, j--) {
    auto tmp = arr->items[i];
    arr->items[i] = arr->items[j];
    arr->items[j] = tmp;
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_object_keys(
    JitObject* obj) {
  auto* r = culebra_runtime_array_new();
  // Walk key_order when populated so String and non-String keys come
  // out interleaved in insertion order. Fall back to the shape walk
  // for objects built via direct slot append (class instances).
  if (obj->key_order && !obj->key_order->empty()) {
    for (auto& k : *obj->key_order) {
      culebra_runtime_value_retain(k.tag, k.data);
      culebra_runtime_array_push(r, k.tag, k.data);
    }
    return r;
  }
  for (size_t i = 0; obj->shape && i < obj->shape->names.size(); i++) {
    auto& k = obj->shape->names[i];
    auto* buf = _culebra_heap_str(k);
    culebra_runtime_array_push(r, TAG_STRING, reinterpret_cast<int64_t>(buf));
  }
  return r;
}

// True if a TAG_OBJECT receiver is a Shared.new view (the O(1) flag).
// Lets compile_user_method_over_builtin route every builtin-named method
// on a view to the interp's "read the field, then fail to call it" path
// instead of running the builtin.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_is_shared_val(
    int64_t data) {
  return reinterpret_cast<JitObject*>(data)->is_shared_val ? 1 : 0;
}

// A Shared.new view exposes only its own reader methods. A dict builtin
// (`get`/`remove`/`get_or_put`) compiled against the view must behave
// like the interp: the name reads from the frozen tree (functions can't
// be in a tree, so the result is data or nil) and calling that fails —
// reproduce that exact TypeError instead of running the builtin (which
// would otherwise read or even MUTATE the handle's marker slots).
[[noreturn]] inline void _jit_shared_val_builtin_reject(JitObject* obj,
                                                        const char* name,
                                                        int64_t line,
                                                        int64_t col) {
  JitValue v = culebra::_jit_shared_val_prop(obj, name, line, col);
  const char* got = _culebra_tag_name(v.tag);
  _culebra_value_release_impl(v.tag, v.data);
  throw culebra::CulebraError(
      "TypeError", culebra::type_mismatch_message("Function", got),
      line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_remove(
    JitObject* obj, const char* key) {
  if (obj->is_shared_val) _jit_shared_val_builtin_reject(obj, "remove", 0, 0);
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) return;
  _culebra_value_release_impl(obj->slots[idx].value.tag,
                              obj->slots[idx].value.data);
  obj->erase(key);
  if (std::string_view(key) == "drop") _jit_owned_unbind_drop(obj);
}

// Drop a key from `key_order` if present. Matches by JitValueEq so
// numerically-equivalent keys (e.g. `1`/`1.0`/`true`) all resolve to
// the same slot — same rule the AnyKeyMap uses on insert.
inline void _key_order_erase(JitObject* obj, const JitValue& key) {
  if (!obj->key_order) return;
  JitValueEq eq;
  auto& ko = *obj->key_order;
  for (auto it = ko.begin(); it != ko.end(); ++it) {
    if (eq(*it, key)) {
      // Refcounted (Tuple) keys hold a +1 here — release before erasing.
      if (_is_refcounted_value_tag(it->tag)) {
        _culebra_value_release_impl(it->tag, it->data);
      }
      ko.erase(it);
      return;
    }
  }
}

// Generic remove. String keys go through the shape (TAG_STRING data
// is a borrowed cstring, no refcount to manage); non-String keys go
// through the sidecar where the caller's +1 to the lookup key is
// consumed, and the stored map-entry key's +1 (transferred from the
// original `object_set_any` insert) is also released before erase.
// `_key_order_erase` drops the key_order entry's +1 separately.
// dict.get(key, fallback): read-only. Returns the stored value (retained +1)
// for `key`, else `fallback`. Never mutates the object. Consumes the key's +1
// and, on a hit, the fallback's +1 — mirroring the interp builtin's ownership.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_object_get_default(
    JitObject* obj, int8_t kt, int64_t kd, int8_t ft, int64_t fd,
    int64_t line, int64_t col) {
  if (obj->is_shared_val) {
    _culebra_value_release_impl(ft, fd);
    if (kt != TAG_STRING) _culebra_value_release_impl(kt, kd);
    _jit_shared_val_builtin_reject(obj, "get", line, col);
  }
  (void)line;
  (void)col;
  std::string kbuf;
  _jit_normalize_str_key(kt, kd, kbuf);
  bool found = false;
  JitValue stored{TAG_NIL, 0};
  if (kt == TAG_STRING) {
    auto idx = obj->find_slot(reinterpret_cast<const char*>(kd));
    if (idx != static_cast<size_t>(-1)) {
      stored = obj->slots[idx].value;
      found = true;
    }
  } else if (obj->non_string_props) {
    auto it = obj->non_string_props->find(JitValue{kt, kd});
    if (it != obj->non_string_props->end()) {
      stored = it->second.value;
      found = true;
    }
  }
  // Release the key once: no-op for a borrowed String/Long, frees a refcounted
  // (e.g. Tuple) key; a StringView was already released by the normalize above.
  _culebra_value_release_impl(kt, kd);
  if (found) {
    culebra_runtime_value_retain(stored.tag, stored.data);
    _culebra_value_release_impl(ft, fd);  // fallback unused
    return stored;
  }
  return JitValue{ft, fd};  // transfer the fallback's +1 to the caller
}

// dict.get_or_put(key, init): return the value for `key`; on a miss, store
// `init` and return it (sharing storage, so the accumulator idiom
// `d.get_or_put(k, || []).push(x)` grows the dict's array). `init` is invoked
// lazily when it is a function — only a miss pays for it.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_object_get_or_put(
    JitObject* obj, int8_t kt, int64_t kd, int8_t it_tag, int64_t it_data,
    int64_t line, int64_t col) {
  if (obj->is_shared_val) {
    _culebra_value_release_impl(it_tag, it_data);
    if (kt != TAG_STRING) _culebra_value_release_impl(kt, kd);
    _jit_shared_val_builtin_reject(obj, "get_or_put", line, col);
  }
  std::string kbuf;
  _jit_normalize_str_key(kt, kd, kbuf);
  bool found = false;
  JitValue stored{TAG_NIL, 0};
  if (kt == TAG_STRING) {
    auto idx = obj->find_slot(reinterpret_cast<const char*>(kd));
    if (idx != static_cast<size_t>(-1)) {
      stored = obj->slots[idx].value;
      found = true;
    }
  } else if (obj->non_string_props) {
    auto iter = obj->non_string_props->find(JitValue{kt, kd});
    if (iter != obj->non_string_props->end()) {
      stored = iter->second.value;
      found = true;
    }
  }
  if (found) {
    culebra_runtime_value_retain(stored.tag, stored.data);
    _culebra_value_release_impl(it_tag, it_data);  // init unused
    _culebra_value_release_impl(kt, kd);           // release key; no-op for String/Long
    return stored;
  }
  // Miss: evaluate init lazily when it is a function (zero-arg thunk).
  JitValue v;
  if (it_tag == TAG_FUNC) {
    v = culebra_runtime_call_with_kwargs(
        reinterpret_cast<JitClosure*>(it_data), int8_t{TAG_NIL}, int64_t{0}, 0,
        nullptr, 0, nullptr, nullptr, 0, nullptr, line, col);
    _culebra_value_release_impl(it_tag, it_data);  // release the closure
  } else {
    v = JitValue{it_tag, it_data};  // use as-is (already +1)
  }
  // Two refs: one consumed by the slot store, one returned to the caller.
  // Immutable slot, matching a normal dict entry (`d[k] = v`); the value can
  // still be mutated in place (e.g. `.push`).
  culebra_runtime_value_retain(v.tag, v.data);
  culebra_runtime_object_set_any(obj, kt, kd, /*mut*/ false, v.tag, v.data,
                                 line, col, /*is_init*/ false);
  return v;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_remove_any(
    JitObject* obj, int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (obj->is_shared_val) {
    if (tag != TAG_STRING) _culebra_value_release_impl(tag, data);
    _jit_shared_val_builtin_reject(obj, "remove", line, col);
  }
  std::string _kbuf;
  _jit_normalize_str_key(tag, data, _kbuf);
  if (tag == TAG_STRING) {
    auto* cstr = reinterpret_cast<const char*>(data);
    if (obj->find_slot(cstr) != static_cast<size_t>(-1)) {
      culebra_runtime_object_remove(obj, cstr);
      _key_order_erase(obj, {TAG_STRING, data});
    }
    return;
  }
  JitValue key{tag, data};
  if (!obj->non_string_props) {
    // No non-string keys stored yet, but the interpreter's sidecar still
    // hashes the key on every remove — an unhashable key (Function, Array,
    // ...) must raise here too rather than silently no-op.
    _culebra_hash_at(line, col, [&] { return JitValueHash{}(key); });
    _culebra_value_release_impl(tag, data);
    return;
  }
  auto it = _culebra_hash_at(
      line, col, [&] { return obj->non_string_props->find(key); });
  if (it == obj->non_string_props->end()) {
    _culebra_value_release_impl(tag, data);
    return;
  }
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  _culebra_value_release_impl(it->first.tag, it->first.data);
  obj->non_string_props->erase(it);
  _key_order_erase(obj, key);
  _culebra_value_release_impl(tag, data);
}

}  // extern "C" (close briefly for C++ impl helpers)

// --- to_set / group_by / partition ---------------------------------------
//
// Each is written once over an element source. A source hands `emit` a value
// that already carries a +1, so every sink below simply consumes it (stores
// it or lets a guard release it) without caring which receiver it came from:
// the Array source retains each borrowed element, the iterator source passes
// the +1 its pull produced.

template <typename Emit>
inline void _each_of_jit_array(JitArray* arr, Emit emit) {
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    emit(e);
  }
}

template <typename Emit>
inline void _each_of_jit_iter(int8_t it, int64_t id, Emit emit) {
  JitValue v;
  auto* has_next_cls = _iter_has_next_closure({it, id});
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(has_next_cls, next_cls, {it, id}, v)) emit(v);
}

// Duplicates collapse; first-seen order is kept. set_add absorbs the +1 on
// insert and releases it on a duplicate, so the guard only covers the throw
// edge (an unhashable element).
template <typename Each>
inline JitSet* _collect_set_jit(Each each, int64_t line, int64_t col) {
  auto* out = culebra_runtime_set_new();
  JitOwnedVal out_guard(JitValue{TAG_SET, reinterpret_cast<int64_t>(out)});
  each([&](JitValue v) {
    JitOwnedVal vg(v);
    _culebra_hash_at(line, col, [&] {
      culebra_runtime_set_add(out, v.tag, v.data);
      return 0;
    });
    vg.consume();
  });
  out_guard.consume();
  return out;
}

// Buckets are Arrays in first-seen key order. The key's hashability is
// checked up front so an unhashable key reports at this call site (and so
// neither the key nor the fresh bucket is stranded when it does).
template <typename Each>
inline JitObject* _collect_group_by_jit(Each each, int8_t ft, int64_t fd,
                                        int64_t line, int64_t col) {
  JitHofCallback fn(ft, fd, 1, "group_by", "f", line, col);
  auto* out = culebra_runtime_object_new();
  JitOwnedVal out_guard(JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(out)});
  each([&](JitValue v) {
    JitOwnedVal vg(v);
    culebra_runtime_value_retain(v.tag, v.data);  // the call consumes one
    auto k = _culebra_invoke1_at(fn, v, line, col);
    JitOwnedVal kg(k);
    auto* fresh = culebra_runtime_array_new();
    JitOwnedVal fg(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(fresh)});
    _culebra_hash_at(line, col, [&] { return JitValueHash{}(k); });
    kg.consume();
    fg.consume();
    // get_or_put consumes the key and the fresh bucket, and hands back the
    // stored bucket with a +1 (the fresh one when this key is new).
    auto bucket = culebra_runtime_object_get_or_put(
        out, k.tag, k.data, TAG_ARRAY, reinterpret_cast<int64_t>(fresh),
        line, col);
    JitOwnedVal bg(bucket);
    auto owned = vg.consume();
    culebra_runtime_array_push(reinterpret_cast<JitArray*>(bucket.data),
                               owned.tag, owned.data);
  });
  out_guard.consume();
  return out;
}

// One pass into `(matching, non_matching)`, order preserved in both halves.
template <typename Each>
inline JitArray* _collect_partition_jit(Each each, int8_t pt, int64_t pd,
                                        int64_t line, int64_t col) {
  JitHofCallback fn(pt, pd, 1, "partition", "p", line, col);
  auto* yes = culebra_runtime_array_new();
  JitOwnedVal yg(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(yes)});
  auto* no = culebra_runtime_array_new();
  JitOwnedVal ng(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(no)});
  each([&](JitValue v) {
    JitOwnedVal vg(v);
    culebra_runtime_value_retain(v.tag, v.data);  // the call consumes one
    auto r = _culebra_invoke1_at(fn, v, line, col);
    JitOwnedVal rg(r);
    bool hit = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    auto owned = vg.consume();
    culebra_runtime_array_push(hit ? yes : no, owned.tag, owned.data);
  });
  auto* out = culebra_runtime_tuple_new();
  culebra_runtime_tuple_push(out, TAG_ARRAY, yg.consume().data);
  culebra_runtime_tuple_push(out, TAG_ARRAY, ng.consume().data);
  return out;
}

// The six exported entry points; the templates above stay C++.
extern "C" {

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_iter_to_set(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  return _collect_set_jit(
      [&](auto emit) { _each_of_jit_iter(it, id, emit); }, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_array_to_set(
    JitArray* arr, int64_t line, int64_t col) {
  return _collect_set_jit(
      [&](auto emit) { _each_of_jit_array(arr, emit); }, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_iter_group_by(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _collect_group_by_jit(
      [&](auto emit) { _each_of_jit_iter(it, id, emit); }, ft, fd, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_array_group_by(
    JitArray* arr, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  return _collect_group_by_jit(
      [&](auto emit) { _each_of_jit_array(arr, emit); }, ft, fd, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_iter_partition(
    int8_t it, int64_t id, int8_t pt, int64_t pd, int64_t line, int64_t col) {
  return _collect_partition_jit(
      [&](auto emit) { _each_of_jit_iter(it, id, emit); }, pt, pd, line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_partition(
    JitArray* arr, int8_t pt, int64_t pd, int64_t line, int64_t col) {
  return _collect_partition_jit(
      [&](auto emit) { _each_of_jit_array(arr, emit); }, pt, pd, line, col);
}

}  // extern "C"

#endif  // CULEBRA_JIT_ENABLED
