#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdio>      // snprintf / sscanf (anonymous shm name; share env)
#include <cstdlib>     // getenv (share env)
#include <fcntl.h>     // open
#include <pthread.h>   // PROCESS_SHARED mutex (with_lock, cross-process)
#include <sched.h>     // sched_yield (lock-init wait spin)
#include <sys/mman.h>  // mmap / munmap / msync / shm_open (POSIX; file/shm)
#include <sys/stat.h>  // fstat (file reopener waits for the creator to size it)
#include <unistd.h>    // close / ftruncate
#if defined(__linux__)
#include <sys/syscall.h>  // SYS_memfd_create (anonymous shm fd)
#endif

#include <shared.h>  // CulebraError

// @packable fixed-layout structs and the SharedBuffer backing store.
// This header is pure metadata + raw byte storage — it knows nothing
// about Value (the Value<->bytes bridge lives in interpreter.h, where
// Value is complete). See [[project_packable_c3]].
namespace culebra {

// Byte size + alignment of a packable scalar type. Reference types
// (String, Array, ...) are not packable — they carry no fixed layout, so
// they report {0,0} and the caller raises the constraint error.
struct PackableTypeInfo {
  size_t size;
  size_t align;
};

inline PackableTypeInfo packable_type_info(std::string_view t) {
  if (t == "Float32") return {4, 4};
  if (t == "Float64" || t == "Float") return {8, 8};
  if (t == "Int8") return {1, 1};
  if (t == "Int16") return {2, 2};
  if (t == "Int32") return {4, 4};
  if (t == "Int64" || t == "Long") return {8, 8};
  if (t == "Byte") return {1, 1};
  if (t == "Bool") return {1, 1};
  return {0, 0};
}

inline size_t packable_align_up(size_t n, size_t a) {
  return (n + a - 1) / a * a;
}

inline std::string_view _packable_trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// --- @packable enum: a fixed tagged union `[tag:i32][payload]` ------------
// A `@packable` enum lays each variant's scalar payload at offset 0 of a
// shared payload region (sized to the largest variant); the i32 tag selects
// which variant's fields are live. One scalar field of a variant:
struct PackableEnumField {
  std::string type;     // scalar type name
  size_t offset;        // byte offset within the payload region
  size_t size;
};
struct PackableEnumVariant {
  std::string name;
  std::vector<PackableEnumField> fields;
};
struct PackableEnumLayout {
  std::vector<PackableEnumVariant> variants;
  size_t payload_offset = 4;   // payload starts after the i32 tag (aligned up)
  size_t size = 4;             // tag + payload region
  size_t align = 4;
  int index_of(std::string_view variant) const {
    for (size_t i = 0; i < variants.size(); i++)
      if (variants[i].name == variant) return static_cast<int>(i);
    return -1;
  }
};

// Compute the layout for an ordered list of (variant, [scalar field types]).
// Returns size 0 if any payload field is non-scalar (the @packable enum
// constraint). The payload region is sized to the largest variant; each
// variant lays its fields C-ABI from payload offset 0.
inline PackableEnumLayout compute_packable_enum_layout(
    const std::vector<std::pair<std::string,
                                std::vector<std::string>>>& variants) {
  PackableEnumLayout layout;
  size_t payload_size = 0, payload_align = 1;
  for (const auto& [vname, ftypes] : variants) {
    PackableEnumVariant v;
    v.name = vname;
    size_t off = 0;
    for (const auto& t : ftypes) {
      auto si = packable_type_info(t);
      if (si.size == 0) return {};  // non-scalar payload -> not packable
      off = packable_align_up(off, si.align);
      v.fields.push_back({t, off, si.size});
      off += si.size;
      payload_align = std::max(payload_align, si.align);
    }
    payload_size = std::max(payload_size, off);
    layout.variants.push_back(std::move(v));
  }
  layout.align = std::max<size_t>(4, payload_align);
  layout.payload_offset = packable_align_up(4, payload_align);
  layout.size = layout.payload_offset + payload_size;
  return layout;
}

inline std::map<std::string, PackableEnumLayout, std::less<>>&
packable_enum_registry() {
  static std::map<std::string, PackableEnumLayout, std::less<>> reg;
  return reg;
}
inline std::mutex& packable_enum_mutex() {
  static std::mutex m;
  return m;
}
inline void register_packable_enum(std::string name, PackableEnumLayout layout) {
  std::lock_guard<std::mutex> lk(packable_enum_mutex());
  packable_enum_registry()[std::move(name)] = std::move(layout);
}
inline const PackableEnumLayout* lookup_packable_enum(std::string_view name) {
  std::lock_guard<std::mutex> lk(packable_enum_mutex());
  auto it = packable_enum_registry().find(name);
  return it == packable_enum_registry().end() ? nullptr : &it->second;
}

// Validate + register a @packable enum from already-parsed variants. Throws
// SyntaxError (the @packable constraint surfacing at declaration time) if any
// variant payload is non-scalar. Shared by interp (builds the list from the
// AST) and the JIT runtime hook (parses it from a spec string).
inline void validate_and_register_packable_enum(
    const std::string& enum_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>&
        variants) {
  auto layout = compute_packable_enum_layout(variants);
  if (layout.size == 0 || (variants.empty())) {
    throw CulebraError(
        "SyntaxError",
        std::format("@packable enum `{}`: every variant payload must be a "
                    "fixed scalar (Float32/.../Bool)", enum_name));
  }
  register_packable_enum(enum_name, std::move(layout));
}

// Spec = `variant:type,type;variant:;...` (a variant with no payload has an
// empty type list). The codegen emits this so AOT can register at runtime.
inline std::vector<std::pair<std::string, std::vector<std::string>>>
parse_packable_enum_spec(std::string_view spec) {
  std::vector<std::pair<std::string, std::vector<std::string>>> variants;
  size_t i = 0;
  while (i < spec.size()) {
    size_t semi = spec.find(';', i);
    if (semi == std::string_view::npos) semi = spec.size();
    auto seg = spec.substr(i, semi - i);
    auto colon = seg.find(':');
    std::string vname(colon == std::string_view::npos ? seg
                                                      : seg.substr(0, colon));
    std::vector<std::string> ftypes;
    if (colon != std::string_view::npos) {
      auto rest = seg.substr(colon + 1);
      size_t j = 0;
      while (j < rest.size()) {
        size_t comma = rest.find(',', j);
        if (comma == std::string_view::npos) comma = rest.size();
        if (comma > j) ftypes.emplace_back(rest.substr(j, comma - j));
        j = comma + 1;
      }
    }
    variants.emplace_back(std::move(vname), std::move(ftypes));
    i = semi + 1;
  }
  return variants;
}

// Layout of a whole field — a scalar, or a `FixedArray<T, N>` (a fixed-
// capacity inline collection: `[len:i32][T × N]`, no pointers, so it fits a
// @packable record). size 0 means the type isn't a packable field type.
struct PackableFieldInfo {
  size_t size = 0;
  size_t align = 1;
  bool is_fixed_array = false;
  bool is_fixed_string = false;  // FixedString<N>: `[len:i32][byte × N]`
  bool is_fixed_set = false;     // FixedSet<T,N>: `[count][state×N][T×N]`
  bool is_fixed_map = false;     // FixedMap<K,V,N>: `[count][state×N][K×N][V×N]`
  bool is_optional = false;      // T?: `[present:byte][T]`, nil when present==0
  bool is_enum = false;          // a registered @packable enum: `[tag:i32][payload]`
  bool is_bytes = false;         // Bytes<N>: exactly N raw bytes (no length prefix)
  std::string elem_type;     // FixedArray/Set element (or FixedMap key) scalar
                             // — or, for is_enum, the enum name
  std::string val_type;      // FixedMap value scalar
  size_t capacity = 0;       // N
  size_t elem_size = 0;      // sizeof(T) / sizeof(K)
  size_t val_size = 0;       // sizeof(V) (FixedMap)
  size_t data_offset = 0;    // byte offset of the element/key array within field
  size_t val_offset = 0;     // byte offset of the value array (FixedMap)
};

inline PackableFieldInfo packable_field_info(std::string_view t) {
  // Optional scalar `T?` = `[present:byte][T]`; read/written as a whole value
  // that is `nil` when present==0. Only scalar optionals are packable (a
  // collection `T?` has no fixed layout).
  if (t.ends_with("?")) {
    auto inner = _packable_trim(t.substr(0, t.size() - 1));
    auto si = packable_type_info(inner);
    if (si.size == 0) return {};
    PackableFieldInfo fi;
    fi.is_optional = true;
    fi.elem_type = std::string(inner);
    fi.elem_size = si.size;
    fi.align = si.align;
    fi.data_offset = packable_align_up(1, si.align);  // T after the present byte
    fi.size = fi.data_offset + si.size;
    return fi;
  }
  // Bytes<N> = exactly N raw bytes (no length prefix), read/written as a whole
  // byte String. For hashes / UUIDs / fixed binary blobs.
  constexpr std::string_view kBytes = "Bytes<";
  if (t.starts_with(kBytes) && t.ends_with(">")) {
    auto nstr = _packable_trim(t.substr(kBytes.size(), t.size() - kBytes.size() - 1));
    long n = 0;
    auto [ptr, ec] =
        std::from_chars(nstr.data(), nstr.data() + nstr.size(), n);
    if (ec != std::errc() || ptr != nstr.data() + nstr.size() || n <= 0)
      return {};
    PackableFieldInfo fi;
    fi.is_bytes = true;
    fi.capacity = static_cast<size_t>(n);
    fi.elem_size = 1;
    fi.align = 1;
    fi.data_offset = 0;
    fi.size = fi.capacity;  // exactly N bytes
    return fi;
  }
  // FixedString<N> = `[len:i32][byte × N]` inline, a fixed-capacity UTF-8
  // string field read/written as a whole String value (a VARCHAR(N)).
  constexpr std::string_view kFS = "FixedString<";
  if (t.starts_with(kFS) && t.ends_with(">")) {
    auto nstr = _packable_trim(t.substr(kFS.size(), t.size() - kFS.size() - 1));
    long n = 0;
    auto [ptr, ec] =
        std::from_chars(nstr.data(), nstr.data() + nstr.size(), n);
    if (ec != std::errc() || ptr != nstr.data() + nstr.size() || n <= 0)
      return {};
    PackableFieldInfo fi;
    fi.is_fixed_string = true;
    fi.capacity = static_cast<size_t>(n);
    fi.elem_size = 1;
    fi.align = 4;                  // the inline len is an i32
    fi.data_offset = 4;
    fi.size = fi.data_offset + fi.capacity;  // 4 + N
    return fi;
  }
  // FixedSet<T, N> = `[count:i32][state:byte × N][T × N]`, an open-addressed
  // hash set of up to N scalar values, mutated in place through a view.
  constexpr std::string_view kFSet = "FixedSet<";
  if (t.starts_with(kFSet) && t.ends_with(">")) {
    auto inner = t.substr(kFSet.size(), t.size() - kFSet.size() - 1);  // "T , N"
    auto comma = inner.rfind(',');
    if (comma == std::string_view::npos) return {};
    auto elem = _packable_trim(inner.substr(0, comma));
    auto nstr = _packable_trim(inner.substr(comma + 1));
    auto ei = packable_type_info(elem);
    if (ei.size == 0) return {};
    long n = 0;
    auto [ptr, ec] = std::from_chars(nstr.data(), nstr.data() + nstr.size(), n);
    if (ec != std::errc() || ptr != nstr.data() + nstr.size() || n <= 0)
      return {};
    PackableFieldInfo fi;
    fi.is_fixed_set = true;
    fi.elem_type = std::string(elem);
    fi.capacity = static_cast<size_t>(n);
    fi.elem_size = ei.size;
    fi.align = std::max<size_t>(4, ei.align);
    fi.data_offset = packable_align_up(4 + fi.capacity, ei.align);  // values[]
    fi.size = fi.data_offset + fi.capacity * ei.size;
    return fi;
  }
  // FixedMap<K, V, N> = `[count:i32][state:byte × N][K × N][V × N]`, an
  // open-addressed hash map of up to N scalar key→value pairs.
  constexpr std::string_view kFMap = "FixedMap<";
  if (t.starts_with(kFMap) && t.ends_with(">")) {
    auto inner = t.substr(kFMap.size(), t.size() - kFMap.size() - 1);
    auto c1 = inner.find(',');
    auto c2 = inner.rfind(',');
    if (c1 == std::string_view::npos || c1 == c2) return {};
    auto kt = _packable_trim(inner.substr(0, c1));
    auto vt = _packable_trim(inner.substr(c1 + 1, c2 - c1 - 1));
    auto nstr = _packable_trim(inner.substr(c2 + 1));
    auto ki = packable_type_info(kt);
    auto vi = packable_type_info(vt);
    if (ki.size == 0 || vi.size == 0) return {};
    long n = 0;
    auto [ptr, ec] = std::from_chars(nstr.data(), nstr.data() + nstr.size(), n);
    if (ec != std::errc() || ptr != nstr.data() + nstr.size() || n <= 0)
      return {};
    PackableFieldInfo fi;
    fi.is_fixed_map = true;
    fi.elem_type = std::string(kt);
    fi.val_type = std::string(vt);
    fi.capacity = static_cast<size_t>(n);
    fi.elem_size = ki.size;
    fi.val_size = vi.size;
    fi.align = std::max<size_t>({4, ki.align, vi.align});
    fi.data_offset = packable_align_up(4 + fi.capacity, ki.align);  // keys[]
    fi.val_offset =
        packable_align_up(fi.data_offset + fi.capacity * ki.size, vi.align);
    fi.size = fi.val_offset + fi.capacity * vi.size;
    return fi;
  }
  constexpr std::string_view kFA = "FixedArray<";
  if (t.starts_with(kFA) && t.ends_with(">")) {
    auto inner = t.substr(kFA.size(), t.size() - kFA.size() - 1);  // "T , N"
    auto comma = inner.rfind(',');
    if (comma == std::string_view::npos) return {};
    auto elem = _packable_trim(inner.substr(0, comma));
    auto nstr = _packable_trim(inner.substr(comma + 1));
    auto ei = packable_type_info(elem);
    if (ei.size == 0) return {};  // element must be a fixed scalar
    long n = 0;
    auto [ptr, ec] =
        std::from_chars(nstr.data(), nstr.data() + nstr.size(), n);
    if (ec != std::errc() || ptr != nstr.data() + nstr.size() || n <= 0)
      return {};
    PackableFieldInfo fi;
    fi.is_fixed_array = true;
    fi.elem_type = std::string(elem);
    fi.capacity = static_cast<size_t>(n);
    fi.elem_size = ei.size;
    fi.align = std::max<size_t>(4, ei.align);   // the inline len is an i32
    fi.data_offset = packable_align_up(4, ei.align);
    fi.size = fi.data_offset + fi.capacity * ei.size;
    return fi;
  }
  // A registered @packable enum used by name -> a tagged-union field.
  if (auto* el = lookup_packable_enum(t)) {
    PackableFieldInfo fi;
    fi.is_enum = true;
    fi.elem_type = std::string(t);
    fi.size = el->size;
    fi.align = el->align;
    return fi;
  }
  auto si = packable_type_info(t);
  if (si.size == 0) return {};
  PackableFieldInfo fi;
  fi.size = si.size;
  fi.align = si.align;
  return fi;
}

inline bool is_packable_type(std::string_view t) {
  return packable_field_info(t).size != 0;
}

// Small integer code for a packable scalar, so a JIT FixedArray view can
// store its element type in a numeric slot (no JIT-string allocation) and
// map back to the name for the byte read/write switch. -1 = not a scalar.
inline int packable_scalar_code(std::string_view t) {
  if (t == "Float32") return 0;
  if (t == "Float64" || t == "Float") return 1;
  if (t == "Int8") return 2;
  if (t == "Int16") return 3;
  if (t == "Int32") return 4;
  if (t == "Int64" || t == "Long") return 5;
  if (t == "Byte") return 6;
  if (t == "Bool") return 7;
  return -1;
}
inline std::string_view packable_scalar_name(int code) {
  switch (code) {
    case 0: return "Float32";
    case 1: return "Float64";
    case 2: return "Int8";
    case 3: return "Int16";
    case 4: return "Int32";
    case 5: return "Int64";
    case 6: return "Byte";
    case 7: return "Bool";
  }
  return "";
}

// A single fixed-layout field: its name, the type token as written
// (`Float32`, `FixedArray<Int32, 8>`, ...), and the byte offset/size from
// C-ABI natural alignment. FixedArray fields carry their element layout so
// the view can address `[len][T × N]` inline.
struct PackableField {
  std::string name;
  std::string type;
  size_t offset;
  size_t size;
  bool is_fixed_array = false;
  bool is_fixed_string = false;
  bool is_fixed_set = false;
  bool is_fixed_map = false;
  bool is_optional = false;
  bool is_enum = false;
  bool is_bytes = false;
  std::string elem_type;
  std::string val_type;
  size_t capacity = 0;
  size_t elem_size = 0;
  size_t val_size = 0;
  size_t data_offset = 0;  // offset of T[0]/K[0] within the field
  size_t val_offset = 0;   // offset of V[0] within the field (FixedMap)
};

// FNV-1a over `n` key bytes — the hash for open-addressed Fixed{Set,Map}.
// Byte-level so interp and JIT share one hash (the keys are fixed scalars,
// already canonicalized into their field bytes before hashing).
inline uint64_t fixed_hash_bytes(const uint8_t* key, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= key[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// Slot states for an open-addressed Fixed{Set,Map}.
enum : uint8_t { kFixedEmpty = 0, kFixedFull = 1, kFixedTomb = 2 };

// Linear-probe for `key` (key_size bytes) among `cap` slots. `keys` is the
// key array with element stride `stride`. Returns the slot index of an
// existing equal key, or -1 if absent; `*out_insert` receives the first
// reusable slot (empty or tombstone) for an insert, or -1 if the table is
// full. Shared by interp and JIT so probing/equality stay identical.
inline long fixed_probe(const uint8_t* states, const uint8_t* keys, size_t cap,
                        size_t key_size, size_t stride, const uint8_t* key,
                        long* out_insert) {
  if (out_insert) *out_insert = -1;
  if (cap == 0) return -1;
  uint64_t h = fixed_hash_bytes(key, key_size);
  long insert = -1;
  for (size_t probe = 0; probe < cap; probe++) {
    size_t i = (h + probe) % cap;
    uint8_t st = states[i];
    if (st == kFixedEmpty) {
      if (insert < 0) insert = static_cast<long>(i);
      if (out_insert) *out_insert = insert;
      return -1;  // an empty slot terminates the probe chain
    }
    if (st == kFixedTomb) {
      if (insert < 0) insert = static_cast<long>(i);
      continue;
    }
    if (std::memcmp(keys + i * stride, key, key_size) == 0) {
      if (out_insert) *out_insert = static_cast<long>(i);
      return static_cast<long>(i);
    }
  }
  if (out_insert) *out_insert = insert;
  return -1;  // table full, key not present
}

// Full layout of a @packable class: ordered fields + total stride (record
// size, rounded up to the record alignment) and the record alignment.
struct PackableLayout {
  std::vector<PackableField> fields;
  size_t stride = 0;
  size_t align = 1;

  const PackableField* find(std::string_view name) const {
    for (const auto& f : fields)
      if (f.name == name) return &f;
    return nullptr;
  }
};

// Compute the C-ABI layout for an ordered list of (name, type) fields.
// Throws SyntaxError on a non-fixed field type — that is the @packable
// constraint surfacing at declaration time.
inline PackableLayout compute_packable_layout(
    std::string_view class_name,
    const std::vector<std::pair<std::string, std::string>>& fields) {
  PackableLayout layout;
  size_t off = 0;
  for (const auto& [name, type] : fields) {
    auto info = packable_field_info(type);
    if (info.size == 0) {
      throw CulebraError(
          "SyntaxError",
          std::format("@packable class `{}`: field `{}` has non-packable "
                      "type `{}` (expected a fixed scalar — Float32/Float64/"
                      "Int8/Int16/Int32/Int64/Byte/Bool — or "
                      "FixedArray<scalar, N> / FixedString<N> / "
                      "FixedSet<scalar, N> / FixedMap<scalar, scalar, N> / "
                      "Bytes<N> / an optional scalar `T?` / a @packable enum)",
                      class_name, name, type));
    }
    off = packable_align_up(off, info.align);
    PackableField f;
    f.name = name;
    f.type = type;
    f.offset = off;
    f.size = info.size;
    f.is_fixed_array = info.is_fixed_array;
    f.is_fixed_string = info.is_fixed_string;
    f.is_fixed_set = info.is_fixed_set;
    f.is_fixed_map = info.is_fixed_map;
    f.is_optional = info.is_optional;
    f.is_enum = info.is_enum;
    f.is_bytes = info.is_bytes;
    f.elem_type = info.elem_type;
    f.val_type = info.val_type;
    f.capacity = info.capacity;
    f.elem_size = info.elem_size;
    f.val_size = info.val_size;
    f.data_offset = info.data_offset;
    f.val_offset = info.val_offset;
    layout.fields.push_back(std::move(f));
    off += info.size;
    layout.align = std::max(layout.align, info.align);
  }
  layout.stride = packable_align_up(off, layout.align);
  return layout;
}

// Registry of @packable class layouts, keyed by class name. Populated at
// class-declaration time (interp eval_class_decl), read by SharedBuffer.new.
inline std::map<std::string, PackableLayout, std::less<>>&
packable_layout_registry() {
  static std::map<std::string, PackableLayout, std::less<>> reg;
  return reg;
}
inline std::mutex& packable_layout_mutex() {
  static std::mutex m;
  return m;
}
inline void register_packable_layout(std::string name, PackableLayout layout) {
  std::lock_guard<std::mutex> lk(packable_layout_mutex());
  packable_layout_registry()[std::move(name)] = std::move(layout);
}
inline const PackableLayout* lookup_packable_layout(std::string_view name) {
  std::lock_guard<std::mutex> lk(packable_layout_mutex());
  auto it = packable_layout_registry().find(name);
  return it == packable_layout_registry().end() ? nullptr : &it->second;
}

// --- with_lock escape hatch: the cross-process lock header ----------------
// A File/Shared buffer reserves a fixed-size header at the very front of its
// mapped region holding a PROCESS_SHARED pthread mutex; records start right
// after it (`data = map_base + kLockHeader`). Heap buffers carry no header and
// use a plain std::mutex in the core — they never leave the process. The
// header is a fixed 128 bytes: room for a pthread_mutex_t on every supported
// platform (macOS 64B, Linux 40B) plus the ready flag, and a multiple of the
// 8-byte max record alignment so the records stay naturally aligned.
inline constexpr size_t kLockHeader = 128;
struct SharedLockHeader {
  uint32_t ready;        // 0 = mutex uninitialized, 1 = ready (read via atomic_ref)
  uint32_t _pad;
  pthread_mutex_t mutex;
};
static_assert(sizeof(SharedLockHeader) <= kLockHeader,
              "the pthread lock header must fit in the reserved prefix");
// Bounded escape for the creator-init race: a receiver/reopener yields up to
// this many times for the creator to publish the lock, then gives up rather
// than spin forever on a wedged creator. The creator finishes within
// microseconds, so the cap is never reached in practice.
inline constexpr long kInitSpinLimit = 100'000'000;

// Backing store for a SharedBuffer: a flat byte vector holding `count`
// records of `layout.stride` bytes each. A global registry owns the core
// and culebra references it by integer id — the same indirection Channels
// use, since a Value can't carry a raw shared_ptr. `bytes` is a shared_ptr
// so the zero-copy cross-isolate share lane can later hand the same
// storage to another isolate (C3 step ②→③).
struct SharedBufferCore {
  // Storage-independent view: every reader/writer addresses records through
  // `data`. Heap owns a vector; File/Shared map an fd. For File/Shared, `data`
  // points past the kLockHeader prefix; `map_base` is the raw mmap base (used
  // for munmap/msync and to reach the lock header). Heap leaves map_base null.
  uint8_t* data = nullptr;
  uint8_t* map_base = nullptr;
  size_t byte_size = 0;   // mapped size (File/Shared: kLockHeader + records)
  enum class Storage { Heap, File, Shared };
  Storage storage = Storage::Heap;
  // Heap: owns the vector here. File/Shared: the mmap is munmap'd + fd closed in
  // the destructor (heap_bytes stays empty).
  std::shared_ptr<std::vector<uint8_t>> heap_bytes;
  std::mutex heap_mutex;  // Heap with_lock (cross-isolate, same process)
  int fd = -1;            // File/Shared: backing fd (for msync/close/inherit)
  std::string name;       // File: the path (introspection / cross-proc by path)
  PackableLayout layout;
  std::string class_name;
  size_t count = 0;
  // Live handle count (guarded by shared_buffer_mutex). The core — and its
  // bytes — is reclaimed when the last handle drops. Crossing an isolate
  // boundary bumps it (the child gets its own handle); a transient in-flight
  // bump covers the serialize→rebuild gap. Mirrors ChannelCore.tx_count.
  long refcount = 0;

  SharedBufferCore() = default;
  SharedBufferCore(const SharedBufferCore&) = delete;  // only held by shared_ptr
  SharedBufferCore& operator=(const SharedBufferCore&) = delete;
  ~SharedBufferCore() {
    if (storage != Storage::Heap && map_base) munmap(map_base, byte_size);
    if (fd >= 0) close(fd);
  }
};

// --- Lock header accessors + lifecycle ------------------------------------
// The pthread mutex lives at the front of a File/Shared mapping; heap has none.
inline SharedLockHeader* shared_lock_header(const SharedBufferCore& core) {
  return reinterpret_cast<SharedLockHeader*>(core.map_base);
}
// Creator side: initialize the PROCESS_SHARED mutex, then publish ready=1 so
// receivers (fork children / file reopeners) may use it. The backing pages are
// zero-filled (ftruncate), so ready starts at 0.
inline void shared_lock_init(SharedBufferCore& core) {
  auto* h = shared_lock_header(core);
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&h->mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  std::atomic_ref<uint32_t>(h->ready).store(1, std::memory_order_release);
}
// Receiver side: spin until the creator has published the mutex. For `.shared`
// the parent initializes before forking, so the child sees ready=1 at once;
// only a racing file reopener actually waits.
inline void shared_lock_wait_ready(SharedBufferCore& core) {
  std::atomic_ref<uint32_t> ready(shared_lock_header(core)->ready);
  for (long i = 0; ready.load(std::memory_order_acquire) != 1; ++i) {
    if (i > kInitSpinLimit) {
      throw CulebraError("IOError",
          "SharedBuffer: timed out waiting for the lock to initialize");
    }
    sched_yield();
  }
}
inline void shared_buffer_lock(SharedBufferCore& core) {
  if (core.storage == SharedBufferCore::Storage::Heap) core.heap_mutex.lock();
  else pthread_mutex_lock(&shared_lock_header(core)->mutex);
}
inline void shared_buffer_unlock(SharedBufferCore& core) {
  if (core.storage == SharedBufferCore::Storage::Heap) core.heap_mutex.unlock();
  else pthread_mutex_unlock(&shared_lock_header(core)->mutex);
}
// RAII lock guard that also pins the core alive across the user callback (so a
// callback that drops the buffer can't free the mutex out from under us).
struct SharedBufferLockGuard {
  std::shared_ptr<SharedBufferCore> core;
  explicit SharedBufferLockGuard(std::shared_ptr<SharedBufferCore> c)
      : core(std::move(c)) {
    shared_buffer_lock(*core);
  }
  ~SharedBufferLockGuard() { shared_buffer_unlock(*core); }
  SharedBufferLockGuard(const SharedBufferLockGuard&) = delete;
  SharedBufferLockGuard& operator=(const SharedBufferLockGuard&) = delete;
};

inline std::map<long, std::shared_ptr<SharedBufferCore>>&
shared_buffer_registry() {
  static std::map<long, std::shared_ptr<SharedBufferCore>> reg;
  return reg;
}
inline std::mutex& shared_buffer_mutex() {
  static std::mutex m;
  return m;
}
inline std::atomic<long>& shared_buffer_next_id() {
  static std::atomic<long> id{1};
  return id;
}
inline long register_shared_buffer(std::shared_ptr<SharedBufferCore> core) {
  long id = shared_buffer_next_id().fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(shared_buffer_mutex());
  shared_buffer_registry()[id] = std::move(core);
  return id;
}
inline std::shared_ptr<SharedBufferCore> lookup_shared_buffer(long id) {
  std::lock_guard<std::mutex> lk(shared_buffer_mutex());
  auto it = shared_buffer_registry().find(id);
  return it == shared_buffer_registry().end() ? nullptr : it->second;
}

// Allocate a zero-initialized SharedBuffer of `count` records laid out per
// `layout`, register it (refcount 1 for the handle the caller builds), and
// return its id. Pure metadata + raw bytes (no Value / JitValue), so interp
// and JIT share this construction.
inline long make_shared_buffer(const PackableLayout& layout,
                               std::string class_name, size_t count) {
  auto core = std::make_shared<SharedBufferCore>();
  core->byte_size = layout.stride * count;
  core->heap_bytes = std::make_shared<std::vector<uint8_t>>(core->byte_size, 0);
  core->data = core->heap_bytes->data();  // fixed-size vector → never reallocs
  core->storage = SharedBufferCore::Storage::Heap;
  core->layout = layout;
  core->class_name = std::move(class_name);
  core->count = count;
  core->refcount = 1;
  return register_shared_buffer(std::move(core));
}

// File-backed SharedBuffer: mmap `count` records over `path` (created/sized if
// needed). The region is `kLockHeader` (a PROCESS_SHARED lock) followed by the
// records, so a buffer shared by re-opening the same path can synchronize with
// with_lock. Writes go to the file's page cache (visible to other processes
// that mmap the same path; durable after flush/close). The file persists; the
// user deletes it like any file. The process that wins an exclusive create
// initializes the lock; everyone else waits for it. Throws IOError on an OS
// failure.
inline long make_shared_buffer_file(const PackableLayout& layout,
                                    std::string class_name, size_t count,
                                    const std::string& path) {
  size_t bytes = kLockHeader + layout.stride * count;
  // O_EXCL picks exactly one creator; the loser reopens the existing file.
  bool creator = true;
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    creator = false;
    fd = ::open(path.c_str(), O_RDWR, 0644);
    if (fd < 0) {
      throw CulebraError("IOError",
          std::format("SharedBuffer.file: cannot open `{}`", path));
    }
  }
  if (creator) {
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      ::close(fd);
      throw CulebraError("IOError",
          std::format("SharedBuffer.file: cannot size `{}`", path));
    }
  } else {
    // The creator may still be sizing the file; wait until it's big enough.
    for (long i = 0;; ++i) {
      struct stat st;
      if (::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) >= bytes)
        break;
      if (i > kInitSpinLimit) {
        ::close(fd);
        throw CulebraError("IOError",
            std::format("SharedBuffer.file: `{}` not ready", path));
      }
      sched_yield();
    }
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    throw CulebraError("IOError",
        std::format("SharedBuffer.file: cannot map `{}`", path));
  }
  auto core = std::make_shared<SharedBufferCore>();
  core->map_base = static_cast<uint8_t*>(p);
  core->data = core->map_base + kLockHeader;
  core->byte_size = bytes;
  core->storage = SharedBufferCore::Storage::File;
  core->fd = fd;
  core->name = path;
  core->layout = layout;
  core->class_name = std::move(class_name);
  core->count = count;
  core->refcount = 1;
  if (creator) shared_lock_init(*core);
  else shared_lock_wait_ready(*core);
  return register_shared_buffer(std::move(core));
}

