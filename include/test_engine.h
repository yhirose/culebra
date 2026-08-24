#pragma once

// The engine `culebra test` runs a suite on, behind test_runner.h's
// TestHost. Split from that header the way debug_engine.h is split from the
// DAP: everything above the engine — discovery, fixture injection, the
// reporters, the summary — is the same for any host, and what differs is a
// handful of questions about values that only the engine holding them can
// answer.
//
// `test` and `parametrize` themselves are not the engine's: they are culebra
// source (src/preambles/test_ambient.cul), so the registry the host reads
// back was built by the same program.

#include <module_loader.h>
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
      describe_thrown_value({e.tag, e.data}, kind, message);
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
  // run in one process must not see the first's registry.
  vm::ReplSessionSwap swap_;
  vm::Session session_;
  std::vector<JitValue> store_;
};

inline std::unique_ptr<TestHost> make_test_host() {
  return std::make_unique<VmTestHost>();
}

}  // namespace culebra
