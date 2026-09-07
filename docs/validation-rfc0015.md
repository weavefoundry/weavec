# RFC 0015 validation

Validated on 2026-09-07 against baseline commit
`cafee09054b59b8549b4fb49150715cd6b4d899e`. The RFC was drafted and accepted
before implementation, under the owner's instruction to complete both.

## Correctness and build checks

- **699/699 CTest entries pass** in Debug with warnings as errors.
- **699/699 pass with ASan and UBSan**, including whole-program analysis,
  compiler sidecars, the driver, recall and fixed evaluation. LeakSanitizer
  is disabled on this macOS run; address and undefined-behavior checks are
  active. The sanitizer build uses `-O0 -gline-tables-only` to fit the local
  disk budget. Its final complete run takes 139.83 seconds.
- The CTest integration entry includes **89/89 lit tests**. The new tests
  pin selected-element errors, incomplete-coverage messages, format 11
  records, cross-file inference and the compiler's serialized link analysis.
- **49 array Analysis tests, 12 Core tests and one sidecar test** are added.
  The existing recall set retains **67/67 pinned detections**, with no
  unexpected reports or parse failures across its 33 source cases.
- Clang-tidy passes on the changed implementations and new tests, including
  the final cleanup, copy, range and fill changes. Clang-format, cmake-format
  and whitespace checks pass. Core has no Clang/LLVM includes.

The feature matrix covers independent and aliased cells, saved and rewritten
indices, initialization and nullness, selected callback targets, retained
shares, nested arrays, pointer-bearing records, simultaneous copies and
overlapping moves in both directions. It also exercises symbolic count
snapshots, final helper outputs, returned containers, reallocation success
and failure, lost children on shrinking, fill/cleanup loops, partial copies,
unknown selections, representation limits, malformed interfaces and lost
global guards. Clean counterparts distinguish independent storage from copied
pointee identity. A cleanup proof rejects early exits, side-effecting array
bases and stores to different cells.

## Fixed evaluation

Both revisions run the same unfiltered
[52-program manifest](../test/evaluation/manifest.json). The twelve new
bug/clean pairs reduce release history, rewritten indices, pointer and record
copies, memmove overlap, symbolic lengths, initialization, fill and cleanup,
returned arrays, linenoise-style compaction and Jansson-style table growth.

| Measurement | Before | After |
| --- | ---: | ---: |
| Seeded bugs detected | 18/32 | 30/32 |
| Clean programs accepted without reports | 14/20 | 20/20 |
| Unexpected reports | 20 | 0 |
| Clang parse failures | 0 | 0 |
| Tool failures / timeouts | 0 / 0 | 0 / 0 |

The two remaining size-analysis misses, `product-known-miss` and
`vla-known-miss`, remain in the denominator. This selected feature evaluation
does not estimate recall on arbitrary C code. Corpus warnings below are not
counted as independently confirmed bugs.

## Reproducing the measurements

Use LLVM/Clang 23.1.0 and CMake Release (`-O3 -DNDEBUG`) for both revisions.
The baseline executable is built from an isolated checkout of the commit
above. The implementation executable is built from the RFC 0015 working tree.

```sh
python3 scripts/evaluate.py --weavec build/rfc15-before/bin/weavec --json /tmp/evaluation-before.json
python3 scripts/evaluate.py --weavec build/rel/bin/weavec --json /tmp/evaluation-after.json
python3 scripts/corpus.py --weavec build/rfc15-before/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-before.json
python3 scripts/corpus.py --weavec build/rel/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-after.json
```

The pre-change evaluation deliberately exits unsuccessfully because it misses
the newly required cases; those misses are retained as evidence. Corpus
execution failures are distinct from normal checker diagnostics. The pinned
manifest contains log.c, cJSON as a program, linenoise as a program, Jansson
and Lua. It extends the RFC 0014 CI subset with Lua; it is not a rerun of all
eleven configurations in the historical corpus.

## Pinned corpus results

[Machine-readable results](../scripts/corpus/rfc0015-results.json) preserve
both revisions' counts, wall times, peak memory, fixed evaluation cases and
every changed diagnostic location/message, including duplicate multiplicities.
The final before/after corpus runs execute sequentially after builds and test
suites finish. Time includes process startup and the measuring wrapper; peak
memory is the checker's resident set, not a sum over translation units.
These are single observations on macOS arm64, not statistical guarantees.

