#include "init_cmd.h"

#include <zlib.h>  // crc32() for the hand-rolled .vsix (STORE-only) writer

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath (self_executable_path)
#include <climits>        // PATH_MAX
#elif defined(__linux__)
#include <climits>   // PATH_MAX
#include <unistd.h>  // readlink (self_executable_path)
#elif defined(_WIN32)
#include <os_compat.h>  // <windows.h> (guarded) — GetModuleFileNameW
#endif

#include "docs_embedded.h"
#include "editor_assets_embedded.h"

namespace culebra {
namespace fs = std::filesystem;
namespace {

#if defined(_WIN32)
constexpr char kPathSep = ';';
#else
constexpr char kPathSep = ':';
#endif

// --- self-exe path -----------------------------------------------------
//
// Duplicated from shared.h's current_executable_path rather than included:
// this TU stays free of culebra.h / interpreter internals, the same
// isolation docs_cmd.h documents for itself, so a docs-or-init edit never
// drags the interpreter into the link.
std::string self_executable_path() {
#if defined(__APPLE__)
  uint32_t sz = 0;
  _NSGetExecutablePath(nullptr, &sz);  // first call reports the needed size
  std::string buf(sz, '\0');
  if (_NSGetExecutablePath(buf.data(), &sz) != 0) return "";
  buf.resize(std::strlen(buf.c_str()));
  char real[PATH_MAX];
  if (::realpath(buf.c_str(), real)) return real;
  return buf;
#elif defined(__linux__)
  char buf[PATH_MAX];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  return std::string(buf, static_cast<size_t>(n));
#elif defined(_WIN32)
  std::wstring wbuf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, wbuf.data(),
                                 static_cast<DWORD>(wbuf.size()));
    if (n == 0) return "";
    if (n < wbuf.size()) {
      wbuf.resize(n);
      break;
    }
    wbuf.resize(wbuf.size() * 2);
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(),
                                static_cast<int>(wbuf.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(wbuf.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
#else
  return "";
#endif
}

std::string home_dir() {
  const char* h = std::getenv("HOME");
#if defined(_WIN32)
  if (!h) h = std::getenv("USERPROFILE");
#endif
  return h ? h : "";
}

// PATH search without shelling out to `command -v`/`where` — the candidate
// list (code/code-insiders/cursor/codium, zed) is small and fixed, so this
// stays a plain string scan instead of pulling in proc.h's subprocess
// machinery for something this cheap.
std::string find_on_path(std::string_view name) {
  const char* path_env = std::getenv("PATH");
  if (!path_env) return "";
  std::string_view path(path_env);
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t sep = path.find(kPathSep, pos);
    std::string_view dir = path.substr(
        pos, sep == std::string_view::npos ? std::string_view::npos - pos
                                           : sep - pos);
    if (!dir.empty()) {
      fs::path base = fs::path(dir) / name;
      std::error_code ec;
#if defined(_WIN32)
      for (const char* ext : {".exe", ".cmd", ".bat", ""}) {
        fs::path candidate = base;
        candidate += ext;
        if (fs::exists(candidate, ec)) return candidate.string();
      }
#else
      if (fs::exists(base, ec)) return base.string();
#endif
    }
    if (sep == std::string_view::npos) break;
    pos = sep + 1;
  }
  return "";
}

const editor_assets::Asset* find_asset(std::string_view editor,
                                       std::string_view rel_path) {
  for (size_t i = 0; i < editor_assets::kAssetCount; i++) {
    const editor_assets::Asset& a = editor_assets::kAssets[i];
    if (editor == a.editor && rel_path == a.rel_path) return &a;
  }
  return nullptr;
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// --- minimal ZIP writer (STORE only) ------------------------------------
//
// A .vsix is just an OPC/zip container. The payload here is a handful of
// text files a few KB each, so skipping DEFLATE costs nothing worth a second
// codec — and building the container in-process (instead of shelling out to
// `zip`, as misc/vscode/build-vsix.sh does) means `init` doesn't depend on a
// `zip` binary being on PATH, which Windows makes no promise of.

struct ZipEntry {
  std::string name;  // path inside the archive, e.g. "extension/package.json"
  std::string data;
};

std::string u16le(uint16_t v) {
  return std::string{static_cast<char>(v & 0xff),
                     static_cast<char>((v >> 8) & 0xff)};
}

std::string u32le(uint32_t v) {
  return std::string{
      static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
      static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff)};
}

std::string build_zip(const std::vector<ZipEntry>& entries) {
  constexpr uint16_t kDosTime = 0x0000;
  constexpr uint16_t kDosDate = 0x0021;  // 1980-01-01 — the ZIP epoch's start

  std::string out;
  std::vector<std::string> central;

  for (const ZipEntry& e : entries) {
    uint32_t crc = static_cast<uint32_t>(::crc32(
        0, reinterpret_cast<const Bytef*>(e.data.data()),
        static_cast<uInt>(e.data.size())));
    uint32_t size = static_cast<uint32_t>(e.data.size());
    uint32_t offset = static_cast<uint32_t>(out.size());
    uint16_t name_len = static_cast<uint16_t>(e.name.size());

    out += "PK\x03\x04";
    out += u16le(20);  // version needed to extract
    out += u16le(0);   // flags
    out += u16le(0);   // method: STORE
    out += u16le(kDosTime);
    out += u16le(kDosDate);
    out += u32le(crc);
    out += u32le(size);  // compressed size == uncompressed (STORE)
    out += u32le(size);
    out += u16le(name_len);
    out += u16le(0);  // extra field length
    out += e.name;
    out += e.data;

    std::string cd;
    cd += "PK\x01\x02";
    cd += u16le(20);  // version made by
    cd += u16le(20);  // version needed
    cd += u16le(0);
    cd += u16le(0);
    cd += u16le(kDosTime);
    cd += u16le(kDosDate);
    cd += u32le(crc);
    cd += u32le(size);
    cd += u32le(size);
    cd += u16le(name_len);
    cd += u16le(0);  // extra
    cd += u16le(0);  // comment
    cd += u16le(0);  // disk number
    cd += u16le(0);  // internal attrs
    cd += u32le(0);  // external attrs
    cd += u32le(offset);
    cd += e.name;
    central.push_back(std::move(cd));
  }

  uint32_t cd_offset = static_cast<uint32_t>(out.size());
  for (const std::string& cd : central) out += cd;
  uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;

  out += "PK\x05\x06";
  out += u16le(0);  // this disk
  out += u16le(0);  // disk with central directory start
  out += u16le(static_cast<uint16_t>(central.size()));
  out += u16le(static_cast<uint16_t>(central.size()));
  out += u32le(cd_size);
  out += u32le(cd_offset);
  out += u16le(0);  // comment length

  return out;
}

fs::path make_temp_vsix_path() {
  std::random_device rd;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string suffix;
  for (int i = 0; i < 8; i++) suffix += kHex[rd() % 16];
  return fs::temp_directory_path() / ("culebra-init-" + suffix + ".vsix");
}

#if defined(_WIN32)
std::string quote_for_shell(std::string_view s) {
  return "\"" + std::string(s) + "\"";
}
#else
std::string quote_for_shell(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}
#endif

std::string extract_json_string_field(std::string_view json,
                                      std::string_view key) {
  size_t p = json.find(key);
  if (p == std::string_view::npos) return "";
  size_t colon = json.find(':', p);
  if (colon == std::string_view::npos) return "";
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string_view::npos) return "";
  size_t q2 = json.find('"', q1 + 1);
  if (q2 == std::string_view::npos) return "";
  return std::string(json.substr(q1 + 1, q2 - q1 - 1));
}

constexpr std::string_view kContentTypesXml =
    R"(<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
</Types>
)";

