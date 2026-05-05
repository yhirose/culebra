#pragma once

#ifdef CULEBRA_JIT_ENABLED

#include <parser.h>

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
#include "llvm/Support/TargetSelect.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

struct JitObjectEntry {
  JitValue value;
  bool mut;
};

// All refcounted heap types share the same first field: i64 refcount.
// This uniform layout lets the cycle collector read/write refcounts without
// per-type dispatch.

struct JitObject {
  int64_t refcount;
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

// --- Cycle collector ---
//
// Python-style mark-and-sweep: tracks all refcounted heap objects, runs
// periodically and on program exit.

inline void _culebra_value_release_impl(int8_t tag, int64_t data);
inline void _culebra_cell_release(JitCell* c);

// Tag values stored in the tracker. Values 3/5/6 match Value::Type for
// Function/Array/Object. 100 distinguishes cells (not a Value type).
static constexpr int8_t GC_TAG_FUNC = 3;
static constexpr int8_t GC_TAG_ARRAY = 5;
static constexpr int8_t GC_TAG_OBJECT = 6;
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
    case 0:
      return "nil";
    case 1:
      return data ? "true" : "false";
    case 2:
      return std::to_string(data);
    case 3:
      return "[function]";
    case 4: {
      std::string s = "'";
      s += reinterpret_cast<const char*>(data);
      s += "'";
      return s;
    }
    case 5: {
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
    case 6: {
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

inline const char* _culebra_heap_str(const std::string& s) {
  auto buf = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return buf;
}

// Same-tag equality matching the interpreter's operator==. Strings compare by
// contents; reference types (func/array/object) by identity.
inline bool _culebra_value_equal(int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  if (t1 != t2) return false;
  switch (t1) {
    case 0: return true;
    case 1: return (d1 != 0) == (d2 != 0);
    case 2: return d1 == d2;
    case 4: return std::strcmp(reinterpret_cast<const char*>(d1),
                               reinterpret_cast<const char*>(d2)) == 0;
    default: return d1 == d2;  // func/array/object: identity
  }
}

// Strict less-than on comparable tags. Mismatched tags or unsupported
// types (Function/Array/Object) throw — mirrors interpreter `Value::<`.
inline bool _culebra_value_less(int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  if (t1 != t2) throw std::runtime_error("type error.");
  switch (t1) {
    case 0: return false;
    case 1: return (d1 != 0) < (d2 != 0);
    case 2: return d1 < d2;
    case 4: return std::strcmp(reinterpret_cast<const char*>(d1),
                               reinterpret_cast<const char*>(d2)) < 0;
    default: throw std::runtime_error("type error.");
  }
}

extern "C" {

__attribute__((used)) inline void culebra_runtime_puts(int8_t type,
                                                       int64_t data) {
  switch (type) {
    case 0:
      std::cout << "nil" << std::endl;
      break;
    case 1:
      std::cout << (data ? "true" : "false") << std::endl;
      break;
    case 2:
      std::cout << data << std::endl;
      break;
    case 4:
      std::cout << "'" << reinterpret_cast<const char*>(data) << "'"
                << std::endl;
      break;
    case 5:
    case 6:
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
    case 1:
    case 2:
      truthy = (data != 0);
      break;
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
  using JitFn0 = JitValue (*)(JitClosure*, JitValue);
  auto& s = _culebra_defer_stack();
  while (static_cast<int64_t>(s.size()) > mark) {
    auto v = s.back();
    s.pop_back();
    auto* c = reinterpret_cast<JitClosure*>(v.data);
    auto call = reinterpret_cast<JitFn0>(c->fn_ptr);
    JitValue nil_val = {0, 0};
    try {
      auto r = call(c, nil_val);
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
    case 0: return "Nil";
    case 1: return "Bool";
    case 2: return "Long";
    case 3: return "Function";
    case 4: return "String";
    case 5: return "Array";
    case 6: return "Object";
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
  if (fn_tag != 3) {
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
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "map", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  auto* out = culebra_runtime_array_new();
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    culebra_runtime_array_push(out, r.tag, r.data);
  }
  return out;
}

__attribute__((used)) inline JitArray* culebra_runtime_array_filter(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "filter", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  auto* out = culebra_runtime_array_new();
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    bool keep = (r.tag == 1 || r.tag == 2) ? (r.data != 0) : false;
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
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "for_each", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    _culebra_value_release_impl(r.tag, r.data);
  }
}

// find returns the first matching element (or nil) via out-params.
__attribute__((used)) inline void culebra_runtime_array_find(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col,
    int8_t* out_tag, int64_t* out_data) {
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "find", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    bool keep = (r.tag == 1 || r.tag == 2) ? (r.data != 0) : false;
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
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "any", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    bool keep = (r.tag == 1 || r.tag == 2) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (keep) return 1;
  }
  return 0;
}

__attribute__((used)) inline int64_t culebra_runtime_array_all(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "all", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    bool keep = (r.tag == 1 || r.tag == 2) ? (r.data != 0) : false;
    _culebra_value_release_impl(r.tag, r.data);
    if (!keep) return 0;
  }
  return 1;
}

__attribute__((used)) inline JitArray* culebra_runtime_array_flat_map(
    JitArray* arr, int8_t fn_tag, int64_t fn_data, int64_t line, int64_t col) {
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "flat_map", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  auto* out = culebra_runtime_array_new();
  JitValue nil_val = {0, 0};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    JitValue r = call(fn, nil_val, e);
    if (r.tag != 5) {  // TAG_ARRAY
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
  using JitFn1 = JitValue (*)(JitClosure*, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 1, "sort_by", line, col);
  auto call = reinterpret_cast<JitFn1>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  std::vector<std::pair<JitValue, size_t>> keyed;
  keyed.reserve(arr->size);
  auto release_keys = [&] {
    for (auto& [k, _] : keyed) _culebra_value_release_impl(k.tag, k.data);
  };
  try {
    for (size_t i = 0; i < arr->size; i++) {
      auto e = arr->items[i];
      culebra_runtime_value_retain(e.tag, e.data);
      keyed.emplace_back(call(fn, nil_val, e), i);
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) {
                       return _culebra_value_less(a.first.tag, a.first.data,
                                                  b.first.tag, b.first.data);
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
  using JitFn2 = JitValue (*)(JitClosure*, JitValue, JitValue, JitValue);
  auto* fn = _culebra_expect_callback(fn_tag, fn_data, 2, "reduce", line, col);
  auto call = reinterpret_cast<JitFn2>(fn->fn_ptr);
  JitValue nil_val = {0, 0};
  JitValue acc = {init_tag, init_data};
  for (size_t i = 0; i < arr->size; i++) {
    auto e = arr->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    acc = call(fn, nil_val, acc, e);
  }
  *out_tag = acc.tag;
  *out_data = acc.data;
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
        r, /*String*/ 4,
        reinterpret_cast<int64_t>(_culebra_heap_str(std::string(sv))));
    return r;
  }
  size_t pos = 0;
  while (true) {
    auto p = sv.find(sp, pos);
    if (p == std::string_view::npos) {
      auto* piece = _culebra_heap_str(std::string(sv.substr(pos)));
      culebra_runtime_array_push(r, 4, reinterpret_cast<int64_t>(piece));
      break;
    }
    auto* piece = _culebra_heap_str(std::string(sv.substr(pos, p - pos)));
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
    if (tag == 4) {
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
    culebra_runtime_array_push(r, /*String*/ 4, reinterpret_cast<int64_t>(buf));
  }
  return r;
}

__attribute__((used)) inline void culebra_runtime_object_remove(
    JitObject* obj, const char* key) {
  auto it = obj->props.find(key);
  if (it == obj->props.end()) return;
  _culebra_value_release_impl(it->second.value.tag, it->second.value.data);
  obj->props.erase(it);
}

}  // extern "C" (close briefly for C++ impl helpers)

// --- Reference counting (C++ impl helpers, not extern "C") ---
//
// Only heap-allocated refcounted types have refcount headers:
//   TAG_FUNC (closure), TAG_ARRAY, TAG_OBJECT, and JitCell (internal).
// Strings (TAG_STRING) are NOT refcounted in this implementation and leak.
// Nil/Bool/Long are value types and have no refcount.

inline void _culebra_cell_release(JitCell* c);

inline void _culebra_value_release_impl(int8_t tag, int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case 3: {  // TAG_FUNC (closure)
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
    case 5: {  // TAG_ARRAY
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
    case 6: {  // TAG_OBJECT
      auto* o = reinterpret_cast<JitObject*>(data);
      if (--o->refcount == 0) {
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
// Tag constants matching Value::Type
// ---------------------------------------------------------------------------
static constexpr int8_t TAG_NIL = 0;
static constexpr int8_t TAG_BOOL = 1;
static constexpr int8_t TAG_LONG = 2;
static constexpr int8_t TAG_FUNC = 3;
static constexpr int8_t TAG_STRING = 4;
static constexpr int8_t TAG_ARRAY = 5;
static constexpr int8_t TAG_OBJECT = 6;

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
inline constexpr auto array_reduce        = "culebra_runtime_array_reduce";
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
inline constexpr auto object_get          = "culebra_runtime_object_get";
inline constexpr auto object_has          = "culebra_runtime_object_has";
inline constexpr auto object_keys         = "culebra_runtime_object_keys";
inline constexpr auto object_new          = "culebra_runtime_object_new";
inline constexpr auto object_remove       = "culebra_runtime_object_remove";
inline constexpr auto object_set          = "culebra_runtime_object_set";
inline constexpr auto object_size         = "culebra_runtime_object_size";
inline constexpr auto print               = "culebra_runtime_print";
inline constexpr auto puts                = "culebra_runtime_puts";
inline constexpr auto range               = "culebra_runtime_range";
inline constexpr auto read_file           = "culebra_runtime_read_file";
inline constexpr auto rethrow             = "culebra_runtime_rethrow";
inline constexpr auto str_cmp             = "culebra_runtime_str_cmp";
inline constexpr auto str_concat          = "culebra_runtime_str_concat";
inline constexpr auto str_contains        = "culebra_runtime_str_contains";
inline constexpr auto str_ends_with       = "culebra_runtime_str_ends_with";
inline constexpr auto str_eq              = "culebra_runtime_str_eq";
inline constexpr auto str_lower           = "culebra_runtime_str_lower";
inline constexpr auto str_size            = "culebra_runtime_str_size";
inline constexpr auto str_slice           = "culebra_runtime_str_slice";
inline constexpr auto str_split           = "culebra_runtime_str_split";
inline constexpr auto str_starts_with     = "culebra_runtime_str_starts_with";
inline constexpr auto str_trim            = "culebra_runtime_str_trim";
inline constexpr auto str_upper           = "culebra_runtime_str_upper";
inline constexpr auto throw_              = "culebra_runtime_throw";
inline constexpr auto to_long             = "culebra_runtime_to_long";
inline constexpr auto type_check          = "culebra_runtime_type_check";
inline constexpr auto type_error          = "culebra_runtime_type_error";
inline constexpr auto type_of             = "culebra_runtime_type_of";
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
    std::vector<std::map<std::string, VarSlot>> scopes;
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
        "map",         "filter",     "reduce",   "for_each",
        "find",        "any",        "all",      "flat_map", "sort_by"};
    return known;
  }
  // Stdlib helpers — bodies live in stdlib_jit.h.
  void declare_stdlib_runtime();
  llvm::Value* try_compile_stdlib_global(const std::string& name,
                                         const peg::Ast& argsAst,
                                         const peg::Ast& callAst);

 private:
  llvm::LLVMContext& ctx_;
  llvm::Module* module_;
  llvm::IRBuilder<>& builder_;
  llvm::StructType* valueType_;    // {i8, i64}
  llvm::StructType* cellType_;     // {Value}
  llvm::StructType* closureType_;  // {ptr fn, i64 n, ptr captures}

  // Variable scoping: stack of maps from name -> slot
  std::vector<std::map<std::string, VarSlot>> scopes_;

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

  void pop_scope() { scopes_.pop_back(); }

  const VarSlot* lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  void define_var(const std::string& name, VarSlot slot) {
    scopes_.back()[name] = slot;
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
    auto errorBB = llvm::BasicBlock::Create(ctx_, "tobool.error", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "tobool.merge", fn);

    auto sw = builder_.CreateSwitch(tag, errorBB, 3);
    sw->addCase(builder_.getInt8(TAG_NIL), nilBB);
    sw->addCase(builder_.getInt8(TAG_BOOL), boolBB);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);

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

    builder_.SetInsertPoint(errorBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 3, "tobool");
    phi->addIncoming(nilVal, nilBB);
    phi->addIncoming(boolVal, boolBB);
    phi->addIncoming(longVal, longBB);
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

  // --- Free variable analysis ---

  static bool is_builtin_var(const std::string& name) {
    static const std::unordered_set<std::string_view> names = {
        "puts",    "print",     "assert",    "self",    "this",
        "abs",     "min",       "max",       "range",   "to_long",
        "to_string", "type_of", "input",     "read_file", "write_file"};
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

    if (node.tag == "DOT"_) {
      // DOT node is a property name, not a variable reference. Skip entirely.
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
      case "IDENTIFIER"_:
        return compile_identifier(ast);
      case "NUMBER"_:
        return compile_number(ast);
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
      auto tag = extract_tag(val);
      auto data = extract_data(val);
      auto keyPtr = builder_.CreateGlobalString(name, ".key");
      emit_call(
          module_->getOrInsertFunction(
              rt::object_set, builder_.getVoidTy(), ptrTy,
              ptrTy, builder_.getInt1Ty(), builder_.getInt8Ty(),
              builder_.getInt64Ty(), builder_.getInt64Ty(),
              builder_.getInt64Ty()),
          {objPtr, keyPtr, builder_.getInt1(mut), tag, data,
           current_line_val(), current_column_val()});
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
    return VarSlot{VarSlot::Cell, cellSlotAlloca};
  }

  VarSlot make_stack_slot(const std::string& name, llvm::Value* initValue) {
    auto fn = builder_.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(),
                             fn->getEntryBlock().begin());
    auto alloca = entryB.CreateAlloca(valueType_, nullptr, name);
    entryB.CreateStore(llvm::ConstantAggregateZero::get(valueType_), alloca);
    // At declaration point: use store_slot (releases old = nil first run, else
    // previous iteration's value).
    VarSlot slot{VarSlot::Stack, alloca};
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
        auto it = scopes_.back().find(name);
        if (it != scopes_.back().end()) {
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
        auto keyPtr = builder_.CreateGlobalString(name, ".key");
        auto rtag = extract_tag(rval);
        auto rdata = extract_data(rval);
        emit_call(
            module_->getOrInsertFunction(
                rt::object_set, builder_.getVoidTy(), ptrTy,
                ptrTy, builder_.getInt1Ty(), builder_.getInt8Ty(),
                builder_.getInt64Ty(), builder_.getInt64Ty(),
                builder_.getInt64Ty()),
            {objPtr, keyPtr, builder_.getInt1(mut), rtag, rdata,
             current_line_val(), current_column_val()});
        emit_value_release(lval);  // release the lvalue's ref
        emit_value_retain(rval);
        return rval;
      }
      default:
        throw std::runtime_error("invalid lvalue postfix");
    }
  }

  // --- Arithmetic ---

  llvm::Value* compile_additive(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);

      auto ldata = value_to_long(lhs);
      auto rdata = value_to_long(rhs);

      auto ope = ast.nodes[i]->token[0];
      llvm::Value* result;
      switch (ope) {
        case '+':
          result = builder_.CreateAdd(ldata, rdata, "add");
          break;
        case '-':
          result = builder_.CreateSub(ldata, rdata, "sub");
          break;
        default:
          throw std::runtime_error("invalid additive operator");
      }
      lhs = make_long(result);
    }
    return lhs;
  }

  llvm::Value* compile_multiplicative(const peg::Ast& ast) {
    auto lhs = compile(*ast.nodes[0]);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = compile(*ast.nodes[i + 1]);

      auto ldata = value_to_long(lhs);
      auto rdata = value_to_long(rhs);

      auto ope = ast.nodes[i]->token[0];
      llvm::Value* result;
      switch (ope) {
        case '*':
          result = builder_.CreateMul(ldata, rdata, "mul");
          break;
        case '/': {
          auto fn = builder_.GetInsertBlock()->getParent();
          auto isZero =
              builder_.CreateICmpEQ(rdata, builder_.getInt64(0), "iszero");
          auto zeroBB = llvm::BasicBlock::Create(ctx_, "div.zero", fn);
          auto okBB = llvm::BasicBlock::Create(ctx_, "div.ok", fn);
          builder_.CreateCondBr(isZero, zeroBB, okBB);

          builder_.SetInsertPoint(zeroBB);
          emit_div_zero();
          builder_.CreateUnreachable();

          builder_.SetInsertPoint(okBB);
          result = builder_.CreateSDiv(ldata, rdata, "div");
          break;
        }
        case '%': {
          auto fn = builder_.GetInsertBlock()->getParent();
          auto isZero =
              builder_.CreateICmpEQ(rdata, builder_.getInt64(0), "iszero");
          auto zeroBB = llvm::BasicBlock::Create(ctx_, "mod.zero", fn);
          auto okBB = llvm::BasicBlock::Create(ctx_, "mod.ok", fn);
          builder_.CreateCondBr(isZero, zeroBB, okBB);

          builder_.SetInsertPoint(zeroBB);
          emit_div_zero();
          builder_.CreateUnreachable();

          builder_.SetInsertPoint(okBB);
          result = builder_.CreateSRem(ldata, rdata, "mod");
          break;
        }
        default:
          throw std::runtime_error("invalid multiplicative operator");
      }
      lhs = make_long(result);
    }
    return lhs;
  }

  // --- Unary ---

  llvm::Value* compile_unary_plus(const peg::Ast& ast) {
    return compile(*ast.nodes[1]);
  }

  llvm::Value* compile_unary_minus(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto data = value_to_long(val);
    auto neg = builder_.CreateNeg(data, "neg");
    return make_long(neg);
  }

  llvm::Value* compile_unary_not(const peg::Ast& ast) {
    auto val = compile(*ast.nodes[1]);
    auto b = value_to_bool(val);
    auto notb = builder_.CreateNot(b, "not");
    return make_bool(notb);
  }

  // --- Comparison ---

  llvm::Value* compile_condition(const peg::Ast& ast) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);
    auto lhs = compile(*ast.nodes[0]);
    auto ope_str = std::string(ast.nodes[1]->token);
    auto rhs = compile(*ast.nodes[2]);

    auto ltag = extract_tag(lhs);
    auto ldata = extract_data(lhs);
    auto rtag = extract_tag(rhs);
    auto rdata = extract_data(rhs);

    if (ope_str == "==" || ope_str == "!=") {
      auto tagsEq = builder_.CreateICmpEQ(ltag, rtag, "tags.eq");

      auto fn = builder_.GetInsertBlock()->getParent();
      auto sameTagBB = llvm::BasicBlock::Create(ctx_, "cmp.same", fn);
      auto diffTagBB = llvm::BasicBlock::Create(ctx_, "cmp.diff", fn);
      auto mergeBB = llvm::BasicBlock::Create(ctx_, "cmp.merge", fn);

      builder_.CreateCondBr(tagsEq, sameTagBB, diffTagBB);

      // Same tag: if String use runtime str_eq, else compare data
      builder_.SetInsertPoint(sameTagBB);
      auto isString =
          builder_.CreateICmpEQ(ltag, builder_.getInt8(TAG_STRING), "is.str");
      auto strBB = llvm::BasicBlock::Create(ctx_, "cmp.str", fn);
      auto intBB = llvm::BasicBlock::Create(ctx_, "cmp.int", fn);
      builder_.CreateCondBr(isString, strBB, intBB);

      builder_.SetInsertPoint(strBB);
      auto lptr = builder_.CreateIntToPtr(ldata, ptrTy);
      auto rptr = builder_.CreateIntToPtr(rdata, ptrTy);
      auto strEq = emit_call(
          module_->getOrInsertFunction(rt::str_eq,
                                       builder_.getInt1Ty(), ptrTy, ptrTy),
          {lptr, rptr}, "str.eq");
      // emit_call may have split the block with an invoke; capture the
      // actual predecessor for the PHI below.
      auto strEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(intBB);
      auto intEq = builder_.CreateICmpEQ(ldata, rdata, "data.eq");
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(diffTagBB);
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 3, "eq.result");
      phi->addIncoming(strEq, strEndBB);
      phi->addIncoming(intEq, intBB);
      phi->addIncoming(builder_.getFalse(), diffTagBB);

      llvm::Value* result = phi;
      if (ope_str == "!=") {
        result = builder_.CreateNot(result, "neq");
      }
      return make_bool(result);
    }

    // Ordering: types must match. Nil returns false (matches interpreter).
    // String uses str_cmp, Long uses icmp. Other types → type error.
    auto tagsEq = builder_.CreateICmpEQ(ltag, rtag, "tags.eq");
    auto fn = builder_.GetInsertBlock()->getParent();
    auto sameBB = llvm::BasicBlock::Create(ctx_, "ord.same", fn);
    auto diffBB = llvm::BasicBlock::Create(ctx_, "ord.diff", fn);
    auto mergeBB = llvm::BasicBlock::Create(ctx_, "ord.merge", fn);
    builder_.CreateCondBr(tagsEq, sameBB, diffBB);

    builder_.SetInsertPoint(diffBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(sameBB);
    auto nilBB = llvm::BasicBlock::Create(ctx_, "ord.nil", fn);
    auto strBB = llvm::BasicBlock::Create(ctx_, "ord.str", fn);
    auto longBB = llvm::BasicBlock::Create(ctx_, "ord.long", fn);
    auto errBB = llvm::BasicBlock::Create(ctx_, "ord.err", fn);
    auto sw = builder_.CreateSwitch(ltag, errBB, 4);
    sw->addCase(builder_.getInt8(TAG_NIL), nilBB);
    sw->addCase(builder_.getInt8(TAG_STRING), strBB);
    sw->addCase(builder_.getInt8(TAG_LONG), longBB);
    sw->addCase(builder_.getInt8(TAG_BOOL), longBB);  // compare as int

    // Nil: always false
    builder_.SetInsertPoint(nilBB);
    auto nilResult = builder_.getFalse();
    builder_.CreateBr(mergeBB);

    // String
    builder_.SetInsertPoint(strBB);
    auto lptr = builder_.CreateIntToPtr(ldata, ptrTy);
    auto rptr = builder_.CreateIntToPtr(rdata, ptrTy);
    auto strCmp = emit_call(
        module_->getOrInsertFunction(rt::str_cmp,
                                     builder_.getInt32Ty(), ptrTy, ptrTy),
        {lptr, rptr}, "str.cmp");
    auto zero32 = builder_.getInt32(0);
    llvm::Value* strResult;
    if (ope_str == "<") {
      strResult = builder_.CreateICmpSLT(strCmp, zero32);
    } else if (ope_str == "<=") {
      strResult = builder_.CreateICmpSLE(strCmp, zero32);
    } else if (ope_str == ">") {
      strResult = builder_.CreateICmpSGT(strCmp, zero32);
    } else if (ope_str == ">=") {
      strResult = builder_.CreateICmpSGE(strCmp, zero32);
    } else {
      throw std::runtime_error("invalid comparison operator");
    }
    auto strEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // Long (and Bool compared as int)
    builder_.SetInsertPoint(longBB);
    llvm::Value* longResult;
    if (ope_str == "<") {
      longResult = builder_.CreateICmpSLT(ldata, rdata);
    } else if (ope_str == "<=") {
      longResult = builder_.CreateICmpSLE(ldata, rdata);
    } else if (ope_str == ">") {
      longResult = builder_.CreateICmpSGT(ldata, rdata);
    } else if (ope_str == ">=") {
      longResult = builder_.CreateICmpSGE(ldata, rdata);
    } else {
      throw std::runtime_error("invalid comparison operator");
    }
    auto longEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    // Other types → error
    builder_.SetInsertPoint(errBB);
    emit_type_error();
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(mergeBB);
    auto phi = builder_.CreatePHI(builder_.getInt1Ty(), 3, "ord.result");
    phi->addIncoming(nilResult, nilBB);
    phi->addIncoming(strResult, strEndBB);
    phi->addIncoming(longResult, longEndBB);
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
      auto has = emit_call(
          module_->getOrInsertFunction(rt::object_has,
                                       builder_.getInt1Ty(), ptrTy, ptrTy),
          {objPtr, keyPtr});
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
    compile(*ast.nodes[1]);
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    return make_nil();
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

    // Signature: %Value fn(ptr __cls__, %Value this, %Value p1, ...)
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(ptrTy);  // __cls__
    paramTypes.push_back(valueType_);  // this
    for (size_t i = 0; i < paramNames.size(); i++) {
      paramTypes.push_back(valueType_);
    }
    auto fnType = FunctionType::get(valueType_, paramTypes, false);

    auto fnName = std::format("__culebra_fn_{}", funcCounter_++);
    auto fn = Function::Create(fnType, GlobalValue::ExternalLinkage, fnName,
                               module_);
    if (info.has_eh) {
      fn->setPersonalityFn(get_personality_fn());
    }

    // Name parameters
    auto argIt = fn->arg_begin();
    argIt->setName("__cls__");
    ++argIt;
    argIt->setName("this");
    ++argIt;
    for (auto& name : paramNames) {
      argIt->setName(name);
      ++argIt;
    }

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

    // `self` = make_func(__cls__): calling it re-enters this fn with this closure
    {
      auto selfVal = make_func(clsArg);
      define_var("self", make_stack_slot("self", selfVal));
    }

    // `this` is from arg; if captured, allocate cell
    if (info.captured_locals.contains("this")) {
      define_var("this", make_cell_slot("this", thisArg));
    } else {
      define_var("this", make_stack_slot("this", thisArg));
    }

    // Bind declared parameters
    for (size_t i = 0; i < paramNames.size(); i++) {
      const auto& name = paramNames[i];
      auto argVal = &*argIt++;
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
      define_var(fv, VarSlot{VarSlot::Cell, holder});
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

    // User-defined method on Object: fetch property, call with this=receiver
    auto methodVal = compile_property_get(receiver, method);
    return compile_function_call(argsAst, methodVal, receiver);
  }

  llvm::Value* compile_function_call(const peg::Ast& argsAst,
                                     llvm::Value* callee,
                                     llvm::Value* thisVal = nullptr) {
    auto ptrTy = llvm::PointerType::get(ctx_, 0);

    // Type check: TAG_FUNC
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

    // Extract closure pointer from Value
    auto clsPtr = builder_.CreateIntToPtr(extract_data(callee), ptrTy);
    // Load fn_ptr from closure (field 1, after refcount)
    auto fnFieldPtr =
        builder_.CreateStructGEP(closureType_, clsPtr, 1, "fn.ptr");
    auto fnPtr = builder_.CreateLoad(ptrTy, fnFieldPtr, "fn");

    // Build args: __cls__, this, user args
    std::vector<llvm::Value*> args;
    args.push_back(clsPtr);
    args.push_back(thisVal ? thisVal : make_nil());
    for (auto& argNode : argsAst.nodes) {
      args.push_back(compile(*argNode));
    }

    std::vector<llvm::Type*> argTypes;
    argTypes.push_back(ptrTy);  // __cls__
    for (size_t i = 1; i < args.size(); i++) argTypes.push_back(valueType_);
    auto calleeType = llvm::FunctionType::get(valueType_, argTypes, false);

    return emit_call(calleeType, fnPtr, args, "call.result");
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

    // Closure signature: %Value fn(ptr __cls__, %Value this). 0 params.
    std::vector<Type*> paramTypes = {ptrTy, valueType_};
    auto fnType = FunctionType::get(valueType_, paramTypes, false);
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
      define_var(fv, VarSlot{VarSlot::Cell, holder});
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

  // --- Higher-order array methods (§17.2) ---

  auto ho_line = current_line_val();
  auto ho_col = current_column_val();

  if (method == "map" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "map");
    auto f = compile(*argsAst.nodes[0]);
    auto out = emit_call(
        module_->getFunction(rt::array_map),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_array(out);
  }

  if (method == "filter" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "filter");
    auto f = compile(*argsAst.nodes[0]);
    auto out = emit_call(
        module_->getFunction(rt::array_filter),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_array(out);
  }

  if (method == "for_each" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "foreach");
    auto f = compile(*argsAst.nodes[0]);
    emit_call(
        module_->getFunction(rt::array_for_each),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_nil();
  }

  if (method == "reduce" && argsAst.nodes.size() == 2) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "reduce");
    auto init = compile(*argsAst.nodes[0]);
    auto f = compile(*argsAst.nodes[1]);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "red.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "red.data");
    emit_call(
        module_->getFunction(rt::array_reduce),
        {arrPtr, extract_tag(init), extract_data(init), extract_tag(f),
         extract_data(f), ho_line, ho_col, outTag, outData});
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if (method == "find" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "find");
    auto f = compile(*argsAst.nodes[0]);
    llvm::IRBuilder<> entryB(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto outTag =
        entryB.CreateAlloca(builder_.getInt8Ty(), nullptr, "find.tag");
    auto outData =
        entryB.CreateAlloca(builder_.getInt64Ty(), nullptr, "find.data");
    emit_call(
        module_->getFunction(rt::array_find),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col,
         outTag, outData});
    emit_value_release(f);
    auto tagLoaded = builder_.CreateLoad(builder_.getInt8Ty(), outTag);
    auto dataLoaded = builder_.CreateLoad(builder_.getInt64Ty(), outData);
    llvm::Value* result = llvm::UndefValue::get(valueType_);
    result = builder_.CreateInsertValue(result, tagLoaded, {0});
    result = builder_.CreateInsertValue(result, dataLoaded, {1});
    return result;
  }

  if ((method == "any" || method == "all") && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, method.c_str());
    auto f = compile(*argsAst.nodes[0]);
    auto rt_name = method == "any" ? rt::array_any
                                   : rt::array_all;
    auto result_i64 = emit_call(
        module_->getFunction(rt_name),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    auto as_bool =
        builder_.CreateICmpNE(result_i64, builder_.getInt64(0));
    return make_bool(as_bool);
  }

  if (method == "flat_map" && argsAst.nodes.size() == 1) {
    auto arrPtr = expect_tag(receiver, TAG_ARRAY, "flat_map");
    auto f = compile(*argsAst.nodes[0]);
    auto out = emit_call(
        module_->getFunction(rt::array_flat_map),
        {arrPtr, extract_tag(f), extract_data(f), ho_line, ho_col});
    emit_value_release(f);
    return make_array(out);
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
