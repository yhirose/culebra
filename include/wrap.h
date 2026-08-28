#pragma once
// Declarative C++ class wrapping — Phase 4.
//
// The "codegen" is the C++ compiler, pybind11-style: a thin declaration
// instantiates the glue (conversion thunks, method prototypes, Foreign
// registration) at compile time, and a static initializer records the
// class in the process-wide registry the stdlib setup walks:
//
//   // In a .cpp, at file scope. NEVER in a header: an inline variable is a
//   // COMDAT nothing odr-uses, need never be initialized at all
//   // ([basic.start.dynamic]), and lld — the Windows linker — drops it with
//   // the registration inside. [[gnu::used]] does not rescue it (measured):
//   // what runs the initializer is a separate static-init entry, associative
//   // to that COMDAT and collected with it. A .cpp's file-scope variable is
//   // not a COMDAT, so its entry stays. A header that wants to carry the
//   // declaration puts it in a function and lets each .cpp call it — as
//   // include/foreign_binding.h does.
//   namespace {
//   const bool registered = [] {
//     culebra::wrap<mylib::Counter>("__Foreign", "Counter")
//         .ctor<long>({"start"})
//         .method<&mylib::Counter::value>("value")
//         .method<&mylib::Counter::add>("add", {"n"})
//         .static_method<&mylib::Counter::live>("live");
//     return true;
//   }();
//   }
//
// Instances ride the Foreign id table (foreign.h): `drop` erases the
// entry (~T() runs NOW), a dropped instance's method raises ClosedError,
// and the handle — an ordinary drop-having Object — gets the whole
// Phase 1/2 lifetime machinery (scope-exit drop, cycles, GC backstop,
// exactly-once) for free. Return values lower by ownership shape: a by-value or
// unique_ptr<U> return becomes an owning handle of U, shared_ptr<U>
// holds one share, primitives lower to their tagged values. U must itself be
// wrapped (its jit_class_info carries the display name the handle shows).

#include <fn_traits.h>
#include <foreign.h>
#include <wrap_registry.h>
// wrap.h can be reached before culebra.h pulls the runtime layer, so
// include it here.
#include <rt.h>

#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace culebra {

namespace _detail {

// Type annotation matching Culebra's "Bool"/"Long"/"Float"/"String"
// names. Empty for types that don't map cleanly — an empty annotation
// then means "no annotation" (any).
template <class T> constexpr std::string_view type_annotation_for() { return {}; }
template <> constexpr std::string_view type_annotation_for<long>()              { return "Long"; }
template <> constexpr std::string_view type_annotation_for<long long>()         { return "Long"; }
template <> constexpr std::string_view type_annotation_for<int>()               { return "Long"; }
template <> constexpr std::string_view type_annotation_for<double>()            { return "Float"; }
template <> constexpr std::string_view type_annotation_for<float>()             { return "Float"; }
template <> constexpr std::string_view type_annotation_for<bool>()              { return "Bool"; }
template <> constexpr std::string_view type_annotation_for<std::string>()       { return "String"; }
template <> constexpr std::string_view type_annotation_for<std::string_view>()  { return "String"; }
template <> constexpr std::string_view type_annotation_for<const std::string&>(){ return "String"; }

}  // namespace _detail

