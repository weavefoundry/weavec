#!/usr/bin/env python3
"""Run the test/cases suites (RFC 0035 section 11).

Each case is a C file under test/cases/<suite>/ whose expectations are
written as line-comment markers (test/cases/README.md has the grammar). For
every selected case the runner

1. analyses it with the `weavec` tool (`--whole-program` when it has
   several units) and checks the diagnostics against the BUG, CLEAN and
   ALLOW markers;
2. builds it with `weavec-cc` and its FLAGS (trap mode, or verify mode under
   `--checks verify`), writing the enforcement ledger when a marker reads
   it, and checks the ledger against the GUARDED, PROVEN, UNGUARDED and
   EXPECT-LEDGER markers;
3. runs the executable once per RUN-INPUT and attributes every stop to a
   line and a kind: a `weavec: <kind> at <file>:<line>:<col>` report, an
   invalid release the allocator stopped (no location), or a fault (no
   location, a null dereference). Every stop must be one a TRAP marker
   expects, and every TRAP marker must be hit by some run;
4. runs the ASan oracle (`--asan` or an ASAN marker);
5. with `--checks verify`, fails any case that reports `weavec.proven`.

A DETECT case is judged instead by whether its bug stops (its first report
is on a STOP line, or it faults) and whether its fixed twin runs clean;
`--min-stops N` gates the count. An XFAIL case is expected to fail today
(XFAIL), and is reported when it passes (XPASS); neither fails the run.

Examples:

  scripts/run-cases.py --filter 'soundness/**' --asan
  scripts/run-cases.py --checks verify
  scripts/run-cases.py --filter detection --min-stops 58
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import fnmatch
import json
import os
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = ROOT / "test" / "cases"
RESOURCE_INCLUDE = ROOT / "resources" / "include"

# ---------------------------------------------------------------------------
# Vocabulary (RFC 0035 sections 5.3 and 9, and the Diagnostics section)
# ---------------------------------------------------------------------------

# The `version` of the enforcement ledger this runner reads.
LEDGER_VERSION = 3
# Run-time report kinds (runtime/weavec_report.c, section 5.3).
KINDS = (
    "heap-buffer-overflow", "heap-use-after-free", "stack-buffer-overflow",
    "stack-use-after-scope", "dynamic-stack-buffer-overflow", "global-buffer-overflow",
    "buffer-overflow", "null-dereference", "unterminated-string", "index-out-of-bounds",
    "invalid-release", "invalid-access", "overlapping-copy",
)
# The diagnostic ids of the analysis (include/weavec/Core/Diagnostic.h).
IDS = frozenset((
    "use-after-free", "double-free", "use-after-move", "conflicting-borrow",
    "lifetime-too-short", "unsafe-operation", "annotation-mismatch", "invalid-annotation",
    "leak", "mismatched-release", "null-dereference", "use-of-uninitialized",
    "invalid-release", "out-of-bounds", "invalid-integer-operation",
    "contradicted-assumption", "allocation-failure",
))
LEDGER_REASONS = {
    "GUARDED": ("access", "range", "string", "checked-call", "loop-range"),
    "PROVEN": ("in-bounds", "dominated", "merged", "optimized"),
    "UNGUARDED": ("unsafe",),
}

FILE_MARKERS = frozenset(("CLEAN", "ALLOW", "RUN-INPUT", "EXPECT-LEDGER", "FLAGS", "UNITS",
                          "ASAN", "TOOL", "DETECT", "XFAIL", "TRAP-AT"))
LINE_MARKERS = frozenset(("BUG", "TRAP", "GUARDED", "PROVEN", "UNGUARDED", "NEUTRALISED",
                          "MISS", "STOP"))
MARKERS = FILE_MARKERS | LINE_MARKERS
NO_ARGUMENT_MARKERS = frozenset(("CLEAN", "ASAN", "TOOL", "STOP"))
# A TRAP and the ledger markers may name a kind or a reason, or not.
OPTIONAL_ARGUMENT_MARKERS = frozenset(("TRAP", "GUARDED", "PROVEN", "UNGUARDED"))
# Markers a detection case (DETECT) cannot have: it is judged by whether its
# bug stops, not by what is reported where.
NOT_IN_DETECTION = frozenset(("CLEAN", "ALLOW", "TOOL", "BUG", "TRAP", "GUARDED", "PROVEN",
                              "UNGUARDED", "NEUTRALISED", "EXPECT-LEDGER"))
LEDGER_MARKERS = frozenset(("GUARDED", "PROVEN", "UNGUARDED"))
EXPECTATION_MARKERS = frozenset(("CLEAN", "EXPECT-LEDGER")) | LINE_MARKERS
COMPARISONS = ("==", "!=", "<=", ">=", "<", ">")

COMPILE_TIMEOUT = 120.0
# A case's program does its work in milliseconds; this bounds a hang, and is
# generous because CI shares a small runner between suites.
RUN_TIMEOUT = 30.0
ASAN_TIMEOUT = 60.0
TRAP_SIGNALS = frozenset(s for s in (getattr(signal, "SIGTRAP", None),
                                     getattr(signal, "SIGILL", None)) if s is not None)
FAULT_SIGNALS = frozenset(s for s in (getattr(signal, "SIGSEGV", None),
                                      getattr(signal, "SIGBUS", None)) if s is not None)

# ---------------------------------------------------------------------------
# Marker parsing
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Comment:
    line: int
    body: str
    code_before: bool


@dataclasses.dataclass(frozen=True)
class Scan:
    comments: tuple[Comment, ...]
    first_declaration: int  # first line with code outside a preprocessor directive


def scan_source(text: str) -> Scan:
    """Find `//` comments, skipping string and character literals and block comments."""
    comments: list[Comment] = []
    first_declaration = 0
    i, n, line = 0, len(text), 1
    code_on_line = False
    at_line_start = True
    in_directive = False
    while i < n:
        c = text[i]
        if c == "\n":
            continued = text[i - 1:i] == "\\" or text[i - 2:i] == "\\\r"
            if in_directive and not continued:
                in_directive = False
            line += 1
            code_on_line = False
            at_line_start = True
            i += 1
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            comments.append(Comment(line, text[i + 2:end], code_on_line))
            i = end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            newlines = text.count("\n", i, end)
            if newlines:
                line += newlines
                code_on_line = False
                at_line_start = True
                if in_directive:
                    in_directive = False
            i = end
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "#" and at_line_start:
            in_directive = True
        if not in_directive and not first_declaration:
            first_declaration = line
        code_on_line = True
        at_line_start = False
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    if text[j + 1] == "\n":
                        line += 1
                    j += 2
                    continue
                j += 1
            i = j + 1 if j < n and text[j] == c else j
            continue
        i += 1
    return Scan(tuple(comments), first_declaration or line + 1)


_KEYWORD = re.compile(r"^([A-Z](?:[A-Z-]*[A-Z])?)(?=$|[\s:.,;])")


def split_segments(body: str) -> list[str]:
    """A comment may carry several markers separated by `//`."""
    return [segment.strip() for segment in body.split("//")]


def parse_segment(segment: str) -> tuple[str, str] | None:
    """Return (keyword, argument) for a marker, None for prose; raise ValueError if malformed."""
    match = _KEYWORD.match(segment)
    if not match:
        return None
    keyword, rest = match.group(1), segment[match.end():]
    if keyword not in MARKERS:
        if rest.startswith(":"):
            near = [k for k in MARKERS if len(keyword) >= 3
                    and (k.startswith(keyword) or keyword.startswith(k))]
            if near:
                raise ValueError(f"unknown marker '{keyword}' (did you mean '{sorted(near)[0]}'?)")
        return None
    if keyword in NO_ARGUMENT_MARKERS:
        if rest.strip():
            raise ValueError(f"marker '{keyword}' takes no argument")
        return keyword, ""
    if keyword in OPTIONAL_ARGUMENT_MARKERS and not rest.strip():
        return keyword, ""
    if not rest.startswith(":"):
        raise ValueError(f"marker '{keyword}' needs ':' and an argument")
    argument = rest[1:].strip()
    if not argument and keyword != "RUN-INPUT":
        raise ValueError(f"marker '{keyword}' needs an argument")
    return keyword, argument


@dataclasses.dataclass(frozen=True)
class Marker:
    kind: str
    file: Path
    line: int
    value: Any  # parsed argument; see parse_argument


@dataclasses.dataclass(frozen=True)
class RunInput:
    args: tuple[str, ...]
    stdin: Path | None


@dataclasses.dataclass(frozen=True)
class Expectation:
    pointer: str
    op: str
    value: Any
    text: str


def parse_argument(kind: str, argument: str, directory: Path) -> Any:
    """Validate a marker argument and return its parsed form."""
    if kind == "BUG":
        parts = argument.split()
        if len(parts) not in (1, 2) or (len(parts) == 2 and parts[1] not in ("definite", "possible")):
            raise ValueError("BUG takes '<id> [definite|possible]'")
        if parts[0] not in IDS:
            raise ValueError(f"unknown diagnostic id '{parts[0]}'")
        return (parts[0], parts[1] if len(parts) == 2 else None)
    if kind == "TRAP":
        if argument and argument not in KINDS:
            raise ValueError(f"unknown report kind '{argument}' (one of {', '.join(KINDS)})")
        return argument or None
    if kind in LEDGER_MARKERS:
        if argument and argument not in LEDGER_REASONS[kind]:
            raise ValueError(f"unknown {kind.lower()} reason '{argument}'")
        return argument or None
    if kind == "NEUTRALISED":
        if argument != "zero-init":
            raise ValueError("NEUTRALISED takes 'zero-init'")
        return argument
    if kind == "MISS":
        return argument
    if kind == "ALLOW":
        ids = argument.split()
        unknown = [i for i in ids if i not in IDS]
        if unknown:
            raise ValueError(f"unknown diagnostic id '{unknown[0]}'")
        return tuple(ids)
    if kind == "RUN-INPUT":
        try:
            tokens = shlex.split(argument)
        except ValueError as error:
            raise ValueError(f"RUN-INPUT: {error}") from None
        stdin = None
        if "<" in tokens:
            at = tokens.index("<")
            if at != len(tokens) - 2:
                raise ValueError("RUN-INPUT takes '<argv...> [< <file>]'")
            stdin = (directory / tokens[at + 1]).resolve()
            if not stdin.is_file():
                raise ValueError(f"RUN-INPUT input file '{tokens[at + 1]}' does not exist")
            tokens = tokens[:at]
        return RunInput(tuple(tokens), stdin)
    if kind == "EXPECT-LEDGER":
        parts = argument.split(None, 2)
        if len(parts) != 3 or not parts[0].startswith("/") or parts[1] not in COMPARISONS:
            raise ValueError("EXPECT-LEDGER takes '<json-pointer> <op> <value>'")
        try:
            value = json.loads(parts[2])
        except ValueError:
            value = parts[2]
        return Expectation(parts[0], parts[1], value, argument)
    if kind == "FLAGS":
        try:
            return tuple(shlex.split(argument))
        except ValueError as error:
            raise ValueError(f"FLAGS: {error}") from None
    if kind == "UNITS":
        return tuple(argument.split())
    if kind == "TRAP-AT":
        unit, _, line = argument.rpartition(":")
        if not unit or not line.isdigit():
            raise ValueError("TRAP-AT takes '<unit>:<line>'")
        return (unit, int(line))
    if kind == "DETECT":
        try:
            flags = tuple(shlex.split(argument))
        except ValueError as error:
            raise ValueError(f"DETECT: {error}") from None
        if not all(flag.startswith("-") for flag in flags):
            raise ValueError("DETECT takes the flags that select the fixed twin, e.g. '-DFIX'")
        return flags
    return argument


@dataclasses.dataclass
class SourceMarkers:
    path: Path
    file_markers: list[Marker]
    line_markers: list[Marker]
    errors: list[str]


def parse_markers(path: Path, text: str) -> SourceMarkers:
    scan = scan_source(text)
    result = SourceMarkers(path, [], [], [])
    for comment in scan.comments:
        for segment in split_segments(comment.body):
            where = f"{path.name}:{comment.line}"
            try:
                parsed = parse_segment(segment)
                if parsed is None:
                    continue
                kind, argument = parsed
                value = parse_argument(kind, argument, path.parent)
            except ValueError as error:
                result.errors.append(f"{where}: {error}")
                continue
            marker = Marker(kind, path, comment.line, value)
            if kind in FILE_MARKERS:
                if comment.code_before:
                    result.errors.append(f"{where}: file marker {kind} must be on a comment line")
                elif comment.line > scan.first_declaration:
                    result.errors.append(
                        f"{where}: file marker {kind} must appear before the first declaration")
                else:
                    result.file_markers.append(marker)
            elif not comment.code_before:
                result.errors.append(
                    f"{where}: line marker {kind} is on a line without code (it applies to its own line)")
            else:
                result.line_markers.append(marker)
    return result


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------

_MAIN = re.compile(r"^[ \t]*(?:(?:static|extern|inline)\s+)*(?:int|void)\s+main\s*\([^)]*\)\s*\{",
                   re.MULTILINE)


def defines_main(text: str) -> bool:
    return bool(_MAIN.search(text))


@dataclasses.dataclass
class Case:
    path: Path
    rel: str
    suite: str
    units: list[Path]
    unit_flags: dict[Path, tuple[str, ...]]
    flags: tuple[str, ...]
    clean: bool
    allow: frozenset[str]
    tool: bool
    asan: bool
    run_inputs: list[RunInput]
    expectations: list[Expectation]
    markers: list[Marker]  # line markers of every unit
    has_main: bool
    errors: list[str]
    # RFC 0034 section 9: a detection case's twin flags (DETECT), and the reason
    # a case is expected to fail today (XFAIL).
    detect: tuple[str, ...] | None = None
    xfail: str | None = None

    def line_markers(self, kind: str) -> list[Marker]:
        return [m for m in self.markers if m.kind == kind]

    @property
    def bugs(self) -> list[Marker]:
        return self.line_markers("BUG")

    @property
    def traps(self) -> list[Marker]:
        return self.line_markers("TRAP")

    def bug_lines(self) -> set[tuple[Path, int]]:
        return {(m.file, m.line) for m in self.markers if m.kind in ("BUG", "MISS", "NEUTRALISED", "TRAP")}

    @property
    def is_bug_case(self) -> bool:
        return bool(self.bug_lines())

    def analysed_units(self) -> list[Path]:
        """Units the analysis sees: those not compiled with -fno-weavec."""
        return [u for u in self.units if "-fno-weavec" not in self.unit_flags.get(u, ())]


def relative(path: Path | str | None) -> str:
    if path is None:
        return "<no file>"
    path = Path(path)
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def load_case(path: Path, cases_root: Path, parsed: dict[Path, SourceMarkers] | None = None) -> Case:
    path = path.resolve()
    rel = path.relative_to(cases_root.resolve()).as_posix()
    parsed = {} if parsed is None else parsed

    def markers_of(file: Path) -> SourceMarkers:
        if file not in parsed:
            parsed[file] = parse_markers(file, file.read_text(errors="replace"))
        return parsed[file]

    main = markers_of(path)
    errors = list(main.errors)
    file_markers = collections.defaultdict(list)
    for marker in main.file_markers:
        file_markers[marker.kind].append(marker)
    units = [path]
    unit_flags: dict[Path, tuple[str, ...]] = {}
    markers = list(main.line_markers)
    # TRAP-AT: a stop at a line of another unit (a helper several cases
    # share), as a TRAP marker on that line would be for this case alone.
    for marker in file_markers["TRAP-AT"]:
        unit = (path.parent / marker.value[0]).resolve()
        if not unit.is_file():
            errors.append(f"{path.name}:{marker.line}: TRAP-AT file '{marker.value[0]}' does not exist")
        else:
            markers.append(Marker("TRAP", unit, marker.value[1], None))
    for marker in file_markers["UNITS"]:
        for name in marker.value:
            unit = (path.parent / name).resolve()
            if not unit.is_file():
                errors.append(f"{path.name}:{marker.line}: UNITS file '{name}' does not exist")
            elif unit in units:
                errors.append(f"{path.name}:{marker.line}: UNITS file '{name}' is listed twice")
            else:
                units.append(unit)
    texts = {path: path.read_text(errors="replace")}
    for unit in units[1:]:
        texts[unit] = unit.read_text(errors="replace")
        own = markers_of(unit)
        errors.extend(own.errors)
        markers.extend(own.line_markers)
        flags: list[str] = []
        for marker in own.file_markers:
            if marker.kind == "FLAGS":
                flags.extend(marker.value)
            else:
                errors.append(f"{unit.name}:{marker.line}: file marker {marker.kind} is only "
                              f"allowed in the case's main file")
        if flags:
            unit_flags[unit] = tuple(flags)
    main_flags = tuple(flag for marker in file_markers["FLAGS"] for flag in marker.value)
    if "-fno-weavec" in main_flags:
        errors.append(f"{path.name}: FLAGS of a case's main file cannot contain -fno-weavec")
    case = Case(
        path=path, rel=rel, suite=rel.split("/", 1)[0], units=units, unit_flags=unit_flags,
        flags=main_flags, clean=bool(file_markers["CLEAN"]),
        allow=frozenset(i for marker in file_markers["ALLOW"] for i in marker.value),
        tool=bool(file_markers["TOOL"]), asan=bool(file_markers["ASAN"]),
        run_inputs=[marker.value for marker in file_markers["RUN-INPUT"]],
        expectations=[marker.value for marker in file_markers["EXPECT-LEDGER"]],
        markers=markers, has_main=any(defines_main(t) for t in texts.values()), errors=errors,
        detect=file_markers["DETECT"][0].value if file_markers["DETECT"] else None,
        xfail="; ".join(m.value for m in file_markers["XFAIL"]) or None)
    kinds = {m.kind for m in main.file_markers} | {m.kind for m in markers}
    if len(file_markers["DETECT"]) > 1:
        errors.append(f"{path.name}: DETECT is given more than once")
    if case.detect is not None:
        clash = sorted(kinds & NOT_IN_DETECTION)
        if clash:
            errors.append(f"{path.name}: a DETECT case has no {', '.join(clash)} markers (its bug lines are STOP)")
        if not case.has_main:
            errors.append(f"{path.name}: DETECT needs a unit that defines main")
        stops = {(m.file, m.line) for m in markers if m.kind == "STOP"}
        if not stops:
            errors.append(f"{path.name}: a DETECT case needs a STOP line (where the bug must stop)")
        for marker in markers:
            if marker.kind == "MISS" and (marker.file, marker.line) not in stops:
                errors.append(f"{marker.file.name}:{marker.line}: in a DETECT case MISS marks a STOP line "
                              f"as a known miss")
    elif "STOP" in kinds:
        errors.append(f"{path.name}: STOP needs a DETECT file marker")
    if not kinds & EXPECTATION_MARKERS:
        errors.append(f"{path.name}: no expectation (CLEAN, EXPECT-LEDGER or a line marker)")
    if case.clean and {"BUG", "MISS", "NEUTRALISED", "TRAP"} & kinds:
        errors.append(f"{path.name}: CLEAN contradicts its BUG, MISS, NEUTRALISED or TRAP markers")
    if case.allow and not case.clean:
        errors.append(f"{path.name}: ALLOW only applies to a CLEAN case")
    if case.tool and (case.run_inputs or case.asan):
        errors.append(f"{path.name}: a TOOL case is not run (no RUN-INPUT or ASAN)")
    if case.asan and not case.tool and not case.has_main:
        errors.append(f"{path.name}: ASAN needs a unit that defines main")
    if case.run_inputs and not case.has_main:
        errors.append(f"{path.name}: RUN-INPUT needs a unit that defines main")
    return case


def discover(cases_root: Path) -> tuple[list[Case], dict[Path, SourceMarkers]]:
    """Every .c file that is neither under an Inputs directory nor another case's unit."""
    cases_root = cases_root.resolve()
    sources = sorted(p.resolve() for p in cases_root.rglob("*.c")
                     if "Inputs" not in p.relative_to(cases_root).parts[:-1])
    parsed: dict[Path, SourceMarkers] = {}
    units: set[Path] = set()
    for source in sources:
        markers = parsed.setdefault(source, parse_markers(source, source.read_text(errors="replace")))
        for marker in markers.file_markers:
            if marker.kind == "UNITS":
                units.update((source.parent / name).resolve() for name in marker.value)
    cases = [load_case(source, cases_root, parsed) for source in sources if source not in units]
    return cases, parsed


