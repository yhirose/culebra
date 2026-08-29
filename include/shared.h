#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <numeric>
#include <csignal>
#include <cctype>
#include <mutex>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <random>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdio>    // Windows stdin fallback (read_stdin_*_interruptible)
#include <thread>    // SizedThread's Windows arm and its std::thread surface
#if !defined(_WIN32)
#include <unistd.h>   // read (interruptible stdin)
#include <poll.h>     // interruptible stdin (poll fd 0 between interrupt checks)
#include <pthread.h>  // SizedThread (explicit stack size)
#endif
#if defined(_WIN32)
#include <os_compat.h>  // <windows.h> (guarded)
#endif

#include <rt_format.h>

#include <unicodelib_encodings.h>  // unicode::utf8::encode_codepoint

#include "exe_path.h"  // current_executable_path (Sys.executable)

namespace culebra {

// Names of value-type built-in methods (Array / String / Set / Tuple / Object-
// dict / Iterator / Tensor). These are dispatched inline by the JIT (no stored
// closure) and table-wrapped by the interp, so a BARE reference to one
// (`let m = [1,2].map`) is not a first-class value on either backend — both
// reject it (call it, or wrap it in a lambda). A user-defined property/method
// of the same name on an Object/class is found first and stays first-class, so
// this set is only consulted after the stored-property lookup misses. Single
// source for both backends' bare-method-reference check AND the startup
// drift self-check in culebra.h.
inline const std::unordered_set<std::string_view>& builtin_method_names() {
  static const std::unordered_set<std::string_view> kNames = {
      "size",       "empty",       "presence",   "push",       "pop",
      "reverse",
      "extend",     "insert",      "remove_at",
      "slice",      "join",        "index_of",   "contains",   "upper",
      "lower",
      "trim",       "tr",          "trim_start", "trim_end",   "split",
      "repeat",     "truncate",    "capitalize", "lines",      "to_string",
      "starts_with","ends_with",   "keys",       "values",     "has",
      "remove",     "get",         "get_or_put", "map",        "filter",
      "reduce",     "for_each",    "find",
      "any",        "all",         "flat_map",   "sort_by",    "sorted_by",
      "sort",       "sorted",
      "sum",        "product",     "min",        "max",        "collect",
      "min_by",     "max_by",     "to_set",     "to_object",  "group_by",
      "partition",
      "count",      "take",        "skip",       "take_while", "chain",
      "zip",        "enumerate",   "code_points","graphemes",  "iter",
      "chunks",     "windows",     "skip_while", "first",      "last",
      "nth",        "position",    "flatten",    "scan",       "distinct",
      "tap",        "step_by",     "chunk_by",   "unzip",
      "bytes",
      "view",       "split_iter",  "shape",      "pow",        "transpose",
      "reshape",    "mean",        "argmax",     "to_array",   "dot",
      "linear_sigmoid", "clone",   "relu",       "sigmoid",    "softmax",
      "log",
      "add",
      "union",      "intersect",   "diff",       "sym_diff",   "subset",
      "superset",   "requires_grad","grad",      "backward",   "zero_grad",
      "detach",     "item",
      "last_index_of", "strip_prefix", "strip_suffix", "rsplit",
      "split_whitespace", "normalize", "title", "eq_ignore_case",
      "is_digit",   "is_alpha",    "is_alnum",   "is_space",   "is_ascii"};
  return kNames;
}
inline bool is_builtin_method_name(std::string_view name) {
  return builtin_method_names().count(name) > 0;
}

// The subset of builtin methods that apply to a plain Object/dict receiver.
// A builtin namespace (IO, FS, ...) reads its unknown members as an
// AttributeError, EXCEPT these: `IO.keys()` / `Sys.has("script")` and the
// like still dispatch the dict builtin. Array/Set/Tensor/String method names
// are deliberately excluded so `IO.push()` / `FS.split()` raise.
inline bool is_object_builtin_method_name(std::string_view name) {
  static const std::unordered_set<std::string_view> kNames = {
      "size",  "empty", "presence",   "keys",   "has", "get", "get_or_put",
      "remove", "values", "iter"};
  return kNames.count(name) > 0;
}

// The culebra-source stdlib modules (src/preambles/*.cul) whose public binding
// is a namespace object, so their unknown members raise like a C++ namespace's
// instead of reading as nil. `Path` is absent on purpose: its module returns a
// class, and class property misses stay nil (user classes included). `replace`
// and the matcher family bind bare functions, not objects.
//
// Returns the module's static name, which the tagging sites keep as
// ObjectValue::ns_name / JitObject::ns_name for the error message.
inline const char* lazy_namespace_static_name(std::string_view name) {
  static constexpr const char* kNames[] = {"Time",  "Term", "Canvas",  "Args",
                                           "Regex", "Log",  "Desktop", "__Eff"};
  for (const char* n : kNames)
    if (name == n) return n;
  return nullptr;
}

// The culebra-source stdlib modules that bind bare *functions* instead of a
// namespace object: the matcher family (matchers.cul) and the String helpers
// (string_fns.cul). Both backends must hand out one instance of each per
// Runtime, so a function reached from two modules compares equal on either.
// The interp binds a group's names once in the global environment
// (initialize_lazy_group); the JIT/AOT wrap the same source in a builder that
// returns `{name: name, ...}`, register it under the group name, and resolve
// every bare reference to the member the group built (_jit_lazy_fn_closure).
// The group name is internal — user code never writes it, so it is not a
// builtin global on either side.
struct LazyFnGroup {
  std::string_view name;
  std::span<const std::string_view> members;
};

inline std::span<const LazyFnGroup> lazy_fn_groups() {
  // Only assert_throws is lazy source now; the comparison matchers are
  // native globals in kBuiltinFns (they need the call site's position).
  static constexpr std::string_view kMatchers[] = {"assert_throws"};
  static constexpr std::string_view kStringFns[] = {"replace", "replace_first",
                                                    "split_once",
                                                    "rsplit_once"};
  static constexpr LazyFnGroup kGroups[] = {{"__Matchers", kMatchers},
                                            {"__StringFns", kStringFns}};
  return kGroups;
}

// The group `name` belongs to, or empty when it is not a bare stdlib function.
inline std::string_view lazy_fn_group_of(std::string_view name) {
  for (const auto& g : lazy_fn_groups()) {
    for (auto m : g.members)
      if (m == name) return g.name;
  }
  return {};
}

// Transparent hash/eq for std::string-keyed unordered_map that allows
// std::string_view lookups without constructing a temporary std::string
// on every find (C++20 heterogeneous lookup needs is_transparent on
// both the hash and the equality functor).
struct sv_hash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};
struct sv_equal {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

// --- Structured runtime error ---

// Records a just-constructed CulebraError into the current Runtime's pending
// carrier (defined after Runtime is complete). The JIT try/catch pad reads that
// carrier instead of re-inspecting the in-flight C++ exception with `throw;` —
// the portable path for Windows SEH funclet EH. Declared here so the ctor below
// can call it; a no-op for anything that ignores the carrier (the interpreter).
inline void culebra_note_pending_error(const std::string& kind,
                                       const std::string& msg, int64_t line,
                                       int64_t col);

// Both backends throw this; try/catch machinery translates it into a
// culebra Object with `kind`/`message`/`line`/`col` fields. Inherits
// from std::runtime_error so unconverted call sites still work via
// the standard exception interface.
class CulebraError : public std::runtime_error {
 public:
  std::string kind;
  int64_t line = 0;   // int64_t: positions round-trip through Value Longs
  int64_t col = 0;

  CulebraError(std::string k, std::string msg, int64_t l = 0, int64_t c = 0)
      : std::runtime_error(std::move(msg)),
        kind(std::move(k)),
        line(l),
        col(c) {
    // Publish this error as the current pending one at construction — which
    // immediately precedes the throw at every call site — so a JIT catch pad
    // can materialize it from the carrier without a `throw;` re-inspection.
    culebra_note_pending_error(kind, what(), line, col);
  }
  // Copy/move must publish too: an isolate re-surfaces a producer's stored error
  // at join() with `throw *stored`, which COPIES — and that copy is what unwinds,
  // so the JIT catch pad must find it in the carrier exactly as a fresh throw
  // would. Assignment is plain storage (not a throw), so it stays defaulted.
  CulebraError(const CulebraError& o)
      : std::runtime_error(o), kind(o.kind), line(o.line), col(o.col) {
    culebra_note_pending_error(kind, what(), line, col);
  }
  CulebraError(CulebraError&& o)
      : std::runtime_error(std::move(o)),
        kind(std::move(o.kind)),
        line(o.line),
        col(o.col) {
    culebra_note_pending_error(kind, what(), line, col);
  }
  CulebraError& operator=(const CulebraError&) = default;
  CulebraError& operator=(CulebraError&&) = default;
};

// Re-publish an in-flight error into the pending carrier after a backfill catch
// has adjusted its position (a runtime site that stamps its own line/col onto a
// positionless error and rethrows). Keeps the carrier the JIT pad reads in sync
// with the exception object. One place maps a CulebraError to the carrier.
inline void culebra_note_pending_error(const CulebraError& e) {
  culebra_note_pending_error(e.kind, e.what(), e.line, e.col);
}

// The one spelling of an uncaught error: "Kind: msg", plus " at L:C." when
// the error carries a position. Every engine and runner prints this same
// text — doctest `# !!` patterns match against it, so a reworded copy in one
// lane would split doc-block results between engines.
inline std::string format_error_message(const CulebraError& e) {
  if (e.line <= 0 && e.col <= 0)
    return culebra::format("{}: {}", e.kind, e.what());
  // A message that runs to several lines — the matchers put each operand on
  // its own — would otherwise end "...  right: bar at 3:1.", where the
  // position reads as part of the last value. Lead with it instead.
  std::string_view msg{e.what()};
  if (msg.find('\n') != std::string_view::npos)
    return culebra::format("{} at {}:{}: {}", e.kind, e.line, e.col, msg);
  return culebra::format("{}: {} at {}:{}.", e.kind, e.what(), e.line, e.col);
}

// Diagnostics joined into one "; "-separated line — the text the doctest
// and unit-test runners report a failed parse as.
inline std::string join_messages(const std::vector<std::string>& msgs) {
  std::string joined;
  for (const auto& m : msgs) {
    if (!joined.empty()) joined += "; ";
    joined += m;
  }
  return joined;
}

// --- Call-binding error messages (single source for both backends) ---
//
// Builders for the error texts the argument binder can produce. The
// throw-form helpers below use them, but so do the sites that cannot
// throw directly — the JIT's compile-time Err records, emit_throw_error
// (deferred runtime throws baked into IR), and runtime slow paths that
// must release owned values first. Any independent std::format of these
// texts is a symmetry hazard: route new sites through a builder.

// "takes N positional argument(s) but M given" — a call overflowed the
// positional cap of a kw-only section.
inline std::string too_many_positionals_message(int64_t cap, int64_t got) {
  return culebra::format("takes {} positional argument{} but {} given", cap,
                         cap == 1 ? "" : "s", got);
}

// "missing required argument 'name'" — no positional, no kwarg, no default.
inline std::string missing_required_arg_message(std::string_view name) {
  return culebra::format("missing required argument '{}'", name);
}

// "unknown keyword argument 'name'" — a kwarg the callee doesn't accept.
inline std::string unknown_kwarg_message(std::string_view name) {
  return culebra::format("unknown keyword argument '{}'", name);
}

// The keyword bindings a call resolved, in the order they arrived: `**`
// splat entries in operand order, then the explicit keywords, a repeat
// overwriting the value where the name first landed. A `**rest` catch-all
// hands this straight to the callee as an Object, and the language's objects
// are ordered — so the container must be too. Both binders build one of
// these; a hash map gave each backend its own permutation of the same keys.
// A call's keyword count is small, so the linear scan IS the lookup.
template <typename V>
struct MergedKwargs {
  std::vector<std::pair<std::string_view, V>> items;
  using iterator = typename std::vector<std::pair<std::string_view, V>>::iterator;
  using const_iterator =
      typename std::vector<std::pair<std::string_view, V>>::const_iterator;
  iterator begin() { return items.begin(); }
  iterator end() { return items.end(); }
  const_iterator begin() const { return items.begin(); }
  const_iterator end() const { return items.end(); }
  iterator find(std::string_view k) {
    return std::find_if(items.begin(), items.end(),
                        [&](const auto& e) { return e.first == k; });
  }
  bool contains(std::string_view k) { return find(k) != items.end(); }
  // Insert, or overwrite in place so the name keeps the position it first
  // arrived at.
  void set(std::string_view k, V v) {
    auto it = find(k);
    if (it == items.end())
      items.emplace_back(k, std::move(v));
    else
      it->second = std::move(v);
  }
  void erase(iterator it) { items.erase(it); }
  void clear() { items.clear(); }
  bool empty() const { return items.empty(); }
};

// Among leftover kwargs the callee can't accept, pick a backend-independent
// one to name in the error. Every backend collects leftovers in a name-keyed
// map, but the iteration order differs by backend and container (unordered_map
// vs map), so naming `merged.begin()` would let the reported kwarg drift
// between interp and JIT for an otherwise-identical call. Return the
// lexicographically smallest name so all backends report the same kwarg — do
// not "simplify" this back to `begin()`. `merged` must be non-empty.
template <typename Map>
std::string_view canonical_unknown_kwarg(const Map& merged) {
  std::string_view best = merged.begin()->first;
  for (const auto& entry : merged) {
    if (entry.first < best) best = entry.first;
  }
  return best;
}

// "got argument 'name' both positionally and as a keyword".
inline std::string positional_kw_conflict_message(std::string_view name) {
  return culebra::format("got argument '{}' both positionally and as a keyword",
                         name);
}

// "type error: expected X, got Y" — an argument/receiver type mismatch.
inline std::string type_mismatch_message(std::string_view expected,
                                         std::string_view got) {
  return culebra::format("type error: expected {}, got {}", expected, got);
}

// Throw TypeError "takes N positional argument(s) but M given" when M
// exceeds the cap. `cap < 0` means no cap (no kw-only section, or a
// `*args` catch-all that swallows overflow — callers pass -1 for both).
// `cap == 0` is a real cap: the first param is keyword-only, so zero
// positionals are accepted and any positional overflows. Shared by
// interp's bind_call_args, the JIT static kwargs resolver, and the JIT
// dynamic-callee runtime guard — all three throw the same shape.
inline void throw_if_too_many_positionals(int64_t cap, int64_t n_pos,
                                           int64_t line, int64_t col) {
  if (cap < 0 || n_pos <= cap) return;
  throw CulebraError("TypeError", too_many_positionals_message(cap, n_pos),
                     line, col);
}

// --- recursion guard --------------------------------------------------
// One logical culebra call = one unit, counted identically by the interp
// (the user-body eval closures — make_function_value and the two ctor
// shapes) and by the JIT/AOT (the compiled-function prologue), so
// `RecursionError` fires at the same depth on every backend. Without it the
// two backends overflow the C stack at wildly different depths (interp
// ~4.7KB of eval frames per call, JIT ~350B) and die as an uncatchable
// SIGSEGV. 1000 matches CPython's default; the deepest recursion in the
// test corpus is ~614. 1000 interp frames measure 6.6MB (gcc-14 -O3+LTO,
// x86-64) and more under clang, so every thread that runs user code is
// given 16MB: SizedThread below sizes the ones it spawns, and CMakeLists
// reserves it at link time for the main thread, which neither can size
// (macOS -stack_size, Windows PE reserve).
inline constexpr int64_t kCulebraRecursionLimit = 1000;
inline thread_local int64_t _culebra_call_depth = 0;

inline void check_recursion_depth(int64_t line, int64_t col) {
  if (_culebra_call_depth >= kCulebraRecursionLimit) {
    throw CulebraError(
        "RecursionError",
        culebra::format("maximum recursion depth exceeded ({})",
                        kCulebraRecursionLimit),
        line, col);
  }
}

// Wording shared by every nesting bound (PEG rule depth, JSON data depth);
// toml.h keeps a value-neutral copy its backends reuse.
inline std::string nesting_too_deep_message(int64_t limit) {
  return culebra::format("nesting too deep (limit {})", limit);
}

// RAII frame for the interp's C++-recursive eval: the dtor runs while an
// exception unwinds, so every abandoned frame decrements as it is passed —
// the depth a `catch` observes matches the JIT, whose compiled frames
// restore the count at cleanup pads / catch entry instead.
struct RecursionFrame {
  RecursionFrame(int64_t line, int64_t col) {
    check_recursion_depth(line, col);
    ++_culebra_call_depth;
  }
  ~RecursionFrame() { --_culebra_call_depth; }
  RecursionFrame(const RecursionFrame&) = delete;
  RecursionFrame& operator=(const RecursionFrame&) = delete;
};

// Depth of the current *value* walk — the C++-recursive traversals that
// follow a value's own nesting (str/inspect, ==, hash, sendable serialize)
// rather than culebra calls. A loop can build `a = [a]` deeper than any
// parse, so every walker bounds its descent here and fails as a catchable
// ValueError instead of overflowing the C stack. One shared counter, not
// one per walker: nested walks (a `__str__` that inspects, eq inside a Set
// probe) stack on the same C stack and must share the budget.
inline thread_local int64_t _culebra_value_walk_depth = 0;

struct ValueWalkFrame {
  ValueWalkFrame() {
    if (_culebra_value_walk_depth >= kCulebraRecursionLimit) {
      throw CulebraError("ValueError",
                         nesting_too_deep_message(kCulebraRecursionLimit));
    }
    ++_culebra_value_walk_depth;
  }
  ~ValueWalkFrame() { --_culebra_value_walk_depth; }
  ValueWalkFrame(const ValueWalkFrame&) = delete;
  ValueWalkFrame& operator=(const ValueWalkFrame&) = delete;
};

// Deallocation deferral threshold (CPython's trashcan): a container release
// nested deeper than this moves its work to a thread-local list drained at
// the outermost level, so dropping `a = [a]`×100k never overflows the C
// stack — a dtor cannot throw, so unlike the walkers above this bound has
// to be structural. ~84B/level (interp ~Value chain) keeps the in-stack
// portion under ~42KB. Consumers: interp ~Value, JIT value release,
// tensor.h's ~TensorImpl (the autograd tape's own linear parent->input
// chain).
inline constexpr int64_t kValueTeardownDepthBudget = 500;

// The trashcan itself. `Release` tears one payload down in place — `reset()`
// on interp's std::any, `clear()` on a tensor node's input vector, a refcount
// decrement for the JIT's tagged handle — and re-enters this class through
// whatever recursion that teardown drives. Callers do their own early exit
// (a scalar Value, a leaf tensor, a non-refcounted tag) before calling in, so
// the budget check only runs for a payload that can chain.
template <class Payload, void (*Release)(Payload&)>
class Trashcan {
  // How a payload crosses a call boundary. A trivially copyable one (the
  // JIT's tagged word) goes by value so the outlined defer takes it in
  // registers; passing it by reference instead makes the caller materialize
  // it on the stack, which pulls a stack-protector canary onto the hottest
  // release path (measured: +7 instructions there). One with a real move (an
  // any, a vector) goes by rvalue reference so no temporary is built at all.
  using Arg = std::conditional_t<std::is_trivially_copyable_v<Payload>,
                                 Payload, Payload&&>;

