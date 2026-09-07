#pragma once

// The lazy-stdlib preamble machinery: the embedded culebra-source modules
// (stdlib_preambles.gen.h), the builder-wrapping that registers them per
// Runtime, the AST token scan that decides which modules a program names,
// and splice_stdlib_preamble — the compiled lanes' way of putting those
// builders ahead of every program. Engine-neutral (pure AST/string work;
// no Value / Environment / JitValue), moved verbatim from the old stdlib
// binding header (Phase 4 B7-b); the engines and vm::Session splice or
// diff the sources from here.

#include <frontend/module_loader.h>  // LoadedModule, kStdlibPreamblePath
#include <frontend/parser.h>         // parse_with_transforms, peg::Ast
#include <base/shared.h>         // LazyFnGroup, lazy_fn_groups, trim_ascii

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
#include "stdlib/preambles.gen.h"

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
      {"PEG", PEG_MODULE_SOURCE, "_peg_module"},
      {"FST", FST_MODULE_SOURCE, "_fst_module"},
      {"Log", LOG_MODULE_SOURCE, "_log_module"},
      {"Path", PATH_MODULE_SOURCE, "_path_module"},
      {"Vector2", VECTOR2_MODULE_SOURCE, "_vector2_module"},
      {"Vector3", VECTOR3_MODULE_SOURCE, "_vector3_module"},
      {"Deque", DEQUE_MODULE_SOURCE, "_deque_module"},
      {"PriorityQueue", PRIORITY_QUEUE_MODULE_SOURCE,
       "_priority_queue_module"},
      {"StateMachine", STATE_MACHINE_MODULE_SOURCE, "_state_machine_module"},
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
// Which stdlib modules a token set names. The one place that rule lives:
// the preamble source, the trigger set the REPL remembers, and the baked/
// unbaked split all read it, so they cannot disagree about what a program
// pulled in. Matching the parsed token set rather than the raw source keeps
// a mention in a comment — or a longer identifier that merely contains the
// name — from inlining a whole module: "Terminal" in a comment used to pull
// in Term, and `side_effect` the effects runtime, each about a second of JIT
// compile for nothing. A `re'...'` / `re"..."` / `` re`...` `` regex literal
// desugars to `Regex.compile(...)` in the parser, so it shows up as a plain
// `Regex` token and needs no extra marker. Bare-function modules come in
// whole: naming any one member pulls in its group, since they share a source
// (one `assert_*` brings the family, as on the interp side, where
// initialize_lazy_group binds all ten at once).
struct StdlibSelection {
  std::vector<const LazyNsModule*> modules;
  std::vector<const LazyFnGroup*> groups;
};
inline StdlibSelection select_stdlib_modules(
    const std::unordered_set<std::string_view>& names) {
  StdlibSelection sel;
  for (const auto& m : lazy_ns_modules())
    if (names.contains(m.name)) sel.modules.push_back(&m);
  for (const auto& g : lazy_fn_groups())
    if (std::any_of(g.members.begin(), g.members.end(),
                    [&](std::string_view n) { return names.contains(n); }))
      sel.groups.push_back(&g);
  return sel;
}

// The registration source for one selected module — what the preamble is
// made of, one entry at a time (culebra_preamble_cc bakes exactly this).
inline std::string stdlib_module_source(const LazyNsModule& m) {
  return _wrap_lazy_ns_module(m.source, m.name, m.builder);
}
inline std::string stdlib_module_source(const LazyFnGroup& g) {
  return _wrap_lazy_fn_group(g);
}

inline std::string stdlib_preamble_for(
    const std::unordered_set<std::string_view>& names) {
  auto sel = select_stdlib_modules(names);
  std::string preamble;
  for (const auto* m : sel.modules) preamble.append(stdlib_module_source(*m));
  for (const auto* g : sel.groups) preamble.append(stdlib_module_source(*g));
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
  auto sel = select_stdlib_modules(names);
  std::unordered_set<std::string_view> out;
  for (const auto* m : sel.modules) out.insert(m->name);
  for (const auto* g : sel.groups)
    out.insert(g->members.begin(), g->members.end());
  return out;
}

