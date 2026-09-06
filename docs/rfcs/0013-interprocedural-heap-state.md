# RFC 0013: Interprocedural heap state and value identity

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-06
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0003's summaries, RFC 0007's
  escaped resources, RFC 0008's replaced values and result objects, RFC 0011's
  extents, and RFC 0012's string facts.

Implementation is authorized by the project owner's request to draft this
RFC first and then implement the recommended milestone end to end. This
records that authorization; it does not claim a separate RFC review or merge.

## Summary

Preserve the contents of allocated objects across function boundaries. A
constructor's result includes the owned allocations, argument aliases,
null pointers, sizes and string facts reachable through its fields. The
same information crosses out-parameters and stores into caller memory.
Copies preserve the identities of those objects, while separate constructor
calls create separate objects. Allocation extents refer to the value of a
size at allocation time, not to a variable that can later change. A fixed
evaluation suite records both successful checks and known misses.

The acceptance criterion is that extracting supported allocation and
initialization code into a helper, including a helper in another translation
unit, preserves ownership, alias and bounds diagnostics.

## Motivation

The following three errors are reported when initialization is in the
caller and missed when it is extracted into an ordinary constructor:

```c
struct box { char *data; };

struct box *box_new(void) {
  struct box *b = malloc(sizeof *b);
  if (!b) return NULL;
  b->data = malloc(4);
  if (!b->data) { free(b); return NULL; }
  return b;
}

void overflow(void) {
  struct box *b = box_new();
  if (!b) return;
  b->data[4] = 0;                    // out-of-bounds
  free(b->data);
  free(b);
}
void leak(void) {
  struct box *b = box_new();
  if (b) free(b);                    // leak of b->data
}

struct box *wrap(char *p) {
  struct box *b = malloc(sizeof *b);
  if (b) b->data = p;
  return b;
}
void use_after_free(void) {
  char *p = malloc(4);
  if (!p) return;
  struct box *b = wrap(p);
  if (!b) { free(p); return; }
  free(p);
  b->data[0] = 0;                    // use-after-free through p
  free(b);
}
```

The old summary of `box_new` is only `returns{fresh(free) extent=8, null}`
on a target with eight-byte pointers. The allocation stored in `data`
is invisible. The old summary of `wrap` additionally says `p: escaped`;
it does not connect `p` to the returned object's field.

Likewise, `n = 4; p = malloc(n); n = 8; p[7] = 0;` loses its extent when
`n` changes. The allocation did not grow. Finally, the existing recall set
admits only bugs already caught, so its percentage is a regression measure,
not a measurement of the unsupported cases these examples expose.

## Soundness

### Bugs caught

In addition to the examples above:

- A child allocation released twice, including through two fields that
  refer to it, and a use through one field after release through another.
- A child allocation lost when a constructor's object is freed, including
  nested owned objects and incorrect release families.
- A nullable field dereferenced without a test after a constructor returns.
- A returned object's borrowed field used after its referent is freed; a
  pointer to a local escaping inside a returned object.
- A returned or out-parameter buffer accessed past its allocation-time size,
  including when the size was assigned to a local or an out-parameter.
- String errors through returned fields when the constructor establishes
  a known length or an unterminated buffer under RFC 0012.
- Old aliases used after a helper releases and replaces a field, while the
  field holding the new value remains usable under RFC 0008.

Every bug has a clean counterpart. In particular, a caller that releases
all owned children before their container, handles construction failure,
or preserves a borrowed referent must be accepted.

### Bugs deliberately not caught

This is not a general proof mode. RFCs 0001–0012's remaining limitations
still apply: unknown bounds, integer overflow in C size computations,
concurrency, arbitrary pointer arithmetic, unknown external code, and
unmodelled element witnesses are not closed by this RFC. There is no
new callback points-to analysis, garbage-collector invariant inference,
solver, runtime instrumentation, or library packaging.

The heap description follows the existing finite place vocabulary. It does
not distinguish arbitrary elements of unbounded arrays or list lengths.
Paths beyond the existing summary depth limit are not described. Recursive
structures preserve finite reachable fields and backreferences, rather
than expanding a cycle indefinitely. Information lost at this boundary
must be visible in the analysis dump; it must not be described as proven
safe. A future verification mode can make incomplete coverage an error.

A field whose value differs between returning paths is a set of alternatives.
A missing or unreadable field value is unknown, not evidence of a non-null
pointer or a particular allocation size. Unsupported string lengths remain
unknown under RFC 0012. Existing known-violation diagnostics keep their
current policy when those facts are unknown.

### Accepted false positives

