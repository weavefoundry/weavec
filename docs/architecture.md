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

`lib/Core` contains everything that is *about* ownership and nothing that is
about C or Clang. It depends only on the C++ standard library. This is the
piece the README asks to keep "as modular as possible": it can be unit-tested
without parsing any code, reused by a different frontend, or embedded in other
tools.

| Header             | Purpose                                                                                        |
| ------------------ | ---------------------------------------------------------------------------------------------- |
| `Ownership.h`      | `OwnershipKind` lattice (`Unknown ⊑ {Owned, Shared, Mutable} ⊑ Raw`) and `join`. `Raw` is "no guarantee": tracked, but usable only inside an unsafe region. |
| `Place.h`          | `PlaceId` and `PlaceTable`: structured places (`p`, `s.f`, `*p`, `p->f`, `a[*]`) with parent/descendant/translate queries. |
| `AliasRelation.h`  | Symmetric may-alias graph over places; closed under copies, plain union at joins (deliberately not transitive). |
| `Lifetime.h`       | `LifetimeId` and `LifetimeConstraints` (transitive `outlives` queries; `'static` is id 0).      |
| `Borrow.h`         | `Loan` (place, kind, lifetime, holder) and `BorrowState`: may this borrow be created; may this place be moved or mutated. |
| `Moves.h`          | `MoveTracker`: which places are currently moved-out/freed (and through which alias), with a conservative `join`. |
| `Raw.h`            | `RawTracker`: which places currently hold a raw pointer, why (`RawReason`: integer cast, `WEAVEC_RAW` declaration, loaded through a raw pointer, callee result, unchecked callee) and through which alias; union at joins. |
| `AnalysisState.h`  | The dataflow state: moves, loans, aliases, raw pointers, pending `realloc`s and inferred kinds, with component-wise `join`. |
| `Summary.h`        | `FunctionSummary`: what a function does to its interface. `SummaryPath` (`param(i)`/`global(g)` plus deref/field/index steps), `PlaceEffect` (read/written/freed/moved), `Store` (value written into caller-visible memory), `ValueSource` (fresh/copy/borrow/null/raw/unknown), with `join`, `remapGlobals` and the derived `consumes`/`borrowKind`/`inferredKind` queries. |
| `SummaryIO.h`      | The stable text form of a `FunctionSummary` (`summary` ... `end` records; RFC 0005): `printSummary`/`parseSummary` with callbacks that name and resolve globals, so the format is Clang-free and the on-disk sidecar format is defined here. |
| `Scc.h`            | Tarjan's strongly connected components over an adjacency list, in reverse topological order; used for the call graph inside a unit and for the unit graph of a program. |
| `Diagnostic.h`     | `Diagnostic`, stable ids in `diag::` (with `All`, `isKnown`, `isWarningByDefault`), `FixItHint`, `DiagnosticSink`, and an in-memory `DiagnosticCollector`. |
| `SourceLocation.h` | Frontend-neutral positions with an `opaque` slot for the frontend's native encoding.           |

The core never sees a `clang::VarDecl`; it sees a `PlaceId`. It never sees a
`clang::SourceLocation`; it sees a `core::SourceLocation` whose `opaque` field
the frontend fills in so it can report at the exact original position.

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
  For a call through a function pointer (`lookupIndirect`) the order is:
  annotations on the pointer's type, else the join of the summaries of every
  address-taken function of that type in the TU and in the program
  database, else the same default;
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
  emits each diagnostic once. While running it applies callee summaries at
  every call, records its own effects, stores and returns, checks them
  against the function's annotations, tracks raw pointers and reports raw
  operations, and produces the function's `FunctionSummary` at exit.
  `WEAVEC_UNSAFE` regions are analysed like any other code; the pass only
  suppresses what it would report inside them. `FunctionAnalysis.h` is the
  per-function entry point;
- drives a whole translation unit (`TranslationUnitAnalysis.h`,
  `TranslationUnitAnalyzer`): collects definitions and address-taken
  functions, builds the call graph (with an edge from every indirect call to
  each candidate of its type), and analyses strongly connected components in
  reverse topological order (callees first), iterating recursive components
  to a fixpoint on their summaries before the final reporting pass. With a
  `ProgramDatabase` attached, callers see callees from other units;
  `discover()` returns the unit's exports without analysing it (what it
  defines and imports, for ordering units) and `exports()` returns them
  with summaries after `run()`.

The model is specified by [RFC 0001](rfcs/0001-ownership-model.md), the
dataflow by [RFC 0002](rfcs/0002-intraprocedural-checking.md), summaries by
[RFC 0003](rfcs/0003-signature-inference.md), raw pointers, unsafe
regions and indirect calls by
[RFC 0004](rfcs/0004-unsafe-boundaries.md), and cross-unit analysis by
[RFC 0005](rfcs/0005-whole-program-analysis.md); each RFC's
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
  against the database of what has been analysed so far.
  `CompilationDatabaseUnit` parses from a compilation database.
- `Sidecar.h` reads and writes `foo.o.weavec`: the unit's exports, the cc1
  command that produced it and the diagnostics already reported, in a
  line-oriented text format versioned by its `weavec-summaries 1` header.
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
`-fweavec-dump-analysis`, `-fweavec-link`/`-fno-weavec-link` and the `-W`
spellings; everything else is Clang's. The design is
[RFC 0005](rfcs/0005-whole-program-analysis.md).

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
