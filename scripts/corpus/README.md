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
  `"whole_program": true` runs `weavec --whole-program` once over all the
  files (RFC 0005) instead of once per file; `--local-whole-program` does
  the same for `--local`. Checkouts go under `build/corpus/`; `--refresh`
  re-fetches them.
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

RFC 0005 (whole-program analysis) added the `*-program` entries, which
analyse two files of a project together. The single-file rows did not move:
the struct-copy rule and the program tier cost nothing on the single-unit
view. The whole-program rows show both what the RFC set out to do and what
it exposes:

| Project           | Diagnostic                                                      | Cause                                                                                                                                                                                                                                                                                                                                                                                    |
| ----------------- | --------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| cJSON-program     | (none of `cJSON_Utils.c`'s 20 `annotation-required`)            | Alone, `cJSON_Utils.c` warns at every call into `cJSON.c` (`cJSON_free`, `cJSON_Delete`, `cJSON_Duplicate`, ...). As a program every one resolves to a summary: the boundary the RFC moved is the whole of that file's noise.                                                                                                                                                             |
| cJSON-program     | `use-after-free` + `double-free` on `object->string` (876, 878) | `overwrite_item` frees `root->string` and then `memcpy(root, &replacement, ...)` replaces the whole struct. RFC 0003 effects are a set, not a sequence: `written` on `*root` does not reinstate the `freed` descendant at the call site. `cJSON_free` was unknown in the single-unit view, which is why this did not show before. Candidate refinement: a `written` effect on a place forgets what is below it. |
| cJSON-program     | `double-free` on `object` at `apply_patch(...)` (1056, 1085)   | Path-insensitive join (RFC 0002): on the `MOVE` arm `value` is `detach_path(object, ...)`, whose summary (through `get_item_from_pointer`, which returns its argument for an empty pointer) may return `object` itself; on the other arms it is fresh. At `cleanup:` `cJSON_Delete(value)` therefore *may* free `object`, and the next loop iteration frees it again. Real code separates the arms by `opcode`.                                                          |
| linenoise-program | `annotation-required` ×1 (down from 3)                          | `example.c` installs the completion and hints callbacks, so those two indirect calls now have candidates; `freeHintsCallback` is never set anywhere in the program and remains a boundary, as it should.                                                                                                                                                                                  |
| linenoise-program | 14 `use-after-free`, 1 `double-free` on `line` in `example.c`   | One root cause. `linenoiseEditFeed` returns either a fresh line or the sentinel global `linenoiseEditMore`, so `linenoise()` returns `{fresh, copy linenoiseEditMore}` and `free(line)` may free the global; the guard `if (line != linenoiseEditMore)` is pointer equality, which RFC 0002 deferred. Every later use of `line` in the loop, and the free on the next iteration, then reports. Pointer-equality guards are the highest-value refinement the corpus points at. |

Analysis time for the two-file programs is about the sum of the two
single-file runs plus one extra parse (discovery), as the RFC's cost model
predicts.

RFC 0006 (precision) removed every `use-after-free` and `conflicting-borrow`
row above and all but one of the `double-free` rows, without adding any:

| Was                                                            | Now                                                                                                                                                                                                     |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| linenoise `double-free` ×3 on `*lc->cvec`, `*history`          | Gone: element witnesses. `free(a[i])` in a loop frees element `i`, and the next iteration's `i` is a different (unknown) element.                                                                        |
| linenoise `conflicting-borrow` ×2 at `freeCompletions(&ctable)`| Gone: the `lc != &ctable` guard is a pointer-equality condition fact, and a borrow held by a dead local no longer counts.                                                                                |
| cJSON-program `use-after-free` + `double-free` on `object->string` | Gone: `written` on `*root` forgets what lies below it.                                                                                                                                              |
| cJSON-program `double-free` on `object` ×2 at `apply_patch`    | **Stays.** The arms are separated by an integer `opcode` that has nothing to do with the pointers; condition facts do not cover it (RFC 0006, *Accepted false positives*).                                |
| linenoise-program 14 `use-after-free` + 1 `double-free` on `line` | Gone: `(res = feed()) == sentinel` separates on the exit edge, so `linenoise()` returns `{fresh}` and `free(line)` frees only that.                                                                   |

Two larger projects were added as precision canaries. zlib (15 units as one
program) is clean apart from two boundary warnings for its `in`/`out`
callbacks. Lua (34 units as one program, ~12 minutes since RFC 0007, ~6
before; see below) exercises the model harder than anything else here, and
every one of its reports is a false positive of a kind an RFC already names:

| Project | Diagnostic                                                                | Cause                                                                                                                                                                                                                                                                                         |
| ------- | ------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| lua     | `lifetime-too-short` ×10 (`fs->bl = bl`, `ls->fs = new_fs`, `L->errorJmp = &lj`, `endptr`) | The linked-stack-of-locals idiom: a local's address is stored into a longer-lived struct and unlinked again before the local dies. RFC 0001's lifetime rule cannot see the unlink; RFC 0006 lists it under *Accepted false positives* for loans.                                    |
| lua     | `use-after-free` ×5 (`oldstack`, `tb->hash`, `buff->b`)                   | `realloc` through `l_alloc`, which also does `if (nsize == 0) { free(ptr); return NULL; }`. Its null class therefore *may* free, and a `newp == NULL` test cannot retract (RFC 0006 tracks the result, not the size argument). `buff->b` adds a `?:` whose arms are separated by a pointer test on a different place. |
| lua     | `double-free` ×4 (`block`, `dummy`, `stdin` ×2)                           | The same `l_alloc` shape (`firsttry` then `tryagain` on the same block), and `luaL_loadfilex` closing `stdin` only `if (filename)`: a correlation between a parameter and which global was used.                                                                                             |
| lua     | `conflicting-borrow` ×4 (`luaM_free(L, l)` in `luaE_freethread`, `L->l_G` ×2 in `close_state`, `newt.node` in `luaH_resize`) | An interior pointer into the object being freed is still live: `l` points into `L1` (`fromstate`), `L` is `&g->mainth.l` inside the `global_State` that `close_state` frees, `newt.lastfree` points into `newt.node`. RFC 0001's conflict rule; the holder dies with the object. The `close_state` and `luaH_resize` reports appeared with RFC 0007: the coarse `*L: written` that every callee used to report had made callers forget the very aliases the rule needs (RFC 0006, *What a callee wrote, its caller wrote*). |
| lua     | `annotation-required` at `lua.c:498`                                      | `l_getenv` is a function-pointer variable set to `getenv` or `no_getenv`; neither is address-taken *as that type* in the program.                                                                                                                                                              |

Lua also found three scalability problems that would have made the corpus
run unusable, all fixed in the same change: `luaV_execute` (968 CFG blocks,
80 block-scoped `StkId ra` locals whose scope ends never appear in the CFG
under computed `goto`) grew a dense alias relation over dead variables,
`PlaceTable::descendants` scanned the whole table at every call site, and
`BorrowState` was an unsorted vector. The unit went from 56 s to 3 s; the
whole program from 23 minutes to under 6.

### RFC 0007: leaks

The leak check adds 18 `leak` reports across the ten projects and no
`mismatched-release`. Six are genuine; the rest fall into three shapes the
summaries cannot express, each already named by RFC 0006 or RFC 0007:

| Project             | Diagnostic                                                                                  | Cause                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| jsmn                | `leak` ×6 (`js`, `tok` at `return 1/2/3` in `jsondump.c`'s `main`)                          | **Genuine.** The example's error exits return from `main` without freeing the growing buffers. RFC 0007 does not exempt `main` (*Unresolved questions*); these are the reports that decision costs, and they are correct.                                                                                                                                                       |
| cJSON, cJSON-program | `leak` on `p.buffer` in `cJSON_PrintPreallocated`                                           | `print_value` may store a fresh buffer into `p->buffer` — but only when `p->noalloc` is false, and this caller set it true. An integer field separates the arms (RFC 0006, *Accepted false positives*).                                                                                                                                                                       |
| linenoise-program   | `leak` on `ls.buf` at the end of `main`                                                     | `linenoiseEditFeed` may grow `l->buf` with `realloc` — only when `l->buflen_max` is non-zero, and `linenoiseEditStart` with a caller buffer sets it to zero. The same integer correlation.                                                                                                                                                                                       |
| zlib                | `leak` ×2 on `stream.state` after `deflateInit`/`inflateInit` fails (`compress.c`, `uncompr.c`) | `deflateInit2_` ends in `return deflateReset(strm)`, whose error class (a state check that cannot fail on a state just built) leaves the fresh `strm->state` in place. The `!= Z_OK` class therefore *may* hold the allocation; *Per-outcome null stores* can only say where a class holds nothing.                                                                            |
| zlib                | `leak` ×2 on `state->msg` when `state` is freed (`gzclose_r`, `gzclose_w`)                  | `gz_error(state, Z_OK, NULL)` frees the old message and returns early because `msg` is null; on its other paths it stores a fresh message. The store depends on an argument's value, which the summary has no class for.                                                                                                                                                       |
| lua                 | `leak` ×5 (`result of a function pointer` ×4, `p.buff.buffer` in `luaD_protectedparser`)   | Lua frees through its allocator: `(*g->frealloc)(ud, block, osize, 0)` returns null and `luaZ_resizebuffer(L, buff, 0)` stores it. The `lua_Alloc` summary returns `{fresh, null}` and nothing ties the fresh alternative to `nsize != 0`, so the discarded result, and the value stored into `p.buff.buffer`, look like allocations. The same size-argument blindness as the `use-after-free` row above. |

RFC 0007 also made Lua slower: about twice the time on the same machine.
Every callee's writes below a mutably borrowed argument are now copied into
the caller's summary path by path, where the coarse `*L: written` used to
stand for all of them and, applied at the caller, erased everything the
caller knew below `L` (the `close_state` row above). Lua threads one state
pointer through every call, so its summaries carry an order of magnitude
more written paths and every call applies them; RFC 0007's *Performance*
section records what was done about it and what is left.
