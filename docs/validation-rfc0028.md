# RFC 0028 validation

RFC 0028 is implemented. Correctness, corpus preservation, cache replay and
isolated cost gates pass. The [machine-readable evidence](../scripts/corpus/rfc0028-results.json)
records frozen input hashes, executable identities, complete-contract
preservation, test results and retained development observations.

## Scope

Supported opaque library objects retain their inferred representation, lifetime,
initialization and ownership contracts across public headers. Private static
storage, including nested allocation hooks, integer configuration cells and
fixed arrays, has a portable declaration identity and validated type description.
Actual initialization and call effects establish its values. A declaration,
cast, layout description or callback prototype alone grants no proof.

The implementation transports this evidence through separate source units,
compiler objects and persistent checkpoints. It replaces the private callback
proxy special case. Summary format 23, sidecar format 24 and checkpoint format 3
reject older artifacts; users must rebuild them. Checked encoding remains 9.
There are no new annotations, diagnostic identifiers or runtime checks.

Opaque chains, supported buffers and finite ownership forests retain their
existing structural and allocation-footprint obligations. A new verified
`allocation-consumed` output accounts for one entry allocation released by a
wrapper, independently of the stronger whole-container consumption output.
This does not authorize dropping child allocations. Scalar array writes forget
possibly overlapping cell facts while preserving proven disjoint cells.

## Frozen populations

The initial 24 clients and five public-header cJSON clients were frozen before
checker changes. Independent regression and follow-up populations were added
during implementation and frozen separately. Their manifests and SHA-256
inventories remain separate; later cases are not presented as initial coverage.
The source and compiler-object populations use the same frozen C files.

| Population | Baseline outcomes met | Final expectations met |
| --- | ---: | ---: |
| Initial separate-source clients | 12/24 | 24/24 |
| Initial clients through ordinary objects and checked linking | Not measured | 24/24 |
| Cache equivalence and invalidation | Not measured | 10/10 |
| Independent regressions and follow-ups | Not measured | 16/16 |
| Independent regressions through compiler objects | Not measured | 16/16 |
| Unchanged cJSON public-header clients | 2/5 | 5/5 |
| Unchanged cJSON through separate compiler objects | Not measured | 5/5 |

The initial population has thirteen positives and eleven negatives. The baseline
accepts two positives and rejects ten negatives, but incorrectly accepts the
disabled-private-state skipped-cleanup counterexample. The final checker adds
eleven closed positive proofs, preserves the two existing positives, and rejects all
eleven negatives for their intended property. Baseline rejection counts measure
outcomes only: several baseline cases stop at an unsupported interface before
reaching their intended counterexample.

The independent population contains seven positives and nine negatives across
thirteen regression cases and three follow-ups. It exercises same-spelled
private roots in different modules, local statics, integer arrays, forwarding,
wrapper callbacks, independent object cleanup, nullable targets, unknown state
mutation and buffer failure outcomes. A head-only release wrapper cannot certify
cleanup of its children. Disjoint configuration writes preserve an existing
object's lifetime and cleanup evidence.

Every successful closed client has zero entry requirements, no deferral or
analysis limit, and no annotation or unsafe trust. Existing modeled C-library
contracts remain explicit in the reports. Negative expectations require their
intended violated or unresolved property. Syntax failures, crashes, timeouts
and absent reports do not count as successful rejections.

## Unchanged cJSON

The implementation and public header come from commit
`fb16e5cf358798aabb049655975cde8427101056`. Both files are checked against the
pinned Git contents and their frozen hashes before each population runs.
Clients include only `cJSON.h`; `cJSON.c` is analyzed and compiled separately.
The three positives cover an empty object, nested ownership and supported
custom allocation hooks. The negatives cover double deletion and a stale borrow.
No cJSON-specific trusted contract or modified upstream definition is used.

