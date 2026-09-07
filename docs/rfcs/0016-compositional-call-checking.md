# RFC 0016: Compositional call checking under caller alias relationships

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-07
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0003's call summaries, RFC 0005's
  program dependencies and sidecars, RFC 0009's heap-fact branch pruning,
  RFC 0013's input/output identities,
  RFC 0014's bounded specialization, and RFC 0015's selected-element interfaces.

## Summary

Check a callee under the pointer relationships established by its caller.
Preserve the difference between two operations and two names for one operation,
including when two parameters or reachable pointer cells refer to the same
resource. Use bounded context specialization of the existing CFG checker, so
statement order, branches, replacement, and ordinary lifetime transitions have
one implementation. Context-specific final summaries cross translation units
and compiler sidecars. Unavailable, unrepresentable, or exhausted required
contexts expose incomplete coverage rather than silently asserting safety.

This RFC was drafted before implementation. The project owner explicitly
requested drafting the recommended RFC and then implementing it end to end.
Acceptance recorded that authorization to proceed; it does not claim a
separate review or merge. The [validation report](../validation-rfc0016.md)
records the completed implementation, tests and measured tradeoffs.

## Motivation

At commit `a5583dc` both of these helpers are accepted when their arguments
are the same non-null allocation, including under `--strict-externs`:

```c
void release_then_write(char *a, char *b) { free(a); *b = 1; }
void release_twice(char *a, char *b) { free(a); free(b); }

void caller(void) {
    char *p = malloc(1);
    if (p) release_then_write(p, p);
}
```

The first is a use-after-free and the second a double-free. The inline forms
are diagnosed. The same gap occurs through aliased output parameters and
through different records whose child pointers contain the same allocation.
The use-after-free has been confirmed with AddressSanitizer.

A safe helper `void write_then_release(char *a, char *b) { *b = 1; free(a); }`
exports the same aggregate summary as the first helper. Aggregate flags cannot
reconstruct order. Removing the alias deduplication in `applySummary` is also
incorrect: one release may be exported under two paths to the same cell.
The correct distinction is the source execution under the caller's inputs.

## Soundness

### Bugs caught

Within the existing supported pointer, field, array-selection, and guard
models, check:

- An access through one parameter after another parameter releases the same
  allocation, including a supported interior pointer.
- Two releases of one allocation through different parameters, fields,
  selected array elements, or globals.
- A use through a saved incoming value after a callee replaces the cell that
  originally held it.
- The same errors through forwarding helpers, resolved callbacks, other
  translation units, and the compiler's serialized link analysis.
- Reference-counted release conflicts when two arguments carry the same share;
  separately retained shares must remain distinct.

Moving a supported operation into a helper must preserve its safety meaning
under the caller's actual relationships. The useful converse is mandatory:
write-before-free, independent allocations, one release visible through two
paths, replacement followed by use of the replacement, and guarded operations
on infeasible paths must not acquire an alias-context error.

### Bugs deliberately not caught

This is not an unrestricted verification mode. Previous limitations for
unknown bounds, C integer wrap/truncation, concurrency, arbitrary byte-level
pointer encodings, GC invariants, unrestricted container traversals, and code
outside known definitions or trusted contracts remain.

Contexts are bounded projections, not copies of arbitrary caller states.
Unresolved input selection, incompatible object views, ambiguous alias
alternatives, unsupported relative offsets, or limits can prevent contextual
checking. Such cases retain existing conservative effects and report incomplete
coverage where the missing relationship can affect the call. No missing alias
edge is evidence that two arbitrary input pointers refer to separate objects.

This milestone specializes relationships established by the caller; it does
not enumerate possible alias partitions of wholly unconnected input values.
Calls with no established interacting relationship retain generic checking.
Their silence does not prove the inputs disjoint. Requiring complete coverage
for every such input belongs to the separate verification-coverage milestone.
Once an interacting context is requested, missing premises needed to apply it
are explicit incomplete coverage, with generic effects retained.

### Accepted false positives

A possible caller alias without sufficient correlation can prevent precise
contextual checking. Existing may-effect joins and generic checks remain
conservative. Context limits may expose a boundary on code that is correct.
Disjointness of two array cells does not establish distinctness of their
pointees. Distinct pointer addresses do not establish distinct allocations
when an interior-pointer relationship is possible.

