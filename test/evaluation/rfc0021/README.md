# RFC 0021 evaluation

The accepted RFC freezes 18 positive/negative pairs in `manifest.json` and
the unchanged upstream selections in `real-modules.json` before checker
implementation. Additional tests supplement these populations. Expectations
must not be weakened to accommodate missing coverage.

Frozen SHA-256:

| File | SHA-256 |
| --- | --- |
| `manifest.json` | `6e34c6a0c212fdabfad58f80b1cfc47ad10d281e577bed4db4c5ed100e9d10f3` |
| `real-modules.json` | `951c3c72b5ee84424f7391db64e3606c9713a309c770b49b1c376e9f984614f5` |
| `callers.json` | `cd983030597d5470e00df76ea46992a7cc381af404a348e03e7b3685c92d8421` |

```sh
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0021/manifest.json --json build/rfc21-cases.json
```

Every selected closed caller must have zero entry requirements. Positive
results require complete contracts without limits, deferral or unsafe/annotation
trust. Negative cases must retain the intended property; parse/tool failures,
timeouts and absent reports never satisfy them. The real interfaces require
unchanged upstream bodies at the recorded revisions and complete contracts
for every named selected definition, in addition to their closed callers.

`callers.json` adds three closed callers for each upstream module, frozen
before implementing the UTF and string traversal support. The real-source
runner verifies revisions, unchanged source/header bytes and caller hashes:

```sh
python3 scripts/checked-traversal.py --weavec build/dev/bin/weavec \
  --corpus-root build/corpus --json build/rfc21-real.json
```
