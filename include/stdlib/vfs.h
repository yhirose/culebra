#pragma once

// The C++ side of the Dir module: the read-only `Dir` interface, the path rule
// every kind answers by (dir_path_normalize), and the kinds that hold no
// Culebra value — `LiveDir` and `EmbeddedDir` behind Dir.embedded (live from
// disk run from source, a baked table in an AOT binary), `RootDir` behind
// Dir.disk, and `MapDir`, the copy of a Dir.memory a server keeps. A server
// serves any of them through `serve_static()`, so every lane answers a
// request byte for byte the same.
//
// This header is value-neutral: it touches no culebra Value type, so the
// lanes share one implementation (the CSV/TOML/http core pattern).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace culebra {

// A path as a Dir keys it: relative to the root, `/` separated, no `.` or
// empty segment (`a//b`, `./a`, `a/./b`, a trailing `/`), and `""` for the
// root itself (`"."` too). False for a path that starts with `/` or `\` or
// climbs with `..` — such a path is simply not there, never an error. A
// backslash is an ordinary character otherwise (`foo\bar` is one name); an
// archive that recorded its entries with `\` converts them as it indexes.
// The one rule for every kind of Dir (`Dir.disk` / `memory` / `embedded` /
// `zip`) and the _Dir.normalize native the preamble classes call.
inline bool dir_path_normalize(std::string_view path, std::string& out) {
  out.clear();
  if (!path.empty() && (path.front() == '/' || path.front() == '\\'))
    return false;
  for (size_t i = 0; i <= path.size();) {
    size_t j = path.find('/', i);
    if (j == std::string_view::npos) j = path.size();
    auto seg = path.substr(i, j - i);
    if (seg == "..") return false;
    if (!seg.empty() && seg != ".") {
      if (!out.empty()) out += '/';
      out += seg;
    }
    i = j + 1;
  }
  return true;
}

// The bare name directly under the directory `dir` on the way to `key` (both
// normalized), or false when `key` does not lie below `dir`.
inline bool dir_child(std::string_view key, std::string_view dir,
                      std::string_view& name) {
  if (!dir.empty()) {
    if (key.size() <= dir.size() || !key.starts_with(dir) ||
        key[dir.size()] != '/')
      return false;
    key.remove_prefix(dir.size() + 1);
  }
  if (key.empty()) return false;
  name = key.substr(0, key.find('/'));
  return true;
}

// The Dir module's classes as the natives know them: a class says which it
// is on its meta (JitSpecialTable::dir_kind, set by `_Dir.mark`), so a native
// handed a Dir (`srv.static`) knows it by its class, not by its name.
enum class DirKind : int8_t { None = 0, Disk, Memory, Embedded, Zip };

// A read-only directory. Every path is already normalized
// (dir_path_normalize): relative, `/` separated, `""` for the root.
struct Dir {
  virtual ~Dir() = default;
  // Fill `out` with the bytes of `path` and return true; return false when
  // there is no such file.
  virtual bool read(std::string_view path, std::string& out) const = 0;
  // Whether `path` is a file here. Separate from read() so an existence test
  // costs no bytes.
  virtual bool is_file(std::string_view path) const = 0;
  virtual bool is_dir(std::string_view path) const = 0;
  // The bare names directly under the directory `path`, sorted by bytes;
  // false when `path` is no directory.
  virtual bool list_dir(std::string_view path,
                        std::vector<std::string>& out) const = 0;
  // The byte size of the file `path`; false when there is no such file.
  virtual bool size(std::string_view path, std::uintmax_t& out) const = 0;
  bool exists(std::string_view path) const {
    return is_file(path) || is_dir(path);
  }
};

// What `culebra build` bakes of a directory, and so what a Dir.embedded
// holds wherever it runs: the regular files below it, never a symlink (to a
// file or a directory) nor a special file, and a directory only while a file
// lies under it. One rule for the bake (embed_kind) and the live reading.
enum class EmbedKind { None, File, Directory };
inline EmbedKind embed_kind(const std::filesystem::file_status& st) {
  switch (st.type()) {
    case std::filesystem::file_type::regular: return EmbedKind::File;
    case std::filesystem::file_type::directory: return EmbedKind::Directory;
    default: return EmbedKind::None;
  }
}