namespace wrap_detail {

template <class T>
struct is_unique_ptr : std::false_type {};
template <class U, class D>
struct is_unique_ptr<std::unique_ptr<U, D>> : std::true_type {};
template <class T>
struct is_shared_ptr : std::false_type {};
template <class U>
struct is_shared_ptr<std::shared_ptr<U>> : std::true_type {};

// The Culebra-visible return annotation: handles surface as "Object",
// primitives via type_annotation_for, void/unknown stay unannotated.
template <class R>
constexpr std::string_view return_annotation() {
  using D = std::decay_t<R>;
  if constexpr (std::is_void_v<D>) {
    return {};
  } else if constexpr (is_unique_ptr<D>::value || is_shared_ptr<D>::value) {
    return "Object";
  } else if constexpr (!_detail::type_annotation_for<D>().empty()) {
    return _detail::type_annotation_for<D>();
  } else if constexpr (std::is_same_v<D, const char*>) {
    return {};
  } else {
    return "Object";
  }
}

// Surface a wrapped body's own C++ exception (a ctor refusing construction, a
// method reporting failure) as a RuntimeError the script can catch — without
// this the exception would escape the process. Position stays 0:0, backfilled
// at the engine boundary like every positionless lib helper; CulebraError
// passes through untouched. The conversion is outlined and non-templated on purpose:
// with the handlers inside the template, GCC outlined the whole wrapper at
// every instantiation (~93 KB of duplicated catch code per archive) and the
// wrapped body stopped inlining.
[[noreturn]] inline void rethrow_as_culebra() {
  try {
    throw;
  } catch (const culebra::CulebraError&) {
    throw;
  } catch (const std::exception& e) {
    throw culebra::CulebraError("RuntimeError", e.what(), 0, 0);
  }
}
template <class F>
auto surface_native_error(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (...) {
    rethrow_as_culebra();
  }
}

// Fill omitted parameter names with `_arg<i>`. Every consumer copies the
// names into storage of its own (jit_method_info, the handle meta, the ns
// registry rows), so plain value return is the whole contract.
inline std::vector<std::string> pin_param_names(std::vector<std::string> names,
                                                size_t arity) {
  names.resize(arity);
  for (size_t i = 0; i < arity; i++) {
    if (names[i].empty()) names[i] = "_arg" + std::to_string(i);
  }
  return names;
}

// --- The thunks and registry rows --------------------------------------------
//
// The declaration instantiates per-method handle thunks (plain functions —
// the member-fn pointer is a NON-TYPE template argument, so each method gets
// its own static thunk, exactly the hand-written Phase 3 shape), ns adapters
// for the NsMethod-style dispatch, and a row registry stdlib_jit.h merges
// with its static table. Type checks and error positions follow the Phase 3
// thunk conventions: param type-check BEFORE the closed check, TypeError at
// the argument's threaded position (argpos / call_arg0 fallback),
// ClosedError at the call site.

// Annotation check through the canonical matcher — the SAME predicate the
// ns-method trampoline applies to ctor/static params, so a String param
// rejects a StringView slice identically on every path.
template <class A>
inline bool jit_arg_matches(const JitValue& v) {
  return _culebra_value_matches_type(
      v.tag, v.data, _detail::type_annotation_for<std::decay_t<A>>());
}

template <class A>
inline std::decay_t<A> jit_arg_get(const JitValue& v) {
  using D = std::decay_t<A>;
  if constexpr (std::is_integral_v<D> && !std::is_same_v<D, bool>) {
    return static_cast<D>(v.data);
  } else if constexpr (std::is_same_v<D, double> || std::is_same_v<D, float>) {
    double d;
    std::memcpy(&d, &v.data, sizeof(double));
    return static_cast<D>(d);
  } else if constexpr (std::is_same_v<D, bool>) {
    return v.data != 0;
  } else {
    return D(_culebra_str_view(v.tag, v.data));
  }
}

// Per-(T, method) param names for the thunk's error wording. Filled by
// the binder at static-init; constinit for the same ordering reason as
// jit_class_info.
template <class T, auto Mf>
struct jit_method_info {
  static constinit inline std::vector<std::string> param_names;
};

// Per-T binding info, filled once by the binder's static-init run: the
// display name handles carry, and the method table the handle builder
// consumes. `meta` carries the param names so a method binds keyword
// arguments (null for the auto-bound `drop`). constinit pins the
// constant-initialization the registration scheme relies on: the binder's
// dynamic initializer writes into these, so they must never be dynamically
// (re)initialized themselves — templated statics have no ordering guarantee
// against other TUs' initializers.
template <class T>
struct jit_class_info {
  struct Method {
    std::string name;
    void (*thunk)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*);
    size_t arity;
    const JitParamMeta* meta;
  };
  static constinit inline std::string name;
  static constinit inline std::vector<Method> methods;
};

