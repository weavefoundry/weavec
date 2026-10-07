# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based drop-in C compiler and an ownership analysis
for C. `weavec-cc` guards every memory access of the C it compiles: LLVM
passes (`lib/Frontend/Guard*.cpp`) put a guard before every load, store,
atomic operation, memory intrinsic and library-call memory argument, remove
a guard only where a local rule proves it redundant, lay out stack frames
and globals with redzones, and expand each remaining guard into an inline
check of the runtime's shadow memory; the program traps at the first bad
access with a report. `runtime/` is the C runtime every enforcing link
carries: the shadow, the arena allocator with its quarantine, the guards'
slow paths, the checked library wrappers and the reports
(`libweavec_rt.a`), and the `malloc` family over the arena
(`libweavec_alloc.a`). The ownership and lifetime analysis is *advisory*: it
runs in the `weavec` tool and under `weavec-cc -fweavec-diagnose`, reports
diagnostics and a summary line, and never changes generated code. Three C++
libraries: `weavec::Core` (the analysis's model, the analysis ledger,
pointer kinds, the abstract heap, the library table; **no Clang/LLVM
includes allowed**), `weavec::Analysis` (Clang AST → core facts: sites,
kinds, slots, the object engine behind the `SafetyEngine` seam in
`lib/Analysis/Engine*.cpp`; the only layer that includes both),
`weavec::Frontend` (the guard passes, unsafe regions, the enforcement
ledger, the analysis action and diagnostics bridging, whole-program
analysis, the compiler driver). `tools/weavec` is a libTooling CLI;
`tools/weavec-cc` is Clang's driver with WeaveC inside. Full picture:
`docs/architecture.md`. Model semantics and the reasoning behind them:
`docs/rfcs/`. Read `0001-ownership-model.md` first, then
`0030-prove-or-trap.md` through `0034-fast-enforcement.md` for the
analysis (the model of facets, outcomes and reasons, the object engine,
the library table, the work budget), and then
`0035-guard-by-default.md`, the current design: it supersedes the
enforcement of RFCs 0030–0034 (inserted checks, require levels, unit
records and the link step, guards lowered from declared calls) and keeps
their analysis as advisory. Read its *Implementation amendments* too; they
override its body. RFC 0036 (planned) brings ownership back as contracts
checked at function entry.

## Before touching the model, the passes or the runtime

Design decisions for `Core`, the analysis (the engine behind
`SafetyEngine`: `ObjectEngine` in `lib/Analysis/Engine*.cpp` over the
domain in `include/weavec/Core/Heap.h`), its outcomes and reasons,
`LibrarySpec.txt`, `weavec.h`, diagnostic ids, what is guarded and which
rules remove a guard, the shadow encoding, frames and globals, the runtime
(`runtime/`), the report formats, the enforcement ledger, and the driver's
flags are recorded as RFCs in `docs/rfcs/`. **Read the relevant RFC before
changing any of these**, and treat it as authoritative over comments in the
code. If the change you are about to make is not covered by an Accepted
RFC, or contradicts one, stop and write or amend an RFC first
(`docs/rfcs/README.md` explains when one is required and the process); do
not encode a new design decision in code alone. Bug fixes that bring code
in line with an RFC need no RFC. A rule that removes a guard must be local
to a function and monitored by `verify` mode (RFC 0035 §6); nothing the
analysis concludes may remove a guard.

## Build and test

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Unit tests: `build/dev/unittests/WeaveC{Core,Analysis,Frontend}Tests`
  (`GuardPassTest` runs the passes on IR text).
