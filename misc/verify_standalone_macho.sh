#!/usr/bin/env bash
# Prove a macOS build is standalone: every library it loads at launch ships
# with macOS itself.
#
# Usage: misc/verify_standalone_macho.sh <culebra>
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
# Skips (exit 0) off macOS; the Windows half is
# misc/windows_toolchain/verify_standalone_exe.sh.
set -eu

bin=${1:?usage: verify_standalone_macho.sh <binary>}

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
