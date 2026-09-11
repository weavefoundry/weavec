# RFC 0022 validation

Status: **implemented; all amended acceptance gates passed** for
[RFC 0022](rfcs/0022-checked-c-interfaces.md). Debug, sanitizer, Release fixture,
lint, exact preservation/equivalence and measured performance checks pass on
the final executable. The [measured results](../scripts/corpus/rfc0022-results.json)
retain executable and input identities, individual observations, test evidence,
the audited baseline false proof and failed development measurements. The
[diagnostic changes](../scripts/corpus/rfc0022-diagnostics.jsonl) retain every
added and removed diagnostic.

## Scope and frozen inputs

The implementation starts from `4c455cd`. It carries existing checked memory
facts through compatible opaque pointer recovery, synchronous callbacks,
configurable allocation functions, nullable output pointers, separate source
units, compiler objects and persistent checkpoints. Object type and alignment
are independent of validity, initialization, capacity and write permission.
Every known callback target must satisfy its input requirements; guaranteed
outputs intersect over the targets that can return.

The [evaluation population](../test/evaluation/rfc0022/README.md) freezes
16 positive and 16 negative source programs and five actual Jansson clients
before checker edits. All 39 frozen source/manifest identities are verified.
The independent hook-record and setter clients use ordinary C implementations
with different names and layouts from Jansson. Supplemental adversarial and
transport populations are distinguished from the frozen population.

Jansson is pinned at `851a2145e3256f2e67e5dfe24b0e456bf198b741`.
The real-source harness uses unchanged `src/memory.c` and `src/strbuffer.c`,
including actual default/custom allocator hook plumbing. It independently
syntax-checks the sources and verifies that upstream tracked source and headers
have not changed. It does not substitute RFC 0019's allocator adapters.

The original `callback-lifetime-good` fixture left a registration pointing to
a local at scope exit. It now clears the registration after its valid read,
as required by the existing lifetime rule. The original hash and baseline
report are retained, and the correction is recorded in the RFC. The case
population and intended stale-userdata distinction are unchanged.

The preserved baseline Release executable has SHA-256
`9505618ba097bbb907f6dd447110bb5bdc06fd4347e4f42146b7eec77b4ad775`.
The baseline passes 20/32 frozen expectations: four positive programs and all
16 negative programs. The lifetime-fixture correction is separate from an
implementation precision gain.

## Correctness

| Check | Result |
| --- | --- |
| Debug CTest | 1,098/1,098; 38.82 seconds |
| ASan/UBSan CTest | 1,098/1,098; 126.44 seconds |
| Frozen interface source cases | 32/32 |
| Supplemental adversarial cases | 31/31 on Debug, Release and ASan/UBSan |
| Cross-source / compiler-object cases | 8/8 each |
| Actual Jansson implementation clients | 5/5 |
| Prior RFC 0021 upstream evaluation | 8/8 |
| Prior RFC 0019 upstream audit | Baseline and final 7/8; exact completeness preserved |
| Relevant strict clang-tidy | 35 translation units passed |
| C++/CMake formatting, whitespace, Core boundary | Passed |

The CTest population includes 120 lit cases, all previous fixed evaluations,
compiler sidecars and incremental cache checks. The source, compiler-object
and actual upstream evaluations also pass on that Release candidate.

Independent unit checks cover object descriptors, offsets, conflicting/lost
views, malformed metadata, output conditions, global callback context remapping
and invisible foreign callback storage proxies. Supplemental cases exercise
packed member alignment, unrelated local record tags, typed allocation returns,
explicit null output edges in both orders, pointer replacement, indirect string
and zero-fill operations, and mixed builtin/source targets. A `malloc` target
cannot inherit another allocator's initialized output; a `strlen` target's
input requirement cannot disappear when joined with a function that ignores
its argument.

The real-source positives check buffer lifecycle and compatible custom hooks.
The negatives check insufficient allocation, partial initialization and a saved
pointer used after release. Closed positive callers must have zero entry
requirements, no limit or deferral, and no unsafe or annotation trust. Modeled
library trust remains explicit. Generic hook-dependent definitions can remain
incomplete until a caller establishes concrete bindings; this milestone does
not prove arbitrary callbacks from their C prototypes.

## Legacy upstream audit

The earlier RFC 0021 traversal evaluation passes 8/8 on the final binary.
An additional rerun of RFC 0019's upstream adapter evaluation produces **7/8
on both the preserved baseline and the final candidate**. The original
`strbuffer-lifecycle` selection includes generic `strbuffer_append_byte` and
`strbuffer_append_bytes` contracts that are already incomplete in the baseline.
Their missing-property reasons are unchanged; the closed lifecycle caller is
complete in both. All eight case outcomes and every selected function's
completeness flag are preserved. The original expectations and failed reports
remain available; this audit is not reported as 8/8.

