#pragma once

// A tiny read-only virtual directory used to serve static web assets.
//
// `DiskDir` reads live from a base directory on disk (dev: edit a file and the
// next request sees it); `EmbeddedDir` reads from a baked, read-only asset
// table linked into an AOT single binary. `serve_static()` drives either one
// the same way, so the interpreter, the JIT, and an AOT build all answer a
// request byte-for-byte identically — the only difference is which `Dir` the
// `Embed.dir(...)` handle wraps (chosen per backend, see stdlib).
//
// This header is value-neutral: it touches no culebra Value type, so the three
// backends share one implementation (the CSV/TOML/http core pattern).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace culebra {

// A read-only directory: resolve a forward-slashed, already-sanitized relative
// path (no leading '/', no "..") to its bytes.
struct Dir {
  virtual ~Dir() = default;
  // Fill `out` with the bytes of `path` and return true; return false when
  // there is no such file.
  virtual bool read(std::string_view path, std::string& out) const = 0;
  // Whether `path` is a file here. Separate from read() so an existence test
  // costs no bytes.
  virtual bool exists(std::string_view path) const = 0;
};

// Dev: read from a base directory on disk at request time, so edits are live.
struct DiskDir : Dir {
  std::string base;
  explicit DiskDir(std::string b) : base(std::move(b)) {}
  std::string full_path(std::string_view path) const {
    std::string full = base;
    if (!full.empty() && full.back() != '/') full += '/';
    full.append(path);
    return full;
  }
  // Only a regular file counts. Opening a directory succeeds on some platforms
  // and reads back as an empty file, where the baked table — which holds no
  // directories — reports not-found; the two have to answer alike.
  static bool is_file(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
  }
  bool read(std::string_view path, std::string& out) const override {
    std::string full = full_path(path);
    if (!is_file(full)) return false;
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
  }
  bool exists(std::string_view path) const override {
    return is_file(full_path(path));
  }
};

// One baked file: path is NUL-terminated, data/len need not be.
struct AssetEntry {
  const char* path;
  const unsigned char* data;
  std::size_t len;
};

// AOT: read from a baked, read-only asset table (sorted or not — linear scan is
// fine for the handful of files a web UI ships).
struct EmbeddedDir : Dir {
  const AssetEntry* entries;
  std::size_t count;
  EmbeddedDir(const AssetEntry* e, std::size_t n) : entries(e), count(n) {}
  const AssetEntry* find(std::string_view path) const {
    for (std::size_t i = 0; i < count; i++)
      if (path == entries[i].path) return &entries[i];
    return nullptr;
  }
  bool read(std::string_view path, std::string& out) const override {
    const AssetEntry* e = find(path);
    if (!e) return false;
    out.assign(reinterpret_cast<const char*>(e->data), e->len);
    return true;
  }
  bool exists(std::string_view path) const override {
    return find(path) != nullptr;
  }
};

// Build-generated code registers each baked table under the literal directory
// name passed to `Embed.dir(...)`; the AOT `Embed.dir` resolves through here.
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

// Directory of the entry script, set at startup (dev). `Embed.dir(name)`
// resolves its disk fallback relative to this so it works regardless of the
// current working directory — the same base the AOT build walks at build time.
// Unused under AOT (the baked table wins), where it stays empty.
inline std::string& main_script_dir() {
  static std::string s;
  return s;
}

// The `Dir` behind `Embed.dir(name)`: the baked asset table when this binary
// was built with that directory embedded, else the live on-disk directory —
// the choice that makes `Embed.dir` resolve per backend with no code change.
inline std::unique_ptr<Dir> open_embed_dir(const std::string& name) {
  auto& tables = _asset_tables();
  if (auto it = tables.find(name); it != tables.end())
    return std::make_unique<EmbeddedDir>(it->second.first, it->second.second);
  std::string base = main_script_dir();
  if (!base.empty() && base.back() != '/') base += '/';
  base += name;
  return std::make_unique<DiskDir>(std::move(base));
}

// A direct read's path is already the lookup key (no mount to strip), so it
// only has to stay inside the directory: no absolute path and no "..", the
// traversal rule `static_key` applies.
inline bool embed_path_ok(std::string_view path) {
  return !path.empty() && path.front() != '/' &&
         path.find("..") == std::string_view::npos;
}

// The reads behind `Embed.dir(name).read(path)` / `.exists(path)`. Shared by
// the three backends, so a lookup answers identically wherever it runs.
inline bool embed_dir_read(const std::string& name, std::string_view path,
                           std::string& out) {
  return embed_path_ok(path) && open_embed_dir(name)->read(path, out);
}

inline bool embed_dir_exists(const std::string& name, std::string_view path) {
  return embed_path_ok(path) && open_embed_dir(name)->exists(path);
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
// program, not per process, and `Embed.dir(...)` resolves against it.
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
