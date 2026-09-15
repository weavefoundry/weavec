# RFC 0027 validation

All RFC 0027 acceptance gates pass. The final candidate passes both full test
suites, all 136 Release expectations, strict lint, corpus preservation, cache
replay and the ordinary runtime and memory limits. The
[machine-readable evidence](../scripts/corpus/rfc0027-results.json) records
source and executable hashes, frozen populations, final observations and retained
failed development attempts.

## Scope

The implementation infers structural predicates for finite acyclic ownership
forests and separate equalities over their allocation identities. Recursive
traversal and destruction can use proper-child induction. Construction,
attachment, detachment and supported relinking compose through preservation,
partition, combination and complete-consumption outputs. A subset output alone
cannot settle the caller's cleanup obligation.

Initialized ownership flags can exclude borrowed children or payloads. Inactive
edges confer no permission on their pointees. Nonowning backlinks do not become
ownership edges. Payloads and recursive children require their own separation
and release evidence. Closed cJSON clients establish its actual default hooks;
no cJSON-specific trusted contract or modified upstream definition is used.

The initial recursive-consumption proof covers direct self-recursive one-pointer
void functions, local cursors and actual resolved release callbacks. Mutual
recursive groups and additional hidden effects remain conservative. This does
not certify arbitrary graphs, shared ownership, the cJSON parser/printer, or
private mutable global hook state across separate translation units. The
upstream compiler-object population includes each complete unchanged cJSON
implementation in its client translation unit before serializing the result.

Checked encoding 9, summary format 22 and sidecar format 23 carry the new
descriptors and conservation facts. Older objects and checkpoints must rebuild.
There are no new annotations, diagnostic identifiers or runtime checks.

## Acceptance populations

The 27 primary cases and seven unchanged-upstream clients were frozen before
checker changes. Twenty separate-source cases were mechanically extracted
during implementation from those clients; compiler-object cases use the same
files. The independent oracle was generated during implementation and frozen
before its first analyzer run. These are distinct populations; their provenance
and source hashes are retained, and expectations were not changed after failures.

| Population | Baseline expectations met | Final expectations met |
| --- | ---: | ---: |
| Primary source, including generic helpers | 14/27 | 27/27 |
| Separate translation units | 11/20 | 20/20 |
| Compiler objects and checked linking | Not measured | 20/20 |
| Checkpoint invalidation and equivalence | Not measured | 6/6 |
| Independent concrete-heap oracle | Not measured | 43/43 |
| Runtime-size work observations | Not measured | 6/6 |
| Unchanged cJSON clients | 2/7 | 7/7 |
| Unchanged cJSON clients through compiler objects | Not measured | 7/7 |

The primary population has thirteen positives and fourteen negatives. The
baseline accepts one positive and correctly rejects thirteen negatives. Its
accepted dropped-head reverse-wrapper counterexample is a false proof removed
by this change. The baseline's positive nested-call reverse wrapper already
passes and is preservation, not new coverage. An existing separately allocated
reversal-wrapper unit test now also passes.

All eleven closed primary positives have zero entry requirements. The two
generic helpers export explicit sufficient requirements. The separate-source
population has nine positives and eleven negatives; the upstream population
has five positives and two negatives. Successful closed clients have no
deferral, limit, or new annotation/unsafe trust. Existing allocator/libc trust
remains explicit. Negative results must carry their intended unresolved or
violated property; syntax errors, crashes, absent reports and timeouts cannot
satisfy an expectation.

The oracle enumerates all 4,096 three-node binary pointer topologies and selects
the ten rooted ownership trees. Empty, singleton and independent-root cases
bring the positive population to thirteen. Three mutations of each three-node
tree omit cleanup, duplicate a root or introduce an owning cycle, producing
thirty negatives. Concrete DFS tracks acquired and released allocation
identities independently of the abstract domain. The 43 C files and two JSON
files reproduce byte-for-byte from the retained generator. Separate Core tests
check all 4,096 graph topologies and 32 allocation partitions. These finite
checks do not establish completeness for arbitrary C.

Runtime-size observations use the same constructor body with sizes 0, 1, 8,
64, 1,024 and 1,048,576. All six complete with sixteen function analyses each.
Block transfers are 108, 114, 156, 146, 146 and 146 respectively. The recursive
proof does not unroll one abstract node per runtime allocation.

## Correctness checks

