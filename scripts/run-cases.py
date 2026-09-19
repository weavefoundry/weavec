#!/usr/bin/env python3
"""Run the test/cases suites (RFC 0030 section 17).

Each case is a C file under test/cases/<suite>/ whose expectations are
written as line-comment markers (test/cases/README.md has the grammar). For
every selected case the runner

1. builds it with `weavec-cc -c ... -fweavec-ledger=<tmp>/` and links the
   units when one of them defines `main` (TOOL cases use `weavec --ledger`);
2. checks the WeaveC diagnostics against the BUG, CLEAN and ALLOW markers;
3. checks the unit and program ledgers against the UNRESOLVED, TRUSTED,
   NOT-PROVEN and EXPECT-LEDGER markers;
4. runs the trap-mode executable once per RUN-INPUT, rebuilds it with
   `-fweavec-checks=report` and attributes every failed check to a line and
   template, which the TRAP markers must match;
5. runs the ASan oracle (`--asan` or an ASAN marker);
6. with `--checks verify`, fails any case that hits a `weavec.proven` trap.

`--legacy` applies the same markers with v0.10.0 semantics: diagnostics
only, from the `weavec` tool of WEAVEC_GOLDEN_DIR, and additionally
classifies every bug case as CAUGHT, SILENT, SIGNAL, MISLABEL or LEAK-ONLY.
`--compare-golden` runs the binaries under test and the golden binaries in
that legacy mode and fails on any difference in their sorted diagnostics
(the S1 gate).

Examples:

  scripts/run-cases.py --filter 'soundness/**' --asan
  WEAVEC_GOLDEN_DIR=/opt/weavec-0.10.0 scripts/run-cases.py --legacy
  scripts/run-cases.py --compare-golden --weavec build/dev/bin/weavec
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
# Vocabulary (RFC 0030 sections 2, 10.2, 17.3 and the Diagnostics section)
# ---------------------------------------------------------------------------

FACETS = ("spatial", "null", "temporal", "assertion")
TEMPLATES = ("nonnull", "index", "span", "len", "disjoint", "assert", "violation")
UNRESOLVED_REASONS = (
    "unknown-extent", "unknown-index", "inexpressible", "may-released", "may-moved",
    "may-alias-released", "may-invalid-release", "may-mismatched-release", "may-dangle",
    "may-conflict", "unknown-callee", "callback", "setjmp", "budget", "unanalysed",
    "raw-cast", "dangling-escape", "second-owner", "no-zero-init",
)
TRUST_REASONS = (
    "unsafe", "system-api", "library-spec", "extern-contract", "caller-contract",
    "external-unit", "concurrency",
)
OUTCOMES = ("proven", "checked", "violation", "unresolved", "trusted")
# The ids of v0.10.0 and of RFC 0030; a pin converted from the golden run may
# name an id that RFC 0030 removes.
IDS = frozenset((
    "use-after-free", "double-free", "use-after-move", "conflicting-borrow",
    "lifetime-too-short", "unsafe-operation", "annotation-mismatch", "invalid-annotation",
    "leak", "mismatched-release", "null-dereference", "use-of-uninitialized",
    "invalid-release", "out-of-bounds", "invalid-integer-operation",
    "contradicted-assumption", "allocation-failure", "unresolved-operation",
    "unchecked-operation", "unanalyzed-input",
    "analysis-incomplete", "annotation-required", "checking-incomplete", "checking-failed",
))
# Section 17.3: the facet a BUG id is matched against (gate G4).
FACET_OF_ID = {
    "use-after-free": "temporal", "double-free": "temporal", "use-after-move": "temporal",
    "conflicting-borrow": "temporal", "lifetime-too-short": "temporal",
    "mismatched-release": "temporal", "annotation-mismatch": "temporal",
    "null-dereference": "null", "use-of-uninitialized": "null",
    "out-of-bounds": "spatial", "invalid-release": "spatial",
    "contradicted-assumption": "assertion",
}
# The trap templates that enforce each facet. `violation` is the lowered
# violation of any facet (section 3.4).
TEMPLATES_OF_FACET = {
    "null": frozenset(("nonnull", "violation")),
    "spatial": frozenset(("index", "span", "len", "disjoint", "violation")),
    "assertion": frozenset(("assert", "violation")),
    "temporal": frozenset(("violation",)),
}
# Facets whose BUG markers a TRAP on the same line satisfies (section 17.3
# names null and spatial; assertion is added because WEAVEC_ASSUME checks
# trap with `assert`).
TRAPPABLE_FACETS = frozenset(("null", "spatial", "assertion"))
CLASSES = ("error", "warning", "trap", "row", "neutralised", "miss", "silent")
LEGACY_CLASSES = ("CAUGHT", "SILENT", "SIGNAL", "MISLABEL", "LEAK-ONLY")
LEGACY_SIGNAL_IDS = frozenset(("analysis-incomplete", "annotation-required",
                               "checking-incomplete", "checking-failed"))

FILE_MARKERS = frozenset(("CLEAN", "ALLOW", "RUN-INPUT", "EXPECT-LEDGER", "FLAGS", "UNITS",
                          "ASAN", "TOOL"))
LINE_MARKERS = frozenset(("BUG", "TRAP", "UNRESOLVED", "TRUSTED", "NOT-PROVEN",
                          "NEUTRALISED", "MISS"))
MARKERS = FILE_MARKERS | LINE_MARKERS
NO_ARGUMENT_MARKERS = frozenset(("CLEAN", "ASAN", "TOOL"))
EXPECTATION_MARKERS = frozenset(("CLEAN", "EXPECT-LEDGER")) | LINE_MARKERS
COMPARISONS = ("==", "!=", "<=", ">=", "<", ">")

COMPILE_TIMEOUT = 120.0
RUN_TIMEOUT = 10.0
ASAN_TIMEOUT = 60.0
TRAP_SIGNALS = frozenset(s for s in (getattr(signal, "SIGTRAP", None),
                                     getattr(signal, "SIGILL", None)) if s is not None)

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
        if argument not in TEMPLATES:
            raise ValueError(f"unknown trap template '{argument}' (one of {', '.join(TEMPLATES)})")
        return argument
    if kind in ("UNRESOLVED", "TRUSTED"):
        facet, _, reason = argument.partition(":")
        facet, reason = facet.strip(), reason.strip()
        if facet not in FACETS:
            raise ValueError(f"unknown facet '{facet}'")
        reasons = UNRESOLVED_REASONS if kind == "UNRESOLVED" else TRUST_REASONS
        if reason not in reasons:
            raise ValueError(f"unknown {kind.lower()} reason '{reason}'")
        return (facet, reason)
    if kind == "NOT-PROVEN":
        if argument not in FACETS:
            raise ValueError(f"unknown facet '{argument}'")
        return argument
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

    def line_markers(self, kind: str) -> list[Marker]:
        return [m for m in self.markers if m.kind == kind]

    @property
    def bugs(self) -> list[Marker]:
        return self.line_markers("BUG")

    @property
    def traps(self) -> list[Marker]:
        return self.line_markers("TRAP")

    def bug_lines(self) -> set[tuple[Path, int]]:
        return {(m.file, m.line) for m in self.markers if m.kind in ("BUG", "MISS", "NEUTRALISED")}

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
        markers=markers, has_main=any(defines_main(t) for t in texts.values()), errors=errors)
    kinds = {m.kind for m in main.file_markers} | {m.kind for m in markers}
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
_LEGACY_TOOL_SWITCHES = {
    "-fweavec-strict": "--strict-externs",
    "-fweavec-exclusive-borrows": "--exclusive-borrows",
    "-fweavec-report-unannotated": "--report-unannotated",
    "-fweavec-analyze-headers": "--analyze-headers",
    "-fweavec-dump-analysis": "--dump-analysis",
    "-fweavec-checked": "--checked",
}
_LEGACY_TOOL_VALUES = {
    "-fweavec-checked-function=": "--checked-function=",
    "-fweavec-checked-report-format=": "--checked-report-format=",
    "-fweavec-checked-report=": "--checked-report=",
    "-fweavec-analysis-cache=": "--analysis-cache=",
    "-fweavec-analysis-stats=": "--analysis-stats=",
}
# RFC 0030 flags that v0.10.0 does not have (section 16).
_RFC0030_ONLY = ("-fweavec-checks=", "-fweavec-require=", "-fweavec-ledger=",
                 "-fweavec-ledger-format=", "-fweavec-budget=")
_RFC0030_SWITCHES = ("-fweavec-zero-init", "-fno-weavec-zero-init", "-fweavec-summary",
                     "-fno-weavec-summary", "-fweavec-print-prelude")


@dataclasses.dataclass
class ToolArguments:
    options: list[str]      # before the sources
    compiler: list[str]     # after `--`
    dropped: list[str]


def tool_arguments(flags: tuple[str, ...] | list[str], legacy: bool) -> ToolArguments:
    """Translate weavec-cc FLAGS for the `weavec` tool (section 16)."""
    result = ToolArguments([], [], [])
    for flag in flags:
        if _W_FLAG.match(flag):
            result.options.append(flag)
        elif flag in _LEGACY_TOOL_SWITCHES:
            result.options.append(_LEGACY_TOOL_SWITCHES[flag])
        elif any(flag.startswith(p) for p in _LEGACY_TOOL_VALUES):
            prefix = next(p for p in _LEGACY_TOOL_VALUES if flag.startswith(p))
            result.options.append(_LEGACY_TOOL_VALUES[prefix] + flag[len(prefix):])
        elif not legacy and flag.startswith("-fweavec-require="):
            result.options.append("--require=" + flag.split("=", 1)[1])
        elif not legacy and flag.startswith("-fweavec-budget="):
            result.options.append("--budget=" + flag.split("=", 1)[1])
        elif not legacy and flag == "-fno-weavec-zero-init":
            result.options.append("--no-zero-init")
        elif flag.startswith(_RFC0030_ONLY) or flag in _RFC0030_SWITCHES:
            result.dropped.append(flag)
        elif flag.startswith(("-fweavec", "-fno-weavec")):
            # -fweavec, -f[no-]weavec-link and the negative forms of the
            # switches above are defaults or meaningless for the tool.
            result.dropped.append(flag)
        else:
            result.compiler.append(flag)
    return result


def plain_flags(flags: tuple[str, ...] | list[str]) -> list[str]:
    """FLAGS without WeaveC options, for the plain-Clang ASan build."""
    return [f for f in flags if not f.startswith(("-fweavec", "-fno-weavec")) and not _W_FLAG.match(f)]


# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------

_DIAGNOSTIC = re.compile(
    r"^(?P<file>.*?):(?P<line>\d+):(?P<column>\d+): (?P<severity>error|warning): "
    r"(?P<message>.*) \[weavec::(?P<id>[a-z0-9-]+)\]$")
_LOCATIONLESS = re.compile(
    r"^(?:(?P<file>[^:]*): )?(?P<severity>error|warning): (?P<message>.*) \[weavec::(?P<id>[a-z0-9-]+)\]$")
_CLANG_ERROR = re.compile(r"(?:^|: )(?:fatal )?error: ")
_LINK_ERROR = re.compile(r"Undefined symbols|undefined reference to|linker command failed|"
                         r"^(?:ld|ld\.lld|/usr/bin/ld|collect2): (?:fatal )?error")
_REPORT = re.compile(r"^weavec: runtime check failed: (?P<template>[a-z]+) at "
                     r"(?P<file>.*):(?P<line>\d+):(?P<column>\d+)\s*$")


@dataclasses.dataclass(frozen=True, order=True)
class Diagnostic:
    file: str        # resolved path, or "" when the diagnostic has no location
    line: int
    column: int
    severity: str
    id: str
    message: str

    def key(self) -> tuple:
        return (relative(self.file) if self.file else "", self.line, self.column, self.severity,
                self.id, self.message)

    def text(self) -> str:
        where = f"{relative(self.file)}:{self.line}:{self.column}" if self.file else "<no location>"
        return f"{where}: {self.severity}: {self.message} [weavec::{self.id}]"


def resolve(file: str, cwd: Path | None) -> str:
    path = Path(file)
    if not path.is_absolute() and cwd is not None:
        path = cwd / path
    try:
        return str(path.resolve())
    except OSError:
        return str(path)


def parse_diagnostics(output: str, cwd: Path | None = None) -> tuple[list[Diagnostic], list[str], list[str]]:
    """Return (weavec diagnostics, Clang errors, linker errors) found in tool output."""
    diagnostics: list[Diagnostic] = []
    clang_errors: list[str] = []
    link_errors: list[str] = []
    for raw in output.splitlines():
        line = raw.rstrip("\r")
        if match := _DIAGNOSTIC.match(line):
            diagnostics.append(Diagnostic(resolve(match["file"], cwd), int(match["line"]),
                                          int(match["column"]), match["severity"], match["id"],
                                          match["message"]))
        elif match := _LOCATIONLESS.match(line):
            diagnostics.append(Diagnostic("", 0, 0, match["severity"], match["id"], match["message"]))
        elif _LINK_ERROR.search(line):
            link_errors.append(line)
        elif _CLANG_ERROR.search(line):
            clang_errors.append(line)
    return diagnostics, clang_errors, link_errors


@dataclasses.dataclass(frozen=True)
class CheckReport:
    template: str
    file: str
    line: int
    column: int


def parse_reports(output: str, cwd: Path | None = None) -> list[CheckReport]:
    reports = []
    for line in output.splitlines():
        if match := _REPORT.match(line.rstrip("\r")):
            reports.append(CheckReport(match["template"], resolve(match["file"], cwd),
                                       int(match["line"]), int(match["column"])))
    return reports


_SANITIZER_ERROR = re.compile(r"ERROR: AddressSanitizer: (?P<kind>[^:]+?)(?= on | at |:|$)|"
                              r"(?P<file>[^\s:]+):(?P<line>\d+):(?P<column>\d+): runtime error: (?P<what>.*)")
_FRAME = re.compile(r"#\d+ 0x[0-9a-fA-F]+ in (?P<function>\S+) (?:\()?(?P<file>[^\s():]+):(?P<line>\d+)")


@dataclasses.dataclass(frozen=True)
class SanitizerReport:
    kind: str
    frames: tuple[tuple[str, int, str], ...]  # (file, line, function), innermost first

    def in_case(self, files: list[Path]) -> list[tuple[str, int, str]]:
        """Frames inside the case's units, matched by path or, as symbolizers print, by basename."""
        resolved = {str(f.resolve()) for f in files}
        names = {f.name for f in files}
        return [frame for frame in self.frames
                if frame[0] in resolved or Path(frame[0]).name in names]


