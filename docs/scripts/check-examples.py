#!/usr/bin/env python3
"""Exercise the published tutorial examples against a built weavec-cc.

Every example in docs/examples must compile. `weavec-cc` must compile
lookup.c, record the guarded load of `table[i]` in the enforcement ledger
(`weavec-ledger` version 7, RFC 0035 section 9, RFC 0039 section 8, RFC 0042 section 10, RFC 0043 section 11), link it, run it, and stop
a negative index with an `index-out-of-bounds` report; and it must build
vec.c and stop a read past its heap block with a `heap-buffer-overflow`
report, its stack and its summary line (RFC 0035 section 5.3, RFC 0040
section 5; the default `-fweavec-checks=trap`).

Every example of resources/safe/rules.md (RFC 0043 section 12) tagged
`c accepted` compiles under -fweavec-safe, and every one tagged `c rejected`
fails with the error of the rule whose section holds it.
"""

import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--weavec-cc", required=True, type=Path, help="the weavec-cc compiler")
args = parser.parse_args()
weavec_cc = args.weavec_cc.resolve()
examples = Path(__file__).resolve().parent.parent / "examples"
traps = {-signal.SIGTRAP, -signal.SIGILL}


def run(command, **options):
    return subprocess.run(
        [str(part) for part in command],
        capture_output=True, text=True, timeout=60, check=False, **options,
    )


def enforcement_summary(stderr, name):
    """The enforcement summary line (RFC 0035 section 9) for source `name`."""
    pattern = rf"^weavec: (\S*/)?{re.escape(name)}: \d+ accesses: \d+ proven, \d+ guarded, \d+ unguarded$"
    return re.search(pattern, stderr, re.M)


def expect_trap(result, kind, name, line, detail):
    """A trap whose first report is `weavec: <kind> at <name>:<line>:<col>: <detail>`."""
    assert result.returncode in traps, f"expected a trap, got exit status {result.returncode}"
    pattern = rf"^weavec: {re.escape(kind)} at (\S*/)?{re.escape(name)}:{line}:\d+: {detail}"
    assert re.search(pattern, result.stderr, re.M), "unexpected report:\n" + result.stderr


