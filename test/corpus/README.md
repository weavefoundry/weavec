# The corpus gate

WeaveC on real C projects at pinned revisions: RFC 0030, section 17.5, run by
[`scripts/corpus-gate.py`](../../scripts/corpus-gate.py) (tests:
[`scripts/test_corpus_gate.py`](../../scripts/test_corpus_gate.py)). It
replaces `scripts/corpus.py` and `scripts/corpus/`.

| File | What it holds |
| --- | --- |
| `manifest.json` | The 9 projects (url, 40-hex `sha`, `support` files) and their 11 configs: `compile` (files and arguments), `wholeProgram`, `build`, `test`, `bench`, `link`, `lowered`; the 11 held-out projects of RFC 0031 (`heldOut`, below); and `gates`, the limits of gates G9–G15 (G14 as RFC 0032 amended it: three limits per benchmark), under `heldOut` of RFC 0031's G5, G6 and G12, and under `rfc0032` of RFC 0032's R4 |
| `expected.json` | The ratchet, per platform and config (written by `--update`), and `legacy`, v0.10.0's numbers for S0 and S1 |
| `triage.json` | A verdict for every definite error and possible temporal warning, and `guardFailures`: the guard failures of a test suite that were triaged as true bugs |
| `injections/` | `injections.json` and one patch per injected bug, by project (39; RFC 0032's eight exercise the runtime's guards), plus drivers the trap injections build |
| `bench/` | `lua-bench.lua`, the `cjson-bench.c` driver and `zlib-input.py`, the generator of the 64 MiB zlib input |
| `support/` | Files a checkout needs that it does not have: jansson's, bzip2's, libyaml's and miniz's generated headers (`{support}` in compile arguments), the Lua `testes` subset driver, bzip2's makefile, hiredis's test wrapper and utf8proc's test-data fetcher |

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

Per-file compiles use exactly the files and arguments `scripts/corpus.py`
analysed, so `--legacy` reproduces v0.10.0. The projects without a build
(log.c, printf) or a C test suite (printf's is C++, which includes `printf.c`
into a C++ unit) are compile-only: their per-file compiles are their build.

The Lua test is `support/lua/subset.lua`, which sets up what `testes/all.lua`
sets up in user mode (`_U`: no `T` library, no long or non-portable tests)
and runs gc, db, calls, strings, literals, tpack, attrib, gengc, locals,
constructs, code, cstack, nextvar, pm, utf8, api, memerr, events, vararg,
closure, coroutine, goto, errors, math, sort, bitwise and verybig. It leaves
out `files.lua` (it needs `/dev/full`, which macOS lacks), `main.lua` (the
stand-alone interpreter tests, skipped in user mode anyway), `big.lua` and
`heavy.lua` (long), and the internal tests (they need a build with
`ltests.h`).

## The held-out configs (RFC 0031, section 11.2)

Eleven projects WeaveC was never tuned on, each pinned by SHA with its own
build and test suite and marked `"heldOut": true`. Their triage entries record
verdicts only; no engine rule may be motivated by them (gate H2 already
forbids naming a corpus project under `lib/`).

| Config | Files | Whole program | Build | Test |
| --- | --- | --- | --- | --- |
| bzip2 | the library and `bzip2.c` (`-I{support}` for `bz_version.h`) | yes | `support/bzip2/bzip2.mk` (the checkout has only CMake and Meson): `libbz2.a`, `bzip2`, `bzip2-direct`, `bzip2recover` | the six-sample round trip for both `bzip2` binaries |
| hiredis | the library and `test.c` | yes | `make static hiredis-test` (no SSL, `-Werror` as shipped) | `test.sh` against a `redis-server` it starts (needs `redis-server` on `PATH`), through `support/hiredis/run-tests.sh`, which tolerates only the two connection-error tests that fail on Darwin with any compiler |
| http-parser | `http_parser.c test.c` | yes | `make test_g test_fast bench` | `test_g`, `test_fast` |
| inih | `ini.c tests/unittest.c` | yes | `ini.o` | `tests/unittest.sh` (15 configurations), then the tracked baselines must be unchanged |
| libyaml | `src/*.c` (`-I{support}` for `config.h`) | yes | CMake with tests | CTest and the `run-*` programs over `examples/*.yaml` |
| lz4 | the five `lib/` files | | `lib-release lz4-release` and the test programs | `tests/` `check`, 20 s each of `fuzzer` and `frametest`, `decompress-partial` |
| miniz | the four library files (`-I{support}` for `miniz_export.h`) | | CMake with the examples | examples 1, 2 and 6, and round trips of `miniz_zip.c` through examples 5 and 3 |
| mujs | `one.c` (every source in one unit) | | `build/release/mujs`, `mujs-pp` | none (compile and time only) |
| sqlite | `sqlite3.c shell.c` | | both units and the shell | none (compile and time only) |
| tinyexpr | `tinyexpr.c smoke.c` | yes | `smoke smoke_pr example example2 example3 repl` | the smoke tests, the examples, one `repl` expression |
| utf8proc | `utf8proc.c` | | `libutf8proc.a` and the test programs | the table tests and the Unicode 18.0.0 conformance files, which `support/utf8proc/fetch-test-data.sh` downloads once into `$CACHE` and checks by SHA-256 |

