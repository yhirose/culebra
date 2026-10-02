#pragma once

// Value-neutral ZIP core for Dir.zip and Compress.zip, over MiniZip through
// cpp-zipper (vendor/minizip, vendor/cpp-zipper). No culebra Value type: the
// natives (bindings.h `_Dir.zip_*`, `Compress.zip`) adapt it.
//
// An archive's index is plain data, built when it opens: each file's
// normalized path, its size and where its entry sits. So asking what an
// archive holds never reaches minizip — only open / read / close / pack do,
// and those four are the choke the AOT runtime partitions exactly like
// compress.h's gzip (they share its zlib):
//   - core archive     (CULEBRA_RT_COMPRESS_WEAK):   weak stubs, no minizip
//     symbol, so a program that never opens or packs an archive links none.
//   - compress archive (CULEBRA_RT_COMPRESS_STRONG): the real bodies, with the
//     minizip objects beside them, force-loaded when the program names `zip`.
//   - in-process (neither): the inline bodies.
// A build without minizip (CULEBRA_ENABLE_ZIP off — the WASM playground)
// answers every call with "not available in this build".

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdlib/vfs.h>  // Dir, dir_path_normalize

#if !defined(CULEBRA_RT_COMPRESS_WEAK) && defined(CULEBRA_ENABLE_ZIP)
#define CULEBRA_ZIP_REAL 1
#include <zipper.h>
#endif

namespace culebra::zip {

// One file of an archive: where its entry sits (minizip's unz64_file_pos, so
// a read goes straight to it rather than searching by name), its size, and
// what a read checks the entry it lands on against (crc, compressed size).
struct Entry {
  uint64_t pos_in_central_dir = 0;
  uint64_t num_of_file = 0;
  uint64_t size = 0;
  uint64_t compressed_size = 0;
  uint32_t crc = 0;
};

// What an archive holds, keyed by normalized path. A directory is there when
// an entry names it or a file lies under it — many archives record no
// directory entries at all — and the root always is.
struct Index {
  std::map<std::string, Entry> files;
  std::set<std::string> dirs;

  bool is_dir(std::string_view key) const {
    return key.empty() || dirs.count(std::string(key)) != 0;
  }
  bool list_dir(std::string_view key, std::vector<std::string>& out) const {
    out.clear();
    if (!is_dir(key)) return false;
    std::string prefix = key.empty() ? std::string() : std::string(key) + "/";
    std::set<std::string> names;
    auto add = [&](const std::string& p) {
      if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix))
        return;
      auto rest = p.substr(prefix.size());
      names.insert(rest.substr(0, rest.find('/')));
    };
    for (const auto& [p, _] : files) add(p);
    for (const auto& p : dirs) add(p);
    out.assign(names.begin(), names.end());
    return true;
  }

  // The path an entry's `name` (as the archive wrote it) is read by, or false
  // when it is left out. `\` — which some Windows tools write — is a
  // separator here, and a path that starts with a drive (`C:`) is as absolute
  // as one that starts with `/`: such an entry, or one that climbs with `..`,
  // could never be read by path.
  static bool entry_key(std::string name, std::string& key) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return dir_path_normalize(name, key) && !key.empty() && !has_drive(key);
  }

  // Records the entry `name`. The first entry wins: over a later one naming
  // the same file, and over one that would make a file a directory too, or a
  // directory a file.
  void add(const std::string& name, const Entry& e) {
    bool dir = !name.empty() && (name.back() == '/' || name.back() == '\\');
    std::string key;
    if (!entry_key(name, key)) return;
    if (files.count(key) || (!dir && dirs.count(key))) return;
    std::vector<std::string> parents;
    for (auto slash = key.find('/'); slash != std::string::npos;
         slash = key.find('/', slash + 1)) {
      parents.push_back(key.substr(0, slash));
      if (files.count(parents.back())) return;
    }
    dirs.insert(parents.begin(), parents.end());
    if (dir) {
      dirs.insert(key);
    } else {
      files.emplace(key, e);
    }
  }
  static bool has_drive(std::string_view key) {
    return key.size() >= 2 && key[1] == ':' &&
           std::isalpha(static_cast<unsigned char>(key[0]));
  }
};

// The open archive itself: the minizip handle, and the bytes an in-memory
// archive reads in place. Defined only where minizip is.
struct Reader;

struct Opened {
  Reader* reader = nullptr;  // null when `error` says why
  Index index;
  std::string error;
};

