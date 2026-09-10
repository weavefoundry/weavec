# Architecture

WeaveC is organised as three C++ libraries and two thin command-line tools.
The libraries form a strict dependency chain; the arrows below point from a
layer to what it may depend on.

```
        ┌──────────────────────┐  ┌──────────────────────────┐
        │  tools/weavec        │  │  tools/weavec-cc         │
        │  (libTooling)        │  │  (drop-in C compiler)    │
        └──────────┬───────────┘  └────────────┬─────────────┘
                   └───────────┬───────────────┘
                               ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  Clang FrontendAction, libTooling,
                 │  lib/Frontend            │  whole-program orchestration,
                 │                          │  sidecars, driver, diagnostics
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Analysis        │  Clang AST → core facts,
                 │  lib/Analysis            │  inference, checkers
                 └──────┬───────────┬───────┘
                        ▼           ▼
        ┌────────────────────┐   ┌────────────────────┐
        │  weavec::Core      │   │  Clang / LLVM      │
        │  lib/Core          │   │  (external)        │
        │  no Clang/LLVM     │   └────────────────────┘
        └────────────────────┘
```

## `weavec::Core` — the model

`lib/Core` contains the ownership, integer and spatial domains without AST or
Clang dependencies. It depends only on the C++ standard library. This is the
piece the README asks to keep "as modular as possible": it can be unit-tested
without parsing any code, reused by a different frontend, or embedded in other
tools.