The compiler-object population uses the existing unselected checked-report
workflow to infer and serialize library contracts. Compiling `cJSON.c` with
`-fweavec-checked-report=library-contracts.json` selects zero functions, then
the checked link selects the closed client `main`. Incomplete generic library
functions remain visible in the compile report; this is not whole-library
certification. The initial frozen object population additionally verifies
ordinary compilation followed by checked linking.

An earlier cJSON object attempt used ordinary compilation and stopped on four
existing ordinary null-dereference diagnostics in unrelated `cJSON_Create*Array`
functions. That failed observation is retained. The final object workflow uses
the documented unselected checked-report mode without warning suppression or
upstream edits. Parser/printer certification remains outside this change.

## Correctness checks

Candidate 35 passes all 1,346 CTest entries in Debug in 630.41 seconds and all
1,346 entries under ASan/UBSan in 587.11 seconds. Each suite includes 1,299 unit tests and
47 integration entries, including all 134 lit tests and all five registered
RFC 0028 populations: 90 case expectations per configuration. Candidate 29
previously passed all 1,337 entries in both configurations.
Suite times are validation observations, separate from the isolated cost gates.
The immutable candidate-35 Release executables also pass all 50 source, independent-regression
and unchanged-upstream expectations, including separate cJSON compiler objects.

The new typed-layout reuse regression approves repeated reads from initialized
live storage and rejects repeated reads after release or without initialization.
The existing erased-pointer regression also rechecks changed object views between
calls. Reusing a static layout result grants no flow-sensitive memory permission.

The unchanged fixed evaluation detects all 44 bugs and accepts all 32 clean
cases, with no parse failures, tool failures, timeouts or unexpected reports.
Warnings-as-errors builds use LLVM 23. All 57 changed C++ translation units pass
strict clang-tidy; checks were rerun after subsequent source edits. C++ and CMake
formatting and the Core prohibition on Clang/LLVM includes pass.

Debug uses `-O0 -gline-tables-only` with assertions enabled. The sanitizer build
uses `-O1 -gline-tables-only`, ASan/UBSan and assertions. Both retained baseline
and final Release builds use matching `-O3 -DNDEBUG` and ThinLTO settings.
The final harness checks executable hashes before every invocation and retains
full reports as checksum-verified gzip archives. Positive object cases must
produce an executable, and every compiled unit must produce its sidecar.

## Corpus preservation and cost

Candidate 35 passes the cold/warm gate. All 148 valid baseline-complete identities are preserved among the same 1,619 selected
definitions; two newly expressible conditional contracts bring the total to 150.
Every cold invocation finishes within 600 seconds with a valid complete report.
No complete function carries an iteration limit. Full normalized reports match
between cold and warm runs, with 51 warm unit hits and zero function analyses.
Ordered diagnostics also match for every project. All five final reports match
the reviewed candidate-27 and candidate-29 semantic reports exactly.

| Project | Baseline complete | Final complete / selected | Cold seconds | Warm seconds | Warm unit hits |
| --- | ---: | ---: | ---: | ---: | ---: |
| log.c | 5 | 5 / 12 | 0.219 | 0.145 | 1 |
| cJSON | 32 | 33 / 151 | 29.785 | 0.872 | 2 |
| linenoise | 27 | 27 / 88 | 16.469 | 0.855 | 2 |
| Jansson | 24 | 24 / 211 | 115.936 | 2.497 | 12 |
| Lua | 60 | 61 / 1,157 | 552.014 | 18.788 | 34 |

Lua still reports function-level iteration limits in incomplete generic
contracts; these are explicit incomplete coverage, never complete proofs.
Its final invocation finishes normally with the expected checked-diagnostic
status. Earlier timeouts remain failed observations below. Compact cJSON and
linenoise reports are 6.04 and 5.09 times smaller than their expanded versions.

Candidate 35 passes the ordinary comparison. Three sequential baseline runs
measure 141.779, 141.892 and 141.442 seconds; the final runs measure 150.539,
150.485 and 152.012 seconds. Median total time rises from 141.779 to 150.539
seconds (1.0618×). Median peak RSS rises from 597,032,960 to 649,920,512 bytes
(1.0886×). Both ratios meet the unchanged 1.10 limits. All thirty project
observations finish without tool or parse failures. Builds, tests and profiling
were stopped before measurement.

