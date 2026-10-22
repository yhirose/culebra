#pragma once

// The runtime side of isolate value transfer (C2-c): JitValue <-> SendNode.
//
// Culebra's isolates follow the architecture every isolate-based runtime uses
// (V8 Workers, Ruby Ractor, Java, Go, Erlang): **immutable code is shared, the
// mutable heap is isolated, only data is copied.** The shared code here is the
// compiled `JitClosure::fn_ptr`, live as long as the engine that produced it —
// which spans the whole run, and therefore every isolate joined within it.
//
// So a closure crosses a thread boundary as (fn_ptr + positionally-copied
// captures) — no AST, no names: the compiled body reads captures[i] by index,
// so the child rebuilds the cells in the same order. The child runs on its own
// heap (a fresh Runtime) and invokes the shared fn_ptr. Values cross as the
// neutral sendable::SendNode, which is what keeps this symmetric across the
// executor and the LLVM lane.

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "conc/isolate_core.h"  // IsolateCore, SendNode, isolate_cap, g_live_isolates
#include "conc/sharedval_core.h"  // SharedValCore and its counts
#include "rt/rt.h"       // JitValue and the runtime constructors

namespace culebra {

// --- JitValue <-> SendNode ---------------------------------------------------

struct JitSerCtx {
  // A closure, a class object, an enum object or a meta -> its ref id: one
  // node per message however often it is reached.
  std::map<const void*, int> ids;
  int next_id = 0;
  std::set<const void*> visiting;          // container cycle guard

  // Numbers `n` for `p` on its first reach, before its walk, so a reach
  // from inside that walk refers back (true); a later reach makes `n` a
  // backref (false). The receiver's twin is JitDeCtx::remember / recall.
  bool first(const void* p, sendable::SendNode& n) {
    auto [it, fresh] = ids.try_emplace(p, next_id);
    n.ref_id = it->second;
    n.is_backref = !fresh;
    if (fresh) next_id++;
    return fresh;
  }
};
// One Runtime's hold on a frozen tree: what a view's kSvHoldSlot points at
// (rt/string.inc.h), shared by a view the Runtime was handed (Shared.new's,
// one a message carried) and every view made off it. The hold has one count
// on the tree, taken when it is made and given back when its last view goes.
// So the views a walk makes and drops, one per row, write nothing another
// isolate reads: counting them is this Runtime's own business. (A count on
// the tree per view had 8 isolates walking one tree take 12 times as long
// per row as one.)
struct JitSvHold {
  const culebra::SharedValCore* core;
  int64_t id;        // the tree's, in the registry
  JitObject* meta;   // the Runtime's view meta (borrowed: it is pinned)
  int64_t views = 0;
  bool parked = false;  // its last view went during a read (JitSharedValReads)
};
// Null once the view is dropped.
inline JitSvHold* _jit_sv_hold(const JitObject* view) {
  return reinterpret_cast<JitSvHold*>(view->slots[kSvHoldSlot].value.data);
}

// A channel end's C++ half (a handle, see JitHandle): its channel and which
// end it is. It holds one count on that end (ChannelCore's tx_count or
// rx_count) from when it is made until drop() or the Object's death gives it
// back. A dropped end reads as a closed channel.
struct JitChanEnd final : JitHandle {
  ChannelCore* core;  // null once dropped
  const int role;     // 0 = tx, 1 = rx
  JitChanEnd(ChannelCore* c, int r)
      : JitHandle(Kind::ChannelEnd), core(c), role(r) {}
  ~JitChanEnd() override { release(); }
  void release() {
    if (auto* c = std::exchange(core, nullptr)) chan_drop(c, role);
  }
  std::string show() const override {
    std::string s = core ? "[Channel " : "[dropped Channel ";
    return s + (role == 0 ? "tx]" : "rx]");
  }
  // A clone is its original's equal; a dropped end, only its own.
  bool same(const JitHandle& other) const override {
    auto* o = other.kind == kind ? static_cast<const JitChanEnd*>(&other)
                                 : nullptr;
    return o && core && o->core == core && o->role == role;
  }
};
inline JitChanEnd* _jit_chan_end(const JitObject* o) {
  auto* h = o->handle_of();
  return h && h->kind == JitHandle::Kind::ChannelEnd
             ? static_cast<JitChanEnd*>(h)
             : nullptr;
}
inline JitChanEnd* _jit_chan_end(JitValue v) {
  return v.tag == TAG_OBJECT ? _jit_chan_end(reinterpret_cast<JitObject*>(v.data))
                             : nullptr;
}

// A merged rx's C++ half (Channel.fan_in): its share of the merge it reads.
struct JitMergedRx final : JitHandle {
  std::shared_ptr<ChannelMerge> merge;  // null once dropped
  explicit JitMergedRx(std::shared_ptr<ChannelMerge> m)
      : JitHandle(Kind::MergedRx), merge(std::move(m)) {}
  std::string show() const override {
    return merge ? "[merged Channel rx]" : "[dropped merged Channel rx]";
  }
  // Nothing makes a second rx of one merge.
  bool same(const JitHandle&) const override { return false; }
};

// An Isolate.spawn handle's C++ half: its share of the isolate's core.
struct JitIsolate final : JitHandle {
  std::shared_ptr<IsolateCore> core;  // null once dropped
  explicit JitIsolate(std::shared_ptr<IsolateCore> c)
      : JitHandle(Kind::Isolate), core(std::move(c)) {}
  std::string show() const override {
    return core ? "[Isolate]" : "[dropped Isolate]";
  }
  // Nothing makes a second handle of one isolate.
  bool same(const JitHandle&) const override { return false; }
};

// An rx's for-in cursor's C++ half (see _jit_chan_iter): what it reads, held
// on its own while it has it (an rx count on a channel, or a share of a
// merge), and the value has_next() took.
struct JitChanIter final : JitHandle {
  ChannelCore* core = nullptr;          // a plain rx's channel
  std::shared_ptr<ChannelMerge> merge;  // a merged rx's merge
  std::optional<sendable::SendNode> taken;
  // Both null once the close is met, or when made from a dropped rx.
  explicit JitChanIter(ChannelCore* c) : JitHandle(Kind::ChannelIter), core(c) {}
  explicit JitChanIter(std::shared_ptr<ChannelMerge> m)
      : JitHandle(Kind::ChannelIter), merge(std::move(m)) {}
  ~JitChanIter() override { close(); }
  // The next value into `taken`, or let the source go once it is met closed.
  void pull() {
    if (taken || (!core && !merge)) return;
    taken = core ? chan_pop_blocking(*core) : chan_select_recv(*merge);
    if (!taken) close();
  }
  void close() {
    if (auto* c = std::exchange(core, nullptr)) chan_drop(c, /*role=*/1);
    merge.reset();
  }
  std::string show() const override { return "[Channel rx iterator]"; }
  bool same(const JitHandle&) const override { return false; }
};

// What one message's rebuild remembers: the node JitSerCtx::first numbered,
// which each later backref names.
struct JitDeCtx {
  std::map<int, JitValue> rebuilt;  // ref id -> the value
  // A reference on each until the message is rebuilt, so a later backref
  // finds it whatever becomes of the node that carried it. A heap Array,
  // which a collection reaches from this frame, as it does not a map's nodes.
  JitOwnedVal held{TAG_NIL, 0};

  void remember(int id, JitValue v) {
    if (held.borrow().tag == TAG_NIL)
      held = JitOwnedVal(TAG_ARRAY,
                         reinterpret_cast<int64_t>(culebra_runtime_array_new()));
    JitValue ref = JitOwnedVal::from_borrowed(v).consume();
    culebra_runtime_array_push(reinterpret_cast<JitArray*>(held.borrow().data),
                               ref.tag, ref.data);
    rebuilt.emplace(id, v);
  }
  // A reference of its own, like any other node's.
  JitOwnedVal recall(int id) const {
    assert(rebuilt.contains(id) && "a backref ahead of its node");
    return JitOwnedVal::from_borrowed(rebuilt.at(id));
  }
  // A Set member, and an entry under a key that is no String, put where its
  // sender held it: under the hash the sender stored the key under
  // (SendNode::hash), with no hash() run on this side, whose class and
  // statics may still be mid-rebuild, and no eq() either, as the sender's
  // keys were apart from each other. Each takes the +1s it is handed.
  static void put_member(JitSet* s, JitValue m, uint64_t hash) {
    DistinctKeys distinct;
    [[maybe_unused]] bool added = _jit_set_insert(
        s, JitHashedKey::restore(m, static_cast<size_t>(hash)));
    assert(added && "a member its sender held apart");
  }
  static void put_entry(JitObject* o, JitValue key, uint64_t hash,
                        JitValue val, bool mut) {
    DistinctKeys distinct;
    [[maybe_unused]] bool added = _jit_object_set_hashed(
        o, JitHashedKey::restore(key, static_cast<size_t>(hash)), mut, val, 0,
        0, /*is_init=*/true);
    assert(added && "a key its sender held apart");
  }

