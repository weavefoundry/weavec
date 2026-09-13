# RFC 0026 validation

All RFC 0026 acceptance gates pass. The [machine-readable evidence](../scripts/corpus/rfc0026-results.json)
records the final observations, executable and source hashes, frozen populations,
and retained failed development observations.

## Scope

[RFC 0026](rfcs/0026-growable-buffer-contracts.md) introduces inferred contracts
for ordinary contiguous C buffers. Length, capacity, actual allocation extent,
initialized prefix, termination, backing ownership and pointer-element ownership
are distinct facts. A capacity assignment cannot create storage or initialize
its tail. A successful grow can replace storage while preserving contents;
allocation failure preserves the old storage, and saved aliases do not acquire
the replacement's validity.

The implementation checks reserve, append, copying append, complete suffix
initialization for resize, truncate, clear, steal and supported element cleanup.
The finite bounded representation folds facts at actual control-flow joins and
transports outcome-dependent guarantees through helper summaries, separate
translation units, compiler sidecars and checkpoints. It adds no annotation or
runtime instrumentation.

Pointer vectors distinguish initialized cells from distinct live owned values.
Null elements are permitted. Borrowed elements cannot be freed, duplicate or
interior owners cannot establish the owned prefix, and incomplete cleanup cannot
discard it. Quantified owned-element pop, truncate and steal remain outside the
initial representation. Nested vectors and arbitrary shared ownership are also
outside this milestone.

Checked encoding 8, summary format 21 and sidecar format 22 carry these facts.
Objects and checkpoints from older formats must be rebuilt. Anonymous records
from a shared header now use canonical source paths for their cross-unit type
identity. A direct constructor wrapper forwards the callee's per-outcome null
and non-null stores; a saved result does not gain this guarantee through an
intervening write.

## Acceptance populations

The preserved v0.6.0 baseline meets 16 of the primary 28 expectations, including
six accepted cases. The passing primary population accepts 15 positive cases
and rejects 13 negative cases for their intended properties. The primary 28
cases and separate-source 11 cases retain their frozen sources,
expectations and hashes. Compiler-object evaluation compiles the separate-source
cases and resolves their checked contracts at link time. Supplemental unit and
lit cases cover status-dependent construction, unknown mutation, callbacks,
aliases, failure cleanup, physical capacity versus advertised capacity,
nonwrapping growth and skipped or conditional resize stores.

The independent finite-state oracle enumerates 252 establishment states, 144
branch-join states and seven ownership cases. It compares concrete allocation
sizes, counts and initialized byte sets rather than calling the analyzer's
abstract-domain code. Rejection of an invalid buffer predicate does not claim
that every represented execution necessarily has C undefined behavior.

Three additional callers use the complete unchanged pinned Jansson strbuffer
implementation: runtime repeated growth with a string read, runtime repeated
growth followed by steal, and an uninitialized-tail read that must be rejected.
The original eight-case RFC 0019 Jansson population remains separate. In that
population the baseline's lifecycle caller was already complete; its generic
append helpers were the missing proof. This milestone requires those generic
helpers as well as the callers to be complete.

## Correctness checks

The final Debug suite passes 1,243/1,243 tests in 465.57 seconds. ASan/UBSan
passes 1,243/1,243 in 489.23 seconds. Both include all registered evaluations
and lit tests. All builds use LLVM 23 and warnings as errors. Strict clang-tidy
passes for all 36 changed C++ translation units: 35 passed the full invocation;
test-only string-construction findings in the remaining file were corrected
and that entire translation unit passed a strict rerun. Its 14 focused JSON
and ledger tests pass in both configurations after the correction. Formatting
and the Core include boundary pass. Suite timings include concurrent validation
work and are not performance measurements. The nine-test Python harness suite
passes, and all 50 frozen input files match their recorded SHA-256 hashes.

| Population | Expectations met |
| --- | ---: |
| Primary source | 28/28 |
| Separate translation units | 11/11 |
| Compiler objects and checked linking | 11/11 |
| Checkpoint invalidation and report equivalence | 4/4 |
| Independent finite-state oracle | 403/403 |
| Unchanged Jansson runtime clients | 3/3 |
| Original RFC 0019 Jansson population | 8/8 |

The oracle includes both valid and invalid predicates. It reports no false
proofs or false rejections in its finite population. This is a bounded
validation result, not a completeness claim for arbitrary C programs.

## Corpus coverage and checked cost

The final uncached, cold-cache and warm-cache observations preserve all 148 exact
complete identities among the same 1,619 selected definitions. Their canonical
reports are equal for each project. No new complete broad-corpus identity is
claimed. The additional coverage is demonstrated by the frozen runtime-buffer
population and independently selected generic Jansson helpers.