def select(cases: list[Case], filters: list[str]) -> list[Case]:
    if not filters:
        return cases

    def matches(case: Case, pattern: str) -> bool:
        pattern = pattern.strip("/")
        if any(ch in pattern for ch in "*?["):
            return fnmatch.fnmatchcase(case.rel, pattern)
        return case.rel == pattern or case.rel.startswith(pattern + "/")

    return [case for case in cases if any(matches(case, f) for f in filters)]


# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------

_W_FLAG = re.compile(r"^-W(?:no-)?(?:error=)?weavec(?:-[a-z0-9-]+)?$")


def tool_flags(flags: tuple[str, ...] | list[str]) -> tuple[list[str], list[str]]:
    """Split a case's FLAGS into the `weavec` tool's own options and the compiler flags."""
    own: list[str] = []
    compiler: list[str] = []
    for flag in flags:
        if flag.startswith("-fweavec-budget="):
            own.append("--budget=" + flag.split("=", 1)[1])
        elif flag == "-fno-weavec-zero-init":
            own.append("--no-zero-init")
        elif _W_FLAG.match(flag):
            own.append(flag)
        elif flag.startswith("-fweavec") or flag.startswith("-fno-weavec"):
            continue
        else:
            compiler.append(flag)
    return own, compiler


def plain_flags(flags: tuple[str, ...] | list[str]) -> list[str]:
    """The flags another compiler accepts: WeaveC's own dropped."""
    return [f for f in flags if not (f.startswith("-fweavec") or f.startswith("-fno-weavec")
                                     or _W_FLAG.match(f))]