def parse_sanitizer(output: str) -> SanitizerReport | None:
    """The first ASan or UBSan report in a run's stderr, with its stack."""
    lines = output.splitlines()
    for index, line in enumerate(lines):
        match = _SANITIZER_ERROR.search(line)
        if not match:
            continue
        frames = []
        if match["file"]:
            kind = "runtime error: " + match["what"]
            frames.append((match["file"], int(match["line"]), ""))
        else:
            kind = match["kind"]
        for follow in lines[index + 1:]:
            if _SANITIZER_ERROR.search(follow) or follow.startswith("SUMMARY:"):
                break
            if frame := _FRAME.search(follow):
                frames.append((frame["file"], int(frame["line"]), frame["function"]))
        return SanitizerReport(kind, tuple(frames))
    return None


# ---------------------------------------------------------------------------
# Ledgers (section 12.1)
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Row:
    file: str
    line: int
    column: int
    kind: str
    text: str
    facets: dict  # facet -> the JSON object of section 12.1
    boundary: str = ""  # "call" or "exit" for Call sites


def ledger_rows(ledger: dict) -> list[Row]:
    root = Path(ledger.get("root") or "/")
    rows = []

    def where(name: str | None) -> str:
        if not name:
            return ""
        path = Path(name)
        return resolve(str(path if path.is_absolute() else root / path), None)

    for unit in ledger.get("units") or []:
        unit_file = where(unit.get("source"))
        for function in unit.get("functions") or []:
            function_file = where(function.get("file")) or unit_file
            for site in function.get("sites") or []:
                rows.append(Row(where(site.get("file")) or function_file, int(site.get("line", 0)),
                                int(site.get("column", 0)), str(site.get("kind", "")),
                                str(site.get("text", "")), dict(site.get("facets") or {}),
                                str(site.get("boundary") or "")))
    return rows


