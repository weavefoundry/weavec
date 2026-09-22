---
title: Analyze the whole program
description: Check calls across C translation units with weavec --whole-program or at link time with weavec-cc, and understand how unknown code is treated.
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

Each translation unit exports summaries. WeaveC combines the available definitions and revisits callers as their callees' effects become known. A wrapper that releases a resource can therefore invalidate aliases in a different file.

## Analyze during linking

```sh
weavec-cc -c buffer.c -o buffer.o
weavec-cc -c main.c -o main.o
weavec-cc main.o buffer.o -o app
```

Each compile checks its own file, inserts its runtime checks and writes a record next to the object (`buffer.o.weavec`). The link step then reads the records and:

- solves which functions each function pointer can hold across the program;
- checks every declaration against its definition in another unit: a declaration that says `WEAVEC_BORROWED` for a parameter the definition frees is an `annotation-mismatch` error;
- checks the requirements an exported function relies on at the calls other units make;
- re-runs the temporal analysis with every unit's summaries in view, so a use-after-free that needs two files is reported; a definite one fails the link;
- writes the program ledger under `-fweavec-ledger` and prints the program's summary line.

Spatial and null outcomes are decided when each file is compiled, because they decide the checks in its object; the link step keeps them. Link inputs without a WeaveC record are named in one `unanalyzed-input` warning, and calls into them are trusted.

## Understand unknown code

A call to a function WeaveC cannot see (no definition in the program, no library-table entry, no ownership annotation, and not declared in a C library, POSIX or platform header) is an _unknown callee_. WeaveC assumes it may release, keep or replace every pointer argument that has no ownership annotation. Later uses of those pointers are not reported, but their ledger rows are `unresolved(unknown-callee)` rather than proven, and the call's row suggests an annotation such as `WEAVEC_BORROWED` on a parameter the callee neither keeps nor frees. In a per-file compile, calls into other files are unknown until the link step replaces them with the definitions' summaries.

Functions declared in platform headers that the library table does not describe are assumed to borrow their arguments; those rows are `trusted(system-api)`. Calls through a function pointer use the targets the program stores into it; a pointer with no known target is treated like an unknown callee, with reason `callback`.

To make unknown code an error instead of a ledger row, build with `-fweavec-require=checked`. The design and the record format are specified by [RFC 0005](/rfcs/0005-whole-program-analysis/) and [RFC 0030](/rfcs/0030-prove-or-trap/).
