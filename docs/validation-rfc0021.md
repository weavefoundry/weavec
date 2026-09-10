# RFC 0021 validation

Status: **all acceptance gates pass** for
[RFC 0021](rfcs/0021-practical-c-traversal.md). The
[measured results](../scripts/corpus/rfc0021-results.json) record binary/source
identities, correctness checks, costs, coverage, trust, limits and cache reuse.
[Diagnostic deltas](../scripts/corpus/rfc0021-diagnostics.jsonl) preserve every
added and removed diagnostic row.

## Scope and frozen inputs

The implementation starts from `90caa9547e95`. It adds same-array pointer
arithmetic and differences, bounded inductive traversal facts, initialized
prefixes, terminated string witnesses, cursor positions and paired progress
across calls, translation units and compiler objects. Direct local gotos use
their actual CFG edges. Unsupported operations and exhausted limits remain
incomplete.

The source evaluation fixes 18 positive and 18 negative programs before
checker edits. The real-source evaluation fixes all five Jansson UTF definitions
and all four cJSON minifier definitions, together with closed positive callers
and four adversarial caller cases. The pinned revisions, complete source
populations and SHA-256 identities are recorded in the
[evaluation README](../test/evaluation/rfc0021/README.md). Upstream sources are
unchanged; the harness checks every tracked C/header file against its pinned
revision and separately syntax-checks the evaluation callers.

The preserved baseline Release executable has SHA-256
`bb36714315dafd91508366ed04a3e3c5e5149a21e93b0f8941d599c027c031ce`.
The final implementation executable has SHA-256
`9505618ba097bbb907f6dd447110bb5bdc06fd4347e4f42146b7eec77b4ad775`.
Measurements use macOS 26.6.2, arm64, eight logical CPUs, 24 GiB physical RAM
and Homebrew LLVM 23.1.0. Both binaries use Release `-O3 -DNDEBUG`, warnings
as errors and **LTO off**. Builds, tests, lint and profiling finish before
sequential checker measurements; sleep is prevented.

## Correctness

| Check | Result |
| --- | --- |
| Debug CTest | 1,082/1,082; 28.54 seconds |
| ASan/UBSan CTest | 1,082/1,082; 100.26 seconds |
| Frozen traversal source population | 36/36 |
| Unchanged upstream interface/caller cases | 8/8 |
| Relevant strict clang-tidy | 33 translation units passed |
| Formatting, whitespace and Core include boundary | Passed |

The baseline CTest population has 1,029 entries. The final population includes
117 lit cases and the existing ownership, checked source, compiler transport
and incremental evaluations. Independent concrete checks cover difference
constraints and joins, target-width pointer differences, mutation of shared
contract sets, path projections and adversarial CFG entry/back-edge cases.
Compiler-object tests transport traversal facts through three source units;
persistent reuse tests require zero warm function analyses.

The selected Jansson interfaces are `utf8_encode`, `utf8_check_first`,
`utf8_check_full`, `utf8_iterate` and `utf8_check_string`. The cJSON interfaces
are `cJSON_Minify`, `skip_oneline_comment`, `skip_multiline_comment` and
`minify_string`. All nine have complete, nonlimited, nondeferred generic
contracts without new unsafe or annotation trust. Their closed positive callers
are proven with zero entry requirements. Short output, truncated UTF,
unterminated input and partially initialized input fail for their intended
properties.

Additional regressions cover private callback guards and advanced string
inputs. Export may strengthen a sufficient entry requirement by removing a
known private antecedent while preserving required-property dependencies and
strict output premises. An advanced string requirement retains its minimum
terminator index and the reserved zero end field through serialization.
Concrete witnesses must be at or after the requested suffix start; an earlier
zero cannot satisfy an empty interval. An inferred entry-prefix requirement
cannot justify a scan that may start before its input pointer.

## Checked corpus and completeness

Every uncached and cold-cache project produces a completed report within the
600-second per-project limit. Exact function identities preserve all 99
baseline-complete selected functions. Complete selected contracts increase to
120 across the unchanged 1,619-function selection. These counts describe
conditional function contracts; the complete projects still have incomplete
checked invocations.

