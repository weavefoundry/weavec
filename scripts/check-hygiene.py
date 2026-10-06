#!/usr/bin/env python3
"""Check repository hygiene: gate H2 of RFC 0030 (docs/rfcs/0030-prove-or-trap.md)
as RFC 0031 (docs/rfcs/0031-object-engine.md, section 2) moves it to the object engine.

Seven checks, one per bullet of H2 (its fourth bullet is split in two):

  retired-name       0 occurrences of SafetyState, CheckedContract,
                     checkContracts or --checked, or of the old engine's
                     FunctionDataflow, DataflowEngine, TranslationUnitAnalyzer,
                     FunctionAnalyzer, SummaryStore or FunctionSummary, in
                     lib/, tools/ and docs/ outside docs/rfcs/ (superseded
                     RFCs keep their text). Plain case-sensitive substrings:
                     --checked-report counts.
  library-name-test  0 `== "NAME"` or `"NAME" ==` comparisons (any whitespace
                     around ==) in the C and C++ sources (.h .hpp .cpp .cc .c
                     .def .inc) of lib/, include/ and tools/ outside
                     lib/Core/LibrarySpec*, where NAME is a name that
                     lib/Core/LibrarySpec.txt answers to: an entry, a chk()
                     alias (a fortified form), or the __builtin_ spelling of
                     either. Comparisons inside comments do not count.
  corpus-word        0 occurrences of the words cJSON, jansson, linenoise,
                     jsmn, zlib, lua, Lua, sds and minigzip, and of RFC 0033's
                     fresh and sealed projects zstd, libuv, oniguruma, redis,
                     expat, pcre2, libevent, libsodium, libxml2, libpng,
                     mbedtls, msgpack and yyjson, in any file under lib/,
                     comments included (the word rule is below).
  engine-include     only lib/Analysis/Engine*.cpp and
                     lib/Analysis/ObjectEngine.cpp include Engine.h (the
                     engine's private header): no other file under lib/ or
                     include/ includes a header whose last path component is
                     Engine.h. Nor does a file of a new component (RFC 0030
                     section 1: every file under lib/ or include/ whose name
                     starts with SiteCollector, AttributeReader,
                     KindInference, SlotCollector, BoundaryInvariants,
                     CheckPlanner or LedgerAdapter) include it through the
                     project headers it includes. The include chain is
                     reported.
  engine-sink        the engine receives no DiagnosticSink (it publishes only
                     through LedgerAdapter): no identifier containing
                     DiagnosticSink (ClangDiagnosticSink too) in
                     lib/Analysis/Engine*.{h,cpp}, lib/Analysis/ObjectEngine.cpp
                     or include/weavec/Analysis/ObjectEngine.h, comments and
                     string literals ignored. A missing lib/Analysis/Engine.h
                     is a violation.
  engine-lines       lib/Analysis/Engine*.h and lib/Analysis/Engine*.cpp
                     (EngineIntegers.h included) total at most 22,000 lines.
  library-lines      the code under lib/, include/ and tools/ totals at most
                     70,000 lines. lib/Core/LibrarySpec.txt does not count:
                     it is a declarative table, and its growth is coverage,
                     not sprawl. Both limits are ratchets (RFC 0030 gate H2):
                     they are lowered as code is deleted, and raised only by
                     an RFC amendment that records the measurement. RFC 0031
                     stage S7 recorded them: the measurement at the end of S7
                     (19,654 and 64,367 lines), rounded up to a multiple of
                     500. RFC 0032 (gate H1, Implementation amendments)
                     raised the library limit to 69,000 for the planner's
                     guards, the range caches, object registration and the
                     driver's runtime handling (68,744 lines measured).
                     RFC 0033 (gate H1, Implementation amendments) raised
                     them to 21,500 and 70,000 for the witness rule, the
                     length guards, the link modes and the unit budget
                     (21,078 and 69,980 lines measured). RFC 0034 (gate
                     H1, Implementation amendments) raised them to 22,000
                     and 72,000 for the shadow guards' passes, confirmation
                     by replay and the work and memory budgets.
  runtime-lines      the C runtime (runtime/, its tests under runtime/test/
                     excluded) totals at most 4,000 lines (RFC 0032 gate H1).
                     It is counted apart from the libraries: it is linked
                     into compiled programs, not into the compiler.

The word rule of corpus-word: an occurrence counts when it is not immediately
preceded or followed by an ASCII letter or digit, and case matters.
Underscores and other punctuation are boundaries, so cJSON_IsString, lua_State
and <zlib.h> are violations, while evaluation, sdsnew, LUA and Zlib are not.

engine-include ignores commented-out includes; it resolves an include against
the including file's directory, lib/Analysis/ and include/, follows only files
under lib/ and include/, and counts every include whose last component is
Engine.h, resolved or not (SafetyEngine.h and ObjectEngine.h are other
headers).

Files come from `git ls-files --cached --others --exclude-standard`, so ignored
build output (docs/node_modules, docs/dist, docs/.astro, ...) is never read
while new untracked files are; files deleted in the working tree are skipped.
When the root is not the top of a git work tree (a source archive), the
directories are walked instead, skipping node_modules, dist, .generated,
.astro, test-results, playwright-report, __pycache__, .git and build*.
Symbolic links are skipped, and so are binary files (a NUL byte in the first
8 KiB), by every check, the line counts included. Text is read as UTF-8 with
invalid bytes replaced. Lines are counted as `wc -l` counts them, plus one
for a last line that has no trailing newline.

Exit status: 0 when every check passes, 1 when any fails, 2 on a usage error.

Examples:

  scripts/check-hygiene.py
  scripts/check-hygiene.py --root ~/src/weavec-0.11.0 --json hygiene.json
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import fnmatch
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Container, Iterator, Optional

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Gate H2 (RFC 0030, Acceptance gates, Hygiene; RFC 0031, section 2)
# ---------------------------------------------------------------------------

# Recorded by RFC 0031 stage S7 and RFC 0032, 0033 and 0034 gate H1 (see the
# module docstring).
ENGINE_LINE_LIMIT = 22_000
LIBRARY_LINE_LIMIT = 72_000
RUNTIME_LINE_LIMIT = 4_000

# retired-name
RETIRED_CHECKED_NAMES = ("SafetyState", "CheckedContract", "checkContracts", "--checked")
# The old engine's (RFC 0031 section 1).
RETIRED_ENGINE_NAMES = ("FunctionDataflow", "DataflowEngine", "TranslationUnitAnalyzer",
                        "FunctionAnalyzer", "SummaryStore", "FunctionSummary")
RETIRED_NAMES = RETIRED_CHECKED_NAMES + RETIRED_ENGINE_NAMES
RETIRED_NAME_DIRS = ("lib", "tools", "docs")
RETIRED_NAME_EXEMPT = ("docs/rfcs/",)
# The pages that still describe the old engine until the rewrite that RFC
# 0031 section 10 asks for lands. Remove each one as it is rewritten.
# library-name-test (section 8)
LIBRARY_SPEC = "lib/Core/LibrarySpec.txt"
LIBRARY_SPEC_PREFIX = "lib/Core/LibrarySpec"
LIBRARY_DIRECTIVES = ("header", "builtins")
BUILTIN_PREFIX = "__builtin_"
SOURCE_DIRS = ("lib", "include", "tools")
SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".c", ".def", ".inc")
# corpus-word (the corpus projects of section 17.5, and RFC 0033 section 11's
# fresh and sealed projects)
CORPUS_WORDS = ("cJSON", "jansson", "linenoise", "jsmn", "zlib", "lua", "Lua", "sds", "minigzip",
                "zstd", "libuv", "oniguruma", "redis", "expat", "pcre2", "libevent", "libsodium",
                "libxml2", "libpng", "mbedtls", "msgpack", "yyjson",
                # RFC 0034, section 9.
                "quickjs", "QuickJS", "lmdb", "LMDB", "janet", "Janet", "brotli", "xz",
                "libdeflate", "zlib-ng", "curl", "cmark", "libgit2", "libjpeg", "opus",
                "flac", "giflib", "wren")
CORPUS_WORD_DIRS = ("lib",)
# engine-include (RFC 0030 sections 1 and 14, RFC 0031 section 2)
NEW_COMPONENTS = ("SiteCollector", "AttributeReader", "KindInference", "SlotCollector",
                  "BoundaryInvariants", "CheckPlanner", "LedgerAdapter")
COMPONENT_DIRS = ("lib", "include")
ENGINE_HEADER = "Engine.h"
ENGINE_DIR = "lib/Analysis"
ENGINE_INCLUDERS = ("Engine*.cpp", "ObjectEngine.cpp")  # in ENGINE_DIR
# Searched after the including file's directory.
INCLUDE_PATH = ("lib/Analysis", "include")
# engine-sink (RFC 0030 section 14)
ENGINE_PRIVATE_HEADER = "lib/Analysis/Engine.h"
ENGINE_SINK_PATTERNS = ("Engine*.h", "Engine*.cpp", "ObjectEngine.cpp")  # in ENGINE_DIR
ENGINE_PUBLIC_HEADER = "include/weavec/Analysis/ObjectEngine.h"
DIAGNOSTIC_SINK = "DiagnosticSink"
# engine-lines and library-lines (RFC 0030 section 18, The line budget)
ENGINE_LINE_PATTERNS = ("Engine*.h", "Engine*.cpp")  # in ENGINE_DIR
LIBRARY_DIRS = ("lib", "include", "tools")
# What a total's violation line shows in place of a path.
ENGINE_TOTAL = "lib/Analysis/Engine*.{h,cpp}"
LIBRARY_TOTAL = "{lib,include,tools}/**"
RUNTIME_DIRS = ("runtime",)
RUNTIME_TESTS = "runtime/test/"
RUNTIME_TOTAL = "runtime/**"

CHECKS = (
    ("retired-name", "checked-mode or old-engine names outside docs/rfcs/"),
    ("library-name-test", '== "<name>" against a LibrarySpec name outside lib/Core/LibrarySpec*'),
    ("corpus-word", "a corpus project's name (cJSON, jansson, ..., zstd, redis, ...) in lib/"),
    ("engine-include", "Engine.h included outside Engine*.cpp and ObjectEngine.cpp"),
    ("engine-sink", "the engine naming a DiagnosticSink"),
    ("engine-lines", "lib/Analysis/Engine*.{h,cpp} over their line limit"),
    ("library-lines", "lib/, include/ and tools/ over their line limit"),
    ("runtime-lines", "runtime/ (without runtime/test/) over its line limit"),
)

# ---------------------------------------------------------------------------
# File selection
# ---------------------------------------------------------------------------

SCANNED_DIRS = ("lib", "include", "tools", "docs", "runtime")
WALK_SKIPPED_DIRS = frozenset(("node_modules", "dist", ".generated", ".astro", "test-results",
                               "playwright-report", "__pycache__", ".git"))
WALK_SKIPPED_PREFIX = "build"
BINARY_PROBE = 8192


@dataclasses.dataclass(frozen=True)
class Violation:
    check: str
    path: str
    line: Optional[int]
    message: str

    def render(self) -> str:
        where = self.path if self.line is None else f"{self.path}:{self.line}"
        return f"{where}: {self.check}: {self.message}"


@dataclasses.dataclass
class Result:
    root: Path
    file_source: str = "walk"
    files: int = 0
    binary: list[str] = dataclasses.field(default_factory=list)
    unreadable: list[str] = dataclasses.field(default_factory=list)
    library_entries: Optional[int] = None
    library_aliases: Optional[int] = None
    engine_lines: dict[str, int] = dataclasses.field(default_factory=dict)
    library_lines: dict[str, int] = dataclasses.field(default_factory=dict)
    runtime_lines: dict[str, int] = dataclasses.field(default_factory=dict)
    violations: list[Violation] = dataclasses.field(default_factory=list)

    def add(self, check: str, path: str, line: Optional[int], message: str) -> None:
        self.violations.append(Violation(check, path, line, message))

    def count(self, check: str) -> int:
        return sum(1 for violation in self.violations if violation.check == check)


def count_lines(data: bytes) -> int:
    """`wc -l`: the number of newline characters. Unlike `wc -l`, a last line
    without a trailing newline is counted as a line too."""
    lines = data.count(b"\n")
    if data and not data.endswith(b"\n"):
        lines += 1
    return lines


def git_files(root: Path, dirs: tuple[str, ...]) -> Optional[list[str]]:
    """The files under `dirs` that git tracks or would track (untracked but not
    ignored), or None when git is missing or `root` is not the top of a work
    tree."""
    git = shutil.which("git")
    if git is None:
        return None
    try:
        top = subprocess.run([git, "rev-parse", "--show-toplevel"], cwd=root,
                             capture_output=True, text=True, check=False)
        if top.returncode != 0 or not top.stdout.strip() or Path(top.stdout.strip()).resolve() != root:
            return None
        listed = subprocess.run([git, "ls-files", "-z", "--cached", "--others", "--exclude-standard",
                                 "--", *dirs], cwd=root, capture_output=True, check=False)
    except OSError:
        return None
    if listed.returncode != 0:
        return None
    return sorted({os.fsdecode(path) for path in listed.stdout.split(b"\0") if path})


def walk_files(root: Path, dirs: tuple[str, ...]) -> list[str]:
    """The files under `dirs`, without the usual build and tool output."""
    found = []
    for top in dirs:
        for current, subdirs, names in os.walk(root / top):
            subdirs[:] = [d for d in subdirs
                          if d not in WALK_SKIPPED_DIRS and not d.startswith(WALK_SKIPPED_PREFIX)]
            found.extend(Path(current, name).relative_to(root).as_posix() for name in names)
    return sorted(found)


def list_files(root: Path, dirs: tuple[str, ...]) -> tuple[str, list[str]]:
    """("git" or "walk", the regular files under `dirs` relative to `root`)."""
    listed = git_files(root, dirs)
    source = "walk" if listed is None else "git"
    if listed is None:
        listed = walk_files(root, dirs)
    # A path git lists may be deleted in the working tree, a symlink or a
    # submodule.
    return source, [path for path in listed
                    if not (root / path).is_symlink() and (root / path).is_file()]


def under(path: str, dirs: tuple[str, ...]) -> bool:
    return any(path.startswith(directory.rstrip("/") + "/") for directory in dirs)


class SourceFile:
    """One text file of the tree, with its C lexing computed on demand."""

    def __init__(self, path: str, data: bytes) -> None:
        self.path = path
        self.data = data
        self._text: Optional[str] = None
        self._lexed: Optional[CText] = None

    @property
    def text(self) -> str:
        if self._text is None:
            self._text = self.data.decode("utf-8", errors="replace")
        return self._text

    @property
    def lexed(self) -> CText:
        if self._lexed is None:
            self._lexed = lex_c(self.text)
        return self._lexed

    @property
    def lines(self) -> int:
        return count_lines(self.data)


class Tree:
    """The files of the scanned directories, read once and on demand."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.source, self.paths = list_files(root, SCANNED_DIRS)
        self._listed = frozenset(self.paths)
        self._files: dict[str, Optional[SourceFile]] = {}
        self.binary: list[str] = []
        self.unreadable: list[str] = []

    def get(self, path: str) -> Optional[SourceFile]:
        """The text file at `path`, or None when it is not listed, binary or
        unreadable."""
        if path not in self._files:
            self._files[path] = self._read(path) if path in self._listed else None
        return self._files[path]

    def _read(self, path: str) -> Optional[SourceFile]:
        try:
            data = (self.root / path).read_bytes()
        except OSError:  # removed since it was listed, for instance
            self.unreadable.append(path)
            return None
        if b"\0" in data[:BINARY_PROBE]:
            self.binary.append(path)
            return None
        return SourceFile(path, data)

    def files(self, dirs: tuple[str, ...], exclude: tuple[str, ...] = ()) -> Iterator[SourceFile]:
        """The text files under `dirs` whose paths start with none of `exclude`."""
        for path in self.paths:
            if under(path, dirs) and not path.startswith(exclude):
                file = self.get(path)
                if file is not None:
                    yield file


