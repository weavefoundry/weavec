# Architecture

WeaveC is organised as three C++ libraries and a thin command-line tool. The
libraries form a strict dependency chain; the arrows below point from a layer
to what it may depend on.

```
                 ┌──────────────────────────┐
                 │  tools/weavec (driver)   │
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  Clang FrontendAction, libTooling,
                 │  lib/Frontend            │  diagnostics bridging, resource dir
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
| `Summary.h`        | `FunctionSummary`: what a function does to its interface. `SummaryPath` (`param(i)`/`global(g)` plus deref/field/index steps), `PlaceEffect` (read/written/freed/moved), `Store` (value written into caller-visible memory), `ValueSource` (fresh/copy/borrow/null/raw/unknown), with `join` and the derived `consumes`/`borrowKind`/`inferredKind` queries. |
| `Diagnostic.h`     | `Diagnostic`, stable ids in `diag::`, `FixItHint`, `DiagnosticSink`, and an in-memory `DiagnosticCollector`. |
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
  this TU, the shipped libc/POSIX table (`Builtins.cpp`), and finally a
  documented default that also records the callee as an unknown boundary.
  For a call through a function pointer (`lookupIndirect`) the order is:
  annotations on the pointer's type, else the join of the summaries of every
  address-taken function of that type in the TU, else the same default;
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
  to a fixpoint on their summaries before the final reporting pass.

The model is specified by [RFC 0001](rfcs/0001-ownership-model.md), the
dataflow by [RFC 0002](rfcs/0002-intraprocedural-checking.md), summaries by
[RFC 0003](rfcs/0003-signature-inference.md), and raw pointers, unsafe
regions and indirect calls by
[RFC 0004](rfcs/0004-unsafe-boundaries.md); each RFC's *Implementation
notes* record where the code refines the design.

## `weavec::Frontend` — Clang integration

`lib/Frontend` adapts the analysis to Clang's frontend machinery:

- `WeaveCAction` is an `ASTFrontendAction` whose consumer hands the whole
  translation unit to `TranslationUnitAnalyzer`; every definition contributes
  a summary, but by default only those in the main file are reported.
- `ClangDiagnosticSink` forwards `core::Diagnostic`s (including fix-its) to
  Clang's `DiagnosticsEngine`, so WeaveC's output is rendered exactly like
  Clang's own (carets, colours, `-fdiagnostics-format=`,
  `-fdiagnostics-parseable-fixits`, `-Werror`, ...).
- `ResourceDir.h` locates `weavec.h` in installed and build-tree layouts.

Because it is a standard `FrontendAction`, the same code can later be exposed
as a Clang plugin or wired into a full compiler driver without changes to the
analysis.

## `tools/weavec` — the driver

Today `weavec` is a libTooling application: `weavec file.c -- <compiler
flags>` or `weavec -p build/ file.c` with a compilation database. It injects
`-isystem <resource-dir>/include` and `-D__WEAVEC__=1` so user code can
`#include <weavec.h>`. `--dump-analysis` prints the inferred facts and
summary per function for debugging; `--report-unannotated` offers fix-its
for exported functions; `--strict-externs` makes every call into unknown
code a raw operation, so it is an error outside a `WEAVEC_UNSAFE` region.

A drop-in compiler mode (`weavec -c foo.c -o foo.o`, i.e. behaving as `cc`
and delegating code generation to Clang) is planned; see
[roadmap.md](roadmap.md).

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
(`scripts/corpus/projects.json`) and compares diagnostic counts with
`scripts/corpus/baseline.json`; see `scripts/corpus/README.md`. It is the
empirical check on the RFCs' precision claims and runs weekly in CI.
