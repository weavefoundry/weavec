# Test cases

`test/cases` is the one tree of executable C test cases (RFC 0030, section 17).
Each case is a C file whose expectations are line-comment *markers*;
`scripts/run-cases.py` builds it with `weavec-cc`, checks its diagnostics and
ledger, runs it, and optionally runs it under ASan. The lit suites
(`test/Analysis`, `test/Driver`, ...) pin exact messages; these cases pin what
must be reported, checked or left unproven.

| Directory | Contents |
| --- | --- |
| `evaluation/` | the 44 bug and 32 clean programs of the fixed evaluation (RFC 0013) |
| `pairs/` | the 24 RFC 0017 cases (12 bug/clean pairs) |
| `recall/<CWE>/` | the recall pins (67, as the retired `scripts/recall.py` counted them) |
| `engine/` | ordinary-era lit engine pins, reduced to (line, id) from the golden run |
| `soundness/` | the 113 soundness probes (85 bug, 28 correct) and their extra units; see its README |
| `repros/` | the 12 root-cause false-positive repros, with their intended RFC 0030 expectations |
| `proofs/` | salvaged cases that once caught a false proof (`SOURCES.md` gives their origin) |
| `semantics/<feature>/` | new cases per RFC 0030 feature |

`GOLDEN.md` describes the golden v0.10.0 binaries and `KNOWN-DIFFERENCES.md`
lists the engine pins the RFC 0030 build no longer reproduces.

## Running

```sh
scripts/run-cases.py                                  # every case, trap mode
scripts/run-cases.py --filter 'soundness/**' --asan   # gate G4
scripts/run-cases.py --checks verify                  # gate G6
scripts/run-cases.py --require checked --filter 'soundness/*_ok.c' --filter 'soundness/*_fp.c'  # gate G5 (the 28 twins)
scripts/run-cases.py --no-emission                    # before checks are emitted (S3, S4)
scripts/run-cases.py --no-run                         # build, diagnostics and ledger only
WEAVEC_GOLDEN_DIR=<dir> scripts/run-cases.py --legacy               # S0: v0.10.0 semantics
WEAVEC_GOLDEN_DIR=<dir> scripts/run-cases.py --compare-golden       # S1: same diagnostics
```

- The binaries default to `build/release/bin`, or `build/dev/bin` when there is
  no release build; `--build-dir`, `--weavec` and `--weavec-cc` override them.
  `--legacy` uses `$WEAVEC_GOLDEN_DIR/weavec` (or `--golden-dir`) unless
  `--weavec` is given.
- `--filter GLOB` selects cases by their path under `test/cases`
  (`'soundness/**'`, `'pairs/*-bug.c'`); a pattern without wildcards selects
  a file or a directory. It is repeatable.
- `--jobs N` runs N cases at once (default: the CPU count). `--json OUT` writes
  every result, the per-suite tallies and the failures. `--keep` keeps each
  case's build directory; `--verbose` prints notes and passing cases.
- The exit status is non-zero when any selected case fails or has a marker
  error.

CTest runs one test per top-level directory, `cases-<suite>`, against the build
tree's binaries (`ctest -L cases`, or `ctest -R cases-soundness`), plus the
runner's own tests (`cases-runner-harness`). The cache variable
`WEAVEC_CASES_ARGS` adds runner options to every suite, such as `--no-run` for
the ASan CI job.

## Markers

Markers are `//` comments. A *file marker* is on a comment line before the
first declaration (preprocessor lines may come first); a *line marker* is at
the end of the code line it applies to. One comment can hold several markers
separated by `//`:

```c
// RFC 0017: added regression pair.      <- prose: not a marker
// FLAGS: -std=c11
// RUN-INPUT: 1
#include <stdlib.h>
...
  p[n] = 0; // BUG: out-of-bounds // TRAP: index
```