Candidate 13 passes all 1,298 Debug tests in 172.35 seconds and all 1,298
ASan/UBSan tests in 225.44 seconds. Each suite includes 1,256 unit tests and
42 integration tests, covering the registered evaluation populations, lit tests,
contract limits, guard caches, alias storage and the borrowed-caller lifetime
regression. Suite times are validation observations, separate from the isolated
corpus performance measurements.

Warnings-as-errors builds use LLVM 23. Candidate 13 passes fresh strict
clang-tidy checks of all 45 changed C++ translation units. Production lint
checks remain enabled. The deliberate moved-from-object
regression has a local, documented test-only suppression. Formatting, the Core
include boundary, frozen inventories and oracle regeneration pass.

Candidate 9's initial lint pass rejected a one-character string append in a new test
fixture builder. Replacing it with a character append emits identical C source.
The corrected translation unit passes strict lint; both test binaries were
rebuilt and the pointer-identity tests rerun successfully under Debug and
ASan/UBSan. The original failure remains recorded. Production executables and
frozen fixtures did not change after those full suites. The later candidate 10
and candidate 11 runs include that correction.

The final Debug build uses `-O0 -gline-tables-only` with assertions enabled;
smaller debug symbols avoid recreating the earlier large intermediate files.
The sanitizer build retains `-O1 -gline-tables-only`, ASan/UBSan and assertions,
matching the project's CI sanitizer configuration. Release uses `-O3 -DNDEBUG`
and ThinLTO for both baseline and final executables.

The final Release population run uses immutable executables. Every invocation
checks that its binary has not changed; reports are retained as checksum-verified
gzip archives. Object positives must produce an executable, and each compiled
unit must produce a sidecar. Cache checks compare complete expanded, compact,
warm and uncached reports and diagnostics, change a child destructor to verify
invalidation, recompute corrupt checkpoints, and reject stale sources and
previous sidecar versions.

## Corpus preservation and cost

Candidate 13 passes the checked cost and replay
requirements. It preserves all 148 exact complete identities across all 1,619
selected definitions. Every cold/warm report pair is semantically equal, and
all 51 units replay with zero function analyses. The known Lua iteration-limit
findings remain visible; a complete report and preservation of the selected
complete identities do not certify every Lua function.

| Project | Cold seconds | Warm seconds | Reused units |
| --- | ---: | ---: | ---: |
| log.c | 0.566 | 0.142 | 1 |
| cJSON-program | 33.778 | 0.899 | 2 |
| linenoise-program | 17.151 | 0.569 | 2 |
| jansson | 136.671 | 2.687 | 12 |
| lua | 565.277 | 17.262 | 34 |

Lua's measured cold peak RSS is 8,396,259,328 bytes. Three sequential ordinary
baseline observations were followed by three final-build observations. Each
covers the same five projects. Runtime is their total analyzer time; peak RSS
is the largest project process in each observation.

| Measurement | Baseline | Final |
| --- | ---: | ---: |
| Total seconds, three runs | 163.143, 163.438, 160.579 | 146.454, 147.536, 144.144 |
| Median seconds | 163.143 | 146.454 |
| Median peak RSS bytes | 592,658,432 | 596,459,520 |

The median runtime ratio is 0.897703 and the median peak-RSS
ratio is 1.006414. Both pass the unchanged 1.10 limit. All
six observations finish without failures. Baseline and final executables use
matching Release compiler, SDK and ThinLTO settings. Builds, tests and profiling
were stopped during these sequential measurements.

All five cold/warm report pairs have equivalent full contracts and metadata,
and their ordered diagnostics agree. Warm replay reuses all 51 units with zero
function analyses. Every cold checked project finishes within 600 seconds, and
the final reports retain the same 148 exact complete identities among the
original 1,619 selected definitions. Candidate 13's expanded semantic report
hashes also match candidate 12, confirming that the caller-text lifetime fix
preserves the reported analysis results.

## Development observations

The completed candidate 6 run preserves all 148 exact complete identities among
1,619 selected definitions. All five cold/warm reports are semantically equal;
all 51 units replay with zero function analyses. Lua takes 639.268 seconds cold
and 19.898 seconds warm, so this candidate still fails the 600-second gate.
Its three ordinary observations pass: baseline totals are 164.047, 178.271 and
190.725 seconds; final totals are 181.283, 159.569 and 158.529 seconds. The median
runtime ratio is 0.8951 and the median peak-RSS ratio is 1.0132. The later alias-storage experiment also failed the cold limit and was withdrawn;
those earlier non-LTO observations remain separate from the final comparison.

