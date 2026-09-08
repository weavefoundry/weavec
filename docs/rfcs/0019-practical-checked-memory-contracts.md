# RFC 0019: Practical checked memory contracts for buffers and heap objects

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-08
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFC 0018's checked contracts and
  RFCs 0013/0015's heap and array postconditions. Amends checked call and
  initialization transfer, without replacing the ordinary diagnostic model.

## Summary

Make checked memory facts compose through ordinary C buffer interfaces.
Requirements can name nested pointer paths; initialized outputs can name
returned allocations, records and out-parameters. Conditions and return
outcomes qualify contracts. Complete fill/copy loops establish initialized
ranges. Represented offsets distinguish disjoint slices of one object, and
checked string and allocation models cover the common buffer lifecycle.
Retain evidence across calls with complete effects, with explicit trust for
modeled external operations. Transport the facts through whole-program
analysis and compiler sidecars, and test useful positive programs as well as
rejections. Ordinary checking remains available and unchanged in policy.

The owner requested an RFC first followed by end-to-end implementation.
Acceptance after drafting records that authorization, not independent review
or a merge. Implementation must satisfy the frozen evaluation and cost gates
before this RFC becomes Implemented.

## Motivation

At `ed3f712`, the original 44-bug/32-clean evaluation, the 24 RFC 0017 cases,
and the 18 RFC 0018 cases pass. Nevertheless, checked mode rejects:

```c
int main(void) {
  char a[8];
  for (unsigned i = 0; i < 8; ++i) a[i] = 1;
  return a[7];
}
```

It also rejects an initialized allocation returned by a constructor, an
initialized buffer behind `b->data`, a successful out-parameter constructor,
`strlen` on a terminated array, `realloc` preserving initialized data, and
`memcpy(a + 4, a, 4)` on an eight-byte initialized array. These are missing
positive proofs, not newly discovered C bugs. Root-only checked paths and
postconditions do not yet exploit the richer ordinary heap representation.

RFC 0018's recorded whole-project checked results complete 3/12 selected
functions in log.c, 16/151 in cJSON, 18/88 in linenoise and 13/211 in Jansson.
cJSON and Jansson also reach iteration limits; Lua times out at 600 seconds.
The next milestone needs recognizable checked code and bounded analysis cost.

## Soundness

### Guarantee and assumptions

Preserve RFC 0018's conditional guarantee for selected source functions under
reported entry requirements and trusted boundaries. Every caller establishes
its callee's applicable requirements. An unresolved requirement, incomplete
path translation, inconsistent object view or exhausted limit cannot become
an empty contract. The execution model is single-threaded C under Clang's
target types and layout, with the existing external-library and unsafe trust.
There is no pointer ABI change or runtime instrumentation.

Byte initialization, initialized pointer representations, pointer provenance,
nullness, writability and lifetime are separate properties. Initialized bytes
alone cannot fabricate a pointer, infer a null pointer from arbitrary byte
zeroing, revive a freed object, or validate a type reinterpretation.

### Required positive and negative distinctions

- A constructor that writes a byte on every successful return establishes
  that byte. A path returning an allocated but unwritten byte does not.
- A successful out-parameter constructor publishes its final initialized
  object. Failure leaves exactly the state its body establishes.
- A replacement preserves applicable bytes of the replacement object;
  saved aliases of a released incoming object remain invalid.
- Complete initialization loops establish their covered interval; a skipped
  store, early exit, changed bound or unproved induction cannot establish it.
- Complete copies preserve exactly the initialized source portion and the
  existing pointer representation model. `memcpy` requires nonoverlap;
  `memmove` uses the source state before the move.
- A terminated initialized string permits a terminator-seeking read. An
  initialized array without a terminator does not.
- A callee's writes invalidate affected evidence. A may-alias relation is
  insufficient for a strong update or a claim of disjointness.

### Conservative rejections and exclusions

General recursive shape invariants, tracing collectors, concurrency, signals,
nonlocal jumps, assembly, union/type punning and partially encoded pointers
remain outside the supported model. Arbitrary nonlinear arithmetic and loop
induction remain unresolved. Unsupported library/variadic formatting behavior
continues to need explicit trust. Unknown input relationships can require
conservative separation. A function with representable sufficient conditions
can be conditional without every possible caller being admissible.

## Detailed design

### 1. Object-relative memory evidence

