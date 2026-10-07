# The corpus gate

WeaveC on real C projects at pinned revisions: RFC 0035, section 11 and the
*Acceptance gates*, run by
[`scripts/corpus-gate.py`](../../scripts/corpus-gate.py) (tests, which need
no compiler: [`scripts/test_corpus_gate.py`](../../scripts/test_corpus_gate.py)).

Since RFC 0035 every memory access of a `weavec-cc` build is guarded unless
a local rule proves the guard redundant, and the analysis is advisory. The
gate follows: it builds each project with `CC=weavec-cc` and runs its own
test suite, which must pass with no guard failing; it measures how many
accesses the build leaves guarded (the enforcement ledger), what the
`weavec` analysis reports, whether each injected bug stops at its line, and
what the guards cost at build and run time.

| File | What it holds |
| --- | --- |
| `manifest.json` | The 48 projects (url, 40-hex `sha`, `support` files) and their 50 configs: `compile` (files and arguments), `wholeProgram`, `build`, `test`, `testTimeout`, `bench`, `heldOut` and `set` (below); and `gates`, the limits of the gates below |
| `expected.json` | The ratchet, per platform and config, written by `--update` |
| `triage.json` | Verdicts on the analysis' findings (`entries`), and `guardFailures`: the guard failures of test suites that are true bugs of the project |
| `injections/` | `injections.json` and one patch per injected bug, by project (39), plus the drivers some of them build |
| `bench/` | The benchmark workloads and drivers, and `zlib-input.py`, the generator of the 64 MiB compression input |
| `support/` | Files a checkout needs that it does not have: generated configuration headers (`{support}` in compile arguments), test wrappers and data fetchers, build files |
| `identity.txt` | The units [`scripts/codegen-identity.py`](../../scripts/codegen-identity.py) compiles with `weavec-cc -fweavec-checks=none` and with the reference compiler, whose objects must be identical (RFC 0035, section 1) |

## The configs

| Config | Files | Whole program | Build | Test | Bench |
| --- | --- | --- | --- | --- | --- |
| sds | `sds.c` | | `make sds-test` | `./sds-test` | |
| cJSON | `cJSON.c` | | CMake with tests and utils | CTest (22 tests) | parse + print |
| cJSON-program | `cJSON.c cJSON_Utils.c` | yes | (the cJSON build) | | |
| jsmn | `example/simple.c example/jsondump.c` | | `make simple_example jsondump` | `make test` (4 builds of `test/tests.c`) | |
| log.c | `src/log.c` | | | | |
| printf | `printf.c` | | | | |
| linenoise | `linenoise.c` | | | | |
| linenoise-program | `linenoise.c example.c` | yes | `make linenoise-example` | `make test` (the pty harness, 102 checks, about 20 s) | |
| zlib | 15 library files | yes | `./configure && make -j8` | `make test` | minigzip on 64 MiB |
| lua | `l*.c` | yes | the developer makefile | 27 `testes` files (below) | `lua-bench.lua` |
| jansson | 12 `src/` files | yes | CMake | CTest (215 tests) | |

The projects without a build (log.c, printf) or a C test suite (printf's is
C++, which includes `printf.c` into a C++ unit) are compile-only: their
per-file compiles are their build; their injections build drivers.

The Lua test is `support/lua/subset.lua`, which sets up what `testes/all.lua`
sets up in user mode (`_U`: no `T` library, no long or non-portable tests)
and runs gc, db, calls, strings, literals, tpack, attrib, gengc, locals,
constructs, code, cstack, nextvar, pm, utf8, api, memerr, events, vararg,
closure, coroutine, goto, errors, math, sort, bitwise and verybig. It leaves
out `files.lua` (it needs `/dev/full`, which macOS lacks), `main.lua` (the
stand-alone interpreter tests, skipped in user mode anyway), `big.lua` and
`heavy.lua` (long), and the internal tests (they need a build with
`ltests.h`).

## The sets

A config without `heldOut` is `original`: a project WeaveC was developed
on. The others are projects nobody tuned WeaveC on, marked
`"heldOut": true`, and grouped by the RFC that chose them with `set`:

