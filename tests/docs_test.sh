#!/usr/bin/env bash
# Regression test for `culebra docs` (src/docs_cmd.cc). The properties that
# matter are the ones a caller depends on without reading the output: the exit
# codes, the fact that a broad pattern degrades to an index instead of dumping
# the reference set, and that a signature fragment is still searchable even
# though it is not a valid regex. Usage: docs_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: docs_test.sh <culebra-binary>}"
fail=0

check_exit() {  # <label> <want> <args...>
  local label=$1 want=$2; shift 2
  "$CULEBRA" docs "$@" >/dev/null 2>&1
  local got=$?
  if [[ $got != "$want" ]]; then
    echo "FAIL exit [$label]: want $want, got $got"; fail=1
  fi
}

check_contains() {  # <label> <needle> <args...>
  local label=$1 needle=$2; shift 2
  local out
  out=$("$CULEBRA" docs "$@" 2>&1)
  if [[ "$out" != *"$needle"* ]]; then
    echo "FAIL contains [$label]: no '$needle' in output"; fail=1
  fi
}

check_absent() {  # <label> <needle> <args...>
  local label=$1 needle=$2; shift 2
  local out
  out=$("$CULEBRA" docs "$@" 2>&1)
  if [[ "$out" == *"$needle"* ]]; then
    echo "FAIL absent [$label]: unexpected '$needle' in output"; fail=1
  fi
}

# The grep convention, which is what makes an existence check a one-liner.
check_exit "hit"            0 -g 'Math.wrap'
# A name chosen so it cannot appear in the docs — including in this file's
# own prose, which would otherwise make the query match itself.
check_exit "miss"           1 -g 'Zqx.nonexistent'
check_exit "unknown topic"  2 nosuchtopic
check_exit "unknown flag"   2 --bogus
check_exit "-g without arg" 2 -g
check_exit "--at needs a topic" 2 --at 5
check_exit "list"           0
check_exit "topic"          0 tooling
check_exit "ja topic"       0 --ja tooling

# Every topic is present in both editions: a missing half would mean the
# bilingual pair broke, and gen_docs.cmake refuses to build in that case —
# this catches a topic silently dropped from its table instead.
for t in llm agent guide language stdlib tooling deployment; do
  check_exit "topic $t"    0 "$t" --at 1
  check_exit "topic $t ja" 0 --ja "$t" --at 1
done

# A heading hit outranks a body mention, and the locator says where it came
# from so `--at` can retrieve the rest.
check_contains "signature lookup" 'Math.wrap(x: Long, n: Long)' -g 'Math.wrap'
check_contains "locator"          'stdlib.md:'                  -g 'Math.wrap'

# An unbalanced signature fragment is not a regex; searching for it literally
# beats refusing the query.
check_contains "literal fallback" 'not a valid regex' -g 'get_or_put('
check_exit     "literal fallback finds it" 0 -g 'get_or_put('

# A broad pattern must not dump the reference set. `the` appears on thousands
# of lines; the output has to stay an index.
lines=$("$CULEBRA" docs -g 'the' 2>&1 | wc -l)
if [[ $lines -gt 400 ]]; then
  echo "FAIL broad pattern: $lines lines of output (expected an index)"; fail=1
fi
check_contains "broad pattern degrades" 'headings only' -g 'the'

# llm.md's signature index is generated from the other topics, so a
# corpus-wide search must not report every API twice — but naming it works.
check_absent   "llm excluded by default" 'llm.md:' -g 'Math.wrap'
check_contains "llm searchable on request" 'llm.md:' llm -g 'Math.wrap'
check_absent   "agent excluded by default" 'agent.md:' -g 'Math.wrap'
check_contains "agent searchable on request" 'agent.md:' agent -g 'Math.wrap'

# `culebra docs agent` is meant to be redirected into an instructions file, so
# the note about which file that is has to stay out of the redirect.
check_contains "agent names its destinations" 'CLAUDE.md' agent
if "$CULEBRA" docs agent 2>/dev/null | grep -q 'CLAUDE.md'; then
  echo "FAIL agent hint: destinations leaked into stdout"; fail=1
fi

if [[ $fail == 0 ]]; then echo "docs_test OK"; fi
exit $fail
