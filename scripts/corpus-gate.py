#!/usr/bin/env python3
"""The corpus gate: WeaveC on real C projects at pinned revisions.

RFC 0030, section 17.5. Reads test/corpus/ (manifest.json, expected.json,
triage.json, injections/, bench/, support/) and runs the 11 corpus configs
(sds, cJSON, jsmn, log.c, printf, linenoise, cJSON-program, zlib, lua,
linenoise-program, jansson). test/corpus/README.md documents the files.

Modes (combine freely; at least one, or --update-from):

  --quick     per-file `weavec-cc -c` of each config's files and
              `weavec --whole-program` for wholeProgram configs; ledger
              summaries against the expected.json ratchet, findings against
              triage.json, gates G9 (count), G10, G13 and G15. Every PR.
  --full      --quick plus project builds (trap mode) and test suites with
              trap detection, a report-mode rerun that attributes checks to
              lines (G11), the injections and the benchmarks. Weekly.
  --inject    apply each injection patch to a copy and check that the bug is
              reported at the injected line (G12).
  --bench     min-of-N user CPU of each benchmark built by weavec-cc and by
              the reference compiler (G14).

Modifiers:

  --legacy          golden semantics (v0.10.0): the `weavec` tool exactly as
                    scripts/corpus.py ran it, diagnostics only, no ledger; the
                    binaries default to $WEAVEC_GOLDEN_DIR. With --quick it
                    checks the tallies recorded under "legacy" in
                    expected.json (S0); with --inject, the injection baseline.
  --compare-golden  run the binaries under test and the golden ones and fail
                    on any difference in the sorted diagnostics of a config
                    (S1): the `weavec` analyses, and the per-file
                    `weavec-cc -c` diagnostics when both weavec-cc exist.
  --checks verify   build and test in verify mode; any trap fails (G6).
  --reference-only  build, test and benchmark with the reference compiler
                    (--cc) alone, no WeaveC; checks that the manifest's
                    commands work and gives the baseline times. With
                    --inject, runs each trap injection under ASan instead.
  --update          rewrite expected.json from this run (the sections the
                    run measured); --update-from RESULTS does the same from
                    a --json file written elsewhere (for example CI).

Examples:

  WEAVEC_GOLDEN_DIR=/path/to/golden scripts/corpus-gate.py --quick --legacy
  scripts/corpus-gate.py --quick --weavec build/release/bin/weavec \\
      --weavec-cc build/release/bin/weavec-cc --only jansson --json out.json
  scripts/corpus-gate.py --full --reference-only --cc "$(brew --prefix llvm)/bin/clang"
  scripts/corpus-gate.py --inject --legacy --update

Exit status: 0 when every check passes, 1 when a check fails, 2 on a usage
or setup error. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import datetime
import fnmatch
import glob
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable, Iterable

ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = ROOT / "test" / "corpus"
DEFAULT_MANIFEST = CORPUS_DIR / "manifest.json"
DEFAULT_EXPECTED = CORPUS_DIR / "expected.json"
DEFAULT_TRIAGE = CORPUS_DIR / "triage.json"
DEFAULT_INJECTIONS = CORPUS_DIR / "injections" / "injections.json"
DEFAULT_WORKDIR = ROOT / "build" / "corpus"

RESULTS_SCHEMA = "weavec-corpus-gate-results"
EXPECTED_SCHEMA = "weavec-corpus-expected"
TRIAGE_SCHEMA = "weavec-corpus-triage"
MANIFEST_SCHEMA = "weavec-corpus-manifest"
INJECTIONS_SCHEMA = "weavec-corpus-injections"
LEDGER_SCHEMA = "weavec-ledger"

# `file:line:col: severity: message [weavec::id]`, as both tools print it.
DIAG_RE = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<severity>error|warning): (?P<message>.*) \[weavec::(?P<id>[a-z-]+)\]$"
)
# Anything Clang itself reports (parse errors, missing headers) has no
# `[weavec::...]` tag; it is counted separately so broken setups are visible.
CLANG_DIAG_RE = re.compile(r"^(?P<file>[^:\n]+):\d+:\d+: (?P<severity>error|fatal error): ")
# The report-mode runtime (RFC 0030, section 10.7).
REPORT_RE = re.compile(
    r"weavec: runtime check failed: (?P<template>[a-z]+) at "
    r"(?P<file>.+?):(?P<line>\d+):(?P<col>\d+)\s*$"
)
# How shells, make and CTest describe a process that died by SIGTRAP or
# SIGILL (the trap of a failed check), plus the 128+signal exit statuses.
# CTest 3.29 prints "SIGTRAP***Exception:" and "***Exception: Illegal";
# older versions print "***Exception: Other" for SIGTRAP.
TRAP_TEXT_RE = re.compile(
    r"Trace/BPT trap|Trace/breakpoint trap|Illegal instruction|\bSIGTRAP\b|\bSIGILL\b|\(ILLEGAL\)"
    r"|\*\*\*Exception: (?:Illegal|Other)|\bError 13[23]\b|exit (?:status|code) 13[23]\b"
)

# Ids that report coverage rather than a bug; everything else is a bug claim.
COVERAGE_IDS = frozenset({"analysis-incomplete", "annotation-required", "checking-incomplete", "checking-failed"})
# Matching facet of each id (RFC 0030, section 17.3).
FACET_OF_ID = {
    "use-after-free": "temporal", "double-free": "temporal", "use-after-move": "temporal",
    "conflicting-borrow": "temporal", "lifetime-too-short": "temporal",
    "mismatched-release": "temporal", "annotation-mismatch": "temporal",
    "null-dereference": "null", "use-of-uninitialized": "null",
    "out-of-bounds": "spatial", "invalid-release": "spatial",
    "contradicted-assumption": "assertion",
}
# Ids a trap template stands for, when a diagnostic satisfies a trap
# expectation or a trap satisfies a diagnostic expectation (section 17.3).
TEMPLATE_IDS = {
    "nonnull": {"null-dereference"},
    "index": {"out-of-bounds"}, "span": {"out-of-bounds"}, "len": {"out-of-bounds"},
    "disjoint": {"out-of-bounds"},
    "assert": {"contradicted-assumption"},
    "violation": None,  # any id
}
FACETS = ("spatial", "null", "temporal", "assertion")
OUTCOMES = ("proven", "checked", "violation", "unresolved", "trusted")
TRAP_SIGNALS = (signal.SIGTRAP, signal.SIGILL)

# The ratchet (section 17.5). Exact fields must equal the recorded value: a
# worse value is a regression, a better one an improvement that --update
# must record, a neutral change likewise. Budget fields fail only above
# their tolerance and are rewritten by --update.
EXACT_FIELDS: tuple[tuple[tuple[str, ...], str], ...] = (
    (("errors",), "lower"),
    (("warnings",), "lower"),
    (("ledger", "proven"), "higher"),
    (("ledger", "checked"), "neutral"),
    (("ledger", "violation"), "lower"),
    (("ledger", "unresolved"), "lower"),
    (("ledger", "trusted"), "lower"),
    (("unresolvedShare", "spatialNull"), "lower"),
)
BUDGET_FIELDS: tuple[tuple[tuple[str, ...], float, float, bool], ...] = (
    # (path, relative tolerance, absolute slack, machine-dependent). The slack
    # keeps timer noise on sub-second CPU times from failing the gate.
    (("cpuSeconds",), 0.10, 1.0, True),
    (("workCounters", "blockTransfers"), 0.02, 0, False),
    (("workCounters", "functions"), 0.02, 0, False),
    (("workCounters", "sites"), 0.02, 0, False),
)
ANALYSIS_KINDS = ("units", "program")

_log_lock = threading.Lock()
VERBOSE = False


class GateError(Exception):
    """A usage or setup problem: exit status 2."""


def log(message: str) -> None:
    with _log_lock:
        print(message, file=sys.stderr, flush=True)


def vlog(message: str) -> None:
    if VERBOSE:
        log(message)


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


# -- platform -----------------------------------------------------------------


def platform_key() -> str:
    machine = platform.machine().lower() or "unknown"
    if machine in ("amd64", "x64"):
        machine = "x86_64"
    if machine == "aarch64":
        machine = "arm64"
    return f"{sys.platform}-{machine}"


def cpu_brand() -> str:
    try:
        if sys.platform == "darwin":
            out = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True,
                                 text=True, check=True)
            return out.stdout.strip()
        if sys.platform.startswith("linux"):
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    return platform.processor() or "unknown CPU"


def machine_key() -> str:
    """Identifies a machine well enough to compare CPU times recorded on it."""
    return f"{platform_key()} {cpu_brand()} x{os.cpu_count() or 1}"


# -- processes ----------------------------------------------------------------


@dataclasses.dataclass
class ProcResult:
    command: str
    returncode: int
    stdout: str
    stderr: str
    seconds: float
    user: float = 0.0
    system: float = 0.0
    maxrss: int | None = None
    timed_out: bool = False
    error: str = ""

    @property
    def cpu(self) -> float:
        return self.user + self.system

    @property
    def output(self) -> str:
        # scripts/corpus.py parsed stderr, then stdout.
        return self.stderr + self.stdout

    @property
    def signal(self) -> int | None:
        return -self.returncode if self.returncode < 0 else None


def _pump(stream, sink: list) -> None:
    try:
        for chunk in iter(lambda: stream.read(65536), b""):
            sink.append(chunk)
    finally:
        stream.close()


def run_process(argv: list[str] | str, *, cwd: Path | str, env: dict | None = None,
                timeout: float | None = None, shell: bool = False,
                stdin: Any = subprocess.DEVNULL) -> ProcResult:
    """Run one command in its own session and account for its whole tree.

    The rusage that wait4 returns for the child includes its reaped
    descendants, so the CPU time of `make` covers its compilers and that of
    `sh -c 'a | b'` both sides of the pipe. On timeout the session is killed.
    """
    command = argv if isinstance(argv, str) else shlex.join(str(a) for a in argv)
    start = time.perf_counter()
    try:
        proc = subprocess.Popen(argv, cwd=str(cwd), env=env, stdin=stdin, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, start_new_session=True, shell=shell)
    except OSError as exc:
        return ProcResult(command, 127, "", "", 0.0, error=str(exc))
    out_chunks: list[bytes] = []
    err_chunks: list[bytes] = []
    pumps = [threading.Thread(target=_pump, args=(proc.stdout, out_chunks), daemon=True),
             threading.Thread(target=_pump, args=(proc.stderr, err_chunks), daemon=True)]
    for pump in pumps:
        pump.start()
    deadline = None if timeout is None else start + timeout
    timed_out = False
    delay = 0.001
    status = 0
    rusage = None
    try:
        while True:
            pid, status, rusage = os.wait4(proc.pid, os.WNOHANG)
            if pid:
                break
            if deadline is not None and time.perf_counter() > deadline:
                timed_out = True
                _kill_group(proc.pid)
                _, status, rusage = os.wait4(proc.pid, 0)
                break
            time.sleep(delay)
            delay = min(delay * 2, 0.05)
    except KeyboardInterrupt:
        _kill_group(proc.pid)
        raise
    seconds = time.perf_counter() - start
    # Descendants left behind (a daemon, a hung grandchild) would keep the
    # pipes open; the session goes with the command.
    _kill_group(proc.pid)
    for pump in pumps:
        pump.join(timeout=30)
    returncode = os.waitstatus_to_exitcode(status)
    proc.returncode = returncode  # reaped here; keep Popen from waiting again
    maxrss = None
    user = system = 0.0
    if rusage is not None:
        user, system = rusage.ru_utime, rusage.ru_stime
        maxrss = int(rusage.ru_maxrss if sys.platform == "darwin" else rusage.ru_maxrss * 1024)
    return ProcResult(command, returncode, b"".join(out_chunks).decode("utf-8", "replace"),
                      b"".join(err_chunks).decode("utf-8", "replace"), seconds, user, system,
                      maxrss, timed_out)


def _kill_group(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def run_shell(command: str, *, cwd: Path, env: dict, timeout: float | None) -> ProcResult:
    return run_process(["/bin/sh", "-c", command], cwd=cwd, env=env, timeout=timeout)


def describe_status(result: ProcResult) -> str:
    if result.error:
        return result.error
    if result.timed_out:
        return f"timed out after {result.seconds:.0f} s"
    if result.returncode < 0:
        try:
            name = signal.Signals(-result.returncode).name
        except ValueError:
            name = f"signal {-result.returncode}"
        return f"killed by {name}"
    return f"exit status {result.returncode}"


# -- files --------------------------------------------------------------------


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        raise GateError(f"{path}: not found") from None
    except json.JSONDecodeError as exc:
        raise GateError(f"{path}: invalid JSON: {exc}") from None


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def copy_tree(source: Path, dest: Path) -> None:
    """Copy a checkout, .git included: the copy stays the fingerprint root (RFC 0030,
    section 12.3), so builds and injections fingerprint findings as --quick does."""
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(source, dest, symlinks=True)


def remove_tree(path: Path) -> None:
    shutil.rmtree(path, ignore_errors=True)


def rel_path(file: str, root: Path) -> str:
    """Root-relative path with / separators, or the path unchanged outside it."""
    try:
        return Path(file).resolve().relative_to(root.resolve()).as_posix()
    except (ValueError, OSError):
        return file


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def get_path(data: dict, path: Iterable[str]) -> Any:
    for key in path:
        if not isinstance(data, dict) or key not in data:
            return None
        data = data[key]
    return data


def set_path(data: dict, path: tuple[str, ...], value: Any) -> None:
    for key in path[:-1]:
        data = data.setdefault(key, {})
    data[path[-1]] = value


# -- manifest -----------------------------------------------------------------


@dataclasses.dataclass
class Project:
    name: str
    url: str
    sha: str
    support: list[str]
    configs: list["Config"] = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Bench:
    name: str
    build: list[str]
    command: str
    repeat: int
    input: str | None = None
    check: str | None = None


@dataclasses.dataclass
class Config:
    name: str
    project: Project
    files: list[str]
    args: list[str]
    whole_program: bool = False
    build: list[str] = dataclasses.field(default_factory=list)
    test: list[str] = dataclasses.field(default_factory=list)
    test_timeout: float | None = None
    bench: Bench | None = None
    link: dict | None = None
    lowered: list[dict] = dataclasses.field(default_factory=list)
    notes: str = ""


@dataclasses.dataclass
class Manifest:
    projects: list[Project]
    configs: list[Config]
    gates: dict
    path: Path

    def config(self, name: str) -> Config:
        for config in self.configs:
            if config.name == name:
                return config
        raise KeyError(name)


SHA_RE = re.compile(r"[0-9a-f]{40}")
LOWERED_RE = re.compile(r"-Wno-error=weavec-(?P<id>[a-z-]+)")
# Flags that would lower or silence WeaveC diagnostics outside `lowered`.
FORBIDDEN_FLAG_RE = re.compile(r"(?<![\w-])(-Wno-error(?:=\S*)?|-Wno-weavec\S*|-w)(?![\w=-])")


def load_manifest(path: Path, support_root: Path) -> Manifest:
    data = read_json(path)
    problems: list[str] = []
    if data.get("schema") != MANIFEST_SCHEMA or data.get("version") != 1:
        problems.append(f"schema must be {MANIFEST_SCHEMA} version 1")
    projects: list[Project] = []
    configs: list[Config] = []
    names: set[str] = set()
    for p in data.get("projects", []):
        name = p.get("name", "?")
        sha = p.get("sha", "")
        if not SHA_RE.fullmatch(sha):
            problems.append(f"project {name}: sha must be 40 lowercase hex digits (got {sha!r})")
        if not p.get("url"):
            problems.append(f"project {name}: url is missing")
        project = Project(name=name, url=p.get("url", ""), sha=sha, support=list(p.get("support", [])))
        for rel in project.support:
            if not (support_root / rel).is_file():
                problems.append(f"project {name}: support file {rel} is missing from {support_root}")
        for c in p.get("configs", []):
            cname = c.get("name", "?")
            if cname in names:
                problems.append(f"config {cname}: duplicate name")
            names.add(cname)
            compile_ = c.get("compile") or {}
            if not compile_.get("files"):
                problems.append(f"config {cname}: compile.files is missing")
            bench = None
            if c.get("bench"):
                b = c["bench"]
                if not b.get("command") or not b.get("build"):
                    problems.append(f"config {cname}: bench needs build and command")
                bench = Bench(name=b.get("name", cname), build=list(b.get("build", [])),
                              command=b.get("command", ""), repeat=int(b.get("repeat", 7)),
                              input=b.get("input"), check=b.get("check"))
            config = Config(
                name=cname, project=project, files=list(compile_.get("files", [])),
                args=list(compile_.get("args", [])), whole_program=bool(c.get("wholeProgram", False)),
                build=list(c.get("build", [])), test=list(c.get("test", [])),
                test_timeout=c.get("testTimeout"), bench=bench, link=c.get("link"),
                lowered=list(c.get("lowered", [])), notes=c.get("notes", ""))
            problems.extend(check_lowering(config))
            project.configs.append(config)
            configs.append(config)
        projects.append(project)
    if problems:
        raise GateError(f"{path}: invalid manifest:\n  " + "\n  ".join(problems))
    return Manifest(projects=projects, configs=configs, gates=data.get("gates", {}), path=path)


def check_lowering(config: Config) -> list[str]:
    """Only `lowered` entries may lower a WeaveC error (section 17.5)."""
    problems = []
    for entry in config.lowered:
        flag = entry.get("flag", "")
        if not LOWERED_RE.fullmatch(flag):
            problems.append(f"config {config.name}: lowered flag {flag!r} is not -Wno-error=weavec-<id>")
        if not entry.get("fingerprint"):
            problems.append(f"config {config.name}: lowered flag {flag!r} has no fingerprint")
    commands = list(config.build) + list(config.test) + list(config.args)
    if config.bench:
        commands += config.bench.build + [config.bench.command]
    for command in commands:
        match = FORBIDDEN_FLAG_RE.search(command)
        if match:
            problems.append(f"config {config.name}: {match.group(1)!r} in {command!r}; lower errors only "
                            f"through `lowered` entries")
    return problems


def support_dir(config: Config, support_root: Path) -> Path:
    return support_root / config.project.name


def expand_args(config: Config, support_root: Path) -> list[str]:
    support = str(support_dir(config, support_root))
    return [a.replace("{support}", support) for a in config.args]


def expand_files(root: Path, patterns: Iterable[str]) -> list[Path]:
    """As scripts/corpus.py did: each pattern's sorted glob, in pattern order."""
    files: list[Path] = []
    for pattern in patterns:
        matches = sorted(glob.glob(str(root / pattern), recursive=True))
        if not matches:
            raise GateError(f"{root}: pattern {pattern!r} matched nothing")
        files.extend(Path(m) for m in matches)
    return files


