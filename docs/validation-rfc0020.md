# RFC 0020 validation

RFC 0020 is **Implemented**. All mandatory correctness, cold-run, warm-reuse
and ordinary-cost gates pass. The [recorded results](../scripts/corpus/rfc0020-results.json)
include every acceptance observation, executable and manifest identities,
work counts, report digests and diagnostic comparisons.

## Reproducibility

- Baseline commit: `b5805f5`.
- Preserved baseline Release executable SHA-256:
  `c667451d6f75cecb5ad83e76389af17075e91052649c01d266e906c611a37e4d`.
- Frozen incremental manifest SHA-256:
  `42384fb09aedc7549157e91ed5d5cf7681027679d777f95ec7ee055ffbee14ce`.
- Pinned five-project corpus manifest SHA-256:
  `879b548e563af06033ac6ca1ecb66ad691cfc0cfa8ce4048a0c1fdf5e8de9f1e`.

Projects and revisions remain those in
[`scripts/corpus/rfc0015.json`](../scripts/corpus/rfc0015.json). Measurements
use macOS 26.6.2 arm64, eight CPUs, 24 GiB memory, Clang/LLVM 23.1.0 and a
Release build with warnings as errors. The harness measures per-child peak
RSS. Quiet timing observations run sequentially with sleep prevented, after
builds, tests and profiling finish. No checking scope or semantic limit is
relaxed for a performance observation.

## Correctness checks

The final validated executable has SHA-256
`bb36714315dafd91508366ed04a3e3c5e5149a21e93b0f8941d599c027c031ce`.

| Check | Result |
| --- | --- |
| Debug CTest | 1,029/1,029; 46.64 seconds |
| ASan/UBSan CTest | 1,029/1,029; 101.87 seconds |
| Frozen incremental evaluation | 17/17 |
| Pinned Jansson interfaces | 8/8 |
| Relevant strict clang-tidy | Passed |
| Formatting, whitespace and Core include boundary | Passed |

CTest includes 111 lit cases, the original fixed ownership evaluation,
checked source and compiler transport populations, recall and corpus harness
checks. Concurrent correctness runs are not performance measurements. The direct
spatial-join reference tests pass, and all 22 analysis-dump fixtures produce
byte-for-byte identical dumps, diagnostics and exit codes before and after
the spatial-join, final-pass state-consumption and ordered alias-join changes.
Independent edge-map models pin union, directional intersection, every
mutation on copied relations and surviving rows after source destruction.

The incremental evaluation compares full expanded reports, exit status and
diagnostics with originating locations and call notes. It checks zero function
analyses on reusable warm units and an independent-unit hit after a leaf edit.
It covers missing and nested dependencies, callback/global facts, header edits
with preserved timestamps, conditional includes, compiler options, stale
objects, corruption, concurrent publication and output failures. A preserved
diamond-shaped explanation fixture pins the complete pre-optimization report.

Core tests cover shared-contract and row lifetimes, mutation of sole-owner
published snapshots, weak-index expiry, eviction and disabled scopes, retained
call paths, exact cap order, truncation and source replacement. Checkpoint
tests validate table references, enumerations, normalization, exhaustion,
canonical remaining metadata and oversized records before publishing facts.

## Checked cold and warm cost

All five projects finish with full reports within the mandatory 600-second
limit using the final executable above. Cold runs use a fresh cache and
expanded reports; warm runs use that cache and compact reports. Timings
include the requested report generation.

| Project | Cold seconds | Warm seconds | Complete / selected | Warm hits | Warm function analyses | Compact reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| log.c | 0.135 | 0.141 | 3/12 | 1 | 0 | 3.86× |
| cJSON | 6.950 | 0.807 | 18/151 | 2 | 0 | 6.65× |
| linenoise | 2.839 | 0.495 | 20/88 | 2 | 0 | 7.53× |
| Jansson | 45.002 | 2.382 | 18/211 | 12 | 0 | 10.13× |
| Lua | 367.665 | 35.268 | 40/1,157 | 34 | 0 | 15.32× |

Every final cold report is byte-for-byte identical to its previously reviewed
non-cache report. Every compact warm report has exactly the same canonical
expanded content as its cold report, including all metadata, ordered functions,
obligations, requirements, trust and limits. All 51 reusable units have zero
warm function analyses. A warm hit still validates inputs, parses, decodes
checkpoints and generates the report.

Jansson beats the 180-second secondary target. cJSON and linenoise take less
than half their RFC 0019 checked times (45.180 and 20.953 seconds). Their
compact reports exceed the fivefold size-reduction target. An earlier Lua
cache attempt had 32 hits and two analyses; it failed acceptance and exposed
raw imported type keys that required lossless encoding. The final run reuses
all 34 Lua units.

Lua's raw cold and warm corpus results each retain one function iteration-limit
failure. Both completed reports retain the originating limit, and every limited
function remains incomplete. This is valid reported coverage, not successful
checking of the whole program. No timeout is counted as coverage. Selected
completion sets are unchanged on all five projects.

Final cold peak RSS is 53.7 MiB for log.c, 239.3 MiB for cJSON, 181.8 MiB for
linenoise, 825.2 MiB for Jansson and 7,201.0 MiB for Lua. Full per-run memory
measurements, including warm runs, are in the recorded results.

Lua's expanded report is 2,754,090,574 bytes, SHA-256
`d0818f80802a87ff91e7562b226a3c83579d83a83bdef5394218317fad1433b5`.
Its verified gzip archive is 73,570,211 bytes. The streamed canonical expanded
report digest (`ordered-function-sha256-v1`) is
`7956e96c115e85b57eef66766ccf16ab7c5c8a703ebaa1a8b830bdfa7b405f0e`.
Archival compression is separate from compact-report size reduction.