The two newly complete generic contracts retain every baseline obligation
exactly, including outcomes, trust and explanations. `cJSON_GetErrorPtr` retains
six obligations and now expresses validity and pointer-formation extent
requirements on private `global_error.json` and `.position`. Lua's `print_usage`
retains 42 obligations, its existing argument requirements and library trust,
and adds a terminated-string requirement on private `progname`. These are
conditional contracts, separate from the closed-client proofs above.

## Development observations

Candidate 29 fails its ordinary comparison: baseline and final median times are
141.885 and 172.744 seconds (1.2175×); median peak RSS is 599,539,712 and
786,956,288 bytes (1.3126×). Candidate 30 restricts the additional copy-only
callback setter requests to checked-contract analysis, retaining ordinary
symbolic forwarding and the existing indirect-call requests. Its isolated Lua
probe improves peak RSS to 675,725,312 bytes, but takes 163.244 seconds and
remains above target. These observations precede the final passing measurement.

Candidate 20's first isolated cold run completed log.c, cJSON, linenoise and
Jansson in 0.211, 32.681, 18.018 and 132.715 seconds respectively, preserving
every baseline-complete identity in those projects. Lua timed out at 600.159
seconds without a completed report, so that candidate failed the cost gate.
Warm replay completed for the first four projects; the redundant Lua retry was
interrupted after the failed cold observation. These results remain separate
from final acceptance measurements.

A CPU profile identified repeated analysis-place resolution during typed object
path validation. Candidate 21 resolves that chain only when recovering actual
opaque evidence, while continuing to check every type prefix. Its frozen source
and independent regression expectations, focused Debug/sanitizer interface tests
and strict lint pass. Its four completed cold reports are unchanged from candidate
20; the run was interrupted during Lua while a further optimization was prepared.
Candidate 22 additionally reuses fully typed layout validation for an exact call
and path under the same live immutable summary. Opaque evidence and memory
permissions are excluded, and the cache evicts at 1,024 paths. The failed and
partial observations, profile and subsequent measurements are retained; the
600-second deadline and semantic budgets are unchanged.

Candidate 22 completed the first four projects in 0.224, 32.532, 17.838 and
127.333 seconds, with reports identical to candidate 20. Lua again timed out,
at 600.245 seconds without a completed report. Its partial warm replay is
retained separately. Candidate 23 additionally transfers finished specialization
summaries and diagnostic vectors into publication storage instead of deep-copying
them before destroying their producers. The source and independent regression
populations remain fully passing after this change.

Candidate 23's first four reports are also identical, but Lua timed out at
600.161 seconds. Candidate 24 replaces whole-index clearing in the prepared
call-explanation cache with least-recently-used eviction, retaining its
1,024-entry and 64 MiB bounds. A new unit comparison covers entry pressure,
byte pressure, hot-entry reuse and replacement of the immutable source
projection. Its first test budget could hold only one large preparation; that
failed observation is retained. The corrected test uses 1 MiB and enough
distinct calls to force eviction, and all 32 focused cache/ledger tests pass.
Candidate 24 completes the first four projects in 0.219, 32.160, 17.702 and
124.590 seconds with unchanged reports, but Lua still times out at 600.180
seconds. Candidate 25 compares only the first differing serialized route
component, avoids rescanning unrelated specializations after a current
publication, and joins numeric outputs without a redundant deep snapshot.
A differential route-ordering test covers escaped filenames, numeric prefixes,
shared prefixes and both insertion and ledger joins.

Candidate 25 preserves the same four reports but times out on Lua at 600.293
seconds. Candidate 26 changes only implementation costs: place lookup uses
owned hashed keys and non-owning probes, sorted effect maps merge with a moving
insertion position, and null-entry accounting reuses pointer-parameter places.
New comparisons cover map joins, place-table growth, borrowed lookup text and
independent table copies.

