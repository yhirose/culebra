#!/usr/bin/env bash
# IO.stdin() reader handle — .lines() streams lines (trailing newline
# stripped, including a final unterminated line), .read() slurps the rest,
# .read(n) takes up to n bytes off the same shared buffer. Driven over a real
# pipe across vm, JIT, and AOT.
#
# This can't be a plain tests/*.cul sweep test: the sweep feeds no stdin, and
# in an AOT binary `Sys.executable` is the program itself (so a child-driver
# trick would re-exec the test, not a culebra interpreter). A shell driver
# that pipes input into each backend is the portable way — same pattern as
# signal_test.sh's check_stdin.
#
# Usage: io_stdin_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: io_stdin_test.sh <culebra>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/lines.cul" <<'EOF'
let mut a = []
for l in IO.stdin().lines() { a.push("[" + l + "]") }
IO.print(a.join(","))
EOF

cat > "$TMP/readn.cul" <<'EOF'
let s = IO.stdin()
IO.print(s.read(4) + "|" + s.read())
EOF

fail=0
# check <desc> <input> <want> -- <command...>
check() {
  local desc="$1" input="$2" want="$3"; shift 3
  [ "$1" = "--" ] && shift
  local got
  got="$(printf '%s' "$input" | "$@")"
  if [ "$got" != "$want" ]; then
    echo "FAIL [$desc]: want [$want] got [$got]" >&2
    fail=1
  else
    echo "ok   [$desc]"
  fi
}

# lines(): final 'gamma' has no trailing newline → still yielded once.
LINES_IN=$'alpha\nbeta\ngamma'
LINES_OUT='[alpha],[beta],[gamma]'

# --- VM executor ---
check "vm lines" "$LINES_IN" "$LINES_OUT" -- "$CULEBRA" --vm "$TMP/lines.cul"
check "vm readn" "abcdefgh"  "abcd|efgh"  -- "$CULEBRA" --vm "$TMP/readn.cul"

# --- JIT ---
check "jit lines" "$LINES_IN" "$LINES_OUT" -- "$CULEBRA" --jit "$TMP/lines.cul"
check "jit readn" "abcdefgh"  "abcd|efgh"  -- "$CULEBRA" --jit "$TMP/readn.cul"

# --- AOT (skip if this build can't produce binaries) ---
if "$CULEBRA" build "$TMP/lines.cul" -o "$TMP/lines_aot" >/dev/null 2>&1 \
   && "$CULEBRA" build "$TMP/readn.cul" -o "$TMP/readn_aot" >/dev/null 2>&1; then
  check "aot lines" "$LINES_IN" "$LINES_OUT" -- "$TMP/lines_aot"
  check "aot readn" "abcdefgh"  "abcd|efgh"  -- "$TMP/readn_aot"
else
  echo "skip [aot] (this build can't produce binaries)"
fi

if [ "$fail" -ne 0 ]; then
  echo "io_stdin_test FAIL" >&2
  exit 1
fi
echo "io_stdin_test OK"
