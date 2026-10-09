#!/usr/bin/env bash
# Error-kind list gate.
#
# `catch IOError` takes an error by its kind, and the names that work that way
# are a list: culebra::is_error_kind_name (include/base/shared.h). A kind the
# sources raise and the list lacks is not caught by name, with nothing
# failing — the clause just never matches. So every kind raised by a literal
# has to be in the list:
#
#   - in C++, the first argument of a CulebraError built in place, of
#     culebra_note_pending_error, and of the helpers that raise one
#     (culebra_runtime_throw_error, emit_throw_error, throw_runtime_error_at,
#     emit_raise).
#     A call is often wrapped after the parenthesis, so the sources are read
#     with their newlines folded (check_codegen_enum_coverage.sh's reason);
#   - in the standard library's culebra-source modules, the `kind:` of an
#     Object literal whose value ends in `Error`.
#
# A kind that reaches a raise through a variable is not seen here, and a name
# in the list that nothing raises only costs a program's class of that name
# its typed reads.
set -euo pipefail
cd "$(dirname "$0")/../.."

listed=$(sed -n '/^inline bool is_error_kind_name/,/^}/p' include/base/shared.h \
         | grep -o '"[A-Za-z]*"' | tr -d '"' | sort -u) || true
native=$(grep -rh --include='*.h' --include='*.cc' '' include src | tr '\n' ' ' \
         | grep -oE '(CulebraError|culebra_note_pending_error|culebra_runtime_throw_error|emit_throw_error|throw_runtime_error_at|emit_raise)[({][[:space:]]*"[A-Za-z]+"' \
         | grep -oE '"[A-Za-z]+"' | tr -d '"' | sort -u) || true
source=$(grep -rhoE "kind:[[:space:]]*['\"][A-Za-z]+Error['\"]" src/preambles \
         | grep -oE "[A-Za-z]+Error" | sort -u) || true

if [[ -z "$listed" || -z "$native" || -z "$source" ]]; then
  echo "error-kinds: an anchor moved (listed: $(wc -w <<<"$listed"), native: $(wc -w <<<"$native"), source: $(wc -w <<<"$source"))" >&2
  exit 1
fi

raised=$(printf '%s\n%s\n' "$native" "$source" | sort -u)
missing=$(comm -13 <(echo "$listed") <(echo "$raised"))
if [[ -n "$missing" ]]; then
  echo "error-kinds: raised but not in is_error_kind_name (include/base/shared.h):" >&2
  echo "$missing" | sed 's/^/  /' >&2
  exit 1
fi
echo "error-kinds OK ($(wc -w <<<"$raised" | tr -d ' ') raised by name, $(wc -w <<<"$listed" | tr -d ' ') listed)"