| Header             | Purpose                                                                                        |
| ------------------ | ---------------------------------------------------------------------------------------------- |
| `Ownership.h`      | `OwnershipKind` lattice (`Unknown ⊑ {Owned, Shared, Mutable} ⊑ Raw`) and `join`. `Raw` is "no guarantee": tracked, but usable only inside an unsafe region. |
| `Place.h`          | `PlaceId` and `PlaceTable`: structured places (`p`, `s.f`, `*p`, `p->f`, `a[*]`, `a[0]`) with parent/descendant/translate queries. |
| `Array.h`          | Bounded constant/symbolic selectors, half-open intervals, sparse spans and evidence-based membership/disjointness queries (RFC 0015). |
| `AliasRelation.h`  | Symmetric may-alias graph over places; closed under copies, plain union at joins (deliberately not transitive). Each edge records the `PointerOffset` between the two places — the same value (`Zero`), a constant number of elements, a field, or `Unknown` — so a pointer derived from another is a name for the same object at a known distance (RFC 0011), and which element of the other end is meant (RFC 0006); `separateExact` refutes a zero-offset edge on a `!=` edge. |
| `Lifetime.h`       | `LifetimeId` and `LifetimeConstraints` (transitive `outlives` queries; `'static` is id 0).      |
| `Borrow.h`         | `Loan` (place, kind, lifetime, holder) and `BorrowState`: may this borrow be created; may this place be moved or mutated; `expireHolders` drops the loans of holders a predicate declares dead (RFC 0006 liveness). |
| `Integer.h`        | Target integer types, masked bit-pattern values, bounded modular ranges, concrete/abstract arithmetic, conversion and checked-overflow results (RFC 0017). Implementations are `Integer.cpp` and `CheckedInteger.cpp`; neither uses host signed overflow to model target arithmetic. |
| `IntegerExpression.h` | Bounded typed expressions and predicates over local places or stable summary paths: constants, inputs, casts, arithmetic, min/max and overflow tests, with validation, substitution and canonical serialization (RFC 0017). |
| `Scalar.h`         | `ValueFact` (a set of RFC 0006 outcome classes — `zero`/`positive`/`negative` or `null`/`nonnull` — plus an optional exact constant; `join`, `narrow`, `disjointFrom`, `implies`), `GuardOn<Key>` (a conjunction of facts about places — `PlaceGuard` — or summary paths — `PathGuard` — under which alone a record or effect holds; `require`, `learn`, `refine`, `join`, `drop`, bounded by `MaxGuardConjuncts`) and `ScalarTracker` (per integer place, what is known about its value; RFC 0009). |
| `Offset.h`         | `PointerOffset`: where inside its object a pointer points — `Zero`, `Elements(k)`, `Field(key)`, `Unknown` — with `plus`, `negated`, `toString`/`parse` (RFC 0011). |
| `Spatial.h`        | `Affine` (an extent: a constant, or `scale * place + constant`), `SpatialRecord` (a place's extent and offset, and its `StringFact` — the length of the string the object holds, or that it has no terminator — RFC 0012), `SpatialTracker` (per place, joined by agreement) and `boundsVerdict`, the pure decision of RFC 0011's bounds rules (`OutOfBounds`, `MayBeOutOfBounds`, `MayReachPastEnd`, `BeforeStart`, and RFC 0012's `AtLeastPastEnd` from a lower bound) over `KnownBounds` (constant upper and lower bounds on either side). |
| `Relation.h`       | `RelationTracker`: what the path knows of one integer place against another (`Less`, `LessEqual`, `Equal`, `GreaterEqual`, `Greater`, learnt from condition edges, through one equality hop, each with an *offset*: `i < n + k`, RFC 0012) and against a constant (`learnAtMost`/`atMost`, `learnAtLeast`/`atLeast`); `forget` on a write, `join` by agreement (RFC 0011). |
| `Moves.h`          | `MoveTracker`: which places are currently moved-out/freed (and through which alias), each with an `ElementWitness` (whole / constant / variable / unknown) saying which element of an `a[*]` place was named (RFC 0006); a use is only reported when the witnesses match; conservative `join`. `MoveReason::Uninitialized` marks a local pointer place that has never been assigned (RFC 0008). Each record carries a `PlaceGuard` (RFC 0009): `learn` refutes and erases the records a condition edge contradicts, `dropGuardsOn` weakens the guards that name a written place. |
| `Raw.h`            | `RawTracker`: which places currently hold a raw pointer, why (`RawReason`: integer cast, `WEAVEC_RAW` declaration, loaded through a raw pointer, callee result, unchecked callee) and through which alias; union at joins. |
| `Resource.h`       | `ResourceTracker`: which places hold an owned resource this function is responsible for (`ResourceRecord`: origin — allocated or declared `WEAVEC_OWNED` —, location, release family, escaped, and the number of shares — RFC 0010; the offset a place points at is on its alias edges and in the `SpatialTracker`, RFC 0011), plus the places known to hold null; records join by union, null facts by intersection (RFC 0007). Each record carries a `PlaceGuard` (RFC 0009): a resource held only under a fact is cleared, not leaked, on the edge that refutes it. |
| `Nullness.h`       | `NullTracker`: per place, whether the pointer it holds is `Null`, `MaybeNull` or `NonNull` (`NullRecord`: state, where the fact comes from and why — assigned null, a callee's result or store, a merged null test, a `WEAVEC_NULLABLE` declaration); no record is *unknown* and trusted. Joins by the RFC 0008 table (`MaybeNull` absorbs, `Null` with anything else is `MaybeNull`, `NonNull` with no fact is no fact). A `Null`/`MaybeNull` record carries the `PlaceGuard` of the paths that made it null and, when every other path was non-null, `otherwiseNonNull`, so refuting the guard makes the record `NonNull` (RFC 0009). |
| `AnalysisState.h`  | The dataflow state: moves, loans, aliases, raw pointers, resources, nullness, scalar facts (RFC 0009), extents and offsets (`spatial`) and integer relations (`relations`, RFC 0011), inferred kinds, the `PendingOutcome`s of calls whose consumption (and whose null and non-null stores, RFC 0007/0008) depend on a not-yet-tested result, the flow-sensitive `consumed` record that feeds outcome classes at `return` (RFC 0006), and the `overwritten` caller-visible paths whose entry value has been replaced on every path (RFC 0008), with component-wise `join`. `pathGuard()` is the facts of the current path as the guard of a record created here; `learn` propagates a condition edge's fact to every guarded record; `factOf` reads scalar and definite nullness facts through one interface. |
| `Summary.h`        | `FunctionSummary`: what a function does to its interface. `SummaryPath` (`param(i)`/`global(g)`/`result` plus deref/field/index steps), `PlaceEffect` (read/written/freed/moved, with the release family of a consumption, `replaced` when every consuming path reinitialised the place, `element` when every consume went through an element access — RFC 0008 — and `when`, the `PathGuard` under which alone the consume happens — RFC 0009), `Store` (value written into caller-visible memory), `ValueSource` (fresh — with its release family and, when known, its extent —/copy/borrow/null/raw/unknown, each at a `PointerOffset` from its root and with a `when` guard — RFC 0011; alternatives that differ only in their guard are merged by `addReturn`/`addStore`), `neverReturns` (the exit is unreachable; joins by conjunction, RFC 0009), `outcomes` (per `Outcome` class — `Null`, `NonNull`, `Zero`, `Positive`, `Negative` — the consumption that holds on the paths returning it; RFC 0006), `nullOn` (per class, the caller places that are null; RFC 0007), `nonNullOn` and `requiresNonNull` (per class, the caller places that are non-null; the parameters the function dereferences untested; RFC 0008), `requiresExtent` (per parameter, the `PathAffine` extent the body needs behind it, RFC 0011), with `join`, `remapGlobals` and the derived `consumes`/`consumesUnconditionally`/`borrowKind`/`inferredKind`/`freshReturnFamily` queries. |
| `SummaryIO.h`      | The stable text form of a `FunctionSummary` (`summary` ... `end` records; RFC 0005): `printSummary`/`parseSummary` with callbacks that name and resolve globals, so the format is Clang-free and the on-disk sidecar format is defined here. |
| `Scc.h`            | Tarjan's strongly connected components over an adjacency list, in reverse topological order; used for the call graph inside a unit and for the unit graph of a program. |
| `Diagnostic.h`     | `Diagnostic`, stable ids in `diag::` (with `All`, `isKnown`, `isWarningByDefault`), `FixItHint`, `DiagnosticSink`, and an in-memory `DiagnosticCollector`. |
| `SourceLocation.h` | Frontend-neutral positions with an `opaque` slot for the frontend's native encoding.           |

The core never sees a `clang::VarDecl`; it sees a `PlaceId`. It never sees a
`clang::SourceLocation`; it sees a `core::SourceLocation` whose `opaque` field
the frontend fills in so it can report at the exact original position.
RFC 0017 extends `ValueFact` with typed integer ranges and `GuardOn` with
integer predicates. `AnalysisState` retains numeric expressions, conditions
and writes; `FunctionSummary::numericOutputs` and expression-bearing
`PathAffine` values extend the earlier scalar and affine interfaces. Spatial
requirements also retain the first accessed byte, so a negative start cannot
be mistaken for an empty access.

## `weavec::Analysis` — the bridge

`lib/Analysis` is the only library allowed to include both `weavec/Core/*` and
`clang/*`. It:

- recognises WeaveC annotations on declarations, statements and
  function-pointer types (`Annotations.h`; `collectFunctionTypeAnnotations`
  walks through typedefs, fields and parameters to the prototype);
- converts source locations in both directions (`ClangLocation.h`);
- resolves the summary of any callee (`Summaries.h`, `SummaryStore`), in
  order: the callee's own annotations, the summary inferred from its body in
  this TU, the program database (a definition in another unit of the
  program), the shipped libc/POSIX table (`Builtins.cpp`), and finally a
  documented default that also records the callee as an unknown boundary.
  For a call through a function pointer (`lookupCall`), annotations on the
  pointer's type remain authoritative; otherwise the current function-value
  targets select the summaries. Unknown alternatives retain the boundary
  behavior (RFC 0014). `lookupIndirect` supplies only an explicit type contract;
- holds what other units export (`ProgramDatabase.h`): `UnitExports` (the
  functions a unit defines with their summaries, linkage, canonical type key
  and address-taken flag; the names it imports; the indirect-call type keys
  it has no signature for; the boundaries it deferred) and
  `ProgramDatabase`, which joins exports by name and by type key and remaps
  summaries that mention globals into the importing unit's `GlobalTable`;
- classifies calls by their ownership effect on top of that
  (`Allocators.h`, `classifyCall` → `CallEffects`);
- maps expressions onto structured places, classifies pointer-typed values
  as allocation, copy, borrow, null, raw or opaque, and translates summary
  paths into the caller's places and back (`lib/Analysis/PlaceBuilder.h`).
  Pointer arithmetic and pointer-to-pointer casts preserve identity; only
  integer-to-pointer casts produce raw values;
- runs a forward dataflow over `clang::CFG` for each function body
  (`lib/Analysis/Dataflow.h`, `FunctionDataflow`): a worklist to a fixpoint
  with `core::AnalysisState` as the lattice, then one reporting pass that
  emits each diagnostic once. A backward liveness pass over the same CFG
  runs first; before each element the loans held by dead locals expire
  (RFC 0006). On each edge out of a conditional, `applyEdge` refines the
  state with what the condition says: pointer equality unites or separates
  aliases, and a test on a call result selects outcome classes and
  reinstates what the callee consumed only in the other classes. While
  running it applies callee summaries at every call (element-aware, deepest
  consumed path first, with `written` effects forgetting the facts below
  the written place), records its own effects, stores, returns and
  per-class consumption, checks them against the function's annotations,
  tracks raw pointers and reports raw operations, keeps the books of owned
  resources (acquired at allocations and `WEAVEC_OWNED` declarations;
  released, moved, escaped or lost — the leak and release-family checks of
  RFC 0007 run where a holder dies, on each CFG edge, at overwrites and at
  container frees), tracks what is known about each pointer's nullness and
  reports dereferences and calls that need more (RFC 0008; the null test
  idioms are the RFC 0006 condition facts), marks uninitialised locals and
  checks what a releaser is handed, tracks what is known about each
  integer's value (constants assigned, `==`/`!=`/`<`/... against a
  constant, truthiness, `switch` cases) and attaches the facts of the
  current path as a *guard* to every move, held resource and null record
  it creates so that a later test can refute them, translates a callee's
  `when` guards to the arguments and prunes them against its own facts,
  ends the block at a call to a callee inferred `never-returns` (RFC
  0009), keeps what is known of the string each object holds and checks
  the string copies and terminator-seeking reads of the shipped table
  against it (`DataflowStrings.cpp`, RFC 0012), gives a `WEAVEC_SIZED_BY`
  or inferred sized field the extent its count says and records what
  every store into a field says about the pair (`DataflowSizedFields.cpp`,
  RFC 0012), applies `WEAVEC_ASSUME` as a condition edge, and produces the
  function's `FunctionSummary` at exit (with `neverReturns` when the exit
  was never reached).
  `WEAVEC_UNSAFE` regions are analysed like any other code; the pass only
  suppresses what it would report inside them. `FunctionAnalysis.h` is the
  per-function entry point; `AnalysisOptions::exclusiveBorrows` switches
  RFC 0001's exclusivity rules back on;
- drives a whole translation unit (`TranslationUnitAnalysis.h`,
  `TranslationUnitAnalyzer`): collects definitions and address-taken
  functions, builds the call graph (with an edge from every indirect call to
  each candidate of its type), and analyses strongly connected components in
  reverse topological order (callees first), iterating recursive components
  to a fixpoint on their summaries before the final reporting pass. When the
  unit's own stores confirm a sized-field pair (RFC 0012) the functions that
  read the field are analysed once more with it in force and only their new
  reports are shown. With a `ProgramDatabase` attached, callers see callees
  from other units; `discover()` returns the unit's exports without
  analysing it (what it defines and imports, for ordering units) and
  `exports()` returns them with summaries, sized-field witnesses and
  refutations, and the fields it looked up, after `run()`.

The model is specified by [RFC 0001](rfcs/0001-ownership-model.md), the
dataflow by [RFC 0002](rfcs/0002-intraprocedural-checking.md), summaries by
[RFC 0003](rfcs/0003-signature-inference.md), raw pointers, unsafe
regions and indirect calls by
[RFC 0004](rfcs/0004-unsafe-boundaries.md), cross-unit analysis by
[RFC 0005](rfcs/0005-whole-program-analysis.md), non-lexical loans,
condition facts, element witnesses and outcome-conditional summaries by
[RFC 0006](rfcs/0006-precision.md), leaks, release families and owned
fields by [RFC 0007](rfcs/0007-resource-lifecycle.md), nullness,
uninitialised pointers, invalid releases and replaced values by
[RFC 0008](rfcs/0008-pointer-validity.md), and scalar facts, guards,
argument-conditional summaries and inferred `noreturn` by
[RFC 0009](rfcs/0009-value-conditional-behaviour.md); each RFC's
*Implementation notes* record where the code refines the design.

## `weavec::Frontend` — Clang integration

`lib/Frontend` adapts the analysis to Clang's frontend machinery:

- `WeaveCAction` is an `ASTFrontendAction` whose consumer hands the whole
  translation unit to `TranslationUnitAnalyzer`; every definition contributes
  a summary, but by default only those in the main file are reported. The
  same consumer (`createWeaveCConsumer`) is what `weavec-cc` multiplexes
  beside Clang's code generator. `FrontendOptions` carries the analysis
  options, the program database to consult, the diagnostics already
  reported for the unit, and a receiver for the unit's exports.
- `ClangDiagnosticSink` forwards `core::Diagnostic`s (including fix-its) to
  Clang's `DiagnosticsEngine`, so WeaveC's output is rendered exactly like
  Clang's own (carets, colours, `-fdiagnostics-format=`,
  `-fdiagnostics-parseable-fixits`, `-Werror`, ...). `DiagnosticControl`
  applies `-Wno-weavec-<id>`, `-Wno-error=weavec-<id>`, `-Werror=weavec`
  and friends before the sink sees a diagnostic; `FilteringSink` drops
  diagnostics already reported by an earlier step and repeats of a boundary
  warning within a program.