Extend the checked memory projection to resolve the same exact identities,
record layout and selected cells used by the ordinary checker. A memory
interval has a storage identity, mathematical byte start/end, an optional
extent, and an optional stable interface path. A pointer holder is distinct
from its pointee's storage. Canonicalize only proven aliases, preserving
offsets; an uncertain identity stays unresolved.

Resolve requirements on parameters with bounded dereference/field/selected
index paths, including `param 0 *.data` and `param 0 *` for an out-parameter.
Reading an intervening pointer cell requires its own initialization, extent
and validity. A nested requirement must not assume that every pointer along
its path exists. Caller projection uses the entry identity before mutations;
postconditions use final output identities after ordinary heap application.

Pointer arithmetic uses mathematical byte offsets after C evaluation of its
operands. Support represented sums/differences of stable byte quantities and
same-object disjoint intervals. Ordered comparisons and differences require
established compatible same-array evidence, including one-past formation;
unrelated pointers do not become comparable because their addresses differ.

For sums of independent nonnegative byte quantities, an unsigned 64-bit
expression represents the mathematical sum only with a `sum-fits` obligation:
both operands are nonnegative and their sum is at most `UINT64_MAX`.
Export that sufficient requirement when its operands are stable inputs;
otherwise require a local proof. Callers discharge it before using the
instantiated interval. Wrapping C addition alone is never a byte-range proof.

### 2. Conditional requirements and postconditions

Checked requirements gain an input guard using the bounded existing
`PathGuard` vocabulary. Requirements are sufficient implications: a disproved
guard makes the requirement inapplicable, a proved guard requires discharge,
and an unknown guard requires conditional caller projection or a conservative
unconditional discharge. Losing a guard may conservatively strengthen an
entry requirement but cannot strengthen an established fact or manufacture a
concrete bug witness. Guard dependencies use stable entry snapshots.

Postconditions additionally distinguish returning outcome classes. They name
parameter/global output paths or `result`, including reachable heap fields.
The supported facts are initialized byte intervals, valid represented pointer
values, known extents, writability and known string termination/length where
the corresponding domain supplies positive evidence. Output facts are
guaranteed only for the applicable return outcome and guard. A null result
does not describe an allocated object.

An initialized-prefix transfer (`copied`) additionally names an incoming
source path. It guarantees that bytes initialized in that source at entry,
within the specified interval, remain initialized in the output at the same
offset. It does not assert that the incoming interval was initialized. This
relational postcondition composes realloc wrappers without requiring unused
allocation capacity to be initialized. Instantiate from call-entry evidence;
later writes to the source cannot retroactively initialize an earlier copy.
Unknown memory effects discard this preservation guarantee. Bounds, lifetime
and release remain separate obligations.

Produce facts at each return before resource escape, including void
fallthrough. Intersect facts across all applicable returning paths. Preserve
the distinction between no return in an outcome and a return missing a fact.
Recursive joins retain only established must-facts; optimistic seed facts
cannot close their own proof. Constructor graph identity follows RFC 0013.

Represent known zero bytes as a refinement of an initialized interval and
transport them as `zeroed` postconditions. A terminator witness within an
initialized accessible prefix proves string termination without asserting
that it is the first NUL or inventing an exact string length. Any possibly
overlapping write invalidates zero-byte evidence; unknown effects invalidate
it along with other memory facts. Joins keep zero evidence only where every
applicable path supplies it.

Instantiate unconditional output facts after call effects, and retain bounded
pending outcome facts until a test establishes the returned outcome. Invalidate
pending facts when the result, output storage or a guard dependency changes.
Never apply an earlier call's facts to a later replacement of the same cell.

Numeric output alternatives may also carry a returning outcome. Union all
applicable alternatives before the result is tested, then retain the union
for the selected outcome after a test. An unknown alternative absorbs only
its own outcome, unless it applies to every outcome. Evaluate output
expressions against call-entry snapshots, including when an output overwrites
an input on which another output depends.
Guards and output references participate in global remapping and dependency
tracking; an unrepresentable mapping invalidates checked completeness.

RFC 0005 excludes private globals from the cross-unit namespace. An optional
postcondition whose output path is wholly rooted in a private global is
therefore omitted when exporting a unit; it remains available for calls
inside that unit. Dropping the entire private output fact cannot strengthen
the remaining guarantees. This exception does not permit dropping an entry
requirement, a private-global premise of a public/result postcondition, or an
unavailable externally visible global. Those still invalidate completeness.

### 3. Initialization ranges and loops

