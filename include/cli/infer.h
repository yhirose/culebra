#pragma once

// Type inference for an editor, over the source as written: enough of an
// expression's type to list what `value.` completes to and to show a name's
// type on hover. It establishes types and never guesses — what it cannot
// establish is unknown — and nothing here rejects a program.
//
// It knows literals; the operators whose result does not depend on
// overloading; the built-in methods' return types (stdlib/canon_sigs.h) and
// the stdlib namespaces a Catalog supplies; a class's instance (`C()`,
// `C.new()`), whose members are its methods, typed fields and every
// `self.x = ...` its methods write; an object literal's keys; a function's
// return type, from its annotation or what it returns; a variable's type,
// joined over its assignments; a parameter's annotation; and a loop
// variable's element type.

#include <frontend/parser.h>
#include <frontend/resolve.h>
#include <stdlib/canon_sigs.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culebra::infer {

enum class Kind : uint8_t {
  Nil,
  Bool,
  Long,
  Float,
  String,
  Array,
  Object,
  Function,
  Tensor,
  Tuple,
  Set,
  Iterator,
  Instance,   // of a class
  Class,      // a class or enum value itself
  Namespace,  // a stdlib namespace, by dotted path
  Module,     // what an import names
};

class Inference;
struct Type;
using TypeRef = std::shared_ptr<const Type>;

struct Field {
  std::string name;
  TypeRef type;
};

struct Alt {
  Kind kind = Kind::Nil;
  std::string name;  // Instance / Class: the class; Namespace: its path; Module: the import's path
  const peg::Ast* decl = nullptr;      // Instance / Class: CLASS_DECL or ENUM_DECL
  const peg::Ast* function = nullptr;  // Function: FUNCTION, LAMBDA, MULTIFN_DECL or METHOD
  const Inference* owner = nullptr;    // the inference that can read decl / function
  TypeRef element;                     // Array / Set / Iterator
  std::vector<Field> fields;           // Object: what a literal gave it
};

struct Type {
  std::vector<Alt> alts;  // empty: unknown

  bool unknown() const { return alts.empty(); }
  static Type of(Kind k) {
    Type t;
    t.alts.push_back(Alt{k});
    return t;
  }
  static Type single(Alt a) {
    Type t;
    t.alts.push_back(std::move(a));
    return t;
  }
  std::string to_string() const;
};

inline void add_alt(Type& t, Alt a) {
  for (auto& x : t.alts) {
    if (x.kind != a.kind || x.name != a.name || x.decl != a.decl ||
        x.function != a.function)
      continue;
    if (!x.element && a.element) x.element = a.element;
    for (auto& f : a.fields) {
      bool have = std::any_of(x.fields.begin(), x.fields.end(),
                              [&](const Field& g) { return g.name == f.name; });
      if (!have) x.fields.push_back(f);
    }
    return;
  }
  t.alts.push_back(std::move(a));
}

inline Type join(Type a, const Type& b) {
  for (const auto& x : b.alts) add_alt(a, x);
  return a;
}

inline Type without_nil(Type t) {
  std::erase_if(t.alts, [](const Alt& a) { return a.kind == Kind::Nil; });
  return t;
}

inline TypeRef ref(Type t) {
  return t.unknown() ? nullptr : std::make_shared<const Type>(std::move(t));
}

inline std::string_view kind_name(Kind k) {
  switch (k) {
    case Kind::Nil: return "Nil";
    case Kind::Bool: return "Bool";
    case Kind::Long: return "Long";
    case Kind::Float: return "Float";
    case Kind::String: return "String";
    case Kind::Array: return "Array";
    case Kind::Object: return "Object";
    case Kind::Function: return "Function";
    case Kind::Tensor: return "Tensor";
    case Kind::Tuple: return "Tuple";
    case Kind::Set: return "Set";
    case Kind::Iterator: return "Iterator";
    case Kind::Instance: return "Object";
    case Kind::Class: return "Class";
    case Kind::Namespace: return "Namespace";
    case Kind::Module: return "Module";
  }
  return "";
}

inline std::string Type::to_string() const {
  std::string out;
  for (const auto& a : alts) {
    if (!out.empty()) out += " | ";
    switch (a.kind) {
      case Kind::Instance:
      case Kind::Namespace:
        out += a.name;
        break;
      case Kind::Class:
        out += "class " + a.name;
        break;
      case Kind::Array:
      case Kind::Set:
      case Kind::Iterator:
        out += kind_name(a.kind);
        if (a.element) out += "<" + a.element->to_string() + ">";
        break;
      default:
        out += kind_name(a.kind);
    }
  }
  return out;
}

// ---- type text ----------------------------------------------------------------

// A class a type's text may name, or nothing.
using ClassLookup = std::function<std::optional<Alt>(std::string_view)>;

// A type annotation or a declared return type: `Long`, `Array<Long>`,
// `Long | Nil`, `T?`, `fn(Long) -> Long`. A name it does not know adds nothing.
inline Type parse_type(std::string_view text, const ClassLookup& classes = {}) {
  Type t;
  std::vector<std::string_view> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '<' || c == '(') depth++;
    if (c == '>' || c == ')') depth--;
    if (c == '|' && depth == 0) {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(text.substr(start));
  for (auto part : parts) {
    part = trim_ascii(part);
    if (part.empty()) continue;
    if (part.starts_with("fn")) {
      add_alt(t, Alt{Kind::Function});
      continue;
    }
    if (part.back() == '?') {
      add_alt(t, Alt{Kind::Nil});
      part.remove_suffix(1);
    }
    std::string_view name = part, arg;
    if (auto lt = part.find('<'); lt != std::string_view::npos && part.back() == '>') {
      name = trim_ascii(part.substr(0, lt));
      arg = part.substr(lt + 1, part.size() - lt - 2);
      int d = 0;
      for (size_t i = 0; i < arg.size(); i++) {
        if (arg[i] == '<') d++;
        if (arg[i] == '>') d--;
        if (arg[i] == ',' && d == 0) {
          arg = arg.substr(0, i);
          break;
        }
      }
    }
    static const std::map<std::string_view, Kind> kNames = {
        {"Nil", Kind::Nil},       {"Bool", Kind::Bool},
        {"Long", Kind::Long},     {"Float", Kind::Float},
        {"String", Kind::String}, {"StringView", Kind::String},
        {"Array", Kind::Array},   {"Object", Kind::Object},
        {"Function", Kind::Function}, {"Tensor", Kind::Tensor},
        {"Tuple", Kind::Tuple},   {"Set", Kind::Set},
        {"Iterator", Kind::Iterator},
    };
    if (name == "Range") {
      Alt a{Kind::Iterator};
      a.element = ref(Type::of(Kind::Long));
      add_alt(t, std::move(a));
    } else if (auto it = kNames.find(name); it != kNames.end()) {
      Alt a{it->second};
      if (!arg.empty()) a.element = ref(parse_type(arg, classes));
      add_alt(t, std::move(a));
    } else if (classes) {
      if (auto a = classes(name)) add_alt(t, std::move(*a));
    }
  }
  return t;
}

// ---- members --------------------------------------------------------------

