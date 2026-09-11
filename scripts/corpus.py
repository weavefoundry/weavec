#!/usr/bin/env python3
"""Run weavec over a corpus of real C projects and tally what it reports.

The corpus is the empirical side of the RFCs: every "deferred to corpus
testing" question in docs/rfcs/ is answered by looking at these numbers. The
harness clones (shallow) each project listed in scripts/corpus/projects.json,
runs weavec on the listed translation units (one at a time, or as one
program with `"whole_program": true`), parses the diagnostics and prints a
table of counts per diagnostic id along with analysis time. Results
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
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / "scripts" / "corpus" / "projects.json"
DEFAULT_WORKDIR = ROOT / "build" / "corpus"
# Files a checkout needs that its build would generate (a configured header,
# say), kept per project; `{support}` in a project's `args` expands to its
# directory.
SUPPORT_DIR = ROOT / "scripts" / "corpus" / "support"

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
    # Analyse every file as one program (`weavec --whole-program`, RFC 0005)
    # instead of one translation unit at a time.
    whole_program: bool = False
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
            whole_program=bool(obj.get("whole_program", False)),
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
    failure: str = ""
    peak_rss_bytes: int | None = None


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# -- checkout -----------------------------------------------------------------


def checkout(project: Project, workdir: Path, refresh: bool) -> Path:
    if project.path is not None:
        return project.path
    assert project.url is not None
    dest = workdir / project.name
    if dest.exists() and not refresh:
        if re.fullmatch(r"[0-9a-f]{7,40}", project.ref) and not resolved_commit(dest).startswith(project.ref):
            raise ValueError(f"{project.name}: checkout does not match pinned revision {project.ref}")
        return dest
    if dest.exists():
        subprocess.run(["git", "-C", str(dest), "fetch", "--depth", "1", "origin", project.ref], check=True)
        subprocess.run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], check=True)
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    log(f"[{project.name}] cloning {project.url} @ {project.ref}")
    if re.fullmatch(r"[0-9a-f]{40}", project.ref):
        subprocess.run(["git", "init", "--quiet", str(dest)], check=True)
        subprocess.run(["git", "-C", str(dest), "remote", "add", "origin", project.url], check=True)
        subprocess.run(["git", "-C", str(dest), "fetch", "--depth", "1", "origin", project.ref], check=True)
        subprocess.run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], check=True)
    else:
        subprocess.run(
            ["git", "clone", "--quiet", "--depth", "1", "--branch", project.ref, project.url, str(dest)],
            check=True,
        )
    return dest


def resolved_commit(path: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
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


def run_units(weavec: str, project: Project, root: Path, files: list[Path], extra: list[str],
              timeout: float = 600, measure_memory: bool = False) -> UnitResult:
    """Run weavec once over `files`: one unit, or a whole program."""
    cmd = [weavec, *extra]
    if project.whole_program:
        cmd.append("--whole-program")
    cmd.extend(str(f) for f in files)
    # Clang stops after 20 errors per unit by default; a tally that is capped
    # per unit cannot be compared between runs (Lua's lstrlib.c hits the cap).
    support = str(SUPPORT_DIR / project.name)
    cmd.extend(["--", "-ferror-limit=0", *(a.replace("{support}", support) for a in project.args)])
    if measure_memory:
        if sys.platform != "darwin" and not sys.platform.startswith("linux"):
            raise ValueError("memory measurement requires macOS or Linux resource accounting")
        cmd = [sys.executable, str(Path(__file__).resolve()), "--measure-child", *cmd]
    start = time.perf_counter()
    failure = ""
    output = ""
    peak_rss = None
    try:
        with subprocess.Popen(cmd, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, start_new_session=True) as proc:
            try:
                stdout, stderr = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                failure = f"timeout after {timeout:g} seconds"
                os.killpg(proc.pid, signal.SIGKILL)
                stdout, stderr = proc.communicate()
            except KeyboardInterrupt:
                # The checker has its own session, so terminal cancellation
                # reaches this runner but not the checker or RSS wrapper.
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                raise
            output = stderr + stdout
            exit_code = proc.returncode
    except OSError as exc:
        failure = str(exc)
        exit_code = 127
    seconds = time.perf_counter() - start
    diagnostics, clang_errors = parse_output(project.name, root, output)
    if not failure:
        if clang_errors:
            failure = f"{clang_errors} Clang parse error(s)"
        elif exit_code < 0 or exit_code > 1:
            failure = f"checker exited with status {exit_code}"
        elif exit_code and not any(d.severity == "error" for d in diagnostics):
            failure = f"checker failed without a WeaveC error (status {exit_code})"
        elif "non-converg" in output or "failed to converge" in output or "did not converge" in output:
            failure = "program analysis did not converge"
        elif any(d.id == "analysis-incomplete" and "iteration limit reached" in d.message
                 for d in diagnostics):
            failure = "analysis reached an iteration limit"
    if measure_memory:
        match = re.search(r"^weavec-corpus-rss-bytes: (\d+)$", output, re.MULTILINE)
        if match:
            peak_rss = int(match[1])
        if peak_rss is None and not failure:
            failure = "peak memory was requested but not reported"

    return UnitResult(
        project=project.name,
        file=" ".join(str(f.relative_to(root)) for f in files),
        seconds=seconds,
        exit_code=exit_code,
        diagnostics=diagnostics,
        clang_errors=clang_errors,
        failure=failure,
        peak_rss_bytes=peak_rss,
    )


# -- reporting ----------------------------------------------------------------


def summarise(results: list[UnitResult]) -> dict:
    per_project: dict[str, dict] = {}
    totals: collections.Counter[str] = collections.Counter()
    for r in results:
        entry = per_project.setdefault(
            r.project,
            {"units": 0, "seconds": 0.0, "clang_errors": 0, "failures": 0, "peak_rss_bytes": None, "by_id": collections.Counter()},
        )
        entry["units"] += 1
        entry["seconds"] += r.seconds
        entry["clang_errors"] += r.clang_errors
        entry["failures"] += bool(r.failure)
        if r.peak_rss_bytes is not None:
            entry["peak_rss_bytes"] = max(entry["peak_rss_bytes"] or 0, r.peak_rss_bytes)
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
            f"{name:<{name_w}}  {commits.get(name, '-')[:8]:<8} {entry['units']:>5} "
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
    base_projects = baseline.get("summary", baseline).get("projects", {})
    for project, before in base_projects.items():
        now = summary["projects"].get(project)
        if now is None or now["units"] != before["units"]:
            print(f"  {project}: project missing or unit count changed")
            regressed = 1
        elif any(now["by_id"].get(id_, 0) > count for id_, count in before.get("by_id", {}).items()) or any(
            count > before.get("by_id", {}).get(id_, 0) for id_, count in now["by_id"].items()
        ):
            print(f"  {project}: one or more diagnostic counts grew")
            regressed = 1
    if any(entry.get("failures", 0) for entry in summary["projects"].values()):
        print("  analysis failures prevent a clean comparison")
        regressed = 1
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
    ap.add_argument("--timeout", type=float, default=600, help="seconds allowed per checker process")
    ap.add_argument("--measure-memory", action="store_true", help="record per-process peak RSS via child resource accounting")
    ap.add_argument("--refresh", action="store_true", help="re-fetch already cloned projects")
    ap.add_argument("--local", type=Path, help="analyse a local directory instead of the manifest")
    ap.add_argument("--local-files", default="**/*.c", help="glob for --local (default: **/*.c)")
    ap.add_argument("--local-whole-program", action="store_true", help="analyse --local files as one program")
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
                whole_program=args.local_whole_program,
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
        except (subprocess.CalledProcessError, ValueError) as e:
            log(f"[{project.name}] checkout failed: {e}")
            return 2
        commits[project.name] = resolved_commit(root)
        files = expand_files(root, project.files)
        if not files:
            log(f"[{project.name}] no translation units matched; refusing an empty result")
            return 2
        mode = "as one program" if project.whole_program else "one at a time"
        log(f"[{project.name}] {len(files)} translation unit(s), {mode}")
        groups = [files] if project.whole_program and files else [[f] for f in files]
        for group in groups:
            unit = run_units(args.weavec, project, root, group, args.weavec_arg, args.timeout, args.measure_memory)
            results.append(unit)
            if unit.failure:
                log(f"[{project.name}] {unit.file}: {unit.failure}")
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
                "failure": r.failure,
                "peak_rss_bytes": r.peak_rss_bytes,
                "diagnostics": [dataclasses.asdict(d) for d in r.diagnostics],
            }
            for r in results
        ],
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2) + "\n")
        log(f"wrote {args.json}")

    status = 2 if any(r.failure for r in results) else 0
    if args.baseline and args.baseline.exists() and not args.update_baseline:
        baseline = json.loads(args.baseline.read_text())
        for project, commit in baseline.get("commits", {}).items():
            if project not in commits or not commits[project].startswith(commit):
                log(f"error: {project}: source revision differs from baseline")
                status = 2
        status = max(status, compare_to_baseline(summary, baseline))
    if args.baseline and args.update_baseline and status == 0:
        # Timings are not part of the contract; keep the baseline diff-stable.
        stable = json.loads(json.dumps(summary))
        for entry in stable["projects"].values():
            entry.pop("seconds", None)
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        args.baseline.write_text(json.dumps({"commits": commits, "summary": stable}, indent=2) + "\n")
        log(f"updated {args.baseline}")
    return status


def measured_child(command: list[str]) -> int:
    """Account for one checker in a fresh process, excluding previous units.

    The checker inherits the wrapper's process group, so timeout cancellation
    still kills both. Unlike macOS time(1), getrusage needs no sysctl access.
    """
    import resource

    try:
        status = subprocess.call(command)
    except OSError as exc:
        log(str(exc))
        return 127
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if sys.platform != "darwin":
        peak *= 1024
    log(f"weavec-corpus-rss-bytes: {int(peak)}")
    return 128 - status if status < 0 else status


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--measure-child":
        sys.exit(measured_child(sys.argv[2:]))
    sys.exit(main(sys.argv[1:]))
