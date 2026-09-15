#!/usr/bin/env bash
# Documented parameter names are the names a call can bind.
#
# Every stdlib function accepts its parameters by keyword and `**` splat, so a
# parameter's name is API: `Canvas.rect(**shape)` works only when the binder's
# names are the ones docs/stdlib.md prints. The binder reads them from two
# places the docs know nothing about -- canon_sigs_table.h for a native, the
# `fn (...)` list for a preamble function -- and both had drifted (`Canvas.rect`
# took `a, b, c, d, e`; `IO.println` takes `arg` where the docs say `x`).
#
# This gate asks the binary: each `Ns.fn(...)` row of tools/checks/api_surface.txt
# (generated from the reference docs) is resolved to its Function value and its
# `fn.params` -- what the binder itself binds -- is compared with the row. The
# documented names must be the leading params in order; params the row leaves
# out must all have defaults (Http's `into:`, documented in prose). A function
# with no nameable params (`Math.max(*args)`) is skipped, and a function the
# docs list under several rows passes when any row matches.
#
# Usage: tools/checks/check_param_names.sh [culebra-binary]
set -euo pipefail
cd "$(dirname "$0")/../.."

CULEBRA="${1:-./build-dev/culebra}"
[ -x "$CULEBRA" ] || { echo "param-names: $CULEBRA not found/executable" >&2; exit 1; }

# 221 documented functions have nameable params (the rest take none, or only
# `*args`); far fewer means the surface list or the resolution broke, and a
# gate comparing nothing would pass.
MIN_COMPARED=200

# Namespaces a CMake option takes away (tools/checks/check_tests_optional_ns.sh
# keeps the authoritative table). Their rows run in a file of their own, so a
# binary built without one skips that file instead of failing on its name.
OPTIONAL='Scene Webview Desktop'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/culebra-param-names.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" "$OPTIONAL" <<'PY'
import collections
import re
import sys

work, optional = sys.argv[1], set(sys.argv[2].split())
ROW = re.compile(r'^([A-Z][A-Za-z0-9]*)\.([a-z_][A-Za-z0-9_]*)\((.*)\)(\s*->.*)?$')
# A receiver-less row is a bare global (`to_string(v)`) or a method listed
# under its type's heading (`size()`); only the table's bare globals resolve
# as names, and an unresolvable name would stop the whole file from compiling.
BARE_ROW = re.compile(r'^([a-z_][A-Za-z0-9_]*)\((.*)\)(\s*->.*)?$')
IDENT = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)')

bare_globals = set()
in_bare = False
for line in open('include/stdlib/canon_sigs_table.h'):
    if 'kCanonParams_Bare[]' in line:
        in_bare = True
    elif in_bare and line.startswith('};'):
        break
    elif in_bare:
        m = re.match(r'\s*// \d+: ([a-z_][A-Za-z0-9_]*)$', line)
        if m and not m.group(1).startswith('_'):
            bare_globals.add(m.group(1))
if len(bare_globals) < 10:
    sys.exit(f'param-names: found only {len(bare_globals)} bare globals in '
             'canon_sigs_table.h -- did kCanonParams_Bare move?')


def split_top(s):
    parts, depth, buf = [], 0, ''
    for ch in s:
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(buf)
            buf = ''
        else:
            buf += ch
    if buf.strip():
        parts.append(buf)
    return [p.strip() for p in parts]


def names(params):
    out = []
    for p in split_top(params):
        # `fn.params` lists neither a bare `*` nor a `*args` collector, and a
        # `**rest` one exempts the function below.
        if p == '...' or p.startswith('*'):
            continue
        m = IDENT.match(p)
        if not m:
            return None  # not a signature this gate can read
        out.append(m.group(1))
    return out