def select_configs(manifest: Manifest, only: list[str]) -> list[Config]:
    if not only:
        return list(manifest.configs)
    known = {c.name for c in manifest.configs}
    unknown = sorted(set(only) - known)
    if unknown:
        raise GateError(f"unknown config(s): {', '.join(unknown)} (known: {', '.join(sorted(known))})")
    return [c for c in manifest.configs if c.name in only]


# -- checkouts ----------------------------------------------------------------


def git(args: list[str], cwd: Path, check: bool = True) -> str:
    out = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True)
    if check and out.returncode != 0:
        raise GateError(f"git {' '.join(args)} in {cwd} failed: {out.stderr.strip()}")
    return out.stdout


def ensure_checkout(project: Project, workdir: Path, fetch: bool, offline: bool) -> Path:
    dest = workdir / project.name
    if dest.exists():
        head = git(["rev-parse", "HEAD"], dest, check=False).strip()
        if head != project.sha:
            if not fetch or offline:
                raise GateError(
                    f"{dest}: checkout is at {head[:12] or 'no commit'}, the manifest pins "
                    f"{project.sha[:12]}; rerun with --fetch or check out the pinned commit")
            log(f"[{project.name}] fetching {project.sha[:12]}")
            git(["fetch", "--depth", "1", "origin", project.sha], dest)
            git(["checkout", "--quiet", "--detach", project.sha], dest)
        modified = git(["status", "--porcelain", "--untracked-files=no"], dest).strip()
        if modified:
            raise GateError(f"{dest}: tracked files are modified; the gate analyses pristine checkouts:\n"
                            f"{modified}")
        return dest
    if offline:
        raise GateError(f"{dest}: missing and --offline was given")
    log(f"[{project.name}] cloning {project.url} at {project.sha[:12]}")
    dest.mkdir(parents=True)
    try:
        git(["init", "--quiet"], dest)
        git(["remote", "add", "origin", project.url], dest)
        git(["fetch", "--quiet", "--depth", "1", "origin", project.sha], dest)
        git(["checkout", "--quiet", "--detach", "FETCH_HEAD"], dest)
    except GateError:
        remove_tree(dest)
        raise
    head = git(["rev-parse", "HEAD"], dest).strip()
    if head != project.sha:
        raise GateError(f"{dest}: fetched {head}, expected {project.sha}")
    return dest


def tracked_files(root: Path) -> set[str]:
    return set(git(["ls-files"], root).splitlines())


def config_files(config: Config, root: Path, tracked: set[str] | None = None) -> list[Path]:
    files = expand_files(root, config.files)
    if tracked is not None:
        stray = [str(f.relative_to(root)) for f in files if f.relative_to(root).as_posix() not in tracked]
        if stray:
            raise GateError(f"{config.name}: untracked files match the compile patterns: {', '.join(stray)}")
    return files


# -- diagnostics --------------------------------------------------------------


@dataclasses.dataclass
class Diagnostic:
    file: str
    line: int
    col: int
    severity: str
    id: str
    message: str
    certainty: str = ""
    facet: str = ""
    function: str = ""
    fingerprint: str = ""

    def render(self) -> str:
        return f"{self.file}:{self.line}:{self.col}: {self.severity}: {self.message} [weavec::{self.id}]"

    def to_json(self) -> dict:
        data = {"file": self.file, "line": self.line, "col": self.col, "severity": self.severity,
                "id": self.id, "message": self.message}
        for key in ("certainty", "facet", "function", "fingerprint"):
            if getattr(self, key):
                data[key] = getattr(self, key)
        return data

    @property
    def facet_or_mapped(self) -> str:
        return self.facet or FACET_OF_ID.get(self.id, "")


def parse_diagnostics(text: str, root: Path) -> tuple[list[Diagnostic], int]:
    """WeaveC diagnostics and the number of Clang errors in tool output."""
    diagnostics: list[Diagnostic] = []
    clang_errors = 0
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if m:
            diagnostics.append(Diagnostic(file=rel_path(m.group("file"), root), line=int(m.group("line")),
                                          col=int(m.group("col")), severity=m.group("severity"),
                                          id=m.group("id"), message=m.group("message")))
        elif CLANG_DIAG_RE.match(line):
            clang_errors += 1
    return diagnostics, clang_errors


def classify_failure(result: ProcResult, diagnostics: list[Diagnostic], clang_errors: int) -> str:
    """Setup and tool failures must never look like clean code (RFC 0014)."""
    if result.error:
        return result.error
    if result.timed_out:
        return f"timeout after {result.seconds:.0f} seconds"
    if clang_errors:
        return f"{clang_errors} Clang parse error(s)"
    if result.returncode < 0 or result.returncode > 1:
        return f"checker exited with status {result.returncode}"
    if result.returncode and not any(d.severity == "error" for d in diagnostics):
        return f"checker failed without a WeaveC error (status {result.returncode})"
    output = result.output
    if "non-converg" in output or "failed to converge" in output or "did not converge" in output:
        return "program analysis did not converge"
    if any(d.id == "analysis-incomplete" and "iteration limit reached" in d.message for d in diagnostics):
        return "analysis reached an iteration limit"
    return ""


def digest_diagnostics(diagnostics: Iterable[Diagnostic]) -> str:
    return "sha256:" + sha256_text("\n".join(sorted(d.render() for d in diagnostics)))


def is_bug_claim(id_: str) -> bool:
    return id_ not in COVERAGE_IDS


# -- binaries -----------------------------------------------------------------


@dataclasses.dataclass
class Binaries:
    weavec: str | None
    weavec_cc: str | None
    golden_weavec: str | None
    golden_weavec_cc: str | None
    reference_cc: str | None


def tool_version(path: str | None) -> str:
    if not path:
        return ""
    try:
        out = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"unavailable: {exc}"
    text = (out.stdout or out.stderr).strip().splitlines()
    return text[0] if text else ""


def resolve_binary(value: str | None, candidates: list[Path], what: str, required: bool) -> str | None:
    if value:
        path = Path(value)
        if os.sep in value or path.exists():
            # Absolute, but not through symlinks: a compiler may depend on the
            # name it is called by.
            path = Path(os.path.abspath(path))
            if not path.is_file() or not os.access(path, os.X_OK):
                raise GateError(f"{what}: {path} is not an executable file")
            return str(path)
        found = shutil.which(value)
        if not found:
            raise GateError(f"{what}: {value} not found")
        return found
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    if required:
        raise GateError(f"{what}: no binary given and none found at "
                        + ", ".join(str(c) for c in candidates))
    return None


def default_reference_cc() -> str | None:
    prefix = os.environ.get("WEAVEC_LLVM_PREFIX")
    if prefix and (Path(prefix) / "bin" / "clang").is_file():
        return str(Path(prefix) / "bin" / "clang")
    return shutil.which("clang")


# -- legacy analysis (v0.10.0 semantics) ----------------------------------------


@dataclasses.dataclass
class UnitRun:
    config: str
    files: list[str]
    command: str
    seconds: float
    cpu: float
    exit_code: int
    diagnostics: list[Diagnostic]
    clang_errors: int
    failure: str = ""
    maxrss: int | None = None

    def to_json(self) -> dict:
        return {"config": self.config, "files": self.files, "seconds": round(self.seconds, 3),
                "cpuSeconds": round(self.cpu, 3), "exitCode": self.exit_code, "clangErrors": self.clang_errors,
                "failure": self.failure, "maxRssBytes": self.maxrss,
                "diagnostics": [d.to_json() for d in self.diagnostics]}


def legacy_groups(config: Config, files: list[Path]) -> list[list[Path]]:
    return [files] if config.whole_program else [[f] for f in files]


def legacy_command(weavec: str, config: Config, group: list[Path], support_root: Path,
                   whole_program: bool | None = None) -> list[str]:
    """The command scripts/corpus.py ran (v0.10.0)."""
    cmd = [weavec]
    if config.whole_program if whole_program is None else whole_program:
        cmd.append("--whole-program")
    cmd.extend(str(f) for f in group)
    # Clang stops after 20 errors per unit by default; a tally that is capped
    # per unit cannot be compared between runs (Lua's lstrlib.c hits the cap).
    cmd.extend(["--", "-ferror-limit=0", *expand_args(config, support_root)])
    return cmd


def run_tool_unit(argv: list[str], config: Config, root: Path, group: list[Path],
                  timeout: float) -> UnitRun:
    result = run_process(argv, cwd=root, timeout=timeout)
    diagnostics, clang_errors = parse_diagnostics(result.output, root)
    failure = classify_failure(result, diagnostics, clang_errors)
    return UnitRun(config=config.name, files=[f.relative_to(root).as_posix() for f in group],
                   command=result.command, seconds=result.seconds, cpu=result.cpu,
                   exit_code=result.returncode, diagnostics=diagnostics, clang_errors=clang_errors,
                   failure=failure, maxrss=result.maxrss)


def run_cc_unit(weavec_cc: str, config: Config, root: Path, file: Path, support_root: Path,
                objdir: Path, timeout: float) -> UnitRun:
    """Per-file `weavec-cc -c` with only the config's arguments (diagnostics)."""
    rel = file.relative_to(root).as_posix()
    obj = objdir / (rel.replace("/", "__") + ".o")
    argv = [weavec_cc, "-c", "-ferror-limit=0", *expand_args(config, support_root), str(file), "-o", str(obj)]
    result = run_process(argv, cwd=root, timeout=timeout)
    diagnostics, clang_errors = parse_diagnostics(result.output, root)
    return UnitRun(config=config.name, files=[rel], command=result.command, seconds=result.seconds,
                   cpu=result.cpu, exit_code=result.returncode, diagnostics=diagnostics,
                   clang_errors=clang_errors, failure=classify_failure(result, diagnostics, clang_errors),
                   maxrss=result.maxrss)


def tally_legacy(config: Config, units: list[UnitRun]) -> dict:
    by_id: collections.Counter[str] = collections.Counter()
    diagnostics: list[Diagnostic] = []
    for unit in units:
        for d in unit.diagnostics:
            by_id[d.id] += 1
        diagnostics.extend(unit.diagnostics)
    return {
        "units": len(units),
        "byId": dict(sorted(by_id.items())),
        "bugClaims": sum(n for id_, n in by_id.items() if is_bug_claim(id_)),
        "digest": digest_diagnostics(diagnostics),
        "clangErrors": sum(u.clang_errors for u in units),
        "failures": [f"{' '.join(u.files)}: {u.failure}" for u in units if u.failure],
        "seconds": round(sum(u.seconds for u in units), 3),
        "cpuSeconds": round(sum(u.cpu for u in units), 3),
    }


