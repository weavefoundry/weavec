# RFC 0027: Recursive object ownership and complete cleanup contracts

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-13
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFCs 0013, 0018–0019 and 0023;
  supersedes RFC 0023's restriction of derived outputs to subset information
  when a separate conservation proof is available. Preserves RFCs 0020,
  0022, 0025 and 0026's input validation, callbacks, cases and buffer rules.

## Summary

Infer reusable checked contracts for finite recursive C objects and account for
their complete allocation footprints through construction, traversal, attachment,
detachment, relinking and destruction. Distinguish a structurally valid object
from a proof that every owned allocation is preserved, transferred or released.
Carry both kinds of evidence through helpers, recursive calls, translation units,
compiler objects and validated checkpoints. Add no annotation or runtime check.

The project owner requested this RFC first, followed by implementation end to end.
Acceptance records that authorization; it does not claim independent review or
an RFC-only merge. Changes to the decisions below must be recorded before the
corresponding implementation, with failed acceptance observations retained.

## Motivation

The v0.7.0 baseline (`ad26255`, implementation `00575b9`) detects all 44 original
fixed bugs and accepts all 32 clean counterparts. Its broad checked population
contains 148 complete conditional contracts among 1,619 selected definitions.
The validated Release executable has SHA-256
`a97c924a61dcb3cc16f03cc2efa84966934b1253b715fa6944fd73cfb2f704bd`.

The following safe clients still fail checked mode:

```c
struct node { unsigned value; struct node *left, *right; };
static unsigned total(const struct node *p) {
    if (!p) return 0;
    return p->value + total(p->left) + total(p->right);
}
int main(void) {
    struct node a = {1, 0, 0}, b = {2, 0, 0}, root = {3, &a, &b};
    return total(&root) != 6;
}
```

A recursive destructor fails even for a single initialized allocated node.
The existing reversal-wrapper unit test rejects a caller that explicitly
allocates and initializes its list node before calling
`dispose(p) { destroy(reverse(p)); }`. RFC 0023 correctly refuses to infer
complete consumption from a subset guarantee: another transformation could
drop a node and really leak it. The frozen nested-call spelling
`dispose(build(n))` already passes the baseline; that positive is preservation,
not new coverage, and its dropped-head counterpart exposes the false proof below.

The frozen `reverse-drop-bad` client confirms a baseline false proof: the
baseline accepts `dispose(build(2))` even when `dispose` first discards the
head. RFC 0027 intentionally removes that result. The executable, source
inventory and baseline report retain this counterexample; it is not a valid
contract to preserve. Existing corpus identities remain a separate frozen gate.

Unchanged cJSON clients that establish default allocation hooks, create an empty
object, and delete it also remain incomplete. Child/sibling recursion, ownership
flags, callback release effects and complete cleanup must compose. The target
is a bounded lifecycle slice, not certification of the parser, printer or library.

## Soundness

Retain RFC 0018's conditional, single-threaded source guarantee. A selected
operation needs positive evidence, a sufficient exported entry requirement, or
an explicitly recorded trust boundary. No source shape, field name, allocation
site, callback type or recursive declaration supplies that evidence by itself.

The supported structural family is a finite acyclic ownership forest. A node
has a compatible initialized record, a finite set of recursive successor fields,
and optionally separately owned allocation payloads. Nonowning backlinks may
form pointer cycles but are not followed as ownership edges. Borrowed traversal
can use initialized local or static nodes. Releasing a node additionally requires
allocation-base identity, a matching release family and permission to consume it.

Supported scalar flag conditions can make an edge or payload nonowning. The
condition is part of the predicate and depends on an initialized field. Inactive
edges grant no validity, initialization or release permission for their pointees.
Changing a controlling field invalidates dependent facts. A flag never creates
an allocation, an initialized payload or a release capability.

An owned footprint denotes actual allocation identities, not an integer node
count. Equal allocation counts do not prove equal footprints. Separate dynamic
allocations at one allocation site remain distinct. Copies retain identity;
they do not create ownership shares. Saved aliases into released nodes or payloads
remain invalid through copies, joins and subsequent allocations.

Required distinctions include:

- Empty, singleton and runtime-sized valid structures versus unknown, dead,
  uninitialized, cyclic or overlapping owned children.
- Borrowed backlinks and reference payloads versus owned children and payloads.
- Structurally decreasing recursive calls versus recursion on the unchanged
  object, an unrelated pointer or an unproved child.
