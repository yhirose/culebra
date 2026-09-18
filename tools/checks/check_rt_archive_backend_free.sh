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
# The check is a link: force live everything generated code can name -- the
# `culebra_runtime_*` helpers and the `culebra_ns_group_*` dispatch groups,
# whatever a program happens to use -- dead-strip, and see what is left
# undefined. Reachability is the linker's question, so ask the linker rather
# than reading the archive's symbol table:
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
# and macOS is where a missing framework is what breaks. It needs the built
# archives, so it is gate-only (`test-dev` has none).
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

# Generated code reaches the archive two ways, and both are roots here. The
# runtime helpers it calls by name, and -- for every stdlib namespace it uses
# -- a dispatch group, which an AOT object reaches only through its own
# culebra_aot_ns_groups table (bindings.h). Without the groups as roots every
# namespace body strips out and nothing under it is examined.
syms=$(nm -g --defined-only "$core" 2>/dev/null \
  | awk '{ print $NF }' \
  | sed 's/^_//' \
  | { grep -E '^culebra_(runtime|ns_group)_' || true; } | sort -u)
count=$(printf '%s\n' "$syms" | grep -c . || true)
if [[ "$count" -eq 0 ]]; then
  echo "rt-archive-backend-free: no culebra_runtime_* in $core" >&2
  exit 1
fi

# Mach-O prefixes C symbols with an underscore; ELF does not.
uflags=()
if [[ "$(uname -s)" == "Darwin" ]]; then
  strip_flag=-Wl,-dead_strip
  for s in $syms; do uflags+=("-Wl,-u,_$s"); done
else
  strip_flag=-Wl,--gc-sections
  for s in $syms; do uflags+=("-Wl,-u,$s"); done
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
rc=0
out=$("$cxx" -o "$work/probe" "$work/main.o" "$core" "${uflags[@]}" \
      "$strip_flag" -lpthread 2>&1) || rc=$?

# The control. This verdict is a grep over a linker's undefined-symbol report,
# so a link that never reaches one -- a library this toolchain cannot find, a
# flag it rejects -- leaves an output no backend name can match, and that reads
# as a pass. Nothing else here would notice.
if [[ "$rc" -ne 0 ]] &&
   ! printf '%s\n' "$out" |
     grep -qE 'Undefined symbols|undefined reference|undefined symbol'; then
  echo "rt-archive-backend-free: the probe link failed before it could report" >&2
  echo "  undefined symbols, so nothing was checked:" >&2
  printf '%s\n' "$out" | sed 's/^/  /' >&2
  exit 1
fi

# One entry per gated external dependency. cblas is only the matmul half of
# Accelerate: tl's macOS elementwise kernels are vDSP and vForce, and
# -framework Accelerate rides the Tensor axis like -framework Metal, so a
# reference to either breaks the same links.
backends='MTLCreateSystemDefaultDevice|cblas_|vDSP_|_vv[a-z]+f|sqlite3_[a-z]|SSL_CTX_new|deflateInit|inflateInit'
hits=$(printf '%s\n' "$out" | { grep -E "$backends" || true; })

if [[ -z "$hits" ]]; then
  echo "rt-archive-backend-free OK ($count roots, no backend reachable)"
  exit 0
fi

cat >&2 <<'EOF'
rt-archive-backend-free FAIL: the core archive reaches a backend it cannot
  link against. Each symbol below belongs to a feature archive's external
  library, which is on the link line only when the program uses the feature --
  so this breaks `culebra build` for programs that do not. Choke whatever
  reaches it into that feature's archive (include/stdlib/tensor.h has the
  pattern), then re-run. To see the path, add -Wl,-why_live,<mangled symbol>
  (ld64) or -Wl,-y,<mangled symbol> (GNU ld) to the link this script makes.
EOF
printf '%s\n' "$hits" | sed 's/^/  /' >&2
exit 1
