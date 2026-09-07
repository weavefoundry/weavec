# RFC 0017: C integer semantics and compositional spatial safety

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-07
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0009's mathematical-integer
  assumptions and scalar guards, RFC 0011's affine extents and requirements,
  RFC 0012's relations and counted fields, RFC 0013's value snapshots,
  RFC 0015's selectors and range interfaces, and RFC 0016's numeric context
  facts. RFC 0005 continues to specify whole-program orchestration.

## Summary

Interpret integer expressions in the C target's types before using their
values to discard paths, apply ownership effects, select array cells, or
compare an access with an allocation. Carry bounded symbolic size expressions
and numeric postconditions across ordinary calls and compiler sidecars.
Recognize variable-length array extents and allocated flexible-array tails.
Distinguish a proved spatial check from a violation or unresolved check.

This is a static-analysis milestone. It adds neither runtime instrumentation
nor a whole-program verification certificate. Existing unsupported alias,
concurrency, collector, arbitrary-byte and unrestricted traversal models remain
outside its guarantee.

The project owner requested drafting this RFC first and then implementing the
recommended milestone end to end. Acceptance records that authorization after
the design is written; it does not claim an independent RFC review or merge.
The implementation and validation are complete in the working tree. The
[validation report](../validation-rfc0017.md) records the passing correctness
checks, exceeded performance targets and remaining corpus false positives.

## Motivation

At baseline `910dc8a`, all 769 CTest entries pass, but this function is quiet
under `--strict-externs -Werror=weavec` on an eight-bit-char target:

```c
#include <stdlib.h>
void example(unsigned n) {
    if (n != 256) return;
    unsigned char k = n;
    int *p = malloc(sizeof *p);
    if (!p) return;
    free(p);
    if (k == 0) *p = 1;
}
```

The conversion produces zero. The last statement executes and uses freed
memory. Replacing `k = n` with `k = 0` makes the checker report correctly.
Similarly, `malloc((unsigned char)n)` can be mistaken for an allocation of
`n` bytes. These are not just missing buffer checks: an incorrect numeric fact
can remove a reachable temporal error from the CFG analysis.

The fixed evaluation retains two size misses:

```c
char *p = malloc(rows * cols);
if (p) { p[rows * cols] = 0; free(p); }

if (n) { char a[n]; a[n] = 0; }
```

A helper also loses requirements that combine bounds, such as a loop bounded
by both `n` and `cap`. The present format cannot express the minimum. Some
requirements conditional on integer orderings are discarded entirely.

The intended result is useful checking of allocation, buffer and container
code without assuming that conversions preserve sign or that arithmetic never
wraps. The work must preserve the ownership checks already implemented.

## Soundness

### Bugs caught

- Reachable null, lifetime, ownership and release errors whose branch depends
  on a narrowing conversion, unsigned wrap, or correctly converted comparison.
- Bounds errors caused by a size conversion, wrapped addition/multiplication,
  or a count whose value differs from the allocation-time value.
- Access one past a represented symbolic product allocation or VLA.
- Access outside the allocated tail of a flexible-array member, with element
  sizes and record layout obtained from the target.
- A caller supplying too little storage for a representable conditional,
  minimum-bound or multivariable requirement, including through wrappers,
  out-parameters, returned objects and separate compiler objects.
- Definite invalid integer operations in supported integer expressions:
  signed overflow, division by zero, signed minimum divided by minus one,
  and invalid shift counts or signed left shifts. These get their own stable
  diagnostic rather than becoming a reason to assume an arbitrary branch.

### Bugs deliberately not caught

This remains a bounded checker, not a decision procedure for C. An unresolved
spatial check does not become proof merely because no error is emitted.
General nonlinear inequalities, arbitrary induction, arbitrary pointer
provenance, unknown code without a trusted contract, unrestricted alias
partitions, GC invariants, data races and byte-encoded pointers remain outside
scope. Unions, dynamic type punning, unbounded expression history and target
integer types wider than 64 bits can remain unresolved, with explicit missing
coverage when their numeric representation is required.