def facet_records(row: Row, facet: str) -> list[dict]:
    """The merged facet and each of its requirement records."""
    entry = row.facets.get(facet)
    if not isinstance(entry, dict):
        return []
    return [entry] + [r for r in entry.get("requirements") or [] if isinstance(r, dict)]


def json_pointer(document: Any, pointer: str) -> Any:
    """RFC 6901; raises KeyError when the pointer does not resolve."""
    if pointer == "":
        return document
    value = document
    for token in pointer.split("/")[1:]:
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(value, dict) and token in value:
            value = value[token]
        elif isinstance(value, list) and token.isdigit() and int(token) < len(value):
            value = value[int(token)]
        else:
            raise KeyError(pointer)
    return value


def compare(actual: Any, op: str, expected: Any) -> bool:
    if op in ("==", "!="):
        return (actual == expected) == (op == "==")
    if isinstance(actual, bool) or isinstance(expected, bool) or \
            not isinstance(actual, (int, float)) or not isinstance(expected, (int, float)):
        raise TypeError(f"'{op}' compares numbers, got {json.dumps(actual)} and {json.dumps(expected)}")
    return {"<=": actual <= expected, ">=": actual >= expected, "<": actual < expected,
            ">": actual > expected}[op]


# ---------------------------------------------------------------------------
# Evidence and evaluation
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Run:
    args: tuple[str, ...]
    returncode: int | None
    timed_out: bool = False
    stderr: str = ""

    @property
    def signal(self) -> int | None:
        return -self.returncode if self.returncode is not None and self.returncode < 0 else None

    @property
    def trapped(self) -> bool:
        return self.signal in TRAP_SIGNALS

    def describe(self) -> str:
        if self.timed_out:
            return "timed out"
        if self.signal is not None:
            try:
                return f"killed by {signal.Signals(self.signal).name}"
            except ValueError:
                return f"killed by signal {self.signal}"
        return f"exit {self.returncode}"


@dataclasses.dataclass
class Evidence:
    mode: str  # "legacy", "trap", "verify"
    diagnostics: list[Diagnostic] = dataclasses.field(default_factory=list)
    raw_diagnostics: list[Diagnostic] = dataclasses.field(default_factory=list)  # as printed
    clang_errors: list[str] = dataclasses.field(default_factory=list)
    tool_failures: list[str] = dataclasses.field(default_factory=list)
    ledgers: list[dict] = dataclasses.field(default_factory=list)
    ledger_expected: bool = False
    built: bool = False  # an executable was produced
    ran: bool = False
    runs: list[Run] = dataclasses.field(default_factory=list)
    reports: list[CheckReport] = dataclasses.field(default_factory=list)
    proven_traps: list[str] = dataclasses.field(default_factory=list)
    asan_ran: bool = False
    asan: SanitizerReport | None = None
    asan_runs: list[Run] = dataclasses.field(default_factory=list)
    ledger_proxy: bool = False  # TRAP markers are judged from the ledger (no runs possible)
    no_emission: bool = False
    zero_init: bool = False     # zero-initialisation is in effect in the executable build
    notes: list[str] = dataclasses.field(default_factory=list)
    commands: list[str] = dataclasses.field(default_factory=list)
    seconds: float = 0.0

    def program_ledger(self) -> dict | None:
        programs = [l for l in self.ledgers if l.get("scope") == "program"]
        return programs[-1] if programs else None

    def rows(self) -> list[Row]:
        program = self.program_ledger()
        if program is not None:
            return ledger_rows(program)
        return [row for ledger in self.ledgers for row in ledger_rows(ledger)]


def located(file: Path | str, line: int) -> tuple[str, int]:
    return (str(Path(file).resolve()), line)


def loc(marker: Marker) -> str:
    return f"{relative(marker.file)}:{marker.line}"


def bug_class_and_satisfaction(case: Case, bug: Marker, ev: Evidence,
                               index: dict) -> tuple[str, str | None, bool]:
    """Return (class, what satisfies the marker or None, reported at any severity).

    The class is the strongest observed evidence at the line: a diagnostic
    with the id (error or warning), a runtime check failure whose template
    enforces the id's facet (trap), a non-proven matching facet or a matched
    UNRESOLVED/TRUSTED/NOT-PROVEN marker (row), then neutralised, miss and
    silent. Satisfaction follows section 17.3: the diagnostic with the right
    severity class; a matched TRAP marker on the line (null, spatial and
    assertion ids, or any facet for `violation`); under --no-emission, TOOL
    or --no-run, a checked matching facet (null and spatial); a matched
    ledger marker on the line; NEUTRALISED; MISS.
    """
    identifier, severity = bug.value
    facet = FACET_OF_ID.get(identifier)
    at = located(bug.file, bug.line)
    legacy = ev.mode == "legacy"
    same = [d for d in index["diagnostics"].get(at, []) if d.id == identifier]
    wanted = [d for d in same if severity is None or (d.severity == "error") == (severity == "definite")]
    reported = bool(same)
    satisfied = "diagnostic" if wanted else None
    klass = "error" if any(d.severity == "error" for d in same) else "warning" if same else None
    if not legacy:
        if facet and any(r.template in TEMPLATES_OF_FACET[facet] for r in index["reports"].get(at, [])):
            reported = True
            klass = klass or "trap"
        if satisfied is None and facet:
            for trap in index["traps"].get(at, []):
                enforces = trap.value == "violation" or (
                    facet in TRAPPABLE_FACETS and trap.value in TEMPLATES_OF_FACET[facet])
                if enforces and index["trap_ok"].get(id(trap)):
                    satisfied, reported = "trap", True
        rows = [r for r in index["rows"].get(at, []) if facet and facet in r.facets]
        outcomes = {rec.get("outcome") for r in rows for rec in facet_records(r, facet)[:1]}
        if satisfied is None and (ev.no_emission or ev.ledger_proxy) and facet in ("null", "spatial") \
                and "checked" in outcomes:
            satisfied, reported = "checked", True
        ledger_match = any(index["ledger_ok"].get(id(m)) for m in index["ledger_markers"].get(at, []))
        if outcomes - {"proven", None} or ledger_match:
            klass = klass or "row"
        if satisfied is None and ledger_match:
            satisfied = "row"
    if index["neutralised"].get(at):
        effective = not legacy and not ev.no_emission and ev.zero_init
        klass = klass or ("neutralised" if effective else "miss")
        satisfied = satisfied or ("neutralised" if effective else "miss")
    if index["misses"].get(at):
        klass = klass or "miss"
        satisfied = satisfied or "miss"
    return klass or "silent", satisfied, reported


def legacy_class(case: Case, diagnostics: list[Diagnostic]) -> str:
    """The v0.10.0 probe classification (scratchpad table.txt, 'ord=' column)."""
    files = {str(u.resolve()) for u in case.units}
    own = [d for d in diagnostics if d.file in files]
    for bug in case.bugs:
        at = located(bug.file, bug.line)
        if any((d.file, d.line) == at and d.id == bug.value[0] for d in own):
            return "CAUGHT"
    ids = {d.id for d in own}
    if ids - LEGACY_SIGNAL_IDS - {"leak"}:
        return "MISLABEL"
    if ids & LEGACY_SIGNAL_IDS:
        return "SIGNAL"
    if ids:
        return "LEAK-ONLY"
    return "SILENT"


