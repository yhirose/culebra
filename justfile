set shell := ["bash", "-cu"]

# Background-friendly scheduling priority for local builds/tests. Several
# worktree sessions building/testing at once would otherwise each claim every
# core, degrading interactive Mac use; `nice` only yields under contention —
# a solo run still gets full CPU, since there's nothing to yield to.
# Override with CULEBRA_NICE (0 = no-op, matches un-niced behavior; CI leaves
# it at the default since nothing else competes for cores there either).
nice_cmd := "nice -n " + env_var_or_default("CULEBRA_NICE", "10")

# Serialize the heavy lanes machine-wide. `nice` above shares the cores kindly
# but does not stop two full gates from thrashing the same 15 GB box; this puts
# them in a queue, which finishes the same work sooner and keeps only one
# session blocked. Wraps the -O3 build, the gate build and the gate itself —
# never `dev`/`test-dev`, which must stay responsive while a gate runs.
# CULEBRA_GATE_LOCK=0 opts out (see misc/one_at_a_time.sh).
lock_cmd := justfile_directory() / "misc/one_at_a_time.sh"

# Share ccache entries between worktrees of the same commit: every -I CMake
# generates is an absolute path into this checkout, so without a base_dir each
# new worktree pays a full cold build. Only sound while no cacheable TU bakes in
# an absolute path — see include/source_dir.h.
export CCACHE_BASEDIR := justfile_directory()

# Keep Canvas headless under every recipe. A window-enabled build (the default
# where a window works) must not open one here: the gate compares framebuffer
# bytes (PPM md5), and no test should steal focus mid-run. Run a game with its
# window by invoking the binary directly, or with CULEBRA_CANVAS_HEADLESS=0.
export CULEBRA_CANVAS_HEADLESS := env_var_or_default("CULEBRA_CANVAS_HEADLESS", "1")

# List recipes
default:
    @just --list

# Configure and build with LLVM JIT enabled (Release + LTO by default).
# Extra cmake flags can be passed positionally — CI uses this to wire
# ccache, contributors can pass `-DCULEBRA_LTO=OFF` to skip LTO link.
[group("build")]
build *extra:
    mkdir -p build
    cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON {{extra}} .. > /dev/null
    # CULEBRA_BUILD_JOBS overrides the parallel job count (defaults to all
    # cores). CI sets it to cap RAM on the memory-tight macOS runner — an
    # LLVM-header-heavy TU peaks at ~3 GB, so too many at once swap.
    cd build && {{lock_cmd}} {{nice_cmd}} make -j${CULEBRA_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}

# Fast dev build: LTO off and -O1, still Release + JIT,
# uses a separate `build-dev/` so it doesn't fight `just build`'s cache.
# CULEBRA_DEV_NO_RT skips the four AOT runtime archives (each a full
# culebra_rt.cc recompile), so a header touch rebuilds one TU instead of
# five. `culebra build` (AOT) is disabled here — use `just build` for it.
# Build 2:20 -> 1:33 vs the -O3 gate build, with an unchanged test sweep;
# benchmark with `just build`, never with build-dev/ (see CULEBRA_DEV_O1).
# Pair with ccache (auto-detected by CMake) for near-instant rebuilds
# when only ephemeral mtimes changed.
[group("build")]
dev *extra:
    mkdir -p build-dev
    cd build-dev && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON -DCULEBRA_LTO=OFF -DCULEBRA_DEV_NO_RT=ON -DCULEBRA_DEV_O1=ON {{extra}} .. > /dev/null
    cd build-dev && {{nice_cmd}} make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8) culebra

# Gate build for `just test`: Release + JIT like `just build`, but LTO OFF.
# LTO is a pure link-time optimization — every test phase's output is identical
# with or without it — so the gate skips it and keeps its own ccache: the LTO
# define flips every key, so build-gate/ holds the warm no-LTO half `just build`
# would otherwise keep evicting. Unlike `just dev` it keeps the AOT runtime
# archives (no DEV_NO_RT) and builds the whole tree (the embedding-test
# executables included), so the AOT and ctest phases still run. `just build`
# stays LTO for release / the CI LTO-link check.
[group("build")]
build-gate *extra:
    mkdir -p build-gate
    cd build-gate && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON -DCULEBRA_LTO=OFF {{extra}} .. > /dev/null
    cd build-gate && {{lock_cmd}} {{nice_cmd}} make -j${CULEBRA_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}

[private]
_gen-blob-tool:
    mkdir -p build-dev
    {{ if path_exists("/opt/homebrew/opt/llvm/bin/clang++") == "true" { "/opt/homebrew/opt/llvm/bin/clang++" } else { env_var_or_default("CXX", "c++") } }} \
        -std=c++23 -O2 -I include -I vendor/cpp-peglib \
        tools/gen_grammar_blob.cc -o build-dev/gen_grammar_blob

# Regenerate include/grammar_blob.h — the serialized grammar that lets
# get_parser() skip peglib's ~10 ms meta-parse on startup. Run after editing the
# grammar (grammar_def.h) or bumping vendor/cpp-peglib (the blob layout is
# peglib-version-specific). Skipping it still runs correctly — get_parser()
# falls back to load_grammar() on hash mismatch — but costs that ~10 ms on every
# startup, silently. `just check-blob` is the gate that catches it.
[group("build")]
gen-blob: _gen-blob-tool
    ./build-dev/gen_grammar_blob include/grammar_blob.h

# Verify include/grammar_blob.h is in sync with the grammar (CI gate).
[group("build")]
check-blob: _gen-blob-tool
    ./build-dev/gen_grammar_blob --check include/grammar_blob.h

# Build without JIT (interpreter only, no LLVM). Builds just the `culebra`
# driver — the only TU with CULEBRA_JIT_ENABLED #ifdef gating, so this is the
# compile gate that catches interp-only build breakage (the rest of the tree is
# backend-agnostic). Uses its own build-no-jit/ dir so it doesn't clobber the
# JIT `build/`'s cmake cache (the JIT define flips every ccache key anyway).
[group("build")]
build-no-jit:
    mkdir -p build-no-jit
    cd build-no-jit && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=OFF .. > /dev/null
    cd build-no-jit && {{nice_cmd}} make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8) culebra