| Marker | Kind | Meaning |
| --- | --- | --- |
| `CLEAN` | file | no errors, no warnings (except `ALLOW`), no traps, and no death by any other signal |
| `ALLOW: <id> [<id> ...]` | file | warnings with these ids do not fail `CLEAN`; justify each in a comment |
| `BUG: <id> [definite\|possible]` | line | a diagnostic with `<id>` on this line; `definite` = error, `possible` = warning, omitted = either |
| `TRAP: <template>` | line | the program traps here with `nonnull`, `index`, `span`, `len`, `disjoint`, `assert` or `violation` |
| `RUN-INPUT: <argv...> [< <file>]` | file | run the program with these arguments (shell quoting; the input file is relative to the case); repeatable, each run independent; an empty `RUN-INPUT:` is a run without arguments |
| `UNRESOLVED: <facet>:<reason>` | line | a ledger row here has that facet unresolved with that reason (§2.3) |
| `TRUSTED: <facet>:<reason>` | line | likewise, trusted (§2.4) |
| `NOT-PROVEN: <facet>` | line | a ledger row here has that facet, and none has it proven |
| `NEUTRALISED: zero-init` | line | the defect is defined away by zero-initialisation (§11) |
| `MISS: <reason text>` | line | a known miss: counted in the denominator, expected silent |
| `EXPECT-LEDGER: <json-pointer> <op> <value>` | file | e.g. `/summary/unresolved <= 3`; `op` is `==` `!=` `<=` `>=` `<` `>`; the value is JSON, or a bare string |
| `FLAGS: <weavec-cc flags>` | file | extra flags for every compile and link (repeatable) |
| `UNITS: <file.c> [<file.c> ...]` | file | further translation units linked with this one, relative to the case |
| `ASAN` | file | also run the ASan oracle, and require it to report the bug |
| `TOOL` | file | analyse with `weavec --ledger` (no build, no run) |

Facets are `spatial`, `null`, `temporal` and `assertion`; reasons are the
spellings of RFC 0030 §2.3–2.4. Ids are those of v0.10.0 and RFC 0030, so a pin
converted from the golden run may name a removed id.

Rules the grammar leaves implicit, as the runner enforces them:

- The marker keywords are reserved at the start of a comment segment. A
  keyword used wrongly (`// CLEAN please`, `// BUG leak`), an unknown id,
  template, facet or reason, and a near-miss keyword (`BUGS:`, `UNIT:`) are
  *marker errors*: the case is reported as `ERROR` and fails the run. Other
  uppercase prefixes (`NOTE:`, `RFC 0017:`) are prose.
- A line marker on a comment-only line, or a file marker on a code line or
  after the first declaration, is a marker error.
- A unit named by `UNITS` is not a case of its own, and neither is any file
  under an `Inputs/` directory. A unit's own line markers apply to its lines,
  and its own `FLAGS` apply to its compile only; any other file marker in a
  unit is a marker error. A unit whose `FLAGS` contain `-fno-weavec` is
  compiled as plain Clang: the link sees an input without a WeaveC record, and
  the analysis (and `--legacy`) does not see its code. Markers are read from
  the case file and its units only, never from headers.
- A case needs at least one expectation (`CLEAN`, `EXPECT-LEDGER` or a line
  marker). `CLEAN` excludes `BUG`, `MISS`, `NEUTRALISED` and `TRAP`; `ALLOW`
  needs `CLEAN`; `ASAN` and `RUN-INPUT` need a unit that defines `main`;
  `TOOL` excludes both.

## How a case is judged

1. **Build.** Each unit is compiled with
   `weavec-cc -c -fweavec-checks=trap|verify [-fweavec-require=...] <FLAGS> -fweavec-ledger=<tmp>/`
   and the units are linked into `a.out` when one defines `main` (the same flags
   at link). Without `main`, several analysed units are also given to
   `weavec --whole-program --ledger=...`. `TOOL` cases run
   `weavec --ledger=... [--whole-program] <units> -- <compiler flags>`, with the
   weavec-cc flags translated (`-fweavec-require=` to `--require=`, `-W...weavec...`
   before the sources, other flags after `--`). Compiles and links time out
   after 120 s. A compiler crash, a timeout, a link error, a Clang error or an
   error exit without a diagnostic fails the case.
2. **Diagnostics.** `file:line:col: error|warning: ... [weavec::<id>]` lines,
   from every compile and link, deduplicated. Every `BUG` must be *satisfied*
   (below). A `CLEAN` case fails on any error and on any warning whose id is not
   in `ALLOW`. Any case fails on an error on a line that has no `BUG`, `MISS` or
   `NEUTRALISED` marker, including an error without a location.
