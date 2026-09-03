#pragma once

// Extract a callable's argument types and return type: a function pointer,
// a member function pointer (const or not — the non-const arm also matches
// a mutable lambda / non-const functor's `operator()`), or `std::function`.
// Shared by wrap.h's ClassBinder (binding a C++ method) and vm_embed.h's
// Embed::define (binding a host callable) to introspect the signature
// before generating its thunk.

#include <functional>
#include <tuple>
#include <type_traits>

namespace culebra {

template <class T>
struct fn_traits : fn_traits<decltype(&std::decay_t<T>::operator())> {};

template <class R, class... Args>
struct fn_traits<R (*)(Args...)> {
  using ret = R;
  using args = std::tuple<Args...>;
};

template <class C, class R, class... Args>
struct fn_traits<R (C::*)(Args...) const> {
  using ret = R;
  using args = std::tuple<Args...>;
};

template <class C, class R, class... Args>
struct fn_traits<R (C::*)(Args...)> {  // mutable lambda / non-const functor
  using ret = R;
  using args = std::tuple<Args...>;
};

template <class R, class... Args>
struct fn_traits<std::function<R(Args...)>> {
  using ret = R;
  using args = std::tuple<Args...>;
};

}  // namespace culebra
