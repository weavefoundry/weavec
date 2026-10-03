# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based C compiler and analysis tool that adds inferred
ownership and borrowing to C. For every safety facet (spatial, null,
temporal) of every memory operation it records one outcome in a *ledger*:
proven, checked by a runtime check it inserts, guarded by a check against
the runtime's object table, a definite violation (an error), or unresolved
or trusted with a reason. Three C++ libraries:
`weavec::Core` (the model, the ledger, pointer kinds, the library table;
**no Clang/LLVM includes allowed**), `weavec::Analysis` (Clang AST → core
facts: sites, kinds, the object engine behind the `SafetyEngine` seam in
`lib/Analysis/Engine*.cpp`, the check planner; the only layer that includes
both), `weavec::Frontend` (deferred CodeGen, check and guard emission, object
registration, zero-initialisation, ledger writers, unit records,
whole-program orchestration, the compiler driver, diagnostics bridging).
`runtime/` is the C runtime every enforcing link carries: the object table
(heap, stack and global objects), the guards and the reports
(`libweavec_rt.a`), the arena allocator's `malloc` family
(`libweavec_alloc.a`), and the out-of-line check helpers for precompiled
headers (`libweavec_chk.a`). `tools/weavec` is a libTooling CLI;
`tools/weavec-cc` is a drop-in C compiler (Clang's driver with WeaveC
inside) that inserts the checks and guards, links the runtime and analyses
the whole program at link time. Full picture:
`docs/architecture.md`. Model semantics and the reasoning behind them:
`docs/rfcs/`. Read `0001-ownership-model.md` first, then
`0030-prove-or-trap.md`, which is the current model: it replaces RFC 0001's
guarantee, amends RFCs 0002–0017 where they say so, and supersedes RFCs
0018–0029. Then `0031-object-engine.md`, which replaced the engine behind
the seam and the summary format (30); read its *Implementation amendments*
too. Then `0032-runtime-enforcement.md`, the current amendment of both: the
`guarded` outcome, the runtime, the guards, ledger JSON version 2 and unit
record format 30; read its *Implementation amendments* too, which override
its body. There is no checked mode.

## Before touching the model or the checker

