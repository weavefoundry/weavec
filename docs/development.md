# Developer guide

## Toolchain

| Requirement        | Version                    | Notes                                                     |
| ------------------ | -------------------------- | --------------------------------------------------------- |
| CMake              | ≥ 3.24 (3.25 for presets)  |                                                           |
| Ninja              | any                        | Presets use Ninja; other generators work with `-G`.       |
| C++ compiler       | C++20                      | Clang ≥ 17, GCC ≥ 12, Apple Clang ≥ 15.                   |
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

- `weavec` — the analysis tool, in `build/<preset>/bin/`.
- `weavec-cc` — the drop-in compiler driver, next to it. It needs Clang's
  resource directory and (for jobs it does not run in-process, such as
  `-cc1as`) a `clang` binary; both are recorded at configure time from the
  LLVM install used to build, and can be overridden with
  `WEAVEC_RESOURCE_DIR` and `WEAVEC_CLANG`.
- `weavec_rt`, `weavec_alloc`, `weavec_chk` — the runtime archives
  (`libweavec_rt.a`, `libweavec_alloc.a`, `libweavec_chk.a`), in
  `build/<preset>/lib/weavec/`; `weavec_rt_test` is the runtime's own test.
- `check-weavec` — build and run all tests.
- `check-weavec-unit`, `check-weavec-lit` — only one suite.

## Testing

### Unit tests (`unittests/`)

GoogleTest, one binary per library (`WeaveCCoreTests`, `WeaveCAnalysisTests`,
`WeaveCFrontendTests`). Core tests exercise the model directly, including
the object engine's domain (`HeapTest.cpp`, one test per invariant I1–I6 of
RFC 0031 §4.7) and the round trip of summary format 30
(`EffectsIOTest.cpp`); Analysis tests parse snippets with
`clang::tooling::buildASTFromCodeWithArgs`, run `ObjectEngine` over them and
collect diagnostics with `core::DiagnosticCollector` (`TestUtils.h` has
`analyze`, `analyzeInProgram` for a snippet checked against another unit's
exports, and `analyzeAtLink`, and the result gives each function's
`core::FunctionEffects`); Frontend tests run `ProgramAnalysis` and the link
step over in-memory units and round-trip format-30 unit records. The
sixth outcome `guarded` is covered in `LedgerTest.cpp` (rank, spellings,
the require level), `LedgerWriterTest.cpp` (the JSON's counts, reasons and
shares, the SARIF rules) and the planner's guards in
`CheckPlannerTest.cpp`. Run one
with `build/dev/unittests/WeaveCCoreTests --gtest_filter='Heap*'`.

### The runtime (`runtime/`)

The runtime is plain C and is built with the host compiler into the build
tree's `lib/weavec` whatever the preset; `libweavec_rt.a` and
`libweavec_alloc.a` are always compiled with `-O2`. Its test is one program,
[`runtime/test/rt_test.c`](../runtime/test/rt_test.c), linked with both
archives so that its `malloc` is the arena's:

```sh
ctest --preset dev -R '^runtime$'          # every test
build/dev/runtime/weavec_rt_test stack longjmp   # only the named tests
```

The tests are `classes`, `alignment`, `realloc`, `invalid-releases`, `huge`,
`foreign`, `stack`, `longjmp`, `deep-stack`, `globals`, `strings`,
`threads`, `fork`, `reuse-is-zero` and `quarantine`. A test that must stop
the program (an invalid release) runs in a child process, and the parent
checks how it died. The runtime has platform-specific halves (Darwin's
malloc zone, glibc's `__libc_free`, the two section-bounds spellings of the
global table), so a change to it must pass on both Darwin and Linux; CI runs
the test in its Linux and macOS CTest jobs (RFC 0032 gate R8). The runtime
includes no WeaveC header but `runtime/weavec_rt.h`, and
`scripts/check-hygiene.py` holds it to its own line budget.

### Integration tests (`test/`)

lit + FileCheck; see [`test/README.md`](../test/README.md). Run a single test
with `lit -v build/dev/test/Analysis/rfc0008-null.c`. Every diagnostic change
should be covered by a lit test because they pin the exact user-visible output.
`test/WholeProgram/` runs several files through `%weavec --whole-program`
(shared inputs in `test/WholeProgram/Inputs/`); `test/Driver/` drives
`%weavec_cc` through compile, link and flag handling; `test/Emission/` pins the
checks `weavec-cc` inserts against hand-written equivalents. New tests are
named by feature (`test/Emission/<feature>-*.c`); existing `rfcNNNN-` names
stay.

