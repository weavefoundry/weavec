# The corpus gate

WeaveC on real C projects at pinned revisions: RFC 0030, section 17.5, run by
[`scripts/corpus-gate.py`](../../scripts/corpus-gate.py) (tests:
[`scripts/test_corpus_gate.py`](../../scripts/test_corpus_gate.py)). It
replaces `scripts/corpus.py` and `scripts/corpus/`.

| File | What it holds |
| --- | --- |
| `manifest.json` | The 9 projects (url, 40-hex `sha`, `support` files) and their 11 configs: `compile` (files and arguments), `wholeProgram`, `build`, `test`, `bench`, `link`, `lowered`; and `gates`, the limits of gates G9–G15 |
| `expected.json` | The ratchet, per platform and config (written by `--update`), and `legacy`, v0.10.0's numbers for S0 and S1 |
| `triage.json` | A verdict for every definite error and possible temporal warning |
| `injections/` | `injections.json` and one patch per injected bug, by project, plus drivers the trap injections build |
| `bench/` | `lua-bench.lua`, the `cjson-bench.c` driver and `zlib-input.py`, the generator of the 64 MiB zlib input |
| `support/` | Files a checkout needs that it does not have: jansson's configured headers (`{support}` in compile arguments) and the Lua `testes` subset driver |

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
scripts/corpus-gate.py --full ...                  # G9-G15
scripts/corpus-gate.py --full --checks verify ...  # G6
scripts/corpus-gate.py --inject ...                # G12
scripts/corpus-gate.py --bench ...                 # G14

# The build, test and bench commands with the reference compiler only, and
# the trap injections under ASan (checks that their run commands reach them).
scripts/corpus-gate.py --full --reference-only --cc "$(brew --prefix llvm)/bin/clang"
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
  gates G9 (count), G10, G13 and G15.
- **`--full`**: `--quick`, then each config's `build` and `test` commands in a
  copy with `CC` set to a wrapper around `weavec-cc -fweavec-checks=trap
  -fweavec-ledger=<dir>/` (plus the config's `lowered` flags). Ledgers of
  CMake's compiler probes and configure tests are ignored: only units whose
  source is a tracked file count. A failing build or test command fails the
  run. A trap is a death by `SIGTRAP` or `SIGILL` (as the shell, make or
  CTest report it, or as the exit status of the command) or a
  `weavec: runtime check failed:` line in the report-mode rerun
  (`-fweavec-checks=report`, same commands); a check that fails at the site of
  a triaged-true definite error does not count (G11). zlib's
  `test/minigzip.c:568` (the repeated `fclose(stdout)`) must be reported
  (G9). Then the injections and the benchmarks.
- **`--inject`**: applies each patch to a copy and analyses it: the patched
  file alone (`unit`), or the config's files as one program
  (`whole-program`), where configs with `link` (lua) also compile every file
  with `weavec-cc -c` and link the objects directly, and both halves must
  report. An injection is reported when a diagnostic with one of its `ids` (and
  its `severity`, unless `any`) is at `file:line`, or, for `trap`
  expectations, when a report-mode build running its `run` command fails a
  check with that template at that line. G12 needs 90% reported and every
  `required` injection (the two Lua allocator bugs).
- **`--bench`**: builds each benchmark with the reference compiler and with
  `weavec-cc` (no extra flags: the default trap build), runs each once to
  warm up, then `repeat` (7) times interleaved, and takes the minimum user
  CPU time of each; the ratio is the overhead (G14). The two builds must print
  the same result, and `check` must pass.
- **`--checks verify`**: builds and tests in verify mode; any trap fails (G6),
  and the report-mode rerun tells unproven checks from `weavec.proven` ones.

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
`checked`, `violation`, `unresolved`, `trusted`),
`unresolvedShare.spatialNull` (unresolved spatial and null facets over all
spatial and null facets), `cpuSeconds` and `workCounters` (`blockTransfers`
from `-fweavec-analysis-stats`, `functions`, `sites`); and `traps` (`--full`)
and `overhead` (`--bench`).

- Counts and shares must equal the record. A worse value is a regression; a
  better one (or a changed neutral count such as `checked`) fails too until
  `--update` records it, so a PR that improves the numbers ratchets them in.
- `cpuSeconds` and `overhead` may exceed the record by 10% on the machine that
  recorded them (`cpuSeconds` also by up to one second, for timer noise), and
  are not compared on others. Work counters may exceed it by 2%. `--update`
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

## Commands and their environment

`build`, `test`, `bench.build`, `bench.command`, `bench.check`, `bench.input`
and an injection's `run` are `/bin/sh -c` commands run in the copy's root,
with `CC`, `JOBS` (from `--jobs`), `SRC` (the copy), `SUPPORT`
(`support/<project>`), `BENCH` (`bench/`), `INPUT` (the generated benchmark
input) and `INJECTION_DIR` (`injections/<project>`); `CFLAGS`, `CPPFLAGS`,
`LDFLAGS` and `MAKEFLAGS` are cleared.

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
satisfies it. `--inject --reference-only` checks that the run command
reaches the line (ASan and UBSan, or `_FORTIFY_SOURCE` for `len`).
`scripts/test_corpus_gate.py` checks that every patch applies to the pinned
checkouts and puts its marker on its line.
