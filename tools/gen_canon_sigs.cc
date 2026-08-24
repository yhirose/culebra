// Generates include/canon_sigs.gen.h — the canonical native-signature tables
// (see canon_sigs.h) — from the tree-walker's own parameter tables: the
// environment() dictionary for stdlib natives (kNsMethods / kBuiltinFns) and
// the seven value-type built-in method tables. Run via `just gen-canon-sigs`
// after changing any stdlib signature; `just check-canon-sigs` is the sync
// gate (part of `just check-generated`).
//
// This generator is also the drift check that used to run at startup
// (_check_ns_drift_once / the culebra.h builtin-method drift check): a
// kNsMethods row whose canonical lookup fails, or an interp namespace no
// compiled-side table knows, is a hard error here — at generation time,
// where the fix is made, instead of at first slow-path resolve.
//
// Build flags matter: the emitted file must be the FULL superset, so the
// recipe compiles this TU with CULEBRA_HTTP_ENABLED + CULEBRA_SQLITE_ENABLED
// (a binary built without a feature simply never looks its rows up).

#include <culebra.h>
#include <stdlib_interp.h>
#include <stdlib_jit.h>

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string read_file(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::string out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// C string literal with escapes — names/types are identifiers, but defaults
// can be arbitrary text (",", ".env", "0.0.0.0").
std::string quoted(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\x%02x", c);
          out += buf;
          // A hex escape swallows following hex digits; the emitted strings
          // never mix control chars with text, so no splice is needed.
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  out += '"';
  return out;
}

struct Pool {
  std::string text;   // the CanonParam initializers
  int count = 0;
};

using SigRows = std::string;  // the CanonSig initializers

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "gen_canon_sigs: FAIL — %s\n", msg.c_str());
  std::exit(1);
}

// The canonical interp environment, built once — this generator (and the
// debug-only _check_canon_sigs_once) are the only remaining readers.
const culebra::Environment& canonical_env() {
  static const std::shared_ptr<culebra::Environment> env =
      culebra::environment();
  return *env;
}

// Resolve an NsMethod row to its canonical interp FunctionValue, or null.
// Namespace methods (Ns.method), nested sub-namespace methods
// (Ns.sub.method), and bare globals (ns == "" — range / iota / the IO
// aliases inspect/print/println, which live on the IO namespace).
const culebra::FunctionValue* canon_fn(const culebra::NsMethod& m) {
  const auto& env = canonical_env();
  const culebra::Value* fnv = nullptr;
  if (m.ns == nullptr || m.ns[0] == '\0') {
    auto it = env.dictionary.find(std::string(m.name));
    if (it != env.dictionary.end()) {
      fnv = &it->second.val;
    } else {
      auto io_it = env.dictionary.find("IO");
      if (io_it != env.dictionary.end() &&
          io_it->second.val.type == culebra::Value::Object) {
        const auto& io_obj = io_it->second.val.to_object();
        if (io_obj.has(m.name)) fnv = &io_obj.get(m.name);
      }
    }
  } else {
    auto it = env.dictionary.find(std::string(m.ns));
    if (it == env.dictionary.end() ||
        it->second.val.type != culebra::Value::Object) return nullptr;
    const auto& ns_obj = it->second.val.to_object();
    if (m.sub != nullptr && m.sub[0] != '\0') {
      if (!ns_obj.has(m.sub)) return nullptr;
      const auto& sub_v = ns_obj.get(m.sub);
      if (sub_v.type != culebra::Value::Object) return nullptr;
      const auto& sub_obj = sub_v.to_object();
      if (!sub_obj.has(m.name)) return nullptr;
      fnv = &sub_obj.get(m.name);
    } else {
      if (!ns_obj.has(m.name)) return nullptr;
      fnv = &ns_obj.get(m.name);
    }
  }
  if (fnv == nullptr || fnv->type != culebra::Value::Function) return nullptr;
  return &fnv->get<culebra::FunctionValue>();
}