// Create an anonymous shared-memory fd of `bytes` bytes — RAM only, no name on
// disk. The fd is CLOEXEC by default so it does NOT leak into unrelated Proc
// children; the `share:` mechanism clears CLOEXEC on exactly the child that
// opts in. Linux: memfd_create. macOS/BSD: shm_open a unique name, then
// shm_unlink it immediately so only the live fd keeps the object alive (the
// kernel refcounts it across processes; the last close frees it). Throws
// IOError on an OS failure.
inline int make_anon_shm_fd(size_t bytes) {
  int fd = -1;
#if defined(__linux__)
  // MFD_CLOEXEC == 1U; spell it literally to avoid a header-version dependency.
  fd = static_cast<int>(::syscall(SYS_memfd_create, "culebra_shm", 1U));
  if (fd < 0)
    throw CulebraError("IOError", "SharedBuffer.shared: memfd_create failed");
#else
  // POSIX shm: a unique name (pid + counter), created exclusively, then
  // unlinked right away. PSHMNAMLEN is small (31 on macOS), so keep it short.
  static std::atomic<unsigned> ctr{0};
  unsigned k = ctr.fetch_add(1, std::memory_order_relaxed);
  char nm[32];
  std::snprintf(nm, sizeof(nm), "/cul_%d_%u", static_cast<int>(::getpid()), k);
  fd = ::shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    throw CulebraError("IOError", "SharedBuffer.shared: shm_open failed");
  ::shm_unlink(nm);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    throw CulebraError("IOError", "SharedBuffer.shared: cannot size the buffer");
  }
  return fd;
}