### Assumptions

RFCs 0001–0015's assumptions continue. In particular, the summarized definition
is the definition that executes, explicit unsafe assertions and external
contracts are trusted, and Clang supplies the target layout. We do not assume
that ordinary C pointer parameters are implicitly `restrict` or disjoint.

## Detailed design

### 1. Context representation in Core

Introduce a portable call-context type, separate from a final function summary.
A context contains:

- Existing callback bindings, so function values and data aliases are checked
  together rather than as unrelated specializations.
- Canonical pairs of interface paths describing a shared allocation, its
  relative `PointerOffset`, whether identity is definite, and whether both
  holders carry the same reference-counted share.
- Canonical pairs known to designate distinct objects. These are stronger
  than unequal addresses. Keep them as entry value facts, intersect them at
  CFG joins, and invalidate them on replacement. Definite copies can consult
  their source's separation while that identity remains live. Distinct live
  allocation origins and disjoint declared storage can establish separation;
  unrelated parameters and different array cells cannot establish it alone.
- Bounded entry facts for integer and pointer values relevant to path pruning.
  These are facts about input values, not permanent invariants of mutable cells.

Requests also retain whether diagnostics are enabled at their originating call.
An unsafe request still needs final effects but cannot cause the definer to
report contextual errors later. This bit follows nested requests and travels
with the context record; it is not an alias premise.

Paths retain the existing parameter/global roots, dereferences, named fields,
and selected array elements. Contexts contain no Clang declarations, AST
pointers, or caller-local place numbers. Symbolic selected indices translate
through the existing interface selector rules; an untranslatable selector
cannot be installed as an exact input cell.

Use a maximum of 32 pointer input paths, 64 relationships/facts, 32 distinct
memory contexts per callable, and eight nested contextual analyses. These are
shared named constants. Canonical ordering and equality must make contexts
stable across traversals and serialization. Reject contradictory, duplicate,
malformed, over-depth, or oversized serialized contexts.

The relationship convention must be explicit: the first pointer equals the
second plus the recorded offset. Same-allocation identity with a nonzero or
unknown interior offset does not establish address equality. May relationships
must not populate the definite-alias tracker or justify a strong update.
For a copied borrow, its loan identifies the borrowed storage while the
spatial record retains the address's offset within that storage. Context
capture must combine both; two loans against one object do not establish
equal addresses.

### 2. Selecting a context at a call

Use the callee's interface footprint: consumed values, pointer bases of memory
accesses and writes, output storage, relevant input paths, and guards. Include
prefix pointers needed to describe aliased storage, not only the leaf pointers
that happen to be consumed. Ordinary read-only calls and calls whose relevant
footprint cannot contain interacting identities need no memory specialization.
Preflight bounds the complete relevant footprint at 64 interface paths before
typed projection; the projected pointer inputs then obey the 32-path bound.
Exceeding either bound reports incomplete coverage. This early bound also
applies when the oversized footprint prevents establishing its interactions.

Resolve the footprint against the actual call before applying any effects.
Distinguish a pointer's value from the address of its storage: `f(&p, p)` does
not pass the same pointer twice. Conversely, `f(&p, &p)` passes the same
storage address, while `f(&a, &b)` can pass different storage with equal pointer
contents. Account for offsets in the argument expression as well as offsets
on alias edges. Preserve the caller's share relationship.

A definite shared input identity selects a context. A possible identity may
be represented conservatively without inventing definite equality; when the
projection cannot preserve the necessary distinctions, expose incomplete
coverage. Preserve provable disjointness and known null inputs, but do not
infer separation from unrelated spellings or absence of edges. Entry scalar
facts prune branches only within the established scalar domain.

The lookup cache is scoped to the state in which the call is evaluated. A
changed alias, scalar fact, callback target, or final reporting pass must not
reuse the first visit's context. Context-specific summaries never replace the
generic definition in `SummaryStore`.

### 3. Checking the callee

Run the existing `FunctionDataflow` on the definition's CFG under the selected
context. Seed contextual paths and alias relationships in the entry state
before the first block. Materialize the path types and selected cells needed
by those facts, using normal places and alias mirroring. Apply entry facts
through the ordinary scalar/null trackers. All ordinary writes, moves, and
scope transitions invalidate those facts as usual.

