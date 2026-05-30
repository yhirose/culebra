#!/usr/bin/env bash
# arm64-Linux validation of the CULEBRA_NEW_GC build (run inside the container).
set -uo pipefail
cd /workspace
echo "=== arch: $(uname -m) / $(uname -s) ==="

echo "=== configure + build culebra (NEW_GC=ON) into build-linux ==="
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
  -DCULEBRA_ENABLE_JIT=ON -DCULEBRA_LTO=OFF -DCULEBRA_NEW_GC=ON >/tmp/cfg.log 2>&1 \
  || { echo "CONFIGURE FAILED"; tail -20 /tmp/cfg.log; exit 1; }
cmake --build build-linux -j"$(nproc)" >/tmp/build.log 2>&1 \
  || { echo "BUILD FAILED"; tail -30 /tmp/build.log; exit 1; }
echo "build OK"
BIN=./build-linux/culebra

echo "=== leak gates (JIT) ==="
for t in test_gc_leak_self_closure test_gc_leak_operator test_gc_leak_for_range test_gc_no_leak; do
  $BIN --jit "tests/$t.cul" >/dev/null 2>&1 && echo "  $t: green" || echo "  $t: RED($?)"
done

echo "=== full symmetry sweep (interp vs jit) ==="
pass=0; crash=0; bad=""
for f in tests/*.cul; do
  n=$(basename "$f" .cul); oi=$($BIN "$f" 2>/dev/null); ei=$?; oj=$($BIN --jit "$f" 2>/dev/null); ej=$?
  if [[ "$oi" == "$oj" && $ei -eq 0 && $ej -eq 0 ]]; then pass=$((pass+1))
  elif [[ ($ei -ne 0 && $ei -ne 255) || ($ej -ne 0 && $ej -ne 255) ]]; then crash=$((crash+1)); bad="$bad $n(i=$ei,j=$ej)!"
  else bad="$bad $n(i=$ei,j=$ej)"; fi
done
echo "  symmetry: pass=$pass crash=$crash  non-clean:$bad"

echo "=== JIT GC_STRESS (root completeness on glibc stack scan) ==="
ok=0; cr=0; cc=""
for f in tests/*.cul; do CULEBRA_GC_STRESS=1 timeout 120 $BIN --jit "$f" >/dev/null 2>&1; c=$?
  [[ $c -eq 0 || $c -eq 255 ]] && ok=$((ok+1)) || { cr=$((cr+1)); cc="$cc $(basename "$f" .cul)($c)"; }; done
echo "  JIT GC_STRESS: ok=$ok crash=$cr$cc"

echo "=== interp GC_STRESS ==="
ok=0; cr=0; cc=""
for f in tests/*.cul; do CULEBRA_GC_STRESS=1 timeout 120 $BIN "$f" >/dev/null 2>&1; c=$?
  [[ $c -eq 0 || $c -eq 255 ]] && ok=$((ok+1)) || { cr=$((cr+1)); cc="$cc $(basename "$f" .cul)($c)"; }; done
echo "  interp GC_STRESS: ok=$ok crash=$cr$cc"

echo "=== ctest (jit_gc units + mt_smoke multi-thread) ==="
(cd build-linux && ctest --output-on-failure 2>&1 | tail -12)
echo "=== DONE ==="
