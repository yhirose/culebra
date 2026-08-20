#!/usr/bin/env bash
# Microbenchmark regression suite.
#
# For every tests/perf/*.cul, run it on the tree-walking interpreter and
# on the LLVM ORC JIT, then assert speedup (interp / jit) is at least
# the per-bench threshold declared in the .cul file via:
#
#   # perf: min_speedup 8.0
#
# (placed in the comment header). The per-bench threshold lets fib /
# sum aim high while allocator-heavy benches like object_churn declare
# a sub-1× threshold honestly — JIT startup overhead dominates tiny
# benches.
#
# Size a new bench so the interp run lands well above 0.1s: `time -p`
# reports hundredths, so below that the ratio measures the timer.
#
# Usage:
#   tests/perf/run.sh
#   CULEBRA=./build/culebra tests/perf/run.sh
set -u

BIN=${CULEBRA:-./build/culebra}

if [[ ! -x "$BIN" ]]; then
    echo "perf: culebra binary not found at $BIN — run 'just build' first" >&2
    exit 2
fi

shopt -s nullglob
files=(tests/perf/*.cul)
if (( ${#files[@]} == 0 )); then
    echo "perf: no tests/perf/*.cul files found" >&2
    exit 2
fi

failed=()
printf "%-22s %10s %10s %10s %10s\n" "benchmark" "interp" "jit" "speedup" "threshold"
printf "%-22s %10s %10s %10s %10s\n" "---------" "------" "---" "-------" "---------"
for f in "${files[@]}"; do
    name=$(basename "$f" .cul)
    threshold=$(awk '
        /^[^#]/ { exit }
        /^# perf: min_speedup [0-9.]+/ { print $4; exit }
    ' "$f")
    if [[ -z "$threshold" ]]; then
        echo "perf: $f missing '# perf: min_speedup N' directive in header" >&2
        exit 2
    fi
    t_interp=$({ /usr/bin/time -p "$BIN" --tree "$f" > /dev/null; } 2>&1 | awk '/^real/ {print $2}')
    t_jit=$(   { /usr/bin/time -p "$BIN" --jit "$f" > /dev/null; } 2>&1 | awk '/^real/ {print $2}')
    speedup=$(awk -v i="$t_interp" -v j="$t_jit" 'BEGIN {
        if (j+0 == 0) print "inf"; else printf "%.2f", i/j
    }')
    printf "%-22s %10s %10s %10s %10s\n" "$name" "${t_interp}s" "${t_jit}s" "${speedup}x" "${threshold}x"
    ok=$(awk -v s="$speedup" -v m="$threshold" 'BEGIN {
        if (s == "inf") { print "ok"; exit }
        print (s+0 >= m+0) ? "ok" : "FAIL"
    }')
    if [[ "$ok" != "ok" ]]; then
        failed+=("$name (${speedup}x < ${threshold}x)")
    fi
done

if (( ${#failed[@]} > 0 )); then
    echo
    echo "perf FAIL: ${#failed[@]} bench(es) below their declared threshold:"
    for entry in "${failed[@]}"; do echo "  - $entry"; done
    exit 1
fi
echo
echo "perf OK: all benchmarks meet their declared threshold"
