# Fixed evaluation set

This suite measures RFC 0013's constructor and allocation-time value cases,
RFC 0014's callback, pointer-guard and complete-copy cases, and RFC 0015's
array/container ownership cases.
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

The checked-in set contains 52 programs: 32 bugs and 20 clean counterparts.
RFC 0015 detects **30/32 bugs**, accepts **20/20 clean programs**, and has no
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
