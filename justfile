set shell := ["bash", "-cu"]

# List recipes
default:
    @just --list

# Configure and build with LLVM JIT enabled (Release)
[group("build")]
build:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON .. > /dev/null
    cd build && make

# Build without JIT (Release)
[group("build")]
build-no-jit:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=OFF .. > /dev/null
    cd build && make

# Clean build directory
[group("build")]
clean:
    rm -rf build

# Run the test suite. BACKEND selects what to run:
#   all     (default) — interp vs JIT diff + AOT vs JIT diff + C++
#                       embedding smoke. Run before every commit.
#   interp            — every tests/*.cul on the tree-walking interp.
#   jit               — every tests/*.cul on the LLVM ORC JIT.
#   aot               — every tests/*.cul through `culebra build`,
#                       assert stdout matches `--jit`.
#   embed             — C++ ctest (mt_smoke, mi_smoke, define_smoke).
# The single-backend modes are for focused debugging.
[doc("Run tests. BACKEND=all|interp|jit|aot|embed (default: all)")]
[group("test")]
test BACKEND='all': build
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob

    run_interp() {
        for f in tests/*.cul; do
          ./build/culebra "$f"
        done
    }

    run_jit() {
        for f in tests/*.cul; do
          ./build/culebra --jit "$f"
        done
    }

    run_diff_interp_jit() {
        local failed=()
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
          echo "test (interp vs jit) FAIL: ${#failed[@]} file(s) diverge"
          exit 1
        fi
        echo "test (interp vs jit) OK"
    }

    run_aot() {
        local failed=()
        local out_dir="${TMPDIR:-/tmp}/culebra-aot-test"
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
          echo "test aot FAIL: ${#failed[@]} file(s) diverge"
          exit 1
        fi
        echo "test aot OK: AOT binaries match --jit"
    }

    run_embed() {
        (cd build && ctest --output-on-failure)
    }

    # Exercises `culebra test`-only ambient bindings (matchers, DI,
    # @parametrize). The subdir layout keeps these out of the
    # `tests/*.cul` glob that direct interp/JIT runs use.
    run_culebra_test_self() {
        ./build/culebra test tests/culebra_test_self/ > /dev/null
        # Sanity-check the JSON reporter: final line is a run_end with
        # failed=0 and errored_files=0; every other line begins with
        # one of the documented event tags.
        local last
        last=$(./build/culebra test --reporter json tests/culebra_test_self/ | tail -1)
        case "$last" in
            *'"event":"run_end"'*'"failed":0'*'"errored_files":0'*) ;;
            *) echo "json reporter: bad run_end line: $last" >&2; exit 1 ;;
        esac
    }

    case "{{BACKEND}}" in
      # Order: cheap tests first, then AOT (slowest + most env-sensitive,
      # so a failure there shouldn't mask matcher regressions).
      all)    run_diff_interp_jit; run_embed; run_culebra_test_self; run_aot; echo "test OK" ;;
      interp) run_interp ;;
      jit)    run_jit ;;
      aot)    run_aot ;;
      embed)  run_embed ;;
      *) echo "test: unknown backend '{{BACKEND}}' (expected: all|interp|jit|aot|embed)" >&2; exit 2 ;;
    esac

# Microbenchmark regression check: every tests/perf/*.cul on interp
# and JIT, asserts speedup meets the per-bench `# perf: min_speedup N`
# directive declared in the file header. Not part of `just test`
# because runtimes are noisy and machine-dependent.
[doc("Microbench regression check (per-bench thresholds in tests/perf/*.cul)")]
[group("test")]
perf: build
    ./tests/perf/run.sh

# Smoke: run microgpt 5 training steps (no inference) on both backends
# to catch regressions in the JIT value-ownership / special-method
# dispatch paths that the unit tests don't exercise at scale.
[doc("Run microgpt 5 training steps on both backends (large-scale JIT smoke)")]
[group("bench")]
smoke-microgpt: build fetch-names
    ./build/culebra       benchmarks/microgpt/microgpt.cul -- 5 0 > /dev/null
    ./build/culebra --jit benchmarks/microgpt/microgpt.cul -- 5 0 > /dev/null
    @echo "smoke-microgpt OK: 5 steps completed on both backends"

# Download Karpathy's names dataset for benchmarks/microgpt.
[group("bench")]
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
[group("bench")]
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