 private:
  // Open while a key is put: two Objects are one key only if they are one
  // Object (JitValueEq), and no program code runs (_jit_invoke_rooted).
  struct DistinctKeys {
    DistinctKeys() { ++_jit_thread.keys_distinct; }
    ~DistinctKeys() { --_jit_thread.keys_distinct; }
    DistinctKeys(const DistinctKeys&) = delete;
    DistinctKeys& operator=(const DistinctKeys&) = delete;
  };
};

// The node of a value that holds no other value; false for one that does.
inline bool _jit_scalar_node(const JitValue& v, sendable::SendNode& n) {
  using K = sendable::SendNode::K;
  switch (v.tag) {
    case TAG_NIL:   n.kind = K::Nil; return true;
    case TAG_BOOL:  n.kind = K::Bool; n.b = v.data != 0; return true;
    case TAG_LONG:  n.kind = K::Long; n.i = v.data; return true;
    case TAG_FLOAT:
      n.kind = K::Float;
      n.d = _culebra_float_to_double(v.data);
      return true;
    case TAG_STRING:
    case TAG_STRINGVIEW:
      n.kind = K::Str;
      n.s = std::string(_culebra_str_view(v.tag, v.data));
      return true;
    default: return false;
  }
}

// A class meta holds its class's name (and, for a variant, its enum's) in C++
// fields rather than slots, so a walk of its entries cannot carry them. They
// go along as one String, separated by '/' (no identifier contains one),
// which _jit_rebuilt_object_done reads back. Without it the receiver rebuilds
// a nameless meta and every instance behind it loses the one thing that says
// what it is. Empty for an Object that is no meta.
inline std::string _jit_class_meta_names(const JitObject* o) {
  if (!o->is_class_meta || !o->specials || !o->specials->name) return {};
  std::string names = o->specials->name;
  if (o->specials->enum_name) {
    names += '/';
    names += o->specials->enum_name;
  }
  return names;
}

// object-facts: begin
// What an Object is, member by member, when a message or a Shared tree
// rebuilds it on another heap. A member the rebuild knew nothing of was lost
// without a sound (a received typed field took any value, a meta ran its own
// `drop`), so tools/checks/check_object_facts.sh holds every data member of
// JitObject to a row here.
//   slots             entries: stored in the sender's order
//   non_string_props  entries: the ones a non-String key holds
//   key_order         entries: their order, kept by storing them in it
//   shape             entries: the sender's (ObjectFacts::decl), types and all
//   is_dict           entries: a dictionary where the sender's was one
//   dict_             entries: a dictionary's index, built as it fills
//   has_drop          carried: registered where the sender's was
//   dropped           carried: a drop that ran does not run again
//   is_error          carried: a re-throw still reports the error it was
//   is_match          carried: a Regex match reads its groups by `m[i]`
//   frozen            carried: a @value instance's fields stay as they are
//   fields_closed     carried: and none is added
//   proto_            restored: an instance's class meta, the node after it
//   is_class_meta     restored: a meta is sealed as declared
//   specials          restored: by the seal
//   is_lowered_state  restored: with the meta's flags (meta_flags)
//   names_class       restored: with the meta's flags
//   is_value          restored: with the meta's flags
//   methods_drop      restored: the seal recounts it from the methods
//   refcount          the receiver's own
//   gc_slot           the receiver's own
//   mut_count         the receiver's own
//   owned_idx         the receiver's own owned stack
//   cls               restored: an instance's class, when it takes it along
//                     (_jit_class_along); else none
//   is_class          restored: a class node (SendNode::is_class); a Shared
//                     tree refuses a class
//   instance_meta     restored: a class node's meta
//   is_enum           restored: an enum node (SendNode::is_enum); a Shared
//                     tree keeps an enum object as a plain Object
//   enum_ref          restored: the rebuilt enum's, named by the variants it
//                     declared (the node's elems); a variant that came
//                     without its enum names none
//   is_namespace      left behind: a namespace's members are native functions
//   ns_name           left behind: likewise
//   has_handle        left behind: a handle is refused or is a node of its own
//   handle            left behind: likewise
//   is_gen_frame      left behind: a generator is refused
//   gen_frame         left behind: likewise
//   fast_next_fn      left behind: an iterator's, and an iterator is refused
//   is_shared_buffer  left behind: a node of its own (SharedBuffer)
//   is_shared_val     left behind: a node of its own (SharedVal)
// object-facts: end

// The flags the table says are carried, by position in ObjectFacts::bits.
// has_drop takes the bit after them: putting it back registers a drop.
inline constexpr bool JitObject::* const kCarriedFacts[] = {
    &JitObject::dropped, &JitObject::is_error, &JitObject::is_match,
    &JitObject::frozen,  &JitObject::fields_closed};
inline constexpr uint32_t kFactHasDrop = 1u << std::size(kCarriedFacts);

inline culebra::ObjectFacts _jit_object_facts(const JitObject* o) {
  culebra::ObjectFacts f;
  for (size_t i = 0; i < std::size(kCarriedFacts); i++)
    if (o->*kCarriedFacts[i]) f.bits |= 1u << i;
  if (o->has_drop) f.bits |= kFactHasDrop;
  f.decl = o->is_dict ? nullptr : o->shape;
  if (o->is_class_meta && o->specials) {
    f.meta_flags = static_cast<uint32_t>(_jit_class_meta_flags(o));
    f.dir_kind = o->specials->dir_kind;
  }
  return f;
}

// The handle an Object is, if it is one: a thing a message carries by
// reference or not at all, and never data.
enum class JitHandleKind {
  None,
  Channel,
  SharedBuffer,
  BufView,
  SharedVal,
  Generator,
  Native,
};
inline JitHandleKind _jit_handle_kind(JitObject* o) {
  constexpr auto none = static_cast<size_t>(-1);
  if (_jit_chan_end(o)) return JitHandleKind::Channel;
  if (o->is_shared_buffer) return JitHandleKind::SharedBuffer;
  if (_jit_buf_view(o)) return JitHandleKind::BufView;
  if (o->has_handle) return JitHandleKind::Native;
  if (o->is_shared_val) return JitHandleKind::SharedVal;
  if (o->is_gen_frame) return JitHandleKind::Generator;
  // A native handle says so with a slot of its own; a wrapped class says it
  // once, on the meta all its handles share.
  if (o->find_slot("__nonsendable__") != none || _jit_meta_nonsendable(o))
    return JitHandleKind::Native;
  return JitHandleKind::None;
}

inline sendable::SendNode jit_serialize(JitValue v, JitSerCtx& ctx);

// The class object an instance takes along to the other side, or null: the
// one its methods name (names_class), which they read through the instance.
inline JitObject* _jit_class_along(const JitObject* o) {
  auto* meta = o->proto();
  return meta && meta->names_class ? o->built_by() : nullptr;
}

// An Object's node. A class object, an enum object and a meta (what an
// instance's methods live on: a class meta, or one a native module keeps,
// reached `as_meta` through an instance) are one node per message: a
// variant's meta names one enum, so a second copy of its enum is no enum.
inline sendable::SendNode _jit_object_node(JitObject* o, JitSerCtx& ctx,
                                           bool as_meta) {
  using K = sendable::SendNode::K;
  sendable::SendNode n;
  switch (_jit_handle_kind(o)) {
    // Channel endpoint — the Sendable exception: ship its channel + role,
    // and a count of the node's own on it. A dropped end ships as one.
    case JitHandleKind::Channel: {
      auto* end = _jit_chan_end(o);
      if (end->core) chan_bump(end->core, end->role);
      return chan_node(end->core, end->role);
    }
    // SharedBuffer — the other shared-reference exception: the node holds
    // a ref of its own on the bytes. A dropped buffer ships as one.
    case JitHandleKind::SharedBuffer: {
      n.kind = K::SharedBuffer;
      n.held = _jit_shared_buf(o)->core;
      return n;
    }
    // A view of a SharedBuffer: the buffer, as above, and where in its
    // bytes the view reads what (a field's type; a record of the
    // buffer's class has none).
    case JitHandleKind::BufView: {
      auto* view = _jit_buf_view(o);
      n.kind = K::BufView;
      n.i = view->off;
      n.s = std::string(view->type());
      n.elems.push_back(jit_serialize(view->buf, ctx));
      return n;
    }
    // Shared.new view: ship the frozen tree by (id, node) reference, with
    // a count of the node's own on it (the receiver's view takes another
    // at rebuild).
    case JitHandleKind::SharedVal: {
      auto* hold = _jit_sv_hold(o);
      if (!hold) {
        throw culebra::CulebraError("ClosedError",
                                    "Shared value has been dropped");
      }
      n.held = culebra::shared_val_held(*hold->core, hold->id);
      n.kind = K::SharedVal;
      n.i = hold->id;
      n.ref_id = static_cast<int>(o->slots[kSvNodeSlot].value.data);
      return n;
    }
    // A generator is a frame of this thread's, part-way through its body.
    case JitHandleKind::Generator:
      sendable::send_error("a generator is not Sendable");
    case JitHandleKind::Native:
      sendable::send_error("a native handle is not Sendable");
    case JitHandleKind::None: break;
  }
  // A @value instance freezes when its `new` returns, and a copy made
  // before then never would: sent from inside it, or left behind by one
  // that threw.
  if (o->fields_closed && !o->frozen)
    sendable::send_error(
        "a @value instance whose `new` has not returned is not Sendable");
  n.kind = K::Object;
  if ((as_meta || o->is_class_meta || o->class_meta_of() || o->is_enum) &&
      !ctx.first(o, n))
    return n;
  culebra::ValueWalkFrame walk;
  // What the Object is (SendNode::elems) goes ahead of what it holds, and
  // before it counts as visited: an instance a static of its own class
  // holds is reached again from there, and copied there as any second
  // reach of an Object is.
  const char* cls_name = nullptr;
  if (auto* meta = o->class_meta_of()) {
    cls_name = meta->specials->name;
    // A record type's byte layout (JitRecordInfo) is its declaring
    // Runtime's.
    if (meta->specials->record)
      sendable::send_error(std::string(cls_name) +
                           ": a @packable class is not Sendable");
    n.is_class = true;
    n.elems.push_back(_jit_object_node(meta, ctx, /*as_meta=*/true));
  } else if (auto* meta = o->proto()) {
    n.elems.push_back(_jit_object_node(meta, ctx, /*as_meta=*/true));
    if (auto* cls = _jit_class_along(o))
      n.elems.push_back(_jit_object_node(cls, ctx, /*as_meta=*/false));
  } else if (o->is_enum) {
    // The variants it declared, which name it back on the other side.
    n.is_enum = true;
    o->for_each([&](std::string_view key, const JitObjectEntry& entry) {
      if (auto* m = _jit_declared_variant(o, key, entry.value)) {
        cls_name = m->specials->enum_name;
        n.elems.push_back(_jit_object_node(m, ctx, /*as_meta=*/true));
      }
    });
  }
  if (!ctx.visiting.insert(o).second)
    sendable::send_error("a cyclic value cannot be sent");
  // The Object's own facts (the object-facts table above), the instance's
  // drop gate among them: carried rather than re-derived from the meta on
  // the other side, since a nested instance can relink while its meta is
  // still mid-fill (the memo registers before the entries land).
  n.facts = _jit_object_facts(o);
  n.s = _jit_class_meta_names(o);
  // A member of a class or an enum that cannot cross says which, as
  // `Class.member`: the class may have gone along with an instance, and the
  // enum with a constructor, unseen by the sender.
  auto member = [&](std::string_view key, JitValue v) {
    try {
      return jit_serialize(v, ctx);
    } catch (const culebra::CulebraError& e) {
      if (!cls_name) throw;
      sendable::rethrow_send_error_in(
          e, std::string(cls_name) + "." + std::string(key));
    }
  };
  // Every entry, in the order the Object holds them: the receiver stores
  // them as they arrive, and that is the order it then iterates in. A key
  // that is no String serializes as a value node.
  _jit_each_entry(
      o,
      [&](std::string_view name, const JitObjectEntry& entry) {
        sendable::SendNode key;
        key.kind = K::Str;
        key.s = std::string(name);
        n.entries.emplace_back(std::move(key),
                               member(name, entry.value));
        n.entry_mut.push_back(entry.mut);
      },
      [&](const JitHashedKey& k, const JitObjectEntry& entry) {
        // The value before its key, as the receiver rebuilds them: the
        // first of two instances of a class carries the class, and the
        // other refers back to it.
        auto val = jit_serialize(entry.value, ctx);
        auto key = jit_serialize(k.value(), ctx);
        key.hash = k.hash();
        n.entries.emplace_back(std::move(key), std::move(val));
        n.entry_mut.push_back(entry.mut);
      });
  ctx.visiting.erase(o);
  return n;
}

inline sendable::SendNode jit_serialize(JitValue v, JitSerCtx& ctx) {
  using K = sendable::SendNode::K;
  sendable::SendNode n;
  if (_jit_scalar_node(v, n)) return n;
  auto seq = [&](JitArray* a, K kind) -> sendable::SendNode {
    if (!ctx.visiting.insert(a).second)
      sendable::send_error("a cyclic value cannot be sent");
    culebra::ValueWalkFrame walk;
    n.kind = kind;
    n.elems.reserve(a->size);
    for (size_t i = 0; i < a->size; i++)
      n.elems.push_back(jit_serialize(a->items[i], ctx));
    ctx.visiting.erase(a);
    return n;
  };
  switch (v.tag) {
    case TAG_NO_SELF: n.kind = K::Unbound; n.i = v.data; return n;
    case TAG_ARRAY:  return seq(reinterpret_cast<JitArray*>(v.data), K::Array);
    case TAG_TUPLE:  return seq(reinterpret_cast<JitArray*>(v.data), K::Tuple);
    case TAG_SET: {
      auto* s = reinterpret_cast<JitSet*>(v.data);
      if (!ctx.visiting.insert(s).second)
        sendable::send_error("a cyclic value cannot be sent");
      culebra::ValueWalkFrame walk;
      n.kind = K::Set;
      n.elems.reserve(s->members.size());
      for (const auto& m : s->members) {
        n.elems.push_back(jit_serialize(m.value(), ctx));
        n.elems.back().hash = m.hash();
      }
      ctx.visiting.erase(s);
      return n;
    }
    case TAG_OBJECT:
      return _jit_object_node(reinterpret_cast<JitObject*>(v.data), ctx,
                              /*as_meta=*/false);
    case TAG_FUNC: {
      auto* c = reinterpret_cast<JitClosure*>(v.data);
      n.kind = K::Closure;
      if (!ctx.first(c, n)) return n;  // a recursive fn refers back to itself
      // Closure chains nest through captures, not containers, so the
      // Function node counts a level too (mirrors the interp serializer).
      culebra::ValueWalkFrame walk;
      n.jit_fn = c->fn_ptr;
      n.jit_flags = c->flags;
      // The body's metadata is a constant of the module both Runtimes run
      // (the LLJIT outlives every closure over that body), so the rebuilt
      // closure points at the same one rather than losing it.
      n.jit_meta = const_cast<void*>(static_cast<const void*>(c->meta));
      n.i = static_cast<int64_t>(c->arity);
      // `fn name` is a dispatcher thunk: its overloads live in its record,
      // so ship each method body + its dispatch types explicitly.
      if (const auto* rec = _jit_dispatcher_record(c)) {
        n.mf_name = rec->name;
        for (const auto& mth : rec->methods) {
          n.mf_param_types.push_back(mth.param_types);
          n.mf_param_names.push_back(mth.param_names);
          n.mf_variadic.push_back(mth.variadic);
          n.mf_min_params.push_back(mth.min_params);
          n.elems.push_back(jit_serialize(
              JitValue{TAG_FUNC, reinterpret_cast<int64_t>(mth.body)}, ctx));
        }
        return n;
      }
      // A native (C++-bodied) closure crosses only when PORTABLE (see the
      // flag): its captures are then copied below as any closure's are.
      if ((c->flags & JIT_CLOSURE_NATIVE) && !(c->flags & JIT_CLOSURE_PORTABLE))
        sendable::send_error("a native/builtin function is not Sendable");
      // A `mut` capture is not Sendable: the value would silently diverge from
      // the parent's. Reject at the boundary.
      if (const char* nm = _jit_first_mut_capture_of(c))
        sendable::send_error(
            "closure captures the mutable variable '" + std::string(nm) +
            "' (mutable captures are not Sendable — pass it as an "
            "argument instead)");
      n.elems.reserve(c->n_captures);  // positional captures
      for (size_t i = 0; i < c->n_captures; i++)
        n.elems.push_back(jit_serialize(c->captures[i]->value, ctx));
      return n;
    }
    case TAG_TENSOR:
      // Wording matches the interp serializer exactly (sendable.h).
      sendable::send_error("Tensor is not Sendable (share via SharedBuffer instead)");
  }
  sendable::send_error("value is not Sendable");
}

inline JitValue jit_float(double d) {
  int64_t bits;
  std::memcpy(&bits, &d, sizeof(double));
  return {TAG_FLOAT, bits};
}

inline JitValue _jit_make_channel_endpoint(ChannelCore* core, int role);  // fwd
inline JitValue _jit_make_shared_buffer_handle(culebra::SharedBufferRef core);  // fwd
inline JitValue _jit_make_shared_val_view(JitSvHold* hold,
                                          int64_t node);  // fwd
inline JitValue _jit_shared_val_root(const culebra::SharedValCore* core,
                                     int64_t id, int64_t node);  // fwd

// A rebuilt Object (jit_deserialize's, JitSvThaw's) is put back as the
// sender held it, not by replaying a program's writes: those passed their
// checks where they were made, and what they left behind is in its facts.
// One String-keyed entry, typed as the sender's Shape typed its `nth` String
// key, which this is (an Object holds them in its Shape's order). A shaped
// sender's Object stays shaped however many keys it has: an instance's proto
// is linked after its entries and shares a dictionary's storage. Takes the +1.
inline void _jit_rebuilt_put(JitObject* o, const culebra::ObjectFacts& f,
                             size_t& nth, std::string_view key, JitValue v,
                             bool mut) {
  if (auto* decl = static_cast<const culebra::Shape*>(f.decl)) {
    assert(nth < decl->names.size() && decl->names[nth] == key);
    auto& shapes = culebra::shape_registry();
    if (!o->shape) o->shape = shapes.root();
    o->append_slot_shaped(
        shapes.transition_add(o->shape, key, decl->type_at(nth),
                              decl->annotation_at(nth)),
        v, mut);
  } else {
    o->append_slot(key, v, mut);
  }
  ++nth;
  // A plain Object's `drop` registers it where the sender's key did, so the
  // copies' drops run in the sender's order.
  if ((f.bits & kFactHasDrop) && key == "drop") _jit_owned_bind_drop(o);
}

// What the rebuilt Object is besides its entries, once they are in: a meta
// sealed as its declaration sealed it (`names` is `Name` or `Name/Enum`,
// empty for no meta), an instance linked to its class's meta and, when it
// came along, its class, and its facts last, so a freeze closes over the
// entries already stored.
inline void _jit_rebuilt_object_done(JitObject* o, std::string_view names,
                                     JitObject* meta, JitObject* cls,
                                     const culebra::ObjectFacts& f) {
  if (!names.empty()) {
    auto sep = names.find('/');
    _jit_seal_class_meta(
        o, names.substr(0, sep),
        sep == std::string_view::npos ? std::string_view{}
                                      : names.substr(sep + 1),
        f.meta_flags, f.dir_kind);
  }
  assert(!cls || cls->is_class);
  if (meta) o->link_class(meta, cls);
  for (size_t i = 0; i < std::size(kCarriedFacts); i++)
    o->*kCarriedFacts[i] = ((f.bits >> i) & 1u) != 0;
  // An instance's drop (a plain Object's registered at its key already).
  if (f.bits & kFactHasDrop) _jit_owned_bind_drop(o);
}

// A field of type `type` in `layout`, or in a record nested in it, or null.
inline const culebra::PackableField* _jit_field_of_type(
    const culebra::PackableLayout& layout, std::string_view type) {
  for (const auto& f : layout.fields) {
    if (f.type == type) return &f;
    if (f.layout.is_struct)
      if (auto* hit = _jit_field_of_type(*f.layout.record, type)) return hit;
  }
  return nullptr;
}

// The value `n` describes, rebuilt on this Runtime's heap and owned from the
// moment it exists. A container is owned before its members are rebuilt, and
// each member goes into it with its +1 (the runtime's adds consume it on every
// exit), so a rebuild that throws part-way lets go of everything it built.
// It runs no program code, as it may meet a class whose meta and statics are
// still being rebuilt: a Set member or a non-String key goes in under the
// hash it was sent with, with no eq() asked (JitDeCtx::put_member).
inline JitOwnedVal jit_deserialize(const sendable::SendNode& n, JitDeCtx& ctx) {
  using K = sendable::SendNode::K;
  if (n.is_backref) return ctx.recall(n.ref_id);
  auto fill = [&](auto* c, int8_t tag, auto add) {
    JitOwnedVal out(tag, reinterpret_cast<int64_t>(c));
    for (const auto& e : n.elems) add(c, jit_deserialize(e, ctx).consume());
    return out;
  };
  auto push = [](JitArray* a, JitValue v) {
    culebra_runtime_array_push(a, v.tag, v.data);
  };
  switch (n.kind) {
    case K::Nil:   return JitOwnedVal(TAG_NIL, 0);
    case K::Unbound: return JitOwnedVal(TAG_NO_SELF, n.i);
    case K::Bool:  return JitOwnedVal(TAG_BOOL, n.b ? 1 : 0);
    case K::Long:  return JitOwnedVal(TAG_LONG, n.i);
    case K::Float: return JitOwnedVal(jit_float(n.d));
    case K::Str:
      return JitOwnedVal(TAG_STRING,
                         reinterpret_cast<int64_t>(_culebra_heap_str(n.s)));
    case K::Array: return fill(culebra_runtime_array_new(), TAG_ARRAY, push);
    case K::Tuple: return fill(culebra_runtime_tuple_new(), TAG_TUPLE, push);
    case K::Set: {
      JitOwnedVal out(TAG_SET,
                      reinterpret_cast<int64_t>(culebra_runtime_set_new()));
      auto* s = reinterpret_cast<JitSet*>(out.borrow().data);
      for (const auto& e : n.elems)
        JitDeCtx::put_member(s, jit_deserialize(e, ctx).consume(), e.hash);
      return out;
    }
    case K::Object: {
      auto* o = culebra_runtime_object_new();
      JitOwnedVal out(TAG_OBJECT, reinterpret_cast<int64_t>(o));
      if (n.ref_id >= 0) ctx.remember(n.ref_id, out.borrow());
      // What it is, ahead of what it holds (SendNode::elems). A class is one
      // from the start, since its meta's methods may reach an instance that
      // links it; the mark then takes the meta's +1. An instance links its
      // meta and class once its entries are in.
      JitOwnedVal meta(TAG_NIL, 0), cls(TAG_NIL, 0);
      if (n.is_class) {
        o->is_class = true;
        JitValue m = jit_deserialize(n.elems[0], ctx).consume();
        culebra_runtime_mark_class(o, reinterpret_cast<JitObject*>(m.data));
      } else if (n.is_enum) {
        // An enum even when no variant it declared is left in its slots;
        // each that is names it, as the declaration did. A variant's meta
        // holds no entries, so each is sealed when its rebuild returns.
        _jit_enum_ref_of(o);
        for (const auto& e : n.elems) {
          JitOwnedVal m = jit_deserialize(e, ctx);
          _jit_enum_declare(o, reinterpret_cast<JitObject*>(m.borrow().data));
        }
      } else if (!n.elems.empty()) {
        meta = jit_deserialize(n.elems[0], ctx);
        if (n.elems.size() > 1) cls = jit_deserialize(n.elems[1], ctx);
      }
      size_t nth = 0;
      for (size_t k = 0; k < n.entries.size(); k++) {
        JitOwnedVal val = jit_deserialize(n.entries[k].second, ctx);
        bool mut = k < n.entry_mut.size() ? n.entry_mut[k] : true;
        if (n.entries[k].first.kind == sendable::SendNode::K::Str) {
          _jit_rebuilt_put(o, n.facts, nth, n.entries[k].first.s, val.consume(),
                           mut);
        } else {
          // Non-string key: rebuilt after its value, as it was sent, and
          // put as the runtime stores a key already hashed, never through
          // a program's write.
          const auto& key = n.entries[k].first;
          JitValue kv = jit_deserialize(key, ctx).consume();
          JitDeCtx::put_entry(o, kv, key.hash, val.consume(), mut);
        }
      }
      // The link takes a reference of its own on each.
      _jit_rebuilt_object_done(
          o, n.s, reinterpret_cast<JitObject*>(meta.borrow().data),
          reinterpret_cast<JitObject*>(cls.borrow().data), n.facts);
      return out;
    }
    case K::Closure: {
      // Multifn dispatcher: rebuild the thunk closure, with a table of its
      // own, and register the overload methods there (recursion resolves
      // because the dispatcher is memoized before its method bodies are
      // rebuilt).
      if (!n.mf_name.empty()) {
        auto* c = _jit_multifn_new_dispatcher(n.mf_name,
                                              static_cast<size_t>(n.i));
        JitOwnedVal out(TAG_FUNC, reinterpret_cast<int64_t>(c));
        ctx.remember(n.ref_id, out.borrow());
        auto& methods = _jit_dispatcher_record(c)->methods;
        for (size_t i = 0; i < n.elems.size(); i++) {
          // The table holds the body's +1.
          auto* body_cls = reinterpret_cast<JitClosure*>(
              jit_deserialize(n.elems[i], ctx).consume().data);
          methods.push_back({n.mf_param_types[i],
                             n.mf_param_names[i],
                             body_cls,
                             n.mf_variadic[i],
                             n.mf_min_params[i]});
          // The rebuilt body's self-recursion uplink, exactly as
          // registration would have installed it.
          _jit_multifn_uplink_set(body_cls, c);
        }
        _jit_multifn_refresh_mono(c);
        return out;
      }
      auto* c = culebra_runtime_closure_new(
          n.jit_fn, n.elems.size(), static_cast<size_t>(n.i), n.jit_flags,
          reinterpret_cast<const JitParamMeta*>(n.jit_meta));
      JitOwnedVal out(TAG_FUNC, reinterpret_cast<int64_t>(c));
      ctx.remember(n.ref_id, out.borrow());
      for (size_t i = 0; i < n.elems.size(); i++) {
        JitValue capv = jit_deserialize(n.elems[i], ctx).consume();
        c->captures[i] = culebra_runtime_cell_new(capv.tag, capv.data);
      }
      return out;
    }
    case K::Channel: {
      auto* core = chan_core(n.i);
      int role = n.b ? 1 : 0;
      if (core) chan_bump(core, role);  // this rebuilt endpoint's own count
      return JitOwnedVal(_jit_make_channel_endpoint(core, role));
    }
    case K::SharedBuffer:  // the rebuilt handle's own ref
      return JitOwnedVal(_jit_make_shared_buffer_handle(
          std::static_pointer_cast<culebra::SharedBufferCore>(n.held)));
    case K::BufView: {
      JitOwnedVal buf = jit_deserialize(n.elems[0], ctx);
      auto* b = reinterpret_cast<JitObject*>(buf.borrow().data);
      // The field, in the layout the rebuilt buffer keeps: any of that type
      // lays out the same. A dropped buffer keeps none, and its view reads as
      // dropped whatever it is.
      return JitOwnedVal(_jit_make_buf_view(new JitBufView(
          b, n.i, _jit_field_of_type(_jit_shared_buf(b)->layout, n.s))));
    }
    case K::SharedVal: {
      // The receiver takes its own count; the node's goes with the node. A
      // tree that is gone makes a view that reads as a dropped one.
      return JitOwnedVal(_jit_shared_val_root(culebra::shared_val_acquire(n.i),
                                              n.i, n.ref_id));
    }
  }
  return JitOwnedVal(TAG_NIL, 0);
}

// A message rebuilt by itself: its backrefs name only what it carries.
inline JitOwnedVal jit_deserialize(const sendable::SendNode& n) {
  JitDeCtx ctx;
  return jit_deserialize(n, ctx);
}

// Messages rebuilt each by itself into one Array; a rebuild that throws lets
// go of the Array and what it holds.
template <class Nodes>
inline JitOwnedVal jit_deserialize_each(const Nodes& nodes) {
  auto* out =
      culebra_runtime_array_new_reserved(static_cast<int64_t>(nodes.size()));
  JitOwnedVal held(TAG_ARRAY, reinterpret_cast<int64_t>(out));
  for (const auto& n : nodes) {
    JitValue v = jit_deserialize(n).consume();  // the array takes the +1
    culebra_runtime_array_push(out, v.tag, v.data);
  }
  return held;
}

// --- Trait defaults across Runtimes ------------------------------------------
// The default-method table (_jit_trait_default_impls) is per-Runtime, filled
// when a trait declaration executes. A worker Runtime executes only the
// closure it was sent, so its table would stay empty and `Eq.neq` — or a user
// trait's default — would miss there. Every worker site (Isolate.spawn,
// Channel.fan_in, Parallel.*, the Http/Net server pools) therefore ships the
// parent's table with the message: the parent snapshots it at spawn, and the
// worker rebuilds a default only when a lookup first misses it, so what a
// default captures is copied into a worker that calls it, not into every
// worker. A default body is a closure like the sent one; one that cannot be
// sent (a mut capture, a captured native, a dropped Shared value) is left
// out, and the worker misses that one default alone.
// Shipped entry i is still live: not installed yet, and its trait not
// declared again in this Runtime.
inline bool jit_shipped_pending(const _JitTraitDefaultTable& t, size_t i) {
  return t.shipped_open[i] && !t.shipped_closed.count((*t.shipped)[i].trait);
}

inline std::shared_ptr<const sendable::TraitDefaults>
jit_snapshot_trait_defaults() {
  sendable::TraitDefaults out;
  for (const auto& [trait_name, methods] : _jit_trait_default_impls()) {
    for (const auto& [method_name, cls] : methods) {
      if (!cls) continue;
      try {
        JitSerCtx sc;
        sendable::SendNode body = jit_serialize(
            JitValue{TAG_FUNC, reinterpret_cast<int64_t>(cls)}, sc);
        // A body over a channel or a shared handle would keep it open for as
        // long as the snapshot lives; a default is not the place to share
        // one, so it stays with its Runtime.
        if (!node_carries_inflight(body))
          out.push_back({trait_name, method_name, std::move(body)});
      } catch (const culebra::CulebraError&) {
      }
    }
  }
  // What this Runtime was itself shipped and never installed travels on.
  const auto& t = _jit_trait_default_table();
  for (size_t i = 0; t.shipped && i < t.shipped->size(); i++)
    if (jit_shipped_pending(t, i)) out.push_back((*t.shipped)[i]);
  return std::make_shared<const sendable::TraitDefaults>(std::move(out));
}

// The lookup (_jit_find_shipped_default): rebuild the shipped default `key`
// of a trait `obj` conforms to, and register it — unless `found` names the
// trait of an installed one. Every pending match is counted first, so a
// worker finds two traits ambiguous exactly where its parent does, whatever
// it has installed so far. The rebuilt closure arrives at +1 and the table
// takes it (culebra_runtime_register_trait_default's contract).
inline JitClosure* jit_install_shipped_default(JitObject* obj, const char* key,
                                               const std::string* found) {
  auto& t = _jit_trait_default_table();
  size_t pick = t.shipped->size();
  for (size_t i = 0; i < t.shipped->size(); i++) {
    const auto& d = (*t.shipped)[i];
    if (!jit_shipped_pending(t, i) || d.method != key ||
        !_jit_conforms_to_trait(TAG_OBJECT, reinterpret_cast<int64_t>(obj),
                                d.trait))
      continue;
    if (found) _jit_trait_default_ambiguous(*found, d.trait, key);
    if (pick < t.shipped->size())
      _jit_trait_default_ambiguous((*t.shipped)[pick].trait, d.trait, key);
    pick = i;
  }
  if (pick == t.shipped->size()) return nullptr;
  const auto& d = (*t.shipped)[pick];
  auto* cls =
      reinterpret_cast<JitClosure*>(jit_deserialize(d.body).consume().data);
  t.shipped_open[pick] = false;
  culebra_runtime_register_trait_default(d.trait.c_str(), d.method.c_str(),
                                         cls);
  return cls;
}

// Under the worker's RuntimeScope, before it runs anything: the defaults wait
// there, still serialized, until a lookup misses one.
inline void jit_install_trait_defaults(
    std::shared_ptr<const sendable::TraitDefaults> defaults) {
  [[maybe_unused]] static const bool hooked =
      (_jit_shipped_default_hook = &jit_install_shipped_default, true);
  auto& t = _jit_trait_default_table();
  t.shipped_open.assign(defaults->size(), true);
  t.shipped = std::move(defaults);
}

// --- Child entry point: run the spawned closure on a fresh JIT heap ----------

// `core` is borrowed: ~IsolateCore joins this thread before it goes, so the
// thread is never the last to let it go (and never joins itself).
inline void run_isolate_child_jit(IsolateCore& core,
                                  sendable::SendNode sclosure,
                                  std::vector<sendable::SendNode> sargs,
                                  std::shared_ptr<const sendable::TraitDefaults> defaults,
                                  bool decrement_live) {
  // A fresh Runtime gives this work its own JIT heap (and its own multifn
  // registry); the shared fn_ptr allocates on whichever Runtime is active.
  culebra::Runtime rt(&core.interrupt);
  culebra::RuntimeScope scope(rt);
  try {
    jit_install_trait_defaults(std::move(defaults));
    JitDeCtx dc;
    // The rebuilt closure is the child heap's only ref to whatever crossed the
    // boundary, so its captures drop when it does (the interp path drops them
    // via heap teardown). Held in JitOwnedVal because that has to happen on the
    // unwind path too: a body that throws past here with a captured `tx` would
    // otherwise leave the channel open forever and hang every receiver.
    JitOwnedVal fn = jit_deserialize(sclosure, dc);
    auto* cls = reinterpret_cast<JitClosure*>(fn.borrow().data);
    // The rebuilt arguments wait in a heap Array this frame reaches, not in a
    // std::vector: rebuilding the next one allocates, and a collection there
    // scans the machine stack and what it reaches, never a vector's buffer.
    // Held only in one, an argument was swept while the ones after it were
    // rebuilt, and the worker ran on whatever took its place.
    auto* argv = culebra_runtime_array_new_reserved(
        static_cast<int64_t>(sargs.size()));
    JitOwnedVal argv_guard(
        JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(argv)});
    for (const auto& sa : sargs) {
      JitValue a = jit_deserialize(sa, dc).consume();
      culebra_runtime_array_push(argv, a.tag, a.data);  // absorbs the +1
    }
    // Rebuilt endpoints hold their own refs now, so the message goes before
    // the body runs, and the rebuild's memo with it: a channel it carried
    // closes when the body's end does.
    sclosure = {};
    sargs.clear();
    dc = {};
    // Each arm consumes exactly what it passes: the invoke takes the args over,
    // and the callee frame releases the params on both its exit paths. So the
    // Array gives its elements up first, and is only their buffer from here.
    const size_t argc = argv->size;
    argv->size = 0;
    JitValue raw;
    if (argc == 0) raw = _culebra_invoke0(cls);
    else if (argc == 1) raw = _culebra_invoke1(cls, argv->items[0]);
    else if (argc == 2)
      raw = _culebra_invoke2(cls, argv->items[0], argv->items[1]);
    else
      raw = _jit_invoke(cls, {TAG_NO_SELF, 0}, static_cast<int64_t>(argc),
                        argv->items);
    JitOwnedVal r(raw);
    JitSerCtx sc;
    sendable::SendNode out = jit_serialize(r.borrow(), sc);
    {
      std::lock_guard<std::mutex> lk(core.m);
      core.result = std::move(out);
      core.finished = true;
    }
  } catch (const culebra::Interrupted& e) {
    culebra_runtime_release_passed_error();
    // A cancelled child: join() re-raises this as an Interrupted, not as a
    // program error. Explicit — the type no longer rides in `error`.
    std::lock_guard<std::mutex> lk(core.m);
    core.interrupted = e.what();
    core.finished = true;
  } catch (culebra::CulebraError& e) {
    culebra_runtime_release_passed_error();
    std::lock_guard<std::mutex> lk(core.m);
    core.error = e;
    core.finished = true;
  } catch (CulebraException& e) {
    // A raw user `throw <value>` (not a structured error): the value crosses,
    // and _jit_isolate_settle re-raises it as a CulebraException on the
    // parent. One that cannot cross fails the join with its SendError, as a
    // result that cannot does; the throw is let go either way.
    std::optional<sendable::SendNode> tn;
    std::optional<culebra::CulebraError> unsent;
    try {
      JitSerCtx sc;
      tn = jit_serialize({e.tag, e.data}, sc);
    } catch (culebra::CulebraError& se) {
      unsent = std::move(se);
    } catch (std::exception& se) {  // as the handler below reports one
      unsent = culebra::CulebraError("RuntimeError", se.what());
    }
    culebra_runtime_consume_throw(e);
    std::lock_guard<std::mutex> lk(core.m);
    if (tn) core.thrown = std::move(*tn);
    else core.error = std::move(*unsent);
    core.finished = true;
  } catch (std::exception& e) {
    std::lock_guard<std::mutex> lk(core.m);
    core.error = culebra::CulebraError("RuntimeError", e.what());
    core.finished = true;
  }
  if (decrement_live)
    g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
}

