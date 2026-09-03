#pragma once

// The DAP server's JSON DOM: a small value type over json.h's Builder/Reader
// policies, so dap.h builds and serializes protocol messages without the
// interp's Value (Phase 4 B7-b — the wire format is engine-independent, and
// the debuggee's values reach the protocol as plain strings via DebugVar /
// DebugFrame). Kinds cover exactly what DAP traffic contains; Float appears
// only when a client sends one (we never emit it).

#include <stdlib/json.h>
#include <base/shared.h>  // CulebraError (parse errors)

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culebra::dapjson {

struct Value {
  enum Kind { Nil, Bool, Long, Float, String, Array, Object };
  Kind type = Nil;
  bool b = false;
  int64_t l = 0;
  double d = 0.0;
  std::string s;
  std::vector<Value> elems;
  std::vector<std::pair<std::string, Value>> entries;

  Value() = default;
  explicit Value(bool v) : type(Bool), b(v) {}
  explicit Value(int64_t v) : type(Long), l(v) {}
  explicit Value(double v) : type(Float), d(v) {}
  explicit Value(std::string v) : type(String), s(std::move(v)) {}

  bool to_bool() const { return b; }
  int64_t to_long() const { return l; }
  const std::string& to_string() const { return s; }
  const std::vector<Value>& elements() const { return elems; }

  void set(std::string key, Value v) {
    entries.emplace_back(std::move(key), std::move(v));
  }
  const Value* find(std::string_view key) const {
    for (const auto& [k, v] : entries)
      if (k == key) return &v;
    return nullptr;
  }
};

struct _Builder {
  using Value = dapjson::Value;  // json.h reads B::Value
  using Object = dapjson::Value;
  using Array = dapjson::Value;
  static Value make_null() { return Value(); }
  static Value boolean(bool b) { return Value(b); }
  static Value integer(int64_t n) { return Value(n); }
  static Value real(double d) { return Value(d); }
  static Value string(std::string&& s) { return Value(std::move(s)); }
  static Object object_new() {
    Value v;
    v.type = Value::Object;
    return v;
  }
  static void object_set(Object& o, const std::string& key, Value&& v) {
    o.set(key, std::move(v));
  }
  static Value object_done(Object&& o) { return std::move(o); }
  static Array array_new() {
    Value v;
    v.type = Value::Array;
    return v;
  }
  static void array_push(Array& a, Value&& v) {
    a.elems.push_back(std::move(v));
  }
  static Value array_done(Array&& a) { return std::move(a); }
  // Value-type storage: C++ unwinding releases partials.
  static void object_abandon(Object&) {}
  static void array_abandon(Array&) {}
  static void abandon_value(Value&) {}
};

struct _Reader {
  using Value = dapjson::Value;  // json.h reads R::Value
  static json::Kind kind(const Value& v) {
    switch (v.type) {
      case Value::Nil: return json::Kind::Nil;
      case Value::Bool: return json::Kind::Bool;
      case Value::Long: return json::Kind::Long;
      case Value::Float: return json::Kind::Float;
      case Value::String: return json::Kind::String;
      case Value::Array: return json::Kind::Seq;
      case Value::Object: return json::Kind::Object;
    }
    return json::Kind::Other;
  }
  static bool as_bool(const Value& v) { return v.b; }
  static int64_t as_long(const Value& v) { return v.l; }
  static double as_double(const Value& v) { return v.d; }
  static std::string_view as_string(const Value& v) { return v.s; }
  static size_t seq_size(const Value& v) { return v.elems.size(); }
  static const Value& seq_at(const Value& v, size_t i) { return v.elems[i]; }
  static bool object_has_non_string_keys(const Value&) { return false; }
  static std::vector<std::pair<std::string_view, const Value*>>
  object_entries(const Value& v) {
    std::vector<std::pair<std::string_view, const Value*>> out;
    out.reserve(v.entries.size());
    for (const auto& [k, val] : v.entries) out.emplace_back(k, &val);
    return out;
  }
  static std::string_view type_name(const Value&) { return "Object"; }
};

inline Value parse(std::string_view s) {
  return json::parse<_Builder>(s, "auto", false);
}
inline std::string stringify(const Value& v) {
  return json::stringify<_Reader>(v, 0, false);
}

}  // namespace culebra::dapjson