| Project | Reports before → after | Seconds before → after | Peak MiB before → after |
| --- | ---: | ---: | ---: |
| log.c | 2 → 2 | 0.10 → 0.09 | 46.8 → 46.8 |
| cJSON-program | 33 → 32 | 0.45 → 0.52 | 53.8 → 53.9 |
| linenoise-program | 11 → 16 | 0.28 → 0.33 | 51.9 → 52.2 |
| Jansson | 88 → 92 | 0.78 → 0.94 | 55.1 → 56.0 |
| Lua | 757 → 1,533 | 111.78 → 163.67 | 560.5 → 580.8 |

All five projects have **zero parse errors, crashes, timeouts and convergence
failures in the final comparison**. An exploratory run before the last
hardening changes timed out on Lua at 180 seconds. Its partial diagnostics
are not used as final evidence. The comparison above uses the same
600-second limit for both revisions and records complete executions.

The subset takes **113.39 → 165.56 seconds**, about **46% longer**, with Lua
accounting for almost all of the increase. Lua's peak resident memory rises
about 20.3 MiB. Exact existing-cell lookup avoids rescanning array descendants,
and constant selectors skip symbolic-equivalence searches, but richer array
states and summary propagation still have a measurable cost.

Total reports rise **891 → 1,675**. Incomplete-coverage warnings account for
**653 → 1,435** of those reports; other diagnostics total **238 → 240**.
Additional coverage warnings describe previously unmodeled operations. Their
count is not a measure of actual memory bugs or of precision improvement.

## Diagnostic changes and triage

| Project | Changes and interpretation |
| --- | --- |
| log.c | Identical complete diagnostic lists: the two configurable callback boundaries remain. |
| cJSON-program | The null report at `cJSON.c:1280` disappears. `print` uses a one-element `printbuffer` array, checks its allocated buffer and follows successful printing before copying it. Selected record initialization replaces the old shared `buffer[*].buffer` state. Allocation-hook boundaries remain; this does not verify arbitrary user-supplied hooks. |
| linenoise-program | The two old history double-free reports at `2113` and `2371` disappear with selected values and copy ranges. The unsupported-copy warning at `2287` disappears. Three range-membership warnings remain at `1720`, `2290`, `2371`; `history_len - 1 - history_index` adds three unresolved-selection warnings and one unresolved-update warning around `1583–1597`. A new double-free at `2313` occurs in the cleanup loop bounded by `tocopy - len`; it is a false positive where multiple scalar values and loop history exceed the traversal proof. The existing leak and null reports are unchanged. |
| Jansson | Complete pointer-table copy/move helpers at `value.c:504/509` lose their unsupported-copy warnings. Two hash-derived selections remain unresolved at `hashtable.c:266/304`, and two callers with unrepresentable range arguments warn at `value.c:578/600`. Two new double-release reports at `value.c:455/616` come from `json_decref(array->table[i])` loops. These are false positives: the current traversal proof covers the shipped `free`, not general reference-counted cleanup and its per-iteration share relationships. The prior 45 double-free and 28 use-after-free reports remain. |
| Lua | Coverage adds 769 unresolved-selection reports, one unresolved update and one oversized initializer (`ljumptab.h:19`); object-view boundaries change by four additions and one removal. Most selection warnings propagate through callers of stack/tag/GC helpers, so they are not 769 independent unsupported source operations. The old container null report at `lundump.c:253` is replaced by three selected-cell null reports at `256/257`. `loadProtos` first clears the array, then assigns `luaF_newproto` before each use. Those three reports are false positives of the loop/constructor/GC reasoning, not discovered null dereferences. Existing release, lifetime and callback reports remain. |

These regressions remain visible. No warning suppression, removed evaluation
case or refreshed historical baseline is used to hide them. The fixed
evaluation establishes the new supported array behavior; the full libraries
still contain invariants and operations beyond that model.

## Supported boundaries

Precise selected cells use constants or one immutable scalar plus an offset.
The representation bounds cells, captured range inputs and materialized
outputs independently of the source buffer's size. Copies preserve complete
pointer or compatible record elements; arbitrary byte fragments and strides
remain incomplete boundaries. Source rewrites or chained symbolic copies that
cannot yield a valid final interface relation do not export a fabricated
entry-source range.

Complete traversals require a zero-based local induction variable, a supported
affine count and the restricted body specified in RFC 0015. Both counter
declaration spellings are supported. General loops, reference-counted cleanup,
multi-variable index expressions, GC invariants and machine-width arithmetic
remain outside that proof. Unknown overlap may produce conservative temporal
reports. A quiet run is not a verification certificate.

Summary and sidecar format **11** require rebuilding objects carrying older
sidecars. No new annotations or diagnostic IDs are introduced.
