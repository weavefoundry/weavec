# RFC 0023 validation

Status: implemented. All frozen, preservation, sanitizer, cache and performance
gates in [RFC 0023](rfcs/0023-inductive-container-contracts.md) pass.
[Machine-readable results](../scripts/corpus/rfc0023-results.json) record
source/executable identities, frozen inputs, every acceptance case and measured
observation. [Diagnostic changes](../scripts/corpus/rfc0023-diagnostics.jsonl)
retain each added or removed diagnostic. Full canonical diagnostic sets and raw
reports remain under `build/rfc23-validation/`, with hashes in the results.

## Scope and evidence

The implementation starts from `8c75d4b` and adds finite-chain predicates,
read/write/release capabilities, saved successors, fresh and derived outputs,
separation and conservative mutation invalidation. It preserves ordinary
initialization, integer, lifetime, unsupported-call and leak obligations.
The [evaluation notes](../test/evaluation/rfc0023/README.md) distinguish primary
frozen cases, supplemental adversarial cases, separate compiler objects and
unchanged full-source cJSON clients. No annotation or runtime instrumentation
is added. Summary format 18 and sidecar format 19 require older objects to be
rebuilt.

The original 24 source cases were frozen before checker changes. Baseline
satisfies 12/24 expectations, all negatives. The separate-source population
satisfies 8/16 at baseline; the unchanged cJSON population satisfies 4/8.
Positive closed callers need zero entry requirements and no unsafe or
annotation trust. A timeout, syntax error or unrelated failure cannot satisfy
a negative expectation.

## Development observations

The primary source population now passes 24/24. All 23 supplemental adversarial
cases and 16 separate-source cases pass. The compiler population passes all
16 expectations: 14 reach link checking and two are rejected during compilation.
The early caller rejection is checked use-after-free; the helper rejection
requires both an ordinary hard diagnostic and its matching computed checked
violation. They are not reported as 16 successful link tests.

Eight unchanged cJSON clients pass on the initial Release candidate. Their
actual definitions come from commit
`fb16e5cf358798aabb049655975cde8427101056`; hashes are checked against both the
working tree and the commit. Requested constructed-list lengths from zero to
1,048,576 all check with 11 function analyses. Runtime allocations are not
executed by that scaling test.

Core's independent graph oracle checks 24,000 explicit states. A separate
release-order oracle checks 9,216 payload ownership/liveness combinations,
including shared payloads and node/payload overlap. Analysis tests
cover all 64 three-node topologies and 24 direct/helper relinks after an initial
traversal. Every cycle is rejected; six valid arbitrary pointer-output helper
relinks conservatively lose their link relation. The direct relinks remain
exact within that population. Other tests cover capabilities, endpoints,
payload ownership, joins, descriptor limits and malformed metadata.

The transport generator originally reused good helper definitions for two
negative forms. It was corrected before transport-specific implementation;
both inventories and both baseline runs are retained. A supplemental generic
relink fixture was changed to an actual closed aliasing client, since a generic
function may legitimately export a separation premise. The primary frozen
population was never changed.

An early Debug attempt at the full cJSON population exceeded a 120-second
per-case development timeout and was interrupted. That run is retained as a
failed attempt, not counted as correctness evidence. The Release runner uses
the RFC's 600-second bound and completes all eight cases. Initial full-suite
failures also exposed stale format-version assertions and validation-harness
handling of legitimate early compiler rejection; those are retained in the
local development logs.

Final alias review found a real false proof in the development candidate:
`audit/saved-tail-payload.c` read a saved byte pointer into a later node's payload
after destroying the chain. The original complete report and pre-fix executable
are preserved under `build/rfc23-validation/pre-alias-fix/`. An independently
compiled ASan witness allocates two separated nodes and payloads satisfying the
entry premises and reports heap-use-after-free. Quantified release now retires
native payload aliases as well as container holders, including copies and
joined paths. It preserves ordinary ownership records so leak obligations
remain. All seven supplemental generic audit cases pass Debug, Release and
sanitizer validation.

A subsequent callback audit reproduced the same false proof through resolved
function pointers. The pre-fix Debug executable and complete reports are
preserved under `build/rfc23-validation/pre-callback-fix/`; a second independent
ASan witness confirms the invalid read. Six additional callback cases cover
singleton and multiple targets, head-only versus whole-chain consumption, and
safe reads/reassignment. Invalidation now applies to each returning target
before joining states. Both complete suites were rerun after this correction.
Their earlier passes are development evidence, not validation of the callback
fix.

Further review tightened two conservative frame rules: fresh outputs from the
same call do not imply mutual separation, and equality of a folded allocation
identity cannot prove that a native alias belongs to a preserved region.
Aliased constructor-output probes reject, with an additional unit regression.
The prior traversal evaluation then caught two false ordinary null diagnostics
in cJSON's parser, despite the selected minifier contracts remaining complete.
An empty container certificate had overwritten ordinary assignment nullness.
That interaction was removed, and conflicting captured empty certificates are
discarded. The unchanged upstream population remains a required final gate.

The corrected checker passed all 1,144 CTest entries in both Debug and
ASan/UBSan builds, all source/object/cache populations, the prior interface and
traversal evaluations, and strict clang-tidy on 19 changed translation units.
The first sequential Release cost run then timed out on Lua at 600 seconds;
it produced no complete Lua report and is recorded as a failed observation.
The four smaller projects completed, retaining their 76 baseline-complete
definitions. That partial result does not establish preservation of all 124.