| Project | Baseline uncached seconds | Final uncached seconds | Peak RSS bytes | Complete selected |
| --- | ---: | ---: | ---: | ---: |
| log.c | 0.177 | 0.191 | 56,295,424 | 5/12 |
| cJSON | 25.175 | 27.338 | 320,634,880 | 32/151 |
| linenoise | 17.307 | 17.710 | 382,697,472 | 27/88 |
| Jansson | 105.672 | 134.479 | 1,135,722,496 | 24/211 |
| Lua | 528.608 | 581.722 | 8,070,561,792 | 60/1157 |

Every checked project finishes below the unchanged 600-second limit. Lua uses
compact reports for cold timing comparisons; the other cold reports are
expanded. Lua's 249,139,746-byte report retains the full checked content. Both
Lua cold observations take about 582 seconds, leaving a narrow timing margin.
Peak memory remains substantial: approximately 8.1 GB uncached and 8.3 GB with
a cold cache. Incomplete and iteration-limited functions remain explicitly
incomplete; the 148 complete identities do not include limited proofs.

| Project | Cold cache seconds | Warm cache seconds | Cold peak RSS bytes | Warm peak RSS bytes | Warm hits |
| --- | ---: | ---: | ---: | ---: | ---: |
| log.c | 0.677 | 0.144 | 60,735,488 | 56,459,264 | 1 |
| cJSON | 27.966 | 0.959 | 485,130,240 | 351,649,792 | 2 |
| linenoise | 17.832 | 0.611 | 484,360,192 | 227,573,760 | 2 |
| Jansson | 131.755 | 2.782 | 1,578,778,624 | 881,573,888 | 12 |
| Lua | 581.733 | 20.779 | 8,325,824,512 | 8,136,605,696 | 34 |

Warm replay uses compact reports and totals 51 cache hits with zero function
analyses. Semantic report comparison covers all contracts, obligations and
metadata despite the cold/warm encoding differences.

## Ordinary analysis cost

Three sequential observations of the preserved baseline were followed by three
of the final Release binary, with builds, tests and profiling stopped. Each
observation covers all five unchanged projects. Time is their total analyzer
time; peak RSS is the largest project process in that observation.

| Measurement | Preserved baseline | Final |
| --- | ---: | ---: |
| Total seconds, three runs | 167.760, 166.902, 166.642 | 164.047, 165.102, 163.424 |
| Median seconds | 166.902 | 164.047 |
| Median peak RSS bytes | 648,232,960 | 587,071,488 |

The time ratio is 0.9829 and the memory ratio is 0.9056, both below
the unchanged 1.10 limits. Median ordinary time decreases by 1.71% and
median peak RSS by 9.44%. All six observations finish without failures.

## Development observations

Failed development observations remain in `build/rfc26-validation` and the
machine-readable ledger. Early implementations lost the initialized prefix at
runtime joins, retained insufficient allocation bounds through reserve wrappers,
or failed to restore outcome-dependent facts before a join. Separate negative
checks exposed stale saved aliases and conditional-allocation mistakes. The
oracle exposed a missing distinction between physical allocation capacity and
advertised capacity, followed by a valid null-element case that was rejected.
Those inputs and failed results were retained through the fixes.

The unchanged-source evaluation found mismatched identities for an anonymous
header record included through relative and absolute paths, and loss of string
termination across unrelated local writes. The last constructor-forwarding
change initially confused an omitted fresh store with a NULL store and rejected
failure cleanup in two Jansson clients. The final version forwards actual NULL
facts while preserving the incoming value when no store occurs.

The first full Debug and sanitizer runs had four failures, all in the new
buffer coverage: constructor forwarding, resize, compiler objects and the
oracle. The final runs above pass without changing their expectations. Earlier
build attempts also recorded disk-space and temporary instrumentation errors.
A later descriptor-cache test initially collided with a function declaration in
its shared test prelude; renaming that test helper fixed both suite failures.
Strict lint found excess padding from a Boolean cost hint; placing it beside
the existing flags resolved the warning without a suppression.
The upstream syntax harness was corrected to pass the manifest's compiler
include flags to its independent syntax check. No frozen source or expectation
was changed to accommodate a missing proof. Historical large report files were
compressed with verification of their decoded SHA-256 hashes to recover local
scratch space.

