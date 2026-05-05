#pragma once

#ifdef CULEBRA_JIT_ENABLED

#include <num_format.h>
#include <parser.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <well_known_props.h>

// Bring numeric helpers into the JIT file's top-level scope so the
// inline runtime helpers (defined in extern "C" blocks below) can use
// unqualified names.
using culebra::format_float_shortest;

// macOS termios.h defines CR1/CR2/CR3 macros that conflict with LLVM headers
#if defined(__APPLE__)
#undef CR1
#undef CR2
#undef CR3
#endif

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Target/TargetMachine.h"

#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Runtime types and functions callable from JIT'd code
//
// THREAD SAFETY: the Culebra runtime is single-threaded. The globals
// below (exception carriers `culebra_thrown_tag` / `_data` /
// `culebra_is_throw`, the defer stack `_culebra_defer_stack`, the
// shared `InterpGC` used by the tree interpreter) are plain globals,
// NOT `thread_local`. Concurrent use from multiple host threads would
// race on these. Script-level concurrency would require moving this
// state to `thread_local` or per-interpreter-context storage.
// ---------------------------------------------------------------------------

extern "C" {

struct JitValue {
  int8_t tag;
  int64_t data;
};

struct JitArray {
  int64_t refcount;
  size_t size;
  size_t capacity;
  JitValue* items;
};

}  // extern "C"

struct JitClosure;

// Fast-path iterator advance: writes the next value into out-params
// instead of allocating a `{done, value}` step Object. Set on wrapper
// Objects by JIT-controlled iterator factories; `_iter_advance_raw`
// picks it up and skips the user-visible `next()` closure when present.
using JitIterFastFn = void (*)(JitClosure* /*next_cls*/, JitValue /*iter*/,
                               bool* /*done*/, int8_t* /*out_tag*/,
                               int64_t* /*out_data*/);

struct JitObjectEntry {
  JitValue value;
  bool mut;
};

// All refcounted heap types share the same first field: i64 refcount.
// This uniform layout lets the cycle collector read/write refcounts without
// per-type dispatch.

struct JitObject {
  int64_t refcount;
  bool has_drop = false;
  JitIterFastFn fast_next_fn = nullptr;
  std::map<std::string, JitObjectEntry> props;
};

struct JitCell {
  int64_t refcount;
  JitValue value;
};

struct JitClosure {
  int64_t refcount;
  void* fn_ptr;
  size_t n_captures;
  JitCell** captures;
  size_t arity;  // number of user-visible params (excluding __cls__, this)
};

// Uniform calling convention for every JIT closure. Callers build a
// stack slab of user args and pass (cls, this, n_args, args_ptr); the
// callee extracts its declared params from `args` and bundles any
// overflow into the function-scope `__ARGS__` Array.
using JitFn =
    JitValue (*)(JitClosure*, JitValue /*this*/, int64_t /*n_args*/,
                 JitValue* /*args*/);

// Invocation helpers for the common runtime-callback arities. Each
// packs its args into a stack array and hands them to the closure's
// fn_ptr — matching what the JIT-compiled caller emits inline. Caller
// is responsible for retaining each arg before handoff; the callee
// frame takes over ownership on entry.
inline JitValue _culebra_invoke0(JitClosure* fn) {
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 0, nullptr);
}
inline JitValue _culebra_invoke1(JitClosure* fn, JitValue a) {
  JitValue args[1] = {a};
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 1, args);
}
inline JitValue _culebra_invoke2(JitClosure* fn, JitValue a, JitValue b) {
  JitValue args[2] = {a, b};
  return reinterpret_cast<JitFn>(fn->fn_ptr)(fn, {0, 0}, 2, args);
}

// --- Cycle collector ---
//
// Python-style mark-and-sweep: tracks all refcounted heap objects, runs
// periodically and on program exit.

inline void _culebra_value_release_impl(int8_t tag, int64_t data);
inline void _culebra_cell_release(JitCell* c);
inline void _culebra_call_drop_if_present(JitObject* o);

// JitValue::tag values. The refcounted subset (Func/Array/Object)
// doubles as GC tracker tags; cells get a distinct GC-only tag below.
static constexpr int8_t TAG_NIL = 0;
static constexpr int8_t TAG_BOOL = 1;
static constexpr int8_t TAG_LONG = 2;
static constexpr int8_t TAG_FUNC = 3;
static constexpr int8_t TAG_STRING = 4;
static constexpr int8_t TAG_ARRAY = 5;
static constexpr int8_t TAG_OBJECT = 6;
// IEEE 754 binary64. The i64 `data` slot carries the bit pattern of
// the double; consumers bitcast back (see `make_float` /
// `coerce_to_double`). Float is a value type: retain/release is a
// no-op and the cycle collector ignores it via `is_refcounted_value_tag`.
static constexpr int8_t TAG_FLOAT = 7;

static constexpr int8_t GC_TAG_FUNC = TAG_FUNC;
static constexpr int8_t GC_TAG_ARRAY = TAG_ARRAY;
static constexpr int8_t GC_TAG_OBJECT = TAG_OBJECT;
static constexpr int8_t GC_TAG_CELL = 100;

static constexpr size_t GC_THRESHOLD = 10000;
static constexpr int64_t GC_REFCOUNT_BOOST = 1000000;

struct _GcTracker {
  std::unordered_map<void*, int8_t> objects;
  size_t alloc_counter = 0;
  size_t threshold = GC_THRESHOLD;
  bool running = false;      // prevent re-entry

  static _GcTracker& instance() {
    static _GcTracker t;
    return t;
  }

  ~_GcTracker() { collect(); }

  void add(void* ptr, int8_t tag) {
    objects[ptr] = tag;
    if (!running && ++alloc_counter >= threshold) {
      alloc_counter = 0;
      collect();
    }
  }
  void remove(void* ptr) { objects.erase(ptr); }

  static bool is_refcounted_value_tag(int8_t tag) {
    return tag == GC_TAG_FUNC || tag == GC_TAG_ARRAY || tag == GC_TAG_OBJECT;
  }

  static void push_if_refcounted(std::vector<void*>& out, int8_t tag,
                                 int64_t data) {
    if (is_refcounted_value_tag(tag))
      out.push_back(reinterpret_cast<void*>(data));
  }

  // Enumerate child objects (cells/closures/arrays/objects) directly
  // reachable from ptr.
  void enumerate_children(void* ptr, int8_t tag,
                          std::vector<void*>& out) {
    switch (tag) {
      case GC_TAG_FUNC: {
        auto* c = static_cast<JitClosure*>(ptr);
        for (size_t i = 0; i < c->n_captures; i++) {
          if (c->captures[i]) out.push_back(c->captures[i]);
        }
        break;
      }
      case GC_TAG_ARRAY: {
        auto* a = static_cast<JitArray*>(ptr);
        for (size_t i = 0; i < a->size; i++) {
          push_if_refcounted(out, a->items[i].tag, a->items[i].data);
        }
        break;
      }
      case GC_TAG_OBJECT: {
        auto* o = static_cast<JitObject*>(ptr);
        for (auto& [_, entry] : o->props) {
          push_if_refcounted(out, entry.value.tag, entry.value.data);
        }
        break;
      }
      case GC_TAG_CELL: {
        auto* cell = static_cast<JitCell*>(ptr);
        push_if_refcounted(out, cell->value.tag, cell->value.data);
        break;
      }
    }
  }

  // All refcounted types share refcount as their first i64 field.
  static int64_t& refcount_ref(void* ptr) {
    return *reinterpret_cast<int64_t*>(ptr);
  }

  // Clear internal references by releasing each child.
  // This breaks cycles and cascades normal RC freeing.
  void clear_references(void* ptr, int8_t tag) {
    switch (tag) {
      case GC_TAG_FUNC: {
        auto* c = static_cast<JitClosure*>(ptr);
        for (size_t i = 0; i < c->n_captures; i++) {
          if (c->captures[i]) {
            auto* cell = c->captures[i];
            c->captures[i] = nullptr;
            _culebra_cell_release(cell);
          }
        }
        break;
      }
      case GC_TAG_ARRAY: {
        auto* a = static_cast<JitArray*>(ptr);
        auto items = a->items;
        auto size = a->size;
        a->items = nullptr;
        a->size = 0;
        a->capacity = 0;
        for (size_t i = 0; i < size; i++) {
          _culebra_value_release_impl(items[i].tag, items[i].data);
        }
        delete[] items;
        break;
      }
      case GC_TAG_OBJECT: {
        auto* o = static_cast<JitObject*>(ptr);
        // Fire drop before tearing down the prop map. Order among
        // cycle members is undefined; each drop still runs exactly
        // once because the helper short-circuits on missing drop.
        _culebra_call_drop_if_present(o);
        auto props = std::move(o->props);
        o->props.clear();
        for (auto& [_, entry] : props) {
          _culebra_value_release_impl(entry.value.tag, entry.value.data);
        }
        break;
      }
      case GC_TAG_CELL: {
        auto* cell = static_cast<JitCell*>(ptr);
        auto v = cell->value;
        cell->value = {0, 0};
        _culebra_value_release_impl(v.tag, v.data);
        break;
      }
    }
  }

  void collect() {
    if (objects.empty() || running) return;
    running = true;
    _do_collect();
    running = false;
  }

  void _do_collect() {
    if (objects.empty()) return;

    if (std::getenv("CULEBRA_GC_DEBUG")) {
      std::println(stderr, "[GC] scan tracked={}", objects.size());
    }

    // Snapshot tracked objects (collection may modify the map)
    std::vector<std::pair<void*, int8_t>> snapshot(objects.begin(),
                                                   objects.end());

    // gc_refs = refcount initially
    std::unordered_map<void*, int64_t> gc_refs;
    gc_refs.reserve(snapshot.size());
    for (auto& [p, t] : snapshot) gc_refs[p] = refcount_ref(p);

    // Subtract internal references
    std::vector<void*> children;
    for (auto& [p, t] : snapshot) {
      children.clear();
      enumerate_children(p, t, children);
      for (auto* c : children) {
        auto it = gc_refs.find(c);
        if (it != gc_refs.end()) --it->second;
      }
    }

    // Mark external roots and propagate reachability
    std::unordered_set<void*> reachable;
    std::queue<void*> q;
    for (auto& [p, r] : gc_refs) {
      if (r > 0) {
        reachable.insert(p);
        q.push(p);
      }
    }
    while (!q.empty()) {
      auto* p = q.front();
      q.pop();
      auto it = objects.find(p);
      if (it == objects.end()) continue;
      children.clear();
      enumerate_children(p, it->second, children);
      for (auto* c : children) {
        auto [_, inserted] = reachable.insert(c);
        if (inserted && gc_refs.contains(c)) q.push(c);
      }
    }

    // Collect garbage (unreachable)
    std::vector<std::pair<void*, int8_t>> garbage;
    for (auto& [p, t] : snapshot) {
      if (!reachable.contains(p)) garbage.push_back({p, t});
    }

    if (garbage.empty()) return;

    if (std::getenv("CULEBRA_GC_DEBUG")) {
      std::println(stderr, "[GC] collecting {} cycle objects (tracked={})",
                   garbage.size(), snapshot.size());
    }

    // Boost refcounts to prevent premature destruction during clearing
    for (auto& [p, t] : garbage) refcount_ref(p) += GC_REFCOUNT_BOOST;

    // Clear references (breaks cycles)
    for (auto& [p, t] : garbage) clear_references(p, t);

    // Set refcount to 1 and release: release brings it to 0 and triggers
    // normal destruction, which safely iterates the (now empty) children.
    for (auto& [p, t] : garbage) {
      refcount_ref(p) = 1;
      if (t == GC_TAG_CELL) {
        _culebra_cell_release(static_cast<JitCell*>(p));
      } else {
        _culebra_value_release_impl(t, reinterpret_cast<int64_t>(p));
      }
    }
  }
};

inline _GcTracker& _gc() { return _GcTracker::instance(); }

// Cycle detection during string conversion.
inline thread_local std::unordered_set<const void*> _jit_str_visiting;

// RAII: inserts on construction, erases on destruction. `already` is true if
// the pointer was present (i.e., we're inside a cycle).
struct _JitStrGuard {
  const void* key;
  bool already;
  explicit _JitStrGuard(const void* k) : key(k) {
    already = !_jit_str_visiting.insert(k).second;
  }
  ~_JitStrGuard() {
    if (!already) _jit_str_visiting.erase(key);
  }
};

// Internal helper (C++ - not extern C, but inline to satisfy ODR)
inline std::string _culebra_value_to_str_impl(int8_t type, int64_t data) {
  switch (type) {
    case TAG_NIL:
      return "nil";
    case TAG_BOOL:
      return data ? "true" : "false";
    case TAG_LONG:
      return std::to_string(data);
    case TAG_FUNC:
      return "[function]";
    case TAG_STRING: {
      std::string s = "'";
      s += reinterpret_cast<const char*>(data);
      s += "'";
      return s;
    }
    case TAG_FLOAT: {
      double d;
      std::memcpy(&d, &data, sizeof(d));
      return format_float_shortest(d);
    }
    case TAG_ARRAY: {
      auto* arr = reinterpret_cast<JitArray*>(data);
      _JitStrGuard guard(arr);
      if (guard.already) return "[...]";
      std::string s = "[";
      for (size_t i = 0; i < arr->size; i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(arr->items[i].tag,
                                        arr->items[i].data);
      }
      s += "]";
      return s;
    }
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(data);
      _JitStrGuard guard(obj);
      if (guard.already) return "{...}";
      std::string s = "{";
      bool first = true;
      for (const auto& [name, entry] : obj->props) {
        if (!first) s += ", ";
        first = false;
        if (entry.mut) s += "mut ";
        s += name;
        s += ": ";
        s += _culebra_value_to_str_impl(entry.value.tag, entry.value.data);
      }
      s += "}";
      return s;
    }
    default:
      return "[unknown]";
  }
}

inline const char* _culebra_heap_str(std::string_view s) {
  auto buf = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  return buf;
}

// Bitcast the i64 payload of a Float JitValue back to double.
inline double _culebra_float_to_double(int64_t data) {
  double d;
  std::memcpy(&d, &data, sizeof(d));
  return d;
}

inline int64_t _culebra_double_to_bits(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i;
}

// Coerce a numeric JitValue (Long or Float) to double. Any other tag
// throws. Used by the arithmetic/comparison slow paths.
inline double _culebra_coerce_num(int8_t tag, int64_t data) {
  if (tag == TAG_LONG) return static_cast<double>(data);
  if (tag == TAG_FLOAT) return _culebra_float_to_double(data);
  throw std::runtime_error("type error.");
}

// Same-tag equality matching the interpreter's operator==. Strings compare by
// contents; reference types (func/array/object) by identity. Long↔Float
// cross-type uses numeric equality (`1 == 1.0`).
inline bool _culebra_value_equal(int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  if (t1 != t2) {
    // Numeric cross-type: promote to double and compare by value.
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return _culebra_coerce_num(t1, d1) == _culebra_coerce_num(t2, d2);
    }
    return false;
  }
  switch (t1) {
    case TAG_NIL: return true;
    case TAG_BOOL: return (d1 != 0) == (d2 != 0);
    case TAG_LONG: return d1 == d2;
    case TAG_FLOAT:
      return _culebra_float_to_double(d1) == _culebra_float_to_double(d2);
    case TAG_STRING:
      return std::strcmp(reinterpret_cast<const char*>(d1),
                         reinterpret_cast<const char*>(d2)) == 0;
    default: return d1 == d2;  // func/array/object: identity
  }
}

// Ordering helpers. Each one exactly mirrors the corresponding
// interpreter `Value::operator<` / `<=` / `>` / `>=` so JIT
// semantics match bit-for-bit. Nil is a special case: `nil op nil`
// always returns false (ordering on `nil` is not defined). Cross-type
// numeric (Long↔Float) promotes to double.

template <typename Cmp>
inline bool _culebra_value_ord(int8_t t1, int64_t d1, int8_t t2, int64_t d2,
                               Cmp cmp) {
  if (t1 != t2) {
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return cmp(_culebra_coerce_num(t1, d1), _culebra_coerce_num(t2, d2));
    }
    throw std::runtime_error("type error.");
  }
  switch (t1) {
    case TAG_NIL: return false;
    case TAG_BOOL: return cmp(double(d1 != 0), double(d2 != 0));
    case TAG_LONG: return cmp(double(d1), double(d2));
    case TAG_FLOAT:
      return cmp(_culebra_float_to_double(d1), _culebra_float_to_double(d2));
    case TAG_STRING: {
      auto c = std::strcmp(reinterpret_cast<const char*>(d1),
                           reinterpret_cast<const char*>(d2));
      return cmp(double(c), 0.0);
    }
    default: throw std::runtime_error("type error.");
  }
}

// Four ordering predicates share `_culebra_value_ord`'s Nil/cross-type
// scaffolding; the public extern "C" trampolines below are the only
// callers. A single `cmp` comparator parameterises the leaf compare.

extern "C" {

__attribute__((used)) inline void culebra_runtime_puts(int8_t type,
                                                       int64_t data) {
  switch (type) {
    case TAG_NIL:
      std::cout << "nil" << std::endl;
      break;
    case TAG_BOOL:
      std::cout << (data ? "true" : "false") << std::endl;
      break;
    case TAG_LONG:
      std::cout << data << std::endl;
      break;
    case TAG_STRING:
      std::cout << "'" << reinterpret_cast<const char*>(data) << "'"
                << std::endl;
      break;
    case TAG_ARRAY:
    case TAG_OBJECT:
    case TAG_FLOAT:
      std::cout << _culebra_value_to_str_impl(type, data) << std::endl;
      break;
    default:
      std::cout << "[unknown]" << std::endl;
      break;
  }
}

// For interpolation: strings unquoted, everything else via str()
__attribute__((used)) inline const char* culebra_runtime_value_to_display(
    int8_t type, int64_t data) {
  if (type == 4) return reinterpret_cast<const char*>(data);
  return _culebra_heap_str(_culebra_value_to_str_impl(type, data));
}

__attribute__((used)) inline const char* culebra_runtime_str_concat(
    const char* a, const char* b) {
  auto la = std::strlen(a);
  auto lb = std::strlen(b);
  auto r = static_cast<char*>(std::malloc(la + lb + 1));
  std::memcpy(r, a, la);
  std::memcpy(r + la, b, lb);
  r[la + lb] = '\0';
  return r;
}

__attribute__((used)) inline bool culebra_runtime_str_eq(const char* a,
                                                         const char* b) {
  return std::strcmp(a, b) == 0;
}

__attribute__((used)) inline int32_t culebra_runtime_str_cmp(const char* a,
                                                             const char* b) {
  return std::strcmp(a, b);
}

__attribute__((used)) inline void culebra_runtime_assert(int8_t type,
                                                         int64_t data,
                                                         int64_t line,
                                                         int64_t col) {
  bool truthy = false;
  switch (type) {
    case TAG_BOOL:
    case TAG_LONG:
      truthy = (data != 0);
      break;
    case TAG_FLOAT: {
      // Python-style truthiness: NaN is true; only ±0.0 is false.
      double d;
      std::memcpy(&d, &data, sizeof(d));
      truthy = (d != 0.0) || std::isnan(d);
      break;
    }
  }
  if (!truthy) {
    throw std::runtime_error(
        std::format("assert failed at {}:{}.", line, col));
  }
}

__attribute__((used)) inline void culebra_runtime_type_error(int64_t line,
                                                             int64_t col) {
  throw std::runtime_error(std::format("type error at {}:{}.", line, col));
}

__attribute__((used)) inline void culebra_runtime_arity_error(
    int64_t got, int64_t declared, int64_t line, int64_t col) {
  throw std::runtime_error(std::format(
      "arguments error: called with {} argument(s), expected at least {} "
      "at {}:{}.",
      got, declared, line, col));
}

// C++ exception thrown by `culebra_runtime_throw` when user code runs
// `throw expr`. The payload (tag + data) is NOT read via this object
// in JITed code — that would require exposing the typeinfo symbol
// (`_ZTI16CulebraException`) to the JIT linker, which is fragile
// across platforms. Instead, landingpads use catch-all and read the
// `culebra_thrown_*` globals below. The member fields are only read
// by the host-side catch in JIT::run to format the uncaught-throw
// message.
class CulebraException : public std::exception {
 public:
  int8_t tag;
  int64_t data;
  CulebraException(int8_t t, int64_t d) : tag(t), data(d) {}
  const char* what() const noexcept override { return "CulebraException"; }
};

__attribute__((used)) void culebra_runtime_value_retain(int8_t tag,
                                                        int64_t data);

// Culebra is single-threaded, so plain globals are fine. Populated
// by `culebra_runtime_throw` and read by try/catch landingpads to
// distinguish user throws (`culebra_is_throw == 1`) from internal
// runtime errors (`std::runtime_error` etc., which re-raise).
__attribute__((used)) inline int8_t culebra_thrown_tag = 0;
__attribute__((used)) inline int64_t culebra_thrown_data = 0;
__attribute__((used)) inline int8_t culebra_is_throw = 0;

__attribute__((used)) inline void culebra_runtime_throw(int8_t tag,
                                                        int64_t data) {
  // Retain so the payload stays alive through stack unwinding. The
  // matching catch handler is responsible for the balancing release.
  culebra_runtime_value_retain(tag, data);
  culebra_thrown_tag = tag;
  culebra_thrown_data = data;
  culebra_is_throw = 1;
  throw CulebraException(tag, data);
}

// Re-throw the currently-in-flight exception. Used by cleanup
// landingpads to let the exception keep unwinding after running
// deferred work, while still allowing the JIT to choose the next
// landingpad via an `invoke` unwind edge (LLVM's `resume` alone
// abandons the current function, skipping outer landingpads in the
// same function).
__attribute__((used)) inline void culebra_runtime_rethrow() {
  throw;
}

// --- Defer stack (JIT side) ---
//
// Culebra is single-threaded, so a single global LIFO stack of retained
// closure Values is sufficient. Each lexical scope records a "mark"
// (the stack size) on entry and, on every exit path (fall-through,
// return, throw), unwinds the stack back to that mark — running each
// popped closure as a 0-arg call.
inline std::vector<JitValue>& _culebra_defer_stack() {
  static std::vector<JitValue> s;
  return s;
}

__attribute__((used)) inline int64_t culebra_runtime_defer_mark() {
  return static_cast<int64_t>(_culebra_defer_stack().size());
}

__attribute__((used)) inline void culebra_runtime_defer_push(int8_t tag,
                                                             int64_t data) {
  // Retain: stack owns a reference until run_to drops it.
  culebra_runtime_value_retain(tag, data);
  _culebra_defer_stack().push_back(JitValue{tag, data});
}

__attribute__((used)) inline void culebra_runtime_defer_run_to(int64_t mark) {
  auto& s = _culebra_defer_stack();
  while (static_cast<int64_t>(s.size()) > mark) {
    auto v = s.back();
    s.pop_back();
    auto* c = reinterpret_cast<JitClosure*>(v.data);
    try {
      auto r = _culebra_invoke0(c);
      _culebra_value_release_impl(r.tag, r.data);
    } catch (...) {
      _culebra_value_release_impl(v.tag, v.data);
      // Drop remaining defers for this scope so they don't leak.
      while (static_cast<int64_t>(s.size()) > mark) {
        auto rem = s.back();
        s.pop_back();
        _culebra_value_release_impl(rem.tag, rem.data);
      }
      throw;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

inline const char* _culebra_tag_name(int8_t tag) {
  switch (tag) {
    case TAG_NIL:    return "Nil";
    case TAG_BOOL:   return "Bool";
    case TAG_LONG:   return "Long";
    case TAG_FUNC:   return "Function";
    case TAG_STRING: return "String";
    case TAG_ARRAY:  return "Array";
    case TAG_OBJECT: return "Object";
    case TAG_FLOAT:  return "Float";
  }
  return "Unknown";
}

// Check that a Value's tag matches a named type ("Any" matches everything).
__attribute__((used)) inline void culebra_runtime_type_check(
    int8_t tag, const char* expected, const char* context, int64_t line,
    int64_t col) {
  if (expected == nullptr || expected[0] == '\0') return;
  if (std::strcmp(expected, "Any") == 0) return;
  auto* actual = _culebra_tag_name(tag);
  if (std::strcmp(actual, expected) != 0) {
    throw std::runtime_error(std::format(
        "type error: {} expects {} at {}:{}.", context, expected, line, col));
  }
}

__attribute__((used)) inline void culebra_runtime_div_zero(int64_t line,
                                                           int64_t col) {
  throw std::runtime_error(
      std::format("divide by 0 error at {}:{}.", line, col));
}

// --- Numeric runtime helpers (Float-aware arithmetic slow paths) ---
//
// Mirror eval_bin_op_step / eval_power in interpreter.h. The JIT
// emits an inline "both Long" fast path and only calls these when at
// least one operand is Float or the types are mixed.

#define CUL_NUM_BINOP(name, expr)                                       \
  __attribute__((used)) inline JitValue culebra_runtime_num_##name(     \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd) {                   \
    auto a = _culebra_coerce_num(lt, ld);                               \
    auto b = _culebra_coerce_num(rt, rd);                               \
    return {TAG_FLOAT, _culebra_double_to_bits(expr)};                  \
  }
CUL_NUM_BINOP(add, a + b)
CUL_NUM_BINOP(sub, a - b)
CUL_NUM_BINOP(mul, a * b)
#undef CUL_NUM_BINOP

__attribute__((used)) inline JitValue culebra_runtime_num_div(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw std::runtime_error("divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(a / b)};
}

__attribute__((used)) inline JitValue culebra_runtime_num_mod(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw std::runtime_error("divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(std::fmod(a, b))};
}

// extern "C" entry points for the comparison helpers. One per
// operator so the JIT can look them up by name.
__attribute__((used)) inline bool culebra_runtime_value_equal(
    int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  return _culebra_value_equal(t1, d1, t2, d2);
}

#define CUL_VALUE_ORD(name, expr)                                       \
  __attribute__((used)) inline bool culebra_runtime_value_##name(       \
      int8_t t1, int64_t d1, int8_t t2, int64_t d2) {                   \
    return _culebra_value_ord(                                          \
        t1, d1, t2, d2, [](double a, double b) { return expr; });       \
  }
CUL_VALUE_ORD(less,    a <  b)
CUL_VALUE_ORD(leq,     a <= b)
CUL_VALUE_ORD(greater, a >  b)
CUL_VALUE_ORD(geq,     a >= b)
#undef CUL_VALUE_ORD

// Power with full Python-style semantics:
//   Long ** non-negative Long  → Long (exp-by-squaring, wraps)
//   Long ** negative Long      → Float (promotes via std::pow)
//   any Float                  → Float
__attribute__((used)) inline JitValue culebra_runtime_num_pow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  if (lt == TAG_LONG && rt == TAG_LONG) {
    int64_t a = ld, e = rd;
    if (e >= 0) {
      int64_t result = 1, acc = a;
      while (e > 0) {
        if (e & 1) result *= acc;
        e >>= 1;
        if (e > 0) acc *= acc;
      }
      return {TAG_LONG, result};
    }
    if (a == 0) throw std::runtime_error("divide by 0 error");
    return {TAG_FLOAT,
            _culebra_double_to_bits(std::pow(static_cast<double>(a),
                                             static_cast<double>(e)))};
  }
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  return {TAG_FLOAT, _culebra_double_to_bits(std::pow(a, b))};
}

// Unary negation: Long → Long (wraps), Float → Float. Non-numeric
// raises type error. Called only from the unary-minus slow path.
__attribute__((used)) inline JitValue culebra_runtime_num_neg(
    int8_t t, int64_t d) {
  if (t == TAG_LONG) return {TAG_LONG, -d};
  if (t == TAG_FLOAT) {
    auto v = _culebra_float_to_double(d);
    return {TAG_FLOAT, _culebra_double_to_bits(-v)};
  }
  throw std::runtime_error("type error.");
}

__attribute__((used)) inline void culebra_runtime_debugger_break(
    const char* path, int64_t line, int64_t col) {
  std::println(stderr, "\nBreak in {}:{}:{}", path, line, col);
  // Show a few lines of source around the break point, if we can open it
  std::ifstream ifs(path);
  if (ifs) {
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::string cur;
    for (auto c : src) {
      if (c == '\n') { lines.push_back(cur); cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);

    auto start = line > 2 ? static_cast<size_t>(line) - 3 : 0;
    auto end = std::min(lines.size(),
                        static_cast<size_t>(line) + 2);
    for (auto i = start; i < end; i++) {
      auto marker = (i + 1 == static_cast<size_t>(line)) ? ">" : " ";
      std::println(stderr, "{} {:>4} {}", marker, i + 1, lines[i]);
    }
  }
  std::print(stderr, "\ndebug> ");
  std::string s;
  std::getline(std::cin, s);
  // Any input continues execution (minimal: no commands yet)
}

// --- Array runtime ---

__attribute__((used)) inline JitArray* culebra_runtime_array_new() {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = 0;
  arr->items = nullptr;
  _gc().add(arr, GC_TAG_ARRAY);
  return arr;
}

__attribute__((used)) inline void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data);

}  // extern "C"

// Forward declaration for runtime helpers that need refcount logic
inline void _culebra_value_release_impl(int8_t tag, int64_t data);

extern "C" {

__attribute__((used)) inline void culebra_runtime_array_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data) {
  if (arr->size >= arr->capacity) {
    size_t new_cap = arr->capacity * 2;
    if (new_cap < 8) new_cap = 8;
    auto* new_items = new JitValue[new_cap];
    if (arr->items) {
      std::memcpy(new_items, arr->items, arr->size * sizeof(JitValue));
      delete[] arr->items;
    }
    arr->items = new_items;
    arr->capacity = new_cap;
  }
  // Array absorbs ownership of the pushed value (+1).
  arr->items[arr->size].tag = tag;
  arr->items[arr->size].data = data;
  arr->size++;
}

__attribute__((used)) inline void culebra_runtime_array_resize(
    JitArray* arr, int64_t count, int8_t def_tag, int64_t def_data) {
  while (arr->size < static_cast<size_t>(count)) {
    culebra_runtime_array_push(arr, def_tag, def_data);
  }
  if (static_cast<size_t>(count) < arr->size) {
    arr->size = static_cast<size_t>(count);
  }
}

__attribute__((used)) inline void culebra_runtime_array_get(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t* out_tag,
                                                            int64_t* out_data,
                                                            int64_t line,
                                                            int64_t col) {
  if (idx < 0) idx = static_cast<int64_t>(arr->size) + idx;
  if (idx < 0 || static_cast<size_t>(idx) >= arr->size) {
    throw std::runtime_error(
        std::format("index out of range at {}:{}.", line, col));
  }
  *out_tag = arr->items[idx].tag;
  *out_data = arr->items[idx].data;
}

__attribute__((used)) inline void culebra_runtime_array_set(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t tag,
                                                            int64_t data,
                                                            int64_t line,
                                                            int64_t col) {
  if (idx < 0 || static_cast<size_t>(idx) >= arr->size) {
    throw std::runtime_error(
        std::format("index out of range at {}:{}.", line, col));
  }
  _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
  arr->items[idx].tag = tag;
  arr->items[idx].data = data;
}

__attribute__((used)) inline int64_t culebra_runtime_array_size(JitArray* arr) {
  return static_cast<int64_t>(arr->size);
}

// Forward decl (defined later alongside the refcount runtime).
__attribute__((used)) inline void culebra_runtime_value_retain(int8_t tag,
                                                               int64_t data);

// Create a new array by copying [start, start+len) from src. Each copied
// element is retained (new array holds another reference).
__attribute__((used)) inline JitArray* culebra_runtime_array_slice(
    JitArray* src, int64_t start, int64_t len) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = 0; i < len; i++) {
    auto& e = src->items[start + i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(r, e.tag, e.data);
  }
  return r;
}

__attribute__((used)) inline void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data) {
  if (static_cast<size_t>(idx) < arr->size) {
    _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
    arr->items[idx].tag = tag;
    arr->items[idx].data = data;
  } else {
    culebra_runtime_array_push(arr, tag, data);
  }
}

