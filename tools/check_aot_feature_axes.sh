#!/usr/bin/env bash
# AOT feature-axis gate. Two different mechanisms keep an unused feature's
# code out of a binary, and this script verifies both. What decides which
# one applies is whether the feature has a compile-time side effect the link
# cannot undo — a global constructor pins its whole translation unit, so
# only a separate archive keeps it out; plain code does not, so the
# namespace group is enough.
#
#   - Regex is a weak/strong archive axis (regex.h / kFeatureAxes): the base
#     archive's choke is a throwing stub, force-loading the real body only
#     when the AST names Regex. Losing this axis is silent either way (no
#     link error, no ldd hit) — regexlib.h's __builtin_cpu_supports() call
#     makes GCC emit a start-up CPUID constructor for whatever translation
#     unit compiles it, and that constructor is NOT function-level dead code
#     (see the comment in include/regex.h), so if regexlib.h ever gets
#     included from a translation unit that is unconditionally linked (not
#     this axis's own archive), an unrelated hello-world binary picks up the
#     constructor even though nothing calls it.
#   - __Foreign is an axis for the same reason one step removed: the fixture
#     registers itself with a static `wrap<T>` initializer, which .init_array
#     pins, dragging the wrap metadata and template instantiations behind it
#     (~64 KB) into every binary if it lives in the core archive.
#   - libstdc++'s formatter is neither: it is kept out by nothing being able
#     to name it. Its argument visitor names the integer, float and string
#     formatters from one function, so one reachable std::format call links
#     all of it — 15% of a hello — and a header the runtime archive compiles
#     therefore formats messages with culebra::format (include/rt_format.h).
#     Losing that is silent too: the build links, the binary grows. Every
#     probe below is checked for it, since the leak follows whatever the
#     program touches (a @packable class, Proc, a wrapped class), and the
#     `spec` probe is the control that proves the check still bites.
#   - Proc and the Canvas PNG/TTF decoders are NOT an axis: they compile as
#     plain `inline` code, reached only through their `_ns_*` adapters. Once
#     a namespace's dispatch group (stdlib_jit.h ns_groups()) is unreferenced,
#     `--gc-sections` drops the group, its adapters, and everything only they
#     reached — the same mechanism §4 of docs/deployment.md describes for
#     Math/IO. This script checks that these choke functions are present when
#     the namespace is used and gone (not merely stubbed) when it is not.
#
# The choke names below are this script's own copy of what the source
# actually reaches; renaming one means updating it.
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
expect_strong() {  # expect_strong <binary> <symbol ERE> <why>
  local got
  got=$(sym_class "$1" "$2")
  if [[ "$got" != "T" ]]; then
    echo "check_aot_feature_axes FAIL: $1: $2 is '$got', expected 'T' ($3)" >&2
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
# The strong body must not be linked: its weak stub is (the choke a linked
# adapter still calls), or nothing is — once no adapter names the choke, the
# stub is dead-stripped with it, and that is the better of the two outcomes.
expect_stub_or_absent() {  # expect_stub_or_absent <binary> <symbol ERE> <why>
  local got
  got=$(sym_class "$1" "$2")
  if [[ "$got" != "W" && "$got" != "" ]]; then
    echo "check_aot_feature_axes FAIL: $1: $2 is '$got', expected 'W' or absent ($3)" >&2
    fail=1
  fi
}
expect_present() {  # expect_present <binary> <ERE> <why>
  if ! grep -qE "$2" "$work/$1.nm"; then
    echo "check_aot_feature_axes FAIL: $1 lacks $2 ($3)" >&2
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

# The standard library's formatter, by its entry points: the visitor and
# vformat rather than `__format` alone, since <charconv> — which
# culebra::format uses too — shares that namespace on some libraries.
#
# Both libraries are named, because the alternation has to match wherever the
# gate runs and neither library's names appear under the other. The libstdc++
# half alone made every expect_absent below vacuously true on macOS, and the
# `spec` control is what reported it: libc++ puts its formatters in a
# `__formatter` namespace under the `__1` inline namespace, so not one of the
# three libstdc++ alternatives can match there. libc++'s own `vformat` is not
# a third alternative — it inlines away under -O3 + LTO, so only the two
# per-type formatters are reliable.
fmt_machinery='std::__format::__do_vformat_to|std::vformat|__format::__formatter_(fp|int)|__formatter::__format_(integer|floating_point)'

# Regex: the weak/strong choke (kFeatureAxes still force-loads this one).
regex_choke='^culebra::regex::compile[(]'
# Proc / Canvas assets: no weak stub — plain inline code reached only through
# the namespace's dispatch group, so `culebra::proc::run_command` and
# `culebra::image::decode_png` / `culebra::_canvas_detail::ttf_load` are either
# linked (as ordinary — usually weak/COMDAT — symbols) or gone entirely.
proc_choke=' culebra::proc::run_command[(]'
png_choke=' culebra::image::decode_png[(]'
ttf_choke=' culebra::_canvas_detail::ttf_load[(]'
# The __Foreign fixture is its own archive: a `wrap<T>` registrar its TU pins
# through static init, so it can't ride namespace-group dead-stripping either.
# The class metadata wrap<T> instantiates is the tell — the registrar variable
# itself is the archive TU's, with a name this has no business knowing.
foreign_choke='jit_class_info<culebra::foreign_fixture::Counter>::methods'

# 1. Names none of them: Regex stubbed, Proc/Canvas engines entirely absent.
build none 'IO.print("none")'
expect_stub_or_absent none "$regex_choke" "no Regex use"
expect_absent none "$proc_choke" "no Proc use"
expect_absent none "$png_choke" "no Canvas use"
expect_absent none "$ttf_choke" "no Canvas use"
expect_absent none "$foreign_choke" "no __Foreign use"
expect_absent none ' reg::' "regexlib"
expect_absent none 'culebra::proc::_detail::' "the fork/exec layer"
expect_absent none 'culebra::_canvas_detail::ttf_rasterize' "stb_truetype"
expect_absent none 'culebra::foreign_fixture::' "the fixture's own C++ class"
# The isolate transfer graph (serialize / SharedVal readers / channel
# endpoints) is reached only through the hooks _jit_make_shared_val_view
# installs (jit_runtime.h), never by symbol from the generic property paths.
expect_absent none 'culebra::jit_serialize[(]' "the isolate transfer graph"
expect_absent none 'culebra::_jit_shared_val_prop_impl[(]' "the SharedVal reader"
expect_absent none '_jit_isolate_teardown_join_all' "the isolate teardown join"
expect_absent none "$fmt_machinery" "libstdc++'s formatter, a program that formats nothing"
expect_output none "none"
# The namespace groups (stdlib_jit.h ns_groups()): a namespace's dispatch rows
# and adapters link only when the program names it. No axis, no choke — the
# program object's culebra_aot_ns_groups[] is the one reference that keeps a
# group, so an unnamed one is dead-stripped with everything only it reached.
# `_?`: these are C symbols, and Mach-O spells them with a leading underscore.
expect_present none ' _?culebra_ns_group_IO$' "IO named, its group must be linked"
expect_absent none ' _?culebra_ns_group_Math$' "the Math group"
expect_absent none ' [A-Za-z] culebra::_ns_isolate_spawn[(]' "the Isolate adapter"
expect_absent none ' [A-Za-z] culebra::_ns_http_[a-z_]*[(]' "the Http adapters"

# 2. Each on its own: the engine is what runs, and no other feature's code
#    came along with it.
build regex 'IO.print(re"(\d+)-(\d+)".find("a 12-34 b").groups[2].value)'
expect_strong regex "$regex_choke" "Regex named, the strong body must override"
expect_absent regex "$proc_choke" "Regex only"
expect_absent regex "$fmt_machinery" "libstdc++'s formatter, Regex"
expect_output regex "34"

build math 'let m = Math
IO.print(m.abs(-3) + Math.floor(1.5))'
expect_present math ' _?culebra_ns_group_Math$' "Math named, its group must be linked"
expect_present math 'culebra::_ns_math_floor[(]' "a Math adapter, reached only through the group"
expect_absent math ' _?culebra_ns_group_FS$' "the FS group"
expect_absent math "$fmt_machinery" "libstdc++'s formatter, Math"
expect_output math "4"

build proc 'IO.print(Proc.run(["echo", "spawned"]).stdout)'
expect_present proc "$proc_choke" "Proc named"
expect_stub_or_absent proc "$regex_choke" "Proc only"
expect_absent proc "$fmt_machinery" "libstdc++'s formatter, Proc"
expect_output proc "spawned"

# from_png decodes what to_png encoded (the latter rides the Compress axis).
build canvas 'let s = Canvas.Sprite.blank(3, 2, 0xFF336699)
let back = Canvas.Sprite.from_png(s.to_png())
IO.print(back.width() * 10 + back.height())'
expect_present canvas "$png_choke" "Canvas named"
expect_present canvas "$ttf_choke" "Canvas named"
expect_stub_or_absent canvas "$regex_choke" "Canvas only"
expect_absent canvas "$fmt_machinery" "libstdc++'s formatter, Canvas"
expect_output canvas "32"

# The wrap fixture: naming __Foreign force-loads its archive, and the
# registrar has to have run for the namespace to resolve at all.
build foreign 'let c = __Foreign.Counter.new(10)
c.add(5)
IO.print(c.value())'
expect_present foreign "$foreign_choke" "__Foreign named"
expect_stub_or_absent foreign "$regex_choke" "__Foreign only"
expect_absent foreign "$fmt_machinery" "libstdc++'s formatter, __Foreign"
expect_output foreign "15"

# A Shared.new view: its reader arrives through the hook, and the view's
# `copy` reaches the deserializer, which reaches everything else.
build shared 'let s = Shared.new({a: 1, xs: [10, 20]})
IO.print(s.a + s.xs[1])'
expect_present shared 'culebra::_jit_shared_val_prop_impl[(]' "Shared named"
expect_present shared 'culebra::jit_serialize[(]' "Shared named"
expect_stub_or_absent shared "$regex_choke" "Shared only"
expect_absent shared "$fmt_machinery" "libstdc++'s formatter, Shared"
expect_output shared "21"

# The spec after a colon in an interpolation IS std::format's mini-language
# (shared.h's format_value_as vformats it), so this one program is expected to
# carry the formatter. Without it, the seven checks above could pass because
# the probe stopped working rather than because the boundary holds.
build spec 'let x = 1.5
IO.print("{x:.2f}")'
expect_present spec "$fmt_machinery" "an interpolation spec asks for std::format"
expect_output spec "1.50"

if (( fail )); then
  cat >&2 <<'EOF'
  Regex 'W' (or nothing) where 'T' was expected: the axis did not force-load —
  check the kFeatureAxes row (src/main.cc) and that libculebra_rt_regex.a is
  in _rt_embed_files (CMakeLists). A Regex strong body where none was
  expected, or a `reg::` symbol in `none`: something bypasses the
  CULEBRA_RT_REGEX_WEAK gate, or regexlib.h leaked into an always-linked
  translation unit (see the comment in include/regex.h).
  Proc / Canvas: a choke present in `none`, or absent where it was named:
  something outside the choke reaches the engine unconditionally, or the
  adapter isn't reachable only through its kNsRows_* table.
  libstdc++'s formatter in a probe: a header the runtime archive compiles
  called std::format (or std::print/println) — put the message on
  culebra::format instead, see include/rt_format.h. Missing from `spec`:
  the control stopped working, so fix the probe (or fmt_machinery) before
  trusting the rest.
  A namespace group or adapter in a binary that never names it: something in
  the core archive refers to the group (only the program object may), or an
  adapter is reachable outside its kNsRows_* table.
EOF
  exit 1
fi
echo "aot-feature-axes OK (Regex / __Foreign by axis; Proc/Canvas/Shared by namespace group; groups linked only when named; no libstdc++ formatter)"