Performance review found unnecessary place resolution during candidate
discovery, container lookup when no shapes or facts exist, and output-witness
creation for ordinary pointer assignments with no container proof. The fast
paths remove that work while retaining invalidation latches and retiring
existing output witnesses when their holder loses its proof. Final validation
was rerun on this optimized source; the 600-second and 1.10 cost limits
were unchanged. The initial failed run remains under
`build/rfc23-validation/checked-timeout-before-optimization/`.
The optimized uncached Lua analysis completes in 535.127 seconds with a
6,432,505,856-byte peak RSS. All five reports validate and all 124 exact
baseline-complete identities are retained; 130 selected contracts now complete.

## Remaining model limits

A derived output guarantees that its nodes came from stated input chains or
fresh allocations; it does not promise that all original nodes reach the output.
Consequently, a wrapper such as `destroy(reverse(p))` can propagate a sufficient
ownership requirement while its closed caller still reports an ordinary leak.
Suppressing that leak from a subset relation would be unsound for transformations
that drop nodes. Whole-footprint consumption inference is separate future work.

Descriptors currently require initialization of all named record fields.
Arbitrary pointer-output relinks, indirect container outputs, graphs, cyclic
ownership, arbitrary trees, doubly linked mutation, volatile/atomic links and
concurrency retain conservative limits. Passing a conditional generic contract
is not proof that an arbitrary caller supplied a valid chain.

## Final gates

The final uncached checked observation passes the unchanged 600-second bound
for every project. These are conditional contracts, with their actual entry
requirements preserved in the reports; the remaining selected functions stay
explicitly incomplete.

| Project | Selected | Baseline complete | Final complete | Seconds |
| --- | ---: | ---: | ---: | ---: |
| log.c | 12 | 5 | 5 | 0.579 |
| cJSON | 151 | 25 | 29 | 17.472 |
| linenoise | 88 | 24 | 24 | 9.557 |
| Jansson | 211 | 22 | 22 | 64.759 |
| Lua | 1,157 | 48 | 50 | 535.127 |
| Total | 1,619 | 124 | 130 | 627.494 |

The four cJSON gains are `cJSON_GetArraySize`, `cJSON_GetArrayItem`, and
`get_array_item` in both cJSON.c and cJSON_Utils.c. The two additional Lua gains
are `findpcall` and `udata2finalize`. Those Lua gains were observed in the corpus,
not part of the frozen primary or unchanged-cJSON acceptance populations.
Their exported premises remain explicit: `findpcall` needs a live initialized
`CallInfo` chain; `udata2finalize` needs writable, separated `allgc` and
`tobefnz` chains along with its ordinary validity and field-access requirements.
These contracts do not verify the garbage collector's lifecycle as a whole.

Both complete suites pass on the optimized source: 1,144/1,144 CTest entries
in Debug and 1,144/1,144 under ASan/UBSan. Each suite includes 1,122 unit tests
and 22 integration entries; lit runs 122 cases within one integration entry.
Prior unchanged traversal clients pass 8/8 and interface clients pass 5/5.
The older memory population retains its baseline 7/8 result: the
`strbuffer-lifecycle` case still has incomplete `strbuffer_append_byte` and
`strbuffer_append_bytes` contracts, while its other previously complete
interfaces remain complete. That existing miss is preserved in the results.

All five uncached, cold-cache and warm-cache reports are canonically equivalent.
Warm runs reuse all 51 units with zero function analyses. Lua's cold-cache run
completes in 553.356 seconds; its warm run takes 48.613 seconds. Report and
checkpoint handling still use substantial memory: Lua peaks at 8,451,506,176
bytes cold and 8,144,961,536 bytes warm. Reuse eliminates function dataflow,
but loading and rendering the large checked result remains a cost.

Three sequential observations use the preserved baseline and final Release
executables with the same compiler/options. Builds, tests and lint were stopped
for these measurements.

| Ordinary analysis | Baseline | Final |
| --- | ---: | ---: |
| Run 1, seconds | 288.741 | 188.616 |
| Run 2, seconds | 359.230 | 236.483 |
| Run 3, seconds | 170.851 | 191.906 |
| Median seconds | 288.741 | 191.906 |
| Median peak RSS, bytes | 649,478,144 | 665,927,680 |

The runtime ratio is 0.665 and the peak-RSS ratio is 1.025; both pass the 1.10
limits. The wide baseline timing range limits interpretation of the runtime
ratio as a speed improvement. All observations, including the slower runs, are
retained. Ordinary diagnostics are identical between baseline and final and
across all three repetitions. Checked diagnostics are identical across
uncached, cold-cache and warm-cache runs.

Comparison with the historical RFC 0022 checked observation uses the same
preserved executable and corpus manifest, and is separate from fresh timing
measurements. No ordinary diagnostic ids disappear. Checked diagnostics change
as follows; additional requirements can increase incomplete-call diagnostics
even while more helper contracts complete.

| Project | Added incomplete | Removed incomplete | Added failed checks |
| --- | ---: | ---: | ---: |
| cJSON | 154 | 88 | 0 |
| Jansson | 96 | 7 | 0 |
| Lua | 1,711 | 33 | 1 |

log.c and linenoise have no checked-diagnostic changes. The additional failed
check is a read-only-storage rejection at Lua's `newcheckedkey` call after
`rehash` (`ltable.c:919`), in an already incomplete function. This remains a
checker precision issue around table growth and is retained for follow-up.
No previously complete selected contract is lost, and no existing failed-check
diagnostic is removed.
