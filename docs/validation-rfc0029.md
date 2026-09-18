# RFC 0029 implementation and validation

This record accompanies the in-progress implementation of
[RFC 0029](rfcs/0029-compositional-recursive-workflows.md). The RFC remains
Accepted. Passing the smaller workflow populations does not satisfy the full
cJSON parse/print goals or establish arbitrary recursive construction.

## Implemented mechanisms

- Semantic buffer candidates with additional fields, loop-index bounds, and
  validated roles imported from separate-source contracts.
- Flow-sensitive entry allocation identity across later pointer reassignment,
  without exporting local parameter-slot initialization as caller memory.
- Shared affine projection excludes overwritten numeric fields, so a changing
  reader cursor requires the complete iteration interval rather than its entry
  cell. Existing relational envelopes supply stable interface endpoints.
- Private direct/mutual cleanup, read-only traversal and fresh-construction
  hypotheses, complete group validation and a bounded progress graph.
  Forwarding edges require strict progress on every cycle. Specialized direct
  cleanup uses the same progress machinery.
- Construction over decreasing initialized byte intervals, immutable entry
  count snapshots, fresh forest outputs and every-exit allocation accounting.
  Complete helper contracts support reads and partial-tree cleanup. Successful
  attachment retains a child's proved parent relationship across later stores
  to other child slots, without admitting shared ownership.
- In-place extension of an owned singleton head with an unsigned decreasing
  count, an unchanged live head on every return, and an explicit fresh-region
  ledger. Preserved neighboring forests retain their established separation.
- Private group members cannot publish nested callback or memory contexts.
- Integer success interfaces can publish a fresh forest through an output
  pointer. Positive returns establish the full forest and zero returns must
  actually leave the slot null. Immediate result tests transfer the conditional
  allocation footprint, including a fresh child written into a parent slot.
  The single-slot helper has no other mutating effects. General deferred result
  tests still require separately invalidated pending evidence.
- Copied reader records nominate a unique byte pointer and remaining count
  from evaluated accesses and decrements. The proof requires the full input
  interval and compares recursive progress with immutable field snapshots.
  Direct updates to unexposed automatic cells preserve separate heap facts.
- Mutable reader construction requires a live initialized writable record,
  separated from its readable byte interval. Its hypothesis invalidates the
  changed reader fields and supplies no post-call bounds. Writes confined to
  a live unchanged entry object preserve only independently proved fresh
  forests, while retiring changed pointer-cell names.
- Explicit allocation and release callback premises, checking of actual targets,
  visible trusted boundaries, and portable input-only contract records.
- Exact zero-initialization frames outside represented writes and concrete
  ownership-forest establishment after attachment.
- Target-aware proofs for finite floating constants and unchanged scalar inputs
  under dominating true bounds. NaN, infinity, rounded integer limits, mutated
  values, address exposure, bypassing jumps and nonstandard modes remain checked.
- Checked `strtod`/`strtof`/`strtold` memory boundaries preserve bounded
  end-pointer provenance, require terminated initialized inputs and writable
  separate output slots, and establish no floating result range.
- Summary format 25, checked encoding 11 and sidecar format 26, with no older
  sidecar reader.

The recursive group rule currently covers same-record, one-pointer-parameter
cleanup and read-only traversal without hidden global effects or arbitrary
external helper calls. Initial constructors take a constant byte pointer and
unsigned remaining count and return a nullable fresh initialized forest.
Partial-consumption/in-place construction, owned-tree writers and broader
floating relational inference remain outstanding. Byte-interval writers are
covered by candidate25 below. No broader interface is
certified by these smaller contracts.

## Frozen populations

The primary 15-case population was frozen before checker edits. Its baseline
rejected stateful runtime/generic writers, reassigned release, mutual cleanup
and generic allocation callbacks. The unchanged reader and adversarial cases
are preservation checks. The full primary population passes the development
candidate.

The independent serializer freezes runtime/generic hexadecimal encoding and a
short-input counterpart. The transport population freezes a closed client of
mutual cleanup plus allocation/release callbacks, generic interfaces and an
undersized allocator. Each population carries source hashes and provenance.

Development object checks pass both closed clients and their intended negative
counterparts. Cold, warm, uncached and compact reports match; the unchanged
transport workflow reuses two units with zero function analyses. Removing a
child release invalidates the proof. Corrupt checkpoints recompute and older
sidecars reject. The reproducible runner is `scripts/checked-workflows.py`.

The pinned cJSON print/delete client passes candidate71a with zero entry
requirements. The original parse/delete client remains incomplete; its
automatic input also has a separately corroborated error-pointer lifetime
defect on allocation failure. Original expectations remain unchanged in the
upstream manifest, with the defect recorded in a separate audit population.
General parser construction, string decoding, nested serialization, and the
remaining whole-project validation gates are still outstanding.
No upstream source was changed and no third-party trusted summary was added.

The later reader population proves a runtime consume loop and separate-source
client, and rejects truncated and partially initialized input through source and
ordinary objects. Its original probe included an invalid negative: `take`
returns before accessing a cursor already beyond the extent. The original
manifest and failed observation remain unchanged. A separately frozen reviewed
manifest expects that safe case to pass and adds an actual escaped access.

The later `traversal` population proves borrowed direct/mutual traversal and
rejects interior pointers, mutation and cycles without progress. An audited
forwarding case was correctly safe: its cycle still contained a strict child
edge. Its original manifest and failed observation are preserved, alongside a
separate reviewed manifest and an actual nondecreasing cycle.

The `construction`, `mutual-construction` and `construction-helpers` populations
cover runtime input, forwarding cycles, binary-tree construction and partial
failure cleanup. Short/uninitialized input, missing or repeated releases, and
nondecreasing recursion reject for their intended obligations. Each population
has its own immutable inventory and provenance. The combined
`recursive-transport` population verifies construction, traversal and cleanup
across separate units. Cold/warm/uncached/compact reports match, with zero warm
function analyses; changing cleanup invalidates dependent proofs. Construction
and combined clients also run through ordinary objects and checked linking.

The `reader-construction` population covers a reader passed by value, including
an unrelated depth field. Its seven frozen cases accept construction and reject
truncated/uninitialized input, nondecreasing recursion, escaped intervals,
leaks and repeated release. Candidate 22's source run and two focused unit tests
pass. The separately frozen `mutable-reader-construction` population applies
the same seven intended properties to a pointer-reader interface; its candidate
22 baseline rejects the positive, and candidate 23's source run passes all seven.

The `numeric-input` population freezes seven libc-boundary cases. Candidate 24b
passes the two positives and the intended initialization, write-permission,
bounds and user-definition rejections. Its initial development candidate
mistook an address of a null-valued slot for a null slot; the corrected test
requires an actual null pointer value, and the failed observation is retained.

## Development regressions

Broader validation found and corrected two precision regressions in role
selection: local loop-index bounds and opaque/imported layouts. It also exposed
a soundness gap in existing cleanup specialization: a root node's `dispose`
callback had been reused as evidence for child callbacks. Recursive hypotheses
now reject that per-node substitution. The existing adversarial test with a
child callback that frees twice remains a required rejection.

The reader probes exposed a pre-existing false proof in both HEAD and candidate
15. A loop advancing `r->position` exported a requirement for only the incoming
cell. A two-byte input with an advertised end of four was accepted; running that
unchanged client with ASan produces a stack-buffer-overflow in `consume`.
Affine projection now applies the numeric-write exclusion already used by typed
integer projection. The inferred requirement covers `[0, r->end)` instead. The
valid client remains complete and both invalid inputs reject. Full Debug
validation after this correction passes all 1,372 tests.

The proof-boundary review then found a new candidate-17 defect: recursive
forwarding resolved `p + 1` to the ownership identity `p` and could consume its
footprint using the unchanged-pointer hypothesis. The corrected rule requires
an exact zero-offset copy, including represented spatial offsets and element
identity. The frozen offset population rejects nonzero forwarding and child
arithmetic while preserving `p + 0`. The invalid forwarding client now reports
the actual release one element past its allocation. Candidate 17's ordinary
measurement was interrupted and retained as an aborted development observation;
it is not final cost evidence.

Core tests exhaust all 19,683 absent/non-strict/strict three-node progress graphs
against an independent transitive-closure oracle. Byte-frame tests compare
retained ranges against concrete byte masks. Analysis and lit tests exercise
null exclusions, callback forwarding and hidden effects, wrong allocation
sizes, same-object recursion, skipped children, shared/cyclic ownership and
stale allocation aliases.

The first resumed full Debug suite passed all 1,390 tests after preserving the
existing cleanup diagnostic wording and fixing object-harness handling of an
ordinary double-free diagnostic. Object generation now lowers ordinary WeaveC
errors to warnings; checked linking must still reject the same frozen mutant.
After the tree-construction changes, all 1,395 tests pass in both Debug and
ASan/UBSan builds. The unchanged fixed evaluation detects 44/44 bugs and accepts
32/32 clean cases, with no parse/tool failures or timeouts. A separately run
concrete allocation ledger covers all 3, 3 and 7 allocation failure points in
the frozen direct-chain, mutual-chain and binary-tree clients. Leak mutants
leave live allocations; repeated cleanup triggers the ledger or an ASan
heap-use-after-free while traversing the already freed child.

The isolated three-run ordinary Release gate passes against the retained RFC
0028 executable (`47b881a…`). Across the unchanged five-project corpus, median
elapsed time is 150.386 seconds versus 151.293 seconds (0.9940×), and median
peak RSS is 650,428,416 bytes versus 651,575,296 bytes (0.9982×). Both satisfy
the 1.10× ceiling. Candidate `c46122a…` and all raw observations are retained in
`build/rfc29-validation/candidate19-bin` and `performance19-ordinary`.

Candidate 19's checked corpus run found two precision regressions and was
interrupted during Lua; its partial reports are preserved. An external child
destructor in cJSON_Utils incorrectly nominated a partial local topology,
masking the imported tree's next link. Discovery now uses actual call-graph
components for mutual edges. Separately, discovering a buffer hid the scalar
entry-validity premise needed by `update_offset`. Pointer formation may now
request that premise only for its exact unchanged entry backing, with no
replacement or invalidation. The frozen `corpus-regressions` reductions pass;
their unterminated-input counterpart still rejects. The broader corpus and
final performance gates must be repeated after these corrections.

Candidate 20 passes all 1,398 Debug and ASan/UBSan tests, strict lint for the
corpus corrections, and formatting. Both lost helpers are complete again in
the actual cJSON/cJSON_Utils analysis. Its unchanged upstream workflow run still
rejects both required positive clients; the double-release counterpart rejects
as intended. That failed observation is retained in `upstream20`.

Candidate 21 adds output-slot construction, with eight frozen source cases,
five unit tests, source transport, and complete cold/warm/uncached/compact
checkpoint comparisons. The candidate-20 baseline rejects the positive and
misses several intended negative explanations; those observations are retained.
Mutual forwarding also proves through the group progress check. The broader
Debug run passes all 1,406 tests. The subsequent subobject-bounds correction
passes six focused unit tests and 25 targeted CTest checks, including lit,
objects, allocation oracles and output transport/checkpoints.
Addressed members retain their own subobject bounds even
while their memory identity is projected through the enclosing allocation.

Candidate 22 passes all 1,410 Debug tests, including the seven copied-reader
source cases, object linking and the allocation-failure oracle. The first
candidate lacked field initialization and discarded fresh-forest facts on a
copied cursor update; those failed observations are retained as `reader22` and
`reader22f`. Its copied-reader positive uses actual interval and allocation
proofs, and its seven-case mutable-reader baseline remains incomplete.

Candidate 23 passes all 1,348 unit tests and 67 integration tests, including
mutable-reader source/object/oracle checks and mutual reader forwarding. The
initial strict lint observation found four style issues, subsequently corrected.
Candidate 24 passes the focused numeric-boundary tests, strict lint for the
changed files, formatting, the Core include boundary and the unchanged fixed
evaluation (44/44 bugs, 32/32 clean). Its Release executables are retained in
`candidate24-bin` with an identity manifest; full Debug, sanitizer and upstream
observations are being recorded separately.

## Remaining validation gates

Formatting and the recent changed-file lint and Core include checks pass.
The earlier isolated ordinary performance result applies to candidate 19;
the final code still needs that gate and complete checked corpus identity
preservation. Mandatory upstream and unimplemented semantic goals must also
be satisfied. Earlier development observations are retained under
`build/rfc29-validation`; generated binaries and reports are not committed.

### Candidate24 completion checks

The full ASan/UBSan suite passed all 1,419 tests (1,351 unit and 68
integration; 241.03 seconds), retained in `asan24-full.log`. The Debug run
passed 1,418/1,419; its sole failure was the numeric-input lit expectation
using `checking-incomplete` instead of the actual `checking-failed` write to
read-only storage. After correcting that expectation, all 143 lit cases passed
in `lit24-message.log`; no checker code changed for that correction.

Immutable Release candidate24 (`candidate24-bin/identity.json`) still fails
both mandatory cJSON positives, parse/delete and create/print/delete/release.
The double-release negative passes. Reports are in `upstream24/source.json`;
these are unresolved gates, not waived acceptance cases. The fixed evaluation
remains 44/44 bugs detected and 32/32 clean programs accepted (`fixed24.json`).

The new writer candidate development retains `writer25-baseline.json` and the
original `recursive-writer24-baseline.json`. The original writer expectation
audit is recorded beside its immutable fixtures. A separately frozen reviewed
population distinguishes complete initialized-prefix evidence from a functional
claim that a success result copied all input.

### Candidate25 recursive byte writers

