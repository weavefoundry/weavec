# RFC 0029: Compositional invariants for recursive C workflows

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-16
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFCs 0017–0028; supersedes RFC
  0026's exact two-counter discovery restriction and RFC 0027's direct,
  single-parameter cleanup induction restriction where the rules below apply.

## Summary

Infer reusable memory contracts for stateful, recursive C workflows. Discover
buffer and cursor roles from operations, retain relationships between entry
values and current storage through helpers, and check recursive contract
candidates as a group. Express required synchronous callback behavior as a
caller obligation rather than treating an unavailable binding as a proof.
Use complete parsing, serialization and cleanup workflows to validate that
the existing ownership, initialization, buffer, callback and recursive models
compose across source files, compiler objects and validated checkpoints.

The owner explicitly requested drafting this RFC followed by implementation
end to end. Accepted records that authorization; it does not claim independent
review, a separate RFC merge, or completed implementation. Any additional
semantic decision must be recorded here before the corresponding code changes.

## Motivation

At `bbf14c3`, the latest retained validation reports 150 complete conditional
contracts among 1,619 selected definitions. The fixed evaluation detects all
44 bugs and accepts all 32 clean programs. These denominators measure their
specific populations, not arbitrary-C soundness or recall.

Three separate-source probes establish the practical gap. With unchanged
pinned cJSON and an explicit `cJSON_InitHooks(0)`, a closed client that creates
and deletes an object succeeds with zero entry requirements and only modeled
C-library trust. A client that parses a small JSON document and deletes the
result remains incomplete. So does a client that creates an object, prints it,
and releases the object and printed buffer. The retained RFC 0028 binary has
SHA-256 `47b881a29c1bb9b7ee547f144e72ef0342376d9a12a2d4ca72db6417156510a3`.

Current buffer discovery requires exactly two unsigned counters; cJSON's
buffer records also contain depth. Recursive may-effect fixed points already
exist, but reusable recursive memory postconditions remain restricted.
Requirements projected through helpers frequently lose their interval or
entry identity. Concrete contexts cannot replace a reusable relational
contract and reach explicit bounds on large recursive programs.

This milestone targets those compositional limitations. A recognized record
shape or familiar library name cannot certify an implementation. No cJSON
or other third-party function receives a handwritten trusted summary.

## Soundness

Retain RFC 0018's conditional, single-threaded source guarantee, target C
semantics, explicit trusted boundaries, and failure on unresolved selected
obligations. Required premises must be discharged by actual callers. A new
contract candidate, callback prototype, layout descriptor, capacity field or
recursive declaration contributes no independent evidence.

The following distinctions are mandatory:

- Accessible storage versus initialized contents and logical cursor position.
- Current pointer identity versus an entry pointer or a saved pre-growth alias.
- Unsigned C arithmetic versus unbounded mathematical interval endpoints.
- Successful output publication versus partial construction and failed calls.
- A supported finite ownership forest versus cyclic or shared owned children.
- An assumed recursive contract being checked versus a published verified one.
- A callback's demanded behavior versus the behavior established for every
  target that can actually return.
- Permission to release an allocation versus proof of complete cleanup.

Required rejected counterparts include truncated/uninitialized input, forged
capacity, cursor escape, overflowing growth, stale aliases after replacement,
false-success returns, failure cleanup that leaks or releases twice, skipped
recursive children, duplicate ownership, nondecreasing recursive proof cycles,
and callbacks that fail to establish required outputs. Every local access and
every relevant return remains an obligation, including error paths.

Unknown mutation invalidates dependent facts. A field unchanged by a verified
callee may retain its value; an unmodeled write cannot. Joins retain only facts
established by every incoming alternative. Dropping a guard, interface path,
callback target or recursive premise loses the dependent proof as well.

A current container predicate's initialized head selectors and empty head
links may answer the same value query in call-case capture and return-guard
checking. Require the exact head cell, a non-null live holder and the validated
target field type. These are values already established by the predicate,
not new facts inferred from a record's layout. A changed selector, replaced
holder, released object, invalidated predicate or nullable head supplies no
value. Queries do not install persistent scalar facts; ordinary call-entry
snapshots remain necessary when the call changes a guard operand.

A represented store may retain zero-initialized byte intervals outside its
proved write range. Constant intervals split at the written boundaries;
an established half-open ordering may retain an entire disjoint symbolic
interval as well. Prove the write ends no later than the interval begins, or
begins no earlier than the interval ends, using the current unchanged values.
Unrepresented overlaps lose the evidence. Zero facts in a distinct concrete
automatic object or a distinct live allocation from this frame may survive
only with actual storage separation. Different indirect path names alone do
not establish separation. This permits attachment to one calloc-initialized
child slot without discarding the zero initialization of the other slots.

A verified structural output for a current pointer also applies to its
definite zero-offset aliases that still denote the same actual head storage
and ownership share. Recheck those identities after the call's effects;
may aliases, interior offsets and replaced pointer cells supply no equality.
Install only the verified output descriptor, including its actual outcome,
and propagate a footprint equality only after that output's independent
conservation relation has been applied. This does not restore a pre-call
descriptor, a released allocation or an ancestor's larger forest.
Before a checked store into an established live non-null container head,
unfold its existing footprint while the old fields and ownership selector
still hold. This also applies when non-nullness and empty slots came from
entry case premises and no branch has yet been visited. The subsequent store
retires affected contributions normally; it cannot reconstruct entry ownership
using the newly written selector.
A completed local structural fold likewise updates every current definite
zero-offset alias with the same sharing identity. Those aliases denote the
proved live head even when a reused allocation site has retired its old
concrete storage name. Require each alias to remain unmoved and uninvalidated;
copy only the newly proved shape and conserved footprint, publish the aliases
as attached, and retain the fold's established separation. This rule cannot
restore a merely possible alias or an enclosing ancestor forest.
An enclosing forest instead needs its independently retained prefix frame:
the current node was an established suffix or proper child, the updated
subtree still entails the ancestor's node capability, and every newly attached
region is disjoint from that ancestor. Save the ancestor-minus-subtree
footprint before invalidation and compose it with the verified replacement.
Definite head aliases are updated as aliases, never restored with an old
prefix descriptor. Retain verified ancestor hints for the updated subtree
and all live aliases of its parent so rebinding one cursor cannot erase the
only surviving name of the relation. A newly attached child also inherits
each independently retained proper ancestor of its parent; subsequent
mutation of that child must preserve every such enclosing prefix, not only
the nearest cursor name. An enclosing prefix retains its prior separation
from a surviving neighbor only when every nonempty replacement child is
independently separated from that neighbor too. Reestablish this relation
after composing the prefix; restoring structure alone loses this evidence.
The same prefix composition applies across a complete helper with a verified
preserved or unconditionally extended footprint on the same root parameter.
Capture the actual proper ancestors and their ancestor-minus-subtree and head
footprints before effects. Every represented write or consumption must be
confined to the passed subtree; an additional write needs the existing
independent local/input/fresh separation frame for that entire ancestor.
Unknown destinations, escaping effects and consumption outside the subtree
lose the frame. The returned subtree must entail the ancestor's node
capability, and its conservation output must already have been applied.
Then recompose the captured prefix with that verified output, restore its
unchanged live head, and retain only separation from independently surviving
neighbors also separated from the returned subtree. A structural output alone,
a changed root path or an incomplete helper cannot restore an ancestor.

Two complete concrete graph witnesses with no summarized suffix or incoming
region may prove separation by disjoint actual node and payload storage.
Every member must still be a live concrete object or an established current
local allocation; distinct allocation-site names for summarized instances
provide no such proof.

Zero-byte evidence in distinct indirect objects may also survive a write under
an explicit separation premise on their unchanged entry identities. Project
that premise using the same immutable backing identities as checked memory
calls, and discharge it at actual callers. A different field spelling or a
replaced pointer alone never establishes this separation.

An actual write to a private automatic scalar cell is separated from incoming
byte storage when that cell's address is never taken. Preserve the existing
content and zero facts of other storage across that write. Resolve the written
object itself, not a referent reached through a local pointer. Records, arrays,
static storage, exposed scalar cells, writes to the same object and unknown
write locations do not qualify for this frame rule.
An indirect pointer-cell read or update may resolve to its actual automatic
pointer variable when the referring pointer identifies exactly that whole
initialized cell. Validate ordinary compatible pointer types, zero cell offset,
full cell bounds and live storage; exclude volatile/atomic cells and ambiguous
storage. Use that variable's existing cursor for the value and update that
same cursor. The pointer variable's mutability authorizes its own cell only,
not the storage it points into. This rule preserves facts
already established by a scan; it introduces no new input contents.
An independently live unchanged entry byte object is also separate from this
invocation's automatic cells and established fresh allocations. Retain its
contents across a represented write confined to those objects, including a
local pointer cell whose address is later passed to a helper. Require the exact
current entry storage and live pointer; a changed, escaped, released or replaced
entry supplies no frame. For an allocation, actual freshness and current
validity must hold independently. Merely different indirect names and unknown
write locations supply no separation.

A complete inferred helper whose every represented write is confined to this
invocation's actual automatic storage may preserve exact byte records in
different automatic objects or independently live unchanged entry storage.
Resolve each effect to its actual object, including addressed scalar cells;
all write roots must be separate. Unknown destinations, unmodeled alternatives,
consumption, replacement and escaping effects decline this frame. No callee
name, declaration or incomplete contract supplies it, and preserved bytes still
require independent validity and bounds when read.

An ordinary integer cell in automatic storage may recover the value zero from
a current unconditional zero-byte interval covering its complete target
representation. Resolve record offsets and exact constant array selections
against the actual C layout and object extent. Partial coverage, ambiguous
selectors, bit-fields, volatile/atomic storage, stale writes and source-copy
ranges supply no such value. Derive this fact from the current byte evidence
when needed; a past `memset` alone does not establish a persistent scalar fact.

A nonzero test of an ordinary byte read is infeasible when its live initialized
position is proved equal to the zero of a current termination witness. Check
that equality before learning a strict pre-terminator bound; an impossible
negative coordinate must not enter a loop join. Volatile/atomic reads, changed
contents, invalidated witnesses and merely initialized bytes cannot refute
the branch. The witness and all of its guards must already hold independently.

