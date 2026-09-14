#pragma once

// Static name resolution over the source as written: which declaration each
// identifier refers to. An editor's go-to-definition, find-references,
// highlight and rename read it, so it follows the compiler's scoping
// (vm::Compiler) rather than an approximation of it:
//
//  - Scopes: the module; each function-like body (`fn`, a lambda, `fn name`, a
//    method or field initializer, `defer`, `effect fn`, a handler clause); a
//    `{}` block; a loop body; a match arm; a try body and its catch; the block
//    `handle` runs; and the init clause around its `if` / `while` / `match`. An
//    `if` arm shares the enclosing scope.
//  - Within one function a declaration is visible from its statement on, after
//    its right-hand side (`let x = x` reads the outer `x`). Every declaration of
//    one name in one scope is the same variable: a repeated `let`, a bare
//    `x = v`, the overloads of `fn name`.
//  - A bare `x = v` (or a bare destructure) writes the `x` visible there, or
//    declares one in the innermost scope when none is.
//  - A function body sees an enclosing function's variables wherever they are
//    declared, since a closure captures the variable itself; so each function
//    body is resolved after the whole body that encloses it.
//
// A name nothing here declares — a stdlib global, `self`, a NameError at run
// time — resolves to no symbol. A member (`o.x`) and an object key are not
// names.

#include "frontend/parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
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
};

struct Scope {
  size_t parent = kNone;
  bool function = false;
  std::map<std::string, size_t, std::less<>> names;  // name -> symbol
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
  // Names called as a method (`v.name(...)`). A free function of that name may
  // be what the call reaches (UFCS), which no static pass can decide.
  std::vector<std::string> method_call_names;  // sorted, unique

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

  bool called_as_method(std::string_view name) const {
    return std::binary_search(method_call_names.begin(),
                              method_call_names.end(), name,
                              std::less<>());
  }
};

namespace _detail {

using namespace peg::udl;

class Resolver {
 public:
  Resolver(const peg::Ast& root, std::string_view source)
      : root_(root), src_(source) {}

  Resolution run() {
    cur_ = push_scope(kNone, /*function=*/true);
    walk_body(root_);
    for (const auto* e : exports_) {
      read(*e, e->token, Spelling::Plain);
      size_t sym = r_.lookup(0, e->token);
      if (sym != kNone) r_.symbols[sym].exported = true;
    }
    while (!jobs_.empty()) {
      Job job = jobs_.front();
      jobs_.pop_front();
      run_job(job);
    }
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
  };
  struct Label {
    size_t callee;
    const peg::Ast* node;
    size_t scope;
  };
  enum class Bind { Declare, Parameter, Bare };

  // ---- bookkeeping ----------------------------------------------------------

  size_t push_scope(size_t parent, bool function) {
    Scope s;
    s.parent = parent;
    s.function = function;
    r_.scopes.push_back(std::move(s));
    return r_.scopes.size() - 1;
  }

  // Byte offset of a name node's text, or kNone for a node the parse
  // synthesized (a desugaring) whose text is not in the source.
  size_t offset_of(const peg::Ast& n, std::string_view name) const {
    const char* p = n.is_token && !n.token.empty() ? n.token.data()
                                                   : src_.data() + n.position;
    if (p < src_.data() || p + name.size() > src_.data() + src_.size())
      return kNone;
    size_t off = static_cast<size_t>(p - src_.data());
    return src_.substr(off, name.size()) == name ? off : kNone;
  }

  void add(const peg::Ast& n, std::string_view name, size_t sym, size_t scope,
           Role role, Spelling spelling) {
    size_t off = offset_of(n, name);
    if (off == kNone) return;
    r_.occurrences.push_back({off, name.size(), sym, scope, role, spelling});
  }

  static bool implicit(std::string_view name) {
    return name == "_" || is_always_bound_name(name);
  }

