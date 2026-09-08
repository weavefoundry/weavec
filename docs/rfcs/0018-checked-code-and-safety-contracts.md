# RFC 0018: Compositional safety contracts and checked code

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-07
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFCs 0003 and 0005's summaries and
  compilation boundaries, RFC 0004's unsafe reporting, and RFCs 0011–0017's
  coverage accounting. The ordinary diagnostic policies remain available.

## Summary

Add opt-in checking of functions and translation units against explicit,
compositional safety contracts. Account for each memory operation, the facts
it requires, unsupported semantics, and trusted assumptions. A selected
function succeeds only when its operations are proved under its exported
entry contract or covered by a recorded trust boundary. Callers must discharge
the contracts of their callees. Unresolved coverage fails selected checking
independently of diagnostic severity controls. Produce a deterministic JSON
report and transport contracts through summaries and compiler sidecars.

This is a bounded, conditional static guarantee. It is not a claim that the
entire executable is verified, that annotations are true, that a solver has
proved arbitrary C, or that generated code has runtime protection.

The owner requested the RFC first and then its implementation end to end.
Acceptance records that authorization after drafting; it does
not claim an independent review or a merge.

## Motivation

At `fb90a22`, all 900 CTest entries pass and the original fixed evaluation
detects all 44 bugs. Nevertheless, each of the following is accepted by
`--strict-externs -Werror=weavec`:

```c
int main(int argc, char **argv) {
  char b[4] = {0};
  return b[argc];
}
```

```c
static void fill(char *p, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    if (p[i] == 42) break;
    p[i] = 1;
  }
}
/* A caller allocates four zeroed bytes and calls fill(p, 8). */
```

```c
union pointer { int *a; int *b; };
/* Store malloc's result in a, check it, free a, then dereference b. */
```

AddressSanitizer confirms a stack overflow, heap overflow and use-after-free,
respectively. These are documented gaps rather than failures of the existing
regression suite. Some missing requirements are only strings in a summary;
they do not become diagnostics. An unknown index can appear as `unresolved`
in a dump while the command succeeds. Unknown input alias relationships can
retain generic checking. Warning promotion therefore cannot establish safety.

Ordinary archives also lose per-member object sidecars. Missing, stale or
incompatible metadata may remain boundaries during migration, but cannot
substantiate a checked result for dependent source.

## Soundness

### Guarantee and assumptions

For a selected function, a successful checked result means that every
reachable operation in the supported model has its safety obligations
discharged, conditional on its recorded entry requirements and trust ledger.
The report distinguishes unconditional proof, proof under entry requirements,
and proof depending on trusted code or assertions. A caller does not receive
permission to assume an entry requirement merely because it was inferred.

The execution model remains single-threaded C with Clang's target types,
layout and integer conversions. The source, headers, command, target and
linked definition used for analysis must match those that execute. Existing
allocator, release, annotation and unsafe contracts are trusted only where
explicitly recorded. Memory safety of the implementation, Clang and external
libraries is not proved by this analysis. A report describes selected source
functions, not unrelated code or all entry points of an executable.

### Rejections added

- Unproved bounds, including unknown indices and a dropped callee access
  requirement. A guard that proves the index safe permits the operation.
- Pointer use without established live provenance, non-nullness, initialized
  pointer storage, or a suitable entry requirement.
- Release without established allocation-base identity and release family.
- Reads of uninitialized local scalar storage or represented memory ranges.
  Such values cannot establish a condition used to prove an access safe.
- Calls whose sufficient contracts, necessary alias relationships, effects,
  or required initialized ranges cannot be established.
- Unsupported operations and exhausted bounds affecting selected checking,
  including union storage, unmodeled byte representations, indirect effects,
  recursive proofs without an established contract, and failed CFG analysis.
- Selected code that already has a memory-safety violation, even when its
  ordinary diagnostic has been lowered to a warning.

