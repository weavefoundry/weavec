---
title: Compatibility
description: Supported toolchains and platforms, what weavec-cc changes in compiled code (the allocator, redzones, zero-initialised locals, the shadow reservation, traps on deliberate over-reads), sanitizers, external code, and version changes.
---

## C and Clang

WeaveC uses Clang for C parsing, semantic analysis, target layout, optimisation and code generation. Pass the language standard and target flags used by your project. `weavec-cc` guards the C units it compiles; C++, Objective-C, OpenCL and CUDA sources are compiled as Clang compiles them, without guards, and link with the guarded C code as usual.

The advisory analysis (`weavec`, `weavec-cc -fweavec-diagnose`) reads C. Clang accepting a construct does not mean the analysis can prove anything about it; what it cannot decide counts as not proven and never changes the generated code. Use [checker coverage](/reference/checker-coverage/) to see what the analysis understands.

## Annotations remain portable

Annotations live in `weavec.h`. Under Clang they use the `annotate` attribute; under compilers without that attribute they expand away. Check [annotation placement](/reference/annotation-placement/) for syntax and [compatibility with other annotation schemes](/reference/annotation-compatibility/) for current integration boundaries. The analysis also reads Clang's own `counted_by`, `sized_by`, `alloc_size`, `nonnull` and ownership attributes.

`WEAVEC_UNSAFE` is the only annotation that changes the generated code: the accesses in its function or block are not guarded and its array indexes are not checked. The others are read by the advisory analysis only. `WEAVEC_ASSUME(expr)` is a statement to the analysis; it is not checked at run time. Under WeaveC its expression is evaluated and discarded, and under other compilers it is not evaluated at all, so it must be free of side effects.

## What changes in compiled code

With the default `-fweavec-checks=trap`, `weavec-cc` compiles each C unit as Clang does and adds a guard before every memory access that a simple local rule cannot prove safe, in LLVM IR, against the runtime's shadow memory. There is no change to pointer representation or the ABI, and objects link with objects from any compiler, but the link must go through `weavec-cc`: the guards call WeaveC's runtime, which every enforcing link adds (`libweavec_rt.a` and `libweavec_alloc.a`). `-fweavec-checks=none` produces the object Clang would produce and links nothing; `-fno-weavec` is plain Clang.

A guarded compile also turns on some of Clang's own options: `-fsanitize=array-bounds` for typed array indexes, reported through WeaveC's runtime (unless the command line already asks for a sanitizer); source locations for the reports, without emitting debug information when there is no `-g`; lifetime markers at every optimisation level, so that a use after scope is caught at `-O0` too; and, on AArch64, no conditional compares in the backend. Object code is larger and compiles take longer than with Clang alone.

### The allocator

With the runtime, the image's `malloc`, `calloc`, `realloc`, `free`, `reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized` and the platform's `memalign`, `pvalloc`, `malloc_usable_size`, `malloc_size`, `malloc_good_size` and `reallocf` are WeaveC's. What a program can observe:

- Every block is zero-filled, and its usable size is its requested size: `malloc_usable_size` and `malloc_size` return what was asked for, and the bytes after it are not addressable. `malloc(0)` and `realloc(p, 0)` return a block of zero bytes; `realloc(p, 0)` does not free `p` and return null.
- A freed block is not reused until it leaves the quarantine (16 MiB by default; `WEAVEC_RT_QUARANTINE=<bytes>`), so a program that frees and allocates heavily can hold more memory than the same program on the system allocator.
- An invalid `free` or `realloc` (a double free, an interior, stack or global pointer) stops the program, in whichever object of the image it happens.
- On ELF targets the executable's allocator serves every shared library in the process, the C library included. On Darwin one runtime serves the process (the copy the dynamic loader finds first; the others forward to it), and its malloc zone becomes the default zone, so the system libraries' own allocations (`strdup`, `getline`, `asprintf`) come from it too. Blocks the system allocated before that are handed back to the zone that owns them.
- Speed, fragmentation and address layout are those of WeaveC's allocator, not the system's. Programs that depend on the system allocator's identity (a `malloc_zone_t` of their own, allocator introspection, `mallopt`) see WeaveC's allocator.

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its own allocator, and the link says so with a note: its heap is untracked, guards pass on it and releases are not validated. A program's own definition of one of the other functions over `malloc` replaces WeaveC's and keeps the tracked heap.

### Frames and globals

Each local that a guard can reach, `alloca` blocks and variable-length arrays included, is followed by a redzone of at least 32 bytes (more for large objects) and aligned to 32 bytes, so stack frames are larger: deeply recursive code may need a larger stack. Locals proven to need no tracking keep Clang's layout.

Each global or static variable a unit defines is followed by a redzone and aligned to 32 bytes, so the data sections grow and their layout changes. `sizeof` and the variable's own bytes are unchanged, but code that relies on two globals being adjacent (walking from one into the next) traps. Thread-local variables, variables in a named section, string literals, common symbols and a few other kinds are left as they are, so linker sets placed in a named section keep working.

