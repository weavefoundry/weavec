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
| `heap-jump` | an index from one heap block that lands in a neighboring live block | stops (colors, RFC 0037 §8) |
| `global-jump` | a read through a pointer from one global into the next | stops (colors, RFC 0037 §8) |
| `use-after-return` | a read through the address of a returned function's local | stops (returned frames, RFC 0037 §9) |
| `use-after-longjmp` | a read through a local of a frame `longjmp` abandoned | `XFAIL`: the jump clears the stack's shadow |
| `loop-range-wrap` | a loop whose range wraps 64 bits over a block whose size wrapped | stops (RFC 0037 §4) |
| `large-atomic` | an atomic load too large to be lock-free, past its object | stops (RFC 0037 §4) |

The `XFAIL` case pins a bug no RFC catches yet; the runner reports `XPASS`
when one starts to stop, and its marker then goes.
