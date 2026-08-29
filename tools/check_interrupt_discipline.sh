#!/usr/bin/env bash
# Interrupt-discipline gate.
#
# An interrupt (Ctrl+C, or an isolate's cancel) is never a program error. It
# arrives as a CulebraError with kind "Interrupted", so any handler that catches
# CulebraError — or std::runtime_error, or std::exception, or `...`, all of
# which catch it too — can turn one into a diagnostic and carry on. When that
# happens in a component that hosts a run, the press is reported as if the
# program had failed and the run continues, which is the opposite of what was
# asked. The flag is one-shot, so the next file or block sees nothing.
#
# That bug has been written three times in this tree: `culebra test` reported an
# interrupted file as errored and ran the rest of the suite; `culebra test --doc`
# did the same per block; and the REPL needed the interrupt reported rather than
# thrown, which is how the first two got their stashes. Each was found by reading
# code, never by a test — the gate was green through all three.
#
# So the rule, from is_interrupt()'s comment: a catch that reports errors lets an
# interrupt through. This checks it on the surface where a violation is
# user-visible — the files that host a run or a CLI analysis (kFiles below).
# Every handler chain there must do one of:
#
#   - ask is_interrupt() and re-throw it                             CHECKED
#   - re-throw everything (`throw;`)                                 RETHROWS
#   - end the process                                                TERMINATES
#   - carry `// interrupt: <why one cannot arrive here>`             DOCUMENTED
#
# The note is deliberately a comment at the site rather than a list of line
# numbers in here: the reason a handler is safe belongs next to the handler, and
# the reasons are all facts about OTHER files (nothing under a parse polls the
# flag; the DAP lane installs no handler) that a future change could quietly
# flip. A note that has to be edited is the point.
set -euo pipefail
cd "$(dirname "$0")/.."

# The run-host surface: everything that runs a culebra program, loads its
# modules, or analyses a file for the CLI. A new one belongs on this list.
kFiles=(
  src/main.cc
  include/dap.h
  include/debug_engine.h
  include/lint.h
  include/parser.h
  include/repl_core.h
  include/test_engine.h
  include/test_runner.h
  include/vm_debug.h
  include/vm_embed.h
  include/vm_session.h
)

missing=0
for f in "${kFiles[@]}"; do
  [[ -f "$f" ]] || { echo "interrupt-discipline: no $f" >&2; missing=1; }
done
[[ $missing -eq 0 ]] || exit 1

chains="$(for f in "${kFiles[@]}"; do awk -f tools/interrupt_chains.awk "$f"; done)"

bad="$(printf '%s\n' "$chains" | awk -F'\t' '$3 == "SWALLOWS"')"
if [[ -n "$bad" ]]; then
  echo "interrupt-discipline: handler(s) can report an interrupt as a program error:" >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  echo >&2
  echo "Add is_interrupt(e) + re-throw, or a '// interrupt: <reason>' note saying" >&2
  echo "why an interrupt cannot reach that handler." >&2
  exit 1
fi

# Positive control: the checker must be able to fail. Without this a change that
# breaks the awk (an unparsed brace, a renamed verdict) reports zero SWALLOWS on
# a tree full of them and the gate goes quietly green — the shape that let a
# one-probe shell gate miss a whole peeling pass before.
probe="$(mktemp -d)"
trap 'rm -rf "$probe"' EXIT
cat > "$probe/swallow.cc" <<'EOF'
void f() {
  try {
    run();
  } catch (const CulebraError& e) {
    report(e);
  }
}
EOF
if [[ -n "$(awk -f tools/interrupt_chains.awk "$probe/swallow.cc" | awk -F'\t' '$3 == "SWALLOWS"')" ]]; then
  :
else
  echo "interrupt-discipline: the checker no longer detects a swallowing handler" >&2
  exit 1
fi

counts="$(printf '%s\n' "$chains" | awk -F'\t' '{c[$3]++} END {
  printf "checked=%d rethrows=%d terminates=%d documented=%d",
         c["CHECKED"], c["RETHROWS"], c["TERMINATES"], c["DOCUMENTED"] }')"
echo "interrupt-discipline OK ($(printf '%s\n' "$chains" | wc -l | tr -d ' ') chains: $counts)"
