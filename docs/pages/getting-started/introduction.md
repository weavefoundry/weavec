---
title: Meet WeaveC
description: What WeaveC does, where it fits in a C toolchain, and how its compiler checks every memory access at run time and never hands back freed memory a pointer still reaches.
---

WeaveC makes existing C code memory-safe at run time. It is `weavec-cc`, a drop-in C compiler built on Clang and LLVM, and a small runtime that every program it links carries. It compiles C as Clang does and _guards_ every memory access: before a load, a store or a library call touches memory, a few inline instructions check that the bytes it touches belong to a live object. The first one that does not stops the program with a report that names the access.

```sh
make CC=weavec-cc
```

There is nothing to annotate and no separate tool to run. Existing code builds unchanged, objects from other compilers link in, and pointers keep their representation.

## No reuse while a pointer remains

A use after free is caught however much the program allocated after the `free`. A released heap block is reused only after a _sweep_ finds no pointer to it in the program's stacks, registers, writable data and live heap; a use after free or a double free through any pointer the program holds therefore traps. Sweeps are paced so that they take a small share of the run time.

A fortified library call (`__strcpy_chk(d, s, 8)`, which Clang emits for a `strcpy` into an 8-byte field when the C library fortifies it at the call site, as Darwin's does) never writes more than that size: an overflow from the field into the next one traps as `intra-object-overflow`. glibc's fortified calls state the rest of the object instead, so there the check stops a write past the object.

## What the compiler guards

The runtime replaces the program's allocator and keeps one _shadow_ byte for every 16 bytes of memory, saying which of them belong to a live object. Heap objects, the locals a guard can reach and the file's globals are separated by _redzones_, bytes no object owns, and locals are zero-initialised. A guard then catches:

- an access past either end of a heap block, a local or a global into its redzone;
- a read or write of a freed heap block, and a double or invalid `free`;
- a use of a local after its scope has ended;
- an index outside an array's declared bound, including one inside a struct;
- a null dereference, and a library call (`memcpy`, `strcpy`, `printf("%s")`) that would read or write outside its arguments' objects.

Some things are deliberately out of reach: an index that jumps over a redzone into another live object, a use of a local after its function returned, a pointer walked by arithmetic from one field of a struct into the next, a pointer the program hides from the sweep (stored as a transformed integer, for example), memory the runtime does not track (another allocator, `mmap`), and code that WeaveC did not compile. The [safety guarantees](/reference/guarantees/) page states exactly what holds and under which assumptions. The [enforcement ledger](/reference/cli/#ledger-and-summary-line) lists every access and whether it kept its guard.

## Built to drop in

`weavec-cc` compiles as AddressSanitizer would for the source: the code sees `__SANITIZE_ADDRESS__` and `__has_feature(address_sanitizer)`, and the runtime implements ASan's poisoning interface. Code written to be ASan-clean, such as an allocator that poisons its free space or a scan that reads past an object only when it is not built for ASan, is WeaveC-clean, and a pool that poisons its free space for ASan is checked by WeaveC too.

Code that reads past an object on purpose and is not written for ASan can be marked with `WEAVEC_UNSAFE` from `weavec.h`, which leaves a function or a block unguarded. It is the only annotation, and it expands to nothing on other compilers. See [unsafe regions](/guides/unsafe/).

## Where to go next

1. [Install from source](/getting-started/installation/) on macOS or Linux.
2. [Run your first check](/getting-started/first-check/): build small buggy programs and read their reports.
3. [Connect an existing build](/guides/build-integration/) and [adopt WeaveC incrementally](/guides/adoption/).

## Project status

WeaveC is early software. Source releases are available; portable prebuilt binaries and package-manager distribution are future work. Flags, report formats and the ledger can change between minor versions. The current design is [RFC 0036, _Deterministic heap reuse_](/rfcs/0036-deterministic-heap-reuse/), which builds on the guards of [RFC 0035, _Guard by default_](/rfcs/0035-guard-by-default/). The [roadmap](/project/roadmap/) tracks progress.
