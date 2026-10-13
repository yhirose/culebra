// vm::Value's host-facing surface: the strict read (as<T>), container
// access (operator[] / size / has / items / range-for), the writes that
// reach through into the script's own Object and Array, the two builders,
// Embed::eval's structured failure, and the container types define()
// converts. (docs/deployment.md §2)

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <culebra.h>
#include <vm/embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace value_api_smoke_ns {

namespace {

using culebra::vm::Value;

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

// Run `code` and return the kind of the error it raised, or "" if it ran.
template <class Body>
std::string kind_of(Body&& body) {
  try {
    body();
  } catch (const culebra::CulebraError& e) {
    return e.kind;
  } catch (const std::exception& e) {
    return std::string("host: ") + e.what();
  }
  return "";
}

// ... or the whole failure the host was handed, as `kind: message`.
template <class Body>
std::string failure_of(Body&& body) {
  try {
    body();
  } catch (const culebra::CulebraError& e) {
    return e.kind + ": " + e.what();
  }
  return "";
}

}  // namespace

int run() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
  bool ok = true;

  // (1) eval: the value comes back, a failure is the exception.
  ok &= check(embed.eval("1 + 2").as<int64_t>() == 3, "eval returns the value");
  ok &= check(kind_of([&] { embed.eval("1 +"); }) == "SyntaxError",
              "eval: a parse failure is SyntaxError");
  ok &= check(kind_of([&] { embed.eval("1 + 'x'"); }) == "TypeError",
              "eval: a runtime failure keeps its kind");
  {
    bool positioned = false;
    try {
      embed.eval("let a = 1\nlet b = a + 'x'");
    } catch (const culebra::CulebraError& e) {
      positioned = e.line == 2;
    }
    ok &= check(positioned, "eval: the error keeps its line");
  }

  // (2) Reading a script-side Object.
  embed.eval(
      "let config = {mut port: 8080, name: 'api', "
      "hosts: ['a.example', 'b.example'], "
      "db: {mut host: 'localhost', mut opts: {mut tls: false}}, "
      "limits: {rps: 10, burst: 20}}");
  Value cfg = embed.global("config");
  ok &= check(cfg.is_object() && !cfg.is_nil(), "global() finds the Object");
  ok &= check(cfg["port"].as<int64_t>() == 8080, "read a Long entry");
  ok &= check(cfg["name"].as<std::string>() == "api", "read a String entry");
  ok &= check(cfg["db"]["host"].as<std::string>() == "localhost",
              "read through a nested Object");
  ok &= check(cfg.size() == 5, "size() counts the entries");
  ok &= check(cfg.has("port") && !cfg.has("nope"), "has()");
  ok &= check(cfg["hosts"].size() == 2 && cfg["hosts"][0].as<std::string>() ==
                                              "a.example",
              "index an Array");
  ok &= check(cfg["hosts"][-1].as<std::string>() == "b.example",
              "a negative index counts from the end");
  ok &= check(cfg["hosts"].at(embed.eval("0..1")).size() == 1,
              "a Range key slices, as it does in script");
  ok &= check(embed.eval("class P { new() { self.a = 1 } }\nP()")
                  .type_name() == std::string("P"),
              "type_name() answers with the class, like type_of");

  {  // range-for over an Array
    std::string joined;
    for (const auto& h : cfg["hosts"]) {
      if (!joined.empty()) joined += ",";
      joined += h.as<std::string>();
    }
    ok &= check(joined == "a.example,b.example", "range-for over an Array");
  }
  {  // items() walks an Object in insertion order
    std::string joined;
    for (const auto& [k, v] : cfg["limits"].items())
      joined += k.as<std::string>() + "=" + std::to_string(v.as<int64_t>()) + ";";
    ok &= check(joined == "rps=10;burst=20;", "items() in insertion order");
  }
  {  // items() finds a stored key by the hash it carries, not by hashing again
    Value d = embed.eval(R"(
      class Id {
        new(n) { self.n = n }
        hash() { self.n }
        eq(o) { self.n == o.n }
      }
      let k = Id.new(1)
      mut d = {a: 0}
      d[k] = 'one'
      k.n = 2
      d
    )");
    auto entries = d.items();
    ok &= check(entries.size() == 2 &&
                    entries[1].second.as<std::string>() == "one",
                "items() keeps an entry whose key's hash changed");
  }

  // (3) The reads that should fail, failing the way the script's do.
  ok &= check(kind_of([&] { cfg["nope"]; }) == "KeyError",
              "a missing key is KeyError");
  ok &= check(kind_of([&] { cfg["hosts"][7]; }) == "IndexError",
              "an out-of-range index is IndexError");
  ok &= check(kind_of([&] { cfg["port"]["x"]; }) == "TypeError",
              "subscripting a Long is TypeError");
  ok &= check(kind_of([&] { cfg["port"].as<std::string>(); }) == "TypeError",
              "as<T> rejects a mismatch");
  ok &= check(cfg["port"].to_string().empty(),
              "to_string() stays lenient on a mismatch");
  ok &= check(cfg["name"].type_name() == std::string("String"), "type_name()");

  // (4) Writing reaches the script's own value.
  cfg.set("port", 9090);
  cfg["db"].set("host", "db.internal");
  cfg["db"]["opts"].set("tls", true);
  cfg["hosts"].push("c.example");
  cfg["hosts"].set(int64_t{0}, "a2.example");
  ok &= check(embed.eval("config.port").as<int64_t>() == 9090,
              "set() is visible to the script");
  ok &= check(embed.eval("config.db.host").as<std::string>() == "db.internal",
              "a nested set() writes into the parent");
  ok &= check(embed.eval("config.db.opts.tls").as<bool>(),
              "two levels down too");
  ok &= check(embed.eval("config.hosts.join(',')").as<std::string>() ==
                  "a2.example,b.example,c.example",
              "push() and an index write");
  // A property the script declared without `mut` refuses the host too.
  ok &= check(kind_of([&] { cfg.set("name", "other"); }) == "ImmutableError",
              "an immutable property refuses a host write");
  ok &= check(kind_of([&] { cfg["hosts"].set(int64_t{9}, 1); }) == "IndexError",
              "an index past the end is IndexError, not a grow");

  // (5) Builders, and calling a script function with inline arguments.
  embed.eval(
      "fn describe(name, opts) { name + ':' + opts.port.to_string() + "
      "':' + opts.tags.join('|') }\n"
      "fn sum(a, b) { a + b }");
  ok &= check(embed.call("sum", 1, 2).as<int64_t>() == 3,
              "call with inline arguments");
  {
    auto opts = Value::object({{"port", 8080},
                               {"tags", Value::array({"x", "y"})}});
    ok &= check(embed.call("describe", "api", opts).as<std::string>() ==
                    "api:8080:x|y",
                "the builders round-trip through a call");
    // What the builders make is the host's own data, so it stays writable.
    opts.set("port", 1);
    ok &= check(opts["port"].as<int64_t>() == 1, "a built Object is mutable");
  }
  {  // the vector<Value> form still takes values the host already holds
    std::vector<Value> args;
    args.emplace_back(int64_t{4});
    args.emplace_back(int64_t{5});
    ok &= check(embed.call("sum", std::move(args)).as<int64_t>() == 9,
                "the explicit vector form is unchanged");
  }
  {  // a caught runtime error thrown again arrives as that error
    embed.eval("fn rethrow() {\n  try { [1][9] } catch e { throw e }\n}");
    std::string kind;
    int64_t line = 0;
    try {
      embed.call("rethrow");
    } catch (const culebra::CulebraError& e) {
      kind = e.kind;
      line = e.line;
    }
    ok &= check(kind == "IndexError" && line == 2,
                "a re-thrown error keeps its kind and position through call");
    // call was that throw's catch, so the next one binds its own error.
    ok &= check(embed.eval("try { [1][9] } catch e { e.kind }")
                        .as<std::string>() == "IndexError",
                "a throw out of call leaves the carrier empty");
  }
  {  // a class's __index__ / __setindex__ run under a Value read or write
    embed.eval(
        "class Sealed {\n"
        "  __index__(k) { throw 'no ' + k }\n"
        "  __setindex__(k, v) {\n"
        "    throw {kind: 'ValueError', message: 'sealed ' + k}\n"
        "  }\n"
        "}\n"
        "let sealed = Sealed()");
    Value sealed = embed.global("sealed");
    // The key is in the message, so it arrived as a String the method could
    // read.
    ok &= check(failure_of([&] { (void)sealed["port"]; }) ==
                    "RuntimeError: no port",
                "a throw out of __index__ is the failure a call's is");
    ok &= check(failure_of([&] { sealed.set("port", 1); }) ==
                    "ValueError: sealed port",
                "a throw out of __setindex__ keeps the kind it names");
    ok &= check(embed.eval("try { [1][9] } catch e { e.kind }")
                        .as<std::string>() == "IndexError",
                "a throw out of a Value read leaves the carrier empty");
  }
  {  // listing or showing an Object walks its own entries: a key it holds is
     // found as that very value, and asked for neither its hash nor its eq
    Value tagged = embed.eval(R"(
      mut strict = false
      fn be_strict() { strict = true }
      class Tag {
        new(n) { self.n = n }
        hash() { 0 }
        eq(o) {
          if strict { throw 'no eq' }
          self.n == o.n
        }
      }
      mut tagged = {}
      tagged[Tag.new(1)] = 'one'
      tagged[Tag.new(2)] = 'two'
      tagged
    )");
    embed.call("be_strict");
    auto entries = tagged.items();
    ok &= check(entries.size() == 2 &&
                    entries[0].second.as<std::string>() == "one" &&
                    entries[1].second.as<std::string>() == "two",
                "items() runs none of the script's code");
    ok &= check(tagged.display().find("'one'") != std::string::npos,
                "nor does display()");
    // The entries are the Object's own: a Regex match's are its fields, where
    // a subscript on one is a capture group.
    Value m = embed.eval("Regex.find('in (?<value>[0-9]+)', 'in 2026')");
    std::string fields;
    for (const auto& [k, v] : m.items())
      if (v.is_string() || v.is_long())
        fields += k.as<std::string>() + "=" +
                  (v.is_string() ? v.as<std::string>()
                                 : std::to_string(v.as<int64_t>())) + ";";
    ok &= check(fields == "value=in 2026;start=0;end=7;",
                "items() lists a Regex match's fields");
  }
  {  // a string key the host holds answers as the same key does in the script
    embed.eval(
        "class Shelf {\n"
        "  new() { self.size = 3 }\n"
        "  __index__(k) { 'no ' + k }\n"
        "  __setindex__(k, v) { self.last = k + '=' + to_string(v) }\n"
        "}\n"
        "let shelf = Shelf()");
    Value shelf = embed.global("shelf");
    ok &= check(shelf["size"].as<int64_t>() == 3,
                "a field is read from its slot, ahead of __index__");
    ok &= check(shelf["top"].as<std::string>() == "no top",
                "a key that is no field goes to __index__");
    shelf.set("size", 4);
    shelf.set("top", 9);
    ok &= check(embed.eval("shelf.size").as<int64_t>() == 4 &&
                    embed.eval("shelf.last").as<std::string>() == "top=9",
                "a field is written to its slot, another key to __setindex__");
    // A Regex match reads a subscript as a capture group, not as a field:
    // `value` is both here, and they differ.
    Value m = embed.eval("Regex.find('in (?<value>[0-9]+)', 'in 2026')");
    ok &= check(m["value"].as<std::string>() ==
                    embed.eval("Regex.find('in (?<value>[0-9]+)', 'in 2026')"
                               "['value']").as<std::string>() &&
                    m["value"].as<std::string>() == "2026",
                "a Regex match answers with the named group");
    // `size` is a method slot of the view and a key of the tree.
    ok &= check(embed.eval("Shared.new({size: 8080})")["size"].as<int64_t>() ==
                    8080,
                "a Shared view answers from its frozen tree");
    // ... and its slots are the handle's own, so it has no entries to list.
    ok &= check(kind_of([&] {
                  embed.eval("Shared.new({size: 8080})").items();
                }) == "TypeError",
                "items() refuses a handle rather than list its bookkeeping");
  }
  {  // a substring is a String to the host, wherever it reads one
    Value part = embed.eval("let line = 'port=8080'\nline[0..4]");
    ok &= check(part.is_string() && part.to_string() == "port" &&
                    part.as<std::string>() == "port" &&
                    part.as<std::string_view>() == "port",
                "a substring reads as its own bytes");
  }

  // (6) define()'s container types, both directions.
  embed.define("host_total", [](std::vector<int64_t> xs) -> int64_t {
    int64_t n = 0;
    for (auto x : xs) n += x;
    return n;
  }, {"xs"});
  embed.define("host_widen", [](std::map<std::string, int64_t> m)
                                 -> std::map<std::string, int64_t> {
    for (auto& [k, v] : m) v *= 2;
    return m;
  }, {"m"});
  embed.define("host_find", [](std::string k) -> std::optional<std::string> {
    if (k == "a") return "found";
    return std::nullopt;
  }, {"k"});
  embed.define("host_names", []() -> std::vector<std::string> {
    return {"a", "b"};
  });

  ok &= check(embed.eval("host_total([1, 2, 3])").as<int64_t>() == 6,
              "an Array argument becomes std::vector");
  ok &= check(embed.eval("host_widen({a: 1, b: 2}).b").as<int64_t>() == 4,
              "an Object round-trips through std::map");
  ok &= check(embed.eval("host_find('a')").as<std::string>() == "found",
              "an engaged optional is the value");
  ok &= check(embed.eval("host_find('z')").is_nil(),
              "an empty optional is nil");
  ok &= check(embed.eval("host_find('za'[1..2])").as<std::string>() == "found",
              "a substring argument is the String it spells");
  ok &= check(embed.eval("host_names().join(',')").as<std::string>() == "a,b",
              "a returned vector is an Array");
  ok &= check(embed.eval("try { host_total([1, 'x']) } catch e { e.kind }")
                      .as<std::string>() == "TypeError",
              "a bad element raises the element's TypeError");
  // ... and the host reads them back the same way.
  ok &= check(embed.eval("[1, 2, 3]").as<std::vector<int64_t>>().size() == 3,
              "as<std::vector>");
  ok &= check(embed.eval("{a: 1}").as<std::map<std::string, int64_t>>().at("a")
                  == 1,
              "as<std::map>");
  ok &= check(!embed.eval("nil").as<std::optional<int64_t>>().has_value(),
              "as<std::optional> on nil");

  // (7) Ownership. Every read hands back a +1 and every write absorbs one, so
  // a value's refcount must come back to where it started once the host's
  // handles are gone — including on the throw paths, where an in-flight +1 is
  // easiest to strand. Read it off the object directly: the GC backstop
  // reclaims a stranded one either way, so heap growth would not show this.
  {
    Value root = Value::object({{"xs", Value::array({1, 2, 3})},
                                {"inner", Value::object({{"k", 1}})}});
    auto* xs = reinterpret_cast<JitArray*>(root["xs"].get().data);
    auto* inner = reinterpret_cast<JitObject*>(root["inner"].get().data);
    const int64_t xs_base = xs->refcount;        // held by `root` alone
    const int64_t inner_base = inner->refcount;

    for (int i = 0; i < 100; i++) {
      root["xs"].size();
      root["xs"][0].as<int64_t>();
      root["xs"].at(Value(int64_t{1}));
      for (const auto& e : root["xs"]) e.as<int64_t>();
      root["inner"].items();
      root["inner"].has("k");
      root["inner"].set("k", 2);
      root["inner"].set("k", Value::array({1}));  // an Object-valued write
      root.set("xs", root["xs"]);  // write a reference back into its parent
      root["xs"].set(int64_t{0}, i);
      embed.call("sum", 1, 2);
      try { root["nope"]; } catch (const culebra::CulebraError&) {}
      try { root["xs"][9]; } catch (const culebra::CulebraError&) {}
      try { root["xs"].set(int64_t{9}, 1); } catch (const culebra::CulebraError&) {}
      try { root["xs"].as<std::string>(); } catch (const culebra::CulebraError&) {}
      try { root["inner"].as<std::vector<int64_t>>(); }
      catch (const culebra::CulebraError&) {}
    }
    if (xs->refcount != xs_base || inner->refcount != inner_base)
      std::cerr << "refcount drift: xs " << xs_base << " -> " << xs->refcount
                << ", inner " << inner_base << " -> " << inner->refcount << "\n";
    ok &= check(xs->refcount == xs_base && inner->refcount == inner_base,
                "100 rounds strand no reference");
  }

  // (8) A Value keeps what it holds wherever the host puts it. A collection
  // finds one on the stack by scanning the stack; one in a std::vector's
  // buffer it does not see, so that one pins its value, and unpins it on the
  // way out. Both kinds of collection run here: the threshold's, which takes
  // its roots from the stack, and GC.stat()'s, which takes them from counts
  // (and so never had a root for a String, which has no count).
  {
    embed.eval(
        "fn churn() {\n"
        "  for i in 0..20000 {\n"
        "    mut a = {name: 'x' + to_string(i)}\n"
        "    mut b = {peer: a}\n"
        "    a.peer = b\n"
        "  }\n"
        "}\n"
        "fn make(i) { [i, 'elem-' + to_string(i)] }\n"
        "fn make_fn(i) {\n  let s = 'cap-' + to_string(i)\n  fn () { s }\n}\n"
        "fn invoke(f) { f() }\n"
        "fn show(a, b, c, d) { [a, b, c, d].join('|') }");
    auto pinned = [](const Value& v) {
      auto* h = _gc_heap().header(reinterpret_cast<void*>(v.get().data));
      return h && (h->flags & culebra::gc::Heap::kFlagPinned);
    };
    auto held = std::make_unique<std::vector<Value>>();
    for (int i = 0; i < 4; i++) {  // unreserved: the buffer moves as it grows
      held->emplace_back("held-" + std::to_string(i));
      held->push_back(embed.call("make", i));
      held->push_back(embed.call("make_fn", i));
    }
    for (int i = 0; i < 3; i++) embed.call("churn");
    embed.eval("GC.stat()");
    std::string seen;
    for (size_t i = 0; i < held->size(); i += 3) {
      seen += (*held)[i].as<std::string>() + ",";
      seen += (*held)[i + 1][1].as<std::string>() + ",";
      seen += embed.call("invoke", std::vector<Value>{(*held)[i + 2]})
                  .as<std::string>() + ";";
    }
    ok &= check(seen == "held-0,elem-0,cap-0;held-1,elem-1,cap-1;"
                        "held-2,elem-2,cap-2;held-3,elem-3,cap-3;",
                "Values off the stack keep their values through a collection");

    Value on_stack = (*held)[1];
    ok &= check(pinned(on_stack), "a Value off the stack pins what it holds");
    held->erase(held->begin() + 1);  // the others shift down one
    ok &= check(!pinned(on_stack) && pinned((*held)[1]),
                "one that lets go unpins, and a moved one still holds");
    held.reset();
    ok &= check(!pinned(on_stack), "the stack's own Value pins nothing");

    // The documented call shape: each emplace_back allocates while the earlier
    // arguments sit in the vector's buffer and nowhere else.
    std::vector<Value> args;
    for (int i = 0; i < 4; i++) args.emplace_back("arg-" + std::to_string(i));
    ok &= check(embed.call("show", std::move(args)).as<std::string>() ==
                    "arg-0|arg-1|arg-2|arg-3",
                "arguments built up in a vector arrive as they were made");
  }

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace value_api_smoke_ns