# ---------------------------------------------------------------------------
# C and C++ text
# ---------------------------------------------------------------------------

# Comments, raw strings, pp-numbers, string and character literals. Numbers
# are matched only so that a digit separator (1'000) does not open a
# character literal. A literal ends at an unescaped newline when unclosed.
_C_TOKEN = re.compile(
    r"""
    (?P<comment> //(?:[^\\\n]|\\.)* | /\*.*?(?:\*/|\Z) )
  | (?<![\w$]) (?P<raw> (?:u8|[uUL])?R" (?P<delim>[^()\\\s"]{0,16}) \(
                        .*? (?:(?P<rclose>\)(?P=delim)")|\Z) )
  | (?<![\w$]) (?P<number> \.?\d(?:[eEpP][+-]|[\w.]|'(?=\w))* )
  | (?P<string> "(?:[^"\\\n]|\\.)*(?P<sclose>")? )
  | (?P<char> '(?:[^'\\\n]|\\.)*(?P<cclose>')? )
    """,
    re.S | re.X,
)
_NOT_NEWLINE = re.compile(r"[^\n]")
_INCLUDE = re.compile(r'^[ \t]*#[ \t]*(?:include|include_next|import)[ \t]*(?:<([^>\n]*)>|"([^"\n]*)")',
                      re.M)
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_WHITESPACE = " \t\r\n\f\v"