Candidate 26 preserves those reports but Lua times out at 600.320 seconds,
with 44,608 analyses recorded in its last progress snapshot. Candidate 27
shares immutable summary-path steps and prefixes, detaches on edits and
preserves exact ordering. A focused benchmark shows cheaper path copies without
slower creation; it remains separate from corpus acceptance. Differential tests
cover borrowed elements, self-append, prefixes, source destruction and moves.
The first build caught one remaining union-path caller using the old vector
API; that caller was updated. A test intentionally inspecting the guaranteed
empty moved-from state has a documented, narrowly scoped lint exception.

Candidate 27 completes the five cold projects in 0.201, 30.143, 16.921,
117.705 and 592.961 seconds. The reports preserve all 148 baseline-complete
identities and add two, with no complete function carrying an iteration limit.
Lua checkpoint publication fails producer round-trip validation, however, so
its warm invocation reanalyzes functions and is interrupted. This is a failed
cache acceptance observation, despite passing cold coverage and elapsed time.
A separate instrumented run investigates that failure; it is excluded from
acceptance cost.

The diagnostic identifies 40 callback requests for `lua_pushcclosure`; the
reader incorrectly applied the 32 computed-specialization limit to accumulated
request metadata. RFC 0028 now specifies a separate 65,536-request transport
bound per symbol and kind, retaining the existing analysis limits. New tests
cover request boundaries, unchanged result limits and checkpoint round trips
for 40 requested contexts. The rejected records are retained for direct
canonical round-trip verification.

Candidate 28 passes all 1,335 Debug and sanitizer CTest entries, all 50
Release expectations, the fixed evaluation and strict lint. Its cold Lua
invocation nevertheless times out at 600.145 seconds during final completion,
after successfully publishing three checkpoint components. The full cold report
is present, but the timed-out invocation remains invalid acceptance coverage.
Warm replay finishes in 18.672 seconds with all 34 Lua units reused; the five
projects total 51 hits and zero function analyses, and every cold/warm semantic
report matches. Checkpoint writing consumes 14.5 seconds. This failed cost
observation is retained while serialization is profiled separately.

Candidate 29 streams checkpoint unit fields and diagnostics directly and uses
owned hash indexes for shared explanation tables. First-use vectors still
determine table ids and row order. In two isolated rewrites of the same loaded
Lua checkpoints, the previous writer takes 11.154 and 10.691 seconds; the new
writer takes 7.876 and 7.693 seconds. These are development measurements,
separate from corpus acceptance. All three decoded payloads match field for
field, including the 32-unit component, and the ordered-table reference and
nested diagnostic-text tests pass in Debug and ASan/UBSan.

An initial independent checksum assertion assumed the stored LLVM digest would
match Python's standard SHA-256 for every payload size. It matches the small
records but differs for the roughly 586 MB component in both writers. The
payload comparison records stored and independent digests separately; raw
artifacts retain standard SHA-256 identities. This dependency discrepancy is
separate from serialization equivalence. WeaveC's existing checksum validation
remains enabled, and the newly written records undergo reader validation.

A development formatting command also changed whitespace in three frozen
regression headers. The inventory guard rejected that population before running
its cases. All three headers were restored to their exact recorded SHA-256
contents, all 58 frozen input hashes were rechecked, and the population was
rerun. The failed guard observation is retained; no inventory or expectation
was changed.

A separate candidate 23 Lua diagnostic run completed in 952.515 seconds while
builds, tests and a short CPU profile ran. It is excluded from acceptance cost
measurements. Its completed report preserves all 60 baseline-complete Lua
identities and adds `print_usage`: 61 complete conditional contracts among
1,157 selected definitions, with no complete function carrying an iteration
limit. The full report and profile are retained.