enum class MemberKind : uint8_t {
  Method,
  Field,
  Function,
  Constructor,
  Class,
  Enum,
  EnumMember,
  Namespace,
  Constant,
  Variable,
  Parameter,
  Module,
};

struct Member {
  std::string name;
  MemberKind kind = MemberKind::Function;
  std::string detail;  // a signature or a type, to display
  // A function or method: what calling it gives. Anything else: its type.
  // Left empty when `owner` reads it on demand (Inference::member_type).
  Type type;
  const Inference* owner = nullptr;
  const peg::Ast* function = nullptr;     // a method: its METHOD node
  const peg::Ast* value = nullptr;        // a static field: its initializer
  const peg::Ast* field_class = nullptr;  // a field `self.name = ...` sets
};

// What the host knows beyond the document: the stdlib's namespaces and globals.
class Catalog {
 public:
  virtual ~Catalog() = default;
  // A namespace's members ("Math", "Encoding.html", "Regex"), or nullptr
  // when `path` names no namespace.
  virtual const std::vector<Member>* namespace_members(
      std::string_view path) const = 0;
  // The global functions and namespaces a bare name can be.
  virtual const std::vector<Member>& globals() const = 0;
};

inline std::string canon_signature(const CanonSig& s) {
  std::string out(s.name);
  out += '(';
  bool kw_marked = false;
  for (int i = 0; i < s.n_params; i++) {
    const CanonParam& p = s.params[i];
    if (i) out += ", ";
    if (p.kw_only && !kw_marked) {
      out += "*, ";
      kw_marked = true;
    }
    if (p.args_rest) out += "*";
    if (p.kwargs_rest) out += "**";
    out += p.name;
    if (!p.type.empty()) {
      out += ": ";
      out += p.type;
    }
    switch (p.default_kind) {
      case CanonDefault::None: break;
      case CanonDefault::Nil: out += " = nil"; break;
      case CanonDefault::Bool: out += p.default_bits ? " = true" : " = false"; break;
      case CanonDefault::Long: out += " = " + std::to_string(p.default_bits); break;
      case CanonDefault::Float:
        out += " = " + std::format("{}", std::bit_cast<double>(p.default_bits));
        break;
      case CanonDefault::Str:
        out += " = \"" + std::string(p.default_str) + "\"";
        break;
    }
  }
  out += ')';
  if (!s.return_type.empty()) {
    out += " -> ";
    out += s.return_type;
  }
  return out;
}

// A built-in value type's methods, from the canonical signature table.
inline const std::vector<Member>& value_members(Kind k) {
  static const auto tables = [] {
    std::map<Kind, std::vector<Member>> m;
    auto fill = [&](Kind kind, std::span<const CanonSig> sigs) {
      auto& v = m[kind];
      for (const auto& s : sigs) {
        if (s.name.starts_with("_")) continue;
        bool have = std::any_of(v.begin(), v.end(), [&](const Member& x) {
          return x.name == s.name;
        });
        if (have) continue;  // one row per arity
        v.push_back({std::string(s.name), MemberKind::Method,
                     canon_signature(s), parse_type(s.return_type)});
      }
    };
    fill(Kind::String, kCanonStringSigs);
    fill(Kind::Array, kCanonArraySigs);
    fill(Kind::Object, kCanonObjectSigs);
    fill(Kind::Set, kCanonSetSigs);
    fill(Kind::Tuple, kCanonTupleSigs);
    fill(Kind::Tensor, kCanonTensorSigs);
    fill(Kind::Iterator, kCanonIteratorSigs);
    return m;
  }();
  static const std::vector<Member> kEmpty;
  auto it = tables.find(k);
  return it == tables.end() ? kEmpty : it->second;
}

// ---- callbacks ------------------------------------------------------------
//
// The built-in methods that take a function (map, filter, reduce, ...) pass it
// the receiver's elements, and what they return depends on the function. The
// canonical signature table declares that parameter as a plain `Function` —
// a string the runtime also reads — so the element flow lives here, for
// inference only.

enum class CallbackResult : uint8_t {
  Same,        // the receiver: filter, sorted_by, take_while, tap
  Map,         // the receiver's kind, of what the function returns
  FlatMap,     // the receiver's kind, of the elements of what it returns
  Element,     // one element or nil: find, min_by, max_by
  Fold,        // the initial value or what the function returns: reduce
  Bool,        // all, any
  Nil,         // for_each, sort_by
  Object,      // group_by
  Tuple,       // partition
  Position,    // an index or nil
  Iterator,    // an iterator of something else: chunk_by, scan
};

struct CallbackRule {
  std::string_view method;
  int callback_arg;   // the positional argument that is the function
  int element_param;  // the function's parameter that receives an element
  CallbackResult result;
};

inline const CallbackRule* callback_rule(std::string_view method) {
  static constexpr CallbackRule kRules[] = {
      {"all", 0, 0, CallbackResult::Bool},
      {"any", 0, 0, CallbackResult::Bool},
      {"chunk_by", 0, 0, CallbackResult::Iterator},
      {"filter", 0, 0, CallbackResult::Same},
      {"find", 0, 0, CallbackResult::Element},
      {"flat_map", 0, 0, CallbackResult::FlatMap},
      {"for_each", 0, 0, CallbackResult::Nil},
      {"group_by", 0, 0, CallbackResult::Object},
      {"map", 0, 0, CallbackResult::Map},
      {"max_by", 0, 0, CallbackResult::Element},
      {"min_by", 0, 0, CallbackResult::Element},
      {"partition", 0, 0, CallbackResult::Tuple},
      {"position", 0, 0, CallbackResult::Position},
      {"reduce", 1, 1, CallbackResult::Fold},
      {"scan", 1, 1, CallbackResult::Iterator},
      {"skip_while", 0, 0, CallbackResult::Same},
      {"sort_by", 0, 0, CallbackResult::Nil},
      {"sorted_by", 0, 0, CallbackResult::Same},
      {"take_while", 0, 0, CallbackResult::Same},
      {"tap", 0, 0, CallbackResult::Same},
  };
  for (const auto& r : kRules)
    if (r.method == method) return &r;
  return nullptr;
}

// The positional arguments of an ARGUMENTS node, keyword arguments left out.
inline std::vector<const peg::Ast*> positional_args(const peg::Ast& args) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  for (const auto& a : args.nodes)
    if (a->tag != "KWARG"_ && a->tag != "KWARG_SPLAT"_) out.push_back(a.get());
  return out;
}

// ---- inference ------------------------------------------------------------

class Inference {
 public:
  // What an import names: the imported module's top-level declarations. Empty
  // when the host does not read other files.
  using ModuleMembers =
      std::function<std::vector<Member>(std::string_view import_path)>;

  Inference(const peg::Ast& root, std::string_view source,
            const resolve::Resolution& res, const Catalog* catalog = nullptr,
            ModuleMembers modules = {})
      : root_(root),
        src_(source),
        res_(res),
        catalog_(catalog),
        modules_(std::move(modules)) {
    index(root_, nullptr);
    bind_calls();
  }

  // ---- queries --------------------------------------------------------------