def legacy_totals(tallies: dict[str, dict]) -> dict:
    by_id: collections.Counter[str] = collections.Counter()
    for tally in tallies.values():
        by_id.update(tally["byId"])
    return {
        "units": sum(t["units"] for t in tallies.values()),
        "byId": dict(sorted(by_id.items())),
        "bugClaims": sum(t["bugClaims"] for t in tallies.values()),
    }


def compare_legacy(tallies: dict[str, dict], recorded: dict | None, platform: str) -> tuple[list[str], list[str]]:
    """Compare legacy tallies with expected.json's legacy.quick section.

    Returns (failures, notes). A baseline recorded on another platform is
    not compared (system headers change the diagnostics).
    """
    if not recorded:
        return ["no legacy baseline recorded in expected.json; run --quick --legacy --update"], []
    if recorded.get("platform") != platform:
        return [], [f"legacy baseline was recorded on {recorded.get('platform')}; not compared on {platform}"]
    failures = []
    configs = recorded.get("configs", {})
    for name, tally in tallies.items():
        want = configs.get(name)
        if want is None:
            failures.append(f"{name}: no legacy baseline recorded")
            continue
        for key in ("units", "byId", "bugClaims"):
            if tally[key] != want.get(key):
                failures.append(f"{name}: {key} {json.dumps(tally[key])} != recorded {json.dumps(want.get(key))}")
        if want.get("digest") and tally["digest"] != want["digest"]:
            failures.append(f"{name}: the sorted diagnostics differ from the recorded baseline "
                            f"(digest {tally['digest'][:19]}... != {want['digest'][:19]}...)")
    if set(tallies) == set(configs):
        totals = legacy_totals(tallies)
        want_totals = recorded.get("totals", {})
        for key in ("units", "byId", "bugClaims"):
            if totals[key] != want_totals.get(key):
                failures.append(f"totals: {key} {json.dumps(totals[key])} != recorded "
                                f"{json.dumps(want_totals.get(key))}")
    return failures, []


def diff_sorted(left: list[str], right: list[str], limit: int = 20) -> list[str]:
    """Lines only in one of two sorted lists (multiset difference)."""
    lc, rc = collections.Counter(left), collections.Counter(right)
    only_left = sorted((lc - rc).elements())
    only_right = sorted((rc - lc).elements())
    lines = [f"- {x}" for x in only_left] + [f"+ {x}" for x in only_right]
    if len(lines) > limit:
        lines = lines[:limit] + [f"... {len(lines) - limit} more"]
    return lines


# -- ledgers (RFC 0030 semantics) -----------------------------------------------


def empty_facets() -> dict:
    return {facet: {outcome: 0 for outcome in OUTCOMES} for facet in FACETS}


@dataclasses.dataclass
class Analysis:
    """One measured analysis of a config: its per-file units, or its program."""
    kind: str
    errors: int = 0
    warnings: int = 0
    facets: dict = dataclasses.field(default_factory=empty_facets)
    sites: int = 0
    functions: int = 0
    block_transfers: int | None = None
    over_budget: list[str] = dataclasses.field(default_factory=list)
    unresolved_reasons: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    diagnostics: list[Diagnostic] = dataclasses.field(default_factory=list)
    cpu: float = 0.0
    seconds: float = 0.0
    failures: list[str] = dataclasses.field(default_factory=list)
    ledgers: int = 0

    def outcome_total(self, outcome: str) -> int:
        return sum(self.facets[f][outcome] for f in FACETS)

    @property
    def spatial_null_share(self) -> float | None:
        total = sum(self.facets[f][o] for f in ("spatial", "null") for o in OUTCOMES)
        if total == 0:
            return None
        unresolved = self.facets["spatial"]["unresolved"] + self.facets["null"]["unresolved"]
        return round(unresolved / total, 4)

    def measured(self) -> dict:
        """The ratchet shape of this analysis (expected.json)."""
        return {
            "errors": self.errors,
            "warnings": self.warnings,
            "ledger": {"sites": self.sites, **{o: self.outcome_total(o) for o in OUTCOMES}},
            "unresolvedShare": {"spatialNull": self.spatial_null_share},
            "cpuSeconds": round(self.cpu, 2),
            "workCounters": {"blockTransfers": self.block_transfers, "functions": self.functions,
                             "sites": self.sites},
        }

    def to_json(self) -> dict:
        data = self.measured()
        data.update({
            "facets": self.facets,
            "overBudget": sorted(set(self.over_budget)),
            "unresolvedReasons": dict(sorted(self.unresolved_reasons.items())),
            "ledgers": self.ledgers,
            "seconds": round(self.seconds, 2),
            "failures": self.failures,
            "diagnostics": [d.to_json() for d in self.diagnostics],
        })
        return data


def validate_ledger(data: Any, path: Path) -> dict:
    if not isinstance(data, dict) or data.get("schema") != LEDGER_SCHEMA:
        raise ValueError(f"{path}: not a {LEDGER_SCHEMA} document")
    if data.get("version") != 1:
        raise ValueError(f"{path}: unsupported ledger version {data.get('version')!r}")
    if not isinstance(data.get("summary"), dict):
        raise ValueError(f"{path}: no summary")
    return data


def ledger_diagnostics(data: dict, root: Path) -> list[Diagnostic]:
    seen = set()
    out = []
    lists = [data.get("diagnostics") or []]
    for unit in data.get("units") or []:
        lists.append(unit.get("diagnostics") or [])
    for items in lists:
        for item in items:
            file = item.get("file", "")
            if file and os.path.isabs(file):
                file = rel_path(file, root)
            d = Diagnostic(file=file, line=int(item.get("line", 0)), col=int(item.get("column", 0)),
                           severity=item.get("severity", ""), id=item.get("id", ""),
                           message=item.get("message", ""), certainty=item.get("certainty", ""),
                           facet=item.get("facet", ""), function=item.get("function", ""),
                           fingerprint=item.get("fingerprint", ""))
            key = (d.fingerprint, d.file, d.line, d.col, d.id, d.message)
            if key not in seen:
                seen.add(key)
                out.append(d)
    return out


def add_ledger(analysis: Analysis, data: dict, root: Path) -> None:
    summary = data["summary"]
    analysis.ledgers += 1
    analysis.errors += int(summary.get("errors", 0))
    analysis.warnings += int(summary.get("warnings", 0))
    analysis.sites += int(summary.get("sites", 0))
    analysis.functions += int(summary.get("functions", 0))
    facets = summary.get("facets") or {}
    for facet in FACETS:
        for outcome in OUTCOMES:
            analysis.facets[facet][outcome] += int((facets.get(facet) or {}).get(outcome, 0))
    analysis.unresolved_reasons.update({k: int(v) for k, v in (summary.get("unresolvedReasons") or {}).items()})
    analysis.over_budget.extend(summary.get("overBudget") or [])
    analysis.diagnostics.extend(ledger_diagnostics(data, root))


def add_stats(analysis: Analysis, path: Path) -> None:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return
    value = (data.get("counters") or {}).get("block_transfers")
    if value is not None:
        analysis.block_transfers = (analysis.block_transfers or 0) + int(value)


# -- quick (RFC 0030 semantics) -------------------------------------------------


def probe_ledger_support(binaries: Binaries, scratch: Path) -> None:
    """Fail early, with a clear message, on binaries without RFC 0030 ledgers."""
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / "probe.c"
    source.write_text("int weavec_probe(void) { return 0; }\n")
    if binaries.weavec_cc:
        result = run_process([binaries.weavec_cc, "-c", f"-fweavec-ledger={scratch / 'probe.ledger.json'}",
                              str(source), "-o", str(scratch / "probe.o")], cwd=scratch, timeout=120)
        if result.returncode != 0 or not (scratch / "probe.ledger.json").exists():
            raise GateError(f"{binaries.weavec_cc} does not write RFC 0030 ledgers (-fweavec-ledger): "
                            f"{describe_status(result)}; {result.stderr.strip()[:300]}\n"
                            f"Use --legacy for v0.10.0 binaries.")
    if binaries.weavec:
        result = run_process([binaries.weavec, f"--ledger={scratch / 'probe.tool.json'}", str(source), "--"],
                             cwd=scratch, timeout=120)
        if not (scratch / "probe.tool.json").exists():
            raise GateError(f"{binaries.weavec} does not write RFC 0030 ledgers (--ledger): "
                            f"{describe_status(result)}; {result.stderr.strip()[:300]}\n"
                            f"Use --legacy for v0.10.0 binaries.")


def quick_units(binaries: Binaries, config: Config, root: Path, files: list[Path], support_root: Path,
                work: Path, checks: str, timeout: float,
                pool: concurrent.futures.Executor | None = None) -> Analysis:
    analysis = Analysis(kind="units")
    work.mkdir(parents=True, exist_ok=True)

    def one(file: Path) -> tuple[Path, ProcResult, Path, Path]:
        stem = file.relative_to(root).as_posix().replace("/", "__")
        ledger = work / f"{stem}.ledger.json"
        stats = work / f"{stem}.stats.json"
        for stale in (ledger, stats):
            stale.unlink(missing_ok=True)
        argv = [binaries.weavec_cc, "-c", "-ferror-limit=0", f"-fweavec-checks={checks}",
                f"-fweavec-ledger={ledger}", f"-fweavec-analysis-stats={stats}",
                *expand_args(config, support_root), str(file), "-o", str(work / f"{stem}.o")]
        return file, run_process(argv, cwd=root, timeout=timeout), ledger, stats

    for file, result, ledger, stats in (pool.map(one, files) if pool else map(one, files)):
        analysis.cpu += result.cpu
        analysis.seconds += result.seconds
        text_diags, clang_errors = parse_diagnostics(result.output, root)
        failure = classify_failure(result, text_diags, clang_errors)
        rel = file.relative_to(root).as_posix()
        if failure:
            analysis.failures.append(f"{rel}: {failure}")
            continue
        try:
            add_ledger(analysis, validate_ledger(json.loads(ledger.read_text()), ledger), root)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            analysis.failures.append(f"{rel}: no valid ledger ({exc})")
            continue
        add_stats(analysis, stats)
    return analysis


def quick_program(binaries: Binaries, config: Config, root: Path, files: list[Path], support_root: Path,
                  work: Path, timeout: float) -> Analysis:
    analysis = Analysis(kind="program")
    work.mkdir(parents=True, exist_ok=True)
    ledger = work / "program.ledger.json"
    stats = work / "program.stats.json"
    for stale in (ledger, stats):
        stale.unlink(missing_ok=True)
    argv = [binaries.weavec, "--whole-program", f"--ledger={ledger}", f"--analysis-stats={stats}",
            *[str(f) for f in files], "--", "-ferror-limit=0", *expand_args(config, support_root)]
    result = run_process(argv, cwd=root, timeout=timeout)
    analysis.cpu, analysis.seconds = result.cpu, result.seconds
    text_diags, clang_errors = parse_diagnostics(result.output, root)
    failure = classify_failure(result, text_diags, clang_errors)
    if failure:
        analysis.failures.append(f"whole program: {failure}")
        return analysis
    try:
        add_ledger(analysis, validate_ledger(json.loads(ledger.read_text()), ledger), root)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        analysis.failures.append(f"whole program: no valid ledger ({exc})")
        return analysis
    add_stats(analysis, stats)
    return analysis


# -- ratchet ------------------------------------------------------------------


@dataclasses.dataclass
class RatchetResult:
    regressions: list[str] = dataclasses.field(default_factory=list)
    improvements: list[str] = dataclasses.field(default_factory=list)
    changes: list[str] = dataclasses.field(default_factory=list)
    over_budget: list[str] = dataclasses.field(default_factory=list)
    missing: list[str] = dataclasses.field(default_factory=list)
    notes: list[str] = dataclasses.field(default_factory=list)

    @property
    def failed(self) -> bool:
        return bool(self.regressions or self.improvements or self.changes or self.over_budget or self.missing)

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


def compare_analysis(label: str, measured: dict, recorded: dict, same_machine: bool,
                     result: RatchetResult) -> None:
    for path, direction in EXACT_FIELDS:
        now, before = get_path(measured, path), get_path(recorded, path)
        name = f"{label}.{'.'.join(path)}"
        if before is None and now is None:
            continue
        if before is None:
            result.missing.append(f"{name}: not recorded (now {now})")
            continue
        if now == before:
            continue
        if now is None:
            result.regressions.append(f"{name}: {before} -> not measured")
        elif direction == "neutral":
            result.changes.append(f"{name}: {before} -> {now} (changed; run --update)")
        elif (now > before) == (direction == "lower"):
            result.regressions.append(f"{name}: {before} -> {now} (worse)")
        else:
            result.improvements.append(f"{name}: {before} -> {now} (better; run --update to ratchet it in)")
    for path, tolerance, slack, machine_dependent in BUDGET_FIELDS:
        now, before = get_path(measured, path), get_path(recorded, path)
        name = f"{label}.{'.'.join(path)}"
        if before is None or now is None:
            if before is None and now is not None:
                result.missing.append(f"{name}: not recorded (now {now})")
            continue
        if machine_dependent and not same_machine:
            result.notes.append(f"{name}: recorded on another machine; {before} vs {now} not compared")
            continue
        if now > before * (1 + tolerance) and now - before > slack:
            result.over_budget.append(f"{name}: {before} -> {now} (more than {tolerance:.0%} over)")


def compare_ratchet(measured: dict[str, dict], expected: dict, platform: str, machine: str) -> RatchetResult:
    """Check measured config sections against expected.json for this platform."""
    result = RatchetResult()
    section = (expected.get("platforms") or {}).get(platform)
    if section is None:
        result.missing.append(f"no expectations recorded for {platform}; run with --update (or "
                              f"--update-from a results file measured on {platform})")
        return result
    same_machine = section.get("machine") == machine
    recorded_configs = section.get("configs") or {}
    for name, now in measured.items():
        before = recorded_configs.get(name)
        if before is None:
            result.missing.append(f"{name}: not recorded for {platform}")
            continue
        for kind in ANALYSIS_KINDS:
            if kind not in now:
                continue  # not measured by this run (or failed, which is reported as such)
            if kind not in before:
                result.missing.append(f"{name}.{kind}: not recorded")
                continue
            compare_analysis(f"{name}.{kind}", now[kind], before[kind], same_machine, result)
        if "traps" in now:
            if "traps" not in before:
                result.missing.append(f"{name}.traps: not recorded (now {now['traps']})")
            elif now["traps"] > before["traps"]:
                result.regressions.append(f"{name}.traps: {before['traps']} -> {now['traps']} (worse)")
            elif now["traps"] < before["traps"]:
                result.improvements.append(f"{name}.traps: {before['traps']} -> {now['traps']} "
                                           f"(better; run --update)")
        if "overhead" in now and now["overhead"] is not None:
            if before.get("overhead") is None:
                result.missing.append(f"{name}.overhead: not recorded (now {now['overhead']})")
            elif not same_machine:
                result.notes.append(f"{name}.overhead: recorded on another machine; not compared")
            elif now["overhead"] > before["overhead"] * 1.10:
                result.over_budget.append(f"{name}.overhead: {before['overhead']} -> {now['overhead']} "
                                          f"(more than 10% over)")
    return result


