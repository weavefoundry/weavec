#!/usr/bin/env python3
"""Move hand-written Unreleased notes into the version selected by PSR."""

import datetime
import os
from pathlib import Path
import re


def main():
    version = os.environ["NEW_VERSION"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit(f"Unsupported release version: {version!r}")
    cmake = Path("CMakeLists.txt").read_text()
    if not re.search(rf"^  VERSION {re.escape(version)}$", cmake, re.MULTILINE):
        raise SystemExit("CMake version does not match the version selected by PSR")

    path = Path("CHANGELOG.md")
    text = path.read_text()
    marker = "## [Unreleased]\n"
    if text.count(marker) != 1 or f"## [{version}]" in text:
        raise SystemExit("Expected one Unreleased section and no existing release section")
    today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    text = text.replace(marker, f"{marker}\n## [{version}] - {today}\n", 1)
    url = "https://github.com/weavefoundry/weavec"
    text, count = re.subn(
        r"^\[Unreleased\]: .*?$",
        f"[Unreleased]: {url}/compare/v{version}...main",
        text,
        flags=re.MULTILINE,
    )
    if count != 1:
        raise SystemExit("Expected one Unreleased comparison link")
    text = text.rstrip() + f"\n[{version}]: {url}/releases/tag/v{version}\n"
    path.write_text(text)


if __name__ == "__main__":
    main()