// --- Object runtime ---

__attribute__((used)) inline JitObject* culebra_runtime_object_new() {
  auto* o = new JitObject();
  o->refcount = 1;
  _gc().add(o, GC_TAG_OBJECT);
  return o;
}

// Build an Array from args[start..n) for binding to `__ARGS__`. Caller
// transferred +1 ownership of each arg via the stack-allocated slab;
// Array takes over (object_set-style: no extra retain, slot owns +1).
// Returns a fresh JitArray with refcount 1.
__attribute__((used)) inline JitArray* culebra_runtime_args_slice_to_array(
    const JitValue* args, int64_t start, int64_t n) {
  auto* a = culebra_runtime_array_new();
  for (int64_t i = start; i < n; i++) {
    culebra_runtime_array_push(a, args[i].tag, args[i].data);
  }
  return a;
}

// Validate the well-known-property contract (see well_known_props.h)
// for a freshly-bound JIT value: must be a 0-arg Function. The arg's
// +1 is released before throwing — codegen passes ownership and
// expects either store-into-map or release.
inline void _culebra_check_well_known_prop(std::string_view name,
                                           int8_t tag, int64_t data) {
  if (!culebra::is_well_known_prop(name)) return;
  auto bad = [&]() {
    _culebra_value_release_impl(tag, data);
    culebra::throw_well_known_prop_contract_error(name);
  };
  if (tag != GC_TAG_FUNC) bad();
  auto* cls = reinterpret_cast<JitClosure*>(data);
  if (!cls || cls->arity != 0) bad();
}

// Standalone version of the well-known check for codegen to call only
// when the property name is statically "drop" / "iter" / "next".
// Frees the regular object_set hot path from a name comparison on
// every property bind.
__attribute__((used)) inline void culebra_runtime_check_well_known_prop(
    const char* key, int8_t tag, int64_t data) {
  _culebra_check_well_known_prop(key, tag, data);
}

__attribute__((used)) inline void culebra_runtime_object_set(
    JitObject* obj, const char* key, bool mut, int8_t tag, int64_t data,
    int64_t line, int64_t col) {
  auto it = obj->props.find(key);
  if (it == obj->props.end()) {
    obj->props.emplace(std::string(key),
                       JitObjectEntry{JitValue{tag, data}, mut});
  } else {
    if (!it->second.mut) {
      _culebra_value_release_impl(tag, data);
      throw std::runtime_error(std::format(
          "immutable property '{}' at {}:{}.", key, line, col));
    }
    _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
    it->second.value.tag = tag;
    it->second.value.data = data;
  }
  if (std::string_view(key) == "drop") obj->has_drop = true;
}

__attribute__((used)) inline void culebra_runtime_object_get(
    JitObject* obj, const char* key, int8_t* out_tag, int64_t* out_data) {
  auto it = obj->props.find(key);
  if (it == obj->props.end()) {
    *out_tag = 0;
    *out_data = 0;
    return;
  }
  *out_tag = it->second.value.tag;
  *out_data = it->second.value.data;
}

__attribute__((used)) inline bool culebra_runtime_object_has(JitObject* obj,
                                                             const char* key) {
  return obj->props.find(key) != obj->props.end();
}

__attribute__((used)) inline int64_t culebra_runtime_object_size(
    JitObject* obj) {
  return static_cast<int64_t>(obj->props.size());
}

// --- Cell runtime ---

__attribute__((used)) inline JitCell* culebra_runtime_cell_new(int8_t tag,
                                                               int64_t data) {
  auto* c = new JitCell();
  c->refcount = 1;
  c->value.tag = tag;
  c->value.data = data;
  _gc().add(c, GC_TAG_CELL);
  return c;
}

// --- Closure runtime ---

__attribute__((used)) inline JitClosure* culebra_runtime_closure_new(
    void* fn_ptr, size_t n_captures, size_t arity) {
  auto* c = new JitClosure();
  c->refcount = 1;
  c->fn_ptr = fn_ptr;
  c->n_captures = n_captures;
  c->captures =
      n_captures ? static_cast<JitCell**>(
                       std::malloc(sizeof(JitCell*) * n_captures))
                 : nullptr;
  c->arity = arity;
  _gc().add(c, GC_TAG_FUNC);
  return c;
}

// --- Iterator protocol runtime --------------------------------------------
//
// The iterator protocol uses Objects with `iter` (returns self-like
// view) and `next` (yields a {done, value} step Object) 0-arg
// closures. The JIT drives for-in and iterator methods through
// runtime C++ helpers so each call site emits a single factory /
// terminal call rather than a full closure body. State is captured
// via JitCells attached to the wrapper's `next` closure.
//
// Conventions:
//   - `_iter_advance` transfers +1 to `this` for the next call.
//   - `_iter_step_value` returns +1; caller owns.
//   - Terminal methods consume the iterator (don't release it; caller
//     holds the +1 and will release after the terminal returns).
//   - Lazy wrapper factories transfer upstream's +1 into a capture
//     cell so the wrapper owns it for its lifetime.

// Forward declared — defined later alongside the array higher-order
// runtime helpers.
inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            int64_t line, int64_t col);
extern "C" __attribute__((used)) inline JitArray*
culebra_runtime_object_keys(JitObject* obj);

// Generic "iter returns self" — used by every wrapper we build.
inline JitValue _iter_self_iter_fn(JitClosure*, JitValue this_val,
                                   int64_t, JitValue*) {
  culebra_runtime_value_retain(this_val.tag, this_val.data);
  return this_val;
}

// Look up an iterator's `next` closure pointer. Used once at entry to
// a walk so subsequent advances skip the std::map::find per step.
// Returns nullptr if the receiver isn't a proper iterator (tag must be
// Object, must carry a Function-typed `next`).
inline JitClosure* _iter_next_closure(JitValue iter_val) {
  if (iter_val.tag != TAG_OBJECT) return nullptr;
  auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
  auto it = iter_obj->props.find("next");
  if (it == iter_obj->props.end()) return nullptr;
  if (it->second.value.tag != TAG_FUNC) return nullptr;
  return reinterpret_cast<JitClosure*>(it->second.value.data);
}

// Advance with an on-the-fly next-closure lookup — one std::map::find
// per step. Used by call sites that don't hold on to the iterator
// long enough to bother caching.
inline JitValue _iter_advance(JitValue iter_val) {
  auto* next_cls = _iter_next_closure(iter_val);
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  return reinterpret_cast<JitFn>(next_cls->fn_ptr)(
      next_cls, iter_val, 0, nullptr);
}

// Advance using a pre-cached `next` closure (fast path). Extern "C"
// forbids C++ overloading on signature, so the cached variant gets a
// distinct name.
inline JitValue _iter_advance_fast(JitClosure* next_cls,
                                   JitValue iter_val) {
  culebra_runtime_value_retain(iter_val.tag, iter_val.data);
  return reinterpret_cast<JitFn>(next_cls->fn_ptr)(
      next_cls, iter_val, 0, nullptr);
}

inline bool _iter_step_done(JitValue step) {
  auto* step_obj = reinterpret_cast<JitObject*>(step.data);
  auto it = step_obj->props.find("done");
  if (it == step_obj->props.end()) return false;
  auto v = it->second.value;
  if (v.tag == TAG_NIL) return false;
  if (v.tag == TAG_BOOL || v.tag == TAG_LONG) return v.data != 0;
  return true;
}

// Returns step.value with +1 retained (caller owns). Nil if missing.
inline JitValue _iter_step_value(JitValue step) {
  auto* step_obj = reinterpret_cast<JitObject*>(step.data);
  auto it = step_obj->props.find("value");
  if (it == step_obj->props.end()) return {0, 0};
  auto v = it->second.value;
  culebra_runtime_value_retain(v.tag, v.data);
  return v;
}

// Build a {done: true} step Object.
inline JitObject* _iter_done_step() {
  auto* step = culebra_runtime_object_new();
  culebra_runtime_object_set(step, "done", /*mut*/ false, TAG_BOOL, 1, 0,
                             0);
  return step;
}

// Build a {done: false, value: v} step. Transfers v's +1 into the step.
inline JitObject* _iter_value_step(JitValue v) {
  auto* step = culebra_runtime_object_new();
  culebra_runtime_object_set(step, "done", /*mut*/ false, TAG_BOOL, 0, 0,
                             0);
  culebra_runtime_object_set(step, "value", /*mut*/ false, v.tag, v.data, 0,
                             0);
  return step;
}

// Pull the next value via out-params; skips the `{done, value}` step
// Object allocation when the iterator carries a `fast_next_fn`. Returns
// true on a value step with out_tag/out_data holding a +1 Value, false
// on done. Falls back to the user-visible `next` closure otherwise.
inline bool _iter_advance_raw(JitClosure* next_cls, JitValue iter_val,
                              int8_t* out_tag, int64_t* out_data) {
  if (iter_val.tag == TAG_OBJECT) {
    auto* iter_obj = reinterpret_cast<JitObject*>(iter_val.data);
    if (iter_obj->fast_next_fn) {
      bool done;
      // fast_next_fn reads state from cls->captures; iter_val is passed
      // unretained since the fn ignores it.
      iter_obj->fast_next_fn(next_cls, iter_val, &done, out_tag, out_data);
      return !done;
    }
  }
  auto step = _iter_advance_fast(next_cls, iter_val);
  if (_iter_step_done(step)) {
    _culebra_value_release_impl(step.tag, step.data);
    return false;
  }
  auto v = _iter_step_value(step);
  *out_tag = v.tag;
  *out_data = v.data;
  _culebra_value_release_impl(step.tag, step.data);
  return true;
}

// Pull the next value from an iterator using a pre-cached `next`
// closure. Returns true with `out_v` holding a +1-retained Value on
// success; false on done. Uses the raw fast path when available.
inline bool _iter_pull(JitClosure* next_cls, JitValue iter_val,
                       JitValue& out_v) {
  return _iter_advance_raw(next_cls, iter_val, &out_v.tag, &out_v.data);
}

// Build a wrapper iterator Object with `iter: self, next: <next_fn>`
// where `next_fn` captures the given cells. Transfers each cell's +1
// into the captures slab; caller does NOT retain afterwards.
inline JitObject* _iter_wrap_new(
    JitValue (*next_fn)(JitClosure*, JitValue, int64_t, JitValue*),
    std::initializer_list<JitCell*> captures) {
  auto* iter_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(&_iter_self_iter_fn), 0, 0);
  auto* next_cls = culebra_runtime_closure_new(
      reinterpret_cast<void*>(next_fn), captures.size(), 0);
  size_t i = 0;
  for (auto* c : captures) next_cls->captures[i++] = c;
  auto* obj = culebra_runtime_object_new();
  culebra_runtime_object_set(obj, "iter", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(iter_cls), 0, 0);
  culebra_runtime_object_set(obj, "next", /*mut*/ false, GC_TAG_FUNC,
                             reinterpret_cast<int64_t>(next_cls), 0, 0);
  return obj;
}

}  // extern "C" (close briefly for the iterator-wrapper templates)

// Trampoline for `user-level iter.next()` that delegates to a fast_fn
// and wraps the result in a fresh `{done, value}` step Object. Reached
// only from user code calling `.next()` directly — in-JIT for-in and
// the terminal/lazy methods go through `_iter_advance_raw` and skip
// this step-alloc entirely.
template <JitIterFastFn FastFn>
inline JitValue _iter_trampoline_next_fn(JitClosure* cls, JitValue iv,
                                         int64_t, JitValue*) {
  bool done;
  int8_t tag;
  int64_t data;
  FastFn(cls, iv, &done, &tag, &data);
  if (done) return {TAG_OBJECT, reinterpret_cast<int64_t>(_iter_done_step())};
  return {TAG_OBJECT,
          reinterpret_cast<int64_t>(_iter_value_step({tag, data}))};
}

// Build a wrapper Object whose user-visible `next` trampolines to
// `FastFn` and whose `fast_next_fn` slot exposes `FastFn` directly for
// the in-JIT hot path.
template <JitIterFastFn FastFn>
inline JitObject* _iter_wrap_fast(std::initializer_list<JitCell*> captures) {
  auto* obj = _iter_wrap_new(&_iter_trampoline_next_fn<FastFn>, captures);
  obj->fast_next_fn = FastFn;
  return obj;
}

extern "C" {

// --- Terminal iterator methods --------------------------------------------

__attribute__((used)) inline JitArray* culebra_runtime_iter_collect(
    int8_t it, int64_t id) {
  auto* out = culebra_runtime_array_new();
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    culebra_runtime_array_push(out, v.tag, v.data);  // consumes +1
  }
  return out;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_count(
    int8_t it, int64_t id) {
  int64_t n = 0;
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    _culebra_value_release_impl(v.tag, v.data);
    n++;
  }
  return n;
}

__attribute__((used)) inline void culebra_runtime_iter_for_each(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "for_each", line, col);
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

__attribute__((used)) inline void culebra_runtime_iter_reduce(
    int8_t it, int64_t id, int8_t init_tag, int64_t init_data,
    int8_t ft, int64_t fd, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  auto* fn = _culebra_expect_callback(ft, fd, 2, "reduce", line, col);
  JitValue acc = {init_tag, init_data};
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    acc = _culebra_invoke2(fn, acc, v);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

__attribute__((used)) inline void culebra_runtime_iter_find(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "find", line, col);
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    culebra_runtime_value_retain(v.tag, v.data);  // an extra +1 for the call
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      *out_tag = v.tag;
      *out_data = v.data;  // out-param takes v's original +1
      return;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
  *out_tag = 0;
  *out_data = 0;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_any(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "any", line, col);
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_all(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(ft, fd, 1, "all", line, col);
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    auto r = _culebra_invoke1(fn, v);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

// Sum/product/min/max share the same iterate-and-accumulate shape.
// Non-Long elements raise `type error at L:C.`, matching the interp's
// `Value::to_long`. `min` / `max` on an empty input also throw, since
// there's no natural identity for them.

__attribute__((used)) inline int64_t culebra_runtime_iter_sum(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  int64_t acc = 0;
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    acc += v.data;
  }
  return acc;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_product(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  int64_t acc = 1;
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  while (_iter_pull(next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    acc *= v.data;
  }
  return acc;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_min(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(next_cls, {it, id}, v)) {
    throw std::runtime_error(std::format(
        "type error: min of empty Iterator at {}:{}.", line, col));
  }
  if (v.tag != TAG_LONG) {
    _culebra_value_release_impl(v.tag, v.data);
    throw std::runtime_error(
        std::format("type error at {}:{}.", line, col));
  }
  int64_t best = v.data;
  while (_iter_pull(next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    if (v.data < best) best = v.data;
  }
  return best;
}

__attribute__((used)) inline int64_t culebra_runtime_iter_max(
    int8_t it, int64_t id, int64_t line, int64_t col) {
  JitValue v;
  auto* next_cls = _iter_next_closure({it, id});
  if (!_iter_pull(next_cls, {it, id}, v)) {
    throw std::runtime_error(std::format(
        "type error: max of empty Iterator at {}:{}.", line, col));
  }
  if (v.tag != TAG_LONG) {
    _culebra_value_release_impl(v.tag, v.data);
    throw std::runtime_error(
        std::format("type error at {}:{}.", line, col));
  }
  int64_t best = v.data;
  while (_iter_pull(next_cls, {it, id}, v)) {
    if (v.tag != TAG_LONG) {
      _culebra_value_release_impl(v.tag, v.data);
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    if (v.data > best) best = v.data;
  }
  return best;
}

// --- Lazy iterator wrappers: each factory returns a new iterator Object.
// Captures are stored in cells attached to the wrapper's `next` closure.
// Cells transfer their +1 to the closure; callers should not retain
// after handing a value to a factory.

// map: captures upstream + fn. Fast form feeds cascading raw-advance
// through chained wrappers so a deep map/filter/take stack pays zero
// step-object allocations per iteration. Upstream's `next` closure is
// cached at factory time in captures[2] to drop the per-step map::find.
inline void _iter_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  auto mapped = _culebra_invoke1(fn_cls, {tag, data});
  *done = false;
  *out_tag = mapped.tag;
  *out_data = mapped.data;
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_map_fast_fn>({up, f, up_next_cell});
}

// filter: captures upstream + predicate + cached upstream next.
inline void _iter_filter_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  for (;;) {
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    JitValue v = {tag, data};
    culebra_runtime_value_retain(v.tag, v.data);  // for callback
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

__attribute__((used)) inline JitObject* culebra_runtime_iter_filter(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_filter_fast_fn>({up, f, up_next_cell});
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
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  rem_cell->value.data--;
  *done = false;
  *out_tag = tag;
  *out_data = data;
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_take(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_take_fast_fn>({up, rem, up_next_cell});
}

// skip: captures upstream + remaining-to-skip + cached upstream next.
inline void _iter_skip_fast_fn(JitClosure* cls, JitValue, bool* done,
                               int8_t* out_tag, int64_t* out_data) {
  auto* rem_cell = cls->captures[1];
  auto upstream = cls->captures[0]->value;
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  while (rem_cell->value.data > 0) {
    int8_t t;
    int64_t d;
    if (!_iter_advance_raw(up_next, upstream, &t, &d)) {
      *done = true;
      return;
    }
    _culebra_value_release_impl(t, d);
    rem_cell->value.data--;
  }
  if (!_iter_advance_raw(up_next, upstream, out_tag, out_data)) {
    *done = true;
    return;
  }
  *done = false;
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_skip(
    int8_t it, int64_t id, int64_t n) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* rem = culebra_runtime_cell_new(TAG_LONG, n);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_skip_fast_fn>({up, rem, up_next_cell});
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
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* fn_cls = reinterpret_cast<JitClosure*>(pv.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  JitValue v = {tag, data};
  culebra_runtime_value_retain(v.tag, v.data);
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

__attribute__((used)) inline JitObject* culebra_runtime_iter_take_while(
    int8_t it, int64_t id, int8_t ft, int64_t fd) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* p = culebra_runtime_cell_new(ft, fd);
  auto* stopped = culebra_runtime_cell_new(TAG_BOOL, 0);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_take_while_fast_fn>(
      {up, p, stopped, up_next_cell});
}

// enumerate: captures upstream + index + cached upstream next.
inline void _iter_enumerate_fast_fn(JitClosure* cls, JitValue, bool* done,
                                    int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto* idx_cell = cls->captures[1];
  auto* up_next = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  int8_t tag;
  int64_t data;
  if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
    *done = true;
    return;
  }
  auto* pair = culebra_runtime_object_new();
  culebra_runtime_object_set(pair, "index", false, TAG_LONG,
                             idx_cell->value.data, 0, 0);
  culebra_runtime_object_set(pair, "value", false, tag, data, 0, 0);
  idx_cell->value.data++;
  *done = false;
  *out_tag = TAG_OBJECT;
  *out_data = reinterpret_cast<int64_t>(pair);
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_enumerate(
    int8_t it, int64_t id) {
  culebra_runtime_value_retain(it, id);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* idx = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* up_next_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  return _iter_wrap_fast<&_iter_enumerate_fast_fn>({up, idx, up_next_cell});
}

// chain: captures iter1 + iter2 + phase + cached up1 next + cached up2 next.
inline void _iter_chain_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* phase_cell = cls->captures[2];
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[4]->value.data);
  if (phase_cell->value.data == 0) {
    auto up1 = cls->captures[0]->value;
    if (_iter_advance_raw(n1, up1, out_tag, out_data)) {
      *done = false;
      return;
    }
    phase_cell->value.data = 1;
  }
  auto up2 = cls->captures[1]->value;
  if (_iter_advance_raw(n2, up2, out_tag, out_data)) {
    *done = false;
    return;
  }
  *done = true;
}

inline JitObject* _iter_from_array_obj(int8_t at, int64_t ad);

// Coerce any iterable (Array / Object-with-iter / already-iterator) into an
// iterator Object Value. Returns a fresh +1 (caller owns the result).
// Throws `type error at line:col.` on non-iterable input.
inline JitValue _iter_coerce_iterable(int8_t t, int64_t d, int64_t line,
                                      int64_t col) {
  if (t == TAG_ARRAY) {
    auto* wrapped =
        _iter_from_array_obj(t, d);
    return {TAG_OBJECT, reinterpret_cast<int64_t>(wrapped)};
  }
  if (t == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(d);
    auto it = o->props.find("iter");
    if (it != o->props.end() && it->second.value.tag == TAG_FUNC) {
      auto iv = it->second.value;
      auto* iv_cls = reinterpret_cast<JitClosure*>(iv.data);
      culebra_runtime_value_retain(t, d);
      return reinterpret_cast<JitFn>(iv_cls->fn_ptr)(iv_cls, {t, d}, 0,
                                                     nullptr);
    }
  }
  throw std::runtime_error(std::format(
      "type error: target is not iterable at {}:{}.", line, col));
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_chain(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  // Coerce both inputs so raw Arrays can be chained without the caller
  // having to call `.iter()` first. Each coerce call returns a fresh +1.
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto* phase = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* n1 = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv1)));
  auto* n2 = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv2)));
  return _iter_wrap_fast<&_iter_chain_fast_fn>({u1, u2, phase, n1, n2});
}

// zip: captures iter1 + iter2 + cached up1 next + cached up2 next.
inline void _iter_zip_fast_fn(JitClosure* cls, JitValue, bool* done,
                              int8_t* out_tag, int64_t* out_data) {
  auto up1 = cls->captures[0]->value;
  auto up2 = cls->captures[1]->value;
  auto* n1 = reinterpret_cast<JitClosure*>(cls->captures[2]->value.data);
  auto* n2 = reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  int8_t t1, t2;
  int64_t d1, d2;
  if (!_iter_advance_raw(n1, up1, &t1, &d1)) {
    *done = true;
    return;
  }
  if (!_iter_advance_raw(n2, up2, &t2, &d2)) {
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

__attribute__((used)) inline JitObject* culebra_runtime_iter_zip(
    int8_t it1, int64_t id1, int8_t it2, int64_t id2, int64_t line,
    int64_t col) {
  auto iv1 = _iter_coerce_iterable(it1, id1, line, col);
  auto iv2 = _iter_coerce_iterable(it2, id2, line, col);
  auto* u1 = culebra_runtime_cell_new(iv1.tag, iv1.data);
  auto* u2 = culebra_runtime_cell_new(iv2.tag, iv2.data);
  auto* n1 = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv1)));
  auto* n2 = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv2)));
  return _iter_wrap_fast<&_iter_zip_fast_fn>({u1, u2, n1, n2});
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
    cp = static_cast<unsigned char>(s[off]);
    bytes = 1;
  }
  off_cell->value.data = off + static_cast<int64_t>(bytes);
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = static_cast<int64_t>(cp);
}

__attribute__((used)) inline JitObject* culebra_runtime_str_code_points(
    const char* s) {
  // Strings are not refcounted in JIT; the pointer stays valid as long
  // as the source String stays rooted somewhere (usually via the
  // caller's own slot). Lifetime is documented in §16.
  auto* buf_cell = culebra_runtime_cell_new(
      TAG_LONG, reinterpret_cast<int64_t>(s));
  auto* off_cell = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* len_cell = culebra_runtime_cell_new(
      TAG_LONG, static_cast<int64_t>(std::strlen(s)));
  return _iter_wrap_fast<&_iter_code_points_fast_fn>(
      {buf_cell, off_cell, len_cell});
}

// graphemes: eagerly materialize all cluster boundaries into a JitArray
// at factory time, then reuse the generic Array walker. This avoids a
// u32string-shaped leak (no cell tag currently owns `delete` of a
// non-trivial C++ type) and keeps the runtime surface minimal. The
// trade-off is a single O(n) decode up front instead of streaming —
// acceptable because user code that wants streaming usually prefers
// `.code_points()` anyway.
__attribute__((used)) inline JitObject* culebra_runtime_str_graphemes(
    const char* s) {
  std::u32string u32;
  size_t buf_size = std::strlen(s);
  unicode::utf8::decode(s, buf_size, u32);
  auto* arr = culebra_runtime_array_new();
  size_t cp_off = 0;
  while (cp_off < u32.size()) {
    size_t gl = unicode::grapheme_length(u32.data() + cp_off,
                                         u32.size() - cp_off);
    if (gl == 0) gl = 1;
    std::string out;
    unicode::utf8::encode(u32.data() + cp_off, gl, out);
    auto* heap = _culebra_heap_str(out);
    culebra_runtime_array_push(arr, TAG_STRING,
                               reinterpret_cast<int64_t>(heap));
    cp_off += gl;
  }
  return _iter_from_array_obj(TAG_ARRAY, reinterpret_cast<int64_t>(arr));
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

__attribute__((used)) inline JitObject* culebra_runtime_array_iter(
    JitArray* arr) {
  return _iter_from_array_obj(TAG_ARRAY, reinterpret_cast<int64_t>(arr));
}

// Object.iter(): yield keys (ascending std::map order) as Strings.
// Uses the same array-walker as array_iter.
__attribute__((used)) inline JitObject* culebra_runtime_object_iter(
    JitObject* obj) {
  auto* keys = culebra_runtime_object_keys(obj);
  return _iter_from_array_obj(TAG_ARRAY,
                              reinterpret_cast<int64_t>(keys));
}

// flat_map captures: upstream, fn, current-inner-iter (nullable),
// cached upstream next, cached inner next, and a packed (line << 32 |
// col) cell for error reporting when the callback returns a
// non-iterable.
inline void _iter_flat_map_fast_fn(JitClosure* cls, JitValue, bool* done,
                                   int8_t* out_tag, int64_t* out_data) {
  auto upstream = cls->captures[0]->value;
  auto fnv = cls->captures[1]->value;
  auto* fn_cls = reinterpret_cast<JitClosure*>(fnv.data);
  auto* inner_cell = cls->captures[2];
  auto* up_next =
      reinterpret_cast<JitClosure*>(cls->captures[3]->value.data);
  auto* inner_next_cell = cls->captures[4];
  int64_t packed = cls->captures[5]->value.data;
  int64_t line = packed >> 32;
  int64_t col = packed & 0xFFFFFFFF;
  for (;;) {
    auto inner_v = inner_cell->value;
    if (inner_v.tag == TAG_OBJECT && inner_v.data != 0) {
      auto* inner_next =
          reinterpret_cast<JitClosure*>(inner_next_cell->value.data);
      if (_iter_advance_raw(inner_next, inner_v, out_tag, out_data)) {
        *done = false;
        return;
      }
      _culebra_value_release_impl(inner_v.tag, inner_v.data);
      inner_cell->value = {0, 0};
      inner_next_cell->value = {TAG_LONG, 0};
    }
    int8_t tag;
    int64_t data;
    if (!_iter_advance_raw(up_next, upstream, &tag, &data)) {
      *done = true;
      return;
    }
    auto mapped = _culebra_invoke1(fn_cls, {tag, data});
    _culebra_value_release_impl(tag, data);
    auto iv = _iter_coerce_iterable(mapped.tag, mapped.data, line, col);
    _culebra_value_release_impl(mapped.tag, mapped.data);
    inner_cell->value = iv;
    inner_next_cell->value = {
        TAG_LONG, reinterpret_cast<int64_t>(_iter_next_closure(iv))};
  }
}

__attribute__((used)) inline JitObject* culebra_runtime_iter_flat_map(
    int8_t it, int64_t id, int8_t ft, int64_t fd, int64_t line,
    int64_t col) {
  culebra_runtime_value_retain(it, id);
  culebra_runtime_value_retain(ft, fd);
  auto* up = culebra_runtime_cell_new(it, id);
  auto* f = culebra_runtime_cell_new(ft, fd);
  auto* inner = culebra_runtime_cell_new(TAG_NIL, 0);
  auto* up_next = culebra_runtime_cell_new(
      TAG_LONG,
      reinterpret_cast<int64_t>(_iter_next_closure({it, id})));
  auto* inner_next = culebra_runtime_cell_new(TAG_LONG, 0);
  auto* loc = culebra_runtime_cell_new(
      TAG_LONG, (line << 32) | (col & 0xFFFFFFFF));
  return _iter_wrap_fast<&_iter_flat_map_fast_fn>(
      {up, f, inner, up_next, inner_next, loc});
}

inline void _math_range_fast_fn(JitClosure* cls, JitValue, bool* done,
                                int8_t* out_tag, int64_t* out_data) {
  auto* current_cell = cls->captures[0];
  auto* end_cell = cls->captures[1];
  int64_t current = current_cell->value.data;
  int64_t end = end_cell->value.data;
  if (current >= end) {
    *done = true;
    return;
  }
  *done = false;
  *out_tag = TAG_LONG;
  *out_data = current;
  current_cell->value.data = current + 1;
}

// Entry point called from for-in codegen (`rt::iter_advance`): returns
// 1 on a value step with `*out_tag`/`*out_data` holding a +1 Value, 0
// on done. `next_cls` is the `iter.next` closure resolved once before
// the loop; forwarded as the `this` capture slab on the fast path.
__attribute__((used)) inline int64_t culebra_runtime_iter_advance(
    JitClosure* next_cls, int8_t it, int64_t id, int8_t* out_tag,
    int64_t* out_data) {
  return _iter_advance_raw(next_cls, {it, id}, out_tag, out_data) ? 1 : 0;
}

__attribute__((used)) inline JitObject* culebra_runtime_math_range(
    int64_t start, int64_t end) {
  auto* current_cell = culebra_runtime_cell_new(TAG_LONG, start);
  auto* end_cell = culebra_runtime_cell_new(TAG_LONG, end);
  return _iter_wrap_fast<&_math_range_fast_fn>({current_cell, end_cell});
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
inline JitClosure* _culebra_expect_callback(int8_t fn_tag, int64_t fn_data,
                                            size_t expected_arity,
                                            const char* method_name,
                                            int64_t line, int64_t col) {
  if (fn_tag != TAG_FUNC) {
    throw std::runtime_error(
        std::format("type error at {}:{}.", line, col));
  }
  auto* fn = reinterpret_cast<JitClosure*>(fn_data);
  if (fn->arity != expected_arity) {
    throw std::runtime_error(std::format(
        "type error: {} expects a {}-parameter function at {}:{}.",
        method_name, expected_arity, line, col));
  }
  return fn;
}

__attribute__((used)) inline JitArray* culebra_runtime_array_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"map", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    culebra_runtime_array_push(out, r.tag, r.data);
  }
  return out;
}

