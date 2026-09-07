# RFC 0017 validation

Measured on 2026-09-07 for
[RFC 0017: C integer semantics and compositional spatial safety](rfcs/0017-c-integer-semantics-and-spatial-safety.md).
The implementation closes both retained size misses and preserves the original
clean evaluation population. **The corpus performance targets were missed, and
three new Jansson false positives remain.** Those costs are part of this result.
Implementation and validation are complete in the working tree; this report
claims neither an independent design review nor a merge.

The [machine-readable results](../scripts/corpus/rfc0017-results.json) preserve
all six corpus observations, source revisions, binary hashes, evaluation
records, and every changed diagnostic's location, message and multiplicity.
Generated raw runs remain under `build/rfc17-validation/`; their SHA-256 hashes
are retained in the published results. The comparison baseline is
`910dc8a801e3fd6d46c1ef3deb57bcc88c439bab`.

## Correctness and build checks

| Measurement | Baseline | RFC 0017 |
| --- | ---: | ---: |
| CTest entries | 769/769 pass | 900/900 pass |
| Lit integration cases | 96 | 100/100 pass |
| ASan + UBSan CTest entries | Historical RFC 0016 result | 900/900 pass |
| Original fixed bugs detected | 42/44 | 44/44 |
| Original fixed clean cases | 32/32 | 32/32 |
| Separate RFC 0017 regressions | Not in original population | 12/12 bugs, 12/12 clean |
| Recall detections | 67/67 | 67/67, zero unexpected reports |

Debug, Release and ASan/UBSan builds use Homebrew LLVM/Clang 23.1.0 on macOS
arm64, with warnings as errors. The full Debug suite took 17.80 seconds;
the full sanitizer suite took 81.82 seconds. Sanitizer settings were
`ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`; leak sanitizer was not run.
The shared numeric driver fixture was subsequently expanded without changing
implementation code; all 100 lit cases were rerun successfully in both builds.

The 333 Core tests include independently enumerated small-width arithmetic
and conversions, abstract-range containment, full-width boundaries, expression
limits, joins and serialization. Analysis regressions cover narrowing and
boolean conversions, mixed signedness, wrap and undefined operations, switch
conversion, typed guards, checked arithmetic, numeric entry snapshots,
returned/output values, alias write order, products, loop requirements,
VLA dimensions and flexible tails. Explicit tests preserve missing coverage
when an expression is exhausted and prevent abstract range endpoints from
becoming invented reachable bounds violations.