The new RFC 0022 evaluation uses actual Jansson allocator hooks and passes
all five closed clients. Its guarantee concerns established callback bindings,
not arbitrary generic append inputs or all possible allocator implementations.

## Baseline false-proof audit

The first completed Lua comparison retains 46 of its 47 baseline-complete
identities and gains `getS` and `get2digits`, for 48 complete selected contracts.
The lost identity is `lua.c#handle_luainit`. The original baseline reported its
two calls through `l_getenv` complete with no requirements, even though `getenv`
has no checked contract. Joining it with the null-returning `no_getenv` callback
also caused the checker to skip the non-null result branches.

A [reduced counterexample](../test/evaluation/rfc0022/adversarial/mixed-unmodeled-bad.c)
selects between `getenv` and a null-returning callback, then dereferences a null
pointer when the callback returns a string. The preserved baseline reports its
client complete with zero requirements. The current checker rejects the missing
callback contract. An independent Clang UBSan run, with `WEAVEC_RFC22_VALUE=present`,
confirms the reachable null load. A [concrete safe setter](../test/evaluation/rfc0022/adversarial/mixed-unmodeled-good.c)
selecting the null-returning callback still has a complete closed caller.

The RFC explicitly amends the preservation criterion for this demonstrated
false proof. The raw retention is **119/120**, with one audited correction and
no allowance for additional unexplained losses. The original identity remains
in every comparison; it is not removed from the baseline or offset by gains.
The original frozen cases and legacy audit expectations remain unchanged.

## Corpus and performance gates

All 120 RFC 0021 complete selected identities are compared across the same
five-project, 1,619-function selection. The 119 unaffected identities must be
preserved, and the demonstrated false proof below must be rejected. A count
increase cannot hide an unexplained lost baseline-complete function. Every uncached and cold-cache checked
report must finish within 600 seconds per project. All 51 warm units must be
cache hits with zero function analyses. Expanded uncached, expanded cold and
compact warm reports must have equal canonical contents and equal diagnostic
sets.

Three sequential ordinary baseline observations and three final observations
must have median total runtime and peak RSS ratios at most 1.10. Both binaries
use Homebrew LLVM 23.1.0 with Release `-O3 -DNDEBUG`, warnings as errors and LTO
off on the same macOS 26.6.2 arm64 host. Builds, tests and lint finish before
measurements; sleep is prevented. Reports are checksum-verified and compressed
after each measured run. Report processing and compression are outside the
checker timing and peak-RSS observation.

All added and removed diagnostics are preserved with project, file, line,
column, severity, stable ID and message. Changes in an upstream diagnostic are
not independently verified upstream bugs. Conditional function completeness
is distinct from complete project acceptance.

## Completed checked measurements

All five uncached and cold-cache projects produced completed reports within
600 seconds. Canonical expanded uncached, expanded cold and compact warm reports
match for every project. Exact identities retain 119/120 baseline-complete
functions, reject the demonstrated false proof above, and add five complete
contracts, for **124/1,619** selected definitions. These are conditional function
contracts; the whole projects still contain incomplete checked code.

| Project | Baseline complete / selected | Final complete / selected | Uncached seconds | Peak RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| log.c | 3/12 | 5/12 | 0.366 | 53.8 |
| cJSON | 25/151 | 25/151 | 22.393 | 293.8 |
| linenoise | 24/88 | 24/88 | 11.553 | 309.3 |
| Jansson | 21/211 | 22/211 | 82.203 | 768.8 |
| Lua | 47/1,157 | 48/1,157 | 527.277 | 7,317.7 |

The gains are `log_add_callback`, `log_add_fp`, `hashtable_iter_next`, `getS`
and `get2digits`. The raw lost identity remains `lua.c#handle_luainit`, as
required by the documented false-proof correction.

| Project | Cold seconds | Warm seconds | Warm unit hits | Warm function analyses | Compact reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| log.c | 0.185 | 0.465 | 1 | 0 | 3.184× |
| cJSON | 18.638 | 1.063 | 2 | 0 | 5.183× |
| linenoise | 10.627 | 0.845 | 2 | 0 | 5.136× |
| Jansson | 66.301 | 3.694 | 12 | 0 | 7.667× |
| Lua | 544.569 | 42.974 | 34 | 0 | 13.759× |

