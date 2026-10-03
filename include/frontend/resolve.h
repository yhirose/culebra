#pragma once

// Static name resolution over the source as written: which declaration each
// identifier refers to. It is the one statement of culebra's scope rules: the
// compiler's captures are derived from it (fn_analysis.h), its own lookups are
// held to it (scope_check.h), and an editor's go-to-definition,
// find-references, highlight and rename read it:
//
//  - Scopes: the module; each function-like body (`fn`, a lambda, `fn name`, a
//    method, `defer`, `effect fn`, a handler clause); a class's field
//    initializers, which run when an instance is made, as one function body
//    with a scope per initializer; a static value, where the class is
//    declared; a parameter's default; a `{}` block; a loop body; a match
//    arm; a try body and its catch; the block `handle` runs; and the init
//    clause around its `if` / `while` / `match`. An `if` arm shares the
//    enclosing scope. The builder a `_lazy_ns_register` call registers is a
//    module of its own.
//  - Within one function a declaration is visible from its statement on, after
//    its right-hand side (`let x = x` reads the outer `x`). Every declaration of
//    one name in one scope is the same variable: a repeated `let`, a bare
//    `x = v`, the overloads of `fn name`.
//  - A bare `x = v` (or a bare destructure) writes the `x` visible there, or
//    declares one in the innermost scope when none is, unless `x` is a stdlib
//    global (Options::globals), which refuses the write instead.
//  - In a session (the REPL, `culebra test`), the top level of every input is
//    one scope: the names earlier inputs declared (Options::session) are its
//    variables from the start.
//  - A function body sees an enclosing function's variables wherever they are
//    declared, since a closure captures the variable itself; so each function
//    body is resolved after the whole body that encloses it. A parameter's
//    default is the exception: it sees the parameters before it and where
//    the function is defined, so a function in it is resolved right there.
//
// A name nothing here declares — a stdlib global, the receiver `self`, a
// NameError at run time — resolves to no symbol; a `let self` is a variable. A member (`o.x`) and an object key spelled
// as a name (`{x: 1}`) are not names; a computed key and a decorator's callee
// are expressions like any other.

#include "frontend/parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace culebra::resolve {

inline constexpr size_t kNone = static_cast<size_t>(-1);

enum class SymbolKind : uint8_t {
  Variable,
  Parameter,
  Function,
  Class,
  Enum,
  Import,
  EffectOperation,
};

enum class Role : uint8_t { Declaration, Read, Write };

// How a declaration is written.
enum class Form : uint8_t {
  Let,        // `let x`, a leaf of a `let` pattern
  Mut,        // `mut x`, `let mut x`, a leaf of a `mut` pattern
  Bare,       // `x = v`, a leaf of a bare pattern, where nothing visible is `x`
  Parameter,  // a parameter, a leaf of a pattern parameter
  Loop,       // a `for` binding
  Catch,
  Pattern,    // a match arm's binding
  Function,
  Class,
  Enum,
  Import,
  Operation,  // `effect fn`
};

// How an occurrence is spelled, which a rename has to respect.
enum class Spelling : uint8_t {
  Plain,
  ObjectShorthand,   // `{x}` in an object literal: the key and the value at once
  PatternShorthand,  // `let {x} = o`: the key and the binding at once
  KeywordLabel,      // `f(x: 1)`: names a parameter of `f`
};

struct Occurrence {
  size_t position = 0;  // byte offset of the name in the source
  size_t length = 0;
  size_t symbol = kNone;
  size_t scope = kNone;  // the scope the name is written in
  Role role = Role::Read;
  Spelling spelling = Spelling::Plain;
};

struct Symbol {
  std::string name;
  SymbolKind kind = SymbolKind::Variable;
  size_t scope = kNone;
  bool exported = false;               // named by an `export { ... }`
  std::vector<size_t> occurrences;     // into Resolution::occurrences, in order
  const peg::Ast* declared_at = nullptr;  // the name node that first declares it
  size_t declarations = 0;  // the statements declaring it, synthesized ones included
  uint16_t forms = 0;       // a bit per Form that declares it
  bool declared_as(Form f) const { return forms >> static_cast<int>(f) & 1; }
};

struct Scope {
  size_t parent = kNone;
  bool function = false;
  size_t begin = 0, end = 0;  // the bytes of the syntax that opens it
  std::map<std::string, size_t, std::less<>> names;  // name -> symbol
  // The symbol a function scope's function is bound to (a `fn name`, or the
  // name a literal is assigned to), else kNone.
  size_t owner = kNone;
};

struct Options {
  // Record what each name node is (Resolution::uses, declarations,
  // unbound_reads),
  // the nodes a desugaring or a lowering synthesized included: what a
  // consumer of the AST rather than of the source text — the compiler, the
  // load-time lint — looks names up by.
  bool record_nodes = false;
  // The stdlib's global names (lint::builtin_names()). A bare write to one
  // that nothing declares is refused by the global rather than declaring a
  // variable; with none given, it declares.
  const std::set<std::string, std::less<>>* globals = nullptr;
  // The names earlier inputs of a session have declared: the module scope's
  // variables before this input's first statement.
  std::span<const std::string> session;
};