Design decisions for `Core`, the checker (the engine behind `SafetyEngine`:
`ObjectEngine` in `lib/Analysis/Engine*.cpp` over the domain in
`include/weavec/Core/Heap.h`), the ledger and its outcomes and reasons,
`LibrarySpec.txt`, `weavec.h`, diagnostic ids, the inserted checks and
guards, the runtime (`runtime/`), and what crosses translation units (exports, the program database, the unit
record) are recorded as RFCs in `docs/rfcs/`.
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
- Runtime test: `ctest --preset dev -R '^runtime$'` (`runtime/test/rt_test.c`,
  built as `weavec_rt_test`; test names as arguments run only those).
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`).
- Test cases: `scripts/run-cases.py [--filter 'soundness/**']` (see
  `test/cases/README.md`); CTest runs them as `cases-<suite>`. `--asan` adds
  the ASan oracle; `--checks verify` fails on any `weavec.proven` trap.
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
   `scripts/check-hygiene.py`; RFC 0031 §2): only the `Engine*.cpp` files
   include the engine's private header `lib/Analysis/Engine.h`; the engine
   publishes only through `LedgerAdapter` and never receives a
   `DiagnosticSink`; library behaviour goes in
   `lib/Core/LibrarySpec.txt`, never in `name == "…"` tests; nothing under
   `lib/` names a corpus project; library code stays within its line
   budget, and so does the runtime (`runtime/` without `runtime/test/`,
   counted apart; RFC 0032 gate H1). The runtime is C and includes no WeaveC
   header but its own `weavec_rt.h`.

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a model / checker change     | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Add a checker rule                   | After an RFC, behind the seam: the object engine (`lib/Analysis/Engine*.cpp`, `ObjectEngine`; decisions in `EngineDecide.cpp`) publishes only through `LedgerAdapter` (`include/weavec/Analysis/LedgerAdapter.h`, `SafetyEngine.h`) |
| Change the abstract domain (objects, cells, symbols, zone, joins) | `include/weavec/Core/{Heap,Zone,Persistent}.h`, `lib/Core/Heap.cpp`, `lib/Core/Zone.cpp` (unit tests in `unittests/Core/HeapTest.cpp`) |
| Change outcomes, reasons, the summary line | `lib/Core/Ledger.cpp`, `lib/Analysis/LedgerAdapter.cpp` (defaults, rollup) |
| Change which sites exist             | `lib/Analysis/SiteCollector.cpp`                       |
| Change pointer kinds / how annotations become kinds | `lib/Core/PointerKind.cpp`, `lib/Analysis/AttributeReader.cpp`, `lib/Analysis/KindInference*.cpp`; what the engine takes from them: `lib/Analysis/EngineKinds.cpp` |
| Change which checks are inserted     | `lib/Analysis/CheckPlanner.cpp` (plan), `lib/Frontend/CheckEmitter.cpp` and `Prelude.cpp` (emission) |
| Change which facets get a guard      | `CheckPlanner::planGuards` in `lib/Analysis/CheckPlanner.cpp` (RFC 0032 §6); the need of a call argument's guard: `objectWitness` in `lib/Analysis/EngineLibrary.cpp`; possible findings dropped on guarded facets: `LedgerAdapter::dropGuardedPossible` |
| Change the guard helpers / range caches | `lib/Frontend/Prelude.cpp` (`GuardHelpers`, `RuntimeHelpers`, `RuntimeDeclarations`), `lib/Frontend/CheckEmitter.cpp` (`planCache`, `declareCaches`, `isQuietLoop`); oracle tests `test/Emission/runtime-oracle-*.c` |
| Change stack / global object registration | `lib/Frontend/ObjectRegistration.cpp` (`planObjects`), `CheckEmitter::registerObjects` |
| Change the runtime (allocator, object table, guards, reports) | `runtime/weavec_alloc.c`, `weavec_malloc.c`, `weavec_objects.c`, `weavec_report.c`, `weavec_rt.h` (the prelude declares the same entry points); its C test `runtime/test/rt_test.c` (CTest `runtime`); cases in `test/cases/semantics/runtime/` |
| Change the runtime's link line and fallbacks | `lib/Frontend/Driver.cpp` (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`); `test/Driver/runtime-*.c` |
| Change zero-initialisation           | `lib/Frontend/ZeroInit.cpp`                            |
| Evaluate an expression / map an lvalue to an address | `lib/Analysis/EngineExpr.cpp` (`Transfer::evaluate`, `Transfer::addressOf`) |
| Model a C library function / allocator / releaser | `lib/Core/LibrarySpec.txt` (one unit test per row in `unittests/Core/LibrarySpecTest.cpp`); how the engine applies a row: `lib/Analysis/EngineCalls.cpp` (effects), `lib/Analysis/EngineLibrary.cpp` (argument requirements) |
| Change function-pointer slots        | `lib/Core/FnSlots.cpp`, `lib/Analysis/SlotCollector.cpp` |
| Change how a callee's summary is found | `lib/Analysis/EngineCalls.cpp` (`CallApplier::applyDirect`), `UnitRun::summaryOf` in `lib/Analysis/EngineUnit.cpp` (RFC 0031 §5.4) |
| Change how a summary is derived or applied | `lib/Analysis/EngineSummary.cpp` (RFC 0031 §6), `include/weavec/Core/Effects.h` |
| Change the TU driver / call graph    | `lib/Analysis/EngineUnit.cpp` (`UnitRun`), `lib/Analysis/UnitPipeline.cpp` |
| Change what a unit exports / the program database | `UnitRun::exports` in `lib/Analysis/EngineUnit.cpp`, `lib/Analysis/ProgramDatabase.cpp` (RFC 0005, RFC 0031 §7) |
| Change the summary text format       | `lib/Core/EffectsIO.cpp` (format 30; round-trip tests in `unittests/Core/EffectsIOTest.cpp`) |
| Change the whole-program algorithm / link step | `lib/Frontend/ProgramAnalysis.cpp`, `lib/Frontend/Driver.cpp` |
| Change the unit record (`foo.o.weavec`) | `lib/Frontend/UnitRecord.cpp` (format 30; the schema fingerprint follows the codec's field table), `lib/Frontend/RecordPayload.cpp` (the field table) |
| Change the ledger JSON / SARIF       | `lib/Frontend/LedgerWriter.cpp` (`weavec-ledger` version 2, `LedgerSchemaVersion`), `lib/Frontend/LedgerOutput.cpp` |
| Change `weavec-cc` (driver, cc1 wrapping, link step) | `lib/Frontend/Driver.cpp`, `tools/weavec-cc/main.cpp` |
| Change `-W` / `-fweavec-*` handling  | `lib/Frontend/DiagnosticControl.cpp`, `DriverOptions` in `Driver.h` |
| Debug what the runtime did           | `WEAVEC_RT_STATS=1 ./prog` prints the allocator's and the guards' counters on stderr at exit (`runtime/weavec_report.c`); `-fweavec-checks=report` prints each failed check or guard and goes on |
| Debug what the checker inferred      | `weavec --dump-analysis file.c --`; `weavec --dump-kinds file.c --`; `weavec --ledger=out.json file.c --`; `weavec --whole-program --dump-analysis a.c b.c --`; `WEAVEC_ENGINE_DUMP=1` prints every summary on stderr, `=2` adds each run's exit states, `=3` each block's entry state instead (`lib/Analysis/EngineUnit.cpp`, `EngineSummary.cpp`, `EngineRun.cpp`) |
| Add or run a test case               | `test/cases/README.md`, `scripts/run-cases.py`         |
| Measure precision on real code       | `scripts/corpus-gate.py`, `test/corpus/` (README, manifest, expected ratchet, triage) |
| Change how diagnostics are rendered  | `lib/Frontend/ClangDiagnosticSink.cpp`                 |
| Add a CLI flag                       | `tools/weavec/main.cpp`, `FrontendOptions`; driver flags in `Driver.h` |
| Add an annotation                    | `Annotations.h`, `weavec.h`, `docs/annotations.md`     |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