  size_t declare(const peg::Ast& n, std::string_view name, SymbolKind kind,
                 Spelling spelling = Spelling::Plain) {
    if (implicit(name)) return kNone;
    auto& names = r_.scopes[cur_].names;
    size_t sym;
    if (auto it = names.find(name); it != names.end()) {
      sym = it->second;
    } else {
      sym = r_.symbols.size();
      Symbol s;
      s.name = std::string(name);
      s.kind = kind;
      s.scope = cur_;
      r_.symbols.push_back(std::move(s));
      names.emplace(std::string(name), sym);
    }
    add(n, name, sym, cur_, Role::Declaration, spelling);
    return sym;
  }

  size_t read(const peg::Ast& n, std::string_view name, Spelling spelling) {
    if (implicit(name)) return kNone;
    size_t sym = r_.lookup(cur_, name);
    if (sym == kNone) {
      size_t off = offset_of(n, name);
      if (off != kNone) r_.unresolved.push_back({std::string(name), off, cur_});
      return kNone;
    }
    add(n, name, sym, cur_, Role::Read, spelling);
    return sym;
  }

  // `x = v`: the visible `x`, else a new one here.
  size_t bare_write(const peg::Ast& n, std::string_view name,
                    Spelling spelling = Spelling::Plain) {
    if (implicit(name)) return kNone;
    size_t sym = r_.lookup(cur_, name);
    if (sym == kNone) return declare(n, name, SymbolKind::Variable, spelling);
    add(n, name, sym, cur_, Role::Write, spelling);
    return sym;
  }

  void enqueue(const peg::Ast* params, const peg::Ast* body,
               size_t owner = kNone) {
    if (body) jobs_.push_back({params, body, cur_, owner});
  }

  void finish() {
    std::stable_sort(r_.occurrences.begin(), r_.occurrences.end(),
                     [](const Occurrence& a, const Occurrence& b) {
                       return a.position < b.position;
                     });
    for (size_t i = 0; i < r_.occurrences.size(); i++)
      r_.symbols[r_.occurrences[i].symbol].occurrences.push_back(i);
    auto& m = r_.method_call_names;
    std::sort(m.begin(), m.end());
    m.erase(std::unique(m.begin(), m.end()), m.end());
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

  void scoped_body(const peg::Ast& n) {
    size_t saved = cur_;
    cur_ = push_scope(cur_, false);
    walk_body(n);
    cur_ = saved;
  }

  void run_job(const Job& job) {
    size_t saved = cur_;
    cur_ = push_scope(job.parent, true);
    if (job.params) {
      for (const auto& p : job.params->nodes) {
        if (is_kw_only_sep(*p)) continue;
        if (is_pattern_param(*p)) {
          bind_pattern(*p, Bind::Parameter);
          continue;
        }
        // A default sees the parameters before it, not its own.
        if (const auto* d = extract_default_expr(*p)) walk(*d);
        const peg::Ast& name_node =
            (is_kwargs_rest(*p) || is_args_rest(*p)) ? *p : *p->nodes[1];
        auto loc = extract_param_name_loc(*p);
        size_t sym = declare(name_node, loc.name, SymbolKind::Parameter);
        if (sym != kNone && job.owner != kNone)
          params_of_[job.owner].push_back(sym);
      }
    }
    walk_body(*job.body);
    cur_ = saved;
  }

  void bind_pattern(const peg::Ast& pat, Bind mode) {
    switch (pat.tag) {
      case "PATTERN"_:
      case "ARRAY_PATTERN"_:
      case "TUPLE_PATTERN"_:
      case "FOR_BINDING"_:
        for (const auto& c : pat.nodes) bind_pattern(*c, mode);
        return;
      case "IDENTIFIER"_:
        bind(pat, pat.token, mode,
             pat.original_tag == "OBJECT_PAT_ENTRY"_ ? Spelling::PatternShorthand
                                                     : Spelling::Plain);
        return;
      case "TYPED_IDENT"_:
        bind(*pat.nodes[0], pat.nodes[0]->token, mode, Spelling::Plain);
        return;
      case "REST_PATTERN"_:
        if (!pat.nodes.empty())
          bind(*pat.nodes[0], pat.nodes[0]->token, mode, Spelling::Plain);
        return;
      case "CTOR_PATTERN"_:
        for (size_t i = 1; i < pat.nodes.size(); i++)
          bind_pattern(*pat.nodes[i], mode);
        return;
      case "OBJECT_PATTERN"_:
        for (const auto& e : pat.nodes) {
          if (e->tag == "OBJECT_PAT_ENTRY"_ && e->nodes.size() >= 2)
            bind_pattern(*e->nodes[1], mode);
          else if (e->tag == "IDENTIFIER"_)
            bind(*e, e->token, mode, Spelling::PatternShorthand);
        }
        return;
      default:
        return;  // a literal, a wildcard: binds nothing
    }
  }

  void bind(const peg::Ast& n, std::string_view name, Bind mode,
            Spelling spelling) {
    switch (mode) {
      case Bind::Declare:
        declare(n, name, SymbolKind::Variable, spelling);
        return;
      case Bind::Parameter:
        declare(n, name, SymbolKind::Parameter, spelling);
        return;
      case Bind::Bare:
        bare_write(n, name, spelling);
        return;
    }
  }

  static bool is_function_literal(const peg::Ast& n) {
    return n.tag == "FUNCTION"_ || n.tag == "LAMBDA"_;
  }

  void enqueue_literal(const peg::Ast& fn, size_t owner) {
    if (fn.tag == "FUNCTION"_) {
      auto fv = view_function(fn);
      enqueue(fv.params, fv.body.get(), owner);
    } else {
      auto lv = view_lambda(fn);
      enqueue(lv.params, lv.body.get(), owner);
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
        size_t off = offset_of(*target, name);
        if (off != kNone) r_.unresolved.push_back({std::string(name), off, cur_});
      } else {
        add(*target, name, sym, cur_, Role::Write, Spelling::Plain);
      }
      return;
    }
    // A function literal is bound to the name it is assigned to; its body runs
    // later, so binding first changes nothing it reads.
    if (is_function_literal(*av.rhs)) {
      size_t sym = (av.is_let || av.is_mut)
                       ? declare(*target, name, SymbolKind::Variable)
                       : bare_write(*target, name);
      enqueue_literal(*av.rhs, sym);
      return;
    }
    walk(*av.rhs);
    if (av.is_let || av.is_mut)
      declare(*target, name, SymbolKind::Variable);
    else
      bare_write(*target, name);
  }

