# RFC 0015: Array elements, range operations, and container ownership

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-07
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0002's array places, RFC 0006's
  element witnesses, RFC 0008's element consumption, RFC 0013's heap
  descriptions, and RFC 0014's complete memory copies and coverage.

## Summary

Track the contents of multiple array elements and bounded ranges, including
the identity of the objects their pointers refer to. Array updates must
preserve the state of other elements. Copies and overlapping moves of
complete elements preserve pointer values, ownership, aliases, initialization,
nullness, callback targets and reachable children. These operations compose
through helpers, translation units and compiler sidecars. Unsupported ranges
and exhausted limits remain explicit incomplete-analysis boundaries.

This RFC is drafted before the implementation. The project owner requested
drafting the RFC and then implementing option 1 end to end. Accepted records
that authorization to proceed with this design; no implementation preceded
this document. Implementation refinements that change a decision below must
be recorded here before the corresponding code changes.

## Motivation

The pre-change checker accepts each of these under `--strict-externs`:

```c
void lost_history(char **a) {
    free(a[0]);
    free(a[1]);
    a[0][0] = 1;             // use after free
    free(a[0]);              // double free
}

void changed_index(char **a) {
    int i = 0;
    free(a[i]);
    i = 1;
    a[0][0] = 1;             // use after free
}

void copied_array(char **a) {
    char *b[2];
    memcpy(b, a, sizeof b);
    free(a[0]);
    b[0][0] = 1;             // use after free; only a coverage warning today
}
```

RFC 0006's array summary remembers one element witness. Releasing a different
element replaces that witness, and rewriting the index makes it unknown.
The latest witness is useful within one iteration, but is not a history of
the values stored in a container. The same summary place is shared by
initialization, nullness and ownership, which makes independent element
updates difficult to express consistently.

These are ordinary C patterns. Linenoise removes and compacts a pointer
history array; Jansson replaces and resizes pointer tables. Their reduced
forms should be checked without requiring an annotation on every subscript.
RFC 0014 supports one pointer or one compatible record per memory copy;
extending it to arrays requires element identity, not just a larger byte
count accepted by the existing copy handler.

## Soundness

### Bugs caught

- A use or repeated release of any tracked element after other elements
  have been released, read or replaced.
- Release history through constants, saved index values and supported affine
  offsets, including reassignment of the source index after the release.
- Use after free through two elements holding the same pointer, or through
  an ordinary local alias of an element.
- Uninitialized or null pointer elements used after initializing another
  element; partial initialization does not initialize the array.
- Lost owned elements at overwrite, removal, discarded container storage or
  container destruction, where the existing resource model establishes the
  ownership obligation.
- Use after free, repeated release and lost ownership through complete
  pointer-array and compatible record-array copies and overlapping moves.
- The same cases through helper functions, returned containers,
  out-parameters, whole-program inference and compiler sidecars.
- Old storage aliases invalidated by successful reallocation while pointer
  values copied into replacement array storage retain their pointee identity.
  On failed replacement, the original storage and its contents remain.

Each required detection has a clean counterpart. Independent elements may be
freed once each; replacing a released slot makes the replacement usable;
compaction followed by clearing the redundant tail must not duplicate an
ownership obligation; copying pointers does not create new allocations or
reference-counted shares.

### Bugs deliberately not caught

This is not verification mode. Existing unknown-bounds, C arithmetic,
concurrency, GC, stack relocation, arbitrary byte encodings and unchecked
external-code limitations remain. This RFC does not reconstruct a pointer
from separate byte fragments or infer arbitrary list/tree/graph invariants.

The supported range unit is a complete pointer or compatible complete record
element, with target-layout byte size. Arbitrary strides and non-affine
indices are outside precise range reasoning. They must expose the affected
coverage boundary and cannot silently erase known temporal evidence.

Unknown aliasing can prevent a strong update. A summarized range does not
claim that all its elements refer to different allocations. A loop allocation
site can represent several dynamic objects; repeating a site is not proof
that the new object equals an older one.

### Accepted false positives

When an unresolved index may select a released cell, a conservative temporal
report is permitted. When a bounded representation cannot retain the required
distinctions, incomplete coverage is required. Neither absence of an alias
edge nor a discarded index predicate proves disjointness.

Range effects inferred without a representable path condition may be weaker
than their source body. Strong replacement is permitted only when the
destination and the write's execution are definite. Conditional effects keep
the existing outcome/guard machinery.

### Assumptions