The rewrite oracle (`test/Emission/Inputs/rewrite-oracle.py`, the
`%rewrite_oracle` substitution) compiles a source with `weavec-cc` and a
hand-written expected file with the reference Clang and the printed prelude,
and requires equal `-O0` IR. It pins one rewrite at a time, so unless a
test's flags name `-fweavec-runtime` both sides are built with
`-fno-weavec-runtime`: no guard and no object registration. The tests of the
runtime's rewrites pass `-fweavec-runtime` and write the guards and
registrations into their expected file: `test/Emission/runtime-oracle-*.c`
(the `object` guard of a subscript and of a call argument, `live` and
`release`, the range cache, stack objects, global descriptors).
`test/Driver/runtime-flags.c` pins the `guarded` outcome in the summary
line and the ledger, `config.runtime` (`true`, `false`, `"mixed"`), the
require levels and `weavec --no-runtime`; `test/Driver/runtime-link.c` pins
the link line (`-u malloc`, `libweavec_alloc.a`, `libweavec_chk.a`,
`libweavec_rt.a`), the fallbacks for `-fno-weavec-runtime`,
`-fweavec-checks=none`, a sanitizer, `-nostdlib` and a program that defines
the allocator, and the `WEAVEC_RT_STATS` output.

### Test cases (`test/cases/`)

Executable C cases organised by feature, with their expectations in
line-comment markers (`// BUG: use-after-free`, `// TRAP: index`,
`// GUARDED: spatial`, `// UNRESOLVED: spatial:unknown-extent`, `// CLEAN`);
see
[`test/cases/README.md`](../test/cases/README.md). `scripts/run-cases.py`
builds each case with `weavec-cc`, checks its diagnostics and ledger, runs
it, and optionally runs it under ASan. CTest registers one `cases-<suite>`
test per top-level directory, so `ctest --preset dev -L cases` runs them in
parallel; the cache variable `WEAVEC_CASES_ARGS` passes runner options (the
ASan CI job uses `--no-run`). A false-positive fix gets a `// CLEAN` case; a
new rule gets a case under `test/cases/semantics/<feature>/`. The runtime's
cases are in `test/cases/semantics/runtime/`: a bug that must trap with
`object`, `live` or `release` and its correct twin, which must not.
`test/cases/semantics/dropin/` (RFC 0033 §11) holds one case per false stop
and silent miss found on projects WeaveC was not tuned on, with an `_ok`
twin for a false positive and a `_bug` twin for a miss. The runner builds
every multi-unit case with `-fweavec-link=analyze` (unless its flags name
`-fweavec-link=`), so cross-unit expectations keep testing the analysis.
`scripts/run-cases.py --asan` checks that no line ASan reports has its
facet proven, and `--checks verify` that no `weavec.proven` trap fires
(RFC 0032 gate R1).

### Corpus gate (`test/corpus/`)

`scripts/corpus-gate.py` builds and analyses real C projects pinned by SHA
in `test/corpus/manifest.json` and compares the results with the ratchet in
`test/corpus/expected.json`; every definite error and possible temporal
warning needs a verdict in `test/corpus/triage.json`. `--quick` runs on every
pull request and `--full` (project builds, test suites, injections,
benchmarks) weekly; see [`test/corpus/README.md`](../test/corpus/README.md).
Each config has a `set` (RFC 0033 §11): `original`, `heldOut` (RFC 0031),
`fresh` (eight projects WeaveC was not tuned on, run by `--full` and
`--held-out`) and `sealed` (five more, run only by `--sealed`, once, for gate
D2; `--reference-only` builds them with the reference compiler alone).
RFC 0032 changed three of its gates:

- `--bench` (G14) times three builds of each benchmark: the reference
  compiler's, the default `weavec-cc` build (with the runtime) and the
  `weavec-cc -fno-weavec-runtime` build. The manifest's `G14` limits the
  default build's user CPU (`maxOverhead`: Lua 6.0, zlib 2.0, cJSON 2.0),
  the build without the runtime (`maxOverheadNoRuntime`: 1.1, 1.1, 1.15) and
  the default build's peak resident size (`maxRssRatio`: 2.0 each), all over
  the reference compiler's. Timing is sensitive to load: run it on an idle
  machine.
- `rfc0032.R4` limits the unresolved share of each facet over the original
  configs together and over the held-out configs together: spatial 0.12,
  null 0.01, temporal 0.20.
- A guard that fails in a project's test suite is a trap, and fails the
  run, unless `triage.json`'s `guardFailures` records it as a true bug with
  its source evidence. The eight injections that only a guard can catch
  (gate R2) are `required`.

`scripts/check-hygiene.py` checks the repository rules of RFC 0030's gate H2
and RFC 0032's gate H1, and runs in the Linux Release CI job. Its line
budgets are: the libraries (`lib/`, `include/` and `tools/`) at most 69,000 lines
(`library-lines`, raised by RFC 0032), and the runtime (`runtime/` without
`runtime/test/`) at most 4,000 (`runtime-lines`), counted apart because it
is linked into compiled programs, not into the compiler.