// What a name node is, kept per node the walk looked up
// (Options::record_nodes).
struct Use {
  size_t symbol = kNone;  // kNone: nothing declares the name
  size_t scope = kNone;   // the scope the name is written in
};

// A declaration, by its name node.
struct Declaration {
  const peg::Ast* node = nullptr;
  size_t symbol = kNone;
};

// A name read or written where nothing declares it.
struct Unresolved {
  std::string name;
  size_t position = 0;
  size_t scope = kNone;
};

// `import Alias from 'path'`.
struct Import {
  size_t symbol = kNone;
  std::string path;
};

// `v.name(...)`: a free function `name` may be what the call reaches (UFCS),
// which no static pass can decide. `symbol` is what `name` resolves to where
// the call is written, as a read there would (kNone: nothing).
struct MethodCall {
  const peg::Ast* node = nullptr;
  size_t symbol = kNone;
  size_t scope = kNone;
  const peg::Ast* receiver = nullptr;  // the postfix chain before the call
};

// `Alias.member` where Alias is an import: `member` names a top-level
// declaration of the imported module.
struct ModuleMember {
  size_t import_symbol = kNone;
  std::string member;
  size_t position = 0;
  size_t length = 0;
};

struct Resolution {
  std::vector<Scope> scopes;  // [0] is the module
  std::vector<Symbol> symbols;
  std::vector<Occurrence> occurrences;  // sorted by position
  std::vector<Unresolved> unresolved;
  std::vector<Import> imports;
  std::vector<ModuleMember> module_members;
  std::vector<MethodCall> method_calls;  // in walk order

  // The occurrence whose name spans `position` (its end included, so a cursor
  // just past a name still finds it), or nullptr.
  const Occurrence* occurrence_at(size_t position) const {
    auto it = std::upper_bound(
        occurrences.begin(), occurrences.end(), position,
        [](size_t p, const Occurrence& o) { return p < o.position; });
    if (it == occurrences.begin()) return nullptr;
    --it;
    return position <= it->position + it->length ? &*it : nullptr;
  }

  // The symbol `name` resolves to from `scope`, searching outward.
  size_t lookup(size_t scope, std::string_view name) const {
    for (size_t s = scope; s != kNone; s = scopes[s].parent) {
      auto it = scopes[s].names.find(name);
      if (it != scopes[s].names.end()) return it->second;
    }
    return kNone;
  }

  const ModuleMember* module_member_at(size_t position) const {
    for (const auto& m : module_members)
      if (m.position <= position && position <= m.position + m.length)
        return &m;
    return nullptr;
  }

  // The innermost scope whose syntax spans `position`.
  // Scopes nest, so the last one to begin at or before `position` is the
  // innermost that spans it or lies inside it; its parents lead out to it.
  size_t scope_at(size_t position) const {
    auto it = std::upper_bound(
        scopes_by_begin.begin(), scopes_by_begin.end(), position,
        [&](size_t p, size_t i) { return p < scopes[i].begin; });
    if (it == scopes_by_begin.begin()) return 0;
    for (size_t s = *(it - 1); s != kNone; s = scopes[s].parent)
      if (scopes[s].begin <= position && position <= scopes[s].end) return s;
    return 0;
  }
  std::vector<size_t> scopes_by_begin;  // scope indices, ordered by begin

  // With Options::record_nodes: each name node the walk looked up. Keyword
  // labels and implicit names (`_`, `self`, `fn`, `__NAME__`) are left out.
  std::unordered_map<const peg::Ast*, Use> uses;
  // With Options::record_nodes, in walk order (a function's body after the
  // body around it): every declaration, and every read nothing declares.
  std::vector<Declaration> declarations;
  std::vector<const peg::Ast*> unbound_reads;

  // What a name node resolves to: kNone when nothing declares it, or when
  // the walk never looked it up.
  size_t symbol_of(const peg::Ast& name) const {
    auto it = uses.find(&name);
    return it == uses.end() ? kNone : it->second.symbol;
  }
  // With Options::record_nodes: each function body to its scope.
  std::unordered_map<const peg::Ast*, size_t> body_scope;
  // With Options::record_nodes: each class with field initializers to the
  // function scope they run in.
  std::unordered_map<const peg::Ast*, size_t> initializer_scope;

  // The function scope a symbol is declared in.
  size_t frame_of(size_t symbol) const {
    return function_of(symbols[symbol].scope);
  }

  // The function scope `scope` is in (itself, if it is one).
  size_t function_of(size_t s) const {
    while (s != kNone && !scopes[s].function) s = scopes[s].parent;
    return s;
  }

  // Where a symbol is first declared, or kNone.
  size_t first_declaration(size_t symbol) const {
    for (size_t i : symbols[symbol].occurrences)
      if (occurrences[i].role == Role::Declaration)
        return occurrences[i].position;
    return kNone;
  }

  // Whether a method call may reach `symbol` through UFCS.
  bool called_as_method(size_t symbol) const {
    return std::ranges::any_of(
        method_calls, [&](const MethodCall& m) { return m.symbol == symbol; });
  }
};

