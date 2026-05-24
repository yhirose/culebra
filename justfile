set shell := ["bash", "-cu"]

# List recipes
default:
    @just --list

# Configure and build with LLVM JIT enabled (Release)
build:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON .. > /dev/null
    cd build && make

# Build without JIT (Release)
build-no-jit:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=OFF .. > /dev/null
    cd build && make

# Clean build directory
clean:
    rm -rf build

# Run the test suite on the tree-walking interpreter. Picks up
# everything under tests/, including any interp-only subdirectory if
# one is later re-introduced.
test: build
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob
    for f in tests/*.cul; do
      ./build/culebra "$f"
    done

# Run the test suite under the LLVM ORC JIT backend. Every test file
# under tests/ is required to pass on both interp and JIT (see
# test-all for the per-file equality check).
test-jit: build
    #!/usr/bin/env bash
    set -euo pipefail
    for f in tests/*.cul; do
      ./build/culebra --jit "$f"
    done

# Run every test file on both backends and assert their stdout is
# identical per file. Catches regressions where one backend diverges
# from the other (e.g. a new feature implemented in one place only).
test-all: build
    #!/usr/bin/env bash
    set -euo pipefail
    failed=()
    for f in tests/*.cul; do
      out_interp=$(./build/culebra "$f")
      out_jit=$(./build/culebra --jit "$f")
      if [[ "$out_interp" != "$out_jit" ]]; then
          echo "interpreter and JIT outputs differ for $f:"
          diff <(printf '%s' "$out_interp") <(printf '%s' "$out_jit") || true
          failed+=("$f")
      fi
    done
    if (( ${#failed[@]} > 0 )); then
      echo "test-all FAIL: ${#failed[@]} file(s) diverge"
      exit 1
    fi
    echo "test-all OK: interpreter and JIT match"

# Run embedding-API smoke tests via ctest (mt_smoke, mi_smoke,
# define_smoke). These C++ tests verify thread_local isolation, the
# RuntimeScope / per-Runtime hooks story, and the culebra::define
# helper end-to-end.
test-embed: build
    cd build && ctest --output-on-failure

# Run every test file through the AOT build path and assert the
# produced binary's stdout matches `culebra --jit`. Catches
# regressions in `culebra build` (object emission, runtime archive,
# bootstrap) that the JIT-only paths don't see.
test-aot: build
    #!/usr/bin/env bash
    set -euo pipefail
    failed=()
    out_dir="${TMPDIR:-/tmp}/culebra-aot-test"
    rm -rf "$out_dir" && mkdir -p "$out_dir"
    for f in tests/*.cul; do
      name=$(basename "$f" .cul)
      bin="$out_dir/$name"
      if ! ./build/culebra build "$f" -o "$bin" 2>"$out_dir/err"; then
        echo "build failed: $f"
        cat "$out_dir/err"
        failed+=("$f")
        continue
      fi
      out_aot=$("$bin")
      out_jit=$(./build/culebra --jit "$f")
      if [[ "$out_aot" != "$out_jit" ]]; then
        echo "AOT and JIT outputs differ for $f:"
        diff <(printf '%s' "$out_aot") <(printf '%s' "$out_jit") || true
        failed+=("$f")
      fi
    done
    if (( ${#failed[@]} > 0 )); then
      echo "test-aot FAIL: ${#failed[@]} file(s) diverge"
      exit 1
    fi
    echo "test-aot OK: AOT binaries match --jit"

# Full verification: differential test (.cul on both backends) +
# embedding C++ smoke tests + AOT build path. Run before every commit.
verify: test-all test-embed test-aot
    @echo "verify OK"

# Microbenchmark regression check: every tests/perf/*.cul on interp
# and JIT, asserts speedup meets the per-bench `# perf: min_speedup N`
# directive declared in the file header. Not part of `verify` because
# runtimes are noisy and machine-dependent.
perf: build
    ./tests/perf/run.sh

# Smoke: run microgpt 5 training steps (no inference) on both backends
# to catch regressions in the JIT value-ownership / special-method
# dispatch paths that the unit tests don't exercise at scale.
smoke-microgpt: build fetch-names
    ./build/culebra       benchmarks/microgpt/microgpt.cul -- 5 0 > /dev/null
    ./build/culebra --jit benchmarks/microgpt/microgpt.cul -- 5 0 > /dev/null
    @echo "smoke-microgpt OK: 5 steps completed on both backends"

# Download Karpathy's names dataset for benchmarks/microgpt.
fetch-names:
    #!/usr/bin/env bash
    set -euo pipefail
    path=benchmarks/microgpt/names.txt
    if [[ -s "$path" ]]; then
      echo "$path already present ($(wc -l < "$path") lines)"
      exit 0
    fi
    mkdir -p benchmarks/microgpt
    url='https://raw.githubusercontent.com/karpathy/makemore/988aa59/names.txt'
    echo "fetching $url"
    curl -fsSL "$url" -o "$path"
    echo "saved $path ($(wc -l < "$path") lines)"

# Download MNIST IDX files for benchmarks/mnist.
fetch-mnist:
    #!/usr/bin/env bash
    set -euo pipefail
    out=benchmarks/mnist/data
    mkdir -p "$out"
    primary='https://storage.googleapis.com/cvdf-datasets/mnist'
    fallback='https://ossci-datasets.s3.amazonaws.com/mnist'
    for f in train-images-idx3-ubyte.gz train-labels-idx1-ubyte.gz \
             t10k-images-idx3-ubyte.gz  t10k-labels-idx1-ubyte.gz; do
      if [[ -s "$out/$f" ]]; then
        echo "$out/$f already present"
        continue
      fi
      echo "fetching $f"
      curl -fsSL "$primary/$f" -o "$out/$f" \
        || curl -fsSL "$fallback/$f" -o "$out/$f"
    done
    echo "MNIST data ready in $out"
