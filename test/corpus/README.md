# The corpus gate

WeaveC on real C projects at pinned revisions: RFC 0030, section 17.5, run by
[`scripts/corpus-gate.py`](../../scripts/corpus-gate.py) (tests:
[`scripts/test_corpus_gate.py`](../../scripts/test_corpus_gate.py)). It
replaces `scripts/corpus.py` and `scripts/corpus/`.

| File | What it holds |
| --- | --- |
| `manifest.json` | The 9 projects (url, 40-hex `sha`, `support` files) and their 11 configs: `compile` (files and arguments), `wholeProgram`, `build`, `test`, `bench`, `link`, `lowered`; the 11 held-out projects of RFC 0031 (`heldOut`, below); the 8 fresh and 5 sealed projects of RFC 0033 (`set`, below); and `gates`, the limits of gates G9–G15 (G14 as RFC 0032 amended it: three limits per benchmark), under `heldOut` of RFC 0031's G5, G6 and G12, under `rfc0032` of RFC 0032's R4, and under `rfc0033` of RFC 0033's D1 and D5 |
| `expected.json` | The ratchet, per platform and config (written by `--update`), and `legacy`, v0.10.0's numbers for S0 and S1 |
| `triage.json` | A verdict for every definite error and possible temporal warning, and `guardFailures`: the guard failures of a test suite that were triaged as true bugs |
| `injections/` | `injections.json` and one patch per injected bug, by project (39; RFC 0032's eight exercise the runtime's guards), plus drivers the trap injections build |
| `bench/` | `lua-bench.lua`, the `cjson-bench.c` driver and `zlib-input.py`, the generator of the 64 MiB zlib input |
| `support/` | Files a checkout needs that it does not have: jansson's, bzip2's, libyaml's and miniz's generated headers, and those of oniguruma, expat, pcre2, libevent, libsodium, libxml2, libpng and msgpack-c (`{support}` in compile arguments), the Lua `testes` subset driver, bzip2's makefile, the test wrappers of hiredis, libuv and redis, utf8proc's test-data fetcher, mbedtls's `framework` submodule fetcher, and msgpack-c's expected example outputs |

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

## The fresh and sealed configs (RFC 0033, section 11)

Thirteen more projects WeaveC was never tuned on, pinned by SHA with their
own build and test commands, marked `"heldOut": true` and a `set`:
`"fresh"` for the eight that measure the drop-in default (gates D1 and D5)
and `"sealed"` for the five kept for gate D2's single run. A config without
`set` is `original`, or `heldOut` when it is marked heldOut; `set` takes
only `fresh` or `sealed`, and needs `heldOut: true`. None is a whole-program
config (too big for the PR-time run), and the gate's word check forbids
every one of their names under `lib/`. The test times are those of the
reference compiler on darwin-arm64 (`--full --reference-only`, `--jobs 4`).

