#!/usr/bin/env bash
# SPIKE (not a release script yet): pack a Windows "link kit" out of the MSYS2
# UCRT64 tree this build uses — the files an AOT link needs and nothing else.
#
# `culebra build` emits its own object, so the compiler proper is not part of a
# link. What is: whatever orders the command, the linker, the CRT objects, and
# the static libs (libstdc++, libgcc, the mingw runtime, the Win32 import libs).
# Packing those out of the SAME tree that compiled the embedded runtime archives
# is what makes the kit's libstdc++ match theirs by construction — a
# third-party toolchain download would have to be version-matched by hand at
# every release.
#
# The driver is clang++ linking through lld, not g++/GNU ld: that is what
# src/main.cc runs (kLinkDriver) and what misc/configure_windows_release.sh
# builds the archives with. The earlier spike measured the g++/ld pair, which
# no longer exists on this path — hence this rewrite.
#
# Every path comes from the driver itself (-print-prog-name / -print-file-name /
# -###): the layout under a toolchain install is not ours to predict, and
# guessing it is what failed first last time. The kit mirrors the tree's own
# relative layout, because that is how a driver finds its siblings — from its
# own location, not from PATH.
#
# Two kits, because they answer different questions:
#   clang — clang++ + lld + libs. `culebra build` needs no change: it finds
#           clang++ on PATH exactly as it does today. The size ceiling.
#   lld   — lld + libs only, plus link-recipe.txt: the linker command line
#           clang++ WOULD have produced, split at the user object so a caller
#           can rebuild it. Needs driver work; the size floor.
#
# The libraries are not a hardcoded list. Every feature axis `culebra build`
# can append (CMakeLists' _ssl_link / _zlib_link / _raylib_aot_link /
# _webview_link) is put through the driver here, and the union of what those
# link lines resolve to is what the kit carries. A list maintained by hand is
# how an axis that only the release build reaches ends up missing its import
# library.
#
# Usage: misc/pack_linkkit.sh <out dir>   (run from an MSYS2 UCRT64 shell)
set -euo pipefail

out=${1:?usage: pack_linkkit.sh <out dir>}
mkdir -p "$out"

# The driver answers in Windows paths (it is a native binary); this shell wants
# POSIX ones. Normalize everything on the way in.
u() { if command -v cygpath >/dev/null; then cygpath -u "$1"; else printf '%s' "$1"; fi; }

drv=$(u "$(command -v clang++)")
root=$(cd "$(dirname "$drv")/.." && pwd)          # .../ucrt64
target=$(clang++ -dumpmachine)
drvver=$(clang++ -dumpversion)

echo "== source tree: $root"
echo "   target $target, clang $drvver"

find_prog() {   # <name> -> absolute path, or empty
  local p
  p=$(clang++ -print-prog-name="$1" 2>/dev/null || true)
  p=$(u "$p")
  [ -f "$p" ] && { printf '%s' "$p"; return; }
  find "$root" \( -name "$1" -o -name "$1.exe" \) 2>/dev/null | head -1
}

find_file() {   # <libname> -> absolute path, or empty
  local p
  p=$(clang++ -print-file-name="$1" 2>/dev/null || true)
  p=$(u "$p")
  [ -f "$p" ] && printf '%s' "$p"
}

