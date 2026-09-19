# RFC 0026: Inferred relational contracts for growable buffers and vectors

- **Status**: Superseded
- **Authors**: WeaveC authors
- **Created**: 2026-09-13
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Superseded by RFC 0030

> Superseded by [RFC 0030](0030-prove-or-trap.md).

## Summary

Infer sufficient, reusable contracts for contiguous growable containers. Keep
allocation capacity, logical length, initialized contents and lifetime related
through actual C stores, branches, loop back edges and helper calls. Prove
construction, reserve, append, resize, truncate, clear, steal and destruction,
including allocation failure. Carry the same premises and outputs across
translation units, compiler objects and validated analysis checkpoints.

The owner requested drafting this RFC followed by implementation end to end.
After drafting, acceptance records that authorization; it does not claim a
separate independent review or an RFC-only merged pull request.

## Motivation

At v0.6.0 (`9a62565`), a closed caller that appends once to a buffer is checked.
A caller that repeatedly appends with a runtime count loses bounds, validity
and initialization, even after it tests the final pointer and length/capacity.
A fixed allocation with an ordinary complete fill is already checked. The
missing abstraction relates the container's changing fields and allocation.

Jansson's unchanged `strbuffer_append_bytes` grows by the maximum of twice
capacity and length plus incoming bytes plus a terminator. Its generic
contract remains incomplete although the existing concrete lifecycle caller
is complete. A useful contract must cover runtime inputs and every supported
mutation, not just a bounded enumeration of successful concrete call cases.

RFC 0025 completes 148 of 1,619 selected corpus definitions. Its validated
Release binary is SHA-256
`7494c72da73efa89b6dc6b9c30d4d794260413ea78228a3336ac962d76428b70`.
The unchanged five-project manifest and this executable are the baseline.
Checked Lua already takes 528.608 seconds under a 600-second limit. Candidate
inference must remain bounded and avoid unnecessary work in unrelated code.

## Soundness

Retain RFC 0018's conditional, single-threaded source guarantee, target C
integer semantics, library trust and explicit unsafe boundaries. A container
predicate is independent evidence about current storage. Field names, a record
type, a familiar helper name, or a branch comparing length with capacity cannot
establish that predicate. Candidate discovery contributes no proof.

A sufficient input predicate is an explicit caller requirement. A successful
closed caller has no input requirements and no added unsafe or annotation
trust. A generic helper remains independently selected and must check every
operation reachable under its sufficient entry contract. Postconditions are
installed only from complete contracts after establishing every premise.

Required distinctions:

- A live allocation with an established capacity versus a numeric capacity
  field larger than its allocation, a borrowed interior pointer, or dead storage.
- An initialized prefix versus unused capacity, skipped stores, and an early
  return before the advertised prefix has been written.
- Logical length at most capacity versus unsigned subtraction underflow and
  wrapping growth or element-byte multiplication.
- Successful replacement versus allocation failure: failure preserves the
  old pointer and its actual contents only when the implementation does so.
- The current field after replacement versus a separately saved pointer into
  the old allocation. Successful growth invalidates old aliases even if an
  allocator could reuse the numerical address.
- Disjoint append input versus a source alias invalidated by growth or an
  overlapping `memcpy`. `memmove` has its existing overlap semantics.
- Initialized pointer cells versus live, owned pointees. Borrowed elements
  cannot acquire release permission from the vector's ownership of its array.
- Complete destruction versus a lost owned element, a skipped release, or
  releasing the same element twice.
- A predicate established on every incoming edge versus an optimistic loop
  candidate, an unknown callback effect, or an unrepresented alias write.

The initial structural family is a complete ordinary record containing a
contiguous data pointer and represented integer length/capacity fields. Byte
buffers, terminated byte strings and fixed-size scalar/pointer element vectors
are included. Element ownership is tracked separately from the backing array.
Arrays of arbitrary recursive records, nested vector shape inference, shared
reference-count protocols, general recursive heaps, tracing collectors,
concurrency, representation punning and arbitrary nonlinear invariants remain
outside this milestone. Unsupported forms remain incomplete; no ordinary
violation is suppressed merely because a container predicate exists.

## Detailed design

### 1. Portable predicates in Core

Add bounded, Clang-free descriptors for a record's data, length and capacity
fields, target element size and count units. Descriptors validate canonical
record identity, field offsets/types and distinct count fields. Distinguish
accessible storage, initialized prefix, optional termination and element
ownership capabilities. An empty container may have a null pointer; positive
capacity requires live storage. Length zero never permits a positive read.

A predicate connects current values to storage, rather than treating mutable
field names as immutable allocation sizes. Preserve declaration/allocation-time
snapshots from earlier RFCs. Mathematical byte endpoints require represented,
nonoverflowing scaling; C integer wrap cannot supply that evidence.

The must-fact domain joins only predicates established on both predecessors.
The numeric values and allocation may differ between predecessors while their
common relational predicate survives. Pointee identity and saved aliases keep
their ordinary meaning; folding must not equate successive allocations or
revive an invalidated pointer. Unknown mutation retires dependent evidence.

