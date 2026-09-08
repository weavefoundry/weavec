# RFC 0018 validation (performance signoff pending)

This report accompanies [RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md)
and the checked-code implementation based on `fb90a22`. The feature adds a
conditional safety check for selected source functions. An unresolved rejection
is not counted as a newly diagnosed concrete bug. Implementation and validation
are in the working tree; this report claims neither an independent review nor
a merge.

Correctness validation is complete. Performance acceptance is **pending**:
competing builds and benchmark/test workloads repeatedly prevented an isolated
comparison. RFC 0018 remains Accepted until its three-run final-binary gate is
satisfied. Earlier binaries' measurements do not substitute for that gate.

## Correctness

| Check | Before | RFC 0018 |
| --- | ---: | ---: |
| Debug CTest | 900/900 | 939/939 |
| ASan + UBSan CTest | Historical RFC 0017 result | 939/939 |
| Lit cases | 100 | 102/102 |
| Original fixed bugs detected | 44/44 | 44/44 |
| Original fixed clean programs | 32/32 | 32/32 |
| Separate RFC 0017 pairs | 12 bugs + 12 clean | All 24 pass |
| Recall detections | 67/67 | 67/67, zero unexpected reports |
| Frozen RFC 0018 checked cases | Frozen before implementation | 18/18 |

Builds use Homebrew LLVM/Clang 23.1.0 on macOS arm64, warnings as errors, and
LTO disabled. Debug and sanitizer builds use `Debug`; measurements use
`Release`. Sanitizer settings are
`ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. Leak sanitizer was not run.
The final Debug CTest run took 60.63 seconds and the sanitizer run took
179.69 seconds; correctness runs may overlap
other validation jobs and are not performance observations.

The frozen [checked evaluation](../test/evaluation/rfc0018/manifest.json)
contains guarded/unguarded indices, the early-exit helper interval, union
identity, local and heap initialization, interacting aliases, unavailable
callees, unsafe code, assumptions, release families, complete copies and
warning demotion. All three motivating misses are rejected in checked mode.
Useful guarded access, initialized allocation and helper compositions pass.

Write-permission regressions cover literals, cast-away const objects, const
record fields, memory primitives and cross-file helper contracts. Mutable storage
viewed through a const pointer and allocations held by const pointer variables
remain usable.

Deep helper-chain regressions preserve unresolved and trusted outcomes without
exponentially expanding their diagnostic identities.

Core tests cover weakest-outcome joins, deterministic identity, initialization
intersection and coalescing, requirement/postcondition joins, bounds,
serialization, truncation, malformed data and global remapping. Analysis tests
also cover changing indices, scalar writes through helpers, arithmetic,
pointer formation, unsafe propagation, annotation assumptions, unavailable
external bodies and complete initialization postconditions. Additional direct probes check uninitialized conditions, unknown unsafe output
effects, valid one-past pointer formation and guarded arithmetic. Exact-message lit
checks pin `checking-incomplete` and `checking-failed`.

The compiler evaluation exercises selection, deterministic/failed report
publication, provisional compilation, link-time contract discharge, whole-TU
and cross-TU checking, stale headers, altered objects, unavailable helper
metadata, prototype mismatch, annotation-only selection, disabled analysis,
disabled link verification, unsupported language, sidecar write failure, selected header diagnostics and warning demotion. A failed selected proof cannot be made successful through
warning controls.

Strict clang-tidy covered all 31 added/modified C++ translation units; the
remaining findings were corrected and those files rerun. This includes all new
unit-test files. Existing test helpers touched by the format-version update
were adjusted to the repository's static-function and designated-initializer
rules. `clang-format`, `cmake-format`, and `git diff --check` pass. Core still
contains no Clang or LLVM includes.

## Corpus method

Use the unchanged [pinned five-project manifest](../scripts/corpus/rfc0015.json):
log.c (1 TU), cJSON (2), linenoise (2), Jansson (12) and Lua (34). Each whole
program includes every listed source. No diagnostic filtering, reduced input
population, increased timeout or early rejection is used for the ordinary-mode
comparison.

The three complete baseline observations use the preserved, unchanged baseline
binary. Final acceptance requires three implementation observations
taken sequentially using one final binary. Baseline observations were collected
earlier in the same environment and are reused across implementation iterations. Each observation uses:

```sh
python3 scripts/corpus.py --weavec BINARY \
  --manifest scripts/corpus/rfc0015.json --timeout 600 \
  --measure-memory --json OBSERVATION.json
