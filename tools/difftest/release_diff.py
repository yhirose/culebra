#!/usr/bin/env python3
"""Compare two binaries' output over the generated corpus, case by case.

Driven by release_diff.sh, which builds the corpus and runs it under the
previous release's binary and this one.  What arrives here is two files of
records, one per case, in the same order.

Not a byte diff.  The older binary answers `err=` where this one answers `ok=`
whenever a release added a built-in, and an `ok=` case may print lines the
`err=` case never gets to, so the files stop lining up as text long before they
stop lining up as cases.  `_p` emits exactly one record per case under every
binary, so the records are walked positionally and each one carries the output
printed after it.

A change is allowed by naming it in release_diff_allow.txt as a glob over the
case label, which is what makes that file a draft of the release notes.  The
contract runs both ways, like the leak-abort allowlist: an unlisted change
fails, and a listed pattern that no longer matches anything is reported so the
file can shrink after the release that needed it ships.

Usage: release_diff.py --baseline F --head F --allow F --cases N
                       [--baseline-name S] [--head-name S]
"""
import argparse
import re
import sys
from collections import Counter

SEP = " ::: "
# What release_diff.sh's per-case fallback writes for a case a binary could not
# run at all.  Expected on the baseline side — that is what a release predating
# a syntax looks like — and a failure on the head side, which generated the
# corpus and therefore can run every case in it.
UNSUPPORTED = "unsupported"
# How many unlisted changes to spell out in full.  The category tally above
# them is complete either way; this only bounds the paste.
SHOW = 60


def read_records(path):
    """Read one side into [label, body], body carrying any printed output."""
    records = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if SEP in line:
            label, _, body = line.partition(SEP)
            records.append([label, body])
        elif records:
            records[-1][1] += "\n" + line
        # Output printed before the first record belongs to no case: the
        # preamble does not print, so this is a diagnostic from the binary
        # itself, and the completion guard in the shell has already seen it.
    return records


WILDCARDS = {"*": ".*", "?": "."}


def compile_glob(pattern):
    """`*` and `?` are the wildcards; every other character is literal.

    Not fnmatch: a case label is a fragment of culebra source, so `[1, 2, 3]`
    is ordinary text in one and a character class to the other — an entry
    written to name one case would quietly match a hundred.
    """
    body = "".join(WILDCARDS.get(ch) or re.escape(ch) for ch in pattern)
    return re.compile(body + r"\Z", re.DOTALL)


def read_allow(path):
    """Read the allowlist into [(text, regex)], keeping the text for reports."""
    # No FileNotFoundError arm: the file is committed, so a missing one means a
    # mistyped --allow, and returning [] there would report every difference as
    # unlisted or — with no differences — print OK having read nothing.
    patterns = []
    text = open(path, encoding="utf-8").read()
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append((line, compile_glob(line)))
    return patterns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--head", required=True)
    ap.add_argument("--allow", required=True)
    ap.add_argument("--cases", type=int, required=True)
    ap.add_argument("--baseline-name", default="baseline")
    ap.add_argument("--head-name", default="head")
    args = ap.parse_args()

    base = read_records(args.baseline)
    head = read_records(args.head)

    print(f"release-diff: {args.cases} cases")
    print(f"  baseline: {args.baseline_name}")
    print(f"  head:     {args.head_name}")

    # The fallback in the shell exists to make this hold; if it still does not,
    # the two sides are not describing the same corpus and nothing below means
    # anything.
    if len(base) != len(head):
        print(
            f"release-diff: FAIL — record count differs: "
            f"baseline={len(base)} head={len(head)}",
            file=sys.stderr,
        )
        return 1

    patterns = read_allow(args.allow)
    changed, unlisted, used = [], [], set()
    for (blabel, bbody), (hlabel, hbody) in zip(base, head):
        if blabel != hlabel:
            print(
                f"release-diff: FAIL — records out of step: "
                f"baseline '{blabel}' vs head '{hlabel}'",
                file=sys.stderr,
            )
            return 1
        if hbody == UNSUPPORTED:
            print(
                f"release-diff: FAIL — the head binary could not run "
                f"'{hlabel}', a case it generated",
                file=sys.stderr,
            )
            return 1
        if bbody == hbody:
            continue
        changed.append((blabel, bbody, hbody))
        hit = next((text for text, rx in patterns if rx.match(blabel)), None)
        if hit is None:
            unlisted.append((blabel, bbody, hbody))
        else:
            used.add(hit)

    print(
        f"  {len(changed)} changed, {len(changed) - len(unlisted)} allowed, "
        f"{len(unlisted)} unlisted"
    )

    stale = [text for text, _ in patterns if text not in used]
    if stale:
        # Advisory, not a failure: the release that needed the entry may not
        # have shipped yet, and a gate that fails on an entry being *too*
        # generous teaches people to write it late.
        print()
        print(f"{len(stale)} allowlist pattern(s) match nothing — {args.allow} can shrink:")
        for p in stale:
            print(f"  {p}")

    if not unlisted:
        print()
        print("release-diff: OK")
        return 0

    print()
    print("unlisted changes by category:")
    for cat, n in Counter(label.split("|")[0] for label, _, _ in unlisted).most_common():
        print(f"  {n:6d}  {cat}")
    print()
    print(f"unlisted (< baseline / > head), first {SHOW}:")
    for label, bbody, hbody in unlisted[:SHOW]:
        print(f"< {label}{SEP}{bbody}")
        print(f"> {label}{SEP}{hbody}")
    if len(unlisted) > SHOW:
        print(f"  ... and {len(unlisted) - SHOW} more")
    print()
    print(
        f"release-diff: FAIL — {len(unlisted)} behavioural change(s) not named "
        f"in {args.allow}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
