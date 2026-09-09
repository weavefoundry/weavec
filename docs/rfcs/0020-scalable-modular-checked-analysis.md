# RFC 0020: Scalable modular checked analysis

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-08
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFCs 0003/0005's scheduling and
  reuse, RFCs 0014/0016's specialization caches, and RFCs 0018/0019's
  explanation representation. Qualifies RFC 0011's may-alias nullness heuristic
  for checked proof facts. Annotation semantics remain.

## Summary

Make ordinary and checked analysis reuse work without reusing stale proofs.
Measure work explicitly; track the dependencies of specialized analyses;
retain immutable function preparation and parsed translation units; separate
semantic convergence from diagnostic provenance; share report explanations;
and offer an opt-in persistent analysis cache for unchanged translation units
under unchanged imported facts. Validate cold and warm runs, invalidation,
recursive convergence, compiler replay, coverage and real-project cost.

The owner authorized drafting the RFC first, followed by implementation end
to end. Acceptance records that authorization; it does not imply independent
review, commit or merge. Implementation and all acceptance checks are complete
in this change. The [validation report](../validation-rfc0020.md) records the
evidence.

## Motivation

At `b5805f5`, all 963 CTest entries pass. RFC 0019's selected Jansson buffer
and UTF encoding interfaces work, but whole-project checked Jansson and Lua
time out at 600 seconds. Completed selected functions number 3/12 in log.c,
18/151 in cJSON, and 20/88 in linenoise. cJSON's checked report is 13.5 MB.
These are separate limitations from ordinary detection, which passes the
44-bug/32-clean fixed evaluation.

`SummaryStore::setInferred` clears every specialization when any summary
changes. A specialization reruns `FunctionDataflow`, including CFG creation,
scope/statement preparation and liveness. Whole-program rounds parse units
again. Callee explanations are copied through callers and participate in
summary comparison. These mechanisms repeat work that has not changed.

This milestone improves execution and reporting. It does not claim that
unsupported pointer cursors, recursive heap invariants, libraries, collectors
or concurrency become supported simply because an analysis runs faster.

## Soundness

Preserve RFC 0018's conditional guarantee, selected scope, entry requirements,
trust and independent failure under warning suppression. A cache is an
optimization, never an additional source of trusted safety facts.

- A changed callee that frees its argument invalidates a caller previously
  checked against a read-only callee. Its later dereference must fail.
- Adding a formerly unavailable definition, changing a callback target,
  changing a counted-field witness/refutation, or changing reference-count
  recognition invalidates every result that relied on the old fact.
- Header contents, macros, include resolution, compiler options, target,
  tool/model version and source contents affect persistent reuse. Timestamps
  alone are not identities. A newly available conditional include matters.
- Compiler object/source/header/command validation occurs before cached link
  analysis can substantiate the object. Rechecking changed source cannot
  validate a stale object.
- Corrupt, incompatible, truncated, incomplete or unverifiable cache data
  is a miss. Cache I/O failure cannot manufacture successful checking.
- Recursive calls cannot establish their own memory postconditions. May
  effects, sufficient requirements and must outputs retain their existing
  joins. Resource limits remain explicit failures of coverage.

Changing only explanation paths must not change semantic convergence.
Separating explanation limits must nevertheless preserve unresolved status:
losing an explanation cannot turn an incomplete contract into a complete one.
Every distinct originating failure remains identifiable, together with the
call dependencies that propagate it. Trust remains transitive and visible.

The existing single-threaded execution model, library assumptions, accepted
conservative rejections and unsupported constructs remain unchanged. No
semantic budget is increased and no checking scope is silently reduced to
meet a performance target.

Checked branch facts and null-object absence propagate only through proven
equal pointer values, using the must-alias relation. A may-alias edge with
zero offset can hold on just one loop iteration; equal nullness records do
not establish pointer identity either. Such an edge cannot justify marking
another pointer null, forgetting its descendants, learning its scalar class,
or establishing its non-nullness. This qualifies RFC 0011's ordinary-mode
nullness heuristic for checked analysis, consistent with RFCs 0018/0019's
proof boundary. The ordinary diagnostic heuristic remains unchanged. A
fast/slow list traversal must retain checks after the fast cursor reaches
null while the slow cursor still points into the list. Traversal order must
not turn those reachable operations into absent obligations.

