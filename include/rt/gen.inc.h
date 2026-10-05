#pragma once

// A generator's frame, kept off the stack between resumes, and the iterator
// it is to the program (CULEBRA_GEN_FRAMES).
//
// Runtime-layer fragment of rt.h, split out for readability. These
// fragments rely on rt.h's #include block and are included by rt.h in a
// fixed sequence (see rt.h); they are not standalone headers.
//
// A generator fn compiled as written suspends at GenStart and at each Yield.
// What it holds while suspended lives here: its registers and owned marks,
// its pending defers (cut off the Runtime's one defer stack), the value
// has_next() pulled ahead. The engine that made the frame resumes it: the
// executor copies the registers to a window on the machine stack and runs
// its dispatch loop (vm.h), a lowered body works in them where they are
// (jit/lowering.h). Everything else about a frame is the same for both and
// is below: how the collector reads it, how it dies, the iterator protocol.

// What a lowered generator body reads and writes by offset. `state` is the
// one field the executor keeps too: which suspension point the frame is at.
struct JitGenRegs {
  JitValue* regs;       // the frame's registers, in place of allocas
  int64_t* marks;       // its owned marks
  int64_t* scratch;     // the for-in cursors' native fields
  int64_t state;        // which suspension point; 0 until GenStart
  int64_t yielded;      // the body left through a Yield
  int64_t closing;      // this resume closes the generator
  int64_t defer_delta;  // how far the defer stack moved since the yield
  int64_t depth;        // the recursion depth this resume entered at
};
using JitGenBody = void (*)(JitValue*, JitClosure*, int8_t, int64_t, int64_t,
                            JitValue*, JitGenRegs*);

struct JitGenFrame {
  JitGenRegs jit{};
  // Runs the body from where it is suspended to its next yield (true, the
  // value in `out`) or to its end (false). `closing`: the body leaves from
  // the suspension point as a `return` there would, and never yields.
  bool (*resume)(JitGenFrame&, JitValue& out, bool closing) = nullptr;
  JitGenBody body = nullptr;  // a lowered body's entry
  bool counts_frame = false;
  // The stack map: for each suspension point the slots that own a reference
  // there, each as `slot * 2 + is_cell`. Laid out as the number of points,
  // where each point's run starts (and where the last ends), the runs. The
  // compiler derives it from the unwind tables (vm.h chunk_gen_owned_table);
  // a lowered body carries the same table as a constant.
  const int32_t* owned = nullptr;
  // The object this hangs off (JitObject::gen_frame): the generator's
  // iterator, which is what the collector traces. Not owned.
  JitObject* self = nullptr;
  JitClosure* cls = nullptr;     // +1
  std::vector<JitValue> regs;    // owned while suspended
  std::vector<int64_t> marks;
  std::vector<int64_t> scratch;
  std::vector<JitValue> defers;  // pending, off the defer stack
  int64_t defer_base = 0;        // the height they sat on
  // Running: the body is on the machine stack (a lowered one works in
  // `regs` directly), and the frame object is on the heap's active list.
  // Dying: the frame object's count reached zero with the frame never
  // closed, and its references are being released.
  enum State { Suspended, Running, Done, Dying } state = Suspended;
  bool has_la = false;           // a value pulled by has_next()
  JitValue la{TAG_NIL, 0};
  // The executor's own: which chunk of which program, and where in it.
  const void* prog = nullptr;
  int32_t chunk = -1;
  size_t pc = 0;
};

// The stack map's run for the point `g` is suspended at.
inline std::span<const int32_t> _jit_gen_owned(const JitGenFrame& g) {
  int64_t k = g.jit.state;
  if (!g.owned || k < 1 || k > g.owned[0]) return {};
  return {g.owned + g.owned[k],
          static_cast<size_t>(g.owned[k + 1] - g.owned[k])};
}

// Every reference a suspended frame owns: its registers' (the stack map),
// its pending defers, the value has_next() pulled ahead, its closure.
template <class V, class C>
inline void _jit_gen_each_owned(const JitGenFrame& g, V&& value, C&& cell) {
  for (int32_t e : _jit_gen_owned(g)) {
    const JitValue& r = g.regs[static_cast<size_t>(e >> 1)];
    if (e & 1) {
      if (r.data) cell(reinterpret_cast<JitCell*>(r.data));
    } else {
      value(r);
    }
  }
  for (const auto& v : g.defers) value(v);
  if (g.has_la) value(g.la);
  value(JitValue{TAG_FUNC, reinterpret_cast<int64_t>(g.cls)});
}