 public:
  // Tear `p` down at the current depth, or park it for the outermost level
  // once the chain is deeper than the budget.
  static void release(Arg p) {
    State& st = state();
    if (st.depth >= kValueTeardownDepthBudget) {
      defer(std::move(p));
      return;
    }
    ++st.depth;
    Release(p);
    if (st.depth == 1 && st.has_deferred) drain();
    --st.depth;
  }

 private:
  // Two statics, not one bundle: `depth`/`has_deferred` are POD with constant
  // initializers, so the compiler skips the thread-safe-init guard entirely
  // (verified via -O2 disassembly: a single %fs-relative instruction, no
  // branch) — that is what keeps every leaf/shallow teardown free. `deferred`
  // holds a vector, whose non-trivial destructor forces a real guard check;
  // folding it into the same struct would spread that guard onto the
  // depth/has_deferred reads too, on every teardown.
  //
  // Function-local statics, not namespace-scope `inline thread_local`: the
  // consumers live in headers shared between the core runtime archive and the
  // feature archives, where a namespace-scope thread_local with dynamic
  // initialization needs the CULEBRA_RT_CORE_OWNED split rt_shared_tls.h
  // describes. A function-local static's guard is per-inline-function and
  // merges under ODR without that machinery.
  struct State {
    int64_t depth = 0;
    bool has_deferred = false;
  };
  static State& state() {
    thread_local State s;
    return s;
  }
  static std::vector<Payload>& deferred() {
    thread_local std::vector<Payload> d;
    return d;
  }

  // Outlined so `release`'s inline body stays small at its many expansion
  // sites; `has_deferred` mirrors !deferred().empty() so the fast path never
  // touches the guarded vector.
  [[gnu::noinline]] static void defer(Arg p) {
    deferred().push_back(std::move(p));
    state().has_deferred = true;
  }

  [[gnu::noinline]] static void drain() {
    // Runs at depth 1 (see release), so every teardown it triggers bottoms
    // out at depth >= 2 and can never nest a second drain. Each pop may
    // re-defer its own deep tail; loop until dry.
    auto& d = deferred();
    while (!d.empty()) {
      Payload p = std::move(d.back());
      d.pop_back();
      release(std::move(p));
    }
    state().has_deferred = false;
  }
};

// A thread for running culebra code: std::thread's join surface, but with
// an explicit 16MB stack on POSIX. macOS gives non-main pthreads 512KB by
// default — barely 100 interp eval frames — while the recursion limit above
// needs 6.6MB of headroom and more from a wider-framed compiler. Windows
// threads inherit the PE stack reserve (set at link time to the same 16MB),
// so the std::thread arm is already right there; Linux pthreads default to
// the rlimit (8MB), which the explicit size lifts. IO-only threads (pipe
// drains, accept loops) stay plain std::thread.
class SizedThread {
 public:
  static constexpr size_t kStackBytes = 16ull << 20;

  SizedThread() = default;

  template <class F>
  explicit SizedThread(F f) {
#if !defined(_WIN32)
    auto* fn = new std::function<void()>(std::move(f));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    if (pthread_create(&handle_, &attr, &run_boxed, fn) == 0) {
      joinable_ = true;
    } else {
      delete fn;
    }
    pthread_attr_destroy(&attr);
    if (!joinable_) throw std::runtime_error("SizedThread: pthread_create failed");
#else
    thread_ = std::thread(std::move(f));
#endif
  }

  SizedThread(SizedThread&& o) noexcept { swap(o); }
  SizedThread& operator=(SizedThread&& o) noexcept {
    if (this != &o) {
      // std::thread semantics: assigning over a joinable thread terminates.
      if (joinable()) std::terminate();
      swap(o);
    }
    return *this;
  }
  SizedThread(const SizedThread&) = delete;
  SizedThread& operator=(const SizedThread&) = delete;

  ~SizedThread() {
    // Same contract as std::thread: destroying a joinable thread is a bug.
    if (joinable()) std::terminate();
  }

  bool joinable() const {
#if !defined(_WIN32)
    return joinable_;
#else
    return thread_.joinable();
#endif
  }

  void join() {
#if !defined(_WIN32)
    if (joinable_) {
      pthread_join(handle_, nullptr);
      joinable_ = false;
    }
#else
    thread_.join();
#endif
  }

 private:
#if !defined(_WIN32)
  static void* run_boxed(void* p) {
    std::unique_ptr<std::function<void()>> fn(
        static_cast<std::function<void()>*>(p));
    (*fn)();
    return nullptr;
  }
  void swap(SizedThread& o) noexcept {
    std::swap(handle_, o.handle_);
    std::swap(joinable_, o.joinable_);
  }
  pthread_t handle_{};
  bool joinable_ = false;
#else
  void swap(SizedThread& o) noexcept { std::swap(thread_, o.thread_); }
  std::thread thread_;
#endif
};

// Count-based ArityError message for a wrong-arity built-in method call,
// shared by both backends so interp/JIT/AOT emit byte-identical text. A
// fixed arity renders `'push' takes 1 argument but 3 given`; an optional
// range renders `'slice' takes 1 to 2 arguments but 3 given`. The `given`
// count drives nothing; `min`/`max` drive the singular/plural of the
// expected noun (no period — the printer appends ` at L:C.`).
inline std::string builtin_arity_error_message(std::string_view method,
                                               long min, long max, int64_t got) {
  if (min == max) {
    return culebra::format("'{}' takes {} argument{} but {} given", method, min,
                           min == 1 ? "" : "s", got);
  }
  return culebra::format("'{}' takes {} to {} arguments but {} given", method, min,
                         max, got);
}

// A built-in method called with keyword arguments. Built-ins bind their args
// positionally, so any keyword is a TypeError; shared so interp and both
// backends emit byte-identical text.
inline std::string builtin_method_kwargs_error_message(std::string_view method) {
  return culebra::format("built-in method '{}' does not accept keyword arguments",
                         method);
}

// Count-based arity error for a built-in *function* (namespace method or
// bare global) invoked as a value with the wrong number of positional args:
// "expected N positional argument(s), got M". Deliberately nameless: the
// interpreter does not carry the qualified name on these FunctionValues, so a
// nameless message lets both backends render byte-identical text for
// `Math.abs(1, 2)` and `let f = Math.abs; f(1, 2)` alike. Distinct from
// builtin_arity_error_message, which names value-type *methods*.
inline std::string ns_fn_arity_error_message(int64_t expected, int64_t got) {
  return culebra::format("expected {} positional argument{}, got {}", expected,
                         expected == 1 ? "" : "s", got);
}

// --- Numeric formatting / parsing ---

// Shortest round-trip decimal for a double, with a forced decimal point
// or exponent so Float display is visually distinguishable from Long
// (`1.0`, `0.5`, `-2.5`, `1e-05`, `nan`, `inf`, `-inf`). Shared by the
// interpreter's `Value::str_float` and the JIT's `_culebra_value_to_str_impl`.
inline std::string format_float_shortest(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buf[32];
  auto* end =
      std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general).ptr;
  std::string s(buf, end);
  if (s.find('.') == std::string::npos &&
      s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

// JSON.stringify quote escaping. Shared by both backends' stringify
// implementations. Control bytes (<0x20) emit as `\u00xx`.
inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

// Whether `cp` is a Unicode scalar value: <= U+10FFFF and not a surrogate
// (U+D800-U+DFFF). Same boundary the `\u`/`\U` string-literal escapes enforce
// (parser.h::decode_unicode_escape_u/_U) — `String.from_code_point` reuses it
// so both ways of turning a number into a codepoint agree on what's valid.
// Takes int64_t so a negative code point is rejected here rather than
// wrapping into the valid range.
inline bool is_unicode_scalar_value(int64_t cp) {
  return cp >= 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// Append code point `cp` to `out` as UTF-8. **Precondition: `cp` is a scalar
// value** — everything that turns a number into a code point checks first and
// raises (`string_from_code_point`, `append_checked_code_point`, parser.h's
// `\u`/`\U` escapes, toml.h's). A non-scalar appends nothing.
inline void append_utf8(std::string& out, uint32_t cp) {
  unicode::utf8::encode_codepoint(static_cast<char32_t>(cp), out);
}

// `String.from_code_point(cp)` — the runtime counterpart of the `\u`/`\U`
// literal escapes, single-sourced here for interp + JIT. Raises ValueError
// (not SyntaxError — this is a runtime call, not a parse error) on the same
// boundary the escape rejects at parse time.
inline std::string string_from_code_point(int64_t cp) {
  if (!is_unicode_scalar_value(cp)) {
    throw CulebraError("ValueError", culebra::format(
        "String.from_code_point: {} is not a Unicode scalar value.", cp));
  }
  std::string out;
  append_utf8(out, static_cast<uint32_t>(cp));
  return out;
}

// Element-level helper for `String.from_bytes(bytes)` — the inverse of
// `.bytes()`. Appends one raw byte (no UTF-8 validation: culebra Strings
// tolerate invalid UTF-8, same as `.iter()` — see docs/language.md's
// `String.from_bytes` entry). Raises ValueError for a value outside 0-255;
// single-sourced here for interp + JIT.
inline void append_checked_byte(std::string& out, int64_t b) {
  if (b < 0 || b > 255) {
    throw CulebraError("ValueError", culebra::format(
        "String.from_bytes: {} is not a byte (0-255).", b));
  }
  out += static_cast<char>(b);
}

// Element-level helper for `String.from_code_points(cps)` — the plural
// inverse of `.code_points()`. Each element passes through the same
// `is_unicode_scalar_value` gate as `string_from_code_point`, so
// `String.from_code_points([cp]) == String.from_code_point(cp)`.
inline void append_checked_code_point(std::string& out, int64_t cp) {
  if (!is_unicode_scalar_value(cp)) {
    throw CulebraError("ValueError", culebra::format(
        "String.from_code_points: {} is not a Unicode scalar value.", cp));
  }
  append_utf8(out, static_cast<uint32_t>(cp));
}

// HTML escape: the five characters that are unsafe in HTML text/attributes.
// `&` is replaced first so the others' replacements aren't re-escaped.
inline std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// Named character references covered by html_unescape. Not the full HTML5
// table (~2200 entries) — a practical set of the common typographic, Latin-1,
// Greek, math, and currency entities. Unknown names are left verbatim.
inline const std::unordered_map<std::string_view, std::string_view, sv_hash,
                                std::equal_to<>>&
html_named_entities() {
  static const std::unordered_map<std::string_view, std::string_view, sv_hash,
                                  std::equal_to<>> table = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
    {"nbsp", " "}, {"shy", "­"},
    {"copy", "©"}, {"reg", "®"}, {"trade", "™"},
    {"hellip", "…"}, {"mdash", "—"}, {"ndash", "–"},
    {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"},
    {"rdquo", "”"}, {"sbquo", "‚"}, {"bdquo", "„"},
    {"laquo", "«"}, {"raquo", "»"}, {"lsaquo", "‹"},
    {"rsaquo", "›"}, {"bull", "•"}, {"middot", "·"},
    {"dagger", "†"}, {"Dagger", "‡"}, {"prime", "′"},
    {"Prime", "″"}, {"permil", "‰"},
    {"emsp", " "}, {"ensp", " "}, {"thinsp", " "},
    {"deg", "°"}, {"plusmn", "±"}, {"times", "×"},
    {"divide", "÷"}, {"frac12", "½"}, {"frac14", "¼"},
    {"frac34", "¾"}, {"sup1", "¹"}, {"sup2", "²"},
    {"sup3", "³"}, {"micro", "µ"}, {"not", "¬"},
    {"sect", "§"}, {"para", "¶"}, {"iexcl", "¡"},
    {"iquest", "¿"}, {"ordf", "ª"}, {"ordm", "º"},
    {"euro", "€"}, {"pound", "£"}, {"yen", "¥"},
    {"cent", "¢"}, {"curren", "¤"},
    {"larr", "←"}, {"uarr", "↑"}, {"rarr", "→"},
    {"darr", "↓"}, {"harr", "↔"},
    {"infin", "∞"}, {"ne", "≠"}, {"le", "≤"}, {"ge", "≥"},
    {"asymp", "≈"}, {"equiv", "≡"}, {"sum", "∑"},
    {"prod", "∏"}, {"radic", "√"}, {"part", "∂"},
    {"nabla", "∇"}, {"int", "∫"}, {"minus", "−"},
    {"agrave", "à"}, {"aacute", "á"}, {"acirc", "â"},
    {"atilde", "ã"}, {"auml", "ä"}, {"aring", "å"},
    {"aelig", "æ"}, {"ccedil", "ç"}, {"egrave", "è"},
    {"eacute", "é"}, {"ecirc", "ê"}, {"euml", "ë"},
    {"igrave", "ì"}, {"iacute", "í"}, {"icirc", "î"},
    {"iuml", "ï"}, {"eth", "ð"}, {"ntilde", "ñ"},
    {"ograve", "ò"}, {"oacute", "ó"}, {"ocirc", "ô"},
    {"otilde", "õ"}, {"ouml", "ö"}, {"oslash", "ø"},
    {"ugrave", "ù"}, {"uacute", "ú"}, {"ucirc", "û"},
    {"uuml", "ü"}, {"yacute", "ý"}, {"thorn", "þ"},
    {"yuml", "ÿ"}, {"szlig", "ß"},
    {"Agrave", "À"}, {"Aacute", "Á"}, {"Acirc", "Â"},
    {"Atilde", "Ã"}, {"Auml", "Ä"}, {"Aring", "Å"},
    {"AElig", "Æ"}, {"Ccedil", "Ç"}, {"Egrave", "È"},
    {"Eacute", "É"}, {"Ecirc", "Ê"}, {"Euml", "Ë"},
    {"Igrave", "Ì"}, {"Iacute", "Í"}, {"Icirc", "Î"},
    {"Iuml", "Ï"}, {"ETH", "Ð"}, {"Ntilde", "Ñ"},
    {"Ograve", "Ò"}, {"Oacute", "Ó"}, {"Ocirc", "Ô"},
    {"Otilde", "Õ"}, {"Ouml", "Ö"}, {"Oslash", "Ø"},
    {"Ugrave", "Ù"}, {"Uacute", "Ú"}, {"Ucirc", "Û"},
    {"Uuml", "Ü"}, {"Yacute", "Ý"}, {"THORN", "Þ"},
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"},
    {"delta", "δ"}, {"epsilon", "ε"}, {"theta", "θ"},
    {"lambda", "λ"}, {"mu", "μ"}, {"pi", "π"},
    {"sigma", "σ"}, {"phi", "φ"}, {"omega", "ω"},
    {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"},
    {"Lambda", "Λ"}, {"Pi", "Π"}, {"Sigma", "Σ"},
    {"Phi", "Φ"}, {"Omega", "Ω"},
  };
  return table;
}

// HTML unescape: turn entity references back into their characters. Handles
// numeric refs `&#DDD;` / `&#xHHH;` (any case) plus the named set in
// html_named_entities(). A reference must end in `;`; anything that isn't a
// recognized, well-formed reference is left exactly as written (browser-style
// leniency), so `&` on its own and unknown entities pass through unchanged.
inline std::string html_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') { out += s[i++]; continue; }
    size_t semi = s.find(';', i + 1);
    // Bound the search so a stray '&' doesn't scan the whole string.
    if (semi == std::string_view::npos || semi - i > 32) {
      out += s[i++];
      continue;
    }
    std::string_view body = s.substr(i + 1, semi - i - 1);
    bool replaced = false;
    if (!body.empty() && body[0] == '#') {  // numeric reference
      uint32_t cp = 0;
      bool ok = false;
      std::string_view digits = body.substr(1);
      if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
        digits = digits.substr(1);
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(),
                                 cp, 16);
        ok = !digits.empty() && r.ec == std::errc{} &&
             r.ptr == digits.data() + digits.size();
      } else {
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(),
                                 cp, 10);
        ok = !digits.empty() && r.ec == std::errc{} &&
             r.ptr == digits.data() + digits.size();
      }
      if (ok) {
        // HTML5 maps a surrogate or out-of-range numeric reference to U+FFFD
        // rather than rejecting the document. This is the one caller that
        // substitutes; every other path raises on a non-scalar value.
        if (!is_unicode_scalar_value(cp)) cp = 0xFFFD;
        append_utf8(out, cp);
        replaced = true;
      }
    } else {  // named reference
      const auto& table = html_named_entities();
      if (auto it = table.find(body); it != table.end()) {
        out += it->second;
        replaced = true;
      }
    }
    if (replaced) {
      i = semi + 1;
    } else {
      out += s[i++];  // leave the '&' as-is and keep scanning
    }
  }
  return out;
}

