#!/usr/bin/env bash
# Benchmark every inference and training implementation across $RUNS
# external runs. Each script reports its own in-process cold (cycle 1)
# vs warm-mean (cycles 2..K) on a final `BENCH ...` line; we average
# those across the external runs and print one row per script.
#
# Usage: ./samples/mnist/bench.sh [RUNS]
#
# Requires that `just fetch-mnist`, `train.py`, `prep_test.py`, and
# `prep_train.py` have already been run (W*.csv, b*.csv, init_*.csv,
# train_*.csv, test_*.csv must exist).

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

RUNS=${1:-3}

# bench_cmd LABEL CMD...  — runs CMD $RUNS times, reads its `BENCH ...`
# line, and prints one row with mean(load), mean(cold), mean(warm).
bench_cmd() {
  local label=$1; shift
  local out
  out=$(
    for _ in $(seq "$RUNS"); do
      "$@" 2>&1 | awk '/^BENCH /'
    done
  )
  if [ -z "$out" ]; then
    printf "  %-22s (no BENCH output)\n" "$label"
    return
  fi
  echo "$out" | awk -v l="$label" -v r="$RUNS" '
    {
      for (i = 2; i <= NF; i++) {
        n = split($i, kv, "=")
        if (n == 2) {
          if (kv[1] == "warm" && (kv[2] == "nan" || kv[2] == "NaN")) {
            warm_nan = 1
            continue
          }
          sum[kv[1]] += kv[2] + 0; cnt[kv[1]]++
        }
      }
    }
    END {
      load = (cnt["load"] ? sum["load"] / cnt["load"] : 0)
      cold = (cnt["cold"] ? sum["cold"] / cnt["cold"] : 0)
      acc  = (cnt["accuracy"] ? sum["accuracy"] / cnt["accuracy"] : 0)
      warmS = (warm_nan || cnt["warm"] == 0) \
              ? "    -  " \
              : sprintf("%6.3fs", sum["warm"] / cnt["warm"])
      printf "  %-22s load=%6.3fs  cold=%6.3fs  warm=%s  acc=%.4f  (n=%d)\n",
             l, load, cold, warmS, acc, r
    }
  '
}

echo "=== Inference: 1000 test images, $RUNS external runs ==="
echo "(load = CSV read; cold = cycle 1; warm = mean of cycles 2..K within one process)"
echo
bench_cmd "numpy"             python3.11 samples/mnist/infer_numpy.py
bench_cmd "pure Python"       python3    samples/mnist/infer_pure.py
bench_cmd "PyTorch CPU"       env DEVICE=cpu python3.11 samples/mnist/infer_torch.py
bench_cmd "PyTorch MPS (GPU)" env DEVICE=mps python3.11 samples/mnist/infer_torch.py
bench_cmd "Julia"             julia samples/mnist/infer.jl
bench_cmd "Culebra --jit"     ./build/culebra --jit  samples/mnist/infer.cul
bench_cmd "Culebra Tensor"    ./build/culebra --jit  samples/mnist/infer_tensor.cul

echo
echo "=== Training: 1 epoch, mini-batch SGD, $RUNS external runs ==="
echo "(hand-coded backprop, identical algorithm across implementations)"
echo
bench_cmd "numpy"             python3.11 samples/mnist/train_bench_numpy.py
bench_cmd "pure Python"       python3    samples/mnist/train_bench_pure.py
bench_cmd "PyTorch CPU"       env DEVICE=cpu python3.11 samples/mnist/train_bench_torch.py
bench_cmd "PyTorch MPS (GPU)" env DEVICE=mps python3.11 samples/mnist/train_bench_torch.py
bench_cmd "Julia"             julia samples/mnist/train_bench.jl
bench_cmd "Culebra --jit"     ./build/culebra --jit  samples/mnist/train_bench.cul
bench_cmd "Culebra Tensor"    ./build/culebra --jit  samples/mnist/train_bench_tensor.cul