## Detailed design

### 1. Work statistics

Add optional invocation-owned statistics shared by Frontend and Analysis.
Use `--analysis-stats=<path>` and `-fweavec-analysis-stats=<path>` for a
versioned JSON report. Counts include unit parses/reuses, CFG builds/reuses,
function analyses, block transfers/joins, summary changes, specialization
hits/misses/invalidations, recursive rounds and persistent hits/misses.
Timing separates parsing, preparation, dataflow and persistent validation.
Counts describe work actually performed, including unsuccessful analyses.
Per-function entries use `function:<main-source>#<name>` for dataflow counts
and inclusive timings, and `summary:<main-source>#<name>` for summary extraction
timings. `generic:<symbol>`, `memory:<symbol>` and `callback:<symbol>`
time the full generic or specialized invocation, including state destruction.
Nested function timings overlap and must not be summed as wall time.
Whole-program runs atomically update the requested statistics file after each
unit completes, with `final: false`. The final invocation snapshot has
`final: true`; this denotes completion of statistics collection, not successful
checking. A killed process may leave its last progress snapshot. Progress
output failure retains the explicit-output failure policy below. No snapshot
or timing can replace a completed checked report in the acceptance gates.

Statistics never enter summary equality, cache validity or proof outcomes.
Their output is atomic and an explicitly requested output failure fails the
invocation. Disabled statistics impose no allocations on the dataflow state.
Deterministic counters, rather than timing values, support regression tests.

### 2. Dependencies and specialization reuse

Each cached callback or memory context records the function summaries and
global analysis facts consulted while computing it. Lookups record absence
as well as presence. Nested contexts contribute their dependencies to their
caller even when the nested result was a cache hit.

The dependency vocabulary covers function summaries, callback-global target
sets, reference-count recognition and counted-field facts. Record a stable
semantic revision for each value. Updates invalidate only entries whose
dependencies changed. Global facts may use conservative shared revisions
initially; an unrelated function without such an effect must not invalidate
all contexts. Discovery and changed global facts remain conservatively
scheduled even if the caller has not yet discovered its eventual target.

An active context is never reused as a completed result. Cached diagnostics,
unknown boundaries and other externally visible analysis side effects must
survive reuse. Invalidation must not leave dangling pointers into summaries
being applied by an active caller. Preserve existing context count/depth
limits, annotation precedence and unsafe reporting behavior.

Recursive components use dependency-directed dirty work where possible.
Whole-program invalidation distinguishes imported function summaries from
context requests travelling back to their defining unit. A changed export
invalidates consumers that observed its symbol, declared it as an import, or
use its indirect candidate type. Changed callback/memory requests invalidate
the unit defining the requested symbol. Global names, counted/sized facts and
callback-global changes remain conservative. Local checked-report changes
alone do not invalidate another unit. Capture consulted symbols on every
whole-program analysis, including absent and dynamically resolved callees;
custom units without dependency records retain the existing graph fallback.
This refines scheduling within the existing component, iteration and widening
bounds. It does not reuse a provisional component as a settled checkpoint.
Within a translation-unit run, a silent generic function analysis may be
skipped when every consulted revision is unchanged. Record its own summary
revision before analysis as well, so recursive reads, explicit reseeding and
new incompleteness invalidate reuse. The widening mode must match. A lookup
that changed during analysis remains stale until another analysis settles it.
This memo belongs to that unit's SummaryStore lifetime; reporting passes still
run and no diagnostics or context side effects are reconstructed from a silent
memo. Earlier side effects remain in the same store. Dump mode bypasses reuse.
Imported contracts may also share a single renumbered copy within a
SummaryStore. Key that copy by the source contract, destination AST and an
opaque database generation covering all summary and global-numbering
mutations. Retain generation identities and imported copies for the store's
lifetime, so replacements cannot alias an old key or invalidate a pointer
being applied by a caller. Copies of an unchanged database may share the
generation; mutating either copy creates a new one. Missing contracts remain
misses, and every lookup still records its dependency and context request.
This memo preserves the complete contract, including explanation provenance;
it does not replace semantic dependency validation of inferred contexts.
Publish resolved generic, contextual and imported summaries as immutable
shared contracts. Resolved summaries, call effects and call caches retain the
same contract; do not keep a second full copy merely to extend its lifetime.
Replacing or invalidating a cache entry cannot alter a contract already held
by an active caller. Shipped builtin summaries may use their static lifetime.
Stateful builtin specialization and indirect-target joins construct private
mutable results before publishing them. Reuse includes all explanations and
never skips context capture, requests or dependency lookups. The existing
context and AST bounds govern these caches; no separate snapshot-copy index
is necessary. Count immutable publications and shared call uses in statistics.
Semantic requirements, effects and output guarantees determine changes;
diagnostic route selection alone cannot restart the fixed point. Any
unsettled component remains incomplete and cannot publish a settled cache.