| Project | Baseline complete / selected | Final complete / selected | Uncached seconds | Peak RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| log.c | 3/12 | 3/12 | 0.185 | 53.5 |
| cJSON | 18/151 | 25/151 | 18.014 | 295.2 |
| linenoise | 20/88 | 24/88 | 4.186 | 148.2 |
| Jansson | 18/211 | 21/211 | 65.058 | 758.6 |
| Lua | 40/1,157 | 47/1,157 | 575.005 | 7,473.7 |

The gains include the four cJSON minifier definitions, three additional
Jansson UTF definitions, four linenoise helpers and seven Lua helpers.
`lua_freeline` retains its baseline completeness.

Reports include unselected definitions and header functions. Their totals and
limits must not be confused with the selected-function denominator above:

| Project | Reported functions | Complete | Complete with trust | Incomplete | Limit-marked records |
| --- | ---: | ---: | ---: | ---: | ---: |
| log.c | 13 | 3 | 0 | 10 | 0 |
| cJSON | 249 | 37 | 1 | 212 | 3 |
| linenoise | 130 | 28 | 3 | 102 | 12 |
| Jansson | 475 | 49 | 0 | 426 | 8 |
| Lua | 1,364 | 71 | 2 | 1,293 | 639 |

Complete-with-trust is a subset of complete. All reports have zero deferred
functions. No limit-marked function is complete. Lua's raw corpus result
records a function iteration-limit failure; the finished checked report retains
its originating obligation. That is explicit incomplete coverage. There is no
whole-program nonconvergence or timeout in the accepted checked observations.
General nonlinear relations, unsupported object layouts, recursive heap/GC
invariants and exhausted bounded domains remain coverage limits.

## Persistent reuse and report size

All 51 translation units are reused on the warm invocation, with zero function
analyses and zero checkpoint write failures. Expanded uncached, expanded cold
and compact warm reports have identical canonical semantic content, including
requirements, outputs, trust and limits. Their diagnostic sets also match
exactly. These sets compare project, file, line, column, severity, stable ID and
message; diagnostic-note round trips have separate unit coverage.

| Project | Cold seconds | Warm seconds | Warm unit hits | Expanded MiB | Compact MiB | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| log.c | 0.196 | 0.340 | 1 | 0.08 | 0.02 | 3.69× |
| cJSON | 17.811 | 0.837 | 2 | 13.26 | 2.55 | 5.21× |
| linenoise | 4.357 | 0.566 | 2 | 9.96 | 1.92 | 5.20× |
| Jansson | 69.776 | 2.979 | 12 | 45.87 | 5.64 | 8.13× |
| Lua | 541.508 | 34.501 | 34 | 2,516.81 | 182.84 | 13.77× |

The cJSON and linenoise reductions exceed the required 5×. Warm validation and
reporting have overhead even when no dataflow runs; log.c's small cold run is
faster than its warm run. Lua's cold cache run peaks at 9,505,914,880 RSS bytes
and writes 18,579,175 checkpoint bytes. Its expanded report is about 2.46 GiB,
so compact reports are useful for routine inspection. The implementation keeps
the existing semantic budgets and checkpoint size limits.

## Ordinary cost

Three sequential baseline observations take 172.983, 166.534 and 191.878
seconds across the unchanged five-project corpus. Three final observations
take 185.929, 163.001 and 168.874 seconds. Median total runtime decreases from
172.983 to 168.874 seconds: **2.38% faster** (ratio 0.97625).

Median peak RSS decreases from 670,498,816 to 668,155,904 bytes: **0.35% lower**
(ratio 0.99651). Both ratios pass the maximum 1.10 gate. Every observation
completes without a corpus failure.

The ordinary diagnostic sets are identical across all six observations on
each project: 2 in log.c, 32 in cJSON, 26 in linenoise, 141 in Jansson and
3,867 in Lua. No ordinary diagnostic is added or removed.

## Diagnostic changes

Exact checked diagnostic-set changes against the preserved RFC 0020 final
cold reports are:

| Project | Baseline | Final | Removed | Added |
| --- | ---: | ---: | ---: | ---: |
| log.c | 75 | 74 | 1 | 0 |
| cJSON | 4,533 | 4,144 | 568 | 179 |
| linenoise | 1,734 | 1,690 | 250 | 206 |
| Jansson | 7,320 | 7,285 | 785 | 750 |
| Lua | 61,229 | 67,817 | 3,877 | 10,465 |

Most changes are unresolved proof obligations: some direct accesses gain
bounds/initialization evidence while callees expose new sufficient
requirements or propagate unsupported operations and limits. The larger Lua
set includes additional unsupported-layout, copy, bounds and context evidence.
Its additions include 87 `analysis-incomplete` entries, seven null-dereference
warnings and fourteen `checking-failed` entries; removals include eighteen
null-dereference warnings.

Ownership findings also change under checked analysis. Jansson adds six
`double-free` entries concerning `parents->buckets`/`parents_set.buckets` in
`dump.c` and `value.c`, plus an overwritten-array-element leak at `value.c:637`.
Earlier findings at `hashtable.c:195` and `pack_unpack.c:192`, `:209` and `:606`
disappear. cJSON adds six read-only-storage checking failures in `cJSON_Utils.c`.
Upstream bug status is unverified; false positives and model limitations remain.
The diagnostic artifact preserves every added and removed row with its project,
location, severity, stable ID and exact message.

## Development failures retained

Early candidates exceeded Lua's time limit. Shared ordered contract sets,
immutable path projections and grouped conditional requirement translation
reduced repeated work without changing semantic budgets. A candidate that
completed within the limit still failed exact completeness preservation because
it lost `lua_freeline`; private-antecedent export fixed that regression.

A later candidate passed correctness and uncached cost but failed cJSON/Lua
checkpoint publication. Strict producer validation rejected advanced
`terminated` records with a nonzero reserved end field. The final candidate
corrects the producer and adds the suffix/witness counterexamples described
above. Failed measurements remain in local development artifacts and are not
counted as acceptance results.

## Reproduction

Use the same LLVM installation and Release configuration, explicitly disabling
LTO when reproducing these measurements. The regular `release` preset enables
LTO by default. Preserve the baseline executable before rebuilding.

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"
cmake --preset release -B build/rfc21-release \
  -DCMAKE_C_COMPILER="$WEAVEC_LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$WEAVEC_LLVM_PREFIX/bin/clang++" \
  -DWEAVEC_ENABLE_LTO=OFF -DWEAVEC_WARNINGS_AS_ERRORS=ON
cmake --build build/rfc21-release --target weavec -j3

python3 scripts/checked-evaluation.py --weavec build/rfc21-release/bin/weavec \
  --manifest test/evaluation/rfc0021/manifest.json \
  --json build/rfc21-validation/reproduced-cases.json
python3 scripts/checked-traversal.py --weavec build/rfc21-release/bin/weavec \
  --corpus-root build/corpus --timeout 600 \
  --json build/rfc21-validation/reproduced-real.json
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc21-release/bin/weavec \
  --baseline build/rfc21-validation/baseline/weavec \
  --output build/rfc21-validation/reproduced-corpus \
  --phase all --repetitions 3 --timeout 600 --archive-reports
```

Run each checker process sequentially after all builds/tests/profiling finish.
Use a fresh output/cache directory. Also compare exact completed function
identities and require all 51 warm hits, zero warm analyses and three-way
report/diagnostic equality. Executable, source/preprocessing and option
identities remain part of cache validation.

`--archive-reports` compresses each report after its measured run and semantic
digest collection. It verifies the decoded SHA-256 before replacing the plain
file with its gzip archive. Observations retain compressed and decoded
identities, original sizes and expanded semantic digests. Archiving is outside
the checker timing and RSS observation.

The published result file stores identical detailed function/limit lists once
in its uncached checked section. Each cached report references those fields
with an internal JSON Pointer after exact equality checks, retaining its own
cost, digest, totals and raw artifact identities.
