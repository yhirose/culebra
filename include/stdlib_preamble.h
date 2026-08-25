#pragma once

// The lazy-stdlib preamble machinery: the embedded culebra-source modules
// (stdlib_preambles.gen.h), the builder-wrapping that registers them per
// Runtime, the AST token scan that decides which modules a program names,
// and splice_stdlib_preamble — the compiled lanes' way of putting those
// builders ahead of every program. Engine-neutral (pure AST/string work;
// no Value / Environment / JitValue), moved verbatim from the old stdlib
// binding header (Phase 4 B7-b); the engines and vm::Session splice or
// diff the sources from here.

#include <module_loader.h>  // LoadedModule, kStdlibPreamblePath
#include <parser.h>         // parse_with_transforms, peg::Ast
#include <shared.h>         // LazyFnGroup, lazy_fn_groups, trim_ascii

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace culebra {

// Embedded culebra source for stdlib modules that are easier to express
// in culebra than in C++. `environment()` registers each of them lazily;
// the JIT/AOT path registers a builder for it instead (see
// splice_stdlib_preamble).
//
// `Time` — Instant / Duration classes wrapping `_Time` primitives.
// Operator overloads (`__add__` / `__sub__` / `__mul__` / `__div__`
// / `__neg__` / `__lt__` / `__le__` / `__eq__`) give natural
// timestamp arithmetic.
//
// The module bodies live in src/preambles/*.cul (the editable source of
// truth); misc/gen_preambles.sh embeds each as a <NAME>_MODULE_SOURCE
// constant in the generated header below (run `just gen-preambles`; CI
// checks they're in sync). The per-module notes here and below document
// those sources.
#include "stdlib_preambles.gen.h"

// `Term` — terminal-control / TUI primitives layered on the `_Term`
// native helpers (raw mode / size / key read / flush). Provides ANSI
// colour + escape builders (pure strings), a buffered `Screen` for
// flicker-free frames, normalized key names, and `Term.app` — a render
// loop wrapper that enters raw mode + the alternate screen and restores
// the terminal on scope exit (normal return, exception, or Ctrl+C) via
// `defer`. Written as culebra source so it is automatically symmetric
// across interp / JIT / AOT. A C++ raw string so culebra's `\x1b`
// escapes pass through verbatim.

// `Args` — declarative CLI argument parser. Spec is a culebra
// Object listing positionals + options + subcommands; `Args.parse`
// returns an Object with parsed fields, prints help on `--help`,
// and reports errors with `Sys.exit(2)`. `try_parse` raises
// `{kind: \"ArgParseError\", message}` for programmatic control.

// Matcher family — `assert_true` / `assert_false` / `assert_eq` /
// `assert_ne` / `assert_lt` / `assert_le` / `assert_gt` / `assert_ge` /
// `assert_throws` / `assert_close`. Authored in culebra so all 3
// backends (interp/JIT/AOT) share one implementation: the `==` / `<`
// operators here dispatch through `__eq__` / `__lt__` on class
// instances, matching the operator semantics each backend already
// implements. `assert` itself is not in culebra — production code uses
// `if (!cond) throw {kind: "AssertionError", ...}` (Go / Ruby style).

// `Regex` — the user-facing object API over the native `_Regex` primitives
// (the `_Time` / `Time` split). `Regex.compile(pat, flags?)` returns a Regex
// instance whose methods delegate to `_Regex`; flags ("i"/"m"/"s") are folded
// into the pattern as an inline `(?…)` group. Patterns are best written as
// single-quoted raw strings ('\d+'). A Match is a data object
// { value, start, end, groups: [Group|nil], named: {name: Group} }; no-match nil.

// `s.replace(pat, repl)` (UFCS over this free function) — replace every
// occurrence and return a new String, so transforms chain:
//   text.replace(re'\s+', " ").replace("，", "、")...
// `pat` String -> literal replace (split/join); `pat` a compiled Regex ->
// regex replace (so `repl` may be a `$1`/`$<name>` template or a
// `fn (Match) -> String` callback, via the Regex's own `replace_all`).
// All-occurrences semantics match Python's `str.replace`.

// `Log` — leveled, structured logging to stderr, built on `_Time` (timestamp),
// `IO.eprint` (sink), `JSON.stringify` (json format), and `Sys.env`. Levels
// debug<info<warn<error; the threshold ($LOG_LEVEL or set_level, default info)
// filters. `Log.{level}(msg, fields={})` emits `msg` plus the structured
// fields; `Log.with(fields)` returns a child logger that binds those fields
// into every record (request-scoped context). Format ($LOG_FORMAT or
// set_format, default "text", or "json" for JSON Lines); the text level is
// colored when stderr is a tty.

// JIT/AOT only: rewrite a lazy source module so the entry module registers a
// captureless builder thunk instead of materializing a top-level slot.
//
// interp binds these culebra-source modules lazily (initialize_lazy) and
// re-resolves them per environment, so a closure shipped to another isolate is
// rebuilt there from the same source and never carries the module Object's
// native members (which aren't Sendable). The JIT can't carry the built Object
// either, so it drops the public slot and routes every reference — main and
// child — through namespace_get + the builder registry (one instance per
// Runtime). The builder is the whole module wrapped in a thunk
// `fn(){ <module body>; _x_module() }`, which is captureless: it references
// only builtins (resolved per-Runtime via namespace_get) plus its own nested
// helpers/classes, so namespace_get can rebuild + invoke it on any Runtime —
// exactly how run_isolate_child_jit rebuilds a user closure from a shared
// fn_ptr. The trailing public binding `let X = _x_module()` becomes the thunk's
// result expression `_x_module()`.
inline std::string _wrap_lazy_ns_module(std::string_view src, const char* pub,
                                        const char* builder) {
  std::string s(src);
  std::string bind = std::string("let ") + pub + " = " + builder + "()";
  auto pos = s.find(bind);
  // Every namespace module ends with its public binding; a miss means the
  // source shape changed (the JIT would silently lose the module). Assert so a
  // source edit that drops/renames the binding is caught in a dev build.
  assert(pos != std::string::npos &&
         "lazy-ns module is missing its `let X = _x_module()` public binding");
  if (pos != std::string::npos)
    s.replace(pos, bind.size(), std::string(builder) + "()");
  return "_lazy_ns_register(\"" + std::string(pub) + "\", fn(){ " + s + " });";
}

// The culebra-source stdlib modules whose public binding is a namespace
// object: public name, source, and the builder its last statement calls.
// One list for both backends — the interp's lazy bindings and the JIT/AOT
// builder registrations read it — so a module added to one cannot go missing
// from the other.
struct LazyNsModule {
  const char* name;
  const char* source;
  const char* builder;
};

inline std::span<const LazyNsModule> lazy_ns_modules() {
  static constexpr LazyNsModule kModules[] = {
      {"Time", TIME_MODULE_SOURCE, "_time_module"},
      {"Term", TERM_MODULE_SOURCE, "_term_module"},
      {"Canvas", CANVAS_MODULE_SOURCE, "_canvas_module"},
      {"Args", ARGS_MODULE_SOURCE, "_args_module"},
      {"Regex", REGEX_MODULE_SOURCE, "_regex_module"},
      {"Log", LOG_MODULE_SOURCE, "_log_module"},
      {"Path", PATH_MODULE_SOURCE, "_path_module"},
      {"Vector2", VECTOR2_MODULE_SOURCE, "_vector2_module"},
      {"Vector3", VECTOR3_MODULE_SOURCE, "_vector3_module"},
      // Algebraic-effects runtime. The transform has already lowered every
      // effect construct into `__Eff.*` calls by the time we see the AST, so
      // that one token is the exact marker (see effects_transform.h).
      {"__Eff", EFFECTS_MODULE_SOURCE, "_eff_module"},
#ifdef CULEBRA_ENABLE_WEBVIEW
      // `Desktop.run` facade — only when the Webview namespace it drives is
      // built in.
      {"Desktop", DESKTOP_MODULE_SOURCE, "_desktop_module"},
#endif
  };
  return kModules;
}

// The source backing a bare-function group (see LazyFnGroup in shared.h).
inline std::string_view lazy_fn_group_source(std::string_view group) {
  if (group == "__Matchers") return MATCHERS_MODULE_SOURCE;
  if (group == "__StringFns") return STRING_FNS_MODULE_SOURCE;
  return {};
}

// JIT/AOT only: give a bare-function module the same lazy builder a namespace
// module gets, by appending an Object literal that exposes its functions as
// members. A bare `assert_eq` then resolves to that Object's member — one
// closure per Runtime, reached identically from every module — where the
// interp resolves the same source's `let` into the one global environment.
inline std::string _wrap_lazy_fn_group(const LazyFnGroup& g) {
  // Trimmed because the members literal is appended as a further statement:
  // the `;` separator must not land after the source's trailing newline.
  auto src = trim_ascii(lazy_fn_group_source(g.name));
  // A group declared in shared.h with no source here would register a builder
  // that binds nothing, and every one of its names would raise NameError under
  // the JIT while the interp resolved them fine.
  assert(!src.empty() && "bare-function group has no source module");
  std::string members;
  for (auto m : g.members) {
    if (!members.empty()) members += ", ";
    members += std::string(m) + ": " + std::string(m);
  }
  return "_lazy_ns_register(\"" + std::string(g.name) + "\", fn(){ " +
         std::string(src) + "; {" + members + "} });";
}

// Every token the parsed program carries. Comments and whitespace are gone
// by this point, and a token is a whole lexeme — which is what makes the
// module selection below name-exact instead of substring-based.
inline void collect_ast_tokens(const peg::Ast& ast,
                               std::unordered_set<std::string_view>& out) {
  if (ast.is_token) {
    if (!ast.token.empty()) out.insert(ast.token);
    return;
  }
  for (const auto& n : ast.nodes) collect_ast_tokens(*n, out);
}

// The JIT/AOT stdlib preamble: `_lazy_ns_register("Ns", fn(){...})`
// statements, one per stdlib module the program names. They are pure runtime
// side effects with no scope bindings, so they run once in their own module
// ahead of every other one, and every reference — in any module, from any
// closure — resolves through the builder registry to a single instance per
// Runtime. That is what makes the JIT match the interp, which binds each
// stdlib module once in the global environment every module can see.
inline std::string stdlib_preamble_for(
    const std::unordered_set<std::string_view>& names) {
  auto has = [&](std::string_view m) { return names.contains(m); };
  // Each module is pulled in when the program names it. Matching the parsed
  // token set rather than the raw source keeps a mention in a comment — or a
  // longer identifier that merely contains the name — from inlining a whole
  // module: "Terminal" in a comment used to pull in Term, and `side_effect`
  // the effects runtime, each about a second of JIT compile for nothing.
  // A `re'...'` / `re"..."` / `` re`...` `` regex literal desugars to
  // `Regex.compile(...)` in the parser, so it shows up as a plain `Regex`
  // token here and needs no extra marker.
  std::string preamble;
  for (const auto& m : lazy_ns_modules()) {
    if (has(m.name))
      preamble.append(_wrap_lazy_ns_module(m.source, m.name, m.builder));
  }
  // Bare-function modules: naming any one member pulls in its whole group,
  // since they share a source (one `assert_*` brings the family, as on the
  // interp side, where initialize_lazy_group binds all ten at once).
  for (const auto& g : lazy_fn_groups()) {
    if (std::any_of(g.members.begin(), g.members.end(), has))
      preamble.append(_wrap_lazy_fn_group(g));
  }
  return preamble;
}

// Which names in `names` made stdlib_preamble_for emit something — module
// names, and every member of a bare-function group one of whose members was
// named (naming any one pulls in the whole group). The REPL splices one line
// at a time and has to remember what it has already registered: a builder
// registered twice would mint a second instance of the namespace, and values
// an earlier line built would stop matching it.
inline std::unordered_set<std::string_view> stdlib_preamble_triggers(
    const std::unordered_set<std::string_view>& names) {
  std::unordered_set<std::string_view> out;
  for (const auto& m : lazy_ns_modules())
    if (names.contains(m.name)) out.insert(m.name);
  for (const auto& g : lazy_fn_groups())
    if (std::any_of(g.members.begin(), g.members.end(),
                    [&](std::string_view m) { return names.contains(m); }))
      out.insert(g.members.begin(), g.members.end());
  return out;
}

// JIT/AOT only: prepend the synthesized `<stdlib>` preamble module.
//
// The preamble registers the builders behind the helpers user code calls
// (assert_*, Time, …). The registrations are pure side effects with no scope
// bindings, so they run as their own module *ahead of every dependency* — a
// dependency's top-level `Canvas.rgba(...)` must find the builder already
// registered, and appending them to the entry module (which the loader
// schedules last) would run them too late for that. Every reference then
// resolves through the registry, so no module needs a copy of its own — the
// same reach the interpreter gets from its global environment.
inline void splice_stdlib_preamble(std::vector<LoadedModule>& modules) {
  if (modules.empty()) return;
  // Scan every module, not just the entry: an imported module's `Canvas`
  // (or `Time`, ...) must pull that namespace's preamble in too, even when
  // the entry module never names it.
  std::unordered_set<std::string_view> names;
  for (const auto& m : modules) {
    if (m.ast) collect_ast_tokens(*m.ast, names);
  }
  std::string preamble = stdlib_preamble_for(names);
  if (preamble.empty()) return;

  auto buf = std::make_shared<std::string>(std::move(preamble));
  std::vector<std::string> msgs;
  auto ast = parse_with_transforms(kStdlibPreamblePath, *buf, msgs);
  if (!ast) return;  // stdlib is trusted; a parse failure is a build bug
  LoadedModule pre;
  pre.abs_path = kStdlibPreamblePath;
  pre.source = std::move(buf);
  pre.ast = std::move(ast);
  modules.insert(modules.begin(), std::move(pre));
}

}  // namespace culebra