@dataclasses.dataclass
class CText:
    """C or C++ source with the same offsets and lines as the original."""

    code: str  # comments blanked out
    bare: str  # comments and the insides of string and character literals blanked out
    strings: list[tuple[int, int, str]]  # (start, end, body) of each closed "..." literal


def _blank(text: str) -> str:
    return _NOT_NEWLINE.sub(" ", text)


def lex_c(text: str) -> CText:
    code: list[str] = []
    bare: list[str] = []
    strings: list[tuple[int, int, str]] = []
    last = 0
    for match in _C_TOKEN.finditer(text):
        if match.group("number") is not None:
            continue
        start, end = match.span()
        code.append(text[last:start])
        bare.append(text[last:start])
        token = match.group()
        if match.group("comment") is not None:
            code.append(_blank(token))
            bare.append(_blank(token))
        else:
            closed = any(match.group(name) is not None for name in ("rclose", "sclose", "cclose"))
            opening = token.index('"') if match.group("raw") is not None else 0
            closing = len(token) - 1 if closed else len(token)
            code.append(token)
            bare.append(token[:opening + 1] + _blank(token[opening + 1:closing]) + token[closing:])
            if match.group("string") is not None and closed:
                strings.append((start, end, token[1:-1]))
        last = end
    code.append(text[last:])
    bare.append(text[last:])
    return CText("".join(code), "".join(bare), strings)


