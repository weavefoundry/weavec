# RFC 0024 runtime evaluation

The primary population has 51 source cases. Their sources, expected outcomes
and missing-property checks were frozen before checker implementation in
`frozen-sha256.json`. The preserved RFC 0023 executable is recorded separately
in the validation evidence. Syntax errors, crashes, timeouts, unrelated
rejections and added entry assumptions do not satisfy a case.

The `transport` and `upstream` directories are supplemental populations selected
during implementation, before their final validation. Their own inventories
record the inputs. Upstream callers include complete unchanged source files
from the pinned corpus checkouts. The caller supplies `<strings.h>` for the
POSIX declaration used by linenoise; no upstream function body is edited.

Run the source population and cross-source transport with:

```sh
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0024/manifest.json --json build/runtime-source.json
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0024/transport/manifest.json --json build/runtime-cross-source.json
```

Run real compiler objects, cache invalidation and unchanged-source clients with
`scripts/checked-runtime.py --population objects|cache|upstream --weavec ...
--cc ... --output ...`. Each population saves logs and reports independently.
Upstream evaluation needs the pinned `cJSON-program` and `linenoise-program`
checkouts prepared by the corpus tooling.

The cache population compares uncached, cold and warm reports, requires zero
function analyses in the unchanged warm run, changes the helper's returned
result, corrupts cache records, and checks stale and old-format sidecars.