- Complete destruction versus a skipped child, early return or dropped node.
- Ownership-preserving reverse/attachment versus transformations that lose a
  node, duplicate ownership or introduce an owning cycle.
- Detachment transferring a live object versus returning a released alias.
- Failure cleanup that releases exactly acquired resources versus leaks,
  double cleanup or publication of a partially initialized object.
- An independent object preserved by mutation versus a saved descendant alias
  whose lifetime ends during recursive cleanup.

General graphs, shared ownership protocols, tracing collectors, concurrency,
signals, nonlocal control transfer, representation punning and arbitrary logical
predicates remain outside this milestone. An unsupported case stays incomplete;
it cannot become complete by truncating an obligation, raising a depth bound or
assuming that an active recursive analysis already proved its own outputs.

## Detailed design

### 1. Structural predicates

Extend the Clang-free container representation with recursive successor fields
and optional initialized-scalar ownership conditions. Preserve existing chain
predicates and their meaning. A recursive descriptor includes canonical target
record identity, field byte ranges, initialized fields, recursive links,
node/payload capabilities and release families. Conditions use represented
target-width integer tests; unsupported conditions prevent the stronger proof.

Discover candidates from evaluated field accesses, cursor updates and calls on
subobjects, including recursive calls. A candidate nominates an invariant; it
does not establish one. Borrowed fields do not become recursive ownership edges
merely because they point to the same record type. Discovery and immutable
layout work are reused within an AST and bounded independently of runtime size.

Establish predicates from actual local graph facts or complete helper outputs.
Unfolding exposes a live head and independently separated child footprints.
Folding requires all relevant fields, children and ownership conditions to be
established. Unknown writes, unresolved callbacks and lifetime ends retire the
affected evidence. Checked access still requires the actual non-null branch.

Use at most 16 recursive descriptors per function, 32 descriptor fields and
64 represented nodes/facts. Descriptors are at most 16 KiB. The representation
can describe an unbounded runtime number of nodes inductively; those limits
bound analysis metadata, not a concrete unrolling used as a proof.

### 2. Complete footprint conservation

Add an independent must-domain for equalities between allocation footprints.
Its terms name immutable entry footprints, current pointer footprints, individual
node/payload allocations and the accumulated released footprint. A valid split
states that the parent's footprint is the disjoint union of its head, active
owned payloads and child footprints. A null pointer has the empty footprint.

Pointer copies preserve footprint equality. A supported link update replaces
the old child contribution with the proved new child contribution. An ownership
transfer moves a contribution between roots. Release consumes exactly the
proved contribution. Unknown mutation forgets dependent equalities; ordinary
may-alias information cannot introduce a must equality or disjointness fact.

Use bounded sparse linear equalities over formal allocation identities, not
numeric sizes. Elimination, assignment and joins preserve only equalities true
on all incoming paths. Exact arithmetic must detect overflow; exhaustion loses
proof and remains visible. Cap the domain at 64 variables and 64 independent
relations per relevant function state. The analysis need not enumerate actual
heap members to retain an invariant such as
`footprint(processed) + footprint(remaining) = footprint(input)`.

If an existing contract bound prevents retaining a container premise or a
separation guarantee, retire every output that depends on that missing fact
before publishing the contract. Keep the limit flag and all other represented
facts. The producer and strict decoder use the same premise checks; decoding
must still reject a dangling relation. This also applies after output joins.
The Lua checkpoint gate exposed the existing failure mode at the unchanged
256-requirement bound in `finishbinexpneg`.

Conservation and structural separation are both required. Algebraic cancellation
alone cannot justify overlapping ownership, releasing a borrowed allocation or
restoring validity. Concrete resource accounting remains authoritative where
available. A complete-consumption fact can settle an ordinary leak obligation
only for the actual covered input footprint, after its preconditions hold.
Unrelated owned children and resources remain obligations.

An entry release capability grants permission; it does not implicitly transfer
the caller's entire cleanup duty. A helper that releases only a head can have a
complete conditional safety contract without a complete-consumption output.
When no allocation is acquired locally, unconsumed input allocations therefore
need not make that helper incomplete. Its callers retain their allocation ledger
and cannot settle it using that partial helper. This distinction preserves the
frozen RFC 0023 callback audit's safe replacement of a saved pointer after a
head-or-chain release. Recursive candidates promising consumption still must
prove consumption of the entire input on every exit.

At a loop header, entry and every back edge must establish the same invariant.
At an exit, a known empty remainder can establish full transfer or release.
An early exit with a nonempty remainder cannot establish complete consumption.
This proves wrappers around transformations as well as inline transformations.

