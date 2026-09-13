# RFC 0025: Case-sensitive checked contracts and discriminated C objects

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-12
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFCs 0016 and 0019's caller
  contexts, RFC 0018's checked operation inventory, and RFC 0022's object
  views. Amends the blanket exclusion of union storage in RFC 0018.

## Summary

Check a helper under established input cases, including read-only helpers,
and retain the conditions of that proof through calls and compilation.
Recognize a bounded subset of discriminated C objects, including scalar and
pointer union members. Track the member actually initialized, invalidate
overlapping storage facts on replacement, and require callers to establish
the member a callee reads. A tag is a branch selector, never evidence that
its payload is initialized, live, correctly typed, or owned.

The owner requested this RFC first and then implementation end to end.
Acceptance after drafting records that authorization; it does not imply an
independent review or a merged RFC pull request.

Implementation and acceptance evidence are recorded in the
[validation document](../validation-rfc0025.md).

## Motivation

At `c04a43b`, this closed caller is incomplete:

```c
static int read_if(int enabled, const char *p) {
    if (!enabled) return 0;
    return 100 / *p;
}
int main(void) { return read_if(0, 0); }
```

The equivalent inline code passes. The helper's possible division error is
propagated despite the caller's established zero argument. Memory-context
capture currently declines read-only helpers, and the generic obligation
ledger cannot describe which input cases execute its unresolved operations.

The frozen RFC 0024 cJSON string comparison client has the same practical
problem. Both tags name strings, but the caller inherits recursive array and
object requirements from `cJSON_Compare`. Simple guarded pointer requirements
already work; adding a second unconditional requirement mechanism would not
address this gap. Union storage is also rejected wholesale, including a
locally initialized integer member that is read without reinterpretation.

The last published corpus completes 142 of 1,619 selected conditional
contracts. Lua takes 595.754 seconds and its expanded report is about 2.64 GB.
Input-case inference must reuse bounded infrastructure and retain explicit
failure at limits, rather than specialize every possible integer value or
duplicate complete explanation trees.

## Soundness

### Guarantee and assumptions

Retain RFC 0018's conditional, single-threaded source guarantee and target C
semantics. A proof case consists of established entry predicates and the
contract obtained by checking the actual CFG with those predicates. The
case's operations, requirements, outputs, trust and incomplete reasons remain
together. Only callers that establish every case premise may use its result.
The generic definition remains incomplete if an unproved case is reachable
under its unrestricted entry contract. Selecting the generic definition must
not silently select only its successful callers.

Case discovery supplies candidates, not proof. Scalar/tag facts do not supply
storage validity, initialized bytes, capacity, alignment, writability or
ownership. Null facts retain the existing distinction between a null pointer
value and valid pointee storage. Unknown aliases and unknown call targets stay
unknown. A complete case never overrides an independently selected generic
definition's failure. Existing annotation and library trust stays visible.

### Required positive and negative distinctions

- A known zero selector avoids an unexecuted read or division; a possibly
  nonzero selector retains the reachable obligations.
- A read-only tagged helper selects the established variant; an unknown tag,
  changed tag, or helper that overwrites the selector cannot reuse an old case.
- A written integer or pointer union member can be read as that member;
  an unwritten member or incompatible member view supplies no proof.
- An initialized pointer member can still be null, dangling, too short,
  borrowed, or point to uninitialized memory. Those checks remain independent.
- Changing a tag without writing its payload establishes no union member.
- Overwriting union storage invalidates earlier overlapping member values,
  including scalar conditions and pointer holders. A separately saved pointer
  retains its original pointee identity and still becomes invalid on release.
- Branch joins preserve member evidence only when justified on all applicable
  incoming paths. Different variants may be retained as bounded alternatives
  tied to stable path conditions; an unknown alternative cannot disappear.
- A complete object copy preserves represented member evidence and pointer
  identity. Partial byte copies, unknown writes and unsupported views retire
  evidence instead of manufacturing a member value.
