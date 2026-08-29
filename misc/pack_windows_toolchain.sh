#!/usr/bin/env bash
# Pack the Windows AOT toolchain kit: the files `culebra build` needs to link
# on a machine that has never had a compiler installed.
#
# The kit holds NO executables. culebra links lld into itself (CMakeLists, the
# CULEBRA_INPROCESS_LLD block) — it already carries LLVM for the JIT, and lld
# is 4 MB of library on top of that — so what a downloader is missing is only
# the mingw side of a link: the CRT objects, libstdc++/libgcc, and the Win32
# import libraries. Shipping ld.lld.exe instead would mean shipping the 147 MB
# libLLVM DLL it links against; measured, that is the difference between a
# 6 MB download and a 55 MB one.
#
# Packed from the SAME MSYS2 tree that compiled the embedded runtime archives,
# which is what makes the kit's libstdc++ match theirs by construction. A
# third-party toolchain would have to be version-matched by hand at every
# release. release.yml therefore runs this in its windows-x64 job, after the
# build and before the upload.
#
# Every path comes from the compiler driver (-print-file-name / -###), never
# from a guess about the install layout: that layout is not ours to predict,
# and guessing it is what failed the first spike. The kit mirrors the tree's
# own relative directory names so the recipe's -L paths stay meaningful.
#
# The library list is not maintained by hand. Every feature axis `culebra
# build` can append (CMakeLists' _ssl_link / _zlib_link / _raylib_aot_link /
# _webview_link) is put through the driver here, and the union of what those
# link lines resolve to is what gets packed — a hand-written list is how an
# axis that only a released binary reaches ends up missing its import library.
#
# Usage: misc/pack_windows_toolchain.sh <out dir> <version>
#   <out dir>  where to write the kit tree and the .zip
#   <version>  the culebra version this kit accompanies, e.g. 0.5.0
# Prints the files it wrote, one per line, for the caller to upload.
set -euo pipefail

out=${1:?usage: pack_windows_toolchain.sh <out dir> <version>}
version=${2:?usage: pack_windows_toolchain.sh <out dir> <version>}

mkdir -p "$out"
out=$(cd "$out" && pwd)
kit="$out/culebra-toolchain-windows-x64"
rm -rf "$kit"; mkdir -p "$kit"

# The driver answers in Windows paths (it is a native binary); this shell wants
# POSIX ones. Normalize everything on the way in.
u() { if command -v cygpath >/dev/null; then cygpath -u "$1"; else printf '%s' "$1"; fi; }

drv=$(u "$(command -v clang++)")
root=$(cd "$(dirname "$drv")/.." && pwd)          # .../ucrt64
target=$(clang++ -dumpmachine)
drvver=$(clang++ -dumpversion)

# Where it got to, on stderr: this runs inside a release job, and an exit
# status with no context is what the first CI run of it produced.
step() { echo "== $*" >&2; }

step "packing from $root ($target, clang $drvver)"

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

copy_into() {   # <abs src>
  local src=$1 rel
  rel=$(rel_of "$src") || return 0          # outside the tree: not ours to ship
  [ -f "$kit/$rel" ] && return 0
  mkdir -p "$kit/$(dirname "$rel")"
  cp "$src" "$kit/$rel"
}

# --- what the driver would run ---------------------------------------------
step "probing the driver"
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
printf 'int main(){return 0;}\n' > "$probe/probe.cc"
clang++ -c "$probe/probe.cc" -o "$probe/probe.o"

# The flags src/main.cc's Windows link is built on. The recipe below records
# what clang++ expands these into, so the two cannot drift: culebra never
# spells the expansion itself, it splices this file.
BASE_FLAGS=(-fuse-ld=lld -static -static-libgcc -static-libstdc++
            -lstdc++exp -lws2_32 -Wl,--stack,16777216 -lstdc++ -lm)