Separation evidence may retain an entry backing identity when every current
alternative is that entry allocation or a fresh allocation created during the
call. This identity is usable only to express separation from other entry
objects. It supplies neither entry validity nor entry extent after replacement.
Replacing the backing with another input pointer loses this evidence; joins
retain it only when both edges prove the same entry identity.

Bound discovery to 16 candidate descriptors per function and retained facts
to 64 container instances. Descriptor encodings are bounded to 16 KiB. Existing
integer, path, context, initialization and obligation limits remain. An
exhausted bound loses proof and cannot be treated as an empty successful result.

### 2. Discovery, establishment and invalidation

Analysis supplies target layouts and discovers field relationships from actual
indexing, pointer arithmetic, allocation sizes and stores. No special names
or annotations are required. Ambiguity may conservatively decline inference.
Inference should not visit or allocate a container domain in ordinary mode.

At generic entry, a candidate sufficient predicate may be used only with an
exported requirement naming its actual interface path. After a mutation, it
cannot be assumed again from the original input. Constructors and local
objects establish predicates from actual allocation, initialization and count
facts. Initializers, complete copies, aliases and nested field paths must use
the existing storage and view machinery.

Before changing dependent fields, retain only the appropriate entry snapshots.
Validate the new predicate from the resulting state. Capacity changes alone
never resize storage; length changes alone never initialize bytes. Pointer
replacement retains new allocation facts and invalidates affected saved
aliases. Unknown writes, callback effects, unsafe operations and lifetime ends
retire affected predicates and cannot silently restore entry assumptions.

### 3. Mutations and loop induction

Support reserve and append using `realloc` and allocate/copy/free helpers,
including guarded addition, multiplication and maximum-based capacity growth.
A reserve preserves exactly the initialized old contents that fit the new
allocation; its new tail is uninitialized. An append extends the initialized
prefix only after the corresponding stores or checked copy complete.
Termination additionally requires an actual zero store at the new endpoint.

Resize, truncate and clear distinguish logical contents from allocated storage.
Steal transfers the actual backing ownership and leaves only the state the
source code establishes. Destruction discharges backing storage and, for
owned vectors, the independently established owned element footprint.
Supported failure branches preserve or change the container exactly as their
CFG and return outcome specify. A false success or partial update cannot
export the full success predicate.

Fold established current-state relations before a CFG join and use the common
predicate on the next iteration. Validate base edges and every back edge;
`break`, `continue`, `goto` and allocation failure must not be omitted. Proof
cost for repeated appends must not depend on enumerating runtime length.
Do not raise iteration/context budgets to accept the new population.

For pointer elements, maintain selected-element identity together with any
quantified initialized/owned prefix. Pop, transfer and cleanup must preserve
which owner is responsible for each pointee. A backing reallocation preserves
pointee identity but invalidates pointers into the old array itself. Unknown
selection or unsupported quantified ownership transfer remains incomplete.

### 4. Contracts and transport

Introduce a checked `buffer` predicate on an interface object path, using a
validated descriptor in its portable payload. Requirements are sufficient
premises; outputs state only independently established current-state facts.
Reuse input guards, outcome classes, strict path/global remapping and checked
call-entry snapshots. All descriptor paths and premises participate in summary
comparison, dependency invalidation, canonical serialization and cache keys.

Buffer descriptors distinguish permission to release the backing allocation
from permission to release distinct pointees in the logical prefix. The latter
is available only for pointer elements; neither follows from initialized
pointer cells. A sufficient owned-buffer input states those capabilities
explicitly. A join retains each capability only when both predecessors prove
it. Helpers that can replace or release backing storage may infer the stronger
input; constructors establish it from allocation or a null pointer. Reading
helpers do not demand allocation ownership merely to read the prefix.

A `buffer` output may additionally carry a nonnegative minimum capacity in
`end`, in elements and expressed using immutable call-entry integer values.
Zero is the empty bound; `begin` and `other` remain reserved. Each return edge
must prove this lower bound independently. This represents reserve's successful
`capacity >= requested` result without pretending that every implementation
sets capacity to exactly the requested value. Outcome-dependent bounds are
kept guarded until the result is tested; writes to the destination or captured
dependencies retire them. Input buffer predicates reserve both endpoints at
zero. These bounds never establish a buffer predicate from numerical fields.

Generic helpers must remain complete under their stated contracts. Concrete
input specialization remains available but is not a substitute for the generic
acceptance population. Calls discharge the buffer premise against actual
storage and contents or export an appropriate sufficient input premise.
Direct and resolved callback calls retain the same requirements; outputs
intersect over returning targets. Missing or unknown alternatives stay visible.

Two output-only refinements carry pointer-cell identity independently of byte
initialization. `buffer-preserved` states that the logical sequence equals its
entry sequence. `buffer-appended` states that it equals its entry sequence
followed by the pointer value at `other`, with length increased by exactly one.
Both use the same validated object descriptor and reserve their numeric
endpoints at zero. Their proof domain starts with an immutable entry sequence,
preserves it through proved backing copies, and loses identity evidence on
overwriting an existing element or an unknown mutation. An actual store at the
old endpoint supplies the appended value; the output must also prove the new
length and initialized prefix. These refinements cannot be input assumptions.