Failed observations remain under `build/rfc27-validation`. Early implementations
rejected runtime construction, partial-release callback wrappers, conditional
payload cleanup and existing buffer-array cleanup. The first full Debug suite
had fourteen failures; the next had two. Their corrections preserve the original
expectations. Serialization assertions were updated for the deliberately bumped
format versions.

A detachment regression exposed a false proof: returning a child and then
discarding it could retain the parent's old footprint equation after mutation.
Invalidation now forgets affected current equations while preserving independent
entry snapshots. A nested getter passed to that helper must also retain its
entry value when a different formal parameter writes the same caller cell.
Positive and dropped-child unit cases pin both behaviors.

Callback checks exposed stale known hooks after an unknown aggregate copy and
missing actual bindings through indirect wrapper calls. Record copies now carry
the actual function-pointer facts, and each indirect target receives its actual
bindings. Alternative targets retain only common guaranteed outputs. Tests
include one target that omits child cleanup or returns NULL instead of preserving
the input; those alternatives cannot publish complete consumption/preservation.

An exploratory separate-unit cJSON probe with private mutable global hooks
remains incomplete. It is retained as an unsupported boundary, not included in
the frozen positive denominator. An earlier successful upstream-object run
overlapped a rebuild and is retained only as a development observation; the
final population reruns all cases with the immutable Release executable.

Final review also checked recursive destruction with an unrelated global
release and nodes with different release callbacks. Both were already rejected;
their retained observations and unit tests preserve those boundaries.

The first isolated checked corpus attempt timed out on Lua at 600.272 seconds
and produced no completed report. It is a failed gate, not accepted coverage.
Its incomplete cache replay was interrupted after it began recomputing the
program. The other four projects preserved all their original complete identities.
The subsequent optimization resolves repeated heap-input paths once per call
and reuses imported recursive-field discovery within a database generation.
Discovery reuse retains all observed dependencies, and a generation change
invalidates its candidates. Neither change alters proof rules or analysis budgets.

The next isolated checked run completed Lua in 599.714 seconds and preserved
all sixty of its baseline complete identities (148 across the five projects).
However, the large Lua component could not write a lossless checkpoint, so warm
replay recomputed those units and was interrupted. That attempt is also a failed
gate. A separate instrumented reproduction isolated `finishbinexpneg`: its
256 retained requirements omitted a container premise while its output still
referenced that premise. The strict decoder correctly rejected it. The producer
now retires dependent outputs whose premises or separation facts were lost,
retaining the limit flag and other represented facts. The same premise validator
serves final inference, joins and strict decoding. Core regressions cover the
capacity and output-intersection boundaries; an analysis stress case retains
portable incomplete contracts. Temporary instrumentation was removed.

The first full run with the producer correction timed out at 600.416 seconds
while writing the large Lua checkpoint. It completed 42,906 function analyses,
reported no cache-write validation failure, and had encoded 515,006,633 bytes
before timeout; it did not produce a final report. The recorded dataflow time
was 501.600 seconds and cache-writing time at least 13.589 seconds. Warm replay
was interrupted and ordinary timing did not start. This remains a failed cost
gate, despite correcting the earlier contract-validation failure.

Temporary internal timing probes localized repeated alias-path queries. By
round four of the Lua probe, 34,392,151 non-root mirror queries returned
44,229,011 places in total, with maximum width 52. Footprint-state copying
accounted for only 0.0104 seconds of the instrumented conditional checks, so
that representation was left unchanged. Alias queries instead reuse their
existing singleton path/vector when no alias expansion is needed. Actual
expansion retains its previous order, offsets and depth exclusions; definite
queries borrow alias edges only while the relation is immutable. All probes
were removed before rebuilding and rerunning validation. Probe timers can nest;
they are diagnostic measurements, not additive wall-clock components or
acceptance evidence.

The following alias-query build also missed the cold limit at 600.351 seconds,
with 42,906 function analyses and no checkpoint-validation failure. Warm replay
then completed in 18.752 seconds for Lua: all 34 Lua units (51 across the corpus)
were reused with zero function analyses. Thus the producer repair restored
reuse, but the absent cold Lua report still prevented the combined equivalence
and cost gate from passing. The ordinary comparison did not start.