# Every axis that can append to that line, minus the archives `culebra build`
# materializes out of its own embedded copies (@libssl.a@ and friends): those
# are the driver's, not the toolchain's. What is left is the toolchain-side
# surface a kit has to cover.
declare -A AXIS=(
  [base]=""
  [http]="-lz -lcrypt32 -lbcrypt -lws2_32"
  [compress]="-lz"
  [canvas]="-lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"
  [webview]="-lole32 -lshell32 -lshlwapi -luser32 -lversion -ladvapi32 -Wl,--allow-multiple-definition"
)

# Tokenize a -### line: every argument comes back "..."-quoted, which is what
# makes this exact where a word split would not be.
link_line_for() {   # <extra args...> -> the linker argv, one token per line
  local log="$probe/v.log" line
  clang++ -### "$probe/probe.o" -o "$probe/probe.exe" \
      "${BASE_FLAGS[@]}" "$@" > /dev/null 2> "$log" || true
  line=$(grep -E '"[^"]*(ld\.lld|ld|lld-link)(\.exe)?"' "$log" | tail -1)
  [ -n "$line" ] || { echo "pack_windows_toolchain: no link line in -### output" >&2
                      sed -n '1,60p' "$log" >&2; return 1; }
  printf '%s\n' "$line" | grep -o '"[^"]*"' | sed 's/^"//; s/"$//'
}

step "resolving the link line for each feature axis"
: > "$probe/all-tokens"
for axis in "${!AXIS[@]}"; do
  # shellcheck disable=SC2086
  if ! link_line_for ${AXIS[$axis]} > "$probe/line-$axis"; then
    echo "pack_windows_toolchain: could not resolve the $axis link line" >&2
    exit 1
  fi
  printf '   %-9s %d tokens\n' "$axis" "$(wc -l < "$probe/line-$axis")" >&2
  cat "$probe/line-$axis" >> "$probe/all-tokens"
done

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

step "copying the libraries those lines name"
missing=0
for p in $direct; do if [ -f "$p" ]; then copy_into "$p"; fi; done
for n in $libnames; do
  p=$(resolve_lib "$n" || true)
  if [ -n "$p" ]; then copy_into "$p"
  else echo "pack_windows_toolchain: -l$n does not resolve" >&2; missing=1; fi
done
# clang picks its unwinder/builtins runtime by name rather than by -l on mingw;
# ask it directly rather than hope the line spelled it.
p=$(u "$(clang++ -print-libgcc-file-name 2>/dev/null || true)")
if [ -f "$p" ]; then copy_into "$p"; fi
[ "$missing" = 0 ] || { echo "pack_windows_toolchain: incomplete kit" >&2; exit 1; }

# --- the link recipe -------------------------------------------------------
# The linker argv clang++ builds, split at the user object. culebra emits
#     <PREFIX> <its own objects and flags> -o <out> <SUFFIX>
# and never spells the toolchain's half itself.
step "writing the link recipe"
recipe="$kit/link-recipe.txt"
{
  echo "# The linker argv a C++ driver builds for a culebra AOT link on this"
  echo "# toolchain, with paths inside it rewritten to @KIT@. culebra splices"
  echo "# its own objects and axis flags between the two sections."
  echo "# Generated by misc/pack_windows_toolchain.sh — do not edit."
  echo "TARGET $target"
} > "$recipe"

awk -v obj="$(cygpath -m "$probe/probe.o" 2>/dev/null || printf '%s' "$probe/probe.o")" \
    -v objalt="$probe/probe.o" \
    -v outexe="$(cygpath -m "$probe/probe.exe" 2>/dev/null || printf '%s' "$probe/probe.exe")" \
    -v outalt="$probe/probe.exe" '
  NR == 1 { next }                                  # argv[0]: the linker
  $0 == obj || $0 == objalt { print "--- SUFFIX"; next }
  $0 == outexe || $0 == outalt { next }             # -o value: culebra sets it
  $0 == "-o" { next }
  { print }
' "$probe/line-base" > "$probe/split"

grep -q '^--- SUFFIX$' "$probe/split" ||
  { echo "pack_windows_toolchain: the probe object is not on the link line" >&2; exit 1; }

