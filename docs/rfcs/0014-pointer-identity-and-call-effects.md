# RFC 0014: Pointer identity and precise call effects

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-06
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0004's indirect callees,
  RFC 0005's exported summaries, RFC 0006's written objects, RFC 0009's
  guards, and RFC 0013's heap identities and projection.

## Summary

Preserve the identity of data and function pointers through ordinary C
operations and apply only the call effects that their values permit.
Function pointers have bounded sets of actual targets, including an explicit
unknown alternative. Pointer equality and inequality can guard ownership
effects across calls. Complete pointer and record copies through `memcpy`
and `memmove` preserve their pointer facts. Summary paths respect the object
views in which fields exist. These facts cross helpers, translation units,
and compiler sidecars, with loss of analysis coverage visible to the user.

This RFC was drafted before implementation. The project owner explicitly
requested drafting the RFC and then implementing Option 1 end to end. Moving
it to Accepted records that authorization, not a separate review or merge.

## Motivation

RFC 0013 preserves constructors' reachable heaps, but the surrounding call
and memory operations can still associate those facts with the wrong object
or erase an ordinary pointer copy. The following were reproduced on the
pre-change compiler, including with `--strict-externs`:

```c
static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
void (*unrelated)(void *) = drop;
void example(int *p) {
  void (*cb)(void *) = keep;
  cb(p);
  *p = 1;                         // currently a false use-after-free
}

static void maybe_drop(int *p, int *q) { if (p == q) free(p); }
void guarded(int *p, int *q) {
  if (p != q) {
    maybe_drop(p, q);
    *p = 1;                       // currently a false use-after-free
  }
}

void copy(void) {
  int *p = malloc(sizeof *p), *q;
  if (!p) return;
  memcpy(&q, &p, sizeof p);
  free(p);
  *q = 1;                         // currently missed
}
```

The recorded corpus has 2,744 reports after RFC 0013, up from 959. Lua's
callback pool applies parser field effects to unrelated userdata. Other
reports depend on GC and stack-rebasing invariants, which this RFC does not
claim to infer. Reducing a total report count is insufficient evidence of
improvement: the eliminated false positives need unsafe counterparts.

## Soundness

### Bugs caught

- Use after free, double release, leak and invalid release through supported
  complete memory copies, including pointer fields and their children.
- Consumption through a known function pointer, through a helper invoking a
  supplied callback, and through cross-file registration or returned hooks.
- Both possible callback effects at an actual control-flow merge.
- A conditional release when the caller's pointers satisfy the callee's
  equality guard, including through aliases and overwritten input cells.
- An unknown callback remains an unchecked boundary even if an unrelated
  function of its type is known. A known alternative does not erase unknown.

### Bugs deliberately not caught

This is not verification mode. Unknown bounds, C integer overflow, arbitrary
byte encodings of pointers, concurrency, GC reachability, pointer relocation
and general container invariants retain their prior limitations. This RFC
does not add runtime checks, a general theorem prover or archive packaging.

Complete copies mean an entire pointer object or an entire compatible record
object of a known byte size. Partial, unresolved or incompatible copies must
invalidate affected must-facts and mark coverage incomplete; they cannot be
represented as a complete copy. They need not reconstruct pointers from
arbitrary byte fragments. Unbounded array identities remain RFC 0006's
summary elements.

Calls through externally supplied hooks can only be proved against explicit
contracts or resolved target values. A function body with a symbolic callback
can export its invocation requirement to callers; it does not become checked
merely because some same-type function is address-taken. Conditional or
unrepresentable invocation compositions retain uncertainty.

### Accepted false positives

Finite target sets join by union. A callback with several possible values
has all their possible effects; correlations not preserved by the bounded
representation may still cause reports. Pointer guards describe conjunctions
of equalities and inequalities, not arbitrary Boolean predicates. Losing a
guard weakens its condition and may make a consume unconditional. An object
view that cannot be established cannot justify a field-sensitive update.

### Assumptions

Earlier RFC assumptions continue to apply: single-threaded execution, the
summarized definition is the one actually called, declared contracts and
explicit unsafe assertions are trusted, and the target layout is Clang's.
An exact memory copy uses C's ordinary object-representation semantics;
`memmove` captures all source values before any destination is overwritten.
Function identities distinguish internal linkage functions in different
translation units. No Clang or LLVM dependency enters Core.

