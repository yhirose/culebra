#!/usr/bin/env bash
# Source-anchor door gate.
#
# The generator and effects transforms copy a body's source into text they
# re-parse, so a node's position there says nothing about the user's file.
# What leads back is the record of every copy: user source leaves its buffer
# only as a MappedSource (include/frontend/fragments.h), and reaches emitted
# text only through `anchored`, whose comment names that record. A copy made
# as a plain string instead is silently unmapped — an error inside it reports
# the lowering's column (and, for a line with no marker, its line). Before the
# door, every copy was one, and a body's columns were off by however much the
# rewrite had inserted ahead of them.
#
# So in the two transforms a raw slice — `ast_source_slice`, the effects
# lowerer's `slice` — may be measured but not built into a string, a
# MappedSource's `.text()` may be inspected but not emitted, and a diagnostic
# takes its position from the resolver, not from a node's own column.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'source-anchor-door'
FILES = ('include/frontend/generator_transform.h',
         'include/frontend/effects_transform.h')
RAW = re.compile(r"\b(?:ast_source_slice|slice)\s*\(")
BUILD = re.compile(r'std::string\s*[({]|std::format\s*\(|\+=|[^+]\+[^+=]|'
                   r'\.append\s*\(|make_shared<std::string>')
# `.text()` read, not emitted: followed by a member that only inspects it.
EMITTED_TEXT = re.compile(r'\.text\(\)(?!\s*\.\s*(?:back|find|empty|size)\s*\()')

# A diagnostic positioned by a node's own `column`, which is the fragment's
# when the body was lowered from one; source_pos / err_pos resolve (`.col`).
NODE_POS = re.compile(r'CulebraError\s*\(\s*"(?!InternalError)[^"]*"'
                      r'[\s\S]*(?:->|\.)column\b')

match = lambda stmt: (RAW.search(stmt) and BUILD.search(stmt)) or \
    EMITTED_TEXT.search(stmt) or NODE_POS.search(stmt)
door_gate.require_scanner(
    LABEL, match, 'out += std::string(\n    slice(*st));')
door_gate.require_scanner(LABEL, match, 'out += piece.text();')
door_gate.require_scanner(
    LABEL, match, 'throw CulebraError("SyntaxError", "m",\n'
    '    static_cast<long>(n->line), static_cast<long>(n->column));')

text = door_gate.sources()
bad = [(p, n, s) for p in FILES for n, s in door_gate.sites(text[p], match)]

frag = 'include/frontend/fragments.h'
count = lambda path, s: text[path].count(s)
population = [
    ('anchored defined once, in fragments.h',
     sum(t.count('inline std::string anchored(') for t in text.values()) == 1
     and count(frag, 'inline std::string anchored(') == 1),
    ('the anchor spelled only in fragments.h',
     all(p == frag or 'kSourceAnchor' not in t for p, t in text.items())),
    ('the generator places anchored copies',
     count(FILES[0], 'anchored(') >= 4),
    ('the effects lowering places anchored copies',
     count(FILES[1], 'anchored(') >= 10),
    ('the generator reads anchors back after its final parse',
     count(FILES[0], 'reposition_fragment(') >= 2),  # def + use
    ('the effects lowering reads anchors back after its final parses',
     count(FILES[1], 'reposition_fragment(') >= 2),
    ('reposition_fragment resolves through the anchors',
     re.search(r'reposition_fragment\([^{]*\{[^}]*SourceResolver',
               text[FILES[0]]) is not None),
]

door_gate.report(
    LABEL, bad, population,
    '  User source reaches lowered text only as a MappedSource, placed with\n'
    '  anchored() — build it with node_source / rewrite_edits /\n'
    '  splice_source instead of a string.',
    FILES, 'no unmapped copy of user source')
PY
