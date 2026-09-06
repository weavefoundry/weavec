#!/usr/bin/env python3
"""Failure-mode tests for the fixed evaluation runner (RFC 0013)."""
import json
import tempfile
import unittest
from pathlib import Path

from evaluate import Bug, Case, classify, load_manifest, run_case, summarize


class EvaluationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="weavec-evaluation-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "case.c"
        self.source.write_text("void f(void) { int a[1]; a[1] = 0; } // BUG: overflow\n")
        self.bug = Bug("overflow", "out-of-bounds", self.source, 1, True)
        self.case = Case("bad", (self.source,), (self.bug,), False, ())

    def diagnostic(self, kind="out-of-bounds", severity="error", line=1):
        return f"{self.source}:{line}:5: {severity}: message [weavec::{kind}]\n"

    def test_known_miss_stays_in_denominator_and_new_detection_is_improvement(self):
        bug = Bug("overflow", "out-of-bounds", self.source, 1, False)
        case = Case("unsupported", (self.source,), (bug,), False, ())
        missing = classify(case, "", "", 0)
        caught = classify(case, "", self.diagnostic(), 1)
        self.assertTrue(missing["ok"])
        self.assertTrue(caught["ok"])
        self.assertEqual(summarize([missing])["bugs"], 1)
        self.assertEqual(summarize([missing])["missed"], 1)
        self.assertEqual(summarize([caught])["detected"], 1)

    def test_wrong_diagnostic_at_right_line_does_not_count(self):
        result = classify(self.case, "", self.diagnostic("double-free"), 1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["detected"], [])
        self.assertEqual(result["required_misses"], ["overflow"])
        self.assertEqual(len(result["unexpected"]), 1)

    def test_warnings_are_checked_and_leaks_can_be_expected_bugs(self):
        good = Case("good", (self.source,), (), False, ())
        result = classify(good, "", self.diagnostic("leak", "warning"), 0)
        self.assertFalse(result["ok"])
        leak = Bug("leak", "leak", self.source, 1, True)
        bad = Case("leak", (self.source,), (leak,), False, ())
        self.assertTrue(classify(bad, "", self.diagnostic("leak", "warning"), 0)["ok"])

    def test_parse_failure_does_not_count_partial_detections(self):
        output = self.diagnostic() + f"{self.source}:2:1: error: expected ';'\n"
        result = classify(self.case, "", output, 1)
        self.assertFalse(result["ok"])
        self.assertTrue(result["parse_errors"])
        self.assertFalse(result["tool_failure"])
        self.assertEqual(result["detected"], [])
        self.assertEqual(result["missed"], ["overflow"])

    def test_crash_or_silent_failure_is_not_clean(self):
        good = Case("good", (self.source,), (), False, ())
        for code in [-11, 1, 2]:
            result = classify(good, "", "", code)
            self.assertFalse(result["ok"])
            self.assertTrue(result["tool_failure"])

    def test_timeout_is_distinct_from_failure_and_from_missed_bug(self):
        tool = self.root / "sleeping-tool"
        tool.write_text("#!/bin/sh\nsleep 2\n")
        tool.chmod(0o755)
        result = run_case(self.case, tool, 0.01)
        self.assertFalse(result["ok"])
        self.assertTrue(result["timeout"])
        self.assertFalse(result["tool_failure"])
        self.assertEqual(result["missed"], ["overflow"])

    def test_missing_binary_is_a_tool_failure(self):
        result = run_case(self.case, self.root / "no-tool", 1)
        self.assertTrue(result["tool_failure"])
        self.assertFalse(result["timeout"])

    def test_manifest_rejects_erasing_bug_markers_and_duplicate_cases(self):
        manifest = self.root / "manifest.json"
        entry = {"name": "case", "sources": ["case.c"],
                 "bugs": [{"marker": "overflow", "diagnostic": "out-of-bounds"}]}
        manifest.write_text(json.dumps({"version": 1, "cases": [entry]}))
        self.assertEqual(load_manifest(manifest)[0].bugs[0].line, 1)
        manifest.write_text(json.dumps({"version": 1, "cases": [entry, entry]}))
        with self.assertRaises(ValueError):
            load_manifest(manifest)
        del entry["bugs"]
        manifest.write_text(json.dumps({"version": 1, "cases": [entry]}))
        with self.assertRaises(ValueError):
            load_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
