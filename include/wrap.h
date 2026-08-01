#pragma once
// Declarative C++ class wrapping — Phase 4 (P4-1).
//
// The "codegen" is the C++ compiler, pybind11-style: a thin declaration
// instantiates the glue (conversion thunks, method prototypes, Foreign
// registration) at compile time, and a static initializer records the
// class in the process-wide registry the stdlib setup walks:
//
//   // The registrar needs a name UNIQUE across all binding headers —
//   // two `inline const bool _` definitions would be an ODR violation
//   // that silently drops one registration.
//   inline const bool _mylib_counter_wrapped = [] {
//     culebra::wrap<mylib::Counter>("__Foreign", "Counter")
//         .ctor<long>({"start"})
//         .method<&mylib::Counter::value>("value")
//         .method<&mylib::Counter::add>("add", {"n"})
//         .static_method<&mylib::Counter::live>("live");
//     return true;
//   }();
//
// Instances ride the Foreign id table (foreign.h): `drop` erases the
// entry (~T() runs NOW), a dropped instance's method raises ClosedError,
// and the handle — an ordinary drop-having Object — gets the whole
// Phase 1/2 lifetime machinery (scope-exit drop, cycles, GC backstop,
// exactly-once) for free. Return values lower by ownership shape: a by-value or
// unique_ptr<U> return becomes an owning handle of U, shared_ptr<U>
// holds one share, primitives go through cpp_to_value. U must itself be
// wrapped (its class_info carries the display name the handle shows).
//
// Phase 4 sub-phasing: P4-1 = the interp artifacts, P4-2 = the JIT
// thunks + NsMethod registry rows (below, CULEBRA_JIT_ENABLED), P4-3 =
// the `culebra wrap` build pipeline.

#include <foreign.h>
#include <interpreter.h>
#ifdef CULEBRA_JIT_ENABLED
// wrap.h can be reached before culebra.h pulls jit.h (repl.h includes
// stdlib_interp.h first), so include it here — after interpreter.h,
// preserving the interpreter-before-jit order jit.h's inline bodies
// rely on.
#include <jit.h>
#endif

#include <cassert>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace culebra {

