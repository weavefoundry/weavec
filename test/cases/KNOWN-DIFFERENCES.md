# Known differences from the golden run

`engine/` holds v0.10.0's lit engine pins, reduced to (line, id) from the
golden run (RFC 0030 §17.2, `GOLDEN.md`). Gate G3 requires at least 95% of
them to be reproduced at any severity. A pin whose golden message said a
pointer "may be null", or an access "may be" out of bounds, counts as
reproduced when the matching facet at that line is *checked*.

Every pin that is not reproduced gets an entry below. The list may hold at
most 25 entries. Each entry gives:

- the case and line;
- the golden id and severity;
- what the current run reports instead;
- why the difference is intended, with the RFC section that decides it.

## Entries

None. In S0, `run-cases.py --legacy --filter 'engine/**'` reproduces every
pin.

## Excluded

These 11 lit files test flags that RFC 0030 removes or replaces (§17.6).
They stay in lit and are rewritten in S3. They are outside G3's
denominator, and once S3 removes the flags they are outside
`--compare-golden` too. They do not count toward the 25 entries.

| File | Flag |
| --- | --- |
| `test/Analysis/rfc0002-borrows.c` | `--exclusive-borrows` |
| `test/Analysis/rfc0004-function-pointers.c` | `--strict-externs` |
| `test/Analysis/rfc0004-posix.c` | `--strict-externs` |
| `test/Analysis/rfc0016-clean.c` | `--strict-externs` |
| `test/Annotations/ownership-annotations.c` | `--report-unannotated` |
| `test/Annotations/rfc0003-unknown-extern.c` | `--strict-externs` |
| `test/WholeProgram/rfc0016-composition.c` | `--strict-externs` |
| `test/WholeProgram/rfc0017-numeric.c` | `--strict-externs` |
| `test/Driver/headers-skipped.c` | `--analyze-headers` (becomes `headers-analysed.c`) |
| `test/Driver/version.c` | `--help` lists `--report-unannotated` and `--analyze-headers` |
| `test/Driver/rfc0005-flags.c` | warning control over `annotation-required`, which S3 removes |
