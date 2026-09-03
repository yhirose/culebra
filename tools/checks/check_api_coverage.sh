#!/usr/bin/env bash
# API-surface coverage gate.
#
# docs/quick-guide.md's generated signature index (misc/gen_quick_guide.cul)
# is a machine-readable list of every `Ns.fn(...)` the reference docs
# document, native and preamble-defined alike — the thing canon_sigs_table.h
# alone cannot give, since it only knows natives. The PEG grammar
# (include/grammar_def.h) is the same kind of list for keywords, contextual
# ones included, so a new control-flow form (`cond`, `nobreak`, `effect`)
# shows up here the same way a new namespace function does.
#
# A name only docs/quick-guide.md or the grammar knows about, with no
# `tests/*.cul`, `tests/*.sh` or executed doctest block ever calling it, is a
# rule nothing durable exercises — the class of hole `Http.put`/`FS.abspath`
# sat in for two releases (docs/essays/one-front-end.md).
#
# Deliberately out of scope: the 250+ receiver-method signatures in the same
# index (`h.poll()`, `re.test(s)`) — a bare method name cannot be matched
# without false positives from `.close()`-shaped unrelated calls, and
# built-in methods already have check-difftest-coverage.
#
# Usage:
#   tools/checks/check_api_coverage.sh            # gate (fail on a new uncalled name)
#   tools/checks/check_api_coverage.sh --update   # rewrite the allowlist below
set -euo pipefail
cd "$(dirname "$0")/../.."

QUICKGUIDE=docs/quick-guide.md
GRAMMAR=include/grammar_def.h
LIST=tools/checks/api_untested.txt
UPDATE=0
[ "${1:-}" = --update ] && UPDATE=1

