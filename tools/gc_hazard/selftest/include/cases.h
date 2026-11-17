// gc_hazard's self-test: each `yes_*` function must be reported under the
// rule its name says, and nothing else (SELFTEST in
// tools/checks/check_gc_hazard.sh).
#include <cstdint>
#include <functional>
#include <vector>

struct JitValue { int64_t tag; int64_t data; };
struct JitCell { int64_t refcount; JitValue value; };
struct JitClosure { int64_t refcount; void* fn_ptr; JitCell** captures; };
struct JitArray { int64_t refcount; JitValue* items; int64_t size; };
struct JitSlot { JitValue value; };
struct JitObject { int64_t refcount; std::vector<JitSlot> slots; };
using JitFn = void (*)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*);
using JitIterFastFn = void (*)(JitClosure*, bool*, int8_t*, int64_t*);

inline JitValue _jit_invoke(JitClosure* fn, JitValue, int64_t, JitValue*) {
  JitValue r;
  reinterpret_cast<JitFn>(fn->fn_ptr)(&r, fn, 0, 0, 0, nullptr);
  return r;
}
inline void plain() {}

// R1: the callee is borrowed.
inline void yes_r1(JitArray* a) {
  JitValue f = a->items[0];
  _jit_invoke(reinterpret_cast<JitClosure*>(f.data), {}, 0, nullptr);
}
// The caller holds a parameter.
inline void no_r1_param(JitValue f) {
  _jit_invoke(reinterpret_cast<JitClosure*>(f.data), {}, 0, nullptr);
}
// R1 through a lambda's capture.
template <class F> JitValue host(F f) { return f(); }
inline void yes_r1_lambda(JitArray* a) {
  JitValue m = a->items[1];
  host([&] { return _jit_invoke(reinterpret_cast<JitClosure*>(m.data), {}, 0, nullptr); });
}

// R2: kept in a std::function, returned or assigned to a field.
inline std::function<void()> yes_r2_return(JitArray* a) {
  JitValue cb = a->items[2];
  return [cb] { plain(); (void)cb; };
}
struct Part { std::function<bool(int)> source; };
inline void yes_r2_field(JitObject* o, Part& part) {
  JitValue sv = o->slots[0].value;  // a container's []
  auto* producer = reinterpret_cast<JitClosure*>(sv.data);
  part.source = [producer](int) { (void)producer; return true; };
}

// R3: live past a call that runs program code.
inline int64_t yes_r3(JitArray* a, JitClosure* g) {
  JitValue x = a->items[0];
  _jit_invoke(g, {}, 0, nullptr);
  return x.data;
}
inline int64_t no_r3_dead(JitArray* a, JitClosure* g) {
  JitValue x = a->items[0];
  int64_t d = x.data;
  _jit_invoke(g, {}, 0, nullptr);
  return d;
}
// Inside a lambda's body, and a handler's (a std::function's).
inline void yes_r3_in_lambda(JitArray* a, JitClosure* g) {
  auto body = [&] {
    JitValue x = a->items[0];
    _jit_invoke(g, {}, 0, nullptr);
    return x.data;
  };
  body();
}
inline void yes_r3_handler(std::function<void()>& run, JitArray* a) {
  JitValue x = a->items[0];
  run();
  (void)x.data;
}

// The running closure's own captures, in a closure body: not borrowed.
inline void no_running_capture(JitValue* ret, JitClosure* cls, int8_t, int64_t,
                               int64_t, JitValue*) {
  JitValue fv = cls->captures[0]->value;
  _jit_invoke(reinterpret_cast<JitClosure*>(fv.data), {}, 0, nullptr);
  *ret = fv;
}
inline void no_running_capture_fast(JitClosure* cls, bool*, int8_t*,
                                    int64_t* out) {
  JitValue fv = cls->captures[0]->value;
  _jit_invoke(reinterpret_cast<JitClosure*>(fv.data), {}, 0, nullptr);
  *out = fv.data;
}
// Any other closure's captures are: a program's closure rewrites them.
inline void yes_r1_other_capture(JitClosure* cls) {
  JitValue fv = cls->captures[0]->value;
  _jit_invoke(reinterpret_cast<JitClosure*>(fv.data), {}, 0, nullptr);
}

// Two variables of one name are two declarations.
inline int64_t yes_r3_two_v(JitArray* a, JitClosure* g) {
  int64_t s = 0;
  {
    JitValue v = a->items[0];
    _jit_invoke(g, {}, 0, nullptr);
    s += v.data;
  }
  {
    JitValue v = a->items[1];
    _jit_invoke(g, {}, 0, nullptr);
    s += v.data;
  }
  return s;
}
