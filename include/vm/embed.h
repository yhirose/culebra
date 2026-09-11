#pragma once

// Embedding a culebra engine in a C++ host, on the bytecode VM — the
// successor to the tree-walker's environment()/interpret/call/define surface
// (docs/deployment.md §2). One `Embed` is what one `env` shared_ptr used to
// be: a session whose top-level bindings outlive the programs that made
// them, so the host can run a script and then read its globals or call its
// functions; `vm::Value` is the owning value handle the host passes and
// receives (retain/release stay inside it, so the RC discipline never leaks
// into embedder code).
//
// Threading and isolation follow the Runtime contract unchanged: the caller
// scopes a `culebra::Runtime` + `RuntimeScope` per independent engine
// instance, and each Embed carries its own ReplSession (two Embeds on one
// thread do not share globals — the mi_smoke contract). Every public method
// swaps this Embed's session in for its own duration.

#include <base/fn_traits.h>
#include <vm/script_teardown.h>  // ScriptTeardownGuard (run boundaries)
#include <stdlib/bindings.h>       // install_jit_stdlib, the runtime helpers
#include <vm/vm.h>
#include <vm/session.h>

#include <bit>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace culebra::vm {

class Value;
namespace _embed_detail {
class Arg;
}

// ---------------------------------------------------------------------------
// Value — an owning RAII handle over the runtime's tagged value. The tag and
// payload are visible (advanced hosts can `get()` a borrowed JitValue), but
// every retain/release lives here.
// ---------------------------------------------------------------------------
class Value {
 public:
  Value() : v_{TAG_NIL, 0} {}
  explicit Value(bool b) : v_{TAG_BOOL, b ? 1 : 0} {}
  explicit Value(int64_t n) : v_{TAG_LONG, n} {}
  explicit Value(double d) : v_(jit_float(d)) {}
  explicit Value(std::string_view s)
      : v_{TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(s))} {}
  // Without this, a string literal would take the BOOL ctor (pointer->bool
  // is a standard conversion, ->string_view is user-defined).
  explicit Value(const char* s) : Value(std::string_view(s)) {}

  // Adopt an OWNED runtime value (a +1 the caller holds transfers here).
  static Value adopt(JitValue v) { return Value(v, Adopt{}); }
  // Borrow: retain a value someone else owns and wrap the new reference.
  static Value borrow(JitValue v) {
    culebra_runtime_value_retain(v.tag, v.data);
    return Value(v, Adopt{});
  }

  Value(const Value& o) : v_(o.v_) {
    culebra_runtime_value_retain(v_.tag, v_.data);
  }
  Value(Value&& o) noexcept : v_(o.v_) { o.v_ = {TAG_NIL, 0}; }
  Value& operator=(Value o) noexcept {
    std::swap(v_, o.v_);
    return *this;
  }
  ~Value() { _culebra_value_release_impl(v_.tag, v_.data); }

  // Reads are lenient: a mismatched tag yields false/0/empty rather than
  // throwing (check is_nil() / get().tag to distinguish); the strict typed
  // rejection lives on the define() boundary, where the script-visible
  // TypeError belongs.
  bool is_nil() const { return v_.tag == TAG_NIL; }
  bool is_function() const { return v_.tag == TAG_FUNC; }
  bool to_bool() const { return v_.tag == TAG_BOOL && v_.data != 0; }
  int64_t to_long() const { return v_.tag == TAG_LONG ? v_.data : 0; }
  double to_double() const {
    if (v_.tag == TAG_LONG) return static_cast<double>(v_.data);
    if (v_.tag != TAG_FLOAT) return 0.0;
    return std::bit_cast<double>(v_.data);
  }
  std::string to_string() const {
    if (v_.tag != TAG_STRING && v_.tag != TAG_STRINGVIEW) return {};
    return std::string(_str_sv(reinterpret_cast<const char*>(v_.data)));
  }
  // The value rendered the way `inspect` renders it — for diagnostics.
  std::string display() const {
    return _culebra_uncaught_display(v_.tag, v_.data);
  }

  // --- What this value is --------------------------------------------------
  bool is_long() const { return v_.tag == TAG_LONG; }
  bool is_float() const { return v_.tag == TAG_FLOAT; }
  bool is_bool() const { return v_.tag == TAG_BOOL; }
  bool is_string() const {
    return v_.tag == TAG_STRING || v_.tag == TAG_STRINGVIEW;
  }
  bool is_array() const { return v_.tag == TAG_ARRAY || v_.tag == TAG_TUPLE; }
  bool is_object() const { return v_.tag == TAG_OBJECT; }
  // The name the script's own `type_of` gives this value ('Long', 'Object').
  const char* type_name() const { return _culebra_tag_name(v_.tag); }

  // The strict read, where to_long() and friends hand back a silent 0: a
  // mismatch raises the same TypeError text the script's accessors raise.
  // T is the define() set (long/int/double/float/bool/std::string/
  // std::string_view/Value) plus std::vector<T>, std::map<std::string, T>
  // and std::optional<T>. A std::string_view borrows this value's bytes.
  template <class T>
  T as() const;

  // --- As a container ------------------------------------------------------
  // Array/Tuple length, or an Object's entry count (TypeError otherwise).
  int64_t size() const {
    if (v_.tag == TAG_ARRAY || v_.tag == TAG_TUPLE)
      return culebra_runtime_array_size(reinterpret_cast<JitArray*>(v_.data));
    if (v_.tag == TAG_OBJECT)
      return culebra_runtime_object_size(reinterpret_cast<JitObject*>(v_.data));
    culebra_runtime_type_error_typed(0, 0, "Array, Tuple, or Object", v_.tag);
  }

  // `obj.has(key)`: the lookup question, so a class method answers it too.
  bool has(std::string_view key) const {
    if (v_.tag != TAG_OBJECT)
      culebra_runtime_type_error_typed(0, 0, "Object", v_.tag);
    std::string k(key);
    return culebra_runtime_object_has(reinterpret_cast<JitObject*>(v_.data),
                                      k.c_str());
  }

  // `recv[key]`, with the script's semantics unchanged: an Object miss is a
  // KeyError, an out-of-range index an IndexError, another receiver a
  // TypeError. Object and Array are references, so what comes back IS the
  // parent's value — set()/push() through it write into the parent.
  Value operator[](std::string_view key) const {
    if (v_.tag != TAG_OBJECT)
      culebra_runtime_type_error_typed(0, 0, "Object", v_.tag);
    std::string k(key);  // a String key is borrowed bytes, never consumed
    int8_t t;
    int64_t d;
    culebra_runtime_object_get_any(reinterpret_cast<JitObject*>(v_.data),
                                   TAG_STRING,
                                   reinterpret_cast<int64_t>(k.c_str()), &t, &d,
                                   0, 0, /*own_receiver=*/false);
    return Value(JitValue{t, d}, Adopt{});
  }
  Value operator[](int64_t idx) const { return at(Value(idx)); }
  // Any hashable key (a Long index, a Tuple, ...) — Op::Index's dispatch.
  Value at(const Value& key) const {
    const JitValue k = key.v_;
    if (v_.tag == TAG_ARRAY || v_.tag == TAG_TUPLE) {
      if (k.tag != TAG_LONG)
        culebra_runtime_type_error_typed(0, 0, "Long", k.tag);
      int8_t t;
      int64_t d;
      culebra_runtime_array_get(reinterpret_cast<JitArray*>(v_.data), k.data,
                                &t, &d, 0, 0);
      culebra_runtime_value_retain(t, d);  // array_get borrows the slot
      return Value(JitValue{t, d}, Adopt{});
    }
    if (v_.tag == TAG_OBJECT) {
      // object_get_any consumes a non-String key on every path.
      culebra_runtime_value_retain(k.tag, k.data);
      int8_t t;
      int64_t d;
      culebra_runtime_object_get_any(reinterpret_cast<JitObject*>(v_.data),
                                     k.tag, k.data, &t, &d, 0, 0,
                                     /*own_receiver=*/false);
      return Value(JitValue{t, d}, Adopt{});
    }
    culebra_runtime_type_error_typed(0, 0, "Array", v_.tag);
  }

  // An Object's entries in insertion order — `for k, v in obj`'s set (own
  // entries only, non-String keys included). A snapshot: the host may write
  // to the object while walking it.
  std::vector<std::pair<Value, Value>> items() const {
    if (v_.tag != TAG_OBJECT)
      culebra_runtime_type_error_typed(0, 0, "Object", v_.tag);
    auto* obj = reinterpret_cast<JitObject*>(v_.data);
    Value keys = Value(
        JitValue{TAG_ARRAY,
                 reinterpret_cast<int64_t>(culebra_runtime_object_keys(obj))},
        Adopt{});
    std::vector<std::pair<Value, Value>> out;
    out.reserve(static_cast<size_t>(keys.size()));
    for (int64_t i = 0; i < keys.size(); i++) {
      Value k = keys[i];
      out.emplace_back(k, at(k));
    }
    return out;
  }

  // Array/Tuple elements, for range-for (defined below — it stashes the
  // element it is on, so `for (auto& e : arr)` binds to a Value that lives
  // as long as the loop step).
  class iterator;
  iterator begin() const;
  iterator end() const;

  // --- Writing -------------------------------------------------------------
  // `recv[key] = v` / `arr.push(v)`. The key's own rules hold: a property
  // declared without `mut` raises ImmutableError here too, and an index past
  // the end of an Array is an IndexError (push is how an Array grows).
  void set(std::string_view key, _embed_detail::Arg v);
  void set(int64_t idx, _embed_detail::Arg v);
  void set(const Value& key, _embed_detail::Arg v);
  void push(_embed_detail::Arg v);

  // Build one for the script: the entries are mutable, like a spread's.
  static Value array(std::initializer_list<_embed_detail::Arg> items);
  static Value object(
      std::initializer_list<std::pair<std::string_view, _embed_detail::Arg>>
          entries);

  JitValue get() const { return v_; }  // borrowed view
  // Hand the owned reference out (the caller now holds the +1).
  JitValue release() {
    JitValue v = v_;
    v_ = {TAG_NIL, 0};
    return v;
  }

 private:
  struct Adopt {};
  Value(JitValue v, Adopt) : v_(v) {}
  JitValue v_;
};