// Append one parameter list to the pool; returns its starting offset.
int emit_params(const std::vector<culebra::FunctionValue::Parameter>& ps,
                const std::string& label, Pool& pool) {
  int off = pool.count;
  pool.text += "  // " + std::to_string(off) + ": " + label + "\n";
  for (const auto& p : ps) {
    bool has_default = p.default_expr != nullptr || p.default_value != nullptr;
    const char* kind = "CanonDefault::None";
    bool is_str = false;
    int64_t bits = 0;
    std::string_view str{};
    bool bits_hex = false;
    if (has_default) {
      if (!p.default_value) {
        die(label + ": param '" + std::string(p.name) +
            "' has a default_expr but no default_value — a native's default "
            "must be a literal Value for the compiled lanes to rebuild it");
      }
      const auto& v = *p.default_value;
      switch (v.type) {
        case culebra::Value::Nil: kind = "CanonDefault::Nil"; break;
        case culebra::Value::Bool:
          kind = "CanonDefault::Bool";
          bits = v.get<bool>() ? 1 : 0;
          break;
        case culebra::Value::Long:
          kind = "CanonDefault::Long";
          bits = v.get<int64_t>();
          break;
        case culebra::Value::Float:
          kind = "CanonDefault::Float";
          bits = std::bit_cast<int64_t>(v.get<double>());
          bits_hex = true;
          break;
        case culebra::Value::String:
          kind = "CanonDefault::Str";
          is_str = true;
          str = v.to_string_view();
          break;
        default:
          die(label + ": param '" + std::string(p.name) +
              "' has an unsupported default Value type");
      }
    }
    auto b = [](bool v) { return v ? "true" : "false"; };
    char bitsbuf[64];
    if (bits_hex) {
      std::snprintf(bitsbuf, sizeof(bitsbuf),
                    "static_cast<int64_t>(0x%016" PRIx64 "ULL)",
                    static_cast<uint64_t>(bits));
    } else {
      std::snprintf(bitsbuf, sizeof(bitsbuf), "%" PRId64, bits);
    }
    pool.text += "  {" + quoted(p.name) + ", " + b(has_default) + ", " +
                 b(p.kw_only) + ", " + b(p.kwargs_rest) + ", " +
                 b(p.args_rest) + ", " + b(p.mut) + ", " + quoted(p.type_name) +
                 ", " + kind + ", " + bitsbuf + ", " +
                 (is_str ? quoted(str) : std::string("{}")) + "},\n";
    pool.count++;
  }
  return off;
}

