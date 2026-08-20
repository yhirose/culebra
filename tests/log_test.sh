#!/usr/bin/env bash
# Log writes structured records to stderr; this checks the actual content
# across interp / JIT / AOT. Timestamps vary per run, so it asserts substrings,
# not exact lines. (A tests/*.cul sweep test only sees stdout, where logs never
# appear, so it can't verify the records.)
#
# Usage: log_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: log_test.sh <culebra>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/w.cul" <<'EOF'
# default text format first: level, message, and k=v fields.
Log.info("textline", {n: 1})
# then JSON, exercising filtering / set_level / child binding.
Log.set_format("json")
Log.info("hello", {n: 1})
Log.debug("nope")
Log.set_level("debug")
Log.debug("yes", {k: "v"})
Log.with({req: "r1"}).warn("child")
IO.inspect("done")
EOF

fail=0
has()     { case "$2" in *"$3"*) ;; *) echo "FAIL [$1]: missing <$3>" >&2; fail=1;; esac; }
lacks()   { case "$2" in *"$3"*) echo "FAIL [$1]: unexpected <$3>" >&2; fail=1;; esac; }

run_checks() {  # run_checks <label> <stderr-text> <stdout-text>
  local l="$1" err="$2" out="$3"
  has   "$l stdout"        "$out" "done"            # program output untouched
  # text format (default): "<level> <msg>" and "key=value" field rendering.
  has   "$l text-line"     "$err" "info textline"
  has   "$l text-field"    "$err" "n=1"
  # json format.
  has   "$l info+fields"   "$err" '"msg":"hello"'
  has   "$l fields"        "$err" '"n":1'
  has   "$l level"         "$err" '"level":"info"'
  lacks "$l debug-filtered" "$err" '"msg":"nope"'   # below threshold → dropped
  has   "$l debug-after"   "$err" '"msg":"yes"'     # after set_level("debug")
  has   "$l child-bound"   "$err" '"req":"r1"'      # with() field on the record
}

# interp / JIT: pass the script; AOT: run the prebuilt binary.
ERR="$("$CULEBRA" --tree "$TMP/w.cul" 2>"$TMP/err")"; run_checks interp "$(cat "$TMP/err")" "$ERR"
ERR="$("$CULEBRA" --jit "$TMP/w.cul" 2>"$TMP/err")"; run_checks jit "$(cat "$TMP/err")" "$ERR"

if "$CULEBRA" build "$TMP/w.cul" -o "$TMP/w_aot" >/dev/null 2>&1; then
  ERR="$("$TMP/w_aot" 2>"$TMP/err")"; run_checks aot "$(cat "$TMP/err")" "$ERR"
else
  echo "skip [aot] (this build can't produce binaries)"
fi

if [ "$fail" -ne 0 ]; then
  echo "log_test FAIL" >&2
  exit 1
fi
echo "log_test OK"
