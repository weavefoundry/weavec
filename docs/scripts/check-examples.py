#!/usr/bin/env python3
"""Exercise the published tutorial examples against built WeaveC binaries.

`weavec` must report the use-after-free in use-after-free.c and accept fixed.c
(RFC 0035 section 8: the advisory analysis). `weavec-cc` must compile
lookup.c, record the guarded load of `table[i]` in the enforcement ledger
(`weavec-ledger` version 3, RFC 0035 section 9), link it, run it, and stop a
negative index with an `index-out-of-bounds` report; and it must build vec.c
and stop a read past its heap block with a `heap-buffer-overflow` report
(RFC 0035 section 5.3; the default `-fweavec-checks=trap`).
"""

import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--weavec", required=True, type=Path, help="the weavec analysis tool")
parser.add_argument(
    "--weavec-cc", type=Path, help="the weavec-cc compiler (default: next to --weavec)"
)
args = parser.parse_args()
weavec = args.weavec.resolve()
weavec_cc = (args.weavec_cc or weavec.with_name("weavec-cc")).resolve()
examples = Path(__file__).resolve().parent.parent / "examples"
traps = {-signal.SIGTRAP, -signal.SIGILL}


def run(command, **options):
    return subprocess.run(
        [str(part) for part in command],
        capture_output=True, text=True, timeout=60, check=False, **options,
    )


def analyse(source):
    return run([weavec, source, "--", "-std=c17"])


def analysis_summary(stderr, name):
    """The analysis's summary line (RFC 0035 section 8) for source `name`."""
    return re.search(rf"^weavec: (\S*/)?{re.escape(name)}: [\d,]+ sites?: ", stderr, re.M)


def enforcement_summary(stderr, name):
    """The enforcement summary line (RFC 0035 section 9) for source `name`."""
    pattern = rf"^weavec: (\S*/)?{re.escape(name)}: \d+ accesses: \d+ proven, \d+ guarded, \d+ unguarded$"
    return re.search(pattern, stderr, re.M)


def expect_trap(result, kind, name, line, detail):
    """A trap whose first report is `weavec: <kind> at <name>:<line>:<col>: <detail>`."""
    assert result.returncode in traps, f"expected a trap, got exit status {result.returncode}"
    pattern = rf"^weavec: {re.escape(kind)} at (\S*/)?{re.escape(name)}:{line}:\d+: {detail}"
    assert re.search(pattern, result.stderr, re.M), "unexpected report:\n" + result.stderr


bad = analyse(examples / "use-after-free.c")
assert bad.returncode != 0 and "[weavec::use-after-free]" in bad.stderr, bad.stderr
good = analyse(examples / "fixed.c")
assert good.returncode == 0, good.stderr
assert analysis_summary(good.stderr, "fixed.c"), "no summary line:\n" + good.stderr

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    ledgers = temp / "ledger"
    ledgers.mkdir()
    obj = temp / "lookup.o"
    compiled = run([weavec_cc, "-std=c17", f"-fweavec-ledger={ledgers}/", "-c",
                    examples / "lookup.c", "-o", obj])
    assert compiled.returncode == 0, compiled.stderr
    assert enforcement_summary(compiled.stderr, "lookup.c"), "no summary line:\n" + compiled.stderr

    ledger = json.loads((ledgers / "lookup.o.ledger.json").read_text())
    assert ledger["schema"] == "weavec-ledger" and ledger["version"] == 3, ledger
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
    assert re.search(r"^weavec: 0x[0-9a-f]+ is 0 bytes after the 16-byte heap object at 0x[0-9a-f]+$",
                     past.stderr, re.M), "no heap object line:\n" + past.stderr

print("Documentation examples passed: use-after-free, fixed lifetime, the guarded "
      "load in the ledger, and the index and heap reports.")
