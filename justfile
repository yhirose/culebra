set shell := ["bash", "-cu"]

# List recipes
default:
    @just --list

# Configure and build with LLVM JIT enabled (Release)
build:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)" .. > /dev/null
    cd build && make

# Build without JIT (Release)
build-no-jit:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=OFF .. > /dev/null
    cd build && make

# Clean build directory
clean:
    rm -rf build

# Run the test suite (samples/test.cul)
test: build
    ./build/culebra samples/test.cul

# Run shadow-prohibition negative tests (expected to fail with a
# shadow error message, on both interpreter and JIT).
test-shadow-errors: build
    ./scripts/test_shadow_errors.sh

# Quick fib(33) benchmark comparing interpreter vs JIT
bench: build
    #!/usr/bin/env bash
    echo "== Tree Interpreter =="
    time ./build/culebra benchmarks/fib.cul > /dev/null
    echo ""
    echo "== LLVM ORC JIT =="
    time ./build/culebra --jit benchmarks/fib.cul > /dev/null

# Full benchmark suite: run every benchmarks/*.cul on interp & JIT, print table
bench-all: build
    #!/usr/bin/env bash
    set -u
    printf "%-22s %10s %10s %10s\n" "benchmark" "interp" "jit" "speedup"
    printf "%-22s %10s %10s %10s\n" "---------" "------" "---" "-------"
    for f in benchmarks/*.cul; do
        name=$(basename "$f" .cul)
        t_interp=$(
            { /usr/bin/time -p ./build/culebra "$f" > /dev/null; } 2>&1 \
              | awk '/^real/ {print $2}'
        )
        t_jit=$(
            { /usr/bin/time -p ./build/culebra --jit "$f" > /dev/null; } 2>&1 \
              | awk '/^real/ {print $2}'
        )
        speedup=$(awk -v i="$t_interp" -v j="$t_jit" 'BEGIN {
            if (j+0 == 0) print "n/a"; else printf "%.1fx\n", i/j
        }')
        printf "%-22s %10s %10s %10s\n" "$name" "${t_interp}s" "${t_jit}s" "$speedup"
    done
