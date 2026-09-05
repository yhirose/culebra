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
#     (see the comment in include/stdlib/regex.h), so if regexlib.h ever gets
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
#     therefore formats messages with culebra::format (include/base/format.h).
#     Losing that is silent too: the build links, the binary grows. Every
#     probe below is checked for it, since the leak follows whatever the
#     program touches (a @packable class, Proc, a wrapped class), and the
#     `spec` probe is the control that proves the check still bites.
#   - Proc, the Canvas PNG/TTF decoders, and PEG are NOT an axis: they compile
#     as plain `inline` code, reached only through their `_ns_*` adapters. Once
#     a namespace's dispatch group (stdlib_rt.h ns_groups()) is unreferenced,
#     `--gc-sections` drops the group, its adapters, and everything only they
#     reached — the same mechanism §4 of docs/deployment.md describes for
#     Math/IO. This script checks that these choke functions are present when
#     the namespace is used and gone (not merely stubbed) when it is not.
#     PEG is the one exception `--gc-sections` only partly reaches: peglib's
#     Ope class hierarchy leaves typeinfo/vtables behind (see peg.h), and a
#     couple of `std::function`-wrapped local lambdas leave an inert
#     `_Function_handler<...>::_M_manager` comdat behind even after their only
#     caller is pruned (a linker limitation, not a reachable call path). Both
#     are accepted rather than chased with a second archive: they're fixed at
#     a measured ~53 KB total and never executed in a binary that never names
#     PEG (see peg.h). What this script's `none` probe checks instead is the
#     thing that DOES matter -- that `culebra::pegparser::compile` itself
#     (the actual entry point, not a nested lambda's enclosing-scope name) is
#     gone, not merely stubbed, when the program never names PEG.
#
# The choke names below are this script's own copy of what the source
# actually reaches; renaming one means updating it.
#
# Usage: tools/checks/check_aot_feature_axes.sh <build dir>
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${1:-build}"
bin="$BUILD_DIR/culebra"
[[ -x "$bin" ]] || { echo "check_aot_feature_axes: no $bin" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-axes.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail=0
build() {  # build <name> <source> [extra build flags...]: binary + nm listing
  # (--keep-symbols: the default post-link strip would leave nm nothing to
  # read; which bodies got linked is the question here, not the symbol table)
  printf '%s\n' "$2" > "$work/$1.cul"
  local extra=("${@:3}")
  if ! "$bin" build --keep-symbols "${extra[@]}" "$work/$1.cul" -o "$work/$1" > "$work/$1.err" 2>&1; then
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
# Checks the nm class of the ONE symbol whose demangled name starts with
# <symbol-start ERE> (sym_class's field-3 anchor) against <allowed-class ERE>
# -- "T" for strong-linked, "W|" for weak-or-absent, ".+" for defined
# (any class), "" for absent. This is the anchored idiom for "does this one
# fully-qualified symbol exist, and in what form" -- use it for any new choke
# shaped that way. It is NOT a substitute for expect_absent/expect_present
# below, which stay in whole-line grep -E form on purpose for the checks
# further down that are deliberately broad (an alternation of several
# patterns, or "no symbol anywhere under this namespace prefix" rather than
# one exact function) -- anchoring those would narrow what they catch.
expect_class() {  # expect_class <binary> <symbol-start ERE> <allowed-class ERE> <expected-desc> <why>
  local got
  got=$(sym_class "$1" "$2")
  if ! [[ "$got" =~ ^($3)$ ]]; then
    echo "check_aot_feature_axes FAIL: $1: $2 is '$got', $4 ($5)" >&2
    fail=1
  fi
}
# Whole-line grep -E, unanchored: matches a choke's demangled name wherever it
# appears as a substring, including nested inside an unrelated symbol (a
# lambda's mangled enclosing-scope name, say) -- fine for the deliberately
# broad checks below (fmt_machinery's alternation, a bare namespace prefix
# like ' reg::'), wrong for "does this one function exist" (use expect_class).
expect_absent() {  # expect_absent <binary> <ERE> <why>
  local hits
  hits=$(grep -E "$2" "$work/$1.nm" | head -3 || true)
  if [[ -n "$hits" ]]; then
    echo "check_aot_feature_axes FAIL: $1 carries the engine it never names ($3):" >&2
    printf '%s\n' "$hits" | sed 's/^/  /' >&2
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
# Search: the same shape over cpp-searchlib. Losing this axis is silent too —
# the binary still runs, it just carries the whole engine (postings, the
# succinct structures, the FST term dictionary and a PEG query grammar).
search_choke='^culebra::search::index_new[(]'
# PEG / Proc / Canvas assets: no weak stub — plain inline code reached only
# through the namespace's dispatch group, so these are either linked (as
# ordinary — usually weak/COMDAT — symbols) or gone entirely. The first three
# are each one fully-qualified function, checked with expect_class (sym_class's
# field-3-start anchor): a whole-line grep -E would also match a nested
# lambda's mangled enclosing-scope name containing the same text as a
# substring (see the PEG note above, which is where this was first caught) --
# any future choke of this exact shape (one function, `name[(]`) should use
# expect_class too, not expect_absent/expect_present.
peg_choke='^culebra::pegparser::compile[(]'
proc_choke='^culebra::proc::run_command[(]'
png_choke='^culebra::image::decode_png[(]'
ttf_choke='^culebra::_canvas_detail::ttf_load[(]'
# The __Foreign fixture is its own archive: a `wrap<T>` registrar its TU pins
# through static init, so it can't ride namespace-group dead-stripping either.
# The class metadata wrap<T> instantiates is the tell — the registrar variable
# itself is the archive TU's, with a name this has no business knowing. Unlike
# the three above, this isn't one top-level function under a stable qualified
# name: it's a template instantiation reached under `culebra::wrap_detail::`
# through several related symbols (a static member, its guard variable,
# emplace_back thunks, …), so it stays on the broader expect_absent/present.
foreign_choke='jit_class_info<culebra::foreign_fixture::Counter>::methods'

# CodeGen.Module is wrap<T>'d the same way, for the same reason: its
# registrar is a static initializer (src/runtime/culebra_rt_codegen.cc), not
# namespace-group-strippable, so it gets its own force-load axis.
codegen_choke='jit_class_info<culebra::codegen::Module>::methods'

# Tensor's elementwise kernels. The backend choke (tensor_eval_node) gates
# BLAS/Metal, but cpp-tensorlib's map_binary instantiations are pure C++ and
# so were never behind it -- and the generic arithmetic helper reaches them
# whenever either operand could be a Tensor, which is every binary. They are
# choked at culebra::tensor_binop / tensor_inplace_binop (tensor.h) instead.
# Checked by the kernels rather than by the choke's nm class: ld64 reports the
# core archive's weak definition as 'T' once it is the only one linked, so the
# class letter cannot tell the stub from the real body. Losing this axis is
# silent -- the binary still runs, it just carries ~115 KB it never enters.
tensor_kernels='tl::detail::map_binary'

# OpenSSL, by two symbols that cannot both survive an accident: an entry point
# the TLS client calls and the largest table it drags along (512 KB of SM2
# curve, for an algorithm no HTTPS client on the public web negotiates — it is
# live because OpenSSL 3's default provider names every MAC, which reaches
# every PKEY method, which reaches every curve). Http has two archives rather
# than one, and `--no-tls` force-loads the httplib built without TLS: that is
# 5.07 MB against 1.22 MB, so losing the axis is a 4x regression that nothing
# else would report.
openssl_syms='SSL_CTX_new|ecp_sm2p256_precomputed'

# 1. Names none of them: Regex stubbed, Proc/Canvas/PEG engines entirely absent.
build none 'IO.print("none")'
expect_class none "$regex_choke" "W?" "expected 'W' or absent" "no Regex use"
expect_class none "$search_choke" "W?" "expected 'W' or absent" "no Search use"
expect_class none "$peg_choke" "" "expected absent" "no PEG use"
expect_class none "$proc_choke" "" "expected absent" "no Proc use"
expect_class none "$png_choke" "" "expected absent" "no Canvas use"
expect_class none "$ttf_choke" "" "expected absent" "no Canvas use"
expect_absent none "$foreign_choke" "no __Foreign use"
expect_absent none "$codegen_choke" "no CodeGen use"
expect_absent none ' reg::' "regexlib"
expect_absent none 'searchlib::InMemoryInvertedIndexBase' "cpp-searchlib"
# peglib's typeinfo/vtables, and a couple of unreachable std::function comdat
# thunks, survive here by design (see the PEG note above) -- culebra's own
# front-end parser (parser.h) already needs a working peg::parser regardless
# of this program, so raw `peg::` symbols are not checked for absence here.
expect_absent none 'culebra::proc::_detail::' "the fork/exec layer"
expect_absent none 'culebra::_canvas_detail::ttf_rasterize' "stb_truetype"
expect_absent none 'culebra::foreign_fixture::' "the fixture's own C++ class"
# The isolate transfer graph (serialize / SharedVal readers / channel
# endpoints) is reached only through the hooks _jit_make_shared_val_view
# installs (rt_runtime.inc.h), never by symbol from the generic property paths.
expect_absent none 'culebra::jit_serialize[(]' "the isolate transfer graph"
expect_absent none 'culebra::_jit_shared_val_prop_impl[(]' "the SharedVal reader"
expect_absent none '_jit_isolate_teardown_join_all' "the isolate teardown join"
expect_absent none "$fmt_machinery" "libstdc++'s formatter, a program that formats nothing"
expect_absent none "$tensor_kernels" "cpp-tensorlib's elementwise kernels"
expect_absent none "$openssl_syms" "OpenSSL"
expect_output none "none"
# The namespace groups (stdlib_rt.h ns_groups()): a namespace's dispatch rows
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
expect_class regex "$regex_choke" "T" "expected 'T'" "Regex named, the strong body must override"
expect_class regex "$proc_choke" "" "expected absent" "Regex only"
expect_class regex "$peg_choke" "" "expected absent" "Regex only"
expect_absent regex "$fmt_machinery" "libstdc++'s formatter, Regex"
expect_output regex "34"

# Search: the strong body has to override, and the printed key is what proves
# the engine actually ran — the core archive's stub throws.
build search 'let idx = Search.Index.new()
idx.add("a", "the quick brown fox")
idx.add("b", "a lazy dog")
IO.print(idx.search("quick")[0].key)'
expect_class search "$search_choke" "T" "expected 'T'" "Search named, the strong body must override"
expect_class search "$regex_choke" "W?" "expected 'W' or absent" "Search only"
# The engine parses queries with peglib, so `peg_choke` is the check that
# reaching peglib does not drag culebra's own PEG namespace in with it.
expect_class search "$peg_choke" "" "expected absent" "Search only"
expect_class search "$proc_choke" "" "expected absent" "Search only"
expect_absent search "$fmt_machinery" "libstdc++'s formatter, Search"
expect_output search "a"

build peg 'IO.print(PEG.parse(`N <- < [0-9]+ >`, "42").token)'
expect_class peg "$peg_choke" ".+" "expected defined" "PEG named"
expect_class peg "$regex_choke" "W?" "expected 'W' or absent" "PEG only"
expect_class peg "$search_choke" "W?" "expected 'W' or absent" "PEG only"
expect_absent peg "$fmt_machinery" "libstdc++'s formatter, PEG"
expect_output peg "42"

build math 'let m = Math
IO.print(m.abs(-3) + Math.floor(1.5))'
expect_present math ' _?culebra_ns_group_Math$' "Math named, its group must be linked"
expect_present math 'culebra::_ns_math_floor[(]' "a Math adapter, reached only through the group"
expect_absent math ' _?culebra_ns_group_FS$' "the FS group"
expect_absent math "$fmt_machinery" "libstdc++'s formatter, Math"
expect_output math "4"

build proc 'IO.print(Proc.run(["echo", "spawned"]).stdout)'
expect_class proc "$proc_choke" ".+" "expected defined" "Proc named"
expect_class proc "$regex_choke" "W?" "expected 'W' or absent" "Proc only"
expect_class proc "$search_choke" "W?" "expected 'W' or absent" "Proc only"
expect_class proc "$peg_choke" "" "expected absent" "Proc only"
expect_absent proc "$fmt_machinery" "libstdc++'s formatter, Proc"
expect_output proc "spawned"

# from_png decodes what to_png encoded (the latter rides the Compress axis).
build canvas 'let s = Canvas.Sprite.blank(3, 2, 0xFF336699)
let back = Canvas.Sprite.from_png(s.to_png())
IO.print(back.width() * 10 + back.height())'
expect_class canvas "$png_choke" ".+" "expected defined" "Canvas named"
expect_class canvas "$ttf_choke" ".+" "expected defined" "Canvas named"
expect_class canvas "$regex_choke" "W?" "expected 'W' or absent" "Canvas only"
expect_class canvas "$search_choke" "W?" "expected 'W' or absent" "Canvas only"
expect_absent canvas "$fmt_machinery" "libstdc++'s formatter, Canvas"
expect_output canvas "32"

# The wrap fixture: naming __Foreign force-loads its archive, and the
# registrar has to have run for the namespace to resolve at all.
build foreign 'let c = __Foreign.Counter.new(10)
c.add(5)
IO.print(c.value())'
expect_present foreign "$foreign_choke" "__Foreign named"
expect_class foreign "$regex_choke" "W?" "expected 'W' or absent" "__Foreign only"
expect_class foreign "$search_choke" "W?" "expected 'W' or absent" "__Foreign only"
expect_absent foreign "$fmt_machinery" "libstdc++'s formatter, __Foreign"
expect_output foreign "15"

# CodeGen.Module: naming it force-loads its archive the same way.
build codegen 'let m = CodeGen.Module.new()
let a = m.literal(v: 40, line: 1, col: 1)
let b = m.literal(v: 2, line: 1, col: 1)
let sum = m.binary(op: "add", lhs: a, rhs: b, line: 1, col: 1)
let args = m.list_new()
m.list_push(args, sum)
let stmts = m.list_new()
m.list_push(stmts, m.intrinsic(name: "print", args_list: args, line: 1, col: 1))
m.add_func(name: "main", num_locals: 0, num_captures: 0, num_cells: 0, num_params: 0,
          body: m.block(stmts_list: stmts, line: 1, col: 1))
m.verify()
m.run()'
expect_present codegen "$codegen_choke" "CodeGen named"
expect_class codegen "$regex_choke" "W?" "expected 'W' or absent" "CodeGen only"
expect_class codegen "$search_choke" "W?" "expected 'W' or absent" "CodeGen only"
expect_class codegen "$peg_choke" "" "expected absent" "CodeGen only"
expect_absent codegen "$fmt_machinery" "libstdc++'s formatter, CodeGen"
expect_output codegen "42"

# Http, both halves of its axis. The default links OpenSSL; --no-tls links the
# other archive and must carry not one OpenSSL symbol. The second probe also
# runs, because "no OpenSSL" is only half the contract: an https URL has to
# come back as an ordinary HttpError naming the flag, rather than as httplib's
# own std::invalid_argument escaping as a crash. Neither probe touches the
# network — the refusal happens before any connect.
build http 'IO.print(Http.get("http://127.0.0.1:1/").status)'
expect_present http "$openssl_syms" "Http named, TLS is the default"
expect_class http "$peg_choke" "" "expected absent" "Http only"

build http_notls 'try {
  Http.get("https://127.0.0.1:1/")
  IO.print("no-refusal")
} catch e {
  IO.print("refused")
}' --no-tls
expect_absent http_notls "$openssl_syms" "OpenSSL under --no-tls"
expect_absent http_notls "$tensor_kernels" "cpp-tensorlib's elementwise kernels"
expect_output http_notls "refused"

# Tensor: `+=` and `+` both have to reach the real kernels, and the printed
# value is what proves they did — the core archive's stub throws rather than
# computing, so a binary that force-loaded nothing would die here instead of
# quietly answering wrong.
build tensor 'mut a = Tensor.from([[1.0, 2.0]])
a += Tensor.from([[10.0, 20.0]])
let b = a + Tensor.from([[100.0, 200.0]])
IO.print(b.to_array()[0][1])'
expect_present tensor "$tensor_kernels" "Tensor named, the kernels must link"
expect_class tensor "$regex_choke" "W?" "expected 'W' or absent" "Tensor only"
expect_class tensor "$search_choke" "W?" "expected 'W' or absent" "Tensor only"
expect_class tensor "$peg_choke" "" "expected absent" "Tensor only"
expect_absent tensor "$fmt_machinery" "libstdc++'s formatter, Tensor"
expect_output tensor "222.0"

# A Shared.new view: its reader arrives through the hook, and the view's
# `copy` reaches the deserializer, which reaches everything else.
build shared 'let s = Shared.new({a: 1, xs: [10, 20]})
IO.print(s.a + s.xs[1])'
expect_present shared 'culebra::_jit_shared_val_prop_impl[(]' "Shared named"
expect_present shared 'culebra::jit_serialize[(]' "Shared named"
expect_class shared "$regex_choke" "W?" "expected 'W' or absent" "Shared only"
expect_class shared "$search_choke" "W?" "expected 'W' or absent" "Shared only"
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
  translation unit (see the comment in include/stdlib/regex.h).
  Search 'W' (or nothing) where 'T' was expected, or a `searchlib::` symbol
  in `none`: the same two causes as Regex, one file over — the kFeatureAxes
  row and _rt_embed_files, or something bypassing CULEBRA_RT_SEARCH_WEAK (see
  include/stdlib/search.h).
  Proc / Canvas / PEG: a choke present in `none`, or absent where it was
  named: something outside the choke reaches the engine unconditionally, or
  the adapter isn't reachable only through its kNsRows_* table.
  libstdc++'s formatter in a probe: a header the runtime archive compiles
  called std::format (or std::print/println) — put the message on
  culebra::format instead, see include/base/format.h. Missing from `spec`:
  the control stopped working, so fix the probe (or fmt_machinery) before
  trusting the rest.
  A namespace group or adapter in a binary that never names it: something in
  the core archive refers to the group (only the program object may), or an
  adapter is reachable outside its kNsRows_* table.
EOF
  exit 1
fi
echo "aot-feature-axes OK (Regex / Search / Tensor / Http+TLS / __Foreign / CodeGen by axis; Proc/Canvas/PEG/Shared by namespace group; PEG's fixed RTTI residue accepted; groups linked only when named; no libstdc++ formatter)"
