#!/usr/bin/env bash
# Run one of examples/languages' front ends over its samples, and compare each
# against the oracle for that language. The three differ only in what to run
# and what to believe, so they are one script with one arm each:
#
#   pl0           the tree-walking interpreter in pl0.cul beside it, which is
#                 an independent second implementation of the same language
#   mini-js       `node`. The samples are plain JavaScript, and fmt.js is the
#                 one formatting function both sides print through, so the
#                 comparison is the language rather than console.log's own
#                 rendering
#   mini-culebra  culebra. The samples are plain culebra.
#   mini-go       the output `go run` gives, frozen beside each sample --
#                 the oracle is Go itself, recorded rather than re-run.
#   mini-lua      `lua` itself, which is installed here -- the real
#                 implementation rather than a transcript of it.
#   mini-csharp   the output `dotnet run` gives, frozen beside each sample.
#                 A run costs several seconds, which is why it is recorded.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end -- and its culebra-side oracle -- on.
ENGINE=${ENGINE:---vm}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
export CULEBRA ENGINE

case "${1:-}" in
  pl0)
    set -- examples/languages/pl0/samples/*.pas
    # PL/0 reads from stdin, so a sample may bring its own
    pl0_run() {
      local in=${1%.pas}.stdin
      [ -e "$in" ] || in=/dev/null
      "$CULEBRA" "$ENGINE" "$2" "$1" <"$in"
    }
    got() { pl0_run "$1" examples/languages/pl0/pl0_codegen.cul; }
    want() { pl0_run "$1" examples/languages/pl0/pl0.cul; }
    export -f pl0_run
    ;;
  mini-js)
    set -- examples/languages/mini-js/samples/*.js
    LIB=${LIB:-examples/languages/mini-js/fmt.js}
    export LIB
    got() {
      "$CULEBRA" "$ENGINE" examples/languages/mini-js/mini_js.cul \
        --lib "$LIB" "$1"
    }
    want() { node -e "$(cat "$LIB"; cat "$1")"; }
    ;;
  mini-culebra)
    set -- examples/languages/mini-culebra/samples/*.cul
    got() {
      "$CULEBRA" "$ENGINE" \
        examples/languages/mini-culebra/mini_culebra.cul "$1"
    }
    want() { "$CULEBRA" "$ENGINE" "$1"; }
    ;;
  mini-go)
    set -- examples/languages/mini-go/samples/*.go
    got() { "$CULEBRA" "$ENGINE" examples/languages/mini-go/mini_go.cul "$1"; }
    want() { cat "${1%.go}.txt"; }
    ;;
  mini-lua)
    set -- examples/languages/mini-lua/samples/*.lua
    LUA=${LUA:-lua}
    export LUA
    got() {
      "$CULEBRA" "$ENGINE" examples/languages/mini-lua/mini_lua.cul "$1"
    }
    want() { "$LUA" "$1"; }
    ;;
  mini-csharp)
    set -- examples/languages/mini-csharp/samples/*.cs
    got() {
      "$CULEBRA" "$ENGINE" examples/languages/mini-csharp/mini_csharp.cul "$1"
    }
    want() { cat "${1%.cs}.txt"; }
    ;;
  *)
    echo "usage: $0 pl0|mini-js|mini-culebra|mini-go|mini-lua|mini-csharp" >&2
    exit 2
    ;;
esac

# An empty glob leaves the pattern itself in $1, and a comparison of two
# identical "no such file" errors would pass. Say so instead.
if [ ! -e "$1" ]; then
  echo "$0: $1 matched no samples" >&2
  exit 2
fi

one() {
  local f=$1 g w
  g=$(got "$f" 2>&1)
  w=$(want "$f" 2>&1)
  if [ "$g" = "$w" ]; then
    echo "OK   $(basename "$f")"
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$w") <(printf '%s\n' "$g") | head -20
    return 1
  fi
}
export -f got want one

# xargs answers 123 when any child failed; normalize it to a plain 1.
printf '%s\n' "$@" | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {} || exit 1
