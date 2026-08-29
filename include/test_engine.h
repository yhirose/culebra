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
#include <script_teardown.h>
#include <stdlib_jit.h>
#include <test_runner.h>
#include <vfs.h>  // MainScriptScope — Embed.dir / Sys.script are per program
#include <vm_session.h>

#include <filesystem>
#include <memory>
#include <optional>
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
    // Ctrl+C stops the run, it does not mark this file as broken.
    if (is_interrupt(e)) throw;
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
//
// One such session per file, opened by run_file and dropped by end_file: a
// test file is a program, and what it writes at the top level is its own. A
// shared session made a `mut range = 5` in one file the `range` every later
// file saw.
class VmTestHost : public TestHost {
 public:
  bool begin_run(std::vector<std::string>& msgs) override {
    install_jit_stdlib();
    // Parsed once and run into each file's session: the AST and the buffer its
    // tokens point into are shared, the bindings it makes are not.
    ambient_source_ = std::make_shared<std::string>(TEST_AMBIENT_MODULE_SOURCE);
    std::vector<std::string> parse_msgs;
    ambient_ = parse_with_transforms(kTestAmbientPath, *ambient_source_,
                                     parse_msgs);
    if (!ambient_) {  // the ambient is ours; a parse failure is a build bug
      msgs.insert(msgs.end(), parse_msgs.begin(), parse_msgs.end());
      return false;
    }
    return true;
  }

  bool run_file(const std::string& path, const std::string& source,
                TestFileError& err) override {
    std::vector<LoadedModule> modules;
    if (!load_test_file(path, source, modules, err)) return false;
    // The error_code overload, as the script lane uses: a path the filesystem
    // will not answer for should fail this one file, not the run.
    std::error_code ec;
    auto abs = std::filesystem::absolute(path, ec);
    unit_.emplace(ec ? path : abs.string());
    std::vector<std::string> msgs;
    // Ctrl+C stops the run rather than marking this file broken and moving on
    // to the next: the interrupt is one-shot, so without this the rest of the
    // suite would run as if nothing had been pressed. Both failure exits go
    // through it — the ambient prologue can be interrupted too.
    auto fail = [&](const char* kind) {
      if (const auto& i = unit_->session.last_interrupt()) throw *i;
      err = {kind, join_messages(msgs), 0, 0};
      return false;
    };
    if (!unit_->session.run_builtin_traits(msgs) ||
        !run_session_unit(ambient_, ambient_source_, msgs)) {
      return fail("internal_error");
    }
    // A file that imports is its whole module list as one session unit;
    // run_modules asks for the stdlib the list names itself.
    auto& m = modules.front();
    bool ok = modules.size() > 1
                  ? run_session_modules(modules, msgs)
                  : unit_->session.run_stdlib_delta(*m.ast, msgs) &&
                        run_session_unit(m.ast, m.source, msgs);
    if (!ok) return fail("interpret_failed");
    return true;
  }

  // The file's values first, then the session holding the cells they lived in.
  void end_file() override {
    release_to(0);
    unit_.reset();
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

  // The runner closes every file it opens, so this is only for a host
  // destroyed unrun; `store_` is a plain vector whose destructor releases
  // nothing, so it has to be drained before the Runtime that owns its values.
  ~VmTestHost() override { end_file(); }

 private:
  // One file's scope: its own Runtime — where the namespace caches and the
  // class / overload registries live, which is why a doc block gets one too —
  // and its own session cells on top of it.
  //
  // Declared so that teardown runs backwards through that. The file's threads
  // are joined FIRST, since an isolate still running would otherwise outlive
  // the Runtime it allocates in. Then the names are handed back, because a
  // cell's last reference can run culebra code (a `drop` body) and a closure
  // reaches its bytecode through a descriptor pointing into a retained program
  // — so both the programs and the Runtime must outlive the cells holding them.
  struct Unit {
    explicit Unit(const std::string& path) : script(path) {}
    Runtime rt;
    RuntimeScope scope{rt};
    // `Sys.script` and Embed.dir's base. Before the session, so a `drop`
    // body running as the cells are handed back still sees its own file's.
    MainScriptScope script;
    vm::Session session;
    vm::ReplSessionSwap names;  // makes its ReplSession the current one
    ScriptTeardownGuard threads;
  };

  // A session unit, plus the housekeeping a prompt would do: the value the
  // unit's last statement left in the result cell has no one to echo it here,
  // and leaving it there pins it for the rest of the file.
  bool run_session_unit(const std::shared_ptr<peg::Ast>& ast,
                        std::shared_ptr<std::string> source,
                        std::vector<std::string>& msgs) {
    bool ok =
        unit_->session.run_unit(ast, std::move(source), /*session=*/true, msgs);
    unit_->session.drop_result();
    return ok;
  }

  bool run_session_modules(const std::vector<LoadedModule>& modules,
                           std::vector<std::string>& msgs) {
    bool ok = unit_->session.run_modules(modules, msgs);
    unit_->session.drop_result();
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

  std::shared_ptr<std::string> ambient_source_;
  std::shared_ptr<peg::Ast> ambient_;
  std::optional<Unit> unit_;
  std::vector<JitValue> store_;
};

inline std::unique_ptr<TestHost> make_test_host() {
  return std::make_unique<VmTestHost>();
}

}  // namespace culebra
