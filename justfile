set shell := ["bash", "-cu"]

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
    cd build && make -j${CULEBRA_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)}

# Fast dev build: LTO off (saves ~15-25 s link), still Release + JIT,
# uses a separate `build-dev/` so it doesn't fight `just build`'s cache.
# CULEBRA_DEV_NO_RT skips the four AOT runtime archives (each a full
# culebra_rt.cc recompile), so a header touch rebuilds one TU instead of
# five. `culebra build` (AOT) is disabled here — use `just build` for it.
# Pair with ccache (auto-detected by CMake) for near-instant rebuilds
# when only ephemeral mtimes changed.
[group("build")]
dev *extra:
    mkdir -p build-dev
    cd build-dev && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON -DCULEBRA_LTO=OFF -DCULEBRA_DEV_NO_RT=ON {{extra}} .. > /dev/null
    cd build-dev && make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8) culebra

# Regenerate include/grammar_blob.h — the serialized grammar that lets
# get_parser() skip peglib's ~10 ms meta-parse on startup. Run after editing the
# grammar (grammar_def.h) or bumping vendor/cpp-peglib (the blob layout is
# peglib-version-specific). Skipping it is safe: get_parser() guards the blob
# with a grammar hash and falls back to load_grammar() on mismatch.
[group("build")]
gen-blob:
    mkdir -p build-dev
    {{ if path_exists("/opt/homebrew/opt/llvm/bin/clang++") == "true" { "/opt/homebrew/opt/llvm/bin/clang++" } else { env_var_or_default("CXX", "c++") } }} \
        -std=c++23 -O2 -I include -I vendor/cpp-peglib \
        tools/gen_grammar_blob.cc -o build-dev/gen_grammar_blob
    ./build-dev/gen_grammar_blob include/grammar_blob.h

# Build without JIT (interpreter only, no LLVM). Builds just the `culebra`
# driver — the only TU with CULEBRA_JIT_ENABLED #ifdef gating, so this is the
# compile gate that catches interp-only build breakage (the rest of the tree is
# backend-agnostic). Uses its own build-no-jit/ dir so it doesn't clobber the
# JIT `build/`'s cmake cache (the JIT define flips every ccache key anyway).
[group("build")]
build-no-jit:
    mkdir -p build-no-jit
    cd build-no-jit && cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=OFF .. > /dev/null
    cd build-no-jit && make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8) culebra

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
    rm -rf build build-dev build-asan build-no-jit
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
# The single-backend modes are for focused debugging.
[doc("Full gate (LTO build). BACKEND=all|fast|interp|jit|aot|embed|isolate (default: all). JOBS=N controls parallelism (default: CPU cores). CULEBRA_TEST_SKIP_HEAVY=1 skips difftest + gc-stress + AOT (set on the slow macOS CI runner).")]
[group("test")]
test BACKEND='all': build
    @just _run-tests {{BACKEND}}

