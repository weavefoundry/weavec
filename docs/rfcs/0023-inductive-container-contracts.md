# RFC 0023: Inductive ownership contracts for linked containers

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-11
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFC 0013's finite heap descriptions,
  RFC 0015's container ownership, and RFCs 0018–0022's compositional checked
  contracts. Preserves RFC 0020's dependency and artifact validation rules.

The project owner requested this RFC first and then implementation end to end.
The initial Accepted status recorded authorization to implement this design;
it does not claim an independent review, an RFC pull request, or a merge. The status
is now Implemented after the acceptance population and validation gates below
passed; see [the validation record](../validation-rfc0023.md). Implementation
discoveries that change these rules are recorded here before the corresponding
code changes.

## Summary

Infer and check contracts for finite linked chains and list segments. A contract
can describe every remaining node without expanding a fixed number of `next`
fields. Construction establishes the predicate; supported traversal, insertion,
detachment, reversal and destruction preserve or consume it. Helpers transport
the same predicates across source units, compiler objects and checkpoints.
Positive evidence remains separate from ordinary may-alias and may-free facts.
No source annotation, pointer ABI change or runtime instrumentation is added.

## Motivation

At `8c75d4b`, the Debug suite has 1,098 passing CTest entries. RFC 0022's frozen
corpus has 124 complete conditional contracts among 1,619 selected definitions.
The baseline Release executable is preserved at
`build/rfc23-validation/baseline/weavec`, with SHA-256
`882ddf6d9d6135ca338dcce76396e4b2a4c4f70c6f1a0ab587b2c44369603048`.

Reading one initialized node through a helper checks successfully. The following
helper remains incomplete even when its caller constructs two initialized local
nodes. A standard cleanup loop remains incomplete for one allocated node.

```c
struct node { unsigned value; struct node *next; };

static unsigned count(const struct node *p) {
    unsigned n = 0;
    while (p) {
        ++n;
        p = p->next;
    }
    return n;
}

static void destroy(struct node *p) {
    while (p) {
        struct node *next = p->next;
        free(p);
        p = next;
    }
}
```

The same missing validity, initialization and bounds evidence appears in
unchanged cJSON traversal definitions. Increasing `MaxHeapPathDepth` cannot
establish an invariant over runtime list lengths. The missing abstraction is a
finite description of a potentially unbounded number of distinct live nodes.

## Soundness

### Guarantee and assumptions

Retain RFC 0018's conditional source guarantee. Every reachable operation in a
selected function needs positive evidence, a sufficient entry requirement, or
an explicit trusted boundary. An inferred container requirement is an obligation
on callers, not permission to assume that arbitrary incoming pointers are lists.
The execution model is single-threaded C with Clang's target types and layout.
Allocation and library implementations retain their explicit existing trust.

The supported predicate describes a finite acyclic chain ending at a specified
endpoint, initially null. Each non-end node has a compatible record view, live
storage and the initialized fields required by the operation. Traversal may
borrow stack, static or allocated nodes. Destruction additionally requires
allocation-base identity, the matching release family, permission to consume
each node and owned payload, and separation from the surviving structure.

Node type, field spelling, a non-null test, or allocation-site equality cannot
establish a chain. Two allocations executed at the same site are distinct
dynamic objects. A copied pointer retains its alias relationship and does not
create a new ownership share. In particular, a folded tail cannot erase a saved
alias and subsequently make its use after destruction safe.

### Required positive and negative distinctions

- Empty, singleton and runtime-length constructed chains versus unknown,
  uninitialized, dangling or cyclic links.
- Read-only traversal of live borrowed nodes versus release of stack nodes,
  interior pointers or allocations from another release family.
- Save-successor-before-free cleanup versus reading `p->next` after `free(p)`.
- Complete construction, including allocation failure cleanup, versus publishing
  a node before its link or required payload is initialized.
- Detach and return a still-live node versus return or read a deleted node.
- Reverse or concatenate disjoint segments versus introduce a cycle or give
  two owners the same suffix.
- Preserve an independent chain across mutation or release versus use a saved
  alias into the affected chain after release.
- A payload actually owned by each node versus shared or borrowed payloads
  whose release would invalidate another live object.
- A proved loop invariant on every entry and back edge versus a path that
  skips the required initialization, relinking or ownership transfer.

### Conservative rejections and exclusions

General graphs, arbitrary recursive trees, cyclic owning containers, tracing
collectors, concurrent mutation, asynchronous callbacks, tagged unions and
arbitrary enclosing-record recovery are outside this milestone. Nonowning
backlinks are ordinary borrowed fields; they do not confer recursive ownership.
The frozen real traversal targets do not require proving doubly linked mutation.
Unrestricted user-defined logical predicates are not introduced.