- **Selection.** `--full` runs them; `--quick` and the other modes leave them
  out, so the PR-time run stays fast, unless `--held-out` is given.
  `--no-held-out` leaves them out of `--full`; a config named by `--only` runs
  either way. `--legacy` never runs them (v0.10.0 was not measured on them).
- **Gates.** They are reported in a section of their own at the end of the
  run (`heldOut` in `--json`) and gated by RFC 0031 (`gates.heldOut`), not by
  G9, G10 and G11, which count the original configs:
  `rfc0031.G5`, no definite error triaged false and, with `--full`, every
  build and test suite passes with no trap; `rfc0031.G6`, their unresolved
  temporal share together (program ledgers where a whole-program analysis
  exists, unit ledgers otherwise) at most 0.50, the value at RFC 0031's close
  kept as a ratchet; `rfc0031.G12`, `one.c` and `sqlite3.c` each compiled
  within 900 CPU seconds and 4 GiB (with `--full` each held-out config is
  also built, not tested, with the reference compiler, and the ratio of the
  two builds' CPU times is reported, not limited: RFC 0031, *Gates carried
  forward*). G15's over-budget share counts them with the original configs
  (RFC 0031 G11).
- **Triage.** Every definite error needs an entry; possible temporal
  warnings are counted in their section but need none.
- **Ratchet.** A held-out config without an `expected.json` record is a note,
  not a failure, until `--update` records it; from then on it ratchets like
  the others. Every analysis now also records `unresolvedShare.temporal`,
  compared once a record has it.

## Running it

Checkouts live in `build/corpus/<project>` (`--workdir`). A missing one is
cloned at its `sha`; one at another commit is an error unless `--fetch` is
given; one with modified tracked files is an error. Nothing is ever built in
a checkout: builds, injections and benchmarks work on copies under
`<workdir>/.gate/`, deleted afterwards unless `--keep`.

```sh
# S0: v0.10.0 semantics with the golden binaries (test/cases/GOLDEN.md).
WEAVEC_GOLDEN_DIR=/path/to/golden scripts/corpus-gate.py --quick --legacy
WEAVEC_GOLDEN_DIR=/path/to/golden scripts/corpus-gate.py --inject --legacy

# S1: the binaries under test must print exactly the golden diagnostics.
scripts/corpus-gate.py --compare-golden --weavec build/release/bin/weavec \
    --weavec-cc build/release/bin/weavec-cc

# The RFC 0030 compiler (from S3): every PR, weekly and release runs.
scripts/corpus-gate.py --quick --weavec build/release/bin/weavec \
    --weavec-cc build/release/bin/weavec-cc [--only jansson ...] [--json out.json]
scripts/corpus-gate.py --full ...                  # G9-G15, rfc0032.R4
scripts/corpus-gate.py --full --checks verify ...  # G6 (RFC 0032 R1: temporal proofs too)
scripts/corpus-gate.py --inject ...                # G12 (RFC 0032 R2)
scripts/corpus-gate.py --bench ...                 # G14 (RFC 0032 R6): idle machine

# The build, test and bench commands with the reference compiler only, and
# the trap injections under ASan (checks that their run commands reach them).
scripts/corpus-gate.py --full --reference-only --cc "$(brew --prefix llvm)/bin/clang"

# RFC 0031's held-out configs: in --full by default, in --quick on request.
scripts/corpus-gate.py --quick --held-out --only bzip2 hiredis
scripts/corpus-gate.py --full --no-held-out ...
```

