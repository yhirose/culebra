#!/usr/bin/env bash
# Smoke test for the release-diff comparator (release_diff.py).
#
# The gate it guards is quiet by construction: when nothing changed, a working
# comparator and a broken one both print OK. It also runs on master pushes
# only, so a comparator that stopped comparing would sit unnoticed until the
# release it was supposed to describe. So the detector gets its own test, the
# way the loud leak detector does (leak_abort.sh) — synthetic records, no
# culebra binary, well under a second.
#
# Each case is a way the comparator has to be able to fail, not just to pass.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="$HERE/release_diff.py"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/culebra-rdiff.XXXXXX")" || { echo "error: mktemp -d failed" >&2; exit 2; }
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fails=0
LAST=""

# Run the comparator and check its exit status, then let the caller grep LAST.
check() {
  local name="$1" want="$2" rc
  shift 2
  LAST=$("$@" 2>&1); rc=$?
  if [ "$rc" != "$want" ]; then
    echo "release-diff selftest: FAIL $name — rc=$rc, expected $want" >&2
    printf '%s\n' "$LAST" >&2
    fails=$((fails + 1))
  fi
}

# Check that the report said something, so a case cannot pass on exit status
# while reporting nothing.
says() {
  local name="$1" pat="$2"
  grep -q "$pat" <<< "$LAST" || {
    echo "release-diff selftest: FAIL $name — report lacks '$pat'" >&2
    printf '%s\n' "$LAST" >&2
    fails=$((fails + 1))
  }
}

mk() { printf '%s\n' "$@"; }

: > allow_empty.txt

# Nothing changed.
mk "a|1 ::: ok=Long:1" "b|2 ::: ok=Long:2" > same_b.txt
cp same_b.txt same_h.txt
check identical 0 "$PY" --baseline same_b.txt --head same_h.txt \
  --allow allow_empty.txt --cases 2
says identical "0 changed, 0 allowed, 0 unlisted"

# A change the allowlist names is allowed; the same change unnamed fails.
mk "m|1|title|'' ::: err=TypeError|no method 'title'|1|1" "b|2 ::: ok=Long:2" \
  > new_b.txt
mk "m|1|title|'' ::: ok=String:\"\"" "b|2 ::: ok=Long:2" > new_h.txt
printf 'm|*|title|*\n' > allow_title.txt
check allowed 0 "$PY" --baseline new_b.txt --head new_h.txt \
  --allow allow_title.txt --cases 2
says allowed "1 changed, 1 allowed, 0 unlisted"
check unlisted 1 "$PY" --baseline new_b.txt --head new_h.txt \
  --allow allow_empty.txt --cases 2
says unlisted "1 unlisted"

# A pattern matching nothing is reported, not fatal — the release that needed
# it may not have shipped yet.
printf 'm|*|title|*\nm|*|nosuch|*\n' > allow_stale.txt
check stale 0 "$PY" --baseline new_b.txt --head new_h.txt \
  --allow allow_stale.txt --cases 2
says stale 'nosuch'

# The two sides must describe the same corpus, in the same order.
mk "a|1 ::: ok=Long:1" > short.txt
check count-mismatch 1 "$PY" --baseline short.txt --head same_h.txt \
  --allow allow_empty.txt --cases 2
says count-mismatch "record count differs"
mk "z|9 ::: ok=Long:1" "b|2 ::: ok=Long:2" > step.txt
check out-of-step 1 "$PY" --baseline step.txt --head same_h.txt \
  --allow allow_empty.txt --cases 2
says out-of-step "out of step"

# What a case printed belongs to the case: two records that match on their
# result line and differ in their output are a difference.
mk "p|1 ::: ok=Nil:nil" "hello" > out_b.txt
mk "p|1 ::: ok=Nil:nil" "goodbye" > out_h.txt
check trailing-output 1 "$PY" --baseline out_b.txt --head out_h.txt \
  --allow allow_empty.txt --cases 1
says trailing-output "1 changed"

# `unsupported` is what the per-case fallback writes for a case the baseline
# cannot parse; it is an ordinary difference and an allowlist entry covers it.
mk "n|1 ::: unsupported" > unsup_b.txt
mk "n|1 ::: ok=Long:1" > unsup_h.txt
printf 'n|*\n' > allow_n.txt
check unsupported 0 "$PY" --baseline unsup_b.txt --head unsup_h.txt \
  --allow allow_n.txt --cases 1
says unsupported "1 changed, 1 allowed"

# The head binary generated the corpus, so a case it cannot run is a failure
# rather than a change, and no allowlist entry covers it.
check head-unsupported 1 "$PY" --baseline unsup_h.txt --head unsup_b.txt \
  --allow allow_n.txt --cases 1
says head-unsupported "could not run"

# A label is a fragment of culebra source, so it is full of brackets. Only `*`
# and `?` are wildcards — under fnmatch's reading, `[1, 2, 3]` is a character
# class and the first entry below would swallow the second case as well.
mk "kw|[1, 2, 3].sorted(bad: 1) ::: err=A" "kw|1.sorted(bad: 1) ::: err=A" \
  > brk_b.txt
mk "kw|[1, 2, 3].sorted(bad: 1) ::: err=B" "kw|1.sorted(bad: 1) ::: err=B" \
  > brk_h.txt
printf 'kw|[1, 2, 3].sorted(bad: 1)\n' > allow_brk.txt
check literal-brackets 1 "$PY" --baseline brk_b.txt --head brk_h.txt \
  --allow allow_brk.txt --cases 2
says literal-brackets "2 changed, 1 allowed, 1 unlisted"

if [ "$fails" != 0 ]; then
  echo "release-diff selftest: $fails case(s) failed" >&2
  exit 1
fi
echo "release-diff selftest OK (10 cases)"
