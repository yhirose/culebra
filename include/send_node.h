#pragma once

// The neutral transfer representation (SendNode) plus the two helpers every
// backend's serializer shares: send_error (one SendError spelling) and the
// Shared.new freeze flag. Owns no GC-heap pointer and no engine type, so
// the compiled lanes' serializer (sendable_jit.h) and the isolate/channel
// core (isolate_core.h) can include it without the interp. Moved verbatim
// from sendable.h (Phase 4 B7-b).

#include <peglib.h>
#include <shared.h>  // CulebraError

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace culebra::sendable {

// ---------------------------------------------------------------------------
// Neutral representation. Owns no GC-heap pointer. The only cross-heap-safe
// shared_ptr it may hold is shared_ptr<peg::Ast> (process-lifetime, atomic
// control block).
// ---------------------------------------------------------------------------
struct SendNode {
  // Unbound is the "nothing has bound this yet" sentinel, in `i` with the
  // payload that says which kind. It travels because a session unit's closure
  // captures a cell for every name the unit does not bind (a stdlib name's
  // stays unbound) — and so does a closure over a `self` no frame supplied.
  // Either way the receiver re-asks against its own Runtime.
  enum class K { Nil, Bool, Long, Float, Str, Array, Object, Set, Tuple,
                 Closure, Channel, SharedBuffer, SharedVal, EmbedDir,
                 Unbound };
  K kind = K::Nil;

  bool b = false;  // Bool; also Channel role (false = tx, true = rx)
  int64_t i = 0;   // Long; also Channel id (into the process-wide registry)
  double d = 0.0;
  std::string s;   // Str

  std::vector<SendNode> elems;  // Array / Set / Tuple

  // Object: ordered (key, value) pairs + parallel mutability flags. Keys are
  // themselves SendNodes (String keys → K::Str; non-String keys serialize as
  // values). A nested by-value Entry struct can't be defined inside the still-
  // incomplete SendNode, but a vector of std::pair<SendNode,SendNode> is fine.
  std::vector<std::pair<SendNode, SendNode>> entries;
  std::vector<bool> entry_mut;

  // Closure — interp form: defining AST + named free-var captures.
  const peg::Ast* params_ast = nullptr;            // borrowed, process-lifetime
  std::shared_ptr<peg::Ast> body;                  // shared, thread-safe
  std::string return_type;
  std::vector<std::pair<std::string, SendNode>> captures;  // free var → value
  // Closure — JIT form: shared native code pointer (process-lifetime while the
  // LLJIT is alive) + positional captures in `elems` + arity in `i`. The two
  // forms never mix: an interp program ships interp closures, a JIT program
  // ships JIT closures. `jit_fn != nullptr` selects the JIT form.
  void* jit_fn = nullptr;
  // …and what that closure IS (jit_value.h JIT_CLOSURE_*). A property the
  // receiving Runtime cannot infer: it never ran the class declaration the
  // getter came from, and the code address it would have to ask is not an
  // identity across Runtimes. So the closure carries its own answers over.
  uint64_t jit_flags = 0;
  // Closure — JIT multifn dispatcher (`fn name`): the dispatcher thunk shares
  // fn_ptr, but its overload methods live in a thread_local table, so they are
  // shipped explicitly (method bodies in `elems`, types/variadic parallel) and
  // re-registered on the child. mf_name non-empty selects this form.
  std::string mf_name;
  std::vector<std::vector<std::string>> mf_param_types;
  std::vector<std::vector<std::string>> mf_param_names;
  std::vector<bool> mf_variadic;
  std::vector<size_t> mf_min_params;
  int ref_id = -1;          // closure identity (for back-references)
  bool is_backref = false;  // true ⇒ this node is just a ref to ref_id

  // Declared here, defaulted below the class, for the reason toml::Node's are
  // (toml.h): the self-referential vector<pair<SendNode, SendNode>> makes
  // clang's evaluation of the implicit ones circular.
  SendNode();
  ~SendNode();
  SendNode(const SendNode&);
  SendNode(SendNode&&) noexcept;
  SendNode& operator=(const SendNode&);
  SendNode& operator=(SendNode&&) noexcept;
};
inline SendNode::SendNode() = default;
inline SendNode::~SendNode() = default;
inline SendNode::SendNode(const SendNode&) = default;
inline SendNode::SendNode(SendNode&&) noexcept = default;
inline SendNode& SendNode::operator=(const SendNode&) = default;
inline SendNode& SendNode::operator=(SendNode&&) noexcept = default;

[[noreturn]] inline void send_error(const std::string& what) {
  throw culebra::CulebraError("SendError", what);
}

// True while Shared.new is freezing a value (serialize-as-freeze). The
// extract hooks consult it to SKIP the in-flight refcount bump: a freeze
// is synchronous (no cross-thread in-flight window to protect) and Shared
// rejects every handle anyway, so the bump would only ever need undoing
// — and undoing it on the serialize-throws path (a cycle past an already
// extracted handle) is exactly the leak this avoids. Set by a RAII guard
// in make_shared_namespace / _ns_shared_new.
inline bool& sharedval_freezing() {
  static thread_local bool f = false;
  return f;
}
struct FreezeGuard {
  bool prev;
  FreezeGuard() : prev(sharedval_freezing()) { sharedval_freezing() = true; }
  ~FreezeGuard() { sharedval_freezing() = prev; }
};

}  // namespace culebra::sendable
