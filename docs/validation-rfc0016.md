# RFC 0016 validation

Validated on 2026-09-07 against baseline commit
`a5583dc4999f319dbb089b9f16a93c1c2eef862b`. The owner requested drafting the
RFC and then implementing it end to end. The design was recorded and accepted
before implementation; acceptance records that authorization, without claiming
a separate RFC review or merge.

## Correctness and build checks

- **769/769 CTest entries pass** in Debug with warnings as errors.
- **769/769 pass with ASan and UBSan**, including compiler linking, sidecars,
  whole-program analysis, recall and fixed evaluation. LeakSanitizer is
  disabled on this macOS run. The sanitizer configuration uses
  `-O0 -gline-tables-only`; its final complete run takes 47.80 seconds.
- The integration entry includes **96/96 lit tests**. Seven new tests pin
  temporal errors, each new context-boundary reason, cross-file checking,
  format 12 metadata, a rejected compiler link and a clean linked executable.
- **20 Core, 38 Analysis and 12 Frontend unit tests** cover context algebra,
  checking and transport. The existing recall set retains **67/67 detections**
  across 33 sources, with no unexpected reports or parse failures.
- Focused clang-tidy passes on the changed implementations and new tests.
  Clang-format, cmake-format and whitespace checks pass. Core remains free
  of Clang/LLVM includes.

The matrix covers source operation order, one release described under multiple
paths, multiple releases, aliased output storage, independent cells containing
one child, replacement and saved incoming values, null and scalar guards,
record fields, globals, selected elements, reference-counted shares, callbacks,
indirect targets and forwarding across multiple files. Tests also exercise
state changes at one call site, invalidation, mutable field guards, copied
borrows into different subobjects, internal-name collisions, unsafe reporting,
missing globals, malformed contexts, context/depth/path bounds and failed
typed requests retaining ordinary body errors. Annotated definitions receive
the same contextual body checking.

## Fixed evaluation

Both revisions run the same unfiltered
[76-program manifest](../test/evaluation/manifest.json). Twelve added bug/clean
pairs cover ordered calls, repeated releases, outputs, replacement, guards,
callbacks, selected cells, interior pointers, globals and nested forwarding,
including inline and cross-file variants.

| Measurement | Before | After |
| --- | ---: | ---: |
| Seeded bugs detected | 32/44 | 42/44 |
| Clean programs accepted without reports | 31/32 | 32/32 |
| Unexpected reports | 1 | 0 |
| Clang parse failures | 0 | 0 |
| Tool failures / timeouts | 0 / 0 | 0 / 0 |

The old checker already detects the new inline bug and the selected-array
bug, but falsely reports the independent-array counterpart. The ten other
new bug cases expose composition gaps. The two remaining size-analysis
misses, `product-known-miss` and `vla-known-miss`, remain in the denominator.
This selected feature evaluation does not estimate recall on arbitrary C.

## Reproducing the measurements

Use LLVM/Clang 23.1.0 and CMake Release (`-O3 -DNDEBUG`, without LTO) for both
revisions. Build the baseline from the commit above in an isolated checkout.
Use the same [pinned five-project manifest](../scripts/corpus/rfc0015.json)
for both executables; no source revisions or historical baselines are changed.

```sh
python3 scripts/evaluate.py --weavec /path/to/baseline/bin/weavec --json /tmp/evaluation-before.json
python3 scripts/evaluate.py --weavec build/rfc16-rel/bin/weavec --json /tmp/evaluation-after.json
python3 scripts/corpus.py --weavec /path/to/baseline/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-before.json
python3 scripts/corpus.py --weavec build/rfc16-rel/bin/weavec --manifest scripts/corpus/rfc0015.json --timeout 600 --measure-memory --json /tmp/corpus-after.json
```

The baseline evaluation deliberately fails because ten newly required bugs
are missed and one clean case is falsely reported. The machine-readable
[comparison](../scripts/corpus/rfc0016-results.json) preserves both evaluations,
corpus counts, timings, peak memory and every changed diagnostic location and
message, including multiplicities. Builds, tests and clang-tidy finish before
the final corpus runs execute sequentially. Both use a 600-second timeout.
These are single observations on macOS arm64, not statistical guarantees.

## Pinned corpus results

