#!/usr/bin/env bash
# CodeGen <-> cpp-vmlib IR vocabulary drift gate.
#
# vendor/cpp-vmlib/vmlib.h owns the IR's vocabulary now (coreir::name_of /
# from_name); include/stdlib/codegen.h's to_kind/to_unop/to_binop/
# to_intrinsic just call it (see "codegen: one vocabulary, defined
# upstream"). That closed the drift this gate used to have to catch on the
# BUILD side -- a member cpp-vmlib gains is automatically accepted the
# moment the submodule bumps. What's left, and what this checks:
#
#   1. Every UnOp/BinOp/VarKind/IntrinsicId/ConstKind member's name_of()
#      string is documented, in both docs/stdlib.md and docs/stdlib.ja.md's
#      section 35 -- a new member is usable from a script the moment the
#      submodule bumps, but silently undocumented until someone notices.
#   2. Every Tag has a row in the TAG_MAP table below: its name_of() string,
#      a builder method that produces it, and a reader method that can read
#      it back. A new Tag has neither until someone writes them -- this is
#      the "give it a builder and a reader" half of the same reminder.
#
# Parsing vmlib.h has bitten this script twice already, so both fixes are
# load-bearing, not stylistic:
#   - `sed -n '/start/,/end/p'` does not stop on the SAME line a single-line
#     enum's start and end pattern both match (UnOp, VarKind: `enum class
#     UnOp : uint8_t { Neg, BitNot };` is one line) -- it keeps scanning for
#     the NEXT `};`, which happens to be the FOLLOWING enum's. Extraction
#     below uses awk instead, checking the end pattern on every line
#     INCLUDING the one that set the start flag.
#   - IntrinsicId's own doc comments contain a literal "};" inside prose
#     ("... answers {value: nil, done: true}; resuming one ...") -- a normal
#     word-boundary-free NR isn't enough to check against, hence the
#     comment strip on every line before checking for the closing brace.
#
# Usage:
#   tools/checks/check_codegen_enum_coverage.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

VMLIB="vendor/cpp-vmlib/vmlib.h"
CODEGEN_BINDING="include/stdlib/codegen_binding.h"
STDLIB_MD="docs/stdlib.md"
STDLIB_JA="docs/stdlib.ja.md"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/culebra-cgenum.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail=0
note() {
  echo "check_codegen_enum_coverage: $*" >&2
  fail=1
}

# --- Extraction -------------------------------------------------------------

# Every member of `enum class <name> ... { A, B, C };` in vmlib.h, one per
# line, in declaration order. See the header comment above for why this is
# awk (not sed's range addressing) and why comments are stripped per-line
# before the closing-brace check, not after.
extract_members() {
  local name="$1"
  awk -v pat="^enum class ${name} " '
    { line = $0; sub(/\/\/.*/, "", line) }
    $0 ~ pat { grab = 1 }
    grab { print line; if (line ~ /\};/) exit }
  ' "$VMLIB" \
    | tr '\n' ' ' \
    | sed -E 's/.*\{(.*)\}.*/\1/' \
    | tr ',' '\n' \
    | sed -E 's/=.*//' \
    | sed -E 's/^[[:space:]]+|[[:space:]]+$//g' \
    | grep -E '^[A-Z][A-Za-z0-9]*$' || true
}

tag_members=$(extract_members Tag)
unop_members=$(extract_members UnOp)
binop_members=$(extract_members BinOp)
intrinsic_members=$(extract_members IntrinsicId)
varkind_members=$(extract_members VarKind)
constkind_members=$(extract_members ConstKind)

# Self-check: a parse that stopped matching vmlib.h must fail loudly, never
# pass with an empty (or truncated) list. Floors are each enum's member
# count as of this writing; cpp-vmlib only grows these.
check_floor() {
  local name="$1" list="$2" floor="$3"
  local n
  n=$(echo "$list" | grep -c . || true)
  if [ "$n" -lt "$floor" ]; then
    note "its own parse of ${name} found only ${n} member(s) (expected >= ${floor}) -- fix this script before trusting it"
  fi
}
check_floor Tag "$tag_members" 24
check_floor UnOp "$unop_members" 2
check_floor BinOp "$binop_members" 16
check_floor IntrinsicId "$intrinsic_members" 24
check_floor VarKind "$varkind_members" 3
check_floor ConstKind "$constkind_members" 5