namespace wrap_detail {

template <class T>
struct is_unique_ptr : std::false_type {};
template <class U, class D>
struct is_unique_ptr<std::unique_ptr<U, D>> : std::true_type {};
template <class T>
struct is_shared_ptr : std::false_type {};
template <class U>
struct is_shared_ptr<std::shared_ptr<U>> : std::true_type {};

// Per-T binding info, filled once by the binder's static-init run and
// read by make_foreign_handle on every construction. The prototypes
// capture nothing per-instance (the foreign table resolves per call,
// `self` per invocation), so each handle just copies the shared Values
// into its property map — the hand-written Phase 3 binding's shape.
// constinit pins the constant-initialization the registration scheme
// relies on: the binder's dynamic initializer writes into these, so they
// must never be dynamically (re)initialized themselves — templated
// statics have no ordering guarantee against other TUs' initializers.
template <class T>
struct class_info {
  static constinit inline std::string name;
  static constinit inline std::vector<std::pair<std::string, Value>> methods;
};

template <class T>
inline int64_t handle_id(const std::shared_ptr<Environment>& env) {
  return static_cast<int64_t>(
      env->get("self").to_object().get("_id").to_long());
}

// Generation of a handle, or -1 when it is no longer valid.
// Owning handles read their table entry through the type-erased
// _state_fn; a borrowing handle (`_bid`) resolves through the borrow
// table — both indirections, so a forged slot is a table miss, never a
// dereferenced raw pointer.
inline int64_t handle_gen(const ObjectValue& h) {
  if (h.has("_state_fn")) {
    return foreign::owner_gen_via(h.get("_state_fn").to_long(),
                                  h.get("_id").to_long());
  }
  if (h.has("_bid")) return foreign::borrow_gen(h.get("_bid").to_long());
  return -1;
}

// The live instance, or ClosedError — every method's first step. A
// borrowing handle resolves its raw pointer from the borrow table only
// after the parent chain validates; the pointer is never read from a
// script-writable slot.
template <class T>
inline T* handle_self(const std::shared_ptr<Environment>& env) {
  int64_t line = env->get("__LINE__").to_long();
  int64_t col = env->get("__COLUMN__").to_long();
  const auto& h = env->get("self").to_object();
  if (h.has("_bid")) {
    int64_t bid = h.get("_bid").to_long();
    if (!foreign::borrow_valid(bid))
      foreign::throw_borrow_invalid(class_info<T>::name, line, col);
    return reinterpret_cast<T*>(foreign::borrow_ptr(bid));
  }
  return foreign::get_or_throw<T>(handle_id<T>(env), class_info<T>::name,
                                  line, col);
}

// The per-instance handle: data slots + the shared method prototypes +
// the drop event (erase the table entry — ~T() runs NOW; idempotent,
// exactly-once via the Phase 1 `dropped` flag on the handle).
template <class T>
Value make_foreign_handle(int64_t id) {
  ObjectValue h;
  h.initialize("__foreign__", Value(std::string(class_info<T>::name)), false);
  h.initialize("_id", Value(static_cast<int64_t>(id)), false);
  // Type-erased closed+gen read for the borrow validation chain.
  h.initialize("_state_fn",
               Value(static_cast<int64_t>(foreign::state_fn_id<T>())), false);
  // A foreign instance lives in a process-local table — not Sendable.
  h.initialize("__nonsendable__", Value(true), false);
  for (const auto& [name, proto] : class_info<T>::methods) {
    h.initialize(name, proto, false);
  }
  static const Value drop_proto(
      FunctionValue({}, [](std::shared_ptr<Environment> env) {
        foreign::table<T>().erase(handle_id<T>(env));
        return Value();
      }));
  h.initialize("drop", drop_proto, false);
  return Value(std::move(h));
}

// The borrow id of a parent handle (owning or borrowing): owning
// parents have no borrow id, so they pass -1 and an owning parent_state.
inline void parent_link(const ObjectValue& parent, int64_t* state,
                        int64_t* owner_id, int64_t* bid) {
  if (parent.has("_bid")) {
    *state = -1;
    *owner_id = 0;
    *bid = parent.get("_bid").to_long();
  } else {
    *state = parent.get("_state_fn").to_long();
    *owner_id = parent.get("_id").to_long();
    *bid = -1;
  }
}

// A borrowing handle: the raw pointer + parent link live in the
// borrow table (forgery-safe — the handle holds only the opaque id).
// `__parent__` is an ordinary GC reference keeping the whole chain
// alive. The handle owns nothing, but it DOES carry an internal `drop`
// (erase the borrow id) and registers on the owned stack, so its table
// entry is reclaimed deterministically when the handle dies — and an
// explicit `.drop()` closes this borrow (without touching the parent's
// resource).
template <class T2>
inline Value make_borrow_handle(T2* p, const Value& parent, int64_t pgen) {
  int64_t state = -1, owner_id = 0, parent_bid = -1;
  parent_link(parent.to_object(), &state, &owner_id, &parent_bid);
  int64_t bid = foreign::borrow_adopt(p, state, owner_id, parent_bid, pgen);

  ObjectValue h;
  h.initialize("__foreign__", Value(std::string(class_info<T2>::name)),
               false);
  h.initialize("_bid", Value(static_cast<int64_t>(bid)), false);
  h.initialize("__parent__", parent, false);
  h.initialize("__nonsendable__", Value(true), false);
  for (const auto& [name, proto] : class_info<T2>::methods) {
    h.initialize(name, proto, false);
  }
  static const Value borrow_drop(
      FunctionValue({}, [](std::shared_ptr<Environment> env) {
        foreign::borrow_erase(env->get("self").to_object().get("_bid").to_long());
        return Value();
      }));
  h.initialize("drop", borrow_drop, false);
  return Value(std::move(h));
}

// Non-const method dispatch bumps the instance's generation,
// staling its outstanding borrows: the foreign table entry for an
// owning handle, the borrow table entry for a borrow (a mutation
// through a borrow stales its children, not its siblings).
template <class T>
inline void bump_handle_gen(const std::shared_ptr<Environment>& env) {
  const auto& h = env->get("self").to_object();
  if (h.has("_bid")) {
    foreign::borrow_bump(h.get("_bid").to_long());
  } else {
    foreign::table<T>().bump_gen(handle_id<T>(env));
  }
}

// Return-value lowering. `R` decayed: handles for wrapped
// class values / unique_ptr / shared_ptr, cpp_to_value for the rest.
template <class R>
inline Value lower_return(R r) {
  using D = std::decay_t<R>;
  if constexpr (is_unique_ptr<D>::value) {
    using U = typename D::element_type;
    auto id = foreign::table<U>().adopt(std::move(r));
    return make_foreign_handle<U>(id);
  } else if constexpr (is_shared_ptr<D>::value) {
    using U = typename D::element_type;
    auto id = foreign::table<U>().adopt_shared(std::move(r));
    return make_foreign_handle<U>(id);
  } else if constexpr (requires(D v) { _detail::cpp_to_value(std::move(v)); }) {
    return _detail::cpp_to_value(std::move(r));
  } else {
    // A wrapped-class reference return is not an ownership shape:
    // lowering it by value would silently move from (T&) or shadow-copy
    // (const T&) the live instance. Borrowing is Phase 5.
    static_assert(!std::is_reference_v<R>,
                  "wrap.h: a method returning T& / const T& of a wrapped "
                  "class has no ownership shape — return T, unique_ptr<T>, "
                  "or shared_ptr<T> (borrowing lands in Phase 5)");
    // By value = the unique shape after one move-in (foreign.h).
    auto id = foreign::table<D>().adopt(std::make_unique<D>(std::move(r)));
    return make_foreign_handle<D>(id);
  }
}

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
  } else if constexpr (requires(D v) { _detail::cpp_to_value(std::move(v)); }) {
    return {};
  } else {
    return "Object";
  }
}

