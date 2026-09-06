#!/usr/bin/env bash
# Five small programs -- startup (prints a constant), fib(28) (recursion),
# loop (a tight arithmetic loop), array (building then indexing a
# container), strings (naive repeated concatenation) -- run on both culebra
# engines and on seven other language runtimes.
#
# `just perf` measures the VM against the JIT, which cannot say whether
# either is fast in absolute terms. This says it, in the only unit a reader
# already has a feel for: how long the same program takes under python3,
# ruby, lua, guile, node, dotnet and go on the same machine.
#
# `startup` is its own row because process startup is uneven across these
# runtimes -- a bare "print a constant" costs a compiled Go binary about a
# millisecond and python3 tens of them. Every other row includes that same
# tax, so reading a row next to `startup` is what separates "slow here" from
# "slow to start".
#
# loop and array reduce their accumulator by `% 1000000007` to keep the
# answer inside a JS number's 53-bit exact range at the sizes below; every
# element stored is still the real square. The Scheme program sums a list by
# tail recursion rather than indexing a vector, and the Go programs spell out
# `int64` / `string` because Go is the one static language here -- both are
# the shape those languages reach for, not a different benchmark.
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

# Answer each workload prints, so a wrong-but-fast number cannot pass as a
# result. A mismatch warns on stderr and still reports its time.
expected() {
    case $1 in
        startup) echo 1 ;;
        fib) echo 317811 ;;
        loop) echo 968194995 ;;
        array) echo 375082463 ;;
        strings) echo 40000 ;;
    esac
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
GO_DIR="$WORK/go"
CS_DIR="$WORK/cs"
mkdir -p "$GO_DIR" "$CS_DIR"

cul_vm() { "$BIN" --vm "$1.cul"; }
cul_jit() { "$BIN" --jit "$1.cul"; }
run_python() { command -v python3 >/dev/null && python3 "$1.py"; }
run_ruby() { command -v ruby >/dev/null && ruby "$1.rb"; }
run_lua() {
    local l
    l=$(command -v lua5.4 || command -v lua || true)
    [ -n "$l" ] && "$l" "$1.lua"
}
# guile compiles a script it is given on first sight and says so on stderr;
# --no-auto-compile keeps the run interpreted and the note out of it.
run_guile() { command -v guile >/dev/null && guile --no-auto-compile -s "$1.scm"; }
run_node() { command -v node >/dev/null && node "$1.js"; }
run_dotnet() {
    local dll="$CS_DIR/$1/out/s.dll"
    [ -f "$dll" ] || return 1
    dotnet "$dll"
}
run_go() {
    local exe="$GO_DIR/$1"
    [ -x "$exe" ] || return 1
    "$exe"
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

# Wall seconds for $1 back-to-back runs of "$2 $3". bash's own `time` reports
# milliseconds, where /usr/bin/time -p reports hundredths.
batch_seconds() {
    local n=$1 fn=$2 w=$3
    local TIMEFORMAT='%R'
    {
        time {
            local i=0
            while [ "$i" -lt "$n" ]; do
                "$fn" "$w" >/dev/null 2>&1 || true
                i=$((i+1))
            done
        }
    } 2>&1
}

# Milliseconds per run, best of REPS batches. The repetition count grows
# until a batch takes 0.1s, so a runtime that starts in two milliseconds is
# measured rather than rounded.
per_run_ms() {
    local fn=$1 w=$2 n=1 real best i
    while :; do
        real=$(batch_seconds "$n" "$fn" "$w")
        awk -v r="$real" 'BEGIN { exit !(r + 0 >= 0.1) }' && break
        [ "$n" -ge 512 ] && break
        n=$((n * 8))
    done
    best=$real
    i=1
    while [ "$i" -lt "$REPS" ]; do
        real=$(batch_seconds "$n" "$fn" "$w")
        best=$(awk -v a="$best" -v b="$real" 'BEGIN { print (b + 0 < a + 0) ? b : a }')
        i=$((i + 1))
    done
    awk -v r="$best" -v n="$n" 'BEGIN { printf "%.0f", r * 1000 / n }'
}

# One run to check the answer, then the timed batches.
cell() {
    local fn=$1 w=$2 out want
    if ! out=$("$fn" "$w" 2>/dev/null); then
        echo "-"
        return
    fi
    want=$(expected "$w")
    if [ "$out" != "$want" ]; then
        echo "langs: $fn($w) printed '$out', expected $want" >&2
    fi
    echo "$(per_run_ms "$fn" "$w")ms"
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

# shellcheck disable=SC2086 # $WORKLOADS is a deliberate word list
printf '%-14s %9s %9s %9s %9s %9s\n' runtime $WORKLOADS
printf '%-14s %9s %9s %9s %9s %9s\n' -------- ------- ------- ------- ------- -------
printf '%s\n' "$ROWS" | while IFS='|' read -r label fn; do
    line=$(printf '%-14s' "$label")
    for w in $WORKLOADS; do
        line="$line $(printf '%9s' "$(cell "$fn" "$w")")"
    done
    printf '%s\n' "$line"
done