- Runtime test: `ctest --preset dev -R '^runtime$'` (`runtime/test/rt_test.c`,
  built as `weavec_rt_test`; test names as arguments run only those).
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`). `test/Guards/` pins the passes' IR (`ir-*.c`, on
  `-emit-llvm`) and the run-time reports.
- Test cases: `scripts/run-cases.py [--filter 'soundness/**']` (see
  `test/cases/README.md`); CTest runs them as `cases-<suite>`. `--asan` adds
  the ASan oracle; `--checks verify` fails on any `weavec.proven` report.
- Corpus gate: `scripts/corpus-gate.py --quick` (see `test/corpus/README.md`).
- Documentation examples: `python3 docs/scripts/check-examples.py --weavec
  build/dev/bin/weavec --weavec-cc build/dev/bin/weavec-cc`; the site:
  `npm ci && npm test && npm run build` in `docs/`.
- Hygiene gate: `scripts/check-hygiene.py`.
- Format: `scripts/format.sh`; check: `scripts/check-format.sh`.

## Rules that reviewers will enforce

1. Never add `clang/` or `llvm/` includes under `include/weavec/Core` or
   `lib/Core`.
2. Every new diagnostic gets a stable id in `weavec::core::diag`
   (`include/weavec/Core/Diagnostic.h`), an entry in `docs/annotations.md`
   and `docs/data/diagnostic-remedies.json`, a unit test, and a lit test that
   pins the exact message. A new run-time report kind is not a diagnostic:
   it gets a `test/Guards/` test that pins its exact report line, and an
   entry in the command-line reference's list of kinds.
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
7. Do not commit generated files (`build/`, `compile_commands.json`,
   `docs/.generated/`).
8. Changes to `Core`, the analysis's rules, the guard passes, the runtime,
   annotations or diagnostic ids reference the RFC that specifies them in
   the PR. New tests are named by feature (`test/cases/<area>/…`,
   `test/Guards/<feature>.c`); existing `rfcNNNN-` names may stay.
9. Keep the engine seam and the library table clean (`scripts/check-hygiene.py`;
   RFC 0031 §2): only the `Engine*.cpp` files include the engine's private
   header `lib/Analysis/Engine.h`; the engine publishes only through
   `LedgerAdapter` and never receives a `DiagnosticSink`; library behaviour
   goes in `lib/Core/LibrarySpec.txt` (the guards read it too, through
   `GuardLibrary.cpp`), never in `name == "…"` tests; nothing under `lib/`
   names a corpus project; library code stays within its line budget, and
   so does the runtime (`runtime/` without `runtime/test/`, counted apart).
   The runtime is C and includes no WeaveC header but its own `weavec_rt.h`,
   and the entry points it declares there must match the ones the passes
   declare.

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a design change              | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Change what weavec-cc adds to a compile (zero-init, array bounds, locations, the passes) | `configureEnforcement` in `lib/Frontend/Driver.cpp`; `registerGuardPasses` in `lib/Frontend/GuardPass.cpp` (RFC 0035 §1, amendment 1) |
| Change which accesses get a guard    | `lib/Frontend/GuardInsert.cpp` (`GuardInsert`, at `PipelineStartEP`: the markers `__weavec.guard`, `.range`, `.strlen`, `.disjoint`, `.scope`; RFC 0035 §2.1, amendments 1–2) |
| Change how a library call is guarded | `lib/Frontend/GuardLibrary.cpp` (row-driven guards, `wrapper(…)` redirection, `wide` rows, `%s`/`%n` arguments); the rows and their clauses in `lib/Core/LibrarySpec.txt` (one unit test per row in `unittests/Core/LibrarySpecTest.cpp`); the wrappers in `runtime/weavec_libc.c` (RFC 0035 §2.5, amendment 7) |
| Change the rules that remove guards  | `GuardPrune` (`PeepholeEP`; `pruneGuards` in `lib/Frontend/GuardInsert.cpp`: rule 6.1 after inlining) and `GuardExpand` (`OptimizerLastEP`; `lib/Frontend/GuardExpand.cpp`: rules 6.1–6.3); the passes' entry points are in `GuardPass.cpp`; `lib/Frontend/GuardLoops.cpp` (`GuardLoop`, `VectorizerStartEP`: loop versioning, rule 6.4); each removal must stay monitored by `verify` mode |
| Change the inline checks (fast paths, null bases) | `lib/Frontend/GuardPass.cpp`, `GuardPassImpl.h`; the shadow encoding in `runtime/weavec_rt.h` (RFC 0035 §2.2–§2.4, amendments 3–4); tests `test/Guards/ir-inline-check.c`, `null-base.c` |
| Change stack frames, scopes, dynamic allocas, non-local exits | `lib/Frontend/GuardFrames.cpp` (RFC 0035 §3, amendment 5); tests `test/Guards/ir-frames.c`, `stack-overflow.c`, `use-after-scope*.c`, `vla-overflow.c`, `longjmp.c`, `ir-noreturn.c` |
| Change global redzones and registration | `lib/Frontend/GuardGlobals.cpp`; `__weavec_rt_globals_register` in `runtime/weavec_guard.c` (RFC 0035 §4); tests `test/Guards/ir-globals.c`, `global-overflow.c` |
| Change unsafe regions                | `lib/Frontend/UnsafeRegions.cpp` (the AST collector; `no_sanitize("address")` functions too; RFC 0035 §2.6, amendment 8) |
| Change the enforcement ledger or the summary line | `lib/Frontend/EnforcementLedger.cpp` (`weavec-ledger` version 3, `EnforcementLedgerVersion`; RFC 0035 §9, amendment 10); test `test/Guards/ledger.c` |
| Change the runtime (allocator, shadow, guards' slow paths, reports) | `runtime/weavec_alloc.c` (arena, quarantine, the heap's shadow), `weavec_malloc.c` (the `malloc` family), `weavec_guard.c` (slow paths, ranges, strings, stack and globals), `weavec_libc.c` (checked library calls, `mmap`), `weavec_owner.c` (one runtime per process on Darwin), `weavec_report.c` (report formats, `WEAVEC_RT_*` variables), `weavec_rt.h`; its C test `runtime/test/rt_test.c` (CTest `runtime`); cases in `test/cases/semantics/runtime/` |
| Change the runtime's link line and fallbacks | `lib/Frontend/Driver.cpp` (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`); `test/Driver/runtime-*.c` |
| Change `weavec-cc` (driver, cc1 wrapping, flags) | `lib/Frontend/Driver.cpp` (`DriverOptions::consume`, `driverFlagsHelp`, `runCc1Job`), `include/weavec/Frontend/Driver.h`, `tools/weavec-cc/main.cpp` |
| Change the analysis under `-fweavec-diagnose` | `analysisOptions` in `lib/Frontend/Driver.cpp`; `lib/Frontend/FrontendAction.cpp` (the analysis action shared with `weavec`) |
| Change `-W` handling                 | `lib/Frontend/DiagnosticControl.cpp`                  |
| Add a checker rule                   | After an RFC, behind the seam: the object engine (`lib/Analysis/Engine*.cpp`, `ObjectEngine`; decisions in `EngineDecide.cpp`) publishes only through `LedgerAdapter` (`include/weavec/Analysis/LedgerAdapter.h`, `SafetyEngine.h`) |
| Change the abstract domain (objects, cells, symbols, zone, joins) | `include/weavec/Core/{Heap,Zone,Persistent}.h`, `lib/Core/Heap.cpp`, `lib/Core/Zone.cpp` (unit tests in `unittests/Core/HeapTest.cpp`) |
| Change analysis outcomes, reasons, the analysis summary line | `lib/Core/Ledger.cpp`, `lib/Analysis/LedgerAdapter.cpp` (defaults, rollup), `lib/Frontend/AnalysisSummary.cpp` |
| Change which sites exist             | `lib/Analysis/SiteCollector.cpp`                       |
| Change pointer kinds / how annotations become kinds | `lib/Core/PointerKind.cpp`, `lib/Analysis/AttributeReader.cpp`, `lib/Analysis/KindInference*.cpp`; what the engine takes from them: `lib/Analysis/EngineKinds.cpp` |
| Evaluate an expression / map an lvalue to an address | `lib/Analysis/EngineExpr.cpp` (`Transfer::evaluate`, `Transfer::addressOf`) |
| Model a C library function for the analysis | `lib/Core/LibrarySpec.txt`; how the engine applies a row: `lib/Analysis/EngineCalls.cpp` (effects), `lib/Analysis/EngineLibrary.cpp` (argument requirements) |
| Change function-pointer slots        | `lib/Core/FnSlots.cpp`, `lib/Analysis/SlotCollector.cpp` |
| Change how a callee's summary is found | `lib/Analysis/EngineCalls.cpp` (`CallApplier::applyDirect`), `UnitRun::summaryOf` in `lib/Analysis/EngineUnit.cpp` (RFC 0031 §5.4) |
| Change how a summary is derived or applied | `lib/Analysis/EngineSummary.cpp` (RFC 0031 §6), `include/weavec/Core/Effects.h` |
| Change the TU driver / call graph    | `lib/Analysis/EngineUnit.cpp` (`UnitRun`), `lib/Analysis/UnitPipeline.cpp` |
| Change what a unit exports / the program database | `UnitRun::exports` in `lib/Analysis/EngineUnit.cpp`, `lib/Analysis/ProgramDatabase.cpp` (RFC 0005, RFC 0031 §7) |
| Change how summaries join, widen or renumber their globals | `lib/Core/Effects.cpp` (`joinEffects`, `widenEffects`, `renumberGlobals`; tests in `unittests/Core/EffectsTest.cpp`) |
| Change `weavec --whole-program`      | `lib/Frontend/ProgramAnalysis.cpp` (the units as one program, in memory), `lib/Frontend/ProgramChecks.cpp` (declarations against definitions), `lib/Frontend/InterfaceFacts.cpp`, `tools/weavec/main.cpp` (RFC 0035 §8) |
| Debug what a guarded program did     | the report line names the kind, the access and the address; `-fweavec-checks=report` prints each failing site once and goes on; `WEAVEC_RT_STATS=1 ./prog` prints the runtime's counters at exit; `-fweavec-ledger=out/ -fweavec-summary` shows which accesses kept their guard |
| Debug the passes                     | `weavec-cc -O2 -S -emit-llvm file.c -o -` (the expanded guards); `-mllvm -print-after-all` (the markers through the pipeline; the passes print as `weavec::frontend::GuardInsertPass` and so on); `-fweavec-checks=verify` (removed guards kept as monitors) |
| Debug what the analysis inferred     | `weavec --dump-analysis file.c --`; `weavec --dump-kinds file.c --`; `weavec --whole-program --dump-analysis a.c b.c --`; `WEAVEC_ENGINE_DUMP=1` prints every summary on stderr, `=2` adds each run's exit states, `=3` each block's entry state instead (`lib/Analysis/EngineUnit.cpp`, `EngineSummary.cpp`, `EngineRun.cpp`) |
| Add or run a test case               | `test/cases/README.md`, `scripts/run-cases.py`         |
| Measure on real code                 | `scripts/corpus-gate.py`, `test/corpus/` (README, manifest, expected ratchet, triage) |
| Change how diagnostics are rendered  | `lib/Frontend/ClangDiagnosticSink.cpp`                 |
| Add a CLI flag                       | `tools/weavec/main.cpp`, `FrontendOptions`; driver flags in `Driver.h` and `Driver.cpp`; document them in `docs/pages/reference/cli.md` |
| Add an annotation                    | `Annotations.h`, `weavec.h`, `docs/annotations.md`     |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