Between completed whole-program unit analyses, discard the stale program
database before constructing its replacement from retained unit exports.
No active analyzer or imported summary may depend on the discarded instance.
After the reporting pass, release that working database before adding the
completed component to the settled database. Keep a single completed export
per successfully reported unit; failed units retain the same previous exports
and failure status as before. Preserve global-numbering reconstruction,
component/member order, diagnostic replay and checkpoint publication. This
bounds simultaneous copies without changing the fixed-point inputs.

### 3. Immutable preparation

Own prepared function data for the lifetime of its Clang AST. Reuse CFGs,
statement roles, scope/unsafe classification and liveness where independent
of caller context. Keep mutable place topology, flow states, entry facts,
summary observations and reporting state local to each run. Prepared data
must not retain pointers into a completed FunctionDataflow instance.

Retain parsed translation units during whole-program analysis and compiler
source replay. Reattach the current program database and reporting options
for each analysis. Reusing the AST must preserve parsing diagnostics, target
layout, source locations and the distinction between a silent approximation
and the final reporting pass. Preparation caches cannot cross AST lifetimes.

Ordinary runs without persistent reuse retain at most one recently used
translation-unit AST after discovery; a currently running unit may temporarily
be the second. Release preparation before its AST and count evictions and
reparses. Discover all unit selections before enabling this bound. Explicit
checked mode, checked function selection or annotations, checked input binding,
analysis dumps and persistent caches retain their existing AST lifetimes. In
particular, eviction cannot rebind a checked AST or a pending checkpoint key
to a later filesystem state. This retention bound changes resource use only.

Use measured transfer, join and state-copy costs to guide further data-layout
changes. Preserve Core's independence from Clang/LLVM. Representation changes
require algebraic and behavioral equivalence tests, including alias joins,
guard weakening and initialization intersection. A range already established
on both incoming edges does not acquire redundant stricter copies with branch
premises. Such copies add no evidence and must not consume the finite range
budget. Limits themselves remain unchanged.
Spatial joins may merge their ordered maps directly instead of copying both
maps to pad missing records on null-object paths. Evaluate object absence from
the incoming nullness and resource domains before joining those domains. The
nullness query reads only the state, without copying guards or witnesses. The
result, including offsets, string witnesses, empty-record removal and the
changed flag, must equal the existing pad-then-join operation. Missing facts
on unknown-pointer paths still weaken the result. Check both operand orders,
self joins and mixed present/absent records against that reference algorithm.
The diagnostic pass may consume each non-exit block's settled entry state and
give its last reachable edge the remaining state. Preserve CFG traversal and
edge order, and retain the original exit state for summary finalization,
checked coverage and dumps. No fixpoint state is consumed while solving.
Alias relations retain value-owned adjacency maps. A shared-row experiment
did not establish a resident-memory improvement, so it is not part of the
final representation.
Merge ordered rows directly, preserving directional offsets and witnesses,
same-share disjunction, alternative-copy behavior, exact-edge intersection,
canonical empty-row removal and exact change detection. Joins remain unions
of edges without transitive closure. Read-only mirror expansion may borrow
ordered edges while that relation remains unmodified; callers that mutate
relations retain owned snapshots. Compare union/intersection with independent
edge-map models and test every mutation for isolation of copied relations.
When invalidating initialization guards, test whether they mention a changed
place directly instead of copying and deleting from a temporary guard. The
query must agree with the existing deletion result for scalar, pointer-pair
and typed-integer predicates.
Forgetting a whole place already invalidates its safety dependencies. The
remaining guarded domains must still be invalidated, including pending
initialization outputs, but need not scan those safety dependencies a second
time. These domains do not consume one another's facts during invalidation.

