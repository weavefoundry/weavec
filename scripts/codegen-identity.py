#!/usr/bin/env python3
"""Gate G7 of RFC 0030: deferred CodeGen leaves every object unchanged.

For each translation unit of a list and each configuration (`-O2`,
`-O0 -g` and `-O2 -flto=thin` by default), the object `weavec-cc -c`
writes must be byte-identical (`cmp`) to the one the reference Clang writes
with `-D__WEAVEC__=1 -isystem resources/include`, the two flags `weavec-cc`
adds itself. `DeferredCodeGenConsumer` (RFC 0030, section 10.5) holds every
CodeGen callback until the WeaveC analysis has run; with no checks emitted
it must change nothing.

Both compilers get the same `-isysroot` (on macOS, `xcrun --show-sdk-path`
unless `--sysroot` says otherwise): Homebrew's clang reads a configuration
file that may name a different SDK than the one `weavec-cc` finds, and
`-g` records the SDK. Until `-fweavec-checks=none` exists, `weavec-cc` gets
`-Wno-error=weavec` (see `--weavec-flag`) so that units with WeaveC errors
still produce objects.

List format (`--list`): one unit per line, `<project>/<path> [args...]`,
shell-quoted; `#` starts a comment. Each unit compiles in
`<root>/<project>` with `<path>` relative to it, so `-I.` means the
project's top directory. `{support}` in an argument expands to
`<support>/<project>`.

Examples:

  scripts/codegen-identity.py --weavec-cc build/release/bin/weavec-cc \\
      --list test/corpus/identity.txt --min 100
  scripts/codegen-identity.py --weavec-cc build/w3/bin/weavec-cc \\
      --root build/corpus --list test/corpus/identity.txt \\
      --config='-O1' -- -DNDEBUG

The exit status is 0 when every compared object is identical, no
`weavec-cc` compile fails where the reference compile succeeded, and each
configuration has at least `--min` identical units. Units the reference
compiler cannot build are skipped and counted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import filecmp
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIGS = ["-O2", "-O0 -g", "-O2 -flto=thin"]
DEFAULT_WEAVEC_FLAGS = ["-Wno-error=weavec"]


@dataclasses.dataclass(frozen=True)
class Unit:
    project: str
    path: str
    args: tuple[str, ...]

    @property
    def name(self) -> str:
        return f"{self.project}/{self.path}"


@dataclasses.dataclass
class Outcome:
    unit: Unit
    config: str
    status: str  # identical | different | weavec-failed | skipped
    detail: str = ""


def parse_list(text: str, support: Path) -> list[Unit]:
    units = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = shlex.split(line)
        project, _, path = words[0].partition("/")
        if not project or not path:
            raise SystemExit(f"list line {number}: expected <project>/<path>")
        args = tuple(
            word.replace("{support}", str(support / project)) for word in words[1:]
        )
        units.append(Unit(project=project, path=path, args=args))
    return units


def default_clang() -> str:
    prefix = os.environ.get("WEAVEC_LLVM_PREFIX")
    if not prefix and shutil.which("brew"):
        result = subprocess.run(
            ["brew", "--prefix", "llvm"], capture_output=True, text=True
        )
        prefix = result.stdout.strip() if result.returncode == 0 else ""
    if prefix and (Path(prefix) / "bin" / "clang").exists():
        return str(Path(prefix) / "bin" / "clang")
    return shutil.which("clang") or "clang"


def default_sysroot() -> str:
    if platform.system() != "Darwin":
        return ""
    if os.environ.get("SDKROOT"):
        return os.environ["SDKROOT"]
    result = subprocess.run(
        ["xcrun", "--show-sdk-path"], capture_output=True, text=True
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def default_support() -> Path:
    for candidate in (ROOT / "test" / "corpus" / "support",
                      ROOT / "scripts" / "corpus" / "support"):
        if candidate.is_dir():
            return candidate
    return ROOT / "test" / "corpus" / "support"


def run_compiler(command: list[str], cwd: Path, out: Path, timeout: int) -> str:
    """Compiles to `out`; returns an empty string or why it failed."""
    try:
        result = subprocess.run(
            command + ["-o", str(out)],
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return "timed out"
    if result.returncode == 0 and out.exists():
        return ""
    lines = result.stderr.strip().splitlines()
    return lines[-1][:300] if lines else f"exit status {result.returncode}"


def compare(unit: Unit, config: str, options: argparse.Namespace) -> Outcome:
    cwd = options.root / unit.project
    # weavec-cc appends -isystem and -D__WEAVEC__=1 after the user's flags;
    # the reference gets them in the same place, so the -cc1 lines match.
    common = (
        shlex.split(config)
        + list(unit.args)
        + options.both
        + ["-c", unit.path]
    )
    reference = [options.clang] + common + options.reference_only
    weavec = [options.weavec_cc] + common + options.weavec_only
    with tempfile.TemporaryDirectory(prefix="weavec-g7-", dir=options.tmp) as tmp:
        work = Path(tmp)
        out = work / "unit.o"
        why = run_compiler(reference, cwd, out, options.timeout)
        if why:
            return Outcome(unit, config, "skipped", why)
        out.rename(work / "clang.o")
        # Same output path for both: nothing about it can differ.
        why = run_compiler(weavec, cwd, out, options.timeout)
        if why:
            return Outcome(unit, config, "weavec-failed", why)
        out.rename(work / "weavec.o")
        if filecmp.cmp(work / "clang.o", work / "weavec.o", shallow=False):
            return Outcome(unit, config, "identical")
        if options.keep:
            label = (unit.name + config).replace("/", "_").replace(" ", "")
            options.keep.mkdir(parents=True, exist_ok=True)
            shutil.copy(work / "clang.o", options.keep / f"{label}.clang.o")
            shutil.copy(work / "weavec.o", options.keep / f"{label}.weavec.o")
        return Outcome(unit, config, "different")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="Flags after `--` go to both compilers.",
    )
    parser.add_argument("--weavec-cc", required=True, help="weavec-cc binary")
    parser.add_argument("--clang", help="reference clang (default: LLVM's)")
    parser.add_argument("--list", type=Path, action="append", default=[],
                        help="unit list (repeatable)")
    parser.add_argument("--root", type=Path, default=ROOT / "build" / "corpus",
                        help="directory holding the project checkouts")
    parser.add_argument("--support", type=Path,
                        help="per-project support files ({support})")
    parser.add_argument("--resource-include", type=Path,
                        default=ROOT / "resources" / "include",
                        help="the directory with weavec.h weavec-cc uses")
    parser.add_argument("--sysroot", default="auto",
                        help="-isysroot for both compilers: auto, none or a path")
    parser.add_argument("--config", action="append",
                        help="flags of one configuration (repeatable; default: "
                        + ", ".join(repr(c) for c in DEFAULT_CONFIGS) + ")")
    parser.add_argument("--weavec-flag", action="append",
                        help="flag only weavec-cc gets (repeatable; default: "
                        + " ".join(DEFAULT_WEAVEC_FLAGS) + ")")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--min", type=int, default=0,
                        help="identical units each configuration needs")
    parser.add_argument("--keep", type=Path,
                        help="copy differing object pairs here")
    parser.add_argument("--tmp", help="directory for temporary objects")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("units", nargs="*",
                        help="extra units, as in a list line (quoted)")
    argv = list(sys.argv[1:] if argv is None else argv)
    extra: list[str] = []
    if "--" in argv:
        extra = argv[argv.index("--") + 1 :]
        argv = argv[: argv.index("--")]
    options = parser.parse_args(argv)
    # Units compile in their project's directory: make every path absolute.
    options.weavec_cc = str(Path(options.weavec_cc).resolve())
    options.clang = options.clang or default_clang()
    if os.sep in options.clang:
        options.clang = str(Path(options.clang).resolve())
    options.root = options.root.resolve()
    options.support = (options.support or default_support()).resolve()
    options.resource_include = options.resource_include.resolve()
    options.keep = options.keep.resolve() if options.keep else None
    sysroot = default_sysroot() if options.sysroot == "auto" else (
        "" if options.sysroot == "none" else options.sysroot)
    options.both = (["-isysroot", sysroot] if sysroot else []) + extra
    options.reference_only = ["-isystem", str(options.resource_include),
                              "-D__WEAVEC__=1"]
    options.weavec_only = options.weavec_flag or list(DEFAULT_WEAVEC_FLAGS)
    configs = options.config or list(DEFAULT_CONFIGS)

    units: list[Unit] = []
    for path in options.list:
        units += parse_list(path.read_text(), options.support)
    units += parse_list("\n".join(options.units), options.support)
    if not units:
        parser.error("no units: give --list or units")
    if not Path(options.weavec_cc).exists():
        parser.error(f"no weavec-cc at {options.weavec_cc}")

    tasks = [(unit, config) for config in configs for unit in units]
    outcomes: list[Outcome] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        futures = [pool.submit(compare, unit, config, options)
                   for unit, config in tasks]
        for future in concurrent.futures.as_completed(futures):
            outcome = future.result()
            outcomes.append(outcome)
            if options.verbose or outcome.status in ("different", "weavec-failed"):
                print(f"{outcome.status:13} {outcome.config:16} "
                      f"{outcome.unit.name} {outcome.detail}".rstrip(),
                      file=sys.stderr, flush=True)

    ok = True
    print(f"units: {len(units)}; weavec-cc: {options.weavec_cc}; "
          f"reference: {options.clang}; sysroot: {sysroot or '(default)'}")
    for config in configs:
        counts = {status: 0 for status in
                  ("identical", "different", "weavec-failed", "skipped")}
        for outcome in outcomes:
            if outcome.config == config:
                counts[outcome.status] += 1
        print(f"{config:16} " + ", ".join(f"{n} {s}" for s, n in counts.items()))
        if counts["different"] or counts["weavec-failed"]:
            ok = False
        if counts["identical"] < options.min:
            print(f"{config:16} fewer than {options.min} identical units")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
