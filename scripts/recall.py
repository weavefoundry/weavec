#!/usr/bin/env python3
"""Run weavec over the recall set and report what it caught, per bug class.

The recall set (test/recall/) holds small C programs in the shape of the
Juliet test suite's CWE cases: one bug each, with a comment naming the
diagnostic that must be reported on that line (RFC 0011, *Recall check*).
This script runs each case, checks the pinned diagnostics were reported at
the pinned lines, and prints recall per CWE. It exits non-zero when a pinned
diagnostic is missing, or when an *error* the case did not pin appears (the
cases are free of other bugs by construction, so that is a false positive).

Case format:

  // CWE-121: what the case is about.
  #include "recall.h"
  void bad(void) {
    char buf[10];
    buf[10] = 0;                 // RECALL: out-of-bounds
  }

`// RECALL: <id>` pins `<id>` on the line it sits on; `// RECALL: <id>
@<line>` pins it on another line. A case with no RECALL comment is a "good"
case: it must produce no error at all. The CWE is the case's directory name.

The lit suite (test/Analysis) pins messages and locations exactly and is the
regression suite; this set asks only "was this bug class caught", and is
what the README's status table cites.

Examples:

  scripts/recall.py --weavec build/dev/bin/weavec
  scripts/recall.py --weavec build/dev/bin/weavec --only CWE-122 --verbose
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = ROOT / "test" / "recall"
RESOURCE_INCLUDE = ROOT / "resources" / "include"

RECALL_RE = re.compile(r"//\s*RECALL:\s*(?P<id>[a-z-]+)(?:\s*@(?P<line>\d+))?")
# `file:line:col: severity: message [weavec::id]` as printed by the tool.
DIAG_RE = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<severity>error|warning): (?P<message>.*) \[weavec::(?P<id>[a-z-]+)\]$"
)
CLANG_ERROR_RE = re.compile(r"^(?P<file>[^:\n]+):\d+:\d+: (?:fatal )?error: (?P<message>.*)$")


@dataclasses.dataclass(frozen=True)
class Pin:
    id: str
    line: int


@dataclasses.dataclass
class Diagnostic:
    line: int
    severity: str
    id: str
    message: str


@dataclasses.dataclass
class Outcome:
    case: Path
    cwe: str
    pins: list[Pin]
    caught: list[Pin]
    missed: list[Pin]
    unexpected: list[Diagnostic]
    clang_errors: list[str]

    @property
    def ok(self) -> bool:
        return not self.missed and not self.unexpected and not self.clang_errors


def pins_of(source: Path) -> list[Pin]:
    pins: list[Pin] = []
    for number, text in enumerate(source.read_text().splitlines(), start=1):
        for match in RECALL_RE.finditer(text):
            line = int(match.group("line")) if match.group("line") else number
            pins.append(Pin(id=match.group("id"), line=line))
    return pins


def run_case(weavec: Path, source: Path, extra_args: list[str]) -> tuple[list[Diagnostic], list[str]]:
    command = [
        str(weavec),
        str(source),
        "--",
        f"-I{source.parent.parent}",
        f"-I{RESOURCE_INCLUDE}",
        "-ferror-limit=0",
        *extra_args,
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    diagnostics: list[Diagnostic] = []
    clang_errors: list[str] = []
    for text in (completed.stdout + completed.stderr).splitlines():
        if match := DIAG_RE.match(text):
            if Path(match.group("file")).name != source.name:
                continue
            diagnostics.append(
                Diagnostic(
                    line=int(match.group("line")),
                    severity=match.group("severity"),
                    id=match.group("id"),
                    message=match.group("message"),
                )
            )
        elif match := CLANG_ERROR_RE.match(text):
            clang_errors.append(match.group("message"))
    return diagnostics, clang_errors


def judge(source: Path, diagnostics: list[Diagnostic], clang_errors: list[str]) -> Outcome:
    pins = pins_of(source)
    reported = {(d.id, d.line) for d in diagnostics}
    caught = [pin for pin in pins if (pin.id, pin.line) in reported]
    missed = [pin for pin in pins if (pin.id, pin.line) not in reported]
    pinned_lines = {pin.line for pin in pins}
    # An error on a line the case did not pin is a false positive. Warnings
    # are not counted: a leak on the path a bug diverts is the bug's shadow.
    unexpected = [d for d in diagnostics if d.severity == "error" and d.line not in pinned_lines]
    return Outcome(
        case=source,
        cwe=source.parent.name,
        pins=pins,
        caught=caught,
        missed=missed,
        unexpected=unexpected,
        clang_errors=clang_errors,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--weavec", type=Path, required=True, help="path to the weavec binary")
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES, help="directory of CWE-*/ case directories")
    parser.add_argument("--only", action="append", default=[], help="run only cases whose path contains this (repeatable)")
    parser.add_argument("--verbose", "-v", action="store_true", help="list every case, not just the failures")
    parser.add_argument("extra", nargs="*", help="extra compiler arguments (after --)")
    args = parser.parse_args()

    if not args.weavec.exists():
        print(f"error: weavec not found at {args.weavec}", file=sys.stderr)
        return 2

    sources = sorted(args.cases.glob("CWE-*/*.c"))
    if args.only:
        sources = [s for s in sources if any(o in str(s) for o in args.only)]
    if not sources:
        print(f"error: no cases under {args.cases}", file=sys.stderr)
        return 2

    outcomes = [judge(s, *run_case(args.weavec, s, args.extra)) for s in sources]

    per_cwe: dict[str, list[Outcome]] = collections.defaultdict(list)
    for outcome in outcomes:
        per_cwe[outcome.cwe].append(outcome)

    width = max(len(cwe) for cwe in per_cwe)
    print(f"{'class':<{width}}  {'bugs':>5} {'caught':>6} {'recall':>7}  {'false+':>6}  cases")
    total_pins = total_caught = total_unexpected = 0
    for cwe in sorted(per_cwe):
        group = per_cwe[cwe]
        pins = sum(len(o.pins) for o in group)
        caught = sum(len(o.caught) for o in group)
        unexpected = sum(len(o.unexpected) + len(o.clang_errors) for o in group)
        recall = f"{100.0 * caught / pins:6.1f}%" if pins else "     -"
        print(f"{cwe:<{width}}  {pins:>5} {caught:>6} {recall:>7}  {unexpected:>6}  {len(group)}")
        total_pins += pins
        total_caught += caught
        total_unexpected += unexpected
    recall = f"{100.0 * total_caught / total_pins:6.1f}%" if total_pins else "     -"
    print(f"{'total':<{width}}  {total_pins:>5} {total_caught:>6} {recall:>7}  {total_unexpected:>6}  {len(outcomes)}")

    failed = [o for o in outcomes if not o.ok]
    for outcome in outcomes:
        if outcome.ok and not args.verbose:
            continue
        rel = outcome.case.relative_to(args.cases)
        status = "ok" if outcome.ok else "FAIL"
        print(f"\n{status}: {rel}")
        for pin in outcome.caught:
            if args.verbose:
                print(f"  caught  {pin.id} @{pin.line}")
        for pin in outcome.missed:
            print(f"  MISSED  {pin.id} @{pin.line}")
        for d in outcome.unexpected:
            print(f"  EXTRA   {d.id} @{d.line}: {d.message}")
        for message in outcome.clang_errors:
            print(f"  CLANG   {message}")
    if failed:
        print(f"\n{len(failed)} of {len(outcomes)} cases failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