3. **Ledger.** `UNRESOLVED`, `TRUSTED` and `NOT-PROVEN` are checked against the
   rows at their line: the merged facet or any of its `requirements` records
   for the first two. The program ledger is used when the link wrote one,
   otherwise the unit ledgers. `EXPECT-LEDGER` reads the program ledger, else
   the main unit's. A case with ledger markers fails when no ledger was written.
4. **Run**, when the build produced `a.out` (not with `--no-run` or
   `--no-emission`). The trap-mode binary runs once per `RUN-INPUT` (stdin is
   `/dev/null` unless redirected), with a 10 s timeout. Every run must end by
   `SIGTRAP` or `SIGILL` if and only if the case has a `TRAP` marker. The units
   are then rebuilt with `-fweavec-checks=report`, the runs repeated, and every
   `weavec: runtime check failed: <template> at <file>:<line>:<col>` line
   collected: each `TRAP` must be matched by line and template, and a failure on
   any other line fails the case. When no run is possible (`TOOL`,
   `--no-run`, `--no-emission`, no `main`), a `TRAP` is matched instead by a
   facet at its line carrying a check whose `check.template` is the template —
   `checked`, or `violation` for a lowered definite violation (§3.4); when the
   build stopped at an error, a `TRAP` on the line of a `BUG` satisfied by that
   error is not required.
5. **ASan oracle** (`--asan` or `ASAN`). All units are built with
   `weavec-cc -fno-weavec -fsanitize=address -fsanitize=array-bounds -g -O0`
   (plain Clang with `weavec.h`; `array-bounds` catches the static-array
   indices whose neighbours ASan cannot see) and run with
   `detect_stack_use_after_return=1`. A `CLEAN` case fails on any report. A bug
   case with `ASAN` fails without one; with `--asan` alone a missing report is
   only a note. The report's first frame in a case file is the bug site: if
   the ledger has the matching facet (below) *proven* there, the case fails
   (gate G4), unless an enclosing in-case frame's line has it non-proven,
   because a static callee's accesses are proven by the check or violation at
   its call (§7.5).
6. **Verify** (`--checks verify`). The build uses `-fweavec-checks=verify`
   (with `-g`). A run that traps in verify mode while its report-mode run
   reports no failed check is a `weavec.proven` trap and fails the case,
   whatever its markers — but only when a `__weavec_prv_*` check could have
   fired. Programs trap on their own too: macOS's libmalloc traps on a real
   double free and `_FORTIFY_SOURCE` on an overflow, both with `SIGTRAP`. Two
   facts rule that in: the ledger's `summary.verifyChecks`, which is 0 when
   the build planned no check of a proven facet, and whether the report-mode
   run, which has no such check, trapped the same way. Either one makes it the
   program's own trap, which step 4 already reports as a note. When the
   report-mode run does report a failed check, `--lldb` asks `lldb` for the
   trap's category (`weavec.proven` or `weavec`); without it such a trap is
   taken as the unproven check's.

A `BUG` marker is satisfied by, in order:

- a diagnostic with its id on its line and the right severity class;
- a matched `TRAP` on the same line whose template enforces the id's facet
  (null: `nonnull`; spatial: `index`, `span`, `len`, `disjoint`; assertion:
  `assert`) or is `violation` (a lowered violation, any facet);