## Detailed design

### 1. Function values and indirect calls

Represent function values independently of data ownership. A function pointer
is not an allocation and creates no release obligation. A target set contains
stable function identities and an explicit unknown alternative. An empty
reachable set is not proof of a harmless call. Null remains distinguishable
from an unknown non-null target for validity checking.

A direct function designator introduces its identity; assignments, copies,
conditionals, field stores, initializers, returns and out-parameters propagate
it. A strong assignment replaces a previous set. CFG joins union alternatives;
a missing reachable value introduces uncertainty rather than acting as bottom.
A test of a known function pointer can refine alternatives. Matching C types
constrains compatibility but is not evidence that a particular function flows
to a particular call.

Target information survives helpers using bounded specialization. A generic
summary records the interface paths through which it invokes callbacks.
A caller binds those paths to its actual target sets and asks for a summary
of that same body under those bindings. The body then passes its own userdata
arguments to the selected target; separate bindings do not form a cross-product
of unrelated target and userdata values. Returned function values and stores
use the ordinary value-source and heap summary mechanisms.

Local bodies can be specialized on demand. Cross-file callers export requests;
the program database delivers them to the defining unit, which exports the
specialized summaries. Callback-capable interfaces add reverse scheduling dependencies, and the
existing SCC fixpoint carries requests and answers before final reporting.
References to external function values, including global initializers, also
introduce dependencies. Generic invocation requirements are deferred when concrete contexts
are available; each concrete body context is still checked, including uses
and releases after a callback. Unresolved inputs remain boundaries. This is
bounded specialization of the existing dataflow, not replay of an inferred
operation sequence: it preserves branch and statement ordering in the body.

Use deterministic bounds: at most 32 alternatives in a function value and
32 binding contexts per function, with the existing eight-step path bound.
Exceeding a bound retains unknown coverage. Cycles must converge by ordinary
summary iteration with a recursion guard on active contexts. Nonconvergence
or a requested context not available at a boundary is incomplete, not empty.

A declaration's ownership contract remains authoritative. The actual target
is still checked against its own declaration/body as before. Otherwise an
indirect call uses its actual target summaries. Unknown targets use the normal
boundary behavior (`annotation-required`, or `unsafe-operation` in strict
mode), even alongside known alternatives; known effects still apply.

Call-graph dependencies include the known target functions and dependencies
needed to resolve symbolic bindings. A conservative scheduling graph may
include extra edges, but those edges must not add effects to a resolved call.
Global callback entry values include initializers and possible stores from
all analyzed functions, including stores through extern declarations in other
units. A unit repeats silent inference when those global values change,
with the existing 16-round limit. Hitting that limit records incomplete
coverage. Discovery and inference remain separate from final reporting.

### 2. Pointer comparisons as guards

Extend the bounded guard vocabulary with canonical unordered pairs of pointer
keys plus an equality/inequality bit. State keys are places; summary keys are
interface paths. `p == q` and `q == p` name the same predicate. This is address
equality, not equality of the containing allocations: two different interior
pointers may still share an allocation.

The path learns comparison facts on CFG edges and transfers them through exact
copies. Joins retain only facts both predecessors justify. Reassignment,
unknown writes and lifetime retirement invalidate predicates involving the
old value unless an existing RFC 0013 input identity preserves it. A definite
copy can discharge equality; absence of a may-alias edge is not sufficient
evidence for inequality.

Consumes, stores, returned alternatives and heap publication can carry these
predicates using the same guard mechanism as scalar facts. Consuming paths
join their guards conservatively. Translate both sides at the call before
applying writes; use input snapshots when a call replaces an operand. A guard
refuted by caller facts removes that effect or alternative. A guard not
proved either way continues to describe a possible effect.

The total guard remains bounded. Comparing a pointer after a possible release remains a pointer-value use under
the existing rules; learning an unequal edge can retract a guarded consume for
subsequent operations. Dropping a conjunct weakens the guard; it
must never turn uncertainty into proof that an effect cannot happen. Existing
scalar guard parsing and behavior remain compatible within the new version.

