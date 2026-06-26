#!/usr/bin/env bash
# Regression test for `culebra fmt` (include/formatter.h). Three checks:
#   1. Golden fixtures: a messy input formats to an exact expected output.
#   2. Comment guard: a file with comments is left byte-for-byte unchanged
#      (Phase 0 cannot preserve comments, so it must not drop them).
#   3. Corpus safety + idempotency: every real `.cul` under tests/ and
#      examples/ formats without the safety net refusing (exit 2) and is a
#      fixed point of the formatter (fmt(fmt(x)) == fmt(x)). The comment guard
#      is bypassed (CULEBRA_FMT_FORCE=1) so the printer itself is exercised
#      over the whole corpus, not just the comment-free files.
# Usage: fmt_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: fmt_test.sh <culebra-binary>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="${TMPDIR:-/tmp}/culebra_fmt_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
fail=0

# --- 1. Golden fixture ----------------------------------------------------
cat > "$TMP/in.cul" <<'EOF'
let   x=1+2*3
fn add(a,b){return a+b}



let g=(a||b)&&c
if x>0{puts("hi")}else{puts("lo")}
xs.map(|n|n*2).filter(|n|n>0)
EOF
cat > "$TMP/want.cul" <<'EOF'
let x = 1 + 2 * 3
fn add(a, b) {
  return a + b
}

let g = (a || b) && c
if x > 0 {
  puts("hi")
} else {
  puts("lo")
}
xs.map(|n| n * 2).filter(|n| n > 0)
EOF
"$CULEBRA" fmt "$TMP/in.cul" > "$TMP/got.cul" 2>"$TMP/err"
if ! diff -u "$TMP/want.cul" "$TMP/got.cul" > "$TMP/diff" 2>&1; then
  echo "FAIL golden: formatted output differs from expected"
  cat "$TMP/diff"
  fail=1
fi

# --- 2. Comment guard: file with comments must be untouched ---------------
printf 'let x = 1  # keep\nfn f() { x }\n' > "$TMP/c.cul"
"$CULEBRA" fmt "$TMP/c.cul" > "$TMP/c_out.cul" 2>/dev/null
if ! diff -q "$TMP/c.cul" "$TMP/c_out.cul" >/dev/null; then
  echo "FAIL comment-guard: a commented file was modified"
  fail=1
fi

# --- 3. Corpus safety + idempotency (force, so the printer is exercised) ---
export CULEBRA_FMT_FORCE=1
refused=0; notidem=0; n=0
while IFS= read -r f; do
  n=$((n + 1))
  "$CULEBRA" fmt "$f" > "$TMP/o1" 2>"$TMP/err"; rc=$?
  if [[ $rc -eq 2 ]]; then
    refused=$((refused + 1))
    echo "FAIL refused: $f -- $(cat "$TMP/err")"
    continue
  fi
  "$CULEBRA" fmt - < "$TMP/o1" > "$TMP/o2" 2>/dev/null
  if ! diff -q "$TMP/o1" "$TMP/o2" >/dev/null; then
    notidem=$((notidem + 1))
    echo "FAIL not-idempotent: $f"
  fi
done < <(find "$ROOT/tests" "$ROOT/examples" -name '*.cul')
unset CULEBRA_FMT_FORCE
echo "corpus: $n files, refused=$refused notidem=$notidem"
[[ $refused -ne 0 || $notidem -ne 0 ]] && fail=1

if [[ $fail -eq 0 ]]; then echo "fmt_test OK"; exit 0; fi
echo "fmt_test FAILED"; exit 1
