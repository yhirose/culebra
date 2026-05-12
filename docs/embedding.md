# Embedding Culebra

Culebra is header-only. Include the headers, link LLVM if you want the
JIT, and you can drive the interpreter or JIT from C++.

## Minimal example

```cpp
#include <culebra.h>
#include <stdlib_interp.h>

int main() {
  auto env = culebra::environment({});  // stdlib bound

  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", "1 + 2", 5, msgs);

  culebra::Value val;
  culebra::interpret(ast, env, val, msgs, culebra::Debugger());
  // val.to_long() == 3
}
```

For the JIT, add `<stdlib_jit.h>`, call `culebra::install_jit_stdlib()`
once at startup, and use `culebra::JIT::run(ast)` instead of
`interpret`.

## Threading

The runtime is single-threaded *per Runtime*. To use Culebra from
multiple host threads, give each thread its own work:

```cpp
std::thread([&]{
  auto env = culebra::environment({});
  // ... independent of the main thread ...
}).detach();
```

Per-thread state (GC, exception carriers, PRNG, defer stack, shape
registry) lives in a thread-local default `Runtime` that's lazily
created on first use. Different threads see fully isolated state.

The PEG parser is also thread-local; concurrent `parse()` calls in
different threads are safe.

## Running multiple scripts on one thread

`culebra::Runtime` owns one VM context. Create explicit Runtimes when
you need more than one on the same thread (plugin isolation, sandboxed
DSLs, per-document state):

```cpp
culebra::Runtime rt_a, rt_b;

{
  culebra::RuntimeScope scope(rt_a);
  // ... compile / run inside rt_a's context ...
}
{
  culebra::RuntimeScope scope(rt_b);
  // ... separate state, separate GC, separate exception carriers ...
}
```

`RuntimeScope` is RAII: on destruction it restores the previous active
Runtime. Re-entering `rt_a` later picks up exactly where it left off.

Values created in one Runtime should not be passed to another. The GC
trackers, shape registries, and exception carriers belong to whichever
Runtime hosted the value's allocation.

## Per-Runtime extension hooks

`JIT::install_extension()` (and the convenience wrapper
`culebra::install_jit_stdlib()`) targets the currently-active Runtime.
That makes it possible to expose different host APIs to different
Runtimes:

```cpp
culebra::Runtime trusted, sandbox;

{
  culebra::RuntimeScope s(trusted);
  culebra::install_jit_stdlib();      // full stdlib (IO, Sys, Math, ...)
}
{
  culebra::RuntimeScope s(sandbox);
  install_jit_stdlib_restricted();    // your own subset
}

// Each Runtime resolves Math/IO/Sys against its own hook set.
```

With no `RuntimeScope` active, `install_jit_stdlib()` writes to a
process-wide default that every Runtime falls back to when its own
override is unset — that's the legacy single-VM and multi-thread
embedding path.

## Calling script functions from C++

After a script has run, any top-level `fn` or `let f = fn ...` lands
in the `Environment`. Call it from C++ via `culebra::call`:

```cpp
// Script: fn update(x, y) { x + y * 2 }
auto v = culebra::call(env, "update",
                      {culebra::Value(1L), culebra::Value(2L)});
// v.to_long() == 5
```

`call` binds positional args to positional params, packs any overflow
into `__ARGS__`, and translates a top-level `return` into the return
value. Default-valued parameters aren't resolved through this helper —
supply all positional arguments explicitly.

## Defining host functions

Add a host function to an `Environment` by binding a `FunctionValue`
to a name:

```cpp
env->initialize("log_msg",
    culebra::Value(culebra::FunctionValue(
        {{"msg", false}},
        [](std::shared_ptr<culebra::Environment> env) {
          std::cout << env->get("msg").to_string() << "\n";
          return culebra::Value();
        })),
    /*mut=*/false);
```

The `FunctionValue` body reads arguments from its callee `env`. See
`include/stdlib_interp.h` for many examples of this pattern (Math.abs,
IO.print, Random.uniform, ...).

## Smoke tests

The repository includes two small samples that exercise the contract:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  four host threads each parse + interpret a script with try/catch,
  plus four more on the JIT path. 240 concurrent runs.
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  two Runtimes on a single thread, alternating between them, with
  independent PRNG state and independent JIT hook sets.
