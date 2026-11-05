#!/usr/bin/env python3
"""Tests for scripts/check-spelling.py (American English throughout).

The word tests call the rules directly; RepositoryTest makes a small git
repository in a temporary directory to check what is scanned and fixed.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("check_spelling", HERE / "check-spelling.py")
spelling = importlib.util.module_from_spec(SPEC)
sys.modules["check_spelling"] = spelling
SPEC.loader.exec_module(spelling)


class WordTest(unittest.TestCase):
    def test_british_forms_have_american_ones(self) -> None:
        cases = {
            "behaviour": "behavior", "behaviours": "behaviors",
            "neighbouring": "neighboring", "favourite": "favorite",
            "misbehaviour": "misbehavior", "dishonour": "dishonor",
            "initialise": "initialize", "uninitialised": "uninitialized",
            "reinitialisation": "reinitialization", "optimiser": "optimizer",
            "visualise": "visualize", "emphasise": "emphasize",
            "recognisable": "recognizable", "analyse": "analyze",
            "unanalysed": "unanalyzed", "labelled": "labeled",
            "mislabelled": "mislabeled", "licence": "license",
            "judgement": "judgment", "learnt": "learned",
        }
        for british, american in cases.items():
            with self.subTest(british=british):
                self.assertEqual(spelling.american(british), american)

    def test_case_is_kept(self) -> None:
        self.assertEqual(spelling.american("Behaviour"), "Behavior")
        self.assertEqual(spelling.american("OPTIMISER"), "OPTIMIZER")
        self.assertEqual(spelling.american("Learnt"), "Learned")

    def test_words_spelled_alike_are_left(self) -> None:
        # "analyses" is also the plural of "analysis".
        for word in ("analysis", "analyses", "emphasis", "optimism",
                     "optimistic", "realistic", "specialist", "criticism",
                     "promise", "precise", "otherwise", "fulfill", "hour",
                     "your", "four", "detour", "flour", "called", "filled"):
            with self.subTest(word=word):
                self.assertEqual(list(spelling.findings(word)), [])

    def test_identifiers_are_checked_at_each_hump(self) -> None:
        found = [hit[2] for hit in spelling.findings(
            "zeroInitialises(); weavecRtInitialise(); initialiseEarly; normalise_path")]
        self.assertEqual(found, ["Initialises", "Initialise", "initialise", "normalise"])

    def test_allowed_text_is_left(self) -> None:
        text = "see docs/rfcs/0009-value-conditional-behaviour.md on behaviour"
        self.assertEqual([hit[2] for hit in spelling.findings(text)], ["behaviour"])
        self.assertEqual(spelling.fixed(text),
                         "see docs/rfcs/0009-value-conditional-behaviour.md on behavior")

    def test_findings_name_the_line_and_column(self) -> None:
        self.assertEqual(list(spelling.findings("ok\n  a grey colour\n")),
                         [(2, 5, "grey", "gray"), (2, 10, "colour", "color")])

    def test_fixed_rewrites_every_word(self) -> None:
        self.assertEqual(
            spelling.fixed("The optimiser's behaviour: zeroInitialises, labelled."),
            "The optimizer's behavior: zeroInitializes, labeled.")


class RepositoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        (self.root / ".gitignore").write_text("ignored.txt\n")
        (self.root / "notes.md").write_text("The optimiser's behaviour.\n")
        (self.root / "CHANGELOG.md").write_text("- behaviour\n")
        (self.root / "ignored.txt").write_text("behaviour\n")
        (self.root / "blob.bin").write_bytes(b"behaviour\0\1\2")

    def run_main(self, *arguments: str) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = spelling.main(["--root", str(self.root), *arguments])
        return status, out.getvalue(), err.getvalue()

    def test_reports_only_scanned_files(self) -> None:
        status, out, _ = self.run_main()
        self.assertEqual(status, 1)
        self.assertEqual(out.splitlines(), [
            "notes.md:1:5: optimiser -> optimizer",
            "notes.md:1:17: behaviour -> behavior",
        ])

    def test_fix_rewrites_and_then_passes(self) -> None:
        status, _, _ = self.run_main("--fix")
        self.assertEqual(status, 0)
        self.assertEqual((self.root / "notes.md").read_text(), "The optimizer's behavior.\n")
        self.assertEqual((self.root / "CHANGELOG.md").read_text(), "- behaviour\n")
        self.assertEqual(self.run_main()[0], 0)

    def test_outside_a_work_tree_is_an_error(self) -> None:
        outside = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, outside)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            status = spelling.main(["--root", str(outside)])
        self.assertEqual(status, 2)
        self.assertIn("cannot list the files", err.getvalue())


if __name__ == "__main__":
    unittest.main()
