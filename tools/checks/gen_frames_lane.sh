#!/usr/bin/env bash
# Generators that keep their frame (CULEBRA_GEN_FRAMES=1), on every engine,
# for as long as that is a flag: the gate's other lanes run the default, in
# which a generator is lowered to a state class, so nothing else here reaches
# Op::GenStart / Op::Yield, the frame runtime (include/rt/gen.inc.h) or the
# collector's view of a suspended frame. When frames are the default this
# lane's files are ordinary corpus and the lane goes.
#
# Two populations, both run with the flag set:
#
#   tests/gen_frames/*.cul  probes held to a frozen output
#                           (tests/gen_frames/expected/<name>.out). Print-based
#                           on purpose: some of what they show the lowered
#                           class answers differently (what a generator is as
#                           a value, when a local of the body is dropped), so
#                           they cannot sit in tests/*.cul, which every lane
#                           runs in the default.
#   tests/*.cul that yield  the corpus files the compiler makes a frame
#                           generator in, held by their assertions: exit 0,
#                           and --jit prints what the executor prints.
#
# Usage: gen_frames_lane.sh <culebra> [probes|dev|full]
#        gen_frames_lane.sh --freeze <culebra> [probe.cul ...]
#
#   probes  the probes on the executor and --jit
#   dev     the landing gate's share: the probes on both engines, the corpus
#           files on the executor, and on --jit the ones that are about
#           generators (tests/test_generator*.cul). The rest cost on --jit
#           what their other contents cost (an effect body's lowering, mostly:
#           21 of the 32 seconds all twenty took, for one generator), which
#           is the corpus sweep's reason for a cover in that gate too.
#   full    what `dev` leaves: the other corpus files on --jit, and the
#           collector's axes. Those are a collection at every allocation
#           (CULEBRA_GC_STRESS: an edge a frame does not report frees a live
#           object), every collection refcount-seeded (CULEBRA_GC_REFS +
#           stress: an edge it reports and does not own does), and the
#           collector off under the leak audit (CULEBRA_GC_NEVER +
#           CULEBRA_GC_LEAK_ABORT). The probes and the generator files take
#           them on both engines, the other corpus files on the executor: a
#           --jit run pays its compile again on every axis. And the probes
#           through `culebra build`, plain and under the first two.
#
# A probe's output is the same under every axis but the last: two of them
# count what a collection reclaims, so with the collector off only their exit
# status is held. `--freeze` rewrites the expected output from an executor
# run and then asserts --jit agrees; the diff review is where the change of
# behaviour is checked.
set -uo pipefail
cd "$(dirname "$0")/../.."

FREEZE=0
if [[ "${1:-}" == --freeze ]]; then FREEZE=1; shift; fi
BIN="${1:?usage: gen_frames_lane.sh [--freeze] <culebra> [probes|dev|full]}"
[[ -x "$BIN" ]] || { echo "gen-frames: no executable $BIN" >&2; exit 1; }
shift

export CULEBRA_GEN_FRAMES=1
export CULEBRA_CANVAS_HEADLESS="${CULEBRA_CANVAS_HEADLESS:-1}"
export CULEBRA_AUDIO="${CULEBRA_AUDIO:-off}"
dir=tests/gen_frames

if [[ -z "${TIMEOUT_BIN+x}" ]]; then
  TIMEOUT_BIN=""
  if command -v timeout > /dev/null 2>&1; then TIMEOUT_BIN=timeout
  elif command -v gtimeout > /dev/null 2>&1; then TIMEOUT_BIN=gtimeout; fi
fi
limit="${CULEBRA_TEST_TIMEOUT:-300}"
# One culebra run, or one built binary's, bounded like the gate's others.
bounded() { ${TIMEOUT_BIN:+$TIMEOUT_BIN "$limit"} "$@"; }
# What a run printed, with the line ends the frozen files have: a Windows
# build writes CRLF. Through tr, as the other Windows checks do it.
no_cr() { printf '%s\n' "$1" | tr -d '\r'; }
# Two files that should be equal. The Windows runner has no diffutils, and a
# failure that cannot say what differed is no report.
show_diff() {
  if command -v diff > /dev/null 2>&1; then diff "$1" "$2"
  else echo "--- $1"; cat "$1"; echo "--- $2"; cat "$2"; fi
}
export -f bounded no_cr show_diff
export BIN TIMEOUT_BIN limit dir

