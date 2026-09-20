# RFC 0028: Inferred contracts for opaque objects and private library state

- **Status**: Superseded
- **Authors**: WeaveC authors
- **Created**: 2026-09-15
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Superseded by RFC 0030

> Superseded by [RFC 0030](0030-prove-or-trap.md).

## Summary

Preserve inferred memory contracts across ordinary C library boundaries. A
client may construct, borrow from, mutate, transfer and destroy a supported
opaque object using its public header without seeing its record definition.
Calls transport established private library state, including nested allocation
hooks, without exposing private C declarations. Contracts compose through source
units, compiler objects and validated checkpoints. Replace the special-purpose
private callback proxy encoding; do not retain a compatibility reader.

The owner requested this RFC first and implementation end to end. Acceptance
records that authorization, not independent review or an RFC-only merge. Any
subsequent design changes must be recorded here before implementation.

## Motivation

The v0.8.0 baseline (`a6657d8`) passes all 1,298 CTest entries. Its broad checked
population contains 148 complete conditional contracts among 1,619 selected
definitions. Those figures measure that population, not arbitrary-C coverage.

A separately defined list constructor and destructor can establish a closed
client when its header contains `struct node { unsigned value; struct node
*next; };`. The same client with only `struct node;` fails with `incompatible
or unknown object view at call`. Public opacity hides layout, not the object's
actual allocation or the verified effect of its destructor.

Likewise, this closed client checks when it includes the unchanged cJSON
implementation, but fails when it includes `cJSON.h` and the implementation is
analyzed as a separate source unit:

```c
int main(void) {
    cJSON_InitHooks(0);
    cJSON *object = cJSON_CreateObject();
    if (!object) return 0;
    cJSON_Delete(object);
    return 0;
}
```

The private `global_hooks` record is outside the existing portable-global
exception. Initializing it in one call cannot establish its actual members
for subsequent calls in a foreign unit. No new allocator-name trust is needed:
the implementation and initialization sequence already provide the evidence.

## Soundness

Retain RFC 0018's conditional, single-threaded source guarantee and explicit
trust ledger. A declaration, layout descriptor, nominal type, function name or
metadata import grants no ownership, initialized bytes, callback target or
release permission. Only actual storage, verified call postconditions and
sufficient entry requirements establish those facts.

Opaque evidence belongs to the represented pointer value and allocation
identity. Copies preserve identity; they do not create allocations, shares,
separation or independent destruction rights. A destructor invalidates saved
aliases and borrowed results. Mutation may preserve an owner's lifetime while
invalidating a borrow into replaced backing storage. Null, raw, released,
uninitialized and incompatible handles cannot acquire evidence from a callee's
requested predicate. An unrelated complete type cannot impersonate an opaque
object merely because it has the same size or field spellings.

Private cells have one identity per defining declaration and translation unit.
Two libraries' `static hooks` cells are distinct. Foreign storage descriptions
are analysis metadata, not source-visible declarations. Initializers describe
initial storage; a reachable later write cannot be overridden by that initial
state. A call's input state is captured before effects, and output facts refer
to the values actually left by that call. Joins retain only guarantees supported
on every relevant path. Unknown writes/callbacks invalidate affected state and
dependent specializations. Missing mappings or conflicting metadata never drop
a required premise or authorize an output.

Required rejected counterparts include double destruction, dropped child
ownership, a stale accessor result, a stale buffer borrow, a forged or cast
incompatible handle, unknown/nullable callback targets, hook mutation invalidating
a previous binding, partial initialization and a callback that omits cleanup.
Independent objects and disjoint private cells must retain their evidence.

The supported object families remain RFCs 0023, 0026 and 0027's bounded buffers,
chains and finite ownership forests, plus represented finite heap objects.
General shared graphs, concurrency, signals, nonlocal jumps, arbitrary type
punning, mutually recursive proof inference and parser/printer certification
remain outside this change. Unsupported private types or exhausted metadata
limits remain incomplete. Ordinary unselected checking retains its diagnostic
policy. No runtime instrumentation or pointer ABI change is introduced.

## Detailed design

### 1. Portable interface descriptions

Add a Clang-free validated representation of the supported C storage types used
by private roots and opaque interface projections. Describe target size and
alignment, scalar category, pointer referent, function signature, fixed array
extent, and record fields with their byte layout and compatible identity.
Recursive pointer types use bounded graph references rather than unbounded
expansion. Qualifiers and unsupported storage remain explicit; dropping a
qualifier cannot create write permission. Representation information is immutable
and separate from all flow-sensitive proof facts.
Object-layout keys describe the unqualified record definition; use-site
qualification remains a separate access restriction. Private-storage
descriptions retain their root qualifiers, and every description retains
qualifiers on its members and referents.

