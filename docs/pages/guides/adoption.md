---
title: Adopt WeaveC incrementally
description: Introduce WeaveC to an existing C project, run its tests under the guarded build, read what traps, fix it, and measure the cost.
---

Start with a component you understand: one with identifiable allocation and cleanup paths and a test suite that exercises them.

## 1. Build and test with weavec-cc

Point the build at `weavec-cc` and run the tests you already have:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc
cmake --build build-weavec
ctest --test-dir build-weavec
```

For Make and configure scripts, `make CC=weavec-cc` or `./configure CC=weavec-cc`. A correct program builds and passes as it does with Clang. Every memory access is guarded, so a test that trapped found a memory error at the access that made it. See [build integration](/guides/build-integration/) for the details of the link.

## 2. Survey what traps

A first run may stop at the first of several problems. To see them all, build the tests with `-fweavec-checks=report`: each failing site prints its report once and the program goes on. If the test harness captures standard error, set `WEAVEC_RT_REPORT_LOG=<path>` and the reports go to that file instead.

```sh
cmake -S . -B build-survey -G Ninja -DCMAKE_C_COMPILER=weavec-cc \
  -DCMAKE_C_FLAGS="-fweavec-checks=report"
cmake --build build-survey
WEAVEC_RT_REPORT_LOG=$PWD/reports.log ctest --test-dir build-survey
```

Keep the trapping build (`trap`, the default) for CI: in `report` mode the program goes on after a bad access, through memory it should not have touched.

## 3. Read each report

Each report names the kind of failure, the source location of the access and its address; a second line places the address relative to the object the pointer came from:

```text
weavec: heap-use-after-free at buffer.c:88:12: read of 8 bytes at 0x...
weavec: 0x... is inside a released heap block at 0x...
```

`WEAVEC_RT_BACKTRACE=1` adds the stack of the access that stopped the program. The [command-line reference](/reference/cli/#check-modes) lists the report kinds, and [troubleshooting](/reference/troubleshooting/) the common causes.

A trap is either a real bug or code that relies on undefined behaviour that happens to work: reading one element past an array, reading a word at a time past the end of a string's allocation, using the slack the system allocator leaves after a block (`malloc_usable_size` returns the requested size), or an index computed from one object that lands in another. Fix the code. Code written to be clean under AddressSanitizer needs nothing more: `weavec-cc` compiles it as ASan would (`__SANITIZE_ADDRESS__` and `__has_feature(address_sanitizer)` are set), honours its `no_sanitize("address")` functions, and implements ASan's poisoning interface. If an over-read is intentional and harmless, and the project does not already keep it from ASan, put it in a small `WEAVEC_UNSAFE` region as a last resort: its accesses are not guarded. See [unsafe boundaries](/guides/unsafe/).

Locals are zero-initialised in the enforcing modes and the runtime's heap is zero-filled, so code that read uninitialised memory now reads zeros.

## 4. Measure the cost

The program now runs on WeaveC's allocator, and every access the local rules cannot prove runs a guard. Measure on your own workload before you ship a guarded build: interpreters and tight loops over pointers pay the most. `WEAVEC_RT_STATS=1` in the environment prints the runtime's counters when the program exits, and `-fweavec-summary` (or a ledger, `-fweavec-ledger=<dir>/`) prints how many accesses of each file kept their guard:

```text
weavec: buffer.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded
```

A freed block waits in a quarantine and is reused only after a _sweep_ (the program's threads stopped, its memory scanned) finds no pointer to it. A block that some stale pointer still names stays allocated until that pointer is overwritten, so a program that keeps dangling pointers keeps their memory, and a program with a large live heap pauses for each sweep. `WEAVEC_RT_STATS=1` reports the sweeps, their longest pause and the bytes they retained; `WEAVEC_RT_QUARANTINE=<bytes>` sets how much released memory waits before one runs (16 MiB, or a quarter of the live heap if that is larger). On Linux the sweep stops threads with a signal (`SIGPWR`, or `SIGXCPU` when the program handles `SIGPWR`); `WEAVEC_RT_SWEEP_SIGNAL=<number>` names another.

A component that cannot afford the guards can build with `-fweavec-checks=none`, which compiles it as Clang does and gives up its protection; units built with and without guards link together.

## 5. Keep the result reproducible

Run the guarded build and its tests in CI, and let a trap fail the job. Set `WEAVEC_RT_REPORT_LOG` so the reports land in a file the job uploads. Keep the enforcement ledgers as build artifacts if you track how much of the code runs guarded, and review every new `WEAVEC_UNSAFE` region as you would any other unchecked code.