At a caller, a preserved sequence keeps its existing element capability. An
appended sequence keeps distinct ownership only when the incoming prefix is
owned and the new value is a separately owned, live allocation base (or null).
The successful result transfers that responsibility into the prefix; failure
does not. Selected concrete cells additionally retain ordinary value identity
and borrowing evidence. Initialized bytes alone never establish either output.

Bump summary format 20 to 21, sidecar format 21 to 22, and checked encoding 7
to 8. Reject old object metadata using the existing rebuild behavior. Validate
malformed descriptors, oversized tables, missing dependencies and unsupported
combinations before publishing facts. Expanded/compact reports retain the
same complete semantic content. Checkpoints remain bound to the executable,
preprocessed inputs and imported dependencies.

### 5. Frozen acceptance and independent validation

Before implementation, freeze sources and expectations for generic helpers and
closed callers covering runtime append, reserve, initialized-prefix reads,
capacity mismatch, arithmetic overflow, allocation failure, aliased input,
retained interior aliases, truncate/clear, steal, scalar vectors and pointer
vector ownership. Include a distinct negative for every claimed preservation
or establishment rule. A negative must fail for its intended property; an
unrelated error, timeout, parser failure or missing report is not acceptance.

Keep separately identified source, separate-unit, compiler-object and cache
populations. Preserve the original RFC 0019 Jansson adapter population and
require its generic append helpers to become complete without weakening the
fixture or selecting away their definitions. Add runtime repeated-growth
clients of unchanged Jansson sources with established allocator bindings.
Exercise a complete vector lifecycle with runtime counts and failure cleanup.
Record pinned upstream identities and source hashes before checking.

Use an independent finite-state oracle for predicate establishment, join,
mutation, initialized-prefix and ownership distinctions. Add malformed-record,
summary round-trip and dependency invalidation tests. Include saved-pointer,
callback, alias-write and branch-join adversarial cases independently of the
primary examples. Run full Debug and ASan/UBSan suites, strict changed-file
clang-tidy, formatting and the existing evaluations.

Preserve all 148 exact complete corpus identities unless a concrete
counterexample demonstrates an existing false proof, which must be documented
separately. Record new complete contracts and their requirements; diagnostic
counts are not defect recall. Compare uncached, cold and warm canonical reports
and require positive warm reuse with zero function analyses.

Run three sequential ordinary Release observations before and after; median
time and peak RSS must each be at most 1.10 times baseline. Every checked
project retains its 600-second limit. Keep the same per-project report encoding
for time comparisons. Record memory, report size, function/context work and
all failed development observations. Publish a validation document and
machine-readable evidence; do not refresh away failures or reduce denominators.

## Annotation surface

None. Existing selection, ownership, sizing and unsafe annotations retain
their meanings. Positive inference fixtures require no new annotation trust.

## Diagnostics

Use `checking-incomplete` for missing buffer/storage/prefix/ownership evidence
and `checking-failed` for demonstrated violations. Preserve existing temporal,
spatial, integer, invalid-release and leak diagnostics. Pin new explanation
messages with RFC 0026 unit and lit cases. Changing diagnostic severity cannot
discharge a missing predicate or make selected checking succeed.

## Drawbacks

Relational folding can accidentally reconnect old aliases to new allocations.
Mutable counts, conditional outputs and copying require careful invalidation
across every fact domain. Quantified element ownership is distinct from byte
initialization. Overly broad discovery can strengthen unnecessary contracts
and increase cost. Explicit budgets, independently checked transfer rules and
frozen whole-lifecycle examples constrain those risks.

## Alternatives

More concrete specializations accept small examples but do not establish a
runtime invariant. Larger unrolling budgets increase cost and do not prove
arbitrary lengths. Treating sized fields as trusted allocation facts would
accept capacity mismatches. Handwritten library summaries hide the generic
inference gap and add trust. General symbolic execution or user-written
logical predicates would be a larger change to the project's inference model.

## Prior art

RFC 0013 separates allocation-time values from mutable holders. RFC 0015
separates array-cell ownership and range operations. RFC 0019 transports
initialized-prefix preservation through realloc wrappers. RFC 0021 checks
loop induction at actual CFG edges; RFC 0023 separates candidate discovery
from established inductive predicates. This RFC combines those principles for
contiguous mutable containers while preserving C source and pointer ABI.

## Unresolved questions

Internal representation and discovery heuristics may change while preserving
these semantics and acceptance populations. The realized coverage and cost
were recorded in the validation report (removed by RFC 0030), including the
absence of a broad-corpus completion gain and the runtime-buffer/helper gains.
A change to semantic scope or required acceptance must amend
this RFC explicitly before its implementation; a fixture cannot simply be
weakened to accommodate a missing proof.

## Future work

General recursive ownership, nested aggregate containers, shared-reference
protocols, asynchronous mutation, archive distribution and finer-grained
persistent analysis reuse remain separate milestones.
