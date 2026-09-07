#!/bin/sh
# Installs culebra from the latest GitHub release (or CULEBRA_VERSION, e.g.
# "v0.4.0") into /usr/local/bin (or CULEBRA_INSTALL_DIR).
#
#   curl -fsSL https://raw.githubusercontent.com/yhirose/culebra/master/install.sh | sh
#
# Only macOS (Apple Silicon) and Linux (x86-64) have a release asset today —
# see misc/package_release.sh and the table in README.md. Anything else exits
# with a pointer to the manual download / brew / winget instructions there.
#
# POSIX sh on purpose: this runs on whatever shell curl-pipes into, which is
# not always bash (e.g. Debian's /bin/sh is dash).
set -eu

repo=yhirose/culebra

os=$(uname -s)
arch=$(uname -m)
case "$os-$arch" in
  Darwin-arm64) target=macos-arm64 ;;
  Linux-x86_64) target=linux-x64 ;;
  *)
    echo "install.sh: no release build for $os/$arch" >&2
    echo "  macOS (Apple Silicon): brew install yhirose/culebra/culebra" >&2
    echo "  Windows: winget install yhirose.culebra" >&2
    echo "  Otherwise, download a binary from:" >&2
    echo "    https://github.com/$repo/releases/latest" >&2
    exit 1
    ;;
esac

version=${CULEBRA_VERSION:-latest}
if [ "$version" = latest ]; then
  base_url="https://github.com/$repo/releases/latest/download"
else
  base_url="https://github.com/$repo/releases/download/$version"
fi

install_dir=${CULEBRA_INSTALL_DIR:-/usr/local/bin}

# Checked here, before any network I/O, so a machine with neither write
# access nor sudo fails immediately instead of after downloading and
# verifying the archive. mkdir -p on an existing directory is a no-op, so
# once install_dir exists its own writability is what matters — its parent's
# only decides whether mkdir -p could create it in the first place.
if [ -d "$install_dir" ]; then writable=$install_dir; else writable=$(dirname "$install_dir"); fi
if [ -w "$writable" ]; then
  sudo=
elif command -v sudo >/dev/null 2>&1; then
  echo "Need sudo to write to $install_dir" >&2
  sudo=sudo
else
  echo "install.sh: cannot write to $install_dir and no sudo available" >&2
  echo "  set CULEBRA_INSTALL_DIR to a writable directory instead" >&2
  exit 1
fi

archive="culebra-$target.tar.gz"
workdir=$(mktemp -d "${TMPDIR:-/tmp}/culebra-install.XXXXXX")
trap 'rm -rf "$workdir"' EXIT

echo "Downloading $archive ($version)..." >&2
curl -fsSL --retry 3 --retry-connrefused "$base_url/$archive" -o "$workdir/$archive"
curl -fsSL --retry 3 --retry-connrefused "$base_url/SHA256SUMS" -o "$workdir/SHA256SUMS"

# SHA256SUMS covers every asset in the release; only check the line for the
# archive just downloaded so this doesn't fail on unrelated platforms' rows.
if command -v sha256sum >/dev/null 2>&1; then
  sum() { sha256sum "$@"; }
else
  sum() { shasum -a 256 "$@"; }
fi
grep " $archive\$" "$workdir/SHA256SUMS" >"$workdir/SHA256SUMS.archive" || true
if [ ! -s "$workdir/SHA256SUMS.archive" ]; then
  echo "install.sh: $archive not listed in SHA256SUMS, refusing to install unverified" >&2
  exit 1
fi
(cd "$workdir" && sum -c SHA256SUMS.archive) >&2

tar -xzf "$workdir/$archive" -C "$workdir"
binary=$(find "$workdir" -maxdepth 2 -type f -name culebra)
if [ -z "$binary" ]; then
  echo "install.sh: culebra binary not found inside $archive" >&2
  exit 1
fi
chmod +x "$binary"

$sudo mkdir -p "$install_dir"
$sudo mv "$binary" "$install_dir/culebra"

echo "Installed $("$install_dir/culebra" --version) to $install_dir/culebra" >&2
case ":$PATH:" in
  *":$install_dir:"*) ;;
  *) echo "Note: $install_dir is not on PATH." >&2 ;;
esac