# ---------------------------------------------------------------------------
# Evidence
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    column: int
    severity: str  # "error" or "warning"
    id: str
    message: str

    def text(self) -> str:
        return f"{relative(self.file)}:{self.line}:{self.column}: {self.severity}: {self.message} [weavec::{self.id}]"


_DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+): (error|warning): (.*) \[weavec::([a-z0-9-]+)\]$")


def resolve(file: str, cwd: Path | None) -> str:
    path = Path(file)
    if not path.is_absolute() and cwd is not None:
        path = cwd / path
    try:
        return str(path.resolve())
    except OSError:
        return str(path)


def parse_diagnostics(output: str, cwd: Path | None = None) -> list[Diagnostic]:
    result: list[Diagnostic] = []
    for line in output.splitlines():
        match = _DIAGNOSTIC.match(line.strip())
        if match:
            file, number, column, severity, message, ident = match.groups()
            result.append(Diagnostic(resolve(file, cwd), int(number), int(column), severity,
                                     ident, message))
    return result


@dataclasses.dataclass(frozen=True)
class Report:
    """A run-time stop: a report with a location, or one without."""
    kind: str
    file: str | None
    line: int
    column: int
    proven: bool = False

    def text(self) -> str:
        where = f"{relative(self.file)}:{self.line}:{self.column}" if self.file else "<no location>"
        return f"{'weavec.proven: ' if self.proven else ''}{self.kind} at {where}"


