#!/usr/bin/env python3
"""
Generate sidebyside.html from microgpt.py and microgpt.cul.

Edit SECTIONS below when section boundaries shift, then run:
    python3 samples/microgpt/build_sidebyside.py
"""

import re
from pathlib import Path

DIR = Path(__file__).resolve().parent
PY_PATH = DIR / 'microgpt.py'
CUL_PATH = DIR / 'microgpt.cul'
OUT_PATH = DIR / 'sidebyside.html'


# (section_title, py_range, cul_range). None means "no equivalent" and
# renders the empty-cell placeholder. Ranges are inclusive 1-based line
# numbers in the source files.
SECTIONS = [
    ('Module header &amp; imports',           (1, 13),    (1, 5)),
    ('Seed + data file check',                (14, 21),   (7, 14)),
    ('Load training documents',               (22, 24),   (16, 22)),
    ('Vocabulary (sorted unique chars)',      (26, 30),   (24, 30)),
    ('class Value — fields &amp; dunder ops', (32, 60),   (33, 69)),
    ('Value.backward — topo + grad walk',     (62, 75),   (70, 90)),
    ('sum_v / max_data (Culebra helpers; Python uses sum() / max() builtins)',
                                              None,       (92, 96)),
    ('Hyperparameters',                       (77, 82),   (99, 105)),
    ('matrix factory',                        (83, 83),   (107, 110)),
    ('state / params collection',             (84, 93),   (112, 138)),
    ('linear (with dot helper)',              (95, 98),   (141, 146)),
    ('softmax',                               (100, 104), (148, 153)),
    ('rmsnorm',                               (106, 109), (155, 159)),
    ('add_v (element-wise vector add)',       None,       (161, 161)),
    ('gpt — forward pass (model)',            (111, 147), (163, 202)),
    ('Adam state init',                       (149, 152), (205, 212)),
    ('Training loop',                         (154, 188), (214, 258)),
    ('Final loss + inference',                (190, 207), (260, 286)),
]


PY_KEYWORDS = {
    'and', 'as', 'break', 'class', 'continue', 'def', 'del', 'elif',
    'else', 'except', 'False', 'finally', 'for', 'from', 'global', 'if',
    'import', 'in', 'is', 'lambda', 'None', 'nonlocal', 'not', 'or',
    'pass', 'raise', 'return', 'True', 'try', 'while', 'with', 'yield',
}

CUL_KEYWORDS = {
    'and', 'break', 'class', 'continue', 'else', 'false', 'fn', 'for',
    'if', 'in', 'let', 'match', 'mut', 'new', 'or', 'puts', 'return',
    'self', 'this', 'true', 'while',
}


