#!/usr/bin/env python3
"""Rehearse real semantic releases in disposable repositories without a remote push."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="weavec-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.env = dict(os.environ)
        # Do not let PSR write into the parent CI step's outputs or use its token.
        for key in ("GITHUB_OUTPUT", "GH_TOKEN", "GITHUB_TOKEN", "GITHUB_ACTIONS"):
            self.env.pop(key, None)
        for name in (".releaserc.toml", "scripts/package-source.py"):
            destination = self.repo / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24...3.31)\n"
            "project(WeaveC\n  VERSION 0.1.0\n  LANGUAGES C CXX)\n"
        )
        self.original_note = "### Added\n\n- A checker with documented limits.\n"
        (self.repo / "docs").mkdir()
        (self.repo / "docs/development-history.md").write_text(self.original_note)
        for name in ("CMakePresets.json", "README.md", "LICENSE", "resources/include/weavec.h"):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Source fixture\n")
        self.run_command("git", "init", "-b", "main")
        self.run_command("git", "config", "user.name", "Release Test")
        self.run_command("git", "config", "user.email", "release-test@example.invalid")
        self.run_command("git", "remote", "add", "origin", "https://github.com/weavefoundry/weavec.git")
        self.run_command("git", "add", ".")
        self.commit("feat: initial checker")

    def run_command(self, *command, check=True):
        result = subprocess.run(command, cwd=self.repo, env=self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if check and result.returncode:
            self.fail(f"{command!r} failed:\n{result.stdout}")
        return result

    def commit(self, message):
        self.run_command("git", "commit", "--allow-empty", "-m", message)

    def release(self):
        return self.run_command(
            "semantic-release", "-c", ".releaserc.toml", "version",
            "--no-push", "--no-vcs-release",
        )

    def package(self, tag):
        return self.run_command(sys.executable, "scripts/package-source.py", tag,
                                "--output-dir", "dist")

    def test_versions_archives_and_retry(self):
        self.assertFalse((self.repo / "CHANGELOG.md").exists())
        self.release()
        self.assertEqual(self.run_command("git", "describe", "--exact-match").stdout.strip(), "v0.1.0")
        changelog = (self.repo / "CHANGELOG.md").read_text()
        self.assertIn("initial checker", changelog.lower())
        self.assertNotIn(self.original_note, changelog)
        self.assertIn("## v0.1.0 (", changelog)
        self.assertEqual((self.repo / "docs/development-history.md").read_text(), self.original_note)
        self.assertIn("cmake_minimum_required(VERSION 3.24...3.31)",
                      (self.repo / "CMakeLists.txt").read_text())

        # Packaging must use the tag, even with modified and untracked local files.
        (self.repo / "build").mkdir()
        (self.repo / "build/secret.txt").write_text("Do not distribute\n")
        (self.repo / "README.md").write_text("Uncommitted change\n")
        self.package("v0.1.0")
        notes = (self.repo / "dist/release-notes.md").read_text()
        self.assertTrue(notes.startswith("## v0.1.0 ("))
        self.assertIn("initial checker", notes.lower())
        self.assertNotIn("Detailed Changes", notes)
        self.assertNotIn("An early C ownership", notes)
        self.assertNotIn("Download the source archive", notes)
        self.assertNotIn("Generated changelog", notes)
        self.assertNotIn("development-history.md", notes)
        checksum = (self.repo / "dist/SHA256SUMS").read_bytes()
        with tarfile.open(self.repo / "dist/weavec-0.1.0-source.tar.gz") as archive:
            self.assertNotIn("weavec-0.1.0/build/secret.txt", archive.getnames())
            self.assertEqual(archive.extractfile("weavec-0.1.0/README.md").read(), b"Source fixture\n")
            self.assertEqual(archive.extractfile("weavec-0.1.0/docs/development-history.md").read(),
                             self.original_note.encode())
        self.run_command("git", "restore", "README.md")
        head = self.run_command("git", "rev-parse", "HEAD").stdout
        output = self.repo / "github-output"
        self.env["GITHUB_OUTPUT"] = str(output)
        self.release()
        self.assertEqual(self.run_command("git", "rev-parse", "HEAD").stdout, head)
        self.assertIn("released=false\n", output.read_text())
        self.assertIn("tag=v0.1.0\n", output.read_text())
        self.package("v0.1.0")
        self.assertEqual((self.repo / "dist/SHA256SUMS").read_bytes(), checksum)

        previous_version = "0.1.0"
        for message, version in (("fix: handle null", "0.1.1"),
                                 ("perf: reuse summaries", "0.1.2"),
                                 ("feat: add contracts", "0.2.0"),
                                 ("feat!: change sidecar format", "0.3.0")):
            with self.subTest(message=message):
                self.commit(message)
                self.release()
                self.assertEqual(self.run_command("git", "describe", "--exact-match").stdout.strip(),
                                 "v" + version)
                self.assertIn(f"  VERSION {version}\n", (self.repo / "CMakeLists.txt").read_text())
                changelog = (self.repo / "CHANGELOG.md").read_text()
                self.assertIn(f"## v{version} (", changelog)
                self.assertIn("initial checker", changelog.lower())
                self.package("v" + version)
                notes = (self.repo / "dist/release-notes.md").read_text()
                self.assertTrue(notes.startswith(f"## v{version} ("))
                self.assertIn(message.split(": ", 1)[1], notes.lower())
                self.assertNotIn("initial checker", notes.lower())
                self.assertIn(f"/compare/v{previous_version}...v{version}", notes)
                previous_version = version
        self.commit("docs: explain installation")
        head = self.run_command("git", "rev-parse", "HEAD").stdout
        self.release()
        self.assertEqual(self.run_command("git", "rev-parse", "HEAD").stdout, head)
        self.assertEqual((self.repo / "docs/development-history.md").read_text(), self.original_note)

    def test_reject_mismatched_tag(self):
        self.release()
        self.run_command("git", "tag", "v9.9.9")
        result = self.run_command(sys.executable, "scripts/package-source.py", "v9.9.9",
                                  "--output-dir", "dist", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Tagged CMake version does not match the release tag", result.stdout)
        self.assertFalse((self.repo / "dist/SHA256SUMS").exists())

    def test_long_initial_history(self):
        path = self.repo / "docs/development-history.md"
        path.write_text(self.original_note * 2000)
        self.run_command("git", "add", "docs/development-history.md")
        self.commit("docs: retain detailed migration history")
        self.release()
        self.package("v0.1.0")
        notes = (self.repo / "dist/release-notes.md").read_text()
        self.assertLess(len(notes.encode()), 60000)
        self.assertTrue(notes.startswith("## v0.1.0 ("))
        self.assertIn("initial checker", notes.lower())
        self.assertNotIn("development-history.md", notes)
        self.assertNotIn(self.original_note, notes)
        self.assertGreater(len(path.read_bytes()), 60000)
        self.assertLess(len((self.repo / "CHANGELOG.md").read_bytes()), 60000)
        self.assertNotIn(self.original_note, (self.repo / "CHANGELOG.md").read_text())


if __name__ == "__main__":
    unittest.main()