// The byte offset in `source` of a name node's text, or kNone for a node the
// parse synthesized (a desugaring) whose text is not in the source.
inline size_t name_offset(const peg::Ast& n, std::string_view name,
                          std::string_view source) {
  const char* p = n.is_token && !n.token.empty() ? n.token.data()
                                                 : source.data() + n.position;
  if (p < source.data() || p + name.size() > source.data() + source.size())
    return kNone;
  size_t off = static_cast<size_t>(p - source.data());
  return source.substr(off, name.size()) == name ? off : kNone;
}

namespace _detail {

using namespace peg::udl;

class Resolver {
 public:
  Resolver(const peg::Ast& root, std::string_view source, Options opts)
      : root_(root), src_(source), opts_(opts) {}

  Resolution run() {
    cur_ = push_scope(kNone, /*function=*/true, 0, src_.size());
    for (const auto& name : opts_.session)
      if (!implicit(name)) symbol_in(cur_, name, SymbolKind::Variable, nullptr);
    walk_body(root_);
    for (const auto* e : exports_) {
      read(*e, e->token, Spelling::Plain);
      size_t sym = r_.lookup(0, e->token);
      if (sym != kNone) r_.symbols[sym].exported = true;
    }
    drain_jobs();
    for (const auto& l : labels_) {
      auto it = params_of_.find(l.callee);
      if (it == params_of_.end()) continue;
      for (size_t p : it->second)
        if (r_.symbols[p].name == l.node->token)
          add(*l.node, l.node->token, p, l.scope, Role::Read,
              Spelling::KeywordLabel);
    }
    finish();
    return std::move(r_);
  }

 private:
  // A function-like body, resolved once the body enclosing it is complete.
  struct Job {
    const peg::Ast* params;  // PARAMETERS / LAMBDA_PARAMS, or nullptr
    const peg::Ast* body;
    size_t parent;
    size_t owner;  // the symbol the function is bound to, for keyword labels
    bool initializers = false;  // `body` is a CLASS_DECL with initializers
  };
  struct Label {
    size_t callee;
    const peg::Ast* node;
    size_t scope;
  };

  // ---- bookkeeping ----------------------------------------------------------

  size_t push_scope(size_t parent, bool function, size_t begin, size_t end) {
    Scope s;
    s.parent = parent;
    s.function = function;
    s.begin = begin;
    s.end = end;
    r_.scopes.push_back(std::move(s));
    return r_.scopes.size() - 1;
  }

  // Byte offset of a name node's text, or kNone for a node the parse
  // synthesized (a desugaring) whose text is not in the source.
  size_t offset_of(const peg::Ast& n, std::string_view name) const {
    return name_offset(n, name, src_);
  }

  void add(const peg::Ast& n, std::string_view name, size_t sym, size_t scope,
           Role role, Spelling spelling) {
    if (opts_.record_nodes && spelling != Spelling::KeywordLabel)
      r_.uses[&n] = {sym, scope};
    size_t off = offset_of(n, name);
    if (off == kNone) return;
    r_.occurrences.push_back({off, name.size(), sym, scope, role, spelling});
  }

  // A name read or written where nothing declares it.
  void add_unresolved(const peg::Ast& n, std::string_view name,
                      Role role = Role::Read) {
    if (opts_.record_nodes) {
      r_.uses[&n] = {kNone, cur_};
      if (role == Role::Read) r_.unbound_reads.push_back(&n);
    }
    size_t off = offset_of(n, name);
    if (off != kNone) r_.unresolved.push_back({std::string(name), off, cur_});
  }

  // A name the runtime binds, which no declaration makes a variable. `self`
  // is the exception: a frame's receiver unless a declaration names it (`let
  // self`), when it is a variable like any other.
  static bool implicit(std::string_view name) {
    return name == "_" || (is_always_bound_name(name) && !receiver(name));
  }
  static bool receiver(std::string_view name) { return name == "self"; }

  // The variable `name` is in `scope`, made the first time.
  size_t symbol_in(size_t scope, std::string_view name, SymbolKind kind,
                   const peg::Ast* at) {
    auto& names = r_.scopes[scope].names;
    if (auto it = names.find(name); it != names.end()) return it->second;
    size_t sym = r_.symbols.size();
    Symbol s;
    s.name = std::string(name);
    s.kind = kind;
    s.scope = scope;
    s.declared_at = at;
    r_.symbols.push_back(std::move(s));
    names.emplace(std::string(name), sym);
    return sym;
  }

  static SymbolKind kind_of(Form form) {
    switch (form) {
      case Form::Parameter: return SymbolKind::Parameter;
      case Form::Function: return SymbolKind::Function;
      case Form::Class: return SymbolKind::Class;
      case Form::Enum: return SymbolKind::Enum;
      case Form::Import: return SymbolKind::Import;
      case Form::Operation: return SymbolKind::EffectOperation;
      default: return SymbolKind::Variable;
    }
  }

  size_t declare(const peg::Ast& n, std::string_view name, Form form,
                 Spelling spelling = Spelling::Plain) {
    if (implicit(name)) return kNone;
    size_t sym = symbol_in(cur_, name, kind_of(form), &n);
    r_.symbols[sym].declarations++;
    r_.symbols[sym].forms |= static_cast<uint16_t>(1u << static_cast<int>(form));
    if (opts_.record_nodes) r_.declarations.push_back({&n, sym});
    add(n, name, sym, cur_, Role::Declaration, spelling);
    return sym;
  }

