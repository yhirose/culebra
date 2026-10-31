#!/usr/bin/env bash
# Llama Vulkan dynamic-load gate (Linux and Windows).
#
# The Vulkan loader is opened when ggml first asks for it, not linked
# (src/runtime/llama_vulkan_dynload.cc). That is what lets a driver with the
# Vulkan backend start, and run Llama on the CPU, where no GPU driver stack is
# installed. Three ways to lose it silently, all checked on the linked output:
#
#   1. The loader comes back among the binary's needed libraries (something
#      linked Vulkan::Vulkan): libvulkan.so.1 in DT_NEEDED, vulkan-1.dll in the
#      PE import table. The binary keeps working here and stops starting
#      everywhere else.
#   2. The forwarders reach the exported symbols, where they would interpose on
#      the loader's own exports once it is opened.
#   3. The forwarded list drifts from what ggml-vulkan names after a llama.cpp
#      bump. A new name already fails the link; a name the archive dropped
#      leaves a dead forwarder that hides the list going stale. So the two sets
#      must be equal.
#
# Skips (exit 0) on macOS (no Vulkan backend) or a driver built without it.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${1:-build}"
src=src/runtime/llama_vulkan_dynload.cc

case "$(uname -s)" in
  Linux)
    bin="$BUILD_DIR/culebra"
    # stderr: an older readelf warns about DWARF forms it predates; -d reads none.
    needed() { readelf -d "$1" 2>/dev/null | grep NEEDED || true; }
    exported() { nm -D --defined-only "$1" | awk '{print $3}'; }
    ;;
  MINGW* | MSYS* | CYGWIN*)
    bin="$BUILD_DIR/culebra.exe"
    needed() { objdump -p "$1" | grep 'DLL Name' || true; }
    # The export table's name column; empty when the image exports nothing.
    exported() { objdump -p "$1" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | awk '{print $NF}'; }
    ;;
  *)
    echo "check_llama_vulkan_dynload: SKIP (no Vulkan backend on $(uname -s))"
    exit 0
    ;;
esac

[[ -x "$bin" ]] || { echo "check_llama_vulkan_dynload: no $bin" >&2; exit 1; }
archive=$(sed -n 's/^CULEBRA_LLAMA_VULKAN_ARCHIVE:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)
if [[ -z "$archive" ]]; then
  echo "check_llama_vulkan_dynload: SKIP (driver has no Llama Vulkan backend)"
  exit 0
fi
[[ -f "$archive" ]] || { echo "check_llama_vulkan_dynload: no $archive" >&2; exit 1; }

fail=0

libs=$(needed "$bin")
if grep -qi vulkan <<<"$libs"; then
  echo "ERROR: the driver links the Vulkan loader; it will not start without it" >&2
  echo "  (the loader belongs behind dlopen/LoadLibrary: $src)" >&2
  fail=1
else
  echo "OK: the Vulkan loader is not a needed library"
fi

forwarders_out=$(exported "$bin" | grep -E '^vk[A-Z]' || true)
if [[ -n "$forwarders_out" ]]; then
  echo "ERROR: the driver exports the Vulkan forwarders:" >&2
  echo "$forwarders_out" >&2
  fail=1
else
  echo "OK: no forwarder among the exported symbols"
fi

# What the archive asks of the loader, against what the TU forwards.
named=$(nm -u "$archive" | awk '{print $2}' | grep -E '^vk[A-Z]' | sort -u)
forwarded=$(grep -oE '^  X\(vk[A-Za-z0-9]+\)' "$src" | sed -E 's/.*X\((.*)\)/\1/' | sort -u)
if [[ -z "$named" || "$named" != "$forwarded" ]]; then
  echo "ERROR: the forwarders in $src do not match what libggml-vulkan.a names:" >&2
  diff <(echo "$forwarded") <(echo "$named") >&2 || true
  fail=1
else
  echo "OK: forwarders == the archive's loader calls ($(echo $named | tr ' ' ','))"
fi

[[ $fail -eq 0 ]] && echo "check_llama_vulkan_dynload: OK"
exit $fail