// One Array element at a time, stashed so `*it` is a reference with the
// loop step's lifetime. Input-iterator shape: one pass, and `end()` compares
// on the index alone.
class Value::iterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = Value;
  using difference_type = std::ptrdiff_t;
  using reference = const Value&;
  using pointer = const Value*;

  iterator() = default;
  iterator(const Value* owner, int64_t i) : owner_(owner), i_(i) { load(); }

  const Value& operator*() const { return cur_; }
  const Value* operator->() const { return &cur_; }
  iterator& operator++() {
    ++i_;
    load();
    return *this;
  }
  void operator++(int) { ++*this; }
  bool operator==(const iterator& o) const { return i_ == o.i_; }

 private:
  void load() {
    if (owner_ && i_ < owner_->size()) cur_ = (*owner_)[i_];
    else cur_ = Value();
  }
  const Value* owner_ = nullptr;
  int64_t i_ = 0;
  Value cur_;
};

inline Value::iterator Value::begin() const {
  if (!is_array()) culebra_runtime_type_error_typed(0, 0, "Array", v_.tag);
  return iterator(this, 0);
}
inline Value::iterator Value::end() const {
  return iterator(nullptr, is_array() ? size() : 0);
}

// ---------------------------------------------------------------------------
// Host-function bridge. One generic trampoline serves every define(): the
// closure's captures[0] carries an index into a process-wide registry of
// std::function adapters (a per-Embed registry would need per-Embed code
// addresses, which C++ cannot mint; per-Embed NAME resolution still holds
// because the binding lives in each Embed's own session cells).
// ---------------------------------------------------------------------------
namespace _embed_detail {

using HostFn = std::function<JitValue(int64_t n, JitValue* args)>;

inline std::deque<HostFn>& host_fns() {
  static std::deque<HostFn> fns;  // stable addresses; grows only
  return fns;
}

// The closure ABI (see _jit_invoke): args arrive OWNED by the callee, the
// result leaves owned. The adapter consumes the args; a C++ exception from
// the host body converts at this boundary the way wrap.h's
// surface_native_error does, so the script sees a catchable RuntimeError.
inline void trampoline(JitValue* __ret, JitClosure* self, int8_t self_tag,
                       int64_t self_data, int64_t n, JitValue* args) {
  _culebra_value_release_impl(self_tag, self_data);  // bound-self, unused
  JitMethodArgs _a{n, args};  // releases the +1 args on every exit path
  auto idx = static_cast<size_t>(self->captures[0]->value.data);
  try {
    *__ret = host_fns()[idx](n, args);
  } catch (const culebra::CulebraError&) {
    throw;
  } catch (const CulebraException&) {
    throw;  // a script throw from a re-entrant call — pass it through raw
  } catch (const std::exception& e) {
    throw culebra::CulebraError("RuntimeError", e.what(), 0, 0);
  }
}

// C++ parameter <- runtime value. The supported set matches the interp-era
// define(): long/int/double/float/bool/std::string/std::string_view/Value.
template <class T>
struct FromJit;
struct ArgCtx {
  std::string_view fn;    // empty at a host read (Value::as<T>)
  std::string_view param;
};
[[noreturn]] inline void _arg_type_error(ArgCtx c, const char* expected,
                                         int8_t got) {
  // A host read has no argument to name, so it reports what the script's own
  // accessors report for the same mismatch.
  if (c.fn.empty()) culebra_runtime_type_error_typed(0, 0, expected, got);
  throw culebra::CulebraError(
      "TypeError", std::string(c.fn) + "() argument '" + std::string(c.param) +
                       "': expected a " + expected);
}
// int64_t aliases `long` on LP64 and `long long` on macOS/LLP64; specialize
// the fundamental types so each ABI sees exactly one definition per type.
template <>
struct FromJit<long long> {
  static long long get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_LONG) _arg_type_error(c, "Long", v.tag);
    return v.data;
  }
};
template <>
struct FromJit<long> {
  static long get(const JitValue& v, ArgCtx c) {
    return static_cast<long>(FromJit<long long>::get(v, c));
  }
};
template <>
struct FromJit<int> {
  static int get(const JitValue& v, ArgCtx c) {
    return static_cast<int>(FromJit<long long>::get(v, c));
  }
};
template <>
struct FromJit<double> {
  static double get(const JitValue& v, ArgCtx c) {
    if (v.tag == TAG_LONG) return static_cast<double>(v.data);
    if (v.tag != TAG_FLOAT) _arg_type_error(c, "Float", v.tag);
    return std::bit_cast<double>(v.data);
  }
};
template <>
struct FromJit<float> {
  static float get(const JitValue& v, ArgCtx c) {
    return static_cast<float>(FromJit<double>::get(v, c));
  }
};
template <>
struct FromJit<bool> {
  static bool get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_BOOL) _arg_type_error(c, "Bool", v.tag);
    return v.data != 0;
  }
};
template <>
struct FromJit<std::string> {
  static std::string get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
      _arg_type_error(c, "String", v.tag);
    return std::string(_str_sv(reinterpret_cast<const char*>(v.data)));
  }
};
template <>
struct FromJit<std::string_view> {
  static std::string_view get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
      _arg_type_error(c, "String", v.tag);
    return _str_sv(reinterpret_cast<const char*>(v.data));
  }
};
template <>
struct FromJit<Value> {
  static Value get(const JitValue& v, ArgCtx) { return Value::borrow(v); }
};