def merge_expected(expected: dict, measured: dict[str, dict], platform: str, machine: str,
                   producer: str) -> dict:
    """expected.json with this run's measurements recorded for its platform."""
    merged = json.loads(json.dumps(expected))
    merged.setdefault("schema", EXPECTED_SCHEMA)
    merged.setdefault("version", 1)
    section = merged.setdefault("platforms", {}).setdefault(platform, {})
    section["machine"] = machine
    section["producer"] = producer
    section["updated"] = datetime.date.today().isoformat()
    configs = section.setdefault("configs", {})
    for name, now in measured.items():
        entry = configs.setdefault(name, {})
        for key, value in now.items():
            entry[key] = value
    section["configs"] = dict(sorted(configs.items()))
    return merged


# -- triage -------------------------------------------------------------------


@dataclasses.dataclass
class TriageResult:
    untriaged: list[dict] = dataclasses.field(default_factory=list)
    false_errors: list[dict] = dataclasses.field(default_factory=list)
    stale: list[dict] = dataclasses.field(default_factory=list)
    invalid: list[str] = dataclasses.field(default_factory=list)
    definite_errors: list[dict] = dataclasses.field(default_factory=list)
    possible_temporal: list[dict] = dataclasses.field(default_factory=list)

    @property
    def failed(self) -> bool:
        return bool(self.untriaged or self.false_errors or self.invalid)

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


def load_triage(path: Path) -> list[dict]:
    if not path.exists():
        return []
    data = read_json(path)
    if isinstance(data, list):
        return data
    if data.get("schema") != TRIAGE_SCHEMA or data.get("version") != 1:
        raise GateError(f"{path}: schema must be {TRIAGE_SCHEMA} version 1")
    return list(data.get("entries", []))


def finding_kind(d: Diagnostic) -> str | None:
    """`definite` for a definite error, `possible` for a possible temporal warning."""
    if d.severity == "error" and d.certainty in ("definite", ""):
        return "definite"
    if d.certainty == "possible" and d.facet_or_mapped == "temporal":
        return "possible"
    return None


def findings_of(config: str, diagnostics: Iterable[Diagnostic]) -> list[dict]:
    seen = set()
    out = []
    for d in diagnostics:
        kind = finding_kind(d)
        if kind is None:
            continue
        key = d.fingerprint or f"{d.file}:{d.line}:{d.col}:{d.id}:{d.message}"
        if key in seen:
            continue
        seen.add(key)
        out.append({"fingerprint": d.fingerprint, "config": config, "id": d.id, "certainty": kind,
                    "file": d.file, "line": d.line, "message": d.message})
    return out


def check_triage(findings: list[dict], entries: list[dict], configs_run: set[str]) -> TriageResult:
    result = TriageResult()
    by_key: dict[tuple[str, str], dict] = {}
    for entry in entries:
        missing = [k for k in ("fingerprint", "config", "id", "certainty", "file", "line", "verdict")
                   if k not in entry]
        if missing:
            result.invalid.append(f"triage entry {entry.get('fingerprint', '?')}: missing {', '.join(missing)}")
            continue
        if entry["verdict"] not in ("true", "false"):
            result.invalid.append(f"triage entry {entry['fingerprint']}: verdict must be \"true\" or \"false\"")
            continue
        if entry["certainty"] not in ("definite", "possible"):
            result.invalid.append(f"triage entry {entry['fingerprint']}: certainty must be definite or possible")
            continue
        by_key[(entry["config"], entry["fingerprint"])] = entry
    seen = set()
    for finding in findings:
        key = (finding["config"], finding["fingerprint"])
        unique = (finding["config"], finding["fingerprint"] or
                  f"{finding['file']}:{finding['line']}:{finding['id']}:{finding['message']}")
        if unique in seen:
            continue  # the same finding from the unit, program and build analyses
        seen.add(unique)
        seen.add(key)
        if finding["certainty"] == "definite":
            result.definite_errors.append(finding)
        else:
            result.possible_temporal.append(finding)
        entry = by_key.get(key)
        if entry is None or not finding["fingerprint"]:
            result.untriaged.append(finding)
        elif finding["certainty"] == "definite" and entry["verdict"] == "false":
            result.false_errors.append({**finding, "note": entry.get("note", "")})
    for key, entry in by_key.items():
        if entry["config"] in configs_run and key not in seen:
            result.stale.append(entry)
    return result


def true_error_sites(entries: list[dict]) -> set[tuple[str, str, int]]:
    """(config, file, line) of triaged-true definite errors: traps there count as true positives."""
    return {(e["config"], e["file"], int(e["line"])) for e in entries
            if e.get("verdict") == "true" and e.get("certainty") == "definite"}


def check_lowered_against_triage(configs: list[Config], entries: list[dict]) -> list[str]:
    problems = []
    true_definite = {(e.get("config"), e.get("fingerprint")): e for e in entries
                     if e.get("verdict") == "true" and e.get("certainty") == "definite"}
    for config in configs:
        for low in config.lowered:
            entry = true_definite.get((config.name, low.get("fingerprint")))
            match = LOWERED_RE.fullmatch(low.get("flag", ""))
            if entry is None:
                problems.append(f"{config.name}: {low.get('flag')} lowers fingerprint {low.get('fingerprint')}, "
                                f"which has no triaged-true definite error")
            elif match and match.group("id") != entry.get("id"):
                problems.append(f"{config.name}: {low.get('flag')} does not match the triaged id {entry.get('id')}")
    return problems


# -- builds and test suites -----------------------------------------------------


@dataclasses.dataclass
class StepRun:
    command: str
    status: str
    ok: bool
    seconds: float
    cpu: float
    log: str
    tail: str = ""  # the end of the output, kept after the work copy is deleted

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


@dataclasses.dataclass
class BuildRun:
    config: str
    mode: str
    compiler: str
    steps: list[StepRun] = dataclasses.field(default_factory=list)
    tests: list[StepRun] = dataclasses.field(default_factory=list)
    built: bool = False
    tests_passed: bool | None = None
    diagnostics: list[Diagnostic] = dataclasses.field(default_factory=list)
    program: Analysis | None = None
    trap_deaths: list[str] = dataclasses.field(default_factory=list)
    reports: list[dict] = dataclasses.field(default_factory=list)
    failures: list[str] = dataclasses.field(default_factory=list)

    def to_json(self) -> dict:
        return {"config": self.config, "mode": self.mode, "compiler": self.compiler,
                "built": self.built, "testsPassed": self.tests_passed,
                "steps": [s.to_json() for s in self.steps], "tests": [s.to_json() for s in self.tests],
                "diagnostics": [d.to_json() for d in self.diagnostics],
                "ledger": self.program.to_json() if self.program else None,
                "trapDeaths": self.trap_deaths, "reports": self.reports, "failures": self.failures}


def output_tail(text: str, lines: int = 40) -> str:
    return "\n".join(text.rstrip().splitlines()[-lines:])


def write_wrapper(path: Path, compiler: str, flags: list[str]) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    words = " ".join(shlex.quote(w) for w in [compiler, *flags])
    path.write_text(f"#!/bin/sh\n# written by scripts/corpus-gate.py\nexec {words} \"$@\"\n")
    path.chmod(0o755)
    return str(path)


def base_env(config: Config, cc: str, src: Path, support_root: Path, bench_dir: Path, jobs: int,
             extra: dict | None = None) -> dict:
    env = dict(os.environ)
    env.update({
        "CC": cc,
        "JOBS": str(jobs),
        "SRC": str(src),
        "SUPPORT": str(support_dir(config, support_root)),
        "BENCH": str(bench_dir),
    })
    # A build must not pick up the caller's flags.
    for key in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS", "MFLAGS"):
        env.pop(key, None)
    if extra:
        env.update(extra)
    return env


def trap_evidence(result: ProcResult) -> list[str]:
    evidence = []
    if result.signal in TRAP_SIGNALS or result.returncode in tuple(128 + s for s in TRAP_SIGNALS):
        evidence.append(f"{result.command}: {describe_status(result)}")
    for line in result.output.splitlines():
        if TRAP_TEXT_RE.search(line):
            evidence.append(line.strip()[:300])
    return evidence


def parse_reports(text: str, root: Path) -> list[dict]:
    reports = []
    seen = set()
    for line in text.splitlines():
        m = REPORT_RE.search(line)
        if not m:
            continue
        file = m.group("file")
        if os.path.isabs(file):
            file = rel_path(file, root)
        key = (m.group("template"), file, int(m.group("line")), int(m.group("col")))
        if key in seen:
            continue
        seen.add(key)
        reports.append({"template": key[0], "file": key[1], "line": key[2], "col": key[3]})
    return reports


def normalise_report_file(file: str) -> str:
    # Makefiles pass paths relative to the build directory ("./x.c", "../x.c").
    parts = [p for p in Path(file).as_posix().split("/") if p not in (".", "")]
    while parts and parts[0] == "..":
        parts.pop(0)
    return "/".join(parts)


def collect_build_ledgers(ledger_dir: Path, src: Path, tracked: set[str]) -> Analysis:
    """Unit and program ledgers of a build, without compiler probes and configure tests."""
    analysis = Analysis(kind="build")
    for path in sorted(ledger_dir.glob("*.json")):
        try:
            data = validate_ledger(json.loads(path.read_text()), path)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            analysis.failures.append(f"{path.name}: {exc}")
            continue
        sources = [u.get("source", "") for u in data.get("units") or []]
        if sources and not any(normalise_report_file(rel_path(s, src) if os.path.isabs(s) else s)
                               in tracked for s in sources):
            continue  # CMake's compiler probes, configure's tests: no source of the project
        add_ledger(analysis, data, src)
    return analysis


def run_build(config: Config, mode: str, compiler: str, flags: list[str], checkout: Path, run_dir: Path,
              support_root: Path, bench_dir: Path, jobs: int, build_timeout: float, keep: bool,
              tracked: set[str], with_ledger: bool, patch: Path | None = None,
              run_commands: list[str] | None = None, extra_env: dict | None = None) -> BuildRun:
    """Copy the checkout, build it with CC set to `compiler` + `flags`, run the tests.

    `run_commands` replaces the config's tests (injections).
    """
    work = run_dir / mode
    src = work / "src"
    ledger_dir = work / "ledgers"
    remove_tree(work)
    copy_tree(checkout, src)
    run = BuildRun(config=config.name, mode=mode, compiler=compiler)
    if patch is not None:
        applied = apply_patch(patch, src)
        if applied:
            run.failures.append(applied)
            return run
    if with_ledger:
        ledger_dir.mkdir(parents=True, exist_ok=True)
        flags = [*flags, f"-fweavec-ledger={ledger_dir}/"]
    cc = write_wrapper(work / "bin" / "cc", compiler, flags) if flags else compiler
    env = base_env(config, cc, src, support_root, bench_dir, jobs, extra_env)
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    output = []
    run.built = True
    for index, command in enumerate(config.build):
        result = run_shell(command, cwd=src, env=env, timeout=build_timeout)
        log_path = logs / f"build-{index}.log"
        log_path.write_text(f"$ {command}\n{result.output}")
        output.append(result.output)
        ok = result.returncode == 0 and not result.timed_out
        run.steps.append(StepRun(command, describe_status(result), ok, round(result.seconds, 3),
                                 round(result.cpu, 3), str(log_path), output_tail(result.output)))
        if not ok:
            run.built = False
            run.failures.append(f"build step {command!r}: {describe_status(result)}: "
                                f"{output_tail(result.output, 6)}")
            break
    diagnostics, _ = parse_diagnostics("\n".join(output), src)
    run.diagnostics = diagnostics
    if with_ledger:
        run.program = collect_build_ledgers(ledger_dir, src, tracked)
        run.failures.extend(run.program.failures)
    if run.built:
        commands = config.test if run_commands is None else run_commands
        timeout = config.test_timeout or build_timeout
        test_output = []
        passed = True
        for index, command in enumerate(commands):
            result = run_shell(command, cwd=src, env=env, timeout=timeout)
            log_path = logs / f"test-{index}.log"
            log_path.write_text(f"$ {command}\n{result.output}")
            test_output.append(result.output)
            ok = result.returncode == 0 and not result.timed_out
            run.tests.append(StepRun(command, describe_status(result), ok, round(result.seconds, 3),
                                     round(result.cpu, 3), str(log_path), output_tail(result.output)))
            run.trap_deaths.extend(trap_evidence(result))
            if not ok:
                passed = False
        run.tests_passed = passed if commands else None
        run.reports = parse_reports("\n".join(test_output), src)
        for report in run.reports:
            report["file"] = normalise_report_file(report["file"])
    if not keep:
        remove_tree(src)
    return run


# -- injections ---------------------------------------------------------------


@dataclasses.dataclass
class Injection:
    id: str
    config: str
    patch: str
    file: str
    line: int
    expect: dict
    mode: str
    description: str = ""
    required: bool = False
    dossier: str | None = None

    @property
    def ids(self) -> set[str] | None:
        if "ids" in self.expect:
            return set(self.expect["ids"])
        return None

    @property
    def trap(self) -> str | None:
        return self.expect.get("trap")


def load_injections(path: Path, manifest: Manifest) -> list[Injection]:
    data = read_json(path)
    if data.get("schema") != INJECTIONS_SCHEMA or data.get("version") != 1:
        raise GateError(f"{path}: schema must be {INJECTIONS_SCHEMA} version 1")
    names = {c.name for c in manifest.configs}
    problems = []
    injections = []
    ids = set()
    for item in data.get("injections", []):
        try:
            inj = Injection(id=item["id"], config=item["config"], patch=item["patch"], file=item["file"],
                            line=int(item["line"]), expect=dict(item["expect"]), mode=item["mode"],
                            description=item.get("description", ""), required=bool(item.get("required")),
                            dossier=item.get("dossier"))
        except (KeyError, TypeError, ValueError) as exc:
            problems.append(f"entry {item.get('id', '?')}: {exc}")
            continue
        if inj.id in ids:
            problems.append(f"{inj.id}: duplicate id")
        ids.add(inj.id)
        if inj.config not in names:
            problems.append(f"{inj.id}: unknown config {inj.config}")
        if inj.mode not in ("unit", "whole-program"):
            problems.append(f"{inj.id}: mode must be unit or whole-program")
        if not (path.parent / inj.patch).is_file():
            problems.append(f"{inj.id}: patch {inj.patch} not found")
        if inj.ids is None and inj.trap is None:
            problems.append(f"{inj.id}: expect needs ids or trap")
        if inj.trap is not None and (inj.trap not in TEMPLATE_IDS or not inj.expect.get("run")):
            problems.append(f"{inj.id}: expect.trap must be a template with a run command")
        if inj.expect.get("severity", "any") not in ("error", "warning", "any"):
            problems.append(f"{inj.id}: severity must be error, warning or any")
        injections.append(inj)
    if problems:
        raise GateError(f"{path}: invalid injections:\n  " + "\n  ".join(problems))
    return injections


