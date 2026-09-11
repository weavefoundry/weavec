# RFC 0022: Compositional checked interfaces for opaque pointers and callbacks

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-11
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFCs 0014/0016's callback and
  contextual analysis, RFCs 0018/0019's checked interfaces, and RFC 0020's
  dependency-safe transport. Qualifies RFC 0018's pointer-cast exclusion.

## Summary

Preserve checked memory contracts through ordinary C abstraction boundaries:
opaque pointer round trips, synchronous callbacks, configurable allocation
functions, forwarding helpers, and separate compilation. A callback's C type
alone is not its memory contract. Resolve actual functions and instantiate their
requirements and guaranteed outputs at the invocation, preserving the association
with their arguments. Opaque pointers preserve the identity and type evidence
of the object they came from; restoring a typed view requires evidence rather
than an unchecked cast. Existing unknown alternatives and limits remain visible.

The project owner explicitly requested drafting the RFC first and then implementing
the milestone end to end. The initial Accepted status recorded that authorization,
not an independent review or a merge. All amended acceptance gates now pass;
the [validation record](../validation-rfc0022.md) publishes the measurements,
exact diagnostic changes and the demonstrated baseline false-proof correction.

## Motivation

At `4c455cd`, 1,082 CTest entries pass. The frozen RFC 0021 corpus completes
120 of 1,619 selected function contracts. These are conditional contracts,
not complete checked projects. The following valid program is incomplete:

```c
static void *identity(void *p) { return p; }
int main(void) {
    int value = 7;
    int *p = identity(&value);
    return *p;
}
```

A callback that fills a byte, passed through `invoke(fn, p)`, also fails to
establish its caller's subsequent read. Calling `malloc` and `free` through a
hooks structure loses the checked library interpretation. RFC 0019's Jansson
buffer evaluation substitutes allocator adapters for the actual configurable
allocation interface. These restrictions prevent existing buffer and ownership
proofs from composing through reusable C code.

## Soundness

### Guarantee and assumptions

Retain the conditional source guarantee of RFC 0018. Every selected reachable
operation needs positive evidence, an exported sufficient requirement, or an
explicit recorded trusted boundary. A complete helper's requirements must be
established at its call. Checking is single-threaded with Clang's target layout
and C conversions, without runtime instrumentation or a pointer ABI change.

Type evidence does not establish initialization, validity, write permission,
extent, or permission to release. Each remains a separate obligation. Known
callee identities do not prove the safety of their arguments. A pointer returned
through a wrapper is still dead if the wrapper released its allocation.

### Required distinctions

- Restore an original object view after a `void *` round trip; reject an
  incompatible object, unproved alignment, or an offset not identifying that view.
- An initialized pointer holder does not imply initialized pointee bytes.
- A callback that writes every requested byte establishes those bytes. A
  callback that writes fewer, skips a store, or frees the object does not.
- Different target/argument bindings remain different contexts. A safe target
  must not erase another reachable target's failure or unknown alternative.
- Allocation through a known hook retains its actual extent, family and
  nullable outcome. An insufficient allocation, incompatible release, or stale
  alias after replacement cannot pass because the C function types agree.
- Global hook mutation invalidates facts about the previous binding. Installing
  a callback does not extend the lifetime of userdata it retains.
- Direct, forwarded and indirect forms of the same supported call preserve
  applicable checked requirements, output facts, trust and diagnostics.

### Conservative rejections and exclusions

Arbitrary type punning, enclosing-record recovery, tagged unions, encoded
pointers, concurrent/asynchronous callback protocols, dynamic loading and
recursive heap invariants remain separate work. Unknown callbacks retain an
incomplete boundary; no implicit purity or allocator convention follows from
their names or prototypes. This milestone adds no user contract language for
an unavailable callback implementation. Existing explicit unsafe/annotation
boundaries retain their scope and never invent missing output facts.

Only bounded callback specialization is supported. Generic interfaces describe
callback dependencies and the sufficient contracts of resolved specializations;
they do not assert a proof for all functions of a prototype. An open generic
body may remain incomplete while a caller with established bindings is complete.

## Detailed design

### 1. Object-view requirements

Add a checked `object-type` requirement. Its interface path identifies the
incoming pointer; its type descriptor uses target-aware canonical type/layout
identity. Block-scope and anonymous tags retain their declaration identity;
equal layouts alone do not make distinct C types compatible. It requires that
the pointer designate a compatible object view at the recorded byte offset with suitable target alignment. It supplies no other
memory property. Descriptors are data, never source code or executable predicates.

