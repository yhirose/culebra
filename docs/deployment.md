# Deployment: Binaries, Embedding, Wrapping

Three ways to get Culebra code running outside the plain interpreter:
a standalone AOT binary (`culebra build`), an embedded interpreter/JIT
inside a C++ host, and an extended `culebra` binary that exposes your
own C++ classes as builtins (`culebra wrap`). They share one runtime
archive layout, described once in [§4](#4-shared-runtime-archive-layout)
and cross-referenced from each chapter.

## 1. Standalone binary build (`culebra build`)

`culebra build` compiles a `.cul` source into a standalone executable
via LLVM AOT codegen + system `cc` for the link step. No LLVM runtime
is embedded in the produced binary — the dependency surface is just
`libc++` / `libSystem` (macOS) or `libstdc++` / `libc` (Linux), plus
`Accelerate` / BLAS when the program references `Tensor`.

```sh
culebra build path/to/program.cul -o ./program
./program [args...]
```

The default invocation targets the host platform.

### Options

| Flag | Description |
|---|---|
| `-o <path>` | Output executable path (required). |
| `-O<level>` | Optimization level 0–3 (default 2). |
| `--emit-llvm` | Also write the program's LLVM IR (for debugging). |
| `--keep-symbols` | Keep local symbols in the output, for debugging (see [Symbol stripping](#symbol-stripping) below). |
| `--target=<triple>` | Cross-compile for the given LLVM triple. |
| `--sysroot=<path>` | Forwarded to `cc` as `--sysroot=`. |
| `--rt-lib=<path>` | Override the runtime archive path (required for cross-compile). |

### Environment overrides

| Variable | Effect |
|---|---|
| `CULEBRA_VERBOSE=1` | Print the object path and full link command. |
| `TMPDIR` | Directory for the intermediate object file (default `/tmp`). |

### Tensor-free and Http-free binaries

Each feature axis force-loads independently (mechanism: [§4](#4-shared-runtime-archive-layout)),
so a program using neither `Tensor` nor `Http` links only the base
archive. Dropping OpenSSL alone is worth ~4 MB (a non-Http binary is
~5 MB vs ~9.5 MB for an Http one, since OpenSSL is statically linked).

Verify with `otool -L` (macOS) / `ldd` (Linux):

```sh
$ culebra build my-program.cul -o /tmp/my-program     # no Tensor use
$ otool -L /tmp/my-program
/tmp/my-program:
        /usr/lib/libc++.1.dylib
        /usr/lib/libSystem.B.dylib
```

A Tensor user keeps the full archive plus the framework:

```sh
$ otool -L /tmp/microgpt_tensor
/tmp/microgpt_tensor:
        /usr/lib/libc++.1.dylib
        /System/Library/Frameworks/Accelerate.framework/.../Accelerate
        /usr/lib/libSystem.B.dylib
```

### Symbol stripping

The embedded runtime archive carries thousands of local symbols
(`GCC_except_table*`, template and string instantiations) useless in a
distributed executable. The link discards them by default (`-Wl,-x` —
understood by ld64, GNU ld and lld alike), shrinking the binary by
~30% (e.g. a Term/IO program drops from ~7.6 MB to ~5.3 MB) while
keeping the global/dynamic symbols the loader needs. Pass
`--keep-symbols` to retain them for debugging.

### Cross-compilation

`--target=<triple>` selects the LLVM target. Common triples:

- `x86_64-unknown-linux-gnu`
- `aarch64-unknown-linux-gnu`
- `x86_64-apple-macosx`

Cross-compile requires the user to provide:

1. A **sysroot** for the target (target's C++ headers, `libc`, CRT
   files). Pass via `--sysroot=<path>`.
2. A **runtime archive built for the target**. Pass via
   `--rt-lib=<path>`. The host's `libculebra_rt.a` won't work because
   its object files match the host triple.

To build the runtime for a target, point CMake at the target's
toolchain (the same source tree as the host build, but configured
against the target's sysroot and `cc`).

Current limitations: the runtime is not bundled for any cross target —
the user produces it via their own CMake / toolchain (see the example
below) — and `--target=<triple>` with `Tensor` is rejected, since BLAS
link flags are host-specific and would mis-link the target binary.
Drop `Tensor` references or wait for a future phase.

#### Example (Linux x86_64 from macOS host)

```sh
# 1. Build the runtime for the target (one-time per target).
#    Requires a Linux sysroot at $LINUX_SYSROOT and a cross `cc`.
cmake -B build-linux-x86_64 \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS="--target=x86_64-unknown-linux-gnu --sysroot=$LINUX_SYSROOT" \
      -DCMAKE_CXX_FLAGS="--target=x86_64-unknown-linux-gnu --sysroot=$LINUX_SYSROOT" \
      -DCULEBRA_ENABLE_JIT=ON
# The base archive is Tensor-free already (weak stubs), and cross builds
# reject Tensor (see limitations above), so build the base archive.
cmake --build build-linux-x86_64 --target culebra_rt

# 2. Cross-compile the program.
culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./my-program-linux

# 3. Verify (on a Linux host or via emulation).
file ./my-program-linux
# ELF 64-bit LSB executable, x86-64, ...
```

## 2. Embedding Culebra in a C++ host

Culebra is header-only. Include the headers, link LLVM if you want the
JIT, and you can drive the interpreter or JIT from C++.

### Minimal example

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

### Threading

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

### Running multiple scripts on one thread

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

### Per-Runtime extension hooks

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

### Calling script functions from C++

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

### Handling script errors

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

### Defining host functions

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

### Bundling the AOT pathway from your own embedder

If your embedder also wants to drive `culebra::JIT::build_object` for
AOT compilation (as `culebra build` does, [§1](#1-standalone-binary-build-culebra-build)),
link the same runtime archive described in [§4](#4-shared-runtime-archive-layout)
and tell `culebra build` where to find it via `CULEBRA_RT_LIBPATH` (set
at compile time by CMake — see [`CMakeLists.txt`](../CMakeLists.txt)
for the macro definition). For most embedders the archive is
irrelevant — header-only inclusion is the supported path. The archive
only matters for the AOT subprocess that links the standalone binary.

The macro `CULEBRA_RT_DEFINE_RUNTIME` switches every
`CULEBRA_RT_INLINE`-tagged helper from `inline` to `extern "C"` so the
archive's TU is the single owner of those symbols. Header-only
embedders **must not** define it; the AOT archive's source
(`src/runtime/culebra_rt.cc`) is the only TU that should.

### Smoke tests

The repository includes small samples that exercise the contract:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  four host threads each parse + interpret a script with try/catch,
  plus four more on the JIT path. 240 concurrent runs.
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  two Runtimes on a single thread, alternating between them, with
  independent PRNG state and independent JIT hook sets.
* [`tests/embedding/define_smoke.cc`](../tests/embedding/define_smoke.cc)
  — `culebra::define` round-trips through scripts and via
  `culebra::call`, including the auto-attached type annotation.

## 3. Wrapping C++ libraries (`culebra wrap`)

`culebra wrap` builds an **extended culebra binary** with your own C++
classes available as builtins — no fork of the interpreter, no plugin
ABI. You write a short declaration TU; the C++ compiler instantiates
the glue (pybind11-style), and the result works identically under the
interpreter, `--jit`, and AOT binaries produced by the extended
`culebra build`.

### Declare

Given a header-only class of yours:

```cpp
// vec2.hpp — plain C++, knows nothing about culebra
namespace demo {
class Vec2 {
 public:
  Vec2(double x, double y);
  double len() const;
  void scale(double k);
  std::string show() const;
  Vec2 unit() const;          // by value
  static long dims();
};
}
```

write one declaration TU:

```cpp
// vec2_binding.cpp
#include <wrap.h>
#include "vec2.hpp"

namespace {
const bool registered = [] {
  culebra::wrap<demo::Vec2>("Geo", "Vec2")
      .ctor<double, double>({"x", "y"})
      .method<&demo::Vec2::len>("len")
      .method<&demo::Vec2::scale>("scale", {"k"})
      .method<&demo::Vec2::show>("show")
      .method<&demo::Vec2::unit>("unit")
      .static_method<&demo::Vec2::dims>("dims");
  return true;
}();
}
```

The member function is a *template* argument (`method<&T::m>`), so each
method gets its own compiled thunk. Parameter names are optional; they
drive error messages and keyword binding on the interpreter.

You declare construction (`ctor`) but never destruction: there is no
`.dtor` builder. The wrapped type's `~T()` is invoked automatically by
the deterministic-drop machinery the handle inherits (see below), so
the C++ destructor is the only thing you write.

### Build

```sh
culebra wrap vec2_binding.cpp -o ext-culebra
# linking against a prebuilt library:
culebra wrap mylib_binding.cpp --link "-L/opt/mylib/lib -lmylib" -o ext-culebra
```

`culebra wrap` rebuilds the culebra source tree with your TUs compiled
in (the checkout it was built from, or `$CULEBRA_HOME`), caching under
`~/.cache/culebra-wrap/`. With ccache configured the rebuild is
effectively compile-your-declaration + relink. `--lto` produces an
optimized binary (slower build).

### Use — on every backend

```culebra
# doctest: skip
let v = Geo.Vec2.new(3.0, 4.0)
puts(v.len())          # => 5
v.scale(2.0)
let u = v.unit()       # by-value return -> a NEW owned instance
v.drop()               # ~Vec2 runs NOW (deterministic)
v.len()                # !! ClosedError
```

Wrapped instances are resources with the full lifetime model: scope-exit
deterministic drop (cycles included), an idempotent explicit `drop()`,
and `ClosedError` on use-after-drop. `ext-culebra build script.cul`
produces standalone AOT binaries that carry the binding.

The binding and the wrapped library (`--link`) are pulled into an AOT
binary only when the script names a wrapped namespace — a program built by
`ext-culebra` that uses none of the wrapped classes links none of the
wrapped library, same as a stock `culebra build`. The check is a
conservative identifier match (`Geo` etc.), so it can over-link but never
under-link. This is the same usage-gating described in [§4](#4-shared-runtime-archive-layout)
for `Tensor` / `Http`, applied to the `libculebra_rt_wrap.a` archive.

### Marshalling

| C++ | Culebra |
|---|---|
| `long` / `int` | `Long` |
| `double` / `float` | `Float` |
| `bool` | `Bool` |
| `std::string` / `std::string_view` / `const char*` | `String` |
| `T` by value, `std::unique_ptr<T>` | owned instance of wrapped `T` |
| `std::shared_ptr<T>` | instance holding one share |

A method returning `T&` / `const T&` of a wrapped class is a
**compile-time error** under `.method` — references are not an
ownership shape. Declare it `.borrowed_method` instead:

```cpp
.borrowed_method<&Box::inner>("inner")     // Counter& inner()
```

The result is a **borrowing handle**: it does not own (dropping it is a
no-op), it keeps its parent alive while it exists, and every access is
validated — the parent explicitly dropped, or mutated by a non-const
method since the borrow was taken (each wrapped instance carries a
generation counter, bumped automatically by non-const dispatch), raises
`ClosedError` instead of touching freed or reallocated memory:

```culebra
let b = __Foreign.Box.new(3)
let c = b.inner()
b.reset(9)             # non-const -> generation bump
c.value()              # !! ClosedError
```

A non-const method that provably never invalidates borrows can opt out:
`.method<&T::touch>("touch", {}, culebra::wrap_policy::preserves_borrows)`.
Misdeclaring const-ness or this flag is an author-contract violation
(the sol2/pybind11 deal) — the failure mode is a spurious or missed
stale error, never memory-unsafety on the culebra side.

Containers (`std::vector`/`std::map`) and callbacks are not yet
marshalled.

See `examples/wrap/` for the complete working demo this page is based
on, and `tests/wrap_test.sh` for the end-to-end pipeline check.

## 4. Shared runtime archive layout

All three workflows above ship a static **runtime archive** so that
generated or embedding binaries don't need the LLVM dependency. CMake
emits it as a base archive plus one small archive per heavy feature
(N+1, not a 2^N matrix), when configured with `-DCULEBRA_ENABLE_JIT=ON`:

| Archive | Contents |
|---|---|
| `libculebra_rt.a` | base — everything, but with **weak stubs** for the tensor / http / compress chokes (so it references no BLAS, OpenSSL, or zlib) |
| `libculebra_rt_tensor.a` | strong tensor choke (pulls BLAS / Accelerate) |
| `libculebra_rt_http.a` | strong http choke (pulls OpenSSL + zlib) |
| `libculebra_rt_compress.a` | strong compress choke (pulls zlib) |
| `libculebra_rt_wrap.a` | `culebra wrap` bindings |

`culebra build` (and extended `ext-culebra build` binaries) always
link the base archive, then **force-load** a feature archive only when
the source AST references its namespace (`Tensor` / `Http` /
`Compress`, or a wrapped namespace), appending that feature's external
libraries (BLAS / OpenSSL / zlib) on the same condition. The strong
choke overrides the base's weak stub, so a program that uses none of
these features links none of them.

These archives are **embedded directly into the `culebra` driver**
via cpp-embedlib — the driver is a single self-contained binary, no
sibling `.a` files need to be installed. On first invocation of
`culebra build`, the required archives are materialized to
`$HOME/.cache/culebra/<fingerprint>/lib*.a`; subsequent invocations
reuse the cache. The fingerprint is a content-hash of the embedded
archives, so a freshly-built `culebra` automatically isolates its cache
from older copies.

`culebra build --target=<triple>` is currently host-archive only — the
embedded/cached archive is built for the host. For cross-targets,
supply a matching archive via `--rt-lib=<path>` (per-target auto-build
is on the roadmap; see [§1](#1-standalone-binary-build-culebra-build)
for the manual cross-build steps).