// A core whose thread runs the shipped closure on the shipped arguments.
// `counted`: it holds a g_live_isolates count, which the thread gives back.
inline std::shared_ptr<IsolateCore> _jit_start_isolate(
    sendable::SendNode sclosure, std::vector<sendable::SendNode> sargs,
    std::shared_ptr<const sendable::TraitDefaults> defaults, bool counted) {
  auto core = std::make_shared<IsolateCore>();
  core->thread = culebra::SizedThread(
      [c = core.get(), sclosure = std::move(sclosure), sargs = std::move(sargs),
       defaults = std::move(defaults), counted]() mutable {
        run_isolate_child_jit(*c, std::move(sclosure), std::move(sargs),
                              std::move(defaults), counted);
      });
  return core;
}

// --- JIT isolate handle (join/poll/drop): a handle Object whose JitIsolate
//     holds the IsolateCore. ---------------------------------------------------

// Joins the child and re-raises its failure; the result stays serialized.
inline void _jit_isolate_settle(const std::shared_ptr<IsolateCore>& core) {
  core->join();
  if (core->interrupted) throw culebra::Interrupted(*core->interrupted);
  if (core->error) throw *core->error;
  if (core->thrown) {
    // A raw user `throw <value>` from the child. Rebuild it on the parent heap
    // and re-raise in the JIT's throw form, so a compiled `catch` binds the
    // value (symmetric with the interp `throw chan_take(*core.thrown)`).
    JitValue tv = jit_deserialize(*core->thrown).consume();
    // 0:0 — the throw that produced this value ran on the isolate's thread,
    // so no position in this program's source applies to the re-raise.
    culebra_runtime_throw(tv.tag, tv.data, 0, 0);  // [[noreturn]]
  }
}

inline JitValue _jit_isolate_extract(const std::shared_ptr<IsolateCore>& core) {
  _jit_isolate_settle(core);
  return jit_deserialize(core->result).consume();  // on the parent heap
}