| Set | Configs | Chosen by |
| --- | --- | --- |
| `heldOut` | bzip2, hiredis, http-parser, inih, libyaml, lz4, miniz, mujs, sqlite, tinyexpr, utf8proc | RFC 0031, section 11.2 |
| `fresh` | zstd, libuv, oniguruma, redis, expat, pcre2, libevent, libsodium | RFC 0033, section 11 |
| `sealed` | libxml2, libpng, mbedtls, msgpack-c, yyjson | RFC 0033, section 11 |
| `fresh34` | quickjs, lmdb, janet, brotli, xz, libdeflate, zlib-ng, curl, cmark, libgit2 (each with a `bench`) | RFC 0034, section 9 |
| `sealed34` | libjpeg-turbo, opus, flac, giflib, wren | RFC 0034, section 9 |
| `fresh35`, `sealed35` | (none yet: RFC 0035's stage S0 adds them) | RFC 0035, section 11 |

Each config's `notes` say why its build and test are what they are.

- **Selection.** `--quick` runs the original configs; `--full` adds the
  held-out ones (`heldOut`, `fresh`, `fresh34`, `fresh35`); `--held-out`
  and `--no-held-out` say so explicitly. The sealed sets (`sealed`,
  `sealed34`, `sealed35`) run only when `--set` or `--only` names them: an
  RFC builds its sealed set once, at its end, and records what it measured
  (RFC 0035 G2); before that they may be built with `--reference-only`, to
  write their commands. `--set SET` runs one set alone; `--only` names
  configs and overrides both.
- **Gates.** Every selected config is gated alike; the summary at the end
  of a run groups them by set.

## Running it

Checkouts live in `build/corpus/<project>` (`--workdir`). A missing one is
cloned at its `sha`; one at another commit is an error unless `--fetch` is
given; one with modified tracked files is an error. Nothing is ever built in
a checkout: builds, injections and benchmarks work on copies under
`<workdir>/.gate/`, deleted afterwards unless `--keep`.

```sh
# Every PR (CI runs it on a few configs): the analysis and the ledgers.
scripts/corpus-gate.py --quick [--only cJSON sds] [--json out.json]
scripts/corpus-gate.py --quick --update          # record the ratchet

# Weekly: quick, builds and test suites, injections and benchmarks.
scripts/corpus-gate.py --full --json full.json
scripts/corpus-gate.py --full --checks verify    # G6: no weavec.proven report
scripts/corpus-gate.py --inject [--injection sds-uaf-sdsfree]
scripts/corpus-gate.py --bench [--repeat 3] [--no-asan]   # on an idle machine

# One set; a sealed set, once.
scripts/corpus-gate.py --full --set fresh35
scripts/corpus-gate.py --full --set sealed35

# The manifest's commands with the reference compiler alone, and each
# injection's run under ASan (does it reach the injected line?).
scripts/corpus-gate.py --full --reference-only --set sealed35
scripts/corpus-gate.py --inject --reference-only

# Record a results file written elsewhere (for example by CI).
scripts/corpus-gate.py --update-from corpus-gate-quick.json
```

The binaries default to `build/release/bin`, then `build/dev/bin` (a Debug
binary's timings mean nothing: `--bench` refuses one it found itself, and
`--full` warns). The reference compiler (`--cc`) defaults to
`$WEAVEC_LLVM_PREFIX/bin/clang`, else `clang`. `--jobs` bounds parallel
processes, `--timeout` each analysis or per-file compile (default 1,800 s),
`--build-timeout` each build or test step (a config's `testTimeout` replaces
it for its tests). The exit status is 0 on success, 1 when a check fails and
2 on a usage or setup error. `--json OUT` writes everything the run
measured, every diagnostic and report included.

## What each mode checks

- **`--quick`**, per config:
  - the advisory analysis (RFC 0035, section 8): `weavec <file> --
    -ferror-limit=0 <args>` for each file, or `weavec --whole-program
    <files> -- …` for a `wholeProgram` config, in the checkout. Its
    diagnostics go to the triage, its summary lines (`N sites: P proven, U
    not proven, V violations, T trusted; E errors, W warnings`) to the
    ratchet;
  - the enforcement ledgers (RFC 0035, section 9): `weavec-cc -c -O2
    -fweavec-checks=trap -fweavec-ledger=<file> <args> <file>` for each
    file (a config's own `-O` comes later and wins). A ledger must be a
    `weavec-ledger` document of version 3; its units' summaries (accesses,
    proven, guarded, unguarded) are summed per config and go to the ratchet.

  A crash, a timeout, a Clang error, a missing summary line or a missing
  ledger fails the run, and that config is not recorded by `--update`.
- **`--full`**: `--quick` (with each per-file compile also timed with the
  reference compiler, for G9), then for each config with a `build`: the
  build and test commands in a copy with `CC` set to a wrapper around
  `weavec-cc -fweavec-checks=trap`; the same in report mode
  (`-fweavec-checks=report`, which reports each failing guard once and goes
  on), which names every site a trap-mode run stops at only the first of;
  and the build once more with the reference compiler, not tested, for the
  build-time gate. Then the injections and the benchmarks. A failing build
  or test command fails the run. A failing guard is a report line
  (`weavec: <kind> at <file>:<line>:<col>: …`, RFC 0035 section 5.3, in the
  output or in the `WEAVEC_RT_REPORT_LOG` file the gate sets, so that a
  harness that hides a passing test's output does not hide it) or the
  allocator's `weavec: invalid release of …`; a death by `SIGTRAP` or
  `SIGILL` that no report explains counts too. Each distinct report of the
  trap and report runs is a trap and fails the run, except those listed in
  `guardFailures`.
- **`--inject`**: applies each injection's patch to a copy, builds it in
  trap mode (the config's `build`, or the entry's), runs its `run` commands
  (the config's `test` when the entry has none), and checks that the run
  stops at the injected line (below).
- **`--bench`**: builds each benchmark three ways, with the reference
  compiler, with `weavec-cc` (the default trap build) and with the reference
  compiler and `-fsanitize=address` (unless `--no-asan`), runs each once to
  warm up, then `repeat` (7) times interleaved, and takes the best user CPU
  time of each. The builds must print the same result, and `check` must
  pass; `input` makes a deterministic input. Run nothing else heavy on the
  machine meanwhile.
- **`--checks verify`**: the builds, tests and benchmarks in verify mode,
  where each guard a local rule removed is emitted as a monitor (RFC 0035,
  section 6); a `weavec.proven` report fails gate G6. The run-time gate is
  not evaluated.
- **`--reference-only`**: builds, tests and benchmarks with the reference
  compiler alone, to check that the manifest's commands work; with
  `--inject`, builds each injection with ASan and UBSan (with
  `_FORTIFY_SOURCE` for `fortify` entries, short writes ASan cannot see)
  and checks that its run reaches the place the gate expects it to stop.
  Nothing is recorded.

## The gates

| Gate | RFC 0035 | Mode | Passes when |
| --- | --- | --- | --- |
| `triage` | (section 8) | `--quick` | every definite error of the analysis (an `error`) has a verdict in `triage.json`, and none is `"false"` |
| ratchet | (section 11) | `--quick` | no measurement is worse than `expected.json` records (below) |
| `drop-in` | G1, G2, G3 | `--full` | every build passes, and every test suite in trap (or verify) mode and in report mode, with no trap but at `guardFailures` |
| `injections` | (G4, G5 on the corpus) | `--inject`, `--full` | every selected injection stops at its line |
| `verify` | G6 | `--checks verify` | no `weavec.proven` report in any test suite or benchmark |
| `run-time` | G7 | `--bench`, `--full` | `gates.runTime`: each workload of `sets` (fresh34, fresh35) at most the larger of `maxOverhead` (2.0) and its ASan ratio, their geometric mean at most `maxGeometricMean` (1.8) once every one of them ran, and `maxOverheadPerConfig`'s workloads at their own limits (Lua 2.0, zlib 1.4, cJSON 1.4) |
| `build-time` | G9 | `--full` | `gates.buildTime`: each config's `weavec-cc` build CPU time at most `maxBuildRatio` (1.5) times the reference compiler's, and no per-file compile above `maxUnitRatio` (3.0) times the reference compiler's, among the files the reference compiler takes at least `minUnitCpuSeconds` (0.25) for |

`--json` holds each gate's status (`pass`, `fail`, or `skip` when nothing
was measured) and detail. G4 and G5 proper (the historic bugs and the blind
sets) and G8 (start-up) are measured by their own scripts, not here.

## The ratchet (`expected.json`)

`platforms.<platform>.configs.<config>` holds what `--quick` measured:

- `ledger`: `accesses`, `proven`, `guarded`, `unguarded` and `provenShare`
  (proven over accesses, to four places), summed over the per-file ledgers;
- `analysis`: `sites`, `proven`, `notProven`, `violations`, `trusted`,
  `errors`, `warnings` and `overBudget`, from the analysis' summary lines
  (the program line for a `wholeProgram` config).

`provenShare` must not drop and `unguarded`, `errors` and `warnings` must
not rise: a worse value fails the run. A better one, and any change of the
other counts, is reported for `--update` to record. A config or platform
with no record is reported, not failed, until `--update` records it.
Platforms are recorded separately (system headers change the numbers).
`--update` records the configs whose analyses and compiles all succeeded;
`--update-from RESULTS` records a `--json` file written elsewhere, for
example the `corpus-gate-quick-<os>` artifact of a CI run.

The record holds cJSON, cJSON-program, log.c and sds on `darwin-arm64`, measured
with `weavec-cc` 0.15.0-dev (9fb1dc1231cf). The other configs and platforms
have none yet: record them with `--quick --update` on a quiet machine, and
from CI's results files with `--update-from`.

## Triage (`triage.json`)

`entries` holds verdicts on the analysis' findings,
`{config, id, certainty, file, line, verdict, note}`, keyed by config, file,
line and id. A definite finding is an `error` of `weavec`, a possible one a
`warning`. Every definite error a run reports needs an entry and none may
be triaged `"false"`; possible findings are counted and need none. An entry
no finding of a config that ran matches is reported as stale without
failing; entries are removed when a population is re-triaged.

`guardFailures` lists the guard failures of a test suite that are true
bugs of the project: `{config, file, line, verdict, note}`, with an
optional `kind` (a report kind of RFC 0035, section 5.3). The only verdict
is `"true"`, and the note is the source evidence: the gate refuses an entry
with any other verdict, so a false trap is never triaged away (it is a bug
in WeaveC). A report at a listed site is not counted as a trap, and a test
suite that fails in trap mode only because of such reports passes when its
report-mode rerun passes. An entry no run names is reported as stale.

The entries are four for jansson's `hashlittle`
(`src/lookup3.h:259`, `260`, `263`, `264`): its tail switch reads a whole
32-bit word of the key and masks off the bytes past the key's end, as the
comment above the switch says; the source selects a byte-wise tail under
Valgrind and AddressSanitizer (`NO_MASKING_TRICK`). mbedtls's test helpers
read 239 bytes past a certificate array (`library/x509_crt.c:1415`); since
RFC 0035 the read lands inside the next global, which no redzone separates
it from, so no guard fails there (as under ASan), and it has no entry.

## Commands and their environment

`build`, `test`, `bench.build`, `bench.command`, `bench.check`,
`bench.input` and an injection's `build` and `run` are `/bin/sh -c`
commands run in the copy's root, with `CC`, `JOBS` (from `--jobs`), `SRC`
(the copy), `SUPPORT` (`support/<project>`), `BENCH` (`bench/`), `INPUT` (the
generated benchmark input), `INJECTION_DIR` (the injection's directory) and
`CACHE` (`<workdir>/.cache/<project>`, kept between runs, for downloaded
test data); `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, `MAKEFLAGS`, `WEAVEC_RT_ABORT`
and `WEAVEC_RT_STATS` are cleared, and `WEAVEC_RT_REPORT_LOG` is set to a
file of the gate's.

## Injections

Each entry of `injections/injections.json`:

```json
{"id": "sds-uaf-sdsfree", "config": "sds", "patch": "sds/uaf-sdsfree.patch",
 "file": "sds.c", "line": 168, "description": "..."}
```

The patch (`-p1`, `a/<file>`, `b/<file>`) writes the bug into a copy of the
pinned file with an `INJECTED` comment on `line`. The copy is built with the
config's `build` (or the entry's `build`) in trap mode, then the entry's
`run` (a command or a list; the config's `test` when absent) runs, and it
must stop:

- at `file:line`: a report there, of one of `kinds` when the entry names
  them;
- or at `stop` (`{"file": …, "line": …}`), when the bug is not where the
  access is: a freed pointer passed to a function that reads it stops in
  that function (`jsmn-uaf-jsondump`);
- or, with `"unlocated": "invalid-release"`, by the allocator refusing a
  release (`weavec: invalid release of …`, which names no line): the double
  frees; with `"unlocated": "fault"`, by a fault on the null page (a call
  through a null function pointer, `logc-null-lock`).

A run that never reaches the bug is a missed injection like any other: give
the entry a `run` that reaches it (a driver under `injections/<project>/`,
as for log.c, printf and `sds-df-freesplitres`). `--inject
--reference-only` checks a `run` with ASan. `scripts/test_corpus_gate.py`
checks that every patch applies to the pinned checkouts (under
`build/corpus`, or `$WEAVEC_CORPUS_WORKDIR`) and puts its marker on its
line.

Measured on 2026-10-06 with `weavec-cc` 0.15.0-dev (9fb1dc1231cf), the
injections of sds (5), log.c (2), printf (1) and jsmn (2) stop where their
entries say. Those of cJSON, linenoise, zlib, Lua and jansson were
converted from the analysis-era entries (whose `run` commands they keep,
and whose test suites they otherwise run) and await their first run.