void emit_sig(std::string_view ns, std::string_view sub, std::string_view name,
              const std::vector<culebra::FunctionValue::Parameter>& ps,
              std::string_view return_type, Pool& pool, SigRows& rows) {
  std::string label;
  if (!ns.empty()) label += std::string(ns) + ".";
  if (!sub.empty()) label += std::string(sub) + ".";
  label += name;
  int off = ps.empty() ? -1 : emit_params(ps, label, pool);
  int kwargs_rest_idx = -1, first_kw_only_idx = -1, args_rest_idx = -1;
  for (int i = 0; i < static_cast<int>(ps.size()); i++) {
    if (ps[i].kwargs_rest && kwargs_rest_idx < 0) kwargs_rest_idx = i;
    if (ps[i].kw_only && first_kw_only_idx < 0) first_kw_only_idx = i;
    if (ps[i].args_rest && args_rest_idx < 0) args_rest_idx = i;
  }
  auto bnd = culebra::builtin_arity_bounds(ps);
  rows += "  {" + quoted(ns) + ", " + quoted(sub) + ", " + quoted(name) +
               ", " +
               (off < 0 ? std::string("nullptr")
                        : "kCanonParamPool + " + std::to_string(off)) +
               ", " + std::to_string(ps.size()) + ", " + quoted(return_type) +
               ", " + std::to_string(bnd.min) + ", " + std::to_string(bnd.max) +
               ", " + (bnd.variadic ? "true" : "false") + ", " +
               std::to_string(kwargs_rest_idx) + ", " +
               std::to_string(first_kw_only_idx) + ", " +
               std::to_string(args_rest_idx) + "},\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool check = false;
  const char* out = "include/canon_sigs.gen.h";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--check") == 0) {
      check = true;
    } else {
      out = argv[i];
    }
  }

  Pool pool;
  std::string arrays;

  // --- stdlib natives: one sig row per kNsMethods / kBuiltinFns row --------
  {
    SigRows rows;
    std::set<std::string> seen;
    auto add_row = [&](const culebra::NsMethod& m) {
      std::string_view ns = m.ns ? m.ns : "";
      std::string_view sub = m.sub ? m.sub : "";
      if (!seen.insert(culebra::canon_sig_key(ns, sub, m.name)).second) {
        die("duplicate NsMethod row " + std::string(ns) + "." +
            std::string(sub) + "." + m.name);
      }
      const auto* fn = canon_fn(m);
      if (fn == nullptr) {
        die("kNsMethods row " + std::string(ns) + "." + std::string(sub) +
            (sub.empty() ? "" : ".") + m.name +
            " does not resolve in the canonical environment — was the "
            "generator built with the full feature set "
            "(HTTP/SQLite enabled)?");
      }
      emit_sig(ns, sub, m.name, *fn->params, fn->return_type, pool, rows);
    };
    for (const auto& m : culebra::kNsMethods) add_row(m);
    for (const auto& m : culebra::kBuiltinFns) add_row(m);
    arrays += "inline constexpr CanonSig kCanonNsSigs[] = {\n" + rows +
              "};\n\n";
  }

  // --- the value-type built-in method tables -------------------------------
  auto table_array = [&](auto&& tbl, const char* array_name) {
    std::vector<std::string_view> names;
    names.reserve(tbl.size());
    for (const auto& [k, _] : tbl) names.push_back(k);
    std::sort(names.begin(), names.end());
    SigRows rows;
    for (auto name : names) {
      const culebra::Value& fnv = [&]() -> const culebra::Value& {
        const auto& mapped = tbl.find(name)->second;
        if constexpr (std::is_same_v<std::decay_t<decltype(mapped)>,
                                     culebra::IterBuiltin>) {
          return mapped.fn;
        } else {
          return mapped;
        }
      }();
      if (fnv.type != culebra::Value::Function) continue;
      const auto& fn = fnv.get<culebra::FunctionValue>();
      emit_sig("", "", name, *fn.params, fn.return_type, pool, rows);
    }
    arrays += "inline constexpr CanonSig " + std::string(array_name) +
              "[] = {\n" + rows + "};\n\n";
  };
  table_array(culebra::ObjectValue().builtins(), "kCanonObjectSigs");
  table_array(culebra::ArrayValue().builtins(), "kCanonArraySigs");
  table_array(culebra::string_builtins(), "kCanonStringSigs");
  table_array(culebra::set_builtins(), "kCanonSetSigs");
  table_array(culebra::tuple_builtins(), "kCanonTupleSigs");
  table_array(culebra::TensorValue(culebra::TensorPtr{}).builtins(),
              "kCanonTensorSigs");
  table_array(culebra::iterator_builtins(), "kCanonIteratorSigs");

  // --- namespace-coverage drift check (was _check_ns_drift_once) -----------
  // Every Object-valued binding in the canonical dictionary must be a
  // namespace some compiled-side table covers. Wrap registries are empty in
  // this TU (extensions register at their own static-init), which matches
  // the stock binary the generated table serves.
  {
    const auto& env = canonical_env();
    for (const auto& [key, sym] : env.dictionary) {
      if (sym.val.type != culebra::Value::Object) continue;
      if (!culebra::_is_known_ns(key)) {
        die("interp binds namespace '" + std::string(key) +
            "' but stdlib_jit.h::kNsMethods does not cover it — add adapters "
            "and table rows for it");
      }
    }
  }

  std::string text;
  text += "#pragma once\n";
  text +=
      "// GENERATED by tools/gen_canon_sigs.cc (`just gen-canon-sigs`). Do "
      "not edit.\n";
  text +=
      "// Canonical native/built-in signature data — see canon_sigs.h for "
      "the\n// structs and lookup helpers, and `just check-canon-sigs` for "
      "the sync gate.\n";
  text += "// clang-format off\n";
  text += "\nnamespace culebra {\n\n";
  text += "inline constexpr CanonParam kCanonParamPool[] = {\n" + pool.text +
          "};\n\n";
  text += arrays;
  text += "}  // namespace culebra\n";
  text += "// clang-format on\n";

  if (check) {
    if (read_file(out) == text) return 0;
    std::fprintf(stderr,
                 "gen_canon_sigs: %s is stale (a stdlib signature changed).\n"
                 "  Run `just gen-canon-sigs` and commit the result — the "
                 "compiled lanes\n  bind and diagnose against this table.\n",
                 out);
    return 1;
  }

  FILE* f = std::fopen(out, "w");
  if (!f) {
    std::fprintf(stderr, "gen_canon_sigs: cannot open %s\n", out);
    return 1;
  }
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  std::fprintf(stderr, "gen_canon_sigs: wrote %s (%d params)\n", out,
               pool.count);
  return 0;
}
