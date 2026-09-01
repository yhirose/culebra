#!/usr/bin/env bash
# Prove a Windows build is standalone: it may import ONLY DLLs that ship with
# every Windows install.
#
# Usage: misc/windows_toolchain/verify_standalone_exe.sh <culebra.exe>
#
# Anything else — a mingw runtime DLL (libstdc++-6, libgcc_s, libwinpthread), a
# bundled zlib1.dll, or a static lib that quietly resolved to its import lib —
# would make a downloaded binary fail to start on a clean machine. Hence an
# allowlist rather than a blocklist of known-bad names.
#
# Single-sourced across the CI jobs and the release build, which otherwise each
# kept their own copy of the list: the release gate must not be able to drift
# weaker than the CI one by omission.
set -eu

exe=${1:?usage: verify_standalone_exe.sh <exe>}

# imm32 and setupapi are the two the Canvas window backend adds (SDL3 links a
# longer Win32 list than it ends up importing). Both ship with Windows, so a
# window build still starts on a machine with no display. Anything SDL starts
# referencing later should land here deliberately, not be pre-approved.
# ucrtbase and the api-ms-win-crt-* apisets are the UCRT64 toolchain's C
# runtime, where MINGW64 imported msvcrt; shlwapi comes with Webview.
allow='^(kernel32|kernelbase|msvcrt|ucrtbase|api-ms-win-crt-[a-z0-9-]+|ntdll'
allow="$allow"'|user32|advapi32|ws2_32|shell32|shlwapi|ole32|oleaut32|bcrypt'
allow="$allow"'|crypt32|secur32|iphlpapi|dbghelp|version|winmm|gdi32|imm32'
allow="$allow"'|setupapi)\.dll$'

echo "Imported DLLs:"
imports=$(objdump -p "$exe" \
  | grep -i 'DLL Name' | sed -E 's/.*DLL Name:\s*//' | tr 'A-Z' 'a-z' | sort -u)
echo "$imports"

bad=$(echo "$imports" | grep -viE "$allow" || true)
if [ -n "$bad" ]; then
  echo "ERROR: binary imports non-system DLL(s) — not standalone:" >&2
  echo "$bad" >&2
  exit 1
fi
echo "OK: only system DLLs imported — runs on a plain Windows box"