Use must-initialized ranges with interval intersection at control-flow joins.
Preserve exact local/heap identities across assignments, record copies and
complete memory copies. Source initialization is captured before overlapping
moves. Writes invalidate dependencies on the previous numeric value before
establishing the written interval. Limit each object's range description to
32 facts and each contract to 256 requirements and 256 postconditions.

Recognize bounded, unit-stride counted fill/copy loops with stable base,
initial index and upper bound. Prove induction bounds and increment validity
using target integer semantics. Establish the complete covered interval only
on the normal exit and only when the store executes on every covered
iteration. Reject a must-fill proof with conditional/skipped writes, early
exit, unknown calls/writes, reassigned inputs, nonlocal control flow or
unsupported nested induction. Read-only/conditional loop accesses can still
export RFC 0018's sufficient preconditions independently of must-fill proof.
Pointer cursor forms may use the same interval proof when their object,
stride and end are established. Unsupported forms remain explicitly accounted.

### 4. Checked library operations

Keep checked models separate from the ordinary ownership lookup policy.
Model only a resolved builtin/library contract, respecting user definitions
and symbol/type compatibility. Each modeled external operation contributes a
transitive trust record; argument obligations remain independently checked.

- `malloc`, `calloc`, `free` and complete `memset`/`memcpy`/`memmove` retain
  RFC 0018 behavior with improved intervals and identity.
- `realloc` with a positive size requires a nullable allocation base of the
  matching family. Success establishes replacement storage and preserves the
  initialized prefix within `min(old_size, new_size)`; growth leaves the
  added tail uninitialized. Failure retains incoming storage. Zero-size
  behavior follows the supported ordinary target/library model and cannot
  yield invented successful storage or preserved lifetime.
- `strlen` requires an initialized NUL-terminated sequence; `strnlen` requires
  a terminator within the bound or the initialized accessible bounded range.
- `strcpy` and `stpcpy` require the source through its terminator, enough
  writable destination space and nonoverlap; establish copied initialization
  and termination. `strcat` additionally requires the destination's existing
  initialized terminated prefix and enough remaining capacity.
- `strdup` and `strndup` establish initialized terminated successful output
  with the ordinary allocator's null/family/extent facts. Checked arithmetic
  must justify the terminator byte and allocation length.
- `strncpy` writes its bounded destination, including padding; termination
  is established only when justified by the source length and bound.

Fortified/compiler spellings with identical semantics use the same model
with the correct argument positions. Other string/formatting APIs retain
their existing boundary until modeled; ordinary table membership is not proof.

### 5. Call frames and bounded convergence

Complete callee effects identify memory that can change. Preserve initialized
and pointer evidence for objects proven unaffected; invalidate evidence for
written/replaced/consumed reachable storage and its possible aliases. Reapply
only postconditions whose prerequisites and applicability were established.
Unknown effects and unknown unsafe writes invalidate conservatively. A trusted
library call with precise effects does not invalidate every unrelated object
merely because trust propagated through a helper.

Keep semantic requirements/postconditions distinct from diagnostic explanation
identity. Origins remain bounded and stable through recursive propagation;
merging provenance cannot turn an unresolved operation into proof. Maintain
existing context/iteration limits. Fix nonconvergence within the supported
representation rather than increasing limits to pass evaluation. General
recursive proof inference is excluded; a settled incomplete result is valid
coverage reporting, but no successful checked invocation.

Checked calls may reuse RFC 0016's bounded caller contexts for exact scalar
arguments and scalar fields, even without an alias relationship. This lets a
known buffer length/capacity select growth or failure branches. Capture only
established entry facts, keep unknown pointer relationships unknown, and
retain all memory requirements of the specialized body. No extent,
initialization or ownership premise is inferred from a scalar value alone.
Use the existing context count/depth limits and transport; this specialization
is disabled in ordinary mode.

### 6. Transport, reporting and organization

Summary and sidecar format 15 carry conditional memory contracts. Checked
record encoding becomes version 2 and checked JSON schema becomes version 2
with guard/outcome fields. Validate record counts, paths, expressions, guards,
outcomes and remapping. Reject old compiler sidecars with a rebuild message.
Preserve RFC 0018's object/source/header/command bindings and independent
checked failure under diagnostic demotion.

Put contract algebra and range operations in Core, focused memory/path,
output, loop, library and call-frame code in Analysis, and report/sidecar
transport in Frontend. Core remains Clang/LLVM-free. Ordinary analysis does
not allocate or join the optional checked domain.