// mmap an open fd of `bytes` bytes (kLockHeader + records) MAP_SHARED into a
// fresh SharedBufferCore. `data` lands past the lock header. Shared by
// make_shared_buffer_shared (owns the fd it created) and the child's
// make_shared_buffer_receive (owns the fd it inherited). The caller initializes
// or waits on the lock; the core closes the fd + munmaps in its destructor
// either way. Throws IOError on a map failure.
inline long _adopt_shared_fd(const PackableLayout& layout,
                             std::string class_name, size_t count, int fd,
                             size_t bytes) {
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    throw CulebraError("IOError", "SharedBuffer: cannot map shared memory");
  }
  auto core = std::make_shared<SharedBufferCore>();
  core->map_base = static_cast<uint8_t*>(p);
  core->data = core->map_base + kLockHeader;
  core->byte_size = bytes;
  core->storage = SharedBufferCore::Storage::Shared;
  core->fd = fd;
  core->layout = layout;
  core->class_name = std::move(class_name);
  core->count = count;
  core->refcount = 1;
  return register_shared_buffer(std::move(core));
}

// Anonymous-fd SharedBuffer: a name-less RAM region shared with child processes
// via fd inheritance (Proc `share:`). Same buffer interface as heap/file; the
// fd lives in the core and ships to the child by number. The parent is the
// creator, so it initializes the lock before any child can fork. Throws
// IOError on an OS failure.
inline long make_shared_buffer_shared(const PackableLayout& layout,
                                      std::string class_name, size_t count) {
  size_t bytes = kLockHeader + layout.stride * count;
  int fd = make_anon_shm_fd(bytes);
  long id = _adopt_shared_fd(layout, std::move(class_name), count, fd, bytes);
  shared_lock_init(*lookup_shared_buffer(id));
  return id;
}

