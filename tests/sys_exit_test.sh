#!/usr/bin/env bash
# How Sys.exit ends the process (include/base/exit_hooks.h): the exit hooks,
# then the standard streams, then _Exit, with every static left as it is. A
# .cul sweep cannot see how a process ends.
# Usage: sys_exit_test.sh <path-to-culebra> [--aot]
#   --aot also builds the program with `culebra build` and runs the binary,
#   which needs the runtime archive a full build produces (justfile's AOT lane).
set -u
CULEBRA="${1:?usage: sys_exit_test.sh <culebra-binary> [--aot]}"
WITH_AOT="${2:-}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# One program, its case named by its first argument, so AOT builds it once.
#   race:    eight isolates and the main thread call Sys.exit at the same
#            instant: the process ends once, with one of their codes.
#   file:    what a file left open had buffered is written.
#   profile: TL_PROFILE=1's table, which tensorlib prints from an atexit
#            handler, is printed once.
cat > "$TMP/exits.cul" <<'EOF'
let what = Sys.argv[0]
if what == "race" {
  let at = Time.monotonic() + 0.02
  let hs = range(1, 9).map(|i| Isolate.spawn(fn (k, t) {
    while Time.monotonic() < t {}
    Sys.exit(k)
  }, i, at)).collect()
  while Time.monotonic() < at {}
  Sys.exit(9)
} else if what == "file" {
  File.open(Sys.argv[1], "w").write("kept")
  Sys.exit(3)
} else {
  println(Tensor.from([[1.0, 2.0]]).sum(0).sum(0).item())
  Sys.exit(4)
}
EOF

check() { # <label> <command...>
  for _ in 1 2 3; do
    timeout 20 "${@:2}" race > "$TMP/out" 2>&1
    rc=$?
    if (( rc < 1 || rc > 9 )) || [[ -s "$TMP/out" ]]; then
      echo "FAIL $1: Sys.exit from 9 threads at once: exit $rc, expected 1..9"
      head -3 "$TMP/out"
      fail=1
      break
    fi
  done

  rm -f "$TMP/written"
  "${@:2}" file "$TMP/written" > /dev/null 2>&1
  rc=$?
  if (( rc != 3 )) || [[ "$(cat "$TMP/written" 2>/dev/null)" != kept ]]; then
    echo "FAIL $1: an open file at Sys.exit: exit $rc," \
      "wrote [$(cat "$TMP/written" 2>/dev/null)]"
    fail=1
  fi

  out=$(TL_PROFILE=1 "${@:2}" profile 2>&1)
  rc=$?
  [[ "$out" == *NameError* ]] && return  # a build without Tensor
  if (( rc != 4 )) || [[ $(grep -c '^tl profile:' <<< "$out") != 1 ]]; then
    echo "FAIL $1: TL_PROFILE=1 at Sys.exit: exit $rc, expected one table:"
    head -3 <<< "$out"
    fail=1
  fi
}

check vm "$CULEBRA" "$TMP/exits.cul"
check jit "$CULEBRA" --jit "$TMP/exits.cul"
if [[ "$WITH_AOT" == "--aot" ]]; then
  if "$CULEBRA" build "$TMP/exits.cul" -o "$TMP/exits" > "$TMP/build.err" 2>&1; then
    check aot "$TMP/exits"
  else
    echo "FAIL aot: build failed"; cat "$TMP/build.err"; fail=1
  fi
fi

if [[ $fail -eq 0 ]]; then echo "sys_exit_test OK"; exit 0; fi
echo "sys_exit_test FAILED"; exit 1
