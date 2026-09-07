#!/usr/bin/env bash
# mini-lua against `lua` itself. Lua 5.4 is installed here, so the oracle is
# the real implementation rather than a frozen transcript of it.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
ENGINE=${ENGINE:---vm}
LUA=${LUA:-lua}
ok=0
n=0
for f in examples/languages/mini-lua/samples/*.lua; do
  n=$((n + 1))
  got=$("$CULEBRA" "$ENGINE" examples/languages/mini-lua/mini_lua.cul "$f" 2>&1)
  want=$("$LUA" "$f" 2>&1)
  if [ "$got" = "$want" ]; then
    echo "OK   $(basename "$f")"
    ok=$((ok + 1))
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -6
  fi
done
echo "$ok/$n"