May-alias and may-consume joins remain conservative. A recursive constructor
or loop can merge several dynamic allocations represented at one abstract
site. The merge must not assert that two independently returned objects are
one concrete allocation, nor that two possibly aliased fields are disjoint.
Distinct call sites remain independent.

Postconditions can expose genuine downstream bugs and existing imprecision
previously hidden by missing fields. Corpus changes need diagnostic-level
triage, not a blanket requirement that every count decrease. Lua's running
thread and stack-rebasing invariants remain outside this milestone.

### Assumptions

The existing RFC assumptions apply. A summarized definition is the function
actually called; the program database and sidecars describe that program;
annotations and explicit assumptions are trusted. No Clang or LLVM types
enter Core. Byte sizes use the target layout supplied by Clang.

## Detailed design

### Heap descriptions in Core

A function summary gains a separate heap postcondition, distinct from its
sequence-independent may-effects and stores. It describes pointer-valued
cells reachable from the function's outputs at a returning program point.
Its roots are the pointer result, record result, and caller-visible places
written by the function. RFC 0008's `result` root is extended to a pointer
result: `result *.data` is the `data` field of its pointee, whereas
`result .data` remains a field of a record returned by value.

The description is a finite graph expressed with summary paths. A `fresh`
source introduces an abstract allocated object; a copy of another output
path is an edge to that same object, not another allocation. Thus two fields
initialized from one local buffer describe one fresh node and one copy of
it. A self-link is a copy of the output root. Shared children and cycles
must not be expanded into infinitely many fresh nodes. Caller argument and
global references use the existing interface paths.

Each described pointer value uses the existing fresh/copy/borrow/null/raw/
unknown vocabulary, augmented with the spatial and string facts that are
postconditions of its object. The facts include allocation family and
extent, pointer offset, known string length or known absence of a terminator.
Null is a value alternative. Known non-null values do not acquire a null
alternative merely because the containing object could fail construction.

Within the heap description, a copy distinguishes an incoming interface
value from a reference to the postcondition graph. This distinction is explicit in the core representation
and serialized format. A result-relative post reference names a node of its
own description; a parameter/global post reference names another output
cell of this call. Pointer returns can also reference such output cells.
Reading a post-state field as though it held its
entry value is incorrect. Object descriptions have deterministic ordering
and graph references have no dependency on Clang declaration addresses.
The existing may-store projection retains its interface-cell references for
compatibility. Materializing a heap description must not feed synthetic
field assignments back into that may-store projection: otherwise each
summary iteration invents more writes and can invalidate unrelated guards.
Saved entry identities likewise belong to final heap/return values; they
must not replace interface-cell references inside the historical stores.
Those fields remain available through their containing output graph. A heap
description does not interpret an already-written cell as an incoming value
merely because a legacy store names it.

### Producing a heap description

The Analysis layer captures the final reachable contents before resources
escape on return. It follows pointer fields of allocated objects and exact
aliases using the state at that return, not the historical union of every
assignment in the function. Fields initialized then reset to null describe
null. Allocations freed on failing constructor paths are not exported as
live objects.

Fallthrough in a void helper is a returning point even when the function
owns no local allocations. In particular, pointer swaps must export both
incoming identities; applying their assignments sequentially without saved
entry values would copy the first new value into both destinations.

The traversal records a canonical output path for each encountered object.
A subsequent edge to that object is a copy of the canonical path. Every
fresh child retains its family, extent and known string facts. Argument
aliases remain copies of the incoming argument, rather than becoming fresh
allocations. Borrowed stack storage carries the existing lifetime obligation
through the returned graph.

For an out-parameter or caller-visible store, the final state is reached
through the output and its exact aliases, including a local used to finish
initializing the object after publishing it. Scalar facts used in extents
are folded when constant and otherwise translated to the interface only
when that translation refers to the correct value.

Descriptions join across returning paths. Value alternatives join by union;
spatial and termination claims require agreement where the same value can
arrive along multiple paths. Existing argument guards and outcome classes
restrict applicable descriptions. A null result has no object whose fields
need initialization or release. The same distinction applies at CFG
joins: a spatial fact missing only because the pointer or a containing
pointer is definitely null does not weaken the existing object's extent or
string facts. A missing fact under an unknown or non-null pointer still
weakens it. When place topology is available, the core state join implements
this absence rule before joining null facts. Missing field knowledge on a returning path
cannot strengthen the common postcondition.

### Applying a heap description

A caller first checks requirements and applies existing consume/write/store
effects, preserving RFC 0008's distinction between old and replacement
values. It then materializes the postcondition below the received pointer
or output place. The materialized cells participate in the normal alias,
resource, nullness, spatial, move and lifetime trackers. They must not live
in a parallel checker with different release rules.

