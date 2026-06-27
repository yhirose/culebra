#!/usr/bin/env bash
# Regression test for `culebra fmt` (include/formatter.h). Three checks:
#   1. Golden fixture: a messy comment-bearing input formats to an exact
#      expected output (operator spacing, blocks, leading/trailing comments).
#   2. Corpus safety: every real `.cul` under tests/ and examples/ formats
#      without the safety net refusing (exit 2) — i.e. the re-parse AST check
#      and the comment-preservation check both pass.
#   3. Idempotency: formatting the output again is a byte-for-byte fixed point.
# Usage: fmt_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: fmt_test.sh <culebra-binary>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="${TMPDIR:-/tmp}/culebra_fmt_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
fail=0

# --- 1. Golden fixture (comments preserved) -------------------------------
cat > "$TMP/in.cul" <<'EOF'
# header comment
let   x=1+2*3   # trailing
fn add(a,b){
  # leading inside block
  return a+b
}



if x>0{puts("hi")}else{puts("lo")}
EOF
cat > "$TMP/want.cul" <<'EOF'
# header comment
let x = 1 + 2 * 3  # trailing
fn add(a, b) {
  # leading inside block
  return a + b
}

if x > 0 {
  puts("hi")
} else {
  puts("lo")
}
EOF
"$CULEBRA" fmt "$TMP/in.cul" > "$TMP/got.cul" 2>"$TMP/err"
if ! diff -u "$TMP/want.cul" "$TMP/got.cul" > "$TMP/diff" 2>&1; then
  echo "FAIL golden: formatted output differs from expected"
  cat "$TMP/diff"
  fail=1
fi

# --- 2 + 3. Corpus safety + idempotency -----------------------------------
refused=0; notidem=0; n=0
while IFS= read -r f; do
  n=$((n + 1))
  "$CULEBRA" fmt "$f" > "$TMP/o1" 2>"$TMP/err"; rc=$?
  if [[ $rc -eq 2 ]]; then
    refused=$((refused + 1))
    echo "FAIL refused: $f -- $(cat "$TMP/err")"
    continue
  fi
  "$CULEBRA" fmt "$TMP/o1" > "$TMP/o2" 2>/dev/null
  if ! cmp -s "$TMP/o1" "$TMP/o2"; then
    notidem=$((notidem + 1))
    echo "FAIL not-idempotent: $f"
  fi
done < <(find "$ROOT/tests" "$ROOT/examples" -name '*.cul')
echo "corpus: $n files, refused=$refused notidem=$notidem"
[[ $refused -ne 0 || $notidem -ne 0 ]] && fail=1

# --- 4. Directory recursion ------------------------------------------------
mkdir -p "$TMP/dir/sub"
printf 'let  x=1\n' > "$TMP/dir/a.cul"
printf 'let  y=2\n' > "$TMP/dir/sub/b.cul"
printf 'plain text\n' > "$TMP/dir/note.txt"
# --check on a dirty tree exits 1
"$CULEBRA" fmt --check "$TMP/dir" >/dev/null 2>&1
[[ $? -eq 1 ]] || { echo "FAIL dir --check: expected exit 1 on dirty tree"; fail=1; }
# -i formats every .cul (and leaves the .txt alone), then the tree is clean
"$CULEBRA" fmt -i "$TMP/dir" >/dev/null 2>&1
"$CULEBRA" fmt --check "$TMP/dir" >/dev/null 2>&1
[[ $? -eq 0 ]] || { echo "FAIL dir -i: tree not clean after write"; fail=1; }
grep -q 'plain text' "$TMP/dir/note.txt" || { echo "FAIL dir: .txt was touched"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "fmt_test OK"; exit 0; fi
echo "fmt_test FAILED"; exit 1