Index immutable place topology within a function run where repeated full
scans dominate. In particular, discover selected array cells once as places
are appended, bucket them by their scalar selector, and retain creation order.
Index snapshots still consult the current flow state's tracked facts and
descendants before saving a selector. This index cannot cross function runs
or replace the existing array limits, alias rules or snapshot semantics.

Use Clang's reverse-postorder worklist for checked analysis. Retain FIFO
scheduling throughout ordinary analysis: its
legacy heuristic domains can converge differently under another work order,
and a new iteration-limit failure or excess resident memory is not an
acceptable speedup. Mixed FIFO/ordered ordinary scheduling also changed
specialization demand and exceeded the memory gate. Count FIFO and ordered
analyses separately and pin both choices on cyclic and acyclic functions. On an
acyclic graph this processes joins after their predecessors, avoiding
premature transfers of partially joined inputs. Retain the immutable CFG
order with function preparation, and count order construction and reuse.
The worklist still queues a block at most once at a time. Reachability,
inferred nonreturning-edge filtering, transfer and join operations, visit
limits and all semantic budgets remain unchanged. The final diagnostic pass
retains its existing CFG and edge traversal order. Validate acyclic work
bounds, loop and goto backedges, and unequal incoming paths alongside all
mandatory equivalence and coverage gates. Any newly complete function still
requires the obligation-preservation review specified below; changed work
order alone is not evidence of a stronger safety guarantee.

Retain an owned immutable call snapshot while capturing object views and the
call context, then retain the final result in its call cache.
Avoid intermediate copies without borrowing mutable summaries across nested
analysis. Export each definition once when both its ordinary summary and checked
report use it, preserving global-name discovery order. Copy before removing
private output facts only when such a fact is present. Checked diagnostic construction may skip later ledger rows with the
same rendered diagnostic key before allocating notes. Keep the first row in
ledger order, matching the existing stable-sort/unique behavior; all distinct
obligations and call paths remain in the checked report.

### 4. Shared explanations and compact reports

Give originating obligations stable source identities and share immutable
explanation data when copying contracts. A semantic comparison retains
operation identity, property, outcome and completeness, while diagnostic
route and wording tie-breaks are maintained separately. Deduplication cannot
merge different source operations merely because their messages match.

Share individual immutable ledger entries as well as whole-ledger snapshots.
Detaching a ledger copies its ordered index of entry references; unchanged
identities, messages and call paths retain their existing storage. Updating an
entry replaces its reference only in that ledger. Read-only iteration and
lookup preserve the original key order, weakest-outcome join, path tie-breaks
and obligation cap. No entry reference or key view may outlive its owner.
Insertion may reuse a lower-bound position to avoid a second ordered lookup.
When replacing a row, extract the node before releasing its old key owner,
rebind its key view, and reinsert it at the unchanged ordered position.

An invocation-scoped entry pool may also share exactly equal rows produced
by independent contexts. Compare the complete identity, outcome, explanation
and normalized call path before reuse; hashing alone never establishes equality.
The pool holds weak references, is local to the executing thread, and is reused
by nested analysis scopes. Its default index capacity is 262,144 entries. A
Core caller may choose a smaller capacity, including zero. Capacity exhaustion
clears only this optimization index, never a ledger or a proof fact. This bound
does not change any semantic budget. Record pool hits, misses and index resets
in optional invocation statistics. Pool lifetime must not constrain the
lifetime of returned summaries, and expired entries must not remain owners.

