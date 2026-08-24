#!/usr/bin/env bash
# Regression test for `Sys.argv` — that arguments actually reach the program,
# on every backend and on every thread. Neither is observable from the .cul
# sweeps: they run each file with no arguments, so `Sys.argv` is empty there
# and stays empty however badly the plumbing breaks (an AOT binary read its
# arguments correctly for months with nothing checking it, and an isolate read
# an empty array for just as long with nothing noticing).
# Usage: sys_argv_test.sh <path-to-culebra> [--aot]
#   --aot also builds the program with `culebra build` and runs the binary,
#   which needs the runtime archive a full build produces (justfile's AOT lane).
set -u
CULEBRA="${1:?usage: sys_argv_test.sh <culebra-binary> [--aot]}"
WITH_AOT="${2:-}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# An isolate runs the same program on another thread, so it must see the same
# arguments — it builds its own environment, which is where they used to be
# lost.
cat > "$TMP/argv.cul" <<'EOF'
IO.println("argv={Sys.argv}")
IO.println("isolate={Isolate.spawn(|| Sys.argv).join()}")
EOF

check() {  # check <label> <expected> <actual>
  [[ "$3" == "$2" ]] || {
    echo "FAIL $1: expected [$2], got [$3]"; fail=1; }
}

want="argv=['a', 'b c', '--x']
isolate=['a', 'b c', '--x']"

out=$("$CULEBRA" --vm "$TMP/argv.cul" a "b c" --x 2>&1)
check vm "$want" "$out"

out=$("$CULEBRA" --jit "$TMP/argv.cul" a "b c" --x 2>&1)
check jit "$want" "$out"

# No trailing arguments: empty, not the culebra flags or the script path.
none="argv=[]
isolate=[]"
out=$("$CULEBRA" --vm "$TMP/argv.cul" 2>&1)
check "vm (none)" "$none" "$out"
out=$("$CULEBRA" --jit "$TMP/argv.cul" 2>&1)
check "jit (none)" "$none" "$out"

if [[ "$WITH_AOT" == "--aot" ]]; then
  if ! "$CULEBRA" build "$TMP/argv.cul" -o "$TMP/argv" > "$TMP/build.err" 2>&1; then
    echo "FAIL aot: build failed"; cat "$TMP/build.err"; fail=1
  else
    # argv[0] is the program itself and is skipped, as it is for the script path.
    out=$("$TMP/argv" a "b c" --x 2>&1)
    check aot "$want" "$out"
    out=$("$TMP/argv" 2>&1)
    check "aot (none)" "$none" "$out"
  fi
fi

if [[ $fail -eq 0 ]]; then echo "sys_argv_test OK"; exit 0; fi
echo "sys_argv_test FAILED"; exit 1