// Generation of a handle, or -1 when it is no longer valid. Both
// owning (_state_fn + table) and borrowing (_bid + borrow table) read
// through an indirection — a forged slot misses, never derefs.
inline int64_t jit_handle_gen(JitObject* h) {
  constexpr size_t npos = static_cast<size_t>(-1);
  if (h->find_slot("_state_fn") != npos) {
    return foreign::owner_gen_via(_jit_handle_long(h, "_state_fn"),
                                  _jit_handle_long(h, "_id"));
  }
  if (h->find_slot("_bid") != npos)
    return foreign::borrow_gen(_jit_handle_long(h, "_bid"));
  return -1;
}

// The live instance, or ClosedError at the method call site. A borrowing
// handle resolves its raw pointer from the borrow table only after the
// parent chain validates.
template <class T>
inline T* jit_handle_self(JitValue self) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  if (h->find_slot("_bid") != static_cast<size_t>(-1)) {
    int64_t bid = _jit_handle_long(h, "_bid");
    if (!foreign::borrow_valid(bid))
      foreign::throw_borrow_invalid(jit_class_info<T>::name,
                                    _jit_call_site_line, _jit_call_site_col);
    return reinterpret_cast<T*>(foreign::borrow_ptr(bid));
  }
  return foreign::get_or_throw<T>(_jit_handle_long(h, "_id"),
                                  jit_class_info<T>::name, _jit_call_site_line,
                                  _jit_call_site_col);
}

// Non-const method dispatch bumps the instance's generation, staling its
// outstanding borrows.
template <class T>
inline void jit_bump_handle_gen(JitValue self) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  if (h->find_slot("_bid") != static_cast<size_t>(-1)) {
    foreign::borrow_bump(_jit_handle_long(h, "_bid"));
  } else {
    foreign::table<T>().bump_gen(_jit_handle_long(h, "_id"));
  }
}

template <class T>
JitValue jit_make_handle(int64_t id);

// Return-value lowering: handles for wrapped class values / unique_ptr /
// shared_ptr, tagged scalars and strings for the rest.
template <class R>
inline JitValue jit_lower_return(R r) {
  using D = std::decay_t<R>;
  if constexpr (is_unique_ptr<D>::value) {
    using U = typename D::element_type;
    auto id = foreign::table<U>().adopt(std::move(r));
    return jit_make_handle<U>(id);
  } else if constexpr (is_shared_ptr<D>::value) {
    using U = typename D::element_type;
    auto id = foreign::table<U>().adopt_shared(std::move(r));
    return jit_make_handle<U>(id);
  } else if constexpr (std::is_integral_v<D> && !std::is_same_v<D, bool>) {
    return {TAG_LONG, static_cast<int64_t>(r)};
  } else if constexpr (std::is_same_v<D, double> || std::is_same_v<D, float>) {
    double d = static_cast<double>(r);
    int64_t bits;
    std::memcpy(&bits, &d, sizeof(double));
    return {TAG_FLOAT, bits};
  } else if constexpr (std::is_same_v<D, bool>) {
    return {TAG_BOOL, r ? 1 : 0};
  } else if constexpr (std::is_same_v<D, std::string> ||
                       std::is_same_v<D, std::string_view> ||
                       std::is_same_v<D, const char*>) {
    return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(r))};
  } else {
    static_assert(!std::is_reference_v<R>,
                  "wrap.h: a method returning T& / const T& of a wrapped "
                  "class has no ownership shape — return T, unique_ptr<T>, "
                  "or shared_ptr<T> (borrowing lands in Phase 5)");
    auto id = foreign::table<D>().adopt(std::make_unique<D>(std::move(r)));
    return jit_make_handle<D>(id);
  }
}

// The drop event — runs from the destructor's drop protocol, which
// passes self WITHOUT a +1: must not release it (Phase 3 convention).
template <class T>
void jit_drop_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                        int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  foreign::table<T>().erase(_jit_handle_long(h, "_id"));
  { *__ret = {TAG_NIL, 0}; return; }
}

