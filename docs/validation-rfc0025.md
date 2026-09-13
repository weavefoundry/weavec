# RFC 0025 validation

Validation for [RFC 0025](rfcs/0025-case-sensitive-checked-contracts.md) is
complete. The [machine-readable results](../scripts/corpus/rfc0025-results.json)
retain executable and source identities, frozen inventories, all acceptance
populations, cost observations, diagnostic review and development failures.
Raw logs and verified report archives remain under `build/rfc25-validation/`.

## Scope

The checker rechecks a helper's CFG under established caller facts and retains
that case's complete contract and premises. The independently selected generic
definition keeps its own result. New optional discovery targets incomplete
read-only helpers; existing memory-effect contexts retain their previous
eligibility. A complete generic contract is preferred because a bounded
recheck can lose an inductive output that the generic proof already supplies.

Named scalar and pointer union members carry evidence of which member was
initialized. A matching tag selects a branch but does not establish payload
initialization, pointer validity, ownership, bounds or initialized pointee bytes.
Overlapping writes retire old member facts, including writes through aliases,
raw byte views and helpers. Compatible complete copies and guarded joins retain
only independently established evidence. Helper requirements and output-member
guarantees compose through source units, compiler objects and checkpoints.

The initial scope excludes aggregate union members, anonymous member promotion,
bit-fields, volatile/atomic storage and arbitrary representation punning. Those
limits do not imply that the excluded constructs are invalid C. No annotation
spelling, diagnostic identifier or runtime instrumentation is added.

Checked encoding 7, summary format 20 and sidecar format 21 carry the new
records. Older sidecars require recompilation. Reports include `case_inputs`
and full `cases`, with canonical `premises`, alongside the generic contract.
Compact reports retain the same fields and proof content.

## Frozen populations and independent checks

The primary 38 cases and their expected outcomes were frozen before checker
implementation. The preserved baseline is `c04a43b` / v0.5.0, with Release
executable SHA-256
`47c7ee8b0174bb4fee7ed6059c5da1b97968f5c356d123f3e78d48532aa03a38`.
It meets 15/38 primary expectations and 5/10 separate-source expectations;
none of their intended accepted callers is accepted. An unrelated rejection
does not satisfy a negative's expected missing property.

Transport, adversarial and unchanged-source clients were added as separate
populations during implementation. Each inventory retains its own source
hashes. The upstream clients compile the complete, unchanged pinned cJSON
source, not extracted helper bodies. The baseline-complete inventory preserves
142 exact source/function identities from the preceding 1,619-definition corpus.
Equal totals cannot substitute for preserving those identities.

| Population | Final expectations met |
| --- | ---: |
| Primary source: 18 accepted / 20 rejected | 38/38 |
| Separate source: 5 accepted / 5 rejected | 10/10 |
| Compiler objects and checked linking | 10/10 |
| Supplemental member, alias, lifetime and guard cases | 25/25 |
| Checkpoint invalidation and compact-report cases | 5/5 |
| Complete-source cJSON clients: 3 accepted / 2 rejected | 5/5 |
| Prior traversal clients | 8/8 |
| Prior interface clients | 5/5 |
| Prior Jansson memory clients | 7/8 |
| Prior container clients | 8/8 |
| Prior runtime clients | 4/4 |

The three accepted upstream callers compare boolean, masked-tag and string
values. Their selected `main` contracts have zero entry requirements, no
unresolved obligations, no deferral and no exhausted limit. The uninitialized
and unterminated string callers remain rejected. Generic `cJSON_Compare` stays
incomplete; its successful conditional cases do not certify arbitrary recursive
object comparisons. The unchanged RFC 0024 runtime population now passes all
four callers, including its previously incomplete string comparison. The
earlier Jansson memory miss remains visible.

Supplemental unit and lit regressions reject a raw byte write and a helper
write to another member before the first union-member read. The checker must
not introduce a stale entry member requirement after either write. A positive
case confirms that a proved disjoint write preserves the input member. Other
unit tests cover freed pointers, whole-object replacement through an alias,
pointee initialization after a member copy, addressed pointer stores, guard
replacement and unreachable unsupported operations in a rechecked case.
Core includes an independent small-state branch/write oracle and malformed
and over-budget transport-record tests.