Private writer hypotheses share the construction interval-progress machinery
and require an actual writable buffer with separated header, input and backing.
Each body must establish its initialized prefix on every return. All 11 reviewed
source cases pass (`writer25c.json`), as do three targeted unit tests. The first
attempt failed because separation forwarding lost a buffer entry identity;
the second still lacked an explicit failed-writer-output diagnostic. Both
observations are retained. The shared separation projection now uses the same
immutable backing evidence as `memcpy`.

Separate-source transport passes four cases, including an independently
selected generic writer (`writer25-transport`). Cold/warm/uncached and compact
reports agree; warm execution performs zero function analyses, and mutation,
corrupt cache and old-sidecar checks pass (`writer25-cache`). The independent
ASan/UBSan oracle checks 18,513 finite input/capacity/initial-prefix combinations
and detects the false-prefix mutant (`writer25-oracle`). These finite runs do
not establish unbounded recursion. Tree serialization, complete-copy length
claims, in-place parsing and the cJSON gates remain required and unresolved.

Candidate25's full Debug run passed 1,425/1,426 tests. The one failure was
object-runner selection: it selected only `main` for the writer off-by-one case,
although that fixture's bound error is in the independently selected generic
`emit` (the concrete client stops before that bad boundary). The runner now
preserves both selections for the reviewed writer population; no fixture or
expectation changed. `objects25e` retains the corrected run. Strict lint's three
style findings (explicit pointer-to-bool comparison and two qualified `auto`
variables) were corrected; `tidy25e.log` is clean, and the other candidate25
changed implementation files passed their original strict lint invocation.

### Candidate26 cases and candidate27 reader cursors

Candidate26 admits a separately rechecked case that proves its recursive edge
unreachable, while retaining the selected generic summary's exhaustion. Exact
live scalar pointees can supply case values; incompatible, partial and dead
storage cannot. All five frozen recursive-case checks pass. The first full
Debug run passed 1,421/1,430. One new test used an unavailable standard header;
the other failures exposed imported buffer registration after the tightened
nomination filter. Returned objects now validate their transported descriptor,
and forwarding helpers import that descriptor before entry initialization.
The stable candidate27c rerun passes all RFC0026 source transport, object and
cache checks. RFC0028 source/object and regression checks passed candidate27;
a subsequent full-version suite is still required.

The `reader1:` descriptor distinguishes a cursor into fully initialized input
from a writable initialized output prefix. It shares strict layout transport
but cannot imply writer permissions or ownership. Candidate27 passes all 11
reviewed cursor-reader source cases, 13 Core buffer tests, and the targeted
reader/scalar-capture unit checks after correcting two test harness mistakes.
The original cursor fixture audit and baseline remain alongside the reviewed
inventory: generic contracts may have premises, and an advertised unread tail
alone does not constitute a concrete erroneous access. Partial consumption
exports only the bounded reader predicate, not a specific consumed amount.
The mutation/cache and current full sanitizer gates remain pending.

A separate frozen recursive-context population demonstrates exhaustion caused
by many refinements of mutable output cells before a concrete input selector
arrives. Candidate27 rejects its positive. Candidate28 investigates bounded
nomination without increasing, evicting or resetting the context budget. Its
outcome and the mandatory cJSON workflows must be recorded independently.

### Candidates28–29: bounded input cases and mutable output contexts

Candidate28 completed 1,431/1,437 Debug tests. Its six failures exposed reader
candidate overnomination and propagation of generic exhaustion into cases with
separately verified peer base cases. Reader nomination now requires an observed
indexed counter/extent comparison, and actual exhausted-reader cases may check
their early-return path without importing an inapplicable generic predicate.
The audited cursor inventory retains the original cases and adds the actual
extent explanation to one negative's diagnostic matcher. It does not change
that case's rejection requirement.

Candidate29b passes the full Debug suite: 1,440/1,440, including 1,361 unit tests
and 79 integration tests (282.96 seconds, `dev29b-full.log`). The mutual-case
population now passes all four cases: stateful helpers capture forwarded input
selectors and an executed call to a separately completed peer base case can
complete, while the unbounded and invalid alternatives remain rejected.
The recursive-output population passed its baseline as well; it is preservation
coverage, not evidence of a prior missed positive. No context count or nesting
bound was increased, reset or evicted. Three strict lint style findings were
corrected in candidate30; their original reports remain retained.

Candidate28's cursor oracle checks 1,123 finite cases for each ordinary and
partial reader and detects 74 escaped-cursor mutant failures. Its cursor cache
checks pass, including identical canonical cold/warm/uncached reports, zero
warm function analyses, compact encoding, mutation, corruption and old sidecar
rejection. These finite oracles do not certify arbitrary runtime input sizes.

The immutable candidate29b Release executable has SHA-256
`246dd010435569ba0b37ddf29065927678dae28c86685b50fe489bd931ac602b`;
its driver is
`7c06668610c6de23ba83caf6217b3a5ce6a0165cd012ac4ed7ca1ec8e3b95152`.
The matching development identity is retained separately under
`candidate29b-dev-bin/identity.json`. Upstream validation remains **failed**:
`upstream29b/source.json` rejects parse/delete and print/delete, while the
parse/double-release negative passes. Candidate28 had the same three outcomes.
Those observations preserve the unchanged upstream sources and mandatory gates.

### Candidate30: record arrays and current zero-byte evidence (in progress)

A reduced stack-array writer exposed two RFC0015 implementation gaps: arrow
field access lacked a checked byte offset, and arrow selection did not name the
same exact record cell as subscript/dereference spellings. Selected field paths
now agree through forwarded contracts, and buffer arguments validate the actual
selected record's layout. The first candidate passes five of the six frozen
record-array cases; its writer still lost a terminating zero after a header
update. Original and intermediate observations are retained.

Zero-valued ordinary integer cells can now be recovered from complete current
zero-byte intervals in automatic storage. All six isolated zero-counter cases
pass candidate30c, including partial memset and later direct/helper writes.
The original multi-function inventory and failing baseline remain immutable;
`zero-counters/audit.md` explains source isolation needed to avoid unselected
negative functions making positive command invocations fail. Its diagnostic
matcher also records the bounds explanation omitted from the original regex.
No expected safety property changed. Candidate30d passes all 76 focused array
and buffer unit tests. Broader validation and zero-frame preservation are
pending; this is not the RFC's final validation.

Candidate30g passes the full Debug suite: **1,448/1,448**, comprising 1,366 unit
and 82 integration tests (`dev30g-full.log`, 292.54 seconds). All six record-array,
six audited zero-counter and three zero-frame cases pass. The terminating-byte
regression has a focused unit test, and record-array/partial-byte diagnostics
are pinned by lit. Zero preservation now reuses the shared immutable-entry
separation projection, including backing identity retained when a logical
length update retires the full buffer predicate. Neither aliased writes nor
later nonzero stores preserve the zero.

The immutable candidate30g Release executable is
`7e693145e977a4ce0200a06bba32c65e655f9fb68b34965b3a98a997d81481a4`;
its driver is
`f784fd3f7d5da86138d831a0580e63f964db062e771d76d0b184fa3559c09c3a`.
The candidate30e upstream run still fails both positive workflows (183.74
seconds for the three-case population), while preserving the double-release
rejection. Its print context now captures the object's type, null child,
zero cursor/depth, capacity and concrete hooks; downstream helper case limits
remain unresolved. Candidate30e predates the final zero-frame fix, so it is not
a claim about the candidate30g upstream outcome. Formatting and the Core
boundary pass. Strict lint found a repeated branch body, consolidated in
candidate30h; full current sanitizer, fixed evaluation, corpus and final cost
gates remain pending.

Candidate30g also preserves the unchanged fixed evaluation: 44/44 detected
bugs and 32/32 clean cases, with no parse errors, tool errors or timeouts
(`fixed30g.json`). Candidate30h adds an early zero-range lookup to avoid layout
walks for integer cells with no applicable byte evidence and consolidates the
identical array access branch flagged by clang-tidy. Its full sanitizer run is
recorded separately from the following callback scheduler change.

### Candidate31: combined callback and scalar entry cases (in progress)

Direct calls that already supply both callback bindings and captured memory or
scalar facts now request their combined context directly. This avoids first
checking an intermediate callback-only body with unknown selectors. All actual
callback alternatives and case premises remain in the existing canonical
context; budgets and incomplete fallback behavior are unchanged. The five
frozen separate-source cases pass both their candidate30g baseline and
candidate31, so this population establishes preservation, not a previously
missed positive.

The first targeted unit run passed 88/89. The remaining test assumed concrete
callback bindings always appeared in the callback-only request map. It now
checks both callback-only and combined requests for the same required known
target, while still requiring the generic function to remain incomplete and
the concrete caller to be complete with no entry requirements. A new focused
test verifies that a combined call does not create the intermediate request;
both tests pass candidate31b. The first unit failure remains in
`context31-unit.log`. Broader validation and the upstream outcome are pending.


Candidate31b's full Debug suite passed 1,450/1,450 (1,367 unit and 83
integration tests, 391.35 seconds). Candidate30h's ASan/UBSan suite passed
1,448/1,448 (1,366 unit and 82 integration tests, 397.49 seconds). These are
separate executable identities and do not validate subsequent changes.
Candidate31 still rejects both required cJSON positives; its three-case
population took 277.45 seconds while other test work ran. The negative
retained its intended rejection. This is not an isolated cost measurement.

### Candidate32: nomination from settled recursive inputs (in progress)

The immutable `recursive-state-cases` population has 36 generic callers with
varying constants in a mutable output record. Its concrete client supplies a
known input node; the other cases supply a null output or select the generic
recursive definition. Candidate31 rejects the positive and retains both
negatives (`recursive-state-cases31.json`). The proposed scheduler postpones
scalar-only cases inside generic recursive approximation and prioritizes
represented read-only record inputs over unrelated output-state constants.
No existing case is evicted and no bound is changed.

Candidate32 passes all three `recursive-state-cases` cases and all 91 targeted
compositional/recursive unit tests. Its context and translation-unit files
pass strict clang-tidy. The extended upstream population was frozen against
candidate31 before its first run; it adds nested parse/print/release, malformed
partial construction, first-allocation parse failure, and print allocation
failure after creating the input object. These positive proof obligations
remain mandatory.

A separately frozen finite cJSON oracle checks eight valid/malformed documents
with a concrete allocation ledger, expected serialized bytes, and each
allocation failure point observed on the successful path. All 68 executions,
including 52 injected failures, passed ASan/UBSan. This audits only these finite
executions and supplies no checker summary or abstract proof.

Candidate32's full Debug suite passed 1,452/1,452 (1,368 unit and 84
integration tests, 330.31 seconds). Its Release executable is
`a449728a1960c52452b92b54d5baec2e991d2e02083e9aa98556ce8836cf1340`
and driver is
`284e483be784df212bd91a4b215bad74c140e4ce76499005c96d2dd6c25d071e`.
Its cJSON population still fails both positive proofs and preserves the
negative (69.65 seconds total, not isolated). The empty serializer now has
one relevant print-object case and six growth-helper cases; its remaining
print-object failure concerns the initialized prefix at the second reserve.
All four extended upstream cases fail their candidate31 baseline; their
original expected acceptance is retained.

### Candidate33: buffer identity and physical extent (in progress)

The new immutable `writer-return-aliases` population writes through a helper's
returned pointer, advances the header, obtains another pointer, terminates the
output, reads it, and frees it. Candidate32 rejects the positive and rejects
both missing/incorrect-first-byte negatives for initialization/termination.
Candidate33 preserves an independently established physical allocation extent
when materializing a smaller logical buffer capacity. It also normalizes newly
registered imported descriptors and retains surviving entry backing identities
when applying verified buffer outputs. No logical capacity establishes new
storage and no pointer replacement revives the old entry identity.

The focused source population passes all three cases in candidate33e. Temporary
trace observations are retained under `return-aliases33*-trace.log` and
`print33b-trace.log`; trace instrumentation has been removed from the source.
Broader validation and current upstream results remain pending.

The first candidate33 buffer unit run passed 24/26. The existing
`FailedPositiveAllocationDoesNotBecomeLiveAtZeroCapacity` negative exposed
an unsound intermediate change: the larger physical extent was also being
used to infer non-null storage. Candidate33f separates that extent from the
logical buffer's conditional-validity premise; the original negative passes
again. Candidate33 binaries/results remain retained as failed development
observations and must not be used as final soundness evidence.

The other failure is the new positive unit's equivalent conditional-return
helper. Its separately frozen `writer-return-expressions` population rejects
the positive on candidate33 while preserving its two negatives. Candidate34
checks the two pure pointer-return alternatives with ordinary branch refinement
and output intersection. Side-effecting or nested conditional returns retain
conservative output handling. Both the RFC amendment and frozen baseline
precede that implementation. Candidate33 still fails all required positive
upstream workflows; those expectations remain unchanged.


Candidate34 passes all 26 buffer unit tests, including the failed positive-size
allocation negative. The three immutable conditional-return cases also pass.
Candidate35 preserves the tighter proved pointer position when a callee exports
both an exact position and a wider envelope for the same storage and extent.
All three frozen `writer-position-bounds` cases pass; both focused buffer units
pass. Strict lint for the touched safety and buffer files is clean.

Candidate35's immutable Release executable is
`a6a9ea9b552fc3fffde0f9b940b07008175785efce70627e1414a5f28d53c9a6`
and driver is
`2d0badffb6e50e4c3dcadb8b373a6c034273567330355104257b4c796e11dfbf`.
Its required cJSON positive workflows still fail and its double-release
negative still passes. The concrete empty-object `print_object` case now has a
complete contract, but its forwarding caller lacks the buffer entry predicate.
The outer printer also loses its input forest across writes to local storage.

