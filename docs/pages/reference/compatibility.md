---
title: Compatibility
description: Supported toolchains, source portability, what runtime checks, the runtime's allocator and zero-initialisation change, external code, object records, and distribution boundaries.
---

## C and Clang

WeaveC uses Clang for C parsing, semantic analysis, target layout, optimization and code generation. Pass the language standard and target flags used by your project. Clang accepting a construct does not mean WeaveC can prove every safety obligation for it; what it cannot prove or check is guarded at run time or listed as unresolved, and the ledger gives the reason either way. C++ and Objective-C sources pass through to Clang unchanged.

Use [checker coverage](/reference/checker-coverage/) to see what the analysis understands.

## Annotations remain portable

Annotations live in `weavec.h`. Under Clang they use the `annotate` attribute; under compilers without that attribute they expand away. Check [annotation placement](/reference/annotation-placement/) for syntax and [compatibility with other annotation schemes](/reference/annotation-compatibility/) for current integration boundaries. WeaveC also reads Clang's own `counted_by`, `sized_by`, `alloc_size`, `nonnull` and ownership attributes.

`WEAVEC_ASSUME` has special evaluation behavior. Read its reference entry before placing an expression in it: the expression must be side-effect free, and `weavec-cc` may turn it into a runtime assertion.

## What changes in compiled code

With the default `-fweavec-checks=trap`, `weavec-cc` inserts plain C checks and guards before code generation. There is no change to pointer representation or the ABI, and objects link with objects from any compiler, but the link must go through `weavec-cc`: the guards call WeaveC's runtime, which every enforcing link adds (`libweavec_rt.a` and `libweavec_alloc.a`; precompiled-header and module builds also link a small library of out-of-line check helpers). `-fweavec-checks=none` produces the object Clang would produce and links nothing. `-fno-weavec-runtime` keeps the checks and drops the guards and the runtime's allocator.

### The allocator

With the runtime, the image's `malloc`, `calloc`, `realloc`, `free`, `reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized` and the platform's `memalign`, `pvalloc`, `malloc_usable_size`, `malloc_size`, `malloc_good_size` and `reallocf` are WeaveC's. What a program can observe:

- Every block is zero-filled, and its usable size is its requested size: `malloc_usable_size` and `malloc_size` return what was asked for, not a rounded-up size.
- A freed block is not reused until it leaves the quarantine (64 MiB by default; `WEAVEC_RT_QUARANTINE=<bytes>`), so a program that frees and allocates heavily can hold more memory than the same program on the system allocator.
- An invalid `free` or `realloc` (a double free, an interior, stack or global pointer) stops the program, in whichever object of the image it happens.
- On ELF targets the executable's allocator serves every shared library in the process, the C library included. On Darwin one runtime serves the process (the copy the dynamic loader finds first; the others forward to it), and its malloc zone becomes the default zone, so the system libraries' own allocations (`strdup`, `getline`, `asprintf`) come from it too. Blocks the system allocated before that are handed back to the zone that owns them.
- Speed, fragmentation and address layout are those of WeaveC's allocator, not the system's. Programs that depend on the system allocator's identity (a `malloc_zone_t` of their own, allocator introspection, `mallopt`) see WeaveC's allocator.

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its own allocator, and its heap is untracked. A program's own definition of one of the other functions over `malloc` replaces WeaveC's and keeps the tracked heap.

The runtime is not used with a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), with `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or on targets other than 64-bit Darwin and Linux. `weavec-cc` then builds as with `-fno-weavec-runtime` and prints a note at the link.

### Run-time cost

The default mode is slower than a plain Clang build: 1.66 times the user CPU time on the cJSON benchmark, 1.85 on zlib's `minigzip` and 5.94 on the Lua benchmark. With `-fno-weavec-runtime` the same three are at 1.14, 1.00 and 1.11. See [safety guarantees](/reference/guarantees/#what-it-costs) for what each choice enforces.

### Zero-initialisation

In the checking modes, locals and the standard allocation calls are zero-initialised. This changes behavior only for programs that read indeterminate values, which C leaves undefined. Comparing a function pointer with `malloc` sees WeaveC's zero-initialising wrapper and is false; a unit that defines its own `malloc`, `calloc`, `realloc` or `free` is not rewritten. The wrapper asks `realloc` for one byte where the program asks for none, so `realloc(p, 0)` behaves the same on every C library: it returns a one-byte block (moving `p` into it), or null with `p` still allocated, and never frees `p` and returns null. The analysis relies on this; with `-fno-weavec-zero-init` it assumes the C library may free `p` and return null for a zero size (glibc does). `-fno-weavec-zero-init` turns zero-initialisation off.

### Code that can trap

A correct program that relies on undefined behavior that happens to work can trap: reading one element past an array, reading a word at a time past the end of a string's allocation, using the slack after a heap block's requested size. Keep such code in a narrow `WEAVEC_UNSAFE` region. The [guarantees reference](/reference/guarantees/#false-traps) lists what a guard does and does not compare against.

## LLVM versions and platforms

The repository supports LLVM/Clang development installations from version 20, with version 23 recommended. The release CI exercises Linux and macOS. The runtime supports 64-bit Darwin and Linux targets. A C++20 compiler is required to build WeaveC itself.

Current releases are source archives. Portable binary packages and package-manager distribution are future work. A built compiler records paths to its LLVM installation, which must remain available at runtime.

## External code and object files

The library table (`lib/Core/LibrarySpec.txt`) describes the C library, POSIX and platform functions WeaveC models: allocation, release, string, stream and runtime operations. Each row states what every supported C library (Darwin's and glibc) accepts, not the strictest reading of a standard: where one of them documents accepting a null pointer (`gettimeofday(NULL, &tz)`, `setgroups(0, NULL)`), the row allows it. Other functions declared in platform headers are assumed to borrow their arguments and are trusted. Functions WeaveC cannot see at all are treated conservatively as possibly freeing or keeping their pointer arguments, and are listed in the ledger.

Keep `.o.weavec` records beside the corresponding objects and rebuild after source, header or command changes. Link inputs without a valid record, including static archives and shared libraries, are named in one `unanalyzed-input` warning, and calls into them are trusted.

## Version changes

WeaveC is in early 0.x development. Flags, diagnostics, the ledger schema and the object record format can change between minor versions; a record from an incompatible version is treated as missing. The ledger schema is at version 2, changed by [RFC 0032](/rfcs/0032-runtime-enforcement/), and the unit record at format 31, changed by [RFC 0033](/rfcs/0033-drop-in-by-default/): rebuild objects compiled by an earlier version. Check the [release notes](/project/releases/) before upgrading and rebuild affected objects.
