# Fixed evaluation set

This suite measures RFC 0013's constructor and allocation-time value cases,
RFC 0014's callback, pointer-guard and complete-copy cases, RFC 0015's
array/container ownership cases, and RFC 0016's compositional call checking.
Every independently identified bug is listed in `manifest.json`, including
bugs the checker does not yet catch. It complements `test/recall/`, which
pins existing detections against regression, and `scripts/corpus/`, which
measures behavior on real projects with many unsupported invariants.

```sh
python3 scripts/evaluate.py --weavec build/dev/bin/weavec
python3 scripts/evaluate.py --weavec build/rel/bin/weavec --json /tmp/evaluation.json
python3 scripts/test_evaluate.py
ctest --test-dir build/dev -R '^(evaluation|evaluation-harness)$' --output-on-failure
```

The checked-in set contains 76 programs: 44 bugs and 32 clean counterparts.
RFC 0016 detects **42/44 bugs**, accepts **32/32 clean programs**, and has no
unexpected reports, parse failures, tool failures or timeouts. The remaining
two bugs exercise a product of two symbolic sizes and a variable-length array;
both are outside the current affine/layout domain. This small, selected set
is a feature evaluation, not an estimate of recall on arbitrary C programs.

## The matrix

| Shape | Inline | Helper in this file | Helper in another file |
| --- | --- | --- | --- |
| Owned child overflow | required | required | required |
| Owned child lost with container | required | required | required |
| Returned field aliases freed argument | required | required | required |
| Correct child access and cleanup | clean | clean | clean |

Additional cases cover constant and symbolic size reassignment, sizes written
through out-parameters, shared children, returned string facts and a clean
loop using a preserved count. Core, Analysis and lit tests additionally cover
nested graphs, self-links, record returns, replacement, failure restoration,
raw/borrowed fields, release families, projection limits and driver sidecars.

RFC 0014 adds four bug/clean pairs: an actual callback target, a callback
forwarded through another translation unit, a pointer equality guarded
release, and a complete pointer copy. The original two known misses remain
in the denominator.

RFC 0015 adds twelve bug/clean pairs covering release history, rewritten
indices, pointer/record copies, overlapping moves, immutable symbolic counts,
initialization, fill and cleanup helpers, returned arrays across translation
units, compaction and reallocation. The compaction pair reduces linenoise's
history-table movement; the resize pair reduces the pointer preservation needed
by Jansson-style table growth. Core/Analysis/lit tests additionally cover weak
updates, bounds, nullable storage, selected callbacks, reference-counted shares,
range limits, malformed interfaces and compiler sidecars.

RFC 0016 adds twelve bug/clean pairs covering operation order, two releases,
output aliases, replacement and saved values, entry guards, callback targets,
selected elements, interior pointers, globals and nested forwarding. Inline
and cross-file variants pin composition behavior. The same expanded manifest
is run against the previous revision; the two known size misses remain.

## RFC 0017 added regression population

[`rfc0017/manifest.json`](rfc0017/manifest.json) is a separate population of
**12 bug/clean pairs: 24 cases, 12 required bugs and 12 clean counterparts**.
It exercises the current
[RFC 0017 integer and spatial features](../../docs/rfcs/0017-c-integer-semantics-and-spatial-safety.md).
These cases were added during implementation as regression coverage. They are
not an independently collected pre-implementation benchmark, and their results
must not be combined with the original manifest's 44-bug/32-clean denominator.
The original `manifest.json` and its population are unchanged; the RFC 0016
figures above describe that milestone's historical result.

```sh
python3 scripts/evaluate.py --weavec build/dev/bin/weavec --manifest test/evaluation/rfc0017/manifest.json
python3 scripts/evaluate.py --weavec build/dev/bin/weavec --manifest test/evaluation/rfc0017/manifest.json --json /tmp/rfc0017-regressions.json
```