### Candidate36: forwarding candidates and local storage frames (in progress)

`container-local-frames` freezes one generic traversal wrapper with a local
byte initialization and field store, plus corruption of the input root and a
child. Candidate35 rejects the positive and both negatives. Candidate36's first
implementation still rejects the positive because array-arrow stores bypassed
the ordinary holder path; that observation is retained.

`writer-forwarding` retains an initial population whose added forwarding helper
was accidentally unused. Its reviewed inventory actually calls the forwarding
helper, while preserving the positive and two byte-initialization negatives.
Candidate35 rejects the reviewed positive and rejects both negatives. Both
inventories and their observations remain available; the reviewed sources were
frozen before changing discovery. The proposed change imports only a validated
layout candidate from an incomplete generic callee; it supplies no call proof
and every caller must still discharge the buffer predicate.


Candidate36d passes all three reviewed forwarding cases after applying candidate
import consistently at discovery and entry initialization. The local-frame
positive still fails: the container forwarder previously nominated predicates
only for opaque records. Complete record parameters now validate the transported
layout and nominate the same explicit entry premise. The first trace-only debug
build failed because the temporary instrumentation used a nonexistent PlaceTable
method; it produced no executable, and the instrumentation was removed.


The local-frame unit now passes its positive plus three corruption variants,
including attaching an automatic child and then corrupting it. The remaining
array-arrow failure was an identity mismatch: the actual lvalue belongs to the
automatic array, while the generic pointer-holder path named a synthetic
indirection. Framing now uses the actual lvalue storage in this case. Temporary
trace code has been removed.

The first new forwarding unit did not reproduce the intended incomplete-generic
case and failed on a separate terminated-output projection. That observation is
retained in `forward-unit36.c` and `forward-unit36.json`. The unit now uses the
already frozen reviewed regression's reserve/emit/forward route, with its same
missing-byte counterpart; it does not change an acceptance expectation.


Candidate36i passes the full Debug suite: **1,460/1,460**, comprising 1,371 unit
and 89 integration tests (282.17 seconds). The immutable candidate36g Release
checker is `c29ba955484f00563729cf676b02f237161463b5485cc11c4c6aeb42faee7045`
and driver is `d4e4eca4d34e5d05f81e07790c249dd4f639dbe691f75f735dc23794a3cce3f6`.
Strict lint of the container transfer, container discovery and buffer files is
clean. The local-frame source population passes all three cases. Its cJSON
positives remain incomplete, while the negative retains its rejection. Both
the concrete `print_object` and `print_value` cases now complete; the outer
printer still has four unresolved obligation categories. No upstream acceptance
expectation has changed.

The new `container-call-frames` population freezes a wrapper that writes a local
header and fresh byte allocation through a complete helper, plus root and child
corruption variants. Candidate36g rejects its positive and both negatives.
Candidate37 extends entry-only framing through represented complete call effects.
The upstream runner now includes dedicated ordinary-object and cold/warm/uncached
checkpoint modes for all seven original and extended clients. These modes are
not claimed as passing until their actual observations are recorded.


Candidate37 passes all three `container-call-frames` cases. Its cJSON positives
still fail. `mixed-allocation-ledger` freezes plain byte return, lost allocation,
double release and successful/failed resize in a wrapper that also traverses a
borrowed forest. Candidate37 rejects both positives and preserves both negatives.
Candidate38 accepts the plain return while preserving both negatives; resize
remains incomplete. The return discharges only the proved live allocation base's
head footprint and exports no forest predicate.

Candidate39 adds conditional reallocation accounting: capture the old head before
call effects, track the independently fresh replacement, and add the captured
release only after a non-null result. Its first Debug build failed a shadowing
warning treated as an error; the local variable was renamed before rebuilding.
These development results do not satisfy the outstanding whole-workflow gates.

Candidate39d passes all four mixed-allocation cases, all six reallocation-failure
cases, and the new allocation-ledger Analysis and Core units. Its immutable
Release executable is `d9374707a644f21517563d6a2c727e45f15da4dd72c3d0a36f9c3be6242c5607`;
the driver is `7568d3f669d1c0f26085d058df8d49aec0d49ec5d4ce67f38f6de72c9a24f5f4`.
Both original cJSON positive workflows remain incomplete; the double-release
counterpart remains rejected. These results do not establish the final cost,
corpus, sanitizer or transport gates.

The separately frozen `scalar-write-offsets` inventories investigate scalar
facts below advancing pointers. Candidate37's summary for `*out++=7; *out=0`
incorrectly says the original first byte is zero. The initial four clients
reject their two unsafe cases but also reject both safe counterparts; those
observations alone do not demonstrate a false acceptance. A further frozen
`advanced-manifest.json` does: `advanced` writes 7, increments its pointer, and
returns the next byte, which its client initialized to 42. The client writes
past its array when that result differs from 7. Both the exact RFC0028 baseline
and candidate39d accept the selected client. Running the unchanged fixture at
`-O0` with ASan/UBSan reports the executed out-of-bounds index and stack write.
The retained `scalar-advanced-baseline`, `scalar-advanced39d`, and
`scalar-advanced-oracle` logs record this counterexample.

Candidate40b rejects that counterexample and passes the dedicated unit covering
increment, pointer assignment, unchanged aliases, and conditional aliases. The
initial candidate40 focused run passed 116/117 tests: using the promoted LHS
type accidentally discarded bit-field storage width. Restoring the declared
width fixes that regression; its focused retest passes. Broader scalar-output
and corpus checks remain outstanding. The RFC remains Accepted.

### Candidate41: bounded string lengths and reusable generic fallbacks (in progress)

The resumed Debug baseline passed 1,462/1,468 tests. Cursor traversal and
formatted-output regressions came from newly nominated scalar cases replacing
complete generic contracts. An incomplete or unavailable optional case now
falls back to the complete generic contract, preserving all its original caller
requirements and publishing no dependent case output. The eight focused
regression and Core checks pass, including the unchanged RFC0021 and RFC0024
populations. Original observations remain in `resume-dev-tests.log` and
`resume-regression-retest.log`.

Bounded `strlen` now distinguishes its first zero from an arbitrary known zero,
exports an explicit bounded-termination input where needed, and retains the
proved range and initialized prefix through a cursor update. A target-unsigned
cancellation rule preserves the modular identity without discarding invalid
operand evaluations; signed and Boolean arithmetic are excluded. The frozen
six-case `bounded-string-cursor` population passes both positive cases and all
four intended rejections (`resume-bounded3`). This population is now registered
in CTest and ordinary-object validation. These focused observations do not yet
satisfy the upstream or final validation gates.

### Candidate 41 completed Debug observation

The Debug suite passed all 1,472 tests in 290.86 seconds, including the new
bounded-string unit and source cases, the separate-object population and lit.
The upstream source observation remains a failed acceptance run. The closed
print client's selected contract is now complete with zero entry requirements;
two ordinary diagnostics in unselected generic cJSON bodies still make the
invocation unsuccessful. The parse client remains incomplete. This does not
claim either mandatory upstream workflow gate has passed.

### Candidate 42: transparent byte casts

A reduced upstream reader exposed a projection bug: explicit pointer casts
around cursor arithmetic bypassed the checked arithmetic path. RFC 0004 already
requires pointer-to-pointer casts to preserve object identity. The checked
memory path now strips those transparent casts while retaining the arithmetic
operand's original element size. The separately frozen `cast-reader-intervals`
population rejected both positives before the repair and passes all five cases
after it. Forged capacity, uninitialized contents and a scaled one-past read
remain rejected. A reduced unchanged `skip_utf8_bom` body now has a complete
conditional checked contract. No upstream source or frozen expectation changed.

### Candidate 43: stable reader index induction (in progress)

The separately frozen `reader-index-loops` population records the remaining
strict cursor-plus-index traversal gap. Both positive cases failed before the
rule; five adversarial cases were rejected. The RFC now specifies the precise
zero-based, unit-stride induction and its stability/entry restrictions before
implementation. This candidate is under development; the complete upstream,
sanitizer, corpus and performance gates remain outstanding.

Candidate 43e passes the seven frozen reader-index cases, both new focused
unit tests, the cast-reader and bounded-string source populations, the finite
8-bit induction oracle and the full lit suite (7 selected CTest entries).
Discovery recognizes a counter-plus-index comparison as candidate evidence;
actual storage and initialization still come from caller premises. Mathematical
byte displacements remain outside the typed symbolic base, preserving the
strict bound through the exclusive access endpoint. The first reader access in
the reduced cJSON numeric parser is now proved. Later copy/count and numeric
conversion obligations remain unresolved.

### Candidate 44: equal entry counter values (in progress)

The new frozen `paired-reader-counters` population records failure of its clean
copy client and rejection of three count/stride mutants before implementation.
The proposed repair seeds a relation only from two actual equal integer
constants of the same type; increment spelling merely nominates a bounded set
of cells. CFG joins and proved nonwrapping adjustments remain responsible for
maintaining that equality. The first build caught a `-Wshadow` error; the local
was renamed and the warnings-as-errors rebuild is underway.

Candidate 44f passes the four paired-reader source cases. Equal constants now
seed a bounded counter relation, and range queries follow one such equality.
The reader induction retains its non-strict condition/exit bound as well as
its strict body bound. Actual constant reader extents narrow these bounds in
specialized cases. Candidates 44b through 44e still rejected the positive;
44f resolves that case without accepting any count/stride mutant. The generic
copy helper remains incomplete for unrestricted capacity, and this observation
does not satisfy the upstream workflow gates.

The candidate 44g focused run passes both counter/reader units and all six
source, finite-oracle and lit CTest entries. Its clean Release upstream rerun
still fails both mandatory positives and rejects the double release. The
print client's selected contract is complete; its invocation still reports
the two ordinary diagnostics in unselected generic bodies. The parser's
numeric-copy obligations decreased, but recursive construction, string
traversal and numeric conversion remain incomplete. The first Release-copy
attempt preceded completion of linking and failed with missing executables;
it produced no acceptance result and was repeated only after the build ended.

### Candidate 45: character-pointer slot views

A separately frozen five-case population rejects its two positives before
the change. The target-aware character-pointer view rule follows Clang's
pointer alias implementation and requires matching representation and address
space. Candidate 45a accepts both positives and rejects read-only slots,
out-of-bounds end-pointer reads and unrelated pointer types. Actual library
provenance, writable storage and initialized input obligations remain active.
This supplies no floating-point value or range guarantee.

The full candidate 44g Debug suite passed 1,478/1,479 tests in 310.69 seconds.
Its failing existing mutable-storage test exposed a regression in candidate 42:
a casted const field address could inherit its mutable enclosing record's write
permission. Candidate 45b retains the const subobject identity and passes that
regression plus all 15 other selected numeric/slot/source/lit checks. This is a
repair to RFC 0018's existing writable-storage requirement, not a new permission.

### Candidate 46: pointer-difference reader guards (in progress)

The seven-case `pointer-reader-offsets` population was frozen before this
change. Candidate 45a rejects both positives and all five intended negatives.
Candidate 46a accepts the closed positive and preserves all five negatives;
its unrestricted generic helper remains incomplete. A reduced upstream string
parser gains bounds in its initial scan but reaches the existing dataflow
iteration limit. The limit remains unchanged and the failed observation is
retained. Candidate 46b adds an explicit sufficient entry extent bound for
representable byte-pointer differences; this is still under validation.

Candidate 46c passes all seven frozen pointer-offset cases. Candidates 46d–f
exposed and repaired two separate issues: constant relation bounds previously
grew until the existing visit limit, and the initial extent premise was too
broad for the unchanged RFC 0021 generic subtraction rejection. Constant bounds
now use the existing zero-threshold widening only at checked loop joins. The
explicit extent premise is restricted to an established byte-buffer predicate.
The five focused checks, including that existing rejection, the widening unit,
the pointer-reader unit/source population and lit, pass. Trace instrumentation
was removed. The first format attempt lacked clang-format on PATH; the rerun
used the LLVM installation explicitly and succeeded.

Candidate 46g's clean Release upstream observation still rejects both mandatory
positive invocations and the intended double-release negative. The selected
print client remains complete with zero requirements, but its invocation has
the same two ordinary diagnostics. Reader discovery from the upstream TU lets
the concrete string-parser case prove the initial pointer scan. Its second
loop, the UTF conversion helper, numeric conversion and recursive construction
remain incomplete. The full Debug run is recorded separately and was started
against the fixed candidate 46g binaries before subsequent checker edits.

### Candidate 47: explicit initialized byte spans (in progress)

The nine-case `initialized-spans` population was frozen before implementation.
Candidate 46g rejects all three positives and all six unsafe counterparts.
The RFC now specifies a strict input-only requirement for a live, initialized
same-array interval with representable byte distance. Entry nomination is
bounded and excludes changed/address-taken endpoints and ambiguous pairs.
Implementation and validation are in progress; no mandatory gate is claimed.

The full candidate 46g Debug suite passed all 1,484 tests in 398.26 seconds.
Candidate 47a passed eight of nine initialized-span cases; its nonzero-start
positive exposed overflow in the checker's sufficient upper-bound construction.
Candidate 48a repairs that comparison and passes all nine. The separately
frozen four-case `span-outputs` population already passed before the separation
refinement and remains a regression check. Its new unit additionally rejects
an unnecessary mutual-separation premise between read-only endpoints.

### Candidate 48: scalar interval envelopes