def evaluate(case: Case, ev: Evidence) -> dict:
    """Judge one case's evidence against its markers (pure; see section 17.4)."""
    failures: list[str] = []
    notes = list(ev.notes)
    legacy = ev.mode == "legacy"
    failures.extend(ev.tool_failures)
    failures.extend(f"compiler error: {e}" for e in ev.clang_errors)

    index: dict[str, Any] = {k: collections.defaultdict(list) for k in
                             ("diagnostics", "reports", "rows", "traps", "ledger_markers")}
    index["neutralised"] = collections.defaultdict(list)
    index["misses"] = collections.defaultdict(list)
    index["trap_ok"] = {}
    index["ledger_ok"] = {}
    for d in ev.diagnostics:
        if d.file:
            index["diagnostics"][(d.file, d.line)].append(d)
    for r in ev.reports:
        index["reports"][(r.file, r.line)].append(r)
    rows = ev.rows() if not legacy else []
    for row in rows:
        index["rows"][(row.file, row.line)].append(row)
    for marker in case.markers:
        at = located(marker.file, marker.line)
        if marker.kind == "TRAP":
            index["traps"][at].append(marker)
        elif marker.kind in ("UNRESOLVED", "TRUSTED", "NOT-PROVEN"):
            index["ledger_markers"][at].append(marker)
        elif marker.kind == "NEUTRALISED":
            index["neutralised"][at].append(marker)
        elif marker.kind == "MISS":
            index["misses"][at].append(marker)
    ledger_available = bool(ev.ledgers) and not legacy
    if ev.ledger_expected and not ev.ledgers and not legacy:
        needs_ledger = any(m.kind in ("UNRESOLVED", "TRUSTED", "NOT-PROVEN") for m in case.markers) \
            or case.expectations
        (failures if needs_ledger else notes).append("the build wrote no ledger")

    # Step 3: ledger markers.
    for marker in case.markers:
        if marker.kind not in ("UNRESOLVED", "TRUSTED", "NOT-PROVEN") or legacy:
            continue
        at = located(marker.file, marker.line)
        here = index["rows"].get(at, [])
        if marker.kind == "NOT-PROVEN":
            facet = marker.value
            # A function exit on the line (`return p[0];`) is a boundary (section 9.4), not
            # the operation the marker is about, unless it is the only row there.
            accesses = [r for r in here if r.boundary != "exit" and facet_records(r, facet)]
            outcomes = [rec.get("outcome") for r in (accesses or here) for rec in facet_records(r, facet)[:1]]
            ok = bool(outcomes) and "proven" not in outcomes
            if not ok and ledger_available:
                failures.append(f"{loc(marker)}: NOT-PROVEN {facet}: " + (
                    "the facet is proven" if outcomes else f"no ledger row with a {facet} facet here"))
        else:
            facet, reason = marker.value
            outcome = "unresolved" if marker.kind == "UNRESOLVED" else "trusted"
            ok = any(rec.get("outcome") == outcome and rec.get("reason") == reason
                     for r in here for rec in facet_records(r, facet))
            if not ok and ledger_available:
                found = sorted({f"{rec.get('outcome')}({rec.get('reason')})" if rec.get("reason")
                                else str(rec.get("outcome")) for r in here for rec in facet_records(r, facet)})
                failures.append(f"{loc(marker)}: {marker.kind} {facet}:{reason} not in the ledger"
                                + (f" (found {', '.join(found)})" if found else " (no row here)"))
        index["ledger_ok"][id(marker)] = ok
    if not legacy and case.expectations:
        target = ev.program_ledger()
        if target is None:
            main = str(case.path)
            target = next((l for l in ev.ledgers if any(
                resolve(str(Path(l.get("root") or "/") / (u.get("source") or "")), None) == main
                for u in l.get("units") or [])), ev.ledgers[0] if ev.ledgers else None)
        for expectation in case.expectations:
            if target is None:
                break
            try:
                actual = json_pointer(target, expectation.pointer)
                if not compare(actual, expectation.op, expectation.value):
                    failures.append(f"EXPECT-LEDGER {expectation.text}: actual {json.dumps(actual)}")
            except KeyError:
                failures.append(f"EXPECT-LEDGER {expectation.text}: pointer not found")
            except TypeError as error:
                failures.append(f"EXPECT-LEDGER {expectation.text}: {error}")

    # Step 4: runs and TRAP markers.
    trap_expected = bool(case.traps)
    for run in ev.runs:
        what = f"run {shlex.join(run.args) or '(no arguments)'}"
        if run.timed_out:
            failures.append(f"{what}: timed out")
        elif run.trapped != trap_expected and not legacy:
            failures.append(f"{what}: {run.describe()}, but the case has "
                            + ("TRAP markers" if trap_expected else "no TRAP marker"))
        elif case.clean and run.signal is not None and not run.trapped:
            failures.append(f"{what}: {run.describe()}")
    for trap in case.traps:
        if legacy:
            continue
        at = located(trap.file, trap.line)
        if ev.ran:
            ok = any(r.template == trap.value for r in index["reports"].get(at, []))
            if not ok:
                failures.append(f"{loc(trap)}: TRAP {trap.value} not reported by the report-mode run")
        elif ev.ledger_proxy and ledger_available:
            ok = any(rec.get("outcome") == "checked" and (rec.get("check") or {}).get("template") == trap.value
                     for r in index["rows"].get(at, []) for facet in r.facets for rec in facet_records(r, facet))
            if not ok:
                failures.append(f"{loc(trap)}: TRAP {trap.value}: no checked facet with that template "
                                f"in the ledger")
        else:
            ok = False
            stopped = any(d.severity == "error" and d.id == bug.value[0] for bug in case.bugs
                          if located(bug.file, bug.line) == at for d in index["diagnostics"].get(at, []))
            if not stopped:
                failures.append(f"{loc(trap)}: TRAP {trap.value} not observed: no executable was built")
        index["trap_ok"][id(trap)] = ok
    if ev.ran:
        for report in ev.reports:
            at = (report.file, report.line)
            if not any(t.value == report.template for t in index["traps"].get(at, [])):
                failures.append(f"unexpected runtime check failure: {report.template} at "
                                f"{relative(report.file)}:{report.line}:{report.column}")
    failures.extend(ev.proven_traps)

    # Step 2: diagnostics and BUG markers (after the traps they may rely on).
    bug_results = []
    for bug in case.bugs:
        klass, satisfied, reported = bug_class_and_satisfaction(case, bug, ev, index)
        identifier, severity = bug.value
        bug_results.append({"file": relative(bug.file), "line": bug.line, "id": identifier,
                            "severity": severity, "class": klass, "satisfiedBy": satisfied,
                            "reported": reported})
        if satisfied is None:
            here = index["diagnostics"].get(located(bug.file, bug.line), [])
            found = ", ".join(sorted({f"{d.id} ({d.severity})" for d in here})) or "nothing"
            failures.append(f"{loc(bug)}: BUG {identifier}{' ' + severity if severity else ''} "
                            f"not satisfied (found {found})")
        elif reported and index["misses"].get(located(bug.file, bug.line)):
            notes.append(f"{loc(bug)}: known miss is now reported ({klass})")
    for kind in ("MISS", "NEUTRALISED"):
        for marker in case.line_markers(kind):
            at = located(marker.file, marker.line)
            if any(located(b.file, b.line) == at for b in case.bugs):
                continue
            here = index["diagnostics"].get(at, [])
            klass = ("error" if any(d.severity == "error" for d in here) else "warning" if here else
                     "neutralised" if kind == "NEUTRALISED" and not legacy and not ev.no_emission
                     and ev.zero_init else "miss")
            bug_results.append({"file": relative(marker.file), "line": marker.line, "id": None,
                                "severity": None, "class": klass, "satisfiedBy": klass,
                                "reported": bool(here)})
    bug_lines = {located(file, line) for file, line in case.bug_lines()}
    for d in ev.diagnostics:
        if d.severity == "error" and (d.file, d.line) not in bug_lines:
            failures.append(f"unexpected error: {d.text()}")
        elif case.clean and d.severity == "warning" and d.id not in case.allow:
            failures.append(f"unexpected warning: {d.text()}")

    # Step 5: the ASan oracle.
    asan = None
    if ev.asan_ran:
        report = ev.asan
        asan = {"report": report.kind if report else None}
        if report is not None:
            frames = report.in_case(case.units)
            asan["site"] = f"{relative(frames[0][0]) if Path(frames[0][0]).is_absolute() else frames[0][0]}:" \
                           f"{frames[0][1]}" if frames else None
            if case.clean:
                failures.append(f"ASan reported {report.kind}"
                                + (f" at {asan['site']}" if frames else "") + " in a CLEAN case")
            elif not frames:
                notes.append(f"ASan reported {report.kind} outside the case's files")
            elif ledger_available and case.bugs:
                problem = proven_bug_site(case, frames, index)
                if problem:
                    failures.append(problem)
        elif case.is_bug_case and case.asan and any(b.value[0] in FACET_OF_ID for b in case.bugs):
            failures.append("the ASan oracle did not report the bug (probe not validated)")
        elif case.is_bug_case:
            notes.append("the ASan oracle reported nothing")

    classes = [b["class"] for b in bug_results]
    klass = min(classes, key=CLASSES.index) if classes else None
    result = {
        "case": case.rel, "suite": case.suite,
        "status": "error" if case.errors else "fail" if failures else "pass",
        "failures": list(case.errors) + failures, "notes": notes,
        "kind": "bug" if case.is_bug_case else "clean" if case.clean else "other",
        "class": klass, "bugs": bug_results,
        "diagnostics": [d.text() for d in sorted(ev.diagnostics)],
        "runs": [{"args": list(r.args), "result": r.describe()} for r in ev.runs],
        "reports": [f"{r.template} at {relative(r.file)}:{r.line}:{r.column}" for r in ev.reports],
        "asan": asan, "commands": ev.commands, "seconds": round(ev.seconds, 3),
    }
    if legacy:
        result["legacyClass"] = legacy_class(case, ev.diagnostics) if case.is_bug_case else None
    if case.clean:
        result["cleanBuild"] = not any(d.severity == "error" for d in ev.diagnostics) and not ev.clang_errors
    return result


