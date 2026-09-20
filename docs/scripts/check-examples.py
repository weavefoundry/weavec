#!/usr/bin/env python3
"""Exercise the published tutorial examples against built WeaveC binaries.

`weavec` must report the use-after-free in use-after-free.c and accept fixed.c.
`weavec-cc` must compile lookup.c, record its unproven index as a checked
`index` facet in the unit ledger, link it, run it, and trap when the index is
negative (RFC 0030: the default `-fweavec-checks=trap`).
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


def run(command, **options):
    return subprocess.run(
        [str(part) for part in command],
        capture_output=True, text=True, timeout=60, check=False, **options,
    )


def analyse(source):
    return run([weavec, source, "--", "-std=c17"])


def summary_line(stderr, name):
    """The one-line ledger summary (RFC 0030 section 12.4) for source `name`."""
    return re.search(rf"^weavec: (\S*/)?{re.escape(name)}: [\d,]+ sites?: ", stderr, re.M)


bad = analyse(examples / "use-after-free.c")
assert bad.returncode != 0 and "[weavec::use-after-free]" in bad.stderr, bad.stderr
good = analyse(examples / "fixed.c")
assert good.returncode == 0, good.stderr
assert summary_line(good.stderr, "fixed.c"), "no summary line:\n" + good.stderr

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    ledgers = temp / "ledger"
    ledgers.mkdir()
    obj = temp / "lookup.o"
    compiled = run([weavec_cc, "-std=c17", f"-fweavec-ledger={ledgers}/", "-c",
                    examples / "lookup.c", "-o", obj])
    assert compiled.returncode == 0, compiled.stderr
    assert summary_line(compiled.stderr, "lookup.c"), "no summary line:\n" + compiled.stderr

    ledger = json.loads((ledgers / "lookup.o.ledger.json").read_text())
    assert ledger["schema"] == "weavec-ledger" and ledger["scope"] == "unit", ledger
    sites = [site for unit in ledger["units"] for function in unit["functions"]
             for site in function["sites"] if site["text"] == "table[i]" and site["kind"] == "index"]
    assert sites, "no ledger row for table[i]"
    spatial = sites[0]["facets"]["spatial"]
    assert spatial["outcome"] == "checked" and spatial["check"]["template"] == "index", spatial

    program = temp / "lookup"
    linked = run([weavec_cc, obj, "-o", program])
    assert linked.returncode == 0, linked.stderr
    inside = run([program, "2"])
    assert inside.returncode == 0 and inside.stdout == "30\n", (inside.returncode, inside.stdout)
    below = run([program, "-1"])
    traps = {-signal.SIGTRAP, -signal.SIGILL}
    assert below.returncode in traps, f"expected a trap, got exit status {below.returncode}"

print("Documentation examples passed: use-after-free, fixed lifetime, "
      "checked index in the ledger, and its trap.")