The five-case `reverse-byte-writes` population was frozen before this change;
candidate 47a rejects both positives and all three intended negatives. The RFC
specifies projection of actual narrowed scalar facts into sufficient interval
requirements. Candidate 48a accepts the generic reverse writer, but a concrete
specialization loses its bound when distinct exact counter constants join.
Candidate 48b seeds the actual constant assignment's lower and upper bounds in
the existing relation tracker. All ten focused Core/Analysis/source/lit checks
now pass, including unchanged unbounded-subtraction rejection. The reduced
unchanged UTF conversion helper now has a complete conditional contract.
The string parser still lacks second-loop and call-output evidence; the full
upstream parse, sanitizer, corpus and performance gates remain outstanding.

Candidate 48b's Release upstream observation confirms that the generic UTF
conversion helper and both observed specialized cases are complete. Both
mandatory positive invocations still fail, and the intended double release is
rejected. No frozen upstream input or expectation changed.

### Candidate 49: bounded counts from byte-span helpers (in progress)

The separately frozen `span-counts` population rejects all three positives and
both intended negatives on candidate 48b. The RFC now specifies an output-only
nonnegative count bound, tied to a matching explicit entry span and verified
at every returning path. The first build failed because a private field used a
type alias before its declaration. The field was moved after the alias, and
capture preserves the types of saved byte coordinates. Candidate 49b is still
building; no acceptance result is claimed.

The full candidate 48b Debug run passed all 1,492 tests in 816.48 seconds.
This was a regression run concurrent with development builds and an upstream
observation, not an isolated cost measurement. Candidate 49b's build rejected
passing a const state to expression materialization; output verification now
uses a local state copy only for functions with nominated spans. Candidate 49c
builds and correctly emits the count guarantee for the helper, but still
rejects all three positive callers because the numeric guarantee had not been
connected to the interval relation solver. Both negatives remain rejected.
Candidate 49d projects a representable captured distance through the existing
nonwrapping linear-expression machinery; validation is pending.


Candidate 49d passes the five frozen span-count cases and both focused unit
checks. Candidate 50's separately frozen `span-count-joins` population fails
all three positives and rejects both unsafe counterparts on 49d. It tests
counts assigned on different branches, including assignment before the guard;
implementation is pending and mandatory upstream gates remain incomplete.

Candidate 50a still rejects the joined positives: the nomination function had
been extended, but its callers only visited loop operations. Candidate 50b
also visits return statements in the existing bounded syntax scan.

Candidate 50b passes all five joined-count cases. The positive helper now
exports the count guarantee across the assignment/guard branch join; the false
count and later mutation remain rejected. Unit, lit and upstream observations
follow independently.

Candidate 50b passes five focused count checks. Its unchanged upstream run
still rejects both mandatory positives and the intended double-release
counterpart. The UTF helper now additionally exports `count-within-span`.
A reduced numeric client proves copying and termination but loses ownership
when `strtod` writes its end pointer into a local slot. The newly frozen
six-case `local-callee-copies` population rejects all three positives on 50b
and rejects its three unsafe counterparts. Candidate 51 narrows the existing
copy-store escape rule only for a complete callee and a confined automatic slot.

Candidate 51a's first build failed the existing shadow-warning check; the
local syntax-worklist name was corrected. Candidate 51b also keeps the escape
rule for additional store destinations, heap outputs or returned slot addresses.

Candidate 51b accepts the signed/unsigned character `strtod` clients but still
rejects the inferred copy helper, whose equivalent final heap-root description
hit the conservative exclusion. Candidate 51c permits that same root-only
output description while still declining nested or additional destinations.
The reduced numeric-parser case now retains ownership; its remaining failures
are target ptrdiff representability and the floating conversion.

Candidate 51c passes all six local-copy cases, including the inferred helper
and actual leak rejection. The next Debug build includes dedicated units and
lit coverage; no mandatory whole-program gate is claimed from these results.

The eight-case `reverse-initialization` population was frozen before candidate
52. Candidate 51c accepts its generic helper conditionally but rejects both
closed positives; all five unsafe counterparts reject. The generic helper's
pre-existing acceptance therefore does not demonstrate its must-write output.
Candidate 52 adds a separately validated normal-exit rule for the visited suffix.

Candidate 52a publishes the reverse helper's initialized suffix, but both
closed positives still reject. One needs direct automatic-array bases added
to the stable-base eligibility; the other exposes a separately retained
limitation when an interval endpoint is supplied as a conditional scalar call
argument. All five unsafe counterparts remain rejected.

Candidate 52b still declined automatic arrays because their ordinary decay
marks their address taken. Candidate 52c keeps that restriction on mutable
pointer cells only; an automatic array's identity cannot be reassigned by the
eligible body. The full candidate 51c suite remains isolated from these edits.

The five-case `conditional-count-arguments` inventory was frozen in the build
validation directory while the stable full suite was running. Candidate 51c
rejects all three positives and both unsafe counterparts. It will be moved
unchanged, including its hashes, into the test inventory after that suite.
An early candidate 52c snapshot was copied before linking finished; its result
is retained but is not attributed to the completed 52c build. A fresh snapshot
and observation will follow verified build completion.

The completed candidate 52c snapshot accepts the direct local reverse-write
positive; its remaining closed rejection is the conditional-argument case.
Candidate 53a passes all five conditional-argument cases and all eight reverse
initialization cases. Candidate 53b restricts additional legacy endpoint capture
to actual conditional arguments; ordinary represented arguments retain their
existing path. Recapture explicitly retires an older saved expression as well
as its scalar range. The full candidate 51c suite is still running separately.

Candidate 51c's complete Debug regression run passes 1,500/1,500 tests in
609.92 seconds (`resume-candidate51c-full.log`). It overlapped Release builds,
so this elapsed time is not performance acceptance evidence. Candidate 53b
passes all eight reverse-initialization and five conditional-argument cases.
The latter inventory has now been copied unchanged into the evaluation tree.

The separate five-case `initialized-advance` inventory was frozen before
candidate 54. Candidate 53a rejects both closed positives and correctly rejects
all three unsafe counterparts. Candidate 54 introduces an output-only relation
between initialized bytes and the actual advanced pointer; it must never use
an upper position envelope as a must-initialized endpoint. The frozen inventory
has been copied unchanged into the test tree with its original hashes.

Candidate 54a failed to compile because it used a private requirement-set
accessor; candidate 54b uses the public iterator interface. Candidate 54b builds,
but still rejects both initialized-advance positives. Its internal final cursor
has the actual range `{1,4}`, yet the upper-envelope exporter only consults
ranges on declared C cells, so no position output is published. The next fix
extends the already specified range rule to typed internal cursor coordinates.
All three unsafe counterparts still reject. Candidate 53b's unchanged upstream
run continues to fail parse/delete; print/delete has a complete selected client
but the invocation still fails ordinary upstream diagnostics.

Candidate 54c failed compilation because the represented integer type is a
field, not an accessor. Candidate 54d exports the position and initialized
advance, and accepts the interior-pointer positive. The base-pointer positive
still hit a stale ordinary spatial offset. Candidate 54e retires that offset
when the verified final cursor is symbolic; all five initialized-advance cases
pass. Its core schema unit passes. The reverse-write unit was corrected to
check a closed caller: a generic reader can legitimately require initialized
input, so generic completeness alone was the wrong negative assertion. The
frozen source inventories and their expectations were not changed.

The five-case `advance-outcomes` inventory is frozen before candidate 54f.
Candidate 54e rejects both positives and all three unsafe counterparts. A
common initialized-advance fact was lost when its matching position envelopes
differed between early and successful returns. Candidate 54f retains the
independently proved enclosing constant position envelope across all outcomes.

Candidate 54f passes all five early-outcome cases, and the unchanged cJSON
UTF conversion helper now exports its actual initialized advance across success
and zero returns. The focused Debug units and source cases pass after supplying
the runner's JSON inventory alongside the original SHA256SUMS (same hashes).
The whole upstream workflow remains incomplete; no acceptance is claimed.

A counter-reset probe isolates the numeric parser's ptrdiff loss: removing only
the later index reuse, or retaining an explicit count bound, removes that loss.
Before candidate 55, the exploratory `counter-reset-ranges` inventory rejects
both positives. Its negative matcher accidentally omitted the actual `leaked`
and `after it was freed` wording. Those failures and original manifest remain
retained. The separate `counter-reset-ranges-reviewed` inventory corrects the
matchers before implementation, preserves the original manifest, and changes
neither source bytes nor acceptance expectations. Its baseline rejects both
positives and matches all three unsafe counterparts.

Candidate 55a preserves a proved range on an unchanged equal counter before
an exact index reset. All five reviewed cases pass. The reduced unchanged
numeric-parser client now has only the unsupported floating conversion;
its pointer subtraction is proved representable. A changed-count unit and
regular source/object registration accompany this fix.

Candidate 55a's focused Debug counter-reset tests pass, including the
changed-count rejection. Formatting passes with the configured clang-format;
the Core include boundary and all five newly registered frozen inventories
verify. The complete 1,511-test Debug suite is running against stable 55a
binaries. Subsequent source changes use separate Release builds only.

The five-case `guarded-advance` inventory was frozen in the build validation
directory before candidate 56 while the stable suite runs. Candidate 55a
rejects both positives and all unsafe counterparts. Candidate 56a retains the
conditional initialized interval but still lacks a positive lower cursor bound.
Candidate 56b exports the actual narrowed lower bound and passes all five cases.
A separate five-case `guarded-cursor-bounds` inventory covers zero advance on
failure and positive advance on success. Candidate 56a rejects its two positives
and all unsafe counterparts. Candidate 56c carries constant outcome-specific
bounds on the actual installed cursor through existing pending integer facts;
result and coordinate mutation retain their normal invalidation.

Candidate 55a's full Debug suite passes 1,511/1,511 tests in 530.23 seconds
(`resume-candidate55a-full.log`). Release development overlapped this run, so
it remains regression evidence, not an isolated performance measurement.
Candidate 56c passes all five guarded-cursor cases and the focused Debug unit
and both guarded source populations. Both inventories were copied unchanged
into the evaluation tree after the stable full suite completed.

The six-case `span-advances` population is frozen before candidate 57 in the
build validation directory. Candidate 56c accepts the generic count helper
and the single-call client, but rejects repeated traversal. All three unsafe
counterparts reject. Candidate 57 extends the existing bounded-sum implication
to constant endpoints and actual equal captured coordinates, and retains only
independently established bounds when replacing a cursor's old value.

Candidate 57a passes all six span-advance cases and both focused Debug tests.
The frozen population is registered for regular source and object testing.
The unchanged cJSON workflows remain incomplete.

The five-case `paired-cursor-loops` population was frozen before candidate 58.
Candidate 57a rejects both positives and all three unsafe counterparts.
Candidate 58a preserves unit-step relations using an established transitive
constant upper bound and passes all five cases. The reduced unchanged string
parser remains incomplete because its escape scan and allocation-size relation
are not yet established; passing the cursor regression is no parser claim.

Candidate 58a's focused Debug cursor tests pass. Its full suite is running
against stable binaries. The seven-case `numeric-text` inventory was frozen
before candidate 59: candidate 58a rejects both positives and all five unsafe
counterparts. Candidate 59a failed compilation due to a missing internal helper
header; candidate 59b adds that include before further validation.

Candidate 59b builds. Review before the next candidate identified two content
invalidation requirements: unknown writes must also remove pending numeric
content, and an unrepresented existential string endpoint cannot count as an
empty interval. Candidate 59c applies both and preserves content through only
recognized byte-copy primitives. Zero-byte facts keep their existing canonical
representation. The separate seven-case `numeric-scans` inventory is frozen
before scan inference; its candidate 58a baseline rejects both positives and
all five unsafe counterparts.

Candidates 59b and 59c pass all seven frozen numeric-text cases. A separate
copy probe remains incomplete: byte-copy primitives return from the runtime
path before general postcondition capture. Candidate 60a adds only the bounded
switch-scan prefix and alphabet-preserving stores; candidate 60b adds explicit
snapshot-based content transfer for modeled memcpy/memmove and accounts for
the source interval's actual storage offset. General helper output contracts
continue to discard content guarantees.

The complete candidate 58a Debug run reports 1,517/1,518 passing tests in
849.54 seconds, overlapping Release builds. The existing terminated scan after
compaction test regressed; its original assertion remains unchanged and the
failure is under investigation. Candidate 60b still rejects both numeric-scan
positives: the platform's fortified builtin spelling was not normalized during
content capture. Candidate 60c uses the existing builtin-name normalization.

Candidate 60c proves the numeric-scan floating conversion, but both positives
retain a pre-existing release failure after strtod with a null end pointer.
Candidate 61b guards that modeled store and applies RFC 0009's refuted-store
rule to source escape. The reduced unchanged numeric parser still lacks its
content proof. Exploratory declaration/constant variants are separate files;
one allocation-declaration variant had a source-generation typo and is invalid
evidence. Candidate 61a's stronger symbolic cursor bound does not fix the
compaction regression; candidate 61c restricts that retained terminator bound
to independently proved constant endpoints. Both numeric populations retain
their original inventories and are now registered with source, object and lit
checks, plus Core and Analysis unit coverage.

Candidate 61b passes all seven numeric-scan cases. The unchanged reduced
numeric parser loses its scanned numeric prefix on a later private local
pointer assignment, before memcpy. A separate three-case `numeric-scan-locals`
inventory freezes two equivalent local-write positives and an actual input
alias mutation before candidate 62. Candidate 61b rejects both positives and
the intended unsafe counterpart. Candidate 62 preserves existing byte facts
across writes confined to unexposed automatic scalar cells.

Candidate 62a passes all three private-scalar cases, all seven numeric-scan
cases, all seven numeric-text cases, all six span-advance cases and all five
paired-cursor cases. The original compacted-string traversal probe also passes.
The unchanged numeric-parser probe still rejects its floating conversion, so
these results do not establish the mandatory upstream workflow. The private
scalar inventory is copied unchanged into the regular source/object tests.