### Sanitizers

`cmake --workflow --preset ci-debug` builds with ASan+UBSan and runs
everything. This CI preset uses `-O1 -gline-tables-only` to keep the test
suites within the job's runtime budget while retaining assertions,
both sanitizers and source locations in stack traces. The `dev-asan` preset
keeps the unoptimized Debug build for interactive debugging. Analysis code is
the most likely place for lifetime bugs of our own, so run the CI preset
before submitting analysis changes.

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

## Debugging the analysis

- `weavec file.c -- -Xclang -ast-dump` does *not* work (the tooling action
  replaces Clang's); use `clang -fsyntax-only -Xclang -ast-dump file.c`
  directly to inspect the AST.
- Run the tool under a debugger with `lldb -- build/dev/bin/weavec file.c --`.
- `weavec-cc` runs its `-cc1` jobs in-process, so `lldb -- build/dev/bin/
  weavec-cc -c file.c` stops in the analysis directly; `weavec-cc -###
  file.c` prints the jobs Clang's driver planned. A unit's record is
  `<object>.weavec` next to the object: format 31, a framed JSON header
  (producer, source, `-cc1` command, target, configuration, object digest)
  and payload (format-30 summaries, kinds, imports, slots, every site's
  ledger row), with a schema fingerprint and a SHA-256 digest (RFC 0030
  §13.1, RFC 0031 §7, RFC 0033 §7); `weavec --dump-record=<path>` prints it
  as JSON. A default link reads only the records; `-fweavec-link=analyze`
  re-runs the recorded command of each unit it analyses again.
- Write the ledger (`weavec --ledger=out.json file.c --`) and read the rows
  at the line in question: their facets, outcomes, reasons and fix-its say
  what was decided and why.
- `weavec --whole-program --dump-analysis a.c b.c --` names each unit in
  analysis order and then prints the joined program database (every
  exported summary) and the program's function-pointer slots.
- The object engine prints its own state to stderr when
  `WEAVEC_ENGINE_DUMP` is set (`lib/Analysis/EngineUnit.cpp`,
  `EngineSummary.cpp`, `EngineRun.cpp`): any value prints the summary of
  every function the unit analyses and of every summary it imports from the
  program database; `2` also prints each run's exit states, and `3` each
  run's block entry states (objects, cells, symbols and the zone), as
  `ObjectEngine::dump` does. `WEAVEC_ENGINE_TRACE` prints every block visit
  of the fixpoint with the states it sends and the joins it makes. Both are
  unstable and verbose; use them on a reduced case.
- `weavec --dump-kinds file.c --` prints the unit's RFC 0030 pointer kinds
  (declared and inferred, with must-access requirements, slot demotions,
  store groups and §7.6 candidates) and its function-pointer slots, without
  running the engine.
- To see what the runtime did in a compiled program, run it with
  `WEAVEC_RT_STATS=1`: at exit the runtime prints one line per counter on
  stderr, `weavec: runtime: <n> <what>` (allocations, releases, recycled
  slots, huge blocks, lookups and of those heap, stack, global and
  untracked, range requests, ranges kept, stack objects entered). The
  counters are not synchronised. `WEAVEC_RT_QUARANTINE=<bytes>` sets the
  quarantine budget (0 recycles a released block at once).
- To find which guard fails without stopping at the first, build with
  `-fweavec-checks=report`: each failed check or guard prints
  `weavec: runtime check failed: <template> at <file>:<line>:<column>` once
  per site and the program goes on; `WEAVEC_RT_ABORT=1` aborts at the first.
  A release the allocator itself rejects (no guard saw it) prints
  `weavec: invalid release of <pointer>: <why>` and traps.
  `WEAVEC_RT_REPORT_LOG=<path>` appends each report line to a file instead
  of standard error, which is how `scripts/corpus-gate.py` sees the
  failures of a test that a harness (CTest without `-V`) reports as passed
  and keeps quiet about, without changing what the test sees on stderr.
- The ASan runs of `scripts/run-cases.py --asan` and of the corpus gate set
  `ASAN_SYMBOLIZER_PATH` to an `llvm-symbolizer` (from `WEAVEC_LLVM_PREFIX`,
  the `PATH` or Homebrew, or beside the reference compiler) unless the
  environment already has one. Without it the sanitizer runtime on macOS
  runs `atos` against the dying process, which waits for the system's
  permission to inspect it when a debugger prompt is pending, and every
  ASan run then hangs until its timeout.
- `weavec-cc -fno-weavec-runtime` builds without guards, object registration
  and the allocator, which separates a problem in the runtime from one in
  the static checks; `-fno-weavec-stack-objects` and
  `-fno-weavec-global-objects` turn off one kind of registration.
- For lit failures, `lit -a` prints the full command and output; the test's
  working files are under `build/<preset>/test/<suite>/Output/`.

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