  Type expr_type(const peg::Ast& e) const {
    using namespace peg::udl;
    switch (e.tag) {
      case "NUMBER"_:
        return Type::of(Kind::Long);
      case "FLOAT"_:
        return Type::of(Kind::Float);
      case "STRING"_:
      case "RAW_STRING"_:
      case "INTERPOLATED_STRING"_:
      case "TRIPLE_STRING"_:
        return Type::of(Kind::String);
      case "BOOLEAN"_:
        return Type::of(Kind::Bool);
      case "NIL"_:
        return Type::of(Kind::Nil);
      case "ARRAY"_:
      case "SET"_: {
        Alt a{e.tag == "ARRAY"_ ? Kind::Array : Kind::Set};
        Type element;
        for (const auto* item : sequence(e)) {
          if (item->tag == "SPREAD_ELEM"_ && !item->nodes.empty())
            element = join(element, element_of(expr_type(*item->nodes[0])));
          else
            element = join(element, expr_type(*item));
        }
        a.element = ref(std::move(element));
        return Type::single(std::move(a));
      }
      case "TUPLE"_:
        return Type::of(Kind::Tuple);
      case "OBJECT"_: {
        Alt a{Kind::Object};
        for (const auto& p : e.nodes) {
          if (p->tag != "OBJECT_PROPERTY"_) continue;
          auto pv = view_object_property(*p);
          std::string key(pv.key->token);
          if (key.size() >= 2 && (key.front() == '"' || key.front() == '\''))
            key = key.substr(1, key.size() - 2);
          if (pv.key->tag != "IDENTIFIER"_ && pv.key->tag != "STRING"_) continue;
          a.fields.push_back({key, ref(expr_type(*pv.value))});
        }
        return Type::single(std::move(a));
      }
      case "FUNCTION"_:
      case "LAMBDA"_: {
        Alt a{Kind::Function};
        a.function = &e;
        a.owner = this;
        return Type::single(std::move(a));
      }
      case "RANGE"_: {
        Alt a{Kind::Iterator};
        a.element = ref(Type::of(Kind::Long));
        return Type::single(std::move(a));
      }
      case "IDENTIFIER"_: {
        if (e.original_tag == "DOT"_ || e.original_tag == "SAFE_DOT"_) return {};
        if (e.token == "self") return self_type(e);
        size_t s = symbol_at(e);
        if (s != resolve::kNone) return symbol_type(s);
        if (catalog_ && catalog_->namespace_members(e.token))
          return Type::single(Alt{Kind::Namespace, std::string(e.token)});
        if (global_function(e.token)) {
          Alt a{Kind::Function};
          a.name = std::string(e.token);
          return Type::single(std::move(a));
        }
        return {};
      }
      case "CALL"_:
        return chain_type(e, e.nodes.size());
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_:
        return arithmetic(e);
      case "CONDITION"_:
        return Type::of(Kind::Bool);
      case "UNARY_NOT"_:
        return e.nodes.size() >= 2 ? Type::of(Kind::Bool) : Type{};
      case "UNARY_MINUS"_:
      case "UNARY_PLUS"_:
        return e.nodes.empty() ? Type{} : expr_type(*e.nodes.back());
      case "LOGICAL_AND"_:
      case "LOGICAL_OR"_: {
        Type t;
        for (const auto& c : e.nodes) t = join(t, expr_type(*c));
        return t;
      }
      case "NIL_COALESCE"_: {
        if (e.nodes.empty()) return {};
        Type t = without_nil(expr_type(*e.nodes[0]));
        for (size_t i = 1; i < e.nodes.size(); i++)
          t = join(t, expr_type(*e.nodes[i]));
        return t;
      }
      case "CONDITIONAL"_:
        return e.nodes.size() == 3
                   ? join(expr_type(*e.nodes[1]), expr_type(*e.nodes[2]))
                   : Type{};
      case "IF"_: {
        auto iv = view_if(e);
        Type t;
        size_t i = iv.arm_off;
        for (; i + 1 < e.nodes.size(); i += 2) t = join(t, tail_type(*e.nodes[i + 1]));
        if (i < e.nodes.size())
          t = join(t, tail_type(*e.nodes[i]));
        else
          t = join(t, Type::of(Kind::Nil));
        return t;
      }
      case "MATCH"_: {
        if (e.nodes.size() < 2) return {};
        auto mv = view_match(e);
        Type t;
        for (const auto& arm : mv.arms->nodes)
          if (arm->nodes.size() >= 2) t = join(t, tail_type(*arm->nodes.back()));
        return t;
      }
      case "TRY"_:
        return e.nodes.size() >= 3
                   ? join(tail_type(*e.nodes[0]), tail_type(*e.nodes[2]))
                   : Type{};
      case "STATEMENTS"_:
      case "LEXICAL_SCOPE"_:
        return tail_type(e);
      default:
        return {};
    }
  }

  Type symbol_type(size_t s) const {
    if (auto it = symbol_memo_.find(s); it != symbol_memo_.end()) return it->second;
    if (!symbol_busy_.insert(s).second) return {};  // a cycle: unknown
    const resolve::Symbol& sym = res_.symbols[s];
    Type t;
    switch (sym.kind) {
      case resolve::SymbolKind::Function:
      case resolve::SymbolKind::EffectOperation: {
        Alt a{Kind::Function};
        a.name = sym.name;
        if (auto it = functions_.find(s); it != functions_.end())
          a.function = it->second.front();
        a.owner = this;
        t = Type::single(std::move(a));
        break;
      }
      case resolve::SymbolKind::Class:
      case resolve::SymbolKind::Enum: {
        Alt a{Kind::Class};
        a.name = sym.name;
        if (auto it = types_.find(s); it != types_.end()) a.decl = it->second;
        a.owner = this;
        t = Type::single(std::move(a));
        break;
      }
      case resolve::SymbolKind::Import: {
        Alt a{Kind::Module};
        for (const auto& imp : res_.imports)
          if (imp.symbol == s) a.name = imp.path;
        t = Type::single(std::move(a));
        break;
      }
      default: {
        auto it = sources_.find(s);
        if (it == sources_.end()) break;
        // An annotation states the type; assigned values stand in only when
        // there is none.
        bool annotated = false;
        for (const auto& src : it->second)
          if (src.kind == Source::Annotation) {
            t = join(t, parse_type(src.text, class_lookup()));
            annotated = true;
          }
        if (annotated) break;
        for (const auto& src : it->second) {
          if (src.kind == Source::Value) t = join(t, expr_type(*src.node));
          if (src.kind == Source::ElementOf)
            t = join(t, element_of(expr_type(*src.node)));
          if (src.kind == Source::CallbackElement)
            t = join(t, element_of(chain_type(*src.node, src.index)));
          if (src.kind == Source::Argument || src.kind == Source::UfcsReceiver) {
            if (escapes(src.function) || !reaches_free_function(src)) continue;
            t = join(t, src.kind == Source::Argument
                            ? expr_type(*src.node)
                            : chain_type(*src.call, src.index));
          }
        }
      }
    }
    symbol_busy_.erase(s);
    symbol_memo_[s] = t;
    return t;
  }