// The handle a method was called on (see _jit_self_handle).
inline JitIsolate* _jit_self_isolate(JitValue self) {
  return _jit_self_handle<JitIsolate>(self, JitHandle::Kind::Isolate,
                                      "handle from Isolate.spawn");
}

inline void _jit_isolate_join(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // Callee-consumes on any exit: _jit_isolate_extract re-raises the child's
  // failure, and the caller does not clean a native method's operands on
  // unwind, so a tail release would strand `self` there.
  JitMethodSelf _s{self};
  auto core = _jit_self_isolate(self)->core;  // a copy: outlives a drop() during the wait
  JitValue ret{TAG_NIL, 0};
  if (core) ret = _jit_isolate_extract(core);
  { *__ret = ret; return; }
}

inline void _jit_isolate_poll(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (extract re-raises)
  auto core = _jit_self_isolate(self)->core;
  JitValue ret{TAG_NIL, 0};
  if (core) {
    bool done;
    { std::lock_guard<std::mutex> lk(core->m); done = core->finished; }
    if (done) ret = _jit_isolate_extract(core);
  }
  { *__ret = ret; return; }
}

inline void _jit_isolate_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  // Idempotent, like a channel end's: a dropped handle holds no core.
  if (auto core = std::exchange(_jit_self_isolate(self)->core, nullptr)) {
    core->cancel();
    core->join();
  }
  { *__ret = {TAG_NIL, 0}; return; }
}

// The meta every Isolate.spawn handle of this Runtime points its `proto` at,
// pinned like the channel metas. Borrowed.
inline JitObject* _jit_isolate_meta() {
  return _jit_pinned_meta(culebra::kSlotIsolateMeta, 0, [] {
    auto* meta = culebra_runtime_object_new();
    _jit_view_method(meta, "join", _jit_isolate_join);
    _jit_view_method(meta, "poll", _jit_isolate_poll);
    _jit_view_method(meta, "drop", _jit_isolate_drop);
    return meta;
  });
}
inline JitValue _jit_make_isolate_handle(std::shared_ptr<IsolateCore> core) {
  JitValue v = _jit_make_handle(new JitIsolate(std::move(core)),
                                _jit_isolate_meta());
  reinterpret_cast<JitObject*>(v.data)->has_drop = true;
  return v;
}

// Runtime entry for Isolate.spawn(fn, *args). First slice: validates cross-
// thread execution of a shared fn_ptr. Always threaded (the parallelism-cap
// fallback lands with the full port).
inline JitValue culebra_jit_isolate_spawn(int8_t fn_tag, int64_t fn_data,
                                          int64_t n_args, JitValue* args,
                                          int64_t line, int64_t col) {
  if (fn_tag != TAG_FUNC) {
    throw culebra::CulebraError(
        "TypeError", "Isolate.spawn: first argument must be a function", line,
        col);
  }
  JitSerCtx sc;
  sendable::SendNode sclo;
  std::vector<sendable::SendNode> sargs;
  // A SendError names the operation and the spawn site.
  try {
    sclo = jit_serialize({fn_tag, fn_data}, sc);
    sargs.reserve(n_args);
    for (int64_t i = 0; i < n_args; i++)
      sargs.push_back(jit_serialize(args[i], sc));
  } catch (const culebra::CulebraError& e) {
    sendable::rethrow_send_error_in(e, "Isolate.spawn", line, col);
  }

  auto defaults = jit_snapshot_trait_defaults();
  // A channel-carrying closure (or arg) may block on a peer isolate, so it MUST
  // run concurrently — never the synchronous over-cap fallback below, which
  // joins immediately and would deadlock (e.g. a streaming producer fills a
  // bounded channel before any consumer starts). Mirrors the interp spawn and
  // fan_in(items, fn). CPU-bound closures still honor the cap.
  bool must_thread = node_carries_channel(sclo);
  for (const auto& a : sargs)
    if (!must_thread && node_carries_channel(a)) must_thread = true;
  bool under_cap =
      g_live_isolates().fetch_add(1, std::memory_order_relaxed) < isolate_cap();
  bool threaded = must_thread || under_cap;
  if (!threaded) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
  auto core = _jit_start_isolate(std::move(sclo), std::move(sargs),
                                 std::move(defaults), /*counted=*/threaded);
  if (threaded) {
    started_isolates_add(core);
  } else {
    // Synchronous fallback over the cap: still a fresh thread with a Runtime
    // of its own, but joined immediately, so concurrency stays bounded.
    core->join();
  }
  return _jit_make_isolate_handle(std::move(core));
}

// ===========================================================================
// JIT channel endpoints. The ChannelCore + its counts + auto-close are the
// backend-neutral machinery in isolate_core.h; here is the endpoint (a handle
// Object holding a JitChanEnd) and the JitValue serialize/deserialize
// boundary. Endpoint methods are captureless and reach the core through the
// end they are called on.
// ===========================================================================

// The end a method was called on (see _jit_self_handle).
inline JitChanEnd* _jit_self_chan_end(JitValue self) {
  return _jit_self_handle<JitChanEnd>(self, JitHandle::Kind::ChannelEnd,
                                      "channel endpoint");
}

inline void _jit_chan_send(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                               JitValue* args) {
  JitValue self{self_tag, self_data};
  // Callee-consumes: release self + the sent value on any exit. jit_serialize
  // throws for a non-Sendable value, and the caller does not clean a native
  // method's operands on unwind, so the guards must (else self + arg strand).
  JitMethodSelf _s{self};
  JitMethodArgs _a{n, args};
  auto* end = _jit_self_chan_end(self);
  if (n >= 1) {
    JitSerCtx sc;
    // The thunk ABI carries no line/col; backfill the serializer's
    // positionless errors (SendError, nesting ValueError) from the call
    // site the codegen published, like _jit_ns_method_dispatch does.
    _jit_at_call_site([&] {
      auto node = jit_serialize(args[0], sc);  // Sendable check here
      channel_send_node(end->core, std::move(node));
    });
  }
  { *__ret = {TAG_NIL, 0}; return; }
}

inline void _jit_chan_recv(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // Callee-consumes on any exit: chan_pop_blocking raises Interrupted on a
  // cancelled isolate, which a tail release would strand `self` on.
  JitMethodSelf _s{self};
  auto* end = _jit_self_chan_end(self);
  JitValue ret{TAG_NIL, 0};
  if (!end->core) { *__ret = ret; return; }
  auto node = chan_pop_blocking(*end->core);
  if (node) ret = jit_deserialize(*node).consume();
  { *__ret = ret; return; }
}

// The ChannelResult variant names, interned once: `Empty` is the common
// answer in a polling loop, so don't retake _intern_str's global lock per
// call. build_variant is the same entry point compiled `Enum.Variant(...)`
// calls use, so the instance is indistinguishable from a declared one.
inline JitValue _jit_channel_result(ChanTryPopStatus status, JitValue* payload) {
  static const char* kEnum = _intern_str("ChannelResult");
  static const char* kValue =
      _intern_str(chan_status_variant(ChanTryPopStatus::Value));
  static const char* kEmpty =
      _intern_str(chan_status_variant(ChanTryPopStatus::Empty));
  static const char* kClosed =
      _intern_str(chan_status_variant(ChanTryPopStatus::Closed));
  const char* name = kClosed;
  switch (status) {
    case ChanTryPopStatus::Value: name = kValue; break;
    case ChanTryPopStatus::Empty: name = kEmpty; break;
    case ChanTryPopStatus::Closed: break;
  }
  return _jit_build_native_variant(name, kEnum, payload);
}

inline void _jit_chan_try_recv(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                   JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* end = _jit_self_chan_end(self);
  auto r = end->core ? chan_pop_nonblocking(*end->core)
                     : ChanTryPopResult{ChanTryPopStatus::Closed, std::nullopt};
  JitValue ret;
  if (r.status == ChanTryPopStatus::Value) {
    JitValue v = jit_deserialize(*r.node).consume();  // build_variant takes it
    ret = _jit_channel_result(r.status, &v);
  } else {
    ret = _jit_channel_result(r.status, nullptr);
  }
  { *__ret = ret; return; }
}

inline void _jit_chan_drain(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                                JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  JitMethodArgs _a{n, args};
  auto* end = _jit_self_chan_end(self);
  JitValue m = n >= 1 ? args[0] : JitValue{TAG_NIL, 0};
  size_t max = _jit_at_call_site([&] {  // the thunk ABI carries no line/col
    return chan_drain_max(m.tag == TAG_NIL    ? DrainMaxKind::Nil
                          : m.tag == TAG_LONG ? DrainMaxKind::Count
                                              : DrainMaxKind::Other,
                          m.data, 0, 0);
  });
  auto taken = end->core ? chan_take_queued(*end->core, max)
                         : std::deque<sendable::SendNode>{};
  // Taken in one step, the batch is rebuilt as one Array.
  { *__ret = jit_deserialize_each(taken).consume(); return; }
}

// Idempotent: the end gives its count back once (explicit `drop()`, the
// release path's, the collector's).
inline void _jit_chan_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  _jit_self_chan_end(self)->release();
  { *__ret = {TAG_NIL, 0}; return; }
}

// Another end of the same channel, with a count of its own. A dropped end's
// clone is dropped too.
inline void _jit_chan_clone(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* end = _jit_self_chan_end(self);
  if (end->core) chan_bump(end->core, end->role);
  { *__ret = _jit_make_channel_endpoint(end->core, end->role); return; }
}

// `rx.iter()`: a cursor with a count of its own on the rx end, so the loop
// runs whatever becomes of the rx it came from, and the value has_next() took
// and next() has not yet handed out. A cursor that met the close has let its
// count go.
inline JitChanIter* _jit_self_chan_iter(JitValue self) {
  return _jit_self_handle<JitChanIter>(self, JitHandle::Kind::ChannelIter,
                                       "channel iterator");
}
inline void _jit_chan_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (blocking pull can raise)
  auto* it = _jit_self_chan_iter(self);
  it->pull();
  { *__ret = {TAG_BOOL, it->taken ? 1 : 0}; return; }
}
inline void _jit_chan_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                    JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = _jit_self_chan_iter(self);
  JitValue ret{TAG_NIL, 0};
  // Taken before it is rebuilt, as recv() takes it: a rebuild that throws
  // has still consumed the value.
  if (auto node = std::exchange(it->taken, std::nullopt))
    ret = jit_deserialize(*node).consume();
  { *__ret = ret; return; }
}
inline JitObject* _jit_channel_meta(int role);
inline void _jit_chan_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* core = _jit_self_chan_end(self)->core;
  if (core) chan_bump(core, /*role=*/1);
  { *__ret = _jit_make_handle(new JitChanIter(core), _jit_channel_meta(2)); return; }
}

// A merged rx (Channel.fan_in) reads its sources through chan_select_recv,
// the backend-neutral core; its cursor is an rx's, reading the merge.
inline JitMergedRx* _jit_self_merged_rx(JitValue self) {
  return _jit_self_handle<JitMergedRx>(self, JitHandle::Kind::MergedRx,
                                       "merged channel endpoint");
}
inline void _jit_merged_recv(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (blocking select can raise)
  const auto& merge = _jit_self_merged_rx(self)->merge;
  JitValue ret{TAG_NIL, 0};
  if (merge) {
    if (auto node = chan_select_recv(*merge))
      ret = jit_deserialize(*node).consume();
  }
  { *__ret = ret; return; }
}
// Idempotent, like an end's.
inline void _jit_merged_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  _jit_self_merged_rx(self)->merge.reset();
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_merged_join(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (join re-raises a failure)
  // Join the producers (fan_in(items, fn)) and surface the first error. Settle
  // through the JIT path so a structured error re-raises as CulebraError and a
  // raw `throw <value>` re-raises as a CulebraException — both caught by a
  // compiled `catch`.
  auto merge = _jit_self_merged_rx(self)->merge;  // held for the joins
  static const std::vector<std::shared_ptr<IsolateCore>> none;
  std::optional<culebra::CulebraError> first_err;
  bool have_thrown = false;
  bool interrupted = false;
  int8_t tt = 0;
  int64_t td = 0;
  for (const auto& core : merge ? merge->producers : none) {
    try {
      _jit_isolate_settle(core);  // the producer's return value is not join()'s
    } catch (const culebra::Interrupted&) {
      // A cancelled producer. Recorded like a failure, first-wins with the
      // two below, so the remaining producers are still joined here.
      if (!first_err && !have_thrown) interrupted = true;
    } catch (culebra::CulebraError& e) {
      if (!first_err && !have_thrown && !interrupted) first_err = e;
    } catch (CulebraException& e) {
      // We are the matching catch, so we owe the throw's balancing release
      // (a compiled `catch` would do it). Keep the first value's origin ref for
      // the re-raise below; drop any later ones fully.
      culebra_runtime_consume_throw(e);
      if (!first_err && !have_thrown && !interrupted) {
        have_thrown = true;
        tt = e.tag;
        td = e.data;
      } else {
        culebra_runtime_value_release(e.tag, e.data);
      }
    }
  }
  if (interrupted) throw culebra::Interrupted("isolate cancelled");
  if (have_thrown) culebra_runtime_throw(tt, td, 0, 0);  // see above: no position
  if (first_err) throw *first_err;
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_merged_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = new JitChanIter(_jit_self_merged_rx(self)->merge);
  { *__ret = _jit_make_handle(it, _jit_channel_meta(2)); return; }
}

// The meta every tx (role 0), every rx (1), every rx's cursor (2) or every
// merged rx (3) of this Runtime points its `proto` at: a plain Object holding
// its methods, pinned like the native metas. Borrowed: the table holds the
// +1, and each handle takes one of its own.
inline JitObject* _jit_channel_meta(int role) {
  return _jit_pinned_meta(culebra::kSlotChannelMetas, role, [&] {
    auto* meta = culebra_runtime_object_new();
    switch (role) {
      case 0:
        _jit_view_method(meta, "drop", _jit_chan_drop);
        _jit_view_method(meta, "clone", _jit_chan_clone);
        _jit_view_method(meta, "send", _jit_chan_send);
        break;
      case 1:
        _jit_view_method(meta, "drop", _jit_chan_drop);
        _jit_view_method(meta, "clone", _jit_chan_clone);
        _jit_view_method(meta, "recv", _jit_chan_recv);
        _jit_view_method(meta, "iter", _jit_chan_iter);
        _jit_view_method(meta, "try_recv", _jit_chan_try_recv);
        _jit_view_method(meta, "drain", _jit_chan_drain);
        break;
      case 2:
        _jit_view_method(meta, "iter", _jit_iter_self);
        _jit_view_method(meta, "has_next", _jit_chan_iter_has_next);
        _jit_view_method(meta, "next", _jit_chan_iter_next);
        break;
      case 3:
        _jit_view_method(meta, "recv", _jit_merged_recv);
        _jit_view_method(meta, "iter", _jit_merged_iter);
        _jit_view_method(meta, "drop", _jit_merged_drop);
        _jit_view_method(meta, "join", _jit_merged_join);
        break;
    }
    return meta;
  });
}

// An end of `core` (null: one already dropped), taking over a count the
// caller holds for it.
inline JitValue _jit_make_channel_endpoint(ChannelCore* core, int role) {
  JitValue v = _jit_make_handle(new JitChanEnd(core, role), _jit_channel_meta(role));
  reinterpret_cast<JitObject*>(v.data)->has_drop = true;
  return v;
}
inline JitValue _jit_make_merged_rx(std::shared_ptr<ChannelMerge> merge) {
  JitValue v = _jit_make_handle(new JitMergedRx(std::move(merge)),
                                _jit_channel_meta(3));
  reinterpret_cast<JitObject*>(v.data)->has_drop = true;
  return v;
}

// The buffer a method was called on. Its methods are only ever reached
// through a buffer, so anything else is a call no program can spell.
inline JitSharedBuf* _jit_self_shared_buf(JitValue self) {
  return _jit_self_handle<JitSharedBuf>(self, JitHandle::Kind::SharedBuffer,
                                        "SharedBuffer");
}

// Idempotent: the buffer lets go of its ref once (explicit `buf.drop()`, the
// release path's, the collector's).
inline void _jit_shared_buffer_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  _jit_self_shared_buf(self)->core.reset();
  { *__ret = {TAG_NIL, 0}; return; }
}

