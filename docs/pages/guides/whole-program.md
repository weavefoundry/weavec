---
title: Analyze the whole program
description: Check contracts across C translation units using source files, compilation databases, and compiler sidecars.
---

A function's implementation may live in another source file. Whole-program analysis lets the caller use the effects inferred from that definition.

## Analyze source files together

```sh
weavec --whole-program main.c buffer.c -- -std=c17 -Iinclude
```

For files with different compiler flags, use their compilation database:

```sh
weavec --whole-program -p build
```

Each translation unit exports summaries. WeaveC combines available definitions and revisits callers as relevant contracts change. A wrapper that releases a resource can therefore invalidate aliases in a different file.

## Analyze during linking

```sh
weavec-cc -c buffer.c -o buffer.o
weavec-cc -c main.c -o main.o
weavec-cc main.o buffer.o -o app
```

The link step consumes object sidecars and performs whole-program analysis before linking the executable. A cross-file error stops the link. Checking inside a single source file still happens at compile time.

## Understand unresolved boundaries

Missing definitions and unknown callback targets remain boundaries. An unrelated function with a matching type is not evidence for an unknown callback's behavior.

`--strict-externs` makes calls without a definition, annotation, or library summary raw operations. It helps make interfaces explicit, but it is not a substitute for checked obligations.

Use [checked builds](/guides/checked-builds/) when selected functions must have complete contracts. Use [analysis caching](/guides/incremental-analysis/) to reuse validated work on repeated runs.

The architecture and object metadata contract are specified by [RFC 0005](/rfcs/0005-whole-program-analysis/).
