#!/usr/bin/env bash
# AOT-build a program that runs a model through Llama, and check what it says.
#
# Usage: misc/aot_axes/probe_llama_aot.sh <culebra binary> [work dir]
#
# Needs CULEBRA_TEST_MODEL (a GGUF file; the 1.2 MB stories260K.gguf is
# enough) and a driver built with CULEBRA_ENABLE_LLAMA=ON; skips otherwise.
# The program generates under a grammar, so the text is exact whatever the
# model, which proves the binary runs the engine and not just links it. It also
# prints device().reason, with no Vulkan driver listed:
#
#   - the default link is the driver's own: the binary answers what the driver
#     does ("no Vulkan device" where it has the Vulkan backend, "built without
#     GPU support" where it has none), and never needs the Vulkan loader to
#     start;
#   - --llama-cpu leaves the GPU half out: "built without GPU support".
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
# A relative name the loader cannot find: no Vulkan driver, on every OS.
# Windows binaries end lines with CRLF.
run() { VK_DRIVER_FILES=nonexistent.json VK_ICD_FILENAMES=nonexistent.json "$@" | tr -d '\r'; }

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

check() {  # binary, expected reason, label
  local bin=$1 reason=$2 label=$3
  [ -f "$bin.exe" ] && bin="$bin.exe"
  local libs
  libs=$("$needed_libs" "$bin")
  if grep -qi vulkan <<<"$libs"; then
    fail "the $label binary links the Vulkan loader: [$libs]"
  fi
  local out want
  out=$(run "./$bin")
  want=$(printf 'ok\n%s' "$reason")
  [ "$out" = "$want" ] || fail "AOT $label run printed [$out], expected [$want]"
  echo "OK: the $label binary generated with the engine ($reason)"
}

driver_reason=$(run "$exe" --vm llama_aot.cul | sed -n 2p)
case "$driver_reason" in
  "no Vulkan device" | "built without GPU support") ;;
  *) fail "the driver itself answered [$driver_reason]" ;;
esac
"$exe" build llama_aot.cul -o llama_aot
check llama_aot "$driver_reason" "default"
"$exe" build --llama-cpu llama_aot.cul -o llama_aot_cpu
check llama_aot_cpu "built without GPU support" "--llama-cpu"
