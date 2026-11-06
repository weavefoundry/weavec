#!/usr/bin/env python3
"""Check a self-contained WeaveC toolchain (RFC 0038, section 6, gate G8).

Unpacks a release archive (weavec-<version>-<os>-<arch>.tar.xz) or takes an
installed prefix, moves the tree to a new directory, and with nothing but that
tree and the platform's C toolchain:

  - checks the layout (bin/weavec-cc, lib/weavec/ with the runtime and
    weavec.h, lib/clang/<major>/include) and that weavec-cc loads no LLVM
    library from outside the tree;
  - compiles and runs the README's two examples (a heap overflow, a use after
    free) and checks their reports;
  - links a shared library and a program that uses it, and checks that an
    overflow inside the library stops;
  - on Linux, links a static program.

usage: check-toolchain.py ARCHIVE_OR_PREFIX [--keep]
Exit status 0 when every check passes, 1 otherwise. Standard library only.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

VEC = r"""
#include <stdio.h>
#include <stdlib.h>
struct vec { int *data; size_t len; };
static struct vec make(size_t n) {
  struct vec v = {calloc(n, sizeof(int)), n};
  return v;
}
int get(struct vec *v, size_t i) { return v->data[i]; }
int main(int argc, char **argv) {
  struct vec v = make(4);
  printf("%d\n", get(&v, argc > 1 ? (size_t)atoi(argv[1]) : 0));
  return 0;
}
"""

STALE = r"""
#include <stdlib.h>
struct session { void (*on_close)(void); };
static void bye(void) {}
int main(void) {
  struct session *s = malloc(sizeof *s);
  s->on_close = bye;
  free(s);
  for (int i = 0; i < 400000; i++)
    free(malloc(sizeof *s));
  s->on_close();
  return 0;
}
"""

LIBRARY = "int lib_read(const char *p, int i) { return p[i]; }\n"
PROGRAM = r"""
#include <stdlib.h>
int lib_read(const char *p, int i);
int main(int argc, char **argv) {
  char *p = calloc(4, 1);
  return lib_read(p, argc > 1 ? atoi(argv[1]) : 0);
}
"""


class Checker:
    def __init__(self, prefix: Path, work: Path):
        self.prefix = prefix
        self.work = work
        self.cc = prefix / "bin" / "weavec-cc"
        self.failures: list[str] = []
        # Nothing of a developer's environment: no LLVM on PATH, no WeaveC
        # variables.
        self.env = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": os.environ.get("HOME", "/tmp"),
                    "TMPDIR": os.environ.get("TMPDIR", "/tmp")}

    def check(self, condition: bool, what: str) -> None:
        print(("ok    " if condition else "FAIL  ") + what)
        if not condition:
            self.failures.append(what)

    def run(self, *args: str | Path, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([str(a) for a in args], cwd=cwd or self.work, env=self.env,
                              capture_output=True, text=True, timeout=600)

    def compile(self, name: str, source: str, *flags: str) -> Path | None:
        (self.work / f"{name}.c").write_text(source)
        out = self.work / name
        result = self.run(self.cc, "-O1", *flags, f"{name}.c", "-o", out)
        self.check(result.returncode == 0, f"weavec-cc builds {name}" + (
            "" if result.returncode == 0 else f": {result.stderr.strip()[:400]}"))
        return out if result.returncode == 0 else None

    def layout(self) -> None:
        self.check(self.cc.is_file(), "bin/weavec-cc is there")
        runtime = self.prefix / "lib" / "weavec"
        shared = "libweavec_rt.dylib" if sys.platform == "darwin" else "libweavec_rt.so"
        self.check((runtime / shared).exists(), f"lib/weavec/{shared} is there")
        self.check((runtime / "include" / "weavec.h").is_file(), "lib/weavec/include/weavec.h is there")
        headers = list((self.prefix / "lib" / "clang").glob("*/include/stddef.h"))
        self.check(bool(headers), "lib/clang/<major>/include holds Clang's resource headers")
        if sys.platform == "darwin":
            listing = self.run("otool", "-L", self.cc).stdout
            outside = [line.strip() for line in listing.splitlines()[1:]
                       if not re.match(r"\s*(/usr/lib/|/System/|@rpath/|@executable_path/)", line)]
        else:
            listing = self.run("ldd", self.cc).stdout
            outside = [line.strip() for line in listing.splitlines() if re.search(r"LLVM|clang", line)]
        self.check(not outside, "weavec-cc loads no LLVM library from outside the tree" + (
            f": {outside}" if outside else ""))
        version = self.run(self.cc, "--version")
        self.check(version.returncode == 0 and "runtime ABI" in version.stdout, "weavec-cc --version runs")

    def examples(self) -> None:
        vec = self.compile("vec", VEC)
        if vec:
            self.check(self.run(vec, "3").returncode == 0, "vec 3 runs")
            stopped = self.run(vec, "4")
            self.check(stopped.returncode != 0 and "weavec: heap-buffer-overflow at vec.c:" in stopped.stderr,
                       "vec 4 stops with heap-buffer-overflow")
        stale = self.compile("stale", STALE)
        if stale:
            stopped = self.run(stale)
            self.check(stopped.returncode != 0 and "weavec: heap-use-after-free at stale.c:" in stopped.stderr,
                       "stale stops with heap-use-after-free")

    def shared_library(self) -> None:
        (self.work / "lib.c").write_text(LIBRARY)
        name = "libread.dylib" if sys.platform == "darwin" else "libread.so"
        flags = ["-dynamiclib"] if sys.platform == "darwin" else ["-shared", "-fPIC"]
        built = self.run(self.cc, "-O1", *flags, "lib.c", "-o", name)
        self.check(built.returncode == 0, "weavec-cc links a shared library")
        program = self.compile("uses", PROGRAM, "-L.", "-lread", f"-Wl,-rpath,{self.work}")
        if program and built.returncode == 0:
            self.check(self.run(program, "3").returncode == 0, "the program runs with the library")
            stopped = self.run(program, "4")
            self.check(stopped.returncode != 0 and "weavec: heap-buffer-overflow at lib.c:" in stopped.stderr,
                       "an overflow inside the library stops")

    def lto_program(self) -> None:
        """RFC 0038 amendment 22: a Darwin -flto link uses the tree's libLTO."""
        if sys.platform != "darwin":
            return
        lto = self.prefix / "lib" / "libLTO.dylib"
        self.check(lto.is_file(), "lib/libLTO.dylib is there")
        program = self.compile("lto", VEC, "-flto=thin")
        if program:
            stopped = self.run(program, "4")
            self.check(stopped.returncode != 0 and "heap-buffer-overflow" in stopped.stderr,
                       "an -flto program stops an overflow")

    def static_program(self) -> None:
        if not sys.platform.startswith("linux"):
            return
        program = self.compile("static", VEC, "-static")
        if program:
            stopped = self.run(program, "4")
            self.check(stopped.returncode != 0 and "heap-buffer-overflow" in stopped.stderr,
                       "a static program stops an overflow")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("toolchain", type=Path)
    parser.add_argument("--keep", action="store_true", help="keep the moved tree and the work directory")
    args = parser.parse_args(argv)
    scratch = Path(tempfile.mkdtemp(prefix="weavec-toolchain-"))
    try:
        unpacked = scratch / "unpacked"
        if args.toolchain.is_file():
            with tarfile.open(args.toolchain) as archive:
                archive.extractall(unpacked, filter="tar")
            entries = list(unpacked.iterdir())
            tree = entries[0] if len(entries) == 1 else unpacked
        else:
            tree = scratch / "copied"
            shutil.copytree(args.toolchain, tree, symlinks=True)
        # Moved after unpacking: nothing may depend on where it was.
        prefix = scratch / "moved" / "weavec"
        prefix.parent.mkdir()
        shutil.move(str(tree), str(prefix))
        work = scratch / "work"
        work.mkdir()
        checker = Checker(prefix, work)
        print(f"toolchain: {prefix} ({platform.system()} {platform.machine()})")
        checker.layout()
        checker.examples()
        checker.shared_library()
        checker.static_program()
        checker.lto_program()
        if checker.failures:
            print(f"{len(checker.failures)} check(s) failed")
            return 1
        print("every check passed")
        return 0
    finally:
        if args.keep:
            print(f"kept {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
