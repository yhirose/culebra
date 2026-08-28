#!/usr/bin/env bash
# The suite, on Windows, held to a shrinking list of known failures.
#
# `just test` runs tests/*.cul on Linux and macOS. Windows ran six of them
# until 2026-08-28, which is how a `wrap<T>` registration that the clang/lld
# switch dropped went unnoticed — the build linked, the tests that would have
# caught it were never run here, and the symptom was a namespace quietly
# missing its methods at run time. This runs all of them.
#
# The oracle is the file's own assertions plus its exit status, under the
# default engine. `--jit` and AOT are not swept: the JIT compile of the larger
# files costs minutes here, and what this is for is the platform difference,
# which the VM lane shows just as well. Per-backend symmetry on Windows is
# covered file by file elsewhere in the job.
#
# Usage: tools/check_suite_on_windows.sh <culebra.exe>
set -uo pipefail   # not -e: a failing test is data, not the end of the run
cd "$(dirname "$0")/.."

BIN="${1:?usage: check_suite_on_windows.sh <culebra exe>}"
[[ -x "$BIN" ]] || { echo "suite-on-windows: no $BIN" >&2; exit 1; }
LIST="${LIST:-tools/windows_known_failures.txt}"

# The window backend is a build option; a test naming Canvas must not try to
# open one here, exactly as the `just` recipes arrange for every other lane.
export CULEBRA_CANVAS_HEADLESS="${CULEBRA_CANVAS_HEADLESS:-1}"

known=$(grep -vE '^\s*(#|$)' "$LIST" | sort)
failed=""
pass=0
for f in tests/*.cul; do
  # A hang is a failure, not a stuck job: the file with the most work in it
  # runs in a couple of seconds, so a minute is a generous ceiling.
  out=$(timeout 60 "$BIN" --vm "$f" 2>&1)
  rc=$?
  if [[ $rc -eq 0 ]]; then
    pass=$((pass + 1))
    continue
  fi
  failed="$failed$f"$'\n'
  # Print what a NEW failure said; a known one is already accounted for.
  if ! grep -qxF "$f" <<<"$known"; then
    echo "=== $f (rc=$rc) ==="
    tail -8 <<<"$out"
  fi
done
failed=$(printf '%s' "$failed" | sort)

new=$(comm -23 <(printf '%s\n' "$failed") <(printf '%s\n' "$known") | grep -c . || true)
fixed=$(comm -13 <(printf '%s\n' "$failed") <(printf '%s\n' "$known") | grep -c . || true)

if [[ $new -ne 0 ]]; then
  echo "suite-on-windows FAIL: $new file(s) fail that $LIST does not list:" >&2
  comm -23 <(printf '%s\n' "$failed") <(printf '%s\n' "$known") | sed 's/^/  /' >&2
  echo "  Their output is above. Fix the file, or add it with the reason." >&2
  exit 1
fi
if [[ $fixed -ne 0 ]]; then
  echo "suite-on-windows FAIL: $fixed listed file(s) now pass:" >&2
  comm -13 <(printf '%s\n' "$failed") <(printf '%s\n' "$known") | sed 's/^/  /' >&2
  echo "  Delete them from $LIST — the list only shrinks." >&2
  exit 1
fi
echo "suite-on-windows OK ($pass passed, $(grep -c . <<<"$known") known-failing)"