{
  echo "--- PREFIX"
  sed -n '1,/^--- SUFFIX$/p' "$probe/split" | sed '$d'
  echo "--- SUFFIX"
  sed -n '/^--- SUFFIX$/,$p' "$probe/split" | sed '1d'
} | while IFS= read -r tok; do
  case "$tok" in
    -L*)  d=$(u "${tok#-L}"); if r=$(rel_of "$d" 2>/dev/null); then echo "-L@KIT@/$r"; else echo "$tok"; fi ;;
    [A-Za-z]:[\\/]*|/*) d=$(u "$tok"); if r=$(rel_of "$d" 2>/dev/null); then echo "@KIT@/$r"; else echo "$tok"; fi ;;
    *) echo "$tok" ;;
  esac
done >> "$recipe"

# Nothing in a kit may name the machine that packed it: those paths do not
# exist on a downloader's disk, and a link that silently picks up a local file
# is the failure this whole design exists to avoid (docs/deployment.md §1).
if grep -nE '^(-L)?([A-Za-z]:[\\/]|/)' "$recipe" | grep -v '@KIT@'; then
  echo "pack_windows_toolchain: the recipe carries a build-machine path" >&2
  exit 1
fi

# --- manifest and licences -------------------------------------------------
# What this kit is, and what it must be paired with. `toolchain install`
# refuses a kit whose VERSION is not the running binary's: the libstdc++ here
# has to be the one culebra's embedded runtime archives were compiled against,
# and the release that shipped them is the only thing that says so.
{
  echo "VERSION $version"
  echo "TARGET $target"
  echo "CLANG $drvver"
  echo "PACKED $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$kit/MANIFEST.txt"

# GPLv3 code (libstdc++, libgcc — under the GCC Runtime Library Exception)
# is redistributed here, so the sources have to be named. MSYS2 builds every
# package from a recipe in its own repository; recording the exact package
# versions is what makes those recipes findable.
step "recording the manifest and licences"
mkdir -p "$kit/LICENSES"
{
  echo "This kit redistributes files from the MSYS2 UCRT64 packages listed"
  echo "below. Their sources are at https://github.com/msys2/MINGW-packages"
  echo "(recipe per package) and the upstream projects they build."
  echo
  if command -v pacman >/dev/null; then
    find "$kit" \( -name '*.a' -o -name '*.o' \) -print0 |
      xargs -0 -r pacman -Qo 2>/dev/null |
      sed 's/.* is owned by //' | sort -u || true
  else
    echo "(pacman unavailable at pack time — package list not recorded)"
  fi
} > "$kit/LICENSES/SOURCES.txt"
for l in "$root/share/licenses/gcc-libs/RUNTIME.LIBRARY.EXCEPTION" \
         "$root/share/licenses/gcc-libs/COPYING3" \
         "$root/share/licenses/mingw-w64-headers/COPYING" ; do
  if [ -f "$l" ]; then
    cp "$l" "$kit/LICENSES/$(basename "$(dirname "$l")")-$(basename "$l")"
  fi
done

# --- report and archive ----------------------------------------------------
raw=$(find "$kit" -type f -printf '%s\n' | awk '{s+=$1} END {print s}')
echo "== kit: $(find "$kit" -type f | wc -l) files, $raw bytes raw" >&2
find "$kit" -type f -printf '%s\t%P\n' | sort -rn | head -8 |
  awk -F'\t' '{printf "   %12d  %s\n", $1, $2}' >&2

step "archiving"
archive="culebra-toolchain-windows-x64.zip"
(cd "$out" && rm -f "$archive" && zip -qr "$archive" "$(basename "$kit")")
printf '%s  %s\n' "$(sha256sum "$out/$archive" | cut -d' ' -f1)" "$archive" \
  > "$out/$archive.sha256"
(cd "$out" && sha256sum -c "$archive.sha256" >/dev/null) ||
  { echo "pack_windows_toolchain: the .sha256 we wrote does not verify" >&2; exit 1; }

echo "== zipped $(stat -c %s "$out/$archive") bytes" >&2
echo "$out/$archive"
echo "$out/$archive.sha256"