Consumption exported under an interior alias is relative to that alias's
entry value. For `a = b - 1`, a release of `a` is at offset `-1` when described
under `b`; it must not become a spurious release of `b` itself at the caller.
Retain an entry-relative offset frame for this translation, without changing
the ordinary spatial bounds attached independently to either parameter.

Record initialization preserves integer field facts, including implicit zero
initialization, so a caller's initialized guard field reaches the context just
as an explicit field assignment does.

RFC 0009 declines to prune a contradictory branch on memory behind arbitrary
input pointers. A validated memory context has projected the relevant pointer
relationships, so contextual analysis may prune from the ordinary current
scalar tracker there too. Reassignment and writes through supported aliases
must invalidate or update those facts before any subsequent branch; an
unrepresentable input context is rejected instead of installing partial facts.

This preserves operation identity and order by executing the actual CFG.
There is no second instruction interpreter, and no replay of source-order
operations reconstructed from aggregate flags. Two `free` statements remain
two transfers; one statement whose effect is visible under several paths is
still one transfer. Existing aggregate consumption deduplication remains valid
when applying the resulting final summary.

Produce a context-specific summary using the existing incoming/output and
heap-projection rules. Thus output aliasing is reflected in the final values,
not merely checked in a temporary state and then discarded. Branch outcomes,
replacement, child ownership and shares use their existing implementations.
Forwarded calls can request further contexts. Known indirect targets are
checked separately with the actual arguments before their possible final
summaries join. Unknown targets retain their existing contract/boundary.

Context diagnostics are retained independently of the generic summary and
emitted during final reporting. Emit existing diagnostic IDs at the callee's
operation, with a note identifying the call/context where appropriate. Do not
emit contextual reports from a caller's unsafe region, even if a cached
context was previously checked in safe code. Generic inference continues for
every definition. Final body reporting uses requested contexts when available,
as callback specialization already does: a generic possible alias between two
selected cells must not override callers that prove those cells independent.
Definitions without requests still receive generic reporting. Every feasible
requested context executes the complete CFG, so unconditional errors in it
remain checked; diagnostics are never filtered merely by ID or a matching source
location. Unrepresentable requested contexts remain explicit coverage boundaries.
Definition annotation validation remains independent of requested contexts.

### 4. Recursion, caching, and incomplete coverage

Reuse a completed context only for the same inputs and current callee imports.
Summary changes invalidate dependent contextual results. Keep active contexts
separate from completed contexts. Re-entering an active context or exceeding
any bound is a conservative incomplete result, never a cached empty proof.
A failed/unavailable lookup must retain the ordinary effects used before this
RFC and expose the missing check.

Deduplicate diagnostics by meaningful source operation and context reason.
Repeated analysis rounds must not emit repeated reports. Dumps show the
selected context and its facts so an unexpected inference is inspectable.
The absence of a context-specific diagnostic is not a safety certificate for
unsupported operations in the surrounding program.

### 5. Cross-unit analysis and sidecars

Export memory-context requests and completed specialized summaries separately
from callback-only contexts and generic summaries. Every global path in a
context is remapped with the same global-name discipline as summaries. A
context whose required global cannot be mapped is rejected as unavailable;
dropping a premise and reusing its specialized summary would be unsound.

The program dependency graph must allow requests to travel from caller to
definer and results back to caller. Context-relevant definitions participate
in the corresponding component even if the ordinary direct-call graph is
acyclic. Requests and completed contexts participate in equality, invalidation,
widening and termination accounting. A definer's diagnostics must be checked
under the contexts requested by other units, including requests discovered
through another forwarding helper.

The compiler's replay planner must select definitions that can receive a
context from another input object, even when those definitions have no
unresolved callees themselves. Such objects cannot be treated as fixed summary
exports merely because their compilation was locally complete.

Summary/sidecar formats become version 12. The context text format is bounded,
deterministic, and strictly validated. Existing callback records retain their
meaning. Old object sidecars require rebuilding. Both tooling and compiler
link analysis use the same context resolution and checking.

### 6. Implementation organization and performance

Put context algebra/validation and serialization in Core; context capture,
entry initialization, and specialization in focused Analysis files. Extract
call application from `Dataflow.cpp` when it helps keep those boundaries
coherent. Keep the three-library layering rule.

