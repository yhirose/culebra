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

# --- 2 + 3. Corpus safety + idempotency (parallel) ------------------------
# Format every corpus file twice — once to check the re-parse/comment safety
# net doesn't refuse (exit 2), once more to assert idempotency. The files are
# independent and each run is a fresh CPU-bound culebra startup, so this fans
# out near-linearly. Per-file scratch keeps workers from clobbering a shared
# temp; refused/not-idempotent verdicts land as marker files collected below.
CORPUS="$TMP/corpus"; mkdir -p "$CORPUS"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)"
export CULEBRA CORPUS
# Enumerate the corpus once (NUL-delimited) so the worker fan-out and the file
# count `n` can't drift apart if the roots/pattern are ever edited.
find "$ROOT/tests" "$ROOT/examples" -name '*.cul' -print0 > "$CORPUS/files.z"
n=$(tr -dc '\0' < "$CORPUS/files.z" | wc -c | tr -d ' ')
xargs -0 -n1 -P "$JOBS" bash -c '
      f="$1"
      slug=$(printf "%s" "$f" | tr -c "[:alnum:]" "_")
      o1="$CORPUS/$slug.o1"; o2="$CORPUS/$slug.o2"
      "$CULEBRA" fmt "$f" > "$o1" 2>"$CORPUS/$slug.err"
      if [[ $? -eq 2 ]]; then printf "%s" "$f" > "$CORPUS/$slug.refused"; exit 0; fi
      "$CULEBRA" fmt "$o1" > "$o2" 2>/dev/null
      cmp -s "$o1" "$o2" || printf "%s" "$f" > "$CORPUS/$slug.notidem"
    ' _ < "$CORPUS/files.z"
refused=0; notidem=0
for m in "$CORPUS"/*.refused; do
  [[ -e "$m" ]] || continue
  refused=$((refused + 1))
  echo "FAIL refused: $(cat "$m") -- $(cat "${m%.refused}.err")"
done
for m in "$CORPUS"/*.notidem; do
  [[ -e "$m" ]] || continue
  notidem=$((notidem + 1))
  echo "FAIL not-idempotent: $(cat "$m")"
done
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