### 3. Complete memory copies

Recognize libc `memcpy`, `memmove` and their fortified builtin spellings only
when their resolved summary is the shipped library contract. User definitions
with those names retain their own behavior.

Determine source and destination storage and the number of bytes using
Clang's target layout and the existing scalar facts. For an exact pointer
copy, capture the source pointer value before invalidation and perform the
normal pointer assignment. For an exact record copy, capture every known
pointer field (including nested fields), then restore its identity, loans,
moves, ownership, raw/null facts, extents, string facts and heap children in
the destination. Existing source/destination aliasing is handled as a
simultaneous copy. A self-copy preserves the existing facts.

Use the ordinary overwrite/leak checks for values replaced in the destination.
The library's buffer bounds checks still run. A copy does not independently
allocate its source's children or acquire another counted share. The result
still aliases the destination as the library table specifies.

When a helper copies caller memory, export the pointer stores and final heap
postconditions needed to make callers observe the same copy. If the byte
range or object view is unsupported, forget the affected must-facts and record
incomplete coverage. No blanket clearing of temporal evidence on unaffected
objects is allowed.

### 4. Object views and field paths

Summary application must distinguish paths that exist in a caller object from
synthetic paths imported through erased types. Field metadata records enough
of the defining view to validate complete compatible record paths. Named
fields alone are insufficient to reinterpret unrelated records.

A direct typed path, a valid embedded-record path, or a recovered view through
a supported pointer cast can identify a real cell. Record layout is supplied
by Analysis; Core keeps only frontend-neutral keys. At an incompatible view,
do not manufacture the field and do not silently discard all effects: retain
an incomplete application/boundary and preserve conservative effects on the
reachable object. `void *` interfaces may regain the actual argument's view;
they are not themselves evidence of incompatibility.

### 5. Coverage and explanations

Summaries and analysis dumps distinguish complete target resolution from
unknown targets, unsupported memory copies, incompatible object views and
analysis limits. A limit is a loss of coverage, never a successful proof.
Surface incomplete analysis through a stable warning where no existing
boundary diagnostic accounts for it. Deduplicate per source operation and
reason. `WEAVEC_UNSAFE` follows existing report suppression rules while the
summary still records what was not modeled.

Ownership reports caused by an indirect call name its resolved target(s) or
carry a note at the responsible call. Conditional effects show the pointer
comparison when it remains relevant. Debug output exposes the target and
binding facts so corpus reports can be traced to their source.

### 6. Serialization and whole-program behavior

Summary and sidecar formats become version 10. Serialize function values,
specialization bindings and requests, pointer predicates, object-view metadata and incomplete
coverage deterministically. Validate bounds and malformed references on read.
Global remapping must visit new fields. Internal function identities include
the defining translation unit; exported functions retain their external name.
A cross-file registration and its consumers participate in dependency
invalidation, including dependencies discovered during inference.

Local inference and whole-program reanalysis must reach the same result for
the supported cases. Compiler sidecars preserve the facts required for that
result. Source order, unrelated address-taken functions and equivalent
forwarding helpers must not alter resolved target effects.

### 7. Validation and performance

The acceptance matrix includes local variables, conditional targets, struct
fields, global registration, callbacks with userdata, returned hooks,
out-parameters, forwarding wrappers, recursive dependencies and cross-file
variants. Each has clean and faulty counterparts. In particular:

- The three motivation examples receive their intended results.
- Replacing `keep` with `drop` still reports the later use and release.
- Unknown callbacks stay boundaries despite same-type known candidates.
- Separate callback/userdata bindings do not exchange field effects.
- Equality-guarded releases fire for equal arguments and do not fire under a
  proven inequality; overwritten guard operands use incoming values.
- Exact copies carry both invalid temporal state and valid bounds/children.
- Partial copies, incompatible views and exceeded limits expose incomplete
  coverage rather than claiming safety.
- Summary round trips and driver sidecars agree with tooling analysis.