Unknown aliasing, an unrepresented mutation, an unverified recursive proof or a
budget limit remains incomplete. A safe cyclic traversal that runs forever may
be rejected by the finite-chain predicate. This is a memory-safety analysis,
not a termination proof for arbitrary C. No existing violation or unrelated
unsupported construct may be removed because a container operation is proved.

## Detailed design

### 1. Portable container predicates in Core

Add a Clang-free container domain. A shape descriptor records the canonical
record identity and target layout, the link field and its byte range, the
required initialized data fields, write capability and optional node/payload
release families. Field offsets and widths are validated against the record.
Descriptors are bounded, canonical data; they contain no executable expressions.

A segment denotes an endpoint-exclusive sequence. Empty segments are valid.
A nonempty segment unfolds into a head node and a separated remaining segment.
Folding requires both pieces and proof of separation. Cap the number of active
descriptors, explicit nodes and segments; exhaustion supplies no proof. Dynamic
length is represented abstractly and does not require increasing these limits.

The domain distinguishes concrete node identity, borrowed cursor membership,
owned segments, and unknown pointer values. It records initialized fields and
the dependencies of each predicate. Joins keep only properties established on
every incoming path. Null and non-null alternatives are not interchangeable.
Unsupported overlap weakens the affected predicate, never invents disjointness.
Release invalidates every represented alias/member of its consumed region.
This includes native byte pointers into owned payloads, not only holders with a
container predicate. Quantified consumption retires their input snapshots,
initialized bytes, termination witnesses and positive release evidence. A
separate may-invalid lifetime mark follows pointer copies and joins until an
actual new value is installed. It does not alter ordinary ownership records or
claim full consumption of a derived source, so ordinary leak obligations remain.

### 2. Inference and transfer in Analysis

Discover candidate link fields from actual record operations and repeated
cursor updates. A candidate alone supplies no proof. Generic entry predicates
are sufficient requirements and must remain in the exported contract whenever
used. A caller establishes them from its own constructed graph or from another
checked helper's guaranteed postcondition.

Use dedicated Analysis components for shape discovery, transfer, loop induction
and call composition. Interpret evaluated CFG operations, including local
declarations, pointer copies, field reads/writes, allocation/release, branch
tests, calls and returns. Preserve source operation ordering. Unsupported
operations invalidate affected evidence or produce an incomplete obligation.
The existing dataflow remains authoritative for arithmetic and other operations.
An empty-chain certificate captured before an assignment must not overwrite
the ordinary assignment's resulting nullness. If that result is proved non-null,
discard a conflicting captured empty certificate and its separation evidence.

Initialize node fields from actual stores, initializers and modeled complete
zero/copy operations. Publishing a fresh node can fold it into a chain only
after all required fields and its successor relation are established. A null
allocation outcome creates no node. Overwriting a link retires predicates that
depend on the old value; a supported relink may establish a new predicate.

The first supported transformations are traversal/count/search, prepend,
append/concatenation of disjoint segments, head removal/detachment, reversal,
and complete node/payload cleanup. They are recognized by operations and
validated dataflow, not by function, type or field names. Ordinary wrappers and
output parameters compose under the same contract rules.

### 3. Inductive loops and ownership

At a supported loop header, distinguish processed segments, the current node
and the remaining chain. Prove initialization and preservation on every entry
and back edge. Use bounded symbolic states for the invariant, not a concrete
iteration count. Construction and reverse loops fold completed nodes; traversal
unfolds the next node; cleanup consumes it after saving a live successor.

Conditional exits export only facts guaranteed on their actual outcomes. A
break or early return must not imply that the whole input was traversed or
destroyed. An internal goto or alternate entry participates in the same CFG
proof; if its invariant cannot be established the result is incomplete.

Separation describes ownership of storage, not mere pointer inequality. Saved
aliases may exist while a chain is borrowed. Mutating or destroying the chain
must preserve their ordinary lifetime/release obligations. Detachment transfers
ownership of the detached node without releasing it. Payload destruction must
use its actual release contract, not a convention based on field names.

### 4. Checked contracts and calls

Add portable container entry requirements and established output facts to the
checked contract. Include enough shape, endpoint, mutation and ownership
information to distinguish borrowed traversal, preserved/returned segments and
consumed nodes. Reuse interface paths and immutable call-entry snapshots.
Shape requirements cannot be discharged using may-alias or may-initialized facts.

