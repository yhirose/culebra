#pragma once

// The compiler's name resolution, checked against resolve.h's. Off unless
// CULEBRA_SCOPE_CHECK names a directory (the `scope agreement` gate row,
// tools/checks/scope_agreement.py); then each module the compiler checks
// writes one report file there:
//
//   SUMMARY <path> checked=<n> dynamic=<n>
//   MISMATCH <what> '<name>' <path>:<line>:<col> resolve=<symbol> compiler=<symbol>
//   MISSING '<name>' <path>:<line>:<col>   the compiler looked up a name node
//                                          resolve.h never did
//   UNSEEN '<name>' <path>:<line>:<col>    resolve.h looked up a name node no
//                                          compile step did
//
// `dynamic` counts the reads and writes whose binding only the run can name
// (a pre-declaration shadowing another, a conditional declaration): resolve.h
// gives each a symbol, the compiler a guarded fallback.

#include "base/shared.h"
#include "frontend/fn_analysis.h"
#include "frontend/resolve.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace culebra::scope_check {

inline const char* report_dir() {
  static const char* dir = [] {
    const char* d = std::getenv("CULEBRA_SCOPE_CHECK");
    return d && *d ? d : nullptr;
  }();
  return dir;
}

// A module the check covers: one the program spells, not a `<stdlib>` unit
// (checked as a file of its own).
inline bool checks(const peg::Ast& root) {
  return report_dir() && !root.path.starts_with('<');
}

// A name node with no record in the resolution: one resolve.h never looked
// up, or one outside the module being checked.
inline constexpr size_t kUnrecorded = static_cast<size_t>(-2);

inline std::string where(std::string_view path, int64_t line, int64_t col) {
  return std::format("{}:{}:{}", path, line, col);
}
inline std::string where(const peg::Ast& n) {
  return where(n.path, static_cast<int64_t>(n.line),
               static_cast<int64_t>(n.column));
}
inline std::string missing_line(std::string_view name, std::string_view at) {
  return std::format("MISSING '{}' {}", name, at);
}

inline std::string describe(const resolve::Resolution& res, size_t symbol) {
  if (symbol == resolve::kNone) return "none";
  if (symbol == kUnrecorded) return "unrecorded";
  const auto& s = res.symbols[symbol];
  if (!s.declared_at) return s.name;
  return std::format("{}@{}:{}", s.name, s.declared_at->line,
                     s.declared_at->column);
}

// One report file per checked module, created exclusively, so processes and
// threads writing into one directory never overwrite each other.
inline void write_report(std::string_view header,
                         const std::vector<std::string>& lines) {
  static std::atomic<uint64_t> serial{0};
  FILE* out = nullptr;
  for (int tries = 0; !out && tries < 64; tries++) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto file = std::filesystem::path(report_dir()) /
                std::format("{}-{}-{}.txt", stamp,
                            std::hash<std::thread::id>{}(std::this_thread::get_id()),
                            serial++);
    out = std::fopen(file.string().c_str(), "wx");
  }
  if (!out) return;
  if (!header.empty())
    std::fprintf(out, "%.*s\n", static_cast<int>(header.size()), header.data());
  for (const auto& l : lines) std::fprintf(out, "%s\n", l.c_str());
  std::fclose(out);
}

// The options every resolution the check makes is held to: the compiler's.
inline resolve::Options options(std::span<const std::string> session = {}) {
  return compiler_resolve_options(session);
}

class Report {
 public:
  // `session`: the names earlier inputs declared, when `root` is a session's.
  explicit Report(const peg::Ast& root, std::span<const std::string> session)
      : res_(resolve::resolve_module(root, {}, options(session))),
        path_(root.path) {
    std::vector<const peg::Ast*> stack{&root};
    while (!stack.empty()) {
      const peg::Ast* n = stack.back();
      stack.pop_back();
      own_.insert(n);
      for (const auto& c : n->nodes) stack.push_back(c.get());
    }
  }
  Report(const Report&) = delete;
  Report& operator=(const Report&) = delete;
  ~Report() {
    try {
      for (const auto& [node, use] : res_.uses)
        if (!seen_.contains(node) && covers(*node, node->token))
          lines_.push_back(std::format("UNSEEN '{}' {}", node->token, where(*node)));
      write_report(std::format("SUMMARY {} checked={} dynamic={}", path_,
                               checked_, dynamic_),
                   lines_);
    } catch (...) {
    }
  }

  const resolve::Resolution& resolution() const { return res_; }

  // Whether a lookup of `name` at `node` is one this report checks: a name
  // the module spells, not an implicit one (`self`, `fn`, `_`, `__ARGS__`).
  bool covers(const peg::Ast& node, std::string_view name) const {
    return own_.contains(&node) && name != "_" && !is_always_bound_name(name);
  }

  // What resolve.h says `node` names (kNone: nothing), and that the
  // compiler asked; kUnrecorded when resolve.h never looked it up.
  size_t symbol_of(const peg::Ast& node) {
    auto it = res_.uses.find(&node);
    if (it == res_.uses.end()) return kUnrecorded;
    seen_.insert(&node);
    return it->second.symbol;
  }

  void count_check() { checked_++; }
  void count_dynamic() { dynamic_++; }

  void mismatch(std::string_view what, const peg::Ast& at,
                std::string_view name, size_t expected, size_t actual) {
    lines_.push_back(std::format("MISMATCH {} '{}' {} resolve={} compiler={}",
                                 what, name, where(at), describe(res_, expected),
                                 describe(res_, actual)));
  }

  void missing(const peg::Ast& at, std::string_view name) {
    lines_.push_back(missing_line(name, where(at)));
  }

 private:
  resolve::Resolution res_;
  std::string path_;
  std::unordered_set<const peg::Ast*> own_;
  std::unordered_set<const peg::Ast*> seen_;
  std::vector<std::string> lines_;
  size_t checked_ = 0, dynamic_ = 0;
};


}  // namespace culebra::scope_check