| Project | Reports before → after | Seconds before → after | Peak MiB before → after |
| --- | ---: | ---: | ---: |
| log.c | 2 → 2 | 0.09 → 0.08 | 47.0 → 46.8 |
| cJSON-program | 32 → 33 | 0.43 → 0.92 | 54.1 → 55.3 |
| linenoise-program | 16 → 16 | 0.28 → 0.29 | 52.5 → 52.3 |
| Jansson | 92 → 131 | 0.76 → 1.75 | 56.0 → 57.0 |
| Lua | 1,533 → 3,650 | 123.55 → 136.33 | 566.9 → 556.1 |

All five projects have **zero Clang parse errors, crashes, timeouts and
convergence failures** in both final runs. The corpus takes
**125.11 → 139.36 seconds**, about **11.4% longer**. Lua accounts for most
of that cost. Its observed peak memory decreases by 10.8 MiB; this single
run does not establish a general memory improvement. Earlier development
measurements overlapped builds and are excluded from this comparison.

Total reports rise **1,675 → 3,832**. Incomplete-coverage warnings account for
**1,435 → 3,590**; the other diagnostics total **240 → 242**. This is a
substantial increase in visible coverage warnings, not an improvement in
corpus precision. The additional two temporal/validity reports are false
positives, as described below. No corpus report is counted as an independently
confirmed new bug in the feature evaluation.

## Diagnostic changes and triage

| Project | Changes and interpretation |
| --- | --- |
| log.c, linenoise-program | Complete diagnostic lists are unchanged. Existing configurable callbacks and linenoise traversal limitations remain. |
| cJSON-program | One unresolved-alias boundary at `cJSON_Utils.c:1209`, where `create_patches` calls `compose_patch` with the patch container and a child from the input tree. The captured interface cannot establish all required relationships among these reachable objects. Existing null and allocation-hook reports are unchanged. |
| Jansson | Thirty-seven added incomplete reports: 17 incompatible/unknown object views, 11 unresolved alias relationships, five unrepresentable paths and four unavailable callback contexts. These include recursive `do_dump` calls at `dump.c:290/298/371/401`, callback userdata views and intrusive-list operations. These are missing contextual coverage, not 37 independently invalid calls. |
| Jansson | A new use-after-free at `hashtable.c:195` names `hashtable` as freed through `hashtable->list.next` by `hashtable_do_clear`; an invalid release at `pack_unpack.c:606` then claims the stack `key_set` is freed. Both are false positives. The list sentinel belongs to the table itself, while cleanup releases allocated pairs and excludes the sentinel with `list != &hashtable->list`. The aggregate traversal effect loses that exclusion before contextual projection relates the sentinel path back to the table. Proving this intrusive traversal invariant remains outside the bounded call model. |
| Lua | Added coverage reports comprise 1,821 input-footprint limits, 298 unrepresentable context paths, 16 unresolved alias relationships and two array selections. Five prior object-view warnings and 15 prior selection warnings disappear at their old locations. All non-coverage diagnostic lists are unchanged. Broad stack/GC interfaces, such as `luaD_growstack` at `lapi.c:118`, exceed the preflight bound; buffer helpers in `luaL_traceback` expose paths that cannot be projected completely. Incomplete reasons propagate through callers, so these counts do not represent that many independent unsupported operations. |

The copied-borrow regression found during triage is covered separately:
two pointers into different parts of one local array or record must not become
equal merely because their loans name the same borrowed object. Their relative
offsets now survive context capture. The Jansson traversal false positives
remain visible after that correction; neither warning suppression nor a
historical baseline refresh is used to conceal them.

Reducing redundant propagated coverage warnings and proving general sentinel
traversals would improve adoption on these libraries. Those are follow-up
work; raising context limits or suppressing the warnings would not establish
the missing invariants.

## Supported boundaries

This milestone projects caller-established interacting identities; it does
not enumerate arbitrary alias partitions of wholly unconnected inputs.
Ordinary read-only calls and calls with no such established relationship
retain generic checking. The absence of a report does not establish that
arbitrary inputs are disjoint or that the whole program is verified.

Contexts bound the complete input footprint at 64 paths before typed
projection, pointer inputs at 32, relationships/facts at 64, contexts per
callable at 32 and nested specialization at eight. Incompatible views,
unrepresentable paths, unresolved required relationships and exhausted limits
retain ordinary effects and expose incomplete coverage. The early footprint
bound can warn before the checker establishes whether its inputs interact.
Unknown selections and unrestricted heap, loop and GC invariants retain the
limitations of the preceding RFCs. Machine-width and non-affine arithmetic
remain separate work.

Summary and sidecar format **12** require rebuilding objects carrying older
sidecars. No new annotation spellings or diagnostic IDs are introduced.