// Base64 (RFC 4648, standard alphabet, `=` padding). Shared by the interp and
// JIT `Encoding.base64.*` adapters.
inline std::string base64_encode(std::string_view in) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  auto u = [&](size_t k) { return static_cast<unsigned char>(in[k]); };
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned n = (u(i) << 16) | (u(i + 1) << 8) | u(i + 2);
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += tbl[n & 63];
  }
  if (size_t rem = in.size() - i; rem == 1) {
    unsigned n = u(i) << 16;
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned n = (u(i) << 16) | (u(i + 1) << 8);
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

// Decode standard base64. Returns nullopt on an invalid character; ASCII
// whitespace (so wrapped base64 decodes) and `=` padding are tolerated.
inline std::optional<std::string> base64_decode(std::string_view in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(in.size() / 4 * 3);
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=') break;  // padding → end of data
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    int v = val(c);
    if (v < 0) return std::nullopt;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buf >> bits) & 0xFF);
    }
  }
  return out;
}

// One hex digit → its value, or -1 if not [0-9a-fA-F]. Shared by hex_decode
// and url_decode (the percent-escape body is two hex digits).
inline int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Lower-case hex encode: each byte becomes two hex digits. Shared by the
// interp and JIT `Encoding.hex.*` adapters.
inline std::string hex_encode(std::string_view in) {
  static const char tbl[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out += tbl[c >> 4];
    out += tbl[c & 0xF];
  }
  return out;
}

// Decode hex. Returns nullopt on an odd length or any non-hex character;
// both upper- and lower-case digits are accepted.
inline std::optional<std::string> hex_decode(std::string_view in) {
  if (in.size() % 2 != 0) return std::nullopt;
  std::string out;
  out.reserve(in.size() / 2);
  for (size_t i = 0; i < in.size(); i += 2) {
    int hi = hex_digit(in[i]), lo = hex_digit(in[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out += static_cast<char>((hi << 4) | lo);
  }
  return out;
}

// Percent-encode: the RFC 3986 unreserved set `A-Za-z0-9-_.~` plus whatever
// `extra_unreserved` lists is kept verbatim, every other byte becomes `%XX`
// with upper-case hex (so a space is `%20`, not `+`). Byte-oriented, so UTF-8
// is encoded as its bytes.
//
// `extra_unreserved` is what separates the two percent-encodings culebra
// speaks: RFC 3986 for `Encoding.url.*` (nothing extra) and ECMA-262
// encodeURIComponent for HTTP query strings and form bodies (`!*'()` — see
// http.h's percent_encode_component).
inline std::string percent_encode(std::string_view in,
                                  std::string_view extra_unreserved = {}) {
  static const char tbl[] = "0123456789ABCDEF";
  std::string out;
  // Enough for a run of unreserved bytes plus a few escapes, so the common
  // "mostly unreserved" input never reallocates.
  out.reserve(in.size() + in.size() / 2);
  for (unsigned char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~' ||
        extra_unreserved.find(static_cast<char>(c)) != std::string_view::npos) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += tbl[c >> 4];
      out += tbl[c & 0xF];
    }
  }
  return out;
}

// RFC 3986 percent-encoding. Backs the interp and JIT `Encoding.url.*`
// adapters.
inline std::string url_encode(std::string_view in) {
  return percent_encode(in);
}

// Decode percent-encoding: `%XX` (either hex case) becomes its byte. A `%`
// not followed by two hex digits is left verbatim (lenient, like Python's
// urllib unquote), and `+` stays literal so url_encode/url_decode round-trip
// exactly. Never fails.
inline std::string url_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hex_digit(in[i + 1]), lo = hex_digit(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 3;
        continue;
      }
    }
    out += in[i++];
  }
  return out;
}

// Location lives in the CulebraError's line/col fields; the top-level
// printer (src/main.cc) appends ` at L:C.` once, so messages must not
// embed it themselves (else it prints twice).
[[noreturn]] inline void throw_type_error_at(int64_t line, int64_t col) {
  throw CulebraError("TypeError", "type error", line, col);
}

// Shared by interp's `eval_destructure_assign` and JIT's
// `compile_destructure_assign` so both backends report the same
// structured error (`ValueError` + descriptive message + location)
// when an Object / Array / Tuple pattern fails to match its rval.
[[noreturn]] inline void throw_destructure_mismatch_at(int64_t line, int64_t col) {
  throw CulebraError("ValueError",
                     "destructure pattern did not match value", line, col);
}

// `o.x += rhs` against a missing property `x`. Both backends used to
// diverge here — interp checked existence and threw AttributeError,
// JIT read the missing slot as nil and then threw TypeError on
// `nil + rhs`. This helper unifies the kind + message + location.
[[noreturn]] inline void throw_compound_missing_property_at(
    int64_t line, int64_t col) {
  throw CulebraError("AttributeError",
                     "compound assignment on missing property.",
                     line, col);
}

// `Ns.member` where `member` isn't in the closed namespace `Ns`'s member
// set — on a read (`Ns.member`) or a plain/`??=` write (`Ns.member = v`).
// Shared so both directions and both backends report the identical kind +
// message; only the location differs by call site.
[[noreturn]] inline void throw_namespace_missing_member_at(
    const char* ns_name, std::string_view member, int64_t line, int64_t col) {
  throw CulebraError(
      "AttributeError",
      culebra::format("namespace '{}' has no member '{}'",
                      ns_name ? ns_name : "?", member),
      line, col);
}

// Reassigning a `let` (non-mut) binding. Interp tracks per-binding
// mut via the `Symbol`'s `mut` flag; the JIT carries `mut` on
// `VarSlot` and routes here when a write hits a non-mut slot.
[[noreturn]] inline void throw_immutable_assign_at(
    const std::string& name, int64_t line, int64_t col) {
  throw CulebraError("ImmutableError",
                     culebra::format("cannot reassign '{}' (declared without 'mut')",
                                     name),
                     line, col);
}

// Raise the uniform shadow-prohibition error. Used by interp and JIT at
// every binding site that would shadow a closure-captured outer variable
// (let/mut declarations, function parameters, match pattern bindings).
[[noreturn]] inline void throw_shadow_error(std::string_view name,
                                            size_t line, size_t column) {
  throw CulebraError(
      "ShadowError",
      culebra::format("cannot shadow outer variable '{}' (declared in an enclosing "
                      "function)",
                      name),
      static_cast<long>(line), static_cast<long>(column));
}

// Call site passed a keyword the callee doesn't accept. Both backends
// used to diverge: interp threw at runtime (catchable by try/catch),
// JIT raised at IR-emit time (uncatchable). This helper unifies them
// to runtime throws.
[[noreturn]] inline void throw_unknown_kwarg_at(
    const std::string& name, int64_t line, int64_t col) {
  throw CulebraError("TypeError", unknown_kwarg_message(name), line, col);
}

// Call site failed to bind a required parameter (no positional, no
// kwarg, no default). Same backend-asymmetry rationale as the unknown-
// kwarg helper above.
[[noreturn]] inline void throw_missing_required_arg_at(
    const std::string& name, int64_t line, int64_t col) {
  throw CulebraError("ArityError", missing_required_arg_message(name),
                     line, col);
}

// Generic runtime throw for cases where the JIT used to detect the
// error at IR-emit time (uncatchable) while the interp threw at
// eval time (catchable). Both backends now route through this helper
// to keep `try { ... } catch e { e.kind }` semantics symmetric.
[[noreturn]] inline void throw_runtime_error_at(
    const std::string& kind, const std::string& msg,
    int64_t line, int64_t col) {
  throw CulebraError(kind, msg, line, col);
}

// Canonical arithmetic type error: `1 + "a"`, `nil + nil`, `2 ** "a"`,
// `1 @ 2`, etc. Single message source for both backends — interp's
// `arith_dispatch` / `compute_power` and the JIT's typed fast-path and
// `_arith_*` helpers all route here so the wording stays in lockstep.
[[noreturn]] inline void throw_arith_type_error(std::string_view op,
                                                std::string_view lhs,
                                                std::string_view rhs,
                                                int64_t line = 0, int64_t col = 0) {
  throw CulebraError("TypeError",
                     "type error: cannot apply '" + std::string(op) +
                         "' to " + std::string(lhs) + " and " +
                         std::string(rhs),
                     line, col);
}

// Canonical comparison type error for `<`, `<=`, `>`, `>=`: cross-type
// (`1 < "a"`) and same-type-unorderable (`[1] < [2]`, `{} < {}`). Both
// backends single-source from here.
[[noreturn]] inline void throw_compare_type_error(std::string_view lhs,
                                                  std::string_view rhs,
                                                  int64_t line = 0,
                                                  int64_t col = 0) {
  throw CulebraError("TypeError",
                     "type error: cannot compare " + std::string(lhs) +
                         " and " + std::string(rhs),
                     line, col);
}

// Resolve the position argument of Array `insert` / `remove_at`: negative
// counts from the end, like `a[i]`. `allow_end` admits `size` itself — the
// append slot `insert` accepts and `remove_at` (which must address a live
// element) does not. Single source so both backends raise the same
// IndexError as a subscript.
inline size_t resolve_position_index(int64_t i, size_t size, bool allow_end,
                                     int64_t line = 0, int64_t col = 0) {
  auto n = static_cast<int64_t>(size);
  if (i < 0) i += n;
  if (i < 0 || i > n || (i == n && !allow_end))
    throw CulebraError("IndexError", "index out of range", line, col);
  return static_cast<size_t>(i);
}

// A sort's comparison sequence is unobservable when every element belongs to
// the same comparison class: `<` then neither throws nor enters user code, so
// which pairs get compared — and in what order — cannot be seen from the
// language. Sorts take the in-place std::stable_sort fast path in that case.
// Anything else (user objects with cmp/__lt__, or mixed types whose `<`
// throws) must go through stable_sort_permutation below. `kind(x)` returns a
// nonzero id per comparison class and 0 for everything else.
template <class It, class Kind>
inline bool ordering_unobservable(It first, It last, Kind kind) {
  if (first == last) return true;
  auto k0 = kind(*first);
  if (k0 == 0) return false;
  for (auto it = std::next(first); it != last; ++it) {
    if (kind(*it) != k0) return false;
  }
  return true;
}

// Stable sort expressed as a permutation of indices, for the observable case.
// std::stable_sort chooses insertion-sort or merge by
// `is_trivially_copy_assignable<T>`, so sorting elements directly ran
// different algorithms for the interpreter's Value and the JIT's JitValue:
// a user `cmp`'s side-effect order and the operand order of an
// incomparable-element TypeError then diverged between backends. Sorting
// indices makes the sorted type the same on both sides. It also keeps the
// elements untouched while comparisons run, so a throwing comparison leaves
// the array exactly as it was rather than strewn with moved-from slots.
template <class Less>
inline std::vector<size_t> stable_sort_permutation(size_t n, Less less) {
  std::vector<size_t> perm(n);
  std::iota(perm.begin(), perm.end(), size_t{0});
  std::stable_sort(perm.begin(), perm.end(), less);
  return perm;
}

// Canonical "expected X, got Y" type error. Used for unary negation
// (`-"a"` → expected "Long or Float") and any other single-operand type
// guard shared between backends.
[[noreturn]] inline void throw_type_mismatch(std::string_view expected,
                                             std::string_view got,
                                             int64_t line = 0, int64_t col = 0) {
  throw CulebraError("TypeError",
                     "type error: expected " + std::string(expected) +
                         ", got " + std::string(got),
                     line, col);
}

// Integer power by squaring. `exp` must be non-negative; result wraps
// on overflow (matches the rest of Long arithmetic — no bignum).
inline int64_t ipow_nonneg(int64_t base, int64_t exp) {
  int64_t r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    exp >>= 1;
    if (exp > 0) base *= base;
  }
  return r;
}

// The numeric hash rule both backends use: a Float holding an integral value
// hashes as that Long, so 2 and 2.0 land in the same bucket. Single-sourced
// here so ValueHash and JitValueHash cannot drift — including on the width,
// which is int64_t because that is what a culebra Long is.
inline size_t hash_long(int64_t v) { return std::hash<int64_t>{}(v); }
inline size_t hash_double(double d) {
  int64_t as_long = static_cast<int64_t>(d);
  if (std::isfinite(d) && static_cast<double>(as_long) == d) {
    return hash_long(as_long);
  }
  return std::hash<double>{}(d);
}

// Floored remainder — the one `%` does not give, since `%` truncates.
// The result carries the sign of `n`, so a positive `n` lands it in
// `[0, n)`: the wrap a circular index wants. `n == 0` is the caller's
// to reject. Single source for interp and JIT.
inline int64_t floored_mod(int64_t x, int64_t n) {
  if (n == -1) return 0;  // INT64_MIN % -1 overflows; the answer is 0 anyway
  int64_t r = x % n;
  return (r != 0 && (r < 0) != (n < 0)) ? r + n : r;
}