std::string vsix_manifest(std::string_view version) {
  std::string v = version.empty() ? "0.0.1" : std::string(version);
  return std::string(
             R"(<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="culebra-debug" Version=")") +
         v +
         R"(" Publisher="local" />
    <DisplayName>Culebra</DisplayName>
    <Description xml:space="preserve">Syntax highlighting and debugging for Culebra (.cul) programs.</Description>
    <Categories>Programming Languages</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.70.0" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
)";
}

// code, code-insiders, cursor, codium — in that order, matching
// misc/vscode/install.sh's candidate list (first found wins).
constexpr const char* kVSCodeClis[] = {"code", "code-insiders", "cursor",
                                       "codium"};

bool setup_vscode() {
  std::string cli;
  for (const char* c : kVSCodeClis) {
    cli = find_on_path(c);
    if (!cli.empty()) break;
  }
  if (cli.empty()) return true;  // no VSCode-family editor found — not an error

  const editor_assets::Asset* package_asset = find_asset("vscode", "package.json");
  const editor_assets::Asset* ext_asset = find_asset("vscode", "extension.js");
  const editor_assets::Asset* lang_asset =
      find_asset("vscode", "language-configuration.json");
  const editor_assets::Asset* grammar_asset =
      find_asset("vscode", "syntaxes/culebra.tmLanguage.json");
  if (!package_asset || !ext_asset || !lang_asset || !grammar_asset) {
    std::println(stderr, "culebra init: VSCode assets missing from this binary");
    return false;
  }
  std::string package_json = package_asset->text;
  std::string ext_js = ext_asset->text;
  std::string lang_conf = lang_asset->text;
  std::string tm_grammar = grammar_asset->text;

  // The debug adapter (`program`) and the formatter (extension.js) default to
  // plain "culebra" resolved on PATH. Bake in this process's own absolute
  // path when known, so they still work when the editor launches without
  // inheriting the shell PATH — this is more reliable than
  // build-vsix.sh's `command -v culebra`, since init IS culebra running.
  std::string self = self_executable_path();
  if (!self.empty()) {
    replace_all(package_json, "\"program\": \"culebra\"",
               "\"program\": \"" + self + "\"");
    replace_all(ext_js, "const CULEBRA = 'culebra';",
               "const CULEBRA = '" + self + "';");
  }

  std::string version = extract_json_string_field(package_json, "\"version\"");

  std::vector<ZipEntry> entries = {
      {"extension.vsixmanifest", vsix_manifest(version)},
      {"[Content_Types].xml", std::string(kContentTypesXml)},
      {"extension/package.json", package_json},
      {"extension/language-configuration.json", lang_conf},
      {"extension/extension.js", ext_js},
      {"extension/syntaxes/culebra.tmLanguage.json", tm_grammar},
  };
  std::string zip_bytes = build_zip(entries);

  fs::path vsix_path = make_temp_vsix_path();
  {
    std::ofstream out(vsix_path, std::ios::binary);
    if (!out) {
      std::println(stderr, "culebra init: can't write '{}'",
                   vsix_path.string());
      return false;
    }
    out.write(zip_bytes.data(), static_cast<std::streamsize>(zip_bytes.size()));
  }

  // A stray copy from the old (unsupported) folder-drop installer would
  // shadow the packaged one by id.
  std::error_code ec;
  std::string home = home_dir();
  if (!home.empty()) {
    fs::remove_all(fs::path(home) / ".vscode/extensions/culebra-debug", ec);
  }

  std::string cmd = quote_for_shell(cli) + " --install-extension " +
                    quote_for_shell(vsix_path.string());
  int rc = std::system(cmd.c_str());
  fs::remove(vsix_path, ec);

  if (rc != 0) {
    std::println(stderr, "culebra init: {} --install-extension failed", cli);
    return false;
  }
  std::println("VSCode: installed via {}", cli);
  return true;
}