An unresolved requirement is not reported as a demonstrated memory bug.
The early-exit example requires enough memory for its possible accesses;
failure to establish that precondition alone does not prove which iteration
executes. Ordinary bug diagnostics retain their existing witness policy.

### Scope and accepted conservative rejections

The first supported proof model covers ordinary scalar C expressions,
non-union records and arrays whose storage can be represented, allocation
and release through supported contracts, pointer copies and derived pointers,
conditional control flow, supported counted loops, and direct helpers.
Pointer-containing heap paths, callback contexts, references, dynamic bounds
and postconditions are usable only when their existing models provide the
needed complete facts. Missing facts always remain unresolved.

Sufficient loop contracts initially support zero-based increasing counted
loops with stable scalar bounds, including conditional accesses, `break`,
`continue`, and early returns. Reassigning the index or bound, changing pointer
identity, calls with unknown writes, nonlocal jumps and unsupported induction
prevent that projection. They do not cause the access to disappear.

General recursive heap invariants, tracing collectors, concurrency, signals,
nonlocal jumps, arbitrary assembly, effective-type reasoning, arbitrary union
reinterpretation, and partially encoded pointers are outside this proof model.
If relevant to selected code they require a recorded unsafe boundary or cause
rejection. Unsupported memory initialization and alias relationships likewise
cause rejection, even when the source is valid. This design does not promise
that all existing clean regression cases are provable in checked mode.

## Detailed design

### 1. Selection and activation

- `--checked` selects every reported definition in the input translation
  units; `--analyze-headers` extends that selection to header definitions.
- Repeated `--checked-function=<name>` selects named definitions. A requested
  name that is not found is an error, not an empty successful scope.
- `WEAVEC_CHECKED` on a function selects its definition without a global flag.
  It is a checking request, not a trusted external ownership annotation.
  Explicitly named and annotated header definitions emit their checked
  diagnostics independently of the ordinary header-reporting filter.
- The compiler equivalents are `-fweavec-checked` and
  `-fweavec-checked-function=<name>`. Selection survives recorded cc1 commands
  and sidecars. A checked annotation also activates link-time checking of its
  dependencies without repeating a command-line flag.
- `--checked-report=<path>` and `-fweavec-checked-report=<path>` write a JSON
  report. Requesting a report activates contract computation but does not
  implicitly select unrequested functions. Reports include selected scope.

Contract computation covers the dependencies necessary for selected functions,
including unannotated helper definitions. Ordinary checking remains the default
when no selection or report requests the additional work. A compilation unit
containing checked declarations enables contract computation for its helpers.

### 2. Core contract and obligation model

Add frontend-neutral types for:

- An obligation property: bounds, validity, initialization, release, aliasing,
  call, supported semantics, arithmetic, or resource lifecycle.
- An outcome: proved, required on entry, trusted, unresolved, or violation.
- A stable origin containing source location, function and operation/property
  identity; a reason and bounded dependency/call provenance accompany it.
- Sufficient entry requirements on parameter-rooted paths: valid non-null
  live memory, accessible, writable and initialized byte intervals, permitted release
  family and base, and separation where interacting effects require it.
- Established initialization postconditions, when true on every returning
  path, separately from entry requirements.
- A contract carrying those requirements, obligations, trust dependencies,
  completion status and a flag identifying selected definitions.

The existing `requiresExtent` remains the bug checker's requirement domain.
Sufficient checked requirements are separate: their joins retain every needed
precondition, never erase an unsupported alternative, and never convert an
overapproximation into a reachable violation witness. Postconditions join by
intersection. Incomplete contracts are never treated as empty pure contracts.

The contract representation is bounded: 256 entry requirements, 256
postconditions, 2,048 obligation origins and 16 dependency steps per chain.
Exhaustion records an unresolved limit obligation. Every limit has an explicit
marker which survives joins and serialization; truncation cannot imply success.
Propagated call origins use a stable terminal source location and reason,
combined with the immediate callee and current call site. They must not recursively
embed escaped incoming identities. Multiple paths to the same source obligation
retain the weakest outcome and a bounded call chain; merging provenance cannot
discharge a requirement or turn unresolved coverage into proof.
Recursive joins are monotone. A recursive dependency whose proof cannot be
closed within existing analysis limits stays unresolved.

