#!/usr/bin/env bash
# Five small programs -- startup (prints a constant), fib (recursion), loop
# (a tight arithmetic loop), array (building then indexing a container),
# strings (naive repeated concatenation) -- run on both culebra engines and
# on seven other language runtimes.
#
# `just perf` measures the VM against the JIT, which cannot say whether
# either is fast in absolute terms. This says it, in the only unit a reader
# already has a feel for: how long the same program takes under python3,
# ruby, lua, guile, node, dotnet and go on the same machine.
#
# Two tables. "wall" is one run of each program at its base size, start to
# exit: what running a short script costs. At these sizes a runtime's
# fixed costs -- process startup, and a JIT's compile before the first
# instruction runs -- are a large share of the number, and they are uneven
# across runtimes (a Go binary starts in about a millisecond, python3 in
# tens of them). "compute" takes them out: every program also runs at a
# larger size, and the difference between the two runs, scaled back to the
# base size's work, is what the base-size workload costs once running. A
# fixed cost is in both runs and cancels; so does a JIT's warmup, since
# the size never reaches the compiler (below). The two tables read
# together: a runtime that is slow in "wall" but not in "compute" is slow
# to start, not slow here. `startup` has no size and appears in "wall"
# only.
#
# Every program reads its size from the command line rather than naming it
# in the source: a constant size is one an optimizing compiler may fold into
# the loop, which would time the compiler's arithmetic instead of the
# runtime's. Every program is the same loop in each language -- array reads
# its container's length on every step in all of them -- except where the
# language's own shape differs: the Scheme programs recurse instead of
# looping and sum a list rather than indexing a vector, and the Go programs
# spell out `int64` / `string` because Go is the one static language here.
#
# loop and array reduce their accumulator by `% 1000000007` to keep the
# answer inside a JS number's 53-bit exact range at the sizes below; every
# element stored is still the real square.
#
# C# and Go compile once per workload, outside the timed loop: timing `dotnet
# build` or `go run` reports a compiler, not a runtime. A runtime that is not
# installed is reported as "-" rather than failing the table.
#
# Numbers from a shared CI runner are noisy -- this is a report, not a gate.
#
# Usage:
#   tools/bench/langs/run.sh [--reps N]
#   CULEBRA=./build-dev/culebra tools/bench/langs/run.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
BIN=${CULEBRA:-"$ROOT/build/culebra"}
REPS=3
if [ "${1:-}" = "--reps" ]; then REPS=$2; fi

if [ ! -x "$BIN" ]; then
    echo "langs: culebra binary not found at $BIN -- run 'just build' first" >&2
    exit 2
fi

WORKLOADS="startup fib loop array strings"

# Base size, then the larger size "compute" differences against. The larger
# size is picked so the slowest runtime here still finishes in seconds.
base_size() {
    case $1 in
        fib) echo 28 ;;
        loop) echo 1200000 ;;
        array) echo 500000 ;;
        strings) echo 40000 ;;
    esac
}
big_size() {
    case $1 in
        fib) echo 32 ;;
        loop) echo 12000000 ;;
        array) echo 5000000 ;;
        strings) echo 80000 ;;
    esac
}

# How much work a size is, in any unit that is the same for both sizes: the
# calls fib(n) makes, the steps of a loop. strings counts its appends, and
# its big size is twice the base, so its "compute" is the second half's
# appends as they are -- how their cost grows is the runtime's own (a
# string copied per append grows with its length, one appended in place
# does not), so no growth model is assumed for it.
work() {
    case $1 in
        fib) awk -v n="$2" 'BEGIN { a = 0; b = 1
                                    for (i = 0; i <= n; i++) { t = a + b; a = b; b = t }
                                    print 2 * a - 1 }' ;;
        *) echo "$2" ;;
    esac
}

