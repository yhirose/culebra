#!/usr/bin/env bash
# JIT host symbol gate: the driver defines every helper codegen names.
#
# The in-process JIT emits references to `culebra_runtime_*` by name and
# resolves them with ORC's DynamicLibrarySearchGenerator, which reads the
# running process (dlsym; GetProcAddress over this image on Windows, where the
# export table is what answers — see below). There is no archive in that path
# — the definitions have to be in the driver image itself, which is why they carry
# __attribute__((used)) there (rt_macros.h): the driver is dead-stripped
# (-Wl,-dead_strip / --gc-sections) and nothing in the driver's own code calls
# most of them.
#
# What that guards against is a definition surviving compilation and then being
# stripped from the link. It happened: CULEBRA_RT_FEATURE_ARCHIVE's predecessor
# was a source-file property, so the driver's copies of the binding TUs lost
# `used` too, the linker coalesced one of those copies as the surviving weak
# definition, and dead_strip took it. macOS lost `culebra_runtime_object_new`
# and every JIT test died with `Symbols not found`. Linux did not, because its
# export whitelist (cmake/exported_symbols.txt) is an independent GC root —
# which is exactly why this ratchet is worth its seconds: the platform that
# breaks is the one local dev never builds.
#
# Names come from the headers rather than a list kept here: codegen's only
# spelling of a helper is the string literal it passes to the module, so
# grepping those literals is the same set by construction.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${1:-build/culebra}"
[[ -x "$BIN" ]] || { echo "jit-host-symbols: no $BIN" >&2; exit 1; }

names=$(grep -rho '"culebra_runtime_[a-z0-9_]*"' include/ | tr -d '"' | sort -u)

# What "defined in the driver" means is the resolver's question, and the two
# resolvers ask different ones.
#
# PE: ORC resolves through GetProcAddress over this image, which reads the
# EXPORT TABLE — so a helper compiled in but not exported is exactly as missing
# as one that was stripped, and the symbol table would say it is fine. The
# table is generated (cmake/gen_pe_exports.cmake reads the driver's objects
# with nm and writes a .def) precisely because neither PE linker takes the
# `culebra_*` pattern the ELF list states, and this is what holds that
# generator to the same set the ELF and Mach-O lanes are held to.
#
# ELF / Mach-O: dlsym over the process, so the defined symbols are the answer.
# `nm -g` (external only) is the one spelling GNU, LLVM and Apple nm agree on;
# undefined entries are class U. Mach-O prefixes a leading underscore that ELF
# does not, so normalize it off — safe here because every name matched above is
# C-linkage, never a `_Z` mangling. NM overrides the tool, which is how a
# Mach-O driver gets read from a Linux box (GNU nm cannot; llvm-nm can).
if [[ "$BIN" == *.exe ]]; then
  READOBJ="${READOBJ:-llvm-readobj}"
  if ! raw=$("$READOBJ" --coff-exports "$BIN" 2>/dev/null); then
    echo "jit-host-symbols: $READOBJ cannot read $BIN" >&2
    exit 1
  fi
  defs=$(printf '%s\n' "$raw" \
    | sed -n 's/^ *Name: \([A-Za-z_][A-Za-z0-9_]*\)$/\1/p' | sort -u)
  what="exports"
else
  NM="${NM:-nm}"
  if ! raw=$("$NM" -g "$BIN" 2>/dev/null); then
    echo "jit-host-symbols: $NM cannot read $BIN" >&2
    exit 1
  fi
  defs=$(printf '%s\n' "$raw" \
    | awk '$2 != "U" && $2 != "u" {print $NF}' | sed 's/^_//' | sort -u)
  what="defines"
fi

# An interpreter-only build (`just build-no-jit`) has no codegen and defines
# none of them; that is not this check's business. Fed by here-string, not by a
# pipe: `grep -q` stops at the first match, and under `pipefail` the SIGPIPE it
# hands the writer becomes the pipeline's status — which read as "no helpers"
# and skipped this check on a perfectly good JIT build.
if ! grep -q '^culebra_runtime_' <<<"$defs"; then
  echo "jit-host-symbols SKIP ($BIN $what no runtime helper — no-JIT build)"
  exit 0
fi

missing=$(comm -23 <(printf '%s\n' "$names") <(printf '%s\n' "$defs"))
if [[ -n "$missing" ]]; then
  n=$(printf '%s\n' "$missing" | grep -c .)
  echo "jit-host-symbols FAIL: $(basename "$BIN") is missing $n of" \
       "$(printf '%s\n' "$names" | grep -c .) helpers codegen resolves by name:" >&2
  # Indent and cap in one process: `head` would SIGPIPE its writer, which under
  # `pipefail` replaces this failure's exit status with 141.
  awk 'NR <= 20 {print "  " $0}' <<<"$missing" >&2
  cat >&2 <<'EOF'
  Every JIT program that reaches one of these dies at compile time with
  "JIT session error: Symbols not found". The definition is inline in a header,
  so this is a link-time strip, not a missing body: check that the TU defining
  it still gets __attribute__((used)) — CULEBRA_RT_FEATURE_ARCHIVE and
  CULEBRA_RT_DEFINE_RUNTIME both drop it (rt_macros.h), and neither belongs on
  anything compiled into the driver.
EOF
  if [[ "$what" == exports ]]; then
    cat >&2 <<'EOF'
  Read off the PE export table, so the definition may well be in the binary
  and simply not exported: check cmake/gen_pe_exports.cmake and the .def it
  wrote beside the executable.
EOF
  fi
  exit 1
fi

echo "jit-host-symbols OK ($(printf '%s\n' "$names" | grep -c .) helpers" \
     "codegen names, all in $(basename "$BIN")'s $what)"