  void walk_call(const peg::Ast& n) {
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
          r_.method_call_names.emplace_back(c.token);
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

  void walk_decl_head(const peg::Ast& n, SymbolKind kind, size_t* sym_out) {
    size_t i = first_non_decorator_index(n);
    if (i >= n.nodes.size()) return;
    const peg::Ast& head = *n.nodes[i];
    auto name = parse_generic_head(head.token).outer;
    size_t sym = declare(head, name, kind);
    if (sym_out) *sym_out = sym;
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
        bool declared =
            n.nodes[0]->token == "let" || n.nodes[1]->token == "mut";
        bind_pattern(*n.nodes[2], declared ? Bind::Declare : Bind::Bare);
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
        walk_decl_head(n, SymbolKind::Function, &sym);
        size_t i = first_non_decorator_index(n);
        // [DECORATOR*, head, PARAMETERS, (RETURN_TYPE)?, body]
        if (i + 3 <= n.nodes.size())
          enqueue(n.nodes[i + 1].get(), n.nodes.back().get(), sym);
        return;
      }

      case "EFFECT_FN_DECL"_: {
        size_t sym = kNone;
        walk_decl_head(n, SymbolKind::EffectOperation, &sym);
        if (effect_fn_has_body(n) && n.nodes.size() >= 3)
          enqueue(n.nodes[1].get(), n.nodes.back().get(), sym);
        return;
      }

      case "CLASS_DECL"_: {
        walk_decl_head(n, SymbolKind::Class, nullptr);
        size_t i = first_non_decorator_index(n);
        for (size_t j = i + 1; j < n.nodes.size(); j++) {
          if (n.nodes[j]->tag != "METHOD"_) continue;
          auto mv = view_method(*n.nodes[j]);
          if (mv.body)
            enqueue(mv.params, mv.body->get());
          else if (mv.value)
            enqueue(nullptr, mv.value);
        }
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
        walk_decl_head(n, SymbolKind::Enum, nullptr);
        return;

      case "IMPORT_STMT"_:
        if (!n.nodes.empty() && n.nodes[0]->is_token) {
          size_t sym = declare(*n.nodes[0], n.nodes[0]->token, SymbolKind::Import);
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
        enqueue_literal(n, kNone);
        return;

      case "DEFER"_:
        if (!n.nodes.empty()) enqueue(nullptr, n.nodes[0].get());
        return;

      case "DECORATOR"_:
        return;  // a decorator's callee and trait names are not variable reads

      case "LEXICAL_SCOPE"_: {
        size_t saved = cur_;
        cur_ = push_scope(cur_, false);
        for (const auto& c : n.nodes) walk_body(*c);
        cur_ = saved;
        return;
      }

      case "FOR"_: {
        if (n.nodes.size() < 3) break;
        auto fv = view_for(n);
        walk(*fv.iter);
        size_t saved = cur_;
        cur_ = push_scope(cur_, false);
        bind_pattern(*fv.binding, Bind::Declare);
        walk_body(*fv.body);
        cur_ = saved;
        if (fv.nobreak) scoped_body(*fv.nobreak);
        return;
      }

      case "WHILE"_: {
        if (n.nodes.size() < 2) break;
        auto wv = view_while(n);
        size_t saved = cur_;
        if (wv.init) {
          cur_ = push_scope(cur_, false);
          for (const auto& b : wv.init->nodes) walk(*b);
        }
        walk(*wv.cond);
        scoped_body(*wv.body);
        if (wv.nobreak) scoped_body(*wv.nobreak);
        cur_ = saved;
        return;
      }

      case "IF"_: {
        auto iv = view_if(n);
        size_t saved = cur_;
        if (iv.init) {
          cur_ = push_scope(cur_, false);
          for (const auto& b : iv.init->nodes) walk(*b);
        }
        for (size_t i = iv.arm_off; i < n.nodes.size(); i++)
          walk_body(*n.nodes[i]);
        cur_ = saved;
        return;
      }

      case "MATCH"_: {
        if (n.nodes.size() < 2) break;
        auto mv = view_match(n);
        size_t saved = cur_;
        if (mv.init) {
          cur_ = push_scope(cur_, false);
          for (const auto& b : mv.init->nodes) walk(*b);
        }
        walk(*mv.subject);
        size_t around = cur_;
        for (const auto& arm : mv.arms->nodes) {
          if (arm->nodes.empty()) continue;
          cur_ = push_scope(around, false);
          bind_pattern(*arm->nodes[0], Bind::Declare);
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
        cur_ = push_scope(cur_, false);
        if (n.nodes[1]->is_token)
          declare(*n.nodes[1], n.nodes[1]->token, SymbolKind::Variable);
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
inline Resolution resolve_module(const peg::Ast& root, std::string_view source) {
  return _detail::Resolver(root, source).run();
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
    const char* p = name_node.is_token && !name_node.token.empty()
                        ? name_node.token.data()
                        : source.data() + name_node.position;
    if (p < source.data() || p + name.size() > source.data() + source.size())
      return false;
    b = static_cast<size_t>(p - source.data());
    e = b + name.size();
    return true;
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
      case "DESTRUCTURE_ASSIGN"_:
        if (s.nodes.size() >= 3)
          for_each_pattern_binding(
              *s.nodes[2], [&](std::string_view name, size_t, size_t) {
                // Find the binding's node by its text inside the pattern.
                size_t from = s.nodes[2]->position;
                size_t to = from + s.nodes[2]->length;
                size_t at = source.substr(0, to).find(name, from);
                if (at == std::string_view::npos) return;
                const Occurrence* o = res.occurrence_at(at);
                if (!o || o->role != Role::Declaration) return;
                OutlineItem it;
                it.name = std::string(name);
                it.kind = OutlineKind::Variable;
                it.begin = s.position;
                it.end = s.position + s.length;
                it.name_begin = o->position;
                it.name_end = o->position + o->length;
                out.push_back(std::move(it));
              });
        return;
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
