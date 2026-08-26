#!/usr/bin/env bash
# Measure which of the surface the two compiled engines share is reached only
# by the generated corpus (docs/internals/vm.md §10.4).
#
# Two profiles are produced, not one:
#   durable/  the hand-maintained suites, driven through the gate itself
#   all/      durable + the generated difftest corpus
# The difference is the answer: code live in `all` and dead in `durable` is
# semantics that only a generated case has ever executed.
#
# The durable half is the gate's `ci-light` lane spelled out here rather than
# invoked: three of that lane's phases inspect the binary itself with `nm` and
# would reject the shim below, and one `set -e` failure would truncate the
# measurement silently. Keeping the list means keeping it honest — when a suite
# joins the gate it has to join this too, or the difference this script prints
# grows for a reason that has nothing to do with the corpus.
#
# `ci-light` is the largest lane carrying no generated corpus; difftest,
# leak-fuzz and leak-abort-suite all build one from gen.cul and belong to the
# other half. The AOT lane is left out on purpose: what it adds over `--jit` is
# `vm::Lowering`, a consumer of the bytecode rather than shared fate, and
# report.py excludes it.
#
# gcda files are written through GCOV_PREFIX rather than into the build tree:
# libgcov's read-modify-write is not safe across concurrent processes, and both
# the gate and the corpus run many at once. A shim gives every process its own
# prefix, which is what libgcov actually wants, and lets the suites keep the
# parallelism they were written with.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${COVERAGE_BUILD:-$ROOT/build-cov}"
OUT="${COVERAGE_OUT:-$BUILD/profile}"
BIN="$BUILD/culebra"
OBJ="$BUILD/CMakeFiles/culebra.dir/src"
GCOV="${GCOV:-gcov-14}"
GCOV_TOOL="${GCOV_TOOL:-gcov-tool-14}"

fail() { echo "coverage: $*" >&2; exit 1; }
[ -x "$BIN" ] || fail "no instrumented binary at $BIN — run \`just coverage-build\`"
[ -n "$(ls "$OBJ"/*.gcno 2>/dev/null)" ] ||
  fail "$BIN has no .gcno — not a -DCULEBRA_COVERAGE=ON build"
command -v "$GCOV" > /dev/null || fail "$GCOV not found (override with GCOV=)"
command -v "$GCOV_TOOL" > /dev/null ||
  fail "$GCOV_TOOL not found (override with GCOV_TOOL=)"

cd "$ROOT"
rm -rf "$OUT"; mkdir -p "$OUT"

# The environment the gate runs its suites in, pinned rather than inherited:
# this script measures what the gate measures however it was launched, and
# neither of these has a reading here other than the gate's. (The justfile
# defaults them instead, because a developer may legitimately want a window;
# in a measurement, a windowed run just means a shorter one — see §13.2.)
export CULEBRA_CANVAS_HEADLESS=1
export CULEBRA_REQUIRE_EXPLICIT_ENGINE=1

# Every culebra process gets its own gcda directory, named by its pid. The
# suites run whatever binary $BIN names, so pointing them at this is the whole
# of the wiring.
SHIM="$BUILD/culebra-cov"
cat > "$SHIM" <<'SHIM_EOF'
#!/usr/bin/env bash
export GCOV_PREFIX="$COV_RAW/$COV_SET/$$"
export GCOV_PREFIX_STRIP=99
exec "$COV_BIN" "$@"
SHIM_EOF
chmod +x "$SHIM"
export COV_RAW="$OUT/raw" COV_BIN="$BIN"

