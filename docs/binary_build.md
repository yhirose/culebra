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
| `--target=<triple>` | Cross-compile for the given LLVM triple. |
| `--sysroot=<path>` | Forwarded to `cc` as `--sysroot=`. |
| `--rt-lib=<path>` | Override the runtime archive path (required for cross-compile). |

### Environment overrides

| Variable | Effect |
|---|---|
| `CULEBRA_CC` | Linker driver (default `cc`). |
| `CULEBRA_RT_LIB` | Override the host runtime archive (`libculebra_rt.a`). |
| `CULEBRA_RT_NO_TENSOR_LIB` | Override the host tensor-free archive. |
| `CULEBRA_VERBOSE=1` | Print the object path and full link command. |
| `TMPDIR` | Directory for the intermediate object file (default `/tmp`). |

## Tensor-free binaries

`culebra build` scans the AST for any bare `Tensor` identifier. If
none is found, the link picks `libculebra_rt_no_tensor.a` — a second
runtime archive whose tensor entry points (`culebra_runtime_tensor_*`,
the `TAG_TENSOR` case in value release / to-string, etc.) are
replaced with abort-on-call stubs. Because the static reachability
chain from `culebra_runtime_num_add` to `cblas_*` is broken, the
binary also drops the `Accelerate` / BLAS dependency.

Verify with `otool -L` (macOS) / `ldd` (Linux):

```sh
$ culebra build samples/fizzbuzz.cul -o /tmp/fizzbuzz
$ otool -L /tmp/fizzbuzz
/tmp/fizzbuzz:
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
      -DCULEBRA_ENABLE_JIT=ON \
      -DCULEBRA_ENABLE_TENSOR=OFF
cmake --build build-linux-x86_64 --target culebra_rt_no_tensor

# 2. Cross-compile the program.
culebra build samples/fizzbuzz.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt_no_tensor.a \
  -o ./fizzbuzz-linux

# 3. Verify (on a Linux host or via emulation).
file ./fizzbuzz-linux
# ELF 64-bit LSB executable, x86-64, ...
```