- A helper's output member is established only after checking its input
  premises and applying its effects, on the recorded returning outcome.

### Conservative limits

The supported union model initially covers named, complete unions with scalar
or pointer members, including such unions embedded in ordinary records.
Nested aggregate/array members, bit-fields, anonymous member promotion,
volatile/atomic storage, arbitrary representation punning, common-initial-
sequence reinterpretation and encoded pointers remain incomplete unless an
existing explicit unsafe boundary applies. This restriction does not claim
that other union uses are invalid C.

Uninitialized or unrepresented case inputs, missing definitions, recursive
specializations without a settled proof, and exhausted budgets retain the
generic result and explicit incompleteness where required. A missing case is
never an empty or pure contract. General recursive trees, collector protocols,
growable-buffer invariant inference and concurrency remain separate work.

## Detailed design

### 1. Bounded proof cases

Use RFC 0016's canonical `CallContext` as the entry predicate of a proof case,
and its separately inferred `FunctionSummary` as the result. This represents
conditions on completeness by retaining an entire conditional ledger, rather
than weakening or filtering individual unguarded rows from a generic ledger.
The original CFG supplies statement order and reachability. An unresolved
operation disappears only when rechecking proves its block unreachable under
the case premises. No caller-side suppression based solely on source syntax
or a callee's spelling is allowed.

Extend checked context discovery to read-only callees. Discover relevant
scalar/tag input paths from control expressions and summary dependencies,
including integer parameters, record fields, pointer nullness and masked tag
tests. Export a bounded input inventory when the ordinary may-effect summary
cannot describe a relevant selector. This inventory requests established
caller facts; it is neither an entry requirement nor an output guarantee.
Propagate relevant inputs through forwarding helpers using strict path
translation. Failed optional discovery may reduce precision, but cannot turn
an incomplete generic contract into a complete one.

Prefer an already complete generic contract and its established outputs.
Optional case discovery targets incomplete read-only definitions: a bounded
recheck can lose an inductive output that the generic proof already establishes.
The initial discovery heuristic leaves existing memory-effect specializations
with their previous input eligibility and selection behavior; it does not
expand recursive mutating helpers solely to obtain new checked cases. Read-only
eligibility consults all may-write effects, including writes whose values
cannot be represented as stores or numeric outputs.

Only established caller facts enter a case. Input storage reads still generate
their normal validity, initialization and bounds requirements in the callee.
Capture inputs before a call changes them. Reuse the existing exact constants,
target-width scalar ranges, null classes and alias relationships, with at most
64 case facts, 32 pointer paths, 32 contexts per function and depth eight.
Unrepresented input paths cannot supply facts. Preserve generic behavior in
ordinary mode. Statistics distinguish case requests, reuse and declined cases.

Candidate input paths are limited to 64 and RFC 0013's eight path steps. The
limit bounds precision; the checker must still account for all operations.
Dependency invalidation includes the generic summary, consulted definitions,
selector inventory and all case premises. A case hit contributes its nested
dependencies to its caller, as in RFC 0020.

### 2. Evaluated operation inventory

Separate declaration-level unsupported semantics from operations that execute
at a CFG point. Keep structural exclusions that affect the whole function
unconditional. For supported case checking, account for unsupported evaluated
expressions at reachable CFG points, including expressions visited within a
larger CFG statement. Record the same operation on every applicable path and
retain the weakest outcome. Unevaluated operands and proved unreachable
branches do not execute their operations.

Fail conservatively if the inventory cannot associate an excluded operation
with evaluated control flow. Inline assembly, nonlocal control flow, opaque
language extensions and malformed annotations cannot vanish merely because a
syntax node was absent from Clang's ordinary CFG statement list. Existing
unsafe regions continue to invalidate affected positive evidence.

### 3. Union storage evidence in Core

Add a Clang-free bounded domain for union member witnesses. A portable
descriptor identifies the target union layout, member name, member byte size
and category. All members overlap the union's storage; their place names do
not imply separation. Validate descriptor sizes, alignment, member uniqueness
and bounded encodings. Keep at most 32 member descriptions per union and 64
active union objects per function state. Budget exhaustion loses evidence.