### 3. Recursive proof rules

Check recursive traversal and cleanup against structural induction. A proposed
contract names the required input predicate and proposed output/consumption
relation. Recursive hypotheses are usable only for established proper child
footprints of the current input, under the appropriate live-node branch.
Each body must verify all local operations and its promised result on every
returning path. Nullable base cases and failure branches participate explicitly.

The proof must distinguish an induction hypothesis from a published complete
contract. Unverified candidates never escape through the program database,
callback specialization, sidecar, report or checkpoint. Mutually dependent
candidate contracts are accepted together only when each body discharges its
local obligations and every recursive edge meets the structural rule; otherwise
the affected candidates remain incomplete. Ordinary recursive may-effects keep
their existing conservative behavior.

A recursive release of one child must preserve the head and separated siblings
while invalidating every represented alias into the released child. The final
head release requires all owned contributions to be transferred or consumed.
Rechecking a generic helper under a concrete case may refine scalar/flag premises,
but bounded concrete cases cannot substitute for a runtime recursive invariant.

The initial cleanup rule supports direct self recursion in a one-pointer,
void-returning destructor, including resolved release callbacks and local saved
cursors. Mutual recursive groups and destructors with additional side effects
remain incomplete unless a separately verified ordinary contract suffices. No
unverified hypothesis is published for those unsupported candidates.

### 4. Helpers, callbacks and ownership flags

Export sufficient recursive shape requirements and proved footprint relations:
complete preservation/transfer, separated output partitions and complete
consumption. Outputs are conditional on their actual return outcomes and entry
premises. Snapshot inputs before applying effects. Install outputs only after
all preconditions and ordinary required semantics are established.

Reversal, attachment and detachment must retain the whole input relation where
proved, while existing subset-only outputs remain valid weaker information.
Fresh outputs do not imply that two outputs are separate unless that relation
was proved. Release through a resolved callback uses its actual contract and
family; default allocator hooks must be established by actual program state.
Unknown targets remain incomplete. Multiple targets require all their premises
and only their common guaranteed outputs.

The cJSON lifecycle population exercises its existing initialization, allocation
hooks, ownership flags, child/sibling links and nonowning previous links. No
function-name special case or hand-authored trusted cJSON summary is permitted.
Unchanged source definitions and closed clients with established default hooks
are required. Generic supported recursive helpers must also have usable
conditional contracts independently of those clients.

### 5. Transport and reports

Bump summary format 21 to 22, sidecar format 22 to 23 and checked encoding 8
to 9. Validate recursive descriptors, scalar conditions, relation kinds and
all interface paths. Reject missing premises, malformed layouts, contradictory
capabilities, output facts in entry positions, noncanonical encodings and
incomplete remapping. Old compiler objects must rebuild.

Keep existing expanded/compact JSON meanings. Reports identify recursive shape,
separation and footprint obligations and their actual premises. Unproved
candidate induction is not a complete contract. Cache reuse depends on all
observed layouts, bodies, target bindings and imported contract facts, retaining
RFC 0020's source/header/command/object validation and conservative misses.

### 6. Frozen acceptance populations

Before checker changes, preserve the baseline executable and create manifests
with source SHA-256 inventories under `test/evaluation/rfc0027/`. Record baseline
results, including failures. Freeze these distinct populations:

- Primary positive/negative pairs: borrowed tree traversal, singleton and
  runtime construction/cleanup, multiple children, payloads, saved aliases,
  allocation failure, reverse-wrapper cleanup, attachment, detachment, dropped
  children, owning cycles, shared owned children and renamed equivalents.
- Generic recursive helper checks with explicit sufficient premises, including
  a nondecreasing recursive counterexample and incomplete cleanup.
- Separate-source and compiler-object variants exercising both successful
  transport and intended checked rejections.
- Unchanged pinned cJSON clients for empty creation/deletion, nested attachment
  and cleanup, traversal, detachment with separate cleanup, borrowed reference
  nodes and adversarial stale aliases or duplicated ownership. Keep creation,
  mutation and destruction requirements in the selected workflows; constructing
  only stack fixtures does not satisfy the lifecycle target.
- A deterministic independent small-heap oracle, using concrete reachability,
  ownership and released allocation sets to generate valid/invalid clients.

