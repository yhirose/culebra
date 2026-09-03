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
#
# The same chain has a first link. builtin_method_names() is not the
# implementation -- vm/vm.h's kSpecs table is -- and it is the compile-time filter
# in front of the cold diagnostics a built-in call owes (BareMethChk, and the
# arity and keyword arms of BArity). A name kSpecs implements and that set
# omits loses those checks AND drops out of the corpus, silently, with nothing
# failing: eighteen Tensor methods sat that way, each from the day it was
# added. So the gate is two links, kSpecs -> builtin_method_names() ->
# gen.cul, and a new built-in has to be written into all three.
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
# A kSpecs row opens `{"<name>", <arity>, ...` and carries a receiver mask
# (kRecvArray, kRecvTensor, ...). The arity is what tells a row apart from the
# parameter-name list that follows it: a row wrapped over two lines puts those
# names on a continuation opening `{"i", "fallback"}`, which matches `kRecv`
# too — so a first-quoted-token rule would read `i` as a method name. A name
# is only a name when a number follows it.
implemented=$(sed -n '/^  static constexpr BMethSpec kSpecs\[\] = {/,/^  };/p' \
        include/vm/vm.h | grep 'kRecv' \
        | sed -nE 's/^[[:space:]]*\{"([A-Za-z_0-9]+)",[[:space:]]*[0-9]+,.*/\1/p' \
        | sort -u) || true

if [ -z "$names" ] || [ -z "$swept" ] || [ -z "$implemented" ]; then
  echo "difftest-coverage FAIL: could not extract one of the three lists —" >&2
  echo "  did builtin_method_names(), gen.cul's \`let methods = [\`," >&2
  echo "  or vm/vm.h's \`static constexpr BMethSpec kSpecs[]\` move?" >&2
  exit 1
fi

unnamed=$(comm -23 <(echo "$implemented") <(echo "$names"))
if [ -n "$unnamed" ]; then
  echo "difftest-coverage FAIL: built-in methods vm.h implements that" >&2
  echo "  builtin_method_names() does not name — they lose BareMethChk and" >&2
  echo "  the BArity arms, and never reach the corpus:" >&2
  echo "$unnamed" | sed 's/^/  /' >&2
  echo "  Add them to builtin_method_names() in include/base/shared.h." >&2
  exit 1
fi

missing=$(comm -23 <(echo "$names") <(echo "$swept"))
if [ -n "$missing" ]; then
  echo "difftest-coverage FAIL: built-in methods the corpus never applies:" >&2
  echo "$missing" | sed 's/^/  /' >&2
  echo "  Add them to \`let methods\` in tools/difftest/gen.cul." >&2
  exit 1
fi

echo "difftest-coverage OK ($(echo "$names" | wc -l | tr -d ' ') built-in methods, all named and all swept)"
