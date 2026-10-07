# Test cases

`test/cases` is the tree of executable C test cases (RFC 0035 §11). Each
case is a C file whose expectations are line-comment *markers*.
`scripts/run-cases.py` checks the advisory analysis (the `weavec` tool)
against the `BUG` and `CLEAN` markers, builds the case as a default
`weavec-cc` build does, checks its enforcement ledger, runs it and attributes
every stop to a line and a report kind, and optionally runs it under ASan.
The lit suites (`test/Guards`, `test/Driver`, ...) pin exact IR and
messages; these cases pin what must be reported and what must stop.

| Directory | Contents |
| --- | --- |
| `evaluation/` | the bug and clean programs of the fixed evaluation (RFC 0013) |
| `pairs/` | RFC 0017's bug/clean pairs |
| `recall/<CWE>/` | recall pins, by CWE |
| `engine/` | analysis pins reduced from the lit engine tests |
| `soundness/` | soundness probes, each bug with a correct twin, and the alias probes (`alias-*`) |
| `repros/` | root-cause false-positive repros and RFC 0031's held-out repros (`ooc-*`) |
| `proofs/` | cases that once caught a false proof (`SOURCES.md` gives their origin) |
| `semantics/<feature>/` | cases per feature: `objects/` the analysis's object domain, `runtime/` the runtime's guards and releases, `dropin/` untuned code, `confirm/` false stops and silent misses, `detect-probes/` detection probes, ... |
| `detection/` | RFC 0034's detection set: one program per directory, each with a fixed twin ([Detection cases](#detection-cases)) |
| `detection-blind/` | 48 blind single-file bug programs with `-DFIX` twins (its `README.md`) |

A file under an `Inputs/` directory is never a case: it holds units,
headers and input files that cases share.

## Running

```sh
scripts/run-cases.py                                # every case, trap mode
scripts/run-cases.py --filter 'soundness/**' -j 8   # one suite
scripts/run-cases.py --checks verify                # removed guards become monitors
scripts/run-cases.py --asan                         # the ASan oracle for every case
scripts/run-cases.py --filter 'detection/**' --min-stops 58
scripts/run-cases.py --filter pairs/min-bound-bug.c -v --keep
```

- The binaries default to `build/release/bin`, or `build/dev/bin` when there
  is no release build; `--build-dir`, `--weavec` and `--weavec-cc` override
  them. The ASan oracle uses `$WEAVEC_LLVM_PREFIX/bin/clang`, else `clang`
  on the `PATH`, or `--clang`.
- `--filter GLOB` selects cases by their path under `test/cases`
  (`'soundness/**'`, `'pairs/*-bug.c'`); a pattern without wildcards selects
  a file or a directory. It is repeatable.
- `--checks verify` builds with `-fweavec-checks=verify`: every guard the
  pass removed as proven is emitted as a monitor, and a monitor that fires
  (a `weavec.proven:` report) fails the case whatever its markers.
- `--asan` runs the ASan oracle for every case that has a `main` (the `ASAN`
  marker does so for one case, and also requires a report).
- `--no-run` analyses and builds only. `--jobs N` runs N cases at once
  (default: the CPU count). `--timeout` bounds each compile and link (120 s),
  `--run-timeout` each run (30 s).
- `--json OUT` writes `{"summary", "results"}`: per-suite tallies and, per
  case, its status, failures, notes, diagnostics, runs, ASan result and the
  commands it ran. `--verbose` prints passing cases too; `--keep` keeps the
  build directories (under `$TMPDIR/weavec-cases.*`).
- A case's status is `PASS`, `FAIL`, `ERROR` (a marker error), `XFAIL` or
  `XPASS`. The exit status is 1 when any selected case fails or has a marker
  error, or when `--min-stops` is not met, and 2 when a binary is missing or
  no case is selected.

CTest runs one test per top-level directory, `cases-<suite>`, against the
build tree's binaries (`ctest -L cases`, or `ctest -R cases-soundness`), each
with `WEAVEC_CASES_JOBS` workers (default 2) and its JSON in the build tree,
plus the runner's own unit tests, `cases-runner-harness`
(`python3 scripts/test_run_cases.py`, no compiler needed). The cache variable
`WEAVEC_CASES_ARGS` adds runner options to every suite.