# A site without a location reports `at <unknown>: ` (runtime/weavec_report.c).
_REPORT = re.compile(r"^weavec: (weavec\.proven: )?([a-z-]+) at (?:(.+?):(\d+):(\d+)|<unknown>): ")
_FATAL = re.compile(r"^weavec: (invalid release) of 0x[0-9a-f]+: (.*)$")


def parse_reports(output: str, cwd: Path | None = None) -> list[Report]:
    reports: list[Report] = []
    for line in output.splitlines():
        match = _REPORT.match(line)
        if match:
            proven, kind, file, number, column = match.groups()
            if file is None:
                reports.append(Report(kind, None, 0, 0, bool(proven)))
            else:
                reports.append(Report(kind, resolve(file, cwd), int(number), int(column),
                                      bool(proven)))
            continue
        if _FATAL.match(line):
            reports.append(Report("invalid-release", None, 0, 0))
    return reports


@dataclasses.dataclass
class Run:
    args: tuple[str, ...]
    code: int | None
    timed_out: bool
    reports: list[Report]

    @property
    def signal(self) -> int | None:
        return -self.code if self.code is not None and self.code < 0 else None

    @property
    def trapped(self) -> bool:
        return self.signal in TRAP_SIGNALS

    @property
    def faulted(self) -> bool:
        return self.signal in FAULT_SIGNALS

    def describe(self) -> str:
        if self.timed_out:
            return "timed out"
        if self.signal is not None:
            try:
                name = signal.Signals(self.signal).name
            except ValueError:
                name = f"signal {self.signal}"
            return f"killed by {name}"
        return f"exit {self.code}"

    @property
    def stop(self) -> Report | None:
        """What stopped the run: its first report, or a fault."""
        if self.reports and (self.trapped or self.reports[0].kind == "invalid-release"):
            return self.reports[0]
        if self.faulted:
            return Report("null-dereference", None, 0, 0)
        return None


