#!/usr/bin/env bash
# Benchmark all 4 inference implementations: 5 runs each, report mean.
# Usage: ./samples/mnist/bench.sh
#
# Requires that `just fetch-mnist`, `train.py` and `prep_test.py` have
# already been run (W*.csv, b*.csv, test_*.csv must exist).

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

RUNS=5

bench_cmd() {
  # bench_cmd LABEL CMD...
  local label=$1; shift
  local sum=0 t
  for _ in $(seq "$RUNS"); do
    t=$({ /usr/bin/time -p "$@" > /dev/null; } 2>&1 \
        | awk '/^real/ {print $2}')
    sum=$(awk -v s="$sum" -v x="$t" 'BEGIN { print s + x }')
  done
  awk -v s="$sum" -v r="$RUNS" -v l="$label" \
      'BEGIN { printf "  %-22s mean = %.3fs (n=%d)\n", l, s/r, r }'
}

echo "MNIST MLP inference, 1000 test images, mean of $RUNS runs"
echo "(includes load + inference; Culebra JIT includes ~1s warmup)"
echo
bench_cmd "numpy"           python3.11 samples/mnist/infer_numpy.py
bench_cmd "pure Python"     python3    samples/mnist/infer_pure.py
bench_cmd "Culebra interp"  ./build/culebra        samples/mnist/infer.cul
bench_cmd "Culebra --jit"   ./build/culebra --jit  samples/mnist/infer.cul
