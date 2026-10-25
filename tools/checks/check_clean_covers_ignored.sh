#!/usr/bin/env bash
# `just clean` removes every temporary file, and a new kind of temporary file
# cannot be forgotten.
#
# .gitignore lists what git leaves out, and that is two kinds of thing: what
# the work regenerates (builds, editor config, an instrument's work folder) and
# what a person put there (local settings, private notes, art dropped in). `git
# clean -X` would remove both. So `clean` names what it removes, and this check
# makes each pattern of .gitignore either appear in the clean recipe or be
# listed in tools/checks/clean_keeps.txt with its reason. A new ignore pattern
# fails here until it is one or the other (the kauai/sounds/*/work/ folders were
# once missed, and left hundreds of megabytes behind).
#
# A pattern is cleaned when a word of the recipe is that pattern (a path given
# to rm, or a name given to find's -name, which finds it at any depth), not
# when its text is part of another word: `.zed` is not cleaned by
# `misc/*/.zed`. What it does not see: a pattern with no slash before it is
# ignored at any depth, and rm removes it at the top only.
set -euo pipefail
cd "$(dirname "$0")/../.."

# The clean recipe's lines: from `clean:` to the next line at column 0.
recipe=$(awk '/^clean:/ {f = 1; next} f && /^[^ \t]/ {exit} f' justfile)
[[ -n "$recipe" ]] || { echo "clean-covers-ignored: no clean recipe in justfile" >&2; exit 1; }

# The recipe's words, unquoted and without a trailing slash.
words=$(tr -s ' \t' '\n\n' <<<"$recipe" | sed -e "s/^['\"]//" -e "s/['\"]\$//" -e 's:/$::')

# The patterns kept on purpose, exactly as .gitignore writes them.
keeps=$(grep -vE '^[[:space:]]*(#|$)' tools/checks/clean_keeps.txt | cut -f1)

fail=0
while IFS= read -r line; do
  case "$line" in '' | '#'* | '!'*) continue ;; esac
  pat="${line%/}"
  pat="${pat#/}"
  if grep -qxF -- "$pat" <<<"$words" || grep -qxF -- "$line" <<<"$keeps"; then
    continue
  fi
  echo "clean-covers-ignored: .gitignore ignores '$line', but 'just clean' does not remove it" >&2
  echo "  name it in the clean recipe of the justfile, or keep it on purpose in tools/checks/clean_keeps.txt" >&2
  fail=1
done < .gitignore

# A kept pattern that .gitignore no longer has is a stale reason.
while IFS= read -r k; do
  grep -qxF -- "$k" .gitignore || {
    echo "clean-covers-ignored: clean_keeps.txt keeps '$k', which .gitignore does not have" >&2
    fail=1
  }
done <<<"$keeps"

[[ "$fail" == 0 ]] || exit 1
echo "clean-covers-ignored OK ($(grep -cvE '^[[:space:]]*(#|$|!)' .gitignore) ignored patterns, each cleaned or kept on purpose)"