The same scope may weakly intern completed ledger snapshots, including their
ordered indexes and cached origin projections. Compare every immutable entry,
including its reason and call path, before sharing; semantic equality alone
is insufficient. The snapshot index defaults to 8,192 entries and is bounded
independently of the row index. Core callers may choose a smaller capacity,
including zero. Evicting either index releases weak references only. Outcome
and exhaustion metadata remain properties of each ledger. Count snapshot
hits, misses and resets separately. Intern completed function ledgers and
decoded checked contracts. Sharing must preserve later copy-on-write mutation
and every report field. Weakly indexed snapshots remain immutable even when
only one ledger retains a strong reference.

When applying a shared call-origin projection, prepare the caller's identity
prefix once. Examine borrowed source, subject, outcome and reason fields before
copying a call path or constructing an owned entry. Entries rejected by the
existing cap or weakest-outcome rule need no such allocation. This bulk path
must match ordinary insertion exactly, including truncation, unsafe conversion,
exhaustion, duplicate identities and the canonical call-route tie-break.

An invocation may cache the callee-specific portion of an originating call
explanation: its bounded subject, escaped subject and bounded reason. Key
this preparation by the immutable origin projection's identity, whether its
trusted or unresolved sequence is selected, the callee name and unsafe
conversion. Validate a weak owner before reuse so a recycled address cannot
match an expired projection. Caller location and function remain inputs to
each application, and source call paths remain those of the retained immutable
projection. Apply prepared entries in their original sequence; preparing them
must not pre-join, sort or truncate that sequence according to another ledger's
remaining capacity. Preserve string-truncation exhaustion even for entries
the destination rejects. This is explanation reuse, never proof reuse.

The preparation index defaults to at most 1,024 entries and 64 MiB of
conservatively accounted retained strings, vectors and index entries. Oversized
entries and callee keys longer than 4,096 bytes use uncached preparation.
Exhaustion clears only this optimization index. Core callers can lower either
bound, including to zero; nested scopes share the outer invocation's bounds.
Record hits, misses, rejected publications and index resets separately.

Join two ordered ledgers by advancing through their keys in order. Existing
rows keep their position; newly accepted rows use that position as an insertion
hint. Detaching shared storage invalidates its iterators, so locate the current
key again after the first detachment. Preserve the prior source-key order,
weakest-outcome rule, route tie-breaks and cap behavior. No row may be skipped
merely because its source ledger is semantically equal with different routes.

An obligation's call path may use shared immutable storage independently of
the enclosing row. Copying a path preserves its normalized representation;
editing a copy detaches it and invalidates that normalization marker. Reusing
an already normalized path must preserve opaque-handle removal, cycle collapse,
the existing depth bound and the final origin. Read-only iteration cannot
expose mutation that bypasses detachment. This sharing carries no proof facts
and does not change path comparison, serialization or report contents.

Provide a compact checked report representation with one table of originating
obligations and references from functions/call edges. It records selected
scope, sufficient requirements, guaranteed outputs, trust, limits, deferral,
completion and invocation success as before. Consumers can trace any selected
failure to its source and enumerate affected callers without an exponentially
expanded call tree. Recursive paths retain the existing bounded, cycle-collapsing provenance;
shared path indices preserve exactly what the expanded report records.

Keep expanded version-2 reports available for compatibility; select the
compact version-3 representation with `--checked-report-format=compact`
(compiler spelling `-fweavec-checked-report-format=compact`). The default
remains expanded until consumers choose compact. Tests compare expanded and
compact semantic content, not only JSON validity. Stable ordering makes both
forms deterministic for identical inputs. Publish reports directly to the atomic
output stream. Compact encoding interns records from contracts without first
materializing expanded JSON. Validation reads expanded reports one function at
a time; compact expansion may retain shared tables, but must not materialize
all expanded functions together. These representation changes preserve every
report field and the canonical expanded content.
Core summary format 15 remains unchanged. Sidecar format 16 adds a
`checked-preprocessing` digest binding the object to its effective preprocessed
input. Link validation compares that digest before source replay or any cache
hit. This closes the case where a newly available conditional include changes
the analyzed body without changing any previously loaded source/header bytes.
Objects with older sidecars must be rebuilt. Unsupported preprocessing inputs
cannot substantiate a checked object; ordinary compilation remains available.

### 5. Persistent translation-unit checkpoints

