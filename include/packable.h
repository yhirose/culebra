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

#include <fcntl.h>      // open
#include <sys/mman.h>   // mmap / munmap / msync (POSIX; file/shm storage)
#include <unistd.h>     // close / ftruncate

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

// Layout of a whole field — a scalar, or a `FixedArray<T, N>` (a fixed-
// capacity inline collection: `[len:i32][T × N]`, no pointers, so it fits a
// @packable record). size 0 means the type isn't a packable field type.
struct PackableFieldInfo {
  size_t size = 0;
  size_t align = 1;
  bool is_fixed_array = false;
  std::string elem_type;     // FixedArray element scalar type
  size_t capacity = 0;       // N
  size_t elem_size = 0;      // sizeof(T)
  size_t data_offset = 0;    // byte offset of T[0] within the field (after len)
};

inline PackableFieldInfo packable_field_info(std::string_view t) {
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
  auto si = packable_type_info(t);
  if (si.size == 0) return {};
  return {si.size, si.align, false, "", 0, 0, 0};
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
  std::string elem_type;
  size_t capacity = 0;
  size_t elem_size = 0;
  size_t data_offset = 0;  // offset of T[0] within the field (after len)
};

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
                      "FixedArray<scalar, N>)",
                      class_name, name, type));
    }
    off = packable_align_up(off, info.align);
    PackableField f;
    f.name = name;
    f.type = type;
    f.offset = off;
    f.size = info.size;
    f.is_fixed_array = info.is_fixed_array;
    f.elem_type = info.elem_type;
    f.capacity = info.capacity;
    f.elem_size = info.elem_size;
    f.data_offset = info.data_offset;
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

// Backing store for a SharedBuffer: a flat byte vector holding `count`
// records of `layout.stride` bytes each. A global registry owns the core
// and culebra references it by integer id — the same indirection Channels
// use, since a Value can't carry a raw shared_ptr. `bytes` is a shared_ptr
// so the zero-copy cross-isolate share lane can later hand the same
// storage to another isolate (C3 step ②→③).
struct SharedBufferCore {
  // Storage-independent view: every reader/writer addresses bytes through
  // `data`, regardless of where they live. Heap owns a vector; File maps a
  // file (Shared/fd lands in Phase C). `data` always points at the bytes.
  uint8_t* data = nullptr;
  size_t byte_size = 0;
  enum class Storage { Heap, File };
  Storage storage = Storage::Heap;
  // Heap: owns the vector here. File: the mmap is munmap'd + fd closed in the
  // destructor (heap_bytes stays empty).
  std::shared_ptr<std::vector<uint8_t>> heap_bytes;
  int fd = -1;            // File: backing fd (for msync/close)
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
    if (storage != Storage::Heap && data) munmap(data, byte_size);
    if (fd >= 0) close(fd);
  }
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
// needed). Writes go to the file's page cache (visible to other processes that
// mmap the same path; durable after flush/close). The file persists; the user
// deletes it like any file. Throws IOError on an OS failure.
inline long make_shared_buffer_file(const PackableLayout& layout,
                                    std::string class_name, size_t count,
                                    const std::string& path) {
  size_t bytes = layout.stride * count;
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    throw CulebraError("IOError",
        std::format("SharedBuffer.file: cannot open `{}`", path));
  }
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    throw CulebraError("IOError",
        std::format("SharedBuffer.file: cannot size `{}`", path));
  }
  void* p = bytes ? ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, 0)
                  : nullptr;
  if (bytes && p == MAP_FAILED) {
    ::close(fd);
    throw CulebraError("IOError",
        std::format("SharedBuffer.file: cannot map `{}`", path));
  }
  auto core = std::make_shared<SharedBufferCore>();
  core->data = static_cast<uint8_t*>(p);
  core->byte_size = bytes;
  core->storage = SharedBufferCore::Storage::File;
  core->fd = fd;
  core->name = path;
  core->layout = layout;
  core->class_name = std::move(class_name);
  core->count = count;
  core->refcount = 1;
  return register_shared_buffer(std::move(core));
}

// Flush a file-backed buffer's dirty pages to disk (msync). No-op for Heap.
inline void shared_buffer_flush(long id) {
  auto core = lookup_shared_buffer(id);
  if (core && core->storage != SharedBufferCore::Storage::Heap && core->data &&
      core->byte_size) {
    ::msync(core->data, core->byte_size, MS_SYNC);
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