// Fill omitted parameter names with `_arg<i>` and pin the storage —
// FunctionValue::Parameter holds string_views into it.
// A wrapped body's own C++ exception (a ctor refusing construction, a method
// reporting failure), surfaced as a culebra RuntimeError the script can catch.
// Both backends' entry points route their body call through here — without it
// the interp's eval boundary bakes the position into the MESSAGE (its legacy
// path for unconverted throws) while the JIT lets the exception escape the
// process, so the two disagree on message, catchability and position at once.
// Position stays 0:0: the interp eval boundary stamps the call node into the
// fields, and the JIT boundaries backfill from the published op position —
// the same convention as every positionless lib helper. CulebraError passes
// through untouched (argument conversion and handle validation own theirs).
template <class F>
auto surface_native_error(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const culebra::CulebraError&) {
    throw;
  } catch (const std::exception& e) {
    throw culebra::CulebraError("RuntimeError", e.what(), 0, 0);
  }
}

inline std::shared_ptr<std::vector<std::string>> pin_param_names(
    std::vector<std::string> names, size_t arity) {
  names.resize(arity);
  for (size_t i = 0; i < arity; i++) {
    if (names[i].empty()) names[i] = "_arg" + std::to_string(i);
  }
  return std::make_shared<std::vector<std::string>>(std::move(names));
}

template <class... Args>
inline std::vector<FunctionValue::Parameter> typed_params(
    const std::vector<std::string>& storage) {
  std::vector<FunctionValue::Parameter> params;
  params.reserve(sizeof...(Args));
  size_t i = 0;
  (params.push_back({std::string_view(storage[i++]), false,
                     _detail::type_annotation_for<Args>()}),
   ...);
  return params;
}

// One method prototype: resolve self (ClosedError when dropped),
// convert each declared param, call, lower the return. `bump` (a
// non-const method without preserves_borrows) increments the
// generation BEFORE the call, so a borrow the call itself returns
// snapshots the post-bump value.
template <class T, class Mf, class R, class... Args>
inline Value make_method_proto(Mf mf, std::vector<std::string> names,
                               bool bump = false) {
  auto storage = pin_param_names(std::move(names), sizeof...(Args));
  auto eval = [storage, mf, bump](std::shared_ptr<Environment> env) -> Value {
    T* self = handle_self<T>(env);
    if (bump) bump_handle_gen<T>(env);
    auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> Value {
      if constexpr (std::is_void_v<R>) {
        (self->*mf)(_detail::ValueAs<Args>::convert(
            env->get(std::string_view((*storage)[I])))...);
        return Value();
      } else {
        return lower_return<R>((self->*mf)(_detail::ValueAs<Args>::convert(
            env->get(std::string_view((*storage)[I])))...));
      }
    };
    return surface_native_error(
        [&] { return invoke(std::index_sequence_for<Args...>{}); });
  };
  return Value(FunctionValue(typed_params<Args...>(*storage),
                             std::move(eval), return_annotation<R>()));
}