// `flush()` on a file-backed buffer (msync). Only on a file buffer's meta.
inline void _jit_shared_buffer_flush(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                         JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  culebra::shared_buffer_flush(_jit_self_shared_buf(self)->core);
  { *__ret = {TAG_NIL, 0}; return; }
}

// `with_lock(fn)`: run `fn()` holding the buffer's lock, return its value. The
// lock guard + JitMethodSelf + JitMethodArgs release the lock, `self`, and the
// callback on any exit. The guard holds a ref of its own, so a callback that
// drops the buffer leaves the lock where it was.
inline void _jit_shared_buffer_with_lock(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                             int64_t n_args, JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  // Callee-consumes the callback arg on any exit. _culebra_invoke0 only borrows
  // it, so without this the +1 the caller handed over strands (the callback's
  // result is a separate +1, returned before this guard runs).
  JitMethodArgs _a{n_args, args};
  if (n_args < 1 || args[0].tag != TAG_FUNC) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.with_lock expects a function");
  }
  auto core = _jit_self_shared_buf(self)->core;
  if (!core) {
    throw culebra::CulebraError("ValueError", "SharedBuffer has been dropped");
  }
  culebra::SharedBufferLockGuard guard(std::move(core));
  { *__ret = _culebra_invoke0(reinterpret_cast<JitClosure*>(args[0].data)); return; }
}


// ===========================================================================
// Shared.new views (concurrency C4). The frozen tree lives in
// culebra::shared_val_registry(); a view is a JitObject flagged
// `is_shared_val` whose two slots say which node it reads and through which
// hold (kSvNodeSlot, kSvHoldSlot) and whose `proto` is the one meta every
// view of a Runtime shares, where the reader methods are. Data reads are
// intercepted in the object get/index runtime helpers (rt/fixed.inc.h).
// ===========================================================================

// The reads of views under way on this thread, and the holds whose last view
// went while one was. A read counts nothing of its own: the view it reads
// through is counted on its hold, and this is what keeps the tree there when
// the program's own code, run from inside the read (a key's `hash`, an `==`,
// a `drop` body a collection fires), drops that view. Every read passes
// through here, so it is constant-initialized (see JitThreadState,
// rt/runtime.inc.h) and the waiting holds are kept apart.
struct JitSharedValReads {
  int depth = 0;
  int parked = 0;
};
inline constinit thread_local JitSharedValReads _jit_sv_reads;
inline auto& _jit_sv_parked() {
  thread_local std::vector<JitSvHold*> parked;
  return parked;
}

// A hold whose last view has gone gives its count on the tree back.
inline void _jit_sv_retire(JitSvHold* hold) {
  culebra::shared_val_release(*hold->core, hold->id);
  delete hold;
}
// One view fewer. The last one retires the hold: now, or once the reads
// under way on this thread are over, by when a view made meanwhile may be
// counted on it again.
inline void _jit_sv_unhold(JitSvHold* hold) {
  if (--hold->views > 0) return;
  auto& reads = _jit_sv_reads;
  if (reads.depth == 0) {
    _jit_sv_retire(hold);
    return;
  }
  if (hold->parked) return;
  hold->parked = true;
  _jit_sv_parked().push_back(hold);
  ++reads.parked;
}

// A view's node, for the length of one read (see JitSharedValReads).
struct JitResolvedNode {
  JitSvHold* hold;
  const culebra::FrozenTree* tree;
  const culebra::FrozenNode* node;
  size_t idx;  // which node of the tree
  JitSharedValReads& reads = _jit_sv_reads;  // one thread-local fetch a read

  JitResolvedNode(JitSvHold* h, size_t at)
      : hold(h), tree(&h->core->tree), node(&tree->nodes[at]), idx(at) {
    ++reads.depth;
  }

  culebra::FrozenKind kind() const { return node->kind; }
  // How many values the node yields (a Leaf, none), and the one at `at`: an
  // element, a member, an Object's value.
  int64_t count() const { return node->count; }
  culebra::FrozenCell cell(size_t at) const {
    return tree->cells[node->cells + at];
  }
  // A keyed node's shape (an Object's, a Set's), and its key at `at`.
  const culebra::FrozenShape& shape() const {
    return tree->shapes[node->shape];
  }
  culebra::FrozenCell key(size_t at) const {
    return tree->cells[shape().keys + at];
  }
  JitResolvedNode(const JitResolvedNode&) = delete;
  JitResolvedNode& operator=(const JitResolvedNode&) = delete;
  ~JitResolvedNode() {
    if (--reads.depth > 0 || reads.parked == 0) return;
    reads.parked = 0;
    // Retiring a hold runs none of the program's code, so nothing parks
    // another while these go.
    auto& parked = _jit_sv_parked();
    for (auto* waiting : parked) {
      waiting->parked = false;
      if (waiting->views == 0) _jit_sv_retire(waiting);
    }
    parked.clear();
  }
};

inline JitResolvedNode
_jit_shared_val_node_of(JitObject* view, int64_t line = 0, int64_t col = 0) {
  auto* hold = _jit_sv_hold(view);
  if (!hold) {
    // Position: the get/index interceptions pass the access position; the
    // handle methods leave 0 and fall back to the published call site
    // (the handle-method ClosedError convention).
    if (line == 0) {
      line = _jit_thread.call_line;
      col = _jit_thread.call_col;
    }
    throw culebra::CulebraError("ClosedError",
                                "Shared value has been dropped", line, col);
  }
  return JitResolvedNode(
      hold, static_cast<size_t>(view->slots[kSvNodeSlot].value.data));
}

// Whether a cell names a node of the tree (a container) and not a value.
inline bool _jit_sv_is_node(culebra::FrozenCell c) {
  return c.tag == TAG_ARRAY || c.tag == TAG_TUPLE || c.tag == TAG_SET ||
         c.tag == TAG_OBJECT;
}

// The value of a cell that names no node (+1): a scalar is the cell itself,
// a String is copied out to a heap string.
inline JitValue _jit_sv_leaf(const culebra::FrozenTree& t,
                             culebra::FrozenCell c) {
  if (c.tag == TAG_STRING)
    return {TAG_STRING,
            reinterpret_cast<int64_t>(_culebra_heap_str(t.str(c.data)))};
  return {c.tag, c.data};
}

// One cell, one JitValue (+1): a leaf's value (a String leak-bounded by
// design), or for a container a sub-view (one more view on the reader's hold,
// until that view is dropped).
inline JitValue _jit_shared_val_read(const JitResolvedNode& rn,
                                     culebra::FrozenCell c) {
  if (_jit_sv_is_node(c)) return _jit_make_shared_val_view(rn.hold, c.data);
  return _jit_sv_leaf(*rn.tree, c);
}

// Memoize a container child read on the PARENT view so the +0-borrowed
// contract the get/index interceptions return under is honored WITHOUT a leak.
// `_jit_shared_val_read` mints a FRESH +1 sub-view for a container cell;
// returning that through the borrowed-read channel orphaned the +1 (only the
// tracing backstop reclaimed it — the sharedval carve-out leak). Instead cache
// the sub-view on the parent view, keyed by the child's node, so the PARENT
// owns the +1 and every read of the same child returns the same borrowed
// handle. The parent's release cascades: its non_string_props sidecar (unused
// by a view otherwise — views are immutable, no user non-String keys)
// releases the cached sub-views on the object release path, each dropped as
// any view is. Primitives/strings have no sub-view, so they pass
// straight through. Views are per-isolate JitObjects (only the tree is
// shared), so this mutation needs no lock. "Leak-free" here means bounded retention for the
// parent view's lifetime (a distinct child read pins its sub-view until the
// view drops), not eager reclaim of a transiently-unreferenced sub-view — the
// necessary cost of honoring the +0-borrowed return contract. Retention is
// bounded by the frozen tree's own container-node count per live view.
inline JitValue _jit_shared_val_child(JitObject* view,
                                      const JitResolvedNode& rn,
                                      culebra::FrozenCell child) {
  if (!_jit_sv_is_node(child)) return _jit_shared_val_read(rn, child);
  auto key = JitHashedKey::of({TAG_LONG, child.data});
  if (view->non_string_props) {
    auto it = view->non_string_props->find(key);
    if (it != view->non_string_props->end())
      return it->second.value;  // cache hit: borrowed (+0), parent owns the +1
  }
  JitValue sub = _jit_shared_val_read(rn, child);  // fresh +1
  if (!view->non_string_props) {
    view->non_string_props = new JitObject::AnyKeyMap();
    view->key_order = new std::vector<JitHashedKey>();
  }
  // Transfer the mint's +1 into the cache (no extra retain). key_order carries
  // the key so the GC child enumeration (which walks key_order ∩
  // non_string_props) sees the cached sub-view as an owned edge; the object
  // release path frees the stored value, and the Long key is non-refcounted.
  (*view->non_string_props)[key] = JitObjectEntry{sub, false};
  view->key_order->push_back(key);
  return sub;  // borrowed (+0)
}

// --- a frozen tree's keys ---------------------------------------------------
// An Object's keys and a Set's members are found by hash when they are plain:
// a scalar, a String, a Tuple of those. The hash is the one the collection
// it froze kept the key under (_jit_try_hash), so a lookup asks with the hash
// any keyed lookup makes, and the relation is the collection's own
// (JitValueEq): never across types, a Tuple by its elements, NaN one key. A
// key of any other kind (an enum variant) is matched as the value it is.

inline size_t _jit_sv_str_hash(std::string_view s) {
  return std::hash<std::string_view>{}(s);
}

// The hash of key cell `c`; false for one that is not plain.
inline bool _jit_sv_cell_hash(const culebra::FrozenTree& t,
                              culebra::FrozenCell c, size_t* h) {
  if (c.tag == TAG_STRING) {
    *h = _jit_sv_str_hash(t.str(c.data));
    return true;
  }
  if (c.tag == TAG_TUPLE) {
    const auto& n = t.nodes[static_cast<size_t>(c.data)];
    size_t acc = n.count;
    for (uint32_t i = 0; i < n.count; i++) {
      size_t e;
      if (!_jit_sv_cell_hash(t, t.cells[n.cells + i], &e)) return false;
      acc = acc * 31 + e;
    }
    *h = acc;
    return true;
  }
  if (_jit_sv_is_node(c)) return false;
  // Any other cell is the value itself.
  JitValue unhashable;
  auto scalar = _jit_try_hash({c.tag, c.data}, &unhashable);
  if (scalar) *h = *scalar;
  return scalar.has_value();
}

// Whether plain key cell `c` is the value `v`.
inline bool _jit_sv_cell_is(const culebra::FrozenTree& t,
                            culebra::FrozenCell c, const JitValue& v) {
  if (v.tag == TAG_STRING || v.tag == TAG_STRINGVIEW)
    return c.tag == TAG_STRING &&
           t.str(c.data) == _culebra_str_view(v.tag, v.data);
  if (c.tag != v.tag) return false;
  switch (c.tag) {
    case TAG_NIL: return true;
    case TAG_BOOL: return (c.data != 0) == (v.data != 0);
    case TAG_LONG: return c.data == v.data;
    case TAG_FLOAT:
      return culebra::same_float_key(_culebra_float_to_double(c.data),
                                     _culebra_float_to_double(v.data));
    case TAG_TUPLE: {
      const auto& n = t.nodes[static_cast<size_t>(c.data)];
      auto* tuple = reinterpret_cast<JitArray*>(v.data);
      if (n.count != tuple->size) return false;
      for (uint32_t i = 0; i < n.count; i++)
        if (!_jit_sv_cell_is(t, t.cells[n.cells + i], tuple->items[i]))
          return false;
      return true;
    }
    default: return false;
  }
}

