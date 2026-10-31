#!/usr/bin/env bash
# Baked-preamble gate (stdlib_preamble.h, CMakeLists' culebra_preamble_cc
# step). Losing the bake is silent: the lanes fall back to splicing the
# source, every program still runs, and `--jit` is just two seconds slower
# per stdlib module again. So read it off the lanes directly:
#
#   1. `--jit --emit-llvm` of a program naming a baked module calls the
#      module's entry and lowers no registration of its own (the IR calls
#      no culebra_runtime_lazy_ns_register), while CULEBRA_PREAMBLE_SOURCE=1
#      makes it splice again — which proves the switch is live, not that the
#      module was never needed;
#   2. `culebra build` of the same program links the module's baked entry
#      out of libculebra_rt.a, and only that module's;
#   3. both lanes agree with the executor, which always compiles the source.
#
# A built-in trait unit (__DirTrait) is held to the same three, with a class
# that defines Dir's five methods as the program that selects it.
#
# The `_?` in the nm patterns is Mach-O's leading underscore: without it all
# three symbol assertions matched nothing on macOS and the gate was inert
# there, which is where a link that drops a baked entry shows up first.
#
# Usage: tools/checks/check_baked_preamble.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${1:-build}"
bin="$BUILD_DIR/culebra"
[[ -x "$bin" ]] || { echo "check_baked_preamble: no $bin" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-baked.XXXXXX")
trap 'rm -rf "$work"' EXIT
fail=0

# Time only; Args would pull the biggest module and slow the gate for nothing.
cat > "$work/t.cul" <<'EOF'
IO.print(Time.seconds(90) == Time.minutes(1) + Time.seconds(30))
EOF
cat > "$work/plain.cul" <<'EOF'
IO.print("plain")
EOF
# The built-in trait unit __DirTrait, selected by a class that defines the
# five methods Dir requires in a program that never names the trait.
cat > "$work/own.cul" <<'EOF'
class Own {
  read(p) { 'x' }
  list_dir(p) { p == '' ? ['a', 'b'] : [] }
  exists(p) { true }
  is_file(p) { p != '' }
  is_dir(p) { p == '' }
}
IO.print(Own().files())
EOF

# 1. The JIT lane: baked -> a call to the entry, no registration lowered;
#    forced source -> one registration.
"$bin" --jit --emit-llvm "$work/t.cul" > "$work/baked.ll" 2> "$work/baked.err" || {
  echo "check_baked_preamble FAIL: --jit --emit-llvm failed:" >&2; cat "$work/baked.err" >&2; exit 1; }
CULEBRA_PREAMBLE_SOURCE=1 "$bin" --jit --emit-llvm "$work/t.cul" > "$work/source.ll" 2>/dev/null
if grep -q 'culebra_runtime_lazy_ns_register' "$work/baked.ll"; then
  echo "check_baked_preamble FAIL: --jit still lowers the Time preamble (is it baked? see CMakeLists' _baked_names)" >&2
  fail=1
fi
if ! grep -q 'call void @culebra_preamble_Time()' "$work/baked.ll"; then
  echo "check_baked_preamble FAIL: --jit does not call culebra_preamble_Time" >&2
  fail=1
fi
if ! grep -q 'culebra_runtime_lazy_ns_register' "$work/source.ll"; then
  echo "check_baked_preamble FAIL: CULEBRA_PREAMBLE_SOURCE=1 did not splice the source" >&2
  fail=1
fi

# 1b. The built-in traits, whose entry every program calls: baked -> the call
#     and none of the five default bodies (register_trait_default is what a
#     `neq` / `lt` / … default lowers to); forced source -> them back.
if grep -q 'culebra_runtime_register_trait_default' "$work/baked.ll"; then
  echo "check_baked_preamble FAIL: --jit still lowers the built-in trait defaults (is __Traits in _baked_names?)" >&2
  fail=1
fi
if ! grep -q 'call void @culebra_preamble___Traits()' "$work/baked.ll"; then
  echo "check_baked_preamble FAIL: --jit does not call culebra_preamble___Traits" >&2
  fail=1
fi
if ! grep -q 'culebra_runtime_register_trait_default' "$work/source.ll"; then
  echo "check_baked_preamble FAIL: CULEBRA_PREAMBLE_SOURCE=1 did not splice the trait defaults" >&2
  fail=1
fi

# 1c. A trait unit is an entry the splice selects: called where a class can
#     conform to the trait, lowered nowhere, and absent where none can.
"$bin" --jit --emit-llvm "$work/own.cul" > "$work/own.ll" 2> "$work/own.err" || {
  echo "check_baked_preamble FAIL: --jit --emit-llvm failed:" >&2; cat "$work/own.err" >&2; exit 1; }
if ! grep -q 'call void @culebra_preamble___DirTrait()' "$work/own.ll"; then
  echo "check_baked_preamble FAIL: --jit does not call culebra_preamble___DirTrait for a class with Dir's five methods" >&2
  fail=1
fi
if grep -q 'culebra_runtime_register_trait_default' "$work/own.ll"; then
  echo "check_baked_preamble FAIL: --jit lowers the Dir defaults (is __DirTrait in _baked_names?)" >&2
  fail=1
fi
if grep -q '@culebra_preamble___DirTrait()' "$work/baked.ll"; then
  echo "check_baked_preamble FAIL: --jit calls culebra_preamble___DirTrait for a program no Dir can reach" >&2
  fail=1
fi

# 3. Executor / JIT / JIT-from-source agree (prints the executor's output).
lanes() {
  local vm jit src
  vm=$("$bin" --vm "$work/$1.cul"); jit=$("$bin" --jit "$work/$1.cul")
  src=$(CULEBRA_PREAMBLE_SOURCE=1 "$bin" --jit "$work/$1.cul")
  if [[ "$vm" != "$jit" || "$vm" != "$src" ]]; then
    echo "check_baked_preamble FAIL: $1: lanes disagree: vm=[$vm] jit=[$jit] jit-source=[$src]" >&2
    return 1
  fi
  printf '%s' "$vm"
}
vm=$(lanes t) || fail=1
own_vm=$(lanes own) || fail=1
if [[ "$own_vm" != "['a', 'b']" ]]; then
  echo "check_baked_preamble FAIL: own: the executor printed [$own_vm], expected the Dir default's ['a', 'b']" >&2
  fail=1
fi

# 2. The AOT lane: the named module's entry is linked, an unnamed one is not.
if ! "$bin" build --keep-symbols "$work/t.cul" -o "$work/t" > "$work/t.buildlog" 2>&1; then
  echo "check_baked_preamble FAIL: culebra build failed:" >&2; cat "$work/t.buildlog" >&2; exit 1; fi
nm "$work/t" > "$work/t.nm"
if ! grep -qE ' T _?culebra_preamble_Time$' "$work/t.nm"; then
  echo "check_baked_preamble FAIL: the built binary lacks culebra_preamble_Time" >&2; fail=1; fi
if grep -qE ' T _?culebra_preamble_Args$' "$work/t.nm"; then
  echo "check_baked_preamble FAIL: the built binary carries culebra_preamble_Args it never names" >&2; fail=1; fi
aot=$("$work/t")
if [[ "$aot" != "$vm" ]]; then
  echo "check_baked_preamble FAIL: AOT printed [$aot], executor [$vm]" >&2; fail=1; fi
"$bin" build --keep-symbols "$work/plain.cul" -o "$work/plain" > /dev/null 2>&1
nm "$work/plain" > "$work/plain.nm"
# __Traits is the exception the grep has to make: every program registers the
# built-in traits, so its entry is linked even here. That it IS linked is the
# assertion — a miss means the traits went back to being lowered per program.
if ! grep -qE ' T _?culebra_preamble___Traits$' "$work/plain.nm"; then
  echo "check_baked_preamble FAIL: the built binary lacks culebra_preamble___Traits" >&2; fail=1; fi
if grep -E ' T _?culebra_preamble_' "$work/plain.nm" | grep -v '___Traits$' >/dev/null; then
  echo "check_baked_preamble FAIL: a program naming no stdlib module links a baked preamble" >&2; fail=1; fi
"$bin" build --keep-symbols "$work/own.cul" -o "$work/own" > /dev/null 2>&1
nm "$work/own" > "$work/own.nm"
if ! grep -qE ' T _?culebra_preamble___DirTrait$' "$work/own.nm"; then
  echo "check_baked_preamble FAIL: the built binary of a class with Dir's five methods lacks culebra_preamble___DirTrait" >&2; fail=1; fi
own_aot=$("$work/own")
if [[ "$own_aot" != "$own_vm" ]]; then
  echo "check_baked_preamble FAIL: own: AOT printed [$own_aot], executor [$own_vm]" >&2; fail=1; fi

if (( fail )); then
  cat >&2 <<'EOF'
  A registration in the baked IR, no call to the entry, or a missing
  culebra_preamble_<Name> in the built binary: the module is not in
  CMakeLists' _baked_names, the driver's table (baked_preambles.gen.cc) is
  stale, or resolve_baked_preamble did not see the spliced <stdlib> module
  first. An entry in a binary that never names the module: lower_program
  emitted a call it should not have.
EOF
  exit 1
fi
echo "baked-preamble OK (--jit calls the entries and lowers no baked module; build links only the named entries; lanes agree)"