// --- Baked preamble (the JIT/AOT lanes) -------------------------------------
//
// A stdlib module compiled into this binary at build time (CMake's
// culebra_preamble_cc step, src/preamble_cc.cc): the machine code the splice
// above would have lowered at every start-up, with an entry that performs
// the module's registration. A lowered program calls that entry instead of
// carrying the module — Args alone was 20k lines of IR and ~2 s of codegen
// per run — while the executor keeps compiling the spliced source, which is
// what the symmetry gate compares the baked code against. Only the driver
// carries a table (its generated baked_preambles.gen.cc, compiled under
// CULEBRA_HAS_BAKED_PREAMBLES): the list of what is baked, the anchor that
// keeps the objects in the driver, and the addresses the JIT defines the
// entries from (JIT::define_baked_preambles). Everyone else who reaches the
// lowering — the inner-loop driver (CULEBRA_DEV_NO_RT), a header-only
// embedder, culebra_preamble_cc itself — gets the empty one below and
// splices as before. CULEBRA_PREAMBLE_SOURCE=1 forces that for A/B measurements (read once at
// the resolver, beside the lane's own force_source, so one flag reaches the
// decision by one path).
struct BakedPreamble {
  const char* name;  // the registered name: `Time`, `__Matchers`, …
  void (*init)();    // baked_preamble_symbol(name): registers the builder
};
#ifdef CULEBRA_HAS_BAKED_PREAMBLES
std::span<const BakedPreamble> baked_preambles();
#else
inline std::span<const BakedPreamble> baked_preambles() { return {}; }
#endif

// The entry a baked module's object defines, spelled once — the lowering
// emits it, the tool names it, the gate greps it (ns_group_symbol's rule).
inline std::string baked_preamble_symbol(std::string_view name) {
  return "culebra_preamble_" + std::string(name);
}

inline const BakedPreamble* baked_preamble(std::string_view name) {
  for (const auto& b : baked_preambles())
    if (name == b.name) return &b;
  return nullptr;
}

// The built-in traits (shared.h's builtin_traits_preamble) as a baked entry.
// They are not part of the splice — every program registers them, whatever
// it names — so they never reach `baked` through resolve_baked_preamble, and
// a compiled lane ran them as a prologue it lowered itself. That prologue is
// the largest thing in a trivial program's IR: the five default bodies
// (`Eq.neq`, `Comparable.lt/le/gt/ge`) are ~4.8k of the ~6.2k lines `let x =
// 1` lowered, none of them reachable from the program's own code.
inline constexpr const char* kBuiltinTraitsBakedName = "__Traits";

// What a lane that calls baked entries runs first: the traits entry when
// this binary carries one, then the stdlib modules the splice took out. One
// helper so the JIT and AOT lanes cannot disagree about either half —
// `traits` is also what tells the compiler to leave the prologue out, and
// emitting the call without skipping the source would register twice.
// `force_source` is the caller's own reason to compile everything (a cross
// build links the user's archive); CULEBRA_PREAMBLE_SOURCE is the A/B flag
// plan_stdlib_preamble reads, honoured here so one run means one thing.
struct BakedEntries {
  std::vector<const BakedPreamble*> all;
  bool traits = false;
};
inline BakedEntries with_baked_traits(
    std::span<const BakedPreamble* const> stdlib, bool force_source = false) {
  BakedEntries e;
  const auto* t = (force_source || std::getenv("CULEBRA_PREAMBLE_SOURCE"))
                      ? nullptr
                      : baked_preamble(kBuiltinTraitsBakedName);
  e.traits = t != nullptr;
  e.all.reserve(stdlib.size() + (t ? 1 : 0));
  if (t) e.all.push_back(t);
  e.all.insert(e.all.end(), stdlib.begin(), stdlib.end());
  return e;
}