An erased incoming pointer recovered to a typed pointer may export this
requirement. A caller discharges it using original declared storage, an existing
compatible view, or a represented allocation eligible for that view. Copying,
returning and storing a pointer retain original object identity and offset;
casting does not itself create a typed object. Compatible qualification changes
preserve the view and continue to obey writable-storage checks.

Move cast accounting from the unconditional syntax exclusion to the evaluated
CFG point for supported opaque recoveries. Unreachable or unevaluated casts
must not create executed memory obligations. Unrelated typed record casts remain
unsupported. Unknown alignment, identity or layout is unresolved. A known
incompatible view fails acceptance even when byte extents happen to match.

Use a dedicated Analysis helper for type descriptors and recovery. Declared
member alignment, including packed layout, constrains recovered views independently
of the natural alignment of the member type. Typedef-supplied alignment must be
read before canonicalizing type identity. An over-aligned scalar whose layout
the descriptor cannot represent remains unresolved; stripping typedef sugar
cannot weaken its required alignment. A returned fresh allocation keeps
any object-view postcondition established by its allocator body. Core stores
portable descriptors and requirement semantics without Clang includes. Follow
existing must-fact intersection at joins and invalidation on replacement,
unknown writes, release and lifetime end. Do not use may aliases as type proof.

### 2. Checked callback invocation

Keep RFC 0014's actual reaching function-value sets, including explicit null
and unknown alternatives. Preserve the resolved source/identity of each target
through contextualization; joining targets must not discard builtin semantics
or treat mixed alternatives as one arbitrarily chosen builtin.

At a call, apply each applicable target's contract under the same pre-call
argument identities. Requirements combine conservatively and guaranteed outputs
intersect over returning alternatives. Target-specific effects retain source
ordering through the existing body specialization. The function-pointer value
and all data arguments have their own initialization and validity obligations.

Correct checked path projection for callback forwarding: parameter indices are
always relative to the contract's defining function, not an enclosing wrapper.
Captures use immutable call-entry values before any argument/output holder is
overwritten. A callback may initialize caller storage, return an alias or fresh
allocation, or replace an output pointer. Forwarders export only guarantees
established on every applicable return path.

An initialized/zeroed/copied output can additionally carry `if-nonnull`: its
guarantee applies only when the final pointer at its output path is non-null.
This is an output-value condition, not an entry assumption. Export it only
when removing that exact final pointer's non-null guard makes the remaining
premises portable. Capture input premises before the call, resolve the output
pointer after writes, and install a guarded fact without assuming non-nullness.
Caller replacement retires the guard and its dependent memory evidence.
Requirements cannot carry this flag. Checked encoding 4 and report output
record it explicitly; older metadata must not silently make it unconditional.
When intersecting return edges, an edge that proves the final output pointer
null satisfies an `if-nonnull` guarantee vacuously. Track the intersection of
such final-null paths across earlier edges, so return visitation order cannot
invent or remove a conditional guarantee. An unknown or non-null output edge
must establish the actual bytes. The null-path set has the existing checked
requirement bound; overflow loses precision.

Expose nested/global callback inputs when their values are supplied through a
helper interface or mutable hook registry. Capture established caller bindings
for these paths, and retain generic unknown alternatives when not established.
No default initializer may override a reachable later store. Setter summaries
transport their actual final function values. Callback and userdata paths
remain paired by the analyzed invocation rather than a type-wide cross product.

An unresolved global binding identical to the generic global target set adds
no premise: it does not establish nullness or exclude any candidate. Omit such
bindings from specialization keys while retaining the global dependency and
the helper's callback input. Concrete caller bindings continue to specialize.
This avoids repeatedly analyzing the generic unknown alternative as a new
context; it does not change target bounds or discard an unknown target.

Private file-scope scalar function-pointer cells are a narrow exception to RFC 0005's
private-global exclusion. Their portable names encode the defining source and
declaration identity. A foreign unit represents such a cell with an internal
function-pointer storage proxy, never a C declaration visible to source lookup.
The proxy supplies only the cell's identity and pointer representation: target
sets come from represented stores or conservatively joined global information.
It supplies no callback contract, userdata, or pointee evidence. On return to
the defining unit the same name resolves to the original declaration. Other
private globals retain the existing exclusion. Global callback bindings use
the same strict name remapping as memory contexts; missing names invalidate a
context instead of dropping a premise. Sidecars encode names, not local ids.

Use existing bounds: 32 targets, 32 callback contexts, 32 memory contexts,
eight nested contextual analyses and eight path steps. Hitting a bound remains
explicit incomplete coverage. No budget increases are part of this design.

### 3. Allocation hooks and library identity

