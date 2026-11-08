# Sourced, not run: `source misc/gate_jit_cache.sh <fallback dir>` sets
# `jit_cache` to the JIT object cache a gate shares with every run of every
# worktree, and exports its size limit. `_run-tests` and `_doctest` both
# source it, so the place, the limit and the fallback are decided once.
#
# An object is keyed by the module the backend was handed and by that backend
# (docs/internals/vm.md §7), so a later run, of this build or another, compiles
# only what changed. CULEBRA_GATE_JIT_CACHE names another directory, or `off`
# for the fallback, which lasts the run. A directory that cannot be written —
# a sandbox that was not told of it — is said and passed over for the
# fallback, rather than left to fail a write at a time, silently.

jit_cache="${1:?usage: source misc/gate_jit_cache.sh <fallback dir>}"
_kept="${CULEBRA_GATE_JIT_CACHE:-}"
if [[ -z "$_kept" ]]; then
    case "$(uname -s)" in
      Darwin) _kept="${HOME:-}/Library/Caches/culebra/gate-jit" ;;
      *)      _kept="${XDG_CACHE_HOME:-${HOME:-}/.cache}/culebra/gate-jit" ;;
    esac
fi
if [[ "$_kept" != off ]]; then
    if mkdir -p "$_kept" 2>/dev/null && ( : > "$_kept/.probe.$$" ) 2>/dev/null; then
        rm -f "$_kept/.probe.$$"
        jit_cache="$_kept"
    else
        echo "jit cache: cannot write $_kept; this run keeps its own" >&2
    fi
fi
unset _kept
# Several builds' worth: the test corpus is 72 MB of objects, and an entry no
# run has read for longest is the first to go. Every user of the shared
# directory sets the same limit, so none trims it to a smaller one.
export CULEBRA_JIT_CACHE_MAX_MB="${CULEBRA_JIT_CACHE_MAX_MB:-2048}"