Apply entry requirements before effects. Install guaranteed outputs after
effects and only for the supported return outcomes. Multiple possible callees
require the union of input requirements and intersection of guaranteed outputs.
Unresolved callbacks and incomplete ordinary callee semantics remain visible.
Container facts never upgrade an otherwise incomplete callee to complete.
Resolved callbacks apply quantified invalidation independently for every
returning target before joining states. Invalidated lifetime marks combine by
union; a target that frees only the head cannot preserve an alias consumed by
another target. This applies to singleton resolved function pointers as well.
Dropping indirect container output predicates alone is insufficient: native
byte-pointer evidence into an owned payload must also be retired.

`container-separated` is a distinct checked requirement on two chain paths;
it quantifies over their node and owned-payload footprints. The existing
`separated` byte-object requirement is insufficient. Output predicates may
carry this relation as well. A call conservatively retires aliases of writable
or releasable chain inputs before installing its guaranteed outputs. Borrowed
traversal preserves its inputs. A postcondition never silently promises that an
input was completely consumed: complete consumption follows only from the
ordinary release effects and the proved control flow.

Output records distinguish an ordinary `container`, a `container-derived`
predicate whose non-fresh nodes come from the named call-entry chain, and a
`container-fresh` predicate whose entire owned footprint was allocated during
the call. Derived outputs preserve a stronger caller capability only when all
additional nodes have the corresponding allocation capability. Fresh outputs
establish separation from pre-existing chains. These are checked postconditions,
not admissible entry assumptions; allocation-site spelling does not prove them.
Several outputs from one invocation can alias despite each being fresh. Their
mutual separation needs an explicit proved relation, not repeated application
of freshness. Reusing a call-region identity across loop iterations may lose
fresh separation conservatively. Likewise, matching an abstract folded storage
identity cannot establish that an arbitrary byte alias belongs to a preserved
chain; preservation needs an exact explicit graph or a covered input path.
The derived record carries up to three source paths: `other`, then optional
unit-coefficient, zero-offset paths in `begin` and `end`. All are conjunctive
premises and use the existing strict path remapper. More sources lose the
capability refinement. A terminal-head descriptor refines a chain to at most
one node; it proves a null link after detachment and is weakened at joins when
an incoming nonempty chain lacks that fact. Entry witnesses retain their own
immutable descriptors when an output is strengthened or relinked.
Capability requirements can propagate backward through derived outputs: a
wrapper that destroys a reversed input needs ownership of that original input.
This adds explicit entry premises; it cannot manufacture ownership of borrowed
local storage. `container-tail` records an output that is a saved successor of
an input head. The initial consumer preserves this head/tail relationship across
calls with no mutation or consumption effects; other calls retain the ordinary
conservative invalidation rule.

The derived relation is a subset guarantee, not conservation of every input
node. It can carry a sufficient ownership premise through
`destroy(reverse(p))`, but it cannot by itself certify that this wrapper
consumes the entire original footprint. If the ordinary release summary loses
that relation, a closed owning caller can still report a leak. Inferring a
whole-footprint consumption postcondition is deferred; suppressing that leak
from the derived relation alone would also accept a wrapper that drops nodes.

Generic summaries describe valid entry shapes. Closed callers must construct
or establish those shapes; they cannot obtain a proof by exporting assumptions
about private local storage. Recursive inference may use a candidate invariant
only after its defining transfer preserves it; an active specialization is not
an independently established recursive proof.

### 5. Reports, transport and caching

Represent container obligations explicitly in reports, including the needed
shape, ownership capability and missing property. Use existing checked outcome
categories. Keep reasons and source routes stable through helper propagation.

Bump summary format 17 to 18, sidecar format 18 to 19 and checked encoding 4
to 5 when container records land. Readers reject malformed descriptors,
oversized tables, unsupported versions, invalid interface paths, lost endpoints
and incomplete remapping. Never drop a premise to make a record portable.
Persistent checkpoints depend on all observed shape and callee facts and retain
canonical equality across uncached, cold and warm runs. Old objects must rebuild.

### 6. Frozen acceptance population

Before checker edits, create `test/evaluation/rfc0023/` with a manifest and
SHA-256 inventory. Freeze positive/negative pairs covering empty/singleton and
runtime construction, traversal/search, prepend, concatenation, detach, reverse,
cleanup, owned payloads, saved aliases, allocation failure and invalid links.
Keep inline/helper and separate-source/compiler-object populations identifiable.
Preserve baseline reports including existing misses and false positives.

