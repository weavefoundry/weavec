# RFC 0027 evaluation

`manifest.json` contains 27 primary cases, including generic traversal and
destruction contracts. `upstream/` contains seven clients of unchanged pinned
cJSON. These populations and their SHA-256 inventories were frozen before the
checker changed. The baseline executable and failed baseline observations are
retained in the validation artifacts.

`transport/` contains twenty mechanically extracted separate-source variants of
the frozen primary clients, with the same expectations. Compiler-object tests
compile and link those same files through `weavec-cc`. These variants were
derived during implementation; `transport/provenance.json` records the method.

`oracle/` contains 43 cases generated independently from concrete allocation
graphs: thirteen valid forests and thirty invalid variants. The generator
exhaustively examines all 4,096 three-node binary topologies to find the ten
rooted ownership trees, adds empty/singleton/independent-root cases, then mutates
each three-node tree by omitting cleanup, sharing a root or introducing a cycle.
Its DFS tracks actual allocation identities and requires each acquired node to
be released exactly once. These files were frozen during implementation before
the first checker run. To reproduce them without rewriting frozen evidence:

```sh
python3 scripts/generate-recursive-oracle.py --output /tmp/weavec-oracle
```

Run each population with:

```sh
python3 scripts/checked-recursive.py --population source \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc \
  --output build/rfc27-source
```

Other populations are `transport`, `objects`, `cache`, `oracle`, `scaling`,
`upstream` and `upstream-objects`. The six populations from `source` through
`scaling` run under CTest. Upstream requires the pinned
`build/corpus/cJSON-program` checkout; the runner verifies both commit and source
hashes. It checks full unchanged library definitions and explicitly established
default hooks, without a trusted cJSON summary. `upstream-objects` compiles and
links the same unchanged clients through compiler sidecars; their included
upstream definitions remain in the original translation unit.

Positive closed clients require zero entry requirements, no missing proof and no
new annotation/unsafe trust. Generic helpers have explicit expected requirements.
Syntax errors, missing selections/reports, crashes and timeouts are failures of
the evaluation, never successful negative tests. Full source reports are retained
as gzip files alongside selected-function results. Each run records its executable
hash. Cache tests compare expanded, compact, warm and uncached results, invalidate
changed destructor bodies, and reject corrupt/stale metadata. Runtime-size tests
vary a constructor argument from zero to 1,048,576 while retaining one helper
body and recording analysis work.

See [the RFC](../../../docs/rfcs/0027-recursive-object-ownership.md) and
[validation](../../../docs/validation-rfc0027.md) for acceptance gates and results.