  // The type of a CALL chain's first `upto` children: the receiver of the
  // member at `upto`.
  Type chain_type(const peg::Ast& call, size_t upto) const {
    using namespace peg::udl;
    if (call.nodes.empty()) return {};
    const peg::Ast& head = *call.nodes[0];
    Type t = expr_type(head);
    upto = std::min(upto, call.nodes.size());
    for (size_t i = 1; i < upto; i++) {
      const peg::Ast& c = *call.nodes[i];
      bool member = c.tag == "IDENTIFIER"_ &&
                    (c.original_tag == "DOT"_ || c.original_tag == "SAFE_DOT"_);
      if (member) {
        bool called = i + 1 < upto && call.nodes[i + 1]->original_tag == "ARGUMENTS"_;
        if (called) {
          t = method_result(t, c.token, c.position, call.nodes[i + 1].get());
          i++;
        } else {
          t = property_type(t, c.token);
        }
        continue;
      }
      if (c.original_tag == "ARGUMENTS"_) {
        t = call_result(t);
      } else if (c.original_tag == "INDEX"_ || c.original_tag == "SAFE_INDEX"_ ||
                 c.tag == "INDEX"_ || c.tag == "SAFE_INDEX"_) {
        t = index_type(t);
      } else if (c.tag == "NONNULL"_) {
        t = without_nil(t);
      } else {
        t = {};
      }
    }
    return t;
  }

  // What calling a function gives: its annotation, or what it returns (an
  // iterator of what it yields, for a generator).
  Type return_type(const peg::Ast& fn) const {
    using namespace peg::udl;
    if (auto it = return_memo_.find(&fn); it != return_memo_.end()) return it->second;
    if (!return_busy_.insert(&fn).second) return {};
    const peg::Ast* body = nullptr;
    std::string_view annotation;
    switch (fn.tag) {
      case "FUNCTION"_: {
        auto fv = view_function(fn);
        body = fv.body.get();
        annotation = fv.return_type;
        break;
      }
      case "LAMBDA"_:
        body = view_lambda(fn).body.get();
        break;
      case "MULTIFN_DECL"_: {
        size_t i = first_non_decorator_index(fn);
        if (i + 3 <= fn.nodes.size()) {
          body = fn.nodes.back().get();
          if (fn.nodes[i + 2]->tag == "RETURN_TYPE"_) annotation = fn.nodes[i + 2]->token;
        }
        break;
      }
      case "METHOD"_: {
        auto mv = view_method(fn);
        if (mv.body) body = mv.body->get();
        break;
      }
      default:
        break;
    }
    Type t;
    if (!annotation.empty()) {
      t = parse_type(annotation, class_lookup());
    } else if (body) {
      std::vector<const peg::Ast*> returns, yields;
      collect_exits(*body, returns, yields);
      if (!yields.empty()) {
        Alt a{Kind::Iterator};
        Type element;
        for (const auto* y : yields)
          if (!y->nodes.empty()) element = join(element, expr_type(*unwrap(*y->nodes.back())));
        a.element = ref(std::move(element));
        t = Type::single(std::move(a));
      } else {
        t = tail_type(*body);
        for (const auto* r : returns)
          t = join(t, r->nodes.empty() ? Type::of(Kind::Nil)
                                      : expr_type(*unwrap(*r->nodes.back())));
      }
    }
    return_busy_.erase(&fn);
    return_memo_[&fn] = t;
    return t;
  }

  // What `value.` completes to, for a value of type `t`.
  std::vector<Member> members(const Type& t) const {
    std::vector<Member> out;
    std::set<std::string, std::less<>> seen;
    for (const auto& a : t.alts)
      for (auto& m : members_of(a))
        if (seen.insert(m.name).second) out.push_back(std::move(m));
    return out;
  }

  // The free functions visible at `offset`, which `value.name(...)` reaches
  // when the value has no member of that name (UFCS).
  std::vector<Member> ufcs(size_t offset) const {
    std::vector<Member> out = visible(offset, /*functions_only=*/true);
    if (catalog_) {
      std::set<std::string, std::less<>> seen;
      for (const auto& m : out) seen.insert(m.name);
      for (const auto& g : catalog_->globals())
        if (g.kind == MemberKind::Function && seen.insert(g.name).second)
          out.push_back(g);
    }
    return out;
  }

  // The names a bare identifier at `offset` can be: declared earlier in the
  // same function, or anywhere in an enclosing one.
  std::vector<Member> visible(size_t offset, bool functions_only = false) const {
    std::vector<Member> out;
    std::set<std::string, std::less<>> seen;
    bool same_function = true;
    for (size_t sc = res_.scope_at(offset); sc != resolve::kNone;
         sc = res_.scopes[sc].parent) {
      for (const auto& [name, sym] : res_.scopes[sc].names) {
        if (seen.contains(name)) continue;
        if (same_function && res_.first_declaration(sym) >= offset) continue;
        if (functions_only && !callable(sym)) continue;
        seen.insert(name);
        out.push_back(member_for(sym));
      }
      if (res_.scopes[sc].function) same_function = false;
    }
    return out;
  }

  // A declaration as a hover shows it: `fn total(count)`, `class Box`,
  // `sum: Long`.
  std::string describe(size_t s) const {
    const resolve::Symbol& sym = res_.symbols[s];
    switch (sym.kind) {
      case resolve::SymbolKind::Function:
      case resolve::SymbolKind::EffectOperation: {
        std::string out = sym.kind == resolve::SymbolKind::Function
                              ? "fn " : "effect fn ";
        out += sym.name;
        if (auto it = functions_.find(s); it != functions_.end())
          out += params_text(*it->second.front());
        Type r = symbol_type(s);
        if (!r.unknown() && r.alts[0].function) {
          Type ret = return_type(*r.alts[0].function);
          if (!ret.unknown()) out += " -> " + ret.to_string();
        }
        return out;
      }
      case resolve::SymbolKind::Class:
        return "class " + sym.name;
      case resolve::SymbolKind::Enum:
        return "enum " + sym.name;
      case resolve::SymbolKind::Import: {
        for (const auto& imp : res_.imports)
          if (imp.symbol == s) return "import " + sym.name + " from '" + imp.path + "'";
        return "import " + sym.name;
      }
      default: {
        Type t = symbol_type(s);
        return t.unknown() ? sym.name : sym.name + ": " + t.to_string();
      }
    }
  }

  // A member's type, reading a method's return, a static field's initializer or
  // a `self` field's assignments when the member defers to them.
  Type member_type(const Member& m) const {
    if (!m.owner) return m.type;
    if (m.owner != this) return m.owner->member_type(m);
    if (m.function) return return_type(*m.function);
    if (m.value) {
      if (!value_busy_.insert(m.value).second) return {};
      Type t = expr_type(*m.value);
      value_busy_.erase(m.value);
      return t;
    }
    if (m.field_class) return self_field_type(*m.field_class, m.name);
    return m.type;
  }

  // `name(params)` for a function's declaration (FUNCTION, LAMBDA,
  // MULTIFN_DECL).
  std::string signature(std::string_view name, const peg::Ast& fn) const {
    return std::string(name) + params_text(fn);
  }