`--analysis-cache=<directory>` and `-fweavec-analysis-cache=<directory>`
enable an optional local cache. No cache is used by default. A checkpoint
contains a settled unit result, its full diagnostics and input/dependency
identities; it is not a compiler object or an external library contract.
Bind an input identity when its retained AST is created, not when a later
component requests a checkpoint key. Capture preprocessing before parsing and
verify it afterwards. An observed change makes a source unit ineligible for
persistent reuse; a checked compiler replay with a changed object binding
fails. Subsequent source edits must not attach a newer identity to the older
AST. As with compilation, inputs must remain stable during parsing; these
checks detect observed drift rather than provide a filesystem transaction.
Writes use bounded, versioned, validated records and atomic replacement.
Private checkpoint format 2 extracts obligations from each checked definition,
generic summary and callback/memory specialization. Share strings, locations,
call paths, exact obligation rows (including the originating function), and
ordered ledger rows. Preserve each ledger's exhaustion flag and require exactly
one ledger reference per contract in deterministic traversal order. The remaining
unit record retains all contract metadata, requirements and outputs. Validate
its producer round trip before writing, comparing globals by their names rather
than incidental table numbers, and validate every table index, enum,
normalized path, duplicate identity and ledger bound before restoring contracts.
Reject missing or extra references; publish no partially restored unit. This
format avoids the per-contract expanded-text transport bound without changing
Core summary or compiler sidecar formats. Input-fact fingerprints use the same
lossless compact explanation representation. Ledger joins may retain existing
normalized immutable row references directly, preserving ordinary insertion's
outcome and explanation tie-breaks even without an interning pool.
Large payloads use LLVM's optional zstd support: encoded records are bounded
at 256 MiB and decoded payloads at 4 GiB, with both bounds checked before
decompression. Unsupported compression or larger records lose reuse. This
I/O bound is independent of all semantic analysis limits. Small records remain
plain JSON under the same checksum and schema validation.
Recursive units are published and validated as one settled component, with a
unit result and reporting facts for each member. Changed component members
require reanalysis; independent components remain reusable. Symbol projections
include observed missing lookups. Global facts, callback candidate buckets and
context requests may be conservatively fingerprinted together.

Persistent granularity is a translation unit. Within an invocation, function
and context dependencies provide finer reuse. An edit inside a unit can
reanalyze that unit; unchanged units whose relevant imported facts remain
unchanged reuse their checkpoints. An unchanged warm invocation performs no
function dataflow for a reusable settled unit. Input validation, preprocessing
or parsing needed to verify its identity is still counted and reported.

Fingerprint the effective compiler command and working directory, target,
tool/model/cache versions, checking and diagnostic options, source/header
contents and preprocessing outcome. Validation must observe include search
changes, `__has_include`, forced includes and volatile preprocessor macros;
hashing only previously included files is insufficient. Unsupported input
mechanisms disable reuse conservatively. Output paths and statistics are not
semantic inputs, but requested selection and warning controls are.

The imported-fact fingerprint includes all dependencies of the unit's result,
including missing symbols and dynamic callback/context requests. It may
conservatively include additional dependencies. Program membership changes
must account for newly available definitions and targets. Preserve unit
discovery and ordering before deciding imported results are reusable.
The fingerprint's private function-table keys use distinct callable, external
and indirect namespaces with lossless hexadecimal encoding of the original
symbol or type key. Type spellings and source-qualified symbols may contain
spaces and cannot be inserted as raw C function names into summary metadata.
Encoding changes only the fingerprint representation; dependency lookup and
published source-level names remain unchanged. Unrepresentable metadata must
still lose reuse rather than omit imported facts.

Replay cached diagnostics with source locations from the validated input,
honoring deduplication and the final pass. Cached and uncached runs agree on
exit status, semantic reports and diagnostics. Dump/fix-it modes that cannot
be reproduced faithfully bypass the cache. A read/write cache failure is a
miss or lost optimization, not a memory-safety diagnostic. Explicit statistics
and checked-report output failures retain their existing failure policy.

### 6. Frozen evaluation and acceptance

Preserve the `b5805f5` Release and development binaries and their SHA-256
identities before implementation. Retain RFC 0019's published measurements
and original manifests. Add a frozen RFC 0020 acceptance manifest before
changing analysis code, covering:

