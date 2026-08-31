#!/usr/bin/env bash
# examples/pl0/pl0_codegen.cul against examples/pl0/pl0.cul: two independent
# implementations of the same language (one interprets the AST directly, the
# other compiles it to Core-IR and runs cpp-vmlib's bytecode executor), each
# the other's oracle. Every examples/pl0/samples/*.pas must produce identical
# stdout from both, across the executor, --jit and an AOT-built binary.
#
# Can't be a plain tests/*.cul sweep test: it has to invoke two whole
# top-level scripts as subprocesses (one of them needs stdin), and needs to
# `culebra build` for the AOT leg — same reason io_stdin_test.sh/
# sys_argv_test.sh are shell-driven.
#
# Usage: pl0_codegen_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: pl0_codegen_test.sh <culebra>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PL0="$ROOT/examples/pl0/pl0.cul"
PL0_CODEGEN="$ROOT/examples/pl0/pl0_codegen.cul"
SAMPLES="$ROOT/examples/pl0/samples"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

# run <desc> <engine-flag> <sample> [stdin-file]
run() {
  local desc="$1" flag="$2" sample="$3" stdin_file="${4:-}"
  local want got
  if [ -n "$stdin_file" ]; then
    want="$("$CULEBRA" ${flag:+$flag} "$PL0" "$SAMPLES/$sample" <"$stdin_file" 2>&1)"
    got="$("$CULEBRA" ${flag:+$flag} "$PL0_CODEGEN" "$SAMPLES/$sample" <"$stdin_file" 2>&1)"
  else
    want="$("$CULEBRA" ${flag:+$flag} "$PL0" "$SAMPLES/$sample" 2>&1)"
    got="$("$CULEBRA" ${flag:+$flag} "$PL0_CODEGEN" "$SAMPLES/$sample" 2>&1)"
  fi
  if [ "$got" != "$want" ]; then
    echo "FAIL [$desc]: pl0_codegen differs from pl0.cul" >&2
    echo "--- pl0.cul ---" >&2; echo "$want" >&2
    echo "--- pl0_codegen.cul ---" >&2; echo "$got" >&2
    fail=1
  else
    echo "ok   [$desc]"
  fi
}

for sample in square.pas gcd.pas fib.pas nested.pas odd.pas unary.pas; do
  run "vm  $sample"  "--vm"  "$sample"
  run "jit $sample"  "--jit" "$sample"
done
run "vm  read.pas"  "--vm"  "read.pas"  "$SAMPLES/read.stdin"
run "jit read.pas"  "--jit" "read.pas"  "$SAMPLES/read.stdin"

# --- AOT: build pl0_codegen.cul itself once, run every sample through it. ---
# `--vm` on the pl0.cul side only: a gate build with CULEBRA_REQUIRE_EXPLICIT_ENGINE
# set refuses to pick a default engine, and the AOT binary has no engine flag
# of its own to match.
if "$CULEBRA" build "$PL0_CODEGEN" -o "$TMP/pl0_codegen_aot" >/dev/null 2>&1; then
  for sample in square.pas gcd.pas fib.pas nested.pas odd.pas unary.pas; do
    want="$("$CULEBRA" --vm "$PL0" "$SAMPLES/$sample" 2>&1)"
    got="$("$TMP/pl0_codegen_aot" "$SAMPLES/$sample" 2>&1)"
    if [ "$got" != "$want" ]; then
      echo "FAIL [aot $sample]: pl0_codegen_aot differs from pl0.cul" >&2
      fail=1
    else
      echo "ok   [aot $sample]"
    fi
  done
  want="$("$CULEBRA" --vm "$PL0" "$SAMPLES/read.pas" <"$SAMPLES/read.stdin" 2>&1)"
  got="$("$TMP/pl0_codegen_aot" "$SAMPLES/read.pas" <"$SAMPLES/read.stdin" 2>&1)"
  if [ "$got" != "$want" ]; then
    echo "FAIL [aot read.pas]: pl0_codegen_aot differs from pl0.cul" >&2
    fail=1
  else
    echo "ok   [aot read.pas]"
  fi

  # AOT binary size: a sanity ceiling, not a tight budget. Measured ~2 MiB
  # (Peg's own force-loaded archive is ~1.1 MiB of that on its own; CodeGen
  # alone, without Peg, is under 1 MiB). The number that would actually be
  # alarming is LLVM accidentally getting linked in (50-80 MiB) -- cpp-vmlib's
  # LLVM lane is not compiled into culebra at all, so that can't happen here,
  # but the ceiling stays as a tripwire against a future regression.
  size="$(stat -c '%s' "$TMP/pl0_codegen_aot" 2>/dev/null || stat -f '%z' "$TMP/pl0_codegen_aot")"
  if [ "$size" -gt 5242880 ]; then
    echo "FAIL [aot size]: pl0_codegen_aot is $size bytes, expected < 5 MiB" >&2
    fail=1
  else
    echo "ok   [aot size] ($size bytes)"
  fi
else
  echo "skip [aot] (this build can't produce binaries)"
fi

# --- Diagnostics: pl0_codegen.cul's own binder, sanity-checked directly
# (not compared against pl0.cul -- see examples/pl0/pl0_codegen.cul's own
# header: bind-time vs run-time diagnostics are a deliberate difference). ---
check_err() {
  local desc="$1" sample_src="$2" want_substr="$3"
  local f="$TMP/$desc.pas"
  printf '%s' "$sample_src" >"$f"
  local got
  got="$("$CULEBRA" --vm "$PL0_CODEGEN" "$f" 2>&1)"
  case "$got" in
    *"$want_substr"*) echo "ok   [err $desc]" ;;
    *)
      echo "FAIL [err $desc]: want substring [$want_substr] got [$got]" >&2
      fail=1
      ;;
  esac
}

check_err "undefvar" 'BEGIN
  x := 1
END.
' "undefined variable 'x'"

check_err "constassign" 'CONST k = 1;
BEGIN
  k := 2
END.
' "cannot assign to constant 'k'"

check_err "undefproc" 'BEGIN
  CALL p
END.
' "undefined procedure 'p'"

if [ "$fail" -ne 0 ]; then
  echo "pl0_codegen_test FAIL" >&2
  exit 1
fi