// Where a key hashing to `h` starts looking for its slot. A Long's hash is
// the Long, so the hash is stirred before the low bits are taken
// (keys a power of two apart would all start at one slot).
inline size_t _jit_sv_slot(size_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

// The position of the key of `shape` hashing to `h` that `is` accepts, or
// kNoKey. The slots are never all taken (JitSvFreezer::index_keys).
inline constexpr size_t kNoKey = static_cast<size_t>(-1);

template <class Is>
inline size_t _jit_sv_probe(const culebra::FrozenTree& t,
                            const culebra::FrozenShape& shape, size_t h,
                            Is&& is) {
  const auto* keys = t.keys_of(shape);
  for (size_t at = _jit_sv_slot(h) & shape.mask;; at = (at + 1) & shape.mask) {
    uint32_t slot = t.index[shape.slots + at];
    if (slot == 0) return kNoKey;
    if (is(keys[slot - 1])) return slot - 1;
  }
}

// --- freezing ---------------------------------------------------------------
// What Shared.new makes of a value: the tree the readers here read, written
// straight from the value. The Objects with one sequence of keys, none of
// them a node (and one class, and one mutability a key) get one shape between
// them, and an Object's key Strings are written once each.
struct JitSvFreezer {
  culebra::FrozenTree t;

  // A key String's place in `bytes`, found by its bytes: slots probed as a
  // shape's are, at most half of them taken.
  struct KeyStr {
    size_t hash = 0;
    int64_t at = -1;
  };
  std::vector<KeyStr> key_strs = std::vector<KeyStr>(16);
  size_t n_key_strs = 0;
  // The shapes another Object may turn out to have, by what makes them one.
  std::unordered_multimap<size_t, uint32_t> shapes;
  // The node each class meta is at.
  std::unordered_map<const void*, uint32_t> metas;
  // The containers being frozen, outermost first: what a cycle comes back to.
  std::vector<const void*> path;
  // The values of the container being frozen, above those of the containers
  // around it; an Object's keys the same way, and whether each takes a write.
  std::vector<culebra::FrozenCell> values;
  std::vector<culebra::FrozenCell> keys;
  std::vector<uint8_t> muts;
  JitValue root;  // the whole value: what a refusal is worded about

  explicit JitSvFreezer(JitValue v) : root(v) {}
  JitSvFreezer(const JitSvFreezer&) = delete;
  JitSvFreezer& operator=(const JitSvFreezer&) = delete;

  // Room for `n` more in an array the tree indexes in 32 bits.
  template <class T>
  uint32_t grow(std::vector<T>& v, size_t n) {
    size_t at = v.size();
    if (at + n >= culebra::kFrozenNone)
      sendable::send_error("the value is too large to share");
    v.resize(at + n);
    return static_cast<uint32_t>(at);
  }

  // A container: its node is numbered as it is entered, so the root is node
  // 0, and its cells are laid down as it is left, once its values are known.
  uint32_t open(culebra::FrozenKind kind) {
    uint32_t at = grow(t.nodes, 1);
    t.nodes[at].kind = kind;
    return at;
  }
  void close(uint32_t at, size_t base) {
    size_t count = values.size() - base;
    uint32_t cells = grow(t.cells, count);
    std::copy(values.begin() + base, values.end(), t.cells.begin() + cells);
    values.resize(base);
    t.nodes[at].count = static_cast<uint32_t>(count);
    t.nodes[at].cells = cells;
  }

  int64_t key_str(std::string_view s) {
    size_t h = _jit_sv_str_hash(s), mask = key_strs.size() - 1;
    size_t i = _jit_sv_slot(h) & mask;
    for (; key_strs[i].at >= 0; i = (i + 1) & mask)
      if (key_strs[i].hash == h && t.str(key_strs[i].at) == s)
        return key_strs[i].at;
    int64_t at = t.add_str(s);
    key_strs[i] = {h, at};
    if (++n_key_strs * 2 > key_strs.size()) {
      std::vector<KeyStr> wider(key_strs.size() * 2);
      for (const auto& k : key_strs) {
        if (k.at < 0) continue;
        size_t j = _jit_sv_slot(k.hash) & (wider.size() - 1);
        while (wider[j].at >= 0) j = (j + 1) & (wider.size() - 1);
        wider[j] = k;
      }
      key_strs = std::move(wider);
    }
    return at;
  }

  // The hash slots of shape `s`'s keys, twice as many as the keys so a probe
  // always ends, and the keys that are not hashed.
  void index_keys(culebra::FrozenShape& s) {
    size_t slots = 1;
    while (slots < 2 * static_cast<size_t>(s.count)) slots <<= 1;
    s.slots = grow(t.index, slots);
    s.mask = static_cast<uint32_t>(slots - 1);
    std::vector<uint32_t> others;
    for (uint32_t at = 0; at < s.count; at++) {
      size_t h;
      if (!_jit_sv_cell_hash(t, t.cells[s.keys + at], &h)) {
        others.push_back(at);
        continue;
      }
      size_t slot = _jit_sv_slot(h) & s.mask;
      while (t.index[s.slots + slot] != 0) slot = (slot + 1) & s.mask;
      t.index[s.slots + slot] = at + 1;
    }
    s.others = grow(t.index, others.size());
    s.n_others = static_cast<uint32_t>(others.size());
    std::copy(others.begin(), others.end(), t.index.begin() + s.others);
  }

  static void mix(size_t& h, size_t x) {
    h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  }

  // The shape of the Object whose keys are the stack's from `base` up, which
  // it takes off. When no key is a node (`shared`) the shape may be another
  // Object's already; a key that is one (a Tuple, an enum variant) makes the
  // shape the Object's own.
  uint32_t shape(culebra::FrozenShape s, size_t base, bool shared) {
    uint32_t at = shape_of(s, base, shared);
    keys.resize(base);
    muts.resize(base);
    return at;
  }
  uint32_t shape_of(culebra::FrozenShape s, size_t base, bool shared) {
    size_t count = keys.size() - base;
    s.count = static_cast<uint32_t>(count);
    size_t h = count;
    if (shared) {
      mix(h, s.meta);
      mix(h, static_cast<size_t>(s.name));
      mix(h, s.facts.bits | (static_cast<size_t>(s.facts.meta_flags) << 16) |
                 (static_cast<size_t>(static_cast<uint8_t>(s.facts.dir_kind))
                  << 8));
      mix(h, reinterpret_cast<size_t>(s.facts.decl));
      for (size_t i = base; i < keys.size(); i++) {
        mix(h, static_cast<size_t>(keys[i].tag));
        mix(h, static_cast<size_t>(keys[i].data));
        mix(h, muts[i]);
      }
      for (auto [it, end] = shapes.equal_range(h); it != end; ++it)
        if (same(t.shapes[it->second], s, base)) return it->second;
    }
    s.keys = grow(t.cells, count);
    std::copy(keys.begin() + base, keys.end(), t.cells.begin() + s.keys);
    s.mut = grow(t.index, (count + 31) / 32);
    for (size_t i = 0; i < count; i++)
      if (muts[base + i]) t.index[s.mut + i / 32] |= 1u << (i % 32);
    index_keys(s);
    uint32_t at = grow(t.shapes, 1);
    t.shapes[at] = s;
    if (shared) shapes.emplace(h, at);
    return at;
  }

  bool same(const culebra::FrozenShape& a, const culebra::FrozenShape& s,
            size_t base) const {
    if (a.count != s.count || a.meta != s.meta || a.name != s.name ||
        a.facts != s.facts)
      return false;
    const auto* theirs = t.keys_of(a);
    for (uint32_t i = 0; i < s.count; i++) {
      const auto& mine = keys[base + i];
      if (theirs[i].tag != mine.tag || theirs[i].data != mine.data ||
          t.mut_at(a, i) != (muts[base + i] != 0))
        return false;
    }
    return true;
  }

  // A Set's members are its keys: the shape is its own.
  void set_shape(uint32_t at) {
    culebra::FrozenShape s{};
    s.keys = t.nodes[at].cells;
    s.count = t.nodes[at].count;
    index_keys(s);
    uint32_t shape_at = grow(t.shapes, 1);
    t.shapes[shape_at] = s;
    t.nodes[at].shape = shape_at;
  }

  // Entering container `p`: a value that holds itself is the serializer's
  // SendError, and one nested too deep the walkers' ValueError.
  struct Enter {
    std::vector<const void*>& path;
    culebra::ValueWalkFrame walk;
    Enter(JitSvFreezer& f, const void* p) : path(f.entered(p)) {}
    ~Enter() { path.pop_back(); }
  };
  std::vector<const void*>& entered(const void* p) {
    if (std::find(path.begin(), path.end(), p) != path.end())
      sendable::send_error("a cyclic value cannot be sent");
    path.push_back(p);
    return path;
  }

  // Some part of the value is no data, so the freeze is refused, and with
  // what a send of the whole value would say when it cannot be sent at all
  // (the serializer's first objection, wherever in the value that is), or
  // else with Shared's own reason (a function, a handle).
  [[noreturn]] void refuse() {
    JitSerCtx ctx;
    sendable::SendNode n = jit_serialize(root, ctx);
    auto* why = culebra::_shared_val_reject_reason(n);
    sendable::send_error(why ? why : "value is not Sendable");
  }

  culebra::FrozenCell cell(JitValue v) {
    using F = culebra::FrozenKind;
    switch (v.tag) {
      case TAG_NIL: return {TAG_NIL, 0};
      case TAG_BOOL: return {TAG_BOOL, v.data != 0};
      case TAG_LONG:
      case TAG_FLOAT:
      case TAG_NO_SELF: return {v.tag, v.data};
      case TAG_STRING:
      case TAG_STRINGVIEW:
        return {TAG_STRING, t.add_str(_culebra_str_view(v.tag, v.data))};
      case TAG_ARRAY:
      case TAG_TUPLE: {
        auto* a = reinterpret_cast<JitArray*>(v.data);
        Enter in(*this, a);
        uint32_t at = open(v.tag == TAG_ARRAY ? F::Array : F::Tuple);
        size_t base = values.size();
        for (size_t i = 0; i < a->size; i++) {
          auto c = cell(a->items[i]);
          values.push_back(c);
        }
        close(at, base);
        return {v.tag, at};
      }
      case TAG_SET: {
        auto* s = reinterpret_cast<JitSet*>(v.data);
        Enter in(*this, s);
        uint32_t at = open(F::Set);
        size_t base = values.size();
        for (const auto& m : s->members) {
          auto c = cell(m.value());
          values.push_back(c);
        }
        close(at, base);
        set_shape(at);
        return {TAG_SET, at};
      }
      case TAG_OBJECT:
        return {TAG_OBJECT, object(reinterpret_cast<JitObject*>(v.data), false)};
      default: refuse();
    }
  }

  // An Object, a class instance or (`is_meta`) the class meta of one, which
  // is registered ahead of its entries: one may hold an instance of the class
  // it is. Any other Object is a handle of some kind, or a class, and no
  // data.
  uint32_t object(JitObject* o, bool is_meta) {
    if (_jit_handle_kind(o) != JitHandleKind::None || o->class_meta_of())
      refuse();
    Enter in(*this, o);
    uint32_t at = open(culebra::FrozenKind::Object);
    if (is_meta) metas.emplace(o, at);
    size_t base = values.size(), key_base = keys.size();
    bool shared = true;
    // Every entry, in the order the Object holds them, and the value before
    // its key.
    _jit_each_entry(
        o,
        [&](std::string_view name, const JitObjectEntry& entry) {
          auto value = cell(entry.value);
          values.push_back(value);
          keys.push_back({TAG_STRING, key_str(name)});
          muts.push_back(entry.mut);
        },
        [&](const JitHashedKey& k, const JitObjectEntry& entry) {
          auto value = cell(entry.value);
          values.push_back(value);
          auto key = cell(k.value());
          shared = shared && !_jit_sv_is_node(key);
          keys.push_back(key);
          muts.push_back(entry.mut);
        });
    culebra::FrozenShape s{};
    s.facts = _jit_object_facts(o);
    if (auto names = _jit_class_meta_names(o); !names.empty())
      s.name = key_str(names);
    if (auto* meta = o->proto()) {
      auto it = metas.find(meta);
      s.meta = it != metas.end() ? it->second : object(meta, true);
      // An instance that takes its class along is no data, as its class is
      // not.
      if (_jit_class_along(o)) refuse();
    }
    close(at, base);
    t.nodes[at].shape = shape(s, key_base, shared);
    return at;
  }

  // The tree of `v`. Node 0 is the root, a Leaf (one cell, yielding nothing)
  // when the value is no container.
  static culebra::FrozenTree freeze(JitValue v) {
    JitSvFreezer f(v);
    auto root = f.cell(v);
    if (!_jit_sv_is_node(root)) {
      uint32_t at = f.open(culebra::FrozenKind::Leaf);
      f.values.push_back(root);
      f.close(at, 0);
      f.t.nodes[at].count = 0;
    }
    f.t.cells.shrink_to_fit();
    f.t.nodes.shrink_to_fit();
    f.t.shapes.shrink_to_fit();
    f.t.index.shrink_to_fit();
    f.t.bytes.shrink_to_fit();
    return std::move(f.t);
  }
};

// --- thawing ----------------------------------------------------------------
// A part of a frozen tree as an ordinary value in the local heap (+1): what
// copy(), keys(), an iteration's keys and a comparison by `==` read. A class
// instance names its class by the node its meta is at, wherever in the tree
// that is, so a part thaws whole on its own.
struct JitSvThaw {
  const culebra::FrozenTree& t;
  // The class metas rebuilt so far, by node. Each is held until the thaw is
  // over; an instance takes a count of its own.
  std::unordered_map<uint32_t, JitObject*> metas;

  explicit JitSvThaw(const culebra::FrozenTree& tree) : t(tree) {}
  JitSvThaw(const JitSvThaw&) = delete;
  JitSvThaw& operator=(const JitSvThaw&) = delete;
  ~JitSvThaw() {
    for (auto& [at, meta] : metas)
      _culebra_value_release_impl(TAG_OBJECT,
                                  reinterpret_cast<int64_t>(meta));
  }

  JitValue cell(culebra::FrozenCell c) {
    if (_jit_sv_is_node(c)) return node(static_cast<uint32_t>(c.data));
    return _jit_sv_leaf(t, c);
  }

  JitValue node(uint32_t at) {
    using K = culebra::FrozenKind;
    const auto& n = t.nodes[at];
    const auto* cells = t.cells_of(n);
    switch (n.kind) {
      case K::Leaf: return cell(cells[0]);
      case K::Array:
      case K::Tuple: {
        auto* a = n.kind == K::Array
                      ? culebra_runtime_array_new_reserved(n.count)
                      : culebra_runtime_tuple_new();
        for (uint32_t i = 0; i < n.count; i++) {
          JitValue ev = cell(cells[i]);
          culebra_runtime_array_push(a, ev.tag, ev.data);
        }
        return {n.kind == K::Array ? TAG_ARRAY : TAG_TUPLE,
                reinterpret_cast<int64_t>(a)};
      }
      case K::Set: {
        auto* s = _set_new_reserved(n.count);
        for (uint32_t i = 0; i < n.count; i++) {
          JitValue ev = cell(cells[i]);
          culebra_runtime_set_add(s, ev.tag, ev.data);
        }
        return {TAG_SET, reinterpret_cast<int64_t>(s)};
      }
      case K::Object:
        return {TAG_OBJECT, reinterpret_cast<int64_t>(object(at, false))};
    }
    return {TAG_NIL, 0};
  }

  // Object node `at`, rebuilt as jit_deserialize rebuilds one that crossed in
  // a message. A class meta (`is_meta`) is registered ahead of its entries,
  // which may hold an instance of the class it is.
  JitObject* object(uint32_t at, bool is_meta) {
    const auto& n = t.nodes[at];
    const auto& shape = t.shapes[n.shape];
    auto* o = culebra_runtime_object_new();
    if (is_meta) metas.emplace(at, o);
    const auto* keys = t.keys_of(shape);
    const auto* values = t.cells_of(n);
    size_t nth = 0;
    for (uint32_t i = 0; i < n.count; i++) {
      JitValue val = cell(values[i]);
      bool mut = t.mut_at(shape, i);
      if (keys[i].tag == TAG_STRING) {
        _jit_rebuilt_put(o, shape.facts, nth, t.str(keys[i].data), val, mut);
      } else {
        JitValue key = cell(keys[i]);
        culebra_runtime_object_set_any(o, key.tag, key.data, mut, val.tag,
                                       val.data, 0, 0, /*is_init=*/true);
      }
    }
    JitObject* proto = nullptr;
    if (shape.meta != culebra::kFrozenNone) {
      auto it = metas.find(shape.meta);
      proto = it != metas.end() ? it->second : object(shape.meta, true);
    }
    // A Shared tree holds no class (the freeze refuses one), so no instance
    // in it links one.
    _jit_rebuilt_object_done(
        o, shape.name >= 0 ? t.str(shape.name) : std::string_view{}, proto,
        /*cls=*/nullptr, shape.facts);
    return o;
  }
};

inline JitValue _jit_sv_materialize(const culebra::FrozenTree& t,
                                    culebra::FrozenCell c) {
  return JitSvThaw(t).cell(c);
}
// The whole of what a view reads.
inline JitValue _jit_sv_materialize(const JitResolvedNode& rn) {
  return JitSvThaw(*rn.tree).node(static_cast<uint32_t>(rn.idx));
}

// Where a key is in keyed node `rn` (an Object's entry, a Set's member), or
// kNoKey: the lookup the collection it froze would have made. A plain key is
// found by its hash (a String by its bytes, with nothing built); the few keys
// of any other kind the node holds are compared as the values they are.
inline size_t _jit_sv_find_name(const JitResolvedNode& rn,
                                std::string_view name) {
  const auto& t = *rn.tree;
  return _jit_sv_probe(t, rn.shape(), _jit_sv_str_hash(name),
                       [&](culebra::FrozenCell k) {
                         return k.tag == TAG_STRING && t.str(k.data) == name;
                       });
}

inline size_t _jit_sv_find(const JitResolvedNode& rn, const JitValue& key) {
  const auto& t = *rn.tree;
  const auto& shape = rn.shape();
  // A key that cannot be hashed is the TypeError an Object's lookup is.
  JitValue unhashable{TAG_NIL, 0};
  auto hashed = JitHashedKey::try_of(key, &unhashable);
  if (!hashed) _jit_throw_unhashable(unhashable);
  size_t found =
      _jit_sv_probe(t, shape, hashed->hash(), [&](culebra::FrozenCell k) {
        return _jit_sv_cell_is(t, k, key);
      });
  if (found != kNoKey) return found;
  for (uint32_t i = 0; i < shape.n_others; i++) {
    size_t at = t.index[shape.others + i];
    JitOwnedVal stored(_jit_sv_materialize(t, t.keys_of(shape)[at]));
    if (JitValueEq{}(stored.borrow(), key)) return at;
  }
  return kNoKey;
}

// The index of the first element of Array or Tuple node `rn` that `==` finds
// equal to `x`, or -1: what the sequence it froze answers `contains` and
// `index_of` by. An element is compared as the value it is; a String against
// a String needs none made.
inline int64_t _jit_sv_position(const JitResolvedNode& rn, const JitValue& x) {
  bool strlike = x.tag == TAG_STRING || x.tag == TAG_STRINGVIEW;
  for (int64_t i = 0; i < rn.count(); i++) {
    auto c = rn.cell(static_cast<size_t>(i));
    if (strlike && c.tag == TAG_STRING) {
      if (_culebra_str_view(x.tag, x.data) == rn.tree->str(c.data)) return i;
      continue;
    }
    JitOwnedVal elem(_jit_sv_materialize(*rn.tree, c));
    if (_culebra_value_equal(static_cast<int8_t>(elem.v.tag), elem.v.data,
                             static_cast<int8_t>(x.tag), x.data))
      return i;
  }
  return -1;
}

// The tree's entry under a String key, for a read by name (`view.name`, an
// Object pattern): false on a miss, and for a view of anything but an Object.
// Reached through rt_runtime.inc.h's SharedValOps, which
// _jit_shared_val_meta installs.
inline bool _jit_shared_val_find_impl(JitObject* view, std::string_view name,
                                      JitValue* out, int64_t line,
                                      int64_t col) {
  auto rn = _jit_shared_val_node_of(view, line, col);
  if (rn.kind() != culebra::FrozenKind::Object) return false;
  size_t at = _jit_sv_find_name(rn, name);
  if (at == kNoKey) return false;
  *out = _jit_shared_val_child(view, rn, rn.cell(at));
  return true;
}

// `view[key]` — Object key lookup (KeyError on miss) or Array/Tuple
// positional read (IndexError). Key is BORROWED here (the caller
// consumes it per the object_get_any contract).
inline JitValue _jit_shared_val_index_impl(JitObject* view, int8_t key_tag,
                                           int64_t key_data, int64_t line,
                                           int64_t col) {
  auto rn = _jit_shared_val_node_of(view, line, col);
  using K = culebra::FrozenKind;
  switch (rn.kind()) {
    case K::Object: {
      if (size_t at = _jit_sv_find(rn, {key_tag, key_data}); at != kNoKey)
        return _jit_shared_val_child(view, rn, rn.cell(at));
      throw culebra::CulebraError("KeyError", "key not present", line, col);
    }
    case K::Array:
    case K::Tuple: {
      int64_t len = rn.count();
      if (key_tag != TAG_LONG) {
        // The interp's key.to_long(): no Float truncation, canonical
        // "expected Long, got <T>".
        culebra_runtime_type_error_typed(line, col, "Long", key_tag);
      }
      int64_t idx = key_data;
      if (idx < 0) idx += len;
      if (idx < 0 || idx >= len) {
        throw culebra::CulebraError("IndexError", "index out of range",
                                    line, col);
      }
      return _jit_shared_val_child(view, rn,
                                   rn.cell(static_cast<size_t>(idx)));
    }
    default:
      throw culebra::CulebraError("TypeError",
                                  "this Shared value is not indexable",
                                  line, col);
  }
}

// --- reader methods (handle ABI: self arrives +1, released here) ---
// Throw for an Object-only reader (has/keys/values) on a non-Object node.
inline void _jit_sv_require_object(const JitResolvedNode& rn,
                                   const char* m) {
  if (rn.kind() != culebra::FrozenKind::Object) {
    throw culebra::CulebraError("TypeError",
        std::string(m) + "() requires an Object Shared view");
  }
}

// The same for an Array-only one (index_of/map/filter).
inline void _jit_sv_require_array(const JitResolvedNode& rn,
                                  const char* m) {
  if (rn.kind() != culebra::FrozenKind::Array) {
    throw culebra::CulebraError(
        "TypeError", std::string(m) + "() requires an Array Shared view");
  }
}

// A reader's frame. `self` and the arguments are consumed on every exit; a
// reader takes the `takes` arguments the collection's own method of the name
// takes, and says so in its words when it is handed another count; and what
// it raises is reported at the call, since the thunk ABI carries no position.
// `at_most` is `takes` but for `index_of`: a call site that names a built-in
// passes that built-in's optional arguments filled in, and `index_of` has one
// (a String's starting offset), so its one argument arrives as two.
// `body(view, node)` returns the result at +1.
template <class Body>
inline void _jit_sv_reader(JitValue* ret, int8_t self_tag, int64_t self_data,
                           int64_t n_args, JitValue* args, const char* method,
                           int64_t takes, int64_t at_most, Body&& body) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  JitMethodArgs _a{n_args, args};
  *ret = _jit_at_call_site([&]() -> JitValue {
    // Ahead of the node: a call with the wrong arguments is that, whatever
    // the view it is made on.
    if (n_args < takes || n_args > at_most)
      throw culebra::CulebraError(
          "ArityError",
          culebra::builtin_arity_error_message(method, takes, takes, n_args));
    auto* view = reinterpret_cast<JitObject*>(self.data);
    return body(view, _jit_shared_val_node_of(view));
  });
}

