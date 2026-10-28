# Determinism cases

RFC 0036 §9 and gate G1: each bug of the RFC's motivation, and the other
bugs it names, stops with its report kind in every run, however much the
program allocated in between; each `_bug.c` has a correct twin, `_ok.c`,
that runs clean.

| Case | Bug | Today |
| --- | --- | --- |
| `uaf-after-churn` | a call through a released block after ~28 MiB of allocations | stops (reuse after a sweep, §3) |
| `uaf-spray` | a read through a released block after 64 MiB of reuse | stops (§3) |
| `double-free-after-churn` | a second release after the block's slot could have been reused | stops (§3) |
| `field-strcpy` | `strcpy` past an 8-byte field into the next one | stops (the fortified size, §6) |
| `asan-pool` | an over-read of a pool object the pool poisons through ASan's interface | stops (§5) |
| `escaping-oob` | a 1-based array read one past its end in another function | stops (the slot's tail) |
| `shared-frame` | (twin only) recursion through locals in disjoint `switch` arms to 10,000 frames | runs (shared frame space, §2) |
| `heap-jump` | an index from one heap block that lands in a neighbouring live block | `XFAIL`: a later RFC |
| `global-jump` | a read through a pointer from one global into the next | `XFAIL`: a later RFC |
| `use-after-return` | a read through the address of a returned function's local | `XFAIL`: a later RFC |
| `use-after-longjmp` | a read through a local of a frame `longjmp` abandoned | `XFAIL`: a later RFC |

The `XFAIL` cases pin the bugs RFC 0036 leaves to a later RFC (spatial
determinism and use after return); the runner reports `XPASS` when one
starts to stop, and its marker then goes.