__attribute__((used)) inline JitArray* culebra_runtime_array_filter(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"filter", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) {
      culebra_runtime_value_retain(e.tag, e.data);
      culebra_runtime_array_push(out, e.tag, e.data);
    }
  }
  return out;
}

__attribute__((used)) inline void culebra_runtime_array_for_each(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"for_each", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

// find returns the first matching element (or nil) via out-params.
__attribute__((used)) inline void culebra_runtime_array_find(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"find", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
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

__attribute__((used)) inline int64_t culebra_runtime_array_any(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"any", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

__attribute__((used)) inline int64_t culebra_runtime_array_all(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"all", line, col);
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    bool keep = (r.tag == TAG_BOOL || r.tag == TAG_LONG) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

__attribute__((used)) inline JitArray* culebra_runtime_array_flat_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"flat_map", line, col);
  auto* out = culebra_runtime_array_new();
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = _culebra_invoke1(fn, e);
    if (r.tag != TAG_ARRAY) {
      _culebra_value_release_impl(r.tag, r.data);
      throw std::runtime_error(std::format(
          "type error: flat_map callback must return an Array at {}:{}.",
          line, col));
    }
    auto* inner = reinterpret_cast<JitArray*>(r.data);
    for (size_t j = 0; j < inner->size; j++) {
      auto ie = inner->items[j];
      culebra_runtime_value_retain(ie.tag, ie.data);
      culebra_runtime_array_push(out, ie.tag, ie.data);
    }
    _culebra_value_release_impl(r.tag, r.data);
  }
  return out;
}

// sort_by: evaluates the key function on each element once, stable-sorts
// in place by those keys. Mutates. Keys are released on every exit path
// (including during a throw from the key comparison).
__attribute__((used)) inline void culebra_runtime_array_sort_by(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1,"sort_by", line, col);
  std::vector<std::pair<JitValue, size_t>> keyed;
  keyed.reserve(arr->size);
  auto release_keys = [&] {
    for (auto& [k, _] : keyed) _culebra_value_release_impl(k.tag, k.data);
  };
  try {
    for (size_t i = 0; i < arr->size; i++) {
      auto e = arr->items[i];
      culebra_runtime_value_retain(e.tag, e.data);
      keyed.emplace_back(_culebra_invoke1(fn, e), i);
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) {
                       return _culebra_value_ord(
                           a.first.tag, a.first.data,
                           b.first.tag, b.first.data,
                           [](double x, double y) { return x < y; });
                     });
    std::vector<JitValue> sorted(arr->size);
    for (size_t i = 0; i < keyed.size(); i++) {
      sorted[i] = arr->items[keyed[i].second];
    }
    for (size_t i = 0; i < arr->size; i++) arr->items[i] = sorted[i];
  } catch (...) {
    release_keys();
    throw;
  }
  release_keys();
}

// reduce returns the final accumulator via out-params (avoids relying on
// cross-language struct-return ABI).
__attribute__((used)) inline void culebra_runtime_array_reduce(
    JitArray* arr, int8_t init_tag, int64_t init_data, int8_t fn_tag,
    int64_t fn_data, int64_t line, int64_t col, int8_t* out_tag,
    int64_t* out_data) {
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 2, "reduce", line, col);
  JitValue acc = {init_tag, init_data};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    acc = _culebra_invoke2(fn, acc, e);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
}

// Eager Array variants of sum/product/min/max. Same semantics as the
// iterator methods above; non-Long elements and empty min/max raise a
// type error at L:C.

__attribute__((used)) inline int64_t culebra_runtime_array_sum(
    JitArray* arr, int64_t line, int64_t col) {
  int64_t acc = 0;
  for (size_t i = 0; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    acc += e.data;
  }
  return acc;
}

__attribute__((used)) inline int64_t culebra_runtime_array_product(
    JitArray* arr, int64_t line, int64_t col) {
  int64_t acc = 1;
  for (size_t i = 0; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    acc *= e.data;
  }
  return acc;
}

__attribute__((used)) inline int64_t culebra_runtime_array_min(
    JitArray* arr, int64_t line, int64_t col) {
  if (arr->size == 0) {
    throw std::runtime_error(std::format(
        "type error: min of empty Array at {}:{}.", line, col));
  }
  if (arr->items[0].tag != TAG_LONG) {
    throw std::runtime_error(
        std::format("type error at {}:{}.", line, col));
  }
  int64_t best = arr->items[0].data;
  for (size_t i = 1; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    if (e.data < best) best = e.data;
  }
  return best;
}

__attribute__((used)) inline int64_t culebra_runtime_array_max(
    JitArray* arr, int64_t line, int64_t col) {
  if (arr->size == 0) {
    throw std::runtime_error(std::format(
        "type error: max of empty Array at {}:{}.", line, col));
  }
  if (arr->items[0].tag != TAG_LONG) {
    throw std::runtime_error(
        std::format("type error at {}:{}.", line, col));
  }
  int64_t best = arr->items[0].data;
  for (size_t i = 1; i < arr->size; i++) {
    auto& e = arr->items[i];
    if (e.tag != TAG_LONG) {
      throw std::runtime_error(
          std::format("type error at {}:{}.", line, col));
    }
    if (e.data > best) best = e.data;
  }
  return best;
}

// Length in UTF-8 bytes of the next scalar at `offset`. Returns 0
// once `offset >= len`; on an invalid lead byte, returns 1 (emit the
// raw byte to avoid stalling the iterator). Mirrors the interpreter's
// String.iter semantics.
__attribute__((used)) inline int64_t culebra_runtime_utf8_scalar_len(
    const char* s, int64_t offset, int64_t len) {
  if (offset >= len) return 0;
  auto r = peg::codepoint_length(s + offset, len - offset);
  return r == 0 ? 1 : static_cast<int64_t>(r);
}

// Heap-copy `scalar_len` bytes from `s + offset` into a new String.
// Used by JIT for-in over String to yield one-scalar Strings.
__attribute__((used)) inline const char* culebra_runtime_str_scalar_at(
    const char* s, int64_t offset, int64_t scalar_len) {
  return _culebra_heap_str(std::string_view(s + offset,
                                            static_cast<size_t>(scalar_len)));
}

__attribute__((used)) inline int64_t culebra_runtime_str_size(const char* s) {
  return static_cast<int64_t>(std::strlen(s));
}

__attribute__((used)) inline const char* culebra_runtime_str_upper(
    const char* s) {
  auto n = std::strlen(s);
  auto* buf = static_cast<char*>(std::malloc(n + 1));
  for (size_t i = 0; i < n; i++) {
    buf[i] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
  }
  buf[n] = '\0';
  return buf;
}

__attribute__((used)) inline const char* culebra_runtime_str_lower(
    const char* s) {
  auto n = std::strlen(s);
  auto* buf = static_cast<char*>(std::malloc(n + 1));
  for (size_t i = 0; i < n; i++) {
    buf[i] =
        static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  }
  buf[n] = '\0';
  return buf;
}

__attribute__((used)) inline const char* culebra_runtime_str_trim(
    const char* s) {
  auto n = std::strlen(s);
  size_t start = 0;
  while (start < n && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = n;
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  auto len = end - start;
  auto* buf = static_cast<char*>(std::malloc(len + 1));
  std::memcpy(buf, s + start, len);
  buf[len] = '\0';
  return buf;
}

__attribute__((used)) inline JitArray* culebra_runtime_str_split(
    const char* s, const char* sep) {
  auto* r = culebra_runtime_array_new();
  std::string_view sv(s);
  std::string_view sp(sep);
  if (sp.empty()) {
    culebra_runtime_array_push(
        r, TAG_STRING,
        reinterpret_cast<int64_t>(_culebra_heap_str(sv)));
    return r;
  }
  size_t pos = 0;
  while (true) {
    auto p = sv.find(sp, pos);
    if (p == std::string_view::npos) {
      auto* piece = _culebra_heap_str(sv.substr(pos));
      culebra_runtime_array_push(r, 4, reinterpret_cast<int64_t>(piece));
      break;
    }
    auto* piece = _culebra_heap_str(sv.substr(pos, p - pos));
    culebra_runtime_array_push(r, 4, reinterpret_cast<int64_t>(piece));
    pos = p + sp.size();
  }
  return r;
}

__attribute__((used)) inline bool culebra_runtime_str_contains(
    const char* s, const char* sub) {
  return std::strstr(s, sub) != nullptr;
}

__attribute__((used)) inline bool culebra_runtime_str_starts_with(
    const char* s, const char* prefix) {
  auto lp = std::strlen(prefix);
  return std::strncmp(s, prefix, lp) == 0;
}

__attribute__((used)) inline bool culebra_runtime_str_ends_with(
    const char* s, const char* suffix) {
  auto ls = std::strlen(s);
  auto lsuf = std::strlen(suffix);
  if (lsuf > ls) return false;
  return std::strncmp(s + (ls - lsuf), suffix, lsuf) == 0;
}

__attribute__((used)) inline const char* culebra_runtime_str_slice(
    const char* s, int64_t start, int64_t end) {
  int64_t size = static_cast<int64_t>(std::strlen(s));
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  auto len = static_cast<size_t>(end - start);
  auto* buf = static_cast<char*>(std::malloc(len + 1));
  std::memcpy(buf, s + start, len);
  buf[len] = '\0';
  return buf;
}

__attribute__((used)) inline void culebra_runtime_array_pop(
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
__attribute__((used)) inline JitArray* culebra_runtime_array_slice2(
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

__attribute__((used)) inline const char* culebra_runtime_array_join(
    JitArray* arr, const char* sep) {
  std::string out;
  for (size_t i = 0; i < arr->size; i++) {
    if (i > 0) out += sep;
    auto tag = arr->items[i].tag;
    auto data = arr->items[i].data;
    if (tag == TAG_STRING) {
      out += reinterpret_cast<const char*>(data);
    } else {
      out += _culebra_value_to_str_impl(tag, data);
    }
  }
  return _culebra_heap_str(out);
}

__attribute__((used)) inline int64_t culebra_runtime_array_index_of(
    JitArray* arr, int8_t tag, int64_t data) {
  for (size_t i = 0; i < arr->size; i++) {
    if (_culebra_value_equal(arr->items[i].tag, arr->items[i].data, tag, data))
      return static_cast<int64_t>(i);
  }
  return -1;
}

__attribute__((used)) inline bool culebra_runtime_array_contains(
    JitArray* arr, int8_t tag, int64_t data) {
  return culebra_runtime_array_index_of(arr, tag, data) >= 0;
}

__attribute__((used)) inline void culebra_runtime_array_reverse(JitArray* arr) {
  if (arr->size < 2) return;
  for (size_t i = 0, j = arr->size - 1; i < j; i++, j--) {
    auto tmp = arr->items[i];
    arr->items[i] = arr->items[j];
    arr->items[j] = tmp;
  }
}

__attribute__((used)) inline JitArray* culebra_runtime_object_keys(
    JitObject* obj) {
  auto* r = culebra_runtime_array_new();
  for (const auto& [k, _] : obj->props) {
    auto* buf = _culebra_heap_str(k);
    culebra_runtime_array_push(r, TAG_STRING, reinterpret_cast<int64_t>(buf));
  }
  return r;
}

__attribute__((used)) inline void culebra_runtime_object_remove(
    JitObject* obj, const char* key) {
  auto it = obj->props.find(key);
  if (it == obj->props.end()) return;
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  obj->props.erase(it);
  if (std::string_view(key) == "drop") obj->has_drop = false;
}

}  // extern "C" (close briefly for C++ impl helpers)

// --- Reference counting (C++ impl helpers, not extern "C") ---
//
// Only heap-allocated refcounted types have refcount headers:
//   TAG_FUNC (closure), TAG_ARRAY, TAG_OBJECT, and JitCell (internal).
// Strings (TAG_STRING) are NOT refcounted in this implementation and leak.
// Nil/Bool/Long are value types and have no refcount.

inline void _culebra_cell_release(JitCell* c);

// RAII drop: if `o` has a 0-arg `drop` Function property, invoke it
// with `this` bound to `o` before any child values are released.
// Called from the normal refcount-0 path and from the cycle collector.
// The contract (drop must be a 0-arg Function) is validated at
// assignment time, so a mis-shaped drop is silently skipped here as a
// belt-and-braces check.
//
// Refcount trick (matches interpreter's shared_ptr + no-op deleter):
// bump high enough that the drop body's function-frame release of its
// owned `this` slot — plus any retain/release pairs in the body —
// can't drive refcount back to 0 and re-enter destruction. The frame
// release subtracts 1, so 2 is the minimum safe baseline. After drop
// returns we slam the count to 0; if the body resurrected `o` by
// storing it somewhere, the caller owns the resulting dangling
// reference (matches interp's documented warning).
inline void _culebra_call_drop_if_present(JitObject* o) {
  if (!o || !o->has_drop) return;
  auto it = o->props.find("drop");
  if (it == o->props.end()) return;
  const auto& v = it->second.value;
  if (v.tag != GC_TAG_FUNC) return;
  auto* cls = reinterpret_cast<JitClosure*>(v.data);
  if (!cls || cls->arity != 0) return;

  o->refcount = 2;
  JitValue this_val{GC_TAG_OBJECT, reinterpret_cast<int64_t>(o)};
  try {
    auto r = reinterpret_cast<JitFn>(cls->fn_ptr)(cls, this_val, 0,
                                                  nullptr);
    _culebra_value_release_impl(r.tag, r.data);
  } catch (const std::exception& e) {
    std::cerr << "drop: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "drop: unknown error" << std::endl;
  }
  o->refcount = 0;
}

inline void _culebra_value_release_impl(int8_t tag, int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case GC_TAG_FUNC: {
      auto* c = reinterpret_cast<JitClosure*>(data);
      if (--c->refcount == 0) {
        for (size_t i = 0; i < c->n_captures; i++) {
          _culebra_cell_release(c->captures[i]);
        }
        std::free(c->captures);
        _gc().remove(c);
        delete c;
      }
      break;
    }
    case GC_TAG_ARRAY: {
      auto* a = reinterpret_cast<JitArray*>(data);
      if (--a->refcount == 0) {
        for (size_t i = 0; i < a->size; i++) {
          _culebra_value_release_impl(a->items[i].tag, a->items[i].data);
        }
        delete[] a->items;
        _gc().remove(a);
        delete a;
      }
      break;
    }
    case GC_TAG_OBJECT: {
      auto* o = reinterpret_cast<JitObject*>(data);
      if (--o->refcount == 0) {
        _culebra_call_drop_if_present(o);
        for (auto& [name, entry] : o->props) {
          _culebra_value_release_impl(entry.value.tag, entry.value.data);
        }
        _gc().remove(o);
        delete o;
      }
      break;
    }
    default:
      break;  // TAG_NIL/BOOL/LONG/STRING: no-op
  }
}

inline void _culebra_cell_release(JitCell* c) {
  if (!c) return;
  if (--c->refcount == 0) {
    _culebra_value_release_impl(c->value.tag, c->value.data);
    _gc().remove(c);
    delete c;
  }
}

extern "C" {

__attribute__((used)) inline void culebra_runtime_value_retain(int8_t tag,
                                                               int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case 3:
      reinterpret_cast<JitClosure*>(data)->refcount++;
      break;
    case 5:
      reinterpret_cast<JitArray*>(data)->refcount++;
      break;
    case 6:
      reinterpret_cast<JitObject*>(data)->refcount++;
      break;
  }
}

__attribute__((used)) inline void culebra_runtime_value_release(int8_t tag,
                                                                int64_t data) {
  _culebra_value_release_impl(tag, data);
}

__attribute__((used)) inline void culebra_runtime_cell_retain(JitCell* c) {
  if (c) c->refcount++;
}

__attribute__((used)) inline void culebra_runtime_cell_release(JitCell* c) {
  _culebra_cell_release(c);
}

}  // extern "C"

namespace culebra {

// ---------------------------------------------------------------------------
// Runtime function names
//
// Centralized so: a typo fails to compile (unknown identifier) instead
// of silently producing an unresolved LLVM declaration; renames happen
// once; the full runtime ABI is scannable in one place.
// ---------------------------------------------------------------------------
namespace rt {
inline constexpr auto array_all           = "culebra_runtime_array_all";
inline constexpr auto array_any           = "culebra_runtime_array_any";
inline constexpr auto array_contains      = "culebra_runtime_array_contains";
inline constexpr auto array_filter        = "culebra_runtime_array_filter";
inline constexpr auto array_find          = "culebra_runtime_array_find";
inline constexpr auto array_flat_map      = "culebra_runtime_array_flat_map";
inline constexpr auto array_for_each      = "culebra_runtime_array_for_each";
inline constexpr auto array_get           = "culebra_runtime_array_get";
inline constexpr auto array_index_of      = "culebra_runtime_array_index_of";
inline constexpr auto array_join          = "culebra_runtime_array_join";
inline constexpr auto array_map           = "culebra_runtime_array_map";
inline constexpr auto array_new           = "culebra_runtime_array_new";
inline constexpr auto array_pop           = "culebra_runtime_array_pop";
inline constexpr auto array_push          = "culebra_runtime_array_push";
inline constexpr auto array_max           = "culebra_runtime_array_max";
inline constexpr auto array_min           = "culebra_runtime_array_min";
inline constexpr auto array_product       = "culebra_runtime_array_product";
inline constexpr auto array_reduce        = "culebra_runtime_array_reduce";
inline constexpr auto array_sum           = "culebra_runtime_array_sum";
inline constexpr auto array_resize        = "culebra_runtime_array_resize";
inline constexpr auto array_reverse       = "culebra_runtime_array_reverse";
inline constexpr auto array_set           = "culebra_runtime_array_set";
inline constexpr auto array_set_or_push   = "culebra_runtime_array_set_or_push";
inline constexpr auto array_size          = "culebra_runtime_array_size";
inline constexpr auto array_slice         = "culebra_runtime_array_slice";
inline constexpr auto array_slice2        = "culebra_runtime_array_slice2";
inline constexpr auto array_sort_by       = "culebra_runtime_array_sort_by";
// Trailing underscore on `assert_` / `throw_` dodges C++ keyword
// collision (the `assert` macro from <cassert>, and the `throw` keyword).
inline constexpr auto assert_             = "culebra_runtime_assert";
inline constexpr auto cell_new            = "culebra_runtime_cell_new";
inline constexpr auto cell_release        = "culebra_runtime_cell_release";
inline constexpr auto cell_retain         = "culebra_runtime_cell_retain";
inline constexpr auto closure_new         = "culebra_runtime_closure_new";
inline constexpr auto debugger_break      = "culebra_runtime_debugger_break";
inline constexpr auto defer_mark          = "culebra_runtime_defer_mark";
inline constexpr auto defer_push          = "culebra_runtime_defer_push";
inline constexpr auto defer_run_to        = "culebra_runtime_defer_run_to";
inline constexpr auto div_zero            = "culebra_runtime_div_zero";
inline constexpr auto input               = "culebra_runtime_input";
inline constexpr auto math_pow            = "culebra_runtime_math_pow";
inline constexpr auto num_add             = "culebra_runtime_num_add";
inline constexpr auto num_sub             = "culebra_runtime_num_sub";
inline constexpr auto num_mul             = "culebra_runtime_num_mul";
inline constexpr auto num_div             = "culebra_runtime_num_div";
inline constexpr auto num_mod             = "culebra_runtime_num_mod";
inline constexpr auto num_pow             = "culebra_runtime_num_pow";
inline constexpr auto num_neg             = "culebra_runtime_num_neg";
inline constexpr auto sys_argv            = "culebra_runtime_sys_argv";
inline constexpr auto sys_env             = "culebra_runtime_sys_env";
inline constexpr auto sys_exit            = "culebra_runtime_sys_exit";
inline constexpr auto object_get          = "culebra_runtime_object_get";
inline constexpr auto object_has          = "culebra_runtime_object_has";
inline constexpr auto object_keys         = "culebra_runtime_object_keys";
inline constexpr auto object_new          = "culebra_runtime_object_new";
inline constexpr auto object_remove       = "culebra_runtime_object_remove";
inline constexpr auto object_set          = "culebra_runtime_object_set";
inline constexpr auto check_well_known_prop =
    "culebra_runtime_check_well_known_prop";
inline constexpr auto object_size         = "culebra_runtime_object_size";
inline constexpr auto print               = "culebra_runtime_print";
inline constexpr auto puts                = "culebra_runtime_puts";
inline constexpr auto iota                = "culebra_runtime_iota";
inline constexpr auto math_range           = "culebra_runtime_math_range";
inline constexpr auto iter_collect         = "culebra_runtime_iter_collect";
inline constexpr auto iter_count           = "culebra_runtime_iter_count";
inline constexpr auto iter_for_each        = "culebra_runtime_iter_for_each";
inline constexpr auto iter_reduce          = "culebra_runtime_iter_reduce";
inline constexpr auto iter_find            = "culebra_runtime_iter_find";
inline constexpr auto iter_any             = "culebra_runtime_iter_any";
inline constexpr auto iter_all             = "culebra_runtime_iter_all";
inline constexpr auto iter_max             = "culebra_runtime_iter_max";
inline constexpr auto iter_min             = "culebra_runtime_iter_min";
inline constexpr auto iter_product         = "culebra_runtime_iter_product";
inline constexpr auto iter_sum             = "culebra_runtime_iter_sum";
inline constexpr auto iter_map             = "culebra_runtime_iter_map";
inline constexpr auto iter_filter          = "culebra_runtime_iter_filter";
inline constexpr auto iter_take            = "culebra_runtime_iter_take";
inline constexpr auto iter_skip            = "culebra_runtime_iter_skip";
inline constexpr auto iter_take_while      = "culebra_runtime_iter_take_while";
inline constexpr auto iter_chain           = "culebra_runtime_iter_chain";
inline constexpr auto iter_zip             = "culebra_runtime_iter_zip";
inline constexpr auto iter_enumerate       = "culebra_runtime_iter_enumerate";
inline constexpr auto iter_flat_map        = "culebra_runtime_iter_flat_map";
inline constexpr auto iter_advance         = "culebra_runtime_iter_advance";
inline constexpr auto str_code_points      = "culebra_runtime_str_code_points";
inline constexpr auto str_graphemes        = "culebra_runtime_str_graphemes";
inline constexpr auto array_iter           = "culebra_runtime_array_iter";
inline constexpr auto object_iter          = "culebra_runtime_object_iter";
inline constexpr auto read_file           = "culebra_runtime_read_file";
inline constexpr auto rethrow             = "culebra_runtime_rethrow";
inline constexpr auto str_cmp             = "culebra_runtime_str_cmp";
inline constexpr auto str_concat          = "culebra_runtime_str_concat";
inline constexpr auto str_contains        = "culebra_runtime_str_contains";
inline constexpr auto str_ends_with       = "culebra_runtime_str_ends_with";
inline constexpr auto str_eq              = "culebra_runtime_str_eq";
inline constexpr auto str_lower           = "culebra_runtime_str_lower";
inline constexpr auto str_scalar_at       = "culebra_runtime_str_scalar_at";
inline constexpr auto str_size            = "culebra_runtime_str_size";
inline constexpr auto utf8_scalar_len     = "culebra_runtime_utf8_scalar_len";
inline constexpr auto str_slice           = "culebra_runtime_str_slice";
inline constexpr auto str_split           = "culebra_runtime_str_split";
inline constexpr auto str_starts_with     = "culebra_runtime_str_starts_with";
inline constexpr auto str_trim            = "culebra_runtime_str_trim";
inline constexpr auto str_upper           = "culebra_runtime_str_upper";
inline constexpr auto throw_              = "culebra_runtime_throw";
inline constexpr auto to_long             = "culebra_runtime_to_long";
inline constexpr auto to_long_any         = "culebra_runtime_to_long_any";
inline constexpr auto to_float_any        = "culebra_runtime_to_float_any";
inline constexpr auto type_check          = "culebra_runtime_type_check";
inline constexpr auto type_error          = "culebra_runtime_type_error";
inline constexpr auto arity_error         = "culebra_runtime_arity_error";
inline constexpr auto args_slice_to_array =
    "culebra_runtime_args_slice_to_array";
inline constexpr auto type_of             = "culebra_runtime_type_of";
inline constexpr auto value_equal         = "culebra_runtime_value_equal";
inline constexpr auto value_less          = "culebra_runtime_value_less";
inline constexpr auto value_leq           = "culebra_runtime_value_leq";
inline constexpr auto value_greater       = "culebra_runtime_value_greater";
inline constexpr auto value_geq           = "culebra_runtime_value_geq";
inline constexpr auto value_release       = "culebra_runtime_value_release";
inline constexpr auto value_retain        = "culebra_runtime_value_retain";
inline constexpr auto value_to_display    = "culebra_runtime_value_to_display";
inline constexpr auto write_file          = "culebra_runtime_write_file";
}  // namespace rt

// ---------------------------------------------------------------------------
// JIT compiler implementation
// ---------------------------------------------------------------------------

struct JIT {
  // Per-variable slot in a scope: either a stack-allocated value or a cell
  // (heap-allocated box for closure capture).
  struct VarSlot {
    enum Kind { Stack, Cell };
    Kind kind;
    llvm::AllocaInst* alloca;  // Stack: holds Value. Cell: holds Cell pointer.
    // True when the slot owns the +1 ref it holds — its value will be
    // released on scope exit. False for borrowed views: capture cells
    // (the closure object owns the cell) and any other slot whose ref
    // count is managed elsewhere. Required to keep callers honest;
    // every binding has a clear ownership story.
    bool owned;
  };

  // Analysis result for a function (including top-level __culebra_main).
  struct FuncInfo {
    std::vector<std::string> free_vars;   // captured from outer
    std::set<std::string> captured_locals;  // my locals captured by nested
    // EH/defer emission flags (populated by scan_eh_defer):
    bool has_eh = false;        // contains TRY or any scope with defers
    bool has_fn_defer = false;  // contains DEFER directly in fn body
  };

  // LEXICAL_SCOPE nodes that contain a DEFER within their own scope level
  // (not crossing nested LEXICAL_SCOPE / FUNCTION / DEFER). Scopes not in
  // this set skip emitting scope.mark / scope.cleanup landingpad.
  std::unordered_set<const peg::Ast*> scope_has_defer_;

  // Variable scoping: `slots` for lookup, `order` for LIFO release.
  // `std::map` iterators are stable under try_emplace, so `order` can
  // hold them directly — no second string copy and no per-release
  // lookup.
  struct Scope {
    using Slots = std::map<std::string, VarSlot>;
    Slots slots;
    std::vector<Slots::iterator> order;
  };

  // RAII snapshot of compiler per-function state. Entering a nested
  // LLVM function body (compile_function / compile_defer) constructs
  // one; the dtor (or an explicit `restore()` before the caller's
  // closure-build) restores the outer context. Ctor resets all fields
  // to safe defaults — outer's lpad, fn-mark, scopes, etc. are
  // intentionally not inherited since they live in a different LLVM
  // function. `restore()` is idempotent.
  struct CompilerStateSaver {
    JIT* jit;
    llvm::BasicBlock* insert_block;
    std::vector<Scope> scopes;
    const FuncInfo* info;
    llvm::Value* closure_arg;
    std::string_view return_type;
    llvm::BasicBlock* lpad;
    llvm::Value* fn_defer_mark;

    explicit CompilerStateSaver(JIT& j)
        : jit(&j),
          insert_block(j.builder_.GetInsertBlock()),
          scopes(std::exchange(j.scopes_, {})),
          info(std::exchange(j.current_info_, nullptr)),
          closure_arg(std::exchange(j.current_closure_arg_, nullptr)),
          return_type(std::exchange(j.current_return_type_, {})),
          lpad(std::exchange(j.current_lpad_, nullptr)),
          fn_defer_mark(std::exchange(j.current_fn_defer_mark_, nullptr)) {}