template <class T>
JitValue jit_make_handle(int64_t id) {
  // Multi-object construction: the method closures are unrooted until
  // slotted into `h`, so a GC_STRESS collect mid-build would sweep them.
  culebra::gc::Heap::CollectPause pause(_gc_heap());
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__foreign__",
                   JitValue{TAG_STRING, reinterpret_cast<int64_t>(_intern_str(
                                            jit_class_info<T>::name))},
                   false);
  h->set_or_append("_id", JitValue{TAG_LONG, id}, false);
  h->set_or_append("_state_fn",
                   JitValue{TAG_LONG, foreign::state_fn_id<T>()}, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  for (const auto& m : jit_class_info<T>::methods) {
    _jit_handle_bind_method(h, m.name.c_str(), m.thunk, m.arity, m.meta);
  }
  _jit_handle_bind_method(h, "drop", &jit_drop_thunk<T>, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// The borrow id of a JIT parent handle; -1 owner_id / state for a
// borrowing parent (chained borrow).
inline void jit_parent_link(JitObject* parent, int64_t* state,
                           int64_t* owner_id, int64_t* bid) {
  if (parent->find_slot("_bid") != static_cast<size_t>(-1)) {
    *state = -1;
    *owner_id = 0;
    *bid = _jit_handle_long(parent, "_bid");
  } else {
    *state = _jit_handle_long(parent, "_state_fn");
    *owner_id = _jit_handle_long(parent, "_id");
    *bid = -1;
  }
}

// Internal drop for a borrow handle: erase its borrow-table id. Like
// every drop thunk it must NOT release self (the destructor protocol
// passes self without a +1). Does not touch the parent's resource.
template <class T2>
void jit_borrow_drop_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                               int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  foreign::borrow_erase(_jit_handle_long(h, "_bid"));
  { *__ret = {TAG_NIL, 0}; return; }
}

// Borrowing handle, JIT side — same shape as the interp's: opaque _bid
// (raw ptr + parent link in the borrow table), __parent__ for liveness,
// an internal drop that erases the id, registered on the owned stack so
// the table entry is reclaimed deterministically.
template <class T2>
JitValue jit_make_borrow_handle(T2* p, JitValue parent, int64_t pgen) {
  culebra::gc::Heap::CollectPause pause(_gc_heap());
  auto* pobj = reinterpret_cast<JitObject*>(parent.data);
  int64_t state = -1, owner_id = 0, parent_bid = -1;
  jit_parent_link(pobj, &state, &owner_id, &parent_bid);
  int64_t bid = foreign::borrow_adopt(p, state, owner_id, parent_bid, pgen);

  auto* h = culebra_runtime_object_new();
  h->set_or_append("__foreign__",
                   JitValue{TAG_STRING, reinterpret_cast<int64_t>(_intern_str(
                                            jit_class_info<T2>::name))},
                   false);
  h->set_or_append("_bid", JitValue{TAG_LONG, bid}, false);
  culebra_runtime_value_retain(parent.tag, parent.data);
  h->set_or_append("__parent__", parent, false);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  for (const auto& m : jit_class_info<T2>::methods) {
    _jit_handle_bind_method(h, m.name.c_str(), m.thunk, m.arity, m.meta);
  }
  _jit_handle_bind_method(h, "drop", &jit_borrow_drop_thunk<T2>, 0);
  _jit_owned_bind_drop(h);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Binder-order param checks shared by the method and borrowed thunks:
// a missing required argument is an ArityError at the call site, a
// wrong-typed one a TypeError at the argument's threaded position —
// both released `fn` first.
template <class T, auto Mf, class... Args>
inline void jit_check_args(JitValue self, int64_t n, JitValue* args) {
  size_t bad = sizeof...(Args);
  bool missing = false;
  {
    size_t i = 0;
    auto check_one = [&](bool present, bool ok) {
      if (bad == sizeof...(Args)) {
        if (!present) {
          bad = i;
          missing = true;
        } else if (!ok) {
          bad = i;
        }
      }
      ++i;
    };
    (check_one(static_cast<int64_t>(i) < n,
               static_cast<int64_t>(i) < n && jit_arg_matches<Args>(args[i])),
     ...);
  }
  if (bad < sizeof...(Args)) {
    culebra_runtime_value_release(self.tag, self.data);
    const auto& names = jit_method_info<T, Mf>::param_names;
    if (missing) {
      throw culebra::CulebraError(
          "ArityError", culebra::missing_required_arg_message(names[bad]),
          _jit_call_site_line, _jit_call_site_col);
    }
    static constexpr std::string_view kAnnos[] = {
        _detail::type_annotation_for<Args>()...};
    const bool ap = static_cast<int>(bad) < _jit_argpos_n;
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("type error: parameter '{}' expects {}", names[bad],
                        kAnnos[bad]),
        ap ? _jit_argpos_line[bad]
           : (bad == 0 ? _jit_call_arg0_line : _jit_call_site_line),
        ap ? _jit_argpos_col[bad]
           : (bad == 0 ? _jit_call_arg0_col : _jit_call_site_col));
  }
}

// The thunk twin of surface_native_error: the same conversion, plus the
// call-site position the thunk ABI has no room for. The ns adapters below keep
// the plain form — their trampoline backfills from a line/col it was handed,
// which a stamp here (from whatever call published last) would beat to it.
// Outlined and non-templated for the reason its twin is, and so the hot path
// doesn't read the call-site thread-locals it only needs when throwing.
[[noreturn]] inline void rethrow_as_culebra_at_call_site() {
  try {
    rethrow_as_culebra();
  } catch (culebra::CulebraError& e) {
    _jit_backfill_error_pos(e, _jit_call_site_line, _jit_call_site_col);
    throw;
  }
}
template <class F>
auto surface_native_error_at_call_site(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (...) {
    rethrow_as_culebra_at_call_site();
  }
}

// One handle method thunk per (T, member fn). Self arrives +1, released on
// every exit by the guard below — an early release on a temp receiver would
// fire its drop and erase the table entry under our feet, so the guard runs at
// scope exit, after the body. `jit_check_args` still owns its own binder-throw
// release (it runs before the guard is armed); the guard closes the previously
// leaking ClosedError (jit_handle_self) and body-throw paths.
// `Bump` (a non-const method without preserves_borrows) increments the
// generation after validation, BEFORE the call.
template <class T, auto Mf, class R, bool Bump, class... Args>
void jit_method_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                          int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  jit_check_args<T, Mf, Args...>(self, n, args);
  JitMethodSelf _s{self};
  T* obj = jit_handle_self<T>(self);
  if constexpr (Bump) jit_bump_handle_gen<T>(self);
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
    if constexpr (std::is_void_v<R>) {
      (obj->*Mf)(jit_arg_get<Args>(args[I])...);
      return {TAG_NIL, 0};
    } else {
      auto&& r = (obj->*Mf)(jit_arg_get<Args>(args[I])...);
      return jit_lower_return<R>(std::forward<decltype(r)>(r));
    }
  };
  *__ret = surface_native_error_at_call_site(
      [&] { return invoke(std::index_sequence_for<Args...>{}); });
}

