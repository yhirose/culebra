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
//   // include/interop/foreign_binding.h does.
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

#include <base/fn_traits.h>
#include <interop/foreign.h>
#include <interop/wrap_registry.h>
// wrap.h can be reached before culebra.h pulls the runtime layer, so
// include it here.
#include <rt/rt.h>

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
// for the NsMethod-style dispatch, and a row registry stdlib_rt.h merges
// with its static table. Type checks and error positions follow the Phase 3
// thunk conventions: param type-check BEFORE the closed check, TypeError at
// the argument's threaded position (argpos / call_arg0 fallback),
// ClosedError at the call site.

// A declared method parameter: its name, and — for a trailing optional —
// the default a call site may omit. Implicitly constructible from a bare
// name, so every existing `{"a", "b"}` list keeps its spelling and its
// meaning; only a parameter that wants to be optional spells one entry as
// `{"name", value}` instead. Defaults are C++-side literals, not Culebra
// expressions (there is no evaluator here): the four kinds jit_lower_return
// and jit_arg_get already know how to move as a bare JitValue. Optional
// parameters must be a trailing run — nothing here checks that; it mirrors
// how the underlying has-default bitmap (_jit_make_handle_meta) already
// reads it, min_arity first.
struct wrap_param {
  std::string name;
  bool has_default = false;
  int8_t default_tag = TAG_NIL;
  int64_t default_data = 0;

  wrap_param() = default;
  wrap_param(const char* n) : name(n) {}
  wrap_param(std::string n) : name(std::move(n)) {}

  // One template, not an overload per kind: `0L`/`0`/`nullptr` are all
  // mutually-convertible null-pointer-constant-or-arithmetic literals, so
  // overload resolution between separate bool/int64_t/double/nullptr_t
  // constructors ranks their conversions as equally good and rejects the
  // call as ambiguous. Branching on the deduced type inside one
  // constructor sidesteps that; only one candidate exists to select.
  template <class V>
  wrap_param(std::string n, V v) : name(std::move(n)), has_default(true) {
    using D = std::decay_t<V>;
    if constexpr (std::is_same_v<D, std::nullptr_t>) {
      default_tag = TAG_NIL;
    } else if constexpr (std::is_same_v<D, bool>) {
      default_tag = TAG_BOOL;
      default_data = v ? 1 : 0;
    } else if constexpr (std::is_floating_point_v<D>) {
      default_tag = TAG_FLOAT;
      double d = static_cast<double>(v);
      std::memcpy(&default_data, &d, sizeof(double));
    } else if constexpr (std::is_integral_v<D>) {
      default_tag = TAG_LONG;
      default_data = static_cast<int64_t>(v);
    } else if constexpr (std::is_constructible_v<std::string, V>) {
      // Interned (immortal, GC-invisible — the same storage a handle's
      // __foreign__ name uses), so an omitted argument costs no allocation
      // and needs no rooting.
      default_tag = TAG_STRING;
      default_data =
          reinterpret_cast<int64_t>(_intern_str(std::string(v)));
    } else {
      static_assert(!sizeof(V*),
                    "wrap.h: a wrap_param default must be nullptr, bool, an "
                    "integer, a float/double, or a string");
    }
  }

  JitValue default_value() const { return {default_tag, default_data}; }
};

// Per-(T, method) params for the thunk's error wording and its optional
// defaults. Filled by the binder at static-init; constinit for the same
// ordering reason as jit_class_info.
template <class T, auto Mf>
struct jit_method_info {
  static constinit inline std::vector<wrap_param> params;
};

// The JitValue at slot i, or its declared default when the caller omitted
// it (i >= n, a plain positional call shorter than the full arity; or a
// TAG_UNFILLED slot, the kwargs resolver's own omission marker). Mirrors
// _jit_file_arg_present's reasoning: check `i < n` before ever touching
// `args[i]`, so a short positional call never reads past what the caller
// actually supplied.
template <class T, auto Mf>
inline JitValue jit_arg_or_default(int64_t n, JitValue* args, size_t i) {
  if (static_cast<int64_t>(i) < n && args[i].tag != TAG_UNFILLED) {
    return args[i];
  }
  return jit_method_info<T, Mf>::params[i].default_value();
}

