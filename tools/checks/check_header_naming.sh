#!/usr/bin/env bash
# Header-naming gate.
#
# The names in include/ carry three claims, and each one drifted before it was
# checked. This gate holds all three.
#
#   1. A name that was renamed away stays gone. The rename that made `jit`
#      mean LLVM landed while other branches were already written against the
#      old spelling, and the first rebase after it reintroduced three of them
#      in comments — textually clean, semantically stale, and invisible to
#      every other gate. Old spellings are a hard zero.
#
#   2. `jit` means LLVM. Only the files listed below may name llvm:: or
#      include an <llvm/...> header; the runtime layer both engines stand on
#      is rt.h and its fragments, and it must stay buildable with no LLVM at
#      all (docs/internals/vm.md §9). The list may shrink, never grow.
#
#   3. `.gen.h` means generated, in both directions: every .gen.h has a
#      generator, and every generator writes to a .gen.h. Before this gate
#      grammar_blob.h was generated without saying so and canon_sigs.gen.h
#      had been hand-maintained since its generator was deleted.
#
#   4. `.inc.h` is not a header. Those files are rt.h's body — one extern "C"
#      block runs across four of them — so only rt.h may include one.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
note() { echo "header-naming FAIL: $*" >&2; fail=1; }

# --- 1. names that were renamed away ---------------------------------------

RETIRED=(
  jit_value.h jit_owned.h jit_string.h jit_runtime.h jit_fixed.h
  jit_dispatch.h jit_iter.h jit_mem.h jit_gc.h jit_slab.h
  canon_sigs.gen.h stdlib_jit.h sendable_jit.h
)
# layout.md is excluded because naming these is its job: it is the document
# that says which spellings were retired and what each one had come to hide.
# This script is excluded for the same reason — the list above is the list.
for old in "${RETIRED[@]}"; do
  hits=$(grep -rIln --exclude-dir=vendor --exclude-dir=build --exclude-dir=build-dev \
         --exclude-dir=.git --exclude=check_header_naming.sh --exclude=layout.md --exclude=layout.ja.md -F "$old" . 2>/dev/null || true)
  if [[ -n $hits ]]; then
    note "\`$old\` was renamed away but is named again in:"
    printf '  %s\n' $hits >&2
  fi
done
# grammar_blob.h is retired too, but grammar_blob.gen.h contains it as no
# substring, so a plain -F search would be clean either way; match the bare
# form only where it is not followed by the new suffix.
hits=$(grep -rIln --exclude-dir=vendor --exclude-dir=build --exclude-dir=build-dev \
       --exclude-dir=.git --exclude=check_header_naming.sh --exclude=layout.md --exclude=layout.ja.md -E 'grammar_blob\.h([^a-z]|$)' . 2>/dev/null || true)
[[ -n $hits ]] && { note "\`grammar_blob.h\` was renamed to grammar_blob.gen.h; still named in:"; printf '  %s\n' $hits >&2; }

# --- 2. jit means LLVM ------------------------------------------------------

# Files allowed to name LLVM. Shrink this as the layering tightens; adding to
# it means a new part of the tree stopped building without LLVM.
LLVM_OK=(include/jit/jit.h include/jit/lowering.h include/stdlib/bindings.h)
mapfile -t llvm_users < <(
  grep -rIl --exclude-dir=vendor -E '(#include *<llvm/)|llvm::' include/ 2>/dev/null | sort)
for f in "${llvm_users[@]}"; do
  ok=0
  for a in "${LLVM_OK[@]}"; do [[ $f == "$a" ]] && ok=1 && break; done
  (( ok )) || note "$f names LLVM, but only ${LLVM_OK[*]} may — the rest of include/ has to build with no LLVM linked."
done
# And the reverse: an entry that no longer names LLVM should leave the list.
for a in "${LLVM_OK[@]}"; do
  [[ -f $a ]] || { note "$a is in the LLVM allow-list but does not exist"; continue; }
  grep -qIE '(#include *<llvm/)|llvm::' "$a" \
    || note "$a no longer names LLVM — drop it from this gate's allow-list."
done

# --- 3. .gen.h means generated, both ways -----------------------------------

# Every committed .gen.h is named by a recipe that regenerates it. The count
# matters as much as the loop: a glob that stops matching passes this check
# while measuring nothing, which is exactly how three other gates went quiet.
gens=0
while IFS= read -r g; do
  gens=$((gens + 1))
  base=$(basename "$g")
  grep -rIq -F "$base" justfile misc tools 2>/dev/null \
    || note "$g is named .gen.h but no recipe under justfile/misc/tools writes it."
done < <(git ls-files 'include/*.gen.h' 'include/**/*.gen.h')
(( gens > 0 )) || note "no .gen.h under include/ — the generated headers moved, and this check is measuring nothing."

# Every path a generator writes into include/ ends in .gen.h.
while IFS= read -r p; do
  [[ $p == *.gen.h ]] || note "a generator writes $p, which does not end in .gen.h."
done < <(grep -rIhoE 'include/[A-Za-z0-9_./]+\.h' misc/gen_*.sh tools/gen_*.cc 2>/dev/null \
         | sort -u | grep -v '\.gen\.h$' || true)

# --- 4. .inc.h is rt.h's body ----------------------------------------------

# Same shape: if the fragments were renamed out from under this pattern there
# would be nothing to find, and nothing to say so.
frags=$(find include -name '*.inc.h' | wc -l | tr -d ' ')
(( frags > 0 )) || note "no .inc.h under include/ — the rt.h fragments moved, and this check is measuring nothing."

while IFS= read -r line; do
  f=${line%%:*}
  [[ $f == include/rt/rt.h ]] && continue
  note "$f includes an .inc.h fragment; only rt.h may (they are its body, and one extern \"C\" block spans four of them)."
done < <(grep -rIn --exclude-dir=vendor --exclude-dir=build --exclude-dir=build-dev \
         -E '#include *[<"]rt/[a-z]+\.inc\.h[>"]' include/ src/ tests/ 2>/dev/null || true)

(( fail )) && exit 1
echo "header-naming OK (retired names gone; LLVM confined to ${#LLVM_OK[@]} files; .gen.h and .inc.h mean what they say)"