A separate benchmark loaded the settled Lua checkpoints without reanalysis and
rendered all 34 report units in 4.379 seconds. A checkpoint rewrite took 10.237
seconds. Checked transport now reuses repeated rendered guards within one
contract and its global-name mapping, bounded to 64 guards and 1 MiB of text.
The reader also caches only successfully decoded guards within one contract and
resolver, with the same bounds. Field framing, requirement validation and
producer round-trip checks remain enabled. The observed rewrite time was 9.193
seconds after both caches; all three rewritten checkpoint files were byte-identical
to the earlier output. These single microbenchmarks are diagnostic observations,
not the acceptance timing comparison. Namespace and cache-eviction regressions
cover both codec paths.

During candidate 6's cold run, cJSON took 82.424 seconds and linenoise
48.630 seconds. Parsing, liveness and dataflow slowed together while analysis
and block-transfer counts stayed unchanged. The owner confirmed Low Power Mode
and connected power, restoring normal mode while Jansson was in round six.
Lua had not started, and the ordinary comparison had not started. The first
three checked observations therefore reflect Low Power Mode; Jansson spans the
transition. These remain actual elapsed-time observations subject to the same
600-second gate. Lua, warm replay and the sequential ordinary comparison run
after the confirmed power transition. Earlier failed attempts remain retained;
the confirmation does not retrospectively attribute all failures to power mode.

The subsequent storage experiment shares a complete alias adjacency map across
value copies and detaches before mutation. Outer rows use a hash index while
edges, pair enumeration and predicate visitation retain their original order.
This differs from RFC 0020's earlier per-row sharing experiment. Twenty alias
tests, including the independent directional-edge model and every mutation's
copy isolation, pass; all 514 Core tests also pass under ASan/UBSan. One test
intentionally reuses a moved-from relation as required by this RFC, with a
local, explained suppression of the two moved-from-object lint diagnostics.

Three alternating microbenchmark repetitions cover 8, 64 and 512 disjoint
pairs. Lookup time ratios are 0.807, 0.390 and 0.293. Read-only and mixed copies
improve, while copies that always mutate have ratios 0.997, 0.946 and 0.937.
Checksums agree. These synthetic observations justify evaluating the storage
change on actual programs; they do not establish the corpus performance gate.
The full rebuilt Debug and sanitizer suites, all 28 changed translation-unit
lint checks and all 136 frozen Release expectations passed. Cold Lua instead
took 785.956 seconds; warm replay took 20.168 seconds with all 51 corpus cache
hits and no analyses. All 148 identities and cold/warm contracts were preserved,
but the cost gate failed. Four alternating cJSON observations compare candidate
6 (35.023, 37.068 seconds) and candidate 7 (36.760, 36.525 seconds), with full
semantic report equality. Their median ratio is 1.0166, providing no convincing
program-level benefit. The shared alias storage was withdrawn and RFC 0020's
value-owned ordered adjacency restored; the experiment's source is retained.

A ten-second native sample of the immutable candidate 6 Lua analysis recorded
7,630 main-thread samples. Inclusive, nonrecursive totals include 948 samples
in summary-path resolution, 333 in object-view validation, 203 in dereference
record construction and 965 in call-explanation propagation. These categories
can overlap and are diagnostic evidence, not acceptance timing. The next
candidate groups each dereferenced pointer, source expression and element
witness into one growing sequence with two inline entries, and uses four inline
entries for mirror results. Deep and wide queries remain unbounded by those
storage capacities. Successful type-only object-view checks may be reused for
the same live immutable summary and call path; erased-pointer recovery and
failed checks remain uncached. The function-local index is bounded to 128 paths
with at most 4 KiB of field text each. New release/replacement, deep-dereference
and changing erased-view regressions accompany these representation changes.

The first rebuild after restoring alias storage reused an incompatible Core
object because the restored source retained its earlier timestamp. A smoke test
caught the resulting crash before acceptance observations. Refreshing both
restored source timestamps rebuilt Core and every dependent Analysis object;
the failed executable remains retained. A broad manual format invocation also
touched two frozen fixture headers. The inventory check rejected them before
analysis, and their original bytes were restored against the unchanged hashes.
No frozen expectation or inventory changed. The rebuilt candidate passes all
27 source expectations and matches prior diagnostics on eight wide-alias and
two deep-record release/replacement probes. An exploratory five-level indirect
pointer spelling was outside the prior ordinary checker's observed release
tracking; it is retained separately and supplies no acceptance evidence.