inline void _jit_sv_size(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                             JitValue* args) {
  _jit_sv_reader(__ret, self_tag, self_data, n_args, args, "size", 0, 0,
                 [](JitObject*, const JitResolvedNode& rn) {
                   return JitValue{TAG_LONG, rn.count()};
                 });
}

inline void _jit_sv_has(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                            JitValue* args) {
  _jit_sv_reader(__ret, self_tag, self_data, n_args, args, "has", 1, 1,
                 [&](JitObject*, const JitResolvedNode& rn) {
                   _jit_sv_require_object(rn, "has");
                   bool found = _jit_sv_find(rn, args[0]) != kNoKey;
                   return JitValue{TAG_BOOL, found ? 1 : 0};
                 });
}

// `view.get(key, fallback)`: the Object's own `get`, or an Array's, which
// reads by position (from the end when negative) and never raises for one
// that is out of range.
inline void _jit_sv_get(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                            JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "get", 2, 2,
      [&](JitObject* view, const JitResolvedNode& rn) {
        using K = culebra::FrozenKind;
        size_t at = kNoKey;
        if (rn.kind() == K::Array) {
          if (args[0].tag != TAG_LONG)
            throw culebra::CulebraError(
                "TypeError", "type error: parameter 'i' expects Long");
          int64_t len = rn.count();
          int64_t i = args[0].data < 0 ? args[0].data + len : args[0].data;
          if (i >= 0 && i < len) at = static_cast<size_t>(i);
        } else if (rn.kind() == K::Object) {
          at = _jit_sv_find(rn, args[0]);
        } else {
          throw culebra::CulebraError(
              "TypeError", "get() requires an Object or Array Shared view");
        }
        // Either is borrowed: the fallback from the arguments, a child from
        // the view that holds it.
        JitValue v = at != kNoKey
                         ? _jit_shared_val_child(view, rn, rn.cell(at))
                         : args[1];
        culebra_runtime_value_retain(v.tag, v.data);
        return v;
      });
}

// `view.contains(x)`: a Set's member, found as a key is; an Array's or a
// Tuple's element, found by `==`.
inline void _jit_sv_contains(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                                 JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "contains", 1, 1,
      [&](JitObject*, const JitResolvedNode& rn) {
        using K = culebra::FrozenKind;
        bool found = false;
        switch (rn.kind()) {
          case K::Set: found = _jit_sv_find(rn, args[0]) != kNoKey; break;
          case K::Array:
          case K::Tuple: found = _jit_sv_position(rn, args[0]) >= 0; break;
          default:
            throw culebra::CulebraError(
                "TypeError",
                "contains() requires an Array, Tuple or Set Shared view");
        }
        return JitValue{TAG_BOOL, found ? 1 : 0};
      });
}

// `view.index_of(x)`: the Array's own `index_of` (a Tuple has none).
inline void _jit_sv_index_of(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                                 JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "index_of", 1, 2,
      [&](JitObject*, const JitResolvedNode& rn) {
        _jit_sv_require_array(rn, "index_of");
        return JitValue{TAG_LONG, _jit_sv_position(rn, args[0])};
      });
}

inline void _jit_sv_keys(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                             JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "keys", 0, 0,
      [](JitObject*, const JitResolvedNode& rn) {
        _jit_sv_require_object(rn, "keys");
        auto* arr = culebra_runtime_array_new();
        for (int64_t i = 0; i < rn.count(); i++) {
          JitValue kv = _jit_sv_materialize(
              *rn.tree, rn.key(static_cast<size_t>(i)));
          culebra_runtime_array_push(arr, kv.tag, kv.data);  // absorbs the +1
        }
        return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
      });
}

inline void _jit_sv_values(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                               JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "values", 0, 0,
      [](JitObject*, const JitResolvedNode& rn) {
        _jit_sv_require_object(rn, "values");
        auto* arr = culebra_runtime_array_new();
        for (int64_t i = 0; i < rn.count(); i++) {
          JitValue vv = _jit_shared_val_read(rn, rn.cell(static_cast<size_t>(i)));
          culebra_runtime_array_push(arr, vv.tag, vv.data);  // absorbs the +1
        }
        return JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(arr)};
      });
}

// The value a view froze, as an ordinary one: `copy()`, and what
// _jit_shared_val_plain (rt_runtime.inc.h) reaches through its hook.
inline JitValue _jit_shared_val_plain_impl(JitObject* view, int64_t line = 0,
                                           int64_t col = 0) {
  return _jit_sv_materialize(_jit_shared_val_node_of(view, line, col));
}

inline void _jit_sv_copy(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                             JitValue* args) {
  _jit_sv_reader(__ret, self_tag, self_data, n_args, args, "copy", 0, 0,
                 [](JitObject*, const JitResolvedNode& rn) {
                   return _jit_sv_materialize(rn);
                 });
}

// A view's drop: it stops being counted on its hold. Once: clearing the slot
// is what says so. The runtime's drop paths come here directly
// (_culebra_call_drop_if_present); the method is for `view.drop` as a value.
inline void _jit_shared_val_drop_impl(JitObject* view) {
  if (auto* hold = _jit_sv_hold(view)) {
    view->slots[kSvHoldSlot].value.data = 0;
    _jit_sv_unhold(hold);
  }
}
inline void _jit_sv_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  _jit_shared_val_drop_impl(reinterpret_cast<JitObject*>(self.data));
  { *__ret = {TAG_NIL, 0}; return; }
}

// `view.map(f)` and `view.filter(f)`: the Array's own, over the elements a
// walk of the view yields (a container element is a view). The callback is
// taken as the Array's takes it, so it is refused in the same words.
template <class Each>
inline JitValue _jit_sv_collect(const JitResolvedNode& rn, const char* method,
                                JitValue callback, Each&& each) {
  _jit_sv_require_array(rn, method);
  int64_t line = _jit_thread.call_line, col = _jit_thread.call_col;
  // A value that is no function is refused at the argument. An Array's own
  // call publishes that position; this one arrives as any method call does.
  auto arg = _jit_arg_pos(0);
  culebra_runtime_set_callback_arg_site(arg.line, arg.col);
  // The frame's arguments are its own to release; the callback's holder
  // consumes one too.
  culebra_runtime_value_retain(callback.tag, callback.data);
  JitHofCallback fn(static_cast<int8_t>(callback.tag), callback.data, 1,
                    method, "f", line, col);
  auto* out = culebra_runtime_array_new();
  JitOwnedVal out_guard(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(out)});
  for (int64_t i = 0; i < rn.count(); i++) {
    JitOwnedVal e(_jit_shared_val_read(rn, rn.cell(static_cast<size_t>(i))));
    culebra_runtime_value_retain(e.v.tag, e.v.data);  // the call consumes one
    JitOwnedVal r(_culebra_invoke1_at(fn, e.borrow(), line, col));
    each(out, e, r);
  }
  return out_guard.consume();
}

inline void _jit_sv_map(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                            JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "map", 1, 1,
      [&](JitObject*, const JitResolvedNode& rn) {
        return _jit_sv_collect(
            rn, "map", args[0],
            [](JitArray* out, JitOwnedVal&, JitOwnedVal& r) {
              auto v = r.consume();
              culebra_runtime_array_push(out, v.tag, v.data);
            });
      });
}

inline void _jit_sv_filter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                               JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "filter", 1, 1,
      [&](JitObject*, const JitResolvedNode& rn) {
        return _jit_sv_collect(
            rn, "filter", args[0],
            [](JitArray* out, JitOwnedVal& e, JitOwnedVal& r) {
              // An Array's filter keeps on a true Bool or a non-zero Long.
              if ((r.v.tag != TAG_BOOL && r.v.tag != TAG_LONG) || !r.v.data)
                return;
              auto v = e.consume();
              culebra_runtime_array_push(out, v.tag, v.data);
            });
      });
}

// --- walking a view -------------------------------------------------------
// `for ... in view` and `view.iter()` yield the same thing: an Object node's
// (key, value) pairs, any other node's elements, mirroring the local
// collection protocols. The tree is immutable, so a position is a snapshot.

// What position `at` of `rn` yields, at +1. False past the end.
inline bool _jit_sv_step(const JitResolvedNode& rn, int64_t at,
                         JitValue* out) {
  if (at >= rn.count()) return false;
  auto pos = static_cast<size_t>(at);
  if (rn.kind() != culebra::FrozenKind::Object) {
    *out = _jit_shared_val_read(rn, rn.cell(pos));
    return true;
  }
  JitValue kv = _jit_sv_materialize(*rn.tree, rn.key(pos));
  JitValue vv = _jit_shared_val_read(rn, rn.cell(pos));
  auto* pair = culebra_runtime_tuple_new();
  // Each push absorbs its +1, so the pair is the sole owner — releasing
  // here would free a container value out from under it.
  culebra_runtime_tuple_push(pair, kv.tag, kv.data);
  culebra_runtime_tuple_push(pair, vv.tag, vv.data);
  *out = {TAG_TUPLE, reinterpret_cast<int64_t>(pair)};
  return true;
}

// A walk holds the tree open on its own, so an explicit `view.drop()`
// part-way through doesn't pull the tree out from under it: it reads through
// a private view of the same node (+1), never handed to user code, which is
// dropped when the walk lets go. ClosedError for a dropped view.
inline JitObject* _jit_sv_walk_of(const JitResolvedNode& rn) {
  return reinterpret_cast<JitObject*>(
      _jit_make_shared_val_view(rn.hold, static_cast<int64_t>(rn.idx)).data);
}
inline JitObject* _jit_shared_val_walk_impl(JitObject* view, int64_t line,
                                    int64_t col) {
  return _jit_sv_walk_of(_jit_shared_val_node_of(view, line, col));
}
inline bool _jit_shared_val_step_impl(JitObject* walk, int64_t at, JitValue* out) {
  return _jit_sv_step(_jit_shared_val_node_of(walk), at, out);
}

// `view.iter()`: a built-in iterator like an Array's own (so it answers
// `iter` itself, and a lazy chain pulls it with no call per element). Its
// captures: [0] the walk's private view, [1] the position.
inline void _jit_sv_iter_fast_fn(JitClosure* cls, JitValue, bool* done,
                                 int8_t* out_tag, int64_t* out_data) {
  auto* at = cls->captures[1];
  JitValue v;
  *done = !_jit_shared_val_step_impl(
      reinterpret_cast<JitObject*>(cls->captures[0]->value.data),
      at->value.data, &v);
  if (*done) return;
  at->value.data++;
  *out_tag = static_cast<int8_t>(v.tag);
  *out_data = v.data;
}

inline void _jit_sv_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                             JitValue* args) {
  _jit_sv_reader(
      __ret, self_tag, self_data, n_args, args, "iter", 0, 0,
      [](JitObject*, const JitResolvedNode& rn) {
        auto* walk = _jit_sv_walk_of(rn);
        // Each cell takes over the +1 it is made with.
        auto* it = _iter_wrap_fast<&_jit_sv_iter_fast_fn>(
            {culebra_runtime_cell_new(TAG_OBJECT,
                                      reinterpret_cast<int64_t>(walk)),
             culebra_runtime_cell_new(TAG_LONG, 0)});
        return JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(it)};
      });
}

// The meta every view of this Runtime points `proto` at: a plain Object (not
// a class meta, so `type_of` and equality see a view as before) that holds
// the reader methods, pinned like the native metas. Borrowed: the table holds
// the +1, and each view takes one of its own.
inline JitObject* _jit_shared_val_meta() {
  return _jit_pinned_meta(culebra::kSlotSharedValMeta, 0, [] {
    auto* meta = culebra_runtime_object_new();
    _jit_view_method(meta, "size", _jit_sv_size);
    _jit_view_method(meta, "has", _jit_sv_has);
    _jit_view_method(meta, "get", _jit_sv_get);
    _jit_view_method(meta, "contains", _jit_sv_contains);
    _jit_view_method(meta, "index_of", _jit_sv_index_of);
    _jit_view_method(meta, "keys", _jit_sv_keys);
    _jit_view_method(meta, "values", _jit_sv_values);
    _jit_view_method(meta, "copy", _jit_sv_copy);
    _jit_view_method(meta, "iter", _jit_sv_iter);
    _jit_view_method(meta, "map", _jit_sv_map);
    _jit_view_method(meta, "filter", _jit_sv_filter);
    _jit_view_method(meta, "drop", _jit_sv_drop);
    // No view exists before the first meta does, so this is where the
    // generic paths learn how to read one (rt_runtime.inc.h).
    static constexpr SharedValOps ops{
        _jit_shared_val_find_impl, _jit_shared_val_index_impl,
        _jit_shared_val_plain_impl, _jit_shared_val_walk_impl,
        _jit_shared_val_step_impl,  _jit_shared_val_drop_impl};
    _jit_shared_val_ops_hook().store(&ops, std::memory_order_release);
    return meta;
  });
}

