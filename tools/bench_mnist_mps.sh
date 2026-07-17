#!/usr/bin/env bash
# Run the full benchmarks/mnist/bench.sh (all six implementations,
# including PyTorch MPS) in a dedicated worktree.
#
# PyTorch MPS (Metal) is unreachable from inside the Claude Code
# sandbox — it reports `torch.backends.mps.is_available() == False`
# even when torch is MPS-built and macOS supports it. Run this
# script directly on the host (outside the sandbox) to get real
# GPU numbers.
#
# Usage: tools/bench_mnist_mps.sh [RUNS]
set -euo pipefail

RUNS=${1:-3}
ROOT=$(git rev-parse --show-toplevel)
REPO_NAME=$(basename "$ROOT")
WT="$(dirname "$ROOT")/${REPO_NAME}-bench-mnist-mps"
BRANCH="bench-mnist-mps-$(date +%Y%m%d%H%M%S 2>/dev/null || echo tmp)"

cleanup() {
  cd "$ROOT"
  git worktree remove --force "$WT" 2>/dev/null || true
  git branch -D "$BRANCH" 2>/dev/null || true
}
trap cleanup EXIT

git -C "$ROOT" worktree add "$WT" -b "$BRANCH" master
cd "$WT"
git submodule update --init --recursive

export CCACHE_DIR="${TMPDIR:-/tmp}"
just build

just fetch-mnist
if [[ ! -f benchmarks/mnist/W1.csv ]]; then
  python3.11 benchmarks/mnist/train.py
fi
if [[ ! -f benchmarks/mnist/test_labels.csv ]]; then
  python3.11 benchmarks/mnist/prep_test.py 10000
fi
if [[ ! -f benchmarks/mnist/init_W1.csv ]]; then
  python3.11 benchmarks/mnist/prep_train.py 10000
fi

echo "--- MPS availability ---"
python3.11 -c "import torch; print(torch.__version__); print('available:', torch.backends.mps.is_available()); print('built:', torch.backends.mps.is_built())"
sw_vers

./benchmarks/mnist/bench.sh "$RUNS"