Candidate 62a's six focused Debug tests pass, including the original compacted
string regression. Its full 1,526-test suite is running against stable binaries.
The separate five-case numeric-short-circuit inventory was frozen before
candidate 63: both equivalent positives reject at 62a while all three unsafe
cases retain their intended rejection. Candidate 63a maps bounded condition
subexpressions to the loop and preserves the non-strict visited-prefix fact
before every short-circuit test. All five cases pass, and the reduced client
using unchanged cJSON parse_number now passes. This is a helper-client result,
not completion of the complete parser lifecycle. Formatting and the Core
include boundary pass at 63a.

The complete candidate 62a Debug run passes 1,525/1,526 in 634.53 seconds,
overlapping Release development. The one failure is Builtins.Entries, whose
exact strtol store expectation still omits the newly required non-null output
guard. The compaction regression and every new source/object population pass.
Candidate 63a's complete upstream run still fails both positives; print-delete
has a complete zero-requirement selected entry but retains the two ordinary
generic-body diagnostics. Parse-delete retains 174 unresolved entry obligations.

Candidate 64a retains a proved updated physical cursor bound across branch
joins. It passes the inline two-pass cursor case and the compacted-string
probe, but its equivalent raw-pointer/count helper remains incomplete. The
original cursor-envelope-joins inventory is retained; before implementation,
review found its forged-capacity literal stopped at an early quote. The
separate reviewed inventory removes that closing quote. ASan confirms that
reviewed counterpart's actual out-of-bounds read. The over-step counterpart
forms an out-of-object pointer but does not dereference it; ASan/UBSan does not
diagnose that pointer-formation UB. Both inventories retain every observation.
The actual reduced string parser still fails, although its isolated two-pass
input traversal now passes. The pointer-count-readers inventory is frozen
before candidate 65, with both positives rejected and all three unsafe cases
rejected by candidate 64a.

Candidate 65a adds explicit pointer/count interval premises but both helper
positives still reject. Temporary diagnostic builds (65debug and 65debug2)
showed a deeper identity error: an adjusted pointer inherited the original
pointee's exact value. The four-case shifted-pointee-facts population freezes
three actual out-of-bounds writes that candidate 65a falsely accepts, plus
an unchanged-pointer positive. Candidate 66 removes cross-cell subtree facts
for nonzero or unknown element displacements. The temporary debug logging is
removed before that candidate; the original reports remain available.

Candidate 66a passes all four shifted-pointee cases, all five pointer/count
cases and all five reviewed cursor-join cases. ASan and UBSan independently
confirm all three shifted-pointee unsafe executions; the unchanged-pointer
control runs clean. The numeric-parser client remains complete. These three
frozen populations are registered for regular source/object validation and
the relevant lit and unit tests. The original cursor inventory and failed
candidates remain retained in the build validation directory.

Candidate 66a's full Debug suite passes 1,532/1,532 in 561.67 seconds,
overlapping Release development; this is not an isolated performance run.
Its seven focused Debug checks also pass. The complete upstream population
still has 174 unresolved selected parser obligations, while the complete empty
print entry is blocked by two ordinary generic-body diagnostics.

The frozen buffer-lower-extents population reproduces a false physical bound:
a four-byte backing array advertised as capacity two is diagnosed as an actual
two-byte allocation. Candidate 67a keeps this capacity in the positive checked
memory domain without inventing an exact ordinary spatial extent. It removes
the cJSON ensure memcpy bounds diagnostic, leaving the ordinary print null
warning. Candidate 67a still cannot project the additional entry interval.

The six-case buffer-entry-intervals population was frozen before candidate 67b.
67b projects additional bytes only from unchanged entry backing, but also
projects an unnecessary non-null premise that regresses six existing RFC 0026
positives. Candidate 67c restricts this additional projection to extent and
initialization. Both new positives complete; every intended unsafe case rejects.
Two original reason regexes omitted the word `extent` although the bounds
precondition is reported. Their original manifests remain unchanged in the
build validation directory. The separately reviewed inventory adds that word,
keeps all source bytes and outcomes unchanged, and passes all six cases. It is
registered for source/object and lit checks, with a focused unit test.

Candidate 67c preserves all 28 RFC 0026 source cases and the unchanged fixed
evaluation: 44/44 bugs, 32/32 clean programs, zero parse/tool errors or timeouts.
The numeric-parser client remains complete at 67b. Three temporary tracing
builds fail compilation due to trace-only API mismatches; they supply no proof
or performance evidence. The production candidate67c snapshot is unchanged.

The first focused 67c Debug run passes both source populations but the new
unit's additional assertion about the unrestricted generic helper fails. Its
actual positive/negative caller assertions pass; the generic assertion was
unrelated to the physical-capacity regression and is removed. A subsequent
run passes the caller unit. An initial Werror build rejected the new unit's
signed initializer-list literals; they were corrected to unsigned literals.
The separate ordinary recursive-null-output unit already passes at 67c, so it
does not reproduce the remaining cJSON null warning. Further diagnosis is
required before changing recursive result inference.


### Candidate 68: settled ordinary value outcomes (in progress)

A temporary trace of cJSON's recursive print group shows a positive non-null
buffer outcome in the freshly checked body, lost when intersected with an early
SCC approximation. Candidate 68a preserves the final rechecked null outcome
maps after actual convergence. Its Release build succeeds, but the unchanged
upstream source population still fails parse/delete and print/delete. Parse's
selected main has 168 unresolved obligations and no entry requirements. Print's
selected main is complete with zero entry requirements, while an ordinary
null diagnostic at cJSON.c:1280 still fails the invocation. The double-release
counterpart continues to reject.

Further traces show that the non-null fact now reaches the actual
`buffer[0].buffer` cell, but an earlier numeric approximation still permits a
negative success flag. The final body independently establishes only 0 and 1.
Candidate 68b extends the same existing final pass to retain the numeric return
root's actual alternatives, preserving all guards, unknown alternatives and
existing widening of numeric writes to caller memory. No additional SCC round,
checked memory output, completed call or recursive hypothesis is introduced.
Temporary trace sources were removed before the production build. The traces
and failed upstream observation remain in `build/rfc29-validation`.

Separate frozen caller and three-member value-outcome probes include real
negative failure results, false success, and a pointer overwritten after the
call. The first simpler caller population already passes at candidate67c and
is supplementary coverage, not a reproducer. All production candidate67c and
68a binaries remain immutable.

Candidate 68b passes the fixed 44-bug/32-clean evaluation, all 28 RFC 0026
source cases, and 81 focused Debug unit tests. Both source upstream positives
remain rejected; the double-release case still rejects. The upstream source
population takes 177.42 seconds while a Debug build and other checks run, so
this is not isolated performance evidence. Both smaller ordinary recursive
value-outcome cases already pass at candidate67c and remain supplementary.

The remaining ordinary print warning exposes an independent cross-domain bug:
the tested numeric value's range is checked for a wholly impossible branch,
but its surviving sign classes are not used to narrow pending call outcomes.
Consequently a real 0-or-1 result still retains an obsolete negative outcome
when selecting nonzero. A separate-source unit exercises a conservatively widened outcome set and
independently inferred numeric returns, with actual negative-failure and saved
result counterparts. It already passes on candidate68b and does not reproduce
the cJSON failure; the unchanged upstream case remains the reproducer.

Candidate 68c intersects pending call outcome selections with the current
trusted numeric value's sign classes after the existing conversion checks.
The standalone ordinary cJSON print null diagnostic disappears. Candidate68d
also avoids creating null outcome maps when the widened summary has no
represented outcome partition. Release and Debug builds pass. The unchanged
upstream source population now accepts print/delete with a complete selected
main, zero entry requirements and no ordinary error; parse/delete remains
incomplete and the double-release counterpart rejects. The 107 focused Debug
integer, recursion and buffer units pass. Full transport and remaining gates
are still required.

The frozen `in-place-extension` population precedes any in-place construction
change: runtime-depth extension of an existing owned head fails at candidate68b,
while lost-child, duplicate-child, nondecreasing recursion and failed-cleanup
counterparts reject. These are development observations, not a completed gate.

Candidate68d also passes the fixed 44-bug/32-clean population. Its four frozen
extended upstream cases remain incomplete: nested printing, malformed-input
cleanup, forced parse allocation failure, and forced print allocation failure.
The forced parse client has two unresolved obligations, including a missing
numeric global during contract transport. These failures remain in scope.

### Candidate 69: extension of an existing owned head (in progress)

The RFC now specifies an unconditional `container-extended` output relating
an unchanged singleton input head to fresh descendants. Candidate69a adds strict
encoding and input-premise validation, call ledger transfer, and a private
recursive candidate with an unsigned decreasing count. Its first Release and
Debug builds fail because the contract-kind table size was not increased; both
builds pass after correcting the table size. Five focused Core contract tests
pass. In the frozen in-place population, the safe recursive case remains
incomplete and all four negative cases reject. This is not acceptance evidence
for recursive extension.

The private group trace shows that its recursive call establishes the new
footprint, but loses separation from the original head when reinstalling the
call output. Candidate69b carries that pre-call separation only while the
neighboring complete forest fact survives unchanged, and retains the unchanged
head's live storage identity. Validation is pending.

Candidate69b passes all five frozen source cases and all five ordinary-object
checked links. The safe constructor and closed client are complete; the four
counterparts retain their intended ownership/progress failures. The registered
unit/source checks pass, as do 113 focused Debug regression units and the new
lit test. A separate ASan/UBSan oracle enumerates depths zero through nine and
each allocation-failure position: the positive releases every acquisition;
lost-child, duplicate-child and failed-cleanup variants fail the allocation
ledger or sanitizer. Nondecreasing recursion is a proof-progress rejection,
not a claim corroborated by that finite memory oracle. The first object harness
attempt omitted its binary-identity argument, and the first lit invocation used
a nonexistent executable path; corrected invocations pass. These do not replace
the required full sanitizer suite or transport/checkpoint gates.

### Candidate 70: anonymous typedef storage metadata (in progress)

A temporary tracing binary identifies `global_error` as the missing global in
the forced parse-failure client. Its anonymous typedef record has a canonical
view containing the typedef identity, which the imported adapter cannot
reproduce. A frozen two-source `anonymous-private-state` reproducer rejects at
candidate69b. Candidate70a retains that anonymous typedef identity in strict
`it2` interface metadata, creates an internal typedef for the adapter, and
retains full target-layout/view validation. Forged or missing identities still
fail materialization. Temporary trace code is removed from production sources.

A separately frozen global-cell/forest-frame probe already passes all four
cases at candidate69b. It is supplementary evidence, not a reproducer of the
remaining forced-print failure; no frame-rule change is justified by it.

Candidate70a passes the frozen anonymous-typedef source reproducer, all 24
focused interface/extension tests, all 553 Core tests, all 86 Frontend tests,
and its new lit test. Both new populations pass all six ordinary-object checked
links. Their expanded uncached/cold/warm and expanded compact reports match;
warm and compact runs reuse every source unit with zero function analyses.
The extended upstream rerun and full Debug suite are running. A temporary
forest-transfer trace build fails Werror on a shadowed member name; the local
trace label is corrected. No failed tracing build is validation evidence.


### Resumed candidate 71: validation and retained head values

Candidate70a's complete Debug suite finishes at 1,536/1,541 in 1,150.46
seconds while development commands overlap. Two reader unit assertions and
one lit message predate candidate67's additional physical-interval requirements:
the generic helpers now export the required extra interval, and their actual
undersized clients still reject. The units now check both that premise and
the concrete rejection. The recursive reuse assertion also predates candidate68's
mandatory final body recheck and now requires that recheck. These three units,
the complete 176-case lit suite and the two recent object populations pass in
`candidate71-focused.log`.

The remaining Debug failure is the aggregate RFC0029 object test's 600-second
deadline. It combines 66 independently frozen populations. CTest now schedules
each population separately under the same deadline, retaining every case,
selection and expected outcome. The runner retains its all-populations mode.
The anonymous-private-state split command passes at the immutable candidate70a.

The forced parse-failure client now rejects only the actual escaped automatic
input stored in cJSON's global error state. The static-storage development
counterpart is complete with zero requirements (`failed-parse-static70a.json`).
The separately frozen `upstream-lifetime-audit` population records both cases;
the original upstream fixture and accepted expectation remain unchanged.
Its committed oracle independently reproduces stack-use-after-return (or scope)
when the error pointer is read after the helper returns, and accepts the static
counterpart. Both sanitizer observations pass in `lifetime-audit71-oracle`.
No invalid read is attributed to the original client, which does not use the
retained pointer after its input lifetime ends.

Temporary print-failure traces locate a tag value known only through the live
container's head predicate. Call-context capture uses it, but the returned
null guard cannot query it. `failed-print71guards.log` retains this reproducer.
An initial trace probe was launched before its new executable finished linking,
then deliberately terminated; it is not candidate validation. Temporary trace
code is removed from production sources.


Candidate71a's shared container-value query proves the unchanged forced-print
failure client with a complete selected main, zero requirements and no ordinary
error (`failed-print71a.json`). The seven reviewed reduced cases pass through
source and ordinary objects. The initial six-case inventory mistakenly expected
a replacement under the deliberately failing allocator to reach its tag write;
the baseline correctly accepts that early-return path. Both original files and
failed observation remain frozen. A separate reviewed manifest retains that
safe case and adds an actual live replacement before the invalid access.