All 51 units are warm hits with zero function analyses. cJSON and linenoise
exceed the required 5× compact reduction. Semantic budgets are unchanged.
Canonical reports and exact diagnostic sets agree across all three modes for
every project. The final executable SHA-256 is
`882ddf6d9d6135ca338dcce76396e4b2a4c4f70c6f1a0ab587b2c44369603048`.

## Ordinary cost and diagnostic changes

| Observation | Baseline seconds | Final seconds | Baseline peak RSS MiB | Final peak RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| 1 | 164.759 | 184.755 | 637.2 | 631.6 |
| 2 | 169.423 | 183.510 | 645.5 | 618.3 |
| 3 | 178.153 | 162.479 | 633.2 | 615.0 |
| Median | 169.423 | 183.510 | 637.2 | 618.3 |

The median total runtime ratio is **1.08315** (+8.31%); the median peak RSS
ratio is **0.97028** (−2.97%). Both meet the 1.10 limit. All three baseline
runs preceded all three final runs, as specified; the spread is retained and
these measurements are not a claim about every host or workload. Every ordinary
run completed without tool failures. The 4,068 ordinary diagnostics are identical
between baseline and final and stable across all three repetitions.

Checked diagnostic counts include propagated call locations. A changed message
at the same location appears as both a removal and an addition.

| Project | Baseline diagnostics | Final diagnostics | Removed | Added |
| --- | ---: | ---: | ---: | ---: |
| log.c | 74 | 51 | 23 | 0 |
| cJSON | 4,144 | 4,081 | 158 | 95 |
| linenoise | 1,690 | 1,788 | 52 | 150 |
| Jansson | 7,285 | 7,315 | 212 | 242 |
| Lua | 67,817 | 69,292 | 390 | 1,865 |
| Total | 81,010 | 82,527 | 835 | 2,352 |

Of the 2,352 additions, 2,176 concern object type or alignment: 1,140 require
a compatible view, 988 cannot represent the required view, and 48 report a
mismatch (six direct failures and 42 propagated incomplete obligations).
The remaining additions expose callback alternatives, input requirements,
numeric/range limits and dependent memory operations. All 3,187 delta records
include their exact stable ID, message and source location. Source inspection
and report comparison support the following interpretation:

- **log.c:** 14 initialized-read and nine unsupported-construct diagnostics
  disappear around static callback state and opaque `FILE *` userdata.
  `log_add_callback` and `log_add_fp` gain complete conditional contracts.
- **cJSON:** generic `hooks->allocate` at `cJSON.c:243` still lacks a represented
  object view. Most new messages make that limitation explicit, replacing
  blanket unsupported-cast diagnostics. Static hook initialization and known
  allocator behavior remove initialization requirements elsewhere. Unknown
  mutable hooks remain visible; the selected completeness total stays 25.
- **linenoise:** established hint callbacks remove old unknown-type boundaries,
  while unresolved targets and `freeHintsCallback` retain missing-contract
  obligations. New dependent diagnostics include `write(fd, ab.b, ab.len)` at
  `linenoise.c:1415`: `abInit` starts with `(NULL, 0)` and `abAppend` can leave
  that state on allocation failure. The modeled non-null requirement reports
  this as both `null-dereference` and `checking-failed`. The pointer/zero-length
  relationship is a remaining model limitation, not a confirmed upstream bug.
- **Jansson:** four direct type/alignment failures at `hashtable.c:321–336`
  are `ordered_list_to_pair`, implemented with `container_of`. Two more occur
  in callers at `pack_unpack.c:919` and `value.c:1088`. Enclosing-record recovery
  is explicitly outside this milestone; these remain conservative rejections
  of that idiom, not evidence that Jansson's C code is invalid. Restoring the
  original list view in `hashtable_iter_next` is supported and gains a complete
  conditional contract. The five closed allocator-hook clients pass separately.
- **Lua:** 1,851 additions are explicit object-view obligations, frequently
  propagated through allocation and GC helpers. Tagged/GC layouts and recursive
  invariants remain outside the supported proof. The removed release diagnostic
  at `lua.c:600` involved `lua_readline` choosing external allocation or an
  automatic buffer and `lua_freeline` checking the same private callback cell.
  A removed read-only-store diagnostic at `ltable.c:919` was at the `rehash`
  call. Both containing functions remain incomplete; neither disappearance is
  counted as a new proof. The separately demonstrated `handle_luainit` false
  proof is rejected, while `getS` and `get2digits` gain complete contracts.