- cold/warm checked and ordinary invocations;
- unchanged and unrelated-function updates preserving specialization reuse;
- leaf callee edits changing effects, requirements, outputs and trust;
- direct and indirect missing definitions becoming available;
- callback/global/count/sized-field changes and nested cache hits;
- header contents with preserved timestamps, include-search changes,
  conditional includes, macros, targets and compiler options;
- stale compiler objects, corrupt/truncated records, concurrent atomic writes,
  unwritable cache directories and unavailable source inputs;
- recursive calls, context/iteration limits, and explanation deduplication;
- equivalent inline/helper/cross-unit/compiler-object positive/negative cases.

All existing ordinary and checked evaluations remain green, including the
unchanged pinned Jansson interface cases. Run Debug and ASan/UBSan CTest,
formatting and relevant strict clang-tidy. Record full scope and all failures.
Any newly complete function requires preserved obligations and an explanation
of the old artificial limit or redundant work removed. Diagnostic changes
must be reviewed by originating location and reason.

Cold whole-project checked analysis must finish with a report on all five
pinned projects within 600 seconds each, with the same selected scopes and
semantic limits. Incomplete contracts are valid reported coverage; timeout
and missing reports are not. Target Jansson at most 180 seconds and cJSON and
linenoise at most half the RFC 0019 recorded time. Publish actual observations
even when targets are missed; do not mark the RFC Implemented with an unmet
mandatory acceptance gate. Compact cJSON and linenoise reports should be at
least five times smaller while preserving their semantic content.

Run three sequential ordinary Release corpus observations per baseline/final
binary after builds and tests finish; median time and peak RSS may grow at
most 10%. Measure cold and warm checked cost separately, with cache mode and
binary identity recorded. Warm unchanged acceptance requires zero function
analyses for reused units, not merely a faster elapsed time. A frozen leaf
edit across units verifies that unaffected dependency closures are reused.

## Annotation surface

None. Selection and ownership annotations retain their meanings.

## Diagnostics

No new safety diagnostic identifier. Preserve `analysis-incomplete`,
`checking-incomplete` and `checking-failed` and their existing severities.
Compact reports group explanations without suppressing originating failures.
Invalid CLI options and explicitly requested report write failures are
frontend invocation errors. Cache failures are reported in statistics.

## Drawbacks

Dependency bookkeeping and invalidation are new soundness-sensitive code.
Retaining ASTs increases resident memory; bound retention if measurements
require it, and count reparses. Persistent validation itself costs time and
may reasonably miss on complex builds. Shared explanations introduce another
report schema and require compatibility tests. Fine-grained reuse can reveal
previous accidental dependence on analysis order; such cases must be fixed
without changing the accepted safety rules.

## Alternatives

Pointer-cursor proofs, recursive container invariants, more library contracts
and archive packaging remain valuable next milestones. They add work to an
engine whose checked whole-project runs already time out. Increasing limits,
trusting incomplete callees, dropping selection, or treating a timeout as a
completed incomplete report would defeat the acceptance criteria.

An invocation-output cache alone makes identical reruns faster but cannot
reuse unaffected units after a leaf edit. A full new intermediate language
or solver would broaden the change unnecessarily. This design retains the
existing Clang CFG and Core domains.

## Prior art

RFC 0010 already schedules whole-program members only after imports change;
extend that principle to contextual results and persistent unit checkpoints.
RFCs 0014/0016 define bounded context identity. RFC 0018 supplies checked
artifact binding and explicit trust; RFC 0019 separates semantic contracts
from bounded explanation identity. Existing Clang AST/CFG ownership provides
the lifetime boundary for immutable preparation caches.

## Unresolved questions

No user-facing semantic decision is deferred. Profiling determines internal
data layouts and conservative dependency granularity. Acceptance thresholds,
frozen scope and unchanged safety limits cannot be silently weakened after
measurement. A failed performance gate remains unfinished work.

## Future work

Pointer-cursor/loop reasoning, recursive heap invariants, broader I/O and
formatting contracts, archive distribution, concurrency and runtime checking.
Persistent function-level reuse across edits within a translation unit can
extend these unit checkpoints in a separate milestone.