def html_escape(s: str) -> str:
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def highlight(line: str, lang: str, in_docstring: bool = False) -> str:
    """Tokenize one line and return HTML with class spans.

    The grammar is intentionally tiny — just enough for what the
    microgpt files actually contain. Order: comment-tail, then strings,
    numbers, keywords. The remainder is escaped verbatim.

    `in_docstring=True` skips string parsing for lines that sit inside
    a Python triple-quoted block (otherwise an apostrophe would start a
    spurious string region).
    """
    # Locate the start of a trailing comment (# in py, # in cul) that
    # is not inside a string. Inside a docstring, treat quotes as plain
    # text — they don't open string regions.
    comment_start = -1
    in_str = None
    i = 0
    while i < len(line):
        c = line[i]
        if in_str:
            if c == '\\' and i + 1 < len(line):
                i += 2
                continue
            if c == in_str:
                in_str = None
        else:
            if not in_docstring and c in ('"', "'"):
                in_str = c
            elif c == '#':
                comment_start = i
                break
        i += 1

    if comment_start >= 0:
        code_part = line[:comment_start]
        comment_part = line[comment_start:]
    else:
        code_part = line
        comment_part = ''

    # Tokenize code_part: alternate between strings, numbers, identifiers, other.
    out = []
    i = 0
    keywords = PY_KEYWORDS if lang == 'py' else CUL_KEYWORDS
    while i < len(code_part):
        c = code_part[i]
        # String literal (skipped inside docstrings).
        if not in_docstring and c in ('"', "'"):
            quote = c
            j = i + 1
            while j < len(code_part):
                if code_part[j] == '\\' and j + 1 < len(code_part):
                    j += 2
                    continue
                if code_part[j] == quote:
                    j += 1
                    break
                j += 1
            out.append('<span class="str">' + html_escape(code_part[i:j]) + '</span>')
            i = j
            continue
        # f-string prefix (e.g. f"...") — handle the f as plain text and
        # let the next iter pick up the string.
        if lang == 'py' and c == 'f' and i + 1 < len(code_part) and code_part[i+1] in ('"', "'"):
            out.append('f')
            i += 1
            continue
        # Number. Match int / float / scientific notation only — stop
        # before letters or a `..` range operator so `0..n_layer` keeps
        # the `0` separate from the rest.
        if c.isdigit() or (c == '.' and i + 1 < len(code_part) and code_part[i+1].isdigit()):
            j = i
            seen_dot = (c == '.')
            while j < len(code_part):
                ch = code_part[j]
                if ch.isdigit():
                    j += 1
                elif (ch == '.' and not seen_dot and j + 1 < len(code_part)
                      and code_part[j+1].isdigit()):
                    seen_dot = True
                    j += 1
                else:
                    break
            if j < len(code_part) and code_part[j] in 'eE':
                k = j + 1
                if k < len(code_part) and code_part[k] in '+-':
                    k += 1
                if k < len(code_part) and code_part[k].isdigit():
                    j = k
                    while j < len(code_part) and code_part[j].isdigit():
                        j += 1
            out.append('<span class="num">' + html_escape(code_part[i:j]) + '</span>')
            i = j
            continue
        # Identifier / keyword.
        if c.isalpha() or c == '_':
            j = i
            while j < len(code_part) and (code_part[j].isalnum() or code_part[j] == '_'):
                j += 1
            word = code_part[i:j]
            if word in keywords:
                out.append('<span class="kw">' + html_escape(word) + '</span>')
            else:
                out.append(html_escape(word))
            i = j
            continue
        # Anything else: punctuation, whitespace.
        out.append(html_escape(c))
        i += 1

    code_html = ''.join(out)
    if comment_part:
        code_html += '<span class="cmt">' + html_escape(comment_part) + '</span>'
    return code_html


def docstring_mask(lines: list[str]) -> list[bool]:
    # Mark every line that sits inside a Python triple-quoted block.
    # The line that opens or closes the block is also marked, so its
    # apostrophes don't get parsed as strings. We toggle on each
    # triple-quote occurrence.
    mask = [False] * len(lines)
    open_count = 0
    for idx, line in enumerate(lines):
        triples = line.count('"' * 3) + line.count("'" * 3)
        if open_count > 0:
            mask[idx] = True
        for _ in range(triples):
            open_count = 1 - open_count
            mask[idx] = True
    return mask


def render_line(lineno: int, src: str, lang: str, in_docstring: bool) -> str:
    if src.strip() == '':
        body = '&nbsp;'
    else:
        body = highlight(src, lang, in_docstring=in_docstring)
    return (f'<tr><td class="ln">{lineno:3d}</td>'
            f'<td class="src">{body}</td></tr>')


def render_pane(lines: list[str], mask: list[bool], rng, lang: str) -> str:
    if rng is None:
        return '<div class="empty">— no equivalent (handled by language builtins) —</div>'
    start, end = rng
    rows = []
    for n in range(start, end + 1):
        rows.append(render_line(n, lines[n - 1], lang, mask[n - 1]))
    return ('<div class="scroll"><table class="code"><tbody>\n'
            + '\n'.join(rows)
            + '\n</tbody></table></div>')


def main() -> None:
    py_lines = PY_PATH.read_text().splitlines()
    cul_lines = CUL_PATH.read_text().splitlines()

    py_mask = docstring_mask(py_lines)
    cul_mask = [False] * len(cul_lines)

    py_total = len(py_lines)
    cul_total = len(cul_lines)

    parts = []
    for title, py_rng, cul_rng in SECTIONS:
        parts.append(f'      <tr class="section"><td colspan="2">{title}</td></tr>')
        py_html = render_pane(py_lines, py_mask, py_rng, 'py')
        cul_html = render_pane(cul_lines, cul_mask, cul_rng, 'cul')
        parts.append(
            '      <tr class="code-row">'
            f'<td class="py">{py_html}</td>'
            f'<td class="cul">{cul_html}</td>'
            '</tr>'
        )

    body = '\n'.join(parts)

    html = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>microgpt — Python ↔ Culebra side-by-side</title>
