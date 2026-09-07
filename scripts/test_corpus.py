#!/usr/bin/env python3
"""RFC 0014: corpus setup and tool failures must never look like clean code."""
import contextlib
import io
import tempfile
import sys
import unittest
from pathlib import Path

from corpus import Project, UnitResult, compare_to_baseline, main, run_units, summarise


class CorpusTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-corpus-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "case.c"
        self.source.write_text("void f(void) {}\n")
        self.project = Project("sample", None, "local", ["*.c"], [])

    def fake(self, body):
        tool = self.root / "checker"
        tool.write_text("#!/usr/bin/env python3\n" + body + "\n")
        tool.chmod(0o700)
        return str(tool)

    def run_tool(self, body, **kwargs):
        return run_units(self.fake(body), self.project, self.root, [self.source], [], **kwargs)

    def test_clean_completion(self):
        result = self.run_tool("pass")
        self.assertEqual(result.exit_code, 0)
        self.assertFalse(result.failure)
        self.assertFalse(result.diagnostics)

    @unittest.skipUnless(sys.platform == "darwin" or sys.platform.startswith("linux"),
                         "resource accounting requires macOS or Linux")
    def test_memory_measurement_reports_the_checker(self):
        result = self.run_tool("storage = bytearray(1024 * 1024)", measure_memory=True)
        self.assertFalse(result.failure)
        self.assertGreater(result.peak_rss_bytes, 0)

    def test_an_ownership_error_is_a_checked_result(self):
        result = self.run_tool(
            f"import sys\nprint({str(self.source)!r} + ':1:1: error: bad access [weavec::use-after-free]', file=sys.stderr)\nsys.exit(1)"
        )
        self.assertFalse(result.failure)
        self.assertEqual(len(result.diagnostics), 1)

    def test_parse_error_is_a_failure_even_when_the_tool_exits_zero(self):
        result = self.run_tool(f"print({str(self.source)!r} + ':1:1: error: unknown type')")
        self.assertEqual(result.clang_errors, 1)
        self.assertTrue(result.failure)

    def test_tool_failure_without_ownership_error(self):
        result = self.run_tool("raise SystemExit(1)")
        self.assertTrue(result.failure)
        self.assertFalse(result.diagnostics)

    def test_crash_is_not_a_clean_result(self):
        result = self.run_tool("import os, signal\nos.kill(os.getpid(), signal.SIGTERM)")
        self.assertTrue(result.failure)
        self.assertLess(result.exit_code, 0)

    def test_iteration_limit_warning_is_a_failed_analysis(self):
        result = self.run_tool(
            f"print({str(self.source)!r} + ':1:1: warning: analysis is incomplete: summary iteration limit reached [weavec::analysis-incomplete]')"
        )
        self.assertEqual(result.failure, "analysis reached an iteration limit")

    def test_timeout_is_reported(self):
        result = self.run_tool("import time\ntime.sleep(10)", timeout=0.05)
        self.assertIn("timeout", result.failure)
        self.assertLess(result.seconds, 2)

    def test_missing_binary_is_reported(self):
        result = run_units(str(self.root / "missing"), self.project, self.root, [self.source], [])
        self.assertTrue(result.failure)

    def test_missing_projects_and_changed_unit_counts_fail_comparison(self):
        baseline = {"projects": {"sample": {"units": 2}}, "totals": {}}
        summary = summarise([UnitResult("sample", "case.c", 0, 0, [], 0)])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare_to_baseline(summary, baseline), 1)
            self.assertEqual(compare_to_baseline(summarise([]), baseline), 1)

    def test_failure_count_prevents_a_clean_comparison(self):
        summary = summarise([UnitResult("sample", "case.c", 0, 1, [], 0, failure="crash")])
        self.assertEqual(summary["projects"]["sample"]["failures"], 1)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare_to_baseline(summary, {"totals": {}}), 1)

    def test_empty_source_glob_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(["--local", str(self.root), "--local-files", "missing-*.c"]), 2)


if __name__ == "__main__":
    unittest.main()
