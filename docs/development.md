# Developer guide

## Toolchain

| Requirement        | Version                    | Notes                                                     |
| ------------------ | -------------------------- | --------------------------------------------------------- |
| CMake              | ≥ 3.24 (3.25 for presets)  |                                                           |
| Ninja              | any                        | Presets use Ninja; other generators work with `-G`.       |
| C++ compiler       | C++20                      | Clang ≥ 17, GCC ≥ 12, Apple Clang ≥ 15.                   |
| C compiler (runtime) | Clang                    | The runtime's slow paths use Clang's `preserve_all` convention, which the guard passes call them with. |
| LLVM + Clang (dev) | 23.x recommended, ≥ 20     | Must include CMake packages (`LLVMConfig`, `ClangConfig`). |
| lit                | matching LLVM              | `pip install lit==23.1.0` or `brew install lit`.          |
| FileCheck/not/count| from LLVM                  | Shipped by Homebrew and apt.llvm.org's `llvm-N-tools`.    |
| clang-format/tidy  | same major as CI (23)      | Only for linting.                                         |

### macOS

```sh
brew install llvm ninja lit ccache
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"
```

The build also auto-detects the Homebrew `llvm` keg if `WEAVEC_LLVM_PREFIX`
is not set. Apple's system Clang can compile WeaveC, but the LLVM/Clang
*libraries* must come from Homebrew (Xcode does not ship them).

### Ubuntu / Debian

```sh
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build ccache
pip install lit==23.1.0
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23
export CC=clang-23 CXX=clang++-23
```

### Other LLVM installs

Point `CMAKE_PREFIX_PATH` (or `LLVM_DIR` and `Clang_DIR`) at any LLVM build
that installed its CMake packages, e.g. a from-source build with
`-DLLVM_ENABLE_PROJECTS=clang -DLLVM_INSTALL_UTILS=ON`.

## Building

```sh
cmake --preset dev             # Debug, assertions, compile_commands.json
cmake --build --preset dev
ctest --preset dev             # unit tests, the runtime test, lit, the cases
```

| Preset           | Purpose                                              |
| ---------------- | ---------------------------------------------------- |
| `dev`            | Debug build for day-to-day work                      |
| `dev-asan`       | Debug + AddressSanitizer + UBSan                     |
| `dev-tidy`       | Debug with clang-tidy running as part of compilation |
| `release`        | Optimised, LTO (the runtime archives excepted)       |
| `relwithdebinfo` | Optimised with debug info                            |
| `ci-*`           | What CI runs: warnings are errors                    |

Useful cache variables (`-D...` or in `CMakeUserPresets.json`):

| Variable                   | Default | Effect                                              |
| -------------------------- | ------- | --------------------------------------------------- |
| `WEAVEC_BUILD_TESTS`       | ON      | Build unit and lit tests                            |
| `WEAVEC_WARNINGS_AS_ERRORS`| OFF     | `-Werror`                                           |
| `WEAVEC_SANITIZERS`        | ""      | `address;undefined`, `thread`, `memory`, `leak`     |
| `WEAVEC_ENABLE_LTO`        | OFF     | IPO for Release/RelWithDebInfo                      |
| `WEAVEC_ENABLE_CCACHE`     | ON      | Use ccache/sccache when found                       |
| `WEAVEC_ENABLE_CLANG_TIDY` | OFF     | Run clang-tidy during the build                     |
| `WEAVEC_LLVM_MIN_VERSION`  | 20.0    | Minimum accepted LLVM                               |
| `WEAVEC_CASES_ARGS`        | ""      | Extra `scripts/run-cases.py` options for the `cases-*` tests (`--no-run` in the ASan job) |

Build targets of note:

- `weavec` — the advisory analysis tool, in `build/<preset>/bin/`.
- `weavec-cc` — the drop-in compiler driver, next to it. It needs Clang's
  resource directory and (for jobs it does not run in-process, such as
  `-cc1as`) a `clang` binary; both are recorded at configure time from the
  LLVM install used to build, and `WEAVEC_CLANG` overrides the binary. It
  finds `weavec.h` and the runtime archives in WeaveC's own resource
  directory, `../lib/weavec/` from the executable (the build tree has the
  same layout), or where `WEAVEC_RESOURCE_DIR` points.
