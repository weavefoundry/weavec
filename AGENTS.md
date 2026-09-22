# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based C compiler and analysis tool that adds inferred
ownership and borrowing to C. For every safety facet (spatial, null,
temporal) of every memory operation it records one outcome in a *ledger*:
proven, checked by a runtime check it inserts, a definite violation (an
error), or unresolved or trusted with a reason. Three C++ libraries:
`weavec::Core` (the model, the ledger, pointer kinds, the library table;
**no Clang/LLVM includes allowed**), `weavec::Analysis` (Clang AST → core
facts: sites, kinds, the engine behind the `SafetyEngine` seam, the check
planner; the only layer that includes both), `weavec::Frontend` (deferred
CodeGen, check emission, zero-initialisation, ledger writers, unit records,
whole-program orchestration, the compiler driver, diagnostics bridging).
`runtime/` holds the small C runtime for report mode and precompiled
headers. `tools/weavec` is a libTooling CLI; `tools/weavec-cc` is a drop-in C
compiler (Clang's driver with WeaveC inside) that inserts the checks and
analyses the whole program at link time. Full picture:
`docs/architecture.md`. Model semantics and the reasoning behind them:
`docs/rfcs/`. Read `0001-ownership-model.md` first, then
`0030-prove-or-trap.md`, which is the current model: it replaces RFC 0001's
guarantee, amends RFCs 0002–0017 where they say so, and supersedes RFCs
0018–0029. There is no checked mode.

## Before touching the model or the checker

Design decisions for `Core`, the checker (the engine behind `SafetyEngine`,
today `FunctionDataflow` in `lib/Analysis/Dataflow*.cpp`), the ledger and
its outcomes and reasons, `LibrarySpec.txt`, `weavec.h`, diagnostic ids,
the inserted checks, and what crosses translation units (exports, the
program database, the unit record) are recorded as RFCs in `docs/rfcs/`.
**Read the relevant RFC before changing any of these**, and treat it as
authoritative over comments in the code. If the change you are about to
make is not covered by an Accepted RFC, or contradicts one, stop and write
or amend an RFC first (`docs/rfcs/README.md` explains when one is required
and the process); do not encode a new design decision in code alone. Bug
fixes that bring code in line with an RFC need no RFC.

## Build and test

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Unit tests: `build/dev/unittests/WeaveC{Core,Analysis,Frontend}Tests`.
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`).
- Test cases: `scripts/run-cases.py [--filter 'soundness/**']` (see
  `test/cases/README.md`); CTest runs them as `cases-<suite>`.
- Corpus gate: `scripts/corpus-gate.py --quick` (see `test/corpus/README.md`).
- Hygiene gate: `scripts/check-hygiene.py`.
- Format: `scripts/format.sh`; check: `scripts/check-format.sh`.

## Rules that reviewers will enforce

1. Never add `clang/` or `llvm/` includes under `include/weavec/Core` or
   `lib/Core`.
2. Every new diagnostic gets a stable id in `weavec::core::diag`
   (`include/weavec/Core/Diagnostic.h`), an entry in `docs/annotations.md`
   and `docs/data/diagnostic-remedies.json`, a unit test, and a lit test that
   pins the exact message.
3. Annotation spellings live in exactly two places that must agree:
   `include/weavec/Analysis/Annotations.h` (`spelling::`) and
   `resources/include/weavec.h`.
4. Warnings are errors in CI (`WEAVEC_WARNINGS_AS_ERRORS=ON`). Do not add
   suppressions without a comment.
5. Follow the existing file header block and naming (`CamelCase` types and
   constants, `camelBack` functions/variables, `static` free functions rather
   than anonymous namespaces); `.clang-tidy` enforces it.
6. Use Conventional Commit PR titles and document user-visible changes in the
   relevant guides. `CHANGELOG.md` is generated solely by semantic-release;
   do not edit it manually.
7. Do not commit generated files (`build/`, `compile_commands.json`).
8. Changes to `Core`, checker rules, annotations or diagnostic ids reference
   the RFC that specifies them in the PR. New tests are named by feature
   (`test/cases/<area>/…`, `test/Emission/<feature>-*.c`); existing
   `rfcNNNN-` names may stay.
9. Keep the engine seam and the library table clean (gate H2,
   `scripts/check-hygiene.py`): only `DataflowEngine.cpp`,
   `FunctionAnalysis.cpp`, `CallbackSummaries.cpp`,
   `CallContextSummaries.cpp`, `KindSeeding.cpp` and the `Dataflow*.cpp`
   files include `Dataflow.h`; `FunctionDataflow` publishes only through `LedgerAdapter`
   and never receives a `DiagnosticSink`; library behaviour goes in
   `lib/Core/LibrarySpec.txt`, never in `name == "…"` tests; nothing under
   `lib/` names a corpus project; library code stays within its line
   budget.

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a model / checker change     | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Add a checker rule                   | After an RFC, behind the seam: the engine (`lib/Analysis/Dataflow*.cpp`, run by `DataflowEngine`) publishes only through `LedgerAdapter` (`include/weavec/Analysis/LedgerAdapter.h`, `SafetyEngine.h`) |
| Change outcomes, reasons, the summary line | `lib/Core/Ledger.cpp`, `lib/Analysis/LedgerAdapter.cpp` (defaults, rollup) |
| Change which sites exist             | `lib/Analysis/SiteCollector.cpp`                       |
| Change pointer kinds / how annotations become kinds | `lib/Core/PointerKind.cpp`, `lib/Analysis/AttributeReader.cpp`, `lib/Analysis/KindInference*.cpp`; what the engine takes from them: `lib/Analysis/KindSeeding.cpp` |
| Change which checks are inserted     | `lib/Analysis/CheckPlanner.cpp` (plan), `lib/Frontend/CheckEmitter.cpp` and `Prelude.cpp` (emission), `runtime/` |
| Change zero-initialisation           | `lib/Frontend/ZeroInit.cpp`                            |
| Map an expression to a place         | `lib/Analysis/PlaceBuilder.cpp`                        |
| Model a C library function / allocator / releaser | `lib/Core/LibrarySpec.txt` (one unit test per row in `unittests/Core/LibrarySpecTest.cpp`), `lib/Analysis/Allocators.cpp` |
| Change function-pointer slots        | `lib/Core/FnSlots.cpp`, `lib/Analysis/SlotCollector.cpp` |
| Change how a callee's summary is found | `lib/Analysis/Summaries.cpp` (`SummaryStore`, RFC 0003/0005) |
| Change the TU driver / call graph    | `lib/Analysis/TranslationUnitAnalysis.cpp`, `lib/Analysis/UnitPipeline.cpp` |
| Change what a unit exports / the program database | `lib/Analysis/ProgramDatabase.cpp` (RFC 0005) |
| Change the summary text format       | `lib/Core/SummaryIO.cpp` (versioned; round-trip tests) |
| Change the whole-program algorithm / link step | `lib/Frontend/ProgramAnalysis.cpp`, `lib/Frontend/Driver.cpp` |
| Change the unit record (`foo.o.weavec`) | `lib/Frontend/UnitRecord.cpp` (format 28; the schema fingerprint follows the codec's field table), `lib/Frontend/Sidecar.cpp` |
| Change the ledger JSON / SARIF       | `lib/Frontend/LedgerWriter.cpp`, `lib/Frontend/LedgerOutput.cpp` |
| Change `weavec-cc` (driver, cc1 wrapping, link step) | `lib/Frontend/Driver.cpp`, `tools/weavec-cc/main.cpp` |
| Change `-W` / `-fweavec-*` handling  | `lib/Frontend/DiagnosticControl.cpp`, `DriverOptions` in `Driver.h` |
| Debug what the checker inferred      | `weavec --dump-analysis file.c --`; `weavec --dump-kinds file.c --`; `weavec --ledger=out.json file.c --`; `weavec --whole-program --dump-analysis a.c b.c --` |
| Add or run a test case               | `test/cases/README.md`, `scripts/run-cases.py`         |
| Measure precision on real code       | `scripts/corpus-gate.py`, `test/corpus/` (README, manifest, expected ratchet, triage) |
| Change how diagnostics are rendered  | `lib/Frontend/ClangDiagnosticSink.cpp`                 |
| Add a CLI flag                       | `tools/weavec/main.cpp`, `FrontendOptions`; driver flags in `Driver.h` |
| Add an annotation                    | `Annotations.h`, `weavec.h`, `docs/annotations.md`     |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