The first quiet Release corpus run preserves the 88 complete identities in
log.c, cJSON, linenoise and Jansson. Jansson takes 163.495 seconds versus its
recorded 105.672-second baseline. Lua times out at 600.746 seconds, leaving a
non-final statistics snapshot and no complete report. This is a failed gate,
not incomplete coverage that can be counted as a successful run. Subsequent
optimization caches immutable descriptor discovery within an AST and avoids
interning places for pointer and record types with no buffer subobjects.
It does not cache flow-sensitive predicates or change their proof rules.
Two subsequent attempts also exceeded Lua's limit: 600.129 and 600.463 seconds.
The latter preserves the same first 88 identities, with Jansson taking 156.273
seconds. A fourth attempt reached 600.165 seconds without a final Lua report.
A fifth attempt reached 600.186 seconds without a final Lua report.
A sixth attempt reached 600.324 seconds, and a seventh reached 600.342 seconds.
The first passing uncached candidate then failed the cold-cache Lua gate at
600.485 seconds. Warm replay restarted function analysis; it was deliberately
stopped after that zero-analysis requirement had already failed. The exact
analyzer command and interruption are recorded. A ninth cold-cache attempt completed all 39,833 function analyses but reached
600.528 seconds during final compact report output. Its warm replay completed
in 25.036 seconds with 34 Lua hits and zero function analyses; all five projects
contributed 51 hits. The missing cold report still fails acceptance. These failed cost observations remain in the evidence ledger.
The final candidate additionally confines buffer-specific initialized-range
snapshots and separation scans to relevant functions and reuses an already
selected call outcome when forwarding constructor stores. A further call-path
optimization skips destination resolution for unconditional summaries and tests
conditional-store membership without copying each destination set. The
Jansson diagnostic run retains identical function-analysis, block-transfer and
state-join counts while reducing accumulated dataflow time. The initial system sampling attempt was
unavailable in the sandbox. A later narrowly scoped, approved invocation
sampled only a child analyzer launched for profiling. Its three samples exposed
repeated whole-summary copies in publication and database construction.
The implementation now moves completed summaries and shares immutable database
publications, with tests for copy isolation and reordered global identifiers. Instrumented buffer-specific routines then accounted for
less than one second of a bounded 180-second Lua profile. Inspection found a
buffer-return refinement resolving unrelated pointer-returning calls; it now
runs only in functions with registered buffer objects. The fresh preserved
baseline completes Jansson in 114.449 seconds on the same machine, versus its
original 105.672-second observation.

The buffer runner now removes old result files before invoking a child
analyzer. Fault-injection tests cover a child returning either success or
failure without producing a new report: neither can reuse a previous passing
result. Missing reports remain failed observations.

## Reproduce

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
python3 scripts/checked-buffers.py --population upstream \
  --weavec build/rfc26-release/bin/weavec \
  --output build/rfc26-validation/upstream --timeout 600
python3 scripts/checked-memory-corpus.py \
  --weavec build/rfc26-release/bin/weavec \
  --output build/rfc26-validation/jansson --timeout 600
```

The source, transport, objects, cache and oracle populations use the same
`checked-buffers.py` interface. Objects also require `--cc` naming `weavec-cc`.
The upstream runners require the pinned Jansson checkout in `build/corpus`.
The complete Debug and sanitizer CTest suites include the five local buffer
populations and all existing registered evaluations.

Cost measurements use `scripts/scalability-evaluation.py` with three sequential
ordinary observations per executable, followed by checked and cache phases.
They run after builds and other evaluations have stopped. Lua uses compact
reports in the cold observations; the other four projects use expanded reports,
matching the baseline. Each checked project retains a 600-second limit.

Subsequent native samples of a child analyzer using the warm checkpoint reached
cache decoding, diagnostic rendering and compact report serialization. These
are profiling observations, not acceptance timings. The first relative-path
probe missed the cache; it and the next bounded probe were deliberately stopped.
The final absolute-path probe completed with 34 hits and no function analyses.

The revised frontend moves completed exports through checkpoint publication and
restores them after both successful and failed writes. A canonical decoder
round trip still validates the cache producer. Diagnostics use a private bounded
stream, flushed after every diagnostic; exact error/note output, stream lifetime
and restoration of the previous diagnostic client are tested. Compact reports
reuse immutable call paths through a bounded memo and avoid copying interned
strings on hits. Memo eviction changes neither report contents nor table order.
A 1,200-path test checks decoded locations and obligations across that bound.

The first full suite after buffering found a diagnostic-order regression in
both Debug and ASan/UBSan. Clang's formatted stream inherited its sink's buffer,
so the final locationless error could precede the preceding note's source
snippet. The fix presents an unbuffered facade to Clang and buffers only its
private destination. The exact-output test now includes that locationless
transition; the original frozen explanation output remains unchanged. Both
failed full-suite observations are retained.

The next cold-cache run exceeded Lua's limit at 600.357 seconds with 37,935
function analyses and no final report. Its warm replay restarted analysis and
was deliberately stopped after failing the zero-analysis requirement. This is
the tenth failed cost observation. Six further native samples of an uncached
child identify repeated diagnostic identity escaping, eager object-view
resolution and full-ledger trust scans. The final implementation uses bounded
word scans for plain ASCII, resolves erased holders only when recovery is
needed, and reuses the existing exact trusted-origin projection. An exhaustive
byte/UTF-8 boundary test checks that escaped text remains canonical. The
final full suites, focused corrected tests and cost measurements pass as recorded
above. The initial strict-lint findings and successful test-file rerun remain
separate observations in the ledger.