RFCs 0001–0014's assumptions continue: single-threaded execution, the called
definition is the summarized one, target layout is supplied by Clang, and
explicit unsafe assertions and declared contracts are trusted. Existing
reference-count rules continue to distinguish copying a share from retaining
one. Core remains independent of Clang and LLVM.

## Detailed design

### 1. Array storage and element identities

Keep three concepts separate: the storage of an array, a cell containing a
pointer or record, and the resource reached through that cell. Extend the
place vocabulary with selected elements below array storage. Selection must
survive place translation, alias mirroring, heap projection, summary paths
and serialization; it must not be encoded as a fabricated record field.

An element selector is a constant or a stable scalar value plus a constant
offset. The source form `a[i]`, `*(a + i)` and a pointer stepped to the same
element must resolve consistently. Indexing nested arrays retains each
dimension when representable within the existing path-depth bound.

The existing summary element remains the fallback for unresolved selections.
It must not be treated as an exact cell. Track up to 32 selected cells per
array storage object and up to 32 range facts per object. These bounds are
deterministic, shared constants in Core. Reaching them marks coverage
incomplete and weakens affected must-facts; it never evicts a previous free
record in order to make a later read appear safe.

The array contents use the existing state domains through selected places:
aliases and definite aliases, moves, loans, resources, nullness, scalar facts,
spatial/string records, incoming values, heap children and function targets.
This avoids a second independent definition of what a copied pointer means.
The array domain supplies selection, range relationships and update coverage.

### 2. Index values and joins

Fold a known constant at the access. Preserve saved values and supported
equalities/offsets through the scalar relation machinery. Before a scalar
write destroys a selector dependency, capture the old value under a stable
analysis identity or conservatively summarize the affected selection. A
write to `i` must not turn the contents of `a[old_i]` into those of `a[new_i]`.

Snapshot identities are bounded by source sites, as in RFC 0013. Reusing a
snapshot site across iterations invalidates facts that require two different
generations to be equal. Iteration does not allocate an unbounded sequence of
places. An ambiguous generation is incomplete, not a new exact identity.

At joins, possible consumed cells and incomplete coverage join by union;
definite values and relationships survive only when every predecessor
justifies them. Joining two different exact cells must retain their possible
consumption separately. Selection and overlap operations distinguish equal,
disjoint and unresolved cases. Unknown indices cannot acquire disjointness
merely because their spelling differs.

### 3. Reads, writes, releases and initialization

A precise read obtains the selected cell's value. An unresolved read joins
the cells/ranges it may select and retains the corresponding uncertainty.
Reads through nested pointer elements check both the container storage and
the loaded pointer. Element writes check the previous value's outstanding
ownership before replacing it.

An exact write updates only the definite destination. A weak write joins
possible new values into every represented overlapping destination and
invalidates incompatible must-facts. It must not revive released aliases
held elsewhere. Overwriting the storage of a pointer differs from releasing
the object that pointer refers to.

Local array declarations start with uninitialized pointer cells. Aggregate
initializers initialize the cells they cover, including C's implicit zero
initialization; omitted elements in an initializer are null where C requires
them to be. Ordinary assignment to one element initializes only that element.
Raw byte zeroing is not assumed to produce a null pointer on every target;
any supported recognition must use the target's representation guarantees.

Container resource checks visit the represented element contents without
counting an allocation separately for every alias. Releasing array storage
invalidates pointers into that storage but does not implicitly release its
pointer elements' pointees. Losing the final owning holder retains RFC 0007's
leak behavior. Reference-counted copies preserve their share identity.

### 4. Complete range copies and moves

Extend RFC 0014's shipped `memcpy`/`memmove` recognition, including its
fortified spellings. User definitions retain their inferred contracts.
Resolve source storage, destination storage, element type, starting offsets
and count from the actual arguments and Clang's target layout.

For a bounded constant range, capture all source cells before changing any
destination. Apply ordinary overwrite checks and assignment/heap transfer to
the destination cells. This is simultaneous copy semantics, including both
directions of overlap for `memmove` and self-copies. Bounds and null checks
still run. A copy preserves pointee identity and does not retain a new share.

For a symbolic contiguous range, retain a bounded range relationship between
the immutable source contents at the operation and the destination interval.
Materialize selected cells on demand when membership and the corresponding
source selector can be established. Endpoints use existing affine values and
allocation-time snapshots; a changed length does not change an earlier copy.
An unresolved membership yields weak information and incomplete coverage.

Record-array copies use the same operation on each record's pointer fields
and reachable children, with layout validation. A copied scalar count must
remain associated with the pointer field it sizes where the existing scalar
and heap domains represent the relation.

