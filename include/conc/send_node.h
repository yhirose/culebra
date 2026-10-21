#pragma once

// The neutral transfer representation (SendNode) plus the one helper every
// backend's serializer shares: send_error (one SendError spelling). Owns no
// GC-heap pointer and no engine type, so
// the compiled lanes' serializer (sendable_rt.h) and the isolate/channel
// core (isolate_core.h) can include it without the interp. Moved verbatim
// from sendable.h (Phase 4 B7-b).

#include <peglib.h>
#include <base/shared.h>  // CulebraError

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace culebra::sendable {

// ---------------------------------------------------------------------------
// Neutral representation. Owns no GC-heap pointer. The shared_ptrs it holds
// have atomic control blocks: `body` (a process-lifetime peg::Ast) and `held`
// (a count on a handle, given back on whichever thread drops the last copy).
// ---------------------------------------------------------------------------
struct SendNode {
  // Unbound is the "nothing has bound this yet" sentinel, in `i` with the
  // payload that says which kind. It travels because a session unit's closure
  // captures a cell for every name the unit does not bind (a stdlib name's
  // stays unbound) — and so does a closure over a `self` no frame supplied.
  // Either way the receiver re-asks against its own Runtime.
  enum class K { Nil, Bool, Long, Float, Str, Array, Object, Set, Tuple,
                 Closure, Channel, SharedBuffer, BufView, SharedVal, Unbound };
  K kind = K::Nil;

  bool b = false;  // Bool; also Channel role (false = tx, true = rx)
  int64_t i = 0;   // Long; also a Channel word, a BufView's first byte
  double d = 0.0;
  // What a node carrying a handle by reference (a Channel end, a SharedBuffer,
  // a Shared tree) holds on it in flight: the count serialize took, given
  // back when the last copy of the node goes, consumed or not. Empty for a
  // dropped handle and for every other kind.
  std::shared_ptr<void> held;
  std::string s;   // Str; also a BufView's field type (empty: `buf[i]`)

  std::vector<SendNode> elems;  // Array / Set / Tuple; a BufView's buffer

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
  // …and what that closure IS (rt_value.inc.h JIT_CLOSURE_*). A property the
  // receiving Runtime cannot infer: it never ran the class declaration the
  // getter came from, and the code address it would have to ask is not an
  // identity across Runtimes. So the closure carries its own answers over.
  uint64_t jit_flags = 0;
  // …and its body's metadata, a constant of the same module. Borrowed, never
  // owned: it outlives every closure over that body.
  void* jit_meta = nullptr;
  // Closure — JIT multifn dispatcher (`fn name`): the dispatcher thunk shares
  // fn_ptr, but its overload methods live in a per-Runtime table, so they are
  // shipped explicitly (method bodies in `elems`, types/variadic parallel) and
  // registered on the receiver over a table of its own. mf_name is the
  // user-facing name (never the sender's registry key); non-empty selects
  // this form.
  std::string mf_name;
  std::vector<std::vector<std::string>> mf_param_types;
  std::vector<std::vector<std::string>> mf_param_names;
  std::vector<bool> mf_variadic;
  std::vector<size_t> mf_min_params;
  int ref_id = -1;          // closure identity (for back-references)
  bool is_backref = false;  // true ⇒ this node is just a ref to ref_id
  // Object: the Error Object a catch built (JitObject::is_error), which a
  // re-throw on the receiving side still reports as that error.
  bool is_error = false;
  // Object, a class meta: which Dir module class it is (JitSpecialTable::
  // dir_kind), a fact of the meta's C++ side that the entries cannot carry.
  int8_t dir_kind = 0;
  // Object, a class meta: a lowering's (JitObject::is_lowered_state), whose
  // instances' own slots hand a function back unbound; the same kind of fact.
  bool lowered_state = false;

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

// A trait's default-method body in transit, with the (trait, method) it is
// registered under. The default table is per-Runtime and only a trait
// declaration fills it, so a worker Runtime — which executes only the closure
// it was sent — keeps the parent's table serialized and rebuilds an entry the
// first time a lookup misses it (sendable.h: jit_snapshot_trait_defaults /
// jit_install_shipped_default).
struct TraitDefaultNode {
  std::string trait;
  std::string method;
  SendNode body;
};
using TraitDefaults = std::vector<TraitDefaultNode>;

[[noreturn]] inline void send_error(const std::string& what) {
  throw culebra::CulebraError("SendError", what);
}

}  // namespace culebra::sendable