### 3. Operation inventory and positive evidence

Use the existing CFG transfer and reporting pass. Enumerate relevant operations
before attempting their proof, including paths on which the existing checker
returns early. Each supported memory access records distinct property outcomes;
proving its bounds does not establish its lifetime or initialization.

Maintain must-initialized local storage and bounded initialized memory ranges
in the dataflow state. Joins retain only facts true on every incoming path.
Assignments, initialization, complete copies, zeroing and supported call
postconditions establish facts; unknown writes and replacement invalidate
affected facts. Pointer copies preserve established object identity. Writing
one selected cell does not initialize a different cell or an entire allocation.
Possible aliases do not justify a strong initialization update.

Pointers require a represented origin (caller contract, local/static storage,
tracked allocation or an established returned/stored value). Unknown, raw,
escaped or unmodeled values provide no positive evidence. Existing may-move,
loan, lifetime, null, ownership and release checks continue to apply. A value
cannot be proved live merely because no error was emitted. Unsupported state
transfers are explicit obligations rather than silent forgetting followed by
success. Existing memory violations also enter the obligation ledger before
diagnostic filtering or unsafe suppression.

Writes additionally require positive evidence of writable storage. A string
literal or a declared const object is not writable merely because a cast removes
a pointer qualifier. Incoming writes export a distinct writable-interval
precondition, discharged at callers and by memory primitives alongside extent
and initialization obligations. Unknown mutability remains unresolved; a zero
byte interval has no write-permission obligation. Allocation contracts and
represented mutable local storage provide positive evidence. Borrowing a
const-qualified view alone does not prove the underlying object is mutable.

Numeric operations affecting the proof use RFC 0017 target semantics. Possible
invalid arithmetic and uninitialized conditions cannot justify path pruning.
Unsupported evaluated AST/CFG constructs create semantic coverage obligations.
Unevaluated expressions do not create fictitious accesses; evaluated VLA bounds
do. Unsupported constructs in unreachable CFG blocks do not fabricate executed
memory bugs. Structural restrictions may conservatively reject a function.

### 4. Sufficient preconditions and calls

For an incoming pointer access, export the sufficient byte interval and
validity/initialization/release requirements instead of treating unknown entry
memory as proved. Local obligations whose quantities cannot be projected stay
unresolved. Function parameters have the language's initialized argument values;
the memory they point to has only the recorded requirements.

For a supported counted loop `i = 0; i < n; ++i`, an access to `p[i]` may
require the whole interval `[0, n * sizeof(*p))`. A conditional access or early
exit does not require that every byte actually be accessed. Bounds and loop
increment validity must still follow the target's arithmetic. Multiple stable
upper bounds may use the supported minimum expression. An unsupported loop
leaves explicit unresolved obligations at its accesses.

At a call, instantiate the callee's sufficient contract against the caller's
entry snapshots and current facts. Prove it, propagate a representable caller
entry requirement, or record unresolved coverage. Include the originating
operation and an immediate call note. Checked requirements propagate through
unannotated helpers and across units. Missing or incomplete contracts do not
turn into pure calls. Generic summaries and contextual reanalysis still drive
effects; their required identities must be established for checked acceptance.

When multiple pointer inputs can interact through a write, release or escape,
derive a conservative separation precondition unless an applicable complete
context proves the call under their relationship. Unequal addresses alone
cannot discharge allocation separation. Read-only aliasing does not require
invented exclusivity. If a necessary relationship cannot be represented, the
call remains unresolved. Conservative separation is an inferred requirement,
not a language assumption that C parameters are `restrict`.