Whole-program and compiler-driver tests cover numeric values, lower access
bounds and format 13 transport across translation units and separate object
files. Summary and sidecar tests reject malformed/oversized representations
and check remapping. Recursive summary joins have regression coverage.
The new `invalid-integer-operation` diagnostic has a stable ID, exact-message
lit tests, unit tests and [documented controls](annotations.md#diagnostics).
Unsafe suppression does not suppress numeric state transfer or spatial dump
accounting.

`clang-format`, `cmake-format` and `git diff --check` pass. Strict clang-tidy
checks covered the new and modified implementation translation units, including
a Core header check. Core still has no Clang or LLVM includes. No diagnostic
suppression or increased convergence limit was added to make validation pass.

## Fixed evaluation and recall

The original [76-program manifest](../test/evaluation/manifest.json) is
unchanged. It retains 44 bugs and 32 clean cases, including
`product-known-miss` and `vla-known-miss`. These two entries still have
`required: false`, so a successful harness exit alone would not establish
improvement. Explicit unfiltered results show **42/44 before and 44/44 after**,
with **32/32 clean in both runs** and zero unexpected reports, parse failures,
tool failures or timeouts. The published JSON retains both sets of case records.

The [separate RFC 0017 manifest](../test/evaluation/rfc0017/manifest.json)
contains twelve bug/clean pairs added during implementation. It reports
**12/12 detections and 12/12 clean cases**, with every failure counter zero.
These are regression tests, not an independently collected benchmark, and do
not inflate the original denominator. CTest registers this population as
`evaluation-rfc0017` separately from `evaluation`.

The recall set still detects **67/67 marked bugs across 33 source files**,
with zero unexpected reports on the paired good functions. Its hardened runner
also rejects timeouts, crashes, abnormal exits and silent failures, even if
some expected diagnostics were printed. Independent harness tests exercise
that accounting. These selected tests do not estimate recall on arbitrary C.
Neither a corpus report nor an incomplete warning counts as a seeded detection.

## Reproduction and provenance

Use the pinned [five-project manifest](../scripts/corpus/rfc0015.json), which
includes all 34 Lua translation units. Build the baseline revision and the
implementation with the same compiler and Release settings: `-O3 -DNDEBUG`,
warnings as errors, no LTO and no sanitizers. The measured binaries have these
SHA-256 hashes:

- Baseline: `5ae8d5cdc68899d47d04540126bcfa9cd82ddf35ec017dbb4b3ee870401215d8`.
- RFC 0017: `7305f897ab322572d8d1d53d6f09c6f0ae21ad77a8f59bf478f30e82b8cb7c31`.

The local implementation Release directory is named `build/rfc16-rel` for
historical reasons; its measured binary contains RFC 0017. The preserved
baseline binary is in `build/rfc17-baseline/bin/`. Do not infer a revision
from a build-directory name.

```sh
ctest --test-dir build/dev --output-on-failure
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build/rfc17-sanitize --output-on-failure
python3 scripts/evaluate.py --weavec /path/to/baseline/bin/weavec --json /tmp/evaluation-before.json
python3 scripts/evaluate.py --weavec /path/to/current-release/bin/weavec --json /tmp/evaluation-after.json
python3 scripts/evaluate.py --weavec /path/to/current-release/bin/weavec --manifest test/evaluation/rfc0017/manifest.json --json /tmp/evaluation-rfc0017.json
python3 scripts/recall.py --weavec /path/to/current-release/bin/weavec
python3 scripts/corpus.py --weavec /path/to/baseline/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-before-1.json
python3 scripts/corpus.py --weavec /path/to/current-release/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-after-1.json
```

Repeat each corpus command for runs 2 and 3. Finish builds, tests and profiling
before measuring; execute corpus runs sequentially. Every observation is
retained, including the slower first baseline log.c startup. Wall times include
checker startup and the memory-accounting wrapper. RSS is the per-checker peak,
not accumulated memory from preceding projects. Results are machine-specific
observations, not statistical performance guarantees.

## Corpus performance

All three runs on each side have identical full diagnostic multisets, including
locations, columns, IDs, severity, messages and multiplicities. Every run has
zero Clang parse errors, tool failures, timeouts and nonconvergence. Ordinary
analyzer exits that report source errors are retained. No project was removed
and the common timeout remains 600 seconds.

| Project | Median seconds before → after | Runtime growth | Maximum peak MiB before → after | RSS growth |
| --- | ---: | ---: | ---: | ---: |
| log.c | 0.097 → 0.102 | 5.2% | 46.95 → 47.41 | 1.0% |
| cJSON-program | 0.922 → 1.761 | 91.0% | 55.27 → 58.88 | 6.5% |
| linenoise-program | 0.297 → 0.687 | 131.3% | 52.34 → 54.02 | 3.2% |
| Jansson | 1.788 → 3.757 | 110.1% | 56.89 → 60.27 | 5.9% |
| Lua | 137.697 → 329.810 | 139.5% | 552.48 → 657.98 | 19.1% |

The baseline run totals are **140.915, 140.800 and 140.991 seconds**; the
implementation totals are **335.658, 342.888 and 336.161 seconds**. Their
medians are **140.915 → 336.161 seconds**, a **138.6% increase (2.39×)**.
This is the median of run totals, not the sum of project medians. Maximum
peak RSS is **552.48 → 657.98 MiB**, a **19.1% increase**. MiB uses 1,048,576
bytes; peaks are never summed across projects.

**Both RFC targets are exceeded:** at most 20% median runtime growth and 15%
peak-RSS growth. The RFC requires investigation and disclosure of excess;
it does not authorize dropping Lua, raising timeouts or weakening facts.
Lua dominates the remaining cost. Development profiling found roughly 2,900
numeric-write entries carried through states in its large interpreter CFG,
with about 14,000 block visits. Replacing tree-based write sets with packed
bit sets reduces state-copy and union overhead while retaining the same facts;
a differential Core test checks the set behavior. A separate Jansson summary
oscillation was fixed by monotone recursive summary joins, including reporting
passes, without increasing the iteration limit.

The measured implementation includes those fixes. Numeric expression/range
transfer, input snapshots and guard projection add work; the remaining cost
has not been fully attributed to individual operations. Earlier preview and
instrumentation timings used different code or overlapped builds/tests and
are excluded. They cannot substantiate a quantitative optimization claim.
Further performance work is needed before claiming this milestone meets its
cost targets.

## Diagnostic changes and triage

| Project | Reports before → after | Added / removed occurrences |
| --- | ---: | ---: |
| log.c | 2 → 2 | 0 / 0 |
| cJSON-program | 33 → 32 | 2 / 3 |
| linenoise-program | 16 → 26 | 18 / 8 |
| Jansson | 131 → 141 | 14 / 4 |
| Lua | 3,650 → 3,868 | 335 / 117 |
| Total | 3,832 → 4,069 | 369 / 132 |

Incomplete warnings increase **3,590 → 3,828**; other reports decrease
**242 → 241**. The latter include boundary warnings and are not counts of
confirmed bugs. All 501 changed keys have their exact primary message and
before/after multiplicity in the published JSON, with a triage category.
Primary diagnostic notes are not present in the corpus JSON, so internal
causal explanations below are marked as inferences where no trace was taken.

- **One newly reported, source-confirmed linenoise bug:** `linenoise.c:1598:22`
  passes a nullable history entry to `strlen`. History navigation frees the
  current entry and stores unchecked `strdup` at lines 1583–1584. With two
  entries, PREV can leave null in slot 1 and select valid slot 0; a subsequent
  NEXT can select still-null slot 1. No fault-injection executable was run,
  and this source path is not a claim that the checker modeled that exact
  sequence of navigation calls.
- **Three new Jansson false positives:** `src/value.c:550:5`, `580:5` and
  `637:16` report use-after-free of `array->table`. Each caller checks
  `json_array_grow` for failure. Growth installs the successful replacement
  table before returning, and no-growth returns the existing table. Under
  the allocator contract in `src/memory.c:39–60`, the subsequent accesses
  use live replacement storage. Old-table release state surviving conditional
  replacement/summary projection is the likely cause, inferred rather than
  established by a checker trace. Possible size overflow is a separate concern
  and does not substantiate these use-after-free reports.
- **Five old false positives disappear:** cJSON's `result_tail` at
  `cJSON_Utils.c:556:13`, `578:9` and `588:9` is initialized with a nonempty
  result before dereference. Lua's `lstrlib.c:159:5` uses a buffer supplied by
  the success-or-error buffer helper; `lua.c:760:9` no longer inherits a
  double release of stdin, which `luaL_loadfilex` excludes with its filename
  guard. Improved numeric guards and call facts are consistent with these
  removals; their exact internal traces were not isolated.

The remaining changes are coverage, not confirmed memory bugs:

- **30 new interval-projection warnings**: one cJSON, 17 linenoise, four
  Jansson and eight Lua sites combine pointer displacement with another
  symbolic count. The C count can be retained while the remaining affine
  first/end interval cannot represent two independent operands. Examples
  include editing suffix moves, history resizing, Jansson string/table copies
  and Lua memory operations; none is counted as a bounds detection.
- **181 added and four removed object-view warnings** occur in Jansson's
  heterogeneous callback userdata and Lua's VM/GC interfaces. **12 added and
  34 removed input-path-limit warnings**, **39 added and one removed
  unrepresentable-input-path warnings**, and **two new alias-relationship
  warnings** reflect changed numeric footprints and projections. cJSON's new
  alias warning is at `minify_string(&json, (char **)&into)`, with related
  cursors into one in-place buffer. These do not establish bad client calls.
- **80 new Lua element-limit warnings** replace many opaque instruction/bitfield
  selections with typed values whose selected cells exceed the 64-cell budget.
  Across the corpus, 11 selection warnings are added and 80 removed; two
  update warnings are removed. These changes do not establish array bounds.
  **Nine new Lua expression-limit warnings** expose arithmetic/bitfield
  expressions exceeding the bounded representation.
- Three linenoise range-membership warnings disappear because represented
  copy and untouched alternatives are both retained. Its history suffix copy
  changes from unsupported pointer-copy to interval-projection coverage at
  the same site. Two Jansson `array_move` call-range warnings disappear with
  typed counts, while the later insert at `value.c:580:5` gains an unsupported
  selected-range warning. These operations are not declared fully checked.

There are no out-of-bounds reports in either final side of this corpus
comparison. Development-only spurious reports from treating abstract range
endpoints as reachable witnesses were repaired before the three final runs;
they are not counted as removed baseline findings. Existing Jansson intrusive
list false positives and Lua GC limitations from the
[RFC 0016 triage](validation-rfc0016.md#diagnostic-changes-and-triage) remain
relevant. Unchanged diagnostics are not reclassified as confirmed bugs here.

## Supported boundaries

The implementation supports target integers through 64 bits, up to two range
intervals, 64 expression nodes and depth 12, eight guard conjuncts, eight
numeric-output alternatives and sixteen extent requirements per pointer
parameter. Arithmetic uses C conversions and unsigned wrap before checking
ownership conditions or sizes. Mathematical pointer offsets remain distinct
from modular allocation byte counts. Possibly invalid arithmetic retains
conservative facts; only definitely invalid supported operations get the new
error. `-fwrapv` covers signed add/subtract/multiply, not invalid signed shifts.

Numeric entry/allocation/output snapshots prevent later writes from resizing
older objects. Supported products, guarded checked arithmetic, returned and
output counts, conditional access intervals and minimum-bound loops compose
through summaries. Unsupported or early-exit loops do not export a weakened
must-requirement. VLA extents retain declaration-time dimensions, including
supported nested/typedef forms; flexible-array extents use actual backing
storage, target layout and its temporal identity.

Side-effecting VLA dimensions such as `char a[n++]` explicitly report
`analysis-incomplete`. Since the CFG has already executed their effects,
re-reading operands could capture a later value. Affected declaration bounds
and dependent `sizeof` values remain unknown; effects are not replayed.
General nonlinear inequalities, arbitrary induction/strides, wider integer
types, unrestricted aliases/provenance, union/type punning, byte-encoded
pointers, GC invariants and concurrency remain outside the supported model.

Spatial dump outcomes distinguish `proven`, `violation` and `unresolved`
independently of warning controls and unsafe suppression. An unknown index
need not cause an out-of-bounds error; a callee requirement remains an
obligation. Silence is not proof. No annotation spellings, runtime checks,
`--verify` flag or whole-program verification certificate are introduced.
Existing annotations remain trusted contracts. Summary and sidecar formats
are both **13**, requiring objects carrying older sidecars to be rebuilt.
