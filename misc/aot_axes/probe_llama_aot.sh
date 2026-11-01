#!/usr/bin/env bash
# AOT-build a program that runs a model through Llama, and check what it says.
#
# Usage: misc/aot_axes/probe_llama_aot.sh <culebra binary> [work dir]
#
# Needs CULEBRA_TEST_MODEL (a GGUF file; the 1.2 MB stories260K.gguf is
# enough) and a driver built with CULEBRA_ENABLE_LLAMA=ON; skips otherwise.
# The program generates under a grammar, so the text is exact whatever the
# model, which proves the binary runs the engine and not just links it. It also
# prints device().reason: `culebra build` output carries no GPU code, so the
# answer is "built without GPU support" on every OS.
#
# Sibling of probe_scene_aot_link.sh; tools/checks/check_aot_feature_axes.sh
# checks which bodies the Llama link pulls in, where nm can tell.
set -eu

exe=${1:?usage: probe_llama_aot.sh <culebra binary> [work dir]}
[ "${exe#/}" = "$exe" ] && exe="$PWD/$exe"
if [ -z "${CULEBRA_TEST_MODEL:-}" ]; then
  echo "probe_llama_aot: SKIP (CULEBRA_TEST_MODEL is not set)"
  exit 0
fi
work=${2:-$(mktemp -d)}
cd "$work"

# (captured, then matched: a `| grep -q` would SIGPIPE the writer)
has=$(printf 'IO.print(Llama)\n' | "$exe" --vm - 2>&1 || true)
case "$has" in
  *"undefined variable 'Llama'"*)
    echo "probe_llama_aot: SKIP (this driver has no Llama)"
    exit 0 ;;
esac

cat > llama_aot.cul <<'EOF'
let m = Llama.load(Sys.env("CULEBRA_TEST_MODEL"), n_ctx: 256)
println(m.generate("x", max_tokens: 4, grammar: 'root ::= "ok"').text)
println(m.device().reason)
EOF

"$exe" build llama_aot.cul -o llama_aot
out=$(./llama_aot)
want=$(printf 'ok\nbuilt without GPU support')
if [ "$out" != "$want" ]; then
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}AOT Llama run printed [$out], expected [$want]" >&2
  exit 1
fi
echo "OK: the AOT binary generated with the engine, on the CPU"
