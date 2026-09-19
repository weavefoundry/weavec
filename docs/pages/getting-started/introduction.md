---
title: Meet WeaveC
description: What WeaveC does, where it fits in a C toolchain, and how to start using inferred ownership and checked safety.
---

WeaveC brings inferred ownership and borrowing to existing C code. It uses Clang to understand your program, follows how pointers are allocated, shared, moved, and released, and reports memory errors with the source locations that explain them.

You can start with one file. You can also use `weavec-cc` as the compiler in an existing build and analyze the program across source files at link time.

## Two ways to use it

| Tool        | Use it when                                           | Example                        |
| ----------- | ----------------------------------------------------- | ------------------------------ |
| `weavec`    | You want analysis without producing a binary.         | `weavec example.c -- -std=c17` |
| `weavec-cc` | You want checking as part of compilation and linking. | `CC=weavec-cc make`            |

Both use the same ownership model. The analysis tool accepts a compilation database, so it can use the includes, defines, and language flags from your build.

## Start with inference

Function bodies often tell WeaveC enough to infer a contract. A helper that calls `free` consumes its argument. A constructor can describe the memory it returns. Callers are checked against those facts, including across source files in whole-program mode.

Annotations let you state a contract where inference needs help, especially at public interfaces. They live in `weavec.h` and preserve C source portability. Start with the code you have, then add precise annotations where they are useful.

## Choose the guarantee you need

**Ordinary analysis** reports the errors and warnings it can establish. This is useful for finding bugs and introducing the tool incrementally. A successful ordinary run does not establish that every operation is safe.

**Checked code** selects functions whose safety obligations must be discharged within the supported model. Its report records entry requirements, trusted boundaries, failures, and unresolved operations. An incomplete selected check fails even when warnings are disabled.

Read [safety guarantees](/reference/guarantees/) before relying on a checked result.

## Where to go next

1. [Install from source](/getting-started/installation/) on macOS or Linux.
2. [Run your first check](/getting-started/first-check/) and fix a use-after-free.
3. [Connect an existing build](/guides/build-integration/) or [select checked functions](/guides/select-functions/).

## Project status

WeaveC is early software. Source releases are available; portable prebuilt binaries and package-manager distribution are future work. APIs and serialized analysis formats can change between minor versions. The [roadmap](/project/roadmap/) describes implementation progress and remaining limitations.