// Containers, element-wise through the same specializations — so a
// std::vector<std::string> parameter rejects [1, 2] with the String
// TypeError its elements would have raised one at a time.
template <class T>
struct FromJit<std::vector<T>> {
  static std::vector<T> get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_ARRAY && v.tag != TAG_TUPLE)
      _arg_type_error(c, "Array", v.tag);
    auto* arr = reinterpret_cast<JitArray*>(v.data);
    std::vector<T> out;
    out.reserve(arr->size);
    for (size_t i = 0; i < arr->size; i++)
      out.push_back(FromJit<T>::get(arr->items[i], c));
    return out;
  }
};
template <class T>
struct FromJit<std::map<std::string, T>> {
  static std::map<std::string, T> get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_OBJECT) _arg_type_error(c, "Object", v.tag);
    auto* obj = reinterpret_cast<JitObject*>(v.data);
    Value keys = Value::adopt(
        {TAG_ARRAY,
         reinterpret_cast<int64_t>(culebra_runtime_object_keys(obj))});
    std::map<std::string, T> out;
    for (int64_t i = 0; i < keys.size(); i++) {
      Value k = keys[i];
      // A std::map<std::string, T> names String keys; an Object holding a
      // Long or Tuple key cannot round-trip into one.
      if (!k.is_string()) _arg_type_error(c, "Object with String keys", v.tag);
      Value e = Value::borrow(v).at(k);
      out.emplace(k.to_string(), FromJit<T>::get(e.get(), c));
    }
    return out;
  }
};
template <class T>
struct FromJit<std::optional<T>> {
  static std::optional<T> get(const JitValue& v, ArgCtx c) {
    if (v.tag == TAG_NIL) return std::nullopt;
    return FromJit<T>::get(v, c);
  }
};