Allocation nodes are instantiated for this invocation. Calling the same
constructor at two sites gives distinct allocations. Copies within one
description refer to the same instantiated node. Materialization resolves
fresh nodes before graph references, allowing shared children and cycles.
Graph references use the post-state outputs; incoming references retain
entry identity even if a caller-visible cell is replaced by the call.

A successful result whose allocation was only possible on the non-null
outcome guards its children's ownership by that outcome. Testing the result
null removes those records, so an ordinary `if (!b) return;` cannot leak
phantom fields. A constructor returning a non-null container with a null
child preserves the child's own nullness.

A store made only on success is governed by the existing outcome machinery.
On a path that did not replace an output, the caller keeps the applicable
incoming value and its facts. On a path that did replace it, old copies are
still invalid if the callee consumed them. Incomplete descriptions do not
resurrect a consumed old value.

A known final value whose write is guaranteed under the caller's facts
takes precedence over a widened historical consume
whose `replaced` must-fact was lost: a fresh or null final value is live,
and a final copy inherits the source value's move state. An unknown final
value retains the conservative old consume record. In every case the
consume still invalidates other holders of the incoming object.

The historical store projection is computed once from the call's incoming
state, independently of final value materialization. Applying final values
must not feed a different sequence of stores back into the next summary
iteration. An unknown final object has no extent or string must-fact merely
because an intermediate store allocated a known-size buffer.
An unknown output also does not prove disjointness from the old summary's
copy alternatives. Those alternatives retain may-alias edges and held
loans, without allocating another object, copying child graphs, or asserting
definite identity, bounds or string facts.

### Allocation-time scalar values

Capture an allocation's extent from the current value facts. A known local
or out-parameter size becomes a constant, so the common constant-size
constructor needs no public count annotation.

When a spatial fact depends on a nonconstant scalar place which is about
to be overwritten, preserve its old value under a synthetic snapshot place.
Copy the scalar and relation facts needed to interpret that value and
redirect the affected extent/length references to the snapshot. Reassigning
the original place invalidates its old relationships, but does not resize
an object allocated with the old value. An unaffected copy of the count
can still establish an index relation to the allocation.

Snapshots are interned per function and write site. Re-entering a site in
a loop must invalidate or conservatively merge previous snapshot facts
before reuse; it must never bind an older allocation to a newer count.
This bounds the value domain. Expressions outside the existing one-place
affine domain remain unknown. Snapshot names are shown as allocation-time
values in dumps, with useful source names in diagnostics.

### State updates and replacement

Definite assignments update one cell and its exact mirrors. Alongside the
existing may-alias relation, the state retains a definite pointer-identity
relation with offsets: whole-value copies and pointer derivations establish it,
pointer arithmetic shifts its offsets, and joins keep only edges
agreed on by every predecessor. Overwrites remove the old edges.
Projection through zero-offset edges supplies the mirrors used for strong
updates; derived edges preserve object identity in postcondition references.
The existing may relation continues to govern possible consumes. An uncertain
alias does not justify clearing unrelated move records or asserting that
all possible cells received the new value. Ownership and spatial facts of
an old value remain on its other holders when the original location is
reassigned. Existing `freed,replaced` semantics remain authoritative.
Releasing a saved incoming pointer records consumption of its entry path
even after replacement removed its alias edge to that cell. Thus swapping
in a new allocation and freeing the old one invalidates the caller's old
copies while leaving the replacement live.


The state distinguishes pointers whose non-null value definitely belongs to
an allocation created during the current function from unknown incoming
objects. Copies preserve this fact; a join retains it only if every non-null
alternative agrees. Cleaning up fields of such an object is not consumption
of the caller's old fields at the same interface path. A field that copied an
incoming pointer still consumes that incoming identity when released. Exit
move records on copied fields likewise must not consume their destinations'
old values. An ownership move transfers a live value to an output; a free
leaves copies of its incoming value freed.
An interior borrow held in a field of the very allocation being released
dies with that storage. It does not conflict with release; a holder outside
the allocation still does. This uses definite object identity, including
field offsets, and never a may-alias edge to justify dropping a conflict.

Output writes and pointer returns retain immutable interface guards describing
when they execute. Copied incoming pointers retain their original guard operands
even if their interface cell is subsequently cleared.
A post-write null test is not a test of the incoming pointer. Writes below a
newly published allocation inherit its entry guard when that publication
occurred on every predecessor; a join with an untouched object must not
impose that guard on an unconditional later field write. Call application
snapshots guard operands that the call can overwrite, so root and child
materialization test entry values consistently. A lazy initializer called
on an already initialized object must leave its contents and move records
intact, including after a child has been freed.

