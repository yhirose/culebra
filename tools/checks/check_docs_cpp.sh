#!/usr/bin/env bash
# Doc C++ example gate.
#
# The embedding chapter is the only place a host author is told which
# headers to include, and nothing ever compiled those blocks: `just
# doctest` runs ```culebra fences and skips ```cpp entirely. So a header
# rename, a moved file or a changed signature broke the embedding
# examples silently, and the first person to find out was whoever pasted
# one. Two checks, cheap first:
#
#   A. Every `#include <x>` inside a ```cpp fence resolves — to a header
#      under include/, or to one the compiler finds on its own. Prose
#      counts too: a page that names `<base/shared.h>` in a sentence is
#      telling a host to include it, and a rename leaves it behind the
#      same way (the page sent hosts to `<stdlib_rt.h>` long after that
#      header was gone). Runs in under a second, so --fast has it.
#
#   B. Every fence that is a complete program (it has `int main(`)
#      compiles with the flags deployment.md documents. ~25s per block,
#      so the blocks build in parallel and this half lives in `doctest`
#      rather than `test-dev`.
#
#   C. The include list each deployment page prints is the list below,
#      entry for entry, and the link line names the same libraries. Text
#      only, so it runs with A.
#
#   D. Every entry of that list is load-bearing: dropping any one of them
#      stops the build. B only proves the list is enough, so an entry the
#      headers stopped needing (cpp-vmlib, once CodeGen left embed.h) sat
#      in the docs with nothing to say so.
#
#   E. A host links and runs with the link line the docs give: the core
#      build, then Http, SQLite (the amalgamation TU) and CodeGen the way
#      the same page adds them. B is -fsyntax-only, so the macOS frameworks
#      the stdlib reaches unconditionally were missing from the docs for a
#      release and nothing noticed. The landing page's C++ card is a lane
#      too — A reads its #includes, and nothing else ever read the rest.
#
#   F. `culebra embed-flags` prints this same list, and a host built from
#      nothing but its output runs. The command is the third copy of the
#      flags (source, docs, gate) and the one a host actually types, so it
#      is the one that must not drift. Needs a built binary: $BIN, or the
#      build / build-dev driver beside this checkout; skips with a word
#      when there is none. The --sqlite build is here because a feature's
#      flags have their own way to be wrong: --sources named a .c file
#      that the page then handed to the C++ driver, which rejects it.
#
# The include list below is duplicated in the docs on purpose: the gate
# exists to prove that what the doc tells a reader to type is what actually
# builds. C fails when the copies differ, B and D when the list is wrong.
#
# Usage: check_docs_cpp.sh [--fast]     (--fast runs A and C only)
set -euo pipefail
cd "$(dirname "$0")/../.."

fast=0
[[ "${1:-}" == "--fast" ]] && fast=1

# Three stages degrade to a skip when the box is missing a toolchain piece:
# B's C++23 probe, B's JIT blocks without an llvm-config at the floor, and F
# without a driver. That is right on a developer's box and wrong in CI, where
# the missing piece means the gate stopped checking what it exists to check.
# CI sets this and the skips become failures instead.
require_full="${CULEBRA_REQUIRE_DOCS_CPP:-0}"

# Report a stage that degraded for want of one of those pieces.
degraded() {
  if [[ $require_full != 0 ]]; then
    echo "docs-cpp FAIL: $1 — CULEBRA_REQUIRE_DOCS_CPP is set, so this gate" \
         "must run every stage" >&2
    exit 1
  fi
  echo "docs-cpp SKIP: $1"
}