The seven added `checking-failed` records and two removed ones are therefore
reported with their context, without asserting new upstream vulnerabilities or
counting fewer diagnostics as verification. Only the exact complete-function
identity comparison supplies the stated coverage result.

## Development measurements retained

The first cold-cache candidate preserved the first four projects' complete
selected totals (with two gains in log.c and one in Jansson), but linenoise
failed checkpoint publication. Re-interning callback global names in first-use
order changed the serialized ordering of callback inputs and context keys.
The strict checkpoint producer check correctly rejected the mismatch. That
measurement run was stopped and retained as a development failure.

Sidecar format 18 now includes a bounded portable-name prelude that preserves
the original table order. It supplies no extra memory facts. New tests cover
multiple globals referenced in a different order, unused names, requests,
specializations and malformed/duplicate/oversized tables. The focused linenoise
check reuses both warm units with zero function analyses and exact cold/warm
report equivalence. All accepted measurements must use fresh directories and
the corrected final executable.

The second candidate fixed checkpoint publication but exceeded the 600-second
Lua cold limit, without producing a complete report. It is retained as a failed
cost observation. A subsequent development run was converted to a sampled
profile and stopped; it is not an acceptance measurement. The fourth candidate
also exceeded the Lua cold limit (600.223 seconds). Its other four cold projects
completed with zero checkpoint publication failures and all prior complete
function identities preserved. These failed attempts do not satisfy acceptance.

Profiling led to two optimizations that preserve the model. An unresolved
global callback binding equal to the generic target set no longer creates a
duplicate specialization; concrete bindings and global dependencies remain.
Borrow-kind derivation searches only the parameter's pointee subtree and stops
once a mutation determines the result, following RFC 0003's existing rule.
Regression tests retain the distinction between open generic and closed concrete
hook callers, neighboring parameter roots and nested pointee effects.

A subsequent five-project cold pass completed Lua in 541.008 seconds. Its warm
Lua statistics recorded 34 hits and zero function analyses. The run was stopped
after review found that canonicalization discarded stronger typedef alignment.
A reduced aligned-array case demonstrated an incorrectly accepted misaligned
read. Type size and alignment are now read before canonical type identity;
unrepresentable over-aligned scalar views stay unresolved. Supplemental good/bad
cases pin compatible ordinary typedefs and rejection of the stronger alignment.
These observations remain development data, not final acceptance.

Further targeted changes avoid reconstructing equal summary facts while still
joining canonical explanations, and reject duplicate propagated call routes
before copying their strings. Summary equality deliberately ignores explanation
routes; a regression test requires the preferred route to survive this fast path.
Existing bulk-versus-individual insertion tests cover route normalization,
truncation, unsafe conversion and obligation-cap behavior.

Cancellation testing also exposed detached checker children surviving an
interrupted measurement runner. The outer runner now lets the corpus runner
reap its checker process group before exiting. Two tests verify direct and
nested cancellation with real child processes. Development children were
identified and stopped before subsequent measurements.

## Reproduction

Preserve the baseline executable before rebuilding. The regular Release preset
enables LTO; disable it for these matching observations.

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"
cmake --preset release -B build/rfc22-release \
  -DCMAKE_C_COMPILER="$WEAVEC_LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$WEAVEC_LLVM_PREFIX/bin/clang++" \
  -DWEAVEC_ENABLE_LTO=OFF -DWEAVEC_WARNINGS_AS_ERRORS=ON
cmake --build build/rfc22-release --target weavec weavec-cc -j3

python3 scripts/checked-evaluation.py --weavec build/rfc22-release/bin/weavec \
  --manifest test/evaluation/rfc0022/manifest.json \
  --json build/rfc22-validation/reproduced-cases.json
python3 scripts/checked-interfaces.py --weavec build/rfc22-release/bin/weavec \
  --output build/rfc22-validation/reproduced-real
python3 scripts/checked-memory-build.py --cc build/rfc22-release/bin/weavec-cc \
  --manifest test/evaluation/rfc0022/transport-manifest.json \
  --json build/rfc22-validation/reproduced-objects.json
python3 scripts/scalability-evaluation.py \
  --weavec build/rfc22-release/bin/weavec \
  --baseline build/rfc22-validation/baseline/weavec \
  --output build/rfc22-validation/reproduced-corpus \
  --phase all --repetitions 3 --timeout 600 --archive-reports
```

Use fresh output/cache directories, one checker process at a time, after all
other build/test/profiling work finishes. Compare exact completed identities
and all three report/diagnostic modes in addition to the harness's aggregate
gates. Retain executable, preprocessing, option and report identities.