def line_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def include_directives(code: str) -> list[tuple[int, str, str]]:
    """(line, spelling with its delimiters, path) of each include in `code`,
    whose comments are blanked out."""
    found = []
    for match in _INCLUDE.finditer(code):
        angled = match.group(1) is not None
        path = (match.group(1) if angled else match.group(2)).strip()
        spelling = f"<{path}>" if angled else f'"{path}"'
        found.append((line_at(code, match.start()), spelling, path))
    return found


def resolve_include(including: str, path: str, known: Container[str]) -> Optional[str]:
    """The file among `known` that `#include "path"` in `including` names:
    relative to the including file's directory, then to INCLUDE_PATH."""
    for base in (posixpath.dirname(including), *INCLUDE_PATH):
        candidate = posixpath.normpath(posixpath.join(base, path))
        if candidate in known:
            return candidate
    return None


def identifiers_containing(text: str, word: str) -> list[tuple[int, str]]:
    """(offset, identifier) of each identifier in `text` that contains `word`."""
    pattern = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z0-9_]*" + re.escape(word) + r"[A-Za-z0-9_]*")
    return [(match.start(), match.group()) for match in pattern.finditer(text)]


# ---------------------------------------------------------------------------
# LibrarySpec names (the grammar is in lib/Core/LibrarySpec.txt's header)
# ---------------------------------------------------------------------------