// Child side of `share:`: mmap an fd inherited from the parent. `count` and the
// byte size are the parent's (passed through the environment); this only maps
// the bytes and waits for the parent-initialized lock. Throws IOError on a map
// failure.
inline long make_shared_buffer_receive(const PackableLayout& layout,
                                       std::string class_name, size_t count,
                                       int fd) {
  long id = _adopt_shared_fd(layout, std::move(class_name), count, fd,
                             kLockHeader + layout.stride * count);
  shared_lock_wait_ready(*lookup_shared_buffer(id));
  return id;
}

// The environment-variable name a `share:` buffer travels under, keyed by the
// user's name for it. The parent sets it (`fd:bytes:stride:count`), the child's
// SharedBuffer.receive reads it. One place so both ends agree on the spelling.
inline std::string share_env_key(std::string_view name) {
  return "CULEBRA_SHARE_" + std::string(name);
}
inline std::string share_env_value(int fd, size_t bytes, size_t stride,
                                   size_t count) {
  return std::format("{}:{}:{}:{}", fd, bytes, stride, count);
}

// Parent side of `Proc.run(..., share: {name: buf})`: validate that `buf` is an
// anonymous-fd (Shared) buffer and return its fd plus the env value the child
// reads. Only `.shared()` buffers cross a process boundary this way — heap is
// isolate-local, and a file buffer is shared by re-opening its path. The fd
// isn't touched here (CLOEXEC is cleared in the child, for that child only).
// Throws ValueError on a dropped or wrong-storage buffer.
inline std::pair<int, std::string> prepare_share_buffer(long id,
                                                        std::string_view name) {
  auto core = lookup_shared_buffer(id);
  if (!core) {
    throw CulebraError(
        "ValueError",
        std::format("Proc share `{}`: SharedBuffer has been dropped", name));
  }
  if (core->storage != SharedBufferCore::Storage::Shared) {
    throw CulebraError(
        "ValueError",
        std::format("Proc share `{}`: only a SharedBuffer.shared(...) buffer "
                    "can be shared with a child process (heap is isolate-local; "
                    "share a file buffer by re-opening its path)",
                    name));
  }
  return {core->fd, share_env_value(core->fd, core->byte_size,
                                    core->layout.stride, core->count)};
}