def apply_patch(patch: Path, src: Path) -> str:
    """Apply a -p1 patch to a copy; returns a failure message or ''."""
    result = run_process(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=src, timeout=60)
    if result.returncode == 0:
        return ""
    fallback = run_process(["patch", "-p1", "--batch", "--forward", "-i", str(patch)], cwd=src, timeout=60)
    if fallback.returncode == 0:
        return ""
    return f"patch {patch.name} does not apply: {result.stderr.strip()[:200]} {fallback.output.strip()[:200]}"


def diagnostic_matches(d: Diagnostic, inj: Injection, legacy: bool) -> bool:
    if d.file != inj.file or d.line != inj.line:
        return False
    ids = inj.ids
    if ids is None:
        allowed = TEMPLATE_IDS.get(inj.trap or "")
        if allowed is not None and d.id not in allowed:
            return False
    elif d.id not in ids:
        return False
    severity = inj.expect.get("severity", "any")
    # v0.10.0 reported temporal findings as errors whatever their certainty;
    # the legacy baseline matches by line and id alone.
    return legacy or severity == "any" or d.severity == severity


def report_matches(report: dict, inj: Injection) -> bool:
    if report["file"] != inj.file or report["line"] != inj.line:
        return False
    if inj.trap is not None:
        return report["template"] == inj.trap
    # A diagnostic expectation for a null or spatial id is also met by a trap
    # of a matching template (section 17.3).
    allowed = TEMPLATE_IDS.get(report["template"], set())
    return allowed is None or bool((inj.ids or set()) & allowed)


@dataclasses.dataclass
class InjectionRun:
    injection: Injection
    reported: bool = False
    via: list[str] = dataclasses.field(default_factory=list)
    halves: dict = dataclasses.field(default_factory=dict)
    matches: list[dict] = dataclasses.field(default_factory=list)
    nearby: list[dict] = dataclasses.field(default_factory=list)
    cpu: float = 0.0
    failures: list[str] = dataclasses.field(default_factory=list)
    asan: dict | None = None

    def to_json(self) -> dict:
        inj = self.injection
        return {"id": inj.id, "config": inj.config, "file": inj.file, "line": inj.line, "mode": inj.mode,
                "expect": inj.expect, "required": inj.required, "dossier": inj.dossier,
                "reported": self.reported, "via": self.via, "halves": self.halves, "matches": self.matches,
                "nearby": self.nearby, "cpuSeconds": round(self.cpu, 2), "failures": self.failures,
                "asan": self.asan}


def check_injected_line(src: Path, inj: Injection) -> str:
    try:
        lines = (src / inj.file).read_text(errors="replace").splitlines()
    except OSError as exc:
        return f"{inj.file}: {exc}"
    if inj.line > len(lines) or "INJECTED" not in lines[inj.line - 1]:
        return f"{inj.file}:{inj.line} does not carry the INJECTED marker after patching"
    return ""


def same_file(reported: str, expected: str) -> bool:
    """Whether a sanitizer's path names the expected file: symbolizers print
    either the full path or, on macOS, only the file name."""
    reported, expected = normalise_report_file(reported), normalise_report_file(expected)
    return reported == expected or expected.endswith("/" + reported) or reported.endswith("/" + expected)


FRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in (?P<func>\S+) (?P<file>[^\s:()]+):(?P<line>\d+)")
UBSAN_RE = re.compile(r"(?P<file>[^\s:]+):(?P<line>\d+):\d+: runtime error:")