### Zero-initialisation

In the enforcing modes, locals of up to 4 KiB are zero-initialised (`-ftrivial-auto-var-init=zero` with `-ftrivial-auto-var-init-max-size=4096`, unless the command line sets its own limit), and the runtime's heap is zero-filled. This changes behaviour only for programs that read indeterminate values, which C leaves undefined. `-fno-weavec-zero-init` turns zero-initialisation of locals off.

### The shadow and the arena

At start-up the runtime reserves address space, without committing memory: the shadow (one byte per 16 bytes of the window it covers: the whole user address space on Linux, 2^42 bytes on Darwin) and the arena's size-class regions (2 GiB each). The reservation takes about a millisecond. Tools that report virtual size show it, and a limit on virtual memory (`ulimit -v`) smaller than the reservation makes it fail: the program then prints `weavec: warning: the shadow memory could not be reserved; memory accesses are not checked` and runs with every guard passing. The shadow is left out of core dumps.

### Code that can trap

A program that reads past an object on purpose traps, as it does under AddressSanitizer: a word-at-a-time string scan, a hash that reads whole words of a key, a conservative collector's stack scan, or code that uses the slack after a heap block's requested size. So does an index past an inner bound of a multidimensional array (`int m[4][4]; m[0][k]` with `k ≥ 4`). Keep such code in a narrow `WEAVEC_UNSAFE` region; a function already marked `no_sanitize("address")` (or `no_sanitize_address`, `disable_sanitizer_instrumentation`) is left unguarded without a change. The [guarantees reference](/reference/guarantees/#false-traps) lists what is accepted.

### Run-time cost

A guarded program runs slower than a plain Clang build and uses more memory. [RFC 0035's acceptance gates](/rfcs/0035-guard-by-default/#acceptance-gates) set the targets: each workload at most twice the reference compiler's run time, or AddressSanitizer's ratio where that is larger, 1.8 times on average, and a build within 1.5 times the reference compiler's CPU time. These are targets; the RFC records the measurements when its gates are run. See [troubleshooting](/reference/troubleshooting/#the-program-is-slower-or-uses-more-memory) for what to do when a program is too slow.

### Sanitizers

The runtime is not used with a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), with `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or on targets other than 64-bit Darwin and Linux. `weavec-cc` then compiles every unit without guards, as with `-fweavec-checks=none`, and the link prints `weavec-cc: note: building without the WeaveC runtime (<reason>): memory accesses are not guarded`. Other sanitizers, such as `-fsanitize=undefined`, work alongside the guards; a program that links UBSan's runtime reports array-bounds failures through UBSan's handlers rather than WeaveC's.

## LLVM versions and platforms

The repository supports LLVM/Clang development installations from version 20, with version 23 recommended. The release CI exercises Linux and macOS. A C++20 compiler is required to build WeaveC itself.

The runtime supports 64-bit Darwin and Linux targets. Its archives are built for the host, so `weavec-cc` adds them only to a link for the host's architecture and operating system; to cross-compile with guards, link a runtime built for the target yourself, or build with `-fweavec-checks=none`.

Current releases are source archives. Portable binary packages and package-manager distribution are future work. A built compiler records paths to its LLVM installation, which must remain available at run time.

## External code and object files

Only code that `weavec-cc` compiles is guarded. Code built by another compiler, precompiled static and shared libraries, and assembly make their accesses unchecked; their stack frames and globals are untracked, and an access into them from guarded code passes. Their heap allocations still come from WeaveC's allocator when the program links the runtime, so a guarded access to a block they allocated is checked against its size and a release of it is validated. Link inputs need no WeaveC metadata: objects from any compiler link as they are.

The library table (`lib/Core/LibrarySpec.txt`) describes the C library, POSIX and platform functions WeaveC models: allocation, release, string, stream and runtime operations. A call to one of them has its memory arguments guarded over the bytes the row says it accesses, or is replaced by a checked wrapper in the runtime. Each row states what every supported C library (Darwin's and glibc) accepts, not the strictest reading of a standard: where one of them documents accepting a null pointer (`gettimeofday(NULL, &tz)`, `setgroups(0, NULL)`), the row allows it. A call to a function outside the table is not checked at the call; its own accesses are guarded if WeaveC built it. Wide-character string arguments are not scanned.

The advisory analysis sees only the source it is given. Functions it cannot see are treated conservatively as possibly freeing or keeping their pointer arguments; analyse the program's files together with `weavec --whole-program` to see across them.

## Version changes

WeaveC is in early 0.x development. Flags, diagnostics, run-time reports and the ledger schema can change between minor versions. [RFC 0035](/rfcs/0035-guard-by-default/) replaced the earlier enforcement: the enforcement ledger is now `weavec-ledger` version 3, `weavec.h` is at version 0.10, objects carry no WeaveC records, and objects compiled by an earlier version call runtime entry points this version no longer has, so rebuild them. Its removed options are errors rather than aliases; see [removed options](/reference/cli/#removed-options). Check the [release notes](/project/releases/) before upgrading.