// Trim ASCII whitespace from both ends of a string view, returning the
// substring. Shared by the numeric string parsers below.
inline std::string_view trim_ascii(std::string_view s) {
  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) j--;
  return s.substr(i, j - i);
}

// ASCII-only case fold. Range-checked so the result is locale-
// independent — std::toupper/tolower consult the C locale and would
// fold Latin-1 bytes too under e.g. en_US.UTF-8. Non-ASCII bytes
// (>= 0x80) pass through unchanged, matching the "ASCII uppercase /
// lowercase" contract in docs/language.md §17.1.
inline std::string ascii_upper(std::string s) {
  for (auto& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}
inline std::string ascii_lower(std::string s) {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// First ASCII letter uppercased, the rest lowercased — Python/Ruby
// `capitalize`. ASCII-only like ascii_upper/ascii_lower, so a leading
// multi-byte scalar passes through unchanged (its lead byte is >= 0x80).
inline std::string ascii_capitalize(std::string s) {
  s = ascii_lower(std::move(s));
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
    s[0] = static_cast<char>(s[0] - 'a' + 'A');
  }
  return s;
}

// Byte length of the UTF-8 scalar that starts at leading byte `c`. A
// continuation or invalid byte reports 1, so callers always advance.
inline size_t utf8_scalar_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

// Trim scalars in `chars` from one or both ends. An empty `chars` trims
// ASCII whitespace (the no-arg `trim`/`trim_start`/`trim_end` default).
// Returns a view into `s`. Shared by interp + JIT so both agree.
inline std::string_view trim_chars(std::string_view s, std::string_view chars,
                                   bool left, bool right) {
  auto in_set = [&](std::string_view ch) {
    if (chars.empty()) {
      return ch.size() == 1 && std::isspace(static_cast<unsigned char>(ch[0]));
    }
    for (size_t i = 0; i < chars.size();) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(chars[i]));
      if (i + n > chars.size()) n = 1;
      if (chars.substr(i, n) == ch) return true;
      i += n;
    }
    return false;
  };
  size_t b = 0, e = s.size();
  if (left) {
    while (b < e) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(s[b]));
      if (b + n > e) n = 1;
      if (!in_set(s.substr(b, n))) break;
      b += n;
    }
  }
  if (right) {
    while (e > b) {
      size_t st = e - 1;  // walk back over UTF-8 continuation bytes
      while (st > b && (static_cast<unsigned char>(s[st]) & 0xC0) == 0x80) st--;
      if (!in_set(s.substr(st, e - st))) break;
      e = st;
    }
  }
  return s.substr(b, e - b);
}

// Per-scalar translation (Ruby `String#tr`, character-list form — no `a-z`
// ranges or `^` negation). Each Unicode scalar of `s` that appears in `from`
// is replaced by the scalar at the same position in `to`; if `to` is shorter
// its last scalar repeats, and an empty `to` deletes the matched scalars.
// Shared by the interp String method and the JIT runtime so both agree.
inline std::string str_tr(std::string_view s, std::string_view from,
                          std::string_view to) {
  auto scalars = [](std::string_view x) {
    std::vector<std::string_view> v;
    for (size_t i = 0; i < x.size();) {
      size_t n = utf8_scalar_len(static_cast<unsigned char>(x[i]));
      if (i + n > x.size()) n = 1;
      v.push_back(x.substr(i, n));
      i += n;
    }
    return v;
  };
  auto from_s = scalars(from);
  auto to_s = scalars(to);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = utf8_scalar_len(static_cast<unsigned char>(s[i]));
    if (i + n > s.size()) n = 1;
    std::string_view ch = s.substr(i, n);
    auto it = std::find(from_s.begin(), from_s.end(), ch);
    if (it == from_s.end()) {
      out += ch;
    } else if (!to_s.empty()) {
      size_t idx = static_cast<size_t>(it - from_s.begin());
      out += to_s[idx < to_s.size() ? idx : to_s.size() - 1];
    }  // else: `to` empty → delete (append nothing)
    i += n;
  }
  return out;
}

// `s.repeat(n)` — `n` copies of `s` concatenated. `n == 0` is the empty
// String; a negative `n` is a ValueError rather than a silent empty, and an
// oversized result is caught before the allocation is attempted. Shared by
// the interp String method and the JIT runtime so both agree.
inline std::string str_repeat(std::string_view s, int64_t n, int64_t line = 0,
                              int64_t col = 0) {
  if (n < 0) {
    throw CulebraError("ValueError", "repeat() n must not be negative", line,
                       col);
  }
  if (s.empty() || n == 0) return {};
  // Bound against what a String can hold, not just against size_t overflow —
  // a result past max_size would otherwise reach reserve() and escape as a
  // std::length_error instead of a catchable culebra error.
  if (static_cast<uint64_t>(n) > std::string().max_size() / s.size()) {
    throw CulebraError("ValueError", "repeat() result is too large", line, col);
  }
  std::string out;
  out.reserve(s.size() * static_cast<size_t>(n));
  for (int64_t i = 0; i < n; i++) out += s;
  return out;
}

// `s.truncate(max, ellipsis)` — `s` unchanged if it already fits in `max`
// bytes; otherwise cut so the result (content + ellipsis) is exactly `max`
// bytes. Byte-based like `slice()`, so a multi-byte scalar can split at the
// cut point same as slice's existing behavior. A negative `max`, or one too
// small to fit `ellipsis`, is a ValueError rather than a silently wrong cut.
inline std::string str_truncate(std::string_view s, int64_t max,
                                std::string_view ellipsis, int64_t line = 0,
                                int64_t col = 0) {
  if (max < 0) {
    throw CulebraError("ValueError", "truncate() max must not be negative",
                       line, col);
  }
  auto umax = static_cast<uint64_t>(max);
  if (s.size() <= umax) return std::string(s);
  if (umax < ellipsis.size()) {
    throw CulebraError("ValueError",
                       culebra::format("truncate() max must be at least {}",
                                       ellipsis.size()),
                       line, col);
  }
  std::string out(s.substr(0, umax - ellipsis.size()));
  out += ellipsis;
  return out;
}

// Split into lines on `\n`, `\r\n`, or `\r`. The terminator is dropped and a
// trailing one does not yield a final empty line, so `f.lines()` and
// `s.lines()` agree line-for-line. Returns views into `s`.
inline std::vector<std::string_view> str_lines(std::string_view s) {
  std::vector<std::string_view> out;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    while (j < s.size() && s[j] != '\n' && s[j] != '\r') j++;
    out.push_back(s.substr(i, j - i));
    if (j == s.size()) break;
    i = j + (s[j] == '\r' && j + 1 < s.size() && s[j + 1] == '\n' ? 2 : 1);
  }
  return out;
}

// Non-overlapping occurrences of `sub` in `s`. An empty `sub` counts 0,
// matching this String family's own `split("")` (which yields the receiver
// whole) rather than Python's len+1.
inline int64_t str_count(std::string_view s, std::string_view sub) {
  if (sub.empty()) return 0;
  int64_t n = 0;
  for (size_t pos = s.find(sub); pos != std::string_view::npos;
       pos = s.find(sub, pos + sub.size())) {
    n++;
  }
  return n;
}

// `s.index_of(sub, start)` — the byte offset of the first match at or after
// `start`, or -1 (Array.index_of's convention). A negative `start` counts
// from the end, like slice's indices.
inline int64_t str_index_of(std::string_view s, std::string_view sub,
                            int64_t start) {
  auto n = static_cast<int64_t>(s.size());
  if (start < 0) start += n;
  if (start < 0) start = 0;
  if (start > n) return -1;
  auto p = s.find(sub, static_cast<size_t>(start));
  return p == std::string_view::npos ? -1 : static_cast<int64_t>(p);
}

// `s.last_index_of(sub)` — the byte offset of the last match, or -1.
inline int64_t str_last_index_of(std::string_view s, std::string_view sub) {
  auto p = s.rfind(sub);
  return p == std::string_view::npos ? -1 : static_cast<int64_t>(p);
}

// `s.strip_prefix(p)` / `s.strip_suffix(p)` — `s` without that exact
// affix, or `s` unchanged when it is not there. Unlike `trim_start(chars)`,
// which trims a *set* of scalars, these match the whole string once.
inline std::string_view str_strip_prefix(std::string_view s,
                                         std::string_view p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0
             ? s.substr(p.size())
             : s;
}

inline std::string_view str_strip_suffix(std::string_view s,
                                         std::string_view p) {
  return s.size() >= p.size() &&
                 s.compare(s.size() - p.size(), p.size(), p) == 0
             ? s.substr(0, s.size() - p.size())
             : s;
}

// `s.split(sep, limit)` / `s.rsplit(sep, limit)` — the pieces, as views into
// `s`. An empty `sep` never splits. `limit` caps how many pieces come back
// (0 = uncapped); `rsplit` fills that cap from the right but still returns
// the pieces left to right, so `'a.b.c'.rsplit('.', 2)` is `['a.b', 'c']`.
// Single source for both backends and for both directions.
inline std::vector<std::string_view> str_split_pieces(std::string_view s,
                                                      std::string_view sep,
                                                      int64_t limit,
                                                      bool from_right) {
  std::vector<std::string_view> out;
  if (sep.empty() || limit == 1) {
    out.push_back(s);
    return out;
  }
  auto capped = [&] {
    return limit > 0 && static_cast<int64_t>(out.size()) + 1 == limit;
  };
  if (!from_right) {
    size_t pos = 0;
    while (!capped()) {
      auto p = s.find(sep, pos);
      if (p == std::string_view::npos) break;
      out.push_back(s.substr(pos, p - pos));
      pos = p + sep.size();
    }
    out.push_back(s.substr(pos));
    return out;
  }
  size_t end = s.size();
  while (!capped() && end >= sep.size()) {
    auto p = s.rfind(sep, end - sep.size());
    if (p == std::string_view::npos) break;
    out.push_back(s.substr(p + sep.size(), end - p - sep.size()));
    end = p;
  }
  out.push_back(s.substr(0, end));
  std::reverse(out.begin(), out.end());
  return out;
}

// Advance the bracket-nesting `depth` for one character. `<...>` Generic
// args and `(...)` function-type params both nest, so a separator (`|`,
// `+`, `,`) is only top-level when depth is 0 — that's how `fn(A | B) -> C`
// and `Array<Long, fn(A, B) -> C>` keep their inner punctuation private.
// Unbalanced closers clamp at 0. Shared by every type-string split/scan
// below so the nesting rule has one definition.
inline void track_type_bracket_depth(char c, int& depth) {
  if (c == '<' || c == '(') depth++;
  else if (c == '>' || c == ')') { if (depth > 0) depth--; }
}

// True iff `name` has a `|` at the outermost bracket depth (i.e. a
// top-level Union alt separator). `Array<Long | Float>` returns
// false — the `|` lives inside `<...>`. Callers gate the Union
// branch on this so a single Generic-with-inner-Union doesn't
// recurse forever via the otherwise-safe `find('|') != npos` check.
inline bool has_toplevel_pipe(std::string_view name) {
  int depth = 0;
  for (char c : name) {
    track_type_bracket_depth(c, depth);
    if (c == '|' && depth == 0) return true;
  }
  return false;
}