Support the modeled allocation/release and complete memory primitives through
explicit builtin trust entries and checked argument obligations. Merely being
one of the shipped library names is not evidence of complete memory semantics.
An unmodeled library operation remains an external contract/unsafe boundary or
unresolved. Declared external ownership contracts may be trusted, but their
missing bounds, effects or initialization guarantees may still block a caller.

### 5. Unsafe and trust

Preserve RFC 0004's transfer through unsafe code. Obligations at unsafe source
points are recorded as trusted rather than proved. Their effects continue to
invalidate facts used by later safe operations. A surrounding checked function
does not become unchecked because it contains one unsafe block.

Record `WEAVEC_ASSUME`, assumed annotations, builtin contracts and external
contracts in a bounded trust ledger. A declaration carrying only
`WEAVEC_CHECKED` cannot make unknown external behavior trusted. A function
annotated both checked and unsafe is permitted and is reported as trusted;
it is not reported as a proved body. Trust is transitive through calls.

### 6. Reporting and enforcement

Add the stable error `checking-incomplete` for selected code whose obligations
cannot be discharged, and `checking-failed` when an existing violation prevents
checked acceptance. The primary text is respectively:

```
cannot establish checked safety: <reason>
checked safety failed: <reason>
```

The source operation is primary; a note identifies the origin in a callee or
the relevant entry requirement. Duplicate propagated failures retain their
root identity and call provenance. Checked status is computed before `-W`
controls: lowering errors or suppressing ordinary coverage warnings cannot
make a rejected checked result successful. Compiler/tool exit status follows
checked status as well as ordinary diagnostics.

JSON schema version 1 includes tool/model version, target, source scope,
selected function names, each function's conditional status, requirements,
obligations, trust dependencies and totals. It contains no whole-executable
certificate. Order is deterministic. Strings use correct JSON escaping;
source file names and diagnostic text are data. Report writing is atomic;
unwritable output fails the command. A failing analysis still produces its
report when enough information is available, and failure is recorded explicitly.

Report collection is separate from diagnostic emission so silent inference
rounds and warning filtering cannot erase evidence or publish provisional
success. Only settled summaries enter final reports. Failed parsing and
nonconvergence prevent an overall successful result.

### 7. Summaries, sidecars and artifact identity

Summary and sidecar formats become 14. Serialize contracts, requirements,
postconditions, origins and trust dependencies with strict bounds and input
validation. All parameter/global paths follow the existing remapping rules;
an unrepresentable remap invalidates checked completeness. Generic and
specialized summaries participate in equality and invalidation.

Compiler sidecars record whether checked selection was requested and bind
checked results to the object contents, source/header inputs and recorded
command/target. Replaying changed source against an older object cannot
establish that object's safety. Missing input files, changed dependencies,
malformed or stale checked metadata, and incompatible model versions block
dependent checked acceptance. Ordinary boundary behavior remains available
when no selected result depends on that metadata.

Compilation can produce a provisional contract with external dependencies
deferred until the normal link analysis. A local unresolved operation cannot
be deferred as an external dependency. Link analysis checks requested
definitions and contracts together before code generation's link proceeds.
Unknown object/archive members cannot substantiate a checked dependency;
archive packaging/caching itself remains a separate milestone. Checked-mode
requests combined with disabling analysis or link verification are rejected
when they would otherwise claim completed checked compilation.

### 8. Implementation organization and validation

Put contract algebra, initialized-range operations and portable formatting in
Core, operation accounting and proof projection in focused Analysis files,
and JSON/report publication and artifact validation in Frontend. Core has no
Clang/LLVM dependency. Avoid adding another independent C interpreter or
growing the main dataflow implementation with an unrelated monolith.

Freeze an evaluation manifest before implementation. Include unchecked/guarded
indices, early-exit helper intervals, union identities, unknown input aliases,
local scalar and memory initialization, release families, caller requirements,
transitive helpers, unsafe effects, trusted assumptions, resource limits,
unavailable metadata, and diagnostic demotion. Bugs, conservative rejections,
proved programs and conditionally checked functions are distinct categories.
Do not count an incomplete rejection as a newly diagnosed concrete bug.

