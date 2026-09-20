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

In S0, `run-cases.py --legacy --filter 'engine/**'` reproduced every pin.
No pin is currently listed as a miss.

## Converted pins

RFC 0030 removes `analysis-incomplete` and `annotation-required`
(*Diagnostics*, *Removed ids*). What the engine cannot model becomes an
`unresolved` row whose reason §15 item 3 maps from the old text (`budget` for
a limit, `raw-cast` for a reinterpretation, `inexpressible` for
"unrepresentable …", `unanalysed` otherwise), and a call into unknown code
becomes `unresolved(unknown-callee)` (§5.1). No diagnostic can reproduce these
ten pins, so in S3 their `BUG` markers were replaced by `UNRESOLVED` markers
for the rows that replace them. They are no longer pins: G3's denominator is
the remaining 141 pins, and these lines are checked as ledger rows instead.
They are listed for traceability and do not count toward the 25 entries.

| Case and line | Golden | Now |
| --- | --- | --- |
| `engine/Analysis-rfc0006-elements.c:60` | `analysis-incomplete`, warning | `a[0]` temporal `unresolved(unanalysed)`: array cleanup membership is unresolved |
| `engine/Analysis-rfc0014-pointer-identity.c:33` | `analysis-incomplete`, warning | `memcpy` temporal `unresolved(raw-cast)`: unsupported memory copy of pointer-containing storage |
| `engine/Analysis-rfc0014-pointer-identity.c:43` | `analysis-incomplete`, warning | `release_field(p)` temporal `unresolved(raw-cast)`: incompatible or unknown object view at call |
| `engine/Analysis-rfc0016-boundaries.c:10` | `analysis-incomplete`, warning | the call's temporal `unresolved(budget)`: call context relationship limit reached |
| `engine/Analysis-rfc0016-boundaries.c:21` | `analysis-incomplete`, warning | the call's temporal `unresolved(budget)`: call context input path limit reached |
| `engine/Analysis-rfc0016-boundaries.c:27` | `analysis-incomplete`, warning | the call's temporal `unresolved(unanalysed)`: unresolved call alias relationship |
| `engine/Analysis-rfc0016-boundaries.c:33` | `analysis-incomplete`, warning | the call's temporal `unresolved(inexpressible)`: unrepresentable call context input path |
| `engine/Analysis-rfc0016-context-limits.c:6` | `analysis-incomplete`, warning | nothing on this line: the limit is reached inside the context run of `recurse(p, p, 20)` (line 11), which decides no rows (§2.6); that call's temporal facet is `unresolved(budget)` |
| `engine/Analysis-rfc0003-wrappers.c:70` | `annotation-required`, warning | the call's temporal `unresolved(unknown-callee)` (§5.1) |
| `engine/Analysis-rfc0003-wrappers.c:71` | `annotation-required`, warning | the call's temporal `unresolved(unknown-callee)` (§5.1) |

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
| `test/Driver/headers-analysed.c` (was `headers-skipped.c`) | `--analyze-headers`, inverted in S3 |
| `test/Driver/version.c` | `--help` lists `--report-unannotated` and `--analyze-headers` |
| `test/Driver/rfc0005-flags.c` | warning control over `annotation-required`, which S3 removes |
