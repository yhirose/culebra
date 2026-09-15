#!/usr/bin/env bash
# Culebra Tensor against PyTorch on the same machine, three workloads:
#
#   gpt    one GPT training step (forward, backward, Adam) — gpt_train.{cul,py}
#   ops    single ops, one shape each — ops.{cul,py}
#   block  one transformer block forward — ../mnist/train_bench_transformer*
#
# Both sides run in the same round, alternating which goes first, and each
# cell reports the minimum over rounds: a GPU's timing drifts with heat and
# with whatever else the machine is doing, and rounds taken back to back see
# the same drift on both sides. A culebra/torch ratio below 1 means Culebra
# is faster. A GPT row whose loss did not fall is flagged — a fast step that
# does not learn is not a result.
#
# Usage:
#   benchmarks/tensor/run.sh [--rounds N] [--device gpu|cpu] [--only gpt,ops,block]
#                            [--configs small,medium,large]
#   CULEBRA=./build-dev/culebra benchmarks/tensor/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

BIN=${CULEBRA:-./build/culebra}
PY=${PYTHON:-python3}
ROUNDS=5
DEVICE=gpu
ONLY=gpt,ops,block
CONFIGS=
while [ $# -gt 0 ]; do
  case $1 in
    --rounds) ROUNDS=$2; shift 2 ;;
    --device) DEVICE=$2; shift 2 ;;
    --only) ONLY=$2; shift 2 ;;
    --configs) CONFIGS=$2; shift 2 ;;
    *) echo "run.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ ! -x "$BIN" ]; then
  echo "run.sh: culebra binary not found at $BIN -- run 'just build' first" >&2
  exit 2
fi

# The CPU runs medium and large in minutes a round; default it to small.
if [ -z "$CONFIGS" ]; then
  if [ "$DEVICE" = cpu ]; then CONFIGS=small; else CONFIGS=small,medium,large; fi
fi

# torch's name for the device: cuda, or mps on Apple silicon.
TORCH_DEVICE=cpu
HAVE_TORCH=1
if ! "$PY" -c 'import torch' 2>/dev/null; then
  HAVE_TORCH=0
elif [ "$DEVICE" = gpu ]; then
  TORCH_DEVICE=$("$PY" -c 'import torch; print("cuda" if torch.cuda.is_available() else "mps" if torch.backends.mps.is_available() else "")')
  [ -n "$TORCH_DEVICE" ] || HAVE_TORCH=0
fi

echo "culebra: $("$BIN" --version)"
if [ "$HAVE_TORCH" = 1 ]; then
  echo "torch:   $("$PY" -c 'import torch; print(torch.__version__)') on $TORCH_DEVICE"
else
  echo "torch:   not available for --device $DEVICE (culebra rows only)"
fi
if command -v nvidia-smi >/dev/null; then
  echo "gpu:     $(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader | head -1)"
fi
echo "rounds:  $ROUNDS"
echo

RAW=$(mktemp)
trap 'rm -f "$RAW"' EXIT

wants() { case ",$ONLY," in *",$1,"*) return 0 ;; esac; return 1; }

# run_pair ROUND CULEBRA_CMD... -- TORCH_CMD...
# Odd rounds start with Culebra, even rounds with torch.
run_pair() {
  local round=$1; shift
  local cul=() tor=()
  while [ "$1" != -- ]; do cul+=("$1"); shift; done
  shift
  tor=("$@")
  local first=cul
  [ $((round % 2)) -eq 0 ] && first=torch
  for side in $first $([ $first = cul ] && echo torch || echo cul); do
    if [ $side = cul ]; then
      "${cul[@]}" 2>&1 | awk '/^BENCH /' >> "$RAW" || true
    elif [ "$HAVE_TORCH" = 1 ]; then
      "${tor[@]}" 2>&1 | awk '/^BENCH /' >> "$RAW" || true
    fi
  done
}

for r in $(seq "$ROUNDS"); do
  echo "round $r / $ROUNDS" >&2
  if wants gpt; then
    for c in ${CONFIGS//,/ }; do
      run_pair "$r" "$BIN" --jit benchmarks/tensor/gpt_train.cul "$DEVICE" "$c" -- \
        "$PY" benchmarks/tensor/gpt_train.py "$TORCH_DEVICE" "$c"
    done
  fi
  if wants ops; then
    run_pair "$r" "$BIN" --jit benchmarks/tensor/ops.cul "$DEVICE" -- \
      "$PY" benchmarks/tensor/ops.py "$TORCH_DEVICE"
  fi
  if wants block; then
    run_pair "$r" env RUNS=1 "$BIN" --jit benchmarks/mnist/train_bench_transformer.cul "$DEVICE" -- \
      env RUNS=1 DEVICE="$TORCH_DEVICE" "$PY" benchmarks/mnist/train_bench_transformer_torch.py
  fi
done

"$PY" - "$RAW" <<'EOF'
import collections
import re
import sys

best = {}
losses = collections.defaultdict(list)
for line in open(sys.argv[1]):
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    label = kv["label"]
    side = "culebra" if label.startswith("culebra") else "torch"
    if "gpt_train" in label:
        key, unit, value = ("gpt", kv["config"] + f" ({kv['tokens']} tokens)"), "ms/step", kv["ms_per_step"]
        losses[(side, key)].append(float(kv["loss"]) < float(kv["loss0"]))
    elif label.endswith("_op"):
        key, unit, value = ("ops", kv["op"]), "µs", kv["us"]
    else:
        key, unit, value = ("block", f"seq={kv['seq']} d={kv['d']}"), "ms/iter", kv["ms_per_iter"]
    cell = (side, key)
    best[cell] = min(best.get(cell, float("inf")), float(value))
    best[("unit", key)] = unit

titles = {"gpt": "GPT training step", "ops": "single ops", "block": "transformer block forward"}
for suite in ("gpt", "ops", "block"):
    keys = sorted({k for (s, k) in best if s != "unit" and k[0] == suite}, key=str)
    if not keys:
        continue
    unit = best[("unit", keys[0])]
    print(f"== {titles[suite]} ({unit}, min over rounds) ==")
    print(f"{'':44} {'culebra':>10} {'torch':>10} {'ratio':>7}")
    for key in keys:
        c = best.get(("culebra", key))
        t = best.get(("torch", key))
        ratio = f"{c / t:7.2f}" if c is not None and t is not None else "      -"
        flags = ""
        for side in ("culebra", "torch"):
            if not all(losses.get((side, key), [True])):
                flags += f"  !! {side} loss did not fall"
        cs = f"{c:10.3f}" if c is not None else "         -"
        ts = f"{t:10.3f}" if t is not None else "         -"
        print(f"{key[1]:44} {cs} {ts} {ratio}{flags}")
    print()
EOF