  // A class's members, as an instance or as the class value.
  std::vector<Member> class_members(const peg::Ast& decl, bool instance) const {
    using namespace peg::udl;
    std::vector<Member> out;
    std::set<std::string, std::less<>> seen;
    auto push = [&](Member m) {
      if (seen.insert(m.name).second) out.push_back(std::move(m));
    };
    if (decl.tag == "ENUM_DECL"_) {
      if (instance) return out;
      size_t i = first_non_decorator_index(decl);
      for (size_t j = i + 1; j < decl.nodes.size(); j++) {
        const peg::Ast& v = *decl.nodes[j];
        if (v.tag != "VARIANT"_ || v.nodes.empty()) continue;
        Alt a{Kind::Instance};
        a.name = std::string(parse_generic_head(decl.nodes[i]->token).outer);
        a.decl = &decl;
        a.owner = this;
        push({std::string(v.nodes[0]->token), MemberKind::EnumMember,
              std::string(src_.substr(v.position, v.length)),
              Type::single(std::move(a))});
      }
      return out;
    }
    size_t i = first_non_decorator_index(decl);
    std::string class_name =
        i < decl.nodes.size()
            ? std::string(parse_generic_head(decl.nodes[i]->token).outer)
            : std::string();
    if (!instance) {
      Alt a{Kind::Instance};
      a.name = class_name;
      a.decl = &decl;
      a.owner = this;
      std::string sig = "new()";
      for (size_t j = i + 1; j < decl.nodes.size(); j++) {
        if (decl.nodes[j]->tag != "METHOD"_) continue;
        auto mv = view_method(*decl.nodes[j]);
        if (mv.name == "new" && mv.params) sig = "new" + params_text(*mv.params);
      }
      push({"new", MemberKind::Constructor, sig, Type::single(std::move(a))});
    }
    for (size_t j = i + 1; j < decl.nodes.size(); j++) {
      const peg::Ast& m = *decl.nodes[j];
      if (m.tag != "METHOD"_) continue;
      auto mv = view_method(m);
      std::string name(mv.name);
      if (name == "new") continue;
      if (mv.is_static == instance) continue;
      if (mv.body) {
        MemberKind kind = mv.is_getter ? MemberKind::Field : MemberKind::Method;
        std::string sig = name + (mv.params ? params_text(*mv.params) : "()");
        Member member{name, kind, sig, {}, this};
        member.function = &m;
        push(std::move(member));
      } else if (mv.is_typed_field) {
        push({name, MemberKind::Field, name + ": " + std::string(mv.type_annotation),
              parse_type(mv.type_annotation, class_lookup())});
      } else if (mv.value) {
        Member member{name, MemberKind::Field, name, {}, this};
        member.value = mv.value;
        push(std::move(member));
      }
    }
    if (instance) {
      if (auto it = self_fields_.find(&decl); it != self_fields_.end())
        for (const auto& [name, values] : it->second) {
          Member member{name, MemberKind::Field, name, {}, this};
          member.field_class = &decl;
          push(std::move(member));
        }
    }
    return out;
  }

 private:
  struct Source {
    enum Kind : uint8_t {
      Value,
      ElementOf,
      Annotation,
      CallbackElement,  // `node` is the CALL, `index` the member taking the callback
      Argument,         // `node` is the argument a call passes
      UfcsReceiver,     // `call`'s chain before `index` is the receiver
    } kind;
    const peg::Ast* node;
    std::string_view text;
    size_t index = 0;
    // Argument / UfcsReceiver: the function called, whose callers must all be
    // in view; and for a UFCS call, the CALL and the member, since the call
    // reaches the function only when the receiver has no member of that name.
    size_t function = resolve::kNone;
    const peg::Ast* call = nullptr;
  };

  size_t offset_of(const peg::Ast& n) const {
    return resolve::name_offset(n, n.is_token ? n.token : std::string_view{}, src_);
  }

  const Member* global_function(std::string_view name) const {
    if (!catalog_) return nullptr;
    for (const auto& g : catalog_->globals())
      if (g.name == name && g.kind == MemberKind::Function) return &g;
    return nullptr;
  }

  size_t symbol_at(const peg::Ast& n) const {
    size_t off = offset_of(n);
    if (off == resolve::kNone) return resolve::kNone;
    const resolve::Occurrence* o = res_.occurrence_at(off);
    return o && o->position == off ? o->symbol : resolve::kNone;
  }

  static const peg::Ast* unwrap(const peg::Ast& n) {
    using namespace peg::udl;
    const peg::Ast* p = &n;
    while (p->tag == "EXPRESSION"_ && p->nodes.size() == 1) p = p->nodes[0].get();
    return p;
  }

  static std::vector<const peg::Ast*> sequence(const peg::Ast& e) {
    using namespace peg::udl;
    std::vector<const peg::Ast*> out;
    for (const auto& c : e.nodes) {
      if (c->tag == "SEQUENCE"_)
        for (const auto& item : c->nodes) out.push_back(item.get());
      else
        out.push_back(c.get());
    }
    return out;
  }

  ClassLookup class_lookup() const {
    return [this](std::string_view name) -> std::optional<Alt> {
      auto it = classes_by_name_.find(name);
      if (it == classes_by_name_.end()) return std::nullopt;
      Alt a{Kind::Instance};
      a.name = std::string(name);
      a.decl = it->second;
      a.owner = this;
      return a;
    };
  }

  std::string params_text(const peg::Ast& n) const {
    using namespace peg::udl;
    const peg::Ast* params = &n;
    if (n.tag == "MULTIFN_DECL"_) {
      size_t i = first_non_decorator_index(n);
      params = i + 1 < n.nodes.size() ? n.nodes[i + 1].get() : nullptr;
    } else if (n.tag == "FUNCTION"_ || n.tag == "LAMBDA"_) {
      params = n.nodes.empty() ? nullptr : n.nodes[0].get();
    }
    if (!params || params->position + params->length > src_.size()) return "()";
    std::string text(src_.substr(params->position, params->length));
    if (n.tag == "LAMBDA"_ && text.size() >= 2) text = "(" + text.substr(1, text.size() - 2) + ")";
    return text;
  }

  // A function declaration, or a variable bound to a function literal.
  bool callable(size_t s) const {
    auto k = res_.symbols[s].kind;
    return k == resolve::SymbolKind::Function ||
           k == resolve::SymbolKind::EffectOperation ||
           (k == resolve::SymbolKind::Variable && declarations_.contains(s));
  }

  Member member_for(size_t s) const {
    const resolve::Symbol& sym = res_.symbols[s];
    MemberKind kind = MemberKind::Variable;
    switch (sym.kind) {
      case resolve::SymbolKind::Variable: kind = MemberKind::Variable; break;
      case resolve::SymbolKind::Parameter: kind = MemberKind::Parameter; break;
      case resolve::SymbolKind::Function:
      case resolve::SymbolKind::EffectOperation: kind = MemberKind::Function; break;
      case resolve::SymbolKind::Class: kind = MemberKind::Class; break;
      case resolve::SymbolKind::Enum: kind = MemberKind::Enum; break;
      case resolve::SymbolKind::Import: kind = MemberKind::Module; break;
    }
    Type t = symbol_type(s);
    // A variable bound to a function literal is a function to call.
    if (kind == MemberKind::Variable && t.alts.size() == 1 &&
        t.alts[0].kind == Kind::Function)
      kind = MemberKind::Function;
    Type shown = t;
    if (kind == MemberKind::Function && !t.unknown() && t.alts[0].function)
      shown = t.alts[0].owner->return_type(*t.alts[0].function);
    return {sym.name, kind, describe(s), shown};
  }

