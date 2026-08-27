#!/usr/bin/env bash
# AOT link-portability gate: nothing outside this driver's own cache may reach
# the link line.
#
# `culebra build` runs on the user's machine, not on the one that built the
# driver, so every file the link needs has to come out of the binary itself
# (materialize_archive) rather than off a path baked in at build time. The
# third-party statics an axis links whole — OpenSSL for Http, SDL3 + raylib for
# Canvas/Scene — used to be spliced in as absolute paths: a distro directory, a
# Homebrew prefix, the release runner's deps cache. Those exist nowhere but the
# build machine, so a downloaded binary could not AOT an Http or Canvas program
# at all, and no test saw it because every AOT test runs inside a build tree
# where the paths happen to exist.
#
# CMakeLists rejects a path in a link fragment at configure time; this is the
# other end of the same claim, read off a real link command: every absolute path
# on it is under $CULEBRA_CACHE (or is the scratch object), and everything else
# is a `-l` the host toolchain resolves.
#
# Usage: tools/check_aot_link_portability.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
bin="$BUILD_DIR/culebra"
[[ -x "$bin" ]] || bin="$bin.exe"
[[ -x "$bin" ]] || { echo "check_aot_link_portability: no $BUILD_DIR/culebra" >&2; exit 1; }
bin=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The driver is a native binary: on MSYS2 it reads none of this shell's mount
# table, so hand it a Windows path or the cache it prints is a different
# directory from the one named here.
cache="$work/cache"
command -v cygpath >/dev/null && cache="$(cygpath -m "$work")/cache"

# One program per axis that links a third-party static. Whether the axis is in
# THIS build is a question for the binary, not for the build directory: the CI
# lanes run against a downloaded artifact that holds the driver alone, with the
# archives where they belong — inside it. Running the program under the default
# engine is the same probe release.yml makes before shipping.
cat > "$work/http.cul" <<'CUL'
let srv = Http.server()
print("http")
CUL
cat > "$work/canvas.cul" <<'CUL'
Canvas.init(8, 8)
print("canvas {Canvas.width()}")
CUL

checked=0
for name in http canvas; do
  # --vm, not a bare run: every `just` recipe exports
  # CULEBRA_REQUIRE_EXPLICIT_ENGINE=1, under which an unnamed engine aborts by
  # design (src/main.cc). A bare probe therefore reads every axis as absent.
  # Print what a failing probe said rather than swallowing it — "absent" and
  # "broken" look the same from an exit code.
  if ! probe=$(cd "$work" && "$bin" --vm "$name.cul" 2>&1); then
    echo "  skip $name (this build has no such namespace)"
    printf '%s\n' "$probe" | head -3 | sed 's/^/      /'
    continue
  fi

  link=$(cd "$work" && CULEBRA_CACHE="$cache" CULEBRA_VERBOSE=1 \
    "$bin" build "$name.cul" -o "$name.out" 2>&1 >/dev/null |
    sed -n 's/^culebra build: link: //p')
  [[ -n "$link" ]] || {
    echo "check_aot_link_portability: no link command for $name" >&2
    exit 1
  }
  # Absolute paths only: quoted (the driver quotes what it materializes) or
  # bare. Separators are normalized both sides — a Windows path picks up its
  # backslashes at the join, after the forward slashes the cache was named with.
  for tok in $(tr " '\"" '\n\n\n' <<<"$link"); do
    tok=${tok//\\//}
    case "$tok" in
      /*|[A-Za-z]:/*) ;;
      *) continue ;;
    esac
    case "$tok" in
      "${cache//\\//}"/*) continue ;;  # materialized out of the binary: portable
      *.o) continue ;;                 # the scratch object, in $TMPDIR
    esac
    echo "check_aot_link_portability: $name links a build-machine path:" >&2
    echo "  $tok" >&2
    echo "  Embed the file (CMakeLists, _rt_embed_extern) and name it" >&2
    echo "  @<filename>@ in the link fragment instead." >&2
    exit 1
  done
  checked=$((checked + 1))
done

if [[ $checked -eq 0 ]]; then
  echo "check_aot_link_portability: no axis with an external static in this build" >&2
  exit 1
fi
echo "aot-link-portability OK ($checked axis link line(s), no path outside the cache)"