SOURCES=(docs/*.md README.md README.ja.md)

# The include path a host build needs, exactly as documented. Every entry
# is load-bearing: without vendor/stb the build stops in font_ttf.h,
# without vendor/cpp-regexlib in regex.h, without vendor/cpp-fstlib in
# fst.h, without vendor/cpp-searchlib/include in search.h, without
# vendor/cpp-searchlib/third_party in searchlib_segment.h — the stdlib
# bindings reach all five unconditionally.
INC=(-I include
     -I vendor/cpp-peglib
     -I vendor/cpp-unicodelib
     -I vendor/cpp-tensorlib/include
     -I vendor/stb
     -I vendor/cpp-regexlib
     -I vendor/cpp-fstlib
     -I vendor/cpp-searchlib/include
     -isystem vendor/cpp-searchlib/third_party)

# The link line, as documented: zlib everywhere (Compress, and Http's gzip),
# and on macOS the frameworks FS.watch (FSEvents) and the Tensor backends
# reach whether or not the script does.
FRAMEWORKS=(CoreServices Accelerate Metal)
LIBS=(-lz)
if [[ $(uname) == Darwin ]]; then
  for fw in "${FRAMEWORKS[@]}"; do LIBS+=(-framework "$fw"); done
fi

# The pages that print that build line; C holds all of them to the lists above.
# The READMEs are not among them: they point at deployment.md rather than
# reprinting nine include flags a reader of a language README did not ask for.
BUILD_LINE_PAGES=(docs/deployment.md docs/deployment.ja.md)

# HTML to the text it shows: tags first (they use real angle brackets), then
# the entities that spell the code's own — the other order eats `</span>`.
html_text() { sed -e 's/<[^>]*>//g' -e 's/&lt;/</g' -e 's/&gt;/>/g' -e 's/&amp;/\&/g'; }

fail=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- extract every ```cpp fence -------------------------------------------

for f in "${SOURCES[@]}"; do
  [[ -f $f ]] || continue
  awk -v out="$TMP" -v tag="${f//\//_}" '
    /^```cpp$/ { n++; file = out "/" tag "-" n ".cpp"; inb = 1; next }
    /^```$/    { inb = 0 }
    inb        { print > file }
  ' "$f"
done
blocks=$(find "$TMP" -name '*.cpp' | wc -l | tr -d ' ')

if (( blocks == 0 )); then
  echo "docs-cpp FAIL: no \`\`\`cpp fences in ${SOURCES[*]} — the docs moved" >&2
  echo "  or the fence tag changed, and this gate is measuring nothing." >&2
  exit 1
fi

# --- pick a C++23 compiler ------------------------------------------------

CXXBIN="${CULEBRA_DOCS_CXX:-${CXX:-c++}}"
command -v "$CXXBIN" >/dev/null 2>&1 || CXXBIN=c++
if ! echo 'int main(){}' | "$CXXBIN" -std=c++23 -fsyntax-only -x c++ - 2>/dev/null; then
  degraded "$CXXBIN does not accept -std=c++23 (set CULEBRA_DOCS_CXX)"
  exit 0
fi

# --- A. every #include resolves -------------------------------------------

missing=0
while IFS= read -r line; do
  hdr=$(sed -n 's/.*#include *<\([^>]*\)>.*/\1/p' <<<"$line")
  [[ -n $hdr ]] || continue
  [[ -f "include/$hdr" ]] && continue
  # Not one of ours, so it has to be one the compiler finds by itself.
  printf '#include <%s>\n' "$hdr" \
    | "$CXXBIN" -std=c++23 -fsyntax-only -x c++ - 2>/dev/null && continue
  echo "docs-cpp FAIL: <$hdr> is neither under include/ nor findable —" >&2
  echo "  renamed, moved, or a typo. Named by a \`\`\`cpp block, by the" >&2
  echo "  prose around it, or by the landing page's embedding sample." >&2
  missing=1
done < <({
  grep -ho '#include *<[^>]*>' "$TMP"/*.cpp 2>/dev/null
  # The landing page carries the same embedding sample, HTML-escaped, so no
  # pass that rewrites `#include <x>` ever touched it. It sat one release
  # behind the headers until this check reached it.
  cat site/*.html 2>/dev/null | html_text | grep -ho '#include *<[^>]*>'
  # Headers named in prose, as `<base/shared.h>`. Nothing compiles those, and
  # a backticked <...h> is always a header, so the shape alone identifies them.
  grep -hoE '`<[A-Za-z0-9_/.-]+\.h>`' "${SOURCES[@]}" 2>/dev/null \
    | tr -d '`' | sed 's/^/#include /'
} | sort -u)
(( missing )) && fail=1
(( missing )) || echo "docs-cpp OK (includes): every <header> in $blocks block(s), the prose and site/ resolves"

# --- C. the documented include list is this one -----------------------------

# The first ```sh fence that spells `-I culebra/...` is the host build line;
# the later ones (Http, SQLite) add to it and are not the list.
expected=$(printf '%s %s\n' "${INC[@]}")
drifted=0
for f in "${BUILD_LINE_PAGES[@]}"; do
  [[ -f $f ]] || continue
  got=$(awk '
    /^```sh$/ { inb = 1; n = 0; next }
    /^```$/   { if (inb && n) exit; inb = 0; next }
    inb && match($0, /-(I|isystem) culebra\/[^ ]+/) {
      e = substr($0, RSTART, RLENGTH); sub(/culebra\//, "", e); print e; n++
    }
  ' "$f")
  if [[ -z $got ]]; then
    echo "docs-cpp FAIL: $f has no host build line (-I culebra/...) any more" >&2
    drifted=1
  elif [[ $got != "$expected" ]]; then
    echo "docs-cpp FAIL: $f's include list differs from check_docs_cpp.sh's:" >&2
    diff <(echo "$expected") <(echo "$got") | sed 's/^/  /' >&2 || true
    drifted=1
  fi
  # The link tokens can sit on any line of any ```sh fence (the macOS ones
  # are a comment after the build line), so this half reads all the fences.
  fences=$(awk '/^```sh$/ { inb = 1; next } /^```$/ { inb = 0 } inb' "$f")
  for tok in -lz "${FRAMEWORKS[@]/#/-framework }"; do
    grep -qF -- "$tok" <<<"$fences" && continue
    echo "docs-cpp FAIL: $f's host build line does not mention '$tok'" >&2
    drifted=1
  done
done
(( drifted )) && fail=1
(( drifted )) || echo "docs-cpp OK (docs): the deployment pages print this include list and link line"

(( fast )) && exit $fail

# --- B. complete programs compile ----------------------------------------

# The LLVM lane needs LLVM's own headers. Find llvm-config the way an
# embedder would; with none present, say so rather than passing the JIT
# blocks silently.
#
# The last resort takes the highest version installed, not the first the glob
# names: an Ubuntu runner already carries LLVM 16, and culebra's JIT needs 20+,
# so the lexical order compiled the embedding examples against headers the
# project cannot build with at all — every push, from the day this gate landed.
# A machine with two LLVMs is the normal case, not an odd one.
#
# Ordering alone still lands on a wrong one where the newest installed is under
# the floor, and the compile error that follows reads as a broken example
# rather than a missing toolchain. So a candidate also has to clear
# CULEBRA_MIN_LLVM_VERSION, read out of CMakeLists.txt so the gate and the
# build cannot drift; below it, the JIT blocks skip and say why.
LLVM_FLOOR=$(sed -n 's/^set(CULEBRA_MIN_LLVM_VERSION \([0-9]\{1,\}\).*/\1/p' CMakeLists.txt)
: "${LLVM_FLOOR:=20}"

llvm_inc=""
llvm_via=""
accept_llvm() { # llvm-config path or name
  local c=$1 v
  [[ -n $c ]] || return 1
  command -v "$c" >/dev/null 2>&1 || [[ -x $c ]] || return 1
  v=$("$c" --version 2>/dev/null) || return 1
  [[ $v =~ ^([0-9]+) ]] || return 1
  (( BASH_REMATCH[1] >= LLVM_FLOOR )) || return 1
  llvm_inc=$("$c" --includedir 2>/dev/null) || return 1
  [[ -n $llvm_inc ]] || return 1
  llvm_via=$c
  return 0
}

if [[ -n ${LLVM_CONFIG:-} ]] && accept_llvm "$LLVM_CONFIG"; then :
elif accept_llvm llvm-config; then :
else
  while IFS= read -r c; do
    accept_llvm "$c" && break
  done < <(printf '%s\n' /usr/lib/llvm-*/bin/llvm-config | sort -Vr
           printf '%s\n' /opt/homebrew/opt/llvm/bin/llvm-config)
fi

progs=()
while IFS= read -r f; do progs+=("$f"); done \
  < <(grep -l 'int main(' "$TMP"/*.cpp 2>/dev/null || true)

if (( ${#progs[@]} == 0 )); then
  echo "docs-cpp FAIL: not one of the $blocks block(s) is a complete program" >&2
  echo "  — the embedding examples lost their \`int main(\`, or extraction broke." >&2
  exit 1
fi

# Every block parses the whole stdlib header set (1.6 GB peak RSS for one
# compile), so the fan-out is bounded by RAM before cores: one per 3 GB. On an
# 8-core 16 GB machine a plain -P nproc swapped and the gate took 208 s where
# -P 4 takes 60 s.
jobs_cpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
mem_bytes=$(sysctl -n hw.memsize 2>/dev/null \
  || echo $(( $(getconf _PHYS_PAGES 2>/dev/null || echo 0) * $(getconf PAGE_SIZE 2>/dev/null || echo 4096) )))
jobs_mem=$(( mem_bytes / 3000000000 ))
(( jobs_mem >= 1 )) || jobs_mem=1
(( jobs_mem < jobs_cpu )) && jobs_cpu=$jobs_mem
JOBS="${JOBS:-$jobs_cpu}"
export CXXBIN llvm_inc TMP
export INCS="${INC[*]}"

printf '%s\n' "${progs[@]}" | xargs -P "$JOBS" -I{} bash -c '
  f="$1"
  read -ra inc <<< "$INCS"
  flags=(-std=c++23 -fsyntax-only "${inc[@]}")
  if grep -q "JIT::\|stdlib_jit\.h" "$f"; then
    if [[ -z $llvm_inc ]]; then echo "SKIP $f"; exit 0; fi
    flags+=(-DCULEBRA_JIT_ENABLED -I "$llvm_inc")
  fi
  if "$CXXBIN" "${flags[@]}" "$f" 2>"$f.err"; then echo "OK $f"; else echo "BAD $f"; fi
' _ {} > "$TMP/results" 2>/dev/null || true

built=$(grep -c '^OK '   "$TMP/results" || true)
skipped=$(grep -c '^SKIP ' "$TMP/results" || true)
bad=$(grep -c '^BAD '  "$TMP/results" || true)

if (( bad > 0 )); then
  while IFS= read -r f; do
    echo "docs-cpp FAIL: $(basename "$f") does not compile with the flags" >&2
    echo "  documented in deployment.md (Building your host program):" >&2
    # Lead with the errors: a header cascade opens with warnings out of LLVM's
    # own headers, and the first ten lines said nothing about the failure.
    { grep -m5 'error:' "$f.err" || sed -n '1,10p' "$f.err"; } >&2
  done < <(sed -n 's/^BAD //p' "$TMP/results")
  fail=1
fi

# Its own line rather than a suffix on the OK message: a JIT block that went
# unbuilt is the gate shrinking, not a footnote on a pass.
if (( skipped > 0 )); then
  degraded "$skipped JIT example(s) unbuilt: no llvm-config >= $LLVM_FLOOR"
fi

if (( fail == 0 )); then
  msg="docs-cpp OK (compile): $built complete example(s) build"
  [[ -n $llvm_via ]] && msg="$msg (LLVM via $llvm_via)"
  echo "$msg"
fi

# --- D. every entry is load-bearing -------------------------------------------

cat > "$TMP/minimal.cc" <<'EOF'
#include <culebra.h>
int main() {}
EOF
# -1 is the control: with the full list the probe has to compile, or every
# drop below fails for the same unrelated reason and this check proves nothing.
{ echo -1; seq 0 2 $(( ${#INC[@]} - 2 )); } | xargs -P "$JOBS" -I{} bash -c '
  i="$1"
  read -ra inc <<< "$INCS"
  if (( i < 0 )); then
    "$CXXBIN" -std=c++23 -fsyntax-only "${inc[@]}" "$TMP/minimal.cc" 2>/dev/null || echo CONTROL
    exit 0
  fi
  dropped="${inc[$i]} ${inc[$((i + 1))]}"
  rest=("${inc[@]:0:$i}" "${inc[@]:$((i + 2))}")
  if "$CXXBIN" -std=c++23 -fsyntax-only "${rest[@]}" "$TMP/minimal.cc" 2>/dev/null; then
    echo "UNUSED $dropped"
  fi
' _ {} > "$TMP/unused" 2>/dev/null || true

if grep -q '^CONTROL$' "$TMP/unused"; then
  echo "docs-cpp FAIL: the probe does not compile with the full include list —" >&2
  echo "  the drop results prove nothing." >&2
  fail=1
elif [[ -s $TMP/unused ]]; then
  while IFS= read -r line; do
    echo "docs-cpp FAIL: the host build line does not need '${line#UNUSED }' —" >&2
    echo "  the headers stopped reaching it. Drop it here and from the docs." >&2
  done < "$TMP/unused"
  fail=1
else
  echo "docs-cpp OK (needed): each of the $(( ${#INC[@]} / 2 )) include entries is needed"
fi

# --- E. a host links and runs, per feature, with the documented line ---------

# One program per lane; each prints the line the run below expects, so a
# feature that compiles and links but is not registered (a missing static
# registrar, a define one TU short) is caught here rather than by a host.
mkdir -p "$TMP/link"
cat > "$TMP/link/core.cc" <<'EOF'
#include <culebra.h>
#include <print>
int main() {
  culebra::vm::Embed embed;
  embed.define("host_add", [](int64_t a, int64_t b) { return a + b; });
  std::println("{}", embed.eval("host_add(40, 2)").as<int64_t>());
}
EOF
cat > "$TMP/link/http.cc" <<'EOF'
#include <culebra.h>
#include <print>
int main() {
  culebra::vm::Embed embed;
  std::println("{}", embed.eval("type_of(Http.get)").as<std::string>());
}
EOF
cat > "$TMP/link/sqlite.cc" <<'EOF'
#include <culebra.h>
#include <print>
int main() {
  culebra::vm::Embed embed;
  // SQLITE_ENABLE_FTS5: the amalgamation TU's options, not a system library's.
  std::println("{}", embed.eval(
      "db = SQLite.open(':memory:')\n"
      "db.execute('create virtual table d using fts5(b)')\n"
      "db.execute(\"insert into d values ('x')\")\n"
      "db.query(\"select b from d where d match 'x'\").size()").as<int64_t>());
}
EOF
cat > "$TMP/link/codegen.cc" <<'EOF'
#include <stdlib/codegen_binding.h>  // before culebra.h
#include <culebra.h>
#include <print>
namespace {
const bool codegen_registered = culebra::register_codegen_binding();
}
int main() {
  culebra::vm::Embed embed;
  std::println("{}", embed.eval("type_of(CodeGen.Module.new())").as<std::string>());
}
EOF

# The landing page's C++ card, extracted the way A reads its includes: tags
# first (they use real angle brackets), then the entities that spell the
# code's own. It is HTML, so no pass over the .md files ever touched it.
awk '
  /<pre class="sample">/ { blk = ""; inb = 1 }
  inb                    { blk = blk $0 "\n" }
  inb && /<\/pre>/       { inb = 0; if (blk ~ /#include/) { printf "%s", blk; exit } }
' site/index.html | html_text > "$TMP/link/site.cc"
if ! grep -q 'int main(' "$TMP/link/site.cc"; then
  echo "docs-cpp FAIL: site/index.html has no C++ sample with an int main() —" >&2
  echo "  the card moved or its markup changed, and its lane below proves nothing." >&2
  fail=1
fi

# lane: name | extra compile flags | extra sources or objects | expected stdout
lanes=(
  "core|||42"
  "http|-DCULEBRA_HTTP_ENABLED -I vendor/cpp-httplib||Function"
  "sqlite|-DCULEBRA_SQLITE_ENABLED -I vendor/sqlite|$TMP/link/culebra_sqlite3.o|1"
  "codegen|-I vendor/cpp-vmlib|src/runtime/codegen_rt.cc|Module"
  "site|||true false"
)
"${CULEBRA_DOCS_CC:-${CC:-cc}}" -O0 -w -c src/runtime/culebra_sqlite3.c -o "$TMP/link/culebra_sqlite3.o"

export LIBS_STR="${LIBS[*]}"
printf '%s\n' "${lanes[@]}" | xargs -P "$JOBS" -I{} bash -c '
  IFS="|" read -r name flags extra want <<< "$1"
  read -ra inc <<< "$INCS"; read -ra libs <<< "$LIBS_STR"; read -ra fl <<< "$flags"
  src="$TMP/link/$name.cc"; exe="$TMP/link/$name"
  if ! "$CXXBIN" -std=c++23 -O0 -w "${inc[@]}" "${fl[@]}" "$src" $extra "${libs[@]}" -o "$exe" 2>"$exe.err"; then
    echo "BAD $name (build)"; exit 0
  fi
  got=$("$exe" 2>>"$exe.err") || { echo "BAD $name (run)"; exit 0; }
  [[ $got == "$want" ]] && echo "OK $name" || echo "BAD $name (printed: $got)"
' _ {} > "$TMP/link/results" 2>/dev/null || true

linked=$(grep -c '^OK ' "$TMP/link/results" || true)
if grep -q '^BAD ' "$TMP/link/results"; then
  while IFS= read -r line; do
    name=${line#BAD }; name=${name%% *}
    echo "docs-cpp FAIL: the documented $name host build does not link and run: ${line#BAD }" >&2
    { grep -m5 'error' "$TMP/link/$name.err" || sed -n '1,10p' "$TMP/link/$name.err"; } >&2
  done < <(grep '^BAD ' "$TMP/link/results")
  fail=1
elif (( linked != ${#lanes[@]} )); then
  echo "docs-cpp FAIL: only $linked of the ${#lanes[@]} host lanes reported a verdict —" >&2
  echo "  the lane runner died before finishing, so this check proved nothing." >&2
  fail=1
else
  echo "docs-cpp OK (link): $linked host builds link and run (core, Http, SQLite, CodeGen, the site card)"
fi

# --- F. `culebra embed-flags` says the same thing, and its output builds -----

# `just doctest` builds build/ first, so that driver is the fresh one; a
# build-dev/ left over from an inner loop would otherwise answer for it, and a
# stale answer is the drift this stage is looking for.
EF_BIN="${BIN:-}"
[[ -n $EF_BIN && -x $EF_BIN ]] || EF_BIN=build/culebra
[[ -x $EF_BIN ]] || EF_BIN=build-dev/culebra
if [[ ! -x $EF_BIN ]]; then
  degraded "embed-flags has no built driver (set BIN=<path>)"
  exit $fail
fi

# The include entries the command prints, in the shape INC holds them: the
# paths are absolute there, and this checkout is what they are absolute to.
# CULEBRA_HOME is what the command answers with, and a developer who has it
# exported would otherwise diff two identical lists under different roots.
export CULEBRA_HOME="$PWD"
if ! ef_cflags_out=$("$EF_BIN" embed-flags --cflags); then
  echo "docs-cpp FAIL: \`$EF_BIN embed-flags --cflags\` exited non-zero" >&2
  exit 1
fi
ef_inc=$(printf '%s\n' "$ef_cflags_out" \
  | tr ' ' '\n' \
  | awk -v root="$PWD/" '
      $0 == "-I" || $0 == "-isystem" { flag = $0; next }
      flag != "" { sub("^" root, "", $0); print flag " " $0; flag = "" }
    ')
if [[ $ef_inc != "$expected" ]]; then
  echo "docs-cpp FAIL: \`culebra embed-flags --cflags\` differs from this list:" >&2
  diff <(echo "$expected") <(echo "$ef_inc") | sed 's/^/  /' >&2 || true
  fail=1
fi

# A checkout is what the flags name, so the command owes an error rather than
# paths from the machine it was built on when it has none. Run it somewhere
# that is not one, with the override this stage sets undone.
if (CULEBRA_HOME="$TMP" "$EF_BIN" embed-flags --cflags >/dev/null 2>&1); then
  echo "docs-cpp FAIL: embed-flags answered for a directory that is no checkout" >&2
  fail=1
fi

# And the flags work. One lane per feature that has flags of its own, each
# building the program stage E wrote for it, from the command's output alone:
# a wrong entry the documented list happens to carry passes the comparison
# above and fails here. In parallel, since a host compile is the whole cost.
#
# lane: name | embed-flags features | program | expected stdout
ef_lanes=(
  "core||$TMP/link/core.cc|42"
  "sqlite|--sqlite|$TMP/link/sqlite.cc|1"
  "codegen|--codegen|$TMP/link/codegen.cc|Module"
  "http|--http|$TMP/link/http.cc|Function"
)
export EF_BIN
printf '%s\n' "${ef_lanes[@]}" | xargs -P "$JOBS" -I{} bash -c '
  IFS="|" read -r name feats prog want <<< "$1"
  read -ra feat <<< "$feats"
  err="$TMP/link/ef_$name.err"
  read -ra cf <<< "$("$EF_BIN" embed-flags --cflags "${feat[@]}")"
  read -ra lb <<< "$("$EF_BIN" embed-flags --libs "${feat[@]}")"
  objs=()
  while IFS= read -r src; do
    [[ -n $src ]] || continue
    o="$TMP/link/ef_${name}_$(basename "${src%.*}").o"
    # Each source through its own language driver: the SQLite amalgamation is
    # C, and a C++ compiler stops on the first `new` it uses as a name.
    if [[ $src == *.c ]]; then
      "${CULEBRA_DOCS_CC:-${CC:-cc}}" -w -c "$src" -o "$o" 2>>"$err" || { echo "BAD $name (sources)"; exit 0; }
    else
      "$CXXBIN" -w "${cf[@]}" -c "$src" -o "$o" 2>>"$err" || { echo "BAD $name (sources)"; exit 0; }
    fi
    objs+=("$o")
  done < <("$EF_BIN" embed-flags --sources "${feat[@]}")
  if ! "$CXXBIN" -w "${cf[@]}" "$prog" "${objs[@]}" "${lb[@]}" \
         -o "$TMP/link/ef_$name" 2>>"$err"; then
    echo "BAD $name (build)"; exit 0
  fi
  got=$("$TMP/link/ef_$name" 2>>"$err") || { echo "BAD $name (run)"; exit 0; }
  [[ $got == "$want" ]] && echo "OK $name" || echo "BAD $name (printed: $got)"
' _ {} > "$TMP/link/ef_results" 2>/dev/null || true

ef_ok=$(grep -c '^OK ' "$TMP/link/ef_results" || true)
if grep -q '^BAD ' "$TMP/link/ef_results"; then
  while IFS= read -r line; do
    name=${line#BAD }; name=${name%% *}
    echo "docs-cpp FAIL: \`culebra embed-flags\` flags do not build a host: ${line#BAD }" >&2
    { grep -m5 'error' "$TMP/link/ef_$name.err" || sed -n '1,10p' "$TMP/link/ef_$name.err"; } >&2
  done < <(grep '^BAD ' "$TMP/link/ef_results")
  fail=1
elif (( ef_ok != ${#ef_lanes[@]} )); then
  echo "docs-cpp FAIL: only $ef_ok of the ${#ef_lanes[@]} embed-flags lanes reported a verdict" >&2
  fail=1
elif (( fail == 0 )); then
  echo "docs-cpp OK (embed-flags): the command prints this list, and $ef_ok hosts built from it run (core, SQLite, CodeGen, Http)"
fi

exit $fail