The focused head-value unit passes all seven variants. Its first lit run used
an ordinary exact-extent diagnostic for the alias-mutated case; the actual
checked rejection reports the bounds obligation after retiring its predicate.
The expectation now pins that explanation; no frozen fixture or outcome changed.
The first candidate71 build failed Werror because a new local name shadowed a
range-query local, and was corrected. An attempted sanitizer command targeted
the obsolete dev-asan configuration, whose make path is unavailable; the active
rfc28-asan build is used instead. Full Debug/sanitizer, strict lint, and final
corpus/cost gates are still pending.

### Candidate 72: independent byte cursor coordinates (in progress)

Candidate 71a's fixed evaluation passes all 76 cases (44 bugs and 32 clean
programs). The guard lit test passes after its diagnostic expectation correction.
The concurrently run full Debug suite observed two RFC0026 transport subprocess
timeouts; those need an isolated rerun without competing builds. The original
upstream source population now accepts print/delete with no entry requirements
and rejects the double release. Parse/delete remains incomplete.

The frozen `independent-cursors` population reduces one decoder failure to a
copy loop whose input and output are distinct arrays. Candidate 71a rejects all
four intended positives. Candidate 72b retains inequalities between their
numeric byte coordinates only when both incoming edges establish them. It
accepts the closed and separate-source clients, including reordered declarations,
and rejects the four bad variants. The generic helper still loses a relation.
Candidate 72c additionally seeds equal exact coordinates at assignments but does
not yet resolve that generic failure. Both observations are retained; the
population is not yet counted as complete.

The first `candidate72a-bin` snapshot was accidentally made before the linker
finished and has exactly candidate71a's identity. Its evaluation is a repeated
baseline observation, not evidence for the new code. A duplicate upstream
runner was also stopped after inspecting the process tree; its aborted
observation supplies no cost evidence. Production snapshots 72b and 72c were
made only after their builds completed.

A temporary cursor trace showed the generic copy losing its relation at `q++`:
the previous increment retained the numeric equality, but the nonwrapping query
looked only at explicitly stored integer ranges. Widening had removed a
redundant full-range fact. Candidate72d also queries the validated C type and
current cursor type of represented relation endpoints. A type maximum is used
only to prove arithmetic does not wrap, never as an inferred allocation extent.
Production trace code was removed before this build.

The separately frozen `fixed-span-steps` population exposes a second gap:
`consume(first,last)` is complete and proves its zero-or-two result fits the
input span, but even, odd and runtime-length closed callers lose the bound when
the nonzero result folds to two. Candidate72c rejects those three positives and
preserves all four bad counterparts. Candidate72d extends the existing
nonwrapping difference query to exact constant endpoints; validation is pending.

Candidate72d completes all eight independent-cursor source cases. Its constant
span endpoints restore even and odd fixed-size traversal; candidate72e also
removes only proved value-preserving endpoint conversions. A runtime client
whose length comes from `(unsigned)argc` still failed because capture expanded
the stored length back into that earlier conversion. Candidate72f snapshots
the actual typed endpoint cell instead. The runtime source client and both new
unit tests, including the converted-length case, pass under ASan/UBSan. Full
source/object populations and final code gates still need completion.

The full candidate71a Debug run completed 1,609 tests in 1,956.17 seconds under
concurrent build load. All 1,418 unit tests pass. Eight integration tests fail:
the already-corrected guard lit expectation, two 60-second RFC0026 transport
subprocess timeouts, and six newly separated object-harness tests. Five object
tests previously skipped every case because the harness accepted only the exact
selection `[main]`; one rejected a supported singular `source`/`function`
manifest. The harness now keeps helper-plus-main selections, normalizes the
existing manifest spellings, and supplies an unused ordinary main for the
named closed zero-counter functions without changing their selected functions.
All six corrected object populations pass with immutable candidate72e binaries.
The failed full observation is preserved; it is not a successful full-suite gate.

Candidate72h extends cursor join completion to finite constant displacements
proved independently by both incoming difference-constraint systems. This
preserves `output <= input + 1` between a decoding helper and its input advance,
without equating the two storage objects. The frozen `helper-cursor-pairs`
population then passes seven of eight cases: the helper, once-only client,
helper-only loop, and all four intended rejections pass. The mixed direct/helper
loop still loses its input upper bound. Evidence is
`helper-cursor-pairs72h.json` and its log; the failed mixed case is retained.
The source run used the completed Debug72h build, before temporary diagnostic
instrumentation. It is development evidence, not an isolated cost observation.
All eight frozen `fixed-span-steps` cases pass production ASan72f in
`fixed-span-steps72f-asan.json`. All six formerly skipped/malformed object
populations pass after the harness correction, in their `objects-*72e` results.

Candidate72k completes all eight frozen helper-cursor cases. Before recording a
nonzero byte's strict pre-terminator bound, it refutes an edge at the actual
current initialized zero. This prevents an impossible negative coordinate from
polluting a loop merge. The generic helper and mixed direct/helper clients pass;
actual writes and escaped or insufficient intervals remain rejected.

### Candidate 73: bounded byte contents (in progress)

The separate `byte-cursor-content` population was frozen before this extension.
Completed Debug72k rejects its three intended positives and rejects all five
bad counterparts. Exact initialized byte records, strict context transport,
write slicing/invalidation, and byte comparisons were then added under the
amended RFC. Candidate73c passes all 555 Core tests but still fails those three
positives and regresses the mixed helper-cursor unit. Candidate73e preserves
only independently proved canonical cursor endpoints through widening and avoids
adding temporary bounds when a byte comparison excludes no positions. It
accepts the local scan and restores the three focused cursor unit tests; its
two separate-source positives still fail. An initial focused test command used
the wrong suite name and ran zero tests; the corrected command ran all three.

Candidate73f rechecks complete conditional contracts when actual byte contents
are captured, because a generic sufficient requirement can cover an impossible
branch for that input. All eight frozen byte-content source cases pass, including
changed bytes, unknown writes, partial initialization, and an offset pointer.
Two Core tests pass canonical binary encoding, strict decoding and remapping,
shifted alias consistency, and the existing context budget. Temporary context
tracing has been removed. Parser, object/checkpoint, full-suite, and final cost
validation remain outstanding; these focused observations are not final gates.

Candidate73f also passes the byte-content object population and exact-message
lit test, plus all 37 BufferAnalysis and new byte-context unit cases. Candidate73g
passes its cold/warm/uncached and expanded/compact checkpoint equivalence gates,
including zero unchanged warm function analyses, changed helper and changed byte
invalidation, corrupt checkpoint recomputation, and rejection of older sidecars.
The accompanying compiler binary was still 73f; both identities are retained by
the runner. This is focused transport evidence, not final compiler validation.

The broader Debug73g Analysis run passes 777 of 779 tests in 125.718 seconds.
Two existing sequential-cursor positives fail. Candidate73i avoids nominating
incidental negative initial distances at cursor joins; both regressions and the
mixed helper loop pass. Candidate73k additionally retains only independently
proved canonical strict/non-strict cursor orders at widening joins. Its five
focused tests pass, including the new postfix allocation permission regression.
The extracted cJSON string routine remains incomplete.

The separately frozen `byte-cursor-frames` population records further gaps.
Its original positive cases use a local pointer-to-pointer alias for increment;
73f and 73g reject them. An aliased-input negative is rejected for the intended
separation obligation, but the original reason expression omitted the word
`separated`; that manifest and failed observation remain unchanged. A direct
increment reduction also exposed lost write permission for a saved postfix
result into malloc storage. Candidate73j accepts that reduction; its focused
unit still rejects released storage and a substituted string literal. General
indirect pointer-cell updates remain under investigation.

Candidate73l's byte query intersects ordinary scalar bounds with the already
proved bounded difference constraints. Previously, a valid read through
`cursor < end` could still have a loose scalar maximum one past its array, so
the content query declined and introduced impossible escape paths. The original
extracted cJSON string routine now accepts its `"abc"` client with no selected
entry requirements. All 41 focused tests pass, including an escaped-byte
counterpart. The production binaries are retained in `candidate73l-bin`.

The immutable-static population was frozen before enabling const static local
array contents. Debug73l fails its three positives and preserves all three
required rejections. The new `upstream-static-inputs` population preserves the
original nested and malformed JSON texts while avoiding the separately audited
automatic-storage lifetime defect. Baselines and subsequent observations remain
separate from the original upstream manifests.

Portable versions advance to summary 25, sidecar 26 and checked encoding 11 so
interim development caches cannot supply older byte-context semantics. Release,
Debug and sanitizer rebuilds are in progress; no final suite or cost claim is
made by those development builds.

The first Release rebuild after the artifact-version increment failed while
compiling the new local pointer-cell resolution: the code used a nonexistent
`Affine::isZero` helper. It now compares with the zero affine value. The failed
build log is retained, and no snapshot or validation result uses that unfinished
compiler pair. A fresh build of both tools is underway.

## Candidate 74: static input, pointer cells and byte forwarding

The six frozen `byte-cursor-static` cases pass candidate74a. Both unchanged
upstream inputs in the separate static-input population still fail at 74a.
Their safe storage duration removes the audited escaped-error-pointer defect,
but does not by itself establish parser construction. Original upstream
populations and source identities remain unchanged.

The new local-pointer test exposed inconsistent indirect cursor handling: an
ordinary increment was incomplete and an oversized compound increment was
accepted. Canonical memory lookup now uses the actual exact initialized local
pointer cell's current cursor before consulting older ordinary pointer offsets.
Updates advance that same holder. The clean candidate74g build passes the test
five consecutive times, accepting a unit step and rejecting the out-of-object
step. Earlier 74a/74c/74e failures and the temporary 74d/74f trace observations
remain under `build/rfc29-validation`; no trace code remains in the candidate.

The separately frozen `byte-cursor-forwarding` population failed its two
forwarded positive cases at 74a. Installing an actual captured byte payload
now restores its independently proved live readable interval, as specified by
the RFC, without granting write permission or an exact physical allocation
size. All eight source cases pass at 74e.

Candidate74g also gives every `byte-cursor-frames` case its expected proof
outcome. Its original alias diagnostic expectation missed the actual required
separation premise; the original failed manifest is retained. A reviewed
manifest changes only that diagnostic pattern. All 22 ordinary-object cases
across static bytes, forwarding and pointer frames pass. The byte checkpoint
population passes canonical cold/warm/uncached and compact equivalence, changed
helper and changed input-byte invalidation, corrupt recomputation and old
sidecar rejection. Core has 556 passing tests; the refreshed Frontend build has
86 passing tests after correcting its stale model-version expectation. Full
Analysis, sanitizer, integration and final cost gates remain pending.

Candidate74g's full Debug unit runs completed: 556 Core, 782 Analysis and 86
Frontend tests pass. The default 18-case checked evaluation passes; the full
76-case fixed population still needs the latest-candidate run. The original Debug73l upstream run exhausted
the unchanged 600-second deadline for each of its three cases under concurrent
development load; all three failed observations remain recorded in
`upstream73l`. Release74g still rejects both static-input upstream positives.

Candidate75 development freezes ten `byte-global-frames` cases before its
checker changes. At 74g the two forwarded constant-array positives fail, while
the eight remaining expected outcomes pass. The proposed immutable-byte
premise is restricted to the actual constant array object and is recorded in
the RFC before implementation. The primitive byte premises, target arithmetic,
write permissions and lifetime checks remain separate obligations.

The full 76-case fixed evaluation on candidate74g passes separately from the
18-case checked population: 44/44 bugs detected, 32/32 clean cases, no unexpected
reports, parse failures, tool failures or timeouts. Candidate75a's ten immutable
byte-frame cases pass through both source and ordinary objects, and 30 focused
Core plus three focused Analysis tests pass.

The 74h sanitizer integration run exposed redundant byte-context nomination
for complete inductive writers and output-slot constructors: their generic
contracts remained complete, but an extra specialized case emitted a failure
diagnostic. Candidate75c retains existing constructor, scalar and alias cases
while declining this additional byte-only nomination for verified complete
induction. Both unchanged original positive programs pass again. The same run
found the RFC0023 cache harness's stale sidecar-version assertion, updated to
26; its deliberate old-format mutation still targets version 20. The 75b build
failed on a shadowed local variable; 75c fixes the naming without suppressions.

The completed 74h ASan/UBSan run passes 1,626 of 1,635 CTest entries in
879.51 seconds. The nine failed entries are retained in its log: stale static
byte/forwarding objects, the recursive byte-specialization regression, the
RFC0023 sidecar-version expectation, and lit (which also picked up the newly
added immutable-global test). No checker sanitizer crash occurred. A consistent
full rebuild is underway; the build wrapper detects source edits during a
compilation pass and forces those inputs to rebuild before recording source
identities, avoiding misleadingly newer object timestamps from an in-flight
compile. These development observations are not a final sanitizer gate.

Candidate75a carries constant input bytes through global error-state updates
and into the upstream BOM helper. Its returned call still erased those bytes
when updating the separate local reader header. Eight `byte-helper-frames`
cases were frozen against 75c: its two forwarded positives fail and all six
other outcomes pass. After the RFC amendment, candidate75e preserves exact
input bytes only when a complete helper's every represented write resolves to
separate automatic storage, with no consumption, replacement or escaping
effects. All eight source cases pass.

### Candidate 75f and bounded comparisons (76)

The consistent 75f ASan/UBSan rebuild completed a second pass after detecting
four source inputs changed during the first pass. All 13 selected reruns passed
in 25.66 seconds, including the earlier static-byte, recursive-writer, output
construction and container-cache failures and the new global/helper byte frames.
The full 75f sanitizer suite is running separately. Strict changed-file tidy
passed all 19 files rerun after the candidate 74 findings.