Four alternating cJSON observations compare candidate 6 (37.033, 35.435 seconds)
and candidate 9 (35.248, 34.754 seconds). Their median ratio is 0.9660 and all
expanded report hashes agree. The subsequent candidate 9 Lua run instead takes
863.752 seconds cold and 43.882 seconds warm. Both reports have exactly the same
expanded semantic hash as candidates 6 and 7, preserving all sixty complete
Lua identities among 1,157 selected definitions. Warm replay reuses all 34 Lua
units with no function analyses. This remains a failed 600-second cost gate.

Read-only host observations during that run show substantial active memory
compression on a 24 GiB machine. The owner reported one other Codex task that
might have been running web-app tests, then confirmed it was idle. Neither that
task nor memory compression is established as the cause of the timing change.
The raw observations and failed run are retained. The fresh non-LTO baseline
Lua observation took 659.667 seconds cold and 30.784 seconds warm, but neither
produced a completed report, so neither establishes checked coverage. Free disk
space fell to 268 MiB during that run; the retained harness output does not
establish the cause of the missing reports. Removing only regenerable Debug
objects and static archives recovered about 2.4 GiB. Every removed intermediate
is inventoried; sources, executables and validation records were retained.

The following candidate 9 cold observations completed log.c, cJSON, linenoise
and Jansson in 0.673, 56.657, 30.057 and 254.554 seconds. Lua timed out at
900.401 seconds without a final report. A ten-second native sample began only
after the 600-second gate had already failed. Its 6,970 main-thread samples
included 1,057 under object-view validation, with repeated path comparison,
insertion and destruction. The sample and its instrumented tail are retained
as diagnostic evidence. Warm Lua replay began reanalysis and was stopped after
126.059 seconds; the following ordinary comparison was interrupted when this
candidate was superseded. Neither is a completed acceptance observation.

Candidate 10 replaces whole-path memoization with successful individual
type/layout comparisons. Its 128-entry index borrows view addresses and checks
the live immutable summary owner. Every path prefix is still visited, including
prefixes below a cache hit. Erased-pointer recovery and failures remain uncached.
Both the unchanged baseline and candidate 10 are rebuilt with the normal Release
preset's link-time optimization, matching the compiler, SDK and flags. Earlier
non-LTO observations remain separate from the new comparison. The 600-second
gate, exact baseline identities and semantic budgets are unchanged.

A separate four-case interface probe confirms the current conservative boundary:
with complete record layouts visible, complete cleanup passes and dropped-head
cleanup fails. With only a forward-declared record visible, both clients remain
incomplete because the required object view cannot be established. The probes
ran after the timing queue stopped and are retained outside the frozen population.

Review of recursive-summary widening found another contract-portability edge.
Widening first merges entry requirements under their fixed 256-entry bound, then
restores independently verified induction outputs. A standalone `SummaryStore`
reproduction supplies a saturated earlier contract and a complete consumption
contract. Their widened result retains an output after losing its required
container premise, and the strict decoder rejects it. The original inputs and
result are retained. This is a storage/transport regression; the reproduction
constructs summaries directly and is not a demonstrated accepted C program.
Candidate 10's pending benchmark queue was stopped before any timing workload
started. Candidate 11 rechecks restored outputs against the widened premises.
Its regression passes immediately below and at the requirement cap, verifies
lossless checked-contract round trips, and retains the widened ordinary effects.
Candidate 10's preceding validation passed 1,293 Debug tests in 288.65 seconds,
1,293 sanitizer tests in 365.40 seconds, all 136 Release expectations, forty
strict lint checks and the fixed evaluation. Those results remain development
evidence; the final correction receives its own validation.

During candidate 11's Release correctness populations, a later process
observation found a Next.js server using 333.3% CPU alongside Playwright activity.
The owner had earlier reported the other task idle; this observation records
the later host state. The pending timing queue was held before any measurement.
The busy server subsequently exited and the remaining server was observed idle.
No other task's processes were stopped or modified. Correctness-population times
remain validation observations rather than the final sequential cost comparison.

Candidate 11 passed all correctness checks but failed the strict cold Lua gate
at 600.370 seconds without a final report. Cold log.c, cJSON, linenoise and
Jansson completed in 0.682, 34.023, 16.881 and 138.732 seconds. The first four
warm projects completed, but Lua had no published checkpoint and restarted
analysis. After the gate had already failed, a ten-second sample of that
reanalysis recorded 6,805 main-thread samples, including 727 under subtree
forgetting, 672 under guard invalidation and 637 under call-origin application.
The warm reanalysis was then stopped; no ordinary candidate 11 observations
were started. Partial warm artifacts are not accepted coverage. The full stop
record and profile are retained. These overlapping profile counts motivate
batched invalidation and shared callee-origin text in the next candidate.

