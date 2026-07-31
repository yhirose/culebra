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
  them, CULEBRA_RT_FEATURE_BUILD covers only the CULEBRA_RT_CORE_OWNED
  variables (rt_shared_tls.h); the net/http/sqlite registries are plain
  `inline thread_local` and no build flag will move them.
EOF
    exit 1
  fi
  echo "rt-driver-tls OK ($objs driver TUs borrow main.cc's" \
       "$(printf '%s\n' "$owner_defs" | grep -c . || true) thread_locals)"
fi
