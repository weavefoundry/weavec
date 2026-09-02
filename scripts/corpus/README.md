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
`annotation-required` boundary warnings fire for *direct* calls because the
shipped libc table covers every external call these projects make.

RFC 0004 (unsafe boundaries) added the rows below. They are the boundary
reports the RFC predicted rather than false positives: each one points at a
place where the tool genuinely cannot see what is called or what a pointer
is, and each names its fix.

| Project   | Diagnostic                                                                 | Cause                                                                                                                                                                                                                                              |
| --------- | -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| log.c     | `annotation-required` at `L.lock(...)`                                     | The lock callback is installed by the library's *user* (`log_set_lock`); no function of its type has its address taken in `log.c`, so the call is a boundary. Annotating `log_LockFn`'s parameter (`void *WEAVEC_BORROWED`) would resolve it.        |
| printf    | `annotation-required` at `buffer->fct(...)`                                | Same shape: `fctprintf`'s `out` callback comes from the caller. The internal `_out_*` functions have a different type, so the per-type join finds nothing.                                                                                          |
| printf    | `unsafe-operation` at `_vsnprintf(..., (char*)(uintptr_t)&out_fct_wrap, ...)` | A deliberate integer round-trip (to shed `const`) is exactly the RFC 0004 definition of a raw pointer, and `_vsnprintf` writes through that parameter. The RFC lists this idiom under *Deliberately not caught*; the intended fix is a `WEAVEC_UNSAFE` region around the call. |
| linenoise | `annotation-required` ×3 at `completionCallback`, `hintsCallback`, `freeHintsCallback` | User-installed callbacks (`linenoiseSetCompletionCallback` and friends): no candidate of the type in the TU.                                                                                                                                       |

So the corpus answer to RFC 0004's deferred question ("how many indirect
calls fall to the boundary") is: five call sites across six projects, all of
them library hooks installed by code outside the translation unit, which is
the single-TU limitation Milestone 4 addresses. Hook tables initialised
*inside* a TU (cJSON's `global_hooks = { internal_malloc, internal_free,
internal_realloc }`, called as `global_hooks.deallocate(item->valuestring)`)
resolve through the address-taken join and produce nothing. No `use-after-free`,
`double-free` or `conflicting-borrow` changed when pointer arithmetic and
casts started preserving identity, which supports the RFC's claim that the
identity rule costs no precision on real code.