with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    for source in sorted(examples.glob("*.c")):
        compiled = run([weavec_cc, "-std=c17", "-c", source, "-o", temp / (source.stem + ".o")])
        assert compiled.returncode == 0, f"{source.name}:\n{compiled.stderr}"

    ledgers = temp / "ledger"
    ledgers.mkdir()
    obj = temp / "lookup.o"
    compiled = run([weavec_cc, "-std=c17", f"-fweavec-ledger={ledgers}/", "-c",
                    examples / "lookup.c", "-o", obj])
    assert compiled.returncode == 0, compiled.stderr
    assert enforcement_summary(compiled.stderr, "lookup.c"), "no summary line:\n" + compiled.stderr

    ledger = json.loads((ledgers / "lookup.o.ledger.json").read_text())
    assert ledger["schema"] == "weavec-ledger" and ledger["version"] == 8, ledger
    rows = [row for unit in ledger["units"] for row in unit["rows"]
            if row["function"] == "lookup" and row["line"] == 8 and row["operation"] == "load"]
    assert rows, "no ledger row for the load of table[i]"
    assert rows[0]["outcome"] == "guarded", rows[0]

    program = temp / "lookup"
    linked = run([weavec_cc, obj, "-o", program])
    assert linked.returncode == 0, linked.stderr
    inside = run([program, "2"])
    assert inside.returncode == 0 and inside.stdout == "30\n", (inside.returncode, inside.stdout)
    expect_trap(run([program, "-1"]), "index-out-of-bounds", "lookup.c", 8, "index -1")

    vec = temp / "vec"
    built = run([weavec_cc, "-std=c17", examples / "vec.c", "-o", vec])
    assert built.returncode == 0, built.stderr
    inside = run([vec, "2"])
    assert inside.returncode == 0 and inside.stdout == "0\n", (inside.returncode, inside.stdout)
    past = run([vec, "4"])
    expect_trap(past, "heap-buffer-overflow", "vec.c", 9, "read of 4 bytes at 0x[0-9a-f]+$")
    assert re.search(r"^weavec: 0x[0-9a-f]+ is 0 bytes after the 16-byte heap object at 0x[0-9a-f]+, "
                     r"allocated by thread T0:$", past.stderr, re.M), "no heap object line:\n" + past.stderr
    # RFC 0040 section 5: the access's stack, the allocation's, and the summary line.
    assert re.search(r"^    #\d+ 0x[0-9a-f]+ in get \S*vec\.c:9\b", past.stderr, re.M), \
        "no frame of get:\n" + past.stderr
    assert re.search(r"^weavec: summary: heap-buffer-overflow at \S*vec\.c:9:\d+ in get \(thread T0\)$",
                     past.stderr, re.M), "no summary line:\n" + past.stderr

    # RFC 0043 section 12: the rules' examples.
    rules = examples.parent.parent / "resources" / "safe" / "rules.md"
    prelude = "".join(f"#include <{h}>\n" for h in
                      ("weavec.h", "stddef.h", "stdio.h", "stdlib.h", "string.h"))
    cxx_prelude = "".join(f"#include <{h}>\n" for h in (
        "weavec.h", "algorithm", "cstdarg", "cstdio", "cstdlib", "cstring", "functional", "initializer_list", "map",
        "memory", "optional", "span", "string", "string_view", "unordered_map", "utility", "variant", "vector"))
    weavec_cxx = Path(weavec_cc).with_name("weavec-c++")
    checked = 0
    checked_cxx = 0
    for section in rules.read_text().split("\n## ")[1:]:
        rule = section.split("\n", 1)[0]
        for number, match in enumerate(re.finditer(r"```c (accepted|rejected)\n(.*?)```", section, re.S)):
            source = temp / f"rule-{rule}-{number}.c"
            source.write_text(prelude + match.group(2))
            compiled = run([weavec_cc, "-fsyntax-only", "-fweavec-safe", source])
            if match.group(1) == "accepted":
                assert compiled.returncode == 0, f"{rule} accepted example {number}:\n{compiled.stderr}"
            else:
                assert compiled.returncode != 0 and f"[weavec-safe:{rule}]" in compiled.stderr, \
                    f"{rule} rejected example {number}:\n{compiled.stderr}"
            checked += 1
        # RFC 0044 section 13: the C++ examples, with weavec-c++.
        for number, match in enumerate(re.finditer(r"```cpp (accepted|rejected)\n(.*?)```", section, re.S)):
            source = temp / f"rule-{rule}-{number}.cpp"
            source.write_text(cxx_prelude + match.group(2))
            compiled = run([weavec_cxx, "-std=c++20", "-fsyntax-only", "-w", "-fweavec-safe", source])
            if match.group(1) == "accepted":
                assert compiled.returncode == 0, f"{rule} accepted C++ example {number}:\n{compiled.stderr}"
            else:
                assert compiled.returncode != 0 and f"[weavec-safe:{rule}]" in compiled.stderr, \
                    f"{rule} rejected C++ example {number}:\n{compiled.stderr}"
            checked_cxx += 1
    assert checked >= 30, checked
    assert checked_cxx >= 30, checked_cxx
    # RFC 0044 section 13: the guide's building blocks.
    guide = examples.parent / "pages" / "guides" / "safe-cxx.md"
    blocks = re.findall(r"```cpp accepted\n(.*?)```", guide.read_text(), re.S)
    assert len(blocks) >= 9, len(blocks)
    for number, block in enumerate(blocks):
        source = temp / f"guide-{number}.cpp"
        source.write_text(cxx_prelude + block)
        compiled = run([weavec_cxx, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror", "-fweavec-safe",
                        source])
        assert compiled.returncode == 0, f"the C++ guide's block {number}:\n{compiled.stderr}"

print("Documentation examples passed: every example compiles, the guarded load "
      "in the ledger, the index and heap reports, and the rules' examples.")
