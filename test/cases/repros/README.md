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