# Every method codegen_binding.h binds, by its SCRIPT-VISIBLE name (the
# string literal, not the C++ member). Joined with spaces first: several
# `.method<...>("name", ...)` calls wrap the string onto its own line, which
# a per-line grep would miss entirely.
bound_methods=$(tr '\n' ' ' < "$CODEGEN_BINDING" \
  | grep -oE '\.method<[^>]+>\([[:space:]]*"[a-z_0-9]+"' \
  | grep -oE '"[a-z_0-9]+"' | tr -d '"' | sort -u)
n_bound=$(echo "$bound_methods" | grep -c . || true)
if [ "$n_bound" -lt 46 ]; then
  note "found only ${n_bound} bound method(s) in ${CODEGEN_BINDING} (expected >= 46) -- its own parse stopped matching, fix this script before trusting it"
fi
method_exists() { echo "$bound_methods" | grep -qx -- "$1"; }

# --- 1. UnOp/BinOp/VarKind/IntrinsicId/ConstKind: documented both ways -----
#
# name_of() lowercases the plain CamelCase identifier with no separator for
# every member of these five enums (BitAnd -> "bitand", ReadInt ->
# "readint", ...) -- Tag is the one enum where that mechanical rule does not
# hold (TryCatch -> "try", not "trycatch"), which is why it gets its own
# explicit map in part 2, checked a different way (see there for why).
lower() { echo "$1" | tr 'A-Z' 'a-z'; }

section_slice() {
  # docs/stdlib{,.ja}.md section 35 (`## 35. ...` through the next `## `).
  sed -n '/^## 35\. /,/^## 36\. /p' "$1"
}
md_en=$(section_slice "$STDLIB_MD")
md_ja=$(section_slice "$STDLIB_JA")
if [ -z "$md_en" ] || [ -z "$md_ja" ]; then
  note "section 35 (CodeGen) is empty in ${STDLIB_MD} or ${STDLIB_JA} -- did the heading move or get renumbered?"
fi

# Every code span (`...`) in the section, one per line -- the only place a
# vocabulary word is meaningfully "documented"; prose can and does contain
# common-word overlaps (if/while) that would false-positive a plain
# whole-section grep. Most rows quote a single value ('bitand'); the
# `m.binary(op:, ...)` row instead lists all sixteen as bare, space-
# separated words inside one span, so a word is looked for bounded by either
# a quote or a non-identifier character (space, backtick) -- not `\b`, which
# BSD grep (the macOS default) does not support in -E.
code_spans() { grep -oE '`[^`]*`' <<< "$1" || true; }
spans_en=$(code_spans "$md_en")
spans_ja=$(code_spans "$md_ja")

check_documented() {
  local enum_name="$1" member="$2"
  local word pat
  word=$(lower "$member")
  pat="'${word}'|(^|[^A-Za-z_])${word}([^A-Za-z_]|\$)"
  if ! grep -qE -- "$pat" <<< "$spans_en"; then
    note "${enum_name}::${member} ('${word}') is not documented in ${STDLIB_MD} section 35"
  fi
  if ! grep -qE -- "$pat" <<< "$spans_ja"; then
    note "${enum_name}::${member} ('${word}') is not documented in ${STDLIB_JA} section 35"
  fi
}
while IFS= read -r m; do [ -n "$m" ] && check_documented UnOp "$m"; done <<< "$unop_members"
while IFS= read -r m; do [ -n "$m" ] && check_documented BinOp "$m"; done <<< "$binop_members"
while IFS= read -r m; do [ -n "$m" ] && check_documented VarKind "$m"; done <<< "$varkind_members"
while IFS= read -r m; do [ -n "$m" ] && check_documented IntrinsicId "$m"; done <<< "$intrinsic_members"
while IFS= read -r m; do [ -n "$m" ] && check_documented ConstKind "$m"; done <<< "$constkind_members"

