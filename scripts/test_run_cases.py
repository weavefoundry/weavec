#!/usr/bin/env python3
"""Tests for the test/cases runner (RFC 0030 section 17): marker grammar,
classification, and the build/ledger/run/ASan pipeline against fake tools."""
import contextlib
import importlib.util
import io
import json
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location("run_cases", Path(__file__).with_name("run-cases.py"))
rc = importlib.util.module_from_spec(_SPEC)
sys.modules["run_cases"] = rc
_SPEC.loader.exec_module(rc)


def ledger(source, sites, scope="unit", summary=None, root=None):
    """A minimal weavec-ledger document with one function holding `sites`."""
    return {
        "schema": "weavec-ledger", "version": 1, "scope": scope, "root": root or "/",
        "summary": summary or {"errors": 0, "warnings": 0},
        "units": [{"source": str(source), "functions": [{"name": "f", "line": 1, "sites": sites}]}],
        "diagnostics": [],
    }


def site(line, **facets):
    return {"ordinal": 0, "kind": "deref", "line": line, "column": 3, "text": "p[0]", "facets": facets}


class Workspace(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-run-cases-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.cases = self.root / "cases"

    def write(self, rel, text):
        path = self.cases / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text).lstrip("\n"))
        return path.resolve()

    def case(self, rel, text):
        path = self.write(rel, text)
        return rc.load_case(path, self.cases)

    def diag(self, path, line, identifier="use-after-free", severity="error", column=3):
        return rc.Diagnostic(str(path), line, column, severity, identifier, "message")