The conditions captured at a write govern that write's postcondition.
Later exit facts must not add preconditions to it: a list traversal after
an unconditional publication can end with a null iterator without requiring
the caller to have passed an empty list. This distinction also prevents
recursive summaries from losing unconditional stack-pointer replacements.

Postcondition capture and application must have paired tests for fresh
replacement, reset-to-null, allocation failure leaving the old pointer
intact, and old interior aliases. This milestone does not infer arbitrary
postcondition-guarded consumes such as Lua's trap invariant.

### Serialization and whole-program integration

Summary format and sidecar format advance to version 9. Heap postconditions,
post-state references and string metadata round-trip deterministically.
Malformed graph records and invalid references are rejected. Unknown global
roots weaken or remove dependent facts just as existing summary remapping
does; a missing global cannot silently become a different allocation.

Heap descriptions participate in summary equality, joins, widening,
renumbering/imports and dependency invalidation. The tooling whole-program
mode and compiler driver's compile/link pipeline use the same facts. Tests
cover cross-unit constructors, returned aliases and object destruction,
including object-sidecar round trips.

### Boundedness, diagnostics and performance

Use the existing maximum synthesized place depth for graph projection.
Traversal memoizes already represented objects and paths and emits at most
128 field alternatives per description. More than eight alternatives for
one cell widen that cell to unknown and mark the description incomplete;
further alternatives cannot restore its precision. It is driven by
reachable fields with state facts rather than every possible C type path.
Descriptions must converge under recursive summary joins. Coverage loss
from a depth or traversal bound is retained in the summary and shown in
`--dump-analysis`; widening cannot turn incomplete into complete.

Keep heap inference and materialization in a dedicated Analysis source file
and value snapshot operations in a separate unit where appropriate. Core
contains only frontend-neutral facts and operations. Avoid duplicating the
8,598-line dataflow implementation for each existing diagnostic family.

Incoming pointer snapshots needed by pending store outcomes or result
references are retained; other call temporaries are retired after
materialization. Guard-only snapshots retain scalar and null facts without
creating value aliases. Value snapshotting copies the affected cell facts and held loans without replaying program
assignments or duplicating loans against mirrored storage.

Measure the release-build corpus at the pinned revisions with the same
arguments before and after. Record per-project diagnostics and time, plus
heap description sizes in targeted stress tests. Any material slowdown is
investigated, especially on Lua's cyclic call graph. A timeout or failure to
converge is a test failure, not a clean result. Performance tuning must not
silently remove the acceptance cases.

### Evaluation and acceptance tests


Keep the current lit suite and recall pins as regression gates. Add a fixed
checked-in evaluation manifest containing independently selected good/bad
cases and known misses. Every expected bug remains in its denominator even
when it is not yet caught. Report detected bugs, missed bugs, unexpected
reports, parse failures and timeouts separately. New true-positive reports
are improvements, not failures merely because a diagnostic count grew.

The required matrix includes:

1. Inline, helper, and separate-TU versions of constructor child overflow,
   child leak, and a borrowed field's use-after-free.
2. Nested owned objects, two fields aliasing one child, self-links, distinct
   calls, and constructor failure cleanup.
3. Pointer returns, out-parameters, and initialization through local aliases
   of caller-visible memory.
4. Null, non-null, raw, borrowed and differently released field values.
5. Known string lengths and unterminated buffers through returned fields.
6. Size reassignment, count copies, symbolic sizes, and loops reusing a
   snapshot site.
7. Replacement with stale aliases, reset-to-null and failure retaining the
   input pointer.
8. Core join/serialization/remapping tests, compiler-driver tests, and
   bounded recursive graph cases.

## Annotation surface

None. Existing ownership, nullness, release-family and sized-field annotations
continue to apply. New knowledge is inferred from bodies.

## Diagnostics

Existing stable identifiers and message shapes apply to the newly visible
facts: `use-after-free`, `double-free`, `leak`, `mismatched-release`,
`null-dereference`, `invalid-release`, `unsafe-operation`,
`lifetime-too-short`, and `out-of-bounds`. Their primary locations remain
the invalid operation or point where ownership is lost. Allocation notes
at a call identify where the caller received the object; alias notes should
use caller-visible field names rather than internal graph identifiers.

Graph incompleteness is information in the analysis dump, not a new safety
error. It does not turn a successful current-mode run into a safety proof.
New behavior is pinned in `rfc0013-*.c` integration tests and documented in
`docs/annotations.md`.