Positive closed callers require zero entry assumptions, no limit or deferral,
and no unsafe or annotation trust. Library trust is explicit. Negatives must
report the intended missing safety property; syntax errors, crashes, tool
failures and timeouts never satisfy an expectation. Include renamed field/type
variants and branches that almost satisfy each supported invariant.
Compiler cases record whether an intended checked rejection happens during
source compilation or linking. An early checked violation with a matching
report is a valid rejection, but is not counted as a successful link test.
An ordinary hard use-after-free error in an unselected helper can also prevent
object generation; that case requires the stable diagnostic id and a matching
computed checked violation, and is recorded separately from selected checking.

Freeze actual cJSON `cJSON_GetArraySize`, both `get_array_item` definitions and
their public forwarding operations from the existing pinned corpus, with
unchanged definitions and concrete closed callers. These are traversal targets,
not a promise to prove the whole library or its recursive destructor. Any extra
real-source target is recorded separately from the frozen population.

### 7. Independent validation and performance

Add a deterministic small-heap reference model that independently enumerates
live nodes, successor edges, payload ownership and effects. Compare abstract
acceptance against concrete states and mutation sequences. Generate valid and
invalid C clients from reviewed cases and test inline/helper/cross-source forms.
Reduce any mismatch to a permanent regression. The reference oracle must not
call the abstract transfer implementation to decide expected safety.

Run complete Debug and ASan/UBSan suites, strict syntax checks for fixtures,
format and relevant clang-tidy checks, old evaluation populations, transport and
cache invalidation tests. Freeze and compare exact identities for all 124
baseline-complete selected corpus definitions. An independently demonstrated
baseline false proof may be corrected only with the original case, a reduced
counterexample and an explicit recorded amendment; counts cannot conceal losses.

Measure the same five pinned projects, with the baseline and final Release
binaries using matching compiler/options. Three sequential ordinary observations
must have median total runtime and peak RSS ratios at most 1.10. Checked reports
must finish within 600 seconds per project. All 51 unchanged warm units must
reuse checkpoints with zero function analyses, preserving canonical reports and
diagnostic sets. Measure constructed-list families to ensure proof work does not
scale by unrolling runtime lengths. Publish all outcomes and remaining limits in
`docs/validation-rfc0023.md` and a machine-readable results artifact.

## Annotation surface

None. Inference and exported requirements carry the supported predicates.
Existing annotations retain their meaning and explicit trust provenance.

## Diagnostics

Use existing `checking-incomplete` for missing shape, initialization, ownership,
separation or unsupported transfer; `checking-failed` for demonstrated safety
violations. Existing temporal, invalid-release and leak identifiers retain their
meaning. Container explanations identify the operation and required property;
unit and RFC-numbered lit cases pin them. No new ordinary diagnostic identifier
is introduced by this design.

## Drawbacks

Inductive shape reasoning adds a second abstraction of heap connectivity that
must agree with concrete aliases, checked storage and ordinary release effects.
Its benefits depend on conservative invalidation and genuinely independent
validation. Bounded shapes still reject valid graph algorithms. Recursive C
types are widespread, so candidate discovery must avoid paying the full cost
for functions without relevant operations. The implementation size is a budget,
not an acceptance criterion.

## Alternatives

Increasing heap-path or loop limits proves only larger fixed examples and adds
cost without induction. Name-based list summaries assume undocumented library
semantics. An unrestricted predicate language or general-purpose solver would
expand the trust, annotation and performance surface substantially. Modeling
more libc calls is useful separate work but does not justify the next node of a
list. Keeping all such operations incomplete remains sound but does not meet
the README's practical source-compatibility goal.

## Prior art

RFC 0013 supplies object identity and finite heap outputs; RFC 0015 supplies
selected ownership and range effects; RFC 0016 preserves operation order under
caller alias relationships; RFCs 0018/0019 separate sufficient contracts from
bug witnesses; RFC 0021 validates inductive buffer traversal; RFC 0022 preserves
object views and callback alternatives. This RFC applies the analogous
head-and-remainder induction to linked storage, retaining those RFCs' explicit
unknown alternatives, transport discipline and independent validation.

## Unresolved questions

No user-facing semantic decision is deferred. Internal canonicalization and
candidate scheduling may change with profiling while preserving the predicate,
proof obligations, frozen cases and cost gates. Changes to supported shapes or
guarantees require an explicit amendment before implementation.

## Future work

Recursive trees and arbitrary graphs, inductive doubly linked mutation,
region/arena ownership, tracing collectors, general user-defined predicates,
concurrency, tagged object representations and additional library contracts.
