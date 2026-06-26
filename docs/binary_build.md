# Culebra Binary Build

`culebra build` compiles a `.cul` source into a standalone executable
via LLVM AOT codegen + system `cc` for the link step. No LLVM runtime
is embedded in the produced binary — the dependency surface is just
`libc++` / `libSystem` (macOS) or `libstdc++` / `libc` (Linux), plus
`Accelerate` / BLAS when the program references `Tensor`.

## Usage

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
| `--keep-symbols` | Keep local symbols in the output. By default the link discards them (`-Wl,-x`), which is ~30% smaller. Use for debugging. |
| `--target=<triple>` | Cross-compile for the given LLVM triple. |
| `--sysroot=<path>` | Forwarded to `cc` as `--sysroot=`. |
| `--rt-lib=<path>` | Override the runtime archive path (required for cross-compile). |

### Environment overrides

| Variable | Effect |
|---|---|
| `CULEBRA_VERBOSE=1` | Print the object path and full link command. |
| `TMPDIR` | Directory for the intermediate object file (default `/tmp`). |

### Runtime archive distribution

The runtime archives — a base `libculebra_rt.a` plus one small archive
per heavy feature (`libculebra_rt_tensor.a`, `libculebra_rt_http.a`,
`libculebra_rt_compress.a`, and `libculebra_rt_wrap.a` for `culebra
wrap` bindings) — are **embedded directly into the `culebra` driver**
via cpp-embedlib. The driver is a single self-contained binary — no
sibling `.a` files need to be installed. On first invocation of
`culebra build`, the required archives are materialized to
`$HOME/.cache/culebra/<fingerprint>/lib*.a`. Subsequent invocations
reuse the cached files. The fingerprint is a content-hash of the
embedded archives, so a freshly-built `culebra` automatically isolates
its cache from older copies.

The base archive carries **weak-symbol stubs** for the tensor / http /
compress chokes, so on its own it references no BLAS, OpenSSL, or zlib.
`culebra build` always links the base, then **force-loads** a feature
archive only when the source uses it — the strong choke in that archive
overrides the base's weak stub. This is N+1 archives, not a 2^N matrix.

## Tensor-free binaries

`culebra build` scans the AST for any bare `Tensor` identifier. Only
when one is found does it force-load `libculebra_rt_tensor.a` (the
strong tensor choke — `culebra_runtime_tensor_*`, the `TAG_TENSOR`
cases) and append the `Accelerate` / BLAS link. With no `Tensor`, the
base archive's weak tensor stub breaks the static reachability chain
from `culebra_runtime_num_add` to `cblas_*`, so the binary references
no BLAS symbol and drops that dependency.

## Http-free binaries

The same applies, independently, to `Http`. Only when the AST has a
bare `Http` identifier does the link force-load `libculebra_rt_http.a`
(the strong http choke, which `#include`s `httplib.h`) and append
OpenSSL + zlib. The runtime's http helpers are `__attribute__((used))`
for the in-process JIT, which pins them past `-dead_strip` /
`--gc-sections` — but they live in this separate archive, so a non-Http
program never force-loads it and references no OpenSSL/zlib symbol.
This is the larger win: a non-Http binary is ~5 MB versus ~9.5 MB for
an Http one (OpenSSL is statically linked). The axes are independent, so
a program using neither Tensor nor Http links only the base archive and
avoids both BLAS and OpenSSL.

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

## Symbol stripping

The embedded runtime archive carries thousands of local symbols
(`GCC_except_table*`, template and string instantiations) that are
useless in a distributed executable. The link discards them by default
(`-Wl,-x` — understood by ld64, GNU ld and lld alike), which keeps the
global/dynamic symbols the loader needs while shrinking the binary by
~30% (e.g. a Term/IO program drops from ~7.6 MB to ~5.3 MB). Pass
`--keep-symbols` to retain them for debugging.

## Cross-compilation

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

### Phase E MVP limitations

- The runtime is not bundled for any cross target; the user produces
  it via their own CMake / toolchain.
- `--target=<triple>` with Tensor is rejected: BLAS link flags are
  host-specific and would mis-link the target binary. Drop `Tensor`
  references or wait for a future phase.

### Example (Linux x86_64 from macOS host)

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