Make checked builtin dispatch use a verified resolved callable identity, not
only `CallExpr::getDirectCallee()` and its source spelling. The existing checked
allocation, release, reallocation, memory and string contracts are available
through function pointers when those exact library targets are established.
An overriding user definition with the same name retains its own body semantics.

Preserve allocator family, size expressions, initialized ranges and null/error
outcomes through hook wrappers and returned pointers. Preserve input aliases on
failed replacement and invalidate saved old aliases on success. Custom hooks
with available bodies derive their contracts normally. C type compatibility does
not imply compatibility of allocation/release families or sufficient capacity.

Support initialized hook records in local/static storage and copies without
assuming their userdata pointees are initialized. Reads of global scalar and
function-pointer cells may rely on C static initialization and represented stores;
reads of memory reached through a global pointer require separate evidence.

Changing a hook cannot retroactively change the release family of previously
allocated objects. Unknown hook state remains unresolved at dependent operations.
Positive-size reallocation retains RFC 0019's model; target-dependent zero-size
library behavior is not inferred as a new guarantee.

### 4. Transport, reports and organization

Transport object-type requirements and any additional callback interface facts
through summaries, program-database remapping, compiler sidecars and persistent
checkpoints. Bump summary and sidecar versions for semantic additions and the
checked-record encoding when its grammar changes. Reject malformed descriptors,
invalid paths, duplicate or oversized metadata and missing context premises.

Sidecar format 18 writes a bounded `global-name` prelude containing the unit's
portable global names in its table order, including unused names. References
still use names, never cross-unit numeric ids. The reader interns this prelude
before reading facts so first-use order cannot reorder callback bindings,
requests or specializations during a checkpoint round trip. Reject duplicate,
invalid or more than 65,536 name records. The name table supplies identity only;
it does not declare C variables or supply any memory or callback facts.

Reports preserve sufficient entry requirements, target dependencies, originating
obligations and trust. Conditional helper acceptance is distinguishable from a
closed caller with zero requirements. Failed calls retain the callback body's
origin and the caller route. Diagnostic severity controls do not discharge them.

Dependency fingerprints include callable lookup results, missing targets,
global hook facts and the context premises that affect a specialization. Cache
reuse must agree with uncached analysis after edits to callback bodies, hook
initializers/setters, object layouts, headers and compiler arguments.

Keep new logic in focused helpers. Reuse the existing CFG and immutable function
preparation. Neither a second call interpreter nor a broad Dataflow.cpp rewrite
is part of this milestone.

### 5. Frozen acceptance

Preserve the RFC 0021 Release binary and identify it by SHA-256 before rebuilding.
Freeze source populations and expectations before checker edits. Add paired
positive/negative cases covering:

1. Local and returned opaque pointer recovery, including incompatible types.
2. Opaque userdata forwarded through a source callback; lifetime and const cases.
3. Full and partial callback initialization, callback release and saved aliases.
4. Multiple callbacks, null/unknown alternatives and changing bindings.
5. Default malloc/free hooks and compatible source wrappers.
6. Insufficient allocation, wrong release family and replacement outcomes.
7. Hook records, global setter forwarding and copied callback values.
8. Output pointers and nested helper argument-index translation.
9. Separate source units, compiler objects and cold/warm checkpoints.

Every positive closed caller must have zero entry requirements, no limits or
deferral, and no new unsafe/annotation trust. Existing modeled libc trust is
recorded. Negative cases must fail for their intended memory property, never
merely because of parse errors, tool failures or an unrelated boundary.

Freeze unchanged Jansson `memory.c` allocation wrappers and selected `strbuffer.c`
interfaces with concrete compatible default/custom hooks and adversarial callers.
Use actual upstream definitions instead of RFC 0019's allocator adapter. Include
the hook setters necessary for these clients. The selected contracts may state
requirements such as positive reallocation size; closed callers must discharge
them. Frozen populations and expected function identities are recorded in
`test/evaluation/rfc0022/` before implementation. Add a second independent
hook-based client family to avoid specializing on Jansson's names or layout.

Run the full Debug and ASan/UBSan CTest suites, relevant strict clang-tidy,
formatting and the Core include-boundary check. Preserve all existing fixed
bug/clean expectations and audit all 120 RFC 0021 complete selected function
identities. Preserve the 119 unaffected identities; the one demonstrated false
proof described below must become incomplete. Report the raw 119/120 retention
separately from gains, without describing this as 120 preserved proofs.
Add independent checks of object-view matching, requirement transport, joins,
callback alternatives and invalidation; malformed metadata must not be accepted.

