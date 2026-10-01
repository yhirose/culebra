#pragma once

// A tiny read-only virtual directory used to serve static web assets.
//
// `DiskDir` reads live from a base directory on disk (dev: edit a file and the
// next request sees it); `EmbeddedDir` reads from a baked, read-only asset
// table linked into an AOT single binary. `serve_static()` drives either one
// the same way, so the interpreter, the JIT, and an AOT build all answer a
// request byte-for-byte identically — the only difference is which `Dir` a
// `Dir.embedded(...)` reads (chosen per backend, see stdlib).
//
// This header is value-neutral: it touches no culebra Value type, so the three
// backends share one implementation (the CSV/TOML/http core pattern).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

// Dev: read from a base directory on disk at request time, so edits are live.
struct DiskDir : Dir {
  std::string base;
  explicit DiskDir(std::string b) : base(std::move(b)) {}
  // The OS path under the base, or false for a path the OS would read as
  // leaving it: on Windows a `\` is a separator and a `:` a drive, so a
  // normalized path holding either is not inside.
  bool full_path(std::string_view path, std::filesystem::path& out) const {
#ifdef _WIN32
    if (path.find_first_of("\\:") != std::string_view::npos) return false;
#endif
    out = std::filesystem::path(base);
    if (!path.empty()) out /= std::filesystem::path(std::string(path));
    return true;
  }
  // Only a regular file counts. Opening a directory succeeds on some platforms
  // and reads back as an empty file, where the baked table — which holds no
  // directories — reports not-found; the two have to answer alike.
  bool is_file(std::string_view path) const override {
    std::filesystem::path p;
    std::error_code ec;
    return full_path(path, p) && std::filesystem::is_regular_file(p, ec);
  }
  bool is_dir(std::string_view path) const override {
    std::filesystem::path p;
    std::error_code ec;
    return full_path(path, p) && std::filesystem::is_directory(p, ec);
  }
  bool read(std::string_view path, std::string& out) const override {
    std::filesystem::path p;
    if (!is_file(path) || !full_path(path, p)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    std::filesystem::path p;
    if (!is_dir(path) || !full_path(path, p)) return false;
    std::error_code ec;
    out.clear();
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec))
      out.push_back(it->path().filename().generic_string());
    if (ec) return false;
    std::sort(out.begin(), out.end());
    return true;
  }
  bool size(std::string_view path, std::uintmax_t& out) const override {
    std::filesystem::path p;
    if (!is_file(path) || !full_path(path, p)) return false;
    std::error_code ec;
    out = std::filesystem::file_size(p, ec);
    return !ec;
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
  // The part of an entry's path below the directory `path`, if it is there.
  static bool under(std::string_view entry, std::string_view path,
                    std::string_view& rest) {
    if (path.empty()) {
      rest = entry;
      return true;
    }
    if (entry.size() <= path.size() || !entry.starts_with(path) ||
        entry[path.size()] != '/')
      return false;
    rest = entry.substr(path.size() + 1);
    return true;
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
    std::string_view rest;
    for (std::size_t i = 0; i < count; i++)
      if (entries[i].path && under(entries[i].path, path, rest)) return true;
    return false;
  }
  bool list_dir(std::string_view path,
                std::vector<std::string>& out) const override {
    if (!is_dir(path)) return false;
    out.clear();
    std::string_view rest;
    for (std::size_t i = 0; i < count; i++) {
      if (!entries[i].path || !under(entries[i].path, path, rest)) continue;
      std::string name(rest.substr(0, rest.find('/')));
      if (std::find(out.begin(), out.end(), name) == out.end())
        out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
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
  return std::make_unique<DiskDir>(std::move(base));
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
//   - reject any path containing ".." (traversal)
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
  key.assign(url);
  if (key.empty() || key.back() == '/') key += "index.html";
  if (key.find("..") != std::string::npos) return false;
  return true;
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