WORK=$(mktemp -d "${TMPDIR:-/tmp}/culebra-api.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# 1. Namespace signatures: `**Ns** — Ns.fn(...); ...` lines inside the
# generated block. Each `;`-joined entry that starts `Ns.fn(` names a call;
# idioms like `let f = File.open(p); defer { f.close() }` inside the same
# entry don't match the anchored pattern and are ignored.
sed -n '/<!-- BEGIN GENERATED: signature index -->/,/<!-- END GENERATED -->/p' "$QUICKGUIDE" \
  | grep '^\*\*' | tr ';' '\n' \
  | grep -oE '(^|[^A-Za-z0-9_.])[A-Z][A-Za-z0-9]*\.[a-z_][A-Za-z0-9_]*\(' \
  | grep -oE '[A-Z][A-Za-z0-9]*\.[a-z_][A-Za-z0-9_]*' | sort -u > "$WORK/sigs.txt"

# 2. Grammar keywords, the same extraction misc/sync_grammar.sh does: pull
# the embedded PEG block, then every quoted bare-word literal in it.
awk '/^const auto grammar_ = R"\(/{flag=1; next} /^\)";/{flag=0; exit} flag{print}' "$GRAMMAR" \
  | grep -oE "'[a-zA-Z_]+'" | tr -d "'" | grep -vE '^(_|re)$' | sort -u > "$WORK/kw.txt"

if [ ! -s "$WORK/sigs.txt" ] || [ ! -s "$WORK/kw.txt" ]; then
  echo "api-coverage FAIL: could not extract signatures or keywords —" >&2
  echo "  did the quick-guide markers or the grammar block move?" >&2
  exit 1
fi

cat "$WORK/sigs.txt" "$WORK/kw.txt" > "$WORK/want.txt"

# 3. The durable corpus: every .cul/.sh test, plus every doctest block this
# repo actually runs (skip the ones `just doctest` skips too). Comment lines
# are stripped first so a name mentioned only in prose doesn't count as a
# call — the false-"covered" a first pass over this gave for `Sys.exit`,
# which appeared only in a comment in tests/lint_test.sh.
{
  find tests -name '*.cul' -o -name '*.sh' | xargs cat
  for f in docs/*.md docs/guides/*.md; do
    case "$f" in *.ja.md) continue ;; esac
    awk '
      /^```culebra[[:space:]]*$/ { infence = 1; leading = 1; next }
      /^```/ { infence = 0; next }
      infence {
        if (leading) { leading = 0; if ($0 ~ /^# doctest: skip/) { infence = 0; next } }
        print
      }' "$f"
  done
} | grep -vE '^[[:space:]]*(#|//)' > "$WORK/corpus.txt"

# 4. One pass, tokenizing the corpus on anything that isn't part of a dotted
# identifier — this is a literal-token match, not a grep substring match, so
# a keyword only ever counted as a keyword (not, say, as a substring of an
# identifier) and `Ns.fn` only counts a real call, not a mention inside a
# longer name.
uncalled=$(awk -v want="$WORK/want.txt" '
  BEGIN { while ((getline w < want) > 0) need[w] = 1 }
  { n = split($0, tok, /[^A-Za-z0-9_.]+/)
    for (i = 1; i <= n; i++) if (tok[i] in need) seen[tok[i]] = 1 }
  END { for (w in need) if (!(w in seen)) print w }
' "$WORK/corpus.txt" | LC_ALL=C sort)

if [ "$UPDATE" = 1 ]; then
  if [ -f "$LIST" ]; then
    printf '%s\n' "$uncalled" > "$WORK/cur.txt"
    awk -v cur="$WORK/cur.txt" '
      BEGIN { while ((getline l < cur) > 0) keep[l] = 1 }
      /^[[:space:]]*(#|$)/ { print; next }
      ($0 in keep) { print }' "$LIST" > "$WORK/tmp.txt"
    grep -v '^[[:space:]]*#' "$LIST" | grep -v '^[[:space:]]*$' | LC_ALL=C sort > "$WORK/old.txt"
    new=$(LC_ALL=C comm -23 "$WORK/cur.txt" "$WORK/old.txt")
    if [ -n "$new" ]; then
      { echo ""; echo "# --- new since the last update ---"; printf '%s\n' "$new"; } >> "$WORK/tmp.txt"
    fi
    mv "$WORK/tmp.txt" "$LIST"
  else
    printf '%s\n' "$uncalled" > "$LIST"
  fi
  echo "api-coverage: list updated — $(printf '%s\n' "$uncalled" | grep -c '') name(s) with no durable caller"
  exit 0
fi

[ -f "$LIST" ] || { echo "api-coverage FAIL: no list ($LIST). Run --update." >&2; exit 1; }

grep -v '^[[:space:]]*#' "$LIST" | grep -v '^[[:space:]]*$' | LC_ALL=C sort > "$WORK/listed.txt"
printf '%s\n' "$uncalled" > "$WORK/uncalled.txt"

added=$(LC_ALL=C comm -23 "$WORK/uncalled.txt" "$WORK/listed.txt")
fixed=$(LC_ALL=C comm -13 "$WORK/uncalled.txt" "$WORK/listed.txt")

rc=0
if [ -n "$fixed" ]; then
  echo "api-coverage: $(printf '%s\n' "$fixed" | grep -c '') listed name(s) now have a durable caller — shrink $LIST:"
  printf '%s\n' "$fixed" | sed 's/^/  /'
  echo "  → run: tools/checks/check_api_coverage.sh --update"
  rc=1
fi
if [ -n "$added" ]; then
  echo "api-coverage FAIL: $(printf '%s\n' "$added" | grep -c '') name(s) documented but never called by a durable test:" >&2
  printf '%s\n' "$added" | sed 's/^/  /' >&2
  echo "  Call it from a tests/*.cul (or *.sh) case, or a doctest block \`just doctest\` runs," >&2
  echo "  or file it in $LIST if it genuinely has no durable caller yet." >&2
  rc=1
fi
[ "$rc" = 0 ] || exit 1

total=$(wc -l < "$WORK/want.txt" | tr -d ' ')
listed=$(grep -vc '^[[:space:]]*\(#\|$\)' "$LIST")
echo "api-coverage OK ($total names, $listed without a durable caller)"
