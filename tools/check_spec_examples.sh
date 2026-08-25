#!/usr/bin/env bash
# Spec-example ratchet: every section of docs/language.md that states a rule
# should have a runnable ``` ```culebra ``` block, because a runnable block is
# the only part of the spec `just doctest` executes on both engines.
#
# Usage:
#   tools/check_spec_examples.sh            # gate (fail on a new unpinned section)
#   tools/check_spec_examples.sh --update   # rewrite the list below
#
# Why it matters: since the tree-walker retired, the executor and the LLVM
# lowering consume bytecode from one compiler, so nothing in the tree
# disagrees with a compiler bug on its own (docs/essays/one-front-end.md).
# What pins a rule is an assertion someone wrote — a `tests/*.cul` case or a
# doctest block. This gate cannot see the first kind, and does not try to:
# it holds the second, where the prose and the check live in one file and
# cannot drift apart.
#
# The unit is a `###` section, or a `##` chapter that has no `###` under it —
# so every heading that owns prose is counted exactly once. A section is
# PINNED if it holds at least one ` ```culebra ` block that is not
# `# doctest: skip`; the skipped ones are illustrative and run nowhere.
#
# Two-sided, like tools/difftest/leak_abort_allow.txt: an unpinned section
# missing from the list fails (write an example, or file it), and a listed
# section that has since gained one fails too (shrink the file).
set -euo pipefail
cd "$(dirname "$0")/.."

SPEC=docs/language.md
LIST=tools/spec_unpinned_sections.txt
UPDATE=0
[ "${1:-}" = --update ] && UPDATE=1

# One record per unit: "<key>\t<runnable culebra blocks>". The file is read
# twice: pass 1 marks the chapters that have `###` sections (so a chapter
# without one becomes a unit itself), pass 2 flushes a unit at every heading
# that ends one. Fence state is tracked in both, so a `#` line inside a code
# block can never read as a heading, and a block's directive is honoured only
# on its leading line — the rule doctest_runner.h applies.
scan() {
  awk '
    function flush() { if (key != "") printf "%s\t%d\n", key, runnable; key = ""; runnable = 0 }
    NR == FNR {
      if (fence1) { if (/^```/) fence1 = 0; next }
      if (/^```/) { fence1 = 1; next }
      if (/^## /)  chapter = substr($0, 4)
      if (/^### /) has[chapter] = 1
      next
    }
    # --- second pass ---
    infence {
      if (/^```/) { infence = 0; leading = 0; next }
      if (leading) { leading = 0; if ($0 !~ /^# doctest: skip[[:space:]]*$/) runnable++ }
      next
    }
    /^```/ {
      infence = 1
      leading = ($0 ~ /^```culebra[[:space:]]*$/)
      next
    }
    /^## / {
      flush()
      chapter2 = substr($0, 4)
      if (!(chapter2 in has)) key = chapter2
      next
    }
    /^### / {
      flush()
      key = chapter2 " > " substr($0, 5)
      next
    }
    END { flush() }
  ' "$1" "$1"
}

all=$(scan "$SPEC")
total=$(printf '%s\n' "$all" | grep -c '' || true)
if [ "$total" -lt 100 ]; then
  echo "spec-examples FAIL: only $total sections found in $SPEC —" >&2
  echo "  did the heading structure change, or did a fence go unclosed?" >&2
  exit 1
fi

dupes=$(printf '%s\n' "$all" | cut -f1 | LC_ALL=C sort | uniq -d)
if [ -n "$dupes" ]; then
  echo "spec-examples FAIL: two sections share a key, so the list cannot address them:" >&2
  printf '%s\n' "$dupes" | sed 's/^/  /' >&2
  echo "  Rename one heading, or give this script a finer key." >&2
  exit 1
fi

unpinned=$(printf '%s\n' "$all" | awk -F'\t' '$2 == 0 { print $1 }' | LC_ALL=C sort)

if [ "$UPDATE" = 1 ]; then
  # Keep the file's own comments and order, drop entries that now have an
  # example, and append the new ones for filing — the same surgical update
  # tools/difftest/leak.sh does, so hand-written notes survive their tooling.
  if [ -f "$LIST" ]; then
    printf '%s\n' "$unpinned" > "$LIST.cur"
    awk -v cur="$LIST.cur" '
      BEGIN { while ((getline l < cur) > 0) keep[l] = 1 }
      /^[[:space:]]*(#|$)/ { print; next }
      ($0 in keep) { print }' "$LIST" > "$LIST.tmp"
    grep -v '^[[:space:]]*#' "$LIST" | grep -v '^[[:space:]]*$' \
      | LC_ALL=C sort > "$LIST.old"
    new=$(LC_ALL=C comm -23 "$LIST.cur" "$LIST.old")
    if [ -n "$new" ]; then
      { echo ""
        echo "# --- new since the last update ---"
        printf '%s\n' "$new"; } >> "$LIST.tmp"
    fi
    mv "$LIST.tmp" "$LIST"
    rm -f "$LIST.cur" "$LIST.old"
  else
    printf '%s\n' "$unpinned" > "$LIST"
  fi
  echo "spec-examples: list updated — $(printf '%s\n' "$unpinned" | grep -c '') unpinned sections"
  exit 0
fi

[ -f "$LIST" ] || { echo "spec-examples FAIL: no list ($LIST). Run --update." >&2; exit 1; }

grep -v '^[[:space:]]*#' "$LIST" | grep -v '^[[:space:]]*$' | LC_ALL=C sort > "$LIST.listed"
printf '%s\n' "$unpinned" > "$LIST.unpinned"

added=$(LC_ALL=C comm -23 "$LIST.unpinned" "$LIST.listed")
fixed=$(LC_ALL=C comm -13 "$LIST.unpinned" "$LIST.listed")
rm -f "$LIST.listed" "$LIST.unpinned"

rc=0
if [ -n "$fixed" ]; then
  echo "spec-examples: $(printf '%s\n' "$fixed" | grep -c '') listed section(s) now have a runnable example — shrink $LIST:"
  printf '%s\n' "$fixed" | sed 's/^/  /'
  echo "  → run: tools/check_spec_examples.sh --update"
  rc=1
fi
if [ -n "$added" ]; then
  echo "spec-examples FAIL: $(printf '%s\n' "$added" | grep -c '') section(s) state a rule with no runnable example:" >&2
  printf '%s\n' "$added" | sed 's/^/  /' >&2
  echo '  Add a ```culebra block `just doctest` can run (a `# =>` or `# !!` line),' >&2
  echo "  or file the section in $LIST if it is prose that cannot carry one." >&2
  rc=1
fi
[ "$rc" = 0 ] || exit 1

echo "spec-examples OK ($total sections, $(grep -vc '^[[:space:]]*\(#\|$\)' "$LIST") without a runnable example)"