Bounded exact byte contents may refine a scan of an actual initialized object.
An ordinary eight-bit character-array initializer can establish at most 64
consecutive byte values in one initialized-range record. The same rule applies
to an ordinary const-qualified static local array: C requires its constant
initializer and prohibits modifying that object. Mutable static arrays do not
receive this premise, since a previous invocation may have changed them.
Volatile and atomic storage remain excluded. Its constant endpoints
must match the stored byte count. This is a content must-fact: overlapping or
unknown writes retire it, separated writes preserve it, and joins keep only
identical supported contents. Partial writes may retain exact unaffected slices.
Do not recover an initializer after mutation or use volatile/atomic reads.

A byte read may query that record only after independent live-storage, bounds,
and initialization checks, with a represented finite interval of possible byte
positions wholly covered by the record. Evaluate comparisons in the actual
target integer type. The minimum and maximum matching positions bound the
selected edge; no matching byte makes it infeasible. Gaps between matching
positions remain possible in the affine abstraction. This neither assumes
input syntax nor grants permission to read outside the established interval.

For an input case carrying actual bounded byte contents, a single-entry `while`
or `do` region may use the existing `MaxTraversalIterations` iteration partitions.
Counted `for` loops retain their existing bounded-range nomination. In a byte
context, a single `for` region with an ordinary character read controlling a
switch and a plain incremented integer index may use the same partitions when
its bound is expressed through an offset. This nominates input dispatch scans;
subsequent copy or replacement loops retain their existing nominations. Removing
the region's header backedges must
leave an acyclic region, and nested or irreducible regions are not nominated.
Each partition executes the ordinary CFG transfer and checked branch rules;
byte contents nominate extra precision but establish no loop exit or operation.
At the existing last partition, retain every further backedge and apply the
ordinary fixed-point join and widening. Never truncate execution or infer that
the loop terminates at the partition limit. This can retain the first actual
delimiter exit instead of joining it with later equal bytes. Existing context,
per-block visit and traversal bounds remain unchanged.

A modeled `memcmp`, `strcmp` or `strncmp` may retain its result sign when
independently live initialized byte values establish it within the existing
64-byte bound. Compare bytes as unsigned characters, stop string comparisons
at the first zero, and honor a proved exact nonnegative count for bounded
comparisons. Literal bytes come only from the actual ordinary narrow literal
expression, never from a shared literal-storage identifier. A mismatch proves
only negative or positive, not a particular magnitude. Equality requires the
whole count or a shared terminator. Unknown, mutated, partially initialized,
volatile/atomic or inaccessible contents supply no result fact. Runtime access
obligations remain independently required and the existing library trust is
reported; this does not introduce a trusted user-function summary.

Actual call contexts may carry these bytes relative to a byte-pointer input
whose current offset is exact. Capture only an entirely proved initialized
slice before call effects and install it under the callee's corresponding
entry storage. Count byte payloads against the existing 64-fact bound and input
paths against the existing path bound; decline excess rather than raise either
limit. Portable contexts encode payloads canonically as hexadecimal bytes and
reject malformed, oversized, duplicate or unmappable entries. Callback types,
library names, parameter names and merely const-qualified pointers supply no
contents. These are explicit context premises, not trusted library contracts;
ordinary object and checkpoint validation applies to every dependent result.
A complete conditional generic contract may still be specialized when actual
byte contents are captured: its sufficient generic requirements can cover
branches impossible for that input. Recheck the body with all captured premises;
never erase a generic requirement without a separately completed call case.
A byte-content entry denotes a live initialized slice of that length, as proved
at capture. The callee may use that minimum accessible length for checked
bounds, but it is not an exact physical allocation extent or write permission.

An exact-byte record may additionally retain that its actual object is a
const-qualified ordinary character array. This requires the array declaration
itself, not a const-qualified pointer or cast, and does not cover allocated
storage. Its bytes may survive a represented write to another storage identity:
defined C cannot modify that constant object through a hidden alias. A write
to the represented object still retires overlapping contents, and an unknown
write retires the record. This frame neither proves a write legal nor supplies
liveness, bounds or release permission; all those obligations remain checked.
The same frame applies to a complete helper only when every represented write
destination resolves to an actual storage identity other than the constant
object. Unknown write destinations and incomplete or unsafe calls supply no
frame. Pointer replacement still retires its current referent evidence.
Joins retain the immutable premise only when every alternative establishes it.
Call contexts may carry it only beside captured exact bytes, encoded as a
canonical `k:` path record, counting each marker against the existing fact
budget. Missing byte premises, duplicate markers and failed path remapping
invalidate the context. Mutating or releasing a constant array remains rejected.

General concurrent safety, tracing collectors, arbitrary shared graphs,
unrestricted nonlinear arithmetic, arbitrary type punning and nonlocal control
transfer remain outside the model. Supported C can still be conservatively
rejected when its invariant cannot be represented or established. An upstream
defect discovered during validation remains a rejection with a concrete
counterexample; acceptance must not suppress it to obtain a green workflow.

## Detailed design

### 1. Semantic discovery of state roles

Replace exact record-field-count discovery with bounded candidates selected
from actual uses. Candidate data fields are non-function pointers to supported
fixed-size scalar or pointer elements. Candidate counters are represented
unsigned non-Boolean integer fields. Additional counters, flags, nested hooks
and unrelated fields do not by themselves disqualify the record.

Collect evidence from indexing and pointer arithmetic, allocation extents,
guarded accesses, initialization/copy lengths, and assignments. Distinguish
accessible capacity, initialized logical length, and consumed/produced cursor
position. The same field may have different roles in different proposed
predicates; only a consistent proved predicate is published. Ambiguous usage
declines inference or retains bounded alternatives; field names and declaration
order must not authorize stronger semantics.

Even a record with exactly two unsigned counters must have an evaluated
indexing or pointer-arithmetic use of its candidate backing field before the
legacy two-counter fallback is nominated. Merely dereferencing a scalar
payload beside two unrelated counters does not nominate a buffer predicate.
This filter supplies no new proof and does not alter established predicates.

Keep immutable discovery within function/TU preparation. Candidate descriptors
are validated against Clang's actual target layout, and the flow-sensitive
facts remain in Core. Preserve the existing 16-descriptor and 64-instance
bounds. Candidate enumeration must be bounded before allocating a Cartesian
product. A limit remains explicit incomplete coverage.

A forwarding helper may import a callee's validated layout candidate even
when that callee's generic contract is incomplete. This nominates roles only:
the helper must export the sufficient input predicate, and each actual caller
must establish it. An incomplete callee still supplies no guaranteed output
or complete-call proof. Layout import cannot strengthen a post-call state.
Likewise, a forwarded forest requirement may nominate an entry predicate for
a complete record type whose layout matches the transported descriptor. The
existing opaque-record route and complete-record route carry the same explicit
caller premise; neither a record declaration nor a forwarded call proves it.

Establish a predicate from actual initialized fields, live storage, extents,
initialized intervals and permissions, or as an explicit sufficient entry
requirement. Requiring a predicate at entry is not permission to restore it
after a mutation. Existing buffer ownership and element ownership remain
independent of initialized bytes.

### 2. Versioned relational inputs and outputs

Use the existing immutable entry snapshots as the basis for relational
contracts. Factor the common capture, projection, substitution and invalidation
operations used by buffers, cursors and checked memory calls into shared
implementation helpers. Preserve separate may-effects and must-proofs; neither
may be substituted for the other.

A relational predicate may describe a pointer's object and offset, an
accessible interval, an initialized interval, a terminated prefix, and
relations between represented scalar endpoints. Its portable interface paths
refer to entry snapshots or established output values explicitly. Mutable
field spellings do not become immutable values merely because they appear
in a summary. Output requirements continue to carry outcome and input guards.

For a reader, relate `0 <= position <= initialized <= capacity` to its actual
backing storage. For a writer, separate the written prefix from unused
capacity, and require a real zero store when promising termination. A helper
may return a pointer at the old cursor and advance the cursor only after
proving its guarded extent. Read-only forwarding preserves identity and
premises through copied records and nested fields.

Reader inputs may use the existing initialized-interval and extent requirements
without introducing a distinct predicate encoding. A changing field cursor
cannot be projected to its entry field spelling. Affine projection follows the
same numeric-write exclusion as typed integer-expression projection, including
equalities used to find alternative interface names. Existing relation envelopes
may then export a stable upper endpoint that covers every iteration. Failure to
find such an endpoint remains incomplete. This sufficient readable interval
supplies no write permission or ownership; a const-qualified field alone proves
neither initialized bytes nor accessible extent.

A byte reader with a fixed backing pointer, initialized input length and a
changing cursor may use a distinct `reader1:` variant of the buffer descriptor.
The descriptor's length field is the cursor; its capacity field is the readable
input length. It requires `0 <= cursor <= capacity`, actual accessible storage,
and initialization of the entire `[0, capacity)` interval. It grants no backing
write permission. The first variant applies to const-qualified byte pointers;
constness nominates a candidate but establishes none of its premises. Reader
and writer descriptors cannot entail one another. Ownership and an actual zero
at the cursor remain separate capabilities. Every cursor or backing change
invalidates affected facts and every returning reader output must be proved
from the actual final state, including partially consumed failure returns.
The variant shares bounded layout validation, strict remapping and transport
with the existing descriptor; malformed or noncanonical encodings are rejected.
Reader nomination requires an indexed counter and an observed comparison with
the readable extent counter. A constant-offset read beside unrelated counters
does not nominate this predicate. A concrete exhausted or escaped cursor case
may decline the generic reader premise and check its actual early-return path;
an executed read still needs ordinary storage and initialization evidence.

A zero-based unit-stride local index may traverse a stable reader with a strict
unsigned guard `position + index < capacity`. The established reader premise
gives `position <= capacity` at entry. Induction then proves
`index < capacity - position` in the body: the initial sum cannot wrap, and a
successful strict test leaves room for the next unit increment. Require the
index, sum and counters to have the same unsigned non-Boolean type. The index
must not be address-taken or changed in the body, and the body must not change
the reader, call unknown code or admit an entry through a label or outer switch.
Early exits, outgoing gotos and internal switches do not weaken this invariant.
Bound eligibility with the existing loop syntax budget; non-unit increments,
non-strict guards, narrowing conversions and unrepresented mutation decline
this rule. It provides arithmetic bounds only, never initialized bytes, writable
storage, an exit count or a promise that the loop processes all input.
The same induction establishes `index <= capacity - position` at the loop
condition and on every exit. Retain this weaker bound alongside the strict
body bound so ordinary CFG joins preserve it across exhaustion and early exits.
It does not establish equality to the available length or a produced count;
relationships to other counters still require actual scalar evidence.

