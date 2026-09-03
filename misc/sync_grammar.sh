#!/usr/bin/env bash
# Sync misc/culebra.peg and the AUTO-KEYWORDS blocks in the editor syntax files
# (misc/vim/culebra.vim, misc/vscode/syntaxes/culebra.tmLanguage.json,
# misc/zed/languages/culebra/highlights.scm, playground/culebra-lang.js — the
# last only on branches where it exists) from include/grammar_def.h (the
# single-source grammar; parser.h #includes it) and misc/keyword-map.txt.
# Run `misc/sync_grammar.sh` to overwrite the files;
# `misc/sync_grammar.sh --check` exits non-zero if any is stale.

set -euo pipefail

cd "$(dirname "$0")/.."

CHECK=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

PARSER=include/grammar_def.h
PEG=misc/culebra.peg
VIM=misc/vim/culebra.vim
TM=misc/vscode/syntaxes/culebra.tmLanguage.json
ZED=misc/zed/languages/culebra/highlights.scm
CMLANG=playground/culebra-lang.js
STDLIB=include/stdlib_rt.h
MAP=misc/keyword-map.txt

# Name the directory explicitly: macOS mktemp ignores TMPDIR for a bare call and
# uses the darwin per-user temp dir, which a sandboxed run cannot write.
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/sync_grammar.XXXXXX")
trap 'rm -rf "$TMPD"' EXIT

TMP_PEG=$TMPD/peg
TMP_KW=$TMPD/kw
TMP_MAP_KW=$TMPD/map_kw
TMP_NS=$TMPD/ns
TMP_MAP_NS=$TMPD/map_ns
TMP_VIM_BLOCK=$TMPD/vim_block
TMP_VIM=$TMPD/vim
TMP_TM_BLOCK=$TMPD/tm_block
TMP_TM=$TMPD/tm
TMP_ZED_BLOCK=$TMPD/zed_block
TMP_ZED=$TMPD/zed
TMP_CMLANG_BLOCK=$TMPD/cmlang_block
TMP_CMLANG=$TMPD/cmlang

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

# 4. Warn about stdlib namespaces missing from the culBuiltin lines. Those
#    lines carry the names the editors colour as library-provided rather than
#    user-defined, so a namespace added to the engines but not here would show
#    up in an editor as if the user had declared it. Capitalized names only:
#    the set also holds bare function globals and `_`-prefixed internals whose
#    public spelling is listed separately in it.
awk '/^inline const std::unordered_set<std::string_view>& builtin_var_names/,/^}/' \
  "$STDLIB" | grep -oE '"[A-Za-z_][A-Za-z0-9_]*"' | tr -d '"' |
  grep -E '^[A-Z]' | sort -u > "$TMP_NS"
awk -F: '/^culBuiltin:/{
  n = split($2, parts, /[[:space:]]+/)
  for (i = 1; i <= n; i++) if (parts[i] != "") print parts[i]
}' "$MAP" | sort -u > "$TMP_MAP_NS"
MISSING_NS=$(comm -23 "$TMP_NS" "$TMP_MAP_NS")
if [[ -n "$MISSING_NS" ]]; then
  echo "ERROR: stdlib namespaces without a culBuiltin entry in $MAP:" >&2
  echo "$MISSING_NS" | sed 's/^/  /' >&2
  echo "  (add to a \`culBuiltin: ...\` line of $MAP)" >&2
  exit 1
fi

# 5. Warn about categories with no `hi def link` in culebra.vim. The generated
#    block only defines the syntax groups; the links are hand-written, and an
#    unlinked group renders uncoloured.
UNLINKED=$(awk -F: '/^cul/{print $1}' "$MAP" | sort -u | while IFS= read -r cat; do
  grep -qE "^hi def link[[:space:]]+$cat[[:space:]]" "$VIM" || echo "$cat"
done)
if [[ -n "$UNLINKED" ]]; then
  echo "ERROR: categories without a \`hi def link\` in $VIM:" >&2
  echo "$UNLINKED" | sed 's/^/  /' >&2
  echo "  (add a \`hi def link <category> <HighlightGroup>\` line to $VIM)" >&2
  exit 1
fi

# 6. Emit the AUTO-KEYWORDS block in the order categories appear in the map.
awk -F: '/^cul/{
  cat = $1
  rest = $2
  sub(/^[[:space:]]+/, "", rest)
  printf "syn keyword %-14s %s\n", cat, rest
}' "$MAP" > "$TMP_VIM_BLOCK"

# 7. Splice the new block into culebra.vim between BEGIN/END markers.
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

# 8. Emit the VSCode TextMate keyword rules — one per category, mapping the
#    culebra.vim category to a TextMate scope. Each line carries a trailing comma;
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
    culBuiltin)     echo support.class.culebra;;
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

# 9. Splice the block into the tmLanguage between the `\b\B` marker rules.
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

