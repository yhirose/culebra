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
# Usage: tools/check_baked_preamble.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/.."

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

# 3. Executor / JIT / JIT-from-source agree.
vm=$("$bin" --vm "$work/t.cul"); jit=$("$bin" --jit "$work/t.cul")
src=$(CULEBRA_PREAMBLE_SOURCE=1 "$bin" --jit "$work/t.cul")
if [[ "$vm" != "$jit" || "$vm" != "$src" ]]; then
  echo "check_baked_preamble FAIL: lanes disagree: vm=[$vm] jit=[$jit] jit-source=[$src]" >&2
  fail=1
fi

# 2. The AOT lane: the named module's entry is linked, an unnamed one is not.
if ! "$bin" build --keep-symbols "$work/t.cul" -o "$work/t" > "$work/t.buildlog" 2>&1; then
  echo "check_baked_preamble FAIL: culebra build failed:" >&2; cat "$work/t.buildlog" >&2; exit 1; fi
nm "$work/t" > "$work/t.nm"
if ! grep -qE ' T culebra_preamble_Time$' "$work/t.nm"; then
  echo "check_baked_preamble FAIL: the built binary lacks culebra_preamble_Time" >&2; fail=1; fi
if grep -qE ' T culebra_preamble_Args$' "$work/t.nm"; then
  echo "check_baked_preamble FAIL: the built binary carries culebra_preamble_Args it never names" >&2; fail=1; fi
aot=$("$work/t")
if [[ "$aot" != "$vm" ]]; then
  echo "check_baked_preamble FAIL: AOT printed [$aot], executor [$vm]" >&2; fail=1; fi
"$bin" build --keep-symbols "$work/plain.cul" -o "$work/plain" > /dev/null 2>&1
if nm "$work/plain" | grep -qE ' T culebra_preamble_'; then
  echo "check_baked_preamble FAIL: a program naming no stdlib module links a baked preamble" >&2; fail=1; fi

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