// The collector's view of a frame. Suspended, it is the exact set above, so
// the refcount arithmetic may subtract each. Running or dying, the heap
// leaves it out of that arithmetic and only marks through it: every payload
// is a candidate, by the machine-stack scan's rule (a cell slot's JitCell
// and a cursor's closures ride a tag that does not say so).
inline void _jit_gen_frame_children(const JitGenFrame* frame,
                                    std::vector<void*>& out) {
  const JitGenFrame& g = *frame;
  switch (g.state) {
    case JitGenFrame::Suspended:
      _jit_gen_each_owned(
          g, [&](const JitValue& v) { _gc_push_value(out, v); },
          [&](JitCell* c) { out.push_back(c); });
      break;
    case JitGenFrame::Done:
      if (g.has_la) _gc_push_value(out, g.la);
      out.push_back(g.cls);
      break;
    case JitGenFrame::Running:
    case JitGenFrame::Dying:
      for (const auto& v : g.regs)
        if (v.data) out.push_back(reinterpret_cast<void*>(v.data));
      for (const auto& v : g.defers) _gc_push_value(out, v);
      if (g.has_la) _gc_push_value(out, g.la);
      out.push_back(g.cls);
      break;
  }
}

// The same exact set with its types, for the owned-scope trial deletion: a
// frame that is not suspended explains nothing.
inline void _jit_gen_frame_edges(const JitGenFrame* frame,
                                 std::vector<JitValue>& values,
                                 std::vector<JitCell*>& cells) {
  const JitGenFrame& g = *frame;
  if (g.state != JitGenFrame::Suspended) return;
  _jit_gen_each_owned(
      g, [&](const JitValue& v) { values.push_back(v); },
      [&](JitCell* c) { cells.push_back(c); });
}

// The frame object's count reached zero. A frame its iterator closed owns
// only its closure by now; one that was never closed (the iterator's drop
// was suppressed, as at program exit) lets go of what it holds without
// running its defers, as the lowered state object does.
inline void _jit_gen_frame_release(JitGenFrame* g) {
  if (g->state == JitGenFrame::Suspended) {
    std::vector<JitValue> values;
    std::vector<JitCell*> cells;
    _jit_gen_frame_edges(g, values, cells);
    g->state = JitGenFrame::Dying;
    for (const auto& v : values)
      _culebra_value_release_impl(static_cast<int8_t>(v.tag), v.data);
    for (auto* c : cells) culebra_runtime_cell_release(c);
  } else {
    if (g->has_la)
      _culebra_value_release_impl(static_cast<int8_t>(g->la.tag), g->la.data);
    _culebra_value_release_impl(TAG_FUNC, reinterpret_cast<int64_t>(g->cls));
  }
  delete g;
}

// Swept: its references are the collector's to reclaim, each by its own
// sweep entry.
inline void _jit_gen_frame_sweep(JitGenFrame* g) { delete g; }

// The body is about to run / has left: see Heap::begin_active. The count
// held across the run keeps the frame object for the body's own sake — a
// body can let go of the last reference to its iterator.
struct JitGenActive {
  JitGenFrame& g;
  culebra::gc::Heap& heap;
  explicit JitGenActive(JitGenFrame& g_) : g(g_), heap(_gc_heap()) {
    g.self->refcount++;
    heap.begin_active(g.self);
    g.state = JitGenFrame::Running;
  }
  void leave(JitGenFrame::State st) { g.state = st; }
  ~JitGenActive() {
    if (g.state == JitGenFrame::Running) g.state = JitGenFrame::Done;
    heap.end_active(g.self);
    _culebra_value_release_impl(TAG_OBJECT, reinterpret_cast<int64_t>(g.self));
  }
};

