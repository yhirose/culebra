#!/usr/bin/env bash
# Doc C++ example gate.
#
# The embedding chapter is the only place a host author is told which
# headers to include, and nothing ever compiled those blocks: `just
# doctest` runs ```culebra fences and skips ```cpp entirely. So a header
# rename, a moved file or a changed signature broke the embedding
# examples silently, and the first person to find out was whoever pasted
# one. Two checks, cheap first:
#
#   A. Every `#include <x>` inside a ```cpp fence resolves — to a header
#      under include/, or to one the compiler finds on its own. This is
#      the half that catches a rename, and it runs in under a second, so
#      it is also available as --fast for the always-on lane.
#
#   B. Every fence that is a complete program (it has `int main(`)
#      compiles with the flags deployment.md documents. ~25s per block,
#      so the blocks build in parallel and this half lives in `doctest`
#      rather than `test-dev`.
#
# The include list below is duplicated in deployment.md's "Building your
# host program" on purpose: the gate exists to prove that what the doc
# tells a reader to type is what actually builds. Change one, change the
# other — B is what fails when they drift.
#
# Usage: check_docs_cpp.sh [--fast]     (--fast runs A only)
set -euo pipefail
cd "$(dirname "$0")/../.."

fast=0
[[ "${1:-}" == "--fast" ]] && fast=1