// Child side of `SharedBuffer.receive(name, T)`: look the buffer up in the
// environment (it was passed via Proc.run `share:`), validate the parent's
// record stride against this process's @packable layout (a cheap drift guard),
// then mmap the inherited fd. `count` is the parent's — the child never states
// it. Throws ValueError if the name isn't present or the layouts disagree.
inline long make_shared_buffer_from_share_env(const PackableLayout& layout,
                                              std::string class_name,
                                              std::string_view name) {
  std::string key = share_env_key(name);
  const char* v = std::getenv(key.c_str());
  if (!v) {
    throw CulebraError(
        "ValueError",
        std::format("SharedBuffer.receive: no shared buffer named `{}` "
                    "(pass it from the parent via Proc.run share:)",
                    name));
  }
  long fd = -1, bytes = -1, stride = -1, count = -1;
  if (std::sscanf(v, "%ld:%ld:%ld:%ld", &fd, &bytes, &stride, &count) != 4 ||
      fd < 0 || bytes < 0 || stride < 0 || count < 0) {
    throw CulebraError(
        "ValueError",
        std::format("SharedBuffer.receive: malformed share entry for `{}`",
                    name));
  }
  if (static_cast<size_t>(stride) != layout.stride) {
    throw CulebraError(
        "ValueError",
        std::format("SharedBuffer.receive: layout mismatch for `{}` (parent "
                    "record is {} bytes, this @packable `{}` is {})",
                    name, stride, class_name, layout.stride));
  }
  return make_shared_buffer_receive(layout, std::move(class_name),
                                    static_cast<size_t>(count),
                                    static_cast<int>(fd));
}

