#!/usr/bin/env bash
# Print the shared libraries a binary needs at load time, one per line:
# DT_NEEDED on ELF, the PE import table on Windows, the load commands on Mach-O.
#
# Usage: tools/checks/needed_libs.sh <binary>
#
# The one place the gates that ask "does this binary need library X to start"
# read it (check_llama_vulkan_dynload.sh, misc/aot_axes/probe_llama_aot.sh).
set -euo pipefail

bin=${1:?usage: needed_libs.sh <binary>}
case "$(uname -s)" in
  Linux)
    # stderr: an older readelf warns about DWARF forms it predates; -d reads none.
    readelf -d "$bin" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' ;;
  Darwin)
    otool -L "$bin" | tail -n +2 | awk '{print $1}' ;;
  *)
    objdump -p "$bin" | sed -n 's/.*DLL Name:[[:space:]]*//p' ;;
esac