Measure the same pinned five-project corpus sequentially with Release builds
and matching LLVM/flags after tests/builds finish. Three ordinary baseline and
final observations must have median total runtime and peak RSS ratios at most
1.10. Every uncached/cold checked report must finish within 600 seconds per
project. Preserve semantic budgets, the 51 warm unit hits and zero warm function
analyses. Compare canonical uncached/cold/warm reports and diagnostics, and record
all added/removed diagnostics with their locations and reasons. Investigate
changes without declaring unverified upstream findings to be bugs.

## Annotation surface

None. Available callback implementations supply inferred contracts. The existing
ownership, bounds and explicit unsafe annotations retain their meanings. An
annotation on a pointer's C type is not a complete checked callback contract.

## Diagnostics

Use the existing stable `checking-incomplete`, `checking-failed` and ordinary
memory diagnostic IDs. Add precise checked reasons for object-view recovery and
callback requirements, retaining call origins. A mismatched recovery is not
silenced by matching object sizes. Lit tests pin the new user-visible reasons.

## Drawbacks

More contexts and object-view evidence increase inference and metadata cost.
Global hook mutation can still force conservative rejection. Bounded matching
does not cover all legal C type manipulations. Accurate initialized-range
projection and target alternatives need adversarial validation because an
incorrect must-output can falsely establish a caller's read.

## Alternatives

Implement recursive containers first, add broader libc/POSIX models, or ship
archive transport first. Each is useful but leaves basic opaque interfaces and
allocator hooks unable to carry existing proofs. Treating all `void *` casts as
safe, all callbacks as pure, or every allocator-shaped function as `malloc`
would hide missing evidence. A universal callback contract language and general
solver are larger designs than the bounded inferred interfaces selected here.

## Prior art

RFC 0013 supplies output object identity; RFC 0014 supplies actual callback
targets and bindings; RFC 0016 supplies argument relationships and contextual
body checking; RFC 0019 supplies sufficient memory requirements and guaranteed
outputs; RFC 0020 supplies dependency-safe reuse. This milestone connects those
mechanisms while keeping type evidence independent of memory safety properties.

## Unresolved questions

Internal indexing, helper boundaries and canonical descriptor encoding are
implementation choices. Scope, trust, limits and acceptance populations are
fixed here. Newly discovered semantic decisions require an explicit amendment
before implementation; failed cases cannot be removed to make validation pass.

The positive retained-userdata fixture was corrected during initial validation:
it now clears the registration after the valid read, before the local dies.
The original fixture left a dangling registration, contrary to RFC 0011's
existing lifetime discipline. Original hashes and baseline reports are retained;
the case population and intended lifetime distinction are unchanged.

A supplementary rerun of RFC 0019's original upstream adapter evaluation found
7/8 case expectations satisfied by the preserved RFC 0021 baseline. Its generic
`strbuffer_append_byte` and `strbuffer_append_bytes` contracts are already
incomplete, while the closed lifecycle caller is complete. Retain this audit,
its original expectations and exact baseline function outcomes; do not claim
8/8 or attribute the pre-existing failure to this milestone. The new frozen
32-case and five-client populations must still pass in full.

### Audited false proof in the corpus baseline

The completed Lua comparison found one lost baseline-complete identity:
`lua.c#handle_luainit`. Its two calls through `l_getenv` can invoke either
`getenv` or `no_getenv`. The baseline joined the latter's complete null-returning
contract with `getenv`, which has no checked contract, and reported both calls
complete. It then treated the result as always null, ignoring the remaining
branches and reporting the whole function proven with zero requirements.

A reduced C counterexample reproduces this on the preserved executable: select
between `getenv` and a null-returning callback, and dereference a null pointer
when the returned string is non-null. The baseline reports its client complete
with no requirements; RFC 0022's per-target checking rejects the missing
`getenv` contract. A concrete setter selecting only the null-returning callback
still permits a complete closed caller. Supplemental regression fixtures retain
both cases and the original baseline comparison.

This is an explicit correction to the preservation criterion, justified by a
demonstrated false proof, not a precision exception for an unexplained loss.
Require exactly this lost identity and preserve the other 119. Retain all 120
original identities, the before/after reports and the raw loss in publication.
Any additional loss still fails acceptance. Do not add a trusted `getenv` model,
discard the unknown contract, or restore the incorrect null-only conclusion to
recover the old count. All frozen bug/clean expectations remain unchanged.

## Future work

Recursive containers, tagged layouts, a declarative contract language for
unavailable callbacks, asynchronous/concurrent protocols, broad I/O and formatting
models, archive distribution, runtime enforcement and independent certificates.
