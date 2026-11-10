---
title: Troubleshooting
description: Resolve installation problems, read a run-time report, its stacks and objects, and find the bug behind each kind, handle code that reads past objects on purpose or was written for AddressSanitizer, understand the link notes, and deal with memory held by the quarantine, slow programs and the sweep's signal.
---

## CMake cannot find LLVM or Clang

Install the development libraries, not just a compiler executable. Set `WEAVEC_LLVM_PREFIX` to the LLVM installation prefix, or supply `CMAKE_PREFIX_PATH`, `LLVM_DIR`, and `Clang_DIR`. On macOS, use Homebrew LLVM; Xcode does not ship the required CMake packages.

Use a fresh build directory if a previous configuration selected a different LLVM installation. See the [developer toolchain guide](/contributing/development/#toolchain).

## FileCheck or lit is missing

Tests require `lit` plus LLVM's `FileCheck`, `not`, and `count` utilities. Make sure the chosen LLVM tool directory is on PATH and use a matching `lit` release. Building LLVM from source requires its utilities to be installed.

## The compiler cannot locate Clang's headers or WeaveC's files

A toolchain is one directory tree: `bin/weavec-cc`, WeaveC's files in `lib/weavec/` (`weavec.h`, the runtime's library), Clang's headers and builtins in `lib/clang/<major>/`, and on macOS the LLVM libraries `weavec-cc` loads in `lib/weavec/llvm/`. `weavec-cc` finds them all from its own executable, so move or copy the tree as a whole; `WEAVEC_RESOURCE_DIR` names another directory for WeaveC's files. A build tree has the same layout. A missing `<stddef.h>` means `lib/clang/<major>/include` is not beside `bin/`. See [installation](/getting-started/installation/).

## macOS refuses to run weavec-cc

A downloaded toolchain is not yet signed and notarized, and Gatekeeper refuses its unsigned binaries. Remove the quarantine attribute from the unpacked tree (`xattr -dr com.apple.quarantine weavec-<version>-darwin-arm64`), or install with Homebrew, which does not set it.

## The link cannot find the WeaveC runtime

```text
weavec-cc: error: cannot find the WeaveC runtime (libweavec_rt.dylib), which an enforcing link carries (-fweavec-checks=none builds without it)
```

Every link of a guarded program adds the runtime's library (`libweavec_rt.so` on Linux, `libweavec_rt.a` for `-static`), installed in `lib/weavec/` beside the `include/` directory that holds `weavec.h`. The driver looks for it in `../lib/weavec/` from its own executable, or in the directory `WEAVEC_RESOURCE_DIR` names. `-fweavec-checks=none` builds without guards and without the runtime.

## A program cannot load libweavec_rt

```text
error while loading shared libraries: libweavec_rt.so.38: cannot open shared object file: No such file or directory
dyld: Library not loaded: @rpath/libweavec_rt.38.dylib
```

A guarded program loads the runtime's library where it runs, from the rpath its link recorded: the toolchain's `lib/weavec` unless `-fweavec-rpath` named another directory. A program copied to another machine, or a toolchain moved after the link, needs it there: install `libweavec_rt` beside the program and link with `-fweavec-rpath='$ORIGIN'` (`@loader_path` on macOS), or link a static Linux program (`-static`). The name carries the runtime's ABI (38 here): a toolchain whose runtime has another ABI cannot serve the program, so rebuild it with that toolchain.

## Two WeaveC runtimes in one process

```text
weavec: error: two WeaveC runtimes in one process (<first> and <second>): build every image with one weavec-cc
```

Images built by two WeaveC versions loaded into one process, each with its own runtime: rebuild them with one `weavec-cc`.

## The program's allocator is not WeaveC's

```text
weavec: note: the program's allocator is not WeaveC's (/lib/x86_64-linux-gnu/libc.so.6 serves it: the program was not linked by weavec-cc): heap accesses are not checked; preload …/libweavec_rt.so.38 to check them
```

On Linux a guarded library was loaded by a program `weavec-cc` did not link (an interpreter's extension module, a server's plug-in): the C library's allocator serves the process, so the guarded code's heap accesses pass. Run the program with `LD_PRELOAD=<toolchain>/lib/weavec/libweavec_rt.so.<abi>` to check them, or `WEAVEC_RT_NOTES=0` to silence the note.

## The link fails with undefined `__weavec_rt_` or `__asan_` symbols