| Config | Set | Files | Build | Test | Test time |
| --- | --- | --- | --- | --- | --- |
| zstd | fresh | `lib/{common,compress,decompress,dictBuilder}/*.c` (30) | `lib-release zstd-release`, `tests/` `datagen fuzzer zstreamtest` | `playTests.sh` without long tests, `fuzzer -s1 -i300`, `zstreamtest -s1 -i100`, a `--train`ed dictionary round trip at `-13` | 21 s |
| libuv | fresh | the 37 sources of the Darwin build | CMake, `uv_run_tests_a` | `support/libuv/run-tests.sh`: every test in its own process but `emfile`, `spawn_exercise_sigchld_issue`, `udp_multicast_join` and `udp_multicast_join6`, which fail on Darwin with the reference compiler too | 43 s |
| oniguruma | fresh | the 35 library sources (`-I{support}` for `config.h`) | CMake, static, POSIX API, tests | the eight test programs | 1 s |
| redis | fresh | 40 data-structure and command sources of `src/` | `make MALLOC=libc BUILD_TLS=no CLANG=clang OPTIMIZATION=-O2` | `support/redis/run-tests.sh`: 11 units of the Tcl suite (list, hash, set, zset, string, incr, expire, keyspace, sort, scan, hyperloglog), 4 clients | 55 s |
| expat | fresh | `expat/lib/xml{parse,role,tok}.c` (`-I{support}` for `expat_config.h`) | CMake in `expat/`: `runtests`, `xmlwf`, three examples | `runtests`, `xmlwf` over `testdata/largefiles`, the examples | 11 s |
| pcre2 | fresh | 29 library sources, 8-bit (`-I{support}` for `config.h`, `pcre2.h`) | CMake, 8/16/32-bit, no JIT | CTest: `RunTest`, `RunGrepTest`, `pcre2posix_test` | 2 s |
| libevent | fresh | the 24 core and extra sources (`-I{support}` for the two config headers) | CMake, static, no OpenSSL, Mbed TLS or benchmarks | CTest without `regress`, then `regress` on kqueue and on poll in debug mode, without `dns/initialize_nameservers` | 167 s |
| libsodium | fresh | 44 portable sources (`-I{support}` for `sodium/version.h`) | the shipped `./configure --disable-shared`, `make`, `make check TESTS=` | `make check` (80 programs against their `.exp` files) | 5 s |
| libxml2 | sealed | 39 library sources | CMake, static, no Python, iconv, ICU, lzma, zlib, HTTP | CTest (7 tests) | 2 s |
| libpng | sealed | the 15 library sources and `pngtest.c` | CMake with tests and tools (the SDK's zlib) | CTest (33 tests) | 20 s |
| mbedtls | sealed | 40 `library/` sources | `support/mbedtls/fetch-framework.sh`, CMake with tests and programs | CTest serially (135 suites), `selftest` | 36 s |
| msgpack-c | sealed | the five `src/` files | CMake, static, examples | the examples, three against recorded outputs (its tests need GoogleTest) | 2 s |
| yyjson | sealed | `src/yyjson.c` | CMake with tests | CTest (12 tests) | 3 s |

Each config's `notes` say why its build and test are what they are.

- **Selection.** The fresh configs run with the held-out ones: in `--full`,
  in other modes with `--held-out`, never with `--legacy`. The sealed ones
  run only with `--sealed` (which selects them alone) or when `--only` names
  them. RFC 0033 runs `--sealed` once, on the final tree, for gate D2;
  before that a sealed config may be built only with the reference compiler
  (`--full --sealed --reference-only`), to write its commands. After D2 they
  become fresh configs and the next RFC seals a new set.
- **Gates.** Each set has its own section at the end of the run (`fresh` and
  `sealed` in `--json`) and none counts towards G9, G10, G11, RFC 0031's
  held-out gates or G15's over-budget share. `rfc0033.D1`: no definite error
  triaged false and, with `--full`, every build and test suite passes, in
  trap mode with no trap and in report mode with no failure (a build that
  stops only at definite errors triaged true is kept, as for `rfc0031.G5`);
  `rfc0033.D5`: with `--full`, each config's `weavec-cc` build CPU time at
  most `gates.rfc0033.D5.maxBuildRatio` (4.0) times the reference
  compiler's, which builds it once more for the measurement. The sealed
  configs get the same values as `rfc0033.sealed.D1` and
  `rfc0033.sealed.D5`; a failure fails the run, and D2 records the result
  whatever it is. Their unresolved shares are reported under `rfc0032.R4`,
  not gated.
- **Triage and ratchet.** As for the held-out configs: every definite error
  needs a verdict, and a config without an `expected.json` record is a note
  until `--update` records it.

## The RFC 0034 sets (`fresh34`, `sealed34`; RFC 0034, section 9)

Fifteen more projects nobody tuned WeaveC on, pinned by SHA with their own
build, test and (for `fresh34`) a run-time workload (`bench`), marked
`"heldOut": true` and `"set": "fresh34"` or `"sealed34"`. `--set SET`
runs one set alone; `sealed34` runs only with `--set sealed34` (before the
final tree only with `--reference-only`, to write its commands).

| Config | Set | Files | Test | Workload (`bench`) |
| --- | --- | --- | --- | --- |
| quickjs | fresh34 | 6 | `make test`, the examples | `qjs` over `bench/quickjs-bench.js` |
| lmdb | fresh34 | 8 | `support/lmdb/run-tests.sh` | `bench/lmdb-bench.c` (put, get) |
| janet | fresh34 | 2 | `make test` (its socket suite is timing-dependent) | `bench/janet-bench.janet` |
| brotli | fresh34 | 3 | CTest, round trips | `brotli -q 7` and back, 64 MiB |
| xz | fresh34 | 45 | CTest, round trips | `xz -6 -T1` and back, 64 MiB |
| libdeflate | fresh34 | 2 | CTest, round trips | `libdeflate-gzip -1/-6/-9` and back, 64 MiB |
| zlib-ng | fresh34 | 38 | CTest, `support/zlib-ng/round-trips.sh` | `minigzip -1/-6/-9` and back, 64 MiB |
| curl | fresh34 | 45 | `runtests.pl` (1–999, 1300–1399, 1500–1699, not flaky or timing-dependent) | `bench/curl-bench.c` (URL and cookie parsing) |
| cmark | fresh34 | 1 | CTest | the spec rendered in five formats |
| libgit2 | fresh34 | 45 | `util_tests`, offline suites of `libgit2_tests` | `bench/libgit2-bench.c` (history walk) |
| libjpeg-turbo | sealed34 | 45 | CTest | — |
| opus | sealed34 | 45 | CTest (`SEED=1`) | — |
| flac | sealed34 | 42 | CTest, the test streams at level 0 | — |
| giflib | sealed34 | 1 | `support/giflib/run-tests.sh` | — |
| wren | sealed34 | 4 | `util/test.py` | — |

- **Gates** (`gates.rfc0034` in the manifest): `rfc0034.F1` over `fresh34`
  (every build passes with the default flags, or stops only at definite
  errors triaged true, and every test suite passes in trap mode with no trap
  and in report mode with no failure); `rfc0034.F2`, the same over
  `sealed34` from its one run; `rfc0034.F5` (each config's build CPU time
  at most 5 times the reference compiler's, `sqlite3.c` within 120 CPU
  seconds, no compiler process above 2,048 MiB of peak memory) and
  `rfc0034.F7` (with `--bench`, each `fresh34` workload at most 4 times the
  reference compiler's user CPU time and their geometric mean at most 2.5;
  RFC 0032's G14 benchmarks: Lua 2.5, zlib 1.5, cJSON 1.5).
- **Benchmarks.** A config's `bench` builds the workload three times (the
  default build, one with `-fno-weavec-runtime`, and the reference
  compiler's), runs `command` `repeat` times and takes the minimum user CPU time; `check`
  verifies the output, `input` makes a deterministic input. Run nothing
  else heavy on the machine during `--full`: the workloads and janet's
  socket suite are timing-sensitive.

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

# RFC 0033: the fresh configs run with the held-out ones; the sealed ones
# only with --sealed (gate D2, once) or by name.
scripts/corpus-gate.py --quick --held-out --only zstd redis
scripts/corpus-gate.py --full --sealed --reference-only --cc "$(brew --prefix llvm)/bin/clang"
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
configs were selected; the shares are reported either way, and those of
RFC 0033's fresh and sealed sets are reported but never gated.

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