// C++ return -> owned runtime value.
inline JitValue to_jit(long long n) {
  return {TAG_LONG, static_cast<int64_t>(n)};
}
inline JitValue to_jit(long n) { return {TAG_LONG, static_cast<int64_t>(n)}; }
inline JitValue to_jit(int n) { return {TAG_LONG, n}; }
inline JitValue to_jit(double d) { return jit_float(d); }
inline JitValue to_jit(float d) { return jit_float(d); }
inline JitValue to_jit(bool b) { return {TAG_BOOL, b ? 1 : 0}; }
inline JitValue to_jit(std::string_view s) {
  return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(s))};
}
inline JitValue to_jit(const std::string& s) {
  return to_jit(std::string_view(s));
}
inline JitValue to_jit(const char* s) { return to_jit(std::string_view(s)); }
inline JitValue to_jit(Value v) { return v.release(); }
inline JitValue to_jit(std::nullptr_t) { return {TAG_NIL, 0}; }

template <class T>
JitValue to_jit(const std::vector<T>& xs) {
  auto* arr = culebra_runtime_array_new();
  JitValue out{TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
  // An element's conversion cannot throw today, but the array is live from
  // here on; hold it so a future one that does cannot strand it.
  Value guard = Value::adopt(out);
  for (const auto& x : xs) {
    auto e = to_jit(x);
    culebra_runtime_array_push(arr, e.tag, e.data);  // absorbs the +1
  }
  return guard.release();
}
template <class T>
JitValue to_jit(const std::map<std::string, T>& m) {
  auto* obj = culebra_runtime_object_new();
  Value guard =
      Value::adopt({TAG_OBJECT, reinterpret_cast<int64_t>(obj)});
  for (const auto& [k, x] : m) {
    auto e = to_jit(x);
    culebra_runtime_object_set(obj, k.c_str(), /*mut=*/true, e.tag, e.data, 0,
                               0, /*is_init=*/true);
  }
  return guard.release();
}
template <class T>
JitValue to_jit(const std::optional<T>& o) {
  if (!o) return {TAG_NIL, 0};
  return to_jit(*o);
}

// ---------------------------------------------------------------------------
// Arg — a value the host spells inline: at set/push/call and the two builders
// the scalar conversions `Value` keeps explicit are implicit, so
// `cfg.set("port", 8080)` needs no cast. Holds one +1; `owned()` mints the
// one its consumer absorbs.
// ---------------------------------------------------------------------------
class Arg {
 public:
  Arg(Value v) : v_(v.release()) {}
  Arg(std::nullptr_t) : v_{TAG_NIL, 0} {}
  Arg(bool b) : v_(to_jit(b)) {}
  Arg(int n) : v_(to_jit(n)) {}
  Arg(long n) : v_(to_jit(n)) {}
  Arg(long long n) : v_(to_jit(n)) {}
  Arg(double d) : v_(to_jit(d)) {}
  Arg(float d) : v_(to_jit(d)) {}
  Arg(const char* s) : v_(to_jit(s)) {}
  Arg(std::string_view s) : v_(to_jit(s)) {}
  Arg(const std::string& s) : v_(to_jit(s)) {}
  template <class T>
  Arg(const std::vector<T>& xs) : v_(to_jit(xs)) {}
  template <class T>
  Arg(const std::map<std::string, T>& m) : v_(to_jit(m)) {}
  template <class T>
  Arg(const std::optional<T>& o) : v_(to_jit(o)) {}

  Arg(const Arg& o) : v_(o.v_) {  // initializer_list copies its elements out
    culebra_runtime_value_retain(v_.tag, v_.data);
  }
  Arg& operator=(const Arg&) = delete;
  ~Arg() { _culebra_value_release_impl(v_.tag, v_.data); }

  JitValue owned() const {
    culebra_runtime_value_retain(v_.tag, v_.data);
    return v_;
  }

 private:
  JitValue v_;
};

template <class Fn, class R, class... A>
HostFn make_adapter(Fn fn, std::string name,
                    std::vector<std::string> param_names, std::tuple<A...>*) {
  return [fn = std::move(fn), name = std::move(name),
          param_names = std::move(param_names)](
             int64_t n, JitValue* args) -> JitValue {
    if (n != static_cast<int64_t>(sizeof...(A))) {
      throw culebra::CulebraError(
          "ArityError", name + "() expects " +
                            std::to_string(sizeof...(A)) + " argument(s), got " +
                            std::to_string(n));
    }
    auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> JitValue {
      if constexpr (std::is_void_v<R>) {
        fn(FromJit<std::decay_t<A>>::get(args[I],
                                         {name, param_names[I]})...);
        return {TAG_NIL, 0};
      } else {
        return to_jit(fn(FromJit<std::decay_t<A>>::get(
            args[I], {name, param_names[I]})...));
      }
    };
    return invoke(std::index_sequence_for<A...>{});
  };
}

}  // namespace _embed_detail