Undefined `__weavec_rt_` symbols mean the link did not go through `weavec-cc`, or an object was compiled by an earlier version of `weavec-cc` whose runtime entry points this version no longer has: link with `weavec-cc` and rebuild every object with the current version. An undefined `__asan_` or `__sanitizer_` symbol that the runtime does not provide comes from code that took its AddressSanitizer path; build that unit with `-fno-weavec-asan-compat` (see [AddressSanitizer compatibility](/reference/cli/#addresssanitizer-compatibility)).

## A program built with weavec-cc traps

A trap (`SIGTRAP`, or `SIGILL` on x86-64; a shell shows exit status 133 or 132) means a guard found an access the program must not make, and the program stopped before making it. The runtime prints a report on standard error first; [reading a report](#reading-a-report) explains its parts, and the [command-line reference](/reference/cli/#check-modes) lists every kind.

A C++ program has three more kinds: `alloc-release-mismatch` (see [bad releases](#the-program-stops-at-a-free-or-delete)); `container-overflow`, an access past a `std::vector`'s or `std::deque`'s elements inside its capacity; and `library-assertion`, a check of the C++ library itself (`libc++ Hardening assertion __n < size() failed: vector[] index out of bounds`), reported at the line that called it. A `container-overflow` that the code cannot explain comes from a container grown by code built without WeaveC (a library compiled by another compiler): rebuild that code with `weavec-c++`, or run with `WEAVEC_RT_CONTAINERS=0`. To turn off the C++ library's checks, define `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE` or undefine `_GLIBCXX_ASSERTIONS` on the command line.

To see every failing site in one run, rebuild with `-fweavec-checks=report` and rerun: each failure is printed once per site and the program goes on (what it does after an invalid access is not defined). `WEAVEC_RT_ABORT=1` makes a report-mode build trap at the first failure again. `WEAVEC_RT_REPORT_LOG=<path>` appends the reports to that file instead of standard error, which helps when a test harness hides a passing test's output; a set-user-ID program ignores it. `WEAVEC_RT_REPORT_FORMAT=json` writes each report as one line of [JSON](/reference/cli/#json-reports), for a tool to read.

A trap is almost always a real bug. The exceptions are programs that read past an object on purpose, described [below](#code-that-reads-past-an-object-on-purpose). See [false traps](/reference/guarantees/#false-traps) for what the guarantee accepts.

## Reading a report

```text
weavec: heap-use-after-free at uaf.c:6:56: read of 1 bytes at 0xbe00000000
    #0 0x100c285f3 in use uaf.c:6
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0xbe00000000 is 0 bytes inside the 24-byte heap block at 0xbe00000000, released by thread T0:
    #0 0x100c28593 in drop uaf.c:5
    #1 0x100c2861b in main uaf.c:7
    #2 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: the block was allocated by thread T0:
    #0 0x100c28553 in make uaf.c:4
    #1 0x100c28613 in main uaf.c:7
    #2 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: heap-use-after-free at uaf.c:6:56 in use (thread T0)
```

Read it top to bottom:

1. **What went wrong, and where.** The first line names the kind, the source location of the access (`<unknown>` when the unit has no location for it), whether it reads or writes, its width and its address. An index check has its own form, `weavec: index-out-of-bounds at lookup.c:8:12: index -1`; a bad release names the release instead of an access (`weavec: double-free at f.c:3:40: free of 0x…`); a fault the guards did not see names the faulting instruction (`weavec: memory-fault at pc 0x…: read at 0x…`).
2. **How the program got there.** The indented lines are the stack of the access, innermost first: function, file and line (and column, on Linux). A frame that repeats the number above it is a call the compiler inlined into that function. A frame such as `start (/usr/lib/dyld+0x31e7f)` is in code with no line table: a system library, or code compiled with `-g0`.
3. **Which object, and its history.** Each line that starts with `weavec: 0x… is` places the address in an object. For the heap it gives the block's size, where the address lies in it, and the stacks of the `free` that released it and the `malloc` (or `new`) that allocated it: here `drop` freed what `make` allocated, and `use` read it afterwards. For a local it names the variable, its function and the frame's other locals with their offsets; for a global, its name and where it is defined.
4. **The summary line** repeats the kind and the location with the function and the thread, so `grep 'weavec: summary'` over a log lists every report.

An enforcing compile keeps line tables (as `-gline-tables-only` does) unless its command line ends in `-g0`, so frames carry files and lines in a release build too; on Darwin keep the object files, or the `.dSYM` a one-step build writes, beside the program. The runtime names frames by running a symbolizer (`weavec-cc --weavec-symbolize` on Linux, `atos` on Darwin). When it cannot (it is missing, the program is set-user-ID, `WEAVEC_RT_SYMBOLIZE=0`), a frame reads `in <function> (<module>+0x<offset>)`; save the report with `WEAVEC_RT_REPORT_LOG=<path>` and run `weavec-cc --weavec-symbolize <path>` where the same binaries are to name those frames. Under a debugger the program stops at the trap, in the frame of the access.

A use after return usually gets only `weavec: 0x… is in a frame that returned`: the runtime's own frames, printing the report, overwrite the returned frame's record of its locals. The stack and the location still say which access read the dead frame.

## Heap reports

- `heap-buffer-overflow`: the access reached the bytes after a heap block's object in its slot. The object paragraph says where (`is 16 bytes after the 64-byte heap object at …`) and which call allocated the block. An index that jumps further, over the slot's tail into a neighboring live block, is not caught. Look for an index or length that exceeds the allocation, an allocation sized in elements where bytes were meant (or the reverse), or a missing byte for a string's terminator. The runtime's allocator gives each block exactly its requested size: slack a system allocator rounds up to is not addressable, and `malloc_usable_size` returns the requested size.
- `heap-use-after-free`: the access is to a block that was freed. The report gives the stack that released the block and the stack that allocated it: look for the pointer the reading function got from the allocating one and kept across the `free`, or across a `realloc` that moved the block. A released block is not reused while the program holds a pointer to it, so this is reported however long ago the block was freed and however much the program allocated since.

## Stack and global reports

- `stack-buffer-overflow`: the access left a local into the redzone after it (or, for a negative index, before it). The report names the local and its function and lists the frame's locals: `0x… is 4 bytes after 'buf' (8 bytes at offset 64) in the frame of idx (thread T0), whose locals are:`. `dynamic-stack-buffer-overflow` is the same for an `alloca` block or a variable-length array.
- `stack-use-after-scope`: the access is to a local whose block has ended, through a pointer that outlived it (`0x… is 0 bytes inside 'n' (4 bytes at offset 64), whose scope has ended, in the frame of main`). This is caught at every optimization level.
- `stack-use-after-return`: the access is to a local of a function that returned, through a pointer that outlived the call, before another frame reused its bytes. The report usually says only `0x… is in a frame that returned` ([why](#reading-a-report)); look for a function that returns or stores the address of one of its locals.
- `global-buffer-overflow`: the access left a global or static variable the unit defines into its redzone. The report names it and where it is defined (`0x… is 4 bytes after the 16-byte global 'table' defined at sg.c`, with its line when the unit was compiled with `-g`).

Within a struct, an index into a declared array field is checked against its bound (`index-out-of-bounds`), and a fortified string call into a field against the size the call states (`intra-object-overflow`): the field's with Darwin's headers, the rest of the object with glibc's; a pointer walk past a field is not checked.

## Other reports

- `index-out-of-bounds`: `a[i]` into an array whose bound the type declares, with `i` outside it. A multidimensional array indexed past an inner bound (`m[0][k]` with `k` at least the inner size) is reported too; index the flattened storage through a pointer instead.
- `intra-object-overflow`: a fortified library call (`__strcpy_chk`, `__memcpy_chk`, `__sprintf_chk` and the like, which the C library's headers substitute for `strcpy`, `memcpy` and `sprintf` when they know the destination's size) would write more than that size, usually a field of a struct: `weavec: 0x… is 0 bytes inside the 8-byte destination at 0x…`. The bytes past the field belong to the same object, so only the field's size stops the write. Use a bounded copy (`snprintf`, or `memcpy` with a checked length) into the field.
- `use-after-poison`: the access is to bytes the program poisoned through AddressSanitizer's interface (`ASAN_POISON_MEMORY_REGION`), typically free space in a pool or arena allocator, or an object the allocator handed back. It is the bug ASan reports under the same name.
- `null-dereference`: the access is through a null pointer at an offset the guard could not rule out, or to the lowest 64 KiB of the address space. A string argument that is null where the library function requires a string is reported the same way.
- `unterminated-string`: a library call that reads a string (`strlen`, `strcpy`, a `%s` conversion) ran off the end of its object without finding the terminator.
- `overlapping-copy`: the source and destination of `memcpy` or another function that requires disjoint operands overlap; use `memmove`.
- `invalid-access`: the address is not addressable, but the runtime cannot say which kind of object it belongs to.
- `weavec.proven: <kind>`: only in a `-fweavec-checks=verify` build. A guard that a local rule removed would have failed, so the rule was wrong. Please [report it](#report-an-issue) with the source.

## The program stops at a `free` or `delete`

```text
weavec: double-free at cases.c:7:48: free of 0xbd80000000
weavec: invalid-free at bad.c:8:15: free of 0xba00000001: inside the 8-byte heap block at 0xba00000000
weavec: alloc-release-mismatch at cx.cpp:7:51: operator delete of 0xbe00000000: allocated by operator new[]
```

The runtime's allocator received a `free`, `realloc` or `delete` it cannot honor, and stopped the program, in every check mode. The location is the call that released; a release through a function pointer, or from code `weavec-cc` did not compile, is located at its stack's first frame with a line. The report goes on with the stack of the release and, for a heap block, the block's history: for a `double-free` the stack of the first release and of the allocation, otherwise the allocation's.

| Kind and reason                                           | Usual cause                                                                                                                     |
| --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `double-free`                                             | a second release of a block, through a pointer kept after the first                                                             |
| `invalid-free`: `inside the <n>-byte heap block at …`     | a release of an interior pointer (a pointer advanced past the start of what `malloc` returned)                                  |
| `invalid-free`: `no block was allocated there`            | a release of a pointer into the heap that no block starts at                                                                    |
| `invalid-free`: `not heap memory`                         | a release of a global, a local, or memory no allocator the runtime knows owns; the report names the global or local when it can |
| `alloc-release-mismatch`: `allocated by <form>`           | a release by the wrong form: `delete` of `new[]`, `delete[]` of `new`, `free` of `new`, `delete` or `realloc` of `malloc`'s     |
| `alloc-release-mismatch`: `size <n> for a <m>-byte block` | a sized `delete` of another size: a `delete` through a base class without a virtual destructor                                  |

A released block is not reused while the program holds a pointer to it, so a second `free` through a pointer the program kept is always a `double-free`, however long after the first. A program that defines its own allocator does not get these checks (see [below](#the-link-prints-defines-the-allocator)).

## Code that reads past an object on purpose

Some correct-looking code reads past the end of an object deliberately: string functions that load a word at a time, hash functions that read whole words of a key (json-c's `hashlittle`). It traps under WeaveC as it does under AddressSanitizer. Code that already avoids such reads under ASan, by testing `__SANITIZE_ADDRESS__` or `__has_feature(address_sanitizer)` or by marking the function `__attribute__((no_sanitize("address")))`, needs no change: `weavec-cc` compiles it as ASan would. Otherwise use the project's own switch if it has one (json-c's `PRECISE_MEMORY_ACCESS`), or keep the code out of the guards:

```c
#include <weavec.h>

WEAVEC_UNSAFE static unsigned long hash_words(const unsigned long *p, size_t n) {
    /* reads whole words past the key on purpose */
}
```

Accesses in a `WEAVEC_UNSAFE` function or block are not guarded and its array indexes are not checked. A function marked `__attribute__((no_sanitize("address")))`, `no_sanitize_address` or `disable_sanitizer_instrumentation` is treated the same way. Keep the region to the code that needs it; see [unsafe boundaries](/guides/unsafe/).

## Code written for AddressSanitizer behaves differently

`weavec-cc` defines `__SANITIZE_ADDRESS__` and makes `__has_feature(address_sanitizer)` true, so a project's ASan paths are compiled in: a pool allocator poisons its free space, and a use of it traps as `use-after-poison`, which is the intent. When an ASan path is not wanted (it calls a sanitizer interface the runtime does not provide, or changes behavior in a way the program should not see), build with `-fno-weavec-asan-compat`: the macros are left undefined and the code is compiled as for a plain build, still guarded. Code that reads past an object on purpose only when ASan is off then traps again, and needs a `WEAVEC_UNSAFE` region.

## The link prints "building without the WeaveC runtime"

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
```

The command line has something the runtime cannot work with: a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), `-ffreestanding`, `-nostdlib`, `-nodefaultlibs`, `-nolibc`, or a target other than 64-bit Darwin or Linux. The units are then compiled as with `-fweavec-checks=none`: Clang's output, no guards and no runtime. Nothing is wrong with the build; the note records that it enforces nothing. Remove the flag, or pass `-fweavec-checks=none` yourself to say so explicitly. See [without the runtime](/reference/guarantees/#without-the-runtime).

## The link prints "defines the allocator"

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated
```

An object or archive member on the link line defines `malloc`, `calloc`, `realloc` or `free`, so the program keeps its own allocator. Guards on locals and globals still work; accesses to the heap pass, and no heap overflow, use after free or invalid free is caught. To have the heap tracked, build without the program's allocator if it has a switch for that. An allocator that poisons its free space through ASan's interface gets `use-after-poison` reports for it.

## The program warns that the shadow memory could not be reserved

```text
weavec: warning: the shadow memory could not be reserved; memory accesses are not checked
```

At start-up the runtime reserves address space for its shadow and its arena, without committing memory. A limit on virtual memory (`ulimit -v`, `RLIMIT_AS`) smaller than the reservation makes it fail, and the program then runs with every guard passing, allocating from the C library's allocator. The warning goes to stderr, so a test that compares stderr under such a limit sees it. Raise or remove the limit for guarded programs. The similar warning that the heap lies outside the shadow's window means the arena could not be placed where the shadow covers it; heap accesses are then not checked.

## The program is slower or uses more memory

A guarded program reads the runtime's shadow memory (one byte per 16 bytes of the program's memory) before each access the guard passes could not prove safe, writes the shadow of each local frame with redzones on entry and return, and holds released blocks until a sweep finds no pointer to them. [RFC 0036's acceptance gates](/rfcs/0036-deterministic-heap-reuse/#acceptance-gates) set targets for that cost against the previous release (v0.16.0): a geometric mean of the corpus benchmarks at most 1.1 times its run time and no benchmark above 1.25 times, peak memory at most 1.5 times its or 64 MiB more, and a sweep pause of at most 150 ms with 256 MiB of live heap; the RFC's measured gates record the results.

- Run with `WEAVEC_RT_STATS=1` to see the runtime's counters at exit: how many guards reached the runtime's slow path (`slow guards`), how many range and string guards ran, how many sweeps ran and how long they paused the program, and how many blocks the last sweep had to keep.
- Write the ledger (`-fweavec-ledger=<dir>/ -fweavec-summary`) and look at the guarded share: the summary line (`812 accesses: 431 proven, 381 guarded, 0 unguarded`) and the rows say which accesses in a hot function kept their guard. A loop with a known trip count of at least 16 iterations, one exit and nothing that may end a lifetime runs a copy without the guards of its invariant and affine accesses once one range check passes; other loops keep their guards.
- **Memory held by the quarantine.** A released block stays allocated until a sweep finds no pointer to it, and a block some stale word still points to (a dangling pointer the program never clears, or an integer that happens to look like its address) stays allocated as long as that word does. The last line of `WEAVEC_RT_STATS=1` says how many blocks and bytes the last sweep kept. Clearing pointers to freed memory (`free(p); p = NULL;`) lets their blocks be reused. `WEAVEC_RT_QUARANTINE=<bytes>` sets how many bytes may be released before a sweep is due (64 MiB by default, and at least a quarter of the live heap); sweeps are then paced to take at most about a ninth of the run time, and the quarantine may grow to twice that value, or half the live heap, before one runs regardless. A smaller value holds less memory and sweeps more often. It never makes a block reusable without a sweep, so it does not weaken the guarantee.
- **Sweep pauses.** A sweep stops every thread for a time that grows with the live heap and the program's writable data. A larger `WEAVEC_RT_QUARANTINE` sweeps less often, at the cost of memory.
- As a last resort, put a reviewed hot function in a `WEAVEC_UNSAFE` region, or build a unit that cannot pay with `-fweavec-checks=none`. Both give up the guarantee for that code.

## A program that uses SIGPWR on Linux

On Linux the sweep stops the program's other threads with a signal, `SIGPWR` by default. The runtime chooses it the first time the program sets a signal mask or sweeps, whichever comes first: when the program has installed its own `SIGPWR` handler by then, the runtime uses `SIGXCPU` instead. A program that installs its own handler for the runtime's signal later keeps it: before each sweep the runtime checks that the signal is still its own, and otherwise chooses again, though a thread that blocked every signal before that blocks the new one too (see below). If the program handles both, a process with more than one thread never reuses released memory. Such a program names another signal for the runtime:

```sh
WEAVEC_RT_SWEEP_SIGNAL=40 ./program
```

The runtime takes that signal over, so it must be one the program does not handle. `pthread_sigmask` and `sigprocmask` never block it: the runtime leaves it out of every mask the program sets through them. A thread that blocks it by other means (the system call itself, or a handler or `pselect` mask that includes it), or does not respond within a second, ends that sweep without recycling anything, and the next sweep waits until much more memory has been released: memory is held longer, but no released block is reused. Darwin stops threads without a signal.

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete run-time report, stacks included. Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