A witness identifies the storage and the member whose value was established
by a complete typed store or compatible complete object copy. Absence is
unknown. Facts join by must-evidence, with bounded guarded alternatives where
the existing path domain can retain every required premise. A guard is tied
to the values at the establishing store, not the future contents of its names.
Unknown or conflicting views cannot establish a witness. The domain supplies
no pointee permission or ownership by itself.

### 4. Analysis transfers and invalidation

Use Clang target layout to discover supported union members. Complete union
initializers establish only their initialized member, including the first
member selected by a valid zero initializer. Ordinary record initialization
recurses into represented union fields; it does not initialize every member.

At a member write, invalidate overlapping sibling values before establishing
the new member. Retire scalar facts, numeric expressions, pointer positions,
initialization, object views, callback bindings and guarded facts that depend
on the overwritten holder. Preserve independently saved pointee identities.
Do not hide an owned resource lost by overwriting its last holder. Resolve
definite storage aliases for strong updates; possible alias writes invalidate
every affected witness. Reads and compound operations require the current
member before reading its value. Taking a member's address alone does not
read its payload or confer initialization.

Existing memory copies carry member evidence only for complete compatible
objects. A raw write into any overlapping byte, an unknown callback effect,
release, or storage lifetime end retires the witness. Ordinary pointer and
resource diagnostics continue to apply after member selection. Keep these
transfers in dedicated Analysis files rather than adding another interpreter.

### 5. Contracts, calls and output state

Add a `union-member` checked requirement on an input union storage path with
its validated member descriptor. Generic helpers may export this sufficient
requirement for the member they read; a tag check only guards its applicability.
Calls discharge it against the actual member witness. A different or unknown
member cannot satisfy it through initialized-byte extent alone.

Carry established members through helper stores, constructor results, record
copies and returned/out-parameter records where existing path projection is
complete. Member postconditions are must-facts, guarded by entry conditions
and returning outcomes. Ordinary heap/pointer facts still transport the
actual member value. A postcondition may not revive consumed storage or infer
the contents of a member that was not written. Unknown effects invalidate
first; guaranteed outputs are installed afterwards.

Case inputs may include represented union member/value information only with
explicit input premises. A callee that operates through a union must retain
its member requirement even under a scalar case. Mixed callback targets must
each satisfy their own requirements; outputs intersect over returning targets.

### 6. Portable records and reports

Bump summary format from 19 to 20, sidecar format from 20 to 21, and checked
record encoding from 6 to 7 for the new selector/member records. Reject old
compiler sidecars with the existing rebuild behavior. Validate limits,
descriptors and every path/global remapping; losing a required premise makes
the contract unusable. The checkpoint format or executable binding prevents
reuse of older semantic records.

Retain the existing generic definition report. Make case premises and case
completion inspectable without describing a complete case as proof of all
inputs to that definition. Preserve deterministic expanded and compact reports
and their canonical equivalence. Case records retain requirements, outputs,
trust, incomplete reasons and source origins through source units, compiler
objects and checkpoints. No report flag changes selected-function failure.

On a call-explanation cache miss, an already crowded destination may use the
existing ordered insertion path instead of constructing an entire temporary
callee ledger. Reuse a cached prepared ledger when available. Both paths must
retain identical origin order, outcomes, truncation, exhaustion and diagnostic
routes under RFCs 0020 and 0024; no obligation or semantic budget changes.

### 7. Frozen acceptance and cost

Before checker changes, freeze positive/negative fixtures and their source
hashes. Include closed read-only helpers, forwarded cases, masked tags,
selector mutation, uninitialized selectors, supported union initialization,
member replacement, tag/payload disagreement, alias writes, saved dangling
pointers, complete/partial copies, branch joins and helper output members.
Exercise direct calls and resolved callback forwarding where supported.