// A borrowed-return method thunk: the parent's generation is
// snapshotted after validation, and the result is a borrowing handle.
// Taking a borrow never bumps.
template <class T, auto Mf, class T2, class... Args>
void jit_borrowed_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                            int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  jit_check_args<T, Mf, Args...>(self, n, args);
  // Release self on every exit (see jit_method_thunk). The borrow handle keeps
  // its own reference to self as its parent, so the scope-exit release — after
  // the handle is built and returned — leaves the borrow valid while closing
  // the ClosedError / body-throw strand.
  JitMethodSelf _s{self};
  T* obj = jit_handle_self<T>(self);
  int64_t pgen = jit_handle_gen(reinterpret_cast<JitObject*>(self.data));
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
    auto& r = (obj->*Mf)(jit_arg_get<Args>(args[I])...);
    return jit_make_borrow_handle<T2>(const_cast<T2*>(&r), self, pgen);
  };
  *__ret = surface_native_error_at_call_site(
      [&] { return invoke(std::index_sequence_for<Args...>{}); });
}

// ns adapters: reached through the ns-method trampoline, which has
// already arity- and type-checked every positional against the
// CANONICAL interp params (the calling-convention single source), so
// these convert and call.
template <class T, class... Args>
JitValue jit_ctor_adapter(JitValue* a, int64_t) {
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) {
    return foreign::table<T>().adopt(
        std::make_unique<T>(jit_arg_get<Args>(a[I])...));
  };
  return jit_make_handle<T>(surface_native_error(
      [&] { return invoke(std::index_sequence_for<Args...>{}); }));
}

