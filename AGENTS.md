# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based drop-in C compiler that enforces memory safety
at run time. `weavec-cc` guards every memory access of the C it compiles:
LLVM passes (`lib/Frontend/Guard*.cpp`) put a guard before every load,
store, atomic operation, memory intrinsic and library-call memory argument,
remove a guard only where a local rule proves it redundant, lay out stack
frames (locals whose lifetimes never overlap share space) and globals with
redzones, check fortified calls against their object size, and expand each
remaining guard into an inline check of the runtime's shadow memory; the
program traps at the first bad access with a report. `runtime/` is the C
runtime every enforcing link carries: the shadow, the arena allocator whose
quarantine reuses a released block only after a *sweep* finds no pointer to
it, the guards' slow paths, the checked library wrappers, ASan's interface
and the reports (`libweavec_rt.a`), and the `malloc` family over the arena
(`libweavec_alloc.a`). Two C++ libraries: `weavec::Core` (only the library
table, `LibrarySpec`; **no Clang/LLVM includes allowed**) and
`weavec::Frontend` (the guard passes, unsafe regions, the enforcement
ledger, the compiler driver). `tools/weavec-cc` is Clang's driver with
WeaveC inside; there is no other tool and no static analysis. Full
picture: `docs/architecture.md`. Design and the reasoning behind it:
`docs/rfcs/`. Read `0036-deterministic-heap-reuse.md`, the current design
(sweep-before-reuse, shared frame space, fortified sizes, ASan's contract;
it deleted the advisory ownership analysis, and its *Alternatives* record
why provenance bounds and frame blocks were measured and left out), and
then `0035-guard-by-default.md` for what RFC 0036 keeps: the guards, the
shadow, frames and globals, the library-call guards, the removal rules, the
modes and the ledger. Read each one's *Implementation amendments* too; they
override its body. RFCs 0001–0034 record the analysis and the enforcement
designs that came before.

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
- Corpus gate: `scripts/corpus-gate.py --quick` (see `test/corpus/README.md`).
- Documentation examples: `python3 docs/scripts/check-examples.py
  --weavec-cc build/dev/bin/weavec-cc`; the site: `npm ci && npm test &&
  npm run build` in `docs/`.