  Type self_field_type(const peg::Ast& decl, const std::string& name) const {
    auto key = std::make_pair(&decl, name);
    if (auto it = field_memo_.find(key); it != field_memo_.end()) return it->second;
    if (!field_busy_.insert(key).second) return {};
    Type t;
    if (auto it = self_fields_.find(&decl); it != self_fields_.end())
      if (auto f = it->second.find(name); f != it->second.end())
        for (const auto* v : f->second) t = join(t, expr_type(*v));
    field_busy_.erase(key);
    field_memo_[key] = t;
    return t;
  }

  // `self` in a class's method, field initializer or a closure inside one: an
  // instance of that class. A static method has none.
  Type self_type(const peg::Ast& e) const {
    using namespace peg::udl;
    bool in_static = false;
    for (auto p = e.parent.lock(); p; p = p->parent.lock()) {
      if (p->tag == "METHOD"_ && view_method(*p).is_static) in_static = true;
      if (p->tag != "CLASS_DECL"_) continue;
      size_t i = first_non_decorator_index(*p);
      if (in_static || i >= p->nodes.size()) return {};
      Alt a{Kind::Instance};
      a.name = std::string(parse_generic_head(p->nodes[i]->token).outer);
      a.decl = p.get();
      a.owner = this;
      return Type::single(std::move(a));
    }
    return {};
  }

  Type element_of(const Type& t) const {
    Type out;
    for (const auto& a : t.alts) {
      if ((a.kind == Kind::Array || a.kind == Kind::Set || a.kind == Kind::Iterator) &&
          a.element)
        out = join(out, *a.element);
      if (a.kind == Kind::String) out = join(out, Type::of(Kind::String));
    }
    return out;
  }

  Type index_type(const Type& t) const {
    Type out;
    for (const auto& a : t.alts) {
      if (a.kind == Kind::Array && a.element) out = join(out, *a.element);
      if (a.kind == Kind::String) out = join(out, Type::of(Kind::String));
    }
    return out;
  }

  Type tail_type(const peg::Ast& body) const {
    using namespace peg::udl;
    if (body.tag == "STATEMENTS"_ || body.tag == "LEXICAL_SCOPE"_)
      return body.nodes.empty() ? Type::of(Kind::Nil) : tail_type(*body.nodes.back());
    if (body.tag == "RETURN"_) return {};  // counted with the returns
    return expr_type(body);
  }

  void collect_exits(const peg::Ast& n, std::vector<const peg::Ast*>& returns,
                     std::vector<const peg::Ast*>& yields) const {
    using namespace peg::udl;
    switch (n.tag) {
      case "FUNCTION"_:
      case "LAMBDA"_:
      case "MULTIFN_DECL"_:
      case "CLASS_DECL"_:
      case "DEFER"_:
        return;  // their exits are their own
      case "RETURN"_:
        returns.push_back(&n);
        break;
      case "YIELD"_:
      case "YIELD_FROM"_:
        yields.push_back(&n);
        break;
      default:
        break;
    }
    for (const auto& c : n.nodes) collect_exits(*c, returns, yields);
  }

  Type arithmetic(const peg::Ast& e) const {
    using namespace peg::udl;
    bool all_string = true, all_number = true, any_float = false, any_tensor = false;
    size_t operands = 0;
    for (const auto& c : e.nodes) {
      if (c->name.ends_with("_OPERATOR")) continue;
      operands++;
      Type t = expr_type(*c);
      if (t.unknown()) return {};
      for (const auto& a : t.alts) {
        all_string &= a.kind == Kind::String;
        bool number = a.kind == Kind::Long || a.kind == Kind::Float;
        all_number &= number || a.kind == Kind::Tensor;
        any_float |= a.kind == Kind::Float;
        any_tensor |= a.kind == Kind::Tensor;
      }
    }
    if (operands == 0) return {};
    if (all_string && e.tag == "ADDITIVE"_) return Type::of(Kind::String);
    if (all_number) {
      if (any_tensor) return Type::of(Kind::Tensor);
      return Type::of(any_float ? Kind::Float : Kind::Long);
    }
    return {};
  }

  std::vector<Member> members_of(const Alt& a) const {
    switch (a.kind) {
      case Kind::String:
      case Kind::Array:
      case Kind::Set:
      case Kind::Tuple:
      case Kind::Tensor:
      case Kind::Iterator:
        return value_members(a.kind);
      case Kind::Object: {
        std::vector<Member> out;
        for (const auto& f : a.fields) {
          Type ft = f.type ? *f.type : Type{};
          out.push_back({f.name, MemberKind::Field,
                         ft.unknown() ? f.name : f.name + ": " + ft.to_string(), ft});
        }
        for (const auto& m : value_members(Kind::Object)) out.push_back(m);
        return out;
      }
      case Kind::Instance:
      case Kind::Class:
        if (a.decl && a.owner)
          return a.owner->class_members(*a.decl, a.kind == Kind::Instance);
        return {};
      case Kind::Namespace:
        if (catalog_)
          if (const auto* ms = catalog_->namespace_members(a.name)) return *ms;
        return {};
      case Kind::Module:
        return modules_ ? modules_(a.name) : std::vector<Member>{};
      default:
        return {};
    }
  }

  // The member `name` of a value of one alternative, without copying the
  // built-in or namespace table it comes from.
  std::optional<Member> find_member(const Alt& a, std::string_view name) const {
    auto in = [&](const std::vector<Member>& ms) -> std::optional<Member> {
      for (const auto& m : ms)
        if (m.name == name) return m;
      return std::nullopt;
    };
    switch (a.kind) {
      case Kind::String:
      case Kind::Array:
      case Kind::Set:
      case Kind::Tuple:
      case Kind::Tensor:
      case Kind::Iterator:
        return in(value_members(a.kind));
      case Kind::Namespace:
        if (catalog_)
          if (const auto* ms = catalog_->namespace_members(a.name)) return in(*ms);
        return std::nullopt;
      default:
        return in(members_of(a));
    }
  }

  Type call_result(const Type& t) const {
    Type out;
    for (const auto& a : t.alts) {
      if (a.kind == Kind::Function && a.function && a.owner) {
        out = join(out, a.owner->return_type(*a.function));
      } else if (a.kind == Kind::Function && !a.name.empty()) {
        if (const Member* g = global_function(a.name)) out = join(out, g->type);
      } else if (a.kind == Kind::Class && a.decl) {
        Alt inst{Kind::Instance};
        inst.name = a.name;
        inst.decl = a.decl;
        inst.owner = a.owner;
        add_alt(out, std::move(inst));
      }
    }
    return out;
  }