_SPEC_WORD = re.compile(r"[A-Za-z0-9_-]*")
_SPEC_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_SPEC_ALIAS = re.compile(r"(?<![A-Za-z0-9_-])chk\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*:")


@dataclasses.dataclass
class LibraryNames:
    names: dict[str, str]  # name -> what it is, for the messages
    entries: int
    aliases: int


def library_spec_lines(text: str) -> list[tuple[int, str]]:
    """(first physical line, text) of each logical line of LibrarySpec.txt:
    '#' starts a comment, and a line whose last character before any comment
    is a backslash continues on the next line."""
    logical = []
    parts: list[str] = []
    first = 0
    for number, physical in enumerate(text.split("\n"), 1):
        physical = physical.split("#", 1)[0].rstrip(" \t\r")
        if not parts:
            first = number
        continues = physical.endswith("\\")
        parts.append(physical[:-1] if continues else physical)
        if not continues:
            logical.append((first, " ".join(parts)))
            parts = []
    if parts:
        logical.append((first, " ".join(parts)))
    return logical


def library_names(text: str) -> LibraryNames:
    """The names LibrarySpec.txt answers to: each entry, `NAME '(' params ')'
    '->' result {clause} ';'` (the directives `header NAME;` and `builtins;`
    are not entries); each alias that an entry's chk(ALIAS: ...) clauses give
    it (its fortified forms); and, since every entry also answers to
    __builtin_NAME, the __builtin_ spelling of each of those."""
    entries: dict[str, None] = {}
    aliases: dict[str, str] = {}
    for _, line in library_spec_lines(text):
        line = line.strip()
        word = _SPEC_WORD.match(line).group()
        rest = line[len(word):].lstrip()
        if word in LIBRARY_DIRECTIVES or not _SPEC_NAME.fullmatch(word) or not rest.startswith("("):
            continue
        entries.setdefault(word)
        for alias in _SPEC_ALIAS.findall(rest):
            aliases.setdefault(alias, word)
    names = {entry: "entry" for entry in entries}
    for alias, entry in aliases.items():
        names.setdefault(alias, f"chk alias (of {entry})")
    for name, what in list(names.items()):
        of = name if what == "entry" else f"{name}, a chk alias of {aliases[name]}"
        names.setdefault(BUILTIN_PREFIX + name, f"{BUILTIN_PREFIX} spelling (of {of})")
    return LibraryNames(names, len(entries), len(aliases))