Implementation provenance amendment: the 27 primary cases and seven upstream
clients were frozen before checker changes. The twenty separate-source cases
were extracted mechanically from those frozen clients during implementation;
their compiler-object variants use the same files. The independent oracle's
43 clients were generated during implementation, then frozen before their first
checker run. They are additional validation, not a blind preimplementation
population. Their generator and source inventories are retained; no expected
result was changed in response to checker output. Development regressions added
to unit tests remain separate from all three frozen denominators.

Positive closed clients require zero unexplained entry requirements, no
deferral/limit and no new unsafe or annotation trust. Existing allocator/libc
trust remains explicit. Negatives must fail for the intended property. Parse
errors, crashes, missing reports and timeouts never satisfy an expectation.
The oracle must not call the abstract-domain implementation to decide truth.

Freeze preservation of the existing 148 exact complete corpus identities.
Measure new completed lifecycle clients separately from generic function counts.
A demonstrated baseline false proof requires a retained counterexample and
explicit RFC amendment; a changed denominator cannot conceal lost coverage.

### 7. Validation and cost

Run full Debug and ASan/UBSan suites, existing fixed evaluations, new source,
transport, object and oracle populations, and cache invalidation/equivalence
cases. Validate fixture syntax, formatting, the Core include boundary and
strict clang-tidy for changed C++ translation units. Preserve failed development
observations alongside the final evidence.

Compare matching baseline/final Release builds on the same five pinned projects.
Three sequential ordinary observations must have median total runtime and peak
RSS ratios at most 1.10. Every checked project must finish within the existing
600-second limit; the scope and semantic budgets must not be weakened to meet
it. Unchanged warm replay must reuse all 51 units with zero function analyses
and equivalent contracts, reports and diagnostics. Measure work on runtime-size
families to show that recursive proof is not concrete heap unrolling.

Use the normal Release preset's link-time optimization setting for the final
cost comparison, rebuilding both the unchanged baseline revision and the final
source with matching compiler, SDK and optimization flags. Retain the earlier
non-LTO executables and measurements separately; do not mix their observations
into the new medians. The frozen baseline identities and every acceptance limit
remain unchanged.

Publish `docs/validation-rfc0027.md` (removed by RFC 0030) and machine-readable
evidence. The RFC moves to Implemented only when the detailed design and
acceptance gates are complete.

The final cost investigation may optimize alias queries without changing their
results: reuse the original place when its parent has no relevant alias
expansion, retain expansion order and depth checks for other parents, and borrow
immutable alias-edge views while the queried relation cannot change. These
changes preserve the relation and all generated alias identities.
Mirror-result lists may keep four place identifiers inline to avoid allocation
for the measured common small result. Larger lists must grow normally, retaining
all places, order and duplicate handling. Inline capacity is a storage choice,
not a new proof bound or a reason to truncate recursive expansion.
An internal resolved-place reference may store each dereferenced pointer,
source expression and element witness together, with two entries inline, in
place of three parallel allocated vectors. Preserve every triple and its order,
including synthesized expressions, deep paths and per-element witnesses. Larger
paths grow normally. This representation remains confined to Analysis.
Cache a successful individual object-view comparison between an immutable AST
type and a view owned by the same live immutable callee summary. Index by the
exact qualified type and the expected view's address; a weak summary owner
prevents reuse after its address is recycled. Walk every summary-path step and
check every expected view, including views below a reused prefix. Erased-pointer
recovery remains flow-dependent and uncached, and failures still report at every
normal checking point. Limit this per-function index to 128 comparisons; it owns
no copied paths or layout strings. Exhaustion clears the index and repeats the
comparison without changing evidence. This replaces the earlier whole-path
memo: a late Lua sample recorded 1,057 of 6,970 samples under view validation,
including repeated path comparison, insertion and destruction. The sample was
taken after that run had already failed the 600-second gate and is profiling
evidence, not an uninstrumented acceptance measurement.

Subtree overwrites may batch guard invalidation after the existing per-place
local erasures. Invalidate checked safety dependencies in the original place
order, then remove matching scalar, pointer and integer guard conjuncts in one
pass over each remaining domain. Pending result facts, initialized ranges and
numeric expressions lose exactly the dependencies that sequential invalidation
would remove. Preserve loans against overwritten objects and all other local
erasures. An empty batch changes nothing; duplicates and input order do not
change the result. Compare whole ordinary and checked states against sequential
invalidation, including mixed guards, pending outputs and copied states.

