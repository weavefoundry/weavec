# RFC 0020 evaluation

`manifest.json` freezes the acceptance population and performance limits before
implementation. Keep these expectations when a regression occurs. The cases
exercise cold/warm equivalence, cross-unit invalidation, unchanged dependency
closures, missing definitions, callback/global facts, preprocessing changes,
compiler object binding, corrupt records, failed writes and explanation identity.

```sh
python3 scripts/incremental-evaluation.py \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc \
  --json build/incremental-rfc0020.json
```

CTest runs this command in Debug, sanitizer and Release configurations. Each
case owns a temporary source tree and cache. Cached and uncached results must
agree on exit status, diagnostics and the full expanded checked report. Warm
runs assert zero function analyses. The leaf-edit cases also require a hit in
an independent translation unit. The compiler case changes a conditional
include after compilation to ensure current source cannot validate an old
object. The write-failure case uses a child-only POSIX file-size limit to
exercise failure after a successful open.

The existing RFC 0019 source, transport, object and pinned Jansson interface
populations remain separate regression gates. They are not replaced by this
cache evaluation.

For sequential Release measurements, preserve the baseline executable before
building the change. Finish builds, sanitizer tests and profiling first:

```sh
python3 scripts/scalability-evaluation.py \
  --weavec build/release/bin/weavec --baseline build/baseline/weavec \
  --output build/scalability-rfc0020 --phase all
```

The harness checks all five projects in the unchanged RFC 0015 corpus
manifest. It records executable and manifest hashes, pinned revisions, commands,
diagnostic counts, elapsed time, peak RSS, reports and work counters. Three
ordinary observations per binary compare median total time and median peak
memory. Cold checked runs require reports within 600 seconds per project.
Cold-cache and warm-cache runs compare every expanded report field and require
zero warm dataflow; cJSON and linenoise must have at least fivefold smaller
compact reports. A cache phase requires a new output directory. A failed gate
returns a failure status and remains visible in its results JSON.

`projection.c` adds a diamond of repeated failure paths and transitive unsafe
trust. Its `projection.json` expectation was captured from the preserved
pre-projection binary (SHA-256 `431eb6e6981dd50eec434380b526a067316700ff80df1adbf90eaa1f872e02b4`).
Only the repository prefix is normalized; target/tool labels are excluded from
this portable expectation. Every function field, call route and total remains
pinned. The explanations case expands compact output before comparing it.

`projection.stderr` preserves the complete diagnostic sequence from the
pre-deduplication executable `c5741233bac04e81f3c8749d1dea35a33e104b6af4780f3d70861192d4c3cf64`.
Only the absolute repository root is replaced with `$ROOT`. The explanation
case checks the original messages and notes as well as the earlier JSON
report fixture; shared ledger storage and early diagnostic deduplication must
preserve both.
