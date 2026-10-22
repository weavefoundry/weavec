---
title: Troubleshooting
description: Resolve installation problems, read each run-time report and find the bug behind it, handle code that reads past objects on purpose or was written for AddressSanitizer, understand the link notes, and deal with memory held by the quarantine, slow programs and the sweep's signal.
---

## CMake cannot find LLVM or Clang

Install the development libraries, not just a compiler executable. Set `WEAVEC_LLVM_PREFIX` to the LLVM installation prefix, or supply `CMAKE_PREFIX_PATH`, `LLVM_DIR`, and `Clang_DIR`. On macOS, use Homebrew LLVM; Xcode does not ship the required CMake packages.

Use a fresh build directory if a previous configuration selected a different LLVM installation. See the [developer toolchain guide](/contributing/development/#toolchain).

## FileCheck or lit is missing

Tests require `lit` plus LLVM's `FileCheck`, `not`, and `count` utilities. Make sure the chosen LLVM tool directory is on PATH and use a matching `lit` release. Building LLVM from source requires its utilities to be installed.

## The installed compiler cannot locate Clang resources

Keep the LLVM installation used at configure time available. The driver records Clang's resource directory and executable; `WEAVEC_CLANG` can name a compatible `clang` executable instead. WeaveC's own files (`weavec.h` and the runtime archives) are found in `lib/weavec/` beside the installed `bin/`, or where `WEAVEC_RESOURCE_DIR` points. See [build targets](/contributing/development/#building).

## The link cannot find the WeaveC runtime

```text
weavec-cc: error: cannot find the WeaveC runtime (libweavec_rt.a, libweavec_alloc.a), which an enforcing link carries (-fweavec-checks=none builds without it)
```

Every link of a guarded program adds the runtime's two archives, which are installed in `lib/weavec/` (beside the `include/` directory that holds `weavec.h`). The driver looks for them in `../lib/weavec/` from its own executable, or in the directory `WEAVEC_RESOURCE_DIR` names. Install WeaveC with `cmake --install`, or run `weavec-cc` from the build tree, which has the same layout. `-fweavec-checks=none` builds without guards and without the runtime.

## The link fails with undefined `__weavec_rt_` or `__asan_` symbols

Undefined `__weavec_rt_` symbols mean the link did not go through `weavec-cc`, or an object was compiled by an earlier version of `weavec-cc` whose runtime entry points this version no longer has: link with `weavec-cc` and rebuild every object with the current version. An undefined `__asan_` or `__sanitizer_` symbol that the runtime does not provide comes from code that took its AddressSanitizer path; build that unit with `-fno-weavec-asan-compat` (see [AddressSanitizer compatibility](/reference/cli/#addresssanitizer-compatibility)).

## A program built with weavec-cc traps

A trap (`SIGTRAP`, or `SIGILL` on x86-64; a shell shows exit status 133 or 132) means a guard found an access the program must not make, and the program stopped before making it. The runtime prints one line on standard error first, and a second that places the address in its object when it can:

```text
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The first line names the kind of bad access, the source location of the access (`<unknown>` when the unit has no location for it), whether it reads or writes, its width and its address. An index check has its own form, `weavec: index-out-of-bounds at lookup.c:8:12: index -1`, and an access with no known width reads `access at 0x…`. The [command-line reference](/reference/cli/#check-modes) lists every kind and its second line.

To see how the program reached the access, run it with `WEAVEC_RT_BACKTRACE=1`, which prints the call stack after the report, or under a debugger, which stops at the trap. Compile with `-g` for the most useful stacks.

To see every failing site in one run, rebuild with `-fweavec-checks=report` and rerun: each failure is printed once per site and the program goes on (what it does after an invalid access is not defined). `WEAVEC_RT_ABORT=1` makes a report-mode build trap at the first failure again. `WEAVEC_RT_REPORT_LOG=<path>` appends the reports to that file instead of standard error, which helps when a test harness hides a passing test's output; a set-user-ID program ignores it.

A trap is almost always a real bug. The exceptions are programs that read past an object on purpose, described [below](#code-that-reads-past-an-object-on-purpose). See [false traps](/reference/guarantees/#false-traps) for what the guarantee accepts.

## Heap reports

- `heap-buffer-overflow`: the access reached the bytes after a heap block's object in its slot. The second line says where (`is 16 bytes after the 64-byte heap object`). An index that jumps further, over the slot's tail into a neighbouring live block, is not caught. Look for an index or length that exceeds the allocation, an allocation sized in elements where bytes were meant (or the reverse), or a missing byte for a string's terminator. The runtime's allocator gives each block exactly its requested size: slack a system allocator rounds up to is not addressable, and `malloc_usable_size` returns the requested size.
- `heap-use-after-free`: the access is to a block that was freed (`is inside a released heap block at …`). Look for a pointer kept across a `free`, or across a `realloc` that moved the block. A released block is not reused while the program holds a pointer to it, so this is reported however long ago the block was freed and however much the program allocated since.

## Stack and global reports

- `stack-buffer-overflow`: the access left a local into the redzone after it (or, for a negative index, before it). `dynamic-stack-buffer-overflow` is the same for an `alloca` block or a variable-length array.
- `stack-use-after-scope`: the access is to a local whose block has ended, through a pointer that outlived it. This is caught at every optimisation level. A use of a local after its function returned is not caught.
- `global-buffer-overflow`: the access left a global or static variable the unit defines into its redzone.

Within a struct, an index into a declared array field is checked against its bound (`index-out-of-bounds`), and a fortified string call into a field against the field's size (`intra-object-overflow`); a pointer walk past a field is not.

## Other reports

- `index-out-of-bounds`: `a[i]` into an array whose bound the type declares, with `i` outside it. A multidimensional array indexed past an inner bound (`m[0][k]` with `k` at least the inner size) is reported too; index the flattened storage through a pointer instead.
- `intra-object-overflow`: a fortified library call (`__strcpy_chk`, `__memcpy_chk`, `__sprintf_chk` and the like, which the C library's headers substitute for `strcpy`, `memcpy` and `sprintf` when they know the destination's size) would write more than that size, usually a field of a struct: `weavec: 0x… is 0 bytes inside the 8-byte destination at 0x…`. The bytes past the field belong to the same object, so only the field's size stops the write. Use a bounded copy (`snprintf`, or `memcpy` with a checked length) into the field.
- `use-after-poison`: the access is to bytes the program poisoned through AddressSanitizer's interface (`ASAN_POISON_MEMORY_REGION`), typically free space in a pool or arena allocator, or an object the allocator handed back. It is the bug ASan reports under the same name.
- `null-dereference`: the access is through a null pointer at an offset the guard could not rule out, or to the lowest 64 KiB of the address space. A string argument that is null where the library function requires a string is reported the same way.
- `unterminated-string`: a library call that reads a string (`strlen`, `strcpy`, a `%s` conversion) ran off the end of its object without finding the terminator.
- `overlapping-copy`: the source and destination of `memcpy` or another function that requires disjoint operands overlap; use `memmove`.
- `invalid-access`: the address is not addressable, but the runtime cannot say which kind of object it belongs to.
- `weavec.proven: <kind>`: only in a `-fweavec-checks=verify` build. A guard that a local rule removed would have failed, so the rule was wrong. Please [report it](#report-an-issue) with the source.

## The program stops with "weavec: invalid release"

```text
weavec: invalid release of 0xbd00000000: the block was already released
```

The runtime's allocator received a `free` or `realloc` it cannot honour, and stopped the program. This line has no source location, because the allocator validates the release, not a guard. The reasons are:

| Reason                                                        | Usual cause                                                                                                                   |
| ------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `the block was already released`                              | a double free                                                                                                                 |
| `not the start of its block`, `not the start of a live block` | a free of an interior pointer                                                                                                 |
| `no block is allocated there`                                 | a free of a pointer into the heap that no block starts at                                                                     |
| `not a heap block`                                            | a free of a global or other pointer that no allocator owns (on Linux the C library's allocator rejects such a pointer itself) |

A released block is not reused while the program holds a pointer to it, so a second `free` through a pointer the program kept is always reported as `the block was already released`. Run the program with `WEAVEC_RT_BACKTRACE=1` or under a debugger to find the caller of `free`. A program that defines its own allocator does not get this check (see [below](#the-link-prints-defines-the-allocator)).

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

`weavec-cc` defines `__SANITIZE_ADDRESS__` and makes `__has_feature(address_sanitizer)` true, so a project's ASan paths are compiled in: a pool allocator poisons its free space, and a use of it traps as `use-after-poison`, which is the intent. When an ASan path is not wanted (it calls a sanitizer interface the runtime does not provide, or changes behaviour in a way the program should not see), build with `-fno-weavec-asan-compat`: the macros are left undefined and the code is compiled as for a plain build, still guarded. Code that reads past an object on purpose only when ASan is off then traps again, and needs a `WEAVEC_UNSAFE` region.

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

At start-up the runtime reserves address space for its shadow and its arena, without committing memory. A limit on virtual memory (`ulimit -v`, `RLIMIT_AS`) smaller than the reservation makes it fail, and the program then runs with every guard passing. Raise or remove the limit for guarded programs. The similar warning that the heap lies outside the shadow's window means the arena could not be placed where the shadow covers it; heap accesses are then not checked.

## The program is slower or uses more memory

A guarded program reads the runtime's shadow memory (one byte per 16 bytes of the program's memory) before each access the guard passes could not prove safe, writes the shadow of each local frame with redzones on entry and return, and holds released blocks until a sweep finds no pointer to them. [RFC 0036's acceptance gates](/rfcs/0036-deterministic-heap-reuse/#acceptance-gates) set targets for that cost against the previous release (v0.16.0): a geometric mean of the corpus benchmarks at most 1.1 times its run time and no benchmark above 1.25 times, peak memory at most 1.5 times its or 64 MiB more, and a sweep pause of at most 150 ms with 256 MiB of live heap; the RFC's measured gates record the results.

- Run with `WEAVEC_RT_STATS=1` to see the runtime's counters at exit: how many guards reached the runtime's slow path (`slow guards`), how many range and string guards ran, how many sweeps ran and how long they paused the program, and how many blocks the last sweep had to keep.
- Write the ledger (`-fweavec-ledger=<dir>/ -fweavec-summary`) and look at the guarded share: the summary line (`812 accesses: 431 proven, 381 guarded, 0 unguarded`) and the rows say which accesses in a hot function kept their guard. A loop with a known trip count of at least 16 iterations, one exit and nothing that may end a lifetime runs a copy without the guards of its invariant and affine accesses once one range check passes; other loops keep their guards.
- **Memory held by the quarantine.** A released block stays allocated until a sweep finds no pointer to it, and a block some stale word still points to (a dangling pointer the program never clears, or an integer that happens to look like its address) stays allocated as long as that word does. The last line of `WEAVEC_RT_STATS=1` says how many blocks and bytes the last sweep kept. Clearing pointers to freed memory (`free(p); p = NULL;`) lets their blocks be reused. `WEAVEC_RT_QUARANTINE=<bytes>` sets how many bytes may be released before a sweep is due (16 MiB by default, and at least a quarter of the live heap); sweeps are then paced to take at most about a ninth of the run time, and the quarantine may grow to twice that value, or half the live heap, before one runs regardless. A smaller value holds less memory and sweeps more often. It never makes a block reusable without a sweep, so it does not weaken the guarantee.
- **Sweep pauses.** A sweep stops every thread for a time that grows with the live heap and the program's writable data. A larger `WEAVEC_RT_QUARANTINE` sweeps less often, at the cost of memory.
- As a last resort, put a reviewed hot function in a `WEAVEC_UNSAFE` region, or build a unit that cannot pay with `-fweavec-checks=none`. Both give up the guarantee for that code.

## A program that uses SIGPWR on Linux

On Linux the sweep stops the program's other threads with a signal, `SIGPWR` by default. When the program has installed its own `SIGPWR` handler by the time of the first sweep, the runtime uses `SIGXCPU` instead; if the program handles both, a process with more than one thread never reuses released memory. A program that installs a handler for the runtime's signal after the first sweep, or that handles both, names another signal for the runtime:

```sh
WEAVEC_RT_SWEEP_SIGNAL=40 ./program
```

The runtime takes that signal over, so it must be one the program neither handles nor blocks in any thread. A thread that blocks it, or does not respond within a second, ends that sweep without recycling anything: memory is held longer, but no released block is reused. Darwin stops threads without a signal.

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete run-time report (with `WEAVEC_RT_BACKTRACE=1` if the stack matters). Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