The original RFC 0020 source and expected output remain unchanged. A separate
RFC 0025 projection snapshot records the more precise call origin for
`diamond(1)`: the else-branch arithmetic operation is reached through
`middle(!n)` at column 48. The previous generic-only snapshot used column 36.
Other generic proof fields are preserved, and the new case records have
separate assertions.

## Builds and tooling

The final candidate uses Release SHA-256
`7494c72da73efa89b6dc6b9c30d4d794260413ea78228a3336ac962d76428b70`.
Release uses `-O3 -DNDEBUG`, with LTO disabled. All three builds use LLVM 23
and warnings as errors. The sanitizer build enables both AddressSanitizer and
UndefinedBehaviorSanitizer, including nonrecovering undefined-behavior checks.

| Configuration | CTest entries | Time |
| --- | ---: | ---: |
| Release | 1,199/1,199 | 45.71 s |
| Debug | 1,199/1,199 | 74.27 s |
| Debug with ASan/UBSan | 1,199/1,199 | 220.19 s |

Each suite contains 1,168 unit tests and 31 integration entries, including
130 lit cases. No sanitizer finding appears in the final log. Strict
clang-tidy passes all 23 changed C++ translation units after the last header
change. C++ and CMake formatting pass. Core has no Clang or LLVM includes.
These are native macOS/arm64 observations; native Linux CI has not been run
in this local task.

## Cost and checkpoint validation

All five projects produce valid checked reports within the unchanged
600-second process limit. All 142 baseline-complete identities are preserved;
six Lua contracts are newly complete, for 148/1,619 selected definitions.
The gains are `clearkey`, `nextrand`, `block_follow`, `finishnodeget`,
`keyinarray` and `rawfinishnodeset`. These are conditional contracts whose
entry requirements remain visible. Lua retains explicit incomplete function
coverage; its raw corpus failure classification is retained, and no missing
report or whole-program nonconvergence is counted as success.

| Project | Checked time | Complete contracts | Report encoding |
| --- | ---: | ---: | --- |
| log.c | 0.177 s | 5 | Expanded |
| cJSON | 25.175 s | 32 | Expanded |
| linenoise | 17.307 s | 27 | Expanded |
| Jansson | 105.672 s | 24 | Expanded |
| Lua | 528.608 s | 60 | Compact |

Cold and warm checkpoints preserve the uncached report's canonical proof
content for all five projects. All 51 warm translation-unit lookups hit, with
zero function analyses. Expanded-to-compact report reductions are 6.67 times
for cJSON and 8.85 times for linenoise, above the required five times.

| Project | Cold checkpoint | Warm checkpoint | Warm hits |
| --- | ---: | ---: | ---: |
| log.c | 0.188 s | 0.134 s | 1 |
| cJSON | 27.164 s | 1.198 s | 2 |
| linenoise | 17.871 s | 0.739 s | 2 |
| Jansson | 116.774 s | 3.407 s | 12 |
| Lua | 565.022 s | 23.727 s | 34 |

Ordinary mode passes the unchanged 1.10 median time and peak-RSS ratios.
Each observation runs the same five projects sequentially; time is their sum,
and peak RSS is the largest project measurement. Three baseline observations
precede three final observations. Builds, tests, lint and profiling are stopped
during cost measurements.

| Ordinary mode | Baseline | Final | Ratio |
| --- | ---: | ---: | ---: |
| Median time | 167.847 s | 178.624 s | 1.0642 |
| Median peak RSS | 629.672 MiB | 619.188 MiB | 0.9833 |

The three baseline times are 167.847, 179.426 and 164.881 seconds; the final
times are 178.624, 198.928 and 163.569 seconds. All observations have zero
corpus failures and zero Clang errors. The variation is retained rather than
selecting only the fastest runs.

Lua uses the existing compact report encoding in uncached, cold and warm
observations to bound disk use. cJSON and linenoise retain expanded cold
reports for the compact-size comparison. Every format is expanded into the
same canonical proof representation before comparison. The report format is
recorded in each command; historical expanded-report timings are not a
format-matched timing comparison with the new Lua observation.

Run the checked observations with:

```sh
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc25-release/bin/weavec --phase checked \
  --compact-project lua --archive-reports --output build/rfc25-checked
```

