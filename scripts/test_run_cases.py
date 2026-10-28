#!/usr/bin/env python3
"""Tests for the test/cases runner (scripts/run-cases.py, RFC 0036 section 9).

Unit level: the marker grammar, case loading and discovery, flags, output
parsing and the judging of synthetic evidence. The steps that start
processes (build, run_all, run_asan) are replaced by fakes, so no compiler
is needed."""
import contextlib
import importlib.util
import io
import signal
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location("run_cases", Path(__file__).with_name("run-cases.py"))
rc = importlib.util.module_from_spec(_SPEC)
sys.modules["run_cases"] = rc
_SPEC.loader.exec_module(rc)

TRAP = -int(signal.SIGTRAP)
SEGV = -int(signal.SIGSEGV)
ABRT = -int(signal.SIGABRT)


class Workspace(unittest.TestCase):
    """A temporary cases tree."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-run-cases-")
        self.addCleanup(self.directory.cleanup)
        self.cases = Path(self.directory.name).resolve() / "cases"

    def write(self, rel, text):
        path = self.cases / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text).lstrip("\n"))
        return path.resolve()

    def case(self, rel, text):
        return rc.load_case(self.write(rel, text), self.cases)

    def errors(self, text):
        return self.case("s/x.c", text).errors


def markers(text):
    return rc.parse_markers(Path("x.c"), textwrap.dedent(text).lstrip("\n"))


def report(kind, file=None, line=0, proven=False):
    return rc.Report(kind, None if file is None else str(file), line, 3 if file else 0, proven)


def run(code=0, *reports, args=(), timed_out=False):
    return rc.Run(tuple(args), code, timed_out, list(reports))


# ---------------------------------------------------------------------------
# Marker grammar
# ---------------------------------------------------------------------------


class ScanTest(unittest.TestCase):
    def test_comments_in_literals_and_block_comments_are_skipped(self):
        scan = rc.scan_source(textwrap.dedent("""\
            const char *s = "// TRAP"; /* // TRAP */
            char c = '/'; char q = '"'; // tail
            /* a block
               // TRAP
            */ int x;
            """))
        self.assertEqual([(c.line, c.body.strip(), c.code_before) for c in scan.comments],
                         [(2, "tail", True)])

    def test_first_declaration_skips_directives_and_continuations(self):
        scan = rc.scan_source("// head\n#include <x.h>\n#define M \\\n  1\n\nint x; // here\n")
        self.assertEqual(scan.first_declaration, 6)
        self.assertEqual([(c.line, c.code_before) for c in scan.comments], [(1, False), (6, True)])

    def test_file_markers_in_a_file_without_code(self):
        got = markers("// CLEAN\n// FLAGS: -O2\n")
        self.assertEqual((got.errors, [m.kind for m in got.file_markers]), ([], ["CLEAN", "FLAGS"]))


class SegmentTest(unittest.TestCase):
    def test_prose_is_not_a_marker(self):
        for text in ("RFC 0017: added", "NOTE: fine", "CWE-121: prose", "ASAN-confirmed prose",
                     "lowercase: text", "", "TODO"):
            with self.subTest(text=text):
                self.assertIsNone(rc.parse_segment(text))

    def test_keywords_and_arguments(self):
        self.assertEqual(rc.parse_segment("CLEAN"), ("CLEAN", ""))
        self.assertEqual(rc.parse_segment("STOP"), ("STOP", ""))
        self.assertEqual(rc.parse_segment("TRAP"), ("TRAP", ""))
        self.assertEqual(rc.parse_segment("GUARDED"), ("GUARDED", ""))
        self.assertEqual(rc.parse_segment("TRAP: null-dereference"), ("TRAP", "null-dereference"))
        self.assertEqual(rc.parse_segment("TRAP-AT: a.c:3"), ("TRAP-AT", "a.c:3"))
        self.assertEqual(rc.parse_segment("RUN-INPUT:"), ("RUN-INPUT", ""))

    def test_malformed_segments(self):
        for text, expected in (("CLEAN please", "takes no argument"),
                               ("CLEAN.", "takes no argument"),
                               ("STOP: here", "takes no argument"),
                               ("MISS why", "needs ':'"),
                               ("MISS", "needs ':'"),
                               ("MISS:", "needs an argument"),
                               ("TRAPS: x", "did you mean 'TRAP'"),
                               ("UNIT: a.c", "did you mean 'UNITS'")):
            with self.subTest(text=text):
                with self.assertRaisesRegex(ValueError, expected):
                    rc.parse_segment(text)

    def test_split_segments(self):
        self.assertEqual(rc.split_segments(" STOP // MISS: why "), ["STOP", "MISS: why"])

    def test_the_analysis_markers_are_gone(self):
        for text in ("BUG: leak", "ALLOW: leak", "TOOL", "NEUTRALISED: zero-init"):
            with self.subTest(text=text):
                self.assertIsNone(rc.parse_segment(text))


class ArgumentTest(unittest.TestCase):
    def parse(self, kind, argument, directory=Path(".")):
        return rc.parse_argument(kind, argument, directory)

    def test_trap_kinds(self):
        self.assertIsNone(self.parse("TRAP", ""))
        for kind in rc.KINDS:
            self.assertEqual(self.parse("TRAP", kind), kind)
        for old in ("index", "nonnull", "span", "live", "violation"):
            with self.subTest(kind=old), self.assertRaisesRegex(ValueError, "unknown report kind"):
                self.parse("TRAP", old)

    def test_ledger_reasons(self):
        self.assertIsNone(self.parse("GUARDED", ""))
        self.assertEqual(self.parse("GUARDED", "loop-range"), "loop-range")
        self.assertEqual(self.parse("PROVEN", "dominated"), "dominated")
        self.assertEqual(self.parse("UNGUARDED", "unsafe"), "unsafe")
        for kind, reason in (("GUARDED", "in-bounds"), ("PROVEN", "access"),
                             ("UNGUARDED", "spatial:unknown")):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "unknown"):
                self.parse(kind, reason)

    def test_miss(self):
        self.assertEqual(self.parse("MISS", "free text, any words"), "free text, any words")

    def test_run_input(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "in.txt").write_text("x")
            self.assertEqual(self.parse("RUN-INPUT", '1 "two words"', directory),
                             rc.RunInput(("1", "two words"), None))
            self.assertEqual(self.parse("RUN-INPUT", "", directory), rc.RunInput((), None))
            got = self.parse("RUN-INPUT", "a < in.txt", directory)
            self.assertEqual((got.args, got.stdin), (("a",), (directory / "in.txt").resolve()))
            with self.assertRaisesRegex(ValueError, "does not exist"):
                self.parse("RUN-INPUT", "< missing.txt", directory)
            with self.assertRaisesRegex(ValueError, "argv"):
                self.parse("RUN-INPUT", "< in.txt extra", directory)
            with self.assertRaisesRegex(ValueError, "RUN-INPUT"):
                self.parse("RUN-INPUT", '"unclosed', directory)

    def test_expect_ledger(self):
        got = self.parse("EXPECT-LEDGER", "/summary/unguarded == 0")
        self.assertEqual((got.pointer, got.op, got.value), ("/summary/unguarded", "==", 0))
        self.assertEqual(self.parse("EXPECT-LEDGER", "/config/checks != trap").value, "trap")
        self.assertEqual(self.parse("EXPECT-LEDGER", '/rows/0/reason == "access"').value, "access")
        for bad in ("summary/guarded == 0", "/summary/guarded ~ 0", "/summary/guarded"):
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "json-pointer"):
                self.parse("EXPECT-LEDGER", bad)

    def test_flags_units_trap_at_detect(self):
        self.assertEqual(self.parse("FLAGS", "-O2 -DX='a b'"), ("-O2", "-DX=a b"))
        self.assertEqual(self.parse("UNITS", "a.c  Inputs/b.c"), ("a.c", "Inputs/b.c"))
        self.assertEqual(self.parse("TRAP-AT", "Inputs/h.h:9"), ("Inputs/h.h", 9))
        self.assertEqual(self.parse("TRAP-AT", "c:/x.c:9"), ("c:/x.c", 9))
        for bad in ("a.c", "a.c:", ":9", "a.c:x"):
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "TRAP-AT takes"):
                self.parse("TRAP-AT", bad)
        self.assertEqual(self.parse("DETECT", "-DFIX -O0"), ("-DFIX", "-O0"))
        with self.assertRaisesRegex(ValueError, "fixed twin"):
            self.parse("DETECT", "FIX")


class ParseMarkersTest(unittest.TestCase):
    def test_line_and_file_markers(self):
        got = markers("""
            // Prose about the case.
            // FLAGS: -O2 // ASAN
            #include <stdlib.h>
            int f(int *p) {
              return p[4]; // STOP // MISS: the index is opaque
            }
            int g(int *p) { return *p; } // TRAP // GUARDED: access
            """)
        self.assertEqual(got.errors, [])
        self.assertEqual([(m.kind, m.line) for m in got.file_markers], [("FLAGS", 2), ("ASAN", 2)])
        self.assertEqual([(m.kind, m.line, m.value) for m in got.line_markers],
                         [("STOP", 5, ""), ("MISS", 5, "the index is opaque"),
                          ("TRAP", 7, None), ("GUARDED", 7, "access")])

    def test_placement_errors(self):
        got = markers("""
            // TRAP
            int x; // CLEAN
            // ASAN
            int y;
            """)
        self.assertEqual(len(got.errors), 3, got.errors)
        self.assertIn("x.c:1: line marker TRAP is on a line without code", got.errors[0])
        self.assertIn("x.c:2: file marker CLEAN must be on a comment line", got.errors[1])
        self.assertIn("x.c:3: file marker ASAN must appear before the first declaration", got.errors[2])

    def test_one_bad_segment_keeps_the_others(self):
        got = markers("int x; // GUARDED // TRAP: bounds\n")
        self.assertEqual([m.kind for m in got.line_markers], ["GUARDED"])
        self.assertEqual(len(got.errors), 1)
        self.assertIn("unknown report kind 'bounds'", got.errors[0])


# ---------------------------------------------------------------------------
# Cases and discovery
# ---------------------------------------------------------------------------


class LoadCaseTest(Workspace):
    def test_case_fields(self):
        self.write("s/in.txt", "data\n")
        case = self.case("s/sub/a.c", """
            // FLAGS: -std=c11
            // FLAGS: -O2
            // RUN-INPUT: 1
            // RUN-INPUT: 2 < ../in.txt
            // ASAN
            // EXPECT-LEDGER: /summary/unguarded == 0
            // XFAIL: one
            // XFAIL: two
            #include <stdlib.h>
            int main(int argc, char **argv) {
              int a[2]; a[argc] = 0; // TRAP: stack-buffer-overflow
              return 0; // GUARDED
            }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual((case.rel, case.suite), ("s/sub/a.c", "s"))
        self.assertEqual(case.flags, ("-std=c11", "-O2"))
        self.assertEqual([r.args for r in case.run_inputs], [("1",), ("2",)])
        self.assertEqual(case.run_inputs[1].stdin, self.cases / "s" / "in.txt")
        self.assertTrue(case.asan and case.has_main and case.is_bug_case)
        self.assertFalse(case.clean)
        self.assertEqual([e.pointer for e in case.expectations], ["/summary/unguarded"])
        self.assertEqual([(t.line, t.value) for t in case.traps], [(11, "stack-buffer-overflow")])
        self.assertEqual(case.xfail, "one; two")
        self.assertIsNone(case.detect)

    def test_trap_at_names_a_line_of_another_file(self):
        helper = self.write("s/Inputs/h.h", "static inline int h(int *p) { return p[9]; }\n")
        case = self.case("s/a.c", """
            // TRAP-AT: Inputs/h.h:1
            #include "Inputs/h.h"
            int main(void) { int a[2]; return h(a); }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual([(t.file, t.line, t.value) for t in case.traps], [(helper, 1, None)])
        self.assertTrue(case.is_bug_case)
        self.assertIn("TRAP-AT file 'Inputs/nope.h' does not exist",
                      self.errors("// TRAP-AT: Inputs/nope.h:1\nint main(void) { return 0; }\n")[0])

    def test_units(self):
        unit = self.write("s/Inputs/u.c", """
            // FLAGS: -DU
            int u(int *p) { return p[3]; } // TRAP
            """)
        plain = self.write("s/Inputs/plain.c", "// FLAGS: -fno-weavec\nint plain(void) { return 0; }\n")
        case = self.case("s/a.c", """
            // UNITS: Inputs/u.c Inputs/plain.c
            // FLAGS: -O1
            int u(int *); int plain(void);
            int main(void) { int a[2]; return u(a) + plain(); }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual(case.units, [self.cases / "s" / "a.c", unit, plain])
        self.assertEqual(case.unit_flags, {unit: ("-DU",), plain: ("-fno-weavec",)})
        self.assertEqual(case.flags, ("-O1",))
        self.assertEqual([(t.file, t.line) for t in case.traps], [(unit, 2)])

    def test_unit_errors(self):
        self.write("s/Inputs/bad.c", "// CLEAN\nint b;\n")
        errors = self.errors("""
            // UNITS: Inputs/bad.c Inputs/missing.c Inputs/bad.c
            // CLEAN
            int main(void) { return 0; }
            """)
        self.assertEqual(len(errors), 3, errors)
        self.assertIn("UNITS file 'Inputs/missing.c' does not exist", errors[0])
        self.assertIn("UNITS file 'Inputs/bad.c' is listed twice", errors[1])
        self.assertIn("bad.c:1: file marker CLEAN is only allowed in the case's main file", errors[2])
        self.assertIn("cannot contain -fno-weavec",
                      self.errors("// FLAGS: -fno-weavec\n// CLEAN\nint main(void) { return 0; }\n")[0])

    def test_consistency_errors(self):
        for text, expected in (
                ("int main(void) { return 0; }\n", "no expectation"),
                ("// CLEAN\nint main(void) { return 0; } // TRAP\n", "CLEAN contradicts"),
                ("// ASAN\nint x; // GUARDED\n", "ASAN needs a unit that defines main"),
                ("// RUN-INPUT: 1\nint x; // GUARDED\n", "RUN-INPUT needs a unit that defines main"),
                ("int x; // TRAP\n", "a case without main has nothing to run"),
                ("int main(void) { return 0; } // STOP\n", "STOP needs a DETECT file marker"),
                ("int main(void) { return 0; } // TRAP // MISS: why\n", "MISS marks a STOP line")):
            with self.subTest(text=text):
                errors = self.errors(text)
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(expected, errors[0])

    def test_detection_case(self):
        case = self.case("d/p.c", """
            // DETECT: -DFIX
            // RUN-INPUT: 3
            int main(int argc, char **argv) {
              int a[2];
              a[argc] = 0; // STOP // MISS: not yet
              return 0;
            }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual(case.detect, ("-DFIX",))

    def test_detection_errors(self):
        errors = self.errors("""
            // DETECT: -DFIX
            // DETECT: -DFIX2
            // CLEAN
            int x; // MISS: why
            """)
        self.assertEqual(len(errors), 5, errors)
        for expected in ("DETECT is given more than once", "a DETECT case has no CLEAN",
                         "DETECT needs a unit that defines main", "needs a STOP line",
                         "MISS marks a STOP line"):
            self.assertTrue(any(expected in e for e in errors), (expected, errors))
        errors = self.errors("// DETECT: -DFIX\nint main(void) { return 0; } // STOP // GUARDED // TRAP\n")
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("has no GUARDED, TRAP markers", errors[0])

    def test_defines_main(self):
        for text, expected in (("int main(void) {", True), ("static int main(int c, char **v)\n{", True),
                               ("void main() {", True), ("int main(void);", False),
                               ("int domain(void) {", False)):
            with self.subTest(text=text):
                self.assertEqual(rc.defines_main(text), expected)


class DiscoverTest(Workspace):
    def test_discover_and_select(self):
        self.write("a/one.c", "// UNITS: two.c\n// CLEAN\nint main(void) { return 0; }\n")
        self.write("a/two.c", "int t;\n")
        self.write("a/Inputs/helper.c", "int h;\n")
        self.write("a/deep/Inputs/x.c", "int h;\n")
        self.write("a/deep/three.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.write("b/four.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.write("b/notes.md", "// CLEAN\n")
        cases, parsed = rc.discover(self.cases)
        self.assertEqual([c.rel for c in cases], ["a/deep/three.c", "a/one.c", "b/four.c"])
        self.assertIn(self.cases / "a" / "two.c", parsed)
        self.assertEqual([c.rel for c in rc.select(cases, [])], [c.rel for c in cases])
        self.assertEqual([c.rel for c in rc.select(cases, ["a"])], ["a/deep/three.c", "a/one.c"])
        self.assertEqual([c.rel for c in rc.select(cases, ["a/"])], ["a/deep/three.c", "a/one.c"])
        self.assertEqual([c.rel for c in rc.select(cases, ["a/*.c"])], ["a/deep/three.c", "a/one.c"])
        self.assertEqual([c.rel for c in rc.select(cases, ["b/four.c", "a/deep"])],
                         ["a/deep/three.c", "b/four.c"])
        self.assertEqual(rc.select(cases, ["b/four"]), [])


# ---------------------------------------------------------------------------
# Flags and output parsing
# ---------------------------------------------------------------------------


class FlagsTest(unittest.TestCase):
    def test_plain_flags(self):
        self.assertEqual(rc.plain_flags(("-O2", "-fweavec-checks=verify", "-fno-weavec-zero-init",
                                         "-fno-weavec-asan-compat", "-std=c11")),
                         ["-O2", "-std=c11"])


class ParseOutputTest(unittest.TestCase):
    def test_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory).resolve()
            got = rc.parse_reports(textwrap.dedent("""\
                weavec: heap-buffer-overflow at a.c:12:5: write of 4 bytes at 0x1000
                weavec: 0x1000 is 0 bytes after the 16-byte heap object at 0xff0
                weavec: weavec.proven: index-out-of-bounds at /abs/b.c:3:9: index 7
                weavec: null-dereference at <unknown>: access at 0x0
                weavec: invalid release of 0x7ff0: not a heap block
                weavec: runtime: 3 allocations
                program output at x.c:1:1: is not a report
                """), directory)
            self.assertEqual(got, [
                rc.Report("heap-buffer-overflow", str(directory / "a.c"), 12, 5, False),
                rc.Report("index-out-of-bounds", str(Path("/abs/b.c").resolve()), 3, 9, True),
                rc.Report("null-dereference", None, 0, 0, False),
                rc.Report("invalid-release", None, 0, 0, False)])
        self.assertTrue(got[1].text().startswith("weavec.proven: index-out-of-bounds at "))
        self.assertEqual(got[2].text(), "null-dereference at <no location>")


class RunTest(unittest.TestCase):
    def test_stop(self):
        located = report("heap-use-after-free", "/x.c", 4)
        self.assertEqual(run(TRAP, located).stop, located)
        self.assertEqual(run(-int(signal.SIGILL), located).stop, located)
        self.assertEqual(run(SEGV).stop, rc.Report("null-dereference", None, 0, 0))
        fatal = report("invalid-release")
        self.assertEqual(run(ABRT, fatal).stop, fatal)
        self.assertIsNone(run(TRAP).stop)          # a trap with no report (the C library's own)
        self.assertIsNone(run(0, located).stop)    # a report that did not stop the run
        self.assertIsNone(run(1).stop)

    def test_describe(self):
        self.assertEqual(run(0).describe(), "exit 0")
        self.assertEqual(run(3).describe(), "exit 3")
        self.assertEqual(run(TRAP).describe(), "killed by SIGTRAP")
        self.assertEqual(run(None, timed_out=True).describe(), "timed out")
        self.assertTrue(run(SEGV).faulted and run(TRAP).trapped and not run(1).trapped)


class LedgerHelpersTest(unittest.TestCase):
    def test_json_pointer(self):
        document = {"summary": {"guarded": 3}, "rows": [{"a/b": 1, "m~n": 2}]}
        self.assertEqual(rc.json_pointer(document, "/summary/guarded"), 3)
        self.assertEqual(rc.json_pointer(document, "/rows/0/a~1b"), 1)
        self.assertEqual(rc.json_pointer(document, "/rows/0/m~0n"), 2)
        with self.assertRaises(KeyError):
            rc.json_pointer(document, "/summary/missing")

    def test_compare(self):
        self.assertTrue(rc.compare(3, "<=", 3) and rc.compare(3, ">", 2) and rc.compare("a", "!=", "b"))
        self.assertFalse(rc.compare(3, "<", 3))
        self.assertFalse(rc.compare("3", "<", 4))  # incomparable types never hold


# ---------------------------------------------------------------------------
# Judging
# ---------------------------------------------------------------------------


class JudgeRunsTest(Workspace):
    def judge(self, case, *runs):
        failures = []
        rc.judge_runs(case, rc.Evidence(runs=list(runs)), failures, [])
        return failures

    def trap_case(self, marker="TRAP: heap-buffer-overflow"):
        return self.case("s/t.c", f"""
            // RUN-INPUT: 1
            // RUN-INPUT: 2
            int main(void) {{
              return 0; // {marker}
            }}
            """)

    def test_expected_stop(self):
        case = self.trap_case()
        hit = run(TRAP, report("heap-buffer-overflow", case.path, 4))
        self.assertEqual(self.judge(case, hit), [])
        self.assertEqual(self.judge(case, run(0, args=("1",)), hit), [])  # any run may hit it

    def test_bare_trap_matches_any_kind(self):
        case = self.trap_case("TRAP")
        self.assertEqual(self.judge(case, run(TRAP, report("stack-use-after-scope", case.path, 4))), [])

    def test_wrong_kind_or_line(self):
        case = self.trap_case()
        for stop in (report("heap-use-after-free", case.path, 4), report("heap-buffer-overflow", case.path, 3)):
            with self.subTest(stop=stop):
                failures = self.judge(case, run(TRAP, stop))
                self.assertEqual(len(failures), 2, failures)
                self.assertIn("unexpected stop", failures[0])
                self.assertIn("t.c:4: expected a stop (heap-buffer-overflow); killed by SIGTRAP", failures[1])

    def test_trap_not_reached(self):
        failures = self.judge(self.trap_case(), run(0, args=("1",)), run(2, args=("2",)))
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("expected a stop (heap-buffer-overflow); exit 0; exit 2", failures[0])

    def test_unlocated_stops(self):
        null = self.trap_case("TRAP: null-dereference")
        self.assertEqual(self.judge(null, run(SEGV)), [])
        self.assertEqual(self.judge(self.trap_case("TRAP"), run(SEGV)), [])
        self.assertIn("unexpected stop: null-dereference at <no location>",
                      self.judge(self.trap_case(), run(SEGV))[0])

    def test_stop_in_a_shared_helper(self):
        helper = self.write("s/Inputs/h.c", "int h(int *p) {\n  return p[9];\n}\n")
        case = self.case("s/a.c", "// UNITS: Inputs/h.c\n// TRAP-AT: Inputs/h.c:2\nint main(void) { return 0; }\n")
        self.assertEqual(self.judge(case, run(TRAP, report("stack-buffer-overflow", helper, 2))), [])
        self.assertTrue(self.judge(case, run(0)))

    def test_bad_runs(self):
        case = self.case("s/c.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.assertIn("timed out", self.judge(case, run(None, timed_out=True))[0])
        self.assertIn("killed by SIGTRAP with no report", self.judge(case, run(TRAP))[0])
        self.assertIn("reported heap-buffer-overflow at", self.judge(
            case, run(0, report("heap-buffer-overflow", case.path, 2)))[0])
        self.assertEqual(self.judge(case, run(0), run(1)), [])

    def test_a_proven_report_is_a_wrong_proof(self):
        case = self.trap_case("TRAP")
        failures = self.judge(case, run(TRAP, report("heap-buffer-overflow", case.path, 4, proven=True)))
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("a proof was wrong: weavec.proven: heap-buffer-overflow", failures[0])


class JudgeLedgerTest(Workspace):
    def ledger(self, path, rows, version=3, summary=None):
        return {"schema": "weavec-ledger", "version": version, "units": [{
            "source": str(path), "config": {"checks": "trap", "zeroInit": True},
            "summary": summary or {"accesses": len(rows), "proven": 0, "guarded": len(rows), "unguarded": 0},
            "rows": [{"function": "main", "file": str(path), "line": line, "column": 3,
                      "operation": "load", "bytes": 4, "outcome": outcome, "reason": reason}
                     for line, outcome, reason in rows]}]}

    def judge(self, case, ledger):
        failures = []
        rc.judge_ledger(case, rc.Evidence(ledger=ledger), failures)
        return failures

    def test_rows(self):
        case = self.case("s/l.c", """
            int main(int c, char **v) {
              int a[4]; a[c] = 0; // GUARDED: access
              return a[0]; // PROVEN // UNGUARDED: unsafe
            }
            """)
        rows = [(2, "guarded", "access"), (3, "proven", "in-bounds"), (3, "unguarded", "unsafe")]
        self.assertEqual(self.judge(case, self.ledger(case.path, rows)), [])
        failures = self.judge(case, self.ledger(case.path, [(2, "guarded", "range"), (3, "proven", "dominated")]))
        where = rc.relative(case.path)
        self.assertEqual(failures, [f"{where}:2: expected a guarded ledger row (access)",
                                    f"{where}:3: expected a unguarded ledger row (unsafe)"])

    def test_missing_or_old_ledger(self):
        case = self.case("s/l.c", "int main(void) { return 0; } // GUARDED\n")
        self.assertEqual(self.judge(case, None), ["no enforcement ledger was written"])
        self.assertEqual(self.judge(case, self.ledger(case.path, [], version=2)),
                         ["ledger version 2, expected 3"])
        plain = self.case("s/p.c", "int main(void) { return 0; } // TRAP\n")
        self.assertEqual(self.judge(plain, None), [])  # no ledger marker, no ledger needed

    def test_expect_ledger(self):
        case = self.case("s/e.c", """
            // EXPECT-LEDGER: /summary/unguarded == 0
            // EXPECT-LEDGER: /summary/guarded >= 2
            // EXPECT-LEDGER: /config/checks == trap
            // EXPECT-LEDGER: /summary/missing == 0
            int main(void) { return 0; }
            """)
        failures = self.judge(case, self.ledger(case.path, [(5, "guarded", "access")]))
        self.assertEqual(failures, ["EXPECT-LEDGER /summary/guarded >= 2: actual 1",
                                    "EXPECT-LEDGER /summary/missing == 0: no such value"])


class Fakes(Workspace):
    """Replace the steps that start processes with ones that return canned evidence."""

    def setUp(self):
        super().setUp()
        self.scratch = Path(self.directory.name).resolve() / "scratch"
        self.scratch.mkdir()
        self.runs = {"build": [], "bug": [], "twin": []}
        self.ledger = None
        self.asan = None
        self.builds = []
        fakes = {"build": self.fake_build, "run_all": self.fake_run_all,
                 "run_asan": self.fake_run_asan}
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        for name, fake in fakes.items():
            original = getattr(rc, name)
            setattr(rc, name, fake)
            stack.callback(setattr, rc, name, original)

    def config(self, **overrides):
        values = dict(weavec_cc=Path("weavec-cc"), clang="clang", checks="trap",
                      asan=False, no_run=False, keep=False, compile_timeout=1.0, run_timeout=1.0,
                      scratch=self.scratch)
        values.update(overrides)
        return rc.Config(**values)

    def fake_build(self, case, cfg, ev, directory, extra, ledger):
        self.builds.append((directory.name, tuple(extra), ledger))
        ev.ledger = self.ledger if ledger else None
        ev.built = True
        return directory / "a.out" if case.has_main else None

    def fake_run_all(self, case, cfg, ev, exe, directory):
        ev.runs.extend(self.runs[directory.name])

    def fake_run_asan(self, case, cfg, ev, directory):
        ev.asan_ran = True
        ev.asan_report = self.asan


class RunMarkersTest(Fakes):
    def test_bug_case_passes_on_its_stop(self):
        case = self.case("s/b.c", "// ASAN\nint main(void) {\n  return 0; // TRAP\n}\n")
        self.runs["build"] = [run(TRAP, report("heap-buffer-overflow", case.path, 3))]
        self.asan = "heap-buffer-overflow"
        result = rc.run_case(case, self.config())
        self.assertEqual((result["status"], result["failures"]), ("pass", []))
        self.assertEqual(self.builds, [("build", (), False)])
        self.asan = None
        self.assertEqual(rc.run_case(case, self.config())["failures"], ["ASan reported nothing"])

    def test_clean_case_fails_on_a_stop_and_an_asan_report(self):
        case = self.case("s/c.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.runs["build"] = [run(TRAP, report("heap-buffer-overflow", case.path, 2))]
        self.asan = "heap-buffer-overflow"
        failures = rc.run_case(case, self.config(asan=True))["failures"]
        self.assertTrue(any("unexpected stop" in f for f in failures), failures)
        self.assertTrue(any("in a CLEAN case" in f for f in failures), failures)
        self.assertTrue(any("ASan reported heap-buffer-overflow in a case without a bug" in f
                            for f in failures), failures)

    def test_ledger_is_requested_only_when_read(self):
        case = self.case("s/l.c", "int main(void) { return 0; } // GUARDED\n")
        self.ledger = {"version": 3, "units": [{"rows": [{"file": str(case.path), "line": 1,
                                                          "outcome": "guarded", "reason": "access"}]}]}
        self.assertEqual(rc.run_case(case, self.config())["status"], "pass")
        self.assertEqual(self.builds, [("build", (), True)])

    def test_no_run(self):
        case = self.case("s/b.c", "int main(void) {\n  return 0; // TRAP\n}\n")
        self.assertEqual(rc.run_case(case, self.config(no_run=True))["status"], "pass")

    def test_marker_errors_and_xfail(self):
        bad = self.case("s/e.c", "int main(void) { return 0; }\n")
        self.assertEqual(rc.run_case(bad, self.config())["status"], "error")
        xfail = self.case("s/x.c", "// XFAIL: not yet\nint main(void) {\n  return 0; // TRAP\n}\n")
        self.runs["build"] = [run(0)]
        result = rc.run_case(xfail, self.config())
        self.assertEqual(result["status"], "xfail")
        self.assertIn("expected to fail: not yet", result["notes"])
        self.runs["build"] = [run(TRAP, report("heap-buffer-overflow", xfail.path, 3))]
        result = rc.run_case(xfail, self.config())
        self.assertEqual(result["status"], "xpass")
        self.assertIn("passes now: remove XFAIL (not yet)", result["notes"])


class RunDetectionTest(Fakes):
    def detection(self, stop_marker="STOP"):
        return self.case("d/p.c", f"""
            // DETECT: -DFIX
            int main(void) {{
              return 0; // {stop_marker}
            }}
            """)

    def test_the_bug_stops_and_the_twin_runs_clean(self):
        case = self.detection()
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 3))]
        self.runs["twin"] = [run(0)]
        result = rc.run_case(case, self.config())
        self.assertEqual((result["status"], result["failures"]), ("pass", []))
        self.assertEqual(result["detection"]["stop"], "run")
        self.assertEqual(self.builds, [("bug", (), False), ("twin", ("-DFIX",), False)])

    def test_a_fault_or_an_allocator_stop_counts(self):
        case = self.detection()
        for stop in (run(SEGV), run(ABRT, report("invalid-release"))):
            self.runs["bug"] = [stop]
            result = rc.run_case(case, self.config())
            self.assertEqual((result["status"], result["detection"]["stop"]), ("pass", "fault"))

    def test_a_stop_elsewhere_or_none_fails(self):
        case = self.detection()
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 2))]
        result = rc.run_case(case, self.config())
        self.assertEqual(result["status"], "fail")
        self.assertIn("on no STOP line", result["failures"][0])
        self.runs["bug"] = [run(0)]
        self.assertEqual(rc.run_case(case, self.config())["failures"],
                         ["the bug did not stop: the run did not stop"])

    def test_known_miss(self):
        case = self.detection("STOP // MISS: through a global")
        self.runs["bug"] = [run(0)]
        result = rc.run_case(case, self.config())
        self.assertEqual(result["status"], "pass")
        self.assertIn("known miss (through a global)", result["notes"][0])
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 3))]
        result = rc.run_case(case, self.config())
        self.assertEqual(result["status"], "pass")
        self.assertTrue(result["detection"]["flip"])
        self.assertIn("remove its MISS marker", result["notes"][0])

    def test_the_twin_must_not_stop(self):
        case = self.detection()
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 3))]
        self.runs["twin"] = [run(TRAP, report("heap-use-after-free", case.path, 3))]
        failures = rc.run_case(case, self.config())["failures"]
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("the fixed twin (-DFIX) stops: run (no arguments): killed by SIGTRAP", failures[0])

    def test_a_proven_report_fails(self):
        case = self.detection()
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 3, proven=True))]
        failures = rc.run_case(case, self.config(checks="verify"))["failures"]
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("a proof was wrong", failures[0])

    def test_summary(self):
        case = self.detection()
        self.runs["bug"] = [run(TRAP, report("heap-use-after-free", case.path, 3))]
        stopped = rc.run_case(case, self.config())
        self.runs["bug"] = [run(0)]
        missed = rc.run_case(case, self.config())
        error = rc.run_case(self.case("s/e.c", "int main(void) { return 0; }\n"), self.config())
        summary = rc.summarize([stopped, missed, error])
        self.assertEqual(summary["suites"], {"d": {"pass": 1, "fail": 1}, "s": {"error": 1}})
        self.assertEqual(summary["totals"], {"pass": 1, "fail": 1, "error": 1})
        self.assertEqual(summary["detection"], {"cases": 2, "stops": 1, "asan": 0})


class MainTest(Workspace):
    def test_missing_binaries(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = rc.main(["--build-dir", str(Path(self.directory.name) / "nowhere")])
        self.assertEqual(code, 2)
        self.assertIn("does not exist; build it first", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
