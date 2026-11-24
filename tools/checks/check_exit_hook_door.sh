#!/usr/bin/env bash
# Exit-hook door gate: no code of ours registers a std::atexit handler, as
# Sys.exit runs none (an exit hook instead, include/base/exit_hooks.h).
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'exit-hook-door'
ATEXIT = re.compile(r'\b(?:at_quick_exit|atexit|__cxa_atexit)\s*\(')
match = lambda stmt: ATEXIT.search(stmt)
door_gate.require_scanner(LABEL, match, 'x;\n  std::atexit(\n      fn);')

text = door_gate.sources()
bad = [(path, n, s) for path in sorted(text)
       for n, s in door_gate.sites(text[path], match)]
door_gate.report(
    LABEL, bad, [],
    'Arm an exit hook instead (arm_exit_hook, include/base/exit_hooks.h): '
    'Sys.exit runs no atexit handler.',
    text, 'no atexit handler')
PY
