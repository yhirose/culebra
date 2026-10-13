#!/usr/bin/env bash
# Thrown-value discipline gate.
#
# A script's `throw <value>` crosses C++ frames as a culebra::CulebraException.
# What it carries is the program's value, and only the engine's own boundaries
# know what to do with one: report it as `uncaught: ...`, hand it to a `catch`,
# or turn it into the structured error a host reads. Native code that stopped
# one anywhere else has got it wrong three times here (Parallel, Http.server and
# Net.serve; the Search analyzer hook; the REPL echo, the embedding API's
# subscripts and the debugger's variable display), each found by reading code
# or by a crash, never by a test.
#
# The rule is carried by the type, as it is for an interrupt
# (check_interrupt_discipline.sh): CulebraException derives from nothing, so a
# handler written for a library's failure (`catch (const std::exception&)`)
# cannot take a thrown value by accident. What the type cannot close is
# `catch (...)`, which takes anything, so this checks those, over the whole
# tree. In each chain, the first handler that can take a thrown value must do
# one of:
#
#   - name CulebraException, and so answer for one deliberately     EXPLICIT
#   - re-throw everything (`throw;`)                                RETHROWS
#   - end the process                                               TERMINATES
#   - carry `// thrown value: <what becomes of one here>`           DOCUMENTED
#
# The note says where the value goes (kept and thrown again, converted by
# culebra_runtime_call_from_host before it gets here, dropped on purpose), or
# why none can arrive. It sits at the handler for the reason the interrupt
# notes do: what makes a handler right is usually a fact about another file.
set -euo pipefail
cd "$(dirname "$0")/../.."

gate=thrown-value-discipline
carrier=CulebraException
note="thrown value"
behind=std::exception
source tools/checks/catch_discipline.sh

# Every source of the engine's own that has a handler. A new file is on the
# list by having one.
files=()
while IFS= read -r f; do files+=("$f"); done < <(
  grep -rlE 'catch[[:space:]]*\(' include src playground \
    --include='*.h' --include='*.cc' | sort)
[[ ${#files[@]} -gt 0 ]] || { echo "$gate: no sources found" >&2; exit 1; }

chains="$(catch_chains "${files[@]}")"

bad="$(swallowing_chains "$chains")"
if [[ -n "$bad" ]]; then
  echo "thrown-value-discipline: handler(s) can stop a script's thrown value:" >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  echo >&2
  echo "Run the script's code through culebra_runtime_call_from_host (or an" >&2
  echo "engine boundary) so a thrown value arrives as an error, name" >&2
  echo "CulebraException in a handler of its own, or add a" >&2
  echo "'// thrown value: <what becomes of one here>' note at the handler." >&2
  exit 1
fi

require_swallow_probes
report_ok "$chains"
