#pragma once

#ifdef CULEBRA_JIT_ENABLED

// JIT string representation: value-to-string conversion, JitStrHeader
// (inline length header), string interning and JitStringView helpers.
//
// Runtime-layer fragment of jit.h, split out for readability. These
// fragments rely on jit.h's #include block and are included by jit.h in a
// fixed sequence (see jit.h); they are not standalone headers.

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

// Heap-allocated descriptor for TAG_STRINGVIEW. `ptr`/`len` borrow bytes
// owned by `owner_base` — the *registered* GC base pointer of the backing
// String (data pointer, i.e. what a TAG_STRING JitValue carries), or nullptr
// when the backing is not GC-tracked (a literal's .rodata, an interned/pinned
// name). Traced-only strings sweep their backing, so the view roots that
// backing through this edge (see _jit_gc_enumerate_children). ptr@0 / len@8
// stay fixed — codegen loads len at offset 8 (jit.h).
struct JitStringView {
  const char* ptr;
  uint64_t len;
  const char* owner_base;
};
static_assert(sizeof(JitStringView) == 24, "JitStringView layout drift");

// `obj.k` as a JitValue, or Nil when the key is absent. Used where a
// missing slot should read as Nil (e.g. a range value's optional bounds).
inline JitValue _jit_slot_or_nil(const JitObject* obj, std::string_view k) {
  auto i = obj->find_slot(k);
  return i == static_cast<size_t>(-1) ? JitValue{TAG_NIL, 0}
                                      : obj->slots[i].value;
}

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
      auto* p = reinterpret_cast<const char*>(data);
      std::string s = "'";
      s.append(p, _str_len(p));
      s += "'";
      return s;
    }
    case TAG_STRINGVIEW: {
      auto* v = reinterpret_cast<const JitStringView*>(data);
      std::string s = "'";
      s.append(v->ptr, v->len);
      s += "'";
      return s;
    }
    case TAG_FLOAT: {
      double d;
      std::memcpy(&d, &data, sizeof(d));
      return culebra::format_float_shortest(d);
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
    case TAG_TUPLE: {
      auto* arr = reinterpret_cast<JitArray*>(data);
      _JitStrGuard guard(arr);
      if (guard.already) return "(...)";
      std::string s = "(";
      for (size_t i = 0; i < arr->size; i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(arr->items[i].tag,
                                        arr->items[i].data);
      }
      // (x,) trailing comma marks the 1-element tuple, matching the
      // interpreter and Python.
      if (arr->size == 1) s += ",";
      s += ")";
      return s;
    }
    case TAG_SET: {
      auto* set = reinterpret_cast<JitSet*>(data);
      _JitStrGuard guard(set);
      if (guard.already) return "{...}";
      std::string s = "{";
      for (size_t i = 0; i < set->members.size(); i++) {
        if (i > 0) s += ", ";
        s += _culebra_value_to_str_impl(set->members[i].tag,
                                        set->members[i].data);
      }
      s += "}";
      return s;
    }
    case TAG_TENSOR: {
      auto* t = reinterpret_cast<JitTensor*>(data);
      return tensor_str(*t->impl);
    }
    case TAG_OBJECT: {
      auto* obj = reinterpret_cast<JitObject*>(data);
      _JitStrGuard guard(obj);
      if (guard.already) return "{...}";
      // Range value: print in source form (`1..3`, `2..`, `..3`, `..`,
      // `1..=3`) rather than as a raw object (matches the interpreter).
      {
        auto cidx = obj->find_slot("class");
        if (cidx != static_cast<size_t>(-1) &&
            obj->slots[cidx].value.tag == TAG_STRING &&
            std::string_view(reinterpret_cast<const char*>(
                obj->slots[cidx].value.data)) == "Range") {
          auto sv = _jit_slot_or_nil(obj, "start");
          auto ev = _jit_slot_or_nil(obj, "end");
          auto iv = _jit_slot_or_nil(obj, "inclusive");
          std::string out;
          if (sv.tag != TAG_NIL) out += std::to_string(sv.data);
          out += (iv.tag == TAG_BOOL && iv.data != 0) ? "..=" : "..";
          if (ev.tag != TAG_NIL) out += std::to_string(ev.data);
          return out;
        }
      }
      // Hoist a String `class:` tag to a prefix (matches the tree
      // interpreter's str_object formatting).
      std::string s;
      bool has_class_tag = false;
      {
        auto idx = obj->find_slot("class");
        if (idx != static_cast<size_t>(-1) &&
            obj->slots[idx].value.tag == TAG_STRING) {
          s = reinterpret_cast<const char*>(obj->slots[idx].value.data);
          s += " ";
          has_class_tag = true;
        }
      }
      s += "{";
      bool first = true;
      if (obj->key_order) {
        // Mixed-key path: walk the unified vector so String and
        // non-String keys interleave in true insertion order.
        for (const auto& key : *obj->key_order) {
          if (key.tag == TAG_STRING) {
            auto name = reinterpret_cast<const char*>(key.data);
            if (has_class_tag && std::string_view(name) == "class") continue;
            auto idx = obj->shape ? obj->shape->offset(name)
                                  : static_cast<size_t>(-1);
            if (idx == static_cast<size_t>(-1)) continue;
            const auto& entry = obj->slots[idx];
            if (!first) s += ", ";
            first = false;
            if (entry.mut) s += "mut ";
            s += name;
            s += ": ";
            s += _culebra_value_to_str_impl(entry.value.tag,
                                            entry.value.data);
          } else {
            if (!obj->non_string_props) continue;
            auto it = obj->non_string_props->find(key);
            if (it == obj->non_string_props->end()) continue;
            if (!first) s += ", ";
            first = false;
            if (it->second.mut) s += "mut ";
            s += _culebra_value_to_str_impl(key.tag, key.data);
            s += ": ";
            s += _culebra_value_to_str_impl(it->second.value.tag,
                                            it->second.value.data);
          }
        }
      } else if (obj->shape) {
        // String-only fast path: walk shape->names directly, skipping
        // the per-property key_order push that mixed-key objects need.
        for (size_t i = 0; i < obj->shape->names.size(); i++) {
          const auto& name = obj->shape->names[i];
          if (has_class_tag && name == "class") continue;
          const auto& entry = obj->slots[i];
          if (!first) s += ", ";
          first = false;
          if (entry.mut) s += "mut ";
          s += name;
          s += ": ";
          s += _culebra_value_to_str_impl(entry.value.tag,
                                          entry.value.data);
        }
      }
      s += "}";
      return s;
    }
    default:
      return "[unknown]";
  }
}

