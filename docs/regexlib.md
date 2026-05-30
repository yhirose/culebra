# regexlib

`include/regexlib.h` is a header-only regular-expression engine. It is developed
inside culebra and is intended to move to a standalone `cpp-regexlib`
repository; its only dependency is cpp-unicodelib.

This document is the reference for what the engine accepts and how it matches.

## Matching model

- **Unit of matching is the Unicode extended grapheme cluster**, not the code
  point. `.` consumes exactly one user-perceived character, so `/.+/` against
  `"👨‍👩‍👧‍👦"` matches a single element. A character class or `\d`/`\w`/`\s`
  matches a grapheme only when that grapheme is a single code point in the set.
- **Match offsets are byte offsets** into the original UTF-8 subject (Go-style
  indexing). They always fall on grapheme-cluster boundaries.
- **Matching is leftmost-first** (Perl-style alternative/quantifier priority),
  with the exceptions noted under *Semantics and known differences*.
- **Matching runs in linear time.** The engine is a Thompson NFA simulated by a
  Pike VM; catastrophic backtracking cannot occur, so the engine is immune to
  ReDoS. Backreferences are therefore not supported.

## Supported syntax

| Category | Syntax |
| --- | --- |
| Literal | any character; a base + combining marks form one literal grapheme |
| Any | `.` (excludes line breaks unless DotAll — see below) |
| Anchors | `^`, `$` (line-relative under Multiline) |
| Character class | `[abc]`, `[a-z]`, `[^…]`; `\p{…}` may appear inside |
| Predefined class | `\d \w \s` and `\D \W \S` (Unicode-property aware) |
| Unicode property | `\p{Name}`, `\P{Name}`, single-letter `\pL` |
| Quantifiers | `* + ?`, `{n}`, `{n,m}`, `{n,}`; lazy variants `*? +? ?? {n,m}?` |
| Alternation | `\|` |
| Group | `(…)` capturing, `(?:…)` non-capturing |
| Named group | `(?<name>…)`, `(?'name'…)` |
| Word boundary | `\b`, `\B` |
| Escapes | `\n \r \t \f \v \0`, `\xHH`, `\x{…}`, `\uHHHH`, escaped metacharacters |
| Lookahead | `(?=…)`, `(?!…)` — variable length |
| Lookbehind | `(?<=…)`, `(?<!…)` — variable length |
| Flags (inline) | `(?i)`, `(?m)`, `(?s)` |

`.` excludes the line-break graphemes `\n`, `\r`, `\v`, `\f`, NEL (U+0085),
LS (U+2028) and PS (U+2029). `DotAll` makes `.` match those too.

Supported `\p{…}` names: `L Lu Ll Lt Lm Lo N Nd Nl No P M S Z C` and
`White_Space`, plus the long aliases (`Letter`, `Number`, `Punctuation`,
`Mark`, `Symbol`, `Separator`, `Other`, `Uppercase_Letter`, …).

## API

```cpp
regexlib::Regex re("pattern", flags);   // flags default to 0
```

Flags are an OR-combinable mask:

| Flag | Equivalent | Effect |
| --- | --- | --- |
| `regexlib::IgnoreCase` | `(?i)` | case-insensitive |
| `regexlib::Multiline` | `(?m)` | `^`/`$` match at line breaks |
| `regexlib::DotAll` | `(?s)` | `.` matches a line break |

Constructor flags and inline flags compose (either turns the flag on).

Methods:

| Method | Result |
| --- | --- |
| `re.test(s)` | `bool` — does the pattern match anywhere |
| `re.search(s)` | `MatchResult` — leftmost match anywhere |
| `re.match(s)` | `MatchResult` — match anchored at the start |
| `re.find_all(s)` | `std::vector<MatchResult>` — all non-overlapping matches |
| `re.replace_all(s, repl)` | `std::string` — `$0`–`$9`, `$<name>`, `$$` in `repl` |

`MatchResult` carries `matched`, `begin`/`end` (byte offsets), `str`, the
capture `groups` (group 0 is the whole match), and named-group lookup. Access a
group with `m.group(i)` or `m.group("name")`; an unmatched group reports
`matched == false`.

A pattern that cannot be parsed (or that exceeds a resource limit) throws
`regexlib::RegexError`, whose message carries the offending position and a
caret line.

## Semantics and known differences

- **Grapheme units** differ from PCRE / RE2 / Python `re`, which match by code
  point. A multi-code-point cluster (emoji, ZWJ sequences, base + combining
  marks) is matched by `.` and by negated predicates (`\D`, `[^…]`), but not by
  a positive range or `\d`/`\w`/`\s`.
- **Nullable quantifier corner.** An unbounded quantifier whose body can match
  the empty string stops after an empty iteration (the Perl rule), so `(.*?)*`
  matches the empty string. One sub-case differs from Perl: an alternation
  whose first branch is nullable, nested in an unbounded quantifier, yields the
  POSIX leftmost-longest result rather than Perl's leftmost-first — `(a*|b)*`
  against `"ab"` matches `"ab"` (POSIX) where Perl yields `"a"`. Reproducing
  Perl here would require backtracking, which is incompatible with the
  linear-time guarantee; RE2 documents the same class of divergence.
- **Captures inside a lookaround are not exported.** Groups inside `(?=…)` /
  `(?<=…)` are treated as non-capturing.
- **Inline flags apply globally.** `(?i)`, `(?m)`, `(?s)` set the flag for the
  whole pattern regardless of position; scoped flags `(?i:…)` are not scoped.
- **`\b` at the end of the subject matches**, as in Perl and Python.

## Resource limits

To keep a small adversarial pattern from exhausting memory or the stack, a
pattern that exceeds any of these raises `RegexError` at construction:

| Limit | Value |
| --- | --- |
| Pattern source length | 32 KiB |
| Compiled program size | 262144 instructions |
| Nesting depth | 1000 |

These bound compile cost (e.g. `a{1000000}`, nested `(x{50}){50}…`) and parser
recursion (e.g. `((((…))))`).

Match time is linear in the subject length, but the constant is the program
size, so a dense pattern (e.g. `(a?){9000}`) on a long subject is
bounded-but-slow. A match-time step budget, proportional to the subject length,
caps this: a match that exceeds it raises `RegexError` from the matching call
(`search`, `match`, `find_all`, `test`, `replace_all`) rather than running for
many seconds. Real patterns stay far below the budget. The ε-closure is
iterative, so a long zero-width chain cannot overflow the stack.

## Not supported

| Feature | Reason / alternative |
| --- | --- |
| Backreferences `\1` | NP-hard; incompatible with the linear-time guarantee |
| POSIX classes `[[:alpha:]]` | use `\p{…}` |
| Conditionals, atomic groups, possessive quantifiers | not implemented |
