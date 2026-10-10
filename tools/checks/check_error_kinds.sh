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
#   - in the library's culebra source, the first argument of `__raise` /
#     `__raise_at`: in the modules, and in what the C++ sources hold of it
#     (the built-in traits' defaults, the code the transforms write), where
#     the quote may be escaped. In the modules also the `kind:` of an Object
#     literal whose value is a capitalized name (`ArgParseHelp`, which a
#     module throws as an Object; the lowercase ones — `key`, `mouse`,
#     `resize` — are event kinds, not errors).
#
# A kind that reaches a raise through a variable is not seen here, and a name
# in the list that nothing raises only costs a program's class of that name
# its typed reads.
set -euo pipefail
cd "$(dirname "$0")/../.."

listed=$(sed -n '/^inline bool is_error_kind_name/,/^}/p' include/base/shared.h \
         | grep -o '"[A-Za-z]*"' | tr -d '"' | sort -u) || true
# The C++ sources, folded, without their comment lines and without the baked
# copy of the modules (those are read below as they are written).
cxx=$(grep -rh --include='*.h' --include='*.cc' --exclude='preambles.gen.h' '' include src \
      | grep -vE '^[[:space:]]*//' | tr '\n' ' ')
native=$(grep -oE '(CulebraError|culebra_note_pending_error|culebra_runtime_throw_error|emit_throw_error|throw_runtime_error_at|emit_raise)[({][[:space:]]*"[A-Za-z]+"' <<<"$cxx" \
         | grep -oE '"[A-Za-z]+"' | tr -d '"' | sort -u) || true
source=$( { grep -rh '' src/preambles | tr '\n' ' ' \
              | grep -oE "(kind:|__raise(_at)?\()[[:space:]]*['\"][A-Z][A-Za-z]+"
            grep -oE "__raise(_at)?\([[:space:]]*\\\\?['\"][A-Z][A-Za-z]+" <<<"$cxx"
          } | grep -oE '[A-Z][A-Za-z]+$' | sort -u) || true

if [[ -z "$listed" || -z "$native" || -z "$source" ]]; then
  echo "error-kinds: an anchor moved (listed: $(wc -w <<<"$listed"), native: $(wc -w <<<"$native"), source: $(wc -w <<<"$source"))" >&2
  exit 1
fi

# A library module raises an error with `__raise`, which makes it the error a
# native raises. Thrown as an Object or a String it would be a program's
# throw: reported as `uncaught: ...`, with no position, and a user throw at
# every boundary. So a capitalized `kind` in the library's culebra source is
# one of the kinds it throws as an Object on purpose — a signal, not an
# error — or it is refused, and so is a thrown literal String. In what the
# C++ sources hold of the library's source (the built-in traits' defaults,
# the code the transforms write) the Object is found as a `throw {` with a
# capitalized `kind` among its keys; a thrown String there cannot be told
# from C++'s own and is not looked for.
as_object="ArgParseHelp"
kind_key="[\"']?kind[\"']?:[[:space:]]*\\\\?['\"][A-Z][A-Za-z]+"
thrown=$( { { grep -rnE "$kind_key" src/preambles
              grep -rnE "throw[[:space:]]+['\"\`]" src/preambles
            } | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(#|//)'
            grep -oE "throw[[:space:]]*\{+[^}]*$kind_key" <<<"$cxx"
          } | grep -vE "kind[\"']?:[[:space:]]*\\\\?['\"]($as_object)\\\\?['\"]" || true)
if [[ -n "$thrown" ]]; then
  echo "error-kinds: the library throws an error as an Object or a String (raise it with __raise):" >&2
  echo "$thrown" | cut -c1-160 | sed 's/^/  /' >&2
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
