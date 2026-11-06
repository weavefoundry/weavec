#!/usr/bin/env python3
"""Render packaging/homebrew/weavec.rb.in for a release (RFC 0038, section 6).

usage: render-formula.py VERSION DIST_DIR [--base-url URL] [--output FILE]

DIST_DIR holds the release archives weavec-<version>-<os>-<arch>.tar.xz for
darwin-arm64, linux-x86_64 and linux-aarch64; their sha256 checksums go into
the formula with the URLs they are published at (by default the GitHub
release of tag v<version>).
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "packaging" / "homebrew" / "weavec.rb.in"
PLATFORMS = ("darwin-arm64", "linux-x86_64", "linux-aarch64")


def render(version: str, dist: Path, base_url: str | None = None) -> str:
    base = base_url or f"https://github.com/weavefoundry/weavec/releases/download/v{version}"
    text = TEMPLATE.read_text().replace("@VERSION@", version)
    for target in PLATFORMS:
        name = f"weavec-{version}-{target}.tar.xz"
        archive = dist / name
        if not archive.is_file():
            raise SystemExit(f"render-formula: {archive} is missing")
        key = target.upper().replace("-", "_")
        text = text.replace(f"@URL_{key}@", f"{base}/{name}")
        text = text.replace(f"@SHA256_{key}@", hashlib.sha256(archive.read_bytes()).hexdigest())
    if "@" in "".join(line for line in text.splitlines() if not line.lstrip().startswith("#")):
        raise SystemExit("render-formula: a placeholder was left in the formula")
    return text


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version")
    parser.add_argument("dist", type=Path)
    parser.add_argument("--base-url")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    formula = render(args.version.removeprefix("v"), args.dist, args.base_url)
    if args.output:
        args.output.write_text(formula)
    else:
        sys.stdout.write(formula)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
