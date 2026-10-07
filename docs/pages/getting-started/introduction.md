---
title: Meet WeaveC
description: What WeaveC does, where it fits in a C toolchain, how its compiler guards every memory access at run time, and how its ownership analysis reports bugs before the program runs.
---

WeaveC makes existing C code memory-safe at run time, and tells you about its memory bugs before it runs. It is built on Clang and LLVM and has two parts:

- **`weavec-cc`, a drop-in C compiler.** It compiles C as Clang does and _guards_ every memory access: before a load, a store or a library call touches memory, a few inline instructions ask a small runtime linked into the program whether those bytes are addressable. The first access that leaves its object, reads freed memory or uses a local after its scope ends stops the program with a report that names the access. A guard is removed only where a simple local rule proves it redundant.
- **An ownership and borrowing analysis.** It follows how pointers are allocated, shared, moved and released, infers what each function does to its arguments, and reports use after free, double free, dangling pointers and leaks with the source locations that explain them. It is advisory: it never changes the compiled code.

## Two ways to use it

| Tool        | Use it when                                                                  | Example                        |
| ----------- | ---------------------------------------------------------------------------- | ------------------------------ |
| `weavec-cc` | You want binaries that stop at the first bad memory access.                  | `make CC=weavec-cc`            |
| `weavec`    | You want the analysis's diagnostics without building, for example in review. | `weavec example.c -- -std=c17` |

The analysis tool accepts a compilation database, so it uses the includes, defines and language flags of your build. `weavec-cc -fweavec-diagnose` runs the same analysis during a build and prints its findings as warnings.

## What the compiler guards

The runtime replaces the program's allocator and keeps one _shadow_ byte for every 16 bytes of memory, saying which of them belong to a live object. `weavec-cc` gives the stack objects a guard can reach and the file's globals _redzones_, bytes no object owns, and zero-initialises locals. A guard then catches:

- an access past either end of a heap block, or past a local or a global into its redzone;
- a read or write of a freed heap block while it waits in the runtime's quarantine, and a double or invalid `free`;
- a use of a local after its scope has ended;
- an index outside an array's declared bound, including one inside a struct;
- a null dereference, and a library call (`memcpy`, `strcpy`, `printf("%s")`) that would read or write outside its arguments' objects.

It does not catch everything: an access that jumps over a redzone into another live object passes, a freed block is recognised only until its storage is reused, and code that WeaveC did not compile is not guarded. The [safety guarantees](/reference/guarantees/) page states exactly what holds and under which assumptions. The [enforcement ledger](/reference/cli/#ledger-and-summary-line) lists every access and whether it kept its guard.

## Start with inference

The analysis needs no annotations to start. A helper that calls `free` consumes its argument; a constructor describes the memory it returns; callers are checked against those facts, across source files with `weavec --whole-program`. Where inference needs help, typically at public interfaces, annotations from `weavec.h` state the contract: ownership (`WEAVEC_OWNED`, `WEAVEC_BORROWED`), nullability and extents such as `WEAVEC_COUNTED_BY(n)`. They expand to nothing under other compilers. One annotation changes the compiled code: `WEAVEC_UNSAFE` leaves a function or block unguarded, for code that reads past an object on purpose.

## Where to go next

1. [Install from source](/getting-started/installation/) on macOS or Linux.
2. [Run your first check](/getting-started/first-check/): fix a use-after-free, then watch an index check and a guard stop a program.
3. [Connect an existing build](/guides/build-integration/) and [adopt WeaveC incrementally](/guides/adoption/).

## Project status

WeaveC is early software. Source releases are available; portable prebuilt binaries and package-manager distribution are future work. Flags, diagnostics, report formats and the ledger can change between minor versions. The current design is [RFC 0035, _Guard by default_](/rfcs/0035-guard-by-default/); [RFC 0036](/project/roadmap/) will make the inferred ownership contracts checked at function entry, so that they remove guards. The [roadmap](/project/roadmap/) tracks progress.