bool write_file(const fs::path& path, std::string_view text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

bool install_vim_into(const fs::path& root, std::string_view name) {
  const editor_assets::Asset* syn = find_asset("vim", "culebra.vim");
  const editor_assets::Asset* ftp = find_asset("vim", "culebra_ftplugin.vim");
  if (!syn || !ftp) {
    std::println(stderr, "culebra init: Vim assets missing from this binary");
    return false;
  }
  bool ok = true;
  ok &= write_file(root / "syntax" / "culebra.vim", syn->text);
  ok &= write_file(root / "ftplugin" / "culebra.vim", ftp->text);
  ok &= write_file(root / "ftdetect" / "culebra.vim",
                   "autocmd BufRead,BufNewFile *.cul set filetype=culebra\n");
  if (ok) {
    std::println("Vim ({}): installed into {}", name, root.string());
  } else {
    std::println(stderr, "culebra init: couldn't write into {}",
                 root.string());
  }
  return ok;
}

// Targets ~/.vim and ~/.config/nvim — whichever already exists, matching
// misc/vim/install.sh (directory presence, not a `vim`/`nvim` binary check).
bool setup_vim() {
  std::string home = home_dir();
  if (home.empty()) return true;
  std::error_code ec;
  bool ok = true;
  if (fs::is_directory(fs::path(home) / ".vim", ec)) {
    ok &= install_vim_into(fs::path(home) / ".vim", "vim");
  }
  if (fs::is_directory(fs::path(home) / ".config" / "nvim", ec)) {
    ok &= install_vim_into(fs::path(home) / ".config" / "nvim", "neovim");
  }
  return ok;
}

// Zed's grammar/debug-adapter extension is fetched by Zed itself from a git
// checkout (file:// + commit rev — see misc/zed/install.sh), which a
// binary-only download has none of. Full auto-install needs its own design;
// for now `init` only makes Zed users aware setup exists at all.
void note_zed_if_present() {
  bool found = !find_on_path("zed").empty();
  if (!found) {
    std::string home = home_dir();
    if (!home.empty()) {
      std::error_code ec;
#if defined(__APPLE__)
      found = fs::exists(fs::path(home) / "Library/Application Support/Zed",
                         ec);
#elif defined(__linux__)
      found = fs::exists(fs::path(home) / ".config/zed", ec);
#endif
    }
  }
  if (found) {
    std::println(
        "Zed: detected, but automatic setup isn't available yet — see"
        " `culebra docs tooling -g Zed` for the manual steps.");
  }
}

// --- AI coding agent instructions ---------------------------------------

constexpr std::string_view kMarkerBegin = "<!-- culebra:agent:begin -->\n";
constexpr std::string_view kMarkerEnd = "<!-- culebra:agent:end -->\n";

const docs::Topic* find_agent_topic() {
  for (size_t i = 0; i < docs::kTopicCount; i++) {
    if (std::string_view(docs::kTopics[i].name) == "agent" &&
        std::string_view(docs::kTopics[i].lang) == "en") {
      return &docs::kTopics[i];
    }
  }
  return nullptr;
}

std::string build_block(std::string_view agent_text) {
  std::string block(kMarkerBegin);
  block += agent_text;
  if (!agent_text.empty() && agent_text.back() != '\n') block += '\n';
  block += kMarkerEnd;
  return block;
}

// Insert or replace the marker block inside `content` in place. Returns
// false if nothing changed (content already carries this exact block), so a
// second `init` run can say "already up to date" instead of "updated".
bool upsert_block(std::string& content, std::string_view block) {
  size_t begin = content.find(kMarkerBegin);
  if (begin != std::string::npos) {
    size_t end = content.find(kMarkerEnd, begin);
    if (end != std::string::npos) {
      end += kMarkerEnd.size();
      if (std::string_view(content).substr(begin, end - begin) == block) {
        return false;
      }
      content.replace(begin, end - begin, block);
      return true;
    }
  }
  if (!content.empty() && content.back() != '\n') content += '\n';
  if (!content.empty()) content += '\n';
  content += block;
  return true;
}

bool write_agent_block_to(const fs::path& path, std::string_view block,
                          bool create_if_missing) {
  std::error_code ec;
  bool existed = fs::exists(path, ec);
  if (!existed && !create_if_missing) return true;

  std::string content;
  if (existed) {
    std::ifstream in(path, std::ios::binary);
    content.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
  }
  if (!upsert_block(content, block)) {
    std::println("{}: already up to date", path.string());
    return true;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::println(stderr, "culebra init: can't write '{}'", path.string());
    return false;
  }
  out << content;
  std::println("{}: {}", path.string(), existed ? "updated" : "created");
  return true;
}

bool setup_agent_instructions() {
  const docs::Topic* t = find_agent_topic();
  if (!t) return true;  // shouldn't happen — the topic ships with this binary
  std::string block = build_block(t->text);

  constexpr const char* kCandidates[] = {"CLAUDE.md", "AGENTS.md",
                                         ".github/copilot-instructions.md"};
  bool any_existing = false;
  bool ok = true;
  for (const char* c : kCandidates) {
    std::error_code ec;
    if (fs::exists(c, ec)) {
      any_existing = true;
      ok &= write_agent_block_to(c, block, /*create_if_missing=*/false);
    }
  }
  if (!any_existing) {
    ok &= write_agent_block_to("AGENTS.md", block, /*create_if_missing=*/true);
  }
  return ok;
}

void print_usage() {
  std::println("Usage: culebra init");
  std::println("");
  std::println(
      "Set up this directory and this machine's editors for culebra"
      " development:");
  std::println(
      "  - append AI coding agent instructions to whichever of CLAUDE.md,");
  std::println(
      "    AGENTS.md, .github/copilot-instructions.md already exist here");
  std::println("    (creating AGENTS.md if none do)");
  std::println(
      "  - install/update syntax highlighting and the `culebra dap` debug");
  std::println(
      "    adapter for whichever of VSCode, Vim, Neovim this machine has");
  std::println("");
  std::println(
      "Safe to re-run any time: every step overwrites with whatever this");
  std::println("binary carries, so re-running after an upgrade is the update"
               " path.");
}

}  // namespace

int run_init(int argc, const char** argv) {
  for (int i = 2; i < argc; i++) {
    std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    }
    std::println(stderr, "culebra init: unknown argument {}", a);
    print_usage();
    return 2;
  }

  bool ok = true;
  ok &= setup_agent_instructions();
  ok &= setup_vscode();
  ok &= setup_vim();
  note_zed_if_present();

  return ok ? 0 : 1;
}

}  // namespace culebra