@dataclasses.dataclass
class Evidence:
    commands: list[str] = dataclasses.field(default_factory=list)
    failures: list[str] = dataclasses.field(default_factory=list)
    notes: list[str] = dataclasses.field(default_factory=list)
    diagnostics: list[Diagnostic] = dataclasses.field(default_factory=list)
    built: bool = False
    runs: list[Run] = dataclasses.field(default_factory=list)
    ledger: dict | None = None
    asan_ran: bool = False
    asan_report: str | None = None
    seconds: float = 0.0


def located(file: Path | str, line: int) -> tuple[str, int]:
    return (str(Path(file).resolve()), line)


def loc(marker: Marker) -> str:
    return f"{relative(marker.file)}:{marker.line}"


def json_pointer(document: Any, pointer: str) -> Any:
    node = document
    for part in pointer.split("/")[1:]:
        part = part.replace("~1", "/").replace("~0", "~")
        if isinstance(node, list):
            node = node[int(part)]
        else:
            node = node[part]
    return node


def compare(actual: Any, op: str, expected: Any) -> bool:
    try:
        return {"==": actual == expected, "!=": actual != expected, "<=": actual <= expected,
                ">=": actual >= expected, "<": actual < expected, ">": actual > expected}[op]
    except TypeError:
        return False


# ---------------------------------------------------------------------------
# Processes
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Config:
    weavec: Path
    weavec_cc: Path
    clang: str
    checks: str
    asan: bool
    no_run: bool
    keep: bool
    compile_timeout: float
    run_timeout: float
    scratch: Path


def no_core_dump() -> None:
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except (ValueError, OSError):
        pass


def run_process(command: list[str], cwd: Path, timeout: float, stdin: Path | None = None,
                env: dict | None = None) -> tuple[int | None, str, str, bool]:
    try:
        with open(stdin, "rb") if stdin else open(os.devnull, "rb") as input_stream:
            done = subprocess.run(command, cwd=cwd, stdin=input_stream, capture_output=True,
                                  timeout=timeout, env=env, preexec_fn=no_core_dump)
        return (done.returncode, done.stdout.decode(errors="replace"),
                done.stderr.decode(errors="replace"), False)
    except subprocess.TimeoutExpired as expired:
        out = expired.stdout.decode(errors="replace") if expired.stdout else ""
        err = expired.stderr.decode(errors="replace") if expired.stderr else ""
        return None, out, err, True


def run_environment(**extra: str) -> dict:
    env = dict(os.environ)
    for name in ("WEAVEC_RT_ABORT", "WEAVEC_RT_REPORT_LOG", "WEAVEC_RT_QUARANTINE"):
        env.pop(name, None)
    env.update(extra)
    return env


def first_line(text: str) -> str:
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:300]
    return "(no output)"


# ---------------------------------------------------------------------------
# The steps
# ---------------------------------------------------------------------------


def analyse(case: Case, cfg: Config, ev: Evidence, directory: Path) -> None:
    """The `weavec` tool over the case's analysed units."""
    units = case.analysed_units()
    if not units:
        return
    own, compiler = tool_flags(case.flags)
    command = [str(cfg.weavec), *own]
    if len(units) > 1:
        command.append("--whole-program")
    command.extend(str(u) for u in units)
    command.extend(["--", f"-I{RESOURCE_INCLUDE}", *compiler])
    ev.commands.append(shlex.join(command))
    code, out, err, timed_out = run_process(command, directory, cfg.compile_timeout)
    if timed_out:
        ev.failures.append("the analysis timed out")
        return
    ev.diagnostics = parse_diagnostics(out + "\n" + err, directory)
    if code not in (0, 1):
        ev.failures.append(f"the analysis crashed ({code}): {first_line(err)}")


def build(case: Case, cfg: Config, ev: Evidence, directory: Path, extra: list[str],
          ledger: bool) -> Path | None:
    """`weavec-cc` per unit, then the link."""
    directory.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []
    checks = [f"-fweavec-checks={cfg.checks}"] if cfg.checks != "trap" else []
    for index, unit in enumerate(case.units):
        obj = directory / f"u{index}-{unit.stem}.o"
        flags = [*case.flags, *case.unit_flags.get(unit, ())]
        command = [str(cfg.weavec_cc), *checks, f"-I{RESOURCE_INCLUDE}", *flags, *extra]
        if ledger and "-fno-weavec" not in flags:
            command.append(f"-fweavec-ledger={directory}/")
        command.extend(["-c", str(unit), "-o", str(obj)])
        ev.commands.append(shlex.join(command))
        code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
        if timed_out or code != 0:
            ev.failures.append(f"compiling {unit.name} failed: "
                               + ("timed out" if timed_out else first_line(err)))
            return None
        objects.append(obj)
    if ledger:
        main = directory / f"u0-{case.units[0].stem}.o.ledger.json"
        if main.is_file():
            try:
                ev.ledger = json.loads(main.read_text())
            except ValueError as error:
                ev.failures.append(f"the ledger is not JSON: {error}")
    if not case.has_main:
        ev.built = True
        return None
    exe = directory / "a.out"
    command = [str(cfg.weavec_cc), *checks, *case.flags, *extra, *(str(o) for o in objects),
               "-o", str(exe)]
    ev.commands.append(shlex.join(command))
    code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
    if timed_out or code != 0 or not exe.exists():
        ev.failures.append("linking failed: " + ("timed out" if timed_out else first_line(err)))
        return None
    ev.built = True
    return exe


