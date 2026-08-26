#!/usr/bin/env bash
# AOT feature-axis gate for the self-contained axes (Regex / Proc / Canvas
# assets). The library axes fail loudly when lost (a link error, OpenSSL in
# `ldd`); these link nothing external, so both ways of losing one are silent:
# the core's weak stub satisfying every reference (the program links, then
# throws at the first match), or the engine creeping back into every binary
# (the program links and runs, 320 KB heavier). Read off the linked outputs:
#
#   1. a program that names none of the axes links the WEAK stub (`W`) for
#      each choke and no engine symbol behind it;
#   2. a program that names an axis links the STRONG body (`T`) and runs it.
#
# The choke names below are this script's own copy of what the headers stub
# and src/main.cc's kFeatureAxes force-loads; renaming one means updating it.
#
# Usage: tools/check_aot_feature_axes.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
bin="$BUILD_DIR/culebra"
[[ -x "$bin" ]] || { echo "check_aot_feature_axes: no $bin" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-axes.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail=0
build() {  # build <name> <source>: the binary plus one nm listing of it
  # (--keep-symbols: the default post-link strip would leave nm nothing to
  # read; which bodies got linked is the question here, not the symbol table)
  printf '%s\n' "$2" > "$work/$1.cul"
  if ! "$bin" build --keep-symbols "$work/$1.cul" -o "$work/$1" > "$work/$1.err" 2>&1; then
    echo "check_aot_feature_axes FAIL: $1 did not build:" >&2
    cat "$work/$1.err" >&2
    exit 1
  fi
  nm -C --defined-only "$work/$1" > "$work/$1.nm"
}
# nm class letter of the defined symbol whose demangled name starts with `$2`
# (int64_t demangles as `long` on LP64 and `long long` on Mach-O, so the
# patterns stop before the argument list).
sym_class() {
  awk -v n="$2" '$3 ~ n { print toupper($2); exit }' "$work/$1.nm"
}
expect_class() {  # expect_class <binary> <symbol ERE> <W|T> <why>
  local got
  got=$(sym_class "$1" "$2")
  if [[ "$got" != "$3" ]]; then
    echo "check_aot_feature_axes FAIL: $1: $2 is '$got', expected '$3' ($4)" >&2
    fail=1
  fi
}
expect_absent() {  # expect_absent <binary> <ERE> <why>
  local hits
  hits=$(grep -E "$2" "$work/$1.nm" | head -3 || true)
  if [[ -n "$hits" ]]; then
    echo "check_aot_feature_axes FAIL: $1 carries the engine it never names ($3):" >&2
    printf '%s\n' "$hits" | sed 's/^/  /' >&2
    fail=1
  fi
}
expect_output() {  # expect_output <binary> <expected stdout>
  local got
  got=$("$work/$1")
  if [[ "$got" != "$2" ]]; then
    echo "check_aot_feature_axes FAIL: $1 printed '$got', expected '$2'" >&2
    fail=1
  fi
}

# One choke per axis: every entry point of an axis shares its TU and linkage.
regex_choke='^culebra::regex::compile[(]'
proc_choke='^culebra::proc::kill_pid[(]'
png_choke='^culebra::image::decode_png[(]'
ttf_choke='^culebra::_canvas_detail::ttf_free[(]'

# 1. Names none of them: stubs only, engines absent.
build none 'IO.print("none")'
expect_class none "$regex_choke" W "no Regex use, the core stub must be linked"
expect_class none "$proc_choke" W "no Proc use"
expect_class none "$png_choke" W "no Canvas use"
expect_class none "$ttf_choke" W "no Canvas use"
expect_absent none ' reg::' "regexlib"
expect_absent none 'culebra::proc::_detail::' "the fork/exec layer"
expect_absent none 'culebra::_canvas_detail::ttf_rasterize' "stb_truetype"
expect_output none "none"

# 2. Each axis on its own: the strong body is what runs, and no other axis's
#    archive came along with it.
build regex 'IO.print(re"(\d+)-(\d+)".find("a 12-34 b").groups[2].value)'
expect_class regex "$regex_choke" T "Regex named, the strong body must override"
expect_class regex "$proc_choke" W "Regex only"
expect_output regex "34"

build proc 'IO.print(Proc.run(["echo", "spawned"]).stdout)'
expect_class proc "$proc_choke" T "Proc named"
expect_class proc "$regex_choke" W "Proc only"
expect_output proc "spawned"

# from_png decodes what to_png encoded (the latter rides the Compress axis).
build canvas 'let s = Canvas.Sprite.blank(3, 2, 0xFF336699)
let back = Canvas.Sprite.from_png(s.to_png())
IO.print(back.width() * 10 + back.height())'
expect_class canvas "$png_choke" T "Canvas named"
expect_class canvas "$ttf_choke" T "Canvas named"
expect_class canvas "$regex_choke" W "Canvas only"
expect_output canvas "32"

if (( fail )); then
  cat >&2 <<'EOF'
  A 'W' where 'T' was expected: the axis did not force-load — check the
  kFeatureAxes row (src/main.cc) and that the archive is in _rt_embed_files
  (CMakeLists). A 'T' where 'W' was expected, or an engine symbol in `none`:
  something outside the choke reaches the engine (a new call site that
  bypasses the CULEBRA_RT_*_WEAK gate, or the gate lost its #if).
EOF
  exit 1
fi
echo "aot-feature-axes OK (Regex / Proc / Canvas-assets stubbed when unused, strong when named)"