Failed observations remain under `build/rfc28-validation`. Early candidates
rejected supported opaque outputs, default hooks, custom wrapper callbacks,
private array state and forwarded head cleanup. Those failures led to explicit
RFC amendments before the corresponding semantic changes. Frozen fixture
expectations were not weakened to accommodate them.

An independent integer-array probe exposed a false proof during development:
after setting `values[1] = 1`, a helper's write `values[i] = 0` for unknown
`i < 2` retained the old cell value. The checker could then incorrectly approve
index `values[1] - 1`. The retained unit test
`CheckedCode.PrivateIntegerArrayCellsRetainOnlyEstablishedValues` includes this
counterexample, disjoint writes and integer-conversion cases. Unknown overlap
now invalidates the old value and dependent numeric relations.

Other regression checks exposed overly broad terminal-field specialization,
lost callback bindings through setters, and loss of independent allocation
footprints during borrowed/conditional copies. Corrections preserve actual
argument identity, require must-release evidence for wrapper cleanup and retain
the allocation balance independently of the ordinary ownership holder.

Strict-lint failures from candidates 16 and 18 and the corrected candidate 20
lit assertion are retained. Each candidate 20 full suite passed 1,324/1,325
entries and failed only a new assertion expecting `checked safety violated`
instead of the existing `checked safety failed` diagnostic. Correcting that
test required no production edit; all 134 lit tests then passed on both builds.
Those original failure logs and reruns remain distinct from later passing
full suites. Candidate 24 passed all 1,327 entries in both
configurations. Candidate 22's two earlier full suites also passed
all 1,326 entries. A safe exploratory `free(p); p = 0;` wrapper still
has an incomplete generic contract under the existing reassigned-entry
projection. The implemented release output covers direct entry releases and
unconditional composition; richer conditional forwarding and reassigned-entry
projection remain explicit inference limits.

Candidate 30 adds a callback-setter regression in both ordinary and checked
modes. The ordinary setter retains a symbolic input without requesting a
specialization; its caller still diagnoses a callback-induced double free.
Checked mode retains the additional actual-input request. All 38 focused pointer
identity tests pass in Debug and ASan/UBSan. The failed candidate-29 ordinary cost observation
is retained separately from final acceptance.

Candidate 30's diagnostic counters show nearly identical ordinary work to the
baseline: 15,237 versus 15,231 function analyses and nine whole-program rounds
in both. Two CPU samples identify repeated path comparisons and call-input
footprint construction. A simpler comparator has only a small focused benefit
and was not adopted. Candidate 31 instead bounds immutable footprint preparation
at the existing 64-fact limit, returns an explicit rejection on overflow,
reuses shared path prefixes and caches up to 64 preparations per function.
The cache includes over-limit results but contains no caller state or proofs.
Boundary, duplication, expiration and eviction tests accompany the change.
The diagnostic runs and comparator benchmark are excluded from acceptance cost.

Candidate 31's initial new boundary test omitted the required zero offset for
`ValueSource::copyAt`; strict lint also required designated initialization of a
cache entry. Both were corrected. The rebuilt Debug suite passes all 99 focused
context and pointer-identity tests, and the affected translation units pass
strict lint. Original build and lint failures remain retained.

Candidate 31's isolated Lua probe takes 150.906 seconds with 678,707,200 bytes
peak RSS and exactly the candidate-30 diagnostics. Time approaches the target,
but memory remains above it. An experimental standard shared-array backing
(candidate 32) improves path creation but increases retained allocator bytes
from 40,588,656 to 44,788,640 in a 100,000-path benchmark. It was not retained;
its initial warnings-as-errors build and lint failures are recorded.

Candidate 33 instead uses one aligned allocation for an atomic reference count,
capacity, element pointer and trailing element array. The path handle shrinks
from 24 to 16 bytes. The same retained-path benchmark uses 27,491,504 bytes;
creation improves from roughly 0.102 to 0.062 seconds, with comparable steady
copy cost. These focused measurements are separate from corpus acceptance.
Lengths retain their full `size_t` representation, allocation arithmetic is
checked, and an added concurrent-copy test exercises detachment and shared
source preservation alongside the existing differential path tests.

