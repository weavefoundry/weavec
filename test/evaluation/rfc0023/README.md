# RFC 0023 acceptance populations

The primary manifest freezes 24 closed callers: 12 valid/invalid pairs for
borrowed traversal, empty and singleton chains, runtime construction, reversal,
concatenation, head detachment, saved aliases, borrowed-node release,
independent ownership, allocation failure, and owned payload cleanup.
`frozen-sha256.json` was written before checker changes. Those sources and the
manifest have not been changed to fit the implementation. The preserved
`8c75d4b` Release baseline satisfies 12/24 expectations: every negative and
none of the positives.

Positive callers require zero entry requirements, no limit or deferral, and
no unsafe or annotation trust. Negative outcomes need a matching missing
safety property; a syntax error, crash or timeout is never a success.

```sh
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0023/manifest.json
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0023/adversarial/manifest.json
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0023/transport/manifest.json
python3 scripts/checked-containers.py --population objects \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc \
  --output build/rfc23-validation/objects
python3 scripts/checked-containers.py --population cache \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc \
  --output build/rfc23-validation/cache
```

The supplemental transport population splits eight primary pairs into separate
caller/helper units. The sources retain their original definitions apart from
external linkage. A generator initially reused the good helper for the reverse
and allocation-failure negatives; it was corrected before transport-specific
implementation. `transport/superseded-freeze.json` preserves the earlier hashes
and reason. Both original and corrected baseline runs satisfy 8/16 expectations.
This is a fixture correction, not an implementation gain.

Compiler validation builds each source independently and verifies sidecars.
Fourteen cases reach selected link-time checking. `detach-bad` is rejected in
the selected caller during compilation, and `failure-bad` produces an ordinary
hard use-after-free error in its helper before an object exists. The latter
also requires a matching computed checked violation in the report. These two
are recorded as early rejections, not successful link checks.

The supplemental adversarial cases cover changed and aliased links, unknown
calls, writes through unrelated helper arguments, pointer arithmetic, byte
corruption, const and misaligned storage, partially initialized loops, cached
allocation aliases, shared owned payloads and consumed payload aliases. They
were added during implementation and are not part of the original freeze.
`generic-link-unknown` was corrected to a closed aliased caller: a generic
relink function can legitimately export a separation premise, so its conditional
contract is not by itself a false proof.

The separate `audit/` population preserves a false proof found during final
review: a generic function saved an initialized payload pointer from a later
node, destroyed the chain, then read the saved pointer. The pre-fix candidate
reported a complete conditional contract. `audit/runtime-witness.c` constructs
two distinct allocated nodes with distinct initialized payloads satisfying its
premises; ASan independently reports heap-use-after-free. Seven frozen audit
cases cover the saved alias, copying, conditional release, a second release,
an independently passed alias, reading before release and fresh reassignment.
These are generic conditional contracts, separate from the closed primary
population. The five unsafe variants must reject; the two safe variants retain
explicit entry requirements. They were added after discovery of the defect,
not counted as a baseline improvement.

`callback-audit/` adds six cases after the same defect was reproduced through
resolved function pointers, including a singleton target and targets with
different consumption footprints. Its runtime witness also independently
reports heap-use-after-free. Four unsafe cases must reject and two safe cases
must retain complete conditional contracts. Callback invalidation is applied
to each returning target before joining; dropping container outputs alone
cannot retire a saved native byte pointer.

Core tests enumerate 24,000 three-node states across successor values,
initialization, liveness and starting node using an independent integer graph
oracle. A separate concrete release-order oracle checks 9,216 two-node
payload states, including shared payloads, node/payload overlap and missing
release capabilities. Analysis tests generate all 64 concrete three-node topologies and 24
subsequent direct/helper relinks. Direct relinks are exact in this population;
arbitrary pointer-output helpers conservatively lose some valid link relations.
Both forms reject every cyclic topology. Additional checks cover endpoint
exclusion, ownership, shared payloads, joins, limits and malformed transport.

## Unchanged upstream source

The supplemental upstream population contains four positive/negative cJSON
pairs: `cJSON_GetArraySize`, `get_array_item` in `cJSON.c`, its public
`cJSON_GetArrayItem` forwarding operation, and `get_array_item` in
`cJSON_Utils.c`. Each caller includes the actual full implementation file;
none substitutes a rewritten helper. cJSON is pinned at
`fb16e5cf358798aabb049655975cde8427101056`.

These callers were frozen after the first local traversal hooks, before
upstream-specific work. They are explicitly separate from the primary freeze.
The preserved baseline satisfies 4/8 expectations, all negatives. The runner
checks both the source hashes and their equality to the upstream commit.

```sh
python3 scripts/corpus.py --weavec build/rfc23-release/bin/weavec --only cJSON-program \
  --manifest scripts/corpus/rfc0015.json
python3 scripts/checked-containers.py --population upstream \
  --weavec build/rfc23-release/bin/weavec \
  --output build/rfc23-validation/upstream --timeout 600
python3 scripts/checked-containers.py --population scaling \
  --weavec build/rfc23-release/bin/weavec \
  --output build/rfc23-validation/scaling
```

The scaling population generates identical construction/traversal/cleanup
programs for requested list lengths 0, 1, 8, 64, 1,024 and 1,048,576. It checks
proof work rather than executing a million-node allocation.

`baseline-complete-functions.json` records the exact 124 complete selected
identities from RFC 0022's final corpus report, with its source artifact and
executable hashes. Corpus preservation compares identities, not aggregate
counts. See [validation](../../../docs/validation-rfc0023.md) for results,
commands, measured cost and remaining limitations.