A comparison of an established same-array byte-pointer difference may refine
the corresponding byte coordinate. First prove live in-array/one-past operands,
the target `ptrdiff_t` range and every intervening integer conversion. The
initial projection covers a difference from a constant-offset array base and
a represented integer bound. Record the actual edge inequality even when a
particular iteration already satisfies it, so ordinary nonwrapping pointer
updates and CFG joins may preserve the relation. No difference with unknown
provenance, a potentially lossy conversion or an unproved range refines a path.
A branch comparing independently live in-bounds same-object pointers may be
refuted by the current mathematical coordinate relations. Check the opposite
inequality before learning the branch's relation; an impossible initial loop
exit must not overwrite an established bound. A shared spelling, unrelated
objects, invalid pointer formation or an unproved coordinate supplies no such
refutation.

A loop's cursor boundary may be another unchanged pointer's represented
same-object offset, including an evaluated conditional value that is never
stepped. Both incoming states must independently establish the inequality and
agree on that pointer's storage and offset. Preserve only the established
boundary across widening; a changed holder, replaced storage, self-dependent
endpoint or unproved edge supplies no invariant. Existing candidate and
relational bounds still apply.

An evaluated side-effect-free ordinary integer conditional expression may
retain the union of its target-typed arm ranges in an evaluation-site value.
It selects one value from that union; it does not assert which arm ran. Require
valid represented ranges and exclude volatile/atomic accesses or side effects.
Capture at the actual CFG expression evaluation, then use the saved value in
pointer offsets and integer expressions. Before reusing the site on another
iteration, snapshot or invalidate every dependent prior value exactly as for
existing numeric call results. Later condition changes cannot rewrite the
value used by an earlier pointer or allocation.

The same independently validated ordinary byte-pointer subtraction may enter
the existing typed integer-expression domain. Require identical live storage,
proved in-array or one-past coordinates, a representable target difference,
and nonnegative coordinates individually representable in the target signed
difference type. Capture the actual coordinates, preserving subsequent C casts
and arithmetic rather than substituting the difference's range. An assignment
may retain an affine equality derived from that expression only when every
conversion and operation preserves its mathematical value. Ordinary dependency
snapshots freeze it on later writes; advancing an endpoint never resizes an
older allocation. Unknown provenance, element scaling, unproved coordinate
conversions or overflow decline the symbolic expression.
A saved evaluated cursor, including a postfix-increment result, may use the
writability of its exact current allocation when another live represented
holder still establishes that allocation's writable `free`-family origin.
Require actual storage identity, satisfied acquisition guards and a live,
unescaped holder. This confers write permission only; bounds, validity,
initialization and release ownership remain independent. A stale cursor,
changed object, declared-only resource or another allocation supplies no proof.
For an established byte-buffer predicate with a stable projected entry extent,
subtraction may
export the sufficient premise that the extent is at most target `PTRDIFF_MAX`.
Encode this with the existing unsigned 64-bit byte `sum-fits` requirement by
adding `UINT64_MAX - PTRDIFF_MAX`; callers must discharge it normally. Only
already established same-array, live, bounded positions use this premise.
It establishes a difference range, not array storage or an in-bounds cursor.
An established current buffer predicate may discharge a copied cursor's extent
requirement when both still identify the same backing storage and the access
fits that predicate's capacity. Do not export an unrelated entry interval
envelope merely because the copied pointer retains an entry spelling. Replaced
storage and lost buffer predicates supply no such discharge.
The capacity proves an accessible lower bound, not the allocation's exact
physical size. Materializing that predicate must not install capacity as an
exact ordinary spatial extent: an access beyond the advertised capacity still
needs independent bounds evidence, but is not thereby a demonstrated access
beyond the physical allocation. Preserve actual allocation and declaration
extents, and retain ordinary violations established from those extents.
A buffer whose backing is still its unchanged live entry object may export
additional extent and initialized-interval requirements on that same entry
pointer. These are explicit caller premises beyond the buffer predicate's
minimum guarantees. Require actual storage identity and absence of replacement
on every path. A retained separation identity after growth does not qualify;
ordinary unknown-call, release and pointer invalidation still apply.

Checked loop widening applies the existing zero threshold to constant bounds
as well as differences between two variables. A growing positive upper bound
becomes unbounded; a growing nonpositive upper bound first weakens to zero.
The symmetric rule applies to decreasing lower bounds. Unchanged bounds and
acyclic joins retain their ordinary precision. This weakening contains every
incoming value and prevents an unbounded sequence of constant thresholds;
it introduces no assumed invariant and changes no work limit.
At a widening join, a current cursor's unchanged accessible endpoint or current
termination-witness endpoint may be retained as a canonical bound only when
both incoming states independently prove it. Recheck the represented cursor
after the join. This preserves a common endpoint when one edge has a tighter
temporary bound; a larger step that exceeds that endpoint fails the same proof.
The same join may preserve canonical non-strict and strict orders between two
current numeric cursor coordinates. Nominate only differences zero and minus
one and prove the chosen inequality on both incoming states before the join.
Restore it only while both represented cursors survive. This prevents widening
an exact initial distance from erasing a loop body's already-proved strict
guard. It neither assumes that guard at the loop header nor permits a larger
advance: a returning edge that violates the candidate removes it. Candidate
enumeration and difference queries retain their existing bounded limits.