# Same Release + JIT shape as `just dev`, minus -DNDEBUG, so the tree's asserts
# actually execute. Every other lane is Release, so without this one NO build
# ever runs an assert — jit_slab.h's cross-allocator free guard sat dead while
# exactly that corruption shipped. -O1 keeps rebuilds in inner-loop range; its
# own build-assert/ dir keeps the NDEBUG flag flip out of the other lanes'
# ccache keys, which also means this lane is always ccache-cold on first use —
# hence the CULEBRA_BUILD_JOBS cap, since it doesn't take the machine lock.
[group("build")]
build-assert *extra:
    mkdir -p build-assert
    cd build-assert && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON -DCULEBRA_LTO=OFF -DCULEBRA_DEV_NO_RT=ON -DCMAKE_CXX_FLAGS_RELEASE="-O1" {{extra}} .. > /dev/null
    cd build-assert && {{nice_cmd}} make -j${CULEBRA_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)} culebra

# Install the Release binary (`just build`) into PREFIX/bin, with sudo only when
# that directory isn't already writable. PREFIX defaults to /usr/local: on macOS
# /usr/bin is read-only even for root (SIP), so a system-wide install goes here.
# Pass it positionally for a user-local one: `just install ~/.local`.
# The AOT runtime archives are embedded in the binary, so it needs nothing else
# to run scripts or to `culebra build`. Only `Embed.dir` and `culebra wrap` read
# a source checkout — from the path baked in at build time, or $CULEBRA_HOME
# (include/source_dir.h) — so keep this tree, or point CULEBRA_HOME at one.
[doc("Install the Release binary into PREFIX/bin (default /usr/local, sudo only if needed).")]
[group("build")]
install PREFIX='/usr/local': build
    #!/usr/bin/env bash
    set -euo pipefail
    dest="{{PREFIX}}/bin"
    # Writability of the nearest existing ancestor: PREFIX itself may not exist
    # yet (`PREFIX=~/.local` on a fresh account creates two levels).
    probe="$dest"
    while [[ ! -e "$probe" ]]; do probe="$(dirname "$probe")"; done
    sudo=""
    [[ -w "$probe" ]] || sudo=sudo
    $sudo mkdir -p "$dest"
    # -s: the same ~9 MB of global symbols the release archive drops
    # (misc/package_release.sh), for the same reason.
    $sudo install -m 755 -s build/culebra "$dest/culebra"
    echo "installed -> $dest/culebra"
    case ":${PATH}:" in *":$dest:"*) ;;
      *) echo "note: $dest is not on your PATH" >&2 ;;
    esac

