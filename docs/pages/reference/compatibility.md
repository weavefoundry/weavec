---
title: Compatibility
description: Supported toolchains and platforms, what weavec-cc changes in compiled code (the allocator and its sweep, frames and redzones, fortification, AddressSanitizer's macros, zero-initialised locals, the shadow reservation, traps on deliberate over-reads), sanitizers, external code, and version changes.
---

## C and Clang

WeaveC uses Clang for C parsing, semantic analysis, target layout, optimisation and code generation. Pass the language standard and target flags used by your project. `weavec-cc` guards the C units it compiles; C++, Objective-C, OpenCL and CUDA sources are compiled as Clang compiles them, without guards, and link with the guarded C code as usual.

## weavec.h

`weavec.h` (version 1.0, `WEAVEC_H_VERSION_MAJOR` and `WEAVEC_H_VERSION_MINOR`) provides one macro, `WEAVEC_UNSAFE`, which marks a function or a block whose accesses are not guarded and whose array indexes are not checked. Under Clang it uses the `annotate` attribute; under compilers without that attribute it expands to nothing, so the code stays buildable with any C toolchain. `weavec-cc` puts the header's directory on the system include path; a project that also builds with other compilers can copy the header into its tree. A function marked `no_sanitize("address")` is an unsafe region too, without the header.

## What changes in compiled code

With the default `-fweavec-checks=trap`, `weavec-cc` compiles each C unit as Clang does and adds a guard before every memory access that a simple local rule cannot prove safe, in LLVM IR, against the runtime's shadow memory. There is no change to pointer representation or the ABI, and objects link with objects from any compiler, but the link must go through `weavec-cc`: the guards call WeaveC's runtime, which every enforcing link adds (`libweavec_rt.a` and `libweavec_alloc.a`). `-fweavec-checks=none` produces the object Clang would produce and links nothing; `-fno-weavec` is plain Clang.

A guarded compile also turns on some of Clang's own options: `-fsanitize=array-bounds` for typed array indexes, reported through WeaveC's runtime (unless the command line already asks for a sanitizer); source locations for the reports, without emitting debug information when there is no `-g`; lifetime markers at every optimisation level, so that a use after scope is caught at `-O0` too; and, on AArch64, no conditional compares in the backend. Object code is larger and compiles take longer than with Clang alone.

### Preprocessor macros and fortification

In the enforcing modes a C unit sees what it would see under AddressSanitizer: `__SANITIZE_ADDRESS__` is defined and `__has_feature(address_sanitizer)` is true, so a project's ASan-specific code (poisoning in a pool allocator, a scan kept from ASan) is compiled in, and the runtime implements ASan's poisoning interface for it. `__WEAVEC__` is defined whenever WeaveC is on. `-fno-weavec-asan-compat` leaves the ASan macros undefined. See [AddressSanitizer compatibility](/reference/cli/#addresssanitizer-compatibility).

On glibc targets an optimising compile whose command line does not set `_FORTIFY_SOURCE` gets `-D_FORTIFY_SOURCE=3`, so string and `printf`-family calls carry the size of the object they write, which the guards check (glibc's inline wrappers state the rest of the object even for a field, so a write into the next field is not stopped there). Darwin's SDK fortifies optimising compiles at level 2, and keeps doing so under WeaveC although Clang turns it off under ASan.

### The allocator

With the runtime, the image's `malloc`, `calloc`, `realloc`, `free`, `reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized` and the platform's `memalign`, `pvalloc`, `malloc_usable_size`, `malloc_size`, `malloc_good_size` and `reallocf` are WeaveC's. What a program can observe:

- Every block is zero-filled, and its usable size is its requested size: `malloc_usable_size` and `malloc_size` return what was asked for, and the bytes after it are not addressable. `malloc(0)` and `realloc(p, 0)` return a block of zero bytes; `realloc(p, 0)` does not free `p` and return null.
- A freed block is not reused until a _sweep_ finds no pointer to it in the program's stacks, registers, writable data and live heap. A sweep is due when the bytes released since the last one exceed 16 MiB (`WEAVEC_RT_QUARANTINE=<bytes>`) and a quarter of the live heap, and runs once the last sweep ended at least eight of its durations ago (at once past twice that budget and half the live heap). It stops every thread of the process for a time that grows with the live heap; on Linux it does so with a signal, `SIGPWR` by default (`WEAVEC_RT_SWEEP_SIGNAL=<n>` names another). A program that frees and allocates heavily can hold more memory than on the system allocator, and a block that a stale pointer still points to stays allocated as long as the pointer does.
- An invalid `free` or `realloc` (a double free, an interior, stack or global pointer) stops the program, in whichever object of the image it happens.
- On ELF targets the executable's allocator serves every shared library in the process, the C library included. On Darwin one runtime serves the process (the copy the dynamic loader finds first; the others forward to it), and its malloc zone becomes the default zone, so the system libraries' own allocations (`strdup`, `getline`, `asprintf`) come from it too. Blocks the system allocated before that are handed back to the zone that owns them.
- Speed, fragmentation and address layout are those of WeaveC's allocator, not the system's. Programs that depend on the system allocator's identity (a `malloc_zone_t` of their own, allocator introspection, `mallopt`) see WeaveC's allocator.

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its own allocator, and the link says so with a note: its heap is untracked, guards pass on it and releases are not validated. A program's own definition of one of the other functions over `malloc` replaces WeaveC's and keeps the tracked heap.

### Frames and globals

Each local that a guard can reach or whose address escapes is laid out in one frame with redzones between the locals, and `alloca` blocks and variable-length arrays get a redzone after them; in a frame larger than 512 bytes, locals whose lifetimes never overlap (the arms of a `switch`) share space, as in Clang's own frames, so the frame grows by the redzones rather than by the locals. Deep recursion through such locals reaches a lower depth than with Clang, as it does under AddressSanitizer. Before a call that does not return (`longjmp`, `exit`'s kin aside), the stack's shadow below the caller is cleared, so `setjmp` and `longjmp` work as usual.

Each global or static variable a unit defines is followed by a redzone and aligned to 32 bytes, so the data sections grow and their layout changes. `sizeof` and the variable's own bytes are unchanged, but code that relies on two globals being adjacent (walking from one into the next) traps. Thread-local variables, variables in a named section, string literals, common symbols and a few other kinds are left as they are, so linker sets placed in a named section keep working. The globals of shared libraries and bundles built by `weavec-cc` are protected as the executable's are.

### Zero-initialisation

In the enforcing modes, locals of up to 4 KiB are zero-initialised (`-ftrivial-auto-var-init=zero` with `-ftrivial-auto-var-init-max-size=4096`, unless the command line sets its own limit), and the runtime's heap is zero-filled. This changes behaviour only for programs that read indeterminate values, which C leaves undefined. `-fno-weavec-zero-init` turns zero-initialisation of locals off.

### The shadow and the arena

At start-up the runtime reserves address space, without committing memory: the shadow (one byte per 16 bytes of the window it covers: the whole user address space on Linux, 2^42 bytes on Darwin) and the arena's size-class regions (2 GiB each). The reservation takes about a millisecond. Tools that report virtual size show it, and a limit on virtual memory (`ulimit -v`) smaller than the reservation makes it fail: the program then prints `weavec: warning: the shadow memory could not be reserved; memory accesses are not checked` and runs with every guard passing. The shadow is left out of core dumps.

### Code that can trap

A program that reads past an object on purpose traps, as it does under AddressSanitizer: a word-at-a-time string scan, a hash that reads whole words of a key, or code that uses the slack after a heap block's requested size. So does an index past an inner bound of a multidimensional array (`int m[4][4]; m[0][k]` with `k ≥ 4`). Code that keeps such reads from ASan behind `__SANITIZE_ADDRESS__` or `__has_feature(address_sanitizer)`, or in a function marked `no_sanitize("address")` (or `no_sanitize_address`, `disable_sanitizer_instrumentation`), needs no change; other code needs a narrow `WEAVEC_UNSAFE` region. The [guarantees reference](/reference/guarantees/#false-traps) lists what is accepted.

### Run-time cost

A guarded program runs slower than a plain Clang build and uses more memory. [RFC 0036's acceptance gates](/rfcs/0036-deterministic-heap-reuse/#acceptance-gates) set the targets against the previous release (v0.16.0): the corpus benchmarks' geometric mean at most 1.1 times its run time and no benchmark above 1.25 times; peak memory at most 1.5 times its, or 64 MiB more; the longest sweep pause at most 150 ms with 256 MiB of live heap; and a build at most 1.1 times as long as its. The RFC's measured gates record the results. See [troubleshooting](/reference/troubleshooting/#the-program-is-slower-or-uses-more-memory) for what to do when a program is too slow or holds too much memory.

### Sanitizers

The runtime is not used with a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), with `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or on targets other than 64-bit Darwin and Linux. `weavec-cc` then compiles every unit without guards, as with `-fweavec-checks=none`, and the link prints `weavec-cc: note: building without the WeaveC runtime (<reason>): memory accesses are not guarded`. Other sanitizers, such as `-fsanitize=undefined`, work alongside the guards; a program that links UBSan's runtime reports array-bounds failures through UBSan's handlers rather than WeaveC's. LeakSanitizer's interface is accepted and does nothing: WeaveC does not check for leaks.

## LLVM versions and platforms

The repository supports LLVM/Clang development installations from version 20, with version 23 recommended. The release CI exercises Linux and macOS. A C++20 compiler is required to build WeaveC itself.

The runtime supports 64-bit Darwin and Linux targets. Its archives are built for the host, so `weavec-cc` adds them only to a link for the host's architecture and operating system; to cross-compile with guards, link a runtime built for the target yourself, or build with `-fweavec-checks=none`.

Current releases are source archives. Portable binary packages and package-manager distribution are future work. A built compiler records paths to its LLVM installation, which must remain available at run time.

## External code and object files

Only code that `weavec-cc` compiles is guarded. Code built by another compiler, precompiled static and shared libraries, and assembly make their accesses unchecked; their stack frames and globals are untracked, and an access into them from guarded code passes. Their heap allocations still come from WeaveC's allocator when the program links the runtime, so a guarded access to a block they allocated is checked, a release of it is validated, and the sweep scans their memory for pointers like the rest of the program's. Link inputs need no WeaveC metadata: objects from any compiler link as they are.

The library table (`lib/Core/LibrarySpec.txt`) describes the C library, POSIX and platform functions WeaveC models: allocation, release, string, stream and runtime operations. A call to one of them has its memory arguments guarded over the bytes the row says it accesses, or is replaced by a checked wrapper in the runtime. Each row states what every supported C library (Darwin's and glibc) accepts, not the strictest reading of a standard: where one of them documents accepting a null pointer (`gettimeofday(NULL, &tz)`, `setgroups(0, NULL)`), the row allows it. A call to a function outside the table is not checked at the call; its own accesses are guarded if WeaveC built it. Wide-character string arguments are not scanned.

## Version changes

WeaveC is in early 0.x development. Flags, run-time reports and the ledger schema can change between minor versions. [RFC 0036](/rfcs/0036-deterministic-heap-reuse/) changed the model of [RFC 0035](/rfcs/0035-guard-by-default/): released memory is reused only after a sweep, fortified calls are checked against their object size, code is compiled as AddressSanitizer would compile it, and locals whose lifetimes never overlap share frame space. With it:

- The advisory ownership and lifetime analysis is gone: the `weavec` tool, `weavec-cc -fweavec-diagnose`, whole-program analysis, every diagnostic and its identifier. WeaveC reports nothing before the program runs; its guarantees are those of the guarded program.
- `weavec.h` is version 1.0 and provides only `WEAVEC_UNSAFE`. A program that uses one of the other annotation macros fails to compile, with `weavec-cc` and other compilers alike, until it stops using it.
- The runtime's entry points and the table its copies share on Darwin changed, so objects compiled by an earlier `weavec-cc` must be rebuilt: they call entry points this runtime no longer has, and on Darwin a shared image built by an earlier version keeps a runtime, heap and shadow of its own.
- The report kind `buffer-overflow` is no longer reported (a partial granule the program poisoned is `use-after-poison`); `intra-object-overflow` and `use-after-poison` are new.

The removed `-fweavec-*` options are errors rather than aliases; see [removed options](/reference/cli/#removed-options). Check the [release notes](/project/releases/) before upgrading.
