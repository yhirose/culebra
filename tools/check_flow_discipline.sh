#!/usr/bin/env bash
# Control-flow discipline ratchet.
#
# return / break / continue complete through the thread-local slot in
# interpreter.h (flow_pending / flow_set_* / flow_take_return), not exceptions. The
# guard at the top of eval() makes that safe for call sites that simply pass a
# value along: once a return is pending, eval() hands back nil and the
# completion travels up on its own.
#
# It is NOT safe for a site that immediately *converts* the result — to_bool /
# to_long / to_string raise a TypeError on nil, so a pending return would
# surface as a bogus type error instead of returning. Those sites must go
# through eval_operand(), which reports the pending completion to its caller.
#
# This gate counts the raw `eval(...).to_X()` forms. The count may only shrink:
# lower a ceiling when you convert a site, never raise one without review.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
ratchet() { # name actual ceiling hint
  if (( $2 > $3 )); then
    echo "flow-discipline FAIL: $1 = $2 (ceiling $3)" >&2
    echo "  $4" >&2
    fail=1
  fi
}

# Direct `eval(...).to_bool()/.to_long()/.to_string()` in the interpreter: a
# pending `return` would be converted instead of propagated. The one remaining
# site is deliver_call(fn.eval(env)).to_bool() in _invoke_predicate, where the
# completion has already been consumed at the call boundary.
conv=$(grep -cE "eval\([^;]*\)\.to_(bool|long|string)\(" include/interpreter.h || true)
ratchet "raw eval().to_X() conversions (interpreter.h)" "$conv" 1 \
  "Use eval_operand(ast, env, out) and return early when it reports pending."

# Control-flow exceptions. `return` no longer throws; break/continue still do
# (they are caught in run_loop_body, the single place that pairs a loop body
# with its defers). ReturnValue must stay unused — a new throw site would
# bypass every boundary that now reads the slot instead of catching.
ret_throw=$(grep -c "throw ReturnValue" include/interpreter.h || true)
ratchet "throw ReturnValue sites" "$ret_throw" 0 \
  "return completes via flow_set_return(); it is not an exception any more."

ret_catch=$(grep -c "catch (const ReturnValue" include/interpreter.h || true)
ratchet "catch ReturnValue sites" "$ret_catch" 0 \
  "Use deliver_call(f.eval(env)) at a call boundary instead."

# break / continue are consumed in run_loop_body, the single place that pairs a
# loop body with its defers. Keeping this at zero stops a handler block from
# being copied back into a loop, which is what made a missing run_deferred
# possible before.
sig=$(grep -cE "BreakSignal|ContinueSignal" include/interpreter.h || true)
ratchet "Break/ContinueSignal references" "$sig" 0 \
  "break/continue complete via flow_set_break() / flow_set_continue()."

# run_deferred must stay reachable from one guarded place per scope kind. A new
# bare call usually means a hand-rolled scope that will miss an exit path.
deferred=$(grep -c "run_deferred(" include/interpreter.h || true)
ratchet "run_deferred call sites (interpreter.h)" "$deferred" 19 \
  "Prefer run_loop_body()/existing scope helpers over a new handler block."

if (( fail )); then
  echo "flow-discipline FAILED" >&2
  exit 1
fi
echo "flow-discipline OK (conv=$conv/1 ret-throw=$ret_throw/0 ret-catch=$ret_catch/0" \
     "signals=$sig/0 deferred=$deferred/19)"
