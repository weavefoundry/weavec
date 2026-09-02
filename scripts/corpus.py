#!/usr/bin/env python3
"""Run weavec over a corpus of real C projects and tally what it reports.

The corpus is the empirical side of the RFCs: every "deferred to corpus
testing" question in docs/rfcs/ is answered by looking at these numbers. The
harness clones (shallow) each project listed in scripts/corpus/projects.json,
runs weavec on the listed translation units, parses the diagnostics and
prints a table of counts per diagnostic id along with analysis time. Results
are written as JSON so runs can be diffed; `--baseline` compares against a
previous run and exits non-zero when a diagnostic id's count grows.

Examples:

  scripts/corpus.py --weavec build/dev/bin/weavec
  scripts/corpus.py --weavec build/dev/bin/weavec --only sds --show use-after-free
  scripts/corpus.py --weavec build/dev/bin/weavec --baseline scripts/corpus/baseline.json
  scripts/corpus.py --weavec build/dev/bin/weavec --local path/to/project --local-args -Iinclude

Only the standard library is used so the script runs anywhere weavec does.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import glob
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / "scripts" / "corpus" / "projects.json"
DEFAULT_WORKDIR = ROOT / "build" / "corpus"

# `file:line:col: severity: message [weavec::id]` as printed by the tool.
DIAG_RE = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<severity>error|warning): (?P<message>.*) \[weavec::(?P<id>[a-z-]+)\]$"
)
# Anything Clang itself reports (parse errors, missing headers) has no
# `[weavec::...]` tag; it is counted separately so broken setups are visible.
CLANG_DIAG_RE = re.compile(r"^(?P<file>[^:\n]+):\d+:\d+: (?P<severity>error|fatal error): ")


@dataclasses.dataclass
class Project:
    name: str
    url: str | None
    ref: str
    files: list[str]
    args: list[str]
    notes: str = ""
    path: Path | None = None

    @staticmethod
    def from_json(obj: dict) -> "Project":
        return Project(
            name=obj["name"],
            url=obj.get("url"),
            ref=obj.get("ref", "HEAD"),
            files=list(obj["files"]),
            args=list(obj.get("args", [])),
            notes=obj.get("notes", ""),
        )


@dataclasses.dataclass
class Diagnostic:
    project: str
    file: str
    line: int
    col: int
    severity: str
    id: str
    message: str

    def render(self) -> str:
        return f"{self.file}:{self.line}:{self.col}: {self.severity}: {self.message} [{self.id}]"


@dataclasses.dataclass
class UnitResult:
    project: str
    file: str
    seconds: float
    exit_code: int
    diagnostics: list[Diagnostic]
    clang_errors: int


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# -- checkout -----------------------------------------------------------------


def checkout(project: Project, workdir: Path, refresh: bool) -> Path:
    if project.path is not None:
        return project.path
    assert project.url is not None
    dest = workdir / project.name
    if dest.exists() and not refresh:
        return dest
    if dest.exists():
        subprocess.run(["git", "-C", str(dest), "fetch", "--depth", "1", "origin", project.ref], check=True)
        subprocess.run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], check=True)
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    log(f"[{project.name}] cloning {project.url} @ {project.ref}")
    subprocess.run(
        ["git", "clone", "--quiet", "--depth", "1", "--branch", project.ref, project.url, str(dest)],
        check=True,
    )
    return dest


def resolved_commit(path: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


# -- running ------------------------------------------------------------------


def expand_files(root: Path, patterns: Iterable[str]) -> list[Path]:
    files: list[Path] = []
    for pattern in patterns:
        matches = sorted(glob.glob(str(root / pattern), recursive=True))
        if not matches:
            log(f"warning: pattern {pattern!r} matched nothing under {root}")
        files.extend(Path(m) for m in matches)
    return files


def parse_output(project: str, root: Path, text: str) -> tuple[list[Diagnostic], int]:
    diagnostics: list[Diagnostic] = []
    clang_errors = 0
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if m:
            file = m.group("file")
            try:
                file = str(Path(file).resolve().relative_to(root.resolve()))
            except ValueError:
                pass
            diagnostics.append(
                Diagnostic(
                    project=project,
                    file=file,
                    line=int(m.group("line")),
                    col=int(m.group("col")),
                    severity=m.group("severity"),
                    id=m.group("id"),
                    message=m.group("message"),
                )
            )
            continue
        if CLANG_DIAG_RE.match(line):
            clang_errors += 1
    return diagnostics, clang_errors


def run_unit(weavec: str, project: Project, root: Path, file: Path, extra: list[str]) -> UnitResult:
    cmd = [weavec, *extra, str(file), "--", *project.args]
    start = time.perf_counter()
    proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    seconds = time.perf_counter() - start
    diagnostics, clang_errors = parse_output(project.name, root, proc.stderr + proc.stdout)
    return UnitResult(
        project=project.name,
        file=str(file.relative_to(root)),
        seconds=seconds,
        exit_code=proc.returncode,
        diagnostics=diagnostics,
        clang_errors=clang_errors,
    )


# -- reporting ----------------------------------------------------------------


def summarise(results: list[UnitResult]) -> dict:
    per_project: dict[str, dict] = {}
    totals: collections.Counter[str] = collections.Counter()
    for r in results:
        entry = per_project.setdefault(
            r.project,
            {"units": 0, "seconds": 0.0, "clang_errors": 0, "by_id": collections.Counter()},
        )
        entry["units"] += 1
        entry["seconds"] += r.seconds
        entry["clang_errors"] += r.clang_errors
        for d in r.diagnostics:
            entry["by_id"][d.id] += 1
            totals[d.id] += 1
    for entry in per_project.values():
        entry["by_id"] = dict(sorted(entry["by_id"].items()))
        entry["seconds"] = round(entry["seconds"], 3)
    return {"projects": per_project, "totals": dict(sorted(totals.items()))}


def print_table(summary: dict, commits: dict[str, str]) -> None:
    ids = sorted(summary["totals"])
    name_w = max([len("project"), *(len(p) for p in summary["projects"])])
    header = f"{'project':<{name_w}}  {'commit':<8} {'units':>5} {'time':>7} {'clang':>5}  " + "  ".join(ids)
    print(header)
    print("-" * len(header))
    for name, entry in summary["projects"].items():
        cells = "  ".join(f"{entry['by_id'].get(i, 0):>{len(i)}}" for i in ids)
        print(
            f"{name:<{name_w}}  {commits.get(name, '-'):<8} {entry['units']:>5} "
            f"{entry['seconds']:>6.2f}s {entry['clang_errors']:>5}  {cells}"
        )
    print("-" * len(header))
    cells = "  ".join(f"{summary['totals'].get(i, 0):>{len(i)}}" for i in ids)
    units = sum(e["units"] for e in summary["projects"].values())
    seconds = sum(e["seconds"] for e in summary["projects"].values())
    clang = sum(e["clang_errors"] for e in summary["projects"].values())
    print(f"{'total':<{name_w}}  {'':<8} {units:>5} {seconds:>6.2f}s {clang:>5}  {cells}")


def compare_to_baseline(summary: dict, baseline: dict) -> int:
    """Print per-id deltas; return 1 if any id grew (or a project vanished)."""
    regressed = 0
    base_totals = baseline.get("summary", baseline).get("totals", {})
    all_ids = sorted(set(summary["totals"]) | set(base_totals))
    print()
    print("baseline comparison:")
    for id_ in all_ids:
        now = summary["totals"].get(id_, 0)
        before = base_totals.get(id_, 0)
        marker = ""
        if now > before:
            marker = "  <-- regression"
            regressed = 1
        elif now < before:
            marker = "  (improved)"
        print(f"  {id_:<24} {before:>6} -> {now:<6}{marker}")
    return regressed


# -- main ---------------------------------------------------------------------


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weavec", default=os.environ.get("WEAVEC", "weavec"), help="weavec binary")
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="projects.json")
    ap.add_argument("--workdir", type=Path, default=DEFAULT_WORKDIR, help="where projects are cloned")
    ap.add_argument("--only", action="append", default=[], help="run only this project (repeatable)")
    ap.add_argument("--refresh", action="store_true", help="re-fetch already cloned projects")
    ap.add_argument("--local", type=Path, help="analyse a local directory instead of the manifest")
    ap.add_argument("--local-files", default="**/*.c", help="glob for --local (default: **/*.c)")
    ap.add_argument("--local-args", nargs=argparse.REMAINDER, default=[], help="compiler args for --local")
    ap.add_argument("--weavec-arg", action="append", default=[], help="extra weavec option (repeatable)")
    ap.add_argument("--show", action="append", default=[], help="print every diagnostic with this id")
    ap.add_argument("--show-all", action="store_true", help="print every diagnostic")
    ap.add_argument("--json", type=Path, help="write full results here")
    ap.add_argument("--baseline", type=Path, help="compare against a previous --json output")
    ap.add_argument("--update-baseline", action="store_true", help="overwrite --baseline with this run")
    args = ap.parse_args(argv)
    # Units run with cwd set to the project checkout; keep a relative binary
    # path meaningful.
    if os.sep in args.weavec:
        args.weavec = str(Path(args.weavec).resolve())

    if args.local:
        projects = [
            Project(
                name=args.local.name,
                url=None,
                ref="local",
                files=[args.local_files],
                args=args.local_args,
                path=args.local.resolve(),
            )
        ]
    else:
        manifest = json.loads(args.manifest.read_text())
        projects = [Project.from_json(p) for p in manifest["projects"]]
        if args.only:
            projects = [p for p in projects if p.name in args.only]
            missing = set(args.only) - {p.name for p in projects}
            if missing:
                log(f"error: unknown project(s): {', '.join(sorted(missing))}")
                return 2

    results: list[UnitResult] = []
    commits: dict[str, str] = {}
    for project in projects:
        try:
            root = checkout(project, args.workdir, args.refresh)
        except subprocess.CalledProcessError as e:
            log(f"[{project.name}] checkout failed: {e}")
            return 2
        commits[project.name] = resolved_commit(root)
        files = expand_files(root, project.files)
        log(f"[{project.name}] {len(files)} translation unit(s)")
        for file in files:
            unit = run_unit(args.weavec, project, root, file, args.weavec_arg)
            results.append(unit)
            if unit.clang_errors:
                log(f"[{project.name}] {unit.file}: {unit.clang_errors} clang error(s); check args")

    summary = summarise(results)
    print_table(summary, commits)

    shown = set(args.show)
    if shown or args.show_all:
        print()
        for r in results:
            for d in r.diagnostics:
                if args.show_all or d.id in shown:
                    print(f"[{d.project}] {d.render()}")

    payload = {
        "weavec": args.weavec,
        "commits": commits,
        "summary": summary,
        "units": [
            {
                "project": r.project,
                "file": r.file,
                "seconds": round(r.seconds, 3),
                "exit_code": r.exit_code,
                "clang_errors": r.clang_errors,
                "diagnostics": [dataclasses.asdict(d) for d in r.diagnostics],
            }
            for r in results
        ],
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2) + "\n")
        log(f"wrote {args.json}")

    status = 0
    if args.baseline and args.baseline.exists() and not args.update_baseline:
        status = compare_to_baseline(summary, json.loads(args.baseline.read_text()))
    if args.baseline and args.update_baseline:
        # Timings are not part of the contract; keep the baseline diff-stable.
        stable = json.loads(json.dumps(summary))
        for entry in stable["projects"].values():
            entry.pop("seconds", None)
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        args.baseline.write_text(json.dumps({"commits": commits, "summary": stable}, indent=2) + "\n")
        log(f"updated {args.baseline}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
