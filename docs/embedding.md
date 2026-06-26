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

## Handling script errors

Failures inside script code surface as `culebra::CulebraError`
(declared in `<shared.h>`), a `std::runtime_error` subclass carrying
the structured fields exposed to script `try`/`catch`:

```cpp
class CulebraError : public std::runtime_error {
public:
  std::string kind;   // e.g. "TypeError", "ArityError", user-thrown kind
  long line = 0, col = 0;
};
```

Catch it around `culebra::interpret`, `culebra::call`,
`culebra::JIT::run`, or any path that drives user code:

```cpp
try {
  culebra::call(env, "update", {culebra::Value(1L), culebra::Value("oops")});
} catch (const culebra::CulebraError& e) {
  std::println(stderr, "{}: {} at {}:{}",
               e.kind, e.what(), e.line, e.col);
} catch (const std::exception& e) {
  // Anything not raised by culebra itself (host bug, std failure)
  std::println(stderr, "host: {}", e.what());
}
```

Inside script code the same value appears as the `e` bound by
`catch e { ... }` with properties `e.kind` / `e.message` / `e.line` /
`e.col` — see [§15 of the language spec](language.md) for the user-side
shape and the standard kinds (`TypeError`, `ArityError`, `IOError`,
`ValueError`, `NameError`, `IndexError`, `KeyError`, `AssertionError`,
`InternalError`).

User-thrown values via script `throw expr` arrive as a separate
`culebra::CulebraException` carrying the raw `JitValue`; embedders
typically don't catch this directly — wrap the script-side `throw`
in a `try`/`catch` so it lands as a `CulebraError` with a chosen
`kind`.

## Defining host functions

`culebra::define` registers a C++ callable as a script-visible
function. Argument types and return type are deduced from the
callable's signature.

```cpp
culebra::define(env, "log", [](const std::string& msg) {
  std::cout << msg << "\n";
}, {"msg"});

culebra::define(env, "host_add",
                [](long a, long b) { return a + b; }, {"a", "b"});
```

Supported argument and return types: `long`, `int`, `double`, `float`,
`bool`, `std::string`, `std::string_view`, `const std::string&`, and
`culebra::Value` (passthrough). The deduced type maps to an annotation
on the script-side parameter (`Long`, `Float`, `Bool`, `String`), so a
mistyped argument fails at the call site rather than inside the
callable.

Parameter names default to `_arg0`, `_arg1`, ... when omitted — pass
explicit names if scripts will introspect via `fn.parameters()`.

For control over the raw `FunctionValue` (variadics, default values,
manual env access), bind the value directly:

```cpp
env->initialize("custom",
    culebra::Value(culebra::FunctionValue(
        {{"msg", false}},
        [](std::shared_ptr<culebra::Environment> env) {
          // hand-roll: pull args from env, return Value
          return culebra::Value();
        })),
    /*mut=*/false);
```

`include/stdlib_interp.h` has many examples of the raw form (Math.abs,
IO.print, Random.uniform, ...).

## AOT runtime archive (`libculebra_rt.a`)

The header-only path above includes every `culebra_runtime_*` helper
as `inline` into the embedder's TUs — fine for one binary, but if you
also want to **ship binaries produced by `culebra build`** (the AOT
mode, see [`binary_build.md`](binary_build.md)), those need a static
archive of the same helpers without the LLVM dependency. CMake emits a
base archive plus one small archive per heavy feature (N+1, not a 2^N
matrix), when configured with `-DCULEBRA_ENABLE_JIT=ON`:

| Archive | Contents |
|---|---|
| `libculebra_rt.a` | base — everything, but with **weak stubs** for the tensor / http / compress chokes (so it references no BLAS, OpenSSL, or zlib) |
| `libculebra_rt_tensor.a` | strong tensor choke (pulls BLAS / Accelerate) |
| `libculebra_rt_http.a` | strong http choke (pulls OpenSSL + zlib) |
| `libculebra_rt_compress.a` | strong compress choke (pulls zlib) |

`culebra build` always links the base archive, then **force-loads** a
feature archive only when the source AST references its namespace
(`Tensor` / `Http` / `Compress`), and appends that feature's external
libraries (BLAS / OpenSSL / zlib) on the same condition. The strong
choke overrides the base's weak stub; a program that uses none links
none of them. Dropping OpenSSL alone is worth ~4 MB (a non-Http binary
is ~5 MB vs ~9.5 MB for an Http one). The same usage-gating applies to
the `culebra wrap` archive (`libculebra_rt_wrap.a`).

### Embedders that bundle the AOT pathway

If you want your own embedder to also drive `culebra::JIT::build_object`
for AOT compilation, link in the same archive and tell `culebra build`
where to find it via `CULEBRA_RT_LIBPATH` (set at compile time by
CMake — see [`CMakeLists.txt`](../CMakeLists.txt) for the macro
definition).

For most embedders the archive is irrelevant — header-only inclusion
is the supported path. The archive only matters for the AOT subprocess
that links the standalone binary.

### `CULEBRA_RT_DEFINE_RUNTIME`

The macro `CULEBRA_RT_DEFINE_RUNTIME` switches every
`CULEBRA_RT_INLINE`-tagged helper from `inline` to `extern "C"` so
the archive's TU is the single owner of those symbols. Header-only
embedders **must not** define it; the AOT archive's source
(`src/runtime/culebra_rt.cc`) is the only TU that should.

### Cross-compile

`culebra build --target=<triple>` is currently host-archive only —
the archive on disk is the host's. For cross-targets, supply a
matching archive via `--rt-lib=<path>` (per-target auto-build is on
the roadmap, see `docs/binary_build.md`).

## Smoke tests

The repository includes two small samples that exercise the contract:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  four host threads each parse + interpret a script with try/catch,
  plus four more on the JIT path. 240 concurrent runs.
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  two Runtimes on a single thread, alternating between them, with
  independent PRNG state and independent JIT hook sets.
* [`tests/embedding/define_smoke.cc`](../tests/embedding/define_smoke.cc)
  — `culebra::define` round-trips through scripts and via
  `culebra::call`, including the auto-attached type annotation.
