# Root-cause repros

The 12 reduced programs behind the v0.10.0 false-positive root causes (RFC 0030,
*Motivation*). Their markers state the behaviour RFC 0030 intends, so they fail
in `--legacy` by design; they are not part of the S0 gate. Each file's header
says what v0.10.0 reports and what is intended. `main` drivers were added where
missing so the executable oracle runs them; every driver is ASan-clean except
`luaalloc`'s, which reaches its double free.

| Repro | v0.10.0 | Intended | Rule | Gate |
| --- | --- | --- | --- | --- |
| `realloc0.c`, `realloc1.c`, `realloc5.c` | `analysis-incomplete` or nothing (controls) | `CLEAN` | §9.1 | S7 |
| `realloc2.c`, `realloc3.c`, `realloc4.c` | `'b->value' is freed twice` (false) | `CLEAN` | §9.1, §9.3 | S7 |
| `luaalloc.c` | a double-free and a false use-after-free in `tfree2`; misses `tfree` | exactly two definite errors: `use-after-free` in `tfree`, `double-free` in `tfree2` (`EXPECT-LEDGER` on the error and warning counts) | §9.1, §9.3 | S7 |
| `pred.c` | `dereference of 'it', which may be null` (false) | `CLEAN` | §9.2 | S4 |
| `zerolen.c` | `'ab.b', which may be null, is passed to 'write'` (false) | `CLEAN` | §8.3 | S4 |
| `mainleak.c` | a leak at a return from `main` | `CLEAN`, run with and without the input that takes that return | §8.4 | S4 |
| `arrloop.c` | a double-free and four leaks on array cells (false) | no error and no trap; possible `double-free` and `leak` warnings are allowed | §3.1 | — |
| `loopcorr.c` | `dereference of 'p', which may be null` (false) and an out-of-memory leak | no error and no trap (a checked facet); the leak may stay a warning | §3.2 | — |

## Held-out repros (RFC 0031)

One reduced program per false error and false trap that v0.11.0 gave on the
eleven held-out projects (RFC 0031, *Motivation* and §11.1), added in its stage
S0. Each is correct code (built with the reference Clang under
`-fsanitize=address,undefined` and run clean with and without an argument;
the zero-length null arguments of `fwrite` and `strncmp` are defined by C2y and
read nothing), each is `CLEAN` and `ASAN`, and each file's header names the
project, file and line it comes from. All eight fail on v0.11.0 by design.

| Repro | Origin | v0.11.0 |
| --- | --- | --- |
| `ooc-memcpy-element-address.c` | lz4 `lib/lz4.c:561` | false `out-of-bounds` errors: "copies 4 bytes between overlapping ranges" for `&v[4], v` |
| `ooc-unknown-outparam.c` (+ `Inputs/ooc-unknown-outparam-impl.c`, plain Clang) | hiredis `test.c:292-350` | false `use-after-free` and `double-free` errors after `fmt(&cmd, ...)` by an external callee |
| `ooc-realloc-rebase.c` | libyaml `src/api.c:74-154` | 18 false `use-after-move` errors on the stale-pointer arithmetic, and warnings in the driver |
| `ooc-global-dangling-overwritten.c` | http-parser `test.c:2687` | false `lifetime-too-short` error on `current_pause_parser = &s` |
| `ooc-back-pointer.c` (+ `Inputs/ooc-back-pointer-lib.c`, `Inputs/ooc-back-pointer.h`) | bzip2 `bzlib.c:1321` | false `lifetime-too-short` error at link on `strm.state->strm` |
| `ooc-fwrite-null-empty.c` | miniz `miniz_zip.c:2911` | traps (`nonnull`) on `fwrite(NULL, 1, 0, f)` |
| `ooc-strncmp-null-empty.c` | mujs `regexp.c:1174` | traps (`nonnull`) on `strncmp(s, NULL, 0)` |
| `ooc-anon-union-record.c` (+ `Inputs/ooc-anon-union-eval.c`) | tinyexpr `te_eval` | the second unit's record does not decode at link ("invalid object view"): `unanalyzed-input` warning |

`ooc-global-dangling-overwritten.c` follows the real code: the global still
points to the dead local when `parse_pause` returns and is overwritten by the
next call before any read. RFC 0031 §5.7 names this case as a false error in
its second bullet, but its first bullet ("left in a cell reachable from a
... global at the exit") would report it; the case pins the intended `CLEAN`.