Validate references, field names, duplicate members, bounds, alignment, recursive
by-value cycles and canonical encodings before use. Bound each description to
128 type nodes, 64 fields per record and 64 KiB encoded bytes, with path depth
bounded by the existing eight-step summary limit. Unsupported types decline
portable projection. These limits bound metadata, not runtime allocations.

Analysis creates descriptions from Clang's target types and layout. It may
materialize internal type/storage adapters for existing place operations, but
must not complete or replace the client's forward declarations, insert private
names into source lookup, or let metadata change compilation/code generation.
An adapter must reproduce and validate the described target layout. Complete
source types continue to undergo compatible-view checks.

Verified buffer postconditions may name a returned object's storage (`result *`)
just as existing postconditions name caller-supplied storage. Export these only
on represented non-null returns, after proving the buffer predicate in the
callee. Capture input-dependent bounds before call effects and install the
returned predicate under the returned pointer's non-null guard. This carries
an initialized-prefix relation; it does not invent exact field values or
initialized bytes beyond that prefix.

### 2. Private module roots

Give supported private static storage a stable portable identity containing its
defining source and declaration identity, including local statics. Keep the
identity separate from the storage description. Macro-generated declarations
also include their
spelling and immediate expansion chain (bounded to 128 steps), so two private
declarations in one outer macro expansion cannot collide. Export and import
the same root through summaries, guards, callback bindings, memory contexts, proof
requirements and outputs. A defining unit resolves its own root to the original
declaration; another unit uses internal analysis storage.

Replace RFC 0022's `@weavec-hook` special case with this general facility.
Supported records include nested scalar, data-pointer and callback members;
fixed arrays and scalar private configuration cells use the same mechanism.
Private names must not collide with public linkage names or another module.
Conflicting descriptions for one identity lose portable evidence and are never
resolved by choosing whichever unit was imported first.

Private roots remain private in C, but their abstract effects are caller-visible.
Keep the existing ordinary may-effect and checked must-proof distinctions.
Transport conditional writes, entry snapshots and replacement effects using the
normal summary machinery. Unsupported roots retain explicit incomplete checked
coverage; they cannot silently disappear from a public contract's premises.

### 3. Opaque object evidence

Carry verified record-view evidence with heap outputs and supported abstract
buffer/container predicates. Calls using a forward-declared handle may inspect
the inferred contract's representation metadata only after the actual argument
provides compatible evidence. A fresh allocator alone does not establish an
initialized list or buffer. A constructor's verified initialized heap or
inductive postcondition can do so.

Resolve summary paths against the evidenced object and internal projections,
preserving field offsets, nested identities, aliases and allocation footprints.
Continue checking every required path prefix. Metadata for an unrelated object
or another argument cannot authorize a path. Forwarded opaque parameters may
export sufficient requirements; a closed caller must discharge them. Typed
descriptions are reused within the immutable unit/database lifetime.
Typed path validation need not materialize a parallel chain of analysis places.
Resolve that chain lazily only when an erased or incomplete type requires
flow-sensitive object evidence; still validate every requested type prefix.
After every prefix of an exact call/path has passed using source types alone,
its static layout validation may be reused for that call expression and the
same live immutable summary. Bound this implementation cache to 1,024 paths per
function analysis and discard it on saturation. An expired or replaced summary
cannot validate another owner at a reused address. Never cache a validation
that used flow-sensitive opaque evidence, and never reuse memory permissions,
initialization, lifetime or release obligations through this layout cache.
Completed specializations transfer their summaries and collected diagnostics
into immutable publication storage. Avoid deep copies of objects whose producer
has finished; keep every diagnostic, dependency and proof fact unchanged.

Prepared call explanations retain the existing 1,024-entry / 64 MiB bounds,
but replace whole-index clearing with least-recently-used eviction. Refresh
recency only after validating the same live immutable projection. On capacity
pressure, discard only enough oldest preparations to admit the new one; charge
the recency index to the same byte bound. Expired source projections remain
unusable, and eviction changes no ledger content, insertion order, truncation
or proof limit. Compare cached applications against individual insertion while
refreshing entries, evicting older entries and replacing source projections.
The existing `explanation_call_resets` counter records capacity-pressure events;
each such event may now replace a subset of the index instead of all entries.

Canonical explanation selection may compare the first differing serialized
location component instead of constructing both complete route strings. Keep
the existing shortest-route priority and bytewise JSON/decimal ordering,
including the separators after line and column numbers. Differential tests
must include escaped filenames, common prefixes and decimal prefix cases.