Freeze the existing unchanged cJSON string comparison caller and add scalar
tag clients against the same pinned complete upstream source. Its string case
must become complete with no entry requirements, deferral or limit. Generic
recursive cJSON object comparison may remain incomplete. Add adversarial
counterparts and record unchanged-source identity independently of results.

Test source, separate-unit, compiler-object and cache forms. Positive closed
callers must have zero entry requirements, no deferral/limit, no new unsafe
boundary and a successful checked invocation. Negatives must fail for their
intended property; syntax errors, crashes and timeouts do not satisfy them.
Use independent small-state union-write/join oracles and malformed-record
tests in addition to end-to-end fixtures. Keep all prior evaluations intact.

Run complete Debug and ASan/UBSan suites, strict formatting and changed-file
clang-tidy. Preserve the 142 exact baseline-complete corpus identities unless
a documented counterexample proves a baseline false proof. Publish any such
correction separately, never as lost coverage without explanation.

Measure three sequential ordinary Release runs before and after, with median
time and peak RSS no more than 1.10 times baseline. Run all five checked corpus
projects under the unchanged 600-second process limit. Record time, memory,
report bytes, incomplete/limited functions and case work counters. The Lua
observation may use the existing compact report encoding to bound disk use;
record that choice in every command and compare expanded canonical content
across uncached, cold and warm runs. Keep expanded cold cJSON and linenoise
reports for the existing compact-size reduction checks. Require
uncached/cold/warm canonical report equivalence and positive warm reuse with
zero function analyses. Investigate and fix regressions instead of raising
semantic budgets or timeout limits. Frozen populations and failures remain
visible in a validation document and machine-readable evidence.

## Annotation surface

None. Existing `WEAVEC_CHECKED`, ownership annotations, `WEAVEC_ASSUME` and
`WEAVEC_UNSAFE` retain their meanings. No tag declaration supplies a payload
fact; the implementation infers it from actual operations.

## Diagnostics

Use existing `checking-incomplete` and `checking-failed` identifiers with
specific reasons for missing union-member evidence, unsupported union layout,
and unavailable proof cases. Preserve ordinary invalid-release, lifetime,
initialization, bounds and resource diagnostics. Pin new messages in RFC 0025
lit tests. An unsupported member interpretation is missing checked evidence,
not a claim that all C union reinterpretation is undefined behavior.

## Drawbacks

More contexts can increase work in recursive components. Union storage makes
previously independent field names overlap, so incomplete invalidation is a
soundness risk across every existing fact domain. Conservative alias writes
can discard valid evidence. Portable records change and require rebuilding
compiler objects. These costs justify frozen cases, independent oracles and
explicit performance gates.

## Alternatives

Filtering generic unresolved messages at a caller loses the conditions and
state that produced them. Increasing context budgets does not repair missing
case discovery. Treating every union as raw preserves current behavior but
rejects ordinary discriminated objects. Treating tag tests as member evidence
accepts uninitialized or mismatched payloads. A general symbolic executor or
unrestricted predicate language is a substantially different inference engine.
The chosen design reuses checked CFG transfer and canonical bounded contexts.

## Prior art

RFC 0019 already retains guards on requirements and outputs and uses bounded
scalar contexts for memory-changing helpers. RFC 0022 establishes the pattern
of keeping object type evidence separate from storage permissions and of
checking a known callback even when its generic interface remains incomplete.
RFC 0023 separates candidate discovery from established shape predicates.
This RFC applies those existing design lessons to input cases and overlapping
union member storage, preserving C syntax and ABI.

## Unresolved questions

Concrete data structures and candidate deduplication can change during
implementation while preserving this scope, the limits and acceptance
populations. The measured number of newly complete generic corpus functions
is an observation, not a substitute for the required closed callers. Any
semantic change to the guarantees above requires amending this RFC first.

## Future work

General recursive ownership, relational growable-container invariants,
aggregate union members and C representation punning, asynchronous protocols,
checked archive packaging, and finer-grained persistent analysis reuse.