class MarkerGrammarTest(Workspace):
    def test_line_and_file_markers(self):
        case = self.case("s/a.c", """
            // Provenance comment.
            // FLAGS: -std=c11 -DX='a b'
            // RUN-INPUT: 1 "two words"
            // RUN-INPUT:
            // ASAN
            #include <stdlib.h>
            int main(void) {
              int a[2]; a[2] = 0; // BUG: out-of-bounds // TRAP: index
              return 0; // BUG: leak possible
            }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual(case.flags, ("-std=c11", "-DX=a b"))
        self.assertEqual([r.args for r in case.run_inputs], [("1", "two words"), ()])
        self.assertTrue(case.asan and case.has_main and case.is_bug_case)
        self.assertEqual([(b.line, b.value) for b in case.bugs],
                         [(8, ("out-of-bounds", None)), (9, ("leak", "possible"))])
        self.assertEqual([(t.line, t.value) for t in case.traps], [(8, "index")])
        self.assertEqual(case.suite, "s")

    def test_comments_inside_literals_and_block_comments_are_not_markers(self):
        markers = rc.parse_markers(Path("x.c"), textwrap.dedent("""
            const char *s = "// BUG: leak"; /* // BUG: leak */
            char c = '/'; // CLEAN-ish prose is not a marker: NOTE: fine
            /* a block
               // BUG: leak
            */ int x; // RFC 0017: prose // ASAN-confirmed prose // CWE-121: prose
            """))
        self.assertEqual(markers.line_markers, [])
        self.assertEqual(markers.file_markers, [])
        self.assertEqual(markers.errors, [])

    def test_placement_rules(self):
        markers = rc.parse_markers(Path("x.c"), textwrap.dedent("""
            // BUG: leak
            int x; // CLEAN
            // ASAN
            int y;
            """))
        self.assertEqual(len(markers.errors), 3, markers.errors)
        self.assertIn("line without code", markers.errors[0])
        self.assertIn("must be on a comment line", markers.errors[1])
        self.assertIn("before the first declaration", markers.errors[2])

    def test_malformed_markers_are_errors(self):
        for text, expected in (("int x; // BUG: no-such-id", "unknown diagnostic id"),
                               ("int x; // BUG: leak maybe", "definite|possible"),
                               ("int x; // TRAP: bounds", "unknown trap template"),
                               ("int x; // UNRESOLVED: spatial:whatever", "unknown unresolved reason"),
                               ("int x; // TRUSTED: temporal:unknown-callee", "unknown trusted reason"),
                               ("int x; // NOT-PROVEN: memory", "unknown facet"),
                               ("int x; // NEUTRALISED: zero", "zero-init"),
                               ("int x; // BUGS: leak", "did you mean 'BUG'"),
                               ("// CLEAN please", "takes no argument"),
                               ("// CLEAN.", "takes no argument"),
                               ("int x; // MISS", "needs ':'"),
                               ("// EXPECT-LEDGER: summary/errors == 0", "json-pointer")):
            with self.subTest(text=text):
                errors = rc.parse_markers(Path("x.c"), text + "\n").errors
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(expected, errors[0])

    def test_marker_values(self):
        markers = rc.parse_markers(self.root / "x.c", textwrap.dedent("""
            // EXPECT-LEDGER: /summary/unresolved <= 3
            // EXPECT-LEDGER: /config/checks == trap
            // EXPECT-LEDGER: /summary/overBudget == []
            int x; // UNRESOLVED: spatial:unknown-extent // TRUSTED: temporal:concurrency
            int y; // NOT-PROVEN: null // MISS: not modelled yet // NEUTRALISED: zero-init
            """))
        self.assertEqual(markers.errors, [])
        values = [m.value for m in markers.file_markers]
        self.assertEqual([(v.pointer, v.op, v.value) for v in values],
                         [("/summary/unresolved", "<=", 3), ("/config/checks", "==", "trap"),
                          ("/summary/overBudget", "==", [])])
        self.assertEqual([(m.kind, m.value) for m in markers.line_markers],
                         [("UNRESOLVED", ("spatial", "unknown-extent")),
                          ("TRUSTED", ("temporal", "concurrency")), ("NOT-PROVEN", "null"),
                          ("MISS", "not modelled yet"), ("NEUTRALISED", "zero-init")])

    def test_run_input_redirect(self):
        self.write("s/input.txt", "x\n")
        case = self.case("s/a.c", """
            // CLEAN
            // RUN-INPUT: -v < input.txt
            int main(void) { return 0; }
            """)
        self.assertEqual(case.errors, [])
        self.assertEqual(case.run_inputs[0].args, ("-v",))
        self.assertEqual(case.run_inputs[0].stdin, self.cases / "s" / "input.txt")
        bad = self.case("s/b.c", """
            // CLEAN
            // RUN-INPUT: < missing.txt
            int main(void) { return 0; }
            """)
        self.assertIn("does not exist", bad.errors[0])

    def test_case_level_consistency(self):
        for text, expected in (("// CLEAN\nint x; // BUG: leak\n", "contradicts"),
                               ("// ALLOW: leak\nint x; // BUG: leak\n", "ALLOW only applies"),
                               ("int x;\n", "no expectation"),
                               ("// TOOL\n// RUN-INPUT: 1\n// CLEAN\nint x;\n", "not run"),
                               ("// CLEAN\n// ASAN\nint x;\n", "ASAN needs"),
                               ("// CLEAN\n// FLAGS: -fno-weavec\nint main(void) { return 0; }\n", "-fno-weavec")):
            with self.subTest(text=text):
                case = self.case("s/c.c", text)
                self.assertTrue(any(expected in e for e in case.errors), case.errors)

    def test_units_and_discovery(self):
        self.write("s/Inputs/helper.h", "void helper(char *p);\n")
        self.write("s/Inputs/helper.c", """
            // FLAGS: -DHELPER
            #include <stdlib.h>
            void helper(char *p) { free(p); } // BUG: double-free
            """)
        self.write("s/link.c", """
            // FLAGS: -fno-weavec
            int shared;
            """)
        main = self.write("s/main.c", """
            // UNITS: Inputs/helper.c link.c
            // FLAGS: -O1
            #include "Inputs/helper.h"
            int main(void) { return 0; } // BUG: leak
            """)
        self.write("t/other.c", "// CLEAN\nint main(void) { return 0; }\n")
        cases, _ = rc.discover(self.cases)
        self.assertEqual([c.rel for c in cases], ["s/main.c", "t/other.c"])
        case = cases[0]
        self.assertEqual(case.errors, [])
        self.assertEqual([u.name for u in case.units], ["main.c", "helper.c", "link.c"])
        self.assertEqual([u.name for u in case.analysed_units()], ["main.c", "helper.c"])
        self.assertEqual(case.unit_flags[case.units[1]], ("-DHELPER",))
        self.assertEqual([(b.file.name, b.value[0]) for b in case.bugs],
                         [("main.c", "leak"), ("helper.c", "double-free")])
        self.assertEqual([c.rel for c in rc.select(cases, ["s/**"])], ["s/main.c"])
        self.assertEqual([c.rel for c in rc.select(cases, ["t"])], ["t/other.c"])
        self.assertEqual(rc.select(cases, ["u/**"]), [])
        self.assertEqual(rc.legacy_command(Path("weavec"), case)[1:5],
                         ["--whole-program", str(main), str(case.units[1]), "--"])

    def test_unit_file_markers_other_than_flags_are_errors(self):
        self.write("s/u.c", "// CLEAN\nint u;\n")
        case = self.case("s/m.c", "// UNITS: u.c\n// CLEAN\nint main(void) { return 0; }\n")
        self.assertTrue(any("only allowed in the case's main file" in e for e in case.errors))

    def test_defines_main(self):
        self.assertTrue(rc.defines_main("int main(int argc,\n char **argv)\n{ return 0; }"))
        self.assertTrue(rc.defines_main("static int helper(void);\nint main() { return helper(); }"))
        self.assertFalse(rc.defines_main("int main(void);\nint not_main(void) { return 0; }"))


class TranslationAndParsingTest(unittest.TestCase):
    def test_flag_translation(self):
        flags = ["-std=c11", "-Wno-weavec-leak", "-Werror=weavec", "-fweavec-strict",
                 "-fweavec-checked-function=f", "-fweavec-require=checked", "-fno-weavec-zero-init",
                 "-fweavec-checks=verify", "-fno-weavec-strict", "-Werror"]
        legacy = rc.tool_arguments(flags, legacy=True)
        self.assertEqual(legacy.options, ["-Wno-weavec-leak", "-Werror=weavec", "--strict-externs",
                                          "--checked-function=f"])
        self.assertEqual(legacy.compiler, ["-std=c11", "-Werror"])
        self.assertEqual(legacy.dropped, ["-fweavec-require=checked", "-fno-weavec-zero-init",
                                          "-fweavec-checks=verify", "-fno-weavec-strict"])
        new = rc.tool_arguments(flags, legacy=False)
        self.assertIn("--require=checked", new.options)
        self.assertIn("--no-zero-init", new.options)
        self.assertEqual(rc.plain_flags(flags), ["-std=c11", "-Werror"])

    def test_diagnostic_parsing(self):
        output = "\n".join([
            "/src/a.c:4:7: error: use of 'p' after it was freed [weavec::use-after-free]",
            "/src/a.c:4:7: note: freed here",
            "weavec-cc: warning: link input 'x.o' has no WeaveC record; calls into it are trusted "
            "[weavec::unanalyzed-input]",
            "rel.c:2:1: warning: 'q' is leaked [weavec::leak]",
            "/src/a.c:9:1: error: expected ';' after expression",
            "weavec-cc: error: unknown WeaveC flag '-fweavec-ledger=/tmp/'",
            "ld: warning: object file was built for newer macOS version",
            "Undefined symbols for architecture arm64:",
            "clang: error: linker command failed with exit code 1 (use -v to see invocation)",
        ])
        diagnostics, clang_errors, link_errors = rc.parse_diagnostics(output, Path("/work"))
        self.assertEqual([(Path(d.file).name if d.file else "", d.line, d.severity, d.id) for d in diagnostics],
                         [("a.c", 4, "error", "use-after-free"), ("", 0, "warning", "unanalyzed-input"),
                          ("rel.c", 2, "warning", "leak")])
        self.assertTrue(diagnostics[2].file.endswith("/work/rel.c"))
        self.assertEqual(len(clang_errors), 2)
        self.assertEqual(len(link_errors), 2)

    def test_report_and_sanitizer_parsing(self):
        reports = rc.parse_reports("hello\nweavec: runtime check failed: span at /s/a.c:12:9\n")
        self.assertEqual([(r.template, r.file, r.line, r.column) for r in reports],
                         [("span", str(Path("/s/a.c").resolve()), 12, 9)])
        asan = rc.parse_sanitizer(textwrap.dedent("""
            =================================================================
            ==123==ERROR: AddressSanitizer: heap-use-after-free on address 0x602 at pc 0x1
            READ of size 1 at 0x602 thread T0
                #0 0x100003f1c in peek b.c:5
                #1 0x100003f80 in main /abs/src/b.c:12:10
                #2 0x18a0 in start+0x1b4c (dyld:arm64e+0x204e0)
            0x602 is located 0 bytes inside of 8-byte region
            freed by thread T0 here:
                #0 0x1 in free+0x98
            SUMMARY: AddressSanitizer: heap-use-after-free b.c:5 in peek
            """))
        self.assertEqual(asan.kind, "heap-use-after-free")
        self.assertEqual(asan.frames[:2], (("b.c", 5, "peek"), ("/abs/src/b.c", 12, "main")))
        self.assertEqual(len(asan.in_case([Path("/elsewhere/b.c")])), 2)
        ubsan = rc.parse_sanitizer("x.c:4:35: runtime error: index -1 out of bounds for type 'int[4]'\n"
                                   "    #0 0x1 in lookup x.c:4\n    #1 0x2 in main x.c:7\n")
        self.assertTrue(ubsan.kind.startswith("runtime error: index -1"))
        self.assertEqual([f[1] for f in ubsan.frames], [4, 4, 7])
        self.assertIsNone(rc.parse_sanitizer("all good\n"))

    def test_ledger_helpers(self):
        document = ledger("test/a.c", [site(3, null={"outcome": "checked", "check": {"template": "nonnull"},
                                                     "requirements": [{"outcome": "unresolved",
                                                                       "reason": "unknown-extent"}]})],
                          root="/repo")
        rows = rc.ledger_rows(document)
        self.assertEqual(rows[0].file, str(Path("/repo/test/a.c").resolve()))
        records = rc.facet_records(rows[0], "null")
        self.assertEqual([r["outcome"] for r in records], ["checked", "unresolved"])
        self.assertEqual(rc.facet_records(rows[0], "spatial"), [])
        self.assertEqual(rc.json_pointer({"a/b": {"~k": [1, 2]}}, "/a~1b/~0k/1"), 2)
        with self.assertRaises(KeyError):
            rc.json_pointer({"a": 1}, "/b")
        self.assertTrue(rc.compare(3, "<=", 3) and rc.compare("trap", "==", "trap")
                        and rc.compare([1], "!=", [2]))
        with self.assertRaises(TypeError):
            rc.compare("3", "<", 4)


class EvaluationTest(Workspace):
    def bug_case(self, marker="BUG: use-after-free", extra_file_markers="// A use after free."):
        return self.case("s/p.c", f"""
            {extra_file_markers}
            #include <stdlib.h>
            int main(void) {{
              char *p = malloc(1);
              free(p);
              return p[0]; // {marker}
            }}
            """)

    def evidence(self, **kwargs):
        ev = rc.Evidence(kwargs.pop("mode", "trap"))
        for key, value in kwargs.items():
            setattr(ev, key, value)
        return ev

    def test_bug_severity_classes(self):
        case = self.bug_case("BUG: use-after-free definite")
        self.assertEqual(case.bugs[0].line, 6)
        warning = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6, severity="warning")]))
        self.assertEqual(warning["status"], "fail")
        self.assertEqual(warning["class"], "warning")
        self.assertTrue(warning["bugs"][0]["reported"])
        error = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6)]))
        self.assertEqual((error["status"], error["class"]), ("pass", "error"))
        either = self.bug_case()
        self.assertEqual(rc.evaluate(either, self.evidence(
            diagnostics=[self.diag(either.path, 6, severity="warning")]))["status"], "pass")

    def test_wrong_id_or_line_and_unexpected_errors(self):
        case = self.bug_case()
        wrong = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6, "double-free")]))
        self.assertEqual(wrong["class"], "silent")
        self.assertTrue(any("not satisfied (found double-free (error))" in f for f in wrong["failures"]))
        elsewhere = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6),
                                                                 self.diag(case.path, 5, "double-free")]))
        self.assertTrue(any(f.startswith("unexpected error") for f in elsewhere["failures"]))
        warned = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6),
                                                              self.diag(case.path, 4, "leak", "warning")]))
        self.assertEqual(warned["status"], "pass")
        crashed = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6)],
                                                  tool_failures=["compile p.c failed with status -11"]))
        self.assertEqual(crashed["status"], "fail")

    def test_clean_and_allow(self):
        case = self.case("s/c.c", "// CLEAN\n// ALLOW: leak\nint main(void) { return 0; }\n")
        leak = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 3, "leak", "warning")]))
        self.assertEqual(leak["status"], "pass")
        other = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 3, "double-free", "warning")]))
        self.assertEqual(other["status"], "fail")
        trapped = rc.evaluate(case, self.evidence(runs=[rc.Run((), -5)], ran=True, built=True))
        self.assertEqual(trapped["status"], "fail")
        segv = rc.evaluate(case, self.evidence(runs=[rc.Run(("x",), -11)], ran=True, built=True))
        self.assertTrue(any("SIGSEGV" in f for f in segv["failures"]))
        self.assertTrue(rc.evaluate(case, self.evidence())["cleanBuild"])

    def test_traps_and_reports(self):
        case = self.case("s/t.c", """
            int main(int argc, char **argv) {
              int a[4] = {0};
              return a[argc + 3]; // BUG: out-of-bounds // TRAP: index
            }
            """)
        at = str(case.path)
        good = rc.evaluate(case, self.evidence(built=True, ran=True, runs=[rc.Run((), -5)],
                                               reports=[rc.CheckReport("index", at, 3, 10)]))
        self.assertEqual((good["status"], good["class"]), ("pass", "trap"))
        self.assertEqual(good["bugs"][0]["satisfiedBy"], "trap")
        wrong_template = rc.evaluate(case, self.evidence(built=True, ran=True, runs=[rc.Run((), -4)],
                                                         reports=[rc.CheckReport("span", at, 3, 10)]))
        self.assertTrue(any("TRAP index not reported" in f for f in wrong_template["failures"]))
        self.assertTrue(any("unexpected runtime check failure: span" in f for f in wrong_template["failures"]))
        no_trap = rc.evaluate(case, self.evidence(built=True, ran=True, runs=[rc.Run((), 0)],
                                                  reports=[rc.CheckReport("index", at, 3, 10)]))
        self.assertTrue(any("but the case has TRAP markers" in f for f in no_trap["failures"]))
        unobserved = rc.evaluate(case, self.evidence())
        self.assertTrue(any("no executable was built" in f for f in unobserved["failures"]))
        stopped = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 3, "out-of-bounds")]))
        self.assertEqual((stopped["status"], stopped["class"]), ("pass", "error"))

    def test_temporal_bug_is_satisfied_only_by_a_violation_trap(self):
        case = self.bug_case("BUG: use-after-free // TRAP: violation")
        at = str(case.path)
        result = rc.evaluate(case, self.evidence(built=True, ran=True, runs=[rc.Run((), -5)],
                                                 reports=[rc.CheckReport("violation", at, 6, 3)]))
        self.assertEqual((result["status"], result["class"]), ("pass", "trap"))
        index_trap = self.bug_case("BUG: use-after-free // TRAP: index")
        result = rc.evaluate(index_trap, self.evidence(built=True, ran=True, runs=[rc.Run((), -5)],
                                                       reports=[rc.CheckReport("index", at, 6, 3)]))
        self.assertTrue(any("BUG use-after-free not satisfied" in f for f in result["failures"]))

    def test_ledger_markers_and_rows(self):
        case = self.case("s/l.c", """
            int f(int *p) {
              return p[1]; // UNRESOLVED: spatial:unknown-extent // TRUSTED: temporal:caller-contract // NOT-PROVEN: null
            }
            """)
        facets = {"spatial": {"outcome": "unresolved", "reason": "unknown-extent"},
                  "temporal": {"outcome": "trusted", "reason": "caller-contract"},
                  "null": {"outcome": "checked", "check": {"template": "nonnull"}}}
        good = rc.evaluate(case, self.evidence(ledgers=[ledger(case.path, [site(2, **facets)])],
                                               ledger_expected=True))
        self.assertEqual(good["status"], "pass", good["failures"])
        proven = dict(facets, null={"outcome": "proven"}, spatial={"outcome": "unresolved", "reason": "budget"})
        bad = rc.evaluate(case, self.evidence(ledgers=[ledger(case.path, [site(2, **proven)])],
                                              ledger_expected=True))
        self.assertTrue(any("NOT-PROVEN null: the facet is proven" in f for f in bad["failures"]))
        self.assertTrue(any("found unresolved(budget)" in f for f in bad["failures"]))
        missing = rc.evaluate(case, self.evidence(ledger_expected=True))
        self.assertTrue(any("wrote no ledger" in f for f in missing["failures"]))
        empty = rc.evaluate(case, self.evidence(ledgers=[ledger(case.path, [])], ledger_expected=True))
        self.assertTrue(any("no ledger row with a null facet" in f for f in empty["failures"]))
        legacy = rc.evaluate(case, self.evidence(mode="legacy"))
        self.assertEqual(legacy["status"], "pass")

    def test_requirement_records_match_ledger_markers(self):
        case = self.case("s/r.c", "void g(char *d, char *s, int n) { f(d, s, n); } // UNRESOLVED: spatial:unknown-extent\n")
        facets = {"spatial": {"outcome": "checked", "requirements": [
            {"arg": 0, "outcome": "checked", "check": {"template": "len"}},
            {"arg": 1, "outcome": "unresolved", "reason": "unknown-extent"}]}}
        result = rc.evaluate(case, self.evidence(ledgers=[ledger(case.path, [site(1, **facets)])],
                                                 ledger_expected=True))
        self.assertEqual(result["status"], "pass", result["failures"])

    def test_bug_with_row_marker_is_satisfied_by_the_row(self):
        case = self.bug_case("BUG: use-after-free // NOT-PROVEN: temporal")
        row = ledger(case.path, [site(6, temporal={"outcome": "unresolved", "reason": "dangling-escape"})])
        result = rc.evaluate(case, self.evidence(ledgers=[row], ledger_expected=True))
        self.assertEqual((result["status"], result["class"]), ("pass", "row"))
        plain = self.bug_case()
        result = rc.evaluate(plain, self.evidence(ledgers=[ledger(plain.path, [site(6, temporal={
            "outcome": "unresolved", "reason": "dangling-escape"})])], ledger_expected=True))
        self.assertEqual((result["status"], result["class"]), ("fail", "row"))

    def test_no_emission_uses_checked_facets_and_neutralisation_counts_as_miss(self):
        case = self.case("s/n.c", """
            int main(int argc, char **argv) {
              int a[4] = {0}, i;
              if (argc > 5) i = 1;
              return a[argc + 3] + a[i]; // BUG: out-of-bounds // NEUTRALISED: zero-init
            }
            """)
        row = ledger(case.path, [site(4, spatial={"outcome": "checked", "check": {"template": "index"}})])
        result = rc.evaluate(case, self.evidence(ledgers=[row], ledger_expected=True, no_emission=True))
        self.assertEqual((result["status"], result["bugs"][0]["satisfiedBy"]), ("pass", "checked"))
        self.assertTrue(result["bugs"][0]["reported"])
        other = self.case("s/z.c", """
            int main(int argc, char **argv) {
              int t[4] = {0}, i;
              return t[i]; // BUG: use-of-uninitialized // NEUTRALISED: zero-init
            }
            """)
        on = rc.evaluate(other, self.evidence(zero_init=True, built=True, ran=True, runs=[rc.Run((), 0)]))
        self.assertEqual((on["status"], on["class"]), ("pass", "neutralised"))
        off = rc.evaluate(other, self.evidence(no_emission=True))
        self.assertEqual((off["status"], off["class"]), ("pass", "miss"))

    def test_miss_is_expected_silent(self):
        case = self.bug_case("BUG: use-after-free // MISS: aliasing through a union is not modelled")
        silent = rc.evaluate(case, self.evidence())
        self.assertEqual((silent["status"], silent["class"]), ("pass", "miss"))
        caught = rc.evaluate(case, self.evidence(diagnostics=[self.diag(case.path, 6)]))
        self.assertEqual((caught["status"], caught["class"]), ("pass", "error"))
        self.assertTrue(any("now reported" in n for n in caught["notes"]))

    def test_expect_ledger_prefers_the_program_ledger(self):
        case = self.case("s/e.c", """
            // EXPECT-LEDGER: /summary/errors == 2
            // EXPECT-LEDGER: /summary/warnings <= 0
            // EXPECT-LEDGER: /summary/missing == 1
            int main(void) { return 0; }
            """)
        unit = ledger(case.path, [], summary={"errors": 2, "warnings": 0})
        program = ledger(case.path, [], scope="program", summary={"errors": 1, "warnings": 0})
        only_unit = rc.evaluate(case, self.evidence(ledgers=[unit], ledger_expected=True))
        self.assertEqual(only_unit["failures"], ["EXPECT-LEDGER /summary/missing == 1: pointer not found"])
        both = rc.evaluate(case, self.evidence(ledgers=[unit, program], ledger_expected=True))
        self.assertIn("EXPECT-LEDGER /summary/errors == 2: actual 1", both["failures"])

    def test_verify_mode_failures(self):
        case = self.case("s/v.c", "// CLEAN\nint main(void) { return 0; }\n")
        result = rc.evaluate(case, self.evidence(mode="verify", proven_traps=["run x: weavec.proven trap"]))
        self.assertIn("run x: weavec.proven trap", result["failures"])

    def test_asan_oracle(self):
        clean = self.case("s/ok.c", "// CLEAN\n// ASAN\nint main(void) { return 0; }\n")
        report = rc.SanitizerReport("heap-use-after-free", (("ok.c", 3, "main"),))
        result = rc.evaluate(clean, self.evidence(asan_ran=True, asan=report))
        self.assertTrue(any("in a CLEAN case" in f for f in result["failures"]))
        case = self.bug_case(extra_file_markers="// ASAN")
        silent = rc.evaluate(case, self.evidence(asan_ran=True, diagnostics=[self.diag(case.path, 7)]))
        self.assertTrue(any("did not report the bug" in f for f in silent["failures"]))
        proven = ledger(case.path, [site(7, temporal={"outcome": "proven"})])
        g4 = rc.evaluate(case, self.evidence(
            asan_ran=True, asan=rc.SanitizerReport("heap-use-after-free", (("p.c", 7, "main"),)),
            diagnostics=[self.diag(case.path, 7)], ledgers=[proven], ledger_expected=True))
        self.assertTrue(any(f.startswith("G4: the temporal facet is proven") for f in g4["failures"]))
        self.assertEqual(g4["asan"]["site"], "p.c:7")

    def test_asan_site_proven_by_a_checked_call(self):
        case = self.case("s/call.c", """
            // ASAN
            static int get(int *p) { return p[10]; }
            int main(void) {
              int a[4] = {0};
              return get(a); // BUG: out-of-bounds
            }
            """)
        rows = ledger(case.path, [site(2, spatial={"outcome": "proven"}),
                                  site(5, spatial={"outcome": "violation"})])
        report = rc.SanitizerReport("stack-buffer-overflow", (("call.c", 2, "get"), ("call.c", 5, "main")))
        result = rc.evaluate(case, self.evidence(asan_ran=True, asan=report, ledgers=[rows], ledger_expected=True,
                                                 diagnostics=[self.diag(case.path, 5, "out-of-bounds")]))
        self.assertEqual(result["status"], "pass", result["failures"])

    def test_legacy_classes(self):
        case = self.bug_case()
        classes = {
            "CAUGHT": [self.diag(case.path, 6), self.diag(case.path, 4, "leak", "warning")],
            "MISLABEL": [self.diag(case.path, 6, "use-of-uninitialized"),
                         self.diag(case.path, 2, "analysis-incomplete", "warning")],
            "SIGNAL": [self.diag(case.path, 3, "annotation-required", "warning"),
                       self.diag(case.path, 4, "leak", "warning")],
            "LEAK-ONLY": [self.diag(case.path, 4, "leak", "warning")],
            "SILENT": [],
        }
        for expected, diagnostics in classes.items():
            with self.subTest(expected=expected):
                result = rc.evaluate(case, self.evidence(mode="legacy", diagnostics=diagnostics))
                self.assertEqual(result["legacyClass"], expected)
        wrong_line = rc.evaluate(case, self.evidence(mode="legacy", diagnostics=[self.diag(case.path, 5)]))
        self.assertEqual(wrong_line["legacyClass"], "MISLABEL")

    def test_marker_errors_make_an_error_status_and_summary(self):
        broken = self.case("s/b.c", "int x; // BUG: nonsense\n")
        good = self.case("s/g.c", "// CLEAN\nint main(void) { return 0; }\n")
        bug = self.bug_case()
        results = [rc.evaluate(broken, self.evidence()), rc.evaluate(good, self.evidence()),
                   rc.evaluate(bug, self.evidence(diagnostics=[self.diag(bug.path, 6)]))]
        self.assertEqual([r["status"] for r in results], ["error", "pass", "pass"])
        suite = rc.summarize(results)["suites"]["s"]
        self.assertEqual((suite["cases"], suite["passed"], suite["errors"]), (3, 2, 1))
        self.assertEqual((suite["cleanCases"], suite["cleanPassed"], suite["bugCases"]), (1, 1, 1))
        self.assertEqual((suite["pins"], suite["pinsSatisfied"], suite["classes"]["error"]), (1, 1, 1))


FAKE_CC = r'''#!{python}
"""A stand-in weavec-cc driven by <source>.fake.json next to each case file."""
import json, os, signal, sys
args = sys.argv[1:]
out = args[args.index("-o") + 1] if "-o" in args else "a.out"
sources = [a for a in args if a.endswith(".c")]
objects = [a for a in args if a.endswith(".o") and a != out]
ledger_dir = next((a.split("=", 1)[1] for a in args if a.startswith("-fweavec-ledger=")), None)
checks = next((a.split("=", 1)[1] for a in args if a.startswith("-fweavec-checks=")), "trap")
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(args) + "\n")
def spec_of(source):
    try:
        return json.load(open(source + ".fake.json"))
    except OSError:
        return {}
def emit(part, source):
    text = part.get("stderr", "").replace("{file}", source)
    sys.stderr.write(text)
    if ledger_dir and "ledger" in part:
        document = json.loads(json.dumps(part["ledger"]).replace("{file}", source))
        with open(os.path.join(ledger_dir, os.path.basename(out) + ".ledger.json"), "w") as f:
            json.dump(document, f)
    return part.get("rc", 0)
if "-fno-weavec" in args and "-c" not in args:
    spec = spec_of(sources[0])
    body = "import sys\nsys.stderr.write(%r)\nsys.exit(1 if %r else 0)\n" % (
        spec.get("asan", {}).get("stderr", "").replace("{file}", sources[0]), bool(spec.get("asan")))
elif "-c" in args:
    code = emit(spec_of(sources[0]).get("compile", {}), sources[0])
    if code == 0:
        open(out, "w").write(sources[0])
    sys.exit(code)
else:
    main = open(objects[0]).read()
    spec = spec_of(main)
    code = emit(spec.get("link", {}), main)
    if code:
        sys.exit(code)
    run = spec.get("run", {})
    if checks == "report":
        lines = "".join("weavec: runtime check failed: %s\n" % r.replace("{file}", main)
                        for r in run.get("report", []))
        body = "import sys\nsys.stderr.write(%r)\n" % lines
    else:
        trap = run.get("verify-trap" if checks == "verify" else "trap", run.get("trap", False))
        body = ("import os, signal\nos.kill(os.getpid(), signal.SIGTRAP)\n" if trap else "pass\n")
open(out, "w").write("#!" + sys.executable + "\n" + body)
os.chmod(out, 0o755)
'''

FAKE_WEAVEC = r'''#!{python}
"""A stand-in weavec: prints <source>.fake.json's '{key}' stderr for the first source."""
import json, sys
args = sys.argv[1:]
if "--strict-externs" in args and {reject}:
    sys.stderr.write("weavec: Unknown command line argument '--strict-externs'.\n")
    sys.exit(1)
sources = [a for a in args[:args.index("--")] if a.endswith(".c")]
try:
    spec = json.load(open(sources[0] + ".fake.json"))
except OSError:
    spec = {{}}
part = spec.get("{key}", {{}})
sys.stderr.write(part.get("stderr", "").replace("{{file}}", sources[0]))
sys.exit(part.get("rc", 0))
'''


class PipelineTest(Workspace):
    def setUp(self):
        super().setUp()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "log.txt"
        self.log.write_text("")
        os.environ["FAKE_LOG"] = str(self.log)
        self.addCleanup(os.environ.pop, "FAKE_LOG", None)
        self.cc = self.tool("weavec-cc", FAKE_CC.replace("{python}", sys.executable))
        self.weavec = self.tool("weavec", FAKE_WEAVEC.format(python=sys.executable, key="legacy", reject=False))
        self.golden = self.tool("golden/weavec", FAKE_WEAVEC.format(python=sys.executable, key="golden",
                                                                    reject=False))
        self.strict = self.tool("strict/weavec", FAKE_WEAVEC.format(python=sys.executable, key="legacy",
                                                                    reject=True))

    def tool(self, name, text):
        path = self.bin / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def spec(self, source, **parts):
        (Path(str(source) + ".fake.json")).write_text(json.dumps(parts))

    def run_main(self, *args):
        out = self.root / "results.json"
        with contextlib.redirect_stdout(io.StringIO()):
            code = rc.main(["--cases", str(self.cases), "--weavec", str(self.weavec), "--weavec-cc",
                            str(self.cc), "--json", str(out), "--jobs", "2", *args])
        return code, json.loads(out.read_text())

    def test_trap_mode_pipeline(self):
        trap = self.write("s/trap.c", """
            // RUN-INPUT: 1
            // ASAN
            int main(int argc, char **argv) {
              int a[4] = {0};
              return a[argc + 3]; // BUG: out-of-bounds // TRAP: index
            }
            """)
        unit_ledger = ledger("{file}", [site(5, spatial={"outcome": "checked", "check": {"template": "index"}})])
        self.spec(trap, compile={"ledger": unit_ledger},
                  run={"trap": True, "report": ["index at {file}:5:10"]},
                  asan={"stderr": "==1==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x1\n"
                                  "    #0 0x1 in main trap.c:5\n"})
        clean = self.write("s/clean.c", "// CLEAN\n// RUN-INPUT: a\n// RUN-INPUT: b\nint main(void) { return 0; }\n")
        self.spec(clean, compile={"ledger": ledger("{file}", [])})
        rogue = self.write("s/rogue.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.spec(rogue, run={"trap": True, "report": ["nonnull at {file}:2:1"]})
        code, results = self.run_main("--filter", "s/**")
        self.assertEqual(code, 1)
        by_case = {r["case"]: r for r in results["cases"]}
        self.assertEqual(by_case["s/trap.c"]["status"], "pass", by_case["s/trap.c"]["failures"])
        self.assertEqual(by_case["s/trap.c"]["class"], "trap")
        self.assertEqual(by_case["s/trap.c"]["asan"]["site"], "trap.c:5")
        self.assertEqual(by_case["s/clean.c"]["status"], "pass", by_case["s/clean.c"]["failures"])
        self.assertEqual(len(by_case["s/clean.c"]["runs"]), 2)
        self.assertEqual(by_case["s/rogue.c"]["status"], "fail")
        self.assertTrue(any("unexpected runtime check failure: nonnull" in f
                            for f in by_case["s/rogue.c"]["failures"]))
        log = self.log.read_text()
        self.assertIn("-fweavec-checks=trap", log)
        self.assertIn("-fweavec-checks=report", log)
        self.assertIn("-fweavec-ledger=", log)
        self.assertIn("-fno-weavec -fsanitize=address", log)
        suite = results["summary"]["suites"]["s"]
        self.assertEqual((suite["cases"], suite["passed"], suite["classes"]["trap"]), (3, 2, 1))

    def test_no_emission_and_require(self):
        case = self.write("s/n.c", """
            int main(int argc, char **argv) {
              int a[4] = {0};
              return a[argc + 3]; // BUG: out-of-bounds
            }
            """)
        self.spec(case, compile={"ledger": ledger("{file}", [site(3, spatial={"outcome": "checked"})])})
        code, results = self.run_main("--no-emission", "--require", "checked")
        self.assertEqual(code, 0, results["failures"])
        log = self.log.read_text()
        self.assertNotIn("-fweavec-checks=", log)
        self.assertIn("-fweavec-require=checked", log)

    def test_verify_mode_flags_a_proven_trap(self):
        case = self.write("s/v.c", "// CLEAN\nint main(void) { return 0; }\n")
        self.spec(case, run={"verify-trap": True, "trap": False, "report": []})
        code, results = self.run_main("--checks", "verify")
        self.assertEqual(code, 1)
        failures = results["cases"][0]["failures"]
        self.assertTrue(any("weavec.proven trap" in f for f in failures), failures)

    def test_legacy_and_compare_golden(self):
        bug = self.write("s/bug.c", """
            #include <stdlib.h>
            int main(void) { char *p = malloc(1); free(p); return p[0]; } // BUG: use-after-free
            """)
        line = "{file}:2:52: error: use of 'p' after it was freed [weavec::use-after-free]\n"
        self.spec(bug, legacy={"stderr": line, "rc": 1}, golden={"stderr": line, "rc": 1})
        silent = self.write("s/silent.c", """
            // FLAGS: -fweavec-strict
            #include <stdlib.h>
            int main(void) { char *p = malloc(1); free(p); return p[0]; } // BUG: use-after-free
            """)
        self.spec(silent, legacy={"stderr": "{file}:3:1: warning: 'p' is leaked [weavec::leak]\n"},
                  golden={"stderr": ""})
        code, results = self.run_main("--legacy")
        self.assertEqual(code, 1)
        classes = {r["case"]: r["legacyClass"] for r in results["cases"]}
        self.assertEqual(classes, {"s/bug.c": "CAUGHT", "s/silent.c": "LEAK-ONLY"})
        self.assertEqual(results["summary"]["suites"]["s"]["legacyClasses"]["CAUGHT"], 1)
        code, results = self.run_main("--compare-golden", "--golden-dir", str(self.golden.parent))
        self.assertEqual(code, 1)
        by_case = {r["case"]: r for r in results["cases"]}
        self.assertEqual(by_case["s/bug.c"]["status"], "pass")
        self.assertEqual(by_case["s/silent.c"]["status"], "fail")
        self.assertTrue(by_case["s/silent.c"]["failures"][0].startswith("+ tested: "))
        code, results = self.run_main("--compare-golden", "--golden-dir", str(self.golden.parent),
                                      "--weavec", str(self.strict))
        statuses = {r["case"]: r["status"] for r in results["cases"]}
        self.assertEqual(statuses, {"s/bug.c": "pass", "s/silent.c": "skip"})
        self.assertEqual(code, 0)

    def test_compare_golden_counts_repeated_diagnostics(self):
        case = self.write("s/dup.c", """
            #include <stdlib.h>
            int main(void) { char *p = malloc(1); free(p); return p[0]; } // BUG: use-after-free
            """)
        line = "{file}:2:52: error: use of 'p' after it was freed [weavec::use-after-free]\n"
        self.spec(case, legacy={"stderr": line * 2, "rc": 1}, golden={"stderr": line, "rc": 1})
        code, results = self.run_main("--compare-golden", "--golden-dir", str(self.golden.parent))
        self.assertEqual(code, 1)
        self.assertEqual(len(results["cases"][0]["failures"]), 1)
        self.assertTrue(results["cases"][0]["failures"][0].startswith("+ tested: "))

    def test_marker_errors_fail_the_run(self):
        self.write("s/bad.c", "int x; // TRAP: bounds\n")
        code, results = self.run_main()
        self.assertEqual(code, 1)
        self.assertEqual(results["cases"][0]["status"], "error")


if __name__ == "__main__":
    unittest.main()