The place interning index may use hashed, non-owning lookup keys to avoid
repeated tree searches and temporary field strings. Stored keys must own their
bytes. Dense identifiers, display names and descendant order remain defined by
creation order, independently of hash-table iteration or rehashing. Insertion
must also accept a field view borrowed from an existing entry while the entry
vector grows. Copies of a place table retain independent key storage.

Effect-map joins may merge sorted paths with a moving insertion position,
instead of independently searching from the root for every path. Preserve the
exact `PlaceEffect::join` operation and the existing distinction between generic
empty effects (discarded) and empty per-outcome effects (retained). Check the
merged maps against individual insertion across overlapping and disjoint keys.
Null-entry allocation accounting may precompute pointer-parameter places once
per function run. Every edge still checks current nullness and replacement;
precomputation grants no flow-sensitive fact.

Summary-path steps may use shared immutable backing storage. Copies and
prefixes may reuse those bytes; every edit must detach before changing storage
that another path can observe. Expose only const element views and explicit
append, prepend and truncation operations, including safe self-append. Retain
one immutable canonical dereference step for this common representation value.
This supplies no object evidence or memory permission. Preserve exact path
equality, lexicographic ordering, field bytes, index selectors and serialization;
the existing path limits do not change. Moves must leave a valid empty source.
Compare edited/shared paths with independent vectors, including borrowed input
elements, prefix growth, aliasing, self-append and source destruction. Measure
both path creation and repeated copying, then verify complete corpus reports.

The shared backing may use one checked allocation for an atomic reference count,
capacity and a contiguous array of elements, replacing a separately allocated
vector buffer and shared-pointer control block. Keep the handle to one backing
pointer and visible length. Edits detach shared storage, growth moves unique
storage, and truncation still preserves shared prefixes. Use checked size
arithmetic, correctly aligned object construction and complete destruction;
copying different handles retains thread-safe ownership accounting. A uniqueness
check must acquire prior releases before editing backing storage, so reads
through a released copy happen before subsequent in-place mutation. All path
lengths remain `size_t`, with the existing eight-step summary-path model limit.
Compare creation, copying and allocation cost, and retain the differential path
tests and complete report comparisons.

Exported generic and specialized function summaries may retain an immutable
shared publication instead of a second mutable value copy. Copying unit exports
and building database indexes with unchanged global numbering reuse that
publication.
Replacing an export or widening creates a new publication. Remapping recomputes
the global projection and may retain the input publication only when its entire
semantic value and explanations remain unchanged; otherwise it publishes a new
value.
Previous exports and readers keep their original contents. Specializations stay
indexed by their exact callback or memory input; sharing does not merge input
contexts or authorize reuse under different premises. Expose a const value
view and explicit replacement, never a mutable reference into a publication.
Equality and serialization compare the complete summary value, independently
of pointer identity. Test replacement, unit copying, database reuse, remapping
and source-owner destruction, and retain complete cold/warm report equality.
Memory-specialization publication retains the existing join, including its
normalization from an empty summary. Reuse an input owner only when the joined
semantic value and complete explanations are unchanged; otherwise publish the
joined replacement. Prior readers retain the previous specialization.

Returned borrows and subobjects retain their owner's identity and actual
offset. Transfer, attachment, detachment and destruction use the existing
structural and footprint obligations. A successful complete-consumption output
settles only the covered allocation footprint. Ordinary use-after-free, leak,
wrong-release and borrow checks remain effective through opaque aliases.

### 4. State transitions and specialization

Private state participates in the existing flow-sensitive stores and guarded
outcomes. Explicit initialization and setters establish actual scalar and
callback values. Generic joined target sets remain conservative scheduling
information and cannot replace a caller's established value or exclude an
unknown alternative. Carry callback/userdata associations through their actual
paths where already supported.

Checked-contract analysis also requests an actual callback input when a setter
only copies that input to storage. This preserves precise caller targets through
checked heap outputs. Ordinary analysis keeps these copy-only stores symbolic
in their parameter or global input paths; copying alone does not request a
callback specialization. Actual indirect calls still nominate callback inputs
under the existing ordinary rules. Validate both the symbolic forwarding of
ownership effects in ordinary mode and the additional checked setter requests.

Specialize only on represented relevant state using the existing context and
target bounds. A changed cell invalidates dependent facts and contexts, while
unrelated module cells and independent object evidence survive. Nested calls
inherit all consulted dependencies. Changed descriptors and missing/new roots
participate in database and checkpoint validation. Reusing a specialization
requires its complete input premises and current dependencies.
Publishing a specialization whose dependency snapshot is current need not
schedule a scan of every other specialization. Preserve any pending validation
from an actual dependency change, and check the newly captured snapshot: a
nested analysis may have changed a dependency since its first observation.
Discarded specializations need no repeated version validation. These changes
must preserve dependency misses, transitive inheritance and invalidation.