// ---------------------------------------------------------------------------
// Value's host-facing writes and reads, defined once `Arg` and the FromJit
// specializations they name are complete.
// ---------------------------------------------------------------------------

template <class T>
T Value::as() const {
  return _embed_detail::FromJit<T>::get(v_, _embed_detail::ArgCtx{});
}

inline void Value::set(std::string_view key, _embed_detail::Arg v) {
  if (v_.tag != TAG_OBJECT)
    culebra_runtime_type_error_typed(0, 0, "Object", v_.tag);
  std::string k(key);  // a String key is borrowed bytes, never consumed
  const JitValue val = v.owned();  // the store absorbs this +1
  culebra_runtime_object_set_any(reinterpret_cast<JitObject*>(v_.data),
                                 TAG_STRING,
                                 reinterpret_cast<int64_t>(k.c_str()),
                                 /*mut=*/true, val.tag, val.data, 0, 0,
                                 /*is_init=*/false);
}

inline void Value::set(int64_t idx, _embed_detail::Arg v) {
  set(Value(idx), std::move(v));
}

inline void Value::set(const Value& key, _embed_detail::Arg v) {
  const JitValue k = key.get();
  const JitValue val = v.owned();  // the store absorbs this +1
  if (v_.tag == TAG_ARRAY) {
    if (k.tag != TAG_LONG) {
      _culebra_value_release_impl(val.tag, val.data);
      culebra_runtime_type_error_typed(0, 0, "Long", k.tag);
    }
    culebra_runtime_array_set(reinterpret_cast<JitArray*>(v_.data), k.data,
                              val.tag, val.data, 0, 0);
    return;
  }
  if (v_.tag == TAG_OBJECT) {
    // object_set_any consumes the key and the value on every path.
    culebra_runtime_value_retain(k.tag, k.data);
    culebra_runtime_object_set_any(reinterpret_cast<JitObject*>(v_.data), k.tag,
                                   k.data, /*mut=*/true, val.tag, val.data, 0,
                                   0, /*is_init=*/false);
    return;
  }
  _culebra_value_release_impl(val.tag, val.data);
  culebra_runtime_type_error_typed(0, 0, "Array", v_.tag);
}