Candidate 33's first strict-lint pass requested a const accessor, explicit
allocation-expression grouping, an explicit self-assignment branch and a
pointer comparison. The source-retention copy in `append` is intentional and
has a narrow documented lint exception: replacing it with a reference would
break self-append and shared-prefix lifetime. Those observations are retained
and the corrected source is rebuilt before final measurements.

The corrected candidate 33 build passes all 106 focused storage, context and
pointer-identity tests in Debug, including the concurrent-copy regression.
Strict lint passes for the changed backing implementation and tests.

Candidate 33 passes the repeated ordinary time gate at 1.0912 times baseline,
but fails the memory gate at 1.1437 times baseline. Baseline and final median
times are 141.514 and 154.421 seconds; median peak RSS is 596,918,272 and
682,704,896 bytes. This remains a failed acceptance observation.

Candidate 34 corrects the shared-path uniqueness check to acquire prior owner
releases before editing their backing bytes. A concurrent reader-release and
remaining-owner mutation regression accompanies the change. Allocation profiles
then identify copied generic function summaries as a large retained category.
Immutable exported publications remove the duplicate database copy and share
unchanged unit copies and checkpoint inputs. Replacement and widening still
publish independent complete values. Dedicated tests check value equality,
publication identity, replacement and destruction of source owners.
The malloc-logging investigation is bounded to 180 seconds and is excluded
from acceptance cost; the ordinary heap probe also overlaps builds and profiling.
The Lua memory probe uses 534,495,232 bytes of peak RSS and preserves its
4,081 ordered ordinary diagnostics exactly; its 270.977-second runtime overlaps
builds and is not an acceptance observation. A direct element-tag read makes ThreadSanitizer
report the relaxed ownership-check race (exit 66), while the acquire variant
passes (exit 0). Initial string-only controls did not expose the race; the
first detected report had symbolizer warnings, and the explicit LLVM-
symbolizer rerun is retained. Compiler diagnostics during migration to the
const publication API are retained.

Candidate 34 passes the repeated ordinary time gate at 1.0593 times baseline,
but fails memory at 1.1019 times baseline. Baseline and final median times are
142.172 and 150.600 seconds; median peak RSS is 599,179,264 and 660,242,432
bytes. The unchanged memory limit is exceeded by approximately 1.15 MB.
The much lower earlier overlapping probe was not representative of isolated
peak RSS and remains excluded from acceptance.

Candidate 35 extends immutable publication to callback and memory
specializations. Unchanged global projections retain their original value
while input keys remap independently. The existing memory-summary join still
normalizes its first input; reusing a publication requires identical semantic
facts and proof explanations. Regression tests cover duplicate definitions,
normalization, canonical explanation selection, changed global keys, retained
readers and report invalidation. The 131 focused tests pass. Initial migration
build failures and a static-analysis correction are retained separately.

## Reproduction

Use the [build and test instructions](../AGENTS.md).
Run `scripts/checked-opaque-interfaces.py --help` for the source, object,
regression, cache and pinned-upstream populations. The upstream checkout must
match `test/evaluation/rfc0028/upstream/upstream-identity.json`.

After builds, tests and profiling have stopped, run the existing sequential
cost harness with a fresh cache directory:

```sh
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc28-validation/final35-bin/weavec \
  --phase cache --compact-project lua --archive-reports \
  --output build/rfc28-repeat-cache --timeout 600
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc28-validation/final35-bin/weavec \
  --baseline build/rfc28-validation/baseline-bin/weavec \
  --phase ordinary --repetitions 3 --archive-reports \
  --output build/rfc28-repeat-ordinary --timeout 600
```

Existing semantic analysis budgets are unchanged. General shared graphs,
mutual recursive inference, asynchronous callbacks, nonlocal control transfer,
archive packaging and source-free proof artifacts remain separate work.
