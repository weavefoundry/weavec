# RFC 0019 validation

Functional validation and the final-binary ordinary cost gate pass.
All five whole-project checked measurements are recorded; Jansson and Lua
exceed the 600-second timeout. The [results artifact](../scripts/corpus/rfc0019-results.json)
records the binary identities, every completed observation, exact diagnostic
changes, selected real-interface contracts and failed measurements.

## Build and regression checks

Validated on macOS 26.6.2 arm64 with Clang/LLVM 23.1.0, warnings as errors,
and the repository's configured libc++ toolchain.

| Check | Result |
| --- | --- |
| Debug CTest | 963/963 passed |
| ASan + UBSan CTest | 963/963 passed |
| Original fixed evaluation | 44/44 bugs, 32/32 clean cases |
| RFC 0017 fixed evaluation | 24/24 |
| RFC 0018 checked evaluation | 18/18 |
| RFC 0019 source evaluation | 32/32 |
| RFC 0019 whole-program transport | 10/10 |
| RFC 0019 compiler-object transport | 10/10 |
| Pinned real-interface cases | 8/8 |
| Strict clang-tidy | 26 changed C++ translation units passed |
| clang-format, cmake-format, whitespace | Passed |
| Core Clang/LLVM include boundary | No forbidden includes |

The CTest total includes 107 FileCheck/lit cases and the frozen evaluation,
recall and harness checks. Additional RFC 0019 lit cases cover partial loop
coverage, overwritten output counts, nested result fields, zero-byte witnesses,
alias writes and lost conditional terminators. Core tests exercise guarded
joins, dependency invalidation, output outcomes, missing globals, strict guard
parsing, truncated records and bounded facts. The compiler harness checks both
provisional deferral and final executable creation/rejection.

The whole-project pass exposed a transport regression in three private-state
setters in log.c. Exporting their newly inferred private output facts marked
otherwise complete contracts limited. The final exporter omits those optional
private output facts, preserving strict handling of entry requirements and
private premises of public/result outputs. Two unit cases and a compiler-link
lit case pin that distinction. All three log.c setters check completely again.

Release binaries independently pass the 32 source, ten whole-program, ten
compiler-object and eight real-interface cases. Debug and sanitizer test times
are validation observations, not the uncontended performance comparison.

```sh
cmake --build build/dev -j2
ctest --test-dir build/dev --output-on-failure -j2
cmake --build build/rfc17-sanitize -j2
ctest --test-dir build/rfc17-sanitize --output-on-failure -j2
CLANG_FORMAT=/opt/homebrew/opt/llvm/bin/clang-format scripts/check-format.sh
scripts/check-cmake-format.sh
```

`build/rfc17-sanitize` is configured with
`WEAVEC_SANITIZERS=address;undefined`. The directory name identifies the reused
build configuration; its sources and binaries contain RFC 0019.

## Frozen cases and positive progress

The preserved RFC 0018 binary accepts **1/16** positive cases (`frame-good`).
RFC 0019 accepts **16/16**. Both reject all 16 negative programs, but the old
binary rejects two string cases for unrelated missing contract coverage;
RFC 0019 reports the intended missing property in **16/16** negatives.

The pairs cover nested buffers, returned allocation initialization, successful
output parameters, fixed and helper fills, copies, disjoint/overlapping slices,
strings, reallocation, stale aliases and preservation across complete calls.
The manifests require the exact selected scope, zero entry requirements on
`main`, and no unsafe or annotation assumptions. Negative cases cannot pass
through a crash, parse error, absent report or unrelated obligation.

Frozen manifest SHA-256 values:

| File | SHA-256 |
| --- | --- |
| `manifest.json` | `0fbc670a6a7d4f5d20fc3a1484335f407367d2f007178f1c080e79590229bd35` |
| `transport-manifest.json` | `7427116d93a9ed050e67405e1daa1c32ff263440b078d241dba922141458a3a5` |
| `real-modules.json` | `b032f29ef5db6b8c4f90b9a78b40228ce885dc400e1990d19bc4dbfdfdf1e068` |

Reproduction commands and fixture boundaries are in
[the evaluation README](../test/evaluation/rfc0019/README.md).