# 10. Emit the Zed tree-sitter builtin queries. The grammar already lexes a
#    capitalized name as its own `type` token, so only the builtin/user split
#    needs a word list; one query per culBuiltin map line, and a later pattern
#    on the same node wins, so these override the plain `(type) @type` above.
: > "$TMP_ZED_BLOCK"
while IFS= read -r line; do
  case "$line" in culBuiltin:*) ;; *) continue;; esac
  # shellcheck disable=SC2086
  set -- ${line#*:}
  pipe=$(printf '%s|' "$@"); pipe=${pipe%|}
  printf '((type) @type.builtin\n (#match? @type.builtin "^(%s)$"))\n' "$pipe" \
    >> "$TMP_ZED_BLOCK"
done < "$MAP"

awk -v block_file="$TMP_ZED_BLOCK" '
  /^; === BEGIN AUTO-BUILTINS/ {
    print
    while ((getline line < block_file) > 0) print line
    close(block_file)
    in_block = 1
    next
  }
  /^; === END AUTO-BUILTINS/ {
    in_block = 0
    print
    next
  }
  !in_block { print }
' "$ZED" > "$TMP_ZED"

# 11. Emit the CodeMirror (playground) KEYWORDS/CONSTANTS/BUILTINS sets —
#    culBoolean and culConstant fold into CONSTANTS (mirroring the tmLanguage's
#    separate constant.language.* scopes vs its keyword.* scopes), culBuiltin
#    into BUILTINS, and every other category folds into KEYWORDS. Only runs
#    where playground/culebra-lang.js exists.
if [[ -f "$CMLANG" ]]; then
  KW_LINES=$TMPD/kw_lines
  CONST_WORDS=$TMPD/const_words
  BUILTIN_LINES=$TMPD/builtin_lines
  : > "$KW_LINES"
  : > "$CONST_WORDS"
  : > "$BUILTIN_LINES"
  while IFS= read -r line; do
    case "$line" in ''|\#*) continue;; esac
    cat=${line%%:*}
    # shellcheck disable=SC2086
    set -- ${line#*:}
    quoted=$(printf '"%s", ' "$@"); quoted=${quoted%, }
    case "$cat" in
      culBoolean|culConstant) printf '%s ' "$@" >> "$CONST_WORDS" ;;
      culBuiltin)             echo "  $quoted," >> "$BUILTIN_LINES" ;;
      *)                      echo "  $quoted," >> "$KW_LINES" ;;
    esac
  done < "$MAP"
  {
    echo "const KEYWORDS = new Set(["
    cat "$KW_LINES"
    echo "]);"
    const_words=$(cat "$CONST_WORDS")
    const_quoted=$(printf '"%s", ' $const_words); const_quoted=${const_quoted%, }
    echo "const CONSTANTS = new Set([$const_quoted]);"
    echo "const BUILTINS = new Set(["
    cat "$BUILTIN_LINES"
    echo "]);"
  } > "$TMP_CMLANG_BLOCK"
  rm -f "$KW_LINES" "$CONST_WORDS" "$BUILTIN_LINES"

  awk -v block_file="$TMP_CMLANG_BLOCK" '
    /\/\/ === BEGIN AUTO-KEYWORDS/ {
      print
      while ((getline line < block_file) > 0) print line
      close(block_file)
      in_block = 1
      next
    }
    /\/\/ === END AUTO-KEYWORDS/ {
      in_block = 0
      print
      next
    }
    !in_block { print }
  ' "$CMLANG" > "$TMP_CMLANG"
fi

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
  if ! diff -q "$ZED" "$TMP_ZED" > /dev/null 2>&1; then
    echo "ERROR: $ZED AUTO-BUILTINS block is out of sync. Run \`just sync-grammar\`." >&2
    diff -u "$ZED" "$TMP_ZED" >&2 || true
    STATUS=1
  fi
  if [[ -f "$CMLANG" ]] && ! diff -q "$CMLANG" "$TMP_CMLANG" > /dev/null 2>&1; then
    echo "ERROR: $CMLANG AUTO-KEYWORDS block is out of sync. Run \`just sync-grammar\`." >&2
    diff -u "$CMLANG" "$TMP_CMLANG" >&2 || true
    STATUS=1
  fi
  exit $STATUS
fi

cp "$TMP_PEG" "$PEG"
cp "$TMP_VIM" "$VIM"
cp "$TMP_TM" "$TM"
cp "$TMP_ZED" "$ZED"
if [[ -f "$CMLANG" ]]; then
  cp "$TMP_CMLANG" "$CMLANG"
  echo "synced $PEG, $VIM, $TM, $ZED, and $CMLANG from $PARSER and $MAP"
else
  echo "synced $PEG, $VIM, $TM, and $ZED from $PARSER and $MAP"
fi
