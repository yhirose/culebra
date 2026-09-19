# GC leak check

Detects reference-count leaks in the JIT — codegen paths that forget to emit a
release, so an object keeps a non-zero refcount and is never freed by RC alone.
Such leaks are invisible under normal runs because the conservative mark-sweep
backstop reclaims the garbage anyway (by reachability, ignoring refcounts); they
only show up as extra GC work and higher memory. This tool surfaces them.

## How it works

Each program runs once under the collector's quiescent-point audit
(`CULEBRA_GC_LEAK_ABORT=1`, `docs/internals/memory.md` §5.2) with the
collector otherwise off (`CULEBRA_GC_NEVER=1`, so no background collection
reclaims the leaked garbage before the audit sees it). After the top-level
program returns, the audit classifies every object the conservative scan
finds unreachable: one whose refcount still exceeds the references the heap
holds to it carries a phantom +1 — a definite missed release — and the
process aborts naming the object's allocation site. A reference cycle the
program built on purpose has no phantom count and is not reported, so the
detector has no false positives.

## Usage

```sh
# Run the built-in pattern battery (one isolated operation per row):
tools/analysis/gc_leak_check.sh

# Audit a single program:
tools/analysis/gc_leak_check.sh path/to/program.cul

# Pick the binary / loop size:
CULEBRA=./build-gate/culebra N=100000 tools/analysis/gc_leak_check.sh
```

Exit status is non-zero when any pattern leaks, so it doubles as a regression
gate. `gc_leak_patterns.cul` is the battery — add a branch there to cover a new
operation.

## Caveat

Pass a binary built without LTO (`build-dev/` or `build-gate/`): the audit
rides the conservative scan's completeness, and LTO's altered stack layout
aliases leaked objects as live and under-reports.
