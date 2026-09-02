// The JIT recognizes two kinds of compiled body by address: natives, which
// cannot be sent to another Runtime, and class getters, which a bare property
// read invokes 0-arg. Both keys are code addresses, and a code address means
// something only while that code is mapped — the JIT arena goes with its
// Runtime, and the next Runtime's compiler is handed the same pages.
//
// Held per process, an entry outlived the code it described, and a later
// program's function inherited the verdict. In the doctest lane one program's
// class getter and a later one's `handle` adapter shared an address, so the
// adapter was invoked as a getter and a program with no getter in it failed
// with "ArityError: missing required argument '_eh_args'". Whether the
// addresses collide is a matter of codegen sizes, so an unrelated change to
// the JIT could turn it on, and it never reproduced on the machine it was
// diagnosed from.
//
// The scoping is what this pins, not the collision: an address registered
// under one Runtime must be unknown to the next. A test that waited for two
// arenas to actually overlap would pass by luck.

#include <iostream>

#include <culebra.h>
#include <vm_embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps this file's
// internals from colliding with the other smokes.
namespace fn_registry_smoke_ns {

namespace {

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

// Two addresses no compiler will hand out, standing in for compiled bodies.
// The registries only ever compare them.
const void* const kGetterBody = reinterpret_cast<const void*>(0x100);
const void* const kNativeBody = reinterpret_cast<const void*>(0x200);

}  // namespace

inline int run() {
  bool ok = true;

  culebra::Runtime first;
  {
    culebra::RuntimeScope scope(first);
    _jit_register_getter_fn(kGetterBody);
    _jit_register_native_fn(kNativeBody);
    ok &= check(_jit_is_getter_fn(kGetterBody), "getter visible where declared");
    ok &= check(_jit_is_native_fn(kNativeBody), "native visible where declared");
  }

  {
    culebra::Runtime second;
    culebra::RuntimeScope scope(second);
    ok &= check(!_jit_is_getter_fn(kGetterBody),
                "a getter body does not carry into the next Runtime");
    ok &= check(!_jit_is_native_fn(kNativeBody),
                "a native body does not carry into the next Runtime");
  }

  {
    culebra::RuntimeScope scope(first);
    ok &= check(_jit_is_getter_fn(kGetterBody), "getter still known to its own");
    ok &= check(_jit_is_native_fn(kNativeBody), "native still known to its own");
  }

  if (ok) std::cout << "fn_registry smoke OK\n";
  return ok ? 0 : 1;
}

}  // namespace fn_registry_smoke_ns
