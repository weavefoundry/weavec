# RFC 0024 validation

Validation is complete for [RFC 0024](rfcs/0024-checked-runtime-contracts.md).
This document distinguishes the frozen acceptance population from supplemental
clients and whole-corpus conditional contracts. A complete function contract
retains its stated entry requirements; it is not a certificate for an entire
library or for arbitrary callers.

The [machine-readable results](../scripts/corpus/rfc0024-results.json) contain
source and executable hashes, frozen inventories, exact function identities,
individual observations, diagnostic review and retained development failures.

## Scope

Runtime models check initialized comparison/search inputs, borrowed search
results, stream provenance, partial input, formatting, compiler intrinsics and
variadic forwarding. Core's bounded format parser has no Clang dependencies.
The Analysis layer validates target argument types and uses existing memory,
numeric, callback and summary machinery. No runtime instrumentation, annotation
spelling or diagnostic identifier is added.

New records use checked encoding 6, summary format 19 and sidecar format 20.
Older compiler sidecars require rebuilding. An external contract deferred during
compilation still requires whole-program analysis against its definition at
checked linking; deferral never supplies a completed proof.

## Populations and identities

The original 51 source cases, their expectations and their intended missing
properties were frozen before checker implementation. The preserved RFC 0023
Release executable has SHA-256
`d33d17867f8956d5e684e3540d74493f08ede5eec7c3547da3c112ae608b0828`.
It meets 16/51 original expectations and 2/8 supplemental transport expectations.
The five pinned projects have 1,619 selected definitions and 130 exact
baseline-complete identities. The identity list is retained in
[the evaluation directory](../test/evaluation/rfc0024/baseline-complete-functions.json).

Transport and unchanged-source clients were selected during implementation,
separately from the original source population. Their inventories record their
own hashes. The upstream clients include the complete, unchanged cJSON and
linenoise source files, verified against both their checkouts and pinned commits.
The four baseline upstream clients are all rejected. Supplemental include paths
and the linenoise caller's missing POSIX declaration were corrected before the
final runs; the earlier syntax failures remain development observations.

The harness requires a valid checked report and the intended property for each
negative. Syntax errors, crashes, timeouts, unrelated failures and additional
entry requirements do not satisfy an expected closed caller. Compiler-object
tests record whether rejection occurs during compilation or linking and require
an executable from an accepted checked link.

## Correctness validation

The frozen final-source Debug and ASan/UBSan builds each pass 1,171/1,171
CTest entries: 1,145 unit tests and 26 integration entries, including 128 lit
cases. Debug takes 69.08 seconds and the sanitizer suite 228.13 seconds.
No AddressSanitizer or UndefinedBehaviorSanitizer finding appears in the
sanitizer log. C++ and CMake formatting pass, and Core contains no Clang or
LLVM includes. Strict clang-tidy passes all 25 changed translation units with
verified source and header hashes. Uncached checked, cache and ordinary-mode
cost validation also pass on the same final Release executable.

| Population | Baseline expectations met | Final expectations met |
| --- | ---: | ---: |
| Original frozen source | 16/51 | 51/51 |
| Separate source transport | 2/8 | 8/8 |
| Compiler objects | Not measured separately | 8/8 |
| Cache invalidation | Not measured separately | 5/5 |
| Supplemental unchanged runtime clients | 0/4 | 3/4 |
| Prior traversal clients | 8/8 | 8/8 |
| Prior interface clients | 5/5 | 5/5 |
| Prior Jansson memory clients | 7/8 | 7/8 |
| Prior cJSON container clients | 8/8 | 8/8 |

The compiler-object population exercises compile-stage deferral and checked
linking against actual helper definitions. Cache tests cover unchanged report
equivalence, invalidation after changing returned-output behavior, corruption,
changed source behind a sidecar, and rejection of the older sidecar version.
Variadic representation lit cases analyze native macOS, x86-64 Linux and
AArch64 Linux targets, including C23 startup. They do not substitute for native
Linux CI execution.

## Corpus preservation and cost

The final uncached observation preserves every one of the 130 exact
baseline-complete identities and completes 12 additional conditional contracts.
All five projects produce valid checked coverage within the unchanged
600-second process limit. Lua retains explicit incomplete function coverage;
its ordinary corpus failure classification is preserved in the raw result and
is not treated as whole-program verification.