### 7. Frozen evaluation and acceptance

Freeze `test/evaluation/rfc0019/manifest.json` before implementation. Include
positive/negative pairs for nested fields, returned initialized allocation,
out-parameters, conditional initialization, replacement aliases, fixed and
symbolic fill/copy loops, disjoint/overlapping slices, strings and realloc.
Run applicable cases inline, through helpers and across compiler objects.
Assert selected scope, intended property/reason, completeness, reported entry
requirements and trust; an unrelated parse/tool failure is not a rejection.

Freeze two real-source selections from pinned Jansson revision
`851a2145e3256f2e67e5dfe24b0e456bf198b741`:

1. `src/strbuffer.c`: all eight `strbuffer_*` definitions, with lifecycle
   callers exercising initialization, append/growth, pop, clear, steal and
   close. Its allocator integration is explicitly bound to libc-compatible
   `malloc`/`realloc`/`free` wrappers; configurable allocator callbacks are
   outside this evaluation's selected scope.
2. `src/utf.c`: the encoding interface `utf8_encode` and `utf8_check_first`,
   with callers establishing output capacity and consuming the initialized
   encoded prefix after success. The three decoding/traversal functions are
   unselected and are not claimed checked by this evaluation.

Keep upstream function bodies unchanged and identify all build adapters,
entry requirements and trusted library operations. These are explicit
selected source interfaces, not proof of all Jansson or its allocator hooks.
Add allocation-failure, short-output and stale/partial-state negative callers.

Acceptance requires all frozen cases and selected real interfaces/callers to
pass their specified outcomes, all existing ordinary/frozen checked
evaluations to remain green, Debug and ASan/UBSan CTest, formatting and
relevant strict clang-tidy. Exercise joins, alias invalidation, conditional
outputs, malformed transport, missing globals, limits and deterministic reports.

Preserve the RFC 0018 final Release binary and collect three sequential
baseline observations using the unchanged five-project manifest and
600-second timeout. These also supply the missing RFC 0018 final-binary
observations when uncontended. Repeat with one final implementation binary
after builds/tests finish. Ordinary median total runtime and peak RSS may
grow at most 10%; diagnose and fix excess before completion. Publish exact
diagnostic changes and every observation. Record checked function coverage,
time, memory and report size separately on all five projects. The selected
real interfaces must settle without limits/timeouts; whole-project limitations
remain in the denominator and must not be represented as successful proofs.

## Annotation surface

None. Existing selection, ownership, size and unsafe annotations retain their
meaning. Inferred contracts appear in reports and pass through sidecars.

## Diagnostics

Retain `checking-incomplete` and `checking-failed`, including their severity
independence. Add specific reasons for unresolved nested memory, conditional
outputs, string termination and copy overlap under those IDs, with unit and
exact-message lit coverage. Ordinary concrete diagnostics keep their witness
policy; absence of sufficient checked evidence is not a new concrete bug ID.

## Drawbacks

Conditional must-facts and output identities complicate joins and invalidation.
More precise contracts may expose existing ordinary false positives. Byte
ranges and nested interface paths add analysis and serialization cost. Real
modules may need sufficient entry contracts stronger than their particular
callers use. The frozen positive/negative evaluation and cost gates constrain
these risks without equating rejection with proof.

## Alternatives

Optimize the engine first; package archive metadata first; infer recursive
container invariants; or add runtime enforcement. Each is useful but solves
a different immediate problem. This milestone extends existing identities and
contracts to the common buffer operations already motivating the checker.
Making all library calls trusted, suppressing unresolved operations, or
initializing entire objects after arbitrary writes would violate RFC 0018.

## Prior art

RFC 0013 supplies incoming versus final heap identities; RFC 0015 supplies
selected cells and simultaneous range copies; RFC 0017 supplies target
integers and stable numeric snapshots; RFC 0018 supplies sufficient contracts
and must-initialization. This proposal composes those designs and uses the
frame principle: evidence survives only for memory a complete effect model
establishes unaffected. No additional solver or external runtime is introduced.

## Unresolved questions

No user-facing design decision is deferred. Internal data layout and helper
file organization may change while preserving the contract. Validation may
require conservative extensions of the implementation, but frozen cases,
selected scopes and performance criteria cannot be silently removed.

## Future work

General recursive heap and collector invariants, archive distribution,
persistent incremental analysis, broader formatting/I/O contracts, concurrency,
runtime discharge and independent proof certification remain separate work.