// The split a lane that can call the baked entries makes: the stdlib modules
// `names` pulls in, as the entries this binary baked plus the source of the
// rest — in the order stdlib_preamble_for emits, so the registrations still
// run in that order. The one place the split lives: the splice prunes with
// it (a lane that calls an entry must not also compile its source) and
// resolve_baked_preamble takes it back out of an already-spliced list, and
// they cannot disagree about what a program pulled in. Everything stays
// source where there is nothing to call — a header-only embedder, a
// CULEBRA_DEV_NO_RT driver — or where an A/B run asked for the splice.
struct StdlibPlan {
  std::vector<const BakedPreamble*> baked;
  std::string rest;
};
inline StdlibPlan plan_stdlib_preamble(
    const std::unordered_set<std::string_view>& names) {
  StdlibPlan p;
  if (baked_preambles().empty() || std::getenv("CULEBRA_PREAMBLE_SOURCE")) {
    p.rest = stdlib_preamble_for(names);
    return p;
  }
  auto sel = select_stdlib_modules(names);
  auto take = [&](std::string_view name, auto&& src) {
    if (auto* b = baked_preamble(name))
      p.baked.push_back(b);
    else
      p.rest.append(src());
  };
  for (const auto* m : sel.modules)
    take(m->name, [&] { return stdlib_module_source(*m); });
  for (const auto* g : sel.groups)
    take(g->name, [&] { return stdlib_module_source(*g); });
  return p;
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
// `baked`, when given, makes this a lane that calls the baked entries: they
// are collected there and their source is left out of the spliced module,
// so it is never parsed. That is the whole point of passing it — parsing the
// Args preamble alone costs ~19 ms, a fifth of a `--jit` start-up, for
// source the lane then drops (resolve_baked_preamble). A lane whose modules
// something else still reads — `culebra build`, whose namespace scan wants
// their AST — leaves it null and resolves later instead.
inline void splice_stdlib_preamble(
    std::vector<LoadedModule>& modules,
    std::vector<const BakedPreamble*>* baked = nullptr) {
  if (modules.empty()) return;
  // Scan every module, not just the entry: an imported module's `Canvas`
  // (or `Time`, ...) must pull that namespace's preamble in too, even when
  // the entry module never names it.
  std::unordered_set<std::string_view> names;
  for (const auto& m : modules) {
    if (m.ast) collect_ast_tokens(*m.ast, names);
  }
  std::string preamble;
  if (baked) {
    auto plan = plan_stdlib_preamble(names);
    *baked = std::move(plan.baked);
    preamble = std::move(plan.rest);
  } else {
    preamble = stdlib_preamble_for(names);
  }
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

// The lowering's view of a spliced module list: the `<stdlib>` module is
// replaced by one registering only the modules this binary has not baked
// (and dropped when that is none), and `baked` lists the entries the
// program calls first — where the spliced registrations used to run.
// `force_source` keeps the splice whole (a cross build links an archive
// that may not carry the objects).
struct BakedResolution {
  std::vector<LoadedModule> modules;
  std::vector<const BakedPreamble*> baked;
};
inline BakedResolution resolve_baked_preamble(
    const std::vector<LoadedModule>& modules, bool force_source) {
  BakedResolution r;
  r.modules = modules;
  // `force_source` is this lane's own reason to compile everything; the rest
  // only skips the scan below in the cases plan_stdlib_preamble would answer
  // "nothing baked" for anyway, so the two cannot disagree.
  if (force_source || baked_preambles().empty() ||
      std::getenv("CULEBRA_PREAMBLE_SOURCE") || modules.empty() ||
      modules.front().abs_path != kStdlibPreamblePath)
    return r;
  // The same scan the splice made, over the same (user) modules: what the
  // program names decides what it registers, baked or not.
  std::unordered_set<std::string_view> names;
  for (size_t i = 1; i < modules.size(); ++i)
    if (modules[i].ast) collect_ast_tokens(*modules[i].ast, names);
  auto plan = plan_stdlib_preamble(names);
  if (plan.baked.empty()) return r;
  r.baked = std::move(plan.baked);
  std::string rest = std::move(plan.rest);
  if (rest.empty()) {
    r.modules.erase(r.modules.begin());
    return r;
  }
  auto buf = std::make_shared<std::string>(std::move(rest));
  std::vector<std::string> msgs;
  auto ast = parse_with_transforms(kStdlibPreamblePath, *buf, msgs);
  if (!ast) {  // a build bug; keep the full splice rather than lose a module
    r.baked.clear();
    return r;  // r.modules is still the caller's list
  }
  r.modules.front().source = std::move(buf);
  r.modules.front().ast = std::move(ast);
  return r;
}

// The compile-time face of the baked stdlib modules: the parsed registration
// source of every baked module that mentions `@value`. A lane that calls a
// baked entry never compiles that source, so without this the compiler never
// sees the module's `@value` class declarations — and the unbox splice
// (Compiler::postfix_value_class) can only fire on a declaration it holds.
// Parsed, never compiled or run: the baked entry still carries the module's
// code and registrations, this only restores what the splice would have seen
// (Compiler::register_stdlib_value_decls consumes the ASTs). The raw-source
// scan over-approximates — a mention in a comment costs one small parse.
inline std::vector<LoadedModule> parse_baked_value_decls(
    std::span<const BakedPreamble* const> baked) {
  std::vector<LoadedModule> decls;
  for (const auto* b : baked) {
    std::string src;
    for (const auto& m : lazy_ns_modules())
      if (std::string_view(m.name) == b->name &&
          std::string_view(m.source).find("@value") != std::string_view::npos)
        src = stdlib_module_source(m);
    if (src.empty()) continue;  // bare-function groups carry no classes
    auto buf = std::make_shared<std::string>(std::move(src));
    std::vector<std::string> msgs;
    auto ast = parse_with_transforms(kStdlibPreamblePath, *buf, msgs);
    if (!ast) continue;  // stdlib is trusted; a parse failure is a build bug
    LoadedModule d;
    d.abs_path = kStdlibPreamblePath;
    d.source = std::move(buf);
    d.ast = std::move(ast);
    decls.push_back(std::move(d));
  }
  return decls;
}

}  // namespace culebra