| Project | Selected definitions | Baseline complete | Final complete | Uncached seconds | Cold-cache seconds | Warm seconds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| log.c | 12 | 5 | 5 | 0.156 | 0.226 | 0.147 |
| cJSON | 151 | 29 | 32 | 16.305 | 18.365 | 0.810 |
| linenoise | 88 | 24 | 27 | 9.265 | 9.566 | 0.608 |
| Jansson | 211 | 22 | 24 | 61.607 | 62.761 | 2.528 |
| Lua | 1,157 | 50 | 54 | 595.754 | 594.779 | 22.041 |
| Total | 1,619 | 130 | 142 | 683.087 | 685.697 | 26.134 |

The gains are `cJSON_Version` and both `compare_double` definitions;
`linenoiseBeep`, `linenoiseClearScreen` and `shouldFoldText`; Jansson's
`dump_to_fd` and `fd_get_func`; and Lua's `makemask`, `readable`, `l_message`
and `print_version`. These contracts still require their stated inputs and
environmental premises. Lua's uncached and cold-cache timing margins are
narrow: 4.246 and 5.221 seconds below the limit on this host.

Uncached, cold-cache and warm reports have identical canonical contents for
each project. Warm reuse performs zero function analyses and records 51 cache
hits: 1 for log.c, 2 each for cJSON and linenoise, 12 for Jansson and 34 for Lua.
The warm compact report is 5.31 times smaller for cJSON, 5.18 for linenoise and
13.81 for Lua than the expanded cold report.

Ordinary-mode observations run sequentially, three baseline passes followed by
three final passes. Builds, tests, lint and profiling are stopped. Peak memory
is the largest child peak RSS among the five projects in each pass; time is
their summed child wall time. The host is ARM64 macOS with LLVM 23, using
Release `-O3 -DNDEBUG`, warnings as errors and LTO disabled.

| Repetition | Baseline seconds | Final seconds | Baseline peak MB | Final peak MB |
| --- | ---: | ---: | ---: | ---: |
| 1 | 162.081 | 165.198 | 651.297 | 652.263 |
| 2 | 167.727 | 164.271 | 663.634 | 650.134 |
| 3 | 183.750 | 171.562 | 659.653 | 651.969 |
| Median | 167.727 | 165.198 | 659.653 | 651.969 |

The final/baseline median ratios are **0.9849 for time** and **0.9884 for
peak RSS**, both below the unchanged 1.10 ceiling. Individual observations and
host-load snapshots retain the measured variation.

## Diagnostic comparison

Ordinary diagnostics are identical between baseline and final, and across all
three repetitions of each executable: 4,068 unique diagnostic records.
Checked diagnostics are identical across final uncached, cold-cache and warm
runs. The checked baseline comes from the historical RFC 0023 observation with
the exact preserved executable and corpus manifest; it is not a fresh timing
observation.

| Project | Baseline checked | Final checked | Added | Removed |
| --- | ---: | ---: | ---: | ---: |
| cJSON | 4,147 | 4,260 | 168 | 55 |
| Jansson | 7,404 | 7,577 | 468 | 295 |
| linenoise | 1,788 | 1,870 | 170 | 88 |
| log.c | 51 | 47 | 10 | 14 |
| Lua | 70,971 | 73,386 | 2,711 | 296 |

All 3,527 additions are `checking-incomplete`: explicit runtime, formatting,
stream and argument-list premises, plus propagated existing representation and
analysis limits. Of the 748 removals, 747 are `checking-incomplete`, including
unavailable-call boundaries and local-initialization placeholders handled by
the new models. No `checking-failed` diagnostic is added or removed. The
[full diagnostic changes](../scripts/corpus/rfc0024-diagnostics.jsonl) preserve
each record; the results JSON groups every reason and records the review.

The one other removal is Lua's checked-mode `null-dereference` at
`lstrlib.c:1197`, passing `buff` to `quotefloat`. The final callee exports an
explicit valid-buffer requirement, while both `quotefloat` and its caller
`addliteral` remain incomplete; the latter retains its 2,048-obligation limit.
This disappearance is not counted as a newly proven caller or a demonstrated
false-positive fix. Its baseline/final state is recorded separately.

## Development findings

Early tests exposed fortified argument positions, unsigned return guards and
variadic forwarding issues. An initialized terminated prefix needed a separate
representation from a fully initialized capacity; merging those facts would
incorrectly prove reads of an unwritten tail. Output facts now retain their
result and input dependencies and are invalidated when those dependencies change.

Adversarial review found that raw list writes could preserve an active cursor
and nested scope exit could lose its cleanup obligation. Writes now retire
cursor evidence while preserving the outstanding `va_end` obligation. Scope
exit checks cleanup before forgetting storage. Raw copies do not establish an
active cursor, and unknown escape or mutation retires existing evidence.