// A borrowed-return method: the reference points INTO self, so
// the result is a borrowing handle snapshotting the parent's current
// generation. Taking a borrow never bumps — the declaration already
// states the method returns a view, and multiple live borrows are the
// point.
template <class T, class Mf, class T2, class... Args>
inline Value make_borrowed_proto(Mf mf, std::vector<std::string> names) {
  auto storage = pin_param_names(std::move(names), sizeof...(Args));
  auto eval = [storage, mf](std::shared_ptr<Environment> env) -> Value {
    T* self = handle_self<T>(env);
    const Value parent = env->get("self");
    int64_t pgen = handle_gen(parent.to_object());
    auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> Value {
      auto& r = (self->*mf)(_detail::ValueAs<Args>::convert(
          env->get(std::string_view((*storage)[I])))...);
      return make_borrow_handle<T2>(const_cast<T2*>(&r), parent, pgen);
    };
    return surface_native_error(
        [&] { return invoke(std::index_sequence_for<Args...>{}); });
  };
  return Value(FunctionValue(typed_params<Args...>(*storage),
                             std::move(eval), "Object"));
}

// A free / static function entry (no self).
template <class Fn, class R, class... Args>
inline Value make_static_proto(Fn fn, std::vector<std::string> names) {
  auto storage = pin_param_names(std::move(names), sizeof...(Args));
  auto eval = [storage, fn](std::shared_ptr<Environment> env) -> Value {
    auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> Value {
      if constexpr (std::is_void_v<R>) {
        fn(_detail::ValueAs<Args>::convert(
            env->get(std::string_view((*storage)[I])))...);
        return Value();
      } else {
        return lower_return<R>(fn(_detail::ValueAs<Args>::convert(
            env->get(std::string_view((*storage)[I])))...));
      }
    };
    return surface_native_error(
        [&] { return invoke(std::index_sequence_for<Args...>{}); });
  };
  return Value(FunctionValue(typed_params<Args...>(*storage),
                             std::move(eval), return_annotation<R>()));
}

#ifdef CULEBRA_JIT_ENABLED

// --- JIT artifacts (P4-2) ---------------------------------------------------
//
// The same declaration instantiates the JIT side: per-method handle
// thunks (plain functions — the member-fn pointer is a NON-TYPE template
// argument, so each method gets its own static thunk, exactly the
// hand-written Phase 3 shape), ns adapters for the kNsMethods-style
// dispatch, and a row registry stdlib_jit.h merges with its static
// table. Type checks and error positions follow the Phase 3 thunk
// conventions: param type-check BEFORE the closed check (interp binder
// order), TypeError at the argument's threaded position (argpos /
// call_arg0 fallback), ClosedError at the call site.

// Annotation check through the JIT's canonical matcher — the SAME
// predicate the kNsMethods trampoline applies to ctor/static params
// (and the interp's type_matches mirror), so a String param rejects a
// StringView slice identically on every path.
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
// class_info.
template <class T, auto Mf>
struct jit_method_info {
  static constinit inline std::vector<std::string> param_names;
};

// Per-T handle method table consumed by the handle builder. `meta`
// carries the param names so the method binds keyword arguments like
// the interp (null for the auto-bound `drop`).
template <class T>
struct jit_class_info {
  struct Method {
    std::string name;
    void (*thunk)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*);
    size_t arity;
    const JitParamMeta* meta;
  };
  static constinit inline std::vector<Method> methods;
};

// JitObject mirror of handle_gen: generation, or -1 when invalid. Both
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
      foreign::throw_borrow_invalid(class_info<T>::name,
                                    _jit_call_site_line, _jit_call_site_col);
    return reinterpret_cast<T*>(foreign::borrow_ptr(bid));
  }
  return foreign::get_or_throw<T>(_jit_handle_long(h, "_id"),
                                  class_info<T>::name, _jit_call_site_line,
                                  _jit_call_site_col);
}

// Non-const dispatch bump — the JitValue mirror of bump_handle_gen.
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

