"""The scanner the door gates share.

A door gate says "one question, one place that answers it" and then proves
nobody asks it elsewhere. Two things are the same for every such gate and are
here rather than in each script: the sweep is over whole STATEMENTS, not lines
(these calls wrap, and the first sweep written for one — a grep for the literal
— missed half), and the scanner is checked against a planted call before a
green run is believed.

What differs — the door, what an offending statement looks like, and the
positive checks that the gate is still looking at the real thing — stays in the
gate's own script, with the prose that says why the door exists.
"""

import pathlib
import re
import sys

_COMMENT = re.compile(r'//[^\n]*')
_END = re.compile(r'(?<=[;{}])')


def sources(roots=('include', 'src')):
    """{path: text} over the tree's own C++ files, keyed as written."""
    files = [p for root in roots for p in pathlib.Path(root).rglob('*')
             if p.suffix in ('.h', '.cc') and 'vendor' not in p.parts]
    return {str(p): p.read_text() for p in files}


def sites(src, match):
    """(line, one line of text) for each statement `match` accepts."""
    src = _COMMENT.sub('', src)
    line, out = 1, []
    for stmt in _END.split(src):
        lead = stmt[:len(stmt) - len(stmt.lstrip())]
        here = line + lead.count('\n')  # the statement's own first line
        line += stmt.count('\n')
        if match(stmt):
            out.append((here, ' '.join(stmt.split())[:140]))
    return out


def require_scanner(label, match, planted):
    """The scanner has to be able to fail, or a green run proves nothing."""
    if not sites(planted, match):
        sys.exit(f'{label} FAIL: the scanner no longer sees a wrapped call, '
                 'so a green run proves nothing')


def report(label, bad, population, hint, files, held):
    """Fail with every offending site and every positive check that is off."""
    missing = [what for what, ok in population if not ok]
    for p, n, s in bad:
        print(f'{label} FAIL: {p}:{n}: {s}', file=sys.stderr)
    for what in missing:
        print(f'{label} FAIL: expected {what}', file=sys.stderr)
    if bad or missing:
        print(hint, file=sys.stderr)
        sys.exit(1)
    print(f'{label} OK ({len(files)} files, {held})')
