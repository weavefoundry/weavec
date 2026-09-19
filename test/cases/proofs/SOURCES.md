# Salvaged false proofs

Each case here comes from a population case that caught a false proof in
v0.10.0's checked mode (RFCs 0018–0029), which RFC 0030 deletes. RFC 0030
§17.2 names what to salvage: the reader cursor, `p + 1` forwarding, the
candidates 99–102 leaks, and every population case whose expected outcome
changed from accepted to rejected.

With the ledger, `// NOT-PROVEN: <facet>` on the defect line means that no
row on that line may have that facet proven (gate G6). Two defects have no
facet, so those cases use other markers:

- a leak is not a facet (§3.4), so the leak cases use `MISS`;
- an uninitialised scalar is not a facet either, and zero-initialisation
  (§11) makes the read well defined, so that case uses `NEUTRALISED: zero-init`.

The sources are copied unchanged, except for:

- the include paths;
- two leading comment lines in each case;
- the markers.

All origins are in commit `fcc3b46` (squashed from `9be55b9` on
`feat/ownership-transfer-attaching-helpers`). They are unchanged at
`e0e2bd6` (v0.10.0).

## Reader cursor

The origin is `test/evaluation/rfc0029/readers/`. Both `manifest.json` and
`reviewed-manifest.json` list these cases with expect `rejected`. Evidence,
from `readers/audit.md`:

> Both the immutable HEAD baseline and candidate 15 wrongly accept
> `reader-short` and `reader-uninitialized`.

`consume` accesses every position up to `end`, but its exported contract
required only the cell at the incoming `position`. Each case gets its own
copy of `cursor.c` because the two cases put different markers on the same
line.

| Case | Origin | Defect | Marker |
| --- | --- | --- | --- |
| `reader-short.c`, `Inputs/reader-short-cursor.c`, `Inputs/cursor.h` | `short.c`, `cursor.c`, `cursor.h` (case `reader-short`) | `Inputs/reader-short-cursor.c:11` reads `data[2]` and `data[3]` of a 2-byte array. ASan reports a stack-buffer-overflow there. | `NOT-PROVEN: spatial` |
| `reader-uninitialized.c`, `Inputs/reader-uninitialized-cursor.c` | `uninitialized.c`, `cursor.c`, `cursor.h` (case `reader-uninitialized`) | `Inputs/reader-uninitialized-cursor.c:11` reads `data[1..3]`, which are never written. | `NEUTRALISED: zero-init` |

## `p + 1` forwarding (candidate 17)

- **Case:** `recursive-offset-forward.c`.
- **Origin:** `test/evaluation/rfc0029/offsets/forward.c`, case
  `recursive-offset-forward` in `offsets/manifest.json`, expect `rejected`.
- **Evidence:** `offsets/provenance.md` says "Candidate 17 incorrectly
  accepts forward.c."
- **Defect:** `odd` passes `p + 1`, one element past the 16-byte `calloc`
  block, to `even`. `even` then reads `->left` and `->right` past the object
  and frees `p + 1`. ASan reports a heap-buffer-overflow on that line.
- **Markers:**
  - `NOT-PROVEN: spatial` on the line in `even`;
  - `BUG: invalid-release` on the call in `main`, where v0.10.0 reports the
    release.

## Candidates 99–102 leaks

- **Cases:** `attached-payload-leak.c` and `attached-payload-ignored.c`, with
  `Inputs/attached-payload-api.h` and `Inputs/attached-payload-library.c`.
- **Origin:** `test/evaluation/rfc0029/attached-payload-transfer/`, files
  `leak.c`, `ignored.c`, `api.h` and `library.c`. The cases are `leak` and
  `ignored` in `manifest.json`, expect `rejected`, reason
  `leak|footprint|ownership`.
- **Evidence:** `docs/validation-rfc0029.md` at `e0e2bd6`, section
  "Candidates 99-102". The first revision "accepted two leaks: replaying an
  unrelated path's captured entry description erased the caller's evidence
  for a still-live allocation".
- **Defect:** `i` is never released when `attach` fails, which happens when
  its 4-byte key allocation returns null. In `ignored.c` the result of
  `attach_wrapper` is ignored.
- **Markers:** `MISS` on the defect line. v0.10.0 is silent here. §8.4 does
  not report a leak at a return from `main`, so a later leak pin needs the
  client moved out of `main`. Running the leak needs an allocation failure;
  a plain run is clean.

## Changes from accepted to rejected

There are none.

- 14 commits (all refs) touch `test/evaluation/rfc00NN` manifests.
  `git log --all -p -- 'test/evaluation/rfc00*/**/*manifest*.json'` shows
  no removed line: each manifest was added once and never edited.
- Across superseding manifests, the only change from accepted to rejected
  is `rfc0029/upstream-extended` `failed-parse` →
  `rfc0029/upstream-lifetime-audit` `failed-parse-stack`. It is not
  salvaged:
  - the original expectation was wrong, and checked mode correctly rejected
    the case;
  - the case needs the pinned cJSON checkout, which is not in this
    repository.