Partial-element copies, incompatible layouts, non-integral element counts
and unsupported ranges invalidate affected destination must-facts and report
incomplete coverage. Preserve temporal information about the source and
unaffected objects. Do not let a fallback turn a destination into a fresh
allocation or discard unrelated resources.

### 5. Container helpers, ranges and replacement

Element effects are part of function summaries. Constant selectors and
selectors relative to immutable entry arguments remain distinguishable at
call sites. An unknown selection is explicitly unknown. Extend the existing
effects, stores and heap postconditions, using final output values separately
from incoming values as RFC 0013 requires.

Represent contiguous copy effects with source/destination interface paths,
offsets, length, element layout and guard/outcome information. Preserve the
operation's input identities before applying overlapping writes. A helper's
final postcondition must describe its final contents, not replay an
intermediate store after a later removal or replacement. Unrepresentable
compositions are incomplete boundaries.

Loop cleanup may summarize a contiguous released interval when the loop's
induction and coverage are justified by the existing CFG/scalar facts.
An arbitrary loop is not assumed to visit every element; early exits and
conditional releases weaken that claim. Selected elements retain their own
history even when the overall loop range cannot be established.

The same structural proof applies to a canonical zero-based fill loop whose
only body operation assigns null or calls the shipped `malloc` with a
constant extent into the current pointer cell. Each selected cell receives
its own allocation identity; a fill does not alias all iterations to one
allocation. Store a bounded initialization interval, with the entry count,
the fresh/null value description and its execution guard. Export it through
the same summary and sidecar machinery as copies and cleanup. More general
loop bodies continue through ordinary CFG analysis and cannot assert complete
initialization merely because one iteration writes an element.

Growing, shrinking and replacing arrays use RFCs 0008/0013's incoming/output
identities. Preserve copied element values in new storage and invalidate old
storage aliases. Failed reallocation restores the incoming contents. Shrunk
or discarded elements retain their outstanding ownership obligations.

### 6. Integration, format and diagnostics

Summary and sidecar formats become version 11. Serialize selected elements,
range relationships, conditions and incomplete coverage in deterministic
order. Validate selectors, bounds, counts and references before accepting
serialized input. Global remapping and equality/dependency invalidation visit
all new summary data. Old sidecars require rebuilding their objects.

The tooling and compiler use the same inference and summary application.
The program fixpoint must observe new array facts, including callback values
stored in arrays. Actual element values drive calls; unrelated elements in a
callback table must not contribute their targets to an exact selection.

Use the existing stable diagnostic IDs. Temporal and validity diagnostics
name the selected element where possible, with the existing release/copy
notes. `analysis-incomplete` gains reasons for unresolved element updates,
unsupported array ranges and array analysis limits. Deduplicate by operation
and reason. Unsafe regions retain normal analysis and existing report
suppression; their summaries retain incomplete coverage.

### 7. Implementation structure and performance

Put array selection/range algebra in Core, AST interpretation and storage
updates in focused Analysis files. Extract related storage/copy logic from
`Dataflow.cpp` when needed to give the new operations a coherent interface.
Preserve the existing CFG engine and three-library dependency structure.

Bound work by tracked cells and ranges, never by an unchecked source count.
Large and symbolic copies must not allocate a vector proportional to the
program's buffer size. Range dependency cycles and repeated loop sites must
terminate with conservative coverage accounting.

Measure release builds on identical pinned corpus revisions, comparing
diagnostic locations and IDs as well as elapsed time and peak memory. Record
regressions and improvements with their causes. Do not trade a missing safety
report for silence or describe an incomplete operation as proved safe.

### 8. Acceptance tests

The implementation is complete only when the following are exercised:

- Core selection/range relations, joins, strong/weak updates, boundedness,
  deterministic serialization, malformed inputs and remapping.
- Several independent elements and several aliases of one pointee;
  initialized/uninitialized and nullable/nonnull cells; retained shares and
  callback targets; records with pointer fields and nested arrays.
- Constant indices, saved indices, reassignment, supported offsets, loops,
  joins and unresolved selections, with paired bad and good cases.
- Non-overlapping copy, self-copy and both overlap directions; whole and
  partial elements; zero, constant, symbolic and excessive lengths;
  destination overwrite leaks; source snapshot stability after later writes.
- Copy, removal, compaction, replacement and reallocation through inline
  code, same-file helpers, other-file helpers and compiler sidecars.
- All three motivating bugs, existing regression detections and clean cases.
  Add the new feature matrix to the fixed evaluation with known misses kept
  in its denominator. Strict Clang parsing precedes acceptance as evidence.
