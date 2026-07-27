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



if x>0{inspect("hi")}else{inspect("lo")}
EOF
cat > "$TMP/want.cul" <<'EOF'
# header comment
let x = 1 + 2 * 3  # trailing
fn add(a, b) {
  # leading inside block
  return a + b
}

if x > 0 {
  inspect("hi")
} else {
  inspect("lo")
}
EOF
"$CULEBRA" fmt "$TMP/in.cul" > "$TMP/got.cul" 2>"$TMP/err"
if ! diff -u "$TMP/want.cul" "$TMP/got.cul" > "$TMP/diff" 2>&1; then
  echo "FAIL golden: formatted output differs from expected"
  cat "$TMP/diff"
  fail=1
fi

# --- 1b. Golden fixture: bare lexical-scope blocks (LEXICAL_SCOPE) ---------
# A `{ ... }` statement block nested inside a fn/loop body must keep its braces
# and reformat its interior (comments included). The optimizer folds the
# enclosing body's `{` onto the scope, which once made fmt strip the inner
# braces (safety net refused) — see include/formatter.h print_lexical_scope.
cat > "$TMP/blk_in.cul" <<'EOF'
fn g() {
  // before block
  {
    let r=1 // trailing
    // standalone
    let s=2
  }
}
for i in 0..3 {
  {
    let a = i
  }
}
EOF
cat > "$TMP/blk_want.cul" <<'EOF'
fn g() {
  // before block
  {
    let r = 1  // trailing
    // standalone
    let s = 2
  }
}
for i in 0..3 {
  {
    let a = i
  }
}
EOF
"$CULEBRA" fmt "$TMP/blk_in.cul" > "$TMP/blk_got.cul" 2>"$TMP/blk_err"
if ! diff -u "$TMP/blk_want.cul" "$TMP/blk_got.cul" > "$TMP/blk_diff" 2>&1; then
  echo "FAIL golden (bare block): formatted output differs from expected"
  cat "$TMP/blk_diff"
  fail=1
fi

# --- 1c. Golden fixture: same-precedence parentheses -----------------------
# The grammar folds a run of same-precedence operators into ONE flat node, so
# `(a * b) / c` and `a * b / c` mean the same thing but are different trees.
# Associativity once made fmt drop those parentheses, which reshaped the AST
# and made the re-parse safety net refuse the whole file — see
# include/formatter.h print_operand.
cat > "$TMP/par_in.cul" <<'EOF'
let a=1
let b=2
let c=3
let p = (a*b)/c
let q = a+(b*c)*a
let r = (a-b)-c
let s = a**(b**c)
let t = a*(b*c)/a
let u = a*b/c
EOF
cat > "$TMP/par_want.cul" <<'EOF'
let a = 1
let b = 2
let c = 3
let p = (a * b) / c
let q = a + (b * c) * a
let r = (a - b) - c
let s = a ** (b ** c)
let t = a * (b * c) / a
let u = a * b / c
EOF
"$CULEBRA" fmt "$TMP/par_in.cul" > "$TMP/par_got.cul" 2>"$TMP/par_err"
if ! diff -u "$TMP/par_want.cul" "$TMP/par_got.cul" > "$TMP/par_diff" 2>&1; then
  echo "FAIL golden (same-precedence parens): formatted output differs from expected"
  cat "$TMP/par_diff"
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

# --- 5. Input selection ----------------------------------------------------
# stdin mode is chosen from the arguments alone, never as a fallback: paths
# that expand to no .cul file used to land here and block on the terminal.
# Every case reads /dev/null so a regression shows up as a wrong exit code
# rather than a hung gate.
mkdir -p "$TMP/emptydir"
"$CULEBRA" fmt "$TMP/emptydir" >/dev/null 2>&1 < /dev/null
[[ $? -eq 2 ]] || { echo "FAIL empty dir: expected exit 2, not stdin mode"; fail=1; }
"$CULEBRA" fmt "$TMP/dir/a.cul" - >/dev/null 2>&1 < /dev/null
[[ $? -eq 2 ]] || { echo "FAIL '-' with a path: expected exit 2"; fail=1; }
printf 'let  z=3\n' | "$CULEBRA" fmt - > "$TMP/stdin_dash" 2>/dev/null
printf 'let  z=3\n' | "$CULEBRA" fmt > "$TMP/stdin_bare" 2>/dev/null
grep -qx 'let z = 3' "$TMP/stdin_dash" || { echo "FAIL fmt -: stdin not formatted"; fail=1; }
cmp -s "$TMP/stdin_dash" "$TMP/stdin_bare" || { echo "FAIL fmt: '-' and no-args disagree"; fail=1; }