// A view of node `node` through `hold`, counted on it (a null hold makes a
// view that reads as a dropped one). Every view has the one shape, so it is
// stamped on rather than grown a slot at a time: a shape transition is a
// hashed lookup a slot, and an iteration over rows mints a view per row.
inline JitValue _jit_make_shared_val_view(JitSvHold* hold, int64_t node) {
  static culebra::Shape* const shape = [] {
    auto& shapes = culebra::shape_registry();
    auto* s = shapes.view_root();
    for (const char* name : {"__sharedval_node__", "__sharedval_hold__"})
      s = shapes.transition_add(s, name);
    return s;
  }();
  auto* meta = hold ? hold->meta : _jit_shared_val_meta();
  auto* h = _jit_view_new(&JitObject::is_shared_val);
  h->shape = shape;
  h->slots.reserve(2);
  h->slots.push_back({JitValue{TAG_LONG, node}, false});
  h->slots.push_back(
      {JitValue{TAG_LONG, reinterpret_cast<int64_t>(hold)}, false});
  if (hold) ++hold->views;
  meta->refcount++;
  h->set_proto(meta);
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// The view a Runtime is handed a tree through (Shared.new's, one a message
// carried): it takes over one count on `core`, as this Runtime's hold on it.
// A null core is a tree that is gone.
inline JitValue _jit_shared_val_root(const culebra::SharedValCore* core,
                                     int64_t id, int64_t node) {
  auto* meta = _jit_shared_val_meta();
  return _jit_make_shared_val_view(
      core ? new JitSvHold{core, id, meta} : nullptr, node);
}

// The meta every SharedBuffer of this Runtime points its `proto` at: one for
// a file-backed buffer, whose `flush` writes its pages out, one for the rest.
inline JitObject* _jit_shared_buffer_meta(bool file) {
  return _jit_pinned_meta(culebra::kSlotSharedBufferMetas, int{file}, [&] {
    auto* meta = culebra_runtime_object_new();
    _jit_view_method(meta, "drop", _jit_shared_buffer_drop);
    _jit_view_method(meta, "with_lock", _jit_shared_buffer_with_lock);
    if (file) _jit_view_method(meta, "flush", _jit_shared_buffer_flush);
    return meta;
  });
}

// A SharedBuffer over `core` (null: one already dropped), with a ref of its
// own on the bytes.
inline JitValue _jit_make_shared_buffer_handle(culebra::SharedBufferRef core) {
  bool file =
      core && core->storage == culebra::SharedBufferCore::Storage::File;
  JitValue v = _jit_make_handle(new JitSharedBuf(std::move(core)),
                                _jit_shared_buffer_meta(file));
  auto* h = reinterpret_cast<JitObject*>(v.data);
  h->is_shared_buffer = true;
  h->has_drop = true;
  return v;
}

// Channel.fan_in(a, fn = nil) -> merged rx. With one arg, merge the given
// receivers. With (items, fn), spawn one producer per item (fn(item, tx)) and
// merge their streams — JIT mirror of make_channel_namespace's fan_in.
inline JitValue culebra_jit_channel_fan_in(int64_t n, JitValue* args,
                                           int64_t line, int64_t col) {
  if (n < 1 || args[0].tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        "Channel.fan_in: first argument must be an Array", line, col);
  }
  auto* arr = reinterpret_cast<JitArray*>(args[0].data);

  if (n < 2 || args[1].tag != TAG_FUNC) {
    // fan_in([rx, ...]): merge existing receivers. Every one is checked
    // before any is counted, so a refusal leaves no count behind; a dropped
    // rx is a source already closed and drained.
    for (size_t i = 0; i < arr->size; i++) {
      auto* end = _jit_chan_end(arr->items[i]);
      if (!end || end->role != 1) {
        throw culebra::CulebraError("TypeError",
            "Channel.fan_in: every source must be a receiver (rx)", line, col);
      }
    }
    auto merge = std::make_shared<ChannelMerge>();
    merge->sources.reserve(arr->size);
    for (size_t i = 0; i < arr->size; i++) {
      auto* end = _jit_chan_end(arr->items[i]);
      if (!end->core) continue;
      chan_bump(end->core, /*role=*/1);
      merge->sources.push_back(end->core);
    }
    return _jit_make_merged_rx(std::move(merge));
  }

  // fan_in(items, fn): spawn one producer per item, merge. Producers run
  // threaded (a streaming producer blocks on send, so the inline-over-cap
  // fallback would deadlock). fn(item, tx) sends to its own channel; the
  // parent's tx is never created, the producer's auto-drops on exit.
  // The function is checked once up front; each producer is then sent the
  // function and its item serialized together (one context, so a value they
  // share arrives as one).
  auto with_fan_in_context = [&](auto&& send) {
    try {
      send();
    } catch (const culebra::CulebraError& e) {
      sendable::rethrow_send_error_in(e, "Channel.fan_in", line, col);
    }
  };
  with_fan_in_context([&] {
    JitSerCtx fsc;
    jit_serialize(args[1], fsc);
  });
  auto defaults = jit_snapshot_trait_defaults();  // shared by the producers
  // A refused item lets the merge go with the channels and producers made so
  // far, as a dropped merged rx would.
  auto merge = std::make_shared<ChannelMerge>();
  for (size_t i = 0; i < arr->size; i++) {
    JitSerCtx asc;
    sendable::SendNode sfn;
    std::vector<sendable::SendNode> sargs;
    with_fan_in_context([&] {
      sfn = jit_serialize(args[1], asc);
      sargs.push_back(jit_serialize(arr->items[i], asc));
    });
    // Its rx count is the merge's; its tx count is the message's, in flight
    // to the producer.
    auto* chcore = new ChannelCore(1);
    merge->sources.push_back(chcore);
    sargs.push_back(chan_node(chcore, /*role=*/0));
    g_live_isolates().fetch_add(1, std::memory_order_relaxed);
    auto pcore = _jit_start_isolate(std::move(sfn), std::move(sargs), defaults,
                                    /*counted=*/true);
    started_isolates_add(pcore);
    merge->producers.push_back(std::move(pcore));
  }
  return _jit_make_merged_rx(std::move(merge));
}

// ===========================================================================
// JIT Parallel.map / Parallel.each — reuses the backend-neutral ParallelState
// (SendNode-based) and fail-fast machinery from isolate.h; only the per-element
// work (deserialize → invoke fn_ptr → serialize) is JIT-specific.
// ===========================================================================

// A settled Result Object {ok, value, error} on the JIT heap; set_or_append
// takes ownership of `value`, so the caller hands its +1 over and releases the
// returned Object (which frees the fields). Mirrors interp _settled_ok/_err.
inline JitValue _jit_settled_result(bool ok, JitValue value, const char* err) {
  auto* o = culebra_runtime_object_new();
  o->set_or_append("ok", JitValue{TAG_BOOL, ok ? 1 : 0}, false);
  o->set_or_append("value", value, false);
  o->set_or_append("error",
                   ok ? JitValue{TAG_NIL, 0}
                      : JitValue{TAG_STRING,
                                 reinterpret_cast<int64_t>(_culebra_heap_str(err))},
                   false);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
}

// JIT counterparts of parallel_record_success / _element_error (the interp ones
// build Value; these build JitValue). The result is owned and released here.
inline void jit_parallel_record_success(ParallelState& st, size_t i,
                                        JitValue raw) {
  JitOwnedVal r(raw);  // released on every path, a throwing serialize included
  switch (st.mode) {
    case culebra::PMode::Map: {
      JitSerCtx sc;
      st.results[i] = jit_serialize(r.borrow(), sc);
      break;
    }
    case culebra::PMode::MapSettled: {
      JitOwnedVal o(_jit_settled_result(true, r.consume(), nullptr));  // owns r
      JitSerCtx sc;
      st.results[i] = jit_serialize(o.borrow(), sc);
      break;
    }
    case culebra::PMode::Each:
      break;
    case culebra::PMode::Race:
      if (!st.race_won.exchange(true)) {
        std::lock_guard<std::mutex> lk(st.err_m);
        JitSerCtx sc;
        st.race_result = jit_serialize(r.borrow(), sc);
        st.interrupt.store(true, std::memory_order_relaxed);
      }
      break;
  }
}

inline void jit_parallel_record_element_error(ParallelState& st, size_t i,
                                              culebra::CulebraError e) {
  switch (st.mode) {
    case culebra::PMode::MapSettled: {
      JitValue o = _jit_settled_result(false, JitValue{TAG_NIL, 0}, e.what());
      JitSerCtx sc;
      st.results[i] = jit_serialize(o, sc);
      culebra_runtime_value_release(o.tag, o.data);
      break;
    }
    case culebra::PMode::Race: {
      std::lock_guard<std::mutex> lk(st.err_m);
      if (!st.failed) { st.failed = true; st.err_index = i; st.err = std::move(e); }
      break;
    }
    default:
      parallel_record_error(st, i, std::move(e));
  }
}

inline void jit_parallel_worker(std::shared_ptr<ParallelState> st) {
  culebra::Runtime rt(&st->interrupt);
  culebra::RuntimeScope scope(rt);
  try {
    jit_install_trait_defaults(st->trait_defaults);
    // Owned for the same reason as the isolate child's closure: the captures
    // (a `tx` above all) must drop on every exit path, throw included.
    JitOwnedVal fn = jit_deserialize(st->fn);
    auto* cls = reinterpret_cast<JitClosure*>(fn.borrow().data);
    while (!st->interrupt.load(std::memory_order_relaxed)) {
      size_t i = st->next.fetch_add(1, std::memory_order_relaxed);
      if (i >= st->items.size()) break;
      try {
        JitOwnedVal item = jit_deserialize(st->items[i]);
        // A thrown value fails its element as an error does, by what it says,
        // and the next element starts with nothing of this one in the Runtime.
        JitValue r = culebra_runtime_call_from_host([&] {
          return _culebra_invoke1(cls, item.consume());  // invoke consumes
        });
        jit_parallel_record_success(*st, i, r);
      } catch (culebra::CulebraError& e) {
        jit_parallel_record_element_error(*st, i, e);
      } catch (std::exception& e) {
        jit_parallel_record_element_error(*st, i,
            culebra::CulebraError("RuntimeError", e.what()));
      }
      st->done.fetch_add(1, std::memory_order_relaxed);  // for on_progress
    }
  } catch (const culebra::Interrupted&) {
    // A fail-fast consequence, not the cause: stop the others, record nothing.
    // Explicit — an escape here would end on this worker thread.
    st->interrupt.store(true, std::memory_order_relaxed);
  } catch (culebra::CulebraError& e) {
    parallel_record_error(*st, 0, e);
  } catch (std::exception& e) {
    parallel_record_error(*st, 0,
        culebra::CulebraError("RuntimeError", e.what()));
  }
}

// Parent-thread coordinator: polls `done` and calls the JIT closure
// on_progress(done, total) on the parent heap. Mirrors the interp version; the
// JIT callback is a JitClosure invoked directly via _culebra_invoke2.
inline void jit_parallel_progress_coordinator(
    std::shared_ptr<ParallelState> st, JitClosure* cb,
    std::exception_ptr& cb_err) {
  const size_t total = st->items.size();
  size_t reported = 0;
  while (reported < total && !st->interrupt.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    size_t d = std::min(st->done.load(std::memory_order_relaxed), total);
    if (d == reported) continue;
    reported = d;
    try {
      JitValue rv = _culebra_invoke2(
          cb, JitValue{TAG_LONG, static_cast<int64_t>(reported)},
          JitValue{TAG_LONG, static_cast<int64_t>(total)});
      culebra_runtime_value_release(rv.tag, rv.data);
    } catch (...) {
      // interrupt: kept, not swallowed — jit_parallel_run re-raises this with
      // rethrow_exception once the workers are joined, an Interrupted from the
      // progress callback included.
      // thrown value: kept the same way, and thrown again as it is.
      cb_err = std::current_exception();
      st->interrupt.store(true, std::memory_order_relaxed);
      return;
    }
  }
}

inline JitValue jit_parallel_run(JitValue items_v, JitValue fn_v, int64_t limit,
                                 culebra::PMode mode, int64_t line, int64_t col,
                                 JitValue on_progress = JitValue{TAG_NIL, 0}) {
  const char* who = culebra::parallel_mode_name(mode);
  if (items_v.tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        culebra::format("Parallel.{}: first argument must be an Array", who),
        line, col);
  }
  if (fn_v.tag != TAG_FUNC) {
    throw culebra::CulebraError("TypeError",
        culebra::format("Parallel.{}: second argument must be a function", who),
        line, col);
  }
  auto* arr = reinterpret_cast<JitArray*>(items_v.data);
  auto st = std::make_shared<ParallelState>();
  st->mode = mode;
  { JitSerCtx sc; st->fn = jit_serialize(fn_v, sc); }
  st->trait_defaults = jit_snapshot_trait_defaults();
  st->items.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    JitSerCtx sc;
    st->items.push_back(jit_serialize(arr->items[i], sc));
  }
  if (arr->size == 0) {
    if (mode == culebra::PMode::Race) {
      throw culebra::CulebraError("ParallelError",
          "Parallel.race: empty array has no result", line, col);
    }
    return st->collects() ? JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                culebra_runtime_array_new())}
                          : JitValue{TAG_NIL, 0};
  }
  if (st->collects()) st->results.resize(arr->size);

  if (limit < 1) limit = isolate_cap();
  size_t target = std::min(static_cast<size_t>(limit),
                           static_cast<size_t>(arr->size));
  // Workers run on threads only, each with a Runtime of its own. Each thread
  // drains the shared queue; one thread still finishes the whole job.
  std::vector<culebra::SizedThread> threads;
  for (size_t w = 0; w < target; w++) {
    if (g_live_isolates().fetch_add(1, std::memory_order_relaxed) <
        isolate_cap()) {
      threads.emplace_back([st] {
        jit_parallel_worker(st);
        g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      });
    } else {
      g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      break;
    }
  }
  std::exception_ptr cb_err;
  if (threads.empty()) {
    // Cap exhausted: run the whole job on one thread (no live progress).
    culebra::SizedThread t([st] { jit_parallel_worker(st); });
    t.join();
  } else {
    if (on_progress.tag == TAG_FUNC) {
      jit_parallel_progress_coordinator(
          st, reinterpret_cast<JitClosure*>(on_progress.data), cb_err);
    }
    for (auto& t : threads) t.join();
  }

  // Every worker is joined, so the work's messages go now (a channel one of
  // them carried closes when the parent's end does); the results go with st.
  st->fn = {};
  st->items.clear();
  if (cb_err) std::rethrow_exception(cb_err);

  if (mode == culebra::PMode::Race) {
    if (st->race_won.load()) return jit_deserialize(st->race_result).consume();
    throw culebra::CulebraError("ParallelError",
        culebra::format("Parallel.race: all {} elements failed: {}",
                        static_cast<size_t>(arr->size),
                        st->err ? st->err->what() : "unknown"),
        line, col);
  }
  if (st->failed) {
    throw culebra::CulebraError("ParallelError",
        culebra::format("Parallel.{}: element[{}] failed: {}", who, st->err_index,
                        st->err->what()),
        line, col);
  }
  if (!st->collects()) return {TAG_NIL, 0};
  return jit_deserialize_each(st->results).consume();
}

// Channel.new(cap = 1) -> (tx, rx).
inline JitValue culebra_jit_channel_new(int64_t n, JitValue* args,
                                        int64_t line, int64_t col) {
  int64_t cap = (n >= 1 && args[0].tag == TAG_LONG) ? args[0].data : 1;
  if (cap < 0) {
    throw culebra::CulebraError("ValueError",
        "Channel.new: capacity must be >= 0", line, col);
  }
  auto* core = new ChannelCore(static_cast<size_t>(cap));  // the two ends'
  auto* tup = culebra_runtime_tuple_new();
  JitValue tx = _jit_make_channel_endpoint(core, 0);
  JitValue rx = _jit_make_channel_endpoint(core, 1);
  culebra_runtime_array_push(tup, tx.tag, tx.data);  // adopts the +1
  culebra_runtime_array_push(tup, rx.tag, rx.data);
  return {TAG_TUPLE, reinterpret_cast<int64_t>(tup)};
}

}  // namespace culebra