SOURCES=(docs/*.md README.md README.ja.md)

# The include path a host build needs, exactly as documented. Every entry
# is load-bearing: without vendor/stb the build stops in font_ttf.h,
# without vendor/cpp-regexlib in regex.h, without vendor/cpp-fstlib in
# fst.h, without vendor/cpp-searchlib/include in search.h — the stdlib
# bindings reach all four unconditionally.
INC=(-I include
     -I vendor/cpp-peglib
     -I vendor/cpp-vmlib
     -I vendor/cpp-unicodelib
     -I vendor/cpp-tensorlib/include
     -I vendor/stb
     -I vendor/cpp-regexlib
     -I vendor/cpp-fstlib
     -I vendor/cpp-searchlib/include)

fail=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- extract every ```cpp fence -------------------------------------------

for f in "${SOURCES[@]}"; do
  [[ -f $f ]] || continue
  awk -v out="$TMP" -v tag="${f//\//_}" '
    /^```cpp$/ { n++; file = out "/" tag "-" n ".cpp"; inb = 1; next }
    /^```$/    { inb = 0 }
    inb        { print > file }
  ' "$f"
done
blocks=$(find "$TMP" -name '*.cpp' | wc -l | tr -d ' ')

if (( blocks == 0 )); then
  echo "docs-cpp FAIL: no \`\`\`cpp fences in ${SOURCES[*]} — the docs moved" >&2
  echo "  or the fence tag changed, and this gate is measuring nothing." >&2
  exit 1
fi

# --- pick a C++23 compiler ------------------------------------------------

CXXBIN="${CULEBRA_DOCS_CXX:-${CXX:-c++}}"
command -v "$CXXBIN" >/dev/null 2>&1 || CXXBIN=c++
if ! echo 'int main(){}' | "$CXXBIN" -std=c++23 -fsyntax-only -x c++ - 2>/dev/null; then
  echo "docs-cpp SKIP: $CXXBIN does not accept -std=c++23 (set CULEBRA_DOCS_CXX)"
  exit 0
fi

# --- A. every #include resolves -------------------------------------------

missing=0
while IFS= read -r line; do
  hdr=$(sed -n 's/.*#include *<\([^>]*\)>.*/\1/p' <<<"$line")
  [[ -n $hdr ]] || continue
  [[ -f "include/$hdr" ]] && continue
  # Not one of ours, so it has to be one the compiler finds by itself.
  printf '#include <%s>\n' "$hdr" \
    | "$CXXBIN" -std=c++23 -fsyntax-only -x c++ - 2>/dev/null && continue
  echo "docs-cpp FAIL: <$hdr> is neither under include/ nor findable —" >&2
  echo "  renamed, moved, or a typo. Named by a \`\`\`cpp block in the docs" >&2
  echo "  or by the embedding sample on the landing page." >&2
  missing=1
done < <({
  grep -ho '#include *<[^>]*>' "$TMP"/*.cpp 2>/dev/null
  # The landing page carries the same embedding sample, HTML-escaped, so no
  # pass that rewrites `#include <x>` ever touched it. It sat one release
  # behind the headers until this check reached it.
  # Tags first (they use real angle brackets), then the entities that spell
  # the include's own brackets — the other order eats `#include</span>`.
  sed -e 's/<[^>]*>//g' -e 's/&lt;/</g' -e 's/&gt;/>/g' site/*.html 2>/dev/null \
    | grep -ho '#include *<[^>]*>'
} | sort -u)
(( missing )) && fail=1
(( missing )) || echo "docs-cpp OK (includes): every <header> in $blocks block(s) and site/ resolves"

(( fast )) && exit $fail

# --- B. complete programs compile ----------------------------------------

# The LLVM lane needs LLVM's own headers. Find llvm-config the way an
# embedder would; with none present, say so rather than passing the JIT
# blocks silently.
#
# The last resort takes the highest version installed, not the first the glob
# names: an Ubuntu runner already carries LLVM 16, and culebra's JIT needs 20+,
# so the lexical order compiled the embedding examples against headers the
# project cannot build with at all — every push, from the day this gate landed.
# A machine with two LLVMs is the normal case, not an odd one.
#
# Ordering alone still lands on a wrong one where the newest installed is under
# the floor, and the compile error that follows reads as a broken example
# rather than a missing toolchain. So a candidate also has to clear
# CULEBRA_MIN_LLVM_VERSION, read out of CMakeLists.txt so the gate and the
# build cannot drift; below it, the JIT blocks skip and say why.
LLVM_FLOOR=$(sed -n 's/^set(CULEBRA_MIN_LLVM_VERSION \([0-9]\{1,\}\).*/\1/p' CMakeLists.txt)
: "${LLVM_FLOOR:=20}"

llvm_inc=""
llvm_via=""
accept_llvm() { # llvm-config path or name
  local c=$1 v
  [[ -n $c ]] || return 1
  command -v "$c" >/dev/null 2>&1 || [[ -x $c ]] || return 1
  v=$("$c" --version 2>/dev/null) || return 1
  [[ $v =~ ^([0-9]+) ]] || return 1
  (( BASH_REMATCH[1] >= LLVM_FLOOR )) || return 1
  llvm_inc=$("$c" --includedir 2>/dev/null) || return 1
  [[ -n $llvm_inc ]] || return 1
  llvm_via=$c
  return 0
}

if [[ -n ${LLVM_CONFIG:-} ]] && accept_llvm "$LLVM_CONFIG"; then :
elif accept_llvm llvm-config; then :
else
  while IFS= read -r c; do
    accept_llvm "$c" && break
  done < <(printf '%s\n' /usr/lib/llvm-*/bin/llvm-config | sort -Vr
           printf '%s\n' /opt/homebrew/opt/llvm/bin/llvm-config)
fi

progs=()
while IFS= read -r f; do progs+=("$f"); done \
  < <(grep -l 'int main(' "$TMP"/*.cpp 2>/dev/null || true)

if (( ${#progs[@]} == 0 )); then
  echo "docs-cpp FAIL: not one of the $blocks block(s) is a complete program" >&2
  echo "  — the embedding examples lost their \`int main(\`, or extraction broke." >&2
  exit 1
fi

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
export CXXBIN llvm_inc
export INCS="${INC[*]}"

printf '%s\n' "${progs[@]}" | xargs -P "$JOBS" -I{} bash -c '
  f="$1"
  read -ra inc <<< "$INCS"
  flags=(-std=c++23 -fsyntax-only "${inc[@]}")
  if grep -q "JIT::\|stdlib_jit\.h" "$f"; then
    if [[ -z $llvm_inc ]]; then echo "SKIP $f"; exit 0; fi
    flags+=(-DCULEBRA_JIT_ENABLED -I "$llvm_inc")
  fi
  if "$CXXBIN" "${flags[@]}" "$f" 2>"$f.err"; then echo "OK $f"; else echo "BAD $f"; fi
' _ {} > "$TMP/results" 2>/dev/null || true

built=$(grep -c '^OK '   "$TMP/results" || true)
skipped=$(grep -c '^SKIP ' "$TMP/results" || true)
bad=$(grep -c '^BAD '  "$TMP/results" || true)

if (( bad > 0 )); then
  while IFS= read -r f; do
    echo "docs-cpp FAIL: $(basename "$f") does not compile with the flags" >&2
    echo "  documented in deployment.md (Building your host program):" >&2
    # Lead with the errors: a header cascade opens with warnings out of LLVM's
    # own headers, and the first ten lines said nothing about the failure.
    { grep -m5 'error:' "$f.err" || sed -n '1,10p' "$f.err"; } >&2
  done < <(sed -n 's/^BAD //p' "$TMP/results")
  fail=1
fi

if (( fail == 0 )); then
  msg="docs-cpp OK (compile): $built complete example(s) build"
  [[ -n $llvm_via ]] && msg="$msg (LLVM via $llvm_via)"
  (( skipped > 0 )) && msg="$msg, $skipped skipped: no llvm-config >= $LLVM_FLOOR"
  echo "$msg"
fi

exit $fail
