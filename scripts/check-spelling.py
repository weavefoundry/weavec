#!/usr/bin/env python3
"""Check that the repository is written in American English.

Every file git tracks (or would track: untracked but not ignored) is read
as text, but those in SKIPPED (a generated changelog, a lock file's
third-party package names) and binary ones. Each British spelling in
RULES is reported with its American form:

  -our      behaviour, neighbour, honour, favour, ... (and behaviours,
            neighbouring, favourite, ...): -or.
  -ise      initialise, optimiser, normalisation, recognisable, ...: -ize,
            for the stems in ISE_STEMS, with the endings in ISE_ENDINGS
            (never -ism, -ist or -istic: optimism stays).
  -yse      analyse, analysed, analyser, paralyse, ...: -yze. "analyses" is
            not checked: it is also the plural of "analysis".
  -ll-      labelled, modelling, signalled, travelling, ...: one l.
  words     licence, judgement, artefact, programme, learnt, grey, ...

A word is matched at the start of a word or of a camelCase hump
(zeroInitialises), after the prefixes re-, un-, mis-, pre-, non-, de-,
sub- and auto- too. ALLOWED lists text that must keep a British spelling:
RFC 0009's file name, which is the URL of its page on the documentation
site. `--fix` rewrites each file with the American forms, keeping each
word's case.

Exit status: 0 when nothing is found (or everything was fixed), 1 when a
spelling is found, 2 when the files cannot be listed.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterator, Optional

ROOT = Path(__file__).resolve().parent.parent

# Generated or third-party text, and this checker's own rule table.
SKIPPED = frozenset((
    "CHANGELOG.md",
    "docs/package-lock.json",
    "scripts/check-spelling.py",
    "scripts/test_check_spelling.py",
))

# Text that keeps its British spelling.
ALLOWED = ("0009-value-conditional-behaviour",)

OUR_STEMS = ("arm", "behavi", "col", "endeav", "fav", "flav", "harb", "hon",
             "hum", "lab", "neighb", "od", "parl", "rum", "sav", "vap")
ISE_STEMS = (
    "amort", "apolog", "author", "canonical", "capital", "categor",
    "central", "character", "critic", "custom", "digit", "discret",
    "emphas", "equal", "final", "formal", "general", "global", "harmon",
    "ideal", "initial", "item", "legal", "linear", "local", "material",
    "maxim", "memo", "minim", "mobil", "modern", "neutral", "normal",
    "optim", "organ", "parallel", "parametr", "penal", "polar", "priorit",
    "random", "rational", "real", "recogn", "sanit", "serial", "special",
    "stabil", "standard", "summar", "symbol", "synchron", "synthes",
    "theor", "token", "trivial", "util", "vector", "virtual", "visual",
)
ISE_ENDINGS = ("e", "ed", "es", "ing", "er", "ers", "ation", "ations",
               "able", "ability")
YSE_STEMS = ("analy", "cataly", "dialy", "electroly", "paraly")
YSE_ENDINGS = ("e", "ed", "ing", "er", "ers")
LL_WORDS = {
    "cancelled": "canceled", "cancelling": "canceling",
    "channelled": "channeled", "channelling": "channeling",
    "counselling": "counseling", "dialled": "dialed", "dialling": "dialing",
    "fuelled": "fueled", "fuelling": "fueling",
    "labelled": "labeled", "labelling": "labeling",
    "levelled": "leveled", "levelling": "leveling",
    "modelled": "modeled", "modelling": "modeling",
    "modeller": "modeler", "modellers": "modelers",
    "quarrelled": "quarreled", "signalled": "signaled",
    "signalling": "signaling", "totalled": "totaled",
    "totalling": "totaling", "travelled": "traveled",
    "travelling": "traveling", "traveller": "traveler",
    "travellers": "travelers", "tunnelled": "tunneled",
    "tunnelling": "tunneling",
}
WORDS = {
    "aluminium": "aluminum", "amongst": "among", "artefact": "artifact",
    "artefacts": "artifacts", "catalogue": "catalog",
    "catalogues": "catalogs", "centre": "center", "centres": "centers",
    "defence": "defense", "enrol": "enroll", "fibre": "fiber",
    "fibres": "fibers", "fulfil": "fulfill", "grey": "gray",
    "instalment": "installment", "judgement": "judgment",
    "judgements": "judgments", "learnt": "learned", "licence": "license",
    "licences": "licenses", "manoeuvre": "maneuver", "offence": "offense",
    "practise": "practice", "programme": "program",
    "programmes": "programs", "sceptical": "skeptical",
    "skilful": "skillful", "spelt": "spelled", "whilst": "while",
    "wilful": "willful",
}


def _any(words) -> str:
    return "(?:" + "|".join(sorted(words, key=len, reverse=True)) + ")"


# The start of a word or of a camelCase hump, an optional prefix, and (case
# sensitively) no lowercase letter after the word.
START = r"(?:(?<![A-Za-z])|(?<=[a-z0-9_])(?=[A-Z]))"
PREFIX = r"(?:re|un|mis|dis|pre|non|de|sub|auto)?"
END = r"(?-i:(?![a-z]))"

# Each rule: a pattern whose groups are the word's kept head, its British
# middle and its kept tail, and the American middle (None: from a table).
RULES: list[tuple[re.Pattern, Optional[str]]] = [
    (re.compile(START + "(?P<head>" + PREFIX + _any(OUR_STEMS) + ")(?P<mid>our)"
                r"(?P<tail>(?-i:[a-z]*))", re.IGNORECASE), "or"),
    (re.compile(START + "(?P<head>" + PREFIX + _any(ISE_STEMS) + ")(?P<mid>is)"
                "(?P<tail>" + _any(ISE_ENDINGS) + ")" + END, re.IGNORECASE), "iz"),
    (re.compile(START + "(?P<head>" + PREFIX + _any(YSE_STEMS) + ")(?P<mid>s)"
                "(?P<tail>" + _any(YSE_ENDINGS) + ")" + END, re.IGNORECASE), "z"),
    (re.compile(START + "(?P<head>" + PREFIX + ")(?P<mid>" + _any(LL_WORDS) + ")"
                "(?P<tail>)" + END, re.IGNORECASE), None),
    (re.compile(START + "(?P<head>)(?P<mid>" + _any(WORDS) + ")(?P<tail>)" + END,
                re.IGNORECASE), None),
]


def _keep_case(original: str, american: str) -> str:
    """`american` cased as `original` is: lower, Capitalized or UPPER."""
    if len(original) > 1 and original.isupper():
        return american.upper()
    if original[:1].isupper():
        return american[:1].upper() + american[1:]
    return american


def _american(match: re.Match, middle: Optional[str]) -> str:
    british = match.group("mid")
    if middle is None:
        middle = {**LL_WORDS, **WORDS}[british.lower()]
        return match.group("head") + _keep_case(british, middle)
    return match.group("head") + _keep_case(british, middle) + match.group("tail")


def _matches(text: str) -> list[tuple[re.Match, Optional[str]]]:
    """The British spellings in `text`, in order, outside ALLOWED."""
    allowed = [(m.start(), m.end()) for name in ALLOWED
               for m in re.finditer(re.escape(name), text)]
    found: dict[int, tuple[re.Match, Optional[str]]] = {}
    for pattern, middle in RULES:
        for match in pattern.finditer(text):
            if match.start() not in found and not any(
                    a <= match.start() < b for a, b in allowed):
                found[match.start()] = (match, middle)
    return [found[start] for start in sorted(found)]


def american(word: str) -> str:
    """The American form of `word`, or `word` when no rule matches it."""
    for pattern, middle in RULES:
        match = pattern.fullmatch(word)
        if match is not None:
            return _american(match, middle)
    return word


def findings(text: str) -> Iterator[tuple[int, int, str, str]]:
    """(line, column, British, American) of each spelling in `text`."""
    for match, middle in _matches(text):
        line = text.count("\n", 0, match.start()) + 1
        column = match.start() - (text.rfind("\n", 0, match.start()) + 1) + 1
        yield line, column, match.group(0), _american(match, middle)


def fixed(text: str) -> str:
    """`text` with every British spelling replaced by its American form."""
    pieces = []
    at = 0
    for match, middle in _matches(text):
        pieces.append(text[at:match.start()])
        pieces.append(_american(match, middle))
        at = match.end()
    pieces.append(text[at:])
    return "".join(pieces)


def tracked_files(root: Path) -> Optional[list[str]]:
    """The files git tracks or would track under `root`, or None."""
    try:
        listed = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root, capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return sorted({path for path in listed.stdout.decode().split("\0") if path})


def read_text(path: Path) -> Optional[str]:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--fix", action="store_true",
                        help="rewrite the files with the American forms")
    args = parser.parse_args(argv)
    files = tracked_files(args.root)
    if files is None:
        print("check-spelling: cannot list the files (is this a git work tree?)",
              file=sys.stderr)
        return 2
    found = 0
    for name in files:
        if name in SKIPPED:
            continue
        path = args.root / name
        text = read_text(path)
        if text is None:
            continue
        hits = list(findings(text))
        if not hits:
            continue
        if args.fix:
            path.write_text(fixed(text), encoding="utf-8")
            continue
        for line, column, british, us in hits:
            print(f"{name}:{line}:{column}: {british} -> {us}")
        found += len(hits)
    if found:
        print(f"check-spelling: {found} British spelling(s); "
              "scripts/check-spelling.py --fix rewrites them", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