  Type method_result(const Type& t, std::string_view name, size_t offset,
                     const peg::Ast* args = nullptr) const {
    Type out;
    bool found = false;
    if (const CallbackRule* rule = callback_rule(name); rule && args) {
      Type r = callback_result(t, *rule, *args);
      if (!r.unknown()) return r;
    }
    for (const auto& a : t.alts) {
      if (auto m = find_member(a, name)) {
        found = true;
        bool callable_value = m->kind == MemberKind::Field ||
                              m->kind == MemberKind::Constant ||
                              m->kind == MemberKind::Variable;
        Type mt = member_type(*m);
        out = join(out, callable_value ? call_result(mt) : mt);
      }
    }
    if (!found) return free_function_result(name, offset);
    return out;
  }

  // Whether a function's callers may lie outside this document or reach it
  // through a value: it is exported, or its name is read other than as a
  // callee. Then the calls in view do not tell a parameter's type.
  bool escapes(size_t fn) const {
    if (fn == resolve::kNone) return true;
    const resolve::Symbol& sym = res_.symbols[fn];
    if (sym.exported) return true;
    for (size_t i : sym.occurrences) {
      const auto& o = res_.occurrences[i];
      if (o.role != resolve::Role::Read ||
          o.spelling == resolve::Spelling::KeywordLabel)
        continue;
      if (!called_at_.contains(o.position)) return true;
    }
    return false;
  }

  // A source from a UFCS call counts only if the receiver has no member of
  // the called name; any other source counts.
  bool reaches_free_function(const Source& src) const {
    if (!src.call) return true;
    std::string_view name = src.call->nodes[src.index]->token;
    for (const auto& a : chain_type(*src.call, src.index).alts)
      if (find_member(a, name)) return false;
    return true;
  }

  // The parameter symbols of every declaration bound to `fn`, in order (kNone
  // for a pattern parameter).
  std::vector<std::vector<size_t>> parameter_lists(size_t fn) const {
    using namespace peg::udl;
    std::vector<std::vector<size_t>> out;
    auto it = declarations_.find(fn);
    if (it == declarations_.end()) return out;
    for (const peg::Ast* params : it->second) {
      std::vector<size_t> list;
      for (const auto& p : params->nodes) {
        if (is_kw_only_sep(*p)) continue;
        bool plain = p->tag == "PARAMETER"_ && p->nodes.size() >= 2 &&
                     !is_pattern_param(*p);
        list.push_back(plain ? symbol_at(*p->nodes[1]) : resolve::kNone);
      }
      out.push_back(std::move(list));
    }
    return out;
  }

  // What a built-in method that takes a function gives, from its rule.
  Type callback_result(const Type& receiver, const CallbackRule& rule,
                       const peg::Ast& args) const {
    auto positional = positional_args(args);
    const peg::Ast* fn = rule.callback_arg < static_cast<int>(positional.size())
                             ? positional[rule.callback_arg]
                             : nullptr;
    auto fn_result = [&]() -> Type {
      return fn ? call_result(expr_type(*fn)) : Type{};
    };
    Type out;
    for (const auto& a : receiver.alts) {
      bool container =
          a.kind == Kind::Array || a.kind == Kind::Iterator || a.kind == Kind::Set;
      if (!container) continue;
      Kind kind = a.kind == Kind::Set ? Kind::Array : a.kind;
      switch (rule.result) {
        case CallbackResult::Same:
          add_alt(out, a);
          break;
        case CallbackResult::Map: {
          Alt m{kind};
          m.element = ref(fn_result());
          add_alt(out, std::move(m));
          break;
        }
        case CallbackResult::FlatMap: {
          Alt m{kind};
          m.element = ref(element_of(fn_result()));
          add_alt(out, std::move(m));
          break;
        }
        case CallbackResult::Element:
          if (a.element) out = join(out, *a.element);
          add_alt(out, Alt{Kind::Nil});
          break;
        case CallbackResult::Fold:
          if (!positional.empty()) out = join(out, expr_type(*positional[0]));
          out = join(out, fn_result());
          break;
        case CallbackResult::Bool:
          add_alt(out, Alt{Kind::Bool});
          break;
        case CallbackResult::Nil:
          add_alt(out, Alt{Kind::Nil});
          break;
        case CallbackResult::Object:
          add_alt(out, Alt{Kind::Object});
          break;
        case CallbackResult::Tuple:
          add_alt(out, Alt{Kind::Tuple});
          break;
        case CallbackResult::Position:
          add_alt(out, Alt{Kind::Long});
          add_alt(out, Alt{Kind::Nil});
          break;
        case CallbackResult::Iterator:
          add_alt(out, Alt{Kind::Iterator});
          break;
      }
    }
    return out;
  }

  // What `value.name(...)` gives when the value has no member `name`: the free
  // function of that name visible at `offset` (UFCS), or a global's.
  Type free_function_result(std::string_view name, size_t offset) const {
    size_t s = res_.lookup(res_.scope_at(offset), name);
    if (s != resolve::kNone) return call_result(symbol_type(s));
    if (const Member* g = global_function(name)) return g->type;
    return {};
  }

  Type property_type(const Type& t, std::string_view name) const {
    Type out;
    for (const auto& a : t.alts)
      if (auto m = find_member(a, name)) {
        if (m->kind == MemberKind::Method || m->kind == MemberKind::Function)
          out = join(out, Type::of(Kind::Function));
        else
          out = join(out, member_type(*m));
      }
    return out;
  }

