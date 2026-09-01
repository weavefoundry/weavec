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
| `Ownership.h`      | `OwnershipKind` lattice (`Unknown ⊑ {Owned, Shared, Mutable} ⊑ Raw`) and `join`.               |
| `Place.h`          | `PlaceId` — an opaque handle for a storage location — and `PlaceTable` for display names.     |
| `Lifetime.h`       | `LifetimeId` and `LifetimeConstraints` (transitive `outlives` queries; `'static` is id 0).      |
| `Borrow.h`         | `Loan` and `BorrowState`: may this borrow be created; may this place be moved or mutated.       |
| `Moves.h`          | `MoveTracker`: which places are currently moved-out/freed, with a conservative `join`.         |
| `Diagnostic.h`     | `Diagnostic`, stable ids in `diag::`, `DiagnosticSink`, and an in-memory `DiagnosticCollector`. |
| `SourceLocation.h` | Frontend-neutral positions with an `opaque` slot for the frontend's native encoding.           |

The core never sees a `clang::VarDecl`; it sees a `PlaceId`. It never sees a
`clang::SourceLocation`; it sees a `core::SourceLocation` whose `opaque` field
the frontend fills in so it can report at the exact original position.

## `weavec::Analysis` — the bridge

`lib/Analysis` is the only library allowed to include both `weavec/Core/*` and
`clang/*`. It:

- recognises WeaveC annotations on declarations and statements
  (`Annotations.h`);
- converts source locations in both directions (`ClangLocation.h`);
- walks function bodies, maps declarations onto `PlaceId`s, and feeds events
  (borrow, move, free, use) into the core model (`FunctionAnalysis.h`).

The current `LocalOwnershipChecker` is a small, path-insensitive AST walk that
exists to exercise the pipeline end to end. The planned replacement is a
CFG-based dataflow analysis over `clang::CFG`; see
[design/ownership-model.md](design/ownership-model.md).

## `weavec::Frontend` — Clang integration

`lib/Frontend` adapts the analysis to Clang's frontend machinery:

- `WeaveCAction` is an `ASTFrontendAction` whose consumer runs the analyzer
  over every function definition (by default only those in the main file).
- `ClangDiagnosticSink` forwards `core::Diagnostic`s to Clang's
  `DiagnosticsEngine`, so WeaveC's output is rendered exactly like Clang's own
  (carets, colours, `-fdiagnostics-format=`, `-Werror`, ...).
- `ResourceDir.h` locates `weavec.h` in installed and build-tree layouts.

Because it is a standard `FrontendAction`, the same code can later be exposed
as a Clang plugin or wired into a full compiler driver without changes to the
analysis.

## `tools/weavec` — the driver

Today `weavec` is a libTooling application: `weavec file.c -- <compiler
flags>` or `weavec -p build/ file.c` with a compilation database. It injects
`-isystem <resource-dir>/include` and `-D__WEAVEC__=1` so user code can
`#include <weavec.h>`.

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