def run_all(case: Case, cfg: Config, ev: Evidence, exe: Path, directory: Path) -> None:
    env = run_environment()
    for run_input in case.run_inputs or [RunInput((), None)]:
        code, _, err, timed_out = run_process([str(exe), *run_input.args], directory,
                                              cfg.run_timeout, run_input.stdin, env)
        ev.runs.append(Run(run_input.args, code, timed_out, parse_reports(err, directory)))


def run_asan(case: Case, cfg: Config, ev: Evidence, directory: Path) -> None:
    """The reference: Clang with AddressSanitizer and weavec.h on the path."""
    directory.mkdir(parents=True, exist_ok=True)
    exe = directory / "a.asan"
    command = [cfg.clang, "-fsanitize=address,array-bounds",
               "-fno-sanitize-recover=array-bounds", "-fno-omit-frame-pointer", "-g", "-O0",
               f"-I{RESOURCE_INCLUDE}", *plain_flags(case.flags)]
    for unit in case.units:
        command.extend(plain_flags(case.unit_flags.get(unit, ())))
    command.extend([*(str(u) for u in case.units), "-o", str(exe)])
    code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
    if code != 0 or not exe.exists():
        ev.failures.append("the ASan build failed: " + ("timed out" if timed_out else first_line(err)))
        return
    env = run_environment(ASAN_OPTIONS="detect_leaks=0:detect_stack_use_after_return=1")
    ev.asan_ran = True
    for run_input in case.run_inputs or [RunInput((), None)]:
        _, _, err, _ = run_process([str(exe), *run_input.args], directory, ASAN_TIMEOUT,
                                   run_input.stdin, env)
        match = re.search(r"ERROR: AddressSanitizer: ([a-z-]+)|(SEGV on unknown address)"
                          r"|(runtime error: index -?\d+ out of bounds)", err)
        if match and ev.asan_report is None:
            ev.asan_report = match.group(1) or ("SEGV" if match.group(2) else "index-out-of-bounds")


# ---------------------------------------------------------------------------
# Judging
# ---------------------------------------------------------------------------


# A BUG marker of these ids is also satisfied by a run that stops on its line:
# the analysis is advisory, and the guards catch what it does not report.
STOPPABLE_IDS = frozenset(("out-of-bounds", "null-dereference", "use-after-free", "double-free",
                           "invalid-release", "use-of-uninitialized", "mismatched-release",
                           "lifetime-too-short", "use-after-move", "allocation-failure",
                           "contradicted-assumption", "unsafe-operation"))


def stopped_lines(ev: Evidence) -> set[tuple[str, int]]:
    lines = set()
    for run in ev.runs:
        for report in run.reports:
            if report.file is not None:
                lines.add((report.file, report.line))
    return lines


def judge_diagnostics(case: Case, ev: Evidence, failures: list[str], ran: bool) -> None:
    stops = stopped_lines(ev)
    by_line: dict[tuple[str, int], list[Diagnostic]] = collections.defaultdict(list)
    for diagnostic in ev.diagnostics:
        by_line[(diagnostic.file, diagnostic.line)].append(diagnostic)
    expected = set()
    # `BUG: <id> // MISS: <reason>`: the analysis is known not to report it;
    # `// NEUTRALISED: zero-init`: zero-initialisation defines it away.
    missed = {located(m.file, m.line) for m in case.markers
              if m.kind in ("MISS", "NEUTRALISED")}
    # A stop with no location (the allocator's, a fault) of the BUG's kind.
    unlocated = {run.stop.kind for run in ev.runs
                 if run.stop is not None and run.stop.file is None}
    for bug in case.bugs:
        ident, certainty = bug.value
        key = located(bug.file, bug.line)
        expected.add(key)
        if key in missed:
            continue
        found = [d for d in by_line.get(key, []) if d.id == ident]
        if certainty == "definite":
            found = [d for d in found if d.severity == "error"]
        elif certainty == "possible":
            found = [d for d in found if d.severity == "warning"]
        if not found and ident in STOPPABLE_IDS and (key in stops or not ran
                                                     or ident in unlocated):
            continue
        if not found:
            failures.append(f"{loc(bug)}: expected weavec::{ident}"
                            + (f" ({certainty})" if certainty else "")
                            + " from the analysis or a stop on its line")
    expected |= {located(m.file, m.line) for m in case.markers if m.kind in ("MISS", "NEUTRALISED")}
    for diagnostic in ev.diagnostics:
        key = (diagnostic.file, diagnostic.line)
        if key in expected:
            continue
        # ALLOW excuses warnings only: an error always fails a CLEAN case.
        if case.clean and (diagnostic.id not in case.allow or diagnostic.severity == "error"):
            failures.append(f"unexpected diagnostic in a CLEAN case: {diagnostic.text()}")
        elif diagnostic.severity == "error" and not case.clean:
            failures.append(f"unexpected error: {diagnostic.text()}")


def judge_runs(case: Case, ev: Evidence, failures: list[str], notes: list[str]) -> None:
    traps = case.traps
    hit: set[Marker] = set()
    for run in ev.runs:
        label = f"run {shlex.join(run.args) or '(no arguments)'}"
        if run.timed_out:
            failures.append(f"{label}: timed out")
            continue
        stop = run.stop
        if stop is None:
            if run.signal is not None:
                failures.append(f"{label}: {run.describe()} with no report")
            elif run.reports:
                failures.append(f"{label}: reported {run.reports[0].text()} but was not stopped")
            continue
        matching = [m for m in traps
                    if (m.value is None or m.value == stop.kind)
                    and (stop.file is None or located(m.file, m.line) == (stop.file, stop.line))]
        on_bug_line = stop.file is None and bool(case.bugs) or any(
            located(m.file, m.line) == (stop.file, stop.line) for m in case.bugs)
        if not matching and not on_bug_line:
            failures.append(f"{label}: unexpected stop: {stop.text()} ({run.describe()})")
        hit.update(matching)
    if ev.runs:
        for marker in traps:
            if marker not in hit:
                failures.append(f"{loc(marker)}: expected a stop"
                                + (f" ({marker.value})" if marker.value else "") + "; "
                                + "; ".join(r.describe() for r in ev.runs))
    for run in ev.runs:
        for report in run.reports:
            if report.proven:
                failures.append(f"a proof was wrong: {report.text()}")