A helper receiving separate byte-pointer endpoints may require an explicit
`initialized-span` input. Its `path` is the first pointer and `other` is the
exclusive endpoint. Both must be live positions in the same byte array,
`first <= last`, their byte distance must be at most the positive constant in
`end` (the helper target's `PTRDIFF_MAX`), and every byte in `[first,last)` must
be initialized and accessible. `begin` is zero and `family` is empty. This is
an input requirement only; it grants neither write permission nor ownership.
Empty spans still require valid same-array positions. All ordinary path/global
remapping and strict decoding rules apply.

Nominate a span only from evaluated subtraction of two distinct byte-pointer
parameters, allowing a bounded chain of unchanged local copies. A parameter
may belong to at most one nominated pair; ambiguous candidates are declined.
Both parameters and the local-copy chain must be unchanged and not
address-taken, volatile or atomic. Enumeration retains the existing 65,536
syntax-node limit and at most 16 disjoint endpoint pairs.
Nomination installs the sufficient requirement once at function entry, with a
stable internal distance coordinate bounded by zero and target `PTRDIFF_MAX`.
It does not recover storage after mutation, replacement, release or an unknown
call. The endpoint bounds permit ordinary pointer-difference guards to refine
the distance, and accesses still require their actual intervals to fit. Callers
must discharge every part from actual storage evidence; pointer ordering, a
may-alias edge or a callback signature alone cannot discharge the requirement.
Forwarding is permitted only when an already established span covers the
actual argument interval; unknown endpoints remain unresolved.
An established span's two read-only endpoints do not require mutual separation
merely because a different parameter is written. Exclude that pair from the
generic separation premises only when neither endpoint's may-effects write,
consume or escape its referent. Preserve all other separation premises. The
span does not grant an ownership share or justify overlapping output writes.

A complete integer-returning span helper may establish `count-within-span`:
its returned C integer is nonnegative and no greater than the byte distance
between the unchanged entry endpoints named by `path` and `other`. The record
requires a matching unconditional `initialized-span` input, has zero `begin`
and `end`, empty `family`, and no guard, outcome or non-null flag. It is an
output only. Prove it independently at every reachable return and intersect
the outputs normally; an invalid or unrepresented result establishes nothing.
This bounds a count only, without asserting that bytes were processed, written
or consumed. Capture caller endpoint coordinates before applying call effects,
and relate the actual captured integer result to their nonwrapping byte
difference. Released storage and changed pointer identities gain no memory
capability from this numeric fact. Checked decoding rejects a missing span
premise or any noncanonical reserved field; strict remapping must retain both
endpoints and the result relationship.
For a represented nonnegative byte step bounded by `last - current`, an
actual same-value relation between the captured and current coordinates may
prove `current + step <= last`. Constant endpoints are permitted. First prove
`current <= last` so unsigned subtraction has its mathematical meaning; retain
the actual C types and conversions. This is a bounded implication from the
captured count contract, not an assumption about arbitrary returned counts.
Before overwriting a cursor coordinate, retire expressions that depend on its
old value. Never equate the new coordinate to an expression that now refers to
itself. A separately proved bound within a stable extent may be retained on
the new coordinate, including the initial edge, so ordinary loop joins can
preserve it. Mutation and missing count or extent evidence lose the bound.
When installing a cursor that is already proved no later than an initialized
terminator, retain that actual stronger upper bound separately from the
allocation extent. The terminator must refer to the same unchanged storage,
and its coordinate must not depend on the overwritten cursor. The accessible
extent still permits forming one-past pointers, but that weaker bound must not
replace an independently established bound used by a terminated scan.

A constant positive cursor step may use an already proved transitive constant
upper bound to establish that its unsigned coordinate addition cannot wrap.
The bound must fit the coordinate type after adding the actual step; a type
maximum or a missing range on a neighboring cursor supplies no substitute.
This preserves ordinary difference relations when scalar widening has retained
the relational bound but generalized that neighbor's standalone range.

A constant cursor step may also retain an independently proved bound on its
actual updated coordinate against its unchanged physical extent. Prove that
bound before replacing the old coordinate, then carry it through the ordinary
update and joins. This canonical bound survives joins between different
proved step counts; it is not inferred from the mere existence of an extent.
An out-of-bounds step supplies no such fact, and an extent depending on the
updated coordinate cannot be reused as an unchanged bound.

An evaluated comparison of a byte-pointer distance from an unchanged input
base against an unchanged unsigned input count may nominate an initialized
entry interval. Require a unique base/count pairing, target byte-pointer
compatibility and bounded syntax discovery. Publish explicit live-storage,
extent and initialized-byte requirements for `[0, count)`, plus the count's
`ptrdiff_t` representability bound. Actual callers must establish every premise.
The comparison alone establishes none of them, and no cursor provenance is
inferred from the nomination. Materialize the interval once at entry; ordinary
writes, replacement, release and joins still invalidate dependent facts.
Changed or exposed base/count cells and ambiguous pairings decline nomination.

Represented local loop counters assigned the same exact C integer value may
seed a mathematical equality when their integer types agree. Nominate only a
bounded set of counter cells; nomination itself supplies no equality. Ordinary
assignment invalidation, proved nonwrapping adjustments and CFG joins must
preserve or discard that relation on each actual path. Similar increment syntax
alone does not prove counters equal, nor does it establish an exit count.
An actual exact assignment to such a counter may also seed matching lower and
upper constant bounds. This preserves the assigned value's numeric evidence
when a scalar join generalizes two distinct constants to an outcome class;
the ordinary relation updates and loop widening still apply.
The same bounded nomination may include a directly returned ordinary local
integer cell. For an established entry span, an actual exact nonnegative count
and a proved lower bound on the span distance may seed `count <= distance`.
Reconsider this implication after assignments and pointer-difference guards,
so either ordering of assignment and guard has the same evidence. Every join
must retain the relation on all alternatives and every later mutation must
update or invalidate it. A returned variable name alone supplies no bound.
Range queries may follow one counter equality to the other counter's represented
bounds, using a fixed one-hop query depth. This preserves nonwrapping adjustment
proofs such as `count == index < limit` without iterating a new relational
closure or treating modular equality as a mathematical offset.
The same numeric relation may connect represented byte coordinates in different
objects. Nominate only current cursor coordinates and independently prove each
candidate inequality on every incoming CFG edge before retaining it at a join.
The inequality may carry a finite constant displacement derived from each
edge's bounded difference constraints; retain the larger of the two proved
upper bounds, weakened to zero when both are negative. Avoid introducing a
temporary negative displacement merely because the initial loop iteration is
exact; ordinary guard refinements remain responsible for strict inequalities.
This preserves a helper's temporary output lead before the
input advance. Ordinary loop widening still removes an increasing bound;
neither a saved call input nor a candidate displacement is an assumption.
Two cursors initialized at offset zero may then retain equal offsets through
matched nonwrapping advances. This supplies no shared-object provenance and
never licenses pointer subtraction or ordering across distinct objects. An
initial displacement, unequal advance, changed backing, or missing coordinate
must still satisfy the ordinary per-edge proof and position-join rules.
Before an exact assignment resets a nominated counter, an unchanged nominated
counter related by a proved equality may retain its current target-typed range.
Compute that range from the pre-write state with the same one-hop query bound,
then retire the equality normally. Do not retain facts in any possibly written
alias cell. This preserves evidence about an unchanged count when an index is
reused; it supplies no equality to the reset value and no range on a later write.

A sufficient access interval may use a constant upper envelope from the
current scalar range of its endpoint, when its upper bound is below the
declared type maximum (or the actual value is an exact constant) and its mathematical scaling and
displacement fit. Require a nonnegative access start as for other widened
interval requirements. This projects an actual value fact from initialization,
control flow and joins; a type maximum alone does not nominate a traversal
capacity. It supplies only a caller requirement for bounds, initialization or
writability and never a must-written interval or an output count.
The same rule applies to internal cursor coordinates with a represented target
integer range. Their synthetic identity does not require a C declaration;
the captured type and narrowed range remain mandatory evidence.
A proved comparison against a nonwrapping unsigned remaining length may refine
its byte endpoints even when one endpoint is an exact representable constant.
Prove the endpoints' order before using the comparison; the comparison cannot
justify its own no-wrap premise. Use the compared value's actual target range
and checked mathematical displacements. An endpoint conversion may be removed
only when it preserves every value of the operand's established target range.
When capturing a span endpoint held in an ordinary integer cell, retain that
cell's actual call-entry value and validated type. Its earlier initializer,
including a lossy conversion, need not be substituted for the stored value.
This retains a verified span count
when a successful helper result becomes constant, without granting storage,
initialization, or a larger step than the helper actually proves.

The normal exit of a reverse unit-stride byte-writing loop may establish its
visited suffix. Require an ordinary local integer index initialized from a
stable represented nonnegative value, the strict test `index > 0`, a unit
decrement, and exactly one unconditional byte store to `base[index]` per
iteration. The resulting initialized interval is `[1, initial_index + 1)`;
index zero remains uninitialized unless separately written. Other assignments
may update unrelated ordinary local scalars, but cannot change the index,
initial-bound inputs or any pointer. Reject branches, early exits, nested
loops, calls, labels, volatile/atomic storage and exhausted syntax bounds.
Every actual write still needs ordinary bounds and write permission. A base
loaded from a pointer slot additionally requires separation of the slot from
the written bytes, so byte stores cannot change the next iteration's base.
No sufficient interval requirement alone supplies this must-write guarantee.

A complete helper may establish `initialized-advance` for an output pointer
slot. `other` names the unchanged entry pointer, and `path` names its final
position. Every byte from that entry position to the actual final position is
initialized. Require a matching unconditional `position` output with the same
paths and an outcome that covers this guarantee. The final displacement must
be nonnegative and the entire prefix must be initialized at every applicable
return. The record has zero `begin` and `end`, empty `family`, no input guard
or non-null flag, and may have a return outcome. The initial representation
covers non-result output slots only. It is an output-only record and conveys
neither ownership, write permission, termination nor an exact produced count.
Capture the entry storage and coordinate before effects, then use the actual
installed final coordinate in that same storage. Never initialize the whole
upper envelope of possible positions. Missing position evidence, changed
storage, a missing outcome or unproved ordering discards the dependent fact.
Strict decoding and remapping preserve both paths and the matching position.
When every represented return outcome establishes a constant position interval
for the same output and entry paths, their enclosing interval may be published
unconditionally. Each outcome contributes its proved interval; an absent,
guarded or nonconstant alternative declines this generalization. This weakens
the displacement bounds only. It neither combines different storage identities
nor makes outcome-specific initialized bytes unconditional.
An actual target-typed lower bound on a cursor coordinate may similarly narrow
the exported interval's first endpoint, with checked mathematical scaling.
When an unconditional position supplies the final coordinate, a guarded
initialized-advance output may retain its return-outcome guard on that exact
interval. Testing the actual result then activates the initialization through
the existing guarded-range mechanism; changing or ignoring the result does not.
When an unconditional installed position has a stable internal coordinate,
constant outcome-specific bounds may refine that coordinate through the existing
pending-outcome integer facts. The actual result test must select the outcome;
different storage, missing unconditional positions or unrepresented bounds
supply no refinement. Pointer mutation retires the coordinate's pending facts
before a new coordinate value is installed. These are local call-frame facts,
not new portable arithmetic assumptions.

The existing `terminated-within` interval may also be an explicit input
requirement. It requires live accessible bytes, an initialized prefix starting
at `begin`, and an actual zero somewhere before the exclusive `end` bound.
It does not require every byte up to `end` to be initialized. Both endpoints
refer to immutable entry values. The reserved `other` and `family` fields stay
empty/default, and input requirements have no return-outcome or result guards.
A caller must prove a witness inside that interval or a stronger established
bounded-termination fact. An ordinary unbounded string premise cannot satisfy
this requirement merely because a buffer also has a capacity field.

A modeled `strlen` may use such an established bounded prefix. Its returned
length is the distance from the actual starting pointer to the first zero,
not necessarily to an arbitrary previously known zero. Introduce a bounded
flow-local first-zero identity between the start and a proved zero or exclusive
upper bound. The model establishes initialization through that first zero and
an actual zero there. A buffer cursor update can then reestablish its initialized
prefix and termination from those facts. When a generic buffer helper needs
this relationship, it may export the bounded-termination premise using the
unchanged backing, entry cursor and capacity. Unknown writes and pointer
replacement retire the dependent witnesses. No capacity spelling supplies a
terminator, and no uninitialized tail is silently initialized.

The same first-zero construction applies to an ordinary modeled `strlen`
with an explicit initialized-termination input premise. Materialize that
entry witness only for the unchanged entry storage, recording the actual
guarded premise at the call. It supplies a readable prefix through its zero,
not an exact first-zero location. The measured first zero is independently
bounded by that witness; copying `strlen(input) + 1` may then use the proved
prefix. Unknown writes, replacement and release must still invalidate it;
an extra byte beyond the first zero receives no access permission.

Target-unsigned expressions may normalize `a + (b - a)` to `b` when all
operands have the same target type and the removed evaluations are total.
This is a C modular identity, not an assumption that an intermediate operation
did not wrap. Invalid subexpressions, incompatible conversions and signed
operations retain their ordinary evaluation. Bounds on the final cursor still
need independent evidence about `b`.

A side-effect-free conditional integer argument may use its actual target-typed
range as a captured call-entry value when its expression is not representable.
Both possible arms contribute after the actual argument conversions. The
captured identity serves conditional requirements and interval endpoints;
use the range as bounds, never substitute its maximum as an exact value.
The callee's guarded premise is assumed only while checking that implication.
Retire the preceding invocation's range and expression before recapture, and
preserve ordinary unknown results when neither expression nor range is valid.
This local fallback adds no portable arithmetic operation or inferred output.

Capture all required inputs before effects. Invalidate facts affected by writes
and replacement, then install only independently verified outputs. Project
conditional release and reassigned parameters using their incoming identity;
`free(p); p = 0;` still consumes the entry allocation. A new allocation does
not discharge an old allocation's cleanup obligation or revive an alias.

A side-effect-free pointer conditional return may describe each of its two
result alternatives under the condition's established true or false facts.
Reuse the ordinary branch refinement and output intersection rules; an
unknown alternative must still contribute its actual outputs. The initial
rule covers a single conditional with nonconditional arms. Side effects,
volatile accesses and nested conditionals retain conservative output handling.
This recovers the same pointer-position result as equivalent explicit return
statements without reevaluating a state-changing condition.

Scalar must-values distinguish the actual cells reached by pointers. A may-alias
edge or an element-summary overlap authorizes invalidation only; installing a
value needs a definite same-cell identity. Proved disjoint byte intervals may
retain their values. Advancing a pointer retires its old pointee values without
changing values under aliases that still name their original cells. Apply the
same rule to direct stores and numeric outputs from verified calls. These rules
correct stale-value proofs under RFCs 0017 and 0021; they introduce no stronger
pointer provenance or type-punning permission.
A copied pointer may mirror pointee facts only for an exact zero displacement
or a represented field projection. Nonzero element offsets and unknown offsets
retain their ordinary alias effects but do not copy scalar values, nullness,
pointee ownership, spatial facts or nested equalities into a different cell.
This applies to initial pointer assignments as well as subsequent advances.

An established scalar fact in this invocation's live concrete automatic storage
or independently fresh allocation may refute an indirect-read branch, provided
the current access is represented, in bounds and initialized. This does not
authorize branch pruning from unrelated generic input pointers. Unknown writes,
escapes, invalidated pointers and ambiguous cells retain their ordinary losses
of evidence. Numeric output projection must preserve the actual argument's
offset; an unrepresented interior argument cannot overwrite a base-cell value.
Concrete automatic integer arrays whose constant element count fits the existing
cell bound may use the existing exact scalar-cell selectors and initialization
rules. Unknown or overlapping indices invalidate affected values; byte writes
and callees still invalidate every possibly changed cell. This bounded layout
support does not unroll runtime loops or establish a fact about an arbitrary
element. Larger or unrepresented arrays keep their existing conservative route.

Arithmetic retains target-width conversions and overflow checks. A relation
that only holds for mathematical addition cannot justify wrapping C addition.
Guarded floating-to-integer conversions encountered in numeric workflow code
require a separately established finite representable range; unsupported
floating predicates stay incomplete. This is not a full floating-point
functional-correctness analysis.

The initial floating conversion proof is local and target-aware. A constant
operand must be finite and convertible to the actual target integer type with
rounding toward zero. For a scalar local or parameter, lexically dominating
true comparisons may establish finite lower and upper endpoints (or equality
to a finite constant). Conjunctions contribute both bounds; disjunctions and
false floating comparisons do not, since unordered NaN alternatives remain
possible. The scalar must be unchanged and have no taken address anywhere in
the function. Reject intervening floating conversions, volatile/atomic storage,
nonstandard floating modes and exhausted syntax bounds. LLVM's target floating
semantics and integer width validate each endpoint; host floating arithmetic
must not decide representability. No numerical return relation is inferred
merely because a conversion is safe.

A local initialized byte interval may additionally prove that every byte is
one of the target execution characters `0` through `9`, `+`, `-`, `.`, `e`,
`E`, or the zero terminator. This is a content must-fact, independent of plain
initialization. It follows actual constant arrays, narrowed one-byte stores
and byte-preserving library copies. Joins intersect evidence; an overlapping
write removes the content fact unless the new bytes independently satisfy it.
A summary that promises only initialized output does not preserve contents.
The fact remains local and introduces no portable record or caller assumption.

A bounded unit-stride reader scan may establish this content fact for its
already visited prefix. The existing zero-start, stable-reader and nonwrapping
index proof must hold. The loop body must enter a switch on the current byte;
every non-default label must be a numeric-alphabet constant and the default
must exit the loop before its increment. No memory writes, calls, index writes,
volatile accesses or alternate entries may change the scanned bytes or bypass
the test. Short-circuit condition operands retain the non-strict visited-prefix
invariant before their test; the strict current-index bound belongs only to
the body after the complete condition succeeds. A condition operand alone
does not prove that the complete condition holds. The default exit certifies
only bytes before the rejected current byte. The prefix remains attached to
the actual storage and offset; its end
uses the actual visited index, not an unrelated count or capacity. Later exact
counter equality may project that end through a byte copy. A same-storage
one-byte store of independently numeric text preserves the existing numeric
alphabet throughout its old interval, but does not preserve zeros or initialize
new bytes outside the proved write. Unknown replacement bytes lose this fact.

For a live initialized terminated interval with this numeric alphabet, the
modeled numeric conversion boundary may exclude NaN while retaining infinity,
underflow and failed conversion. This uses the standard subject-sequence and
zero-on-no-conversion behavior, not successful parsing or finite output (see
[WG14's subject-sequence correction](https://www.open-std.org/jtc1/sc22/wg14/issues/c99/issue0225.html)).
The scalar fact is invalidated by writes and unknown effects and intersected
at joins; it is never inferred merely from the function name or a clamp.
When NaN is independently excluded at a floating-to-integer conversion,
dominating false comparisons may supply the complementary finite bounds.
Each bound still needs unchanged storage between its test and the conversion,
no alternate entry into the branch, standard target floating semantics, and
representable target integer endpoints. A preceding scalar assignment outside
those branches does not itself invalidate a later dominating bound. No floating
arithmetic result range or NaN exclusion is inferred through unsupported
operations.

A preceding `if` whose true arm immediately returns and has no else arm may
supply its false comparison to a later conversion in the same compound block.
For this lexical early-return proof, require the floating scalar to be
unchanged and unexposed throughout the function and reject jumps, labels,
switches and assembly anywhere in the function. Only a direct return or a
compound containing exactly one return terminates the nominated arm. Thus
neither a mutation between guards nor an alternate entry can bypass the bound.
False comparisons still require independent exclusion of NaN.

Bounded caller cases may carry an independently proved non-NaN property for
an ordinary by-value real-floating parameter. Capture the actual converted
argument at the call, and validate the parameter's floating type on entry.
Use the existing fact budget; encode a distinct `n` record for a root parameter
path, reject duplicate or conflicting pointer/integer premises, and retain it
in strict remapping and cache identity. It implies neither finiteness nor an
exact value. Assignment, address exposure, unsupported arithmetic and joins
retain the local non-NaN domain's existing invalidation rules. A callee may
forward the property only while it still holds; NaN and unknown alternatives
cannot satisfy it. Generic definitions remain incomplete when they require
this unproved floating premise.
Compiler builtins that the target evaluator proves to be floating constant
expressions without side effects need no external call contract. Their actual
constant may be infinite or NaN; constant evaluation alone does not supply a
finite conversion range or exclude NaN.

The conversion routines' optional end-pointer store is guarded by the actual
output argument being non-null. A refuted store guard must also suppress that
store's source-escape effect, as required by RFC 0009. Passing null does not
export the input allocation through a nonexistent output slot; other stores,
unknown guards and escaping destinations keep their ordinary effects.

The existing modeled C-library boundary may cover `strtod`, `strtof` and
`strtold` memory behavior. They require a live initialized terminated input.
A non-null end-pointer argument needs a writable pointer slot separated from
the input. The stored end pointer belongs to that same input object, between
its start and an established terminator, inclusive. A null slot is permitted.
Validate the actual C signature and library provenance; a user definition or
incompatible prototype receives no library evidence. This model supplies no
numerical return value, finite-range guarantee, or exclusion of NaN/infinity.

A complete callee's copied-pointer store into a confined automatic pointer
slot does not itself escape the source allocation. This narrows RFC 0007's
conservative rule that every summary copy store escapes its source. The actual
output argument must directly address that local slot, with no other address
use in the function's bounded syntax scan; the callee may neither retain nor
consume the slot address. A modeled library contract or complete inferred
contract supplies the effects. Preserve ordinary may-aliases, release-family
and interior-offset restrictions, and subsequent mutation/unknown-call
invalidation. Other stores and escaping effects still retire ownership.
Unresolved, indirect, global, caller-owned and unconfined output destinations
retain the conservative escape rule. A local alias does not discharge cleanup:
losing all local holders without release still reports the allocation.

For the supported Clang target, a pointer cell may be accessed through another
character-pointer type when the pointer representations have matching size,
alignment and address space. This narrow rule includes the `char **` view of
an `unsigned char *` end-pointer slot. Clang's
[pointer alias implementation](https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/CodeGenTBAA.cpp)
uses the same character base type for these views. It does not generalize to
unrelated pointee types or additional pointer nesting. Volatile/atomic views
remain unsupported; ordinary slot validity, initialization and write permission
still apply, including the original object's const qualification. A view
supplies neither pointee storage nor a numerical result from a parsing call.

### 3. Recursive contract groups

Use the existing call-graph strongly connected components to organize candidate
checked contracts. Candidates describe sufficient entry predicates and proposed
memory/ownership outputs, separately from ordinary recursive may-effects.
Do not install candidates as ordinary completed summaries or cached results.

Check every body under the group's proposed interfaces. A recursive edge may
use a candidate only with established input premises and a supported progress
relation: a proper child of a finite ownership forest, or a strictly smaller
nonnegative remaining initialized input interval. Non-strict forwarding edges
are permitted only when every cycle contains a proved strict edge; removing
strict edges must leave an acyclic graph. Neither a syntactic recursive call
nor a scalar decrement without an established lower bound supplies progress.
The actual pointer must identify the exact node. Ownership identity resolution
that strips arithmetic cannot establish this premise; nonzero, unknown or
interior offsets do not become proper children or unchanged forwarding edges.

Verify all base cases, local operations, returning outcomes and promised
outputs. Construction additionally accounts for newly acquired allocations and
all failure exits. Cleanup must conserve and consume the complete relevant
footprint. Traversal/serialization must preserve borrowed input ownership and
prove the output storage and initialized prefix. Calls on unrelated or
unproved child objects cannot use the induction hypothesis.

Allocation accounting also covers ordinary byte allocations in functions that
handle forests. Returning a live free-compatible allocation base may transfer
that single allocation's proved footprint. This supplies no forest predicate,
child ownership or initialized contents. Returning an interior pointer, a stale
alias, null, or only the head of a larger owned forest does not discharge the
remaining allocations.
The same single-allocation transfer applies when a function directly returns
the fresh free-compatible result of a complete callee without a local holder.
The exit ledger must already relate this invocation's acquisitions to that one
returned allocation identity at offset zero. Its null alternative contributes
the empty footprint, and its non-null alternative transfers that allocation;
an opaque, borrowed, offset or non-free result transfers nothing.
A modeled nullable release may settle a represented ordinary allocation head
without first splitting the null and non-null paths. The current pointer must
be known null-or-live, unescaped, unreplaced and at the allocation base, and
its acquisition guard must hold whenever the pointer is non-null. The null
alternative contributes the empty footprint. This accounting supplies no new
release permission and cannot settle a stale or interior pointer.
A complete helper's unconditional free-family `allocation-consumed` output
settles the same represented allocation head under the same conditions. The
helper's own release requirement is still discharged at the call; a guarded
or outcome-specific consumption output settles nothing here.
Publishing an ordinary payload into an established container may use a local
alias of a fresh allocation, including a proved allocating helper's result.
Allocation conservation through a visible output must survive summary joining.
For each returning path that needs an output to settle a local acquisition,
retain the bounded alternatives of output paths that account for its ledger.
After joining all returns, require at least one complete alternative to retain
portable footprint guarantees applicable to that outcome. A structural fact
alone is insufficient. Fresh output fields must use a supported caller-side
acquisition carrier (the existing single success-published, null-on-failure
slot); a fresh result or verified in-place extension uses its existing carrier.
Losing a path-specific fresh output at a void-return join leaves conservation
incomplete, even if each return separately exposed some reachable allocation.
Payload fields may be nominated from actual local release operations or from
imported container descriptors on functions taking the same record. Revalidate
the local field type and layout; nomination alone establishes no ownership,
initialization, nullness or release permission. Imported nominations retain
their summary dependencies and are invalidated with the database generation.
Import the corresponding conditional-ownership selectors as nominations too,
checking their descriptors against the local target layout. Conflicting local
and imported selector candidates cannot establish an ownership condition.
Before capturing a call case, an independently proved concrete container may
supply its actual empty links and payload slots. Capture those null values
under the existing context bounds. A specialized input predicate may retain
them as explicit premises. Publishing a new owned payload may strengthen an
unchanged input predicate to require release ownership; this is an additional
caller obligation, never ownership inferred from a write or pointer type.
A context that establishes every recursive link and owned payload slot null
may nominate that complete singleton input for a forwarding helper without
direct release operations too. Cleanup retains its general owned input. Keep
it as an explicit input descriptor, so a verified extension can cross helper
boundaries without treating the incoming head as newly allocated. Partial
empty-slot information does not nominate an unchanged singleton footprint.
Such a complete singleton consists of exactly its head object. Two incoming
singleton heads related by an explicit distinct-object case premise therefore
have separated container footprints at entry. A head with a possibly non-null
link or owned payload keeps the ordinary explicit container-separation
premise; pointer inequality or different parameter names prove nothing here.
A helper with a store into a nominated recursive link may likewise nominate
release ownership when its caller case establishes the complete singleton
input. Require that owned predicate on every return, including allocation
failure, before using it in extension conservation. The generic non-singleton
contract keeps its existing access requirement; no pointer write establishes
ownership of a caller object.
A helper that stores a possibly non-null pointer into a nominated owned payload
may nominate that release-capable input for its whole body. Its failure paths
then preserve the same explicitly required ownership as its success paths.
Install this nominated premise before checking returning outcomes; CFG block
visitation order must not determine whether an early return retains it.
Current ordinary null facts may refine an established live head's empty-link
and empty-payload descriptors when discharging a call requirement. They must
refer to the exact current cells; a stale null fact or an invalidated head
cannot supply this refinement.
Require the actual live non-null allocation base, compatible release family,
an unescaped current acquisition, its represented head footprint, and separation
from every allocation already in the established container. Preserve the full
acquisition ledger; a copied field name alone supplies no ownership. Interior,
released, merely borrowed, duplicated, escaped or unaccounted allocations do not
fold into the parent. Failure paths must release or retain every acquisition.
A payload relocation may temporarily leave two slots naming one allocation.
Such a state supplies no complete forest. After clearing the old slot, a
concrete graph may be established again only by checking every current link,
payload, live allocation base and ownership condition independently. Retain the
original acquisition ledger throughout; re-establishing structure cannot erase
an overwritten allocation or excuse an uncleared duplicate or stale pointer.
For two adjacent pure stores in one CFG block, an already established owned
forest can also prove `head->destination = head->source; head->source = 0`.
Both accesses must use the same unchanged pointer variable and compatible
payload field types and release families. Require an actually empty destination,
active ownership for both slots, and a live complete input forest. Snapshot
the whole footprint and source payload before the copy; after the clear, the
same allocations occur exactly once under the destination slot. Retain the
original acquisition ledger and only unchanged definite aliases and separation
facts. The intermediate duplicated state establishes no forest. Any intervening
operation, control-flow edge, non-null clear, changed holder, unknown ownership
condition or failed ordinary memory obligation prevents this transfer. The
snapshot is local to that block execution and never survives a join or call.

Modeled C-library byte/string comparisons preserve established container
predicates because their represented behavior only reads memory. Require a
recognized builtin comparison and a compatible C signature; all ordinary
argument validity, bounds and initialization obligations still apply. An
unavailable function, body-defined replacement or incompatible signature has
no such frame, and a failed read cannot make the selected contract complete.
The existing modeled floating parser also preserves an incoming forest when
its end-pointer output is null, or writes only to an actual automatic pointer
cell separate from that forest. Resolve the output object's identity and apply
the existing local-write frame; a cell within the forest, an unknown output or
an attached local member supplies no frame. Retain every input-string, output
bounds, writability and separation obligation. The modeled scalar math family
does not mutate a forest either; any floating result still obeys its existing
NaN, infinity and conversion rules.
A modeled release of a fresh local allocation may preserve an unchanged
incoming forest whose every represented member still has entry provenance.
Capture this frame before release invalidates the allocation's pointer. Require
an unescaped live allocation base with the matching release family and actual
local allocation identity; a borrowed or interior pointer supplies no frame.
Retire any forest containing local or unrepresented members, and retain the
ordinary release checks and complete acquisition ledger. This rule cannot
restore the released allocation, an attached payload or a stale alias.

For a modeled positive-size reallocation, snapshot the old allocation's head
footprint before effects. A successful result acquires its distinct new
allocation and releases precisely that old head; a null result acquires
nothing and leaves the old allocation outstanding. Retain the conditional
release in the flow state until the result is established non-null. Joins keep
only identical pending evidence, and replacing its result holder or losing its
storage identity retires it. This accounting does not restore old aliases,
transfer owned children, or prove a size, release permission or library call.
Use the existing footprint bounds and keep unresolved outcomes conservative.

Publish the group's contracts only after every participating candidate and
every recursive edge verifies. Failed or exhausted candidates retain explicit
unresolved obligations and publish no dependent outputs. Dependency changes
invalidate the entire affected proof group. Verified recursive contracts then
compose through the existing summary, callback and cross-unit machinery.

An independently rechecked input case retains its own exhaustion state. A
generic recursive summary's fixed-point limit is not an executed operation in
a case whose CFG proves the recursive branch unreachable. Such a case may
complete only from its own checked operations and complete callees; using an
exhausted generic approximation, reaching an unavailable recursive case, or
exhausting the case's own budget still leaves it incomplete. Selecting the
generic definition continues to report its original limit. This follows the
case-local proof rule of RFC 0025 and does not raise any analysis budget.
An executed edge within the syntactic recursive group may use a separately
completed input case whose actual path reaches a base case. Merely calling a
group member does not prove a cycle occurred. Falling back to the group's
generic approximation still retains the exhaustion marker, even when an
intermediate optimistic approximation has no local failed obligation.
Case capture may follow an exact pointer to a live initialized scalar object
of the same C type to obtain its current value. Unknown offsets, partial
objects, incompatible views and invalidated storage supply no scalar premise.
Stateful helpers may nominate entry selectors forwarded from their callees as
well as selectors read locally. Mutating other cells does not prohibit entry
case capture; actual writes and aliasing still invalidate dependent facts
during the case's ordinary CFG analysis.

Bounded case nomination may decline an unverified recursive target when its
only scalar refinements concern mutable output cells or non-exact ranges.
An exact scalar or null input not overwritten by the target's may-effects can
nominate a case; pointer alias and ordering cases retain their existing route.
By-value scalar inputs remain eligible even when the private copy changes.
This filter saves context slots for informative input cases and contributes no
proof: admitted cases still check every operation, and declined cases retain
the original contract with its actual completeness. Apply the nomination filter
also during the ordinary fixed point's optimistic initial rounds; otherwise
uninformative requests can consume all slots before iteration settles. A
completed generic contract can be used directly without spending a case slot.
Verified induction groups retain their established contextual route. The filter
cannot evict completed cases, reset a
budget, or increase the existing context count or nesting bounds.
If an attempted input case is incomplete or unavailable, a complete generic
contract may still be used with all of its original caller obligations. No
outputs or diagnostics from the abandoned case are installed, and the generic
contract's stronger input premises must be discharged normally. This fallback
does not complete, cache as complete, or erase the incomplete case itself.
When the same target already has an active input case, a recursive request
whose bindings differ only in facts about overwritten or replaced cells may
also decline nomination. Compare a temporary projection for this scheduling
decision only; admitted cases retain every original premise and guard. The
existing generic contract remains the fallback, with its actual completeness.
No projected context is analyzed or published as if it were the actual input.

During a recursive component's generic may-effect fixed point, postpone
scalar-only checked case requests. Its provisional output state and selector
inventory must not consume the permanent case budget before the component's
effects settle. Actual alias and ordering contexts retain their existing route;
postponed calls keep the generic approximation and explicit incompleteness.
After the component settles, ordinary callers may request their actual cases.
This is a scheduling rule, not a recursive proof or a budget reset.

When that may-effect iteration has actually converged, the existing final body
pass may retain ordinary null/non-null pointer outcome facts and integer return
values re-established by that pass. Earlier less precise approximations are not additional executable
return paths. Check every applicable return against the settled conservative
callee effects, preserve all input requirements and ordinary may-effects, and
replace only these pointer-value outcome maps and the numeric output at the
return-value root. Retain the actual guards and outcome restrictions on each
numeric alternative; an unrepresented alternative remains unknown. Numeric
writes to caller memory keep their existing widening. This can exclude a
spurious negative success result only when every current returning alternative
establishes a nonnegative result. The final pass must run against
the current dependencies rather than reuse a pre-finalization result. An
exhausted component cannot use this refinement. This adds no fixed-point round
or analysis bound. It establishes no checked memory output, recursion progress,
allocation accounting, initialization or complete-call guarantee; those still
require their independent proofs and existing group verification.
When an outcome test selects sign classes, intersect its pending classes with
independently represented facts about that same tested value. Follow RFC 0017's
existing trust boundary for direct call results, private scalars and contextual
values, and prove any intervening conversion preserves the tested relation.
Do not project facts from a stale mutable heap cell or from a lossy conversion.

When an unverified recursive interface has a represented read-only record
input as well as mutable output state, nominate scalar cases from exact facts
about the read-only input. Constants describing only the mutable state cannot
stand in for its missing input selector. Identify read-only inputs from actual
read effects and absence of writes, replacement or consumption under that
parameter; C constness alone is insufficient. Interfaces without such a
read-only record input keep the existing nomination rule. Every admitted case
retains all of its actual premises, including mutable-state facts.

When a direct call supplies both represented callback bindings and a captured
memory/scalar case, check their combined entry context directly. An intermediate
callback-only analysis need not first explore all unknown selector alternatives
and populate unrelated nested cases. Capture the combined premises from the
same pre-call state and generic selector inventory, retain every actual callback
alternative, and use the existing dependency and publication machinery. Calls
without a represented memory case keep callback-only checking. A declined or
incomplete combined case supplies no proof and does not increase either budget.

The initial construction interface takes an immutable byte input and an
unsigned remaining count and returns a nullable fresh initialized forest.
Its explicit sufficient entry premise is live input with the entire remaining
interval initialized. Recursive calls must stay inside that entry interval;
progress compares against an immutable entry count, including when the count
is mutated through an alias. Complete external helper contracts may compose
through ordinary call transfer, including cleanup of a partially constructed
tree. Every helper requirement must follow from the candidate input premise,
every promised output must hold on the corresponding return, and all acquired
allocations must be accounted for on success and failure. The candidate grants
no hidden writes, captures or releases of caller inputs. Private proof members
cannot publish nested callback or memory specializations during verification.
Broader record-reader and output-parameter interfaces remain part of this
milestone's required workflow goals; this initial rule does not replace them.

An output-slot construction interface may instead take the same immutable byte
input and unsigned remaining count followed by a pointer to a node-pointer slot,
and return an integer success indication. Its candidate requires a live writable
slot separated from the input. Positive returns must publish a non-null fresh
initialized forest; zero returns must actually leave the slot null. Negative
returns, unchanged failure slots and other stores are not covered by this
candidate. Validate actual final null facts on every failure exit, not the
ordinary may-store summary's weaker null-or-no-store information. The slot's
previous contents confer no ownership or initialization premise. Recursive
calls use the same interval progress proof, and all allocations remain subject
to the ordinary failure-exit ledger. This adds no portable contract kind.

A copied reader may package the immutable byte input and remaining count in a
record passed by value. Nominate the byte pointer from evaluated byte accesses
and its unsigned remaining counter from decrementing operations, requiring a
unique bounded candidate across the group. Unrelated fields contribute no
premises. The proposed contract still requires the complete initialized entry
interval, and progress uses immutable entry snapshots of both field values.
Local changes to the copied record do not change the caller's record. Every
recursive actual must name a subinterval of the same entry storage with a
nonnegative smaller remaining length; decrement syntax alone proves nothing.
This rule uses existing field interface paths and fresh return contracts.
Mutable reader pointers and partial consumption require their own established
relational outputs; passing a record by value does not certify those interfaces.

An in-place constructor may instead extend a live owned initialized head.
Its initial supported input predicate has no owned successors or payloads:
the existing terminal flag covers every recursive link, and every payload
slot is explicitly null. A private candidate for `(node *, unsigned count)`
requires that predicate and a non-null head. Recursive calls must decrease
the immutable entry count on every group cycle. The head remains the same
live allocation on every return, including allocation failures. Every newly
allocated descendant must be initialized and uniquely attached or released;
all return paths retain the ordinary acquisition/release ledger. Merely
returning success supplies no ownership evidence.

Transport this relation as the output-only `container-extended` record. Its
`path` is the final forest and `other` is the unchanged entry head, with zero
`begin` and `end`, an owned release-capable descriptor in `family`, no
non-null flag, and no outcome or input guard. Require a matching unconditional
owned singleton-head input predicate. It promises that the final footprint
is the disjoint union of that complete entry allocation and a possibly empty
fresh region acquired by this call. It does not call the original head fresh.
Prove actual unchanged live head identity, complete output structure, exclusive
entry provenance and allocation conservation before publishing it. A replaced
or released head, another input region, a borrowed new member, or an unresolved
output provides no such relation. At a complete call, capture the actual entry
footprint before mutation, add the fresh region to the caller's acquisition
ledger once, and establish the final union on every returning outcome.
Do not settle ownership merely from the callee's may-store effects. Preserve
this record through strict remapping, joins, reports, sidecars and checkpoints;
missing singleton premises or incompatible layouts invalidate the record.
If the same applicable output also preserves exactly its input footprint,
its extension region is empty. Prefer that preservation relation when applying
the call, rather than introducing an unrelated fresh region for the redundant
extension guarantee.
For this initial encoding, both paths name the same root pointer parameter.
The unchanged head retains its live storage identity through the call. A
forest proved separate from that head before the call remains separate from
its extension only if its own complete fact survives the call unchanged:
the added region is fresh, and the original head was already separate.
Invalidated or replaced neighboring facts supply no such frame.
A verified derived output has the same separation frame when every named
incoming region is separately proved disjoint from the neighboring forest.
Capture those facts before the call and require the complete neighbor fact to
survive unchanged. All other members of a derived output are fresh to this
invocation; an ordinary structural output without verified provenance supplies
no frame. Checking just one of several incoming regions is insufficient.
A verified fresh output is disjoint from each live forest established before
the invocation whose complete fact survives the call unchanged. This remains
true when a loop revisits the same allocation site; a site identifier alone
is neither a new allocation identity nor evidence of aliasing with the older
instance. Capture the surviving candidates before applying any output, exclude
every destination of this invocation's outputs, and exclude definite aliases
of the newly installed result. Multiple fresh outputs from one invocation do
not become mutually disjoint without an independent separation guarantee.
Replacing a temporary pointer may retire ancestor or tail names from a
surviving forest without changing the forest itself. Such loss of relational
hints may be ignored when comparing these snapshots; require identical shape,
membership, incoming provenance, allocation capability and release state.
Do not ignore a changed descriptor, region membership or any newly introduced
relation to another forest.
The extension descriptor may retain head selectors and empty links or payloads
proved on every applicable return. Joining extensions of the same unchanged
head intersects those facts using the container must-domain; incompatible
layouts or capabilities lose the relation. Every outcome must still prove the
extension, even when their stronger structural descriptors differ. A caller
therefore retains a slot known empty on both success and allocation failure,
without assuming that a possibly populated neighboring slot is empty.
An attaching helper may also publish a payload acquired by the same call.
`container-combined` may therefore carry the constant `end` value one. The
final forest at `path` is then the disjoint union of the two complete entry
footprints named by `other` and `begin` and a possibly empty fresh region
acquired by this invocation. Both inputs need owned release-capable
predicates, an explicit container-separation premise and a live `other` head
premise. The output head must be that unchanged live entry head, every other
member must come from one of the two inputs or from this invocation, and the
exit ledger must equal exactly that union. At a complete call, capture both
entry footprints before effects and add the fresh region to the caller's
acquisition ledger once. The region is empty on every outcome the output does
not cover, so an established other outcome constrains it to the empty
footprint; an untested result leaves the caller's ledger unsettled. The value
zero keeps its exact two-input meaning and every other `end` value is
rejected. Joins, remapping, reports, sidecars and checkpoints keep the field.

A function whose return expression is exactly a complete call's own integer
result may likewise install that call's outcome-specific structural and
footprint outputs while checking each of its own returning outcomes. Require
the returned value to be that call's actual result identity, with no
intervening evaluation, and check every ordinary obligation of the outcome
under those installed outputs. This forwards an already verified guarantee;
it neither assumes an outcome nor republishes consumption or effects.

When a more specific verified output then states that the same path's final
footprint is exactly its incoming regions, the call's fresh region for that
path is empty. Both are outputs of the same checked contract, so retiring the
region keeps the caller's acquisition ledger consistent; it establishes no
allocation and never revives one the callee actually released.

A verified ownership transfer of a complete call may be installed on the CFG
edge of an immediate test of that call's own result, as for the output-slot
constructor. The call must be the tested expression with no intervening
evaluation, and the selected outcome must imply the transfer's outcome. Only
that transfer's own structural outputs, on its path and the paths below it,
accompany it: replaying an unrelated outcome-specific output could replace
current caller evidence with a weaker captured entry description. An output already installed at the call because its outcome was then
established is not installed twice; unconditional outputs, consumption and
other effects are never replayed. A saved or later-tested result still needs
separately invalidated pending evidence.

A recursive extension candidate accepts an inferred output with the same
paths, guards and footprint relation when its structural descriptor entails
the candidate's descriptor. Additional proved head facts do not invalidate
that weaker candidate; incompatible structure supplies no proof.
The same must-join applies to ordinary, fresh and input-derived structural
outputs. Join only matching output paths, capabilities and guards, intersecting
head-selector and empty-slot facts. Derived outputs retain the union of every
required input source under the existing three-source bound. A structural
output common to every returning outcome may be published without an outcome
guard, even when stronger per-outcome descriptors differ. Preserve separate
footprint conservation obligations: structure alone does not account for an
input allocation or a newly acquired region. A missing outcome, changed
layout, incompatible ownership condition or lost source premise drops the
dependent output rather than generalizing it into an ownership proof.
A represented selector store may retain an untouched payload when the prior
head predicate proves the selector bits and the new value selects the same
ownership branch. Require the prior complete payload capability; changing
borrowed storage into owned storage still requires independent evidence.
Across a helper that may write an ownership selector, retire the current
conditional footprint names for every possibly affected forest before applying
its outputs. Immutable entry snapshots retain their old allocations, but stale
conditional contributions cannot reconnect a new structure to an old ledger.
Only separately verified conservation outputs may restore that equality.

A mutable pointer-reader constructor may use the same discovered byte interval
and fresh-forest result without promising any reader outputs. Its additional
entry premises require a live initialized writable record, separated from the
readable bytes. Only the data and remaining-count cells may be changed; those
effects invalidate their caller facts on every returning outcome. Recursive
progress is checked against the original input interval before applying these
effects. Each body must prove its complete fresh output and failure cleanup
without using any unstated post-call cursor or count fact. Clients that need
partial-consumption bounds still require independently established outputs.
An unchanged live entry object is separate from allocations acquired during
this invocation. Represented writes confined to that object may preserve a
forest proved wholly fresh in this invocation; they may not preserve an input
forest, an escaped or replaced entry identity, or facts reached through a
changed pointer cell.

Conversely, a represented write into this invocation's automatic storage
cannot alter a forest whose entire current membership still comes from live
entry witnesses. Preserve only such unchanged entry forests across direct
stores and modeled byte writes to that automatic object. A forest containing
an attached local node, an unknown call region, or a replaced holder receives
no such frame. Unknown pointer destinations and writes through an input root
or any descendant retain ordinary invalidation. The write's own bounds,
initialization and permission obligations remain independent.
That automatic-storage write likewise cannot alter a forest with no incoming
region whose every node was acquired as a free-compatible allocation during
this invocation: an automatic object and a heap allocation are distinct
objects. Holder names reached through the written object are still retired,
and a forest with an incoming region, an attached automatic or static node or
an unknown callee region keeps the rules above. For the same reason an
allocation identity acquired by this invocation stays separated from this
invocation's automatic objects after attachment retires its resource record.
Pointer validity, bounds and initialization remain independent obligations.
The same entry-only frame applies to writes confined to an independently
established fresh allocation from this invocation, and to complete callees
whose represented effects identify only these separate objects. Check every
effect; unknown destinations, consumption and mutation of a potentially
overlapping input still retire the forest. This rule establishes neither
freshness nor callee completeness from an allocation-site spelling.

A forest containing an unchanged singleton entry head and allocations acquired
in this invocation may survive a represented store to a separate unchanged
entry object. Require the singleton input descriptor for every incoming member
and an explicit object-separation premise between each such head and the written
entry object. Every other member must already be proved fresh in this invocation;
an unknown callee region or an incoming descendant supplies no frame. The same
rule applies to a complete helper's represented stores. A cursor that actually
aliases the head or its ownership selector cannot discharge the separation
premise. This rule preserves an existing forest and acquisition ledger; it
establishes no allocation, initialization or ownership by itself.

Verified fresh and derived helper outputs retain their allocation provenance
when composed into that mixed forest. A derived predicate already establishes
that every non-fresh member belongs to one of its explicit incoming regions;
require a separated singleton descriptor for each such region. Synthetic
call-region identifiers need not enumerate fresh descendants. An ordinary
structural output without this provenance, an unrepresented input, or an
incoming non-singleton still receives no frame. Footprint conservation remains
an independent obligation, including failed construction paths.

Bound candidate discovery and validation using the existing function/context
budgets. Deduplicate equivalent candidates and cache immutable preparation.

A byte-interval writer may take an immutable byte pointer, an unsigned remaining
count, and a pointer to a discovered byte-buffer record, returning an integer.
Its private candidate requires the initialized input interval, the buffer's
actual capacity and initialized prefix, writable header and backing storage,
and separation of the input, header and backing. Only the logical-length cell
and backing bytes may change. Every returning outcome must establish the buffer
predicate anew, including failures after a partial write. Input progress uses
the same immutable interval snapshots as construction. A buffer output alone
promises neither that all input was consumed nor a particular output length;
clients must check the established current length before reading the prefix.
Returning a success value without writing bytes cannot authorize an advertised
longer initialized prefix. Stronger serialization length and termination claims
remain separate obligations. This interface reuses the existing buffer and
interval encodings and introduces no trusted recursive body.

The initial cleanup group limit is 32 members and 256 proof edges; the bounded
syntactic eligibility scan visits at most 65,536 statements per member.
Do not increase analysis bounds or use bounded execution as proof of arbitrary
runtime input length. Direct cleanup uses this same machinery once migrated;
remove superseded special-case proof paths after equivalence is tested.

### 4. Synchronous callback requirements

Allow supported generic helpers to export a requirement on the behavior of a
callback input or represented callback field. The initial vocabulary covers
allocation of a requested extent, release of an input allocation with the
matching family, and represented synchronous memory input/output contracts.
Requirements include the relevant argument/result paths, null alternatives,
guards and side effects. A function type alone does not satisfy them.

The initial portable allocation/release demands are input-only checked
requirements `callback-allocate` and `callback-release`. Their `path` identifies
an immutable entry callback, `family` is `free`, and `other`, `begin` and `end`
are the reserved zero/default values. Outcome guards are prohibited; input
guards have their ordinary meaning. The callback itself must be non-null.
Allocation demands a synchronous `void *(size_t)` operation that returns null
or a fresh free-compatible allocation of the requested extent, with no other
observable writes, captures or releases. Release demands a synchronous
`void (void *)` operation accepting null and otherwise consuming precisely the
argument's free-compatible allocation. An unknown entry callback may satisfy
either operation conditionally by exporting that demand. A replaced local,
unknown cast, unresolved global or known incompatible alternative cannot.
Forwarding the demand requires the same immutable-entry evidence.

The initial actual-target matcher accepts the existing modeled malloc/free
boundary and complete inferred wrappers with the same precise interface and
effects. It does not infer compatibility from a spelling, prototype, isolated
fresh return or may-release effect. Every returning alternative and all
preconditions must be covered. An unrecognized wrapper remains incomplete.
More general memory callback contracts continue through the existing explicit
function-type contract and concrete-binding mechanisms until a separately
specified behavioral matcher supports them.

An actual target satisfies a callback requirement only through a complete
verified inferred contract or an existing explicit modeled/trusted boundary.
All possible targets must satisfy the requirement; output guarantees intersect
across returning alternatives. Unknown or null alternatives remain unresolved
unless the actual call path excludes them. Copies, setters and nested hooks
retain their actual identities and dependencies.

This representation does not infer an arbitrary callback's body from its use.
The demanded contract is a visible caller premise, and closed clients must
establish actual bindings. Asynchronous invocation, escaping callback protocols
and arbitrary behavioral subtyping remain outside this change.

### 5. Portable representation and implementation boundaries

Core owns Clang-free value/relational/predicate records and their joins,
validation and canonical encodings. Analysis supplies evaluated C operations,
target types/layout, candidate discovery and body verification. Frontend
transports only completed contracts and explicit incomplete results, and
preserves source/header/command/object binding and dependency invalidation.

Any new portable records must participate in equality, strict path/global
remapping, summary I/O, checked reports, sidecars and persistent checkpoints.
Reject dangling references, missing premises, contradictory capabilities,
noncanonical encodings and oversized records. Retain source-level locations
and proof origins independently of semantic convergence.

Portable storage descriptions must retain the typedef identity of an anonymous
record when Clang uses it in the canonical layout view. Record the typedef name
separately from a tag name; an anonymous record may have one identifier here,
whereas a tagged record must not carry this anonymous-record field. The `it2`
interface encoding adds that bounded identifier and rejects earlier encodings.
Materialize its typedef only inside the analysis adapter, without publishing it
in the client's declarations or lookup tables. Recheck the complete target
layout and canonical view, including qualified and nested references. A missing,
forged or incompatible typedef identity loses the adapter; it never permits
dropping a private global from a dependent contract.

Portable records use summary format 26, sidecar format 27 and checked encoding
12, incrementing the RFC 0028 formats 23/24/9 and the interim development
formats 24/25/10 and 25/26/11. Rebuild older objects and discard old caches. Do
not add compatibility readers. Expanded and compact reports remain current
representations of the same evidence, not different proof modes.

Remove superseded inference and projection paths when the common machinery
covers them. Preserve useful domain-specific transfer rules rather than
duplicating the whole checker behind a new mode. No new compiler option,
pointer ABI, runtime instrumentation or source-language annotation is required.

### 6. Frozen acceptance and validation

Preserve an immutable baseline executable and source revision. Before checker
changes, freeze manifests, expected properties and source hashes for:

1. Stateful reader/writer helpers with extra unrelated fields, copied state,
   helper-mediated cursor updates and success/failure outputs.
2. Direct and mutual recursive traversal, construction and cleanup, with
   runtime-sized inputs and concrete nondecreasing/skipped-child counterparts.
3. Generic allocator/releaser and synchronous callback helpers, including
   compatible multiple bindings and incompatible/null/unknown alternatives.
4. Closed parser/serializer lifecycles through separate source files, ordinary
   compiler objects with checked linking, and validated cold/warm checkpoints.

Keep the original populations immutable. Independently discovered regressions
receive separate manifests and provenance; do not rewrite initial expectations
to fit the implementation. A negative must fail for its intended obligation,
not for a parse error, unrelated unsupported operation, crash or timeout.

Use pinned unchanged cJSON public APIs for parse/delete and
create/serialize/delete/output-release clients, covering nested values,
malformed input and allocation failure. Record exactly which inputs and API
contracts are proved. Also use an independent parser or serializer with
runtime input and separately selected generic helpers. Audit claimed positive
workflows for real upstream defects before requiring successful proof.
No third-party library-name certificates, upstream source edits, annotation
trust or unsafe trust may establish a positive inference result. Existing
modeled C-library trust remains explicit.

Add adversarial mutations and equivalent-source variants: bounds off by one,
uninitialized tails, capacity lies, overflow, saved aliases, missing cleanup,
duplicate ownership, reordered independent fields, helper extraction and
callback forwarding. Independent concrete oracles validate relevant finite
memory/ownership cases. Runtime sanitizers corroborate counterexamples but
are not a soundness proof of the analyzer.

Run full Debug and ASan/UBSan suites, strict changed-file clang-tidy,
formatting, the Core dependency boundary check and the unchanged fixed
evaluation. Preserve every valid baseline-complete corpus identity; remove a
false proof only with a documented counterexample. Compare complete canonical
reports across uncached/cold/warm execution, requiring positive warm reuse and
zero function analyses for unchanged reusable populations.

Measure three isolated sequential ordinary Release runs before and after;
median elapsed time and peak RSS must each remain at most 1.10 times baseline.
Every checked corpus project retains its 600-second deadline and the same
report encoding. Record function/context work, report and checkpoint sizes,
peak RSS and all failed development observations. Publish validation and
machine-readable evidence. No acceptance denominator, resource bound or
required workflow may be silently reduced to meet these gates.

## Annotation surface

None. Existing annotations retain their meanings. Positive inference cases
require no additional annotation or unsafe trust.

## Diagnostics

Retain `checking-incomplete` for missing evidence and `checking-failed` for
demonstrated violations, independently of warning severity controls. Existing
ownership, bounds, validity, integer and leak identifiers retain their meanings.
Pin new explanations for ambiguous state roles, unavailable relational
projection, failed recursive progress, unverified group outputs and unsatisfied
callback behavior with RFC 0029 unit and lit tests. Explain the originating
premise and call route without changing completeness when explanations share
storage. No new diagnostic identifier is planned.

## Drawbacks

More general contracts increase projection and invalidation complexity.
Recursive construction and callback requirements can accidentally authorize
facts circularly. More discovery can increase cost even for unrelated code.
Explicit hypotheses, group validation, bounded preparation, independent
counterexamples and measured whole-program gates address these risks.

## Alternatives

Raising unrolling/context limits cannot establish arbitrary-length invariants.
Library-specific trusted summaries would hide the inference gap. A complete
new semantic IR would offer stronger long-term isolation but adds migration
risk before delivering these workflows. This RFC extracts shared operations
where their semantics and acceptance cases are concrete. Archive packaging
would improve adoption but would transport the same incomplete contracts.

## Prior art

RFC 0013 supplies immutable entry identity and heap outputs. RFCs 0017 and
0021 supply target arithmetic and checked induction over cursor progress.
RFCs 0018–0020 separate sufficient requirements, verified outputs, explanations
and validated reuse. RFCs 0022 and 0028 preserve actual callback bindings and
opaque/private storage evidence. RFCs 0023, 0026 and 0027 separate structural
discovery from proof and allocation conservation. This RFC extends those
specific mechanisms while retaining their trust and mutation boundaries.

## Unresolved questions

Candidate ranking, internal indexes and precise factoring may be refined
during implementation without changing proof rules. The validation record
must state measured workflow coverage and cost. Any semantic extension or
acceptance change requires an explicit amendment before implementation;
unmet mandatory gates keep this RFC Accepted rather than Implemented.

## Future work

A general semantic IR, source-free checked library distribution, shared
recursive ownership, tracing collectors, asynchronous callbacks and concurrent
execution require separate designs. This milestone makes no whole-library or
whole-executable certificate claim beyond its actual selected contracts.