// Return lowering — the JitValue mirror of lower_return.
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
    static_assert(!std::is_same_v<D, Value>,
                  "wrap.h: a culebra::Value return has no JitValue lowering");
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
                                            class_info<T>::name))},
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
                                            class_info<T2>::name))},
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
        std::format("type error: parameter '{}' expects {}", names[bad],
                    kAnnos[bad]),
        ap ? _jit_argpos_line[bad]
           : (bad == 0 ? _jit_call_arg0_line : _jit_call_site_line),
        ap ? _jit_argpos_col[bad]
           : (bad == 0 ? _jit_call_arg0_col : _jit_call_site_col));
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
  {
    *__ret = surface_native_error(
        [&] { return invoke(std::index_sequence_for<Args...>{}); });
    return;
  }
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
  {
    *__ret = surface_native_error(
        [&] { return invoke(std::index_sequence_for<Args...>{}); });
    return;
  }
}

// ns adapters: reached through the kNsMethods trampoline, which has
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

#endif  // CULEBRA_JIT_ENABLED

}  // namespace wrap_detail

#ifdef CULEBRA_JIT_ENABLED
// One kNsMethods-shaped row per ctor/static of a wrapped class.
// stdlib_jit.h materializes NsMethod rows from these (lazily, after
// static-init froze the registry, so the c_str pointers are stable) and
// merges them into every table consumer. The class name rides in `sub`:
// `Ns.Class.method` is a nested namespace, slow-path only — the same
// shape as Encoding.html.
struct WrappedNsRow {
  std::string ns;
  std::string name;
  std::string sub;
  std::string arg0_type;  // empty = none
  std::string arg0_name;
  int8_t arity;
  JitValue (*adapter)(JitValue*, int64_t);
};
inline std::vector<WrappedNsRow>& wrapped_ns_rows() {
  static std::vector<WrappedNsRow> v;
  return v;
}
#endif  // CULEBRA_JIT_ENABLED

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

// Process-wide registry of wrapped classes, populated at static-init
// time by the wrap<T>() declarations and consumed by the stdlib setup
// (one namespace object per distinct `ns`, holding one class object —
// ctor + statics — per class).
struct WrappedClass {
  std::string ns;
  std::string name;
  std::function<Value()> build_class_object;
};
inline std::vector<WrappedClass>& wrapped_classes() {
  static std::vector<WrappedClass> v;
  return v;
}

// Builder. Destruction finalizes: the collected ctor/static entries
// become the class object, registered under (ns, name).
template <class T>
class ClassBinder {
 public:
  ClassBinder(std::string ns, std::string name) : ns_(std::move(ns)) {
    assert(wrap_detail::class_info<T>::name.empty() &&
           "wrap<T>: this class is already wrapped");
    wrap_detail::class_info<T>::name = name;
    name_ = std::move(name);
  }

  // Constructor: `Ns.Name.new(args...)` → owning handle. A static proto
  // returning unique_ptr<T> — lower_return's adopt branch does the rest.
  template <class... Args>
  ClassBinder& ctor(std::vector<std::string> names = {}) {
    auto make = [](Args... args) {
      return std::make_unique<T>(std::move(args)...);
    };
    statics_.emplace_back(
        "new", wrap_detail::make_static_proto<decltype(make),
                                              std::unique_ptr<T>, Args...>(
                   make, names));
#ifdef CULEBRA_JIT_ENABLED
    push_ns_row<Args...>("new", &wrap_detail::jit_ctor_adapter<T, Args...>,
                         std::move(names));
#endif
    return *this;
  }

  // The member fn is a NON-TYPE template argument so the JIT side gets a
  // distinct static thunk per method (the closure ABI needs a plain
  // function pointer). The interp proto uses the same constant at runtime.
  // A non-const method bumps the instance's generation (staling its
  // borrows) unless declared wrap_policy::preserves_borrows.
  template <auto Mf>
  ClassBinder& method(std::string name, std::vector<std::string> names = {},
                      wrap_policy policy = wrap_policy::standard) {
    using traits = _detail::fn_traits<decltype(Mf)>;
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
    using traits = _detail::fn_traits<decltype(Mf)>;
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
    using traits = _detail::fn_traits<decltype(Fn)>;
    return static_impl<Fn, typename traits::ret>(
        std::move(name), std::move(names),
        static_cast<typename traits::args*>(nullptr));
  }