    void restore() {
      if (!jit) return;
      // Caller is expected to pop every scope it pushed; anything left
      // behind signals a push/pop imbalance that would silently leak.
      assert(jit->scopes_.empty() && "unbalanced push_scope/pop_scope");
      jit->current_fn_defer_mark_ = fn_defer_mark;
      jit->current_lpad_ = lpad;
      jit->current_return_type_ = return_type;
      jit->current_closure_arg_ = closure_arg;
      jit->current_info_ = info;
      jit->scopes_ = std::move(scopes);
      jit->builder_.SetInsertPoint(insert_block);
      jit = nullptr;
    }

    ~CompilerStateSaver() { restore(); }

    CompilerStateSaver(const CompilerStateSaver&) = delete;
    CompilerStateSaver& operator=(const CompilerStateSaver&) = delete;
    CompilerStateSaver(CompilerStateSaver&&) = delete;
    CompilerStateSaver& operator=(CompilerStateSaver&&) = delete;
  };

  // When inside a `try { ... }` region, points at the landingpad BB
  // that catches `CulebraException` for that region. Any call emitted
  // while this is non-null is emitted as `invoke` with this as the
  // unwind destination, so a user `throw` propagates back to the
  // nearest enclosing `try`. Nested try blocks save/restore.
  llvm::BasicBlock* current_lpad_ = nullptr;

  // Targets for the innermost enclosing loop. `break` jumps to the
  // break target (after the loop); `continue` jumps to the continue
  // target (loop-header / increment). Loops push a frame on entry and
  // pop on exit; nested loops stack correctly.
  struct LoopBlocks {
    llvm::BasicBlock* continue_target;
    llvm::BasicBlock* break_target;
  };
  std::vector<LoopBlocks> loop_stack_;

  // Unified call-site emitter: `invoke` if inside a try, else `call`.
  // All JIT-generated call sites (runtime functions, user functions,
  // closures) go through this so user `throw` propagates uniformly.
  llvm::CallBase* emit_call(llvm::FunctionCallee callee,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    if (current_lpad_) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
      auto inv = builder_.CreateInvoke(callee, contBB, current_lpad_, args,
                                       name);
      builder_.SetInsertPoint(contBB);
      return inv;
    }
    return builder_.CreateCall(callee, args, name);
  }

