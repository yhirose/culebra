#!/usr/bin/env bash
# Registrar ratchet: a header variable that exists only to run its initializer
# must carry [[gnu::used]].
#
# `inline const bool _x = []{ ...register...; return true; }();` is how this
# tree installs a hook or registers a wrapped class (wrap.h documents the
# idiom). Nothing ever reads the variable, and an inline variable nothing
# odr-uses need never be initialized at all ([basic.start.dynamic]) — lld's
# COFF --gc-sections takes exactly that liberty and drops the COMDAT with the
# registration inside it. The failure is silent and Windows-only: the build
# links, and the registration's effect is just missing at run time
# (`Http.get(url, headers: h)` answering "does not accept keyword arguments").
# [[gnu::used]] is what pins it, and this is what keeps every site carrying it.
#
# Headers only: a variable at file scope in a .cc is not a COMDAT, so its
# initializer is rooted by the TU's .init_array and none of this applies.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
checked=0
while IFS= read -r file; do
  # A namespace-scope definition whose initializer opens a lambda, minus the
  # ones already pinned. `^inline` anchors it to namespace scope: a member or
  # a function-local static is indented, and a local static is odr-used by the
  # function that holds it anyway.
  while IFS= read -r line; do
    checked=$((checked + 1))
    case "$line" in
      *'[[gnu::used]]'*) ;;
      *) echo "  $file: $line" >&2; fail=1;;
    esac
  done < <(grep -nE '^(\[\[gnu::used\]\] )?inline [A-Za-z_].*= *\[\] *\{' "$file" || true)
done < <(git ls-files 'include/*.h' 'include/**/*.h')

if [[ $fail -ne 0 ]]; then
  echo "check_registrar_used: the definitions above run only for their" >&2
  echo "  initializer's side effect; add [[gnu::used]] so a linker that" >&2
  echo "  garbage-collects unreferenced COMDATs cannot drop them (see the" >&2
  echo "  idiom in include/wrap.h)." >&2
  exit 1
fi
echo "registrar-used OK ($checked pinned)"
