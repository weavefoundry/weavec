#!/usr/bin/env python3
"""Check the release configuration and package release tags in disposable repositories."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class ConfigTest(unittest.TestCase):
    """release-please's configuration agrees with the version CMake declares."""

    def test_version_sources_agree(self):
        config = json.loads((ROOT / "release-please-config.json").read_text())
        manifest = json.loads((ROOT / ".release-please-manifest.json").read_text())
        package = config["packages"]["."]
        self.assertEqual(config["release-type"], "simple")
        self.assertEqual(package["changelog-path"], "CHANGELOG.md")
        self.assertIn("CMakeLists.txt", package["extra-files"])
        # feat and breaking changes raise the minor while below 1.0.
        self.assertTrue(config["bump-minor-pre-major"])
        self.assertFalse(config["bump-patch-for-minor-pre-major"])
        # The release stays a draft until the toolchains are checked, and its
        # tag exists from the start, so the next release PR finds it.
        self.assertTrue(config["draft"])
        self.assertTrue(config["force-tag-creation"])
        self.assertFalse(config["include-component-in-tag"])
        cmake = (ROOT / "CMakeLists.txt").read_text()
        marked = re.findall(r"^  VERSION (\S+) # x-release-please-version$", cmake, re.MULTILINE)
        self.assertEqual(marked, [manifest["."]])


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="weavec-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.env = dict(os.environ)
        for key in ("GITHUB_OUTPUT", "GH_TOKEN", "GITHUB_TOKEN", "GITHUB_ACTIONS"):
            self.env.pop(key, None)
        destination = self.repo / "scripts/package-source.py"
        destination.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "scripts/package-source.py", destination)
        for name in ("CMakePresets.json", "README.md", "LICENSE", "resources/include/weavec.h"):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Source fixture\n")
        self.run_command("git", "init", "-b", "main")
        self.run_command("git", "config", "user.name", "Release Test")
        self.run_command("git", "config", "user.email", "release-test@example.invalid")
        self.changelog = "# CHANGELOG\n\n\n## v0.1.0 (2026-01-01)\n\n### Features\n\n- Initial checker\n"
        self.release("0.1.0", "feat: initial checker")

    def run_command(self, *command, check=True):
        result = subprocess.run(command, cwd=self.repo, env=self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if check and result.returncode:
            self.fail(f"{command!r} failed:\n{result.stdout}")
        return result

    def release(self, version, message, entry=None):
        """Commit what a merged release PR leaves: the stamped version and changelog."""
        (self.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24...3.31)\n"
            f"project(WeaveC\n  VERSION {version} # x-release-please-version\n  LANGUAGES C CXX)\n"
        )
        if entry:
            # release-please adds each entry above the newest one.
            first = self.changelog.index("\n## ") + 1
            self.changelog = f"{self.changelog[:first]}{entry}\n\n\n{self.changelog[first:]}"
        (self.repo / "CHANGELOG.md").write_text(self.changelog)
        self.run_command("git", "add", ".")
        self.run_command("git", "commit", "-m", message)
        self.run_command("git", "tag", f"v{version}")

    def package(self, tag, check=True):
        return self.run_command(sys.executable, "scripts/package-source.py", tag,
                                "--output-dir", "dist", check=check)

    def test_earlier_heading(self):
        # Packaging uses the tag, even with modified and untracked local files.
        (self.repo / "build").mkdir()
        (self.repo / "build/secret.txt").write_text("Do not distribute\n")
        (self.repo / "README.md").write_text("Uncommitted change\n")
        self.package("v0.1.0")
        notes = (self.repo / "dist/release-notes.md").read_text()
        self.assertTrue(notes.startswith("## v0.1.0 ("))
        self.assertIn("initial checker", notes.lower())
        self.assertNotIn("Detailed Changes", notes)
        checksum = (self.repo / "dist/SHA256SUMS").read_bytes()
        with tarfile.open(self.repo / "dist/weavec-0.1.0-source.tar.gz") as archive:
            self.assertNotIn("weavec-0.1.0/build/secret.txt", archive.getnames())
            self.assertEqual(archive.extractfile("weavec-0.1.0/README.md").read(),
                             b"Source fixture\n")
        # A retry reproduces the same archive.
        self.package("v0.1.0")
        self.assertEqual((self.repo / "dist/SHA256SUMS").read_bytes(), checksum)

    def test_release_please_headings(self):
        compare = "https://github.com/weavefoundry/weavec/compare"
        self.release("0.2.0", "chore(release): v0.2.0",
                     f"## [0.2.0]({compare}/v0.1.0...v0.2.0) (2026-02-01)\n\n\n"
                     "### Features\n\n* add contracts ([#2](https://example.invalid/2))")
        self.release("0.2.1", "chore(release): v0.2.1",
                     f"## [0.2.1]({compare}/v0.2.0...v0.2.1) (2026-02-02)\n\n\n"
                     "### Bug Fixes\n\n* handle null ([#3](https://example.invalid/3))")
        self.package("v0.2.1")
        notes = (self.repo / "dist/release-notes.md").read_text()
        self.assertTrue(notes.startswith(f"## [0.2.1]({compare}/v0.2.0...v0.2.1) ("))
        self.assertIn("handle null", notes)
        self.assertNotIn("add contracts", notes)
        self.assertNotIn("Detailed Changes", notes)
        self.package("v0.2.0")
        notes = (self.repo / "dist/release-notes.md").read_text()
        self.assertIn("add contracts", notes)
        self.assertNotIn("handle null", notes)
        self.assertNotIn("initial checker", notes.lower())
        self.assertTrue((self.repo / "dist/weavec-0.2.0-source.tar.gz").exists())

    def test_reject_mismatched_tag(self):
        self.run_command("git", "tag", "v9.9.9")
        result = self.package("v9.9.9", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Tagged CMake version does not match the release tag", result.stdout)
        self.assertFalse((self.repo / "dist/SHA256SUMS").exists())

    def test_reject_missing_entry(self):
        (self.repo / "CMakeLists.txt").write_text(
            "project(WeaveC\n  VERSION 0.3.0 # x-release-please-version\n  LANGUAGES C CXX)\n")
        self.run_command("git", "commit", "-am", "chore(release): v0.3.0")
        self.run_command("git", "tag", "v0.3.0")
        result = self.package("v0.3.0", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Tagged changelog has no entry for this release", result.stdout)


class FormulaTest(unittest.TestCase):
    """RFC 0038 section 6: the Homebrew formula names each platform's archive and checksum."""

    def test_render(self):
        import hashlib
        import importlib.util
        spec = importlib.util.spec_from_file_location("render_formula", ROOT / "scripts" / "render-formula.py")
        render = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(render)
        with tempfile.TemporaryDirectory() as directory:
            dist = Path(directory)
            sums = {}
            for target in ("darwin-arm64", "linux-x86_64", "linux-aarch64"):
                archive = dist / f"weavec-1.2.3-{target}.tar.xz"
                archive.write_bytes(target.encode())
                sums[target] = hashlib.sha256(target.encode()).hexdigest()
            formula = render.render("1.2.3", dist)
            self.assertIn('version "1.2.3"', formula)
            for target, digest in sums.items():
                self.assertIn(f"/v1.2.3/weavec-1.2.3-{target}.tar.xz", formula)
                self.assertIn(f'sha256 "{digest}"', formula)
            (dist / "weavec-1.2.3-linux-aarch64.tar.xz").unlink()
            with self.assertRaises(SystemExit):
                render.render("1.2.3", dist)

if __name__ == "__main__":
    unittest.main()