  // Indirect-call overload (function pointer + explicit FunctionType).
  llvm::CallBase* emit_call(llvm::FunctionType* fty, llvm::Value* fnPtr,
                            llvm::ArrayRef<llvm::Value*> args,
                            const llvm::Twine& name = "") {
    if (current_lpad_) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto contBB = llvm::BasicBlock::Create(ctx_, "call.cont", fn);
      auto inv = builder_.CreateInvoke(fty, fnPtr, contBB, current_lpad_, args,
                                       name);
      builder_.SetInsertPoint(contBB);
      return inv;
    }
    return builder_.CreateCall(fty, fnPtr, args, name);
  }

  // Re-raise the currently-handled exception (inside a catch-all
  // cleanup landingpad). If `outerLpad` is non-null, invoke
  // `__cxa_rethrow` with it as the unwind target so the exception
  // reaches that outer lpad within the same LLVM function; otherwise
  // a plain call lets the exception propagate out to the caller.
  void emit_rethrow(llvm::BasicBlock* outerLpad) {
    auto rethrowFn = module_->getOrInsertFunction(
        "__cxa_rethrow", builder_.getVoidTy());
    auto fn = builder_.GetInsertBlock()->getParent();
    if (outerLpad) {
      auto deadBB = llvm::BasicBlock::Create(ctx_, "rethrow.dead", fn);
      builder_.CreateInvoke(rethrowFn, deadBB, outerLpad, {});
      builder_.SetInsertPoint(deadBB);
    } else {
      builder_.CreateCall(rethrowFn, {});
    }
    builder_.CreateUnreachable();
  }

  // Build a catch-all cleanup landingpad at the current insertion
  // point: catches any in-flight exception, runs the scope's defers
  // back to `mark`, and re-raises so the exception keeps propagating
  // (to `outerLpad` if given, otherwise out of the current function).
  void emit_cleanup_landingpad(llvm::BasicBlock* cleanupBB,
                               llvm::Value* mark,
                               llvm::BasicBlock* outerLpad,
                               const char* excName) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    builder_.SetInsertPoint(cleanupBB);
    auto lpadTy = llvm::StructType::get(ptrTy, builder_.getInt32Ty());
    auto lpad = builder_.CreateLandingPad(lpadTy, 1, excName);
    lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));  // catch-all
    auto excPtr = builder_.CreateExtractValue(lpad, {0});
    builder_.CreateCall(
        module_->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy),
        {excPtr});
    builder_.CreateCall(
        module_->getFunction(rt::defer_run_to), {mark});
    emit_rethrow(outerLpad);
  }

  // Itanium C++ personality. Same attribute used by -fexceptions output.
  llvm::Constant* get_personality_fn() {
    auto fty = llvm::FunctionType::get(builder_.getInt32Ty(), {}, true);
    auto callee = module_->getOrInsertFunction("__gxx_personality_v0", fty);
    return llvm::cast<llvm::Constant>(callee.getCallee());
  }

  static inline void run(const std::shared_ptr<peg::Ast>& ast,
                         bool emit_llvm = false, bool debug = false,
                         int opt_level = 2) {
    using namespace llvm;

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("culebra", *ctx);
    // Apply the host's data layout so struct field offsets, alignments
    // and pointer sizes match the C++ runtime we're linking against.
    // Without this, a {i8, i64} Value is laid out with i64 at offset 1
    // (no padding), diverging from C++'s 16-byte layout the runtime
    // uses for JitValue — any alloca-backed arg-slab would break.
    {
      llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
      std::string err;
      auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
      if (target) {
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple, "", "", {}, {}));
        if (tm) {
          mod->setDataLayout(tm->createDataLayout());
          mod->setTargetTriple(triple);
        }
      }
    }
    IRBuilder<> builder(*ctx);

    JIT jit(ctx.get(), mod.get(), builder);
    jit.debug_enabled_ = debug;
    jit.declare_runtime_functions();

    // Pre-pass: free variable analysis for the whole program
    jit.main_info_ = jit.analyze_program(*ast);
    jit.current_info_ = &jit.main_info_;

    // Create __culebra_main: void ()
    auto mainFnType = FunctionType::get(builder.getVoidTy(), {}, false);
    auto mainFn = Function::Create(mainFnType, GlobalValue::ExternalLinkage,
                                   "__culebra_main", mod.get());
    if (jit.main_info_.has_eh) {
      mainFn->setPersonalityFn(jit.get_personality_fn());
    }

    auto entryBB = BasicBlock::Create(*ctx, "entry", mainFn);
    builder.SetInsertPoint(entryBB);

    jit.push_scope();

    // Top-level defers run on normal completion; on an uncaught throw
    // they are skipped (wrap in a `{}` or try/catch for throw-path
    // cleanup).
    llvm::Value* mainMark = nullptr;
    if (jit.main_info_.has_fn_defer) {
      mainMark = builder.CreateCall(
          mod->getFunction(rt::defer_mark), {}, "main.mark");
      jit.current_fn_defer_mark_ = mainMark;
    }

    jit.compile(*ast);

    if (!builder.GetInsertBlock()->getTerminator()) {
      if (mainMark) {
        builder.CreateCall(
            mod->getFunction(rt::defer_run_to), {mainMark});
      }
      jit.release_all_scopes_for_exit();
      builder.CreateRetVoid();
    }

    jit.current_fn_defer_mark_ = nullptr;
    jit.pop_scope();
    jit.current_info_ = nullptr;

    verifyFunction(*mainFn);

    if (opt_level > 0) {
      optimize_module(*mod, opt_level);
    }

    if (emit_llvm) {
      mod->print(outs(), nullptr);
    } else {
      exec(std::move(ctx), std::move(mod));
    }
  }

  static void optimize_module(llvm::Module& mod, int opt_level) {
    using namespace llvm;
    PassBuilder PB;

    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    OptimizationLevel level;
    switch (opt_level) {
      case 1:
        level = OptimizationLevel::O1;
        break;
      case 2:
        level = OptimizationLevel::O2;
        break;
      case 3:
        level = OptimizationLevel::O3;
        break;
      default:
        level = OptimizationLevel::O2;
        break;
    }

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
    MPM.run(mod, MAM);
  }

  // Built-in type method dispatch (language-level; lives in jit.h).
  llvm::Value* compile_builtin_method(const std::string& method,
                                      const peg::Ast& argsAst,
                                      llvm::Value* receiver);

  // Method names with JIT-native codegen in compile_builtin_method.
  // Exposed so a startup self-check (see culebra.h) can guard against
  // drift from the interpreter's builtin tables.
  static const std::unordered_set<std::string_view>& known_builtin_methods() {
    static const std::unordered_set<std::string_view> known = {
        "size",        "push",       "pop",      "reverse", "slice",
        "join",        "index_of",   "contains", "upper",   "lower",
        "trim",        "split",      "starts_with", "ends_with",
        "keys",        "has",        "remove",
        // Array eager + iterator lazy / terminal methods. Tag-dispatched
        // in compile_builtin_method — Array receivers keep the eager
        // path; iterator-protocol Objects route to the lazy runtime.
        "map",         "filter",     "reduce",   "for_each",
        "find",        "any",        "all",      "flat_map", "sort_by",
        "sum",         "product",    "min",      "max",
        "collect",     "count",      "take",     "skip",
        "take_while",  "chain",      "zip",      "enumerate",
        "code_points", "graphemes",  "iter"};
    return known;
  }
  // Stdlib helpers — bodies live in stdlib_jit.h.
  void declare_stdlib_runtime();
  llvm::Value* try_compile_stdlib_global(const std::string& name,
                                         const peg::Ast& argsAst,
                                         const peg::Ast& callAst);
  llvm::Value* try_compile_stdlib_namespace(std::string_view ns,
                                            std::string_view method,
                                            const peg::Ast& argsAst,
                                            const peg::Ast& callAst);
  llvm::Value* try_compile_stdlib_namespace_property(std::string_view ns,
                                                      std::string_view prop);
  llvm::Value* emit_output_call(const char* rt_name,
                                const peg::Ast& argsAst);

 private:
  llvm::LLVMContext& ctx_;
  llvm::Module* module_;
  llvm::IRBuilder<>& builder_;
  llvm::StructType* valueType_;    // {i8, i64}
  llvm::StructType* cellType_;     // {Value}
  llvm::StructType* closureType_;  // {ptr fn, i64 n, ptr captures}

  std::vector<Scope> scopes_;

  // Analysis results for each FUNCTION AST node (plus the main program)
  std::map<const peg::Ast*, FuncInfo> func_info_;
  FuncInfo main_info_;

  // Currently compiling function's info (to know which locals are cells)
  const FuncInfo* current_info_ = nullptr;

  // LLVM function-level state for the function currently being compiled
  llvm::Value* current_closure_arg_ = nullptr;  // __cls__ argument

  // Current AST position for error reporting
  size_t current_line_ = 0;
  size_t current_column_ = 0;

  // Debug mode flag
  bool debug_enabled_ = false;

  // Current function's declared return type (empty = unchecked). The token is
  // owned by the AST, which outlives the compilation, so string_view is safe.
  std::string_view current_return_type_;

  // Defer-stack mark recorded at the current function's entry. `return`
  // lowers `defer_run_to(this mark)` before emitting `ret` so the
  // function's own defers run regardless of where the return sits.
  llvm::Value* current_fn_defer_mark_ = nullptr;

  // Counter for generating unique function names
  int funcCounter_ = 0;

  JIT(llvm::LLVMContext* ctx, llvm::Module* mod, llvm::IRBuilder<>& builder)
      : ctx_(*ctx), module_(mod), builder_(builder) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    valueType_ = llvm::StructType::create(
        ctx_, {builder_.getInt8Ty(), builder_.getInt64Ty()}, "Value");
    // All refcounted types have i32 refcount as field 0.
    cellType_ = llvm::StructType::create(
        ctx_, {builder_.getInt64Ty(), valueType_}, "Cell");
    closureType_ = llvm::StructType::create(
        ctx_,
        {builder_.getInt64Ty(), ptrTy, builder_.getInt64Ty(), ptrTy,
         builder_.getInt64Ty()},
        "Closure");
  }

  // --- Scope management ---

  void push_scope() { scopes_.emplace_back(); }

  // Emit IR to release every owned binding in `scope` and zero the
  // underlying allocas so a subsequent re-entry (loop iteration,
  // recursive call) starts from a clean slate. Borrowed slots — the
  // function's `self` / `this` / params and capture cells — are
  // skipped: their refcounts belong to the caller or the enclosing
  // closure, not to the callee's frame.
  //
  // Releases in reverse declaration order (LIFO) so a later-declared
  // binding is destroyed before an earlier one it may reference.
  void release_scope_slots(const Scope& scope) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    for (auto it = scope.order.rbegin(); it != scope.order.rend(); ++it) {
      const auto& slot = (*it)->second;
      if (!slot.owned) continue;
      if (slot.kind == VarSlot::Stack) {
        auto val = builder_.CreateLoad(valueType_, slot.alloca);
        emit_value_release(val);
        builder_.CreateStore(
            llvm::ConstantAggregateZero::get(valueType_), slot.alloca);
      } else {
        auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca);
        emit_cell_release(cellPtr);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                             slot.alloca);
      }
    }
  }

  // Release every binding in every active scope. Used at function exit
  // (return / fall-through) so locals' refcounts reach zero in the
  // callee — this is what gives auto-drop interp-equivalent timing.
  // Throw / break / continue paths still leak their inner-scope slots
  // until the cycle collector runs (matches prior JIT behaviour).
  void release_all_scopes_for_exit() {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      release_scope_slots(*it);
    }
  }

  void pop_scope() {
    if (!builder_.GetInsertBlock()->getTerminator()) {
      release_scope_slots(scopes_.back());
    }
    scopes_.pop_back();
  }

  const VarSlot* lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->slots.find(name);
      if (found != it->slots.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  void define_var(const std::string& name, VarSlot slot) {
    auto& scope = scopes_.back();
    auto [it, inserted] = scope.slots.try_emplace(name, slot);
    if (inserted) {
      scope.order.push_back(it);
    } else {
      // Re-binding within the same scope keeps the original release
      // position (first-declared wins for LIFO order).
      it->second = slot;
    }
  }

  // Raw load (no retain) - for internal ownership transfer
  llvm::Value* load_slot_raw(const VarSlot& slot, const std::string& name) {
    if (slot.kind == VarSlot::Stack) {
      return builder_.CreateLoad(valueType_, slot.alloca, name);
    }
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca, name + ".cellp");
    auto valPtr = builder_.CreateStructGEP(cellType_, cellPtr, 1, name + ".vp");
    return builder_.CreateLoad(valueType_, valPtr, name);
  }

  // Load a value from a slot with retain (+1 for caller).
  llvm::Value* load_slot(const VarSlot& slot, const std::string& name) {
    auto val = load_slot_raw(slot, name);
    emit_value_retain(val);
    return val;
  }

  // Store a value into a slot (does NOT retain/release — caller's responsibility).
  void store_slot_raw(const VarSlot& slot, llvm::Value* val) {
    if (slot.kind == VarSlot::Stack) {
      builder_.CreateStore(val, slot.alloca);
      return;
    }
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto cellPtr = builder_.CreateLoad(ptrTy, slot.alloca);
    auto valPtr = builder_.CreateStructGEP(cellType_, cellPtr, 1);
    builder_.CreateStore(val, valPtr);
  }

  // Store with RC semantics: releases previous slot contents (after replacing).
  // Caller's `val` ownership is absorbed into the slot.
  void store_slot(const VarSlot& slot, llvm::Value* val) {
    auto old = load_slot_raw(slot, "old");
    store_slot_raw(slot, val);
    emit_value_release(old);
  }

  // Emit IR to retain a Value (no-op for non-refcounted tags; done in runtime).
  void emit_value_retain(llvm::Value* val) {
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    emit_call(
        module_->getOrInsertFunction(
            rt::value_retain, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {tag, data});
  }

  void emit_value_release(llvm::Value* val) {
    auto tag = extract_tag(val);
    auto data = extract_data(val);
    emit_call(
        module_->getOrInsertFunction(
            rt::value_release, builder_.getVoidTy(),
            builder_.getInt8Ty(), builder_.getInt64Ty()),
        {tag, data});
  }

  void emit_cell_retain(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_retain,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  void emit_cell_release(llvm::Value* cellPtr) {
    emit_call(
        module_->getOrInsertFunction(rt::cell_release,
                                     builder_.getVoidTy(),
                                     llvm::PointerType::get(ctx_, 0)),
        {cellPtr});
  }

  llvm::Value* current_line_val() {
    return builder_.getInt64(static_cast<int64_t>(current_line_));
  }
  llvm::Value* current_column_val() {
    return builder_.getInt64(static_cast<int64_t>(current_column_));
  }

  void emit_type_error() {
    emit_call(
        module_->getOrInsertFunction(rt::type_error,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {current_line_val(), current_column_val()});
  }

  void emit_div_zero() {
    emit_call(
        module_->getOrInsertFunction(rt::div_zero,
                                     builder_.getVoidTy(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {current_line_val(), current_column_val()});
  }

  void emit_type_check(llvm::Value* val, std::string_view expected_type,
                       std::string_view context) {
    if (expected_type.empty() || expected_type == "Any") return;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto tag = extract_tag(val);
    auto expPtr = get_or_create_global_str(expected_type, ".tycheck.exp");
    auto ctxPtr = get_or_create_global_str(context, ".tycheck.ctx");
    emit_call(
        module_->getOrInsertFunction(
            rt::type_check, builder_.getVoidTy(),
            builder_.getInt8Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {tag, expPtr, ctxPtr, current_line_val(), current_column_val()});
  }

  // Cache of compile-time string constants to avoid duplicate globals per
  // emission.
  std::unordered_map<std::string, llvm::Constant*> global_str_cache_;
  llvm::Constant* get_or_create_global_str(std::string_view s,
                                           const char* name_hint) {
    std::string key(s);
    auto it = global_str_cache_.find(key);
    if (it != global_str_cache_.end()) return it->second;
    auto* g = builder_.CreateGlobalString(key, name_hint);
    global_str_cache_.emplace(std::move(key), g);
    return g;
  }

  // Get the raw cell pointer from a Cell slot (for passing to closure).
  llvm::Value* cell_ptr_of(const VarSlot& slot) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return builder_.CreateLoad(ptrTy, slot.alloca, "cellp");
  }

  // --- Value helpers ---

  llvm::Value* make_nil() {
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_NIL), {0});
    val = builder_.CreateInsertValue(val, builder_.getInt64(0), {1});
    return val;
  }

  llvm::Value* make_bool(llvm::Value* b) {
    auto data = builder_.CreateZExt(b, builder_.getInt64Ty());
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_BOOL), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_long(llvm::Value* l) {
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_LONG), {0});
    val = builder_.CreateInsertValue(val, l, {1});
    return val;
  }

  // Float is stored bit-cast into the i64 payload. The layout of
  // JitValue ({ i8 tag, i64 data }) is unchanged; only the
  // interpretation of `data` differs when tag is TAG_FLOAT.
  llvm::Value* make_float(llvm::Value* d) {
    auto i64 = builder_.CreateBitCast(d, builder_.getInt64Ty(), "f.bits");
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_FLOAT), {0});
    val = builder_.CreateInsertValue(val, i64, {1});
    return val;
  }

  llvm::Value* make_func(llvm::Value* ptr) {
    auto data = builder_.CreatePtrToInt(ptr, builder_.getInt64Ty());
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_FUNC), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_string(llvm::Value* ptr) {
    auto data = builder_.CreatePtrToInt(ptr, builder_.getInt64Ty());
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_STRING), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_array(llvm::Value* ptr) {
    auto data = builder_.CreatePtrToInt(ptr, builder_.getInt64Ty());
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_ARRAY), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* make_object(llvm::Value* ptr) {
    auto data = builder_.CreatePtrToInt(ptr, builder_.getInt64Ty());
    llvm::Value* val = llvm::UndefValue::get(valueType_);
    val = builder_.CreateInsertValue(val, builder_.getInt8(TAG_OBJECT), {0});
    val = builder_.CreateInsertValue(val, data, {1});
    return val;
  }

  llvm::Value* extract_ptr(llvm::Value* v) {
    auto data = extract_data(v);
    return builder_.CreateIntToPtr(data, llvm::PointerType::get(ctx_, 0));
  }

  llvm::Value* extract_tag(llvm::Value* v) {
    return builder_.CreateExtractValue(v, {0}, "tag");
  }

  llvm::Value* extract_data(llvm::Value* v) {
    return builder_.CreateExtractValue(v, {1}, "data");
  }

  // value_to_bool: returns i1
  llvm::Value* value_to_bool(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto nilBB = llvm::BasicBlock::Create(ctx_, "tobool.nil", fn);
    auto boolBB = llvm::BasicBlock::Create(ctx_, "tobool.bool", fn);
    auto longBB = llvm::BasicBlock::Create(ctx_, "tobool.long", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "tobool.float", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tobool.error", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "tobool.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errorBB, 4);
    sw->addCase(builder_.getInt8(TAG_NIL), nilBB);
    sw->addCase(builder_.getInt8(TAG_BOOL), boolBB);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);
    sw->addCase(builder_.getInt8(TAG_FLOAT), floatBB);

    builder_.SetInsertPoint(nilBB);
    auto nilVal = builder_.getFalse();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(boolBB);
    auto boolVal =
        builder_.CreateICmpNE(data, builder_.getInt64(0), "bool.nz");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(longBB);
    auto longVal =
        builder_.CreateICmpNE(data, builder_.getInt64(0), "long.nz");
    builder_.CreateBr(mergeBB);

    // Float: 0.0 and -0.0 are false; NaN is true (Python's
    // bool(float('nan'))). FCmpUNE("unordered not-equal") returns
    // true when either operand is NaN *or* d != 0 — exactly the
    // NaN-is-truthy rule.
    builder_.SetInsertPoint(floatBB);
    auto asD = builder_.CreateBitCast(data, builder_.getDoubleTy(), "f.bits");
    auto zeroD = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
    auto floatVal = builder_.CreateFCmpUNE(asD, zeroD, "float.nz");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 4, "tobool");
    phi->addIncoming(nilVal, nilBB);
    phi->addIncoming(boolVal, boolBB);
    phi->addIncoming(longVal, longBB);
    phi->addIncoming(floatVal, floatBB);
    return phi;
  }

  // value_to_long: returns i64, calls type_error if not Long
  llvm::Value* value_to_long(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "tolong.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tolong.error", fn);

    auto isLong = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
    builder_.CreateCondBr(isLong, okBB, errorBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    return data;
  }

  // coerce_to_double: returns f64 from a Long (SIToFP) or Float
  // (bitcast). Any other tag raises `type error`. Used on every
  // Float-aware arithmetic and comparison slow path.
  llvm::Value* coerce_to_double(llvm::Value* v) {
    auto tag = extract_tag(v);
    auto data = extract_data(v);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto longBB = llvm::BasicBlock::Create(ctx_, "todouble.long", fn);
    auto floatBB = llvm::BasicBlock::Create(ctx_, "todouble.float", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "todouble.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "todouble.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errorBB, 2);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);
    sw->addCase(builder_.getInt8(TAG_FLOAT), floatBB);

    builder_.SetInsertPoint(longBB);
    auto fromLong =
        builder_.CreateSIToFP(data, builder_.getDoubleTy(), "l.sitofp");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(floatBB);
    auto fromFloat =
        builder_.CreateBitCast(data, builder_.getDoubleTy(), "f.bitcast");
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getDoubleTy(), 2, "num.d");
    phi->addIncoming(fromLong, longBB);
    phi->addIncoming(fromFloat, floatBB);
    return phi;
  }

  // --- Free variable analysis ---

  static bool is_builtin_var(const std::string& name) {
    static const std::unordered_set<std::string_view> names = {
        "puts",      "print",   "assert", "self", "this",
        "to_long",   "to_string", "type_of",
        "Math",      "IO",      "Sys"};
    return names.contains(name);
  }

  // Invoke `f(name, line, column)` for each identifier that a pattern
  // would bind if it matches.
  template <typename F>
  static void for_each_pattern_binding(const peg::Ast& pattern, F&& f) {
    using namespace peg::udl;
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      for (auto& sub : pattern.nodes) for_each_pattern_binding(*sub, f);
      return;
    }
    switch (pattern.tag) {
      case "IDENTIFIER"_:
        f(pattern.token, pattern.line, pattern.column);
        return;
      case "TYPED_IDENT"_: {
        auto& id = *pattern.nodes[0];
        f(id.token, id.line, id.column);
        return;
      }
      case "ARRAY_PATTERN"_:
        for (auto& e : pattern.nodes) for_each_pattern_binding(*e, f);
        return;
      case "REST_PATTERN"_: {
        auto& id = *pattern.nodes[0];
        f(id.token, id.line, id.column);
        return;
      }
      case "OBJECT_PATTERN"_:
        for (auto& key_node : pattern.nodes) {
          f(key_node->token, key_node->line, key_node->column);
        }
        return;
      default:
        return;
    }
  }

  // Throw if `name` appears in any closure-captured outer scope.
  // outer[0] is the top-level (main) scope; its entries behave like
  // globals and may be shadowed freely. outer[1..] are enclosing
  // function locals whose names would be captured by the current
  // function and thus must not be shadowed.
  static void check_shadow_against_captures(
      const std::string& name,
      const std::vector<const std::set<std::string>*>& outer,
      size_t line, size_t column) {
    for (size_t i = 1; i < outer.size(); i++) {
      if (outer[i]->contains(name)) throw_shadow_error(name, line, column);
    }
  }

  void check_pattern_shadow(
      const peg::Ast& pattern,
      const std::vector<const std::set<std::string>*>& outer) const {
    for_each_pattern_binding(
        pattern, [&](std::string_view name, size_t line, size_t col) {
          check_shadow_against_captures(std::string(name), outer, line, col);
        });
  }

  // Collect names introduced by `let x = ...` or by bare `x = ...` where x is
  // not in any outer scope (auto-local). Does not descend into nested
  // functions.
  void collect_fn_locals(
      const peg::Ast& node, std::set<std::string>& locals,
      const std::vector<const std::set<std::string>*>& outer) const {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_) return;

    if (node.tag == "MATCH"_) {
      // MATCH = [subject, MATCH_ARMS]; MATCH_ARM = [PATTERN, (GUARD)?, EXPR].
      // Register pattern-bound names as locals of the enclosing
      // function so that nested closures capturing them are handled
      // correctly by the free-variable analysis (the bindings are
      // then promoted to cells via `captured_locals`). Same mechanism
      // as TRY's catch binding below.
      for (auto& arm : node.nodes[1]->nodes) {
        check_pattern_shadow(*arm->nodes[0], outer);
        for_each_pattern_binding(
            *arm->nodes[0],
            [&](std::string_view name, size_t, size_t) {
              locals.insert(std::string(name));
            });
      }
      // fall through to normal recursive walk
    }

    if (node.tag == "TRY"_) {
      // TRY = [body_block, catch_ident, catch_body]. The catch binding
      // introduces a new local in the enclosing function; register it
      // so nested closures that capture it see it and get a cell.
      auto& id = *node.nodes[1];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      locals.insert(name);
      // fall through to walk the bodies
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body)]. The loop
      // binding is BLOCK-SCOPED (visible only within the body), so we
      // deliberately don't add it to the enclosing function's flat
      // `locals` set — otherwise functions defined OUTSIDE the for
      // body in the same enclosing function would wrongly see the
      // binding in their `outer` and treat references to a same-named
      // identifier as an outer-capture.
      //
      // The shadow check still applies: introducing the binding must
      // not collide with a closure-captured name from an enclosing
      // function. Subtree-local visibility is re-established in
      // visit_for_frees' FOR handler so nested closures inside the
      // body can still capture it correctly.
      auto& id = *node.nodes[0];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      collect_fn_locals(*node.nodes[1], locals, outer);
      collect_fn_locals(*node.nodes[2], locals, outer);
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto lvalcnt = static_cast<int>(node.nodes.size()) - 3;
      if (lvalcnt == 1) {
        auto ident_node = node.nodes[2];
        if (ident_node->tag == "IDENTIFIER"_) {
          auto name = std::string(ident_node->token);
          bool is_let = (node.nodes[0]->token == "let");
          bool is_mut = (node.nodes[1]->token == "mut");
          bool is_declare = is_let || is_mut;

          if (is_declare) {
            check_shadow_against_captures(name, outer, ident_node->line,
                                          ident_node->column);
            locals.insert(name);
          } else {
            // Bare assignment: local only if not in outer
            bool in_outer = false;
            for (auto* s : outer) {
              if (s->contains(name)) {
                in_outer = true;
                break;
              }
            }
            if (!in_outer) {
              locals.insert(name);
            }
          }
        }
      }
      collect_fn_locals(*node.nodes.back(), locals, outer);
      return;
    }

    for (auto& c : node.nodes) {
      collect_fn_locals(*c, locals, outer);
    }
  }

  void visit_for_frees(const peg::Ast& node,
                       const std::set<std::string>& my_locals,
                       std::vector<const std::set<std::string>*>& outer,
                       FuncInfo& info) {
    using namespace peg::udl;

    if (node.tag == "FUNCTION"_ || node.tag == "DEFER"_) {
      // Analyze nested function / defer body; its locals/frees don't
      // leak into the enclosing scope, but the enclosing scope owns any
      // captured vars (cells) that it references.
      outer.push_back(&my_locals);
      auto nested_info = (node.tag == "FUNCTION"_)
                             ? analyze_function(node, outer)
                             : analyze_defer(node, outer);
      outer.pop_back();
      for (const auto& fv : nested_info.free_vars) {
        if (my_locals.contains(fv)) {
          info.captured_locals.insert(fv);
        } else {
          if (std::find(info.free_vars.begin(), info.free_vars.end(), fv) ==
              info.free_vars.end()) {
            info.free_vars.push_back(fv);
          }
        }
      }
      return;
    }

    // DOT[IDENTIFIER] is a property name, not a variable reference.
    // The AST optimizer collapses the single-child rule so node.tag
    // reads as IDENTIFIER; use original_tag and check before the
    // IDENTIFIER handler below.
    if (node.original_tag == "DOT"_) {
      return;
    }

    if (node.tag == "IDENTIFIER"_) {
      auto name = std::string(node.token);
      if (my_locals.contains(name) || is_builtin_var(name)) return;
      // Check if in any outer scope — if so, it's a free var
      bool in_outer = false;
      for (auto* scope : outer) {
        if (scope->contains(name)) {
          in_outer = true;
          break;
        }
      }
      if (in_outer) {
        if (std::find(info.free_vars.begin(), info.free_vars.end(), name) ==
            info.free_vars.end()) {
          info.free_vars.push_back(name);
        }
      }
      // else: unresolved (will error at runtime) — don't add as free
      return;
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body)]. The
      // binding is block-scoped to the body — make it visible only
      // while walking the body (by extending `my_locals` for that
      // subtree) so nested closures inside the body can capture it
      // while closures outside the body don't see it. Also register
      // the binding in `info.captured_locals` if any nested closure
      // references it — that flag is what triggers cell promotion at
      // emit_for_body_iteration time.
      visit_for_frees(*node.nodes[1], my_locals, outer, info);
      auto extended = my_locals;
      auto name = std::string(node.nodes[0]->token);
      extended.insert(name);
      visit_for_frees(*node.nodes[2], extended, outer, info);
      // If the body walk pulled `name` into the enclosing function's
      // free-vars (because a nested closure referenced it), we instead
      // mark it captured here and drop it from the free list — the
      // enclosing function owns it.
      auto it = std::find(info.free_vars.begin(), info.free_vars.end(),
                          name);
      if (it != info.free_vars.end()) {
        info.captured_locals.insert(name);
        info.free_vars.erase(it);
      }
      return;
    }

    if (node.tag == "OBJECT_PROPERTY"_) {
      // [MUTABLE, IDENTIFIER(key), EXPRESSION]
      // Only visit the value; key is not a variable reference.
      if (node.nodes.size() == 3) {
        visit_for_frees(*node.nodes[2], my_locals, outer, info);
      }
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto lvalcnt = static_cast<int>(node.nodes.size()) - 3;
      if (lvalcnt == 1) {
        // Simple target: `x = expr` or `let x = expr`
        auto ident_node = node.nodes[2];
        if (ident_node->tag == "IDENTIFIER"_) {
          auto name = std::string(ident_node->token);
          bool is_let = (node.nodes[0]->token == "let");
          // If not let and x is not my local and not builtin, treat as
          // a reference to outer (visit as identifier to capture as free).
          if (!is_let && !my_locals.contains(name) &&
              !is_builtin_var(name)) {
            visit_for_frees(*ident_node, my_locals, outer, info);
          }
        }
      } else {
        // Complex lvalue: primary + postfixes
        visit_for_frees(*node.nodes[2], my_locals, outer, info);
        for (int i = 3; i < static_cast<int>(node.nodes.size()) - 1; i++) {
          visit_for_frees(*node.nodes[i], my_locals, outer, info);
        }
      }
      visit_for_frees(*node.nodes.back(), my_locals, outer, info);
      return;
    }

    for (auto& c : node.nodes) {
      visit_for_frees(*c, my_locals, outer, info);
    }
  }

  // Walk this function's body populating `info.has_eh`,
  // `info.has_fn_defer`, and `scope_has_defer_`. Returns true iff the
  // subtree contains a DEFER at the caller's own scope level — used so
  // an enclosing LEXICAL_SCOPE can mark itself without rescanning.
  // Recursion stops at nested FUNCTION / DEFER boundaries (those own
  // their own defers, analyzed separately). `at_fn_top` is true only
  // while still at the function's top level (not inside any `{}`).
  bool scan_eh_defer(const peg::Ast& node, bool at_fn_top, FuncInfo& info) {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_) return false;
    if (node.tag == "DEFER"_) {
      if (at_fn_top) info.has_fn_defer = true;
      return true;
    }
    if (node.tag == "TRY"_) {
      info.has_eh = true;
      bool any = false;
      for (auto& c : node.nodes) any |= scan_eh_defer(*c, at_fn_top, info);
      return any;
    }
    if (node.tag == "LEXICAL_SCOPE"_) {
      bool inner = false;
      for (auto& c : node.nodes) inner |= scan_eh_defer(*c, false, info);
      if (inner) {
        scope_has_defer_.insert(&node);
        info.has_eh = true;
      }
      return false;  // this scope absorbs its own defers
    }
    bool any = false;
    for (auto& c : node.nodes) any |= scan_eh_defer(*c, at_fn_top, info);
    return any;
  }

  FuncInfo analyze_function(
      const peg::Ast& fnAst,
      std::vector<const std::set<std::string>*>& outer) {
    std::set<std::string> my_locals;
    for (auto& p : fnAst.nodes[0]->nodes) {
      auto& id = *p->nodes[1];
      auto name = std::string(id.token);
      check_shadow_against_captures(name, outer, id.line, id.column);
      my_locals.insert(name);
    }
    collect_fn_locals(*fnAst.nodes[1], my_locals, outer);

    FuncInfo info;
    visit_for_frees(*fnAst.nodes[1], my_locals, outer, info);
    scan_eh_defer(*fnAst.nodes[1], true, info);

    func_info_[&fnAst] = info;
    return info;
  }

  // `defer { BODY }` behaves like a 0-parameter nested function that
  // closes over the enclosing scope. Shadow checks are unneeded (no
  // params, no let/mut at the defer line itself).
  FuncInfo analyze_defer(
      const peg::Ast& deferAst,
      std::vector<const std::set<std::string>*>& outer) {
    std::set<std::string> my_locals;
    collect_fn_locals(*deferAst.nodes[0], my_locals, outer);

    FuncInfo info;
    visit_for_frees(*deferAst.nodes[0], my_locals, outer, info);
    scan_eh_defer(*deferAst.nodes[0], true, info);

    func_info_[&deferAst] = info;
    return info;
  }

  // One JIT instance per compilation. `func_info_` and
  // `scope_has_defer_` are keyed by `peg::Ast*` and never cleared —
  // only the `JIT::run`-local instance keeps pointer stability.
  // Reusing an instance across compilations would collide on reused
  // node addresses; the assert below guards future misuse.
  FuncInfo analyze_program(const peg::Ast& programAst) {
    assert(func_info_.empty() && scope_has_defer_.empty() &&
           "JIT reused — analyze_program expects a fresh instance");
    std::vector<const std::set<std::string>*> outer;
    std::set<std::string> my_locals;
    collect_fn_locals(programAst, my_locals, outer);

    FuncInfo info;
    visit_for_frees(programAst, my_locals, outer, info);
    scan_eh_defer(programAst, true, info);
    return info;
  }

  // --- Runtime function declarations ---

  void declare_runtime_functions() {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    module_->getOrInsertFunction(rt::puts, builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::assert_, builder_.getVoidTy(), builder_.getInt8Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::type_error,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // User-level throw: stashes tag+data in globals and raises a
    // C++ exception; try/catch landingpads read the globals back.
    module_->getOrInsertFunction(rt::throw_,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::rethrow,
                                 builder_.getVoidTy());
    // Defer: (push takes tag+data of a closure Value)
    module_->getOrInsertFunction(rt::defer_mark,
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_push,
                                 builder_.getVoidTy(), builder_.getInt8Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::defer_run_to,
                                 builder_.getVoidTy(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::type_check, builder_.getVoidTy(),
        builder_.getInt8Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
        builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::div_zero,
                                 builder_.getVoidTy(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::debugger_break,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_to_display, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::str_concat, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_eq,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_cmp,
                                 builder_.getInt32Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_new, ptrTy);
    module_->getOrInsertFunction(rt::array_push,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_resize, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_get,
                                 builder_.getVoidTy(), ptrTy,
                                 builder_.getInt64Ty(), ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_set, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(
        rt::array_set_or_push, builder_.getVoidTy(), ptrTy,
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_new, ptrTy);
    module_->getOrInsertFunction(
        rt::object_set, builder_.getVoidTy(), ptrTy, ptrTy,
        builder_.getInt1Ty(), builder_.getInt8Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::object_get,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::object_has,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::closure_new, ptrTy, ptrTy,
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_retain,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::value_release,
                                 builder_.getVoidTy(),
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::cell_retain,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::cell_release,
                                 builder_.getVoidTy(), ptrTy);

    // Built-in type methods.
    module_->getOrInsertFunction(rt::str_size,
                                 builder_.getInt64Ty(), ptrTy);
    module_->getOrInsertFunction(rt::str_upper, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_lower, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_trim, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_split, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::str_contains,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_starts_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_ends_with,
                                 builder_.getInt1Ty(), ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::str_slice, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_pop,
                                 builder_.getVoidTy(), ptrTy, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::array_slice2, ptrTy, ptrTy,
                                 builder_.getInt64Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_join, ptrTy, ptrTy,
                                 ptrTy);
    module_->getOrInsertFunction(rt::array_contains,
                                 builder_.getInt1Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_index_of,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_reverse,
                                 builder_.getVoidTy(), ptrTy);
    module_->getOrInsertFunction(rt::object_keys, ptrTy, ptrTy);
    module_->getOrInsertFunction(rt::object_remove,
                                 builder_.getVoidTy(), ptrTy, ptrTy);
    // Higher-order array helpers (§17.2): (arr, fn_tag, fn_data, line, col).
    module_->getOrInsertFunction(rt::array_map, ptrTy, ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_filter, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_for_each, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty());
    module_->getOrInsertFunction(
        rt::array_reduce, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt8Ty(),
        builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        ptrTy, ptrTy);
    // find (arr, fn_tag, fn_data, line, col, out_tag, out_data)
    module_->getOrInsertFunction(
        rt::array_find, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty(), ptrTy, ptrTy);
    // any/all (arr, fn_tag, fn_data, line, col) -> i64 (0/1)
    module_->getOrInsertFunction(rt::array_any,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_all,
                                 builder_.getInt64Ty(), ptrTy,
                                 builder_.getInt8Ty(), builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // flat_map (arr, fn_tag, fn_data, line, col) -> ptr
    module_->getOrInsertFunction(rt::array_flat_map, ptrTy,
                                 ptrTy, builder_.getInt8Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    // sort_by (arr, fn_tag, fn_data, line, col)
    module_->getOrInsertFunction(
        rt::array_sort_by, builder_.getVoidTy(), ptrTy,
        builder_.getInt8Ty(), builder_.getInt64Ty(), builder_.getInt64Ty(),
        builder_.getInt64Ty());
    // sum/product/min/max: (arr, line, col) -> i64
    module_->getOrInsertFunction(rt::array_sum, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_product, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_min, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());
    module_->getOrInsertFunction(rt::array_max, builder_.getInt64Ty(),
                                 ptrTy, builder_.getInt64Ty(),
                                 builder_.getInt64Ty());

    declare_stdlib_runtime();
  }

  // --- Main dispatch ---

  llvm::Value* compile(const peg::Ast& ast) {
    using namespace peg::udl;

    // Track position for runtime error messages
    if (ast.line) current_line_ = ast.line;
    if (ast.column) current_column_ = ast.column;

    switch (ast.tag) {
      case "STATEMENTS"_:
        return compile_statements(ast);
      case "WHILE"_:
        return compile_while(ast);
      case "FOR"_:
        return compile_for(ast);
      case "BREAK"_:
        return compile_break(ast);
      case "CONTINUE"_:
        return compile_continue(ast);
      case "IF"_:
        return compile_if(ast);
      case "MATCH"_:
        return compile_match(ast);
      case "FUNCTION"_:
        return compile_function(ast);
      case "CALL"_:
        return compile_call_with_builtins(ast);
      case "LEXICAL_SCOPE"_:
        return compile_lexical_scope(ast);
      case "ASSIGNMENT"_:
        return compile_assignment(ast);
      case "LOGICAL_OR"_:
        return compile_logical_or(ast);
      case "LOGICAL_AND"_:
        return compile_logical_and(ast);
      case "CONDITION"_:
        return compile_condition(ast);
      case "UNARY_PLUS"_:
        return compile_unary_plus(ast);
      case "UNARY_MINUS"_:
        return compile_unary_minus(ast);
      case "UNARY_NOT"_:
        return compile_unary_not(ast);
      case "ADDITIVE"_:
        return compile_additive(ast);
      case "MULTIPLICATIVE"_:
        return compile_multiplicative(ast);
      case "POWER"_:
        return compile_power(ast);
      case "IDENTIFIER"_:
        return compile_identifier(ast);
      case "NUMBER"_:
        return compile_number(ast);
      case "FLOAT"_:
        return compile_float(ast);
      case "BOOLEAN"_:
        return compile_bool(ast);
      case "NIL"_:
        return make_nil();
      case "STRING"_:
      case "INTERPOLATED_CONTENT"_:
        return compile_string(ast);
      case "INTERPOLATED_STRING"_:
        return compile_interpolated_string(ast);
      case "ARRAY"_:
        return compile_array(ast);
      case "OBJECT"_:
        return compile_object(ast);
      case "RETURN"_:
        return compile_return(ast);
      case "DEBUGGER"_:
        return compile_debugger(ast);
      case "THROW"_:
        return compile_throw(ast);
      case "TRY"_:
        return compile_try(ast);
      case "DEFER"_:
        return compile_defer(ast);
    }

    // Token (e.g., operator string)
    if (ast.is_token) {
      return nullptr;  // handled by parent
    }

    // Single child: recurse
    if (ast.nodes.size() == 1) {
      return compile(*ast.nodes[0]);
    }

    return make_nil();
  }

  // --- Statements ---

  llvm::Value* compile_statements(const peg::Ast& ast) {
    if (ast.is_token) {
      return compile(ast);
    }
    if (ast.nodes.empty()) {
      return make_nil();
    }
    llvm::Value* val = nullptr;
    for (auto& node : ast.nodes) {
      if (builder_.GetInsertBlock()->getTerminator()) break;
      // Release previous statement's result (not used)
      if (val) emit_value_release(val);
      val = compile(*node);
    }
    return val ? val : make_nil();
  }

  // --- Literals ---

  llvm::Value* compile_number(const peg::Ast& ast) {
    auto n = ast.token_to_number<long>();
    return make_long(builder_.getInt64(n));
  }

  llvm::Value* compile_float(const peg::Ast& ast) {
    auto d = ast.token_to_number<double>();
    auto constD = llvm::ConstantFP::get(builder_.getDoubleTy(), d);
    return make_float(constD);
  }

  llvm::Value* compile_bool(const peg::Ast& ast) {
    auto b = (ast.token == "true");
    return make_bool(builder_.getInt1(b));
  }

  llvm::Value* compile_string(const peg::Ast& ast) {
    auto str = std::string(ast.token);
    auto global = builder_.CreateGlobalString(str, ".str");
    return make_string(global);
  }

  llvm::Value* compile_array(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Create empty array
    auto arrPtr = emit_call(
        module_->getOrInsertFunction(rt::array_new, ptrTy),
        {}, "arr");

    // Resize first if size/default specified (matches interpreter order)
    if (ast.nodes.size() >= 2) {
      auto countVal = compile(*ast.nodes[1]);
      auto count = value_to_long(countVal);
      llvm::Value* defTag = builder_.getInt8(TAG_NIL);
      llvm::Value* defData = builder_.getInt64(0);
      if (ast.nodes.size() >= 3) {
        auto defVal = compile(*ast.nodes[2]);
        defTag = extract_tag(defVal);
        defData = extract_data(defVal);
      }
      emit_call(
          module_->getOrInsertFunction(
              rt::array_resize, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty()),
          {arrPtr, count, defTag, defData});
    }

    // Overwrite or push literal values (matches interpreter:
    // literals go into positions 0..n-1, extending if needed)
    const auto& seqNodes = ast.nodes[0]->nodes;
    for (auto i = 0u; i < seqNodes.size(); i++) {
      auto val = compile(*seqNodes[i]);
      auto tag = extract_tag(val);
      auto data = extract_data(val);
      emit_call(
          module_->getOrInsertFunction(
              rt::array_set_or_push, builder_.getVoidTy(),
              ptrTy, builder_.getInt64Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty()),
          {arrPtr, builder_.getInt64(i), tag, data});
    }

    return make_array(arrPtr);
  }

  // Object property bind. The runtime well-known check (drop/iter/
  // next must be 0-arg Function) is emitted only when the literal
  // name is one of the three — keeping the hot path for ordinary
  // literal properties free of any name comparison at runtime.
  void emit_object_set(llvm::Value* objPtr, const std::string& name,
                       bool mut, llvm::Value* tag, llvm::Value* data) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    if (is_well_known_prop(name)) {
      auto wkKey = get_or_create_global_str(name, ".wkkey");
      emit_call(
          module_->getOrInsertFunction(rt::check_well_known_prop,
                                       builder_.getVoidTy(), ptrTy,
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty()),
          {wkKey, tag, data});
    }
    auto keyPtr = get_or_create_global_str(name, ".key");
    emit_call(
        module_->getOrInsertFunction(
            rt::object_set, builder_.getVoidTy(), ptrTy,
            ptrTy, builder_.getInt1Ty(), builder_.getInt8Ty(),
            builder_.getInt64Ty(), builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {objPtr, keyPtr, builder_.getInt1(mut), tag, data,
         current_line_val(), current_column_val()});
  }

  llvm::Value* emit_object_has(llvm::Value* objPtr, llvm::Value* keyPtr,
                               const llvm::Twine& name = "has.prop") {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    return emit_call(
        module_->getOrInsertFunction(rt::object_has, builder_.getInt1Ty(),
                                     ptrTy, ptrTy),
        {objPtr, keyPtr}, name);
  }

  llvm::Value* compile_object(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto objPtr = emit_call(
        module_->getOrInsertFunction(rt::object_new, ptrTy),
        {}, "obj");

    // Each child is OBJECT_PROPERTY: [MUTABLE, IDENTIFIER, EXPRESSION]
    for (auto& prop : ast.nodes) {
      bool mut = (prop->nodes[0]->token == "mut");
      auto name = std::string(prop->nodes[1]->token);
      auto val = compile(*prop->nodes[2]);
      emit_object_set(objPtr, name, mut, extract_tag(val), extract_data(val));
    }

    return make_object(objPtr);
  }

  llvm::Value* compile_interpolated_string(const peg::Ast& ast) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Start with empty string
    llvm::Value* result = builder_.CreateGlobalString("", ".str.empty");

    for (auto& node : ast.nodes) {
      llvm::Value* piece;
      if (node->tag == "INTERPOLATED_CONTENT"_) {
        // Raw text between expressions
        piece = builder_.CreateGlobalString(std::string(node->token), ".str");
      } else {
        // Expression - evaluate and convert to display string
        auto val = compile(*node);
        auto tag = extract_tag(val);
        auto data = extract_data(val);
        piece = emit_call(
            module_->getOrInsertFunction(rt::value_to_display,
                                         ptrTy, builder_.getInt8Ty(),
                                         builder_.getInt64Ty()),
            {tag, data}, "disp");
      }
      result = emit_call(
          module_->getOrInsertFunction(rt::str_concat, ptrTy,
                                       ptrTy, ptrTy),
          {result, piece}, "concat");
    }

    return make_string(result);
  }

  // --- Identifier ---

  llvm::Value* compile_identifier(const peg::Ast& ast) {
    auto name = std::string(ast.token);
    auto slot = lookup_var(name);
    if (!slot) {
      throw std::runtime_error(
          std::format("undefined variable '{}'...", name));
    }
    return load_slot(*slot, name);
  }

  // Create an alloca + cell for a new captured variable.
  // Alloca is pre-initialized to null in entry block for safe release on
  // re-execution (e.g., let inside a loop body).
  VarSlot make_cell_slot(const std::string& name, llvm::Value* initValue) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto cellSlotAlloca = entryB.CreateAlloca(ptrTy, nullptr, name);
    entryB.CreateStore(llvm::ConstantPointerNull::get(ptrTy),
                       cellSlotAlloca);

    // At declaration point: release old cell (null on first run), create new.
    auto oldCell = builder_.CreateLoad(ptrTy, cellSlotAlloca, name + ".old");
    emit_cell_release(oldCell);
    auto tag = extract_tag(initValue);
    auto data = extract_data(initValue);
    auto cellPtr = emit_call(
        module_->getOrInsertFunction(rt::cell_new, ptrTy,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {tag, data}, "cell");
    builder_.CreateStore(cellPtr, cellSlotAlloca);
    return VarSlot{VarSlot::Cell, cellSlotAlloca, /*owned=*/true};
  }

  VarSlot make_stack_slot(const std::string& name, llvm::Value* initValue) {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto alloca = entryB.CreateAlloca(valueType_, nullptr, name);
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_), alloca);
    // At declaration point: use store_slot (releases old = nil first run, else
    // previous iteration's value).
    VarSlot slot{VarSlot::Stack, alloca, /*owned=*/true};
    store_slot(slot, initValue);
    return slot;
  }

  // --- Assignment ---

  llvm::Value* compile_assignment(const peg::Ast& ast) {
    using namespace peg::udl;
    auto lvaloff = 2;
    auto lvalcnt = static_cast<int>(ast.nodes.size()) - 3;

    auto type_name = extract_type_annotation(ast, ast.nodes.size() - 2);
    if (!type_name.empty()) lvalcnt--;

    auto let = ast.nodes[0]->token == "let";
    auto mut = ast.nodes[1]->token == "mut";
    auto rval = compile(*ast.nodes.back());

    if (!type_name.empty()) {
      emit_type_check(rval, type_name, "assignment");
    }

    if (lvalcnt == 1) {
      auto name = std::string(ast.nodes[lvaloff]->token);

      // Check if the variable already exists in the current (innermost) scope.
      // If so, reuse the slot (avoid alloca accumulation in loops with `let`).
      if (!scopes_.empty()) {
        auto& slots = scopes_.back().slots;
        auto it = slots.find(name);
        if (it != slots.end()) {
          store_slot(it->second, rval);
          emit_value_retain(rval);
          return rval;
        }
      }

      if (!let) {
        auto existing = lookup_var(name);
        if (existing) {
          store_slot(*existing, rval);
          emit_value_retain(rval);
          return rval;
        }
      }

      // New variable: decide cell vs stack based on whether a nested function
      // captures it.
      bool captured = current_info_ &&
                      current_info_->captured_locals.contains(name);
      auto slot = captured ? make_cell_slot(name, rval)
                           : make_stack_slot(name, rval);
      define_var(name, slot);
      emit_value_retain(rval);
      return rval;
    }

    using namespace peg::udl;

    // Complex lvalue (obj.prop, arr[idx])
    llvm::Value* lval = compile(*ast.nodes[lvaloff]);

    // Process intermediate postfixes (all but the last)
    auto end = lvaloff + lvalcnt - 1;
    for (auto i = lvaloff + 1; i < end; i++) {
      const auto& postfix = *ast.nodes[i];
      switch (postfix.original_tag) {
        case "INDEX"_:
          lval = compile_index_access(postfix, lval);
          break;
        case "ARGUMENTS"_:
          lval = compile_function_call(postfix, lval);
          break;
        default:
          throw std::runtime_error(
              "complex lvalue path not supported in JIT");
      }
    }

    // Final postfix - do the assignment
    const auto& finalPostfix = *ast.nodes[end];
    switch (finalPostfix.original_tag) {
      case "INDEX"_: {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        // Type check: Array
        auto tag = extract_tag(lval);
        auto isArr =
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
        auto fn = builder_.GetInsertBlock()->getParent();
        auto okBB = llvm::BasicBlock::Create(ctx_, "set.ok", fn);
        auto errBB = llvm::BasicBlock::Create(ctx_, "set.err", fn);
        builder_.CreateCondBr(isArr, okBB, errBB);

        builder_.SetInsertPoint(errBB);
        emit_type_error();
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(okBB);
        auto arrPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
        auto idxVal = compile(finalPostfix);
        auto idx = value_to_long(idxVal);
        auto rtag = extract_tag(rval);
        auto rdata = extract_data(rval);
        emit_call(
            module_->getOrInsertFunction(
                rt::array_set, builder_.getVoidTy(), ptrTy,
                builder_.getInt64Ty(), builder_.getInt8Ty(),
                builder_.getInt64Ty(), builder_.getInt64Ty(),
                builder_.getInt64Ty()),
            {arrPtr, idx, rtag, rdata, current_line_val(),
             current_column_val()});
        emit_value_release(lval);  // release the lvalue's ref
        emit_value_retain(rval);
        return rval;
      }
      case "DOT"_: {
        auto ptrTy = llvm::PointerType::get(ctx_, 0);
        auto tag = extract_tag(lval);
        auto isObj =
            builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
        auto fn = builder_.GetInsertBlock()->getParent();
        auto okBB = llvm::BasicBlock::Create(ctx_, "propset.ok", fn);
        auto errBB = llvm::BasicBlock::Create(ctx_, "propset.err", fn);
        builder_.CreateCondBr(isObj, okBB, errBB);

        builder_.SetInsertPoint(errBB);
        emit_type_error();
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(okBB);
        auto objPtr = builder_.CreateIntToPtr(extract_data(lval), ptrTy);
        auto name = std::string(finalPostfix.token);
        emit_object_set(objPtr, name, mut, extract_tag(rval),
                        extract_data(rval));
        emit_value_release(lval);  // release the lvalue's ref
        emit_value_retain(rval);
        return rval;
      }
      default:
        throw std::runtime_error("invalid lvalue postfix");
    }
  }

  // --- Arithmetic ---
  //
  // Float-aware binary arithmetic uses a 3-way dispatch:
  //   1. If both operands are Long, take the inline i64 fast path
  //      (CreateAdd / CreateMul / CreateSDiv / CreateSRem).
  //   2. Otherwise, call a runtime helper that promotes each operand
  //      to double and runs the operation.
  // The branch predictor learns Long-only code paths immediately, so
  // scripts that stay on Long pay essentially no runtime cost.

  // If both operands are TAG_LONG, run `long_path` inline on their i64
  // payloads; otherwise invoke the runtime helper identified by
  // `rt_name`. Kept as a template so the caller's lambda is inlined
  // (no std::function heap allocation per arithmetic AST node).
  template <class LongPath>
  llvm::Value* emit_binop_dispatch(llvm::Value* lhs, llvm::Value* rhs,
                                   const char* rt_name, LongPath long_path) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "binop.int", fn);
    auto numBB = llvm::BasicBlock::Create(ctx_, "binop.num", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "binop.merge", fn);

    auto ltag = extract_tag(lhs);
    auto rtag = extract_tag(rhs);
    auto ldata = extract_data(lhs);
    auto rdata = extract_data(rhs);
    auto longTag = builder_.getInt8(TAG_LONG);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "both.long");
    builder_.CreateCondBr(bothLong, intBB, numBB);

    builder_.SetInsertPoint(intBB);
    auto intResult = long_path(ldata, rdata);
    auto intEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(numBB);
    auto numResult = emit_call(
        module_->getOrInsertFunction(rt_name, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {ltag, ldata, rtag, rdata}, "num.op");
    auto numEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "binop.r");
    phi->addIncoming(intResult, intEndBB);
    phi->addIncoming(numResult, numEndBB);
    return phi;
  }

  llvm::Value* compile_additive(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);
      auto ope = ast.nodes[i]->token[0];
      const char* rt_name = (ope == '+') ? rt::num_add
                           : (ope == '-') ? rt::num_sub
                                          : nullptr;
      if (!rt_name) throw std::runtime_error("invalid additive operator");
      lhs = emit_binop_dispatch(
          lhs, rhs, rt_name,
          [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
            auto r = (ope == '+') ? builder_.CreateAdd(ld, rd, "add")
                                  : builder_.CreateSub(ld, rd, "sub");
            return make_long(r);
          });
    }
    return lhs;
  }

  llvm::Value* compile_multiplicative(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);
      auto ope = ast.nodes[i]->token[0];

      const char* rt_name = nullptr;
      switch (ope) {
        case '*': rt_name = rt::num_mul; break;
        case '/': rt_name = rt::num_div; break;
        case '%': rt_name = rt::num_mod; break;
        default:
          throw std::runtime_error("invalid multiplicative operator");
      }

      lhs = emit_binop_dispatch(
          lhs, rhs, rt_name,
          [&](llvm::Value* ld, llvm::Value* rd) -> llvm::Value* {
            if (ope == '*') {
              return make_long(builder_.CreateMul(ld, rd, "mul"));
            }
            // `/` or `%` on two Longs: zero-check then SDiv/SRem.
            auto fn = builder_.GetInsertBlock()->getParent();
            auto isZero =
                builder_.CreateICmpEQ(rd, builder_.getInt64(0), "iszero");
            auto zeroBB = llvm::BasicBlock::Create(
                ctx_, ope == '/' ? "div.zero" : "mod.zero", fn);
            auto okBB = llvm::BasicBlock::Create(
                ctx_, ope == '/' ? "div.ok" : "mod.ok", fn);
            builder_.CreateCondBr(isZero, zeroBB, okBB);

            builder_.SetInsertPoint(zeroBB);
            emit_div_zero();
            builder_.CreateUnreachable();

            builder_.SetInsertPoint(okBB);
            auto r = (ope == '/') ? builder_.CreateSDiv(ld, rd, "div")
                                  : builder_.CreateSRem(ld, rd, "mod");
            return make_long(r);
          });
    }
    return lhs;
  }

  // --- Unary ---

  llvm::Value* compile_unary_plus(const peg::Ast& ast) {
    return compile(*ast.nodes[1]);
  }

  llvm::Value* compile_unary_minus(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto tag = extract_tag(val);
    auto longTag = builder_.getInt8(TAG_LONG);
    auto isLong = builder_.CreateICmpEQ(tag, longTag);

    auto fn = builder_.GetInsertBlock()->getParent();
    auto intBB = llvm::BasicBlock::Create(ctx_, "neg.int", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "neg.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "neg.merge", fn);
    builder_.CreateCondBr(isLong, intBB, slowBB);

    builder_.SetInsertPoint(intBB);
    auto intResult = make_long(builder_.CreateNeg(extract_data(val), "neg"));
    auto intEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(slowBB);
    auto slowResult = emit_call(
        module_->getOrInsertFunction(rt::num_neg, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {extract_tag(val), extract_data(val)}, "neg.num");
    auto slowEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "neg.r");
    phi->addIncoming(intResult, intEndBB);
    phi->addIncoming(slowResult, slowEndBB);
    return phi;
  }

  llvm::Value* compile_unary_not(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto b = value_to_bool(val);
    auto notb = builder_.CreateNot(b, "not");
    return make_bool(notb);
  }

  // --- Exponentiation ---

  // If `ast` is a compile-time numeric literal (possibly wrapped in a
  // single unary minus), extract its value. Returns nullopt otherwise.
  struct NumLit { bool is_float; double d; long l; };
  std::optional<NumLit> try_numeric_literal(const peg::Ast& ast) {
    using namespace peg::udl;
    if (ast.original_tag == "UNARY_MINUS"_ && ast.nodes.size() == 2) {
      auto inner = try_numeric_literal(*ast.nodes[1]);
      if (!inner) return std::nullopt;
      if (inner->is_float) return NumLit{true, -inner->d, 0};
      return NumLit{false, 0.0, -inner->l};
    }
    if (ast.tag == "NUMBER"_) {
      return NumLit{false, 0.0, ast.token_to_number<long>()};
    }
    if (ast.tag == "FLOAT"_) {
      return NumLit{true, ast.token_to_number<double>(), 0};
    }
    return std::nullopt;
  }

  llvm::Value* compile_power(const peg::Ast& ast) {
    // POWER = [base, POWER_OPERATOR, exponent]; optimizer strips the
    // node entirely when there's no `**`.
    auto base = compile(*ast.nodes[0]);

    // Peephole specialization for compile-time-constant exponents.
    // Correctness note: sqrt(x) and std::pow(x, 0.5) agree on NaN
    // behavior for negative x, so the 0.5 case is safe to inline.
    if (auto lit = try_numeric_literal(*ast.nodes[2])) {
      if (!lit->is_float && lit->l == 2) {
        return emit_binop_dispatch(
            base, base, rt::num_mul,
            [&](llvm::Value* ld, llvm::Value* rd) {
              return make_long(builder_.CreateMul(ld, rd, "mul"));
            });
      }
      if (lit->is_float && lit->d == 0.5) {
        auto d = coerce_to_double(base);
        auto sqrtFn = llvm::Intrinsic::getOrInsertDeclaration(
            module_, llvm::Intrinsic::sqrt, {builder_.getDoubleTy()});
        auto result = builder_.CreateCall(sqrtFn, {d}, "sqrt");
        return make_float(result);
      }
      if ((!lit->is_float && lit->l == 1) ||
          (lit->is_float && lit->d == 1.0)) {
        return base;
      }
    }

    auto exp = compile(*ast.nodes[2]);
    return emit_call(
        module_->getOrInsertFunction(rt::num_pow, valueType_,
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {extract_tag(base), extract_data(base),
         extract_tag(exp), extract_data(exp)}, "pow.r");
  }

  // --- Comparison ---

  // Fast Long-Long equality / inequality fast path plus a slow path
  // that forwards to the runtime helper. String, reference-type, and
  // numeric cross-type (Long↔Float) all go through the runtime — it
  // already implements matching semantics with the interpreter.
  llvm::Value* compile_condition(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto lhs = compile(*ast.nodes[0]);
    auto ope_str = std::string(ast.nodes[1]->token);
    auto rhs = compile(*ast.nodes[2]);

    auto ltag = extract_tag(lhs);
    auto ldata = extract_data(lhs);
    auto rtag = extract_tag(rhs);
    auto rdata = extract_data(rhs);

    auto fn = builder_.GetInsertBlock()->getParent();

    if (ope_str == "==" || ope_str == "!=") {
      // Fast path: both TAG_LONG — inline icmp. Anything else goes
      // through the runtime equality helper, which handles Float,
      // cross-type numeric (`1 == 1.0`), String by contents, and
      // reference-type identity.
      auto longTag = builder_.getInt8(TAG_LONG);
      auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
      auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
      auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "eq.bothlong");

      auto fastBB = llvm::BasicBlock::Create(ctx_, "eq.fast", fn);
      auto slowBB = llvm::BasicBlock::Create(ctx_, "eq.slow", fn);
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "eq.merge", fn);
      builder_.CreateCondBr(bothLong, fastBB, slowBB);

      builder_.SetInsertPoint(fastBB);
      auto fastEq = builder_.CreateICmpEQ(ldata, rdata, "long.eq");
      auto fastEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      auto slowEq = emit_call(
          module_->getOrInsertFunction(rt::value_equal,
                                       builder_.getInt1Ty(),
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty(),
                                       builder_.getInt8Ty(),
                                       builder_.getInt64Ty()),
          {ltag, ldata, rtag, rdata}, "val.eq");
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "eq.r");
      phi->addIncoming(fastEq, fastEndBB);
      phi->addIncoming(slowEq, slowEndBB);
      llvm::Value* result = phi;
      if (ope_str == "!=") result = builder_.CreateNot(result, "neq");
      return make_bool(result);
    }

    // Ordering operators. Fast path for both-Long; runtime helper
    // handles Float, Long↔Float promotion, String, Nil, and type
    // errors on reference types.
    auto longTag = builder_.getInt8(TAG_LONG);
    auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
    auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
    auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "ord.bothlong");

    auto fastBB = llvm::BasicBlock::Create(ctx_, "ord.fast", fn);
    auto slowBB = llvm::BasicBlock::Create(ctx_, "ord.slow", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ord.merge", fn);
    builder_.CreateCondBr(bothLong, fastBB, slowBB);

    builder_.SetInsertPoint(fastBB);
    llvm::Value* fastResult;
    if (ope_str == "<") {
      fastResult = builder_.CreateICmpSLT(ldata, rdata);
    } else if (ope_str == "<=") {
      fastResult = builder_.CreateICmpSLE(ldata, rdata);
    } else if (ope_str == ">") {
      fastResult = builder_.CreateICmpSGT(ldata, rdata);
    } else if (ope_str == ">=") {
      fastResult = builder_.CreateICmpSGE(ldata, rdata);
    } else {
      throw std::runtime_error("invalid comparison operator");
    }
    auto fastEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // Slow path: dispatch to the helper that mirrors the interpreter's
    // Value::operator<{,=,>,=}. Separate helpers (rather than deriving
    // <=, >, >= from <) are necessary because `nil op nil` is `false`
    // for all ordering operators — not the standard total-order
    // relationship.
    builder_.SetInsertPoint(slowBB);
    const char* ord_rt = nullptr;
    if (ope_str == "<") ord_rt = rt::value_less;
    else if (ope_str == "<=") ord_rt = rt::value_leq;
    else if (ope_str == ">") ord_rt = rt::value_greater;
    else if (ope_str == ">=") ord_rt = rt::value_geq;
    else throw std::runtime_error("invalid comparison operator");

    auto slowResult = emit_call(
        module_->getOrInsertFunction(ord_rt,
                                     builder_.getInt1Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty(),
                                     builder_.getInt8Ty(),
                                     builder_.getInt64Ty()),
        {ltag, ldata, rtag, rdata}, "val.ord");
    auto slowEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "ord.r");
    phi->addIncoming(fastResult, fastEndBB);
    phi->addIncoming(slowResult, slowEndBB);
    return make_bool(phi);
  }

  // --- Logical operators (short-circuit) ---

  llvm::Value* compile_logical_or(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "or.merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca =
        entryBuilder.CreateAlloca(valueType_, nullptr, "or.tmp");

    for (auto i = 0u; i < ast.nodes.size(); i++) {
      auto val = compile(*ast.nodes[i]);
      builder_.CreateStore(val, resultAlloca);
      auto b = value_to_bool(val);

      if (i < ast.nodes.size() - 1) {
        auto nextBB = llvm::BasicBlock::Create(ctx_, "or.next", fn);
        builder_.CreateCondBr(b, mergeBB, nextBB);
        builder_.SetInsertPoint(nextBB);
      } else {
        builder_.CreateBr(mergeBB);
      }
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "or.result");
  }

  llvm::Value* compile_logical_and(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "and.merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca =
        entryBuilder.CreateAlloca(valueType_, nullptr, "and.tmp");

    for (auto i = 0u; i < ast.nodes.size(); i++) {
      auto val = compile(*ast.nodes[i]);
      builder_.CreateStore(val, resultAlloca);
      auto b = value_to_bool(val);

      if (i < ast.nodes.size() - 1) {
        auto nextBB = llvm::BasicBlock::Create(ctx_, "and.next", fn);
        builder_.CreateCondBr(b, nextBB, mergeBB);
        builder_.SetInsertPoint(nextBB);
      } else {
        builder_.CreateBr(mergeBB);
      }
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "and.result");
  }

  // --- Control flow ---

  llvm::Value* compile_if(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "if.merge");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(),
                                   fn->getEntryBlock().begin());
    auto resultAlloca =
        entryBuilder.CreateAlloca(valueType_, nullptr, "if.tmp");

    // Initialize with nil
    builder_.CreateStore(make_nil(), resultAlloca);

    const auto& nodes = ast.nodes;
    for (auto i = 0u; i < nodes.size(); i += 2) {
      if (i + 1 == nodes.size()) {
        // else block
        auto val = compile(*nodes[i]);
        if (!builder_.GetInsertBlock()->getTerminator()) {
          builder_.CreateStore(val, resultAlloca);
          builder_.CreateBr(mergeBB);
        }
      } else {
        auto cond = compile(*nodes[i]);
        auto b = value_to_bool(cond);

        auto thenBB = llvm::BasicBlock::Create(ctx_, "if.then", fn);
        auto elseBB = llvm::BasicBlock::Create(ctx_, "if.else", fn);
        builder_.CreateCondBr(b, thenBB, elseBB);

        builder_.SetInsertPoint(thenBB);
        auto thenVal = compile(*nodes[i + 1]);
        if (!builder_.GetInsertBlock()->getTerminator()) {
          builder_.CreateStore(thenVal, resultAlloca);
          builder_.CreateBr(mergeBB);
        }

        builder_.SetInsertPoint(elseBB);
      }
    }

    // If no else clause, current block is the last else-BB
    if (nodes.size() % 2 == 0) {
      builder_.CreateBr(mergeBB);
    }

    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "if.result");
  }

  // Emit IR that tests whether `subject` matches `pattern`.
  // On success, emits variable bindings (via define_var) visible from the
  // current scope; returns i1 true. On failure, returns i1 false; any
  // bindings emitted speculatively are not read because the arm is skipped.
  llvm::Value* emit_pattern(const peg::Ast& pattern, llvm::Value* subject) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // OR pattern: PATTERN node with multiple sub-patterns
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      auto fn = builder_.GetInsertBlock()->getParent();
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "or.match", fn);
      std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
      for (size_t i = 0; i < pattern.nodes.size(); i++) {
        auto m = emit_pattern(*pattern.nodes[i], subject);
        incoming.push_back({m, builder_.GetInsertBlock()});
        if (i + 1 < pattern.nodes.size()) {
          auto nextBB = llvm::BasicBlock::Create(ctx_, "or.try", fn);
          builder_.CreateCondBr(m, mergeBB, nextBB);
          builder_.SetInsertPoint(nextBB);
        } else {
          builder_.CreateBr(mergeBB);
        }
      }
      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(builder_.getInt1Ty(),
                                    incoming.size(), "or.result");
      for (auto& [v, bb] : incoming) phi->addIncoming(v, bb);
      return phi;
    }

    switch (pattern.tag) {
      case "WILDCARD"_:
        return builder_.getTrue();
      case "NIL"_:
        return builder_.CreateICmpEQ(extract_tag(subject),
                                     builder_.getInt8(TAG_NIL));
      case "BOOLEAN"_: {
        auto is_bool = builder_.CreateICmpEQ(extract_tag(subject),
                                             builder_.getInt8(TAG_BOOL));
        auto want = builder_.getInt64(pattern.token == "true" ? 1 : 0);
        auto eq = builder_.CreateICmpEQ(extract_data(subject), want);
        return builder_.CreateAnd(is_bool, eq);
      }
      case "NUMBER"_: {
        auto is_long = builder_.CreateICmpEQ(extract_tag(subject),
                                             builder_.getInt8(TAG_LONG));
        auto want = builder_.getInt64(pattern.token_to_number<long>());
        auto eq = builder_.CreateICmpEQ(extract_data(subject), want);
        return builder_.CreateAnd(is_long, eq);
      }
      case "FLOAT"_: {
        auto is_float = builder_.CreateICmpEQ(extract_tag(subject),
                                              builder_.getInt8(TAG_FLOAT));
        auto subjD = builder_.CreateBitCast(
            extract_data(subject), builder_.getDoubleTy(), "pat.d");
        auto want = llvm::ConstantFP::get(
            builder_.getDoubleTy(), pattern.token_to_number<double>());
        // FCmpOEQ: NaN never equals, matching interpreter.
        auto eq = builder_.CreateFCmpOEQ(subjD, want);
        return builder_.CreateAnd(is_float, eq);
      }
      case "STRING"_:
      case "INTERPOLATED_CONTENT"_: {
        auto is_str = builder_.CreateICmpEQ(extract_tag(subject),
                                            builder_.getInt8(TAG_STRING));
        auto lit = builder_.CreateGlobalString(std::string(pattern.token),
                                               ".pat.str");
        auto subj_ptr =
            builder_.CreateIntToPtr(extract_data(subject), ptrTy);
        auto eq = emit_call(
            module_->getOrInsertFunction(rt::str_eq,
                                         builder_.getInt1Ty(), ptrTy, ptrTy),
            {subj_ptr, lit});
        return builder_.CreateAnd(is_str, eq);
      }
      case "IDENTIFIER"_: {
        // Always matches; bind subject to this name.
        auto name = std::string(pattern.token);
        bool captured = current_info_ &&
                        current_info_->captured_locals.contains(name);
        auto slot = captured ? make_cell_slot(name, subject)
                             : make_stack_slot(name, subject);
        define_var(name, slot);
        return builder_.getTrue();
      }
      case "TYPED_IDENT"_: {
        auto type_name = pattern.nodes[1]->token;
        auto tag = extract_tag(subject);
        llvm::Value* tag_match;
        if (type_name == "Any") {
          tag_match = builder_.getTrue();
        } else if (type_name == "Nil") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_NIL));
        } else if (type_name == "Bool") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_BOOL));
        } else if (type_name == "Long") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
        } else if (type_name == "Float") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FLOAT));
        } else if (type_name == "String") {
          tag_match =
              builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_STRING));
        } else if (type_name == "Array") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));
        } else if (type_name == "Object") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));
        } else if (type_name == "Function") {
          tag_match = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC));
        } else {
          tag_match = builder_.getFalse();
        }
        // Bind unconditionally; only used if arm actually runs.
        auto name = std::string(pattern.nodes[0]->token);
        bool captured = current_info_ &&
                        current_info_->captured_locals.contains(name);
        auto slot = captured ? make_cell_slot(name, subject)
                             : make_stack_slot(name, subject);
        define_var(name, slot);
        return tag_match;
      }
      case "ARRAY_PATTERN"_:
        return emit_array_pattern(pattern, subject);
      case "OBJECT_PATTERN"_:
        return emit_object_pattern(pattern, subject);
    }
    return builder_.getFalse();
  }

  // Element-by-element destructuring with short-circuit on first mismatch.
  llvm::Value* emit_array_pattern(const peg::Ast& pattern,
                                  llvm::Value* subject) {
    using namespace peg::udl;
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx_, "arr.fail", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "arr.ok", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "arr.end", fn);

    // 1. Tag must be Array
    auto isArr = builder_.CreateICmpEQ(extract_tag(subject),
                                       builder_.getInt8(TAG_ARRAY));
    auto sizeBB = llvm::BasicBlock::Create(ctx_, "arr.size", fn);
    builder_.CreateCondBr(isArr, sizeBB, failBB);
    builder_.SetInsertPoint(sizeBB);

    auto arrPtr = builder_.CreateIntToPtr(extract_data(subject), ptrTy);
    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size,
                                     builder_.getInt64Ty(), ptrTy),
        {arrPtr}, "arr.n");

    const auto& elems = pattern.nodes;
    int rest_idx = -1;
    for (size_t i = 0; i < elems.size(); i++) {
      if (elems[i]->tag == "REST_PATTERN"_) {
        rest_idx = static_cast<int>(i);
        break;
      }
    }
    auto fixed = elems.size() - (rest_idx >= 0 ? 1 : 0);

    // 2. Size check
    llvm::Value* size_ok;
    if (rest_idx < 0) {
      size_ok = builder_.CreateICmpEQ(size, builder_.getInt64(fixed));
    } else {
      size_ok = builder_.CreateICmpSGE(size, builder_.getInt64(fixed));
    }
    auto elemBB = llvm::BasicBlock::Create(ctx_, "arr.elem", fn);
    builder_.CreateCondBr(size_ok, elemBB, failBB);
    builder_.SetInsertPoint(elemBB);

    // Helper: fetch arr[i] as a Value
    auto get_elem = [&](llvm::Value* idx) {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto outTag =
          entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "ap.tag");
      auto outData =
          entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "ap.data");
      emit_call(
          module_->getOrInsertFunction(
              rt::array_get, builder_.getVoidTy(), ptrTy,
              builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
              builder_.getInt64Ty()),
          {arrPtr, idx, outTag, outData, current_line_val(),
           current_column_val()});
      auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
      auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, t, {0});
      v = builder_.CreateInsertValue(v, d, {1});
      return v;
    };

    // 3. Match each element, short-circuit on failure
    auto pre_count =
        rest_idx < 0 ? elems.size() : static_cast<size_t>(rest_idx);
    for (size_t i = 0; i < pre_count; i++) {
      auto v = get_elem(builder_.getInt64(i));
      auto m = emit_pattern(*elems[i], v);
      auto contBB = llvm::BasicBlock::Create(ctx_, "arr.cont", fn);
      builder_.CreateCondBr(m, contBB, failBB);
      builder_.SetInsertPoint(contBB);
    }

    if (rest_idx >= 0) {
      // Slice rest: start = rest_idx, len = size - fixed
      auto rest_start = builder_.getInt64(rest_idx);
      auto rest_len =
          builder_.CreateSub(size, builder_.getInt64(fixed), "rest.len");
      auto rest_arr_ptr = emit_call(
          module_->getOrInsertFunction(rt::array_slice, ptrTy,
                                       ptrTy, builder_.getInt64Ty(),
                                       builder_.getInt64Ty()),
          {arrPtr, rest_start, rest_len}, "rest.arr");
      auto rest_val = make_array(rest_arr_ptr);
      auto rest_name =
          std::string(elems[rest_idx]->nodes[0]->token);
      bool cap = current_info_ &&
                 current_info_->captured_locals.contains(rest_name);
      auto slot = cap ? make_cell_slot(rest_name, rest_val)
                      : make_stack_slot(rest_name, rest_val);
      define_var(rest_name, slot);

      // Match post-rest elements (from end of array)
      for (size_t i = rest_idx + 1; i < elems.size(); i++) {
        auto off = static_cast<int64_t>(elems.size() - i);
        auto idx = builder_.CreateSub(size, builder_.getInt64(off));
        auto v = get_elem(idx);
        auto m = emit_pattern(*elems[i], v);
        auto contBB = llvm::BasicBlock::Create(ctx_, "arr.cont2", fn);
        builder_.CreateCondBr(m, contBB, failBB);
        builder_.SetInsertPoint(contBB);
      }
    }

    builder_.CreateBr(okBB);

    builder_.SetInsertPoint(okBB);
    auto okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(failBB);
    auto failEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "arr.match");
    phi->addIncoming(builder_.getTrue(), okEnd);
    phi->addIncoming(builder_.getFalse(), failEnd);
    return phi;
  }

  llvm::Value* emit_object_pattern(const peg::Ast& pattern,
                                   llvm::Value* subject) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx_, "obj.fail", fn);
    auto okBB = llvm::BasicBlock::Create(ctx_, "obj.ok", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "obj.end", fn);

    auto isObj = builder_.CreateICmpEQ(extract_tag(subject),
                                       builder_.getInt8(TAG_OBJECT));
    auto contBB = llvm::BasicBlock::Create(ctx_, "obj.next", fn);
    builder_.CreateCondBr(isObj, contBB, failBB);
    builder_.SetInsertPoint(contBB);

    auto objPtr = builder_.CreateIntToPtr(extract_data(subject), ptrTy);

    // For each key: has + get + bind
    for (const auto& key_node : pattern.nodes) {
      auto key = std::string(key_node->token);
      auto keyPtr = get_or_create_global_str(key, ".obj.key");
      auto has = emit_object_has(objPtr, keyPtr);
      auto next = llvm::BasicBlock::Create(ctx_, "obj.bind", fn);
      builder_.CreateCondBr(has, next, failBB);
      builder_.SetInsertPoint(next);

      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      auto outTag =
          entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "op.tag");
      auto outData =
          entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "op.data");
      emit_call(
          module_->getOrInsertFunction(rt::object_get,
                                       builder_.getVoidTy(), ptrTy, ptrTy,
                                       ptrTy, ptrTy),
          {objPtr, keyPtr, outTag, outData});
      auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
      auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
      llvm::Value* v = llvm::UndefValue::get(valueType_);
      v = builder_.CreateInsertValue(v, t, {0});
      v = builder_.CreateInsertValue(v, d, {1});
      // Retain since we're creating a new owning reference in the slot
      emit_value_retain(v);

      bool cap = current_info_ &&
                 current_info_->captured_locals.contains(key);
      auto slot = cap ? make_cell_slot(key, v) : make_stack_slot(key, v);
      define_var(key, slot);
    }

    builder_.CreateBr(okBB);
    builder_.SetInsertPoint(okBB);
    auto okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(failBB);
    auto failEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, "obj.match");
    phi->addIncoming(builder_.getTrue(), okEnd);
    phi->addIncoming(builder_.getFalse(), failEnd);
    return phi;
  }

  llvm::Value* compile_match(const peg::Ast& ast) {
    using namespace peg::udl;
    auto fn = builder_.GetInsertBlock()->getParent();

    auto subject = compile(*ast.nodes[0]);
    // Stash subject in a local so every arm reads the same SSA value.
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto subjAlloca = entryB.CreateAlloca(valueType_, nullptr, "match.subj");
    builder_.CreateStore(subject, subjAlloca);

    auto endBB = llvm::BasicBlock::Create(ctx_, "match.end", fn);
    auto resultAlloca =
        entryB.CreateAlloca(valueType_, nullptr, "match.result");
    builder_.CreateStore(make_nil(), resultAlloca);

    const auto& arms = ast.nodes[1]->nodes;  // MATCH_ARMS
    for (size_t ai = 0; ai < arms.size(); ai++) {
      auto next_arm_bb =
          llvm::BasicBlock::Create(ctx_, "match.next", fn);
      auto body_bb = llvm::BasicBlock::Create(ctx_, "match.body", fn);

      push_scope();

      auto subj_val = builder_.CreateLoad(valueType_, subjAlloca);
      auto match_cond = emit_pattern(*arms[ai]->nodes[0], subj_val);
      // Either head for guard check or straight to body
      size_t next_idx = 1;
      bool has_guard = arms[ai]->nodes.size() > 1 &&
                       arms[ai]->nodes[1]->tag == "GUARD"_;
      if (has_guard) {
        auto guard_bb =
            llvm::BasicBlock::Create(ctx_, "match.guard", fn);
        builder_.CreateCondBr(match_cond, guard_bb, next_arm_bb);
        builder_.SetInsertPoint(guard_bb);
        auto guard_val = compile(*arms[ai]->nodes[1]->nodes[0]);
        auto guard_bool = value_to_bool(guard_val);
        builder_.CreateCondBr(guard_bool, body_bb, next_arm_bb);
        next_idx = 2;
      } else {
        builder_.CreateCondBr(match_cond, body_bb, next_arm_bb);
      }

      builder_.SetInsertPoint(body_bb);
      auto body_val = compile(*arms[ai]->nodes[next_idx]);
      builder_.CreateStore(body_val, resultAlloca);
      if (!builder_.GetInsertBlock()->getTerminator()) {
        builder_.CreateBr(endBB);
      }

      pop_scope();
      builder_.SetInsertPoint(next_arm_bb);
    }

    // No arm matched → result stays nil
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(valueType_, resultAlloca, "match.res");
  }

  llvm::Value* compile_while(const peg::Ast& ast) {
    auto fn = builder_.GetInsertBlock()->getParent();

    auto condBB = llvm::BasicBlock::Create(ctx_, "while.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "while.body", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "while.end", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto cond = compile(*ast.nodes[0]);
    auto b = value_to_bool(cond);
    builder_.CreateCondBr(b, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    loop_stack_.push_back({condBB, endBB});
    compile(*ast.nodes[1]);
    loop_stack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    return make_nil();
  }

  // Branch to `target`, then switch the insert point to a fresh dead
  // block so any statements the caller still emits in the same scope
  // land somewhere valid (their IR is simply unreachable).
  void branch_then_dead(llvm::BasicBlock* target, const char* dead_name) {
    builder_.CreateBr(target);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto dead = llvm::BasicBlock::Create(ctx_, dead_name, fn);
    builder_.SetInsertPoint(dead);
  }

  llvm::Value* compile_break(const peg::Ast& ast) {
    if (loop_stack_.empty()) {
      throw std::runtime_error(std::format(
          "break outside loop at {}:{}.", ast.line, ast.column));
    }
    branch_then_dead(loop_stack_.back().break_target, "break.dead");
    return make_nil();
  }

  llvm::Value* compile_continue(const peg::Ast& ast) {
    if (loop_stack_.empty()) {
      throw std::runtime_error(std::format(
          "continue outside loop at {}:{}.", ast.line, ast.column));
    }
    branch_then_dead(loop_stack_.back().continue_target, "continue.dead");
    return make_nil();
  }

  llvm::Value* compile_for(const peg::Ast& ast) {
    auto& id = *ast.nodes[0];
    auto& iter_expr = *ast.nodes[1];
    auto& body = *ast.nodes[2];
    auto var_name = std::string(id.token);

    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto iterable = compile(iter_expr);
    auto tag = extract_tag(iterable);
    auto data = extract_data(iterable);

    auto arrayBB = llvm::BasicBlock::Create(ctx_, "for.array", fn);
    auto objectBB = llvm::BasicBlock::Create(ctx_, "for.object", fn);
    auto stringBB = llvm::BasicBlock::Create(ctx_, "for.string", fn);
    auto badBB = llvm::BasicBlock::Create(ctx_, "for.bad_type", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "for.end", fn);

    auto sw = builder_.CreateSwitch(tag, badBB, 3);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrayBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objectBB);
    sw->addCase(builder_.getInt8(TAG_STRING), stringBB);

    builder_.SetInsertPoint(badBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrayBB);
    compile_for_array_loop(builder_.CreateIntToPtr(data, ptrTy),
                           var_name, body, endBB);

    // Object branch splits on whether the receiver carries its own
    // `iter` property:
    //   yes → drive the iterator protocol (Math.range, user-defined
    //         iterators, `.code_points()`/`.graphemes()` et al.)
    //   no  → materialize keys and reuse the native array loop
    // The keys path is unchanged from before, so plain `for k in obj`
    // pays zero overhead vs the prior compiler output.
    builder_.SetInsertPoint(objectBB);
    auto objPtr = builder_.CreateIntToPtr(data, ptrTy);
    auto iterKeyPtr = get_or_create_global_str("iter", ".iter.key");
    auto hasIter = emit_object_has(objPtr, iterKeyPtr);
    auto keysBB = llvm::BasicBlock::Create(ctx_, "for.obj.keys", fn);
    auto protoBB = llvm::BasicBlock::Create(ctx_, "for.obj.proto", fn);
    builder_.CreateCondBr(hasIter, protoBB, keysBB);

    builder_.SetInsertPoint(keysBB);
    auto keysArr = emit_call(
        module_->getOrInsertFunction(rt::object_keys, ptrTy, ptrTy),
        {objPtr}, "obj.keys");
    compile_for_array_loop(keysArr, var_name, body, endBB);

    builder_.SetInsertPoint(protoBB);
    llvm::Value* objVal = llvm::UndefValue::get(valueType_);
    objVal = builder_.CreateInsertValue(objVal, builder_.getInt8(TAG_OBJECT),
                                        {0});
    objVal = builder_.CreateInsertValue(objVal, data, {1});
    compile_for_protocol_loop(objVal, var_name, body, endBB);

    builder_.SetInsertPoint(stringBB);
    compile_for_string_loop(builder_.CreateIntToPtr(data, ptrTy),
                            var_name, body, endBB);

    builder_.SetInsertPoint(endBB);
    return make_nil();
  }

  // Bind `elemVal` to `var_name` for the body of a for-in. Caller must
  // have transferred `+1` into `elemVal`; the slot takes over that ref
  // and releases on scope exit. Useful when the caller has already
  // arranged ownership (e.g. the protocol loop pre-retains the step
  // value before releasing the enclosing {done,value} object).
  void emit_for_body_with_owned_binding(const std::string& var_name,
                                        llvm::Value* elemVal,
                                        const peg::Ast& body,
                                        llvm::BasicBlock* continue_bb,
                                        llvm::BasicBlock* break_bb) {
    push_scope();
    bool captured = current_info_ &&
                    current_info_->captured_locals.contains(var_name);
    auto slot = captured ? make_cell_slot(var_name, elemVal)
                         : make_stack_slot(var_name, elemVal);
    define_var(var_name, slot);

    loop_stack_.push_back({continue_bb, break_bb});
    compile(body);
    loop_stack_.pop_back();

    pop_scope();

    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(continue_bb);
    }
  }

  // Borrowed-input variant: retains `elemVal` once (to balance the
  // slot release) and delegates. Used by the Array / String for-in
  // loops where `elemVal` is read directly out of the container with
  // no owning ref of its own.
  void emit_for_body_iteration(const std::string& var_name,
                               llvm::Value* elemVal,
                               const peg::Ast& body,
                               llvm::BasicBlock* continue_bb,
                               llvm::BasicBlock* break_bb) {
    emit_value_retain(elemVal);
    emit_for_body_with_owned_binding(var_name, elemVal, body, continue_bb,
                                     break_bb);
  }

  void compile_for_array_loop(llvm::Value* arrPtr,
                              const std::string& var_name,
                              const peg::Ast& body,
                              llvm::BasicBlock* endBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto size = emit_call(
        module_->getOrInsertFunction(rt::array_size, builder_.getInt64Ty(),
                                     ptrTy),
        {arrPtr}, "for.size");

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto idxAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.idx");
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "for.out.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.out.data");
    builder_.CreateStore(builder_.getInt64(0), idxAlloca);

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.a.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.a.body", fn);
    auto incBB = llvm::BasicBlock::Create(ctx_, "for.a.inc", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto idx = builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca, "for.i");
    builder_.CreateCondBr(builder_.CreateICmpSGE(idx, size), endBB, bodyBB);

    builder_.SetInsertPoint(bodyBB);
    emit_call(
        module_->getOrInsertFunction(
            rt::array_get, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {arrPtr, idx, outTag, outData, current_line_val(),
         current_column_val()});
    auto t = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto d = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* elem = llvm::UndefValue::get(valueType_);
    elem = builder_.CreateInsertValue(elem, t, {0});
    elem = builder_.CreateInsertValue(elem, d, {1});

    emit_for_body_iteration(var_name, elem, body, incBB, endBB);

    builder_.SetInsertPoint(incBB);
    auto next = builder_.CreateAdd(
        builder_.CreateLoad(builder_.getInt64Ty(), idxAlloca),
        builder_.getInt64(1));
    builder_.CreateStore(next, idxAlloca);
    builder_.CreateBr(condBB);
  }

  // Drive the iterator protocol for objects that carry a user-defined
  // `iter` property (Math.range, `.code_points()`, custom iterators).
  // Semantics match `_get_iterator` / `eval_for`'s protocol branch in
  // the interpreter: `obj.iter()` yields an iterator Object; then each
  // `next()` returns `{done, value}` until `done == true`.
  //
  // Refcount flow: `iter()` returns a fresh `+1` iterator value stored
  // in a local slot so it survives across iterations. Each method call
  // retains its receiver before invocation (the callee frame releases
  // on exit via the owned `this` slot). `step` — the {done, value}
  // object — is released on every loop path before branching.
  void compile_for_protocol_loop(llvm::Value* objVal,
                                 const std::string& var_name,
                                 const peg::Ast& body,
                                 llvm::BasicBlock* endBB) {
    auto fn = builder_.GetInsertBlock()->getParent();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // iter_val = obj.iter()
    auto iter_fn_val = compile_property_get(objVal, "iter");
    emit_value_retain(objVal);  // handed off to `this` slot
    auto iter_val = compile_function_call_raw(iter_fn_val, objVal, {});

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto iterAlloca = entryB.CreateAlloca(valueType_, nullptr, "for.iter");
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_),
                       iterAlloca);
    builder_.CreateStore(iter_val, iterAlloca);

    // `next` is fixed for an iterator's lifetime — hoist the lookup.
    auto next_fn_val = compile_property_get(iter_val, "next");
    auto next_cls_ptr =
        builder_.CreateIntToPtr(extract_data(next_fn_val), ptrTy, "next.cls");

    auto outTagAlloca =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "for.p.out_tag");
    auto outDataAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.p.out_data");

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.p.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.p.body", fn);
    auto cleanupBB = llvm::BasicBlock::Create(ctx_, "for.p.cleanup", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto iterCur = builder_.CreateLoad(valueType_, iterAlloca, "iter.cur");
    auto iterTag = extract_tag(iterCur);
    auto iterData = extract_data(iterCur);
    auto ok = emit_call(
        module_->getFunction(rt::iter_advance),
        {next_cls_ptr, iterTag, iterData, outTagAlloca, outDataAlloca},
        "for.p.ok");
    auto alive = builder_.CreateICmpNE(ok, builder_.getInt64(0),
                                       "for.p.alive");
    builder_.CreateCondBr(alive, bodyBB, cleanupBB);

    builder_.SetInsertPoint(bodyBB);
    auto outTag =
        builder_.CreateLoad(builder_.getInt8Ty(), outTagAlloca, "for.p.tag");
    auto outData = builder_.CreateLoad(builder_.getInt64Ty(), outDataAlloca,
                                       "for.p.data");
    llvm::Value* loop_val = llvm::UndefValue::get(valueType_);
    loop_val = builder_.CreateInsertValue(loop_val, outTag, {0});
    loop_val = builder_.CreateInsertValue(loop_val, outData, {1});
    emit_for_body_with_owned_binding(var_name, loop_val, body, condBB,
                                     cleanupBB);

    // cleanupBB: release the iterator slot contents, exit the for-in
    builder_.SetInsertPoint(cleanupBB);
    auto iterFinal = builder_.CreateLoad(valueType_, iterAlloca, "iter.fin");
    emit_value_release(iterFinal);
    builder_.CreateBr(endBB);
  }

  void compile_for_string_loop(llvm::Value* strPtr,
                               const std::string& var_name,
                               const peg::Ast& body,
                               llvm::BasicBlock* endBB) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto len = emit_call(
        module_->getOrInsertFunction(rt::str_size, builder_.getInt64Ty(),
                                     ptrTy),
        {strPtr}, "for.slen");

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto offAlloca =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "for.off");
    builder_.CreateStore(builder_.getInt64(0), offAlloca);

    auto condBB = llvm::BasicBlock::Create(ctx_, "for.s.cond", fn);
    auto bodyBB = llvm::BasicBlock::Create(ctx_, "for.s.body", fn);
    auto incBB = llvm::BasicBlock::Create(ctx_, "for.s.inc", fn);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    auto off = builder_.CreateLoad(builder_.getInt64Ty(), offAlloca, "for.o");
    auto scalarLen = emit_call(
        module_->getOrInsertFunction(rt::utf8_scalar_len,
                                     builder_.getInt64Ty(), ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {strPtr, off, len}, "for.slen1");
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(scalarLen, builder_.getInt64(0)), endBB,
        bodyBB);

    builder_.SetInsertPoint(bodyBB);
    auto scalarPtr = emit_call(
        module_->getOrInsertFunction(rt::str_scalar_at, ptrTy, ptrTy,
                                     builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {strPtr, off, scalarLen}, "for.scalar");
    emit_for_body_iteration(var_name, make_string(scalarPtr), body, incBB,
                            endBB);

    builder_.SetInsertPoint(incBB);
    auto next = builder_.CreateAdd(
        builder_.CreateLoad(builder_.getInt64Ty(), offAlloca), scalarLen);
    builder_.CreateStore(next, offAlloca);
    builder_.CreateBr(condBB);
  }

  // --- Lexical scope ---

  llvm::Value* compile_lexical_scope(const peg::Ast& ast) {
    // No defers in this scope → only variable-binding scoping needed.
    bool has_defer = scope_has_defer_.contains(&ast);

    llvm::Value* mark = nullptr;
    llvm::BasicBlock* cleanupBB = nullptr;
    llvm::BasicBlock* savedLpad = current_lpad_;
    if (has_defer) {
      // defer_mark is nothrow, so a plain call (not invoke) is safe.
      mark = builder_.CreateCall(
          module_->getFunction(rt::defer_mark), {}, "scope.mark");
      auto fn = builder_.GetInsertBlock()->getParent();
      cleanupBB = llvm::BasicBlock::Create(ctx_, "scope.cleanup", fn);
      current_lpad_ = cleanupBB;
    }

    push_scope();
    llvm::Value* val = make_nil();
    for (auto& node : ast.nodes) {
      if (builder_.GetInsertBlock()->getTerminator()) break;
      val = compile(*node);
    }
    // Statement-like: drop the block's final expression before
    // pop_scope so `drop` callbacks fire in true LIFO order (a retained
    // expression result would otherwise outlive the slot release).
    // Matches the interpreter's standalone-block semantics.
    if (!builder_.GetInsertBlock()->getTerminator()) {
      emit_value_release(val);
    }
    val = make_nil();
    pop_scope();
    current_lpad_ = savedLpad;

    if (has_defer && !builder_.GetInsertBlock()->getTerminator()) {
      emit_call(module_->getFunction(rt::defer_run_to),
                {mark});
    }
    auto afterBB = builder_.GetInsertBlock();

    if (has_defer) {
      emit_cleanup_landingpad(cleanupBB, mark, savedLpad, "scope.exc");
    }

    builder_.SetInsertPoint(afterBB);
    return val;
  }

  // --- Function ---

  llvm::Value* compile_function(const peg::Ast& ast) {
    using namespace llvm;
    auto ptrTy = PointerType::get(ctx_, 0);

    // Look up this function's analysis info
    auto infoIt = func_info_.find(&ast);
    if (infoIt == func_info_.end()) {
      throw std::runtime_error("missing func_info for function");
    }
    const FuncInfo& info = infoIt->second;

    // Collect parameters (name + optional type annotation)
    std::vector<std::string> paramNames;
    std::vector<std::string_view> paramTypeNames;
    for (auto& node : ast.nodes[0]->nodes) {
      paramNames.push_back(std::string(node->nodes[1]->token));
      paramTypeNames.push_back(extract_type_annotation(*node, 2));
    }

    size_t bodyIdx = 1;
    auto returnType = extract_return_type(ast, bodyIdx);

    // Uniform closure ABI:
    //   Value fn(ptr __cls__, Value this, i64 n_args, ptr args)
    // `args` points at a caller-owned stack slab of `n_args` Value
    // structs (each with a transferred +1). Declared params and the
    // variadic `__ARGS__` binding are extracted from this slab in the
    // function prologue. The uniform shape lets every call site (user
    // calls, UFCS, runtime callbacks) use one ABI even when the
    // declared arity differs from the passed arg count.
    auto fnType = FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);

    auto fnName = std::format("__culebra_fn_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);
    if (info.has_eh) {
      fn->setPersonalityFn(get_personality_fn());
    }

    auto argIt = fn->arg_begin();
    argIt->setName("__cls__");
    ++argIt;
    argIt->setName("this");
    ++argIt;
    argIt->setName("n_args");
    ++argIt;
    argIt->setName("args");

    CompilerStateSaver saver(*this);
    current_info_ = &info;
    current_return_type_ = returnType;

    auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entryBB);
    push_scope();

    // Function-level defers (not inside a nested `{}`) run on normal
    // fall-through and on `return`; for throw-path cleanup, wrap them
    // in a block (`{ defer { ... } ... }`) which emits a proper scope
    // cleanup landingpad.
    llvm::Value* fnMark = nullptr;
    if (info.has_fn_defer) {
      fnMark = builder_.CreateCall(
          module_->getFunction(rt::defer_mark), {}, "fn.mark");
      current_fn_defer_mark_ = fnMark;
    }

    argIt = fn->arg_begin();
    auto clsArg = &*argIt++;
    current_closure_arg_ = clsArg;
    auto thisArg = &*argIt++;
    auto nArgsArg = &*argIt++;
    auto argsArg = &*argIt++;

    // Arity guard: matching the interpreter, it is an error to call
    // with fewer args than declared. Overflow is allowed (lands in
    // `__ARGS__`).
    size_t declaredArity = paramNames.size();
    {
      auto need = builder_.getInt64(static_cast<int64_t>(declaredArity));
      auto tooFew = builder_.CreateICmpULT(nArgsArg, need);
      auto okBB = BasicBlock::Create(ctx_, "arity.ok", fn);
      auto errBB = BasicBlock::Create(ctx_, "arity.err", fn);
      builder_.CreateCondBr(tooFew, errBB, okBB);

      builder_.SetInsertPoint(errBB);
      emit_call(
          module_->getOrInsertFunction(
              rt::arity_error, builder_.getVoidTy(),
              builder_.getInt64Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {nArgsArg, need, current_line_val(), current_column_val()});
      builder_.CreateUnreachable();
      builder_.SetInsertPoint(okBB);
    }

    // `self` = make_func(__cls__): calling it re-enters this fn with this
    // closure. make_func doesn't retain, so retain explicitly so the
    // owned `self` slot has the +1 it will hand back at function exit.
    {
      auto selfVal = make_func(clsArg);
      emit_value_retain(selfVal);
      define_var("self", make_stack_slot("self", selfVal));
    }

    // `this` is from arg; if captured, allocate cell. The arg's +1 was
    // pre-retained by the caller (via load/compile of the receiver) and
    // is transferred into the slot — no extra retain needed.
    if (info.captured_locals.contains("this")) {
      define_var("this", make_cell_slot("this", thisArg));
    } else {
      define_var("this", make_stack_slot("this", thisArg));
    }

    // Declared parameters: load from args[i] (the caller transferred
    // each +1 into the slab) and hand straight to the slot.
    for (size_t i = 0; i < paramNames.size(); i++) {
      const auto& name = paramNames[i];
      auto slotPtr = builder_.CreateInBoundsGEP(
          valueType_, argsArg, {builder_.getInt64(static_cast<int64_t>(i))},
          name + ".slot");
      auto argVal = builder_.CreateLoad(valueType_, slotPtr, name);
      if (!paramTypeNames[i].empty()) {
        emit_type_check(argVal, paramTypeNames[i],
                        std::string("parameter '") + name + "'");
      }
      if (info.captured_locals.contains(name)) {
        define_var(name, make_cell_slot(name, argVal));
      } else {
        define_var(name, make_stack_slot(name, argVal));
      }
    }

    // __ARGS__: Array of overflow args (args[declaredArity..n_args)).
    // Always bound so referencing `__ARGS__` in the body never fails
    // with `undefined variable` — callers that pass exactly the
    // declared count see an empty Array (.size() == 0). The runtime
    // takes over the +1 ownership from the remaining slab slots.
    {
      auto argsArr = emit_call(
          module_->getOrInsertFunction(
              rt::args_slice_to_array, ptrTy, ptrTy,
              builder_.getInt64Ty(), builder_.getInt64Ty()),
          {argsArg,
           builder_.getInt64(static_cast<int64_t>(declaredArity)),
           nArgsArg},
          "args.arr");
      auto argsVal = make_array(argsArr);
      if (info.captured_locals.contains("__ARGS__")) {
        define_var("__ARGS__", make_cell_slot("__ARGS__", argsVal));
      } else {
        define_var("__ARGS__", make_stack_slot("__ARGS__", argsVal));
      }
    }

    // Bind free variables: extract cell pointers from closure->captures[i]
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      const auto& fv = info.free_vars[i];
      // captures_field = load ptr from closure->captures (field 2)
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
      auto capturesArr =
          builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
      // cell_slot = captures[i]
      auto cellSlotPtr = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr, {builder_.getInt64(i)}, "cell.slot");
      auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, fv + ".cell");
      // Store cell pointer into a local alloca (as Cell slot)
      llvm::IRBuilder<> entryB(entryBB, entryBB->begin());
      auto holder = entryB.CreateAlloca(ptrTy, nullptr, fv);
      builder_.CreateStore(cellPtr, holder);
      // Borrowed: the closure object owns the cell ref.
      define_var(fv, VarSlot{VarSlot::Cell, holder, /*owned=*/false});
    }

    auto bodyVal = compile(*ast.nodes[bodyIdx]);

    if (!builder_.GetInsertBlock()->getTerminator()) {
      if (!returnType.empty()) {
        emit_type_check(bodyVal, returnType, "return value");
      }
      // Run function-level defers before returning.
      if (fnMark) {
        builder_.CreateCall(
            module_->getFunction(rt::defer_run_to), {fnMark});
      }
      release_all_scopes_for_exit();
      builder_.CreateRet(bodyVal);
    }

    pop_scope();
    verifyFunction(*fn);

    // Restore outer context before emitting the closure at the caller's
    // insertion point — the cell captures come from the outer scope.
    saver.restore();
    return emit_closure_build(fn, info, paramNames.size());
  }

  // Construct a JitClosure for `fn` at the current insertion point:
  // calls `closure_new`, then fills in the captures array with cell
  // pointers from the caller's scope (each retained). Returns the
  // closure as a %Value (TAG_FUNC). Shared by compile_function and
  // compile_defer (defer uses arity=0).
  llvm::Value* emit_closure_build(llvm::Function* fn, const FuncInfo& info,
                                  size_t arity) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto n = info.free_vars.size();
    auto closurePtr = emit_call(
        module_->getOrInsertFunction(rt::closure_new, ptrTy,
                                     ptrTy, builder_.getInt64Ty(),
                                     builder_.getInt64Ty()),
        {fn, builder_.getInt64(n), builder_.getInt64(arity)}, "closure");
    if (n > 0) {
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, closurePtr, 3);
      auto capturesArr = builder_.CreateLoad(ptrTy, capturesFieldPtr);
      for (size_t i = 0; i < n; i++) {
        const auto& fv = info.free_vars[i];
        auto slot = lookup_var(fv);
        if (!slot) {
          throw std::runtime_error(
              std::format("cannot find free var '{}' in scope", fv));
        }
        if (slot->kind != VarSlot::Cell) {
          throw std::runtime_error(
              std::format("free var '{}' is not a cell", fv));
        }
        auto cellPtr = cell_ptr_of(*slot);
        auto dstSlot = builder_.CreateInBoundsGEP(
            ptrTy, capturesArr, {builder_.getInt64(i)});
        builder_.CreateStore(cellPtr, dstSlot);
        emit_cell_retain(cellPtr);  // closure owns a ref to each cell
      }
    }
    return make_func(closurePtr);
  }

  // --- Call ---

  llvm::Value* compile_call(const peg::Ast& ast) {
    using namespace peg::udl;
    auto calleeNode = ast.nodes[0];
    llvm::Value* callee = compile(*calleeNode);

    for (auto i = 1u; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];

      switch (postfix.original_tag) {
        case "ARGUMENTS"_:
          callee = compile_function_call(postfix, callee);
          break;
        case "INDEX"_:
          callee = compile_index_access(postfix, callee);
          break;
        case "DOT"_: {
          if (i + 1 < ast.nodes.size() &&
              ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
            auto method = std::string(postfix.token);
            callee = compile_method_call(method, *ast.nodes[i + 1], callee);
            i++;  // consume ARGUMENTS
          } else {
            auto name = std::string(postfix.token);
            callee = compile_property_get(callee, name);
          }
          break;
        }
        default:
          throw std::runtime_error("invalid call postfix");
      }
    }

    return callee;
  }

  // Get a property from an object (TAG_OBJECT required)
  llvm::Value* compile_property_get(llvm::Value* receiver,
                                    const std::string& name) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto tag = extract_tag(receiver);
    auto isObj =
        builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT), "is.obj");
    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "prop.ok", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "prop.err", fn);
    builder_.CreateCondBr(isObj, okBB, errBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto keyPtr = builder_.CreateGlobalString(name, ".key");

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "get.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "get.data");

    emit_call(
        module_->getOrInsertFunction(rt::object_get,
                                     builder_.getVoidTy(), ptrTy, ptrTy,
                                     ptrTy, ptrTy),
        {objPtr, keyPtr, outTag, outData});

    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  llvm::Value* compile_index_access(const peg::Ast& idxAst,
                                    llvm::Value* arr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Type check: callee must be Array
    auto tag = extract_tag(arr);
    auto isArr = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_ARRAY));

    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(ctx_, "idx.ok", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "idx.err", fn);
    builder_.CreateCondBr(isArr, okBB, errBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(okBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(arr), ptrTy);
    auto idxVal = compile(idxAst);
    auto idx = value_to_long(idxVal);

    // Allocate output slots in entry block
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "idx.out.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "idx.out.data");

    emit_call(
        module_->getOrInsertFunction(
            rt::array_get, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), ptrTy, ptrTy, builder_.getInt64Ty(),
            builder_.getInt64Ty()),
        {arrPtr, idx, outTag, outData, current_line_val(),
         current_column_val()});

    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  // Helper: check receiver tag matches expected TAG_*, else type error.
  llvm::Value* expect_tag(llvm::Value* receiver, int8_t expected,
                          const char* bb_prefix) {
    auto tag = extract_tag(receiver);
    auto fn = builder_.GetInsertBlock()->getParent();
    auto okBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".ok", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(bb_prefix) + ".err", fn);
    auto cond = builder_.CreateICmpEQ(tag, builder_.getInt8(expected));
    builder_.CreateCondBr(cond, okBB, errBB);
    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(okBB);
    return builder_.CreateIntToPtr(extract_data(receiver),
                                   llvm::PointerType::get(ctx_, 0));
  }

  llvm::Value* compile_method_call(const std::string& method,
                                   const peg::Ast& argsAst,
                                   llvm::Value* receiver) {
    if (auto* r =
            compile_builtin_method(method, argsAst, receiver)) {
      return r;
    }

    // UFCS fallback: if `method` is not a known builtin and resolves as
    // a free function in scope, `x.method(args)` becomes `method(x,
    // args)` — but only when the receiver has no user property by that
    // name. Existing methods always win (interpreter parity: Option 1
    // from the UFCS design discussion).
    bool is_builtin = known_builtin_methods().contains(method);
    const VarSlot* freeFnSlot = is_builtin ? nullptr : lookup_var(method);
    if (freeFnSlot) {
      return compile_method_or_ufcs(method, *freeFnSlot, argsAst, receiver);
    }

    // UFCS to a stdlib builtin: `x.puts()` → `puts(x)` and so on for
    // the handful of unary global builtins. These names live in
    // `is_builtin_var` but don't show up in `scopes_`, so the
    // lookup_var branch above misses them. Matches interp's UFCS
    // resolution, which consults the full env (builtins included).
    if (auto* r = try_compile_ufcs_builtin(method, argsAst, receiver)) {
      return r;
    }

    // User-defined method on Object: fetch property, call with this=receiver
    auto methodVal = compile_property_get(receiver, method);
    return compile_function_call(argsAst, methodVal, receiver);
  }

  // Emit UFCS for `x.NAME()` when NAME is a stdlib global (is_builtin_var)
  // whose arity matches `1 + argsAst.nodes.size()`. Returns nullptr when
  // the pattern doesn't match, letting the caller fall through.
  llvm::Value* try_compile_ufcs_builtin(const std::string& method,
                                        const peg::Ast& argsAst,
                                        llvm::Value* receiver) {
    if (argsAst.nodes.size() != 0) return nullptr;
    auto line = current_line_val();
    auto col = current_column_val();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    if (method == "puts" || method == "print") {
      auto rt_name = method == "puts" ? rt::puts : rt::print;
      builder_.CreateCall(module_->getFunction(rt_name),
                          {extract_tag(receiver), extract_data(receiver)});
      emit_value_release(receiver);
      return make_nil();
    }
    if (method == "assert") {
      builder_.CreateCall(
          module_->getFunction(rt::assert_),
          {extract_tag(receiver), extract_data(receiver), line, col});
      emit_value_release(receiver);
      return make_nil();
    }
    if (method == "to_long") {
      emit_type_check(receiver, "String", "to_long argument");
      auto strPtr =
          builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
      auto r = builder_.CreateCall(
          module_->getFunction(rt::to_long), {strPtr, line, col});
      emit_value_release(receiver);
      return make_long(r);
    }
    if (method == "to_string") {
      auto s = builder_.CreateCall(
          module_->getFunction(rt::value_to_display),
          {extract_tag(receiver), extract_data(receiver)});
      emit_value_release(receiver);
      return make_string(s);
    }
    if (method == "type_of") {
      auto s = builder_.CreateCall(module_->getFunction(rt::type_of),
                                   {extract_tag(receiver)});
      emit_value_release(receiver);
      return make_string(s);
    }
    return nullptr;
  }

  // Emit the runtime dispatch between a user method (receiver owns a
  // property `method`) and UFCS (call the free variable `method` with
  // `receiver` as the first argument). Args are compiled once up front
  // so side-effects don't duplicate across branches.
  llvm::Value* compile_method_or_ufcs(const std::string& method,
                                      const VarSlot& freeFnSlot,
                                      const peg::Ast& argsAst,
                                      llvm::Value* receiver) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    std::vector<llvm::Value*> userArgs;
    userArgs.reserve(argsAst.nodes.size());
    for (auto& argNode : argsAst.nodes) {
      userArgs.push_back(compile(*argNode));
    }

    auto tag = extract_tag(receiver);
    auto isObj = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_OBJECT));

    auto checkBB = llvm::BasicBlock::Create(ctx_, "ufcs.check", fn);
    auto methodBB = llvm::BasicBlock::Create(ctx_, "ufcs.method", fn);
    auto ufcsBB = llvm::BasicBlock::Create(ctx_, "ufcs.free", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ufcs.merge", fn);

    // Non-Object receivers have no user properties, so UFCS always wins.
    builder_.CreateCondBr(isObj, checkBB, ufcsBB);

    builder_.SetInsertPoint(checkBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto keyPtr = get_or_create_global_str(method, ".mkey");
    auto hasProp = emit_object_has(objPtr, keyPtr);
    builder_.CreateCondBr(hasProp, methodBB, ufcsBB);

    builder_.SetInsertPoint(methodBB);
    auto methodVal = compile_property_get(receiver, method);
    auto methodRes =
        compile_function_call_raw(methodVal, receiver, userArgs);
    auto methodEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(ufcsBB);
    auto freeFn = load_slot(freeFnSlot, method);
    std::vector<llvm::Value*> ufcsArgs;
    ufcsArgs.reserve(userArgs.size() + 1);
    ufcsArgs.push_back(receiver);
    for (auto* a : userArgs) ufcsArgs.push_back(a);
    auto ufcsRes = compile_function_call_raw(freeFn, nullptr, ufcsArgs);
    auto ufcsEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "ufcs.res");
    phi->addIncoming(methodRes, methodEndBB);
    phi->addIncoming(ufcsRes, ufcsEndBB);
    return phi;
  }

  // Raw call emission: caller has already compiled the user args.
  // Used by UFCS so a side-effectful arg expression is only lowered
  // once even though both branches need the resulting Values.
  //
  // ABI: Value fn(ptr cls, Value this, i64 n_args, ptr args). The
  // `args` slab is a stack alloca owned by this call; each entry
  // carries a +1 that the callee transfers into its param slot or
  // into `__ARGS__`.
  llvm::Value* compile_function_call_raw(
      llvm::Value* callee, llvm::Value* thisVal,
      llvm::ArrayRef<llvm::Value*> userArgs) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    auto tag = extract_tag(callee);
    auto isFunc = builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_FUNC));

    auto fn = builder_.GetInsertBlock()->getParent();
    auto callBB = llvm::BasicBlock::Create(ctx_, "call.ok", fn);
    auto errorBB = llvm::BasicBlock::Create(ctx_, "call.error", fn);
    builder_.CreateCondBr(isFunc, callBB, errorBB);

    builder_.SetInsertPoint(errorBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(callBB);
    auto clsPtr = builder_.CreateIntToPtr(extract_data(callee), ptrTy);
    auto fnFieldPtr =
        builder_.CreateStructGEP(closureType_, clsPtr, 1, "fn.ptr");
    auto fnPtr = builder_.CreateLoad(ptrTy, fnFieldPtr, "fn");

    // Build the args slab in the entry block (hoisted) so repeated
    // calls within a loop don't grow the stack frame per iteration.
    llvm::Value* argsPtr;
    if (userArgs.empty()) {
      argsPtr = llvm::ConstantPointerNull::get(ptrTy);
    } else {
      llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                               fn->getEntryBlock().begin());
      argsPtr = entryB.CreateAlloca(
          valueType_,
          builder_.getInt64(static_cast<int64_t>(userArgs.size())),
          "call.args");
      for (size_t i = 0; i < userArgs.size(); i++) {
        auto slot = builder_.CreateInBoundsGEP(
            valueType_, argsPtr,
            {builder_.getInt64(static_cast<int64_t>(i))});
        builder_.CreateStore(userArgs[i], slot);
      }
    }

    auto calleeType = llvm::FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);
    std::vector<llvm::Value*> args = {
        clsPtr,
        thisVal ? thisVal : make_nil(),
        builder_.getInt64(static_cast<int64_t>(userArgs.size())),
        argsPtr};
    return emit_call(calleeType, fnPtr, args, "call.result");
  }

  llvm::Value* compile_function_call(const peg::Ast& argsAst,
                                     llvm::Value* callee,
                                     llvm::Value* thisVal = nullptr) {
    std::vector<llvm::Value*> userArgs;
    userArgs.reserve(argsAst.nodes.size());
    for (auto& argNode : argsAst.nodes) {
      userArgs.push_back(compile(*argNode));
    }
    return compile_function_call_raw(callee, thisVal, userArgs);
  }

  // Handle a CALL, intercepting stdlib globals in the leading `name(args)`
  // so they compile directly instead of resolving as variables.
  llvm::Value* compile_call_with_builtins(const peg::Ast& ast) {
    using namespace peg::udl;
    auto calleeNode = ast.nodes[0];

    llvm::Value* start = nullptr;
    size_t next_idx = 1;

    if (calleeNode->tag == "IDENTIFIER"_ && ast.nodes.size() >= 2 &&
        ast.nodes[1]->original_tag == "ARGUMENTS"_) {
      auto name = std::string(calleeNode->token);
      start = try_compile_stdlib_global(name, *ast.nodes[1], ast);
      if (start) next_idx = 2;
    }

    if (!start && calleeNode->tag == "IDENTIFIER"_ &&
        ast.nodes.size() >= 2 &&
        ast.nodes[1]->original_tag == "DOT"_) {
      auto ns = calleeNode->token;
      auto prop = ast.nodes[1]->token;
      if (ast.nodes.size() >= 3 &&
          ast.nodes[2]->original_tag == "ARGUMENTS"_) {
        start = try_compile_stdlib_namespace(ns, prop, *ast.nodes[2], ast);
        if (start) next_idx = 3;
      }
      if (!start) {
        start = try_compile_stdlib_namespace_property(ns, prop);
        if (start) next_idx = 2;
      }
    }

    if (!start) return compile_call(ast);

    // Continue with remaining postfixes (matching compile_call's loop).
    llvm::Value* callee = start;
    for (auto i = next_idx; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];
      switch (postfix.original_tag) {
        case "ARGUMENTS"_:
          callee = compile_function_call(postfix, callee);
          break;
        case "INDEX"_:
          callee = compile_index_access(postfix, callee);
          break;
        case "DOT"_: {
          if (i + 1 < ast.nodes.size() &&
              ast.nodes[i + 1]->original_tag == "ARGUMENTS"_) {
            auto method = std::string(postfix.token);
            callee = compile_method_call(method, *ast.nodes[i + 1], callee);
            i++;
          } else {
            auto name = std::string(postfix.token);
            callee = compile_property_get(callee, name);
          }
          break;
        }
        default:
          throw std::runtime_error("invalid call postfix");
      }
    }
    return callee;
  }

  // --- Debugger ---

  llvm::Value* compile_debugger(const peg::Ast& ast) {
    if (!debug_enabled_) return make_nil();
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto pathPtr = builder_.CreateGlobalString(ast.path, ".dbgpath");
    emit_call(
        module_->getOrInsertFunction(
            rt::debugger_break, builder_.getVoidTy(), ptrTy,
            builder_.getInt64Ty(), builder_.getInt64Ty()),
        {pathPtr, builder_.getInt64(static_cast<int64_t>(ast.line)),
         builder_.getInt64(static_cast<int64_t>(ast.column))});
    return make_nil();
  }

  // --- Return ---

  llvm::Value* compile_return(const peg::Ast& ast) {
    llvm::Value* val;
    if (ast.nodes.empty()) {
      val = make_nil();
    } else {
      val = compile(*ast.nodes[0]);
    }
    if (!current_return_type_.empty()) {
      emit_type_check(val, current_return_type_, "return value");
    }
    // Run any defers registered in this function's scopes before
    // returning to the caller. No invoke needed: we're exiting.
    if (current_fn_defer_mark_) {
      builder_.CreateCall(
          module_->getFunction(rt::defer_run_to),
          {current_fn_defer_mark_});
    }
    release_all_scopes_for_exit();
    builder_.CreateRet(val);
    return val;
  }

  // `defer { BODY }` — compiles BODY as a 0-param closure and pushes
  // it onto the global defer stack. The closure captures any outer
  // variables it references, via the same cell mechanism as regular
  // fn literals. The surrounding scope's exit path (fall-through,
  // return, throw) calls `culebra_runtime_defer_run_to` to unwind the
  // stack back to that scope's mark.
  llvm::Value* compile_defer(const peg::Ast& ast) {
    using namespace llvm;
    auto ptrTy = PointerType::get(ctx_, 0);

    auto infoIt = func_info_.find(&ast);
    if (infoIt == func_info_.end()) {
      throw std::runtime_error("missing func_info for defer");
    }
    const FuncInfo& info = infoIt->second;

    // Same uniform ABI as compile_function; defer thunks ignore
    // n_args/args (callers always pass 0/null).
    auto fnType = FunctionType::get(
        valueType_, {ptrTy, valueType_, builder_.getInt64Ty(), ptrTy},
        false);
    auto fnName = std::format("__culebra_defer_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);
    if (info.has_eh) {
      fn->setPersonalityFn(get_personality_fn());
    }

    auto argIt = fn->arg_begin();
    argIt->setName("__cls__");
    ++argIt;
    argIt->setName("this");
    ++argIt;
    argIt->setName("n_args");
    ++argIt;
    argIt->setName("args");

    CompilerStateSaver saver(*this);
    current_info_ = &info;

    auto entryBB = BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entryBB);
    push_scope();

    argIt = fn->arg_begin();
    auto clsArg = &*argIt++;
    current_closure_arg_ = clsArg;

    // Bind free variables: load cell pointers from closure->captures[i].
    for (size_t i = 0; i < info.free_vars.size(); i++) {
      const auto& fv = info.free_vars[i];
      auto capturesFieldPtr =
          builder_.CreateStructGEP(closureType_, clsArg, 3, "caps.ptr");
      auto capturesArr =
          builder_.CreateLoad(ptrTy, capturesFieldPtr, "caps");
      auto cellSlotPtr = builder_.CreateInBoundsGEP(
          ptrTy, capturesArr, {builder_.getInt64(i)}, "cell.slot");
      auto cellPtr = builder_.CreateLoad(ptrTy, cellSlotPtr, fv + ".cell");
      llvm::IRBuilder<> entryB(entryBB, entryBB->begin());
      auto holder = entryB.CreateAlloca(ptrTy, nullptr, fv);
      builder_.CreateStore(cellPtr, holder);
      // Borrowed: the defer closure object owns the cell ref.
      define_var(fv, VarSlot{VarSlot::Cell, holder, /*owned=*/false});
    }

    compile(*ast.nodes[0]);
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateRet(make_nil());
    }

    pop_scope();
    verifyFunction(*fn);

    saver.restore();

    // Build the 0-arg closure and push it onto the defer stack. The
    // runtime retains it; we drop the local ref after.
    auto closureVal = emit_closure_build(fn, info, 0);
    builder_.CreateCall(
        module_->getFunction(rt::defer_push),
        {extract_tag(closureVal), extract_data(closureVal)});
    emit_value_release(closureVal);
    return make_nil();
  }

  // `throw expr` — evaluates expr, hands its tag/data to the runtime
  // which raises a CulebraException. Control never falls through.
  llvm::Value* compile_throw(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[0]);
    emit_call(module_->getFunction(rt::throw_),
              {extract_tag(val), extract_data(val)});
    builder_.CreateUnreachable();
    // Fresh dead block so any trailing code has a valid insert point.
    auto deadBB = llvm::BasicBlock::Create(
        ctx_, "throw.dead", builder_.GetInsertBlock()->getParent());
    builder_.SetInsertPoint(deadBB);
    return make_nil();
  }

  // `try BODY catch name HANDLER` — evaluates BODY. If BODY throws a
  // CulebraException, HANDLER runs with `name` bound to the caught
  // value. The whole form is an expression yielding whichever block
  // ran last. `return` inside BODY / HANDLER returns from the
  // enclosing user function (standard LLVM ret semantics).
  llvm::Value* compile_try(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto fn = builder_.GetInsertBlock()->getParent();

    auto tryBB = llvm::BasicBlock::Create(ctx_, "try.body", fn);
    auto lpadBB = llvm::BasicBlock::Create(ctx_, "try.lpad", fn);
    auto catchBB = llvm::BasicBlock::Create(ctx_, "try.catch", fn);
    auto endBB = llvm::BasicBlock::Create(ctx_, "try.end", fn);

    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto resultSlot = entryB.CreateAlloca(valueType_, nullptr, "try.result");
    builder_.CreateStore(make_nil(), resultSlot);
    auto caughtSlot = entryB.CreateAlloca(valueType_, nullptr, "try.caught");

    builder_.CreateBr(tryBB);

    // --- Try body: compile with current_lpad_ set to lpadBB ---
    builder_.SetInsertPoint(tryBB);
    auto savedLpad = current_lpad_;
    current_lpad_ = lpadBB;
    push_scope();
    auto tryVal = compile(*ast.nodes[0]);
    pop_scope();
    current_lpad_ = savedLpad;
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateStore(tryVal, resultSlot);
      builder_.CreateBr(endBB);
    }

    // --- Landing pad: catch-all. If it's a Culebra user throw (as
    // signaled by the `culebra_is_throw` global), read the carried
    // tag/data and proceed. Otherwise resume unwinding so runtime
    // errors (std::runtime_error etc.) propagate past `try/catch`. ---
    builder_.SetInsertPoint(lpadBB);
    auto lpadTy = llvm::StructType::get(ptrTy, builder_.getInt32Ty());
    auto lpad = builder_.CreateLandingPad(lpadTy, 1, "exc");
    lpad->addClause(llvm::ConstantPointerNull::get(ptrTy));  // catch-all

    // Check the is-throw flag
    auto flagGlobal = module_->getOrInsertGlobal(
        "culebra_is_throw", builder_.getInt8Ty());
    auto flagVal = builder_.CreateLoad(builder_.getInt8Ty(), flagGlobal,
                                       "is_throw");
    auto isOurs = builder_.CreateICmpNE(flagVal, builder_.getInt8(0));
    auto handleBB = llvm::BasicBlock::Create(ctx_, "try.handle", fn);
    auto notOursBB = llvm::BasicBlock::Create(ctx_, "try.notours", fn);
    builder_.CreateCondBr(isOurs, handleBB, notOursBB);

    // Not a Culebra user throw — let it keep unwinding.
    builder_.SetInsertPoint(notOursBB);
    builder_.CreateResume(lpad);

    builder_.SetInsertPoint(handleBB);
    // Clear the flag and consume the exception.
    builder_.CreateStore(builder_.getInt8(0), flagGlobal);
    auto excPtr = builder_.CreateExtractValue(lpad, {0}, "exc.ptr");
    auto beginCatch = module_->getOrInsertFunction(
        "__cxa_begin_catch", ptrTy, ptrTy);
    builder_.CreateCall(beginCatch, {excPtr});
    auto endCatchFn = module_->getOrInsertFunction("__cxa_end_catch",
                                                   builder_.getVoidTy());
    builder_.CreateCall(endCatchFn);
    // Read the thrown Value from the globals into caughtSlot.
    auto tagGlobal = module_->getOrInsertGlobal(
        "culebra_thrown_tag", builder_.getInt8Ty());
    auto dataGlobal = module_->getOrInsertGlobal(
        "culebra_thrown_data", builder_.getInt64Ty());
    auto tagVal = builder_.CreateLoad(builder_.getInt8Ty(), tagGlobal,
                                      "exc.tag");
    auto dataVal = builder_.CreateLoad(builder_.getInt64Ty(), dataGlobal,
                                       "exc.data");
    llvm::Value* caught = llvm::UndefValue::get(valueType_);
    caught = builder_.CreateInsertValue(caught, tagVal, {0});
    caught = builder_.CreateInsertValue(caught, dataVal, {1});
    builder_.CreateStore(caught, caughtSlot);
    builder_.CreateBr(catchBB);

    // --- Catch body: bind thrown value as `name` ---
    builder_.SetInsertPoint(catchBB);
    push_scope();
    auto caughtName = std::string(ast.nodes[1]->token);
    auto caughtValue = builder_.CreateLoad(valueType_, caughtSlot);
    bool captured = current_info_ &&
                    current_info_->captured_locals.contains(caughtName);
    auto slot = captured ? make_cell_slot(caughtName, caughtValue)
                         : make_stack_slot(caughtName, caughtValue);
    define_var(caughtName, slot);
    auto catchVal = compile(*ast.nodes[2]);
    pop_scope();
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateStore(catchVal, resultSlot);
      builder_.CreateBr(endBB);
    }

    // --- End ---
    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(valueType_, resultSlot, "try.res");
  }

  // --- Execution ---

  static void exec(std::unique_ptr<llvm::LLVMContext> ctx,
                   std::unique_ptr<llvm::Module> mod) {
    using namespace llvm;

    auto jit = cantFail(orc::LLJITBuilder().create());

    auto& jd = jit->getMainJITDylib();
    auto gen = cantFail(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix()));
    jd.addGenerator(std::move(gen));

    orc::ThreadSafeContext tsctx(std::move(ctx));
    cantFail(jit->addIRModule(
        orc::ThreadSafeModule(std::move(mod), std::move(tsctx))));

    auto mainFn =
        cantFail(jit->lookup("__culebra_main")).toPtr<void (*)()>();
    try {
      mainFn();
    } catch (const CulebraException& e) {
      // Balance the retain performed in culebra_runtime_throw.
      _culebra_value_release_impl(e.tag, e.data);
      auto s = _culebra_value_to_str_impl(e.tag, e.data);
      // Run (best-effort) any top-level defers the uncaught throw
      // skipped, so the global defer stack is drained between runs.
      try {
        culebra_runtime_defer_run_to(0);
      } catch (...) {}
      throw std::runtime_error(std::format("uncaught: {}", s));
    }
  }
};