# Answer each workload prints at a size, so a wrong-but-fast number cannot
# pass as a result. A mismatch warns on stderr and still reports its time.
expected() {
    case $1:${2:-} in
        startup:) echo 1 ;;
        fib:28) echo 317811 ;;
        fib:32) echo 2178309 ;;
        loop:1200000) echo 968194995 ;;
        loop:12000000) echo 1524224 ;;
        array:500000) echo 375082463 ;;
        array:5000000) echo 747875 ;;
        strings:*) echo "$2" ;;
    esac
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
GO_DIR="$WORK/go"
CS_DIR="$WORK/cs"
mkdir -p "$GO_DIR" "$CS_DIR"

# Each runner takes the workload's name, then its size (absent for startup).
cul_vm() { "$BIN" --vm "$1.cul" "${@:2}"; }
cul_jit() { "$BIN" --jit "$1.cul" "${@:2}"; }
run_python() { command -v python3 >/dev/null && python3 "$1.py" "${@:2}"; }
run_ruby() { command -v ruby >/dev/null && ruby "$1.rb" "${@:2}"; }
run_lua() {
    local l
    l=$(command -v lua5.4 || command -v lua || true)
    [ -n "$l" ] && "$l" "$1.lua" "${@:2}"
}
# guile compiles a script it is given on first sight and says so on stderr;
# --no-auto-compile keeps the run interpreted and the note out of it.
run_guile() {
    command -v guile >/dev/null && guile --no-auto-compile -s "$1.scm" "${@:2}"
}
run_node() { command -v node >/dev/null && node "$1.js" "${@:2}"; }
run_dotnet() {
    local dll="$CS_DIR/$1/out/s.dll"
    [ -f "$dll" ] || return 1
    dotnet "$dll" "${@:2}"
}
run_go() {
    local exe="$GO_DIR/$1"
    [ -x "$exe" ] || return 1
    "$exe" "${@:2}"
}

# Compiles "$1.go". A missing toolchain or a failed build leaves nothing
# behind, which run_go reports as "not installed". `go build` names its file
# explicitly, so this needs no go.mod.
build_go() {
    command -v go >/dev/null || return 0
    go build -o "$GO_DIR/$1" "$1.go" >/dev/null 2>&1 || true
}

# Same, into a scratch project's Release output.
build_csharp() {
    command -v dotnet >/dev/null || return 0
    local dir="$CS_DIR/$1"
    mkdir -p "$dir"
    cp "$1.cs" "$dir/Program.cs"
    cat > "$dir/s.csproj" <<'EOP'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <AssemblyName>s</AssemblyName>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
</Project>
EOP
    dotnet build "$dir/s.csproj" -c Release -o "$dir/out" >/dev/null 2>&1 || true
}

# Wall seconds for $1 back-to-back runs of "$2 $3 [$4]". bash's own `time`
# reports milliseconds, where /usr/bin/time -p reports hundredths.
batch_seconds() {
    local n=$1 fn=$2
    shift 2
    local TIMEFORMAT='%R'
    {
        time {
            local i=0
            while [ "$i" -lt "$n" ]; do
                "$fn" "$@" >/dev/null 2>&1 || true
                i=$((i+1))
            done
        }
    } 2>&1
}

# Milliseconds per run of "$1 $2 [$3]", best of REPS batches. The repetition
# count grows until a batch takes 0.1s, so a runtime that starts in two
# milliseconds is measured rather than rounded.
per_run_ms() {
    local fn=$1 n=1 real best i
    shift
    while :; do
        real=$(batch_seconds "$n" "$fn" "$@")
        awk -v r="$real" 'BEGIN { exit !(r + 0 >= 0.1) }' && break
        [ "$n" -ge 512 ] && break
        n=$((n * 8))
    done
    best=$real
    i=1
    while [ "$i" -lt "$REPS" ]; do
        real=$(batch_seconds "$n" "$fn" "$@")
        best=$(awk -v a="$best" -v b="$real" 'BEGIN { print (b + 0 < a + 0) ? b : a }')
        i=$((i + 1))
    done
    awk -v r="$best" -v n="$n" 'BEGIN { printf "%.2f", r * 1000 / n }'
}