Call-input footprint preparation may stop as soon as more than the existing
64 context facts are required. Return an explicit over-limit result, never a
truncated footprint that could be used as a context. Smaller footprints retain
their exact paths. Derive storage prefixes from shared immutable path prefixes
without rebuilding their steps. A per-function cache may retain up to 64 such
preparations, keyed by a live immutable summary owner, including over-limit
results. Replaced or expired owners must recompute; no caller values, actual
aliases, checked case additions or proof facts belong in this cache. Verify
boundary rejection, duplicate paths, replacement, expiration and eviction.

Numeric-output joins may update their map in place without copying every
alternative first. A path absent from either non-bottom input still acquires
an unknown alternative; visiting newly inserted paths cannot alter that rule.

An exported numeric alternative may constrain its own value expression with
a typed interval in its input guard. Intersect that alternative's evaluated
range with this interval before joining alternatives. The restriction is local
to the alternative and its result class; it must not narrow caller inputs or
other outcomes. Unknown alternatives still prevent a guaranteed output. This
preserves facts such as a buffer length becoming positive on append success
even when the caller cannot name its hidden length field.

Verified deallocator wrappers export an `allocation-consumed` output for a
direct pointer parameter whose entry allocation is definitely released with
`free`. Track this as historical must-evidence, intersected at CFG joins and
return alternatives. A may-free effect, a callback prototype, a nullable target
or a skipped release cannot produce it. The first implementation records direct
entry-parameter releases; an unchanged entry parameter proved null satisfies
the output vacuously because it represents no allocation. A parameter assigned
null in the body cannot use this rule for its previous entry allocation.
The implementation composes unconditional outputs; richer conditional release
forwarding and reassigned-parameter entry projection may remain incomplete.
The output names one allocation,
never the children or payloads of a container. Applying it accounts for the
actual argument's head allocation only, after the ordinary checked call
preconditions, target and matching release capability have been established.
This is distinct from `container-consumed`, which proves complete cleanup of
an ownership footprint. Both use the new summary/sidecar versions below.

### 5. Transport and reports

Transport descriptions with unit exports and validate them before importing
summaries or contexts that refer to them. Include them in convergence, global
renumbering, database copy isolation, checkpoint fingerprints and round trips.
Object validation retains source/header, preprocessing, compiler-command,
target and object-content bindings. This RFC does not add archive packaging or
authorize source-free checked linking.

Requested callback and memory contexts are demand metadata, distinct from the
computed specializations governed by the existing 32-context analysis limits.
A program can union requests from several callers, and a defining unit can
retain that union even when some requests exceed its specialization budget.
Transport up to 65,536 distinct requests per symbol and context kind, with a
separate validated I/O bound; larger records decline transport. Preserve every
represented request in sidecars and checkpoint fingerprints. Do not trim demand,
increase computed-specialization limits or manufacture results for requests
that the analyzer could not satisfy. Test both request bounds, unchanged result
bounds and lossless checkpoint replay for a demand set larger than 32.

Checkpoint serialization may stream unit fields and diagnostics directly,
using bulk string escaping instead of constructing and walking a second JSON
object tree. Preserve every parsed field, diagnostic order, UTF-8 handling,
producer round-trip validation, checksum and compression bound. JSON escape
spellings may differ while decoding to the same strings. Shared explanation
tables may use owned hash indexes with exact equality; first-use vectors, not
hash iteration, determine serialized ids and row order. Rehashing must preserve
references to owned keys. Compare table contents and ordering against an
independent ordered-map model, and verify nested diagnostic/control-character
round trips and complete cold/warm report equivalence.

Bump summary format 22 to 23 and sidecar format 23 to 24. Bump the private
checkpoint format when its contents change. Older artifacts must rebuild;
remove superseded private-hook import/encoding paths. Update every affected
test and guide in the same change. Existing report representations may continue
to encode the same semantics; add explicit interface/state provenance where
needed to explain newly supported premises without duplicating entire callee
reports. A complete closed client has zero undisclosed entry requirements and
no new annotation or unsafe trust.

### 6. Evaluation and acceptance

Freeze the initial source fixtures and expectations before checker changes,
retain their hashes and baseline outcomes, and keep later regression cases
separate. The initial inventory contains 24 source clients and five unchanged
cJSON clients, with SHA-256 inventories beside their manifests. Required
populations cover:

1. Opaque chains and buffers: constructors, nullable outcomes, accessors,
   forwarding, mutation, transfer and complete cleanup. Include visible-layout
   counterparts, double cleanup, lost ownership and saved aliases.
2. Private state: scalar and record hooks, nested fields, actual initialization,
   setters, state changes, same-spelled roots in independent modules, unknown
   mutation and unsuccessful initialization. Include custom callback wrappers
   and alternatives that fail to establish the promised output.
3. Unchanged pinned cJSON public-header clients for default-hook empty/nested
   lifecycle and supported custom hooks, with stale-alias and shared-owner
   negatives. Compile the implementation separately; including its `.c` file
   in a client does not satisfy this population.
4. Separate-source and compiler-object versions of the supported cases, plus
   cold/warm cache equivalence, changed implementations/state descriptors,
   malformed metadata, corrupted checkpoints and stale object rejection.
5. Core algebra/codec tests for identity separation, descriptor validation and
   bounded metadata; independent small state-transition and alias/lifetime
   counterexamples, including renamed fields and modules.

Positives must complete with their expected requirements and trust. Negatives
must have the intended violated or unresolved property; syntax errors, crashes,
timeouts and absent reports are failed observations. Preserve the original fixed
44-bug/32-clean evaluation and valid existing complete corpus identities. A
demonstrated false proof is removed with a retained reproducer, never preserved
merely to keep a count. Report new generic contracts and closed-client proofs
separately. Do not infer whole-library certification from selected clients.

Run full Debug and ASan/UBSan CTest suites, warnings-as-errors, formatting, strict
lint on changed C++ translation units and the Core include boundary. Measure
three sequential ordinary runs of the five pinned projects against a retained
matching Release baseline. Median total time and peak RSS must be at most 1.10
times baseline. Cold checked runs retain the existing 600-second project limit;
all 51 warm units must reuse with zero function analyses and equivalent reports.
Run isolated cost observations after builds/tests stop and retain failed
observations. Do not increase semantic budgets to satisfy a positive fixture.

Implementation and acceptance results were recorded in the
validation record and machine-readable evidence (both removed by RFC 0030).

## Annotation surface

None. Public headers require no added ownership or state annotation.

## Diagnostics

No new diagnostic identifiers. Existing `checking-incomplete`, `checking-failed`,
`analysis-incomplete` and temporal/resource diagnostics retain their meanings.
Unsupported or conflicting interface metadata supplies an explicit incomplete
reason. Reports must distinguish missing object evidence, incompatible layout,
unestablished state and a violated lifetime or ownership obligation. New lit
tests pin representative messages and borrowed-alias rejection.

## Drawbacks

Transporting private state increases the dependency graph and may create more
specializations. Portable types add a validator and a Clang adapter that must
agree on target layout. Opaque evidence makes invalidation more demanding:
forgetting a hidden dependency could retain a false proof. Conservative
invalidation may reject safe programs or increase analysis cost. The fixed
positive/negative populations and existing cost gates constrain these risks.

## Alternatives

- Expose private layouts in public headers: changes the C interface and leaves
  private state transitions unresolved.
- Trust handwritten library summaries: useful for unavailable code, but adds
  trust where complete source and actual state already exist.
- Extend the callback-only proxy with another special case: repeats the same
  identity/description problem for each private storage family.
- Rewrite the complete analysis engine first: a separate worthwhile project;
  this change replaces the affected boundary machinery while preserving the
  established domains and their tests.
- Do nothing: preserves the demonstrated public-header and separate-unit gaps.

## Prior art

RFCs 0013 and 0027 establish the key distinction between an interface root and
the allocation identities and footprint it carries. RFC 0022 supplies checked
opaque-pointer recovery and the first private-state transport exception. This
RFC generalizes their representation boundary while retaining positive evidence.
RFC 0020 supplies dependency-based reuse and strict artifact validation. C's
incomplete record declarations provide the source abstraction boundary; the
analysis must preserve that boundary instead of rewriting user declarations.

The standard non-allocating placement array form adds no array allocation
overhead. The backing reserves aligned space before beginning element lifetimes;
see the [C++ draft, new expressions](https://eel.is/c++draft/expr.new).

## Unresolved questions

None required for acceptance. Exact adapter organization and fixture counts are
implementation choices within the mandatory populations above. Record any
new semantic decision here before implementing it. Inference limits discovered
on additional real libraries must be documented without weakening frozen gates.

## Future work

Archive packaging, source-free library proof artifacts, arbitrary shared graphs,
mutual recursive inference, parser/printer verification, asynchronous callback
protocols and a general analysis IR remain separate milestones.
