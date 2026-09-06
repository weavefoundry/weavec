#!/usr/bin/env python3
"""Evaluate a fixed set of good programs, expected bugs, and known misses.

Unlike recall.py's regression pins, every bug in the manifest stays in the
recall denominator, including unsupported cases. A newly detected known miss
is an improvement. Parse failures, tool failures, and timeouts are never clean
results. RFC 0013 specifies the contract; test/evaluation/README.md documents
how to add and review cases.
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / "test/evaluation/manifest.json"
MARKER = re.compile(r"//\s*BUG:\s*([a-z0-9_-]+)")
DIAGNOSTIC = re.compile(
    r"^(.*?):(\d+):(\d+): (warning|error): (.*?) \[weavec::([a-z-]+)\]$"
)
CLANG_ERROR = re.compile(r"(?:^|: )(?:fatal )?error:")


@dataclasses.dataclass(frozen=True)
class Bug:
    marker: str
    diagnostic: str
    file: Path
    line: int
    required: bool


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    files: tuple[Path, ...]
    bugs: tuple[Bug, ...]
    whole_program: bool
    arguments: tuple[str, ...]


def load_manifest(path: Path) -> list[Case]:
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.get("version") != 1 or not isinstance(data.get("cases"), list):
        raise ValueError("expected evaluation manifest version 1 with a cases array")
    if not data["cases"]:
        raise ValueError("the evaluation set must not be empty")
    cases: list[Case] = []
    names: set[str] = set()
    for entry in data["cases"]:
        name = entry["name"]
        if not isinstance(name, str) or not name or name in names:
            raise ValueError(f"invalid or duplicate case name: {name!r}")
        names.add(name)
        files = tuple((path.parent / source).resolve() for source in entry["sources"])
        if not files or len(set(files)) != len(files):
            raise ValueError(f"{name}: expected distinct source files")
        markers: dict[str, tuple[Path, int]] = {}
        for source in files:
            for line, text in enumerate(source.read_text().splitlines(), 1):
                for marker in MARKER.findall(text):
                    if marker in markers:
                        raise ValueError(f"{name}: duplicate BUG marker {marker}")
                    markers[marker] = (source, line)
        bugs: list[Bug] = []
        declared: set[str] = set()
        for expected in entry.get("bugs", []):
            marker = expected["marker"]
            if marker in declared or marker not in markers:
                raise ValueError(f"{name}: missing or duplicate bug {marker}")
            declared.add(marker)
            required = expected.get("required", True)
            if not isinstance(required, bool):
                raise ValueError(f"{name}: required must be a boolean")
            diagnostic = expected["diagnostic"]
            if not isinstance(diagnostic, str) or not re.fullmatch(r"[a-z-]+", diagnostic):
                raise ValueError(f"{name}: invalid diagnostic id")
            source, line = markers[marker]
            bugs.append(Bug(marker, diagnostic, source, line, required))
        if declared != markers.keys():
            raise ValueError(f"{name}: every BUG marker must remain in the manifest")
        whole_program = entry.get("whole_program", len(files) > 1)
        if not isinstance(whole_program, bool) or (len(files) > 1 and not whole_program):
            raise ValueError(f"{name}: multiple sources require whole_program")
        arguments = entry.get("arguments", [])
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            raise ValueError(f"{name}: arguments must be strings")
        cases.append(Case(name, files, tuple(bugs), whole_program, tuple(arguments)))
    return cases


def classify(case: Case, stdout: str, stderr: str, returncode: int) -> dict:
    reports: list[dict] = []
    parse_errors: list[str] = []
    for line in (stdout + "\n" + stderr).splitlines():
        if match := DIAGNOSTIC.match(line):
            file, row, column, severity, message, diagnostic = match.groups()
            reports.append({"file": str(Path(file).resolve()), "line": int(row),
                            "column": int(column), "severity": severity,
                            "diagnostic": diagnostic, "message": message})
        elif CLANG_ERROR.search(line):
            parse_errors.append(line)
    # Exit 1 is normal when the checker finds an error. Any other nonzero
    # status, or failure with no reported diagnostic, is an execution failure.
    failure = returncode != 0 and (returncode != 1 or not any(
        r["severity"] == "error" for r in reports)) and not parse_errors
    valid = not parse_errors and not failure
    expected = {(str(b.file.resolve()), b.line, b.diagnostic): b for b in case.bugs}
    observed = {(r["file"], r["line"], r["diagnostic"]) for r in reports}
    detected = [b.marker for key, b in expected.items() if valid and key in observed]
    missed = [b.marker for b in case.bugs if b.marker not in detected]
    unexpected = [r for r in reports if (r["file"], r["line"], r["diagnostic"]) not in expected]
    required_misses = [b.marker for b in case.bugs if b.required and b.marker in missed]
    return {"name": case.name, "bugs": len(case.bugs), "detected": detected,
            "missed": missed, "required_misses": required_misses,
            "unexpected": unexpected, "parse_errors": parse_errors,
            "tool_failure": failure, "timeout": False, "returncode": returncode,
            "ok": valid and not unexpected and not required_misses}


def run_case(case: Case, weavec: Path, timeout: float) -> dict:
    command = [str(weavec)]
    if case.whole_program:
        command.append("--whole-program")
    command.extend(str(source) for source in case.files)
    command.extend(["--", "-ferror-limit=0", "-fno-color-diagnostics",
                    f"-I{ROOT / 'resources/include'}", *case.arguments])
    start = time.perf_counter()
    try:
        completed = subprocess.run(command, capture_output=True, text=True,
                                   timeout=timeout, check=False)
        result = classify(case, completed.stdout, completed.stderr, completed.returncode)
        if result["tool_failure"] or result["parse_errors"]:
            result["output"] = completed.stdout + completed.stderr
    except subprocess.TimeoutExpired:
        result = classify(case, "", "", 1)
        result.update(timeout=True, tool_failure=False, ok=False)
    except OSError as error:
        result = classify(case, "", "", 1)
        result["output"] = str(error)
    result["seconds"] = round(time.perf_counter() - start, 4)
    return result


def summarize(results: list[dict]) -> dict:
    counts = collections.Counter()
    for result in results:
        counts["cases"] += 1
        counts["bugs"] += result["bugs"]
        counts["detected"] += len(result["detected"])
        counts["missed"] += len(result["missed"])
        counts["required_misses"] += len(result["required_misses"])
        counts["unexpected_reports"] += len(result["unexpected"])
        counts["parse_failures"] += bool(result["parse_errors"])
        counts["tool_failures"] += result["tool_failure"]
        counts["timeouts"] += result["timeout"]
        counts["clean_good_cases"] += result["bugs"] == 0 and result["ok"]
        counts["good_cases"] += result["bugs"] == 0
    return dict(counts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weavec", type=Path, default=ROOT / "build/dev/bin/weavec")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--json", type=Path, help="write full machine-readable results")
    parser.add_argument("--only", help="select case names containing this text (denominator is filtered)")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        cases = load_manifest(args.manifest.resolve())
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f"invalid evaluation manifest: {error}", file=sys.stderr)
        return 2
    if args.only:
        cases = [case for case in cases if args.only in case.name]
        if not cases:
            parser.error("--only selected no cases")
    results = []
    for case in cases:
        result = run_case(case, args.weavec.resolve(), args.timeout)
        results.append(result)
        status = "PASS" if result["ok"] else "FAIL"
        if result["timeout"]:
            detail = "timeout"
        elif result["tool_failure"]:
            detail = "tool failure"
        elif result["parse_errors"]:
            detail = "parse failure"
        else:
            detail = f"{len(result['detected'])}/{result['bugs']} bugs detected"
        print(f"{status:4} {case.name}: {detail}")
        for report in result["unexpected"]:
            print(f"     unexpected {report['diagnostic']} at {report['file']}:{report['line']}: {report['message']}")
        for missing in result["required_misses"]:
            print(f"     required bug missed: {missing}")
    totals = summarize(results)
    print(f"\nDetected {totals['detected']}/{totals['bugs']} fixed bugs; "
          f"missed {totals['missed']} ({totals['required_misses']} required).")
    print(f"Clean good cases {totals['clean_good_cases']}/{totals['good_cases']}; "
          f"unexpected reports {totals['unexpected_reports']}; "
          f"parse failures {totals['parse_failures']}; "
          f"tool failures {totals['tool_failures']}; timeouts {totals['timeouts']}.")
    if args.json:
        args.json.write_text(json.dumps({"manifest": str(args.manifest.resolve()),
                                        "filtered": args.only, "summary": totals,
                                        "cases": results}, indent=2) + "\n")
    return 0 if all(result["ok"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
