# RFC 0014 validation

Validated on 2026-09-06 against baseline commit
`1815b67d651428f12165af916b5c06d62d9a3c49`. The RFC was drafted and accepted
before implementation, under the owner's instruction to complete both.

## Correctness and build checks

- **637/637 CTest entries pass** in Debug with warnings as errors.
- **637/637 pass with ASan and UBSan**, including the compiler driver,
  sidecars, whole-program analysis and integration tests. LeakSanitizer is
  disabled on this macOS run; address and undefined-behavior checks are active.
- The recall regression set retains **67/67 bug detections** and accepts
  **33/33 good cases**.
- The independent fixed evaluation detects **18/20 bugs** and accepts
  **8/8 clean programs**, with zero unexpected reports, parse failures,
  tool failures or timeouts. The original symbolic-product and VLA misses
  remain in the denominator.
- Clang-tidy passes on the changed library implementations; clang-format,
  cmake-format, workflow YAML parsing and whitespace checks pass. Core
  remains free of Clang/LLVM includes.
- The corpus harness has 12 tests, including parse errors, crashes, silent
  failures, timeouts, iteration limits, missing inputs and per-child memory
  accounting. Both release CI platforms now run the pinned subset.

Focused cases cover known versus unrelated targets, mixed known/unknown
callbacks, nullable callbacks, copied and returned callback parameters,
userdata forwarding, fields and globals, later and cross-file registrations,
internal symbol collisions, incoming pointer comparison operands, complete
pointer/record copies through helpers and other files, fortified copies,
child bounds, partial copies, const and decayed-array record views, incompatible
views, serialization and link-time sidecars. Invalid C fixtures are rejected
before ownership results can be treated as evidence.

## Reproducible corpus measurement

[Machine-readable results](../scripts/corpus/rfc0014-results.json) record
all source revisions, counts, failures, wall times and peak resident sizes.
Both compilers use LLVM/Clang 23.1.0 and CMake Release (`-O3 -DNDEBUG`) on
macOS arm64. All 11 project configurations used clean checkouts at identical
revisions. The final measurements ran sequentially without competing builds
or tests. These are single observations, not statistical speedup guarantees.
Peak memory is the maximum checker's resident set for each project, not a sum
of separately running units. Time includes process startup and the measuring
wrapper.

| Project | Reports before → after | Seconds before → after | Peak MiB before → after |
| --- | ---: | ---: | ---: |
| sds | 12 → 12 | 0.16 → 0.12 | 48.8 → 49.0 |
| cJSON | 5 → 8 | 0.32 → 0.30 | 52.0 → 51.5 |
| jsmn | 6 → 6 | 0.24 → 0.17 | 48.1 → 48.5 |
| log.c | 1 → 2 | 0.07 → 0.08 | 46.4 → 46.6 |
| printf | 20 → 1 | 0.08 → 0.09 | 45.6 → 46.1 |
| linenoise | 8 → 10 | 0.10 → 0.14 | 50.2 → 50.8 |
| cJSON-program | 30 → 33 | 0.32 → 0.45 | 53.6 → 53.8 |
| zlib | 27 → 54 | 0.70 → 2.52 | 67.3 → 81.0 |
| lua | 2549 → 757 | 60.03 → 139.51 | 336.7 → 557.7 |
| linenoise-program | 7 → 11 | 0.21 → 0.47 | 51.0 → 51.9 |
| jansson | 79 → 88 | 0.61 → 1.28 | 53.5 → 54.9 |

The entire corpus takes **62.85 → 145.11 seconds**, with **2,744 → 982
reports**. Every run has zero parse errors, crashes, timeouts and convergence
failures. Of the new total, **682 are `analysis-incomplete` warnings**.
Dropping a safety report while exposing an unsupported view is a change in
coverage accounting; it does not prove that operation safe.

The four-project pinned subset has **117 → 134 reports**. Its existing
ownership/validity detections are unchanged; the additions are eight unresolved
callback boundaries and nine incomplete-copy/view warnings. CI gates execution
validity and retains these results for review. The separate fixed evaluation
gates required detections and clean cases across platforms.

## Diagnostic changes by cause

| Change | Interpretation |
| --- | --- |
| Lua: 2,549 → 757 reports; 644 new incomplete warnings | Actual callback bindings stop applying parser effects to unrelated userdata. The previous 26 leaks disappear, including the invented parser fields. Many broad GC consumes also cease to be applied through incompatible or unknown object views: 642 warnings identify these paths, and two identify unsupported pointer-storage copies. This is not verification of Lua's GC, tag or stack-rebasing invariants. |
| Lua: double-free 1,563 → 1; use-after-free 846 → 0 | The removed running-state consumes were dominated by the GC/callback overapproximation documented under RFC 0013. The numerous remaining coverage warnings mean these numbers cannot be interpreted as recall on Lua. The remaining stdin release and readline buffer reports still require runtime-state correlations. |
| printf: 18 null reports and one unsafe report removed | Calls now use values flowing into their callback parameters, including the null-output callback, instead of every address-taken function with the same type. The externally supplied output callback remains a boundary. |
| cJSON and log.c: additional callback boundaries | Configurable allocation/deallocation/reallocation and logging hooks can receive values from library users. A matching known function no longer hides the unknown alternative. |
| linenoise: two incomplete-copy warnings per configuration | History compaction copies pointer-array ranges. The supported operation is a complete pointer or compatible record; arbitrary array ranges remain outside it. The prior history double-free reports remain. |
| Jansson: two additional boundaries and seven incomplete warnings | User-supplied read/allocation callbacks remain unresolved. Erased/tagged record views account for five warnings; pointer-table copies account for two. The prior 45 double-free and 28 use-after-free reports remain. |
| zlib: 27 incomplete-view warnings | Opaque internal stream-state views cannot all be established from the caller's tracked type. Root effects are retained where valid; unsupported field paths are visible boundaries. This does not establish zlib's internal-state invariants. |
| sds and jsmn: unchanged | Allocation-failure reports and the six example leaks remain; no corpus baseline refresh erased them. |

## Performance and limits

Lua rises from **60.03 to 139.51 seconds** and **336.7 to 557.7 MiB** peak
resident memory. Context specialization, richer summaries and additional
fixpoint work have a measurable cost. Record-layout keys are cached for their
AST's lifetime; a comparison before and after this cache produced identical
complete diagnostic lists. More selective context invalidation and program
scheduling remain performance work.

Target sets and callback contexts are bounded at 32, and callback paths use
the existing eight-step bound. Recursion or iteration limits expose incomplete
coverage. Unknown hooks, opaque internal views, partial pointer representations,
general array identities, GC/regions and arbitrary integer invariants remain
limitations. No clean-program or verification claim is made for the full
corpus. The historical baseline is preserved; the new result is recorded
separately so the coverage and performance tradeoffs remain reviewable.

Summary and sidecar format 10 require rebuilding objects with older sidecars.
