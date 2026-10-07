---
title: Analyze the whole program
description: Report bugs that span C translation units with weavec --whole-program, what it checks across files, and how the analysis treats code it cannot see.
---

A function's implementation may live in another source file. Whole-program analysis lets a caller use the effects inferred from that definition, so a use after free whose `free` is in another file is reported before the program runs. Like all of WeaveC's analysis it is advisory: a guarded build stops such a bug at run time whether or not the analysis saw it.

## Analyze source files together

```sh
weavec --whole-program main.c buffer.c -- -std=c17 -Iinclude
```

For files with different compiler flags, use their compilation database:

```sh
weavec --whole-program -p build
```

Each translation unit exports what its functions do to their arguments (their _summaries_). WeaveC combines the available definitions and revisits callers as their callees' effects become known, so a wrapper that releases a resource invalidates aliases in a different file. Across the files it also:

- solves which functions each function pointer can hold across the program;
- checks every declaration against its definition in another file: a declaration that says `WEAVEC_BORROWED` for a parameter the definition frees is an `annotation-mismatch` error.

Everything stays in memory; nothing is written next to the objects. The tool prints a summary line per file and one for the program. A group of files that call each other in a cycle and whose summaries do not settle keeps its widened summaries, with a note:

```text
weavec: note: the whole-program analysis of 'a.c', 'b.c' did not converge (its widened summaries are used)
```

`weavec-cc -fweavec-diagnose` analyses each file on its own as it compiles it; it does not analyse the program when it links. Run `weavec --whole-program` in CI to report the bugs that span files.

## Understand unknown code

A call to a function WeaveC cannot see (no definition in the program, no library-table entry, no ownership annotation, and not declared in a C library, POSIX or platform header) is an _unknown callee_. The analysis assumes it may release, keep or replace every pointer argument that has no ownership annotation. Later uses of those pointers are not reported, and they are not proven. Annotating the callee's declaration (`WEAVEC_BORROWED` on a parameter it neither keeps nor frees) tells the analysis what it does. In a per-file run, calls into other files are unknown; `weavec --whole-program` replaces them with the definitions' summaries.

Functions declared in platform headers that the library table does not describe are assumed to borrow their arguments; the analysis trusts them (`system-api`). Calls through a function pointer use the targets the program stores into it; a pointer with no known target is treated like an unknown callee.

None of this affects the compiled program: every access is guarded whatever the analysis knows. The design is specified by [RFC 0005](/rfcs/0005-whole-program-analysis/), [RFC 0031](/rfcs/0031-object-engine/) and [RFC 0035](/rfcs/0035-guard-by-default/) §8.
