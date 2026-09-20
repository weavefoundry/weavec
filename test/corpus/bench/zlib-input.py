#!/usr/bin/env python3
"""Generate the zlib input of the corpus gate's overhead benchmark.

RFC 0030, gate G14 times zlib's minigzip compressing and then decompressing
this file. The data is deterministic: lines of words drawn from a fixed
vocabulary, with 1 KiB of pseudo-random bytes after every 63 KiB of text,
so deflate does both match finding and literal coding. The same seed gives
the same bytes with any Python 3.9 or later (random.Random, choices and
randbytes are reproducible for a given seed). The SHA-256 of the output is
printed, and --sha256 checks it.

  zlib-input.py OUTPUT [--mib 64] [--sha256 HEX]
"""

from __future__ import annotations

import argparse
import hashlib
import random
import sys
from pathlib import Path

MIB = 1 << 20
TEXT_BLOCK = 63 * 1024
RANDOM_BLOCK = 1024


def vocabulary(rng: random.Random) -> list[str]:
    letters = "etaoinshrdlucmfwypvbgkjqxz"
    weights = [26 - i for i in range(len(letters))]
    words = set()
    while len(words) < 4096:
        length = rng.randint(2, 11)
        words.add("".join(rng.choices(letters, weights=weights, k=length)))
    return sorted(words)


def generate(path: Path, mib: int, seed: int = 2026) -> str:
    rng = random.Random(seed)
    vocab = vocabulary(rng)
    digest = hashlib.sha256()
    remaining = mib * MIB
    with path.open("wb") as out:
        while remaining > 0:
            words = rng.choices(vocab, k=TEXT_BLOCK // 5)
            lines = []
            for start in range(0, len(words), 12):
                lines.append(" ".join(words[start:start + 12]))
            text = ("\n".join(lines) + "\n").encode("ascii")[:TEXT_BLOCK]
            block = text + rng.randbytes(RANDOM_BLOCK)
            block = block[:remaining]
            out.write(block)
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mib", type=int, default=64, help="size in MiB (default 64)")
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--sha256", help="fail unless the output has this SHA-256")
    args = parser.parse_args(argv)
    if args.mib <= 0:
        parser.error("--mib must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sha = generate(args.output, args.mib, args.seed)
    print(f"{args.output}: {args.mib} MiB, sha256 {sha}")
    if args.sha256 and args.sha256 != sha:
        print(f"error: expected sha256 {args.sha256}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
