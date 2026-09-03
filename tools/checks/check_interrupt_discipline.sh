#!/usr/bin/env bash
# Interrupt-discipline gate.
#
# An interrupt (Ctrl+C, or an isolate's cancel) is never a program error. When a
# component that hosts a run reports one, the press comes out as if the program
# had failed and the run carries on, which is the opposite of what was asked —
# and the flag is one-shot, so the next file or block sees nothing. That bug has
# been written three times here: `culebra test` reported an interrupted file as
# errored and ran the rest of the suite; `culebra test --doc` did the same per
# block; and the REPL needed the interrupt reported rather than thrown, which is
# how the first two got their stashes. Each was found by reading code, never by
# a test.
#
# The rule is now carried by the type: `culebra::Interrupted` derives from
# nothing, so a handler that reports errors cannot name a type that catches one.
# What the type cannot close is `catch (...)`, which takes anything — so this
# checks those, on the surface where a violation is user-visible: the files that
# host a run or a CLI analysis (kFiles below). In each chain there, the first
# handler that can take an interrupt must do one of:
#
#   - name Interrupted, and so answer for one deliberately          EXPLICIT
#   - re-throw everything (`throw;`)                                RETHROWS
#   - end the process                                               TERMINATES
#   - carry `// interrupt: <why one cannot arrive here>`            DOCUMENTED
#
# The note is deliberately a comment at the site rather than a list of line
# numbers in here: the reason a handler is safe belongs next to the handler, and
# the reasons are all facts about OTHER files (nothing under a parse polls the
# flag; the DAP lane installs no handler) that a future change could quietly
# flip. A note that has to be edited is the point.
set -euo pipefail
cd "$(dirname "$0")/../.."

# The run-host surface: everything that enters a culebra program, loads its
# modules, analyses a file for the CLI, or runs one on a thread it owns. The
# last of those is on the list for the same reason as the rest — a handler
# there answers for a press nobody else can — even where the file has no
# catch-all today, so that adding one asks the question. A new host belongs
# here.
kFiles=(
  src/main.cc
  include/cli/dap.h
  include/cli/debug_engine.h
  include/stdlib/http.h
  include/frontend/lint.h
  include/stdlib/net.h
  include/frontend/parser.h
  include/cli/repl_core.h
  include/aot/bootstrap.h
  include/conc/sendable.h
  include/cli/test_engine.h
  include/cli/test_runner.h
  include/vm/debug.h
  include/vm/embed.h
  include/vm/session.h
  playground/wasm_main.cc
)

missing=0
for f in "${kFiles[@]}"; do
  [[ -f "$f" ]] || { echo "interrupt-discipline: no $f" >&2; missing=1; }
done
[[ $missing -eq 0 ]] || exit 1

chains="$(for f in "${kFiles[@]}"; do awk -f tools/checks/interrupt_chains.awk "$f"; done)"

bad="$(printf '%s\n' "$chains" | awk -F'\t' '$3 == "SWALLOWS"')"
if [[ -n "$bad" ]]; then
  echo "interrupt-discipline: handler(s) can report an interrupt as a program error:" >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  echo >&2
  echo "Name Interrupted in a handler of its own, or add a '// interrupt: <reason>'" >&2
  echo "note saying why an interrupt cannot reach that handler." >&2
  exit 1
fi

# Positive control: the checker must be able to fail. Without this a change that
# breaks the awk (an unparsed brace, a renamed verdict) reports zero SWALLOWS on
# a tree full of them and the gate goes quietly green — the shape that let a
# one-probe shell gate miss a whole peeling pass before.
#
# Two probes, because the two ways to go blind are different. The first is a
# bare swallowing catch-all. The second puts one behind a handler of a type the
# checker does not report on: brace counting alone runs a chain together, since
# `} catch (...) {` is balanced, and then the whole chain is judged by its first
# handler and this catch-all is never seen. That was a live hole — it hid two
# chains on this very surface.
probe="$(mktemp -d "${TMPDIR:-/tmp}/culebra-interrupt.XXXXXX")"
trap 'rm -rf "$probe"' EXIT
cat > "$probe/swallow.cc" <<'EOF'
void f() {
  try {
    run();
  } catch (...) {
    report();
  }
}
EOF
cat > "$probe/behind.cc" <<'EOF'
void g() {
  try {
    run();
  } catch (const CulebraException& e) {
    report(e);
  } catch (...) {
    report();
  }
}
EOF
for p in swallow behind; do
  if [[ -z "$(awk -f tools/checks/interrupt_chains.awk "$probe/$p.cc" |
              awk -F'\t' '$3 == "SWALLOWS"')" ]]; then
    echo "interrupt-discipline: the checker no longer detects a swallowing" >&2
    echo "handler ($p probe)" >&2
    exit 1
  fi
done

counts="$(printf '%s\n' "$chains" | awk -F'\t' '{c[$3]++} END {
  printf "explicit=%d rethrows=%d terminates=%d documented=%d",
         c["EXPLICIT"], c["RETHROWS"], c["TERMINATES"], c["DOCUMENTED"] }')"
echo "interrupt-discipline OK ($(printf '%s\n' "$chains" | wc -l | tr -d ' ') chains: $counts)"