- `weavec_rt`, `weavec_alloc` — the runtime archives (`libweavec_rt.a`,
  `libweavec_alloc.a`), in `build/<preset>/lib/weavec/`; `weavec_rt_test`
  is the runtime's own test.
- `check-weavec` — build and run all tests.
- `check-weavec-unit`, `check-weavec-lit` — only one suite.

## Testing

### Unit tests (`unittests/`)

GoogleTest, one binary per library (`WeaveCCoreTests`, `WeaveCAnalysisTests`,
`WeaveCFrontendTests`). Core tests exercise the model directly, including
the object engine's domain (`HeapTest.cpp`, one test per invariant I1–I6 of
RFC 0031 §4.7), the join and widening of summaries (`EffectsTest.cpp`)
and one test per row of the library table (`LibrarySpecTest.cpp`).
Analysis tests parse snippets with
`clang::tooling::buildASTFromCodeWithArgs`, run `ObjectEngine` over them and
collect diagnostics with `core::DiagnosticCollector`; `TestUtils.h` has
`analyze` and `analyzeInProgram` (a snippet checked against another unit's
interface), and the result gives each function's `core::FunctionEffects`.
Frontend tests run the whole-program analysis over in-memory units, the
diagnostic controls, and the guard passes: `GuardPassTest.cpp` runs
GuardInsert, GuardPrune and GuardExpand on IR text, each removal rule of
RFC 0035 §6 alone, and checks the ledger rows they leave. Run one with
`build/dev/unittests/WeaveCCoreTests --gtest_filter='Heap*'`.

### The runtime (`runtime/`)

The runtime is plain C and is built with the host compiler into the build
tree's `lib/weavec` whatever the preset; `libweavec_rt.a` and
`libweavec_alloc.a` are always compiled with `-O2`. Its test is one program,
[`runtime/test/rt_test.c`](../runtime/test/rt_test.c), linked with both
archives so that its `malloc` is the arena's:

```sh
ctest --preset dev -R '^runtime$'                     # every test
build/dev/runtime/weavec_rt_test guards quarantine    # only the named tests
```

