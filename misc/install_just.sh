#!/usr/bin/env bash
# Install `just` on Linux from its GitHub release, pinned to the published
# checksum for this machine's architecture.
#
#   misc/install_just.sh [dest dir]     # default /usr/local/bin
#
# The one place the pin lives (CI, setup_ubuntu.sh and the repro image run
# it). Not just.systems/install.sh: that host 403s shared runner IPs.
set -euo pipefail

dest=${1:-/usr/local/bin}
ver=1.58.0

if [ "$(uname -s)" != Linux ]; then
  echo "install_just.sh: Linux only (this is $(uname -s))" >&2
  exit 1
fi
case "$(uname -m)" in
  x86_64)
    arch=x86_64
    sha=4a5cc2f53e6f0f8c59092a6cc38291eb729d46a7dd95d3ae582008881b84931d ;;
  aarch64)
    arch=aarch64
    sha=748237128c4c40cbdabc65e841d05ceba13cc23a91eaba395495894c1d9764df ;;
  *)
    echo "install_just.sh: no pinned just for $(uname -m)" >&2
    exit 1 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl --proto '=https' --tlsv1.2 -sSfL --retry 3 --retry-delay 5 \
  -o "$tmp/just.tar.gz" \
  "https://github.com/casey/just/releases/download/$ver/just-$ver-$arch-unknown-linux-musl.tar.gz"
echo "$sha  $tmp/just.tar.gz" | sha256sum -c -
tar -xzf "$tmp/just.tar.gz" -C "$dest" just
"$dest/just" --version