### Review of scheduling and proof changes

Reverse-postorder scheduling reduces repeated transfers of partially joined
inputs. The acyclic work-bound test passes, along with existing loops, gotos,
frozen evaluations and 22 exact analysis-dump comparisons. All five reports
were compared against the completed pre-scheduling reports by originating
operation, property, outcome, route, requirements and completion.

That review found a reachable linked-list split being pruned when a null fact
travelled between fast and slow cursors that only might alias. Checked branch,
nullness and null-object facts now use proven aliases. A small reproducer pins
the reachable null dereference in unit and CLI/whole-program tests. cJSON loses
no direct originating operations after the correction; its split retains every
previous obligation and gains additional unresolved checks.

Jansson's five removed arithmetic warnings are the decimal-point clamp in
`jsonp_dtostr`: the join retains `decpt` in [-3, 16], making the subsequent
subtractions safe. A regression checks both safe arithmetic and a reachable
failing arm. Changed call-context/limit explanations at `load.c` lines 739,
776 and 850 and `pack_unpack.c` line 610 still retain unresolved call evidence.
Linenoise's obligations are unchanged; one limited function's requirements
differ. No function becomes newly complete or loses completion in any project.

All sixteen lost direct Lua row identities belong to functions at the unchanged
2,048-obligation cap before and after. They all remain limited and incomplete;
these capped reports do not establish complete operation coverage. The VM and
`arith` are among them. Changed entries and routes within those caps are retained
in the local comparison artifacts. No semantic limit or selected scope changes.

Earlier 600-second Lua timeouts and the 688.276-second extended investigation
failed acceptance and were used only to locate repeated work. They are preserved
locally, including archived partial reports with verified decoded checksums.
Only the completed current observations above satisfy the cold gate.

## Ordinary cost

All three final ordinary observations pass execution validity and the fixed
10% time/RSS growth limits. Both binaries analyze the same five pinned projects.

| Observation | Baseline seconds | Final seconds | Baseline peak bytes | Final peak bytes |
| --- | ---: | ---: | ---: | ---: |
| 1 | 298.163 | 169.643 | 698,433,536 | 664,109,056 |
| 2 | 297.995 | 170.077 | 698,122,240 | 661,946,368 |
| 3 | 297.545 | 169.563 | 698,105,856 | 670,826,496 |
| Median | 297.995 | 169.643 | 698,122,240 | 664,109,056 |

Median time falls **43.1%** and peak RSS falls **4.9%**. Every project has
exactly the same originating diagnostic location, severity, ID and message
set as the baseline, repeated identically across all three observations.

All three clean observations of the unchanged preserved baseline are retained.
A subsequent fresh baseline observation reproduced its cost: Lua completed
in 291.967 seconds at 699,744,256 bytes. A historical candidate memory probe
failed to reproduce and is not acceptance evidence. Final observations ran
sequentially after all builds, tests and profiling finished.

Applying reverse postorder to ordinary Lua introduced a VM function iteration
limit; that speedup was rejected. Ordinary mode retains FIFO with the original
visit limit. Copy-layout experiments also failed the memory gate. Profiling
then exposed old and replacement program databases alive together, plus
duplicate completed unit exports. Releasing those inactive copies before
constructing replacements resolves the regression without losing active facts.

## Issues found and fixed during validation

- Whole-program rebuilding kept old and new databases alive together; the
  reporting pass also duplicated completed unit exports. Releasing these
  inactive copies before their replacements resolved the ordinary memory
  regression in all three final observations. Active imports keep their
  lifetimes.

- Raw indirect-call type spellings contain spaces and were rejected as
  synthetic function names in imported-fact metadata. Lossless byte encoding
  in separate lookup namespaces preserves those facts, including source paths
  with spaces. Unit tests cover changed inputs and independent-unit warm reuse.

- Large checked contracts duplicated obligation strings and paths across
  contexts. Immutable rows, ledger snapshots and bounded weak indexes share
  storage while preserving ordered identities, witnesses and semantic caps.
  Callee-specific origin preparation remains bounded and caller-independent.
- Persistent records were too large in expanded form. Private checkpoint
  format 2 shares obligation rows and ledgers, validates all references and
  retains separate encoded/decoded bounds. Optional zstd further reduces
  disk use; a rejected or unavailable optimization remains a cache miss.
- Summary parsing rejected standalone widened `Inside` offsets (`@~`).
  Return values, stores, heap fields and bounds/release transport now round-trip.
  All twelve pinned Jansson unit records round-trip canonically.
- Conditional include availability was absent from compiler object bindings.
  Sidecar version 16 binds preprocessing outcomes before AST creation and
  verifies them afterwards; stale or unreproducible checked objects fail.
- Retaining every ordinary AST increased memory use. A one-unit bound after
  selection discovery releases preparation before ASTs, while checked and
  cache-bound invocations preserve input lifetimes. ASan exposed poisoned
  vector capacity reused by uninstrumented Clang; eviction now releases that
  backing allocation as well.
- Published summary references could be invalidated by nested analysis.
  Active calls now own immutable contracts through invalidation and source
  store destruction, with lifetime regression tests.

Raw development observations, failure logs, profiles and archived reports
are retained locally under `build/rfc20-validation/`. They are generated
artifacts and are not committed. Acceptance measurements are published in
[`scripts/corpus/rfc0020-results.json`](../scripts/corpus/rfc0020-results.json).
