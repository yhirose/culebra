# Deployment: Binaries, Embedding, Wrapping

Three ways to get Culebra code running beyond a plain `culebra` run:
a standalone AOT binary (`culebra build`), an embedded VM/JIT
inside a C++ host, and an extended `culebra` binary that exposes your
own C++ classes as builtins (`culebra wrap`). They share one runtime
archive layout, described once in [§4](#4-shared-runtime-archive-layout)
and cross-referenced from each chapter.

Contents
--------

1. [Standalone binary build (`culebra build`)](#1-standalone-binary-build-culebra-build)
2. [Embedding Culebra in a C++ host](#2-embedding-culebra-in-a-c-host)
3. [Wrapping C++ libraries (`culebra wrap`)](#3-wrapping-c-libraries-culebra-wrap)
4. [Shared runtime archive layout](#4-shared-runtime-archive-layout)

The development subcommands — `test`, `lint`, `fmt` and `dap` — are the
subject of [`tooling.md`](tooling.md) instead.

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

### Host requirements

Running a script needs nothing beside the `culebra` binary, and neither
does `--jit` — the LLVM that compiles the code is inside it. `culebra
build` is the one subcommand that reaches outside: the codegen is
in-process, but a link needs the platform's object-file plumbing, which
a machine that has never built anything does not have. `build` checks
before compiling anything, and on a terminal offers to install what is
missing rather than only naming it.

| Host | What the link step needs | How to get it |
|---|---|---|
| macOS | Xcode Command Line Tools — the `cc` shim in `/usr/bin` is not it, and the SDK stubs it brings are what a Mach-O links `libSystem` against | `culebra toolchain install` (starts Apple's installer), or `xcode-select --install` |
| Linux | `cc`, libstdc++ and the C runtime startup files | `sudo apt install g++` (Debian/Ubuntu), `sudo dnf install gcc-c++` (Fedora) |
| Windows | the mingw CRT objects and static libraries — **no compiler and no linker** | `culebra toolchain install` (~8 MB) |

Nothing else: the archives an AOT link needs, third-party statics
included, come out of the `culebra` binary itself ([§4](#4-shared-runtime-archive-layout)).
The one library still linked from the system is zlib, which a program
using `Http`, `Compress` or `to_png` reaches through `-lz` — already
present on macOS and in the Windows kit, but on Linux the linker's
`libz.so` symlink lives in the dev package (`sudo apt install
zlib1g-dev`, `sudo dnf install zlib-devel`).

Linux is the one host culebra will not install for: a C++ toolchain
there belongs to the distribution's package manager and needs root, and
running `sudo` on a user's behalf is not something a compiler should do.
`culebra toolchain install` prints the one command for the distro and
stops.

### `culebra toolchain`

```sh
culebra toolchain status                  # what is here, and whether a link can happen
culebra toolchain install                 # get what is missing
culebra toolchain install --from <zip>    # a kit built locally, instead of the release's
culebra toolchain uninstall               # remove what culebra installed
```

On Windows, `install` fetches the kit published with the release whose
version this binary reports, checks it against the digest published
beside it, and unpacks it into

```
%LOCALAPPDATA%\culebra\toolchain\<version>\
```

Nothing is elevated, no `PATH` is edited and no registry key is written;
`uninstall` is a removal of that directory, and deleting it by hand does
the same thing. Kits are per-version because the kit's libstdc++ has to
be the one this binary's embedded runtime archives were compiled against
— `install` refuses a kit whose manifest names a different version, and
upgrading culebra means installing the kit that came with it.

Elsewhere `install` is a thin thing: on macOS it starts Apple's Command
Line Tools installer (a GUI flow this process cannot wait on, so it
hands over and asks you to re-run the build), and on Linux it prints the
package command. `uninstall` there reports that culebra installed
nothing to remove.

### Why Windows needs no compiler

Windows ships no toolchain at all, and what an AOT link is missing there
is a linker, not a compiler: `culebra build` emits its own object
in-process. So the linker is inside the binary — culebra links lld into
itself, on top of the LLVM it already carries for the JIT — and what the
kit supplies is only the mingw half of a link: the CRT objects,
libstdc++/libgcc, and the Win32 import libraries.

Sharing that LLVM is what makes it cheap, and the alternatives were
measured rather than assumed. MSYS2's `ld.lld.exe` is 5.7 MB of program
linked against a 147 MB `libLLVM` DLL, so a kit shipping it measured
55 MB compressed. A standalone lld built against the same static
archives needs no DLL, but shares nothing with the binary beside it and
came out at 41 MB compressed. Linking lld into `culebra` costs the
download 23 MB — more than the 4 MB of lld archives, because lld's COFF
driver references every LLVM backend and so drags them all in — and
leaves the kit at 8 MB. Whoever runs `culebra build` therefore fetches
about 60 MB in total rather than 84 MB, at the price of 23 MB to
everyone who never runs it.

The kit is packed at release time out of the same MSYS2 UCRT64 tree that
compiled the runtime archives inside the binary
(`misc/windows_toolchain/pack_windows_toolchain.sh`), which is what makes their libstdc++
match by construction rather than by a version comparison someone has to
get right. It records the linker command line that toolchain's C++
driver would build (`link-recipe.txt`), and culebra splices its own
objects and feature libraries into the middle of it — so the link
culebra performs is the one the driver would have performed, without
the driver.

UCRT64 rather than another MSYS2 environment: the same libstdc++ ABI
(UCRT64's clang sits on GCC's libstdc++; CLANG64's is libc++, a
different ABI) and the same C runtime. lld rather than GNU ld because
only lld dead-strips a PE the way the ELF and Mach-O linkers do — GNU ld
keeps every function's unwind record, and each record keeps its
function, so `--gc-sections` would leave the whole runtime archive in
every program.

Building Culebra itself from source on Windows is a different matter and
does want the full MSYS2 toolchain; [`CONTRIBUTING.md`](../CONTRIBUTING.md)
covers that.

### Options

| Flag | Description |
|---|---|
| `-o <path>` | Output executable path (required). |
| `-O<level>` | Optimization level 0–3 (default 2). |
| `--emit-llvm` | Also write the program's LLVM IR (for debugging). |
| `--keep-symbols` | Keep the symbol table in the output, for debugging (see [Symbol stripping](#symbol-stripping) below). |
| `--no-tls` | Link `Http` without OpenSSL. ~3.9 MB smaller; `https://` and `wss://` then raise `HttpError` (see [Building Http without TLS](#building-http-without-tls) below). |
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
archive. Dropping OpenSSL alone is worth ~4.7 MB (a non-Http binary is
~0.3 MB vs ~5.1 MB for an Http one, since OpenSSL is statically
linked). The same gating covers two self-hosted subsystems that link
no external library but carry their own code. The first is the regex
engine (`Regex`, including `re'...'` literals). That axis costs
~0.9 MB — cpp-regexlib plus the namespace's own group, measured as
~0.55 MB of code and ~0.24 MB of tables. It
still needs the weak/strong split even though nothing external is at
stake, because a `__builtin_cpu_supports` runtime-dispatch check inside
it makes the compiler emit a start-up CPUID constructor for whatever
translation unit compiles the engine — leaving that unconditional would
put the constructor in every binary, not just its own code.

The second is Tensor's elementwise kernels, and they are on the axis
for a reason the backend choke does not cover. `culebra::
tensor_eval_node` gates BLAS and Metal, but cpp-tensorlib's
`map_binary` instantiations are pure C++, so nothing external kept them
out — and the generic arithmetic helper reaches them whenever either
operand could be a `Tensor`, which is every binary that does
arithmetic at all. `culebra::tensor_binop` and
`culebra::tensor_inplace_binop` (the lazy `+` and the in-place `+=`
paths) therefore take the same weak/strong split, which is worth
~115 KB — a quarter of a hello. `Proc`'s
fork/exec layer, the PNG/TTF decoders behind
`Canvas.Sprite.from_png` / `Canvas.Font`, and `PEG` (cpp-peglib) link
nothing external either but need no choke of their own: they compile
as plain code reached only through their namespace's dispatch table,
so a program that never names them never links them (mechanism:
[§4](#4-shared-runtime-archive-layout)). `PEG` is the one namespace
where that dead-stripping doesn't quite reach every byte: `culebra::
pegparser::compile()` itself is confirmed absent from a binary that
never names `PEG` (verified with `nm -C --defined-only` on a
`--keep-symbols` build), but peglib's `Ope` class hierarchy still
leaves vtables and typeinfo behind that `--gc-sections` keeps
regardless of use, plus a handful of `std::function`-wrapped local
lambdas whose comdat template instantiation survives after the
function that would have called it is pruned (a linker limitation, not
a reachable call path). Both are inert — nothing in an unused-PEG
binary ever executes either — fixed at a measured ~53 KB total that
every binary carries rather than force-loading a second archive over
it.

Verify with `otool -L` (macOS) / `ldd` (Linux):

```sh
$ culebra build my-program.cul -o /tmp/my-program     # no Tensor use
$ otool -L /tmp/my-program
/tmp/my-program:
        /usr/lib/libc++.1.dylib
        /System/Library/Frameworks/CoreServices.framework/.../CoreServices
        /usr/lib/libSystem.B.dylib
        /System/Library/Frameworks/CoreFoundation.framework/.../CoreFoundation
```

A Tensor user keeps the full archive plus the framework:

```sh
$ otool -L /tmp/microgpt_tensor
/tmp/microgpt_tensor:
        /usr/lib/libc++.1.dylib
        /System/Library/Frameworks/Accelerate.framework/.../Accelerate
        /usr/lib/libSystem.B.dylib
```

### Building Http without TLS

OpenSSL is 82% of what `Http` adds to a binary, and a program that only
ever speaks `http://` pays all of it. `--no-tls` links the other half of
the Http axis — cpp-httplib built without TLS — and appends no OpenSSL
archive:

```sh
$ culebra build fetch.cul -o fetch            # 5.07 MB
$ culebra build fetch.cul -o fetch --no-tls   # 1.22 MB
```

Measured on macOS arm64 for a one-line `Http.get`. The 3.9 MB is
2.1 MB of code, 1.6 MB of tables and 0.2 MB of strings; the tables are
why compressing them instead would not work, since precomputed elliptic
curve points are incompressible (deflate takes that section to 87%).

The scan cannot decide this on its own. Which archive an axis needs is
read off the names a program mentions, and `Http.get(url)` does not say
whether `url` is `https://` — that is a run-time value, and a program
that builds one from a config file would compile fine and fail in the
field. So the choice is yours to make at build time, and a binary built
this way refuses a TLS URL rather than falling back to plaintext:

```
HttpError: Http: this binary was built with --no-tls, so https URLs are
not supported: https://example.com/  at 1:9.
```

`wss://` refuses the same way. Everything else is unchanged: `http://`,
`ws://`, the server, and the transparent gzip decode all still work, and
zlib is still linked.

This is the one place where an AOT binary is allowed to diverge from the
other two lanes: the VM and `--jit` always have TLS, since they run
inside a `culebra` that carries OpenSSL either way. The divergence is
confined to a flag you passed, it is loud rather than silent, and the
default is unchanged — build without `--no-tls` and all three lanes
agree as they always did.

### Symbol stripping

Nothing in the output reads its own symbol table — the runtime resolves
no symbol by name — so the build removes it in two steps. The link
discards the local symbols (`-Wl,-x`, understood by ld64, GNU ld and
lld alike: the embedded runtime archive carries thousands of
`GCC_except_table*`, template and string instantiations), then the
platform's `strip` tool removes the global symbol table the linker
leaves behind, the same step the release packaging applies to
`culebra` itself. Together they take a `print("hello")` binary from
0.52 MB to 0.42 MB; the dynamic symbols the loader needs survive
both. Pass `--keep-symbols` to skip both for debugging. Cross-compiled
outputs (`--target`) stop after the link step, since the host's
`strip` does not read a foreign object format; a `strip` missing from
`PATH` is likewise not an error.

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
JIT, and you can drive the bytecode VM or the JIT from C++.

### Minimal example

```cpp
#include <culebra.h>
#include <vm/embed.h>

int main() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;   // stdlib installed, traits registered

  culebra::vm::Value val;
  std::vector<std::string> msgs;
  embed.run_source("<inline>", "1 + 2", val, msgs);
  // val.to_long() == 3
}
```

`vm::Embed` is a session: each `run_source` sees the top-level bindings
every earlier run made, and the host reads them back afterwards
(`embed.global("x")`) or calls into them (`embed.call`, below). Use one
`Embed` per independent engine instance. `run_source` copies its source
(the parse's AST views into that copy, and the session owns it as long
as the programs that reference it); `run(ast, source, ...)` is the
lower-level entry when the host parsed the input itself — pass the
buffer the AST's tokens view into.

To run a whole program the way the CLI does — imports resolved, stdlib
preamble spliced — load it and hand the module list over:

```cpp
culebra::ModuleLoader loader;
auto modules = loader.load_program(path, entry_source, msgs);
culebra::splice_stdlib_preamble(modules);
culebra::vm::Value val;
embed.run(modules, val, msgs);
```

For the LLVM lane, add `<stdlib_rt.h>`, call
`culebra::install_jit_stdlib()` once at startup, and use
`culebra::JIT::run(ast)`.

### Building your host program

There is no install step and no library to link against: a host builds
against a culebra checkout. The headers reach into the vendored
libraries, so those directories are on the include path too.

The VM lane needs no LLVM at all:

```sh
c++ -std=c++23 \
    -I culebra/include \
    -I culebra/vendor/cpp-peglib \
    -I culebra/vendor/cpp-vmlib \
    -I culebra/vendor/cpp-unicodelib \
    -I culebra/vendor/cpp-tensorlib/include \
    -I culebra/vendor/stb \
    -I culebra/vendor/cpp-regexlib \
    -I culebra/vendor/cpp-fstlib \
    host.cpp -lz -o host
```

Every entry is load-bearing: the build stops in `font_ttf.h` without
`vendor/stb`, in `regex.h` without `vendor/cpp-regexlib`, and in `fst.h`
without `vendor/cpp-fstlib`, because the stdlib reaches all three
unconditionally. `-lz` is the zlib an AOT link needs
for the same reason ([§1](#host-requirements)) — `Compress` and the PNG
writer refer to it whether or not the script does.

The LLVM lane — `JIT::run`, `JIT::build_object` — adds the define that
turns those headers on, plus LLVM:

```sh
c++ -std=c++23 -DCULEBRA_JIT_ENABLED \
    -I "$(llvm-config --includedir)" \
    ...the -I list above... \
    host.cpp $(llvm-config --ldflags --libs --system-libs) -lz -o host
```

It has to be LLVM 20 or newer — the floor `CMakeLists.txt` enforces for
culebra's own build. On a machine carrying several, a bare `llvm-config`
is whichever one is first on `PATH`, which is not necessarily that one:
name the version's own binary (`llvm-config-20`, or the one under the
prefix you built culebra against) rather than trusting the plain name.

Without `-DCULEBRA_JIT_ENABLED`, `jit.h` and `vm_lowering.h` preprocess
to nothing: `culebra.h` still compiles, the VM still runs, and no part of
the host refers to LLVM. It is the same switch a no-JIT build of culebra
itself uses.

`Http` and `SQLite` stay out unless `CULEBRA_HTTP_ENABLED` /
`CULEBRA_SQLITE_ENABLED` are defined — the names CMake sets for a
`culebra` build. Each then wants its vendored directory on the include
path (`vendor/cpp-httplib`, `vendor/sqlite`) and brings its own link
dependency. Undefined, those two names are simply absent from both
lanes.

What the script sees as `Sys.argv` is a process-wide holder. Fill it
once at startup:

```cpp
culebra::sys_argv() = {"--verbose", "input.txt"};
```

### Threading

The runtime is single-threaded *per Runtime*. To use Culebra from
multiple host threads, give each thread its own work:

```cpp
std::thread([&]{
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
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

`culebra::install_extension()` (and the convenience wrapper
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

After a run, any top-level `fn` or `let f = fn ...` lands in the
session. Call it from C++ via `Embed::call`:

```cpp
// Script: fn update(x, y) { x + y * 2 }
std::vector<culebra::vm::Value> args;
args.emplace_back(int64_t{1});
args.emplace_back(int64_t{2});
auto v = embed.call("update", std::move(args));
// v.to_long() == 5
```

`call` binds positional args to positional params. `vm::Value` args are
consumed by the call (pass a fresh vector); the result comes back
owned by the returned handle.

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

Catch it around `Embed::call`, `culebra::JIT::run`, or any path that
drives user code (`Embed::run*` reports through `msgs` instead, the
same text the CLI prints):

```cpp
try {
  std::vector<culebra::vm::Value> args;
  args.emplace_back(int64_t{1});
  args.emplace_back(std::string_view("oops"));
  embed.call("update", std::move(args));
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

A script `throw expr` reaching `Embed::call` is converted for the host:
the thrown object's `kind`/`message` become the `CulebraError`'s fields
(anything else renders through its display form), so the
"catch CulebraError" contract covers user throws too.

### Interrupts are not errors

A Ctrl+C, or an isolate's cancel, is not a failure of the script — it is a
request to stop — so it is **not** a `CulebraError`:

```cpp
class Interrupted {          // derives from nothing, not even std::exception
public:
  const char* what() const noexcept;   // "interrupted" / "isolate cancelled"
};
```

Neither handler above can catch one. That is deliberate: a handler written
to report script errors would otherwise turn a press into a diagnostic and
carry on running, and the flag is one-shot, so nothing would stop. Only the
component that hosts the loop answers for an interrupt, by naming the type:

```cpp
try {
  embed.call("update", std::move(args));
} catch (const culebra::Interrupted&) {
  // stop this run — do not report it as a failure
  return;
} catch (const culebra::CulebraError& e) {
  ...
}
```

An embedder that never wants Ctrl+C to stop anything simply installs no
handler for it (`culebra::install_sigint_handler()` is opt-in, CLI-only).
Script code is unaffected: `catch e` still binds an error object with
`e.kind == "Interrupted"` and may resume, and the press is consumed when
that throw happens, so a caught interrupt does not fire twice.

### Defining host functions

`Embed::define` registers a C++ callable as a script-visible function.
Argument types and return type are deduced from the callable's
signature.

```cpp
embed.define("log", [](std::string msg) {
  std::cout << msg << "\n";
}, {"msg"});

embed.define("host_add",
             [](int64_t a, int64_t b) { return a + b; }, {"a", "b"});
```

Supported argument and return types: `int64_t`, `long`, `long long`,
`int`,
`double`, `float`, `bool`, `std::string`, `std::string_view`, and
`culebra::vm::Value` (passthrough). A mistyped argument raises a
catchable `TypeError` at the call site rather than inside the
callable; a wrong argument count raises `ArityError`. Binding is
positional (host functions take no keyword arguments); for a richer
surface — methods, handles, keyword binding — declare a class through
`culebra wrap` (§3), which serves every lane including AOT.

### Bundling the AOT pathway from your own embedder

For most embedders `libculebra_rt.a` is irrelevant — header-only
inclusion is the supported path, and running scripts in-process needs
no archive at all. It matters in exactly one case: your embedder wants
to *emit standalone binaries*, driving `culebra::JIT::build_object`
the way `culebra build` does ([§1](#1-standalone-binary-build-culebra-build)).
The archive supplies `culebra_aot_bootstrap` and the runtime helpers
the emitted object calls into; its layout is
[§4](#4-shared-runtime-archive-layout).

**Where to get it.** A CMake build configured with
`-DCULEBRA_ENABLE_JIT=ON` writes `libculebra_rt.a` (and the feature
archives) into the build directory. The distributed `culebra` driver
carries the same archives embedded, and materializes them on the first
`culebra build` into `$HOME/.cache/culebra/<fingerprint>/` — a path you
can point your own link step at.

**Emitting the object.** Load the program through `ModuleLoader` and
splice the stdlib preamble *before* handing the modules to
`build_object` — the preamble is what defines `println` / `inspect`.
(The `Stringer` / `Eq` / `Comparable` declarations are not part of it:
`build_object` prepends those itself, as `JIT::run` does.)

```cpp
#include <culebra.h>
#include <frontend/module_loader.h>
#include <stdlib/preamble.h>
#include <stdlib/bindings.h>

int main() {
  std::string src = "1 + 2";                  // read it off disk in a real host
  std::vector<std::string> msgs;
  culebra::ModuleLoader loader;
  auto modules = loader.load_program("prog.cul", src, msgs);
  culebra::splice_stdlib_preamble(modules);   // required — see below

  culebra::install_jit_stdlib();
  return culebra::JIT::build_object(modules, "prog.o", /*opt_level=*/2);
}
```

> **Skipping `splice_stdlib_preamble` fails silently.** The object still
> emits and links, and the binary still exits 0 — it just produces no
> output, because `println` resolved to nothing. There is no diagnostic;
> if your AOT binary runs quietly and does nothing, this is why.

**Linking it.** The emitted object needs the archive, dead-stripping,
and the C++ runtime — nothing else for a program that uses no heavy
feature:

```bash
# macOS
cc prog.o libculebra_rt.a -Wl,-dead_strip -Wl,-x -lc++ -o prog

# Linux (the object is non-PIC, so -no-pie is required on PIE-by-default distros)
cc prog.o libculebra_rt.a -Wl,--gc-sections -Wl,-x -no-pie -lstdc++ -lm -o prog

# Either: drop the remaining symbol table (what `culebra build` does by default)
strip prog
```

A program that references a feature namespace (`Tensor`, `Http`,
`Compress`, `SQLite`, `Regex`, `Canvas`, …; the [§4](#4-shared-runtime-archive-layout)
table) additionally needs that feature's archive **force-loaded** —
`-Wl,-force_load,<archive>` on Mach-O, `-Wl,--whole-archive <archive>
-Wl,--no-whole-archive` on ELF — plus the feature's own external
libraries. A plain append does *not* work: the base archive's weak stub
already satisfies the symbol, so the member would never be pulled in
([§4](#4-shared-runtime-archive-layout) covers the gating).

Rather than transcribing these rules, run the driver once with
`CULEBRA_VERBOSE=1 culebra build prog.cul -o prog` — it prints the exact
`link:` command line it used, feature archives and all, which is the
same shape your embedder needs. (`--rt-lib=<path>` is the CLI's own
override for pointing at a non-default archive, e.g. a cross-built one.)

The macro `CULEBRA_RT_DEFINE_RUNTIME` switches every
`CULEBRA_RT_INLINE`-tagged helper from `inline` to `extern "C"` so the
archive's TU is the single owner of those symbols. Header-only
embedders **must not** define it; the AOT archive's source
(`src/runtime/culebra_rt.cc`) is the only TU that should.

### Smoke tests

The repository includes small samples that exercise the contract:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  four host threads each run an Embed session with try/catch, plus
  four more on the JIT path. 240 concurrent runs.
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  two Runtimes (each with its own Embed) on a single thread,
  alternating between them, with independent PRNG state and
  independent JIT hook sets.
* [`tests/embedding/define_smoke.cc`](../tests/embedding/define_smoke.cc)
  — `Embed::define` round-trips through scripts and via `Embed::call`,
  including the deduced-type rejection.

## 3. Wrapping C++ libraries (`culebra wrap`)

`culebra wrap` builds an **extended culebra binary** with your own C++
classes available as builtins — no fork of the runtime, no plugin
ABI. You write a short declaration TU; the C++ compiler instantiates
the glue (pybind11-style), and the result works identically under
`--vm`, `--jit`, and AOT binaries produced by the extended
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
#include <interop/wrap.h>
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
drive error messages and keyword binding at runtime.

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
inspect(v.len())  # => 5
v.scale(2.0)
let u = v.unit()  # by-value return -> a NEW owned instance
v.drop()          # ~Vec2 runs NOW (deterministic)
v.len()           # !! ClosedError
```

Wrapped instances are resources with the full lifetime model: scope-exit
deterministic drop (cycles included), an idempotent explicit `drop()`,
and `ClosedError` on use-after-drop. `ext-culebra build script.cul`
produces standalone AOT binaries that carry the binding.

A wrapped C++ body that throws — ctor, method or static — does not escape
the process: a `std::exception` surfaces as a catchable `RuntimeError`
carrying its `what()`, reported at the call like any other error, and a
`culebra::CulebraError` the body throws passes through with its own kind
and message.

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
| `U&` / `const U&` / `U*` / `const U*` (wrapped `U`, method parameter only) | a handle of `U` — see below |

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
b.reset(9)  # non-const -> generation bump
c.value()   # !! ClosedError
```

A non-const method that provably never invalidates borrows can opt out:
`.method<&T::touch>("touch", {}, culebra::wrap_policy::preserves_borrows)`.
Misdeclaring const-ness or this flag is an author-contract violation
(the sol2/pybind11 deal) — the failure mode is a spurious or missed
stale error, never memory-unsafety on the culebra side.

A method parameter can itself name a wrapped class — **an argument
borrows the caller's handle for the call; only a return moves ownership**
(the mirror image of the by-value/`unique_ptr`/`shared_ptr` row above).
`U&`/`const U&` require a live handle of `U`; `U*`/`const U*` additionally
accept `nil`. A non-const `U&`/`U*` bumps that argument's own generation,
exactly as a non-const receiver bumps its own — value, `U&&` and smart-pointer
parameters of a wrapped class have no ownership shape to give up, so they
are not supported (a `.method` declaring one fails to compile):

```cpp
void merge(const Counter& o) { value_ += o.value_; }  // borrows, doesn't bump
void steal(Counter& o) { value_ += o.value_; o.value_ = 0; }  // bumps o
void add_maybe(Counter* o) { if (o) value_ += o->value_; }  // nil accepted
```
```culebra
let c = __Foreign.Counter.new(1)
let o = __Foreign.Counter.new(2)
c.merge(o)      # o unaffected
c.add_maybe(nil)
```

A trailing parameter can default, so a call may omit it: spell that entry
as `{"name", value}` instead of a bare name (a bool, integer, float/double,
string, or `nullptr` for a `U*`/`const U*` parameter — there is no
expression evaluator here, so the default is always a C++-side literal):

```cpp
.method<&Counter::bump_by>("bump_by", {{"n", 1L}})
.method<&Counter::scale>("scale", {"k", {"off", 0L}})
```
```culebra
let c = __Foreign.Counter.new(1)
c.bump_by()          # n defaults to 1
c.bump_by(n: 5)
c.scale(2)           # off defaults to 0
c.scale(2, off: 1)
```

Optional parameters must be a trailing run, and both extensions apply to
`.method`/`.borrowed_method` only — a `.ctor`/`.static_method` parameter is
always required and cannot yet name a wrapped class.

Containers (`std::vector`/`std::map`) and callbacks are not marshalled
automatically. A method that needs one — a `Function` to call back into, or
an arbitrary `Object` to read — declares itself with `.raw_method(name,
thunk, params)` and writes the thunk by hand. The declarative half still
applies, so the method keeps its parameter names, keyword arguments and
defaults; what the thunk owes in exchange is the whole ABI contract the
deduced ones get for free:

- Release self and the arguments (`JitMethodSelf` / `JitMethodArgs`) — the
  ABI is callee-consumes, so without them every call leaks a reference.
- Type-check each argument before using it. `jit_check_args` runs only on
  the deduced path, and `jit_handle_self` reinterprets whatever it is handed,
  so an unchecked scalar in a handle's slot is a wild pointer rather than a
  `TypeError`.
- Resolve the receiver with `wrap_detail::jit_handle_self<T>`, read the
  arguments (a `TAG_UNFILLED` slot is one the caller omitted), and write the
  result through `surface_native_error_at_call_site` into `__ret`, so a C++
  exception from the body becomes a catchable error at the call site instead
  of escaping the process.

`CodeGen.Program.run`'s `natives:` table is the in-tree example.

`tests/wrap_test.sh` runs the end-to-end pipeline check for this
workflow.

## 4. Shared runtime archive layout

All three workflows above ship a static **runtime archive** so that
generated or embedding binaries don't need the LLVM dependency. CMake
emits it as a base archive plus one small archive per heavy feature
(N+1, not a 2^N matrix), when configured with `-DCULEBRA_ENABLE_JIT=ON`:

| Archive | Contents |
|---|---|
| `libculebra_rt.a` | base — everything, but with **weak stubs** for each feature's choke (so nothing it can call reaches BLAS, OpenSSL, zlib, sqlite3 or the regex engine); the subprocess layer, the image decoders (stb_image / stb_truetype), and the cpp-peglib parser generator (`PEG`) link nothing external, so they compile straight into this archive and rely on the namespace-group dead-stripping below rather than a choke of their own — `PEG` is the one of the three that still leaves a fixed ~53 KB of peglib RTTI/vtable metadata behind even unused (see [§1](#tensor-free-and-http-free-binaries)) |
| `libculebra_rt_tensor.a` | strong tensor chokes: the backend one (pulls BLAS / Accelerate) and the elementwise kernels the generic arithmetic path would otherwise reach (see [§1](#tensor-free-and-http-free-binaries)) |
| `libculebra_rt_http.a` | strong http choke (pulls OpenSSL + zlib) |
| `libculebra_rt_http_notls.a` | the same choke with cpp-httplib's TLS off, force-loaded in its place under `--no-tls` (see [§1](#building-http-without-tls)). The one axis with two archives; a link takes exactly one, and the base carries neither |
| `libculebra_rt_compress.a` | strong compress choke (pulls zlib; `to_png` rides it too) |
| `libculebra_rt_sqlite.a` | strong sqlite choke plus the sqlite3 amalgamation |
| `libculebra_rt_regex.a` | strong regex choke (the cpp-regexlib engine, ~320 KB) |
| `libculebra_rt_foreign.a` | the `__Foreign` wrap fixture the foreign-object tests are written against (a static `wrap<T>` registrar, so it needs its own archive) |
| `libculebra_rt_canvas.a` | the raylib window backend (window builds only; the base carries headless stubs) |
| `libculebra_rt_scene.a` | Scene's wrap registrar (pulls raylib; not in the base at all) |
| `libculebra_rt_webview.a` | Webview's wrap registrar (the OS WebView framework; not in the base at all) |
| `libculebra_rt_wrap.a` | `culebra wrap` bindings |

`culebra build` (and extended `ext-culebra build` binaries) always
link the base archive, then **force-load** a feature archive only when
the source AST references its namespace (`Tensor` / `Http` /
`Compress` / `SQLite` / `Regex` / `Canvas` / `Scene` / `Webview`, or a
wrapped namespace), appending that feature's external
libraries (BLAS / OpenSSL / zlib / …) on the same condition. The strong
choke overrides the base's weak stub, so a program that uses none of
these features links none of them.

The stdlib's own dispatch tables are linked the same way, one
namespace at a time. Each namespace's rows — the adapters `Math.abs`
or `Isolate.spawn` resolve to at runtime — and their canonical
signatures form a *group* the base archive defines but never refers
to; the program object carries the list of groups (every stdlib
namespace, with a null entry for each one the source never names),
and only a named group's rows survive dead-stripping, along with
whatever only they reached. The scan is textual over the program and
its spliced preamble, so `let m = Math; m.abs(x)` counts as naming
`Math`, and a lazy module's `_Canvas` counts once `Canvas` pulled the
module in. The isolate transfer code (value serialization, channel
endpoints, `Shared` views) rides the same mechanism: the generic
property and index paths reach a `Shared` view's reader through a hook
the view's constructor installs, never by symbol, so only the
`Shared` / `Isolate` / `Parallel` / `Net` adapters refer to that code
and it goes with their groups. The runtime's own messages are built by
a small formatter of culebra's (`include/base/format.h`) rather than
`std::format`, whose implementation links as one block — a single
argument visitor names the integer, float and string formatters
together, so one reachable call would add 15% to a hello. A program
that writes an interpolation format spec (`"{x:.2f}"`) still links it,
because that spec *is* `std::format`'s mini-language. Together with the
feature axes this is what keeps a `print("hello")` binary near
0.4 MB. A namespace the scan missed does not silently read as `nil`:
reaching it raises an `InternalError` naming the namespace.

These archives are **embedded directly into the `culebra` driver**
via cpp-embedlib — the driver is a single self-contained binary, no
sibling `.a` files need to be installed. They are stored deflated,
which is 10.3 MB of the driver rather than 30.9 MB. On first invocation of
`culebra build`, the required archives are inflated to
`$HOME/.cache/culebra/<fingerprint>/lib*.a`; subsequent invocations
reuse the cache. The fingerprint is a content-hash of the embedded
archives, so a freshly-built `culebra` automatically isolates its cache
from older copies.

The third-party statics a feature links **whole** ride along in the
same store, under their own names: OpenSSL's `libssl.a` /
`libcrypto.a` for `Http`, the vendored `libSDL3.a` / `libraylib.a` for
`Canvas`'s window backend and `Scene`. They are not part of a runtime
archive — an `-l` on the link line would pull only what is referenced,
which is the point — but they cannot be named by path either: the
directory each sat in belongs to the machine that built the driver (a
distro dir, a Homebrew prefix, an MSYS2 tree, a CI runner's dependency
cache), and the link runs on the user's. So each feature's link
fragment names them (`@libssl.a@`), and `culebra build` resolves the
name against the same cache before it hands the command to `cc`. What
stays as a plain `-l` is what every machine has: `-lz`, `-lstdc++`,
`-lpthread`, the OS frameworks. CMake refuses at configure time to bake
a path into a fragment, and `tools/checks/check_aot_link_portability.sh`
reads a real link line back to prove none is there — every other AOT
test runs inside a build tree, where a baked path would happen to
exist.

`culebra build --target=<triple>` is currently host-archive only — the
embedded/cached archive is built for the host. For cross-targets,
supply a matching archive via `--rt-lib=<path>` (per-target auto-build
is on the roadmap; see [§1](#1-standalone-binary-build-culebra-build)
for the manual cross-build steps).
