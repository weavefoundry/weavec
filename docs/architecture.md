# Architecture

WeaveC is a memory-safety checker for C and a drop-in C compiler, built on
Clang and LLVM. [RFC 0030](rfcs/0030-prove-or-trap.md), *Prove or trap*,
defines its design. Each safety *facet* (spatial, null, temporal) of every
memory operation, or *site*, gets exactly one outcome in the *ledger*:
proven, checked by a compiler-inserted runtime check, a definite violation
(a build error), or unresolved or trusted with a reason from a closed list.
`weavec-cc` inserts the checks into the AST through Sema before a deferred
CodeGen, with no ABI change. Temporal safety stays static: possible temporal
bugs are warnings and ledger rows, never runtime checks.

The code is three C++ libraries, two thin command-line tools and a small C
runtime. The arrows point from a layer to what it may depend on.

```
        ┌──────────────────────┐  ┌──────────────────────────┐
        │  tools/weavec        │  │  tools/weavec-cc         │
        │  (libTooling)        │  │  (drop-in C compiler)    │
        └──────────┬───────────┘  └────────────┬─────────────┘
                   └───────────┬───────────────┘
                               ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  deferred CodeGen, check
                 │  lib/Frontend            │  emission, ledgers, unit
                 │                          │  records, link step, driver
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Analysis        │  kinds, sites, slots, the
                 │  lib/Analysis            │  engine seam, FunctionDataflow,
                 │                          │  check planning
                 └──────┬───────────┬───────┘
                        ▼           ▼
        ┌────────────────────┐   ┌────────────────────┐
        │  weavec::Core      │   │  Clang / LLVM      │
        │  lib/Core          │   │  (external)        │
        │  no Clang/LLVM     │   └────────────────────┘
        └────────────────────┘
```

`runtime/` is C code that `weavec-cc` links into user programs, only for
report mode and for precompiled-header and module builds. It depends on
nothing above. The layering rule is strict:

- Core includes nothing from `clang/` or `llvm/`. It is the one library
  built without their include paths.
- Analysis is the only layer that knows both Core and Clang.
- Inside Analysis, the components on the near side of the engine seam (see
  *The engine seam*) never include `Dataflow.h`. Only `DataflowEngine.cpp`,
  `FunctionAnalysis.cpp`, `CallbackSummaries.cpp`, `CallContextSummaries.cpp`
  and the `Dataflow*.cpp` files may.

RFC 0030 is being implemented in stages. This page describes its design; the
[roadmap](roadmap.md) says which stages have landed. Checked mode (RFCs
0018–0029) was removed by RFC 0030 and remains in the repository history at
tag `v0.10.0`.

## `weavec::Core` — the model

`lib/Core` holds the model and depends only on the C++ standard library, so
it can be unit-tested without parsing code and reused by another frontend.
It never sees a `clang::VarDecl`, only a `PlaceId`, and never a
`clang::SourceLocation`, only a `core::SourceLocation` whose `opaque` field
the frontend fills in. Check operands that name program state are opaque
place handles, which Analysis resolves to Clang declarations.

### The ledger and its companions

