# Notes for AI coding agents

Read this before making changes; it summarizes the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based drop-in C compiler that enforces memory safety
at run time. `weavec-cc` guards every memory access of the C it compiles:
LLVM passes (`lib/Frontend/Guard*.cpp`) put a guard before every load,
store, atomic operation, memory intrinsic and library-call memory argument,
remove a guard only where a local rule proves it redundant, lay out stack
frames (locals whose lifetimes never overlap share space) and globals with
redzones and colors, check fortified and field-bounded calls against their
sizes, and expand each remaining guard into an inline check of the runtime's
shadow memory (an access computed from a pointer also against the color of
the pointer's object); the program traps at the first bad access with a
report. `runtime/` is the C
runtime every enforcing link carries: the shadow, the arena allocator (with
per-thread caches) whose quarantine reuses a released block only after a
*sweep* (marked in parallel by worker threads) finds no pointer to it, the
guards' slow paths, the checked library wrappers, ASan's interface, the
fault handler, the reports and the `malloc` family over the arena, one shared
library (`libweavec_rt.so.<abi>`, `libweavec_rt.dylib`) that every image
`weavec-cc` links depends on, so a process has one runtime (and
`libweavec_rt.a` for `-static` Linux programs). Two C++ libraries: `weavec::Core` (only the library
table, `LibrarySpec`; **no Clang/LLVM includes allowed**) and
`weavec::Frontend` (the guard passes, unsafe regions, the enforcement
ledger, the compiler driver). `tools/weavec-cc` is Clang's driver with
WeaveC inside; there is no other tool and no static analysis. Full
picture: `docs/architecture.md`. Design and the reasoning behind it:
`docs/rfcs/`. Read `0038-drop-in-everywhere.md`, the current design (the
runtime as one shared library, searches checked over what they read, color
roots through the optimizer's arithmetic, the fault handler's re-raise, the
gates on every platform and the programs tier, self-contained toolchains),
then `0037-complete-sweep-and-colors.md` (the sweep's complete roots,
thread caches, parallel marking, the guards' holes closed, library
wrappers, fault reports, object colors, returned frames, field limits,
self-relative site records; its *Alternatives* record why cutting the
guards' cost was measured and not taken), then
`0036-deterministic-heap-reuse.md` for sweep-before-reuse,
shared frame space, fortified sizes and ASan's contract, and
`0035-guard-by-default.md` for the guards, the shadow, frames and globals,
the library-call guards, the removal rules, the modes and the ledger. Read
each one's *Implementation amendments* too; they override its body. RFCs
0001–0034 record the analysis and the enforcement designs that came
before.

## Before touching the passes or the runtime

Design decisions for `LibrarySpec.txt`, `weavec.h`, what is guarded and
which rules remove a guard, the shadow encoding, frames and globals, the
runtime (`runtime/`: the allocator, the quarantine and the sweep, ASan's
interface), the report formats, the enforcement ledger, and the driver's
flags are recorded as RFCs in `docs/rfcs/`. **Read the relevant RFC before
changing any of these**, and treat it as authoritative over comments in
the code. If the change you are about to make is not covered by an
Accepted RFC, or contradicts one, stop and write or amend an RFC first
(`docs/rfcs/README.md` explains when one is required and the process); do
not encode a new design decision in code alone. Bug fixes that bring code
in line with an RFC need no RFC. A rule that removes a guard must be local
to a function and monitored by `verify` mode (RFC 0035 §6).

## Build and test

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Unit tests: `build/dev/unittests/WeaveC{Core,Frontend}Tests`
  (`LibrarySpecTest` checks every row of the library table;
  `GuardPassTest` runs the passes on IR text).
- Runtime test: `ctest --preset dev -R '^runtime$'` (`runtime/test/rt_test.c`,
  built as `weavec_rt_test`; test names as arguments run only those).
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`). `test/Guards/` pins the passes' IR (`ir-*.c`, on
  `-emit-llvm`) and the run-time reports; `test/Driver/` the driver.
- Test cases: `scripts/run-cases.py [--filter 'determinism/**']` (see
  `test/cases/README.md`); CTest runs them as `cases-<suite>`. `--asan` adds
  the ASan oracle; `--checks verify` fails on any `weavec.proven` report.
- Corpus gate: `scripts/corpus-gate.py --quick` (see `test/corpus/README.md`);
  `--programs` builds the large programs of `test/corpus/programs.json`
  from their release archives.
- A self-contained toolchain: `cmake --preset release` (`WEAVEC_SELF_CONTAINED`),
  `cmake --install build/release --prefix <dir>`, then
  `scripts/check-toolchain.py <dir>` (or a `cpack` archive).
- Documentation examples: `python3 docs/scripts/check-examples.py
  --weavec-cc build/dev/bin/weavec-cc`; the site: `npm ci && npm test &&
  npm run build` in `docs/`.
- Hygiene gate: `scripts/check-hygiene.py`.
- Spelling: `scripts/check-spelling.py` (American English; `--fix`
  rewrites what it finds).
- Format: `scripts/format.sh`; check: `scripts/check-format.sh`.

## Rules that reviewers will enforce

1. Never add `clang/` or `llvm/` includes under `include/weavec/Core` or
   `lib/Core`.
2. WeaveC has no compile-time diagnostics of its own. A new run-time report
   kind gets a `test/Guards/` test that pins its exact report line, and an
   entry in the command-line reference's list of kinds
   (`docs/pages/reference/cli.md`); the case runner's list of kinds
   (`scripts/run-cases.py`) must know it too.
3. `WEAVEC_UNSAFE` is the only annotation. Its spelling
   (`annotate("weavec.unsafe")`) lives in exactly two places that must
   agree: `UnsafeAnnotation` in `include/weavec/Frontend/UnsafeRegions.h`
   and `resources/include/weavec.h`.
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
8. Changes to the library table, the guard passes, the runtime, the
   annotation or the report kinds reference the RFC that specifies them in
   the PR. New tests are named by feature (`test/cases/<area>/…`,
   `test/Guards/<feature>.c`); existing `rfcNNNN-` names may stay.
9. Keep the library table and the runtime clean (`scripts/check-hygiene.py`;
   RFC 0037 gate H1): library behavior goes in `lib/Core/LibrarySpec.txt`
   (the guards read it through `GuardLibrary.cpp`), never in
   `name == "…"` tests; nothing under `lib/` names a corpus project; the
   library code (`lib/`, `include/`, `tools/`) stays within its line budget
   (9,000), and so does the runtime (`runtime/` without `runtime/test/`
   and its export list, counted apart: 7,050). The runtime is C and
   includes no WeaveC header but its own `weavec_rt.h`, and every runtime
   entry point the passes declare (and every `wrapper(NAME)` of the table)
   must be declared there with the same signature. What the runtime's
   library exports (`WEAVEC_RT_API`) is listed in `runtime/weavec_rt.exports`,
   which the hygiene gate checks; raise `WEAVEC_RT_ABI` when an entry point,
   the shadow encoding or a site record changes. The runtime's `preserve_all` entry points
   (`WEAVEC_RT_SLOW`) stay trivial wrappers that call non-inlined helpers:
   Apple clang 21 miscompiles a `preserve_all` function that uses many
   callee-saved registers (it saves some below the stack pointer).
10. Write American English (behavior, initialize, color) in code, comments,
    documentation, RFCs and file names; `scripts/check-spelling.py` checks
    every tracked file in CI.

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a design change              | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Change what weavec-cc adds to a compile (zero-init, array bounds, locations, the passes) | `configureEnforcement` in `lib/Frontend/Driver.cpp`; `registerGuardPasses` in `lib/Frontend/GuardPass.cpp` (RFC 0035 §1, amendment 1; RFC 0036 §1) |
| Change fortification or ASan's contract (`_FORTIFY_SOURCE=3`, `__SANITIZE_ADDRESS__`, `-fno-weavec-asan-compat`) | `wantsFortification`, `applyAsanCompat` and `AsanOptionsReset` in `lib/Frontend/Driver.cpp` (RFC 0036 §1, §5); tests `test/Driver/fortify.c`, `asan-compat.c`, `asan-compat-fortify.c` |
| Change which accesses get a guard    | `lib/Frontend/GuardInsert.cpp` (`GuardInsert`, at `PipelineStartEP`: the markers `__weavec.guard`, `.range`, `.strlen`, `.disjoint`, `.scope`; RFC 0035 §2.1, amendments 1–2) |
| Change how a library call is guarded, fortified limits included | `lib/Frontend/GuardLibrary.cpp` (row-driven guards, `wrapper(…)` redirection, `chk` object sizes, field limits and `limit` arguments, `wide` rows scanned by `__weavec_rt_wcslen`, `%s`/`%n` arguments, thunks of library functions whose address is taken, `scan` arguments guarded after the call; RFC 0037 §5, §10; RFC 0038 §2); the rows and their clauses in `lib/Core/LibrarySpec.txt` (one unit test per row in `unittests/Core/LibrarySpecTest.cpp`); the wrappers, their limits and the number parsers in `runtime/weavec_libc.c` (RFC 0035 §2.5, amendment 7; RFC 0036 §6); tests `test/Guards/checked-calls.c`, `intra-object.c`, `library-scan.c`, cases `test/cases/semantics/library/scan-*.c` |
| Change the rules that remove guards  | `GuardPrune` (`PeepholeEP`; `pruneGuards` in `lib/Frontend/GuardInsert.cpp`: rule 6.1 after inlining) and `GuardExpand` (`OptimizerLastEP`; `lib/Frontend/GuardExpand.cpp`: rules 6.1–6.3); the passes' entry points are in `GuardPass.cpp`; `lib/Frontend/GuardLoops.cpp` (`GuardLoop`, `VectorizerStartEP`: loop versioning, rule 6.4); each removal must stay monitored by `verify` mode |
| Change the inline checks (fast paths, null bases) | `lib/Frontend/GuardPass.cpp`, `GuardPassImpl.h`; the shadow encoding in `runtime/weavec_rt.h` (RFC 0035 §2.2–§2.4, amendments 3–4; RFC 0037 §7); tests `test/Guards/ir-inline-check.c`, `null-base.c` |
| Change colors (who gets which, which accesses are checked, the slow path) | `colorRoot` and `loadRootShadows` in `lib/Frontend/GuardExpand.cpp` (roots through integer arithmetic, phis and selects: RFC 0038 §3), the frame colors in `GuardFrames.cpp`, the loop ranges' color test in `GuardLoops.cpp`; `weavecRtColorOfHeap` in `runtime/weavec_alloc.c`, `__weavec_rt_jump` and `__weavec_rt_color_ok` in `weavec_guard.c` (RFC 0037 §7–§8); tests `test/Guards/color-jump.c`, `color-roots.c`, cases `test/cases/semantics/colors/` |
| Change returned frames (use after return) | the exit shadow in `lib/Frontend/GuardFrames.cpp`, `settle` in `runtime/weavec_guard.c` (RFC 0037 §9); test `test/Guards/use-after-return.c` |
| Change stack frames, shared frame space, scopes, dynamic allocas, non-local exits | `lib/Frontend/GuardFrames.cpp` (RFC 0035 §3, amendment 5; RFC 0036 §2: `StackLifetime`); tests `test/Guards/ir-frames.c`, `shared-frame.c`, `stack-overflow.c`, `use-after-scope*.c`, `vla-overflow.c`, `alloca-entry.c`, `longjmp.c`, `ir-noreturn.c`; case `test/cases/determinism/shared-frame_ok.c` |
| Change global redzones and registration | `lib/Frontend/GuardGlobals.cpp`; `__weavec_rt_globals_register` in `runtime/weavec_guard.c` (RFC 0035 §4, RFC 0036 §4); tests `test/Guards/ir-globals.c`, `global-overflow.c`, `test/Driver/image-globals.c` |
| Change unsafe regions                | `lib/Frontend/UnsafeRegions.cpp` (the AST collector; `no_sanitize("address")` functions too; RFC 0035 §2.6, amendment 8) |
| Change the enforcement ledger or the summary line | `lib/Frontend/EnforcementLedger.cpp` (`weavec-ledger` version 3, `EnforcementLedgerVersion`; RFC 0035 §9, amendment 10); test `test/Guards/ledger.c` |
| Change the allocator, the quarantine or when to sweep | `runtime/weavec_alloc.c` (arena, slot words, quarantine, huge blocks, the heap's shadow; the soft and hard budgets and the pacing, `overBudget`; recycling in address order; RFC 0036 §3; thread caches, spare regions for full classes, the 64 MiB default, RFC 0037 §2), `weavec_malloc.c` (the `malloc` family); `rt_test` tests `pacing`, `sweep-*`, `thread-caches` |
| Change the sweep                     | `runtime/weavec_sweep.c` (stopping threads: Darwin `thread_suspend`, Linux signals; the roots: registers and vector registers, stacks, thread-specific data, readable mappings, the image table for the dyld shared cache; the conservative mark and its workers; RFC 0036 §3.4, RFC 0037 §1, §3); `rt_test` tests `sweep-*`, `*-is-scanned`, `vector-registers-are-scanned`, `signals-during-sweeps` |
| Change ASan's interface              | `runtime/weavec_asan.c` (RFC 0036 §5); test `test/Guards/use-after-poison.c`, case `test/cases/determinism/asan-pool_*.c` |
| Change the rest of the runtime (slow paths, reports) | `runtime/weavec_guard.c` (slow paths, ranges, strings, frames' helpers, globals, fortified-size failures), the fault handler in `weavec_report.c` (RFC 0037 §6, RFC 0038 §4), `weavec_report.c` (report formats, `WEAVEC_RT_*` variables, the check for a second runtime and a foreign allocator, RFC 0038 §1), `weavec_rt.h`; its C test `runtime/test/rt_test.c` (CTest `runtime`); cases in `test/cases/semantics/runtime/` |
| Change the runtime's library, its link line and fallbacks | `runtime/CMakeLists.txt` (the shared library, its export list `weavec_rt.exports`, the `-static` archive); `lib/Frontend/Driver.cpp` (`addRuntimeLibraries`, `-fweavec-rpath`, `runtimeObstacle`, `noteProgramAllocator`; RFC 0038 §1); `test/Driver/runtime-link*.c`, `shared-images.c` |
| Change `weavec-cc` (driver, cc1 wrapping, flags) | `lib/Frontend/Driver.cpp` (`DriverOptions::consume`, `driverFlagsHelp`, `runCc1Job`, `createAction`), `include/weavec/Frontend/Driver.h`, `tools/weavec-cc/main.cpp`; document flags in `docs/pages/reference/cli.md` |
| Change how a toolchain is built, laid out or released | `cmake/WeaveCLLVM.cmake` (`WEAVEC_SELF_CONTAINED`: static LLVM on Linux), `cmake/WeaveCBundleLLVM.cmake` (Darwin's bundled libraries), `cmake/WeaveCInstall.cmake` (the layout, `cpack`), `.github/workflows/release.yml`, `packaging/`, `scripts/check-toolchain.py`, `scripts/render-formula.py` (RFC 0038 §6) |
| Debug what a guarded program did     | the report line names the kind, the access and the address, and, for the heap, a second line the block; `WEAVEC_RT_BACKTRACE=1` prints the stack of the stopping access; `-fweavec-checks=report` prints each failing site once and goes on; `WEAVEC_RT_STATS=1 ./prog` prints the runtime's counters (sweeps and their pauses included) at exit; `-fweavec-ledger=out/ -fweavec-summary` shows which accesses kept their guard |
| Debug the passes                     | `weavec-cc -O2 -S -emit-llvm file.c -o -` (the expanded guards and the frame layout); `-mllvm -print-after-all` (the markers through the pipeline; the passes print as `weavec::frontend::GuardInsertPass` and so on); `-fweavec-checks=verify` (removed guards kept as monitors) |
| Add or run a test case               | `test/cases/README.md`, `scripts/run-cases.py`         |
| Measure on real code                 | `scripts/corpus-gate.py`, `test/corpus/` (README, manifest with per-platform forms, expected ratchet, triage, prerequisites, the programs tier) |
| Add an annotation                    | Don't, without an RFC: `WEAVEC_UNSAFE` is the only one (`UnsafeRegions.h`, `weavec.h`, `docs/annotations.md`) |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