# --- 6. Argument handling -------------------------------------------------
# A misspelled flag must stop the run instead of being taken for a file name:
# formatting the *other* arguments while the requested flag silently did
# nothing is how a typo goes unnoticed.
printf 'let  x=1\n' > "$TMP/arg.cul"
for bad in --wirte -x --in_place; do
  out=$("$CULEBRA" fmt "$bad" "$TMP/arg.cul" 2>"$TMP/arg.err"); rc=$?
  err=$(cat "$TMP/arg.err")
  if [[ $rc -ne 2 || -n "$out" || "$err" != *"unknown option '$bad'"* ]]; then
    echo "FAIL unknown-flag [$bad]: rc=$rc out=$out err=$err"; fail=1
  fi
done

# `-` is stdin; combining it with real paths would mean two inputs and one
# stdout, so it is refused rather than letting either side win silently.
out=$(printf 'let   Z=9\n' | "$CULEBRA" fmt - "$TMP/arg.cul" 2>"$TMP/arg.err"); rc=$?
if [[ $rc -ne 2 || -n "$out" || "$(cat "$TMP/arg.err")" != *"can't mix"* ]]; then
  echo "FAIL stdin-plus-path: rc=$rc out=$out err=$(cat "$TMP/arg.err")"; fail=1
fi
# Likewise `-i` with stdin: there is no file to rewrite. Both the explicit `-`
# and the implicit no-paths form must say so instead of ignoring `-i`.
for args in "-i -" "-i"; do
  out=$(printf 'let   Z=9\n' | "$CULEBRA" fmt $args 2>"$TMP/arg.err"); rc=$?
  if [[ $rc -ne 2 || -n "$out" || "$(cat "$TMP/arg.err")" != *"nothing to rewrite"* ]]; then
    echo "FAIL stdin-in-place [$args]: rc=$rc out=$out err=$(cat "$TMP/arg.err")"; fail=1
  fi
done

# `-i` composes with the reporting flags (as `gofmt -l -w` does): the file is
# rewritten AND named. Before this, list/check won and nothing was written.
printf 'let  x=1\n' > "$TMP/arg.cul"
out=$("$CULEBRA" fmt -i -l "$TMP/arg.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || "$out" != "$TMP/arg.cul" ]]; then
  echo "FAIL -i -l: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg.cul")" == "let x = 1" ]] || {
  echo "FAIL -i -l: file not rewritten: $(cat "$TMP/arg.cul")"; fail=1; }
# Already formatted: nothing to list, exit 0.
out=$("$CULEBRA" fmt -i -l "$TMP/arg.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL -i -l (clean): rc=$rc out=$out"; fail=1
fi

printf 'let  y=2\n' > "$TMP/arg2.cul"
out=$("$CULEBRA" fmt -i --check "$TMP/arg2.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || -n "$out" ]]; then
  echo "FAIL -i --check: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg2.cul")" == "let y = 2" ]] || {
  echo "FAIL -i --check: file not rewritten: $(cat "$TMP/arg2.cul")"; fail=1; }
out=$("$CULEBRA" fmt -i --check "$TMP/arg2.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL -i --check (clean): rc=$rc out=$out"; fail=1
fi

# Without `-i`, the reporting flags still only report (no stdout dump).
printf 'let  z=3\n' > "$TMP/arg3.cul"
out=$("$CULEBRA" fmt -l "$TMP/arg3.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || "$out" != "$TMP/arg3.cul" ]]; then
  echo "FAIL -l alone: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg3.cul")" == "let  z=3" ]] || {
  echo "FAIL -l alone: file was rewritten"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "fmt_test OK"; exit 0; fi
echo "fmt_test FAILED"; exit 1