Further checks cover format overlap with unknown string lengths, the promoted
unsigned value used when bounding integer output, malformed `va_start` anchors,
empty forwarded literals and compiler object-size bounds. Constant-capacity
formatting also establishes the prefix selected by the successful returned
count, with separate guards for truncation and nontruncation. Negative results
and writes to the result variable cannot establish successful initialization.

The added escape handling initially conflicted with the ordinary library table's
opaque `va_list` placeholder. A valid formatter could consume a cursor and then
have it classified as retained by that fallback, causing `va_end` to fail.
Validated runtime cursor operands now use the runtime consumption rule. The
failed run is retained, and the final builds recompile changed C++ sources after
the last edit before rerunning validation.

The first sequential checked cost observation completes the four smaller
projects but times out on Lua at 600.149 seconds without a final report. It is
retained under `build/rfc24-validation/pre-cost-optimization/`, together with
the executable, source hashes and correctness results for that candidate.
Guard refinement now checks already-decisive scalar facts before reconstructing
integer ranges or scanning alias relations. The fallback and the 600-second
gate are unchanged.

A later observation was converted to a five-second stack profile and interrupted;
it is retained separately under `profile-after-scalar-fast-path/` and excluded
from cost signoff. The completed smaller-project reports matched the first
candidate canonically. The profile showed repeated call-specialization,
contract-merging and explanation costs beyond guard refinement. The bounded
explanation cache now reuses complete call ledgers and their prepared rows.
Near the obligation cap, original insertion order and capacity decisions remain.
The Core oracles compare exact explanations, trust and failure status against
individual insertion, including capped and mutated ledgers. Final validation
of these optimizations includes both full correctness suites and strict lint.

The next unprofiled run completes the smaller projects in 0.162, 16.653,
9.870 and 65.677 seconds, with the same canonical reports. Lua reaches
600.359 seconds while writing a temporary report; it has no final report and
fails the unchanged gate. That observation is preserved under
`checked-timeout-after-call-ledgers/`. A separate 1,200-second diagnostic
process deadline allows a full report to be collected and a five-second stack
profile to be taken; it does not change the 600-second acceptance limit. The
profile identifies duplicate explanation-key construction during preparation.
Preparation now retains the exact bounded insertion keys, and JSON identity
components append directly to their destination string. Individual-insertion
oracles still define the expected ledger contents. A further observation
times out at 600.516 seconds while writing its report; its retained corpus
result remains a failure. Final rendering now reuses immutable expanded call
paths within a 1,024-entry / 8 MiB write-local cache. Field-by-field JSON tests
cover reuse, eviction, escaping and report replacement.

A later adversarial check finds `fclose(stdout); puts(...)` accepted under a
release entry condition. That condition cannot repair the closed stream.
Implicit output now carries a named standard-stream requirement through
helpers, callbacks, source units and objects, including headerless helper
declarations. Known closure, null replacement and unknown mutation prevent
discharge. The original primary population remains frozen; these are separate
regressions.

## Remaining limits

The initial cursor model covers local variables and incoming parameters, with
pointer, record and single-element-array target representations. Record-embedded
lists, direct `va_arg`, escaping cursors, positional and wide formats, `%n`,
unbound dynamic formats and unrepresented output bounds remain incomplete.
An unbounded formatter needs a sufficient upper output bound. A bounded
terminated-prefix fact alone does not initialize the entire capacity.

Three of the four supplemental upstream clients complete: cJSON's unchanged
`compare_double`, `linenoiseClearScreen` and `linenoiseBeep`. The closed string
client of `cJSON_Compare` remains incomplete because recursive object/container
paths and their memory premises are not eliminated by its caller's tag values.
Its expected acceptance is retained as a visible miss. The supported scalar
comparison and output contracts do not imply that all of cJSON or linenoise is
verified.

The older Jansson memory population retains its existing `strbuffer-lifecycle`
miss: the append interfaces still have incomplete contracts. Other previously
passing unchanged-source populations are tracked independently.

## Reproduction

The [evaluation notes](../test/evaluation/rfc0024/README.md) give source,
transport, compiler-object, cache and upstream commands. CTest includes the
source, transport, object and cache populations, unit regressions and lit tests.
The original frozen fixtures are not rewritten to accommodate implementation
results.

Corpus measurements use `scripts/scalability-evaluation.py` after builds, tests
and lint have stopped: one uncached checked observation, cold/warm checkpoint
observations, and three sequential ordinary observations of each executable.
The limits remain 600 seconds per checked project and 1.10 for the ordinary
median time and peak-RSS ratios. Canonical reports, all baseline-complete
identities, cache hits and function-analysis counters are checked separately.
Raw development logs and reports remain under `build/rfc24-validation/`.
