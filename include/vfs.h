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
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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
};

// Dev: read from a base directory on disk at request time, so edits are live.
struct DiskDir : Dir {
  std::string base;
  explicit DiskDir(std::string b) : base(std::move(b)) {}
  bool read(std::string_view path, std::string& out) const override {
    std::string full = base;
    if (!full.empty() && full.back() != '/') full += '/';
    full.append(path);
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
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
// fine for the handful of files a web UI ships).
struct EmbeddedDir : Dir {
  const AssetEntry* entries;
  std::size_t count;
  EmbeddedDir(const AssetEntry* e, std::size_t n) : entries(e), count(n) {}
  bool read(std::string_view path, std::string& out) const override {
    for (std::size_t i = 0; i < count; i++) {
      if (path == entries[i].path) {
        out.assign(reinterpret_cast<const char*>(entries[i].data),
                   entries[i].len);
        return true;
      }
    }
    return false;
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

// Absolute path of the entry script, set at startup before the environment is
// built (so `Sys.script` can be baked into the Sys namespace on both backends).
// Empty when there is no source file at runtime — the REPL, `stdin`, or an AOT
// binary — in which case `Sys.script` reads back as nil.
inline std::string& main_script_path() {
  static std::string s;
  return s;
}

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
