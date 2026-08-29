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

#include <fn_traits.h>
#include <script_teardown.h>  // ScriptTeardownGuard (run boundaries)
#include <stdlib_jit.h>       // install_jit_stdlib, the runtime helpers
#include <vm.h>
#include <vm_session.h>

#include <bit>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace culebra::vm {

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
[[noreturn]] inline void _arg_type_error(std::string_view fn,
                                         std::string_view param,
                                         const char* expected) {
  throw culebra::CulebraError(
      "TypeError", std::string(fn) + "() argument '" + std::string(param) +
                       "': expected a " + expected);
}
struct ArgCtx {
  std::string_view fn;
  std::string_view param;
};
// int64_t aliases `long` on LP64 and `long long` on macOS/LLP64; specialize
// the fundamental types so each ABI sees exactly one definition per type.
template <>
struct FromJit<long long> {
  static long long get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_LONG) _arg_type_error(c.fn, c.param, "Long");
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
    if (v.tag != TAG_FLOAT) _arg_type_error(c.fn, c.param, "Float");
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
    if (v.tag != TAG_BOOL) _arg_type_error(c.fn, c.param, "Bool");
    return v.data != 0;
  }
};
template <>
struct FromJit<std::string> {
  static std::string get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
      _arg_type_error(c.fn, c.param, "String");
    return std::string(_str_sv(reinterpret_cast<const char*>(v.data)));
  }
};
template <>
struct FromJit<std::string_view> {
  static std::string_view get(const JitValue& v, ArgCtx c) {
    if (v.tag != TAG_STRING && v.tag != TAG_STRINGVIEW)
      _arg_type_error(c.fn, c.param, "String");
    return _str_sv(reinterpret_cast<const char*>(v.data));
  }
};
template <>
struct FromJit<Value> {
  static Value get(const JitValue& v, ArgCtx) { return Value::borrow(v); }
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
    // No kwargs meta: the param-meta table is keyed by fn_ptr and every
    // define shares this one trampoline, so per-define names cannot be
    // registered there. Host functions bind positionally; the names feed
    // the per-argument TypeError text. A host needing keyword binding
    // declares a wrap.h class instead.
    fns.push_back(_embed_detail::make_adapter<std::decay_t<Fn>,
                                              typename Traits::ret>(
        std::forward<Fn>(fn), std::string(name), std::move(param_names),
        static_cast<Args*>(nullptr)));
    auto idx = static_cast<int64_t>(fns.size() - 1);
    _jit_register_native_fn(
        reinterpret_cast<const void*>(&_embed_detail::trampoline));
    auto* cls = culebra_runtime_closure_new(
        reinterpret_cast<void*>(&_embed_detail::trampoline), 1, arity);
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
    } catch (const CulebraError& e) {
      if (!is_interrupt(e)) throw;
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