# Build with ASan+UBSan (no-LTO Release) and smoke the JIT GC paths.
# The conservative stack scanner (scan_range, jit_gc.h) is exempted from
# ASan via no_sanitize("address"); without that, every JIT GC collect
# aborts with a stack-buffer-underflow. GC_STRESS=1 forces a collect on
# every allocation, so the feature files below hammer scan_range. Any
# sanitizer diagnostic fails the recipe.
[group("test")]
asan:
    #!/usr/bin/env bash
    set -euo pipefail
    NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
    mkdir -p build-asan
    ( cd build-asan && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON \
        -DCULEBRA_LTO=OFF -DCULEBRA_DEV_NO_RT=ON \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" .. > /dev/null \
      && make -j"$NCPU" culebra )
    bin=build-asan/culebra
    fail=0
    for f in tests/test_forin_codegen.cul tests/test_value_equality.cul \
             tests/test_generator_complex.cul; do
        out=$(ASAN_OPTIONS=detect_leaks=0 CULEBRA_GC_STRESS=1 "$bin" --jit "$f" 2>&1) \
            || { echo "asan: run failed: $f" >&2; fail=1; }
        if grep -qiE "ERROR: AddressSanitizer|runtime error:|-buffer-|use-after" <<<"$out"; then
            echo "asan: sanitizer diagnostic in $f:" >&2
            grep -iE "ERROR|runtime error|buffer|use-after" <<<"$out" | head -5 >&2
            fail=1
        fi
    done
    [[ "$fail" == 0 ]] || { echo "asan FAIL" >&2; exit 1; }
    echo "asan OK (JIT GC paths clean under ASan+UBSan)"

# Clean build directories + local editor/cache scratch (all regenerable)
[group("build")]
clean:
    rm -rf build build-dev build-gate build-asan build-assert build-no-jit
    rm -rf .cache-ccache .zed .vscode .vimspector.json misc/*/.zed

# Regenerate misc/culebra.peg + the Vim/VSCode AUTO-KEYWORDS from include/parser.h
[group("build")]
sync-grammar:
    misc/sync_grammar.sh

# Verify misc/culebra.peg + the Vim/VSCode AUTO-KEYWORDS are in sync (CI gate)
[group("build")]
check-grammar-sync:
    misc/sync_grammar.sh --check

# Regenerate include/stdlib_preambles.gen.h from src/preambles/*.cul
[group("build")]
gen-preambles:
    misc/gen_preambles.sh

# Verify stdlib_preambles.gen.h is in sync with src/preambles/*.cul (CI gate)
[group("build")]
check-preambles:
    misc/gen_preambles.sh --check

# Run the test suite. BACKEND selects what to run:
#   all     (default) — interp vs JIT diff + AOT vs JIT diff + C++
#                       embedding smoke. Run before every commit.
#   interp            — every tests/*.cul on the tree-walking interp.
#   jit               — every tests/*.cul on the LLVM ORC JIT.
#   aot               — every tests/*.cul through `culebra build`,
#                       assert stdout matches `--jit`.
#   embed             — C++ ctest (mt_smoke, mi_smoke, define_smoke).
#   wrap              — `culebra wrap` end-to-end (rebuilds the tree).
# The single-backend modes are for focused debugging.
[doc("Full gate (no-LTO gate build). BACKEND=all|fast|interp|jit|aot|embed|isolate|wrap (default: all). JOBS=N controls parallelism (default: CPU cores). CULEBRA_TEST_SKIP_HEAVY=1 skips difftest + gc-stress + AOT (set on the slow macOS CI runner); CULEBRA_TEST_WRAP=1 adds the wrap lane (set on Ubuntu CI).")]
[group("test")]
test BACKEND='all': build-gate
    @BIN=./build-gate/culebra {{lock_cmd}} just _run-tests {{BACKEND}}

# Fast inner-loop tests against the no-LTO build-dev/ binary (`just dev`).
# Runs only the phases that don't need LTO/AOT/embed exes: the interp==JIT
# symmetry sweep + culebra-test self + isolate (BACKEND=fast, the default).
# Run this after each edit; `just test` is the heavier pre-commit gate.
[doc("Fast inner-loop tests vs build-dev/ (no LTO). BACKEND=fast|interp|jit|isolate (default: fast).")]
[group("test")]
test-dev BACKEND='fast': dev
    @BIN=./build-dev/culebra CULEBRA_TEST_SKIP_HEAVY=1 just _run-tests {{BACKEND}}

# The same sweep as test-dev against the assert-enabled binary (`just
# build-assert`). Not part of `just test`; Ubuntu CI runs it as linux-assert.
[doc("Tests vs an assert-enabled build (no NDEBUG). BACKEND=fast|interp|jit|isolate (default: fast).")]
[group("test")]
test-assert BACKEND='fast': build-assert
    @BIN=./build-assert/culebra CULEBRA_TEST_SKIP_HEAVY=1 just _run-tests {{BACKEND}}

# Land BRANCH onto local master: rebase, rebuild + test-dev, then fast-forward
# merge (misc/land.sh), retrying the rebase if master moves again before the
# merge lands. Runs under its own machine-wide lock — a different lock file
# than build/build-gate/test's (CULEBRA_LOCK_PATH), so landing one branch
# doesn't queue behind someone else's unrelated gate, and the lock path lives
# under the shared .git dir rather than TMPDIR so every session's `just land`
# contends for the same file regardless of its own TMPDIR.
[doc("Land BRANCH onto local master: rebase + test-dev + ff-only merge, retried if master moves mid-test")]
[group("land")]
land BRANCH:
    CULEBRA_LOCK_PATH="$(git rev-parse --git-common-dir)/culebra-land.lock" \
    CULEBRA_LOCK_WAIT_MSG="waiting: another culebra session is landing onto master…" \
        {{lock_cmd}} misc/land.sh {{BRANCH}}

[private]
_run-tests BACKEND:
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob

    # Parallelism for per-file test loops. Default = physical cores;
    # tests are CPU-bound (culebra startup + JIT compile), so this
    # scales near-linearly. Set JOBS=1 to recover the old serial
    # behavior when debugging.
    JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

    # Portable per-invocation timeout. A stalled CI runner (GitHub's scarce
    # macOS VMs occasionally freeze a process mid-test) should fail the phase
    # fast — naming the offending file — instead of hanging the job for 30 min
    # until it's force-cancelled. `cul` wraps every culebra invocation; ctest
    # carries its own --timeout, and the difftest batch is wrapped at its phase.
    # macOS lacks `timeout`; CI brew-installs coreutils for `gtimeout`. If
    # neither exists (a bare local run) we fall through with no limit so dev
    # never breaks on a missing dep.
    TIMEOUT_BIN=""
    if command -v timeout >/dev/null 2>&1; then TIMEOUT_BIN=timeout
    elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT_BIN=gtimeout; fi
    CULEBRA_TEST_TIMEOUT="${CULEBRA_TEST_TIMEOUT:-300}"
    # Which culebra to test. Defaults to the full LTO `just build` output;
    # `test-dev` overrides it to the no-LTO build-dev/ binary for fast inner
    # loops. AOT/embed phases derive their paths from BIN's build dir.
    BIN="${BIN:-./build/culebra}"
    cul() { {{nice_cmd}} ${TIMEOUT_BIN:+$TIMEOUT_BIN "$CULEBRA_TEST_TIMEOUT"} "$BIN" "$@"; }
    export -f cul
    export BIN TIMEOUT_BIN CULEBRA_TEST_TIMEOUT

    # Per-recipe scratch dir for parallel job artifacts (per-file
    # stdout/stderr, .fail markers). Cleaned on exit.
    job_dir="${TMPDIR:-/tmp}/culebra-test-$$"
    rm -rf "$job_dir" && mkdir -p "$job_dir"
    trap 'rm -rf "$job_dir"' EXIT

    # Print stderr for every .fail marker in a job dir; 1 if any exist.
    # Independent of what the items were, so every parallel phase uses it.
    collect_failures() {
        local d="$1" label="$2" fail name
        local fails=("$d"/*.fail)
        (( ${#fails[@]} > 0 )) || return 0
        echo "test $label FAIL: ${#fails[@]} item(s):" >&2
        for fail in "${fails[@]}"; do
            name=$(basename "$fail" .fail)
            echo "--- $name ---" >&2
            [[ -s "$d/$name.err" ]] && cat "$d/$name.err" >&2
        done
        return 1
    }

    # Replay per-file stdout in tests/*.cul order so output is stable
    # regardless of completion order, then report failures.
    collect_results() {
        local d="$1" label="$2"
        for f in tests/*.cul; do
            name=$(basename "$f" .cul)
            [[ -s "$d/$name.out" ]] && cat "$d/$name.out"
        done
        collect_failures "$d" "$label"
    }

    run_interp() {
        local d="$job_dir/interp"
        mkdir -p "$d"
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"
            name=$(basename "$f" .cul)
            if ! cul "$f" > "$d/$name.out" 2> "$d/$name.err"; then
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d"
        collect_results "$d" "interp"
    }

    run_jit() {
        local d="$job_dir/jit"
        mkdir -p "$d"
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"
            name=$(basename "$f" .cul)
            if ! cul --jit "$f" > "$d/$name.out" 2> "$d/$name.err"; then
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d"
        collect_results "$d" "jit"
    }

    # Where this phase parks each file's JIT output for the AOT phase to reuse
    # (see run_aot): the two phases both need `culebra --jit <file>`, and for
    # the slowest file that is 27 s of JIT compile paid twice.
    jit_out="$job_dir/jit-out"

    run_diff_interp_jit() {
        local d="$job_dir/diff"
        mkdir -p "$d" "$jit_out"
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"; jit_out="$3"
            name=$(basename "$f" .cul)
            out_interp=$(cul "$f" 2> "$d/$name.interp.err") || \
                { touch "$d/$name.fail"; echo "interp crashed for $f:" > "$d/$name.err"; \
                  cat "$d/$name.interp.err" >> "$d/$name.err"; exit 0; }
            out_jit=$(cul --jit "$f" 2> "$d/$name.jit.err") || \
                { touch "$d/$name.fail"; echo "jit crashed for $f:" > "$d/$name.err"; \
                  cat "$d/$name.jit.err" >> "$d/$name.err"; exit 0; }
            # Only a run that agreed with interp is worth reusing; a mismatch
            # fails the gate here anyway.
            if [[ "$out_interp" == "$out_jit" ]]; then
                printf "%s" "$out_jit" > "$jit_out/$name.txt"
            fi
            if [[ "$out_interp" != "$out_jit" ]]; then
                {
                    echo "interpreter and JIT outputs differ for $f:"
                    diff <(printf "%s" "$out_interp") <(printf "%s" "$out_jit") || true
                } > "$d/$name.err"
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d" "$jit_out"
        if ! collect_results "$d" "(interp vs jit)"; then
            echo "test (interp vs jit) FAIL" >&2
            exit 1
        fi
        echo "test (interp vs jit) OK"
    }

    # Guard the non-default JIT codegen paths (--jit -O0 = unoptimized IR over
    # the default optimizing backend, --jit-faststart = neither optimizer)
    # against the malformed-Value class of bug that O2 silently legalizes but
    # those paths abort or miscompile on (see
    # tests/test_forin_codegen.cul). Behavior must equal interp on every
    # backend. Cheap: a handful of codegen-sensitive files, not the corpus.
    # Files are chosen for IR shapes that stress the unoptimized backend:
    # for-in tag handling, phi merges (cond/match), destructure, invoke/unwind
    # edges (drop-on-throw), generator CPS, iterator HOF, per-iteration scopes.
    # The second group throws through deep preamble call chains — the shape
    # that hid a backend miscompile from this gate until 2026-07 precisely
    # because the list above never exercised it.
    # One job per (file, backend) pair, heaviest files first: the effects
    # trio dominates the phase, so keeping a heavy file's two backend runs
    # in one job would leave every other lane idle waiting for it.
    codegen_files="tests/test_effects_resume.cul \
        tests/test_effects_defer.cul \
        tests/test_effects.cul \
        tests/test_dynamic_perform.cul \
        tests/test_transform_error_lines.cul \
        tests/test_forin_codegen.cul \
        tests/test_forin_unwind_drop.cul \
        tests/test_destructure_seq_unify.cul \
        tests/test_match_block_arm.cul \
        tests/test_generator_complex.cul \
        tests/test_drop_on_throw.cul \
        tests/test_cond.cul \
        tests/test_while_scope.cul \
        tests/callable_iterator_hof.cul \
        tests/test_path.cul \
        tests/test_regex.cul \
        tests/test_args.cul"
    run_codegen_backends() {
        local d="$job_dir/codegen"
        mkdir -p "$d"
        for flags in "--jit -O0" "--jit-faststart"; do
            printf "%s $flags\n" $codegen_files
        done | xargs -P "$JOBS" -I '{}' bash -c '
            f=${1%% *}; flags=${1#* }; d="$2"
            name=$(basename "$f" .cul).${flags// /}
            # Both sides capture stderr. Comparing a stderr-inclusive run
            # against a stderr-free one makes anything the wrapper writes there
            # (nice reporting it cannot setpriority, say) read as a codegen
            # mismatch on every file; and stderr is half of what interp/JIT
            # symmetry is about, so it belongs in the comparison anyway.
            ref=$(cul "$f" 2>&1) || {
                echo "interp failed: $f" > "$d/$name.err"; touch "$d/$name.fail"; exit 0
            }
            got=$(cul $flags "$f" 2>&1) || {
                echo "FAIL ($flags aborted): $f" > "$d/$name.err"
                touch "$d/$name.fail"; exit 0
            }
            if [[ "$got" != "$ref" ]]; then
                {
                    echo "FAIL ($flags != interp): $f"
                    diff <(printf "%s" "$ref") <(printf "%s" "$got") || true
                } > "$d/$name.err"
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d"
        collect_failures "$d" "(codegen backends)" || exit 1
        echo "test (codegen backends: -O0, fast) OK"
    }

    run_aot() {
        local out_dir="${TMPDIR:-/tmp}/culebra-aot-test"
        rm -rf "$out_dir" && mkdir -p "$out_dir"
        local d="$job_dir/aot"
        mkdir -p "$d"
        # `culebra build` reads $CULEBRA_HOME for the Embed.dir headers (it
        # ignored the env var until 2026-07 while telling the user to set it).
        # The rest of the phase covers the baked-path default; this covers the
        # override, which must fail cleanly rather than in the asset compile.
        local bogus
        if bogus=$(CULEBRA_HOME=/nonexistent-culebra-home \
                cul build tests/test_embed_static.cul -o "$out_dir/home_check" 2>&1); then
            echo "test aot FAIL: build ignored a bogus CULEBRA_HOME" >&2; exit 1
        fi
        case "$bogus" in
            *"set CULEBRA_HOME"*) ;;
            *) echo "test aot FAIL: unclear CULEBRA_HOME error: $bogus" >&2; exit 1 ;;
        esac
        # Every build garbage-collects the runtime-archive cache. It may only
        # delete the fingerprint directories it wrote itself: an earlier version
        # pruned the cache dir's PARENT, so `CULEBRA_CACHE=/tmp/x` swept /tmp.
        # Both roots below hold more entries than the collector keeps, so a
        # regression deletes something here instead of going unnoticed.
        local cdir="$out_dir/cache-safety"
        mkdir -p "$cdir"/sib_{1,2,3,4,5} "$cdir"/root/{deps,notes,a,b,c}
        printf 'IO.inspect(1)\n' > "$cdir/p.cul"
        if ! CULEBRA_CACHE="$cdir/root" cul build "$cdir/p.cul" -o "$cdir/p" \
                > "$d/cache.err" 2>&1; then
            echo "test aot FAIL: build with CULEBRA_CACHE set" >&2
            cat "$d/cache.err" >&2; exit 1
        fi
        for keep in "$cdir"/sib_* "$cdir"/root/{deps,notes,a,b,c}; do
            [[ -d "$keep" ]] && continue
            echo "test aot FAIL: cache prune deleted $keep" >&2; exit 1
        done
        # The one axis whose link no tests/*.cul can reach (it skips itself on a
        # driver built without Webview, which is what the sweep below cannot do).
        {{nice_cmd}} bash misc/probe_webview_aot_link.sh "$BIN" "$out_dir/webview" \
            || exit 1
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"; out_dir="$3"; jit_out="$4"
            name=$(basename "$f" .cul)
            bin="$out_dir/$name"
            if ! cul build "$f" -o "$bin" 2> "$d/$name.build.err"; then
                {
                    echo "build failed: $f"
                    cat "$d/$name.build.err"
                } > "$d/$name.err"
                touch "$d/$name.fail"
                exit 0
            fi
            out_aot=$(${TIMEOUT_BIN:+$TIMEOUT_BIN "$CULEBRA_TEST_TIMEOUT"} "$bin")
            # The symmetry phase already ran this file under the JIT and kept
            # the output; recompiling it here costs 27 s on the worst file for
            # a byte-identical result. Standalone `just test aot` has no such
            # file and falls back to running it.
            if [[ -f "$jit_out/$name.txt" ]]; then
                out_jit=$(cat "$jit_out/$name.txt")
            else
                out_jit=$(cul --jit "$f")
            fi
            if [[ "$out_aot" != "$out_jit" ]]; then
                {
                    echo "AOT and JIT outputs differ for $f:"
                    diff <(printf "%s" "$out_aot") <(printf "%s" "$out_jit") || true
                } > "$d/$name.err"
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d" "$out_dir" "$jit_out"
        if ! collect_results "$d" "aot"; then
            echo "test aot FAIL" >&2
            exit 1
        fi
        # The sweep above runs every binary with no arguments, so this is the
        # one place an AOT build is asked what it does with some.
        {{nice_cmd}} bash tests/sys_argv_test.sh "$BIN" --aot || exit 1
        echo "test aot OK: AOT binaries match --jit"
    }

    run_embed() {
        # -j: the ctest targets are independent processes, so run them in
        # parallel like the per-file phases. The suite's wall-clock is otherwise
        # the serial sum, dominated by the two slowest scripts (signal_test,
        # fmt_test) — parallelism hides everything behind the longest single test.
        # --timeout: bound each test so a stalled CI runner (GitHub's scarce
        # macOS VMs occasionally freeze a process) fails fast instead of hanging
        # the job for hours. 300 s is ~8x the slowest legit test (signal_test).
        (cd "$(dirname "$BIN")" && {{nice_cmd}} ctest --output-on-failure --timeout 300 -j "$JOBS")
    }

    # Exercises `culebra test`-only ambient bindings (matchers, DI,
    # @parametrize). The subdir layout keeps these out of the
    # `tests/*.cul` glob that direct interp/JIT runs use.
    run_culebra_test_self() {
        cul test tests/culebra_test_self/ > /dev/null
        # Sanity-check the JSON reporter: final line is a run_end with
        # failed=0 and errored_files=0; every other line begins with
        # one of the documented event tags.
        local last
        last=$(cul test --reporter json tests/culebra_test_self/ | tail -1)
        case "$last" in
            *'"event":"run_end"'*'"failed":0'*'"errored_files":0'*) ;;
            *) echo "json reporter: bad run_end line: $last" >&2; exit 1 ;;
        esac
        # The runner must catch a raw user `throw {...}` from inside a
        # test body and surface it as a structured test_fail (kind and
        # message lifted from the thrown Object). Exit code is non-zero
        # because the test fails by design — what matters is that the
        # runner itself doesn't crash.
        local throw_out
        throw_out=$(cul test --reporter json \
            tests/culebra_test_throw_self/ 2>&1) || true
        case "$throw_out" in
            *'"event":"test_fail"'*'"kind":"RawThrowKind"'*'"event":"run_end"'*) ;;
            *) echo "runner did not catch raw user throw:" >&2;
               echo "$throw_out" >&2; exit 1 ;;
        esac
    }

    # Isolate tests live in a subdir kept out of the `tests/*.cul` interp-vs-JIT
    # diff glob. All run under interp; `*_jit.cul` additionally run under --jit.
    # Isolate.spawn, Channel, and Parallel are all symmetric across backends now;
    # the interp-only files (no `_jit` suffix) cover surface that doesn't apply
    # under --jit (e.g. the runtime mut-capture SendError and the `limit:` kwarg).
    # The `overcap-*` modes force the isolate cap to 1 so every spawned producer
    # takes the over-cap path, which must still run on a real thread —
    # inline-over-cap deadlocks a streaming producer (it fills a bounded channel
    # with no consumer yet), the intermittent macOS-CI hang on the 3-core runner.
    # Capped at 4 lanes, not $JOBS: these spawn real OS threads per case and the
    # guard is about scheduler pressure, so 20 lanes add flake risk for ~1 s.
    run_isolate() {
        local d="$job_dir/isolate"
        mkdir -p "$d"
        {
            printf 'interp %s\n' tests/isolate/*.cul
            printf 'jit %s\n' tests/isolate/*_jit.cul
            printf 'overcap-interp %s\n' tests/isolate/test_spawn_overcap*.cul
            printf 'overcap-jit %s\n' tests/isolate/test_spawn_overcap*.cul
        } | xargs -P "$(( JOBS < 4 ? JOBS : 4 ))" -I '{}' bash -c '
            mode=${1%% *}; f=${1#* }; d="$2"
            name=$(basename "$f" .cul).$mode
            [[ $mode == *jit ]] && jit=--jit || jit=
            [[ $mode == overcap-* ]] && export CULEBRA_ISOLATE_LIMIT=1
            cul $jit "$f" > /dev/null 2> "$d/$name.err" || {
                echo "test isolate FAIL (or timed out) [$mode]: $f" >> "$d/$name.err"
                touch "$d/$name.fail"
            }
        ' _ '{}' "$d"
        collect_failures "$d" "isolate" || exit 1
        echo "test isolate OK (interp + jit symmetry)"
    }

    # Differential corpus: generate the template-combinator programs and
    # diff interp vs JIT byte-for-byte (tools/difftest). Complements the
    # per-file run_diff_interp_jit above with systematic seam coverage.
    # run.sh exits non-zero on any divergence, which aborts `test`.
    # `culebra wrap` end-to-end: extended binary from the examples/wrap
    # declaration, interp==jit==AOT on the wrapped class. Rebuilds the
    # tree into ~/.cache/culebra-wrap (ccache-incremental when available)
    # — the toolchain-sensitive Phase 4 lane, exercised on both CI OSes
    # unless SKIP_HEAVY (the slow macOS runner skips it like the other
    # heavy phases).
    run_wrap_test() {
      ${TIMEOUT_BIN:+$TIMEOUT_BIN 1800} tests/wrap_test.sh "$BIN" || exit 1
    }
    run_difftest() {
        # A batch of 5114 generated cases, not a single invocation, so it gets
        # its own generous wall-clock bound (well above the honest runtime) —
        # only a genuine hang trips it.
        ${TIMEOUT_BIN:+$TIMEOUT_BIN 1800} tools/difftest/run.sh "$BIN"
    }
    # Leak-fuzz: rerun the same corpus under CULEBRA_GC_NEVER and fail on any
    # JIT RC leak not already in tools/difftest/leak_baseline.txt. A regression
    # gate for the ownership work — catches a new carve-out leak the moment a
    # codegen change introduces one, which the backstop would otherwise mask.
    run_leak_fuzz() {
        ${TIMEOUT_BIN:+$TIMEOUT_BIN 1800} tools/difftest/leak.sh "$BIN"
    }
    # GAP5 detector smoke test (cheap): the loud inflated-RC audit
    # (CULEBRA_GC_LEAK_ABORT=1) must fire with a birth site on a real acyclic
    # leak and stay quiet on clean code and benign cycles. Guards the detector
    # itself, distinct from the growth-based corpus gate above.
    run_leak_abort() {
        LEAKFUZZ_WORK="$job_dir/leakabort" tools/difftest/leak_abort.sh "$BIN"
    }
    # Suite-wide GAP5 gate: run the WHOLE corpus under the loud inflated-RC audit
    # and fail on any acyclic RC leak not in tools/difftest/leak_abort_allow.txt.
    # This is the throw-path-aware complement to run_leak_fuzz: the growth gate's
    # `_p` drops any case that throws, so it never measures throw-path leaks; the
    # audit here runs every case (result discarded) and fires whether or not the
    # thunk threw. Heavy (per-case fallback on aborting chunks), so SKIP_HEAVY.
    run_leak_abort_suite() {
        ${TIMEOUT_BIN:+$TIMEOUT_BIN 1800} \
            env LEAKFUZZ_WORK="$job_dir/leakabort-suite" \
            tools/difftest/leak_abort_suite.sh "$BIN"
    }

    # Run every test file under the JIT with collect-on-every-allocation
    # (CULEBRA_GC_STRESS=1) and assert none crash. This is the phase that would
    # have caught the namespace/HOF dispatch use-after-frees: a transient GC
    # value held only in a std::vector/map buffer (unscanned by the conservative
    # collector) is swept mid-construction. A normal run collects far too rarely
    # to hit the window; only stress mode makes it deterministic. JIT-only — the
    # interp's shared_ptr values can't be swept out from under a C++ local, so it
    # has no equivalent window.
    run_gc_stress() {
        local d="$job_dir/gcstress"
        mkdir -p "$d"
        # Only crash/no-crash matters here (correctness is covered by the
        # interp-vs-JIT diff), so discard stdout and keep stderr for triage.
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"
            name=$(basename "$f" .cul)
            if ! CULEBRA_GC_STRESS=1 cul --jit "$f" > /dev/null 2> "$d/$name.err"; then
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d"
        local fails=("$d"/*.fail)
        if (( ${#fails[@]} > 0 )); then
            echo "test (jit gc-stress) FAIL: ${#fails[@]} file(s) crashed:" >&2
            for fail in "${fails[@]}"; do
                name=$(basename "$fail" .fail)
                echo "--- $name.cul ---" >&2
                [[ -s "$d/$name.err" ]] && tail -3 "$d/$name.err" >&2
            done
            exit 1
        fi
        echo "test (jit gc-stress) OK"
    }

    # RC-leak gate: run the differential leak battery (tools/analysis). Each
    # pattern is collected two ways — conservative (real reachability) vs
    # CULEBRA_GC_REFS (refcount-seeded) — and a gap means a codegen path
    # leaked a release. The conservative backstop hides such leaks at runtime,
    # so without this gate an RC leak ships silently; here it fails the build.
    # This is the Level-2 safety net for the ownership flip: releases that
    # can't be structurally guaranteed by the Owned handle (loop bodies) are
    # still caught if they regress. N is kept modest — the leak signal is a
    # ratio, so a few thousand iterations separate flat from leaking cleanly.
    run_leak_battery() {
        if CULEBRA="$BIN" N=5000 THRESHOLD=4 bash tools/analysis/gc_leak_check.sh; then
            echo "test (rc-leak battery) OK"
        else
            echo "test (rc-leak battery) FAIL: an RC leak regressed" >&2
            exit 1
        fi
    }

    # RC-discipline ratchet (GAP3/GAP4-lite): source-level ceilings on the
    # hand-placed retain/release forms, so migration debt can only shrink and
    # a new bare RC op fails the gate.
    run_rc_discipline() { bash tools/check_rc_discipline.sh; }
    # Long-width ratchet: a language value that passes through a C++ `long`
    # is 32-bit on Windows, and Linux cannot see it (there long IS int64_t).
    run_long_width() { bash tools/check_long_width.sh; }
    run_flow_discipline() { bash tools/check_flow_discipline.sh; }

    # Dispatch-tag symmetry gate: the AST tag sets handled by the interp
    # _eval_dispatch and the JIT compile() switches must stay equal, so a
    # grammar node added to one walker but not the other fails here instead
    # of becoming a one-backend bug (tools/check_dispatch_symmetry.sh).
    run_dispatch_symmetry() { bash tools/check_dispatch_symmetry.sh; }

    # Iterator wiring ratchet: terminals drive through JitIterDrive (which
    # owns dispose-on-every-exit) and lazy combinators forward their
    # upstream(s) to the wrapper factory (tools/check_iter_wiring.sh).
    run_iter_wiring() { bash tools/check_iter_wiring.sh; }

    # Runtime-archive TLS ownership: a dynamically-initialized namespace-scope
    # thread_local must be defined by the core archive or by a force-loaded
    # feature archive, never both — mingw's ld rejects the duplicate TLS init
    # and the Windows AOT link fails (tools/check_rt_archive_tls.sh). Needs the
    # built archives, so this is a gate-only phase (`test-dev` has none).
    run_rt_archive_tls() {
        bash tools/check_rt_archive_tls.sh "$(dirname "$BIN")"
    }

    # Webview dynamic-load gate (Linux): the engine is dlopen'd at window
    # creation, so neither the driver nor an AOT binary may carry it in
    # DT_NEEDED or export the forwarders (tools/check_webview_dynload.sh).
    # Reads the linked output and links an AOT binary, so gate-only too.
    run_webview_dynload() {
        bash tools/check_webview_dynload.sh "$(dirname "$BIN")"
    }

    # Announce each phase with the running elapsed time, so a slow/stalled CI
    # run shows where it is (otherwise the silent phases — difftest, the
    # interp/jit sweep — emit nothing until they finish).
    phase() { echo ">>> [${SECONDS}s] $1"; }
    case "{{BACKEND}}" in
      # Order: cheap tests first, then AOT (slowest + most env-sensitive,
      # so a failure there shouldn't mask matcher regressions).
      # CULEBRA_TEST_SKIP_HEAVY skips the platform-independent heavy phases
      # (the 5114-case generated difftest, the JIT gc-stress sweep, and the
      # per-test AOT links). CI sets it on the slow macOS runner — those run on
      # Linux CI and in local dev.
      all)
        phase "rc-discipline (bare retain/release ratchet)"; run_rc_discipline
        phase "long width (language values are int64_t, not long)"; run_long_width
        phase "flow-discipline (return-completion ratchet)"; run_flow_discipline
        phase "dispatch symmetry (eval_X vs compile_X tag sets)"; run_dispatch_symmetry
        phase "iter wiring (JitIterDrive + upstream forwarding ratchet)"; run_iter_wiring
        phase "rt-archive TLS ownership (core vs force-loaded features)"; run_rt_archive_tls
        phase "webview dynload (engine stays behind dlopen)"; run_webview_dynload
        phase "interp/jit symmetry (real test files)"; run_diff_interp_jit
        phase "codegen backends (-O0, fast vs interp)"; run_codegen_backends
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "difftest (5114 generated cases)"; run_difftest; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "leak-fuzz (corpus RC-leak regression)"; run_leak_fuzz; }
        phase "leak-abort (GAP5 loud detector smoke)"; run_leak_abort
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "leak-abort-suite (corpus inflated-RC, throw-paths)"; run_leak_abort_suite; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "jit gc-stress (collect every alloc)"; run_gc_stress; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "rc-leak battery (gc_refs vs conservative)"; run_leak_battery; }
        phase "ctest (embedding smokes)"; run_embed
        phase "culebra-test self"; run_culebra_test_self
        phase "isolate (interp + jit)"; run_isolate
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "AOT (== jit)"; run_aot; }
        # Opt-in rather than skip-by-flag: wrap rebuilds the whole tree, which
        # doubled this gate, and only a wrap/CMake/AOT change can break it.
        # Ubuntu CI sets CULEBRA_TEST_WRAP=1; locally use `just test wrap`.
        [[ -z "${CULEBRA_TEST_WRAP:-}" ]] || { phase "wrap (extended binary, 3 backends)"; run_wrap_test; }
        phase "done"; echo "test OK"
        ;;
      # Inner-loop core: the interp==JIT correctness invariant plus the two
      # cheap symmetric suites. No difftest/AOT/embed, so it runs against the
      # no-LTO build-dev/ binary too (see `test-dev`). This is the green-light
      # check after a single edit; `all` is the pre-commit gate.
      fast)
        phase "rc-discipline (bare retain/release ratchet)"; run_rc_discipline
        phase "long width (language values are int64_t, not long)"; run_long_width
        phase "flow-discipline (return-completion ratchet)"; run_flow_discipline
        phase "dispatch symmetry (eval_X vs compile_X tag sets)"; run_dispatch_symmetry
        phase "iter wiring (JitIterDrive + upstream forwarding ratchet)"; run_iter_wiring
        phase "interp/jit symmetry (real test files)"; run_diff_interp_jit
        phase "culebra-test self"; run_culebra_test_self
        phase "isolate (interp + jit)"; run_isolate
        phase "done"; echo "test OK (fast)"
        ;;
      interp) run_interp; run_isolate ;;
      jit)    run_jit ;;
      aot)    run_aot ;;
      embed)  run_embed ;;
      isolate) run_isolate ;;
      wrap)   run_wrap_test ;;
      *) echo "test: unknown backend '{{BACKEND}}' (expected: all|fast|interp|jit|aot|embed|isolate|wrap)" >&2; exit 2 ;;
    esac

# Run the doctest examples in the public docs (interp). Both en and ja
# are run — their code blocks are mostly shared but a few string literals
# are localized, so ja needs its own pass. Not part of `just test` — run
# on demand / before publishing docs. The self-test fixture under
# tests/doctest/ guards the runner itself.
[doc("Run ` ```culebra ` doctest blocks in docs/ (interp)")]
[group("test")]
doctest: build
    ./build/culebra test --doc tests/doctest docs

# Rewrite the signature index inside docs/llm.md and docs/llm.ja.md from
# language.md and stdlib.md, so the context pack never becomes a second
# copy of the reference that can drift. `check-llm-context` is the CI gate.
[doc("Regenerate the signature index in docs/llm.md (en+ja)")]
[group("docs")]
gen-llm-context: build
    ./build/culebra misc/gen_llm_context.cul

[doc("Fail if docs/llm.md's signature index is stale")]
[group("docs")]
check-llm-context: build
    ./build/culebra misc/gen_llm_context.cul --check

# Differential test: generate the template-combinator corpus (tools/difftest)
# and assert interp == jit byte-for-byte over every case. Enumerates the full
# current interp/JIT divergence population in one run; exits non-zero on any
# asymmetry. AOT is covered transitively (`just test` asserts aot == jit).
[doc("Differential interp-vs-JIT test over the generated corpus")]
[group("test")]
difftest: build
    tools/difftest/run.sh ./build/culebra

# Leak-fuzz gate: reuse the difftest corpus (each case is a re-runnable thunk)
# as an RC-leak oracle — run every case under CULEBRA_GC_NEVER (backstop off)
# and flag cases whose JIT live-object growth exceeds the interpreter's. Fails
# on any NEW leak vs tools/difftest/leak_baseline.txt (a regression gate; the
# JIT still has a known carve-out leak set the ownership work is closing).
# Regenerate the baseline with `just leak-fuzz-update` after fixing leaks.
[doc("Leak-fuzz gate: new JIT RC leaks vs baseline (tools/difftest/leak.sh)")]
[group("test")]
leak-fuzz: build
    tools/difftest/leak.sh ./build/culebra

# GAP5 loud leak detector. Run a script under CULEBRA_GC_LEAK_ABORT=1: at the
# teardown quiescent point the JIT audits for inflated-RC leaks and, if any
# survive, aborts with each leaked object's allocation birth site (backtrace).
# Best paired with CULEBRA_GC_NEVER=1 so the backstop doesn't reclaim the
# residue first. Use this to localize a leak the corpus gate (leak-fuzz)
# reports. With no FILE, runs the 3-case detector smoke test.
#
# Uses the no-LTO build-dev/ binary on purpose: the audit rides the conservative
# scan's completeness (best-effort), and LTO's altered stack layout lets the
# teardown scan alias leaked objects as live, so it under-reports. GAP5 is a
# debug/CI (no-LTO) tool — the gate runs it against the no-LTO build-gate too.
[doc("Run a script under the GAP5 loud inflated-RC leak detector (birth-site abort)")]
[group("test")]
leak-abort FILE='': dev
    #!/usr/bin/env bash
    if [ -z '{{FILE}}' ]; then
        tools/difftest/leak_abort.sh ./build-dev/culebra
    else
        CULEBRA_GC_NEVER=1 CULEBRA_GC_LEAK_ABORT=1 ./build-dev/culebra --jit '{{FILE}}'
    fi

[doc("Regenerate the leak-fuzz baseline (tools/difftest/leak_baseline.txt)")]
[group("test")]
leak-fuzz-update: build
    tools/difftest/leak.sh ./build/culebra --update-baseline

# Suite-wide GAP5 gate: run the whole difftest corpus under the loud inflated-RC
# audit (CULEBRA_GC_LEAK_ABORT=1) and fail on any acyclic RC leak whose case
# label is not in tools/difftest/leak_abort_allow.txt. Unlike leak-fuzz (growth-
# based, skips throwing cases), this measures the throw-path leaks too. Uses the
# no-LTO build-gate/ binary (LTO under-reports the conservative-scan audit).
[doc("Suite-wide GAP5 gate: corpus inflated-RC leaks vs allowlist (throw-paths)")]
[group("test")]
leak-abort-suite: build-gate
    tools/difftest/leak_abort_suite.sh ./build-gate/culebra

[doc("Regenerate the leak-abort allowlist (tools/difftest/leak_abort_allow.txt)")]
[group("test")]
leak-abort-suite-update: build-gate
    tools/difftest/leak_abort_suite.sh ./build-gate/culebra --update-allowlist

# Microbenchmark regression check: every tests/perf/*.cul on interp
# and JIT, asserts speedup meets the per-bench `# perf: min_speedup N`
# directive declared in the file header. Not part of `just test`
# because runtimes are noisy and machine-dependent.
[doc("Microbench regression check (per-bench thresholds in tests/perf/*.cul)")]
[group("test")]
perf: build
    ./tests/perf/run.sh

# Smoke: run microgpt 5 training steps (no inference) on both backends
# to catch regressions in the JIT value-ownership / special-method
# dispatch paths that the unit tests don't exercise at scale.
[doc("Run microgpt 5 training steps on both backends (large-scale JIT smoke)")]
[group("bench")]
smoke-microgpt: build fetch-names
    ./build/culebra       benchmarks/microgpt/microgpt.cul 5 0 > /dev/null
    ./build/culebra --jit benchmarks/microgpt/microgpt.cul 5 0 > /dev/null
    @echo "smoke-microgpt OK: 5 steps completed on both backends"

# Opens a real window, so it is out of `just test` (and out of tests/*.cul,
# which that sweeps) — but the window backend is the one thing CI cannot fully
# answer, so it needs a command to run by hand. Same script the CI jobs call.
[doc("Drive the Webview event loop on both backends (opens a window)")]
[group("test")]
smoke-webview: build
    misc/probe_webview_window.sh ./build/culebra

# Download Karpathy's names dataset for benchmarks/microgpt.
[group("bench")]
fetch-names:
    #!/usr/bin/env bash
    set -euo pipefail
    path=benchmarks/microgpt/names.txt
    if [[ -s "$path" ]]; then
      echo "$path already present ($(wc -l < "$path") lines)"
      exit 0
    fi
    mkdir -p benchmarks/microgpt
    url='https://raw.githubusercontent.com/karpathy/makemore/988aa59/names.txt'
    echo "fetching $url"
    curl -fsSL "$url" -o "$path"
    echo "saved $path ($(wc -l < "$path") lines)"

# Download MNIST IDX files for benchmarks/mnist.
[group("bench")]
fetch-mnist:
    #!/usr/bin/env bash
    set -euo pipefail
    out=benchmarks/mnist/data
    mkdir -p "$out"
    primary='https://storage.googleapis.com/cvdf-datasets/mnist'
    fallback='https://ossci-datasets.s3.amazonaws.com/mnist'
    for f in train-images-idx3-ubyte.gz train-labels-idx1-ubyte.gz \
             t10k-images-idx3-ubyte.gz  t10k-labels-idx1-ubyte.gz; do
      if [[ -s "$out/$f" ]]; then
        echo "$out/$f already present"
        continue
      fi
      echo "fetching $f"
      curl -fsSL "$primary/$f" -o "$out/$f" \
        || curl -fsSL "$fallback/$f" -o "$out/$f"
    done
    echo "MNIST data ready in $out"

# Check vendor/* submodules for upstream updates (dry-run by default).
# Pass `--run` to actually check out the latest commit for outdated ones
# (working tree only — review + rebuild + test before committing).
[group("vendor")]
vendor-update *extra:
    ./tools/vendor_update.sh {{extra}}

# Build the browser playground (interp-only wasm via emscripten) into
# site/playground/. Artifacts are committed — .github/workflows/pages.yml
# uploads site/ to Pages as-is, so this is the only place they are produced.
# Needs emsdk (default ~/Projects/emsdk, override with EMSDK_DIR).
[group("site")]
[doc("Build the browser playground (wasm) into site/playground/")]
site-build:
    ./playground/build.sh

[group("site")]
[doc("Build the playground, then serve site/ locally")]
site-serve port="8000": site-build
    python3 -m http.server {{port}} -d site

# The committed page under site/ is what GitHub Pages serves, and build.sh
# stamps its version from include/culebra.h. Regenerating it needs emsdk, so a
# version bump can land with the served page still naming the old version and
# nothing else would notice — the wasm is unaffected, so no build breaks. This
# compares the two directly and needs no toolchain (CI gate).
[group("site")]
[doc("Verify site/playground/index.html names the current version")]
check-site-version:
    #!/usr/bin/env bash
    set -euo pipefail
    want=$(sed -n 's/^#define CULEBRA_VERSION "\([^"]*\)"/\1/p' include/culebra.h)
    [ -n "$want" ] || { echo "no CULEBRA_VERSION in include/culebra.h" >&2; exit 1; }
    got=$(sed -n 's|.*<title>culebra Playground v\([^<]*\)</title>.*|\1|p' \
          site/playground/index.html)
    if [ "$want" != "$got" ]; then
        echo "site/playground/index.html says v${got:-<none>}, but include/culebra.h says v$want" >&2
        echo "run \`just site-build\` and commit site/playground/index.html" >&2
        exit 1
    fi
    echo "site version OK (v$want)"
