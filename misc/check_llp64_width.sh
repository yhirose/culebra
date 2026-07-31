#!/usr/bin/env bash
# Ask an LLP64 compiler whether any 64-bit value is landing in a 32-bit type.
#
# Usage: misc/check_llp64_width.sh <cmake build dir with compile_commands.json>
#
# culebra's Long is int64_t. On Windows `long` is 32 bits, so a language value
# that passes through one truncates — and Linux, where `long` IS int64_t,
# cannot produce the diagnostic at all. Two classes hide there:
#
#   * type identity — `Value(some_long)` becomes ambiguous, `get<long>` throws.
#     A build catches those, but only after ~10 minutes of compiling.
#   * silent narrowing — `long n = v.to_long();` compiles everywhere and is
#     wrong only on Windows. NOTHING catches that except -Wconversion.
#
# So: -fsyntax-only (no codegen, no link) over the two TUs that between them
# include every header, with -Wconversion filtered to the one message that
# means the width was lost. The command comes from compile_commands.json, so
# this check cannot drift from the real build.
#
# The harness proves ITSELF before it proves anything else: a three-line probe
# that must draw the narrowing diagnostic wherever long is 32-bit. A broken
# invocation (wrong compiler path, mangled flags) then fails the job instead
# of passing it — which is exactly how the first version of this script
# "passed" in 0.1 s without compiling a thing.
set -euo pipefail
cd "$(dirname "$0")/.."

build=${1:?usage: check_llp64_width.sh <build dir>}
echo "LLP64 width check: $build"
db="$build/compile_commands.json"
[ -f "$db" ] || { echo "no $db — configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2; exit 1; }

# `|| true` matters: under set -e a failing command substitution in an
# assignment exits SILENTLY before the guard below can say why.
PY=$(command -v python3 || command -v python || true)
[ -n "$PY" ] || { echo "no python for reading compile_commands.json" >&2; exit 1; }

# Every repo TU the configure produced — derived from the db, not hand-listed,
# so coverage cannot silently shrink when a header moves. Feature TUs appear
# exactly when their option is ON in the job's configure.
mapfile -t tus < <("$PY" -c '
import json, sys
for e in json.load(open(sys.argv[1])):
    f = e["file"].replace("\\", "/")
    i = f.find("/src/")
    if i != -1:
        print(f[i + 1:])
' "$db" | tr -d '\r' | sort -u)
(( ${#tus[@]} >= 2 )) || { echo "unexpectedly few TUs: ${tus[*]}" >&2; exit 1; }
echo "TUs: ${tus[*]}" 

# The one diagnostic that means a 64-bit value went into a 32-bit type; the
# source may print as itself or as a typedef with an {aka}. ONE regex for the
# self-test AND the analysis, so they cannot disagree.
HIT_RE="conversion from ('long long int'|'[^']+' \{aka 'long long int'\}) to 'long int'"

log=$(mktemp)
probe=$(mktemp --suffix=.cc)
trap 'rm -f "$log" "$probe"' EXIT

any_error=0
for tu in "${tus[@]}"; do
  # directory + raw command, verbatim. No shlex round trip: POSIX shlex eats
  # the backslashes in Windows paths, which is how the invocation silently
  # became a no-op on the runner this check exists for.
  entry=$("$PY" -c '
import json, sys
db, tu = sys.argv[1], sys.argv[2]
for e in json.load(open(db)):
    if e["file"].replace("\\", "/").endswith(tu):
        print(e["directory"])
        print(e.get("command") or " ".join(e["arguments"]))
        break
else:
    sys.exit("no compile_commands entry for " + tu)
' "$db" "$tu")
  entry=${entry//$'\r'/}  # Windows python emits CRLF; a trailing \r poisons every path
  dir=${entry%%$'\n'*}
  cmd=${entry#*$'\n'}
  read -r compiler _ <<<"$cmd"
  command -v "$compiler" >/dev/null || {
    echo "LLP64 width FAIL: compiler '$compiler' from compile_commands not found" >&2
    echo "  raw command: $cmd" >&2
    exit 1
  }
  # Drop the object output; keeping -c is harmless under -fsyntax-only.
  cmd=$(sed -E 's/ -o +[^ ]+//' <<<"$cmd")
  echo "=== syntax-check $tu ==="
  set +e
  (cd "$dir" && eval "$cmd" -fsyntax-only -Wconversion -Wno-error) >>"$log" 2>&1
  rc=$?
  set -e
  echo "    compiler exit $rc"
  if (( rc != 0 )); then
    any_error=1
    echo "    --- last lines ---"
    tail -5 "$log" | sed 's/^/    /'
  fi
done

# Self-test: on an LLP64 target this probe MUST draw the exact diagnostic the
# analysis below greps for. If it does not, the harness — not the code — is
# broken, and "OK" would be the 0.1-second lie all over again.
printf '#include <cstdint>\nint64_t g();\nlong f() { return g(); }\n' > "$probe"
if "$compiler" -fsyntax-only -x c++ -std=c++23 - \
     <<<'static_assert(sizeof(long) == 4, "");' 2>/dev/null; then
  if ! "$compiler" -fsyntax-only -std=c++23 -Wconversion "$probe" 2>&1 \
       | grep -qE "$HIT_RE"; then
    echo "LLP64 width FAIL: harness self-test — the probe drew no narrowing" >&2
    echo "  diagnostic on a 32-bit-long target. Fix the harness, not the code." >&2
    exit 1
  fi
else
  echo "note: LP64 host (long is 64-bit) — narrowing cannot occur here; this run"
  echo "      only proves the harness plumbing. The CI job is the real check."
fi

# The one diagnostic that means a 64-bit value was put into a 32-bit type.
# Everything else -Wconversion reports (size_t -> int, int -> char, …) is
# out of scope here and stays quiet.
hits=$(grep -E "$HIT_RE" "$log" || true)
if [ -n "$hits" ]; then
  echo "LLP64 width FAIL: a 64-bit value is being narrowed to 32 bits" >&2
  echo "$hits" | head -20 >&2
  echo "  (only lines matching the 64->32 rule are listed)" >&2
  echo "  These are culebra Longs. Use int64_t." >&2
  exit 1
fi

# Type-identity errors (ambiguous Value(long), get<long>) surface as errors.
if grep -qE "^[^[:space:]]+:[0-9]+:[0-9]+: (error|fatal error):" "$log"; then
  echo "LLP64 width FAIL: the headers do not compile for an LLP64 target" >&2
  grep -E "^[^[:space:]]+:[0-9]+:[0-9]+: (error|fatal error):" "$log" | head -20 >&2
  exit 1
fi

# A nonzero compiler exit the patterns above cannot explain is a broken
# harness (driver error, missing response file, mangled path) — never OK.
if (( any_error )); then
  echo "LLP64 width FAIL: compiler failed for a reason no pattern explains" >&2
  tail -10 "$log" >&2
  exit 1
fi

echo "LLP64 width OK: no 64-bit value narrowed to 32 bits"