// pin_param_names's twin for method/borrowed_method: fill an omitted
// parameter's name the same way, over wrap_param instead of a bare string.
inline std::vector<wrap_param> pin_wrap_params(std::vector<wrap_param> params,
                                               size_t arity) {
  params.resize(arity);
  for (size_t i = 0; i < arity; i++) {
    if (params[i].name.empty()) params[i].name = "_arg" + std::to_string(i);
  }
  return params;
}

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
  // The `T?` annotation a U* parameter of this class carries (param_type_name
  // above) — computed once here rather than concatenated at every call, so
  // that annotation is always a view of process-lifetime storage, never a
  // temporary.
  static constinit inline std::string opt_name;
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
                                    _jit_thread.call_line, _jit_thread.call_col);
    return reinterpret_cast<T*>(foreign::borrow_ptr(bid));
  }
  return foreign::get_or_throw<T>(_jit_handle_long(h, "_id"),
                                  jit_class_info<T>::name, _jit_thread.call_line,
                                  _jit_thread.call_col);
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

// A method parameter naming a wrapped class: U& / const U& (a required
// handle, resolved to a live T&) or U* / const U* (nil accepted, resolved to
// nullptr). Never std::string — that spelling already means the String
// primitive, marshalled through jit_arg_get's other branch below. A method
// wanting to give up ownership of one of its own arguments still can't:
// value, && and smart-pointer parameters of a wrapped class don't match
// either partial specialization here, so they fall through to the string
// branch and fail to compile — the same asymmetry jit_lower_return's
// static_assert states for returns applies to arguments in the other
// direction: an argument borrows the caller's handle for the call, only a
// return moves ownership.
template <class A>
struct handle_target {
 private:
  using NoRef = std::remove_reference_t<A>;
  using NoPtr = std::remove_pointer_t<A>;
  static constexpr bool is_ref =
      std::is_lvalue_reference_v<A> && std::is_class_v<NoRef> &&
      !std::is_same_v<std::remove_cv_t<NoRef>, std::string>;
  static constexpr bool is_ptr =
      std::is_pointer_v<A> && std::is_class_v<NoPtr>;

 public:
  using type = std::conditional_t<
      is_ref, std::remove_cv_t<NoRef>,
      std::conditional_t<is_ptr, std::remove_cv_t<NoPtr>, void>>;
  static constexpr bool is_pointer = is_ptr;
  // A non-const U&/U* can mutate the referent, so passing one bumps its
  // generation the same way a non-const receiver does; const doesn't.
  static constexpr bool is_mutable =
      is_ref ? !std::is_const_v<NoRef> : (is_ptr && !std::is_const_v<NoPtr>);
};

// The declared Culebra annotation for a parameter: a wrapped class's own
// display name (so `fn f(c: Counter)` and a `Counter` method parameter mean
// the same thing) for a handle shape, the fixed primitive table otherwise.
// Not constexpr — a wrapped class's name is filled by its own wrap<>
// declaration's constructor, at a point this parameter's declaration cannot
// see — so both jit_arg_matches and jit_check_args's kAnnos read through
// this one function rather than each keeping an independent, possibly
// stale copy (decay_t here vs plain Args there was exactly that drift,
// before this existed: a `const int&` param's error wording read blank).
template <class A>
inline std::string_view param_type_name() {
  using Target = typename handle_target<A>::type;
  if constexpr (std::is_void_v<Target>) {
    return _detail::type_annotation_for<std::decay_t<A>>();
  } else if constexpr (handle_target<A>::is_pointer) {
    return jit_class_info<Target>::opt_name;
  } else {
    return jit_class_info<Target>::name;
  }
}

// Annotation check through the canonical matcher — the SAME predicate the
// ns-method trampoline applies to ctor/static params, so a String param
// rejects a StringView slice identically on every path.
template <class A>
inline bool jit_arg_matches(const JitValue& v) {
  return _culebra_value_matches_type(v.tag, v.data, param_type_name<A>());
}

