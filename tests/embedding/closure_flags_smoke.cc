// A closure's kind is its own, not its code address'. The runtime used to
// answer "is this a getter?" and "is this native?" from side tables keyed by
// the compiled body's address, and an address names the code it was taken
// from only while that code stays mapped: a JIT arena is freed with its
// Runtime, and the next Runtime's compiler is handed the same pages.
//
// In the doctest lane one program's class getter and a later one's `handle`
// adapter landed on the same address, so the adapter was invoked 0-arg as a
// getter and a program with no getter in it failed with "ArityError: missing
// required argument '_eh_args'". Whether two bodies collide is a matter of
// codegen sizes, so an unrelated change to the JIT could turn it on.
//
// What this pins is the property that makes that unrepresentable: two
// closures over the SAME body disagree about what they are, and the bare
// property read honours each one's own answer. A test that waited for two
// arenas to actually overlap would pass by luck.

#include <iostream>

#include <culebra.h>
#include <vm_embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps this file's
// internals from colliding with the other smokes.
namespace closure_flags_smoke_ns {

namespace {

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

// One body, standing in for a compiled getter. Ignores its arguments and
// yields a value no bound-method wrapper could be mistaken for.
void body(JitValue* ret, JitClosure*, int8_t, int64_t, int64_t, JitValue*) {
  *ret = JitValue{TAG_LONG, 7};
}

}  // namespace

inline int run() {
  bool ok = true;

  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);

  auto* recv = culebra_runtime_object_new();
  JitValue self{TAG_OBJECT, reinterpret_cast<int64_t>(recv)};

  auto read = [&](uint64_t flags) {
    auto* m = culebra_runtime_closure_new(reinterpret_cast<void*>(&body),
                                          /*n_captures=*/0, /*arity=*/0, flags);
    JitValue view{TAG_FUNC, reinterpret_cast<int64_t>(m)};
    JitValue got = culebra_runtime_bind_method_value(
        static_cast<int8_t>(self.tag), self.data,
        static_cast<int8_t>(view.tag), view.data, "x");
    _culebra_value_release_impl(view.tag, view.data);
    return got;
  };

  JitValue as_getter = read(JIT_CLOSURE_GETTER);
  ok &= check(as_getter.tag == TAG_LONG && as_getter.data == 7,
              "a getter closure is invoked by a bare property read");

  JitValue as_method = read(0);
  ok &= check(as_method.tag == TAG_FUNC,
              "the same body, unflagged, binds as a method instead");

  _culebra_value_release_impl(as_getter.tag, as_getter.data);
  _culebra_value_release_impl(as_method.tag, as_method.data);
  _culebra_value_release_impl(self.tag, self.data);

  if (ok) std::cout << "closure_flags smoke OK\n";
  return ok ? 0 : 1;
}

}  // namespace closure_flags_smoke_ns
