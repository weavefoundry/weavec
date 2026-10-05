---
title: Analyze the whole program
description: Check calls across C translation units with weavec --whole-program or weavec-cc -fweavec-link=analyze, what a default link checks, and how unknown code is treated.
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

Each compile checks its own file, inserts its runtime checks and guards and writes a record next to the object (`buffer.o.weavec`). The link step reads the records; by default (`-fweavec-link=records`) it parses and analyses nothing again, and:

- solves which functions each function pointer can hold across the program;
- checks every declaration against its definition in another unit: a declaration that says `WEAVEC_BORROWED` for a parameter the definition frees is an `annotation-mismatch` error;
- checks the requirements an exported function relies on at the calls other units make;
- adds the runtime's archives to the link, unless the build is without the runtime or a link input defines the allocator itself (see [build integration](/guides/build-integration/#what-changes-in-the-compiled-program));
- under `-fweavec-ledger`, composes the program ledger from the rows the records carry, and prints the program's summary line.

A default link does not report a bug that needs two files, such as a use after free whose `free` is in another unit: the use's guard traps when it happens. To report such bugs at build time, link with `-fweavec-link=analyze`, which also re-runs the analysis of the units with every unit's summaries in view, or run `weavec --whole-program` in CI. `analyze` runs within a wall-clock budget, `-fweavec-link-budget=<seconds>` (default 120; `0` for none). A unit it does not finish, a unit it cannot run again and a group of units that does not converge keep their compile-time results, with a note:

```text
weavec-cc: note: the whole-program analysis of 'main.c' stopped at its budget; their compile-time results stand
```

Only a definite violation it finds fails the link, and `-Wno-error=weavec-<id>` lowers it like any other. `-fweavec-link=none` skips the step and links as Clang would.

Spatial and null outcomes are decided when each file is compiled, because they decide the checks and guards in its object; the link step keeps them, in either mode. Link inputs without a WeaveC record are named in one `unanalyzed-input` warning, and calls into them are trusted.

## Understand unknown code

A call to a function WeaveC cannot see (no definition in the program, no library-table entry, no ownership annotation, and not declared in a C library, POSIX or platform header) is an _unknown callee_. WeaveC assumes it may release, keep or replace every pointer argument that has no ownership annotation. Later uses of those pointers are not reported, but their ledger rows carry the reason `unknown-callee` rather than being proven (`guarded` where a guard covers the use, `unresolved` otherwise), and the call's row suggests an annotation such as `WEAVEC_BORROWED` on a parameter the callee neither keeps nor frees. In a per-file compile, calls into other files are unknown; `weavec --whole-program` and `-fweavec-link=analyze` replace them with the definitions' summaries.

Functions declared in platform headers that the library table does not describe are assumed to borrow their arguments; those rows are `trusted(system-api)`. Calls through a function pointer use the targets the program stores into it; a pointer with no known target is treated like an unknown callee, with reason `callback`.

To make unknown code an error instead of a ledger row, build with `-fweavec-require=checked`; `-fweavec-require=guarded` accepts the uses a guard covers and rejects the rest. The design and the record format are specified by [RFC 0005](/rfcs/0005-whole-program-analysis/), [RFC 0030](/rfcs/0030-prove-or-trap/), [RFC 0032](/rfcs/0032-runtime-enforcement/) and [RFC 0033](/rfcs/0033-drop-in-by-default/).