The unchanged upstream static-input parser remains incomplete in 75e, although
exact bytes now reach the whitespace/value/number helpers. Its number case at
the actual offset completes. The string parser still loses its output bound
when the actual input contains multiple later quotes; this is an open precision
gap, not a passing lifecycle claim.

The `byte-comparisons` population was frozen before comparison-result inference.
Candidate 75e rejected all five intended positives and retained all five
negatives. Candidate 76b passes all ten source cases; the immutable 76c Release
pair passes all ten object cases. The cases distinguish memcmp from string
termination, unsigned character ordering and sign from implementation-specific
nonzero magnitude, and reject changed, partial and short input. Existing modeled
C-library trust remains explicit. Strict tidy passes DataflowRuntime.cpp.

Development failures retained: 76a used the wrong IntegerRange query name and
failed compilation; 76b uses `constant()` and `signedValue()`. The first focused
unit invocation failed to find string.h in the headerless unit-test environment;
the test now declares the compatible standard signatures like existing runtime
tests. The frozen integration sources and their outcomes were unchanged.

The full consistent 75f ASan/UBSan suite passed all 1,643 tests (1,433 unit and
210 integration) in 527.12 seconds. This precedes the candidate 76 comparison
and candidate 77 pointer-expression changes, so it is a preservation checkpoint,
not the final RFC gate. Candidate 76's focused comparison unit and lit tests
also pass. Candidate 76c still rejects both upstream static-input lifecycles.

Candidate 77a exposes a symbolic pointer-difference expression but initially
retained an intermediate expression identifier instead of its validated affine
coordinate. Candidate 77b retains the actual affine equality; the unmodified
extracted parse_string body now completes with the nested JSON bytes and offset
one in `string-nested76-probe.c`. This does not certify the full upstream.
The separately frozen `pointer-difference-sizes` population rejects its two
positives in baseline 76c and intermediate 77a/77d, while keeping all four
negatives. Candidate 77d establishes the pure conditional offset and copy-body
bounds; its remaining failure is the terminator, because widening forgets the
boundary represented by an unchanged pointer's unstepped offset. These failed
observations remain retained.

Candidate 77h passes all six frozen pointer-difference-size cases through both
source and object checking. The final terminator needed both the independently
proved unchanged-pointer boundary and refutation of an impossible initial
loop-exit edge before it entered the join. The old impossible edge had replaced
the established inequality with its contradictory condition.

The complete Debug Analysis suite at 77h passed 786/787 tests in 136.03 seconds.
`InitializedAdvanceEndsAtActualOutputPointer` regressed because an opaque
conditional evaluation value entered reusable numeric outputs. Candidate 77j
keeps such captured values in affine queries, while ordinary scalar assignment
and reusable integer expressions retain their existing projection. The failed
observation is retained in all-analysis77h.log. Both initialized-advance tests
and the new six-variant pointer-difference unit pass at 77j, including the
narrowing-conversion negative. The latest upstream 77f static-input lifecycles
still fail; the exact parse_string case is now left only with its allocation
footprint obligation.

The separately frozen payload-publication population fails both intended
positives in baseline 77f and preserves all four negative outcomes. It tests
publication of an actual live allocation through a local alias, helper allocation,
duplicate ownership, interior and released pointers, and a lost allocation.

Candidates 78a/78c/78d/78e/78f retained the two rejected payload positives;
their observations remain in payload78*.json/log. The causes were distinct:
missing cross-unit payload nomination, lost input forests across element
writes into fresh byte allocations, uncaptured empty-head values, unaccounted
helper-returned byte allocations, and missing explicit live-entry premises
on portable extension outputs. Candidate 78g passes all six frozen source
cases, including both local and helper allocation and all four negatives.
The caller may ignore the helper's success flag because every returning
outcome proves the unchanged owned head plus its actually acquired region.

The full Debug Analysis suite at 78c passes all 788 tests in 131.860 seconds.
That suite predates the final payload transfer changes; it is not the final
RFC validation gate. The mandatory nested upstream workflows and final
cost/reuse/sanitizer gates remain outstanding.

Candidate 78h passes the six payload-publication object cases and the three
new byte-comparison/pointer-difference/payload lit tests. Its eight-variant
payload unit also rejects an overwritten old payload and a leaking helper.
The subsequent full Analysis run passed 785/789: four older positive tests
regressed. Candidate 78i fixes nullable byte-allocation release accounting and
refines actual null slots when discharging a container call, restoring three
tests. Candidate 78l restricts strengthened empty-slot input witnesses to
payload-writing helpers and restores detachment as well. Failed runs and their
reports are retained; these targeted fixes do not substitute for the full gate.

The separately frozen payload-write-frames population fails all three intended
positives in baseline 78h and retains its three negatives. Candidate 79a verifies
the actual extracted parse_string case with its reader update intact, but the
cross-unit frame clients still lack imported conditional ownership selectors.
Candidate 79b imports those nominations with generation/dependency tracking and
passes all six frame cases and all six original payload-publication source
cases. Aliased-selector and aliased-helper fail specifically on input separation.
The independent payload-oracle79 allocation ledger, built with ASan/UBSan,
confirms all twelve concrete outcomes and exercises every allocation-failure
point of the five positive lifecycles. These extracted-body results do not
certify the full unchanged upstream program; that mandatory run remains separate.

Candidate 79b passes all 790 Debug Analysis tests (256.534 seconds while other
validation work was running), six payload-write-frame object cases, and all six
payload checkpoint tests. The unchanged warm checkpoint run performs zero
function analyses; changed publisher and imported cleanup bodies invalidate
dependent evidence. Corrupt checkpoints and the old sidecar are rejected.
The unchanged upstream static-input run still rejects both nested and malformed
parse/delete clients. Its exact string-parser case is complete, but caller
composition loses reader storage evidence and the larger parser remains
incomplete. The retained 132 MB report is upstream79b-static/source.json; these
focused successes do not satisfy the mandatory upstream acceptance gate.

Candidate 80c passes all 790 Debug Analysis tests (141.043 seconds). The exact
unchanged string-parser case now exports its ownership extension on every
return: payload-writer entry premises previously depended on final CFG block
visitation order. Its reader-forwarding wrapper is complete, although the
full unchanged nested parser still fails (upstream80c-static). The new frozen
payload-early-exits population preserves six baseline outcomes.

The separately frozen payload-reader-returns population rejects both positives
in baseline 80c and passes all six outcomes at 80e. Its forwarding helper
needs the captured complete singleton entry descriptor. Broad nomination
regresses the existing detachment lifecycle by over-specializing cleanup;
reader-forward80e-unit.log retains that failure. The subsequent refinement
keeps direct-release helpers on their existing general ownership input.


Candidates 81a–83a add bounded exact-byte while/do-loop partitions and preserve
immutable byte objects across complete helpers with represented writes to
other objects. The frozen byte-loop-partitions population passes all seven
source and object cases; byte-callee-frames passes seven source cases. The
loop unit also rejects an invalid access after the existing 32-partition bound.
The first broader nomination regressed a counter-reset `for` loop (791/792
Analysis tests); restricting this nomination to while/do loops restores that
case without changing any bound. The reader-return population passes all six
object cases. The unchanged upstream 83a run still fails both required clients,
but its exact number-parser case at input offset 6 is now complete.

Candidates 82a and 84a fail the helper payload-relocation positive while
retaining the local positive and four negatives. The helper output had dropped
an unused destination's null-slot evidence. Candidate 84c retains common
structural facts in extension outputs and passes all six frozen source cases.
Its first alias unit fails because equivalent current aliases carry distinct
provenance metadata. Candidate 84d preserves each independently live definite
alias's provenance and passes all eight relocation unit variants. These are
intermediate observations, not the final upstream or full validation gates.


The full candidate84d Analysis run passes 793/794; the stronger extension
head descriptor exposed an exact-equality check in recursive candidate
validation. Candidate85a checks structural entailment and restores the
existing recursive extension positive. All 558 Core tests pass at 85b and
formatting is clean. The comparison-container-frames population's original
signed recursive sum can overflow, so its claimed positives are invalid;
that immutable population and its failing reports are retained. The separately
frozen comparison-container-frames-reviewed population uses defined unsigned
accumulation. Its three positives fail at baseline84d, and all seven source
and object outcomes pass at85a. The numeric parse_value wrapper now verifies
for the exact offset-6 input, but its closed cleanup client still fails because
modeled strtod invalidates the incoming forest despite writing only a local
end-pointer cell. The mandatory full nested clients remain incomplete at85a.


Candidate85b passes all 795 Analysis tests in 142.785 seconds. Candidate86a
passes all seven numeric-container-frame source and object outcomes; baseline85a
rejects its three positives. Candidate87a similarly passes all seven temporary
release source and object outcomes, where baseline86a rejects all three positives.
These retain uninitialized strings, writes into owned payload slots, stale nodes,
interior releases, double releases, leaks and attached released payloads as
negative cases. The exact number-parser body then exports conditional structure
on both returns, but forwarding loses it because selector values differ.

Candidate88a passes the extracted unchanged number-parser plus forwarding and
cleanup probe, but the new container-outcome-frames population exposes a false
proof: changing the payload ownership bit is accepted when stale conditional
footprint names reconnect a changed structure to the caller's entry ledger.
The immutable baseline87a rejects that mutation and accepts its explicitly
branched positive; direct and forwarded positives still fail at88a. This failed
candidate is retained and must not serve as final soundness evidence. The next
revision invalidates those affected conditional names at helper boundaries and
allows a selector store to retain an existing payload only when its old and new
proved bits select the same ownership branch.


Candidate88c restores all six container-outcome-frame source and object
outcomes, including the rejected ownership-bit mutation. The independent
outcome88-oracle uses an allocation ledger under ASan/UBSan: all 24 expected
outcomes agree, including each of the three allocation-failure points in each
of the six workflows. It detects the released-head access and both payload
leaks when construction succeeds. All 559 Core tests, three focused Analysis
units and six selected new lit tests pass; source formatting is clean. These
runs are intermediate, pending the complete upstream and final regression gates.


The full candidate88c Debug Analysis run passes all 798 tests in 164.589
seconds. The unchanged fixed evaluation detects 44/44 bugs and accepts 32/32
clean cases, without parse errors, tool failures or timeouts. CMake formatting
and the Core Clang/LLVM dependency boundary also pass. These observations do
not replace the still-running unchanged upstream workflow or the final isolated
performance, corpus identity, reuse and sanitizer gates.


All eight files in the targeted strict tidy88 run pass after correcting braces
and nested test-condition syntax; the changed recursive-extension and dataflow
files also passed the preceding tidy86 run. The unchanged upstream88c static
parser population still rejects both clients. The number-parser wrapper is
complete and preserves its forest, but its successful offset remains unknown:
the offset-bounded for scan merged delimiter exits before conversion.

Candidate89a nominates ordinary byte-switch for scans under the unchanged
32-partition limit. The extracted unchanged number-parser lifecycle now exports
exact successful offset 7 from entry offset 6. The original byte-switch fixture
had colliding helper/client filenames and is retained with its invocation
failures. The separate byte-switch-partitions-reviewed population passes all
eight outcomes at89a; baseline88c rejects its three positives. The existing
decimal counter-reset positive regresses, losing its initialized terminator.
The next revision retains a zero interval only when current half-open bounds
prove the store wholly disjoint; overlapping and unrepresented writes still
lose that evidence.

A separately frozen upstream-construction population exercises public nested
object/array/string creation, serialization, deletion and output release,
without the parser's retained automatic-input lifetime defect. Its normal
ASan/UBSan run passes before freezing. The first allocation-ledger harness
omitted realloc and correctly aborted on an untracked release; that failed
instrumentation is retained. The corrected construction88-oracle models actual
realloc identity and failure preservation and passes all twelve outcomes:
one complete lifecycle, all nine allocation-failure points, and two output
release mutations. The analyzer's first candidate88c construction probe still
fails; runtime success is not claimed as an inferred proof.


Candidate89d passes all eight reviewed byte-switch object outcomes and all
five counter-reset object outcomes; the five counter-reset source outcomes
also pass. Candidate89f passes all 800 Analysis tests in 143.440 seconds.
The new symbolic-zero unit initially used `i < count + 0`, whose numeric
projection loses the required ordering; its plain `i < count` variant passes.
The finalized unit isolates disjoint writes versus actually overwriting the
zero, while the exploratory additive-zero failure remains in the build logs.


Candidate89d's unchanged nested parser reaches complete cases for all three
children at offsets 6, 8 and 13. Its array owner still loses head/forest
evidence while composing aliases and tail updates. Both mandatory static
upstream clients remain incomplete. The seven separately frozen string-length
copy cases reject all three positives at89d and pass all seven source and
object outcomes at90b. The targeted unit plus all 24 traversal units pass.
A transparent pointer cast additionally hid a literal's terminator; using
the existing storage-preserving cast classifier makes the unchanged extracted
cJSON duplication helper pass at90c. This does not claim the full serializer.
The seven container-alias-output cases are separately frozen against90b:
two positives fail, its payload positive and all four negatives pass.


All seven container-alias-output source outcomes pass at91a. The 91c full
Analysis run passes 802/803; the new alias unit lacks a calloc declaration,
so its failures are frontend errors rather than checker observations. That
fixture declaration is corrected before the next run. Temporary array traces
were removed from source, but restoring a backup's old timestamp initially
left the trace in the Debug object. The source was touched and 91c rebuilt;
these exploratory Debug traces are not final build artifacts.
The separately frozen mixed-helper-frame population still rejects its two
positives at92a. Its bare direct calloc-return wrapper supplies no structural
output; an explicit local initialized constructor isolates a separate missing
owned entry-head conservation premise. These failures remain recorded.