# Fast inner-loop tests against the no-LTO build-dev/ binary (`just dev`).
# Runs only the phases that don't need LTO/AOT/embed exes: the interp==JIT
# symmetry sweep + culebra-test self + isolate (BACKEND=fast, the default).
# Run this after each edit; `just test` is the heavier pre-commit gate.
[doc("Fast inner-loop tests vs build-dev/ (no LTO). BACKEND=fast|interp|jit|isolate (default: fast).")]
[group("test")]
test-dev BACKEND='fast': dev
    @BIN=./build-dev/culebra CULEBRA_TEST_SKIP_HEAVY=1 just _run-tests {{BACKEND}}

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
    cul() { ${TIMEOUT_BIN:+$TIMEOUT_BIN "$CULEBRA_TEST_TIMEOUT"} "$BIN" "$@"; }
    export -f cul
    export BIN TIMEOUT_BIN CULEBRA_TEST_TIMEOUT

    # Per-recipe scratch dir for parallel job artifacts (per-file
    # stdout/stderr, .fail markers). Cleaned on exit.
    job_dir="${TMPDIR:-/tmp}/culebra-test-$$"
    rm -rf "$job_dir" && mkdir -p "$job_dir"
    trap 'rm -rf "$job_dir"' EXIT

    # Replay per-file stdout in tests/*.cul order so output is stable
    # regardless of completion order. Returns 0 if no .fail markers
    # exist, otherwise prints stderr for each failed file and returns 1.
    collect_results() {
        local d="$1" label="$2"
        for f in tests/*.cul; do
            name=$(basename "$f" .cul)
            [[ -s "$d/$name.out" ]] && cat "$d/$name.out"
        done
        local fails=("$d"/*.fail)
        if (( ${#fails[@]} > 0 )); then
            echo "test $label FAIL: ${#fails[@]} file(s):" >&2
            for fail in "${fails[@]}"; do
                name=$(basename "$fail" .fail)
                echo "--- $name.cul ---" >&2
                [[ -s "$d/$name.err" ]] && cat "$d/$name.err" >&2
            done
            return 1
        fi
        return 0
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

    run_diff_interp_jit() {
        local d="$job_dir/diff"
        mkdir -p "$d"
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"
            name=$(basename "$f" .cul)
            out_interp=$(cul "$f" 2> "$d/$name.interp.err") || \
                { touch "$d/$name.fail"; echo "interp crashed for $f:" > "$d/$name.err"; \
                  cat "$d/$name.interp.err" >> "$d/$name.err"; exit 0; }
            out_jit=$(cul --jit "$f" 2> "$d/$name.jit.err") || \
                { touch "$d/$name.fail"; echo "jit crashed for $f:" > "$d/$name.err"; \
                  cat "$d/$name.jit.err" >> "$d/$name.err"; exit 0; }
            if [[ "$out_interp" != "$out_jit" ]]; then
                {
                    echo "interpreter and JIT outputs differ for $f:"
                    diff <(printf "%s" "$out_interp") <(printf "%s" "$out_jit") || true
                } > "$d/$name.err"
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d"
        if ! collect_results "$d" "(interp vs jit)"; then
            echo "test (interp vs jit) FAIL" >&2
            exit 1
        fi
        echo "test (interp vs jit) OK"
    }

    # Guard the non-default JIT codegen backends (--jit -O0 = SDAG at O0,
    # --jit-faststart = FastISel) against the malformed-Value class of bug that
    # O2 silently legalizes but those paths abort or miscompile on (see
    # tests/test_forin_codegen.cul). Behavior must equal interp on every
    # backend. Cheap: a handful of codegen-sensitive files, not the corpus.
    run_codegen_backends() {
        local fail=0
        for f in tests/test_forin_codegen.cul; do
            local ref; ref=$(cul "$f") || { echo "interp failed: $f" >&2; fail=1; continue; }
            for flags in "--jit -O0" "--jit-faststart"; do
                local got
                if ! got=$(cul $flags "$f" 2>&1); then
                    echo "FAIL ($flags aborted): $f" >&2; fail=1; continue
                fi
                if [[ "$got" != "$ref" ]]; then
                    echo "FAIL ($flags != interp): $f" >&2
                    diff <(printf "%s" "$ref") <(printf "%s" "$got") >&2 || true
                    fail=1
                fi
            done
        done
        [[ "$fail" == 0 ]] || { echo "test (codegen backends) FAIL" >&2; exit 1; }
        echo "test (codegen backends: -O0, fast) OK"
    }

    run_aot() {
        local out_dir="${TMPDIR:-/tmp}/culebra-aot-test"
        rm -rf "$out_dir" && mkdir -p "$out_dir"
        local d="$job_dir/aot"
        mkdir -p "$d"
        printf '%s\n' tests/*.cul | xargs -n1 -P "$JOBS" -I '{}' bash -c '
            f="$1"; d="$2"; out_dir="$3"
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
            out_jit=$(cul --jit "$f")
            if [[ "$out_aot" != "$out_jit" ]]; then
                {
                    echo "AOT and JIT outputs differ for $f:"
                    diff <(printf "%s" "$out_aot") <(printf "%s" "$out_jit") || true
                } > "$d/$name.err"
                touch "$d/$name.fail"
            fi
        ' _ '{}' "$d" "$out_dir"
        if ! collect_results "$d" "aot"; then
            echo "test aot FAIL" >&2
            exit 1
        fi
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
        (cd "$(dirname "$BIN")" && ctest --output-on-failure --timeout 300 -j "$JOBS")
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
    run_isolate() {
        for f in tests/isolate/*.cul; do
            cul "$f" > /dev/null || { echo "test isolate FAIL (or timed out): $f" >&2; exit 1; }
        done
        for f in tests/isolate/*_jit.cul; do
            cul --jit "$f" > /dev/null || { echo "test isolate FAIL (or timed out): --jit $f" >&2; exit 1; }
        done
        # Over-cap regression guard (both backends): force the isolate cap to 1 so
        # every spawned producer takes the over-cap path, which must still run on
        # a real thread. Inline-over-cap would deadlock a streaming producer (it
        # fills a bounded channel with no consumer yet) — the cause of an
        # intermittent macOS-CI hang on the 3-core runner. `cul`'s timeout makes a
        # regression fail fast instead of hanging.
        for f in tests/isolate/test_spawn_overcap*.cul; do
            CULEBRA_ISOLATE_LIMIT=1 cul "$f" > /dev/null || { echo "test isolate FAIL (over-cap interp): $f" >&2; exit 1; }
            CULEBRA_ISOLATE_LIMIT=1 cul --jit "$f" > /dev/null || { echo "test isolate FAIL (over-cap jit): $f" >&2; exit 1; }
        done
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
        phase "interp/jit symmetry (real test files)"; run_diff_interp_jit
        phase "codegen backends (-O0, fast vs interp)"; run_codegen_backends
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "difftest (5114 generated cases)"; run_difftest; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "jit gc-stress (collect every alloc)"; run_gc_stress; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "rc-leak battery (gc_refs vs conservative)"; run_leak_battery; }
        phase "ctest (embedding smokes)"; run_embed
        phase "culebra-test self"; run_culebra_test_self
        phase "isolate (interp + jit)"; run_isolate
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "AOT (== jit)"; run_aot; }
        [[ -n "${CULEBRA_TEST_SKIP_HEAVY:-}" ]] || { phase "wrap (extended binary, 3 backends)"; run_wrap_test; }
        phase "done"; echo "test OK"
        ;;
      # Inner-loop core: the interp==JIT correctness invariant plus the two
      # cheap symmetric suites. No difftest/AOT/embed, so it runs against the
      # no-LTO build-dev/ binary too (see `test-dev`). This is the green-light
      # check after a single edit; `all` is the pre-commit gate.
      fast)
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
      *) echo "test: unknown backend '{{BACKEND}}' (expected: all|fast|interp|jit|aot|embed|isolate)" >&2; exit 2 ;;
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

# Differential test: generate the template-combinator corpus (tools/difftest)
# and assert interp == jit byte-for-byte over every case. Enumerates the full
# current interp/JIT divergence population in one run; exits non-zero on any
# asymmetry. AOT is covered transitively (`just test` asserts aot == jit).
[doc("Differential interp-vs-JIT test over the generated corpus")]
[group("test")]
difftest: build
    tools/difftest/run.sh ./build/culebra

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
