#pragma once
// Declarative C++ class wrapping — design §10 / §14.3 Phase 4 (P4-1).
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
//         .method("value", &mylib::Counter::value)
//         .method("add", &mylib::Counter::add, {"n"})
//         .static_method("live", &mylib::Counter::live);
//     return true;
//   }();
//
// Instances ride the Foreign id table (foreign.h): `drop` erases the
// entry (~T() runs NOW), a dropped instance's method raises ClosedError,
// and the handle — an ordinary drop-having Object — gets the whole
// Phase 1/2 lifetime machinery (scope-exit drop, cycles, GC backstop,
// exactly-once) for free. Return values lower per §10.3: a by-value or
// unique_ptr<U> return becomes an owning handle of U, shared_ptr<U>
// holds one share, primitives go through cpp_to_value. U must itself be
// wrapped (its class_info carries the display name the handle shows).
//
// Phase 4 sub-phasing: this header covers the interp artifacts (P4-1);
// the JIT thunk generation + NsMethod registry rows land in P4-2, and
// the `culebra wrap` build pipeline in P4-3.

#include <foreign.h>
#include <interpreter.h>

#include <cassert>
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
// `this` per invocation), so each handle just copies the shared Values
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
      env->get("this").to_object().get("_id").to_long());
}

// The live instance, or ClosedError (§7) — every method's first step.
template <class T>
inline T* handle_self(const std::shared_ptr<Environment>& env) {
  long line = env->get("__LINE__").to_long();
  long col = env->get("__COLUMN__").to_long();
  return foreign::get_or_throw<T>(handle_id<T>(env), class_info<T>::name,
                                  line, col);
}

// The per-instance handle: data slots + the shared method prototypes +
// the §9 drop event (erase the table entry — ~T() runs NOW; idempotent,
// exactly-once via the Phase 1 `dropped` flag on the handle).
template <class T>
Value make_foreign_handle(int64_t id) {
  ObjectValue h;
  h.initialize("__foreign__", Value(std::string(class_info<T>::name)), false);
  h.initialize("_id", Value(static_cast<long>(id)), false);
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

// Return-value lowering (§10.3). `R` decayed: handles for wrapped
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
    // A wrapped-class reference return is not a §10.3 ownership shape:
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
// convert each declared param, call, lower the return.
template <class T, class Mf, class R, class... Args>
inline Value make_method_proto(Mf mf, std::vector<std::string> names) {
  auto storage = pin_param_names(std::move(names), sizeof...(Args));
  auto eval = [storage, mf](std::shared_ptr<Environment> env) -> Value {
    T* self = handle_self<T>(env);
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
    return invoke(std::index_sequence_for<Args...>{});
  };
  return Value(FunctionValue(typed_params<Args...>(*storage),
                             std::move(eval), return_annotation<R>()));
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
    return invoke(std::index_sequence_for<Args...>{});
  };
  return Value(FunctionValue(typed_params<Args...>(*storage),
                             std::move(eval), return_annotation<R>()));
}

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
                   make, std::move(names)));
    return *this;
  }

  template <class R, class... Args>
  ClassBinder& method(std::string name, R (T::*mf)(Args...),
                      std::vector<std::string> names = {}) {
    wrap_detail::class_info<T>::methods.emplace_back(
        std::move(name), wrap_detail::make_method_proto<T, decltype(mf), R,
                                                        Args...>(
                             mf, std::move(names)));
    return *this;
  }
  template <class R, class... Args>
  ClassBinder& method(std::string name, R (T::*mf)(Args...) const,
                      std::vector<std::string> names = {}) {
    wrap_detail::class_info<T>::methods.emplace_back(
        std::move(name), wrap_detail::make_method_proto<T, decltype(mf), R,
                                                        Args...>(
                             mf, std::move(names)));
    return *this;
  }

  template <class R, class... Args>
  ClassBinder& static_method(std::string name, R (*fn)(Args...),
                             std::vector<std::string> names = {}) {
    statics_.emplace_back(
        std::move(name),
        wrap_detail::make_static_proto<decltype(fn), R, Args...>(
            fn, std::move(names)));
    return *this;
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
  std::string ns_;
  std::string name_;
  std::vector<std::pair<std::string, Value>> statics_;
};

template <class T>
ClassBinder<T> wrap(std::string ns, std::string name) {
  return ClassBinder<T>(std::move(ns), std::move(name));
}

}  // namespace culebra