| Header | Purpose |
| --- | --- |
| `Ledger.h` | Sites, facets and outcomes (RFC 0030 §2): `SiteKind`, `Facet`, `SiteOutcome`, the closed reason lists with their JSON spellings and phrases, merging by rank, the defaults for undecided facets, `UnitLedger` and `Ledger` with requirement records, diagnostics and the A1–A5 assumption counts, the `summary` rollup and the summary line. |
| `PointerKind.h` | The kind lattice (§7.1): `single`, `counted(e)`, `sized(e)`, `ended-by(q)`, `nul-terminated` or `unknown`, with nullability, a source (declared, inferred, default) and an `ExtentTerm` over a sibling parameter or field. Every kind is a lower bound. `ExtentClass` says whether an extent is exact, declared or a lower bound; only the first two may be check operands, and only an exact extent can make an access a violation. |
| `LibrarySpec.h`, [`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt) | The one declarative table of C library, POSIX, platform and builtin functions (§8), and its parser: per argument, the access, required length, nullability, ownership effect, release family, state slots and callback clause; per call, the result, disjointness, `exits`/`noreturn`, format arguments and fortified aliases. It also lists the platform headers of §5.2. CMake embeds the text, and it replaces the three library models of v0.10.0. |
| `CheckPlan.h` | Checks as pure data (§10.1): per checked requirement record or lowered violation, a template (`nonnull`, `index`, `span`, `len`, `disjoint`, `assert`), a form, a placement, an optional guard and `CheckTerm` operands. |
| `FnSlots.h` | Function-pointer slots (§9.3): the constraints `f ∈ S`, `S ⊆ T` and `open(S)` over field, global, parameter, result and local slots, and the solver that computes each slot's targets and whether it is closed. |

A site is one operation in one emitted function, identified by
`{function, ordinal, kind, location}`. Its kind is `deref`, `index`,
`ptr-arith`, `cast`, `int-to-ptr`, `lib-call`, `release`, `call` (a call or
a function exit), `assume` or `raw` (§2.1), and a facet exists only where it
has meaning. `assume` sites have a fourth facet, *assertion*. Records of one
facet merge by rank within one pass:
`violation > unresolved > checked > trusted > proven`. The unresolved reasons
are `unknown-extent`, `unknown-index`, `inexpressible`, `may-released`,
`may-moved`, `may-alias-released`, `may-invalid-release`,
`may-mismatched-release`, `may-dangle`, `may-conflict`, `unknown-callee`,
`callback`, `setjmp`, `budget`, `unanalysed`, `raw-cast`, `dangling-escape`,
`second-owner` and `no-zero-init`. The trust reasons are `unsafe`,
`system-api`, `library-spec`, `extern-contract`, `caller-contract`,
`external-unit` and `concurrency`. Adding a reason requires an RFC.

### The ownership, integer and spatial model

| Header | Purpose |
| --- | --- |
| `Ownership.h` | `OwnershipKind` lattice (`Unknown ⊑ {Owned, Shared, Mutable} ⊑ Raw`); `Raw` is usable only inside an unsafe region. |
| `Place.h` | `PlaceId` and `PlaceTable`: structured places (`p`, `s.f`, `*p`, `p->f`, `a[*]`, `a[0]`) with parent and descendant queries. |
| `Array.h` | Bounded selectors, half-open intervals and sparse spans (RFC 0015). |
| `AliasRelation.h` | Symmetric may-alias graph, closed under copies and joined by union (deliberately not transitive). Each edge records the `PointerOffset` between its ends (RFC 0011) and the element meant (RFC 0006). |
| `Lifetime.h` | `LifetimeConstraints`: transitive `outlives` queries; `'static` is id 0. |
| `Borrow.h` | `Loan` and `BorrowState`: whether a borrow may be created and a place moved or mutated. The loans of dead holders expire (RFC 0006). |
| `Moves.h` | `MoveTracker`: moved-out and freed places, with element witnesses (RFC 0006), guards (RFC 0009) and certainty bits. `Uninitialized` marks pointer locals never assigned (RFC 0008). |
| `Nullness.h` | `NullTracker`: `Null`, `MaybeNull` or `NonNull` per place, joined by the RFC 0008 table. |
| `Resource.h` | `ResourceTracker`: owned resources with origin, release family, escape and shares (RFC 0007, RFC 0010). |
| `Raw.h` | `RawTracker`: which places hold raw pointers, and why (RFC 0004). |
| `Scalar.h` | `ValueFact`, the bounded guards `PlaceGuard` and `PathGuard`, and `ScalarTracker` (RFC 0009). |
| `Integer.h`, `IntegerExpression.h` | Target integers, modular ranges, checked-overflow results and bounded typed expressions (RFC 0017). |
| `Offset.h` | `PointerOffset`: `Zero`, `Elements(k)`, `Field(key)` or `Unknown` (RFC 0011). |
| `Spatial.h` | `Affine` extents, `SpatialRecord` with string facts (RFC 0011, RFC 0012), `SpatialTracker`, and the pure bounds decision `checkSpatialBounds`. |
| `Relation.h` | `RelationTracker`: relations with offsets between integer places (`i < n + k`), and bounds against constants. |
| `CallTargets.h`, `CallContext.h` | Bounded sets of function values (RFC 0014) and call-context entry relationships (RFC 0016). |
| `AnalysisState.h` | The dataflow state: every tracker above, the pending outcomes of calls whose result is not yet tested, and the caller-visible paths overwritten on every path; component-wise `join`, and `learn` for condition edges. |
| `Diagnostic.h` | `Diagnostic`, `Certainty`, the ids in `diag::`, `FixItHint` and `DiagnosticSink`. |
| `SourceLocation.h`, `Scc.h`, `AnalysisStats.h` | Frontend-neutral positions with an `opaque` slot; Tarjan's strongly connected components for call and unit graphs; work counters, never part of a proof. |

### Certainty

Every diagnostic is *definite* or *possible* (RFC 0030 §3). A `MoveRecord`
is definite when it has `allPaths` (every predecessor merged since had it),
is not `conditional` (from an effect that holds only on some outcome classes
or paths, or from a `lossy` one) and is not of `unknownOrigin` (the
unknown-callee default or an open slot, never diagnosed). A `NullRecord`'s
`allocatorSource` bit keeps a null allocation result a checked facet rather
than a `null-dereference` error. A `Loan`'s `allPaths` bit makes
`conflicting-borrow` an error only when the conflict is reached on every
path. A join clears `allPaths` for a record present on one side only, even
when a guard encodes the condition, because guards are bounded and can be
weakened. A correlated bug is therefore a warning, not an error.

### Summaries

`FunctionSummary` (`Summary.h`) is what a function does to its interface,
over `SummaryPath`s: `param(i)`, `global(g)` or `result`, with dereference,
field and index steps. It holds effects with release families and guards,
stores and value sources, per-class outcomes and null facts, requirements,
heap descriptions (RFC 0013), numeric outputs (RFC 0017) and kinds.
`SummaryIO.h` defines its stable, Clang-free text form (RFC 0005). Summary
format 27 is format 26 without the parts only checked mode read, plus kinds,
reliance flags, the `lossy` flag and the restricted `outcome … when` cases of
RFC 0030 §9.1. The format-28 unit record carries summaries in this form.

## `weavec::Analysis` — the bridge

`lib/Analysis` is the only library allowed to include both `weavec/Core/*`
and `clang/*`. Besides the engine (next section), it holds shared services
and the components around the engine seam.

| Service | Role |
| --- | --- |
| `Annotations.h`, `ClangLocation.h` | Recognise WeaveC annotations on declarations, statements and function-pointer types; convert source locations both ways. |
| `Summaries.h` (`SummaryStore`) | Resolve a callee's summary, in order: its declaration's annotations, the summary inferred from its body in this unit, the program database, its `LibrarySpec` entry, else the defaults of RFC 0030 §5. A platform-header function borrows its arguments under `trusted(system-api)`; any other callee gets the unknown-callee may-effects. |
| `ProgramDatabase.h` | Other units' exports (summaries, linkage, type keys, imports, context requests), joined by name and type key and remapped into the importing unit's globals. |
| `Allocators.h`, `PlaceBuilder.h` | Classify calls by ownership effect; map expressions onto places and summary paths. Pointer arithmetic and pointer casts keep identity; only integer-to-pointer casts make raw values. |

Before the engine, these components read the AST and the `LibrarySpec`, with
no engine fact:

- `AttributeReader` reads the declared kinds (§7.2): the `WEAVEC_*` extent,
  string and nullability macros, Clang's `counted_by`, `sized_by`,
  `alloc_size` and nullability attributes, and `[static N]` and VLA
  parameters. `WEAVEC_*` annotations win over ecosystem attributes, which win
  over the `LibrarySpec` entry and then system-header attributes; a finding
  that rests only on the last is `trusted(system-api)`.
- `KindInference` fills the rest of the `KindTable` (§7.3–7.6): parameter
  kinds with their `reliesOnSingle` flags, result kinds, slot kinds (a
  greatest fixpoint that demotes `single` to `unknown` at any store that is
  not Single-valid), the must-access requirements R1–R5, store groups, and
  the counted-field candidates that Houdini rounds keep or drop.
- `SiteCollector` enumerates the sites of every emitted function, with their
  ordinals and facets, into the `SiteIndex` and the unit's undecided rows
  (§2.6). It runs after the kinds, because PtrArith and Cast sites exist
  only in required positions.
- `SlotCollector` collects the unit's function-pointer constraints for
  `FnSlots` (§9.3).

After the engine, these components complete the ledger:

- `BoundaryInvariants` (stage S7, not yet in the tree) checks at every call
  boundary and function exit that no place reachable from a parameter or
  global may hold a released or dangling pointer
  (`unresolved(dangling-escape)`), and that no two owning places may hold
  the same object (`unresolved(second-owner)`). It then downgrades every
  temporal facet that relied on the broken entry assumption (§9.4).
- `CheckPlanner` turns checked requirement records into `CheckPlan` entries,
  and adds a trap for each violation lowered to a warning (§10). It decides
  expressibility first (§10.3): an extra term must be side-effect free, name
  C places unmodified since the extent was derived, and compare against an
  exact or declared extent; otherwise the record is
  `unresolved(inexpressible)`. Planning is pure and runs in every mode, so
  the ledger does not depend on whether checks are emitted.
- `LedgerAdapter`, `SafetyEngine` and `DataflowEngine` form the seam
  described in *The engine seam*.

`UnitPipeline` (`runUnitAnalysis`) runs steps 2 and 3 of the compile
pipeline for one unit. It reads the declared kinds, collects the sites, runs
`DataflowEngine` through an authoritative `LedgerAdapter` (a discarding one
for a silent fixpoint round), calls `finish`, and reports the diagnostics in
publication order, followed by the require-level errors. A discovery-only
run returns the unit's exports without analysing it. Until stages S6 and S7
land, the slot solution and field candidates it passes are empty. The
Frontend calls it through `analyzeTranslationUnit`.

## The engine: `FunctionDataflow`

`FunctionDataflow` (`lib/Analysis/Dataflow.h`) is the prover behind the
seam. For each function body it runs a forward dataflow over `clang::CFG` to
a fixpoint, with `core::AnalysisState` as the lattice, then one final pass
that publishes each decision and diagnostic once. It applies callee summaries
at calls, refines the state on condition edges, and tracks loans
(RFC 0006), raw pointers (RFC 0004), resources and leaks (RFC 0007),
nullness (RFC 0008), integer facts and guards (RFC 0009), and strings, sized
fields and extents (RFC 0011, RFC 0012). It produces the function's summary
at exit.

RFC 0030 §15 bounds the changes inside it:

- The final pass decides every site it reaches by the rules of §3, with
  witnesses for the checks (`DataflowWitnesses.cpp`,
  `DataflowLibraryRequirements.cpp`). What used to be incomplete coverage is
  an unresolved decision: `budget`, `unanalysed`, `raw-cast` or
  `inexpressible`.
- An unknown callee gives a `Freed` record of unknown origin to each pointer
  argument's place, to what its non-`const` pointees reach, to escaped places
  and to externally reachable globals, and forgets their facts except each
  argument's own nullness and extent. Later uses are
  `unresolved(unknown-callee)`, and the call carries a fix-it. `asm` operands
  get the same default. In a function that calls `setjmp`, every temporal
  facet is `unresolved(setjmp)`.
- Nothing is suppressed in a `WEAVEC_UNSAFE` region: its spatial and null
  facets are `trusted(unsafe)`, temporal state is tracked as outside, and
  definite violations stay errors. `WEAVEC_ASSUME` is proven, refuted
  (`contradicted-assumption`) or checked.
- A body that transfers more CFG blocks than `-fweavec-budget` stops. Its
  facets take the defaults with reason `budget`, and its summary the
  unknown-callee effects. Callers of an incomplete summary add those
  may-effects to its known effects.
- Kinds seed extents at entry, loads and call results, marked exact,
  declared or lower bound. Trailing arrays are flexible, and a pointer to a
  member or element has the whole object's extent (§7.4).
- Summaries gain outcome cases with `lossy` bits (§9.1), non-null facts from
  guard functions (§9.2), kinds and an "always returns" fact.

`TranslationUnitAnalyzer` (`TranslationUnitAnalysis.h`) drives a unit. It
analyses the call graph's strongly connected components callees first,
iterating recursive ones to a fixpoint through a discarding adapter, then
gives each emitted function one authoritative pass
(`LedgerAdapter::beginFunction`). `discover()` returns the unit's exports
without analysing it.

RFCs [0001](rfcs/0001-ownership-model.md) (model),
[0002](rfcs/0002-intraprocedural-checking.md) (dataflow),
[0003](rfcs/0003-signature-inference.md) (summaries),
[0004](rfcs/0004-unsafe-boundaries.md) (unsafe boundaries),
[0005](rfcs/0005-whole-program-analysis.md) (whole program),
[0006](rfcs/0006-precision.md) (precision),
[0007](rfcs/0007-resource-lifecycle.md) (resources),
[0008](rfcs/0008-pointer-validity.md) (validity) and
[0009](rfcs/0009-value-conditional-behaviour.md) (guards) specify the model
and the engine. RFC 0030 replaces RFC 0001's guarantee statement and amends
RFCs 0002–0008 where it changes them; its §19 lists each amendment.

## `weavec::Frontend` — Clang integration

`lib/Frontend` adapts the analysis to Clang's frontend machinery, emits the
checks, writes ledgers and unit records, and runs the link step.

| Component | Role |
| --- | --- |
| `FrontendAction.h` | `WeaveCAction`, an `ASTFrontendAction` for libTooling, and `createWeaveCConsumer`, which `weavec-cc` runs at the end of each unit; both run `UnitPipeline`. Every emitted function is analysed, including `static inline` functions from user headers (§5.6). |
| `DeferredCodeGenConsumer` | Sits in front of CodeGen in every C code-generating action (§10.5). It forwards Sema set-up at once and records every other callback. At the end of the unit it runs the analysis and `CheckEmitter`, then replays the callbacks in order. Without deferral, CodeGen emits external functions before the analysis runs. It overrides every `ASTConsumer` and `SemaConsumer` virtual of LLVM 23, a list on the LLVM-upgrade checklist. |
| `CheckEmitter` | Applies the `CheckPlan` through Sema (§10.6): `BuildCallExpr` to the helper, `ImpCastExprToType` back to the operand's type so a dereference stays an lvalue, `BuildBinOp` with a comma for a check before a call. User expressions are never evaluated twice. A rewrite Sema rejects leaves the subtree unchanged and fails the compile with an internal error. It also applies the zero-initialisation lowering. |
| `Prelude` | The helpers the rewrites call, injected into the predefines buffer (§10.2): `static`, `always_inline`, `nodebug` functions for the six templates and their forms, term helpers that saturate toward failure, and the allocation wrappers. Trap mode calls `__builtin_verbose_trap("weavec", <template>)`, report mode `__weavec_rt_report`, and verify mode adds `__weavec_prv_*` with the category `weavec.proven`. PCH and module builds declare the helpers `extern` instead (§10.9). |
| `ZeroInit` | Plans the zero-initialisation of the allocation family (§11): calls to `LibrarySpec` entries with the `zero-init` flag become wrappers that zero the usable region, and `alloca` gets a `memset`. The plan is pure, so the ledger's A5 counts precede any rewrite. A unit that defines an allocator lowers nothing. |
| `LedgerWriter` | JSON (`weavec-ledger`, version 1) and SARIF 2.1.0 renderings of a ledger (§12), the `weavec-fp/1` fingerprints (a truncated SHA-256 of key, root-relative path, function, normalised message and ordinal), the fingerprint root and atomic writes. |
| `LedgerOutput` | Completes a unit or program ledger with the producer, root, configuration and the unit's source, object and target; applies the `-W` flags so the ledger counts what was reported; writes it where `-fweavec-ledger` says (a file, or a directory receiving one ledger per unit and per link) through a temporary file renamed into place; and prints the summary line under `-fweavec-summary`, whenever a ledger is written, and always in `weavec`. |
| `UnitRecord` | The format-28 codec (§13.1): framing, a typed header, and a payload checked against the codec's field table, whose SHA-256 is the schema fingerprint. The encoder refuses values the table does not describe; the decoder rejects missing, unknown and mistyped keys. |
| `ProgramAnalysis` | The whole-program algorithm of RFC 0005 over an abstract `ProgramUnit`: discover every unit's exports, order the units by strongly connected component, analyse acyclic units once and cyclic groups to a fixpoint, and publish in the last round only. It hosts the link step. |
| `Sidecar` | Reads and writes `<object>.weavec` next to each object. From stage S8 the file holds the format-28 unit record instead of the line-oriented RFC 0005 sidecar. |
| `Driver` | `weavec-cc`: Clang's driver plans the jobs, each `-cc1` job runs in-process behind `DeferredCodeGenConsumer`, compile jobs write the record, and link jobs run the link step before the linker. |
| `DiagnosticControl` | Applies the `-W` flags by each diagnostic's id and certainty. An error can be lowered but never disabled, and a flag naming a removed id is refused. `FilteringSink` drops what an earlier step already reported. |
| `ClangDiagnosticSink` | Forwards `core::Diagnostic`s, with notes and fix-its, to Clang's `DiagnosticsEngine`, so they render exactly like Clang's own. |
| `ResourceDir`, `AnalysisStats` | Locate `weavec.h`, the runtime archives, Clang's resource directory and `clang`; write the work counters of `--analysis-stats`. |

The ledger writers and the record codec live in Frontend because Core may
not use LLVM. They are built on `llvm::json` and `llvm::SHA256`, and a Core
JSON and SHA-256 implementation would duplicate LLVM's.

## The runtime

`runtime/` holds the only code WeaveC links into user programs. It is C,
installed under `lib/weavec` next to `weavec.h`. The default trap mode needs
none of it, because its checks are the prelude's inline helpers.

- [`weavec_rt.c`](../runtime/weavec_rt.c) builds `libweavec_rt.a`, whose
  `__weavec_rt_report` serves `-fweavec-checks=report`. A failed check
  prints `weavec: runtime check failed: <template> at <file>:<line>:<column>`
  once per site and the program goes on, or aborts under
  `WEAVEC_RT_ABORT=1`. The case runner attributes traps through this output.
- [`weavec_chk.c`](../runtime/weavec_chk.c) and
  [`weavec_chk_report.c`](../runtime/weavec_chk_report.c) build
  `libweavec_chk.a`, the out-of-line helpers for precompiled-header and
  module builds (§10.9), generated from the prelude by
  `weavec-cc -fweavec-print-prelude=out-of-line`. The report family carries a
  `_report` suffix, because one archive cannot define two signatures under
  one name.

`weavec-cc` links `libweavec_rt.a` in report mode, and adds `libweavec_chk.a`
to every checked link for the host; its members are linked only when a PCH
or module build referenced them.

## `tools/weavec` and `tools/weavec-cc`

`weavec` is a libTooling application: `weavec file.c -- <compiler flags>`, or
`weavec -p build/ file.c` with a compilation database (with `-p` and no
source, every file of the database). It injects
`-isystem <resource-dir>/include` and `-D__WEAVEC__=1`, so user code can
`#include <weavec.h>`. `--whole-program` analyses the files as one program.
It always prints the summary line; `--ledger`, `--ledger-format`,
`--require`, `--budget` and `--no-zero-init` model a `weavec-cc` build with
the default checks. `--dump-analysis` and `--dump-kinds` are debugging aids.

`weavec-cc` is the drop-in compiler: `CC=weavec-cc make`. Its own flags,
which `weavec-cc --help-weavec` lists, choose the checks mode
(`-fweavec-checks=trap|report|verify|none`), zero-initialisation, the
require level, the ledger, the summary line and the budget; everything else
is Clang's. The design is [RFC 0005](rfcs/0005-whole-program-analysis.md),
with the command line of RFC 0030 §16. The checked-mode flags, the analysis
cache, `--strict-externs`, `--exclusive-borrows`, `--analyze-headers` and
`--report-unannotated` are gone, with no aliases.

## Compile pipeline

`weavec-cc` compiles one C translation unit in five steps (RFC 0030 §1):

1. Clang parses the unit. `DeferredCodeGenConsumer` forwards Sema set-up and
   records every CodeGen callback without running it.
2. At `HandleTranslationUnit`, `AttributeReader` and the syntactic part of
   `KindInference` compute kinds and must-access requirements,
   `SiteCollector` enumerates the sites, and `SlotCollector` and `FnSlots`
   solve the unit's slots. The engine runs through `LedgerAdapter`, and
   `BoundaryInvariants` and the field-invariant rounds consume what it
   published. `LedgerAdapter::finish` fills the defaults, propagates
   boundary rows and plans the checks. This step runs in every mode.
3. Definite violations are reported as errors and possible temporal findings
   as warnings, with the require-level errors under `-fweavec-require`.
4. After an error, the callbacks are replayed unchanged and CodeGen drops the
   module. Otherwise, when checks are on, `CheckEmitter` applies the plan and
   the zero-initialisation lowering, and then the callbacks are replayed.
5. The object is written, then the format-28 record to `<object>.weavec`, the
   unit ledger to `-fweavec-ledger` if given, and the summary line under
   `-fweavec-summary` or `-fweavec-ledger`.

`weavec` runs steps 1–3 and 5 without CodeGen, per source or as one program
with `--whole-program`, and writes no object and no record. Its summary line
says `checkable (not enforced)` where a `weavec-cc` build would check.

Refinement is split by facet. Spatial, null and assertion outcomes are
decided once per unit, because they decide the emitted code, and the link
step copies them verbatim. Temporal outcomes are refined at link, where
calls into other units stop being unknown.

## The link step

When `weavec-cc` links, it runs the link step (RFC 0030 §13.2) before the
linker; `weavec --whole-program` uses the same `ProgramAnalysis`.

1. **Collect inputs.** `collectLinkInputs` resolves objects, archives,
   shared libraries and `-l` arguments as the linker does. One
   `unanalyzed-input` warning per link names every non-system input without
   a valid record. Calls into functions no record defines are then
   `trusted(external-unit)`.
2. **Solve slots** over all records.
3. **Verify declarations** against the defining units' summaries and kinds.
   A contradiction is an `annotation-mismatch` error.
4. **Re-run the engine** over the units with records, with the program
   database and the solved slots, to refine temporal facets. Only the last
   round publishes, and a definite violation fails the link.
5. **Verify interfaces.** Exported requirements and the reliance on Single
   defaults are decided at cross-unit callers, header-struct invariants are
   checked against every unit that stores to the fields, boundary rows
   propagate, and a unit that defines the allocator is recorded under A5.
6. **Compose the program ledger** from the units' spatial, null and
   assertion facets, the Call rows of step 5 and the temporal facets of
   step 4.

What no record covers stays listed under assumptions A1 and A3. Archives,
shared libraries and ccache do not carry records yet (RFC 0032), but every
link names the gap.

A unit record (§13.1) is one self-delimiting file, so that RFC 0032 can
place it verbatim into an object section:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `89 57 56 43 0D 0A 1A 0A` |
| 8 | 8 | format `u32` = 28, then flags `u32` = 0, little-endian |
| 16 | 32 | schema fingerprint: SHA-256 of the codec's field table |
| 48 | 16 | header length `H` and payload length `P`, `u64` each |
| 64 | `H` + `P` | header and payload, UTF-8 JSON |
| 64+`H`+`P` | 32 | SHA-256 of the bytes before it |

The header names the producer, source, `-cc1` command, target,
configuration and object digest. The payload holds the unit's functions
(summary format 27, kinds, reliance flags, exported requirements), imports,
slots, field invariants, context requests, boundary place classes, one
compact row per site and the diagnostics already reported, but no evidence.
Readers accept only format 28 with a matching schema fingerprint and a valid
digest. Anything else is a stale record, and the input counts as having
none.

## The engine seam

Everything an engine produces flows through one interface (RFC 0030 §14),
so RFC 0031 can replace `FunctionDataflow` by implementing `SafetyEngine`
without touching the ledger, kinds, library table, planner, emitter, formats
or tests. The types are in `include/weavec/Analysis/SafetyEngine.h`,
`LedgerAdapter.h` and `CheckWitness.h`.

`EngineInput` is everything an engine gets for one unit: the `ASTContext`,
the `SiteIndex`, the `KindTable`, the `LibrarySpec`, the `FnSlots` solution
(local, or program-wide at link), the `ProgramDatabase` at link, the field
candidates assumed at entry, and `EngineOptions` (budget,
zero-initialisation, require level, verify mode, strict aliasing, dump
stream and statistics). `LedgerAdapter` is the only channel back:

| Method | Carries |
| --- | --- |
| `beginFunction` | the start of a function's authoritative pass; rows from earlier passes are discarded |
| `decide` | one outcome, with reason and detail, for one facet of a known site; records merge by rank |
| `requirement` | one requirement record of a LibCall, Release or Call facet, kept with its own outcome and check |
| `report` | a diagnostic with its certainty, linked to its site and facet |
| `witness` | what a check needs: the extent and whether it is exact or declared, the base, the offset or index, library lengths |
| `boundary` | the places reachable from parameters and globals that may hold released pointers or aliased owners, with place classes |
| `overBudget` | a function that exceeded its budget |
| `storeVerdict` | a store group's verdict on a field-invariant candidate: holds, violated or unknown |
| `finish` | fills the defaults, applies the unsafe, `setjmp`, concurrency and boundary rules and the field-invariant upgrades, plans the checks, and returns the `PlannedLedger` |

An adapter is authoritative, discarding (fixpoint, Houdini and early link
rounds keep nothing) or collecting (context runs decide no row and keep
their diagnostics for the caller). A decision about a statement
`SiteCollector` did not enumerate is an internal error, and an
`unresolved(unanalysed)` row in a release build.

`SafetyEngine` is what an engine implements: `analyzeUnit(input, out)`,
`exports()` for the unit record, and `dump(function, os)` for
`--dump-analysis`. `DataflowEngine` implements it over `FunctionDataflow`.
`PlannedLedger` is the unit's `core::Ledger` and `core::CheckPlan`, with the
tables that resolve the plan's handles and site ids.

Two rules keep the seam honest, and gate H2 (`scripts/check-hygiene.py`)
checks both:

- `FunctionDataflow` publishes nothing except through `LedgerAdapter`,
  diagnostics included. It receives no `DiagnosticSink`.
- `SiteCollector`, `AttributeReader`, `KindInference`, `SlotCollector`,
  `BoundaryInvariants`, `CheckPlanner` and `LedgerAdapter` itself never
  include `Dataflow.h`.

## Heap postconditions and value snapshots (RFC 0013)

[RFC 0013](rfcs/0013-interprocedural-heap-state.md) adds
`FunctionSummary::heap`: per output path, a graph of result-relative pointer
cells. `copy-post` names an object the graph already holds and `copy` an
incoming value, so shared children and cycles stay finite.
`DataflowHeap.cpp` captures the final reachable facts, resolves incoming
values before a call replaces them, and materialises the graph into the
ordinary trackers. Graphs are bounded to eight path steps, 128 fields and
eight alternatives per cell; fields a graph leaves out are unknown to
callers. A call snapshots any guard operand or returned input pointer it can
overwrite, which keeps extraction (`p = *slot; *slot = NULL; return p`)
precise. `DataflowValues.cpp` moves an extent's dependency to an interned
snapshot before its scalar is overwritten. A snapshot has no C name, so an
extent over one is never a check operand: its requirement is
`unresolved(inexpressible)`.

## Pointer identity and call effects (RFC 0014)

`Core/CallTargets.h` holds bounded sets of function values with unknown and
null alternatives, which the state and summaries carry.
`DataflowCallbacks.cpp` resolves each indirect call against the targets the
state holds, and `CallbackSummaries.cpp` specialises `FunctionDataflow`
analyses under callback bindings. Function pointers stored in fields and
globals are resolved by the slots of RFC 0030 §9.3, which replace
RFC 0014's callback-global fixpoint and its `callbackGlobals` export. A call
through a closed slot with one target is analysed as a direct call, and with
several targets as the join of their summaries. An open slot with known
targets gives their temporal facts only, under `trusted(extern-contract)`;
an open slot without targets gets the unknown-callee default with reason
`callback`. Every indirect call has a null facet on its callee operand.

`DataflowMemory.cpp` snapshots complete pointer and compatible record copies
(`memcpy`, `memmove`) before writing the destination. Pointers from a partial
or unsupported copy, or seen through an incompatible record view
(`DataflowViews.cpp`), are `unresolved(raw-cast)` where they are used.

## Arrays and containers (RFC 0015)

`Core/Array.h` represents a selector as a constant or an immutable scalar
plus an offset. Selected `Index` places live below the array's storage, and
the empty `Index` is an unknown element; moves, aliases, ownership, nullness
and heap children use these ordinary places. `AnalysisState` adds sparse
range-copy, fill and release facts, at most 32 cells and 32 ranges per
object. The `DataflowArray*.cpp` files resolve selectors, handle
simultaneous copies and reallocations, keep copy snapshots and apply
complete traversals, which summaries carry as `array-copy`, `array-fill`
and `array-release` records. An unsupported composition leaves the facets
that needed it unresolved.

## Compositional calls (RFC 0016)

`Core/CallContext.h` describes a callee's entry relationships: aliases,
offsets, shares, distinct objects, scalar and null facts, and callback
bindings, with all-or-nothing global remapping. `DataflowCallContext.cpp`
projects the caller's state into a validated context at the callee's entry,
and `CallContextSummaries.cpp` reuses `FunctionDataflow` to check the body
under it. The context runs of one function share a block-transfer budget,
after which the default-context summary applies. A context run never decides
the rows of the function it analyses (§2.6). It sharpens the summaries
callers see and keeps its diagnostics: each is reported at the use in the
callee with a note naming the call, and linked to that call's Call site,
whose temporal facet becomes a violation or `unresolved(may-released)`. An
absent alias edge is never a proof of disjoint inputs.

## Target integers and compositional bounds (RFC 0017)

[RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) specifies
the target-integer model. Core represents integer types of 1 to 64 bits:
`IntegerValue` is an unsigned bit pattern, `IntegerRange` holds at most two
intervals, and transfers model unsigned wrap, signed validity and
conversions. An invalid operation supplies no invented value.
`IntegerExpression<Key>` holds canonical typed expressions of at most 64
nodes, and guards admit eight conjuncts. Exceeding a limit loses precision,
and the facets that needed it stay unresolved.

| File in `lib/Analysis` | Responsibility |
| --- | --- |
| `IntegerSupport.h` | Target widths, signedness, bit-field widths and operators; checked builtins and value-preserving conversions. |
| `DataflowIntegers.cpp` | Typed ranges of AST expressions, refined comparisons, numeric guards, definite invalid operations. |
| `DataflowIntegerExpressions.cpp` | Bounded expressions, affine forms where justified, substitution of interface inputs, dependency snapshots. |
| `DataflowIntegerStatements.cpp` | Compound assignments in their promoted type; converted switch values and case ranges. |
| `DataflowCheckedIntegers.cpp` | The overflow builtins and their output writes; checked-product `calloc` and `reallocarray`. |
| `DataflowIntegerProofs.cpp` | Non-overflow from range bounds, overflow-success predicates and `MAX / count` guards. |
| `DataflowNumericOutputs.cpp`, `DataflowNumericInputs.cpp` | Guarded numeric returns and caller-visible writes; input snapshots before a callee writes them. |
| `DataflowGuardCompleteness.cpp` | Every premise of a must-fact survives projection. |
| `DataflowLoopRequirements.cpp` | Unit-stride loops with stable bounds; no minimum requirement from early exits. |
| `DataflowDynamicExtents.cpp` | VLA dimensions, `sizeof`, and object extents from the record layout, with flexible trailing arrays. |

C values and byte intervals are distinct: `malloc(n * sizeof(T))` receives
the actual C product, and an access computes its bytes in checked
mathematical arithmetic, so a wrapped product can establish a violation but
never that an access fits. `core::checkSpatialBounds` needs a lower and an
upper bound to prove an access. Its result maps onto the spatial facet
(RFC 0030 §3.3): a violation against an exact extent is an `out-of-bounds`
error; an undecided access against an exact or declared extent is checked
when its terms are expressible; an access that only a lower-bound kind
covers is `unresolved(unknown-extent)`. A summary's `requiresExtent` and
`requiresNonNull` are may-facts for summaries and fix-its; call-site checks
and errors come from the must-access requirements of §7.5.

## Diagnostics contract

Every diagnostic carries a stable id from `weavec::core::diag`, printed as
`[weavec::<id>]`. Scripts and editors filter on ids, so renaming one is a
breaking change. There are 20:

| Ids | Default severity |
| --- | --- |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`, `mismatched-release`, `invalid-release` | error when definite, warning when possible |
| `null-dereference`, `use-of-uninitialized`, `out-of-bounds` | error, reported only when definite |
| `unsafe-operation`, `annotation-mismatch`, `invalid-integer-operation`, `contradicted-assumption` | error |
| `unresolved-operation` | error, only under `-fweavec-require=checked` or `proven`, or in a `WEAVEC_REQUIRE_SAFE` function |
| `unchecked-operation` | error, only under `-fweavec-require=proven` |
| `leak`, `invalid-annotation`, `unanalyzed-input` | warning |
| `allocation-failure` | warning, off by default (`-Wweavec-allocation-failure`) |

`diag::defaultSeverity(id, certainty)` gives these severities. Possible null
and spatial findings are checked facets rather than diagnostics. `-Werror`
in project flags does not promote WeaveC warnings; `-Werror=weavec[-<id>]`
does. An error can be lowered with `-Wno-error=weavec-<id>` but not
disabled, and a lowered violation still traps. RFC 0030 removed
`analysis-incomplete` (now unresolved rows, with reasons such as
`unanalysed` and `budget`), `annotation-required` (now
`unresolved(unknown-callee)` rows with fix-its), `checking-incomplete` and
`checking-failed`. A `-W` flag naming one of them is an error.

## Tests and gates

- **Unit tests** (`unittests/`, GoogleTest) test each component alone;
  `LibrarySpecTest.cpp` checks every library entry against an independent,
  hand-written expectation table.
- **Lit tests** (`test/Analysis`, `test/Annotations`, `test/Driver`,
  `test/WholeProgram`, `test/Prelude`, `test/Emission`) pin exact messages
  and driver behaviour; `test/Emission` holds the rewrite-oracle pairs.
- **`test/cases`** is one tree of executable C cases by feature, with
  expectations as line-comment markers (`BUG`, `TRAP`, `UNRESOLVED`,
  `CLEAN`, …; see its [README](../test/cases/README.md)).
  [`scripts/run-cases.py`](../scripts/run-cases.py) builds each case with
  `weavec-cc`, checks its diagnostics and ledgers, and runs it in trap and
  report mode, optionally under an ASan oracle. CTest registers one
  `cases-<suite>` test per top-level directory, so `ctest -j` runs the
  suites in parallel.
- **`test/corpus`** pins 9 real projects in 11 configurations.
  [`scripts/corpus-gate.py`](../scripts/corpus-gate.py) runs `--quick` on
  every pull request, and `--full` (builds, the projects' own test suites,
  injected bugs, benchmarks) weekly and for releases. `expected.json` is a
  ratchet, and `triage.json` holds a verdict for every definite error and
  possible temporal warning. See its [README](../test/corpus/README.md).

[`scripts/codegen-identity.py`](../scripts/codegen-identity.py) implements
gate G7: with `-fweavec-checks=none`, objects are byte-identical to Clang's.
`scripts/check-hygiene.py` implements gate H2: no checked-mode remnants
outside `docs/rfcs/`, no libc name comparisons outside the library table,
the seam rules and the line budgets. `run-cases.py` measures G1–G6,
`test/Emission` G8, and `corpus-gate.py` G9–G15.

## Build structure

- The top-level `CMakeLists.txt` builds `lib/` (Core, Analysis, Frontend),
  `tools/`, `runtime/` and, with `WEAVEC_BUILD_TESTS`, `unittests/` and
  `test/`.
- `cmake/WeaveCLLVM.cmake` finds LLVM and Clang and provides the
  `weavec::llvm` interface target and `weavec_link_llvm`/`weavec_link_clang`,
  which respect `LLVM_LINK_LLVM_DYLIB` and `CLANG_LINK_CLANG_DYLIB`.
- `cmake/WeaveCHelpers.cmake` provides `weavec_add_library` and
  `weavec_add_executable`. Only `USES_LLVM` targets get LLVM's include
  paths, and Core is not one of them.
- `lib/Core/CMakeLists.txt` embeds `LibrarySpec.txt` as a byte array at
  configure time; editing the table re-runs the configure step.
- `runtime/CMakeLists.txt` builds both archives into the build tree's
  `lib/weavec`, generating the helper bodies with the freshly built
  `weavec-cc`.
- A CMake package config (`find_package(WeaveC)`) exports `weavec::Core`,
  `weavec::Analysis` and `weavec::Frontend` to external tools.