Use `--phase cache` and a fresh output directory for cold/warm validation.
Use `--phase ordinary --baseline PATH` for the three before/after observations.
Full reports and diagnostic sets are retained. Large reports are compressed
only after their decoded SHA-256 is verified; compression does not replace a
missing or failed result.

## Diagnostic review

All 4,068 ordinary diagnostic records are unchanged across the three baseline
and three final observations. Checked diagnostics are identical across final
uncached, cold and warm runs. The checked baseline is the historical RFC 0024
observation with the same preserved executable and corpus manifest; it is not
a fresh timing observation.

| Project | Baseline checked | Final checked | Added | Removed |
| --- | ---: | ---: | ---: | ---: |
| cJSON | 4,260 | 4,452 | 235 | 43 |
| Jansson | 7,577 | 8,712 | 1,294 | 159 |
| linenoise | 1,870 | 2,348 | 592 | 114 |
| log.c | 47 | 45 | 0 | 2 |
| Lua | 73,386 | 75,419 | 4,136 | 2,103 |

Of 6,257 additions, 6,010 are `checking-incomplete`, including 2,043 missing
compatible-member witnesses and 156 member requirements that cannot be
instantiated. Of 2,421 removals, 1,639 are `checking-incomplete`, including
774 blanket unsupported-construct messages. Candidate contexts, reachable
operations and independent generic-definition checks also change which
existing limits, callback boundaries and memory requirements are reported.
These counts are not defect-recall measurements.

The additions include 23 `checking-failed` records and 21 ordinary diagnostic
identifiers emitted during checked analysis: nine use-after-free, ten
double-free and two null-dereference records. Source review and baseline/final
report inspection cover their 12 containing functions. Every one remains
selected and incomplete in both versions. Several newly emitted errors were
already in the baseline generic ledger: the eight `p` use-after-free locations
in Jansson's `lex_scan_string`, and the `strlen(src)` null diagnostic in
`linenoiseEditHistoryNext`, retain their existing proof routes. These are
retained checker observations, not claims of newly demonstrated upstream bugs.

The one removed `checking-failed` record is pointer recovery at Jansson's
`pack_unpack.c:919`. `json_vunpack_ex` remains limited and incomplete, with
91 requirements and five propagated violations. Pointer-recovery diagnostics
at its wrapper call sites, lines 941 and 952, remain. This removal is not
claimed as a false-positive fix or proof of variadic unpacking. Lua's new
`fs->f->code` null diagnostic at `lcode.c:913` likewise belongs to the still
limited `discharge2reg`, whose 256 requirements and generic outcome counts
are unchanged.

The [full diagnostic changes](../scripts/corpus/rfc0025-diagnostics.jsonl)
preserve every added and removed record. The results JSON retains all 140
added and 87 removed reason groups, the review, and the 24 baseline/final
contract summaries. Complete-contract preservation is checked separately.

## Retained development failures

Three earlier candidates time out on checked Lua at 600.147, 600.084 and
600.159 seconds, respectively, before producing a final report. Their results
remain failures.
The implementation subsequently restricts new cases using all may-write
effects, preserves the original behavior for memory-effect contexts, avoids
redundant union work on ordinary variables and reuses computed write locations.
A separate five-second stack sample then
identified call-explanation preparation as a larger cost than union type
descriptions. On cache misses near the obligation limit, the final implementation
uses the existing ordered insertion path directly. An independent capacity
oracle confirms identical outcomes, routes, truncation and exhaustion. The
four completed non-Lua reports match the preceding candidate exactly in
expanded canonical content. The interrupted profiling run is excluded from
cost signoff.

An earlier candidate accepted member reads after raw or helper writes that
should have retired the required evidence. The new write-history checks and
supplemental counterexamples correct that behavior. Original frozen fixtures
and expected outcomes were not changed to obtain a pass.

An initial sanitizer discovery attempt on an earlier candidate exceeded the
GoogleTest listing timeout. Direct listing and a full retry succeeded, and the
final candidate passes a fresh full suite. The final tooling pipeline also
recorded a missing `clang-format` PATH entry; rerunning with the explicit LLVM
tool path passes. Neither infrastructure failure is reported as a successful
initial run.