def proven_bug_site(case: Case, frames: list[tuple[str, int, str]], index: dict) -> str | None:
    """Gate G4: the matching facet at the ASan-reported bug site must not be proven.

    A proof inside a callee can rest on a requirement checked at its call
    (section 7.5), so a proven site passes when an enclosing in-case frame's
    call has the matching facet non-proven.
    """
    def rows_at(frame: tuple[str, int, str]) -> list[Row]:
        file, line, _ = frame
        candidates = [f for f in case.units if str(f.resolve()) == file or f.name == Path(file).name]
        return [r for f in candidates for r in index["rows"].get(located(f, line), [])]

    def facet_for(frame: tuple[str, int, str]) -> str | None:
        file, line, _ = frame
        for bug in case.bugs:
            if bug.line == line and (str(bug.file.resolve()) == file or bug.file.name == Path(file).name):
                return FACET_OF_ID.get(bug.value[0])
        return next((FACET_OF_ID[b.value[0]] for b in case.bugs if b.value[0] in FACET_OF_ID), None)

    facet = facet_for(frames[0])
    if facet is None:
        return None
    outcomes = [rec.get("outcome") for r in rows_at(frames[0]) for rec in facet_records(r, facet)[:1]]
    if "proven" not in outcomes or any(o != "proven" for o in outcomes):
        return None
    for frame in frames[1:]:
        outer = [rec.get("outcome") for r in rows_at(frame) for rec in facet_records(r, facet)[:1]]
        if any(o not in ("proven", None) for o in outer):
            return None
    file, line, _ = frames[0]
    return f"G4: the {facet} facet is proven at the ASan-reported bug site {Path(file).name}:{line}"


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Config:
    weavec: Path | None
    weavec_cc: Path | None
    legacy: bool = False
    checks: str = "trap"
    require: str | None = None
    asan: bool = False
    no_emission: bool = False
    no_run: bool = False
    compile_timeout: float = COMPILE_TIMEOUT
    run_timeout: float = RUN_TIMEOUT
    keep: bool = False
    lldb: bool = False
    work: Path | None = None


def run_process(command: list[str], cwd: Path, timeout: float, stdin: Path | None = None,
                env: dict | None = None) -> tuple[int | None, str, str, bool]:
    try:
        with open(stdin, "rb") if stdin else open(os.devnull, "rb") as handle:
            completed = subprocess.run(command, cwd=cwd, stdin=handle, capture_output=True,
                                       timeout=timeout, env=env, check=False)
        return (completed.returncode, completed.stdout.decode(errors="replace"),
                completed.stderr.decode(errors="replace"), False)
    except subprocess.TimeoutExpired as expired:
        out = (expired.stdout or b"").decode(errors="replace")
        err = (expired.stderr or b"").decode(errors="replace")
        return None, out, err, True
    except OSError as error:
        return 127, "", str(error), False


def run_environment(**extra: str) -> dict:
    env = dict(os.environ)
    env.pop("WEAVEC_RT_ABORT", None)
    env.update(extra)
    return env


def legacy_command(weavec: Path, case: Case) -> list[str]:
    """v0.10.0 semantics: `weavec [--whole-program] <files> -- <flags>`, as scripts/evaluate.py ran them.

    No -I for weavec.h: each binary reads the header it was built with
    (WEAVEC_RESOURCE_DIR, or its own checkout; see test/cases/GOLDEN.md).
    """
    translated = tool_arguments(case.flags, legacy=True)
    units = case.analysed_units()
    command = [str(weavec), *translated.options]
    if len(units) > 1:
        command.append("--whole-program")
    command.extend(str(u) for u in units)
    command.extend(["--", "-ferror-limit=0", "-fno-color-diagnostics", *translated.compiler])
    return command


def record_tool(ev: Evidence, command: list[str], cwd: Path, timeout: float,
                what: str) -> tuple[int | None, str]:
    ev.commands.append(shlex.join(command))
    code, out, err, timed_out = run_process(command, cwd, timeout)
    output = out + ("\n" if out and not out.endswith("\n") else "") + err
    diagnostics, clang_errors, link_errors = parse_diagnostics(output, cwd)
    ev.diagnostics.extend(diagnostics)
    ev.clang_errors.extend(clang_errors)
    if timed_out:
        ev.tool_failures.append(f"{what} timed out after {timeout:g} s")
    elif code is None or code < 0 or code > 1:
        ev.tool_failures.append(f"{what} failed with status {code}: {first_line(err)}")
    elif code == 1 and not clang_errors and not link_errors and \
            not any(d.severity == "error" for d in diagnostics):
        ev.tool_failures.append(f"{what} failed without a diagnostic: {first_line(err)}")
    if link_errors:
        ev.tool_failures.append(f"{what}: {link_errors[0]}")
    return code, output


def first_line(text: str) -> str:
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:200]
    return ""


def run_legacy(case: Case, weavec: Path, cfg: Config) -> Evidence:
    ev = Evidence("legacy")
    start = time.perf_counter()
    dropped = tool_arguments(case.flags, legacy=True).dropped
    if dropped:
        ev.notes.append("legacy mode ignores " + " ".join(dropped))
    record_tool(ev, legacy_command(weavec, case), case.path.parent, cfg.compile_timeout, "weavec")
    ev.raw_diagnostics = list(ev.diagnostics)
    ev.diagnostics = sorted(set(ev.diagnostics))
    ev.seconds = time.perf_counter() - start
    return ev


