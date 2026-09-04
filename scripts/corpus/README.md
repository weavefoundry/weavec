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
- Every run passes `-ferror-limit=0`: Clang's default stops after 20 errors
  per unit, and a tally capped per unit cannot be compared between runs
  (Lua's `lstrlib.c` and `lauxlib.c` hit the cap). Baselines taken before
  this flag was added undercount those units.
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
callbacks. Lua (34 units as one program, ~6 minutes before RFC 0007, ~12
after it, ~29 since RFC 0008; see below) exercises the model harder than
anything else here, and every one of its reports is a false positive of a
kind an RFC already names:

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

### RFC 0008: pointer validity

RFC 0008 adds 266 `null-dereference`, 4 `invalid-release` and no
`use-of-uninitialized` across the ten projects, and it moves Lua's
ownership rows. The RFC predicted that unchecked allocations would
"dominate the corpus delta until triaged"; they do not. Three shapes the
RFC lists under *Accepted false positives* account for almost everything,
and each project's rows are one of them:

| Project             | Diagnostic                                                                   | Cause                                                                                                                                                                                                                                                                                                                                                                                           |
| ------------------- | ---------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| sds                 | `null-dereference` ×12 on `s` in `sdscatfmt`, `sdscatrepr`, `vector` in `sdssplitargs` | **Genuine on allocation failure.** `s = sdsMakeRoomFor(s, 1)` may return `NULL` (sds documents this) and the next line dereferences it; `vector = s_realloc(...)` likewise. This is the *unchecked `malloc`* shape, and here the library's own contract says the check was owed.                                                                                                       |
| printf              | `null-dereference` ×18 on `buffer` in `_vsnprintf`                            | `if (!buffer) out = _out_null;` then `out(c, buffer, ...)`: the null buffer is only ever handed to the one callback that ignores it, but the per-type join over `_out_*` includes `_out_buffer`, which writes through it. A correlation between a null test and a function-pointer value (RFC 0006, *Accepted false positives*).                                                                  |
| cJSON, cJSON-program | `null-dereference` ×7 / ×24 (`current_item`, `p` at `suffix_object`, `buffer->buffer`) | *Tested-then-merged*: `cJSON_CreateIntArray` tests `n == NULL` and `goto fail`, but the earlier `p = n` on the previous iteration carries the `MaybeNull` fact into the next `suffix_object(p, n)`. `print_number` tests `output_pointer == NULL` and returns, and the remaining path is joined with the untested one. The program view adds `cJSON_Utils.c`'s callers of the same functions. |
| linenoise, linenoise-program | `null-dereference` ×3 (`ab.b` at `write`, `line` ×2)                          | `abAppend` does `if (new == NULL) return;` and leaves `ab->b` may-null for the caller's `write(fd, ab.b, ab.len)` (the summary's null class cannot say "unchanged"); `linenoiseEditFeed` returns `{fresh, null}` and `example.c` dereferences `line` after the `linenoiseEditMore` test, which is an equality with a different pointer. Both *tested-then-merged*.                                        |
| zlib                | `null-dereference` ×25 (`s->gzhead`, `state->x.next`, `state->in`, `next`, `_tr_stored_block(s, (char*)0, ...)`) | `s->gzhead != Z_NULL` is tested in the caller's `switch` arm and dereferenced in the next state's arm: a state machine over an integer (`s->status`) separates them. `state->in`/`state->x.next` are set by `gz_init` behind a `state->size == 0` test. `_tr_stored_block(s, (char*)0, 0L, last)` is a **genuine** report against the table's `requires` for `memcpy`'s source when the length is zero; the RFC defers whether zero-length calls should be exempt. |
| lua                 | `null-dereference` ×174                                                        | One shape: `luaL_checklstring` and its relatives do `if (!s) tag_error(L, arg, t); return s;`, and `tag_error` → `luaL_typeerror` → `luaL_argerror` → `luaL_error` are declared `int`, not `l_noret`, so the failing arm rejoins and every `luaL_check*`/`luaL_opt*` result is may-null. *Tested-then-merged* with an unannotated `noreturn`, exactly the RFC's first accepted false positive; `luaD_throw` *is* `l_noret`, and the paths through it are clean. |
| lua                 | `invalid-release` ×4 (`func` in `luaD_precall` ×3, `b` in `pushline`)           | `lua_readline` returns either the dlsym'd `readline`'s result or the caller's automatic `buffer` (via `fgets`), and `lua_freeline` frees only when `l_readline != NULL`: the two arms are separated by a global the model does not correlate (*May-consume of a may-borrow*). `func` is a `StkId` into `L->stack`, whose reallocation goes through `l_alloc` and its `nsize == 0` arm; the interior pointer reaching that release is the RFC's *start of its allocation* rule applied to the same `l_alloc` shape as the rows below. |
| lua                 | `double-free` 4 → 26, `use-after-free` 5 → 37, `conflicting-borrow` 4 → 10       | The single root cause the `use-after-free` row above already names: `l_alloc`'s `nsize == 0` arm frees, so every `luaM_realloc`-family callee *may* free its argument. RFC 0007 read the callee's `written` effect as a reinitialisation and silently dropped the record; *Replaced values* keeps it unless the callee reinitialised on every freeing path, which `realloc`-on-failure does not. These are the reports that closing that soundness hole costs, and the RFC accepts them (*Replaced values are may-consumed*). |
| lua                 | `lifetime-too-short` 10 → 4, `leak` 5 → 4                                        | Improvements from the same change: `L->errorJmp = lj.previous` and `ls->fs = fs->prev` are now stores that replace the loan (the unlink the RFC 0006 row said the lifetime rule could not see), and `luaZ_freebuffer`'s result store is no longer a leak once its `l_alloc` result is `replaced`.                                                                                                    |

