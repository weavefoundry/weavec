# The golden oracle

RFC 0030 (§17.1) measures parity against WeaveC v0.10.0. The `weavec` and
`weavec-cc` binaries built from that release are the *golden* binaries.
`scripts/run-cases.py --legacy` and `--compare-golden` run them, and so does
`scripts/corpus-gate.py`. They are not committed.

| | |
| --- | --- |
| Commit | `e0e2bd68bcae0bf6f7396a35a415db42afa3f0e6` (`chore(release): v0.10.0`) |
| Binaries | `weavec`, `weavec-cc` (`weavec --version` prints `e0e2bd68bcae`) |
| LLVM | 23 (the release was built with LLVM 23.1.0) |
| Location | the directory named by `WEAVEC_GOLDEN_DIR` |

## Building them

Build the binaries in a separate checkout of the release, outside this tree,
with the `release` preset:

```sh
git worktree add ../weavec-golden e0e2bd6     # or any directory outside this checkout
cd ../weavec-golden
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset release
cmake --build --preset release
export WEAVEC_GOLDEN_DIR="$PWD/build/release/bin"
```

## Which `weavec.h` they read

The binaries look for `weavec.h` in three places, in this order:

1. `$WEAVEC_RESOURCE_DIR/include`;
2. `<bin>/../lib/weavec/include`;
3. the `resources/include` directory of the checkout they were built from.

Keep that checkout, and do not build the golden binaries inside this tree.
Otherwise they would read this branch's `weavec.h`, whose annotation surface
RFC 0030 changes, and the golden run would no longer be v0.10.0's. If you
copy the binaries elsewhere, set `WEAVEC_RESOURCE_DIR` to the release
checkout's `resources` directory.

## Using them

```sh
WEAVEC_GOLDEN_DIR=../weavec-golden/build/release/bin \
  python3 scripts/run-cases.py --legacy
```

In S0, `--legacy` must reproduce v0.10.0 on this tree:

- evaluation: 44/44 bugs and 32/32 clean;
- pairs: 24/24;
- recall: 67/67;
- soundness probes: 36 caught, 42 silent, 4 signal, 1 leak-only, 2 mislabelled;
- engine pins: 100%.