inline llvm::Value* JIT::compile_builtin_method(const std::string& method,
                                                const peg::Ast& argsAst,
                                                llvm::Value* receiver) {
  // Fast path: bail out immediately on unknown method names before doing any
  // setup. Keeps user-defined method calls free of stdlib overhead.
  if (!known_builtin_methods().contains(method)) return nullptr;

  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto tag = extract_tag(receiver);
  auto fn = builder_.GetInsertBlock()->getParent();

  // .size() — works on Array, Object, String
  if (method == "size" && argsAst.nodes.size() == 0) {
    auto arrBB = llvm::BasicBlock::Create(ctx_, "size.arr", fn);
    auto objBB = llvm::BasicBlock::Create(ctx_, "size.obj", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "size.str", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "size.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "size.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 3);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto arrSize = emit_call(
        module_->getFunction(rt::array_size), {arrPtr}, "asz");
    auto arrSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    auto objPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto objSize = emit_call(
        module_->getFunction(rt::object_size), {objPtr}, "osz");
    auto objSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto strPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto strSize = emit_call(
        module_->getFunction(rt::str_size), {strPtr}, "ssz");
    auto strSizeBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt64Ty(), 3, "sz");
    phi->addIncoming(arrSize, arrSizeBB);
    phi->addIncoming(objSize, objSizeBB);
    phi->addIncoming(strSize, strSizeBB);
    return make_long(phi);
  }

  // --- Array methods ---

  if (method == "push" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "push");
    auto val = compile(*argsAst.nodes[0]);
    emit_call(module_->getFunction(rt::array_push),
                        {arrPtr, extract_tag(val), extract_data(val)});
    return make_nil();
  }

  if (method == "pop" && argsAst.nodes.size() == 0) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "pop");
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto outTag = entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "pop.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "pop.data");
    emit_call(module_->getFunction(rt::array_pop),
                        {arrPtr, outTag, outData});
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if (method == "reverse" && argsAst.nodes.size() == 0) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "rev");
    emit_call(module_->getFunction(rt::array_reverse),
                        {arrPtr});
    return make_nil();
  }

  // slice works on Array and String
  if (method == "slice" && argsAst.nodes.size() == 2) {
    auto start = value_to_long(compile(*argsAst.nodes[0]));
    auto end = value_to_long(compile(*argsAst.nodes[1]));

    auto arrBB = llvm::BasicBlock::Create(ctx_, "sl.arr", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "sl.str", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "sl.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "sl.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 2);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto newArr = emit_call(
        module_->getFunction(rt::array_slice2),
        {arrPtr, start, end});
    auto arrVal = make_array(newArr);
    auto arrBBEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto strPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto newStr = emit_call(
        module_->getFunction(rt::str_slice),
        {strPtr, start, end});
    auto strVal = make_string(newStr);
    auto strBBEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "sl");
    phi->addIncoming(arrVal, arrBBEnd);
    phi->addIncoming(strVal, strBBEnd);
    return phi;
  }

  if (method == "join" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "join");
    auto sep = compile(*argsAst.nodes[0]);
    emit_type_check(sep, "String", "join separator");
    auto sepPtr = builder_.CreateIntToPtr(extract_data(sep), ptrTy);
    auto s = emit_call(
        module_->getFunction(rt::array_join), {arrPtr, sepPtr});
    emit_value_release(sep);
    return make_string(s);
  }

  if (method == "index_of" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "iof");
    auto v = compile(*argsAst.nodes[0]);
    auto idx = emit_call(
        module_->getFunction(rt::array_index_of),
        {arrPtr, extract_tag(v), extract_data(v)});
    emit_value_release(v);
    return make_long(idx);
  }

  // contains works on Array and String (different arg types).
  if (method == "contains" && argsAst.nodes.size() == 1) {
    auto arrBB = llvm::BasicBlock::Create(ctx_, "ct.arr", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "ct.str", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "ct.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ct.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errBB, 2);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto v = compile(*argsAst.nodes[0]);
    auto arrFound = emit_call(
        module_->getFunction(rt::array_contains),
        {arrPtr, extract_tag(v), extract_data(v)});
    emit_value_release(v);
    auto arrBoolVal = make_bool(arrFound);
    auto arrEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    auto strPtr = builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto sub = compile(*argsAst.nodes[0]);
    emit_type_check(sub, "String", "contains argument");
    auto subPtr = builder_.CreateIntToPtr(extract_data(sub), ptrTy);
    auto strFound = emit_call(
        module_->getFunction(rt::str_contains),
        {strPtr, subPtr});
    emit_value_release(sub);
    auto strBoolVal = make_bool(strFound);
    auto strEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2, "ct");
    phi->addIncoming(arrBoolVal, arrEnd);
    phi->addIncoming(strBoolVal, strEnd);
    return phi;
  }

  // --- String methods ---

  if (method == "upper" && argsAst.nodes.size() == 0) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "up");
    auto s = emit_call(
        module_->getFunction(rt::str_upper), {strPtr});
    return make_string(s);
  }

  if (method == "lower" && argsAst.nodes.size() == 0) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "lo");
    auto s = emit_call(
        module_->getFunction(rt::str_lower), {strPtr});
    return make_string(s);
  }

  if (method == "trim" && argsAst.nodes.size() == 0) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "tr");
    auto s = emit_call(
        module_->getFunction(rt::str_trim), {strPtr});
    return make_string(s);
  }

  if (method == "split" && argsAst.nodes.size() == 1) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "sp");
    auto sep = compile(*argsAst.nodes[0]);
    emit_type_check(sep, "String", "split separator");
    auto sepPtr = builder_.CreateIntToPtr(extract_data(sep), ptrTy);
    auto arr = emit_call(
        module_->getFunction(rt::str_split), {strPtr, sepPtr});
    emit_value_release(sep);
    return make_array(arr);
  }

  if (method == "starts_with" && argsAst.nodes.size() == 1) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "sw");
    auto p = compile(*argsAst.nodes[0]);
    emit_type_check(p, "String", "starts_with argument");
    auto pPtr = builder_.CreateIntToPtr(extract_data(p), ptrTy);
    auto r = emit_call(
        module_->getFunction(rt::str_starts_with),
        {strPtr, pPtr});
    emit_value_release(p);
    return make_bool(r);
  }

  if (method == "ends_with" && argsAst.nodes.size() == 1) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "ew");
    auto p = compile(*argsAst.nodes[0]);
    emit_type_check(p, "String", "ends_with argument");
    auto pPtr = builder_.CreateIntToPtr(extract_data(p), ptrTy);
    auto r = emit_call(
        module_->getFunction(rt::str_ends_with),
        {strPtr, pPtr});
    emit_value_release(p);
    return make_bool(r);
  }

  // --- Object methods ---

  if (method == "keys" && argsAst.nodes.size() == 0) {
    auto objPtr = expect_tag(receiver, TAG_OBJECT, "keys");
    auto arr = emit_call(
        module_->getFunction(rt::object_keys), {objPtr});
    return make_array(arr);
  }

  if (method == "has" && argsAst.nodes.size() == 1) {
    auto objPtr = expect_tag(receiver, TAG_OBJECT, "has");
    auto key = compile(*argsAst.nodes[0]);
    emit_type_check(key, "String", "has argument");
    auto keyPtr = builder_.CreateIntToPtr(extract_data(key), ptrTy);
    auto r = emit_call(
        module_->getFunction(rt::object_has), {objPtr, keyPtr});
    emit_value_release(key);
    return make_bool(r);
  }

  if (method == "remove" && argsAst.nodes.size() == 1) {
    auto objPtr = expect_tag(receiver, TAG_OBJECT, "rm");
    auto key = compile(*argsAst.nodes[0]);
    emit_type_check(key, "String", "remove argument");
    auto keyPtr = builder_.CreateIntToPtr(extract_data(key), ptrTy);
    emit_call(module_->getFunction(rt::object_remove),
                        {objPtr, keyPtr});
    emit_value_release(key);
    return make_nil();
  }

  // --- Higher-order methods (§17.2 Array methods + §17.5 Iterator) ---
  //
  // Each higher-order method tag-dispatches on the receiver: Array
  // receivers keep the eager path (returns a new Array, same as before);
  // iterator-protocol Objects route to the iterator runtime (walks via
  // `next()`, returns Array / Long / Bool / new iterator as appropriate).

  auto ho_line = current_line_val();
  auto ho_col = current_column_val();

  // Emit Array-or-Iterator dispatch with the supplied eager/lazy body
  // emitters. Bodies produce the %Value result for their branch.
  auto dispatch_arr_iter =
      [&](const char* label,
          std::function<llvm::Value*(llvm::Value* arrPtr)> eager,
          std::function<llvm::Value*(llvm::Value* iterTag,
                                     llvm::Value* iterData)>
              lazy) -> llvm::Value* {
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto arrBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".arr", fn);
    auto objBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".obj", fn);
    auto errBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".err", fn);
    auto mergeBB = llvm::BasicBlock::Create(
        ctx_, std::string(label) + ".merge", fn);
    auto sw = builder_.CreateSwitch(t, errBB, 2);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto eagerRes = eager(arrPtr);
    auto eagerEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    auto lazyRes = lazy(t, d);
    auto lazyEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 2,
                                  std::string(label) + ".res");
    phi->addIncoming(eagerRes, eagerEnd);
    phi->addIncoming(lazyRes, lazyEnd);
    return phi;
  };

  if (method == "map" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "map",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_map),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_map),
                               {it, id, ft, fd});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  if (method == "filter" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "filter",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_filter),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_filter),
                               {it, id, ft, fd});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  if (method == "for_each" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "foreach",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_for_each),
                    {arrPtr, ft, fd, ho_line, ho_col});
          return make_nil();
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_for_each),
                    {it, id, ft, fd, ho_line, ho_col});
          return make_nil();
        });
    emit_value_release(f);
    return result;
  }

  if (method == "reduce" && argsAst.nodes.size() == 2) {
    auto init = compile(*argsAst.nodes[0]);
    auto f = compile(*argsAst.nodes[1]);
    auto it_tag = extract_tag(init);
    auto it_data = extract_data(init);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "red.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "red.data");
    dispatch_arr_iter(
        "reduce",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_reduce),
                    {arrPtr, it_tag, it_data, ft, fd, ho_line, ho_col,
                     outTag, outData});
          return make_nil();  // ignored; real result via out-params
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_reduce),
                    {it, id, it_tag, it_data, ft, fd, ho_line, ho_col,
                     outTag, outData});
          return make_nil();
        });
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if (method == "find" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "find.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "find.data");
    dispatch_arr_iter(
        "find",
        [&](llvm::Value* arrPtr) {
          emit_call(module_->getFunction(rt::array_find),
                    {arrPtr, ft, fd, ho_line, ho_col, outTag, outData});
          return make_nil();
        },
        [&](llvm::Value* it, llvm::Value* id) {
          emit_call(module_->getFunction(rt::iter_find),
                    {it, id, ft, fd, ho_line, ho_col, outTag, outData});
          return make_nil();
        });
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if ((method == "any" || method == "all") && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto arr_rt = method == "any" ? rt::array_any : rt::array_all;
    auto iter_rt = method == "any" ? rt::iter_any : rt::iter_all;
    auto result = dispatch_arr_iter(
        method.c_str(),
        [&](llvm::Value* arrPtr) {
          auto r = emit_call(module_->getFunction(arr_rt),
                             {arrPtr, ft, fd, ho_line, ho_col});
          return make_bool(
              builder_.CreateICmpNE(r, builder_.getInt64(0)));
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto r = emit_call(module_->getFunction(iter_rt),
                             {it, id, ft, fd, ho_line, ho_col});
          return make_bool(
              builder_.CreateICmpNE(r, builder_.getInt64(0)));
        });
    emit_value_release(f);
    return result;
  }

  if (method == "flat_map" && argsAst.nodes.size() == 1) {
    auto f = compile(*argsAst.nodes[0]);
    auto ft = extract_tag(f);
    auto fd = extract_data(f);
    auto result = dispatch_arr_iter(
        "flat_map",
        [&](llvm::Value* arrPtr) {
          auto out = emit_call(
              module_->getFunction(rt::array_flat_map),
              {arrPtr, ft, fd, ho_line, ho_col});
          return make_array(out);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto out = emit_call(module_->getFunction(rt::iter_flat_map),
                               {it, id, ft, fd, ho_line, ho_col});
          return make_object(out);
        });
    emit_value_release(f);
    return result;
  }

  // sum / product / min / max — integer aggregates, same shape for
  // Array and Iterator receivers. Non-Long elements raise a type
  // error; min/max additionally throw on empty input.
  if ((method == "sum" || method == "product" ||
       method == "min" || method == "max") &&
      argsAst.nodes.size() == 0) {
    const char* arr_rt_name;
    const char* iter_rt_name;
    if (method == "sum") {
      arr_rt_name = rt::array_sum;
      iter_rt_name = rt::iter_sum;
    } else if (method == "product") {
      arr_rt_name = rt::array_product;
      iter_rt_name = rt::iter_product;
    } else if (method == "min") {
      arr_rt_name = rt::array_min;
      iter_rt_name = rt::iter_min;
    } else {
      arr_rt_name = rt::array_max;
      iter_rt_name = rt::iter_max;
    }
    return dispatch_arr_iter(
        method.c_str(),
        [&](llvm::Value* arrPtr) {
          auto n = emit_call(module_->getFunction(arr_rt_name),
                             {arrPtr, ho_line, ho_col});
          return make_long(n);
        },
        [&](llvm::Value* it, llvm::Value* id) {
          auto n = emit_call(module_->getFunction(iter_rt_name),
                             {it, id, ho_line, ho_col});
          return make_long(n);
        });
  }

  // --- Iterator-only methods (no eager Array equivalent) ---
  // For now these error on Array receivers. Phase B2 may add eager
  // fallbacks so `arr.take(n)` becomes `arr.slice(0, n)` natively.

  // Iterator-only methods — require an iterator-protocol Object
  // receiver (Array users call `.iter()` first, matching interp).
  if (method == "collect" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "collect");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_collect), {t, d});
    return make_array(out);
  }

  if (method == "count" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "count");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto n = emit_call(module_->getFunction(rt::iter_count), {t, d});
    return make_long(n);
  }

  if (method == "take" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "take");
    auto n = value_to_long(compile(*argsAst.nodes[0]));
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_take), {t, d, n});
    return make_object(out);
  }

  if (method == "skip" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "skip");
    auto n = value_to_long(compile(*argsAst.nodes[0]));
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_skip), {t, d, n});
    return make_object(out);
  }

  if (method == "take_while" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "take_while");
    auto f = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_take_while),
                         {t, d, extract_tag(f), extract_data(f)});
    emit_value_release(f);
    return make_object(out);
  }

  if (method == "enumerate" && argsAst.nodes.size() == 0) {
    expect_tag(receiver, TAG_OBJECT, "enumerate");
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out =
        emit_call(module_->getFunction(rt::iter_enumerate), {t, d});
    return make_object(out);
  }

  if (method == "chain" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "chain");
    auto other = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_chain),
                         {t, d, extract_tag(other),
                          extract_data(other), ho_line, ho_col});
    emit_value_release(other);
    return make_object(out);
  }

  if (method == "zip" && argsAst.nodes.size() == 1) {
    expect_tag(receiver, TAG_OBJECT, "zip");
    auto other = compile(*argsAst.nodes[0]);
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto out = emit_call(module_->getFunction(rt::iter_zip),
                         {t, d, extract_tag(other),
                          extract_data(other), ho_line, ho_col});
    emit_value_release(other);
    return make_object(out);
  }

  if (method == "code_points" && argsAst.nodes.size() == 0) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "code_points");
    auto out =
        emit_call(module_->getFunction(rt::str_code_points), {strPtr});
    return make_object(out);
  }

  if (method == "graphemes" && argsAst.nodes.size() == 0) {
    auto strPtr = expect_tag(receiver, TAG_STRING, "graphemes");
    auto out =
        emit_call(module_->getFunction(rt::str_graphemes), {strPtr});
    return make_object(out);
  }

  if (method == "iter" && argsAst.nodes.size() == 0) {
    // Tag-dispatch on receiver: Array → array_iter, Object → if own
    // iter call it, else object_iter (keys); String → str's iter
    // (one-scalar walk via code_points analog). For simplicity here
    // we route Array/Object/String to dedicated runtime helpers.
    auto t = extract_tag(receiver);
    auto d = extract_data(receiver);
    auto arrBB = llvm::BasicBlock::Create(ctx_, "iter.arr", fn);
    auto objBB = llvm::BasicBlock::Create(ctx_, "iter.obj", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "iter.str", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "iter.err", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "iter.merge", fn);
    auto sw = builder_.CreateSwitch(t, errBB, 3);
    sw->addCase(builder_.getInt8(TAG_ARRAY), arrBB);
    sw->addCase(builder_.getInt8(TAG_OBJECT), objBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);

    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(arrBB);
    auto arrPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto arrIter = emit_call(module_->getFunction(rt::array_iter),
                             {arrPtr});
    auto arrVal = make_object(arrIter);
    auto arrEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(objBB);
    // Object.iter() yields keys (matches interp's builtin).
    auto objPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto objIter = emit_call(module_->getFunction(rt::object_iter),
                             {objPtr});
    auto objVal = make_object(objIter);
    auto objEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(strBB);
    // String.iter() yields 1-scalar substrings — same underlying
    // walker as `for c in s`. For the iterator-protocol shape we
    // reuse code_points' runtime but wrap codepoints back as 1-char
    // Strings... simpler: reuse code_points for now since
    // test_iter.cul exercises `for c in s` natively, not s.iter().
    auto strPtr = builder_.CreateIntToPtr(d, ptrTy);
    auto strIter = emit_call(module_->getFunction(rt::str_code_points),
                             {strPtr});
    auto strVal = make_object(strIter);
    auto strEnd = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(valueType_, 3, "iter.res");
    phi->addIncoming(arrVal, arrEnd);
    phi->addIncoming(objVal, objEnd);
    phi->addIncoming(strVal, strEnd);
    return phi;
  }

  if (method == "sort_by" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "sort_by");
    auto f = compile(*argsAst.nodes[0]);
    emit_call(
        module_->getFunction(rt::array_sort_by),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_nil();
  }

  return nullptr;
}

}  // namespace culebra

#include <stdlib_jit.h>

#endif  // CULEBRA_JIT_ENABLED
