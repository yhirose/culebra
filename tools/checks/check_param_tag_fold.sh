#!/usr/bin/env bash
# A parameter's declared primitive type reaches the emitted code.
#
# `fn f(s: String)` is checked on entry (docs/language.md §14), so every read
# of `s` below satisfies it. Op::ArgTag hands that fact to the JIT as a
# constant tag, and what it buys is visible in the IR: a built-in method call
# on the parameter resolves to ONE receiver arm instead of the switch its
# name shares across String/Array/Object/Set/Tuple, and the join those arms
# merge into goes with them.
#
# Both probes below are the same chain; only the annotation differs. The
# annotated one must leave no receiver switch, and the bare one must leave
# some — a control that folds on its own would mean the gate is measuring
# nothing, which is how a check quietly stops guarding anything.
set -euo pipefail
cd "$(dirname "$0")/../.."

# The binary under test, as check_float_carry.sh takes it: the caller's, or
# the one the inner loop just built. Never a build that may predate the
# change being checked.
BIN="${1:-build-dev/culebra}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/culebra-paramtag.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/typed.cul" <<'CUL'
fn hot(s: String) {
  s.trim().size()
}

println(hot(' ab '))
CUL

sed 's/s: String/s/' "$TMP/typed.cul" > "$TMP/bare.cul"

# The size probe's error default names the arm set it could not resolve; the
# label survives only as long as the switch does.
probes() {
  "$BIN" --jit --emit-llvm "$1" 2>/dev/null |
    grep -c 'switch i8 .*label %vbm.missfail' || true
}

typed=$(probes "$TMP/typed.cul")
bare=$(probes "$TMP/bare.cul")

if [ "$bare" -eq 0 ]; then
  echo "param-tag-fold FAIL: the unannotated control folds too, so this gate" >&2
  echo "  measures nothing. Rewrite the probe around a call that still needs" >&2
  echo "  the annotation." >&2
  exit 1
fi

if [ "$typed" -ne 0 ]; then
  echo "param-tag-fold FAIL: a chain on a String-annotated parameter still" >&2
  echo "  emits $typed unresolved receiver switch(es); it should emit none." >&2
  echo "  Op::ArgTag (vm.h) puts the entry check's answer in the slot and" >&2
  echo "  lowering.h substitutes it — one of the two stopped." >&2
  exit 1
fi

echo "param-tag-fold OK (annotated 0, bare $bare receiver switch(es))"
