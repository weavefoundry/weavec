#!/usr/bin/env python3
"""Exercise the published tutorial examples against a built WeaveC binary."""

import argparse
import re
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--weavec", required=True, type=Path)
args = parser.parse_args()
binary = args.weavec.resolve()
examples = Path(__file__).resolve().parent.parent / "examples"

def run(source, options=()):
    return subprocess.run(
        [str(binary), *options, str(source), "--", "-std=c17"],
        capture_output=True, text=True, timeout=30, check=False,
    )

bad = run(examples / "use-after-free.c")
assert bad.returncode != 0 and "[weavec::use-after-free]" in bad.stderr, bad.stderr
good = run(examples / "fixed.c")
assert good.returncode == 0, good.stderr
with tempfile.TemporaryDirectory() as temp:
    report = Path(temp) / "report.json"
    checked = run(examples / "checked.c", ("--checked-function=get", f"--checked-report={report}"))
    assert checked.returncode == 0 and report.is_file(), checked.stderr
    unguarded = Path(temp) / "unguarded.c"
    unguarded.write_text(re.sub(r"\s*if \(index >= 4\)\s*return 0;", "", (examples / "checked.c").read_text()))
    incomplete = run(unguarded, ("--checked-function=get",))
    assert incomplete.returncode != 0 and "[weavec::checking-incomplete]" in incomplete.stderr, incomplete.stderr
print("Documentation examples passed: use-after-free, fixed lifetime, checked bounds, and missing guard.")
