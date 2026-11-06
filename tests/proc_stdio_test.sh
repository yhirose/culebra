#!/usr/bin/env bash
# Proc.run's and Proc.spawn's stdin / stdout / stderr "inherit": the child
# reads and writes this program's own streams, in order with what the program
# wrote before it, and the result keeps nothing of them. Seen from outside the
# program, which tests/test_proc.cul cannot do: on the executor and the JIT,
# and with --aot in a binary `culebra build` makes, each the same.
#
# Usage: tests/proc_stdio_test.sh <culebra-binary> [--aot]
set -euo pipefail
CULEBRA="${1:?usage: proc_stdio_test.sh <culebra-binary> [--aot]}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/inherit.cul" <<'EOF'
# Without a newline, which println would have written out with.
print("before ")
let r = Proc.run(["sh", "-c", "echo child out; echo child err >&2; cat"],
                 stdin: "inherit", stdout: "inherit", stderr: "inherit")
println("after: {r.ok} [{r.stdout}] [{r.stderr}]")
let h = Proc.spawn(["sh", "-c", "echo spawned out"], stdout: "inherit")
let w = h.wait()
println("waited: {w.ok} [{w.stdout}]")
EOF

lanes=(vm jit)
if [[ "${2:-}" == "--aot" ]]; then
  "$CULEBRA" build "$TMP/inherit.cul" -o "$TMP/inherit" > /dev/null || {
    echo "FAIL aot: culebra build"; exit 1; }
  lanes+=(aot)
fi

for lane in "${lanes[@]}"; do
  case $lane in
    vm) run=("$CULEBRA" --vm "$TMP/inherit.cul") ;;
    jit) run=("$CULEBRA" --jit "$TMP/inherit.cul") ;;
    aot) run=("$TMP/inherit") ;;
  esac
  printf 'typed in\n' | "${run[@]}" > "$TMP/$lane.out" 2> "$TMP/$lane.err" || {
    echo "FAIL $lane: exit status $?"; fail=1; continue; }
  want=$'before child out\ntyped in\nafter: true [] []\nspawned out\nwaited: true []'
  [[ "$(cat "$TMP/$lane.out")" == "$want" ]] || {
    echo "FAIL $lane stdout:"; cat "$TMP/$lane.out"; fail=1; }
  [[ "$(cat "$TMP/$lane.err")" == "child err" ]] || {
    echo "FAIL $lane stderr:"; cat "$TMP/$lane.err"; fail=1; }
done

[[ $fail == 0 ]] && echo "proc_stdio_test OK"
exit $fail