// The frame's pending defers go back on the defer stack; how far they sit
// from where they were cut off.
inline int64_t _jit_gen_defers_back(JitGenFrame& g) {
  auto& ds = _culebra_defer_stack();
  int64_t base = static_cast<int64_t>(ds.size());
  ds.insert(ds.end(), g.defers.begin(), g.defers.end());
  g.defers.clear();
  int64_t delta = base - g.defer_base;
  g.defer_base = base;
  return delta;
}
// And off it again, at a yield.
inline void _jit_gen_defers_away(JitGenFrame& g) {
  auto& ds = _culebra_defer_stack();
  g.defers.assign(ds.begin() + g.defer_base, ds.end());
  ds.resize(static_cast<size_t>(g.defer_base));
}

// A lowered body's resume: the defers go back and the body is told how far
// they moved; the registers never left the heap.
inline bool _jit_gen_resume_lowered(JitGenFrame& g, JitValue& out,
                                    bool closing) {
  // The depth first: a RecursionError here leaves the frame suspended and
  // whole, nothing of it having moved.
  g.jit.depth = g.counts_frame ? culebra_runtime_recursion_enter() : -1;
  g.jit.defer_delta = _jit_gen_defers_back(g);
  JitGenActive active(g);  // a throw out of the body leaves the frame Done
  g.jit.yielded = 0;
  g.jit.closing = closing ? 1 : 0;
  JitValue rv{TAG_NIL, 0};
  g.body(&rv, g.cls, TAG_NO_SELF, 0, 0, nullptr, &g.jit);
  if (!g.jit.yielded) {
    _culebra_value_release_impl(static_cast<int8_t>(rv.tag), rv.data);
    return false;
  }
  if (g.counts_frame) culebra_runtime_recursion_leave();
  _jit_gen_defers_away(g);
  active.leave(JitGenFrame::Suspended);
  out = rv;
  return true;
}

// Leave a suspended frame for good: its scopes' pending defers run and its
// bindings release, as a `return` at the suspension would have it.
inline void _jit_gen_close(JitGenFrame& g) {
  if (g.has_la) {
    g.has_la = false;
    _culebra_value_release_impl(static_cast<int8_t>(g.la.tag), g.la.data);
  }
  if (g.state != JitGenFrame::Suspended) return;
  JitValue none;
  g.resume(g, none, /*closing=*/true);
}

// Pull a value ahead, as has_next() must: true when one is in hand.
inline bool _jit_gen_fill(JitGenFrame* g) {
  if (g->has_la) return true;
  if (g->state == JitGenFrame::Done) return false;
  if (g->state == JitGenFrame::Running)
    throw culebra::CulebraError("ValueError", "generator already running");
  JitValue v;
  if (!g->resume(*g, v, /*closing=*/false)) return false;
  g->la = v;
  g->has_la = true;
  return true;
}

// The iterator's methods are the same natives for every generator, on the
// one meta: each reads its frame from the receiver. A method read off the
// object is bound to it on the way out (culebra_runtime_bind_method_value),
// so the receiver is a generator however the call was spelled.
inline JitGenFrame* _jit_gen_of(int8_t st, int64_t sd) {
  if (st == TAG_OBJECT) {
    if (auto* g = reinterpret_cast<JitObject*>(sd)->generator_frame()) return g;
  }
  throw culebra::CulebraError("TypeError",
                              "a generator's method was called on a value "
                              "that is not a generator");
}
inline void _jit_gen_has_next_fn(JitValue* ret, JitClosure*, int8_t st,
                                 int64_t sd, int64_t, JitValue*) {
  JitMethodSelf _s{JitValue{st, sd}};
  *ret = {TAG_BOOL, _jit_gen_fill(_jit_gen_of(st, sd)) ? 1 : 0};
}
inline void _jit_gen_next_fn(JitValue* ret, JitClosure*, int8_t st,
                             int64_t sd, int64_t, JitValue*) {
  JitMethodSelf _s{JitValue{st, sd}};
  JitGenFrame* g = _jit_gen_of(st, sd);
  if (!_jit_gen_fill(g)) {
    *ret = {TAG_NIL, 0};
    return;
  }
  g->has_la = false;
  *ret = g->la;
}
// `dispose`, and the object's `drop` — the last reference went, or the
// collector found the object dead: the frame closes. It goes with its
// object.
inline void _jit_gen_dispose_fn(JitValue* ret, JitClosure*, int8_t st,
                                int64_t sd, int64_t, JitValue*) {
  JitMethodSelf _s{JitValue{st, sd}};
  *ret = {TAG_NIL, 0};
  _jit_gen_close(*_jit_gen_of(st, sd));
}