// Split a pipe-separated Union type annotation into its component
// type names. `Long | Float` → ["Long", "Float"]. Whitespace around
// each candidate is trimmed. A single type name (no pipe) returns
// the input unchanged as the only element. Empty input yields an
// empty vector. Shared by both backends' type checks.
//
// Generic-aware: `|` inside `<...>` does not split, so
// `Array<Long | Float>` stays one candidate. Depth is tracked across
// the whole string; unbalanced `>` are clamped to 0.
//
// DEFENSIVE: empty alternatives (e.g. `Long ||  | Float`, leading
// `|`, trailing `|`) are silently skipped. Grammar prevents these
// from reaching here in normal source, but the helper is also
// called on canonicalized strings during multifn dedup so we don't
// rely on grammar alone.
//
// LIFETIME: each returned string_view aliases bytes inside `name`;
// the caller must keep `name`'s underlying storage alive while
// using the returned views. Do not pass a temporary.
inline std::vector<std::string_view> split_union_types(
    std::string_view name) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    track_type_bracket_depth(c, depth);
    if (c == '|' && depth == 0) {
      auto cand = trim_ascii(name.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(name.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// True iff `name` has a `+` at the outermost bracket depth — the
// intersection separator used in composite Generic bounds
// (`T: Hashable + Stringer`, lowered to `"Hashable + Stringer"`).
// Depth-aware to mirror has_toplevel_pipe, though bound atoms are
// bare trait names today and never carry `<...>`.
inline bool has_toplevel_plus(std::string_view name) {
  int depth = 0;
  for (char c : name) {
    track_type_bracket_depth(c, depth);
    if (c == '+' && depth == 0) return true;
  }
  return false;
}

// Split a `+`-separated intersection bound into its component trait
// names. `Hashable + Stringer` → ["Hashable", "Stringer"]. A value
// satisfies the bound when it conforms to *all* parts (all-of), the
// dual of split_union_types' any-of. Whitespace is trimmed; empty
// parts are skipped. Mirrors split_union_types (incl. LIFETIME: each
// view aliases `name`, so don't pass a temporary).
inline std::vector<std::string_view> split_intersection_types(
    std::string_view name) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    track_type_bracket_depth(c, depth);
    if (c == '+' && depth == 0) {
      auto cand = trim_ascii(name.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(name.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a comma-separated Generic argument list into its components,
// respecting nested `<...>`. `Long, Map<String, Long>` →
// ["Long", "Map<String, Long>"]. Empty args yield empty vector.
inline std::vector<std::string_view> split_generic_args(
    std::string_view args) {
  std::vector<std::string_view> out;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < args.size(); i++) {
    char c = args[i];
    track_type_bracket_depth(c, depth);
    if (c == ',' && depth == 0) {
      auto cand = trim_ascii(args.substr(start, i - start));
      if (!cand.empty()) out.push_back(cand);
      start = i + 1;
    }
  }
  auto last = trim_ascii(args.substr(start));
  if (!last.empty()) out.push_back(last);
  return out;
}

// Split a single (non-Union) type name into outer + generic args.
// `Array<Long>` → {"Array", "Long"}; `Map<String, Long>` →
// {"Map", "String, Long"}; `Long` → {"Long", ""}.
// The args view is the raw inside-the-brackets text (callers split
// it further with `split_generic_args`).
//
// DEFENSIVE: malformed inputs (`<T>` with empty outer, `Array<Long>>`
// or `Array<<Long>` with unbalanced brackets) fall back to the whole
// trimmed string as the outer with no args, rather than yielding a
// degenerate split. Grammar normally prevents these from reaching
// here, but the helper is exported and may be called on
// programmatically-constructed type strings.
//
// LIFETIME: outer and args alias bytes inside `name`; caller must
// keep `name`'s underlying storage alive while using them. Do not
// pass a temporary std::string.
struct GenericHead {
  std::string_view outer;
  std::string_view args;  // empty if no <...>
};
inline GenericHead parse_generic_head(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return {trimmed, {}};
  auto pos = trimmed.find('<');
  if (pos == std::string_view::npos || trimmed.back() != '>') {
    return {trimmed, {}};
  }
  // Outer name must be non-empty.
  if (pos == 0) return {trimmed, {}};
  // Brackets must balance — reject `Array<Long>>` and `Array<<Long>`.
  int depth = 0;
  for (char c : trimmed) {
    if (c == '<') depth++;
    else if (c == '>') {
      depth--;
      if (depth < 0) return {trimmed, {}};
    }
  }
  if (depth != 0) return {trimmed, {}};
  return {
    trim_ascii(trimmed.substr(0, pos)),
    trimmed.substr(pos + 1, trimmed.size() - pos - 2),
  };
}

// Parse a type-parameter declaration like `T` or `T: Comparable` into
// its name and (possibly empty) bound. Used by class / trait / fn
// declarations to extract Generic params from CLASS_HEAD tokens.
struct TypeParam {
  std::string_view name;
  std::string_view bound;   // empty = no bound
};
inline TypeParam parse_type_param(std::string_view raw) {
  auto trimmed = trim_ascii(raw);
  auto pos = trimmed.find(':');
  if (pos == std::string_view::npos) return {trimmed, {}};
  return {trim_ascii(trimmed.substr(0, pos)),
          trim_ascii(trimmed.substr(pos + 1))};
}

// Split a TRAIT_HEAD token into the trait name (with any Generic
// params still attached) and its supertrait list. `Ord: Eq` →
// {"Ord", ["Eq"]}; `Cmp<T>: Eq, Show` → {"Cmp<T>", ["Eq", "Show"]};
// `Foo` → {"Foo", {}}. The name/supertrait split is on the first `:`
// at bracket depth 0 so a type-param bound (`Box<T: Bound>`) inside
// `<...>` is not mistaken for the supertrait separator. Each view
// aliases `token` — keep it alive (do not pass a temporary).
struct TraitHead {
  std::string_view name;                     // may include `<...>`
  std::vector<std::string_view> supertraits;
};
inline TraitHead parse_trait_head(std::string_view token) {
  auto trimmed = trim_ascii(token);
  int depth = 0;
  size_t colon = std::string_view::npos;
  for (size_t i = 0; i < trimmed.size(); i++) {
    char c = trimmed[i];
    if (c == '<') depth++;
    else if (c == '>') { if (depth > 0) depth--; }
    else if (c == ':' && depth == 0) { colon = i; break; }
  }
  if (colon == std::string_view::npos) return {trimmed, {}};
  TraitHead out;
  out.name = trim_ascii(trimmed.substr(0, colon));
  out.supertraits = split_generic_args(trimmed.substr(colon + 1));
  return out;
}

// Lower every occurrence of a type-param name (`T`, `K`, ...) inside
// `tn` to its runtime check target, including nested forms like
// `Array<T>` and `T | Long`. An *unbounded* param (`T`) lowers to
// "Any" (the annotation was documentation, so the check is a no-op).
// A *bounded* param (`T: Comparable`) lowers to its bound trait name
// (`Comparable`) so the existing trait-conformance machinery enforces
// the bound at dispatch / type_check time — no separate generic
// dispatch path is needed. Used by both class-method and free-fn
// signature neutralization. Returns the result in canonical form
// (single-space `|`, comma-space-separated generic args).
inline std::string lower_type_params(
    std::string_view tn,
    const std::vector<std::string_view>& type_params) {
  auto trimmed = trim_ascii(tn);
  if (trimmed.empty()) return "";
  // Top-level Union (depth-aware): rewrite each alt, join with " | ".
  if (has_toplevel_pipe(trimmed)) {
    std::string out;
    bool first = true;
    for (auto cand : split_union_types(trimmed)) {
      if (!first) out += " | ";
      out += lower_type_params(cand, type_params);
      first = false;
    }
    return out;
  }
  auto head = parse_generic_head(trimmed);
  // Outer alone matches a type-param: the whole annotation collapses to
  // the param's lowered form (so `T`, `T<Long>`, etc. all become "Any"
  // for an unbounded param, or the bound trait name for a bounded one —
  // the generic args were documentation anyway).
  for (auto tp_raw : type_params) {
    auto tp = parse_type_param(tp_raw);
    if (head.outer != tp.name) continue;
    if (tp.bound.empty()) return "Any";
    // Composite bound (`A + B`): normalize spacing to a canonical
    // " + "-joined form so dispatch-key dedup compares equal regardless
    // of source whitespace. Matching itself is spacing-insensitive
    // (split_intersection_types trims), but the registered key isn't.
    if (!has_toplevel_plus(tp.bound)) return std::string(tp.bound);
    std::string out;
    bool first = true;
    for (auto part : split_intersection_types(tp.bound)) {
      if (!first) out += " + ";
      out += part;
      first = false;
    }
    return out;
  }
  if (head.args.empty()) return std::string(head.outer);
  // Generic: keep outer, recurse into each arg.
  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += lower_type_params(a, type_params);
    first = false;
  }
  out += '>';
  return out;
}

// A function type `fn(P1, P2) -> R` split into its parameter list and
// return type (all views aliasing `name`). Returns nullopt when `name`
// is not a function type, so callers can fall through to the plain-type
// path. The return is restricted to a single TYPE_ATOM by the grammar:
// if the text after `->` carries a top-level `|`, that pipe belongs to a
// surrounding Union (`fn(A) -> B | C` = `(fn(A) -> B) | C`), so this is
// NOT treated as one function type and we return nullopt — letting the
// Union split handle it. Mirrors the grammar's FN_TYPE / TYPE_ATOM.
//
// LIFETIME: params/ret alias bytes inside `name`; keep `name` alive and
// do not pass a temporary.
struct FnTypeParts {
  std::vector<std::string_view> params;
  std::string_view ret;
};
// `want_params == false` skips building the parameter list (the only
// allocation) — `is_fn_type` only needs the structural yes/no, and that
// is fully decided before the param split. The grammar logic stays in
// one place so the rejection rules can't drift between the two callers.
inline std::optional<FnTypeParts> parse_fn_type(std::string_view name,
                                                bool want_params = true) {
  auto t = trim_ascii(name);
  // Must start with the `fn` keyword followed by `(`. Type names start
  // with `[A-Z]`, so a lowercase `fn(` is unambiguously a function type.
  if (t.size() < 3 || t.substr(0, 2) != "fn") return std::nullopt;
  size_t i = 2;
  auto is_sp = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };
  while (i < t.size() && is_sp(t[i])) i++;
  if (i >= t.size() || t[i] != '(') return std::nullopt;
  // Find the `)` matching this opening paren (paren depth only; generic
  // `<...>` never contributes an unbalanced paren).
  int depth = 0;
  size_t close = std::string_view::npos;
  for (size_t j = i; j < t.size(); j++) {
    if (t[j] == '(') depth++;
    else if (t[j] == ')') { if (--depth == 0) { close = j; break; } }
  }
  if (close == std::string_view::npos) return std::nullopt;
  // Expect `->` after the param list.
  size_t k = close + 1;
  while (k < t.size() && is_sp(t[k])) k++;
  if (k + 1 >= t.size() || t[k] != '-' || t[k + 1] != '>') return std::nullopt;
  auto ret = trim_ascii(t.substr(k + 2));
  if (ret.empty()) return std::nullopt;
  // A top-level `|` in the return means the pipe is the outer Union's, not
  // part of this function type — defer to the Union split.
  if (has_toplevel_pipe(ret)) return std::nullopt;
  FnTypeParts parts;
  parts.ret = ret;
  if (want_params) {
    auto inner = trim_ascii(t.substr(i + 1, close - i - 1));
    if (!inner.empty()) parts.params = split_generic_args(inner);
  }
  return parts;
}

inline bool is_fn_type(std::string_view name) {
  return parse_fn_type(name, /*want_params=*/false).has_value();
}

// Canonical form for a (possibly Union, possibly Generic) type
// annotation, used in fn.params introspection and multifn
// redeclaration matching so whitespace variants of the same
// type compare equal.
//
// `Long|Float`               → `Long | Float`
// `Array<Long|Float>`        → `Array<Long | Float>`
// `Array < Long , Float >`   → `Array<Long, Float>`
// `Long | Long`              → `Long`         (dedup at top level)
// Empty input                → ""
//
// Recursive: nested `<...>` are canonicalized as well. Owned
// std::string so storage outlives `name`.
inline std::string canonicalize_type_annotation(std::string_view name) {
  auto trimmed = trim_ascii(name);
  if (trimmed.empty()) return "";

  auto cands = split_union_types(trimmed);
  if (cands.size() > 1) {
    // Top-level Union — canonicalize each candidate and dedup. Linear
    // dedup against canonical forms covers `Array<Long> | Array<Long>`
    // (different whitespace inside the args still collapses).
    std::string out;
    std::vector<std::string> seen;
    for (auto cand : cands) {
      auto canon = canonicalize_type_annotation(cand);
      bool dup = false;
      for (auto& s : seen) if (s == canon) { dup = true; break; }
      if (dup) continue;
      seen.push_back(canon);
      if (!out.empty()) out += " | ";
      out += canon;
    }
    return out;
  }

  // Function type `fn(P, ...) -> R`. Checked before the `?` sugar so a
  // `fn(A) -> B?` (optional *return*) keeps its `?` on the return atom
  // rather than being read as an optional function. Params canonicalize
  // normally (their `?`/Unions are grouped by the parens); the return
  // preserves a trailing `?` instead of expanding to `| Nil`, because a
  // top-level `| Nil` in the canonical string would later re-parse as the
  // outer Union `(fn(A) -> B) | Nil` — a different type.
  if (auto fn = parse_fn_type(trimmed)) {
    std::string out = "fn(";
    bool first = true;
    for (auto p : fn->params) {
      if (!first) out += ", ";
      out += canonicalize_type_annotation(p);
      first = false;
    }
    out += ") -> ";
    auto ret = fn->ret;
    if (!ret.empty() && ret.back() == '?') {
      auto base = canonicalize_type_annotation(ret.substr(0, ret.size() - 1));
      out += (base.empty() || base == "Nil") ? "Nil" : base + "?";
    } else {
      out += canonicalize_type_annotation(ret);
    }
    return out;
  }

  // `T?` Optional sugar -> `T | Nil`. The `?` binds to a single type
  // name; top-level unions were already split above, so a `?` here is on
  // one alternative (`Foo?`, `Array<Long>?`). Dedup keeps `Nil?` == `Nil`.
  if (trimmed.back() == '?') {
    auto base = canonicalize_type_annotation(trimmed.substr(0, trimmed.size() - 1));
    if (base.empty() || base == "Nil") return "Nil";
    return base + " | Nil";
  }

  // Single type — check for Generic.
  auto head = parse_generic_head(trimmed);
  if (head.args.empty()) return std::string(head.outer);

  std::string out(head.outer);
  out += '<';
  bool first = true;
  for (auto a : split_generic_args(head.args)) {
    if (!first) out += ", ";
    out += canonicalize_type_annotation(a);
    first = false;
  }
  out += '>';
  return out;
}

// Parse a full string as a signed long in `base` (2-36); whitespace-trim
// allowed, any other trailing content or invalid form throws `type error at
// L:C`. A base outside the range is a ValueError.
inline int64_t parse_long_strict(std::string_view s, int64_t line, int64_t col,
                                 int64_t base = 10) {
  if (base < 2 || base > 36) {
    throw CulebraError(
        "ValueError",
        culebra::format("to_long() base must be between 2 and 36, got {}", base),
        line, col);
  }
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    // Accept the radix prefix that matches the base, so a literal copied out
    // of source (`0xff`, `0b1010`) parses as itself. Dropping just the letter
    // leaves its `0` as a leading digit, which every base accepts.
    size_t at = (owned[0] == '+' || owned[0] == '-') ? 1 : 0;
    std::string_view marks =
        base == 16 ? "xX" : base == 8 ? "oO" : base == 2 ? "bB" : "";
    if (!marks.empty() && owned.size() > at + 2 && owned[at] == '0' &&
        marks.find(owned[at + 1]) != std::string_view::npos) {
      owned.erase(at + 1, 1);
    }
    int64_t v = std::stoll(owned, &used, static_cast<int>(base));
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0;  // unreachable
}

// Parse an integer literal token to a signed long, recognizing the
// `0x` / `0o` / `0b` radix prefixes (hex / octal / binary) in addition
// to plain decimal. The grammar (NUMBER rule) guarantees the digit set
// per prefix, so this is a clean base dispatch with no validation churn.
// Shared by interp (eval_number) and JIT (compile NUMBER) so the two
// backends decode literals identically.
inline int64_t parse_integer_literal(std::string_view tok) {
  std::string owned(tok);
  int base = 10;
  size_t off = 0;
  if (tok.size() > 2 && tok[0] == '0') {
    char p = tok[1];
    if (p == 'x' || p == 'X') { base = 16; off = 2; }
    else if (p == 'o' || p == 'O') { base = 8; off = 2; }
    else if (p == 'b' || p == 'B') { base = 2; off = 2; }
  }
  // strtoll over the body after any prefix; the grammar restricted the
  // digits, so no partial-parse guard is needed. errno is checked so an
  // out-of-range literal throws instead of silently clamping to
  // INT64_MAX/MIN (matches parse_long_strict's strict overflow handling).
  // strtoll, not strtol: `long` is 32-bit on Windows, where it rejected every
  // literal above 2^31 that the JIT and AOT accepted.
  errno = 0;
  int64_t v = std::strtoll(owned.c_str() + off, nullptr, base);
  if (errno == ERANGE) {
    throw CulebraError("ValueError",
                       culebra::format("integer literal out of range: {}", tok));
  }
  return v;
}

// --- Interpolation format spec (`"{x:.2f}"`) ----------------------------
//
// The mini-language is delegated to std::format, whose spec syntax is
// Python-derived (`[[fill]align][sign][#][0][width][grp][.prec][type]`).
// We wrap `{:<spec>}` around the captured spec and vformat the value.
// Shared by interp and JIT so both render identically. A bad spec throws
// a ValueError (mapped from std::format_error).

// Context label for the type check on a nested spec field (`"{s:>{w}}"`).
// Shared so interp and JIT render the same "type error: ... expects Long".
// A field is spliced in as decimal text, and a computed width or precision
// is the whole point of the shape, so Long is the only value that fits.
inline constexpr std::string_view kSpecFieldContext = "format spec field";

// Trailing type char of a spec (`f`, `x`, …), or 0 if none.
inline char format_spec_type(std::string_view spec) {
  if (spec.empty()) return 0;
  char c = spec.back();
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '%') return c;
  return 0;
}
inline bool format_spec_wants_float(std::string_view spec) {
  char c = format_spec_type(spec);
  return c == 'e' || c == 'E' || c == 'f' || c == 'F' || c == 'g' ||
         c == 'G' || c == '%' || c == 'a' || c == 'A';
}
inline bool format_spec_wants_int(std::string_view spec) {
  char c = format_spec_type(spec);
  return c == 'b' || c == 'c' || c == 'd' || c == 'o' || c == 'x' || c == 'X';
}

// Common core: wrap `{:<spec>}`, vformat, map format_error to a
// ValueError tagged with the value's culebra type name.
template <typename T>
inline std::string format_value_as(T& v, std::string_view type_name,
                                   std::string_view spec, int64_t line,
                                   int64_t col) {
  std::string fmt = "{:";
  fmt += spec;
  fmt += "}";
  try {
    return std::vformat(fmt, std::make_format_args(v));
  } catch (const std::format_error&) {
    throw CulebraError("ValueError",
        culebra::format("invalid format spec '{}' for {}", spec, type_name), line,
        col);
  }
}
inline std::string format_value_long(int64_t v, std::string_view spec,
                                     int64_t line, int64_t col) {
  return format_value_as(v, "Long", spec, line, col);
}
inline std::string format_value_double(double v, std::string_view spec,
                                       int64_t line, int64_t col) {
  return format_value_as(v, "Float", spec, line, col);
}
inline std::string format_value_string(std::string v, std::string_view spec,
                                       int64_t line, int64_t col) {
  return format_value_as(v, "String", spec, line, col);
}

// Parse a full string as a double; same trim / full-consumption rules.
inline double parse_double_strict(std::string_view s, int64_t line,
                                  int64_t col) {
  auto t = trim_ascii(s);
  if (t.empty()) throw_type_error_at(line, col);
  try {
    size_t used = 0;
    std::string owned(t);
    double v = std::stod(owned, &used);
    if (used != owned.size()) throw std::invalid_argument("");
    return v;
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    throw_type_error_at(line, col);
  }
  return 0.0;  // unreachable
}

// --- Runtime context ---

// Owns all per-instance VM state. Multiple Runtimes can coexist on the
// same thread; RuntimeScope swaps the active one. Single-Runtime
// embedders never construct one — a lazy thread-local default fills in.
//
// Heavy types (InterpGC, ShapeRegistry, ...) live in type-erased slots
// because they're defined in headers that include shared.h, so they
// can't be direct members.
enum RuntimeSlot : size_t {
  kSlotInterpGc = 0,
  // Backs the hot JIT structs/buffers (jit_slab.h). MUST stay below kSlotJitGc
  // and below every JitValue-holding table slot: ~Runtime destroys substates
  // in reverse slot order, and those table dtors (module/namespace/test) free
  // pinned structs through the slab's operator delete during teardown, so the
  // slab must outlive them. See the static_assert below the enum.
  kSlotJitSlab,
  kSlotJitGc,
  // JIT multimethod registries (dispatcher→name + name→overload bodies). Per
  // Runtime, holding +1 refs on the overload body closures. Placed just above
  // the GC heap so they outlive every JitValue-holding table (module / namespace
  // / test): those tables' destructors release closures, and releasing a
  // multifn dispatcher consults these registries (`_jit_multifn_forget`). As
  // runtime substates they follow ~Runtime's reverse-slot teardown + revival,
  // so a table dtor at process exit reads a live (or harmlessly re-revived)
  // registry instead of a destroyed thread_local. See _jit_multifn_forget.
  kSlotJitMultifnNames,
  kSlotJitMultimethods,
  // Non-owning body→dispatcher uplinks (the multifn self-recursion handle).
  // A body's lifetime is bounded by its dispatcher's — the table above holds
  // the only +1 on a body — so the raw pointer can never dangle while the
  // body runs; entries leave with the table entry (register's displacement,
  // _jit_multifn_forget). Same placement rationale as its two siblings.
  kSlotJitMultifnUplinks,
  // JIT trait default-method table (trait → method → closure), holding a +1
  // on each compiled default body. Same placement rationale as the multifn
  // registries above: it outlives the JitValue-holding tables (a teardown
  // `drop` can still dispatch a trait default) while its own destructor's
  // release cascade still sees a live multifn registry, GC heap, and slab.
  kSlotJitTraitDefaults,
  kSlotShapeRegistry,  // reserved/unused: the Shape intern table is now a
                       // process-global singleton (see jit.h ShapeRegistry) —
                       // Shapes are shared immutable metadata, not isolated heap
  kSlotDeferStack,
  kSlotJitHooks,
  kSlotJitModuleTable,
  kSlotJitNamespaceTable,
  kSlotFileTable,
  // The session name table vm.h's repl_session() falls back to when no
  // session is current on the thread. A slot rather than one object for the
  // process because a session's cells are heap objects, and a heap belongs to
  // one Runtime: a thread that never opened a session (an HTTP worker running
  // a route handler, an isolate) must mint its own rather than read cells
  // another Runtime allocated and has since freed. Holds pinned cells but
  // frees nothing itself, so its position relative to the GC slots does not
  // matter: a pin is an unconditional root, so nothing sweeps them, and
  // their storage goes when this Runtime's heap and slab do.
  kSlotReplSession,
  // Per-scope owned-resource stacks for deterministic drop (one per
  // backend; see jit_owned.h "owned stack"). Entries are
  // non-owning, so destruction order relative to the GC slots is moot —
  // but ~InterpGC's collect resolves this slot after it was nulled, so
  // every Runtime that ran interp code revives it empty. ~Runtime's sweep
  // is what keeps that revival from leaking.
  kSlotInterpOwnedStack,
  kSlotJitOwnedStack,
  // Foreign-instance tables (wrapped C++ objects, see foreign.h): one
  // type-erased registry holding a per-T id table. Owns the C++
  // instances; destroyed with the Runtime — FIRST in the reverse-order
  // teardown (highest slot), so every foreign instance dies before the
  // GC slots. A drop firing during later GC teardown resolves a lazily
  // revived empty registry (the ~Runtime null-after-delete protocol)
  // and its erase is a safe no-op.
  kSlotForeignTables,
  // Borrow registry: opaque-id → (raw ptr + parent link). A
  // borrowing handle stores only the opaque id, so its raw pointer is
  // never a script-writable slot — the same forgery-safety owning
  // handles get from kSlotForeignTables. Holds no instances (the parent
  // owns them), so teardown order relative to the foreign tables doesn't
  // matter; placed just below them.
  kSlotBorrowTable,
  kRuntimeSlotCount
};

// The slab must be destroyed last among the JIT slots (reverse-order
// teardown), so any table dtor that frees a pinned struct through operator
// delete still resolves a live allocator. A future slot reorder that violates
// this fails to compile rather than corrupting memory at teardown.
static_assert(kSlotJitSlab < kSlotJitGc &&
                  kSlotJitSlab < kSlotJitModuleTable &&
                  kSlotJitSlab < kSlotJitNamespaceTable,
              "kSlotJitSlab must outlive the GC heap and all struct-holding "
              "table substates during reverse-order Runtime teardown");

struct Runtime;

// The active Runtime on this thread, or none (see current_runtime()).
inline thread_local Runtime* _culebra_current_runtime = nullptr;

// RAII to switch the active Runtime for the current thread. Above Runtime
// rather than beside current_runtime() because ~Runtime uses it on itself; a
// Runtime* and a Runtime& are all it needs, so the forward declaration does.
struct RuntimeScope {
  Runtime* prev_;
  explicit RuntimeScope(Runtime& rt) : prev_(_culebra_current_runtime) {
    _culebra_current_runtime = &rt;
  }
  ~RuntimeScope() { _culebra_current_runtime = prev_; }
  RuntimeScope(const RuntimeScope&) = delete;
  RuntimeScope& operator=(const RuntimeScope&) = delete;
};

struct Runtime {
  std::mt19937_64 random_engine{std::random_device{}()};

  // JIT exception carriers. Set by `culebra_runtime_throw`, read by
  // the try/catch landing pad. See jit.h for the protocol.
  int8_t thrown_tag = 0;
  int64_t thrown_data = 0;
  int8_t is_throw = 0;

  // Pending runtime-error carrier (backend-neutral: kind/msg/line/col, no
  // Value). Every CulebraError records itself here at construction; the JIT
  // try/catch pad materializes it into thrown_tag/data rather than re-inspecting
  // the in-flight C++ exception via `throw;` (which Windows SEH funclet EH can't
  // do). Distinct from the `is_throw` carriers because a user `throw` fills
  // those directly (it already has a Value), while a runtime error carries only
  // text until a catch pad turns it into an error Object. The interpreter
  // ignores this — it catches the C++ CulebraError directly.
  //
  // Invariant: trusted only while a culebra error is unwinding. It is consumed
  // (cleared) by the catch pad's try_translate, restored across a swallowing
  // dispose(), and superseded by the next thrown error. A runtime helper that
  // catches a CulebraError in C++ and RECOVERS (does not rethrow) must clear
  // pending_error, or a later foreign C++ exception could be misread as it.
  std::string pending_kind;
  std::string pending_msg;
  int64_t pending_line = 0;
  int64_t pending_col = 0;
  int8_t pending_error = 0;

  // Cooperative cancellation. When non-null and set, the interpreter's
  // statement dispatch throws `Interrupted` to unwind this thread (an isolate
  // being cancelled / dropped). The flag itself lives on the owning IsolateCore
  // (isolate.h); the Runtime only borrows a pointer. Null on the main thread.
  std::atomic<bool>* interrupt_flag = nullptr;

  std::array<void*, kRuntimeSlotCount> substate{};
  std::array<void (*)(void*), kRuntimeSlotCount> substate_deleter{};

  Runtime() = default;
  ~Runtime() {
    // Destroy substates in reverse slot order. The GC substates (kSlotInterpGc,
    // kSlotJitGc) are the lowest slots and MUST outlive the JitValue-holding
    // substates (module/namespace tables, test registry, defer stack), whose
    // destructors release values back into the GC. Forward order frees the GC
    // first, leaving those later releases to call into a destroyed heap. Null
    // each slot after deleting so any release that still resolves a substate
    // mid-teardown lazily revives an empty one instead of touching freed memory.
    //
    // Install ourselves as current first: the worker's own RuntimeScope is
    // already gone by now, so a release here would otherwise free into a
    // *different* Runtime's slab. Repeat until a pass frees nothing — a slot
    // revived below the cursor leaks (the interp owned stack revives that way,
    // from ~InterpGC's collect).
    RuntimeScope self(*this);
    bool destroyed;
    do {
      destroyed = false;
      for (size_t i = substate.size(); i-- > 0;) {
        if (substate[i] && substate_deleter[i]) {
          substate_deleter[i](substate[i]);
          substate[i] = nullptr;
          destroyed = true;
        }
      }
    } while (destroyed);
  }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
};

// The arguments the program was started with, as `Sys.argv` reports them.
// Process-wide rather than a Runtime member: argv is fixed at startup and is
// the same fact on every thread, so an isolate or a server worker — each of
// which builds its own Runtime — sees what the main thread saw. Filled in once
// by the entry point: main.cc, the AOT bootstrap, or an embedding host.
inline std::vector<std::string>& sys_argv() {
  static std::vector<std::string> v;
  return v;
}

// Globals a subcommand binds on top of the stdlib, for the length of one run:
// `culebra test` adds `test` and `parametrize`, and `culebra prog.cul` has
// neither. Process-wide for sys_argv's reason, and down here rather than in
// the stdlib because a bare name is resolved before any of it runs — by the
// load-stage undefined-variable lint, and by the bytecode compiler. The
// storage is the caller's: a static table of names, set as a whole.
// (Not this: `install_cli_aliases`' inspect / print / println, which are
// unconditional entries in the compiled lanes' own table and so are named
// unconditionally in builtin_global_names.)
inline std::span<const std::string_view>& ambient_globals() {
  static std::span<const std::string_view> names;
  return names;
}

inline bool is_ambient_global(std::string_view name) {
  for (auto n : ambient_globals())
    if (n == name) return true;
  return false;
}

inline Runtime& default_runtime() {
  static thread_local Runtime rt;
  return rt;
}

inline Runtime& current_runtime() {
  return _culebra_current_runtime ? *_culebra_current_runtime
                                  : default_runtime();
}

// Definition of the forward-declared hook (see CulebraError's ctor). Records the
// error's text into the current Runtime's pending carrier; cheap and on the cold
// error path. current_runtime() always returns a valid Runtime (a lazily-created
// thread-local when none is installed), so this is safe from any context — parse,
// interpret, or JIT.
inline void culebra_note_pending_error(const std::string& kind,
                                       const std::string& msg, int64_t line,
                                       int64_t col) {
  auto& rt = current_runtime();
  rt.pending_kind = kind;
  rt.pending_msg = msg;
  rt.pending_line = line;
  rt.pending_col = col;
  rt.pending_error = 1;
}

// True when the current isolate has been asked to cancel (see Runtime::
// interrupt_flag). Polled by the statement dispatch and by blocking channel
// waits. Always false on the main thread (no flag installed).
inline bool interrupt_requested() {
  auto* f = current_runtime().interrupt_flag;
  return f && f->load(std::memory_order_relaxed);
}

// --- Ctrl+C (SIGINT) cooperative interruption ---------------------------
//
// A process-wide one-shot flag set by the installed SIGINT handler. The main
// thread's Runtime::interrupt_flag and the main interpreter's poll both point
// here, and the JIT/AOT loop safepoint reads the same symbol inline. It is
// distinct from an isolate's cancel flag (sticky + per-isolate): a Ctrl+C is
// consumed when the cooperative `Interrupted` is thrown, so a `catch` can
// resume (Python's KeyboardInterrupt model). `extern "C"` + `used` keeps the
// symbol resolvable by the in-process JIT and linkable by AOT output.
extern "C" {
__attribute__((used)) inline std::atomic<bool> culebra_g_sigint{false};
// The JIT/AOT loop safepoint inlines a load of THIS flag — not the per-thread
// interrupt flag, which inlined codegen can't cheaply read. Set whenever ANY
// interrupt becomes pending (a real Ctrl+C OR a per-isolate cancel), so a tight
// JIT loop branches to the cold slow path; the slow path then consults the
// per-thread `interrupt_flag` for the truth (Ctrl+C vs which isolate). Cleared
// once nothing remains pending, so loops return to the one-load fast path.
__attribute__((used)) inline std::atomic<bool> culebra_g_wake{false};
}

// Count of isolates cancelled but not yet reaped. Keeps `culebra_g_wake` set
// while a cancelled JIT isolate may still be spinning toward its next safepoint;
// the last reap (IsolateCore's dtor) clears the wake. Host-side only — no JIT IR
// references it.
inline std::atomic<int> culebra_g_cancel_pending{0};

// Consume a pending Ctrl+C (so a `catch` can resume) and release the JIT wake
// flag when nothing else is pending. Shared by the interp poll, the JIT
// safepoint, and the stdin poll so the wake is dropped uniformly.
inline void _consume_sigint() {
  culebra_g_sigint.store(false, std::memory_order_relaxed);
  if (culebra_g_cancel_pending.load(std::memory_order_relaxed) == 0) {
    culebra_g_wake.store(false, std::memory_order_relaxed);
  }
}

// Signal.notify (Go's signal.Notify model). When a program registers a channel
// to receive Ctrl+C, `culebra_g_signal_notify` is set: the handler then relays
// the signal to a delivery thread (via `culebra_g_signal_pending`) instead of
// latching the throw flag, so the running code keeps going and the program
// drives its own shutdown by reading the channel. Both are plain host-side
// atomics (no JIT IR references them). The registry + delivery thread live in
// isolate.h; here is only the handler's branch.
inline std::atomic<bool> culebra_g_signal_notify{false};
inline std::atomic<bool> culebra_g_signal_pending{false};

// Hook sendable_jit.h fills in at load, read by jit.h and the script
// teardown guard — kept as a hook so script_teardown.h stays light instead
// of pulling the whole isolate stack into every consumer. Called right
// before JIT::exec tears
// down the LLJIT: every isolate's compiled body is the SAME JitClosure::fn_ptr
// living in that LLJIT (sendable_jit.h), so a spawn still running when the
// module's code memory is freed returns into unmapped memory. Cancels + joins
// whatever is still outstanding — a no-op once every isolate has already been
// joined (the normal case), which is the vast majority of runs.
inline std::function<void()>& isolate_teardown_join_hook() {
  static std::function<void()> fn;
  return fn;
}

// True iff `f` is the process SIGINT flag (a Ctrl+C) rather than a per-isolate
// cancel — the poll uses this to pick the message and the one-shot consume.
inline bool is_sigint_flag(const std::atomic<bool>* f) {
  return f == &culebra_g_sigint;
}

// Async-signal-safe SIGINT handler: the first Ctrl+C latches the flag; a second
// one (while the first is still pending / unhandled) restores the default
// disposition and re-raises, so a wedged program that never reaches a safepoint
// can still be force-killed. Only atomic ops + signal()/raise() run here.
inline void _culebra_sigint_handler(int sig) {
  // Notify mode (Signal.notify): relay to the registered channel via the
  // delivery thread instead of throwing. No force-kill escalation — the program
  // opted to handle signals itself (Signal.reset() restores default behavior).
  if (culebra_g_signal_notify.load(std::memory_order_relaxed)) {
    culebra_g_signal_pending.store(true, std::memory_order_relaxed);
    return;
  }
  if (culebra_g_sigint.exchange(true, std::memory_order_relaxed)) {
    std::signal(sig, SIG_DFL);
    std::raise(sig);
  } else {
    // First Ctrl+C: wake the JIT/AOT loop safepoints (atomic store is
    // async-signal-safe). The slow path consumes the one-shot flag.
    culebra_g_wake.store(true, std::memory_order_relaxed);
  }
}

// Request a cooperative interrupt (a "Ctrl+C") from an embedder or test, without
// a real signal: set the throw flag AND the JIT/AOT wake flag, so a running loop
// — interpreter or JIT — stops at its next safepoint. The signal handler sets the
// same pair (plus the double-press force-kill escalation). Set both: the interp
// poll reads the throw flag directly; the inlined JIT safepoint reads only the
// wake flag (it can't cheaply read the per-thread flag).
inline void request_interrupt() {
  culebra_g_sigint.store(true, std::memory_order_relaxed);
  culebra_g_wake.store(true, std::memory_order_relaxed);
}

// Install the SIGINT handler and point the current (main) thread's Runtime at
// the global flag, so blocking channel waits and the statement poll observe
// Ctrl+C. CLI-only: embedders opt in by calling this.
//
// Per lane, not once in main(): the handler only sets a flag, so a lane with
// nothing polling it (`serve`, `lint`, `fmt`) would turn one Ctrl+C into two
// presses. A lane that polls calls this itself; one that does not leaves
// SIG_DFL, where a single press still stops it.
inline void install_sigint_handler() {
  current_runtime().interrupt_flag = &culebra_g_sigint;
  std::signal(SIGINT, _culebra_sigint_handler);
}

// A Windows console decodes output with its code page — on a localized install
// a legacy DBCS one (932 on ja-JP), which turns non-ASCII bytes into mojibake
// and lets an orphaned lead byte eat the ESC of a following `\x1b[y;xH`. The
// input CP follows so read_key returns non-ASCII keys as UTF-8 too. Restored at
// exit: a console left on 65001 changes behaviour for what runs in it next.
// CLI-only, like install_sigint_handler: embedders own their console.
inline void install_console_utf8() {
#if defined(_WIN32)
  static UINT saved_out, saved_in;  // static: the atexit lambda cannot capture
  UINT out = GetConsoleOutputCP();
  if (out == 0) return;  // no console attached (redirected output)
  UINT in = GetConsoleCP();
  if (out == CP_UTF8 && in == CP_UTF8) return;  // already UTF-8, nothing to undo
  if (!SetConsoleOutputCP(CP_UTF8)) return;
  SetConsoleCP(CP_UTF8);
  saved_out = out;
  saved_in = in;
  std::atexit([] {
    SetConsoleOutputCP(saved_out);
    SetConsoleCP(saved_in);
  });
#endif
}

// Throw the cooperative Interrupted if an interrupt is pending — for blocking
// syscalls that poll between waits (interruptible stdin below). Checks the
// process SIGINT flag directly (like the JIT safepoint), so it works regardless
// of which Runtime is active — the JIT runs under a fresh RuntimeScope whose
// borrowed flag is null. A Ctrl+C is one-shot (consumed so a `catch` can
// resume); an isolate's cancel flag (this thread's borrowed flag, if any) stays
// sticky and keeps its own message.
inline void throw_if_interrupted() {
  auto* f = current_runtime().interrupt_flag;
  // Per-thread isolate cancel: sticky, its own message. Checked first so an
  // isolate thread reports its own cancel instead of consuming a Ctrl+C wake.
  if (f && !is_sigint_flag(f) && f->load(std::memory_order_relaxed)) {
    throw CulebraError("Interrupted", "isolate cancelled");
  }
  // Ctrl+C: one-shot. Only the main/CLI context — whose interrupt_flag IS the
  // sigint flag, or is null under a borrowed JIT RuntimeScope — consumes it. An
  // isolate thread that merely saw the wake leaves the flag for the main thread.
  if (culebra_g_sigint.load(std::memory_order_relaxed) &&
      (!f || is_sigint_flag(f))) {
    _consume_sigint();
    throw CulebraError("Interrupted", "interrupted");
  }
}

// Ctrl+C or a cancel, as opposed to a program's own error. A catch that
// reports errors lets this one through; only a component hosting a loop (the
// REPL, an embedder) reports it, and only main() turns it into an exit code.
inline bool is_interrupt(const CulebraError& e) {
  return e.kind == "Interrupted";
}

// Whether a Ctrl+C or `flag`'s cancel is pending. `flag` is the watched
// thread's, captured by it: a watcher on another thread cannot read
// current_runtime() — it would get its own.
inline bool interrupt_fired(std::atomic<bool>* flag) {
  return culebra_g_sigint.load(std::memory_order_relaxed) ||
         (flag && flag->load(std::memory_order_relaxed));
}

// The same, for the current thread. Read-only: the one-shot consume + throw is
// throw_if_interrupted(), once a blocking loop has cleaned up.
inline bool interrupt_pending() {
  return interrupt_fired(current_runtime().interrupt_flag);
}

// --- Interruptible stdin --------------------------------------------------
//
// A plain `std::cin` read is not a cooperative safepoint: a program blocked
// waiting for stdin couldn't be stopped by a single Ctrl+C (only the second,
// force-killing press). These read the raw fd in a `poll` loop that wakes every
// ~200ms to honor the interrupt flag, throwing Interrupted if it fires — so a
// single Ctrl+C breaks a stdin wait too. `IO.stdin()` (read / lines) / `IO.input`
// (both backends) route through here. A thread-local buffer holds bytes read past a
// line so the next read sees them. POSIX only; Windows keeps the blocking
// `std::cin`/stdio path (not a current build target).

#if !defined(_WIN32)
inline std::string& _stdin_leftover() {
  static thread_local std::string buf;
  return buf;
}

// Wait for more bytes on fd 0, appending one chunk to the leftover buffer.
// Returns false at EOF. Polls the interrupt flag between (and on) waits.
inline bool _stdin_fill() {
  char chunk[65536];
  for (;;) {
    throw_if_interrupted();
    struct pollfd pfd{/*fd=*/0, /*events=*/POLLIN, /*revents=*/0};
    int r = ::poll(&pfd, 1, 200);
    if (r < 0) {
      if (errno == EINTR) continue;  // signal arrived → re-check the flag
      return false;                  // real poll error → treat as EOF
    }
    if (r == 0) continue;            // timeout → re-check the flag
    ssize_t n = ::read(0, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;        // EOF
    _stdin_leftover().append(chunk, static_cast<size_t>(n));
    return true;
  }
}
#endif

// Read all of stdin to EOF, interruptibly.
inline std::string read_stdin_all_interruptible() {
#if defined(_WIN32)
  std::string out;
  char chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), stdin)) > 0)
    out.append(chunk, n);
  return out;
