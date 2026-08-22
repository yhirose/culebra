#pragma once

// The two engines `culebra test` can run a suite on, behind test_runner.h's
// TestHost. Split from that header the way debug_engine.h is split from the
// DAP: everything above the engine — discovery, fixture injection, the
// reporters, the summary — is the same either way, and what differs is a
// handful of questions about values that only the engine holding them can
// answer.
//
// `test` and `parametrize` themselves are neither engine's: they are culebra
// source (src/preambles/test_ambient.cul), so the registry both hosts read
// back was built by the same program.

#include <interpreter.h>
#include <module_loader.h>
#include <stdlib_interp.h>
#include <stdlib_jit.h>
#include <test_runner.h>
#include <vm_session.h>

#include <memory>
#include <string>
#include <vector>

namespace culebra {

// Load one test file, reporting a failure the way the runner reports one.
inline bool load_test_file(const std::string& path, const std::string& source,
                           std::vector<LoadedModule>& modules,
                           TestFileError& err) {
  std::vector<std::string> msgs;
  try {
    ModuleLoader loader;
    modules = loader.load_program(path, source, msgs);
  } catch (const CulebraError& e) {
    err = {e.kind, e.what(), e.line, e.col};
    return false;
  }
  if (modules.empty()) {
    err = {"load_failed", join_messages(msgs), 0, 0};
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The tree-walking interpreter
// ---------------------------------------------------------------------------

class InterpTestHost : public TestHost {
 public:
  InterpTestHost() : env_(environment()) {
    install_cli_aliases(*env_);
  }

  bool begin_run(std::vector<std::string>& msgs) override {
    std::vector<std::string> parse_msgs;
    // Both held for the run: a bound FunctionValue points into the AST, and
    // every token in the AST is a view into the source it was parsed from.
    ambient_source_ = TEST_AMBIENT_MODULE_SOURCE;
    ambient_ = parse_with_transforms(kTestAmbientPath, ambient_source_,
                                     parse_msgs);
    if (!ambient_) {  // the ambient is ours; a parse failure is a build bug
      msgs.insert(msgs.end(), parse_msgs.begin(), parse_msgs.end());
      return false;
    }
    Value val;
    return interpret(ambient_, env_, val, msgs);
  }

  bool run_file(const std::string& path, const std::string& source,
                TestFileError& err) override {
    std::vector<LoadedModule> modules;
    if (!load_test_file(path, source, modules, err)) return false;
    Value val;
    Debugger dbg;
    std::vector<std::string> msgs;
    if (!interpret_modules(modules, env_, val, msgs, dbg)) {
      err = {"interpret_failed", join_messages(msgs), 0, 0};
      return false;
    }
    // A registered function reads its AST tokens out of the module's source
    // buffer, so the modules outlive the run rather than this call.
    for (auto& m : modules) loaded_.push_back(std::move(m));
    return true;
  }

  ValueRef global(std::string_view name) override {
    if (!env_->has(name)) return kNoValue;
    return keep(env_->get(name));
  }
  bool is_function(ValueRef v) override {
    return at(v).type == Value::Function;
  }
  bool is_nil(ValueRef v) override { return at(v).type == Value::Nil; }
  int64_t array_size(ValueRef v) override {
    return static_cast<int64_t>(at(v).to_array().values->size());
  }
  ValueRef array_at(ValueRef v, int64_t i) override {
    return keep((*at(v).to_array().values)[static_cast<size_t>(i)]);
  }
  ValueRef member(ValueRef v, std::string_view name) override {
    const auto& obj = at(v).to_object();
    std::string key(name);
    return keep(obj.has(key) ? obj.get(key) : Value());
  }
  std::string as_string(ValueRef v) override {
    return std::string(at(v).to_string());
  }

  ValueRef call(ValueRef fn, const std::vector<ValueRef>& args) override {
    // The public `call` helper takes a name, so the value is bound to a
    // private slot first — the interpreter's own call internals stay private.
    // One slot, overwritten per call: the runner exits right after.
    static constexpr std::string_view kSlot = "__test_call";
    env_->initialize(kSlot, at(fn), false);
    std::vector<Value> vals;
    vals.reserve(args.size());
    for (auto a : args) vals.push_back(at(a));
    return keep(culebra::call(env_, kSlot, std::move(vals)));
  }

  bool describe_current_throw(std::string& kind,
                              std::string& message) override {
    try {
      throw;
    } catch (const Value& v) {
      // `throw <value>`: keep kind / message when the payload has the
      // conventional shape.
      if (v.type == Value::Object) {
        const auto& obj = v.to_object();
        if (obj.has("kind")) {
          auto kv = obj.get("kind");
          if (kv.type == Value::String) kind = std::string(kv.to_string());
        }
        if (obj.has("message")) {
          auto mv = obj.get("message");
          if (mv.type == Value::String) message = std::string(mv.to_string());
        }
      }
      if (message.empty()) message = v.str_display();
      return true;
    } catch (...) {
      return false;
    }
  }

  size_t mark() override { return store_.size(); }
  void release_to(size_t mark) override { store_.resize(mark); }

 private:
  ValueRef keep(Value v) {
    store_.push_back(std::move(v));
    return static_cast<ValueRef>(store_.size() - 1);
  }
  const Value& at(ValueRef v) { return store_[static_cast<size_t>(v)]; }

  std::shared_ptr<Environment> env_;
  std::string ambient_source_;
  std::shared_ptr<peg::Ast> ambient_;
  std::vector<LoadedModule> loaded_;
  std::vector<Value> store_;
};

// ---------------------------------------------------------------------------
// The bytecode VM's executor
// ---------------------------------------------------------------------------

// Each file compiles as a session unit, so its top-level bindings land in
// vm::ReplSession's cells instead of the frame that ran it. That is what lets
// the runner call back into a file after it has returned: a registered
// closure is still callable, and a fixture is still a name to look up. The
// programs are retained for the same reason the REPL retains its lines.
class VmTestHost : public TestHost {
 public:
  bool begin_run(std::vector<std::string>& msgs) override {
    install_jit_stdlib();
    if (!session_.run_builtin_traits(msgs)) return false;
    std::vector<std::string> parse_msgs;
    auto src = std::make_shared<std::string>(TEST_AMBIENT_MODULE_SOURCE);
    auto ast = parse_with_transforms(kTestAmbientPath, *src, parse_msgs);
    if (!ast) {  // the ambient is ours; a parse failure is a build bug
      msgs.insert(msgs.end(), parse_msgs.begin(), parse_msgs.end());
      return false;
    }
    return run_session_unit(ast, src, msgs);
  }

  bool run_file(const std::string& path, const std::string& source,
                TestFileError& err) override {
    std::vector<LoadedModule> modules;
    if (!load_test_file(path, source, modules, err)) return false;
    if (modules.size() > 1) {
      err = {"VmError", "--vm: unsupported: a test file with imports", 0, 0};
      return false;
    }
    std::vector<std::string> msgs;
    auto& m = modules.front();
    if (!session_.run_stdlib_delta(*m.ast, msgs) ||
        !run_session_unit(m.ast, m.source, msgs)) {
      err = {"interpret_failed", join_messages(msgs), 0, 0};
      return false;
    }
    return true;
  }

  ValueRef global(std::string_view name) override {
    if (!vm::repl_session().declared(name)) return kNoValue;
    return keep_borrowed(vm::repl_session().value(name));
  }
  bool is_function(ValueRef v) override { return at(v).tag == TAG_FUNC; }
  bool is_nil(ValueRef v) override { return at(v).tag == TAG_NIL; }
  int64_t array_size(ValueRef v) override {
    auto* a = reinterpret_cast<JitArray*>(at(v).data);
    return a ? static_cast<int64_t>(a->size) : 0;
  }
  ValueRef array_at(ValueRef v, int64_t i) override {
    auto* a = reinterpret_cast<JitArray*>(at(v).data);
    return keep_borrowed(a->items[static_cast<size_t>(i)]);
  }
  ValueRef member(ValueRef v, std::string_view name) override {
    auto* o = reinterpret_cast<JitObject*>(at(v).data);
    auto slot = o ? o->find_slot(name) : static_cast<size_t>(-1);
    if (slot == static_cast<size_t>(-1))
      return keep_owned(JitValue{TAG_NIL, 0});
    return keep_borrowed(o->slots[slot].value);
  }
  std::string as_string(ValueRef v) override {
    auto val = at(v);
    if (val.tag != TAG_STRING) return {};
    return std::string(_str_sv(reinterpret_cast<const char*>(val.data)));
  }

  ValueRef call(ValueRef fn, const std::vector<ValueRef>& args) override {
    std::vector<JitValue> vals;
    vals.reserve(args.size());
    for (auto a : args) {
      auto v = at(a);
      culebra_runtime_value_retain(v.tag, v.data);  // the callee consumes it
      vals.push_back(v);
    }
    auto* cls = reinterpret_cast<JitClosure*>(at(fn).data);
    // The result arrives owned; the store adopts that reference rather than
    // taking a second one, so a fixture's `drop` fires when release_to drops
    // the last of them.
    return keep_owned(_jit_invoke(cls, JitValue{TAG_NO_SELF, 0},
                             static_cast<int64_t>(vals.size()),
                             vals.empty() ? nullptr : vals.data()));
  }

  bool describe_current_throw(std::string& kind,
                              std::string& message) override {
    try {
      throw;
    } catch (const CulebraException& e) {
      JitValue v{e.tag, e.data};
      if (v.tag == TAG_OBJECT) {
        auto* o = reinterpret_cast<JitObject*>(v.data);
        kind = str_member(o, "kind");
        message = str_member(o, "message");
      }
      if (message.empty()) message = _culebra_uncaught_display(v.tag, v.data);
      return true;
    } catch (...) {
      return false;
    }
  }

  size_t mark() override { return store_.size(); }
  void release_to(size_t mark) override {
    while (store_.size() > mark) {
      auto v = store_.back();
      store_.pop_back();
      _culebra_value_release_impl(v.tag, v.data);
    }
  }

  ~VmTestHost() override { release_to(0); }

 private:
  static std::string str_member(JitObject* o, std::string_view name) {
    auto slot = o ? o->find_slot(name) : static_cast<size_t>(-1);
    if (slot == static_cast<size_t>(-1)) return {};
    auto v = o->slots[slot].value;
    if (v.tag != TAG_STRING) return {};
    return std::string(_str_sv(reinterpret_cast<const char*>(v.data)));
  }

  // A session unit, plus the housekeeping a prompt would do: the value the
  // unit's last statement left in the result cell has no one to echo it here,
  // and leaving it there pins it for the rest of the run.
  bool run_session_unit(const std::shared_ptr<peg::Ast>& ast,
                        std::shared_ptr<std::string> source,
                        std::vector<std::string>& msgs) {
    bool ok = session_.run_unit(ast, std::move(source), /*session=*/true, msgs);
    session_.drop_result();
    return ok;
  }

  ValueRef keep_borrowed(JitValue v) {  // take a reference of our own
    culebra_runtime_value_retain(v.tag, v.data);
    return keep_owned(v);
  }
  ValueRef keep_owned(JitValue v) {  // the store takes the caller's over
    store_.push_back(v);
    return static_cast<ValueRef>(store_.size() - 1);
  }
  JitValue at(ValueRef v) { return store_[static_cast<size_t>(v)]; }

  // Owns the session rather than borrowing the process-wide one: a second
  // run in one process must not see the first's registry, and the interp host
  // owns its environment for the same reason.
  vm::ReplSessionSwap swap_;
  vm::Session session_;
  std::vector<JitValue> store_;
};

// Which engine a run happens on, and the one place that names the two hosts —
// the shape debug_engine.h's make_debug_engine has.
enum class TestEngineKind { Interp, Vm };

inline std::unique_ptr<TestHost> make_test_host(TestEngineKind kind) {
  if (kind == TestEngineKind::Vm) return std::make_unique<VmTestHost>();
  return std::make_unique<InterpTestHost>();
}

}  // namespace culebra
