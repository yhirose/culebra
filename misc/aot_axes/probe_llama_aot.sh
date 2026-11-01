#!/usr/bin/env bash
# AOT-build a program that runs a model through Llama, and check what it says.
#
# Usage: misc/aot_axes/probe_llama_aot.sh <culebra binary> [work dir]
#
# Needs CULEBRA_TEST_MODEL (a GGUF file; the 1.2 MB stories260K.gguf is
# enough) and a driver built with CULEBRA_ENABLE_LLAMA=ON; skips otherwise.
# The program generates under a grammar, so the text is exact whatever the
# model, which proves the binary runs the engine and not just links it. It also
# prints device().reason:
#
#   - default `culebra build` output carries no GPU code: "built without GPU
#     support", on every OS;
#   - `--llama-gpu` (where the driver carries the Vulkan backend) links the
#     backend but not the Vulkan loader, and with no Vulkan driver listed says
#     exactly why the model is on the CPU: "no Vulkan device". The flag is also
#     refused for a program that does not use Llama.
#
# Sibling of probe_scene_aot_link.sh; tools/checks/check_aot_feature_axes.sh
# checks which bodies the Llama link pulls in, where nm can tell.
set -eu

exe=${1:?usage: probe_llama_aot.sh <culebra binary> [work dir]}
[ "${exe#/}" = "$exe" ] && exe="$PWD/$exe"
needed_libs="$(cd "$(dirname "$0")/../.." && pwd)/tools/checks/needed_libs.sh"
if [ -z "${CULEBRA_TEST_MODEL:-}" ]; then
  echo "probe_llama_aot: SKIP (CULEBRA_TEST_MODEL is not set)"
  exit 0
fi
work=${2:-$(mktemp -d)}
cd "$work"

fail() {
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}$*" >&2
  exit 1
}

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

# Windows binaries end lines with CRLF.
run() { "$@" | tr -d '\r'; }

"$exe" build llama_aot.cul -o llama_aot
out=$(run ./llama_aot)
want=$(printf 'ok\nbuilt without GPU support')
[ "$out" = "$want" ] || fail "AOT Llama run printed [$out], expected [$want]"
echo "OK: the AOT binary generated with the engine, on the CPU"

printf 'println("no Llama here")\n' > no_llama.cul
refused=$("$exe" build --llama-gpu no_llama.cul -o no_llama 2>&1 || true)
case "$refused" in
  *"built without the Vulkan backend"*)
    echo "probe_llama_aot: --llama-gpu SKIP (this driver has no Vulkan backend)"
    exit 0 ;;
  *"--llama-gpu: the program does not use Llama"*) ;;
  *) fail "--llama-gpu on a program without Llama said [$refused]" ;;
esac

"$exe" build --llama-gpu llama_aot.cul -o llama_aot_gpu
bin=llama_aot_gpu
[ -f "$bin.exe" ] && bin="$bin.exe"
libs=$("$needed_libs" "$bin")
if grep -qi vulkan <<<"$libs"; then
  fail "the --llama-gpu binary links the Vulkan loader: [$libs]"
fi
# A relative name the loader cannot find: no driver, on every OS.
out=$(VK_DRIVER_FILES=nonexistent.json VK_ICD_FILENAMES=nonexistent.json run "./$bin")
want=$(printf 'ok\nno Vulkan device')
[ "$out" = "$want" ] || fail "AOT --llama-gpu run printed [$out], expected [$want]"
echo "OK: the --llama-gpu binary carries the backend, opens the loader at run time"