# ---------------------------------------------------------------------------
# Checks 1 to 3: text
# ---------------------------------------------------------------------------


def find_all(pattern: re.Pattern, text: str) -> Iterator[tuple[int, int, str]]:
    """(line, column, text) of each match of `pattern` within a line of `text`."""
    if not pattern.search(text):
        return
    for number, line in enumerate(text.split("\n"), 1):
        for match in pattern.finditer(line):
            yield number, match.start() + 1, match.group()


def check_retired_names(tree: Tree, result: Result) -> None:
    pattern = re.compile("|".join(re.escape(name) for name in RETIRED_NAMES))
    for file in tree.files(RETIRED_NAME_DIRS, exclude=RETIRED_NAME_EXEMPT):
        for number, column, name in find_all(pattern, file.text):
            result.add("retired-name", file.path, number, f"'{name}' at column {column}")


def _is_word(character: str) -> bool:
    return character.isalnum() or character == "_"


def _equality_before(code: str, start: int) -> bool:
    """Whether `==` comes before the string literal at `start`, skipping an
    encoding prefix (u8, u, U, L) and whitespace."""
    index = start
    for prefix in ("u8", "u", "U", "L"):
        begin = index - len(prefix)
        if begin >= 0 and code.startswith(prefix, begin) and (begin == 0 or not _is_word(code[begin - 1])):
            index = begin
            break
    while index > 0 and code[index - 1] in _WHITESPACE:
        index -= 1
    return index >= 2 and code[index - 2:index] == "=="


def _equality_after(code: str, end: int) -> bool:
    """Whether `==` comes after the string literal that ends at `end`, skipping
    a literal suffix (sv, s) and whitespace."""
    suffix = _IDENTIFIER.match(code, end)
    index = suffix.end() if suffix else end
    while index < len(code) and code[index] in _WHITESPACE:
        index += 1
    return code.startswith("==", index)


def check_library_name_tests(tree: Tree, result: Result) -> None:
    spec = tree.get(LIBRARY_SPEC)
    if spec is None:
        result.add("library-name-test", LIBRARY_SPEC, None,
                   "missing or unreadable, so the LibrarySpec names are unknown")
        return
    library = library_names(spec.text)
    result.library_entries, result.library_aliases = library.entries, library.aliases
    if not library.names:
        result.add("library-name-test", LIBRARY_SPEC, None,
                   "no entry found, so the LibrarySpec names are unknown")
        return
    for file in tree.files(SOURCE_DIRS):
        if file.path.startswith(LIBRARY_SPEC_PREFIX) or not file.path.endswith(SOURCE_SUFFIXES):
            continue
        code = file.lexed.code
        for start, end, body in file.lexed.strings:
            what = library.names.get(body)
            if what is None:
                continue
            before, after = _equality_before(code, start), _equality_after(code, end)
            if before or after:
                shown = ("== " if before else "") + f'"{body}"' + (" ==" if after else "")
                result.add("library-name-test", file.path, line_at(code, start),
                           f"{shown} compares against a LibrarySpec {what}")


def corpus_word_pattern() -> re.Pattern:
    """A corpus word not preceded or followed by an ASCII letter or digit."""
    words = "|".join(re.escape(word) for word in CORPUS_WORDS)
    return re.compile(rf"(?<![A-Za-z0-9])(?:{words})(?![A-Za-z0-9])")


def check_corpus_words(tree: Tree, result: Result) -> None:
    pattern = corpus_word_pattern()
    for file in tree.files(CORPUS_WORD_DIRS):
        for number, column, word in find_all(pattern, file.text):
            result.add("corpus-word", file.path, number, f"word '{word}' at column {column}")


# ---------------------------------------------------------------------------
# Checks 4 and 5: the engine seam (RFC 0030 section 14, "Two rules keep the
# seam honest"; RFC 0031 section 2)
# ---------------------------------------------------------------------------


def _in_engine_dir(path: str, patterns: tuple[str, ...]) -> bool:
    """Whether `path` is directly in ENGINE_DIR and its name matches one of
    `patterns`."""
    name = posixpath.basename(path)
    return (posixpath.dirname(path) == ENGINE_DIR
            and any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns))