- Reduced linenoise history and Jansson pointer-table cases and their unsafe
  mutations. Preserve pinned corpus counts and detailed triage, including
  remaining incomplete coverage and execution failures.
- Warnings-as-errors build, complete CTest/lit suite, ASan/UBSan, formatting,
  relevant clang-tidy checks and Core's no-Clang/LLVM rule.

## Annotation surface

None. Existing ownership, nullability, sized-field, reference-count and unsafe
annotations apply to the values represented by the new cells. No annotation
is required for an ordinary precisely representable element or copy.

## Diagnostics

No new IDs. Existing `use-after-free`, `double-free`, `use-after-move`,
`use-of-uninitialized`, `null-dereference`, `leak`, `conflicting-borrow`,
`invalid-release`, `mismatched-release` and `out-of-bounds` retain their
meanings and severities for selected cells.

The warning `analysis-incomplete` retains its primary form:

```
analysis is incomplete: <reason> [weavec::analysis-incomplete]
```

Reasons identify an unsupported array operation or an exhausted element/range
limit. Unit and RFC-numbered lit tests pin the new reasons and representative
element diagnostics. The annotations reference documents the coverage
extensions.

## Drawbacks

Tracking more cells increases state size and alias propagation cost. Range
snapshots and final helper postconditions require careful ordering. Existing
array-summary expectations and diagnostic spellings will change, and stronger
tracking can expose both real bugs and existing false positives. The format
bump requires rebuilding objects. A bounded design still cannot infer every
container invariant from arbitrary C.

## Alternatives

- Keep several move witnesses only: fixes some release-history examples but
  leaves nullness, initialization, ownership and bulk copies inconsistent.
- Unroll every array or loop: unbounded memory/time and no solution for
  symbolic sizes.
- Treat every element as one may-alias cell: conservative but rejects basic
  cleanup and replacement patterns.
- Infer unrestricted separation-logic container invariants: substantially
  broader than the ordinary arrays and contiguous operations targeted here.
- Verification mode first: establishes an acceptance contract but does not
  supply the missing element semantics required to accept these patterns.

## Prior art

The immediate references are RFC 0006's element witnesses, RFC 0011's affine
relations, RFC 0013's immutable values and heap postconditions, and RFC 0014's
simultaneous complete copies. This design preserves their bounded approach,
while making an element a storage identity shared by all state domains.
Clang's AST and target record layouts provide C structure and element sizes;
none of that frontend dependency enters Core.

## Implementation notes

`Core/Array` holds selector and interval algebra. Selected places reuse the
existing ownership, alias, initialization and validity domains. The Analysis
implementation is split into selection/initialization, simultaneous memory
copies, sparse copy ranges, cleanup and fill operations. Range and index
snapshots are analysis temporaries and do not introduce owning shares.

The canonical traversal proof accepts a local counter declared in the `for`
initializer or assigned zero there after a separate declaration. Its condition
is `counter < affine_count`, its increment is `++`, and its only body action
is a shipped `free` (optionally followed by clearing the same cell), null
assignment, or a shipped `malloc` with a constant extent. Side effects in the
array base, a different cleared slot, breaks, conditional bodies and extra
statements prevent that proof. Other loops still use the ordinary CFG checker;
this does not prove reference-counted cleanup or general loop invariants.

Copy ranges capture represented source cells and freeze further cells before
supported source writes. A symbolic copy of an earlier symbolic copy, or an
entry-source range modified before copying, exposes an incomplete boundary
when the final relation cannot be represented. Concrete cell postconditions
remain useful. These cases do not export an invented entry-value range.

Format 11 adds `array-copy`, `array-fill` and `array-release` records. All
carry interface paths, affine bounds, guards and definite/possible strength.
Copies additionally carry target element size and object view; fills describe
null or fresh allocations; releases distinguish retained pointer values from
cleared cells. Joins and lost interface guards weaken strong writes. Summary
parsing, global remapping and compiler sidecar round trips cover each record.

The [validation report](../validation-rfc0015.md) records acceptance checks,
the fixed evaluation, pinned corpus changes and measured costs.

## Unresolved questions

The accepted bounded design is implemented and validated. The report records
remaining false positives in multi-variable and reference-counted traversals,
Lua's constructor/GC interactions, and the measured analysis cost. Richer loop
invariants and faster whole-program propagation remain follow-up work; the
current coverage boundaries remain explicit.

## Future work

Explicit verification mode, machine-width arithmetic and richer bounds,
general tagged/opaque views, GC/region invariants, archive packaging,
incremental persistent analysis and runtime enforcement remain separate.