  bool global(std::string_view name) const {
    return opts_.globals && opts_.globals->contains(name);
  }

  size_t read(const peg::Ast& n, std::string_view name, Spelling spelling) {
    if (implicit(name)) return kNone;
    size_t sym = r_.lookup(cur_, name);
    if (sym == kNone) {
      if (!receiver(name)) add_unresolved(n, name);
      return kNone;
    }
    add(n, name, sym, cur_, Role::Read, spelling);
    return sym;
  }

  // `x = v`: the visible `x`, else a new one here — or, for a stdlib global,
  // the global, which refuses it.
  size_t bare_write(const peg::Ast& n, std::string_view name,
                    Spelling spelling = Spelling::Plain) {
    if (implicit(name)) return kNone;
    size_t sym = r_.lookup(cur_, name);
    if (sym == kNone && receiver(name)) return kNone;  // the receiver refuses it
    if (sym == kNone && global(name)) {
      add_unresolved(n, name, Role::Write);
      return kNone;
    }
    if (sym == kNone) return declare(n, name, Form::Bare, spelling);
    add(n, name, sym, cur_, Role::Write, spelling);
    return sym;
  }

  void enqueue(const peg::Ast* params, const peg::Ast* body,
               size_t owner = kNone) {
    enqueue_in(cur_, params, body, owner);
  }
  void enqueue_in(size_t parent, const peg::Ast* params, const peg::Ast* body,
                  size_t owner) {
    if (body) jobs_.push_back({params, body, parent, owner});
  }

  void finish() {
    std::stable_sort(r_.occurrences.begin(), r_.occurrences.end(),
                     [](const Occurrence& a, const Occurrence& b) {
                       return a.position < b.position;
                     });
    for (size_t i = 0; i < r_.occurrences.size(); i++)
      r_.symbols[r_.occurrences[i].symbol].occurrences.push_back(i);
    auto& order = r_.scopes_by_begin;
    order.resize(r_.scopes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return r_.scopes[a].begin < r_.scopes[b].begin;
    });
  }

  // ---- walking --------------------------------------------------------------

  // A body whose single statement the parse collapsed into the statement
  // itself, or a STATEMENTS list.
  void walk_body(const peg::Ast& n) {
    if (n.tag == "STATEMENTS"_) {
      for (const auto& c : n.nodes) walk(*c);
    } else {
      walk(n);
    }
  }

  static size_t end_of(const peg::Ast& n) { return n.position + n.length; }

  void scoped_body(const peg::Ast& n) {
    size_t saved = cur_;
    cur_ = push_scope(cur_, false, n.position, end_of(n));
    walk_body(n);
    cur_ = saved;
  }

  void run_job(const Job& job) {
    size_t saved = cur_;
    cur_ = push_scope(job.parent, true,
                      job.params ? job.params->position : job.body->position,
                      end_of(*job.body));
    r_.scopes[cur_].owner = job.owner;
    if (job.initializers) {
      if (opts_.record_nodes) r_.initializer_scope[job.body] = cur_;
      for_each_value(*job.body, /*instance=*/true,
                     [&](const peg::Ast& v) { scoped_body(v); });
      cur_ = saved;
      return;
    }
    if (opts_.record_nodes) r_.body_scope[job.body] = cur_;
    if (job.params) {
      for (const auto& p : job.params->nodes) {
        if (is_kw_only_sep(*p)) continue;
        if (is_pattern_param(*p)) {
          bind_pattern(*p, Form::Parameter);
          continue;
        }
        if (const auto* d = extract_default_expr(*p)) walk_default(*d);
        const peg::Ast& name_node = param_name_node(*p);
        size_t sym = declare(name_node, name_node.token, Form::Parameter);
        if (sym != kNone && job.owner != kNone)
          params_of_[job.owner].push_back(sym);
      }
    }
    walk_body(*job.body);
    cur_ = saved;
  }

  void drain_jobs() {
    while (!jobs_.empty()) {
      Job job = jobs_.front();
      jobs_.pop_front();
      run_job(job);
    }
  }

  // A default sees where its function is defined and the parameters before
  // it, not its own or a later one, a closure in it included: the functions
  // it holds are resolved before the next parameter is declared. What it
  // declares is its own.
  void walk_default(const peg::Ast& d) {
    auto enclosing = std::exchange(jobs_, {});
    scoped_body(d);
    drain_jobs();
    jobs_ = std::move(enclosing);
  }