// Run from source: the directory on disk, read at request time so edits are
// live, holding exactly what the bake would (embed_kind).
struct LiveDir : Dir {
  std::string base;
  explicit LiveDir(std::string b) : base(std::move(b)) {}
  // What `path` is, walked one component at a time so that no symlink is
  // passed through; the OS path in `out`. On Windows a `\` is a separator and
  // a `:` a drive, so a normalized path holding either is not inside.
  EmbedKind reach(std::string_view path, std::filesystem::path& out) const {
#ifdef _WIN32
    if (path.find_first_of("\\:") != std::string_view::npos)
      return EmbedKind::None;
#endif
    std::error_code ec;
    out = std::filesystem::path(base);
    if (!std::filesystem::is_directory(out, ec)) return EmbedKind::None;
    auto kind = EmbedKind::Directory;
    for (const auto& seg : std::filesystem::path(std::string(path))) {
      if (kind != EmbedKind::Directory) return EmbedKind::None;
      out /= seg;
      kind = embed_kind(std::filesystem::symlink_status(out, ec));
    }
    return kind;
  }
  static bool holds_file(const std::filesystem::path& dir) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec))
      if (embed_kind(it->symlink_status(ec)) == EmbedKind::File) return true;
    return false;
  }
  bool is_file(std::string_view path) const override {
    std::filesystem::path p;
    return reach(path, p) == EmbedKind::File;
  }
  // The root always is, as in the baked table.
  bool is_dir(std::string_view path) const override {
    std::filesystem::path p;
    return path.empty() ||
           (reach(path, p) == EmbedKind::Directory && holds_file(p));
  }
  bool read(std::string_view path, std::string& out) const override {
    std::filesystem::path p;
    if (reach(path, p) != EmbedKind::File) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    if (!is_dir(path)) return false;
    out.clear();
    std::filesystem::path p;
    if (reach(path, p) != EmbedKind::Directory) return true;  // a missing root
    std::error_code ec;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec)) {
      auto kind = embed_kind(it->symlink_status(ec));
      if (kind == EmbedKind::File ||
          (kind == EmbedKind::Directory && holds_file(it->path())))
        out.push_back(it->path().filename().generic_string());
    }
    std::sort(out.begin(), out.end());
    return true;
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    std::filesystem::path p;
    if (reach(path, p) != EmbedKind::File) return false;
    std::error_code ec;
    out = std::filesystem::file_size(p, ec);
    return !ec;
  }
};

// `Dir.disk`: a directory on disk, read live, with the root as a boundary
// (Go's os.Root): a symlink is followed while it leads somewhere inside the
// root, and one that leads out, or nowhere, is not there. `real` is the
// root's own real path, taken when the Dir is made.
struct RootDir : Dir {
  std::filesystem::path real;
  explicit RootDir(std::filesystem::path r) : real(std::move(r)) {}
  // Where `path` really is, or false when that is not inside the root. On
  // Windows a `\` is a separator and a `:` a drive, so a path holding either
  // would leave the root.
  bool reach(std::string_view path, std::filesystem::path& out) const {
#ifdef _WIN32
    if (path.find_first_of("\\:") != std::string_view::npos) return false;
#endif
    std::error_code ec;
    out = path.empty() ? real
                       : std::filesystem::weakly_canonical(
                             real / std::filesystem::path(path), ec);
    if (ec) return false;
    const auto& top = real.native();
    const auto& at = out.native();
    if (at == top) return true;
    auto sep = std::filesystem::path::preferred_separator;
    return at.size() > top.size() && at.compare(0, top.size(), top) == 0 &&
           (top.back() == sep || at[top.size()] == sep);
  }
  // What `path` is (a symlink followed), its real path in `p`; not_found
  // when it is not inside the root.
  std::filesystem::file_type kind(std::string_view path,
                                  std::filesystem::path& p) const {
    std::error_code ec;
    return reach(path, p) ? std::filesystem::status(p, ec).type()
                          : std::filesystem::file_type::not_found;
  }
  bool is_file(std::string_view path) const override {
    std::filesystem::path p;
    return kind(path, p) == std::filesystem::file_type::regular;
  }
  bool is_dir(std::string_view path) const override {
    std::filesystem::path p;
    return kind(path, p) == std::filesystem::file_type::directory;
  }
  bool read(std::string_view path, std::string& out) const override {
    std::filesystem::path p;
    if (kind(path, p) != std::filesystem::file_type::regular) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    std::filesystem::path p;
    if (kind(path, p) != std::filesystem::file_type::directory) return false;
    out.clear();
    std::string prefix = path.empty() ? std::string() : std::string(path) + "/";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec)) {
      auto name = it->path().filename().u8string();
      std::string n(name.begin(), name.end());
      std::filesystem::path q;
      auto k = kind(prefix + n, q);
      if (k == std::filesystem::file_type::regular ||
          k == std::filesystem::file_type::directory)
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end());
    return true;
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    std::filesystem::path p;
    if (kind(path, p) != std::filesystem::file_type::regular) return false;
    std::error_code ec;
    out = std::filesystem::file_size(p, ec);
    return !ec;
  }
  // Every file, walked as list_dir and is_dir see it, symlinks included; a
  // directory that leads back to one it lies in is not walked again.
  std::vector<std::string> files() const {
    std::vector<std::string> out;
    std::vector<std::filesystem::path> above;
    walk("", real, above, out);
    std::sort(out.begin(), out.end());
    return out;
  }
  // `dir` (really at `at`) and below; `above` holds the real paths of the
  // directories it lies in.
  void walk(const std::string& dir, const std::filesystem::path& at,
            std::vector<std::filesystem::path>& above,
            std::vector<std::string>& out) const {
    if (std::find(above.begin(), above.end(), at) != above.end()) return;
    std::vector<std::string> names;
    if (!list_dir(dir, names)) return;
    above.push_back(at);
    for (auto& name : names) {
      std::string p = dir.empty() ? name : dir + "/" + name;
      std::filesystem::path q;
      auto k = kind(p, q);
      if (k == std::filesystem::file_type::directory)
        walk(p, q, above, out);
      else if (k == std::filesystem::file_type::regular)
        out.push_back(std::move(p));
    }
    above.pop_back();
  }
};