<style>
  /* GitHub light-mode palette (Primer Primitives v0.x). */
  :root {{
    --bg: #ffffff;
    --fg: #1f2328;
    --fg-muted: #59636e;
    --border: #d1d9e0;
    --border-muted: #d1d9e0b3;
    --header-bg: #ffffff;
    --section-bg: #f6f8fa;
    --hover-bg: #f6f8fa;
    --kw: #cf222e;        /* red */
    --str: #0a3069;       /* dark blue */
    --num: #0550ae;       /* blue */
    --cmt: #59636e;       /* muted grey */
  }}
  * {{ box-sizing: border-box; }}
  html, body {{ margin: 0; padding: 0; }}
  body {{
    background: var(--bg);
    color: var(--fg);
    font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI",
                  "Noto Sans", Helvetica, Arial, sans-serif;
  }}
  header {{
    background: var(--header-bg);
    color: var(--fg);
    padding: 16px 24px;
    border-bottom: 1px solid var(--border);
    position: sticky;
    top: 0;
    z-index: 10;
  }}
  header h1 {{
    font-size: 14px;
    font-weight: 600;
    margin: 0;
    display: flex;
    align-items: baseline;
    gap: 12px;
  }}
  header .sub {{ font-size: 12px; color: var(--fg-muted); font-weight: 400; }}
  main {{
    max-width: 1800px;
    margin: 0 auto;
    padding: 0;
  }}
  table.layout {{
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
    background: var(--bg);
  }}
  table.layout > thead th.col-head {{
    text-align: left;
    background: var(--header-bg);
    border-bottom: 1px solid var(--border);
    padding: 10px 16px;
    font: 600 12px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI",
                       sans-serif;
    color: var(--fg);
    position: sticky;
    top: 49px;
    z-index: 5;
  }}
  table.layout > thead th.col-head .lang {{
    color: var(--fg-muted);
    font-weight: 400;
    margin-left: 8px;
  }}
  table.layout > thead th.col-py  {{
    width: 50%;
    border-right: 1px solid var(--border);
  }}
  table.layout > thead th.col-cul {{ width: 50%; }}
  table.layout > tbody tr.section td {{
    background: var(--section-bg);
    color: var(--fg-muted);
    border-top: 1px solid var(--border);
    border-bottom: 1px solid var(--border);
    padding: 6px 16px;
    font: 500 12px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI",
                       sans-serif;
  }}
  table.layout > tbody tr.code-row > td {{
    vertical-align: top;
    padding: 0;
    border-bottom: 1px solid var(--border-muted);
  }}
  table.layout > tbody tr.code-row > td.py {{
    border-right: 1px solid var(--border);
  }}
  div.scroll {{
    overflow-x: auto;
    max-width: 100%;
  }}
  table.code {{
    border-collapse: collapse;
    width: 100%;
    font: 12px/20px ui-monospace, SFMono-Regular, "SF Mono", Menlo,
                    Consolas, "Liberation Mono", monospace;
  }}
  /* Per-cell rules below set their own padding; specificity matters
     so we don't reset it with a more general `table.code td` selector. */
  table.code td.ln {{
    vertical-align: top;
    color: var(--fg-muted);
    background: var(--bg);
    text-align: right;
    padding: 0 10px;
    user-select: none;
    font-variant-numeric: tabular-nums;
    width: 1%;
    white-space: pre;
  }}
  table.code td.src {{
    vertical-align: top;
    padding: 0 10px;
    white-space: pre;
    color: var(--fg);
  }}
  table.code tr:hover td.ln,
  table.code tr:hover td.src {{
    background: var(--hover-bg);
  }}
  .empty {{
    padding: 16px;
    color: var(--fg-muted);
    font-style: italic;
    font-size: 12px;
  }}
  .kw  {{ color: var(--kw); }}
  .str {{ color: var(--str); }}
  .num {{ color: var(--num); }}
  .cmt {{ color: var(--cmt); }}
</style>
</head>
<body>
<header>
  <h1>microgpt
    <span class="sub">Karpathy scalar autograd — Python ({py_total} lines) vs Culebra ({cul_total} lines)</span>
  </h1>
</header>
<main>
  <table class="layout">
    <thead>
      <tr>
        <th class="col-head col-py">microgpt.py <span class="lang">Python (CPython)</span></th>
        <th class="col-head col-cul">microgpt.cul <span class="lang">Culebra (--jit)</span></th>
      </tr>
    </thead>
    <tbody>
{body}
    </tbody>
  </table>
</main>
</body>
</html>
'''
    OUT_PATH.write_text(html)
    print(f'wrote {OUT_PATH} ({len(html.splitlines())} lines)')
    print(f'  microgpt.py:  {py_total} lines')
    print(f'  microgpt.cul: {cul_total} lines')


if __name__ == '__main__':
    main()