  ~ClassBinder() {
    auto statics = std::move(statics_);
    wrapped_classes().push_back(
        {std::move(ns_), std::move(name_), [statics]() {
           ObjectValue cls;
           for (const auto& [n, v] : statics) cls.initialize(n, v, false);
           return Value(std::move(cls));
         }});
  }

  ClassBinder(const ClassBinder&) = delete;
  ClassBinder& operator=(const ClassBinder&) = delete;

 private:
  template <auto Mf, class R, class... Args>
  ClassBinder& method_impl(std::string name, std::vector<std::string> names,
                           wrap_policy policy, std::tuple<Args...>*) {
    const bool bump =
        !wrap_detail::is_const_member<decltype(Mf)>::value &&
        policy != wrap_policy::preserves_borrows;
    wrap_detail::class_info<T>::methods.emplace_back(
        name, wrap_detail::make_method_proto<T, decltype(Mf), R, Args...>(
                  Mf, names, bump));
#ifdef CULEBRA_JIT_ENABLED
    auto pinned = wrap_detail::pin_param_names(std::move(names),
                                               sizeof...(Args));
    wrap_detail::jit_method_info<T, Mf>::param_names = *pinned;
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(*pinned,
                                    std::vector<bool>(sizeof...(Args), false));
    wrap_detail::jit_class_info<T>::methods.push_back(
        {std::move(name),
         bump ? &wrap_detail::jit_method_thunk<T, Mf, R, true, Args...>
              : &wrap_detail::jit_method_thunk<T, Mf, R, false, Args...>,
         sizeof...(Args), meta});
#endif
    return *this;
  }

  template <auto Mf, class T2, class... Args>
  ClassBinder& borrowed_impl(std::string name, std::vector<std::string> names,
                             std::tuple<Args...>*) {
    wrap_detail::class_info<T>::methods.emplace_back(
        name,
        wrap_detail::make_borrowed_proto<T, decltype(Mf), T2, Args...>(
            Mf, names));
#ifdef CULEBRA_JIT_ENABLED
    auto pinned = wrap_detail::pin_param_names(std::move(names),
                                               sizeof...(Args));
    wrap_detail::jit_method_info<T, Mf>::param_names = *pinned;
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(*pinned,
                                    std::vector<bool>(sizeof...(Args), false));
    wrap_detail::jit_class_info<T>::methods.push_back(
        {std::move(name),
         &wrap_detail::jit_borrowed_thunk<T, Mf, T2, Args...>,
         sizeof...(Args), meta});
#endif
    return *this;
  }

  template <auto Fn, class R, class... Args>
  ClassBinder& static_impl(std::string name, std::vector<std::string> names,
                           std::tuple<Args...>*) {
    statics_.emplace_back(
        name, wrap_detail::make_static_proto<decltype(Fn), R, Args...>(
                  Fn, names));
#ifdef CULEBRA_JIT_ENABLED
    push_ns_row<Args...>(std::move(name),
                         &wrap_detail::jit_static_adapter<Fn, R, Args...>,
                         std::move(names));
#endif
    return *this;
  }

#ifdef CULEBRA_JIT_ENABLED
  template <class... Args>
  void push_ns_row(std::string method_name,
                   JitValue (*adapter)(JitValue*, int64_t),
                   std::vector<std::string> names) {
    auto pinned =
        wrap_detail::pin_param_names(std::move(names), sizeof...(Args));
    std::string arg0_type, arg0_name;
    if constexpr (sizeof...(Args) > 0) {
      using A0 = std::tuple_element_t<0, std::tuple<Args...>>;
      arg0_type = std::string(_detail::type_annotation_for<A0>());
      arg0_name = (*pinned)[0];
    }
    wrapped_ns_rows().push_back(
        {ns_, std::move(method_name), name_, std::move(arg0_type),
         std::move(arg0_name), static_cast<int8_t>(sizeof...(Args)),
         adapter});
  }
#endif

  std::string ns_;
  std::string name_;
  std::vector<std::pair<std::string, Value>> statics_;
};

template <class T>
ClassBinder<T> wrap(std::string ns, std::string name) {
  return ClassBinder<T>(std::move(ns), std::move(name));
}

}  // namespace culebra