template <auto Fn, class R, class... Args>
JitValue jit_static_adapter(JitValue* a, int64_t) {
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
    if constexpr (std::is_void_v<R>) {
      Fn(jit_arg_get<Args>(a[I])...);
      return {TAG_NIL, 0};
    } else {
      return jit_lower_return<R>(Fn(jit_arg_get<Args>(a[I])...));
    }
  };
  return surface_native_error(
      [&] { return invoke(std::index_sequence_for<Args...>{}); });
}

}  // namespace wrap_detail

// (WrappedNsRow and its registry moved to wrap_registry.h in Phase 4
// B7-b, so the compiled lanes read them without this header.)

// Per-method dispatch policy. `standard` derives the generation
// bump from C++ const-ness (non-const = bump); `preserves_borrows` is
// the author's opt-out for non-const methods that don't invalidate
// outstanding borrows. Misdeclaring const-ness or this flag is a
// declaration-contract violation (sol2/pybind11-style author
// responsibility) — the failure mode is a stale-borrow error or a
// missed one, never UB on the culebra side.
enum class wrap_policy { standard, preserves_borrows };

namespace wrap_detail {
template <class M>
struct is_const_member : std::false_type {};
template <class R, class C, class... A>
struct is_const_member<R (C::*)(A...) const> : std::true_type {};
}  // namespace wrap_detail

// Builder. Destruction finalizes: the class registers under (ns, name)
// (wrap_registry.h), and its ctor / statics land in the shared ns-row
// registry stdlib_jit.h merges with its static table.
template <class T>
class ClassBinder {
 public:
  ClassBinder(std::string ns, std::string name) : ns_(std::move(ns)) {
    assert(wrap_detail::jit_class_info<T>::name.empty() &&
           "wrap<T>: this class is already wrapped");
    wrap_detail::jit_class_info<T>::name = name;
    name_ = std::move(name);
  }

  // Constructor: `Ns.Name.new(args...)` → owning handle. The ns adapter
  // returns unique_ptr<T> — jit_lower_return's adopt branch does the rest.
  template <class... Args>
  ClassBinder& ctor(std::vector<std::string> names = {}) {
    push_ns_row<std::unique_ptr<T>, Args...>(
        "new", &wrap_detail::jit_ctor_adapter<T, Args...>, std::move(names));
    return *this;
  }

  // The member fn is a NON-TYPE template argument so the JIT side gets a
  // distinct static thunk per method (the closure ABI needs a plain
  // function pointer). A non-const method bumps the instance's generation (staling its
  // borrows) unless declared wrap_policy::preserves_borrows.
  template <auto Mf>
  ClassBinder& method(std::string name, std::vector<std::string> names = {},
                      wrap_policy policy = wrap_policy::standard) {
    using traits = culebra::fn_traits<decltype(Mf)>;
    return method_impl<Mf, typename traits::ret>(
        std::move(name), std::move(names), policy,
        static_cast<typename traits::args*>(nullptr));
  }

  // A method returning a reference INTO self (`T2&` / `const T2&` of a
  // wrapped class): the result is a borrowing handle validated against
  // this instance's closed flag and generation on every access.
  // Taking a borrow never bumps the generation.
  template <auto Mf>
  ClassBinder& borrowed_method(std::string name,
                               std::vector<std::string> names = {}) {
    using traits = culebra::fn_traits<decltype(Mf)>;
    using R = typename traits::ret;
    static_assert(std::is_lvalue_reference_v<R>,
                  "wrap.h: borrowed_method needs a method returning T2& / "
                  "const T2& of a wrapped class");
    using T2 = std::remove_cv_t<std::remove_reference_t<R>>;
    static_assert(std::is_class_v<T2>,
                  "wrap.h: borrowed_method's referent must be a wrapped "
                  "class");
    return borrowed_impl<Mf, T2>(std::move(name), std::move(names),
                                 static_cast<typename traits::args*>(nullptr));
  }