- `ProgramAnalysis` is the whole-program algorithm over an abstract
  `ProgramUnit` (something that can parse a unit and run an action over
  it): discover every unit's exports, build the unit graph (who imports
  whose definitions, who calls through a type someone else has a candidate
  for), analyse acyclic units once and cyclic groups to a fixpoint, each
  against the database of what has been analysed so far, then one more
  reporting pass over the units that looked up a sized field the program
  confirmed after they were analysed (RFC 0012; a fact about a type, which
  the call graph's order does not carry). `CompilationDatabaseUnit` parses
  from a compilation database.
- `Sidecar.h` reads and writes `foo.o.weavec`: the unit's exports, the cc1
  command that produced it and the diagnostics already reported, in a
  line-oriented text format versioned by its `weavec-summaries 16` header.
- `Driver.h` is `weavec-cc`: Clang's `driver::Driver` plans the jobs, each
  `-cc1` job runs in-process with WeaveC's consumer multiplexed beside
  Clang's, the compile step writes the sidecar, and the link step runs
  `ProgramAnalysis` over the sidecars of the objects being linked before
  the linker.
- `ResourceDir.h` locates `weavec.h`, Clang's resource directory and the
  `clang` binary in installed and build-tree layouts.

## `tools/weavec` and `tools/weavec-cc`

`weavec` is a libTooling application: `weavec file.c -- <compiler flags>`
or `weavec -p build/ file.c` with a compilation database. It injects
`-isystem <resource-dir>/include` and `-D__WEAVEC__=1` so user code can
`#include <weavec.h>`. `--whole-program` analyses every file given (or every
file of the compilation database) as one program. `--dump-analysis` prints
the inferred facts and summary per function for debugging (and, in
whole-program mode, the program database); `--report-unannotated` offers
fix-its for exported functions; `--strict-externs` makes every call into
unknown code a raw operation, so it is an error outside a `WEAVEC_UNSAFE`
region. `-Wno-weavec-<id>` and the other `-W` spellings are accepted.

`weavec-cc` is the drop-in compiler: `CC=weavec-cc make`. Compile steps
analyse the unit alone and write `<object>.weavec`; the link step reads the
sidecars, re-analyses the units whose results depend on other units, reports
what only the program could know, and refuses to link on an error. WeaveC's
own flags are `-fweavec`/`-fno-weavec`, `-fweavec-strict`,
`-fweavec-report-unannotated`, `-fweavec-analyze-headers`,
`-fweavec-dump-analysis`, `-fweavec-exclusive-borrows`,
`-fweavec-link`/`-fno-weavec-link` and the `-W` spellings; everything else
is Clang's. The design is
[RFC 0005](rfcs/0005-whole-program-analysis.md).

## Heap postconditions and value snapshots

[RFC 0013](rfcs/0013-interprocedural-heap-state.md) adds
`FunctionSummary::heap`, a map from an output path to a `HeapDescription`.
Each description uses result-relative pointer cells and the existing value
sources. `copy-post` names an already represented output object; ordinary
`copy` names an incoming value. This makes shared children and cycles finite
and keeps final output values separate from historical `stores`. Core owns
bounds, joins, validation and serialization, without Clang dependencies.

`Analysis/DataflowHeap.cpp` captures final reachable facts, resolves incoming
values before a call replaces them, and materializes the graph into the
normal state trackers. A definite alias relation intersects at joins and
supports strong updates through local aliases; the existing may-alias
relation still governs possible consumes. Copies preserve identity when
liveness retires the original local. Failure outcomes restore captured input
facts. Projection is limited to eight path steps, 128 field alternatives per
description and eight alternatives per cell; lost coverage remains visible.
Materialized children remain in their containing graph instead of becoming
additional historical stores on the next summary iteration.

Output writes and pointer returns retain conditions on immutable entry
values. A call snapshots any guard operand or returned input pointer it
can overwrite. This preserves extraction (`p = *slot; *slot = NULL; return p`)
and lazy publication without treating a test of the new cell as a test of
its old value. Definite publication guards apply to initialized children;
an unconditional later write remains unconditional after a join.

`Analysis/DataflowValues.cpp` folds allocation sizes using current scalar
facts. Before overwriting a scalar used by an extent or string length, it
redirects the dependency to an interned allocation-time snapshot. Reusing a
snapshot site invalidates the old generation's dependent facts. The domain
remains bounded; RFC 0017 extends these snapshots to every dependency of a
typed symbolic expression while retaining affine and relation fast paths.

The current summary version 15 and sidecar version 16 retain heap descriptions, post
references and string metadata. `ProgramDatabase` remaps global references
and compares these descriptions as part of normal dependency invalidation.
The compiler and tooling whole-program modes share this implementation.

## Compositional calls (RFC 0016)

`Core/CallContext.h` describes entry relationships independently of a final
summary: parameter/global paths, may or definite aliases, relative offsets,
same-share identity, proven distinct objects, scalar/null facts and callback
bindings. Canonicalization and strict parsing reject contradictory premises.
Global remapping is all-or-nothing: losing an entry fact cannot leave a
specialized result available under a weaker context.

`Analysis/DataflowCallContext.cpp` projects the caller state through the
callee's relevant input footprint and installs a validated context at the
callee's entry. `CallContextSummaries.cpp` reuses `FunctionDataflow` to check
the body in source order and obtain its final summary. This distinguishes
multiple operations from one operation exported under several aliases,
without adding another interpreter for call effects. Entry-relative release
offsets preserve the different starting positions of interior-pointer inputs.

The normal scalar and alias trackers govern writes after entry. Distinct-object
facts intersect at joins and expire with their input values. A per-call cache
is invalidated when its CFG state changes; specialized summaries are invalidated
when generic dependencies change. Pending contexts are separate from completed
results. Contexts bound pointer paths at 32, relationships/facts at 64, distinct
contexts per callable at 32 and nested specialization at eight levels.

`TranslationUnitAnalysis` infers generic effects for every definition and checks
requested memory contexts in its final reporting pass. Definitions without
contexts retain ordinary reporting, and failed checked contexts retain ordinary
body errors. Annotation validation remains independent. Requests carry whether
their originating call permits diagnostics, so unsafe calls still receive
effects without producing delayed errors in another unit. Nested calls retain
source notes, and the final sink deduplicates reports of the same operation.

`ProgramDatabase` maps requests and completed results through global names.
`ProgramAnalysis` adds caller-to-definer and definer-to-caller dependencies and
converges their context information before reporting. The compiler replay
planner includes definitions that can receive requests from another object,
even if their generic summaries were already locally complete. Introduced in
format 12 and retained in format 15, sidecars serialize
`accepts-memory-contexts`, `memory-request` and `memory-specialization`
records alongside existing callback records.

Unsupported projections retain generic call effects and expose missing
coverage. Calls with no established interacting identity remain generic; they
are not treated as proofs of disjoint inputs. `--dump-analysis` shows aliases,
distinct objects, entry facts and final summaries for requested contexts.

## Arrays and containers (RFC 0015)


`Core/Array` represents a selector as a constant or an immutable scalar plus
an offset. Selected `Index` places live below array storage; the empty `Index`
remains an unknown-element summary. Nested selections preserve each dimension.
Moves, aliases, ownership, loans, nullness, callbacks and heap children use
those ordinary places. `AnalysisState` adds sparse range-copy, fill and release
facts, with must-facts weakened at joins. Limits are 32 selected cells and 32
range facts per storage object, independent of a program's array length.

The Analysis implementation is split by operation: `DataflowArrays.cpp`
resolves selectors and initializes cells, `DataflowArrayMemory.cpp` handles
simultaneous copies and reallocations, `DataflowArrayRanges.cpp` retains copy
snapshots, and the cleanup/fill files recognize and apply complete traversals.
Scalar writes freeze index/count dependencies under bounded source-site
identities. Unsupported generations and compositions retain explicit coverage
information. Snapshots are analysis temporaries, not extra resource owners.

Summary paths encode selected constants and entry-parameter selectors. Final
`array-copy` records carry storage paths, offsets, count, element size/view
and guards; ordinary final cell postconditions take precedence. `array-fill`
and `array-release` encode proved zero-based initialization and contiguous
cleanup. Formats are deterministic and validated in Core, and global remapping
visits every path, affine operand and guard. Returned ranges are captured
before call effects and attached when the result obtains its destination.
The normal function/program fixpoints compare these facts with the rest of
the summary; compiler sidecars use the same format and inference.

## Target integers and compositional bounds (RFC 0017)

[RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) specifies
the target-integer and spatial model. The
[validation report](validation-rfc0017.md) records correctness checks,
corpus diagnostics, performance measurements and supported boundaries.

Core represents types from one through 64 bits, signedness and boolean
conversion behavior. `IntegerValue` stores an unsigned bit pattern;
`IntegerRange` keeps at most two intervals in numeric order. Transfers model
unsigned wrap, signed validity, comparisons and conversion before projecting
legacy constants or sign classes. `_Bool` converts any nonzero value to one.
An invalid operation supplies no invented value; possibly invalid operations
cannot establish a branch fact. Changing loop ranges widen to type endpoints.

`IntegerExpression<Key>` holds canonical typed operations over inputs, including
products, min/max and checked-overflow predicates. Expressions are limited to
64 nodes, depth 12 and 32,768 serialized characters. Guards admit at most eight
conjuncts, including numeric predicates; numeric outputs keep at most eight
alternatives. Exceeding representational limits loses precision or records
incomplete coverage. These bounds do not limit source allocation sizes.

The Analysis implementation separates the following responsibilities:

| File in `lib/Analysis` | Responsibility |
| --- | --- |
| `IntegerSupport.h` | Read Clang target widths, signedness, bit-field storage widths and operators; recognize checked builtins and value-preserving conversions. |
| `DataflowIntegers.cpp` | Evaluate typed AST ranges without discarding implicit casts, refine comparisons, translate numeric guards, diagnose definite invalid operations and record spatial outcomes. |
| `DataflowIntegerExpressions.cpp` | Lower and intern bounded expressions, use affine forms only where justified, substitute interface inputs and snapshot expression dependencies. |
| `DataflowIntegerStatements.cpp` | Compute compound assignments in their promoted type before storage conversion; refine converted switch values and case ranges. |
| `DataflowCheckedIntegers.cpp` | Apply overflow builtins and their output writes; specialize checked-product `calloc`/`reallocarray` success and failure. |
| `DataflowIntegerProofs.cpp` | Use range bounds, overflow-success predicates and matching `MAX / count` guards to establish non-overflow. |
| `DataflowNumericOutputs.cpp` | Capture guarded numeric returns and caller-visible writes, snapshot inputs before call effects, and install final output facts afterward. |
| `DataflowNumericInputs.cpp` | Capture every numeric contract dependency before the callee writes its input storage; retire prior snapshot generations. |
| `DataflowGuardCompleteness.cpp` | Require each must-contract premise to survive projection, accepting equivalent predicates that deduplicate. |
| `DataflowLoopRequirements.cpp` | Recognize eligible unit-stride loops with stable bounds; exclude early exits and unsupported induction from minimum requirements. |
| `DataflowDynamicExtents.cpp` | Capture VLA dimensions and `sizeof`, check dimension bounds, and derive array-subobject extents from target record layout. |

`Dataflow.cpp` connects these transfers to ordinary scalar state, ownership
guards, reference-count adjustments and spatial requirements. Array, string,
heap and sized-field code use the same numeric facts. Numeric writes and
may-alias writes invalidate dependencies; allocation, output and VLA snapshots
retain earlier values under bounded source-site identities. Reusing a snapshot
site invalidates stale dependencies. Inferred pointer/count field witnesses
carry the C multiplication type through `ProgramDatabase` and sidecars.

The monotone set of overwritten numeric inputs uses packed `PlaceSet` words;
copying a large CFG state does not allocate a tree node for every written
place. Recursive function components join fresh summaries into the previous
approximation, including reporting passes. May-effect guards weaken, must
postconditions retain agreement, and separately guarded requirements keep
their premises. This prevents alternating numeric/temporal guard projections
from cycling while retaining the existing convergence failure limit.

An abstract range endpoint can prove an access safe or establish a definite
violation, but cannot alone witness the possible-boundary diagnostic. Source
constraints such as `i <= 8` provide that witness; a type-derived upper bound
such as `INT_MAX - 1` under `i < unknown_count` does not.

C values and byte intervals are distinct. `malloc(n * sizeof(T))` receives
the actual C multiplication result, including unsigned wrap. An access first
evaluates its index in C, then computes its first byte and exclusive end using
checked mathematical byte arithmetic. A wrapped product's mathematical upper
bound may establish a violation, but cannot establish that an access fits.
Repeated expression identity supports one-past-product checks without general
nonlinear solving. VLA extents use captured positive dimensions where the byte
product is representable. Flexible tails use allocation bytes minus the target
field offset, retaining the enclosing object's lifetime and release identity;
fixed-array subobjects keep their own bounds.

`PathAffine` can carry a typed expression followed by mathematical byte scaling.
`ExtentRequirement` carries its guard, exclusive end and optional start.
Supported zero-based, unit-stride loops with two upper bounds export a minimum
bound. Early-exit and other unsupported loops do not produce inferred
must-requirements: a possible access alone cannot establish a required bound
for every caller. Arbitrary strides and induction remain outside this inference.
Caller arguments are converted to the interface types before substitution.
Unresolved requirements can pass through wrappers, while an unsupported
condition is never deleted to create an unconditional caller error.

`SpatialCheck` records `Proven`, `Violation` or `Unresolved`. Proving an access
requires both lower and upper bounds; violations retain the existing definite
or supported reachable-boundary policy. The final reporting pass aggregates
checks by source operation. `--dump-analysis` prints
`spatial: proven=<n> violation=<n> unresolved=<n>` and unresolved reason counts.
These counts are independent of unsafe-region reporting and warning controls.
A caller requirement is an obligation, not a proof that all callers satisfy it.

`SummaryFormatVersion` is **15** and `SidecarFormatVersion` is **16**. Numeric
outputs use `numeric <path> value ...` records; `requires-extent` retains
optional `start` intervals and typed guards. Core validates types, operators,
paths, shapes and limits. Comparison, global remapping and dependency
invalidation visit expression leaves and conditions; losing a required global
invalidates the dependent expression or premise. Frontend transports these
facts through the existing whole-program engine. Rebuild older object sidecars.

Unknown arbitrary indices remain unresolved without automatically producing
`out-of-bounds`. Unsupported numeric projections use `analysis-incomplete`.
Integers wider than 64 bits, general nonlinear inequalities, arbitrary loop
invariants, unrestricted alias/provenance models, unions and type punning,
byte-encoded pointers, GC invariants and concurrency remain outside the model.
Trusted annotations and `WEAVEC_ASSUME` do not widen actual allocations. There
is no runtime instrumentation, `--verify` flag or whole-program certificate.

## Diagnostics contract

Every diagnostic carries a stable identifier from `weavec::core::diag`
(`use-after-free`, `double-free`, `conflicting-borrow`, ...). It is printed
in brackets as `[weavec::<id>]` and is part of the user-facing contract:
scripts and editors may filter on it, so renaming one is a breaking change.

## Build structure

- `cmake/WeaveCLLVM.cmake` finds LLVM/Clang, sets `-fno-rtti`/`-fno-exceptions`
  to match the LLVM build, and provides `weavec_link_llvm` /
  `weavec_link_clang` which respect `LLVM_LINK_LLVM_DYLIB` /
  `CLANG_LINK_CLANG_DYLIB`.
- `cmake/WeaveCHelpers.cmake` provides `weavec_add_library` /
  `weavec_add_executable`, which apply warnings, include paths and export
  metadata uniformly.
- Everything is installed with a CMake package config (`find_package(WeaveC)`)
  so external tools can link `weavec::Core` or `weavec::Frontend`.

## Corpus

`scripts/corpus.py` runs the tool over real C projects
(`scripts/corpus/projects.json`), one file at a time or whole-program
(`"whole_program": true`), and compares diagnostic counts with
`scripts/corpus/baseline.json`; see `scripts/corpus/README.md`. It is the
empirical check on the RFCs' precision claims and runs weekly in CI.
RFC 0014 also pins a smaller subset for both release pull-request jobs.

The fixed evaluation suite (`scripts/evaluate.py`, `test/evaluation/`) is
separate from corpus counts and recall regression pins. It retains known
misses in its denominator and rejects parse errors, crashes, timeouts and
unexpected diagnostics. Both it and its harness unit tests run under CTest.


RFC 0013 also keeps a must-fact for objects allocated within the current
function. Cleanup below those objects does not become consumption of entry
fields merely because the object was published through an interface path.
Copies and record copies preserve the fact; unknown non-null alternatives
drop it at joins. Scalar guard snapshots carry scalar/null facts; RFC 0014
also preserves pointer predicates through input identities. Pointer-value
snapshots retain the required reachable state.

## Pointer identity and contextual call effects (RFC 0014)

`Core/CallTargets` stores bounded symbol sets with independent unknown and
null alternatives. `AnalysisState` carries these values through pointer and
record operations. Summary value sources can carry function values; pointer
comparison predicates share the existing bounded guard representation.

`DataflowCallbacks.cpp` resolves each call against its current state.
`CallbackSummaries.cpp` specializes ordinary `FunctionDataflow` analyses under
callback bindings. Contexts are bounded and cached, and active recursive
contexts remain explicit incomplete boundaries. Generic summaries record the
parameter paths used as callbacks. A caller binds those paths; the callee body
keeps the associated userdata and operation ordering. Declarations retain
authority over the specialized summary.

`UnitExports` carries callback requests, specialized summaries and global
function values. Interfaces capable of accepting callbacks introduce reverse
scheduling dependencies, allowing requests and their answers to settle in the
existing program SCC fixpoint before reporting. Function references also
introduce dependencies, including references in global initializers. Extra
type-compatible edges order inference but do not contribute effects.

`DataflowMemory.cpp` snapshots complete pointer or compatible record copies
before writing their destination. Existing ownership, heap and alias transfer
machinery applies the snapshot. Partial or unsupported pointer-containing
copies discard affected must-facts and record incomplete coverage.
`DataflowViews.cpp` validates the record views attached to summary paths;
Analysis supplies layout keys and Core remains independent of Clang.

Summary and sidecar format 10 serialize these values, pointer predicates,
record views and incomplete reasons. All representations use deterministic
ordering, and sidecar readers reject malformed or oversized contexts.

## Checked contracts (RFCs 0018–0019)

`Core/Safety` owns the obligation ledger, object identities, guarded initialized
and zeroed ranges, and bounded branch premises. Pointer holders and pointee
storage have separate identities; initialization cannot establish provenance.
`Core/CheckedContract` implements sufficient precondition union and
postcondition intersection; `Core/CheckedIO` provides bounded portable
serialization. `FunctionSummary::checked` is deliberately separate from
witness-based `requiresExtent`, and survives global remapping and joins.

The Analysis layer attaches operation accounting to the existing CFG transfer
and reporting passes. `DataflowSafety` inventories supported semantics,
initialization and diagnostics; `DataflowSafetyMemory` resolves byte intervals
and entry requirements; `DataflowSafetyCalls` discharges requirements and models
library effects; `DataflowSafetyContracts` captures call-entry dependencies and
applies conditional output facts; `DataflowSafetyLoops` projects sufficient
counted-loop bounds and proves complete supported fills/copies on normal exits.
`PendingOutcome` carries memory and numeric output facts until an outcome is
selected, with invalidation when output storage or dependencies change.
The additional state is enabled by checked selection or a report request.

RFC 0021 adds stable same-array positions and terminated-prefix witnesses to
the checked state. `Core/Traversal` provides bounded difference constraints
and target pointer-difference arithmetic; `Core/Relation` joins both sides of
a difference interval and widens changing bounds through a finite zero
threshold. These components contain no Clang or LLVM dependencies.

`CheckedRequirements` shares ordered entry/output sets between copied
contracts. Its public iterators are immutable; insertion and intersection
detach shared storage before modifying it. Duplicate joins preserve storage,
the original requirement cap and exact portable contents. This avoids copying
large conditional guards while rebuilding whole-program databases.
Global remapping retains shared checked sets when none of their paths,
affine expressions or guard predicates references a global. Path projection
caches immutable local/synthetic failures as well as stable interface paths;
array selectors remain dependent on the current state.
At unit export, a sufficient entry requirement may omit antecedent predicates
on known private globals. This strengthens its caller obligation while keeping
the native contract precise. Required object/interval references and every
public output premise still undergo strict global remapping.
`DataflowCursors` captures and updates byte coordinates;
`DataflowPointerOperations` validates same-array comparisons and differences;
`DataflowTraversalRelations` verifies common incoming facts and sufficient
buffer envelopes; `DataflowStringTraversal` maintains initialized termination
witnesses. `Dataflow.cpp` retains all CFG edges, including direct gotos, and
can partition eligible small loops by completed back edges before falling
back to widening. Every final obligation is checked on converged states.

`position`, `progress` and explicit `terminator` quantities cross interfaces
through checked-record encoding 3. Call-entry snapshots precede ordinary
effects, with output positions installed afterward. Portable call contexts
can additionally contain directed same-array byte-pointer orders (`o:`
records); these neither equate addresses nor invent ownership shares. They
participate in context equality, global remapping, validation and the existing
context budgets. Interacting writes must preserve the witness byte or require
separation of the actual referents.
`AnalysisState` stores it in an optional domain, so ordinary analysis skips
constructing, copying and joining the proof containers.

Every computed definition, including `main` and private helpers, has a record
in `UnitExports::checkedDefinitions`. This does not change externally visible
function lookup. `Frontend/CheckedReport` collects reporting passes and writes
JSON atomically. `Frontend/CheckedArtifacts` fingerprints build inputs and
objects for compiler-sidecar validation before replay. Compilation may defer
an unavailable external contract; the link pass must resolve it. Checked
failure reaches Clang independently of the diagnostic filtering policy.

Summary format 16 and sidecar format 17 carry guarded/outcome-qualified contracts,
numeric outputs and RFC 0021 traversal records. Checked record encoding uses
version 3; report JSON uses expanded version 2 or compact version 3.
Strict parsing and global remapping reject missing premises. Explanation depth
is bounded independently of semantic contract limits; truncating a call chain
does not change the obligation it explains.

The unit exporter omits optional postconditions rooted wholly in private
globals, following RFC 0005's namespace boundary. Local callers still see
those facts. Entry requirements and private premises of public/result output
facts retain strict remapping; exporting a contract cannot silently forget
something its proof needs.

## Reuse and work accounting (RFC 0020)

`AnalysisStats` is a Clang-free invocation-owned counter/timer sink. The frontend
shares it with function and context analyses and writes the explicit JSON output
atomically. No counter affects the model's joins or coverage decisions.

`SummaryStore` records dependencies in every active analysis frame. Context hits
inherit their dependencies into callers. Function and global-fact updates remove
only affected contexts; revision snapshots also detect changes during a context's
own computation. Invalidated nodes remain alive until the outermost applying
analysis finishes. A preparation cache is owned by the retained AST and contains
CFGs, lexical lifetimes and liveness with its nonreturning-block assumptions.
Place IDs, initial states, call effects and diagnostics are per-run data.

`CompilationDatabaseUnit` and the compiler's `Cc1Unit` retain an AST during
whole-program iteration. The frontend's common analysis/replay path attaches a
fresh reporting consumer, the current program database and warning controls.
`SafetyLedger` copies share storage until mutation. Semantic equality compares
operation identity, outcome and exhaustion; diagnostic wording and route choice
have a separate equality operation.

`AnalysisCache` stores complete unit results only after their program component
settles. Private format 2 separates shared obligation/path/ledger tables from
canonical sidecar records carrying the remaining export metadata. It checks
producer round trips in the same global namespace and validates all table
references before restoring contracts. Diagnostics and dependencies use bounded
JSON records. The cache format is independent of sidecar format 17. Input validation preprocesses an effective
invocation; imported validation projects observed symbols plus conservative
global facts and requests. The [guide](incremental-analysis.md) describes cache
misses and compact report version 3.

Sidecar format 16 introduced `checked-preprocessing`. The compiler
computes it from the effective preprocessor invocation before compiling, then
recomputes it from the recorded command before validating any checked link
input. This catches new conditional-include targets as well as changed loaded
files. Unsupported or unverifiable preprocessing cannot substantiate checked
object replay. RFC 0021 extends the records with summary format 16 and
sidecar format 17; the preprocessing and executable bindings still apply.