def _shortest_chain(start: str, includes: Callable[[str], list[tuple[int, str, str]]],
                    resolve: Callable[[str, str], Optional[str]]) -> Optional[list[str]]:
    """The shortest include chain from `start` to Engine.h: `path:line` for
    each include followed, then Engine.h as resolved (or as spelled)."""
    parents: dict[str, Optional[tuple[str, int]]] = {start: None}
    queue = collections.deque([start])
    while queue:
        node = queue.popleft()
        for line, spelling, included in includes(node):
            target = resolve(node, included)
            if posixpath.basename(included) == ENGINE_HEADER:
                hops = [f"{node}:{line}", target or spelling]
                while parents[node] is not None:
                    node, parent_line = parents[node]
                    hops.insert(0, f"{node}:{parent_line}")
                return hops
            if target is not None and target not in parents:
                parents[target] = (node, line)
                queue.append(target)
    return None


def check_engine_includes(tree: Tree, result: Result) -> None:
    files = {file.path: file for file in tree.files(COMPONENT_DIRS)}
    directives: dict[str, list[tuple[int, str, str]]] = {}
    chains: dict[str, Optional[list[str]]] = {}

    def includes(path: str) -> list[tuple[int, str, str]]:
        if path not in directives:
            directives[path] = include_directives(files[path].lexed.code)
        return directives[path]

    def resolve(including: str, path: str) -> Optional[str]:
        return resolve_include(including, path, files)

    for path in files:
        if _in_engine_dir(path, ENGINE_INCLUDERS):
            continue  # no new component's name starts with Engine
        component = posixpath.basename(path).startswith(NEW_COMPONENTS)
        for line, spelling, included in includes(path):
            target = resolve(path, included)
            if posixpath.basename(included) == ENGINE_HEADER:
                result.add("engine-include", path, line,
                           f"includes Engine.h directly: {path}:{line} -> {target or spelling}")
                continue
            if not component or target is None:
                continue
            if target not in chains:
                chains[target] = _shortest_chain(target, includes, resolve)
            if chains[target] is not None:
                chain = " -> ".join([f"{path}:{line}", *chains[target]])
                result.add("engine-include", path, line, f"includes Engine.h transitively: {chain}")


def check_engine_sink(tree: Tree, result: Result) -> None:
    if tree.get(ENGINE_PRIVATE_HEADER) is None:
        result.add("engine-sink", ENGINE_PRIVATE_HEADER, None,
                   "missing or unreadable, so the engine cannot be checked")
    for file in tree.files(COMPONENT_DIRS):
        if not (_in_engine_dir(file.path, ENGINE_SINK_PATTERNS) or file.path == ENGINE_PUBLIC_HEADER):
            continue
        bare = file.lexed.bare
        for offset, name in identifiers_containing(bare, DIAGNOSTIC_SINK):
            result.add("engine-sink", file.path, line_at(bare, offset), f"the engine names {name}")


# ---------------------------------------------------------------------------
# Checks 6 and 7: line budgets
# ---------------------------------------------------------------------------


def _over_limit(total: int, limit: int, files: int) -> str:
    return f"{total:,} lines in {files:,} files, {total - limit:,} over the limit of {limit:,}"


def check_engine_lines(tree: Tree, result: Result) -> None:
    for file in tree.files((ENGINE_DIR,)):
        if _in_engine_dir(file.path, ENGINE_LINE_PATTERNS):
            result.engine_lines[file.path] = file.lines
    total = sum(result.engine_lines.values())
    if total > ENGINE_LINE_LIMIT:
        result.add("engine-lines", ENGINE_TOTAL, None,
                   _over_limit(total, ENGINE_LINE_LIMIT, len(result.engine_lines)))


def check_library_lines(tree: Tree, result: Result) -> None:
    for file in tree.files(LIBRARY_DIRS):
        if file.path == LIBRARY_SPEC:
            continue
        result.library_lines[file.path] = file.lines
    total = sum(result.library_lines.values())
    if total > LIBRARY_LINE_LIMIT:
        result.add("library-lines", LIBRARY_TOTAL, None,
                   _over_limit(total, LIBRARY_LINE_LIMIT, len(result.library_lines)))


def check_runtime_lines(tree: Tree, result: Result) -> None:
    for file in tree.files(RUNTIME_DIRS, exclude=(RUNTIME_TESTS,)):
        result.runtime_lines[file.path] = file.lines
    total = sum(result.runtime_lines.values())
    if total > RUNTIME_LINE_LIMIT:
        result.add("runtime-lines", RUNTIME_TOTAL, None,
                   _over_limit(total, RUNTIME_LINE_LIMIT, len(result.runtime_lines)))