- Hygiene gate: `scripts/check-hygiene.py`.
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
   RFC 0036 gate H1): library behaviour goes in `lib/Core/LibrarySpec.txt`
   (the guards read it through `GuardLibrary.cpp`), never in
   `name == "…"` tests; nothing under `lib/` names a corpus project; the
   library code (`lib/`, `include/`, `tools/`) stays within its line budget
   (9,000), and so does the runtime (`runtime/` without `runtime/test/`,
   counted apart: 6,000). The runtime is C and includes no WeaveC header
   but its own `weavec_rt.h`, and every runtime entry point the passes
   declare (and every `wrapper(NAME)` of the table) must be declared there
   with the same signature. The runtime's `preserve_all` entry points
   (`WEAVEC_RT_SLOW`) stay trivial wrappers that call non-inlined helpers:
   Apple clang 21 miscompiles a `preserve_all` function that uses many
   callee-saved registers (it saves some below the stack pointer).

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Propose a design change              | `docs/rfcs/README.md`, `docs/rfcs/0000-template.md`    |
| Change what weavec-cc adds to a compile (zero-init, array bounds, locations, the passes) | `configureEnforcement` in `lib/Frontend/Driver.cpp`; `registerGuardPasses` in `lib/Frontend/GuardPass.cpp` (RFC 0035 §1, amendment 1; RFC 0036 §1) |
| Change fortification or ASan's contract (`_FORTIFY_SOURCE=3`, `__SANITIZE_ADDRESS__`, `-fno-weavec-asan-compat`) | `wantsFortification`, `applyAsanCompat` and `AsanOptionsReset` in `lib/Frontend/Driver.cpp` (RFC 0036 §1, §5); tests `test/Driver/fortify.c`, `asan-compat.c`, `asan-compat-fortify.c` |
| Change which accesses get a guard    | `lib/Frontend/GuardInsert.cpp` (`GuardInsert`, at `PipelineStartEP`: the markers `__weavec.guard`, `.range`, `.strlen`, `.disjoint`, `.scope`; RFC 0035 §2.1, amendments 1–2) |
| Change how a library call is guarded, fortified limits included | `lib/Frontend/GuardLibrary.cpp` (row-driven guards, `wrapper(…)` redirection, `chk` object sizes and `limit` arguments, `wide` rows, `%s`/`%n` arguments); the rows and their clauses in `lib/Core/LibrarySpec.txt` (one unit test per row in `unittests/Core/LibrarySpecTest.cpp`); the wrappers, their limits and the number parsers in `runtime/weavec_libc.c` (RFC 0035 §2.5, amendment 7; RFC 0036 §6); tests `test/Guards/checked-calls.c`, `intra-object.c` |
| Change the rules that remove guards  | `GuardPrune` (`PeepholeEP`; `pruneGuards` in `lib/Frontend/GuardInsert.cpp`: rule 6.1 after inlining) and `GuardExpand` (`OptimizerLastEP`; `lib/Frontend/GuardExpand.cpp`: rules 6.1–6.3); the passes' entry points are in `GuardPass.cpp`; `lib/Frontend/GuardLoops.cpp` (`GuardLoop`, `VectorizerStartEP`: loop versioning, rule 6.4); each removal must stay monitored by `verify` mode |
| Change the inline checks (fast paths, null bases) | `lib/Frontend/GuardPass.cpp`, `GuardPassImpl.h`; the shadow encoding in `runtime/weavec_rt.h` (RFC 0035 §2.2–§2.4, amendments 3–4); tests `test/Guards/ir-inline-check.c`, `null-base.c` |
| Change stack frames, shared frame space, scopes, dynamic allocas, non-local exits | `lib/Frontend/GuardFrames.cpp` (RFC 0035 §3, amendment 5; RFC 0036 §2: `StackLifetime`); tests `test/Guards/ir-frames.c`, `shared-frame.c`, `stack-overflow.c`, `use-after-scope*.c`, `vla-overflow.c`, `alloca-entry.c`, `longjmp.c`, `ir-noreturn.c`; case `test/cases/determinism/shared-frame_ok.c` |
| Change global redzones and registration | `lib/Frontend/GuardGlobals.cpp`; `__weavec_rt_globals_register` in `runtime/weavec_guard.c`, which a shared image's copy of the runtime forwards to the process's owner (RFC 0035 §4, RFC 0036 §4); tests `test/Guards/ir-globals.c`, `global-overflow.c`, `test/Driver/image-globals.c` |
| Change unsafe regions                | `lib/Frontend/UnsafeRegions.cpp` (the AST collector; `no_sanitize("address")` functions too; RFC 0035 §2.6, amendment 8) |
| Change the enforcement ledger or the summary line | `lib/Frontend/EnforcementLedger.cpp` (`weavec-ledger` version 3, `EnforcementLedgerVersion`; RFC 0035 §9, amendment 10); test `test/Guards/ledger.c` |
| Change the allocator, the quarantine or when to sweep | `runtime/weavec_alloc.c` (arena, slot words, quarantine, huge blocks, the heap's shadow; the soft and hard budgets and the pacing, `overBudget`; recycling in address order; RFC 0036 §3), `weavec_malloc.c` (the `malloc` family); `rt_test` tests `pacing`, `sweep-*` |
| Change the sweep                     | `runtime/weavec_sweep.c` (stopping threads: Darwin `thread_suspend`, Linux signals; the roots: stacks, registers, writable mappings with dirty pages, the image table for the dyld shared cache; the conservative mark; RFC 0036 §3.4); `rt_test` tests `sweep-*`, `library-data-is-scanned` |
| Change ASan's interface              | `runtime/weavec_asan.c` (RFC 0036 §5); test `test/Guards/use-after-poison.c`, case `test/cases/determinism/asan-pool_*.c` |
| Change the rest of the runtime (slow paths, reports) | `runtime/weavec_guard.c` (slow paths, ranges, strings, frames' helpers, globals, fortified-size failures), `weavec_owner.c` (one runtime per process on Darwin, the dispatch table, version 4), `weavec_report.c` (report formats, `WEAVEC_RT_*` variables), `weavec_rt.h`; its C test `runtime/test/rt_test.c` (CTest `runtime`); cases in `test/cases/semantics/runtime/` |
| Change the runtime's link line and fallbacks | `lib/Frontend/Driver.cpp` (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`); `test/Driver/runtime-*.c` |
| Change `weavec-cc` (driver, cc1 wrapping, flags) | `lib/Frontend/Driver.cpp` (`DriverOptions::consume`, `driverFlagsHelp`, `runCc1Job`), `include/weavec/Frontend/Driver.h`, `tools/weavec-cc/main.cpp`; document flags in `docs/pages/reference/cli.md` |
| Debug what a guarded program did     | the report line names the kind, the access and the address, and, for the heap, a second line the block; `WEAVEC_RT_BACKTRACE=1` prints the stack of the stopping access; `-fweavec-checks=report` prints each failing site once and goes on; `WEAVEC_RT_STATS=1 ./prog` prints the runtime's counters (sweeps and their pauses included) at exit; `-fweavec-ledger=out/ -fweavec-summary` shows which accesses kept their guard |
| Debug the passes                     | `weavec-cc -O2 -S -emit-llvm file.c -o -` (the expanded guards and the frame layout); `-mllvm -print-after-all` (the markers through the pipeline; the passes print as `weavec::frontend::GuardInsertPass` and so on); `-fweavec-checks=verify` (removed guards kept as monitors) |
| Add or run a test case               | `test/cases/README.md`, `scripts/run-cases.py`         |
| Measure on real code                 | `scripts/corpus-gate.py`, `test/corpus/` (README, manifest, expected ratchet, triage) |
| Add an annotation                    | Don't, without an RFC: `WEAVEC_UNSAFE` is the only one (`UnsafeRegions.h`, `weavec.h`, `docs/annotations.md`) |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
