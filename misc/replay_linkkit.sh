#!/usr/bin/env bash
# SPIKE: re-link a `culebra build` output using kit-lld's recipe instead of the
# clang++ driver, to find out whether the driver is separable from the link.
#
# `culebra build` prints the exact command it ran under CULEBRA_VERBOSE:
#     culebra build: link: clang++ "…obj" -o "…exe" <base flags> <axis flags>
# The recipe (misc/pack_linkkit.sh) holds what clang++ WOULD have expanded the
# base flags into — the linker argv, split at the user object. So the question
# this answers is whether
#     ld.lld <PREFIX> <culebra's own objects and axis flags> -o out <SUFFIX>
# links and runs. If it does, a kit needs no compiler driver at all, and the
# driver's job reduces to substituting two lists it can read from a text file.
#
# Usage: misc/replay_linkkit.sh <kit root> <verbose log> <out exe>
set -euo pipefail

kit=${1:?usage: replay_linkkit.sh <kit root> <verbose log> <out exe>}
log=${2:?usage: replay_linkkit.sh <kit root> <verbose log> <out exe>}
out=${3:?usage: replay_linkkit.sh <kit root> <verbose log> <out exe>}

kit=$(cd "$kit" && pwd)
recipe="$kit/link-recipe.txt"
[ -f "$recipe" ] || { echo "replay: no recipe at $recipe" >&2; exit 1; }

# The base flags pack_linkkit.sh already expanded into the recipe. Anything
# else on culebra's line is the part only culebra knows: its object, the
# archives it materialized out of itself, and the axis libraries.
BASE_FLAGS=" -fuse-ld=lld -static -static-libgcc -static-libstdc++ -lstdc++exp -lws2_32 -Wl,--stack,16777216 -lstdc++ -lm "

linker=$(awk '$1 == "LINKER" { print $2 }' "$recipe")
[ -n "$linker" ] || { echo "replay: recipe names no LINKER" >&2; exit 1; }
linker="$kit/$linker"
[ -x "$linker" ] || { echo "replay: $linker is not executable" >&2; exit 1; }

section() {   # <name> -> that section's tokens, @KIT@ resolved
  awk -v want="--- $1" '
    $0 == want { on = 1; next }
    /^--- / { on = 0 }
    on && !/^#/ { print }
  ' "$recipe" | sed "s|@KIT@|$kit|g"
}

mapfile -t PREFIX < <(section PREFIX)
mapfile -t SUFFIX < <(section SUFFIX)

cmd=$(sed -n 's/^culebra build: link: //p' "$log" | tail -1)
[ -n "$cmd" ] || { echo "replay: no link command in $log (CULEBRA_VERBOSE unset?)" >&2; exit 1; }

# The command was built for the platform shell with "…" quoting, which is also
# how the shell here splits it. Backslashes inside Windows paths survive: in a
# double-quoted word only $ ` " \ are special after a backslash.
eval "set -- $cmd"
shift   # argv[0]: the driver being replaced

mid=()
skip_next=0
for tok in "$@"; do
  if [ "$skip_next" = 1 ]; then skip_next=0; continue; fi
  case "$tok" in
    -o) skip_next=1; continue ;;                     # the caller names the output
    -Wl,*)
      # clang passes these through after splitting on commas; do the same.
      IFS=, read -r -a parts <<< "${tok#-Wl,}"
      for p in "${parts[@]}"; do mid+=("$p"); done
      continue ;;
  esac
  # A base flag the recipe already carries, or something only culebra knows.
  case "$BASE_FLAGS" in
    *" $tok "*) continue ;;
  esac
  mid+=("$tok")
done

echo "== replay: $(basename "$out")"
printf '   prefix %d tokens, culebra %d tokens, suffix %d tokens\n' \
  "${#PREFIX[@]}" "${#mid[@]}" "${#SUFFIX[@]}"
printf '   culebra-side: %s\n' "${mid[*]}"

set -x
"$linker" "${PREFIX[@]}" "${mid[@]}" -o "$out" "${SUFFIX[@]}"
