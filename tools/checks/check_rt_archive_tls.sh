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
# Ownership is not enough, though: the variable must also be TOUCHED by that
# archive alone. A TU that sees only `extern thread_local T x;` still emits the
# TLS wrapper that decides whether x's initializer has run, and it names the
# init function by a weak reference. On COFF a weak definition (the owner's,
# a COMDAT whose default is the body) and a weak reference (the borrower's,
# whose default is absolute zero) are the same weak external, and lld keeps
# the first it sees. A wrap.h-reaching archive is force-loaded before the
# core, so the image resolved `TLS init function for _jit_str_visiting` to
# zero and the set was used before it was constructed (a divide by zero in
# the hashtable, Windows AOT only, `__Foreign` + Object display). ELF binds
# the same reference to the owner's definition, which is why no local lane
# saw it.
#
# The two ways to satisfy both rules, both in use:
#   - the state is one archive's alone: gate the declaration and every use
#     into that archive's build                             -> sqlite.h, http.h
#   - both halves need it: make it a Runtime substate, which is built by
#     whichever side touches it first and has no TLS initializer to lose
#                                       -> string.inc.h's _jit_str_visiting
#
# Only namespace-scope variables are at stake. A function-local
# `static thread_local` initializes behind a guard inside its own function and
# emits no init symbol, so the mangling filters below keep it out (`_ZGVZ` /
# no `_ZTH`) rather than carrying an allowlist of false positives.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${1:-build}"
core="$BUILD_DIR/libculebra_rt.a"

if [[ ! -f "$core" ]]; then
  echo "rt-archive-tls: no $core (build the runtime archives first)" >&2
  exit 1
fi

