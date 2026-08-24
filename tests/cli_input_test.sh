#!/usr/bin/env bash
# Regression test for how the driver takes its input and for --ast (src/main.cc
# read_file / run_scripts). None of it is reachable from a .cul sweep: the cases
# are about what the process does with a non-regular path, an unwritable file,
# or a flag documented to stop before running.
# Usage: cli_input_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: cli_input_test.sh <culebra-binary>}"
[[ "$CULEBRA" = /* ]] || CULEBRA="$PWD/$CULEBRA"   # some cases run from $TMP
TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0

# A script arriving through a pipe has no size to seek to. Sizing it up front
# yielded 0 and ran an empty program, exiting 0 with no output at all.
out=$("$CULEBRA" --vm <(printf 'IO.println("piped")\n') 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "piped" ]] || { echo "FAIL pipe: rc=$rc out=$out"; fail=1; }

# A directory opens like a file, so it used to reach the reader and blow up
# there (a huge seek size, then a throw out of the stream).
out=$("$CULEBRA" --vm "$TMP" 2>&1); rc=$?
[[ $rc -ne 0 ]] || { echo "FAIL dir: expected failure, got rc=0"; fail=1; }
[[ "$out" == *"can't open"* ]] || { echo "FAIL dir: unexpected message: $out"; fail=1; }

# --ast prints the parsed program *instead of running it* (docs/language.md,
# and the --help text) — the dump used to be followed by a real run.
printf 'IO.println("RAN")\n' > "$TMP/ast.cul"
out=$("$CULEBRA" --ast "$TMP/ast.cul" 2>&1); rc=$?
[[ $rc -eq 0 ]] || { echo "FAIL --ast: rc=$rc"; fail=1; }
[[ -n "$out" ]] || { echo "FAIL --ast: printed nothing"; fail=1; }
grep -qx 'RAN' <<<"$out" && { echo "FAIL --ast: the program ran"; fail=1; }

# One program has one parsed AST: --jit must not change the dump (it splices
# the stdlib preamble in for its own run, which is not what was parsed).
if "$CULEBRA" --help 2>&1 | grep -q -- '--jit'; then
  jit_out=$("$CULEBRA" --ast --jit "$TMP/ast.cul" 2>&1)
  [[ "$jit_out" == "$out" ]] || { echo "FAIL --ast: --jit dumps a different AST"; fail=1; }
fi

# An in-place rewrite that can't be written must say so and exit non-zero,
# rather than reporting the file as formatted/fixed.
printf 'let  x=1\n' > "$TMP/ro.cul"
chmod 444 "$TMP/ro.cul"
out=$("$CULEBRA" fmt -i "$TMP/ro.cul" 2>&1); rc=$?
[[ $rc -eq 2 && "$out" == *"can't write"* ]] || {
  echo "FAIL fmt -i readonly: rc=$rc out=$out"; fail=1; }
grep -qx 'let  x=1' "$TMP/ro.cul" || { echo "FAIL fmt -i readonly: file changed"; fail=1; }

printf "import Math from 'std/math'\nIO.println(1)\n" > "$TMP/ro_lint.cul"
chmod 444 "$TMP/ro_lint.cul"
out=$("$CULEBRA" lint --fix "$TMP/ro_lint.cul" 2>&1); rc=$?
[[ $rc -eq 2 && "$out" == *"can't write"* ]] || {
  echo "FAIL lint --fix readonly: rc=$rc out=$out"; fail=1; }
[[ "$out" != *"fixed 1 unused import"* ]] || {
  echo "FAIL lint --fix readonly: reported a fix it could not write"; fail=1; }

# `--bail`'s count is optional, so a path could be read as one: std::stoi
# stopped at the first non-digit, making `--bail 3rd_party/` mean 3 and eat the
# path. Three failing tests tell the counts apart.
mkdir -p "$TMP/3rd_party"
cat > "$TMP/3rd_party/test_bail.cul" <<'EOF'
@test
fn fails_one() { assert_eq(1, 2) }
@test
fn fails_two() { assert_eq(3, 4) }
@test
fn fails_three() { assert_eq(5, 6) }
EOF
# The path has to be relative and start with a digit — an absolute /tmp/... one
# fails std::stoi outright and never reproduces the bug.
out=$(cd "$TMP" && "$CULEBRA" test --vm --bail 3rd_party 2>&1)
[[ "$out" == *"0 passed, 1 failed"* ]] || {
  echo "FAIL --bail <path>: the path was read as a count: $(tail -1 <<<"$out")"; fail=1; }
out=$(cd "$TMP" && "$CULEBRA" test --vm --bail 2 3rd_party 2>&1)
[[ "$out" == *"0 passed, 2 failed"* ]] || {
  echo "FAIL --bail 2: $(tail -1 <<<"$out")"; fail=1; }
out=$(cd "$TMP" && "$CULEBRA" test --vm 3rd_party 2>&1)
[[ "$out" == *"0 passed, 3 failed"* ]] || {
  echo "FAIL no --bail: $(tail -1 <<<"$out")"; fail=1; }
# A count that disables bailing while looking like it asks for one is rejected.
for bad in "--bail=abc" "--bail=0" "--bail=3x"; do
  out=$("$CULEBRA" test --vm "$bad" "$TMP/3rd_party" 2>&1); rc=$?
  [[ $rc -eq 2 && "$out" == *"--bail needs a count"* ]] || {
    echo "FAIL $bad: rc=$rc out=$(tail -1 <<<"$out")"; fail=1; }
done

# Embed.dir bakes a directory into the binary. A file it can't read, or a
# subdirectory it can't walk into, used to be skipped in silence — the build
# reported success and the binary served a table with holes in it.
mkdir -p "$TMP/emb/assets/sub"
printf 'a\n' > "$TMP/emb/assets/a.txt"
printf 'b\n' > "$TMP/emb/assets/sub/b.txt"
printf 'let d = Embed.dir("assets")\nIO.println("x")\n' > "$TMP/emb/app.cul"
# `culebra build` needs the runtime archives to finish the link; the asset walk
# runs first, so its diagnostics are observable either way.
embed_out() { "$CULEBRA" build "$TMP/emb/app.cul" -o "$TMP/emb/app" 2>&1; }
out=$(embed_out)
[[ "$out" == *"embedded 2 file(s)"* ]] || {
  echo "FAIL embed baseline: $out"; fail=1; }
chmod 000 "$TMP/emb/assets/sub"
out=$(embed_out)
[[ "$out" == *"can't read"* ]] || {
  echo "FAIL embed unreadable dir: walked past it silently: $out"; fail=1; }
[[ "$out" != *"embedded 1 file(s)"* ]] || {
  echo "FAIL embed unreadable dir: baked a partial table"; fail=1; }
chmod 755 "$TMP/emb/assets/sub"
chmod 000 "$TMP/emb/assets/a.txt"
out=$(embed_out)
[[ "$out" == *"can't read"* ]] || {
  echo "FAIL embed unreadable file: embedded it as empty: $out"; fail=1; }
chmod 644 "$TMP/emb/assets/a.txt"

# `culebra -` reads the script from stdin (the Python / Ruby convention),
# instead of trying to open a file literally named `-`.
out=$(printf 'IO.println("from stdin")\n' | "$CULEBRA" --vm - 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "from stdin" ]] || {
  echo "FAIL culebra -: rc=$rc out=$out"; fail=1; }

if "$CULEBRA" --help 2>&1 | grep -q -- '--jit'; then
  out=$(printf 'IO.println("from stdin")\n' | "$CULEBRA" --jit - 2>&1); rc=$?
  [[ $rc -eq 0 && "$out" == "from stdin" ]] || {
    echo "FAIL culebra --jit -: rc=$rc out=$out"; fail=1; }
fi

# `Sys.script` has no real file to point at when the script came from stdin —
# it reads back as nil there, same as the REPL.
out=$(printf 'IO.println(Sys.script)\n' | "$CULEBRA" --vm - 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "nil" ]] || {
  echo "FAIL culebra - Sys.script: rc=$rc out=$out"; fail=1; }

# Arguments after `-` still land in Sys.argv, same as after a real path.
out=$(printf 'IO.println(Sys.argv)\n' | "$CULEBRA" --vm - a b 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "['a', 'b']" ]] || {
  echo "FAIL culebra - argv: rc=$rc out=$out"; fail=1; }

# A file actually named `-` is unambiguous once it's spelled as a path
# (`./-`), so bare `-` staying reserved for stdin costs nothing.
printf 'IO.println("literal dash file")\n' > "$TMP/-"
out=$(cd "$TMP" && "$CULEBRA" --vm "./-" 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "literal dash file" ]] || {
  echo "FAIL ./- : rc=$rc out=$out"; fail=1; }

# --- which engine a command line that names none means ---
# The only place the default itself is observable: every other lane names an
# engine, which is what CULEBRA_REQUIRE_EXPLICIT_ENGINE is for — so these runs
# drop it, the way tools/difftest/release_diff.sh does, and are the one thing
# in the tree that must NOT name one. The startup profile is the observation,
# and the assertion is POSITIVE (the executor's own mark) rather than "not the
# interpreter's": a default that drifted to the JIT would satisfy the negative
# one. `rc` is asserted too, since an abort prints no mark either.
bare() { env -u CULEBRA_REQUIRE_EXPLICIT_ENGINE "$CULEBRA" "$@"; }

printf 'IO.println("engine")\n' > "$TMP/engine.cul"
out=$(CULEBRA_PROFILE_STARTUP=1 bare "$TMP/engine.cul" 2>&1 >/dev/null); rc=$?
[[ $rc -eq 0 && "$out" == *"vm::Exec::run"* && "$out" != *"interpret_modules"* ]] || {
  echo "FAIL default engine: a bare run did not take the executor (rc=$rc)"
  echo "$out"; fail=1; }
out=$(CULEBRA_PROFILE_STARTUP=1 bare --tree "$TMP/engine.cul" 2>&1 >/dev/null)
[[ "$out" == *"interpret_modules"* ]] || {
  echo "FAIL --tree: it no longer names the tree-walker"; fail=1; }

# The REPL is the second reader of that default, and `--jit` means the same
# tier-0 engine there rather than falling through to the tree-walker.
for flag in "" "--jit"; do
  out=$(printf 'let x = 6 * 7\nx\n' | bare $flag --shell 2>&1)
  [[ "$out" == *"42" ]] || { echo "FAIL default REPL [$flag]: $out"; fail=1; }
done

# `culebra test` and `culebra test --doc` pick their own defaults, in their own
# parser. Each is checked twice: that it still routes through a default-picking
# branch (the ratchet aborts it), and that the branch it picks runs the suite.
mkdir -p "$TMP/suite"
printf '@test\nfn one() {\n  assert_eq(1, 1)\n}\n' > "$TMP/suite/test_one.cul"
printf 'A doc.\n\n```culebra\nIO.inspect(1 + 1)  # => 2\n```\n' > "$TMP/suite/a.md"
out=$(bare test "$TMP/suite" 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == *"ok  one"* ]] || {
  echo "FAIL default test runner: rc=$rc out=$out"; fail=1; }
out=$(bare test --doc "$TMP/suite" 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == *"1 passed"* ]] || {
  echo "FAIL default doctest runner: rc=$rc out=$out"; fail=1; }
for mode in "" "--doc"; do
  out=$(CULEBRA_REQUIRE_EXPLICIT_ENGINE=1 "$CULEBRA" test $mode "$TMP/suite" 2>&1)
  rc=$?
  [[ $rc -eq 134 && "$out" == *"picked an engine by default"* ]] || {
    echo "FAIL test runner default site [$mode]: rc=$rc out=$out"; fail=1; }
done

if [[ $fail -eq 0 ]]; then echo "cli_input_test OK"; exit 0; fi
echo "cli_input_test FAILED"; exit 1
