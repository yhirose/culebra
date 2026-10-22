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


# A quote right after a digit or a letter is a digit separator (1'000), not
# the start of a character literal.
_LEXEME = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"'
                     r"|(?<!\w)'(?:\\.|[^'\\\n])*'", re.S)


def blank(src):
    """`src` with comments and string/char literals turned into spaces, so a
    brace or a name inside one is not code. Lengths and newlines are kept, so
    offsets and line numbers still point into `src`."""
    return _LEXEME.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src)


def _close(code, at, open_ch, close_ch):
    """The index just past the bracket that closes the one at `at`."""
    depth = 0
    for i in range(at, len(code)):
        if code[i] == open_ch:
            depth += 1
        elif code[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(code)


def function_spans(code, name):
    """[(first line, last line, parameter text, body offset, body end)] for
    each definition of the function `name` in `code` (a blank()ed source), at
    any indentation: a declaration or a call is not one, since what follows
    its parameter list is not a body."""
    head = re.compile(r'^[ \t]*(?:[\w:<>*&,]+[ \t]+)*' + re.escape(name) +
                      r'\s*\(', re.M)
    out = []
    for m in head.finditer(code):
        params_end = _close(code, m.end() - 1, '(', ')')
        tail = re.match(r'(?:\s|const\b|noexcept\b|override\b)*\{',
                        code[params_end:])
        if not tail:
            continue
        open_at = params_end + tail.end() - 1
        end = _close(code, open_at, '{', '}')
        out.append((code.count('\n', 0, m.start()) + 1,
                    code.count('\n', 0, end) + 1,
                    code[m.end():params_end - 1], open_at, end))
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