  template <auto Fn>
  ClassBinder& static_method(std::string name,
                             std::vector<std::string> names = {}) {
    using traits = culebra::fn_traits<decltype(Fn)>;
    return static_impl<Fn, typename traits::ret>(
        std::move(name), std::move(names),
        static_cast<typename traits::args*>(nullptr));
  }

  ~ClassBinder() { wrapped_class_names().push_back({ns_, name_}); }

  ClassBinder(const ClassBinder&) = delete;
  ClassBinder& operator=(const ClassBinder&) = delete;

 private:
  template <auto Mf, class R, class... Args>
  ClassBinder& method_impl(std::string name, std::vector<std::string> names,
                           wrap_policy policy, std::tuple<Args...>*) {
    const bool bump =
        !wrap_detail::is_const_member<decltype(Mf)>::value &&
        policy != wrap_policy::preserves_borrows;
    auto pinned = wrap_detail::pin_param_names(std::move(names),
                                               sizeof...(Args));
    wrap_detail::jit_method_info<T, Mf>::param_names = pinned;
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(pinned,
                                    std::vector<bool>(sizeof...(Args), false));
    wrap_detail::jit_class_info<T>::methods.push_back(
        {std::move(name),
         bump ? &wrap_detail::jit_method_thunk<T, Mf, R, true, Args...>
              : &wrap_detail::jit_method_thunk<T, Mf, R, false, Args...>,
         sizeof...(Args), meta});
    return *this;
  }

  template <auto Mf, class T2, class... Args>
  ClassBinder& borrowed_impl(std::string name, std::vector<std::string> names,
                             std::tuple<Args...>*) {
    auto pinned = wrap_detail::pin_param_names(std::move(names),
                                               sizeof...(Args));
    wrap_detail::jit_method_info<T, Mf>::param_names = pinned;
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(pinned,
                                    std::vector<bool>(sizeof...(Args), false));
    wrap_detail::jit_class_info<T>::methods.push_back(
        {std::move(name),
         &wrap_detail::jit_borrowed_thunk<T, Mf, T2, Args...>,
         sizeof...(Args), meta});
    return *this;
  }

  template <auto Fn, class R, class... Args>
  ClassBinder& static_impl(std::string name, std::vector<std::string> names,
                           std::tuple<Args...>*) {
    push_ns_row<R, Args...>(std::move(name),
                            &wrap_detail::jit_static_adapter<Fn, R, Args...>,
                            std::move(names));
    return *this;
  }

  template <class R, class... Args>
  void push_ns_row(std::string method_name,
                   JitValue (*adapter)(JitValue*, int64_t),
                   std::vector<std::string> names) {
    auto pinned =
        wrap_detail::pin_param_names(std::move(names), sizeof...(Args));
    std::string arg0_type, arg0_name;
    if constexpr (sizeof...(Args) > 0) {
      using A0 = std::tuple_element_t<0, std::tuple<Args...>>;
      arg0_type = std::string(_detail::type_annotation_for<A0>());
      arg0_name = pinned[0];
    }
    std::vector<std::string> ptypes = {
        std::string(_detail::type_annotation_for<Args>())...};
    wrapped_ns_rows().push_back(
        {ns_, std::move(method_name), name_, std::move(arg0_type),
         std::move(arg0_name), static_cast<int8_t>(sizeof...(Args)),
         adapter, pinned, std::move(ptypes),
         std::string(wrap_detail::return_annotation<R>())});
  }

  std::string ns_;
  std::string name_;
};

template <class T>
ClassBinder<T> wrap(std::string ns, std::string name) {
  return ClassBinder<T>(std::move(ns), std::move(name));
}

}  // namespace culebra