Candidate 12 batches subtree guard invalidation and caches caller-independent
origin text within the existing call-cache budget. All 517 Core tests, the 27
summary/recursive analysis tests and the five directly affected lint checks
passed. Two lint corrections added explicit arithmetic parentheses and brought
pre-existing test helpers into the repository's static-function convention.
The first candidate 12 cold run completed log.c, cJSON, linenoise and Jansson in
0.846, 56.928, 29.860 and 231.154 seconds, with valid coverage. Lua timed out at
600.099 seconds without a report; the wrapper stopped before warm or ordinary
observations. This remains a failed acceptance observation.

Disk recovery removed only 214 inventoried object/archive intermediates from
an inactive older CMake build (339,790,408 bytes). After the failed timing run,
filesystem compression reclaimed another 2,700,931,072 bytes from 549 older
generated JSON/text reports. Every replacement retained the same path and was
verified against the original content hash. The first restricted test copy
could not publish readable compression metadata and was rejected before any
source replacement; an unrestricted test copy and the full operation verified
successfully. All reports, source snapshots and benchmark executables remain
available. A subsequent paired comparison checks the previous and current
executables under the same host conditions; it is separate from the required
final five-project measurements.

After disk recovery, candidate 12 passed the cost gates: Lua took 539.617
seconds cold and 16.957 seconds warm; ordinary runtime and peak-RSS ratios were
0.917942 and 1.025238. These observations precede the caller-text lifetime
correction. Candidate 13's measurements above supply the final cost evidence.

Candidate 12 subsequently passed all 1,297 Debug tests in 171.38 seconds and
all 1,297 ASan/UBSan tests in 248.90 seconds. A targeted review found an aliasing
regression beyond those tests: the caller name can be a string view into a
destination ledger row that the first insertion replaces. The new fragment path
then rereads that invalid view while updating the next row. A retained standalone
Core reproduction triggers ASan's heap-use-after-free diagnostic. This is a
component API memory-safety regression, not a demonstrated accepted C program.
The remaining candidate 12 validation queue was stopped after the sanitizer
suite finished. Candidate 13 retains the caller string already owned by the
lookup key throughout fragment application; its regression covers cold and
cached preparations, unsafe conversion, and caller names/locations borrowed
from independently built destination rows. Candidate 13 passes both full suites,
all 136 Release expectations, the fixed evaluation and all 45 strict lint checks.
All 66 implementation, header, build-registration and unit-test hashes match
the validated source, and both retained Release executables match their hashes.

The final acceptance audit added exact lit checks for partial recursive cleanup
and saved descendant aliases, including both the error text and the notes that
identify the failing recursive operation or release. These checks use the
existing frozen fixtures and pass in Debug, ASan/UBSan and immutable Release
runs. The full suites precede these additional message assertions; only the
changed lit test needed rerunning, and implementation hashes remain identical.
Formatting passes after this final test update.

## Reproduction

Use the normal Debug and sanitizer builds and run `ctest` in each. The six
registered recursive populations run with the suite. For separate immutable
Release evidence:

```sh
python3 scripts/checked-recursive.py --population source \
  --weavec build/rfc27-validation/final13-bin/weavec \
  --cc build/rfc27-validation/final13-bin/weavec-cc \
  --output build/rfc27-validation/reproduce-source
python3 scripts/generate-recursive-oracle.py --output /tmp/weavec-rfc27-oracle
```

Repeat the first command for `transport`, `objects`, `cache`, `oracle`,
`scaling`, `upstream` and `upstream-objects`. Upstream requires the pinned corpus
checkout and verifies its commit and unchanged source hashes. Run performance
observations sequentially after builds, tests and profiling have stopped:

```sh
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc27-validation/final13-bin/weavec \
  --phase cache --compact-project lua --archive-reports \
  --output build/rfc27-validation/reproduce-cache
python3 scripts/scalability-evaluation.py \
  --baseline build/rfc27-validation/baseline10-bin/weavec \
  --weavec build/rfc27-validation/final13-bin/weavec \
  --phase ordinary --repetitions 3 \
  --output build/rfc27-validation/reproduce-ordinary
```
