# GC leak check

Detects reference-count leaks in the JIT — codegen paths that forget to emit a
release, so an object keeps a non-zero refcount and is never freed by RC alone.
Such leaks are invisible under normal runs because the conservative mark-sweep
backstop reclaims the garbage anyway (by reachability, ignoring refcounts); they
only show up as extra GC work and higher memory. This tool surfaces them.

## How it works

Each program is collected two ways and the live-object count compared:

- **conservative** (default): marks from real reachability (stack + globals).
  Frees everything truly unreachable regardless of refcount, so a leak's
  garbage is still reclaimed — the live count stays flat.
- **`CULEBRA_GC_REFS=1`**: seeds collection purely from reference counts
  (CPython's `gc_refs`: refcount minus internal references → external roots).
  It trusts the refcount, so a leaked object (refcount stuck above its real
  reference count) survives — the live count balloons with the loop.

A pattern whose `gc_refs` live count far exceeds its conservative count has an
RC leak in the operation it exercises. `CULEBRA_GC_REFS_DIAG=1` additionally
prints, per collect, the leaked objects broken down by type tag.

## Usage

```sh
# Run the built-in pattern battery (one isolated operation per row):
tools/analysis/gc_leak_check.sh

# Audit a single program (must end by printing `... live=<N>` via GC.stat()):
tools/analysis/gc_leak_check.sh path/to/program.cul

# Pick the binary / loop size / sensitivity:
CULEBRA=./build/culebra N=100000 THRESHOLD=4 tools/analysis/gc_leak_check.sh
```

Exit status is non-zero when any pattern leaks, so it doubles as a regression
gate. `gc_leak_patterns.cul` is the battery — add a branch there to cover a new
operation.

## Caveat

`CULEBRA_GC_REFS` is an experimental, env-gated collector used here only as a
measurement probe; it is **not** the shipped GC and trusts the refcounts (so it
is not itself leak-proof — that is exactly what makes it a useful leak detector).
The default build is unaffected.
