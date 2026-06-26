#!/usr/bin/env bash
# Sync misc/culebra.peg and the AUTO-KEYWORDS blocks in the editor syntax files
# (misc/vim/cul.vim, misc/vscode/syntaxes/culebra.tmLanguage.json) from
# include/parser.h. Run `misc/sync_grammar.sh` to overwrite the files;
# `misc/sync_grammar.sh --check` exits non-zero if any is stale.

set -euo pipefail

cd "$(dirname "$0")/.."

CHECK=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

PARSER=include/parser.h
PEG=misc/culebra.peg
VIM=misc/vim/cul.vim
TM=misc/vscode/syntaxes/culebra.tmLanguage.json
MAP=misc/keyword-map.txt

TMP_PEG=$(mktemp)
TMP_KW=$(mktemp)
TMP_MAP_KW=$(mktemp)
TMP_VIM_BLOCK=$(mktemp)
TMP_VIM=$(mktemp)
TMP_TM_BLOCK=$(mktemp)
TMP_TM=$(mktemp)
trap 'rm -f "$TMP_PEG" "$TMP_KW" "$TMP_MAP_KW" "$TMP_VIM_BLOCK" "$TMP_VIM" "$TMP_TM_BLOCK" "$TMP_TM"' EXIT

# 1. Extract the embedded grammar block.
awk '/^const auto grammar_ = R"\(/{flag=1; next} /^\)";/{flag=0; exit} flag{print}' "$PARSER" > "$TMP_PEG"

# 2. Extract all keyword literals from the grammar. Skip the wildcard '_' and
#    the regex-literal prefix 're' (REGEX_LIT) — the latter is contextual (a
#    quote must follow) and stays a valid identifier, so it is not a keyword.
grep -oE "'[a-zA-Z_]+'" "$TMP_PEG" | tr -d "'" | grep -vE '^(_|re)$' | sort -u > "$TMP_KW"

# 3. Warn about K() keywords missing from the map.
awk -F: '/^cul/{
  n = split($2, parts, /[[:space:]]+/)
  for (i = 1; i <= n; i++) if (parts[i] != "") print parts[i]
}' "$MAP" | sort -u > "$TMP_MAP_KW"
UNMAPPED=$(comm -23 "$TMP_KW" "$TMP_MAP_KW")
if [[ -n "$UNMAPPED" ]]; then
  echo "ERROR: keywords without a category in $MAP:" >&2
  echo "$UNMAPPED" | sed 's/^/  /' >&2
  echo "  (add to the appropriate \`culCategory: ...\` line of $MAP)" >&2
  exit 1
fi

# 4. Emit the AUTO-KEYWORDS block in the order categories appear in the map.
awk -F: '/^cul/{
  cat = $1
  rest = $2
  sub(/^[[:space:]]+/, "", rest)
  printf "syn keyword %-14s %s\n", cat, rest
}' "$MAP" > "$TMP_VIM_BLOCK"

# 5. Splice the new block into cul.vim between BEGIN/END markers.
awk -v block_file="$TMP_VIM_BLOCK" '
  /^" === BEGIN AUTO-KEYWORDS/ {
    print
    while ((getline line < block_file) > 0) print line
    close(block_file)
    in_block = 1
    next
  }
  /^" === END AUTO-KEYWORDS/ {
    in_block = 0
    print
    next
  }
  !in_block { print }
' "$VIM" > "$TMP_VIM"

# 6. Emit the VSCode TextMate keyword rules — one per category, mapping the
#    cul.vim category to a TextMate scope. Each line carries a trailing comma;
#    the never-matching `\b\B` end marker that follows is the array's last
#    element, so the JSON stays valid.
tm_scope() {
  case "$1" in
    culFunction)    echo keyword.declaration.function.culebra;;
    culClass)       echo keyword.declaration.class.culebra;;
    culConditional) echo keyword.control.conditional.culebra;;
    culRepeat)      echo keyword.control.loop.culebra;;
    culStatement)   echo keyword.control.culebra;;
    culInclude)     echo keyword.control.import.culebra;;
    culDebugger)    echo keyword.other.debugger.culebra;;
    culBoolean)     echo constant.language.boolean.culebra;;
    culConstant)    echo constant.language.culebra;;
    culStorage)     echo storage.modifier.culebra;;
    *)              echo keyword.other.culebra;;
  esac
}
: > "$TMP_TM_BLOCK"
while IFS= read -r line; do
  case "$line" in ''|\#*) continue;; esac
  cat=${line%%:*}
  # shellcheck disable=SC2086
  set -- ${line#*:}              # word-split the keyword list
  pipe=$(printf '%s|' "$@"); pipe=${pipe%|}
  printf '        { "match": "\\\\b(%s)\\\\b", "name": "%s" },\n' \
    "$pipe" "$(tm_scope "$cat")" >> "$TMP_TM_BLOCK"
done < "$MAP"

# 7. Splice the block into the tmLanguage between the `\b\B` marker rules.
awk -v block_file="$TMP_TM_BLOCK" '
  /auto-keywords-begin/ {
    print
    while ((getline line < block_file) > 0) print line
    close(block_file)
    in_block = 1
    next
  }
  /auto-keywords-end/ {
    in_block = 0
    print
    next
  }
  !in_block { print }
' "$TM" > "$TMP_TM"

if [[ "$CHECK" -eq 1 ]]; then
  STATUS=0
  if ! diff -q "$PEG" "$TMP_PEG" > /dev/null 2>&1; then
    echo "ERROR: $PEG is out of sync with $PARSER. Run \`just sync-grammar\`." >&2
    diff -u "$PEG" "$TMP_PEG" >&2 || true
    STATUS=1
  fi
  if ! diff -q "$VIM" "$TMP_VIM" > /dev/null 2>&1; then
    echo "ERROR: $VIM AUTO-KEYWORDS block is out of sync. Run \`just sync-grammar\`." >&2
    diff -u "$VIM" "$TMP_VIM" >&2 || true
    STATUS=1
  fi
  if ! diff -q "$TM" "$TMP_TM" > /dev/null 2>&1; then
    echo "ERROR: $TM AUTO-KEYWORDS block is out of sync. Run \`just sync-grammar\`." >&2
    diff -u "$TM" "$TMP_TM" >&2 || true
    STATUS=1
  fi
  exit $STATUS
fi

cp "$TMP_PEG" "$PEG"
cp "$TMP_VIM" "$VIM"
cp "$TMP_TM" "$TM"
echo "synced $PEG, $VIM, and $TM from $PARSER"