Acceptance requires:

- All existing tests and the original 44-bug/32-clean evaluation remain green
  in ordinary checking; selected unsupported cases need not become provable.
- Fixed checked positive and negative cases pass in tooling, whole-program
  and compiler object modes where applicable; relevant forms include inline,
  helper and cross-unit variants.
- All three motivating misses fail selected checking; useful guarded buffer
  and allocation/helper examples succeed with explicit contracts.
- Core algebra, initialization joins, limits, malformed input, serialization,
  remapping and deterministic reports have tests; diagnostic messages have lit
  coverage. Warning demotion cannot change checked acceptance.
- Debug and ASan/UBSan suites, formatting and relevant clang-tidy checks pass.
- Compare the same pinned five-project Release corpus before/after with three
  sequential observations per binary and unchanged timeouts. Ordinary-mode
  median runtime and peak RSS may grow at most 15%. Publish every observation
  and diagnostic change. Investigate and fix excess before declaring completion.
  Checked-mode cost and accepted/unresolved/trusted functions are measured
  separately; rejection is not a performance shortcut counted as a proof.

## Annotation surface

`WEAVEC_CHECKED` expands to `WEAVEC_ANNOTATE_("weavec.checked")`. It attaches
to function declarations/definitions and requests checking of the body. It
has no ownership, bounds or trust meaning for an unavailable definition.
Other placements are invalid annotations. Mirror the spelling in
`Analysis/Annotations.h`; increment the public header's minor version to 8.
Macros retain ordinary C toolchain compatibility through the existing
`WEAVEC_ANNOTATE_` fallback. Existing annotations keep their spelling.

## Diagnostics

`checking-incomplete` and `checking-failed` are stable error IDs with the
messages specified above, unit and exact-message lit tests, and documentation
in `docs/annotations.md`. Unknown selected function names, report I/O failures
and unusable checked sidecars are frontend failures with descriptive messages.
All checked failures remain failures independently of `-W` severity changes.

## Drawbacks

The supported proof subset will reject valid C that the ordinary checker
accepts. Contracts may demand more than a particular execution needs. Positive
initialization/provenance evidence and per-operation accounting add state and
summary cost. Artifact binding requires reading build inputs and objects.
Reports must explain conditional results well enough that a trusted body is
not mistaken for an independently proved one.

## Alternatives

- Promote `analysis-incomplete` warnings: misses silent paths and does not
  distinguish sufficient conditions from reachable bug witnesses.
- Add only more inference patterns: useful, but leaves success undefined for
  everything those patterns do not represent.
- Require source-wide annotations: defeats incremental inference and hides
  assumptions in annotations instead of accounting for them.
- Runtime enforcement: changes code generation and ABI/runtime deployment;
  a separate milestone can use these explicit obligations later.
- Prove all C immediately: unbounded alias, heap and arithmetic reasoning is
  outside this bounded design.

## Prior art

RFCs 0013–0017 explicitly reserve verification and coverage enforcement for
later work. This proposal builds on their identity, numeric and contextual
models while retaining the distinction between may-effects and must-facts.
Clang's [bounds-safety design](https://clang.llvm.org/docs/BoundsSafety.html)
requires enough bounds information for enforcement and makes ABI/unsafe
boundaries explicit; this proposal takes that accounting discipline without
introducing its dynamic checks or wide-pointer representation. Modular
precondition/postcondition reasoning supplies the distinction between a
function valid under a contract and a caller that establishes that contract.

## Unresolved questions

No user-facing design choice is left to implementation. Internal indexing,
serialization details and allocation of helper files may change while
preserving this contract. Validation may expose unsupported forms; they must
remain explicit unresolved obligations rather than silently narrowing the
guarantee or removing fixed evaluation cases.

## Future work

Archive packaging and incremental caches; broader recursive/collector
invariants; additional initialized byte and pointer representation models;
more general loop inference; runtime discharge of residual obligations;
concurrency; and independently validated proof certificates.
