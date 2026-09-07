#!/usr/bin/env python3
"""RFC 0017: a crashed, timed-out or silently failing analyzer never passes recall."""
from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import recall


class RecallHarness(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "case.c"
        self.source.write_text("int x; // RECALL: out-of-bounds\n")
        self.diagnostic = f"{self.source}:1:1: error: test [weavec::out-of-bounds]\n"

    def run_analyzer(self, status: int, text: str) -> recall.Outcome:
        completed = subprocess.CompletedProcess([], status, "", text)
        with patch.object(recall.subprocess, "run", return_value=completed) as run:
            result = recall.judge(self.source, *recall.run_case(Path("weavec"), self.source, []))
            self.assertEqual(run.call_args.kwargs["timeout"], 30.0)
            return result

    def test_expected_error_exit(self) -> None:
        result = self.run_analyzer(1, self.diagnostic)
        self.assertTrue(result.ok)
        self.assertEqual(len(result.caught), 1)

    def test_signal_even_after_expected_output(self) -> None:
        for status in (-11, -6, 2, 127, 139):
            with self.subTest(status=status):
                result = self.run_analyzer(status, self.diagnostic)
                self.assertFalse(result.ok)
                self.assertFalse(result.caught)
                self.assertTrue(result.process_failures)

    def test_silent_failure_on_a_clean_case(self) -> None:
        self.source.write_text("int x;\n")
        result = self.run_analyzer(1, "")
        self.assertFalse(result.ok)
        self.assertTrue(result.process_failures)

    def test_success_after_error_is_inconsistent(self) -> None:
        result = self.run_analyzer(0, self.diagnostic)
        self.assertFalse(result.ok)
        self.assertFalse(result.caught)

    def test_clean_success(self) -> None:
        self.source.write_text("int x;\n")
        self.assertTrue(self.run_analyzer(0, "").ok)

    def test_timeout_discards_partial_diagnostic(self) -> None:
        expired = subprocess.TimeoutExpired("weavec", 0.1, stderr=self.diagnostic)
        with patch.object(recall.subprocess, "run", side_effect=expired):
            result = recall.judge(self.source, *recall.run_case(Path("weavec"), self.source, [], 0.1))
        self.assertFalse(result.ok)
        self.assertFalse(result.caught)
        self.assertIn("timed out", result.process_failures[0])

    def test_cannot_execute(self) -> None:
        with patch.object(recall.subprocess, "run", side_effect=PermissionError("denied")):
            result = recall.judge(self.source, *recall.run_case(Path("weavec"), self.source, []))
        self.assertFalse(result.ok)
        self.assertIn("could not run", result.process_failures[0])

    def test_same_basename_does_not_match_another_file(self) -> None:
        text = self.diagnostic.replace(str(self.source), str(self.source.parent / "other" / "case.c"))
        result = self.run_analyzer(1, text)
        self.assertFalse(result.ok)
        self.assertFalse(result.caught)
        self.assertTrue(result.clang_errors)

    def test_wrong_id_on_a_pinned_line_is_still_unexpected(self) -> None:
        result = self.run_analyzer(1, self.diagnostic + self.diagnostic.replace("out-of-bounds", "use-after-free"))
        self.assertFalse(result.ok)
        self.assertEqual(len(result.caught), 1)
        self.assertEqual(len(result.unexpected), 1)

    def test_clang_error_is_not_recall(self) -> None:
        result = self.run_analyzer(1, self.diagnostic + f"{self.source}:2:1: error: syntax error\n")
        self.assertFalse(result.ok)
        self.assertTrue(result.clang_errors)

    def test_warning_with_silent_failed_exit(self) -> None:
        self.source.write_text("int x;\n")
        result = self.run_analyzer(1, self.diagnostic.replace("error:", "warning:"))
        self.assertFalse(result.ok)
        self.assertTrue(result.process_failures)


if __name__ == "__main__":
    unittest.main()