#if defined(CULEBRA_RT_COMPRESS_STRONG)
#define CULEBRA_ZIP_LINKAGE
#elif defined(CULEBRA_RT_COMPRESS_WEAK)
#define CULEBRA_ZIP_LINKAGE __attribute__((weak))
#else
#define CULEBRA_ZIP_LINKAGE inline
#endif

#if defined(CULEBRA_ZIP_REAL)
// An archive opened in memory reads the caller's bytes and keeps no pointer
// to them: they are lent for one call (Lend) and taken back at its end, so
// nothing here can outlive the String the program holds.
struct Reader {
  zipper::UnZip unzip;
  bool in_memory = false;
};

// Points an in-memory archive at `bytes` for one scope, and at nothing after.
struct Lend {
  Reader* r;
  Lend(Reader* reader, std::string_view bytes) : r(reader) {
    if (r->in_memory) r->unzip.rebind_memory(bytes.data(), bytes.size());
  }
  ~Lend() {
    if (r->in_memory) r->unzip.rebind_memory(nullptr, 0);
  }
};

// The entry under minizip's cursor, its name included, in one header read.
inline bool _current(unzFile u, unz_file_info64& info, std::string& name) {
  char buf[512];
  if (unzGetCurrentFileInfo64(u, &info, buf, sizeof buf, nullptr, 0, nullptr,
                              0) != UNZ_OK)
    return false;
  if (info.size_filename <= sizeof buf) {
    name.assign(buf, info.size_filename);
    return true;
  }
  name.assign(info.size_filename, '\0');
  return unzGetCurrentFileInfo64(u, &info, name.data(), name.size(), nullptr,
                                 0, nullptr, 0) == UNZ_OK;
}

// Walks the central directory into the index. The cursor stops on an error
// as it does at the end, so `error()` is what tells the two apart. A size
// past what a Long holds is damage, not a file.
inline Opened _index(Reader* r) {
  Opened o;
  r->unzip.enumerate([&](zipper::UnZip& u) {
    unz64_file_pos pos{};
    unz_file_info64 info{};
    std::string name;
    if (unzGetFilePos64(u, &pos) != UNZ_OK || !_current(u, info, name) ||
        info.uncompressed_size > static_cast<uint64_t>(INT64_MAX)) {
      o.error = "not a ZIP archive, or a damaged one";
      return;
    }
    o.index.add(name, {pos.pos_in_zip_directory, pos.num_of_file,
                       info.uncompressed_size, info.compressed_size,
                       static_cast<uint32_t>(info.crc)});
  });
  if (o.error.empty()) o.error = r->unzip.error();
  if (!o.error.empty()) {
    delete r;
    return o;
  }
  o.reader = r;
  return o;
}
#endif

#define CULEBRA_ZIP_UNAVAILABLE                                         \
  "ZIP archives are not available in this build"
#define CULEBRA_ZIP_UNLINKED                                            \
  "runtime not linked (no zip use detected at build)"
#if defined(CULEBRA_RT_COMPRESS_WEAK)
#define CULEBRA_ZIP_STUB CULEBRA_ZIP_UNLINKED
#else
#define CULEBRA_ZIP_STUB CULEBRA_ZIP_UNAVAILABLE
#endif

// Opens the archive in the file at `path`, reading only its central directory.
CULEBRA_ZIP_LINKAGE Opened open_file(const std::string& path) {
#if defined(CULEBRA_ZIP_REAL)
  auto* r = new Reader;
  if (!r->unzip.open(path)) {
    Opened o;
    o.error = "not a ZIP archive, or a damaged one";
    delete r;
    return o;
  }
  return _index(r);
#else
  (void)path;
  Opened o;
  o.error = CULEBRA_ZIP_STUB;
  return o;
#endif
}

// Opens the archive held in `bytes`, which stay the caller's: they are read
// here to index the archive, and lent again to every read.
CULEBRA_ZIP_LINKAGE Opened open_bytes(std::string_view bytes) {
#if defined(CULEBRA_ZIP_REAL)
  auto* r = new Reader;
  if (!r->unzip.open_memory(bytes.data(), bytes.size())) {
    Opened o;
    o.error = r->unzip.error();
    delete r;
    return o;
  }
  r->in_memory = true;
  Opened o;
  {
    Lend lent(r, bytes);
    o = _index(r);
  }
  return o;
#else
  (void)bytes;
  Opened o;
  o.error = CULEBRA_ZIP_STUB;
  return o;
#endif
}