Signed overflow is not modeled as unsigned wrap unless the compilation mode
explicitly gives that operation wrapping semantics. An operation which may
have undefined behavior yields conservative facts and cannot justify pruning
an otherwise reachable path. Only definitely invalid operations receive the
new error. No general-purpose integer lint is introduced for expressions the
checker does not otherwise visit.

`-fwrapv` covers signed addition, subtraction and multiplication; it does not
make negative signed left shifts valid. The C11/C17 signed-left-shift rule is
retained, including rejection of shifting into the sign bit, as discussed in
[WG14 DR 463](https://www.open-std.org/jtc1/sc22/wg14/issues/c11c17/issue0463.html).

### Accepted false positives

Removing the old sign/zero-preservation assumptions can retain effects that
were incorrectly refuted before, increasing temporal diagnostics. Bounded
interval joins, invalidated mutable field facts and unsupported predicates can
also prevent a correct caller's requirement from being discharged. Such cases
remain unresolved or retain existing conservative temporal effects. They are
not repaired by treating missing information as true.

The existing bounds diagnostic policy is retained: established violations and
supported reachable boundary violations are reported. Merely failing to prove
an arbitrary index safe is not, by itself, a new out-of-bounds error.

### Assumptions

Clang supplies target integer widths, signedness, promotions, conversion nodes,
record layout and language options. Core uses only the standard library and
never includes Clang or LLVM. Supported signed conversions follow the target
semantics exposed by Clang; supported ordinary integer targets use two's
complement. `_Bool` conversion is nonzero-to-one, not low-bit truncation.

Annotations and `WEAVEC_ASSUME` remain trusted as specified by previous RFCs.
They are interpreted using the expression's actual types. They must not
implicitly change the type or widen the allocation behind a pointer.

## Detailed design

### 1. Target integer values and abstract ranges in Core

Introduce a small target-integer domain independent of host integer semantics.
A type contains its width (1 through 64), signedness and boolean behavior.
A concrete value contains that type and a masked unsigned bit pattern. A
negative signed value is interpreted explicitly, without overflowing a host
signed integer. Full-width unsigned constants, including `UINT64_MAX`, remain
representable and never become negative `int64_t` constants.

Operations consume already-promoted operands, as Clang's AST does. Implement
conversion, unary negation/complement, addition, subtraction, multiplication,
division/remainder, shifts, bitwise operations and comparisons. Unsigned
arithmetic wraps modulo the result width. Signed arithmetic checks validity
before performing host operations. Invalid operations have no fabricated
numeric result. A definitely invalid operand is not treated as an infeasible
program edge.

Abstract ranges conservatively contain the represented values. They support
intersection on edges, union at joins, conversions, singleton evaluation,
comparison refinement, and bounded arithmetic. Wrap or conversion may require
multiple intervals; bound alternatives and widen to the target's full range
when necessary. Widen changing loop ranges to keep CFG convergence independent
of an integer's width. Exact constants and existing zero/sign classes remain
available as projections for existing consumers.

Type information and nontrivial range facts must survive scalar state joins,
guards, per-outcome facts and contextual entry facts. A mathematical constant
or sign class may only be projected when it is true of every represented
value. In particular, a positive source does not imply a positive narrowed
result. Contradictory ranges can refute a branch only under the existing rules
for trustworthy local or contextual values; arbitrary mutable heap facts do
not acquire stronger authority.

### 2. Typed AST interpretation and integration

Add a dedicated Analysis implementation for numeric interpretation rather
than extending the main dataflow file with another interpreter. Obtain types
from the original expression, retaining implicit casts. Handle integer
literals, scalar reads, supported unary/binary operators, conditional values,
assignments and the existing increment/decrement and atomic-refcount forms.
Opaque operations return unknown and lose any unsupported sign/equality fact.

Use the same numeric interpretation for:

- scalar assignment, truthiness, comparisons and switch edges;
- call argument and outcome-guard translation;
- scalar postconditions and memory-context entry facts;
- array selection and range counts;
- allocation extents, string lengths and spatial requirements;
- count adjustments used by reference ownership.

Facts from a comparison belong to its converted operands. Propagating them
back through a cast, scale or offset requires proof that the transformation
preserves the relevant relationship over the current range. When no such
proof is available, retain a fact on the expression value or keep both edges.
Never attach the converted value's zero/sign class directly to its source.

Likewise, an affine equality such as `j == i + 1` may only be installed when
that particular evaluation cannot wrap or have undefined behavior. Exact
expression identity can be retained even when unsigned arithmetic may wrap;
it does not imply equality to a mathematical affine expansion.

A supported symbolic memory-copy count remains its actual byte count divided
by the element size, including modular multiplication. When divisibility is
proved and the sparse range represents the count, an undecided membership is
modeled by joining copied and untouched contents. That complete pair of
possibilities does not itself require `analysis-incomplete`; unrepresentable
counts, partial cells, exhausted snapshots and missing output projections
still do. This refines RFC 0015's membership warning policy for counts whose
numeric alternatives are represented by this RFC.

Unknown writes and may-alias writes invalidate numeric dependencies under the
same rules as existing scalar, array and heap state. `WEAVEC_UNSAFE` suppresses
diagnostics but does not skip numeric effects on surrounding checked code.

### 3. Bounded symbolic sizes and snapshots

Retain the efficient affine representation for mathematical byte quantities
where justified. Add a bounded, canonical expression representation for actual
C values and size expressions which cannot use that fast path. It records
typed constants, input values, conversions and operations, including products,
sums and minimum/maximum bounds. Limit expression nodes and depth; do not
introduce an unbounded symbolic executor or an SMT dependency.

C expression evaluation and mathematical byte-offset calculation are distinct.
For `p[i]`, evaluate `i` in its C type, then compare its mathematical byte
interval with the storage extent. Do not wrap `i * sizeof(*p) + sizeof(*p)`
in the target's `size_t` while deciding whether the access fits. For
`malloc(n * sizeof(T))`, the allocation receives the C multiplication result.
`calloc` and `reallocarray` instead have checked-product allocation semantics:
an unrepresentable product is failure, never a small wrapped success object.

Exact repeated expressions can establish useful bounds without nonlinear
solving: an access at the same actual value used as an allocation size is one
past the end, even if an unsigned product wrapped. Supported interval and
relation reasoning can prove other comparisons. Failed arithmetic in the
analyzer's own byte calculations becomes unresolved coverage.

Capture values at allocation, VLA declaration and output assignment time.
Overwriting either operand of a product must not resize the earlier object.
Copy numeric facts needed by a snapshot, visit all expression dependencies,
and invalidate stale facts before reusing a snapshot site in a loop. Preserve
stable entry identities when exporting a result. Unrepresentable dependencies
must not be silently replaced with the current value of a mutable variable.

Recognize checked arithmetic through explicit range guards (including the
usual nonzero divisor and `SIZE_MAX / count` multiplication guard) and Clang's
add/subtract/multiply overflow builtins. The overflow result and output value
have correlated success/failure facts. Keep their normal side effects and
preserve the distinction between a successful checked result and wrapping
arithmetic after failure.

### 4. Dynamic storage extents

For a VLA, capture the evaluated dimension at declaration time and compose
nested dimensions in bytes. Later reassignment of a bound variable does not
change `sizeof` the existing VLA or its storage. Use the appropriate
subarray extent for multidimensional access. A nonpositive or unrepresentable
dimension is an invalid or unresolved numeric/storage operation, not a
positive invented extent.

The current milestone conservatively leaves side-effecting dimension
expressions, such as `char a[n++]`, unresolved. The CFG has already executed
the expressions when dimensions are captured; re-reading an operand can see
a later value, including one changed by another dimension. Until individual
evaluation results can be retained, forget the affected declaration's
dimension facts and report `analysis-incomplete` with reason
`side-effecting variable array dimensions`. Apply the same conservative rule
when initializer side effects may have changed a bound's dependencies.
Preserve ordinary CFG effects without replaying them, and do not fabricate
an extent, `sizeof` result or spatial proof from the later values. This
warning marks unsupported precision, not a definitely invalid VLA declaration.

For a final flexible-array member, derive the tail from the backing
allocation and the target's field offset, retaining the element byte size.
The extent is the allocation's bytes beyond the field offset, not a sibling
count assumed without checking. Preserve the enclosing allocation's identity
for lifetime/release checks. Fixed-array subobjects retain their own bounds.
Undersized allocation, padding and non-byte element types need explicit tests.
Passing or returning a pointer to the tail must retain its extent and the
original object's temporal identity.

Existing `WEAVEC_SIZED_BY` annotations continue to count elements (`void *`
counts bytes). Their inferred and declared requirements must propagate through
wrappers even when the body was analyzed under an annotated extent.

An inferred pointer/count field pair records the target multiplication type
when its extent is a C product of that count and a constant. A load reconstructs
that converted product; it must not strengthen it to an unbounded element
count. This type is part of witness agreement and compiler transport. A
mathematical upper bound on a wrapped product can establish a violation, but
cannot establish that an access fits.

### 5. Compositional numeric outputs and requirements

Summary expressions use stable parameter, global and output paths, never
local AST identities. Record representable scalar results and scalar output
postconditions, together with their guards. Translate arguments in the
callee's declared types before substituting them. Returning a narrowed value
or a checked size through an out-parameter must preserve that value in the
caller. Join differing postconditions conservatively.

Extend extent requirements to retain representable numeric conditions and
bounded size expressions. An access requirement retains both its first byte
and its exclusive end; the end alone cannot validate a negative index.
Omitting the first byte denotes a range beginning at zero, including a
possibly empty library byte range. Argument pointer offsets shift both ends.
Alternative guards on the same interval remain a disjunction of separately
guarded requirements. Their common conjuncts alone cannot replace that
disjunction: doing so could create an unconditional caller error.

A requirement's condition must hold before it can
be used to report a caller violation; dropping an unrepresentable condition
must not turn a conditional requirement into an unconditional error.
Unsupported projection is incomplete coverage. Keep requirements when they
cannot be decided so that wrappers can re-export them.

An abstract integer range is an enclosure, not evidence that either endpoint
is reachable. Type limits, masks and one-hop propagation from an unknown
loop bound may prove that an access fits, or that every value is invalid;
they must not by themselves justify the existing possible-boundary diagnostic.
That diagnostic retains its explicit source-bound witness. For example,
`i < unknown_count` does not establish that `i` can reach `INT_MAX - 1`.

For a canonical unit-stride loop bounded by `i < n && i < cap`, express the
required element count as `min(n, cap)` under the loop's entry/termination
conditions. Preserve equivalent explicit minimum expressions. Include
constant-bound and parameter-bound combinations. The loop interpretation
must account for initialization, increment validity and early exits; it does
not infer arbitrary loop invariants or arbitrary strides.

Numeric conditions include constant ranges and representable comparisons
between stable expressions. Bounds constraints may reference pointer-field
counts through existing path rules. The parser validates all paths, widths,
operators, expression shapes and limits. A missing global during remapping
invalidates the whole dependent condition or expression rather than deleting
one premise. Existing global remapping and dependency invalidation must visit
numeric outputs, conditions and every expression leaf.

### 6. Spatial check outcomes and diagnostics

Represent a spatial check as one of:

- **Proven**: the entire access, including its lower bound, is established
  within the object on every represented execution.
- **Violation**: the existing definite or supported boundary violation policy
  applies, with the existing diagnostic and source notes.
- **Unresolved**: available facts do not decide the access or its extent,
  offset, arithmetic or interface projection is unsupported.

Track results at source operations in the final reporting pass and expose
counts and unresolved reasons in `--dump-analysis`. An inferred requirement
is an obligation for callers, not a proof of all its callers. These outcomes
must not be inferred by counting emitted diagnostics, which unsafe regions
and warning controls may suppress. Preserve missing coverage through summaries
where it affects callers.

Use `analysis-incomplete` for unsupported numeric representation/projection or
exhausted limits, consistent with RFCs 0014–0016. A merely unknown arbitrary
index retains the current diagnostic policy; its spatial outcome is
unresolved in the dump. This milestone introduces no `--verify` flag and does
not claim that the absence of incomplete warnings establishes verification.

### 7. Summary format and compiler transport

Bump the summary/sidecar format from 12 to 13. Serialize typed integer facts,
expressions, numeric postconditions and conditional requirements in a
canonical, bounded form. Continue using deterministic ordering and strict
parsing. Reject malformed operators, widths, graph references and oversized
expressions before analysis. Every input dependency participates in summary
comparison, global remapping, SCC invalidation and contextual cache validity.

Both tooling and compiler whole-program modes use the same numeric facts.
Old compiler objects require rebuilding their sidecars. Preserve the existing
handling of stale/unsupported sidecars; this RFC does not redesign archives or
artifact identity.

### 8. Organization, limits and performance

Core owns numeric values/ranges, expression operations, equality, joins,
validation and serialization. Analysis owns AST types, expression lowering,
state transfer, VLA and record layouts. Frontend transports exports through
the existing whole-program engine. Extract numeric and spatial operations
from `Dataflow.cpp` as needed to keep responsibilities clear.

Initial bounds are 64 expression nodes, depth 12, two intervals per numeric
range, eight conjuncts per guard and sixteen extent requirements per pointer
parameter. Exceeding a requirement or condition limit records incomplete
coverage; a must-requirement may not shed a premise to fit the bound.
Snapshot identities are
bounded by existing source-site rules. Changing loop ranges widen instead of
iterating through the target range. These limits are analysis limits, not
limits on the source program's allocation size.

Measure the same pinned five-project corpus, including Lua, with baseline and
new Release binaries built using the same compiler/options. Run sequentially
without overlapping builds, record diagnostic locations and causes, wall time
and peak RSS, and retain the existing evaluation denominator. Use repeated
runs for median runtime comparisons. Target at most 20% median runtime growth
and 15% peak-RSS growth; investigate and document any excess rather than
hiding it by raising timeouts or removing projects. Reducing unsupported
behavior can change diagnostics; each change needs triage.

Recursive call-graph components join each freshly analyzed summary into the
previous approximation, including the reporting pass. Replacing an approximation
can alternate between equivalent but differently projected guards once typed
facts interact with temporal effects. The existing summary join weakens may
effect guards, intersects must postconditions, and retains separately guarded
requirements; it must never discard a requirement's premise. Start a new
component computation at the empty summary as before, keep retained alternatives
first at bounded capacities, and use absorbing unknown numeric outputs. Keep
the existing iteration limit as a failure check, not as the widening mechanism.

### 9. Acceptance and validation

- Preserve existing unit, lit, recall and fixed evaluation coverage. Detect
  both retained size misses (44/44 on the original fixed bug population) and
  retain 32/32 original clean cases.
- Add adversarial bug/clean pairs before implementing their corresponding
  checker behavior. Cover narrowing, `_Bool`, full-width unsigned values,
  mixed signedness, overflow/wrap, invalid division/shifts, switch conversion,
  checked arithmetic, guarded effects, selected elements and count updates.
- Independently enumerate small-width concrete arithmetic and conversions to
  verify abstract results contain all concrete valid outcomes. Exercise
  boundary values at 32 and 64 bits and sanitizer-check analyzer arithmetic.
- Pin outcomes inline, through helpers, across translation units and through
  compiler sidecars. Include returned and output numeric values, conditional
  requirements, arrays, strings, VLA bound snapshots and flexible-array tails.
- Test malformed/oversized formats, missing-global remaps, recursive
  convergence, stale snapshot generations and unsafe diagnostic suppression.
- Add exact-message lit and unit tests for the new diagnostic; update the
  annotations reference and changelog.
- Harden the older recall harness: crashes, abnormal/silent failing exits and
  timeouts cannot pass merely because expected diagnostics were printed.
  Test that accounting independently, as the fixed evaluation harness does.
- Publish reproducible before/after evaluation and corpus results, including
  new false positives and unresolved coverage. Never count an incomplete
  warning as a detected memory bug or as verified code.

## Annotation surface

No new annotation spellings. `WEAVEC_SIZED_BY` and `WEAVEC_ASSUME` retain their
source syntax and trust model; numeric interpretation now follows C types.
Existing annotations must be documented as contracts, not verification flags.

## Diagnostics

Add `weavec::core::diag::InvalidIntegerOperation`, spelling
`invalid-integer-operation`, an error. Primary message:

```
invalid integer operation: <reason>
```

Reasons include `signed integer overflow`, `division by zero`,
`signed division overflow`, `invalid shift count`, and
`invalid signed left shift`. Example:

```c
int bad(int x) {
    if (x != 2147483647) return 0;
    return x + 1;
}
```

For a supported 32-bit signed `int`, the addition is definitely invalid.
The source operation is primary. An optional note identifies a preceding
range fact or size origin when that location is available. No diagnostic
claims that a possibly invalid arithmetic expression is definitely invalid.

Existing `out-of-bounds`, temporal and validity messages remain stable for
unchanged cases. New size forms use the same byte-count/access vocabulary.
`analysis-incomplete` reasons name unsupported numeric expressions, numeric
projection and exhausted expression/predicate limits. `--dump-analysis`
reports spatial proved/violation/unresolved totals independently of warning
severity controls.

## Drawbacks

This adds another abstract domain and changes assumptions shared by several
old rules. Correctly forgetting invalid facts can increase false positives.
Expressions and typed predicates complicate summary joins, remapping and
snapshot invalidation. Dynamic subobject extents require careful separation
of byte bounds from ownership identity. Corpus and sanitizer validation are
required parts of the implementation, not optional polish.

## Alternatives

- Fix only the two evaluation misses: too narrow; temporal branch pruning
  would still be wrong under narrowing conversions.
- Treat every cast as unknown: safe in more cases but loses even precisely
  known results and ordinary widening conversions, needlessly hurting C code.
- Use mathematical unbounded integers everywhere: repeats the current error
  when the machine result wraps or truncates.
- Introduce a solver immediately: adds a large dependency and unpredictable
  cost before establishing correct typed transfer and summary semantics.
- Promote every existing incomplete warning to an error: does not find
  currently silent numeric or alias gaps and is not verification.
- Add runtime bounds enforcement: useful future work, but changes generated
  code and the deployment contract beyond this static-analysis milestone.

## Prior art

- C's integer promotions, usual arithmetic conversions and unsigned modulo
  semantics define the meaning. Clang's typed AST and constant evaluator
  supply target details; host `int64_t` is not a substitute for those types.
- LLVM's `APInt`/`ConstantRange` illustrate bit-pattern arithmetic and
  conservative modular ranges. Core implements the bounded operations it
  needs without depending on LLVM.
- [Clang bounds safety](https://clang.llvm.org/docs/BoundsSafety.html)
  distinguishes C pointer bounds from object representation and makes dynamic
  checks explicit. We retain the static, source-compatible model and do not
  claim its runtime enforcement.
- RFCs 0011–0013 supply affine byte quantities, path requirements and
  allocation-time values; this RFC preserves those useful concepts while
  removing their mathematical-integer assumptions for C evaluations.

## Unresolved questions

No product-scope decisions are left for implementation. The precise storage
layout of canonical expression nodes and placement of helper functions are
implementation choices, provided the semantics, limits and transport above
hold. Corpus measurements determine which numeric transfer operations need
optimization; they cannot authorize weakening the stated semantics.

## Future work

Scoped verification with complete obligation accounting; arbitrary-precision
source integer types; general nonlinear/loop reasoning; recursive container
invariants; comprehensive initialized-memory tracking; runtime enforcement;
and archive/caching deployment improvements remain separate milestones.