The tests are `shadow-ready`, `classes`, `shadow-encoding`,
`reuse-is-zero`, `alignment`, `realloc`, `quarantine`, `quarantine-holds`,
`invalid-releases`, `huge`, `foreign`, `guards`, `strings`,
`checked-calls`, `globals`, `allocas`, `unpoison-stack`, `mappings`,
`array-bounds`, `threads` and `fork`. A test that must stop the program (a
failed guard, an invalid release) runs in a child process, and the parent
checks how it died. The runtime has platform-specific halves (Darwin's
malloc zone and its one runtime per process, glibc's `__libc_free`, the
shadow window's size), so a change to it must pass on both Darwin and
Linux; CI runs the test in its Linux and macOS CTest jobs. The runtime
includes no WeaveC header but `runtime/weavec_rt.h`, and
`scripts/check-hygiene.py` holds it to its own line budget.

### Integration tests (`test/`)

lit + FileCheck; see [`test/README.md`](../test/README.md). Run a single test
with `lit -v build/dev/test/Guards/heap-overflow.c`. The suites:

| Directory | What it pins |
| --- | --- |
| `test/Guards/` | the guard passes and the runtime. `ir-*.c` run `%weavec_cc -O2 -S -emit-llvm` and FileCheck the IR: the inline checks, the frame and global layout, the guards a rule removes, the stack unpoisoning before a `noreturn` call. The others build and run a program and pin the exact report (`weavec: heap-buffer-overflow at …`), report mode, verify mode, the ledger and summary line, unsafe regions and `no_sanitize`, the checked library calls, array bounds, `longjmp`, `mmap` and zero-initialisation. |
| `test/Driver/` | `weavec-cc` and `weavec` on the command line: compile and link, the runtime on the link line and its fallback notes (`runtime-link.c`), one runtime per process on Darwin, `-fweavec-*` and `-W` flags, `-fweavec-diagnose`, versions. |
| `test/Analysis/` | the advisory analysis's diagnostics, through `%weavec`. |
| `test/Annotations/` | the `weavec.h` annotations and the pointer kinds they give (`--dump-kinds`). |
| `test/WholeProgram/` | several files through `%weavec --whole-program`, with shared sources in `test/WholeProgram/Inputs/`. |

Every diagnostic change and every change to a run-time report needs a lit
test, because these pin the exact user-visible output. New tests are named
by feature (`test/Guards/<feature>.c`, `test/Guards/ir-<feature>.c`);
existing `rfcNNNN-` names stay.

### Test cases (`test/cases/`)

Executable C cases organised by feature, with their expectations in
line-comment markers; [`test/cases/README.md`](../test/cases/README.md) has
the grammar. For each case `scripts/run-cases.py`:

1. analyses it with `weavec` (`--whole-program` when it has several units)
   and checks the diagnostics against `BUG: <id>` lines (`MISS: <reason>`
   on a `BUG` line marks one the analysis is known not to report),
   `CLEAN` and `ALLOW: <id>`;
2. builds it with `weavec-cc` (trap mode, or verify mode under
   `--checks verify`) and checks the enforcement ledger against
   `GUARDED`, `PROVEN`, `UNGUARDED` and `EXPECT-LEDGER` markers;
3. runs it once per `RUN-INPUT` and requires every stop to be one a
   `TRAP: <kind>` marker expects (a report kind such as
   `heap-buffer-overflow`, or `invalid-release`), and every `TRAP` to be hit;
   `TRAP-AT: <unit>:<line>` expects a stop in a unit several cases share.
   A `DETECT` case is judged instead by whether its bug stops at or before a
   `STOP` line and its fixed twin runs clean (`--min-stops N` gates the
   count).

`--asan` adds the ASan oracle (each case built with
`-fsanitize=address,array-bounds` by the reference compiler); `--checks
verify` fails any case that reports `weavec.proven`; `--no-run` analyses and
builds only; `--filter GLOB` selects cases. CTest registers one
`cases-<suite>` test per top-level directory, so `ctest --preset dev -L
cases` runs them in parallel; the cache variable `WEAVEC_CASES_ARGS` passes
runner options (the ASan CI job uses `--no-run`). A false trap or false
positive fixed gets a `CLEAN` case; a new guard rule gets a bug case and its
correct twin under `test/cases/semantics/<feature>/`.

### Corpus gate (`test/corpus/`)

`scripts/corpus-gate.py` builds real C projects pinned by SHA in
`test/corpus/manifest.json` with `weavec-cc`, runs their test suites and
benchmarks, and compares the results with the ratchet in
`test/corpus/expected.json`; anything that needs a verdict (a trap in a
project's tests, an analysis finding) is recorded in
`test/corpus/triage.json`. `--quick` runs a few configurations on every
pull request, and the weekly **Corpus** workflow runs the full gate. See
[`test/corpus/README.md`](../test/corpus/README.md) for the configurations,
the evaluation sets and the options. Timing is sensitive to load: run the
benchmarks on an idle machine.

### Hygiene

`scripts/check-hygiene.py` checks the repository rules (no library names
compared outside `lib/Core/LibrarySpec.txt`, no corpus project named under
`lib/`, the engine seam) and the line budgets of the object engine, of the
libraries (`lib/`, `include/` and `tools/`) and of the runtime (counted
apart, because it is linked into compiled programs, not into the
compiler); `--help` lists each check and its limit. It runs in the Linux
Release CI job.

### Sanitizers

`cmake --workflow --preset ci-debug` builds with ASan+UBSan and runs
everything. This CI preset uses `-O1 -gline-tables-only` to keep the test
suites within the job's runtime budget while retaining assertions,
both sanitizers and source locations in stack traces. The `dev-asan` preset
keeps the unoptimized Debug build for interactive debugging. The analysis
and the guard passes are the most likely places for lifetime bugs of our
own, so run the CI preset before submitting changes to them.

## Formatting and linting

- `scripts/format.sh` formats C++ (clang-format) and CMake (cmake-format).
- `scripts/check-format.sh` / `scripts/check-cmake-format.sh` verify (CI runs
  these).
- `scripts/run-clang-tidy.sh [build-dir]` runs clang-tidy over the compile
  database; `cmake --preset dev-tidy` runs it as part of compilation.
- `pre-commit install` wires all of the above into `git commit`.

The `.clangd` config points at `build/compile_commands.json`; symlink your
preset's database there (`ln -s build/dev/compile_commands.json build/`) for
editor integration.

## Debugging guards and the runtime

- `weavec-cc -O2 -S -emit-llvm file.c -o -` prints the unit's IR after the
  guard passes: each surviving guard is an inline shadow check whose slow
  side calls `__weavec_rt_guard`, `__weavec_rt_range` or another
  `__weavec_rt_*` entry point, and tracked locals live in one frame alloca
  with redzones. Compare with `-fweavec-checks=none` to see the guards'
  whole effect.
- `-mllvm -print-after-all` (narrowed with `-mllvm
  -filter-print-funcs=<function>`) prints the IR after every pass, so the
  markers (`__weavec.guard`, `__weavec.range`, `__weavec.strlen`,
  `__weavec.disjoint`, `__weavec.scope`) can be followed from
  `weavec::frontend::GuardInsertPass` through the optimiser,
  `GuardPrunePass` and `GuardLoopPass` to `GuardExpandPass`, which turns
  them into checks. `-mllvm -print-after=<pass>` takes the name of one of
  LLVM's own passes (`sroa`, `licm`); the guard passes are not in LLVM's
  registry, so it cannot name them.
- `-fweavec-checks=verify` keeps every guard a local rule removed as a
  monitor: a failure reports `weavec: weavec.proven: <kind> at …`, which
  means a removal rule was wrong. It also turns off loop versioning.
- `-fweavec-ledger=<dir>/` writes `<object>.ledger.json` per unit: each
  access the passes collected, with its location, operation, width and
  outcome (`guarded`, `proven` or `unguarded`, with a reason).
  `-fweavec-summary` prints the unit's summary line
  (`weavec: file.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded`).
- `-fweavec-checks=report` prints each failure once per site and lets the
  program go on, so one run shows every failing site; `WEAVEC_RT_ABORT=1`
  makes it trap at the first again. `WEAVEC_RT_REPORT_LOG=<path>` appends
  each report to a file instead of standard error, for a test harness that
  keeps a passing test's output to itself.
- `WEAVEC_RT_STATS=1 ./prog` prints the runtime's counters on stderr at
  exit, one line each, `weavec: runtime: <n> <what>`: allocations,
  releases, recycled slots, huge blocks, slow guards, range guards, string
  guards and stack unpoisons. The counters are not synchronised.
  `WEAVEC_RT_QUARANTINE=<bytes>` sets the quarantine budget (0 recycles a
  released block at once).
- `weavec-cc` runs its `-cc1` jobs in-process, so `lldb -- build/dev/bin/
  weavec-cc -c file.c` stops in the passes directly; `weavec-cc -###
  file.c` prints the jobs Clang's driver planned and the link line with the
  runtime archives. Run a trapping program under a debugger: it stops in
  the runtime, and the backtrace leads from there to the function that made
  the access.
- The ASan runs of `scripts/run-cases.py --asan` and of the corpus gate set
  `ASAN_SYMBOLIZER_PATH` to an `llvm-symbolizer` (from `WEAVEC_LLVM_PREFIX`,
  the `PATH` or Homebrew, or beside the reference compiler) unless the
  environment already has one. Without it the sanitizer runtime on macOS
  runs `atos` against the dying process, which waits for the system's
  permission to inspect it when a debugger prompt is pending, and every
  ASan run then hangs until its timeout.
- For lit failures, `lit -a` prints the full command and output; the test's
  working files are under `build/<preset>/test/<suite>/Output/`.

## Debugging the analysis

- `weavec file.c -- -Xclang -ast-dump` does *not* work (the tooling action
  replaces Clang's); use `clang -fsyntax-only -Xclang -ast-dump file.c`
  directly to inspect the AST.
- Run the tool under a debugger with `lldb -- build/dev/bin/weavec file.c --`.
- `weavec --dump-analysis file.c --` prints the inferred places, lifetimes,
  exit state and summary of every analysed function.
  `weavec --whole-program --dump-analysis a.c b.c --` names each unit in
  analysis order and then prints the joined program database (every
  exported summary) and the program's function-pointer slots.
- `weavec --dump-kinds file.c --` prints the unit's pointer kinds (declared
  and inferred, with must-access requirements, slot demotions, store groups
  and field candidates) and its function-pointer slots, without running the
  engine.
- The object engine prints its own state to stderr when
  `WEAVEC_ENGINE_DUMP` is set (`lib/Analysis/EngineUnit.cpp`,
  `EngineSummary.cpp`, `EngineRun.cpp`): any value prints the summary of
  every function the unit analyses and of every summary it imports from the
  program database; `2` also prints each run's exit states, and `3` each
  run's block entry states (objects, cells, symbols and the zone), as
  `ObjectEngine::dump` does. `WEAVEC_ENGINE_TRACE` prints every block visit
  of the fixpoint with the states it sends and the joins it makes. Both are
  unstable and verbose; use them on a reduced case.
- `weavec --analysis-stats=<path>` (`-fweavec-analysis-stats=<path>` in
  `weavec-cc -fweavec-diagnose`) writes the analysis's work statistics as
  JSON, to find what spends a budget.

## Releasing

WeaveC publishes source releases to
[GitHub Releases](https://github.com/weavefoundry/weavec/releases).
`.github/workflows/ci.yml` calls the reusable `release.yml` workflow only
after `CI passed` succeeds on a push to `main` or a manual CI run on `main`.
PRs, merge-queue runs and forks do not publish. The release job skips a tested
commit if `main` has already advanced; the new commit must pass its own CI.

[Python Semantic Release](https://python-semantic-release.readthedocs.io/)
is pinned in `.github/release-requirements.txt` and configured in
`.releaserc.toml`. It is release tooling only, with no Python package or
registry credentials involved. The first feature release is `0.1.0`;
`feat` and breaking changes increment the minor while below 1.0, and
`fix`/`perf` increment the patch. Other commit types do not normally release.
Keep squash-merge titles in Conventional Commit form and describe user-visible
changes in the relevant guides and commit/PR descriptions. `CHANGELOG.md` is
created on the first release and regenerated solely by semantic-release;
do not edit it manually.

The workflow first prepares everything locally: it stamps
`project(... VERSION ...)` in `CMakeLists.txt`, regenerates `CHANGELOG.md` from
the Conventional Commit history, and creates a release commit and `vX.Y.Z`
tag. It packages that exact
tag using `git archive` and verifies the version, changelog and required
source files. Build directories and other untracked files are excluded.
The commit and tag are pushed atomically, so a concurrent update or rejected
push cannot publish just one of them. A GitHub draft receives the source
archive and `SHA256SUMS` before it is made public. Release notes link to the
versioned generated changelog and include its entry inline when it fits GitHub's
body limit.

Repository setup: Actions needs permission to write repository contents,
and the release identity needs to be allowed to push its release commit to
`main` and create `v*` tags under your branch/tag rules. The workflow uses
the built-in `GITHUB_TOKEN`; it cannot bypass repository rules that reject
that identity. Such rules must be configured by a repository administrator
before enabling automatic publication. No PyPI, Cargo or npm token is needed.
Keep `CI passed` as the required branch-protection check.

For a local preview, install the pinned tooling into a virtual environment
and run this on `main` with all tags fetched:

```sh
semantic-release -c .releaserc.toml --noop version --no-push --no-vcs-release
```

For a full rehearsal, use a disposable clone and omit `--noop`. This makes
local commits and tags but does not push or publish. Then run
`python scripts/package-source.py vX.Y.Z --output-dir dist`, extract the
archive, and follow the README's build/install instructions. Release builds
must configure with `-DWEAVEC_VERSION_SUFFIX=""` to omit the development
suffix. `cpack -G TGZ` still produces a local binary installation archive;
it is not the source archive published by this workflow.

If publication fails after the atomic push, manually run the **CI** workflow
on `main` while its tip is the release commit. It reruns validation, recognizes
the existing tag, reconstructs the archive, and finishes a missing or draft
GitHub Release. Already public releases are left unchanged. If `main` has
advanced, recover the existing release from a checkout of its tag using
`scripts/package-source.py` and the GitHub release UI/CLI; do not move a
published tag. Release commits pushed with `GITHUB_TOKEN` do not trigger
another CI run, preventing a release loop.