def judge_ledger(case: Case, ev: Evidence, failures: list[str]) -> None:
    wanted = [m for m in case.markers if m.kind in LEDGER_MARKERS]
    if not wanted and not case.expectations:
        return
    if ev.ledger is None:
        failures.append("no enforcement ledger was written")
        return
    if ev.ledger.get("version") != LEDGER_VERSION:
        failures.append(f"ledger version {ev.ledger.get('version')}, expected {LEDGER_VERSION}")
        return
    unit = ev.ledger["units"][0]
    rows = collections.defaultdict(list)
    for row in unit.get("rows", []):
        rows[(str(Path(row["file"]).resolve()) if row["file"] else "", row["line"])].append(row)
    for marker in wanted:
        outcome = marker.kind.lower()
        found = [r for r in rows.get(located(marker.file, marker.line), [])
                 if r["outcome"] == outcome and (marker.value is None or r["reason"] == marker.value)]
        if not found:
            failures.append(f"{loc(marker)}: expected a {outcome} ledger row"
                            + (f" ({marker.value})" if marker.value else ""))
    for expectation in case.expectations:
        try:
            actual = json_pointer(unit, expectation.pointer)
        except (KeyError, IndexError, ValueError, TypeError):
            failures.append(f"EXPECT-LEDGER {expectation.text}: no such value")
            continue
        if not compare(actual, expectation.op, expectation.value):
            failures.append(f"EXPECT-LEDGER {expectation.text}: actual {json.dumps(actual)}")


def apply_xfail(case: Case, result: dict) -> dict:
    if case.xfail is None:
        return result
    if result["status"] == "fail":
        result["status"] = "xfail"
        result["notes"].append(f"expected to fail: {case.xfail}")
    elif result["status"] == "pass":
        result["status"] = "xpass"
        result["notes"].append(f"passes now: remove XFAIL ({case.xfail})")
    return result


def run_case(case: Case, cfg: Config) -> dict:
    started = time.monotonic()
    result = run_detection(case, cfg) if case.detect is not None else run_markers(case, cfg)
    result["seconds"] = round(time.monotonic() - started, 3)
    return apply_xfail(case, result)


def scratch_for(case: Case, cfg: Config) -> Path:
    return Path(tempfile.mkdtemp(prefix=case.rel.replace("/", "_") + ".", dir=cfg.scratch))


def run_markers(case: Case, cfg: Config) -> dict:
    ev = Evidence()
    failures: list[str] = list(case.errors)
    if case.errors:
        return {"case": case.rel, "suite": case.suite, "status": "error", "failures": failures,
                "notes": [], "kind": "markers"}
    directory = scratch_for(case, cfg)
    try:
        analysed = bool(case.bugs or case.clean or case.tool)
        if analysed:
            analyse(case, cfg, ev, directory)
        if not case.tool:
            ledger = bool([m for m in case.markers if m.kind in LEDGER_MARKERS]
                          or case.expectations)
            exe = build(case, cfg, ev, directory / "build", [], ledger)
            judge_ledger(case, ev, failures)
            if exe is not None and not cfg.no_run:
                run_all(case, cfg, ev, exe, directory / "build")
                judge_runs(case, ev, failures, ev.notes)
                if case.clean:
                    for run in ev.runs:
                        if run.reports or run.signal is not None:
                            failures.append(f"run {shlex.join(run.args) or '(no arguments)'}: "
                                            f"{run.describe()} in a CLEAN case")
            if (cfg.asan or case.asan) and case.has_main and not cfg.no_run:
                run_asan(case, cfg, ev, directory / "asan")
                bug_case = case.is_bug_case
                if bug_case and case.asan and ev.asan_ran and ev.asan_report is None:
                    failures.append("ASan reported nothing")
                if not bug_case and ev.asan_report is not None:
                    failures.append(f"ASan reported {ev.asan_report} in a case without a bug")
        if analysed:
            # A case that cannot run (no main, TOOL) has no stop to excuse
            # a BUG the analysis does not report.
            runnable = case.has_main and not case.tool
            judge_diagnostics(case, ev, failures, bool(ev.runs) or not runnable)
        failures.extend(ev.failures)
    finally:
        if not cfg.keep:
            shutil.rmtree(directory, ignore_errors=True)
    return {
        "case": case.rel, "suite": case.suite, "status": "fail" if failures else "pass",
        "failures": failures, "notes": ev.notes, "kind": "markers",
        "diagnostics": [d.text() for d in ev.diagnostics],
        "runs": [{"args": list(r.args), "result": r.describe(),
                  "reports": [x.text() for x in r.reports]} for r in ev.runs],
        "asan": ev.asan_report, "commands": ev.commands,
    }