def sanitizer_location(text: str, src: Path) -> tuple[bool, str | None, tuple[str, int] | None]:
    """(found, first report line, (file, line) of the first frame in the project)."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        m = UBSAN_RE.search(line)
        if m:
            return True, line.strip(), (normalise_report_file(rel_path(m.group("file"), src)), int(m.group("line")))
        if "ERROR: AddressSanitizer" in line:
            for frame in lines[index + 1:index + 80]:
                fm = FRAME_RE.search(frame)
                if fm:
                    file = rel_path(fm.group("file"), src)
                    if not os.path.isabs(file):
                        return True, line.strip(), (normalise_report_file(file), int(fm.group("line")))
            return True, line.strip(), None
        if "buffer overflow detected" in line or "detected buffer overflow" in line:
            return True, line.strip(), None
    return False, None, None


# -- benchmarks ---------------------------------------------------------------


@dataclasses.dataclass
class BenchRun:
    config: str
    name: str
    times: dict = dataclasses.field(default_factory=dict)
    minimum: dict = dataclasses.field(default_factory=dict)
    outputs: dict = dataclasses.field(default_factory=dict)
    ratio: float | None = None
    failures: list[str] = dataclasses.field(default_factory=list)

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


# -- the gate -----------------------------------------------------------------


class Gate:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.support_root = args.support_dir
        self.bench_dir = args.bench_dir
        self.manifest = load_manifest(args.manifest, self.support_root)
        self.configs = select_configs(self.manifest, args.only)
        self.platform = platform_key()
        self.machine = machine_key()
        self.failures: list[str] = []
        self.tool_failures: list[str] = []
        self.notes: list[str] = []
        self.results: dict[str, Any] = {
            "schema": RESULTS_SCHEMA, "version": 1, "started": now_iso(),
            "platform": self.platform, "machine": self.machine,
            "modes": [m for m in ("quick", "full", "inject", "bench") if getattr(args, m)],
            "legacy": args.legacy, "compareGolden": args.compare_golden, "checks": args.checks,
            "referenceOnly": args.reference_only, "configs": {}, "gates": {},
        }
        self.checkouts: dict[str, Path] = {}
        self.tracked: dict[str, set[str]] = {}
        self.checkout_lock = threading.Lock()
        self.run_dir = args.workdir / ".gate" / f"{int(time.time())}-{os.getpid()}"
        self.jobs = max(1, args.jobs)
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs)
        self.triage_entries = load_triage(args.triage)
        self.measured: dict[str, dict] = {}
        self.findings: list[dict] = []
        self.binaries = self.resolve_binaries()

    # ---- setup ----

    def resolve_binaries(self) -> Binaries:
        a = self.args
        golden_dir = Path(a.golden_dir) if a.golden_dir else None
        golden = [golden_dir / "weavec"] if golden_dir else []
        golden_cc = [golden_dir / "weavec-cc"] if golden_dir else []
        built = [ROOT / "build" / "release" / "bin", ROOT / "build" / "dev" / "bin"]
        needs_weavec = (a.quick or a.full or a.inject or a.compare_golden) and not a.reference_only
        needs_cc = ((a.quick or a.full or a.bench) and not a.legacy and not a.reference_only) or \
                   (a.inject and not a.legacy and not a.reference_only)
        if a.legacy:
            weavec = resolve_binary(a.weavec, golden, "--weavec (legacy: $WEAVEC_GOLDEN_DIR/weavec)",
                                    needs_weavec)
            weavec_cc = resolve_binary(a.weavec_cc, golden_cc, "--weavec-cc (legacy: $WEAVEC_GOLDEN_DIR/weavec-cc)",
                                       a.full)
        else:
            weavec = resolve_binary(a.weavec, [d / "weavec" for d in built], "--weavec", needs_weavec)
            weavec_cc = resolve_binary(a.weavec_cc, [d / "weavec-cc" for d in built], "--weavec-cc", needs_cc)
        golden_weavec = resolve_binary(None, golden, "golden weavec", a.compare_golden)
        golden_weavec_cc = resolve_binary(None, golden_cc, "golden weavec-cc", False)
        needs_reference = a.reference_only or ((a.bench or a.full) and not a.legacy)
        reference = a.cc or default_reference_cc()
        if reference:
            reference = resolve_binary(reference, [], "--cc", needs_reference)
        elif needs_reference:
            raise GateError("--cc: no reference compiler; set WEAVEC_LLVM_PREFIX or pass --cc")
        binaries = Binaries(weavec, weavec_cc, golden_weavec, golden_weavec_cc, reference)
        self.results["binaries"] = {
            name: {"path": path, "version": tool_version(path)}
            for name, path in (("weavec", weavec), ("weavec-cc", weavec_cc), ("golden-weavec", golden_weavec),
                               ("golden-weavec-cc", golden_weavec_cc), ("reference-cc", reference))
            if path
        }
        return binaries

    def checkout(self, config: Config) -> Path:
        project = config.project
        with self.checkout_lock:
            if project.name not in self.checkouts:
                path = ensure_checkout(project, self.args.workdir, self.args.fetch, self.args.offline)
                self.tracked[project.name] = tracked_files(path)
                self.results.setdefault("commits", {})[project.name] = project.sha
                self.checkouts[project.name] = path
            return self.checkouts[project.name]

    def config_entry(self, config: Config) -> dict:
        return self.results["configs"].setdefault(config.name, {})

    def fail(self, message: str, tool: bool = False) -> None:
        """Record a failure; `tool` marks a failed analysis, build or run, after
        which the run's measurements are incomplete and are not recorded."""
        self.failures.append(message)
        if tool:
            self.tool_failures.append(message)
        log(f"FAIL: {message}")

    # ---- legacy ----

    def legacy_units(self, weavec: str, configs: list[Config]) -> dict[str, list[UnitRun]]:
        jobs = []
        for config in configs:
            root = self.checkout(config)
            files = config_files(config, root, self.tracked[config.project.name])
            for group in legacy_groups(config, files):
                jobs.append((config, root, group))
        # The whole programs first: Lua takes minutes.
        jobs.sort(key=lambda j: -len(j[2]))
        futures = {}
        for config, root, group in jobs:
            argv = legacy_command(weavec, config, group, self.support_root)
            futures[self.pool.submit(run_tool_unit, argv, config, root, group, self.args.timeout)] = config
        runs: dict[str, list[UnitRun]] = {c.name: [] for c in configs}
        for future in concurrent.futures.as_completed(futures):
            unit = future.result()
            runs[unit.config].append(unit)
            status = unit.failure or f"{len(unit.diagnostics)} diagnostics"
            vlog(f"[{unit.config}] {' '.join(unit.files)[:60]}: {status} ({unit.seconds:.1f} s)")
        for config in configs:
            order = {tuple(f.relative_to(self.checkouts[config.project.name]).as_posix() for f in g): i
                     for i, g in enumerate(legacy_groups(config, config_files(
                         config, self.checkouts[config.project.name])))}
            runs[config.name].sort(key=lambda u: order.get(tuple(u.files), 0))
        return runs

    def run_legacy_quick(self) -> None:
        log(f"legacy analysis with {self.binaries.weavec}")
        runs = self.legacy_units(self.binaries.weavec, self.configs)
        tallies = {}
        for config in self.configs:
            units = runs[config.name]
            tally = tally_legacy(config, units)
            tallies[config.name] = tally
            entry = self.config_entry(config)
            entry["legacy"] = {**tally, "unitRuns": [u.to_json() for u in units]}
            for failure in tally["failures"]:
                self.fail(f"{config.name}: {failure}", tool=True)
        totals = legacy_totals(tallies)
        self.results["legacyTotals"] = totals
        print_legacy_table(self.configs, tallies, totals)
        self.legacy_tallies = tallies
        expected = self.load_expected()
        recorded = (expected.get("legacy") or {}).get("quick")
        if self.args.update:
            return
        failures, notes = compare_legacy(tallies, recorded, self.platform)
        for note in notes:
            self.note(note)
        for failure in failures:
            self.fail(f"legacy baseline: {failure}")
        if recorded and not failures and not notes:
            log(f"legacy baseline reproduced: {totals['bugClaims']} bug claims over {totals['units']} units")

    def run_compare_golden(self) -> None:
        """S1: the binaries under test print exactly the golden diagnostics."""
        tested, golden = self.binaries.weavec, self.binaries.golden_weavec
        if not tested or not golden:
            raise GateError("--compare-golden needs --weavec and a golden weavec ($WEAVEC_GOLDEN_DIR)")
        log(f"comparing {tested} with the golden {golden}")
        # Both runs share the pool, so the two Lua analyses overlap.
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as outer:
            mine_future = outer.submit(self.legacy_units, tested, self.configs)
            theirs_future = outer.submit(self.legacy_units, golden, self.configs)
            mine, theirs = mine_future.result(), theirs_future.result()
        summary: dict[str, dict] = {}
        for config in self.configs:
            summary[config.name] = {"weavec": self.compare_runs(config, "weavec", mine[config.name],
                                                                theirs[config.name])}
        if self.binaries.weavec_cc and self.binaries.golden_weavec_cc:
            log(f"comparing per-file weavec-cc -c: {self.binaries.weavec_cc} vs {self.binaries.golden_weavec_cc}")
            objroot = self.run_dir / "compare-golden"
            futures = {}
            for config in self.configs:
                root = self.checkout(config)
                for label, cc in (("tested", self.binaries.weavec_cc), ("golden", self.binaries.golden_weavec_cc)):
                    out = objroot / config.name / label
                    out.mkdir(parents=True, exist_ok=True)
                    for file in config_files(config, root, self.tracked[config.project.name]):
                        future = self.pool.submit(run_cc_unit, cc, config, root, file, self.support_root, out,
                                                  self.args.timeout)
                        futures[future] = (config.name, label, file)
            units: dict[tuple[str, str], list[tuple[Path, UnitRun]]] = collections.defaultdict(list)
            for future in concurrent.futures.as_completed(futures):
                name, label, file = futures[future]
                units[(name, label)].append((file, future.result()))
            for config in self.configs:
                runs = {label: [u for _, u in sorted(units[(config.name, label)], key=lambda x: str(x[0]))]
                        for label in ("tested", "golden")}
                summary[config.name]["weavec-cc"] = self.compare_runs(config, "weavec-cc -c", runs["tested"],
                                                                      runs["golden"])
            if not self.args.keep:
                remove_tree(objroot)
        else:
            self.note("compare-golden: weavec-cc not compared (needs --weavec-cc and a golden weavec-cc)")
        self.results["compareGoldenResults"] = summary

    def compare_runs(self, config: Config, what: str, mine: list[UnitRun], theirs: list[UnitRun]) -> dict:
        a = [d.render() for u in mine for d in u.diagnostics]
        b = [d.render() for u in theirs for d in u.diagnostics]
        fails = [f"{' '.join(u.files)}: {u.failure}" for u in mine + theirs if u.failure]
        diff = diff_sorted(b, a)
        if fails:
            self.fail(f"compare-golden {config.name} ({what}): " + "; ".join(fails), tool=True)
        if diff:
            self.fail(f"compare-golden {config.name}: {what} diagnostics differ from the golden run "
                      f"({len(a)} vs {len(b)}):\n    " + "\n    ".join(diff))
        else:
            log(f"compare-golden {config.name}: {what} identical ({len(a)} diagnostics)")
        return {"tested": len(a), "golden": len(b), "identical": not diff, "diff": diff, "failures": fails}

    # ---- quick ----

    def run_quick(self) -> None:
        probe_ledger_support(self.binaries, self.run_dir / "probe")
        log(f"quick: weavec-cc {self.binaries.weavec_cc}, weavec {self.binaries.weavec}")
        jobs = []
        for config in self.configs:
            root = self.checkout(config)
            files = config_files(config, root, self.tracked[config.project.name])
            jobs.append((config, root, files))
        # Whole-program analyses in the background, per-file compiles meanwhile.
        program_futures = {}
        for config, root, files in sorted(jobs, key=lambda j: -len(j[2])):
            if config.whole_program:
                work = self.run_dir / "quick" / config.name / "program"
                program_futures[config.name] = self.pool.submit(
                    quick_program, self.binaries, config, root, files, self.support_root, work, self.args.timeout)
        unit_pool = concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs)
        try:
            for config, root, files in jobs:
                work = self.run_dir / "quick" / config.name / "units"
                units = quick_units(self.binaries, config, root, files, self.support_root, work,
                                    self.args.checks, self.args.timeout, unit_pool)
                self.record_analysis(config, units)
        finally:
            unit_pool.shutdown()
        for config, root, files in jobs:
            if config.name in program_futures:
                self.record_analysis(config, program_futures[config.name].result())

    def record_analysis(self, config: Config, analysis: Analysis) -> None:
        entry = self.config_entry(config)
        entry.setdefault("analyses", {})[analysis.kind] = analysis.to_json()
        for failure in analysis.failures:
            self.fail(f"{config.name} ({analysis.kind}): {failure}", tool=True)
        if not analysis.failures:
            self.measured.setdefault(config.name, {})[analysis.kind] = analysis.measured()
        self.findings.extend(findings_of(config.name, analysis.diagnostics))
        share = analysis.spatial_null_share
        log(f"[{config.name}] {analysis.kind}: {analysis.errors} errors, {analysis.warnings} warnings, "
            f"{analysis.sites} sites ({analysis.outcome_total('proven')} proven, "
            f"{analysis.outcome_total('checked')} checked, {analysis.outcome_total('unresolved')} unresolved), "
            f"spatial/null unresolved share {share if share is not None else '-'}, {analysis.cpu:.1f} s CPU")

    # ---- full: builds and tests ----

    def wrapper_flags(self, mode: str, config: Config) -> tuple[str, list[str]]:
        lowered = [low["flag"] for low in config.lowered]
        if mode == "reference":
            return self.binaries.reference_cc, []
        if mode == "legacy":
            return self.binaries.weavec_cc, ["-Wno-error=weavec"]
        return self.binaries.weavec_cc, [f"-fweavec-checks={mode}", "-fno-weavec-summary", *lowered]

    def run_builds(self) -> None:
        if self.args.reference_only:
            modes = ["reference"]
        elif self.args.legacy:
            modes = ["legacy"]
        else:
            modes = [self.args.checks, "report"]
        true_sites = true_error_sites(self.triage_entries)
        for config in self.configs:
            if not config.build:
                continue
            checkout = self.checkout(config)
            entry = self.config_entry(config)
            runs = {}
            for mode in modes:
                compiler, flags = self.wrapper_flags(mode, config)
                with_ledger = mode in ("trap", "verify")
                log(f"[{config.name}] {mode} build with {compiler}")
                build = run_build(config, mode, compiler, flags, checkout, self.run_dir / "builds" / config.name,
                                  self.support_root, self.bench_dir, self.jobs, self.args.build_timeout,
                                  self.args.keep, self.tracked[config.project.name], with_ledger)
                runs[mode] = build
                for failure in build.failures:
                    self.fail(f"{config.name} ({mode}): {failure}", tool=True)
                if build.program is not None:
                    self.findings.extend(findings_of(config.name, build.program.diagnostics))
                total = sum(s.seconds for s in build.steps)
                log(f"[{config.name}] {mode}: built={build.built} tests={build.tests_passed} "
                    f"({total:.1f} s build, {len(build.trap_deaths)} trap signs, {len(build.reports)} reports)")
            for mode, build in runs.items():
                if not build.built or build.tests_passed is not False:
                    continue
                if mode in ("trap", "verify") and self.only_true_positives(config, runs, true_sites):
                    self.note(f"{config.name} ({mode}): the test suite trapped only at triaged-true definite "
                              f"errors (G11 counts them as true positives)")
                    continue
                failed = [t for t in build.tests if not t.ok]
                self.fail(f"{config.name} ({mode}): test suite failed: " + "; ".join(
                    f"{t.command!r}: {t.status}: {output_tail(t.tail, 6)}" for t in failed), tool=True)
            entry["builds"] = {mode: run.to_json() for mode, run in runs.items()}
            if not self.args.reference_only and not self.args.legacy:
                traps = self.count_traps(config, runs, true_sites)
                self.measured.setdefault(config.name, {})["traps"] = traps
                entry["traps"] = traps
                build = runs.get(self.args.checks)
                if build and build.program:
                    entry["buildLedger"] = build.program.measured()

    def only_true_positives(self, config: Config, runs: dict[str, BuildRun], true_sites: set) -> bool:
        """A trap-mode test failure explained by checks at triaged-true definite errors alone."""
        report = runs.get("report")
        if report is None or not report.built or report.tests_passed is False or not report.reports:
            return False
        return all((config.name, r["file"], r["line"]) in true_sites for r in report.reports)

    def count_traps(self, config: Config, runs: dict[str, BuildRun], true_sites: set) -> int:
        """G11 (G6 in verify mode): check failures in the test suites.

        Each site the report-mode rerun names counts once, except the sites of
        triaged-true definite errors; a trap-mode death that the rerun names no
        site for counts once.
        """
        report = runs.get("report")
        reports = report.reports if report else []
        sites = [r for r in reports if (config.name, r["file"], r["line"]) not in true_sites]
        checked = runs.get(self.args.checks)
        deaths = checked.trap_deaths if checked else []
        for r in sites:
            self.fail(f"{config.name}: check failed in the test suite: {r['template']} at "
                      f"{r['file']}:{r['line']}:{r['col']}")
        unattributed = bool(deaths) and not reports
        if unattributed:
            self.fail(f"{config.name}: the test suite trapped ({deaths[0]}) and the report-mode rerun "
                      f"names no check")
        if self.args.checks == "verify" and deaths and (sites or unattributed):
            self.fail(f"{config.name}: trap in verify mode (G6): {deaths[0]}")
        return len(sites) + (1 if unattributed else 0)

    # ---- injections ----

    def run_injections(self) -> None:
        injections = load_injections(self.args.injections, self.manifest)
        selected = {c.name for c in self.configs}
        injections = [i for i in injections if i.config in selected]
        if self.args.injection:
            wanted = set(self.args.injection)
            unknown = wanted - {i.id for i in injections}
            if unknown:
                raise GateError(f"unknown injection(s): {', '.join(sorted(unknown))}")
            injections = [i for i in injections if i.id in wanted]
        if not self.args.legacy and not self.args.reference_only:
            probe_ledger_support(self.binaries, self.run_dir / "probe")
        log(f"injections: {len(injections)} ({'legacy' if self.args.legacy else 'reference' if self.args.reference_only else 'current'} semantics)")
        for inj in injections:
            self.checkout(self.manifest.config(inj.config))
        order = sorted(injections, key=lambda i: (i.mode != "whole-program", i.config != "lua"))
        futures = {self.pool.submit(self.run_injection, inj): inj for inj in order}
        runs: dict[str, InjectionRun] = {}
        for future in concurrent.futures.as_completed(futures):
            inj = futures[future]
            try:
                run = future.result()
            except GateError as exc:
                run = InjectionRun(injection=inj, failures=[str(exc)])
            runs[inj.id] = run
            state = "reported" if run.reported else "not reported"
            if self.args.reference_only:
                if run.asan is None:
                    state = "no run command (static expectation)"
                elif run.asan.get("atLine"):
                    state = f"{run.asan['sanitizer']} reports the injected line"
                elif run.asan.get("reached"):
                    state = (f"{run.asan['sanitizer']}: {run.asan.get('report')} "
                             f"(at {run.asan.get('location') or 'no location'})")
                else:
                    state = f"{run.asan['sanitizer']}: nothing reported"
            log(f"[inject] {inj.id}: {state}{' (' + ', '.join(run.via) + ')' if run.via else ''}"
                f"{'; ' + '; '.join(run.failures) if run.failures else ''}")
        ordered = [runs[i.id] for i in injections]
        self.results["injections"] = [r.to_json() for r in ordered]
        for run in ordered:
            for failure in run.failures:
                self.fail(f"injection {run.injection.id}: {failure}", tool=True)
        if self.args.reference_only:
            for r in ordered:
                if r.asan is None:
                    continue
                if not r.asan.get("reached"):
                    self.fail(f"injection {r.injection.id}: its run command does not reach the bug "
                              f"({r.asan['sanitizer']} reports nothing)")
                elif r.asan.get("atLine") is False:
                    self.fail(f"injection {r.injection.id}: {r.asan['sanitizer']} reports "
                              f"{r.asan.get('location')}, not {r.injection.file}:{r.injection.line}")
            return
        print_injection_table(ordered)
        reported = sum(r.reported for r in ordered)
        dossier = [r for r in ordered if r.injection.dossier]
        agree = sum((r.reported == (r.injection.dossier == "caught")) for r in dossier)
        self.results["injectionSummary"] = {"reported": reported, "total": len(ordered),
                                            "dossierAgreement": f"{agree}/{len(dossier)}"}
        log(f"injections reported at the injected line: {reported}/{len(ordered)}; "
            f"agreement with the v0.10.0 dossier: {agree}/{len(dossier)}")
        if self.args.legacy:
            self.check_legacy_injections(ordered)
        else:
            self.gate_g12(ordered, all_selected=len(injections) == len(load_injections(
                self.args.injections, self.manifest)))

    def run_injection(self, inj: Injection) -> InjectionRun:
        config = self.manifest.config(inj.config)
        checkout = self.checkout(config)
        run = InjectionRun(injection=inj)
        work = self.run_dir / "inject" / inj.id
        src = work / "src"
        patch = (self.args.injections.parent / inj.patch).resolve()
        injection_dir = patch.parent
        try:
            if self.args.reference_only:
                if inj.expect.get("run"):
                    run.asan = self.asan_injection(inj, config, checkout, patch, work, injection_dir)
                return run
            copy_tree(checkout, src)
            failure = apply_patch(patch, src) or check_injected_line(src, inj)
            if failure:
                run.failures.append(failure)
                return run
            files = config_files(config, src)
            target = src / inj.file
            diagnostics: list[Diagnostic] = []
            if self.args.legacy:
                group = files if inj.mode == "whole-program" else [target]
                argv = legacy_command(self.binaries.weavec, config, group, self.support_root,
                                      whole_program=inj.mode == "whole-program")
                unit = run_tool_unit(argv, config, src, group, self.args.timeout)
                run.cpu += unit.cpu
                if unit.failure:
                    run.failures.append(unit.failure)
                diagnostics = unit.diagnostics
                run.halves["tool"] = any(diagnostic_matches(d, inj, True) for d in diagnostics)
            else:
                diagnostics = self.inject_current(inj, config, src, files, target, work, run)
            run.matches = [d.to_json() for d in diagnostics if diagnostic_matches(d, inj, self.args.legacy)]
            run.nearby = [d.to_json() for d in diagnostics
                          if d.file == inj.file and abs(d.line - inj.line) <= 3 and d.id not in COVERAGE_IDS]
            if run.matches:
                run.via.append("diagnostic")
            if not self.args.legacy and inj.trap and not run.matches:
                self.inject_trap(inj, config, checkout, patch, work, injection_dir, run)
            if self.args.legacy:
                run.reported = bool(run.matches)
            else:
                needed = [k for k in ("tool", "link", "unit") if k in run.halves]
                run.reported = (bool(needed) and all(run.halves[k] for k in needed)) or "trap" in run.via
        finally:
            if not self.args.keep:
                remove_tree(work)
        return run

    def inject_current(self, inj: Injection, config: Config, src: Path, files: list[Path], target: Path,
                       work: Path, run: InjectionRun) -> list[Diagnostic]:
        diagnostics: list[Diagnostic] = []
        if inj.mode == "unit":
            analysis = quick_units(self.binaries, config, src, [target], self.support_root, work / "unit",
                                   self.args.checks, self.args.timeout)
            run.cpu += analysis.cpu
            run.failures.extend(analysis.failures)
            diagnostics.extend(analysis.diagnostics)
            run.halves["unit"] = any(diagnostic_matches(d, inj, False) for d in analysis.diagnostics)
            return diagnostics
        analysis = quick_program(self.binaries, config, src, files, self.support_root, work / "program",
                                 self.args.timeout)
        run.cpu += analysis.cpu
        run.failures.extend(analysis.failures)
        diagnostics.extend(analysis.diagnostics)
        run.halves["tool"] = any(diagnostic_matches(d, inj, False) for d in analysis.diagnostics)
        if config.link:
            link_diags = self.inject_link(config, src, files, work / "link", run)
            diagnostics.extend(link_diags)
            run.halves["link"] = any(diagnostic_matches(d, inj, False) for d in link_diags)
        return diagnostics

    def inject_link(self, config: Config, src: Path, files: list[Path], work: Path,
                    run: InjectionRun) -> list[Diagnostic]:
        """Compile every file with weavec-cc -c and link the objects directly (G12)."""
        work.mkdir(parents=True, exist_ok=True)
        objects = []
        diagnostics: list[Diagnostic] = []
        for file in files:
            stem = file.relative_to(src).as_posix().replace("/", "__")
            obj = work / f"{stem}.o"
            argv = [self.binaries.weavec_cc, "-c", "-ferror-limit=0", f"-fweavec-checks={self.args.checks}",
                    f"-fweavec-ledger={work / (stem + '.ledger.json')}", *expand_args(config, self.support_root),
                    str(file), "-o", str(obj)]
            result = run_process(argv, cwd=src, timeout=self.args.timeout)
            run.cpu += result.cpu
            found, clang_errors = parse_diagnostics(result.output, src)
            diagnostics.extend(found)
            failure = classify_failure(result, found, clang_errors)
            if failure:
                run.failures.append(f"link half: {file.name}: {failure}")
                return diagnostics
            if not obj.exists():
                return diagnostics  # a definite error at compile time: no object, reported above
            objects.append(str(obj))
        ledger = work / "program.ledger.json"
        argv = [self.binaries.weavec_cc, "-o", str(work / "a.out"), *objects, f"-fweavec-ledger={ledger}",
                *(config.link or {}).get("args", [])]
        result = run_process(argv, cwd=src, timeout=self.args.timeout)
        run.cpu += result.cpu
        found, _ = parse_diagnostics(result.output, src)
        diagnostics.extend(found)
        if ledger.exists():
            try:
                diagnostics.extend(ledger_diagnostics(validate_ledger(json.loads(ledger.read_text()), ledger), src))
            except (ValueError, json.JSONDecodeError) as exc:
                run.failures.append(f"link half: {exc}")
        elif result.returncode not in (0, 1):
            run.failures.append(f"link half: {describe_status(result)}")
        return diagnostics

    def inject_trap(self, inj: Injection, config: Config, checkout: Path, patch: Path, work: Path,
                    injection_dir: Path, run: InjectionRun) -> None:
        compiler, flags = self.wrapper_flags("report", config)
        build = run_build(config, "report", compiler, flags, checkout, work / "trap", self.support_root,
                          self.bench_dir, self.jobs, self.args.build_timeout, self.args.keep,
                          self.tracked[config.project.name], with_ledger=False, patch=patch,
                          run_commands=[inj.expect["run"]], extra_env={"INJECTION_DIR": str(injection_dir)})
        run.failures.extend(f for f in build.failures if not f.startswith("build step"))
        # A build that stops on a definite error is judged by its diagnostics.
        for d in build.diagnostics:
            if diagnostic_matches(d, inj, False):
                run.matches.append(d.to_json())
                if "diagnostic" not in run.via:
                    run.via.append("diagnostic")
        hits = [r for r in build.reports if report_matches(r, inj)]
        run.halves["trap"] = bool(hits)
        if hits:
            run.via.append("trap")
            run.matches.extend({"trap": r} for r in hits)
        elif not build.built and not run.matches:
            run.failures.append("the patched build failed without a matching diagnostic: " +
                                "; ".join(build.failures))

    def asan_injection(self, inj: Injection, config: Config, checkout: Path, patch: Path, work: Path,
                       injection_dir: Path) -> dict:
        """Check that an injection's run command reaches the injected line.

        The reference compiler with ASan and UBSan for index and null bugs;
        without ASan but with _FORTIFY_SOURCE for `len` bugs, whose short
        writes ASan cannot see (the fortified call traps or aborts instead,
        without a location).
        """
        cc = self.binaries.reference_cc
        fortify = inj.trap == "len"
        if fortify:
            flags = ["-O1", "-g", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=2"]
        else:
            flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-g", "-O1",
                     "-fno-omit-frame-pointer"]
        build = run_build(config, "asan", cc, flags, checkout, work / "asan", self.support_root, self.bench_dir,
                          self.jobs, self.args.build_timeout, self.args.keep, self.tracked[config.project.name],
                          with_ledger=False, patch=patch, run_commands=[inj.expect["run"]],
                          extra_env={"INJECTION_DIR": str(injection_dir),
                                     "ASAN_OPTIONS": "detect_leaks=0",
                                     "UBSAN_OPTIONS": "print_stacktrace=1"})
        text = ""
        for step in build.tests:
            try:
                text += Path(step.log).read_text(errors="replace")
            except OSError:
                pass
        found, report, location = sanitizer_location(text, work / "asan" / "src")
        if fortify and not found and build.trap_deaths:
            found, report = True, f"fortified call trapped: {build.trap_deaths[0]}"
        at_line = None if location is None else (same_file(location[0], inj.file) and location[1] == inj.line)
        return {"built": build.built, "sanitizer": "fortify" if fortify else "asan+ubsan",
                "reached": found, "report": report,
                "location": f"{location[0]}:{location[1]}" if location else None,
                "atLine": at_line, "failures": [f for f in build.failures if not f.startswith("build step")]}

    def check_legacy_injections(self, runs: list[InjectionRun]) -> None:
        expected = self.load_expected()
        recorded = (expected.get("legacy") or {}).get("injections")
        self.legacy_injection_runs = runs
        if self.args.update:
            return
        if not recorded:
            self.fail("legacy injection baseline: none recorded in expected.json; run --inject --legacy --update")
            return
        if recorded.get("platform") != self.platform:
            self.note(f"legacy injection baseline recorded on {recorded.get('platform')}; not compared")
            return
        results = recorded.get("results", {})
        for run in runs:
            want = results.get(run.injection.id)
            if want is None:
                self.fail(f"legacy injection baseline: {run.injection.id} not recorded")
            elif want != run.reported:
                self.fail(f"legacy injection baseline: {run.injection.id} reported={run.reported}, "
                          f"recorded {want}")

    def gate_g12(self, runs: list[InjectionRun], all_selected: bool) -> None:
        spec = self.manifest.gates.get("G12", {})
        share = sum(r.reported for r in runs) / len(runs) if runs else 0.0
        required_missed = [r.injection.id for r in runs if r.injection.required and not r.reported]
        ok = not required_missed and (share >= spec.get("minReportedShare", 0.9) or not all_selected)
        detail = {"reported": sum(r.reported for r in runs), "total": len(runs), "share": round(share, 3),
                  "requiredMissed": required_missed, "allInjections": all_selected}
        self.gate("G12", ok, detail)

    # ---- benchmarks ----

    def run_benches(self) -> None:
        for config in self.configs:
            if not config.bench:
                continue
            bench = config.bench
            repeat = self.args.repeat or bench.repeat
            checkout = self.checkout(config)
            compilers = [("reference", self.binaries.reference_cc)]
            if not self.args.reference_only:
                compilers.append(("weavec-cc", self.binaries.weavec_cc))
            run = BenchRun(config=config.name, name=bench.name)
            work = self.run_dir / "bench" / config.name
            remove_tree(work)
            env_extra = {}
            if bench.input:
                input_path = work / "input.bin"
                input_path.parent.mkdir(parents=True, exist_ok=True)
                env = base_env(config, "cc", work, self.support_root, self.bench_dir, self.jobs,
                               {"INPUT": str(input_path)})
                result = run_shell(bench.input, cwd=work, env=env, timeout=self.args.build_timeout)
                if result.returncode != 0:
                    run.failures.append(f"input: {describe_status(result)}: {result.output.strip()[:300]}")
                env_extra["INPUT"] = str(input_path)
            builds = {}
            if not run.failures:
                for label, cc in compilers:
                    src = work / label
                    copy_tree(checkout, src)
                    env = base_env(config, cc, src, self.support_root, self.bench_dir, self.jobs, env_extra)
                    for command in bench.build:
                        result = run_shell(command, cwd=src, env=env, timeout=self.args.build_timeout)
                        if result.returncode != 0:
                            run.failures.append(f"{label} build {command!r}: {describe_status(result)}: "
                                                f"{result.output.strip()[-300:]}")
                            break
                    else:
                        builds[label] = (src, env)
            if len(builds) == len(compilers):
                log(f"[{config.name}] bench {bench.name}: {repeat} runs per build")
                for label, (src, env) in builds.items():  # warm up caches and the input
                    run_shell(bench.command, cwd=src, env=env, timeout=self.args.build_timeout)
                for index in range(repeat):
                    for label, (src, env) in builds.items():
                        result = run_shell(bench.command, cwd=src, env=env, timeout=self.args.build_timeout)
                        if result.returncode != 0:
                            run.failures.append(f"{label} run {index}: {describe_status(result)}: "
                                                f"{result.output.strip()[-300:]}")
                            break
                        run.times.setdefault(label, []).append(round(result.user, 4))
                        run.outputs.setdefault(label, result.stdout.strip()[:200])
                    if run.failures:
                        break
                if bench.check and not run.failures:
                    for label, (src, env) in builds.items():
                        result = run_shell(bench.check, cwd=src, env=env, timeout=self.args.build_timeout)
                        if result.returncode != 0:
                            run.failures.append(f"{label} check: {describe_status(result)}")
                run.minimum = {label: min(times) for label, times in run.times.items() if times}
                if len(set(run.outputs.values())) > 1:
                    run.failures.append(f"the builds print different results: {run.outputs}")
                if "weavec-cc" in run.minimum and run.minimum.get("reference"):
                    run.ratio = round(run.minimum["weavec-cc"] / run.minimum["reference"], 4)
            if not self.args.keep:
                remove_tree(work)
            for failure in run.failures:
                self.fail(f"{config.name} bench: {failure}", tool=True)
            self.config_entry(config)["bench"] = run.to_json()
            if run.ratio is not None:
                self.measured.setdefault(config.name, {})["overhead"] = run.ratio
            log(f"[{config.name}] bench {bench.name}: min user CPU "
                + ", ".join(f"{k} {v:.3f} s" for k, v in run.minimum.items())
                + (f"; overhead {run.ratio:.3f}" if run.ratio is not None else ""))

    # ---- gates and ratchet ----

    def gate(self, name: str, ok: bool | None, detail: Any) -> None:
        status = "skip" if ok is None else "pass" if ok else "fail"
        self.results["gates"][name] = {"status": status, "detail": detail}
        if ok is False:
            self.fail(f"gate {name}: {json.dumps(detail)[:500]}")
        else:
            log(f"gate {name}: {status}")

    def note(self, message: str) -> None:
        self.notes.append(message)
        log(f"note: {message}")

    def evaluate_new_semantics(self) -> None:
        gates = self.manifest.gates
        analysed = self.args.quick or self.args.full
        if analysed:
            self.evaluate_findings_and_analyses()
        if self.args.bench or self.args.full:
            g14 = gates.get("G14", {}).get("maxOverhead", {})
            detail14 = {}
            ok14 = True
            for name, limit in g14.items():
                ratio = self.measured.get(name, {}).get("overhead")
                if name in {c.name for c in self.configs} and ratio is not None:
                    detail14[name] = {"overhead": ratio, "limit": limit}
                    ok14 &= ratio <= limit
            self.gate("G14", ok14 if detail14 else None, detail14)
        if self.args.full:
            traps = {name: m.get("traps") for name, m in self.measured.items() if "traps" in m}
            self.gate("G6" if self.args.checks == "verify" else "G11",
                      all(t == 0 for t in traps.values()) if traps else None, traps)

    def evaluate_findings_and_analyses(self) -> None:
        all_configs = len(self.configs) == len(self.manifest.configs)
        gates = self.manifest.gates
        triage = check_triage(self.findings, self.triage_entries, {c.name for c in self.configs})
        self.results["triage"] = triage.to_json()
        for finding in triage.untriaged:
            self.fail(f"untriaged {finding['certainty']} {finding['id']} in {finding['config']} at "
                      f"{finding['file']}:{finding['line']} (fingerprint {finding['fingerprint'] or 'none'}): "
                      f"{finding['message']}")
        for finding in triage.false_errors:
            self.fail(f"G9: definite error triaged false in {finding['config']} at {finding['file']}:"
                      f"{finding['line']}: {finding['message']}")
        for problem in triage.invalid + check_lowered_against_triage(self.configs, self.triage_entries):
            self.fail(f"triage: {problem}")
        for entry in triage.stale:
            self.note(f"stale triage entry {entry['fingerprint']} ({entry['config']} {entry['file']}:"
                      f"{entry['line']})")
        g9 = gates.get("G9", {})
        definite = len(triage.definite_errors)
        detail9 = {"definiteErrors": definite, "limit": g9.get("maxDefiniteErrors", 10),
                   "falseVerdicts": len(triage.false_errors)}
        must = self.must_report(g9.get("mustReport", []))
        if must is not None:
            detail9["mustReport"] = must
        ok9 = (not triage.false_errors and (definite <= g9.get("maxDefiniteErrors", 10) or not all_configs)
               and all(m["reported"] for m in (must or [])))
        self.gate("G9", ok9 if all_configs or triage.false_errors else None, detail9)
        g10 = gates.get("G10", {})
        per_config = collections.Counter(f["config"] for f in triage.possible_temporal)
        limits = g10.get("maxPossibleTemporalPerConfig", {})
        over = {c: n for c, n in per_config.items() if c in limits and n > limits[c]}
        total = len(triage.possible_temporal)
        ok10 = not over and (total <= g10.get("maxPossibleTemporal", 60) or not all_configs)
        self.gate("G10", ok10 if (all_configs or over) else None,
                  {"possibleTemporal": total, "perConfig": dict(per_config), "over": over})
        g13 = gates.get("G13", {}).get("maxUnitUnresolvedShare", {})
        detail13 = {}
        ok13 = True
        for name, limit in g13.items():
            share = get_path(self.measured.get(name, {}), ("units", "unresolvedShare", "spatialNull"))
            if name in {c.name for c in self.configs}:
                detail13[name] = {"share": share, "limit": limit}
                if share is None or share > limit:
                    ok13 = False
        self.gate("G13", ok13 if detail13 else None, detail13)
        g15 = gates.get("G15", {})
        detail15 = {}
        ok15 = True
        on_reference = self.machine == gates.get("referenceMachine")
        for name, limit in (g15.get("maxProgramCpuSeconds") or {}).items():
            cpu = get_path(self.measured.get(name, {}), ("program", "cpuSeconds"))
            if cpu is None:
                continue
            if on_reference:
                detail15[f"{name}.programCpuSeconds"] = {"cpu": cpu, "limit": limit}
                ok15 &= cpu <= limit
            else:
                detail15[f"{name}.programCpuSeconds"] = {"cpu": cpu, "limit": None,
                                                         "note": "not the reference machine; compare with "
                                                                 "the golden binary's time"}
        functions = over_budget = 0
        for name in self.measured:
            for kind in ANALYSIS_KINDS:
                analysis = self.results["configs"].get(name, {}).get("analyses", {}).get(kind)
                if analysis:
                    functions += analysis["workCounters"]["functions"] or 0
                    over_budget += len(analysis["overBudget"])
        if functions:
            share = over_budget / functions
            detail15["overBudget"] = {"functions": functions, "overBudget": over_budget, "share": round(share, 4)}
            ok15 &= share <= g15.get("maxOverBudgetShare", 0.01)
        for name, spec in (g15.get("maxBuildStepWallSeconds") or {}).items():
            builds = self.results["configs"].get(name, {}).get("builds", {})
            build = builds.get(self.args.checks)
            if not build:
                continue
            step = next((s for s in build["steps"] if s["command"] == spec["step"]), None)
            if step is None:
                continue
            limit = spec["seconds"] if on_reference else self.golden_step_seconds(name, spec["step"])
            detail15[f"{name}.{spec['step']}"] = {"seconds": step["seconds"], "limit": limit}
            if limit is not None:
                ok15 &= step["seconds"] <= limit
        self.gate("G15", ok15 if detail15 else None, detail15)
    def must_report(self, entries: list[dict]) -> list[dict] | None:
        if not self.args.full:
            return None
        out = []
        for entry in entries:
            if entry["config"] not in {c.name for c in self.configs}:
                continue
            build = self.results["configs"].get(entry["config"], {}).get("builds", {}).get(self.args.checks, {})
            diagnostics = list(build.get("diagnostics", [])) + list((build.get("ledger") or {}).get("diagnostics", []))
            hit = any(d["file"] == entry["file"] and d["line"] == entry["line"] and d["id"] in entry["ids"]
                      for d in diagnostics)
            out.append({**entry, "reported": hit})
        return out

    def golden_step_seconds(self, name: str, step: str) -> float | None:
        """G15 off the reference machine: the golden weavec-cc's time for the same step."""
        if not self.binaries.golden_weavec_cc:
            self.note(f"G15 {name}: not the reference machine and no golden weavec-cc to compare with")
            return None
        config = self.manifest.config(name)
        build = run_build(config, "golden", self.binaries.golden_weavec_cc, ["-Wno-error=weavec"],
                          self.checkout(config), self.run_dir / "builds" / name, self.support_root,
                          self.bench_dir, self.jobs, self.args.build_timeout, self.args.keep,
                          self.tracked[config.project.name], with_ledger=False, run_commands=[])
        match = next((s for s in build.steps if s.command == step and s.ok), None)
        return match.seconds if match else None

    def load_expected(self) -> dict:
        if not self.args.expected.exists():
            return {"schema": EXPECTED_SCHEMA, "version": 1}
        data = read_json(self.args.expected)
        if data.get("schema") != EXPECTED_SCHEMA or data.get("version") != 1:
            raise GateError(f"{self.args.expected}: schema must be {EXPECTED_SCHEMA} version 1")
        return data

    def producer(self) -> str:
        versions = self.results.get("binaries", {})
        return (versions.get("weavec-cc") or versions.get("weavec") or {}).get("version", "")

    def ratchet(self) -> None:
        expected = self.load_expected()
        if self.args.update:
            if self.tool_failures:
                log("not updating expected.json: analyses, builds or runs failed")
                return
            merged = merge_expected(expected, self.measured, self.platform, self.machine, self.producer())
            write_json(self.args.expected, merged)
            log(f"updated {self.args.expected} ({self.platform}: {', '.join(sorted(self.measured))})")
            return
        result = compare_ratchet(self.measured, expected, self.platform, self.machine)
        self.results["ratchet"] = result.to_json()
        for kind in ("regressions", "improvements", "changes", "over_budget", "missing"):
            for item in getattr(result, kind):
                self.fail(f"ratchet {kind.replace('_', ' ')}: {item}")
        for note in result.notes:
            self.note(note)
        if not result.failed:
            log("ratchet: expected.json matches")

    def update_legacy(self) -> None:
        if self.tool_failures:
            log("not updating the legacy baseline: analyses failed")
            return
        expected = self.load_expected()
        legacy = expected.setdefault("legacy", {})
        legacy["_comment"] = [
            "v0.10.0 (golden) semantics, recorded with the golden binaries by corpus-gate.py --legacy",
            "--update: quick holds each config's diagnostic tally and the SHA-256 of its sorted",
            "diagnostics; injections holds whether each injection is reported at its line. S0",
            "reproduces them (--quick --legacy, --inject --legacy), and S1's binaries must too."
        ]
        producer = self.results["binaries"].get("weavec", {}).get("version", "")
        if getattr(self, "legacy_tallies", None) is not None:
            quick = legacy.setdefault("quick", {})
            quick.update({"platform": self.platform, "producer": producer,
                          "recorded": datetime.date.today().isoformat()})
            configs = quick.setdefault("configs", {})
            for name, tally in self.legacy_tallies.items():
                configs[name] = {k: tally[k] for k in ("units", "byId", "bugClaims", "digest")}
            ordered = [c.name for c in self.manifest.configs if c.name in configs]
            quick["configs"] = {name: configs[name] for name in ordered}
            if len(quick["configs"]) == len(self.manifest.configs):
                quick["totals"] = legacy_totals(quick["configs"])
            log(f"updated the legacy quick baseline ({len(self.legacy_tallies)} configs)")
        if getattr(self, "legacy_injection_runs", None) is not None:
            injections = legacy.setdefault("injections", {})
            injections.update({"platform": self.platform, "producer": producer,
                               "recorded": datetime.date.today().isoformat()})
            results = injections.setdefault("results", {})
            for run in self.legacy_injection_runs:
                results[run.injection.id] = run.reported
            known = [i.id for i in load_injections(self.args.injections, self.manifest)]
            injections["results"] = {k: results[k] for k in known if k in results}
            reported = sum(injections["results"].values())
            injections["summary"] = {"reported": reported, "total": len(injections["results"])}
            log(f"updated the legacy injection baseline ({reported}/{len(injections['results'])} reported)")
        write_json(self.args.expected, expected)

    # ---- main flow ----

    def run(self) -> int:
        a = self.args
        start = time.perf_counter()
        try:
            if a.legacy:
                if a.quick or a.full:
                    self.run_legacy_quick()
                if a.compare_golden:
                    self.run_compare_golden()
                if a.full:
                    self.run_builds()
                if a.inject or a.full:
                    self.run_injections()
                if a.update:
                    self.update_legacy()
            elif a.reference_only:
                if a.full:
                    self.run_builds()
                if a.inject or a.full:
                    self.run_injections()
                if a.bench or a.full:
                    self.run_benches()
            else:
                if a.compare_golden:
                    self.run_compare_golden()
                if a.quick or a.full:
                    self.run_quick()
                if a.full:
                    self.run_builds()
                if a.inject or a.full:
                    self.run_injections()
                if a.bench or a.full:
                    self.run_benches()
                if a.quick or a.full or a.bench:
                    self.evaluate_new_semantics()
                    self.ratchet()
        finally:
            self.pool.shutdown(wait=True, cancel_futures=True)
            if not a.keep:
                remove_tree(self.run_dir)
        self.results["seconds"] = round(time.perf_counter() - start, 1)
        self.results["measured"] = self.measured
        self.results["failures"] = self.failures
        self.results["notes"] = self.notes
        self.results["status"] = "fail" if self.failures else "pass"
        if a.json:
            write_json(a.json, self.results)
            log(f"wrote {a.json}")
        print()
        if self.failures:
            print(f"corpus gate: FAIL ({len(self.failures)} problem(s))")
            for failure in self.failures[:40]:
                print(f"  - {failure.splitlines()[0]}")
            if len(self.failures) > 40:
                print(f"  ... {len(self.failures) - 40} more")
            return 1
        print("corpus gate: PASS")
        return 0