Retain all existing fixed evaluation bugs, including known misses. Add a fixed
RFC 0014 matrix; use the same denominator and independent execution failure
accounting. Make analysis unit helpers reject unexpected Clang parse errors;
repair tests relying accidentally on recovered invalid ASTs. Investigate the
two existing lit zero-output failures. Add a pinned corpus subset suitable for
PR CI, preserving failures and measuring diagnostics, wall time and peak
memory. Review changes by root cause, not just by total count. Run the larger
local corpus, including Lua, on fixed revisions. No clean-Lua target is claimed.

Keep new mechanisms in focused Core and Analysis files rather than adding all
implementation to `Dataflow.cpp`. Reuse existing value snapshots, heap facts,
summary joins and diagnostic infrastructure. Measure the release build before
and after on the same revisions and machine; report observed time and memory
cost honestly. Bound any specialization, invocation expansion and target union.

## Annotation surface

No new ownership annotation. Existing function-pointer contracts and
`WEAVEC_UNSAFE` retain their meaning. No source edits are required for the
supported callback, comparison and copy patterns.

## Diagnostics

Existing ownership and validity IDs retain their messages. An unknown
indirect call's boundary message describes unresolved target values instead
of suggesting that taking an unrelated function's address makes it checked.
Strict mode continues to report `unsafe-operation` for that boundary.

New stable ID: `analysis-incomplete`, warning. Primary message:

`analysis is incomplete: <reason>`

Reasons identify an unsupported memory copy, an incompatible object view, a
function-target/invocation limit or a nonconverging analysis. The location is
the responsible call or function. A note may identify the affected object or
callback. For example, copying an unknown number of bytes into a pointer cell
cannot preserve its complete representation. The warning is distinct from a
memory-safety violation and does not claim the operation is necessarily wrong.
It is documented and controllable by the existing warning machinery.

## Drawbacks

Richer summaries and target propagation increase state size and coupling
between inference and call resolution. The finite model still loses
correlations; making those losses visible can add warnings. Replacing the
same-type callback pool changes diagnostics of existing code and tests that
implicitly relied on its closed-world assumption. Exact memory copies require
care around aliasing, overlapping ranges and destination-owned children.

## Alternatives

- Keep type-wide callback joins: simple, but reproduces the motivating false
  positives and falsely resolves externally supplied hooks.
- Suppress callback or GC reports globally: loses actual detections and gives
  no account of the ownership contract.
- General context-sensitive symbolic execution: potentially more precise,
  but substantially larger and harder to bound than this milestone.
- Verification mode first: useful later, but would reject the common patterns
  whose inference this RFC improves.
- Rewrite the checker around a new IR: unnecessary for the bounded facts and
  complete-copy operations here; preserve the existing layered architecture.

## Prior art

The existing RFCs are the immediate design references: RFC 0004 introduced
function-pointer contracts, RFC 0009 bounded guarded effects, and RFC 0013
immutable incoming values and heap postconditions. This proposal applies
those mechanisms to function identity and pointer relationships. Clang's
AST record layout supplies the byte-level facts; the ownership model remains
independent of Clang as required by RFC 0001.

## Unresolved questions

No separate approval is required before implementation under the owner's
explicit instruction. Implementation must record any refinements to the
bounded representation here before encoding a new model decision. Corpus
precision and performance are empirical acceptance results, not promises
that every count will decrease.

## Future work

Verification mode, richer integer/bounds reasoning, arbitrary element
identities, regions and GC invariants, runtime enforcement, and production
archive/library packaging remain separate milestones.

## Implementation and validation

Implemented in the Core target/guard domains, focused callback, memory-copy
and view analysis files, the program database and version 10 sidecars. Global
callback values settle before final reporting, and immutable layout keys are
cached for the lifetime of their Clang AST. Top-level cv-qualification and
array-to-pointer decay preserve a record's layout.

The [validation report](../validation-rfc0014.md) records the full acceptance
runs and corpus tradeoffs: 637 passing CTest entries in both Debug and
ASan/UBSan; 67/67 retained recall detections; 18/20 fixed evaluation bugs with
8/8 clean cases and the original two misses retained. All corpus executions
completed, but 682 incomplete-coverage warnings remain and Lua costs more
time and memory. Those limitations are part of the recorded outcome, not
claims of verification or a clean Lua analysis.
