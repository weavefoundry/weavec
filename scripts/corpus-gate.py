#!/usr/bin/env python3
"""The corpus gate: WeaveC on real C projects at pinned revisions.

RFC 0035 (section 11 and the *Acceptance gates*). Reads test/corpus/
(manifest.json, expected.json, triage.json, injections/, bench/, support/)
and runs the configs of manifest.json. test/corpus/README.md documents the
files and the gates.

Modes (combine freely; at least one, or --update-from):

  --quick   per config, `weavec-cc -c -O2 -fweavec-ledger=` of each file,
            whose enforcement ledgers are compared with the expected.json
            ratchet (the proven share of the accesses must not drop, the
            unguarded accesses must not rise). Every PR.
  --full    --quick, then each config's build and test suite with
            CC=weavec-cc in trap mode, rerun in report mode to attribute
            failures and traps to lines, and built once more with the
            reference compiler for the build-time gate; the injections;
            the benchmarks. Weekly.
  --inject  build each injection's patched copy in trap mode and check that
            its run stops at the injected line.
  --bench   best-of-N user CPU of each benchmark built by weavec-cc, by the
            reference compiler and by the reference compiler with ASan.
  --programs
            RFC 0038's programs tier (programs.json): each large program,
            from its pinned release archive, built and run through its own
            test suite with weavec-cc in report mode and with the reference
            compiler; fails on an untriaged report, a test that fails with
            weavec-cc but not the reference (but those acceptedFailures
            lists, each with the RFC that accepts it), and a stale
            referenceFailures or acceptedFailures list. --program NAME selects one (repeatable).

Modifiers:

  --checks verify   build, test and benchmark in verify mode: any
                    `weavec.proven` report fails (gate G6).
  --reference-only  build, test and benchmark with the reference compiler
                    (--cc) alone; checks that the manifest's commands work.
                    With --inject, builds each injection with ASan and checks
                    that its run reaches the injected line.
  --update          record this run's measurements in expected.json;
                    --update-from RESULTS does the same from a --json file
                    written elsewhere (for example by CI).
  --held-out / --no-held-out
                    include or leave out the configs marked heldOut (the
                    sets heldOut, fresh, fresh34 and fresh35). --full includes
                    them, the other modes leave them out.
  --set SET         run the configs of SET alone (repeatable). The sealed
                    sets (sealed, sealed34, sealed35) run only when named by
                    --set or --only.
  --no-asan         benchmarks without the ASan build (no ASan ratio).
  --skip-unavailable
                    leave out the configs whose test suites need a program or
                    a user this machine lacks (test/corpus/prerequisites.json),
                    with a note; without it --full stops with a setup error.

Each config is resolved for this machine's platform (darwin, linux-x86_64,
linux-aarch64): its `platforms` fields replace its own (RFC 0038, section 5).

Examples:

  scripts/corpus-gate.py --quick --only cJSON --only sds
  scripts/corpus-gate.py --quick --update
  scripts/corpus-gate.py --full --json full.json
  scripts/corpus-gate.py --full --checks verify --set fresh35
  scripts/corpus-gate.py --inject --injection sds-uaf-sdsfree
  scripts/corpus-gate.py --full --set sealed35 --reference-only

Exit status: 0 when every check passes, 1 when a check fails, 2 on a usage
or setup error. Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import datetime
import glob
import json
import math
import os
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable, Iterable

ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = ROOT / "test" / "corpus"
DEFAULT_MANIFEST = CORPUS_DIR / "manifest.json"
DEFAULT_EXPECTED = CORPUS_DIR / "expected.json"
DEFAULT_TRIAGE = CORPUS_DIR / "triage.json"
DEFAULT_PREREQUISITES = CORPUS_DIR / "prerequisites.json"
DEFAULT_PROGRAMS = CORPUS_DIR / "programs.json"
DEFAULT_INJECTIONS = CORPUS_DIR / "injections" / "injections.json"
DEFAULT_WORKDIR = ROOT / "build" / "corpus"

RESULTS_SCHEMA = "weavec-corpus-gate-results"
EXPECTED_SCHEMA = "weavec-corpus-expected"
TRIAGE_SCHEMA = "weavec-corpus-triage"
PREREQUISITES_SCHEMA = "weavec-corpus-prerequisites"
PROGRAMS_SCHEMA = "weavec-corpus-programs"
MANIFEST_SCHEMA = "weavec-corpus-manifest"
INJECTIONS_SCHEMA = "weavec-corpus-injections"
# The versions this gate reads: RFC 0035 changed every file.
RESULTS_VERSION = 2
EXPECTED_VERSION = 2
TRIAGE_VERSION = 3
MANIFEST_VERSION = 2
INJECTIONS_VERSION = 2
# The enforcement ledger (RFC 0035, section 9).
LEDGER_SCHEMA = "weavec-ledger"
LEDGER_VERSION = 4
LEDGER_COUNTS = ("accesses", "proven", "guarded", "unguarded")
# The per-file compiles: the optimization real builds use (a config's own
# -O in compile.args comes later and wins).
QUICK_FLAGS = ("-O2",)

# Run-time report kinds (runtime/weavec_report.c); the gate reads reports as
# JSON from WEAVEC_RT_REPORT_LOG (RFC 0040 section 6).
KINDS = (
    "heap-buffer-overflow", "heap-use-after-free", "stack-buffer-overflow",
    "stack-use-after-scope", "dynamic-stack-buffer-overflow", "global-buffer-overflow",
    "intra-object-overflow", "use-after-poison",
    "null-dereference", "unterminated-string", "index-out-of-bounds",
    "double-free", "invalid-free", "alloc-release-mismatch", "invalid-access", "overlapping-copy",
    "stack-use-after-return", "memory-fault", "container-overflow", "library-assertion",
)
# How shells, make and CTest describe a process that died by SIGTRAP or
# SIGILL (a guard's trap), plus the 128+signal exit statuses. CTest 3.29
# prints "SIGTRAP***Exception:" and "***Exception: Illegal"; older versions
# print "***Exception: Other" for SIGTRAP.
TRAP_TEXT_RE = re.compile(
    r"Trace/BPT trap|Trace/breakpoint trap|Illegal instruction|\bSIGTRAP\b|\bSIGILL\b|\(ILLEGAL\)"
    r"|\*\*\*Exception: (?:Illegal|Other)|\bError 13[23]\b|exit (?:status|code) 13[23]\b"
)
# ... and one that died on the null page (RFC 0035, section 2.4).
FAULT_TEXT_RE = re.compile(r"Segmentation fault|Bus error|\bSIGSEGV\b|\bSIGBUS\b|\*\*\*Exception: SegFault"
                           r"|\bError 13[89]\b|exit (?:status|code) 13[89]\b")
TRAP_SIGNALS = (signal.SIGTRAP, signal.SIGILL)
FAULT_SIGNALS = (signal.SIGSEGV, signal.SIGBUS)

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


def died_of(result: ProcResult, signals: Iterable[int], text: re.Pattern) -> list[str]:
    """Evidence that a command, or a process it ran, died of one of `signals`."""
    signals = tuple(signals)
    evidence = []
    if result.signal in signals or result.returncode in tuple(128 + s for s in signals):
        evidence.append(f"{result.command}: {describe_status(result)}")
    for line in result.output.splitlines():
        if text.search(line):
            evidence.append(line.strip()[:300])
    return evidence


def trap_evidence(result: ProcResult) -> list[str]:
    return died_of(result, TRAP_SIGNALS, TRAP_TEXT_RE)


def fault_evidence(result: ProcResult) -> list[str]:
    return died_of(result, FAULT_SIGNALS, FAULT_TEXT_RE)


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


def check_schema(data: Any, path: Path, schema: str, version: int) -> None:
    if not isinstance(data, dict) or data.get("schema") != schema or data.get("version") != version:
        found = f"{data.get('schema')} version {data.get('version')}" if isinstance(data, dict) else "no object"
        raise GateError(f"{path}: schema must be {schema} version {version} (found {found})")


def copy_tree(source: Path, dest: Path) -> None:
    """Copy a checkout, .git included (some builds ask git for a version)."""
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


def normalize_path(file: str) -> str:
    """A report's path without the build directory's relative prefixes ("./x.c", "../x.c")."""
    parts = [p for p in Path(file).as_posix().split("/") if p not in (".", "")]
    while parts and parts[0] == "..":
        parts.pop(0)
    return "/".join(parts)


def same_file(reported: str, expected: str) -> bool:
    """Whether a reported path names the expected file: a build may print
    either the full path or one relative to its own directory."""
    reported, expected = normalize_path(reported), normalize_path(expected)
    return reported == expected or reported.endswith("/" + expected) or expected.endswith("/" + reported)


def get_path(data: Any, path: Iterable[str]) -> Any:
    for key in path:
        if not isinstance(data, dict) or key not in data:
            return None
        data = data[key]
    return data


def share(part: int, whole: int) -> float | None:
    return round(part / whole, 4) if whole else None


def geometric_mean(values: list[float]) -> float | None:
    if not values or any(v <= 0 for v in values):
        return None
    return round(math.exp(sum(math.log(v) for v in values) / len(values)), 4)


# -- manifest -----------------------------------------------------------------


@dataclasses.dataclass
class Project:
    name: str
    url: str
    sha: str
    support: list[str]
    configs: list["Config"] = dataclasses.field(default_factory=list)
    # The checkout's submodules, at the commits its pinned commit records.
    submodules: bool = False
    # Other projects whose checkouts the build reads, as $CORPUS/<name>
    # (googletest for Abseil's tests, test data, Catch2).
    uses: list[str] = dataclasses.field(default_factory=list)


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
    build: list[str] = dataclasses.field(default_factory=list)
    test: list[str] = dataclasses.field(default_factory=list)
    test_timeout: float | None = None
    bench: Bench | None = None
    notes: str = ""
    held_out: bool = False
    set: str = "original"
    # RFC 0039, section 9: a C++ config's files compile with weavec-c++
    # (and the reference clang++); every build gets CXX beside CC.
    language: str = "c"

    @property
    def sealed(self) -> bool:
        return self.set in SEALED_SETS


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


# The corpus sets. `original`: the configs WeaveC was developed on; `heldOut`:
# RFC 0031's held-out projects; then each RFC's fresh projects (measured
# during its work) and sealed ones (built once, at its end). A manifest names
# a set in a config's `set`, which needs `heldOut: true`; without `set` a
# config is original, or heldOut when it is marked heldOut.
SETS = ("original", "heldOut", "fresh", "sealed", "fresh34", "sealed34", "fresh35", "sealed35", "sealed38",
        "fresh39", "sealed39")
NAMED_SETS = SETS[2:]
# The sets that run only when asked for (--set, --only).
SEALED_SETS = ("sealed", "sealed34", "sealed35", "sealed38", "sealed39")
CONFIG_KEYS = frozenset(("name", "notes", "compile", "build", "test", "testTimeout", "bench", "heldOut", "set",
                         "platforms", "language"))
LANGUAGES = ("c", "c++")
# RFC 0038, section 5: a config's `platforms` replaces its fields on a platform, the most specific
# key last.
PLATFORMS = ("darwin", "linux", "linux-x86_64", "linux-aarch64")
PLATFORM_KEYS = frozenset(("compile", "build", "test", "testTimeout", "bench"))


def gate_platform() -> str:
    """The manifest's name for this machine: darwin, linux-x86_64 or linux-aarch64."""
    if sys.platform == "darwin":
        return "darwin"
    machine = platform.machine().lower()
    machine = {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
    return f"linux-{machine}"


def platform_overlays(target: str) -> list[str]:
    """The `platforms` keys that apply on `target`, least specific first."""
    keys = [key for key in PLATFORMS if key == target or target.startswith(key + "-")]
    return sorted(keys, key=len)
BENCH_KEYS = frozenset(("name", "build", "command", "repeat", "input", "check"))

SHA_RE = re.compile(r"[0-9a-f]{40}")


def load_manifest(path: Path, support_root: Path, target: str | None = None) -> Manifest:
    """The manifest, each config resolved for `target` (this machine's platform by default)."""
    target = target or gate_platform()
    data = read_json(path)
    check_schema(data, path, MANIFEST_SCHEMA, MANIFEST_VERSION)
    problems: list[str] = []
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
        project = Project(name=name, url=p.get("url", ""), sha=sha, support=list(p.get("support", [])),
                          submodules=p.get("submodules", False) is True, uses=list(p.get("uses", [])))
        for rel in project.support:
            if not (support_root / rel).is_file():
                problems.append(f"project {name}: support file {rel} is missing from {support_root}")
        for c in p.get("configs", []):
            cname = c.get("name", "?")
            if cname in names:
                problems.append(f"config {cname}: duplicate name")
            names.add(cname)
            unknown = sorted(set(c) - CONFIG_KEYS)
            if unknown:
                problems.append(f"config {cname}: unknown field(s) {', '.join(unknown)}")
            overlays = c.get("platforms") or {}
            for key in sorted(set(overlays) - set(PLATFORMS)):
                problems.append(f"config {cname}: unknown platform {key!r} (expected one of "
                                f"{', '.join(PLATFORMS)})")
            for key, overlay in overlays.items():
                bad = sorted(set(overlay) - PLATFORM_KEYS) if isinstance(overlay, dict) else ["(not an object)"]
                if bad:
                    problems.append(f"config {cname}: platform {key}: unknown field(s) {', '.join(bad)}")
            c = dict(c)
            for key in platform_overlays(target):
                if isinstance(overlays.get(key), dict):
                    c.update(overlays[key])
            compile_ = c.get("compile") or {}
            if not compile_.get("files"):
                problems.append(f"config {cname}: compile.files is missing")
            bench = None
            if c.get("bench"):
                b = c["bench"]
                if not b.get("command") or not b.get("build"):
                    problems.append(f"config {cname}: bench needs build and command")
                unknown = sorted(set(b) - BENCH_KEYS)
                if unknown:
                    problems.append(f"config {cname}: unknown bench field(s) {', '.join(unknown)}")
                bench = Bench(name=b.get("name", cname), build=list(b.get("build", [])),
                              command=b.get("command", ""), repeat=int(b.get("repeat", 7)),
                              input=b.get("input"), check=b.get("check"))
            held_out = c.get("heldOut", False)
            if not isinstance(held_out, bool):
                problems.append(f"config {cname}: heldOut must be true or false")
            corpus_set = "heldOut" if held_out is True else "original"
            if "set" in c:
                if c["set"] not in NAMED_SETS:
                    problems.append(f"config {cname}: set must be one of "
                                    f"{', '.join(repr(s) for s in NAMED_SETS)} (got {c['set']!r}); "
                                    f"without it a config is original, or heldOut when marked heldOut")
                elif held_out is not True:
                    problems.append(f"config {cname}: set {c['set']!r} needs heldOut true")
                else:
                    corpus_set = c["set"]
            language = c.get("language", "c")
            if language not in LANGUAGES:
                problems.append(f"config {cname}: language must be one of {', '.join(LANGUAGES)} (got {language!r})")
            if c.get("test") and not c.get("build"):
                problems.append(f"config {cname}: test needs build")
            timeout = c.get("testTimeout")
            if timeout is not None and (not isinstance(timeout, (int, float)) or timeout <= 0):
                problems.append(f"config {cname}: testTimeout must be a positive number of seconds")
            config = Config(
                name=cname, project=project, files=list(compile_.get("files", [])),
                args=list(compile_.get("args", [])), build=list(c.get("build", [])), test=list(c.get("test", [])), test_timeout=timeout,
                bench=bench, notes=c.get("notes", ""), held_out=held_out is True, set=corpus_set,
                language=language)
            project.configs.append(config)
            configs.append(config)
        projects.append(project)
    known = {project.name for project in projects}
    for project in projects:
        for used in project.uses:
            if used not in known or used == project.name:
                problems.append(f"project {project.name}: uses {used!r}, which is not another project")
    gates = data.get("gates", {})
    for key in ("buildTime", "runTime"):
        enforced = (gates.get(key) or {}).get("enforced", True)
        if not isinstance(enforced, bool):
            problems.append(f"gates.{key}.enforced must be true or false (got {enforced!r})")
    if problems:
        raise GateError(f"{path}: invalid manifest:\n  " + "\n  ".join(problems))
    return Manifest(projects=projects, configs=configs, gates=gates, path=path)


def support_dir(config: Config, support_root: Path) -> Path:
    return support_root / config.project.name


def expand_args(config: Config, support_root: Path) -> list[str]:
    support = str(support_dir(config, support_root))
    return [a.replace("{support}", support) for a in config.args]


def expand_files(root: Path, patterns: Iterable[str]) -> list[Path]:
    """Each pattern's sorted glob, in pattern order."""
    files: list[Path] = []
    for pattern in patterns:
        matches = sorted(glob.glob(str(root / pattern), recursive=True))
        if not matches:
            raise GateError(f"{root}: pattern {pattern!r} matched nothing")
        files.extend(Path(m) for m in matches)
    return files


def select_configs(manifest: Manifest, only: list[str], held_out: bool = False,
                   sets: Iterable[str] = ()) -> list[Config]:
    """The configs named by --only; else those of the sets named by --set;
    else every config outside the sealed sets, the held-out ones only when
    `held_out`."""
    if only:
        known = {c.name for c in manifest.configs}
        unknown = sorted(set(only) - known)
        if unknown:
            raise GateError(f"unknown config(s): {', '.join(unknown)} (known: {', '.join(sorted(known))})")
        return [c for c in manifest.configs if c.name in only]
    wanted = set(sets)
    if wanted:
        return [c for c in manifest.configs if c.set in wanted]
    return [c for c in manifest.configs if not c.sealed and (held_out or not c.held_out)]


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
            update_submodules(project, dest)
        modified = git(["status", "--porcelain", "--untracked-files=no"], dest).strip()
        if modified:
            raise GateError(f"{dest}: tracked files are modified; the gate works on pristine checkouts:\n"
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
        update_submodules(project, dest)
    except GateError:
        remove_tree(dest)
        raise
    head = git(["rev-parse", "HEAD"], dest).strip()
    if head != project.sha:
        raise GateError(f"{dest}: fetched {head}, expected {project.sha}")
    return dest


def update_submodules(project: Project, dest: Path) -> None:
    """A project's submodules (LevelDB's and Snappy's googletest), at the commits its pin records."""
    if project.submodules:
        git(["submodule", "update", "--quiet", "--init", "--recursive", "--depth", "1"], dest)


def tracked_files(root: Path) -> set[str]:
    return set(git(["ls-files"], root).splitlines())


def config_files(config: Config, root: Path, tracked: set[str] | None = None) -> list[Path]:
    files = expand_files(root, config.files)
    if tracked is not None:
        stray = [str(f.relative_to(root)) for f in files if f.relative_to(root).as_posix() not in tracked]
        if stray:
            raise GateError(f"{config.name}: untracked files match the compile patterns: {', '.join(stray)}")
    return files


# -- the enforcement ledger (RFC 0035, section 9) --------------------------------


def add_counts(into: dict, counts: dict, keys: Iterable[str]) -> None:
    for key in keys:
        into[key] = into.get(key, 0) + int(counts.get(key, 0))


def read_ledger(path: Path) -> tuple[dict, collections.Counter]:
    """The summed summary of a ledger's units, and its rows by outcome:reason."""
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{path.name}: {exc}") from None
    if not isinstance(data, dict) or data.get("schema") != LEDGER_SCHEMA:
        raise ValueError(f"{path.name}: not a {LEDGER_SCHEMA} document")
    if data.get("version") != LEDGER_VERSION:
        raise ValueError(f"{path.name}: ledger version {data.get('version')!r}, not {LEDGER_VERSION}")
    units = data.get("units")
    if not isinstance(units, list) or not units:
        raise ValueError(f"{path.name}: no units")
    totals = {key: 0 for key in LEDGER_COUNTS}
    reasons: collections.Counter = collections.Counter()
    for unit in units:
        summary = unit.get("summary")
        if not isinstance(summary, dict):
            raise ValueError(f"{path.name}: a unit has no summary")
        add_counts(totals, summary, LEDGER_COUNTS)
        for row in unit.get("rows") or []:
            reasons[f"{row.get('outcome')}:{row.get('reason')}"] += 1
    return totals, reasons


@dataclasses.dataclass
class CompileRun:
    """One per-file `weavec-cc -c`, and the reference compiler's when timed."""
    file: str
    ledger: dict
    reasons: collections.Counter
    cpu: float
    maxrss: int | None
    reference_cpu: float | None = None
    failure: str = ""


def driver_for(compiler: str, config: Config) -> str:
    """The driver that compiles a config's files: `compiler`, or its C++ sibling for a C++ config."""
    return cxx_of(compiler) if config.language == "c++" else compiler


def compile_command(weavec_cc: str, config: Config, file: Path, support_root: Path, ledger: Path,
                    obj: Path) -> list[str]:
    return [driver_for(weavec_cc, config), "-c", *QUICK_FLAGS, "-fweavec-checks=trap", f"-fweavec-ledger={ledger}",
            *expand_args(config, support_root), str(file), "-o", str(obj)]


def run_compile(weavec_cc: str, reference_cc: str | None, config: Config, root: Path, file: Path,
                support_root: Path, work: Path, timeout: float) -> CompileRun:
    rel = file.relative_to(root).as_posix()
    stem = rel.replace("/", "__")
    ledger = work / f"{stem}.ledger.json"
    obj = work / f"{stem}.o"
    ledger.unlink(missing_ok=True)
    result = run_process(compile_command(weavec_cc, config, file, support_root, ledger, obj), cwd=root,
                         timeout=timeout)
    run = CompileRun(file=rel, ledger={}, reasons=collections.Counter(), cpu=result.cpu, maxrss=result.maxrss)
    if result.returncode != 0 or result.timed_out or result.error:
        lines = result.stderr.strip().splitlines()
        errors = [line for line in lines if " error: " in line or line.startswith("error: ")]
        detail = (errors or lines[-1:])[:1]
        run.failure = f"weavec-cc -c: {describe_status(result)}" + (f": {detail[0][:300]}" if detail else "")
        return run
    try:
        run.ledger, run.reasons = read_ledger(ledger)
    except ValueError as exc:
        run.failure = f"no valid ledger ({exc})"
        return run
    if reference_cc:
        argv = [driver_for(reference_cc, config), "-c", *QUICK_FLAGS, *expand_args(config, support_root), str(file),
                "-o", str(work / f"{stem}.reference.o")]
        reference = run_process(argv, cwd=root, timeout=timeout)
        if reference.returncode == 0 and not reference.timed_out:
            run.reference_cpu = reference.cpu
    return run


# -- one config's --quick measurement -----------------------------------------------


@dataclasses.dataclass
class Quick:
    config: str
    ledger: dict = dataclasses.field(default_factory=lambda: {key: 0 for key in LEDGER_COUNTS})
    reasons: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    units: dict = dataclasses.field(default_factory=dict)
    compile_cpu: float = 0.0
    failures: list[str] = dataclasses.field(default_factory=list)

    @property
    def proven_share(self) -> float | None:
        return share(self.ledger["proven"], self.ledger["accesses"])

    def measured(self) -> dict:
        """The ratchet shape (expected.json)."""
        return {"ledger": {**{key: self.ledger[key] for key in LEDGER_COUNTS}, "provenShare": self.proven_share}}

    def to_json(self) -> dict:
        return {**self.measured(), "reasons": dict(sorted(self.reasons.items())),
                "compileCpuSeconds": round(self.compile_cpu, 2), "units": self.units, "failures": self.failures}

    def add_compile(self, run: CompileRun) -> None:
        self.compile_cpu += run.cpu
        self.units[run.file] = {
            "cpuSeconds": round(run.cpu, 3),
            "referenceCpuSeconds": round(run.reference_cpu, 3) if run.reference_cpu is not None else None,
            "maxRssMiB": round(run.maxrss / 2 ** 20, 1) if run.maxrss else None,
            **({"accesses": run.ledger["accesses"], "proven": run.ledger["proven"]} if run.ledger else {}),
        }
        if run.failure:
            self.failures.append(f"{run.file}: {run.failure}")
            return
        add_counts(self.ledger, run.ledger, LEDGER_COUNTS)
        self.reasons.update(run.reasons)


# -- the ratchet (expected.json) ----------------------------------------------------

# (path, the direction that is better). A worse value fails; a better one is a
# note until --update records it.
RATCHET_FIELDS: tuple[tuple[tuple[str, ...], str], ...] = (
    (("ledger", "provenShare"), "higher"),
    (("ledger", "unguarded"), "lower"),
)
# Counts whose change is a note: the code under test changes them both ways.
TRACKED_FIELDS: tuple[tuple[str, ...], ...] = (
    ("ledger", "accesses"), ("ledger", "proven"), ("ledger", "guarded"),
)
EXPECTED_COMMENT = [
    "The corpus ratchet (RFC 0035, section 11), written by scripts/corpus-gate.py --update (or",
    "--update-from a results file measured elsewhere, for example by CI). For each platform,",
    "configs.<config> records what --quick measured: `ledger`, the enforcement ledgers of the",
    "per-file weavec-cc -c -O2 compiles (accesses, proven, guarded, unguarded, provenShare).",
    "provenShare must not drop and unguarded must not rise; a better value and a change of the",
    "other counts are reported until --update records them. test/corpus/README.md documents the",
    "gate.",
]


@dataclasses.dataclass
class RatchetResult:
    regressions: list[str] = dataclasses.field(default_factory=list)
    improvements: list[str] = dataclasses.field(default_factory=list)
    changes: list[str] = dataclasses.field(default_factory=list)
    missing: list[str] = dataclasses.field(default_factory=list)

    @property
    def failed(self) -> bool:
        return bool(self.regressions)

    def to_json(self) -> dict:
        return dataclasses.asdict(self)


def compare_ratchet(measured: dict[str, dict], expected: dict, platform: str) -> RatchetResult:
    """Check measured configs against expected.json's record for `platform`.

    A config with no record is reported, not failed: --update records it.
    """
    result = RatchetResult()
    section = (expected.get("platforms") or {}).get(platform)
    if section is None:
        if measured:
            result.missing.append(f"nothing recorded for {platform}; --update records it")
        return result
    recorded = section.get("configs") or {}
    for name, now in sorted(measured.items()):
        before = recorded.get(name)
        if before is None:
            result.missing.append(f"{name}: not recorded for {platform}; --update records it")
            continue
        for path, better in RATCHET_FIELDS:
            new, old = get_path(now, path), get_path(before, path)
            label = f"{name}.{'.'.join(path)}"
            if new == old:
                continue
            if new is None or old is None:
                result.changes.append(f"{label}: {old} -> {new}")
            elif (new > old) == (better == "higher"):
                result.improvements.append(f"{label}: {old} -> {new} (better)")
            else:
                result.regressions.append(f"{label}: {old} -> {new} (worse)")
        for path in TRACKED_FIELDS:
            new, old = get_path(now, path), get_path(before, path)
            if new != old:
                result.changes.append(f"{name}.{'.'.join(path)}: {old} -> {new}")
    return result


def merge_expected(expected: dict, measured: dict[str, dict], platform: str, machine: str,
                   producer: str) -> dict:
    """expected.json with these measurements recorded for `platform`."""
    merged = {"schema": EXPECTED_SCHEMA, "version": EXPECTED_VERSION, "_comment": EXPECTED_COMMENT,
              "platforms": json.loads(json.dumps(expected.get("platforms") or {}))}
    section = merged["platforms"].setdefault(platform, {})
    section["machine"] = machine
    section["producer"] = producer
    section["updated"] = datetime.date.today().isoformat()
    configs = section.setdefault("configs", {})
    for name, now in measured.items():
        configs[name] = now
    section["configs"] = dict(sorted(configs.items()))
    merged["platforms"] = dict(sorted(merged["platforms"].items()))
    return merged


def load_expected(path: Path, update: bool) -> dict:
    if not path.exists():
        return {}
    data = read_json(path)
    if isinstance(data, dict) and data.get("schema") == EXPECTED_SCHEMA and data.get("version") == EXPECTED_VERSION:
        return data
    if update:
        log(f"{path}: not {EXPECTED_SCHEMA} version {EXPECTED_VERSION}; --update starts it afresh")
        return {}
    check_schema(data, path, EXPECTED_SCHEMA, EXPECTED_VERSION)
    return data


# -- triage (triage.json) -------------------------------------------------------------

TRIAGE_FIELDS = frozenset(("schema", "version", "_comment", "guardFailures"))
GUARD_FAILURE_KEYS = ("config", "file", "line", "verdict", "note")
# RFC 0038, section 5: a true bug of the project, or a false trap an Accepted RFC accepts.
VERDICTS = ("project-bug", "accepted")


@dataclasses.dataclass
class Triage:
    guard_failures: list[dict]


def load_triage(path: Path) -> Triage:
    """The guard failures of test suites that are true bugs of the project."""
    if not path.exists():
        return Triage([])
    data = read_json(path)
    check_schema(data, path, TRIAGE_SCHEMA, TRIAGE_VERSION)
    problems = []
    unknown = sorted(set(data) - TRIAGE_FIELDS)
    if unknown:
        problems.append(f"unknown field(s) {', '.join(unknown)}")
    failures = list(data.get("guardFailures", []))
    for entry in failures:
        where = f"{entry.get('config')} {entry.get('file')}:{entry.get('line')}"
        missing = [k for k in GUARD_FAILURE_KEYS if k not in entry]
        if missing:
            problems.append(f"guardFailures entry {where}: missing {', '.join(missing)}")
        elif entry["verdict"] not in VERDICTS:
            # A false trap is a bug to fix, unless an RFC accepts it.
            problems.append(f"guardFailures entry {where}: the verdict must be one of "
                            f"{', '.join(VERDICTS)}")
        elif entry["verdict"] == "accepted" and not re.fullmatch(r"\d{4}", str(entry.get("rfc", ""))):
            problems.append(f"guardFailures entry {where}: an accepted false trap names its RFC (rfc)")
        elif "kind" in entry and entry["kind"] not in KINDS:
            problems.append(f"guardFailures entry {where}: unknown kind {entry['kind']!r}")
    if problems:
        raise GateError(f"{path}: invalid triage:\n  " + "\n  ".join(problems))
    return Triage(failures)


PREREQUISITE_KEYS = frozenset(("programs", "refusesRoot"))


def load_prerequisites(path: Path) -> dict[str, dict]:
    """RFC 0038, section 5: per config, the programs its tests run and whether they refuse root."""
    if not path.exists():
        return {}
    data = read_json(path)
    check_schema(data, path, PREREQUISITES_SCHEMA, 1)
    configs = data.get("configs", {})
    problems = [f"config {name}: unknown field(s) {', '.join(sorted(set(spec) - PREREQUISITE_KEYS))}"
                for name, spec in configs.items() if set(spec) - PREREQUISITE_KEYS]
    if problems:
        raise GateError(f"{path}: invalid prerequisites:\n  " + "\n  ".join(problems))
    return configs


def missing_prerequisites(configs: Iterable[Config], prerequisites: dict[str, dict],
                          which: Callable[[str], str | None] = shutil.which,
                          root: bool | None = None) -> dict[str, list[str]]:
    """What each config with a test suite lacks on this machine."""
    if root is None:
        root = hasattr(os, "geteuid") and os.geteuid() == 0
    missing: dict[str, list[str]] = {}
    for config in configs:
        spec = prerequisites.get(config.name) or {}
        if not config.test:
            continue
        lacks = [f"{program} (not on PATH)" for program in spec.get("programs", []) if which(program) is None]
        if spec.get("refusesRoot") and root:
            lacks.append("a user other than root")
        if lacks:
            missing[config.name] = lacks
    return missing


def triaged_failure(guard_failures: list[dict], config: str, report: dict) -> bool:
    """Whether a report is a guard failure triaged as a true bug of the project."""
    for entry in guard_failures:
        if (entry["config"] == config and report.get("file") and int(entry["line"]) == report["line"]
                and same_file(report["file"], entry["file"])
                and entry.get("kind", report["kind"]) == report["kind"]):
            return True
    return False


# -- builds and test suites ---------------------------------------------------------


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
    trap_deaths: list[str] = dataclasses.field(default_factory=list)
    fault_deaths: list[str] = dataclasses.field(default_factory=list)
    reports: list[dict] = dataclasses.field(default_factory=list)
    failures: list[str] = dataclasses.field(default_factory=list)

    @property
    def build_cpu(self) -> float:
        return sum(s.cpu for s in self.steps)

    def to_json(self) -> dict:
        return {"config": self.config, "mode": self.mode, "compiler": self.compiler,
                "built": self.built, "testsPassed": self.tests_passed, "buildCpuSeconds": round(self.build_cpu, 3),
                "steps": [s.to_json() for s in self.steps], "tests": [s.to_json() for s in self.tests],
                "trapDeaths": self.trap_deaths, "faultDeaths": self.fault_deaths, "reports": self.reports,
                "failures": self.failures}


def output_tail(text: str, lines: int = 40) -> str:
    return "\n".join(text.rstrip().splitlines()[-lines:])


def sanitizer_symbolizer(cc: str) -> dict:
    """ASAN_SYMBOLIZER_PATH for a sanitizer run: the llvm-symbolizer beside the
    reference compiler, unless the caller set one. Without it the sanitizer
    runtime on Darwin runs `atos`, which needs the system's permission to
    inspect the dying process and waits when a debugger prompt is pending."""
    if os.environ.get("ASAN_SYMBOLIZER_PATH"):
        return {}
    resolved = shutil.which(cc) or cc
    candidate = Path(resolved).resolve().parent / "llvm-symbolizer"
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return {"ASAN_SYMBOLIZER_PATH": str(candidate)}
    return {}


def cxx_of(compiler: str) -> str:
    """The C++ driver beside a C driver (RFC 0039, section 9): weavec-cc's
    weavec-c++, clang's clang++ (clang-21's clang++-21), cc's c++; a wrapper
    this script wrote has its C++ twin beside it."""
    head, _, name = compiler.rpartition("/")
    if name.endswith("gcc"):
        name = name[:-3] + "g++"
    elif name.endswith("cc"):
        name = name[:-2] + "c++"
    elif re.fullmatch(r"clang(-\d+)?", name):
        name = "clang++" + name[5:]
    else:
        name += "++"
    return f"{head}/{name}" if head else name


def write_wrapper(path: Path, compiler: str, flags: list[str]) -> str:
    """`path` running `compiler` with `flags`, and its C++ twin (`cxx_of`) running the compiler's."""
    path.parent.mkdir(parents=True, exist_ok=True)
    for wrapper, driver in ((path, compiler), (Path(cxx_of(str(path))), cxx_of(compiler))):
        words = " ".join(shlex.quote(w) for w in [driver, *flags])
        wrapper.write_text(f"#!/bin/sh\n# written by scripts/corpus-gate.py\nexec {words} \"$@\"\n")
        wrapper.chmod(0o755)
    return str(path)


def base_env(config: Config, cc: str, src: Path, support_root: Path, bench_dir: Path, jobs: int,
             extra: dict | None = None) -> dict:
    env = dict(os.environ)
    env.update({
        "CC": cc,
        "CXX": cxx_of(cc),
        "JOBS": str(jobs),
        "SRC": str(src),
        "SUPPORT": str(support_dir(config, support_root)),
        "BENCH": str(bench_dir),
    })
    # A build must not pick up the caller's flags, nor the caller's runtime settings.
    for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS", "MFLAGS", "WEAVEC_RT_ABORT",
                "WEAVEC_RT_REPORT_LOG", "WEAVEC_RT_STATS"):
        env.pop(key, None)
    env["WEAVEC_RT_REPORT_FORMAT"] = "json"
    if extra:
        env.update(extra)
    return env


def parse_reports(text: str, root: Path) -> list[dict]:
    """Run-time reports from a JSON report log (RFC 0040 section 6), once
    each: {kind, file, line, col, proven}; file is None for a report whose
    stack names no line either."""
    reports = []
    seen = set()
    for line in text.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if not isinstance(entry, dict) or entry.get("kind") not in KINDS:
            continue
        site = entry.get("site") or {}
        file = site.get("file")
        if file:
            file = normalize_path(rel_path(file, root) if os.path.isabs(file) else file)
        report = {"kind": entry["kind"], "file": file or None, "line": int(site.get("line") or 0),
                  "col": int(site.get("column") or 0), "proven": bool(entry.get("proven"))}
        key = tuple(report.values())
        if key not in seen:
            seen.add(key)
            reports.append(report)
    return reports


def describe_report(report: dict) -> str:
    where = f"{report['file']}:{report['line']}:{report['col']}" if report["file"] else "no location"
    return f"{'weavec.proven: ' if report['proven'] else ''}{report['kind']} at {where}"


def apply_patch(patch: Path, src: Path) -> str:
    """Apply a -p1 patch to a copy; returns a failure message or ''."""
    result = run_process(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=src, timeout=60)
    if result.returncode == 0:
        return ""
    # A source with CRLF lines (printf.c) and a patch with LF ones: GNU patch,
    # unlike BSD patch, refuses them; git apply takes them loosely.
    loose = run_process(["git", "apply", "--whitespace=nowarn", "--ignore-whitespace", str(patch)], cwd=src,
                        timeout=60)
    if loose.returncode == 0:
        return ""
    fallback = run_process(["patch", "-p1", "--batch", "--forward", "-i", str(patch)], cwd=src, timeout=60)
    if fallback.returncode == 0:
        return ""
    return f"patch {patch.name} does not apply: {result.stderr.strip()[:200]} {fallback.output.strip()[:200]}"


def run_build(config: Config, mode: str, compiler: str, flags: list[str], checkout: Path, run_dir: Path,
              support_root: Path, bench_dir: Path, jobs: int, timeout: float, keep: bool, *,
              prepare: Callable[[Path], str] | None = None, build: list[str] | None = None,
              tests: list[str] | None = None, extra_env: dict | None = None) -> BuildRun:
    """Copy the checkout, build it with CC set to `compiler` + `flags`, run the tests.

    `prepare` changes the copy first (an injection's patch) and returns a
    failure message or ''; `build` and `tests` replace the config's commands.
    """
    work = run_dir / mode
    src = work / "src"
    remove_tree(work)
    copy_tree(checkout, src)
    run = BuildRun(config=config.name, mode=mode, compiler=compiler)
    if prepare is not None:
        failure = prepare(src)
        if failure:
            run.failures.append(failure)
            if not keep:
                remove_tree(src)
            return run
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    cc = write_wrapper(work / "bin" / "cc", compiler, flags) if flags else compiler
    # A harness may keep a passing test's output to itself (CTest without
    # -V): the runtime appends every report to this file instead.
    report_log = logs / "runtime-reports.log"
    env = base_env(config, cc, src, support_root, bench_dir, jobs,
                   {**(extra_env or {}), "WEAVEC_RT_REPORT_LOG": str(report_log)})
    run.built = True
    for index, command in enumerate(config.build if build is None else build):
        result = run_shell(command, cwd=src, env=env, timeout=timeout)
        log_path = logs / f"build-{index}.log"
        log_path.write_text(f"$ {command}\n{result.output}")
        ok = result.returncode == 0 and not result.timed_out
        run.steps.append(StepRun(command, describe_status(result), ok, round(result.seconds, 3),
                                 round(result.cpu, 3), str(log_path), output_tail(result.output)))
        if not ok:
            run.built = False
            run.failures.append(f"build step {command!r}: {describe_status(result)}: "
                                f"{output_tail(result.output, 6)}")
            break
    if run.built:
        commands = config.test if tests is None else tests
        test_timeout = config.test_timeout or timeout
        passed = True
        for index, command in enumerate(commands):
            result = run_shell(command, cwd=src, env=env, timeout=test_timeout)
            log_path = logs / f"test-{index}.log"
            log_path.write_text(f"$ {command}\n{result.output}")
            ok = result.returncode == 0 and not result.timed_out
            run.tests.append(StepRun(command, describe_status(result), ok, round(result.seconds, 3),
                                     round(result.cpu, 3), str(log_path), output_tail(result.output)))
            run.trap_deaths.extend(trap_evidence(result))
            run.fault_deaths.extend(fault_evidence(result))
            if not ok:
                passed = False
        run.tests_passed = passed if commands else None
    run.reports = parse_reports(report_log.read_text(errors="replace") if report_log.exists() else "", src)
    if not keep:
        remove_tree(src)
    return run


# -- injections ---------------------------------------------------------------------

INJECTION_KEYS = frozenset(("id", "config", "patch", "file", "line", "stop", "kinds", "unlocated", "build", "run",
                            "fortify", "description", "accepted"))
UNLOCATED_STOPS = ("fault",)


@dataclasses.dataclass
class Injection:
    id: str
    config: str
    patch: str
    file: str
    line: int
    # Where the run stops, when not at the injected line: a freed pointer
    # passed to a function that reads it stops in that function.
    stop: tuple[str, int] | None = None
    kinds: tuple[str, ...] = ()
    unlocated: str | None = None
    build: list[str] | None = None
    run: list[str] | None = None
    fortify: bool = False
    description: str = ""
    # Why a miss is accepted, naming the RFC that accepts it (RFC 0038,
    # amendment 28): the gate does not count it, and fails once it stops.
    accepted: str | None = None

    def commands(self, config: Config) -> list[str]:
        return list(self.run) if self.run is not None else list(config.test)

    @property
    def stop_file(self) -> str:
        return self.stop[0] if self.stop else self.file

    @property
    def stop_line(self) -> int:
        return self.stop[1] if self.stop else self.line


def as_commands(value: Any) -> list[str] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return [value]
    if isinstance(value, list) and all(isinstance(v, str) for v in value):
        return list(value)
    raise ValueError("commands must be a string or a list of strings")


def load_injections(path: Path, manifest: Manifest) -> list[Injection]:
    data = read_json(path)
    check_schema(data, path, INJECTIONS_SCHEMA, INJECTIONS_VERSION)
    names = {c.name for c in manifest.configs}
    problems = []
    injections = []
    ids = set()
    for item in data.get("injections", []):
        ident = item.get("id", "?")
        unknown = sorted(set(item) - INJECTION_KEYS)
        if unknown:
            problems.append(f"{ident}: unknown field(s) {', '.join(unknown)}")
        try:
            stop = item.get("stop")
            inj = Injection(id=item["id"], config=item["config"], patch=item["patch"], file=item["file"],
                            line=int(item["line"]), stop=(stop["file"], int(stop["line"])) if stop else None,
                            kinds=tuple(item.get("kinds", ())),
                            unlocated=item.get("unlocated"), build=as_commands(item.get("build")),
                            run=as_commands(item.get("run")), fortify=bool(item.get("fortify", False)),
                            description=item.get("description", ""), accepted=item.get("accepted"))
        except (KeyError, TypeError, ValueError) as exc:
            problems.append(f"{ident}: {exc}")
            continue
        if inj.id in ids:
            problems.append(f"{inj.id}: duplicate id")
        ids.add(inj.id)
        if inj.config not in names:
            problems.append(f"{inj.id}: unknown config {inj.config}")
            continue
        if not (path.parent / inj.patch).is_file():
            problems.append(f"{inj.id}: patch {inj.patch} not found")
        bad = [k for k in inj.kinds if k not in KINDS]
        if bad:
            problems.append(f"{inj.id}: unknown kind(s) {', '.join(bad)}")
        if inj.unlocated is not None and inj.unlocated not in UNLOCATED_STOPS:
            problems.append(f"{inj.id}: unlocated must be one of {', '.join(UNLOCATED_STOPS)}")
        if inj.accepted is not None and (not isinstance(inj.accepted, str)
                                         or not re.search(r"\bRFC \d{4}\b", inj.accepted)):
            problems.append(f"{inj.id}: accepted must name the RFC that accepts the miss")
        if not inj.commands(manifest.config(inj.config)):
            problems.append(f"{inj.id}: no run commands (and its config has no test)")
        injections.append(inj)
    if problems:
        raise GateError(f"{path}: invalid injections:\n  " + "\n  ".join(problems))
    return injections


def check_injected_line(src: Path, inj: Injection) -> str:
    try:
        lines = (src / inj.file).read_text(errors="replace").splitlines()
    except OSError as exc:
        return f"{inj.file}: {exc}"
    if inj.line > len(lines) or "INJECTED" not in lines[inj.line - 1]:
        return f"{inj.file}:{inj.line} does not carry the INJECTED marker after patching"
    return ""


def stops_at(inj: Injection, build: BuildRun) -> list[str]:
    """How the patched build's run stopped at the injection, if it did: a
    report at the injected line, or at the entry's `stop` (of one of `kinds`,
    when the entry names them), or the unlocated stop the entry allows."""
    via = []
    for report in build.reports:
        if (report["file"] and report["line"] == inj.stop_line and same_file(report["file"], inj.stop_file)
                and (not inj.kinds or report["kind"] in inj.kinds)):
            via.append(describe_report(report))
    if inj.unlocated == "fault":
        via.extend(f"fault: {evidence}" for evidence in build.fault_deaths[:1])
    return via


@dataclasses.dataclass
class InjectionRun:
    injection: Injection
    stopped: bool = False
    via: list[str] = dataclasses.field(default_factory=list)
    reports: list[dict] = dataclasses.field(default_factory=list)
    failures: list[str] = dataclasses.field(default_factory=list)
    asan: dict | None = None

    def to_json(self) -> dict:
        inj = self.injection
        return {"id": inj.id, "config": inj.config, "file": inj.file, "line": inj.line,
                "stop": {"file": inj.stop_file, "line": inj.stop_line}, "kinds": list(inj.kinds),
                "unlocated": inj.unlocated, "stopped": self.stopped, "via": self.via, "reports": self.reports,
                "failures": self.failures, "asan": self.asan}


FRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in (?P<func>\S+) (?P<file>[^\s:()]+):(?P<line>\d+)")
UBSAN_RE = re.compile(r"(?P<file>[^\s:]+):(?P<line>\d+):\d+: runtime error:")


def sanitizer_location(text: str, src: Path) -> tuple[bool, str | None, tuple[str, int] | None]:
    """(found, first report line, (file, line) of the first frame in the project)."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        m = UBSAN_RE.search(line)
        if m:
            return True, line.strip(), (normalize_path(rel_path(m.group("file"), src)), int(m.group("line")))
        if "ERROR: AddressSanitizer" in line:
            for frame in lines[index + 1:index + 80]:
                fm = FRAME_RE.search(frame)
                if fm:
                    file = rel_path(fm.group("file"), src)
                    if not os.path.isabs(file):
                        return True, line.strip(), (normalize_path(file), int(fm.group("line")))
            return True, line.strip(), None
        if "buffer overflow detected" in line or "detected buffer overflow" in line:
            return True, line.strip(), None
    return False, None, None


# -- benchmarks ---------------------------------------------------------------------


@dataclasses.dataclass
class BenchRun:
    config: str
    name: str
    times: dict = dataclasses.field(default_factory=dict)
    minimum: dict = dataclasses.field(default_factory=dict)
    outputs: dict = dataclasses.field(default_factory=dict)
    # The peak resident memory of each build's runs, in bytes (RFC 0036 gate G7).
    peak_rss: dict = dataclasses.field(default_factory=dict)
    ratio: float | None = None
    asan_ratio: float | None = None
    reports: list[dict] = dataclasses.field(default_factory=list)
    failures: list[str] = dataclasses.field(default_factory=list)

    def to_json(self) -> dict:
        return {"config": self.config, "name": self.name, "times": self.times, "minimum": self.minimum,
                "outputs": self.outputs, "peakRssMiB": {k: round(v / 2 ** 20, 1) for k, v in self.peak_rss.items()},
                "ratio": self.ratio, "asanRatio": self.asan_ratio,
                "reports": self.reports, "failures": self.failures}


# -- binaries -----------------------------------------------------------------------


@dataclasses.dataclass
class Binaries:
    weavec_cc: str | None
    reference_cc: str | None


def build_type_of(path: str | None) -> str:
    """CMAKE_BUILD_TYPE of the build tree `path` came out of, or "" if unknown."""
    if not path:
        return ""
    for parent in Path(path).resolve().parents:
        cache = parent / "CMakeCache.txt"
        if not cache.is_file():
            continue
        try:
            for line in cache.read_text(errors="replace").splitlines():
                if line.startswith("CMAKE_BUILD_TYPE:"):
                    return line.partition("=")[2].strip()
        except OSError:
            return ""
        return ""
    return ""


def tool_version(path: str | None) -> str:
    if not path:
        return ""
    try:
        out = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"unavailable: {exc}"
    text = (out.stdout or out.stderr).strip().splitlines()
    # weavec-cc prints Clang's version block first; its own line follows.
    own = [line for line in text if line.startswith("weavec-cc version")]
    return own[0] if own else (text[0] if text else "")


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


# -- the programs tier (RFC 0038, section 5) ------------------------------------------


@dataclasses.dataclass
class Program:
    name: str
    url: str
    sha256: str
    build: list[str]
    test: str
    failures: str
    reference_failures: dict[str, list[str]]
    programs: list[str] = dataclasses.field(default_factory=list)
    # platform -> {test: why}: tests that fail with weavec-cc alone for a
    # reason an RFC accepts (RFC 0038, amendment 18).
    accepted_failures: dict[str, dict[str, str]] = dataclasses.field(default_factory=dict)
    timeout: float = 7200
    notes: str = ""
    # Corpus projects whose checkouts the build reads, as $CORPUS/<name>
    # (googletest for ninja_test).
    uses: list[str] = dataclasses.field(default_factory=list)

    def listed_failures(self, target: str) -> set[str]:
        """The tests that fail with the reference compiler on `target`."""
        listed: list[str] = []
        for key in platform_overlays(target):
            if key in self.reference_failures:
                listed = self.reference_failures[key]
        return set(listed)

    def accepted(self, target: str) -> set[str]:
        """The tests that fail with weavec-cc alone on `target`, as an RFC accepts."""
        accepted: dict[str, str] = {}
        for key in platform_overlays(target):
            if key in self.accepted_failures:
                accepted = self.accepted_failures[key]
        return set(accepted)


PROGRAM_KEYS = frozenset(("name", "url", "sha256", "build", "test", "failures", "referenceFailures",
                          "acceptedFailures", "programs", "timeout", "notes", "platforms", "uses"))


def load_programs(path: Path, target: str | None = None) -> list[Program]:
    """The programs tier, each program's `platforms` fields (build, test) applied for `target`."""
    target = target or gate_platform()
    data = read_json(path)
    check_schema(data, path, PROGRAMS_SCHEMA, 1)
    problems: list[str] = []
    programs: list[Program] = []
    for p in data.get("programs", []):
        name = p.get("name", "?")
        unknown = sorted(set(p) - PROGRAM_KEYS)
        if unknown:
            problems.append(f"program {name}: unknown field(s) {', '.join(unknown)}")
        for key in ("url", "sha256", "build", "test", "failures"):
            if not p.get(key):
                problems.append(f"program {name}: {key} is missing")
        if p.get("sha256") and not re.fullmatch(r"[0-9a-f]{64}", p["sha256"]):
            problems.append(f"program {name}: sha256 must be 64 lowercase hex digits")
        try:
            re.compile(p.get("failures", ""))
        except re.error as exc:
            problems.append(f"program {name}: failures is no regular expression: {exc}")
        listed = p.get("referenceFailures", {})
        for key in sorted(set(listed) - set(PLATFORMS)):
            problems.append(f"program {name}: referenceFailures: unknown platform {key!r}")
        accepted = p.get("acceptedFailures", {})
        for key, tests in accepted.items():
            if key not in PLATFORMS or not isinstance(tests, dict):
                problems.append(f"program {name}: acceptedFailures: bad entry {key!r}")
                continue
            for test, why in tests.items():
                if not isinstance(why, str) or not re.search(r"\bRFC \d{4}\b", why):
                    problems.append(f"program {name}: acceptedFailures: {test} must name the RFC that "
                                    f"accepts it")
        overlays = p.get("platforms") or {}
        for key, overlay in overlays.items():
            if key not in PLATFORMS or not isinstance(overlay, dict) or set(overlay) - {"build", "test"}:
                problems.append(f"program {name}: platforms: bad entry {key!r}")
        p = dict(p)
        for key in platform_overlays(target):
            if isinstance(overlays.get(key), dict):
                p.update(overlays[key])
        programs.append(Program(name=name, url=p.get("url", ""), sha256=p.get("sha256", ""),
                                build=list(p.get("build", [])), test=p.get("test", ""),
                                failures=p.get("failures", ""), reference_failures=dict(listed),
                                accepted_failures={k: dict(v) for k, v in accepted.items()
                                                   if isinstance(v, dict)},
                                programs=list(p.get("programs", [])), timeout=float(p.get("timeout", 7200)),
                                notes=p.get("notes", ""), uses=list(p.get("uses", []))))
    if problems:
        raise GateError(f"{path}: invalid programs:\n  " + "\n  ".join(problems))
    return programs


def failed_tests(pattern: str, output: str) -> set[str]:
    """The tests a suite's output names as failing: each match's first group, split at white space."""
    names: set[str] = set()
    for match in re.finditer(pattern, output, re.MULTILINE):
        names.update(match.group(1).split())
    return names


def fetch_archive(program: Program, cache: Path) -> Path:
    """The program's release archive, downloaded once into `cache` and checked against its sha256."""
    import hashlib
    import urllib.request
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / program.url.rsplit("/", 1)[-1]

    def digest(path: Path) -> str:
        sha = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                sha.update(chunk)
        return sha.hexdigest()

    if not archive.exists() or digest(archive) != program.sha256:
        partial = archive.with_suffix(archive.suffix + ".part")
        log(f"[{program.name}] downloading {program.url}")
        try:
            with urllib.request.urlopen(program.url, timeout=600) as response, partial.open("wb") as out:
                shutil.copyfileobj(response, out)
        except OSError as exc:
            raise GateError(f"{program.name}: cannot download {program.url}: {exc}") from exc
        if digest(partial) != program.sha256:
            partial.unlink()
            raise GateError(f"{program.name}: {program.url} does not match its sha256")
        partial.replace(archive)
    return archive


def unpack(archive: Path, dest: Path) -> Path:
    """Unpacks `archive` into a fresh `dest`; the tree it holds (its one top directory)."""
    import tarfile
    import zipfile
    remove_tree(dest)
    dest.mkdir(parents=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as zipped:
            zipped.extractall(dest)
        # zipfile drops the permission bits: restore the executable ones.
        with zipfile.ZipFile(archive) as zipped:
            for info in zipped.infolist():
                mode = info.external_attr >> 16
                if mode & 0o111:
                    (dest / info.filename).chmod(mode & 0o777)
    else:
        with tarfile.open(archive) as tarred:
            tarred.extractall(dest, filter="tar")
    entries = [p for p in dest.iterdir()]
    return entries[0] if len(entries) == 1 and entries[0].is_dir() else dest


def tcl_config_dir() -> str:
    """The directory of a tclConfig.sh (SQLite's test harness links Tcl), or empty."""
    candidates = ["/usr/lib/tcl8.6", "/usr/lib/tcl9.0", f"/usr/lib/{platform.machine()}-linux-gnu/tcl8.6",
                  "/usr/lib64/tcl8.6", "/opt/homebrew/opt/tcl-tk@8/lib", "/opt/homebrew/opt/tcl-tk/lib",
                  "/usr/local/opt/tcl-tk@8/lib", "/usr/local/opt/tcl-tk/lib"]
    extra = os.environ.get("TCL_CONFIG_DIR")
    for directory in ([extra] if extra else []) + candidates:
        if (Path(directory) / "tclConfig.sh").is_file():
            return directory
    return ""


@dataclasses.dataclass
class ProgramRun:
    mode: str
    built: bool = False
    failures: set[str] = dataclasses.field(default_factory=set)
    reports: list[dict] = dataclasses.field(default_factory=list)
    build_reports: list[dict] = dataclasses.field(default_factory=list)
    steps: list[StepRun] = dataclasses.field(default_factory=list)
    seconds: float = 0.0

    def to_json(self) -> dict:
        return {"built": self.built, "failures": sorted(self.failures), "reports": self.reports,
                "buildReports": self.build_reports,
                "steps": [s.to_json() for s in self.steps], "seconds": round(self.seconds, 1)}


def run_program(program: Program, mode: str, compiler: str, flags: list[str], archive: Path, run_dir: Path,
                jobs: int, keep: bool, corpus: Path | None = None) -> ProgramRun:
    """Builds and tests one program with `compiler`; reports collected through WEAVEC_RT_REPORT_LOG."""
    run = ProgramRun(mode=mode)
    start = time.perf_counter()
    work = run_dir / "programs" / program.name / mode
    src = unpack(archive, work / "src")
    cc = write_wrapper(work / "bin" / "cc", compiler, flags)
    # The test suite's reports are judged; those of the build's own probe
    # programs (configure's conftest) are kept apart.
    build_log = work / "build-reports.log"
    report_log = work / "reports.log"
    env = dict(os.environ)
    for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS", "MFLAGS", "WEAVEC_RT_ABORT",
                "WEAVEC_RT_STATS"):
        env.pop(key, None)
    if corpus is not None:
        env["CORPUS"] = str(corpus.resolve())
    env.update({"CC": cc, "CXX": cxx_of(cc), "JOBS": str(jobs), "PREFIX": str(work / "inst"), "TCL_CONFIG_DIR": tcl_config_dir()})
    env["WEAVEC_RT_REPORT_FORMAT"] = "json"
    build_env = {**env, "WEAVEC_RT_REPORT_LOG": str(build_log)}
    env["WEAVEC_RT_REPORT_LOG"] = str(report_log)
    for command in program.build:
        result = run_shell(command, cwd=src, env=build_env, timeout=program.timeout)
        run.steps.append(StepRun(command=command, status=describe_status(result), ok=result.returncode == 0,
                                 seconds=round(result.seconds, 1), cpu=round(result.cpu, 1), log="",
                                 tail=output_tail(result.output)))
        if result.returncode != 0:
            break
    else:
        run.built = True
        result = run_shell(program.test, cwd=src, env=env, timeout=program.timeout)
        run.steps.append(StepRun(command=program.test, status=describe_status(result), ok=result.returncode == 0,
                                 seconds=round(result.seconds, 1), cpu=round(result.cpu, 1), log="",
                                 tail=output_tail(result.output, 80)))
        run.failures = failed_tests(program.failures, result.output)
        if result.timed_out:
            run.failures.add("(the suite timed out)")
    run.reports = parse_reports(report_log.read_text(errors="replace") if report_log.exists() else "", src)
    run.build_reports = parse_reports(build_log.read_text(errors="replace"), src) if build_log.exists() else []
    run.seconds = time.perf_counter() - start
    if not keep:
        remove_tree(work / "src")
    return run


def judge_program(program: Program, target: str, runs: dict[str, ProgramRun],
                  guard_failures: list[dict]) -> list[str]:
    """What fails the programs tier for one program (RFC 0038 section 5)."""
    failures: list[str] = []
    listed = program.listed_failures(target)
    for mode, run in runs.items():
        if not run.built:
            failures.append(f"{program.name} ({mode}): the build failed: {run.steps[-1].status}")
    reference = runs.get("reference")
    if reference is not None and reference.built:
        for name in sorted(listed - reference.failures):
            failures.append(f"{program.name}: {name} passes with the reference compiler: remove it from "
                            f"referenceFailures")
        for name in sorted(reference.failures - listed):
            failures.append(f"{program.name}: {name} fails with the reference compiler too: list it in "
                            f"referenceFailures")
    checked = runs.get("report")
    accepted = program.accepted(target)
    if checked is not None and checked.built:
        for name in sorted(checked.failures - listed - accepted):
            failures.append(f"{program.name}: {name} fails with weavec-cc")
        for name in sorted(accepted - checked.failures):
            failures.append(f"{program.name}: {name} passes with weavec-cc: remove it from "
                            f"acceptedFailures")
        for report in checked.reports:
            if not triaged_failure(guard_failures, program.name, report):
                failures.append(f"{program.name}: an untriaged report: {describe_report(report)}")
    return failures


# -- the gates of RFC 0035 the corpus measures ----------------------------------------
#
# Each takes the selected configs and the results' per-config entries and
# returns (ok, detail); ok is None when nothing was measured.


def drop_in_gate(configs: list[Config], entries: dict, checks: str) -> tuple[bool | None, dict]:
    """G1-G3: every build passes and every test suite passes, in trap (or
    verify) mode and in report mode, with no guard failing but those triaged
    as true bugs of the project."""
    detail = {}
    ok = True
    for config in configs:
        if not config.build:
            continue
        entry = entries.get(config.name) or {}
        builds = entry.get("builds") or {}
        checked, report = builds.get(checks) or {}, builds.get("report") or {}
        row = {"set": config.set, "built": checked.get("built"), "testsPassed": checked.get("testsPassed"),
               "reportBuilt": report.get("built"), "reportTestsPassed": report.get("testsPassed"),
               "trueTrapsOnly": bool(entry.get("trueTrapsOnly")), "traps": entry.get("traps")}
        detail[config.name] = row
        ok &= bool(row["built"]) and bool(row["reportBuilt"])
        ok &= row["testsPassed"] is not False or row["trueTrapsOnly"]
        ok &= row["reportTestsPassed"] is not False
        ok &= not row["traps"]
    return (ok if detail else None), detail


def build_time_gate(configs: list[Config], entries: dict, spec: dict) -> tuple[bool | None, dict]:
    """G9: each config's weavec-cc build CPU time within maxBuildRatio of the
    reference compiler's, and no per-file compile above maxUnitRatio (among
    those the reference compiler takes at least minUnitCpuSeconds for: below
    that, the ratio is start-up noise). C++ configs (RFC 0039 G8) take
    `cxx`'s maxBuildRatio, and their builds' geometric mean at most its
    maxGeometricMean."""
    min_cpu = spec.get("minUnitCpuSeconds", 0.25)
    cxx = spec.get("cxx") or {}
    detail: dict = {}
    ok = True
    cxx_ratios = []
    for config in configs:
        limits = {**spec, **cxx} if config.language == "c++" else spec
        max_build, max_unit = limits.get("maxBuildRatio"), limits.get("maxUnitRatio")
        entry = entries.get(config.name) or {}
        row: dict = {}
        ratio = entry.get("buildCpuRatio")
        if ratio is not None:
            row["buildCpuRatio"] = {"value": ratio, "limit": max_build}
            if config.language == "c++":
                cxx_ratios.append(ratio)
            if max_build is not None:
                ok &= ratio <= max_build
        worst = None
        for file, cost in ((entry.get("quick") or {}).get("units") or {}).items():
            base = cost.get("referenceCpuSeconds")
            if base is None or base < min_cpu:
                continue
            unit_ratio = round(cost["cpuSeconds"] / base, 3)
            if worst is None or unit_ratio > worst[1]:
                worst = (file, unit_ratio)
        if worst is not None:
            row["worstUnit"] = {"file": worst[0], "value": worst[1], "limit": max_unit}
            if max_unit is not None:
                ok &= worst[1] <= max_unit
        if row:
            detail[config.name] = row
    mean = geometric_mean(cxx_ratios)
    if mean is not None:
        detail["cxxGeometricMean"] = {"value": mean, "limit": cxx.get("maxGeometricMean"), "configs": len(cxx_ratios)}
        if cxx.get("maxGeometricMean") is not None:
            ok &= mean <= cxx["maxGeometricMean"]
    return (ok if detail else None), detail


def run_time_gate(configs: list[Config], all_configs: list[Config], entries: dict,
                  spec: dict) -> tuple[bool | None, dict]:
    """G7: each workload of the sets in `sets` at most the larger of
    maxOverhead and its ASan ratio, their geometric mean at most
    maxGeometricMean (once every such workload of the manifest ran), and the
    workloads of maxOverheadPerConfig at their own limits. The C++ workloads
    (RFC 0039 G7): their geometric mean at most `cxx`'s maxGeometricMean
    and below ASan's."""
    limit = spec.get("maxOverhead")
    cxx = spec.get("cxx") or {}
    cxx_ratios, cxx_asan = [], []
    sets = set(spec.get("sets", ()))
    per_config = spec.get("maxOverheadPerConfig") or {}
    detail: dict = {}
    ok = True
    ratios = {}
    for config in configs:
        bench = (entries.get(config.name) or {}).get("bench")
        if not bench or bench.get("ratio") is None:
            continue
        ratio, asan = bench["ratio"], bench.get("asanRatio")
        bound = None
        if config.name in per_config:
            bound = per_config[config.name]
        elif config.set in sets and limit is not None:
            bound = max(limit, asan or 0)
        if config.set in sets:
            ratios[config.name] = ratio
        if config.language == "c++":
            cxx_ratios.append(ratio)
            if asan is not None:
                cxx_asan.append(asan)
        detail[config.name] = {"ratio": ratio, "asanRatio": asan, "limit": bound}
        if bound is not None:
            ok &= ratio <= bound
    mean = geometric_mean(list(ratios.values()))
    if mean is not None:
        complete = all(c.name in ratios for c in all_configs if c.set in sets and c.bench)
        detail["geometricMean"] = {"value": mean, "limit": spec.get("maxGeometricMean"), "complete": complete,
                                   "configs": len(ratios)}
        if complete and spec.get("maxGeometricMean") is not None:
            ok &= mean <= spec["maxGeometricMean"]
    mean = geometric_mean(cxx_ratios)
    if mean is not None:
        asan_mean = geometric_mean(cxx_asan) if len(cxx_asan) == len(cxx_ratios) else None
        detail["cxxGeometricMean"] = {"value": mean, "limit": cxx.get("maxGeometricMean"), "asan": asan_mean,
                                      "configs": len(cxx_ratios)}
        if cxx.get("maxGeometricMean") is not None:
            ok &= mean <= cxx["maxGeometricMean"]
        if asan_mean is not None:
            ok &= mean < asan_mean
    return (ok if detail else None), detail


def verify_gate(configs: list[Config], entries: dict) -> tuple[bool | None, dict]:
    """G6: no `weavec.proven` report on the test suites or the benchmarks."""
    detail = {}
    for config in configs:
        entry = entries.get(config.name) or {}
        proven = [describe_report(r) for build in (entry.get("builds") or {}).values()
                  for r in build.get("reports", []) if r["proven"]]
        proven += [describe_report(r) for r in (entry.get("bench") or {}).get("reports", []) if r["proven"]]
        if entry.get("builds") or entry.get("bench"):
            detail[config.name] = sorted(set(proven))
    return (not any(detail.values()) if detail else None), detail


# -- the gate -----------------------------------------------------------------------


class Gate:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.support_root = args.support_dir
        self.bench_dir = args.bench_dir
        self.manifest = load_manifest(args.manifest, self.support_root)
        self.configs = select_configs(self.manifest, args.only, with_held_out(args), args.set)
        if not self.configs:
            raise GateError(f"no config is in the set(s) {', '.join(args.set)}")
        self.selected = {c.name for c in self.configs}
        self.platform = platform_key()
        self.machine = machine_key()
        self.failures: list[str] = []
        self.tool_failures: set[str] = set()  # configs whose measurement is incomplete
        self.notes: list[str] = []
        self.results: dict[str, Any] = {
            "schema": RESULTS_SCHEMA, "version": RESULTS_VERSION, "started": now_iso(),
            "platform": self.platform, "machine": self.machine,
            "modes": [m for m in ("quick", "full", "inject", "bench", "programs") if getattr(args, m)],
            "checks": args.checks, "referenceOnly": args.reference_only,
            "sets": {s: sorted(c.name for c in self.configs if c.set == s) for s in SETS
                     if any(c.set == s for c in self.configs)},
            "configs": {}, "gates": {},
        }
        self.checkouts: dict[str, Path] = {}
        self.tracked: dict[str, set[str]] = {}
        self.checkout_lock = threading.Lock()
        self.run_dir = args.workdir / ".gate" / f"{int(time.time())}-{os.getpid()}"
        self.jobs = max(1, args.jobs)
        self.triage = load_triage(args.triage)
        if args.full:
            missing = missing_prerequisites(self.configs, load_prerequisites(args.prerequisites))
            if missing and args.skip_unavailable:
                for name, lacks in missing.items():
                    self.notes.append(f"{name}: left out: needs {', '.join(lacks)}")
                self.configs = [c for c in self.configs if c.name not in missing]
                self.selected = {c.name for c in self.configs}
            elif missing:
                raise GateError("the test suites need what this machine lacks (test/corpus/prerequisites.json; "
                                "--skip-unavailable leaves them out):\n  "
                                + "\n  ".join(f"{name}: {', '.join(lacks)}" for name, lacks in missing.items()))
        self.measured: dict[str, dict] = {}
        self.injection_runs: list[InjectionRun] = []
        self.binaries = self.resolve_binaries()
        self.announce_binaries()

    # ---- setup ----

    def resolve_binaries(self) -> Binaries:
        a = self.args
        built = [ROOT / "build" / "release" / "bin", ROOT / "build" / "dev" / "bin"]
        cc_needed = (a.quick or a.full or a.inject or a.bench or a.programs) and not a.reference_only
        reference_needed = a.reference_only or a.full or a.bench or a.programs
        weavec_cc = resolve_binary(a.weavec_cc, [d / "weavec-cc" for d in built], "--weavec-cc", cc_needed)
        reference = a.cc or default_reference_cc()
        if reference:
            reference = resolve_binary(reference, [], "--cc", reference_needed)
        elif reference_needed:
            raise GateError("--cc: no reference compiler; set WEAVEC_LLVM_PREFIX or pass --cc")
        if a.reference_only:
            weavec_cc = None
        self.results["binaries"] = {
            name: {"path": path, "version": tool_version(path)}
            for name, path in (("weavec-cc", weavec_cc), ("reference-cc", reference))
            if path
        }
        return Binaries(weavec_cc, reference)

    def announce_binaries(self) -> None:
        """Say which binaries are under test, and refuse to time a Debug one:
        its verdicts are right and its timings mean nothing."""
        for what, path in (("weavec-cc", self.binaries.weavec_cc), ("reference compiler", self.binaries.reference_cc)):
            if not path:
                continue
            build = build_type_of(path)
            log(f"{what}: {path}" + (f" ({build})" if build else
                                     " (build type unknown: an installed tree?)"
                                     if what != "reference compiler" else ""))
            if what == "reference compiler" or not build or build.lower() == "release":
                continue
            if self.args.bench and not self.args.weavec_cc:
                raise GateError(f"{what}: {path} is a {build} build and --bench measures CPU time. "
                                f"Pass --{what} explicitly, or build a Release tree.")
            if self.args.full:
                log(f"warning: {what} is a {build} build, so the timings in this run mean nothing")

    def checkout(self, config: Config) -> Path:
        project = config.project
        for used in project.uses:
            self.checkout_project(next(p for p in self.manifest.projects if p.name == used))
        return self.checkout_project(project)

    def build_env(self, config: Config) -> dict:
        """What every build of a config sees beside CC and CXX: its cache and the corpus."""
        cache = self.args.workdir / ".cache" / config.project.name
        cache.mkdir(parents=True, exist_ok=True)
        return {"CACHE": str(cache), "CORPUS": str(self.args.workdir.resolve())}

    def checkout_project(self, project: Project) -> Path:
        with self.checkout_lock:
            if project.name not in self.checkouts:
                path = ensure_checkout(project, self.args.workdir, self.args.fetch, self.args.offline)
                self.tracked[project.name] = tracked_files(path)
                self.results.setdefault("commits", {})[project.name] = project.sha
                self.checkouts[project.name] = path
            return self.checkouts[project.name]

    def entry(self, config: Config | str) -> dict:
        name = config if isinstance(config, str) else config.name
        return self.results["configs"].setdefault(name, {"set": self.manifest.config(name).set})

    def fail(self, message: str, config: str | None = None) -> None:
        """Record a failure; with `config`, one after which that config's
        measurements are incomplete and are not recorded by --update."""
        self.failures.append(message)
        if config is not None:
            self.tool_failures.add(config)
        log(f"FAIL: {message}")

    def note(self, message: str) -> None:
        self.notes.append(message)
        log(f"note: {message}")

    def gate(self, name: str, ok: bool | None, detail: Any, failed_above: bool = False,
             enforced: bool = True) -> None:
        """Record a gate. `failed_above`: each of its failures was already
        recorded on its own, so the gate adds no failure of its own. A gate
        that is not `enforced` is measured and recorded: over its limits it
        is `over`, a note, not a failure."""
        status = "skip" if ok is None else "pass" if ok else "fail" if enforced else "over"
        self.results["gates"][name] = {"status": status, "detail": detail}
        if status == "fail" and not failed_above:
            self.fail(f"gate {name}: {json.dumps(detail)[:600]}")
        elif status == "over":
            self.note(f"gate {name}: over its limits, recorded only: {json.dumps(detail)[:600]}")
        else:
            log(f"gate {name}: {status}")

    def probe(self) -> None:
        """Fail early, and clearly, on binaries that predate RFC 0035."""
        scratch = self.run_dir / "probe"
        scratch.mkdir(parents=True, exist_ok=True)
        source = scratch / "probe.c"
        source.write_text("int weavec_probe(int *p) { return *p; }\n")
        ledger = scratch / "probe.ledger.json"
        result = run_process([self.binaries.weavec_cc, "-c", f"-fweavec-ledger={ledger}", str(source),
                              "-o", str(scratch / "probe.o")], cwd=scratch, timeout=120)
        try:
            read_ledger(ledger)
        except ValueError as exc:
            raise GateError(f"{self.binaries.weavec_cc} does not write a {LEDGER_SCHEMA} version {LEDGER_VERSION} "
                            f"ledger (RFC 0035): {describe_status(result)}; {exc}; "
                            f"{result.stderr.strip()[:300]}") from None

    # ---- quick ----

    def run_quick(self) -> None:
        self.probe()
        timed = self.binaries.reference_cc if self.args.full else None
        log(f"quick: weavec-cc {self.binaries.weavec_cc}"
            + (f", per-file compiles timed against {timed}" if timed else ""))
        tasks: list[tuple[Config, Callable[[], Any]]] = []
        for config in self.configs:
            root = self.checkout(config)
            files = config_files(config, root, self.tracked[config.project.name])
            work = self.run_dir / "quick" / config.name
            work.mkdir(parents=True, exist_ok=True)
            for file in files:
                tasks.append((config, lambda c=config, r=root, f=file, w=work: run_compile(
                    self.binaries.weavec_cc, timed, c, r, f, self.support_root, w, self.args.timeout)))
        quick = {c.name: Quick(config=c.name) for c in self.configs}
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            futures = {pool.submit(task): config for config, task in tasks}
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                measure = quick[futures[future].name]
                measure.add_compile(result)
                vlog(f"[{measure.config}] weavec-cc -c {result.file}: {result.failure or 'ok'}")
        for config in self.configs:
            self.record_quick(config, quick[config.name])

    def record_quick(self, config: Config, measure: Quick) -> None:
        self.entry(config)["quick"] = measure.to_json()
        for failure in measure.failures:
            self.fail(f"{config.name}: {failure}", config=config.name)
        if not measure.failures:
            self.measured[config.name] = measure.measured()
        provenness = measure.proven_share
        log(f"[{config.name}] {measure.ledger['accesses']} accesses, "
            f"{'-' if provenness is None else f'{provenness:.1%}'} proven, {measure.ledger['unguarded']} unguarded "
            f"({measure.compile_cpu:.1f} s CPU)")

    # ---- full: builds and tests ----

    def compiler_for(self, mode: str) -> tuple[str, list[str]]:
        if mode == "reference":
            return self.binaries.reference_cc, []
        return self.binaries.weavec_cc, [f"-fweavec-checks={mode}"]

    def run_builds(self) -> None:
        if self.args.reference_only:
            modes = ["reference"]
        else:
            # The rerun in report mode names every failing guard; the
            # reference build only times the build (gate build-time).
            modes = [self.args.checks, "report", "reference"]
        for config in self.configs:
            if not config.build:
                continue
            checkout = self.checkout(config)
            entry = self.entry(config)
            runs: dict[str, BuildRun] = {}
            for mode in modes:
                compiler, flags = self.compiler_for(mode)
                log(f"[{config.name}] {mode} build with {compiler}")
                timed_only = mode == "reference" and not self.args.reference_only
                build = run_build(config, mode, compiler, flags, checkout, self.run_dir / "builds" / config.name,
                                  self.support_root, self.bench_dir, self.jobs, self.args.build_timeout,
                                  self.args.keep, tests=[] if timed_only else None,
                                  extra_env=self.build_env(config))
                runs[mode] = build
                for failure in build.failures:
                    self.fail(f"{config.name} ({mode}): {failure}", config=config.name)
                log(f"[{config.name}] {mode}: built={build.built} tests={build.tests_passed} "
                    f"({build.build_cpu:.1f} s build CPU, {len(build.trap_deaths)} trap signs, "
                    f"{len(build.reports)} reports)")
            entry["builds"] = {mode: run.to_json() for mode, run in runs.items()}
            self.judge_tests(config, runs)
            if not self.args.reference_only:
                entry["traps"] = self.count_traps(config, runs)
                checked, reference = runs.get(self.args.checks), runs.get("reference")
                if checked and reference and checked.built and reference.built and reference.build_cpu > 0:
                    entry["buildCpuRatio"] = round(checked.build_cpu / reference.build_cpu, 3)

    def judge_tests(self, config: Config, runs: dict[str, BuildRun]) -> None:
        for mode, build in runs.items():
            if not build.built or build.tests_passed is not False:
                continue
            if mode == self.args.checks and self.only_triaged_failures(config, runs):
                self.note(f"{config.name} ({mode}): the test suite trapped only at guard failures triaged as "
                          f"true bugs")
                self.entry(config)["trueTrapsOnly"] = True
                continue
            failed = [t for t in build.tests if not t.ok]
            self.fail(f"{config.name} ({mode}): test suite failed: " + "; ".join(
                f"{t.command!r}: {t.status}: {output_tail(t.tail, 6)}" for t in failed), config=config.name)

    def only_triaged_failures(self, config: Config, runs: dict[str, BuildRun]) -> bool:
        """A trap-mode test failure explained by triaged guard failures alone:
        the report-mode rerun passes and names only triaged sites."""
        report = runs.get("report")
        if report is None or not report.built or report.tests_passed is False or not report.reports:
            return False
        return all(triaged_failure(self.triage.guard_failures, config.name, r) for r in report.reports)

    def count_traps(self, config: Config, runs: dict[str, BuildRun]) -> int:
        """Failing guards in the test suites: each site that the checked run or
        the report-mode rerun names once, except guard failures triaged as
        true bugs; each report without a location once; a trap-mode death
        that no report explains once. A `weavec.proven` report in verify mode
        fails gate G6 as well."""
        checked = runs.get(self.args.checks)
        reports: dict[tuple, dict] = {}
        for build in (checked, runs.get("report")):
            for r in (build.reports if build else []):
                reports.setdefault((r["kind"], r["file"], r["line"], r["col"], r["proven"]), r)
        counted = [r for r in reports.values() if not triaged_failure(self.triage.guard_failures, config.name, r)]
        for r in counted:
            self.fail(f"{config.name}: a guard failed in the test suite: {describe_report(r)}")
        proven = [r for r in counted if r["proven"]]
        self.entry(config)["provenReports"] = len(proven)
        deaths = checked.trap_deaths if checked else []
        unexplained = bool(deaths) and not reports
        if unexplained:
            self.fail(f"{config.name}: the test suite trapped ({deaths[0]}) and no report names a guard")
        stale = [e for e in self.triage.guard_failures if e["config"] == config.name and not any(
            triaged_failure([e], config.name, r) for r in reports.values())]
        for e in stale:
            self.note(f"stale guardFailures entry ({config.name} {e['file']}:{e['line']})")
        return len(counted) + (1 if unexplained else 0)

    # ---- injections ----

    def run_injections(self) -> None:
        injections = [i for i in load_injections(self.args.injections, self.manifest) if i.config in self.selected]
        if self.args.injection:
            wanted = set(self.args.injection)
            unknown = wanted - {i.id for i in injections}
            if unknown:
                raise GateError(f"unknown or unselected injection(s): {', '.join(sorted(unknown))}")
            injections = [i for i in injections if i.id in wanted]
        if not injections:
            log("injections: none for the selected configs")
            return
        log(f"injections: {len(injections)}" + (" (ASan)" if self.args.reference_only else ""))
        for inj in injections:
            self.checkout(self.manifest.config(inj.config))
        workers = max(1, min(len(injections), self.jobs // 2 or 1))
        runs: dict[str, InjectionRun] = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(self.run_injection, inj): inj for inj in injections}
            for future in concurrent.futures.as_completed(futures):
                inj = futures[future]
                try:
                    run = future.result()
                except GateError as exc:
                    run = InjectionRun(injection=inj, failures=[str(exc)])
                runs[inj.id] = run
                log(f"[inject] {inj.id}: {self.describe_injection(run)}")
        ordered = [runs[i.id] for i in injections]
        self.injection_runs = ordered
        self.results["injections"] = [r.to_json() for r in ordered]
        for run in ordered:
            for failure in run.failures:
                self.fail(f"injection {run.injection.id}: {failure}")
        if self.args.reference_only:
            for r in ordered:
                if r.asan is None or r.failures:
                    continue
                if not r.asan.get("reached"):
                    self.fail(f"injection {r.injection.id}: its run does not reach the bug "
                              f"({r.asan['sanitizer']} reports nothing)")
                elif r.asan.get("atLine") is False:
                    self.fail(f"injection {r.injection.id}: {r.asan['sanitizer']} reports "
                              f"{r.asan.get('location')}, not {r.injection.stop_file}:{r.injection.stop_line}")
            return
        print_injection_table(ordered)
        missed = [r.injection.id for r in ordered if not r.stopped]
        accepted = [r.injection.id for r in ordered if not r.stopped and r.injection.accepted]
        stale = [r.injection.id for r in ordered if r.stopped and r.injection.accepted]
        for ident in stale:
            self.fail(f"injection {ident} stops now: remove its accepted miss")
        # Stale entries were each recorded above; anything else fails here.
        rest_ok = len(missed) == len(accepted) and not any(r.failures for r in ordered)
        self.gate("injections", rest_ok and not stale,
                  {"stopped": len(ordered) - len(missed), "total": len(ordered), "missed": missed,
                   "accepted": accepted, "stale": stale}, failed_above=rest_ok)

    def describe_injection(self, run: InjectionRun) -> str:
        if run.failures:
            return "; ".join(run.failures)
        if run.asan is not None:
            a = run.asan
            if a.get("atLine"):
                return f"{a['sanitizer']} reports the injected line"
            if a.get("reached"):
                return f"{a['sanitizer']}: {a.get('report')} (at {a.get('location') or 'no location'})"
            return f"{a['sanitizer']}: nothing reported"
        if run.stopped:
            return f"stops ({run.via[0]})"
        seen = ", ".join(describe_report(r) for r in run.reports[:3]) or "no report"
        return f"does not stop at {run.injection.stop_file}:{run.injection.stop_line} ({seen})"

    def run_injection(self, inj: Injection) -> InjectionRun:
        config = self.manifest.config(inj.config)
        checkout = self.checkout(config)
        run = InjectionRun(injection=inj)
        work = self.run_dir / "inject" / inj.id
        patch = (self.args.injections.parent / inj.patch).resolve()

        def prepare(src: Path) -> str:
            return apply_patch(patch, src) or check_injected_line(src, inj)

        extra = {"INJECTION_DIR": str(patch.parent), **self.build_env(config)}
        try:
            if self.args.reference_only:
                run.asan = self.asan_injection(inj, config, checkout, prepare, work, extra)
                return run
            build = run_build(config, "trap", self.binaries.weavec_cc, ["-fweavec-checks=trap"], checkout, work,
                              self.support_root, self.bench_dir, self.jobs, self.args.build_timeout,
                              self.args.keep, prepare=prepare, build=inj.build, tests=inj.commands(config),
                              extra_env=extra)
            if not build.built:
                run.failures.append("the patched copy does not build: " + "; ".join(build.failures))
                return run
            run.reports = build.reports
            run.via = stops_at(inj, build)
            run.stopped = bool(run.via)
        finally:
            if not self.args.keep:
                remove_tree(work)
        return run

    def asan_injection(self, inj: Injection, config: Config, checkout: Path, prepare: Callable[[Path], str],
                       work: Path, extra: dict) -> dict:
        """Check that an injection's run reaches the injected line: the
        reference compiler with ASan and UBSan, or, for `fortify` entries
        (short writes ASan cannot see), with _FORTIFY_SOURCE."""
        cc = self.binaries.reference_cc
        if inj.fortify:
            flags = ["-O1", "-g", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=2"]
        else:
            flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-g", "-O1",
                     "-fno-omit-frame-pointer"]
        build = run_build(config, "asan", cc, flags, checkout, work, self.support_root, self.bench_dir, self.jobs,
                          self.args.build_timeout, self.args.keep, prepare=prepare, build=inj.build,
                          tests=inj.commands(config),
                          extra_env={**extra, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "print_stacktrace=1",
                                     **sanitizer_symbolizer(cc)})
        text = ""
        for step in build.tests:
            try:
                text += Path(step.log).read_text(errors="replace")
            except OSError:
                pass
        found, report, location = sanitizer_location(text, work / "asan" / "src")
        if inj.fortify and not found and build.trap_deaths:
            found, report = True, f"fortified call trapped: {build.trap_deaths[0]}"
        at_line = None if location is None else (same_file(location[0], inj.stop_file)
                                                 and location[1] == inj.stop_line)
        return {"built": build.built, "sanitizer": "fortify" if inj.fortify else "asan+ubsan",
                "reached": found, "report": report,
                "location": f"{location[0]}:{location[1]}" if location else None,
                "atLine": at_line, "failures": build.failures}

    # ---- benchmarks ----

    def run_programs(self) -> None:
        """RFC 0038, section 5: the programs tier."""
        programs = load_programs(self.args.programs_file)
        if self.args.program:
            unknown = sorted(set(self.args.program) - {p.name for p in programs})
            if unknown:
                raise GateError(f"unknown program(s): {', '.join(unknown)}")
            programs = [p for p in programs if p.name in self.args.program]
        lacking = {p.name: [q for q in p.programs if shutil.which(q) is None] for p in programs}
        lacking = {name: what for name, what in lacking.items() if what}
        if lacking:
            raise GateError("the programs' test suites need what this machine lacks:\n  " + "\n  ".join(
                f"{name}: {', '.join(what)} (not on PATH)" for name, what in lacking.items()))
        target = gate_platform()
        results: dict[str, Any] = {}
        failed = False
        projects = {project.name: project for project in self.manifest.projects}
        for program in programs:
            unknown = [name for name in program.uses if name not in projects]
            if unknown:
                raise GateError(f"program {program.name}: uses {', '.join(unknown)}, not projects of the manifest")
            for name in program.uses:
                self.checkout_project(projects[name])
            archive = fetch_archive(program, self.args.workdir / ".programs")
            runs: dict[str, ProgramRun] = {}
            for mode in ("reference",) if self.args.reference_only else ("reference", "report"):
                compiler, flags = self.compiler_for(mode)
                log(f"[{program.name}] {mode} build and test suite with {compiler}")
                runs[mode] = run_program(program, mode, compiler, flags, archive, self.run_dir, self.jobs,
                                         self.args.keep, self.args.workdir)
                log(f"[{program.name}] {mode}: built={runs[mode].built} "
                    f"failures={len(runs[mode].failures)} reports={len(runs[mode].reports)} "
                    f"({runs[mode].seconds:.0f} s)")
            problems = judge_program(program, target, runs, self.triage.guard_failures)
            for problem in problems:
                self.fail(f"programs: {problem}")
            failed = failed or bool(problems)
            results[program.name] = {"runs": {mode: run.to_json() for mode, run in runs.items()},
                                     "referenceFailures": sorted(program.listed_failures(target)),
                                     "acceptedFailures": sorted(program.accepted(target)),
                                     "problems": problems}
        self.results["programs"] = results
        self.gate("programs", not failed, {name: r["problems"] for name, r in results.items()})

    def run_benches(self) -> None:
        for config in self.configs:
            if config.bench:
                self.run_bench(config)

    def run_bench(self, config: Config) -> None:
        bench = config.bench
        repeat = self.args.repeat or bench.repeat
        checkout = self.checkout(config)
        run = BenchRun(config=config.name, name=bench.name)
        work = self.run_dir / "bench" / config.name
        remove_tree(work)
        reference = self.binaries.reference_cc
        compilers = [("reference", reference, {})]
        if not self.args.reference_only:
            compilers.append(("weavec-cc", write_wrapper(work / "bin" / "weavec-cc", self.binaries.weavec_cc,
                                                         [f"-fweavec-checks={self.args.checks}"]), {}))
            if not self.args.no_asan:
                compilers.append(("asan", write_wrapper(work / "bin" / "asan", reference,
                                                        ["-fsanitize=address", "-fno-omit-frame-pointer"]),
                                  {"ASAN_OPTIONS": "detect_leaks=0"}))
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
        report_log = work / "runtime-reports.log"
        builds = {}
        if not run.failures:
            for label, cc, extra in compilers:
                src = work / label
                copy_tree(checkout, src)
                env = base_env(config, cc, src, self.support_root, self.bench_dir, self.jobs,
                               {**self.build_env(config), **env_extra, **extra})
                if label == "weavec-cc":
                    env["WEAVEC_RT_REPORT_LOG"] = str(report_log)
                for command in bench.build:
                    result = run_shell(command, cwd=src, env=env, timeout=self.args.build_timeout)
                    if result.returncode != 0:
                        run.failures.append(f"{label} build {command!r}: {describe_status(result)}: "
                                            f"{result.output.strip()[-300:]}")
                        break
                else:
                    builds[label] = (src, env)
        if builds and len(builds) == len(compilers):
            log(f"[{config.name}] bench {bench.name}: {repeat} runs per build ({', '.join(builds)})")
            for src, env in builds.values():  # warm up caches and the input
                run_shell(bench.command, cwd=src, env=env, timeout=self.args.build_timeout)
            for index in range(repeat):
                for label, (src, env) in builds.items():
                    result = run_shell(bench.command, cwd=src, env=env, timeout=self.args.build_timeout)
                    if result.returncode != 0:
                        run.failures.append(f"{label} run {index}: {describe_status(result)}: "
                                            f"{result.output.strip()[-300:]}")
                        break
                    run.times.setdefault(label, []).append(round(result.user, 4))
                    if result.maxrss:
                        run.peak_rss[label] = max(run.peak_rss.get(label, 0), result.maxrss)
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
            base = run.minimum.get("reference")
            if base:
                if "weavec-cc" in run.minimum:
                    run.ratio = round(run.minimum["weavec-cc"] / base, 4)
                if "asan" in run.minimum:
                    run.asan_ratio = round(run.minimum["asan"] / base, 4)
        if report_log.exists():
            run.reports = parse_reports(report_log.read_text(errors="replace"), work / "weavec-cc")
        if not self.args.keep:
            remove_tree(work)
        for failure in run.failures:
            self.fail(f"{config.name} bench: {failure}", config=config.name)
        self.entry(config)["bench"] = run.to_json()
        log(f"[{config.name}] bench {bench.name}: min user CPU "
            + ", ".join(f"{k} {v:.3f} s" for k, v in run.minimum.items())
            + (f"; ratio {run.ratio:.3f}" if run.ratio is not None else "")
            + (f"; ASan {run.asan_ratio:.3f}" if run.asan_ratio is not None else "")
            + ("; peak RSS " + ", ".join(f"{k} {v / 2 ** 20:.1f} MiB" for k, v in run.peak_rss.items())
               if run.peak_rss else ""))

    # ---- gates ----

    def evaluate(self) -> None:
        a = self.args
        gates = self.manifest.gates
        if a.full and not a.reference_only:
            self.evaluate_drop_in()
            if a.checks == "trap":
                self.evaluate_build_time(gates.get("buildTime") or {})
            else:
                self.note("--checks verify: the build-time gate is not evaluated (verify builds are slower)")
        if (a.full or a.bench) and not a.reference_only:
            if a.checks == "trap":
                self.evaluate_run_time(gates.get("runTime") or {})
            else:
                self.note("--checks verify: the run-time gate is not evaluated (verify builds are slower)")
        if a.checks == "verify" and (a.full or a.bench) and not a.reference_only:
            self.evaluate_verify()

    def evaluate_drop_in(self) -> None:
        # Each failure was recorded where it was found; the gate sums them up.
        ok, detail = drop_in_gate(self.configs, self.results["configs"], self.args.checks)
        # A config that fails the gate with no failure of its own recorded (a
        # build the run never made) fails here.
        unrecorded = [name for name, row in detail.items()
                      if not (row["built"] and row["reportBuilt"])
                      and not any(name in failure for failure in self.failures)]
        for name in unrecorded:
            self.fail(f"gate drop-in: {name}: a build is missing")
        self.gate("drop-in", ok, detail, failed_above=True)

    def evaluate_build_time(self, spec: dict) -> None:
        self.gate("build-time", *build_time_gate(self.configs, self.results["configs"], spec),
                  enforced=spec.get("enforced", True))

    def evaluate_run_time(self, spec: dict) -> None:
        self.gate("run-time", *run_time_gate(self.configs, self.manifest.configs, self.results["configs"], spec),
                  enforced=spec.get("enforced", True))

    def evaluate_verify(self) -> None:
        self.gate("verify", *verify_gate(self.configs, self.results["configs"]))

    def ratchet(self) -> None:
        expected = load_expected(self.args.expected, self.args.update)
        if self.args.update:
            measured = {n: m for n, m in self.measured.items() if n not in self.tool_failures}
            skipped = sorted(set(self.measured) - set(measured))
            if skipped:
                log(f"not recording {', '.join(skipped)}: their compiles, builds or runs failed")
            if not measured:
                log("nothing to record in expected.json")
                return
            write_json(self.args.expected, merge_expected(expected, measured, self.platform, self.machine,
                                                          self.producer()))
            log(f"updated {self.args.expected} ({self.platform}: {', '.join(sorted(measured))})")
            return
        result = compare_ratchet(self.measured, expected, self.platform)
        self.results["ratchet"] = result.to_json()
        for item in result.regressions:
            self.fail(f"ratchet regression: {item}")
        for item in result.improvements + result.changes:
            self.note(f"ratchet: {item}; --update records it")
        for item in result.missing:
            self.note(f"ratchet: {item}")
        if not result.failed:
            log("ratchet: no regression")

    def producer(self) -> str:
        versions = self.results.get("binaries", {})
        return (versions.get("weavec-cc") or {}).get("version", "")

    # ---- main flow ----

    def run(self) -> int:
        a = self.args
        start = time.perf_counter()
        try:
            if (a.quick or a.full) and not a.reference_only:
                self.run_quick()
            if a.full:
                self.run_builds()
            if a.inject or a.full:
                self.run_injections()
            if a.bench or a.full:
                self.run_benches()
            if a.programs:
                self.run_programs()
            self.evaluate()
            if (a.quick or a.full) and not a.reference_only:
                self.ratchet()
        finally:
            if not a.keep:
                remove_tree(self.run_dir)
        self.results["seconds"] = round(time.perf_counter() - start, 1)
        # A gate that failed fails the run, whether or not its failures were
        # recorded one by one.
        for name, gate in self.results["gates"].items():
            if gate.get("status") == "fail" and not any(
                    failure.startswith(f"gate {name}") or name in failure for failure in self.failures):
                self.failures.append(f"gate {name}: fail")
        self.results["measured"] = self.measured
        self.results["failures"] = self.failures
        self.results["notes"] = self.notes
        self.results["status"] = "fail" if self.failures else "pass"
        if a.json:
            write_json(a.json, self.results)
            log(f"wrote {a.json}")
        print()
        print_summary(self.configs, self.results)
        if self.failures:
            print(f"corpus gate: FAIL ({len(self.failures)} problem(s))")
            for failure in self.failures[:40]:
                print(f"  - {failure.splitlines()[0]}")
            if len(self.failures) > 40:
                print(f"  ... {len(self.failures) - 40} more")
            return 1
        print("corpus gate: PASS")
        return 0


def with_held_out(args: argparse.Namespace) -> bool:
    """Whether a run without --only or --set includes the held-out configs:
    --full does, the PR-time --quick and the other modes do not."""
    if args.held_out is not None:
        return args.held_out
    return bool(args.full)


# -- reporting ----------------------------------------------------------------------


def print_summary(configs: list[Config], results: dict) -> None:
    """One row per config, grouped by set."""
    def cell(value, fmt="{}"):
        return "-" if value is None else fmt.format(value)

    def yes_no(value):
        return "-" if value is None else ("yes" if value else "NO")

    width = max([len("config")] + [len(c.name) for c in configs])
    for corpus_set in SETS:
        group = [c for c in configs if c.set == corpus_set]
        if not group:
            continue
        print(f"{corpus_set} configs:")
        print(f"  {'config':<{width}}  {'accesses':>8} {'proven':>7} {'unguarded':>9} "
              f"{'built':>5} {'tests':>5} {'traps':>5} {'build x':>7} {'run x':>6}")
        for c in group:
            entry = results["configs"].get(c.name) or {}
            quick = entry.get("quick") or {}
            ledger = quick.get("ledger") or {}
            checked = (entry.get("builds") or {}).get(results["checks"]) or {}
            proven = ledger.get("provenShare")
            print(f"  {c.name:<{width}}  {cell(ledger.get('accesses')):>8} "
                  f"{cell(None if proven is None else proven * 100, '{:.1f}%'):>7} "
                  f"{cell(ledger.get('unguarded')):>9} "
                  f"{yes_no(checked.get('built')):>5} {yes_no(checked.get('testsPassed')):>5} "
                  f"{cell(entry.get('traps')):>5} {cell(entry.get('buildCpuRatio'), '{:.2f}'):>7} "
                  f"{cell((entry.get('bench') or {}).get('ratio'), '{:.2f}'):>6}")
    for name, gate in results["gates"].items():
        print(f"gate {name}: {gate['status']}")


def print_injection_table(runs: list[InjectionRun]) -> None:
    width = max([len("injection")] + [len(r.injection.id) for r in runs])
    print(f"{'injection':<{width}}  {'config':<17} {'where':<28} stops")
    for r in runs:
        inj = r.injection
        mark = "yes" if r.stopped else ("ERROR" if r.failures else "NO")
        extra = f" ({r.via[0]})" if r.via else ""
        print(f"{inj.id:<{width}}  {inj.config:<17} {f'{inj.file}:{inj.line}':<28} {mark}{extra}")


# -- --update-from --------------------------------------------------------------------


def update_from(results_path: Path, expected_path: Path) -> int:
    results = read_json(results_path)
    check_schema(results, results_path, RESULTS_SCHEMA, RESULTS_VERSION)
    measured = results.get("measured") or {}
    if not measured:
        raise GateError(f"{results_path}: the run measured nothing")
    expected = load_expected(expected_path, update=True)
    versions = results.get("binaries", {})
    producer = (versions.get("weavec-cc") or {}).get("version", "")
    write_json(expected_path, merge_expected(expected, measured, results["platform"], results["machine"], producer))
    log(f"updated {expected_path} for {results['platform']} from {results_path} ({', '.join(sorted(measured))})")
    return 0


# -- main -----------------------------------------------------------------------------


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                 allow_abbrev=False)
    modes = ap.add_argument_group("modes")
    modes.add_argument("--quick", action="store_true", help="the per-file enforcement ledgers")
    modes.add_argument("--full", action="store_true", help="quick + builds, tests, injections, benchmarks")
    modes.add_argument("--inject", action="store_true", help="the injected bugs")
    modes.add_argument("--bench", action="store_true", help="the benchmarks")
    modes.add_argument("--programs", action="store_true",
                       help="the programs tier: large programs from release archives (programs.json)")
    modes.add_argument("--update-from", type=Path, metavar="RESULTS",
                       help="record a --json results file (for example from CI) in expected.json and exit")
    mods = ap.add_argument_group("modifiers")
    mods.add_argument("--checks", choices=("trap", "verify"), default="trap",
                      help="the mode of the builds, tests and benchmarks (default trap)")
    mods.add_argument("--reference-only", action="store_true",
                      help="build, test and bench with --cc only; with --inject, check the injections under ASan")
    mods.add_argument("--update", action="store_true", help="record this run's measurements in expected.json")
    mods.add_argument("--no-asan", action="store_true", help="benchmarks without the ASan build")
    mods.add_argument("--skip-unavailable", action="store_true",
                      help="leave out the configs whose test prerequisites this machine lacks")
    bins = ap.add_argument_group("binaries")
    bins.add_argument("--weavec-cc", help="weavec-cc under test (default build/release/bin, then build/dev/bin)")
    bins.add_argument("--cc", help="reference compiler (default $WEAVEC_LLVM_PREFIX/bin/clang, else clang)")
    sel = ap.add_argument_group("selection and resources")
    sel.add_argument("--program", action="append", default=[], metavar="NAME",
                     help="with --programs, run this program alone (repeatable)")
    sel.add_argument("--only", action="append", nargs="+", default=[], metavar="CONFIG",
                     help="run only these configs (repeatable)")
    sel.add_argument("--held-out", action=argparse.BooleanOptionalAction, default=None,
                     help="include (or leave out) the configs marked heldOut, but the sealed sets "
                          "(default: included by --full)")
    sel.add_argument("--set", action="append", default=[], choices=SETS, metavar="SET",
                     help="run the configs of this set alone (repeatable; one of " + ", ".join(SETS) + ")")
    sel.add_argument("--injection", action="append", default=[], metavar="ID", help="run only this injection")
    sel.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel processes (default: CPUs)")
    sel.add_argument("--timeout", type=float, default=1800, help="seconds per per-file compile (default 1800)")
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
    files.add_argument("--prerequisites", type=Path, default=DEFAULT_PREREQUISITES)
    files.add_argument("--programs-file", type=Path, default=DEFAULT_PROGRAMS)
    files.add_argument("--injections", type=Path, default=DEFAULT_INJECTIONS)
    files.add_argument("--support-dir", type=Path, default=CORPUS_DIR / "support")
    files.add_argument("--bench-dir", type=Path, default=CORPUS_DIR / "bench")
    args = ap.parse_args(argv)
    args.only = [name for group in args.only for name in group]
    if not (args.quick or args.full or args.inject or args.bench or args.programs or args.update_from):
        ap.error("choose a mode: --quick, --full, --inject, --bench, --programs or --update-from")
    if args.reference_only and args.quick:
        ap.error("--reference-only builds, tests, benchmarks and checks injections; it has no --quick")
    if args.update and args.reference_only:
        ap.error("--reference-only measurements are not recorded")
    if args.update and not (args.quick or args.full):
        ap.error("--update records what --quick (or --full) measures")
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")
    for key in ("workdir", "manifest", "expected", "triage", "injections", "support_dir", "bench_dir",
                "programs_file", "prerequisites"):
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