  // `form`: how each leaf is declared; Form::Bare writes the visible name.
  void bind_pattern(const peg::Ast& pat, Form form) {
    switch (pat.tag) {
      case "PATTERN"_:
      case "ARRAY_PATTERN"_:
      case "TUPLE_PATTERN"_:
      case "FOR_BINDING"_:
        for (const auto& c : pat.nodes) bind_pattern(*c, form);
        return;
      case "IDENTIFIER"_:
        bind(pat, pat.token, form,
             pat.original_tag == "OBJECT_PAT_ENTRY"_ ? Spelling::PatternShorthand
                                                     : Spelling::Plain);
        return;
      case "TYPED_IDENT"_:
        bind(*pat.nodes[0], pat.nodes[0]->token, form, Spelling::Plain);
        return;
      case "REST_PATTERN"_:
        if (!pat.nodes.empty())
          bind(*pat.nodes[0], pat.nodes[0]->token, form, Spelling::Plain);
        return;
      case "CTOR_PATTERN"_:
        for (size_t i = 1; i < pat.nodes.size(); i++)
          bind_pattern(*pat.nodes[i], form);
        return;
      case "OBJECT_PATTERN"_:
        for (const auto& e : pat.nodes) {
          if (e->tag == "OBJECT_PAT_ENTRY"_ && e->nodes.size() >= 2)
            bind_pattern(*e->nodes[1], form);
          else if (e->tag == "IDENTIFIER"_)
            bind(*e, e->token, form, Spelling::PatternShorthand);
        }
        return;
      default:
        return;  // a literal, a wildcard: binds nothing
    }
  }

  void bind(const peg::Ast& n, std::string_view name, Form form,
            Spelling spelling) {
    if (form == Form::Bare)
      bare_write(n, name, spelling);
    else
      declare(n, name, form, spelling);
  }

  // `let mut x` is mutable: `mut` decides.
  static Form declared_form(bool is_mut) {
    return is_mut ? Form::Mut : Form::Let;
  }

  static bool is_function_literal(const peg::Ast& n) {
    return n.tag == "FUNCTION"_ || n.tag == "LAMBDA"_;
  }

  void enqueue_literal(const peg::Ast& fn, size_t owner, size_t parent) {
    if (fn.tag == "FUNCTION"_) {
      auto fv = view_function(fn);
      enqueue_in(parent, fv.params, fv.body.get(), owner);
    } else {
      auto lv = view_lambda(fn);
      enqueue_in(parent, lv.params, lv.body.get(), owner);
    }
  }

  // A class's field initializers (run per instance) or static values (run
  // where the class is declared), each by its value expression.
  template <typename F>
  static void for_each_value(const peg::Ast& class_decl, bool instance, F&& f) {
    for (size_t j = first_non_decorator_index(class_decl) + 1;
         j < class_decl.nodes.size(); j++) {
      if (class_decl.nodes[j]->tag != "METHOD"_) continue;
      auto mv = view_method(*class_decl.nodes[j]);
      if (mv.value && mv.instance_field() == instance) f(*mv.value);
    }
  }

  void walk_assignment(const peg::Ast& n) {
    auto av = view_assignment(n);
    const peg::Ast* target = assign_name_target(n, av);
    if (!target) {
      for (int k = 0; k < av.lvalcnt; k++) walk(*n.nodes[av.lvaloff + k]);
      walk(*av.rhs);
      return;
    }
    std::string_view name = target->token;
    if (av.compound) {
      walk(*av.rhs);
      if (implicit(name)) return;
      size_t sym = r_.lookup(cur_, name);
      if (sym == kNone) {
        if (!receiver(name)) add_unresolved(*target, name, Role::Write);
      } else {
        add(*target, name, sym, cur_, Role::Write, Spelling::Plain);
      }
      return;
    }
    // A function literal is bound to the name it is assigned to; its body runs
    // later, so binding first changes nothing it reads.
    if (is_function_literal(*av.rhs)) {
      size_t sym = (av.is_let || av.is_mut)
                       ? declare(*target, name, declared_form(av.is_mut))
                       : bare_write(*target, name);
      enqueue_literal(*av.rhs, sym, cur_);
      return;
    }
    walk(*av.rhs);
    if (av.is_let || av.is_mut)
      declare(*target, name, declared_form(av.is_mut));
    else
      bare_write(*target, name);
  }

  void walk_call(const peg::Ast& n) {
    if (const peg::Ast* builder = lazy_ns_builder(n)) {
      enqueue_literal(*builder, kNone, /*parent=*/kNone);
      return;
    }
    const peg::Ast& head = *n.nodes[0];
    walk(head);
    size_t callee = kNone;
    size_t import_sym = kNone;
    if (head.tag == "IDENTIFIER"_ && head.original_tag != "DOT"_ &&
        head.original_tag != "SAFE_DOT"_ && !implicit(head.token)) {
      callee = r_.lookup(cur_, head.token);
      if (callee != kNone && r_.symbols[callee].kind == SymbolKind::Import)
        import_sym = callee;
    }
    for (size_t i = 1; i < n.nodes.size(); i++) {
      const peg::Ast& c = *n.nodes[i];
      bool member = c.tag == "IDENTIFIER"_ &&
                    (c.original_tag == "DOT"_ || c.original_tag == "SAFE_DOT"_);
      if (member) {
        if (i + 1 < n.nodes.size() && n.nodes[i + 1]->original_tag == "ARGUMENTS"_)
          r_.method_calls.push_back(
              {&c, r_.lookup(cur_, c.token), cur_, n.nodes[i - 1].get()});
        if (i == 1 && import_sym != kNone) {
          size_t off = offset_of(c, c.token);
          if (off != kNone)
            r_.module_members.push_back(
                {import_sym, std::string(c.token), off, c.token.size()});
        }
        continue;
      }
      if (i == 1 && callee != kNone && c.original_tag == "ARGUMENTS"_) {
        for (const auto& arg : c.nodes)
          if (arg->tag == "KWARG"_ && !arg->nodes.empty())
            labels_.push_back({callee, arg->nodes[0].get(), cur_});
      }
      walk(c);
    }
  }