def unit_objects(case: Case, directory: Path) -> dict[Path, Path]:
    objects: dict[Path, Path] = {}
    used: set[str] = set()
    for unit in case.units:
        name = unit.stem
        while name + ".o" in used:
            name += "_"
        used.add(name + ".o")
        objects[unit] = directory / (name + ".o")
    return objects


def cc_flags(case: Case, cfg: Config, checks: str | None, unit: Path | None) -> list[str]:
    """weavec-cc flags for a compile (unit given) or the link (unit None)."""
    own = case.unit_flags.get(unit, ()) if unit is not None else ()
    plain = "-fno-weavec" in own
    flags: list[str] = []
    if not plain:
        if checks and not cfg.no_emission:
            flags.append(f"-fweavec-checks={checks}")
        if cfg.require:
            flags.append(f"-fweavec-require={cfg.require}")
        flags.extend(case.flags)
    else:
        flags.extend(plain_flags(case.flags))
    flags.extend(own)
    return flags


def build(case: Case, cfg: Config, ev: Evidence, directory: Path, checks: str | None,
          ledger: Path | None, extra: list[str], record: bool) -> Path | None:
    """Compile every unit and link them when one defines main; return the executable."""
    directory.mkdir(parents=True, exist_ok=True)
    objects = unit_objects(case, directory)
    ok = True
    for unit in case.units:
        plain = "-fno-weavec" in case.unit_flags.get(unit, ())
        command = [str(cfg.weavec_cc), "-c", "-fno-color-diagnostics", "-ferror-limit=0", *extra,
                   *cc_flags(case, cfg, checks, unit)]
        if ledger is not None and not plain:
            command.append(f"-fweavec-ledger={ledger}{os.sep}")
        command.extend([str(unit), "-o", str(objects[unit])])
        if record:
            code, _ = record_tool(ev, command, directory, cfg.compile_timeout, f"compile {unit.name}")
        else:
            code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
            if code != 0:
                ev.tool_failures.append(f"{checks}-mode compile of {unit.name} failed: "
                                        + ("timed out" if timed_out else first_line(err)))
        ok = ok and code == 0 and objects[unit].exists()
    if not ok or not case.has_main:
        return None
    output = directory / "a.out"
    command = [str(cfg.weavec_cc), *extra, *cc_flags(case, cfg, checks, None)]
    if ledger is not None:
        command.append(f"-fweavec-ledger={ledger}{os.sep}")
    command.extend([*(str(objects[u]) for u in case.units), "-o", str(output)])
    if record:
        code, _ = record_tool(ev, command, directory, cfg.compile_timeout, "link")
    else:
        code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
        if code != 0:
            ev.tool_failures.append(f"{checks}-mode link failed: "
                                    + ("timed out" if timed_out else first_line(err)))
    return output if code == 0 and output.exists() else None


def load_ledgers(ev: Evidence, directory: Path) -> None:
    for path in sorted(directory.glob("*.json")) if directory.is_dir() else []:
        try:
            ledger = json.loads(path.read_text())
        except (OSError, ValueError) as error:
            ev.tool_failures.append(f"ledger {path.name} is not valid JSON: {error}")
            continue
        if not isinstance(ledger, dict) or ledger.get("schema") != "weavec-ledger" or \
                ledger.get("version") != 1:
            ev.tool_failures.append(f"ledger {path.name} is not a weavec-ledger version 1 document")
            continue
        ev.ledgers.append(ledger)


def run_tool_case(case: Case, cfg: Config, ev: Evidence, ledger_dir: Path, cwd: Path) -> None:
    translated = tool_arguments(case.flags, legacy=False)
    units = case.analysed_units()
    command = [str(cfg.weavec), f"--ledger={ledger_dir / 'tool.ledger.json'}", *translated.options]
    if cfg.require:
        command.append(f"--require={cfg.require}")
    if len(units) > 1:
        command.append("--whole-program")
    command.extend(str(u) for u in units)
    command.extend(["--", "-ferror-limit=0", "-fno-color-diagnostics", *translated.compiler])
    record_tool(ev, command, cwd, cfg.compile_timeout, "weavec")


def zero_init_effective(case: Case, cfg: Config) -> bool:
    flags = set(case.flags)
    return not cfg.no_emission and "-fno-weavec-zero-init" not in flags and \
        "-fweavec-checks=none" not in flags


def run_executable(exe: Path, case: Case, cfg: Config, cwd: Path, env: dict,
                   timeout: float) -> list[Run]:
    runs = []
    for run_input in case.run_inputs or [RunInput((), None)]:
        code, _, err, timed_out = run_process([str(exe), *run_input.args], cwd, timeout,
                                              run_input.stdin, env)
        runs.append(Run(run_input.args, code, timed_out, err))
    return runs


def lldb_trap_reason(exe: Path, run_input: RunInput, cwd: Path) -> str | None:
    """Best effort: LLDB names the category of a __builtin_verbose_trap in its stop reason."""
    lldb = shutil.which("lldb")
    if not lldb:
        return None
    command = [lldb, "--batch", "-o", "run", "-o", "thread info", "--", str(exe), *run_input.args]
    _, out, err, _ = run_process(command, cwd, 60, run_input.stdin)
    return out + err


def run_case(case: Case, cfg: Config) -> dict:
    if case.errors:
        return evaluate(case, Evidence("legacy" if cfg.legacy else cfg.checks))
    if cfg.legacy:
        return evaluate(case, run_legacy(case, cfg.weavec, cfg))
    start = time.perf_counter()
    ev = Evidence(cfg.checks)
    ev.no_emission = cfg.no_emission
    ev.zero_init = zero_init_effective(case, cfg)
    temp = Path(tempfile.mkdtemp(prefix="weavec-case-", dir=cfg.work))
    try:
        ledger_dir = temp / "ledger"
        ledger_dir.mkdir()
        ev.ledger_expected = True
        if case.tool:
            ev.ledger_proxy = True
            run_tool_case(case, cfg, ev, ledger_dir, temp)
            load_ledgers(ev, ledger_dir)
        else:
            extra = ["-g"] if cfg.checks == "verify" else []
            exe = build(case, cfg, ev, temp / "main", cfg.checks, ledger_dir, extra, record=True)
            ev.built = exe is not None
            analysed = case.analysed_units()
            if not case.has_main and len(analysed) > 1:
                translated = tool_arguments(case.flags, legacy=False)
                command = [str(cfg.weavec), f"--ledger={ledger_dir / 'whole-program.ledger.json'}",
                           *translated.options, *(["--require=" + cfg.require] if cfg.require else []),
                           "--whole-program", *(str(u) for u in analysed), "--", "-ferror-limit=0",
                           "-fno-color-diagnostics", *translated.compiler]
                record_tool(ev, command, temp, cfg.compile_timeout, "weavec --whole-program")
            load_ledgers(ev, ledger_dir)
            ev.diagnostics = sorted(set(ev.diagnostics))
            runs_possible = ev.built and not cfg.no_run and not cfg.no_emission
            ev.ledger_proxy = not runs_possible and (cfg.no_run or cfg.no_emission or not case.has_main)
            if runs_possible:
                env = run_environment()
                ev.runs = run_executable(exe, case, cfg, temp, env, cfg.run_timeout)
                report_exe = build(case, cfg, ev, temp / "report", "report", None, [], record=False)
                if report_exe is None:
                    ev.tool_failures.append("the report-mode build failed")
                else:
                    ev.ran = True
                    report_runs = run_executable(report_exe, case, cfg, temp, env, cfg.run_timeout)
                    seen = set()
                    for run in report_runs:
                        for report in parse_reports(run.stderr, temp):
                            if report not in seen:
                                seen.add(report)
                                ev.reports.append(report)
                    if cfg.checks == "verify":
                        for run, report_run, run_input in zip(ev.runs, report_runs,
                                                              case.run_inputs or [RunInput((), None)]):
                            if not run.trapped:
                                continue
                            what = shlex.join(run.args) or "(no arguments)"
                            if not parse_reports(report_run.stderr, temp):
                                ev.proven_traps.append(
                                    f"run {what}: weavec.proven trap (the verify build trapped where "
                                    f"no unproven check failed)")
                            elif cfg.lldb:
                                reason = lldb_trap_reason(exe, run_input, temp)
                                if reason and "weavec.proven" in reason:
                                    ev.proven_traps.append(f"run {what}: weavec.proven trap")
        if (cfg.asan or case.asan) and not case.tool and case.has_main and not cfg.no_run:
            run_asan(case, cfg, ev, temp / "asan")
    finally:
        if cfg.keep:
            ev.notes.append(f"kept {temp}")
        else:
            shutil.rmtree(temp, ignore_errors=True)
    ev.seconds = time.perf_counter() - start
    return evaluate(case, ev)


