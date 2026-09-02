# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based tool that adds inferred ownership and borrowing
checks to C. Three C++ libraries: `weavec::Core` (the ownership model, **no
Clang/LLVM includes allowed**), `weavec::Analysis` (Clang AST → core facts;
the only layer that includes both), `weavec::Frontend` (Clang
`FrontendAction`, whole-program orchestration, sidecars, the compiler
driver, diagnostics bridging). `tools/weavec` is a libTooling CLI;
`tools/weavec-cc` is a drop-in C compiler (Clang's driver with WeaveC
inside) that analyses the whole program at link time (RFC 0005).
Full picture: `docs/architecture.md`; model semantics and the reasoning
behind them: `docs/rfcs/` (start with `0001-ownership-model.md`).

## Before touching the model or the checker

Design decisions for `Core`, `lib/Analysis/Dataflow.cpp` (the checker),
`weavec.h`, diagnostic ids, and what crosses translation units (exports,
the program database, the sidecar) are recorded as RFCs in
`docs/rfcs/`. **Read the relevant RFC before changing any of these**, and
treat it as authoritative over comments in the code. If the change you are
about to make is not covered by an Accepted RFC, or contradicts one, stop and
write or amend an RFC first (`docs/rfcs/README.md` explains when one is
required and the process); do not encode a new design decision in code
alone. Bug fixes that bring code in line with an RFC need no RFC.

## Build and test

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Unit tests: `build/dev/unittests/WeaveC{Core,Analysis,Frontend}Tests`.
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`).
- Format: `scripts/format.sh`; check: `scripts/check-format.sh`.

## Rules that reviewers will enforce

1. Never add `clang/` or `llvm/` includes under `include/weavec/Core` or
   `lib/Core`.
2. Every new diagnostic gets a stable id in `weavec::core::diag`
   (`include/weavec/Core/Diagnostic.h`), an entry in `docs/annotations.md`,
   a unit test, and a lit test that pins the exact message.
3. Annotation spellings live in exactly two places that must agree:
   `include/weavec/Analysis/Annotations.h` (`spelling::`) and
   `resources/include/weavec.h`.
4. Warnings are errors in CI (`WEAVEC_WARNINGS_AS_ERRORS=ON`). Do not add
   suppressions without a comment.
5. Follow the existing file header block and naming (`CamelCase` types and
   constants, `camelBack` functions/variables, `static` free functions rather
   than anonymous namespaces); `.clang-tidy` enforces it.
6. Update `CHANGELOG.md` under `[Unreleased]` for user-visible changes.
7. Do not commit generated files (`build/`, `compile_commands.json`).
8. Changes to `Core`, checker rules, annotations or diagnostic ids reference
   the RFC that specifies them (in the PR and, for lit tests, in the
   filename: `test/Analysis/rfc0002-*.c`).

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a model / checker change     | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Add a checker rule                   | `lib/Analysis/Dataflow.cpp` (after an RFC)             |
| Map an expression to a place         | `lib/Analysis/PlaceBuilder.cpp`                        |
| Recognise an allocator / releaser    | `lib/Analysis/Allocators.cpp`, `lib/Analysis/Builtins.cpp` (libc table) |
| Change how a callee's summary is found | `lib/Analysis/Summaries.cpp` (`SummaryStore`, RFC 0003/0005) |
| Change the TU driver / call graph    | `lib/Analysis/TranslationUnitAnalysis.cpp`             |
| Change what a unit exports / the program database | `lib/Analysis/ProgramDatabase.cpp` (RFC 0005) |
| Change the summary text format       | `lib/Core/SummaryIO.cpp` (versioned; round-trip tests) |
| Change the whole-program algorithm   | `lib/Frontend/ProgramAnalysis.cpp`                     |
| Change the sidecar file (`foo.o.weavec`) | `lib/Frontend/Sidecar.cpp` (bump `SidecarFormatVersion`) |
| Change `weavec-cc` (driver, cc1 wrapping, link step) | `lib/Frontend/Driver.cpp`, `tools/weavec-cc/main.cpp` |
| Change `-W` / `-fweavec-*` handling  | `lib/Frontend/DiagnosticControl.cpp`, `DriverOptions` in `Driver.h` |
| Debug what the checker inferred      | `weavec --dump-analysis file.c --`; `weavec --whole-program --dump-analysis a.c b.c --` |
| Measure precision on real code       | `scripts/corpus.py`, `scripts/corpus/README.md`        |
| Change how diagnostics are rendered  | `lib/Frontend/ClangDiagnosticSink.cpp`                 |
| Add a CLI flag                       | `tools/weavec/main.cpp`, `FrontendOptions`; driver flags in `Driver.h` |
| Add an annotation                    | `Annotations.h`, `weavec.h`, `docs/annotations.md`     |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