  void walk_decl_head(const peg::Ast& n, Form form, size_t* sym_out) {
    size_t i = first_non_decorator_index(n);
    for (size_t k = 0; k < i; k++) walk(*n.nodes[k]);
    if (i >= n.nodes.size()) return;
    const peg::Ast& head = *n.nodes[i];
    auto name = parse_generic_head(head.token).outer;
    size_t sym = declare(head, name, form);
    if (sym_out) *sym_out = sym;
  }

  // `if` / `while` / `match` with an init clause: its bindings get a scope
  // around the whole construct.
  void open_init_scope(const peg::Ast& n, const peg::Ast* init) {
    if (!init) return;
    cur_ = push_scope(cur_, false, n.position, end_of(n));
    for (const auto& b : init->nodes) walk(*b);
  }

  void walk(const peg::Ast& n) {
    switch (n.tag) {
      case "IDENTIFIER"_:
        if (n.original_tag == "DOT"_ || n.original_tag == "SAFE_DOT"_) return;
        if (n.is_token) read(n, n.token, Spelling::Plain);
        return;

      case "CALL"_:
        if (n.nodes.empty()) return;
        walk_call(n);
        return;

      case "KWARG"_:
        if (n.nodes.size() >= 2) walk(*n.nodes[1]);
        return;

      case "OBJECT_PROPERTY"_: {
        auto pv = view_object_property(n);
        if (pv.is_shorthand) {
          if (pv.key->tag == "IDENTIFIER"_)
            read(*pv.key, pv.key->token, Spelling::ObjectShorthand);
        } else {
          // A name key is the key's spelling; any other key is an expression.
          if (pv.key->tag != "IDENTIFIER"_) walk(*pv.key);
          walk(*pv.value);
        }
        return;
      }

      case "ASSIGNMENT"_:
        walk_assignment(n);
        return;

      case "DESTRUCTURE_ASSIGN"_: {
        if (n.nodes.size() < 4) return;
        walk(*n.nodes[3]);
        bool is_mut = n.nodes[1]->token == "mut";
        bool declared = n.nodes[0]->token == "let" || is_mut;
        bind_pattern(*n.nodes[2], declared ? declared_form(is_mut) : Form::Bare);
        return;
      }

      case "PLACE_ASSIGN"_: {
        std::vector<const peg::Ast*> names;
        for_each_place_target(
            n, [&](const peg::Ast& chain) { walk(chain); },
            [&](const peg::Ast& name) { names.push_back(&name); });
        walk(*n.nodes.back());
        for (const auto* name : names) bare_write(*name, name->token);
        return;
      }

      case "MULTIFN_DECL"_: {
        size_t sym = kNone;
        walk_decl_head(n, Form::Function, &sym);
        size_t i = first_non_decorator_index(n);
        // [DECORATOR*, head, PARAMETERS, (RETURN_TYPE)?, body]
        if (i + 3 <= n.nodes.size())
          enqueue(n.nodes[i + 1].get(), n.nodes.back().get(), sym);
        return;
      }

      case "EFFECT_FN_DECL"_: {
        size_t sym = kNone;
        walk_decl_head(n, Form::Operation, &sym);
        if (effect_fn_has_body(n) && n.nodes.size() >= 3)
          enqueue(n.nodes[1].get(), n.nodes.back().get(), sym);
        return;
      }

      case "CLASS_DECL"_: {
        walk_decl_head(n, Form::Class, nullptr);
        bool initializers = false;
        for (size_t j = first_non_decorator_index(n) + 1; j < n.nodes.size();
             j++) {
          if (n.nodes[j]->tag != "METHOD"_) continue;
          auto mv = view_method(*n.nodes[j]);
          if (mv.body) enqueue(mv.params, mv.body->get());
          initializers |= mv.value && mv.instance_field();
        }
        for_each_value(n, /*instance=*/false,
                       [&](const peg::Ast& v) { scoped_body(v); });
        if (initializers) jobs_.push_back({nullptr, &n, cur_, kNone, true});
        return;
      }

      case "TRAIT_DECL"_: {
        size_t i = first_non_decorator_index(n);
        for (size_t j = i + 1; j < n.nodes.size(); j++) {
          if (n.nodes[j]->tag != "TRAIT_METHOD"_) continue;
          auto tv = view_trait_method(*n.nodes[j]);
          if (tv.body) enqueue(tv.params, tv.body.get());
        }
        return;
      }

      case "ENUM_DECL"_:
        walk_decl_head(n, Form::Enum, nullptr);
        return;

      case "IMPORT_STMT"_:
        if (!n.nodes.empty() && n.nodes[0]->is_token) {
          size_t sym = declare(*n.nodes[0], n.nodes[0]->token, Form::Import);
          if (sym != kNone && n.nodes.size() >= 2)
            r_.imports.push_back({sym, std::string(n.nodes[1]->token)});
        }
        return;

      case "EXPORT_STMT"_:
        // The module reads its exports once it has run to the end.
        for (const auto& c : n.nodes)
          if (c->tag == "IDENTIFIER"_) exports_.push_back(c.get());
        return;

      case "FUNCTION"_:
      case "LAMBDA"_:
        enqueue_literal(n, kNone, cur_);
        return;

      case "DEFER"_:
        if (!n.nodes.empty()) enqueue(nullptr, n.nodes[0].get());
        return;

      case "DECORATOR"_:
        // The callee is read when the declaration runs, in the scope around
        // it; a compiler directive (`@value`, `@packable`, `@derive(...)`)
        // reads nothing.
        if (!is_compile_time_decorator(n) && !n.nodes.empty()) walk(*n.nodes[0]);
        return;

      case "LEXICAL_SCOPE"_: {
        size_t saved = cur_;
        cur_ = push_scope(cur_, false, n.position, end_of(n));
        for (const auto& c : n.nodes) walk_body(*c);
        cur_ = saved;
        return;
      }

      case "FOR"_: {
        if (n.nodes.size() < 3) break;
        auto fv = view_for(n);
        walk(*fv.iter);
        size_t saved = cur_;
        cur_ = push_scope(cur_, false, fv.binding->position, end_of(*fv.body));
        bind_pattern(*fv.binding, Form::Loop);
        walk_body(*fv.body);
        cur_ = saved;
        if (fv.nobreak) scoped_body(*fv.nobreak);
        return;
      }

      case "WHILE"_: {
        if (n.nodes.size() < 2) break;
        auto wv = view_while(n);
        size_t saved = cur_;
        open_init_scope(n, wv.init);
        walk(*wv.cond);
        scoped_body(*wv.body);
        if (wv.nobreak) scoped_body(*wv.nobreak);
        cur_ = saved;
        return;
      }

      case "IF"_: {
        auto iv = view_if(n);
        size_t saved = cur_;
        open_init_scope(n, iv.init);
        for (size_t i = iv.arm_off; i < n.nodes.size(); i++)
          walk_body(*n.nodes[i]);
        cur_ = saved;
        return;
      }

      case "MATCH"_: {
        if (n.nodes.size() < 2) break;
        auto mv = view_match(n);
        size_t saved = cur_;
        open_init_scope(n, mv.init);
        walk(*mv.subject);
        size_t around = cur_;
        for (const auto& arm : mv.arms->nodes) {
          if (arm->nodes.empty()) continue;
          cur_ = push_scope(around, false, arm->position, end_of(*arm));
          bind_pattern(*arm->nodes[0], Form::Pattern);
          for (size_t k = 1; k < arm->nodes.size(); k++)
            walk_body(*arm->nodes[k]);
          cur_ = around;
        }
        cur_ = saved;
        return;
      }

      case "TRY"_: {
        if (n.nodes.size() < 3) break;
        scoped_body(*n.nodes[0]);
        size_t saved = cur_;
        cur_ = push_scope(cur_, false, n.nodes[1]->position, end_of(*n.nodes[2]));
        if (n.nodes[1]->is_token)
          declare(*n.nodes[1], n.nodes[1]->token, Form::Catch);
        walk_body(*n.nodes[2]);
        cur_ = saved;
        return;
      }

      case "HANDLE"_: {
        if (n.nodes.empty()) return;
        scoped_body(*n.nodes[0]);
        for (size_t j = 1; j < n.nodes.size(); j++) {
          const peg::Ast& clause = *n.nodes[j];
          if (clause.tag == "HANDLE_CLAUSE"_ && clause.nodes.size() >= 3) {
            read(*clause.nodes[0], clause.nodes[0]->token, Spelling::Plain);
            enqueue(clause.nodes[1].get(), clause.nodes[2].get());
          } else if (clause.tag == "RETURN_CLAUSE"_ && clause.nodes.size() >= 2) {
            enqueue(clause.nodes[0].get(), clause.nodes[1].get());
          }
        }
        return;
      }

      case "PERFORM"_:
        if (!n.nodes.empty() && n.nodes[0]->tag == "IDENTIFIER"_)
          read(*n.nodes[0], n.nodes[0]->token, Spelling::Plain);
        for (size_t i = 1; i < n.nodes.size(); i++) walk(*n.nodes[i]);
        return;

      default:
        break;
    }
    for (const auto& c : n.nodes) walk(*c);
  }