# Fold a directory of per-process prefixes into one profile. gcov-tool merges a
# pair at a time, so this is a running accumulator; an optional third argument
# seeds it, which is how `all` is built on top of `durable` without re-reading
# the durable half.
fold() {
  local from="$1" into="$2" seed="${3:-}" d had=0
  rm -rf "$into"
  [ -n "$seed" ] && { cp -r "$seed" "$into"; had=1; }
  for d in "$from"/*/; do
    [ -d "$d" ] || continue
    if [ "$had" = 0 ]; then
      cp -r "$d" "$into"; had=1; continue
    fi
    "$GCOV_TOOL" merge "$into" "$d" -o "$into.next" > /dev/null 2>&1 ||
      fail "merge failed at $d"
    rm -rf "$into"; mv "$into.next" "$into"
  done
  [ "$had" = 1 ] || fail "nothing to fold in $from"
}

# --- the durable suites ----------------------------------------------------
# A failure does not stop the sweep — a program that exits non-zero has still
# executed the code it reached — but it is not ignored either. This is a
# *difference* measurement, and a durable program that dies early has executed
# less than it should, which inflates the corpus-only set by exactly the code
# it did not get to. That is how nine Canvas functions once looked like holes.
# So: keep going, and count.
#
# `culebra test` without --doc runs here too since B7-c: the unit runner
# has been the VM's since B6b (vm::Session + the shared ambient), which is
# exactly the shared-fate surface this script measures. Its old exclusion
# rationale ("the runner is the interpreter's") stopped being true then.
export COV_SET=durable
durable_runs=0 durable_bad=0
declare -a durable_failed=()
JOBS="${COVERAGE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

# Run a list of files through the shim, one process per file, serially.
#
# Serially on purpose. The first version of this ran the durable half at
# `xargs -P $JOBS` and measured *less* of the runtime than the serial version
# had — the load-sensitive suites (the Http server, net, isolate) start losing
# runs when twenty instrumented -O0 processes compete, and a measurement that
# moves with machine load is not a measurement. The corpus below keeps its
# parallelism: those chunks are pure computation, bind no ports and start no
# children.
sweep() {
  local label="$1" lane="$2" f; shift 2
  echo ">>> durable: $label ($lane)"
  for f in "$@"; do note_run "$lane $f" "$SHIM" "$lane" "$f"; done
}

# Run one durable invocation, remembering whether it succeeded.
#
# A case that ends in an uncaught throw is not a failed run. Half of vm_cases
# and four of the leak-abort probes are curated around error behaviour and
# exit non-zero by design — 84 of 177 and 4 of 8 here — and where their exit
# code matters it is checked properly: compare.sh holds all three lanes to the
# same one. Suites like that are swept with NOTE_RUN_MAY_THROW, which accepts
# exactly the status an uncaught throw leaves (255). Anything else is still a
# failure, a signal included, because those are the runs that stopped early
# and left the corpus-only set inflated by what they did not reach.
THROW_STATUS=255
note_run() {
  local what="$1" rc; shift
  durable_runs=$((durable_runs + 1))
  "$@" > /dev/null 2>&1; rc=$?
  [ "$rc" = 0 ] && return
  [ "${NOTE_RUN_MAY_THROW:-0}" = 1 ] && [ "$rc" = "$THROW_STATUS" ] && return
  durable_bad=$((durable_bad + 1))
  durable_failed+=("$what (rc=$rc)")
}

for lane in --vm --jit; do
  sweep "tests" "$lane" tests/*.cul
  NOTE_RUN_MAY_THROW=1 sweep "vm_cases" "$lane" tools/bench/vm_cases/*.cul
  sweep "isolate" "$lane" tests/isolate/*.cul
  NOTE_RUN_MAY_THROW=1 sweep "leak-abort probes" "$lane" \
    tools/difftest/leak_abort_bare.d/*.cul
  echo ">>> durable: doctest ($lane)"
  note_run "doctest $lane" "$SHIM" test --doc "$lane" tests/doctest docs
  [ "$lane" = --vm ] && note_run "culebra test $lane" "$SHIM" test "$lane" tests/culebra_test_self/
done

# The fast-codegen emitter path: same programs, a different route through the
# shared emitter, which nothing above takes.
sweep "fast codegen" --jit-faststart tests/*.cul

# The CLI scripts ctest owns. ctest itself bakes the binary path at configure
# time, so it cannot be pointed at the shim — the scripts take it as $1.
echo ">>> durable: CLI scripts"
for s in tests/*_test.sh; do note_run "$s" bash "$s" "$SHIM"; done

printf '%s\n' "$durable_bad" > "$OUT/durable_failures"
printf '%s\n' "${durable_failed[@]:-}" >> "$OUT/durable_failures"
echo ">>> durable: $durable_runs invocation(s), $durable_bad non-zero"

# --- the generated corpus --------------------------------------------------
export COV_SET=corpus
echo ">>> corpus: generating"
# The generator is not the workload: its own coverage goes to a throwaway.
COV_SET=gen "$SHIM" --vm tools/difftest/gen.cul > "$OUT/cases.cul" ||
  fail "gen.cul did not run cleanly"
cases=$(grep -c '^_p(' "$OUT/cases.cul")
# Same floor difftest carries. A degraded generator would otherwise show up as
# a *smaller* corpus-only set — the measurement getting greener as the corpus
# gets worse.
[ "$cases" -ge 1000 ] || fail "generator produced only $cases cases"
echo "    $cases cases"

CHUNK="${COVERAGE_CHUNK:-400}"
mkdir -p "$OUT/chunks"
split -l "$CHUNK" <(grep '^_p(' "$OUT/cases.cul") "$OUT/chunks/c"
for body in "$OUT"/chunks/c*; do
  cat tools/difftest/preamble.cul tools/difftest/canvas_fixtures.cul "$body" \
    > "$body.cul"
done
echo "    $(ls "$OUT"/chunks/c*.cul | wc -l) chunks"

echo ">>> corpus: running"
for lane in --vm --jit; do
  printf '%s\n' "$OUT"/chunks/c*.cul \
    | xargs -P "$JOBS" -I '{}' "$SHIM" "$lane" '{}' > /dev/null 2>&1
done

# --- fold ------------------------------------------------------------------
echo ">>> folding"
fold "$OUT/raw/durable" "$OUT/durable"
fold "$OUT/raw/corpus" "$OUT/all" "$OUT/durable"

# --- gcov ------------------------------------------------------------------
# gcov wants the notes and the data side by side, so each profile visits the
# object directory in turn. The build tree's own gcda is never used.
emit() {
  local prof="$1" name="$2"
  rm -f "$OBJ"/*.gcda
  cp "$prof"/*.gcda "$OBJ"/ 2>/dev/null
  ( cd "$OBJ" && "$GCOV" -j -f *.gcda > /dev/null 2>&1 )
  mkdir -p "$OUT/json/$name"
  mv "$OBJ"/*.gcov.json.gz "$OUT/json/$name/" 2>/dev/null
  rm -f "$OBJ"/*.gcda
  echo "    $name: $(ls "$OUT/json/$name" | wc -l) TU(s)"
}
echo ">>> gcov"
emit "$OUT/durable" durable
emit "$OUT/all" all

echo "coverage: profiles in $OUT/json"