def run_asan(case: Case, cfg: Config, ev: Evidence, directory: Path) -> None:
    """Plain Clang with weavec.h (-fno-weavec) and ASan, plus array-bounds for static arrays.

    The -I is needed because v0.10.0's -fno-weavec also drops weavec.h.
    """
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / "a.out"
    command = [str(cfg.weavec_cc), "-fno-weavec", "-fsanitize=address", "-fsanitize=array-bounds",
               "-fno-omit-frame-pointer", "-g", "-O0", f"-I{RESOURCE_INCLUDE}", *plain_flags(case.flags)]
    for unit in case.units:
        command.extend(plain_flags(case.unit_flags.get(unit, ())))
    command.extend([*(str(u) for u in case.units), "-o", str(output)])
    code, _, err, timed_out = run_process(command, directory, cfg.compile_timeout)
    if code != 0 or not output.exists():
        ev.tool_failures.append("the ASan build failed: " + ("timed out" if timed_out else first_line(err)))
        return
    options = "detect_leaks=0:detect_stack_use_after_return=1:abort_on_error=0"
    env = run_environment(
        ASAN_OPTIONS=options + (":" + os.environ["ASAN_OPTIONS"] if os.environ.get("ASAN_OPTIONS") else ""),
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    ev.asan_ran = True
    for run_input in case.run_inputs or [RunInput((), None)]:
        code, _, err, timed_out = run_process([str(output), *run_input.args], directory, ASAN_TIMEOUT,
                                              run_input.stdin, env)
        ev.asan_runs.append(Run(run_input.args, code, timed_out))
        report = parse_sanitizer(err)
        if report is not None and ev.asan is None:
            ev.asan = report


# ---------------------------------------------------------------------------
# --compare-golden
# ---------------------------------------------------------------------------

_FLAG_SUPPORT: dict[tuple[str, str], bool] = {}


def tool_accepts(weavec: Path, option: str, scratch: Path) -> bool:
    key = (str(weavec), option)
    if key not in _FLAG_SUPPORT:
        source = scratch / "empty.c"
        source.write_text("int weavec_probe;\n")
        code, _, err, _ = run_process([str(weavec), option, str(source), "--"], scratch, 60)
        _FLAG_SUPPORT[key] = code == 0
    return _FLAG_SUPPORT[key]


def compare_case(case: Case, weavec: Path, golden: Path, cfg: Config, scratch: Path) -> dict:
    if case.errors:
        result = evaluate(case, Evidence("legacy"))
        result.update(compared=False)
        return result
    rejected = [o for o in tool_arguments(case.flags, legacy=True).options
                if not tool_accepts(weavec, o, scratch)]
    if rejected:
        result = evaluate(case, Evidence("legacy"))
        result.update(status="skip", compared=False, failures=[],
                      notes=[f"excluded: the binary under test rejects {' '.join(rejected)}"])
        return result
    mine = run_legacy(case, weavec, cfg)
    theirs = run_legacy(case, golden, cfg)
    result = evaluate(case, mine)
    ours = collections.Counter(d.key() for d in mine.raw_diagnostics)
    gold = collections.Counter(d.key() for d in theirs.raw_diagnostics)
    differences = ["- golden: " + format_key(key) for key in sorted((gold - ours).elements())]
    differences += ["+ tested: " + format_key(key) for key in sorted((ours - gold).elements())]
    failed_tools = [f"tested {f}" for f in mine.tool_failures] + [f"golden {f}" for f in theirs.tool_failures]
    result["compared"] = True
    result["differences"] = differences
    result["markerStatus"] = result["status"]
    result["markerFailures"] = result["failures"]
    result["failures"] = differences + failed_tools
    result["status"] = "fail" if result["failures"] else "pass"
    return result


def format_key(key: tuple) -> str:
    file, line, column, severity, identifier, message = key
    where = f"{file}:{line}:{column}" if file else "<no location>"
    return f"{where}: {severity}: {message} [weavec::{identifier}]"


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def summarize(results: list[dict]) -> dict:
    suites: dict[str, dict] = {}
    for result in results:
        suite = suites.setdefault(result["suite"], {
            "cases": 0, "passed": 0, "failed": 0, "errors": 0, "skipped": 0,
            "bugCases": 0, "bugCasesPassed": 0, "classes": {c: 0 for c in CLASSES},
            "pins": 0, "pinsSatisfied": 0, "pinsReported": 0,
            "cleanCases": 0, "cleanPassed": 0, "cleanBuilds": 0})
        suite["cases"] += 1
        status = result["status"]
        suite[{"pass": "passed", "fail": "failed", "error": "errors", "skip": "skipped"}[status]] += 1
        if result["kind"] == "bug":
            suite["bugCases"] += 1
            suite["bugCasesPassed"] += status == "pass"
            if result.get("class"):
                suite["classes"][result["class"]] += 1
            if "legacyClass" in result and result["legacyClass"]:
                legacy = suite.setdefault("legacyClasses", {c: 0 for c in LEGACY_CLASSES})
                legacy[result["legacyClass"]] += 1
        elif result["kind"] == "clean":
            suite["cleanCases"] += 1
            suite["cleanPassed"] += status == "pass"
            suite["cleanBuilds"] += bool(result.get("cleanBuild"))
        for bug in result.get("bugs", []):
            if bug["id"] is None:
                continue
            suite["pins"] += 1
            suite["pinsSatisfied"] += bug["satisfiedBy"] is not None
            suite["pinsReported"] += bool(bug["reported"])
    total = {k: sum(s[k] for s in suites.values()) for k in
             ("cases", "passed", "failed", "errors", "skipped")}
    return {"total": total, "suites": suites}


def print_result(result: dict, verbose: bool) -> None:
    label = {"pass": "PASS", "fail": "FAIL", "error": "ERROR", "skip": "SKIP"}[result["status"]]
    extra = []
    if result.get("legacyClass"):
        extra.append(result["legacyClass"])
    elif result.get("class"):
        extra.append(result["class"])
    print(f"{label:5} {result['case']}" + (f"  [{', '.join(extra)}]" if extra else ""), flush=True)
    for failure in result["failures"]:
        print(f"      {failure}")
    if verbose:
        for note in result["notes"]:
            print(f"      note: {note}")
        for command in result.get("commands", []):
            print(f"      $ {command}")


def print_summary(summary: dict, legacy: bool, compared: bool = False) -> None:
    print()
    verdict = "identical to the golden run" if compared else "passed"
    for name, suite in sorted(summary["suites"].items()):
        parts = [f"{suite['passed']}/{suite['cases']} cases {verdict}"]
        if suite["failed"] or suite["errors"]:
            parts.append(f"{suite['failed']} {'differ' if compared else 'failed'}, "
                         f"{suite['errors']} marker errors")
        if suite["skipped"]:
            parts.append(f"{suite['skipped']} {'excluded' if compared else 'skipped'}")
        if suite["bugCases"] and not compared:
            parts.append(f"bug cases {suite['bugCasesPassed']}/{suite['bugCases']}")
            parts.append(f"pins {suite['pinsSatisfied']}/{suite['pins']} satisfied, "
                         f"{suite['pinsReported']} reported")
        if suite["cleanCases"] and not compared:
            parts.append(f"clean {suite['cleanPassed']}/{suite['cleanCases']}")
        print(f"{name}: " + "; ".join(parts))
        if suite["bugCases"]:
            if legacy and "legacyClasses" in suite:
                print("  v0.10.0 classes: " + ", ".join(
                    f"{suite['legacyClasses'][c]} {c.lower()}" for c in LEGACY_CLASSES))
            else:
                print("  classes: " + ", ".join(f"{suite['classes'][c]} {c}" for c in CLASSES
                                                if suite["classes"][c]))
    total = summary["total"]
    print(f"total: {total['passed']}/{total['cases']} cases {verdict}, {total['failed']} "
          f"{'differ' if compared else 'failed'}, {total['errors']} marker errors, {total['skipped']} skipped")


def default_build_dir() -> Path:
    for preset in ("release", "dev"):
        if (ROOT / "build" / preset / "bin" / "weavec-cc").exists():
            return ROOT / "build" / preset
    return ROOT / "build" / "dev"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="See test/cases/README.md for the marker grammar.")
    parser.add_argument("--weavec-cc", type=Path, help="weavec-cc to test (default: <build-dir>/bin)")
    parser.add_argument("--weavec", type=Path, help="weavec to test (default: <build-dir>/bin)")
    parser.add_argument("--build-dir", type=Path,
                        help="build tree whose bin/ holds the binaries (default: build/release, else build/dev)")
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES, help="the cases tree")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1,
                        help="cases run at once (default: the CPU count)")
    parser.add_argument("--checks", choices=("trap", "verify"), default="trap",
                        help="the -fweavec-checks mode of the executable build (default: trap)")
    parser.add_argument("--require", choices=("none", "checked", "proven"),
                        help="add -fweavec-require to every build")
    parser.add_argument("--asan", action="store_true", help="run the ASan oracle for every case")
    parser.add_argument("--legacy", action="store_true",
                        help="v0.10.0 semantics against WEAVEC_GOLDEN_DIR: diagnostics only")
    parser.add_argument("--no-emission", action="store_true",
                        help="checks are not emitted yet: checked facets stand in for traps")
    parser.add_argument("--no-run", action="store_true", help="build, diagnostics and ledger only")
    parser.add_argument("--compare-golden", action="store_true",
                        help="fail on any difference from the golden binaries' sorted diagnostics")
    parser.add_argument("--golden-dir", type=Path, default=os.environ.get("WEAVEC_GOLDEN_DIR"),
                        help="golden v0.10.0 binaries (default: $WEAVEC_GOLDEN_DIR)")
    parser.add_argument("--filter", action="append", default=[], metavar="GLOB",
                        help="select cases by path under test/cases, e.g. 'soundness/**' (repeatable)")
    parser.add_argument("--json", type=Path, metavar="OUT", help="write the results as JSON")
    parser.add_argument("--timeout", type=float, default=COMPILE_TIMEOUT, help="seconds per compile or link")
    parser.add_argument("--run-timeout", type=float, default=RUN_TIMEOUT, help="seconds per run")
    parser.add_argument("--keep", action="store_true", help="keep each case's build directory")
    parser.add_argument("--lldb", action="store_true",
                        help="with --checks verify, ask lldb for the category of a trap that the "
                             "report-mode run also attributes to an unproven check")
    parser.add_argument("--verbose", "-v", action="store_true", help="print notes too")
    args = parser.parse_args(argv)

    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.legacy and args.compare_golden:
        parser.error("--compare-golden already runs both binaries in legacy mode")
    if (args.legacy or args.compare_golden) and (args.asan or args.no_emission or args.require
                                                 or args.checks != "trap"):
        parser.error("--legacy and --compare-golden take no build, run or ledger options")
    golden = args.golden_dir.resolve() if args.golden_dir else None
    build_dir = (args.build_dir or default_build_dir()).resolve()
    weavec = args.weavec or build_dir / "bin" / "weavec"
    weavec_cc = args.weavec_cc or build_dir / "bin" / "weavec-cc"
    if args.legacy:
        if golden is None and args.weavec is None:
            parser.error("--legacy needs WEAVEC_GOLDEN_DIR (or --golden-dir, or --weavec)")
        weavec = args.weavec or golden / "weavec"
    elif args.compare_golden and golden is None:
        parser.error("--compare-golden needs WEAVEC_GOLDEN_DIR (or --golden-dir)")

    try:
        cases, _ = discover(args.cases)
    except OSError as error:
        print(f"error: cannot read {args.cases}: {error}", file=sys.stderr)
        return 2
    selected = select(cases, args.filter)
    if not selected:
        print("error: no case matches " + " ".join(args.filter or ["(no filter)"]), file=sys.stderr)
        return 2
    if args.legacy:
        needed = [weavec]
    elif args.compare_golden:
        needed = [weavec, golden / "weavec"]
    else:
        needed = [weavec_cc]
        if any(c.tool or (not c.has_main and len(c.analysed_units()) > 1) for c in selected):
            needed.append(weavec)
    for binary in needed:
        if not Path(binary).is_file():
            print(f"error: {binary} does not exist; build it or pass --weavec/--weavec-cc", file=sys.stderr)
            return 2

    cfg = Config(weavec=Path(weavec).resolve(), weavec_cc=Path(weavec_cc).resolve(),
                 legacy=args.legacy, checks=args.checks,
                 require=args.require if args.require and args.require != "none" else None,
                 asan=args.asan, no_emission=args.no_emission, no_run=args.no_run,
                 compile_timeout=args.timeout, run_timeout=args.run_timeout, keep=args.keep,
                 lldb=args.lldb)
    scratch = Path(tempfile.mkdtemp(prefix="weavec-cases-"))
    cfg.work = scratch
    started = time.perf_counter()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            if args.compare_golden:
                futures = [pool.submit(compare_case, case, cfg.weavec, golden / "weavec", cfg, scratch)
                           for case in selected]
            else:
                futures = [pool.submit(run_case, case, cfg) for case in selected]
            results = []
            for future in futures:
                result = future.result()
                results.append(result)
                if args.compare_golden:
                    label = {"pass": "SAME", "fail": "DIFF", "error": "ERROR", "skip": "SKIP"}[result["status"]]
                    print(f"{label:5} {result['case']}", flush=True)
                    for line in result["failures"][:20]:
                        print(f"      {line}")
                    if args.verbose or result["status"] == "skip":
                        for note in result["notes"]:
                            print(f"      note: {note}")
                elif result["status"] != "pass" or args.verbose:
                    print_result(result, args.verbose)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    summary = summarize(results)
    print_summary(summary, args.legacy or args.compare_golden, args.compare_golden)
    if args.compare_golden:
        excluded = [r["case"] for r in results if r["status"] == "skip"]
        if excluded:
            print(f"excluded from the comparison: {len(excluded)} (flags removed from the binary under test)")
    print(f"({len(results)} cases in {time.perf_counter() - started:.1f} s)")
    if args.json:
        document = {
            "schema": "weavec-cases", "version": 1,
            "mode": {"legacy": args.legacy, "compareGolden": args.compare_golden, "checks": args.checks,
                     "require": args.require or "none", "asan": args.asan,
                     "noEmission": args.no_emission, "noRun": args.no_run},
            "binaries": {"weavec": str(cfg.weavec), "weavec-cc": str(cfg.weavec_cc),
                         "golden": str(golden) if golden else None},
            "filters": args.filter, "summary": summary,
            "failures": [{"case": r["case"], "failures": r["failures"]} for r in results
                         if r["status"] in ("fail", "error")],
            "cases": results,
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(document, indent=2) + "\n")
    return 0 if all(r["status"] in ("pass", "skip") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