// --- JIT String representation (c2: inline length header) ---------------
// Every TAG_STRING value's `data` points at the `bytes` field of a
// length-prefixed buffer:
//
//   [ uint64_t len ][ bytes... ][ '\0' ]
//                    ^ data points here; len lives at data[-8].
//
// Length is authoritative and read in O(1) via _str_len, so embedded NUL
// bytes are preserved (a String is a Go-style byte string). The trailing
// NUL is retained so a string with no embedded NUL can be handed to C
// APIs (paths, printf %s) as-is. The buffer leaks for the program's
// lifetime (cycle-bounded, same as before the header existed). Literals
// carry an identical layout emitted as a .rodata ConstantStruct (see
// emit_str_literal), so readers never branch on a string's origin.
struct JitStrHeader { uint64_t len; };

// Byte length of a TAG_STRING data pointer; the header sits just before
// the bytes. memcpy avoids any alignment assumption (the 8-byte header
// keeps `data` 8-aligned regardless).
inline uint64_t _str_len(const char* data) {
  uint64_t len;
  std::memcpy(&len, data - sizeof(JitStrHeader), sizeof(len));
  // Sanity backstop (debug only): a header-less pointer mis-tagged as
  // TAG_STRING reads garbage here. An implausibly large length means the
  // invariant "every TAG_STRING is header-backed" was violated upstream.
  assert(len <= (uint64_t{1} << 40) && "TAG_STRING without length header");
  return len;
}

// View over a TAG_STRING data pointer, length read from the header.
// (Tag-dispatching counterpart: _culebra_str_view.)
inline std::string_view _str_sv(const char* s) {
  return std::string_view(s, _str_len(s));
}

// Allocate a header-prefixed buffer for `len` bytes (+ trailing NUL) and
// return a pointer to the bytes field. malloc's 16-byte alignment keeps
// the bytes (at base+8) 8-aligned. Caller fills [data, data+len); the NUL
// is pre-written.
// Write the length header + trailing NUL into a raw buffer `base` sized at
// least sizeof(JitStrHeader)+len+1; return the bytes pointer (at base+8, kept
// 8-aligned by the 16-aligned allocators). Caller fills [data, data+len).
inline char* _str_init(char* base, uint64_t len) {
  std::memcpy(base, &len, sizeof(len));
  char* data = base + sizeof(JitStrHeader);
  data[len] = '\0';
  return data;
}