# --- 2. Tag: a builder and a reader, per member ----------------------------
#
# member|tag string|builder method|reader method. Unlike the other five
# enums, a Tag's name_of() string is never a script-visible ARGUMENT (there
# is no `kind: 'if'` parameter anywhere) -- node_tag() only ever returns
# one, and docs/stdlib.md's own convention is to give a couple of example
# return values, not enumerate all 24 (dump_ir() already prints the exact
# same word for anyone who wants to see it). So the tag string column here
# is documentation for whoever maintains this table, not something checked
# against the docs; the builder and the reader are the two facts that
# actually catch a new, unwired Tag. The reader is node_tag (the universal,
# always-present one) for a tag whose only payload is its children --
# num_children/child already reads those generically -- and a tag-specific
# accessor for one that carries scalar fields of its own.
TAG_MAP="
Literal|literal|literal|const_kind
VarRef|varref|var_ref|var_kind
Unary|unary|unary|node_op
Binary|binary|binary|node_op
Assign|assign|assign|var_kind
If|if|make_if|node_tag
While|while|make_while|node_tag
Block|block|block|node_tag
Intrinsic|intrinsic|intrinsic|node_op
MakeClosure|makeclosure|make_closure|closure_func
CallValue|callvalue|call_value|node_tag
ArrayLit|arraylit|array_lit|node_tag
ObjectLit|objectlit|object_lit|node_tag
Index|index|index|node_tag
SetIndex|setindex|set_index|node_tag
Scope|scope|scope|scope_first_local
Return|return|make_return|node_tag
Break|break|make_break|node_tag
Continue|continue|make_continue|node_tag
Throw|throw|make_throw|node_tag
TryCatch|try|make_try|try_caught_local
Defer|defer|make_defer|node_tag
CellFresh|cellfresh|cell_fresh|cell_index
Yield|yield|make_yield|node_tag
"

mapped_members=$(echo "$TAG_MAP" | awk -F'|' 'NF==4{print $1}')
n_mapped=0
while IFS='|' read -r member tagstr builder reader; do
  [ -z "$member" ] && continue
  n_mapped=$((n_mapped + 1))
  if ! method_exists "$builder"; then
    note "Tag::${member}'s mapped builder '${builder}' is not bound in ${CODEGEN_BINDING}"
  fi
  if ! method_exists "$reader"; then
    note "Tag::${member}'s mapped reader '${reader}' is not bound in ${CODEGEN_BINDING}"
  fi
done <<< "$(echo "$TAG_MAP" | grep -F '|')"

# Every Tag vmlib.h declares must have a row -- a new one has neither a
# builder nor a reader until someone adds both, then adds it here.
while IFS= read -r m; do
  [ -z "$m" ] && continue
  if ! grep -qx -- "$m" <<< "$mapped_members"; then
    note "vmlib.h grew Tag::${m} -- give it a builder and a reader in include/stdlib/codegen.h, then add its row to TAG_MAP in this script"
  fi
done <<< "$tag_members"

# The reverse: a mapped name that is no longer a real Tag member (a rename,
# or a member removed) is dead weight this script should not silently carry.
while IFS= read -r m; do
  [ -z "$m" ] && continue
  if ! grep -qx -- "$m" <<< "$tag_members"; then
    note "TAG_MAP names Tag::${m}, which vmlib.h no longer declares -- remove its row"
  fi
done <<< "$mapped_members"

if [ "$n_mapped" -lt 24 ]; then
  note "TAG_MAP has only ${n_mapped} row(s) (expected >= 24) -- its own table shrank"
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi

n_enum_members=$(( $(echo "$tag_members" | grep -c .) \
  + $(echo "$unop_members" | grep -c .) \
  + $(echo "$binop_members" | grep -c .) \
  + $(echo "$intrinsic_members" | grep -c .) \
  + $(echo "$varkind_members" | grep -c .) \
  + $(echo "$constkind_members" | grep -c .) ))
echo "codegen-enums OK (6 enums, ${n_enum_members} members, ${n_mapped} tags mapped, ${n_bound} bound methods)"
