---
title: Adopt WeaveC incrementally
description: Introduce WeaveC to an existing C project, run its tests under the guarded build, fix what traps, measure the cost, and add the advisory analysis to review and CI.
---

Start with a component you understand: one with identifiable allocation and cleanup paths and a test suite that exercises them.

## 1. Build and test with weavec-cc

Point the build at `weavec-cc` and run the tests you already have:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc
cmake --build build-weavec
ctest --test-dir build-weavec
```

A correct program builds and passes as it does with Clang. Every memory access is guarded, so a test that trapped found a memory error at the access that made it. See [build integration](/guides/build-integration/) for Make and other build systems.

## 2. Read what traps

Each trap prints the kind of failure, the source location of the access and its address; for the heap, a second line places the address in its block:

```text
weavec: heap-use-after-free at buffer.c:88:12: read of 8 bytes at 0x...
weavec: 0x... is inside a released heap block at 0x...
```

To collect every failure in one run, build the tests with `-fweavec-checks=report`: each failing site prints once and the program goes on. If the test harness captures standard error, set `WEAVEC_RT_REPORT_LOG=<path>` and the reports go to that file instead. The [command-line reference](/reference/cli/#check-modes) lists the report kinds.

A trap is either a real bug or code that relies on undefined behaviour that happens to work: reading one element past an array, reading a word at a time past the end of a string's allocation, using the slack the system allocator leaves after a block (`malloc_usable_size` now returns the requested size), or indexing a two-dimensional array through its first row. Fix the code. If the over-read is intentional and harmless, put it in a small `WEAVEC_UNSAFE` region, or keep it in a function the project already excludes from AddressSanitizer with `no_sanitize("address")`: neither is guarded. See [unsafe boundaries](/guides/unsafe/).

Locals are zero-initialised in the enforcing modes and the runtime's heap is zero-filled, so code that read uninitialised memory now reads zeros.

## 3. Run the analysis

The ownership analysis reports bugs the tests do not reach: a use after free on an error path, a double free, a dangling pointer returned from a function, a leak. Run it on the same sources, with the build's flags:

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
weavec -p build src/buffer.c
weavec --whole-program -p build
```

Errors are definite: the bug happens on every execution that reaches the line, and the analysis has found a feasible path that reaches it. Follow the reported allocation, alias, release or escape and fix the code. Warnings with "may" wording are temporal bugs on some paths only. Check a helper's inferred behaviour with `--dump-analysis` when its effect is surprising, and add [annotations](/reference/annotations/) where inference needs help, typically at public interfaces.

The analysis is advisory: it does not change what `weavec-cc` compiles, and a false finding does not stop a build. To see its findings during a build, add `-fweavec-diagnose`, which prints them as warnings.

## 4. Measure the cost

The program now runs on WeaveC's allocator, which holds freed blocks in a quarantine (16 MiB by default; `WEAVEC_RT_QUARANTINE=<bytes>` changes it), and every access the local rules cannot prove runs a guard. Measure on your own workload before you ship a guarded build: interpreters and tight loops over pointers pay the most. `WEAVEC_RT_STATS=1` in the environment prints the runtime's counters when the program exits, and `-fweavec-ledger=<dir>/` with `-fweavec-summary` shows how many accesses kept their guard:

```text
weavec: buffer.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded
```

A hot loop over an array whose trip count is known runs without guards when its whole range is addressable. A component that cannot afford the guards can build with `-fweavec-checks=none`, which compiles it as Clang does and gives up its protection; units built with and without guards link together.

## 5. Keep the result reproducible

Run the guarded build and its tests in CI, and let a trap fail the job. Add a `weavec --whole-program -p build` step, or build with `-fweavec-diagnose -Werror=weavec`, to fail the job on what the analysis reports; the second turns a false error into a failed build, so start with the first and review its findings. Keep the enforcement ledgers as build artifacts if you track how much of the code runs guarded.
