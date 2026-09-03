#!/usr/bin/env bash
# Registrar ratchet: no header carries a variable that exists only to run its
# initializer.
#
# `inline const bool _x = []{ ...register...; return true; }();` is the obvious
# way to install a hook or register a wrapped class from a header, and it does
# not survive the Windows link. The variable is a COMDAT nothing odr-uses, an
# inline variable nothing odr-uses need never be initialized at all
# ([basic.start.dynamic]), and lld's --gc-sections takes that liberty. What
# actually runs the initializer is a separate static-init entry, associative to
# that COMDAT and collected with it — so [[gnu::used]] on the variable does not
# rescue it either. Both were measured on windows-aot-build: the kwarg hook
# vanished and `Http.get(url, headers: h)` answered "function does not accept
# keyword arguments".
#
# The failure is silent and Windows-only — the build links, and the effect is
# just missing at run time — so a rule is the only thing that keeps it away.
# Two shapes work, and this permits both:
#   - call it from a live path (stdlib_rt.h: _jit_ns_install_hooks, run from
#     the one function that makes the closures the hooks answer for);
#   - put the variable in a .cc, where it is not a COMDAT and its static-init
#     entry stays (wrap.h's idiom; foreign_binding.h exports the function two
#     .cc files call). .cc files are therefore not scanned.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
while IFS= read -r file; do
  # `^inline` anchors this to namespace scope: a member or a function-local
  # static is indented, and a local static is odr-used by its function anyway.
  while IFS= read -r line; do
    echo "  $file: $line" >&2
    fail=1
  done < <(grep -nE '^(\[\[[a-z:]+\]\] )*inline [A-Za-z_].*= *\[\] *\{' "$file" || true)
done < <(git ls-files 'include/*.h' 'include/**/*.h')

if [[ $fail -ne 0 ]]; then
  echo "check_registrar_rooting: the definitions above run only for their" >&2
  echo "  initializer's side effect, and a header cannot root one — see the" >&2
  echo "  two shapes that work in include/interop/wrap.h." >&2
  exit 1
fi
echo "registrar-rooting OK"