inline char* _str_alloc(uint64_t len) {
  size_t total = sizeof(JitStrHeader) + len + 1;
  // Carve the bytes from the per-Runtime slab (16-aligned, same as malloc),
  // not system malloc: string churn allocates millions of tiny buffers, and
  // the slab collapses that into amortized 64KB-chunk bump-carving with an
  // intrusive free-list — the same win the JIT structs already take. The
  // sweep path frees back to the slab with the recomputed byte count.
  char* data = _str_init(static_cast<char*>(_slab().alloc(total)), len);
  // Traced-only strings: register the buffer as a GC leaf keyed on the
  // *bytes* pointer (`data`) — that is the pointer a TAG_STRING JitValue
  // carries, so the conservative stack scan roots it by exact match. The
  // sweep path recovers the slab base (data - sizeof(JitStrHeader)).
  // adopt() also drives maybe_collect(), so a string-only allocation loop
  // still triggers reclamation. Literals bypass _str_alloc (.rodata) and
  // are therefore never registered nor swept. See docs on this branch.
  _gc_heap().adopt(data, static_cast<uint32_t>(total), GC_TAG_STRING);
  return data;
}

inline const char* _culebra_heap_str(std::string_view s) {
  char* data = _str_alloc(s.size());
  std::memcpy(data, s.data(), s.size());
  return data;
}

// Intern a borrowed name (e.g. shape->names[i]) into a header-backed,
// process-lifetime String buffer so it can be surfaced as a TAG_STRING
// value without a per-access copy. Cached by content; allocated once per
// distinct name. Mutex-guarded like trait_registry (mt_smoke exercises
// concurrent JIT runtimes sharing one process).
inline const char* _intern_str(std::string_view s) {
  static std::mutex mu;
  static std::unordered_map<std::string, const char*> cache;
  std::lock_guard<std::mutex> lock(mu);
  std::string key(s);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  // The intern cache is a PROCESS-GLOBAL static shared across Runtimes, and
  // hands out its pointers for the whole process lifetime. So an interned name
  // must NOT come from the per-Runtime slab (freed at that Runtime's teardown
  // -> the cache would dangle for every other live Runtime) nor be registered
  // with any single Runtime's collector. Allocate an immortal malloc buffer
  // with the same header layout and hand it out uninstrumented — invisible to
  // every Runtime's GC, exactly like a .rodata string literal, never swept.
  uint64_t len = s.size();
  char* base = static_cast<char*>(std::malloc(sizeof(JitStrHeader) + len + 1));
  char* data = _str_init(base, len);
  std::memcpy(data, s.data(), len);
  cache.emplace(std::move(key), data);
  return data;
}

// The registered GC base pointer that owns a strlike value's bytes, for use
// as a view's owner_base. A String owns its own bytes (the data pointer is the
// registered base); a view forwards its owner (transitive, so a view-of-view
// still roots the ultimate backing); anything else has no GC-tracked owner.
inline const char* _view_owner_base(int8_t tag, int64_t data) {
  if (tag == TAG_STRING) return reinterpret_cast<const char*>(data);
  if (tag == TAG_STRINGVIEW)
    return reinterpret_cast<const JitStringView*>(data)->owner_base;
  return nullptr;
}

// Allocate a JitStringView descriptor over [ptr, ptr+len) borrowing bytes
// owned by `owner_base` (the registered backing String's base, or nullptr for
// an untracked backing — a literal / pinned name). The descriptor is adopted
// as a GC_TAG_STRINGVIEW node so the collector reclaims it once no live value
// references it, and so its owner_base edge keeps the backing alive meanwhile
// (traced-only strings are otherwise swept out from under a live view).
inline JitStringView* _culebra_heap_view(const char* ptr, uint64_t len,
                                         const char* owner_base) {
  auto* v = static_cast<JitStringView*>(std::malloc(sizeof(JitStringView)));
  v->ptr = ptr;
  v->len = len;
  v->owner_base = owner_base;
  _gc_heap().adopt(v, static_cast<uint32_t>(sizeof(JitStringView)),
                   GC_TAG_STRINGVIEW);
  return v;
}

// Adapter: get a std::string_view over the bytes of either a TAG_STRING
// (data is `const char*`, null-terminated) or a TAG_STRINGVIEW (data is
// `JitStringView*`). Other tags return an empty view — callers should
// pre-check the tag.
inline std::string_view _culebra_str_view(int8_t tag, int64_t data) {
  if (tag == TAG_STRING) {
    return _str_sv(reinterpret_cast<const char*>(data));
  }
  if (tag == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    return std::string_view(v->ptr, v->len);
  }
  return {};
}

#endif  // CULEBRA_JIT_ENABLED
