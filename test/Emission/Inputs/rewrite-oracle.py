#!/usr/bin/env python3
"""RFC 0030 gate G8: the rewrite oracle.

    rewrite-oracle.py --weavec-cc W --clang C --opt O --resource-dir R \\
        SOURCE EXPECTED WORKDIR [-- FLAGS...]

Compiles SOURCE with `weavec-cc`, which inserts the checks, and EXPECTED, a
hand-written file that calls the same prelude helpers explicitly, with the
reference Clang and `-include` of the prelude `weavec-cc
-fweavec-print-prelude` prints. Both go to LLVM IR at -O0, lose their debug
information (`opt -S --strip-debug`) and the lines that name the source file,
and must then be equal; otherwise the script prints a diff and fails.

FLAGS go to both compilers, except the WeaveC flags among them
(`-fweavec-*`, `-fno-weavec-*`, `-W...weavec...`), which go only to
`weavec-cc` and its prelude. The reference compile also gets what
`weavec-cc` adds itself: `-D__WEAVEC__=1`, `-isystem` of the resource
directory and, unless zero-initialisation is off, `-ftrivial-auto-var-init=
zero`. Both get the same empty `-isysroot`, so neither reads a system
header. `ORACLE_FILE` is defined for the expected file as the path
`weavec-cc` was given, which report mode passes to the helpers.
"""

import argparse
import difflib
import os
import subprocess
import sys


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write("rewrite-oracle: command failed: %s\n%s%s" %
                         (" ".join(command), result.stdout, result.stderr))
        sys.exit(1)
    return result


def is_weavec_flag(flag):
    return (flag.startswith("-fweavec") or flag.startswith("-fno-weavec")
            or (flag.startswith("-W") and "weavec" in flag))


def normalised(opt, path):
    stripped = run([opt, "-S", "--strip-debug", path, "-o", "-"]).stdout
    return [line for line in stripped.splitlines()
            if not line.startswith("; ModuleID")
            and not line.startswith("source_filename")]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--weavec-cc", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--opt", required=True)
    parser.add_argument("--resource-dir", required=True)
    parser.add_argument("source")
    parser.add_argument("expected")
    parser.add_argument("work")
    parser.add_argument("flags", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    flags = [flag for flag in args.flags if flag != "--"]
    weavec_flags = [flag for flag in flags if is_weavec_flag(flag)]
    common = [flag for flag in flags if not is_weavec_flag(flag)]

    os.makedirs(os.path.join(args.work, "sdk"), exist_ok=True)
    sysroot = ["-isysroot", os.path.join(args.work, "sdk")]
    prelude = os.path.join(args.work, "prelude.h")
    instrumented = os.path.join(args.work, "instrumented.ll")
    expected = os.path.join(args.work, "expected.ll")

    run([args.weavec_cc, "-fweavec-print-prelude"] + weavec_flags +
        ["-o", prelude])
    run([args.weavec_cc] + sysroot + ["-O0", "-S", "-emit-llvm",
                                      "-fno-color-diagnostics"] +
        weavec_flags + common + [args.source, "-o", instrumented])
    zero = ("-fno-weavec-zero-init" not in weavec_flags and
            "-fweavec-checks=none" not in weavec_flags)
    reference = [args.clang] + sysroot + [
        "-O0", "-S", "-emit-llvm", "-include", prelude, "-isystem",
        args.resource_dir, "-D__WEAVEC__=1",
        '-DORACLE_FILE="%s"' % args.source]
    if zero:
        reference.append("-ftrivial-auto-var-init=zero")
    run(reference + common + [args.expected, "-o", expected])

    have = normalised(args.opt, instrumented)
    want = normalised(args.opt, expected)
    if have != want:
        sys.stderr.write("rewrite-oracle: %s does not rewrite to %s\n" %
                         (args.source, args.expected))
        sys.stderr.writelines(line + "\n" for line in difflib.unified_diff(
            want, have, "expected", "instrumented", lineterm=""))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