Avoid enumerating arbitrary alias partitions or all pairs of all program
places. Work is bounded by the relevant footprint and context limits. Cache
contexts, layout facts, and dependency-stable results. Do not trade missed
errors for a quiet fallback to meet a performance target.

Use identical pinned revisions and release build settings for before/after
corpus measurements. Record complete diagnostic changes, execution failures,
wall time, and peak RSS. Keep Lua as a stress corpus, not an acceptance demand
that its GC become inferred. Publish any material cost or coverage regression
with its cause and the applicable representation limit.

### 7. Acceptance tests

The implementation is complete when tests cover:

- Core canonicalization, offset direction, same/different shares, facts,
  bounds, deterministic round trips, malformed inputs, and global remapping.
- Release-before-read/write and read/write-before-release; two releases and
  one release under several paths; equal and independent allocations.
- Aliased output parameters, distinct output cells with a shared child,
  replacement, saved incoming values, null guards, conditional releases,
  nested forwarding and supported interior pointers.
- Pointer fields, globals, selected array elements, shared reference counts,
  and resolved callback targets combined with data aliases.
- Context/state changes at one call site, joins, recursion and all limits;
  unsafe callers and callees; context cache invalidation.
- Direct, indirect, same-file, cross-file and serialized compiler-link cases,
  with internal-name collisions and missing/malformed context metadata.
- Paired good/bad cases in the fixed evaluation. Preserve every existing
  required detection and clean counterpart, and retain known misses in the
  denominator. Use inline/helper/cross-file variants to catch composition
  regressions rather than only matching implementation details.
- Warnings-as-errors build, full unit/lit/recall/evaluation/harness tests,
  sanitizer checks, formatting, and focused clang-tidy. Corpus execution
  failures are independent of normal checker reports.

## Annotation surface

None. No implicit `restrict`, new required ownership annotations, or changes
to the portability of `weavec.h`.

## Diagnostics

Use existing stable IDs and severities. A contextual use-after-free or
repeated release uses the existing message at its actual source operation,
with existing release notes and a call-context note when available.
`analysis-incomplete` gains reasons for unresolved call alias relationships,
unrepresentable input paths, unavailable contexts, and context/depth limits.
These reasons require exact-message lit coverage. They do not assert that the
program contains a memory bug.

## Drawbacks

Contextual checking adds inference work, more exported state, and cache and
program-scheduling complexity. Some previously quiet calls expose incomplete
coverage. Projected contexts cannot recover arbitrary relational heap state;
invalidating too little would be unsound and too much wastes work. Extending
an existing CFG checker also exercises its alias mirroring and replacement
rules more deeply, requiring regression pairs before accepting refinements.

## Alternatives

- **Only remove consumption deduplication.** Confuses one operation described
  through aliases with multiple operations, and cannot recover access order.
- **Require disjoint pointer parameters.** Rejects ordinary valid C callers,
  including write-before-free and aliased output parameters.
- **Serialized ordered-effect graphs.** Attractive for avoiding reanalysis,
  but require another interpreter for branches, input loads, replacements,
  joins, and reference counts. Bounded contexts reuse existing semantics and
  the cross-unit specialization infrastructure of RFC 0014.
- **Unrestricted inlining.** Preserves order but gives up the bounded cost and
  summary-based organization needed for whole-program builds.
- **Do nothing.** Leaves confirmed temporal bugs invisible across helpers.

## Prior art

Clang's [interprocedural analysis](https://clang.llvm.org/docs/analyzer/developer-docs/IPA.html)
uses caller context and bounded inlining to preserve execution semantics.
We retain the same motivation while reusing WeaveC's CFG/dataflow engine and
exported summaries. RFC 0014 already specializes callback helpers under
bounded bindings; this RFC extends that strategy to data identity. RFC 0013's
separation of incoming and final values is essential when aliased output
storage is written during a call.

## Unresolved questions

None blocking acceptance. Precision and performance consequences are measured
against the test matrix and pinned corpus; design changes discovered during
implementation must be recorded here before changing the corresponding code.

## Future work

Generalized container traversal, machine-width arithmetic and richer bounds,
complete verification coverage, GC/region protocols, archive packaging, and
incremental AST caching remain separate milestones.
