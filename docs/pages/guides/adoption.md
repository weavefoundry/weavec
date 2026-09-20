---
title: Adopt WeaveC incrementally
description: Introduce WeaveC to an existing C project, fix its errors, read the ledger, roll out runtime checks, and tighten components with require levels.
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

Errors are definite: the bug happens on every execution that reaches the line. Follow the reported allocation, alias, release or escape and fix the code. Warnings with "may" wording are temporal bugs on some paths only; read each one, because the checks inserted at run time do not cover them. Check inferred behavior with `--dump-analysis` when a helper's effect is surprising.

During migration, you can lower a particular error to a warning:

```sh
weavec-cc -Wno-error=weavec-use-after-free -c src/buffer.c
```

This changes only the reporting. The lowered site is still compiled behind a check that traps, and the ledger keeps it as a violation. Remove temporary overrides as the component improves.

## 3. Read the ledger

The ledger lists every operation with its outcome: proven, checked, violation, unresolved or trusted. Write it for a whole build and look at the summary lines first:

```sh
weavec --whole-program -p build --ledger=build/ledger/
```

`summary.unresolvedReasons` in each ledger counts why operations were left unresolved. The common reasons point at their fixes:

| Reason           | Typical cause                                                  | What helps                                                                                                                                                                               |
| ---------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `unknown-callee` | A call into code WeaveC cannot see may free or keep a pointer. | Link the definition in ([whole-program analysis](/guides/whole-program/)) or declare the callee's ownership (`WEAVEC_BORROWED`, `WEAVEC_OWNED`). The row carries a suggested annotation. |
| `unknown-extent` | The size of the object behind a pointer is unknown.            | Pass the length and declare it: `WEAVEC_COUNTED_BY(n)`, `WEAVEC_ENDED_BY(end)`, `WEAVEC_STRING`.                                                                                         |
| `raw-cast`       | The pointer was made from a non-pointer value.                 | Keep such code in a narrow, reviewed [unsafe region](/guides/unsafe/).                                                                                                                   |
| `budget`         | A function exceeded the analysis budget.                       | Split the function, or raise `-fweavec-budget`.                                                                                                                                          |

Trusted rows are the assumptions the result rests on, such as platform library calls (`system-api`) and unsafe regions (`unsafe`). Review them rather than eliminate them.

## 4. Roll out the runtime checks

Build and run your test suite with `weavec-cc`. Unproven null and bounds obligations are checked by default, and a failing check stops the program at the bad access. To collect every failing check in one run, build the tests with `-fweavec-checks=report`, which prints `weavec: runtime check failed: <template> at <file>:<line>:<column>` and continues.

A check that fails is either a real bug or code that relies on undefined behavior that happens to work, such as reading one element past an array. Fix the code; if it is intentional, isolate it in a small `WEAVEC_UNSAFE` region. Locals and standard allocations are zero-initialised in the checking modes, so code that read uninitialised memory now reads zeros.

## 5. Tighten a component

Once a component's unresolved rows are understood, make them errors so they cannot come back:

```sh
weavec-cc -fweavec-require=checked -c src/buffer.c
```

Every operation must then be proven or checked; the rest are `unresolved-operation` errors. Trusted operations stay allowed. `WEAVEC_REQUIRE_SAFE` before a function definition applies the same rule to that function alone, whatever the command line says. `-fweavec-require=proven` also rejects operations that rely on a runtime check, for code that must not trap.

## 6. Keep the result reproducible

Run the same build in CI, keep the ledger as a build artifact (`-fweavec-ledger-format=sarif` for code-scanning tools), and watch the summary counts. Each ledger row has a fingerprint that survives edits elsewhere in the file, so rows can be compared between runs.
