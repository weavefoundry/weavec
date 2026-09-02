# Corpus testing

`scripts/corpus.py` runs `weavec` over a handful of real C projects and
tallies what it reports. It exists to answer the questions the RFCs defer to
"corpus testing": which false positives matter in practice, how noisy the
default `annotation-required` boundary is, and whether analysis time stays
well under Clang's parse time.

```sh
cmake --build --preset dev
scripts/corpus.py --weavec build/dev/bin/weavec                       # table of counts
scripts/corpus.py --weavec build/dev/bin/weavec --only linenoise --show-all
scripts/corpus.py --weavec build/dev/bin/weavec --baseline scripts/corpus/baseline.json
scripts/corpus.py --weavec build/dev/bin/weavec --local ~/src/foo --local-args -Iinclude
```

- `projects.json` lists the projects: a git URL and ref, the translation
  units to analyse (globs) and the compiler arguments passed after `--`.
  Checkouts go under `build/corpus/`; `--refresh` re-fetches them.
- `baseline.json` records the per-project counts from the last accepted
  run. `--baseline` compares against it and exits non-zero when any
  diagnostic id's total grows; `--update-baseline` rewrites it after a
  reviewed change. Because the projects track branches, the resolved commits
  are recorded so a change in the numbers can be attributed to them.
- `--json out.json` writes every diagnostic with its location for triage.
- The `clang` column counts Clang's own errors (missing headers, wrong
  `-std`); it should be zero, otherwise the numbers for that project are not
  meaningful.

The CI workflow `corpus.yml` runs the comparison weekly and on demand; it is
deliberately not part of the pull-request gate because the projects are
moving targets.

## Triage of the current baseline

Every diagnostic in the baseline has been looked at. They are all false
positives, and each one is an instance of a limitation an RFC already names:

| Project   | Diagnostic                                             | Cause                                                                                                                                                                       |
| --------- | ------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| linenoise | `double-free` on `*lc->cvec`, `*history` (three sites) | `free(a[i])` in a loop: RFC 0002 collapses every element to the one `a[*]` place, so the second iteration looks like a second free. Candidate for a may-move refinement of `[*]` places. |
| linenoise | `conflicting-borrow` ×2 at `freeCompletions(&ctable)`  | Guarded by `if (lc != &ctable)`; the analysis does not track pointer equality (deferred with null tracking in RFC 0002).                                                    |

Neither is introduced by signature inference (RFC 0003); the inter-procedural
part of the corpus (sds, cJSON, jsmn, log.c, printf) is clean, and no
`annotation-required` boundary warnings fire because the shipped libc table
covers every external call these projects make.