| Pair | Bug | Clean distinction |
| --- | --- | --- |
| `narrowing` | Converting 256 to an eight-bit `unsigned char` makes a post-release access reachable. | The opposite test on the narrowed zero keeps the access unreachable. |
| `bool` | Converting 256 to `_Bool` yields one, reaching a post-release access. | Testing for zero is false; boolean conversion does not truncate to the low bit. |
| `mixed-signed` | Comparing signed -1 with unsigned 1 converts -1 to `UINT_MAX`, reaching the `>` branch after release. | The `<` branch remains unreachable under the same converted comparison. |
| `unsigned-wrap` | `UINT_MAX + 2u` allocates one byte; index 1 is outside it. | Index 0 fits the same wrapped allocation. |
| `product` | Indexing at the repeated symbolic `rows * cols` value reaches one past its allocation. | Subtracting one fits after excluding a zero product. |
| `vla-snapshot` | After a VLA bound changes from 4 to 8, `malloc(sizeof array)` still allocates four bytes; index 4 overflows. | Index 3 fits the captured declaration-time size. |
| `fam` | A flexible tail allocated using the target field offset has only two `int` elements; index 2 overflows despite a sibling count of 10. | Index 1 fits, and cleanup releases the enclosing allocation. |
| `helper-return` | A helper in another translation unit narrows 257 to a returned byte count of one; index 1 overflows. | Index 0 fits the returned size. |
| `outparam` | A separate helper writes a narrowed count of one through a `size_t` output cell; index 1 overflows. | Index 0 fits the published size. |
| `min-bound` | A helper loop bounded by 5 and 9 requires five bytes from a four-byte allocation. | Bounds 9 and 4 require only four bytes. |
| `checked-builtin` | A separate helper's checked `SIZE_MAX * 2` reports overflow and stores `SIZE_MAX - 1`, making a post-release access reachable. | The opposite test on the overflow flag or stored full-width value stays unreachable. |
| `memcpy-wrapped-zero` | A byte-count product wraps to zero, so `memcpy` leaves a destination pointer intact and its use after release is invalid. | A complete pointer copy installs null, so the guarded access remains unreachable. |

The fixtures use the existing minimal `test/Inputs/prelude.h`, with C11
selected by each manifest entry. They target the ordinary eight-bit-char
platforms used by the tests; the wrapped-copy case assumes the pointer size
divides the `size_t` modulus. Every case obtains its own allocation, checks
for allocation failure and releases it once on all paths that acquire it.
The temporal pairs deliberately access after release only on their marked bug
path. The VLA pair derives its fresh allocation size from the captured VLA;
no test relies on an unexplained incoming pointer or an unrelated leak.

Four small functions in `rfc0017/numeric-helpers.c` are shared by the return,
out-parameter, minimum-bound and checked-builtin pairs. Those eight cases use
`whole_program: true` to check the caller and helper translation units
together. Each bug is required at its marked source line and stable diagnostic
ID; no warning is suppressed or counted as a substitute for the intended bug.

Final unfiltered Release evaluation and Debug/ASan/UBSan CTest runs on
2026-09-07 detected **12/12 bugs**, accepted **12/12 clean cases**, and recorded
**zero unexpected reports, parse failures, tool failures and timeouts**.
CTest runs this manifest as `evaluation-rfc0017`, separately from the original
fixed population. The [validation report](../../docs/validation-rfc0017.md)
also records the original population, corpus measurements and remaining limits.

## Adding cases

1. Add a small C program, with `// BUG: unique-name` on each intended bug's
   source line. Keep unrelated warnings and errors out of the program.
2. Add a manifest entry with a unique `name` and its `sources`. Multiple
   sources require `whole_program: true` (also the default for that shape).
   Optional `arguments` are compiler arguments passed after `--`.
3. Add one `bugs` entry per marker, with `marker` and the exact stable
   `diagnostic` ID. `required` defaults to `true`. Set it to `false` for a
   reviewed known miss; this never removes it from the denominator.
4. Add a clean counterpart and explain which semantic distinction makes it
   clean. Run the full set and inspect every unexpected diagnostic.

A newly detected known miss is an improvement and passes. A required miss,
wrong diagnostic ID, report on another line or an unexpected report fails.
Expected warnings such as `leak` count as detections even when the tool exits
successfully. Every source marker must occur in the manifest, and duplicate
markers or case names are errors. Removing a case or its marker changes the
evaluation population and requires explicit review, rather than a baseline
refresh after a regression.

## Failure accounting

The JSON records each detection, miss, unexpected report and elapsed time.
A Clang parse error invalidates detections for that program. A crash, missing
executable, abnormal exit, or silent failing exit is a tool failure. A timeout
is counted separately; `--timeout` defaults to 30 seconds per program. None
of these outcomes can count as a clean program or a detected bug.

`--only TEXT` is a debugging filter and explicitly marks the JSON denominator
as filtered. Publish the full, unfiltered result when comparing revisions.
The harness unit tests simulate parse errors, crashes, timeouts, warning-only
results, wrong-line reports and newly caught known misses.