// Files held by value, keyed by normalized path — what a server keeps of a
// `Dir.memory`. A directory is there when a file lies under it, and the root
// always is.
struct MapDir : Dir {
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{""};
  explicit MapDir(std::map<std::string, std::string> f) : files(std::move(f)) {
    for (const auto& [key, _] : files)
      for (auto slash = key.find('/'); slash != std::string::npos;
           slash = key.find('/', slash + 1))
        dirs.insert(key.substr(0, slash));
  }
  bool read(std::string_view path, std::string& out) const override {
    auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool is_file(std::string_view path) const override {
    return files.count(std::string(path)) != 0;
  }
  bool is_dir(std::string_view path) const override {
    return dirs.count(std::string(path)) != 0;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    if (!is_dir(path)) return false;
    std::set<std::string, std::less<>> names;
    std::string_view name;
    for (const auto& [key, _] : files)
      if (dir_child(key, path, name)) names.emplace(name);
    out.assign(names.begin(), names.end());
    return true;
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
};

// One baked file: path is NUL-terminated, data/len need not be.
struct AssetEntry {
  const char* path;
  const unsigned char* data;
  std::size_t len;
};

// AOT: read from a baked, read-only asset table (sorted or not — linear scan is
// fine for the handful of files a web UI ships). It holds files only: a
// directory is there when a file lies under it, and the root always is.
struct EmbeddedDir : Dir {
  const AssetEntry* entries;
  std::size_t count;
  EmbeddedDir(const AssetEntry* e, std::size_t n) : entries(e), count(n) {}
  const AssetEntry* find(std::string_view path) const {
    for (std::size_t i = 0; i < count; i++)
      if (entries[i].path && path == entries[i].path) return &entries[i];
    return nullptr;
  }
  bool read(std::string_view path, std::string& out) const override {
    const AssetEntry* e = find(path);
    if (!e) return false;
    out.assign(reinterpret_cast<const char*>(e->data), e->len);
    return true;
  }
  bool is_file(std::string_view path) const override {
    return find(path) != nullptr;
  }
  bool is_dir(std::string_view path) const override {
    if (path.empty()) return true;
    std::string_view name;
    for (std::size_t i = 0; i < count; i++)
      if (entries[i].path && dir_child(entries[i].path, path, name)) return true;
    return false;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    if (!is_dir(path)) return false;
    std::set<std::string, std::less<>> names;
    std::string_view name;
    for (std::size_t i = 0; i < count; i++)
      if (entries[i].path && dir_child(entries[i].path, path, name))
        names.emplace(name);
    out.assign(names.begin(), names.end());
    return true;
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    const AssetEntry* e = find(path);
    if (!e) return false;
    out = e->len;
    return true;
  }
};

// Build-generated code registers each baked table under the literal directory
// name passed to `Dir.embedded(...)`; the AOT `Dir.embedded` resolves through
// here.
inline std::unordered_map<std::string,
                          std::pair<const AssetEntry*, std::size_t>>&
_asset_tables() {
  static std::unordered_map<std::string,
                            std::pair<const AssetEntry*, std::size_t>>
      m;
  return m;
}
inline void register_asset_table(const char* name, const AssetEntry* entries,
                                 std::size_t count) {
  _asset_tables()[name] = {entries, count};
}

// Directory of the entry script, set at startup (dev). `Dir.embedded(name)`
// resolves its disk fallback relative to this so it works regardless of the
// current working directory — the same base the AOT build walks at build time.
// Unused under AOT (the baked table wins), where it stays empty.
inline std::string& main_script_dir() {
  static std::string s;
  return s;
}

// The `Dir` behind `Dir.embedded(name)`: the baked asset table when this
// binary was built with that directory embedded, else the live on-disk
// directory — the choice that makes `Dir.embedded` resolve per backend with no
// code change.
inline std::unique_ptr<Dir> open_embed_dir(const std::string& name) {
  auto& tables = _asset_tables();
  if (auto it = tables.find(name); it != tables.end())
    return std::make_unique<EmbeddedDir>(it->second.first, it->second.second);
  std::string base = main_script_dir();
  if (!base.empty() && base.back() != '/') base += '/';
  base += name;
  return std::make_unique<LiveDir>(std::move(base));
}

// Absolute path of the entry script, set at startup before the environment is
// built (so `Sys.script` can be baked into the Sys namespace on both backends).
// Empty when there is no source file at runtime — the REPL, `stdin`, or an AOT
// binary — in which case `Sys.script` reads back as nil.
inline std::string& main_script_path() {
  static std::string s;
  return s;
}

// Set both at once — the directory is the path's parent by definition, and a
// caller that derives it separately is how the two come to disagree. An empty
// path clears both (no source file: the REPL, stdin, an AOT binary).
inline void set_main_script(const std::string& path) {
  main_script_path() = path;
  main_script_dir() =
      path.empty() ? std::string()
                   : std::filesystem::path(path).parent_path().string();
}

// The entry script for the length of one program. A process that runs several
// — `culebra test` runs each test file as its own — has an entry script per
// program, not per process, and `Dir.embedded(...)` resolves against it.
struct MainScriptScope {
  std::string saved;
  explicit MainScriptScope(const std::string& path)
      : saved(main_script_path()) {
    set_main_script(path);
  }
  ~MainScriptScope() { set_main_script(saved); }
  MainScriptScope(const MainScriptScope&) = delete;
  MainScriptScope& operator=(const MainScriptScope&) = delete;
};

// Content-Type from a file extension — the common web set; anything unknown is
// served as application/octet-stream (the browser sniffs or downloads).
inline std::string content_type_for(std::string_view path) {
  auto dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string ext(path.substr(dot + 1));
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
  if (ext == "json" || ext == "map") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "wasm") return "application/wasm";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "ttf") return "font/ttf";
  if (ext == "txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// Turn a request URL into a directory-relative lookup key:
//   - require the URL to be under `mount` (with a '/' boundary), strip it
//   - strip leading slashes
//   - map "" or a trailing "/" to "index.html"
//   - normalize as every Dir path is (dir_path_normalize), so a path that
//     climbs with `..` is not there while `foo..txt` is an ordinary name
// Returns false when the URL isn't under this mount or escapes it.
inline bool static_key(std::string_view url, std::string_view mount,
                       std::string& key) {
  if (mount.empty()) return false;
  if (mount != "/") {
    if (url.substr(0, mount.size()) != mount) return false;
    if (url.size() != mount.size() && url[mount.size()] != '/') return false;
    url.remove_prefix(mount.size());
  }
  while (!url.empty() && url.front() == '/') url.remove_prefix(1);
  std::string raw(url);
  if (raw.empty() || raw.back() == '/') raw += "index.html";
  return dir_path_normalize(raw, key);
}

struct StaticResult {
  int status;
  std::string content_type;
  std::string body;
};

// Serve `url` from `dir` under `mount`. Returns true and fills `out` when the
// file is found; false when it isn't, so the caller falls through to its
// registered routes (the same precedence as a disk mount point). The caller
// should only invoke this for GET/HEAD.
inline bool serve_static(const Dir& dir, std::string_view mount,
                         std::string_view url, StaticResult& out) {
  std::string key;
  if (!static_key(url, mount, key)) return false;
  std::string body;
  if (!dir.read(key, body)) return false;
  out.status = 200;
  out.content_type = content_type_for(key);
  out.body = std::move(body);
  return true;
}

}  // namespace culebra
