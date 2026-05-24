#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parser.h"
#include "shared.h"

namespace culebra {

// One module after parse: absolute on-disk path, parsed AST, and the
// absolute paths of every module it `import`s. `deps` is in source
// order — the loader's topological sort uses it to schedule evaluation.
// `source` retains the read buffer because the AST's tokens reference
// it as string_views — dropping the buffer would dangle.
struct LoadedModule {
  std::filesystem::path abs_path;
  std::string source;
  std::shared_ptr<peg::Ast> ast;
  std::vector<std::filesystem::path> deps;
};

// Walks the dependency graph reachable from an entry module, returning
// every loaded module in topological order (every module appears after
// all of its dependencies). interp / JIT / AOT all share this loader
// so they observe identical evaluation order, identical caching, and
// identical cycle / I/O / parse errors. The loader itself does not
// evaluate user code — each backend processes the returned ASTs.
class ModuleLoader {
 public:
  // `entry_source` is the already-preprocessed source for the entry
  // file (e.g. with the stdlib preamble prepended). Subsequent modules
  // are read from disk verbatim and do not receive the preamble.
  std::vector<LoadedModule> load_program(
      const std::filesystem::path& entry_path,
      std::string_view entry_source,
      std::vector<std::string>& parse_msgs);

 private:
  std::unordered_map<std::string, size_t> index_;  // abs path → loaded_ idx
  std::vector<LoadedModule> loaded_;               // in load (post-order)
  std::vector<std::filesystem::path> stack_;       // for cycle detection

  size_t load_recursive(const std::filesystem::path& abs_path,
                        std::optional<std::string_view> source,
                        std::vector<std::string>& parse_msgs);

  static std::vector<std::filesystem::path> extract_imports(
      const peg::Ast& ast, const std::filesystem::path& from_dir);

  [[noreturn]] static void throw_io_error(const std::filesystem::path& p);
  [[noreturn]] static void throw_cycle_error(
      const std::vector<std::filesystem::path>& stack,
      const std::filesystem::path& reentered);
};

inline std::vector<LoadedModule> ModuleLoader::load_program(
    const std::filesystem::path& entry_path,
    std::string_view entry_source,
    std::vector<std::string>& parse_msgs) {
  std::error_code ec;
  auto entry_abs = std::filesystem::weakly_canonical(entry_path, ec);
  if (ec) entry_abs = std::filesystem::absolute(entry_path);
  load_recursive(entry_abs, entry_source, parse_msgs);
  return std::move(loaded_);
}

inline size_t ModuleLoader::load_recursive(
    const std::filesystem::path& abs_path,
    std::optional<std::string_view> source,
    std::vector<std::string>& parse_msgs) {
  auto key = abs_path.string();
  if (auto it = index_.find(key); it != index_.end()) {
    // Already loaded (or currently loading). A re-enter on the active
    // stack is a cycle.
    for (const auto& on_stack : stack_) {
      if (on_stack == abs_path) throw_cycle_error(stack_, abs_path);
    }
    return it->second;
  }

  stack_.push_back(abs_path);

  // Read the source unless the caller already provided it (entry file).
  // The buffer is retained inside LoadedModule because the AST stores
  // tokens as string_views into it.
  std::string buf;
  if (source) {
    buf.assign(source->data(), source->size());
  } else {
    std::ifstream ifs(abs_path, std::ios::binary);
    if (!ifs) throw_io_error(abs_path);
    ifs.seekg(0, std::ios::end);
    buf.resize(static_cast<size_t>(ifs.tellg()));
    ifs.seekg(0, std::ios::beg);
    if (!buf.empty()) ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
  }

  auto ast = culebra::parse(abs_path.string(), buf.data(), buf.size(),
                             parse_msgs);
  if (!ast) {
    throw CulebraError("SyntaxError",
                       std::format("failed to parse module '{}'",
                                   abs_path.string()));
  }

  auto deps = extract_imports(*ast, abs_path.parent_path());

  // Recurse before recording self so dependencies sit at lower indices
  // — the result vector ends up topologically sorted.
  for (const auto& dep : deps) {
    load_recursive(dep, std::nullopt, parse_msgs);
  }

  size_t idx = loaded_.size();
  loaded_.push_back(LoadedModule{abs_path, std::move(buf), ast, deps});
  index_[key] = idx;
  stack_.pop_back();
  return idx;
}

inline std::vector<std::filesystem::path> ModuleLoader::extract_imports(
    const peg::Ast& ast, const std::filesystem::path& from_dir) {
  using namespace peg::udl;
  std::vector<std::filesystem::path> out;
  std::unordered_set<std::string> seen;
  // AstOptimizer folds PROGRAM into its sole STATEMENTS child, so the
  // top-level node is already STATEMENTS itself; degenerate single-
  // statement programs come through wrapped, hence the fallback.
  const peg::Ast* stmts =
      ast.tag == "STATEMENTS"_ || ast.original_tag == "STATEMENTS"_
          ? &ast
          : (ast.nodes.empty() ? nullptr : ast.nodes[0].get());
  if (!stmts) return out;
  for (const auto& child : stmts->nodes) {
    const peg::Ast* node = child.get();
    if (node->tag != "IMPORT_STMT"_) continue;
    std::filesystem::path rel(std::string(node->nodes[1]->token));
    std::filesystem::path abs = rel.is_absolute() ? rel : (from_dir / rel);
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(abs, ec);
    if (ec) canon = std::filesystem::absolute(abs);
    if (seen.insert(canon.string()).second) out.push_back(canon);
  }
  return out;
}

[[noreturn]] inline void ModuleLoader::throw_io_error(
    const std::filesystem::path& p) {
  throw CulebraError(
      "IOError",
      std::format("can't open module '{}'", p.string()));
}

[[noreturn]] inline void ModuleLoader::throw_cycle_error(
    const std::vector<std::filesystem::path>& stack,
    const std::filesystem::path& reentered) {
  std::string chain;
  for (const auto& p : stack) {
    chain += p.string();
    chain += " -> ";
  }
  chain += reentered.string();
  throw CulebraError(
      "ImportError",
      std::format("circular import: {}", chain));
}

}  // namespace culebra
