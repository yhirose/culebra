#!/usr/bin/env bash
# Runtime-archive TLS ownership gate.
#
# An AOT link is the core archive (libculebra_rt.a) plus every feature archive
# the program's namespaces select, force-loaded with --whole-archive. A
# namespace-scope `thread_local` with dynamic initialization compiles to a
# per-TU `__tls_init`, and the compiler emits a global "TLS init function for X"
# for it in every TU that includes the declaration — whether or not that TU
# touches the variable. ELF marks those weak and folds the duplicates; PE/COFF
# has no weak external, so mingw's ld reports
#
#   multiple definition of `TLS init function for culebra::sqlite::detail::g_dbs'
#
# and the Windows AOT link fails. The rule is therefore ownership: each such
# variable has exactly one defining archive.
#
# The two ways to satisfy it, both in use:
#   - the core archive has no business holding the state (its stubs never touch
#     it): gate the declaration out of the weak build     -> sqlite.h
#   - both halves need it: the core owns the definition and the feature archive
#     borrows it via `extern thread_local`                -> http.h
#
# Only namespace-scope variables are at stake. A function-local
# `static thread_local` initializes behind a guard inside its own function and
# emits no init symbol, so the mangling filters below keep it out (`_ZGVZ` /
# no `_ZTH`) rather than carrying an allowlist of false positives.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
core="$BUILD_DIR/libculebra_rt.a"

if [[ ! -f "$core" ]]; then
  echo "rt-archive-tls: no $core (build the runtime archives first)" >&2
  exit 1
fi

# Mangled names of the namespace-scope, dynamically-initialized thread_locals a
# given archive *defines*. An undefined reference is the borrowing side of the
# `extern thread_local` split and must not count.
#
# ELF names the colliding symbol directly (_ZTH<var>, absent for function-local
# statics). Mach-O has no init symbol, so stand in with the thread-local guard
# variable (_ZGV<var>) that dynamic initialization needs, minus the `Z`-nested
# (function-local) manglings the PE hazard does not cover.
tls_defs() {
  local ar="$1"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    nm -m "$ar" 2>/dev/null \
      | { grep -F '__DATA,__thread_vars' || true; } \
      | { grep -v 'undefined' || true; } \
      | awk '{print $NF}' \
      | { grep '^__ZGVN' || true; } \
      | sed 's/^__ZGVN/_ZN/' | sort -u
  else
    nm --defined-only "$ar" 2>/dev/null \
      | awk '{print $NF}' \
      | { grep '^_ZTH' || true; } \
      | sed 's/^_ZTH/_Z/' | sort -u
  fi
}

# Every symbol an archive DEFINES, and the subset it defines STRONGLY. A
# feature archive overriding one of the core's weak stubs is the force-load
# design itself (Http, Canvas, Tensor), so only the core's strong definitions
# are off limits to it.
#
# Read on ELF only. `nm`'s one-letter class answers weak-or-not outright, while
# Mach-O spells it several ways (`weak external`, `weak private external`, ...)
# and a filter that misses one reports every inline C++ body in the tree — this
# check did exactly that on the macOS lane. The hazard being proxied belongs to
# PE, so one platform that answers unambiguously is enough.
all_defs() {
  nm --defined-only "$1" 2>/dev/null | awk '{print $NF}' | sort -u
}
strong_defs() {
  nm --defined-only "$1" 2>/dev/null \
    | awk '$2 ~ /^[TDBR]$/ {print $NF}' | sort -u
}

demangle() {
  if command -v c++filt >/dev/null 2>&1; then c++filt; else cat; fi
}

core_defs=$(tls_defs "$core")
fail=0
checked=0

for ar in "$BUILD_DIR"/libculebra_rt_*.a; do
  [[ -f "$ar" ]] || continue
  checked=$((checked + 1))
  shared=$(comm -12 <(printf '%s\n' "$core_defs") <(tls_defs "$ar"))
  [[ -n "$shared" ]] || continue
  fail=1
  echo "rt-archive-tls FAIL: $(basename "$ar") re-defines thread_local state" \
       "the core archive already defines:" >&2
  printf '%s\n' "$shared" | demangle | sed 's/^/  /' >&2
done