template <class A>
inline decltype(auto) jit_arg_get(const JitValue& v) {
  using Target = typename handle_target<A>::type;
  if constexpr (!std::is_void_v<Target>) {
    if constexpr (handle_target<A>::is_pointer) {
      return v.tag == TAG_NIL ? static_cast<Target*>(nullptr)
                              : jit_handle_self<Target>(v);
    } else {
      return *jit_handle_self<Target>(v);
    }
  } else {
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
}

// A sequenced pass over the handle-typed elements of Args, left to right —
// resolving a handle argument inside the actual (obj->*Mf)(...) call has
// unspecified argument evaluation order, so which of two invalid arguments
// gets reported would be compiler-dependent (difftest and a macOS/Linux
// toolchain difference would both see it). This pass — not that call — is
// what fixes the order; jit_arg_get's own resolution afterward is redundant
// but always succeeds once this one has.
template <class A>
inline void jit_arg_handle_check(int64_t n, JitValue* args, size_t i) {
  using Target = typename handle_target<A>::type;
  if constexpr (!std::is_void_v<Target>) {
    if (static_cast<int64_t>(i) >= n) return;  // absent; a default supplies it
    const JitValue& v = args[i];
    if (v.tag == TAG_UNFILLED || v.tag == TAG_NIL) return;
    (void)jit_handle_self<Target>(v);  // ClosedError on a dropped/stale handle
  }
}
template <class... Args>
inline void jit_resolve_arg_handles(int64_t n, JitValue* args) {
  size_t i = 0;
  (jit_arg_handle_check<Args>(n, args, i++), ...);
}

// Companion pass: bump the generation of every non-const handle argument,
// mirroring what a non-const receiver already does. Runs after resolution
// (so a stale argument reports ClosedError, not a bump on top of it) and
// before the call.
template <class A>
inline void jit_arg_handle_bump(int64_t n, JitValue* args, size_t i) {
  using Target = typename handle_target<A>::type;
  if constexpr (!std::is_void_v<Target> && handle_target<A>::is_mutable) {
    if (static_cast<int64_t>(i) >= n) return;
    const JitValue& v = args[i];
    if (v.tag == TAG_UNFILLED || v.tag == TAG_NIL) return;
    jit_bump_handle_gen<Target>(v);
  }
}
template <class... Args>
inline void jit_bump_arg_handles(int64_t n, JitValue* args) {
  size_t i = 0;
  (jit_arg_handle_bump<Args>(n, args, i++), ...);
}

// Binder-order param checks shared by the method and borrowed thunks:
// a missing required argument is an ArityError at the call site, a
// wrong-typed one a TypeError at the argument's threaded position —
// both released `fn` first.
template <class T, auto Mf, class... Args>
inline void jit_check_args(JitValue self, int64_t n, JitValue* args) {
  const auto& params = jit_method_info<T, Mf>::params;
  size_t bad = sizeof...(Args);
  bool missing = false;
  {
    size_t i = 0;
    auto check_one = [&](bool present, bool ok) {
      if (bad == sizeof...(Args)) {
        if (!present) {
          if (!params[i].has_default) {
            bad = i;
            missing = true;
          }
        } else if (!ok) {
          bad = i;
        }
      }
      ++i;
    };
    (check_one(static_cast<int64_t>(i) < n && args[i].tag != TAG_UNFILLED,
               static_cast<int64_t>(i) < n && args[i].tag != TAG_UNFILLED &&
                   jit_arg_matches<Args>(args[i])),
     ...);
  }
  if (bad < sizeof...(Args)) {
    culebra_runtime_value_release(self.tag, self.data);
    if (missing) {
      throw culebra::CulebraError(
          "ArityError",
          culebra::missing_required_arg_message(params[bad].name),
          _jit_thread.call_line, _jit_thread.call_col);
    }
    // Not constexpr: a wrapped-class parameter's annotation (param_type_name)
    // reads that class's own display name, filled at runtime by its wrap<>
    // declaration.
    static const std::string_view kAnnos[] = {param_type_name<Args>()...};
    auto pos = _jit_arg_pos(static_cast<int>(bad));
    throw culebra::CulebraError(
        "TypeError",
        culebra::format("type error: parameter '{}' expects {}",
                        params[bad].name, kAnnos[bad]),
        pos.line, pos.col);
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
    _jit_backfill_error_pos(e, _jit_thread.call_line, _jit_thread.call_col);
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

// One handle method thunk per (T, member fn). Self and every argument arrive
// +1 (callee-consumes, same ABI JitMethodArgs documents): self is released on
// every exit by the guard below — an early release on a temp receiver would
// fire its drop and erase the table entry under our feet, so the guard runs at
// scope exit, after the body. `jit_check_args` still owns its own binder-throw
// release of self (it runs before the guard is armed); the guard closes the
// previously leaking ClosedError (jit_handle_self) and body-throw paths.
// Args are guarded from the top, before jit_check_args even runs: a handle
// argument (Gap A) is itself a +1 reference, and nothing here forwards or
// stores it, so without this it leaked on every call — most visibly a
// temporary (`c.merge(Counter.new(5))`) whose only reference was the
// argument slot.
// `Bump` (a non-const method without preserves_borrows) increments the
// generation after validation, BEFORE the call.
template <class T, auto Mf, class R, bool Bump, class... Args>
void jit_method_thunk(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                          int64_t n, JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodArgs _a{n, args};
  jit_check_args<T, Mf, Args...>(self, n, args);
  JitMethodSelf _s{self};
  T* obj = jit_handle_self<T>(self);
  jit_resolve_arg_handles<Args...>(n, args);  // order-fixing; see its own comment
  if constexpr (Bump) jit_bump_handle_gen<T>(self);
  jit_bump_arg_handles<Args...>(n, args);
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
    if constexpr (std::is_void_v<R>) {
      (obj->*Mf)(jit_arg_get<Args>(jit_arg_or_default<T, Mf>(n, args, I))...);
      return {TAG_NIL, 0};
    } else {
      auto&& r = (obj->*Mf)(
          jit_arg_get<Args>(jit_arg_or_default<T, Mf>(n, args, I))...);
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
  JitMethodArgs _a{n, args};  // see jit_method_thunk
  jit_check_args<T, Mf, Args...>(self, n, args);
  // Release self on every exit (see jit_method_thunk). The borrow handle keeps
  // its own reference to self as its parent, so the scope-exit release — after
  // the handle is built and returned — leaves the borrow valid while closing
  // the ClosedError / body-throw strand.
  JitMethodSelf _s{self};
  T* obj = jit_handle_self<T>(self);
  jit_resolve_arg_handles<Args...>(n, args);  // see jit_method_thunk
  jit_bump_arg_handles<Args...>(n, args);
  int64_t pgen = jit_handle_gen(reinterpret_cast<JitObject*>(self.data));
  auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
    auto& r = (obj->*Mf)(
        jit_arg_get<Args>(jit_arg_or_default<T, Mf>(n, args, I))...);
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
// registry stdlib_rt.h merges with its static table.
template <class T>
class ClassBinder {
 public:
  ClassBinder(std::string ns, std::string name) : ns_(std::move(ns)) {
    assert(wrap_detail::jit_class_info<T>::name.empty() &&
           "wrap<T>: this class is already wrapped");
    wrap_detail::jit_class_info<T>::name = name;
    wrap_detail::jit_class_info<T>::opt_name = name + "?";
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
  ClassBinder& method(std::string name,
                      std::vector<wrap_detail::wrap_param> params = {},
                      wrap_policy policy = wrap_policy::standard) {
    using traits = culebra::fn_traits<decltype(Mf)>;
    return method_impl<Mf, typename traits::ret>(
        std::move(name), std::move(params), policy,
        static_cast<typename traits::args*>(nullptr));
  }

  // A method returning a reference INTO self (`T2&` / `const T2&` of a
  // wrapped class): the result is a borrowing handle validated against
  // this instance's closed flag and generation on every access.
  // Taking a borrow never bumps the generation.
  template <auto Mf>
  ClassBinder& borrowed_method(std::string name,
                               std::vector<wrap_detail::wrap_param> params = {}) {
    using traits = culebra::fn_traits<decltype(Mf)>;
    using R = typename traits::ret;
    static_assert(std::is_lvalue_reference_v<R>,
                  "wrap.h: borrowed_method needs a method returning T2& / "
                  "const T2& of a wrapped class");
    using T2 = std::remove_cv_t<std::remove_reference_t<R>>;
    static_assert(std::is_class_v<T2>,
                  "wrap.h: borrowed_method's referent must be a wrapped "
                  "class");
    return borrowed_impl<Mf, T2>(std::move(name), std::move(params),
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
  // A declared default that jit_arg_get could not honour. Only U*/const U*
  // reads nil as "no object"; a nil standing in for a U&/const U& would
  // dereference a nil handle, which no other declaration mistake in this
  // header can do (the rest are static_asserts). Assert-only, like the
  // already-wrapped check in the constructor -- the `linux-assert` lane and
  // `just test-assert` are where a binding gets to find out.
  template <class... Args>
  static void assert_defaults_declarable(
      const std::vector<wrap_detail::wrap_param>& params) {
    if constexpr (sizeof...(Args) > 0) {
      static constexpr bool kNeedsHandle[] = {
          (!std::is_void_v<typename wrap_detail::handle_target<Args>::type> &&
           !wrap_detail::handle_target<Args>::is_pointer)...};
      for (size_t i = 0; i < sizeof...(Args); i++) {
        assert((!params[i].has_default || !kNeedsHandle[i]) &&
               "wrap.h: a U& / const U& parameter cannot default -- only "
               "U* / const U* reads nil as no object");
      }
    }
  }

  // Splits a pinned wrap_param list into the two flat vectors
  // _jit_make_handle_meta wants (names, has-default bits) — shared by
  // method_impl and borrowed_impl.
  static std::pair<std::vector<std::string>, std::vector<bool>>
  split_wrap_params(const std::vector<wrap_detail::wrap_param>& params) {
    std::vector<std::string> names;
    std::vector<bool> has_default;
    names.reserve(params.size());
    has_default.reserve(params.size());
    for (const auto& p : params) {
      names.push_back(p.name);
      has_default.push_back(p.has_default);
    }
    return {std::move(names), std::move(has_default)};
  }

  template <auto Mf, class R, class... Args>
  ClassBinder& method_impl(std::string name,
                           std::vector<wrap_detail::wrap_param> params,
                           wrap_policy policy, std::tuple<Args...>*) {
    const bool bump =
        !wrap_detail::is_const_member<decltype(Mf)>::value &&
        policy != wrap_policy::preserves_borrows;
    auto pinned =
        wrap_detail::pin_wrap_params(std::move(params), sizeof...(Args));
    assert_defaults_declarable<Args...>(pinned);
    auto [names, has_default] = split_wrap_params(pinned);
    wrap_detail::jit_method_info<T, Mf>::params = std::move(pinned);
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(std::move(names), std::move(has_default));
    wrap_detail::jit_class_info<T>::methods.push_back(
        {std::move(name),
         bump ? &wrap_detail::jit_method_thunk<T, Mf, R, true, Args...>
              : &wrap_detail::jit_method_thunk<T, Mf, R, false, Args...>,
         sizeof...(Args), meta});
    return *this;
  }

  template <auto Mf, class T2, class... Args>
  ClassBinder& borrowed_impl(std::string name,
                             std::vector<wrap_detail::wrap_param> params,
                             std::tuple<Args...>*) {
    auto pinned =
        wrap_detail::pin_wrap_params(std::move(params), sizeof...(Args));
    assert_defaults_declarable<Args...>(pinned);
    auto [names, has_default] = split_wrap_params(pinned);
    wrap_detail::jit_method_info<T, Mf>::params = std::move(pinned);
    const JitParamMeta* meta =
        sizeof...(Args) == 0
            ? nullptr
            : _jit_make_handle_meta(std::move(names), std::move(has_default));
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
