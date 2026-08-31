#!/usr/bin/env bash
# What the compiler RESOLVED, read off the emitted bytecode.
#
# These are the facts no behavioural test can see: a resolution that stops
# happening produces the same output through the dynamic arm it falls back
# to, so the whole suite stays green while the optimization is gone. The
# regression that prompted this file did exactly that — a revocation trigger
# widened to cover the constructor grant, which made a class declaration
# revoke the grant it had just made, and every `C.new(...)` in the language
# quietly went back to a dynamic call with nothing to show for it.
#
# Usage: resolve_shape_test.sh <culebra-binary>
set -uo pipefail

BIN="${1:-}"
if [[ -z "$BIN" ]]; then
  echo "usage: resolve_shape_test.sh <culebra-binary>" >&2
  exit 2
fi

fail=0
tmp="${TMPDIR:-/tmp}/resolve_shape_$$.cul"
trap 'rm -f "$tmp"' EXIT

# check <name> <source> <grep-ERE> <expected-count>
check() {
  local name="$1" src="$2" pat="$3" want="$4" got
  printf '%s\n' "$src" >"$tmp"
  got=$("$BIN" --vm-dump "$tmp" 2>&1 | grep -cE -e "$pat")
  if [[ "$got" != "$want" ]]; then
    echo "FAIL [$name]: '$pat' matched $got, expected $want" >&2
    fail=1
  fi
}

# A class declaration's own store must not revoke the constructor grant it
# just made: `C.new(...)` after the declaration resolves to one chunk.
check "ctor resolves after its declaration" \
'class Ok {
  x: Float
  new(x: Float) { self.x = x }
}
Ok.new(1.0).x' \
'CallM.*-> chunk' 1

# A member constructing its own class resolves too, behind the guard its
# receiver-read name owes.
check "own-class construction is guarded" \
'class Ok {
  x: Float
  new(x: Float) { self.x = x }
  twin() { Ok.new(self.x) }
}
Ok.new(1.0).twin().x' \
'-> chunk .*\(guarded\)' 1

# An overload set makes the constructor a dispatcher rather than a chunk, so
# nothing resolves — the negative half, which is what keeps the positive
# checks from passing for the wrong reason.
check "overloaded ctor resolves nothing" \
'class Over {
  new() { self.a = 0 }
  new(a) { self.a = a }
}
Over.new().a' \
'CallM.*-> chunk' 0

if [[ $fail -eq 0 ]]; then echo "resolve_shape_test OK"; exit 0; fi
exit 1