## Selected Jansson interfaces

Upstream revision: `851a2145e3256f2e67e5dfe24b0e456bf198b741`.
Upstream function bodies are unchanged. Both positive invocations select their
entire frozen interface plus `main`; all selected definitions settle with
complete conditional contracts and no limits or deferral. Both callers have
zero entry requirements.

| Selected definition | Generic entry requirements |
| --- | ---: |
| `strbuffer_init` | 7 |
| `strbuffer_close` | 9 |
| `strbuffer_clear` | 6 |
| `strbuffer_value` | 3 |
| `strbuffer_steal_value` | 4 |
| `strbuffer_append_byte` | 15 |
| `strbuffer_append_bytes` | 24 |
| `strbuffer_pop` | 9 |
| `utf8_encode` | 13 |
| `utf8_check_first` | 0 |

The buffer requirements describe live accessible record fields, initialized
fields where read, writable fields where changed, valid buffer storage,
required byte intervals, the `free` allocation family, disjoint copy inputs
and nonoverflowing byte sums. Generic sufficient requirements can be stronger
than necessary on a particular branch. Checked caller contexts use established
length/capacity values to check growth and failure branches at the call.
The UTF encoder requires valid, writable output storage and byte-count storage;
success establishes the encoded bytes and output count.

The buffer lifecycle caller exercises initialization, growth, byte append,
pop, a string read, clear, steal, release and close. The UTF caller encodes
U+20AC, consumes the initialized three-byte prefix and checks the separately
returned byte count. It does not claim arbitrary variable-length decoding.

| Caller | Expected result | Release report bytes |
| --- | --- | ---: |
| `utf-encode` | Accepted | 50,933 |
| `utf-short` | Rejected: extent | 49,820 |
| `utf-failure` | Rejected: initialization | 49,159 |
| `strbuffer-lifecycle` | Accepted | 211,400 |
| `strbuffer-short` | Rejected: extent | 198,140 |
| `strbuffer-failure` | Rejected: null storage | 198,284 |
| `strbuffer-partial` | Rejected: initialization | 197,781 |
| `strbuffer-stale` | Rejected: released storage | 199,487 |

The allocator adapter binds `jsonp_malloc`, `jsonp_realloc` and `jsonp_free`
to explicit libc-compatible implementations. Modeled C library contracts are
reported trust; there is no unsafe or annotation trust in these callers.
Jansson's configurable allocator hooks and three UTF decoding/traversal
definitions remain unselected. This is not a checked proof of all Jansson.

## Repeated ordinary corpus cost

Both binaries use Release builds against the same LLVM installation. The
baseline is the preserved binary from `ed3f712112b6c9ef2f890002f05aa7c81409928f`.
The unchanged five-project manifest is `scripts/corpus/rfc0015.json`.
Each checker process has a 600-second timeout and fresh child peak-RSS
accounting. Repeated measurements run sequentially after builds and tests.

| Binary | SHA-256 |
| --- | --- |
| RFC 0018 | `18f9d28ab490f38454bbb5d1d56dc57c2b90c1751fe7cfa315982615aa23ac5c` |
| Before private-output correction | `cb142e25258206e4a50430740d5ee17882d807d57f54f195b078aa97baf3e605` |
| Final RFC 0019 | `c667451d6f75cecb5ad83e76389af17075e91052649c01d266e906c611a37e4d` |

Each repetition uses:

```sh
python3 scripts/corpus.py --weavec PATH_TO_PRESERVED_BINARY \
  --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory \
  --json PATH_TO_OBSERVATION.json
```

Two preliminary baseline observations are also retained. The first took
483.990 seconds total (Lua: 476.833 seconds), with peak RSS 693,551,104 bytes.
The second took 607.314 seconds, including a Lua timeout at 600.026 seconds
and no Lua peak-RSS result. The timeout is a failed observation, not a clean
analysis or a usable complete memory sample. These preliminary observations
are separate from the final sequential comparison.

All three isolated baseline repetitions completed successfully:

| Repetition | Total seconds | Peak RSS (MiB) |
| --- | ---: | ---: |
| Baseline 1 | 338.973 | 662.375 |
| Baseline 2 | 344.423 | 656.844 |
| Baseline 3 | 339.899 | 660.766 |
| Before private-output correction 1 | 339.566 | 660.219 |
| Before private-output correction 2 | 315.491 | 662.078 |
| Before private-output correction 3 | 296.222 | 663.734 |
| Final RFC 0019 1 | 319.743 | 663.500 |
| Final RFC 0019 2 | 339.240 | 663.625 |
| Final RFC 0019 3 | 333.351 | 664.359 |

All nine repetitions in the table completed without a process failure or parse
error. The baseline medians are 339.899 seconds and 692,862,976 bytes; pre-correction medians
are 315.491 seconds (**−7.18%**) and 694,239,232 bytes (**+0.20%**). Both meet
the maximum 10% growth criteria. The final binary also completes all three
repetitions, with medians **333.351 seconds (−1.93%)** and
**695,861,248 bytes (+0.43%)**, meeting both limits after the correction.
These are observations on one shared machine;
the timing variation does not isolate an algorithmic speedup.

The final binary adds no ordinary diagnostics and removes
one: Lua `ltable.c:625:17`, `null-dereference`, `dereference of 't->node', which
may be null`. Code inspection identifies this as a false positive in
`setnodevector`: the populated-table branches obtain a positive-sized node
allocation before the initialization loop, and `luaM_malloc_` raises an error
if allocation fails. Its null return applies only to zero-sized requests.
Preserving positive allocation-size facts excludes that irrelevant alternative.
Lua's null-dereference count falls from 105 to 104; the other projects and
diagnostic IDs are unchanged. This single removal does not imply general Lua
coverage or resolve its many incomplete analyses.

## Whole-project checked coverage

The same five pinned projects are measured separately with `--checked` and
one `--checked-report` path per project. Coverage retains all selected
definitions in the denominator. A timeout, unresolved function or iteration
limit is not a successful checked invocation; missing reports provide no
completed proof coverage.

Final-binary observations:

| Project | Complete / selected | Limited functions | Seconds | Peak RSS (MiB) | Report bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| log.c | 3 / 12 | 0 | 0.111 | 49.266 | 78,829 |
| cJSON | 18 / 151 | 2 | 45.180 | 638.828 | 13,545,724 |
| linenoise | 20 / 88 | 11 | 20.953 | 481.672 | 11,808,782 |
| Jansson | unavailable | unavailable | 600.080 | unavailable | no report |
| Lua | unavailable | unavailable | 601.264 | unavailable | no report |

Every completed project above has `invocation_ok: false`: the complete
functions do not discharge unresolved operations in the rest of the selected
project. RFC 0018 recorded 3/12, 16/151 and 18/88 respectively. Checked-mode
costs are materially larger than ordinary checking and are reported separately.

Whole-project Jansson reaches the 600-second timeout without a final report
or peak-RSS sample. This is a scalability regression from RFC 0018's
132.337-second run, which produced a report despite reaching iteration limits.
The timeout's partial diagnostic counts are retained in the results artifact but
cannot supply a completed coverage claim. The frozen selected buffer and UTF
interfaces settle and pass independently. Broader checked-mode scalability
remains unfinished; the ordinary-mode performance gate does not cover that cost.

Lua also times out without a final report or peak-RSS sample, as it did in
RFC 0018. Its measured elapsed time includes timeout cleanup. Missing coverage
and memory values remain unavailable rather than being recorded as zero.

## Remaining limits

The checker remains bounded and conditional on reported entry requirements
and trust. General recursive heap/collector invariants, arbitrary nonlinear
arithmetic and induction, pointer difference/ordering proofs, unsupported
type reinterpretation, concurrency and archive metadata distribution remain
outside this milestone. A complete loop must execute its store on every
covered iteration; an early break or skipped store does not establish a fill.
Byte initialization does not establish pointer provenance. Reallocation growth
does not initialize its tail, and a replacement cannot revive saved aliases.

Summary/sidecar format 15 and checked JSON version 2 require rebuilding older
compiler objects. There are no new annotation spellings or diagnostic IDs.