rows = collections.OrderedDict()
for line in open('tools/checks/api_surface.txt'):
    line = line.strip()
    if m := ROW.match(line):
        ns, name, params = m.group(1), f'{m.group(1)}.{m.group(2)}', m.group(3)
    elif (m := BARE_ROW.match(line)) and m.group(1) in bare_globals:
        ns, name, params = '', m.group(1), m.group(2)
    else:
        continue
    ps = names(params)
    if ps is None:
        continue
    rows.setdefault((ns, name), []).append(ps)

CHECK = '''
let mut compared = 0
let bad = []
for row in rows {
  let f = try {
    row[2]()
  } catch e {
    println("SKIP {row[0]}: resolving it threw {e.kind}")
    nil
  }
  if f == nil {
    continue
  }
  if type_of(f) != 'Function' {
    println("SKIP {row[0]}: a {type_of(f)}, not a Function")
    continue
  }
  let ps = f.params
  if ps.size() == 0 || ps.any(|p| p.kwargs_rest) {
    println("SKIP {row[0]}: no nameable params")
    continue
  }
  # `fn.params` leaves a `*args` collector out, so all-keyword-only params
  # (`range(*args, step:)`) mean the documented positionals are collected.
  if ps.all(|p| p.kw_only) {
    println("SKIP {row[0]}: positionals collected by *args")
    continue
  }
  compared += 1
  let actual = ps.map(|p| p.name)
  let matches = fn (d) {
    d.size() <= actual.size() && actual.slice(0, d.size()) == d &&
      ps.slice(d.size(), ps.size()).all(|p| p.has_default)
  }
  if !row[1].any(matches) {
    let documented = row[1].map(|d| "(" + d.join(", ") + ")").join(" / ")
    bad.push("{row[0]} binds ({actual.join(", ")}), documented {documented}")
  }
}
for b in bad {
  println("MISMATCH {b}")
}
println("compared {compared}")
'''


def emit(path, items):
    with open(path, 'w') as out:
        out.write('# Generated by tools/checks/check_param_names.sh\n')
        out.write('let rows = [\n')
        for (_, name), docs in items:
            lists = ', '.join('[' + ', '.join(repr(n) for n in d) + ']'
                              for d in docs)
            out.write(f"  ['{name}', [{lists}], || {name}],\n")
        out.write(']\n')
        out.write(CHECK)


core = [(k, v) for k, v in rows.items() if k[0] not in optional]
emit(f'{work}/core.cul', core)
for ns in sorted(optional):
    items = [(k, v) for k, v in rows.items() if k[0] == ns]
    if items:
        emit(f'{work}/optional_{ns}.cul', items)
PY

compared=0
fail=0
run_file() {
  local file=$1 out rc=0
  out=$(CULEBRA_CANVAS_HEADLESS=1 "$CULEBRA" --vm "$file" 2>&1) || rc=$?
  if [[ $rc -ne 0 ]]; then
    local ns=${file##*/optional_}
    ns=${ns%.cul}
    if [[ $file == */optional_* ]] && grep -q "NameError" <<<"$out"; then
      echo "param-names: $ns not in this binary, skipped"
      return
    fi
    echo "param-names FAIL: $file did not run (exit $rc):" >&2
    echo "$out" | tail -20 >&2
    fail=1
    return
  fi
  [[ -n "${PARAM_NAMES_VERBOSE:-}" ]] && { grep '^SKIP ' <<<"$out" || true; }
  if grep -q '^MISMATCH ' <<<"$out"; then
    grep '^MISMATCH ' <<<"$out" | sed 's/^MISMATCH /param-names FAIL: /' >&2
    fail=1
  fi
  compared=$((compared + $(sed -n 's/^compared //p' <<<"$out")))
}

run_file "$WORK/core.cul"
for f in "$WORK"/optional_*.cul; do
  [[ -e $f ]] && run_file "$f"
done

if (( compared < MIN_COMPARED )); then
  echo "param-names FAIL: compared only $compared functions (floor $MIN_COMPARED)" \
       "-- did api_surface.txt or the resolution break?" >&2
  fail=1
fi
(( fail == 0 )) && echo "param-names: $compared documented functions bind their documented names"
exit $fail
