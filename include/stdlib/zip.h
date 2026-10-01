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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdlib/vfs.h>  // dir_path_normalize

#if !defined(CULEBRA_RT_COMPRESS_WEAK) && defined(CULEBRA_ENABLE_ZIP)
#define CULEBRA_ZIP_REAL 1
#include <zipper.h>
#endif

namespace culebra::zip {

// One file of an archive: where its entry sits (minizip's unz64_file_pos, so
// a read goes straight to it rather than searching by name) and its size.
struct Entry {
  uint64_t pos_in_central_dir = 0;
  uint64_t num_of_file = 0;
  uint64_t size = 0;
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

  // Records the entry `name` (as the archive wrote it). `\` — which some
  // Windows tools write — is a separator here, and a name that starts with a
  // drive (`C:`) is as absolute as one that starts with `/`: such an entry, or
  // one that climbs with `..`, could never be read by path and is left out.
  // When two entries name one file, the first wins.
  void add(std::string name, const Entry& e) {
    std::replace(name.begin(), name.end(), '\\', '/');
    bool dir = !name.empty() && name.back() == '/';
    if (name.size() >= 2 && name[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(name[0])))
      return;
    std::string key;
    if (!dir_path_normalize(name, key) || key.empty()) return;
    if (dir) {
      dirs.insert(key);
    } else {
      files.emplace(key, e);
    }
    for (auto slash = key.rfind('/'); slash != std::string::npos && slash > 0;
         slash = key.rfind('/', slash - 1))
      dirs.insert(key.substr(0, slash));
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
struct Reader {
  std::string bytes;  // an in-memory archive: unzip reads these in place
  zipper::UnZip unzip;
};

// Walks the central directory into the index. The cursor stops on an error
// as it does at the end, so `error()` is what tells the two apart.
inline Opened _index(Reader* r) {
  Opened o;
  r->unzip.enumerate([&](zipper::UnZip& u) {
    unz64_file_pos pos{};
    if (unzGetFilePos64(u, &pos) != UNZ_OK) return;
    o.index.add(u.file_path(), {pos.pos_in_zip_directory, pos.num_of_file,
                                u.file_size()});
  });
  if (!r->unzip.error().empty()) {
    o.error = r->unzip.error();
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

// Opens the archive held in `bytes`, which the reader keeps.
CULEBRA_ZIP_LINKAGE Opened open_bytes(std::string bytes) {
#if defined(CULEBRA_ZIP_REAL)
  auto* r = new Reader;
  r->bytes = std::move(bytes);
  if (!r->unzip.open_memory(r->bytes.data(), r->bytes.size())) {
    Opened o;
    o.error = r->unzip.error();
    delete r;
    return o;
  }
  return _index(r);
#else
  (void)bytes;
  Opened o;
  o.error = CULEBRA_ZIP_STUB;
  return o;
#endif
}

// The bytes of `e`; false with the reason (encrypted, an unsupported method,
// a CRC mismatch, damage) in `error`.
CULEBRA_ZIP_LINKAGE bool read(Reader* r, const Entry& e, std::string& out,
                              std::string& error) {
#if defined(CULEBRA_ZIP_REAL)
  unz64_file_pos pos{e.pos_in_central_dir, e.num_of_file};
  if (unzGoToFilePos64(r->unzip, &pos) != UNZ_OK) {
    error = "not a ZIP archive, or a damaged one";
    return false;
  }
  out.clear();
  if (!r->unzip.read(out)) {
    error = r->unzip.error();
    return false;
  }
  return true;
#else
  (void)r;
  (void)e;
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

#undef CULEBRA_ZIP_STUB
#undef CULEBRA_ZIP_UNLINKED
#undef CULEBRA_ZIP_UNAVAILABLE
#undef CULEBRA_ZIP_LINKAGE

}  // namespace culebra::zip