# One run to check the answer, then the timed batches. Prints milliseconds,
# or "-" when the runtime is not installed.
measure() {
    local fn=$1 w=$2 size=${3:-} out want
    local args=("$w")
    [ -n "$size" ] && args+=("$size")
    if ! out=$("$fn" "${args[@]}" 2>/dev/null); then
        echo "-"
        return
    fi
    want=$(expected "$w" "$size")
    if [ "$out" != "$want" ]; then
        echo "langs: $fn(${args[*]}) printed '$out', expected $want" >&2
    fi
    per_run_ms "$fn" "${args[@]}"
}

# The base size's cost with the fixed costs differenced out: the extra time
# the big run took, per unit of the extra work it did, times the base's work.
compute_ms() {
    local w=$1 base=$2 big=$3 wb wg
    [ "$base" = "-" ] || [ "$big" = "-" ] && { echo "-"; return; }
    wb=$(work "$w" "$(base_size "$w")")
    wg=$(work "$w" "$(big_size "$w")")
    awk -v a="$base" -v b="$big" -v wb="$wb" -v wg="$wg" \
        'BEGIN { printf "%.2f", (b - a) * wb / (wg - wb) }'
}

# Whole milliseconds, with one decimal under 10 so a fast cell is not all
# rounding; a difference lost in noise reads as "~0".
fmt_ms() {
    case $1 in
        -) echo "-" ;;
        *) awk -v v="$1" 'BEGIN {
               if (v < 0.05) print "~0"
               else if (v < 10) printf "%.1fms\n", v
               else printf "%.0fms\n", v }' ;;
    esac
}

for w in $WORKLOADS; do
    build_go "$w"
    build_csharp "$w"
done

# label|function, culebra first and the rest in the order the table reads.
ROWS="culebra --vm|cul_vm
culebra --jit|cul_jit
python3|run_python
ruby|run_ruby
lua|run_lua
guile|run_guile
node|run_node
dotnet|run_dotnet
go|run_go"

COMPUTED="fib loop array strings"
WALL_TABLE=""
COMPUTE_TABLE=""
while IFS='|' read -r label fn; do
    wall=$(printf '%-14s' "$label")
    comp=$wall
    for w in $WORKLOADS; do
        if [ "$w" = startup ]; then
            wall="$wall $(printf '%9s' "$(fmt_ms "$(measure "$fn" "$w")")")"
            continue
        fi
        base=$(measure "$fn" "$w" "$(base_size "$w")")
        big=$(measure "$fn" "$w" "$(big_size "$w")")
        wall="$wall $(printf '%9s' "$(fmt_ms "$base")")"
        comp="$comp $(printf '%9s' "$(fmt_ms "$(compute_ms "$w" "$base" "$big")")")"
    done
    WALL_TABLE+="$wall"$'\n'
    COMPUTE_TABLE+="$comp"$'\n'
    # Progress on stderr: a full run takes minutes.
    echo "langs: $label done" >&2
done <<< "$ROWS"

# shellcheck disable=SC2086 # $WORKLOADS / $COMPUTED are deliberate word lists
{
    echo "wall: one run at the base size, startup and JIT compile included"
    printf '%-14s %9s %9s %9s %9s %9s\n' runtime $WORKLOADS
    printf '%-14s %9s %9s %9s %9s %9s\n' -------- ------- ------- ------- ------- -------
    printf '%s' "$WALL_TABLE"
    echo
    echo "compute: the same base-size work, startup and JIT compile differenced out"
    printf '%-14s %9s %9s %9s %9s\n' runtime $COMPUTED
    printf '%-14s %9s %9s %9s %9s\n' -------- ------- ------- ------- -------
    printf '%s' "$COMPUTE_TABLE"
    echo "(strings: the $(big_size strings)-append run minus the $(base_size strings)-append run)"
}