if (( FREEZE )); then
  files=("$@")
  (( ${#files[@]} > 0 )) || files=("$dir"/*.cul)
  mkdir -p "$dir/expected"
  for f in "${files[@]}"; do
    n=$(basename "$f" .cul)
    out=$(bounded "$BIN" --vm "$dir/$n.cul" 2>&1); rc=$?
    out=$(no_cr "$out")
    [[ $rc -eq 0 ]] || { echo "FREEZE-FAIL $n.cul: exit $rc (a probe ends cleanly)" >&2; exit 1; }
    printf '%s\n' "$out" > "$dir/expected/$n.out"
    out_j=$(bounded "$BIN" --jit "$dir/$n.cul" 2>&1); rc=$?
    out_j=$(no_cr "$out_j")
    if [[ $rc -ne 0 || "$out_j" != "$out" ]]; then
      echo "FREEZE-FAIL $n.cul: --jit disagrees with the fresh executor output" >&2
      exit 1
    fi
    echo "froze $n.cul"
  done
  exit 0
fi

mode="${1:-dev}"
case "$mode" in probes|dev|full) ;; *)
  echo "gen-frames: unknown mode '$mode' (probes|dev|full)" >&2; exit 2 ;;
esac

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-genframes.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
export work

probes=("$dir"/*.cul)
[[ -f "${probes[0]}" ]] || { echo "gen-frames: no probe under $dir" >&2; exit 1; }
# The corpus files the flag changes: those the compiler makes a frame
# generator in. The word is a cheap first cut (a `yield` in a string, or in an
# effect body alone, makes none), the bytecode the answer.
corpus=()
if [[ "$mode" != probes ]]; then
  cand=$(grep -lE '(^|[^A-Za-z0-9_])yield([^A-Za-z0-9_]|$)' tests/*.cul)
  [[ -n "$cand" ]] || { echo "gen-frames: no tests/*.cul file yields" >&2; exit 1; }
  CULEBRA="$BIN" bash tools/checks/dump_ops.sh $cand > "$work/dumped"
  # dump_ops.sh names a file that does not compile and goes on: with the flag
  # set that is this lane's failure, not one generator file fewer.
  [[ $(wc -l < "$work/dumped") -eq $(wc -l <<< "$cand") ]] \
    || { echo "gen-frames: a tests/*.cul file that yields does not compile with the flag" >&2; exit 1; }
  while read -r f _; do corpus+=("$f"); done < <(grep ' GenStart ' "$work/dumped")
  (( ${#corpus[@]} > 0 )) || { echo "gen-frames: no tests/*.cul file makes a frame generator" >&2; exit 1; }
fi

# The environment one axis adds. The runtime checks these for presence.
axis_env() {
  case "$1" in
    plain)  ;;
    stress) echo CULEBRA_GC_STRESS=1 ;;
    refs)   echo CULEBRA_GC_REFS=1 CULEBRA_GC_STRESS=1 ;;
    leak)   echo CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ;;
  esac
}
export -f axis_env

# job: <kind> <file> <lane> <axis>. Every job writes one .log; a .fail beside
# it is a failure, and a job with neither never ran.
run_job() {
  local kind="$1" f="$2" lane="$3" axis="$4" n id out rc want
  n=$(basename "$f" .cul)
  id="$kind.$n.${lane#--}.$axis"
  fail() { { echo "FAIL $f $lane ($axis): $1"; shift; "$@"; } > "$work/$id.log" 2>&1; : > "$work/$id.fail"; }
  case "$kind" in
    # A probe against its frozen output.
    probe)
      want="$dir/expected/$n.out"
      [[ -f "$want" ]] || { fail "no $want: freeze it (gen_frames_lane.sh --freeze <bin> $f)" true; return; }
      out=$(bounded env $(axis_env "$axis") "$BIN" "$lane" "$f" 2>&1); rc=$?
      out=$(no_cr "$out")
      if [[ $rc -ne 0 ]]; then fail "exit $rc" printf '%s\n' "$out"; return; fi
      if [[ "$axis" != leak && "$out" != "$(< "$want")" ]]; then
        printf '%s\n' "$out" > "$work/$id.got"
        fail "output differs from $want" show_diff "$want" "$work/$id.got"; return
      fi ;;
    # A probe built into a binary, which runs under each axis the collector
    # has in a built program.
    probe-aot)
      want="$dir/expected/$n.out"
      local exe="$work/$n.bin" a
      out=$(bounded "$BIN" build "$f" -o "$exe" 2>&1) || { fail "culebra build failed" printf '%s\n' "$out"; return; }
      for a in plain stress refs; do
        out=$(bounded env $(axis_env "$a") "$exe" 2>&1); rc=$?
        out=$(no_cr "$out")
        if [[ $rc -ne 0 || "$out" != "$(< "$want")" ]]; then
          printf '%s\n' "$out" > "$work/$id.got"
          fail "built binary ($a): exit $rc, or its output differs from $want" \
            show_diff "$want" "$work/$id.got"; rm -f "$exe"; return
        fi
      done
      rm -f "$exe" ;;
    # A corpus file on both engines: its own assertions, and one output.
    pair)
      local out_j
      out=$(bounded "$BIN" --vm "$f" 2> "$work/$id.err"); rc=$?
      if [[ $rc -ne 0 ]]; then fail "--vm exit $rc" cat "$work/$id.err"; return; fi
      out_j=$(bounded "$BIN" --jit "$f" 2> "$work/$id.err"); rc=$?
      if [[ $rc -ne 0 ]]; then fail "--jit exit $rc" cat "$work/$id.err"; return; fi
      if [[ "$out" != "$out_j" ]]; then
        printf '%s\n' "$out" > "$work/$id.vm"; printf '%s\n' "$out_j" > "$work/$id.jit"
        fail "--vm and --jit print differently" show_diff "$work/$id.vm" "$work/$id.jit"; return
      fi ;;
    # A corpus file on one engine, or under a collector axis: that it passes.
    run)
      out=$(bounded env $(axis_env "$axis") "$BIN" "$lane" "$f" 2>&1); rc=$?
      if [[ $rc -ne 0 ]]; then fail "exit $rc" printf '%s\n' "$out"; return; fi ;;
  esac
  echo "ok $f $lane ($axis)" > "$work/$id.log"
}
export -f run_job

# The corpus files that are about generators: `dev` takes them on --jit,
# `full` the rest.
about_generators() { case "$1" in tests/test_generator*) return 0 ;; esac; return 1; }

# A corpus file may opt out of the stress axes by its header, as it does for
# the gate's own gc-stress phase, and for the same reason.
stress_skipped() {
  awk '!/^#/ { exit } /^# gc-stress: skip/ { found = 1; exit } END { exit !found }' "$1"
}

jobs_file="$work/jobs"
{
  case "$mode" in
    probes|dev)
      for f in "${probes[@]}"; do
        echo "probe $f --vm plain"; echo "probe $f --jit plain"
      done
      for f in ${corpus[@]+"${corpus[@]}"}; do
        if about_generators "$f"; then echo "pair $f --both plain"
        else echo "run $f --vm plain"; fi
      done ;;
    full)
      for f in "${corpus[@]}"; do
        about_generators "$f" || echo "pair $f --both plain"
      done
      for f in "${probes[@]}"; do
        for lane in --vm --jit; do
          for axis in stress refs leak; do echo "probe $f $lane $axis"; done
        done
        echo "probe-aot $f --aot all"
      done
      for f in "${corpus[@]}"; do
        lanes=(--vm)
        about_generators "$f" && lanes+=(--jit)
        for lane in "${lanes[@]}"; do
          if ! stress_skipped "$f"; then
            echo "run $f $lane stress"; echo "run $f $lane refs"
          fi
          echo "run $f $lane leak"
        done
      done ;;
  esac
} > "$jobs_file"

want=$(wc -l < "$jobs_file" | tr -d ' ')
xargs -n4 -P "$JOBS" bash -c 'run_job "$1" "$2" "$3" "$4"' _ < "$jobs_file"

got=$(ls "$work"/*.log 2>/dev/null | wc -l | tr -d ' ')
rc=0
for fmark in "$work"/*.fail; do
  [[ -e "$fmark" ]] || continue
  cat "${fmark%.fail}.log" >&2
  rc=1
done
# Silence is not agreement: a job that left no report did not run.
if [[ "$got" != "$want" ]]; then
  echo "gen-frames: $got of $want checks reported" >&2
  rc=1
fi
(( rc == 0 )) || { echo "gen-frames FAIL ($mode)" >&2; exit 1; }
echo "gen-frames OK ($mode: ${#probes[@]} probes, ${#corpus[@]} corpus files, $want checks)"