## Drawbacks

Heap descriptions enlarge summaries and expose more paths to the existing
checker. Alias-heavy code can become expensive, and newly visible facts can
increase both true and false reports. The core format must distinguish entry
and exit values carefully. Recursive graphs need finite abstraction; a
bounded graph is not a proof of arbitrary data-structure invariants.

## Alternatives

- Inline constructors during analysis: duplicates work, scales poorly with
  recursion, and does not supply reusable cross-unit summaries.
- Require owned/sized annotations on every field: cannot express the alias
  between an argument and a particular returned field, and contradicts
  inference-first adoption.
- Replace the checker with a general symbolic heap solver: a much broader
  design and performance change than needed for the demonstrated failures.
- Keep only flat fresh results: leaves ordinary encapsulation able to erase
  existing temporal and spatial checks.

## Prior art

- RFC 0003's interface paths and RFC 0008's record-result stores supply the
  existing vocabulary. This RFC extends that vocabulary to reachable heap
  objects and distinguishes output references from input values.
- RFC 0010's stores-out-of-sight identified the escape but discarded the
  destination; heap postconditions retain it when it is reachable.
- Clang Static Analyzer's symbolic regions separate a memory location from
  the value stored there. The same distinction motivates allocation-time
  size snapshots; WeaveC retains its existing finite dataflow and summaries.
- Rust's `Box` ownership carries through constructors and nested fields.
  WeaveC must infer the corresponding relationships from C assignments and
  tolerate ordinary aliasing rather than making every pointer copy a move.

## Implementation notes

The implementation adds `DataflowHeap.cpp` for heap capture, input snapshots,
and call materialization, and `DataflowValues.cpp` for allocation-time scalar
values. Core stores only frontend-neutral descriptions and state facts.
Summary text and object sidecars are version 9; older sidecars must be rebuilt.
There are no new annotations or diagnostic identifiers.

The regression suite contains 582 CTest tests, including 82 lit cases,
67/67 recall pins, and the fixed evaluation. Debug and ASan/UBSan runs pass;
the Release build passes with warnings treated as errors, and formatting
checks pass. The fixed evaluation detects 14/16 bugs
and accepts 4/4 clean programs with no unexpected diagnostics, parse failures,
execution failures or timeouts. The two retained misses are products of
symbolic sizes and variable-length arrays. These selected cases are not a
population estimate of C memory-safety recall.

Corpus triage exposed and fixed several mistakes before acceptance:

- Final values must not be re-recorded as historical stores during the next
  recursive summary iteration.
- A freed incoming copy stays freed, while an ownership move transfers a live
  object. Exit moves on a copied field do not consume the destination's old
  value. Both direct and local-alias swaps snapshot their inputs.
- Cleanup of a newly allocated object is local, even after publication through
  a parameter. A copied incoming child still contributes its own consume.
- A borrow held inside the released allocation dies with its storage.
- Output-path lookup uses one index per capture instead of scanning all places
  for each output. Settled input snapshots are retired, copied fields are
  bounded, and guard-only snapshots copy facts without creating value aliases.

The final release-build corpus uses the same pinned source revisions as the
pre-change run and reports zero Clang errors. It takes about 64 seconds,
including 62 seconds for Lua; timings depend on machine load. Earlier
experimental implementations took more than 13 minutes. Those runs are not
used for the accepted diagnostic counts. Debug dumps and diagnostics must be
captured separately: combining their streams can interleave lines and corrupt
counts and locations.

**There is a substantial precision regression on Lua.** Its double-free count
rises from 35 to 1,563 and use-after-free from 632 to 846. The new heap aliases
connect collection effects to the running `lua_State`; the model does not
prove that the running thread is excluded from collection or that stack
rebasing updates all live pointers. Broad callback candidate sets also apply
parser-data field effects to unrelated userdata records. These remain limitations
of the implemented analysis, not newly found bugs in Lua or evidence of a
safety proof. The full corpus rises from 959 to 2,744 reports. The reviewed
counts and diagnostic-level categories are in `scripts/corpus/README.md`.
Reducing these reports requires more precise callback targets and object
invariants; it must not be done by treating unknown heap state as safe.

## Unresolved questions

No further annotation or format decision is required for this milestone.
The corpus precision limitations above remain open engineering work.

## Future work

Precise callback targets, pointer-equality guards in summaries, richer
arithmetic and C layouts, explicit verification mode, runtime enforcement,
and production library packaging remain independent milestones. The heap
facts introduced here are inputs to those later designs.
