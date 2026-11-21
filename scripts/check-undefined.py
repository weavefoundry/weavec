#!/usr/bin/env python3
# Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""RFC 0051 gate G2: the list of undefined behavior is closed.

Reads resources/safe/undefined.md and checks that it has a row for each of
the 221 items of C23's Annex J.2 and for each entry of C++'s annex of core
undefined behavior it lists, that every row names one of the page's
dispositions (none says "undefined"), and that every row's test exists.
Exits non-zero, naming each row that fails.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "resources" / "safe" / "undefined.md"
DISPOSITIONS = {"rule", "defined", "stops", "compared", "stale", "cannot", "translation", "library", "assumed"}
C_ITEMS = 221
CXX_ENTRIES = 80


def main() -> int:
    text = PAGE.read_text(encoding="utf-8")
    failures = []
    section = None
    c_items = set()
    cxx_entries = set()
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if section not in ("C", "C++") or not line.startswith("| ") or line.startswith("| ---") or \
                line.startswith("| J.2 ") or line.startswith("| Entry "):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 5:
            failures.append(f"line {number}: {len(cells)} cells, expected 5")
            continue
        item, _, disposition, how, test = cells
        if disposition not in DISPOSITIONS:
            failures.append(f"line {number}: '{item}' has the disposition '{disposition}'")
        if not how:
            failures.append(f"line {number}: '{item}' says nothing of how")
        path = test.strip("`")
        if not (ROOT / path).is_file():
            failures.append(f"line {number}: '{item}' names the test '{path}', which does not exist")
        if section == "C":
            if not item.isdigit():
                failures.append(f"line {number}: '{item}' is not a J.2 item number")
            else:
                c_items.add(int(item))
        else:
            label = item.strip("`")
            if not re.fullmatch(r"[a-z.]+", label):
                failures.append(f"line {number}: '{item}' is not an annex label")
            cxx_entries.add(label)
    missing = sorted(set(range(1, C_ITEMS + 1)) - c_items)
    if missing:
        failures.append(f"C: no row for J.2 items {missing}")
    if len(cxx_entries) != CXX_ENTRIES:
        failures.append(f"C++: {len(cxx_entries)} entries, expected {CXX_ENTRIES}")
    for failure in failures:
        print(f"check-undefined: {failure}")
    if not failures:
        print(f"check-undefined: {len(c_items)} C items and {len(cxx_entries)} C++ entries, each disposed of and tested")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