  const peg::Ast& root_;
  std::string_view src_;
  Options opts_;
  Resolution r_;
  size_t cur_ = kNone;
  std::deque<Job> jobs_;
  std::vector<Label> labels_;
  std::vector<const peg::Ast*> exports_;
  std::map<size_t, std::vector<size_t>> params_of_;  // function -> parameters
};

}  // namespace _detail

// Resolve every name in a module parsed from `source` (the buffer the AST's
// tokens view, after the parse normalized it).
inline Resolution resolve_module(const peg::Ast& root, std::string_view source,
                                 Options opts = {}) {
  return _detail::Resolver(root, source, opts).run();
}

// ---- outline ----------------------------------------------------------------

enum class OutlineKind : uint8_t {
  Function,
  Class,
  Constructor,
  Method,
  Field,
  Enum,
  EnumMember,
  Trait,
  Variable,
  Module,
  EffectOperation,
};

struct OutlineItem {
  std::string name;
  OutlineKind kind = OutlineKind::Variable;
  size_t begin = 0, end = 0;          // the whole declaration, in bytes
  size_t name_begin = 0, name_end = 0;
  std::vector<OutlineItem> children;
};

// The module's top-level declarations, with the members of its classes, enums
// and traits: what an editor's outline lists.
inline std::vector<OutlineItem> outline(const peg::Ast& root,
                                        std::string_view source,
                                        const Resolution& res) {
  using namespace peg::udl;
  std::vector<OutlineItem> out;
  auto span_of = [&](const peg::Ast& name_node, std::string_view name,
                     size_t& b, size_t& e) {
    b = name_offset(name_node, name, source);
    e = b + name.size();
    return b != kNone;
  };
  auto item = [&](const peg::Ast& decl, const peg::Ast& name_node,
                  std::string_view name, OutlineKind kind) {
    OutlineItem it;
    it.name = std::string(name);
    it.kind = kind;
    it.begin = decl.position;
    it.end = decl.position + decl.length;
    if (!span_of(name_node, name, it.name_begin, it.name_end)) {
      it.name_begin = it.begin;
      it.name_end = it.begin;
    }
    return it;
  };
  // A top-level variable is listed at the statement that declares it, not at a
  // later assignment to it.
  auto declares = [&](const peg::Ast& name_node, std::string_view name) {
    size_t b = 0, e = 0;
    if (!span_of(name_node, name, b, e)) return false;
    const Occurrence* o = res.occurrence_at(b);
    return o && o->position == b && o->role == Role::Declaration &&
           res.symbols[o->symbol].scope == 0 &&
           res.symbols[o->symbol].occurrences.front() ==
               static_cast<size_t>(o - res.occurrences.data());
  };

  auto visit = [&](const peg::Ast& s) {
    switch (s.tag) {
      case "MULTIFN_DECL"_:
      case "EFFECT_FN_DECL"_:
      case "CLASS_DECL"_:
      case "ENUM_DECL"_:
      case "TRAIT_DECL"_: {
        size_t i = first_non_decorator_index(s);
        if (i >= s.nodes.size()) return;
        const peg::Ast& head = *s.nodes[i];
        auto name = parse_generic_head(head.token).outer;
        OutlineKind kind = s.tag == "MULTIFN_DECL"_    ? OutlineKind::Function
                           : s.tag == "EFFECT_FN_DECL"_ ? OutlineKind::EffectOperation
                           : s.tag == "CLASS_DECL"_     ? OutlineKind::Class
                           : s.tag == "ENUM_DECL"_      ? OutlineKind::Enum
                                                        : OutlineKind::Trait;
        OutlineItem it = item(s, head, name, kind);
        for (size_t j = i + 1; j < s.nodes.size(); j++) {
          const peg::Ast& m = *s.nodes[j];
          if (m.tag == "METHOD"_) {
            auto mv = view_method(m);
            OutlineKind mk = mv.body ? (mv.name == "new" ? OutlineKind::Constructor
                                                         : OutlineKind::Method)
                                     : OutlineKind::Field;
            it.children.push_back(item(m, *m.nodes[1], mv.name, mk));
          } else if (m.tag == "VARIANT"_ && !m.nodes.empty()) {
            it.children.push_back(
                item(m, *m.nodes[0], m.nodes[0]->token, OutlineKind::EnumMember));
          } else if (m.tag == "TRAIT_METHOD"_ && !m.nodes.empty()) {
            it.children.push_back(
                item(m, *m.nodes[0], m.nodes[0]->token, OutlineKind::Method));
          }
        }
        out.push_back(std::move(it));
        return;
      }
      case "IMPORT_STMT"_:
        if (!s.nodes.empty())
          out.push_back(item(s, *s.nodes[0], s.nodes[0]->token, OutlineKind::Module));
        return;
      case "ASSIGNMENT"_: {
        auto av = view_assignment(s);
        const peg::Ast* t = assign_name_target(s, av);
        if (t && !av.compound && declares(*t, t->token))
          out.push_back(item(s, *t, t->token, OutlineKind::Variable));
        return;
      }
      case "DESTRUCTURE_ASSIGN"_: {
        // The pattern's declarations, as the resolver recorded them.
        if (s.nodes.size() < 3) return;
        size_t from = s.nodes[2]->position, to = from + s.nodes[2]->length;
        auto it = std::lower_bound(
            res.occurrences.begin(), res.occurrences.end(), from,
            [](const Occurrence& o, size_t p) { return o.position < p; });
        for (; it != res.occurrences.end() && it->position < to; ++it) {
          if (it->role != Role::Declaration) continue;
          OutlineItem item;
          item.name = res.symbols[it->symbol].name;
          item.kind = OutlineKind::Variable;
          item.begin = s.position;
          item.end = s.position + s.length;
          item.name_begin = it->position;
          item.name_end = it->position + it->length;
          out.push_back(std::move(item));
        }
        return;
      }
      default:
        return;
    }
  };
  if (root.tag == "STATEMENTS"_) {
    for (const auto& c : root.nodes) visit(*c);
  } else {
    visit(root);
  }
  return out;
}

}  // namespace culebra::resolve
