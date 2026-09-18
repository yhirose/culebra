#!/usr/bin/env bash
# Core runtime archive: nothing generated code can call reaches a gated backend.
#
# `libculebra_rt.a` is linked into every `culebra build` output; the feature
# archives beside it are force-loaded only when the AST scan says the program
# uses the feature, and the link line gets `-framework Metal` (or -lsqlite3,
# ...) on the same condition. So a backend reference the core archive can
# REACH is an undefined symbol nothing supplies -- the AOT link fails outright,
# for a program that never touched the feature. It is not a runtime bug (a
# no-Tensor binary holds no Tensor value to call the path with), which is
# exactly why nothing else catches it.
#
# The check is a link: force every `culebra_runtime_*` helper live -- the
# entire surface generated code can name, whatever a program happens to call
# -- dead-strip, and see what is left undefined. Reachability is the linker's
# question, so ask the linker rather than reading the archive's symbol table:
# an emitted-but-unreachable body (`tl::metal::context::context()` is always
# one) sits in the archive's undefined list either way, and stripping tells
# them apart.
#
# The holes this was written for were reached from a method NAME, never from
# `Tensor`: lowering.h maps `clone` / `backward` / `detach` to the tensor
# runtime helpers, so any class with a method so named emitted a Tensor arm,
# and `culebra build` on it failed on macOS with `Undefined symbols:
# _MTLCreateSystemDefaultDevice`. Both were kernels tl runs eagerly rather
# than through its runtime hooks: array::clone()'s device copy (a hook of its
# own now -- tl calls clone from inside reshape and logsumexp, so no choke on
# culebra's side of the boundary could have covered it) and array::xent_bwd,
# which only culebra's VJP calls, and which culebra::tensor_backward is now
# choked for.
#
# Cheap -- one link, no compile of culebra's own sources -- so it runs in
# every lane instead of the AOT one, which the macOS CI runner skips entirely
# and macOS is where a missing framework is what breaks.
#
# Usage: tools/checks/check_rt_archive_backend_free.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${1:-build}"
core="$BUILD_DIR/libculebra_rt.a"

if [[ ! -f "$core" ]]; then
  echo "rt-archive-backend-free: no $core (build the runtime archives first)" >&2
  exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-rtfree.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Every runtime helper by name: these are what an AOT object calls, and the
# only way into the archive from generated code.
helpers=$(nm -g --defined-only "$core" 2>/dev/null \
  | awk '$2 == "T" || $2 == "W" { print $3 }' \
  | sed 's/^_//' \
  | { grep '^culebra_runtime_' || true; } | sort -u)
count=$(printf '%s\n' "$helpers" | grep -c . || true)
if [[ "$count" -eq 0 ]]; then
  echo "rt-archive-backend-free: no culebra_runtime_* in $core" >&2
  exit 1
fi

# Mach-O prefixes C symbols with an underscore; ELF does not.
uflags=()
if [[ "$(uname -s)" == "Darwin" ]]; then
  strip_flag=-Wl,-dead_strip
  for h in $helpers; do uflags+=("-Wl,-u,_$h"); done
else
  strip_flag=-Wl,--gc-sections
  for h in $helpers; do uflags+=("-Wl,-u,$h"); done
fi

# The C++ driver, so the link picks up this platform's standard library
# (libc++ on macOS, libstdc++ on Linux) the way an AOT link does.
cxx="${CXX:-c++}"
printf 'int main(void) { return 0; }\n' > "$work/main.cc"
"$cxx" -c "$work/main.cc" -o "$work/main.o"

# The link is expected to fail: an AOT object supplies the namespace-group
# table (culebra_aot_ns_groups) that this bare archive still names, among
# others. Its report of what is undefined AFTER stripping is the answer, so
# read that rather than the exit status -- one entry per gated backend, by a
# symbol only that library defines and no culebra name resembles.
set +e
out=$("$cxx" -o "$work/probe" "$work/main.o" "$core" "${uflags[@]}" \
      "$strip_flag" -lpthread 2>&1)
set -e

backends='MTLCreateSystemDefaultDevice|cblas_|sqlite3_[a-z]|SSL_CTX_new|deflateInit|inflateInit'
hits=$(printf '%s\n' "$out" | { grep -E "$backends" || true; })

if [[ -z "$hits" ]]; then
  echo "rt-archive-backend-free OK ($count runtime helpers, no backend reachable)"
  exit 0
fi

echo "rt-archive-backend-free FAIL: the core archive reaches a backend it" >&2
echo "  cannot link against. Each symbol below belongs to a feature archive's" >&2
echo "  external library, which is on the link line only when the program uses" >&2
echo "  the feature -- so this breaks \`culebra build\` for programs that do not." >&2
echo "  Choke whatever reaches it into that feature's archive (include/stdlib/" >&2
echo "  tensor.h has the pattern), then re-run. To see the path, add" >&2
echo "  -Wl,-why_live,<mangled symbol> to the link this script makes." >&2
printf '%s\n' "$hits" | sed 's/^/  /' >&2
exit 1
