---
title: Meet WeaveC
description: What WeaveC does, where it fits in a C toolchain, and how it proves, checks and records the memory safety of existing C code.
---

WeaveC brings inferred ownership and borrowing to existing C code. It uses Clang to understand your program, follows how pointers are allocated, shared, moved and released, and reports memory errors with the source locations that explain them. Where it cannot prove an access in bounds or a pointer non-null, the compiler inserts a check that stops the program before the bad access happens.

You can start with one file. You can also use `weavec-cc` as the compiler in an existing build; it analyses each file as it compiles it and the whole program when it links.

## Two ways to use it

| Tool        | Use it when                                                | Example                        |
| ----------- | ---------------------------------------------------------- | ------------------------------ |
| `weavec`    | You want the analysis and its ledger without building.     | `weavec example.c -- -std=c17` |
| `weavec-cc` | You want checked binaries, analysed as part of your build. | `CC=weavec-cc make`            |

Both use the same model and produce the same ledger. The analysis tool accepts a compilation database, so it can use the includes, defines and language flags from your build.

## Prove, check, or record

For every memory operation, WeaveC decides each safety _facet_ (spatial, null and temporal) and records one outcome:

- **proven**: the analysis shows it holds;
- **checked**: not proven, so `weavec-cc` inserts a runtime check that traps before the operation if it would fail;
- **violation**: it fails on every execution that reaches it, which is a compile error;
- **unresolved** or **trusted**: neither proven nor checkable, with a reason, such as an unknown array size or a call into a library WeaveC cannot see.

Definite bugs are errors. Use-after-free and similar temporal bugs that happen only on some paths are warnings, because a runtime check cannot catch them. Every outcome is listed in a JSON or SARIF _ledger_, and each file ends with a one-line summary. The [safety guarantees](/reference/guarantees/) page states what the outcomes guarantee and under which assumptions.

## Start with inference

Function bodies often tell WeaveC enough to infer a contract. A helper that calls `free` consumes its argument. A constructor can describe the memory it returns. Callers are checked against those facts, including across source files when the program is linked.

Annotations state a contract where inference needs help, especially at public interfaces: ownership (`WEAVEC_OWNED`, `WEAVEC_BORROWED`), nullability, and extents such as `WEAVEC_COUNTED_BY(n)`, which turns an unknown array size into a checked one. They live in `weavec.h` and expand to nothing under other compilers.

## Choose how strict to be

By default, the unresolved and trusted outcomes are listed, not rejected. When a component should admit only proven or checked operations, build it with `-fweavec-require=checked`, or mark a function `WEAVEC_REQUIRE_SAFE`. `-fweavec-require=proven` also rejects operations that rely on a runtime check.

## Where to go next

1. [Install from source](/getting-started/installation/) on macOS or Linux.
2. [Run your first check](/getting-started/first-check/): fix a use-after-free, then watch a runtime check trap.
3. [Connect an existing build](/guides/build-integration/) and [adopt WeaveC incrementally](/guides/adoption/).

## Project status

WeaveC is early software. Source releases are available; portable prebuilt binaries and package-manager distribution are future work. Flags, diagnostics and on-disk formats can change between minor versions. The model described here is [RFC 0030](/rfcs/0030-prove-or-trap/), which is being implemented; the [roadmap](/project/roadmap/) tracks progress.