Candidate93a adds strict portable non-NaN parameter contexts (summary 26,
sidecar 27, checked encoding 12). Its Core encoding/remapping/bounds unit
passes, but the first Analysis unit reveals unsupported early-return clamps.
Candidate93b proves finite and forwarded clamps; its infinite fixture still
requires a modeled constant compiler intrinsic. Candidate94a passes all six
floating source and object outcomes and all three focused floating units,
including jump, mutation and address-exposure counterexamples.

Candidate94a's complete Analysis run passes 803/805 tests in 141.390 seconds.
ExplicitReturnExportsAttachedOwnership and DetachmentPreservesBothAllocationPartitions
regress. The independently frozen singleton-link-ownership population exposes
a false proof of a leaked disowned child at93a and94a; immutable91b rejects it.
The direct positive improves at94a but the forwarded positive remains incomplete.
The unchanged extracted one-element array still loses its footprint. Temporary
94 tracing identifies lost separation after a derived callee output, before the
parent selector/child stores. Traces are restored from text backups with current
modification timestamps; they are not production instrumentation. These are
failed development observations, not completion evidence.


Candidate95a's extracted one-element array passes after verified derived
outputs retain separation from surviving neighbors and definite head aliases
retain that separation too. Reverting the92a mixed frame only hides the new
negative: the same disowned-child helper without its reader write is also
falsely accepted by91b. A trace confirms that the successful exit transfers
through a fresh child slot, but joining its void returns drops that output.
Candidate96 adds final validation of the portable output carriers used by
per-exit allocation accounting. Its first revision incorrectly rejects nullable
fresh results; the null arm of a proved proper returned forest contributes no
allocation, which96b now handles. The new four-variant transfer unit passes.

The singleton96-oracle independently tracks allocations under ASan/UBSan.
All fifteen outcomes agree: two positive workflows, lost/released/disowned
children, and failure of either of the two acquisitions for each workflow.
The disowned and lost successes each retain one live allocation; the released
child success trips the sanitizer. These runtime observations corroborate
the counterexamples rather than certifying analyzer soundness.

Candidate96b passes805/806 Analysis tests in140.971seconds. The remaining
detachment regression was already present in93a and came from over-refining
release-capable entry descriptors at92b. Restoring the generic owned input
except for actual construction nominations fixes it at96e. Redundant preserved
and extended outputs now use the exact preservation instead of adding an
unrelated fresh region. All three focused attachment/detachment/transfer units
pass. Registering actual FieldDecl identities while installing call contexts
restores the independently typed reader field's helper frame; all five frozen
singleton source outcomes pass at96d. The directory initially labeled96b-bin
was found by source hashes to contain96a, was renamed96a-bin, and is not96b
evidence. Singleton96b-source therefore repeats96a's failure;96c-source and
96d-source are the later Debug observations.


Candidate96e passes the targeted attachment, detachment and surviving-transfer
units; all five singleton object outcomes and all seven alias-output source
and object outcomes pass. Candidate97a passes all109 recursive/ownership units
in65.905seconds. The separately frozen repeated-fresh-output population rejects
its three positives at96e and97a while retaining four negatives. The independent
repeated97 allocation ledger under ASan/UBSan agrees on all49 normal and injected
failure outcomes. Tracing finds a genuine lost relation name: reassigning the
temporary allocation holder removes it from an older head's ancestor set,
although its shape, members, inputs and ownership are unchanged. Candidate97b
permits only this loss of hints when retaining a fresh-call separation frame;
the extracted inline three-element array remains incomplete. The next local
fold change shares a verified updated head with its still-live definite aliases.


Candidate97e retains the existing24 container-analysis tests but fails the new
local-fold alias positive and the three-element array. Candidate97g fixes the
local-fold unit by proving separation between complete concrete allocation
graphs and carries all retained proper ancestors to a newly attached child.
The array then has valid structure through its loop, but restoring an enclosing
prefix loses its prior separation from the outer item. The item selector write
therefore retires the completed list. Candidate97i preserves independently
proved prefix separation and passes the extracted inline three-element array;
its ordinary per-element helper variant remains incomplete. All110 recursive
and ownership units pass in65.486seconds. The temporary97f/97h tracing is removed
from production source. The repeated-fresh-output population still fails its
three positives at97g and keeps all four negatives; no successful full nested
upstream workflow is claimed. Candidate98 adds verified helper prefix frames,
with final regressions and upstream acceptance still pending.

### Candidates 99-102: outcome-specific ownership transfer

Candidate 98d's retained state failed three regressions before any new rule.
A conditional size argument such as `min(a, b)` lost its relation to the
operands because the evaluation-site union replaced the recognized minimum;
preferring the structured expression restores the cJSON print copy. Directly
returning a complete callee's fresh byte result settled no allocation, and a
complete release helper did not settle a represented ordinary allocation head.
Both now use the same single-allocation accounting as the modeled release.
With those three corrections the unchanged pinned `print-delete` upstream
client passes again; it had regressed between candidates 77f and 78h and the
retained candidate98b binary still rejects it.

The helper-frame reader variant also failed: a write confined to an automatic
object retired a forest whose every node was allocated in this invocation.
An automatic object and a heap allocation are distinct objects, so that frame
now preserves such forests, and an allocation identity stays separated from
this invocation's automatic objects after attachment retires its resource
record. The stale Frontend model-version assertion was corrected to 26 and the
paired-reader-counters lit expectation to the actual extent diagnostic; no
checker behaviour changed for either.

Candidates 99-102 add the attaching-helper transfer the upstream construction
workflow needs. `container-combined` may now carry the constant `end` value
one: the final forest is the disjoint union of both complete entry footprints
and a possibly empty fresh region this call acquired. Two incoming complete
singleton heads related by an explicit distinct-object premise are separated
at entry, which is what lets an attach helper prove the relation at all. A
function whose return expression is exactly a complete call's integer result
forwards that call's outcome-specific outputs into its own outcomes, and an
immediate test of a call's own result installs the verified transfer on its
CFG edge together with the structural outputs on that transfer's own paths.
A more specific output stating that a path's final footprint is exactly its
incoming regions retires that call's fresh region for the path.

The first revision replayed every outcome-specific output on the tested edge.
That accepted two leaks: replaying an unrelated path's captured entry
description erased the caller's evidence for a still-live allocation. The
failed candidate and its four-variant reproducer are retained. Restricting the
structural replay to the transfer's own paths restores both rejections while
keeping the positive workflow.

The separately frozen `attached-payload-transfer` population covers this rule
across two units: a successful attach transfers both owned inputs plus the key
it allocated, and losing the item on the failure path, releasing it twice, or
ignoring the result all reject. All four cases pass at candidate 102 and the
immutable candidate98b binary rejects the positive. Extracted unchanged cJSON
object and array attach lifecycles also pass, including `cJSON_CreateObject`
with `cJSON_AddItemToObject` and `cJSON_Delete`. Attaching a second element to
a non-empty parent, and the full nested construction and parse clients, remain
incomplete.

Candidate 103 fixes two strict-tidy findings exposed by the changed files, one
of them an excessive-padding report caused by a new flag; the bounded scan now
marks exhaustion with a null entry in its own set. All eleven changed checker
files pass strict clang-tidy with no warning or error.

At candidate 103 the Debug unit suites pass 811 Analysis, 561 Core and 86
Frontend tests, the complete 205-case lit suite passes, source and CMake
formatting are clean, the Core Clang/LLVM dependency boundary holds, and the
unchanged fixed evaluation detects 44/44 bugs and accepts 32/32 clean programs
with no parse failure, tool failure or timeout. Twenty-one related frozen
populations covering payload publication, relocation, reader returns, early
exits, write frames, singleton ownership, in-place extension, outcome and
alias frames, container call and local frames, construction, mutual
construction, construction helpers, temporary release, numeric and comparison
container frames, the mixed and reallocation ledgers, and the recursive and
output transports all pass with no failure.

Of the mandatory upstream clients, `print-delete`, `parse-double-bad`, both
construction output-release mutations and both lifetime-audit cases pass,
while `parse-delete`, `malformed-delete`, `nested-serialize` and `nested-print`
remain rejected. Full sanitizer, corpus identity, warm-reuse and isolated cost
gates have not been rerun since candidate 88c and remain outstanding. RFC 0029
therefore stays Accepted, not Implemented.

### Candidate 104: corrected gate reading, cost measurements and one blocker

An earlier revision of this record stated that the milestone "fails the
mandatory 1.10x cost gate" for checked contract mode. That was a misreading of
section 6 and is withdrawn. The 1.10x median time and peak RSS bound applies to
three isolated sequential **ordinary** Release runs. Checked corpus projects
carry a different requirement: each retains its 600-second deadline. The two
were conflated; the measurements below are reported against the gate as
written.

Checked whole-program mode, user CPU on a quiet machine, against the committed
RFC 0028 cold-coverage baseline:

| project | baseline | candidate 104 | deadline |
| --- | --- | --- | --- |
| log.c | 0.2 | 0.12 | inside |
| cJSON-program | 30.1 | 60.2 | inside |
| jansson | 117.7 | 215.2 | inside |
| linenoise-program | 16.9 | 267.7 | inside |
| lua | 593.0 | over 1,188, abandoned | **exceeded** |

Four of the five projects stay inside the 600-second deadline. lua does not: it
was abandoned after 1,188 seconds at 4.7 GB resident. Note that lua sat at 593
of its 600 seconds before this milestone began, so it had 1.2 percent headroom
and could not absorb any feature work. That is a property of the gate as much
as of this change.

The per-project ratios vary widely. cJSON and jansson are near 2x, while
linenoise is 15.8x. linenoise is the outlier rather than the rule, which is why
a single headline multiplier misdescribes this milestone.

Profiling attributes the cost to the integer relation and range machinery that
every new rule queries, not to the byte-level parser rules. On one linenoise
unit the analysis spent its time in `RelationTracker::equalsOf`, in
`integerRangeAt`, and in the allocator. `equalsOf` linearly scanned every
tracked relation and returned a freshly allocated vector on each call, and
`atMost` and `atLeast` called it on nearly every integer range query.

Candidate 104 replaces that query with an allocation-free visitor that also
skips the keys that cannot name the queried place, an internal index
refinement the RFC's unresolved-questions section explicitly allows. The
linenoise unit falls from 74.35 to 60.76 seconds of user CPU, roughly 18
percent, with no change in behaviour: all 811 Analysis, 561 Core and 86
Frontend unit tests pass, all 205 lit tests pass, the fixed evaluation stays at
44/44 and 32/32, and twenty-six frozen populations report their expected
outcomes.

Two earlier attempts are recorded as failures. Deferring in-round case analysis
to the settled passes moved 662 analyses but did not change wall time, because
the settled passes then perform the same work; it was reverted rather than kept
as dead complexity. Cutting the byte-level parser layer was considered and
rejected on evidence: the hot path is shared by all rules, so removing those
rules would lose capability without addressing the cost.

The ordinary 1.10x gate could not be measured to gate quality here. Three
sequential ordinary runs produced medians spread across 149 to 197 seconds on
this machine, against a 0.5-second spread in the committed RFC 0028 baseline
run. A spread that wide cannot resolve a 10 percent threshold. This gate needs
an isolated machine and is still outstanding, not failed.

The implementation therefore has exactly one demonstrated gate violation, lua's
600-second checked deadline, and one gate awaiting an isolated measurement.
Whether to raise the checked deadline for large projects, to exclude lua from
checked-mode timing with its limitation recorded, or to treat per-analysis cost
as its own milestone is an acceptance decision for the owner; this RFC forbids
reducing a resource bound silently, and no bound has been changed here.

### Candidate 105: CI repairs and the landing decision

Continuous integration on candidate 104 exposed two defects that local runs had
missed.

- `checked-conditional-count-arguments-rfc0029` and its object-mode twin
  rejected the frozen `zero` case, `touch(out, argc > 1 ? 4 : 0)`. Bisecting the
  retained candidates places the regression between candidates 76 and 77. A
  conditional integer argument has no single affine form, so the callee's interval endpoint
  lost the captured call-entry identity that the conditional requirement already
  used. `PlaceBuilder::affineFromPath` now gives both the same captured
  identity. The frozen population reports its recorded outcomes again: `good`,
  `converted` and `zero` accepted, `short` and `changed` rejected.
- clang-tidy rejected two `bugprone-optional-value-conversion` findings in
  `DataflowByteContents.cpp`. Strict clang-tidy is now clean on every source file
  changed by this milestone, not only on the files touched last.

The documentation site also failed to build: the checked-code guide gained a
section that the site generator had no page for. It is now published as
`guides/composing-helpers`.

After these repairs the Debug build passes all 1,709 CTest entries and all 205
lit tests, the fixed evaluation stays at 44/44 and 32/32, and every frozen RFC
0029 population reports its expected outcomes.

**Landing decision.** This milestone lands with RFC 0029 kept at **Accepted**,
not Implemented, following the precedent of RFCs 0007 to 0013 and 0018. Three
acceptance items remain open and are not waived:

1. lua exceeds its 600-second checked deadline (candidate 104). The deadline is
   unchanged and continues to gate the flip to Implemented.
2. The ordinary 1.10x time and RSS gate still needs three isolated sequential
   runs on a quiet machine.
3. The upstream cJSON parse-delete, malformed-delete, nested-serialize and
   nested-print workflows still end `checking-incomplete`.

No resource bound, acceptance denominator or required workflow has been
reduced. The weekly corpus job runs ordinary analysis only, so lua's checked
cost does not affect it. The next milestone should be scoped to
per-analysis checked cost, measured against `checked_case_requests` and user CPU
on every candidate.
