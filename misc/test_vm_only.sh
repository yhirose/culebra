#!/usr/bin/env bash
# A driver whose one engine is the VM (no LLVM: the slim download, or
# build-no-jit): it says so, runs the VM corpus against the frozen expected
# outputs, and its REPL runs.
#
# Usage: misc/test_vm_only.sh <culebra binary>
#
# A link is not the gate: a binary whose one engine is broken can compile and
# link perfectly, and only running a program says so. The corpus is the one
# `just test`'s vm_cases phase uses: it is inside the VM slice (a mismatch is a
# mismatch, not a rejected construct), and compare.sh folds each lane's exit
# code into the comparison.
set -euo pipefail
cd "$(dirname "$0")/.."

bin=${1:?usage: test_vm_only.sh <culebra binary>}
case "$("$bin" --version)" in
  *"(vm)"*) ;;
  *) echo "check failed: --version does not name the VM"; exit 1 ;;
esac
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT_BIN=gtimeout; fi
out="$(${TIMEOUT_BIN:+$TIMEOUT_BIN 300} tools/bench/vm_cases/compare.sh "$bin" --vm 2>&1)" \
  || { printf '%s\n' "$out"; exit 1; }
# The REPL is a separate entry point with its own engine pick, and no corpus
# reaches it. Matched in a case, not `| grep -q`, which would SIGPIPE the REPL.
repl="$(printf 'let x = 6 * 7\nx\n' | ${TIMEOUT_BIN:+$TIMEOUT_BIN 60} "$bin" --vm --shell)"
case "$repl" in
  *42*) ;;
  *) echo "check failed: the REPL does not run on --vm"; exit 1 ;;
esac
echo "$bin: --vm runs the VM corpus and the REPL"
