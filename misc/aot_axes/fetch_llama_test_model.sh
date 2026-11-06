#!/usr/bin/env bash
# Fetch the 1.2 MB model the Llama tests run (llama.cpp's own CI uses it) to
# $CULEBRA_TEST_MODEL, pinned by hash: a model file is code run on this machine.
set -euo pipefail

dest=${CULEBRA_TEST_MODEL:?set CULEBRA_TEST_MODEL to where the model goes}
url=https://huggingface.co/ggml-org/models/resolve/main/tinyllamas/stories260K.gguf
want=270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d

curl -fsSL --retry 5 --retry-delay 5 -o "$dest" "$url"
if command -v sha256sum >/dev/null; then
  got=$(sha256sum "$dest" | cut -d' ' -f1)
else
  got=$(shasum -a 256 "$dest" | cut -d' ' -f1)
fi
[ "$got" = "$want" ] || { echo "fetch_llama_test_model: $dest is $got, expected $want" >&2; exit 1; }
echo "fetched $dest"
