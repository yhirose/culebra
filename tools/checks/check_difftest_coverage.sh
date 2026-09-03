#!/usr/bin/env bash
# Differential-corpus coverage gate.
#
# The corpus applies a hand-written list of method names to every receiver
# type and argument shape; a name outside that list is a name whose
# interp-vs-JIT error shapes nobody compares. `trim_start` sat outside it for
# two months with a live asymmetry (interp silently dropped a surplus
# positional argument where the JIT raised), and only became visible when an
# unrelated commit happened to add a sibling method to the list.
#
# So: every name in shared.h's builtin_method_names() must appear in gen.cul's
# Dimension 2 list. Adding a built-in method now means adding it there too.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Both lists are literal arrays; take each from its opening line to the first
# line that closes it, so a later array in the same file can't leak in. The
# name pattern stays wider than the current all-snake_case set: a name this
# does not match would drop out of `names` and pass silently, where an
# over-match only adds a name gen.cul lacks and fails loudly. `|| true` keeps
# a zero-match grep from killing the script under `set -e` before the
# anchors-moved diagnostic below can run.
names=$(sed -n '/^inline const std::unordered_set<std::string_view>& builtin_method_names/,/return kNames;/p' \
        include/base/shared.h | grep -o '"[A-Za-z_0-9]*"' | tr -d '"' | sort -u) || true
swept=$(sed -n '/^let methods = \[/,/\]/p' tools/difftest/gen.cul \
        | grep -o "'[A-Za-z_0-9]*'" | tr -d "'" | sort -u) || true

if [ -z "$names" ] || [ -z "$swept" ]; then
  echo "difftest-coverage FAIL: could not extract one of the two lists —" >&2
  echo "  did builtin_method_names() or gen.cul's \`let methods = [\` move?" >&2
  exit 1
fi

missing=$(comm -23 <(echo "$names") <(echo "$swept"))
if [ -n "$missing" ]; then
  echo "difftest-coverage FAIL: built-in methods the corpus never applies:" >&2
  echo "$missing" | sed 's/^/  /' >&2
  echo "  Add them to \`let methods\` in tools/difftest/gen.cul." >&2
  exit 1
fi

echo "difftest-coverage OK ($(echo "$names" | wc -l | tr -d ' ') built-in methods, all swept)"