# Mangled names of the namespace-scope, dynamically-initialized thread_locals a
# given archive *defines*. An undefined reference is a borrow, and is the
# second check's subject, not this one's.
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
undef_syms() {
  nm -u "$1" 2>/dev/null | awk '{print $NF}' | sort -u
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
  the weak build when the core's stubs never touch it (sqlite.h, http.h), or
  make it a Runtime substate when both halves need it (string.inc.h's
  _jit_str_visiting).
EOF
  exit 1
fi

echo "rt-archive-tls OK ($checked feature archives," \
     "$(printf '%s\n' "$core_defs" | grep -c . || true) core thread_locals)"

# The borrowing side. A TU referencing a TLS init function it does not define
# has compiled a use of some other TU's dynamically-initialized thread_local
# through an `extern` declaration — the reference lld's COFF link resolves to
# zero (see the header). Read on every archive, the core included, and on ELF
# only (see all_defs): `nm -u` lists the reference whether the compiler made
# it weak (clang) or not (GCC).
tls_borrows() {
  undef_syms "$1" | { grep '^_ZTH' || true; }
}
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "rt-archive-borrow SKIP (references are read on ELF -- see all_defs)"
else
  borrow_fail=0
  for ar in "$core" "$BUILD_DIR"/libculebra_rt_*.a; do
    [[ -f "$ar" ]] || continue
    borrowed=$(tls_borrows "$ar")
    [[ -n "$borrowed" ]] || continue
    borrow_fail=1
    echo "rt-archive-borrow FAIL: $(basename "$ar") borrows thread_local state" \
         "another archive initializes:" >&2
    printf '%s\n' "$borrowed" | demangle | sed 's/^/  /' >&2
  done
  if (( borrow_fail )); then
    cat >&2 <<'EOF'
  On Windows that reference resolves to zero and the initializer never runs
  (see the header). Either gate the variable and every use of it into one
  archive's build (sqlite.h, http.h), or make it a Runtime substate
  (string.inc.h's _jit_str_visiting), built by the first touch on either side.
EOF
    exit 1
  fi
  echo "rt-archive-borrow OK (no archive references a TLS init function it" \
       "does not define)"
fi

# The core archive must never declare an httplib type, let alone reference the
# TLS/compression libraries behind it: http.h gates the httplib.h include on
# CULEBRA_RT_HTTP_REQUEST_WEAK, and the binding layer (
# stdlib_rt.h) names only the neutral ServerRequest/ServerResponse on the
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
core_undef=$(undef_syms "$core")
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
  Check that no binding (stdlib_rt.h) or http.h server-side
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
  # A feature TU must not define what the core archive defines strongly: PE
  # has no weak external, so mingw's ld calls each one a multiple definition
  # and the Windows AOT link fails. It used to be enough to hope the optimizer
  # inlined them all, with --allow-multiple-definition on the axes that reach
  # wrap.h to absorb what it did not — and an absorbed duplicate produced an
  # image that could fail to load. CULEBRA_RT_FEATURE_ARCHIVE now gives those
  # helpers GNU89 inline semantics (rt_macros.h), so a feature archive calls
  # the core's definition instead of emitting one. Nothing is waived: the
  # answer is zero.
  core_strong=$(strong_defs "$core")
  dup_fail=0
  for ar in "$BUILD_DIR"/libculebra_rt_*.a; do
    [[ -f "$ar" ]] || continue
    shared=$(comm -12 <(printf '%s\n' "$core_strong") <(all_defs "$ar"))
    [[ -n "$shared" ]] || continue
    dup_fail=1
    echo "rt-archive-dup FAIL: $(basename "$ar") re-defines $(printf '%s\n' "$shared" | grep -c .) symbol(s) the" \
         "core archive defines strongly:" >&2
    printf '%s\n' "$shared" | demangle | sed 's/^/  /' | head -20 >&2
  done
  if (( dup_fail )); then
    cat >&2 <<'EOF'
  PE has no weak external, so mingw's ld calls each of these a multiple
  definition and the Windows AOT link fails. Check that the archive's build
  carries CULEBRA_RT_FEATURE_ARCHIVE (CMakeLists' feature loop) — that is what
  makes a `culebra_runtime_*` call reach the core's definition instead of
  emitting a copy beside it. A symbol that is not one of those helpers has no
  such owner yet, and needs one.
EOF
    exit 1
  fi
  echo "rt-archive-dup OK (no feature archive re-defines any of the core's" \
       "$(printf '%s\n' "$core_strong" | grep -c . || true) strong symbols)"
fi

# The same invariant one level down: the driver is several TUs in one PE image,
# so two of them defining the same thread_local is the identical link error --
# and until now nothing checked it. main.cc is the owner; no other TU may reach
# the headers that define one. Objects, not archives, so this runs off
# `build-dev` too: seconds here instead of a Windows CI round trip.
driver_dir="$BUILD_DIR/CMakeFiles/culebra.dir"
owner="$driver_dir/src/main.cc.o"
if [[ -f "$owner" ]]; then
  owner_defs=$(tls_defs "$owner")
  objs=0
  while IFS= read -r obj; do
    [[ "$obj" == "$owner" ]] && continue
    objs=$((objs + 1))
    shared=$(comm -12 <(printf '%s\n' "$owner_defs") <(tls_defs "$obj"))
    if [[ -n "$shared" ]]; then
      fail=1
      echo "rt-driver-tls FAIL: ${obj#$driver_dir/} re-defines thread_local state" \
           "main.cc already defines:" >&2
      printf '%s\n' "$shared" | demangle | sed 's/^/  /' >&2
    fi
    # And the borrow, the same way as for the archives: a TU that declared
    # one of these `extern` resolves its initializer to zero under lld.
    [[ "$(uname -s)" == "Darwin" ]] && continue
    borrowed=$(tls_borrows "$obj")
    [[ -n "$borrowed" ]] || continue
    fail=1
    echo "rt-driver-tls FAIL: ${obj#$driver_dir/} borrows thread_local state" \
         "another TU initializes:" >&2
    printf '%s\n' "$borrowed" | demangle | sed 's/^/  /' >&2
  done < <(find "$driver_dir" -name '*.o' -o -name '*.obj' | sort)
  if (( fail )); then
    cat >&2 <<'EOF'
  Two TUs defining one thread_local is a duplicate TLS init function, and a
  TU borrowing one is an initializer lld resolves to zero (see the header);
  ELF folds the first and binds the second, so only Windows would notice.
  Keep the TU off the interpreter/JIT headers if it can be -- reaching
  culebra.h costs ~70 s of compile for them anyway. If it genuinely needs the
  variable, make it a Runtime substate: the net/http/sqlite registries are
  plain `inline thread_local`, and there is no build flag that moves one
  (borrowing through `extern thread_local` was tried, and is the Windows bug
  the header describes). Do not reach for CULEBRA_RT_FEATURE_ARCHIVE here --
  it drops the `used` that keeps the JIT's helpers in the driver image
  (tools/checks/check_jit_host_symbols.sh).
EOF
    exit 1
  fi
  echo "rt-driver-tls OK ($objs driver TUs neither define nor borrow main.cc's" \
       "$(printf '%s\n' "$owner_defs" | grep -c . || true) thread_locals)"
fi