// The bytes of `e`, the entry indexed under `key`; false with the reason
// (encrypted, an unsupported method, a CRC mismatch, damage) in `error`. An
// archive opened in memory reads `bytes`, which should be what it opened.
// Whatever they are, the entry they hold where `e` sits has to be the one
// indexed (key, sizes and crc), or the read is refused as damage: an archive
// handed other bytes, or a file rewritten under an open archive, never reads
// as another entry.
CULEBRA_ZIP_LINKAGE bool read(Reader* r, std::string_view bytes,
                              const Entry& e, std::string_view key,
                              std::string& out, std::string& error) {
#if defined(CULEBRA_ZIP_REAL)
  Lend lent(r, bytes);
  unz64_file_pos pos{e.pos_in_central_dir, e.num_of_file};
  unz_file_info64 info{};
  std::string name, at;
  if (unzGoToFilePos64(r->unzip, &pos) != UNZ_OK ||
      !_current(r->unzip, info, name) || !Index::entry_key(name, at) ||
      at != key || info.crc != e.crc ||
      info.compressed_size != e.compressed_size ||
      info.uncompressed_size != e.size) {
    error = "not a ZIP archive, or a damaged one";
    return false;
  }
  out.clear();
  if (!r->unzip.read(out)) {
    error = r->unzip.error();
    return false;
  }
  // minizip checks the CRC only once the recorded size is read through, so
  // data that ends short of it would pass unchecked.
  if (out.size() != e.size) {
    error = "the data does not match its recorded size";
    return false;
  }
  return true;
#else
  (void)r;
  (void)bytes;
  (void)e;
  (void)key;
  (void)out;
  error = CULEBRA_ZIP_STUB;
  return false;
#endif
}

CULEBRA_ZIP_LINKAGE void close(Reader* r) {
#if defined(CULEBRA_ZIP_REAL)
  delete r;
#else
  (void)r;
#endif
}

// The bytes of an archive holding `files` (path, contents), in the order
// given. Every entry has the same fixed timestamp, so the same files make the
// same bytes. Paths are expected normalized; false with the reason in `error`.
CULEBRA_ZIP_LINKAGE bool pack(
    const std::vector<std::pair<std::string, std::string>>& files,
    std::string& out, std::string& error) {
#if defined(CULEBRA_ZIP_REAL)
  zipper::Zip zip;
  if (!zip.open_memory()) {
    error = zip.error();
    return false;
  }
  for (const auto& [path, data] : files) {
    if (!zip.add_file(path, data)) {
      error = path + ": " + zip.error();
      return false;
    }
  }
  if (!zip.close()) {
    error = zip.error();
    return false;
  }
  out = zip.buffer();
  return true;
#else
  (void)files;
  (void)out;
  error = CULEBRA_ZIP_STUB;
  return false;
#endif
}

// An archive as a Dir that owns everything it reads: what a server keeps of a
// `Dir.zip`, opened again from its path or over its own copy of the bytes, so
// it outlives the ZipArchive it came from and holds no Culebra value. Server
// threads share it, and the one minizip cursor reads under a lock.
struct ZipDir : Dir {
  std::string bytes;  // empty for an archive in a file
  Reader* reader = nullptr;
  Index index;
  mutable std::mutex m;

  ZipDir() = default;
  ZipDir(const ZipDir&) = delete;
  ZipDir& operator=(const ZipDir&) = delete;
  ~ZipDir() override {
    if (reader) close(reader);
  }
  // Null with the reason in `error` when the archive does not open.
  static std::unique_ptr<ZipDir> open(const std::string* path,
                                      std::string bytes, std::string& error) {
    auto d = std::make_unique<ZipDir>();
    d->bytes = std::move(bytes);
    Opened o = path ? open_file(*path) : open_bytes(d->bytes);
    if (!o.reader) {
      error = o.error;
      return nullptr;
    }
    d->reader = o.reader;
    d->index = std::move(o.index);
    return d;
  }
  bool read(std::string_view path, std::string& out) const override {
    auto it = index.files.find(std::string(path));
    if (it == index.files.end()) return false;
    std::lock_guard<std::mutex> lock(m);
    std::string error;
    return zip::read(reader, bytes, it->second, it->first, out, error);
  }
  bool is_file(std::string_view path) const override {
    return index.files.count(std::string(path)) != 0;
  }
  bool is_dir(std::string_view path) const override {
    return index.is_dir(path);
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    return index.list_dir(path, out);
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    auto it = index.files.find(std::string(path));
    if (it == index.files.end()) return false;
    out = it->second.size;
    return true;
  }
};

#undef CULEBRA_ZIP_STUB
#undef CULEBRA_ZIP_UNLINKED
#undef CULEBRA_ZIP_UNAVAILABLE
#undef CULEBRA_ZIP_LINKAGE

}  // namespace culebra::zip