  void index(const peg::Ast& n, const peg::Ast* cls) {
    using namespace peg::udl;
    switch (n.tag) {
      case "ASSIGNMENT"_: {
        auto av = view_assignment(n);
        if (const auto* t = assign_name_target(n, av); t && !av.compound) {
          size_t s = symbol_at(*t);
          if (s != resolve::kNone) {
            if (!av.type_annotation.empty())
              sources_[s].push_back({Source::Annotation, nullptr, av.type_annotation});
            sources_[s].push_back({Source::Value, av.rhs, {}});
            if ((av.rhs->tag == "FUNCTION"_ || av.rhs->tag == "LAMBDA"_) &&
                !av.rhs->nodes.empty())
              declarations_[s].push_back(av.rhs->nodes[0].get());
          }
        } else if (cls && av.lvalcnt == 2 && !av.compound) {
          const peg::Ast& base = *n.nodes[av.lvaloff];
          const peg::Ast& mem = *n.nodes[av.lvaloff + 1];
          if (base.tag == "IDENTIFIER"_ && base.token == "self" &&
              mem.tag == "IDENTIFIER"_ && mem.original_tag == "DOT"_)
            self_fields_[cls][std::string(mem.token)].push_back(av.rhs);
        }
        break;
      }
      case "MULTIFN_DECL"_: {
        size_t i = first_non_decorator_index(n);
        if (i < n.nodes.size()) {
          size_t s = symbol_at(*n.nodes[i]);
          if (s != resolve::kNone) {
            functions_[s].push_back(&n);
            if (i + 1 < n.nodes.size())
              declarations_[s].push_back(n.nodes[i + 1].get());
          }
        }
        break;
      }
      case "CLASS_DECL"_:
      case "ENUM_DECL"_: {
        size_t i = first_non_decorator_index(n);
        if (i < n.nodes.size()) {
          size_t s = symbol_at(*n.nodes[i]);
          if (s != resolve::kNone) types_[s] = &n;
          classes_by_name_[std::string(parse_generic_head(n.nodes[i]->token).outer)] = &n;
        }
        for (const auto& c : n.nodes) index(*c, n.tag == "CLASS_DECL"_ ? &n : cls);
        return;
      }
      case "FOR"_: {
        if (n.nodes.size() >= 3) {
          auto fv = view_for(n);
          if (fv.binding->tag == "IDENTIFIER"_) {
            size_t s = symbol_at(*fv.binding);
            if (s != resolve::kNone) sources_[s].push_back({Source::ElementOf, fv.iter, {}});
          }
        }
        break;
      }
      case "PARAMETER"_:
        if (n.nodes.size() >= 3 && n.nodes[2]->tag == "TYPE_ANNOTATION"_) {
          size_t s = symbol_at(*n.nodes[1]);
          if (s != resolve::kNone)
            sources_[s].push_back({Source::Annotation, nullptr, n.nodes[2]->token});
        }
        if (const auto* d = extract_default_expr(n); d && n.nodes.size() >= 2) {
          size_t s = symbol_at(*n.nodes[1]);
          if (s != resolve::kNone) sources_[s].push_back({Source::Value, d, {}});
        }
        break;
      case "TYPED_IDENT"_:
        if (n.nodes.size() >= 2) {
          size_t s = symbol_at(*n.nodes[0]);
          if (s != resolve::kNone)
            sources_[s].push_back({Source::Annotation, nullptr, n.nodes[1]->token});
        }
        break;
      case "CALL"_:
        note_call(n);
        for (size_t i = 1; i + 1 < n.nodes.size(); i++) {
          const peg::Ast& c = *n.nodes[i];
          if (c.tag != "IDENTIFIER"_ || c.original_tag != "DOT"_) continue;
          if (n.nodes[i + 1]->original_tag != "ARGUMENTS"_) continue;
          const CallbackRule* rule = callback_rule(c.token);
          if (!rule) continue;
          auto positional = positional_args(*n.nodes[i + 1]);
          if (rule->callback_arg >= static_cast<int>(positional.size())) continue;
          const peg::Ast& fn = *positional[rule->callback_arg];
          if (fn.tag != "FUNCTION"_ && fn.tag != "LAMBDA"_) continue;
          const peg::Ast* params = fn.nodes.empty() ? nullptr : fn.nodes[0].get();
          if (!params) continue;
          int k = 0;
          for (const auto& param : params->nodes) {
            if (is_kw_only_sep(*param)) continue;
            if (k++ != rule->element_param) continue;
            if (param->tag == "PARAMETER"_ && param->nodes.size() >= 2 &&
                !is_pattern_param(*param)) {
              size_t sym = symbol_at(*param->nodes[1]);
              if (sym != resolve::kNone)
                sources_[sym].push_back({Source::CallbackElement, &n, {}, i});
            }
          }
        }
        break;
      case "MATCH"_:
        if (n.nodes.size() >= 2) {
          auto mv = view_match(n);
          for (const auto& arm : mv.arms->nodes)
            if (!arm->nodes.empty() && arm->nodes[0]->tag == "IDENTIFIER"_) {
              size_t s = symbol_at(*arm->nodes[0]);
              if (s != resolve::kNone)
                sources_[s].push_back({Source::Value, mv.subject, {}});
            }
        }
        break;
      default:
        break;
    }
    for (const auto& c : n.nodes) index(*c, cls);
  }

  // A call whose arguments bind a function's parameters, kept until every
  // declaration is indexed: the callee may be declared further down.
  struct PendingCall {
    const peg::Ast* call;
    size_t member;  // 0 for `f(args)`; the member's index for `recv.f(args)`
    size_t function;
  };

  void note_call(const peg::Ast& call) {
    using namespace peg::udl;
    if (call.nodes.size() < 2) return;
    const peg::Ast& head = *call.nodes[0];
    if (head.tag == "IDENTIFIER"_ && head.original_tag != "DOT"_ &&
        call.nodes[1]->original_tag == "ARGUMENTS"_) {
      size_t fn = symbol_at(head);
      if (fn != resolve::kNone) {
        called_at_.insert(offset_of(head));
        pending_.push_back({&call, 0, fn});
      }
    }
    for (size_t i = 1; i + 1 < call.nodes.size(); i++) {
      const peg::Ast& m = *call.nodes[i];
      if (m.tag != "IDENTIFIER"_ || m.original_tag != "DOT"_) continue;
      if (call.nodes[i + 1]->original_tag != "ARGUMENTS"_) continue;
      size_t off = offset_of(m);
      if (off == resolve::kNone) continue;
      size_t fn = res_.lookup(res_.scope_at(off), m.token);
      if (fn != resolve::kNone) pending_.push_back({&call, i, fn});
    }
  }

  void bind_calls() {
    using namespace peg::udl;
    for (const auto& c : pending_) {
      bool ufcs = c.member != 0;
      const peg::Ast& args = *c.call->nodes[ufcs ? c.member + 1 : 1];
      for (const auto& list : parameter_lists(c.function)) {
        Source base{Source::Argument, nullptr, {}, c.member, c.function,
                    ufcs ? c.call : nullptr};
        if (ufcs && !list.empty() && list[0] != resolve::kNone) {
          Source recv = base;
          recv.kind = Source::UfcsReceiver;
          sources_[list[0]].push_back(recv);
        }
        size_t k = ufcs ? 1 : 0;
        for (const auto& a : args.nodes) {
          if (a->tag == "KWARG_SPLAT"_) continue;
          if (a->tag == "KWARG"_) {
            if (a->nodes.size() < 2) continue;
            for (size_t p : list)
              if (p != resolve::kNone && res_.symbols[p].name == a->nodes[0]->token) {
                Source arg = base;
                arg.node = a->nodes[1].get();
                sources_[p].push_back(arg);
              }
            continue;
          }
          if (k < list.size() && list[k] != resolve::kNone) {
            Source arg = base;
            arg.node = a.get();
            sources_[list[k]].push_back(arg);
          }
          k++;
        }
      }
    }
    pending_.clear();
  }

  std::vector<PendingCall> pending_;
  std::set<size_t> called_at_;  // offsets of names read as a callee
  std::map<size_t, std::vector<const peg::Ast*>> declarations_;  // fn -> parameter lists

  const peg::Ast& root_;
  std::string_view src_;
  const resolve::Resolution& res_;
  const Catalog* catalog_;
  ModuleMembers modules_;

  std::map<size_t, std::vector<Source>> sources_;
  std::map<size_t, std::vector<const peg::Ast*>> functions_;
  std::map<size_t, const peg::Ast*> types_;
  std::map<std::string, const peg::Ast*, std::less<>> classes_by_name_;
  std::map<const peg::Ast*,
           std::map<std::string, std::vector<const peg::Ast*>, std::less<>>>
      self_fields_;

  mutable std::map<size_t, Type> symbol_memo_;
  mutable std::set<size_t> symbol_busy_;
  mutable std::map<const peg::Ast*, Type> return_memo_;
  mutable std::set<const peg::Ast*> return_busy_;
  mutable std::set<const peg::Ast*> value_busy_;
  mutable std::map<std::pair<const peg::Ast*, std::string>, Type> field_memo_;
  mutable std::set<std::pair<const peg::Ast*, std::string>> field_busy_;
};

}  // namespace culebra::infer