- with `--no-emission`, `TOOL` or `--no-run`, a matching facet on the line that
  carries a check, for null and spatial ids (gate G3's rule): `checked`, or
  `violation` for a definite violation §3.4 lowered and still guarded;
- a matched `UNRESOLVED`, `TRUSTED` or `NOT-PROVEN` marker on the same line:
  the author accepts a ledger row as the report of this bug;
- a `NEUTRALISED` marker on the line, when zero-initialisation is in effect
  (not with `--no-emission`, `--legacy`, `-fno-weavec-zero-init` or
  `-fweavec-checks=none`); otherwise it counts as `MISS`;
- a `MISS` marker on the line (expected silent; a report is noted as an
  improvement, not a failure).

The *matching facet* of an id (§17.3): temporal for `use-after-free`,
`double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`,
`mismatched-release` and `annotation-mismatch`; null for `null-dereference` and
`use-of-uninitialized`; spatial for `out-of-bounds` and `invalid-release`;
assertion for `contradicted-assumption`.

### Classes and tallies

Every bug case (a case with `BUG`, `MISS` or `NEUTRALISED` markers) gets the
strongest class observed at any of its bug lines: `error` or `warning` (a
diagnostic with the id, whatever the marker's severity), `trap` (a failed check
at the line whose template enforces the id's facet, from the report-mode run),
`row` (the matching facet is not proven there, or a ledger marker matched),
`neutralised`, `miss`, or `silent`. Gate G4 counts these. For each suite the
runner prints and writes the cases passed, the bug cases passed, the pins (`BUG`
markers) satisfied and those reported at any severity (a diagnostic with the id,
a matched trap, or under `--no-emission` a checked facet: gate G3's count), the
class tallies, the clean cases passed and, for gate G5, the clean cases that
built without an error.

## Legacy mode and the golden comparison

`--legacy` applies the markers with v0.10.0 semantics, using the golden `weavec`
tool as the retired `scripts/evaluate.py` ran it:

```sh
weavec [--whole-program] <main> [<units>] -- -ferror-limit=0 -fno-color-diagnostics <flags>
```

`--whole-program` is used when the case has several analysed units (units with
`-fno-weavec` are left out). `FLAGS` are translated for the tool:
`-fweavec-strict`, `-fweavec-exclusive-borrows`, `-fweavec-report-unannotated`,
`-fweavec-analyze-headers`, `-fweavec-dump-analysis`, `-fweavec-checked*` and
`-fweavec-analysis-*` become the tool's options, `-W...weavec...` flags go before
the sources, flags that v0.10.0 does not have (`-fweavec-checks=`,
`-fweavec-require=`, `-fweavec-ledger=`, zero-init, budget, summary) are dropped
with a note, and the rest go after `--`. There is no ledger, no run and no ASan:
`TRAP` and the ledger markers are ignored, `NEUTRALISED` counts as `MISS`, and a
null or spatial `BUG` needs a diagnostic. No `-I` is added for `weavec.h`:
each binary reads the header it was built with (`WEAVEC_RESOURCE_DIR`, or the
checkout it was built from), so golden binaries built as `GOLDEN.md` describes
keep reading v0.10.0's header while this branch changes its own.

Legacy mode also classifies every bug case as v0.10.0 was measured, from the
diagnostics located in the case's own units:

| Class | Rule, applied in this order |
| --- | --- |
| `CAUGHT` | some `BUG` marker's id is reported on its line (any severity) |
| `MISLABEL` | some other diagnostic has an id other than `leak`, `analysis-incomplete`, `annotation-required`, `checking-incomplete` and `checking-failed` (including the matching id on another line) |
| `SIGNAL` | some diagnostic is `analysis-incomplete`, `annotation-required`, `checking-incomplete` or `checking-failed` |
| `LEAK-ONLY` | the only diagnostics are `leak` |
| `SILENT` | no diagnostic |

On `soundness/` this reproduces the measured 36 CAUGHT, 42 SILENT, 4 SIGNAL,
2 MISLABEL and 1 LEAK-ONLY, probe by probe, and 24 of the 28 twins clean.
Legacy runs of `soundness/` and `repros/` fail by design: they pin what v0.10.0
misses or gets wrong.

`--compare-golden` (the S1 gate) runs the binary under test and the golden
`weavec` in that legacy mode on every selected case and fails on any difference
between their sorted `(file, line, column, severity, id, message)` diagnostic
lists, printing the first differences (`- golden:` and `+ tested:` lines).
Marker results are computed and tallied but do not decide the exit status. A
case whose flags translate to a tool option that the binary under test rejects
(a flag RFC 0030 removed) is skipped and reported as excluded; such cases belong
in the *Excluded* section of `KNOWN-DIFFERENCES.md`.

## Adding a case

1. Put it in the directory of its feature (`semantics/<feature>/` for new
   behaviour), named for what it tests; there is no manifest to update.
2. Start with a comment saying what the case is about and where it comes from,
   then the file markers, then the code.
3. Pin every expected finding with `BUG` on the line that must be reported, and
   every expected trap with `TRAP`; mark a correct program `CLEAN`. Give a
   null or spatial bug that becomes a check a `main` that reaches it (with
   `RUN-INPUT` if it needs arguments), so the executable oracle observes the
   trap, and add `ASAN` when ASan can confirm the bug.
4. Run `scripts/run-cases.py --filter '<path>' --asan -v` and, for a change of
   behaviour since v0.10.0, `--legacy` to see what the golden build said.
