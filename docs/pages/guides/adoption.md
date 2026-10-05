---
title: Adopt WeaveC incrementally
description: Introduce WeaveC to an existing C project, fix its errors, read the ledger, roll out runtime checks and guards, and tighten components with require levels.
---

Start with a component you understand: one with identifiable allocation and cleanup paths and a manageable boundary to external code.

## 1. Reproduce the real build context

Generate a compilation database or pass the same language standard, include paths and defines as the normal compiler. A parser error caused by missing configuration does not tell you anything about ownership.

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
weavec -p build src/buffer.c
```

See [build integration](/guides/build-integration/) for CMake, Make and compiler-driver workflows.

## 2. Fix the errors, then read the warnings

Errors are definite: the bug happens on every execution that reaches the line, and the analysis has found a feasible path that reaches it. Follow the reported allocation, alias, release or escape and fix the code. A finding the analysis could not confirm on a feasible path is a warning with the same id and the note `not confirmed on a feasible path`; its site is checked or guarded like any possible finding. Warnings with "may" wording are temporal bugs on some paths only. `weavec` prints all of them. A `weavec-cc` build with the runtime prints only those it cannot guard, such as a finding at a call boundary; the others trap at run time if they happen, and `-Wweavec-possible` prints them at compile time as well. A `weavec-cc` build prints no `leak` warnings unless `-Wweavec-leak` (or `-Wweavec`) asks for them; `weavec` prints them. Read the ones you see: a guard catches a use of a freed block only while the block is in the quarantine. Check inferred behavior with `--dump-analysis` when a helper's effect is surprising.

During migration, you can lower a particular error to a warning:

```sh
weavec-cc -Wno-error=weavec-use-after-free -c src/buffer.c
```

This changes only the reporting, and the ledger keeps the site as a violation. A lowered temporal violation, or a lowered violation of a callee's or library function's requirement, is guarded against the runtime: it traps only if the bug happens, so a false error lowered this way does not stop a correct program. A spatial or null violation decided from an exact extent keeps its check. Nothing traps unconditionally: a lowered violation the build can neither check nor guard (a temporal one without the runtime) is left `unresolved(lowered)` in the ledger. Remove temporary overrides as the component improves.

## 3. Read the ledger

The ledger lists every operation with its outcome: proven, checked, guarded, violation, unresolved or trusted. Write it for a whole build and look at the summary lines first:

```sh
weavec --whole-program -p build --ledger=build/ledger/
```

`summary.unresolvedReasons` in each ledger counts why operations were left unresolved, and `summary.guardedReasons` counts the same reasons for the operations a guard covers. A guarded operation is enforced at run time, within the limits of a guard; resolving its reason turns it into a proof or a check and removes the guard and its cost. The common reasons point at their fixes:

| Reason           | Typical cause                                                      | What helps                                                                                                                                                                                               |
| ---------------- | ------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `unknown-callee` | A call into code WeaveC cannot see may free or keep a pointer.     | Analyse the definition with the caller ([whole-program analysis](/guides/whole-program/)) or declare the callee's ownership (`WEAVEC_BORROWED`, `WEAVEC_OWNED`). The row carries a suggested annotation. |
| `unknown-extent` | The size of the object behind a pointer is unknown.                | Pass the length and declare it: `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(end)`, `WEAVEC_STRING`.                                                                                                         |
| `raw-cast`       | The pointer was made from a non-pointer value, such as an integer. | Such accesses are guarded; keep code that forges pointers in a narrow, reviewed [unsafe region](/guides/unsafe/).                                                                                        |
| `budget`         | A function exceeded the analysis budget.                           | Split the function, or raise `-fweavec-budget`.                                                                                                                                                          |

Trusted rows are the assumptions the result rests on, such as platform library calls (`system-api`) and unsafe regions (`unsafe`). Review them rather than eliminate them.

## 4. Roll out the runtime checks and guards

Build and run your test suite with `weavec-cc`. Unproven null and bounds obligations are checked by default, the facets the analysis left unresolved are guarded where they have a pointer to look up, and a failing check or guard stops the program at the bad access. To collect every failure in one run, build the tests with `-fweavec-checks=report`, which prints `weavec: runtime check failed: <template> at <file>:<line>:<column>` and continues.

A check or guard that fails is either a real bug or code that relies on undefined behavior that happens to work: reading one element past an array, reading a word at a time past the end of a string's allocation, or using the slack the system allocator leaves after a block (`malloc_usable_size` now returns the requested size). Fix the code; if it is intentional, isolate it in a small `WEAVEC_UNSAFE` region, where spatial and null facets are trusted and get neither checks nor guards. Locals and standard allocations are zero-initialised in the checking modes, so code that read uninitialised memory now reads zeros.

The program now runs on WeaveC's allocator, which holds freed blocks in a quarantine (16 MiB by default; `WEAVEC_RT_QUARANTINE=<bytes>` in the environment changes it) so that a stale pointer finds a dead block. Measure the cost on your own workload before you ship the default mode. On the project's benchmarks it is 1.66 times the CPU time of a plain Clang build for cJSON, 1.85 for zlib and 5.94 for the Lua interpreter; interpreters and tight loops over pointers pay the most. `WEAVEC_RT_STATS=1` in the environment prints the runtime's counters when the program exits. A component that cannot pay builds with `-fno-weavec-runtime` (1.15, 1.00 and 1.09 times on the same benchmarks): its checks stay, its guardable facets are `unresolved` again, and its uses of freed objects are not caught at run time. Units built with and without the runtime link together.

## 5. Tighten a component

Once a component's unresolved rows are understood, make them errors so they cannot come back:

```sh
weavec-cc -fweavec-require=guarded -c src/buffer.c
```

Every operation must then be proven, checked or guarded; the rest are `unresolved-operation` errors. `-fweavec-require=checked` goes further and rejects guarded operations too, with the message `<operation> is guarded at run time only: …`, so every operation is proven or checked against a bound the code states. Trusted operations stay allowed at every level. `WEAVEC_REQUIRE_SAFE` before a function definition applies the same rule to that function alone, whatever the command line says. `-fweavec-require=proven` also rejects operations that rely on a runtime check, for code that must not trap.

## 6. Keep the result reproducible

Run the same build in CI, with `-fweavec-link=analyze` at the link or a `weavec --whole-program` step to report the bugs that span files (a default link does not), keep the ledger as a build artifact (`-fweavec-ledger-format=sarif` for code-scanning tools), and watch the summary counts. Each ledger row has a fingerprint that survives edits elsewhere in the file, so rows can be compared between runs.