The binaries default to `build/release/bin`, then `build/dev/bin`; with
`--legacy`, to `$WEAVEC_GOLDEN_DIR`. The reference compiler (`--cc`) defaults
to `$WEAVEC_LLVM_PREFIX/bin/clang`. `--jobs` bounds parallel processes,
`--timeout` each analysis process (default 1,800 s), `--build-timeout` each
build or test step. The exit status is 0 on success, 1 when a check fails and
2 on a usage or setup error. `--json OUT` writes everything the run measured,
every diagnostic included.

## What each mode checks

- **`--quick`**: `weavec-cc -c -fweavec-ledger=…` for each file of each config
  (the *units* analysis) and `weavec --whole-program --ledger=…` for
  `wholeProgram` configs (the *program* analysis). A crash, a timeout, a Clang
  error or a missing ledger fails the run. Then the triage, the ratchet, and
  gates G9 (count), G10, G13 and G15, and `rfc0032.R4` (below). A ledger
  must be a `weavec-ledger` document of version 2.
- **`--full`**: `--quick`, then each config's `build` and `test` commands in a
  copy with `CC` set to a wrapper around `weavec-cc -fweavec-checks=trap
  -fweavec-ledger=<dir>/` (plus the config's `lowered` flags). Ledgers of
  CMake's compiler probes and configure tests are ignored: only units whose
  source is a tracked file count. A failing build or test command fails the
  run. A trap is a death by `SIGTRAP` or `SIGILL` (as the shell, make or
  CTest report it, or as the exit status of the command) or a
  `weavec: runtime check failed:` line in the report-mode rerun
  (`-fweavec-checks=report`, same commands), whether a static check or one
  of the runtime's guards (`object`, `live`, `release`) failed: the default
  build links the runtime, so a false trap of a guard fails the run
  (RFC 0032 gate R3). A check that fails at the site of a triaged-true
  definite error, or at a site listed in `guardFailures`, does not count
  (G11). zlib's
  `test/minigzip.c:568` (the repeated `fclose(stdout)`) must be reported
  (G9). Then the injections and the benchmarks.
- **`--inject`**: applies each patch to a copy and analyses it: the patched
  file alone (`unit`), or the config's files as one program
  (`whole-program`), where configs with `link` (lua) also compile every file
  with `weavec-cc -c` and link the objects directly, and both halves must
  report. An injection is reported when a diagnostic with one of its `ids` (and
  its `severity`, unless `any`) is at `file:line`, or, for `trap`
  expectations, when a report-mode build running its `run` command fails a
  check with that template at that line. An injection with only a `trap`
  expectation is met by the run alone. G12 needs 90% reported and every
  `required` injection: the two Lua allocator bugs, and the eight
  injections of RFC 0032 that exercise the runtime's guards (below).
- **`--bench`**: builds each benchmark three times, with the reference
  compiler, with `weavec-cc` (no extra flags: the default trap build, with
  the runtime) and with `weavec-cc -fno-weavec-runtime`, runs each once to
  warm up, then `repeat` (7) times interleaved, and takes the minimum user
  CPU time and the minimum peak resident size of each. G14 has three
  measurements, each a ratio over the reference build, with a limit per
  benchmark in the manifest:

  | Measurement | Manifest key | Ratchet key | lua | zlib | cJSON |
  | --- | --- | --- | --- | --- | --- |
  | user CPU of the default build | `maxOverhead` | `overhead` | 6.0 | 2.0 | 2.0 |
  | user CPU without the runtime | `maxOverheadNoRuntime` | `overheadNoRuntime` | 1.1 | 1.1 | 1.15 |
  | peak resident size of the default build | `maxRssRatio` | `rssRatio` | 2.0 | 2.0 | 2.0 |

  These are RFC 0032's gate R6 as its *Implementation amendments* (3) set
  it; the bounds the RFC first set for Lua (2.0) and zlib (1.5) with the
  runtime are carried forward as an open gate. The three builds must print
  the same result, and `check` must pass. The timings need an idle machine.
- **`--checks verify`**: builds and tests in verify mode; any trap fails (G6),
  and the report-mode rerun tells unproven checks from `weavec.proven` ones.
  With the runtime, verify mode also guards proven temporal facets, so the
  run covers temporal proofs (RFC 0032 gate R1).

`rfc0032.R4` is evaluated with the other analysis gates. It adds up, over
the original configs together and over the held-out configs together (the
program ledger where a whole-program analysis exists, the unit ledgers
otherwise), the unresolved facets of each kind over all facets of that kind,
and compares the shares with `gates.rfc0032.R4.maxUnresolvedShare`: spatial
0.12, null 0.01, temporal 0.20. A facet the runtime guards is `guarded`, not
unresolved, so it does not count. A group is gated only when all of its
configs were selected; the shares are reported either way.

`--legacy` runs v0.10.0's semantics: `weavec` exactly as `scripts/corpus.py`
ran it (per file, or `--whole-program` over all files; `-ferror-limit=0`;
the checkout as the working directory), diagnostics only. `--full --legacy`
builds and tests with the golden `weavec-cc` (`-Wno-error=weavec`), a smoke
test of the build machinery; legacy injections use the tool only, and match
by line and id (v0.10.0 reported every temporal finding as an error).

## The legacy baseline

`expected.json` records under `legacy` what the golden binaries (v0.10.0,
e0e2bd6) produce on macOS arm64: per config, the number of units, the count
of each diagnostic id, the bug claims and the SHA-256 of the sorted
diagnostics, identical to the `scripts/corpus.py` run the RFC's numbers come
from. Every id except `analysis-incomplete`, `annotation-required`,
`checking-incomplete` and `checking-failed` is a bug claim:

| Id | Count |
| --- | --- |
| null-dereference | 186 |
| double-free | 56 |
| use-after-free | 32 |
| leak | 22 |
| invalid-release | 3 |
| lifetime-too-short | 2 |
| **bug claims** | **301** |
| annotation-required | 40 |
| analysis-incomplete | 4,239 |

`legacy.injections` records which injections v0.10.0 reports at the injected
line (`--inject --legacy`). It agrees with the v0.10.0 dossier where that
measured the same bugs (`dossier` in `injections.json`): jansson 3/3, cJSON
1/1, sds 2/2, linenoise 1/1 and the plain `malloc` use-after-free in `lua.c`
caught; both Lua allocator bugs missed. A baseline recorded on another
platform is reported but not compared.

## The ratchet (`expected.json`)

`platforms.<platform>.configs.<config>` holds, for `units` and `program`:
`errors`, `warnings`, the `ledger` outcome counts (`sites`, `proven`,
`checked`, `guarded`, `violation`, `unresolved`, `trusted`),
`unresolvedShare.spatialNull` (unresolved spatial and null facets over all
spatial and null facets), `cpuSeconds` and `workCounters` (`blockTransfers`
from `-fweavec-analysis-stats`, `functions`, `sites`); and `traps` (`--full`)
and `overhead`, `overheadNoRuntime` and `rssRatio` (`--bench`: G14's three
ratios).

- Counts and shares must equal the record. A worse value is a regression; a
  better one (or a changed neutral count such as `checked`) fails too until
  `--update` records it, so a PR that improves the numbers ratchets them in.
- `cpuSeconds`, `overhead`, `overheadNoRuntime` and `rssRatio` may exceed the
  record by 10% on the machine that recorded them (`cpuSeconds` also by up
  to one second, for timer noise), and are not compared on others. `guarded`
  is a neutral count, like `checked`: any change fails until `--update`
  records it. Work counters may exceed it by 2%. `--update`
  rewrites them.
- Platforms are recorded separately (system headers change the numbers).
  `--update` records the sections the run measured; `--update-from RESULTS`
  records a `--json` file written elsewhere, for example by a CI runner.
  Nothing is recorded from a run whose analyses, builds or runs failed.

## Triage (`triage.json`)

Every definite error and every possible temporal warning of a run needs an
entry `{fingerprint, config, id, certainty, file, line, verdict, note}`, keyed
by the ledger fingerprint (RFC 0030, section 12.3). Untriaged findings fail
the gate; a definite error triaged `"false"` fails G9; entries no finding
matches are reported as stale. A `build` config that must build despite a
triaged-true definite error lists the one allowed lowering next to its
fingerprint:

```json
"lowered": [{"flag": "-Wno-error=weavec-double-free", "fingerprint": "…"}]
```

Any other `-Wno-error`, `-Wno-weavec…` or `-w` in a config is rejected.

### Guard failures (`guardFailures`)

A guard that fails while a project's own test suite runs is either a false
trap, which is a bug in WeaveC to fix, or a real bug in the project that
its tests happen to execute. `guardFailures` (RFC 0032 gate R3,
*Implementation amendments* 9) lists the second kind:

```json
{"config": "jansson", "file": "src/lookup3.h", "line": 259,
 "template": "object", "verdict": "true", "note": "…the source evidence…"}
```

All six keys are required, and the only verdict is `"true"`: the gate
refuses to load an entry with any other, so a false trap can never be
triaged away. The note is the source evidence that the access is a bug. A
failure at a listed `config`, `file` and `line` is not counted as a trap,
like one at the site of a triaged-true definite error; an entry that no
report-mode run names is reported as stale without failing.

There is one group, four entries for jansson's `hashlittle`
(`src/lookup3.h:259`, `260`, `263`, `264`): its tail switch reads a whole
32-bit word of the key and masks off the bytes past the key's end. The
source says so in the comment above the switch, and selects a byte-wise
tail instead under Valgrind and AddressSanitizer (`NO_MASKING_TRICK`).

## Commands and their environment

`build`, `test`, `bench.build`, `bench.command`, `bench.check`, `bench.input`
and an injection's `run` are `/bin/sh -c` commands run in the copy's root,
with `CC`, `JOBS` (from `--jobs`), `SRC` (the copy), `SUPPORT`
(`support/<project>`), `BENCH` (`bench/`), `INPUT` (the generated benchmark
input) and `INJECTION_DIR` (`injections/<project>`), and for builds and tests
`CACHE` (`<workdir>/.cache/<project>`, kept between runs, for downloaded test
data); `CFLAGS`, `CPPFLAGS`, `LDFLAGS` and `MAKEFLAGS` are cleared.

## Adding an injection

Write the bug into a copy of the pinned file with an `INJECTED` comment on
the line where it should be reported, make a `-p1` patch (`a/<file>`,
`b/<file>`) under `injections/<project>/`, and add an entry:

```json
{"id": "zlib-df-deflateend", "config": "zlib", "patch": "zlib/df-deflateend.patch",
 "file": "deflate.c", "line": 1302, "expect": {"ids": ["double-free"], "severity": "any"},
 "mode": "whole-program", "description": "..."}
```

A null or spatial bug that should trap names the template and a command that
reaches it, `"expect": {"ids": ["out-of-bounds"], "severity": "any",
"trap": "index", "run": "./example"}`; either a diagnostic or the trap
satisfies it.

A bug the analysis leaves unresolved by design, which only the runtime can
catch, has an `expect` with a `trap` and a `run` and no `ids`:
`"expect": {"trap": "object", "run": "./sds-test"}`. It is met only by a
guard failing at the injected line while `run` executes in the report-mode
build. RFC 0032 (gate R2, *Implementation amendments* 10) added eight
`required` injections, two for each of sds, cJSON, zlib and Lua: a write
past an extent only the allocator knows (`sds-oob-range`,
`cjson-oob-string-terminator`, `zlib-oob-window`, `lua-oob-newlclosure`,
all `object`), and a read through a pointer that a reallocation moved or
that a release behind a function pointer invalidated (`sds-uaf-catlen` and
`lua-uaf-reallocstack`, `live`; `cjson-uaf-print-realloc` and
`zlib-uaf-window`, which the analysis already reports and whose entries
therefore carry `ids` as well). `expect.trap: release` is also met when the
run dies with the allocator's own `weavec: invalid release of 0x…`: a
release behind a function pointer has no site to guard, so no line is
named (`cjson-df-valuestring`). The gate runs every test and injection with
`WEAVEC_RT_REPORT_LOG` set and reads that file as well as the output, so a
harness that hides the output of a passing test (CTest without `-V`) does
not hide a report. A trap satisfies an injection's ids as the guard's
template allows: `object` stands for `out-of-bounds`, `use-after-free` and
`use-after-move`; `live` for `use-after-free` and `use-after-move`;
`release` for `double-free`, `invalid-release` and `use-after-free`. `--inject --reference-only` checks that the run command
reaches the line (ASan and UBSan, or `_FORTIFY_SOURCE` for `len`).
`scripts/test_corpus_gate.py` checks that every patch applies to the pinned
checkouts and puts its marker on its line.