# -- reporting ----------------------------------------------------------------


def print_legacy_table(configs: list[Config], tallies: dict[str, dict], totals: dict) -> None:
    ids = sorted(totals["byId"])
    name_w = max(len("config"), *(len(c.name) for c in configs))
    header = (f"{'config':<{name_w}}  {'units':>5} {'time':>8} {'clang':>5} {'bugs':>5}  "
              + "  ".join(ids))
    print(header)
    print("-" * len(header))
    for config in configs:
        t = tallies[config.name]
        cells = "  ".join(f"{t['byId'].get(i, 0):>{len(i)}}" for i in ids)
        print(f"{config.name:<{name_w}}  {t['units']:>5} {t['seconds']:>7.1f}s {t['clangErrors']:>5} "
              f"{t['bugClaims']:>5}  {cells}")
    print("-" * len(header))
    cells = "  ".join(f"{totals['byId'].get(i, 0):>{len(i)}}" for i in ids)
    seconds = sum(t["seconds"] for t in tallies.values())
    clang = sum(t["clangErrors"] for t in tallies.values())
    print(f"{'total':<{name_w}}  {totals['units']:>5} {seconds:>7.1f}s {clang:>5} {totals['bugClaims']:>5}  {cells}")
    print(f"bug claims (every id but {', '.join(sorted(COVERAGE_IDS))}): {totals['bugClaims']}")


