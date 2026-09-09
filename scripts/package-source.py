#!/usr/bin/env python3
"""Package an immutable release tag as source, with checksums and release notes."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="Release tag, e.g. v0.1.0")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.tag):
        parser.error("expected a vX.Y.Z release tag")
    version = args.tag[1:]
    prefix = f"weavec-{version}/"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    archive = args.output_dir / f"weavec-{version}-source.tar.gz"
    subprocess.run(
        [
            "git", "archive", "--format=tar.gz", f"--prefix={prefix}",
            f"--output={archive.resolve()}", f"refs/tags/{args.tag}",
        ],
        check=True,
    )
    with tarfile.open(archive) as source:
        for name in ("CMakeLists.txt", "CMakePresets.json", "README.md", "LICENSE",
                     "resources/include/weavec.h", "CHANGELOG.md"):
            if not source.getmember(prefix + name).isfile():
                raise SystemExit(f"Missing source file: {name}")
        cmake = source.extractfile(prefix + "CMakeLists.txt").read().decode()
        changelog = source.extractfile(prefix + "CHANGELOG.md").read().decode()
    if not re.search(rf"^  VERSION {re.escape(version)}$", cmake, re.MULTILINE):
        raise SystemExit("Tagged CMake version does not match the release tag")
    section = re.search(
        rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|^\[Unreleased\]:|\Z)",
        changelog,
        re.MULTILINE | re.DOTALL,
    )
    if not section:
        raise SystemExit("Tagged changelog has no entry for this release")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output_dir / "SHA256SUMS").write_text(f"{digest}  {archive.name}\n")
    notes = (
        f"# WeaveC {args.tag}\n\n"
        "An early C ownership and memory-safety checker with documented coverage limits.\n\n"
        "Download the source archive and SHA256SUMS below. Build with LLVM/Clang "
        "and CMake, configuring with `-DWEAVEC_VERSION_SUFFIX=\"\"`. "
        f"See the [build and install instructions](https://github.com/weavefoundry/weavec/"
        f"blob/{args.tag}/README.md#releases).\n\n"
        f"[Full changelog and migration notes](https://github.com/weavefoundry/weavec/"
        f"blob/{args.tag}/CHANGELOG.md)\n\n"
    )
    # The initial hand-written history exceeds GitHub's release-body limit.
    # Keep it in the tagged changelog; shorter later entries can appear inline.
    changes = section.group(1).strip()
    if len((notes + changes).encode()) <= 60000:
        notes += changes + "\n"
    (args.output_dir / "release-notes.md").write_text(notes)
    print(f"Verified {archive} (SHA-256 {digest})")


if __name__ == "__main__":
    main()