CHECK_FUNCTIONS = (check_retired_names, check_library_name_tests, check_corpus_words,
                   check_engine_includes, check_engine_sink, check_engine_lines,
                   check_library_lines, check_runtime_lines)


# ---------------------------------------------------------------------------
# Running and reporting
# ---------------------------------------------------------------------------


def run_checks(root: Path) -> Result:
    root = root.resolve()
    tree = Tree(root)
    result = Result(root=root, file_source=tree.source, files=len(tree.paths))
    for check in CHECK_FUNCTIONS:
        check(tree, result)
    result.binary = sorted(tree.binary)
    result.unreadable = sorted(tree.unreadable)
    return result


def summary_lines(result: Result) -> list[str]:
    source = "git ls-files" if result.file_source == "git" else "a directory walk (not a git work tree)"
    engine = sum(result.engine_lines.values())
    library = sum(result.library_lines.values())
    runtime = sum(result.runtime_lines.values())
    lines = [
        "Summary (RFC 0030 and 0031, gate H2; RFC 0032, gate H1)",
        f"  files:          {result.files:,} from {source}; skipped {len(result.binary)} binary, "
        f"{len(result.unreadable)} unreadable",
    ]
    if result.library_entries is not None:
        lines.append(f"  LibrarySpec:    {result.library_entries:,} entries, {result.library_aliases:,} chk "
                     f"aliases, and their {BUILTIN_PREFIX} spellings")
    lines.append(f"  engine lines:   {engine:,} / {ENGINE_LINE_LIMIT:,} "
                 f"({len(result.engine_lines):,} {ENGINE_DIR}/Engine*.{{h,cpp}} files)")
    lines.append(f"  library lines:  {library:,} / {LIBRARY_LINE_LIMIT:,} "
                 f"({len(result.library_lines):,} code files under lib/, include/ and tools/, "
                 f"{LIBRARY_SPEC} excluded)")
    lines.append(f"  runtime lines:  {runtime:,} / {RUNTIME_LINE_LIMIT:,} "
                 f"({len(result.runtime_lines):,} files under runtime/, {RUNTIME_TESTS} excluded)")
    width = max(len(check) for check, _ in CHECKS)
    for check, description in CHECKS:
        lines.append(f"  {check:<{width}} {result.count(check):>6}  {description}")
    total = len(result.violations)
    verdict = "PASS" if total == 0 else "FAIL"
    lines.append(f"H2: {verdict} ({total:,} violation{'' if total == 1 else 's'})")
    return lines


def document(result: Result) -> dict:
    """The results as JSON."""
    return {
        "schema": "weavec-hygiene",
        "version": 2,
        "root": str(result.root),
        "passed": not result.violations,
        "files": {"source": result.file_source, "count": result.files,
                  "binarySkipped": result.binary, "unreadable": result.unreadable},
        "librarySpec": {"entries": result.library_entries, "aliases": result.library_aliases},
        "lines": {
            "engine": {"total": sum(result.engine_lines.values()), "limit": ENGINE_LINE_LIMIT,
                       "files": result.engine_lines},
            "library": {"total": sum(result.library_lines.values()), "limit": LIBRARY_LINE_LIMIT,
                        "files": len(result.library_lines)},
            "runtime": {"total": sum(result.runtime_lines.values()), "limit": RUNTIME_LINE_LIMIT,
                        "files": result.runtime_lines},
        },
        "checks": {check: {"description": description, "violations": result.count(check)}
                   for check, description in CHECKS},
        "violations": [dataclasses.asdict(violation) for violation in result.violations],
    }


def main(argv: Optional[list[str]] = None) -> int:
    summary, details = __doc__.split("\n\n", 1)
    parser = argparse.ArgumentParser(description=summary,
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=details)
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="the WeaveC source tree to check (default: the one holding this script)")
    parser.add_argument("--json", type=Path, metavar="OUT", help="also write the results as JSON")
    args = parser.parse_args(argv)

    if not args.root.is_dir():
        print(f"error: {args.root} is not a directory", file=sys.stderr)
        return 2
    if not (args.root / "lib").is_dir():
        print(f"error: {args.root} has no lib/ directory; pass the root of a WeaveC source tree",
              file=sys.stderr)
        return 2
    result = run_checks(args.root)
    for violation in result.violations:
        print(violation.render())
    if result.violations:
        print()
    print("\n".join(summary_lines(result)))
    if args.json:
        try:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(document(result), indent=2) + "\n")
        except OSError as error:
            print(f"error: cannot write {args.json}: {error}", file=sys.stderr)
            return 2
    return 1 if result.violations else 0


if __name__ == "__main__":
    sys.exit(main())