## Markers

Markers are `//` comments. A *file marker* is on a comment-only line before
the first declaration (preprocessor lines may come first); a *line marker*
is at the end of the code line it applies to. One comment can hold several
markers separated by `//`:

```c
// RFC 0017: added regression pair.      <- prose: not a marker
// FLAGS: -std=c11
// RUN-INPUT: 1
#include <stdlib.h>
...
  p[n] = 0; // BUG: out-of-bounds // TRAP: heap-buffer-overflow
```

| Marker | Kind | Meaning |
| --- | --- | --- |
| `BUG: <id> [definite\|possible]` | line | the analysis reports `<id>` on this line (`definite`: an error, `possible`: a warning, omitted: either); for an id a guard can catch, a stop on this line also satisfies it ([below](#how-a-case-is-judged)) |
| `TRAP[: <kind>]` | line | some run stops on this line, with that report kind if one is given |
| `MISS: <reason>` | line | a known miss: with `BUG` on the same line, the analysis is known not to report it; in a `DETECT` case, on a `STOP` line, the bug is known not to stop |
| `NEUTRALISED: zero-init` | line | with `BUG` on the same line: zero-initialisation defines the bug away, so the analysis need not report it |
| `GUARDED[: <reason>]` | line | the enforcement ledger has a `guarded` row on this line (reasons: `access`, `range`, `string`, `checked-call`, `loop-range`) |
| `PROVEN[: <reason>]` | line | likewise `proven` (`in-bounds`, `dominated`, `merged`, `optimized`) |
| `UNGUARDED[: <reason>]` | line | likewise `unguarded` (`unsafe`) |
| `STOP` | line | in a `DETECT` case, a line the bug must stop on |
| `CLEAN` | file | no diagnostic (but `ALLOW`ed warnings), no stop, no death by a signal, no ASan report |
| `ALLOW: <id> [<id> ...]` | file | in a `CLEAN` case, warnings with these ids are accepted; justify each in a comment |
| `RUN-INPUT: <argv...> [< <file>]` | file | run the program with these arguments (shell quoting; the input file is relative to the case); repeatable, each run independent; an empty `RUN-INPUT:` is a run without arguments |
| `TRAP-AT: <file>:<line>` | file | as a `TRAP` on that line of another file (relative to the case), such as a shared `Inputs/` unit or header that only some of its cases expect to stop in |
| `EXPECT-LEDGER: <json-pointer> <op> <value>` | file | a value of the main unit's ledger entry (`units[0]`), e.g. `/summary/unguarded == 0`; `op` is `==` `!=` `<=` `>=` `<` `>`; the value is JSON, or a bare string |
| `FLAGS: <flags>` | file | extra `weavec-cc` flags for every compile and link (repeatable; translated for the tool, [below](#how-a-case-is-judged)) |
| `UNITS: <file.c> [<file.c> ...]` | file | further translation units linked with this one, relative to the case |
| `ASAN` | file | also run the ASan oracle, and require it to report the bug |
| `TOOL` | file | analyse only: no build, no run |
| `DETECT: <flags>` | file | a detection case ([below](#detection-cases)): `<flags>` (e.g. `-DFIX`) select the fixed twin |
| `XFAIL: <reason>` | file | the case is expected to fail today: a failure is `XFAIL`, a pass is `XPASS` (remove the marker); neither fails the run |

Report kinds are those of the runtime (`runtime/weavec_report.c`, RFC 0035
§5.3): `heap-buffer-overflow`, `heap-use-after-free`,
`stack-buffer-overflow`, `stack-use-after-scope`,
`dynamic-stack-buffer-overflow`, `global-buffer-overflow`,
`buffer-overflow`, `null-dereference`, `unterminated-string`,
`index-out-of-bounds`, `invalid-release`, `invalid-access` and
`overlapping-copy`. Ids are those of `weavec::core::diag`
(`include/weavec/Core/Diagnostic.h`).

Rules the grammar leaves implicit, as the runner enforces them:

- The marker keywords are reserved at the start of a comment segment. A
  keyword used wrongly (`// CLEAN please`, `// BUG leak`), an unknown id,
  kind or reason, and a near-miss keyword (`BUGS:`, `UNIT:`) are *marker
  errors*: the case is reported as `ERROR` and fails the run. Other
  uppercase prefixes (`NOTE:`, `RFC 0017:`, `STAGE:`) are prose.
- A line marker on a comment-only line, or a file marker on a code line or
  after the first declaration, is a marker error.
- A unit named by `UNITS` is not a case of its own. Its line markers apply
  to its lines, and its own `FLAGS` to its compile only; any other file
  marker in a unit is a marker error. A unit whose `FLAGS` contain
  `-fno-weavec` is compiled as plain Clang and the analysis does not see
  it; the case's main file cannot have `-fno-weavec`. Markers are read from
  the case and its units only, never from headers (use `TRAP-AT`).
- A case needs an expectation (`CLEAN`, `EXPECT-LEDGER` or a line marker).
  `CLEAN` excludes `BUG`, `MISS`, `NEUTRALISED` and `TRAP`; `ALLOW` needs
  `CLEAN`; `ASAN` and `RUN-INPUT` need a unit that defines `main`; `TOOL`
  excludes both. `STOP` needs `DETECT`.

## How a case is judged

1. **Analysis**, when the case has a `BUG`, is `CLEAN` or is a `TOOL` case:
   `weavec [<own options>] [--whole-program] <units> -- -I<resources/include> <compiler flags>`,
   with `--whole-program` when it has several analysed units. `FLAGS` are
   split: `-fno-weavec-zero-init` becomes `--no-zero-init`,
   `-fweavec-budget=N` becomes `--budget=N`, the `-W…weavec…` flags are the
   tool's own, other `-fweavec`/`-fno-weavec` flags are dropped, and the
   rest go after `--`. A crash (an exit other than 0 or 1) or a timeout
   fails the case.
2. **Build** (not for `TOOL`): each unit with
   `weavec-cc [-fweavec-checks=verify] -I<resources/include> <FLAGS> <unit FLAGS> -c`,
   adding `-fweavec-ledger=<dir>/` when a ledger marker or `EXPECT-LEDGER`
   reads the ledger, then a link with the case's `FLAGS` when a unit defines
   `main`. A failed compile or link fails the case.
3. **Ledger.** The main unit's `<object>.ledger.json` must be a
   `weavec-ledger` of version 3. `GUARDED`, `PROVEN` and `UNGUARDED` each
   need a row at their file and line with that outcome (and that reason, if
   given); `EXPECT-LEDGER` compares the value its pointer names.
4. **Runs**, when the build produced an executable (not with `--no-run`):
   once per `RUN-INPUT`, or once without arguments; stdin is `/dev/null`
   unless redirected. A run's *stop* is its first `weavec: <kind> at
   <file>:<line>:<col>: ...` report when it died by `SIGTRAP` or `SIGILL`
   (a report `at <unknown>` has no location); the allocator's
   `weavec: invalid release of ...` (an `invalid-release` with no location);
   or `SIGSEGV`/`SIGBUS` (a `null-dereference` with no location, the null
   page). Every stop must match a `TRAP` (its kind, if it names one, and its
   line, unless the stop has no location) or be on a `BUG` line (a stop with
   no location is accepted in any case with a `BUG`); every `TRAP` must be
   matched by some run. A timeout, a death by another signal or a trap with
   no report, and a report that did not stop the run fail the case; in a
   `CLEAN` case so does any report or signal. A `weavec.proven:` report
   (verify mode) fails the case.
5. **Diagnostics.** Each `BUG` is satisfied by a diagnostic with its id on
   its line and of its severity. For an id a guard can catch
   (`out-of-bounds`, `null-dereference`, `use-after-free`, `double-free`,
   `invalid-release`, `use-of-uninitialized`, `mismatched-release`,
   `lifetime-too-short`, `use-after-move`, `allocation-failure`,
   `contradicted-assumption`, `unsafe-operation`), a stop located on its
   line, or a stop with no location whose kind is the id, satisfies it too,
   and so does the absence of any run (`TOOL`, no `main`, `--no-run`). A
   `MISS` or `NEUTRALISED` on the line excuses it. Beyond the `BUG`, `MISS`
   and `NEUTRALISED` lines, a `CLEAN` case fails on any diagnostic but an
   `ALLOW`ed warning, and any other case on an error.
6. **ASan oracle** (`--asan` or `ASAN`), for a case with `main`:
   `clang -fsanitize=address,array-bounds -fno-sanitize-recover=array-bounds -g -O0 -I<resources/include>`
   over all units with their flags minus WeaveC's own, run with
   `detect_leaks=0:detect_stack_use_after_return=1`. A case without a bug
   line (`BUG`, `MISS`, `NEUTRALISED`, `TRAP`) fails on any report; a case
   with `ASAN` and a bug line fails without one.

An `XFAIL` case is judged as usual; its failure is reported as `XFAIL` and
its pass as `XPASS`. A marker error is still `ERROR`.

## Detection cases

A `DETECT` case is judged by whether its bug *stops*, not by what is
reported where, and is not analysed. It has one or more `STOP` lines (the
faulty operation) and none of `CLEAN`, `ALLOW`, `TOOL`, `BUG`, `TRAP`,
`TRAP-AT`, `GUARDED`, `PROVEN`, `UNGUARDED`, `NEUTRALISED` or
`EXPECT-LEDGER`; a unit must define `main`. The runner builds it twice, as
is (the bug) and with the `DETECT` flags added to every compile and link
(the twin), and runs each build once per `RUN-INPUT`.

- **The bug stops** when some run's stop (as in step 4) is on a `STOP` line
  or has no location. A first stop on another line is not a stop. A bug
  that does not stop fails the case, unless its `STOP` line carries
  `MISS: <reason>`: a known miss passes with a note, and when it stops the
  note says to remove the marker.
- **The twin must not stop**: no timeout, no signal, no report.
- A `weavec.proven:` report of the bug fails the case. `--asan` also runs
  the bug under ASan and counts its reports.

The summary has a line `detection: N of M bugs stopped (ASan: K)`, and
`--min-stops N` fails the run unless at least N of the selected detection
cases stop.

`detection/NN_<class>_<name>/` holds one program (its `prog.c` or `main.c`,
any further units named by `UNITS`, its headers and its inputs), built with
`FLAGS: -O2`. `detection-blind/` holds single-file programs.

## Adding a case

1. Put it in the directory of its feature (`semantics/<feature>/` for new
   behaviour), named for what it tests (`*_bug.c` and `*_ok.c` for a pair);
   there is no manifest to update.
2. Start with a comment saying what the case is about and where it comes
   from, then the file markers, then the code.
3. Mark a correct program `CLEAN`. In a bug program, pin what the analysis
   should report with `BUG` on that line (with `MISS: <reason>` when it does
   not, today), and where the program must stop with `TRAP: <kind>`. Give it
   a `main` that reaches the bug (with `RUN-INPUT` if it needs arguments),
   and add `ASAN` when ASan can confirm the bug. Pin enforcement itself with
   `GUARDED`, `UNGUARDED` or `EXPECT-LEDGER` where that is the point.
4. Run `scripts/run-cases.py --filter '<path>' -v`, then again with
   `--checks verify` and `--asan`.

## History

The tree began as RFC 0030's case suite (§17), when cases also pinned
analysis-ledger facets (`UNRESOLVED`, `TRUSTED`, `NOT-PROVEN`), check
templates in `TRAP`, require levels and a comparison against the v0.10.0
golden binaries (`GOLDEN.md`, `KNOWN-DIFFERENCES.md`). RFC 0035 deleted the
analysis ledger, the checks and the require levels; the cases were
converted to the grammar above, and the comments of older cases may still
cite the RFC they were written for.
