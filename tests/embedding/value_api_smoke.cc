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

// Everything the surface touches lives here, so `run` can audit the heap
// once every Value and the session itself are gone.
bool body() {
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

  return ok;
}

}  // namespace

int run() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  bool ok = body();
  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace value_api_smoke_ns