For a crowded call ledger, retain callee-specific rendered origin fragments as
per RFC 0020 instead of repeating their formatting at each caller. Retain the
RFC 0024 complete call-ledger cache for destinations where it applies. Both
representations share the existing 1,024-entry / 64 MiB pool budget. Fragment
identity includes the same live immutable source projection, trusted sequence,
callee and unsafe mode; it excludes caller fields, which remain application
inputs. Weak ownership prevents stale-address reuse. Keep bounded subjects,
escaped subjects, bounded reasons and outcomes in original origin order, with
truncation exhaustion even for entries rejected at capacity. Read routes from
the retained immutable projection, preserving ordinary insertion's outcome and
route choices. Do not retain a second source projection or impose a new semantic
limit. Test fresh, reused, disabled, evicted and byte-rejected preparations
against individual insertion at capacity and after source replacement.
Caller text may itself borrow a destination row that insertion replaces. Keep
the caller name already owned by the lookup key alive across fragment
application; neither a cache miss nor a hit may reread invalidated caller text.
Test both paths with a full destination whose first updated row owns that name,
including heap-backed names and locations borrowed from the same row.

These two changes follow a candidate 11 Lua sample collected only after the
cold run had failed its 600-second gate. Among 6,805 main-thread samples, 727
included subtree forgetting, 672 included guard invalidation, and 637 included
call-origin application. These overlapping counts guide implementation and do
not constitute an uninstrumented speed comparison or an acceptance result.

Retain RFC 0020's value-owned, ordered alias adjacency. A whole-relation
copy-on-write hash-index experiment passed the independent alias model and
frozen correctness populations but failed the checked Lua timing gate at
785.956 seconds. Alternating cJSON observations had a median runtime ratio of
1.0166 versus the preceding value-owned build, so that experiment is withdrawn.
Retain its source snapshot and before/after observations in the validation
record. Copy isolation, directional witnesses, offset joins, same-share
disjunction, alternative-copy semantics, exact intersection and ordered queries
continue to follow the existing alias model.

Checked transport may memoize rendered guards within one contract write and
its fixed global-name mapping. Bound this cache to 64 guards and 1 MiB of
rendered text; exhaustion only repeats formatting. Preserve identical encoded
bytes, all encoder bounds, and every decoder and producer-round-trip check.
The reader may similarly reuse successfully decoded identical guard text within
one contract and its fixed global resolver, with the same cache bounds. Validate
each field's framing before lookup, never cache failures, and retain every
requirement-kind and whole-contract check. Cached keys borrow only the reader's
immutable decoded input; they cannot outlive it or cross global namespaces.

## Annotation surface

None. Existing annotations retain their meanings and trust provenance.

## Diagnostics

Use existing `checking-incomplete`, `checking-failed`, `analysis-incomplete`,
`leak`, `use-after-free`, `double-free` and `invalid-release` identifiers with
their current severities and meanings. Missing structural induction,
initialization, separation or complete-footprint evidence supplies a checked
obligation. A failed proof is not a claim of a concrete execution bug. Add unit
tests and RFC-numbered lit tests pinning representative messages and call notes.

## Drawbacks

Recursive structure and conservation add state that must agree with ordinary
aliases, resources and checked memory. Exact relational joins and recursive
candidates are implementation risks. The narrow checked-Lua timing margin makes
profiling and avoiding unrelated work necessary. Finite ownership forests still
reject valid shared or cyclic algorithms. Implementation size is not a gate.

## Alternatives

Larger path/iteration budgets establish larger examples without a recursive
proof. Suppressing leaks after any derived output would accept transformations
that lose allocations. Library-name summaries would bypass the unchanged-source
goal. An unrestricted predicate language adds annotation and solver design work
beyond this milestone. Scaling the engine or packaging archives first remains
useful separate work but does not address the demonstrated lifecycle failures.

## Prior art

RFC 0023's head/remainder predicates supply structural separation and alias
invalidation. RFC 0013 supplies allocation identity; RFCs 0018–0019 distinguish
entry premises from guaranteed outputs; RFC 0020 separates convergence from
explanation identity and validates reuse. RFCs 0022/0025 supply actual callback
bindings and input cases. This RFC extends those internal designs with proper
subobject induction and a separate complete-footprint conservation proof.

## Unresolved questions

The semantic boundary and required workflows are fixed above. Internal sparse
relation canonicalization, descriptor caching and candidate scheduling may be
refined through implementation and profiling. Any change to supported ownership,
induction rules, frozen expectations or cost gates requires an explicit recorded
amendment before the corresponding code change.

## Future work

Shared recursive ownership, tracing collectors, arbitrary graphs, concurrent
containers, general predicate annotations, broader callback contract languages,
archive packaging and parser/printer verification.