inline void Value::push(_embed_detail::Arg v) {
  if (v_.tag != TAG_ARRAY)
    culebra_runtime_type_error_typed(0, 0, "Array", v_.tag);
  const JitValue val = v.owned();
  culebra_runtime_array_push(reinterpret_cast<JitArray*>(v_.data), val.tag,
                             val.data);  // absorbs the +1
}

inline Value Value::array(std::initializer_list<_embed_detail::Arg> items) {
  Value out = Value::adopt(
      {TAG_ARRAY, reinterpret_cast<int64_t>(culebra_runtime_array_new())});
  for (const auto& item : items) out.push(item);
  return out;
}

inline Value Value::object(
    std::initializer_list<std::pair<std::string_view, _embed_detail::Arg>>
        entries) {
  Value out = Value::adopt(
      {TAG_OBJECT, reinterpret_cast<int64_t>(culebra_runtime_object_new())});
  auto* obj = reinterpret_cast<JitObject*>(out.get().data);
  for (const auto& [key, val] : entries) {
    std::string k(key);
    const JitValue v = val.owned();
    // is_init: the object-literal seat. `mut` because a host-built Object is
    // the host's own data — `{mut k: v}`, not `{k: v}`.
    culebra_runtime_object_set(obj, k.c_str(), /*mut=*/true, v.tag, v.data, 0,
                               0, /*is_init=*/true);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Embed — one engine session for a host. Construction installs the compiled
// stdlib and runs the built-in traits (so `Comparable` defaults etc. resolve
// even for a bare AST, the single_ast_smoke contract); destruction joins any
// isolates the session's programs left running and releases the session's
// cells.
// ---------------------------------------------------------------------------
class Embed {
 public:
  Embed() {
    install_jit_stdlib();
    Swap in(*this);
    std::vector<std::string> msgs;
    if (!units_.run_builtin_traits(msgs)) {
      // The traits preamble is baked into the binary; failing to run it is a
      // build bug, not a host-recoverable state.
      throw culebra::CulebraError("InternalError",
                                  "built-in traits failed: " + join(msgs));
    }
  }

  ~Embed() {
    ScriptTeardownGuard teardown;  // joins isolates, closes FS.watch handles
    Swap in(*this);
    cells_.release_all();
  }

  Embed(const Embed&) = delete;
  Embed& operator=(const Embed&) = delete;

  // Run a loader's whole module list (interpret_modules' seat): true on
  // success with the entry module's last value in `result`; false with the
  // error text in `msgs` (the same text every engine prints for it).
  bool run(const std::vector<LoadedModule>& modules, Value& result,
           std::vector<std::string>& msgs) {
    Swap in(*this);
    return reporting(msgs, [&] {
      if (!units_.run_modules(modules, msgs)) return false;
      result = Value::adopt(units_.take_result());
      return true;
    });
  }

  // Run one parsed input against the session (interpret's seat): the input
  // sees every earlier run's top-level bindings and its own land beside them.
  // `source` is the buffer the AST's tokens view into — the session must own
  // it as long as the program (a closure the input builds keeps its bytecode
  // and, through the AST, its tokens; a dropped buffer turns every name into
  // garbage — the dangling-string_view trap measured in B6b and again here).
  bool run(const std::shared_ptr<peg::Ast>& ast,
           std::shared_ptr<std::string> source, Value& result,
           std::vector<std::string>& msgs) {
    Swap in(*this);
    return reporting(msgs, [&] {
      if (!units_.run_stdlib_delta(*ast, msgs)) return false;
      if (!units_.run_unit(ast, std::move(source), /*session=*/true, msgs))
        return false;
      result = Value::adopt(units_.take_result());
      return true;
    });
  }

  // Run one input and hand its value back, reporting a failure the way `call`
  // reports one — a CulebraError with kind/line/col — instead of a message
  // list. The entry for a host that has no console to print `msgs` to.
  // (An uncaught script `throw` arrives as kind "RuntimeError" carrying the
  // "uncaught: ..." text: Exec flattens the thrown object at the engine
  // boundary, the same line every lane prints for one.)
  Value eval(std::string_view source, std::string_view name = "<inline>") {
    Value result;
    std::vector<std::string> msgs;
    units_.clear_last_error();
    if (run_source(name, source, result, msgs)) return result;
    if (const auto& e = units_.last_error()) throw *e;
    // A parse failure never reaches the session — the text is all there is.
    throw culebra::CulebraError("SyntaxError", join(msgs));
  }

  // Parse-and-run convenience: copies `source`, parses it under `name`, and
  // runs it — the recommended host entry (ownership cannot be got wrong).
  bool run_source(std::string_view name, std::string_view source,
                  Value& result, std::vector<std::string>& msgs) {
    auto owned = std::make_shared<std::string>(source);
    auto ast = parse_with_transforms(std::string(name), *owned, msgs);
    if (!ast) return false;
    return run(ast, std::move(owned), result, msgs);
  }

  // A top-level binding's current value, or nil if no run declared it.
  Value global(std::string_view name) {
    Swap in(*this);
    if (!repl_session().declared(name)) return Value();
    return Value::borrow(repl_session().value(name));
  }

  // Call a session function by name (call's seat). Throws CulebraError for
  // an unknown/non-function name and for anything the call itself raises —
  // a script `throw` arrives with its object's kind/message, so the
  // "catch CulebraError" contract carries over unchanged.
  Value call(std::string_view name, std::vector<Value> args = {}) {
    Swap in(*this);
    JitValue fn = repl_session().value(name);
    if (fn.tag != TAG_FUNC) {
      throw culebra::CulebraError(
          "TypeError", "'" + std::string(name) + "' is not a session function");
    }
    std::vector<JitValue> vals;
    vals.reserve(args.size());
    for (auto& a : args) vals.push_back(a.release());  // callee consumes
    try {
      return Value::adopt(
          _jit_invoke(reinterpret_cast<JitClosure*>(fn.data),
                      JitValue{TAG_NO_SELF, 0},
                      static_cast<int64_t>(vals.size()),
                      vals.empty() ? nullptr : vals.data()));
    } catch (const CulebraException& e) {
      // A script `throw v` crosses the compiled boundary as a tagged pair;
      // surface it structured.
      JitValue v{e.tag, e.data};
      std::string kind = "RuntimeError", message;
      describe_thrown_value(v, kind, message);
      _culebra_value_release_impl(v.tag, v.data);
      throw culebra::CulebraError(kind, message);
    }
  }

  // The same call with its arguments spelled inline: every scalar `Value`
  // constructs from, plus vector / map / optional, converts here. The
  // `std::vector<Value>` form above stays the way to pass values the host
  // already holds.
  template <class... Ts>
    requires(sizeof...(Ts) > 0 &&
             !(sizeof...(Ts) == 1 &&
               (std::same_as<std::remove_cvref_t<Ts>, std::vector<Value>> &&
                ...)) &&
             (std::constructible_from<_embed_detail::Arg, Ts> && ...))
  Value call(std::string_view name, Ts&&... args) {
    std::vector<Value> vals;
    vals.reserve(sizeof...(Ts));
    (vals.push_back(Value::adopt(
         _embed_detail::Arg(std::forward<Ts>(args)).owned())),
     ...);
    return call(name, std::move(vals));
  }

  // Bind a host callable as a session function (define's seat). Parameter
  // types come from the callable's signature (the interp-era set:
  // long/int/double/float/bool/std::string/std::string_view/vm::Value);
  // binding is positional (see the note at the meta site below).
  template <class Fn>
  void define(std::string_view name, Fn&& fn,
              std::vector<std::string> param_names = {}) {
    using Traits = culebra::fn_traits<std::decay_t<Fn>>;
    using Args = typename Traits::args;
    constexpr size_t arity = std::tuple_size_v<Args>;
    Swap in(*this);
    param_names.resize(arity);
    for (size_t i = 0; i < arity; i++) {
      if (param_names[i].empty())
        param_names[i] = "_arg" + std::to_string(i);
    }
    auto& fns = _embed_detail::host_fns();
    // No kwargs meta: every define shares this one trampoline and the names
    // are known only per define, so there is no per-body metadata to carry.
    // Host functions bind positionally; the names feed the per-argument
    // TypeError text. A host needing keyword binding declares a wrap.h class
    // instead.
    fns.push_back(_embed_detail::make_adapter<std::decay_t<Fn>,
                                              typename Traits::ret>(
        std::forward<Fn>(fn), std::string(name), std::move(param_names),
        static_cast<Args*>(nullptr)));
    auto idx = static_cast<int64_t>(fns.size() - 1);
    auto* cls = culebra_runtime_closure_new(
        reinterpret_cast<void*>(&_embed_detail::trampoline), 1, arity,
        JIT_CLOSURE_NATIVE, /*meta=*/nullptr);
    cls->captures[0] = culebra_runtime_cell_new(TAG_LONG, idx);
    auto* cell = cells_.cell(name);
    _culebra_value_release_impl(cell->value.tag, cell->value.data);
    cell->value = JitValue{TAG_FUNC, reinterpret_cast<int64_t>(cls)};
    cells_.set_mut(name, false);
  }

 private:
  // An interrupt keeps the bool+msgs contract: the host that requested it
  // reads it back as the error text, not as an exception (signal_smoke).
  template <class Body>
  static bool reporting(std::vector<std::string>& msgs, Body&& body) {
    try {
      return body();
    } catch (const Interrupted& e) {
      msgs.push_back(format_error_message(e));
      return false;
    }
  }

  // Points the compiler/executor at THIS Embed's cells for one operation.
  // Distinct from vm.h's ReplSessionSwap, which owns a fresh throwaway
  // session and releases it on exit (the debugger's shape) — here the cells
  // are the Embed's own state and outlive every swap.
  struct Swap {
    ReplSession* saved;
    explicit Swap(Embed& e) : saved(current_repl_session()) {
      current_repl_session() = &e.cells_;
    }
    ~Swap() { current_repl_session() = saved; }
  };

  static std::string join(const std::vector<std::string>& msgs) {
    std::string out;
    for (const auto& m : msgs) {
      if (!out.empty()) out += "; ";
      out += m;
    }
    return out;
  }

  ReplSession cells_;  // this Embed's globals (per-Embed isolation)
  Session units_;      // retained programs + stdlib delta bookkeeping
};

}  // namespace culebra::vm
