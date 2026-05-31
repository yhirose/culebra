#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "generator_transform.h"
#include "lint.h"
#include "parser.h"
#include "shared.h"

namespace culebra {

// One module after parse: absolute on-disk path, parsed AST, and the
// absolute paths of every module it `import`s. `deps` is in source
// order — the loader's topological sort uses it to schedule evaluation.
// `source` retains the read buffer through a shared_ptr because the
// AST's tokens reference it as string_views; using a heap allocation
// pins the bytes against std::string SSO / std::vector reallocation
// that would otherwise relocate them and dangle the views.
struct LoadedModule {
  std::filesystem::path abs_path;
  std::shared_ptr<std::string> source;
  std::shared_ptr<peg::Ast> ast;
  std::vector<std::filesystem::path> deps;
};

// Resolve a relative module path against an importing module's
// directory into a stable absolute key. Used by the loader, interp,
// and JIT — they must all agree on the same key so the module table
// lookup matches the register call.
inline std::filesystem::path resolve_module_path(
    const std::string& rel, const std::filesystem::path& from_dir) {
  std::filesystem::path abs(rel);
  if (!abs.is_absolute()) abs = from_dir / abs;
  std::error_code ec;
  auto canon = std::filesystem::weakly_canonical(abs, ec);
  if (ec) canon = std::filesystem::absolute(abs).lexically_normal();
  return canon;
}

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

  // Enforce two rules that the grammar can't express directly:
  // (1) IMPORT_STMT / EXPORT_STMT only at PROGRAM/STATEMENTS top level
  //     — nested forms (inside fn bodies, if branches, etc.) are
  //     parse-rejected because a static dependency graph requires
  //     import positions to be deterministic.
  // (2) An EXPORT_STMT may not list the same name twice.
  // Both fire as SyntaxError at module load time.
  static void validate_module(const peg::Ast& ast);

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
  // Static lint pass: shared by interp / JIT / AOT (all backends share
  // this loader), so a sound diagnostic aborts before any of them eval.
  for (const auto& m : loaded_) lint::check_module(*m.ast);
  return std::move(loaded_);
}

inline size_t ModuleLoader::load_recursive(
    const std::filesystem::path& abs_path,
    std::optional<std::string_view> source,
    std::vector<std::string>& parse_msgs) {
  auto key = abs_path.string();
  // In-progress modules are on `stack_` but not yet in `index_`.
  for (const auto& on_stack : stack_) {
    if (on_stack == abs_path) throw_cycle_error(stack_, abs_path);
  }
  if (auto it = index_.find(key); it != index_.end()) return it->second;

  stack_.push_back(abs_path);

  // Heap-allocate the source so its data() pointer survives moves of
  // the enclosing LoadedModule — the AST tokens are string_views into
  // these bytes.
  auto src_buf = std::make_shared<std::string>();
  if (source) {
    src_buf->assign(source->data(), source->size());
  } else {
    std::ifstream ifs(abs_path, std::ios::binary);
    if (!ifs) throw_io_error(abs_path);
    ifs.seekg(0, std::ios::end);
    auto pos = ifs.tellg();
    if (pos < 0) throw_io_error(abs_path);  // dir / FIFO / unseekable
    src_buf->resize(static_cast<size_t>(pos));
    ifs.seekg(0, std::ios::beg);
    if (!src_buf->empty()) {
      ifs.read(src_buf->data(),
                static_cast<std::streamsize>(src_buf->size()));
    }
  }

  auto ast = culebra::parse_with_transforms(abs_path.string(),
                                            src_buf->data(),
                                            src_buf->size(), parse_msgs);
  if (!ast) {
    // PEG diagnostics live in parse_msgs (path:line:col: ...). Fold them
    // into the error so the CLI catch handler prints the actual hint.
    std::string detail;
    for (const auto& m : parse_msgs) detail += "\n  " + m;
    throw CulebraError("SyntaxError",
                       std::format("failed to parse module '{}'{}",
                                   abs_path.string(), detail));
  }
  validate_module(*ast);
  auto deps = extract_imports(*ast, abs_path.parent_path());

  // Recurse before recording self so dependencies sit at lower indices
  // — the result vector ends up topologically sorted.
  for (const auto& dep : deps) {
    load_recursive(dep, std::nullopt, parse_msgs);
  }

  size_t idx = loaded_.size();
  loaded_.push_back(LoadedModule{abs_path, src_buf, ast, deps});
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
    auto canon = resolve_module_path(
        std::string(node->nodes[1]->token), from_dir);
    if (seen.insert(canon.string()).second) out.push_back(canon);
  }
  return out;
}

inline void ModuleLoader::validate_module(const peg::Ast& ast) {
  using namespace peg::udl;
  const peg::Ast* stmts =
      ast.tag == "STATEMENTS"_ || ast.original_tag == "STATEMENTS"_
          ? &ast
          : (ast.nodes.empty() ? nullptr : ast.nodes[0].get());

  // Recursive walk that flags an IMPORT_STMT / EXPORT_STMT found
  // anywhere off the toplevel STATEMENTS chain.
  std::function<void(const peg::Ast&, bool)> walk =
      [&](const peg::Ast& node, bool toplevel) {
        if (!toplevel &&
            (node.tag == "IMPORT_STMT"_ || node.tag == "EXPORT_STMT"_)) {
          const char* kind = node.tag == "IMPORT_STMT"_ ? "import" : "export";
          throw CulebraError(
              "SyntaxError",
              std::format("`{}` statement must be at the top level",
                          kind),
              static_cast<long>(node.line),
              static_cast<long>(node.column));
        }
        for (const auto& c : node.nodes) walk(*c, false);
      };
  // Iterate either the STATEMENTS children or — when AstOptimizer
  // folded the wrapper for a single-statement program — `ast` itself.
  auto for_each_toplevel = [&](auto&& fn) {
    if (stmts) {
      for (const auto& child : stmts->nodes) fn(*child);
    } else {
      fn(ast);
    }
  };
  for_each_toplevel([&](const peg::Ast& s) { walk(s, true); });

  std::unordered_set<std::string_view> seen;
  for_each_toplevel([&](const peg::Ast& s) {
    if (s.tag != "EXPORT_STMT"_) return;
    for (const auto& id : s.nodes) {
      auto name = id->token;
      if (!seen.insert(name).second) {
        throw CulebraError(
            "SyntaxError",
            std::format("duplicate export name '{}'", name),
            static_cast<long>(id->line),
            static_cast<long>(id->column));
      }
    }
  });
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
