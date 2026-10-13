# Sourced by the two catch-discipline gates (check_interrupt_discipline.sh,
# check_thrown_value_discipline.sh): what they do with catch_chains.awk's
# verdicts, once. Each gate keeps its own rule, file list and failure hint.
#
# The caller sets, before sourcing:
#   gate      the name the messages carry
#   carrier   the type the gate asks about (it derives from nothing)
#   note      the comment that documents a handler: `// <note>: <why>`
#   behind    a handler type the checker does not report on (second probe)

chain_awk=(awk -v "carrier=$carrier" -v "note=$note"
           -f tools/checks/catch_chains.awk)

# catch_chains <file>...: one verdict line per chain.
catch_chains() {
  local f
  for f in "$@"; do "${chain_awk[@]}" "$f"; done
}

# swallowing_chains <chains>: the ones no verdict excuses.
swallowing_chains() {
  printf '%s\n' "$1" | awk -F'\t' '$3 == "SWALLOWS"'
}

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
# chains on the interrupt gate's own surface.
require_swallow_probes() {
  local probe p
  probe="$(mktemp -d "${TMPDIR:-/tmp}/culebra-catch.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -rf '$probe'" EXIT
  cat > "$probe/swallow.cc" <<'EOF'
void f() {
  try {
    run();
  } catch (...) {
    report();
  }
}
EOF
  cat > "$probe/behind.cc" <<EOF
void g() {
  try {
    run();
  } catch (const $behind& e) {
    report(e);
  } catch (...) {
    report();
  }
}
EOF
  for p in swallow behind; do
    if [[ -z "$(swallowing_chains "$(catch_chains "$probe/$p.cc")")" ]]; then
      echo "$gate: the checker no longer detects a swallowing handler" >&2
      echo "($p probe)" >&2
      exit 1
    fi
  done
}

# report_ok <chains>: the closing line, with the count per verdict.
report_ok() {
  local counts
  counts="$(printf '%s\n' "$1" | awk -F'\t' '{c[$3]++} END {
    printf "explicit=%d rethrows=%d terminates=%d documented=%d",
           c["EXPLICIT"], c["RETHROWS"], c["TERMINATES"], c["DOCUMENTED"] }')"
  echo "$gate OK ($(printf '%s\n' "$1" | wc -l | tr -d ' ') chains: $counts)"
}
