#!/usr/bin/env bash
# Prove a macOS build is standalone: every library it loads at launch ships
# with macOS itself.
#
# Usage: misc/verify_standalone_macho.sh [--check-minos] <culebra>
#   --check-minos  also: it starts on the oldest macOS a download supports
#
# dyld resolves every load command before main, so one that names a library the
# machine lacks stops the process there. v0.7.0 shipped that way: Homebrew's
# LLVM put its libz3 and libzstd on the link line, the binary recorded their
# install names under /opt/homebrew, and the download started only where
# Homebrew had those two at those versions. Every lane was green, because
# every lane ran on a machine that had just installed them.
#
# An allowlist of where a system library lives rather than a blocklist of the
# two names: the next one will not be called z3. An @rpath or @loader_path
# entry fails too, since the archive ships the binary alone.
#
# dyld also refuses a binary built for a newer macOS than the one it runs on
# (LC_BUILD_VERSION minos). Apple clang builds for the host's macOS unless told
# otherwise, so the release jobs run on the oldest one supported (macos-15 in
# release.yml and ci.yml's mirrors of them), and --check-minos holds them to
# it. It is opt-in because every build on a newer Mac, test-macos's among
# them, would fail it. tools/checks/check_runner_pins.sh keeps the runner
# labels on oldest_macos.
#
# Skips (exit 0) off macOS; the Windows half is
# misc/windows_toolchain/verify_standalone_exe.sh.
set -eu

oldest_macos=15.0  # README and the handbook's download tables say it too

check_minos=
if [ "${1:-}" = --check-minos ]; then check_minos=1; shift; fi
bin=${1:?usage: verify_standalone_macho.sh [--check-minos] <binary>}

if [ "$(uname -s)" != Darwin ]; then
  echo "verify_standalone_macho: SKIP (macOS-only)"
  exit 0
fi

# The first line of `otool -L` is the file's own name; each one after is
# "<tab><install name> (compatibility version ...)".
libs=$(otool -L "$bin" | sed -n '2,$s/^[[:space:]]*\(.*\) (compatibility.*$/\1/p')
[ -n "$libs" ] || { echo "verify_standalone_macho: no load commands read from $bin" >&2; exit 1; }
echo "Loaded libraries:"
echo "$libs"

bad=$(echo "$libs" | grep -vE '^(/usr/lib/|/System/Library/)' || true)
if [ -n "$bad" ]; then
  echo "ERROR: binary loads non-system librar(ies) — not standalone:" >&2
  echo "$bad" >&2
  echo "  (link its static archive, or take it off the link line: see what" >&2
  echo "   CMakeLists.txt does with LLVMSupport's zstd and z3)" >&2
  exit 1
fi
echo "OK: only system libraries loaded"

[ -n "$check_minos" ] || exit 0
minos=$(otool -l "$bin" | awk '$1 == "minos" { print $2; exit }')
[ -n "$minos" ] || { echo "verify_standalone_macho: no LC_BUILD_VERSION minos read from $bin" >&2; exit 1; }
if ! printf '%s\n' "$minos" "$oldest_macos" | sort -C -V; then
  echo "ERROR: binary needs macOS $minos, newer than the oldest supported ($oldest_macos)" >&2
  echo "  (build it on macOS $oldest_macos: the release jobs' runs-on)" >&2
  exit 1
fi
echo "OK: starts on macOS $minos and later (oldest supported: $oldest_macos)"