// Flush a file-backed buffer's dirty pages to disk (msync). No-op for Heap.
inline void shared_buffer_flush(long id) {
  auto core = lookup_shared_buffer(id);
  if (core && core->storage != SharedBufferCore::Storage::Heap &&
      core->map_base && core->byte_size) {
    ::msync(core->map_base, core->byte_size, MS_SYNC);
  }
}

// Adjust a buffer's live-handle count (+1 on cross-isolate transfer / rebuild,
// in-flight protection during serialization).
inline void shared_buffer_bump(long id, long delta) {
  std::lock_guard<std::mutex> lk(shared_buffer_mutex());
  auto it = shared_buffer_registry().find(id);
  if (it != shared_buffer_registry().end()) it->second->refcount += delta;
}

// Drop one handle; reclaim the core (freeing its bytes) when the count hits 0.
// The shared_ptr is released outside the registry lock so a concurrent
// lookup that still holds a copy stays valid.
inline void shared_buffer_drop(long id) {
  std::shared_ptr<SharedBufferCore> dead;
  {
    std::lock_guard<std::mutex> lk(shared_buffer_mutex());
    auto it = shared_buffer_registry().find(id);
    if (it == shared_buffer_registry().end()) return;
    if (--it->second->refcount <= 0) {
      dead = it->second;
      shared_buffer_registry().erase(it);
    }
  }
}

}  // namespace culebra