```

The acceptance comparison requires observations without competing compute-heavy
work. Runs with observed interference are retained separately and excluded. Per-run
runtime is the sum of the five project times; per-run peak RSS is the largest
project peak. The acceptance limit is 15% ordinary-mode growth. Checked-mode
coverage and cost are measured separately, using `--checked` and a separate
`--checked-report` for each project. A timeout or missing report supplies no
proof claim.

The preliminary implementation took 368.457 seconds versus 289.713 seconds
for the first baseline, a 27.2% regression, with unchanged diagnostic totals.
Peak RSS was 655.750 versus 652.844 MiB. This failed the acceptance budget.
Proof-state containers were subsequently made optional so ordinary analysis
skips constructing, copying and joining them. A unit test checks independent
copies and conservative joins with an inactive domain. The preliminary result
is retained separately, and the complete unchanged baseline observation is
reused in the final comparison. The optional-state implementation produced a
317.554-second observation (657.219 MiB), also retained separately. Header
selection diagnostics were then completed and the implementation measurements
repeated. That three-run implementation median was 337.110 seconds (+5.15%)
and 657.625 MiB (+0.44%). A subsequent audit found accepted writes to literals
and cast-away const storage. Explicit writable requirements and direct, builtin
and cross-file regression tests corrected the gap; implementation measurements
were repeated again. These earlier runs remain preliminary data. Two interrupted
attempts have no complete
observations and are excluded. A later complete run took 465.811 seconds
while an orphaned analyzer from the interrupted checked Jansson run was still
active. That measurement is invalid for comparison and retained separately.
The following repetition was interrupted, the identified leftover processes
were stopped, and process inventory confirmed that no analyzer or build remained
before final implementation repetitions restarted. The isolated first repetition
completed in 362.053 seconds. A subsequent separate Cargo/rustc build repeatedly
consumed several CPU cores: the affected 574.277-second repetition is retained
as invalid for comparison, and the following repetition was interrupted. No
other task’s build was stopped. Checked-mode coverage was then measured while
waiting for a quiet window to finish the ordinary-mode comparison.

The final binary completed one ordinary corpus run in **507.194 seconds** with
**661.688 MiB** peak RSS and no corpus harness failures. Its 4,069 diagnostic
records, including locations, severities, messages and multiplicities, exactly
match the baseline. The timing is excluded: another task's benchmark/test
workload and heavy system background activity were observed. Compiler-only
sampling missed that workload, and a manual annotation to the temporary monitor
log caused a bookkeeping parse failure after the corpus had completed.

Three uncontended final-binary observations remain required. No final runtime or
memory growth percentage is claimed. The complete excluded observation, raw
hashes, earlier implementation measurements and interruption reasons are retained
in the machine-readable results. Resume with the same final Release binary,
pinned manifest and 600-second timeout in a roughly 20-minute window without
other compute-heavy tasks. The documented corpus command is run three times;
compare its medians with the three preserved baseline observations.

## Checked-mode coverage and cost

Each project uses one whole-project `--checked` observation under the same
600-second timeout. These are observed costs, not a matched speedup comparison;
other Cargo activity overlapped the Lua run. Counts below include selected
functions only. Conditional functions still require callers to establish their
entry requirements. No project establishes all of its selected functions.

| Project | Seconds | Peak RSS (MiB) | Proven | Conditional | Trusted | Incomplete | Analysis result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| log.c | 0.464 | 48.984 | 3 | 0 | 0 | 9 | Report available |
| cJSON | 20.506 | 316.781 | 2 | 14 | 0 | 135 | Iteration limit reached |
| linenoise | 3.410 | 267.750 | 14 | 4 | 0 | 70 | Report available |
| Jansson | 132.337 | 1060.922 | 4 | 9 | 0 | 198 | Iteration limit reached |
| Lua | 600.395 | unavailable | — | — | — | — | Timeout; no report |

The cJSON and Jansson iteration-limit failures are retained as analysis failures,
even though reports contain results for independent functions. Lua supplies no
function coverage or peak-memory claim. Its timeout is not a concrete bug
rejection. Common incomplete reasons include unavailable callee contracts,
initialization, insufficient bounds and unsupported constructs.

A preliminary cJSON run used 6,010.875 MiB and reached an iteration limit;
Jansson timed out at 600 seconds. Investigation found propagated call identities
recursively escaping previous identities. Flat source-location/reason identities
preserve weakest outcomes and bounded call chains without that textual expansion.
The deep-chain tests cover unresolved and trusted outcomes. Earlier checked
observations and the interrupted preliminary Lua run remain recorded separately.

The [machine-readable results](../scripts/corpus/rfc0018-results.json) record
binary hashes, pinned revisions, per-project costs and diagnostic counts,
preliminary measurements and the final comparison when complete. Raw runs and
reports remain under `build/rfc18-validation/`.

## Practical limits

The guarantee is conditional on exported entry requirements, declared annotation
assumptions, modeled library contracts and recorded unsafe boundaries. Reports
include the selected scope, source and target, C signatures, tool/model
versions, requirements, initialized postconditions and obligation provenance.
They describe selected source functions and add no runtime checks.

The supported subset deliberately rejects unmodeled union or byte
reinterpretation, assembly, nonlocal control flow, pointer ordering/differences,
floating-to-integer conversion and insufficiently represented heap/callback
relationships. General recursive heap invariants and arbitrary induction remain
future work. Returned allocation initialization is not a general postcondition
model; unsupported output forms remain unresolved. Compilation may defer an
external dependency, while local unresolved operations still reject it. Archives
do not automatically preserve member sidecars.

Object/source/header/command bindings protect against reusing stale analysis.
They are consistency checks, not signed proof certificates or a defense against
forged metadata. The execution model and external trust assumptions remain those
of the RFC.