#else
  std::string out;
  out.swap(_stdin_leftover());       // take anything already buffered
  while (_stdin_fill()) {
    out += _stdin_leftover();
    _stdin_leftover().clear();
  }
  return out;
#endif
}

// Read up to `n` bytes from stdin, interruptibly. Returns fewer than `n` only
// at EOF (an empty string means immediate EOF). Shares the leftover buffer
// with the line reader, so the two interleave correctly.
inline std::string read_stdin_n_interruptible(size_t n) {
#if defined(_WIN32)
  std::string out;
  out.reserve(n);
  int c;
  while (out.size() < n && (c = std::fgetc(stdin)) != EOF)
    out.push_back(static_cast<char>(c));
  return out;
#else
  std::string out;
  auto& buf = _stdin_leftover();
  while (out.size() < n) {
    if (buf.empty() && !_stdin_fill()) break;  // EOF
    size_t take = std::min(n - out.size(), buf.size());
    out.append(buf, 0, take);
    buf.erase(0, take);
  }
  return out;
#endif
}

// Read one line (without the trailing newline) interruptibly. Returns false at
// EOF with nothing read; a final unterminated line is returned once.
inline bool read_stdin_line_interruptible(std::string& out) {
  out.clear();
#if defined(_WIN32)
  int c;
  bool any = false;
  while ((c = std::fgetc(stdin)) != EOF) {
    any = true;
    if (c == '\n') break;
    out.push_back(static_cast<char>(c));
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return any;
#else
  auto& buf = _stdin_leftover();
  for (;;) {
    auto nl = buf.find('\n');
    if (nl != std::string::npos) {
      out.assign(buf, 0, nl);
      buf.erase(0, nl + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (!_stdin_fill()) {            // EOF
      if (buf.empty()) return false;
      out.swap(buf);                 // trailing line with no newline
      buf.clear();
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
  }
#endif
}

// Serializes stdout/stderr writes so concurrent isolates don't interleave
// mid-line (`inspect` once = one atomic line). Process-wide.
inline std::mutex& stdio_mutex() {
  static std::mutex m;
  return m;
}

// Lazy-init a default-constructible T into a Runtime slot.
template <class T>
inline T& runtime_substate(RuntimeSlot slot) {
  auto& rt = current_runtime();
  if (!rt.substate[slot]) {
    rt.substate[slot] = new T();
    rt.substate_deleter[slot] = [](void* p) { delete static_cast<T*>(p); };
  }
  return *static_cast<T*>(rt.substate[slot]);
}

// --- Shared PRNG (interpreter and JIT) ---

inline std::mt19937_64& random_engine() {
  return current_runtime().random_engine;
}

// --- Well-known property contract ---

// Property names the runtime invokes behind the scenes:
//   drop      - RAII destructor (called on last release / cycle break)
//   iter      - iterator constructor (returns an Iterator Object)
//   has_next  - iterator gate (Bool, called before each next)
//   next      - iterator advance (returns the next element)
// Each must be a 0-arg Function. The name set and error wording live
// here so the two backends can't drift; per-backend type checks stay
// in each backend (Value vs JitClosure aren't interchangeable).
//
// The three protocol names are checked as a set: `has_next` was the one
// left out, which let a broken one reach a raw closure cast in the JIT (a
// jump through a Long payload). Binding is the first of two lines — the
// protocol open (see `_check_iter_protocol` / iter_protocol_open) is what
// makes the cast safe even for objects built by paths that bypass this
// check, e.g. a native builder writing slots directly.
inline bool is_well_known_prop(std::string_view name) {
  return name == "drop" || name == "iter" || name == "has_next" ||
         name == "next";
}

// Two lowerings rewrite a body into a synthesized class and promote every
// body local to a field on the instance — the generator CPS transform
// (generator_transform.h) and the effects transform (effects_transform.h),
// which shares its collect_local_names / rewrite_locals_to_self /
// emit_ctor_param_and_local_inits helpers. These are the class names they
// emit.
//
// Such an instance's own slots are compiler storage, not user object slots,
// so a value read of one must NOT bind `self` the way `o.f` does. Binding
// would leave a promoted local holding a function permanently bound to the
// state object: `f == f` false, and a later `holder.f()` unable to take its
// own receiver. The class's own methods live on the proto, are not own slots,
// and keep binding normally. Both backends test this once per class
// declaration and flag the class meta.
inline constexpr std::string_view kGeneratorStateClassPrefix = "_Gen_";
inline constexpr std::string_view kEffectComputationClassPrefix = "_EffComp_";
inline constexpr std::string_view kEffectBodyClassPrefix = "_EffBody_";

// `parse_path` is the AST node's parse label. A lowering re-parses its
// synthesized source under a `<stem#N>` label (next_fragment_label), which no
// user file path can be, so requiring it keeps a user class that happens to
// be spelled `_Gen_x` on ordinary binding semantics.
inline bool is_lowered_state_class(std::string_view class_name,
                                   std::string_view parse_path) {
  if (!parse_path.starts_with('<') ||
      parse_path.find('#') == std::string_view::npos)
    return false;
  return class_name.starts_with(kGeneratorStateClassPrefix) ||
         class_name.starts_with(kEffectComputationClassPrefix) ||
         class_name.starts_with(kEffectBodyClassPrefix);
}

// The zero value a typed instance field (`x: T` with no initializer) takes
// at construction. Single source for the interp's zero_value_for_type and
// the JIT's emit_zero_value so a declared type maps to the same default on
// every backend.
enum class ZeroKind { Float, Bool, String, Long, Nil };
inline ZeroKind zero_kind_for_type(std::string_view type) {
  if (type == "Float32" || type == "Float64" || type == "Float")
    return ZeroKind::Float;
  if (type == "Bool") return ZeroKind::Bool;
  if (type == "String") return ZeroKind::String;
  if (type == "Long" || type == "Byte" || type.starts_with("Int"))
    return ZeroKind::Long;
  return ZeroKind::Nil;  // reference types default to nil
}

[[noreturn]] inline void throw_well_known_prop_contract_error(
    std::string_view name) {
  throw CulebraError(
      "DropContractError",
      culebra::format("type error: '{}' must be a Function taking no arguments.",
                      name));
}

// --- Iterator protocol diagnostics (docs/language.md §18.5) ---
//
// Raised where an iterator is OPENED — the for-in iterable expression, or
// the point a lazy chain first drives its upstream. Single-sourced here so
// interp and JIT/AOT report the identical kind and wording; each backend
// supplies the position (0/0 leaves it to the positionless-error backfill).
[[noreturn]] inline void throw_iter_not_object(int64_t line = 0, int64_t col = 0) {
  throw CulebraError("TypeError",
                     "type error: iter() did not return an Object", line, col);
}

[[noreturn]] inline void throw_iter_missing_protocol(int64_t line = 0,
                                                     int64_t col = 0) {
  throw CulebraError("TypeError",
                     "type error: iterator missing has_next()/next()", line,
                     col);
}

// --- trait / protocol registry (§15) ---
//
// A trait declares a set of required methods. A class conforms to a
// trait when it has every required method (matching arity) — Go /
// Python `__str__` style structural conformance, no `impl` block.
// Default methods (`has_default = true`) are supplied by the trait
// itself and do NOT need to be on the class.

struct TraitMethod {
  std::string name;
  size_t arity;          // declared positional arity (excluding `self`)
  bool has_default;
};

struct TraitDef {
  std::string name;
  std::vector<TraitMethod> methods;
  // Supertrait names (`trait Ord: Eq` → {"Eq"}); their methods are
  // flattened into `methods` at register_trait time so conformance and
  // dispatch stay single-set look-ups. Empty on the JIT/AOT path, which
  // flattens at runtime via culebra_runtime_register_trait_super.
  std::vector<std::string> supertraits;
};

// Process-wide registry. Trait declarations register here; type_matches
// / multifn_specificity consult it on each lookup. Keyed by trait name.
// Guarded by trait_mutex(): host threads can declare traits and look
// them up concurrently (mt_smoke exercises this).
inline std::shared_mutex& trait_mutex() {
  static std::shared_mutex m;
  return m;
}

inline std::unordered_map<std::string, TraitDef>& trait_registry() {
  static std::unordered_map<std::string, TraitDef> reg;
  return reg;
}

// Cache: class name → trait name → conforms? Populated lazily by
// type_matches when it encounters a value of a known class against a
// known trait. Cleared on new trait registration (later traits may flip
// earlier "no" answers for already-cached classes). Shares trait_mutex().
inline std::unordered_map<std::string,
                          std::unordered_map<std::string, bool>>&
trait_conformance_cache() {
  static std::unordered_map<std::string,
                            std::unordered_map<std::string, bool>>
      cache;
  return cache;
}

// Merge a supertrait's methods into `def` (dedup by name, keeping
// `def`'s own entries). The supertrait must already be registered —
// declaration order guarantees this for `trait Ord: Eq`. Unknown
// supers are silently skipped (best-effort). Caller holds trait_mutex().
inline void merge_supertrait_into(TraitDef& def, const std::string& super) {
  auto& reg = trait_registry();
  auto it = reg.find(super);
  if (it == reg.end()) return;
  for (const auto& m : it->second.methods) {
    bool present = false;
    for (const auto& e : def.methods)
      if (e.name == m.name) { present = true; break; }
    if (!present) def.methods.push_back(m);
  }
}

inline void register_trait(TraitDef def) {
  std::unique_lock lock(trait_mutex());
  // Flatten supertrait methods (trait inheritance) so conformance is a
  // single-set check. JIT/AOT carries empty supertraits and instead
  // merges via culebra_runtime_register_trait_super after registration.
  for (const auto& super : def.supertraits) merge_supertrait_into(def, super);
  trait_registry()[def.name] = std::move(def);
  trait_conformance_cache().clear();
}

inline const TraitDef* lookup_trait(std::string_view name) {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  auto it = reg.find(std::string(name));
  return it == reg.end() ? nullptr : &it->second;
}

// Snapshot every registered trait name under a read lock so the caller
// can iterate the names without holding trait_mutex. Used by interp /
// JIT multifn warm-up where the inner `type_matches` call re-acquires
// the mutex for cache writes — holding it across the loop would
// deadlock. Returns an empty vector with no allocation if the registry
// is empty (= the common case for trait-free programs).
inline std::vector<std::string> snapshot_trait_names() {
  std::shared_lock lock(trait_mutex());
  auto& reg = trait_registry();
  if (reg.empty()) return {};
  std::vector<std::string> names;
  names.reserve(reg.size());
  for (const auto& [n, _] : reg) names.push_back(n);
  return names;
}

// Built-in conformance: hard-coded table of which primitive type
// labels conform to which built-in traits. Lets `fn show(x: Stringer)`
// accept Long / String / Array / ... without each builtin needing a
// class-instance wrapper. Only the three built-in traits are
// supported here — user-declared traits still operate solely on
// class instances. Returns false for unknown traits, leaving
// type_matches to fall back to the registered-trait path.
inline bool builtin_conforms_to_trait(std::string_view type_label,
                                       std::string_view trait_name) {
  if (trait_name == "Stringer") {
    // Every value culebra can produce has a `to_string` interpretation
    // via the runtime display path; expose that uniformly.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor" ||
           type_label == "Function";
  }
  if (trait_name == "Eq") {
    // Equality is defined on every primitive (value-based) and
    // reference types compare by identity — covers all builtins.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Tensor";
  }
  if (trait_name == "Comparable") {
    // Ordering is well-defined on the value primitives. Container
    // types (Array / Tuple / Set / Tensor) do compare lexicographically
    // in the runtime, but we keep this conservative for the MVP.
    return type_label == "Bool" || type_label == "Long" ||
           type_label == "Float" || type_label == "String" ||
           type_label == "StringView";
  }
  if (trait_name == "StringLike") {
    // Byte-readable string flavors: owning `String` and the borrowed
    // `StringView`. User classes that want to be string-substitutable
    // also satisfy this via structural conformance on `to_string_view`.
    return type_label == "String" || type_label == "StringView";
  }
  if (trait_name == "Hashable") {
    // Mirrors what ValueHash / JitValueHash actually hash: every value
    // primitive plus Tuple (hash combines element hashes). Mutable
    // containers (Array / Set / Object / Function / Tensor) stay out
    // — they throw at hash time today and shouldn't pretend otherwise.
    return type_label == "Nil" || type_label == "Bool" ||
           type_label == "Long" || type_label == "Float" ||
           type_label == "String" || type_label == "StringView" ||
           type_label == "Tuple";
  }
  if (trait_name == "Iterable") {
    // Anything `for x in y` accepts. Primitive collections expose
    // `iter()` — String/StringView via string_builtins, Array via the
    // array iterator wrapper, Object via ObjectValue::builtins() (a
    // bare `{...}` literal still has the default key iterator), Set
    // and Tuple via their runtime-built iterator wrappers.
    return type_label == "String" || type_label == "StringView" ||
           type_label == "Array" || type_label == "Tuple" ||
           type_label == "Set" || type_label == "Object";
  }
  return false;
}

// An enum variant is `Eq` and `Hashable` by construction: it has no method
// table to supply them, and its identity is the (enum, variant, payload)
// tuple — hashed and compared as a class deriving both would be.
inline bool enum_variant_conforms_to_trait(std::string_view trait_name) {
  return trait_name == "Eq" || trait_name == "Hashable";
}

// Structural conformance check: `class_methods` maps method name to
// arity for the class under test. The class conforms when every
// non-default method on `trait` is matched by name and the class
// method accepts at least `trait_arity` positional args. Default
// methods don't need a class-side definition — the trait provides
// them and the dispatcher falls through.
//
// The arity check is one-sided (`class_arity >= trait_arity`) to
// accept class methods that carry extra parameters with defaults
// or a `**kwargs` rest. Strict equality would reject `class C {
// foo(a, **rest) }` against `trait T { foo(a) }`, since the
// class-side walk on the JIT path counts the `**rest` toward
// JitClosure::arity. The runtime call site still verifies actual
// argument counts, so an over-strict class signature surfaces as
// ArityError at call time rather than DispatchError.
inline bool class_conforms_to_trait(
    const std::unordered_map<std::string, size_t>& class_methods,
    const TraitDef& trait) {
  for (const auto& m : trait.methods) {
    if (m.has_default) continue;
    auto it = class_methods.find(m.name);
    if (it == class_methods.end() || it->second < m.arity) return false;
  }
  return true;
}

// Built-in trait declarations evaluated at the top of every program
// (interp + JIT). Provides the standard `Stringer`, `Eq`, and
// `Comparable` abstractions — small enough to inline as a const
// string, structurally usable by any class that ships the right
// methods.
inline std::string_view builtin_traits_preamble() {
  static constexpr std::string_view src = R"culebra(
trait Stringer {
  to_s() -> String
}
trait Eq {
  eq(other) -> Bool
  neq(other) -> Bool { !self.eq(other) }
}
trait Comparable {
  cmp(other) -> Long
  lt(other) -> Bool { self.cmp(other) < 0 }
  le(other) -> Bool { self.cmp(other) <= 0 }
  gt(other) -> Bool { self.cmp(other) > 0 }
  ge(other) -> Bool { self.cmp(other) >= 0 }
}
trait StringLike {
  to_string_view() -> StringView
}
trait Hashable {
  hash() -> Long
}
trait Iterator {
  has_next() -> Bool
  next() -> Any
  dispose() {}
}
trait Iterable {
  iter() -> Iterator
}
)culebra";
  return src;
}


// --- Multimethod dispatch (shared between interp and JIT) ---

// True for the reserved type-tag names value_dyn_type returns for
// non-Object values. Class instances surface their class name here,
// so a name that isn't on this list is always a class name.
inline bool is_primitive_type_label(std::string_view n) {
  return n == "Nil" || n == "Bool" || n == "Long" || n == "Float" ||
         n == "String" || n == "StringView" || n == "Array" ||
         n == "Function" || n == "Tensor" || n == "Tuple" || n == "Set";
}

// Specificity score for a (param_type, arg_type) pair. Higher = more
// specific. -1 means no match.
//
// Ordering: Any (0) < Object catch-all (1) < Union exact (2) <
// concrete exact (3) < Generic full match (4). A concrete `Long`
// param therefore wins over `Long | Float`; `Array<Long>` wins over
// bare `Array` when both happen to match.
//
// `Object` param is a catch-all for class instances ONLY (matches
// type_matches: primitives stay -1 so pick doesn't claim a match
// that the post-pick check_type would reject).
//
// Generic param (`Array<Long>`) matches an arg whose type label
// equals the outer name (`Array`) — element info isn't on the arg
// side in the MVP, so it tie-breaks only against bare `Array`.
inline int multifn_specificity(std::string_view param_type,
                                std::string_view arg_type) {
  // Tiers (×2 from the pre-trait era so trait can slot strictly
  // between Object and Union):
  //   0  Any
  //   2  Object catch-all (class instance via `Object` param)
  //   3  trait conformance
  //   4  Union exact (downgrade from any concrete alt inside)
  //   6  concrete exact / Generic outer-only / bare dict via Object param
  //   8  Generic full match (param has args and outer matches concrete)
  if (param_type.empty() || param_type == "Any") return 0;
  // Union branch must use the depth-aware top-level check — a bare
  // `find('|')` on `Array<Long | Float>` triggers the branch but
  // split_union_types yields one candidate, causing self-recursion.
  if (has_toplevel_pipe(param_type)) {
    int best = -1;
    for (auto cand : split_union_types(param_type)) {
      int s = multifn_specificity(cand, arg_type);
      if (s > best) best = s;
    }
    if (best < 0) return -1;
    // A concrete alt inside a Union scored 6; downgrade to 4 so a
    // bare concrete param outranks it. Lower-tier alts pass through.
    return best >= 6 ? 4 : best;
  }
  // Composite bound (`A + B`): all-of. The arg must satisfy every
  // part; score is the best part's tier (parts are traits → 3).
  if (has_toplevel_plus(param_type)) {
    int best = -1;
    for (auto part : split_intersection_types(param_type)) {
      int s = multifn_specificity(part, arg_type);
      if (s < 0) return -1;
      if (s > best) best = s;
    }
    return best;
  }
  // Function type `fn(...) -> R`: ranks like `Function` (structural
  // callable). A closure arg (label "Function") is an exact-ish match (6);
  // a non-primitive arg (callable class instance / bare Object) scores at
  // the Object-catch tier (2) and the value-aware post-pick check confirms
  // `__call__`. Checked before `?` so an `fn(A) -> B?` param isn't read as
  // optional. Mirrors the `Function` param branch below.
  if (is_fn_type(param_type)) {
    if (arg_type == "Function") return 6;
    if (!is_primitive_type_label(arg_type)) return 2;
    return -1;
  }
  // `T?` Optional sugar = `T | Nil`: score like a two-alt Union. A Nil
  // arg matches at the Union tier; a non-nil arg scores its base type,
  // downgraded (concrete-in-Union -> 4) so a bare `T` param outranks it.
  if (!param_type.empty() && param_type.back() == '?') {
    auto base = param_type.substr(0, param_type.size() - 1);
    if (arg_type == "Nil") return 4;
    int s = multifn_specificity(base, arg_type);
    if (s < 0) return -1;
    return s >= 6 ? 4 : s;
  }
  // Generic: outer-match against arg label (arg side carries no
  // type args today, so the args part only tie-breaks against bare
  // outer-only annotations).
  if (param_type.find('<') != std::string_view::npos) {
    auto outer = parse_generic_head(param_type).outer;
    int base = multifn_specificity(outer, arg_type);
    if (base < 0) return -1;
    return base == 6 ? 8 : base;
  }
  if (param_type == "Object") {
    if (arg_type == "Object") return 6;        // bare dict — exact
    if (is_primitive_type_label(arg_type)) return -1;
    return 2;                                  // class instance catch
  }
  if (param_type == arg_type) return 6;
  // A callable class instance satisfies `Function` (Option A: structural
  // callable). The arg side carries only a type label here, so score any
  // non-primitive (class instance / bare Object) at the Object-catch tier
  // and let the value-aware post-pick check_type confirm it actually has
  // `__call__` — a non-callable is rejected there, and an exact class
  // overload (6) still outranks this catch.
  if (param_type == "Function" && !is_primitive_type_label(arg_type)) {
    return 2;
  }
  // Trait conformance: a registered trait scores below concrete and
  // below Union exact, but above the bare-Object catch.
  if (auto* trait = lookup_trait(param_type)) {
    if (arg_type == "Object") {
      // Bare Object literal — `ObjectValue::builtins()` provides
      // default methods (`iter`, etc.) so the built-in table is the
      // authority on which traits the literal conforms to.
      return builtin_conforms_to_trait(arg_type, param_type) ? 3 : -1;
    }
    // Primitive arg: consult the hard-coded built-in conformance
    // table directly (no instance methods to walk).
    if (is_primitive_type_label(arg_type)) {
      return builtin_conforms_to_trait(arg_type, param_type) ? 3 : -1;
    }
    // Read-only path: `multifn_specificity` only reads the cache and
    // returns -1 on miss. Use `.find()` (not `operator[]`) so a miss
    // doesn't materialize an empty by_trait entry, and take a shared
    // lock so dispatchers don't serialize on this hot path.
    std::shared_lock lock(trait_mutex());
    auto& cache = trait_conformance_cache();
    auto outer = cache.find(std::string(arg_type));
    if (outer == cache.end()) return -1;
    auto it = outer->second.find(std::string(param_type));
    if (it != outer->second.end()) return it->second ? 3 : -1;
    return -1;
  }
  return -1;
}

// Pick the most specific matching entry. Returns:
//   idx >= 0  : matching entry index
//   -1        : no match
//   -2        : ambiguous (tie at the top)
// `params_of(entry)` must return a container of (regular) param-type
// strings; `is_variadic_of(entry)` reports whether the entry has a
// `*args` catch-all. A variadic entry matches any arity >= its regular
// count; the surplus positions score as Any (0), and on an otherwise
// exact tie a non-variadic (fixed-arity) entry wins.
// `min_arity_of(entry)` returns how many regular params are *required* (have no
// default). A fixed-arity entry matches arg counts in `[min, params.size()]`;
// the unsupplied tail is filled by each param's default at call time. A
// variadic entry matches any arity >= its required count. Among equal-
// type-specificity fixed-arity matches, the entry that fills fewer params by
// default (smaller regular count) wins; a genuine tie is ambiguous.
//
// `names_of(entry)` returns the regular param names (parallel to params_of) and
// `kwarg_keys` lists the keyword-argument names supplied at the call. A required
// param not covered by a positional argument may instead be covered by a kwarg
// of the same name (CLOS-style: keywords contribute to *applicability* only).
// Selection still scores on positional args, so overloads that differ only by
// keyword are ambiguous — a deliberate, runtime-dispatch-friendly choice.
template <class Entry, class ParamsOf, class IsVariadic, class MinArityOf,
          class NamesOf>
inline int64_t multifn_pick(const std::vector<Entry>& methods,
                             const std::vector<std::string_view>& arg_types,
                             const std::vector<std::string_view>& kwarg_keys,
                             ParamsOf params_of, IsVariadic is_variadic_of,
                             MinArityOf min_arity_of, NamesOf names_of) {
  auto has_kwarg = [&](std::string_view n) {
    for (auto k : kwarg_keys) if (k == n) return true;
    return false;
  };
  std::vector<int> score(arg_types.size());
  std::vector<int> best_score(arg_types.size());
  size_t best_idx = 0;
  bool have_best = false;
  bool best_variadic = false;
  size_t best_regular = 0;
  bool ambiguous = false;
  for (size_t i = 0; i < methods.size(); i++) {
    const auto& params = params_of(methods[i]);
    bool variadic = is_variadic_of(methods[i]);
    size_t min_a = min_arity_of(methods[i]);
    // Too many positional args (a fixed-arity entry can't absorb them).
    if (!variadic && arg_types.size() > params.size()) continue;
    // Every required param past the positional prefix must be named by a kwarg
    // (defaults are trailing, so required params are the leading [0, min_a)).
    if (arg_types.size() < min_a) {
      const auto& names = names_of(methods[i]);
      bool covered = true;
      for (size_t r = arg_types.size(); r < min_a; r++) {
        if (r >= names.size() || !has_kwarg(names[r])) { covered = false; break; }
      }
      if (!covered) continue;
    }
    bool ok = true;
    for (size_t p = 0; p < arg_types.size(); p++) {
      // Positions beyond the regular params are absorbed by `*args` and
      // score as Any (0) so concrete fixed-arity matches outrank them.
      int s = p < params.size()
                  ? multifn_specificity(params[p], arg_types[p])
                  : 0;
      if (s < 0) { ok = false; break; }
      score[p] = s;
    }
    if (!ok) continue;
    if (!have_best) {
      best_idx = i;
      best_score = score;
      best_variadic = variadic;
      best_regular = params.size();
      have_best = true;
      continue;
    }
    bool better_any = false, worse_any = false;
    for (size_t p = 0; p < arg_types.size(); p++) {
      if (score[p] > best_score[p]) better_any = true;
      if (score[p] < best_score[p]) worse_any = true;
    }
    auto take = [&] {
      best_idx = i;
      best_score = score;
      best_variadic = variadic;
      best_regular = params.size();
      ambiguous = false;
    };
    if (better_any && !worse_any) {
      take();
    } else if (!better_any && !worse_any) {
      // Equal type-specificity. Break the tie by, in order: a fixed-arity
      // entry beats a variadic one (`*args` is the fallback); among two
      // variadic entries, the one with more regular params is more
      // specific. Anything still even is genuinely ambiguous.
      if (best_variadic && !variadic) {
        take();
      } else if (best_variadic && variadic) {
        if (params.size() > best_regular) take();
        else if (params.size() == best_regular) ambiguous = true;
      } else if (!best_variadic && !variadic) {
        // Both fixed-arity, equal type-specificity: prefer the entry that
        // fills fewer params by default (the more exact arity match). Equal
        // regular counts are genuinely ambiguous.
        if (params.size() < best_regular) take();
        else if (params.size() == best_regular) ambiguous = true;
      }
    }
  }
  if (!have_best) return -1;
  if (ambiguous)  return -2;
  return static_cast<int64_t>(best_idx);
}

// Normalize a slice range against a sequence length: resolve negative
// indices from the end, apply the inclusive end (`..=`), then clamp both
// ends to [0, len] and force an empty window when start > end. Returns a
// half-open [start, end). Shared by interp + JIT slicing so the two
// backends stay symmetric.
inline std::pair<size_t, size_t> _slice_bounds(int64_t lo, int64_t hi,
                                               bool inclusive, size_t len) {
  int64_t n = static_cast<int64_t>(len);
  if (lo < 0) lo += n;
  if (hi < 0) hi += n;
  // `..=` includes the end. Skip the increment at LONG_MAX so a huge
  // endpoint (`xs[0..=<LONG_MAX>]`) doesn't signed-overflow; it clamps to
  // `n` below either way, so the result is unchanged for every other input.
  if (inclusive && hi != std::numeric_limits<int64_t>::max()) hi += 1;
  if (lo < 0) lo = 0;
  if (lo > n) lo = n;
  if (hi < 0) hi = 0;
  if (hi > n) hi = n;
  if (hi < lo) hi = lo;
  return {static_cast<size_t>(lo), static_cast<size_t>(hi)};
}

}  // namespace culebra