def print_injection_table(runs: list[InjectionRun]) -> None:
    width = max([len("injection")] + [len(r.injection.id) for r in runs])
    print(f"{'injection':<{width}}  {'config':<17} {'mode':<13} {'where':<28} reported")
    for r in runs:
        inj = r.injection
        where = f"{inj.file}:{inj.line}"
        mark = "yes" if r.reported else "NO"
        extra = f" ({', '.join(r.via)})" if r.via else ""
        dossier = f"  [dossier: {inj.dossier}]" if inj.dossier else ""
        print(f"{inj.id:<{width}}  {inj.config:<17} {inj.mode:<13} {where:<28} {mark}{extra}{dossier}")


# -- --update-from ------------------------------------------------------------


def update_from(results_path: Path, expected_path: Path) -> int:
    results = read_json(results_path)
    if results.get("schema") != RESULTS_SCHEMA:
        raise GateError(f"{results_path}: not a corpus-gate results file")
    if results.get("status") != "pass" and not results.get("measured"):
        raise GateError(f"{results_path}: the run measured nothing")
    if results.get("legacy"):
        raise GateError(f"{results_path}: a --legacy run; rerun --legacy --update on that platform instead")
    expected = read_json(expected_path) if expected_path.exists() else {"schema": EXPECTED_SCHEMA, "version": 1}
    versions = results.get("binaries", {})
    producer = (versions.get("weavec-cc") or versions.get("weavec") or {}).get("version", "")
    merged = merge_expected(expected, results.get("measured", {}), results["platform"], results["machine"], producer)
    write_json(expected_path, merged)
    log(f"updated {expected_path} for {results['platform']} from {results_path}")
    return 0


# -- main ---------------------------------------------------------------------


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = ap.add_argument_group("modes")
    modes.add_argument("--quick", action="store_true", help="per-file compiles and whole-program analyses")
    modes.add_argument("--full", action="store_true", help="quick + builds, tests, injections, benchmarks")
    modes.add_argument("--inject", action="store_true", help="injected bugs (G12)")
    modes.add_argument("--bench", action="store_true", help="benchmarks (G14)")
    modes.add_argument("--update-from", type=Path, metavar="RESULTS",
                       help="record a --json results file (for example from CI) in expected.json and exit")
    mods = ap.add_argument_group("modifiers")
    mods.add_argument("--legacy", action="store_true", help="v0.10.0 semantics with the golden binaries")
    mods.add_argument("--compare-golden", action="store_true",
                      help="fail on any difference from the golden binaries' diagnostics (S1)")
    mods.add_argument("--checks", choices=("trap", "verify"), default="trap", help="check mode (default trap)")
    mods.add_argument("--reference-only", action="store_true",
                      help="build, test and bench with --cc only; with --inject, ASan-check the trap injections")
    mods.add_argument("--update", action="store_true", help="rewrite expected.json from this run")
    bins = ap.add_argument_group("binaries")
    bins.add_argument("--weavec", help="weavec under test (default build/release/bin, then build/dev/bin; "
                                       "with --legacy $WEAVEC_GOLDEN_DIR/weavec)")
    bins.add_argument("--weavec-cc", help="weavec-cc under test (same defaults)")
    bins.add_argument("--golden-dir", default=os.environ.get("WEAVEC_GOLDEN_DIR"),
                      help="golden v0.10.0 binaries (default $WEAVEC_GOLDEN_DIR)")
    bins.add_argument("--cc", help="reference compiler (default $WEAVEC_LLVM_PREFIX/bin/clang, else clang)")
    sel = ap.add_argument_group("selection and resources")
    sel.add_argument("--only", action="append", nargs="+", default=[], metavar="CONFIG",
                     help="run only these configs (repeatable)")
    sel.add_argument("--injection", action="append", default=[], metavar="ID", help="run only this injection")
    sel.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel processes (default: CPUs)")
    sel.add_argument("--timeout", type=float, default=1800, help="seconds per analysis process (default 1800)")
    sel.add_argument("--build-timeout", type=float, default=3600, help="seconds per build or test step")
    sel.add_argument("--repeat", type=int, help="benchmark runs per build (default: the manifest's, 7)")
    sel.add_argument("--workdir", type=Path, default=DEFAULT_WORKDIR,
                     help="checkouts and scratch space (default build/corpus)")
    sel.add_argument("--fetch", action="store_true", help="fetch a checkout that is not at its pinned sha")
    sel.add_argument("--offline", action="store_true", help="never clone or fetch")
    sel.add_argument("--keep", action="store_true", help="keep build copies and logs under <workdir>/.gate")
    sel.add_argument("--json", type=Path, help="write the results here")
    sel.add_argument("-v", "--verbose", action="store_true")
    files = ap.add_argument_group("inputs (for tests)")
    files.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    files.add_argument("--expected", type=Path, default=DEFAULT_EXPECTED)
    files.add_argument("--triage", type=Path, default=DEFAULT_TRIAGE)
    files.add_argument("--injections", type=Path, default=DEFAULT_INJECTIONS)
    files.add_argument("--support-dir", type=Path, default=CORPUS_DIR / "support")
    files.add_argument("--bench-dir", type=Path, default=CORPUS_DIR / "bench")
    args = ap.parse_args(argv)
    args.only = [name for group in args.only for name in group]
    if not (args.quick or args.full or args.inject or args.bench or args.update_from or args.compare_golden):
        ap.error("choose a mode: --quick, --full, --inject, --bench, --compare-golden or --update-from")
    if args.legacy and args.bench:
        ap.error("--bench measures the RFC 0030 compiler; it has no --legacy form")
    if args.reference_only and (args.quick or args.compare_golden or args.legacy):
        ap.error("--reference-only runs builds, tests, benchmarks and injection checks only")
    if args.update and (args.compare_golden and not (args.quick or args.full or args.inject or args.bench)):
        ap.error("--update records --quick, --full, --inject or --bench measurements")
    if args.update and args.reference_only:
        ap.error("--reference-only measurements are not recorded")
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")
    for key in ("workdir", "manifest", "expected", "triage", "injections", "support_dir", "bench_dir"):
        setattr(args, key, getattr(args, key).resolve())
    return args


def main(argv: list[str]) -> int:
    global VERBOSE
    try:
        args = parse_args(argv)
        VERBOSE = args.verbose
        if args.update_from:
            return update_from(args.update_from, args.expected)
        return Gate(args).run()
    except GateError as exc:
        log(f"error: {exc}")
        return 2
    except KeyboardInterrupt:
        log("interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