// The meta every generator's iterator points `proto` at: the methods, and
// the name the value answers to.
inline JitObject* _jit_gen_meta() {
  static const char* const kName = _intern_str("Generator");
  return _jit_native_meta(kName, nullptr, [](JitObject* meta) {
    _jit_handle_bind_method(meta, "iter", &_iter_self_iter_fn, 0);
    _jit_handle_bind_method(meta, "has_next", &_jit_gen_has_next_fn, 0);
    _jit_handle_bind_method(meta, "next", &_jit_gen_next_fn, 0);
    _jit_handle_bind_method(meta, "dispose", &_jit_gen_dispose_fn, 0);
    _jit_handle_bind_method(meta, "drop", &_jit_gen_dispose_fn, 0);
    _jit_fill_specials(meta);
  });
}

// A frame and the heap object it hangs off (at +1, the caller's), which
// points at the meta. The object first, before the caller hands the frame
// anything: its allocation may collect. Each engine then fills in its own
// half.
inline JitGenFrame* _jit_gen_frame_new(
    JitClosure* cls, const int32_t* owned,
    bool (*resume)(JitGenFrame&, JitValue&, bool)) {
  auto* o = culebra_runtime_object_new();
  o->is_gen_frame = true;
  o->set_proto(_jit_gen_meta());  // transferred
  auto* g = new JitGenFrame{};
  g->resume = resume;
  g->owned = owned;
  g->self = o;
  g->cls = cls;
  culebra_runtime_value_retain(TAG_FUNC, reinterpret_cast<int64_t>(cls));
  o->gen_frame = g;
  return g;
}

// The frame's object goes out to the program as the generator's iterator,
// the caller's count on it with it: from here its last reference going
// closes the frame.
inline JitValue _jit_gen_iterator(JitGenFrame* g) {
  _jit_owned_bind_drop(g->self);
  return JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(g->self)};
}

// A lowered generator fn's entry (the JitFn its closure names calls this):
// the frame is on the heap from the first instruction, the body runs its
// prologue into it and leaves at GenStart.
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_gen_ramp(
    JitValue* ret, JitClosure* cls, int8_t st, int64_t sd, int64_t n,
    JitValue* args, void* body, int64_t n_slots, int64_t n_marks,
    int64_t n_scratch, int64_t counts_frame, const int32_t* owned) {
  JitGenFrame* g = _jit_gen_frame_new(cls, owned, &_jit_gen_resume_lowered);
  g->body = reinterpret_cast<JitGenBody>(body);
  g->counts_frame = counts_frame != 0;
  g->regs.assign(static_cast<size_t>(n_slots), JitValue{TAG_NIL, 0});
  g->marks.assign(static_cast<size_t>(n_marks), 0);
  g->scratch.assign(static_cast<size_t>(n_scratch), 0);
  g->jit.regs = g->regs.data();
  g->jit.marks = g->marks.data();
  g->jit.scratch = g->scratch.data();
  g->jit.depth = -1;
  // The frame object's one count is this function's until the caller has
  // it: a throw out of the prologue leaves none, and the frame (Done, its
  // registers released by the body's own pads) goes with its object.
  JitOwnedVal fo_guard(
      JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(g->self)});
  {
    // The prologue works in the frame's registers: marked through, never
    // counted, until it leaves at GenStart.
    JitGenActive active(*g);
    g->body(ret, cls, st, sd, n, args, &g->jit);
    g->defer_base = culebra_runtime_defer_mark();
    active.leave(JitGenFrame::Suspended);
  }
  fo_guard.consume();
  *ret = _jit_gen_iterator(g);
}

// A library frame's entering call site is whoever resumes it now, as it is
// for the lowered class's has_next().
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_gen_resnap_site(JitValue* slot) {
  if (int64_t site = culebra_runtime_param_pos(-1, 0, 0))
    *slot = JitValue{TAG_LONG, site};
}