if (( fail )); then
  cat >&2 <<'EOF'
  Windows AOT links that force-load this archive fail with "multiple definition
  of `TLS init function for ...'". Give each variable one owner: gate it out of
  the weak build when the core's stubs never touch it (sqlite.h), or declare it
  `extern thread_local` in the feature build and let the core define it
  (http.h).
EOF
  exit 1
fi

echo "rt-archive-tls OK ($checked feature archives," \
     "$(printf '%s\n' "$core_defs" | grep -c . || true) core-owned thread_locals)"

# The core archive must never declare an httplib type, let alone reference the
# TLS/compression libraries behind it: http.h gates the httplib.h include on
# CULEBRA_RT_HTTP_REQUEST_WEAK, and the binding layer (
# stdlib_jit.h) names only the neutral ServerRequest/ServerResponse on the
# server side. A program that never uses Http should therefore link no such
# symbol at all. This is easy to lose silently: ELF/Mach-O dead-strip an
# unreferenced undefined symbol, so only Windows notices when one comes back
# (PE's ld reports it before --gc-sections runs) -- catch it here instead of
# in a Windows CI round trip.
#
# The pattern covers the library prefixes, not just the 39 symbols the leak
# happened to produce, so a vendored cpp-httplib bump that reaches a different
# corner of OpenSSL/zlib still trips it. `7httplib` is the Itanium mangling of
# `namespace httplib`, matched directly so the check does not depend on c++filt
# being installed (demangle() falls back to `cat`, which would silently pass).
core_undef=$(nm -u "$core" 2>/dev/null | awk '{print $NF}' | sort -u)
leak=$(printf '%s\n' "$core_undef" | grep -E \
  '^_?(SSL|X509|EVP_|BIO_|ERR_|ASN1_|RAND_|PEM_|OPENSSL|CRYPTO_|OCSP_|i2d_|d2i_|GENERAL_NAME|deflate|inflate|crc32|adler32|compress|uncompress|zlib|gz)|7httplib' \
  || true)
if [[ -n "$leak" ]]; then
  echo "rt-archive-deps FAIL: the core archive references OpenSSL/zlib/httplib:" >&2
  printf '%s\n' "$leak" | demangle | sed 's/^/  /' >&2
  cat >&2 <<'EOF'
  A program that never uses Http should link no such symbol -- ELF/Mach-O
  dead-strip these, so only Windows AOT (src/main.cc's win_static) would
  notice, silently paying ~4 MB again for every program regardless of use.
  Check that no binding (stdlib_jit.h) or http.h server-side
  declaration names an httplib type directly; see http.h's
  ServerRequest/ServerResponse for the neutral shape to use instead.
EOF
  exit 1
fi
echo "rt-archive-deps OK (core archive references no OpenSSL/zlib/httplib symbol)"

# Same hazard, one symbol class over: the `culebra_runtime_*` ABI helpers. The
# core archive defines all of them outright, and a feature TU that reaches
# wrap.h emits its own copy of any the compiler declines to inline
# (rt_macros.h explains what stops the rest). Webview's link fragment takes
# the first definition and drops the diagnostic, so a leftover there is
# survivable; nothing absorbs anyone else's. The bound is what separates "a
# call stopped inlining" from "the force-emit attribute came back" — and the
# archives that would show the latter are exactly the ones that reach wrap.h,
# so no other check catches it. (Internal helpers are plain `inline` on both
# sides and fold — rt_macros.h.)
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "rt-archive-dup SKIP (symbol classes are read on ELF -- see strong_defs)"
else
  # The waiver is only as true as the flag, and it is Webview's fragment that
  # must carry it -- not some other axis. This script is where a fragment that
  # lost the flag goes unnoticed otherwise (it did, once: the flag was deleted
  # and only the comment came back).
  waved=""
  if grep -q -- '_webview_link.*--allow-multiple-definition' CMakeLists.txt; then
    waved="libculebra_rt_webview.a"
  fi
  waved_max=8   # a leftover or two; 348 was the archive before the attribute came off
  core_strong=$(strong_defs "$core")
  dup_fail=0
  for ar in "$BUILD_DIR"/libculebra_rt_*.a; do
    [[ -f "$ar" ]] || continue
    shared=$(comm -12 <(printf '%s\n' "$core_strong") <(all_defs "$ar"))
    [[ -n "$shared" ]] || continue
    n=$(printf '%s\n' "$shared" | grep -c .)
    if [[ -n "$waved" && "$ar" == */"$waved" ]] && (( n <= waved_max )); then
      echo "rt-archive-dup OK: $(basename "$ar") leaves $n un-inlined" \
           "helper(s) for its fragment's --allow-multiple-definition"
      continue
    fi
    dup_fail=1
    echo "rt-archive-dup FAIL: $(basename "$ar") re-defines $n symbol(s) the" \
         "core archive defines strongly:" >&2
    printf '%s\n' "$shared" | demangle | sed 's/^/  /' | head -20 >&2
  done
  if (( dup_fail )); then
    cat >&2 <<'EOF'
  PE has no weak external, so mingw's ld calls each of these a multiple
  definition and the Windows AOT link fails. A feature TU must not define what
  the core archive defines strongly: check that its build carries
  CULEBRA_RT_FEATURE_ARCHIVE (CMakeLists' feature loop). Many of them mean the
  __attribute__((used)) came back (rt_macros.h); one or two mean a call stopped
  inlining, which only an axis whose link fragment carries
  --allow-multiple-definition can absorb.

  If this names libculebra_rt_scene.a, it is not your change: Scene reaches
  wrap.h the same way and leaves a leftover or two, and its fragment is
  raylib's, shared with Canvas, so it has no flag to absorb them. Scene has
  never linked on Windows for this reason; the gate builds it OFF.

  (An axis you just switched OFF still has its archive in the build dir --
  delete that .a and re-run.)
EOF
    exit 1
  fi
  echo "rt-archive-dup OK (no unabsorbed duplicate of the core's" \
       "$(printf '%s\n' "$core_strong" | grep -c . || true) strong symbols)"
fi

# The same invariant one level down: the driver is several TUs in one PE image,
# so two of them defining the same thread_local is the identical link error --
# and until now nothing checked it. main.cc is the owner; every other TU must
# borrow (see CMakeLists' set_source_files_properties) or, better, not reach
# the headers at all. Objects, not archives, so this runs off `build-dev` too:
# seconds here instead of a Windows CI round trip.
driver_dir="$BUILD_DIR/CMakeFiles/culebra.dir"
owner="$driver_dir/src/main.cc.o"
if [[ -f "$owner" ]]; then
  owner_defs=$(tls_defs "$owner")
  objs=0
  while IFS= read -r obj; do
    [[ "$obj" == "$owner" ]] && continue
    objs=$((objs + 1))
    shared=$(comm -12 <(printf '%s\n' "$owner_defs") <(tls_defs "$obj"))
    [[ -n "$shared" ]] || continue
    fail=1
    echo "rt-driver-tls FAIL: ${obj#$driver_dir/} re-defines thread_local state" \
         "main.cc already defines:" >&2
    printf '%s\n' "$shared" | demangle | sed 's/^/  /' >&2
  done < <(find "$driver_dir" -name '*.o' -o -name '*.obj' | sort)
  if (( fail )); then
    cat >&2 <<'EOF'
  mingw's ld fails the driver link with "multiple definition of `TLS init
  function for ...'"; ELF and Mach-O fold it, so only Windows CI would notice.
  Keep the TU off the interpreter/JIT headers if it can be -- reaching
  culebra.h costs ~70 s of compile for them anyway. If it genuinely needs
  them, CULEBRA_RT_FEATURE_BUILD covers the CULEBRA_RT_CORE_OWNED variables
  (rt_shared_tls.h) and nothing else: the net/http/sqlite registries are plain
  `inline thread_local` and no build flag will move them. Do not reach for
  CULEBRA_RT_FEATURE_ARCHIVE here -- it drops the `used` that keeps the JIT's
  helpers in the driver image (tools/check_jit_host_symbols.sh).
EOF
    exit 1
  fi
  echo "rt-driver-tls OK ($objs driver TUs borrow main.cc's" \
       "$(printf '%s\n' "$owner_defs" | grep -c . || true) thread_locals)"
fi