def run_detection(case: Case, cfg: Config) -> dict:
    """RFC 0034 section 9, kept by RFC 0035: does the bug stop, is the twin clean?"""
    failures: list[str] = list(case.errors)
    notes: list[str] = []
    if case.errors:
        return {"case": case.rel, "suite": case.suite, "status": "error", "failures": failures,
                "notes": [], "kind": "detection"}
    directory = scratch_for(case, cfg)
    bug, twin = Evidence(), Evidence()
    stop = where = None
    try:
        exe = build(case, cfg, bug, directory / "bug", [], False)
        stops = {located(m.file, m.line) for m in case.markers if m.kind == "STOP"}
        known_miss = [m for m in case.markers if m.kind == "MISS"]
        why_not = "no executable was built"
        if exe is not None and not cfg.no_run:
            run_all(case, cfg, bug, exe, directory / "bug")
            why_not = "the run did not stop"
            for run in bug.runs:
                first = run.stop
                if first is not None and (first.file is None or (first.file, first.line) in stops):
                    stop = "fault" if first.file is None else "run"
                    where = first.text()
                    break
                if first is not None:
                    why_not = f"the first stop is {first.text()}, on no STOP line"
                elif run.timed_out:
                    why_not = "the run timed out"
        flip = False
        if cfg.no_run:
            pass
        elif stop is None and not known_miss:
            failures.append(f"the bug did not stop: {why_not}")
        elif stop is not None and known_miss:
            flip = True
            notes.append(f"known miss now stops ({where}): remove its MISS marker")
        elif stop is None:
            notes.append(f"known miss ({'; '.join(m.value for m in known_miss)}): {why_not}")
        for run in bug.runs:
            failures.extend(f"a proof was wrong: {r.text()}" for r in run.reports if r.proven)
        twin_exe = build(case, cfg, twin, directory / "twin", list(case.detect or ()), False)
        twin_failures = list(twin.failures)
        if twin_exe is not None and not cfg.no_run:
            run_all(case, cfg, twin, twin_exe, directory / "twin")
            for run in twin.runs:
                if run.timed_out or run.signal is not None or run.reports:
                    twin_failures.append(f"run {shlex.join(run.args) or '(no arguments)'}: "
                                         f"{run.describe()}"
                                         + (f", {run.reports[0].text()}" if run.reports else ""))
        failures.extend(f"the fixed twin ({' '.join(case.detect or ())}) stops: {f}"
                        for f in twin_failures)
        failures.extend(f for f in bug.failures)
        if cfg.asan:
            run_asan(case, cfg, bug, directory / "asan")
    finally:
        if not cfg.keep:
            shutil.rmtree(directory, ignore_errors=True)
    return {
        "case": case.rel, "suite": case.suite, "status": "fail" if failures else "pass",
        "failures": failures, "notes": notes, "kind": "detection",
        "detection": {"stop": stop, "where": where, "knownMiss": bool(known_miss),
                      "flip": flip, "asan": bug.asan_report if bug.asan_ran else None},
        "runs": [{"args": list(r.args), "result": r.describe(),
                  "reports": [x.text() for x in r.reports]} for r in bug.runs],
        "commands": bug.commands + twin.commands,
    }


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def summarize(results: list[dict]) -> dict:
    suites: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    detection = collections.Counter()
    for result in results:
        suites[result["suite"]][result["status"]] += 1
        if result.get("kind") == "detection":
            d = result.get("detection") or {}
            detection["cases"] += 1
            detection["stops"] += 1 if d.get("stop") else 0
            detection["asan"] += 1 if d.get("asan") else 0
    totals = collections.Counter()
    for counter in suites.values():
        totals.update(counter)
    return {"suites": {k: dict(v) for k, v in sorted(suites.items())}, "totals": dict(totals),
            "detection": dict(detection)}


def print_result(result: dict, verbose: bool) -> None:
    status = result["status"].upper()
    if status == "PASS" and not verbose:
        return
    print(f"{status}: {result['case']}")
    for failure in result["failures"]:
        print(f"    {failure}")
    for note in result["notes"]:
        print(f"    note: {note}")


def print_summary(summary: dict) -> None:
    for suite, counts in summary["suites"].items():
        print(f"  {suite}: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())))
    totals = summary["totals"]
    print("total: " + ", ".join(f"{v} {k}" for k, v in sorted(totals.items())))
    detection = summary["detection"]
    if detection.get("cases"):
        line = f"detection: {detection.get('stops', 0)} of {detection['cases']} bugs stopped"
        if detection.get("asan"):
            line += f" (ASan: {detection['asan']})"
        print(line)


def default_build_dir() -> Path:
    for name in ("release", "dev"):
        candidate = ROOT / "build" / name
        if (candidate / "bin" / "weavec-cc").exists():
            return candidate
    return ROOT / "build" / "dev"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--weavec-cc", type=Path, help="weavec-cc to test (default: <build-dir>/bin)")
    parser.add_argument("--weavec", type=Path, help="weavec to test (default: <build-dir>/bin)")
    parser.add_argument("--build-dir", type=Path,
                        help="where bin/ is (default: build/release, else build/dev)")
    parser.add_argument("--clang", default=None, help="the reference compiler for --asan")
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES, help="the cases tree")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1,
                        help="cases run at once")
    parser.add_argument("--checks", choices=("trap", "verify"), default="trap",
                        help="the -fweavec-checks mode of the builds")
    parser.add_argument("--asan", action="store_true", help="run the ASan oracle for every case")
    parser.add_argument("--no-run", action="store_true", help="analyse and build only")
    parser.add_argument("--filter", action="append", default=[], metavar="GLOB",
                        help="select cases by path under the cases tree (repeatable)")
    parser.add_argument("--json", type=Path, metavar="OUT", help="write the results as JSON")
    parser.add_argument("--timeout", type=float, default=COMPILE_TIMEOUT,
                        help="seconds per compile or link")
    parser.add_argument("--run-timeout", type=float, default=RUN_TIMEOUT, help="seconds per run")
    parser.add_argument("--keep", action="store_true", help="keep each case's build directory")
    parser.add_argument("--min-stops", type=int, metavar="N",
                        help="fail unless at least N detection cases stop")
    parser.add_argument("--verbose", "-v", action="store_true", help="print passing cases too")
    args = parser.parse_args(argv)

    build_dir = args.build_dir or default_build_dir()
    weavec = args.weavec or build_dir / "bin" / "weavec"
    weavec_cc = args.weavec_cc or build_dir / "bin" / "weavec-cc"
    for tool in (weavec, weavec_cc):
        if not tool.exists():
            print(f"run-cases: {tool} does not exist; build it first", file=sys.stderr)
            return 2
    clang = args.clang or shutil.which("clang") or "clang"
    if args.clang is None:
        prefix = os.environ.get("WEAVEC_LLVM_PREFIX")
        if prefix and (Path(prefix) / "bin" / "clang").exists():
            clang = str(Path(prefix) / "bin" / "clang")
    cases, _ = discover(args.cases)
    selected = select(cases, args.filter)
    if not selected:
        print("run-cases: no case selected", file=sys.stderr)
        return 2
    scratch = Path(tempfile.mkdtemp(prefix="weavec-cases."))
    cfg = Config(weavec=weavec.resolve(), weavec_cc=weavec_cc.resolve(), clang=clang,
                 checks=args.checks, asan=args.asan, no_run=args.no_run, keep=args.keep,
                 compile_timeout=args.timeout, run_timeout=args.run_timeout, scratch=scratch)
    results: list[dict] = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for result in pool.map(lambda case: run_case(case, cfg), selected):
                results.append(result)
                print_result(result, args.verbose)
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)
    summary = summarize(results)
    print_summary(summary)
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "results": results}, indent=2) + "\n")
    failed = any(r["status"] in ("fail", "error") for r in results)
    if args.min_stops is not None and summary["detection"].get("stops", 0) < args.min_stops:
        print(f"run-cases: {summary['detection'].get('stops', 0)} detection stops, fewer than "
              f"{args.min_stops}", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