So the corpus answers RFC 0008's deferred questions as follows. The
unchecked-`malloc` share of the delta is the sds rows, and they are real:
nothing beyond the inferred `returns{fresh}` for an aborting wrapper is
needed, because no project here has one. The zero-length `memcpy` question
is one report (zlib) and stays open. Per-element nullness would change
nothing above. The two largest rows, printf and Lua, are both the
`noreturn`/correlation heuristic the RFC shares with Clang's analyzer, and
both have the fix the RFC names: `l_noret` on Lua's error helpers, and a
`WEAVEC_NONNULL`-free `_out_null` path (or `WEAVEC_UNSAFE`) in printf.

Four false-positive shapes found here were fixed before the baseline was
taken, each recorded under *Implementation notes* in the RFC: a redundant
retest (`if (buf != NULL)` inside cJSON's `can_access_at_index` macro on a
pointer already known non-null) no longer weakens the fact; `(char *)0` is
a null constant (zlib's `buf != (char*)0` guards); a call into unchecked
code forgets the nullness of everything reachable through its pointer
arguments (linenoise's `completionCallback(buf, &lc)` fills `lc.cvec`); and
a callee's failing outcome class no longer says a place is null when the
`null{...}` entry only records a `fresh` store that did not happen
(linenoise's `linenoiseEditGrow`, cJSON's `ensure`, zlib's `gz_init`: the
buffer the caller then uses is the old one).

RFC 0008 also made Lua slower again, from ~12 minutes to ~29 on the same
machine and build, and the whole of it is `luaV_execute`: its ~130 calls
into the `luaM_realloc` family used to have their `moved` effect on
`L->stack` masked by the callee's `written` (the RFC 0007 reading), and
*Replaced values* keeps it, so each call now consumes the path, its mirrors
through `L->twups ~ L` and their aliases. Two costs found on the way were
removed before this baseline was taken (the whole-program database was
renumbered from scratch at every changed member, and `BorrowState` kept one
loan per borrow *site*, 1,173 of them per state in `luaV_execute`, where it
now keeps one per borrow); RFC 0008's *Performance* note records what is
left.

### RFC 0009: value-conditional behaviour

RFC 0009 adds no diagnostic and removes 140 reports across the ten
projects (Lua 697 → 566, cJSON 8 → 5 and 35 → 32, zlib 31 → 28); no project reports anything it did not report before (one Lua
`double-free` moved from `ldump.c:301` to `ldump.c:303`, the next call on the
same `D.L->tbclist.p`). The baseline it replaces was taken with Clang's
default `-ferror-limit`, which capped Lua's `lstrlib.c`, `lauxlib.c` and
`lvm.c`, and cJSON-program's `cJSON.c`, at 20 errors each; the numbers below
compare two runs made with the cap lifted (the pre-RFC binary, then this
one), so the "before" figures are larger than the previous baseline's (Lua's
`use-after-free` 37 there is 405 uncapped; cJSON-program's
`null-dereference` 24 is 32).

| Project              | Change                                                                                     | Cause                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| -------------------- | ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| lua                  | `null-dereference` 215 → 92                                                                | `luaL_error`, `luaL_argerror`, `luaL_typeerror`, `tag_error` and `lua_error` are inferred `never-returns` (`luaD_throw` is `l_noret`; every path through them ends there), so the failing arm of `luaL_checklstring` and its relatives no longer rejoins the succeeding one and their results are non-null.                                                                                                                                                            |
| lua                  | `leak` 5 → 1                                                                               | `luaL_alloc` is summarised `ptr: freed\|moved; returns{fresh when[nsize positive\|negative], null}`: called through `g->frealloc` with `nsize` 0 (`callfrealloc(g, block, osize, 0)` in `lmem.c`, the `(*g->frealloc)(..., 0)` in `lstate.c`, `lstring.c`, `lgc.c`) nothing fresh comes back, so "result of a function pointer is leaked" is gone.                                                                                                                              |
| lua                  | `double-free` 35 → 34, `invalid-release` 6 → 5, `conflicting-borrow` 18 → 16              | Guards on a *global*: `lua_freeline` frees its line `when[l_readline nonnull]` and `lua_readline` returns the caller's stack `buffer` `when[l_readline null]`, two guards that cannot both hold, so `lua.c`'s `pushline` no longer releases `buffer`. `luaL_loadfilex` closes `stdin` `when[filename nonnull]`, and `dofile(L, NULL)` passes null. The two `conflicting-borrow`s (`L->top.p` at `luaC_checkfinalizer` in `lapi.c`) were not traced to one summary; they are reallocation shapes downstream of `luaL_alloc`. A third, `newt.node` at `freehash` in `ltable.c`, had gone too, and came back with the unsigned-comparison fix below: it was a false negative, not a gain. |
| lua                  | `use-after-free` 405 → 405, `lifetime-too-short` 10 → 10                                   | Unchanged. 383 of the `use-after-free`s are one shape in `luaV_execute`: `ra`, `base` and `ci->func.p` are interior pointers into `L->stack`, which every `Protect`ed call may reallocate; the code re-derives `base` from `ci->func.p` afterwards (`updatebase`), and the model has no notion of an interior pointer being *rebased*. RFC 0008's *Replaced values* row above.                                                                                                       |
| cJSON, cJSON-program | `null-dereference` 7 → 5 / 32 → 30, `leak` 1 → 0 / 1 → 0                                  | `parse_array`, `parse_object`: `current_item` is null only `when[head null]` after the first iteration, and `if (head == NULL) ... else current_item->next = ...` refutes it. `ensure` stores a fresh `p->buffer` `when[p->noalloc =0]`; `cJSON_PrintPreallocated` sets `p.noalloc = true`, so `p.buffer` (the caller's) is not an owned value it leaks. `cJSON_Utils.c:1189` stays: `if (index > ULONG_MAX)` briefly hid it (see below).                                            |
| zlib                 | `leak` 4 → 2, `null-dereference` 25 → 24                                                   | `gz_error` frees and nulls `state->msg` `when[state->msg nonnull]` and allocates a new one `when[msg nonnull]`; after `gz_error(state, Z_OK, NULL)` the field is null on every path, so `free(state)` in `gzclose_r`/`gzclose_w` leaks nothing. `inflateCopy`: `window` is null `when[state->window null]` and otherwise non-null, so `if (window != Z_NULL)` learns `state->window` non-null before the `zmemcpy`.                                                                   |

The wall-clock cost is recorded in the RFC's *Performance* note: Lua as
one program went from 26.0 to 33.0–34.5 minutes on the same machine (two
runs; +27–33%), of which the per-unit overhead is about 14%; the rest is
the whole-program fixpoint.

Three checker bugs were found by this triage and fixed before the baseline
was taken; the RFC's *Deriving guards* and *Assumptions* record them. A
value gone at a callee's exit only under a guard was summarised as an
unconditional unreplaced consume (Lua's `str_writer` under `dumpBlock`
reported 19 `double-free`s in `ldump.c`); a replaced consume that found the
value already gone kept the stale record, so every later call reported
again; and an integer constant was read as its two's-complement bits rather
than its value, so `ULONG_MAX` was `-1` and the edge on which
`if (index > ULONG_MAX)` fails was infeasible for `index = 0`, which
silently dropped a `null-dereference` in `cJSON_Utils.c` (a false
negative, the kind of error this corpus exists to catch).

### RFC 0010: shared ownership

RFC 0010 (reference counts, per-outcome stores and facts, stores out of
sight) added **jansson** to the corpus: a reference-counted JSON library
whose `json_incref`/`json_decref` keep `json_t::refcount` with
`__atomic_add_fetch`/`__atomic_sub_fetch` (inline in `jansson.h`), whose
`json_decref` frees through `json_delete` in another unit, and whose
containers keep what they are given in hash-table nodes of their own. Its
configured headers are committed under `support/jansson/` (the `{support}`
expansion in `projects.json`). On the existing projects the RFC changed one
Lua `use-after-free` (below); every other count is unchanged.

| Project | Change                                                                 | Cause                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ------- | ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| jansson | `double-free` 50 → 45, `use-after-free` 33 → 31, `conflicting-borrow` 1 → 0 (pre-RFC binary → this one) | `json_decref` is inferred a share release (`freed(free),share`, `count param 0 *.refcount`) across the unit boundary to `json_delete`; `json_pack`'s `object`/`array` and `json_array_copy`'s `result` are released, not freed, so the sibling shares the containers took (`json_object_set_new` → `hashtable_set`) are not double frees, and `json_object` is not freed while `&object->json` is borrowed. |
| jansson | `leak` 10 → 10                                                         | 11 leaks appeared and went during the RFC: every `json_object_setn_nocheck(object, key, len, value)` loop (`json_object_update*`, `json_object_copy`, `do_object_update_recursive`) retained `value` and, with `hashtable_set` storing it in a `pair_t` the summary could not name, lost the share. *Stores out of sight* (`param 3: escaped` on `hashtable_set`, composed through `json_object_setn_new_nocheck` and the inline `json_object_setn_nocheck(…, json_incref(value))`) removed them. |
| lua     | `use-after-free` 407 → 408; a `conflicting-borrow` at `lstate.c:389` reads "cannot move" instead of "cannot free" | `lapi.c:344`, `L->top.p--` after `luaO_arith(L, …)`: the interior-pointer-into-`L->stack` shape of the 383 in `luaV_execute` (RFC 0008, *Replaced values*). It appears because the whole-program fixpoint stops at `MaxRounds` (8) without converging on Lua (30 of 31 units are still dirty in the last round, before and after this RFC), so which summaries a unit sees last depends on the schedule, and the dirty-tracking change altered it. Not a checker change. |

What jansson still reports, all false positives, each an instance of a
limitation an RFC names:

| Diagnostic                                                     | Count | Cause                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| -------------------------------------------------------------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `double-free` on `lex->saved_text.value`, `strbuff.value`, ...  | 38    | `strbuffer_append_bytes` grows the buffer through `jsonp_realloc` (malloc, copy, free, or the user's `do_realloc` function pointer) and stores the new one: `freed,replaced` on its zero class, `freed` (the failed-realloc path through the pointer) on its negative one. `lex_save` ignores the result, so the classes merge and `replaced` is lost, and the next byte appended is a second free (RFC 0008, *Replaced values*: a must-fact; RFC 0006, an untested outcome). |
| `use-after-free` on `p`, `saved_text`, `array->table`           | 24    | The same buffers used after the merged consume above (`p = strbuffer_value(&lex->saved_text) + 1; while (*p …)`), and `json_array_extend`/`json_array_insert` reading `array->table` after `json_array_grow` reallocated it: the interior-pointer rebasing shape (RFC 0008, *Deliberately not caught*).                                                                                                                                                       |
| `double-free` / `use-after-free` on `object`, `result` (released) | 7  | `json_object_setn_new_nocheck` releases `value` when `json == value`; on that edge `json` and `value` are one place (RFC 0006's pointer-equality alias), so the summary says `param 0: freed,share` and every `json_object_setn_nocheck(object, …)` loop releases `object`. A conditional release whose condition is the equality of two arguments has no summary spelling; candidate for RFC 0006's *Unresolved questions*.                                                                        |
| `leak` of `object`, `array`, `string`, `integer`, `real`, `pair` | 6    | `return &object->json;` hands out the address of the first field of the fresh object (the embedded-base idiom, `json_to_object` casts back) and `insert_to_bucket(…, &pair->list)` links the node by an interior pointer: the owner is not seen to escape (RFC 0002, interior pointers; RFC 0007, *Escape*).                                                                                                                                                                                       |
| `leak` of `buf.value`, `output.value`, `output->value`          | 4     | `json_dumpb`/`json_dumps` pass `&buf` to `json_dump_callback`, which stores through a `json_dump_callback_t` function pointer into `buf.value`; the callback's summary is not the pointer's (RFC 0004, *Function pointers*).                                                                                                                                                                                                                                                                              |
| `lifetime-too-short` on `parents_set.buckets->last`, ...        | 8     | `hashtable_init(&parents_set)` on a stack table stores `&parents_set.list` into memory it allocates (`buckets->last = &hashtable->list`); the RFC 0002 lifetime check reads that as a stack address outliving the frame, though `hashtable_close` frees the buckets before the return.                                                                                                                                                                                                             |
| `null-dereference` ×2, `invalid-release` ×1, `annotation-required` ×2 | 5 | `json_vpack_ex`'s `str` under a `%s?` format is null only when a flag says so (a fact about a `char` in a format string); `json_string("")` reaches `string_create(value, len, own)`, where `v` is `value` itself when `own` and a copy otherwise and the failure path frees `v`; the RFC 0009 guard on `own` that `json_stringn_nocheck`'s constant `0` would refute does not survive the conditional alias, so the literal is "released" (RFC 0008, *Invalid releases*; not traced further); `dtoa_r` is an unannotated extern and `stream->callback` a function pointer with no address-taken candidate. |

Timing, release build (`-DCMAKE_BUILD_TYPE=Release`), same machine: Lua as
one program 3 min 16 s with the pre-RFC binary, 2 min 16 s with the
share-tracking parts of this RFC and the dirty-tracking fixpoint (the same
eight rounds; the saving is cheaper rounds, not fewer runs), and 3 min 22 s
with *Stores out of sight* on top (`escaped` effects are resolved at every
call; the RFC's *Unresolved questions* has the follow-up). That last figure
is the quietest of several runs taken while the machine was busy with other
work, so it is an upper bound. jansson as one program: 0.9 s. Debug builds
are roughly ten times slower on Lua; the 33 minutes in the RFC 0009 note
above was one.