rel_of() {   # <abs path under $root> -> path relative to $root
  local p=$1
  case "$p" in "$root"/*) printf '%s' "${p#"$root"/}" ;; *) return 1 ;; esac
}

copy_into() {   # <kit> <abs src>
  local kit=$1 src=$2 rel
  rel=$(rel_of "$src") || return 0          # outside the tree: not ours to ship
  [ -f "$kit/$rel" ] && return 0
  mkdir -p "$kit/$(dirname "$rel")"
  cp "$src" "$kit/$rel"
}

# The DLLs beside these executables are MSYS2's own (libwinpthread, libc++,
# libLLVM, …); a kit without them cannot start the linker on a machine that has
# no MSYS2. Anything ldd reports outside the tree is a system DLL the target
# already has. Walked transitively: MSYS2's clang reaches its LLVM through more
# than one hop.
copy_exe() {   # <kit> <abs exe>
  local kit=$1 exe=$2 dll seen
  copy_into "$kit" "$exe"
  seen=$(ldd "$exe" 2>/dev/null | awk '{print $3}' | grep -v '^$' || true)
  for dll in $seen; do
    dll=$(u "$dll")
    [ -f "$dll" ] || continue
    rel_of "$dll" >/dev/null 2>&1 || continue      # system DLL: already there
    if [ ! -f "$kit/$(rel_of "$dll")" ]; then
      copy_into "$kit" "$dll"
      copy_exe "$kit" "$dll"                       # its own imports, in turn
    fi
  done
}

# --- what the driver would run ---------------------------------------------
# `-###` prints the link line without running it. Every token comes back
# quoted, which is what makes it parseable: the CRT objects arrive as absolute
# paths, the libraries as -L/-l, and the linker itself as argv[0].
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
printf 'int main(){return 0;}\n' > "$probe/probe.cc"
clang++ -c "$probe/probe.cc" -o "$probe/probe.o"

# The base flags src/main.cc puts on a Windows AOT link (win_static + libcxx),
# kept verbatim so this kit is packed for the link that actually happens.
BASE_FLAGS=(-fuse-ld=lld -static -static-libgcc -static-libstdc++
            -lstdc++exp -lws2_32 -Wl,--stack,16777216 -lstdc++ -lm)

# Every axis CMakeLists can append to that line, minus the parts `culebra
# build` materializes out of its own embedded copies (@libssl.a@ and friends):
# those are the driver's, not the toolchain's. What is left is exactly the
# toolchain-side surface a kit has to cover.
declare -A AXIS=(
  [base]=""
  [http]="-lz -lcrypt32 -lbcrypt -lws2_32"
  [compress]="-lz"
  [canvas]="-lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"
  [webview]="-lole32 -lshell32 -lshlwapi -luser32 -lversion -ladvapi32 -Wl,--allow-multiple-definition"
)

# Tokenize a -### line: every argument is "..."-quoted, so this is exact
# where a word split would not be (paths under Program Files have spaces).
link_line_for() {   # <extra args...> -> the linker argv, one token per line
  local log="$probe/v.log"
  clang++ -### "$probe/probe.o" -o "$probe/probe.exe" \
      "${BASE_FLAGS[@]}" "$@" > /dev/null 2> "$log" || true
  # The linker call is the last -### line naming the linker binary.
  local line
  line=$(grep -E '"[^"]*(ld\.lld|ld|lld-link|link)(\.exe)?"' "$log" | tail -1)
  [ -n "$line" ] || { echo "pack_linkkit: no link line in -### output" >&2
                      sed -n '1,60p' "$log" >&2; return 1; }
  printf '%s\n' "$line" | grep -o '"[^"]*"' | sed 's/^"//; s/"$//'
}

echo "== resolving the link line for each axis"
: > "$probe/all-tokens"
for axis in "${!AXIS[@]}"; do
  # shellcheck disable=SC2086
  if link_line_for ${AXIS[$axis]} > "$probe/line-$axis"; then
    printf '   %-9s %d tokens\n' "$axis" "$(wc -l < "$probe/line-$axis")"
    cat "$probe/line-$axis" >> "$probe/all-tokens"
  else
    printf '   %-9s FAILED\n' "$axis"
  fi
done

linker=$(head -1 "$probe/line-base")
linker=$(u "$linker")
echo "   linker: $linker"

# -L dirs and -l names, unioned over every axis; plus any absolute path the
# lines name directly (the CRT objects).
libdirs=$(grep '^-L' "$probe/all-tokens" | sed 's/^-L//' | while read -r d; do u "$d"; done | sort -u)
libnames=$(grep '^-l' "$probe/all-tokens" | sed 's/^-l//' | sort -u)
direct=$(grep -E '^([A-Za-z]:[\\/]|/)' "$probe/all-tokens" | while read -r p; do u "$p"; done | sort -u)

resolve_lib() {   # <name> -> absolute path, or empty
  local n=$1 d cand
  for d in $libdirs; do
    for cand in "lib$n.a" "lib$n.dll.a" "$n.lib" "lib$n.lib"; do
      [ -f "$d/$cand" ] && { printf '%s' "$d/$cand"; return; }
    done
  done
  find_file "lib$n.a"
}

pack_libs() {   # <kit>
  local kit=$1 n p
  for p in $direct; do
    [ -f "$p" ] && copy_into "$kit" "$p"
  done
  for n in $libnames; do
    p=$(resolve_lib "$n" || true)
    if [ -n "$p" ]; then copy_into "$kit" "$p"; else echo "   ! -l$n unresolved"; fi
  done
  # clang picks its unwinder/builtins runtime by name rather than by -l on
  # mingw; ask it directly rather than hope the line spelled it.
  p=$(u "$(clang++ -print-libgcc-file-name 2>/dev/null || true)")
  [ -f "$p" ] && copy_into "$kit" "$p"
}

# --- kit-clang -------------------------------------------------------------
# What `culebra build` runs today, unchanged: clang++ on PATH, lld behind it.
clangkit="$out/kit-clang"
rm -rf "$clangkit"; mkdir -p "$clangkit"
echo "== kit-clang"
copy_exe "$clangkit" "$drv"
copy_exe "$clangkit" "$linker"
# `culebra build` checks for ld.lld by name before it will link at all
# (have_link_driver in src/main.cc), so the kit must satisfy that probe too.
p=$(find_prog ld.lld); [ -n "$p" ] && copy_exe "$clangkit" "$p"
pack_libs "$clangkit"

# --- kit-lld ---------------------------------------------------------------
# The linker and the libraries, without the compiler driver. The recipe below
# is what replaces the driver: the argv clang++ would have built, split at the
# user object so a caller can put its own objects and axis libs in the middle.
lldkit="$out/kit-lld"
rm -rf "$lldkit"; mkdir -p "$lldkit"
echo "== kit-lld"
copy_exe "$lldkit" "$linker"
pack_libs "$lldkit"

recipe="$lldkit/link-recipe.txt"
{
  echo "# Generated by misc/pack_linkkit.sh from $target clang $drvver."
  echo "# The linker argv clang++ builds for a culebra AOT link, with paths"
  echo "# inside the toolchain rewritten to @KIT@. A caller emits:"
  echo "#     <linker> <PREFIX> <its objects and -l flags> <SUFFIX>"
  echo "# where the object clang++ was given marked the split point."
  echo "LINKER $(rel_of "$linker")"
} > "$recipe"

# Split the base line at the probe object. Everything before it is setup
# (target, CRT, -L); everything after is the default library set.
awk -v obj="$(cygpath -m "$probe/probe.o" 2>/dev/null || printf '%s' "$probe/probe.o")" \
    -v objalt="$probe/probe.o" \
    -v outexe="$(cygpath -m "$probe/probe.exe" 2>/dev/null || printf '%s' "$probe/probe.exe")" \
    -v outalt="$probe/probe.exe" '
  NR == 1 { next }                                  # argv[0]: the linker
  $0 == obj || $0 == objalt { print "--- SUFFIX"; next }
  $0 == outexe || $0 == outalt { next }             # -o value: the caller sets it
  $0 == "-o" { next }
  { print }
' "$probe/line-base" > "$probe/split"

{
  echo "--- PREFIX"
  sed -n '1,/^--- SUFFIX$/p' "$probe/split" | sed '$d'
  echo "--- SUFFIX"
  sed -n '/^--- SUFFIX$/,$p' "$probe/split" | sed '1d'
} | while IFS= read -r tok; do
  # Rewrite toolchain-internal paths so the recipe is relocatable.
  case "$tok" in
    -L*)  d=$(u "${tok#-L}"); if r=$(rel_of "$d" 2>/dev/null); then echo "-L@KIT@/$r"; else echo "$tok"; fi ;;
    [A-Za-z]:[\\/]*|/*) d=$(u "$tok"); if r=$(rel_of "$d" 2>/dev/null); then echo "@KIT@/$r"; else echo "$tok"; fi ;;
    *) echo "$tok" ;;
  esac
done >> "$recipe"

# --- report ----------------------------------------------------------------
printf '\n== kit sizes\n'
for k in "$clangkit" "$lldkit"; do
  printf '%-10s %8s  (%d files)\n' "$(basename "$k")" \
    "$(du -sh "$k" | cut -f1)" "$(find "$k" -type f | wc -l)"
done

printf '\n== the ten largest files in each kit\n'
for k in "$clangkit" "$lldkit"; do
  echo "-- $(basename "$k")"
  find "$k" -type f -printf '%s\t%P\n' | sort -rn | head -10 |
    awk -F'\t' '{printf "   %12d  %s\n", $1, $2}'
done

printf '\n== kit-lld contents\n'
(cd "$lldkit" && find . -type f | sort | sed 's/^\.\///')

printf '\n== link recipe\n'
cat "$recipe"
